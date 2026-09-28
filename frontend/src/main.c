/*
 * main.c - rsos-frontend: the RetroStoneOS menu process.
 *
 * One binary, two roles:
 *   rsos-frontend --run ...   the game process (libretro host, host_main())
 *   rsos-frontend             the supervisor: display, input, UI, power,
 *                             USB import, and the launch of each game in a
 *                             child process (fork + exec of itself --run).
 *
 * Boot order (fastest first frame):
 *   display_init -> input_open -> ui_create -> first frame -> power_init
 *   -> transfer_usb_init. The web share is never started here (only from
 *   its screen). The input layer is opened before ui_create() because the
 *   UI binds it at creation (brightness, controller screens); it costs a
 *   readdir of /dev/input and a few opens.
 *
 * One poll() loop over the display, input, power and USB fds plus a signal
 * eventfd; the timeout is the minimum of the modules' timeouts.
 *
 * Signals: SIGTERM/SIGINT/SIGHUP (rcK, init) -> clean exit with the UI state
 * flushed (forwarded to a running game first). SIGCHLD keeps its default
 * disposition and nothing here calls waitpid(-1): host_launch() and the UI's
 * helper jobs wait for their own pids, and the helpers started here
 * (rsos-boot-ok, rsos-net apply) are double-forked.
 *
 * Logs go to stderr (init sends it to the console) and are copied to
 * /run/rsos/frontend.log; each game's own log is appended to
 * /run/rsos/game.log. /run/rsos/menu-up is written once the menu is on
 * screen (rcS's deferred jobs and the boot logger wait for it).
 *
 * Test mode (development host, no DRM): --headless [WxH] with --root DIR
 * runs this same main loop against an in-memory surface, fake sysfs/dev
 * trees under DIR and a scripted button sequence (--script), and launches
 * games as `--run --headless`. See the Makefile's check-frontend target.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <linux/watchdog.h>

#include "board.h"
#include "display.h"
#include "font8x8.h"
#include "gfx/font.h"
#include "host/host.h"
#include "host/host_png.h"
#include "i18n/i18n.h"
#include "splash.h"
#include "ui/settings.h"
#include "ui/ui.h"
#include "ui/util.h"
#include "board_apply.h"   /* after ui/ui.h: board_apply_ui() */

#define BOOT_OK_PATH      "/usr/bin/rsos-boot-ok"
#define NET_HELPER_PATH   "/usr/bin/rsos-net"
#define LOG_PATH          "/run/rsos/frontend.log"
#define GAME_LOG_PATH     "/run/rsos/game.log"   /* the game child's stderr (appended, one header per launch) */
#define MENU_UP_PATH      "/run/rsos/menu-up"    /* written once the menu is on screen */
#define CHILD_GRACE_MS    8000   /* game child: time to save after SIGTERM/SIGUSR2 */
#define HANG_KILL_MS      5000   /* Select+Start held this long: kill the game */
#define SHUTDOWN_MSG_MS   0      /* no hold: the "Powering off..." frame is drawn at the
				    next loop pass, then the state is flushed and init
				    signalled right away (docs/power.md, shutdown) */
#define SHUTDOWN_START_PATH "/run/rsos/shutdown-start"   /* uptime of the request (rcK) */
#define SHUTDOWN_MODE_PATH  "/run/rsos/shutdown-mode"    /* "poweroff" or "reboot" */
#define SHUTDOWN_SIGNAL_PATH "/run/rsos/shutdown-signal" /* uptime when init was signalled */
#define POWER_LATE_MS     2000   /* power_init at the first frame, or this late */
#define DISPLAY_RETRY_MS  500
#define USB_FAST_MS       3000   /* after a USB uevent: poll often (settle delay) */
#define BATTERY_FILE      "/run/rsos/battery"  /* "<percent> <charging>" for the game child */
#define SCRIPT_PEK_UP     1000   /* pending_release: the power key, not a button */
#define RESUME_PATH       "/data/rsos/resume.ini"  /* the game running at the last power-off */
#define WATCHDOG_PATH     "/dev/watchdog"
#define WATCHDOG_PET_MS   2000   /* the A20's hardware maximum is 16 s; we pet every 2 s,
				    and nothing in the menu loop may block for 16 s */
/* Boot notes left by the init scripts for one message at boot (system layer). */
#define BOOT_FALLBACK_PATH  "/run/rsos/boot-fallback"
#define DATA_FSCK_PATH      "/run/rsos/data-fsck"
#define DATA_REFORMAT_PATH  "/run/rsos/data-reformatted"
#define DATA_ERROR_PATH     "/run/rsos/data-error"
#define DATA_RESTORE_PATH   "/run/rsos/data-restore-failed"

/* ------------------------------------------------------------------ state */
struct script_tok {
	char text[256];
};

static struct {
	/* options */
	bool headless;
	int hl_w, hl_h;
	const char *root;          /* test tree prefix ("" = the device) */
	const char *res_dir;       /* /usr/share/rsos */
	const char *themes_dir;    /* /usr/share/rsos/themes */
	const char *locale_dir;    /* /usr/share/rsos/locale (catalogs) */
	const char *lang;          /* --lang: wins over settings.ini (tests, splash) */
	const char *log_path;
	int run_ms;                /* headless: stop after this long */
	const char *splash_file;   /* <res>/splash.rle */
	const char *shot_file;     /* --splash --headless: write the frame here */
	int logo_min_ms;           /* settings boot_logo_min_ms: splash at least this long */
	int64_t splash_at;         /* when the boot splash was presented (0: none) */
	bool splash_tried;

	/* modules */
	struct display_config dcfg;
	bool display_ok;
	int64_t display_retry_at;
	int display_failures;
	bool display_reinit;       /* hdmi_mode changed while on HDMI */
	bool hdmi_mode_pending;    /* hdmi_mode changed while on the LCD */
	struct display_output_info out;
	bool output_dirty;
	int ui_w, ui_h;

	struct input *in;
	struct ui *ui;
	struct ui_power_api pa;
	struct ui_transfer_api ta;
	char settings_path[PATH_MAX];
	char battery_path[PATH_MAX];

	bool power_tried, power_ok;
	bool usb_tried, usb_started;
	int64_t usb_fast_until;
	bool charge;
	bool first_frame;
	bool boot_ok_done;         /* /run/rsos/menu-up written */
	bool boot_ok_spawned;      /* rsos-boot-ok started (never in charge mode) */
	bool boot_notes_shown;
	/* the hardware watchdog (the menu process only) */
	const char *wd_path;       /* NULL: none (headless without --watchdog) */
	int wd_fd;
	int64_t wd_last;
	bool resume_checked;       /* the boot resume offer was looked for */
	bool lists_logged;
	bool need_redraw;
	int draw_fails;            /* frames that could not be drawn, in a row */
	int64_t draw_retry_at;     /* back-off after a failed frame (now_ms) */
	int64_t update_us;         /* the last ui_update() (frame log) */
	int64_t t0_ms;
	int64_t t0_us;

	/* game */
	bool in_game;
	int64_t combo_since;
	int64_t term_sent_at;
	int64_t poweroff_sent_at;
	uint16_t game_buttons;      /* the pads during a game: activity for the idle power-off */
	int16_t axis_ref[INPUT_MAX_PORTS][2][2]; /* last stick position counted (power_axis_activity) */
	bool activity_pending;      /* a pad moved, not reported yet (rate limit) */
	int64_t activity_sent_at;
	int64_t busy_checked_at;   /* the idle power-off's busy check (once a second) */

	/* shutdown */
	bool shutdown_req;
	enum power_reason shutdown_why;
	int64_t shutdown_at;
	bool poweroff_done;
	bool display_left;         /* powering off after a game: the display was not taken back */

	/* signals */
	volatile sig_atomic_t quit;
	int sigfd;

	/* headless */
	uint32_t *hl_fb;
	int hl_fb_w, hl_fb_h;
	bool hl_active, hl_suspended;
	struct script_tok *script;
	int script_n, script_i;
	int64_t script_at;
	int pending_release;       /* button to release, -1 = none */
	int frames;
	bool expect_failed;        /* a script "expect:" did not match */

	/* batch 2: the running game's play time already added to gamedb */
	long long pt_reported;
} M = { .sigfd = -1, .pending_release = -1, .wd_fd = -1 };

/* ---------------------------------------------------------------- helpers */
static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void mlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void mlog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int64_t t = now_ms();

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[%5lld.%03lld] frontend: %s\n", (long long)(t / 1000), (long long)(t % 1000), buf);
}

/* A path on the device, or under --root in test mode (static lifetime:
 * interned, so a path asked for twice is allocated once and every string
 * stays reachable). */
static const char *P(const char *path)
{
	static struct pnode {
		struct pnode *next;
		char s[];
	} *interned;
	struct pnode *e;
	size_t n;

	if (!M.root || !*M.root)
		return path;
	n = strlen(M.root) + strlen(path) + 1;
	for (e = interned; e; e = e->next)
		if (!strncmp(e->s, M.root, strlen(M.root)) && !strcmp(e->s + strlen(M.root), path))
			return e->s;
	e = malloc(sizeof(*e) + n);
	if (!e)
		return path;
	snprintf(e->s, n, "%s%s", M.root, path);
	e->next = interned;
	interned = e;
	return e->s;
}

static int min_timeout(int a, int b)
{
	if (a < 0)
		return b;
	if (b < 0)
		return a;
	return a < b ? a : b;
}

/*
 * Hardware watchdog (system S7): the menu process opens /dev/watchdog and
 * pets it every WATCHDOG_PET_MS from the main loop (whose poll timeout is
 * capped for it), from launch_idle() while a game runs (every ~100 ms) and
 * in charge mode / with the screen off (the same loop). The long jobs run
 * elsewhere: USB copies and scans in threads, the game, its state saves and
 * the benchmark in the game process, the game lists in the loader thread,
 * network helpers double-forked. Never opened by --splash or --run. An
 * orderly exit writes 'V' (magic close) before closing; a crash or a hang
 * lets it reboot the unit. Headless tests: only with --watchdog PATH (a
 * plain file: the keepalive becomes a written 'k').
 */
static void wd_open(void)
{
	if (!M.wd_path || M.wd_fd >= 0)
		return;
	M.wd_fd = open(M.wd_path, O_WRONLY | O_CLOEXEC | (M.headless ? O_CREAT | O_APPEND : 0), 0644);
	if (M.wd_fd < 0) {
		mlog("watchdog: %s: %s (not used)", M.wd_path, strerror(errno));
		return;
	}
	M.wd_last = 0;
	mlog("watchdog: %s open, kept alive every %d ms", M.wd_path, WATCHDOG_PET_MS);
}

static void wd_pet(void)
{
	int64_t t;

	if (M.wd_fd < 0)
		return;
	t = now_ms();
	if (M.wd_last && t - M.wd_last < WATCHDOG_PET_MS)
		return;
	M.wd_last = t;
	if (ioctl(M.wd_fd, WDIOC_KEEPALIVE, 0) < 0 && (errno != ENOTTY || write(M.wd_fd, "k", 1) != 1))
		mlog("watchdog: keepalive: %s", strerror(errno));
}

/* 0 ms: pet now; the poll timeout never lets it starve */
static int wd_timeout(void)
{
	int64_t left;

	if (M.wd_fd < 0)
		return -1;
	left = M.wd_last + WATCHDOG_PET_MS - now_ms();
	return left > 0 ? (int)left : 0;
}

/* Orderly exit: the magic close ('V') disarms it. */
static void wd_close(void)
{
	if (M.wd_fd < 0)
		return;
	if (write(M.wd_fd, "V", 1) != 1)
		mlog("watchdog: magic close: %s", strerror(errno));
	close(M.wd_fd);
	M.wd_fd = -1;
	mlog("watchdog: closed");
}

static bool read_word(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "re");
	bool ok;

	if (!f)
		return false;
	ok = fgets(out, (int)n, f) != NULL;
	fclose(f);
	if (ok)
		out[strcspn(out, " \t\r\n")] = 0;
	return ok;
}

/*
 * fork + fork + exec: the grandchild is reparented to init, so no zombie
 * and no waitpid() on a pid that host_launch() or the UI could be waiting
 * for. Never blocks for more than the intermediate fork.
 */
static void spawn_detached(const char *const argv[], int niceness)
{
	pid_t p = fork();

	if (p < 0) {
		mlog("cannot start %s: %s", argv[0], strerror(errno));
		return;
	}
	if (p == 0) {
		pid_t q = fork();

		if (q == 0) {
			sigset_t none;

			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			signal(SIGPIPE, SIG_DFL);
			setsid();
			if (niceness && nice(niceness) < 0) {
				/* keep going at the normal priority */
			}
			execv(argv[0], (char *const *)argv);
			_exit(127);
		}
		_exit(q < 0 ? 1 : 0);
	}
	while (waitpid(p, NULL, 0) < 0 && errno == EINTR)
		;
}

/* ------------------------------------------------------------------- logs */
/*
 * stderr is copied to the log file by a small thread: every module logs
 * with fprintf(stderr) (or its own logger that ends there), so this keeps
 * the console output and adds the file without touching them.
 */
static struct {
	int console, file, rd;
	pthread_t th;
	bool running;
} L = { -1, -1, -1, 0, false };

static void *log_thread(void *arg)
{
	char buf[4096];
	ssize_t r;

	(void)arg;
	while ((r = read(L.rd, buf, sizeof(buf))) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (L.console >= 0 && write(L.console, buf, (size_t)r) < 0) {
			/* console gone: keep the file */
		}
		if (L.file >= 0 && write(L.file, buf, (size_t)r) < 0) {
			close(L.file);
			L.file = -1;
		}
	}
	return NULL;
}

static void log_setup(const char *path)
{
	int p[2];
	char dir[PATH_MAX];
	char *slash;

	if (!path || !*path)
		return;
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = 0;
		mkdir(dir, 0755);
	}
	L.file = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (L.file < 0) {
		mlog("log file %s: %s (stderr only)", path, strerror(errno));
		return;
	}
	if (pipe2(p, O_CLOEXEC) < 0) {
		close(L.file);
		L.file = -1;
		return;
	}
	fflush(stderr);
	L.console = fcntl(2, F_DUPFD_CLOEXEC, 3);
	L.rd = p[0];
	if (dup2(p[1], 2) < 0) {
		close(p[0]);
		close(p[1]);
		close(L.file);
		L.file = L.rd = -1;
		return;
	}
	close(p[1]);
	setvbuf(stderr, NULL, _IOLBF, 0);
	if (pthread_create(&L.th, NULL, log_thread, NULL) == 0) {
		L.running = true;
	} else if (L.console >= 0) {
		dup2(L.console, 2);   /* no thread: plain stderr again */
	}
}

static void log_finish(void)
{
	struct timespec ts;

	fflush(stderr);
	if (!L.running)
		return;
	/* Point fd 2 back at the console: the pipe then reaches EOF (unless a
	 * helper still holds it) and the thread drains what is left. */
	if (L.console >= 0)
		dup2(L.console, 2);
	else
		close(2);
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 300 * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_timedjoin_np(L.th, NULL, &ts);
}

/* ---------------------------------------------------------------- signals */
/*
 * rsos-frontend --splash: a cosmetic screen that must never hold up the boot
 * (data-partition stops it before it goes on). Once asked to quit, it has
 * SPLASH_EXIT_S seconds to close the display; then SIGALRM (default action)
 * ends it wherever it is, and the kernel releases the display.
 */
#define SPLASH_EXIT_S 2
static volatile sig_atomic_t g_splash_mode;

static void on_signal(int sig)
{
	uint64_t one = 1;
	int e = errno;

	if (g_splash_mode && !M.quit)
		alarm(SPLASH_EXIT_S);   /* async-signal-safe */
	M.quit = sig;
	if (M.sigfd >= 0 && write(M.sigfd, &one, sizeof(one)) < 0) {
		/* the flag is enough; poll() also returns EINTR */
	}
	errno = e;
}

static void signals_setup(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;   /* no SA_RESTART: poll() returns EINTR */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);   /* sockets (web share) report EPIPE instead */
	/* Default SIGCHLD (never SIG_IGN, which would auto-reap and break the
	 * waitpid() of host_launch() and of the UI's jobs). */
	signal(SIGCHLD, SIG_DFL);
	M.sigfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

/* ------------------------------------------------- screen (DRM or memory) */
static void on_output(const struct display_output_info *now, const struct display_output_info *before,
		      enum display_event_reason why, const struct display_switch_timing *t, void *user);
static void display_try_init(void);

static void hl_alloc(int w, int h)
{
	if (w == M.hl_fb_w && h == M.hl_fb_h && M.hl_fb)
		return;
	free(M.hl_fb);
	M.hl_fb = calloc((size_t)w * h, 4);
	M.hl_fb_w = M.hl_fb ? w : 0;
	M.hl_fb_h = M.hl_fb ? h : 0;
}

static void hl_output(bool hdmi, enum display_event_reason why)
{
	struct display_output_info before = M.out, o;
	struct display_switch_timing t = { 0 };

	memset(&o, 0, sizeof(o));
	o.type = hdmi ? DISPLAY_OUTPUT_HDMI : DISPLAY_OUTPUT_LCD;
	snprintf(o.name, sizeof(o.name), "%s", hdmi ? "HDMI-A-1(fake)" : "Unknown-1(fake)");
	o.width = hdmi ? 1280 : M.hl_w;
	o.height = hdmi ? 720 : M.hl_h;
	/* the device's panel: 78.571 Hz, or 60 Hz retimed (lcd_refresh) */
	o.refresh_mhz = hdmi || M.dcfg.lcd_refresh_hz == 60 ? 60000 : 78571;
	snprintf(o.mode_name, sizeof(o.mode_name), "%dx%d", o.width, o.height);
	t.t_probe = t.t_commit = now_ms();
	on_output(&o, &before, why, &t, NULL);
}

static int scr_init(void)
{
	int r;

	if (M.headless) {
		M.display_ok = true;
		M.hl_active = true;
		hl_output(false, DISPLAY_EVENT_INIT);
		return 0;
	}
	r = display_init(&M.dcfg);
	M.display_ok = r == 0;
	return r;
}

static void scr_shutdown(void)
{
	if (M.headless) {
		free(M.hl_fb);
		M.hl_fb = NULL;
		M.hl_fb_w = M.hl_fb_h = 0;
	} else if (M.display_ok) {
		display_shutdown();
	}
	M.display_ok = false;
}

static void scr_set_surface(int w, int h)
{
	if (M.headless) {
		hl_alloc(w, h);
		return;
	}
	if (!display_set_game_surface(w, h, DRM_FORMAT_XRGB8888, DISPLAY_SURFACE_CACHED)) {
		mlog("display: no %dx%d UI surface", w, h);
		return;
	}
	display_set_scaling(DISPLAY_SCALE_ASPECT, (double)w / h);
}

static int scr_suspend(void)
{
	if (M.headless) {
		M.hl_suspended = true;
		return 0;
	}
	return M.display_ok ? display_suspend() : -ENODEV;
}

static int scr_resume(void)
{
	if (M.headless) {
		M.hl_suspended = false;
		return 0;
	}
	return M.display_ok ? display_resume() : -ENODEV;
}

static void scr_set_active(bool on)
{
	int r;

	if (M.headless) {
		if (M.hl_active != on)
			mlog("screen %s (headless)", on ? "on" : "off");
		M.hl_active = on;
		return;
	}
	if (!M.display_ok)
		return;
	r = display_set_active(on);
	if (r < 0)
		mlog("display_set_active(%d): %s", on, strerror(-r));
	if (on && (r < 0 || !display_is_active())) {
		/* The screen must come back whatever happened while it was off:
		 * re-open the display (full probe + modeset). */
		mlog("screen on failed: re-opening the display");
		scr_shutdown();
		display_try_init();
		M.output_dirty = true;
	}
	M.need_redraw = true;
}

static bool scr_active(void)
{
	if (M.headless)
		return M.hl_active;
	return M.display_ok && display_is_active();
}

/*
 * Frame timing (cheap, always on): the first frames at boot, then any frame
 * over FRAME_LOG_SLOW_US (25 ms: a frame and a half at 60 Hz), with the time
 * of ui_update(), the wait for a free buffer (display_begin_frame: the
 * previous flip), rendering and presenting, and where the UI thread's time
 * went since the frame before (ui_cost_take(), util.h): image decoding,
 * backdrop compositing, cache files, theme parsing, text; "draw" is the
 * rest of the render (blits, fills). Rate-limited: one line per
 * FRAME_LOG_GAP_MS, the slow frames in between counted in the next line.
 * Cost when frames are fast: a few clock reads.
 */
#define FRAME_LOG_FIRST 4
#define FRAME_LOG_SLOW_US 25000
#define FRAME_LOG_GAP_MS 2000

static void frame_log(int64_t update_us, int64_t wait_us, int64_t render_us, int64_t present_us,
		      const struct ui_cost_sample *before, const struct ui_cost_sample *in)
{
	static int64_t last_ms;
	static int skipped;
	int64_t total = update_us + wait_us + render_us + present_us, in_work = 0, now = now_ms();
	char costs[320];
	int o = 0;

	if (M.frames >= FRAME_LOG_FIRST && total < FRAME_LOG_SLOW_US)
		return;
	if (M.frames >= FRAME_LOG_FIRST && last_ms && now - last_ms < FRAME_LOG_GAP_MS) {
		skipped++;
		return;
	}
	costs[0] = 0;
	for (int k = 0; k < UI_COST_N && o < (int)sizeof(costs); k++) {
		int n = before->n[k] + in->n[k];

		in_work += in->us[k];
		if (n)
			o += snprintf(costs + o, sizeof(costs) - (size_t)o, "%s %lld x%d, ", ui_cost_name(k),
				      (long long)(before->us[k] + in->us[k]), n);
	}
	mlog("frame %d: %lld us (update %lld, wait for a buffer %lld, render %lld [%sdraw %lld], "
	     "present %lld)%s", M.frames, (long long)total, (long long)update_us, (long long)wait_us,
	     (long long)render_us, costs, (long long)MAX(0, render_us - in_work), (long long)present_us,
	     M.frames >= FRAME_LOG_FIRST ? " slow" : "");
	if (skipped)
		mlog("frame: %d more slow frame(s) since the last line", skipped);
	last_ms = now;
	skipped = 0;
}

/* Draws one UI frame. Returns false if nothing could be drawn. */
static bool scr_draw(void)
{
	struct gfx_surface fs;
	int64_t t0 = now_us(), t1, t2;
	struct ui_cost_sample before, in;

	if (!M.ui || !M.display_ok || M.display_left)
		return false;
	/* the UI thread's work since the last frame (update, buttons) */
	ui_cost_take(&before);
	if (M.headless) {
		if (!M.hl_fb || M.hl_suspended)
			return false;
		gfx_surface_init(&fs, M.hl_fb, M.hl_fb_w, M.hl_fb_h, M.hl_fb_w);
		t1 = now_us();
		ui_render(M.ui, &fs);
		t2 = now_us();
	} else {
		const struct display_surface *s;
		int i = display_begin_frame(100);

		t1 = now_us();
		s = display_get_surface();
		if (i < 0 || !s || i >= s->count || !s->buffers[i].pixels) {
			/* Review F-M15: this used to spin at 0 ms, one log line per
			 * try. Back off (100 ms, doubling to 500 ms), log the first
			 * failures then one in 50, and re-create a missing surface
			 * (e.g. CMA was short when HDMI came). */
			int64_t now = now_ms();

			M.draw_fails++;
			if (M.draw_fails <= 3 || M.draw_fails % 50 == 0)
				mlog("frame: no buffer after %lld us (%s), %d in a row", (long long)(t1 - t0),
				     i < 0 ? strerror(-i) : "no surface", M.draw_fails);
			M.draw_retry_at = now + (M.draw_fails < 3 ? 100 : M.draw_fails < 6 ? 250 : 500);
			if (!s && M.draw_fails % 4 == 0 && M.ui_w > 0 && M.ui_h > 0) {
				mlog("frame: re-creating the %dx%d UI surface", M.ui_w, M.ui_h);
				scr_set_surface(M.ui_w, M.ui_h);
			}
			return false;
		}
		if (M.draw_fails) {
			mlog("frame: drawing again after %d failed tries", M.draw_fails);
			M.draw_fails = 0;
		}
		M.draw_retry_at = 0;
		gfx_surface_init(&fs, s->buffers[i].pixels, s->width, s->height, s->buffers[i].stride / 4);
		ui_render(M.ui, &fs);
		t2 = now_us();
		display_present();
	}
	ui_cost_take(&in);
	frame_log(M.update_us, t1 - t0, t2 - t1, now_us() - t2, &before, &in);
	M.frames++;
	if (!M.first_frame) {
		struct ui_timings t;

		M.first_frame = true;
		ui_get_timings(M.ui, &t);
		mlog("first frame %lld ms after start (ui_create %lld us)",
		     (long long)((now_us() - M.t0_us) / 1000), (long long)t.create_us);
	}
	return true;
}

/* Called by the display layer (and the headless fake) on every output
 * change, including the first one inside display_init(). */
static void on_output(const struct display_output_info *now, const struct display_output_info *before,
		      enum display_event_reason why, const struct display_switch_timing *t, void *user)
{
	static const char *const reasons[] = { "init", "hotplug", "reprobe", "fallback" };
	bool hdmi = now->type == DISPLAY_OUTPUT_HDMI;
	int w, h;

	(void)before;
	(void)user;
	M.out = *now;
	/* The UI is laid out for the picture's shape: 720x480i on a 4:3
	 * composite screen is a 640x480 UI, scaled to the mode by the plane. */
	ui_pick_logical_size(now->pixel_aspect > 0.0 ? (int)(now->width * now->pixel_aspect + 0.5) : now->width,
			     now->height, &w, &h);
	M.ui_w = w;
	M.ui_h = h;
	/* The surface must exist before the display commits the new output;
	 * everything touching the UI waits for the main loop (this callback
	 * can run inside a UI callback, e.g. display_resume() after a game). */
	scr_set_surface(w, h);
	M.output_dirty = true;
	if (M.power_ok)
		power_set_docked(hdmi);
	if (t && t->t_uevent && t->t_commit >= t->t_uevent)
		mlog("output %s: %s %dx%d@%d.%03d Hz, UI %dx%d, %lld ms after the uevent", reasons[why & 3],
		     now->name, now->width, now->height, now->refresh_mhz / 1000, now->refresh_mhz % 1000,
		     w, h, (long long)(t->t_commit - t->t_uevent));
	else
		mlog("output %s: %s %dx%d@%d.%03d Hz, UI %dx%d", reasons[why & 3], now->name, now->width,
		     now->height, now->refresh_mhz / 1000, now->refresh_mhz % 1000, w, h);
}

/* The UI side of an output change, from the main loop. */
static void apply_output(void)
{
	bool hdmi;

	if (!M.output_dirty)
		return;
	M.output_dirty = false;
	hdmi = M.out.type == DISPLAY_OUTPUT_HDMI;
	if (M.ui) {
		ui_set_size(M.ui, M.ui_w, M.ui_h);
		ui_set_output(M.ui, hdmi);    /* also input_set_docked(): player 1, brightness keys */
	} else if (M.in) {
		input_set_docked(M.in, hdmi);
	}
	if (M.power_ok)
		power_set_docked(hdmi);
	if (hdmi && M.hdmi_mode_pending) {
		M.hdmi_mode_pending = false;
		M.display_reinit = true;      /* the new hdmi_mode applies to this TV */
	}
	M.need_redraw = true;
}

/* ------------------------------------------------------------- boot logo */
static int write_png(const char *file, const uint32_t *pix, int w, int h, int stride)
{
	uint8_t *rgb = malloc((size_t)w * h * 3);
	void *png;
	size_t size = 0;
	FILE *f;
	int r = -1;

	if (!rgb)
		return -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t c = pix[(size_t)y * stride + x];
			uint8_t *d = rgb + 3 * ((size_t)y * w + x);

			d[0] = (uint8_t)(c >> 16);
			d[1] = (uint8_t)(c >> 8);
			d[2] = (uint8_t)c;
		}
	png = host_png_encode(rgb, w, h, &size);
	free(rgb);
	if (png && (f = fopen(file, "wb"))) {
		r = fwrite(png, 1, size, f) == size ? 0 : -1;
		if (fclose(f))
			r = -1;
	}
	free(png);
	mlog("shot %s (%dx%d)%s", file, w, h, r ? ": failed" : "");
	return r;
}

/* One line of text centred at baseline y: the UI's font renderer (DejaVu
 * Sans from <res>/fonts), else the built-in 8x8 font at 2x. */
static void draw_message(uint32_t *pix, int w, int h, int stride, uint32_t bg, const char *text)
{
	struct gfx_surface s;
	char path[PATH_MAX];
	struct font *f;
	int lum = (int)((bg >> 16 & 255) * 3 + (bg >> 8 & 255) * 6 + (bg & 255)) / 10;
	uint32_t fg = lum < 128 ? 0xffd8d8e0u : 0xff303038u;
	int y = h * 82 / 100;

	snprintf(path, sizeof(path), "%s/fonts/DejaVuSans.ttf", M.res_dir ? M.res_dir : "/usr/share/rsos");
	gfx_surface_init(&s, pix, w, h, stride);
	f = access(path, R_OK) == 0 ? font_get(path, h / 20 > 12 ? h / 20 : 12) : NULL;
	if (f) {
		/* a translation may need two or three lines */
		struct text_line lines[3];
		int n = font_wrap(f, text, w * 9 / 10, lines, 3);
		int lh = font_height(f) * 3 / 2;

		y -= (n - 1) * lh / 2;
		for (int i = 0; i < n; i++)
			font_draw(&s, f, (w - lines[i].width) / 2, y + i * lh, text + lines[i].start, lines[i].len, fg);
		return;
	}
	{
		int n = (int)strlen(text), sc = 2, x0 = (w - n * 8 * sc) / 2, y0 = y - 8 * sc;

		for (int i = 0; i < n; i++) {
			const uint8_t *g = font8x8_glyph((unsigned char)text[i]);

			for (int gy = 0; gy < 8 * sc; gy++)
				for (int gx = 0; gx < 8 * sc; gx++) {
					int px = x0 + i * 8 * sc + gx, py = y0 + gy;

					if (px >= 0 && px < w && py >= 0 && py < h && (g[gy / sc] & (0x80 >> (gx / sc))))
						pix[(size_t)py * stride + px] = fg & 0xffffffu;
				}
		}
	}
}

/* Draws the splash (and an optional message) and presents it. Returns the
 * time spent in us, or -1 if nothing could be drawn. */
static int64_t splash_present(const struct splash *sp, const char *message)
{
	int64_t t0 = now_us();
	uint32_t *pix;
	int w, h, stride, i = 0;
	const struct display_surface *s = NULL;

	if (!M.display_ok)
		return -1;
	if (M.headless) {
		if (!M.hl_fb)
			return -1;
		pix = M.hl_fb;
		w = stride = M.hl_fb_w;
		h = M.hl_fb_h;
	} else {
		i = display_begin_frame(100);
		s = display_get_surface();
		if (i < 0 || !s || i >= s->count || !s->buffers[i].pixels)
			return -1;
		pix = s->buffers[i].pixels;
		w = s->width;
		h = s->height;
		stride = s->buffers[i].stride / 4;
	}
	if (!sp || !sp->data || splash_draw(sp, pix, w, h, stride) < 0)
		for (int y = 0; y < h; y++)
			memset(pix + (size_t)y * stride, 0, (size_t)w * 4);
	if (message && *message)
		draw_message(pix, w, h, stride, sp && sp->data ? sp->bg : 0, message);
	if (!M.headless)
		display_present();
	return now_us() - t0;
}

static const char *splash_path(void)
{
	static char p[PATH_MAX];

	if (M.splash_file)
		return M.splash_file;
	snprintf(p, sizeof(p), "%s/splash.rle", M.res_dir ? M.res_dir : "/usr/share/rsos");
	return p;
}

/* Panel safety (display-design.md §8.5): the boot logo is what the panel
 * scans, backlight off, while HDMI shows the picture (black without it). */
static void panel_picture(void)
{
	struct splash sp;
	uint32_t *pix;

	if (M.headless || !M.display_ok || splash_load(splash_path(), &sp) < 0)
		return;
	pix = malloc((size_t)sp.width * sp.height * 4);
	if (pix && splash_draw(&sp, pix, sp.width, sp.height, sp.width) == 0)
		display_set_panel_picture(pix, sp.width, sp.height, sp.bg);
	free(pix);
	splash_free(&sp);
}

/* The boot logo: right after the first display_init(), before anything
 * else loads. Never in charge mode (the charge screen comes first). */
static void boot_splash(void)
{
	struct splash sp;
	int64_t t0 = now_us(), t;
	int r;

	M.splash_tried = true;
	if (M.charge)
		return;
	r = splash_load(splash_path(), &sp);
	if (r < 0) {
		mlog("splash: %s: %s", splash_path(), strerror(-r));
		return;
	}
	t = splash_present(&sp, NULL);
	if (t >= 0) {
		M.splash_at = now_ms();
		mlog("splash: %zu bytes, %dx%d, shown %lld ms after start (read %lld us, decode + present %lld us)",
		     sp.size, sp.width, sp.height, (long long)((now_us() - M.t0_us) / 1000),
		     (long long)(now_us() - t0 - t), (long long)t);
	}
	splash_free(&sp);
}

static void display_try_init(void)
{
	int r;

	if (M.display_ok)
		return;
	r = scr_init();
	if (r == 0) {
		if (M.display_failures)
			mlog("display up after %d attempts", M.display_failures + 1);
		M.display_failures = 0;
		M.need_redraw = true;
		if (!M.splash_tried && !M.first_frame)
			boot_splash();
		panel_picture();   /* after the first picture: no boot delay */
		return;
	}
	if (M.display_failures++ % 20 == 0)
		mlog("display_init: %s, retrying", strerror(-r));
	M.display_retry_at = now_ms() + DISPLAY_RETRY_MS;
}

/* ------------------------------------------------------------- settings */
static void hdmi_mode_to_cfg(const char *mode, struct display_config *c)
{
	if (mode && !strcmp(mode, "1080p")) {
		c->hdmi_width = 1920;
		c->hdmi_height = 1080;
	} else {                          /* auto, 720p: the display default */
		c->hdmi_width = 1280;
		c->hdmi_height = 720;
	}
}

static const char *const power_keys[] = {
	"sleep_timeout_min", "sleep_wake", "idle_dim_min", "idle_off_min", "idle_poweroff_min", "battery_gauge",
	"timezone",
};

/* settings.ini -> power module. The UI does it in ui_create(), which runs
 * before power_init() (and power_init() resets the module), so again here. */
static void apply_power_settings(void)
{
	struct settings *s = settings_open(M.settings_path);

	for (size_t i = 0; i < sizeof(power_keys) / sizeof(power_keys[0]); i++) {
		const char *v = settings_get(s, power_keys[i], NULL);

		if (v && *v && power_set_setting(power_keys[i], v) < 0)
			mlog("power setting %s=%s refused", power_keys[i], v);
	}
	settings_close(s);
}

static void setting_changed(const char *key, const char *value, void *user)
{
	(void)user;
	mlog("setting %s = %s", key, value ? value : "");
	if (!strcmp(key, "hdmi_mode")) {
		struct display_config c = M.dcfg;

		hdmi_mode_to_cfg(value, &c);
		if (c.hdmi_width == M.dcfg.hdmi_width && c.hdmi_height == M.dcfg.hdmi_height)
			return;
		M.dcfg.hdmi_width = c.hdmi_width;
		M.dcfg.hdmi_height = c.hdmi_height;
		/* The display reads its config at init: re-open it now on HDMI,
		 * or when a TV is plugged in next. */
		if (M.out.type == DISPLAY_OUTPUT_HDMI)
			M.display_reinit = true;
		else
			M.hdmi_mode_pending = true;
	} else if (!strcmp(key, "lcd_refresh")) {
		/* live (display-design.md §3.1); the game children get it as
		 * --lcd-refresh (the UI saves it only after the trial) */
		int hz = display_parse_lcd_refresh(value), r = 0;

		if (hz == M.dcfg.lcd_refresh_hz)
			return;
		M.dcfg.lcd_refresh_hz = hz;
		if (M.headless) {
			mlog("lcd refresh: %s (headless)", hz ? "60 Hz" : "78 Hz, the panel's mode");
			if (M.out.type == DISPLAY_OUTPUT_LCD)
				hl_output(false, DISPLAY_EVENT_REPROBE);
		} else if (M.display_ok) {
			r = display_set_lcd_refresh(hz);
		}
		if (r < 0) {
			mlog("lcd refresh %d Hz refused: %s", hz ? hz : 78, strerror(-r));
			M.dcfg.lcd_refresh_hz = display_get_lcd_refresh();
			if (M.ui)
				ui_toast(M.ui, _("The screen refused this refresh rate"), UI_SEV_WARNING);
		}
		M.need_redraw = true;
	}
	/* "scaling" is read at each launch (--scale for the game); "p1",
	 * "cz_buttons" and the power keys are applied by the UI itself;
	 * "theme" needs nothing here. */
}

/* ----------------------------------------------------------------- power */
/*
 * The game child shows the same smoothed percentage as the menu (its
 * in-game battery overlay reads this file every ~10 s; it never computes
 * its own). "<percent> <charging>\n", -1 = unknown; replaced atomically
 * (tmp + rename on tmpfs) and only when the value changes.
 */
static void publish_battery(const struct power_status *st)
{
	static int last_pct = -2, last_chg = -1;
	char tmp[PATH_MAX + 8], buf[32];
	int pct = st && st->valid && st->battery_present ? st->percent : -1;
	int chg = st && st->charger_online ? 1 : 0;
	int fd, n;

	if (!M.battery_path[0] || (pct == last_pct && chg == last_chg))
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", M.battery_path);
	n = snprintf(buf, sizeof(buf), "%d %d\n", pct, chg);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, buf, (size_t)n) != n) {
		close(fd);
		unlink(tmp);
		return;
	}
	if (close(fd) < 0 || rename(tmp, M.battery_path) < 0) {
		unlink(tmp);
		return;
	}
	last_pct = pct;
	last_chg = chg;
}

static void pw_status(const struct power_status *st, void *user)
{
	(void)user;
	publish_battery(st);
	if (M.ui)
		ui_power_event(M.ui, UI_PWR_STATUS);
}

static void pw_warning(enum power_level level, const struct power_status *st, void *user)
{
	(void)user;
	mlog("battery %s (%d %%)", level == POWER_LEVEL_LOW ? "low" : level == POWER_LEVEL_VERY_LOW ?
	     "very low" : "ok", st ? st->percent : -1);
	if (!M.ui)
		return;
	/* During a game the menu cannot draw: the child's OSD warns; only the
	 * persistent banner state is kept for when the menu comes back. */
	if (level == POWER_LEVEL_LOW && !M.in_game)
		ui_power_event(M.ui, UI_PWR_LOW);
	else if (level == POWER_LEVEL_VERY_LOW)
		ui_power_event(M.ui, UI_PWR_VERY_LOW);
	else if (level == POWER_LEVEL_OK)
		ui_power_event(M.ui, UI_PWR_OK);
}

static void pw_critical(const struct power_status *st, void *user)
{
	(void)user;
	mlog("battery critical (%d %%, %d mV)", st ? st->percent : -1, st ? st->voltage_filtered_mv : -1);
	if (M.ui)
		ui_power_event(M.ui, UI_PWR_CRITICAL);
	if (!M.in_game)
		scr_draw();
}

static void pw_sleep(bool enter, void *user)
{
	pid_t c = host_child_pid();

	(void)user;
	mlog("%s", enter ? "sleep" : "wake");
	if (M.in_game && c > 0) {
		/* the game pauses, closes ALSA and turns its own display off */
		kill(c, enter ? RSOS_SIG_SLEEP : RSOS_SIG_WAKE);
		return;
	}
	scr_set_active(!enter);
	if (!enter && M.ui) {
		ui_power_event(M.ui, UI_PWR_STATUS);
		M.need_redraw = true;
	}
}

/* A small file in /run (tmpfs: no fsync needed). */
static void write_run_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd < 0)
		return;
	if (write(fd, text, strlen(text)) < 0) {
		/* best effort: rcK falls back to its own timing */
	}
	close(fd);
}

/* For rcK's fast poweroff/reboot: when it was asked, and which. */
static void shutdown_markers(enum power_reason why)
{
	char up[32] = "0", line[48];

	if (!read_word("/proc/uptime", up, sizeof(up)))
		snprintf(up, sizeof(up), "0");
	snprintf(line, sizeof(line), "%s\n", up);
	write_run_file(P(SHUTDOWN_START_PATH), line);
	write_run_file(P(SHUTDOWN_MODE_PATH), why == POWER_REASON_REBOOT ? "reboot\n" : "poweroff\n");
}

/* A copy to/from a USB drive: stop it (the partial file is removed) and
 * wait for its thread (it stops after the current 1 MiB block). */
static void transfer_stop_jobs(void)
{
	struct transfer_progress pr;

	if (transfer_import_status(&pr) != TRANSFER_IDLE) {
		char changed[TRANSFER_SYS_MAX][TRANSFER_SYSID_MAX];

		mlog("stopping the USB import");
		transfer_import_cancel();
		transfer_import_finish(changed, TRANSFER_SYS_MAX);
	}
	if (transfer_backup_status(&pr) != TRANSFER_IDLE) {
		mlog("stopping the USB export/backup");
		transfer_backup_cancel();
		transfer_backup_finish();
	}
}

static void pw_shutdown(enum power_reason why, void *user)
{
	pid_t c = host_child_pid();

	(void)user;
	if (M.shutdown_req)
		return;
	shutdown_markers(why);
	mlog("shutdown requested: %s", power_reason_name(why));
	M.shutdown_req = true;
	M.shutdown_why = why;
	/* "Powering off..." at the next frame (right away); saves settings,
	 * gamedb, caches */
	if (M.ui)
		ui_power_event(M.ui, why == POWER_REASON_REBOOT ? UI_PWR_REBOOT : UI_PWR_SHUTDOWN);
	if (M.in_game && c > 0) {
		/* SRAM flush + .state.auto, exit 3; launch_idle() waits for it
		 * (then SIGKILL after CHILD_GRACE_MS) and the launch callback
		 * schedules the power-off once it has returned. */
		kill(c, RSOS_SIG_POWEROFF);
		M.poweroff_sent_at = now_ms();
		return;
	}
	M.shutdown_at = now_ms() + SHUTDOWN_MSG_MS;
	M.need_redraw = true;
}

static void pw_screen(enum power_screen s, void *user)
{
	(void)user;
	if (M.in_game)
		return;
	if (s == POWER_SCREEN_OFF) {
		scr_set_active(false);
	} else if (s == POWER_SCREEN_ON) {
		scr_set_active(true);
		if (M.ui)
			ui_power_event(M.ui, UI_PWR_STATUS);
		M.need_redraw = true;
	}
}

/* Idle power-off (docs/power.md "Idle power-off"): the 10 s notice, or its
 * cancellation. In a game the game process shows it (its OSD). The module
 * lit the screen again already (pw_screen). */
static void pw_idle(enum power_idle_event ev, int seconds, void *user)
{
	pid_t c = host_child_pid();

	(void)user;
	mlog("idle power-off: %s", ev == POWER_IDLE_WARN ? "notice (10 s)" : "cancelled");
	(void)seconds;
	if (M.in_game && c > 0) {
		kill(c, ev == POWER_IDLE_WARN ? RSOS_SIG_IDLE_WARN : RSOS_SIG_IDLE_CANCEL);
		return;
	}
	if (M.ui)
		ui_power_event(M.ui, ev == POWER_IDLE_WARN ? UI_PWR_IDLE_WARN : UI_PWR_IDLE_CANCEL);
	M.need_redraw = true;
}

/*
 * Idle power-off: the player on any pad (the power module sees the built-in
 * keys itself; the menu's own button events go through power_on_input()).
 * In a game this process reads the same pads as the game (no grab): a
 * button held or changed, or a real stick move (power_axis_activity(): past
 * half deflection or a 20 % move, never the noise of a drifting stick, so
 * N64/PS1 players steering with the stick keep the unit on). Reported at
 * most once a second (power_notify_activity()), never lost.
 */
static void pads_activity(bool in_game)
{
	bool moved = false;
	int64_t t;

	if (!M.power_ok || !M.in)
		return;
	if (in_game) {
		uint16_t b = input_any_buttons(M.in);

		moved = b || b != M.game_buttons;
		M.game_buttons = b;
	}
	for (int p = 0; p < INPUT_MAX_PORTS; p++)
		for (int s = 0; s < 2; s++)
			for (int a = 0; a < 2; a++)
				moved |= power_axis_activity(input_port_analog(M.in, p, s, a), &M.axis_ref[p][s][a]);
	if (moved)
		M.activity_pending = true;
	t = now_ms();
	if (M.activity_pending && t - M.activity_sent_at >= 1000) {
		M.activity_pending = false;
		M.activity_sent_at = t;
		power_notify_activity();
	}
}

/* An established TCP connection to local port `port` (the SMB share's
 * clients: ksmbd has no client count of its own). */
static bool tcp_client_on(const char *path, unsigned port)
{
	char line[256];
	FILE *f = fopen(path, "r");
	bool found = false;

	if (!f)
		return false;
	while (!found && fgets(line, sizeof(line), f)) {
		unsigned lport, st;
		char local[64];

		/* "  0: 0100007F:01BD 0100007F:D2A4 01 ..." (ipv4) or the ipv6 form */
		if (sscanf(line, " %*d: %63[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%*x %x", local, &lport, &st) == 3)
			found = lport == port && st == 0x01;   /* TCP_ESTABLISHED */
	}
	fclose(f);
	return found;
}

/*
 * The long jobs during which the unit never powers off by itself (checked
 * once a second): a USB import, export or backup, the web share or the SMB
 * share with a client, an OS update download/install, the game list still
 * loading. When the last one ends, the countdown starts over.
 */
static void idle_busy_check(bool force)
{
	struct transfer_progress pr;
	struct webshare_status ws;
	const char *why = NULL;
	int64_t t = now_ms();

	if (!M.power_ok || (!force && t - M.busy_checked_at < 1000))
		return;
	M.busy_checked_at = t;
	if (transfer_import_status(&pr) != TRANSFER_IDLE)
		why = "usb-import";
	else if (transfer_backup_status(&pr) != TRANSFER_IDLE)
		why = "usb-export-backup";
	else if (webshare_running() && (webshare_get_status(&ws), ws.clients > 0 || ws.current[0]))
		why = "web-share-client";
	else if (tcp_client_on(P("/proc/net/tcp"), 445) || tcp_client_on(P("/proc/net/tcp6"), 445))
		why = "smb-client";
	else if (M.ui && ui_update_busy(M.ui))
		why = "os-update";
	else if (M.ui && !M.in_game && !ui_is_loaded(M.ui))
		why = "loading";
	power_set_busy(why != NULL, why);
}

static void pw_thermal(int temp_mc, bool hot, void *user)
{
	(void)user;
	mlog("temperature %d.%d C: %s", temp_mc / 1000, (temp_mc % 1000) / 100, hot ? "hot" : "normal");
	if (M.ui)
		ui_power_event(M.ui, hot ? UI_PWR_HOT : UI_PWR_COOL);
}

static void pw_charge_exit(void *user)
{
	static const char *const net_apply[] = { NET_HELPER_PATH, "apply", NULL };

	(void)user;
	mlog("charge mode: booting normally");
	M.charge = false;
	if (M.ui)
		ui_set_charge_mode(M.ui, false);
	M.need_redraw = true;
	/* rcS/init skipped the network in charge mode (docs/power.md §11) */
	if (!M.headless && access(NET_HELPER_PATH, X_OK) == 0)
		spawn_detached(net_apply, 10);
}

static int hl_poweroff(bool reboot, void *user)
{
	(void)user;
	mlog("(headless) %s requested: exiting instead", reboot ? "reboot" : "power-off");
	M.quit = SIGTERM;   /* what init would send */
	return 0;
}

static void cpu_governor_menu(void)
{
	for (int i = 0; i < 4; i++) {
		char p[96];
		int fd;

		const char *gov = board_get()->cpu_governor_menu;   /* schedutil */

		snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", i);
		fd = open(p, O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (write(fd, gov, strlen(gov)) < 0)
			mlog("%s: %s", p, strerror(errno));
		close(fd);
	}
}

static void power_start(void)
{
	struct power_config pc;
	int r;

	M.power_tried = true;
	power_config_defaults(&pc);
	/* The board profile (the same values as the defaults on the RetroStone2). */
	board_apply_power(board_get(), &pc);
	pc.cb.on_status = pw_status;
	pc.cb.on_warning = pw_warning;
	pc.cb.on_critical = pw_critical;
	pc.cb.on_sleep_request = pw_sleep;
	pc.cb.on_shutdown_request = pw_shutdown;
	pc.cb.on_screen = pw_screen;
	pc.cb.on_thermal = pw_thermal;
	pc.cb.on_charge_exit = pw_charge_exit;
	pc.cb.on_idle_poweroff = pw_idle;
	if (M.root && *M.root) {
		pc.sysfs = P("/sys");
		pc.dev_input = P("/dev/input");
		pc.run_dir = P("/run/rsos");
		pc.clock_file = P("/data/rsos/lastclock");
		pc.rtc_dev = P("/dev/rtc0");
	}
	if (M.headless) {
		pc.no_uevent = true;
		pc.do_poweroff = hl_poweroff;
	}
	r = power_init(&pc);
	M.power_ok = r == 0;
	if (!M.power_ok) {
		mlog("power_init: %s (no battery, sleep or power-key handling)", strerror(-r));
		/* rcS boots at "performance"; the power module normally sets the
		 * menu governor: do it here without it */
		if (!M.headless)
			cpu_governor_menu();
	} else {
		apply_power_settings();
		power_set_docked(M.out.type == DISPLAY_OUTPUT_HDMI);
		mlog("power: mode %s, battery %d %%", power_mode_name(power_get_mode()),
		     power_get_status()->percent);
	}
	/* Charge mode needs the power module to leave it again. */
	if (M.charge && (!M.power_ok || power_get_mode() != POWER_MODE_CHARGE)) {
		mlog("charge mode: %s, starting the menu", M.power_ok ? "not confirmed" : "no power module");
		M.charge = false;
		if (M.ui)
			ui_set_charge_mode(M.ui, false);
		M.need_redraw = true;
	}
}

/* ---------------------------------------------------------------- USB */
/*
 * Headless tests: a "USB stick" is the folder <root>/sticks/<partition>;
 * mounting points the mount point at it (a symlink stands in for a bind
 * mount, no root needed). A remount (read-write for a backup) is logged.
 */
static int hl_mount(const char *src, const char *target, const char *fstype, unsigned long flags,
		    const char *data)
{
	char stick[PATH_MAX];
	const char *dev = src ? strrchr(src, '/') : NULL;

	(void)fstype;
	(void)data;
	if (flags & MS_REMOUNT) {
		mlog("(headless) remount %s %s", target, flags & MS_RDONLY ? "read-only" : "read-write");
		return 0;
	}
	snprintf(stick, sizeof(stick), "%s/sticks/%s", M.root, dev ? dev + 1 : "");
	if (!dev || access(stick, F_OK) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (rmdir(target) < 0 && errno != ENOENT)
		return -1;
	return symlink(stick, target);
}

static int hl_umount(const char *target, int flags)
{
	(void)flags;
	if (unlink(target) < 0)
		return -1;
	mkdir(target, 0755);      /* usb.c removes the mount point itself */
	return 0;
}

static void usb_start(void)
{
	struct transfer_usb_config uc;
	int r;

	memset(&uc, 0, sizeof(uc));             /* defaults: /media, /sys/block, /dev */
	if (M.root && *M.root) {
		uc.mount_base = P("/media");
		uc.sys_block = P("/sys/block");
		uc.dev_dir = P("/dev");
	}
	if (M.headless) {
		/* event driven like the device: an inotify watch on the fake
		 * /sys/block stands in for the kernel's uevents */
		uc.watch_sys_block = true;
		uc.mount_fn = hl_mount;
		uc.umount_fn = hl_umount;
	}
	r = transfer_usb_init(&uc);
	M.usb_tried = true;
	M.usb_started = r == 0;
	if (r < 0)
		mlog("transfer_usb_init: %s", strerror(-r));
}

static void usb_poll(void)
{
	struct transfer_usb_event ev;

	if (!M.usb_started)
		return;
	while (transfer_usb_poll(&ev) > 0) {
		static const char *const names[] = { "mounted", "removed", "ejected", "unsupported",
						     "mount failed" };

		mlog("USB %s: %s %s %s", names[ev.type % 5], ev.drive.dev, ev.drive.mountpoint,
		     ev.drive.fstype);
		if (M.ui)
			ui_usb_event(M.ui, &ev);
		M.usb_fast_until = now_ms() + USB_FAST_MS;
	}
}

/* ---------------------------------------------------------------- input */
static void input_dispatch(void)
{
	struct input_nav ev;
	enum input_hotkey hk;

	if (!M.in)
		return;
	input_poll(M.in);
	/* ui_button() calls power_on_input() itself (idle wake, sleep) */
	while (input_next_nav(M.in, &ev))
		if (M.ui)
			ui_button(M.ui, ev.btn, ev.type);
	while (input_next_hotkey(M.in, &hk))
		if (M.ui)
			ui_hotkey(M.ui, hk);
}

/* Reads and drops everything queued (while a game runs, and right after
 * it: those buttons belong to the game). */
static void input_drain(void)
{
	struct input_nav ev;
	enum input_hotkey hk;

	if (!M.in)
		return;
	input_poll(M.in);
	while (input_next_nav(M.in, &ev))
		;
	while (input_next_hotkey(M.in, &hk))
		;
}

/* ---------------------------------------------------------------- launch */
static void set_msg(const struct ui_launch *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_msg(const struct ui_launch *req, const char *fmt, ...)
{
	va_list ap;

	if (!req->message || !req->message_size)
		return;
	va_start(ap, fmt);
	vsnprintf(req->message, req->message_size, fmt, ap);
	va_end(ap);
}

static void script_in_game(void);

/* Every ~100 ms while the game runs (host_launch()). */
static bool launch_idle(void *user)
{
	int64_t t = now_ms();
	pid_t c = host_child_pid();

	(void)user;
	wd_pet();               /* the menu's loop is blocked in host_launch() */
	if (M.headless)
		script_in_game();   /* test tokens "game:..." (a power-off during the game) */
	if (M.power_ok) {
		idle_busy_check(power_timeout_ms() <= 0);
		power_poll();   /* battery, power key, sleep: forwarded by the callbacks */
	}
	if (M.quit && c > 0) {
		if (!M.term_sent_at) {
			mlog("signal %d: asking the game to quit", (int)M.quit);
			kill(c, SIGTERM);
			M.term_sent_at = t;
		} else if (t - M.term_sent_at > CHILD_GRACE_MS) {
			return true;
		}
	}
	if (M.poweroff_sent_at && t - M.poweroff_sent_at > CHILD_GRACE_MS) {
		mlog("the game did not exit after the power-off signal");
		return true;
	}
	/* Hang kill switch: the game quits on Select+Start at once; if it is
	 * still there after HANG_KILL_MS of holding them, it is stuck. */
	if (M.in) {
		const uint16_t combo = (1u << IN_SELECT) | (1u << IN_START);
		bool awake = !M.power_ok || power_get_mode() == POWER_MODE_NORMAL;

		input_drain();
		if (awake)
			pads_activity(true);
		if (awake && (input_any_buttons(M.in) & combo) == combo) {
			if (!M.combo_since) {
				M.combo_since = t;
			} else if (t - M.combo_since >= HANG_KILL_MS) {
				mlog("Select+Start held %d s: killing the game", HANG_KILL_MS / 1000);
				M.combo_since = 0;
				return true;
			}
		} else {
			M.combo_since = 0;
		}
	}
	return false;
}

static void restore_display_after_game(bool suspended)
{
	int r;

	if (M.headless) {
		scr_resume();
		return;
	}
	if (suspended && (r = scr_resume()) == 0) {
		/* same output: surface and state kept; a hotplug during the game
		 * went through on_output() */
		if (!display_get_surface())
			scr_set_surface(M.ui_w, M.ui_h);
	} else {
		if (suspended)
			mlog("display_resume failed, re-opening the display");
		scr_shutdown();
		display_try_init();
	}
	M.output_dirty = true;
	M.need_redraw = true;
}

/* ui cb.has_resume: is there a .state.auto (written at power-off / exit) for
 * this game? Same core resolution as launch_game(). */
static bool has_resume(const struct ui_launch *req, void *user)
{
	char core[PATH_MAX], st[PATH_MAX];
	const char *core_path = req->core_path;

	(void)user;
	if (!core_path || !*core_path || access(core_path, R_OK) != 0) {
		if (host_pick_core(req->system, req->rom_path, core, sizeof(core)) < 0)
			return false;
		core_path = core;
	}
	return host_auto_state_path(P("/data/states"), core_path, req->rom_path, req->system, st, sizeof(st));
}

/* ------------------------------------------------------------ resume at boot */
/*
 * A power-off (power key, critical battery, thermal) while a game runs: the
 * game process writes SRAM and .state.auto and says so ("autostate"); then
 * the session is recorded in /data/rsos/resume.ini (tmp + fsync + rename +
 * fsync of the folder, like every user file). At the next boot, once the
 * menu is up, "Resume <game>?" is offered if the ROM, the core and the auto
 * state are still there (else the file is dropped, logged). The file is
 * deleted as soon as the player answers. A power cut during the game writes
 * nothing (no state was saved, the last file was deleted at the boot before).
 */
static void core_id_of(const char *core_path, char *id, size_t n)
{
	const char *b = strrchr(core_path, '/');
	size_t len;
	char *s;

	if (!n)
		return;
	b = b ? b + 1 : core_path;
	len = strlen(b) < n - 1 ? strlen(b) : n - 1;
	memcpy(id, b, len);
	id[len] = 0;
	if ((s = strstr(id, "_libretro")) || (s = strstr(id, ".so")))
		*s = 0;
}

/* Durable small file on /data (exFAT: no journal). */
static int write_durable(const char *path, const char *text)
{
	char tmp[PATH_MAX + 8], dir[PATH_MAX];
	char *slash;
	size_t len = strlen(text);
	int fd, r = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	if (write(fd, text, len) != (ssize_t)len)
		r = errno ? -errno : -EIO;
	if (!r && fsync(fd) < 0)
		r = -errno;
	if (close(fd) < 0 && !r)
		r = -errno;
	if (!r && rename(tmp, path) < 0)
		r = -errno;
	if (r) {
		unlink(tmp);
		return r;
	}
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = 0;
		if ((fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0) {
			fsync(fd);
			close(fd);
		}
	}
	return 0;
}

/* One line value: no newline can break the file. */
static void ini_line(char *buf, size_t n, size_t *pos, const char *key, const char *value)
{
	int w;

	if (*pos >= n)
		return;
	w = snprintf(buf + *pos, n - *pos, "%s = ", key);
	if (w < 0)
		return;
	*pos += (size_t)w;
	for (const char *v = value ? value : ""; *v && *pos + 2 < n; v++)
		buf[(*pos)++] = *v == '\n' || *v == '\r' ? ' ' : *v;
	if (*pos + 1 < n)
		buf[(*pos)++] = '\n';
	buf[*pos < n ? *pos : n - 1] = 0;
}

static void resume_record(const struct ui_launch *req, const char *core_path, const char *game,
			  const char *p1dev, const struct host_launch_result *res)
{
	char buf[4096], st[PATH_MAX], opt[PATH_MAX], id[64], num[32];
	const char *options = "default";
	size_t pos = 0;
	int r;

	host_auto_state_path(P("/data/states"), core_path, req->rom_path, req->system, st, sizeof(st));
	/* where the core options came from (the host reads the same layers
	 * again at resume: informational) */
	core_id_of(core_path, id, sizeof(id));
	snprintf(opt, sizeof(opt), "%s/%s/%s.ini", P("/data/rsos/coreopts"), id, game);
	if (access(opt, R_OK) == 0) {
		options = "game";
	} else {
		snprintf(opt, sizeof(opt), "%s/%s.ini", P("/data/rsos/coreopts"), id);
		if (access(opt, R_OK) == 0)
			options = "system";
	}
	buf[0] = 0;
	pos = (size_t)snprintf(buf, sizeof(buf),
			       "; RetroStoneOS: the game that was running at the last power-off.\n"
			       "; The menu offers to resume it at the next boot, then deletes this file.\n");
	ini_line(buf, sizeof(buf), &pos, "name", req->game_name);
	ini_line(buf, sizeof(buf), &pos, "system", req->system);
	ini_line(buf, sizeof(buf), &pos, "rom", req->rom_path);
	ini_line(buf, sizeof(buf), &pos, "core", req->core && *req->core ? req->core : id);
	ini_line(buf, sizeof(buf), &pos, "core_path", core_path);
	ini_line(buf, sizeof(buf), &pos, "core_source", req->core_source ? req->core_source : "");
	ini_line(buf, sizeof(buf), &pos, "options", options);
	ini_line(buf, sizeof(buf), &pos, "p1_device", p1dev);
	ini_line(buf, sizeof(buf), &pos, "reason", power_reason_name(M.shutdown_why));
	ini_line(buf, sizeof(buf), &pos, "state", st);
	snprintf(num, sizeof(num), "%lld", res->auto_state_bytes);
	ini_line(buf, sizeof(buf), &pos, "state_bytes", num);
	snprintf(num, sizeof(num), "%lld", (long long)time(NULL));
	ini_line(buf, sizeof(buf), &pos, "time", num);
	r = write_durable(P(RESUME_PATH), buf);
	if (r < 0)
		mlog("resume: cannot write %s: %s", P(RESUME_PATH), strerror(-r));
	else
		mlog("resume: recorded %s (%s, core %s, options %s) for the next boot", req->game_name,
		     req->rom_path, req->core, options);
}

/* Deletes resume.ini durably (review F-L20: without the folder fsync a
 * power cut could bring the offer back once). */
static void resume_forget(void)
{
	int fd;

	if (unlink(P(RESUME_PATH)) < 0) {
		if (errno != ENOENT)
			mlog("resume: cannot delete %s: %s", P(RESUME_PATH), strerror(errno));
		return;
	}
	fd = open(P("/data/rsos"), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static void resume_answered(bool resume, void *user)
{
	(void)user;
	/* handled: never offered again, whatever happens in the game */
	resume_forget();
	mlog("resume: %s", resume ? "resuming at the player's request" : "start fresh (auto state kept)");
}

/* Once the menu is up (never in charge mode): the resume offer, if any. */
static void resume_check(void)
{
	struct settings *s;
	const char *why = NULL;
	char name[256], sys[64], rom[PATH_MAX], core[64], core_path[PATH_MAX], st[PATH_MAX];

	M.resume_checked = true;
	if (access(P(RESUME_PATH), F_OK) != 0)
		return;
	s = settings_open(P(RESUME_PATH));
	snprintf(name, sizeof(name), "%s", settings_get(s, "name", ""));
	snprintf(sys, sizeof(sys), "%s", settings_get(s, "system", ""));
	snprintf(rom, sizeof(rom), "%s", settings_get(s, "rom", ""));
	snprintf(core, sizeof(core), "%s", settings_get(s, "core", ""));
	snprintf(core_path, sizeof(core_path), "%s", settings_get(s, "core_path", ""));
	settings_close(s);
	if (!rom[0] || !core_path[0])
		why = "incomplete file";
	else if (access(rom, R_OK) != 0)
		why = "the game file is gone";
	else if (access(core_path, R_OK) != 0)
		why = "the emulator core is gone";
	else if (!host_auto_state_path(P("/data/states"), core_path, rom, sys, st, sizeof(st)))
		why = "no auto state";
	if (why) {
		mlog("resume: %s (%s): %s, offer dropped", name[0] ? name : "?", rom, why);
		resume_forget();
		return;
	}
	{
		struct ui_resume_offer o = {
			.game_name = name, .rom_path = rom, .system = sys, .core = core,
			.core_path = core_path, .answered = resume_answered,
		};

		mlog("resume: offering %s (%s, %s)", name, rom, st);
		ui_offer_resume(M.ui, &o);
		M.need_redraw = true;
	}
}

/* ---------------------------------------------------------- batch 2 hooks */
#define SWITCHER_PATH "/run/rsos/switcher.tsv"   /* the game switcher's list (tmpfs) */

/*
 * The game switcher's file (host_switcher_write): the game about to run
 * first, then the most recently played others, each with its auto-state
 * picture (<state>.png). The game process shows them on Select+Y; a "switch
 * <n>" answer names the entry to launch next.
 */
static void switcher_write(const struct ui_launch *req, const char *core_path)
{
	struct host_switch_entry e[HOST_SWITCHER_MAX];
	struct ui_recent_game rg[HOST_SWITCHER_MAX + 1];
	char st[PATH_MAX];
	int n = 0, nr = M.ui ? ui_recent_games(M.ui, rg, HOST_SWITCHER_MAX + 1) : 0, r;

	memset(e, 0, sizeof(e));
	strlcpy_(e[0].name, req->game_name ? req->game_name : "", sizeof(e[0].name));
	strlcpy_(e[0].system, req->system ? req->system : "", sizeof(e[0].system));
	strlcpy_(e[0].rom, req->rom_path, sizeof(e[0].rom));
	strlcpy_(e[0].core, req->core ? req->core : "", sizeof(e[0].core));
	strlcpy_(e[0].core_path, core_path, sizeof(e[0].core_path));
	e[0].current = true;
	n = 1;
	for (int i = 0; i < nr && n < HOST_SWITCHER_MAX; i++) {
		if (!strcmp(rg[i].rom, req->rom_path))
			continue;
		strlcpy_(e[n].name, rg[i].name, sizeof(e[n].name));
		strlcpy_(e[n].system, rg[i].system, sizeof(e[n].system));
		strlcpy_(e[n].rom, rg[i].rom, sizeof(e[n].rom));
		strlcpy_(e[n].core, rg[i].core, sizeof(e[n].core));
		strlcpy_(e[n].core_path, rg[i].core_path, sizeof(e[n].core_path));
		if (rg[i].core_path[0] &&
		    host_auto_state_path(P("/data/states"), rg[i].core_path, rg[i].rom, rg[i].system, st, sizeof(st)) &&
		    strlen(st) + 5 <= sizeof(e[n].thumb)) {
			strlcpy_(e[n].thumb, st, sizeof(e[n].thumb));
			strcat(e[n].thumb, ".png");
		}
		n++;
	}
	r = host_switcher_write(P(SWITCHER_PATH), e, n);
	if (r < 0)
		mlog("switcher: cannot write %s: %s", P(SWITCHER_PATH), strerror(-r));
}

/* Status lines of the running game (host_launch_opts.on_status): its play
 * time so far (saved in gamedb every few minutes, so a crash loses little)
 * and the per-game settings changed in its menu. */
static void launch_status(const char *kind, const char *arg, void *user)
{
	const struct ui_launch *req = user;

	if (!M.ui || !req)
		return;
	if (!strcmp(kind, "playtime")) {
		long long s = atoll(arg);

		if (s < M.pt_reported)
			M.pt_reported = 0;   /* another process of the same session (benchmark) */
		if (s > M.pt_reported) {
			ui_game_add_playtime(M.ui, req->system, req->rom_path, s - M.pt_reported);
			M.pt_reported = s;
		}
	} else if (!strcmp(kind, "setting")) {
		char key[16];
		const char *v = strchr(arg, ' ');

		if (!v || v - arg >= (long)sizeof(key))
			return;
		snprintf(key, sizeof(key), "%.*s", (int)(v - arg), arg);
		mlog("game setting %s = %s (saved for %s)", key, v + 1, req->game_name);
		ui_game_set_option(M.ui, req->system, req->rom_path, key, v + 1);
	}
}

/* ui cb.game_saves (Delete this game): the .srm/.rtc and .state* files of
 * the game, by the name the game process gives them (host_game_name: a
 * zip's inner file). remove = true deletes them. */
static int game_saves(const struct ui_launch *req, bool remove, void *user)
{
	char core[PATH_MAX], game[256];
	const char *core_path = req->core_path;
	static const char *const roots[2] = { "/data/saves", "/data/states" };
	int n = 0;

	(void)user;
	if (!core_path || !*core_path) {
		if (host_pick_core(req->system, req->rom_path, core, sizeof(core)) < 0)
			core[0] = 0;
		core_path = core;
	}
	host_game_name(core_path, req->rom_path, game, sizeof(game));
	for (int k = 0; k < 2 && game[0]; k++) {
		char dir[PATH_MAX];
		DIR *d;
		struct dirent *de;
		size_t l = strlen(game);

		snprintf(dir, sizeof(dir), "%s/%s", P(roots[k]), req->system);
		d = opendir(dir);
		while (d && (de = readdir(d))) {
			char p[PATH_MAX + 260];

			/* <game>.srm, <game>.rtc, <game>.state, .state1-9, .state.auto,
			 * their .png thumbnails and .bak copies */
			if (strncmp(de->d_name, game, l) || de->d_name[l] != '.' ||
			    (k == 0 ? strncmp(de->d_name + l, ".srm", 4) && strncmp(de->d_name + l, ".rtc", 4) :
			     strncmp(de->d_name + l, ".state", 6)))
				continue;
			n++;
			snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
			if (remove && unlink(p) < 0)
				mlog("delete %s: %s", p, strerror(errno));
			else if (remove)
				mlog("deleted %s", p);
		}
		if (d)
			closedir(d);
	}
	return n;
}

static int launch_game(const struct ui_launch *req, void *user)
{
	char core[PATH_MAX], msg[256] = "", game[256], p1dev[320] = "";
	const char *core_path = req->core_path;
	const char *args[72];
	char scale[16], hdmi[24], fonts[PATH_MAX], extra[512];
	struct host_launch_opts o;
	struct host_launch_result res;
	struct settings *s;
	bool suspended, idle_cancelled = false;
	int na = 0, r, ret = 0;

	(void)user;
	/* The UI picked the core (game > system > default); host_pick_core()
	 * only when it has no library path. */
	if (!core_path || !*core_path || access(core_path, R_OK) != 0) {
		if (host_pick_core(req->system, req->rom_path, core, sizeof(core)) < 0 ||
		    access(core, R_OK) != 0) {
			set_msg(req, "%s", _("No emulator is installed for this game."));
			mlog("launch %s: no core (%s)", req->rom_path, core_path ? core_path : "");
			return -1;
		}
		core_path = core;
	}
	if (!host_check_game(core_path, req->rom_path, req->system, msg, sizeof(msg))) {
		set_msg(req, "%s", msg[0] ? msg : _("This game cannot start (missing BIOS?)."));
		mlog("launch %s: %s", req->rom_path, msg);
		return -1;
	}

	/* Command-line options for the child: the menu's settings win over
	 * the host's own settings.ini keys. */
	s = settings_open(M.settings_path);
	/* the game's own scaling (game options, in-game menu) wins */
	snprintf(scale, sizeof(scale), "%s", req->scale && *req->scale ? req->scale : settings_get(s, "scaling", ""));
	snprintf(hdmi, sizeof(hdmi), "%dx%d", M.dcfg.hdmi_width, M.dcfg.hdmi_height);
	settings_close(s);
	if (!strcmp(scale, "aspect") || !strcmp(scale, "integer") || !strcmp(scale, "stretch")) {
		args[na++] = "--scale";
		args[na++] = scale;
	}
	args[na++] = "--hdmi";
	args[na++] = hdmi;
	/* the LCD refresh in use now (also during the 15 s trial) */
	args[na++] = "--lcd-refresh";
	args[na++] = M.dcfg.lcd_refresh_hz == 60 ? "60" : "0";
	/* the menu's language (the game reads settings.ini too, but the menu's
	 * is the one in use, also when forced with --lang) */
	args[na++] = "--lang";
	args[na++] = i18n_language();
	if (M.locale_dir) {
		args[na++] = "--locale";
		args[na++] = M.locale_dir;
	}
	if (M.res_dir) {
		snprintf(fonts, sizeof(fonts), "%s/fonts", M.res_dir);
		args[na++] = "--fonts";
		args[na++] = fonts;
	}
	/* the controller that launched the game is player 1 (auto policy) */
	if (M.in) {
		const char *src = input_last_source_id(M.in);

		snprintf(p1dev, sizeof(p1dev), "%s", src ? src : "");
		if (p1dev[0]) {
			args[na++] = "--p1-device";
			args[na++] = p1dev;
			mlog("launch: player 1 = %s", p1dev);
		}
	}
	/* batch 2: the game switcher's list, the game's CPU profile (shown in
	 * its menu; the UI applied it), the screenshots folder */
	switcher_write(req, core_path);
	args[na++] = "--switcher";
	args[na++] = P(SWITCHER_PATH);
	args[na++] = "--cpu-profile";
	args[na++] = req->cpu && *req->cpu ? req->cpu : "auto";
	args[na++] = "--screenshots";
	args[na++] = P("/data/screenshots");
	if (M.headless) {
		/* test mode: no DRM/ALSA/input in the child, test tree paths */
		args[na++] = "--headless";
		args[na++] = "--frames";
		args[na++] = "120";
		args[na++] = "--cores-info";
		args[na++] = P("/usr/share/rsos/cores");
		args[na++] = "--bios";
		args[na++] = P("/data/bios");
		args[na++] = "--saves";
		args[na++] = P("/data/saves");
		args[na++] = "--states";
		args[na++] = P("/data/states");
		args[na++] = "--coreopts";
		args[na++] = P("/data/rsos/coreopts");
		args[na++] = "--tmp";
		args[na++] = P("/tmp/rsos");
		args[na++] = "--settings";
		args[na++] = M.settings_path;
		/* tests: more game arguments (--switcher-pick, --menu-script...),
		 * for every game or (_ONCE) the first one only */
		{
			const char *ta = getenv("RSOS_TEST_GAME_ARGS_ONCE") ? getenv("RSOS_TEST_GAME_ARGS_ONCE") :
					 getenv("RSOS_TEST_GAME_ARGS");
			char *save = NULL, *tok;

			snprintf(extra, sizeof(extra), "%s", ta ? ta : "");
			unsetenv("RSOS_TEST_GAME_ARGS_ONCE");
			for (tok = strtok_r(extra, " ", &save); tok && na < 70; tok = strtok_r(NULL, " ", &save))
				args[na++] = tok;
		}
	}
	args[na] = NULL;

	memset(&o, 0, sizeof(o));
	o.resume = req->resume;
	o.extra_args = args;
	o.idle = launch_idle;
	o.on_status = launch_status;
	o.user = (void *)req;
	M.pt_reported = 0;
	/* the game's own log (core messages, pacing, overlay) next to
	 * frontend.log; bootlog copies both to the SD card */
	o.log_path = P(GAME_LOG_PATH);
	o.log_append = true;

	/* The parent's input stays open without a grab (the hang kill switch
	 * reads it); game mode + the remap mirror what the child does, so the
	 * parent's view of the pad matches. "Docked" keeps its hands off the
	 * brightness keys: the child's input layer handles them during the
	 * game (both would step the backlight otherwise). */
	host_game_name(core_path, req->rom_path, game, sizeof(game));
	if (M.in) {
		input_set_mode(M.in, INPUT_MODE_GAME);
		input_load_remap(M.in, req->system, game);
		input_set_docked(M.in, true);
	}
	if (M.power_ok)
		power_set_game_running(true);   /* the UI did it already: no-op then */
	suspended = scr_suspend() == 0;
	if (!suspended) {
		/* fallback: close the display so the child can become master */
		mlog("display_suspend failed: closing the display for the game");
		scr_shutdown();
	}
	M.in_game = true;
	M.combo_since = M.term_sent_at = M.poweroff_sent_at = 0;
	mlog("launch %s (%s) with %s%s", req->game_name, req->rom_path, core_path,
	     req->resume ? (req->boot_resume ? ", resuming (boot offer)" : ", resuming") : "");

	r = host_launch(core_path, req->rom_path, req->system, &o, &res);

	M.in_game = false;
	M.game_buttons = 0;
	/*
	 * The idle power-off never cuts the power without saving: it goes on
	 * only if the game wrote its resume state (.state.auto, then
	 * resume.ini below). Otherwise (no save-state support, a write error,
	 * a game that did not exit in time) it is cancelled: the menu comes
	 * back and says so.
	 */
	if (M.shutdown_req && M.shutdown_why == POWER_REASON_IDLE &&
	    (r < 0 || res.status != HOST_EXIT_POWEROFF || !res.auto_state_saved)) {
		mlog("idle power-off cancelled: the game did not write its resume state (status %d, state %s)",
		     r < 0 ? -1 : res.status, r >= 0 && res.auto_state_saved ? "saved" : "not saved");
		M.shutdown_req = false;
		M.shutdown_at = 0;
		if (M.power_ok)
			power_cancel_shutdown();
		if (M.ui)
			ui_power_event(M.ui, UI_PWR_IDLE_CANCEL);   /* "Powering off..." down */
		idle_cancelled = true;
	}
	if (r >= 0 && M.power_ok && !idle_cancelled && (M.shutdown_req || res.status == HOST_EXIT_POWEROFF)) {
		/* powering off: the child showed "Powering off..." and its exit
		 * turned the screen off; no modeset and panel power-up just to
		 * exit (the resume.ini write and the flush below still happen) */
		M.display_left = true;
		mlog("game ended for a power-off: the display is not taken back");
	} else {
		restore_display_after_game(suspended);
	}
	if (M.in) {
		input_set_docked(M.in, M.out.type == DISPLAY_OUTPUT_HDMI);
		input_drain();   /* the game's buttons, and the port change above */
		input_clear_remap(M.in);
		input_set_mode(M.in, INPUT_MODE_UI);
	}
	if (M.power_ok)
		power_set_game_running(false);

	if (r < 0) {
		/* TRANSLATORS: %s is a system error message (in English) */
		set_msg(req, _("Cannot start the game process (%s)."), strerror(-r));
		mlog("host_launch: %s", strerror(-r));
		ret = -1;
	} else {
		mlog("game ended: status %d, exit %d%s%s%s%s", res.status, res.exit_code,
		     res.crashed ? ", crashed" : "", res.message[0] ? ", " : "", res.message,
		     res.warning[0] ? " (warning logged)" : "");
		if (res.warning[0])
			mlog("game warning: %s", res.warning);
		if (res.auto_state_saved)
			mlog("auto state written at exit: %lld KB in %d ms", (res.auto_state_bytes + 1023) / 1024,
			     res.auto_state_ms);
		if (idle_cancelled) {
			/* TRANSLATORS: the automatic power-off (no input for a while)
			 * was stopped because the game's state could not be saved */
			set_msg(req, "%s", _("Automatic power-off cancelled: the game could not be saved."));
		} else if (res.status == HOST_EXIT_POWEROFF) {
			/* the game to offer at the next boot, only if its state is
			 * really on the card */
			if (res.auto_state_saved)
				resume_record(req, core_path, game, p1dev, &res);
			else
				mlog("resume: no auto state was written: nothing to offer at the next boot");
			if (!M.shutdown_req && M.power_ok)
				power_request_shutdown(POWER_REASON_USER);   /* -> pw_shutdown() */
		} else if (res.status == HOST_EXIT_ERROR) {
			set_msg(req, "%s", res.message[0] ? res.message : _("The game could not be started."));
			ret = -1;
		} else if (res.status == HOST_EXIT_SWITCH && !M.quit && !M.shutdown_req) {
			/* the game switcher: the entry it names starts next, resumed
			 * (the UI launches it once this callback has returned) */
			struct host_switch_entry e[HOST_SWITCHER_MAX];
			int n = host_switcher_read(P(SWITCHER_PATH), e, HOST_SWITCHER_MAX);

			if (res.switch_to > 0 && res.switch_to < n) {
				mlog("switcher: %s -> %s (%s)", req->game_name, e[res.switch_to].name, e[res.switch_to].rom);
				ui_switch_to(M.ui, e[res.switch_to].system, e[res.switch_to].rom, e[res.switch_to].core);
			} else {
				mlog("switcher: entry %d of %d: nothing to launch", res.switch_to, n);
			}
		} else if (res.message[0] && !M.quit && !M.shutdown_req) {
			set_msg(req, "%s", res.message);   /* crash / hang / killed: it did start */
		}
	}
	if (M.shutdown_req && !M.shutdown_at)
		M.shutdown_at = now_ms() + SHUTDOWN_MSG_MS;
	M.poweroff_sent_at = 0;
	M.need_redraw = true;
	return ret;
}

/* ------------------------------------------------------------ boot steps */
/*
 * rsos-boot-ok (system S5: it confirms the boot slot; it waits for the
 * network apply + 30 s and checks our pid itself). Only once the normal
 * menu runs: never in charge mode, and then after the charge-mode exit.
 */
static void boot_ok_spawn(void)
{
	static const char *const argv[] = { BOOT_OK_PATH, NULL };

	M.boot_ok_spawned = true;
	if (M.headless) {
		mlog("menu up %lld ms after start%s: would run %s (headless)", (long long)(now_ms() - M.t0_ms),
		     ui_lists_complete(M.ui) ? "" : " (game lists still loading)", BOOT_OK_PATH);
		return;
	}
	if (access(BOOT_OK_PATH, X_OK) != 0) {
		mlog("menu up: %s missing", BOOT_OK_PATH);
		return;
	}
	mlog("menu up %lld ms after start%s: running %s", (long long)(now_ms() - M.t0_ms),
	     ui_lists_complete(M.ui) ? "" : " (game lists still loading)", BOOT_OK_PATH);
	spawn_detached(argv, 0);
}

/* The first frame of a loaded UI: /run/rsos/menu-up (rcS's deferred jobs,
 * bootlog, mixer, governor, wait for it), in charge mode too. */
static void boot_ok(void)
{
	int fd;

	M.boot_ok_done = true;
	fd = open(P(MENU_UP_PATH), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		char b[32];
		int n = snprintf(b, sizeof(b), "%lld\n", (long long)(now_ms() - M.t0_ms));

		if (write(fd, b, (size_t)n) < 0) {
			/* the marker's existence is what counts */
		}
		close(fd);
	}
	if (M.charge)
		mlog("menu up %lld ms after start (charge mode): %s once the menu starts",
		     (long long)(now_ms() - M.t0_ms), BOOT_OK_PATH);
}

/*
 * Notes the init scripts leave for one message at boot (system layer): the
 * update fallback and what happened to /data. Once, when the menu is up.
 */
static void boot_notes(void)
{
	char w[64];
	char msg[1024] = "";
	bool warn = false;

	M.boot_notes_shown = true;
	if (access(P(DATA_RESTORE_PATH), F_OK) == 0) {
		/* TRANSLATORS: boot message: the SD card's data partition (games, saves) */
		snprintf(msg, sizeof(msg), "%s", _("The data partition could not be restored. Your games and saves may "
						 "need to be copied again."));
		warn = true;
	} else if (access(P(DATA_REFORMAT_PATH), F_OK) == 0) {
		snprintf(msg, sizeof(msg), "%s", _("The data partition was unreadable and was reformatted."));
		warn = true;
	} else if (access(P(DATA_ERROR_PATH), F_OK) == 0) {
		snprintf(msg, sizeof(msg), "%s", _("The data partition has errors: saving may fail. Back up your saves."));
		warn = true;
	}
	if (access(P(BOOT_FALLBACK_PATH), F_OK) == 0) {
		mlog("boot note: fallback from slot %s", read_word(P(BOOT_FALLBACK_PATH), w, sizeof(w)) ? w : "?");
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "%s%s", msg[0] ? "\n" : "",
			 _("The last update didn't start; the previous version was restored."));
		warn = true;
	}
	if (!msg[0] && access(P(DATA_FSCK_PATH), F_OK) == 0) {
		/* nothing lost: a short, calm toast */
		mlog("boot note: data checked (%s)", read_word(P(DATA_FSCK_PATH), w, sizeof(w)) ? w : "?");
		ui_toast(M.ui, _("The SD card was checked after an unexpected power loss"), UI_SEV_INFO);
		return;
	}
	if (!msg[0])
		return;
	mlog("boot note: %s", msg);
	if (warn)
		ui_message(M.ui, msg);
}

/* The flushed exit after init was signalled (also rcK's SIGTERM path). */
static void shutdown_exit(int rc) __attribute__((noreturn));
static void shutdown_exit(int rc)
{
	if (M.expect_failed && rc == 0)
		rc = 1;
	wd_close();
	mlog("exit %d after %lld ms (state flushed at the power-off request)", rc, (long long)(now_ms() - M.t0_ms));
	log_finish();
	_exit(rc);
}

/*
 * The power-off (or reboot) itself, right after the "Powering off..." frame
 * (docs/power.md, "Shutdown"): the UI state (settings, gamedb) and a USB copy
 * are flushed here, the saves of a game were fsync'ed by the game process and
 * resume.ini by resume_record(). No sync(): rcK saves the clock, syncs and
 * unmounts /data. Then init is signalled and the process exits at once
 * (rcK's wait for the frontend finds nothing to stop); the kernel turns the
 * screen off when our DRM file closes.
 */
static void finish_shutdown(void)
{
	char up[32] = "0", line[48];
	int r;

	M.shutdown_at = 0;
	if (M.poweroff_done)
		return;
	M.poweroff_done = true;
	if (M.ui)
		ui_power_event(M.ui, UI_PWR_SHUTDOWN);   /* flush again: the game may have changed gamedb */
	if (M.usb_started) {
		transfer_stop_jobs();
		transfer_usb_shutdown();
	}
	M.usb_started = false;
	/* the "Powering off..." frame reaches the screen before we go */
	for (int i = 0; i < 5 && M.display_ok && !M.headless && !M.display_left && display_flip_pending(); i++)
		display_wait_events(20);
	mlog("powering off (%s)", power_reason_name(M.shutdown_why));
	fflush(stderr);
	if (!read_word("/proc/uptime", up, sizeof(up)))
		snprintf(up, sizeof(up), "0");
	snprintf(line, sizeof(line), "%s\n", up);
	write_run_file(P(SHUTDOWN_SIGNAL_PATH), line);
	r = M.power_ok ? power_poweroff(M.shutdown_why == POWER_REASON_REBOOT) : -ENODEV;
	if (r < 0) {
		mlog("power_poweroff: %s", strerror(-r));
		return;          /* init was not signalled: the loop goes on until SIGTERM */
	}
	shutdown_exit(0);
}

/* ------------------------------------------------------------ test script */
static int btn_from_name(const char *s)
{
	static const struct { const char *n; int b; } map[] = {
		{ "up", IN_UP }, { "down", IN_DOWN }, { "left", IN_LEFT }, { "right", IN_RIGHT },
		{ "a", IN_A }, { "b", IN_B }, { "x", IN_X }, { "y", IN_Y },
		{ "l", IN_L }, { "r", IN_R }, { "l2", IN_L2 }, { "r2", IN_R2 },
		{ "select", IN_SELECT }, { "start", IN_START },
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(s, map[i].n))
			return map[i].b;
	return -1;
}

static void script_parse(const char *text)
{
	char *buf = strdup(text), *save = NULL, *tok;

	if (!buf)
		return;
	for (tok = strtok_r(buf, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
		struct script_tok *n = realloc(M.script, sizeof(*n) * (size_t)(M.script_n + 1));

		if (!n)
			break;
		M.script = n;
		snprintf(M.script[M.script_n++].text, sizeof(M.script[0].text), "%s", tok);
	}
	free(buf);
}

static void shot(const char *file)
{
	if (!M.hl_fb || !M.ui)
		return;
	ui_update(M.ui, now_ms());
	scr_draw();
	write_png(file, M.hl_fb, M.hl_fb_w, M.hl_fb_h, M.hl_fb_w);
}

/*
 * While a game runs (from launch_idle(): the main loop is blocked in the
 * launch callback), only tokens prefixed "game:" run: game:wait:MS,
 * game:pek (a short power-key press), game:poweroff, game:term. A button
 * still held from the token that launched the game is released for the
 * power module only (the UI gets nothing while a game runs).
 */
static void script_in_game(void)
{
	int64_t t = now_ms();

	while (M.script && t >= M.script_at) {
		const char *s;

		if (M.pending_release >= 0) {
			if (M.power_ok)
				power_inject_key(M.pending_release == SCRIPT_PEK_UP ? KEY_POWER :
						 BTN_TRIGGER_HAPPY + M.pending_release, 0);
			M.pending_release = -1;
			M.script_at = t + 100;
			continue;
		}
		if (M.script_i >= M.script_n || strncmp(M.script[M.script_i].text, "game:", 5))
			return;
		s = M.script[M.script_i++].text + 5;
		mlog("script (in game): %s", s);
		M.script_at = t + 50;
		if (!strncmp(s, "wait:", 5)) {
			M.script_at = t + atoi(s + 5);
		} else if (!strcmp(s, "pek")) {
			if (M.power_ok)
				power_inject_key(KEY_POWER, 1);
			M.pending_release = SCRIPT_PEK_UP;
			M.script_at = t + 100;
		} else if (!strncmp(s, "idlepoweroff:", 13)) {
			if (M.power_ok && power_set_setting("idle_poweroff_s", s + 13) < 0)
				mlog("script: bad %s", s);
		} else if (!strcmp(s, "poweroff")) {
			if (M.power_ok)
				power_request_shutdown(POWER_REASON_USER);
		} else if (!strcmp(s, "term")) {
			raise(SIGTERM);
		} else {
			mlog("script: unknown in-game token %s", s);
		}
	}
}

/* Runs due script steps. Returns ms until the next one (-1 = none). */
static int script_run(void)
{
	int64_t t = now_ms();

	while (M.script && t >= M.script_at) {
		const char *s;
		int b;

		if (M.pending_release >= 0) {
			if (M.pending_release == SCRIPT_PEK_UP) {
				if (M.power_ok)
					power_inject_key(KEY_POWER, 0);
			} else {
				/* as on the device: the power module reads the built-in
				 * key from its own evdev fd before the UI gets it */
				if (M.power_ok)
					power_inject_key(BTN_TRIGGER_HAPPY + M.pending_release, 0);
				if (M.ui)
					ui_button(M.ui, (enum input_btn)M.pending_release, IN_NAV_RELEASE);
			}
			M.pending_release = -1;
			M.script_at = t + 150;
			continue;
		}
		if (M.script_i >= M.script_n || !M.ui || (!ui_is_loaded(M.ui) && !M.charge)) {
			if (M.script_i >= M.script_n)
				return -1;
			M.script_at = t + 50;   /* wait for the menu */
			break;
		}
		s = M.script[M.script_i++].text;
		mlog("script: %s", s);
		M.script_at = t + 150;
		if ((b = btn_from_name(s)) >= 0) {
			if (M.power_ok)
				power_inject_key(BTN_TRIGGER_HAPPY + b, 1);
			ui_button(M.ui, (enum input_btn)b, IN_NAV_PRESS);
			M.pending_release = b;
			M.script_at = t + 60;
		} else if (!strncmp(s, "wait:", 5)) {
			M.script_at = t + atoi(s + 5);
		} else if (!strncmp(s, "shot:", 5)) {
			shot(s + 5);
		} else if (!strcmp(s, "hdmi") || !strcmp(s, "lcd")) {
			hl_output(!strcmp(s, "hdmi"), DISPLAY_EVENT_HOTPLUG);
		} else if (!strcmp(s, "pek")) {
			/* a short press of the power key (100 ms) */
			if (M.power_ok)
				power_inject_key(KEY_POWER, 1);
			M.pending_release = SCRIPT_PEK_UP;
			M.script_at = t + 100;
		} else if (!strncmp(s, "idleoff:", 8)) {
			/* idle screen-off after N seconds (the power module's timer) */
			if (M.power_ok && power_set_setting("idle_off_s", s + 8) < 0)
				mlog("script: bad %s", s);
		} else if (!strncmp(s, "idlepoweroff:", 13)) {
			/* idle power-off after N seconds (its notice 10 s before) */
			if (M.power_ok && power_set_setting("idle_poweroff_s", s + 13) < 0)
				mlog("script: bad %s", s);
		} else if (!strcmp(s, "poweroff")) {
			if (M.power_ok)
				power_request_shutdown(POWER_REASON_USER);
		} else if (!strncmp(s, "plug:", 5) || !strncmp(s, "unplug:", 7)) {
			/* a USB disk appears in / leaves the fake /sys/block (its
			 * device folder is made by tests/fake-usb-stick.sh) */
			bool plug = s[0] == 'p';
			const char *name = s + (plug ? 5 : 7);
			char link[PATH_MAX], target[PATH_MAX];

			snprintf(link, sizeof(link), "%s/%s", P("/sys/block"), name);
			snprintf(target, sizeof(target), "../devices/platform/soc/1c14000.usb/usb1/1-1/host0/%s", name);
			if (plug ? symlink(target, link) : unlink(link))
				mlog("script: %s: %s", s, strerror(errno));
		} else if (!strncmp(s, "expect:", 7)) {
			/* the top screen (ui_debug_screen) must contain the text; '_' = ' ' */
			char want[256], got[640];

			snprintf(want, sizeof(want), "%s", s + 7);
			for (char *c = want; *c; c++)
				if (*c == '_')
					*c = ' ';
			ui_update(M.ui, now_ms());
			ui_debug_screen(M.ui, got, sizeof(got));
			if (strstr(got, want)) {
				mlog("script: expect ok: %s", want);
			} else {
				mlog("script: EXPECT FAILED: want \"%s\", screen \"%s\"", want, got);
				M.expect_failed = true;
			}
		} else if (!strcmp(s, "term")) {
			raise(SIGTERM);
		} else if (!strncmp(s, "game:", 5)) {
			mlog("script: %s: no game running, skipped", s);
		} else {
			mlog("script: unknown token %s", s);
		}
		t = now_ms();
	}
	if (!M.script)
		return -1;
	return (int)(M.script_at > t ? M.script_at - t : 0);
}

/* ------------------------------------------------------------------ main */
/*
 * rsos-frontend --splash [--message TEXT]: the boot logo plus one line of
 * text, holding DRM master until SIGTERM (data-partition runs it during the
 * first-boot format/conversion). Follows hotplugs. Nothing else starts.
 */
/* The messages data-partition (board/common) passes to --splash --message:
 * shown translated when the language is known (--lang, or settings.ini if
 * /data is already mounted; not on a first boot: nothing chosen yet). */
static const char *const g_splash_messages[] = {
	/* TRANSLATORS: boot screen under the logo (first start, or after an update) */
	N_("Preparing the SD card, please wait..."),
	/* TRANSLATORS: boot screen under the logo, after the power was cut */
	N_("Checking the SD card, please wait..."),
};

static void splash_language(void)
{
	char fonts[PATH_MAX];
	const char *lang = M.lang;
	struct settings *s = NULL;

	i18n_set_dir(M.locale_dir);
	if (!lang || !*lang) {
		s = access(P("/data/rsos/settings.ini"), R_OK) == 0 ? settings_open(P("/data/rsos/settings.ini")) : NULL;
		lang = s ? settings_get(s, "language", NULL) : NULL;
	}
	if (lang && *lang)
		i18n_set_language(lang);
	if (s)
		settings_close(s);
	snprintf(fonts, sizeof(fonts), "%s/fonts", M.res_dir ? M.res_dir : "/usr/share/rsos");
	font_setup_dir(fonts);
	(void)g_splash_messages;
}

/*
 * Its log (stderr) is /run/rsos/splash.log on the device (data-partition
 * copies its end to the first-boot trace): one line per step, so a splash
 * that never shows the logo or never exits says where it stopped.
 */
static int splash_main(const char *message)
{
	struct splash sp;
	int r, tries = 0;
	bool shown = false;
	int64_t t0 = now_ms(), t;

	/* First: SIGTERM must end it from the start (SPLASH_EXIT_S). */
	g_splash_mode = 1;
	signals_setup();
	mlog("splash: pid %d started", (int)getpid());
	splash_language();
	if (message)
		message = _(message);
	r = splash_load(splash_path(), &sp);
	if (r < 0)
		mlog("splash: %s: %s (message only)", splash_path(), strerror(-r));
	board_init(P(BOARD_INI_DEFAULT));
	display_config_defaults(&M.dcfg);
	board_apply_display(board_get(), &M.dcfg);
	/*
	 * No backlight writes from the splash (backlight_name "" = none): the
	 * panel's own enable (drm_panel) lights it with the first modeset, and
	 * nothing here turns the screen off. The menu takes over bl_power
	 * (panel safety, display-design.md 8.5); the panel keeps scanning all
	 * the same (panel_keep_scanning stays on).
	 */
	M.dcfg.backlight_name = "";
	M.dcfg.on_output = on_output;
	/* The DRM driver may still be probing this early in rcS. */
	while (!M.quit && scr_init() < 0 && ++tries < 30)
		usleep(100000);
	if (!M.display_ok) {
		mlog("splash: no display after %d tries (%lld ms)%s", tries, (long long)(now_ms() - t0),
		     M.quit ? ", asked to quit" : "");
		splash_free(&sp);
		return M.quit ? 0 : 1;
	}
	mlog("splash: display up in %lld ms (%s %s)", (long long)(now_ms() - t0), M.out.name, M.out.mode_name);
	panel_picture();
	M.output_dirty = true;
	while (!M.quit) {
		struct pollfd pfd[2];
		int nfd = 0;

		if (M.output_dirty) {
			M.output_dirty = false;
			if (splash_present(&sp, message) < 0) {
				M.output_dirty = true;   /* try again */
			} else {
				if (!shown)
					mlog("splash: logo shown %lld ms after start", (long long)(now_ms() - t0));
				shown = true;
				if (M.headless && M.shot_file)
					break;
			}
		}
		if (M.sigfd >= 0) {
			pfd[nfd].fd = M.sigfd;
			pfd[nfd++].events = POLLIN;
		}
		if (!M.headless) {
			pfd[nfd].fd = display_get_fd();
			pfd[nfd++].events = POLLIN;
		}
		if (poll(pfd, (nfds_t)nfd, M.output_dirty ? 100 : -1) < 0 && errno != EINTR)
			break;
		if (!M.headless)
			display_handle_events();   /* a hotplug calls on_output(): redraw */
	}
	if (M.quit)
		mlog("splash: signal %d, closing the display%s", (int)M.quit, shown ? "" : " (the logo was never shown)");
	if (M.headless && M.shot_file && M.hl_fb)
		r = write_png(M.shot_file, M.hl_fb, M.hl_fb_w, M.hl_fb_h, M.hl_fb_w);
	else
		r = 0;
	t = now_ms();
	scr_shutdown();
	mlog("splash: display closed in %lld ms, exiting", (long long)(now_ms() - t));
	splash_free(&sp);
	return r ? 1 : 0;
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: rsos-frontend                 start the menu (normally from init)\n"
		"       rsos-frontend --run ARGS...   run one game (see rsos-frontend --run --help)\n"
		"       rsos-frontend --splash [--message TEXT]\n"
		"                                     boot logo (+ a message line) until SIGTERM\n"
		"       rsos-frontend --version\n"
		"test options (development host):\n"
		"  --headless [WxH]   no DRM: in-memory surface (default 640x480)\n"
		"  --root DIR         prefix for /data, /sys, /dev, /run, /media, cores\n"
		"  --res DIR          resources (fonts, gamecontrollerdb.txt), default /usr/share/rsos\n"
		"  --themes DIR       built-in themes, default /usr/share/rsos/themes\n"
		"  --locale DIR       translations (<lang>.cat), default /usr/share/rsos/locale\n"
		"  --lang CODE        language (fr, pt_BR...), wins over settings.ini language=\n"
		"  --script \"...\"     buttons (a b up ... start), wait:MS, shot:FILE.png, hdmi, lcd,\n"
		"                     pek (power key short press), idleoff:S (screen off after S s\n"
		"                     idle), poweroff, term; while a game runs: game:wait:MS,\n"
		"                     game:pek, game:poweroff, game:term\n"
		"  --run-ms MS        headless: exit after MS (default 60000)\n"
		"  --log FILE         log copy (default " LOG_PATH ", \"\" = none)\n"
		"  --splash-file FILE the boot logo (default <res>/splash.rle)\n"
		"  --shot FILE.png    with --splash --headless: write the frame and exit\n"
		"  --watchdog PATH    the watchdog device (default /dev/watchdog; headless: none)\n");
}

static void print_version(void)
{
	char v[160] = "";
	FILE *f = fopen("/etc/rsos-version", "re");

	if (f) {
		if (!fgets(v, sizeof(v), f))
			v[0] = 0;
		fclose(f);
	}
	v[strcspn(v, "\r\n")] = 0;
	printf("%s\n", v[0] ? v : "RetroStoneOS frontend (development)");
}

int main(int argc, char **argv)
{
	struct input_config ic;
	struct ui_config uc;
	struct settings *s;
	char word[32];
	const char *script = NULL, *message = NULL;
	bool splash_mode = false;
	int rc = 0;

	/* The game process: nothing of the menu is initialised. */
	if (argc > 1 && !strcmp(argv[1], "--run"))
		return host_main(argc - 1, argv + 1);

	M.t0_ms = now_ms();
	M.t0_us = now_us();
	M.log_path = LOG_PATH;
	M.run_ms = 60000;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "--version")) {
			print_version();
			return 0;
		} else if (!strcmp(a, "--headless")) {
			M.headless = true;
			M.hl_w = 640;
			M.hl_h = 480;
			if (v && sscanf(v, "%dx%d", &M.hl_w, &M.hl_h) == 2) {
				i++;
				if (M.hl_w < 64 || M.hl_h < 64 || M.hl_w > 4096 || M.hl_h > 4096) {
					usage(stderr);
					return 2;
				}
			}
		} else if (!strcmp(a, "--root") && v) {
			M.root = argv[++i];
		} else if (!strcmp(a, "--res") && v) {
			M.res_dir = argv[++i];
		} else if (!strcmp(a, "--themes") && v) {
			M.themes_dir = argv[++i];
		} else if (!strcmp(a, "--locale") && v) {
			M.locale_dir = argv[++i];
		} else if (!strcmp(a, "--lang") && v) {
			M.lang = argv[++i];
		} else if (!strcmp(a, "--script") && v) {
			script = argv[++i];
		} else if (!strcmp(a, "--run-ms") && v) {
			M.run_ms = atoi(argv[++i]);
		} else if (!strcmp(a, "--log") && v) {
			M.log_path = argv[++i];
		} else if (!strcmp(a, "--splash")) {
			splash_mode = true;
		} else if (!strcmp(a, "--message") && v) {
			message = argv[++i];
		} else if (!strcmp(a, "--splash-file") && v) {
			M.splash_file = argv[++i];
		} else if (!strcmp(a, "--shot") && v) {
			M.shot_file = argv[++i];
		} else if (!strcmp(a, "--watchdog") && v) {
			M.wd_path = argv[++i];   /* tests: a plain file stands in */
		} else {
			fprintf(stderr, "rsos-frontend: unknown or incomplete argument %s\n", a);
			usage(stderr);
			return 2;
		}
	}
	if (splash_mode)
		return splash_main(message);     /* never the watchdog: only the menu process */
	if (!M.wd_path && !M.headless)
		M.wd_path = WATCHDOG_PATH;
	if (M.root && *M.root && !strcmp(M.log_path, LOG_PATH))
		M.log_path = P(LOG_PATH);

	signals_setup();
	log_setup(M.log_path);
	mlog("starting%s (pid %d)", M.headless ? " headless" : "", (int)getpid());
	if (script)
		script_parse(script);

	snprintf(M.settings_path, sizeof(M.settings_path), "%s", P("/data/rsos/settings.ini"));
	snprintf(M.battery_path, sizeof(M.battery_path), "%s", P(BATTERY_FILE));
	/* The board profile (/etc/rsos/board.ini, docs/porting.md): read once,
	 * and handed to the game process through RSOS_BOARD_INI. */
	board_init(P(BOARD_INI_DEFAULT));
	{
		char bd[320];

		board_describe(board_get(), bd, sizeof(bd));
		mlog("%s", bd);
	}
	s = settings_open(M.settings_path);
	display_config_defaults(&M.dcfg);
	board_apply_display(board_get(), &M.dcfg);
	M.dcfg.backlight_dir = P("/sys/class/backlight");   /* panel_keep_scanning: bl_power */
	hdmi_mode_to_cfg(settings_get(s, "hdmi_mode", "auto"), &M.dcfg);
	M.dcfg.lcd_refresh_hz = display_parse_lcd_refresh(settings_get(s, "lcd_refresh", NULL));
	M.logo_min_ms = settings_get_int(s, "boot_logo_min_ms", 0);
	if (M.logo_min_ms < 0 || M.logo_min_ms > 10000)
		M.logo_min_ms = 0;
	settings_close(s);
	M.dcfg.on_output = on_output;

	/* Charge mode: known before the UI starts, so it never scans. */
	if (read_word(P("/run/rsos/bootreason"), word, sizeof(word)) && !strcmp(word, "charger")) {
		M.charge = true;
		mlog("boot reason: charger (charge mode)");
	}

	/* 1. display (the first output lights up in here) */
	display_try_init();

	/* 2. input (bound to the UI at creation) */
	input_config_defaults(&ic);   /* handle_power_key = false: the power module owns KEY_POWER */
	board_apply_input(board_get(), &ic);
	if (M.root && *M.root) {
		ic.dev_dir = P("/dev/input");
		ic.user_map_dir = P("/data/rsos/input");
		ic.remap_user_dir = P("/data/rsos/remaps");
		ic.backlight_dir = P("/sys/class/backlight");
	}
	if (M.res_dir) {
		static char gcdb[PATH_MAX], remaps[PATH_MAX];

		snprintf(gcdb, sizeof(gcdb), "%s/gamecontrollerdb.txt", M.res_dir);
		snprintf(remaps, sizeof(remaps), "%s/remaps", M.res_dir);
		ic.gcdb_path = gcdb;
		ic.remap_sys_dir = remaps;
	}
	M.in = input_open(&ic);
	if (!M.in)
		mlog("input_open failed: menu without buttons");

	/* 3. UI (cheap: settings + core .ini files; lists and themes load in
	 * slices from ui_update(), cached assets first) */
	ui_config_defaults(&uc);
	board_apply_ui(board_get(), &uc);
	if (M.root && *M.root) {
		uc.roms_dir = P("/data/roms");
		uc.data_dir = P("/data/rsos");
		uc.cache_dir = P("/data/rsos/cache");
		uc.themes_user = P("/data/themes");
		uc.cores_dir = P("/usr/share/rsos/cores");
		uc.bios_dir = P("/data/bios");
		uc.boot_env = P("/boot/rsos.env");
		uc.power_supply_dir = P("/sys/class/power_supply");
		uc.backlight_dir = P("/sys/class/backlight");
		uc.wpa_conf = P("/data/rsos/wpa_supplicant.conf");
	}
	if (M.headless)
		uc.net_helper = "/bin/false";
	if (M.res_dir)
		uc.res_dir = M.res_dir;
	if (M.themes_dir)
		uc.themes_builtin = M.themes_dir;
	if (M.locale_dir)
		uc.locale_dir = M.locale_dir;
	uc.language = M.lang;
	/* no language chosen yet (first boot, or an update from a version
	 * without translations): the language picker before the menu */
	uc.language_prompt = true;
	uc.input = M.in;
	ui_power_api_from_module(&M.pa);
	uc.power = &M.pa;
	ui_transfer_api_from_module(&M.ta);
	if (M.root && *M.root)
		M.ta.data_root = P("/data");
	uc.transfer = &M.ta;
	uc.cb.launch = launch_game;
	uc.cb.has_resume = has_resume;
	uc.cb.game_saves = game_saves;
	uc.cb.setting_changed = setting_changed;
	M.ui = ui_create(&uc);
	if (!M.ui) {
		mlog("ui_create failed");
		rc = 1;
		goto out;
	}
	if (M.charge)
		ui_set_charge_mode(M.ui, true);
	M.need_redraw = true;

	/* 4. the loop: first frame at the first iteration, then power, USB */
	wd_open();
	while (!M.quit) {
		struct pollfd pfd[5];
		int nfd = 0, t, i_usb = -1;
		int64_t now = now_ms();

		wd_pet();

		if (!M.display_ok && now >= M.display_retry_at)
			display_try_init();
		if (M.display_reinit && M.display_ok && !M.headless) {
			M.display_reinit = false;
			mlog("re-opening the display for the new HDMI mode");
			scr_shutdown();
			display_try_init();
		}
		apply_output();

		/* boot_logo_min_ms (default 0): keep the logo up that long */
		if (M.splash_at && !M.first_frame && now < M.splash_at + M.logo_min_ms) {
			ui_update(M.ui, now);   /* the UI keeps loading meanwhile */
			M.need_redraw = true;
		} else {
			int64_t tu = now_us();
			bool dirty = ui_update(M.ui, now);

			M.update_us = now_us() - tu;   /* (frame log) */
			if (dirty || M.need_redraw) {
				if (now < M.draw_retry_at)
					M.need_redraw = true;   /* drawn once the back-off is over */
				else if (scr_draw())
					M.need_redraw = false;
			}
		}

		if (!M.power_tried && (M.first_frame || now - M.t0_ms >= POWER_LATE_MS))
			power_start();
		if (!M.usb_tried && M.first_frame && !M.charge && M.power_tried)
			usb_start();
		if (!M.boot_ok_done && M.first_frame && ui_is_loaded(M.ui))
			boot_ok();
		/* the normal menu (at boot, or after the charge-mode exit) */
		if (M.boot_ok_done && !M.charge && !M.boot_ok_spawned)
			boot_ok_spawn();
		/* both wait for the first-boot language picker (in the language chosen) */
		if (M.boot_ok_done && !M.charge && !M.boot_notes_shown && !M.shutdown_req &&
		    !ui_first_boot_busy(M.ui))
			boot_notes();
		/* after the first menu frame (never delays it), not in charge mode */
		if (!M.resume_checked && M.first_frame && ui_is_loaded(M.ui) && !M.charge && !M.shutdown_req &&
		    !ui_first_boot_busy(M.ui))
			resume_check();
		if (!M.lists_logged && ui_lists_complete(M.ui)) {
			M.lists_logged = true;
			mlog("game lists complete %lld ms after start", (long long)(now_ms() - M.t0_ms));
		}
		if (M.shutdown_at && now_ms() >= M.shutdown_at)
			finish_shutdown();
		if (M.headless && now_ms() - M.t0_ms >= M.run_ms) {
			mlog("headless: --run-ms reached");
			break;
		}

		/* timeout: the earliest of every module's deadline */
		now = now_ms();
		t = ui_timeout_ms(M.ui, now);
		if (M.splash_at && !M.first_frame && now < M.splash_at + M.logo_min_ms) {
			/* holding the logo: sleep until then once the UI has loaded */
			if (ui_is_loaded(M.ui))
				t = (int)(M.splash_at + M.logo_min_ms - now);
		} else if (M.need_redraw) {
			t = min_timeout(t, !M.display_ok ? DISPLAY_RETRY_MS :
					   M.draw_retry_at > now ? (int)(M.draw_retry_at - now) : 0);
		}
		if (M.in)
			t = min_timeout(t, input_timeout_ms(M.in));
		if (M.power_ok)
			t = min_timeout(t, power_timeout_ms());
		if (M.usb_started) {
			t = min_timeout(t, transfer_usb_fd() < 0 || now < M.usb_fast_until ? 250 : -1);
			/* a disk settling (a stick present at boot has no uevent) */
			t = min_timeout(t, transfer_usb_timeout_ms());
		}
		if (!M.display_ok)
			t = min_timeout(t, (int)(M.display_retry_at > now ? M.display_retry_at - now : 0));
		if (M.shutdown_at)
			t = min_timeout(t, (int)(M.shutdown_at > now ? M.shutdown_at - now : 0));
		if (!M.power_tried)
			t = min_timeout(t, POWER_LATE_MS);
		if (M.headless) {
			t = min_timeout(t, script_run());
			t = min_timeout(t, (int)(M.t0_ms + M.run_ms - now));
		}
		t = min_timeout(t, wd_timeout());   /* screen off, charge mode: still petted */
		if (M.quit)
			break;
		/* screen off: nothing to show, do not spin on animations */
		if (t >= 0 && t < 100 && !scr_active() && M.display_ok)
			t = 100;

		if (M.sigfd >= 0) {
			pfd[nfd].fd = M.sigfd;
			pfd[nfd++].events = POLLIN;
		}
		if (M.display_ok && !M.headless) {
			pfd[nfd].fd = display_get_fd();
			pfd[nfd++].events = POLLIN;
		}
		if (M.in) {
			pfd[nfd].fd = input_fd(M.in);
			pfd[nfd++].events = POLLIN;
		}
		if (M.power_ok && power_get_fd() >= 0) {
			pfd[nfd].fd = power_get_fd();
			pfd[nfd++].events = POLLIN;
		}
		if (M.usb_started && transfer_usb_fd() >= 0) {
			i_usb = nfd;
			pfd[nfd].fd = transfer_usb_fd();
			pfd[nfd++].events = POLLIN;
		}
		if (poll(pfd, (nfds_t)nfd, t) < 0 && errno != EINTR) {
			mlog("poll: %s", strerror(errno));
			usleep(10000);
		}
		if (M.sigfd >= 0) {
			uint64_t v;

			while (read(M.sigfd, &v, sizeof(v)) > 0)
				;
		}
		if (M.quit)
			break;

		/* every module is cheap to poll when idle: no need to look at
		 * revents (timers inside them run from these calls too) */
		/* the events below happen now, not at the last ui_update() before
		 * the sleep (a toast deadline would start in the past) */
		ui_set_now(M.ui, now_ms());
		if (M.display_ok && !M.headless)
			display_handle_events();
		if (M.power_ok) {
			/* a job that started while the loop slept must hold a
			 * power-off that is due now */
			idle_busy_check(power_timeout_ms() <= 0);
			power_poll();
		}
		input_dispatch();
		pads_activity(false);   /* a stick move in the menu (idle power-off) */
		if (i_usb >= 0 && (pfd[i_usb].revents & POLLIN))
			M.usb_fast_until = now_ms() + USB_FAST_MS;
		usb_poll();
	}
	if (M.quit)
		mlog("signal %d: exiting", (int)M.quit);

out:
	if (M.expect_failed && rc == 0)
		rc = 1;
	if (M.poweroff_done)
		/* rcK's SIGTERM after our own power-off request (only when
		 * signalling init failed): finish_shutdown() flushed everything;
		 * nothing slow here: no redraw, no teardown. */
		shutdown_exit(rc);
	/* SIGTERM from init/rcK without a request of ours: flush, release. */
	wd_pet();
	if (M.usb_started) {
		transfer_stop_jobs();
		transfer_usb_shutdown();
	}
	webshare_stop();
	netnames_stop();
	if (M.ui)
		ui_destroy(M.ui);   /* saves settings.ini and gamedb, flushes the image cache */
	if (M.power_ok)
		power_exit();
	if (M.in)
		input_close(M.in);
	scr_shutdown();
	sync();
	wd_close();
	mlog("exit %d after %lld ms, %d frames", rc, (long long)(now_ms() - M.t0_ms), M.frames);
	log_finish();
	free(M.script);
	return rc;
}
