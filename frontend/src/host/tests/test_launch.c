/*
 * test_launch.c - the UI side of the process model: host_launch() runs
 * rsos-run (headless) with the test core and must report a normal exit, a
 * start error with its message, a crash (SIGSEGV) and a hang (watchdog),
 * while the SRAM flushed before the crash stays on disk.
 *
 * usage: rsos-launch-test RSOS_RUN TESTCORE.so WORKDIR
 */
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host.h"
#include "../host_png.h"
#include "../hutil.h"

static int failures;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		int ok_ = (cond); /* evaluated once: it launches a game */ \
		printf(ok_ ? "  ok    " : "  FAIL  ");                     \
		if (!ok_)                                                  \
			failures++;                                        \
		printf(__VA_ARGS__);                                       \
		printf("\n");                                              \
	} while (0)

/* host_launch_opts.idle: once the game process has become the benchmark
 * driver, the power module's sleep then wake signals (a power-key tap during
 * a benchmark). Review F-M12: their default action killed the driver. */
static int g_bench_sleeps;

static bool sleep_wake_driver(void *user)
{
	static int phase;
	pid_t c = host_child_pid();
	char p[64], cmd[4096];
	FILE *f;
	size_t n;

	(void)user;
	if (c <= 0)
		return false;
	if (phase == 0) {
		snprintf(p, sizeof(p), "/proc/%d/cmdline", (int)c);
		f = fopen(p, "rb");
		if (!f)
			return false;
		n = fread(cmd, 1, sizeof(cmd) - 1, f);
		fclose(f);
		for (size_t i = 0; i < n; i += strlen(cmd + i) + 1) {
			cmd[n] = 0;
			if (!strcmp(cmd + i, "--bench-driver")) {
				kill(c, SIGRTMIN + 1);           /* RSOS_SIG_SLEEP */
				phase = 1;
				break;
			}
		}
	} else if (phase++ == 3) {
		kill(c, SIGRTMIN + 2);                   /* RSOS_SIG_WAKE */
		g_bench_sleeps++;
	}
	return false;
}

/* host_launch_opts.idle (every ~100 ms): the power module's power-off signal
 * to the game after about 300 ms. */
static bool poweroff_after_300ms(void *user)
{
	static int calls;
	pid_t c = host_child_pid();

	(void)user;
	if (++calls % 3 == 0 && c > 0)
		kill(c, SIGUSR2);   /* RSOS_SIG_POWEROFF */
	return false;
}

/* ------------------------------------------------------------ batch 2 */
static char g_status[8192];

/* host_launch_opts.on_status: every line, as it arrives */
static void collect_status(const char *kind, const char *arg, void *user)
{
	size_t l = strlen(g_status);

	(void)user;
	if (l < sizeof(g_status))
		snprintf(g_status + l, sizeof(g_status) - l, "%s %s\n", kind, arg);
}

/* base args + more (NULL-terminated) into out */
static const char *const *more_args(const char *const *base, const char **out, int max, ...)
{
	va_list ap;
	const char *s;
	int n = 0;

	while (base[n] && n < max - 1) {
		out[n] = base[n];
		n++;
	}
	va_start(ap, max);
	while ((s = va_arg(ap, const char *)) && n < max - 1)
		out[n++] = s;
	va_end(ap);
	out[n] = NULL;
	return out;
}

static int count_lines(const char *txt, const char *prefix)
{
	int n = 0;

	for (const char *p = txt; p && (p = strstr(p, prefix)); p++)
		if (p == txt || p[-1] == '\n')
			n++;
	return n;
}

static void test_batch2(char **argv, const char *const *args, struct host_launch_opts *o)
{
	char sw[600], rom[600], other[600], st[600], shots[700], log[600], *txt = NULL;
	const char *a2[64];
	struct host_launch_result r;
	struct host_switch_entry e[2], back[HOST_SWITCHER_MAX];

	snprintf(sw, sizeof(sw), "%s/switcher.tsv", argv[3]);
	snprintf(rom, sizeof(rom), "%s/roms/test/game.bin", argv[3]);
	snprintf(other, sizeof(other), "%s/roms/test/other.bin", argv[3]);
	snprintf(st, sizeof(st), "%s/states/test/game.state.auto", argv[3]);
	snprintf(shots, sizeof(shots), "%s/screenshots", argv[3]);
	snprintf(log, sizeof(log), "%s/game.log", argv[3]);
	hwrite_atomic(other, "y", 1, false);

	printf("game switcher: the file, \"switch <n>\", exit 6, the game saved\n");
	memset(e, 0, sizeof(e));
	snprintf(e[0].name, sizeof(e[0].name), "Game");
	snprintf(e[0].system, sizeof(e[0].system), "test");
	snprintf(e[0].rom, sizeof(e[0].rom), "%s", rom);
	snprintf(e[0].core, sizeof(e[0].core), "rsos-testcore");
	e[0].current = true;
	snprintf(e[1].name, sizeof(e[1].name), "Other\tgame");     /* a tab cannot break the file */
	snprintf(e[1].system, sizeof(e[1].system), "test");
	snprintf(e[1].rom, sizeof(e[1].rom), "%s", other);
	snprintf(e[1].thumb, sizeof(e[1].thumb), "%s/states/test/other.state.auto.png", argv[3]);
	CHECK(host_switcher_write(sw, e, 2) == 0 && host_switcher_read(sw, back, HOST_SWITCHER_MAX) == 2 &&
	      back[0].current && !back[1].current && !strcmp(back[1].rom, other) &&
	      !strcmp(back[1].name, "Other game") && !strcmp(back[1].thumb, e[1].thumb),
	      "switcher file round trip (\"%s\")", back[1].name);
	remove(st);
	o->extra_args = more_args(args, a2, 64, "--switcher", sw, "--switcher-pick", "1", "--no-autosave", NULL);
	CHECK(host_launch(argv[2], rom, "test", o, &r) == 0 && r.status == HOST_EXIT_SWITCH && r.switch_to == 1 &&
	      r.auto_state_saved && hfile_exists(st),
	      "entry 1 picked: exit %d, switch_to %d, auto state saved %d (auto-save on exit is off)", r.exit_code,
	      r.switch_to, r.auto_state_saved);
	o->extra_args = more_args(args, a2, 64, "--switcher", sw, "--switcher-shot", shots, "--frames", "20",
				  "--fonts", "third_party/fonts", NULL);
	/* (the picture itself: rsos-run --switcher-shot, docs/ui-previews) */
	CHECK(host_launch(argv[2], rom, "test", o, &r) == 0 && r.status == HOST_EXIT_OK && hfile_size(shots) > 1000,
	      "the switcher drawn (--switcher-shot, %lld bytes)", hfile_size(shots));
	remove(shots);

	printf("play time: \"playtime\" lines while the game runs and at exit\n");
	g_status[0] = 0;
	setenv("RSOS_TESTCORE", "slow", 1);          /* ~20 ms a frame: 150 frames = 3 s */
	o->on_status = collect_status;
	o->extra_args = more_args(args, a2, 64, "--frames", "150", "--playtime-report", "1", NULL);
	CHECK(host_launch(argv[2], rom, "test", o, &r) == 0 && r.status == HOST_EXIT_OK && r.playtime_s >= 2 &&
	      r.playtime_s <= 5 && count_lines(g_status, "playtime ") >= 2,
	      "%lld s played, %d \"playtime\" lines seen as they came", r.playtime_s,
	      count_lines(g_status, "playtime "));
	unsetenv("RSOS_TESTCORE");

	printf("per-game settings from the in-game menu (\"setting\" lines)\n");
	g_status[0] = 0;
	o->extra_args = more_args(args, a2, 64, "--menu-script", "sel=Scaling,r,sel=CPU profile,r", "--frames", "40",
				  NULL);
	CHECK(host_launch(argv[2], rom, "test", o, &r) == 0 && strstr(g_status, "setting scale integer\n") &&
	      strstr(g_status, "setting cpu performance\n"),
	      "Scaling -> Integer, CPU profile -> Performance reported for this game");
	o->extra_args = more_args(args, a2, 64, "--scale", "stretch", "--cpu-profile", "powersave", "--frames", "5",
				  NULL);
	CHECK(host_launch(argv[2], rom, "test", o, &r) == 0 && (txt = hread_file(log, NULL)) &&
	      strstr(txt, "scaling: stretch (--scale), cpu profile: powersave"),
	      "the game's own scaling and CPU profile at start");
	free(txt);
	txt = NULL;
	o->on_status = NULL;

	printf("fast-forward (Select+R2): x3, then the normal speed at once (clock pacing)\n");
	o->extra_args = more_args(args, a2, 64, "--timer-pacing", "--ff-speed", "3", "--hotkey-script",
				  "60:ff,120:ff,180:mark", "--frames", "190", NULL);
	CHECK(host_launch(argv[2], rom, "test", o, &r) == 0 && r.status == HOST_EXIT_OK, "run with the hotkeys");
	txt = hread_file(log, NULL);
	{
		const char *p = txt ? strstr(txt, "fast-forward off at frame 120:") : NULL;
		const char *q = txt ? strstr(txt, "mark: frame 180") : NULL;
		double runs = 0, x = 0;
		long long ms = 0, after = 0;

		if (p)
			sscanf(p, "fast-forward off at frame 120: %lf runs in %lld ms (x%lf)", &runs, &ms, &x);
		if (q && (q = strstr(q, "(")))
			sscanf(q, "(%lld ms after", &after);
		CHECK(p && runs == 180 && x > 2.5 && x < 3.3,
		      "60 frames shown at x3: %.0f runs in %lld ms, x%.2f", runs, ms, x);
		CHECK(q && after > 900 && after < 1300,
		      "then 60 frames at the normal speed: %lld ms (no catch-up burst, no stall)", after);
	}
	free(txt);
	txt = NULL;

	printf("screenshot (Select+L2): a PNG of the game picture, 2x for a small one\n");
	o->extra_args = more_args(args, a2, 64, "--screenshots", shots, "--hotkey-script", "20:shot,21:shot",
				  "--frames", "30", NULL);
	CHECK(host_launch(argv[2], rom, "test", o, &r) == 0 && r.status == HOST_EXIT_OK, "run with Select+L2 twice");
	{
		char dir[800], path[1100] = "";
		DIR *d;
		struct dirent *de;
		int n = 0, w = 0, h = 0, colors = 0;
		uint8_t *px = NULL;

		snprintf(dir, sizeof(dir), "%s/test", shots);
		d = opendir(dir);
		while (d && (de = readdir(d)))
			if (!strncmp(de->d_name, "game-", 5) && strstr(de->d_name, ".png")) {
				n++;
				snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
			}
		if (d)
			closedir(d);
		if (path[0])
			px = host_png_read_rgb(path, &w, &h);
		for (int i = 1; px && i < w * h; i++)
			colors += memcmp(px, px + 3 * i, 3) != 0;
		CHECK(n == 2 && px && w == 320 && h == 240 && colors > 1000,
		      "%d files in %s (the second one -2), %s decodes: %dx%d (the core's 160x120, 2x)", n, dir,
		      hpath_base(path), w, h);
		host_png_free(px);
	}
	o->extra_args = args;
}

int main(int argc, char **argv)
{
	struct host_launch_opts o;
	struct host_launch_result r;
	char bios[512], saves[512], states[512], opts[512], ship[512], info[512], tmp[512], log[512];
	char rom[512], srm[512];
	const char *args[32];
	int n = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: %s RSOS_RUN TESTCORE.so WORKDIR\n", argv[0]);
		return 2;
	}
	hlog_set_level(getenv("RSOS_TEST_VERBOSE") ? HLOG_INFO : HLOG_WARN);
	snprintf(bios, sizeof(bios), "%s/bios", argv[3]);
	snprintf(saves, sizeof(saves), "%s/saves", argv[3]);
	snprintf(states, sizeof(states), "%s/states", argv[3]);
	snprintf(opts, sizeof(opts), "%s/coreopts", argv[3]);
	snprintf(ship, sizeof(ship), "%s/ship", argv[3]);
	snprintf(info, sizeof(info), "%s/cores", argv[3]);
	snprintf(tmp, sizeof(tmp), "%s/tmp", argv[3]);
	snprintf(log, sizeof(log), "%s/game.log", argv[3]);
	snprintf(rom, sizeof(rom), "%s/roms/test/game.bin", argv[3]);
	snprintf(srm, sizeof(srm), "%s/saves/test/game.srm", argv[3]);
	hmkdir_p(info, 0755);
	{
		char d[512];

		snprintf(d, sizeof(d), "%s/roms/test", argv[3]);
		hmkdir_p(d, 0755);
		hwrite_atomic(rom, "x", 1, false);
	}
	args[n++] = "--headless";
	args[n++] = "--bios"; args[n++] = bios;
	args[n++] = "--saves"; args[n++] = saves;
	args[n++] = "--states"; args[n++] = states;
	args[n++] = "--coreopts"; args[n++] = opts;
	args[n++] = "--coreopts-ship"; args[n++] = ship;
	args[n++] = "--cores-info"; args[n++] = info;
	args[n++] = "--tmp"; args[n++] = tmp;
	args[n++] = "--settings"; args[n++] = "/dev/null";
	args[n++] = "--frames"; args[n++] = "3000";
	args[n] = NULL;
	memset(&o, 0, sizeof(o));
	o.exe = argv[1];
	o.extra_args = args;
	o.log_path = log;

	printf("process model (host_launch)\n");
	unsetenv("RSOS_TESTCORE");
	CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK && !r.crashed,
	      "normal run: status %d, exit %d", r.status, r.exit_code);
	CHECK(hfile_size(srm) == 1024, "SRAM written at exit (%lld bytes)", hfile_size(srm));

	{
		char info_ini[600];

		snprintf(info_ini, sizeof(info_ini), "%s/rsos-testcore.ini", info);
		hwrite_atomic(info_ini, "[bios:needed.bin]\nrequired = yes\n", 33, false);
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_ERROR &&
		      strstr(r.message, "needed.bin"),
		      "missing BIOS: status %d, message \"%s\"", r.status, r.message);
		remove(info_ini);
	}

	remove(srm);
	setenv("RSOS_TESTCORE", "segv", 1);
	CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.crashed && r.signal == 11,
	      "crash: crashed %d, signal %d, message \"%s\"", r.crashed, r.signal, r.message);

	setenv("RSOS_TESTCORE", "hang", 1);
	CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_HANG,
	      "hang: status %d, message \"%s\"", r.status, r.message);
	unsetenv("RSOS_TESTCORE");

	/*
	 * The auto state (resume): written at a normal exit unless Settings >
	 * Games > Auto-save on exit is off, always at a power-off, reported to
	 * the supervisor ("autostate"), never replaced when the core cannot
	 * save, and loaded by opts.resume (--load-state auto).
	 */
	printf("auto state (exit, power-off, resume)\n");
	{
		char set_on[600], set_off[600], st[600], png[600], *txt = NULL;
		const char *a2[48];
		int k;

		snprintf(set_on, sizeof(set_on), "%s/settings-on.ini", argv[3]);
		snprintf(set_off, sizeof(set_off), "%s/settings-off.ini", argv[3]);
		snprintf(st, sizeof(st), "%s/test/game.state.auto", states);
		snprintf(png, sizeof(png), "%s/test/game.state.auto.png", states);
		hwrite_atomic(set_on, "autosave_exit = 1\n", 18, false);
		hwrite_atomic(set_off, "autosave_exit = 0\n", 18, false);
		for (k = 0; args[k]; k++)
			a2[k] = !strcmp(args[k], "/dev/null") ? set_off : args[k];
		a2[k] = NULL;
		o.extra_args = a2;
		remove(st);
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK &&
		      !r.auto_state_saved && !hfile_exists(st),
		      "autosave_exit = 0: exit without an auto state (reported %d)", r.auto_state_saved);
		for (k = 0; args[k]; k++)
			a2[k] = !strcmp(args[k], "/dev/null") ? set_on : args[k];
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK &&
		      r.auto_state_saved && hfile_exists(st) && hfile_exists(png),
		      "autosave_exit = 1: .state.auto + thumbnail at exit, reported (%lld bytes, %d ms)",
		      r.auto_state_bytes, r.auto_state_ms);
		o.extra_args = args;   /* no key: the default is on */
		remove(st);
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.auto_state_saved && hfile_exists(st),
		      "no setting: auto-save on exit is the default");

		/* the core cannot save now: the previous state stays as it was */
		hwrite_atomic(st, "GOOD", 4, true);
		o.extra_args = a2;
		setenv("RSOS_TESTCORE", "nostate", 1);
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK && !r.auto_state_saved &&
		      (txt = hread_file(st, NULL)) && !strncmp(txt, "GOOD", 4),
		      "a core that cannot save: the previous .state.auto is kept");
		free(txt);
		txt = NULL;
		unsetenv("RSOS_TESTCORE");
		remove(st);

		/* power-off during the game, with the exit auto-save off */
		for (k = 0; args[k]; k++)
			a2[k] = !strcmp(args[k], "/dev/null") ? set_off : args[k];
		setenv("RSOS_TESTCORE", "slow", 1);
		o.idle = poweroff_after_300ms;
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_POWEROFF &&
		      r.auto_state_saved && hfile_exists(st),
		      "power-off during the game: .state.auto written anyway (status %d, reported %d)", r.status,
		      r.auto_state_saved);
		o.idle = NULL;
		unsetenv("RSOS_TESTCORE");

		/* resume from it */
		o.resume = true;
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK &&
		      (txt = hread_file(log, NULL)) && strstr(txt, "game.state.auto") && strstr(txt, "): ok"),
		      "opts.resume: the game starts from .state.auto");
		free(txt);
		txt = NULL;
		o.resume = false;
		o.extra_args = args;
		remove(st);
		remove(png);
	}

	/*
	 * Core options regression (owner report: "changed the GFX plugin in
	 * Select+X > Core options, restarted, it was back"): a value changed in
	 * the menu must reach the core at the next start, without an explicit
	 * "Save", and "Save for all games" must not be overridden by an older
	 * per-game value.
	 */
	printf("core options: menu change -> restart\n");
	{
		char gopt[600], sopt[600], *txt = NULL;
		const char *a2[48];
		int k = 0;

		snprintf(gopt, sizeof(gopt), "%s/rsos-testcore/game.ini", opts);
		snprintf(sopt, sizeof(sopt), "%s/rsos-testcore.ini", opts);
		remove(gopt);
		remove(sopt);
		for (k = 0; args[k]; k++)
			a2[k] = args[k];
		a2[k++] = "--menu-script";
		a2[k++] = "sel=Core options,a,sel=Speed,r,b,b";
		a2[k++] = "--frames";
		a2[k++] = "40";
		a2[k] = NULL;
		o.extra_args = a2;
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK,
		      "run 1: menu sets Speed = fast, then exits (status %d)", r.status);
		txt = hread_file(gopt, NULL);
		CHECK(txt && strstr(txt, "testcore_speed = \"fast\""), "saved for this game without \"Save\": %s",
		      txt ? "yes" : "no file");
		free(txt);
		txt = NULL;
		a2[k - 4] = "--frames"; /* run 2: no menu */
		a2[k - 3] = "40";
		a2[k - 2] = NULL;
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK &&
		      (txt = hread_file(log, NULL)) && strstr(txt, "testcore_speed=fast"),
		      "run 2: the core receives testcore_speed=fast");
		free(txt);
		txt = NULL;
		a2[k - 4] = "--menu-script";
		a2[k - 3] = "sel=Core options,a,sel=Speed,l,sel=Save for all,a,b,b";
		a2[k - 2] = "--frames";
		a2[k - 1] = "40";
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK,
		      "run 3: menu sets Speed = normal, \"Save for all games\"");
		txt = hread_file(gopt, NULL);
		CHECK(!txt || !strstr(txt, "testcore_speed"), "the older per-game value is gone");
		free(txt);
		txt = NULL;
		a2[k - 4] = "--frames";
		a2[k - 3] = "40";
		a2[k - 2] = NULL;
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && (txt = hread_file(log, NULL)) &&
		      strstr(txt, "testcore_speed=normal"),
		      "run 4: the core receives testcore_speed=normal");
		free(txt);
		txt = NULL;
		remove(gopt);
		remove(sopt);

		/* Review F-H3: the menu's own item ids (D-pad as stick, info rows)
		 * equalled core options #1 and #2. Option #1 (Cost) must step, and
		 * A on an info row (Controls > Player 1) must not touch option #2
		 * (Crash). */
		a2[k - 4] = "--menu-script";
		a2[k - 3] = "sel=Core options,a,sel=Cost,r,b,sel=Controls,a,sel=Player 1,a,b,b";
		a2[k - 2] = "--frames";
		a2[k - 1] = "40";
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK,
		      "run 5: option #1 stepped, A on an info row");
		txt = hread_file(gopt, NULL);
		CHECK(txt && strstr(txt, "testcore_cost = \"4\""), "core option #1 (Cost) stepped to 4: %s",
		      txt ? txt : "no file");
		CHECK(!txt || !strstr(txt, "testcore_crash"), "core option #2 (Crash) untouched by an info row");
		free(txt);
		txt = NULL;
		remove(gopt);
		remove(sopt);
		o.extra_args = args;
	}

	/*
	 * Benchmark orchestration (headless): the plan's three configurations
	 * each run in a fresh process from the start state (one crashes), the
	 * report and screenshots are written, the game restarts from the start
	 * state with the results, and "Use this" writes the per-game options.
	 */
	printf("benchmark (driver, runs, report, \"use this\")\n");
	{
		char plan[600], logs[600], gopt[600], *txt = NULL, report[900] = "";
		const char *a2[48];
		int k = 0, pngs = 0;
		DIR *d;
		struct dirent *de;

		snprintf(plan, sizeof(plan), "%s/test.bench.ini", ship);
		snprintf(logs, sizeof(logs), "%s/logs", argv[3]);
		snprintf(gopt, sizeof(gopt), "%s/rsos-testcore/game.ini", opts);
		hmkdir_p(ship, 0755);
		{
			static const char p[] =
				"[bench]\nseconds = 1\nwarmup = 1\n\n"
				"[slow]\nlabel = Slow (12 ms per frame)\ntestcore_cost = 12\n\n"
				"[fast]\nlabel = Fast (0 ms per frame)\ntestcore_cost = 0\n\n"
				"[crashes]\nlabel = Crashes at frame 30\ntestcore_crash = yes\n";

			hwrite_atomic(plan, p, sizeof(p) - 1, false);
		}
		for (k = 0; args[k]; k++)
			a2[k] = args[k];
		a2[k++] = "--bench-start";
		a2[k++] = "30";
		a2[k++] = "--bench-seconds";
		a2[k++] = "1";
		a2[k++] = "--bench-auto-apply";
		a2[k++] = "--logs";
		a2[k++] = logs;
		a2[k++] = "--frames";
		a2[k++] = "120";
		a2[k] = NULL;
		o.extra_args = a2;
		o.idle = sleep_wake_driver;
		CHECK(host_launch(argv[2], rom, "test", &o, &r) == 0 && r.status == HOST_EXIT_OK && !r.crashed,
		      "benchmark then resume: status %d, exit %d, crashed %d", r.status, r.exit_code, r.crashed);
		o.idle = NULL;
		CHECK(g_bench_sleeps == 1, "a sleep + wake during the benchmark did not kill the driver");
		txt = hread_file(log, NULL);
		CHECK(txt && strstr(txt, "bench driver: run 1/3") && strstr(txt, "bench driver: run 3/3"),
		      "the driver ran the three configurations");
		CHECK(txt && strstr(txt, "bench: run 2/3 \"fast\""), "each run in its own game process with its options");
		CHECK(txt && strstr(txt, "\"crashes\": crash"), "a crashing run is recorded, the benchmark goes on");
		CHECK(txt && strstr(txt, "start.state") && strstr(txt, "bench: best fast"),
		      "the game restarted from the start state with the results, best = fast");
		free(txt);
		txt = NULL;
		d = opendir(logs);
		while (d && (de = readdir(d))) {
			size_t l = strlen(de->d_name);

			if (l > 4 && !strcmp(de->d_name + l - 4, ".png"))
				pngs++;
			if (l > 4 && !strcmp(de->d_name + l - 4, ".txt"))
				snprintf(report, sizeof(report), "%s/%s", logs, de->d_name);
		}
		if (d)
			closedir(d);
		txt = report[0] ? hread_file(report, NULL) : NULL;
		CHECK(txt && strstr(txt, "Best: fast") && strstr(txt, "crash") && strstr(txt, "Ranking"),
		      "report on the card: %s", report[0] ? report : "missing");
		if (txt && getenv("RSOS_TEST_VERBOSE"))
			fputs(txt, stdout);
		free(txt);
		txt = NULL;
		CHECK(pngs >= 2, "a screenshot per completed run (%d)", pngs);
		txt = hread_file(gopt, NULL);
		CHECK(txt && strstr(txt, "testcore_cost = \"0\""), "\"Use this for this game\" wrote %s", gopt);
		free(txt);
		txt = NULL;
		o.extra_args = args;
	}

	test_batch2(argv, args, &o);
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
