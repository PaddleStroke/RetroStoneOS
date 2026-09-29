/*
 * transfer_ui.c - screens of the transfer module (docs/rom-transfer.md §4):
 * the "USB drive connected" dialog (Import games / Export games / Back up
 * saves / Nothing), Import (library search, picker, plan, progress with the
 * Skip / Replace / Skip all / Replace all prompt), Export games and Back up
 * saves (options, progress, summary), and Transfer over network (web share
 * with URL, PIN and QR code). Everything goes through cfg.transfer (struct
 * ui_transfer_api); the module runs its work in threads and the UI only
 * polls status snapshots.
 *
 * Every text shown goes through i18n (_(), docs/translating.md); log lines,
 * paths, volume labels and the describe() test prefixes stay English. The
 * transfer module is English-only: its fixed messages are translated here,
 * at display time (the lists below).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ui_internal.h"
#include "../i18n/i18n.h"

#define TR(ui) ((ui)->cfg.transfer)

/*
 * For the string extractor only. System names of the transfer module
 * (transfer_system_name()): they must match src/transfer/sysmap.c. Shown
 * with C_("system", name), like the UI's own system names.
 */
static const char *const g_sysmap_names[] __attribute__((unused)) = {
	NC_("system", "NES / Famicom"),
	NC_("system", "Famicom Disk System"),
	NC_("system", "Super Nintendo"),
	NC_("system", "Nintendo 64"),
	NC_("system", "Game Boy"),
	NC_("system", "Game Boy Color"),
	NC_("system", "Game Boy Advance"),
	NC_("system", "SG-1000"),
	NC_("system", "Master System"),
	NC_("system", "Mega Drive / Genesis"),
	NC_("system", "Mega-CD / Sega CD"),
	NC_("system", "32X"),
	NC_("system", "Game Gear"),
	NC_("system", "Sega Pico"),
	NC_("system", "PlayStation"),
	NC_("system", "PC Engine"),
	NC_("system", "Atari 2600"),
	NC_("system", "Arcade (MAME 2003-Plus)"),
	NC_("system", "Arcade (FinalBurn Neo)"),
	NC_("system", "Neo Geo"),
	NC_("system", "SuperGrafx"),
	NC_("system", "Pokémon mini"),
	NC_("system", "Commodore 64"),
	NC_("system", "ZX Spectrum"),
	NC_("system", "Amstrad CPC"),
	NC_("system", "MS-DOS"),
	NC_("system", "ScummVM"),
	NC_("system", "PICO-8"),
	NC_("system", "Doom"),
	NC_("system", "Cave Story"),
};

/*
 * For the string extractor only: the fixed English texts the transfer
 * module puts in struct transfer_progress.errmsg (they must match
 * src/transfer/import.c and backup.c), shown with _(errmsg). Others
 * ("<file>: <strerror>") are shown as they are.
 */
static const char *const g_module_errors[] __attribute__((unused)) = {
	/* TRANSLATORS: why an import stopped ("The copy stopped: ..."). */
	N_("The SD card is full"),
	N_("The USB drive was removed"),
	N_("Read error: was the USB drive removed?"),
	/* TRANSLATORS: why an export / saves backup stopped ("Stopped: ..."). */
	N_("The USB drive is full"),
	N_("The USB drive is read-only"),
	N_("Write error: was the USB drive removed?"),
	/* TRANSLATORS: why the network transfer stopped by itself ("Stopped: idle"):
	 * nothing was received for a while (src/transfer/webshare.c). */
	NC_("webshare stop reason", "idle"),
};

/* s was cut by a snprintf/strlcpy: drop an incomplete trailing UTF-8
 * sequence (translations are multibyte). */
static void utf8_fix(char *s)
{
	size_t len = strlen(s), i = len, need;
	unsigned char c;

	while (i > 0 && len - i < 4 && ((unsigned char)s[i - 1] & 0xc0) == 0x80)
		i--;
	if (i == 0)
		return;
	c = (unsigned char)s[i - 1];
	if (c < 0x80)
		return;                               /* ASCII: complete */
	need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
	if (len - (i - 1) < need)
		s[i - 1] = 0;
}

/* snprintf that never leaves half a UTF-8 character at the end. */
static void __attribute__((format(printf, 3, 4))) tfmt(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(out, n, fmt, ap);
	va_end(ap);
	utf8_fix(out);
}

static void tcpy(char *out, const char *s, size_t n)
{
	strlcpy_(out, s, n);
	utf8_fix(out);
}

/* menu_add() with a label cut on a character boundary. */
static struct menu_item *add(struct menu *m, enum mi_type t, int id, const char *label)
{
	char l[sizeof(((struct menu_item *)0)->label)];

	tcpy(l, label, sizeof(l));
	return menu_add(m, t, id, l);
}

/* A size in the language's units ("1,5 Go"). */
#define SIZE(b, buf) i18n_format_size((b), (buf), sizeof(buf))

enum { K_IMPORT = 100, K_DRIVES, K_BACKUP };
enum {
	ID_I_ROMS = 1, ID_I_BIOS, ID_I_SAVES, ID_I_THEMES, ID_I_SHOTS, ID_I_COPY, ID_I_EJECT,
	ID_B_BIOS, ID_B_SETTINGS, ID_B_GAME, ID_B_START,
	ID_TREE0 = 30, ID_TREE_ALL = 60,
	ID_DRIVE0 = 70,
};
/* ui->scan_orphan: scans whose screen was closed, freed when they end */
enum { ORPHAN_PLAN = 1, ORPHAN_TREES = 2, ORPHAN_BACKUP = 4 };

static const char *data_root(struct ui *ui)
{
	return TR(ui) && TR(ui)->data_root ? TR(ui)->data_root : "/data";
}

static const char *sys_label(const char *id)
{
	const struct sysdef *sd = systems_find(id);

	if (sd)
		return C_("system", sd->fullname);
	if (!strcmp(id, "bios"))
		return _("BIOS files");
	if (!strcmp(id, "themes"))
		return _("Themes");
	return id;
}

/* The drive's name for log lines (never translated). */
static const char *drive_log(const struct transfer_usb_drive *d)
{
	return d->label[0] ? d->label : d->vendor[0] ? d->vendor : "USB drive";
}

/* The drive's name on screen: its volume label or vendor as they are. */
static const char *drive_name(const struct transfer_usb_drive *d)
{
	/* TRANSLATORS: a USB stick with no volume label and no vendor name */
	return d->label[0] || d->vendor[0] ? drive_log(d) : _("USB drive");
}

/* The mounted drive at mp (false if it is gone). */
static bool find_drive(struct ui *ui, const char *mp, struct transfer_usb_drive *out)
{
	struct transfer_usb_drive d[8];
	int n = TR(ui)->usb_drives(d, 8);

	for (int i = 0; i < n; i++)
		if (!strcmp(d[i].mountpoint, mp)) {
			*out = d[i];
			return true;
		}
	return false;
}

/* Changed systems -> scan caches dropped; the lists reload later. */
static void invalidate(struct ui *ui, char ids[][TRANSFER_SYSID_MAX], int n)
{
	for (int i = 0; i < n; i++) {
		if (ui->cfg.cache_dir[0])
			games_invalidate(ui->cfg.cache_dir, ids[i]);
		LOGI("transfer: %s changed", ids[i]);
	}
	if (n)
		ui->reload_pending = true;
}

static void reload_if_needed(struct ui *ui)
{
	if (ui->reload_pending) {
		ui->reload_pending = false;
		ui_reload_games(ui, false);
	}
}

/* The kind of the backup that runs (progress title): 1 export, 2 saves. */
static int g_backup_kind = 1;

/* What runs now: 1 an import, 2 an export or a saves backup, 0 nothing. */
static int job_running(struct ui *ui)
{
	struct transfer_progress pr;

	if (!TR(ui))
		return 0;
	if (TR(ui)->import_status(&pr) != TRANSFER_IDLE)
		return 1;
	if (TR(ui)->backup_status && TR(ui)->backup_status(&pr) != TRANSFER_IDLE)
		return 2;
	return 0;
}

static void progress_open(struct ui *ui, int kind, const char *mp, const char *label);

/* Only one copy at a time: its progress screen instead. */
static bool show_running_job(struct ui *ui, const char *mp, const char *label)
{
	struct transfer_progress pr;
	int j = job_running(ui);

	if (!j)
		return false;
	(void)pr;
	progress_open(ui, j == 1 ? 0 : g_backup_kind, mp, label);
	return true;
}

/* "Eject" in the summaries (one at a time). */
static char g_eject_mp[64];

static void eject_mp(struct ui *ui, const char *mp)
{
	int r = TR(ui)->usb_eject(mp);

	LOGI("usb: eject %s: %s", mp, r < 0 ? "busy" : "done");
	if (r < 0)
		/* TRANSLATORS: toast, short */
		ui_toastf(ui, "%s", _("The USB drive is busy"));
	else
		/* TRANSLATORS: toast */
		ui_toast_long(ui, "%s", _("USB drive ejected: you can unplug it"));
}

static void eject_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0 && TR(ui) && g_eject_mp[0])
		eject_mp(ui, g_eject_mp);
	g_eject_mp[0] = 0;
}

static void summary_open(struct ui *ui, const char *mp, const char *msg)
{
	/* TRANSLATORS: dialog button, uppercase, short */
	const char *const buttons[] = { _("EJECT"), _("OK"), NULL };

	strlcpy_(g_eject_mp, mp, sizeof(g_eject_mp));
	dialog_open(ui, msg, buttons, eject_choice, NULL);
}

static void pop_menus(struct ui *ui, int kind)
{
	struct screen *s;

	while ((s = ui_top(ui)) && screen_is_menu(s) && ((struct menu *)s)->kind == kind)
		ui_pop(ui);
}

/* ------------------------------------------------------------- import */
/*
 * One menu does it all: SEARCH (the library search runs) -> PICK (several
 * libraries: the picker; choosing one opens a new menu in PLAN_WAIT, so B
 * comes back here) or PLAN_WAIT (the plan is built) -> PLAN (what will be
 * copied, options, Copy).
 */
enum { IS_SEARCH = 0, IS_PICK, IS_PLAN_WAIT, IS_PLAN };

struct import_ctx {
	char mp[64];
	char label[64];
	int state;
	struct transfer_tree trees[TRANSFER_TREES_MAX];
	int ntrees;
	char roots[8][TRANSFER_PATH_MAX];
	int nroots;
	char what[192];              /* the chosen libraries, shown in "From" */
	char what_log[128];          /* the same in English, for the log */
	struct transfer_plan *plan;
	int err;
	struct transfer_import_opts o;
};

static void import_items(struct ui *ui, struct menu *m);

static void import_destroy(struct ui *ui, struct menu *m)
{
	struct import_ctx *c = m->ctx;

	if (c->state == IS_SEARCH)
		ui->scan_orphan |= ORPHAN_TREES;    /* transfer_poll frees it at the end */
	else if (c->state == IS_PLAN_WAIT)
		ui->scan_orphan |= ORPHAN_PLAN;
	else if (c->plan && TR(ui))
		TR(ui)->plan_free(c->plan);
	free(c);
}

static void start_plan(struct ui *ui, struct menu *m, struct import_ctx *c)
{
	const char *roots[8];
	int r;

	for (int i = 0; i < c->nroots; i++)
		roots[i] = c->roots[i];
	r = TR(ui)->plan_start_trees(roots, c->nroots, data_root(ui));
	if (r < 0) {
		c->err = r;
		c->state = IS_PLAN;
		m->refresh_every = 0;
		return;
	}
	c->state = IS_PLAN_WAIT;
	m->refresh_every = 150;
	m->next_refresh = ui->now + 150;
}

static void tree_label(const struct transfer_tree *t, char *out, size_t n)
{
	char rel[101];

	tcpy(rel, t->rel, sizeof(rel));
	if (!t->rel[0])
		/* TRANSLATORS: a game library found at the root of the USB stick */
		tcpy(out, _("Top of the drive"), n);
	else if (t->backup)
		/* TRANSLATORS: a folder name, then this: a RetroStone backup folder */
		tfmt(out, n, _("%s (backup)"), rel);
	else
		tcpy(out, t->rel, n);
}

/* The same for log lines. */
static void tree_log(const struct transfer_tree *t, char *out, size_t n)
{
	snprintf(out, n, "%.100s%s", t->rel[0] ? t->rel : "top of the drive", t->backup ? " (backup)" : "");
}

static void import_refresh(struct ui *ui, struct menu *m)
{
	struct import_ctx *c = m->ctx;
	int r, n = 0;

	if (c->state == IS_SEARCH) {
		r = TR(ui)->trees_poll(c->trees, TRANSFER_TREES_MAX, &n);
		if (r == 0)
			return;
		c->ntrees = r > 0 ? n : 0;
		LOGI("usb: %s: %d game librar%s found", c->label, c->ntrees, c->ntrees == 1 ? "y" : "ies");
		if (c->ntrees > 1) {
			c->state = IS_PICK;
			m->refresh_every = 0;
		} else {
			/* one library (or none: the drive's top, for BIOS files...) */
			strlcpy_(c->roots[0], c->ntrees ? c->trees[0].path : c->mp, sizeof(c->roots[0]));
			c->nroots = 1;
			if (c->ntrees) {
				tree_label(&c->trees[0], c->what, sizeof(c->what));
				tree_log(&c->trees[0], c->what_log, sizeof(c->what_log));
			}
			start_plan(ui, m, c);
		}
	} else if (c->state == IS_PLAN_WAIT) {
		r = TR(ui)->plan_poll(&c->plan);
		if (r == 0)
			return;
		c->state = IS_PLAN;
		m->refresh_every = 0;
		if (r < 0)
			c->err = r;
	} else {
		return;
	}
	import_items(ui, m);
	m->cursor = 0;
	for (int i = 0; i < m->n; i++)
		if (m->items[i].id == ID_I_COPY || m->items[i].id == ID_TREE_ALL)
			m->cursor = i;
	ui->dirty = true;
}

static void import_menu_open(struct ui *ui, struct import_ctx *c);

static void import_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	struct import_ctx *c = m->ctx;
	char mp[64], label[64];
	int r;

	if (it->id >= ID_TREE0 && it->id <= ID_TREE_ALL && !dir) {
		struct import_ctx *nc = xcalloc(1, sizeof(*nc));

		*nc = *c;
		nc->plan = NULL;
		nc->nroots = 0;
		if (it->id == ID_TREE_ALL) {
			for (int i = 0; i < c->ntrees && nc->nroots < 8; i++)
				strlcpy_(nc->roots[nc->nroots++], c->trees[i].path, sizeof(nc->roots[0]));
			/* TRANSLATORS: "From: all 3 libraries" (game libraries on the USB stick) */
			tfmt(nc->what, sizeof(nc->what), _n("all %d library", "all %d libraries", c->ntrees),
			     c->ntrees);
			snprintf(nc->what_log, sizeof(nc->what_log), "all %d libraries", c->ntrees);
		} else {
			const struct transfer_tree *t = &c->trees[it->id - ID_TREE0];

			strlcpy_(nc->roots[0], t->path, sizeof(nc->roots[0]));
			nc->nroots = 1;
			tree_label(t, nc->what, sizeof(nc->what));
			tree_log(t, nc->what_log, sizeof(nc->what_log));
		}
		LOGI("usb: import from %s: %s", c->label, nc->what_log);
		import_menu_open(ui, nc);
		return;
	}
	switch (it->id) {
	case ID_I_ROMS:
		c->o.roms = it->on;
		break;
	case ID_I_BIOS:
		c->o.bios = it->on;
		break;
	case ID_I_SAVES:
		c->o.saves = it->on;
		break;
	case ID_I_THEMES:
		c->o.themes = it->on;
		break;
	case ID_I_SHOTS:
		c->o.screenshots = it->on;
		break;
	case ID_I_COPY:
		if (dir || !c->plan)
			break;
		strlcpy_(mp, c->mp, sizeof(mp));
		strlcpy_(label, c->label, sizeof(label));
		r = TR(ui)->import_start(c->plan, &c->o);
		if (r < 0) {
			message_open(ui, r == -16 /* EBUSY */ ? _("Another copy is already running.") :
				     _("The copy could not start."));
			break;
		}
		LOGI("usb: import started from %s (%s)", label, c->what_log[0] ? c->what_log : "the drive");
		c->plan = NULL;  /* owned by the import now */
		pop_menus(ui, K_IMPORT);   /* the plan and the picker; frees c */
		progress_open(ui, 0, mp, label);
		return;
	case ID_I_EJECT:
		if (dir)
			break;
		strlcpy_(mp, c->mp, sizeof(mp));
		pop_menus(ui, K_IMPORT);
		eject_mp(ui, mp);
		return;
	}
}

static void picker_items(struct menu *m, struct import_ctx *c)
{
	struct menu_item *it;
	char a[48], l[128], ns[64], ng[64];
	int games = 0, sys = 0;
	uint64_t bytes = 0;

	/* TRANSLATORS: menu section header: the game libraries found on the USB stick */
	add(m, MI_HEADER, 0, _("Game libraries on the drive"));
	for (int i = 0; i < c->ntrees; i++) {
		const struct transfer_tree *t = &c->trees[i];

		tree_label(t, l, sizeof(l));
		it = add(m, MI_ACTION, ID_TREE0 + i, l);
		SIZE(t->bytes, a);
		if (t->games) {
			/* TRANSLATORS: part of "3 systems, 120 games, 1.5 GB" */
			tfmt(ns, sizeof(ns), _n("%d system", "%d systems", t->nsys), t->nsys);
			/* TRANSLATORS: part of "3 systems, 120 games, 1.5 GB" */
			tfmt(ng, sizeof(ng), _n("%d game", "%d games", t->games), t->games);
			/* TRANSLATORS: a game library: "3 systems", "120 games", its size ("1.5 GB") */
			tfmt(it->value, sizeof(it->value), C_("library summary", "%s, %s, %s"), ns, ng, a);
		} else {
			/* TRANSLATORS: a number of files and their size ("12 files, 40 MB") */
			tfmt(it->value, sizeof(it->value), _n("%d file, %s", "%d files, %s", t->files), t->files, a);
		}
		games += t->games;
		sys += t->nsys;
		bytes += t->bytes;
	}
	SIZE(bytes, a);
	/* TRANSLATORS: menu row: import every game library found on the USB stick */
	tfmt(l, sizeof(l), _n("All (%d library)", "All (%d libraries)", c->ntrees), c->ntrees);
	it = add(m, MI_ACTION, ID_TREE_ALL, l);
	/* TRANSLATORS: a number of games and their size ("120 games, 1.5 GB") */
	tfmt(it->value, sizeof(it->value), _n("%d game, %s", "%d games, %s", games), games, a);
	/* TRANSLATORS: menu row: unmount the USB stick so it can be unplugged */
	add(m, MI_ACTION, ID_I_EJECT, _("Eject"));
}

static void import_items(struct ui *ui, struct menu *m)
{
	struct import_ctx *c = m->ctx;
	struct transfer_plan *p = c->plan;
	struct menu_item *it;
	char a[48], b[48];
	bool any;
	int n;

	(void)ui;
	m->n = 0;
	if (c->state == IS_SEARCH || c->state == IS_PLAN_WAIT) {
		/* TRANSLATORS: menu row while the USB stick is scanned (the stick's name on the right) */
		it = add(m, MI_INFO, 0, c->state == IS_SEARCH ? _("Looking for games on the drive...") :
			 /* TRANSLATORS: menu row while the stick is compared with the console */
			 _("Comparing with the console..."));
		tcpy(it->value, c->label, sizeof(it->value));
		return;
	}
	if (c->state == IS_PICK) {
		picker_items(m, c);
		return;
	}
	if (c->err || !p) {
		add(m, MI_INFO, 0, _("This drive could not be read."))->disabled = true;
		add(m, MI_ACTION, ID_I_EJECT, _("Eject"));
		return;
	}
	any = p->bytes_to_copy > 0 || p->save_conflicts > 0;
	if (c->what[0]) {
		/* TRANSLATORS: menu row: where the import copies from (a folder of the USB stick on the right) */
		it = add(m, MI_INFO, 0, _("From"));
		tcpy(it->value, c->what, sizeof(it->value));
	}
	if (p->nsys) {
		/* TRANSLATORS: menu section header: the games found on the USB stick, per system */
		add(m, MI_HEADER, 0, _("Games on the drive"));
		for (int i = 0; i < p->nsys; i++) {
			const struct transfer_plan_system *s = &p->sys[i];

			if (!s->files && !s->saves)
				continue;
			it = add(m, MI_INFO, 0, sys_label(s->id));
			SIZE(s->bytes, a);
			if (s->to_copy && s->saves)
				/* TRANSLATORS: games of a system on the stick: "3 new of 10, 40 MB, saves" */
				tfmt(it->value, sizeof(it->value),
				     _n("%d new of %d, %s, saves", "%d new of %d, %s, saves", s->to_copy),
				     s->to_copy, s->files, a);
			else if (s->to_copy)
				/* TRANSLATORS: games of a system on the stick: "3 new of 10, 40 MB" */
				tfmt(it->value, sizeof(it->value), _n("%d new of %d, %s", "%d new of %d, %s", s->to_copy),
				     s->to_copy, s->files, a);
			else if (s->saves)
				/* TRANSLATORS: "10 (games), already on the console, (and) saves" */
				tfmt(it->value, sizeof(it->value),
				     _n("%d, already here, saves", "%d, already here, saves", s->files), s->files);
			else
				/* TRANSLATORS: "10 (games), already on the console" */
				tfmt(it->value, sizeof(it->value), _n("%d, already here", "%d, already here", s->files),
				     s->files);
		}
	}
	for (int i = 0; i < p->nunknown; i++) {
		char l[128];

		/* TRANSLATORS: menu row: a folder of the stick for no known system (its name) */
		tfmt(l, sizeof(l), _("Not supported: %s"), p->unknown[i]);
		it = add(m, MI_INFO, 0, l);
		tfmt(it->value, sizeof(it->value), _n("%d file", "%d files", p->unknown_files[i]),
		     p->unknown_files[i]);
	}
	if (!any) {
		add(m, MI_INFO, 0, _("Nothing new on this drive"))->disabled = true;
		add(m, MI_ACTION, ID_I_EJECT, _("Eject"));
		return;
	}
	/* TRANSLATORS: menu section header: what to copy (toggles follow) */
	add(m, MI_HEADER, 0, C_("import section", "Copy"));
	if (p->rom_files || p->rom_copy) {
		/* TRANSLATORS: toggle: copy the games */
		it = add(m, MI_TOGGLE, ID_I_ROMS, _("Games"));
		it->on = c->o.roms;
		it->disabled = !p->rom_copy && !p->replace;
	}
	if (p->bios_files) {
		it = add(m, MI_TOGGLE, ID_I_BIOS, _("BIOS files"));
		it->on = c->o.bios;
		it->disabled = !p->bios_copy;
	}
	if (p->save_files) {
		it = add(m, MI_TOGGLE, ID_I_SAVES, _("Saves & save states"));
		it->on = c->o.saves;
	}
	if (p->theme_files) {
		it = add(m, MI_TOGGLE, ID_I_THEMES, _("Themes"));
		it->on = c->o.themes;
		it->disabled = !p->theme_copy;
	}
	if (p->shot_files) {
		it = add(m, MI_TOGGLE, ID_I_SHOTS, _("Screenshots"));
		it->on = c->o.screenshots;
		it->disabled = !p->shot_copy;
	}
	if (p->identical) {
		/* TRANSLATORS: menu row: files of the stick that are already on the console */
		it = add(m, MI_INFO, 0, _("Already on the console"));
		tfmt(it->value, sizeof(it->value), _n("%d file, skipped", "%d files, skipped", p->identical),
		     p->identical);
	}
	n = p->replace + p->save_conflicts;
	if (n) {
		/* TRANSLATORS: menu row: files of the stick that differ from the console's copy */
		it = add(m, MI_INFO, 0, _("Different on the console"));
		if (p->save_conflicts)
			/* TRANSLATORS: "4 (files, saves among them): you will be asked (what to do)" */
			tfmt(it->value, sizeof(it->value),
			     _n("%d (saves too): you will be asked", "%d (saves too): you will be asked", n), n);
		else
			/* TRANSLATORS: "4 (files): you will be asked (what to do with each)" */
			tfmt(it->value, sizeof(it->value), _n("%d: you will be asked", "%d: you will be asked", n), n);
	}
	SIZE(p->bytes_to_copy, a);
	SIZE(p->dst_free, b);
	/* TRANSLATORS: menu row: how much the import will copy / there is not enough free space */
	it = add(m, MI_INFO, 0, p->fits ? _("To copy") : _("Not enough space"));
	/* TRANSLATORS: "up to 1.5 GB, 12 GB free" (on the console's SD card) */
	tfmt(it->value, sizeof(it->value), _("up to %s, %s free"), a, b);
	/* TRANSLATORS: menu row: start the copy */
	it = add(m, MI_ACTION, ID_I_COPY, _("Copy"));
	it->disabled = !p->fits;
	add(m, MI_ACTION, ID_I_EJECT, _("Eject"));
}

static void import_menu_open(struct ui *ui, struct import_ctx *c)
{
	struct menu *m;
	char title[64];

	/* TRANSLATORS: menu title, the USB stick's name follows */
	tfmt(title, sizeof(title), _("Import from %s"), c->label);
	m = menu_new(ui, title, K_IMPORT);
	m->ctx = c;
	m->on_item = import_item;
	m->on_refresh = import_refresh;
	m->on_destroy = import_destroy;
	if (c->state == IS_PICK)            /* a library was chosen: plan it */
		start_plan(ui, m, c);
	import_items(ui, m);
	menu_open(ui, m);
}

void import_open(struct ui *ui, const char *mp, const char *label)
{
	struct import_ctx *c;
	int r;

	if (!TR(ui) || show_running_job(ui, mp, label))
		return;
	c = xcalloc(1, sizeof(*c));
	strlcpy_(c->mp, mp, sizeof(c->mp));
	tcpy(c->label, label && *label ? label : _("USB drive"), sizeof(c->label));
	TR(ui)->import_defaults(&c->o);
	r = TR(ui)->trees_start(mp);
	if (r < 0) {
		free(c);
		message_open(ui, _("This drive is busy (a scan or a copy is running)."));
		return;
	}
	c->state = IS_SEARCH;
	{
		struct menu *m;
		char title[64];

		tfmt(title, sizeof(title), _("Import from %s"), c->label);
		m = menu_new(ui, title, K_IMPORT);
		m->ctx = c;
		m->on_item = import_item;
		m->on_refresh = import_refresh;
		m->on_destroy = import_destroy;
		m->refresh_every = 150;
		import_items(ui, m);
		menu_open(ui, m);
	}
}

/* ------------------------------------------- export games / back up saves */
struct backup_ctx {
	char mp[64], label[64], fstype[16];
	enum transfer_backup_mode mode;
	bool scanning;
	int err;
	struct transfer_backup *b;
	struct transfer_backup_opts o;
	struct transfer_backup_totals t;
	/* back up saves: the last played game */
	char last_rom[1024], last_sys[32], last_name[96];
	int game_choice;             /* 0 all games, 1 the last played one */
};

static void backup_items(struct ui *ui, struct menu *m);

static void backup_destroy(struct ui *ui, struct menu *m)
{
	struct backup_ctx *c = m->ctx;

	if (c->scanning)
		ui->scan_orphan |= ORPHAN_BACKUP;
	else if (c->b && TR(ui))
		TR(ui)->backup_free(c->b);
	free(c);
}

static void backup_apply_opts(struct backup_ctx *c)
{
	c->o.game_rom[0] = c->o.game_system[0] = 0;
	if (c->mode == TRANSFER_BACKUP_SAVES && c->game_choice == 1 && c->last_rom[0]) {
		strlcpy_(c->o.game_rom, c->last_rom, sizeof(c->o.game_rom));
		strlcpy_(c->o.game_system, c->last_sys, sizeof(c->o.game_system));
	}
}

static void backup_refresh(struct ui *ui, struct menu *m)
{
	struct backup_ctx *c = m->ctx;
	int r;

	if (!c->scanning)
		return;
	r = TR(ui)->backup_scan_poll(&c->b);
	if (r == 0)
		return;
	c->scanning = false;
	m->refresh_every = 0;
	if (r < 0)
		c->err = r;
	backup_items(ui, m);
	m->cursor = 0;
	for (int i = 0; i < m->n; i++)
		if (m->items[i].id == ID_B_START)
			m->cursor = i;
	ui->dirty = true;
}

static void backup_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	struct backup_ctx *c = m->ctx;
	char mp[64], label[64];
	int r, keep = it->id;

	switch (it->id) {
	case ID_B_BIOS:
		c->o.bios = it->on;
		break;
	case ID_B_SETTINGS:
		c->o.settings = it->on;
		break;
	case ID_B_GAME:
		c->game_choice = it->val;
		break;
	case ID_B_START: {
		struct transfer_backup *b = c->b;

		if (dir || !b)
			return;
		strlcpy_(mp, c->mp, sizeof(mp));
		strlcpy_(label, c->label, sizeof(label));
		r = TR(ui)->usb_remount(mp, true);
		if (r < 0) {
			LOGI("usb: %s: remount read-write failed (%d)", mp, r);
			message_open(ui, _("The USB drive cannot be written to (is it write-protected?)."));
			return;
		}
		backup_apply_opts(c);
		r = TR(ui)->backup_start(b, &c->o);
		if (r < 0) {
			TR(ui)->usb_remount(mp, false);
			message_open(ui, r == -16 /* EBUSY */ ? _("Another copy is already running.") :
				     _("The copy could not start."));
			return;
		}
		LOGI("usb: %s started to %s: %d files to copy, %d already there",
		     c->mode == TRANSFER_EXPORT_GAMES ? "export" : "saves backup", label, c->t.files,
		     c->t.unchanged);
		c->b = NULL;          /* owned by the backup now */
		g_backup_kind = c->mode == TRANSFER_EXPORT_GAMES ? 1 : 2;
		pop_menus(ui, K_BACKUP);  /* frees c (review F-M2): only locals after this */
		progress_open(ui, g_backup_kind, mp, label);
		return;
	}
	case ID_I_EJECT:
		if (dir)
			return;
		strlcpy_(mp, c->mp, sizeof(mp));
		pop_menus(ui, K_BACKUP);
		eject_mp(ui, mp);
		return;
	default:
		return;
	}
	/* an option changed: new totals, the cursor stays on it */
	backup_items(ui, m);
	for (int i = 0; i < m->n; i++)
		if (m->items[i].id == keep)
			m->cursor = i;
}

static void backup_items(struct ui *ui, struct menu *m)
{
	struct backup_ctx *c = m->ctx;
	const struct transfer_backup_info *in;
	struct menu_item *it;
	char a[48], b[48];
	bool games = c->mode == TRANSFER_EXPORT_GAMES;

	m->n = 0;
	if (c->scanning) {
		/* TRANSLATORS: menu row while the console is compared with the USB stick */
		it = add(m, MI_INFO, 0, _("Comparing with the drive..."));
		tcpy(it->value, c->label, sizeof(it->value));
		return;
	}
	if (c->err || !c->b) {
		add(m, MI_INFO, 0, _("This drive could not be read."))->disabled = true;
		add(m, MI_ACTION, ID_I_EJECT, _("Eject"));
		return;
	}
	in = TR(ui)->backup_info(c->b);
	backup_apply_opts(c);
	TR(ui)->backup_totals(c->b, &c->o, &c->t);
	/* TRANSLATORS: menu row: the folder of the USB stick the copy goes to (its name on the right) */
	it = add(m, MI_INFO, 0, _("Folder on the drive"));
	if (in->exists)
		tcpy(it->value, in->folder, sizeof(it->value));
	else
		/* TRANSLATORS: a folder name, then this: the folder will be created */
		tfmt(it->value, sizeof(it->value), _("%s (new)"), in->folder);
	if (games) {
		/* TRANSLATORS: menu row: the games on the console ("120 files, 1.5 GB") */
		it = add(m, MI_INFO, 0, _("Games on the console"));
		SIZE(in->bytes[TRANSFER_BK_ROMS], a);
		tfmt(it->value, sizeof(it->value), _n("%d file, %s", "%d files, %s", in->files[TRANSFER_BK_ROMS]),
		     in->files[TRANSFER_BK_ROMS], a);
		/* TRANSLATORS: toggle: also copy the BIOS files */
		it = add(m, MI_TOGGLE, ID_B_BIOS, _("BIOS files too"));
		it->on = c->o.bios;
	} else {
		static char last[192];

		/* TRANSLATORS: choice: the saves of which games (All games / Last played: <game>) */
		it = add(m, MI_CHOICE, ID_B_GAME, C_("saves of", "Games"));
		it->nchoices = 1;
		it->choices[0] = _("All games");
		if (c->last_rom[0]) {
			/* TRANSLATORS: choice value: only the last played game (its name follows) */
			tfmt(last, sizeof(last), _("Last played: %s"), c->last_name);
			it->choices[1] = last;
			it->nchoices = 2;
		}
		it->val = MIN(c->game_choice, it->nchoices - 1);
		tcpy(it->value, it->choices[it->val], sizeof(it->value));
		/* TRANSLATORS: toggle: also copy the per-game settings */
		it = add(m, MI_TOGGLE, ID_B_SETTINGS, _("Settings too (core options, remaps)"));
		it->on = c->o.settings;
	}
	/* TRANSLATORS: menu row: files already on the USB stick, not copied again */
	it = add(m, MI_INFO, 0, _("Already on the drive"));
	tfmt(it->value, sizeof(it->value), _n("%d file", "%d files", c->t.unchanged), c->t.unchanged);
	SIZE(c->t.bytes, a);
	SIZE(in->free_bytes, b);
	if (c->t.files) {
		it = add(m, MI_INFO, 0, c->t.fits ? _("To copy") : _("Not enough space"));
		/* TRANSLATORS: "12 files, 1.5 GB (20 GB free)" (on the USB stick) */
		tfmt(it->value, sizeof(it->value), _n("%d file, %s (%s free)", "%d files, %s (%s free)", c->t.files),
		     c->t.files, a, b);
	} else {
		it = add(m, MI_INFO, 0, _("Nothing to copy: the drive is up to date"));
		it->disabled = true;
	}
	if (c->t.too_big) {
		char l[128];

		/* TRANSLATORS: menu row (FAT32 cannot store files of 4 GB or more), about 45 characters */
		tfmt(l, sizeof(l), _n("%d file over 4 GB: FAT32 can't hold it",
				      "%d files over 4 GB: FAT32 can't hold them", c->t.too_big), c->t.too_big);
		add(m, MI_INFO, 0, l)->disabled = true;
		for (int i = 0; i < in->nbig && i < 3; i++) {
			const char *nm = strrchr(in->big[i], '/');

			/* the indent: under the row above */
			/* TRANSLATORS: menu row: a file that is not copied (its name on the right) */
			tfmt(l, sizeof(l), "  %s", _("skipped"));
			it = add(m, MI_INFO, 0, l);
			tcpy(it->value, nm ? nm + 1 : in->big[i], sizeof(it->value));
			it->disabled = true;
		}
	}
	/* TRANSLATORS: menu row: start copying the games / the saves to the USB stick */
	it = add(m, MI_ACTION, ID_B_START, games ? _("Export") : _("Back up"));
	it->disabled = !c->t.files || !c->t.fits;
	add(m, MI_ACTION, ID_I_EJECT, _("Eject"));
}

/* The most recently played game (gamedb), for "Last played". */
struct last_played {
	int64_t when;
	char sys[32], rel[512];
};

static void last_cb(const char *system, const char *rel, int64_t lastplayed, void *user)
{
	struct last_played *lp = user;

	if (lastplayed > lp->when) {
		lp->when = lastplayed;
		strlcpy_(lp->sys, system, sizeof(lp->sys));
		strlcpy_(lp->rel, rel, sizeof(lp->rel));
	}
}

static void backup_open(struct ui *ui, enum transfer_backup_mode mode, const char *mp, const char *label)
{
	struct transfer_usb_drive d;
	struct backup_ctx *c;
	struct menu *m;
	char title[64];
	int r;

	if (!TR(ui) || !TR(ui)->backup_scan_start || show_running_job(ui, mp, label))
		return;
	if (!find_drive(ui, mp, &d)) {
		message_open(ui, _("The USB drive is not there any more."));
		return;
	}
	c = xcalloc(1, sizeof(*c));
	c->mode = mode;
	strlcpy_(c->mp, mp, sizeof(c->mp));
	tcpy(c->label, label && *label ? label : _("USB drive"), sizeof(c->label));
	strlcpy_(c->fstype, d.fstype, sizeof(c->fstype));
	c->o.mode = mode;
	c->o.bios = true;           /* the same defaults as transfer_backup_defaults() */
	c->o.settings = false;
	if (mode == TRANSFER_BACKUP_SAVES && ui->db) {
		struct last_played lp = { 0 };

		gamedb_each(ui->db, false, last_cb, &lp);
		if (lp.when > 0) {
			snprintf(c->last_rom, sizeof(c->last_rom), "%s/%s/%s", ui->cfg.roms_dir, lp.sys, lp.rel);
			strlcpy_(c->last_sys, lp.sys, sizeof(c->last_sys));
			games_clean_name(path_basename(lp.rel), c->last_name, sizeof(c->last_name));
		}
	}
	r = TR(ui)->backup_scan_start(data_root(ui), mp, d.fstype, mode);
	if (r < 0) {
		free(c);
		message_open(ui, _("This drive is busy (a scan or a copy is running)."));
		return;
	}
	c->scanning = true;
	if (mode == TRANSFER_EXPORT_GAMES)
		/* TRANSLATORS: menu title, the USB stick's name follows */
		tfmt(title, sizeof(title), _("Export games to %s"), c->label);
	else
		/* TRANSLATORS: menu title, the USB stick's name follows */
		tfmt(title, sizeof(title), _("Back up saves to %s"), c->label);
	m = menu_new(ui, title, K_BACKUP);
	m->ctx = c;
	m->on_item = backup_item;
	m->on_refresh = backup_refresh;
	m->on_destroy = backup_destroy;
	m->refresh_every = 150;
	backup_items(ui, m);
	menu_open(ui, m);
}

/* ----------------------------------------------------------- progress */
/* kind: 0 import (stick -> console), 1 export games, 2 back up saves */
struct progress {
	struct screen base;
	int kind;
	char mp[64], label[64];
	struct transfer_progress pr;
	int64_t next_poll;
	int last_q;                  /* the last question shown */
};

static void cancel_choice(struct ui *ui, int choice, void *user)
{
	struct progress *p = user;

	if (choice != 0 || !TR(ui))
		return;
	LOGI("usb: copy cancelled by the user");
	if (p->kind == 0)
		TR(ui)->import_cancel();
	else
		TR(ui)->backup_cancel();
}

static void fmt_date(int64_t t, char *out, size_t n)
{
	if (t > 0)
		i18n_format_datetime(t, out, n);
	if (t <= 0 || !out[0])
		/* TRANSLATORS: a file's date is not known (duplicate file prompt) */
		tcpy(out, _("unknown date"), n);
}

static void dup_choice(struct ui *ui, int choice, void *user)
{
	static const enum transfer_answer map[] = {
		TRANSFER_ANSWER_SKIP, TRANSFER_ANSWER_REPLACE, TRANSFER_ANSWER_SKIP_ALL,
		TRANSFER_ANSWER_REPLACE_ALL,
	};
	static const char *const names[] = { "skip", "replace", "skip all", "replace all" };

	(void)user;
	if (choice < 0 || choice > 3)
		choice = 0;                      /* B: skip this one */
	LOGI("usb: duplicate answer: %s", names[choice]);
	TR(ui)->import_answer(map[choice]);
}

static void dup_open(struct ui *ui, const struct transfer_question *q)
{
	/* TRANSLATORS: dialog buttons, uppercase, short: what to do with a file that is
	 * already on the console (this one / every next one) */
	const char *const buttons[] = { _("SKIP"), _("REPLACE"), _("SKIP ALL"), _("REPLACE ALL"), NULL };
	char msg[512], a[48], b[48], da[64], db[64], details[256], note[256];
	const char *name = strrchr(q->path, '/'), *head;

	SIZE(q->src_size, a);
	SIZE(q->dst_size, b);
	fmt_date(q->src_mtime, da, sizeof(da));
	fmt_date(q->dst_mtime, db, sizeof(db));
	name = name ? name + 1 : q->path;
	/* TRANSLATORS: the first line of the duplicate file prompt (the file name follows) */
	if (q->save && q->kind == TRANSFER_CONFIG)
		head = _("This settings file is already on the console, but different:");
	else if (q->save)
		head = _("This save is already on the console, but different:");
	else if (q->kind == TRANSFER_ROM)
		head = _("This game is already on the console, but different:");
	else if (q->kind == TRANSFER_BIOS)
		head = _("This BIOS file is already on the console, but different:");
	else
		head = _("This file is already on the console, but different:");
	/* TRANSLATORS: the two copies of a file: size, date ("USB drive: 2 MB, 2024-05-01 10:00") */
	tfmt(details, sizeof(details), _("USB drive: %s, %s\nConsole: %s, %s"), a, da, b, db);
	note[0] = 0;
	if (q->save)
		/* TRANSLATORS: %s is the save's file name ("Zelda.srm" -> "Zelda.srm.bak") */
		tfmt(note, sizeof(note), _("Saves are precious: if you replace it, the console's copy is kept as %s.bak."),
		     name);
	tfmt(msg, sizeof(msg), "%s\n%s\n%s%s%s", head, name, details, note[0] ? "\n" : "", note);
	LOGI("usb: duplicate %s (%s): asking", q->path, q->save ? "save" : "file");
	dialog_open(ui, msg, buttons, dup_choice, NULL);
}

static void pr_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct progress *p = (struct progress *)scr;
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	struct font *fs = font_get(ms->font_path, ui_font_px(ui, ms->small_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : NULL,
				   ui_font_px(ui, ms->font_size * 1.1f));
	int W = ui->w, H = ui->h, pw = W * 85 / 100, ph = H * 55 / 100;
	int x = (W - pw) / 2, y = (H - ph) / 2, pad = H / 30, lh = font_height(f) * 2;
	int bw = pw - 4 * pad, bh = MAX(8, H / 40);
	char buf[320], a[48], b[48];
	uint64_t done = p->pr.bytes_done, total = MAX(p->pr.bytes_total, 1);
	/* TRANSLATORS: help bar: B stops the copy */
	static const struct help_prompt prompts[] = { { "b", N_("cancel") } };

	draw_panel(s, x, y, pw, ph, MAX(4, H / 60), ms->bg);
	if (p->kind % 3 == 0)
		/* TRANSLATORS: progress title, the USB stick's name follows */
		tfmt(buf, sizeof(buf), _("Copying from %s"), p->label);
	else if (p->kind % 3 == 1)
		/* TRANSLATORS: progress title, the USB stick's name follows */
		tfmt(buf, sizeof(buf), _("Exporting games to %s"), p->label);
	else
		/* TRANSLATORS: progress title, the USB stick's name follows */
		tfmt(buf, sizeof(buf), _("Backing up saves to %s"), p->label);
	draw_text_box(ui, s, fb, buf, x, y + pad, pw, lh, AL_CENTER, ms->title);
	gfx_fill_round(s, x + 2 * pad, y + pad + lh * 3 / 2, bw, bh, bh / 2, gfx_with_alpha(ms->text_dim, 70));
	gfx_fill_round(s, x + 2 * pad, y + pad + lh * 3 / 2,
		       MAX(bh, (int)((uint64_t)bw * MIN(done, total) / total)), bh, bh / 2, ms->accent);
	SIZE(done, a);
	SIZE(p->pr.bytes_total, b);
	/* TRANSLATORS: copy progress: "120 MB of 1.5 GB  -  3 of 21 files" */
	tfmt(buf, sizeof(buf), _n("%s of %s  -  %d of %d file", "%s of %s  -  %d of %d files", p->pr.files_total),
	     a, b, p->pr.files_done, p->pr.files_total);
	draw_text_box(ui, s, f, buf, x + pad, y + pad + lh * 2, pw - 2 * pad, lh, AL_CENTER, ms->text);
	draw_text_box(ui, s, fs, p->pr.current, x + pad, y + pad + lh * 3, pw - 2 * pad, lh,
		      AL_CENTER, ms->text_dim);
	{
		char rate[48], r[160] = "", idn[128] = "";
		int min = (p->pr.eta_s + 59) / 60;

		i18n_format_number(p->pr.rate_kbs / 1024.0, 1, rate, sizeof(rate));
		if (p->pr.rate_kbs && p->pr.eta_s >= 0)
			/* TRANSLATORS: copy speed and time left: "4.2 MB/s, about 3 min left" */
			tfmt(r, sizeof(r), _n("%s MB/s, about %d min left", "%s MB/s, about %d min left", min),
			     rate, min);
		else if (p->pr.rate_kbs)
			/* TRANSLATORS: copy speed: "4.2 MB/s" */
			tfmt(r, sizeof(r), _("%s MB/s"), rate);
		/* incremental copies: what did not need copying */
		if (p->pr.identical)
			/* TRANSLATORS: files not copied because they are already there, "12 already there" */
			tfmt(idn, sizeof(idn), _n("%d already there", "%d already there", p->pr.identical),
			     p->pr.identical);
		tfmt(buf, sizeof(buf), "%s%s%s", r, r[0] && idn[0] ? "  -  " : "", idn);
		if (buf[0])
			draw_text_box(ui, s, fs, buf, x + pad, y + pad + lh * 4, pw - 2 * pad, lh, AL_CENTER,
				      ms->text_dim);
	}
	help_draw(ui, s, &ui->menu_help, prompts, 1);
}

static void import_summary(struct progress *p, enum transfer_state st, char *msg, size_t n)
{
	const struct transfer_progress *r = &p->pr;
	char head[256] = "", body[256];
	bool some_failed = r->failed && st == TRANSFER_DONE;
	const char *tail;

	if (st == TRANSFER_CANCELLED)
		/* TRANSLATORS: import summary, first sentence (the counts follow) */
		tcpy(head, _("Copy cancelled."), sizeof(head));
	else if (st == TRANSFER_FAILED)
		/* TRANSLATORS: import summary, first sentence: %s is why ("The SD card is full") */
		tfmt(head, sizeof(head), _("The copy stopped: %s."), r->errmsg[0] ? _(r->errmsg) : _("error"));
	/* TRANSLATORS: import summary: numbers of files imported, already on the console, skipped
	 * (a duplicate kept) and replaced (a duplicate overwritten) */
	tfmt(body, sizeof(body), _("Imported %d, already there %d, skipped %d, replaced %d."),
	     r->copied - r->replaced, r->identical, r->kept, r->replaced);
	/* TRANSLATORS: import summary, last sentence */
	tail = some_failed ? _("Some files failed.") : "";
	tfmt(msg, n, "%s%s%s%s%s", head, head[0] ? " " : "", body, tail[0] ? " " : "", tail);
}

static void backup_summary(struct progress *p, enum transfer_state st, char *msg, size_t n)
{
	const struct transfer_progress *r = &p->pr;
	char a[48], part[256];
	size_t o = 0;

	SIZE(r->bytes_done, a);
	msg[0] = 0;
	if (st == TRANSFER_CANCELLED) {
		/* TRANSLATORS: export / saves backup summary, first sentence (the counts follow) */
		tfmt(msg, n, "%s ", _("Cancelled."));
	} else if (st == TRANSFER_FAILED) {
		/* TRANSLATORS: export / saves backup summary, first sentences: %s is why ("The USB drive is full") */
		tfmt(part, sizeof(part), _("Stopped: %s. The partial file was removed."),
		     r->errmsg[0] ? _(r->errmsg) : _("error"));
		tfmt(msg, n, "%s ", part);
	}
	o = strlen(msg);
	/* TRANSLATORS: export / saves backup summary: files copied (their size), files already on the stick */
	tfmt(part, sizeof(part), _n("Copied %d file (%s), already there %d.", "Copied %d files (%s), already there %d.",
				    r->copied), r->copied, a, r->identical);
	tfmt(msg + o, n - o, "%s", part);
	o = strlen(msg);
	if (r->skipped) {
		/* TRANSLATORS: export summary: files FAT32 cannot store were not copied */
		tfmt(part, sizeof(part), _n("%d file over 4 GB skipped (FAT32).", "%d files over 4 GB skipped (FAT32).",
					    r->skipped), r->skipped);
		tfmt(msg + o, n - o, " %s", part);
		o = strlen(msg);
	}
	if (r->failed) {
		/* TRANSLATORS: export / saves backup summary: files that could not be copied */
		tfmt(part, sizeof(part), _n("%d failed.", "%d failed.", r->failed), r->failed);
		tfmt(msg + o, n - o, " %s", part);
		o = strlen(msg);
	}
	/* "Last played game": say what was saved (file names) */
	if (p->kind == 2 && r->ncopied_names && r->ncopied_names <= 4) {
		tfmt(msg + o, n - o, "\n");
		o = strlen(msg);
		for (int i = 0; i < r->ncopied_names; i++) {
			const char *nm = strrchr(r->copied_names[i], '/');

			tfmt(msg + o, n - o, "%s%s", i ? ", " : "", nm ? nm + 1 : r->copied_names[i]);
			o = strlen(msg);
		}
	}
}

/* The summary for the log (never translated). */
static void log_summary(struct progress *p, enum transfer_state st)
{
	const struct transfer_progress *r = &p->pr;
	static const char *const kinds[] = { "import", "export", "saves backup" };

	LOGI("usb: %s %s%s%s: copied %d (replaced %d), already there %d, kept %d, skipped %d, failed %d",
	     kinds[p->kind % 3], st == TRANSFER_CANCELLED ? "cancelled" : st == TRANSFER_FAILED ? "stopped" : "done",
	     st == TRANSFER_FAILED ? ": " : "", st == TRANSFER_FAILED ? r->errmsg : "", r->copied, r->replaced,
	     r->identical, r->kept, r->skipped, r->failed);
}

static bool pr_update(struct ui *ui, struct screen *scr)
{
	struct progress *p = (struct progress *)scr;
	enum transfer_state st;
	char changed[TRANSFER_SYS_MAX][TRANSFER_SYSID_MAX];
	char msg[512], mp[64];
	int n;

	if (ui->now < p->next_poll)
		return false;
	p->next_poll = ui->now + 200;
	st = p->kind == 0 ? TR(ui)->import_status(&p->pr) : TR(ui)->backup_status(&p->pr);
	if (st < TRANSFER_DONE) {
		if (p->kind == 0 && p->pr.asking && p->pr.question.seq != p->last_q) {
			p->last_q = p->pr.question.seq;
			dup_open(ui, &p->pr.question);
		}
		return true;
	}
	strlcpy_(mp, p->mp, sizeof(mp));
	if (p->kind == 0) {
		n = TR(ui)->import_finish(changed, TRANSFER_SYS_MAX);
		invalidate(ui, changed, n);
		import_summary(p, st, msg, sizeof(msg));
	} else {
		TR(ui)->backup_finish();
		/* read-only again: always safe to pull */
		if (TR(ui)->usb_remount(mp, false) < 0)
			LOGW("usb: %s stays read-write: eject it before unplugging", mp);
		backup_summary(p, st, msg, sizeof(msg));
	}
	log_summary(p, st);
	ui_pop(ui); /* frees p */
	reload_if_needed(ui);
	summary_open(ui, mp, msg);
	return true;
}

static void pr_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct progress *p = (struct progress *)scr;

	if (t == IN_NAV_PRESS && b == IN_B) {
		/* TRANSLATORS: dialog buttons, uppercase, short: stop the copy / go on copying */
		const char *const buttons[] = { _("STOP"), _("CONTINUE"), NULL };

		/* TRANSLATORS: B during an import (USB stick -> console) */
		dialog_open(ui, p->kind == 0 ? _("Stop copying? Files already copied stay.") :
			    /* TRANSLATORS: B during an export / saves backup (console -> USB stick) */
			    _("Stop? Files already copied stay on the drive."), buttons, cancel_choice, p);
	}
}

static int pr_timeout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
	return 200;
}

static void pr_destroy(struct ui *ui, struct screen *scr)
{
	(void)ui;
	free(scr);
}

static void pr_relayout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
}

static void pr_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	struct progress *p = (struct progress *)scr;
	static const char *const kinds[] = { "import", "export", "saves" };

	(void)ui;
	snprintf(buf, n, "progress:%s %d/%d", kinds[p->kind % 3], p->pr.files_done, p->pr.files_total);
}

static const struct screen_ops pr_ops = {
	.button = pr_button,
	.update = pr_update,
	.render = pr_render,
	.relayout = pr_relayout,
	.destroy = pr_destroy,
	.timeout = pr_timeout,
	.opaque = false,
	.describe = pr_describe,
};

/* the running copy's drive, for reopening its progress screen */
static char g_job_mp[64], g_job_label[64];

static void progress_open(struct ui *ui, int kind, const char *mp, const char *label)
{
	struct progress *p = xcalloc(1, sizeof(*p));

	strlcpy_(g_job_mp, mp, sizeof(g_job_mp));
	strlcpy_(g_job_label, label, sizeof(g_job_label));

	p->base.ops = &pr_ops;
	p->kind = kind;
	strlcpy_(p->mp, mp, sizeof(p->mp));
	strlcpy_(p->label, label, sizeof(p->label));
	ui_push(ui, &p->base);
}

/* --------------------------------------------------------- USB events */
static void usb_choice(struct ui *ui, int choice, void *user)
{
	static const char *const names[] = { "import games", "export games", "back up saves", "nothing" };
	struct transfer_usb_drive d;
	char mp[64];
	/* user: the dialog has "Install system update" (update_ui.c) at 3 */
	bool with_update = user != NULL;

	strlcpy_(mp, ui->usb_dialog_mp, sizeof(mp));
	ui->usb_dialog_mp[0] = 0;
	ui->usb_dialog = NULL;
	LOGI("usb: dialog answer for %s: %s", mp, with_update && choice == 3 ? "install system update" :
	     choice >= 0 && choice < 3 ? names[choice] : names[3]);
	if (with_update && choice == 3) {
		update_open_usb(ui, mp);
		return;
	}
	if (choice < 0 || choice > 2)
		return;
	if (!find_drive(ui, mp, &d)) {
		message_open(ui, _("The USB drive is not there any more."));
		return;
	}
	if (choice == 0)
		import_open(ui, mp, drive_name(&d));
	else
		backup_open(ui, choice == 1 ? TRANSFER_EXPORT_GAMES : TRANSFER_BACKUP_SAVES, mp, drive_name(&d));
}

/* The USB dialog is still on the stack (a reload pops screens without
 * calling their callbacks). */
static bool usb_dialog_open(struct ui *ui)
{
	for (int i = 0; ui->usb_dialog && i < ui->nstack; i++)
		if (ui->stack[i] == ui->usb_dialog)
			return true;
	ui->usb_dialog = NULL;
	ui->usb_dialog_mp[0] = 0;
	return false;
}

/* The dialog, or a toast, for a drive that is mounted now. */
static void usb_offer(struct ui *ui, const struct transfer_usb_drive *d)
{
	/* TRANSLATORS: dialog buttons, uppercase, short: what to do with a USB stick just plugged in */
	const char *buttons[] = { _("IMPORT GAMES"), _("EXPORT GAMES"), _("BACK UP SAVES"), _("NOTHING"), NULL, NULL };
	char msg[512], sz[48];
	/* a system update package (*.rsu) on the stick: one more choice */
	bool with_update = update_available(ui) && update_usb_has_package(d->mountpoint);

	if (with_update) {
		/* TRANSLATORS: USB dialog button, uppercase, short: install the RetroStoneOS update found on the stick */
		buttons[3] = _("INSTALL UPDATE");
		buttons[4] = _("NOTHING");
	}
	if (!settings_get_bool(ui->settings, "usb_import_prompt", true)) {
		LOGI("usb: %s (%s): dialog suppressed: usb_import_prompt=0 (toast only)", drive_log(d),
		     d->mountpoint);
		/* TRANSLATORS: toast; %s is the stick's name. "Settings > Storage" must match those menus */
		ui_toast_long(ui, _("USB drive %s connected: Settings > Storage to use it"), drive_name(d));
		return;
	}
	if (job_running(ui)) {
		LOGI("usb: %s (%s): dialog suppressed: a copy is running", drive_log(d), d->mountpoint);
		/* TRANSLATORS: toast, short; %s is the stick's name */
		ui_toastf(ui, _("USB drive %s connected"), drive_name(d));
		return;
	}
	if (usb_dialog_open(ui)) {
		LOGI("usb: %s (%s): dialog suppressed: the dialog for %s is open", drive_log(d),
		     d->mountpoint, ui->usb_dialog_mp);
		return;
	}
	SIZE(d->size_bytes, sz);
	/* TRANSLATORS: %1$s the stick's name, %2$s its format ("exfat"), %3$s its size */
	tfmt(msg, sizeof(msg), _("USB drive \"%s\" connected (%s, %s). What do you want to do?"),
	     drive_name(d), d->fstype, sz);
	strlcpy_(ui->usb_dialog_mp, d->mountpoint, sizeof(ui->usb_dialog_mp));
	ui->usb_dialog = dialog_open(ui, msg, buttons, usb_choice, with_update ? (void *)ui : NULL);
	if (!ui->usb_dialog) {
		/* the screen stack is full: a toast instead */
		ui->usb_dialog_mp[0] = 0;
		LOGI("usb: %s (%s): dialog not shown (screen stack full)", drive_log(d), d->mountpoint);
		ui_toast_long(ui, _("USB drive %s connected: Settings > Storage to use it"), drive_name(d));
		return;
	}
	LOGI("usb: %s (%s): dialog shown", drive_log(d), d->mountpoint);
}

void ui_usb_event(struct ui *ui, const struct transfer_usb_event *ev)
{
	const struct transfer_usb_drive *d = &ev->drive;

	switch (ev->type) {
	case TRANSFER_USB_MOUNTED:
		if (!ui->loaded || ui->in_game) {
			/* shown by transfer_poll() once the menu is back */
			LOGI("usb: %s (%s): dialog deferred: %s", drive_log(d), d->mountpoint,
			     ui->in_game ? "a game is running" : "the menu is not up yet");
			strlcpy_(ui->usb_pending, d->mountpoint, sizeof(ui->usb_pending));
			break;
		}
		usb_offer(ui, d);
		break;
	case TRANSFER_USB_REMOVED:
		LOGI("usb: %s (%s) removed", drive_log(d), d->mountpoint);
		if (!strcmp(ui->usb_pending, d->mountpoint))
			ui->usb_pending[0] = 0;
		if (usb_dialog_open(ui) && !strcmp(ui->usb_dialog_mp, d->mountpoint)) {
			if (ui_top(ui) == ui->usb_dialog)
				ui_pop(ui);             /* no callback: nothing to answer */
			ui->usb_dialog = NULL;
			ui->usb_dialog_mp[0] = 0;
		}
		{
			/* the copy from/to this drive stops now (review F-M17: it went
			 * on, and could read a stick plugged in at the same place) */
			struct transfer_progress pr;
			int j = job_running(ui);
			enum transfer_state st = j == 1 ? TR(ui)->import_status(&pr) :
						 j == 2 ? TR(ui)->backup_status(&pr) : TRANSFER_IDLE;

			if (st != TRANSFER_IDLE && st < TRANSFER_DONE && !strcmp(g_job_mp, d->mountpoint)) {
				LOGI("usb: %s removed during the copy: stopping it", d->mountpoint);
				if (j == 1)
					TR(ui)->import_cancel();
				else
					TR(ui)->backup_cancel();
				/* TRANSLATORS: toast */
				ui_toast(ui, _("USB drive removed: the copy stopped"), UI_SEV_WARNING);
				break;
			}
		}
		/* TRANSLATORS: toast, short */
		ui_toastf(ui, "%s", _("USB drive removed"));
		break;
	case TRANSFER_USB_EJECTED:
		ui_toast_long(ui, "%s", _("USB drive ejected: you can unplug it"));
		break;
	case TRANSFER_USB_UNSUPPORTED:
		LOGI("usb: disk %s: format not supported", d->disk);
		/* TRANSLATORS: toast */
		ui_toast_long(ui, "%s", _("USB drive: format not supported (use FAT32 or exFAT)"));
		break;
	case TRANSFER_USB_MOUNT_FAILED:
		LOGI("usb: %s: mount failed (%d)", d->dev, ev->err);
		/* TRANSLATORS: toast */
		ui_toast(ui, _("The USB drive could not be read"), UI_SEV_WARNING);
		break;
	}
	ui->dirty = true;
}

void transfer_after_game(struct ui *ui)
{
	(void)ui;   /* transfer_poll() shows the deferred dialog at the next update */
}

/* A deferred offer, once the menu is up and no game runs. */
static void offer_pending(struct ui *ui)
{
	struct transfer_usb_drive d;
	char mp[64];

	if (!ui->usb_pending[0] || !ui->loaded || ui->in_game || ui->charge_mode)
		return;
	strlcpy_(mp, ui->usb_pending, sizeof(mp));
	ui->usb_pending[0] = 0;
	if (find_drive(ui, mp, &d))
		usb_offer(ui, &d);
	else
		LOGI("usb: %s: deferred dialog dropped: the drive is gone", mp);
}

/* Settings > Storage: a drive, then the flow. */
static void drives_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	struct transfer_usb_drive d[8];
	int n = TR(ui)->usb_drives(d, 8), i = it->id - ID_DRIVE0, act = (int)(intptr_t)m->ctx;

	if (dir || i < 0 || i >= n)
		return;
	m->ctx = NULL;
	ui_pop(ui);
	if (act == USB_ACT_IMPORT)
		import_open(ui, d[i].mountpoint, drive_name(&d[i]));
	else
		backup_open(ui, act == USB_ACT_EXPORT ? TRANSFER_EXPORT_GAMES : TRANSFER_BACKUP_SAVES,
			    d[i].mountpoint, drive_name(&d[i]));
}

void transfer_usb_action(struct ui *ui, int action)
{
	/* TRANSLATORS: menu titles over a list of the USB drives plugged in (pick one) */
	static const char *const titles[] = { N_("Import games from"), N_("Export games to"), N_("Back up saves to") };
	struct transfer_usb_drive d[8];
	int n;
	struct menu *m;
	char title[64];

	if (!TR(ui)) {
		message_open(ui, _("USB transfers are not available in this build."));
		return;
	}
	n = TR(ui)->usb_drives(d, 8);
	if (n <= 0) {
		message_open(ui, _("Plug in a USB drive (FAT32, exFAT or NTFS)."));
		return;
	}
	if (n == 1) {
		if (action == USB_ACT_IMPORT)
			import_open(ui, d[0].mountpoint, drive_name(&d[0]));
		else
			backup_open(ui, action == USB_ACT_EXPORT ? TRANSFER_EXPORT_GAMES : TRANSFER_BACKUP_SAVES,
				    d[0].mountpoint, drive_name(&d[0]));
		return;
	}
	tcpy(title, _(titles[action % 3]), sizeof(title));
	m = menu_new(ui, title, K_DRIVES);
	for (int i = 0; i < n; i++) {
		struct menu_item *it = add(m, MI_ACTION, ID_DRIVE0 + i, drive_name(&d[i]));
		char sz[48];

		SIZE(d[i].size_bytes, sz);
		/* the filesystem ("exfat") and the size: no words to translate */
		tfmt(it->value, sizeof(it->value), "%s, %s", d[i].fstype, sz);
	}
	m->ctx = (void *)(intptr_t)action;
	m->on_item = drives_item;
	menu_open(ui, m);
}


void transfer_eject_all(struct ui *ui)
{
	struct transfer_usb_drive d[8];
	int n, bad = 0;

	if (!TR(ui))
		return;
	n = TR(ui)->usb_drives(d, 8);
	if (!n) {
		/* TRANSLATORS: toast, short (Eject USB drive with none plugged in) */
		ui_toastf(ui, "%s", _("No USB drive"));
		return;
	}
	for (int i = 0; i < n; i++)
		if (TR(ui)->usb_eject(d[i].mountpoint) < 0)
			bad++;
	LOGI("usb: eject all: %d drive%s, %d busy", n, n == 1 ? "" : "s", bad);
	if (bad)
		/* TRANSLATORS: toast, short */
		ui_toastf(ui, "%s", _("A USB drive is busy"));
	else
		ui_toast_long(ui, "%s", _("USB drive ejected: you can unplug it"));
}

/* --------------------------------------------------------------- web share */
struct webui {
	struct screen base;
	struct webshare_status st;
	struct qr_code qr;
	char qr_for[96];
	bool qr_ok;
	int64_t next_poll, next_changes;
	char err[256];                    /* translated */
};

/* ------------------------------------------------ Windows file share */
/*
 * \\RETROSTONE (docs/rom-transfer.md §3.3): the in-kernel ksmbd server,
 * started by the rsos-smb helper next to the network transfer when Settings
 * > Network > Windows file share (smb) is on, with its own password for user
 * "retrostone" (g_smb_pass, new at every start, shown on the transfer
 * screen), and stopped with it (before a game, when the network goes off,
 * or on STOP).
 *
 * One helper job at a time (review: a stop could run while the start was
 * still writing the configuration): a start asked during a stop, or a stop
 * during a start, runs when that job ends (smb_job_done), from smb_state
 * (1 = wanted on, 0 = wanted off).
 */
static int g_smb_job;               /* the helper job running: 0, UI_JOB_SMB, UI_JOB_SMB_STOP */
static char g_smb_pass[10];         /* "XXXX-XXXX" */

bool smb_available(struct ui *ui)
{
	return ui->cfg.smb_helper && *ui->cfg.smb_helper && access(ui->cfg.smb_helper, X_OK) == 0;
}

/* 8 characters without 0/O, 1/I/L (31^8, about 2^39), "-" in the middle:
 * typed on a PC from the console screen. The web PIN (6 digits, locked out
 * after 5 tries) is too short for SMB, which has no real lockout. */
static bool smb_make_password(char out[10])
{
	static const char abc[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
	unsigned char b[64];
	FILE *f = fopen("/dev/urandom", "rb");
	size_t n = f ? fread(b, 1, sizeof(b), f) : 0;
	int k = 0;

	if (f)
		fclose(f);
	for (size_t i = 0; i < n && k < 9; i++) {
		if (b[i] >= 248)                        /* 248 = 31 * 8: uniform */
			continue;
		if (k == 4)
			out[k++] = '-';
		out[k++] = abc[b[i] % 31];
	}
	out[k] = 0;
	return k == 9;
}

static void smb_run_start(struct ui *ui)
{
	const char *argv[5];

	if (!smb_make_password(g_smb_pass)) {
		ui->smb_state = -1;
		return;
	}
	argv[0] = ui->cfg.smb_helper;
	argv[1] = "start";
	argv[2] = g_smb_pass;
	argv[3] = settings_get(ui->settings, "hostname", "retrostone");
	argv[4] = NULL;
	if (hw_job_start(argv, UI_JOB_SMB) > 0) {
		g_smb_job = UI_JOB_SMB;
		ui->smb_state = 1;
		LOGI("smb: starting the Windows file share");
	} else {
		ui->smb_state = -1;
	}
}

static void smb_run_stop(struct ui *ui)
{
	const char *argv[] = { ui->cfg.smb_helper, "stop", NULL };

	if (hw_job_start(argv, UI_JOB_SMB_STOP) > 0) {
		g_smb_job = UI_JOB_SMB_STOP;
		LOGI("smb: stopping the Windows file share");
	}
}

void smb_start(struct ui *ui)
{
	if (!TR(ui) || !smb_available(ui) || ui->smb_state == 1 || ui->smb_state == 2 ||
	    !settings_get_bool(ui->settings, "smb", false) || !TR(ui)->webshare_running())
		return;
	if (g_smb_job) {
		ui->smb_state = 1;        /* after the stop that runs (smb_job_done) */
		return;
	}
	smb_run_start(ui);
}

void smb_stop(struct ui *ui)
{
	static bool checked;

	if (ui->smb_state == 0) {
		/* Once: a share left by a menu that crashed (it restarts with
		 * smb_state 0) is stopped, not left running unseen. */
		const char *run = getenv("RSOS_SMB_RUN"), *mod = getenv("RSOS_SMB_SYSMOD");

		if (checked || g_smb_job)
			return;
		checked = true;
		if (!smb_available(ui) || (access(run ? run : "/run/ksmbd", F_OK) != 0 &&
					   access(mod ? mod : "/sys/module/ksmbd", F_OK) != 0))
			return;
		LOGW("smb: a Windows file share was left running: stopping it");
	} else if (!smb_available(ui)) {
		return;
	}
	checked = true;
	ui->smb_state = 0;
	g_smb_pass[0] = 0;
	if (!g_smb_job)
		smb_run_stop(ui);
	/* else: a start runs, the stop follows it (smb_job_done) */
}

void smb_job_done(struct ui *ui, const struct hw_job_result *r)
{
	g_smb_job = 0;
	if (r->tag == UI_JOB_SMB_STOP) {
		if (r->status != 0)
			LOGW("smb: stop: %s", r->out);
		if (ui->smb_state == 1) {          /* started again meanwhile */
			ui->smb_state = 0;
			smb_start(ui);
		}
		return;
	}
	if (ui->smb_state != 1) {
		smb_run_stop(ui);                  /* stopped meanwhile */
		return;
	}
	if (r->status == 0) {
		ui->smb_state = 2;
		LOGI("smb: the Windows file share is on");
	} else {
		ui->smb_state = -1;
		LOGW("smb: start failed (%d): %s", r->status, r->out);
		/* TRANSLATORS: toast; %s is the helper's own message (English) */
		ui_toastf(ui, _("Windows file share: %s"), r->out[0] ? r->out : _("failed"));
	}
}

static void webshare_stop_all(struct ui *ui)
{
	if (!TR(ui))
		return;
	if (TR(ui)->webshare_running())
		TR(ui)->webshare_stop();
	TR(ui)->netnames_stop();
	smb_stop(ui);
}

/* Before a game, and when the network goes off (docs/rom-transfer.md §4.3). */

void transfer_before_game(struct ui *ui)
{
	if (!TR(ui))
		return;
	if (TR(ui)->webshare_running())
		LOGI("transfer: stopping the web share for the game");
	/* the name responder too, also after the share stopped by itself
	 * (review F-L3: it kept running during games) */
	webshare_stop_all(ui);
}


void transfer_network_off(struct ui *ui)
{
	if (!TR(ui))
		return;
	if (TR(ui)->webshare_running()) {
		webshare_stop_all(ui);
		/* TRANSLATORS: toast */
		ui_toastf(ui, "%s", _("Network off: web transfer stopped"));
	} else {
		TR(ui)->netnames_stop();
	}
}

/* Polled from ui_update while the web share runs in the background. */

void transfer_poll(struct ui *ui)
{
	char changed[TRANSFER_SYS_MAX][TRANSFER_SYSID_MAX];

	if (!TR(ui))
		return;
	/* scans whose screen was closed: freed when they end */
	if (ui->scan_orphan & ORPHAN_PLAN) {
		struct transfer_plan *p = NULL;

		if (TR(ui)->plan_poll(&p) != 0) {
			if (p)
				TR(ui)->plan_free(p);
			ui->scan_orphan &= ~(unsigned)ORPHAN_PLAN;
		}
	}
	if (ui->scan_orphan & ORPHAN_TREES) {
		static struct transfer_tree t[TRANSFER_TREES_MAX];
		int n;

		if (TR(ui)->trees_poll(t, TRANSFER_TREES_MAX, &n) != 0)
			ui->scan_orphan &= ~(unsigned)ORPHAN_TREES;
	}
	if (ui->scan_orphan & ORPHAN_BACKUP) {
		struct transfer_backup *b = NULL;

		if (TR(ui)->backup_scan_poll(&b) != 0) {
			if (b)
				TR(ui)->backup_free(b);
			ui->scan_orphan &= ~(unsigned)ORPHAN_BACKUP;
		}
	}
	offer_pending(ui);
	/* a copy that waits for an answer, or has ended, with no progress screen
	 * (a reload closed it): reopen it, it asks / finishes and remounts */
	if (ui->loaded && !ui->in_game) {
		struct transfer_progress pr;
		int j = job_running(ui);
		bool shown = false;

		for (int i = 0; i < ui->nstack; i++)
			shown |= ui->stack[i]->ops == &pr_ops;
		if (j && !shown) {
			enum transfer_state st = j == 1 ? TR(ui)->import_status(&pr) : TR(ui)->backup_status(&pr);

			if (st >= TRANSFER_DONE || pr.asking) {
				LOGI("usb: reopening the progress screen of the running copy");
				progress_open(ui, j == 1 ? 0 : g_backup_kind, g_job_mp, g_job_label);
			}
		}
	}
	if (ui->now >= ui->next_share_poll) {
		ui->next_share_poll = ui->now + 2000;
		/* after an idle stop too: its last changes, and the name responder
		 * goes with the share (review F-L3) */
		invalidate(ui, changed, TR(ui)->webshare_take_changes(changed, TRANSFER_SYS_MAX));
		if (!TR(ui)->webshare_running()) {
			TR(ui)->netnames_stop();
			smb_stop(ui);        /* the Windows share goes with the transfer */
		}
	}
}

static void web_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct webui *w = (struct webui *)scr;
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	struct font *fs = font_get(ms->font_path, ui_font_px(ui, ms->small_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : NULL,
				   ui_font_px(ui, ms->font_size * 1.15f));
	int W = ui->w, H = ui->h, pw = W * 92 / 100, ph = H * 84 / 100;
	int x = (W - pw) / 2, y = H * 3 / 100, pad = H / 40, lh = font_height(f) * 2;
	char buf[320];
	/* TRANSLATORS: help bar: A stops the network transfer, B goes back */
	static const struct help_prompt prompts[] = { { "a", N_("stop") }, { "b", N_("back") } };

	draw_panel(s, x, y, pw, ph, MAX(4, H / 60), ms->bg);
	/* TRANSLATORS: screen title (the Settings > Network row of the same name) */
	draw_text_box(ui, s, fb, _("Transfer over network"), x, y + pad / 2, pw, lh, AL_CENTER, ms->title);
	if (w->err[0]) {
		draw_text_box(ui, s, f, w->err, x + pad, y + ph / 2 - lh, pw - 2 * pad, lh, AL_CENTER,
			      ms->text);
		help_draw(ui, s, &ui->menu_help, prompts + 1, 1);
		return;
	}
	/* QR code: 4-module light border */
	{
		int qs = MIN(ph - lh * 3, pw / 2 - 2 * pad);
		int mods = w->qr_ok ? w->qr.size + 8 : 0;
		int px = mods ? MAX(1, qs / mods) : 0;
		int qx = x + pad * 2, qy = y + lh + pad;

		if (w->qr_ok) {
			gfx_fill(s, qx, qy, px * mods, px * mods, 0xffffffffu);
			for (int j = 0; j < w->qr.size; j++)
				for (int i = 0; i < w->qr.size; i++)
					if (w->qr.m[j][i])
						gfx_fill(s, qx + (i + 4) * px, qy + (j + 4) * px, px, px,
							 0xff000000u);
		}
		/* text column */
		{
			int tx = qx + px * mods + pad * 2, tw = x + pw - pad * 2 - tx, ty = qy;

			lh = font_height(f) * 17 / 10;
			/* TRANSLATORS: above the web address (URL) to type in a computer's browser */
			draw_text_box(ui, s, fs, _("Open this address in a browser:"), tx, ty, tw, lh,
				      AL_LEFT, ms->text_dim);
			ty += lh * 3 / 4;
			draw_text_box(ui, s, fb, w->st.nurls ? w->st.urls[0] : _("(no network address)"),
				      tx, ty, tw, lh, AL_LEFT, ms->accent);
			ty += lh;
			if (w->st.mdns_url[0]) {
				/* TRANSLATORS: a second web address ("or http://retrostone.local") */
				tfmt(buf, sizeof(buf), _("or %s"), w->st.mdns_url);
				draw_text_box(ui, s, fs, buf, tx, ty, tw, lh, AL_LEFT, ms->text_dim);
				ty += lh;
			}
			/* the Windows file share, when it is on */
			if (ui->smb_state == 1 || ui->smb_state == 2) {
				char host[40], path[48];
				const char *h = settings_get(ui->settings, "hostname", "retrostone");
				size_t k = 0;

				for (; *h && k + 1 < sizeof(host) && k < 15; h++)
					host[k++] = (char)(*h >= 'a' && *h <= 'z' ? *h - 32 : *h);
				host[k] = 0;
				snprintf(path, sizeof(path), "\\\\%s", host);
				if (ui->smb_state == 2)
					/* TRANSLATORS: the Windows file share's address (\\RETROSTONE) */
					tfmt(buf, sizeof(buf), _("Windows: %s"), path);
				else
					/* TRANSLATORS: the Windows file share is starting */
					tfmt(buf, sizeof(buf), _("Windows: %s (starting...)"), path);
				draw_text_box(ui, s, fs, buf, tx, ty, tw, lh, AL_LEFT, ms->text_dim);
				ty += lh;
				if (ui->smb_state == 2) {
					/* TRANSLATORS: the Windows file share's login, under its
					 * address; keep "retrostone" (the user name to type) */
					tfmt(buf, sizeof(buf), _("User: retrostone, Password: %s"), g_smb_pass);
					draw_text_box(ui, s, fs, buf, tx, ty, tw, lh, AL_LEFT, ms->text);
					ty += lh;
				}
			}
			/* TRANSLATORS: label above the 4-digit code the web page asks for, short */
			draw_text_box(ui, s, fs, _("PIN"), tx, ty, tw, lh, AL_LEFT, ms->text_dim);
			ty += lh * 3 / 4;
			draw_text_box(ui, s, fb, w->st.pin, tx, ty, tw, lh, AL_LEFT, ms->title);
			ty += lh * 5 / 4;
			if (w->st.lockout_s > 0)
				/* TRANSLATORS: status; %d is a number of seconds */
				tfmt(buf, sizeof(buf), _n("Wrong PIN entered: locked for %d s",
							  "Wrong PIN entered: locked for %d s", w->st.lockout_s),
				     w->st.lockout_s);
			else if (w->st.current[0])
				/* TRANSLATORS: status: a file name, then how much of it arrived ("(40%)") */
				tfmt(buf, sizeof(buf), _("Receiving %s (%d%%)"), w->st.current,
				     w->st.cur_total ? (int)(w->st.cur_done * 100 / w->st.cur_total) : 0);
			else if (!w->st.running && w->st.stop_reason[0])
				/* TRANSLATORS: status: the transfer stopped by itself, %s is why ("idle") */
				tfmt(buf, sizeof(buf), _("Stopped: %s"),
				     C_("webshare stop reason", w->st.stop_reason));
			else if (!w->st.running)
				/* TRANSLATORS: status: the network transfer is stopped */
				tcpy(buf, _("Stopped"), sizeof(buf));
			else
				/* TRANSLATORS: status: the network transfer runs, nothing arrives yet */
				tcpy(buf, _("Waiting for files..."), sizeof(buf));
			draw_text_box(ui, s, fs, buf, tx, ty, tw, lh, AL_LEFT, ms->text);
			ty += lh * 3 / 4;
			tfmt(buf, sizeof(buf), _n("%d file received", "%d files received", w->st.files_received),
			     w->st.files_received);
			draw_text_box(ui, s, fs, buf, tx, ty, tw, lh, AL_LEFT, ms->text_dim);
		}
	}
	draw_text_box(ui, s, fs, _("Only on your home network. Anyone with the PIN can add files."),
		      x + pad, y + ph - font_height(fs) * 2, pw - 2 * pad, font_height(fs) * 2,
		      AL_CENTER, ms->text_dim);
	help_draw(ui, s, &ui->menu_help, prompts, 2);
}

static void web_poll(struct ui *ui, struct webui *w)
{
	TR(ui)->webshare_get_status(&w->st);
	if (strcmp(w->qr_for, w->st.qr_text)) {
		strlcpy_(w->qr_for, w->st.qr_text, sizeof(w->qr_for));
		w->qr_ok = w->st.qr_text[0] && TR(ui)->qr_encode &&
			   TR(ui)->qr_encode(w->st.qr_text, &w->qr) == 0;
	}
}

static bool web_update(struct ui *ui, struct screen *scr)
{
	struct webui *w = (struct webui *)scr;

	if (w->err[0] || ui->now < w->next_poll)
		return false;
	w->next_poll = ui->now + 500;
	web_poll(ui, w);
	transfer_poll(ui);
	return true;
}

static int web_timeout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	return ((struct webui *)scr)->err[0] ? -1 : 500;
}

static void web_leave(struct ui *ui)
{
	ui_pop(ui);
	if (ui->reload_pending) {
		ui->reload_pending = false;
		ui_reload_games(ui, false);
	}
}

static void bg_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice != 1)
		webshare_stop_all(ui);
	web_leave(ui);
}

static void web_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct webui *w = (struct webui *)scr;
	/* TRANSLATORS: dialog buttons, uppercase, short: stop the network transfer / keep it running */
	const char *const buttons[] = { _("STOP"), _("KEEP RECEIVING"), NULL };

	if (t != IN_NAV_PRESS)
		return;
	if (b == IN_A && !w->err[0]) {
		webshare_stop_all(ui);
		web_leave(ui);
	} else if (b == IN_B) {
		if (w->err[0] || !TR(ui)->webshare_running() ||
		    settings_get_bool(ui->settings, "webshare_bg", false))
			web_leave(ui);
		else
			dialog_open(ui, _("Keep receiving files in the background?"), buttons, bg_choice, NULL);
	}
}

static const struct screen_ops web_ops = {
	.button = web_button,
	.update = web_update,
	.render = web_render,
	.relayout = pr_relayout,
	.destroy = pr_destroy,
	.timeout = web_timeout,
	.opaque = false,
};


void webshare_open(struct ui *ui)
{
	struct webui *w;
	struct webshare_config wc;
	struct netnames_config nc;
	char ip[64];
	const char *host = settings_get(ui->settings, "hostname", "retrostone");
	int r;

	if (!TR(ui)) {
		message_open(ui, _("Network transfer is not available in this build."));
		return;
	}
	w = xcalloc(1, sizeof(*w));
	w->base.ops = &web_ops;
	if (!TR(ui)->webshare_running()) {
		if (!hw_has_ipv4(ip, sizeof(ip))) {
			tcpy(w->err, _("Turn on WiFi or Ethernet first (no network address)."), sizeof(w->err));
		} else {
			memset(&wc, 0, sizeof(wc));
			wc.data_root = data_root(ui);
			wc.hostname = host;
			wc.idle_timeout_s = settings_get_int(ui->settings, "webshare_idle_min", 30) * 60;
			r = TR(ui)->webshare_start(&wc);
			if (r < 0) {
				tfmt(w->err, sizeof(w->err), _("The transfer could not start (error %d)."), -r);
			} else {
				TR(ui)->netnames_defaults(&nc);
				nc.hostname = host;
				if (TR(ui)->netnames_start(&nc) < 0)
					LOGW("transfer: the name responder did not start");
			}
		}
	}
	if (!w->err[0]) {
		web_poll(ui, w);
		smb_start(ui);       /* Settings > Network > Windows file share */
	}
	ui_push(ui, &w->base);
}
