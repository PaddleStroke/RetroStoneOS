/*
 * host.c - the game process: set-up, main loop (pacing, input, retro_run,
 * audio), hotkeys, shutdown with durable saves. See docs/host-design.md.
 */
#include <drm_fourcc.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

#include "../audio/audio.h"
#include "../audio/resampler.h"
#include "batt_overlay.h"
#include "bench.h"
#include "content.h"
#include "draw.h"
#include "glprobe.h"
#include "host_input.h"
#include "host_internal.h"
#include "host_png.h"
#include "hutil.h"
#include "../i18n/i18n.h"
#include "../board.h"
#include "../board_apply.h"
#include "hwrender.h"
#include "ini.h"
#include "options.h"
#include "perf.h"
#include "saves.h"

#define WATCHDOG_S 15
#define BATTERY_CHECK_MS 10000
#define BATTERY_CRITICAL 3   /* % : save everything and power off (TODO(hw): tune) */
#define BATTERY_LOW 7

/*
 * Supervisor -> game signals (docs/power.md 10, docs/host-design.md).
 * Same values as RSOS_SIG_* in src/power/power.h.
 */
#if defined(__has_include)
#if __has_include("../power/power.h")
#include "../power/power.h"
#define HOST_SIG_POWEROFF RSOS_SIG_POWEROFF
#define HOST_SIG_SLEEP RSOS_SIG_SLEEP
#define HOST_SIG_WAKE RSOS_SIG_WAKE
#endif
#endif
#ifndef HOST_SIG_POWEROFF
#define HOST_SIG_POWEROFF SIGUSR2
#define HOST_SIG_SLEEP (SIGRTMIN + 1)
#define HOST_SIG_WAKE (SIGRTMIN + 2)
#endif

struct host H;

void host_supervisor_signals(bool block, int *sleep_sig, int *wake_sig, int *poweroff_sig)
{
	sigset_t s;

	if (sleep_sig)
		*sleep_sig = HOST_SIG_SLEEP;
	if (wake_sig)
		*wake_sig = HOST_SIG_WAKE;
	if (poweroff_sig)
		*poweroff_sig = HOST_SIG_POWEROFF;
	sigemptyset(&s);
	sigaddset(&s, HOST_SIG_SLEEP);
	sigaddset(&s, HOST_SIG_WAKE);
	sigaddset(&s, HOST_SIG_POWEROFF);
	sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &s, NULL);
}

static struct audio_config audio_cfg;
static enum audio_output audio_out;
static bool audio_wanted;     /* the display asked for audio (ACQUIRE) */
static int64_t present_wait_us;   /* time blocked in present this frame */
static unsigned surf_w, surf_h;
static uint32_t surf_fmt;

/*
 * In-game battery indicator on the display's overlay plane (host-design.md
 * §8): the value the menu shows, read from the supervisor's file every
 * BOV_READ_MS, redrawn only when it changes. Zero cost per frame.
 */
#define BOV_READ_MS 10000
static struct {
	bool enabled;              /* setting game_battery_overlay (default on) */
	enum display_corner corner;
	bool hidden;               /* the in-game menu is open */
	bool force;                /* output changed / shown again: redraw */
	int64_t next_read;
	int pct;                   /* -1 unknown */
	bool charging;
	int drawn_pct, drawn_scale;
	bool drawn_charging, shown;
	uint32_t px[BOV_W * BOV_MAX_SCALE * BOV_H * BOV_MAX_SCALE];
	/* the strip: FPS / benchmark text + battery on the same plane */
	char bench_line[96];
	bool strip;                /* what is shown is the strip */
	bool refused;              /* the plane never showed it: text in the frame */
	int64_t shown_at;
	char drawn_text[OVL_MAX_LINES][96];
	int drawn_w1, drawn_nl;
	uint32_t *spx;
	size_t spx_cap;
} BOV;

/*
 * Performance windows (perf.h): [0] the FPS overlay (1 s), [1] the game.log
 * line (10 s, N64/GLES games), [2] the session summary at exit.
 */
static struct {
	struct perf_counters a;
	double max;                /* worst core time in the window */
	int64_t next_ms;
} PW[3];

/* The benchmark run of this process (--bench-step) and the results page
 * (--bench-report). */
static struct {
	bool active, measuring;
	struct bench_plan plan;
	int idx;
	bool from_boot;
	char note[96];
	int64_t t_measure, t_end;  /* hnow_us() */
	struct perf_counters a;
	double max;
	float *ft;                 /* frame times of the measured window, ms */
	size_t nft, cap;
	int64_t last_iter_us;
} B;
static struct bench_plan RP;           /* --bench-report: the plan */
static struct host_bench_report REPORT;
static bool report_loaded;

/* ------------------------------------------------------------- helpers */

void host_status(const char *kind, const char *msg)
{
	char line[400];
	int n;

	if (H.cfg.status_fd < 0)
		return;
	n = snprintf(line, sizeof(line), "%s %s\n", kind, msg ? msg : "");
	if (n > 0 && write(H.cfg.status_fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1)) < 0)
		hlog(HLOG_DEBUG, "status fd: %s", strerror(errno));
}

void host_toast(const char *fmt, ...)
{
	char buf[192];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	hutf8_trim(buf);
	osd_toast(buf, 2000);
}

void host_request_quit(int code)
{
	H.quit = true;
	H.exit_code = code;
}

double host_run_hz(void)
{
	return pacing_run_hz(&H.pace);
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		H.flush_sig = 1;
	else if (sig == HOST_SIG_POWEROFF)
		H.poweroff_sig = 1;
	else if (sig == HOST_SIG_SLEEP)
		H.sleep_sig = 1;
	else if (sig == HOST_SIG_WAKE)
		H.sleep_sig = 2;
	else
		H.quit_sig = 1;
}

static void display_log(enum display_log_level lvl, const char *msg, void *user)
{
	(void)user;
	hlog(lvl == DISPLAY_LOG_ERROR ? HLOG_ERROR : lvl == DISPLAY_LOG_WARN ? HLOG_WARN :
	     lvl == DISPLAY_LOG_INFO ? HLOG_INFO : HLOG_DEBUG, "display: %s", msg);
}

/* --------------------------------------------------------------- video */

void host_video_setup(void)
{
	unsigned mw, mh;
	uint32_t fmt;
	size_t need;

	mw = H.av.geometry.max_width ? H.av.geometry.max_width : H.av.geometry.base_width;
	mh = H.av.geometry.max_height ? H.av.geometry.max_height : H.av.geometry.base_height;
	if (!mw || !mh) {
		mw = 320;
		mh = 240;
	}
	fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;
	need = (size_t)mw * mh * 4;
	if (need > H.shadow_size) {
		void *s = realloc(H.shadow, need);

		if (s) {
			H.shadow = s;
			H.shadow_size = need;
		}
	}
	if (!H.display_ok)
		return;
	if (mw != surf_w || mh != surf_h || fmt != surf_fmt) {
		const struct display_surface *s = display_set_game_surface((int)mw, (int)mh, fmt, 0);

		if (!s) {
			hlog(HLOG_ERROR, "cannot create the game surface %ux%u", mw, mh);
			return;
		}
		surf_w = mw;
		surf_h = mh;
		surf_fmt = fmt;
		hlog(HLOG_INFO, "game surface %ux%u %s, %s%s", mw, mh,
		     fmt == DRM_FORMAT_RGB565 ? "RGB565" : fmt == DRM_FORMAT_XRGB8888 ? "XRGB8888" : "XRGB1555",
		     s->hw_scaled ? "hardware scaled" : "software blit", s->direct ? ", zero-copy" : "");
	}
	display_set_scaling((enum display_scale_mode)H.scale,
			    H.av.geometry.aspect_ratio > 0 ? H.av.geometry.aspect_ratio : 0);
}

/* Forget the surface we set (the menu replaces it). */
void host_video_invalidate(void)
{
	surf_w = surf_h = 0;
	surf_fmt = 0;
}

static void wait_vblank(void)
{
	const struct display_output_info *o = display_output();
	drmVBlank vbl;
	int crtc = o ? o->crtc_index : 0;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE;
	if (crtc == 1)
		vbl.request.type |= DRM_VBLANK_SECONDARY;
	else if (crtc > 1)
		vbl.request.type |= (crtc << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK;
	vbl.request.sequence = 1;
	if (drmWaitVBlank(display_get_drm_fd(), &vbl) != 0) {
		struct timespec ts = { 0, (long)(1e9 / (H.pace.disp_hz > 1 ? H.pace.disp_hz : 60)) };

		nanosleep(&ts, NULL);
	}
	display_handle_events();
}

static void present(const void *data, unsigned w, unsigned h, size_t pitch)
{
	const void *src = data;
	size_t spitch = pitch;
	int64_t t0 = hnow_us();
	uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;

	if (osd_active() && H.shadow) {
		int bpp = fmt_bpp(fmt);
		size_t row = (size_t)w * (size_t)bpp;

		if ((size_t)h * row <= H.shadow_size) {
			for (unsigned y = 0; y < h; y++)
				memcpy((uint8_t *)H.shadow + y * row, (const uint8_t *)data + y * pitch, row);
			osd_draw(H.shadow, (int)row, (int)w, (int)h, fmt);
			src = H.shadow;
			spitch = row;
		}
	}
	if (H.pace.mode != PACE_VSYNC && display_begin_frame(0) < 0) {
		/* No free buffer: latest-frame semantics, the screen keeps the
		 * frame it has and this one is skipped. */
		H.frames_skipped++;
		return;
	}
	if (display_present_frame(src, (int)w, (int)h, (int)spitch) == 0)
		H.frames_presented++;
	present_wait_us += hnow_us() - t0;
}

void host_video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		if (H.skip_video)
			return;
		H.last_w = w;
		H.last_h = h;
		/* Zero-copy unless an overlay must be drawn into the frame. */
		H.frames_delivered++;
		if (hwr_zero_copy() && H.display_ok && !osd_active()) {
			int64_t t0 = hnow_us();
			int r = hwr_present(w, h, H.pace.mode == PACE_VSYNC ? -1 : 0);

			present_wait_us += hnow_us() - t0;
			if (r != HWR_FALLBACK) {
				if (r == HWR_SHOWN)
					H.frames_presented++;
				else
					H.frames_skipped++;
				H.last_frame = NULL;
				H.hw_capture = true; /* read back only if needed */
				return;
			}
		}
		data = hwr_readback(&w, &h);          /* clamped to the FBO (F-M5) */
		pitch = (size_t)w * 4;
		if (!data)
			return;
		H.hw_capture = false;
	}
	if (!data) {
		/* Dupe: nothing changed. In vsync mode still wait one refresh. */
		H.frames_duped++;
		if (H.display_ok && H.pace.mode == PACE_VSYNC && !H.skip_video) {
			int64_t t0 = hnow_us();

			wait_vblank();
			present_wait_us += hnow_us() - t0;
		}
		return;
	}
	if (!H.hw_requested && !H.skip_video)
		H.frames_delivered++; /* (GLES frames are counted above) */
	H.last_frame = data;
	H.last_w = w;
	H.last_h = h;
	H.last_pitch = pitch;
	if (H.cfg.headless) {
		int bpp = fmt_bpp(H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format);
		size_t row = (size_t)w * (size_t)bpp, need = row * h;

		if (need > H.headless_size) {
			void *n = realloc(H.headless_frame, need);

			if (!n)
				return;
			H.headless_frame = n;
			H.headless_size = need;
		}
		for (unsigned y = 0; y < h; y++)
			memcpy((uint8_t *)H.headless_frame + y * row, (const uint8_t *)data + y * pitch, row);
		H.last_frame = H.headless_frame;
		H.last_pitch = row;
		return;
	}
	if (H.skip_video || !H.display_ok)
		return;
	present(data, w, h, pitch);
}

void host_capture_last(void)
{
	if (H.hw_capture && hwr_active()) {
		const void *p = hwr_readback(&H.last_w, &H.last_h);   /* clamped (F-M5) */

		if (p) {
			H.last_frame = p;
			H.last_pitch = (size_t)H.last_w * 4;
			H.hw_capture = false;
		}
	}
}

void host_present_last(void)
{
	host_capture_last();
	if (H.display_ok && H.last_frame)
		present(H.last_frame, H.last_w, H.last_h, H.last_pitch);
}

/* The game ends with a write that takes a moment: one frame that says so
 * (an OSD toast over the last game frame), shown while it is written. */
static void show_exit_frame(const char *msg)
{
	if (!H.display_ok || H.cfg.headless)
		return;
	osd_toast(msg, 10000);
	host_capture_last();
	if (!H.last_frame)
		return;
	display_begin_frame(100);   /* a free buffer, whatever the pacing mode */
	present(H.last_frame, H.last_w, H.last_h, H.last_pitch);
}

/* The power key (or the battery) ends the game: SRAM and .state.auto are
 * written under "Powering off...". */
static void show_poweroff_frame(void)
{
	/* TRANSLATORS: toast over the last game picture while the saves are written */
	show_exit_frame(_("Powering off..."));
}

/* --------------------------------------------------------------- audio */

void host_audio_setup(void)
{
	if (audio_wanted && !audio_is_open())
		audio_open(&audio_cfg, audio_out, audio_latency_ms() / 2);
	if (!H.rs) {
		H.rs = resampler_new(RESAMPLER_SINC, 8192);
		if (!H.rs)
			hlog(HLOG_ERROR, "resampler: out of memory");
	}
}

static void audio_process(void)
{
	double fill, ratio;
	int cap, n;

	core_audio_lock();
	if (!H.ain_n) {
		core_audio_unlock();
		return;
	}
	if (!H.rs || (H.skip_video && H.ff_speed > 1) || H.bench_step) {
		H.ain_n = 0;
		core_audio_unlock();
		return;
	}
	fill = audio_is_open() ? audio_fill() : H.pace.target_fill;
	ratio = pacing_ratio(&H.pace, fill);
	H.last_ratio = ratio;
	cap = (int)(H.ain_n * ratio) + 32;
	if (cap > H.aout_cap) {
		int16_t *o = realloc(H.aout, (size_t)cap * 2 * sizeof(int16_t));

		if (!o) {
			H.ain_n = 0;
			core_audio_unlock();
			return;
		}
		H.aout = o;
		H.aout_cap = cap;
	}
	n = resampler_run(H.rs, H.ain, H.ain_n, H.aout, H.aout_cap, ratio);
	H.ain_n = 0;
	core_audio_unlock();
	/* Fast-forward: muted. The stream keeps flowing at the normal rate (one
	 * displayed frame's worth, as silence), so the pacing (vsync + DRC or
	 * the audio clock) and the buffer fill stay where they were and nothing
	 * has to resync when it stops; the resampler keeps its history. */
	if (n > 0 && H.ff_speed > 1)
		memset(H.aout, 0, (size_t)n * 2 * sizeof(int16_t));
	if (audio_is_open() && n > 0) {
		struct audio_stats before, after;

		audio_get_stats(&before);
		audio_write(H.aout, n);
		audio_get_stats(&after);
		if (after.underruns != before.underruns && (H.ff_speed > 1 || hnow_ms() < H.ff_resync_until)) {
			/* fast-forward runs as fast as the CPU allows: a core that
			 * cannot reach the speed drains the queue, which is expected
			 * and must not raise the latency for the rest of the game */
			H.underrun_window = 0;
		} else if (after.underruns != before.underruns) {
			/* Repeated underruns: the core cannot keep up with this
			 * latency. Back off (up to 160 ms) instead of crackling. */
			int64_t now = hnow_ms();

			if (now - H.underrun_t0 > 3000) {
				H.underrun_t0 = now;
				H.underrun_window = 0;
			}
			if (++H.underrun_window >= 4 && audio_latency_ms() < 160) {
				int nl = audio_latency_ms() * 3 / 2;

				hlog(HLOG_WARN, "audio: %u underruns in 3 s, latency -> %d ms", H.underrun_window, nl);
				audio_set_latency(nl);
				H.underrun_window = 0;
			}
		}
	}
}

void host_reevaluate_pacing(void)
{
	const struct display_output_info *o = H.display_ok ? display_output() : NULL;
	double hz = o ? o->refresh_mhz / 1000.0 : 0;

	pacing_setup(&H.pace, H.av.timing.fps, H.av.timing.sample_rate, hz, AUDIO_RATE,
		     H.display_ok && o, audio_is_open(), H.cfg.vsync_tolerance);
	if (H.bench_step)
		H.pace.mode = PACE_FREE; /* a benchmark run is unthrottled */
	else if (H.cfg.timer_pacing && H.pace.mode == PACE_FREE)
		H.pace.mode = PACE_TIMER; /* tests: real-time pacing without a display */
	H.next_frame_us = 0;
	hlog(HLOG_INFO, "pacing: %s (core %.4f fps %.1f Hz, display %.3f Hz, ratio %.6f)",
	     pacing_mode_name(H.pace.mode), H.pace.core_fps, H.pace.core_rate, hz, H.pace.base_ratio);
}

/* ---------------------------------------------------- battery overlay */

/* The FPS / benchmark text and the battery on one overlay-plane strip. */
static void strip_update(int64_t now_ms, const char *const *lines, int nl, bool batt)
{
	const struct display_output_info *o = display_output();
	int scale = o && o->height >= 900 ? 2 : 1;
	int margin = o && o->type == DISPLAY_OUTPUT_HDMI ? o->height * 3 / 100 : 4;
	int W = o && o->width > 0 ? o->width : 640, w1 = (W - 2 * margin) / scale, h;
	bool right = BOV.corner == DISPLAY_CORNER_TOP_RIGHT || BOV.corner == DISPLAY_CORNER_BOTTOM_RIGHT;
	size_t need;

	if (BOV.shown && BOV.strip && !BOV.force && BOV.drawn_w1 == w1 && BOV.drawn_scale == scale &&
		BOV.drawn_nl == nl && (nl < 3 || !strcmp(BOV.drawn_text[2], lines[2])) &&
		!strcmp(BOV.drawn_text[0], nl > 0 ? lines[0] : "") && !strcmp(BOV.drawn_text[1], nl > 1 ? lines[1] : "") &&
		(!batt || (BOV.pct == BOV.drawn_pct && BOV.charging == BOV.drawn_charging)) &&
		batt == (BOV.drawn_pct >= 0)) {
		if (BOV.shown_at && now_ms - BOV.shown_at > 1500 && !display_overlay_visible() && !BOV.refused) {
			/* the plane never showed it: the text goes into the frame */
			BOV.refused = true;
			hlog(HLOG_WARN, "overlay plane refused: FPS overlay drawn into the frame (readback on GLES)");
		}
		return;
	}
	need = (size_t)w1 * scale * (size_t)ovl_strip_height(nl) * scale;
	if (need > BOV.spx_cap) {
		uint32_t *n = realloc(BOV.spx, need * 4);

		if (!n)
			return;
		BOV.spx = n;
		BOV.spx_cap = need;
	}
	h = ovl_render_strip(BOV.spx, w1, scale, lines, nl, batt ? BOV.pct : -1, BOV.charging, right);
	if (display_set_overlay(BOV.spx, w1 * scale, h, batt ? BOV.corner : DISPLAY_CORNER_TOP_LEFT, margin) == 0) {
		if (!BOV.shown || !BOV.strip)
			BOV.shown_at = now_ms;
		BOV.shown = true;
		BOV.strip = true;
		BOV.drawn_pct = batt ? BOV.pct : -1;
		BOV.drawn_charging = BOV.charging;
		BOV.drawn_scale = scale;
		BOV.drawn_w1 = w1;
		hstrlcpy(BOV.drawn_text[0], nl > 0 ? lines[0] : "", sizeof(BOV.drawn_text[0]));
		hstrlcpy(BOV.drawn_text[1], nl > 1 ? lines[1] : "", sizeof(BOV.drawn_text[1]));
		hstrlcpy(BOV.drawn_text[2], nl > 2 ? lines[2] : "", sizeof(BOV.drawn_text[2]));
		BOV.drawn_nl = nl;
	}
	BOV.force = false;
}

static void bov_update(int64_t now_ms)
{
	const struct display_output_info *o;
	const char *lines[OVL_MAX_LINES];
	bool right, want;
	int scale, margin, r, nl = 0;

	if (!H.display_ok)
		return;
	if (!BOV.hidden && !BOV.refused) {
		static char ff_line[16];

		if (H.show_stats && H.stats_line[0])
			lines[nl++] = H.stats_line;
		if (BOV.bench_line[0])
			lines[nl++] = BOV.bench_line;
		/* the fast-forward indicator: two play triangles (glyph 0x10 of
		 * the overlay font) and the speed */
		if (H.ff_speed > 1 && nl < OVL_MAX_LINES) {
			snprintf(ff_line, sizeof(ff_line), "\x10\x10 x%d", H.ff_speed);
			lines[nl++] = ff_line;
		}
	}
	H.stats_on_plane = H.show_stats && !BOV.refused;
	H.ff_on_plane = !BOV.refused;
	if (!BOV.enabled && !nl) {
		if (BOV.shown)
			display_hide_overlay();
		BOV.shown = false;
		return;
	}
	if (BOV.enabled && now_ms >= BOV.next_read) {
		BOV.next_read = now_ms + BOV_READ_MS;
		r = bov_read(H.cfg.battery_path ? H.cfg.battery_path : BOV_FILE, &BOV.pct, &BOV.charging);
		if (r < 0 && H.cfg.status_fd < 0) {
			/* stand-alone rsos-run: no supervisor, the AXP gauge */
			r = sys_battery(&BOV.pct, &BOV.charging);
		}
		if (r < 0)
			BOV.pct = -1;
	}
	want = BOV.enabled && !BOV.hidden && BOV.pct >= 0;
	if (nl) {
		strip_update(now_ms, lines, nl, want);
		return;
	}
	if (!want) {
		if (BOV.shown)
			display_hide_overlay();
		BOV.shown = false;
		return;
	}
	o = display_output();
	/* 2x on large outputs; on a TV keep clear of the overscan (~3 %) */
	scale = o && o->height >= 900 ? 2 : 1;
	margin = o && o->type == DISPLAY_OUTPUT_HDMI ? o->height * 3 / 100 : 4;
	if (BOV.shown && !BOV.strip && !BOV.force && BOV.pct == BOV.drawn_pct && BOV.charging == BOV.drawn_charging &&
	    scale == BOV.drawn_scale)
		return;
	right = BOV.corner == DISPLAY_CORNER_TOP_RIGHT || BOV.corner == DISPLAY_CORNER_BOTTOM_RIGHT;
	bov_render(BOV.px, BOV_W * scale, scale, BOV.pct, BOV.charging, right);
	if (display_set_overlay(BOV.px, BOV_W * scale, BOV_H * scale, BOV.corner, margin) == 0) {
		BOV.shown = true;
		BOV.strip = false;
		BOV.drawn_pct = BOV.pct;
		BOV.drawn_charging = BOV.charging;
		BOV.drawn_scale = scale;
	}
	BOV.force = false;
}

/* The in-game menu covers the screen: no indicator over it. */
static void bov_hide(bool hide)
{
	BOV.hidden = hide;
	BOV.force = true;
	bov_update(hnow_ms());
}

/* -------------------------------------------------- display callbacks */

static void on_output(const struct display_output_info *now, const struct display_output_info *before,
		      enum display_event_reason why, const struct display_switch_timing *t, void *user)
{
	(void)before;
	(void)t;
	(void)user;
	hlog(HLOG_INFO, "output: %s %dx%d@%.3f (reason %d)", now->name, now->width, now->height,
	     now->refresh_mhz / 1000.0, (int)why);
	hin_set_docked(now->type == DISPLAY_OUTPUT_HDMI);
	BOV.force = true;   /* the scale and the TV margin depend on the output */
	if (H.game_loaded)
		host_reevaluate_pacing();
}

static void on_audio(enum display_audio_phase phase, const struct display_output_info *out, void *user)
{
	(void)user;
	if (H.bench_step)
		return; /* a benchmark run has no sound */
	if (phase == DISPLAY_AUDIO_RELEASE) {
		audio_wanted = false;
		audio_close();
	} else {
		audio_out = out && out->type == DISPLAY_OUTPUT_HDMI ? AUDIO_OUT_HDMI : AUDIO_OUT_LCD;
		/* the HDMI card of this port ("HDMI-A-2" -> the second one) */
		audio_cfg.hdmi_port = 0;
		if (out && !strncmp(out->name, "HDMI-A-", 7) && atoi(out->name + 7) > 1)
			audio_cfg.hdmi_port = atoi(out->name + 7) - 1;
		audio_wanted = true;
		/* Before the game is loaded (display_init), wait: an open stream
		 * would underrun while the core loads. */
		if (H.game_loaded) {
			audio_open(&audio_cfg, audio_out, audio_latency_ms() / 2);
			host_reevaluate_pacing();
		}
	}
}

/* --------------------------------------------------------------- input */

static unsigned pick_port_device(int port, bool analog)
{
	if (analog) {
		for (int i = 0; i < H.nctrl[port]; i++)
			if (strcasestr(H.ctrl[port][i].desc, "dualshock"))
				return H.ctrl[port][i].id;
		for (int i = 0; i < H.nctrl[port]; i++)
			if ((H.ctrl[port][i].id & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG)
				return H.ctrl[port][i].id;
	}
	return RETRO_DEVICE_JOYPAD;
}

static void update_ports(bool initial)
{
	bool n64 = !strcasecmp(H.system, "n64");

	for (int p = 0; p < HOST_MAX_PORTS; p++) {
		struct input_port_info pi;
		unsigned dev;

		hin_port_info(p, &pi);
		dev = pick_port_device(p, pi.connected && pi.has_analog);
		if (initial || dev != H.port_device[p]) {
			H.core.set_controller_port_device((unsigned)p, dev);
			if (!initial || dev != RETRO_DEVICE_JOYPAD)
				hlog(HLOG_INFO, "port %d: device 0x%x (%s)", p + 1, dev, pi.connected ? pi.name : "none");
			H.port_device[p] = dev;
		}
		/* N64 without any stick: the d-pad drives the left analog. */
		if (n64)
			hin_set_dpad_to_analog(p, !pi.has_analog);
	}
}

static void check_p1(void)
{
	struct input_port_info pi;

	hin_port_info(0, &pi);
	if (!pi.connected && !H.paused_disconnect) {
		H.paused_disconnect = true;
		host_play_pause(true);
		audio_pause(true);
		osd_toast(_("Player 1 controller disconnected: paused"), 3600 * 1000);
		sram_flush(false);
	} else if (pi.connected && H.paused_disconnect) {
		H.paused_disconnect = false;
		host_play_pause(false);
		osd_clear();
		/* TRANSLATORS: toast; %s is the controller's name */
		host_toast(_("Player 1: %s"), pi.name);
		audio_pause(false);
		H.next_frame_us = 0;
	}
}

static uint8_t *make_thumb(int *tw, int *th)
{
	uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;

	host_capture_last();
	if (!H.last_frame)
		return NULL;
	return frame_thumbnail(H.last_frame, (int)H.last_w, (int)H.last_h, (int)H.last_pitch, fmt, 160, tw, th);
}

/*
 * The auto state (.state.auto + its thumbnail) at exit or power-off,
 * synchronously and durably like every state (tmp, fsync, rename, the
 * previous one kept as .bak, fsync of the folder): a power cut leaves the old
 * or the new file, and a core that cannot serialize now leaves the old one
 * alone. Most states are a few hundred KB (tens of ms); a big one (N64, PS1:
 * several MB on the SD card) gets a "Saving..." frame first. The time is
 * logged, and the supervisor is told the state exists ("autostate" status
 * line): it records the session for the resume offer at the next boot.
 */
#define AUTO_STATE_BIG (1024 * 1024)

static void save_auto_state(void)
{
	int tw = 0, th = 0, r;
	uint8_t *thumb = make_thumb(&tw, &th);
	size_t size = H.core.serialize_size ? H.core.serialize_size() : 0;
	int64_t t0 = hnow_us(), ms;
	char line[64];

	if (size >= AUTO_STATE_BIG && !H.poweroff)
		show_exit_frame(_("Saving..."));   /* "Powering off..." is up already */
	r = state_save(STATE_SLOT_AUTO, thumb, tw, th, true);
	free(thumb);
	ms = (hnow_us() - t0) / 1000;
	if (r == 0) {
		hlog(HLOG_INFO, "auto state saved (%s): %zu KB in %lld ms", H.poweroff ? "power-off" : "exit",
		     (size + 1023) / 1024, (long long)ms);
		snprintf(line, sizeof(line), "%zu %lld", size, (long long)ms);
		host_status("autostate", line);
	} else {
		hlog(HLOG_WARN, "auto state NOT saved (%s) after %lld ms: the previous one, if any, is kept",
		     strerror(-r), (long long)ms);
	}
}

void host_do_hotkey_save(int slot)
{
	int tw = 0, th = 0, r;
	uint8_t *thumb = make_thumb(&tw, &th);

	r = state_save(slot, thumb, tw, th, false);
	free(thumb);
	if (r == -ENOTSUP)
		host_toast("%s", _("This game cannot save a state now"));
	else if (r < 0)
		/* TRANSLATORS: toast; %s is the system's English error text */
		host_toast(_("State NOT saved: %s"), strerror(-r));
	sram_flush(false);
}

void host_do_hotkey_load(int slot)
{
	int r = state_load(slot);

	if (!r)
		host_toast(_("State loaded, slot %d"), slot);
	else if (r == -ENOENT)
		host_toast(_("No state in slot %d"), slot);
	else
		host_toast(_("Cannot load the state of slot %d"), slot);
	H.next_frame_us = 0;
}

/* Select+Left/Right: the slot chosen, and whether it holds a state. */
static void slot_toast(void)
{
	if (state_exists(H.slot, NULL))
		host_toast(_("State slot %d"), H.slot);
	else
		/* TRANSLATORS: toast after Select+Left/Right: the slot holds no state */
		host_toast(_("State slot %d (empty)"), H.slot);
}

/* ------------------------------------------------ batch 2 hotkeys */

/*
 * Fast-forward (Select+R2 toggles it, the in-game menu sets a speed): N runs
 * per displayed frame, the extra frames neither shown nor heard, the sound of
 * the shown one muted (audio_process). The pacing is untouched: vsync + DRC
 * or the audio clock keep their period and their buffer fill, so turning it
 * off needs no resync beyond forgetting the frame timer; underruns while it
 * runs (a core that cannot reach the speed) and for 2 s after do not raise
 * the audio latency.
 */
void host_set_ff(int speed)
{
	if (speed < 2)
		speed = 0;
	if (speed > 4)
		speed = 4;
	if (speed == H.ff_speed)
		return;
	if (speed && !H.ff_speed) {
		H.ff_t0_ms = hnow_ms();
		H.ff_frame0 = H.frame;
		hlog(HLOG_INFO, "fast-forward on (x%d) at frame %llu", speed, (unsigned long long)H.frame);
	} else if (!speed) {
		int64_t ms = hnow_ms() - H.ff_t0_ms;
		double runs = (double)(H.frame - H.ff_frame0) * H.ff_speed;
		double normal = ms > 0 ? (double)ms * H.pace.core_fps / 1000.0 : 0;

		/* the effective speed: runs done / runs a normal speed would do */
		hlog(HLOG_INFO, "fast-forward off at frame %llu: %.0f runs in %lld ms (x%.2f)",
		     (unsigned long long)H.frame, runs, (long long)ms, normal > 0 ? runs / normal : 0.0);
	} else {
		hlog(HLOG_INFO, "fast-forward x%d", speed);
	}
	H.ff_speed = speed;
	H.next_frame_us = 0;
	H.frame_time_last_us = 0;
	H.underrun_window = 0;
	H.ff_resync_until = hnow_ms() + 2000;
	BOV.force = true;
	bov_update(hnow_ms());
}

void host_setting_changed(const char *key, const char *value)
{
	char line[96];

	/* the menu saves it for this game (gamedb) and applies what it owns
	 * (the CPU profile) at once */
	snprintf(line, sizeof(line), "%s %s", key, value);
	host_status("setting", line);
	hlog(HLOG_INFO, "setting for this game: %s", line);
}

/* The in-game menu and the switcher stop the play-time clock. */
void host_play_pause(bool pause)
{
	if (pause)
		pt_pause(&H.pt, hnow_ms());
	else
		pt_resume(&H.pt, hnow_ms());
}

static void playtime_report(void)
{
	char line[32];

	if (H.bench_step)
		return;
	snprintf(line, sizeof(line), "%lld", (long long)(pt_total_ms(&H.pt, hnow_ms()) / 1000));
	host_status("playtime", line);
}

/* 2x for the small pictures (Game Boy, GBA, Game Gear, WonderSwan, Lynx...),
 * nearest neighbour: a sharp picture of a useful size. */
static uint8_t *scale2x_rgb(const uint8_t *rgb, int w, int h)
{
	uint8_t *o = malloc((size_t)w * h * 12);

	if (!o)
		return NULL;
	for (int y = 0; y < h * 2; y++)
		for (int x = 0; x < w * 2; x++)
			memcpy(o + ((size_t)y * w * 2 + x) * 3, rgb + ((size_t)(y / 2) * w + x / 2) * 3, 3);
	return o;
}

/*
 * Select+L2: the game's picture (the core frame, without toasts or
 * overlays; read back from the GPU on the GLES path) as a PNG in
 * <screenshots>/<system>/<game>-<YYYYMMDD-HHMMSS>.png, at the core's
 * resolution, 2x when it is 240 pixels wide or less.
 */
static void take_screenshot(void)
{
	uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;
	char dir[PATH_MAX], path[PATH_MAX + 32], stamp[32];
	int tw = 0, th = 0, r = -1;
	uint8_t *rgb;
	void *png = NULL;
	size_t size = 0;
	time_t now = time(NULL);
	struct tm tm;
	int64_t t0 = hnow_us();

	host_capture_last();
	if (!H.last_frame) {
		host_toast("%s", _("No picture to save yet"));
		return;
	}
	H.busy_ok = true;
	rgb = frame_thumbnail(H.last_frame, (int)H.last_w, (int)H.last_h, (int)H.last_pitch, fmt, 4096, &tw, &th);
	if (rgb && tw <= 240) {
		uint8_t *big = scale2x_rgb(rgb, tw, th);

		if (big) {
			free(rgb);
			rgb = big;
			tw *= 2;
			th *= 2;
		}
	}
	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
	if (rgb && hpath(dir, sizeof(dir), "%s/%s", H.cfg.screenshots_root, H.system) && hmkdir_p(dir, 0755) == 0 &&
	    hpath(path, sizeof(path), "%s/%s-%s.png", dir, H.game, stamp)) {
		/* two in the same second: -2, -3... */
		for (int i = 2; hfile_exists(path) && i < 100; i++)
			hpath(path, sizeof(path), "%s/%s-%s-%d.png", dir, H.game, stamp, i);
		png = host_png_encode(rgb, tw, th, &size);
		if (png)
			r = hwrite_atomic(path, png, size, false);
	}
	free(png);
	free(rgb);
	H.busy_ok = false;
	if (r == 0) {
		hlog(HLOG_INFO, "screenshot %s (%dx%d, %zu KB) in %lld ms", path, tw, th, (size + 1023) / 1024,
		     (long long)(hnow_us() - t0) / 1000);
		/* TRANSLATORS: toast after Select+L2 */
		host_toast("%s", _("Screenshot saved"));
	} else {
		hlog(HLOG_ERROR, "screenshot: cannot write it in %s/%s", H.cfg.screenshots_root, H.system);
		host_toast("%s", _("Screenshot NOT saved"));
	}
	/* the pacing lost the time of the encoding: start again from now */
	H.next_frame_us = 0;
}

/* Select+Y: the game switcher. Another game chosen: this one is saved (auto
 * state, whatever Settings > Games says) and the process exits for the menu
 * to launch the other one, resumed. */
static void open_switcher(void)
{
	int r;

	if (!host_switcher_available()) {
		/* TRANSLATORS: toast after Select+Y when no other game was played yet */
		host_toast("%s", _("No other game played recently"));
		return;
	}
	sram_flush(false);
	host_play_pause(true);
	bov_hide(true);
	r = host_switcher_run();
	bov_hide(false);
	host_play_pause(false);
	if (r > 0 && !H.quit) {
		hlog(HLOG_INFO, "switcher: entry %d chosen, saving and leaving", r);
		H.switch_to = r;
		host_request_quit(HOST_EXIT_SWITCH);
	}
}

static void do_hotkey(enum input_hotkey hk)
{
	{
		if (H.bench_step) {
			/* a benchmark run: only Select+Start (stop the benchmark) */
			if (hk == IN_HK_EXIT) {
				hlog(HLOG_INFO, "bench: stopped by Select+Start");
				H.bench_aborted = true;
				host_request_quit(BENCH_EXIT_ABORT);
			}
			return;
		}
		switch (hk) {
		case IN_HK_EXIT:
			host_request_quit(HOST_EXIT_OK);
			break;
		case IN_HK_SAVE_STATE:
			host_do_hotkey_save(H.slot);
			break;
		case IN_HK_LOAD_STATE:
			host_do_hotkey_load(H.slot);
			break;
		case IN_HK_SLOT_NEXT:
			H.slot = (H.slot + 1) % 10;
			slot_toast();
			break;
		case IN_HK_SLOT_PREV:
			H.slot = (H.slot + 9) % 10;
			slot_toast();
			break;
		case IN_HK_MENU:
			sram_flush(false);
			bov_hide(true);   /* the menu covers the screen */
			host_play_pause(true);
			host_menu_run();
			host_play_pause(false);
			bov_hide(false);
			break;
		case IN_HK_FAST_FORWARD:
			host_set_ff(H.ff_speed > 1 ? 0 : H.ff_cap);
			break;
		case IN_HK_SCREENSHOT:
			take_screenshot();
			break;
		case IN_HK_SWITCHER:
			open_switcher();
			break;
		case IN_HK_RESET:
			H.core.reset();
			/* TRANSLATORS: toast: the game was reset (Select+B) */
			host_toast("%s", C_("toast", "Reset"));
			break;
		case IN_HK_POWER_OFF:
			/* SRAM was flushed by on_power_off(). Under the UI the power
			 * module owns the key and sends RSOS_SIG_POWEROFF; standalone
			 * (rsos-run) we power off ourselves. */
			if (H.cfg.status_fd < 0) {
				H.poweroff = true;
				host_request_quit(HOST_EXIT_POWEROFF);
			}
			break;
		case IN_HK_PAD_CONNECTED:
		case IN_HK_PAD_DISCONNECTED:
		case IN_HK_PORTS_CHANGED:
			update_ports(false);
			check_p1();
			if (hk == IN_HK_PAD_CONNECTED && hin_last_pad()[0])
				/* TRANSLATORS: toast; %s is the controller's name */
				host_toast(_("Connected: %s"), hin_last_pad());
			break;
		default:
			break;
		}
	}
}

static void handle_hotkeys(void)
{
	enum input_hotkey hk;

	while (hin_next_hotkey(&hk))
		do_hotkey(hk);
}

/*
 * Tests (--hotkey-script "F:name,..."): at displayed frame F, the host acts
 * as if the hotkey was pressed: ff (Select+R2), shot (Select+L2), menu,
 * save, load, exit; "mark" only logs the time (pacing checks).
 */
static void hotkey_script_step(void)
{
	static const struct { const char *name; enum input_hotkey hk; } map[] = {
		{ "ff", IN_HK_FAST_FORWARD }, { "shot", IN_HK_SCREENSHOT }, { "switcher", IN_HK_SWITCHER },
		{ "menu", IN_HK_MENU }, { "save", IN_HK_SAVE_STATE }, { "load", IN_HK_LOAD_STATE },
		{ "exit", IN_HK_EXIT },
	};
	const char *p = H.cfg.hotkey_script;

	while (p && *p) {
		long f = strtol(p, (char **)&p, 10);
		char name[16];
		size_t n = 0;

		if (*p == ':')
			p++;
		while (*p && *p != ',' && n + 1 < sizeof(name))
			name[n++] = *p++;
		name[n] = 0;
		if (*p == ',')
			p++;
		if ((uint64_t)f != H.frame)
			continue;
		if (!strcmp(name, "mark")) {
			hlog(HLOG_INFO, "mark: frame %llu, %lld ms (%lld ms after fast-forward)",
			     (unsigned long long)H.frame, (long long)hnow_ms(),
			     (long long)(hnow_ms() - (H.ff_resync_until - 2000)));
			continue;
		}
		for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
			if (!strcmp(map[i].name, name)) {
				hlog(HLOG_INFO, "hotkey script: %s at frame %ld", name, f);
				do_hotkey(map[i].hk);
			}
	}
}

static void on_power_off(void *user)
{
	(void)user;
	/* Called by the input layer as soon as the long press is detected:
	 * make the SRAM durable first, whatever happens next. */
	sram_flush(true);
}

/* -------------------------------------------------------------- pacing */

/* Sleeps up to us microseconds, waking up for display/input events. */
static void wait_us(int64_t us)
{
	struct pollfd pfd[2];
	int n = 0;
	struct timespec ts;

	if (us <= 0)
		return;
	if (H.display_ok) {
		pfd[n].fd = display_get_fd();
		pfd[n++].events = POLLIN;
	}
	ts.tv_sec = (time_t)(us / 1000000);
	ts.tv_nsec = (long)(us % 1000000) * 1000;
	ppoll(pfd, (nfds_t)n, &ts, NULL);
	if (H.display_ok)
		display_handle_events();
}

static void pace_wait(void)
{
	switch (H.pace.mode) {
	case PACE_AUDIO:
		if (audio_is_open()) {
			int target = audio_buffer_frames() / 2;

			for (int i = 0; i < 4; i++) {
				int64_t w = pacing_audio_wait_us(&H.pace, audio_queued(), target);

				if (w <= 200)
					break;
				wait_us(w);
			}
			break;
		}
		/* audio went away (output switch): pace on the clock */
		__attribute__((fallthrough));
	case PACE_TIMER: {
		int64_t now = hnow_us(), period = (int64_t)(1e6 / H.pace.core_fps);

		if (!H.next_frame_us || now - H.next_frame_us > 4 * period)
			H.next_frame_us = now;
		wait_us(H.next_frame_us - now);
		H.next_frame_us += period;
		break;
	}
	default:
		break;
	}
}

/* ----------------------------------------------------------- main loop */

/* RSOS_SIG_SLEEP: the supervisor turned the screen off. Stop everything
 * (the power module lowers the CPU clock) until RSOS_SIG_WAKE. */
static void sleep_until_wake(void)
{
	hlog(HLOG_INFO, "sleep");
	H.busy_ok = true;
	host_play_pause(true);
	sram_flush(false);
	audio_close();          /* before the screen goes off (HDMI encoder) */
	if (H.display_ok)
		display_set_active(false);
	while (!H.quit_sig && !H.poweroff_sig && H.sleep_sig != 2) {
		struct timespec ts = { 0, 200 * 1000000L };

		if (H.flush_sig) {
			H.flush_sig = 0;
			sram_flush(true);
		}
		nanosleep(&ts, NULL); /* signals interrupt it */
		if (H.display_ok)
			display_handle_events();
	}
	if (H.sleep_sig == 2)
		H.sleep_sig = 0;
	hlog(HLOG_INFO, "wake");
	if (H.display_ok)
		display_set_active(true);
	/* The key that woke the unit must not reach the game. */
	hin_poll();
	{
		enum input_hotkey hk;
		struct input_nav ev;

		while (hin_next_hotkey(&hk))
			;
		while (hin_next_nav(&ev))
			;
	}
	host_audio_setup();
	if (H.game_loaded)
		host_reevaluate_pacing();
	H.next_frame_us = 0;
	H.frame_time_last_us = 0;
	H.busy_ok = false;
	host_play_pause(false);
}

void host_poll_signals(void)
{
	if (H.sleep_sig == 1)
		sleep_until_wake();
	if (H.poweroff_sig) {
		/* RSOS_SIG_POWEROFF: SRAM + .state.auto, then exit POWEROFF. */
		H.poweroff_sig = 0;
		H.poweroff = true;
		host_request_quit(HOST_EXIT_POWEROFF);
	}
	if (H.quit_sig && !H.quit)
		host_request_quit(H.poweroff ? HOST_EXIT_POWEROFF : HOST_EXIT_OK);
	if (H.flush_sig) {
		/* SIGUSR1: flush SRAM now (low-battery monitor, rcK). */
		H.flush_sig = 0;
		sram_flush(true);
	}
}

static void periodic(int64_t now_ms)
{
	char msg[128];
	bool err;

	sram_tick(now_ms);
	while (saves_next_message(msg, sizeof(msg), &err))
		osd_toast(msg, err ? 4000 : 2000);
	bov_update(now_ms);
	/* the play time so far, for the menu to save (a crash loses at most
	 * one period of it) */
	if (H.pt_next_report && now_ms >= H.pt_next_report) {
		int period = H.cfg.playtime_report_s > 0 ? H.cfg.playtime_report_s : 300;

		H.pt_next_report = now_ms + (int64_t)period * 1000;
		playtime_report();
	}
	if (now_ms - H.last_battery_ms >= BATTERY_CHECK_MS) {
		int pct;
		bool chg;

		H.last_battery_ms = now_ms;
		/* Only without a supervisor: under the UI the power module owns
		 * battery decisions (docs/power.md) and signals us. */
		if (!H.cfg.headless && H.cfg.status_fd < 0 && !H.bench_step && sys_battery(&pct, &chg) == 0 && !chg) {
			static int warned = 100;

			if (pct <= BATTERY_CRITICAL) {
				hlog(HLOG_WARN, "battery %d%%: saving and powering off", pct);
				osd_toast(_("Battery empty: game saved"), 3000);
				H.poweroff = true;
				host_request_quit(HOST_EXIT_POWEROFF);
			} else if (pct <= BATTERY_LOW && warned > BATTERY_LOW) {
				warned = pct;
				host_toast(_("Battery low (%d%%)"), pct);
				sram_flush(false);
			}
		}
	}
}

static void perf_snap(struct perf_counters *c, double core_ms_max)
{
	struct hwr_timing t;
	struct display_stats ds;
	struct audio_stats as;
	struct timespec ts;

	memset(c, 0, sizeof(*c));
	memset(&ds, 0, sizeof(ds));
	c->t_us = hnow_us();
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	c->proc_cpu_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	c->runs = H.frame;
	c->delivered = H.frames_delivered;
	c->presented = H.frames_presented;
	c->skipped = H.frames_skipped;
	c->dupes = H.frames_duped;
	if (H.display_ok)
		display_get_stats(&ds);
	c->dropped = ds.dropped;
	c->core_ms = H.core_ms_total;
	c->core_ms_max = core_ms_max;
	hwr_get_timing(&t);
	c->gl_swap_us = t.swap_us;
	c->gl_present_us = t.present_us;
	c->gl_readback_us = t.readback_us;
	c->gl_swaps = t.swaps;
	c->gl_readbacks = t.readbacks;
	audio_get_stats(&as);
	c->underruns = as.underruns;
	perf_read_cpu(&c->cpu);
	c->stalls = H.stalls;
	c->stall_ms = H.stall_ms;
	c->cpu_khz = perf_read_cpu_khz(NULL);
	if (H.hw_requested) {
		struct glprobe_stats g;

		glprobe_get(&g);
		c->gl = true;
		c->gl_compiles = g.compiles;
		c->gl_compile_us = g.compile_us;
		c->gl_links = g.links;
		c->gl_link_us = g.link_us;
		c->gl_slow_draws = g.slow_draws;
		c->gl_slow_draw_us = g.slow_draw_us;
		c->gl_tex = g.tex;
		c->gl_tex_us = g.tex_us;
	}
}

/*
 * A frame whose core time is over PERF_STALL_MS (docs/host-design.md §13.4):
 * counted, and logged (up to STALL_LOG_MAX lines per game) with what the GL
 * probe saw during that retro_run(): shader compiles and links, draws slower
 * than GLPROBE_SLOW_DRAW_US (lima compiles a shader variant on the first
 * draw that needs it), texture uploads. "other" is the rest (the dynarec, the
 * RSP/RDP emulation, page faults...).
 */
#define STALL_LOG_MAX 100

static void stall_check(double core_ms, const struct glprobe_stats *g0)
{
	char gl[200] = "";
	bool glwork = false;

	if (core_ms < PERF_STALL_MS)
		return;
	H.stalls++;
	H.stall_ms += core_ms;
	if (H.hw_requested) {
		struct glprobe_stats g1, d;
		double ms;

		glprobe_get(&g1);
		glprobe_diff(g0, &g1, &d);
		ms = (double)(d.compile_us + d.link_us + d.slow_draw_us + d.tex_us) / 1000.0;
		glwork = d.compiles || d.links || d.slow_draws;
		snprintf(gl, sizeof(gl), "; GL: %llu compiles %.0f ms, %llu links %.0f ms, %llu slow draws %.0f ms, "
			 "%llu tex %.0f ms, other %.0f ms", (unsigned long long)d.compiles, d.compile_us / 1000.0,
			 (unsigned long long)d.links, d.link_us / 1000.0, (unsigned long long)d.slow_draws,
			 d.slow_draw_us / 1000.0, (unsigned long long)d.tex, d.tex_us / 1000.0,
			 core_ms > ms ? core_ms - ms : 0.0);
	}
	if (glwork)
		H.stalls_gl++;
	if (H.stalls_logged++ < STALL_LOG_MAX)
		hlog(HLOG_INFO, "stall: frame %llu: core %.0f ms%s; cpu0 %d MHz", (unsigned long long)H.frame, core_ms, gl,
		     perf_read_cpu_khz(NULL) / 1000);
	else if (H.stalls_logged == STALL_LOG_MAX + 1)
		hlog(HLOG_INFO, "stall: more than %d, the next ones are only counted", STALL_LOG_MAX);
}

static double core_fps(void)
{
	return H.av.timing.fps > 1 ? H.av.timing.fps : 60;
}

static void bench_tick(int64_t now_us);

/* 1 s overlay text, 10 s game.log line, benchmark window. */
static void perf_tick(int64_t now_ms, double core_ms, double frame_ms)
{
	H.core_ms_total += core_ms;
	for (int i = 0; i < 3; i++)
		if (core_ms > PW[i].max)
			PW[i].max = core_ms;
	if (B.measuring) {
		if (core_ms > B.max)
			B.max = core_ms;
		if (B.nft < B.cap && frame_ms > 0)
			B.ft[B.nft++] = (float)frame_ms;
	}
	if (now_ms >= PW[0].next_ms && (H.show_stats || B.active)) {
		struct perf_counters b;
		struct perf_report r;
		struct display_stats ds = { 0 };

		perf_snap(&b, PW[0].max);
		if (PW[0].a.t_us) {
			perf_diff(&PW[0].a, &b, core_fps(), &r);
			if (H.display_ok)
				display_get_stats(&ds);
			perf_format_overlay(&r, H.frames_skipped + ds.dropped, H.stats_line, sizeof(H.stats_line));
		}
		PW[0].a = b;
		PW[0].max = 0;
		PW[0].next_ms = now_ms + 1000;
		if (B.active)
			bench_tick(hnow_us()); /* refreshes the countdown line */
	}
	if (H.perf_log && now_ms >= PW[1].next_ms) {
		struct perf_counters b;
		struct perf_report r;
		char line[768];

		perf_snap(&b, PW[1].max);
		if (PW[1].a.t_us) {
			perf_diff(&PW[1].a, &b, core_fps(), &r);
			perf_format_log(&r, line, sizeof(line));
			hlog(HLOG_INFO, "perf: %s", line);
		}
		PW[1].a = b;
		PW[1].max = 0;
		PW[1].next_ms = now_ms + 10000;
	}
}

static void update_stats(int64_t now_ms, double core_ms)
{
	H.ft_acc_ms += core_ms;
	H.ft_n++;
	if (core_ms > H.ft_max_ms)
		H.ft_max_ms = core_ms;
	if (now_ms - H.stat_t0 >= 1000) {
		H.fps = (double)(H.frame - H.stat_frames0) * 1000.0 / (double)(now_ms - H.stat_t0);
		H.ft_avg_ms = H.ft_n ? H.ft_acc_ms / H.ft_n : 0;
		if (H.cfg.log_level >= HLOG_DEBUG)
			hlog(HLOG_DEBUG, "%.2f fps, core %.2f ms (max %.2f), fill %.0f%%, ratio %.5f", H.fps,
			     H.ft_avg_ms, H.ft_max_ms, audio_fill() * 100, H.last_ratio);
		H.stat_t0 = now_ms;
		H.stat_frames0 = H.frame;
		H.ft_acc_ms = 0;
		H.ft_n = 0;
		H.ft_max_ms = 0;
	}
}

static void run_one(void)
{
	int runs = H.ff_speed > 1 ? H.ff_speed : 1;

	for (int i = 0; i < runs; i++) {
		H.skip_video = i < runs - 1;
		if (H.frame_time.callback) {
			int64_t now = hnow_us();
			retro_usec_t d = H.frame_time_last_us ? now - H.frame_time_last_us : H.frame_time.reference;

			H.frame_time_last_us = now;
			H.frame_time.callback(d);
		}
		H.core.run();
		audio_process();
	}
	H.skip_video = false;
}

static void paused_step(void)
{
	host_poll_signals();
	audio_keepalive();
	host_present_last();
	wait_us(50000);
	hin_poll();
	handle_hotkeys();
	check_p1();
}

static void bench_start(void);
static void bench_report_open(void);

static void main_loop(void)
{
	H.stat_t0 = hnow_ms();
	if (H.cfg.bench_report)
		bench_report_open();
	while (!H.quit) {
		int64_t t0, now_ms, frame_ms;
		struct glprobe_stats g0 = { 0 };
		double core_ms;

		H.heartbeat++;
		host_poll_signals();
		if (H.quit)
			break;
		if (H.display_ok)
			display_handle_events();
		now_ms = hnow_ms();
		periodic(now_ms);
		if (H.paused_disconnect) {
			paused_step();
			continue;
		}
		pace_wait();
		/* Input right before retro_run(): lowest latency. */
		hin_poll();
		handle_hotkeys();
		if (H.cfg.hotkey_script)
			hotkey_script_step();
		if (H.quit)
			break;
		if (H.paused_disconnect)
			continue;
		if (H.audio_status_cb) {
			bool active = audio_is_open() && !audio_paused();
			unsigned occ = active ? (unsigned)(audio_fill() * 100.0 + 0.5) : 0;

			H.audio_status_cb(active, occ > 100 ? 100 : occ, active && occ < 25);
		}
		present_wait_us = 0;
		if (H.hw_requested)
			glprobe_get(&g0);
		t0 = hnow_us();
		frame_ms = B.last_iter_us ? t0 - B.last_iter_us : 0;
		B.last_iter_us = t0;
		run_one();
		H.frame++;
		core_ms = (double)(hnow_us() - t0 - present_wait_us) / 1000.0;
		stall_check(core_ms, &g0);
		update_stats(hnow_ms(), core_ms);
		perf_tick(hnow_ms(), core_ms, (double)frame_ms / 1000.0);
		if (B.active)
			bench_tick(hnow_us());
		if (H.cfg.menu_script && H.frame == 10)
			host_menu_script(H.cfg.menu_script);
		if (H.cfg.switcher_pick > 0 && H.frame == 12 && !H.bench_step) {
			/* tests: what picking entry N in the switcher does */
			hlog(HLOG_INFO, "switcher: entry %d picked (test)", H.cfg.switcher_pick);
			H.switch_to = H.cfg.switcher_pick;
			host_request_quit(HOST_EXIT_SWITCH);
		}
		if (H.bench_requested || (H.cfg.bench_start_frame > 0 && !H.bench_step &&
								  (long)H.frame == H.cfg.bench_start_frame)) {
			H.bench_requested = false;
			bench_start();
		}
		if (H.cfg.max_frames > 0 && (long)H.frame >= H.cfg.max_frames && !H.bench_step)
			host_request_quit(HOST_EXIT_OK);
		if (H.cfg.test_states && H.frame == (uint64_t)(H.cfg.max_frames / 2)) {
			char path[PATH_MAX + 16];

			hpath(path, sizeof(path), "%s/%s.state-test", H.state_dir, H.game);
			if (state_save_to(path) != 0)
				hlog(HLOG_ERROR, "state test: save failed");
		}
	}
}

/* ----------------------------------------------------------- benchmark */

/*
 * docs/host-design.md §18. This process either starts a benchmark (the
 * menu: save the start state, write the plan, exec the driver), runs one
 * configuration of it (--bench-step), or shows its results (--bench-report).
 */

static bool bench_core_ok(const char *id)
{
	struct core_info ci;
	bool ok;

	if (!strcmp(id, H.core_id))
		return true;
	if (coreinfo_load(&ci, H.cfg.core_info_dir, id) < 0)
		return false;
	ok = ci.library[0] && hfile_exists(ci.library) && hlist_has(ci.systems, H.system);
	coreinfo_free(&ci);
	return ok;
}

/* The shipped plan for this system (or core), without the configurations
 * whose core is not installed. Returns the number of configurations. */
static int bench_shipped_plan(struct bench_plan *p)
{
	char path[PATH_MAX];
	int k = 0;

	if (!hpath(path, sizeof(path), "%s/%s.bench.ini", H.cfg.coreopts_ship, H.system) ||
	    bench_plan_load(path, H.core_id, p) < 0) {
		if (!hpath(path, sizeof(path), "%s/%s.bench.ini", H.cfg.coreopts_ship, H.core_id) ||
		    bench_plan_load(path, H.core_id, p) < 0)
			return 0;
	}
	for (int i = 0; i < p->n; i++)
		if (bench_core_ok(p->c[i].core))
			p->c[k++] = p->c[i];
		else
			hlog_once(p->c[i].core, "bench: core %s not installed, its configurations are skipped",
				  p->c[i].core);
	p->n = k;
	if (H.cfg.bench_seconds > 0) {
		p->seconds = H.cfg.bench_seconds;
		p->warmup = 1;
	}
	return k;
}

bool host_bench_available(char *desc, size_t n)
{
	static int cached = -1;
	static struct bench_plan sp;

	if (H.bench_step)
		return false;
	if (cached < 0)
		cached = bench_shipped_plan(&sp);
	if (cached <= 0)
		return false;
	if (desc && n) {
		/* TRANSLATORS: in-game menu > Benchmark, value of the "Runs" row
		 * (right half of a 640 px row: keep it short) */
		snprintf(desc, n, _n("%d setting, about %d min", "%d settings, about %d min", sp.n), sp.n,
			 (bench_estimate_s(&sp) + 59) / 60);
		hutf8_trim(desc);
	}
	return true;
}

void host_bench_request(void)
{
	H.bench_requested = true;
}

const struct host_bench_report *host_bench_report(void)
{
	return report_loaded ? &REPORT : NULL;
}

static void bench_start(void)
{
	static struct bench_plan p;
	char dir[PATH_MAX], plan_path[PATH_MAX + 16], safe[128], stamp[32], png_path[PATH_MAX + 8];
	const char *args[128];
	char *const *av;
	int ac, n = 0, tw = 0, th = 0;
	time_t now = time(NULL);
	struct tm tm;
	uint8_t *thumb;

	if (bench_shipped_plan(&p) <= 0) {
		host_toast("%s", _("No benchmark for this system"));
		return;
	}
	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
	strftime(p.date, sizeof(p.date), "%Y-%m-%d %H:%M:%S", &tm);
	bench_safe_name(H.game, safe, sizeof(safe));
	hstrlcpy(p.game, H.game, sizeof(p.game));
	hstrlcpy(p.system, H.system, sizeof(p.system));
	hpath_stem(H.rom_path[0] ? H.rom_path : H.game, p.rom_stem, sizeof(p.rom_stem));
	hstrlcpy(p.core_id, H.core_id, sizeof(p.core_id));
	hstrlcpy(p.core_path, H.cfg.core_path, sizeof(p.core_path));
	hstrlcpy(p.state_core, H.core_id, sizeof(p.state_core));
	hpath(dir, sizeof(dir), "%s/bench", H.cfg.tmp_dir);
	hmkdir_p(dir, 0755);
	hpath(p.state, sizeof(p.state), "%s/start.state", dir);
	hpath(plan_path, sizeof(plan_path), "%s/plan.ini", dir);
	hmkdir_p(H.cfg.logs_dir, 0755);
	hpath(p.results, sizeof(p.results), "%s/bench-%s-%s.txt", H.cfg.logs_dir, safe, stamp);
	state_path(STATE_SLOT_AUTO, p.auto_state, sizeof(p.auto_state));
	for (int i = 0; i < BENCH_MAX_CONFIGS; i++) {
		char rp[PATH_MAX];

		bench_result_path(plan_path, i, rp, sizeof(rp));
		unlink(rp);
	}
	H.busy_ok = true;
	if (state_save_to(p.state) != 0) {
		H.busy_ok = false;
		host_toast("%s", _("Benchmark: this game cannot save a state now"));
		return;
	}
	thumb = make_thumb(&tw, &th);
	if (thumb) {
		size_t sz;
		void *png = host_png_encode(thumb, tw, th, &sz);

		hpath(png_path, sizeof(png_path), "%s.png", p.state);
		if (png)
			hwrite_atomic(png_path, png, sz, false);
		free(png);
		free(thumb);
	}
	if (bench_plan_write(plan_path, &p) != 0) {
		H.busy_ok = false;
		host_toast(_("Benchmark: cannot write %s"), plan_path);
		return;
	}
	hlog(HLOG_INFO, "bench: %d configurations, %d s warm-up + %d s each, report %s", p.n, p.warmup, p.seconds,
	     p.results);
	/* Hand the display and the devices over, then become the driver
	 * (same pid: the UI keeps waiting for us). */
	sram_flush(true);
	opts_autosave();
	playtime_report();   /* this process is replaced: the play time so far */
	audio_close();
	if (H.display_ok)
		display_shutdown();
	H.display_ok = false;
	if (H.hw_requested)
		hwr_deinit();
	hin_close();
	saves_shutdown();
	ac = host_saved_args(&av);
	if (ac > 0 && !strcmp(av[0], "--run")) {
		args[n++] = "rsos-frontend";
		args[n++] = "--run";
	} else {
		args[n++] = ac > 0 ? av[0] : "rsos-run";
	}
	n += bench_filter_args(ac > 0 ? ac - 1 : 0, ac > 0 ? av + 1 : av, false, args + n, 120 - n);
	args[n++] = "--bench-driver";
	args[n++] = plan_path;
	args[n] = NULL;
	fflush(stderr);
	/* a sleep/power-off signal before the driver's handlers exist would
	 * kill it (default action): blocked, it waits (review F-M12) */
	host_supervisor_signals(true, NULL, NULL, NULL);
	execv("/proc/self/exe", (char *const *)args);
	host_supervisor_signals(false, NULL, NULL, NULL);
	hlog(HLOG_ERROR, "bench: exec: %s", strerror(errno));
	_exit(HOST_EXIT_FAIL); /* the display is gone; saves are on disk */
}

static int bench_step_setup(void)
{
	const struct bench_config *c;

	if (bench_plan_load(H.cfg.bench_plan, NULL, &B.plan) < 0 || H.cfg.bench_step < 0 ||
	    H.cfg.bench_step >= B.plan.n) {
		hlog(HLOG_ERROR, "bench: cannot use run %d of %s", H.cfg.bench_step, H.cfg.bench_plan);
		return -1;
	}
	B.idx = H.cfg.bench_step;
	c = &B.plan.c[B.idx];
	H.bench_step = true;
	B.active = true;
	if (strcmp(c->core, H.core_id))
		hlog(HLOG_WARN, "bench: run %s is for core %s, running %s", c->id, c->core, H.core_id);
	for (int k = 0; k < c->nkv; k++)
		opts_set_override(c->kv[k].key, c->kv[k].value);
	B.cap = (size_t)(B.plan.seconds + 2) * 500;
	B.ft = malloc(B.cap * sizeof(float));
	if (!B.ft)
		B.cap = 0;
	hlog(HLOG_INFO, "bench: run %d/%d \"%s\" (%s), core %s, %d s warm-up + %d s measured", B.idx + 1,
	     B.plan.n, c->id, c->label, H.core_id, B.plan.warmup, B.plan.seconds);
	return 0;
}

static void bench_step_load_state(void)
{
	int64_t now;

	if (!strcmp(B.plan.state_core, H.core_id) && hfile_exists(B.plan.state)) {
		H.core.run();
		H.ain_n = 0;
		if (state_load_from(B.plan.state) != 0) {
			B.from_boot = true;
			snprintf(B.note, sizeof(B.note), "the core refused the start state");
		}
	} else {
		B.from_boot = true;
		snprintf(B.note, sizeof(B.note), "start state is from %s", B.plan.state_core);
	}
	if (B.from_boot)
		hlog(HLOG_WARN, "bench: %s: running from boot", B.note);
	now = hnow_us();
	B.t_measure = now + (int64_t)B.plan.warmup * 1000000;
	B.t_end = B.t_measure + (int64_t)B.plan.seconds * 1000000;
}

static void bench_step_finish(void)
{
	const struct bench_config *c = &B.plan.c[B.idx];
	struct perf_counters b;
	struct perf_report r;
	struct bench_result res;
	char path[PATH_MAX], buf[4096];
	double sum = 0;
	int len;

	perf_snap(&b, B.max);
	perf_diff(&B.a, &b, core_fps(), &r);
	memset(&res, 0, sizeof(res));
	hstrlcpy(res.id, c->id, sizeof(res.id));
	hstrlcpy(res.label, c->label, sizeof(res.label));
	hstrlcpy(res.core, H.core_id, sizeof(res.core));
	hstrlcpy(res.status, "ok", sizeof(res.status));
	hstrlcpy(res.note, B.note, sizeof(res.note));
	res.from_boot = B.from_boot;
	res.core_fps = core_fps();
	res.secs = r.secs;
	res.speed = r.speed_pct;
	res.fps = r.fps;
	res.eff_fps = r.eff_fps;
	res.vi = r.vi_rate;
	for (size_t i = 0; i < B.nft; i++)
		sum += B.ft[i];
	res.ft_avg = B.nft ? sum / (double)B.nft : 0;
	res.ft_p95 = perf_percentile(B.ft, B.nft, 95);
	res.ft_p99 = perf_percentile(B.ft, B.nft, 99);
	res.core_ms = r.core_ms_avg;
	res.swap_ms = r.swap_ms;
	res.present_ms = r.present_ms;
	res.readback_ms = r.readback_ms;
	res.proc_cpu = r.proc_cpu_pct;
	res.ncpu = r.ncpu > 4 ? 4 : r.ncpu;
	for (int i = 0; i < res.ncpu; i++)
		res.cpu[i] = r.cpu[i];
	res.runs = r.runs;
	res.delivered = r.delivered;
	res.presented = r.presented;
	res.skipped = r.skipped + r.dropped;
	res.dupes = r.dupes;
	/* the last picture: a PNG next to the report, and a blank check */
	{
		uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;
		int tw = 0, th = 0;
		uint8_t *rgb;

		host_capture_last();
		rgb = H.last_frame ? frame_thumbnail(H.last_frame, (int)H.last_w, (int)H.last_h, (int)H.last_pitch,
						      fmt, 320, &tw, &th) : NULL;
		if (rgb) {
			size_t sz, bl = strlen(B.plan.results);
			void *png;
			double flat = bench_flat_fraction(rgb, tw, th);

			res.blank = flat > 0.97;
			if (bl > 4 && !strcmp(B.plan.results + bl - 4, ".txt"))
				bl -= 4;
			hpath(res.shot, sizeof(res.shot), "%.*s-%02d-%s.png", (int)bl, B.plan.results, B.idx + 1, c->id);
			png = host_png_encode(rgb, tw, th, &sz);
			if (!png || hwrite_atomic(res.shot, png, sz, false) != 0)
				res.shot[0] = 0;
			free(png);
			free(rgb);
			if (res.blank)
				hlog(HLOG_WARN, "bench: the last picture is %.0f %% one colour", flat * 100);
		} else {
			res.blank = true;
		}
	}
	len = bench_result_format(&res, buf, sizeof(buf));
	bench_result_path(H.cfg.bench_plan, B.idx, path, sizeof(path));
	if (len <= 0 || hwrite_atomic(path, buf, (size_t)len, false) != 0)
		hlog(HLOG_ERROR, "bench: cannot write %s", path);
	perf_format_log(&r, buf, sizeof(buf));
	hlog(HLOG_INFO, "bench: result \"%s\": %s", c->id, buf);
	hlog(HLOG_INFO, "bench: frame time avg %.2f ms, p95 %.2f ms, p99 %.2f ms (%zu frames)%s", res.ft_avg,
	     res.ft_p95, res.ft_p99, B.nft, res.blank ? ", picture blank?" : "");
}

static void bench_tick(int64_t now_us)
{
	const struct bench_config *c = &B.plan.c[B.idx];
	int left;

	if (!B.active || !B.t_end)
		return;
	if (!B.measuring && now_us >= B.t_measure) {
		perf_snap(&B.a, 0);
		B.max = 0;
		B.nft = 0;
		B.measuring = true;
		hlog(HLOG_INFO, "bench: measuring");
	} else if (B.measuring && now_us >= B.t_end) {
		bench_step_finish();
		B.active = false;
		B.measuring = false;
		host_request_quit(HOST_EXIT_OK);
		return;
	}
	left = (int)(((B.measuring ? B.t_end : B.t_measure) - now_us + 999999) / 1000000);
	/* Not translated: drawn with the 8x8 font on the overlay plane
	 * (batt_overlay.c, ASCII only), like the FPS line. */
	snprintf(BOV.bench_line, sizeof(BOV.bench_line), "BENCH %d/%d %.20s %s %ds  SELECT+START: stop", B.idx + 1,
		 B.plan.n, c->id, B.measuring ? "measuring" : "warm-up", left);
}

int host_bench_use_best(char *msg, size_t n)
{
	const struct bench_config *c;
	char choices[PATH_MAX], dir[PATH_MAX];
	int r;

	/* msg: the menu's status line (translated); the log stays English */
	if (!report_loaded || REPORT.best < 0) {
		snprintf(msg, n, "%s", _("No result to use"));
		return -1;
	}
	c = bench_find_config(&RP, REPORT.r[REPORT.best].id);
	if (!c) {
		/* TRANSLATORS: %s is a benchmark configuration id ("glide-640") */
		snprintf(msg, n, _("Unknown configuration %s"), REPORT.r[REPORT.best].id);
		hutf8_trim(msg);
		return -1;
	}
	hpath_dir(H.cfg.coreopts_user, dir, sizeof(dir));
	hpath(choices, sizeof(choices), "%s/cores.ini", dir);
	r = bench_apply(c, H.cfg.coreopts_user, H.game, choices, H.system, RP.rom_stem, H.core_id);
	if (r) {
		/* TRANSLATORS: %s is the system's English error text */
		snprintf(msg, n, _("Could not save: %s"), strerror(-r));
		hlog(HLOG_ERROR, "bench: use %s: could not save: %s", c->id, strerror(-r));
	} else if (!strcmp(c->core, H.core_id)) {
		opts_reload_game();
		/* TRANSLATORS: %s is a benchmark configuration id ("glide-640") */
		snprintf(msg, n, _("Saved \"%s\" for this game (applies next start)"), c->id);
		hlog(HLOG_INFO, "bench: use %s: saved for this game (applies next start)", c->id);
	} else {
		/* TRANSLATORS: configuration id, then the core's name ("mupen64plus_next") */
		snprintf(msg, n, _("Saved \"%s\": next start uses %s"), c->id, c->core);
		hlog(HLOG_INFO, "bench: use %s: saved, next start uses %s", c->id, c->core);
	}
	hutf8_trim(msg);
	return r;
}

static void bench_report_open(void)
{
	if (bench_plan_load(H.cfg.bench_plan, NULL, &RP) < 0) {
		hlog(HLOG_ERROR, "bench: no plan %s", H.cfg.bench_plan);
		return;
	}
	memset(&REPORT, 0, sizeof(REPORT));
	for (int i = 0; i < RP.n && REPORT.n < BENCH_MAX_CONFIGS; i++) {
		char path[PATH_MAX];

		bench_result_path(H.cfg.bench_plan, i, path, sizeof(path));
		if (bench_result_load(path, &REPORT.r[REPORT.n]) == 0)
			REPORT.n++;
	}
	REPORT.best = bench_rank(REPORT.r, REPORT.n, REPORT.order);
	hstrlcpy(REPORT.path, RP.results, sizeof(REPORT.path));
	report_loaded = true;
	for (int k = 0; k < REPORT.n; k++) {
		const struct bench_result *r = &REPORT.r[REPORT.order[k]];

		hlog(HLOG_INFO, "bench: #%d %s (%s, %s): %s, speed %.1f %%, %.1f fps, %.1f effective fps%s", k + 1,
		     r->id, r->label, r->core, r->status, r->speed, r->fps, r->eff_fps,
		     bench_eligible(r) ? "" : " (not ranked)");
	}
	hlog(HLOG_INFO, "bench: best %s, report %s", REPORT.best >= 0 ? REPORT.r[REPORT.best].id : "none",
	     REPORT.path);
	if (H.cfg.bench_auto_apply) {
		char msg[160];

		host_bench_use_best(msg, sizeof(msg));
	}
	if (H.display_ok) {
		sram_flush(false);
		bov_hide(true);
		host_menu_run_bench_results();
		bov_hide(false);
	}
}

/* ------------------------------------------------------------ watchdog */

static void *watchdog(void *arg)
{
	uint64_t last = H.heartbeat;
	int stuck = 0;

	(void)arg;
	for (;;) {
		sleep(1);
		if (H.busy_ok) {
			stuck = 0;
			continue;
		}
		if (H.heartbeat != last) {
			last = H.heartbeat;
			stuck = 0;
			continue;
		}
		if (++stuck >= WATCHDOG_S) {
			hlog(HLOG_ERROR, "watchdog: no progress for %d s, the core is hung", WATCHDOG_S);
			host_status("error", _("The emulator stopped responding"));
			/* No save: memory may be corrupt. The last periodic SRAM
			 * flush is on disk. */
			_exit(HOST_EXIT_HANG);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------ settings */

static char g_lang[32];   /* --lang, else settings `language` ("" = English) */

static const char *setting(const struct ini *s, const char *key)
{
	const char *v = ini_get(s, "", key);

	return v ? v : ini_get(s, "game", key);
}

static void load_settings(void)
{
	struct ini s = { 0 };
	const char *v;

	if (H.cfg.settings_path)
		ini_load(&s, H.cfg.settings_path);
	/* Settings > Language (the menu UI also passes --lang, which wins) */
	hstrlcpy(g_lang, H.cfg.lang ? H.cfg.lang : (v = setting(&s, "language")) ? v : "", sizeof(g_lang));
	H.scale =H.cfg.scale >= 0 ? H.cfg.scale : DISPLAY_SCALE_ASPECT;
	if (H.cfg.scale < 0 && ((v = setting(&s, "game_scale")) || (v = setting(&s, "scaling")))) {
		if (!strcasecmp(v, "integer"))
			H.scale = DISPLAY_SCALE_INTEGER;
		else if (!strcasecmp(v, "stretch"))
			H.scale = DISPLAY_SCALE_STRETCH;
	}
	H.show_stats = H.cfg.show_stats >= 0 ? H.cfg.show_stats : ini_get_bool(&s, "", "game_show_fps", false);
	/* Settings > Games > Auto-save on exit (autosave_exit, default on; the
	 * older key game_autosave is still honoured when the new one is absent).
	 * A power-off always writes the auto state, whatever this says. */
	if (H.cfg.autosave >= 0)
		H.autosave = H.cfg.autosave;
	else if (ini_get(&s, "", "autosave_exit"))
		H.autosave = ini_get_bool(&s, "", "autosave_exit", true);
	else
		H.autosave = ini_get_bool(&s, "", "game_autosave", true);
	audio_config_defaults(&audio_cfg);
	/* the board's ALSA cards (board.ini audio_internal / audio_hdmi), else auto */
	if (board_get()->audio_internal_dev[0])
		audio_cfg.device_lcd = board_get()->audio_internal_dev;
	if (board_get()->audio_hdmi_dev[0])
		audio_cfg.device_hdmi = board_get()->audio_hdmi_dev;
	if (board_get()->audio_hdmi_pcm[0])
		audio_cfg.hdmi_pcm = board_get()->audio_hdmi_pcm;
	audio_cfg.latency_ms = H.cfg.audio_latency_ms > 0 ? H.cfg.audio_latency_ms :
			       ini_get_int(&s, "", "audio_latency_ms", 64);
	audio_set_latency(audio_cfg.latency_ms);
	if (H.cfg.vsync_tolerance <= 0) {
		int pct10 = ini_get_int(&s, "", "vsync_tolerance_permille", 10);

		H.cfg.vsync_tolerance = pct10 / 1000.0;
	}
	H.ff_speed = 0; /* fast-forward off at start */
	/* Select+R2's speed: Settings > Games > Fast-forward speed (2-4x, 3x) */
	H.ff_cap = H.cfg.ff_speed > 0 ? H.cfg.ff_speed : ini_get_int(&s, "", "ff_speed", 3);
	if (H.ff_cap < 2 || H.ff_cap > 4)
		H.ff_cap = 3;
	/* Settings > Controls > Controller vibration (default on) */
	H.rumble = H.cfg.rumble >= 0 ? H.cfg.rumble != 0 : ini_get_bool(&s, "", "rumble", true);
	hstrlcpy(H.cpu_profile, H.cfg.cpu_profile && *H.cfg.cpu_profile ? H.cfg.cpu_profile : "auto",
		 sizeof(H.cpu_profile));
	/* Input: the same keys the UI applies to its own input layer. */
	if (H.cfg.p1_policy < 0) {
		v = setting(&s, "p1");
		H.cfg.p1_policy = v && !strcasecmp(v, "builtin") ? INPUT_P1_BUILTIN :
				  v && !strcasecmp(v, "external") ? INPUT_P1_EXTERNAL : INPUT_P1_AUTO;
	}
	if (H.cfg.cz_buttons < 0)
		H.cfg.cz_buttons = ini_get_bool(&s, "", "cz_buttons", false);
	/* Settings > Display > LCD refresh rate (the UI also passes --lcd-refresh,
	 * which wins: during the 15 s trial the value is not saved yet). */
	if (H.cfg.lcd_refresh < 0)
		H.cfg.lcd_refresh = display_parse_lcd_refresh(setting(&s, "lcd_refresh"));
	/* In-game battery indicator (Settings > Display). */
	BOV.enabled = ini_get_bool(&s, "", "game_battery_overlay", true);
	v = setting(&s, "game_battery_corner");
	BOV.corner = !v ? DISPLAY_CORNER_TOP_RIGHT :
		     !strcasecmp(v, "top-left") ? DISPLAY_CORNER_TOP_LEFT :
		     !strcasecmp(v, "bottom-right") ? DISPLAY_CORNER_BOTTOM_RIGHT :
		     !strcasecmp(v, "bottom-left") ? DISPLAY_CORNER_BOTTOM_LEFT : DISPLAY_CORNER_TOP_RIGHT;
	ini_free(&s);
}

/* ---------------------------------------------------------------- run */

void host_config_defaults(struct host_config *c)
{
	memset(c, 0, sizeof(*c));
	c->core_info_dir = "/usr/share/rsos/cores";
	c->bios_dir = "/data/bios";
	c->saves_root = "/data/saves";
	c->states_root = "/data/states";
	c->coreopts_ship = "/usr/share/rsos/coreopts";
	c->coreopts_user = "/data/rsos/coreopts";
	c->tmp_dir = "/tmp/rsos";
	c->settings_path = "/data/rsos/settings.ini";
	c->shader_cache_dir = "/data/rsos/cache/mesa";
	c->lcd_refresh = -1;
	c->scale = -1;
	c->show_stats = -1;
	c->autosave = -1;
	c->status_fd = -1;
	c->governor = true;
	c->load_slot = -2;
	c->p1_policy = -1;
	c->cz_buttons = -1;
	c->logs_dir = "/data/rsos/logs";
	c->locale_dir = "/usr/share/rsos/locale";
	c->fonts_dir = "/usr/share/rsos/fonts";
	c->bench_step = -1;
	c->log_level = HLOG_INFO;
	c->screenshots_root = "/data/screenshots";
	c->rumble = -1;
}

static void dump_png(const char *path)
{
	uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;
	int tw, th;
	uint8_t *rgb;
	size_t size;
	void *png;

	host_capture_last();
	if (!H.last_frame) {
		hlog(HLOG_WARN, "dump: no frame");
		return;
	}
	rgb = frame_thumbnail(H.last_frame, (int)H.last_w, (int)H.last_h, (int)H.last_pitch, fmt, 4096, &tw, &th);
	png = rgb ? host_png_encode(rgb, tw, th, &size) : NULL;
	if (png && hwrite_atomic(path, png, size, false) == 0)
		hlog(HLOG_INFO, "last frame (%dx%d) -> %s", tw, th, path);
	else
		hlog(HLOG_ERROR, "cannot write %s", path);
	free(png);
	free(rgb);
}

static uint64_t frame_hash(void)
{
	int bpp = fmt_bpp(H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format);
	uint64_t h = HASH64_INIT;

	if (!H.last_frame)
		return 0;
	for (unsigned y = 0; y < H.last_h; y++)
		h = hhash64_cont(h, (const uint8_t *)H.last_frame + y * H.last_pitch, (size_t)H.last_w * (size_t)bpp);
	return h;
}

/* Runs the second half again from the saved state: the frame must match. */
static int state_test(void)
{
	char path[PATH_MAX + 16];
	long half = H.cfg.max_frames / 2, rest = H.cfg.max_frames - half;
	uint64_t h1 = frame_hash(), h2;
	int ret;

	hpath(path, sizeof(path), "%s/%s.state-test", H.state_dir, H.game);
	ret = state_load_from(path);
	if (ret) {
		fprintf(stderr, "STATE TEST: FAIL (load: %s)\n", strerror(-ret));
		return HOST_EXIT_TEST_FAIL;
	}
	for (long i = 0; i < rest; i++) {
		H.core.run();
		audio_process();
	}
	h2 = frame_hash();
	unlink(path);
	fprintf(stderr, "STATE TEST: %s (frame %ld: %016llx, replayed from frame %ld: %016llx, state %lld bytes)\n",
		h1 == h2 ? "PASS" : "FAIL", H.cfg.max_frames, (unsigned long long)h1, half,
		(unsigned long long)h2, (long long)H.core.serialize_size());
	return h1 == h2 ? HOST_EXIT_OK : HOST_EXIT_TEST_FAIL;
}

/* The CPU clock policy the game runs under (under the UI the power module
 * sets it: the game profile is the performance governor). */
static void log_cpufreq(void)
{
	static const char dir[] = "/sys/devices/system/cpu/cpu0/cpufreq";
	char gov[32] = "?", path[96];
	int cur, lo, hi;
	FILE *f;

	snprintf(path, sizeof(path), "%s/scaling_governor", dir);
	if ((f = fopen(path, "re"))) {
		if (!fgets(gov, sizeof(gov), f))
			snprintf(gov, sizeof(gov), "?");
		gov[strcspn(gov, "\n")] = 0;
		fclose(f);
	}
	cur = perf_read_cpu_khz(NULL);
	snprintf(path, sizeof(path), "%s/scaling_min_freq", dir);
	lo = perf_read_cpu_khz(path);
	snprintf(path, sizeof(path), "%s/scaling_max_freq", dir);
	hi = perf_read_cpu_khz(path);
	hlog(strcmp(gov, board_get()->cpu_governor_game) ? HLOG_WARN : HLOG_INFO,
	     "cpu: governor %s, %d MHz (policy %d-%d MHz)%s", gov,
	     cur / 1000, lo / 1000, hi / 1000,
	     strcmp(gov, board_get()->cpu_governor_game) ? ": not the game profile" : "");
}

/* A start failure: the English msgid to the log, its translation to the
 * menu UI (status pipe). */
static int fail(int code, const char *msgid)
{
	hlog(HLOG_ERROR, "%s", msgid);
	host_status("error", _(msgid));
	return code;
}

/* The same with a message that is already translated (BIOS check,
 * content, GPU). */
static int fail_text(int code, const char *msg)
{
	hlog(HLOG_ERROR, "%s", msg);
	host_status("error", msg);
	return code;
}

/* Language of the in-game menu and of the messages (before the BIOS check,
 * whose message is shown by the menu UI), then the fonts for it. */
static void setup_language(const char *lang)
{
	i18n_set_dir(H.cfg.locale_dir);
	if (lang && *lang && i18n_set_language(lang) < 0)
		hlog(HLOG_WARN, "language %s: no usable catalog in %s, English", lang, i18n_dir());
	hlog(HLOG_INFO, "language %s (%d translations)", i18n_language(), i18n_catalog_entries());
	/* nothing is drawn by a headless run, except the menu screenshot */
	if (!H.cfg.headless || H.cfg.menu_shot || H.cfg.switcher_shot)
		draw_set_fonts(H.cfg.fonts_dir);
}

int host_run(const struct host_config *cfg)
{
	struct sigaction sa;
	char err[256] = "", warn[256] = "", dir[PATH_MAX];
	struct retro_game_info gi;
	struct opts_paths op;
	pthread_t wd;
	int ret = HOST_EXIT_FAIL;
	bool core_inited = false;

	memset(&H, 0, sizeof(H));
	memset(&BOV, 0, sizeof(BOV));
	memset(PW, 0, sizeof(PW));
	memset(&B, 0, sizeof(B));
	BOV.pct = -1;
	H.cfg = *cfg;
	H.exit_code = HOST_EXIT_OK;
	H.switch_to = -1;
	if (!H.cfg.screenshots_root)
		H.cfg.screenshots_root = "/data/screenshots";
	H.drm_format = DRM_FORMAT_XRGB1555; /* libretro default: 0RGB1555 */
	H.pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
	hlog_set_level((enum hlog_level)cfg->log_level);
	surf_w = surf_h = 0;
	surf_fmt = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(HOST_SIG_POWEROFF, &sa, NULL);
	sigaction(HOST_SIG_SLEEP, &sa, NULL);
	sigaction(HOST_SIG_WAKE, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	/* blocked across the exec by the benchmark driver (F-M12): the ones
	 * that came meanwhile are delivered now, to the handlers */
	host_supervisor_signals(false, NULL, NULL, NULL);

	load_settings();
	setup_language(g_lang);

	/* Identity. */
	coreinfo_id_from_path(cfg->core_path, H.core_id, sizeof(H.core_id));
	if (cfg->rom_path)
		hstrlcpy(H.rom_path, cfg->rom_path, sizeof(H.rom_path));
	if (cfg->system && *cfg->system) {
		hstrlcpy(H.system, cfg->system, sizeof(H.system));
	} else if (cfg->rom_path) {
		hpath_dir(cfg->rom_path, dir, sizeof(dir));
		hstrlcpy(H.system, hpath_base(dir), sizeof(H.system));
	} else {
		hstrlcpy(H.system, H.core_id, sizeof(H.system));
	}
	hstrlcpy(H.bios_dir, cfg->bios_dir, sizeof(H.bios_dir));
	hpath(H.save_dir, sizeof(H.save_dir), "%s/%s", cfg->saves_root, H.system);
	hpath(H.state_dir, sizeof(H.state_dir), "%s/%s", cfg->states_root, H.system);
	hlog(HLOG_INFO, "game process: core %s, system %s, content %s", H.core_id, H.system,
	     cfg->rom_path ? cfg->rom_path : "(none)");

	/* Metadata and BIOS: refuse to start before touching anything. */
	if (coreinfo_load(&H.info, cfg->core_info_dir, H.core_id) < 0)
		hlog(HLOG_WARN, "no %s/%s.ini: core defaults only", cfg->core_info_dir, H.core_id);
	if (!bios_check(&H.info, H.system, cfg->rom_path, H.bios_dir, err, sizeof(err), warn, sizeof(warn))) {
		ret = fail_text(HOST_EXIT_ERROR, err);
		goto out_info;
	}
	if (warn[0])
		host_status("warn", warn);
	hmkdir_p(H.bios_dir, 0755);
	coreinfo_install_system_files(&H.info, H.bios_dir);
	coreinfo_install_system_tree(&H.info, H.bios_dir);
	if (!strcasecmp(H.info.renderer, "gles2")) {
		if (!cfg->headless)
			setenv("EGL_PLATFORM", "gbm", 0); /* GLideN64 opens the default EGL display */
		/* before the core can create an EGL display itself (§13.4) */
		hwr_shader_cache_env(cfg->shader_cache_dir);
	}

	if (core_open(cfg->core_path) < 0) {
		ret = fail(HOST_EXIT_ERROR, N_("Cannot load the emulator core"));
		goto out_info;
	}
	H.core.get_system_info(&H.sysinfo);
	H.info.block_extract |= H.sysinfo.block_extract;
	hlog(HLOG_INFO, "core: %s %s (need_fullpath %d, extensions %s)", H.sysinfo.library_name,
	     H.sysinfo.library_version, H.sysinfo.need_fullpath,
	     H.sysinfo.valid_extensions ? H.sysinfo.valid_extensions : "");
	if (content_prepare(err, sizeof(err)) < 0) {
		ret = fail_text(HOST_EXIT_ERROR, err);
		goto out_core;
	}

	op.ship_dir = cfg->coreopts_ship;
	op.user_dir = cfg->coreopts_user;
	opts_init(H.core_id, H.game, &H.info.ini, &op);
	if (cfg->bench_step >= 0 && bench_step_setup() < 0) {
		ret = fail(HOST_EXIT_FAIL, N_("Benchmark: bad plan"));
		goto out_opts;
	}
	if (saves_init(H.save_dir, H.state_dir, H.game) < 0) {
		ret = fail(HOST_EXIT_FAIL, N_("Cannot start the save thread"));
		goto out_opts;
	}
	if (H.bench_step)
		saves_set_readonly(true);
	hin_open(!cfg->headless, on_power_off, NULL);
	hin_set_rumble(H.rumble);
	/* Before display_init() (docked state) and the remap lookup. */
	hin_set_cz_buttons(H.cfg.cz_buttons > 0);
	hin_set_p1_policy((enum input_p1_policy)H.cfg.p1_policy);
	if (H.cfg.p1_device && *H.cfg.p1_device) {
		hin_set_p1_device(H.cfg.p1_device);
		hlog(HLOG_INFO, "input: launched by %s (player 1 under the auto policy)", H.cfg.p1_device);
	}
	hlog(HLOG_INFO, "input: %s, player 1 %s, C/Z buttons %s", hin_backend(),
	     H.cfg.p1_policy == INPUT_P1_BUILTIN ? "built-in" : H.cfg.p1_policy == INPUT_P1_EXTERNAL ? "external" : "auto",
	     H.cfg.cz_buttons > 0 ? "fitted" : "no");

	{
		char bd[320];

		board_describe(board_get(), bd, sizeof(bd));
		hlog(HLOG_INFO, "%s", bd);
	}
	hlog(HLOG_INFO, "lcd refresh: %s", H.cfg.lcd_refresh == 60 ? "60 Hz (25.2 MHz user mode)" :
	     "the panel's own mode (78.6 Hz)");
	hlog(HLOG_INFO, "scaling: %s%s, cpu profile: %s", H.scale == DISPLAY_SCALE_INTEGER ? "integer" :
	     H.scale == DISPLAY_SCALE_STRETCH ? "stretch" : "aspect", H.cfg.scale >= 0 ? " (--scale)" : "",
	     H.cpu_profile);
	if (!cfg->headless) {
		struct display_config dc;

		display_config_defaults(&dc);
		if (cfg->hdmi_width > 0 && cfg->hdmi_height > 0) {
			dc.hdmi_width = cfg->hdmi_width;
			dc.hdmi_height = cfg->hdmi_height;
		}
		dc.scale = (enum display_scale_mode)H.scale;
		dc.lcd_refresh_hz = H.cfg.lcd_refresh > 0 ? H.cfg.lcd_refresh : 0;
		board_apply_display(board_get(), &dc);
		dc.on_output = on_output;
		dc.on_audio = on_audio;
		dc.log = display_log;
		dc.log_level = cfg->log_level >= HLOG_DEBUG ? DISPLAY_LOG_DEBUG : DISPLAY_LOG_INFO;
		if (display_init(&dc) < 0) {
			ret = fail(HOST_EXIT_FAIL, N_("Cannot open the display"));
			goto out_input;
		}
		H.display_ok = true;
	}

	core_set_callbacks();
	H.core.init();
	core_inited = true;
	{
		/* host-level option: Mesa's GL worker thread (the second A7 core
		 * takes the driver work). Read before the EGL context exists. */
		const char *gt = opts_value("rsos-glthread");

		if (gt && (!strcasecmp(gt, "true") || !strcasecmp(gt, "on") || !strcasecmp(gt, "enabled"))) {
			setenv("mesa_glthread", "true", 1);
			hlog(HLOG_INFO, "rsos-glthread: mesa_glthread=true");
		}
	}
	if (content_load(&gi, err, sizeof(err)) < 0) {
		ret = fail_text(HOST_EXIT_ERROR, err);
		goto out_deinit;
	}
	if (!H.content_path[0] && !H.support_no_game) {
		ret = fail(HOST_EXIT_ERROR, N_("This core needs a game file"));
		goto out_deinit;
	}
	{
		int64_t t0 = hnow_ms();

		if (!H.core.load_game(H.content_path[0] ? &gi : NULL)) {
			ret = fail(HOST_EXIT_ERROR, N_("The emulator could not load this game"));
			goto out_deinit;
		}
		hlog(HLOG_INFO, "retro_load_game: %lld ms", (long long)(hnow_ms() - t0));
	}
	H.game_loaded = true;
	H.perf_log = H.hw_requested || !strcasecmp(H.system, "n64");
	H.core.get_system_av_info(&H.av);
	hlog(HLOG_INFO, "AV: %ux%u (max %ux%u, aspect %.3f), %.4f fps, %.1f Hz", H.av.geometry.base_width,
	     H.av.geometry.base_height, H.av.geometry.max_width, H.av.geometry.max_height,
	     H.av.geometry.aspect_ratio, H.av.timing.fps, H.av.timing.sample_rate);
	if (H.hw_requested) {
		unsigned mw = H.av.geometry.max_width, mh = H.av.geometry.max_height;

		if (hwr_init(mw ? mw : 640, mh ? mh : 480, H.display_ok ? display_get_drm_fd() : -1,
			     err, sizeof(err)) < 0) {
			ret = fail_text(HOST_EXIT_ERROR, err);
			goto out_unload;
		}
		if (H.hw.context_reset)
			H.hw.context_reset();
	}
	if (H.perf_log) {
		static const char *const always[] = {
			"gfxplugin", "screensize", "cpucore", "rspplugin", "frameskip", "CountPerOp", "virefresh",
			"rdp-plugin", "43screensize", "ThreadedRenderer", "EnableFBEmulation", "rsos-glthread", NULL,
		};

		opts_log_effective(always);
	}
	if (H.bench_step) {
		H.autosave = 0;
		audio_wanted = false;
	}
	host_video_setup();
	host_audio_setup();
	host_reevaluate_pacing();
	sram_load();
	update_ports(true);
	{
		const char *remap = hin_load_remap(H.system, H.game);

		if (remap)
			hlog(HLOG_INFO, "remap: %s", remap);
	}
	if (H.bench_step) {
		bench_step_load_state();
	} else if (cfg->load_state_file && hfile_exists(cfg->load_state_file)) {
		H.core.run();
		H.ain_n = 0;
		if (state_load_from(cfg->load_state_file) == 0)
			/* TRANSLATORS: toast: the game continues from the resume state */
			host_toast("%s", _("Resumed"));
	} else if (cfg->load_slot >= -1 && state_exists(cfg->load_slot, NULL)) {
		/* Resume: the state needs one frame first for some cores. */
		H.core.run();
		H.ain_n = 0;
		if (state_load(cfg->load_slot) == 0)
			/* TRANSLATORS: toast: the game continues from the resume state */
			host_toast("%s", _("Resumed"));
	}
	perf_snap(&PW[2].a, 0);
	if (cfg->governor)
		sys_governor_performance(true);
	if (!cfg->headless)
		log_cpufreq();
	pthread_create(&wd, NULL, watchdog, NULL);
	host_status("running", H.game);
	pt_start(&H.pt, hnow_ms());
	H.pt_next_report = hnow_ms() + (int64_t)(cfg->playtime_report_s > 0 ? cfg->playtime_report_s : 300) * 1000;
	hlog(HLOG_INFO, "fast-forward: Select+R2, x%d%s", H.ff_cap, H.rumble ? "" : "; vibration off");

	main_loop();

	/* ---- shutdown: saves first ---- */
	H.busy_ok = true;
	ret = H.exit_code;
	host_play_pause(true);
	playtime_report();
	hlog(HLOG_INFO, "play time this session: %lld s", (long long)(pt_total_ms(&H.pt, hnow_ms()) / 1000));
	if (H.poweroff)
		show_poweroff_frame();
	if (sram_flush(true) < 0)
		hlog(HLOG_ERROR, "final SRAM flush failed");
	if (!H.bench_step)
		opts_autosave();
	if (H.perf_log) {
		struct perf_counters b;
		struct perf_report r;
		char line[768];

		perf_snap(&b, PW[2].max);
		perf_diff(&PW[2].a, &b, core_fps(), &r);
		perf_format_log(&r, line, sizeof(line));
		hlog(HLOG_INFO, "perf session: %s", line);
	}
	if (H.hw_requested) {
		struct glprobe_stats g;

		glprobe_get(&g);
		if (!glprobe_seen())
			hlog(HLOG_WARN, "gl probe: no call of the core went through the probe (not exported?)");
		hlog(HLOG_INFO, "gl probe: %llu shader compiles (%.0f ms), %llu links (%.0f ms), %llu draws "
		     "(%llu over %d ms: %.0f ms), %llu texture uploads (%.0f ms)", (unsigned long long)g.compiles,
		     g.compile_us / 1000.0, (unsigned long long)g.links, g.link_us / 1000.0, (unsigned long long)g.draws,
		     (unsigned long long)g.slow_draws, GLPROBE_SLOW_DRAW_US / 1000, g.slow_draw_us / 1000.0,
		     (unsigned long long)g.tex, g.tex_us / 1000.0);
	}
	if (H.perf_log || H.stalls)
		hlog(HLOG_INFO, "stalls: %llu frames over %.0f ms (%.0f ms in all)", (unsigned long long)H.stalls,
		     PERF_STALL_MS, H.stall_ms);
	if (H.hw_requested && H.stalls)
		hlog(HLOG_INFO, "stalls with shader work (compile, link or slow draw): %llu of %llu",
		     (unsigned long long)H.stalls_gl, (unsigned long long)H.stalls);
	/* the game switcher always saves the game it leaves (it resumes there) */
	if ((H.poweroff || H.autosave || H.switch_to >= 0) && H.info.savestates && !H.bench_step)
		save_auto_state();
	else if (!H.bench_step)
		hlog(HLOG_INFO, "auto state not written (%s)", !H.info.savestates ? "the core has no save states" :
		     "Settings > Games > Auto-save on exit is off");
	if (H.switch_to >= 0 && !H.poweroff) {
		char line[16];

		snprintf(line, sizeof(line), "%d", H.switch_to);
		host_status("switch", line);
		ret = HOST_EXIT_SWITCH;
	}
	if (cfg->switcher_shot)
		hlog(host_switcher_screenshot(cfg->switcher_shot) ? HLOG_ERROR : HLOG_INFO, "switcher screenshot -> %s",
		     cfg->switcher_shot);
	if (cfg->test_states && ret == HOST_EXIT_OK)
		ret = state_test();
	if (cfg->dump_png)
		dump_png(cfg->dump_png);
	if (cfg->menu_shot)
		hlog(host_menu_screenshot(cfg->menu_shot) ? HLOG_ERROR : HLOG_INFO, "menu screenshot -> %s", cfg->menu_shot);
	if (H.poweroff) {
		ret = HOST_EXIT_POWEROFF;
		host_status("poweroff", "");
	}
	hlog(HLOG_INFO, "%llu frames, %llu presented, %llu skipped, %llu dupes", (unsigned long long)H.frame,
	     (unsigned long long)H.frames_presented, (unsigned long long)H.frames_skipped,
	     (unsigned long long)H.frames_duped);
	if (H.poweroff && cfg->status_fd >= 0) {
		/*
		 * Powering off under the UI (docs/power.md, "Shutdown"): SRAM, RTC
		 * and the auto state are on the card (fsync'ed); the core still
		 * writes its own files at unload (nvram, a second memory card) and
		 * the save worker finishes a queued write. The rest of the teardown
		 * (audio drain, GL and display release, the display restore) is
		 * left to the kernel: the unit powers off right after us.
		 */
		/* context_destroy before unload_game, as RetroArch (review F-M13) */
		if (H.hw_requested && H.hw.context_destroy && hwr_active())
			H.hw.context_destroy();
		H.core.unload_game();
		if (core_inited)
			H.core.deinit();
		saves_shutdown();
		hlog(HLOG_INFO, "power-off: saves on the card, exiting without the teardown");
		fflush(stderr);
		_exit(HOST_EXIT_POWEROFF);
	}
	audio_close();

out_unload:
	/* RetroArch's core_unload_game() order, which the GL cores are written
	 * for: context_destroy (context still current), then unload_game
	 * (review F-M13: the other way round could use freed GL wrappers) */
	if (H.hw_requested) {
		if (H.hw.context_destroy && hwr_active())
			H.hw.context_destroy();
	}
	H.core.unload_game();
out_deinit:
	if (core_inited)
		H.core.deinit();
	audio_close();
	if (H.display_ok)
		display_shutdown(); /* releases the GBM BOs it still holds */
	H.display_ok = false;
	if (H.hw_requested)
		hwr_deinit();      /* after display_shutdown (display-design 8.1) */
	audio_wanted = false;
out_input:
	hin_close();
	saves_shutdown();
out_opts:
	opts_free();
out_core:
	content_cleanup();
	core_close();
out_info:
	if (cfg->governor && !cfg->headless)
		sys_governor_performance(false);
	coreinfo_free(&H.info);
	resampler_free(H.rs);
	free(H.ain);
	free(H.aout);
	free(H.shadow);
	free(H.headless_frame);
	free(BOV.spx);
	free(B.ft);
	if (ret == HOST_EXIT_POWEROFF && cfg->status_fd < 0 && cfg->poweroff_cmd)
		sys_power_off(cfg->poweroff_cmd);
	return ret;
}

bool host_check_game(const char *core_path, const char *rom_path, const char *system,
		     char *msg, size_t n)
{
	struct core_info ci;
	char id[64], dir[PATH_MAX], sys[64];
	bool ok;

	coreinfo_id_from_path(core_path, id, sizeof(id));
	if (system && *system) {
		hstrlcpy(sys, system, sizeof(sys));
	} else {
		hpath_dir(rom_path ? rom_path : "", dir, sizeof(dir));
		hstrlcpy(sys, hpath_base(dir), sizeof(sys));
	}
	coreinfo_load(&ci, NULL, id);
	ok = bios_check(&ci, sys, rom_path, "/data/bios", msg, n, NULL, 0);
	coreinfo_free(&ci);
	return ok;
}

int host_pick_core(const char *system, const char *rom_path, char *core_path, size_t n)
{
	char id[64];
	struct core_info ci;
	int r = coreinfo_pick(NULL, "/data/rsos/cores.ini", system, rom_path, id, sizeof(id));

	if (r)
		return r;
	if (coreinfo_load(&ci, NULL, id) == 0 && ci.library[0])
		hstrlcpy(core_path, ci.library, n);
	else
		snprintf(core_path, n, "/usr/lib/libretro/%s_libretro.so", id);
	coreinfo_free(&ci);
	return 0;
}
