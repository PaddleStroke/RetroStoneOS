/*
 * power_test.c - unit tests for the power module, run on the build host.
 *
 *  - battery.c: OCV curve, simulated discharges with noise and spikes,
 *    critical detection (sustained / emergency / single noisy samples),
 *    warning hysteresis, charger re-arm;
 *  - power.c: the whole state machine against a fake sysfs tree and a
 *    fake monotonic clock: PEK timings, V_OFF, governors, idle dimming,
 *    fake sleep and its timeout, long press, watchdog, critical path,
 *    charge mode, thermal, settings;
 *  - pclock.c / bootreason.c: saved clock, restore rule, time zones,
 *    REG00 decode.
 *
 * make -f power.mk power-check
 */
#include "power/power.h"
#include "power/psys.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		fails++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ------------------------------------------------------------ fixtures */
static char root[256];
static int64_t fake_now;
static time_t WALL;   /* build epoch + 30 days, set in main() */

static int64_t fake_ms(void) { return fake_now; }
static time_t fake_wall(void) { return WALL; }

static void mkdirs(const char *path)
{
	char tmp[PSYS_PATH_MAX];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void wfile(const char *rel, const char *content)
{
	char path[PSYS_PATH_MAX], dir[PSYS_PATH_MAX];
	char *slash;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	*slash = '\0';
	mkdirs(dir);
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	fputs(content, f);
	fclose(f);
}

static void wlong(const char *rel, long v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%ld\n", v);
	wfile(rel, buf);
}

static long rlong(const char *rel)
{
	char path[PSYS_PATH_MAX];
	long v = -999;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	psys_read_long(path, &v);
	return v;
}

static const char *rstr(const char *rel)
{
	static char buf[128];
	char path[PSYS_PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if (psys_read_str(path, buf, sizeof(buf)))
		snprintf(buf, sizeof(buf), "<missing>");
	return buf;
}

#define BAT "sys/class/power_supply/axp20x-battery"
#define AC "sys/class/power_supply/axp20x-ac"
#define BL "sys/class/backlight/backlight"
#define POL "sys/devices/system/cpu/cpufreq/policy0"
#define PEK "sys/bus/platform/drivers/axp20x-pek/axp20x-pek"
#define LED "sys/class/leds/blue:status"
#define TZONE "sys/class/thermal/thermal_zone0"

static void set_batt(int mv, int ma, const char *status, int cap)
{
	wlong(BAT "/voltage_now", (long)mv * 1000);
	wlong(BAT "/current_now", (long)ma * 1000);
	wfile(BAT "/status", status);
	wlong(BAT "/capacity", cap);
}

static void make_tree(void)
{
	char cmd[PSYS_PATH_MAX];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (root[0] && system(cmd) != 0)
		exit(2);
	mkdirs(root);
	wfile(BAT "/type", "Battery\n");
	wfile(BAT "/present", "1\n");
	wlong(BAT "/voltage_min", 2900000);
	set_batt(3950, -400, "Discharging\n", 80);
	wfile(AC "/type", "Mains\n");
	wfile(AC "/online", "0\n");
	wfile(BL "/brightness", "50\n");
	wfile(BL "/max_brightness", "100\n");
	wfile(BL "/bl_power", "0\n");
	wfile(POL "/scaling_governor", "performance\n");
	wlong(POL "/scaling_max_freq", 960000);
	wlong(POL "/cpuinfo_max_freq", 960000);
	wlong(POL "/cpuinfo_min_freq", 144000);
	wfile(PEK "/shutdown", "4000\n");
	wfile(PEK "/startup", "1000\n");
	wfile(LED "/brightness", "0\n");
	wfile(TZONE "/type", "cpu-thermal\n");
	wlong(TZONE "/temp", 45000);
	snprintf(cmd, sizeof(cmd), "%s/dev/input", root);
	mkdirs(cmd);
	snprintf(cmd, sizeof(cmd), "%s/run", root);
	mkdirs(cmd);
	snprintf(cmd, sizeof(cmd), "%s/data", root);
	mkdirs(cmd);
}

/* callback recorder */
static struct rec {
	int status, warn, critical, sleep_on, sleep_off, shutdown, screen, thermal, charge_exit;
	enum power_level last_warn;
	enum power_reason last_why;
	enum power_screen last_screen;
	bool last_hot;
	bool call_poweroff;      /* on_shutdown_request calls power_poweroff() */
	int poweroff;
	bool poweroff_reboot;
	int idle_warn, idle_cancel, idle_secs;
} R;

static void cb_status(const struct power_status *st, void *u) { (void)st; (void)u; R.status++; }
static void cb_warn(enum power_level l, const struct power_status *st, void *u)
{ (void)st; (void)u; R.warn++; R.last_warn = l; }
static void cb_crit(const struct power_status *st, void *u) { (void)st; (void)u; R.critical++; }
static void cb_sleep(bool enter, void *u) { (void)u; if (enter) R.sleep_on++; else R.sleep_off++; }
static void cb_shutdown(enum power_reason why, void *u)
{
	(void)u;
	R.shutdown++;
	R.last_why = why;
	if (R.call_poweroff)
		power_poweroff(why == POWER_REASON_REBOOT);
}
static void cb_screen(enum power_screen s, void *u) { (void)u; R.screen++; R.last_screen = s; }
static void cb_thermal(int t, bool hot, void *u) { (void)t; (void)u; R.thermal++; R.last_hot = hot; }
static void cb_charge_exit(void *u) { (void)u; R.charge_exit++; }
static void cb_log(int level, const char *msg, void *u)
{
	(void)u;
	if (getenv("POWER_TEST_VERBOSE"))
		printf("    log%d %s\n", level, msg);
}
static void cb_idle(enum power_idle_event ev, int s, void *u)
{
	(void)u;
	if (ev == POWER_IDLE_WARN) {
		R.idle_warn++;
		R.idle_secs = s;
	} else {
		R.idle_cancel++;
	}
}
static int hook_poweroff(bool reboot, void *u) { (void)u; R.poweroff++; R.poweroff_reboot = reboot; return 0; }

static void init_power(void)
{
	static char sysfs[PSYS_PATH_MAX], dev[PSYS_PATH_MAX], run[PSYS_PATH_MAX], clk[PSYS_PATH_MAX];
	struct power_config c;
	int ret;

	memset(&R, 0, sizeof(R));
	R.call_poweroff = true;
	snprintf(sysfs, sizeof(sysfs), "%s/sys", root);
	snprintf(dev, sizeof(dev), "%s/dev/input", root);
	snprintf(run, sizeof(run), "%s/run", root);
	snprintf(clk, sizeof(clk), "%s/data/lastclock", root);
	power_config_defaults(&c);
	c.sysfs = sysfs;
	c.dev_input = dev;
	c.run_dir = run;
	c.clock_file = clk;
	c.rtc_dev = "/nonexistent-rtc";
	c.now_ms = fake_ms;
	c.wall_now = fake_wall;
	c.do_poweroff = hook_poweroff;
	c.no_uevent = true;
	c.cb.on_status = cb_status;
	c.cb.on_warning = cb_warn;
	c.cb.on_critical = cb_crit;
	c.cb.on_sleep_request = cb_sleep;
	c.cb.on_shutdown_request = cb_shutdown;
	c.cb.on_screen = cb_screen;
	c.cb.on_thermal = cb_thermal;
	c.cb.on_charge_exit = cb_charge_exit;
	c.cb.log = cb_log;
	c.cb.on_idle_poweroff = cb_idle;
	/* the idle power-off is off in the older tests (their clocks run for
	 * hours with nobody pressing anything): test_idle_poweroff() turns it on */
	c.idle_poweroff_s = 0;
	ret = power_init(&c);
	CHECK(ret == 0, "power_init %d", ret);
}

/* advance the fake clock in steps, polling like a main loop would */
static void advance(int64_t ms, int step)
{
	int64_t end = fake_now + ms;

	while (fake_now < end) {
		int64_t s = step;
		if (fake_now + s > end)
			s = end - fake_now;
		fake_now += s;
		power_poll();
	}
}

/* --------------------------------------------------------- battery.c */
static int pct_to_ocv(int pct)
{
	int mv;

	for (mv = 3000; mv <= 4200; mv++)
		if (batt_ocv_to_pct(mv) >= pct)
			return mv;
	return 4200;
}

static unsigned lcg = 12345;
static int noise(int amp)
{
	lcg = lcg * 1103515245u + 12345u;
	return (int)((lcg >> 16) % (unsigned)(2 * amp + 1)) - amp;
}

static void test_ocv(void)
{
	int mv, prev = -1, bad = 0;

	for (mv = 2800; mv <= 4400; mv += 5) {
		int p = batt_ocv_to_pct(mv);
		if (p < prev || p < 0 || p > 100)
			bad++;
		prev = p;
	}
	CHECK(bad == 0, "curve not monotonic (%d)", bad);
	CHECK(batt_ocv_to_pct(2900) == 0, "%d", batt_ocv_to_pct(2900));
	CHECK(batt_ocv_to_pct(4250) == 100, "%d", batt_ocv_to_pct(4250));
	CHECK(batt_ocv_to_pct(3450) == 3, "%d", batt_ocv_to_pct(3450));
	CHECK(batt_ocv_to_pct(3720) == 25, "%d", batt_ocv_to_pct(3720));
	CHECK(batt_state_parse("Charging") == BATT_CHARGING, "parse");
	CHECK(batt_state_parse("Not charging") == BATT_NOT_CHARGING, "parse");
	CHECK(batt_state_parse("x") == BATT_UNKNOWN, "parse");
}

/*
 * A full discharge at 500 mA (8 h for 4000 mAh), sampled like the module
 * does (10 s, 2 s under 3.65 V), with +-30 mV noise and a -250 mV spike
 * every 37th sample. Checks: after the settling window the shown % never
 * rises; LOW and VERY_LOW fire exactly once; critical fires exactly once,
 * never while the true loaded voltage is above the hysteresis band
 * (crit + hyst), and at most 30 s after it went below crit_mv.
 */
static void test_discharge(enum batt_gauge gauge, bool axp_bogus)
{
	struct batt_params p;
	struct batt b;
	struct batt_out o;
	struct batt_sample s;
	double pct = 100.0;
	int64_t t = 0, crit_at = -1, below_at = -1, near_at = -1;
	int shown_prev = 101, rises = 0, n_low = 0, n_vlow = 0, n_crit = 0, early = 0, i = 0;
	const int ma = -500;

	batt_params_defaults(&p);
	p.gauge = gauge;
	batt_init(&b, &p);
	while (pct > 0.0 && t < 12LL * 3600 * 1000) {
		int ocv = pct_to_ocv((int)(pct + 0.5));
		int loaded = ocv + ma * p.r_int_mohm / 1000;
		int mv = loaded + noise(30);
		int period;

		if (++i % 37 == 0)
			mv -= 250;
		memset(&s, 0, sizeof(s));
		s.t_ms = t;
		s.present = true;
		s.state = BATT_DISCHARGING;
		s.mv = mv;
		s.ma = ma + noise(40);
		s.ma_valid = true;
		/* a bogus gauge stuck at 100 % must be ignored by AUTO */
		s.axp_pct = axp_bogus ? 100 : (int)(pct + 0.5);
		batt_update(&b, &s, &o);

		if (o.percent > shown_prev && t >= BATT_SETTLE_MS) {
			rises++;
			if (getenv("POWER_TEST_VERBOSE"))
				printf("    rise t=%lld true=%.1f shown %d->%d pv=%d mvf=%d axp=%d\n",
				       (long long)t, pct, shown_prev, o.percent, o.pct_voltage,
				       o.mv_filtered, s.axp_pct);
		}
		shown_prev = o.percent;
		if (o.level_changed && o.level == BATT_LEVEL_LOW)
			n_low++;
		if (o.level_changed && o.level == BATT_LEVEL_VERY_LOW)
			n_vlow++;
		if (loaded < p.crit_mv + p.crit_hyst_mv && near_at < 0)
			near_at = t;
		if (loaded < p.crit_mv && below_at < 0)
			below_at = t;
		if (o.critical_now) {
			n_crit++;
			crit_at = t;
			if (loaded >= p.crit_mv + p.crit_hyst_mv + 10)
				early++;
		}
		if (n_crit)
			break;
		period = o.mv_filtered < 3650 ? 2000 : 10000;
		t += period;
		pct -= 100.0 * (-ma) * (period / 3600000.0) / p.capacity_mah;
	}
	CHECK(rises == 0, "gauge %d: shown %% rose %d times while discharging", gauge, rises);
	CHECK(n_low == 1, "gauge %d: LOW fired %d times", gauge, n_low);
	CHECK(n_vlow == 1, "gauge %d: VERY_LOW fired %d times", gauge, n_vlow);
	CHECK(n_crit == 1, "gauge %d: critical fired %d times", gauge, n_crit);
	/* the countdown may run in the hysteresis band (crit..crit+hyst) with
	 * +-30 mV noise, never above it */
	CHECK(early == 0 && near_at >= 0 && crit_at >= near_at,
	      "gauge %d: critical fired too early (near at %lld, crit at %lld)", gauge,
	      (long long)near_at, (long long)crit_at);
	CHECK(below_at < 0 || crit_at - below_at <= 30000,
	      "gauge %d: critical %lld ms after the true voltage crossed", gauge,
	      (long long)(crit_at - below_at));
	if (axp_bogus)
		CHECK(o.percent <= 10, "bogus AXP gauge not rejected: %d %%", o.percent);
}

static struct batt_out feed(struct batt *b, int64_t t, int mv, bool charger)
{
	struct batt_sample s;
	struct batt_out o;

	memset(&s, 0, sizeof(s));
	s.t_ms = t;
	s.present = true;
	s.charger = charger;
	s.state = charger ? BATT_CHARGING : BATT_DISCHARGING;
	s.mv = mv;
	s.ma = charger ? 800 : -500;
	s.ma_valid = true;
	s.axp_pct = -1;
	batt_update(b, &s, &o);
	return o;
}

static void test_noise_rejection(void)
{
	struct batt b;
	struct batt_out o;
	int64_t t = 0;
	int i, crit = 0;

	/* isolated deep spikes (single and double) at a healthy 3.60 V */
	batt_init(&b, NULL);
	for (i = 0; i < 200; i++, t += 2000) {
		int mv = 3600;
		if (i % 20 == 15)
			mv = 3100;                     /* single spike */
		if (i % 20 == 8 || i % 20 == 9)
			mv = 3150;                     /* two in a row */
		o = feed(&b, t, mv, false);
		crit += o.critical_now;
	}
	CHECK(crit == 0, "spikes triggered critical %d times", crit);

	/* voltage hovering around crit_mv +- 20 mV: sustained, must trigger once */
	batt_init(&b, NULL);
	crit = 0;
	for (i = 0; i < 60; i++, t += 2000) {
		o = feed(&b, t, 3440 + (i % 2 ? 20 : -15), false);
		crit += o.critical_now;
	}
	CHECK(crit == 1, "hovering: critical %d times", crit);

	/* short dip below crit then back well above: must not trigger */
	batt_init(&b, NULL);
	crit = 0;
	for (i = 0; i < 40; i++, t += 2000) {
		int mv = (i >= 10 && i < 13) ? 3420 : 3560;
		o = feed(&b, t, mv, false);
		crit += o.critical_now;
	}
	CHECK(crit == 0, "3-sample dip triggered critical");

	/* emergency: a sudden collapse to 3.25 V triggers within 3 samples */
	batt_init(&b, NULL);
	for (i = 0; i < 5; i++, t += 2000)
		feed(&b, t, 3550, false);
	for (i = 1; i <= 5; i++, t += 2000) {
		o = feed(&b, t, 3250, false);
		if (o.critical_now)
			break;
	}
	CHECK(i <= 3, "emergency took %d samples", i);

	/* the charger resets the countdown and the latch */
	batt_init(&b, NULL);
	crit = 0;
	for (i = 0; i < 3; i++, t += 2000)
		crit += feed(&b, t, 3430, false).critical_now;
	o = feed(&b, t, 3900, true);
	t += 2000;
	CHECK(o.level == BATT_LEVEL_OK, "charger level %d", o.level);
	for (i = 0; i < 3; i++, t += 2000)
		crit += feed(&b, t, 3430, false).critical_now;
	CHECK(crit == 0, "charger did not reset the countdown");
	for (i = 0; i < 10; i++, t += 2000)
		crit += feed(&b, t, 3430, false).critical_now;
	CHECK(crit == 1, "re-armed critical fired %d times", crit);
}

static void test_warning_hysteresis(void)
{
	struct batt_params p;
	struct batt b;
	struct batt_sample s;
	struct batt_out o;
	static const int seq[] = { 20, 16, 15, 16, 15, 16, 14, 15, 14, 13, 12, 8, 7, 8, 7, 8, 6, 7 };
	int i, lows = 0, vlows = 0, oks = 0;
	int64_t t = 0;

	batt_params_defaults(&p);
	p.gauge = BATT_GAUGE_AXP;
	p.tau_s = 0;                         /* no smoothing: test the levels */
	batt_init(&b, &p);
	for (i = 0; i < (int)(sizeof(seq) / sizeof(seq[0])); i++, t += 5000) {
		memset(&s, 0, sizeof(s));
		s.t_ms = t;
		s.present = true;
		s.state = BATT_DISCHARGING;
		s.mv = 3700;
		s.axp_pct = seq[i];
		batt_update(&b, &s, &o);
		if (o.level_changed) {
			lows += o.level == BATT_LEVEL_LOW;
			vlows += o.level == BATT_LEVEL_VERY_LOW;
			oks += o.level == BATT_LEVEL_OK;
		}
	}
	CHECK(lows == 1 && vlows == 1 && oks == 0, "flapping: low %d vlow %d ok %d", lows, vlows, oks);
	CHECK(o.level == BATT_LEVEL_VERY_LOW, "level %d", o.level);
	/* a clear recovery (> 5 points, e.g. the load went away) above 7 + 3 */
	s.t_ms = t;
	s.axp_pct = 13;
	batt_update(&b, &s, &o);
	CHECK(o.level == BATT_LEVEL_LOW && o.level_changed, "recovery to LOW: %d", o.level);
	s.t_ms = t + 5000;
	s.axp_pct = 19;
	batt_update(&b, &s, &o);
	CHECK(o.level == BATT_LEVEL_OK, "recovery to OK: %d", o.level);
	/* the charger clears everything at once */
	batt_init(&b, &p);
	s.t_ms = 0;
	s.axp_pct = 5;
	batt_update(&b, &s, &o);
	CHECK(o.level == BATT_LEVEL_VERY_LOW, "vlow %d", o.level);
	s.t_ms = 10000;
	s.charger = true;
	s.state = BATT_CHARGING;
	batt_update(&b, &s, &o);
	CHECK(o.level == BATT_LEVEL_OK && o.level_changed, "charger clear %d", o.level);
}

/* ----------------------------------------------------------- power.c */
static void test_init_policy(void)
{
	const struct power_status *st;

	make_tree();
	fake_now = 1000000;
	init_power();
	st = power_get_status();
	CHECK(rlong(PEK "/shutdown") == 6000, "PEK shutdown %ld", rlong(PEK "/shutdown"));
	CHECK(rlong(PEK "/startup") == 128, "PEK power-on hold %ld", rlong(PEK "/startup"));
	CHECK(rlong(BAT "/voltage_min") == 3000000, "V_OFF %ld", rlong(BAT "/voltage_min"));
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "schedutil"), "gov %s", rstr(POL "/scaling_governor"));
	CHECK(st->valid && st->battery_present && !st->charger_online, "status");
	CHECK(st->voltage_mv == 3950 && st->percent_axp == 80, "mv %d axp %d", st->voltage_mv, st->percent_axp);
	CHECK(st->percent >= 70 && st->percent <= 85, "percent %d", st->percent);
	CHECK(st->temp_mc == 45000 && !st->hot, "temp %d", st->temp_mc);
	CHECK(st->boot_reason == POWER_BOOT_UNKNOWN && st->mode == POWER_MODE_NORMAL, "mode");
	CHECK(R.status == 1, "initial on_status %d", R.status);
	CHECK(power_get_fd() >= 0, "fd");

	power_set_game_running(true);
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "performance"), "game gov %s", rstr(POL "/scaling_governor"));
	power_set_game_cpu("schedutil", 720000);
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "schedutil") && rlong(POL "/scaling_max_freq") == 720000,
	      "core profile %s %ld", rstr(POL "/scaling_governor"), rlong(POL "/scaling_max_freq"));
	power_set_game_running(false);
	power_set_game_cpu(NULL, 0);
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "schedutil") && rlong(POL "/scaling_max_freq") == 960000,
	      "menu again %s %ld", rstr(POL "/scaling_governor"), rlong(POL "/scaling_max_freq"));

	/* charger plugged: status callback */
	wfile(AC "/online", "1\n");
	set_batt(4050, 900, "Charging\n", 81);
	advance(10000, 1000);
	CHECK(st->charger_online && st->state == BATT_CHARGING, "charging");
	CHECK(R.status >= 2, "status on charger %d", R.status);
	power_exit();
}

static void test_idle(void)
{
	make_tree();
	fake_now = 1000000;
	init_power();
	advance(119000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_ON, "on at 119 s");
	advance(1000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_DIM, "dim at 120 s");
	CHECK(rlong(BL "/brightness") == 15, "dim brightness %ld", rlong(BL "/brightness"));
	advance(180000, 5000);
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF && R.last_screen == POWER_SCREEN_OFF, "off at 300 s");
	CHECK(rlong(BL "/bl_power") == 4, "bl_power %ld", rlong(BL "/bl_power"));
	/* a key (external pad through the UI) wakes and is swallowed */
	CHECK(power_on_input() == true, "wake key not swallowed");
	CHECK(power_get_status()->screen == POWER_SCREEN_ON, "screen back on");
	CHECK(rlong(BL "/bl_power") == 0 && rlong(BL "/brightness") == 50, "restore %ld %ld",
	      rlong(BL "/bl_power"), rlong(BL "/brightness"));
	fake_now += 200;
	CHECK(power_on_input() == false, "next key swallowed");
	/* a built-in key: swallowed while held, and its release too */
	advance(300000, 5000);
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF, "off again");
	power_inject_key(BTN_SOUTH, 1);
	CHECK(power_get_status()->screen == POWER_SCREEN_ON, "built-in key wakes");
	CHECK(power_on_input() == true, "press swallowed");
	fake_now += 500;
	CHECK(power_on_input() == true, "repeat swallowed");
	power_inject_key(BTN_SOUTH, 0);
	CHECK(power_on_input() == true, "release swallowed");
	fake_now += 150;
	CHECK(power_on_input() == false, "later key passes");
	/*
	 * Regression ("after wake, nothing works"): on the device a built-in
	 * key reaches this module (its evdev fds) before the UI calls
	 * power_on_input() for the same press, so the next press must pass
	 * even though a key is down at that moment.
	 */
	advance(300000, 5000);
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF, "off a third time");
	power_inject_key(BTN_EAST, 1);        /* the waking press, as the evdev read sees it */
	CHECK(power_on_input() == true, "waking press reaches the UI");
	power_inject_key(BTN_EAST, 0);
	CHECK(power_on_input() == true, "waking release reaches the UI");
	advance(400, 100);
	for (int i = 0; i < 3; i++) {
		power_inject_key(BTN_DPAD_DOWN, 1);
		CHECK(power_on_input() == false, "built-in press %d after the wake swallowed", i);
		fake_now += 450;
		CHECK(power_on_input() == false, "its repeat %d swallowed", i);
		power_inject_key(BTN_DPAD_DOWN, 0);
		CHECK(power_on_input() == false, "its release %d swallowed", i);
		fake_now += 30;                     /* fast taps: under SWALLOW_RELEASE_MS */
	}
	/* the dim step (2 min) takes the same path */
	advance(121000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_DIM, "dim again");
	power_inject_key(BTN_SOUTH, 1);
	CHECK(power_on_input() == true && power_get_status()->screen == POWER_SCREEN_ON, "dim wake");
	power_inject_key(BTN_SOUTH, 0);
	CHECK(power_on_input() == true, "dim wake release");
	fake_now += 200;
	power_inject_key(BTN_SOUTH, 1);
	CHECK(power_on_input() == false, "press after the dim wake swallowed");
	power_inject_key(BTN_SOUTH, 0);
	CHECK(power_on_input() == false, "release after the dim wake swallowed");
	/* never while a game runs */
	power_set_game_running(true);
	advance(600000, 10000);
	CHECK(power_get_status()->screen == POWER_SCREEN_ON, "dimmed during a game");
	power_set_game_running(false);
	advance(119000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_ON, "idle restarts after the game");
	/* settings */
	CHECK(power_set_setting("idle_dim_min", "0") == 0, "set");
	CHECK(power_set_setting("idle_off_min", "1") == 0, "set");
	advance(61000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF, "custom off time");
	power_exit();
}

/* Idle power-off: dim -> screen off -> power off (docs/power.md). */
static void test_idle_poweroff(void)
{
	/* 1. screen off 5 min, power off 10 min: the notice at 9:50, then off */
	make_tree();
	fake_now = 1000000;
	init_power();
	CHECK(power_set_setting("idle_poweroff_min", "10") == 0, "setting");
	advance(300000, 5000);
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF && R.idle_warn == 0, "screen off at 5 min");
	advance(289000, 1000);
	CHECK(R.idle_warn == 0 && R.shutdown == 0, "no notice at 9:49");
	advance(1000, 1000);
	CHECK(R.idle_warn == 1 && R.idle_secs == 10, "notice at 9:50 (%d, %d s)", R.idle_warn, R.idle_secs);
	CHECK(power_get_status()->screen == POWER_SCREEN_ON && R.last_screen == POWER_SCREEN_ON &&
	      rlong(BL "/bl_power") == 0, "the screen is lit for the notice");
	advance(9000, 1000);
	CHECK(R.shutdown == 0 && power_get_status()->screen == POWER_SCREEN_ON, "still on at 9:59, screen kept on");
	advance(1000, 1000);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_IDLE && R.poweroff == 1, "powered off at 10 min");
	CHECK(!strcmp(power_reason_name(POWER_REASON_IDLE), "idle"), "reason name");
	power_exit();

	/* 2. equal timers: straight to the power-off, no screen-off stage */
	make_tree();
	init_power();
	power_set_setting("idle_off_min", "5");
	power_set_setting("idle_poweroff_min", "5");
	advance(289000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_DIM, "dimmed at 2 min, not off");
	advance(1000, 1000);
	CHECK(R.idle_warn == 1 && power_get_status()->screen == POWER_SCREEN_ON, "notice at 4:50");
	advance(10000, 1000);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_IDLE, "powered off at 5 min");
	CHECK(R.screen >= 1 && R.last_screen == POWER_SCREEN_ON, "never a screen-off stage");
	power_exit();

	/* 3. power-off shorter than screen-off: the power-off wins */
	make_tree();
	init_power();
	power_set_setting("idle_off_min", "10");
	power_set_setting("idle_poweroff_min", "5");
	advance(300000, 1000);
	CHECK(R.shutdown == 1 && power_get_status()->screen != POWER_SCREEN_OFF, "off at 5 min, screen never off");
	power_exit();

	/* 4. any input cancels the notice (swallowed) and restarts the countdown */
	make_tree();
	init_power();
	power_set_setting("idle_poweroff_min", "5");
	advance(290000, 1000);
	CHECK(R.idle_warn == 1, "notice");
	CHECK(power_on_input() == true, "the cancelling press is swallowed");
	CHECK(R.idle_cancel == 1, "cancelled");
	advance(30000, 1000);
	CHECK(R.shutdown == 0, "no power-off after a cancel");
	fake_now += 200;
	CHECK(power_on_input() == false, "the next press passes");
	advance(289000, 1000);
	CHECK(R.idle_warn == 1 && R.shutdown == 0, "the countdown started over");
	advance(1000, 1000);
	CHECK(R.idle_warn == 2, "notice again 4:50 after the last input");
	power_inject_key(BTN_SOUTH, 1);          /* a built-in key */
	power_inject_key(BTN_SOUTH, 0);
	CHECK(R.idle_cancel == 2 && power_on_input() == true, "built-in key cancels (swallowed)");
	advance(290000, 1000);
	CHECK(R.idle_warn == 3, "notice a third time");
	power_inject_key(KEY_POWER, 1);          /* the power key too, and its release does not power off */
	advance(200, 100);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.idle_cancel == 3 && R.shutdown == 0 && power_get_mode() == POWER_MODE_NORMAL, "power key cancels");
	/* in a game: no dim/off stage, but the power-off counts; game input cancels */
	power_set_game_running(true);
	advance(290000, 1000);
	CHECK(R.idle_warn == 4 && power_get_status()->screen == POWER_SCREEN_ON, "in a game: notice, never dimmed");
	power_notify_activity();                  /* the game reported input */
	CHECK(R.idle_cancel == 4, "game input cancels");
	advance(20000, 1000);
	CHECK(R.shutdown == 0, "no power-off");
	advance(280000, 1000);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_IDLE, "idle game: powered off (save path)");
	power_exit();

	/* 5. busy: never while a long job runs; the countdown starts over after it */
	make_tree();
	init_power();
	power_set_setting("idle_off_min", "2");
	power_set_setting("idle_poweroff_min", "5");
	power_set_busy(true, "usb-import");
	advance(3600000, 10000);
	CHECK(R.idle_warn == 0 && R.shutdown == 0, "no power-off during an hour-long copy");
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF, "the screen still goes off");
	power_set_busy(false, NULL);
	advance(289000, 1000);
	CHECK(R.idle_warn == 0, "countdown restarted at the end of the job");
	advance(1000, 1000);
	CHECK(R.idle_warn == 1, "notice 4:50 after the job");
	power_set_busy(true, "smb-client");     /* a job starts during the notice */
	CHECK(R.idle_cancel == 1, "a job cancels the notice");
	advance(60000, 1000);
	CHECK(R.shutdown == 0, "held");
	power_set_busy(false, NULL);
	advance(300000, 1000);
	CHECK(R.shutdown == 1, "then off 5 min after the job");
	power_exit();

	/* 6. an idle power-off that cannot save is cancelled, never forced */
	make_tree();
	init_power();
	R.call_poweroff = false;                  /* the frontend saves first */
	power_set_setting("idle_poweroff_min", "5");
	advance(300000, 1000);
	CHECK(R.shutdown == 1 && power_get_mode() == POWER_MODE_SHUTTING_DOWN, "shutting down");
	CHECK(power_cancel_shutdown() == 0 && power_get_mode() == POWER_MODE_NORMAL, "cancelled by the frontend");
	CHECK(power_cancel_shutdown() == -EINVAL, "nothing to cancel");
	advance(300000, 1000);
	CHECK(R.shutdown == 2 && power_get_mode() == POWER_MODE_SHUTTING_DOWN, "tried again 5 min later");
	advance(20000, 1000);                     /* the grace time runs out: no forced power-off */
	CHECK(R.poweroff == 0 && power_get_mode() == POWER_MODE_NORMAL, "not completed: cancelled, stays on");
	power_request_shutdown(POWER_REASON_USER);
	CHECK(power_cancel_shutdown() == -EINVAL, "a user power-off is never cancelled");
	power_exit();

	/* 7. never in charge mode; settings */
	make_tree();
	wfile("run/bootreason", "charger\n");
	wfile(AC "/online", "1\n");
	set_batt(3800, 700, "Charging\n", 40);
	init_power();
	power_set_setting("idle_poweroff_min", "5");
	advance(3600000, 30000);
	CHECK(power_get_mode() == POWER_MODE_CHARGE && R.idle_warn == 0 && R.shutdown == 0, "charge mode: never");
	CHECK(power_set_setting("idle_poweroff_min", "241") == -EINVAL, "range");
	CHECK(power_set_setting("idle_poweroff_min", "0") == 0 && power_set_setting("idle_poweroff_s", "30") == 0,
	      "keys");
	power_exit();

	/* 8. analog sticks (in a game, as the menu process reads them): a real
	 * move resets the timer, the jitter of a drifting stick never does */
	{
		int16_t ref = 0;
		int moves = 0;

		CHECK(!power_axis_activity(0, &ref), "centre");
		for (int i = 0; i < 2000; i++)          /* +-3000 noise around a 2000 drift */
			moves += power_axis_activity((int16_t)(2000 + noise(3000)), &ref);
		CHECK(moves == 0, "jitter counted %d times", moves);
		CHECK(power_axis_activity(-16000, &ref) && ref == -16000, "a 20 %% move counts");
		CHECK(!power_axis_activity(-15000, &ref), "small change after it: no");
		CHECK(power_axis_activity(-20000, &ref), "held past half deflection: counts");
		CHECK(power_axis_activity(32767, &ref) && power_axis_activity(32767, &ref), "held right: counts");
		ref = 0;
		CHECK(!power_axis_activity(13000, &ref) && power_axis_activity(13200, &ref), "the 20 %% threshold");

		/* the timer: a player steering once a minute keeps the unit on for
		 * an hour; a drifting stick alone lets it power off at 5 min */
		make_tree();
		init_power();
		power_set_setting("idle_poweroff_min", "5");
		power_set_game_running(true);
		ref = 0;
		for (int m = 0; m < 60; m++) {
			for (int s = 0; s < 60; s++) {
				int16_t v = (int16_t)((s == 0 ? (m % 2 ? 28000 : -28000) : 0) + noise(2000));

				if (power_axis_activity(v, &ref))
					power_notify_activity();
				advance(1000, 1000);
			}
		}
		CHECK(R.shutdown == 0 && R.idle_warn == 0, "steering with the stick: on after an hour (%d, %d)",
		      R.shutdown, R.idle_warn);
		ref = 0;
		for (int s = 0; s < 400 && !R.shutdown; s++) {
			if (power_axis_activity((int16_t)(2500 + noise(3000)), &ref))
				power_notify_activity();
			advance(1000, 1000);
		}
		CHECK(R.idle_warn == 1 && R.shutdown == 1 && R.last_why == POWER_REASON_IDLE,
		      "a drifting stick alone: off at 5 min (%d, %d)", R.idle_warn, R.shutdown);
		power_exit();
	}
	printf("idle power-off: order, equal timers, notice and cancel, busy, charge mode, sticks\n");
}

static void test_sleep(void)
{
	char clk[PSYS_PATH_MAX];
	time_t saved = 0;

	make_tree();
	fake_now = 1000000;
	init_power();
	/* fake sleep: internal only now (the power key powers off) */
	CHECK(power_sleep(true) == 0, "sleep");
	CHECK(power_get_mode() == POWER_MODE_SLEEP && R.sleep_on == 1, "sleep %d", R.sleep_on);
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "powersave"), "sleep gov %s", rstr(POL "/scaling_governor"));
	CHECK(rlong(BL "/bl_power") == 4 && rlong(LED "/brightness") == 1, "bl %ld led %ld",
	      rlong(BL "/bl_power"), rlong(LED "/brightness"));
	CHECK(power_on_input() == true, "input while asleep must be dropped");
	advance(14 * 60000, 30000);
	CHECK(power_get_mode() == POWER_MODE_SLEEP && R.shutdown == 0, "still asleep at 14 min");
	/* any button wakes */
	power_inject_key(BTN_EAST, 1);
	CHECK(power_get_mode() == POWER_MODE_NORMAL && R.sleep_off == 1, "wake");
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "schedutil"), "wake gov %s", rstr(POL "/scaling_governor"));
	CHECK(rlong(BL "/bl_power") == 0 && rlong(BL "/brightness") == 50 && rlong(LED "/brightness") == 0,
	      "wake outputs");
	CHECK(power_on_input() == true, "wake key swallowed");
	power_inject_key(BTN_EAST, 0);
	CHECK(power_on_input() == true, "wake key release swallowed");
	fake_now += 150;
	power_inject_key(BTN_EAST, 1);        /* regression: a built-in key after the wake */
	CHECK(power_on_input() == false, "next built-in key passes");
	power_inject_key(BTN_EAST, 0);
	CHECK(power_on_input() == false, "its release passes");

	/* a game is running: sleep and wake keep the game governor */
	power_set_game_running(true);
	CHECK(power_sleep(true) == 0, "menu sleep");
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "powersave"), "gov in sleep");
	power_inject_key(KEY_POWER, 1);        /* the power key wakes on press */
	advance(200, 100);
	power_inject_key(KEY_POWER, 0);        /* ... and its release is not a new sleep */
	CHECK(power_get_mode() == POWER_MODE_NORMAL, "pek wake");
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "performance"), "game gov after wake %s",
	      rstr(POL "/scaling_governor"));
	power_set_game_running(false);

	/* power-key-only wake setting */
	CHECK(power_set_setting("sleep_wake", "power") == 0, "setting");
	power_sleep(true);
	power_inject_key(BTN_SOUTH, 1);
	power_inject_key(BTN_SOUTH, 0);
	CHECK(power_get_mode() == POWER_MODE_SLEEP, "button woke with sleep_wake=power");

	/* the sleep timeout: shutdown with the save path, then power off */
	advance(15 * 60000, 30000);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_SLEEP_TIMEOUT, "timeout shutdown %d", R.shutdown);
	CHECK(R.poweroff == 1 && !R.poweroff_reboot, "poweroff %d", R.poweroff);
	snprintf(clk, sizeof(clk), "%s/data/lastclock", root);
	CHECK(pclock_load(clk, &saved) == 0 && saved == WALL, "lastclock %lld", (long long)saved);
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "performance"), "save at full speed");
	power_exit();

	/* sleep_timeout_min = 0: never */
	make_tree();
	init_power();
	power_set_setting("sleep_timeout_min", "0");
	power_sleep(true);
	advance(120 * 60000, 60000);
	CHECK(power_get_mode() == POWER_MODE_SLEEP && R.shutdown == 0, "timeout 0");
	power_exit();
}

/* Short press = clean power-off (no sleep mode), docs/power.md §5. */
static void test_power_key(void)
{
	make_tree();
	fake_now = 1000000;
	init_power();
	/* start-up guard: the press that powered the unit on */
	power_inject_key(KEY_POWER, 1);
	advance(300, 100);
	power_inject_key(KEY_POWER, 0);
	advance(1000, 100);
	CHECK(R.shutdown == 0 && R.sleep_on == 0 && power_get_mode() == POWER_MODE_NORMAL,
	      "press during the start-up guard acted: shutdown %d", R.shutdown);
	/* a release whose press was never seen (key held at start) does nothing */
	advance(1000, 100);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 0, "lone release acted");

	/* menu: a short press requests the clean shutdown, once */
	R.call_poweroff = false;
	power_inject_key(KEY_POWER, 1);
	advance(150, 50);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_USER && R.sleep_on == 0,
	      "short press: shutdown %d sleep %d", R.shutdown, R.sleep_on);
	CHECK(power_get_mode() == POWER_MODE_SHUTTING_DOWN, "mode %d", power_get_mode());
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "performance"), "save at full speed");
	/* pressed again (impatient user): still one request */
	power_inject_key(KEY_POWER, 1);
	advance(2500, 100);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 1, "second press: %d requests", R.shutdown);
	/* the frontend saves and calls power_poweroff() */
	CHECK(power_poweroff(false) == 0 && R.poweroff == 1 && !R.poweroff_reboot, "poweroff");
	power_exit();

	/* during a game: the same request (the frontend signals the child) */
	make_tree();
	init_power();
	advance(3000, 500);
	power_set_game_running(true);
	power_inject_key(KEY_POWER, 1);
	advance(200, 100);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_USER, "in-game short press %d", R.shutdown);
	power_exit();

	/* screen off (idle): the press only turns it on; dimmed: it powers off */
	make_tree();
	init_power();
	advance(300000, 5000);
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF, "idle off");
	power_inject_key(KEY_POWER, 1);
	CHECK(power_get_status()->screen == POWER_SCREEN_ON, "press turns the screen on");
	advance(100, 50);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 0, "waking press powered off");
	advance(120000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_DIM, "dim");
	power_inject_key(KEY_POWER, 1);
	advance(100, 50);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 1 && R.poweroff == 1, "dimmed menu: short press powers off %d", R.shutdown);
	power_exit();
}

static void test_powersave_fallback(void)
{
	char p[PSYS_PATH_MAX];

	make_tree();
	/* a policy whose governor attribute rejects writes (powersave not built) */
	snprintf(p, sizeof(p), "%s/sys/devices/system/cpu/cpufreq/policy1/scaling_governor", root);
	mkdirs(p);
	wlong("sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq", 960000);
	wlong("sys/devices/system/cpu/cpufreq/policy1/cpuinfo_max_freq", 960000);
	wlong("sys/devices/system/cpu/cpufreq/policy1/cpuinfo_min_freq", 144000);
	init_power();
	power_sleep(true);
	CHECK(rlong("sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq") == 144000,
	      "fallback clamp %ld", rlong("sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq"));
	power_sleep(false);
	CHECK(rlong("sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq") == 960000,
	      "unclamp %ld", rlong("sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq"));
	power_exit();
}

static void test_long_press_watchdog(void)
{
	make_tree();
	fake_now = 5000000;
	init_power();
	R.call_poweroff = false;               /* a frontend that hangs while saving */
	advance(2500, 500);                    /* past the start-up guard */
	power_inject_key(KEY_POWER, 1);
	advance(1999, 100);
	CHECK(R.shutdown == 0 && power_get_mode() == POWER_MODE_NORMAL, "too early");
	advance(1, 1);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_USER, "long press %d", R.shutdown);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.sleep_on == 0, "release after long press started a sleep");
	advance(14900, 100);
	CHECK(R.poweroff == 0, "watchdog too early");
	advance(200, 100);
	CHECK(R.poweroff == 1, "watchdog did not power off");
	power_exit();

	/* menu reboot */
	make_tree();
	init_power();
	power_request_shutdown(POWER_REASON_REBOOT);
	CHECK(R.poweroff == 1 && R.poweroff_reboot, "reboot");
	power_exit();
}

static void test_critical(void)
{
	int i;

	make_tree();
	fake_now = 1000000;
	set_batt(3620, -600, "Discharging\n", 9);
	init_power();
	CHECK(power_get_status()->level == POWER_LEVEL_LOW || power_get_status()->level == POWER_LEVEL_VERY_LOW,
	      "level %d", power_get_status()->level);
	/* noisy single samples never trigger */
	for (i = 0; i < 20; i++) {
		set_batt(i % 7 == 3 ? 3150 : 3600, -600, "Discharging\n", 8);
		advance(2000, 2000);
	}
	CHECK(R.critical == 0 && R.shutdown == 0, "noise triggered critical");
	/* sustained under-load voltage below 3.45 V */
	for (i = 0; i < 10 && !R.critical; i++) {
		set_batt(3430, -600, "Discharging\n", 5);
		advance(2000, 2000);
	}
	CHECK(R.critical == 1 && R.shutdown == 1 && R.last_why == POWER_REASON_CRITICAL, "critical %d/%d",
	      R.critical, R.shutdown);
	CHECK(i >= 4 && i <= 7, "critical after %d samples", i);
	CHECK(R.poweroff == 1, "poweroff");
	CHECK(power_get_status()->level == POWER_LEVEL_CRITICAL, "level critical");
	power_exit();

	/* on the charger: no critical even at a low voltage */
	make_tree();
	wfile(AC "/online", "1\n");
	set_batt(3400, 500, "Charging\n", 2);
	init_power();
	advance(60000, 2000);
	CHECK(R.critical == 0, "critical while charging");
	power_exit();
}

static void test_warnings_module(void)
{
	int i;

	make_tree();
	fake_now = 1000000;
	init_power();
	power_set_setting("battery_gauge", "axp");
	for (i = 20; i >= 5; i--) {
		set_batt(3700, -400, "Discharging\n", i);
		advance(60000, 10000);
	}
	CHECK(R.warn >= 2, "warnings %d", R.warn);
	CHECK(R.last_warn == POWER_LEVEL_VERY_LOW, "last warn %d", R.last_warn);
	/* the persistent warning repeats every 5 min */
	{
		int w = R.warn;
		advance(300000, 10000);
		CHECK(R.warn == w + 1, "repeat %d -> %d", w, R.warn);
	}
	/* charger: cleared */
	wfile(AC "/online", "1\n");
	set_batt(3900, 800, "Charging\n", 6);
	advance(10000, 10000);
	CHECK(R.last_warn == POWER_LEVEL_OK, "cleared %d", R.last_warn);
	power_exit();
}

static void test_charge_mode(void)
{
	make_tree();
	wfile("run/bootreason", "charger\n");
	wfile(AC "/online", "1\n");
	set_batt(3800, 700, "Charging\n", 40);
	fake_now = 1000000;
	init_power();
	CHECK(power_get_mode() == POWER_MODE_CHARGE, "charge mode");
	CHECK(power_get_status()->boot_reason == POWER_BOOT_CHARGER, "boot reason");
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "powersave"), "charge gov");
	CHECK(rlong(BL "/brightness") == 10, "dimmed %ld", rlong(BL "/brightness"));
	advance(30000, 1000);
	CHECK(power_get_status()->screen == POWER_SCREEN_OFF, "screen off after 30 s");
	CHECK(power_on_input() == true, "keys go nowhere in charge mode");
	CHECK(power_get_status()->screen == POWER_SCREEN_DIM && rlong(BL "/bl_power") == 0, "key shows it again");
	/* short press: normal boot */
	power_inject_key(KEY_POWER, 1);
	advance(200, 100);
	power_inject_key(KEY_POWER, 0);
	CHECK(power_get_mode() == POWER_MODE_NORMAL && R.charge_exit == 1, "charge exit");
	CHECK(!strcmp(rstr("run/bootreason"), "key"), "bootreason rewritten: %s", rstr("run/bootreason"));
	CHECK(rlong(BL "/brightness") == 50, "full brightness %ld", rlong(BL "/brightness"));
	CHECK(!strcmp(rstr(POL "/scaling_governor"), "schedutil"), "menu gov");
	/* a second tap right after (bounce, double press) must not power off */
	advance(300, 100);
	power_inject_key(KEY_POWER, 1);
	advance(100, 100);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 0 && power_get_mode() == POWER_MODE_NORMAL, "double tap powered off");
	/* a deliberate press later: power off */
	advance(2000, 500);
	power_inject_key(KEY_POWER, 1);
	advance(200, 100);
	power_inject_key(KEY_POWER, 0);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_USER, "short press after charge mode %d",
	      R.shutdown);
	power_exit();

	/* charger removed: power off */
	make_tree();
	wfile("run/bootreason", "charger\n");
	wfile(AC "/online", "1\n");
	init_power();
	advance(10000, 1000);
	wfile(AC "/online", "0\n");
	advance(1900, 100);
	CHECK(R.shutdown == 0, "too early");
	advance(4000, 100);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_CHARGER_REMOVED, "charger removed %d", R.shutdown);
	power_exit();

	/* booted by the charger, but it is already gone */
	make_tree();
	wfile("run/bootreason", "charger\n");
	init_power();
	advance(3000, 500);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_CHARGER_REMOVED, "gone at boot %d", R.shutdown);
	power_exit();

	/* long press in charge mode: power off */
	make_tree();
	wfile("run/bootreason", "charger\n");
	wfile(AC "/online", "1\n");
	init_power();
	advance(2500, 500);
	power_inject_key(KEY_POWER, 1);
	advance(2100, 100);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_USER, "long press in charge mode");
	power_exit();
}

static void test_thermal(void)
{
	make_tree();
	fake_now = 1000000;
	init_power();
	wlong(TZONE "/temp", 80000);
	advance(10000, 10000);
	CHECK(R.thermal == 1 && R.last_hot && power_get_status()->hot, "hot");
	wlong(TZONE "/temp", 72000);
	advance(10000, 10000);
	CHECK(R.thermal == 1, "hysteresis");
	wlong(TZONE "/temp", 69000);
	advance(10000, 10000);
	CHECK(R.thermal == 2 && !R.last_hot, "cooled");
	wlong(TZONE "/temp", 96000);
	advance(20000, 10000);
	CHECK(R.shutdown == 0, "thermal shutdown too early");
	advance(10000, 10000);
	CHECK(R.shutdown == 1 && R.last_why == POWER_REASON_THERMAL, "thermal shutdown");
	power_exit();
}

static void test_settings(void)
{
	make_tree();
	init_power();
	CHECK(power_set_setting("sleep_timeout_min", "5") == 0, "ok");
	CHECK(power_set_setting("sleep_timeout_min", "-1") == -EINVAL, "range");
	CHECK(power_set_setting("sleep_timeout_min", "5x") == -EINVAL, "garbage");
	CHECK(power_set_setting("battery_gauge", "magic") == -EINVAL, "gauge");
	CHECK(power_set_setting("theme", "x") == -ENOENT, "not ours");
	CHECK(power_set_setting("timezone", "Europe/Paris") == 0, "tz");
	CHECK(getenv("TZ") && !strcmp(getenv("TZ"), "CET-1CEST,M3.5.0,M10.5.0/3"), "TZ %s", getenv("TZ"));
	CHECK(power_set_timezone("EST5EDT,M3.2.0,M11.1.0") == 0, "posix tz");
	CHECK(power_set_timezone("rm -rf /") == -EINVAL, "bad tz");
	CHECK(power_set_time(100) == -EINVAL, "1970 refused");
	power_exit();
	unsetenv("TZ");
	tzset();
}

/* ------------------------------------------------ pclock / bootreason */
static long off_at(const char *posix, int mon)
{
	struct tm tm;
	time_t t;

	setenv("TZ", posix, 1);
	tzset();
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 126;       /* 2026 */
	tm.tm_mon = mon;
	tm.tm_mday = 15;
	tm.tm_hour = 12;
	t = timegm(&tm);
	localtime_r(&t, &tm);
	return tm.tm_gmtoff;
}

static void test_clock(void)
{
	char f[PSYS_PATH_MAX];
	time_t v = 0;
	const struct power_tz *z;
	int n, i, zero = 0;
	static const struct { const char *name; long jan, jul; } exp[] = {
		{ "UTC", 0, 0 },
		{ "Europe/London", 0, 3600 },
		{ "Europe/Paris", 3600, 7200 },
		{ "Europe/Athens", 7200, 10800 },
		{ "Asia/Kolkata", 19800, 19800 },
		{ "Asia/Kathmandu", 20700, 20700 },
		{ "Asia/Tehran", 12600, 12600 },
		{ "Australia/Adelaide", 37800, 34200 },
		{ "Australia/Sydney", 39600, 36000 },
		{ "Pacific/Auckland", 46800, 43200 },
		{ "America/New_York", -18000, -14400 },
		{ "America/St_Johns", -12600, -9000 },
		{ "America/Santiago", -10800, -14400 },
		{ "America/Sao_Paulo", -10800, -10800 },
		{ "Africa/Cairo", 7200, 10800 },
		{ "Asia/Jerusalem", 7200, 10800 },
	};

	snprintf(f, sizeof(f), "%s/data/lastclock", root);
	make_tree();
	CHECK(pclock_save(f, 1000) == -EINVAL, "saved 1970");
	CHECK(pclock_save(f, WALL) == 0 && pclock_load(f, &v) == 0 && v == WALL, "roundtrip %lld", (long long)v);
	wfile("data/lastclock", "garbage\n");
	CHECK(pclock_load(f, &v) == -EINVAL, "garbage accepted");
	CHECK(pclock_restore_target(100, WALL, true) == WALL, "restore from 1970");
	CHECK(pclock_restore_target(WALL + 50, WALL, true) == 0, "clock fine");
	CHECK(pclock_restore_target(100, 0, false) == pclock_build_epoch(), "build floor");
	CHECK(pclock_restore_target(pclock_build_epoch() + 10, pclock_build_epoch() + 100, true) ==
	      pclock_build_epoch() + 100, "forward only");

	z = pclock_timezones(&n);
	CHECK(n >= 40, "only %d zones", n);
	for (i = 0; i < n; i++) {
		CHECK(pclock_tz_valid(z[i].posix), "invalid %s", z[i].name);
		if (off_at(z[i].posix, 0) == 0 && off_at(z[i].posix, 6) == 0)
			zero++;
	}
	/* glibc falls back to UTC on a string it cannot parse: only UTC may be 0/0 */
	CHECK(zero == 1, "%d zones parse as UTC", zero);
	for (i = 0; i < (int)(sizeof(exp) / sizeof(exp[0])); i++) {
		const char *p = pclock_tz_lookup(exp[i].name);
		long j, u;
		CHECK(p != NULL, "missing %s", exp[i].name);
		if (!p)
			continue;
		j = off_at(p, 0);
		u = off_at(p, 6);
		CHECK(j == exp[i].jan && u == exp[i].jul, "%s: %ld/%ld, want %ld/%ld", exp[i].name, j, u,
		      exp[i].jan, exp[i].jul);
	}
	unsetenv("TZ");
	tzset();
}

static void test_bootreason(void)
{
	CHECK(bootreason_decode(0x00) == POWER_BOOT_KEY, "key");
	CHECK(bootreason_decode(0xe1) == POWER_BOOT_CHARGER, "charger");
	CHECK(bootreason_decode(0xc0) == POWER_BOOT_KEY, "key with charger present");
	CHECK(bootreason_decode(-1) == POWER_BOOT_UNKNOWN, "unknown");
	CHECK(bootreason_parse("charger") == POWER_BOOT_CHARGER && bootreason_parse("key") == POWER_BOOT_KEY &&
	      bootreason_parse("zz") == POWER_BOOT_UNKNOWN, "parse");
	CHECK(!strcmp(bootreason_name(POWER_BOOT_CHARGER), "charger"), "name");
	/* the child protocol signals are distinct and valid */
	CHECK(RSOS_SIG_SLEEP != RSOS_SIG_WAKE && RSOS_SIG_WAKE <= SIGRTMAX &&
	      RSOS_SIG_POWEROFF != SIGUSR1 && RSOS_SIG_POWEROFF != SIGTERM, "signals");
}

int main(void)
{
	char cmd[PSYS_PATH_MAX];

	snprintf(root, sizeof(root), "/tmp/rsos-power-test-%d", (int)getpid());
	WALL = pclock_build_epoch() + 30 * 86400L;
	test_ocv();
	test_discharge(BATT_GAUGE_VOLTAGE, false);
	test_discharge(BATT_GAUGE_AUTO, false);
	test_discharge(BATT_GAUGE_AUTO, true);
	test_discharge(BATT_GAUGE_AXP, false);
	test_noise_rejection();
	test_warning_hysteresis();
	test_init_policy();
	test_idle();
	test_idle_poweroff();
	test_sleep();
	test_power_key();
	test_powersave_fallback();
	test_long_press_watchdog();
	test_critical();
	test_warnings_module();
	test_charge_mode();
	test_thermal();
	test_settings();
	test_clock();
	test_bootreason();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd) != 0)
		printf("warning: could not remove %s\n", root);
	printf("power_test: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
