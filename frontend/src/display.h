/*
 * display.h - RetroStoneOS display layer (atomic KMS, libdrm only).
 *
 * One output at a time: the internal LCD or HDMI, switched live on hotplug
 * (switch, never mirror). The game image is a small buffer at the core's
 * native resolution; the display engine scales it to the screen through
 * the plane SRC_* / CRTC_* properties (sun4i backend x2/x4 integer scaler
 * or DEFE frontend scaler), with a software fallback (CPU integer scale or
 * plain unscaled blit into a screen-sized buffer) when the atomic check
 * rejects the scaled plane.
 *
 * Single-threaded. Typical loop:
 *
 *   struct display_config cfg;
 *   display_config_defaults(&cfg);
 *   if (display_init(&cfg) < 0) ...
 *   const struct display_surface *s =
 *       display_set_game_surface(320, 240, DRM_FORMAT_XRGB8888, 0);
 *   for (;;) {
 *       int i = display_begin_frame(-1);      // waits for a free buffer
 *       if (i < 0) continue;
 *       draw(s->buffers[i].pixels, s->buffers[i].stride);
 *       display_present();                    // vsync-locked page flip
 *   }
 *
 * or, from a libretro video_refresh callback (surface created at the
 * core's max_width x max_height, in its pixel format):
 *
 *   display_present_frame(data, width, height, pitch);
 *
 * Event-driven programs poll display_get_fd() (an epoll fd grouping the
 * DRM fd, the uevent socket and the debounce timer) and call
 * display_handle_events() when it is readable.
 * See docs/display-design.md.
 */
#ifndef RSOS_DISPLAY_H
#define RSOS_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_MAX_BUFFERS 3

enum display_output_type {
	DISPLAY_OUTPUT_NONE = 0,
	DISPLAY_OUTPUT_LCD,   /* internal panel (DPI/Unknown/LVDS/DSI/Virtual) */
	DISPLAY_OUTPUT_HDMI,  /* external (HDMI/DVI/DP/VGA) */
};

enum display_internal_mode {
	DISPLAY_INTERNAL_AUTO = 0,
	DISPLAY_INTERNAL_LIST,
	DISPLAY_INTERNAL_NONE,
};

enum display_scale_mode {
	DISPLAY_SCALE_ASPECT = 0, /* largest rect with the image aspect, letterboxed */
	DISPLAY_SCALE_INTEGER,    /* largest integer factor, square pixels, centered */
	DISPLAY_SCALE_STRETCH,    /* fill the screen */
};

enum display_log_level {
	DISPLAY_LOG_ERROR = 0,
	DISPLAY_LOG_WARN,
	DISPLAY_LOG_INFO,
	DISPLAY_LOG_DEBUG,
};

struct display_rect {
	int x, y, w, h;
};

struct display_output_info {
	enum display_output_type type;
	char name[32];          /* connector name, e.g. "HDMI-A-1" */
	char monitor[16];       /* EDID monitor name, "" if unknown */
	bool has_edid;
	bool builtin_mode;      /* mode was synthesized (no EDID / no modes) */
	int width, height;      /* active mode */
	int refresh_mhz;        /* exact refresh rate in mHz (60000 = 60 Hz) */
	int measured_mhz;       /* measured from the page-flip events ~2 s after the
				   modeset (0 until then), display-design.md §3.1 */
	char mode_name[32];
	uint32_t connector_id, crtc_id, plane_id;
	int crtc_index;
};

/* Why a callback fired. */
enum display_event_reason {
	DISPLAY_EVENT_INIT = 0,   /* first output chosen by display_init() */
	DISPLAY_EVENT_HOTPLUG,    /* switched after a hotplug uevent */
	DISPLAY_EVENT_REPROBE,    /* same output, mode changed (late EDID, ...) */
	DISPLAY_EVENT_FALLBACK,   /* external modeset failed, back on the LCD */
};

/* Timestamps (CLOCK_MONOTONIC, ms) to measure switch latency. */
struct display_switch_timing {
	int64_t t_uevent;   /* first uevent of the burst (0 at init) */
	int64_t t_probe;    /* debounce expired, connectors probed */
	int64_t t_commit;   /* modeset commit returned */
};

typedef void (*display_output_cb)(const struct display_output_info *now,
				  const struct display_output_info *before,
				  enum display_event_reason why,
				  const struct display_switch_timing *timing,
				  void *user);

/*
 * Audio hook, so the ALSA stream follows the picture. Two phases:
 *  - DISPLAY_AUDIO_RELEASE is called *before* the modeset, with the output
 *    that is about to go away (or be re-timed): close its PCM now. The
 *    sun4i HDMI audio device ("sun4i-hdmi" card) must be closed before
 *    the HDMI encoder is switched off.
 *  - DISPLAY_AUDIO_ACQUIRE is called *after* the modeset (and once at
 *    init), with the output now showing the picture: open its PCM (HDMI
 *    card, or the internal codec for the LCD). Also called when a switch
 *    failed and the old output stays, so audio always resumes.
 */
enum display_audio_phase {
	DISPLAY_AUDIO_RELEASE = 0,
	DISPLAY_AUDIO_ACQUIRE,
};

typedef void (*display_audio_cb)(enum display_audio_phase phase,
				 const struct display_output_info *out,
				 void *user);

typedef void (*display_log_cb)(enum display_log_level lvl, const char *msg,
			       void *user);

struct display_config {
	const char *device;      /* "/dev/dri/cardN", NULL = first usable card */

	/*
	 * HDMI mode policy. Default 1280x720@60 (1080p scanout costs twice
	 * the DDR bandwidth). 640x480@60 is the "native 4:3" option: CEA VIC 1,
	 * which every sink must accept, same geometry as the LCD, so 320x240
	 * cores get the sharp backend 2x scaling and the TV upscales.
	 */
	int hdmi_width, hdmi_height;  /* preferred HDMI mode, default 1280x720 */
	int hdmi_refresh;             /* Hz, default 60 */
	bool hdmi_builtin_mode;       /* no EDID: use the CEA timing of hdmi_width x
					 hdmi_height (720p60 or 480p60), default on */
	int force_width, force_height; /* --mode WxH: forced on any output that has it */

	/*
	 * Internal LCD refresh: 0 = the panel's own mode (the DT timing: 33 MHz,
	 * 78.571 Hz on the RetroStone2); 30..120 = the same timings with the
	 * pixel clock set for that rate (60 -> 25.2 MHz), committed as a user
	 * mode. display-design.md §3.1. Settings key lcd_refresh (60|78).
	 */
	int lcd_refresh_hz;

	/* -1 = auto (keep the CRTC already in use when the encoder allows it). */
	int hdmi_crtc_index;

	/*
	 * Which connectors are the built-in screen (board profile key
	 * internal_display, docs/porting.md). DISPLAY_INTERNAL_AUTO (default):
	 * every type that is not external (HDMI/DVI/DP/VGA/TV); _LIST: the
	 * DRM_MODE_CONNECTOR_* bits of internal_types; _NONE: no built-in
	 * screen, the other connectors are ignored. A board with no built-in
	 * screen and no display connected lights the first external connector
	 * anyway (the CEA mode of the HDMI policy), so the menu starts and moves
	 * to the TV as soon as one is plugged in.
	 */
	int internal_mode;           /* enum display_internal_mode */
	uint32_t internal_types;
	/* Log the A20 TCON0 pixel clock model when the panel is retimed (board
	 * quirk sun4i-tcon0-clock). Default on. */
	bool a20_clock_log;

	/* Scaling. */
	enum display_scale_mode scale;
	bool no_hw_scale;        /* force the software fallback path */
	int soft_scale_max;      /* fallback path: max CPU integer factor (default 2, 1 = unscaled) */
	int buffers;             /* game plane buffers: 2 (default) or 3 */

	/* Hotplug. */
	int debounce_ms;         /* quiet time after the last uevent, default 250 */
	int edid_retry_ms;       /* re-probe delay when HDMI has no EDID, default 1000 */
	int edid_retries;        /* default 2 */

	display_output_cb on_output;
	display_audio_cb on_audio;
	display_log_cb log;      /* NULL = stderr with timestamps */
	enum display_log_level log_level;
	void *user;
};

struct display_buffer {
	void *pixels;
	int stride;              /* bytes per line */
};

struct display_surface {
	int width, height;       /* current frame size (draw at the top-left) */
	int max_width, max_height; /* capacity given to display_set_game_surface() */
	uint32_t format;         /* DRM fourcc of pixels (what the caller asked) */
	int count;               /* entries in buffers[] */
	struct display_buffer buffers[DISPLAY_MAX_BUFFERS];
	bool hw_scaled;          /* true: plane scaler; false: software blit */
	bool direct;             /* true: buffers[] are the scanout buffers */
	uint32_t plane_format;   /* format scanned out (may differ: converted) */
	int soft_scale;          /* fallback path: CPU integer factor */
	struct display_rect dst; /* where the image lands on the screen */
};

/* Surface flags. */
#define DISPLAY_SURFACE_CACHED 0x1u /* always draw into cached RAM (shadow) */

void display_config_defaults(struct display_config *cfg);

/*
 * Opens the DRM device, enables atomic + universal planes, becomes DRM
 * master, opens the uevent socket, probes the connectors and lights up
 * HDMI if it is connected (else the LCD). Returns 0 or -errno.
 */
int display_init(const struct display_config *cfg);
void display_shutdown(void);

/* Pollable fd (POLLIN): DRM events, uevents and the debounce timer. */
int display_get_fd(void);
/* The raw DRM fd, for tools. */
int display_get_drm_fd(void);

/*
 * Non-blocking: handles page-flip completions, uevents, debounce expiry
 * and output switches. Returns the number of events handled or -errno.
 */
int display_handle_events(void);

/* Blocks up to timeout_ms (-1 = forever) for events, then handles them.
 * Returns the number handled, or -errno (-EINTR when a signal arrived). */
int display_wait_events(int timeout_ms);

/*
 * (Re)creates the game surface with room for width x height pixels (the
 * largest frame the core can produce; PSX: 640x480). The frame size starts
 * at width x height; change it per frame with display_set_frame_size().
 *
 * format: DRM_FORMAT_RGB565 (libretro RGB565), DRM_FORMAT_XRGB1555
 * (libretro 0RGB1555, the libretro default), DRM_FORMAT_XRGB8888 or
 * DRM_FORMAT_ARGB8888 (alpha ignored). The display picks, with TEST_ONLY
 * commits, the first strategy the kernel accepts:
 *   1. native format on the plane, scaled by the hardware (zero-copy):
 *      XRGB8888 any ratio (frontend), RGB565 x1/x2/x4 only (backend
 *      integer scaler, kernel patch 0003);
 *   2. XRGB8888 plane scaled by the frontend, converted during the copy;
 *   3. software: CPU integer scale (<= soft_scale_max) into a
 *      screen-sized buffer, centered.
 * Results are cached per (CRTC, mode size, frame size, format).
 *
 * Returns the surface description, NULL on error. The pointer stays valid
 * until display_shutdown(); its contents change when the strategy changes
 * (hotplug may move from hardware scaling to the fallback), so read
 * buffers[] again after each display_begin_frame().
 */
const struct display_surface *display_set_game_surface(int width, int height,
						       uint32_t format,
						       unsigned int flags);
const struct display_surface *display_get_surface(void);

/*
 * Changes the frame size inside the surface capacity (PSX resolution
 * switches). No reallocation and no modeset: the plane SRC_W/SRC_H and
 * CRTC_* rectangle travel with the next page flip, together with the first
 * frame of the new size. Only if the current strategy cannot handle the
 * new size (e.g. RGB565 no longer an exact x2) is the strategy re-picked.
 */
int display_set_frame_size(int width, int height);

/* Scale mode and image aspect ratio (0 = square pixels, w/h). */
int display_set_scaling(enum display_scale_mode mode, double aspect);

/*
 * Returns the index in surface->buffers[] to draw the next frame into,
 * waiting up to timeout_ms for the display to release one (this is where
 * a game loop gets paced to vsync). -ETIMEDOUT on timeout, -EINTR on a signal, other -errno on error.
 */
int display_begin_frame(int timeout_ms);

/*
 * Queues the frame drawn since display_begin_frame() for the next vblank
 * (non-blocking atomic page flip, no tearing). Returns 0, or -EAGAIN when
 * the frame was dropped (surface changed meanwhile).
 */
int display_present(void);

/* begin_frame + convert/copy src into the buffer + present. */
int display_present_copy(const void *src, int src_stride);

/*
 * libretro video_refresh(data, width, height, pitch) in one call: adopts
 * the frame size if it changed, then copies/converts and presents.
 * The copy the host needs anyway is where RGB565/0RGB1555 get converted
 * to XRGB8888 (NEON) when the plane cannot take the native format.
 */
int display_present_frame(const void *src, int width, int height, int src_stride);

/* True while a page flip is queued in the kernel. */
bool display_flip_pending(void);

/*
 * Zero-copy presentation of an external framebuffer: a GBM BO (EGL/GLES2
 * rendering) or any buffer the caller wrapped with drmModeAddFB2() on
 * display_get_drm_fd(). It goes on the game plane, scaled and letterboxed
 * like the game surface (scale mode, aspect ratio from
 * display_set_scaling()), and is flipped at the next vblank.
 *
 * Constraints (sun4i): format XRGB8888 for any ratio (frontend scaler);
 * other RGB formats only for x1/x2/x4 (backend, kernel patch 0003). No
 * rotation or Y flip on the plane: render bottom-left-origin GL images
 * upside down yourself. The FB must stay valid until release() is called.
 */
struct display_fb {
	uint32_t fb_id;          /* from drmModeAddFB2() on display_get_drm_fd() */
	uint32_t format;         /* DRM fourcc of the FB, e.g. DRM_FORMAT_XRGB8888 */
	int width, height;       /* FB size in pixels */
	struct display_rect src; /* the frame inside the FB (whole pixels) */
	/*
	 * Called once the FB left the screen (the flip that replaced it has
	 * completed, or at display_shutdown()). Presenting the same fb_id
	 * again while it is on screen does not produce an extra release.
	 * Never called for a present that returned an error.
	 */
	void (*release)(uint32_t fb_id, void *user);
	void *user;
};

/*
 * Returns 0 when the flip is queued. If a flip is still in flight it
 * first waits up to timeout_ms (0 = don't wait, -1 = forever), servicing
 * events (this is the vsync pacing, as in display_begin_frame()).
 * Errors: -EBUSY (timeout_ms == 0 and a flip is pending), -ETIMEDOUT,
 * -EINTR, -EAGAIN (display inactive or suspended: frame not shown),
 * -EINVAL (bad arguments, or the plane refuses this FB/geometry in a
 * TEST_ONLY commit: fall back to a readback + display_present_frame()).
 * Output switches keep working: the FB on screen is re-committed, scaled
 * for the new output, right after the modeset.
 */
int display_present_fb(const struct display_fb *fb, int timeout_ms);

/*
 * Fast DRM master hand-off to a child process (the game):
 *   display_suspend(): waits for the in-flight flip, drops DRM master and
 *     stops touching the hardware. The fd, the buffers and all state stay.
 *     The child then calls display_init() and becomes master.
 *   display_resume(): after the child has exited (it must have closed its
 *     DRM fd), takes master back, re-evaluates the outputs from the
 *     kernel's connector state (the child's probes keep it fresh, so no
 *     EDID read unless needed; a hotplug during the game switches output
 *     here, with the usual callbacks) and re-commits the full state.
 * While suspended, every drawing call returns -EAGAIN and
 * display_handle_events() only drains the uevent socket.
 * Returns 0 or -errno (-EBUSY from resume: master still held elsewhere).
 */
int display_suspend(void);
int display_resume(void);

/*
 * Screen off/on for sleep and idle, keeping the configuration: the CRTC
 * ACTIVE property in one blocking atomic commit. Off: drm_panel disables
 * the LCD panel and its PWM backlight (or the HDMI encoder stops: close
 * the HDMI PCM first). On: full re-commit with the newest frame. While
 * off, presents are kept (newest frame wins, nothing is flipped) and a
 * hotplug is only re-evaluated when the screen comes back on.
 */
int display_set_active(bool active);
bool display_is_active(void);

/*
 * A small ARGB8888 picture (the in-game battery indicator) on its own KMS
 * overlay plane, above the game plane, unscaled (1 pixel = 1 screen pixel)
 * and kept fully on screen in a corner. It costs nothing per frame: it is
 * committed with the next page flip (or by its own commit when no frame is
 * flowing) only when the content or the placement changes, and it works
 * the same for every presentation path, including display_present_fb().
 * It is re-placed on every output switch (LCD <-> HDMI) and re-applied
 * after display_resume() and display_set_active(true).
 *
 * sun4i limits (docs/kernel-patches.md, "Display scaling on A20"): one
 * plane with alpha, an opaque bottom plane, one frontend-scaled plane per
 * CRTC (the game's). The overlay satisfies all three; if a TEST_ONLY
 * commit still refuses it, that is logged once and the overlay is simply
 * not shown: the game display is never affected.
 *
 * argb: w x h pixels, 0xAARRGGBB, rows of w pixels (copied; the caller
 * may free it). Returns 0, or -EINVAL / -ENOMEM.
 */
enum display_corner {
	DISPLAY_CORNER_TOP_RIGHT = 0,
	DISPLAY_CORNER_TOP_LEFT,
	DISPLAY_CORNER_BOTTOM_RIGHT,
	DISPLAY_CORNER_BOTTOM_LEFT,
};
int display_set_overlay(const uint32_t *argb, int w, int h, enum display_corner corner, int margin);
void display_hide_overlay(void);
/* True while the overlay plane shows it (committed and not refused). */
bool display_overlay_visible(void);
/* Placement: the rect of a w x h overlay on a W x H screen, `margin`
 * pixels from the corner, clamped inside the screen. False if it does not
 * fit at all. */
bool display_overlay_rect(int W, int H, int w, int h, enum display_corner corner, int margin,
			  struct display_rect *out);

/*
 * LCD refresh, live (display-design.md §3.1): hz 0 = the panel's own mode,
 * else the retimed mode (60). On the LCD it modesets now (on_audio RELEASE /
 * ACQUIRE and on_output with DISPLAY_EVENT_REPROBE, like a re-probe); on HDMI
 * it applies the next time the LCD is chosen; while suspended or off, at
 * resume / screen on. Returns 0, or -errno when the kernel refused the mode
 * (the previous mode stays on screen).
 */
int display_set_lcd_refresh(int hz);
int display_get_lcd_refresh(void);
/* settings.ini lcd_refresh value -> hz for lcd_refresh_hz: "60" -> 60,
 * anything else ("78", missing) -> 0. */
int display_parse_lcd_refresh(const char *v);

const struct display_output_info *display_output(void);

/* Stats. */
struct display_stats {
	uint64_t flips;          /* completed page flips */
	uint64_t dropped;        /* frames replaced before reaching the screen */
	uint64_t switches;       /* output switches */
	int64_t last_flip_us;    /* timestamp of the last flip event (CLOCK_MONOTONIC) */
};
void display_get_stats(struct display_stats *st);

/* Monotonic clock in milliseconds (for callers' logs). */
int64_t display_now_ms(void);

#endif
