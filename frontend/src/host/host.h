/*
 * host.h - RetroStoneOS libretro host: runs one game (one core, one
 * content) with display, audio and input, then returns. Replaces RetroArch.
 * See docs/host-design.md.
 *
 * Process model: each game runs in its own child process
 * (`rsos-frontend --run ...` or `rsos-run ...`, i.e. host_main()), started
 * by the UI with host_launch(). A crashing or hung core cannot take the
 * menu down, and every core starts from a fresh address space.
 *
 *   UI process                               game process (child)
 *   ----------                               --------------------
 *   display_suspend() (drops DRM master)
 *   host_launch(&game, ...)  --fork+exec-->  host_main(): display_init()
 *        (waitpid, status pipe)              (DRM master), ALSA, input,
 *                                            dlopen(core), run, flush saves
 *   display_resume(), drain input, redraw <-- exit code + status line
 */
#ifndef RSOS_HOST_H
#define RSOS_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "../display.h"

/* Exit codes of the game process. */
enum host_exit {
	HOST_EXIT_OK = 0,        /* user quit */
	HOST_EXIT_FAIL = 1,      /* unexpected failure (see the log) */
	HOST_EXIT_ERROR = 2,     /* could not start: message for the UI */
	HOST_EXIT_POWEROFF = 3,  /* saves flushed, the UI must power off now */
	HOST_EXIT_HANG = 4,      /* watchdog: the core stopped responding */
	HOST_EXIT_TEST_FAIL = 5, /* rsos-run self tests */
	/* The player picked another game in the game switcher (Select+Y): the
	 * auto state was written, a "switch <n>" status line names the entry of
	 * the switcher file (--switcher) to launch next, with resume. */
	HOST_EXIT_SWITCH = 6,
	HOST_EXIT_USAGE = 64,
};

struct host_config {
	const char *core_path;       /* .../<id>_libretro.so (required) */
	const char *rom_path;        /* NULL: core without content */
	const char *system;          /* ROM folder name; NULL = parent dir of the ROM */

	/* Paths (defaults: the device layout). */
	const char *core_info_dir;   /* /usr/share/rsos/cores */
	const char *bios_dir;        /* /data/bios */
	const char *saves_root;      /* /data/saves   (+ /<system>) */
	const char *states_root;     /* /data/states  (+ /<system>) */
	const char *coreopts_ship;   /* /usr/share/rsos/coreopts */
	const char *coreopts_user;   /* /data/rsos/coreopts */
	const char *tmp_dir;         /* /tmp/rsos (extracted zips) */
	const char *settings_path;   /* /data/rsos/settings.ini */
	const char *battery_path;    /* /run/rsos/battery (the supervisor's value), NULL = default */
	const char *locale_dir;      /* /usr/share/rsos/locale (<lang>.cat) */
	const char *fonts_dir;       /* /usr/share/rsos/fonts (in-game menu and toasts) */
	const char *lang;            /* --lang: "fr", "pt_BR"...; NULL = settings `language` */

	/* Behaviour. */
	bool headless;               /* no DRM, no ALSA, no input (tests) */
	long max_frames;             /* 0 = unlimited */
	const char *dump_png;        /* write the last frame here at exit */
	const char *menu_shot;       /* render the in-game menu to PNG at exit (tests) */
	int scale;                   /* enum display_scale_mode, -1 = settings */
	int show_stats;              /* FPS overlay: -1 = settings */
	int audio_latency_ms;        /* 0 = settings / 64 */
	double vsync_tolerance;      /* 0 = 0.01 */
	int status_fd;               /* -1 = none; see host_launch() */
	bool governor;               /* performance governor while playing */
	int load_slot;               /* -2 = none, -1 = auto slot, 0..9 */
	int autosave;                /* write .state.auto on exit: -1 = settings */
	bool test_states;            /* rsos-run: state round-trip self test */
	int hdmi_width, hdmi_height; /* 0 = display default */
	int lcd_refresh;             /* LCD refresh in Hz (60 = the retimed 25.2 MHz mode, 0 =
					the panel's own 78.6 Hz), -1 = settings `lcd_refresh` */
	const char *shader_cache_dir; /* Mesa's on-disk shader cache for GL cores
					 (/data/rsos/cache/mesa), NULL or "" = Mesa's default */
	int p1_policy;               /* enum input_p1_policy, -1 = settings `p1` */
	const char *p1_device;       /* --p1-device: the controller that launched the
					game (input_last_source_id()), NULL = none */
	int cz_buttons;              /* 0/1, -1 = settings `cz_buttons` */
	const char *logs_dir;        /* /data/rsos/logs (benchmark reports) */
	const char *load_state_file; /* start from this state file (benchmark) */
	const char *menu_script;     /* tests: drive the in-game menu at frame 10 */
	/* Benchmark (docs/host-design.md §18) */
	const char *bench_plan;      /* --bench-step / --bench-report: the run's plan */
	int bench_step;              /* >= 0: run configuration N of the plan */
	bool bench_report;           /* show the results page after start */
	bool bench_auto_apply;       /* tests: "Use this" on the best result */
	long bench_start_frame;      /* tests: start a benchmark at this frame */
	int bench_seconds;           /* tests: override the plan's times (0 = plan) */
	const char *poweroff_cmd;    /* run after a power-off flush when there is
					no parent (status_fd < 0), NULL = none */
	int log_level;               /* 0..3 */
	/* Batch 2 (docs/host-design.md §10, §11.1) */
	const char *screenshots_root; /* /data/screenshots (+ /<system>): Select+L2 */
	const char *switcher_path;   /* --switcher FILE: the recent games of the game
					switcher (Select+Y), written by the menu; NULL = none */
	const char *switcher_shot;   /* tests: render the switcher to PNG at exit */
	int switcher_pick;           /* tests: at frame 12, as if entry N was picked (> 0) */
	const char *hotkey_script;   /* tests: "F:name,...": hotkeys at displayed frames */
	bool timer_pacing;           /* tests: pace on the clock at the core's rate (headless) */
	int ff_speed;                /* fast-forward speed of Select+R2 (2..4), 0 = settings
					`ff_speed` (default 3) */
	int rumble;                  /* controller vibration: -1 = settings `rumble` (default on) */
	const char *cpu_profile;     /* --cpu-profile: this game's CPU profile (auto,
					performance, powersave), shown by the in-game menu */
	int playtime_report_s;       /* "playtime" status line period (0 = 300 s) */
};

void host_config_defaults(struct host_config *cfg);

/* Runs the game in this process. Returns an enum host_exit. */
int host_run(const struct host_config *cfg);

/* The rsos-run command line (also `rsos-frontend --run ...`). */
int host_main(int argc, char **argv);
/* The arguments host_main() got (argv[0] is "--run" under the frontend):
 * the benchmark re-executes the game process with them. */
int host_saved_args(char *const **argv);
void host_usage(const char *argv0);
/* Blocks (true) or unblocks the supervisor's signals to the game process
 * (sleep, wake, power-off: RSOS_SIG_*), e.g. across an exec; returns their
 * numbers in the non-NULL pointers. */
void host_supervisor_signals(bool block, int *sleep_sig, int *wake_sig, int *poweroff_sig);

/* ------------------------------------------------------------ UI side */

/* Checks the BIOS files before a launch (no fork). Returns true if the
 * game can start; else msg = "PS1 needs scph5501.bin in /bios". */
bool host_check_game(const char *core_path, const char *rom_path, const char *system,
		     char *msg, size_t n);

/*
 * The core for (system, rom): the per-game then per-system choice in
 * /data/rsos/cores.ini ([snes] core = snes9x2010, [snes/<rom stem>] core = ...)
 * if that core accepts the file's extension, else the automatic default
 * (never an `experimental = true` core). Writes the .so path; returns 0 or
 * -ENOENT. To offer a choice (with "(experimental)" labels), the UI calls
 * coreinfo_candidates() from host/coreinfo.h.
 */
int host_pick_core(const char *system, const char *rom_path, char *core_path, size_t n);

struct host_launch_opts {
	const char *exe;             /* NULL: "/proc/self/exe" with "--run" first */
	const char *const *extra_args; /* extra rsos-run arguments, NULL-terminated */
	const char *log_path;        /* child stderr, NULL = /tmp/rsos-game.log */
	/* Append to log_path with a "=== launch" header line instead of
	 * truncating it (a file over 1 MiB still starts over). */
	bool log_append;
	/* false (default): the power module owns the CPU governor, the child
	 * gets --no-governor. true: the child sets "performance" and this
	 * function restores schedutil after it (no power module). */
	bool manage_governor;
	/* Start from the .state.auto (the UI asked "Resume?"). */
	bool resume;
	/* Called about every 100 ms while the game runs (the UI keeps its
	 * evdev fds open without grabbing them): return true to kill a hung
	 * game (e.g. Select+Start held 5 s). May be NULL. */
	bool (*idle)(void *user);
	void *user;
	/* Called with each status line of the game process as it arrives (kind,
	 * then the rest of the line), while the game runs: "playtime <s>" (the
	 * seconds played in this session, every few minutes and at exit),
	 * "setting scale <mode>" / "setting cpu <profile>" (changed in the
	 * in-game menu: save them for this game), plus the ones res collects.
	 * May be NULL. */
	void (*on_status)(const char *kind, const char *arg, void *user);
};

struct host_launch_result {
	enum host_exit status;       /* HOST_EXIT_*, or HOST_EXIT_FAIL on a crash */
	bool crashed;                /* killed by a signal */
	int signal;
	int exit_code;
	char message[256];           /* for the UI ("" = nothing to say) */
	char warning[256];           /* non-blocking note from the child */
	/* The child wrote .state.auto at its exit (power-off, or exit with
	 * Settings > Games > Auto-save on exit): size and write time. False if
	 * it was not written (setting off, core without states, crash, kill). */
	bool auto_state_saved;
	long long auto_state_bytes;
	int auto_state_ms;
	/* HOST_EXIT_SWITCH: the switcher file entry to launch next, else -1. */
	int switch_to;
	/* The last "playtime" line: seconds played in this session (menus,
	 * pauses and the switcher excluded), -1 if none came. */
	long long playtime_s;
};

/*
 * Starts the game process and waits for it. The caller hands the display
 * over around it: display_suspend() before (drops DRM master, keeps all
 * state), display_resume() after (takes master back, handles a hotplug
 * that happened during the game). Its evdev fds may stay open (no grab):
 * drain them after. With opts.manage_governor, restores the CPU governor
 * whatever happened to the child. Returns 0, or -errno if fork/exec failed.
 */
int host_launch(const char *core_path, const char *rom_path, const char *system,
		const struct host_launch_opts *opts, struct host_launch_result *res);

/* The running game process (0 if none): forward SIGTERM to it. */
pid_t host_child_pid(void);

/*
 * The game switcher's file (--switcher): the recent games, the running one
 * first (current). The menu writes it before each launch (on tmpfs), the game
 * process reads it when Select+Y opens the switcher, and the menu reads it
 * again to launch the entry a "switch <n>" line names. thumb: the entry's
 * auto-state thumbnail (<state>.png, may not exist).
 */
#define HOST_SWITCHER_MAX 8
struct host_switch_entry {
	char name[128];
	char system[32];
	char rom[1024];
	char core[64];
	char core_path[512];
	char thumb[1100];
	bool current;
};
int host_switcher_write(const char *path, const struct host_switch_entry *e, int n);
/* Returns the number of entries read (0 if none or no file). */
int host_switcher_read(const char *path, struct host_switch_entry *e, int max);

/*
 * The save/state base name the game process will use for this content
 * (RetroArch rule: content basename; for an extracted zip, the file inside).
 */
void host_game_name(const char *core_path, const char *rom_path, char *name, size_t n);
/* True if /data/states/<system>/<game>.state.auto exists: offer "Resume
 * where you left off?" and launch with opts.resume. */
bool host_has_resume_state(const char *core_path, const char *rom_path, const char *system);
/* The same under another states root (NULL = /data/states; tests): writes
 * the .state.auto path and returns true if the file exists. */
bool host_auto_state_path(const char *states_root, const char *core_path, const char *rom_path,
			  const char *system, char *path, size_t n);

#endif
