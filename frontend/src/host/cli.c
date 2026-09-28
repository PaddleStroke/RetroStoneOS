/*
 * cli.c - the rsos-run command line (host_main), also reached as
 * `rsos-frontend --run ...` from the UI's fork+exec.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "host.h"
#include "bench.h"
#include "../input/input.h"
#include "hutil.h"

void host_usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s --core CORE.so [--rom FILE] [options]\n"
		"  --system NAME        ROM folder name (default: the ROM's parent directory)\n"
		"  --frames N           quit after N frames\n"
		"  --headless           no display, audio or input; runs as fast as possible\n"
		"  --dump FILE.png      write the last frame at exit\n"
		"  --menu-shot FILE.png render the in-game menu (and FILE-options.png) at exit\n"
		"  --test-states        with --frames N: save at N/2, replay, compare frame N\n"
		"  --scale MODE         aspect | integer | stretch\n"
		"  --stats              frame time / FPS overlay\n"
		"  --latency MS         audio buffer (default 64)\n"
		"  --vsync-tolerance P  vsync pacing window in %% (default 1)\n"
		"  --hdmi WxH           HDMI mode (default 1280x720)\n"
		"  --lcd-refresh HZ     LCD refresh: 60 (25.2 MHz pixel clock) or 78 (the panel's own mode;\n"
		"                       default: settings lcd_refresh)\n"
		"  --shader-cache DIR   Mesa shader cache for GL cores (default /data/rsos/cache/mesa, \"\" = off)\n"
		"  --p1 MODE            player 1: auto | builtin | external (default: settings p1)\n"
		"  --p1-device ID       the controller that launched the game is player 1 (auto policy)\n"
		"  --cz 0|1             C/Z buttons fitted: <system>-cz.ini remaps (default: settings)\n"
		"  --load-state SLOT    0-9 or auto\n"
		"  --load-state-file F  start from this state file\n"
		"  --menu-script STEPS  tests: drive the in-game menu (sel=Label,a,b,l,r,u,d)\n"
		"  --bench-start N      tests: start the benchmark at frame N (as from the menu)\n"
		"  --bench-seconds S    tests: measure S s per configuration, 1 s warm-up\n"
		"  --bench-auto-apply   tests: \"Use this for this game\" on the results\n"
		"  --logs DIR           benchmark reports (default /data/rsos/logs)\n"
		"  --autosave / --no-autosave   .state.auto at exit\n"
		"  --no-governor        leave the cpufreq governor alone\n"
		"  --status-fd N        status lines for the parent (error/warn/running/poweroff)\n"
		"  --poweroff-cmd CMD   run after a power-off flush when there is no parent\n"
		"  --bios DIR --saves DIR --states DIR --coreopts DIR --coreopts-ship DIR\n"
		"  --cores-info DIR --tmp DIR --settings FILE     path overrides (tests)\n"
		"  --battery-file FILE  the supervisor's battery value (default /run/rsos/battery)\n"
		"  --lang CODE          menu and message language: en, fr, pt_BR... (default: settings language)\n"
		"  --locale DIR         translations <lang>.cat (default /usr/share/rsos/locale)\n"
		"  --fonts DIR          TrueType fonts (default /usr/share/rsos/fonts; none: 8x8 ASCII text)\n"
		"  --screenshots DIR    Select+L2 screenshots (default /data/screenshots)\n"
		"  --switcher FILE      the recent games of the game switcher (Select+Y), written by the menu\n"
		"  --switcher-shot F.png  tests: render the game switcher at exit\n"
		"  --switcher-pick N    tests: at frame 12, leave for switcher entry N (as if picked)\n"
		"  --hotkey-script S    tests: \"F:name,...\" hotkeys at displayed frame F (ff, shot, menu,\n"
		"                       save, load, exit, mark = log the time)\n"
		"  --timer-pacing       tests: pace on the clock at the core's rate (with --headless)\n"
		"  --playtime-report S  \"playtime\" status line every S s (default 300)\n"
		"  --ff-speed N         fast-forward speed of Select+R2, 2-4 (default: settings ff_speed, 3)\n"
		"  --rumble 0|1         controller vibration (default: settings rumble, on)\n"
		"  --cpu-profile P      this game's CPU profile, shown in the menu: auto | performance | powersave\n"
		"  -v / -q              more / less logging\n",
		argv0);
}

static int saved_argc;
static char *const *saved_argv;

int host_saved_args(char *const **argv)
{
	*argv = saved_argv;
	return saved_argc;
}

int host_main(int argc, char **argv)
{
	struct host_config c;
	const char *driver_plan = NULL;

	saved_argc = argc;
	saved_argv = argv;

	host_config_defaults(&c);
	c.poweroff_cmd = "/sbin/poweroff";
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && v && (i++, 1))

		if (ARG("--core"))
			c.core_path = v;
		else if (ARG("--rom"))
			c.rom_path = v;
		else if (ARG("--system"))
			c.system = v;
		else if (ARG("--frames"))
			c.max_frames = atol(v);
		else if (!strcmp(a, "--headless"))
			c.headless = true;
		else if (ARG("--dump"))
			c.dump_png = v;
		else if (ARG("--menu-shot"))
			c.menu_shot = v;
		else if (!strcmp(a, "--test-states"))
			c.test_states = true;
		else if (ARG("--scale"))
			c.scale = !strcasecmp(v, "integer") ? DISPLAY_SCALE_INTEGER :
				  !strcasecmp(v, "stretch") ? DISPLAY_SCALE_STRETCH : DISPLAY_SCALE_ASPECT;
		else if (!strcmp(a, "--stats"))
			c.show_stats = 1;
		else if (ARG("--latency"))
			c.audio_latency_ms = atoi(v);
		else if (ARG("--vsync-tolerance"))
			c.vsync_tolerance = atof(v) / 100.0;
		else if (ARG("--p1"))
			c.p1_policy = !strcasecmp(v, "builtin") ? INPUT_P1_BUILTIN :
				      !strcasecmp(v, "external") ? INPUT_P1_EXTERNAL : INPUT_P1_AUTO;
		else if (ARG("--p1-device"))
			c.p1_device = v;
		else if (ARG("--load-state-file"))
			c.load_state_file = v;
		else if (ARG("--menu-script"))
			c.menu_script = v;
		else if (ARG("--logs"))
			c.logs_dir = v;
		else if (ARG("--bench-start"))
			c.bench_start_frame = atol(v);
		else if (ARG("--bench-seconds"))
			c.bench_seconds = atoi(v);
		else if (!strcmp(a, "--bench-auto-apply"))
			c.bench_auto_apply = true;
		else if (ARG("--bench-driver"))
			driver_plan = v;
		else if (ARG("--bench-report")) {
			c.bench_plan = v;
			c.bench_report = true;
		} else if (!strcmp(a, "--bench-step") && i + 2 < argc) {
			c.bench_plan = argv[++i];
			c.bench_step = atoi(argv[++i]);
		} else if (ARG("--cz"))
			c.cz_buttons = atoi(v) ? 1 : 0;
		else if (ARG("--hdmi"))
			sscanf(v, "%dx%d", &c.hdmi_width, &c.hdmi_height);
		else if (ARG("--lcd-refresh"))
			c.lcd_refresh = atoi(v);
		else if (ARG("--shader-cache"))
			c.shader_cache_dir = v;
		else if (ARG("--load-state"))
			c.load_slot = !strcasecmp(v, "auto") ? -1 : atoi(v);
		else if (!strcmp(a, "--autosave"))
			c.autosave = 1;
		else if (!strcmp(a, "--no-autosave"))
			c.autosave = 0;
		else if (!strcmp(a, "--no-governor"))
			c.governor = false;
		else if (ARG("--status-fd"))
			c.status_fd = atoi(v);
		else if (ARG("--poweroff-cmd"))
			c.poweroff_cmd = v;
		else if (ARG("--bios"))
			c.bios_dir = v;
		else if (ARG("--saves"))
			c.saves_root = v;
		else if (ARG("--states"))
			c.states_root = v;
		else if (ARG("--coreopts"))
			c.coreopts_user = v;
		else if (ARG("--coreopts-ship"))
			c.coreopts_ship = v;
		else if (ARG("--cores-info"))
			c.core_info_dir = v;
		else if (ARG("--tmp"))
			c.tmp_dir = v;
		else if (ARG("--settings"))
			c.settings_path = v;
		else if (ARG("--battery-file"))
			c.battery_path = v;
		else if (ARG("--lang"))
			c.lang = v;
		else if (ARG("--locale"))
			c.locale_dir = v;
		else if (ARG("--fonts"))
			c.fonts_dir = v;
		else if (ARG("--screenshots"))
			c.screenshots_root = v;
		else if (ARG("--switcher"))
			c.switcher_path = v;
		else if (ARG("--switcher-shot"))
			c.switcher_shot = v;
		else if (ARG("--switcher-pick"))
			c.switcher_pick = atoi(v);
		else if (ARG("--hotkey-script"))
			c.hotkey_script = v;
		else if (!strcmp(a, "--timer-pacing"))
			c.timer_pacing = true;
		else if (ARG("--playtime-report"))
			c.playtime_report_s = atoi(v);
		else if (ARG("--ff-speed"))
			c.ff_speed = atoi(v);
		else if (ARG("--rumble"))
			c.rumble = atoi(v) ? 1 : 0;
		else if (ARG("--cpu-profile"))
			c.cpu_profile = v;
		else if (!strcmp(a, "-v"))
			c.log_level = HLOG_DEBUG;
		else if (!strcmp(a, "-q"))
			c.log_level = HLOG_WARN;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			host_usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "%s: unknown or incomplete argument %s\n", argv[0], a);
			host_usage(argv[0]);
			return HOST_EXIT_USAGE;
		}
#undef ARG
	}
	if (!c.core_path) {
		host_usage(argv[0]);
		return HOST_EXIT_USAGE;
	}
	if (c.headless)
		c.governor = false;
	if (driver_plan)
		return bench_driver_main(&c, driver_plan);
	return host_run(&c);
}
