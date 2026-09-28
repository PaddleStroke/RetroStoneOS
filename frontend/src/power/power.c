/*
 * power.c - RetroStoneOS power management (see power.h and docs/power.md).
 *
 * Everything is driven from power_poll(): one epoll fd groups a timerfd
 * (the next deadline), a uevent socket (power_supply changes: charger
 * plugged, battery status) and the evdev nodes of the power key and the
 * built-in buttons. All paths go through cfg.sysfs, so the unit tests run
 * the whole state machine against fake sysfs trees and a fake clock.
 */
#include "power.h"
#include "psys.h"
#include "../uevent.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define MAX_EVDEV 8
#define SWALLOW_RELEASE_MS 100
#define THERMAL_CRIT_SAMPLES 3

struct evdev {
	int fd;
	bool pek;
	bool dropped;      /* SYN_DROPPED seen: skip to the next SYN_REPORT, then resync */
};

static struct power {
	bool inited;
	struct power_config cfg;
	struct power_status st;
	struct batt batt;

	int epfd, tfd, ufd;
	struct evdev ev[MAX_EVDEV];
	int nev;

	char bat[PSYS_PATH_MAX], ac[PSYS_PATH_MAX], usb[PSYS_PATH_MAX];
	char bl[PSYS_PATH_MAX], led[PSYS_PATH_MAX], thermal[PSYS_PATH_MAX];

	/* keys */
	int keys_down;
	int64_t pek_down_at;
	bool pek_long_fired, pek_press_woke;
	int64_t pek_quiet_until;   /* power-key presses before this are ignored */
	bool swallow;
	int64_t last_release;

	/* timers */
	int64_t next_sample;
	bool sample_now;
	int64_t last_activity;
	int64_t sleep_since;
	int64_t next_clock_save;
	int64_t shutdown_deadline;
	bool poweroff_started;
	int64_t poweroff_at;
	bool poweroff_reboot;
	int64_t last_warn_at;
	int64_t charger_lost_at;
	int64_t charge_screen_until;
	enum power_reason shutdown_why;

	/* backlight */
	long saved_brightness;     /* -1 = nothing saved */
	bool bl_off;

	/* cpu */
	char gov_applied[32];
	int max_applied;
	bool gov_valid;
	char game_gov[32];
	int game_max_khz;
	bool cpu1_off;

	/* idle power-off */
	bool busy;                 /* a long job runs (power_set_busy) */
	char busy_why[32];
	int64_t busy_end;          /* when the last one ended: the countdown starts over */
	bool idle_warned;          /* the 10 s notice is up */

	bool docked;
	int thermal_crit_count;
} P = { .epfd = -1, .tfd = -1, .ufd = -1 };

/* ------------------------------------------------------------ helpers */
static int64_t now_ms(void)
{
	struct timespec ts;

	if (P.cfg.now_ms)
		return P.cfg.now_ms();
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static time_t wall_now(void)
{
	return P.cfg.wall_now ? P.cfg.wall_now() : time(NULL);
}

__attribute__((format(printf, 2, 3)))
static void plog(int level, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (P.cfg.cb.log)
		P.cfg.cb.log(level, msg, P.cfg.cb.user);
	else if (level <= 2)
		fprintf(stderr, "[power] %s\n", msg);
}

static int rd_long(const char *dir, const char *attr, long *v)
{
	char p[PSYS_PATH_MAX];

	if (!dir[0] || !psys_path(p, sizeof(p), "%s/%s", dir, attr))
		return -ENOENT;
	return psys_read_long(p, v);
}

static int rd_str(const char *dir, const char *attr, char *buf, size_t n)
{
	char p[PSYS_PATH_MAX];

	if (!dir[0] || !psys_path(p, sizeof(p), "%s/%s", dir, attr))
		return -ENOENT;
	return psys_read_str(p, buf, n);
}

static int wr_long(const char *dir, const char *attr, long v)
{
	char p[PSYS_PATH_MAX];

	if (!dir[0] || !psys_path(p, sizeof(p), "%s/%s", dir, attr))
		return -ENOENT;
	return psys_write_long(p, v);
}

static int wr_str(const char *dir, const char *attr, const char *s)
{
	char p[PSYS_PATH_MAX];

	if (!dir[0] || !psys_path(p, sizeof(p), "%s/%s", dir, attr))
		return -ENOENT;
	return psys_write_str(p, s);
}

static int64_t min64(int64_t a, int64_t b)
{
	return a < b ? a : b;
}

const char *power_reason_name(enum power_reason r)
{
	switch (r) {
	case POWER_REASON_USER: return "user";
	case POWER_REASON_REBOOT: return "reboot";
	case POWER_REASON_CRITICAL: return "battery-critical";
	case POWER_REASON_SLEEP_TIMEOUT: return "sleep-timeout";
	case POWER_REASON_CHARGER_REMOVED: return "charger-removed";
	case POWER_REASON_THERMAL: return "thermal";
	case POWER_REASON_IDLE: return "idle";
	}
	return "?";
}

const char *power_mode_name(enum power_mode m)
{
	switch (m) {
	case POWER_MODE_NORMAL: return "normal";
	case POWER_MODE_SLEEP: return "sleep";
	case POWER_MODE_CHARGE: return "charge";
	case POWER_MODE_SHUTTING_DOWN: return "shutting-down";
	}
	return "?";
}

void power_config_defaults(struct power_config *c)
{
	memset(c, 0, sizeof(*c));
	c->sysfs = "/sys";
	c->dev_input = "/dev/input";
	c->run_dir = "/run/rsos";
	c->clock_file = PCLOCK_FILE_DEFAULT;
	c->rtc_dev = PCLOCK_RTC_DEFAULT;
	batt_params_defaults(&c->batt);
	c->poll_ms = 10000;
	c->poll_low_ms = 2000;
	c->poll_sleep_ms = 30000;
	c->low_poll_mv = 3650;
	c->warn_repeat_s = 300;
	c->axp_voff_mv = 3000;
	c->own_input = true;
	c->long_press_ms = 2000;
	c->pek_shutdown_min_ms = 6000;
	c->pek_startup_ms = 128;
	c->pek_start_ignore_ms = 2000;
	c->pek_quiet_ms = 1500;
	c->sleep_timeout_min = 15;
	c->sleep_wake_any = true;
	c->sleep_cpu1_offline = false;
	c->sleep_led = true;
	c->idle_dim_s = 120;
	c->idle_off_s = 300;
	c->idle_poweroff_s = 300;
	c->idle_warn_s = 10;
	c->dim_percent = 30;
	c->menu_governor = "schedutil";
	c->game_governor = "performance";
	c->sleep_governor = "powersave";
	c->thermal_warn_mc = 75000;
	c->thermal_hyst_mc = 5000;
	c->thermal_crit_mc = 95000;
	c->charge_dim_percent = 20;
	c->charge_screen_s = 30;
	c->charge_removed_ms = 2000;
	c->shutdown_grace_ms = 15000;
	c->critical_grace_ms = 10000;
	c->poweroff_fallback_ms = 20000;
	c->clock_save_s = 600;
	c->builtin_prefix = "RetroStone2";
	c->pek_name = "axp20x-pek";
}

/* ----------------------------------------------------- sysfs discovery */
/* A supply named by the board: its directory if it exists, else "" (then
 * the first supply of its type is used). */
static void named_supply(const char *dir, const char *name, char *out, size_t n)
{
	char sub[PSYS_PATH_MAX];
	struct stat st;

	out[0] = '\0';
	if (!name || !*name || !psys_path(sub, sizeof(sub), "%s/%s", dir, name))
		return;
	if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
		snprintf(out, n, "%s", sub);
	else
		plog(1, "power supply %s not found: auto-detecting", name);
}

static void find_supplies(void)
{
	char dir[PSYS_PATH_MAX], sub[PSYS_PATH_MAX], type[32];
	struct dirent *de;
	DIR *d;
	/* "" = the board has none of this kind: never auto-detect it */
	bool no_bat = P.cfg.battery_name && !*P.cfg.battery_name;
	bool no_ac = P.cfg.ac_name && !*P.cfg.ac_name;
	bool no_usb = P.cfg.usb_name && !*P.cfg.usb_name;

	P.bat[0] = P.ac[0] = P.usb[0] = '\0';
	if (!psys_path(dir, sizeof(dir), "%s/class/power_supply", P.cfg.sysfs))
		return;
	named_supply(dir, P.cfg.battery_name, P.bat, sizeof(P.bat));
	named_supply(dir, P.cfg.ac_name, P.ac, sizeof(P.ac));
	named_supply(dir, P.cfg.usb_name, P.usb, sizeof(P.usb));
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (!psys_path(sub, sizeof(sub), "%s/%s", dir, de->d_name))
			continue;
		if (rd_str(sub, "type", type, sizeof(type)))
			continue;
		/* axp20x-battery, axp20x-ac, axp20x-usb on the RetroStone2 */
		if (!strcmp(type, "Battery") && !P.bat[0] && !no_bat)
			snprintf(P.bat, sizeof(P.bat), "%s", sub);
		else if (!strcmp(type, "Mains") && !P.ac[0] && !no_ac)
			snprintf(P.ac, sizeof(P.ac), "%s", sub);
		else if (!strcmp(type, "USB") && !P.usb[0] && !no_usb)
			snprintf(P.usb, sizeof(P.usb), "%s", sub);
	}
	closedir(d);
}

/* First entry of <sysfs>/<cls> whose name contains `want` (NULL = any). */
static void find_class_entry(const char *cls, const char *want, char *out, size_t n)
{
	char dir[PSYS_PATH_MAX];
	struct dirent *de;
	DIR *d;

	out[0] = '\0';
	if (!psys_path(dir, sizeof(dir), "%s/%s", P.cfg.sysfs, cls))
		return;
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (want && !strstr(de->d_name, want))
			continue;
		if (psys_path(out, n, "%s/%s", dir, de->d_name))
			break;
		out[0] = '\0';
	}
	closedir(d);
}

/* The CPU thermal zone: sun4i-ts ("rtp") registers "cpu-thermal" on the A20. */
static void find_thermal(void)
{
	char dir[PSYS_PATH_MAX], sub[PSYS_PATH_MAX], type[64];
	struct dirent *de;
	DIR *d;

	P.thermal[0] = '\0';
	if (!psys_path(dir, sizeof(dir), "%s/class/thermal", P.cfg.sysfs))
		return;
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "thermal_zone", 12))
			continue;
		if (!psys_path(sub, sizeof(sub), "%s/%s", dir, de->d_name))
			continue;
		if (rd_str(sub, "type", type, sizeof(type)))
			continue;
		if (P.cfg.thermal_type && *P.cfg.thermal_type) {
			/* the board's zone; the first zone if it is not there */
			if (!strcmp(type, P.cfg.thermal_type)) {
				snprintf(P.thermal, sizeof(P.thermal), "%s", sub);
				break;
			}
			if (!P.thermal[0])
				snprintf(P.thermal, sizeof(P.thermal), "%s", sub);
			continue;
		}
		if (!P.thermal[0] || strstr(type, "cpu"))
			snprintf(P.thermal, sizeof(P.thermal), "%s", sub);
		if (strstr(type, "cpu"))
			break;
	}
	closedir(d);
}

/*
 * AXP209 REG36 through axp20x-pek's sysfs: "shutdown" is the hardware
 * force-off time (4/6/8/10 s). Keep it >= 6 s so our 2 s long press
 * always gets to save first. "startup" is the power-on hold time: set to
 * pek_startup_ms (owner's choice 2026-09-27: 128 ms instead of the 1 s
 * default, ~0.9 s less black screen; a brief press can wake the console).
 * REG36 lives in the AXP, which stays powered by the battery, so the value
 * applies from the next power-on. The long-press IRQ time (REG36[5:4]) is
 * not exposed and not needed: the long press is timed here.
 */
static void setup_pek(void)
{
	char dir[PSYS_PATH_MAX], sub[PSYS_PATH_MAX];
	struct dirent *de;
	long sd = -1, su = -1;
	DIR *d;

	/* Only a named PMIC power key has these (board: power_key_device). */
	if (!P.cfg.pek_name || !*P.cfg.pek_name)
		return;
	if (!psys_path(dir, sizeof(dir), "%s/bus/platform/drivers/%s", P.cfg.sysfs, P.cfg.pek_name))
		return;
	d = opendir(dir);
	if (!d) {
		plog(1, "%s driver not found: PEK timings unchanged", P.cfg.pek_name);
		return;
	}
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (!psys_path(sub, sizeof(sub), "%s/%s", dir, de->d_name))
			continue;
		if (rd_long(sub, "shutdown", &sd))
			continue;
		rd_long(sub, "startup", &su);
		if (P.cfg.pek_startup_ms > 0 && su != P.cfg.pek_startup_ms) {
			int e = wr_long(sub, "startup", P.cfg.pek_startup_ms);

			plog(e ? 0 : 2, "PEK power-on hold %ld ms -> %d ms%s", su,
			     P.cfg.pek_startup_ms, e ? " FAILED" : "");
			if (!e)
				su = P.cfg.pek_startup_ms;
		}
		if (sd < P.cfg.pek_shutdown_min_ms) {
			int e = wr_long(sub, "shutdown", P.cfg.pek_shutdown_min_ms);
			plog(e ? 0 : 2, "PEK force-off %ld ms -> %d ms%s", sd,
			     P.cfg.pek_shutdown_min_ms, e ? " FAILED" : "");
		} else {
			plog(2, "PEK force-off %ld ms, power-on hold %ld ms", sd, su);
		}
		break;
	}
	closedir(d);
}

/* AXP209 V_OFF (REG31), the hardware cut-off: the DT monitored-battery
 * voltage-min-design sets it at probe; this re-applies it if missing. */
static void setup_voff(void)
{
	long v;
	long want = (long)P.cfg.axp_voff_mv * 1000;

	if (!P.cfg.axp_voff_mv || !P.bat[0] || rd_long(P.bat, "voltage_min", &v))
		return;
	if (v != want) {
		int e = wr_long(P.bat, "voltage_min", want);
		plog(e ? 1 : 2, "AXP V_OFF %ld uV -> %ld uV%s", v, want, e ? " FAILED" : "");
	}
}

/* ------------------------------------------------------------- outputs */
static void bl_save(void)
{
	long v;

	if (P.saved_brightness < 0 && !rd_long(P.bl, "brightness", &v))
		P.saved_brightness = v;
}

static void bl_dim(int percent)
{
	long v;

	if (P.docked || !P.bl[0])
		return;
	bl_save();
	if (P.saved_brightness <= 0)
		return;
	v = P.saved_brightness * percent / 100;
	if (v < 1)
		v = 1;
	if (v < P.saved_brightness)
		wr_long(P.bl, "brightness", v);
}

static void bl_power(bool on)
{
	if (P.docked || !P.bl[0])
		return;
	if (!on)
		bl_save();
	/* FB_BLANK_UNBLANK = 0, FB_BLANK_POWERDOWN = 4 */
	wr_long(P.bl, "bl_power", on ? 0 : 4);
	P.bl_off = !on;
}

static void bl_restore(void)
{
	if (P.docked || !P.bl[0])
		return;
	if (P.bl_off)
		bl_power(true);
	if (P.saved_brightness >= 0)
		wr_long(P.bl, "brightness", P.saved_brightness);
	P.saved_brightness = -1;
}

static void led_set(bool on)
{
	if (P.cfg.sleep_led && P.led[0])
		wr_long(P.led, "brightness", on ? 1 : 0);
}

static void cpu1_set(bool online)
{
	char p[PSYS_PATH_MAX];

	if (!P.cfg.sleep_cpu1_offline || P.cpu1_off == !online)
		return;
	if (!psys_path(p, sizeof(p), "%s/devices/system/cpu/cpu1/online", P.cfg.sysfs))
		return;
	if (psys_write_long(p, online ? 1 : 0) == 0)
		P.cpu1_off = !online;
}

static void policy_apply(const char *pol, const char *gov, int max_khz, bool sleep)
{
	long lim;

	if (wr_str(pol, "scaling_governor", gov)) {
		/* powersave not built in: keep the governor, clamp to the min OPP */
		if (sleep && !rd_long(pol, "cpuinfo_min_freq", &lim)) {
			wr_long(pol, "scaling_max_freq", lim);
			return;
		}
		plog(1, "governor %s rejected by %s", gov, pol);
	}
	if (max_khz > 0)
		wr_long(pol, "scaling_max_freq", max_khz);
	else if (!rd_long(pol, "cpuinfo_max_freq", &lim))
		wr_long(pol, "scaling_max_freq", lim);
}

static void gov_apply(void)
{
	char dir[PSYS_PATH_MAX], pol[PSYS_PATH_MAX];
	const char *g;
	struct dirent *de;
	bool sleep = false;
	int maxk = 0;
	DIR *d;

	switch (P.st.mode) {
	case POWER_MODE_SLEEP:
	case POWER_MODE_CHARGE:
		g = P.cfg.sleep_governor;
		sleep = true;
		break;
	case POWER_MODE_SHUTTING_DOWN:
		g = P.cfg.game_governor;    /* save as fast as possible */
		break;
	default:
		if (P.st.game_running) {
			g = P.game_gov[0] ? P.game_gov : P.cfg.game_governor;
			maxk = P.game_max_khz;
		} else {
			g = P.cfg.menu_governor;
		}
	}
	if (!g || !g[0])
		return;
	if (P.gov_valid && !strcmp(P.gov_applied, g) && P.max_applied == maxk)
		return;
	if (!psys_path(dir, sizeof(dir), "%s/devices/system/cpu/cpufreq", P.cfg.sysfs))
		return;
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "policy", 6))
			continue;
		if (psys_path(pol, sizeof(pol), "%s/%s", dir, de->d_name))
			policy_apply(pol, g, maxk, sleep);
	}
	closedir(d);
	snprintf(P.gov_applied, sizeof(P.gov_applied), "%s", g);
	P.max_applied = maxk;
	P.gov_valid = true;
	plog(3, "cpufreq: %s max %d", g, maxk);
}

static void screen_set(enum power_screen s)
{
	if (P.st.screen == s)
		return;
	switch (s) {
	case POWER_SCREEN_ON:
		bl_restore();
		break;
	case POWER_SCREEN_DIM:
		if (P.bl_off)
			bl_power(true);
		bl_dim(P.st.mode == POWER_MODE_CHARGE ? P.cfg.charge_dim_percent : P.cfg.dim_percent);
		break;
	case POWER_SCREEN_OFF:
		bl_power(false);
		break;
	}
	P.st.screen = s;
	if (P.cfg.cb.on_screen)
		P.cfg.cb.on_screen(s, P.cfg.cb.user);
}

/* ------------------------------------------------------------ actions */
static void wake(void)
{
	int64_t t = now_ms();

	P.st.mode = POWER_MODE_NORMAL;
	cpu1_set(true);
	gov_apply();
	led_set(false);
	bl_restore();
	P.st.screen = POWER_SCREEN_ON;
	P.last_activity = t;
	P.swallow = true;
	plog(2, "wake after %lld s", (long long)((t - P.sleep_since) / 1000));
	if (P.cfg.cb.on_sleep_request)
		P.cfg.cb.on_sleep_request(false, P.cfg.cb.user);
}

int power_sleep(bool enter)
{
	if (!P.inited)
		return -ENODEV;
	if (!enter) {
		if (P.st.mode != POWER_MODE_SLEEP)
			return -EINVAL;
		wake();
		return 0;
	}
	if (P.st.mode != POWER_MODE_NORMAL)
		return -EBUSY;
	P.st.mode = POWER_MODE_SLEEP;
	P.sleep_since = now_ms();
	bl_power(false);
	P.st.screen = POWER_SCREEN_OFF;
	gov_apply();
	led_set(true);
	cpu1_set(false);
	plog(2, "sleep (auto power-off in %d min)", P.cfg.sleep_timeout_min);
	if (P.cfg.cb.on_sleep_request)
		P.cfg.cb.on_sleep_request(true, P.cfg.cb.user);
	return 0;
}

void power_request_shutdown(enum power_reason why)
{
	if (!P.inited || P.st.mode == POWER_MODE_SHUTTING_DOWN)
		return;
	P.idle_warned = false;   /* "Powering off..." replaces the notice */
	cpu1_set(true);
	P.st.mode = POWER_MODE_SHUTTING_DOWN;
	P.shutdown_why = why;
	P.shutdown_deadline = now_ms() + (why == POWER_REASON_CRITICAL ?
					  P.cfg.critical_grace_ms : P.cfg.shutdown_grace_ms);
	gov_apply();
	plog(2, "shutdown requested: %s", power_reason_name(why));
	if (P.cfg.cb.on_shutdown_request)
		P.cfg.cb.on_shutdown_request(why, P.cfg.cb.user);
	else
		power_poweroff(why == POWER_REASON_REBOOT);
}

/* The wall clock for the next boot (no RTC backup cell, docs/power.md). */
static void save_clock_now(void)
{
	time_t t = wall_now();
	int e;

	if (!pclock_sane(t))
		return;
	e = pclock_save(P.cfg.clock_file, t);
	if (e)
		plog(1, "saving %s: %s", P.cfg.clock_file, strerror(-e));
}

int power_poweroff(bool do_reboot)
{
	int e;

	if (!P.inited)
		return -ENODEV;
	if (P.poweroff_started)
		return 0;
	P.poweroff_started = true;
	P.poweroff_reboot = do_reboot;
	P.poweroff_at = now_ms();
	P.st.mode = POWER_MODE_SHUTTING_DOWN;
	plog(2, "%s", do_reboot ? "reboot" : "power off");
	if (P.cfg.do_poweroff) {
		/* no rcK behind the hook (tests, headless): save the clock here */
		save_clock_now();
		return P.cfg.do_poweroff(do_reboot, P.cfg.cb.user);
	}
	/* BusyBox init: SIGUSR2 = poweroff, SIGTERM = reboot. It runs rcK,
	 * which saves the clock (rsos-clock save), syncs and unmounts /data:
	 * nothing of that here, it would only delay the signal. */
	if (kill(1, do_reboot ? SIGTERM : SIGUSR2) < 0) {
		e = -errno;
		plog(0, "signalling init: %s, calling reboot(2)", strerror(-e));
		save_clock_now();
		sync();
		reboot(do_reboot ? RB_AUTOBOOT : RB_POWER_OFF);
		return e;
	}
	return 0;
}

static void charge_screen_on(void)
{
	if (P.st.screen == POWER_SCREEN_OFF)
		screen_set(POWER_SCREEN_DIM);
	P.charge_screen_until = now_ms() + (int64_t)P.cfg.charge_screen_s * 1000;
}

static void charge_enter(void)
{
	P.st.mode = POWER_MODE_CHARGE;
	P.st.screen = POWER_SCREEN_ON;
	gov_apply();
	screen_set(POWER_SCREEN_DIM);
	P.charge_screen_until = now_ms() + (int64_t)P.cfg.charge_screen_s * 1000;
	/* unplugged between the boot and now: power off again */
	if (!P.st.charger_online)
		P.charger_lost_at = now_ms();
	P.next_sample = now_ms() + P.cfg.poll_low_ms;
	plog(2, "charge mode (booted by the charger)");
}

static void charge_exit(void)
{
	char p[PSYS_PATH_MAX];

	/* a frontend restart must not come back to charge mode */
	if (psys_path(p, sizeof(p), "%s/bootreason", P.cfg.run_dir))
		psys_write_atomic(p, "key\n", 4);
	P.st.mode = POWER_MODE_NORMAL;
	P.charger_lost_at = 0;
	gov_apply();
	screen_set(POWER_SCREEN_ON);
	P.last_activity = now_ms();
	P.swallow = true;
	/* The press that left charge mode must not power off right away if it
	 * bounces or is tapped twice. */
	P.pek_quiet_until = now_ms() + P.cfg.pek_quiet_ms;
	plog(2, "charge mode -> normal boot");
	if (P.cfg.cb.on_charge_exit)
		P.cfg.cb.on_charge_exit(P.cfg.cb.user);
}

/* ------------------------------------------------------ idle power-off */
/*
 * dim (idle_dim_s) -> screen off (idle_off_s) -> power off (idle_poweroff_s),
 * docs/power.md "Idle power-off". A stage at or after the power-off never
 * happens (equal timers: straight to the power-off). The power-off also
 * counts in a game (the dim and screen-off stages do not), never in charge
 * mode, never while a long job runs (power_set_busy()); its countdown starts
 * at the last input or at the end of the last long job. idle_warn_s before
 * it, on_idle_poweroff(WARN) puts a notice up (the screen is lit again);
 * any input cancels it (and is swallowed).
 */
static int64_t idle_since(void)
{
	return P.busy_end > P.last_activity ? P.busy_end : P.last_activity;
}

static bool poweroff_armed(void)
{
	return P.cfg.idle_poweroff_s > 0 && !P.busy && P.st.mode == POWER_MODE_NORMAL;
}

static int64_t poweroff_due(void)
{
	return idle_since() + (int64_t)P.cfg.idle_poweroff_s * 1000;
}

static int64_t warn_due(void)
{
	int64_t w = poweroff_due() - (int64_t)P.cfg.idle_warn_s * 1000;

	return w > idle_since() ? w : idle_since();
}

/* A stage of s seconds that comes before the power-off. */
static bool before_poweroff(int s)
{
	return s > 0 && (!P.cfg.idle_poweroff_s || s < P.cfg.idle_poweroff_s);
}

static void idle_cancel(const char *why)
{
	if (!P.idle_warned)
		return;
	P.idle_warned = false;
	plog(2, "idle power-off cancelled (%s)", why);
	if (P.cfg.cb.on_idle_poweroff)
		P.cfg.cb.on_idle_poweroff(POWER_IDLE_CANCEL, 0, P.cfg.cb.user);
}

/* Any key (not the power key) or a UI input event. */
static void activity(void)
{
	switch (P.st.mode) {
	case POWER_MODE_SHUTTING_DOWN:
		return;
	case POWER_MODE_SLEEP:
		if (P.cfg.sleep_wake_any)
			wake();
		return;
	case POWER_MODE_CHARGE:
		charge_screen_on();
		P.swallow = true;
		return;
	case POWER_MODE_NORMAL:
		P.last_activity = now_ms();
		if (P.idle_warned) {
			/* the press only cancels the power-off */
			idle_cancel("input");
			P.swallow = true;
		}
		if (P.st.screen != POWER_SCREEN_ON) {
			screen_set(POWER_SCREEN_ON);
			P.swallow = true;
		}
		return;
	}
}

/*
 * Short press (released before long_press_ms). There is no sleep mode any
 * more (docs/power.md §5): in the menu or in a game it powers off, through
 * the same save path as the long press and the critical battery.
 */
static void pek_short(void)
{
	switch (P.st.mode) {
	case POWER_MODE_NORMAL:
		plog(2, "power key short press: powering off");
		power_request_shutdown(POWER_REASON_USER);
		break;
	case POWER_MODE_CHARGE:
		charge_exit();
		break;
	default:
		break;
	}
}

static void key_event(int code, int value, bool pek)
{
	int64_t t = now_ms();

	if (value == 2)
		return;
	if (pek || code == KEY_POWER) {
		if (value == 1) {
			/*
			 * Start-up guard and debounce: a press this early is the one
			 * that turned the unit on (or left charge mode). Its release
			 * finds pek_down_at = 0 and is ignored too.
			 */
			if (t < P.pek_quiet_until) {
				plog(2, "power key press ignored (%lld ms early)",
				     (long long)(P.pek_quiet_until - t));
				P.pek_down_at = 0;
				return;
			}
			P.pek_down_at = t;
			P.pek_long_fired = false;
			P.pek_press_woke = false;
			if (P.st.mode == POWER_MODE_NORMAL && P.idle_warned) {
				/* "press any button to cancel": the power key too
				 * (its release must not power off) */
				idle_cancel("power key");
				P.last_activity = t;
				P.pek_press_woke = true;
			} else if (P.st.mode == POWER_MODE_SLEEP) {
				wake();
				P.pek_press_woke = true;
			} else if (P.st.mode == POWER_MODE_NORMAL &&
				   P.st.screen == POWER_SCREEN_OFF) {
				/* A black screen: the press only turns it on (a dimmed
				 * menu is visible, so there the press powers off). */
				screen_set(POWER_SCREEN_ON);
				P.last_activity = t;
				P.pek_press_woke = true;
			} else if (P.st.mode == POWER_MODE_NORMAL &&
				   P.st.screen == POWER_SCREEN_DIM) {
				screen_set(POWER_SCREEN_ON);
				P.last_activity = t;
			}
		} else {
			if (P.pek_down_at && !P.pek_long_fired && !P.pek_press_woke)
				pek_short();
			P.pek_down_at = 0;
		}
		return;
	}
	if (value == 1) {
		/*
		 * A new press once every built-in key has been released for
		 * SWALLOW_RELEASE_MS ends the wake-key swallow. It must be
		 * decided here, before the press is counted: the UI only calls
		 * power_on_input() for input events, and every built-in event
		 * reaches this module first (its own evdev fds), so at that
		 * point a key is always down or was just released, and a
		 * latch cleared only from power_on_input() never clears
		 * (the "stuck after wake" bug).
		 */
		if (P.swallow && P.keys_down == 0 && t - P.last_release >= SWALLOW_RELEASE_MS)
			P.swallow = false;
		P.keys_down++;
		activity();
	} else {
		if (P.keys_down > 0)
			P.keys_down--;
		P.last_release = t;
	}
}

void power_inject_key(int code, int value)
{
	if (P.inited)
		key_event(code, value, code == KEY_POWER);
}

/* --------------------------------------------------------------- input */
/* The board's built-in keys ("RetroStone2..."; none when the prefix is ""). */
static bool is_builtin(const char *name)
{
	const char *p = P.cfg.builtin_prefix;

	return p && *p && !strncmp(name, p, strlen(p));
}

/*
 * The power key: the board's device by name (axp20x-pek), none (""), or,
 * without a name, any device that reports KEY_POWER and has at most 4 keys
 * (a power button: gpio-keys "pwr_button", ACPI "Power Button"; never a
 * keyboard) and is not one of the built-in keys.
 */
static bool is_power_key(int fd, const char *name)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(unsigned long)) + 1];
	int nkeys = 0;

	if (P.cfg.pek_name)
		return *P.cfg.pek_name && !strcmp(name, P.cfg.pek_name);
	if (is_builtin(name))
		return false;
	memset(bits, 0, sizeof(bits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
		return false;
	for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
		nkeys += __builtin_popcountl(bits[i]);
	return (bits[KEY_POWER / (8 * sizeof(unsigned long))] >> (KEY_POWER % (8 * sizeof(unsigned long))) & 1) &&
	       nkeys <= 4;
}

static void open_input(void)
{
	char path[PSYS_PATH_MAX], name[80];
	struct dirent *de;
	DIR *d;

	d = opendir(P.cfg.dev_input);
	if (!d)
		return;
	while ((de = readdir(d)) && P.nev < MAX_EVDEV) {
		struct epoll_event ee = { .events = EPOLLIN };
		bool pek;
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		if (!psys_path(path, sizeof(path), "%s/%s", P.cfg.dev_input, de->d_name))
			continue;
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(name, 0, sizeof(name));
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
			close(fd);
			continue;
		}
		pek = is_power_key(fd, name);
		if (!pek && !is_builtin(name)) {
			close(fd);
			continue;
		}
		P.ev[P.nev].fd = fd;
		P.ev[P.nev].pek = pek;
		ee.data.fd = fd;
		if (P.epfd >= 0)
			epoll_ctl(P.epfd, EPOLL_CTL_ADD, fd, &ee);
		P.nev++;
		plog(3, "input: %s (%s)", name, path);
	}
	closedir(d);
}

/*
 * After an evdev overflow (SYN_DROPPED) releases may have been lost: count
 * the built-in keys that are really down, so keys_down (and with it the
 * wake-key swallow) can never stay stuck.
 */
static void resync_keys(void)
{
	int i, n = 0;

	for (i = 0; i < P.nev; i++) {
		unsigned char bits[KEY_MAX / 8 + 1];
		size_t k;

		memset(bits, 0, sizeof(bits));
		if (ioctl(P.ev[i].fd, EVIOCGKEY(sizeof(bits)), bits) < 0)
			continue;
		if (P.ev[i].pek) {
			/* a lost power-key release must not become a long press */
			if (!(bits[KEY_POWER / 8] & (1u << (KEY_POWER % 8))))
				P.pek_down_at = 0;
			continue;
		}
		for (k = 0; k < sizeof(bits); k++)
			n += __builtin_popcount(bits[k]);
	}
	plog(1, "input events dropped: %d built-in keys down (was %d)", n, P.keys_down);
	P.keys_down = n;
	if (!n)
		P.last_release = now_ms();
}

static void drain_input(void)
{
	struct input_event buf[32];
	int i;

	for (i = 0; i < P.nev; i++) {
		for (;;) {
			ssize_t r = read(P.ev[i].fd, buf, sizeof(buf));
			int k, n;

			if (r <= 0)
				break;
			n = (int)(r / (ssize_t)sizeof(buf[0]));
			for (k = 0; k < n; k++) {
				if (buf[k].type == EV_SYN && buf[k].code == SYN_DROPPED) {
					P.ev[i].dropped = true;
				} else if (P.ev[i].dropped) {
					if (buf[k].type == EV_SYN && buf[k].code == SYN_REPORT) {
						P.ev[i].dropped = false;
						resync_keys();
					}
				} else if (buf[k].type == EV_KEY) {
					key_event(buf[k].code, buf[k].value, P.ev[i].pek);
				}
			}
		}
	}
}

static void drain_fds(void)
{
	static struct uevent ev;
	uint64_t exp;

	if (P.tfd >= 0)
		while (read(P.tfd, &exp, sizeof(exp)) > 0)
			;
	if (P.ufd >= 0) {
		while (uevent_read(P.ufd, &ev) > 0)
			if (ev.subsystem && !strcmp(ev.subsystem, "power_supply"))
				P.sample_now = true;
	}
	drain_input();
}

/* ------------------------------------------------------------ sampling */
static void thermal_sample(void)
{
	long t;
	bool hot = P.st.hot;

	if (!P.thermal[0] || rd_long(P.thermal, "temp", &t)) {
		/* sun4i-ts returns -EAGAIN until its first conversion */
		P.st.temp_mc = POWER_TEMP_UNKNOWN;
		return;
	}
	P.st.temp_mc = (int)t;
	if (!hot && t >= P.cfg.thermal_warn_mc)
		hot = true;
	else if (hot && t < P.cfg.thermal_warn_mc - P.cfg.thermal_hyst_mc)
		hot = false;
	if (hot != P.st.hot) {
		P.st.hot = hot;
		plog(hot ? 1 : 2, "CPU %s: %ld mC", hot ? "hot" : "cooled down", t);
		if (P.cfg.cb.on_thermal)
			P.cfg.cb.on_thermal((int)t, hot, P.cfg.cb.user);
	}
	if (P.cfg.thermal_crit_mc && t >= P.cfg.thermal_crit_mc)
		P.thermal_crit_count++;
	else
		P.thermal_crit_count = 0;
	if (P.thermal_crit_count >= THERMAL_CRIT_SAMPLES &&
	    P.st.mode != POWER_MODE_SHUTTING_DOWN) {
		plog(0, "CPU at %ld mC: shutting down", t);
		power_request_shutdown(POWER_REASON_THERMAL);
	}
}

static void sample(void)
{
	struct power_status old = P.st;
	struct batt_sample s;
	struct batt_out o;
	char status[32];
	long v;
	int64_t t = now_ms();
	bool low;
	int period;

	memset(&s, 0, sizeof(s));
	s.t_ms = t;
	s.axp_pct = -1;
	if (P.bat[0]) {
		s.present = rd_long(P.bat, "present", &v) ? true : v != 0;
		if (!rd_str(P.bat, "status", status, sizeof(status)))
			s.state = batt_state_parse(status);
		if (!rd_long(P.bat, "voltage_now", &v))
			s.mv = (int)(v / 1000);
		if (!rd_long(P.bat, "current_now", &v)) {
			s.ma = (int)(v / 1000);
			s.ma_valid = true;
		}
		if (!rd_long(P.bat, "capacity", &v))
			s.axp_pct = (int)v;
	}
	if (!rd_long(P.ac, "online", &v) && v)
		s.charger = true;
	if (!rd_long(P.usb, "online", &v) && v)
		s.charger = true;

	batt_update(&P.batt, &s, &o);

	P.st.valid = P.bat[0] && s.present && s.mv > 0;
	P.st.battery_present = s.present;
	P.st.charger_online = s.charger;
	P.st.state = s.state;
	P.st.percent = o.percent;
	P.st.percent_axp = s.axp_pct;
	P.st.percent_voltage = o.pct_voltage;
	P.st.voltage_mv = s.mv;
	P.st.voltage_filtered_mv = o.mv_filtered;
	P.st.current_ma = o.ma_avg;
	P.st.minutes_left = o.minutes_left;
	P.st.level = (enum power_level)o.level;

	thermal_sample();

	/* charger plugged while the menu is dimmed: show the screen */
	if (s.charger && !old.charger_online && P.st.mode == POWER_MODE_NORMAL) {
		P.last_activity = t;
		if (P.st.screen != POWER_SCREEN_ON)
			screen_set(POWER_SCREEN_ON);
	}
	if (P.st.mode == POWER_MODE_CHARGE) {
		if (!s.charger) {
			if (!P.charger_lost_at)
				P.charger_lost_at = t;
		} else {
			P.charger_lost_at = 0;
		}
	}

	if (P.cfg.cb.on_status &&
	    (old.valid != P.st.valid || old.percent != P.st.percent ||
	     old.charger_online != P.st.charger_online || old.state != P.st.state ||
	     old.level != P.st.level || old.battery_present != P.st.battery_present))
		P.cfg.cb.on_status(&P.st, P.cfg.cb.user);

	if (o.level_changed && o.level != BATT_LEVEL_CRITICAL) {
		plog(2, "battery level %d (%d %%, %d mV)", o.level, o.percent, o.mv_filtered);
		P.last_warn_at = t;
		if (P.cfg.cb.on_warning)
			P.cfg.cb.on_warning((enum power_level)o.level, &P.st, P.cfg.cb.user);
	} else if (o.level == BATT_LEVEL_VERY_LOW && P.cfg.warn_repeat_s > 0 &&
		   t - P.last_warn_at >= (int64_t)P.cfg.warn_repeat_s * 1000) {
		P.last_warn_at = t;
		if (P.cfg.cb.on_warning)
			P.cfg.cb.on_warning(POWER_LEVEL_VERY_LOW, &P.st, P.cfg.cb.user);
	}

	if (o.critical_now && P.st.mode != POWER_MODE_SHUTTING_DOWN) {
		plog(0, "battery critical: %d mV filtered (%d mV now), %d %%",
		     o.mv_filtered, s.mv, o.percent);
		if (P.cfg.cb.on_critical)
			P.cfg.cb.on_critical(&P.st, P.cfg.cb.user);
		power_request_shutdown(POWER_REASON_CRITICAL);
	}

	low = !s.charger && o.mv_filtered > 0 && o.mv_filtered < P.cfg.low_poll_mv;
	if (low || P.st.mode == POWER_MODE_CHARGE)
		period = P.cfg.poll_low_ms;
	else if (P.st.mode == POWER_MODE_SLEEP)
		period = P.cfg.poll_sleep_ms;
	else
		period = P.cfg.poll_ms;
	P.next_sample = t + period;
	P.sample_now = false;
}

/* -------------------------------------------------------------- timers */
static int64_t next_deadline(void)
{
	int64_t d = P.next_sample;

	if (P.pek_down_at && !P.pek_long_fired)
		d = min64(d, P.pek_down_at + P.cfg.long_press_ms);
	switch (P.st.mode) {
	case POWER_MODE_NORMAL:
		if (!P.st.game_running && !P.idle_warned) {
			if (before_poweroff(P.cfg.idle_dim_s) && P.st.screen == POWER_SCREEN_ON)
				d = min64(d, P.last_activity + (int64_t)P.cfg.idle_dim_s * 1000);
			if (before_poweroff(P.cfg.idle_off_s) && P.st.screen != POWER_SCREEN_OFF)
				d = min64(d, P.last_activity + (int64_t)P.cfg.idle_off_s * 1000);
		}
		if (poweroff_armed())
			d = min64(d, P.idle_warned || P.cfg.idle_warn_s <= 0 ? poweroff_due() : warn_due());
		break;
	case POWER_MODE_SLEEP:
		if (P.cfg.sleep_timeout_min)
			d = min64(d, P.sleep_since + (int64_t)P.cfg.sleep_timeout_min * 60000);
		break;
	case POWER_MODE_CHARGE:
		if (P.st.screen != POWER_SCREEN_OFF)
			d = min64(d, P.charge_screen_until);
		if (P.charger_lost_at)
			d = min64(d, P.charger_lost_at + P.cfg.charge_removed_ms);
		break;
	case POWER_MODE_SHUTTING_DOWN:
		if (!P.poweroff_started)
			d = min64(d, P.shutdown_deadline);
		else if (!P.cfg.do_poweroff)
			d = min64(d, P.poweroff_at + P.cfg.poweroff_fallback_ms);
		break;
	}
	if (P.cfg.clock_save_s)
		d = min64(d, P.next_clock_save);
	return d;
}

static void run_timers(void)
{
	int64_t t = now_ms();

	if (P.pek_down_at && !P.pek_long_fired && t - P.pek_down_at >= P.cfg.long_press_ms) {
		P.pek_long_fired = true;
		if (P.st.mode != POWER_MODE_SHUTTING_DOWN) {
			plog(2, "power key long press");
			power_request_shutdown(POWER_REASON_USER);
		}
	}

	if (P.sample_now || t >= P.next_sample)
		sample();

	switch (P.st.mode) {
	case POWER_MODE_NORMAL:
		if (poweroff_armed()) {
			if (t >= poweroff_due()) {
				plog(2, "no input for %d s: saving and powering off", P.cfg.idle_poweroff_s);
				P.idle_warned = false;
				power_request_shutdown(POWER_REASON_IDLE);
				break;
			}
			if (!P.idle_warned && P.cfg.idle_warn_s > 0 && t >= warn_due()) {
				int left = (int)((poweroff_due() - t + 999) / 1000);

				P.idle_warned = true;
				plog(2, "idle: powering off in %d s unless a button is pressed", left);
				/* the notice must be seen: light the screen again */
				if (P.st.screen != POWER_SCREEN_ON)
					screen_set(POWER_SCREEN_ON);
				if (P.cfg.cb.on_idle_poweroff)
					P.cfg.cb.on_idle_poweroff(POWER_IDLE_WARN, left, P.cfg.cb.user);
			}
		}
		if (!P.st.game_running && !P.idle_warned) {
			int64_t idle = t - P.last_activity;
			if (before_poweroff(P.cfg.idle_off_s) && idle >= (int64_t)P.cfg.idle_off_s * 1000)
				screen_set(POWER_SCREEN_OFF);
			else if (before_poweroff(P.cfg.idle_dim_s) && idle >= (int64_t)P.cfg.idle_dim_s * 1000 &&
				 P.st.screen == POWER_SCREEN_ON)
				screen_set(POWER_SCREEN_DIM);
		}
		break;
	case POWER_MODE_SLEEP:
		if (P.cfg.sleep_timeout_min &&
		    t - P.sleep_since >= (int64_t)P.cfg.sleep_timeout_min * 60000) {
			plog(2, "asleep for %d min: saving and powering off", P.cfg.sleep_timeout_min);
			power_request_shutdown(POWER_REASON_SLEEP_TIMEOUT);
		}
		break;
	case POWER_MODE_CHARGE:
		if (P.charger_lost_at && t - P.charger_lost_at >= P.cfg.charge_removed_ms) {
			P.charger_lost_at = 0;
			power_request_shutdown(POWER_REASON_CHARGER_REMOVED);
		} else if (P.st.screen != POWER_SCREEN_OFF && t >= P.charge_screen_until) {
			screen_set(POWER_SCREEN_OFF);
		}
		break;
	case POWER_MODE_SHUTTING_DOWN:
		if (!P.poweroff_started && t >= P.shutdown_deadline && P.shutdown_why == POWER_REASON_IDLE) {
			/* never cut the power without saving for an idle power-off */
			plog(0, "idle power-off not completed in time: cancelled, the unit stays on");
			power_cancel_shutdown();
		} else if (!P.poweroff_started && t >= P.shutdown_deadline) {
			plog(0, "shutdown (%s) not completed in time: powering off now",
			     power_reason_name(P.shutdown_why));
			power_poweroff(P.shutdown_why == POWER_REASON_REBOOT);
		} else if (P.poweroff_started && !P.cfg.do_poweroff &&
			   t - P.poweroff_at >= P.cfg.poweroff_fallback_ms) {
			plog(0, "init did not power off: reboot(2)");
			save_clock_now();
			sync();
			reboot(P.poweroff_reboot ? RB_AUTOBOOT : RB_POWER_OFF);
			P.poweroff_at = t;  /* only reached if reboot() failed */
		}
		break;
	}

	if (P.cfg.clock_save_s && t >= P.next_clock_save) {
		time_t w = wall_now();
		if (pclock_sane(w) && P.st.mode != POWER_MODE_SHUTTING_DOWN)
			pclock_save(P.cfg.clock_file, w);
		P.next_clock_save = t + (int64_t)P.cfg.clock_save_s * 1000;
	}
	/* The wake-key swallow latch is cleared lazily by power_on_input(). */
}

static void rearm(void)
{
	struct itimerspec its;
	int64_t delta;

	if (P.tfd < 0)
		return;
	delta = next_deadline() - now_ms();
	if (delta < 1)
		delta = 1;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = delta / 1000;
	its.it_value.tv_nsec = (delta % 1000) * 1000000;
	timerfd_settime(P.tfd, 0, &its, NULL);
}

/* ----------------------------------------------------------------- API */
int power_init(const struct power_config *cfg)
{
	char p[PSYS_PATH_MAX], buf[32];
	struct epoll_event ee = { .events = EPOLLIN };
	int64_t t;

	if (P.inited)
		power_exit();
	memset(&P, 0, sizeof(P));
	P.epfd = P.tfd = P.ufd = -1;
	if (cfg)
		P.cfg = *cfg;
	else
		power_config_defaults(&P.cfg);
	P.saved_brightness = -1;
	P.st.percent = -1;
	P.st.percent_axp = -1;
	P.st.percent_voltage = -1;
	P.st.minutes_left = -1;
	P.st.temp_mc = POWER_TEMP_UNKNOWN;
	batt_init(&P.batt, &P.cfg.batt);

	P.epfd = epoll_create1(EPOLL_CLOEXEC);
	if (P.epfd < 0)
		return -errno;
	P.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (P.tfd >= 0) {
		ee.data.fd = P.tfd;
		epoll_ctl(P.epfd, EPOLL_CTL_ADD, P.tfd, &ee);
	}
	if (!P.cfg.no_uevent) {
		P.ufd = uevent_open();
		if (P.ufd >= 0) {
			ee.data.fd = P.ufd;
			epoll_ctl(P.epfd, EPOLL_CTL_ADD, P.ufd, &ee);
		} else {
			P.ufd = -1;
		}
	}
	P.inited = true;

	find_supplies();
	if (!P.cfg.backlight_name) {
		find_class_entry("class/backlight", NULL, P.bl, sizeof(P.bl));
	} else if (*P.cfg.backlight_name) {
		/* the board's backlight; the first one if it is not there */
		find_class_entry("class/backlight", P.cfg.backlight_name, P.bl, sizeof(P.bl));
		if (!P.bl[0])
			find_class_entry("class/backlight", NULL, P.bl, sizeof(P.bl));
	} else {
		P.bl[0] = '\0';   /* the board has none */
	}
	find_class_entry("class/leds", "status", P.led, sizeof(P.led));
	find_thermal();
	if (!P.bat[0])
		plog(1, "no battery power supply in %s/class/power_supply", P.cfg.sysfs);
	if (P.cfg.own_input)
		open_input();
	setup_pek();
	setup_voff();

	/* boot reason, written by rsos-bootreason in rcS */
	P.st.boot_reason = POWER_BOOT_UNKNOWN;
	if (psys_path(p, sizeof(p), "%s/bootreason", P.cfg.run_dir) &&
	    !psys_read_str(p, buf, sizeof(buf)))
		P.st.boot_reason = bootreason_parse(buf);
	if (psys_path(p, sizeof(p), "%s/clock-restored", P.cfg.run_dir))
		P.st.clock_restored = access(p, F_OK) == 0;

	t = now_ms();
	P.last_activity = t;
	/*
	 * The press that powered the unit on is over long before this runs
	 * (the AXP needs the key held ~1 s, then U-Boot and the kernel boot;
	 * axp20x-pek only reports edges after its probe, and the MFD's
	 * regmap-irq acks the latched PEK edges at init). Still, a key held
	 * until now, or pressed right as the menu appears, is ignored.
	 */
	P.pek_quiet_until = t + P.cfg.pek_start_ignore_ms;
	P.next_clock_save = t + (int64_t)P.cfg.clock_save_s * 1000;
	P.st.mode = POWER_MODE_NORMAL;
	P.st.screen = POWER_SCREEN_ON;
	gov_apply();
	sample();
	if (P.st.boot_reason == POWER_BOOT_CHARGER)
		charge_enter();
	plog(2, "battery %d %% %d mV %s, charger %s, boot %s", P.st.percent, P.st.voltage_mv,
	     batt_state_name(P.st.state), P.st.charger_online ? "on" : "off",
	     bootreason_name(P.st.boot_reason));
	rearm();
	return 0;
}

void power_exit(void)
{
	int i;

	if (!P.inited)
		return;
	for (i = 0; i < P.nev; i++)
		close(P.ev[i].fd);
	if (P.ufd >= 0)
		uevent_close(P.ufd);
	if (P.tfd >= 0)
		close(P.tfd);
	if (P.epfd >= 0)
		close(P.epfd);
	memset(&P, 0, sizeof(P));
	P.epfd = P.tfd = P.ufd = -1;
}

int power_get_fd(void)
{
	return P.inited ? P.epfd : -1;
}

int power_timeout_ms(void)
{
	int64_t d;

	if (!P.inited)
		return -1;
	d = next_deadline() - now_ms();
	if (d < 0)
		return 0;
	return d > INT_MAX ? INT_MAX : (int)d;
}

int power_poll(void)
{
	if (!P.inited)
		return -ENODEV;
	drain_fds();
	run_timers();
	rearm();
	return 0;
}

const struct power_status *power_get_status(void)
{
	return &P.st;
}

enum power_mode power_get_mode(void)
{
	return P.st.mode;
}

void power_set_game_running(bool running)
{
	if (!P.inited || P.st.game_running == running)
		return;
	P.st.game_running = running;
	P.last_activity = now_ms();
	if (P.st.mode == POWER_MODE_NORMAL && P.st.screen != POWER_SCREEN_ON)
		screen_set(POWER_SCREEN_ON);
	gov_apply();
	rearm();
}

void power_set_game_cpu(const char *governor, int max_khz)
{
	snprintf(P.game_gov, sizeof(P.game_gov), "%s", governor ? governor : "");
	P.game_max_khz = max_khz > 0 ? max_khz : 0;
	if (P.inited && P.st.game_running)
		gov_apply();
}

void power_set_docked(bool docked)
{
	if (docked && !P.docked) {
		/* the display layer switched the LCD off: forget our dim state */
		P.saved_brightness = -1;
		P.bl_off = false;
	}
	P.docked = docked;
}

bool power_on_input(void)
{
	int64_t t;

	if (!P.inited)
		return false;
	drain_input();
	t = now_ms();
	switch (P.st.mode) {
	case POWER_MODE_SLEEP:
	case POWER_MODE_SHUTTING_DOWN:
		return true;
	case POWER_MODE_CHARGE:
		activity();
		return true;
	case POWER_MODE_NORMAL:
		break;
	}
	if (P.swallow) {
		if (P.keys_down == 0 && t - P.last_release >= SWALLOW_RELEASE_MS)
			P.swallow = false;
		else
			return true;
	}
	if (P.st.screen != POWER_SCREEN_ON || P.idle_warned) {
		activity();   /* wakes the screen / cancels the idle power-off */
		rearm();
		return true;
	}
	P.last_activity = t;
	return false;
}

void power_notify_activity(void)
{
	if (!P.inited)
		return;
	if (P.st.mode == POWER_MODE_NORMAL) {
		P.last_activity = now_ms();
		idle_cancel("activity");
		if (P.st.screen != POWER_SCREEN_ON)
			screen_set(POWER_SCREEN_ON);
		rearm();
	}
}

bool power_axis_activity(int16_t v, int16_t *ref)
{
	int d = (int)v - (int)*ref;

	if (d > POWER_AXIS_MOVE || d < -POWER_AXIS_MOVE) {
		*ref = v;
		return true;
	}
	return v > POWER_AXIS_DEADZONE || v < -POWER_AXIS_DEADZONE;
}

void power_set_busy(bool busy, const char *why)
{
	if (!P.inited || P.busy == busy)
		return;
	P.busy = busy;
	if (busy) {
		snprintf(P.busy_why, sizeof(P.busy_why), "%s", why ? why : "busy");
		plog(2, "idle power-off held: %s", P.busy_why);
		idle_cancel(P.busy_why);
	} else {
		P.busy_end = now_ms();
		plog(2, "idle power-off: %s ended, the countdown starts over", P.busy_why);
	}
	rearm();
}

int power_cancel_shutdown(void)
{
	if (!P.inited || P.st.mode != POWER_MODE_SHUTTING_DOWN || P.poweroff_started ||
	    P.shutdown_why != POWER_REASON_IDLE)
		return -EINVAL;
	P.st.mode = POWER_MODE_NORMAL;
	P.last_activity = now_ms();
	P.idle_warned = false;
	gov_apply();
	plog(1, "idle power-off cancelled: the unit stays on");
	rearm();
	return 0;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
	char *end;
	long n;

	if (!v || !*v)
		return -EINVAL;
	n = strtol(v, &end, 10);
	if (*end || n < lo || n > hi)
		return -EINVAL;
	*out = (int)n;
	return 0;
}

int power_set_setting(const char *key, const char *value)
{
	int n;

	if (!key || !value)
		return -EINVAL;
	if (!strcmp(key, "sleep_timeout_min")) {
		if (parse_int(value, 0, 240, &n))
			return -EINVAL;
		P.cfg.sleep_timeout_min = n;
	} else if (!strcmp(key, "idle_dim_min")) {
		if (parse_int(value, 0, 60, &n))
			return -EINVAL;
		P.cfg.idle_dim_s = n * 60;
	} else if (!strcmp(key, "idle_off_min")) {
		if (parse_int(value, 0, 120, &n))
			return -EINVAL;
		P.cfg.idle_off_s = n * 60;
	} else if (!strcmp(key, "idle_poweroff_min")) {
		if (parse_int(value, 0, 240, &n))
			return -EINVAL;
		P.cfg.idle_poweroff_s = n * 60;
	} else if (!strcmp(key, "idle_dim_s") || !strcmp(key, "idle_off_s") ||
		   !strcmp(key, "idle_poweroff_s")) {
		/* seconds: development and tests only (the UI never writes them) */
		if (parse_int(value, 0, 7200, &n))
			return -EINVAL;
		if (key[5] == 'd')
			P.cfg.idle_dim_s = n;
		else if (key[5] == 'o')
			P.cfg.idle_off_s = n;
		else
			P.cfg.idle_poweroff_s = n;
	} else if (!strcmp(key, "sleep_wake")) {
		if (!strcmp(value, "any"))
			P.cfg.sleep_wake_any = true;
		else if (!strcmp(value, "power"))
			P.cfg.sleep_wake_any = false;
		else
			return -EINVAL;
	} else if (!strcmp(key, "battery_gauge")) {
		enum batt_gauge g;
		if (!strcmp(value, "auto"))
			g = BATT_GAUGE_AUTO;
		else if (!strcmp(value, "axp"))
			g = BATT_GAUGE_AXP;
		else if (!strcmp(value, "voltage"))
			g = BATT_GAUGE_VOLTAGE;
		else
			return -EINVAL;
		P.cfg.batt.gauge = g;
		P.batt.p.gauge = g;
	} else if (!strcmp(key, "timezone")) {
		return power_set_timezone(value);
	} else {
		return -ENOENT;
	}
	if (P.inited)
		rearm();
	return 0;
}

int power_set_time(time_t t)
{
	char p[PSYS_PATH_MAX];
	int e;

	if (!pclock_sane(t))
		return -EINVAL;
	e = pclock_set_system(t);
	if (e)
		return e;
	if (pclock_rtc_write(P.cfg.rtc_dev ? P.cfg.rtc_dev : PCLOCK_RTC_DEFAULT, t))
		plog(1, "RTC write failed");
	if (P.cfg.clock_file)
		pclock_save(P.cfg.clock_file, t);
	if (P.cfg.run_dir && psys_path(p, sizeof(p), "%s/clock-restored", P.cfg.run_dir))
		unlink(p);
	P.st.clock_restored = false;
	return 0;
}

int power_set_timezone(const char *s)
{
	const char *posix;

	if (!s)
		return -EINVAL;
	posix = pclock_tz_lookup(s);
	if (!posix && pclock_tz_valid(s))
		posix = s;
	if (!posix)
		return -EINVAL;
	if (setenv("TZ", posix, 1))
		return -errno;
	tzset();
	return 0;
}

const struct power_tz *power_timezones(int *count)
{
	return pclock_timezones(count);
}
