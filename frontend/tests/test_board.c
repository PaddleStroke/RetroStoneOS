/*
 * test_board.c - the board profile (src/board.h, docs/porting.md).
 *
 *   test_board WORKDIR RETROSTONE2_BOARD_INI [OTHER_BOARD_INI...]
 *
 * 1. The RetroStone2 profile (buildroot-external/board/retrostone2/
 *    rootfs-overlay/etc/rsos/board.ini) gives exactly the constants the
 *    code used before the profile existed, and the module configurations
 *    built from it equal the module defaults (which are those constants).
 * 2. The power module, run on the same fake sysfs tree once with its
 *    defaults and once with the RetroStone2 profile applied, does the same
 *    things: PEK timings, V_OFF, governor, battery and backlight found.
 * 3. No profile at all: the generic defaults (no built-in pad, auto power
 *    key, no PEK timings, no storage overlays).
 * 4. Parser details; any other board.ini given on the command line parses
 *    (the Raspberry Pi 4 one: no built-in screen).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board.h"
#include "display.h"
#include "input/input.h"
#include "power/power.h"
#include "power/psys.h"
#include "ui/ui.h"
#include "board_apply.h"

static int g_fail, g_checks;
static char W[512];

#define CHECK(c, ...) do { g_checks++; if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static bool streq(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	return !strcmp(a, b);
}

/* DRM_MODE_CONNECTOR_* */
#define T_UNKNOWN 0
#define T_HDMIA 11
#define T_DSI 16
#define T_DPI 17
#define T_LVDS 7
#define T_COMPOSITE 5

/* ------------------------------------------------------------ 1. values */
static void test_retrostone2_values(const struct board_profile *b)
{
	struct input_config ic, idef;
	struct power_config pc, pdef;
	struct display_config dc, ddef;
	struct ui_config uc, udef;

	printf("1. RetroStone2 profile = the constants of the code\n");
	CHECK(b->loaded, "loaded %s", b->path);
	CHECK(streq(b->name, "RetroStone2"), "name %s", b->name);
	CHECK(streq(b->builtin_pad_prefix, "RetroStone2"), "builtin_pad_prefix %s", b->builtin_pad_prefix);
	CHECK(streq(b->builtin_stick, "analog-stick"), "builtin_stick %s", b->builtin_stick);
	CHECK(streq(b->builtin_pad_name, "RetroStone2 built-in"), "pad name %s", b->builtin_pad_name);
	CHECK(b->internal_display == BOARD_INTERNAL_LIST, "internal_display list");
	CHECK(board_connector_is_internal(b, T_UNKNOWN) && board_connector_is_internal(b, T_DPI),
	      "Unknown (sun4i_rgb) and DPI are the panel");
	CHECK(!board_connector_is_internal(b, T_HDMIA), "HDMI is external");
	CHECK(b->refresh_native == 78 && b->refresh_60, "refresh options 78/60: %d %d", b->refresh_native,
	      b->refresh_60);
	CHECK(streq(b->backlight, ""), "backlight auto");
	CHECK(streq(b->battery_supply, "axp20x-battery") && streq(b->ac_supply, "axp20x-ac") &&
	      streq(b->usb_supply, "axp20x-usb"), "AXP209 supplies");
	CHECK(b->battery_voff_mv == 3000, "V_OFF %d", b->battery_voff_mv);
	CHECK(streq(b->power_key_device, "axp20x-pek"), "power key %s", b->power_key_device);
	CHECK(b->pek_startup_ms == 128, "PEK startup %d", b->pek_startup_ms);
	CHECK(streq(b->audio_internal_dev, "") && streq(b->audio_hdmi_dev, "") && streq(b->audio_hdmi_pcm, ""),
	      "audio auto, plughw");
	CHECK(streq(b->cpu_governor_menu, "schedutil") && streq(b->cpu_governor_game, "performance"),
	      "governors %s/%s", b->cpu_governor_menu, b->cpu_governor_game);
	CHECK(board_has_storage_overlay(b, "emmc") && board_has_storage_overlay(b, "sata") &&
	      !board_has_storage_overlay(b, "sat"), "storage overlays %s", b->storage_overlays);
	CHECK(b->display_quirks == (BOARD_QUIRK_SUN4I_TCON0_CLOCK | BOARD_QUIRK_PANEL_KEEP_SCANNING), "quirks 0x%x",
	      b->display_quirks);

	/* input: the profile gives the defaults (= the old constants) */
	input_config_defaults(&idef);
	input_config_defaults(&ic);
	board_apply_input(b, &ic);
	CHECK(streq(ic.builtin_prefix, idef.builtin_prefix) && streq(ic.builtin_prefix, "RetroStone2"),
	      "input builtin_prefix %s", ic.builtin_prefix);
	CHECK(streq(ic.builtin_stick, idef.builtin_stick), "input builtin_stick %s", ic.builtin_stick);
	CHECK(streq(ic.builtin_name, idef.builtin_name), "input builtin_name %s", ic.builtin_name);
	CHECK(streq(ic.power_key_name, idef.power_key_name) && streq(ic.power_key_name, "axp20x-pek"),
	      "input power_key_name %s", ic.power_key_name);
	CHECK(ic.backlight_name == NULL && idef.backlight_name == NULL, "input backlight: the first");

	/* power */
	power_config_defaults(&pdef);
	power_config_defaults(&pc);
	board_apply_power(b, &pc);
	CHECK(streq(pc.builtin_prefix, pdef.builtin_prefix), "power builtin_prefix %s", pc.builtin_prefix);
	CHECK(streq(pc.pek_name, pdef.pek_name), "power pek_name %s", pc.pek_name);
	CHECK(pc.pek_startup_ms == pdef.pek_startup_ms && pc.pek_startup_ms == 128, "power pek_startup_ms %d",
	      pc.pek_startup_ms);
	CHECK(pc.axp_voff_mv == pdef.axp_voff_mv, "power axp_voff_mv %d", pc.axp_voff_mv);
	CHECK(streq(pc.menu_governor, pdef.menu_governor) && streq(pc.game_governor, pdef.game_governor),
	      "power governors %s/%s", pc.menu_governor, pc.game_governor);
	CHECK(pc.backlight_name == NULL && pc.thermal_type == NULL, "power backlight/thermal auto");

	/* display */
	display_config_defaults(&ddef);
	display_config_defaults(&dc);
	board_apply_display(b, &dc);
	CHECK(ddef.internal_mode == DISPLAY_INTERNAL_AUTO && dc.internal_mode == DISPLAY_INTERNAL_LIST,
	      "display: panel types listed");
	CHECK((dc.internal_types & (1u << T_UNKNOWN)) && (dc.internal_types & (1u << T_DPI)) &&
	      !(dc.internal_types & (1u << T_HDMIA)), "display: Unknown + DPI");
	CHECK(dc.a20_clock_log == ddef.a20_clock_log, "display: A20 clock log");
	CHECK(dc.panel_keep_scanning && ddef.panel_keep_scanning, "display: the panel keeps scanning (no power switch)");
	CHECK(dc.backlight_name == NULL && ddef.backlight_name == NULL, "display: backlight auto");
	CHECK(dc.tv_norm == ddef.tv_norm && dc.tv_overscan == ddef.tv_overscan && !(dc.internal_types & (1u << T_COMPOSITE)),
	      "display: no composite screen, TV settings at their defaults");

	/* UI */
	ui_config_defaults(&udef);
	ui_config_defaults(&uc);
	board_apply_ui(b, &uc);
	CHECK(uc.has_internal_display == udef.has_internal_display && uc.has_internal_display,
	      "ui: built-in screen");
	CHECK(uc.lcd_refresh_choice == udef.lcd_refresh_choice && uc.lcd_refresh_choice, "ui: 78/60 Hz choice");
	CHECK(streq(uc.storage_overlays, udef.storage_overlays), "ui: storage overlays %s", uc.storage_overlays);
}

/* -------------------------------------------------- 2. power module run */
static void mkdirs(const char *path)
{
	char tmp[1024];

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

static void wfile(const char *root, const char *rel, const char *content)
{
	char path[1024], dir[1024];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	snprintf(dir, sizeof(dir), "%s", path);
	*strrchr(dir, '/') = 0;
	mkdirs(dir);
	f = fopen(path, "w");
	if (!f) {
		printf("cannot write %s: %s\n", path, strerror(errno));
		exit(2);
	}
	fputs(content, f);
	fclose(f);
}

static const char *rfile(const char *root, const char *rel)
{
	static char buf[8][128];
	static int k;
	char path[1024];
	char *b = buf[k++ % 8];

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	if (psys_read_str(path, b, 128))
		snprintf(b, 128, "<missing>");
	return b;
}

#define BAT "sys/class/power_supply/axp20x-battery"
#define AC "sys/class/power_supply/axp20x-ac"
#define USB "sys/class/power_supply/axp20x-usb"
#define PEK "sys/bus/platform/drivers/axp20x-pek/axp20x-pek"
#define POL "sys/devices/system/cpu/cpufreq/policy0"

static void make_tree(const char *root)
{
	char cmd[1200];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd) != 0)
		exit(2);
	wfile(root, BAT "/type", "Battery\n");
	wfile(root, BAT "/present", "1\n");
	wfile(root, BAT "/voltage_min", "2900000\n");
	wfile(root, BAT "/voltage_now", "3950000\n");
	wfile(root, BAT "/current_now", "-400000\n");
	wfile(root, BAT "/status", "Discharging\n");
	wfile(root, BAT "/capacity", "80\n");
	wfile(root, AC "/type", "Mains\n");
	wfile(root, AC "/online", "0\n");
	wfile(root, USB "/type", "USB\n");
	wfile(root, USB "/online", "0\n");
	wfile(root, "sys/class/backlight/backlight/brightness", "50\n");
	wfile(root, "sys/class/backlight/backlight/max_brightness", "100\n");
	wfile(root, "sys/class/backlight/backlight/bl_power", "0\n");
	wfile(root, POL "/scaling_governor", "performance\n");
	wfile(root, POL "/scaling_max_freq", "960000\n");
	wfile(root, POL "/cpuinfo_max_freq", "960000\n");
	wfile(root, POL "/cpuinfo_min_freq", "144000\n");
	wfile(root, PEK "/shutdown", "4000\n");
	wfile(root, PEK "/startup", "1000\n");
	wfile(root, "sys/class/thermal/thermal_zone0/type", "cpu-thermal\n");
	wfile(root, "sys/class/thermal/thermal_zone0/temp", "45000\n");
	snprintf(cmd, sizeof(cmd), "%s/dev/input", root);
	mkdirs(cmd);
	snprintf(cmd, sizeof(cmd), "%s/run", root);
	mkdirs(cmd);
}

static int64_t g_now = 100000;
static int64_t fake_ms(void) { return g_now; }
static time_t fake_wall(void) { return (time_t)1790000000; }
static int no_poweroff(bool reboot, void *u) { (void)reboot; (void)u; return 0; }

struct outcome {
	char startup[32], shutdown[32], governor[32], voff[32];
	bool battery;
	int percent;
};

static void run_power(const char *root, const struct board_profile *b, struct outcome *o)
{
	static char sysfs[600], dev[600], run[600];
	struct power_config c;
	int r;

	make_tree(root);
	snprintf(sysfs, sizeof(sysfs), "%s/sys", root);
	snprintf(dev, sizeof(dev), "%s/dev/input", root);
	snprintf(run, sizeof(run), "%s/run", root);
	power_config_defaults(&c);
	if (b)
		board_apply_power(b, &c);
	c.sysfs = sysfs;
	c.dev_input = dev;
	c.run_dir = run;
	c.clock_file = "/nonexistent-clock";
	c.rtc_dev = "/nonexistent-rtc";
	c.now_ms = fake_ms;
	c.wall_now = fake_wall;
	c.do_poweroff = no_poweroff;
	c.no_uevent = true;
	r = power_init(&c);
	CHECK(r == 0, "power_init %d", r);
	snprintf(o->startup, sizeof(o->startup), "%s", rfile(root, PEK "/startup"));
	snprintf(o->shutdown, sizeof(o->shutdown), "%s", rfile(root, PEK "/shutdown"));
	snprintf(o->governor, sizeof(o->governor), "%s", rfile(root, POL "/scaling_governor"));
	snprintf(o->voff, sizeof(o->voff), "%s", rfile(root, BAT "/voltage_min"));
	o->battery = power_get_status()->battery_present;
	o->percent = power_get_status()->percent;
	power_exit();
}

static void test_power_equivalence(const struct board_profile *rs2)
{
	struct outcome def, prof, gen;
	struct board_profile generic;
	char root[600];

	printf("2. power module: defaults vs the RetroStone2 profile vs no profile\n");
	snprintf(root, sizeof(root), "%s/power", W);
	run_power(root, NULL, &def);
	run_power(root, rs2, &prof);
	CHECK(!strcmp(def.startup, prof.startup) && !strcmp(def.startup, "128"), "PEK startup %s / %s",
	      def.startup, prof.startup);
	CHECK(!strcmp(def.shutdown, prof.shutdown) && !strcmp(def.shutdown, "6000"), "PEK shutdown %s / %s",
	      def.shutdown, prof.shutdown);
	CHECK(!strcmp(def.governor, prof.governor) && !strcmp(def.governor, "schedutil"), "governor %s / %s",
	      def.governor, prof.governor);
	CHECK(!strcmp(def.voff, prof.voff) && !strcmp(def.voff, "3000000"), "V_OFF %s / %s", def.voff, prof.voff);
	CHECK(def.battery && prof.battery && def.percent == prof.percent, "battery %d %% / %d %%", def.percent,
	      prof.percent);

	/* A generic board on the same tree: the battery is still found (auto),
	 * nothing AXP-specific is written. */
	board_defaults(&generic);
	run_power(root, &generic, &gen);
	CHECK(!strcmp(gen.startup, "1000") && !strcmp(gen.shutdown, "4000"), "generic: PEK left alone (%s, %s)",
	      gen.startup, gen.shutdown);
	CHECK(!strcmp(gen.voff, "2900000"), "generic: V_OFF left alone (%s)", gen.voff);
	CHECK(!strcmp(gen.governor, "schedutil"), "generic: governor %s", gen.governor);
	CHECK(gen.battery, "generic: battery auto-detected");

	/* battery_supply = none: no battery UI even if a supply exists */
	{
		struct board_profile nb;
		struct outcome o;

		board_defaults(&nb);
		board_parse(&nb, "battery_supply = none\n");
		run_power(root, &nb, &o);
		CHECK(!o.battery, "battery_supply=none: no battery");
		/* a named supply that is missing falls back to auto-detection */
		board_defaults(&nb);
		board_parse(&nb, "battery_supply = bq27546-0\n");
		run_power(root, &nb, &o);
		CHECK(o.battery && o.percent == def.percent, "missing named battery: auto (%d %%)", o.percent);
	}
}

/* ------------------------------------------------------ 3. no profile */
static void test_generic(void)
{
	struct board_profile b;
	char path[600];
	struct input_config ic;
	struct power_config pc;
	struct ui_config uc;
	int r;

	printf("3. no board.ini: generic defaults\n");
	snprintf(path, sizeof(path), "%s/does-not-exist.ini", W);
	r = board_load(&b, path);
	CHECK(r == -ENOENT && !b.loaded, "missing file: %d", r);
	CHECK(streq(b.name, "Generic") && !b.builtin_pad_prefix[0] && !b.builtin_stick[0], "no built-in pad");
	CHECK(b.internal_display == BOARD_INTERNAL_AUTO && board_connector_is_internal(&b, T_DSI) &&
	      !board_connector_is_internal(&b, T_HDMIA), "internal display auto");
	CHECK(!b.refresh_native && !b.storage_overlays[0] && !b.pek_startup_ms && !b.battery_voff_mv,
	      "no refresh choice, overlays, PEK timings, V_OFF");
	input_config_defaults(&ic);
	board_apply_input(&b, &ic);
	CHECK(streq(ic.builtin_prefix, "") && streq(ic.builtin_stick, "") && streq(ic.power_key_name, ""),
	      "input: nothing built in");
	power_config_defaults(&pc);
	board_apply_power(&b, &pc);
	CHECK(pc.pek_name == NULL && streq(pc.builtin_prefix, ""), "power: power key auto-detected");
	ui_config_defaults(&uc);
	board_apply_ui(&b, &uc);
	CHECK(uc.has_internal_display && !uc.lcd_refresh_choice && streq(uc.storage_overlays, ""),
	      "ui: no LCD refresh choice, no overlays");
	{
		struct display_config dc;

		display_config_defaults(&dc);
		board_apply_display(&b, &dc);
		CHECK(!b.display_quirks && !dc.panel_keep_scanning, "display: no quirk, the panel CRTC may stop");
	}
}

/* ------------------------------------------------------ 4. parser */
static void test_parser(void)
{
	struct board_profile b;

	printf("4. parser\n");
	board_defaults(&b);
	board_parse(&b, "; comment\n[board]\n  name = \"My Board\"   # inline\n"
		    "internal_display = none\npower_key_device = NONE\naudio_hdmi = vc4hdmi0\n"
		    "cpu_governor_menu = ondemand\ncpu_governor_game =\nunknown_key = 1\nbroken line\n"
		    "backlight = AUTO\ninternal_refresh_options = 60\n");
	CHECK(streq(b.name, "My Board"), "quoted name, inline comment: '%s'", b.name);
	CHECK(streq(b.builtin_pad_name, "My Board built-in"), "pad name %s", b.builtin_pad_name);
	CHECK(b.internal_display == BOARD_INTERNAL_NONE && !board_connector_is_internal(&b, T_UNKNOWN),
	      "internal_display none");
	CHECK(streq(board_name_or_auto(b.power_key_device), ""), "power key none -> \"\"");
	CHECK(streq(b.audio_hdmi_dev, "plughw:CARD=vc4hdmi0,0") && !b.audio_internal_dev[0], "audio %s",
	      b.audio_hdmi_dev);
	CHECK(streq(b.cpu_governor_menu, "ondemand") && streq(b.cpu_governor_game, "performance"),
	      "governors (empty keeps the default)");
	CHECK(board_name_or_auto(b.backlight) == NULL, "backlight AUTO");
	CHECK(b.refresh_native == 60 && !b.refresh_60, "one refresh option: no choice");
	board_defaults(&b);
	board_parse(&b, "audio_hdmi = vc4hdmi1\naudio_hdmi_pcm = hdmi\n");
	CHECK(streq(b.audio_hdmi_dev, "hdmi:CARD=vc4hdmi1,DEV=0"), "audio hdmi pcm: %s", b.audio_hdmi_dev);
	board_defaults(&b);
	board_parse(&b, "internal_display = dsi,lvds\n");
	CHECK(b.internal_display == BOARD_INTERNAL_LIST && board_connector_is_internal(&b, T_DSI) &&
	      board_connector_is_internal(&b, T_LVDS) && !board_connector_is_internal(&b, T_UNKNOWN),
	      "dsi,lvds");
	board_defaults(&b);
	board_parse(&b, "internal_display = bogus\n");
	CHECK(b.internal_display == BOARD_INTERNAL_NONE, "unknown connector names only: none");
	/* the built-in composite screen */
	board_defaults(&b);
	CHECK(b.tv_norm == BOARD_TV_NTSC && b.tv_overscan == 0, "TV defaults: NTSC, no overscan");
	board_parse(&b, "internal_display = composite\ntv_norm = PAL\ntv_overscan = 50\n");
	CHECK(b.internal_display == BOARD_INTERNAL_LIST && board_connector_is_internal(&b, T_COMPOSITE) &&
	      !board_connector_is_internal(&b, T_HDMIA) && !board_connector_is_internal(&b, T_UNKNOWN),
	      "composite is the built-in screen");
	CHECK(b.tv_norm == BOARD_TV_PAL && b.tv_overscan == 20, "tv_norm PAL, overscan capped: %d", b.tv_overscan);
	board_parse(&b, "tv_norm = auto\ntv_overscan = -3\n");
	CHECK(b.tv_norm == BOARD_TV_AUTO && b.tv_overscan == 0, "tv_norm auto, overscan floor");
	board_parse(&b, "tv_norm = secam\n");
	CHECK(b.tv_norm == BOARD_TV_NTSC, "unknown tv_norm: NTSC");
	board_parse(&b, "display_quirks = panel-keep-scanning, bogus\n");
	CHECK(b.display_quirks == BOARD_QUIRK_PANEL_KEEP_SCANNING, "quirk list: 0x%x", b.display_quirks);
	board_parse(&b, "display_quirks =\n");
	CHECK(!b.display_quirks, "no quirks");
	{
		struct display_config dc;

		display_config_defaults(&dc);
		CHECK(dc.tv_norm == DISPLAY_TV_NTSC && dc.tv_overscan == 0, "display defaults: NTSC, 0");
		board_parse(&b, "tv_norm = pal\ntv_overscan = 4\n");
		board_apply_display(&b, &dc);
		CHECK(dc.tv_norm == DISPLAY_TV_PAL && dc.tv_overscan == 4 && dc.internal_mode == DISPLAY_INTERNAL_LIST &&
		      (dc.internal_types & (1u << T_COMPOSITE)), "applied to the display config");
	}
}

static void test_other(const char *path)
{
	struct board_profile b;
	int r = board_load(&b, path);

	printf("5. %s\n", path);
	CHECK(r == 0 && b.loaded, "loads (%d)", r);
	CHECK(strcmp(b.name, "Generic"), "has a name: %s", b.name);
	if (strstr(path, "rpi4")) {
		CHECK(b.internal_display == BOARD_INTERNAL_NONE, "Pi 4: no built-in screen");
		CHECK(!b.builtin_pad_prefix[0] && !b.refresh_native && !b.storage_overlays[0],
		      "Pi 4: no built-in pad, refresh choice, overlays");
		CHECK(streq(board_name_or_auto(b.battery_supply), ""), "Pi 4: no battery");
	}
	if (strstr(path, "retrostone1")) {
		struct display_config dc;
		struct ui_config uc;

		CHECK(b.internal_display == BOARD_INTERNAL_LIST && board_connector_is_internal(&b, T_COMPOSITE) &&
		      !board_connector_is_internal(&b, T_HDMIA), "RetroStone1: the composite output is the built-in screen");
		CHECK(b.tv_norm == BOARD_TV_NTSC && b.tv_overscan == 0, "RetroStone1: NTSC, no overscan");
		CHECK(streq(board_name_or_auto(b.battery_supply), "") && streq(board_name_or_auto(b.backlight), ""),
		      "RetroStone1: no battery, no backlight");
		display_config_defaults(&dc);
		board_apply_display(&b, &dc);
		CHECK(dc.internal_mode == DISPLAY_INTERNAL_LIST && dc.internal_types == (1u << T_COMPOSITE) &&
		      dc.tv_norm == DISPLAY_TV_NTSC, "RetroStone1: display config");
		CHECK(!dc.panel_keep_scanning, "RetroStone1: no panel-keep-scanning (the AMT630A drives the TFT)");
		ui_config_defaults(&uc);
		board_apply_ui(&b, &uc);
		CHECK(!uc.has_internal_display && !uc.lcd_refresh_choice,
		      "RetroStone1: no Brightness (the AMT630A drives the backlight), no refresh choice");
	}
}

int main(int argc, char **argv)
{
	struct board_profile rs2;
	int r;

	if (argc < 3) {
		fprintf(stderr, "usage: %s WORKDIR RETROSTONE2_BOARD_INI [OTHER...]\n", argv[0]);
		return 2;
	}
	snprintf(W, sizeof(W), "%s", argv[1]);
	mkdirs(W);
	r = board_load(&rs2, argv[2]);
	if (r) {
		printf("cannot read %s: %s\n", argv[2], strerror(-r));
		return 1;
	}
	test_retrostone2_values(&rs2);
	test_power_equivalence(&rs2);
	test_generic();
	test_parser();
	for (int i = 3; i < argc; i++)
		test_other(argv[i]);
	printf("test_board: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
