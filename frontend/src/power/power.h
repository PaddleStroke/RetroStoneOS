/*
 * power.h - RetroStoneOS power management: battery status and warnings,
 * critical-battery emergency save + power-off, the power key (any press:
 * clean shutdown), idle dimming, CPU governor policy, thermal watch,
 * charge mode and the wall clock. Design: docs/power.md. The fake sleep
 * code is still here (power_sleep()), but nothing user-facing reaches it.
 *
 * Runs in the frontend's supervisor (menu) process only, never in the game
 * child. Single-threaded, non-blocking. Integration sketch:
 *
 *   struct power_config pc;
 *   power_config_defaults(&pc);
 *   pc.cb.on_shutdown_request = my_shutdown;   // save everything, then power_poweroff()
 *   pc.cb.on_sleep_request    = my_sleep;      // pause game / display off, and back
 *   ...
 *   power_init(&pc);
 *   for (;;) {
 *       poll({ display fd, input fd, power_get_fd(), game child pidfd }, ...);
 *       power_poll();                            // cheap when nothing is due
 *       while (input_next_nav(in, &ev))
 *           if (!power_on_input()) ui_button(...);   // false: not a wake-up key
 *   }
 *
 * The module owns the AXP209 power key (axp20x-pek, KEY_POWER): the UI and
 * the game child must ignore IN_HK_POWER_SHORT / IN_HK_POWER_OFF.
 */
#ifndef RSOS_POWER_H
#define RSOS_POWER_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "battery.h"
#include "bootreason.h"
#include "pclock.h"

/*
 * Supervisor -> game child signals (docs/power.md §10; the child's side is
 * the host's). SIGUSR1 (flush SRAM) and SIGTERM (quit) already exist.
 */
#define RSOS_SIG_POWEROFF SIGUSR2        /* flush SRAM + .state.auto, exit HOST_EXIT_POWEROFF */
#define RSOS_SIG_SLEEP    (SIGRTMIN + 1) /* pause, mute, display off, wait */
#define RSOS_SIG_WAKE     (SIGRTMIN + 2) /* drain input, display on, resume */
#define RSOS_SIG_IDLE_WARN   (SIGRTMIN + 3) /* idle power-off in 10 s: show the notice */
#define RSOS_SIG_IDLE_CANCEL (SIGRTMIN + 4) /* idle power-off cancelled: clear it */

/* Ordered by severity; the same values as enum batt_level. */
enum power_level {
	POWER_LEVEL_OK = BATT_LEVEL_OK,
	POWER_LEVEL_LOW = BATT_LEVEL_LOW,             /* <= 15 %: a toast */
	POWER_LEVEL_VERY_LOW = BATT_LEVEL_VERY_LOW,   /* <= 7 %: a persistent warning */
	POWER_LEVEL_CRITICAL = BATT_LEVEL_CRITICAL,   /* saving and powering off */
};

enum power_mode {
	POWER_MODE_NORMAL = 0,
	POWER_MODE_SLEEP,          /* fake sleep: screen off, game paused, CPU at min */
	POWER_MODE_CHARGE,         /* booted by the charger: charge screen only */
	POWER_MODE_SHUTTING_DOWN,  /* on_shutdown_request() was called */
};

enum power_screen {
	POWER_SCREEN_ON = 0,
	POWER_SCREEN_DIM,          /* idle in the menu: backlight lowered */
	POWER_SCREEN_OFF,          /* idle longer: backlight off, display may blank */
};

enum power_idle_event {
	POWER_IDLE_WARN = 0,          /* the idle power-off comes in `seconds` */
	POWER_IDLE_CANCEL,            /* cancelled by an input or a busy job */
};

enum power_reason {
	POWER_REASON_USER = 0,         /* power key long press, or the menu */
	POWER_REASON_REBOOT,           /* the menu's "Reboot" */
	POWER_REASON_CRITICAL,         /* battery empty */
	POWER_REASON_SLEEP_TIMEOUT,    /* asleep for sleep_timeout_min */
	POWER_REASON_CHARGER_REMOVED,  /* charge mode, charger unplugged */
	POWER_REASON_THERMAL,          /* sustained over thermal_crit_mc */
	POWER_REASON_IDLE,             /* no input for idle_poweroff_s (never while busy) */
};

struct power_status {
	bool valid;                /* at least one battery sample was read */
	bool battery_present;
	bool charger_online;       /* ACIN (micro-USB) or USB online */
	enum batt_state state;     /* charging / discharging / full ... */
	int percent;               /* what the icon shows, 0..100, -1 unknown */
	int percent_axp;           /* AXP fuel gauge, -1 n/a */
	int percent_voltage;       /* voltage-curve estimate, -1 n/a */
	int voltage_mv;            /* last loaded voltage */
	int voltage_filtered_mv;   /* median of the last samples */
	int current_ma;            /* averaged, + charging, - discharging */
	int minutes_left;          /* to empty / to full, -1 unknown */
	enum power_level level;
	int temp_mc;               /* CPU temperature (milli-C), POWER_TEMP_UNKNOWN */
	bool hot;                  /* over thermal_warn_mc (kernel throttles at 75 C) */
	enum power_mode mode;
	enum power_screen screen;
	bool game_running;
	bool clock_restored;       /* the time came from lastclock: approximate */
	enum power_boot_reason boot_reason;
};
#define POWER_TEMP_UNKNOWN (-1000000)

struct power_callbacks {
	/* Anything the battery icon shows changed (percent, charging, level). */
	void (*on_status)(const struct power_status *st, void *user);
	/* Level changes: LOW = toast, VERY_LOW = persistent warning (repeated
	 * every warn_repeat_s), OK = clear the warning (charger, recovery).
	 * CRITICAL comes through on_critical instead. */
	void (*on_warning)(enum power_level level, const struct power_status *st, void *user);
	/* Battery empty. Show "Battery empty, saving..." now; it is followed
	 * immediately by on_shutdown_request(POWER_REASON_CRITICAL). */
	void (*on_critical)(const struct power_status *st, void *user);
	/* enter = true: pause the game (RSOS_SIG_SLEEP to the game child),
	 * mute audio, display off (display_set_active(false): black frame
	 * with the panel still scanning on panel-keep-scanning boards). The module already turned the
	 * backlight off and lowered the CPU clock.
	 * enter = false: undo it (RSOS_SIG_WAKE to the child); the module
	 * already restored the backlight and the governor. */
	void (*on_sleep_request)(bool enter, void *user);
	/* Save everything (game child: RSOS_SIG_POWEROFF = flush SRAM + auto
	 * state + exit; wait for it), show a message, then call power_poweroff().
	 * If power_poweroff() is not called within the grace time (15 s,
	 * 10 s when critical) the module powers off by itself. */
	void (*on_shutdown_request)(enum power_reason why, void *user);
	/* Idle dimming in the menu. OFF: the display layer turns the
	 * display off too (display_set_active(false)) (the backlight is already off). ON: back on. */
	void (*on_screen)(enum power_screen s, void *user);
	/* Temperature crossed thermal_warn_mc (hot = true) or went back. */
	void (*on_thermal)(int temp_mc, bool hot, void *user);
	/* Charge mode ended by a short power-key press: start the normal UI. */
	void (*on_charge_exit)(void *user);
	/* Idle power-off (docs/power.md, idle power-off): WARN = it happens in
	 * `seconds` (show "Powering off in 10 s", the screen is lit again);
	 * CANCEL = an input (or a busy job) cancelled it: take the notice down.
	 * The power-off itself is on_shutdown_request(POWER_REASON_IDLE). */
	void (*on_idle_poweroff)(enum power_idle_event ev, int seconds, void *user);
	/* Log lines (NULL: stderr). level: 0 error, 1 warning, 2 info, 3 debug. */
	void (*log)(int level, const char *msg, void *user);
	void *user;
};

struct power_config {
	/* Paths (the unit tests point them at temp trees). */
	const char *sysfs;          /* "/sys" */
	const char *dev_input;      /* "/dev/input" */
	const char *run_dir;        /* "/run/rsos" (bootreason, clock-restored) */
	const char *clock_file;     /* "/data/rsos/lastclock" */
	const char *rtc_dev;        /* "/dev/rtc0" */

	/* Battery (see struct batt_params for the meaning and defaults). */
	struct batt_params batt;
	int poll_ms;                /* 10000: normal sampling period */
	int poll_low_ms;            /* 2000: when the filtered voltage < low_poll_mv */
	int poll_sleep_ms;          /* 30000: while asleep / in charge mode */
	int low_poll_mv;            /* 3650 */
	int warn_repeat_s;          /* 300: VERY_LOW warning repeat */
	int axp_voff_mv;            /* 3000: AXP209 hardware cut-off (REG31), 0 = leave */

	/*
	 * The board (src/board.h, docs/porting.md). Names are sysfs/evdev
	 * names; NULL = auto-detect, "" = the board has none.
	 *   builtin_prefix  "RetroStone2": evdev name prefix of the built-in keys
	 *   pek_name        "axp20x-pek": the power key's evdev name, and the
	 *                   platform driver whose startup/shutdown attributes
	 *                   are set (NULL: any device with KEY_POWER and at most
	 *                   4 keys, no PEK timings)
	 *   battery_name, ac_name, usb_name  power supplies (NULL: first of
	 *                   its type; a missing name falls back to that too)
	 *   backlight_name  /sys/class/backlight entry (NULL: the first)
	 *   thermal_type    thermal zone type (NULL: a "cpu" zone)
	 */
	const char *builtin_prefix;
	const char *pek_name;
	const char *battery_name;
	const char *ac_name;
	const char *usb_name;
	const char *backlight_name;
	const char *thermal_type;

	/* Power key */
	bool own_input;             /* true: open the power key and the built-in keys */
	int long_press_ms;          /* 2000: clean shutdown (a short press too) */
	int pek_shutdown_min_ms;    /* 6000: AXP hardware force-off, never shorter */
	int pek_startup_ms;         /* 128: power-on hold (AXP209: 128/1000/2000/3000; 0 = leave) */
	int pek_start_ignore_ms;    /* 2000: presses this soon after power_init() are ignored */
	int pek_quiet_ms;           /* 1500: presses ignored after one left charge mode */

	/* Sleep */
	int sleep_timeout_min;      /* 15: then save + power off (0 = never) */
	bool sleep_wake_any;        /* true: any built-in key wakes, false: power key only */
	bool sleep_cpu1_offline;    /* false: TODO(hw) try hotplugging cpu1 while asleep */
	bool sleep_led;             /* true: blue status LED on while asleep */

	/* Idle (menu only, never while a game runs) */
	int idle_dim_s;             /* 120 (0 = never) */
	int idle_off_s;             /* 300 (0 = never) */
	int idle_poweroff_s;        /* 300: power off (0 = never; also in a game) */
	int idle_warn_s;            /* 10: the notice before it */
	int dim_percent;            /* 30: dimmed brightness, % of the user's level */

	/* CPU */
	const char *menu_governor;  /* "schedutil" */
	const char *game_governor;  /* "performance" */
	const char *sleep_governor; /* "powersave" (fallback: max_freq = min) */

	/* Thermal */
	int thermal_warn_mc;        /* 75000 (the dtsi passive trip) */
	int thermal_hyst_mc;        /* 5000 */
	int thermal_crit_mc;        /* 95000 for 3 samples: save + power off
	                               (the kernel cuts at 100 C without saving) */

	/* Charge mode */
	int charge_dim_percent;     /* 20 */
	int charge_screen_s;        /* 30: charge screen visible after a key */
	int charge_removed_ms;      /* 2000: charger gone this long -> power off */

	/* Shutdown */
	int shutdown_grace_ms;      /* 15000 */
	int critical_grace_ms;      /* 10000 */
	int poweroff_fallback_ms;   /* 20000: init did not power off -> reboot(2) */

	/* Clock */
	int clock_save_s;           /* 600 */

	/* Testing / integration hooks (NULL = real behaviour). */
	int64_t (*now_ms)(void);    /* CLOCK_MONOTONIC in ms */
	time_t (*wall_now)(void);   /* time(NULL) */
	/* Instead of asking init: returns 0. */
	int (*do_poweroff)(bool reboot, void *user);
	bool no_uevent;             /* do not open the netlink socket */

	struct power_callbacks cb;
};

void power_config_defaults(struct power_config *cfg);

/* Opens sysfs, the power key and the timers, applies the AXP PEK timing,
 * the AXP cut-off voltage and the menu governor, reads the boot reason
 * (<run_dir>/bootreason, written by rsos-bootreason) and takes the first
 * battery sample. 0 or -errno. One instance per process. The callbacks
 * can already fire from here (initial on_status, a LOW warning at boot).
 * power_poll() and power_on_input() may also call them. */
int power_init(const struct power_config *cfg);
void power_exit(void);

/* An fd for poll()/epoll (readable when power_poll() has work). */
int power_get_fd(void);
/* ms until power_poll() is due, for callers that do not poll the fd. */
int power_timeout_ms(void);
/* Non-blocking: reads events, samples when due, runs the timers and the
 * callbacks. Always safe to call. */
int power_poll(void);

const struct power_status *power_get_status(void);
enum power_mode power_get_mode(void);

/* The game child started / exited: game governor, idle timers off. */
void power_set_game_running(bool running);
/* Per-core CPU profile for games (from the core .ini): governor NULL =
 * game_governor, max_khz 0 = no cap. Applies now if a game runs. */
void power_set_game_cpu(const char *governor, int max_khz);
/* HDMI docked: no LCD backlight to dim or restore. */
void power_set_docked(bool docked);

/* Call for every UI input event (press, repeat, release) from any pad.
 * Returns true when the event must be dropped: it woke the screen (idle
 * dim/off, charge screen) or the device is asleep / shutting down. The
 * waking key is dropped (press, repeats, release); the next press passes. */
bool power_on_input(void);
/* Activity that is never swallowed (e.g. a game child reported input). */
void power_notify_activity(void);

/*
 * Idle power-off: is this analog axis value (-32768..32767) a player? True
 * for a real move only: the axis held beyond POWER_AXIS_DEADZONE (half
 * deflection), or moved by more than POWER_AXIS_MOVE (20 % of the range)
 * since the last move counted (*ref, updated then; start it at 0). The
 * noise of a drifting stick counts for nothing, so it can never keep the
 * unit awake. Pure: no module state.
 */
#define POWER_AXIS_DEADZONE 16384
#define POWER_AXIS_MOVE     13107
bool power_axis_activity(int16_t v, int16_t *ref);

/*
 * A long job runs (a USB import/export or backup, the web or SMB share with
 * a client, an OS update, ...): no idle power-off while busy (a pending
 * notice is cancelled). When it ends the power-off countdown starts over.
 * why: for the log ("usb-import"), NULL when not busy. Call it whenever the
 * state may have changed (cheap: only a change is acted upon).
 */
void power_set_busy(bool busy, const char *why);
/*
 * An idle power-off (POWER_REASON_IDLE) that cannot be completed safely
 * (the game could not write its resume state): back to normal, the unit
 * stays on and the countdown starts over. Only before power_poweroff() and
 * only for the idle reason. 0 or -EINVAL.
 */
int power_cancel_shutdown(void);

/* Fake sleep. Unreachable from the power key and the menu since the short
 * press powers off (docs/power.md §6); kept for tools and tests. */
int power_sleep(bool enter);
/* The menu's "Power off" / "Reboot": the same path as the long press. */
void power_request_shutdown(enum power_reason why);
/* Final step, after saving: saves the clock, sync(), asks init to power
 * off (BusyBox init: SIGUSR2) or reboot (SIGTERM). A fallback calls
 * reboot(2) directly if init has not done it after poweroff_fallback_ms.
 * 0 or -errno. */
int power_poweroff(bool reboot);

/* Settings by settings.ini key (the UI forwards its setting_changed):
 *   sleep_timeout_min  0..240     idle_dim_min   0..60   idle_off_min 0..120
 *   idle_poweroff_min  0..240 (0 = never)
 *   sleep_wake         any|power  battery_gauge  auto|axp|voltage
 *   idle_dim_s / idle_off_s / idle_poweroff_s  0..7200 seconds (development
 *                      and tests only)
 *   timezone           a zone name from power_timezones() or a POSIX TZ
 * 0, -EINVAL (bad value) or -ENOENT (not a power key). */
int power_set_setting(const char *key, const char *value);

/* Manual date/time: sets the system clock, the RTC and lastclock. */
int power_set_time(time_t t);
/* Time zone: a name from the table or a POSIX TZ string. Sets TZ for this
 * process and its children (the game child inherits it). */
int power_set_timezone(const char *name_or_posix);
const struct power_tz *power_timezones(int *count);

/* For the tests and for callers that feed keys themselves (own_input =
 * false): an EV_KEY event. KEY_POWER is the power key, anything else a
 * built-in button. value: 1 press, 0 release, 2 repeat (ignored). */
void power_inject_key(int code, int value);

const char *power_reason_name(enum power_reason r);
const char *power_mode_name(enum power_mode m);

#endif
