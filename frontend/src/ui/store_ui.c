/*
 * store_ui.c - Settings > Game store (docs/store-design.md, docs/store-ports.md): the games the signed
 * catalog offers, grouped by system. A opens a game: its description, author, licence and size, with INSTALL
 * (or UPDATE) and REMOVE. With several game drives, INSTALL asks where.
 *
 * A port (a game made from the player's own ROM) first looks for that ROM (rsos-store find-rom): found,
 * INSTALL downloads the package and prepares the game data from the ROM; not found, the page says which game
 * and where to put it, and LOOK AGAIN searches again. Preparing can take minutes and nearly all the memory, so
 * the page asks first, wants the charger or a charged battery, and the idle power-off waits (ui_store_busy).
 * Leaving the screen while a port installs cancels it (the helper cleans up what it made).
 *
 * All the work is done by the rsos-store helper (ui->cfg.store_helper), run as a helper job (hw_job_start)
 * that writes its lines into a file; it never blocks the menu:
 *   rsos-store --out F list
 *   rsos-store --out F [--roms D] [--scan MOUNT]... find-rom ID
 *   rsos-store --out F [--roms D] [--rom FILE] get ID | install-port ID | remove ID
 * One job at a time.
 */
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui_internal.h"

enum { K_STORE = 150 };
enum { ID_S_NOTE = 1, ID_S_E0 = 100 };
enum { J_NONE = 0, J_LIST, J_GET, J_REMOVE, J_FIND, J_PORT };
enum { A_INSTALL = 1, A_REMOVE, A_LOOK };

#define SE_MAX 96
#define POLL_MS 600
#define BATTERY_MIN 40            /* percent, without a charger, to prepare a port */

struct sentry {
	char id[48], name[96], system[32], version[32], author[64], license[64], desc[480], kind[8];
	char installed[32], rom[96], mode[8];
	long long size;
	int minutes;
	bool update;
};

static struct {
	struct menu *m;
	struct sentry e[SE_MAX];
	int n;
	char path[96];
	int job_kind, job;
	int cur;                        /* the entry the job or dialog is about */
	bool loaded, failed;
	char root[1100];                /* the drive's root for the install ("" = the system's) */
	int dlg_act[3];
	int ntargets;
	const char *troots[4];
	const char *tlabels[4];
	int pct;                        /* the download or the preparing, -1 unknown */
	char phase[16];
	char rom_path[1100];            /* the ROM a find-rom found for the entry in T.cur ("" = none) */
	bool port_going;                /* a port install runs (also after the screen is closed) */
} T;

bool store_available(const struct ui *ui)
{
	return ui->cfg.store_helper && *ui->cfg.store_helper && access(ui->cfg.store_helper, X_OK) == 0;
}

/* A port is being installed or a game downloaded (no idle power-off). */
bool ui_store_busy(const struct ui *ui)
{
	(void)ui;
	return T.job && (T.job_kind == J_PORT || T.job_kind == J_GET);
}

/* ------------------------------------------------------------ parsing */
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

/* "type<TAB>k=v<TAB>k=v" */
struct fields {
	char *type;
	int n;
	char *k[16], *v[16];
};

static void split(char *line, struct fields *f)
{
	char *p = line;

	f->n = 0;
	f->type = p;
	p = strchr(p, '\t');
	if (p)
		*p++ = 0;
	while (p && f->n < 16) {
		char *next = strchr(p, '\t'), *eq;

		if (next)
			*next++ = 0;
		eq = strchr(p, '=');
		if (eq) {
			*eq++ = 0;
			unescape(eq);
			f->k[f->n] = p;
			f->v[f->n++] = eq;
		}
		p = next;
	}
}

static const char *fget(const struct fields *f, const char *key)
{
	for (int i = 0; i < f->n; i++)
		if (!strcmp(f->k[i], key))
			return f->v[i];
	return "";
}

/* the next line of a buffer being cut up in place */
static bool next_line(char **save, char **line)
{
	char *nl;

	if (!*save || !**save)
		return false;
	*line = *save;
	nl = strchr(*save, '\n');
	if (nl) {
		*nl = 0;
		*save = nl + 1;
	} else {
		*save += strlen(*save);
	}
	return true;
}

/* The last "ok" or "error" line of an output file, as fields (the buffer is cut up). */
static bool read_result(char *buf, struct fields *f, char *errtext, size_t en)
{
	char *save = buf, *l;
	bool have = false;

	while (next_line(&save, &l)) {
		if (!strncmp(l, "ok", 2) || !strncmp(l, "error", 5)) {
			split(l, f);
			have = true;
			if (en)
				strlcpy_(errtext, fget(f, "text"), en);
		}
	}
	return have;
}

static void load_list(const char *buf_in)
{
	char *copy = strdup(buf_in), *save, *l;

	T.n = 0;
	if (!copy)
		return;
	save = copy;
	while (next_line(&save, &l) && T.n < SE_MAX) {
		struct fields f;
		struct sentry *e = &T.e[T.n];

		if (strncmp(l, "entry\t", 6))
			continue;
		split(l, &f);
		memset(e, 0, sizeof(*e));
		strlcpy_(e->id, fget(&f, "id"), sizeof(e->id));
		strlcpy_(e->name, fget(&f, "name"), sizeof(e->name));
		strlcpy_(e->system, fget(&f, "system"), sizeof(e->system));
		strlcpy_(e->version, fget(&f, "version"), sizeof(e->version));
		strlcpy_(e->author, fget(&f, "author"), sizeof(e->author));
		strlcpy_(e->license, fget(&f, "license"), sizeof(e->license));
		strlcpy_(e->desc, fget(&f, "desc"), sizeof(e->desc));
		strlcpy_(e->kind, fget(&f, "kind"), sizeof(e->kind));
		strlcpy_(e->installed, fget(&f, "installed"), sizeof(e->installed));
		strlcpy_(e->rom, fget(&f, "rom"), sizeof(e->rom));
		strlcpy_(e->mode, fget(&f, "mode"), sizeof(e->mode));
		e->minutes = atoi(fget(&f, "minutes"));
		e->size = atoll(fget(&f, "size"));
		e->update = !strcmp(fget(&f, "update"), "1");
		if (e->id[0] && e->name[0])
			T.n++;
	}
	free(copy);
}

static bool is_port(const struct sentry *e)
{
	return !strcmp(e->kind, "port");
}

/* ------------------------------------------------------------ screen */
static const char *system_title(struct ui *ui, const char *sys)
{
	if (!strcmp(sys, "native"))
		/* TRANSLATORS: Game store: the heading of the native games (the Ports system) */
		return _("Ports");
	for (int i = 0; i < ui->nsys; i++)
		if (!ui->sys[i].is_collection && !strcmp(ui->sys[i].name, sys) && ui->sys[i].fullname[0])
			return ui->sys[i].fullname;
	return sys;
}

/* the value right of the game a job is working on */
static void busy_value(struct menu_item *it)
{
	if (T.job_kind == J_FIND) {
		/* TRANSLATORS: Game store: looking for the player's ROM of a port */
		strlcpy_(it->value, _("looking for your ROM..."), sizeof(it->value));
	} else if (!strcmp(T.phase, "extract")) {
		if (T.pct >= 0)
			/* TRANSLATORS: Game store: preparing a port's game data from the ROM; %d is the percent */
			snprintf(it->value, sizeof(it->value), _("preparing %d%%"), T.pct);
		else
			strlcpy_(it->value, _("preparing..."), sizeof(it->value));
	} else if (!strcmp(T.phase, "download") && T.pct >= 0) {
		/* TRANSLATORS: Game store: %d is the percent downloaded */
		snprintf(it->value, sizeof(it->value), _("%d%%"), T.pct);
	} else if (!strcmp(T.phase, "install") || !strcmp(T.phase, "verify")) {
		/* TRANSLATORS: Game store: the downloaded game is checked and put in place */
		strlcpy_(it->value, _("installing..."), sizeof(it->value));
	} else {
		/* TRANSLATORS: Game store: a game is being installed or removed */
		strlcpy_(it->value, _("please wait..."), sizeof(it->value));
	}
}

static void items(struct ui *ui, struct menu *m)
{
	struct menu_item *it;
	char seen[SE_MAX][32];
	int nseen = 0;

	m->n = 0;
	if (T.job_kind == J_LIST) {
		/* TRANSLATORS: Game store: the catalog is being loaded */
		it = menu_add(m, MI_INFO, ID_S_NOTE, _("Loading the catalog..."));
		it->disabled = true;
		return;
	}
	if (T.failed || !T.n) {
		/* TRANSLATORS: Game store: the catalog could not be loaded (no network, or nothing in it) */
		it = menu_add(m, MI_INFO, ID_S_NOTE,
			      T.failed ? _("Could not load the catalog. Check the network (WiFi or Ethernet).") : _("The store is empty"));
		it->disabled = true;
		return;
	}
	for (int i = 0; i < T.n; i++) {
		bool done = false;

		for (int k = 0; k < nseen; k++)
			done |= !strcmp(seen[k], T.e[i].system);
		if (done)
			continue;
		strlcpy_(seen[nseen++], T.e[i].system, sizeof(seen[0]));
		menu_add(m, MI_HEADER, 0, system_title(ui, T.e[i].system));
		for (int j = 0; j < T.n; j++) {
			const struct sentry *e = &T.e[j];
			char sz[32];

			if (strcmp(e->system, T.e[i].system))
				continue;
			it = menu_add(m, MI_ACTION, ID_S_E0 + j, e->name);
			if (T.job_kind && T.cur == j) {
				busy_value(it);
			} else if (e->installed[0]) {
				/* TRANSLATORS: Game store: right of a game: installed / a newer version exists */
				strlcpy_(it->value, e->update ? _("update") : _("installed"), sizeof(it->value));
			} else if (is_port(e)) {
				/* TRANSLATORS: Game store: right of a port that needs the player's own ROM */
				strlcpy_(it->value, _("needs your ROM"), sizeof(it->value));
			} else {
				format_bytes((uint64_t)e->size, sz, sizeof(sz));
				strlcpy_(it->value, sz, sizeof(it->value));
			}
		}
	}
}

static void rebuild(struct ui *ui, struct menu *m)
{
	int cur_id = m->cursor >= 0 && m->cursor < m->n ? m->items[m->cursor].id : 0;

	items(ui, m);
	m->cursor = 0;
	for (int i = 0; i < m->n; i++)
		if (m->items[i].id == cur_id)
			m->cursor = i;
	for (int k = 0; k < m->n && (m->items[m->cursor].type == MI_HEADER || m->items[m->cursor].disabled); k++)
		m->cursor = (m->cursor + 1) % m->n;
	m->top = MIN(m->top, m->cursor);
	ui->dirty = true;
}

/* ------------------------------------------------------------- jobs */
/* the drives a USB stick may be mounted on: ROMs are looked for there too */
static int usb_mounts(char out[3][300])
{
	DIR *d = opendir("/media");
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)) && n < 3) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(out[n++], 300, "/media/%s", de->d_name);
	}
	closedir(d);
	return n;
}

static int run_helper(struct ui *ui, const char *cmd, const char *arg, const char *roms, const char *rom, int tag)
{
	const char *argv[24];
	char mounts[3][300];
	int n = 0, nm = 0;

	argv[n++] = ui->cfg.store_helper;
	argv[n++] = "--out";
	argv[n++] = T.path;
	if (roms && *roms) {
		argv[n++] = "--roms";
		argv[n++] = roms;
	}
	if (rom && *rom) {
		argv[n++] = "--rom";
		argv[n++] = rom;
	}
	if (!strcmp(cmd, "find-rom") || !strcmp(cmd, "install-port"))
		for (nm = usb_mounts(mounts); nm > 0; nm--) {
			argv[n++] = "--scan";
			argv[n++] = mounts[nm - 1];
		}
	argv[n++] = cmd;
	if (arg)
		argv[n++] = arg;
	argv[n] = NULL;
	unlink(T.path);
	return hw_job_start(argv, tag);
}

static void roms_of(struct ui *ui, const char *root, char *out, size_t n)
{
	if (root && *root)
		snprintf(out, n, "%s/roms", root);
	else
		snprintf(out, n, "%s", ui->cfg.roms_dir ? ui->cfg.roms_dir : "/data/roms");
}

static void start_list(struct ui *ui)
{
	int id;

	if (T.job)
		return;
	id = run_helper(ui, "list", NULL, NULL, NULL, UI_JOB_STORE);
	if (id < 0) {
		T.failed = true;
		return;
	}
	T.job = id;
	T.job_kind = J_LIST;
	T.failed = false;
}

static void start_action(struct ui *ui, int kind, int idx, const char *root, const char *rom)
{
	char roms[1200];
	const char *cmd = kind == J_GET ? "get" : kind == J_PORT ? "install-port" : "remove";
	int id;

	if (T.job)
		return;
	roms_of(ui, root, roms, sizeof(roms));
	id = run_helper(ui, cmd, T.e[idx].id, roms, rom, UI_JOB_STORE);
	if (id < 0) {
		ui_toastf(ui, _("Cannot run %s"), ui->cfg.store_helper);
		return;
	}
	T.job = id;
	T.job_kind = kind;
	T.cur = idx;
	T.pct = -1;
	T.phase[0] = 0;
	if (T.m)
		rebuild(ui, T.m);
}

static void start_find(struct ui *ui, int idx)
{
	char roms[1200];
	int id;

	if (T.job)
		return;
	roms_of(ui, NULL, roms, sizeof(roms));
	id = run_helper(ui, "find-rom", T.e[idx].id, roms, NULL, UI_JOB_STORE);
	if (id < 0) {
		ui_toastf(ui, _("Cannot run %s"), ui->cfg.store_helper);
		return;
	}
	T.job = id;
	T.job_kind = J_FIND;
	T.cur = idx;
	T.rom_path[0] = 0;
	T.phase[0] = 0;
	T.pct = -1;
	if (T.m)
		rebuild(ui, T.m);
}

/* the last progress line of the running download or install */
static void read_progress(struct ui *ui)
{
	size_t len = 0;
	char *buf = file_read(T.path, &len), *p;
	int pct = T.pct;
	char phase[16];

	if (!buf)
		return;
	strlcpy_(phase, T.phase, sizeof(phase));
	p = strstr(buf, "progress");
	while (p) {
		char *nl = strchr(p, '\n'), *next = nl ? strstr(nl, "progress") : NULL;

		if (!next) {
			struct fields f;
			char line[256];
			long long done, total;

			strlcpy_(line, p, sizeof(line));
			line[strcspn(line, "\n")] = 0;
			split(line, &f);
			strlcpy_(phase, fget(&f, "phase"), sizeof(phase));
			done = atoll(fget(&f, "done"));
			total = atoll(fget(&f, "total"));
			if ((!strcmp(phase, "download") || !strcmp(phase, "extract")) && total > 0) {
				pct = (int)(done * 100 / total);
				if (pct > 99)
					pct = 99;
			} else {
				pct = -1;
			}
			break;
		}
		p = next;
	}
	free(buf);
	if (pct != T.pct || strcmp(phase, T.phase)) {
		T.pct = pct;
		strlcpy_(T.phase, phase, sizeof(T.phase));
		if (T.m)
			rebuild(ui, T.m);
	}
}

static void refresh(struct ui *ui, struct menu *m)
{
	(void)m;
	if (T.job_kind == J_GET || T.job_kind == J_PORT)
		read_progress(ui);
}

/* a helper error code -> the player's words */
static void error_message(struct ui *ui, const char *code, const char *text)
{
	if (!strcmp(code, "space"))
		/* TRANSLATORS: Game store: not enough free space on the drive */
		message_open(ui, _("There is not enough free space on that drive."));
	else if (!strcmp(code, "exists"))
		/* TRANSLATORS: Game store: a file with that name is already there and was not installed by the store */
		message_open(ui, _("A file with the same name is already there. Rename or move it first."));
	else if (!strcmp(code, "hash") || !strcmp(code, "size"))
		/* TRANSLATORS: Game store: the download is damaged or has been changed */
		message_open(ui, _("The download did not match the catalog and was thrown away."));
	else if (!strcmp(code, "archive"))
		/* TRANSLATORS: Game store: the package is not safe to unpack */
		message_open(ui, _("The package is not valid and was not installed."));
	else if (!strcmp(code, "memory"))
		/* TRANSLATORS: Game store: preparing a port needs nearly all the memory */
		message_open(ui, _("Preparing the game needs more free memory. Restart the console and try again "
				   "before starting anything else."));
	else if (!strcmp(code, "timeout"))
		/* TRANSLATORS: Game store: preparing the game data took far too long and was stopped */
		message_open(ui, _("Preparing the game data took too long and was stopped. Nothing was installed."));
	else if (!strcmp(code, "extract") || !strcmp(code, "rom"))
		/* TRANSLATORS: Game store: the game data could not be made from the ROM */
		message_open(ui, _("The game data could not be made from your ROM. Nothing was installed."));
	else if (!strcmp(code, "norom"))
		/* TRANSLATORS: Game store: the ROM is not the one the port needs */
		message_open(ui, _("That is not the ROM this game needs."));
	else if (!strcmp(code, "cancelled"))
		return;
	else
		/* TRANSLATORS: Game store: the download failed (network) */
		message_open(ui, _("The game could not be downloaded. Check the network (WiFi or Ethernet) and try again."));
	LOGW("store: %s: %s", code, text);
}

static void port_dialog(struct ui *ui);

bool store_job_done(struct ui *ui, const struct hw_job_result *r)
{
	size_t len = 0;
	char *buf;
	int kind = T.job_kind, cur = T.cur;

	if (r->tag != UI_JOB_STORE)
		return false;
	T.job = 0;
	T.job_kind = J_NONE;
	buf = file_read(T.path, &len);
	if (!buf)
		buf = strdup("");
	if (kind == J_LIST) {
		load_list(buf);
		T.loaded = true;
		T.failed = r->status != 0 || !strstr(buf, "\ndone") ? (strstr(buf, "entry\t") == NULL) : false;
		if (T.failed)
			LOGW("store: list: %s", buf);
	} else if (kind == J_FIND) {
		char *copy = strdup(buf), *save = copy, *l;

		T.rom_path[0] = 0;
		while (copy && next_line(&save, &l)) {
			if (!strncmp(l, "rom\t", 4)) {
				struct fields f;

				split(l, &f);
				strlcpy_(T.rom_path, fget(&f, "path"), sizeof(T.rom_path));
			}
		}
		free(copy);
		if (T.m)
			rebuild(ui, T.m);
		free(buf);
		port_dialog(ui);
		return true;
	} else if (cur >= 0 && cur < T.n) {
		struct fields f;
		char text[256] = "";
		bool have = buf && read_result(buf, &f, text, sizeof(text));

		if (have && !strcmp(f.type, "ok")) {
			struct sentry *e = &T.e[cur];

			if (kind == J_GET || kind == J_PORT) {
				strlcpy_(e->installed, e->version, sizeof(e->installed));
				e->update = false;
				/* TRANSLATORS: Game store: toast after an install; %s is the game's name */
				ui_toastf(ui, _("%s installed"), e->name);
			} else {
				e->installed[0] = 0;
				e->update = false;
				/* TRANSLATORS: Game store: toast after a removal; %s is the game's name */
				ui_toastf(ui, _("%s removed"), e->name);
			}
			ui_invalidate_system(ui, !strcmp(e->system, "native") ? "native" : e->system);
			ui->reload_pending = true;
		} else if (have) {
			error_message(ui, fget(&f, "code"), text);
		} else {
			error_message(ui, "download", r->out);
		}
	}
	free(buf);
	if (T.m)
		rebuild(ui, T.m);
	ui->dirty = true;
	return true;
}

/* ----------------------------------------------------------- dialogs */
static void where_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice < 0 || choice >= T.ntargets)
		return;
	start_action(ui, J_GET, T.cur, T.troots[choice], NULL);
}

/* a port's install: the charger or a charged battery first, then the question, then the job */
static void port_start(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0)
		start_action(ui, J_PORT, T.cur, NULL, T.rom_path);
}

static void port_install(struct ui *ui)
{
	const struct sentry *e = &T.e[T.cur];
	const struct power_status *ps = ui_battery(ui);
	const char *buttons[3];
	char text[500];

	if (!strcmp(e->mode, "extract") && ps && ps->valid && ps->battery_present && !ps->charger_online &&
	    ps->percent >= 0 && ps->percent < BATTERY_MIN) {
		/* TRANSLATORS: Game store: preparing a port takes minutes; plug in the charger first; %d is the percent */
		ui_toastf(ui, _("Plug in the charger first (battery %d%%)"), ps->percent);
		return;
	}
	if (strcmp(e->mode, "extract")) {
		start_action(ui, J_PORT, T.cur, NULL, T.rom_path);
		return;
	}
	/* TRANSLATORS: Game store: before preparing a port's game data from the player's ROM; %d is the minutes */
	snprintf(text, sizeof(text), _("Preparing the game data from your ROM takes about %d minutes. Keep the "
				       "console on and nothing else running."), e->minutes > 0 ? e->minutes : 5);
	/* TRANSLATORS: dialog buttons, uppercase, short */
	buttons[0] = _("START");
	buttons[1] = _("CANCEL");
	buttons[2] = NULL;
	dialog_open(ui, text, buttons, port_start, NULL);
}

static void entry_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice < 0 || choice >= 3 || !T.dlg_act[choice])
		return;
	if (T.dlg_act[choice] == A_REMOVE) {
		start_action(ui, J_REMOVE, T.cur, NULL, NULL);
		return;
	}
	if (T.dlg_act[choice] == A_LOOK) {
		start_find(ui, T.cur);
		return;
	}
	if (is_port(&T.e[T.cur])) {
		port_install(ui);
		return;
	}
	T.ntargets = storage_import_targets(ui, T.troots, T.tlabels, 4);
	if (T.ntargets > 1 && !T.e[T.cur].installed[0]) {
		const char *buttons[6];
		int n = 0;

		for (int i = 0; i < T.ntargets; i++)
			buttons[n++] = T.tlabels[i];
		buttons[n] = NULL;
		/* TRANSLATORS: Game store: the question when several drives could hold the game */
		dialog_open(ui, _("Install on which drive?"), buttons, where_choice, NULL);
		return;
	}
	start_action(ui, J_GET, T.cur, NULL, NULL);
}

/* a game's page */
static void page(struct ui *ui, const char *extra)
{
	const struct sentry *e = &T.e[T.cur];
	const char *buttons[4];
	char text[1200], sz[32];
	int n = 0;

	format_bytes((uint64_t)e->size, sz, sizeof(sz));
	/* TRANSLATORS: Game store: a game's page; %1$s name, %2$s version, %3$s author, %4$s licence,
	 * %5$s download size, %6$s the description */
	snprintf(text, sizeof(text), _("%1$s %2$s\nby %3$s - %4$s\n%5$s\n\n%6$s"), e->name, e->version, e->author,
		 e->license, sz, e->desc);
	if (extra && *extra) {
		size_t l = strlen(text);

		snprintf(text + l, sizeof(text) - l, "\n\n%s", extra);
	}
	memset(T.dlg_act, 0, sizeof(T.dlg_act));
	if (is_port(e) && T.rom_path[0] == 0 && (!e->installed[0] || e->update)) {
		/* TRANSLATORS: Game store dialog button: search for the ROM again */
		buttons[n] = _("LOOK AGAIN");
		T.dlg_act[n++] = A_LOOK;
	} else if (!e->installed[0] || e->update) {
		/* TRANSLATORS: Game store dialog buttons, uppercase, short */
		buttons[n] = e->update ? _("UPDATE") : _("INSTALL");
		T.dlg_act[n++] = A_INSTALL;
	}
	if (e->installed[0]) {
		buttons[n] = _("REMOVE");
		T.dlg_act[n++] = A_REMOVE;
	}
	buttons[n++] = _("CANCEL");
	buttons[n] = NULL;
	dialog_open(ui, text, buttons, entry_choice, NULL);
}

/* after the ROM search: the page with what was found */
static void port_dialog(struct ui *ui)
{
	const struct sentry *e = &T.e[T.cur];
	char extra[400];

	if (T.rom_path[0]) {
		const char *base = strrchr(T.rom_path, '/');

		/* TRANSLATORS: Game store: a port's page, the ROM found; %s is the file name */
		snprintf(extra, sizeof(extra), _("Your ROM: %s"), base ? base + 1 : T.rom_path);
	} else {
		/* TRANSLATORS: Game store: a port's page, no ROM found; %s is the game and version it needs */
		snprintf(extra, sizeof(extra), _("Needs your own ROM: %s. Put it in your N64 folder or on a USB "
						 "drive, then choose LOOK AGAIN."), e->rom);
	}
	page(ui, extra);
}

static void item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	(void)m;
	if (dir || it->id < ID_S_E0 || it->id >= ID_S_E0 + T.n)
		return;
	if (T.job) {
		ui_toastf(ui, "%s", _("please wait..."));
		return;
	}
	T.cur = it->id - ID_S_E0;
	T.rom_path[0] = 0;
	if (is_port(&T.e[T.cur]) && (!T.e[T.cur].installed[0] || T.e[T.cur].update)) {
		start_find(ui, T.cur);
		return;
	}
	page(ui, NULL);
}

static void destroy(struct ui *ui, struct menu *m)
{
	(void)ui;
	(void)m;
	T.m = NULL;
	/* leaving while a port installs cancels it: the helper stops its extractor and cleans up */
	if (T.job && T.job_kind == J_PORT)
		hw_job_kill(T.job, SIGTERM);
}

void store_open(struct ui *ui)
{
	/* TRANSLATORS: Settings: the game store (homebrew and freeware downloads) */
	struct menu *m = menu_new(ui, _("Game store"), K_STORE);

	snprintf(T.path, sizeof(T.path), "/tmp/rsos-store-%d.out", (int)getpid());
	T.m = m;
	m->on_item = item;
	m->on_refresh = refresh;
	m->on_destroy = destroy;
	m->refresh_every = POLL_MS;
	if (!T.job)
		start_list(ui);
	items(ui, m);
	menu_open(ui, m);
}
