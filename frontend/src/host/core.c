/*
 * core.c - loading a libretro core (dlopen) and everything the core calls
 * back into: the environment callback, video, audio and input callbacks.
 * The coverage table is in docs/host-design.md; unknown commands return
 * false and are logged once.
 */
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../audio/audio.h"
#include "content.h"
#include "host_input.h"
#include "host_internal.h"
#include "hutil.h"
#include "../i18n/i18n.h"
#include "hwrender.h"
#include "options.h"

/* ------------------------------------------------------------- dlopen */

#define SYM(field, name)                                                  \
	do {                                                              \
		*(void **)&H.core.field = dlsym(H.core.handle, name);     \
		if (!H.core.field) {                                      \
			hlog(HLOG_ERROR, "core: missing symbol %s", name); \
			core_close();                                     \
			return -ENOENT;                                   \
		}                                                         \
	} while (0)

int core_open(const char *path)
{
	memset(&H.core, 0, sizeof(H.core));
	H.core.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!H.core.handle) {
		hlog(HLOG_ERROR, "dlopen %s: %s", path, dlerror());
		return -ENOENT;
	}
	SYM(init, "retro_init");
	SYM(deinit, "retro_deinit");
	SYM(api_version, "retro_api_version");
	SYM(get_system_info, "retro_get_system_info");
	SYM(get_system_av_info, "retro_get_system_av_info");
	SYM(set_environment, "retro_set_environment");
	SYM(set_video_refresh, "retro_set_video_refresh");
	SYM(set_audio_sample, "retro_set_audio_sample");
	SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
	SYM(set_input_poll, "retro_set_input_poll");
	SYM(set_input_state, "retro_set_input_state");
	SYM(set_controller_port_device, "retro_set_controller_port_device");
	SYM(reset, "retro_reset");
	SYM(run, "retro_run");
	SYM(serialize_size, "retro_serialize_size");
	SYM(serialize, "retro_serialize");
	SYM(unserialize, "retro_unserialize");
	SYM(load_game, "retro_load_game");
	SYM(unload_game, "retro_unload_game");
	SYM(get_memory_data, "retro_get_memory_data");
	SYM(get_memory_size, "retro_get_memory_size");
	if (H.core.api_version() != RETRO_API_VERSION)
		hlog(HLOG_WARN, "core API version %u (host %u)", H.core.api_version(), RETRO_API_VERSION);
	return 0;
}

void core_close(void)
{
	/* The game process exits right after: no dlclose() (several cores
	 * leave threads or atexit handlers behind; the fork model is what
	 * frees them). */
	memset(&H.core, 0, sizeof(H.core));
}

/* ------------------------------------------------------------- logging */

/*
 * The core's log goes to game.log, in RAM (/run/rsos): a core that logs
 * every frame must not fill it. Token bucket: CORE_LOG_RATE lines a second,
 * bursts of CORE_LOG_BURST (a core's start-up), then a "suppressed" summary
 * with the next line that passes (and at exit, core_log_flush()). Cores log
 * from their own threads too (GLideN64): one lock.
 */
#define CORE_LOG_RATE 20.0
#define CORE_LOG_BURST 200.0

static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static struct hrate log_rate;

static void log_suppressed(unsigned long n)
{
	if (n)
		hlog(HLOG_WARN, "[%s] %lu core log lines suppressed (over %.0f a second)", H.core_id, n,
		     CORE_LOG_RATE);
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	static const enum hlog_level map[] = { HLOG_DEBUG, HLOG_INFO, HLOG_WARN, HLOG_ERROR };
	enum hlog_level lvl = level <= RETRO_LOG_ERROR ? map[level] : HLOG_INFO;
	unsigned long dropped = 0;
	char buf[1024];
	va_list ap;
	bool pass;

	if (!hlog_enabled(lvl))
		return;
	pthread_mutex_lock(&log_mu);
	pass = hrate_take(&log_rate, hnow_ms(), CORE_LOG_RATE, CORE_LOG_BURST, &dropped);
	pthread_mutex_unlock(&log_mu);
	if (!pass)
		return;
	log_suppressed(dropped);
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	hlog(lvl, "[%s] %s", H.core_id, buf);
}

void core_log_flush(void)
{
	unsigned long n;

	pthread_mutex_lock(&log_mu);
	n = log_rate.dropped;
	log_rate.dropped = 0;
	pthread_mutex_unlock(&log_mu);
	log_suppressed(n);
}

/* ---------------------------------------------------------------- perf */

static retro_time_t perf_time_usec(void)
{
	return hnow_us();
}

static uint64_t perf_cpu_features(void)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	return RETRO_SIMD_NEON;
#elif defined(__SSE2__)
	return RETRO_SIMD_SSE | RETRO_SIMD_SSE2;
#else
	return 0;
#endif
}

static retro_perf_tick_t perf_counter(void)
{
	return (retro_perf_tick_t)hnow_us();
}

static retro_usec_t perf_usec_dummy(void)
{
	return 0;
}

static void perf_register(struct retro_perf_counter *c)
{
	c->registered = true;
}

static void perf_start(struct retro_perf_counter *c)
{
	c->start = perf_counter();
}

static void perf_stop(struct retro_perf_counter *c)
{
	c->total += perf_counter() - c->start;
	c->call_cnt++;
}

static void perf_log(void)
{
}

/* -------------------------------------------------------------- rumble */

static bool rumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
	return hin_rumble((int)port, (int)effect, strength);
}

/* --------------------------------------------------------------- disks */

static void set_disk_v0(const struct retro_disk_control_callback *cb)
{
	memset(&H.disk, 0, sizeof(H.disk));
	H.disk.set_eject_state = cb->set_eject_state;
	H.disk.get_eject_state = cb->get_eject_state;
	H.disk.get_image_index = cb->get_image_index;
	H.disk.set_image_index = cb->set_image_index;
	H.disk.get_num_images = cb->get_num_images;
	H.disk.replace_image_index = cb->replace_image_index;
	H.disk.add_image_index = cb->add_image_index;
	H.has_disk = true;
}

/* ------------------------------------------------ RetroArch private calls */

/* mupen64plus-next's threaded GLideN64 asks for these (values from its
 * mupen64plus-next_common.h, RETRO_ENVIRONMENT_RETROARCH_START_BLOCK). */
#ifndef RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB
#define RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB (3 | 0x800000)
#endif
#ifndef RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE
#define RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE (4 | 0x800000)
#endif

/* RetroArch wakes its audio/video threads here so the core's emulation
 * thread never blocks on them; our audio writes never block the core. */
static bool clear_thread_waits(unsigned clear, void *data)
{
	(void)clear;
	(void)data;
	return true;
}

/* ------------------------------------------------------ the environment */

static const char *env_name(unsigned cmd)
{
	static char buf[32];

	snprintf(buf, sizeof(buf), "%u%s", cmd & 0xffff,
		 cmd & RETRO_ENVIRONMENT_EXPERIMENTAL ? " (experimental)" :
		 cmd & RETRO_ENVIRONMENT_PRIVATE ? " (private)" : "");
	return buf;
}

bool core_environment(unsigned cmd, void *data)
{
	switch (cmd) {
	/* --- paths ------------------------------------------------------ */
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
		*(const char **)data = H.bios_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = H.save_dir;
		return true;
	case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
		*(const char **)data = H.cfg.core_path;
		return true;
	case RETRO_ENVIRONMENT_GET_USERNAME:
		*(const char **)data = "Player";
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		/* the menu's language (cores with translated OSD/options, or a
		 * game language setting, e.g. the NeoGeo/Saturn BIOS) */
		*(unsigned *)data = i18n_retro_language();
		hlog_once("GET_LANGUAGE", "core asked for the language: %u (%s)", i18n_retro_language(),
			  i18n_language());
		return true;

	/* --- logging, messages, perf ------------------------------------ */
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
		*(unsigned *)data = 1;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE: {
		const struct retro_message *m = data;
		double fps = H.av.timing.fps > 1 ? H.av.timing.fps : 60;

		if (m && m->msg)
			osd_toast(m->msg, (int)(m->frames * 1000 / fps) + 1);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
		const struct retro_message_ext *m = data;

		if (!m || !m->msg)
			return false;
		if (m->target == RETRO_MESSAGE_TARGET_LOG)
			hlog(HLOG_INFO, "[%s] %s", H.core_id, m->msg);
		else
			osd_toast(m->msg, m->duration ? (int)m->duration : 2000);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
		struct retro_perf_callback *p = data;

		p->get_time_usec = perf_time_usec;
		p->get_cpu_features = perf_cpu_features;
		p->get_perf_counter = perf_counter;
		p->perf_register = perf_register;
		p->perf_start = perf_start;
		p->perf_stop = perf_stop;
		p->perf_log = perf_log;
		(void)perf_usec_dummy;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
		hlog(HLOG_INFO, "core performance level: %u", *(const unsigned *)data);
		return true;

	/* --- video ------------------------------------------------------ */
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(const enum retro_pixel_format *)data;

		if (f != RETRO_PIXEL_FORMAT_0RGB1555 && f != RETRO_PIXEL_FORMAT_XRGB8888 &&
		    f != RETRO_PIXEL_FORMAT_RGB565)
			return false;
		H.pixfmt = f;
		H.drm_format = f == RETRO_PIXEL_FORMAT_RGB565 ? DRM_FORMAT_RGB565 :
			       f == RETRO_PIXEL_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_XRGB1555;
		hlog(HLOG_INFO, "pixel format %s", f == RETRO_PIXEL_FORMAT_RGB565 ? "RGB565" :
		     f == RETRO_PIXEL_FORMAT_XRGB8888 ? "XRGB8888" : "0RGB1555");
		if (H.game_loaded)
			host_video_setup();
		return true;
	}
	case RETRO_ENVIRONMENT_SET_GEOMETRY: {
		const struct retro_game_geometry *g = data;

		H.av.geometry.base_width = g->base_width;
		H.av.geometry.base_height = g->base_height;
		H.av.geometry.aspect_ratio = g->aspect_ratio;
		if (H.display_ok)
			display_set_scaling((enum display_scale_mode)H.scale, g->aspect_ratio > 0 ? g->aspect_ratio : 0);
		hlog(HLOG_DEBUG, "geometry %ux%u aspect %.3f", g->base_width, g->base_height, g->aspect_ratio);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
		const struct retro_system_av_info *av = data;

		H.av = *av;
		hlog(HLOG_INFO, "AV info: %ux%u (max %ux%u) %.4f fps %.1f Hz", av->geometry.base_width,
		     av->geometry.base_height, av->geometry.max_width, av->geometry.max_height,
		     av->timing.fps, av->timing.sample_rate);
		if (H.game_loaded) {
			host_video_setup();
			host_audio_setup();
			host_reevaluate_pacing();
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_OVERSCAN:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_ROTATION:
		/* The A20 planes cannot rotate: the core rotates itself. */
		hlog_once("rotation", "core asked for rotation %u: refused (core rotates in software)",
			  *(const unsigned *)data);
		return false;
	case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
		*(float *)data = (float)host_run_hz();
		return true;
	case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
		struct retro_throttle_state *t = data;

		t->mode = H.ff_speed > 1 ? RETRO_THROTTLE_FAST_FORWARD :
			  H.pace.mode == PACE_VSYNC ? RETRO_THROTTLE_VSYNC :
			  H.pace.mode == PACE_FREE ? RETRO_THROTTLE_UNBLOCKED : RETRO_THROTTLE_NONE;
		t->rate = (float)host_run_hz() * (H.ff_speed > 1 ? H.ff_speed : 1);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
		/* Scanout buffers are write-combined: cores that read back would
		 * crawl. They render into their own buffer instead. */
		return false;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (data)
			*(int *)data = (H.skip_video ? 0 : 1) | (H.skip_video && H.ff_speed > 1 ? 0 : 2);
		return true;
	case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
		*(bool *)data = H.ff_speed > 1;
		return true;

	/* --- HW rendering ----------------------------------------------- */
	case RETRO_ENVIRONMENT_SET_HW_RENDER: {
		struct retro_hw_render_callback *cb = data;

		if (!hwr_env_set(cb))
			return false;
		H.hw = *cb;
		H.hw_requested = true;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		*(unsigned *)data = hwr_preferred();
		return true;

	/* --- audio ------------------------------------------------------ */
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
		const struct retro_audio_buffer_status_callback *cb = data;

		H.audio_status_cb = cb ? cb->callback : NULL;
		hlog(HLOG_INFO, "core uses the audio buffer status callback (auto frameskip)");
		return true;
	}
	case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: {
		unsigned ms = data ? *(const unsigned *)data : 0;

		H.min_audio_latency_ms = ms;
		hlog(HLOG_INFO, "core asks for >= %u ms audio latency", ms);
		if (ms > (unsigned)audio_latency_ms() && audio_is_open())
			audio_set_latency((int)ms);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE:
		*(unsigned *)data = AUDIO_RATE;
		return true;
	case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
		const struct retro_frame_time_callback *ft = data;

		H.frame_time = *ft;
		H.frame_time_last_us = 0;
		return true;
	}

	/* --- options ---------------------------------------------------- */
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		return opts_env_get_variable(data);
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		return opts_env_set_variables(data);
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		return opts_env_get_update(data);
	case RETRO_ENVIRONMENT_SET_VARIABLE:
		return opts_env_set_variable(data);
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
		return opts_env_set_v1(data);
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
		return opts_env_set_v1_intl(data);
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		opts_env_set_v2(data);
		return true; /* true: we support categories */
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
		opts_env_set_v2_intl(data);
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
		return opts_env_set_display(data);
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
		const struct retro_core_options_update_display_callback *cb = data;

		opts_env_set_update_display_cb(cb ? cb->callback : NULL);
		return true;
	}

	/* --- input ------------------------------------------------------ */
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
		*(uint64_t *)data = (1u << RETRO_DEVICE_JOYPAD) | (1u << RETRO_DEVICE_ANALOG);
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
		*(unsigned *)data = HOST_MAX_PORTS;
		return true;
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
		const struct retro_input_descriptor *d = data;

		H.ndesc = 0;
		for (; d && d->description && H.ndesc < HOST_MAX_DESC; d++) {
			H.desc[H.ndesc].port = d->port;
			H.desc[H.ndesc].device = d->device;
			H.desc[H.ndesc].index = d->index;
			H.desc[H.ndesc].id = d->id;
			hstrlcpy(H.desc[H.ndesc].desc, d->description, sizeof(H.desc[0].desc));
			H.ndesc++;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
		const struct retro_controller_info *ci = data;

		for (int p = 0; ci && ci[p].types && p < HOST_MAX_PORTS; p++) {
			H.nctrl[p] = 0;
			for (unsigned t = 0; t < ci[p].num_types && H.nctrl[p] < 8; t++) {
				if (!ci[p].types[t].desc)
					continue;
				H.ctrl[p][H.nctrl[p]].id = ci[p].types[t].id;
				hstrlcpy(H.ctrl[p][H.nctrl[p]].desc, ci[p].types[t].desc, sizeof(H.ctrl[0][0].desc));
				H.nctrl[p]++;
			}
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
		((struct retro_rumble_interface *)data)->set_rumble_state = rumble;
		return true;

	/* --- content ---------------------------------------------------- */
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		H.support_no_game = *(const bool *)data;
		return true;
	case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
		return content_game_info_ext(data);
	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE: {
		const struct retro_system_content_info_override *o = data;

		if (!o)
			return true; /* "is it supported?" */
		H.noverrides = 0;
		for (; o->extensions && H.noverrides < 16; o++) {
			H.overrides[H.noverrides] = *o;
			H.overrides[H.noverrides].extensions = strdup(o->extensions);
			H.noverrides++;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
		set_disk_v0(data);
		return true;
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
		H.disk = *(const struct retro_disk_control_ext_callback *)data;
		H.has_disk = true;
		return true;
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
		*(unsigned *)data = 1;
		return true;

	/* --- saves / states --------------------------------------------- */
	case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
		if (data)
			*(int *)data = RETRO_SAVESTATE_CONTEXT_NORMAL;
		return true;
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: {
		/* The frontend clears the bits it does not handle (libretro.h;
		 * review F-L17). Handled: INCOMPLETE (no state before frame 1).
		 * The others describe limits we accept as they are: the state is
		 * only used on this device, with the same core build. */
		const uint64_t known = RETRO_SERIALIZATION_QUIRK_INCOMPLETE |
				       RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE |
				       RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE |
				       RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE |
				       RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION |
				       RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT |
				       RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT;

		H.quirks = *(const uint64_t *)data & known;
		*(uint64_t *)data = H.quirks;
		hlog(HLOG_INFO, "serialization quirks 0x%llx", (unsigned long long)H.quirks);
		return true;
	}

	/* --- system ----------------------------------------------------- */
	case RETRO_ENVIRONMENT_SHUTDOWN:
		host_request_quit(HOST_EXIT_OK);
		return true;
	case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_DEVICE_POWER: {
		struct retro_device_power *p = data;
		int pct;
		bool chg;

		if (!p)
			return true;
		if (sys_battery(&pct, &chg) < 0)
			return false;
		p->state = chg ? (pct >= 100 ? RETRO_POWERSTATE_CHARGED : RETRO_POWERSTATE_CHARGING)
			       : RETRO_POWERSTATE_DISCHARGING;
		p->seconds = RETRO_POWERSTATE_NO_ESTIMATE;
		p->percent = (int8_t)pct;
		return true;
	}

	case RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB:
		if (!data)
			return false;
		*(retro_environment_t *)data = clear_thread_waits;
		return true;
	case RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE:
		return true; /* input is polled right before retro_run() anyway */

	/* --- accepted and ignored --------------------------------------- */
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
		return true;

	/* --- explicitly unsupported (false, no log spam) ----------------- */
	case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:           /* cores fall back to stdio */
	case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
	case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
	case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
	case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
	case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
	case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
	case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
	case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
	case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
	case RETRO_ENVIRONMENT_GET_MIDI_INTERFACE:
	case RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE:
	case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
	case RETRO_ENVIRONMENT_GET_CAMERA_INTERFACE:
	case RETRO_ENVIRONMENT_GET_LOCATION_INTERFACE:
	case RETRO_ENVIRONMENT_GET_PLAYLIST_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_FILE_BROWSER_START_DIRECTORY:
		return false;

	default:
		if (!(cmd & RETRO_ENVIRONMENT_PRIVATE)) {
			char key[48];

			snprintf(key, sizeof(key), "env:%u", cmd);
			hlog_once(key, "environment command %s not supported", env_name(cmd));
		}
		return false;
	}
}

static bool env_cb(unsigned cmd, void *data)
{
	return core_environment(cmd, data);
}

/* ------------------------------------------------------ AV callbacks */

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	host_video_refresh(data, w, h, pitch);
}

static pthread_mutex_t ain_mu = PTHREAD_MUTEX_INITIALIZER;

void core_audio_lock(void)
{
	pthread_mutex_lock(&ain_mu);
}

void core_audio_unlock(void)
{
	pthread_mutex_unlock(&ain_mu);
}

static void ain_reserve(int frames)
{
	if (H.ain_n + frames <= H.ain_cap)
		return;
	{
		int nc = (H.ain_n + frames) * 2;
		int16_t *n = realloc(H.ain, (size_t)nc * 2 * sizeof(int16_t));

		if (!n)
			return;
		H.ain = n;
		H.ain_cap = nc;
	}
}

static void audio_cb(int16_t left, int16_t right)
{
	core_audio_lock();
	ain_reserve(1);
	if (H.ain_n < H.ain_cap) {
		H.ain[2 * H.ain_n] = left;
		H.ain[2 * H.ain_n + 1] = right;
		H.ain_n++;
	}
	core_audio_unlock();
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	core_audio_lock();
	ain_reserve((int)frames);
	if (H.ain_n + (int)frames <= H.ain_cap) {
		memcpy(H.ain + 2 * H.ain_n, data, frames * 2 * sizeof(int16_t));
		H.ain_n += (int)frames;
	}
	core_audio_unlock();
	return frames;
}

static void input_poll_cb(void)
{
	/* Input is polled by the host right before retro_run(). */
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port >= HOST_MAX_PORTS || H.bench_step)
		return 0; /* a benchmark run sends no input */
	switch (device & RETRO_DEVICE_MASK) {
	case RETRO_DEVICE_JOYPAD:
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
			return (int16_t)hin_buttons((int)port);
		return id < 16 ? (int16_t)((hin_buttons((int)port) >> id) & 1) : 0;
	case RETRO_DEVICE_ANALOG:
		if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
			return id < 16 ? hin_analog_button((int)port, (int)id) : 0;
		if (index > RETRO_DEVICE_INDEX_ANALOG_RIGHT || id > RETRO_DEVICE_ID_ANALOG_Y)
			return 0;
		return hin_analog((int)port, (int)index, (int)id);
	default:
		return 0;
	}
}

void core_set_callbacks(void)
{
	H.core.set_environment(env_cb);
	H.core.set_video_refresh(video_cb);
	H.core.set_audio_sample(audio_cb);
	H.core.set_audio_sample_batch(audio_batch_cb);
	H.core.set_input_poll(input_poll_cb);
	H.core.set_input_state(input_state_cb);
}
