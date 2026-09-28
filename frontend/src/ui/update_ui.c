/*
 * update_ui.c - Settings > System update (docs/updates.md, docs/ui-design.md
 * §8): the installed version, "Check for updates" (online when there is a
 * network, else the SD card and USB drives; offers to turn WiFi on), the
 * update found with its notes (scrollable) and [Update] / [Later], the
 * progress (download, check, install, verify) and "Restart now"; the
 * optional daily check in the background (a toast, never during a game);
 * "Updated to RetroStoneOS X" once after the restart; "Install system
 * update" in the USB drive dialog.
 *
 * All the work is done by the rsos-update program (src/update), run as a
 * helper process with --machine: one line per event, tab-separated
 * "key=value" fields (\t \n \\ escaped). The menu links neither zstd nor
 * mbedTLS, and a failure of the helper never takes the menu down. One
 * helper at a time.
 *
 * Every text shown goes through i18n; the helper's own messages (English)
 * only go to the log.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ui_internal.h"

enum { K_UPDATE = 130 };
enum { ID_U_VERSION = 1, ID_U_CHECK, ID_U_INSTALL, ID_U_AUTO, ID_U_NOAB };
enum { P_NONE = 0, P_INFO, P_CHECK, P_BGCHECK, P_USBCHECK, P_APPLY, P_BOOT };

#define DAY_S 86400
#define BG_FIRST_MS (3 * 60 * 1000)      /* the first background check: 3 min after start */
#define BG_RETRY_MS (15 * 60 * 1000)     /* then at most every 15 min (network, daily) */
#define BOOT_RETRY_MS (30 * 1000)        /* after a restart: until the new slot is confirmed */
#define WIFI_WAIT_MS (60 * 1000)         /* "turn WiFi on": wait this long for an address */

struct found {
	bool valid, is_signed, installable;
	char status[16];
	char source[8], version[64], where[2048], name[256], page[512], verdict[24];
	long long size;
	char notes[16384];
};

/* the helper process */
static struct {
	pid_t pid;
	int fd;
	int kind;
	char buf[65536];
	size_t len;
} P = { .fd = -1 };

/* what the helper told us */
static struct {
	bool have_info;
	char version[64], board[64], variant[16], reason[160];
	bool ab, tls;
	struct found local, net, best;
	char net_status[16], net_code[24];
	bool checked;                 /* a check completed this session */
	bool has_net;                 /* the check had a network address */
	char status[128];             /* the "Check for updates" value */
	/* install */
	char phase[16];
	long long done, total;
	char done_version[64];
	char err_code[24], err_detail[256];
	struct screen *progress;
	bool cancel_asked;
	/* background, boot */
	int64_t next_bg;
	int64_t next_boot;
	int boot_runs;
	bool boot_done, announced;
	int64_t wifi_until;           /* waiting for WiFi to connect, then check */
	char usb_dir[256];            /* check limited to this drive */
} U;

static void check_done(struct ui *ui);

/* ------------------------------------------------------------ helpers */
static void __attribute__((format(printf, 3, 4))) fmt(char *out, size_t n, const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	vsnprintf(out, n, f, ap);
	va_end(ap);
	/* never half a UTF-8 character at the end */
	for (size_t l = strlen(out), i = l; i > 0 && l - i < 4; i--) {
		unsigned char c = (unsigned char)out[i - 1];

		if ((c & 0xc0) == 0x80)
			continue;
		if (c >= 0xc0 && l - (i - 1) < (size_t)(c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2))
			out[i - 1] = 0;
		break;
	}
}

bool update_available(const struct ui *ui)
{
	/* checked once per helper path (update_poll() asks at every update) */
	static char checked[512];
	static bool avail;
	const char *h = ui->cfg.update_helper;

	if (!h || !*h)
		return false;
	if (strcmp(checked, h)) {
		strlcpy_(checked, h, sizeof(checked));
		avail = access(h, X_OK) == 0;
	}
	return avail;
}

static bool helper_running(void)
{
	return P.pid > 0;
}

static bool helper_start(struct ui *ui, int kind, const char *const *args, bool low_priority)
{
	const char *argv[16];
	int n = 0, pfd[2];
	pid_t pid;

	if (P.pid > 0 || !update_available(ui))
		return false;
	argv[n++] = ui->cfg.update_helper;
	argv[n++] = "--machine";
	for (int i = 0; args && args[i] && n < 15; i++)
		argv[n++] = args[i];
	argv[n] = NULL;
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return false;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return false;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		if (devnull >= 0) {
			dup2(devnull, 0);
			dup2(devnull, 2);
		}
		dup2(pfd[1], 1);
		/* its own session: it finishes an install even if the menu restarts */
		setsid();
		if (low_priority && nice(10) < 0) {
			/* not fatal */
		}
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pfd[1]);
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	P.pid = pid;
	P.fd = pfd[0];
	P.kind = kind;
	P.len = 0;
	if (getenv("RSOS_UPDATE_SYNC")) {
		/* tests (the preview tool runs on virtual time): wait until the
		 * helper is done writing; the next ui_update() reads its lines */
		struct pollfd w = { pfd[0], 0, 0 };
		siginfo_t si;

		poll(&w, 1, 20000);
		/* and exited (not reaped: helper_poll does that) */
		waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT);
	}
	LOGI("update: helper %d: %s %s %s", (int)pid, argv[2] ? argv[2] : "", argv[2] && argv[3] ? argv[3] : "",
	     argv[2] && argv[3] && argv[4] ? argv[4] : "");
	return true;
}

/* "a\tb" escapes of the helper's values */
static void unescape(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			*o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
		} else {
			*o++ = *s;
		}
	}
	*o = 0;
}

struct fields {
	int n;
	char *k[20], *v[20];
};

static const char *get(const struct fields *f, const char *key)
{
	for (int i = 0; i < f->n; i++)
		if (!strcmp(f->k[i], key))
			return f->v[i];
	return "";
}

static void read_found(const struct fields *f, struct found *d)
{
	memset(d, 0, sizeof(*d));
	strlcpy_(d->status, get(f, "status"), sizeof(d->status));
	d->valid = *get(f, "version") != 0;
	strlcpy_(d->source, get(f, "source"), sizeof(d->source));
	strlcpy_(d->version, get(f, "version"), sizeof(d->version));
	strlcpy_(d->where, get(f, "where"), sizeof(d->where));
	strlcpy_(d->name, get(f, "name"), sizeof(d->name));
	strlcpy_(d->page, get(f, "page"), sizeof(d->page));
	strlcpy_(d->verdict, get(f, "verdict"), sizeof(d->verdict));
	strlcpy_(d->notes, get(f, "notes"), sizeof(d->notes));
	d->size = atoll(get(f, "size"));
	d->is_signed = !strcmp(get(f, "signed"), "1");
	d->installable = !strcmp(get(f, "installable"), "1");
}

static void boot_event(struct ui *ui, const char *ev, const char *ver);

static void on_line(struct ui *ui, char *line)
{
	struct fields f = { 0 };
	char *type = line, *p = strchr(line, '\t');

	if (p)
		*p++ = 0;
	while (p && f.n < 20) {
		char *next = strchr(p, '\t'), *eq;

		if (next)
			*next++ = 0;
		eq = strchr(p, '=');
		if (eq) {
			*eq = 0;
			unescape(eq + 1);
			f.k[f.n] = p;
			f.v[f.n++] = eq + 1;
		}
		p = next;
	}
	if (!strcmp(type, "info")) {
		U.have_info = true;
		strlcpy_(U.version, get(&f, "version"), sizeof(U.version));
		strlcpy_(U.board, get(&f, "board"), sizeof(U.board));
		strlcpy_(U.variant, get(&f, "variant"), sizeof(U.variant));
		strlcpy_(U.reason, get(&f, "reason"), sizeof(U.reason));
		U.ab = !strcmp(get(&f, "ab"), "1");
		U.tls = !strcmp(get(&f, "tls"), "1");
	} else if (!strcmp(type, "local")) {
		read_found(&f, &U.local);
	} else if (!strcmp(type, "net")) {
		read_found(&f, &U.net);
		strlcpy_(U.net_status, get(&f, "status"), sizeof(U.net_status));
		strlcpy_(U.net_code, get(&f, "code"), sizeof(U.net_code));
		if (*get(&f, "detail"))
			LOGI("update: online check: %s (%s)", get(&f, "code"), get(&f, "detail"));
	} else if (!strcmp(type, "best")) {
		read_found(&f, &U.best);
	} else if (!strcmp(type, "state") || !strcmp(type, "progress")) {
		strlcpy_(U.phase, get(&f, "phase"), sizeof(U.phase));
		U.done = atoll(get(&f, "done"));
		U.total = atoll(get(&f, "total"));
		ui->dirty = true;
	} else if (!strcmp(type, "done")) {
		strlcpy_(U.done_version, get(&f, "version"), sizeof(U.done_version));
	} else if (!strcmp(type, "error")) {
		strlcpy_(U.err_code, get(&f, "code"), sizeof(U.err_code));
		strlcpy_(U.err_detail, get(&f, "detail"), sizeof(U.err_detail));
		LOGW("update: %s: %s", U.err_code, U.err_detail);
	} else if (!strcmp(type, "boot")) {
		boot_event(ui, get(&f, "event"), get(&f, "version"));
	}
}

/* -------------------------------------------------------- the messages */
/* The helper's result codes, for the user. */
static const char *reason_text(const char *code)
{
	static const struct {
		const char *code, *text;
	} t[] = {
		/* TRANSLATORS: system update errors (a dialog). "Date & time" is the Settings menu */
		{ "clock", N_("The clock is not set. Connect to WiFi and wait a minute, or set the date in Settings > "
			      "Date & time.") },
		{ "network", N_("No connection to the update server. Check the WiFi connection and try again.") },
		{ "tls", N_("The secure connection to the update server failed.") },
		{ "notls", N_("This build cannot connect to the update server.") },
		{ "http", N_("The update server gave an unexpected answer. Try again later.") },
		{ "notfound", N_("The update server gave an unexpected answer. Try again later.") },
		{ "format", N_("This file is not a RetroStoneOS update, or it is damaged.") },
		{ "updater", N_("This update needs a newer updater: install the updates before it first.") },
		{ "unsigned", N_("This update is not signed by RetroStoneOS: it cannot be installed.") },
		{ "badsig", N_("This update is not signed by RetroStoneOS, or it was modified: it cannot be installed.") },
		{ "nokey", N_("This system cannot check updates (no update key).") },
		{ "board", N_("This update is for another console.") },
		{ "bootloader", N_("This update needs a newer bootloader: flash the new image to the SD card instead "
				   "(back up your saves first).") },
		{ "same", N_("This version is already installed.") },
		{ "older", N_("This update is older than the installed version.") },
		{ "payload", N_("The update is damaged: nothing was changed. Download or copy it again.") },
		{ "image", N_("The new system was not written correctly (the SD card may be failing): nothing was "
			      "changed.") },
		{ "io", N_("Read or write error: nothing was changed.") },
		{ "space", N_("Not enough free space on the SD card.") },
		{ "noab", N_("This console is updated by flashing the new image to the SD card.") },
		{ "slot", N_("The system could not be prepared for the update: nothing was changed.") },
		{ "env", N_("The system could not be prepared for the update: nothing was changed.") },
		{ "unconfirmed", N_("The system is still starting up. Try again in a minute.") },
		{ "restart", N_("An update is already installed: restart the console to use it.") },
		{ "busy", N_("An update is already running.") },
		{ "cancelled", N_("Update stopped: nothing was changed.") },
	};

	static char battery[256];

	if (!strcmp(code, "battery")) {
		/* TRANSLATORS: system update refused; %d is the minimum battery level (30) */
		fmt(battery, sizeof(battery), _("The battery is too low (under %d%%): charge it or plug in the charger."),
		    30);
		return battery;
	}
	for (size_t i = 0; i < ARRAY_SIZE(t); i++)
		if (!strcmp(t[i].code, code))
			return _(t[i].text);
	/* TRANSLATORS: system update error, when nothing more precise is known */
	return _("The update failed: nothing was changed.");
}

/* ------------------------------------------------------------ settings */
static void save_key(struct ui *ui, const char *key, const char *value)
{
	settings_set(ui->settings, key, value);
	if (settings_save(ui->settings) < 0)
		LOGW("update: could not save %s", key);
}

const char *update_menu_value(struct ui *ui)
{
	static char v[96];

	(void)ui;
	v[0] = 0;
	if (U.done_version[0] && !helper_running())
		/* TRANSLATORS: Settings > System update value: installed, the restart is still to come */
		fmt(v, sizeof(v), "%s", _("restart to finish"));
	else if (U.best.valid)
		/* TRANSLATORS: Settings > System update value: %s is a version, "0.2.0 available" */
		fmt(v, sizeof(v), _("%s available"), U.best.version);
	else if (U.net.valid && !strcmp(U.net.status, "found"))
		fmt(v, sizeof(v), _("%s available"), U.net.version);
	return v;
}

/* -------------------------------------------------------- notes screen */
struct notes {
	struct screen base;
	struct found f;
	char text[18000];
	int scroll, nlines, visible;
};

/* GitHub release notes are Markdown: drop the markup that would show */
static void clean_notes(const char *in, char *out, size_t n)
{
	size_t o = 0;
	bool bol = true;

	for (const char *p = in; *p && o + 4 < n; p++) {
		if (*p == '\r')
			continue;
		if (bol) {
			while (*p == '#')
				p++;
			while (*p == ' ' && p[-1] == '#')
				p++;
			if ((*p == '*' || *p == '-') && p[1] == ' ') {
				/* a bullet */
				memcpy(out + o, "\xe2\x80\xa2", 3);
				o += 3;
				p++;
			}
		}
		if (!*p)
			break;
		if (p[0] == '*' && p[1] == '*') {
			p++;
			bol = false;
			continue;
		}
		if (*p == '`') {
			bol = false;
			continue;
		}
		out[o++] = *p;
		bol = *p == '\n';
	}
	while (o && (out[o - 1] == '\n' || out[o - 1] == ' '))
		o--;
	out[o] = 0;
}

static void install_start(struct ui *ui, const struct found *f, bool allow_unsigned);

static void unsigned_choice(struct ui *ui, int choice, void *user)
{
	struct found *f = user;

	if (choice == 0)
		install_start(ui, f, true);
	free(f);
}

static void notes_install(struct ui *ui, struct notes *n)
{
	const struct power_status *b = ui_battery(ui);

	if (!n->f.installable) {
		if (!strcmp(n->f.verdict, "noab") || !U.ab)
			/* TRANSLATORS: system update on a board without in-place updates (Raspberry Pi...) */
			message_open(ui, _("This console is updated by flashing the new image to the SD card: download it "
					   "from the RetroStoneOS releases page on GitHub. Back up your saves first (Settings > "
					   "Storage), flashing erases the card."));
		else
			message_open(ui, reason_text(n->f.verdict));
		return;
	}
	if (b && b->valid && b->battery_present && !b->charger_online && b->percent >= 0 && b->percent < 30) {
		message_open(ui, reason_text("battery"));
		return;
	}
	if (!n->f.is_signed) {
		/* TRANSLATORS: dialog buttons, uppercase, short */
		const char *const buttons[] = { _("INSTALL ANYWAY"), _("CANCEL"), NULL };
		struct found *copy = xmalloc(sizeof(*copy));
		struct screen *d;

		*copy = n->f;
		/* TRANSLATORS: development builds only: a package without a signature */
		d = dialog_open(ui, _("This update is not signed. Install it only if you built it yourself."), buttons,
				unsigned_choice, copy);
		if (!d)
			free(copy);
		else
			dialog_select(d, 1);
		return;
	}
	install_start(ui, &n->f, false);
}

static void notes_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct notes *n = (struct notes *)scr;
	const struct menu_style *ms = &ui->ms;
	struct font *fs = font_get(ms->font_path, ui_font_px(ui, ms->small_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : NULL, ui_font_px(ui, ms->font_size * 1.15f));
	int W = ui->w, H = ui->h, pw = W * 90 / 100, ph = H * 82 / 100;
	int x = (W - pw) / 2, y = H * 4 / 100, pad = H / 30, lh = font_height(fs) * 13 / 10;
	int ty = y + pad + font_height(fb) * 2 + font_height(fs) * 2, th = y + ph - pad - ty;
	struct text_line *lines = xmalloc(sizeof(*lines) * 400);
	char buf[320], size[48];
	struct help_prompt prompts[3] = {
		{ "updown", N_("scroll") },
		/* TRANSLATORS: help bar: A installs the update / explains how to update */
		{ "a", n->f.installable ? N_("update") : N_("how to update") },
		/* TRANSLATORS: help bar: B closes the update without installing it */
		{ "b", N_("later") },
	};

	draw_panel(s, x, y, pw, ph, MAX(4, H / 60), ms->bg);
	fmt(buf, sizeof(buf), "RetroStoneOS %s", n->f.version);
	draw_text_box(ui, s, fb, buf, x + pad, y + pad, pw - 2 * pad, font_height(fb) * 3 / 2, AL_LEFT, ms->title);
	i18n_format_size((uint64_t)MAX(0, n->f.size), size, sizeof(size));
	if (!strcmp(n->f.source, "net"))
		/* TRANSLATORS: system update: where it comes from; %s is a size ("84 MB") */
		fmt(buf, sizeof(buf), _("Download: %s"), size);
	else if (!strncmp(n->f.where, "/media/", 7))
		/* TRANSLATORS: system update found on a USB drive; %s is its file name */
		fmt(buf, sizeof(buf), _("On the USB drive: %s"), n->f.name);
	else
		/* TRANSLATORS: system update found on the SD card; %s is its file name */
		fmt(buf, sizeof(buf), _("On the SD card: %s"), n->f.name);
	if (!n->f.is_signed && n->f.installable) {
		size_t l = strlen(buf);

		/* TRANSLATORS: appended to the line above: a development package without a signature */
		fmt(buf + l, sizeof(buf) - l, "  -  %s", _("not signed"));
	}
	if (U.version[0]) {
		size_t l = strlen(buf);

		/* TRANSLATORS: appended: the version installed now */
		fmt(buf + l, sizeof(buf) - l, "  -  %s %s", _("installed:"), U.version);
	}
	draw_text_box(ui, s, fs, buf, x + pad, y + pad + font_height(fb) * 3 / 2, pw - 2 * pad, font_height(fs) * 3 / 2,
		      AL_LEFT, n->f.installable ? ms->text_dim : ms->accent);
	gfx_fill(s, x + pad, ty - pad / 2, pw - 2 * pad, MAX(1, H / 240), ms->separator);
	n->nlines = font_wrap(fs, n->text[0] ? n->text : _("No release notes."), pw - 2 * pad, lines, 400);
	n->visible = MAX(1, th / MAX(1, lh));
	n->scroll = CLAMP(n->scroll, 0, MAX(0, n->nlines - n->visible));
	for (int i = 0; i < n->visible && n->scroll + i < n->nlines; i++) {
		const struct text_line *l = &lines[n->scroll + i];
		const char *t = n->text[0] ? n->text : _("No release notes.");

		font_draw(s, fs, x + pad, font_baseline_in_box(fs, ty + i * lh, lh), t + l->start, l->len, ms->text);
	}
	if (n->nlines > n->visible) {
		/* a scroll bar */
		int bh = MAX(8, th * n->visible / n->nlines);
		int by = ty + (th - bh) * n->scroll / MAX(1, n->nlines - n->visible);

		gfx_fill_round(s, x + pw - pad / 2 - 3, by, 4, bh, 2, ms->text_dim);
	}
	free(lines);
	help_draw(ui, s, &ui->menu_help, prompts, 3);
}

static void notes_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct notes *n = (struct notes *)scr;

	if (t == IN_NAV_RELEASE)
		return;
	switch (b) {
	case IN_UP:
		n->scroll--;
		break;
	case IN_DOWN:
		n->scroll++;
		break;
	case IN_L:
	case IN_LEFT:
		n->scroll -= MAX(1, n->visible - 1);
		break;
	case IN_R:
	case IN_RIGHT:
		n->scroll += MAX(1, n->visible - 1);
		break;
	case IN_A:
		if (t == IN_NAV_PRESS)
			notes_install(ui, n);
		return;
	case IN_B:
		if (t == IN_NAV_PRESS)
			ui_pop(ui);
		return;
	default:
		return;
	}
	n->scroll = CLAMP(n->scroll, 0, MAX(0, n->nlines - n->visible));
	ui->dirty = true;
}

static void notes_destroy(struct ui *ui, struct screen *scr)
{
	(void)ui;
	free(scr);
}

static void notes_relayout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
}

static void notes_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	struct notes *nt = (struct notes *)scr;

	(void)ui;
	snprintf(buf, n, "update:%s|%s|%s", nt->f.version, nt->f.installable ? "installable" : nt->f.verdict,
		 nt->f.source);
}

static const struct screen_ops notes_ops = {
	.button = notes_button,
	.render = notes_render,
	.relayout = notes_relayout,
	.destroy = notes_destroy,
	.opaque = false,
	.describe = notes_describe,
};

static void notes_open(struct ui *ui, const struct found *f)
{
	struct notes *n = xcalloc(1, sizeof(*n));

	n->base.ops = &notes_ops;
	n->f = *f;
	clean_notes(f->notes, n->text, sizeof(n->text));
	LOGI("update: showing %s (%s, %s)", f->version, f->source, f->installable ? "installable" : f->verdict);
	ui_push(ui, &n->base);
}

/* ------------------------------------------------------ progress screen */
struct progress {
	struct screen base;
	char version[64];
};

static void stop_choice(struct ui *ui, int choice, void *user)
{
	(void)ui;
	(void)user;
	if (choice == 0 && P.pid > 0 && P.kind == P_APPLY && strcmp(U.phase, "switch")) {
		LOGI("update: stop requested (phase %s)", U.phase);
		U.cancel_asked = true;
		kill(P.pid, SIGTERM);
	}
}

static void pr_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct progress *p = (struct progress *)scr;
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	struct font *fs = font_get(ms->font_path, ui_font_px(ui, ms->small_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : NULL, ui_font_px(ui, ms->font_size * 1.1f));
	int W = ui->w, H = ui->h, pw = W * 85 / 100, ph = H * 55 / 100;
	int x = (W - pw) / 2, y = (H - ph) / 2, pad = H / 30, lh = font_height(f) * 2;
	int bw = pw - 4 * pad, bh = MAX(8, H / 40);
	long long total = MAX(U.total, 1), done = CLAMP(U.done, 0, total);
	char buf[320], a[48], b[48];
	const char *what;
	bool can_stop = strcmp(U.phase, "switch") != 0;
	/* TRANSLATORS: help bar: B stops the system update */
	static const struct help_prompt prompts[] = { { "b", N_("stop") } };

	draw_panel(s, x, y, pw, ph, MAX(4, H / 60), ms->bg);
	/* TRANSLATORS: system update progress title; %s is the new version */
	fmt(buf, sizeof(buf), _("Updating to RetroStoneOS %s"), p->version);
	draw_text_box(ui, s, fb, buf, x, y + pad, pw, lh, AL_CENTER, ms->title);
	if (!strcmp(U.phase, "download"))
		/* TRANSLATORS: system update steps */
		what = _("Downloading...");
	else if (!strcmp(U.phase, "verify"))
		what = _("Checking the update...");
	else if (!strcmp(U.phase, "write"))
		what = _("Installing...");
	else if (!strcmp(U.phase, "readback"))
		what = _("Checking the installation...");
	else if (!strcmp(U.phase, "switch"))
		what = _("Finishing...");
	else
		what = _("Preparing...");
	gfx_fill_round(s, x + 2 * pad, y + pad + lh * 3 / 2, bw, bh, bh / 2, gfx_with_alpha(ms->text_dim, 70));
	gfx_fill_round(s, x + 2 * pad, y + pad + lh * 3 / 2, MAX(bh, (int)((long long)bw * done / total)), bh, bh / 2,
		       ms->accent);
	if (!strcmp(U.phase, "download") && U.total > 0) {
		i18n_format_size((uint64_t)done, a, sizeof(a));
		i18n_format_size((uint64_t)U.total, b, sizeof(b));
		/* TRANSLATORS: "Downloading...  38 MB of 84 MB" */
		fmt(buf, sizeof(buf), _("%s  %s of %s"), what, a, b);
	} else if (U.total > 0) {
		fmt(buf, sizeof(buf), "%s  %d%%", what, (int)(done * 100 / total));
	} else {
		fmt(buf, sizeof(buf), "%s", what);
	}
	draw_text_box(ui, s, f, buf, x + pad, y + pad + lh * 2, pw - 2 * pad, lh, AL_CENTER, ms->text);
	/* TRANSLATORS: under the system update progress bar */
	draw_text_box(ui, s, fs, _("Your games and saves are kept. If the power goes off, the console starts the "
				   "current version."),
		      x + pad, y + pad + lh * 3, pw - 2 * pad, lh * 2, AL_CENTER, ms->text_dim);
	if (can_stop)
		help_draw(ui, s, &ui->menu_help, prompts, 1);
}

static void pr_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	(void)scr;
	if (t == IN_NAV_PRESS && b == IN_B && strcmp(U.phase, "switch") && P.kind == P_APPLY && P.pid > 0) {
		/* TRANSLATORS: dialog buttons, uppercase, short: stop the update / go on */
		const char *const buttons[] = { _("STOP"), _("CONTINUE"), NULL };
		struct screen *d;

		/* TRANSLATORS: B during a system update */
		d = dialog_open(ui, _("Stop the update? The console keeps its current version."), buttons, stop_choice,
				NULL);
		dialog_select(d, 1);
	}
}

static bool pr_update(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
	return false;                 /* redrawn by on_line() */
}

static int pr_timeout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
	return 250;
}

static void pr_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	(void)ui;
	(void)scr;
	snprintf(buf, n, "progress:update %s %lld/%lld", U.phase[0] ? U.phase : "start", U.done, U.total);
}

static const struct screen_ops pr_ops = {
	.button = pr_button,
	.update = pr_update,
	.render = pr_render,
	.relayout = notes_relayout,
	.destroy = notes_destroy,
	.timeout = pr_timeout,
	.opaque = false,
	.describe = pr_describe,
};

static void install_start(struct ui *ui, const struct found *from, bool allow_unsigned)
{
	/* a copy: from may live in the notes screen, which is closed below */
	static struct found cur;
	const struct found *f = &cur;
	char size[32];
	const char *args[12];
	int n = 0;
	struct progress *p;

	if (from != &cur)
		cur = *from;
	if (helper_running()) {
		if (P.kind != P_BGCHECK) {
			/* TRANSLATORS: toast: the updater is busy (a check runs) */
			ui_toastf(ui, "%s", _("Please wait..."));
			return;
		}
		/* a background check is not worth waiting for */
		kill(P.pid, SIGTERM);
		waitpid(P.pid, NULL, 0);
		close(P.fd);
		P.pid = 0;
		P.fd = -1;
	}
	if (allow_unsigned)
		args[n++] = "--allow-unsigned";
	args[n++] = "apply";
	if (!strcmp(f->source, "net")) {
		snprintf(size, sizeof(size), "%lld", f->size);
		args[n++] = "--url";
		args[n++] = f->where;
		args[n++] = "--name";
		args[n++] = f->name;
		args[n++] = "--size";
		args[n++] = size;
	} else {
		args[n++] = f->where;
	}
	args[n] = NULL;
	U.phase[0] = 0;
	U.done = U.total = 0;
	U.err_code[0] = U.err_detail[0] = U.done_version[0] = 0;
	U.cancel_asked = false;
	if (!helper_start(ui, P_APPLY, args, false)) {
		message_open(ui, reason_text("internal"));
		return;
	}
	/* the notes screen goes, the progress comes */
	if (ui_top(ui) && ui_top(ui)->ops == &notes_ops)
		ui_pop(ui);
	p = xcalloc(1, sizeof(*p));
	p->base.ops = &pr_ops;
	strlcpy_(p->version, f->version, sizeof(p->version));
	U.progress = &p->base;
	if (!ui_push(ui, &p->base))
		U.progress = NULL;
	LOGI("update: installing %s from %s", f->version, f->where);
}

static void restart_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0) {
		LOGI("update: restarting into the new version");
		ui_power(ui, UI_REBOOT);
	} else {
		/* TRANSLATORS: toast after "Later": the update is used at the next start */
		ui_toast_long(ui, "%s", _("The new version starts the next time the console is turned on."));
	}
}

static void apply_done(struct ui *ui, int status)
{
	/* the progress screen, if it is still on the stack */
	for (int i = ui->nstack - 1; i >= 0; i--)
		if (ui->stack[i] == U.progress) {
			while (ui->nstack > i)
				ui_pop(ui);
			break;
		}
	U.progress = NULL;
	if (status == 0 && U.done_version[0]) {
		/* TRANSLATORS: dialog buttons, uppercase, short */
		const char *const buttons[] = { _("RESTART NOW"), _("LATER"), NULL };
		char msg[320];
		struct screen *d;

		LOGI("update: %s installed", U.done_version);
		/* TRANSLATORS: after a system update was installed; %d counts down the seconds */
		fmt(msg, sizeof(msg), _("RetroStoneOS %s is installed. Restart now to use it? Restarting in %%d s."),
		    U.done_version);
		d = dialog_open(ui, msg, buttons, restart_choice, NULL);
		dialog_set_countdown(d, 30, 0);
		U.best.valid = false;
		return;
	}
	if (!strcmp(U.err_code, "cancelled") || (U.cancel_asked && !U.err_code[0])) {
		/* TRANSLATORS: toast: the user stopped the update */
		ui_toast(ui, _("Update stopped: nothing was changed."), UI_SEV_INFO);
		return;
	}
	LOGW("update: failed (%s, exit %d)", U.err_code[0] ? U.err_code : "?", status);
	message_open(ui, reason_text(U.err_code));
}

/* ------------------------------------------------------ the check */
static void wifi_choice(struct ui *ui, int choice, void *user)
{
	const char *argv[] = { ui->cfg.net_helper, "wifi", "on", NULL };

	(void)user;
	if (choice != 0)
		return;
	/* the network settings' own job tag (screens.c JOB_WIFI): its toasts */
	if (hw_job_start(argv, 1) < 0) {
		ui_toastf(ui, _("Cannot run %s"), ui->cfg.net_helper);
		return;
	}
	ui->net_busy[0] = true;
	U.wifi_until = ui->now + WIFI_WAIT_MS;
	/* TRANSLATORS: toast: WiFi is being turned on for the update check */
	ui_toastf(ui, "%s", _("WiFi: turning on, please wait..."));
}

static void check_start(struct ui *ui, const char *dir)
{
	const char *args[6];
	int n = 0;

	if (helper_running()) {
		ui_toastf(ui, "%s", _("Please wait..."));
		return;
	}
	U.has_net = !dir && hw_has_ipv4(NULL, 0);
	memset(&U.local, 0, sizeof(U.local));
	memset(&U.net, 0, sizeof(U.net));
	memset(&U.best, 0, sizeof(U.best));
	U.net_status[0] = U.net_code[0] = 0;
	args[n++] = "check";
	if (!U.has_net)
		args[n++] = "--local-only";
	if (dir) {
		args[n++] = "--dir";
		args[n++] = dir;
	}
	args[n] = NULL;
	if (!helper_start(ui, dir ? P_USBCHECK : P_CHECK, args, false)) {
		message_open(ui, reason_text("internal"));
		return;
	}
	/* TRANSLATORS: Settings > System update: the check runs */
	strlcpy_(U.status, _("Checking..."), sizeof(U.status));
}

static void check_done(struct ui *ui)
{
	const char *wifi = settings_get(ui->settings, "wifi", "0");
	/* TRANSLATORS: dialog buttons, uppercase, short */
	const char *const buttons[] = { _("TURN WIFI ON"), _("CANCEL"), NULL };

	U.checked = true;
	U.status[0] = 0;
	if (U.best.valid) {
		fmt(U.status, sizeof(U.status), _("%s available"), U.best.version);
		notes_open(ui, &U.best);
		return;
	}
	if (U.net.valid && !strcmp(U.net.status, "found")) {
		/* newer online, but not installable here (a board without A/B, a
		 * package that needs a newer bootloader...) */
		fmt(U.status, sizeof(U.status), _("%s available"), U.net.version);
		notes_open(ui, &U.net);
		return;
	}
	if (U.local.valid && !strcmp(U.local.status, "refused") && strcmp(U.local.verdict, "same") &&
	    strcmp(U.local.verdict, "older")) {
		char msg[512];

		/* TRANSLATORS: a package was found but cannot be installed: %1$s its version, %2$s why */
		fmt(msg, sizeof(msg), _("RetroStoneOS %s was found, but: %s"), U.local.version, reason_text(U.local.verdict));
		message_open(ui, msg);
		return;
	}
	if (!strcmp(U.net_status, "error")) {
		/* TRANSLATORS: Settings > System update value after a failed check */
		strlcpy_(U.status, _("Check failed"), sizeof(U.status));
		message_open(ui, reason_text(U.net_code));
		return;
	}
	if (!strcmp(U.net_status, "uptodate") || (U.local.valid && !strcmp(U.local.verdict, "same"))) {
		/* TRANSLATORS: Settings > System update value: no newer version */
		strlcpy_(U.status, _("Up to date"), sizeof(U.status));
		ui_toastf(ui, "%s", _("RetroStoneOS is up to date."));
		return;
	}
	if (P.kind == P_USBCHECK || U.usb_dir[0]) {
		/* TRANSLATORS: toast: nothing for this console on the USB drive */
		ui_toastf(ui, "%s", _("No update for this console on the USB drive."));
		return;
	}
	if (!U.has_net) {
		if (!parse_bool(wifi, false)) {
			/* TRANSLATORS: no update on the SD card or a USB drive, and no network */
			dialog_open(ui, _("No update was found on the SD card or on a USB drive. Turn WiFi on to check "
					  "online?"), buttons, wifi_choice, NULL);
		} else {
			message_open(ui, _("No update was found on the SD card or on a USB drive, and WiFi is not connected "
					   "yet. Check the WiFi settings (Settings > Network)."));
		}
		return;
	}
	/* TRANSLATORS: Settings > System update value: no release for this console yet */
	strlcpy_(U.status, _("No update found"), sizeof(U.status));
	ui_toastf(ui, "%s", _("No update found for this console."));
}

/* the daily check in the background: a toast for a new version, once */
static void bgcheck_done(struct ui *ui)
{
	const struct found *f = U.best.valid ? &U.best : U.net.valid && !strcmp(U.net.status, "found") ? &U.net : NULL;
	char v[32];

	if (strcmp(U.net_status, "error")) {
		snprintf(v, sizeof(v), "%lld", (long long)time(NULL));
		save_key(ui, "update_last_check", v);
	}
	if (!f)
		return;
	if (!strcmp(settings_get(ui->settings, "update_notified", ""), f->version))
		return;
	save_key(ui, "update_notified", f->version);
	LOGI("update: background check: %s available", f->version);
	/* TRANSLATORS: toast after the daily check; "Settings > System update" must match those menus */
	ui_toast_long(ui, _("RetroStoneOS %s is available: Settings > System update"), f->version);
}

/* ------------------------------------------------------ after a restart */
static void boot_event(struct ui *ui, const char *ev, const char *ver)
{
	LOGI("update: after the restart: %s %s", ev, ver);
	if (!strcmp(ev, "updated") && !U.announced) {
		char msg[256];

		/* TRANSLATORS: shown once after a system update, at the first start of the new version */
		fmt(msg, sizeof(msg), _("Updated to RetroStoneOS %s."), ver);
		ui_message(ui, msg);
		U.announced = true;           /* once, even if the helper says it again */
	}
	if (!strcmp(ev, "none") || !strcmp(ev, "confirmed") || !strcmp(ev, "failed"))
		U.boot_done = true;       /* failed: main.c's boot note says it */
}

/* ------------------------------------------------------------ polling */
static void helper_finished(struct ui *ui, int status)
{
	int kind = P.kind;

	P.pid = 0;
	P.kind = P_NONE;
	switch (kind) {
	case P_CHECK:
	case P_USBCHECK:
		check_done(ui);
		U.usb_dir[0] = 0;
		break;
	case P_BGCHECK:
		bgcheck_done(ui);
		break;
	case P_APPLY:
		apply_done(ui, status);
		break;
	default:
		break;
	}
	ui->dirty = true;
}

static void helper_poll(struct ui *ui)
{
	int st;

	if (P.pid <= 0)
		return;
	while (P.fd >= 0) {
		static char chunk[4096];
		ssize_t r;
		char *nl;

		/* read into a small buffer, append what fits (an overlong line,
		 * which the helper never writes, is dropped) */
		r = read(P.fd, chunk, sizeof(chunk));
		if (r > 0) {
			if (P.len + (size_t)r >= sizeof(P.buf))
				P.len = 0;
			memcpy(P.buf + P.len, chunk, (size_t)r);
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			break;                /* EAGAIN: later */
		if (r == 0) {
			close(P.fd);
			P.fd = -1;
			break;
		}
		P.len += (size_t)r;
		P.buf[P.len] = 0;
		while ((nl = memchr(P.buf, '\n', P.len))) {
			size_t l = (size_t)(nl - P.buf) + 1;

			*nl = 0;
			on_line(ui, P.buf);
			memmove(P.buf, P.buf + l, P.len - l);
			P.len -= l;
		}
	}
	if (P.fd < 0 && waitpid(P.pid, &st, WNOHANG) == P.pid) {
		int status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

		LOGI("update: helper %d finished (%d)", (int)P.pid, status);
		helper_finished(ui, status);
	}
}

void update_poll(struct ui *ui)
{
	helper_poll(ui);
	/* nothing of ours while the game lists still load (boot: the SD card is
	 * busy; a dialog would also hold back the carousel's update) */
	if (!ui->loaded || ui->charge_mode || ui->in_game || !update_available(ui) || !ui_lists_complete(ui))
		return;
	/* after a restart: "Updated to ...", then the clean-up once confirmed */
	if (!U.boot_done && !helper_running() && ui->now >= U.next_boot && !ui_first_boot_busy(ui)) {
		char p[1100];

		snprintf(p, sizeof(p), "%s/update-state.ini", ui->cfg.data_dir);
		if (!file_exists(p) || U.boot_runs >= 20) {
			U.boot_done = true;
		} else {
			const char *args[] = { "boot", NULL };

			U.boot_runs++;
			U.next_boot = ui->now + BOOT_RETRY_MS;
			helper_start(ui, P_BOOT, args, true);
		}
	}
	/* WiFi turned on for a check: check as soon as it has an address */
	if (U.wifi_until && !helper_running()) {
		if (hw_has_ipv4(NULL, 0)) {
			U.wifi_until = 0;
			check_start(ui, NULL);
		} else if (ui->now > U.wifi_until) {
			U.wifi_until = 0;
			message_open(ui, reason_text("network"));
		}
	}
	/* the daily check (a setting, on by default) */
	if (!U.next_bg)
		U.next_bg = ui->now + BG_FIRST_MS;
	if (ui->now >= U.next_bg && !helper_running()) {
		time_t now = time(NULL);
		long long last = atoll(settings_get(ui->settings, "update_last_check", "0"));
		const char *args[] = { "check", "--net-only", NULL };

		U.next_bg = ui->now + BG_RETRY_MS;
		if (settings_get_bool(ui->settings, "update_auto", true) && now > 1700000000 &&
		    (now - last >= DAY_S || now < last) && hw_has_ipv4(NULL, 0)) {
			memset(&U.net, 0, sizeof(U.net));
			memset(&U.best, 0, sizeof(U.best));
			U.net_status[0] = 0;
			helper_start(ui, P_BGCHECK, args, true);
		}
	}
}

int update_timeout(const struct ui *ui)
{
	int t = -1;

	if (helper_running())
		return 200;
	if (!ui->loaded || !update_available(ui) || !ui_lists_complete(ui))
		return -1;
	if (U.wifi_until)
		return 1000;
	if (!U.boot_done)
		t = (int)MAX(0, MIN(U.next_boot - ui->now, BOOT_RETRY_MS));
	if (U.next_bg) {
		int b = (int)MAX(0, MIN(U.next_bg - ui->now, BG_RETRY_MS));

		t = t < 0 ? b : MIN(t, b);
	}
	return t;
}

/* ------------------------------------------------------ the menu */
static void menu_refresh(struct ui *ui, struct menu *m)
{
	struct menu_item *it;
	const struct found *f = U.best.valid ? &U.best : U.net.valid && !strcmp(U.net.status, "found") ? &U.net : NULL;

	if ((it = menu_find(m, ID_U_VERSION))) {
		if (U.version[0])
			strlcpy_(it->value, U.version, sizeof(it->value));
		else
			hw_version(ui->cfg.version, it->value, sizeof(it->value));
	}
	if ((it = menu_find(m, ID_U_CHECK)))
		strlcpy_(it->value, helper_running() && P.kind != P_BGCHECK ? _("Checking...") : U.status,
			 sizeof(it->value));
	if ((it = menu_find(m, ID_U_INSTALL))) {
		it->disabled = !f;
		if (f)
			/* TRANSLATORS: Settings > System update: the found update; %s is its version */
			fmt(it->label, sizeof(it->label), _("Install RetroStoneOS %s"), f->version);
		else
			/* TRANSLATORS: Settings > System update, before an update was found */
			strlcpy_(it->label, _("Install an update"), sizeof(it->label));
	}
	if ((it = menu_find(m, ID_U_NOAB)))
		it->disabled = true;
}

static void menu_item_fn(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	const struct found *f = U.best.valid ? &U.best : U.net.valid && !strcmp(U.net.status, "found") ? &U.net : NULL;

	switch (it->id) {
	case ID_U_CHECK:
		if (!dir)
			check_start(ui, NULL);
		break;
	case ID_U_INSTALL:
		if (!dir && f)
			notes_open(ui, f);
		break;
	case ID_U_AUTO:
		save_key(ui, "update_auto", it->on ? "1" : "0");
		break;
	case ID_U_NOAB:
		if (!dir)
			message_open(ui, reason_text("noab"));
		break;
	}
	menu_refresh(ui, m);
}

static void open_menu(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("System update"), K_UPDATE);
	struct menu_item *it;

	/* TRANSLATORS: Settings > System update: the version installed now */
	menu_add(m, MI_INFO, ID_U_VERSION, _("Installed version"));
	/* TRANSLATORS: Settings > System update action */
	menu_add(m, MI_ACTION, ID_U_CHECK, _("Check for updates"));
	menu_add(m, MI_ACTION, ID_U_INSTALL, _("Install an update"));
	/* TRANSLATORS: Settings > System update toggle: a daily check when WiFi is connected */
	it = menu_add(m, MI_TOGGLE, ID_U_AUTO, _("Check every day (WiFi)"));
	it->on = settings_get_bool(ui->settings, "update_auto", true);
	if (U.have_info && !U.ab)
		/* TRANSLATORS: Settings > System update, boards without in-place updates (Raspberry Pi...) */
		menu_add(m, MI_INFO, ID_U_NOAB, _("Updates: flash the new image to the SD card"));
	m->on_item = menu_item_fn;
	m->on_refresh = menu_refresh;
	m->refresh_every = 300;
	menu_refresh(ui, m);
	menu_open(ui, m);
}

void update_open(struct ui *ui)
{
	if (!U.have_info && !helper_running()) {
		const char *args[] = { "info", NULL };

		helper_start(ui, P_INFO, args, false);
	}
	open_menu(ui);
}

/* ------------------------------------------------------ USB drives */
static bool dir_has_rsu(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	bool found = false;

	if (!d)
		return false;
	while (!found && (de = readdir(d))) {
		size_t l = strlen(de->d_name);

		found = de->d_name[0] != '.' && l > 4 && !strcasecmp(de->d_name + l - 4, ".rsu");
	}
	closedir(d);
	return found;
}

bool update_usb_has_package(const char *mountpoint)
{
	char p[512];

	if (dir_has_rsu(mountpoint))
		return true;
	snprintf(p, sizeof(p), "%s/RetroStoneOS", mountpoint);
	return dir_has_rsu(p);
}

void update_open_usb(struct ui *ui, const char *mountpoint)
{
	if (!update_available(ui)) {
		message_open(ui, reason_text("internal"));
		return;
	}
	strlcpy_(U.usb_dir, mountpoint, sizeof(U.usb_dir));
	open_menu(ui);                    /* the check prints the info line too */
	check_start(ui, U.usb_dir);
}
