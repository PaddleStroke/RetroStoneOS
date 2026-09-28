/*
 * host_internal.h - state shared by the host's modules (single-threaded
 * except the save worker in saves.c, which never touches this struct).
 */
#ifndef RSOS_HOST_INTERNAL_H
#define RSOS_HOST_INTERNAL_H

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../third_party/libretro/libretro.h"
#include "../display.h"
#include "bench.h"
#include "coreinfo.h"
#include "host.h"
#include "pacing.h"
#include "playtime.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct core_api {
	void *handle;
	void (*init)(void);
	void (*deinit)(void);
	unsigned (*api_version)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*set_controller_port_device)(unsigned, unsigned);
	void (*reset)(void);
	void (*run)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
};

#define HOST_MAX_PORTS 4
#define HOST_MAX_DESC 64

struct host {
	struct host_config cfg;

	/* Game identity. */
	char core_id[64];
	char system[64];
	char game[256];            /* save/state base name */
	char rom_path[PATH_MAX];   /* as given */
	char content_path[PATH_MAX]; /* passed to the core (maybe extracted) */
	char save_dir[PATH_MAX];
	char state_dir[PATH_MAX];
	char bios_dir[PATH_MAX];
	struct core_info info;

	/* Core. */
	struct core_api core;
	struct retro_system_info sysinfo;
	struct retro_system_av_info av;
	enum retro_pixel_format pixfmt;
	uint32_t drm_format;
	bool support_no_game;
	bool game_loaded;
	uint64_t quirks;
	struct retro_system_content_info_override overrides[16];
	int noverrides;
	struct retro_game_info_ext game_ext;
	void *content_data;        /* ROM in memory (need_fullpath = false) */
	size_t content_size;

	/* Optional core callbacks. */
	struct retro_frame_time_callback frame_time;
	int64_t frame_time_last_us;
	retro_audio_buffer_status_callback_t audio_status_cb;
	unsigned min_audio_latency_ms;
	bool has_disk;
	struct retro_disk_control_ext_callback disk;

	/* Input descriptors / controller info (Controls page). */
	struct { unsigned port, device, index, id; char desc[48]; } desc[HOST_MAX_DESC];
	int ndesc;
	struct { unsigned id; char desc[48]; } ctrl[HOST_MAX_PORTS][8];
	int nctrl[HOST_MAX_PORTS];
	unsigned port_device[HOST_MAX_PORTS];

	/* HW rendering. */
	bool hw_requested;
	struct retro_hw_render_callback hw;

	/* Video. */
	bool display_ok;
	int scale;                 /* enum display_scale_mode */
	const void *last_frame;    /* core's buffer (RetroArch caches the pointer too) */
	unsigned last_w, last_h;
	size_t last_pitch;
	void *shadow;              /* overlay composition buffer */
	size_t shadow_size;
	void *headless_frame;      /* copy of the last frame (headless) */
	size_t headless_size;
	uint64_t frames_presented, frames_skipped, frames_duped;
	bool skip_video;           /* fast-forward: frame not shown */
	bool hw_capture;           /* zero-copy HW frame: last_frame needs a readback */

	/* Audio. */
	bool audio_ok;
	struct resampler *rs;
	int16_t *ain;              /* this frame's core samples */
	int ain_n, ain_cap;
	int16_t *aout;
	int aout_cap;
	int64_t underrun_t0;
	unsigned underrun_window;
	double last_ratio;

	/* Pacing. */
	struct pacing pace;
	int64_t next_frame_us;

	/* Run state. */
	volatile sig_atomic_t quit_sig;
	volatile sig_atomic_t flush_sig;
	volatile sig_atomic_t poweroff_sig;  /* RSOS_SIG_POWEROFF */
	volatile sig_atomic_t sleep_sig;     /* 1 = sleep requested, 2 = wake */
	bool quit;
	int exit_code;
	bool poweroff;
	bool paused_disconnect;
	int slot;
	uint64_t frame;
	volatile uint64_t heartbeat;
	volatile bool busy_ok;     /* long operation in progress: watchdog waits */
	int ff_speed;              /* 0 = off, else runs per displayed frame */
	int autosave;
	bool show_stats;

	/* Stats. */
	int64_t stat_t0;
	uint64_t stat_frames0;
	double fps;
	double ft_avg_ms, ft_max_ms, ft_acc_ms;
	int ft_n;
	int64_t last_battery_ms;
	uint64_t frames_delivered; /* video_refresh with a new frame (not a dupe) */
	double core_ms_total;      /* sum of core time (retro_run minus present) */
	uint64_t stalls;           /* frames with core time over PERF_STALL_MS */
	double stall_ms;           /* their total core time */
	uint64_t stalls_gl;        /* of which with shader compiles/links or slow draws */
	int stalls_logged;
	bool perf_log;             /* N64 / GLES: periodic perf lines in game.log */
	bool stats_on_plane;       /* the FPS overlay is on the overlay plane */
	char stats_line[96];       /* the FPS overlay text (both paths) */

	/* Benchmark (§18). */
	bool bench_step;           /* this process runs one configuration */
	bool bench_requested;      /* the menu asked for a benchmark */
	bool bench_aborted;

	/* Batch 2: fast-forward hotkey, play time, game switcher (§10). */
	int ff_cap;                /* Select+R2 speed (settings ff_speed, 2..4) */
	int64_t ff_resync_until;   /* ms: no latency back-off until then (after FF) */
	int64_t ff_t0_ms;          /* fast-forward started (log: the effective speed) */
	uint64_t ff_frame0;
	struct playtime pt;
	int64_t pt_next_report;    /* ms: next "playtime" status line */
	int switch_to;             /* the switcher entry to launch next, -1 = none */
	bool rumble;               /* controller vibration (settings rumble) */
	bool ff_on_plane;          /* the fast-forward indicator is on the overlay plane */
	char cpu_profile[16];      /* auto / performance / powersave (in-game menu) */
};

/* The benchmark results page (menu.c). */
struct host_bench_report {
	int n, best;
	struct bench_result r[BENCH_MAX_CONFIGS];
	int order[BENCH_MAX_CONFIGS];
	char path[PATH_MAX];       /* the report on the SD card */
};

extern struct host H;

/* core.c */
int core_open(const char *path);
void core_close(void);
bool core_environment(unsigned cmd, void *data);
void core_set_callbacks(void);
void host_video_setup(void);
void host_audio_setup(void);

/* host.c */
void host_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void host_status(const char *kind, const char *msg);
void host_present_last(void);
void host_capture_last(void);
void host_request_quit(int code);
void host_reevaluate_pacing(void);
void host_do_hotkey_save(int slot);
void host_do_hotkey_load(int slot);
double host_run_hz(void);
void host_video_refresh(const void *data, unsigned w, unsigned h, size_t pitch);
void host_video_invalidate(void);
void host_poll_signals(void);

/* osd.c */
void osd_toast(const char *msg, int ms);
bool osd_active(void);
void osd_draw(void *px, int pitch, int w, int h, uint32_t fmt);
void osd_clear(void);

/* host.c: fast-forward (0 = off, else runs per displayed frame), play time
 * pauses (in-game menu, switcher), per-game settings to the menu UI. */
void host_set_ff(int speed);
void host_play_pause(bool pause);
void host_setting_changed(const char *key, const char *value);

/* switcher.c: the game switcher (Select+Y). Blocks until closed; returns the
 * chosen entry of the switcher file (> 0: another game), 0 or -1 (resume). */
bool host_switcher_available(void);
int host_switcher_run(void);
int host_switcher_screenshot(const char *path);

/* menu.c: blocks until the menu is closed. */
void host_menu_run(void);
void host_menu_run_bench_results(void);
int host_menu_screenshot(const char *path);
int host_menu_script(const char *script);

/* host.c: benchmark hooks for the menu. */
bool host_bench_available(char *desc, size_t n);   /* desc: "10 settings, about 7 min" */
void host_bench_request(void);                    /* start once the menu is closed */
const struct host_bench_report *host_bench_report(void);
int host_bench_use_best(char *msg, size_t n);

/* core.c: the core's audio may come from another thread (GLideN64's
 * threaded renderer): H.ain is guarded by this lock. */
void core_audio_lock(void);
void core_audio_unlock(void);

/* sys.c */
void sys_governor_performance(bool on);
int sys_battery(int *percent, bool *charging); /* 0 ok, -errno */
void sys_power_off(const char *cmd);

#endif
