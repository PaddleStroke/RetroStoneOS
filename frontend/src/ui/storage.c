/*
 * storage.c - the RetroStone2 "Pro" storage in Settings > Storage, through
 * the system helper rsos-storage (ui->cfg.storage_helper;
 * board/retrostone2/rootfs-overlay/usr/bin/rsos-storage):
 *   - the Internal eMMC and M.2 SATA switches (U-Boot's rsos_overlays);
 *   - "Install to internal storage": RetroStoneOS copied to the eMMC, then
 *     the console starts without the microSD;
 *   - started from the eMMC: the microSD turned into a games card, or erased
 *     for games; the M.2 drive prepared for games;
 *   - the game libraries of the other disks (/run/rsos/libraries), merged
 *     into the game lists by loader.c, and the hotplug of the microSD and
 *     the M.2 drive (the helper mounts them).
 * The long tasks run as helper jobs (hw_job_start) behind a full-screen
 * progress page that reads /run/rsos/storage-progress ("<percent> <step>").
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ui_internal.h"

/* in /run/rsos (RSOS_RUN overrides it, as for rsos-storage: previews, tests) */
#define ST_STATUS S.p_status
#define ST_PROGRESS S.p_progress
#define ST_LIBS S.p_libs
/* /sys/block (a card or a drive in or out): slowly, so the idle menu sleeps;
 * Settings > Storage starts its own refresh when it opens */
#define ST_POLL_MS 10000
/* RSOS_COVERS_HELPER overrides it (previews, tests) */
#define COVERS_HELPER S.p_covers
#define COVERS_MISSING S.p_missing

enum {
	ID_ST_EMMC = 500, ID_ST_INSTALL, ID_ST_BOOTINFO, ID_ST_SATA, ID_ST_M2, ID_ST_M2FORMAT,
	ID_ST_SD, ID_ST_SDGAMES, ID_ST_SDFORMAT, ID_ST_SDWARN, ID_ST_SDSTARTER, ID_ST_SDFULL, ID_ST_MOVE, ID_ST_LIB0 = 520,
	ID_ST_MODEL = 523, ID_ST_REDETECT,
};

enum { TASK_INSTALL = 1, TASK_INSTALL_KEEP, TASK_SDGAMES, TASK_FORMAT_SD, TASK_FORMAT_M2, TASK_SDSTARTER, TASK_COVERS, TASK_SDFULL };
enum { TS_RUNNING = 0, TS_DONE, TS_FAILED };

struct st_state {
	char boot[8];                /* sd, emmc, other */
	char model[12];              /* pro, regular ("" = not probed yet; boot.cmd) */
	bool emmc;
	int emmc_mb;
	char emmc_state[16];         /* rsos games bootable other empty ("" = the system disk) */
	bool sd_known, sd;
	int sd_mb;
	char sd_state[16];
	bool sata;                   /* the controller is enabled (overlay applied) */
	bool m2;
	int m2_mb;
	char m2_state[16];
	char overlays[128];
	bool starter;                /* started by a starter card (system on the eMMC) */
	bool emmc_ctrl;              /* the eMMC controller is enabled (overlay applied) */
	char lib[3][128];            /* emmc, sd, m2 mount points ("" = none) */
};

static struct {
	bool on;
	const char *helper;
	struct st_state st;
	int64_t next_poll;
	char sig[512];               /* the block devices last seen */
	char libs[512];              /* /run/rsos/libraries as last applied */
	int refresh_job;             /* a "refresh" runs */
	bool overlay_job;
	/* the task page */
	struct screen *task;
	int kind, state, pct, job, sel;
	char step[24], reason[24];
	int d1, d2, d3;              /* covers: done, total / found, missing, failed */
	char dsys[32];
	char p_status[256], p_progress[256], p_libs[256], p_request[256];
	char p_covers[256], p_missing[1100];
} S;

static const char *const g_lib_names[3] = { "emmc", "sd", "m2" };

/* the status the Storage menu was built from (storage_menu_changed) */
static char g_menu_status[1024];
static void status_text(char *out, size_t n);
static void block_sig(char *out, size_t n);

/* --------------------------------------------------------------- state */
static void read_status(void)
{
	FILE *f = fopen(ST_STATUS, "r");
	char line[256];
	struct st_state st;

	if (!f)
		return;
	memset(&st, 0, sizeof(st));
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');

		if (!v)
			continue;
		*v++ = 0;
		v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(line, "boot"))
			strlcpy_(st.boot, v, sizeof(st.boot));
		else if (!strcmp(line, "model"))
			strlcpy_(st.model, v, sizeof(st.model));
		else if (!strcmp(line, "emmc"))
			st.emmc = !strcmp(v, "present");
		else if (!strcmp(line, "emmc_mb"))
			st.emmc_mb = atoi(v);
		else if (!strcmp(line, "emmc_state"))
			strlcpy_(st.emmc_state, v, sizeof(st.emmc_state));
		else if (!strcmp(line, "sd")) {
			st.sd_known = true;
			st.sd = !strcmp(v, "present");
		} else if (!strcmp(line, "sd_mb"))
			st.sd_mb = atoi(v);
		else if (!strcmp(line, "sd_state"))
			strlcpy_(st.sd_state, v, sizeof(st.sd_state));
		else if (!strcmp(line, "sata"))
			st.sata = !strcmp(v, "enabled");
		else if (!strcmp(line, "m2"))
			st.m2 = !strcmp(v, "present");
		else if (!strcmp(line, "m2_mb"))
			st.m2_mb = atoi(v);
		else if (!strcmp(line, "m2_state"))
			strlcpy_(st.m2_state, v, sizeof(st.m2_state));
		else if (!strcmp(line, "starter"))
			st.starter = !strcmp(v, "1");
		else if (!strcmp(line, "emmc_ctrl"))
			st.emmc_ctrl = !strcmp(v, "enabled");
		else if (!strcmp(line, "overlays"))
			strlcpy_(st.overlays, v, sizeof(st.overlays));
		else
			for (int i = 0; i < 3; i++)
				if (!strncmp(line, "lib_", 4) && !strcmp(line + 4, g_lib_names[i]))
					strlcpy_(st.lib[i], v, sizeof(st.lib[i]));
	}
	fclose(f);
	S.st = st;
}

static bool has_overlay(const char *name)
{
	char buf[128], *save = NULL, *t;

	strlcpy_(buf, S.st.overlays, sizeof(buf));
	for (t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save))
		if (!strcmp(t, name))
			return true;
	return false;
}

static bool booted_from_emmc(void)
{
	return !strcmp(S.st.boot, "emmc");
}

/* A regular RetroStone2: boot.cmd's probe found no eMMC, and nothing here
 * says otherwise (the system runs from it, or it answers). The internal
 * storage is then hidden; the M.2 slot stays. */
static bool model_regular(void)
{
	return !strcmp(S.st.model, "regular") && !booted_from_emmc() && !S.st.starter && !S.st.emmc;
}

const char *storage_model_name(void)
{
	if (!S.on)
		return NULL;
	if (!S.st.boot[0])
		read_status();    /* a refresh since the start may have written it */
	if (model_regular())
		return "RetroStone2";
	if (!strcmp(S.st.model, "pro") || booted_from_emmc() || S.st.starter || S.st.emmc)
		return "RetroStone2 Pro";
	return NULL;
}

static int job(const char *a1, const char *a2, const char *a3, int tag)
{
	const char *argv[] = { S.helper, a1, a2, a3, NULL };
	int id = hw_job_start(argv, tag);

	if (id < 0)
		LOGW("storage: cannot start %s %s: %s", S.helper, a1, strerror(-id));
	else
		LOGI("storage: %s %s%s%s%s", S.helper, a1, a2 ? " " : "", a2 ? a2 : "", a3 ? " ..." : "");
	return id;
}

bool storage_available(const struct ui *ui)
{
	(void)ui;
	return S.on;
}

void storage_init(struct ui *ui)
{
	const char *h = ui->cfg.storage_helper;

	const char *run = getenv("RSOS_RUN");

	memset(&S, 0, sizeof(S));
	if (!run || !*run)
		run = "/run/rsos";
	snprintf(S.p_status, sizeof(S.p_status), "%s/storage-status", run);
	snprintf(S.p_progress, sizeof(S.p_progress), "%s/storage-progress", run);
	snprintf(S.p_libs, sizeof(S.p_libs), "%s/libraries", run);
	snprintf(S.p_request, sizeof(S.p_request), "%s/covers-request.txt", run);
	snprintf(S.p_covers, sizeof(S.p_covers), "%s",
		 getenv("RSOS_COVERS_HELPER") ? getenv("RSOS_COVERS_HELPER") : "/usr/bin/rsos-covers");
	snprintf(S.p_missing, sizeof(S.p_missing), "%s/logs/covers-missing.txt", ui->cfg.data_dir);
	S.helper = h;
	S.on = h && *h && access(h, X_OK) == 0 && ui->cfg.storage_overlays &&
	       (strstr(ui->cfg.storage_overlays, "emmc") || strstr(ui->cfg.storage_overlays, "sata"));
	/* no "rsos-storage refresh" at start: the disks as they are now are the
	 * baseline (rsos-storage boot mounted the libraries, when there are
	 * other disks); a change, or Settings > Storage, starts one */
	if (S.on) {
		read_status();
		block_sig(S.sig, sizeof(S.sig));
	}
}

/* ----------------------------------------------------------- libraries */
/* The block devices that matter (the microSD, the eMMC, sd* disks): a
 * change starts "rsos-storage refresh" (mounts or unmounts the libraries). */
static void block_sig(char *out, size_t n)
{
	DIR *d = opendir("/sys/block");
	struct dirent *de;
	size_t len = 0;

	out[0] = 0;
	if (!d)
		return;
	while ((de = readdir(d)))
		if (!strncmp(de->d_name, "mmcblk", 6) || !strncmp(de->d_name, "sd", 2))
			len += (size_t)snprintf(out + len, len < n ? n - len : 0, "%s ", de->d_name);
	closedir(d);
}

static void apply_libraries(struct ui *ui)
{
	char buf[512] = "", *save = NULL, *line;
	const char *roots[3];
	char copy[512];
	int n = 0;
	FILE *f = fopen(ST_LIBS, "r");

	if (f) {
		size_t k = fread(buf, 1, sizeof(buf) - 1, f);

		buf[k] = 0;
		fclose(f);
	}
	if (!strcmp(buf, S.libs))
		return;
	strlcpy_(copy, buf, sizeof(copy));
	for (line = strtok_r(copy, "\n", &save); line && n < 3; line = strtok_r(NULL, "\n", &save)) {
		char *mp = strchr(line, ' ');

		if (mp && mp[1] == '/')
			roots[n++] = mp + 1;
	}
	if (loader_set_libraries(ui, roots, n))
		strlcpy_(S.libs, buf, sizeof(S.libs));
}

void storage_poll(struct ui *ui)
{
	char sig[512];

	if (!S.on || ui->now < S.next_poll)
		return;
	S.next_poll = ui->now + ST_POLL_MS;
	block_sig(sig, sizeof(sig));
	/* (not while a task runs: it refreshes at its end) */
	if (strcmp(sig, S.sig) && !S.refresh_job && !S.job) {
		int id;

		if (S.sig[0])
			LOGI("storage: disks changed (%s): refreshing the game libraries", sig);
		strlcpy_(S.sig, sig, sizeof(S.sig));
		id = job("refresh", NULL, NULL, UI_JOB_STORAGE);
		if (id > 0)
			S.refresh_job = id;
	}
	apply_libraries(ui);
}

void storage_page_opened(struct ui *ui)
{
	int id;

	(void)ui;
	if (!S.on || S.refresh_job || S.job)
		return;
	block_sig(S.sig, sizeof(S.sig));
	id = job("refresh", NULL, NULL, UI_JOB_STORAGE);
	if (id > 0)
		S.refresh_job = id;
}

/* ------------------------------------------------------------ the page */
static const char *step_text(const char *step)
{
	/* TRANSLATORS: storage task page, the step that runs */
	if (!strcmp(step, "system"))
		return _("Copying the system...");
	/* TRANSLATORS: storage task page, the step that runs */
	if (!strcmp(step, "verify"))
		return _("Checking the copy...");
	/* TRANSLATORS: storage task page, the step that runs */
	if (!strcmp(step, "data"))
		return _("Copying your settings and saves...");
	/* TRANSLATORS: storage task page, the step that runs */
	if (!strcmp(step, "games"))
		return _("Copying your games...");
	/* TRANSLATORS: storage task page, the step that runs */
	if (!strcmp(step, "bootloader"))
		return _("Writing the start-up files...");
	/* TRANSLATORS: storage task page, the step that runs */
	if (!strcmp(step, "covers"))
		return _("Downloading game covers...");
	/* TRANSLATORS: storage task page, the step that runs */
	return _("Preparing...");
}

static const char *reason_text(const char *r)
{
	if (!strcmp(r, "noemmc"))
		/* TRANSLATORS: storage task failed */
		return _("The internal storage was not found. Turn on Internal eMMC in Settings > Storage and "
			 "restart the console, then try again.");
	if (!strcmp(r, "nospace"))
		/* TRANSLATORS: storage task failed */
		return _("There is not enough room on the internal storage. Try again without copying the games.");
	if (!strcmp(r, "toosmall"))
		/* TRANSLATORS: storage task failed */
		return _("The internal storage is too small.");
	if (!strcmp(r, "verify") || !strcmp(r, "write") || !strcmp(r, "bootloader"))
		/* TRANSLATORS: storage task failed (write or read-back error) */
		return _("Writing to the disk failed: it may be faulty. The microSD card still works as before.");
	if (!strcmp(r, "busy"))
		/* TRANSLATORS: storage task failed */
		return _("The disk is in use. Restart the console and try again.");
	if (!strcmp(r, "network"))
		/* TRANSLATORS: game covers failed */
		return _("The cover server could not be reached. Check the WiFi connection (Settings > Network) "
			 "and try again later.");
	if (!strcmp(r, "clock") || !strcmp(r, "tls"))
		/* TRANSLATORS: game covers failed: HTTPS needs the right date */
		return _("The secure connection failed. Check the date and time (Settings > Date & time), then "
			 "try again.");
	if (!strcmp(r, "nospace"))
		/* TRANSLATORS: game covers failed */
		return _("The storage is full.");
	if (!strcmp(r, "notstarter"))
		/* TRANSLATORS: storage task failed */
		return _("This only works while the console is started by its SD starter card.");
	if (!strcmp(r, "notrsos"))
		/* TRANSLATORS: storage task failed */
		return _("This SD card does not hold RetroStoneOS.");
	if (!strcmp(r, "oldbootloader"))
		/* TRANSLATORS: storage task failed: the card's start-up files are from an older version */
		return _("This SD card's start-up files are too old to start the internal storage. Flash a new "
			 "RetroStoneOS card first, then try again. Nothing was changed.");
	/* TRANSLATORS: storage task failed (the reason is in the log) */
	return _("Something went wrong. Nothing was changed on the microSD card.");
}

static const char *task_title(void)
{
	switch (S.kind) {
	case TASK_INSTALL:
	case TASK_INSTALL_KEEP:
		/* TRANSLATORS: title of the page while RetroStoneOS is copied to the eMMC */
		return _("Install to internal storage");
	case TASK_SDGAMES:
		/* TRANSLATORS: title: the microSD becomes a card for games only */
		return _("SD card for games");
	case TASK_FORMAT_SD:
		/* TRANSLATORS: title */
		return _("Erase the SD card");
	case TASK_SDSTARTER:
		/* TRANSLATORS: title: the microSD keeps only the start-up files and the games */
		return _("SD starter card");
	case TASK_COVERS:
		/* TRANSLATORS: title: pictures of the game boxes, downloaded */
		return _("Game covers");
	case TASK_SDFULL:
		/* TRANSLATORS: title: the starter card becomes a full system again */
		return _("SD card system");
	default:
		/* TRANSLATORS: title */
		return _("Prepare the M.2 drive");
	}
}

static void task_text(char *out, size_t n)
{
	if (S.state == TS_RUNNING && S.kind == TASK_COVERS) {
		snprintf(out, n, "%s %d %%", step_text(S.step), S.pct);
		if (S.d2 > 0) {
			size_t l = strlen(out);

			/* TRANSLATORS: game covers: "120 of 300 games" */
			snprintf(out + l, n - l, "\n\n");
			l = strlen(out);
			snprintf(out + l, n - l, _("%d of %d games"), S.d1, S.d2);
		}
		return;
	}
	if (S.state == TS_RUNNING) {
		/* TRANSLATORS: storage task page: the step, then a warning */
		snprintf(out, n, "%s %d %%\n\n%s", step_text(S.step), S.pct,
			 _("Do not turn the console off."));
		return;
	}
	if (S.state == TS_FAILED) {
		strlcpy_(out, reason_text(S.reason), n);
		return;
	}
	switch (S.kind) {
	case TASK_INSTALL:
	case TASK_INSTALL_KEEP:
		/* TRANSLATORS: after the install to the eMMC: the starter card question */
		strlcpy_(out, _("RetroStoneOS is now on the internal storage.\n\nThe RetroStone2's processor cannot "
				"start from this internal storage by itself. Use this SD card as the starter card: it "
				"keeps its games and the start-up files, and the console then runs from the internal "
				"storage (keep the card in the slot)."), n);
		break;
	case TASK_SDFULL:
		/* TRANSLATORS: done: the starter card holds a full system again */
		strlcpy_(out, _("The SD card holds a full system again, with your current settings and saves. "
				"From the next start the console runs from the SD card; the internal storage keeps "
				"RetroStoneOS and its games appear as extra games."), n);
		break;
	case TASK_SDSTARTER:
		/* TRANSLATORS: done: the microSD is the starter card now */
		strlcpy_(out, _("This SD card is now the starter card. Keep it in the slot: from the next start, the "
				"console runs from the internal storage, and the card's games appear with the "
				"others."), n);
		break;
	case TASK_COVERS: {
		size_t l;

		/* TRANSLATORS: game covers done: how many were found */
		snprintf(out, n, _("Covers found for %d games."), S.d1);
		if (S.d2 > 0) {
			l = strlen(out);
			snprintf(out + l, n - l, "\n\n");
			l = strlen(out);
			/* TRANSLATORS: game covers done: those not found, and where their list is */
			snprintf(out + l, n - l, _("%d not found (their file names do not match the libretro names; "
						   "arcade games need Skraper). The list is in "
						   "rsos/logs/covers-missing.txt."), S.d2);
		}
		if (S.d3 > 0) {
			l = strlen(out);
			snprintf(out + l, n - l, "\n");
			l = strlen(out);
			/* TRANSLATORS: game covers done */
			snprintf(out + l, n - l, _("%d could not be downloaded: try again later."), S.d3);
		}
		break;
	}
	case TASK_SDGAMES:
		/* TRANSLATORS: done: the microSD only holds games now */
		strlcpy_(out, _("The SD card is now a games card: its games appear with the others, and the "
				"console keeps starting from the internal storage."), n);
		break;
	default:
		/* TRANSLATORS: done: an erased disk ready for games */
		strlcpy_(out, _("Done. Copy games to its roms folder from a PC, or with Import games."), n);
		break;
	}
}

static void read_progress(void)
{
	FILE *f = fopen(ST_PROGRESS, "r");
	char a[24] = "", b[24] = "";

	if (!f)
		return;
	if (fscanf(f, "%23s %23s", a, b) == 2) {
		if (!strcmp(a, "error"))
			strlcpy_(S.reason, b, sizeof(S.reason));
		else {
			int x = 0, y = 0;
			char z[32] = "";

			S.pct = CLAMP(atoi(a), 0, 100);
			strlcpy_(S.step, b, sizeof(S.step));
			/* rsos-covers: "covers <done> <total> <system>",
			 * "done <found> <missing> <failed>" */
			if (fscanf(f, "%d %d %31s", &x, &y, z) >= 2) {
				S.d1 = x;
				S.d2 = y;
				S.d3 = atoi(z);
				strlcpy_(S.dsys, z, sizeof(S.dsys));
			}
		}
	}
	fclose(f);
}

/* The page's buttons once the task stopped; returns how many (1 or 2). */
static int done_buttons(const char **b0, const char **b1)
{
	*b1 = NULL;
	/* TRANSLATORS: storage task page button */
	*b0 = _("OK");
	if (S.state != TS_DONE)
		return 1;
	if (S.kind == TASK_INSTALL || S.kind == TASK_INSTALL_KEEP) {
		/* TRANSLATORS: install done: make the microSD the starter card */
		*b0 = _("Use as starter card");
		/* TRANSLATORS: install done: leave the microSD as it is */
		*b1 = _("Keep SD as it is");
		return 2;
	}
	if (S.kind == TASK_SDSTARTER || S.kind == TASK_SDFULL) {
		/* TRANSLATORS: storage task page button */
		*b0 = _("Restart now");
		/* TRANSLATORS: storage task page button: back to the menu */
		*b1 = _("Later");
		return 2;
	}
	return 1;
}

static void task_start(struct ui *ui, int kind);
static void sdstarter_ask(struct ui *ui);

static void task_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : NULL, ui_font_px(ui, ms->font_size * 1.15f));
	int W = ui->w, H = ui->h, pad = MAX(8, H / 30), w = W - 4 * pad, x = 2 * pad, y = pad * 2;
	int lh = font_height(f) * 3 / 2, bh = font_height(f) * 2, tlh = font_height(f) * 5 / 4;
	/* the buttons sit above the help bar, whatever the text's length */
	int by = H - bh - font_height(f) * 2 - pad;
	char text[1024];
	struct text_line lines[16];
	int n;
	static const struct help_prompt prompts[] = { { "leftright", N_("choose") }, { "a", N_("select") } };

	(void)scr;
	gfx_fill(s, 0, 0, W, H, 0xff000000u);
	gfx_fill(s, 0, 0, W, H, ms->bg);
	draw_text_box(ui, s, fb, task_title(), x, y, w, lh, AL_CENTER, ms->title);
	y += lh * 3 / 2;
	if (S.state == TS_RUNNING) {
		int bw = w - 2 * pad, fill = bw * S.pct / 100;

		gfx_fill_round(s, x + pad, y, bw, bh / 2, bh / 4, gfx_with_alpha(ms->text_dim, 60));
		if (fill > 0)
			gfx_fill_round(s, x + pad, y, MAX(fill, bh / 2), bh / 2, bh / 4, ms->selector);
		y += bh;
	}
	task_text(text, sizeof(text));
	n = font_wrap(f, text, w, lines, 16);
	for (int i = 0; i < n; i++)
		font_draw(s, f, x + (w - lines[i].width) / 2, font_baseline_in_box(f, y + i * tlh, tlh),
			  text + lines[i].start, lines[i].len, ms->text);
	y += MAX(n, 1) * tlh + pad;
	if (S.state == TS_RUNNING && S.kind == TASK_COVERS) {
		static const struct help_prompt bg[] = { { "b", N_("continue in the background") } };

		help_draw(ui, s, &ui->menu_help, bg, 1);
		return;
	}
	if (S.state == TS_RUNNING)
		return;
	y = MAX(y, MIN(by, H - bh - pad));
	{
		const char *b0, *b1;
		int nb = done_buttons(&b0, &b1);

		for (int i = 0; i < nb; i++) {
			bool sel = i == S.sel;
			int bx = x + pad + i * ((w - 2 * pad) / nb), bwid = (w - 2 * pad) / nb - pad / 2;

			gfx_fill_round(s, bx, y, bwid, bh, bh / 4, sel ? ms->selector : gfx_with_alpha(ms->text_dim, 50));
			draw_text_box(ui, s, f, i ? b1 : b0, bx, y, bwid, bh, AL_CENTER, sel ? ms->sel_text : ms->text);
		}
		help_draw(ui, s, &ui->menu_help, prompts, 2);
	}
}

static void task_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	const char *b0, *b1;
	bool two = done_buttons(&b0, &b1) == 2;

	(void)scr;
	if (t == IN_NAV_RELEASE)
		return;
	if (S.state == TS_RUNNING) {
		/* nothing stops a running task; the cover download can go on in
		 * the background (storage_job_done says when it is done) */
		if (S.kind == TASK_COVERS && b == IN_B && t == IN_NAV_PRESS) {
			LOGI("storage: covers continue in the background");
			ui_pop(ui);
			/* TRANSLATORS: toast: the cover download goes on while the player plays */
			ui_toastf(ui, "%s", _("Game covers: downloading in the background"));
		}
		return;
	}
	if (b == IN_LEFT || b == IN_RIGHT) {
		S.sel = two ? !S.sel : 0;
		ui->dirty = true;
	} else if (b == IN_A && t == IN_NAV_PRESS) {
		bool first = two && S.sel == 0;
		int kind = S.kind, state = S.state;

		ui_pop(ui);
		if (kind == TASK_COVERS && state == TS_DONE) {
			ui_reload_games(ui, true);    /* the scan caches knew no covers */
			return;
		}
		if (first && (kind == TASK_INSTALL || kind == TASK_INSTALL_KEEP)) {
			/* never without the confirmation (CANCEL first) */
			sdstarter_ask(ui);
		} else if (first && (kind == TASK_SDSTARTER || kind == TASK_SDFULL)) {
			LOGI("storage: starter card ready: restarting (system on the eMMC)");
			ui_power(ui, UI_REBOOT);
		}
	} else if (b == IN_B && t == IN_NAV_PRESS) {
		bool reload = S.kind == TASK_COVERS && S.state == TS_DONE;

		ui_pop(ui);
		if (reload)
			ui_reload_games(ui, true);
	}
}

static bool task_update(struct ui *ui, struct screen *scr)
{
	int pct = S.pct;
	char step[24];

	(void)ui;
	(void)scr;
	if (S.state != TS_RUNNING)
		return false;
	strlcpy_(step, S.step, sizeof(step));
	read_progress();
	return pct != S.pct || strcmp(step, S.step);
}

static int task_timeout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
	return S.state == TS_RUNNING ? 250 : -1;
}

static void task_relayout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
}

static void task_destroy(struct ui *ui, struct screen *scr)
{
	(void)ui;
	if (S.task == scr)
		S.task = NULL;
	free(scr);
}

static void task_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	static const char *const states[] = { "running", "done", "failed" };

	(void)ui;
	(void)scr;
	snprintf(buf, n, "storage-task:%d:%s %d %s %s", S.kind, states[S.state % 3], S.pct, S.step, S.reason);
}

static const struct screen_ops task_ops = {
	.button = task_button,
	.update = task_update,
	.render = task_render,
	.relayout = task_relayout,
	.destroy = task_destroy,
	.timeout = task_timeout,
	.opaque = true,
	.describe = task_describe,
};

static void task_page_open(struct ui *ui)
{
	struct screen *s;

	if (S.task)
		return;
	s = xcalloc(1, sizeof(*s));
	s->ops = &task_ops;
	if (ui_push(ui, s))
		S.task = s;
}

bool ui_storage_busy(const struct ui *ui)
{
	(void)ui;
	return S.job != 0;
}

static void task_start(struct ui *ui, int kind)
{
	const char *a1 = "install", *a2 = NULL, *a3 = NULL;
	int id;

	switch (kind) {
	case TASK_INSTALL:
		a2 = "--games";
		break;
	case TASK_INSTALL + 100:
		kind = TASK_INSTALL;    /* without the games */
		break;
	case TASK_INSTALL_KEEP:
		a2 = "--keep-data";
		break;
	case TASK_SDGAMES:
		a1 = "sd-games";
		break;
	case TASK_FORMAT_SD:
		a1 = "format";
		a2 = "sd";
		break;
	case TASK_SDSTARTER:
		a1 = "sd-starter";
		break;
	case TASK_SDFULL:
		a1 = "sd-full";
		break;
	default:
		a1 = "format";
		a2 = "m2";
		break;
	}
	if (S.job) {
		/* TRANSLATORS: a storage task or cover download is still running */
		message_open(ui, _("Please wait: another task is still running."));
		return;
	}
	unlink(ST_PROGRESS);
	if (kind == TASK_COVERS) {
		const char *argv[] = { COVERS_HELPER, "--progress", ST_PROGRESS, "--missing",
				       COVERS_MISSING, S.p_request, NULL };

		id = hw_job_start(argv, UI_JOB_STORAGE_TASK);
		LOGI("storage: %s (covers)", COVERS_HELPER);
	} else {
		id = job(a1, a2, a3, UI_JOB_STORAGE_TASK);
	}
	if (id < 0) {
		/* TRANSLATORS: the storage helper could not be started */
		message_open(ui, _("This could not start."));
		return;
	}
	S.kind = kind;
	S.job = id;
	S.state = TS_RUNNING;
	S.pct = 0;
	S.sel = 0;
	S.d1 = S.d2 = S.d3 = 0;
	S.step[0] = S.reason[0] = 0;
	task_page_open(ui);
}

void storage_job_done(struct ui *ui, const struct hw_job_result *r)
{
	if (r->tag == UI_JOB_STORAGE) {
		if (r->id == S.refresh_job)
			S.refresh_job = 0;
		if (S.overlay_job && r->status != 0)
			/* TRANSLATORS: Settings > Storage: the switch could not be saved */
			message_open(ui, _("The setting could not be saved."));
		S.overlay_job = false;
		read_status();
		apply_libraries(ui);
		ui->dirty = true;
		return;
	}
	if (r->tag != UI_JOB_STORAGE_TASK || r->id != S.job)
		return;
	S.job = 0;
	read_progress();
	read_status();
	if (r->status == 0) {
		S.state = TS_DONE;
		S.pct = 100;
		LOGI("storage: task %d done", S.kind);
	} else {
		S.state = TS_FAILED;
		if (!S.reason[0])
			strlcpy_(S.reason, "unknown", sizeof(S.reason));
		LOGW("storage: task %d failed (%d, %s): %s", S.kind, r->status, S.reason, r->out);
	}
	/* an install done: "Keep SD as it is" is preselected (the starter card
	 * asks again before it changes the card) */
	S.sel = S.state == TS_DONE && (S.kind == TASK_INSTALL || S.kind == TASK_INSTALL_KEEP) ? 1 : 0;
	apply_libraries(ui);
	if (S.kind == TASK_COVERS && !S.task) {
		char t[160];

		/* the lists take the covers at the next return to the carousel (no
		 * screen is pulled away); the scan caches knew no covers */
		for (int i = 0; i < systems_count(); i++)
			ui_invalidate_system(ui, systems_get(i)->name);
		ui->reload_pending = true;
		if (S.state == TS_DONE)
			/* TRANSLATORS: toast at the end of a cover download that ran in the background */
			snprintf(t, sizeof(t), _("Game covers: %d found, %d not found"), S.d1, S.d2);
		else
			snprintf(t, sizeof(t), "%s", reason_text(S.reason));
		ui_toast_long(ui, "%s", t);
	}
	ui->dirty = true;
}

/* ---------------------------------------------------------- the menu */
static void size_text(int mb, char *out, size_t n)
{
	i18n_format_size((uint64_t)mb * 1024 * 1024, out, n);
}

static const char *disk_state_text(const char *st)
{
	if (!strcmp(st, "rsos"))
		/* TRANSLATORS: what a disk holds (Settings > Storage) */
		return _("RetroStoneOS");
	if (!strcmp(st, "games"))
		/* TRANSLATORS: what a disk holds (Settings > Storage) */
		return _("games");
	if (!strcmp(st, "starter"))
		/* TRANSLATORS: what a disk holds: the start-up files and games (a starter card) */
		return _("starter + games");
	if (!strcmp(st, "bootable"))
		/* TRANSLATORS: what a disk holds: another system that can start the console */
		return _("another system");
	if (!strcmp(st, "empty"))
		/* TRANSLATORS: what a disk holds (Settings > Storage) */
		return _("empty");
	/* TRANSLATORS: what a disk holds: a format the console does not use */
	return _("not used");
}

void storage_menu_items(struct ui *ui, struct menu *m)
{
	struct menu_item *it;
	char a[32], b[96];
	bool emmc_ov = strstr(ui->cfg.storage_overlays, "emmc") != NULL;
	bool sata_ov = strstr(ui->cfg.storage_overlays, "sata") != NULL;
	const char *model;

	if (!S.on)
		return;
	read_status();
	status_text(g_menu_status, sizeof(g_menu_status));
	/* TRANSLATORS: Settings > Storage section header: which RetroStone2 this is */
	menu_add(m, MI_HEADER, 0, _("This console"));
	/* TRANSLATORS: Settings > Storage: RetroStone2 or RetroStone2 Pro (found at the first start) */
	it = menu_add(m, MI_INFO, ID_ST_MODEL, _("Model"));
	model = storage_model_name();
	/* TRANSLATORS: value of Model before the first start probed it */
	strlcpy_(it->value, model ? model : _("found at the next start"), sizeof(it->value));
	/* TRANSLATORS: Settings > Storage action: look for the eMMC again at the next start */
	menu_add(m, MI_ACTION, ID_ST_REDETECT, _("Detect the model again"));
	if (emmc_ov && !model_regular()) {
		/* TRANSLATORS: Settings > Storage section header: the Pro's built-in 16 GB eMMC */
		menu_add(m, MI_HEADER, 0, _("Internal storage"));
		if (booted_from_emmc()) {
			/* TRANSLATORS: Settings > Storage: the system runs from the eMMC */
			it = menu_add(m, MI_INFO, ID_ST_BOOTINFO, _("Started from"));
			size_text(S.st.emmc_mb, a, sizeof(a));
			if (S.st.starter)
				/* TRANSLATORS: Settings > Storage, Started from: the eMMC system, started by the SD starter card */
				strlcpy_(it->value, _("internal + SD starter card"), sizeof(it->value));
			else
				/* TRANSLATORS: "internal storage (15 GB)" */
				snprintf(it->value, sizeof(it->value), _("internal storage (%s)"), a);
		} else {
			/* TRANSLATORS: toggle: make the eMMC visible (needs a restart) */
			it = menu_add(m, MI_TOGGLE, ID_ST_EMMC, _("Internal eMMC"));
			it->on = has_overlay("emmc");
			it = menu_add(m, MI_ACTION, ID_ST_INSTALL,
				      !strcmp(S.st.emmc_state, "rsos") ?
				      /* TRANSLATORS: Settings > Storage action (the eMMC already has RetroStoneOS) */
				      _("Update internal storage") :
				      /* TRANSLATORS: Settings > Storage action: copy RetroStoneOS to the eMMC */
				      _("Install to internal storage"));
			if (S.st.emmc) {
				size_text(S.st.emmc_mb, a, sizeof(a));
				snprintf(it->value, sizeof(it->value), "%s, %s", a, disk_state_text(S.st.emmc_state));
			} else if (S.st.emmc_ctrl) {
				/* TRANSLATORS: value: the eMMC is switched on, but this unit has none (not a Pro) */
				strlcpy_(it->value, _("not fitted"), sizeof(it->value));
				it->disabled = true;
			} else {
				/* TRANSLATORS: value of "Install to internal storage" before the eMMC is on */
				strlcpy_(it->value, has_overlay("emmc") ? _("restart first") : _("turn on first"),
					 sizeof(it->value));
			}
			/* RetroStoneOS on the eMMC: this card can become its starter card */
			if (S.st.emmc && !strcmp(S.st.emmc_state, "rsos"))
				/* TRANSLATORS: Settings > Storage action: the SD card keeps only the start-up files and games */
				menu_add(m, MI_ACTION, ID_ST_SDSTARTER, _("Use this SD card as starter card"));
		}
	}
	if (booted_from_emmc() && S.st.sd_known) {
		/* TRANSLATORS: Settings > Storage section header: the card in the microSD slot */
		menu_add(m, MI_HEADER, 0, _("SD card"));
		it = menu_add(m, MI_INFO, ID_ST_SD, _("SD card"));
		if (!S.st.sd) {
			/* TRANSLATORS: value: no card in the slot */
			strlcpy_(it->value, _("none"), sizeof(it->value));
		} else {
			size_text(S.st.sd_mb, a, sizeof(a));
			snprintf(it->value, sizeof(it->value), "%s, %s", a, disk_state_text(S.st.sd_state));
			if (S.st.starter) {
				/* TRANSLATORS: Settings > Storage: the console needs the starter card to start */
				it = menu_add(m, MI_INFO, ID_ST_SDWARN, _("Keep it in: the console starts with it"));
				it->disabled = true;
			} else if (!strcmp(S.st.sd_state, "rsos") || !strcmp(S.st.sd_state, "bootable")) {
				/* TRANSLATORS: warning row: with this card in, the console starts from it */
				it = menu_add(m, MI_INFO, ID_ST_SDWARN, _("Starts the console instead"));
				it->disabled = true;
			}
			if (S.st.starter)
				/* TRANSLATORS: Settings > Storage action: undo the starter card */
				menu_add(m, MI_ACTION, ID_ST_SDFULL, _("Make this SD card a full system again"));
			else if (!strcmp(S.st.sd_state, "rsos"))
				/* TRANSLATORS: Settings > Storage action: keep the card's games, remove its system */
				menu_add(m, MI_ACTION, ID_ST_SDGAMES, _("Turn this SD card into a games card"));
			else if (strcmp(S.st.sd_state, "games") && strcmp(S.st.sd_state, "starter"))
				/* TRANSLATORS: Settings > Storage action: erase the card, then use it for games */
				menu_add(m, MI_ACTION, ID_ST_SDFORMAT, _("Erase it and use it for games"));
		}
	}
	if (sata_ov) {
		/* TRANSLATORS: Settings > Storage section header: the optional M.2 SSD */
		menu_add(m, MI_HEADER, 0, _("M.2 drive"));
		/* TRANSLATORS: toggle: make the M.2 slot work (needs a restart) */
		it = menu_add(m, MI_TOGGLE, ID_ST_SATA, _("M.2 SATA drive"));
		it->on = has_overlay("sata");
		if (S.st.sata) {
			it = menu_add(m, MI_INFO, ID_ST_M2, _("M.2 drive"));
			if (!S.st.m2) {
				/* TRANSLATORS: value: the M.2 slot is on, but no drive answers */
				strlcpy_(it->value, _("no drive found"), sizeof(it->value));
			} else {
				size_text(S.st.m2_mb, a, sizeof(a));
				snprintf(it->value, sizeof(it->value), "%s, %s", a, disk_state_text(S.st.m2_state));
				if (strcmp(S.st.m2_state, "games"))
					/* TRANSLATORS: Settings > Storage action: erase the M.2 drive for games */
					menu_add(m, MI_ACTION, ID_ST_M2FORMAT, _("Erase it and use it for games"));
			}
		}
	}
	/* games from one drive to another (the import screen, local source) */
	if (S.st.lib[0][0] || S.st.lib[1][0] || S.st.lib[2][0]) {
		/* TRANSLATORS: Settings > Storage section header: the drives that hold games */
		menu_add(m, MI_HEADER, 0, _("Game drives"));
		/* TRANSLATORS: Settings > Storage action: copy or move games from one drive to another */
		menu_add(m, MI_ACTION, ID_ST_MOVE, _("Move games between drives"));
	}
	/* free space of the other disks' libraries */
	for (int i = 0; i < 3; i++) {
		uint64_t fr, tot;
		char fa[32], ta[32];

		if (!S.st.lib[i][0] || !hw_storage(S.st.lib[i], &fr, &tot))
			continue;
		/* TRANSLATORS: Settings > Storage: free space of a game disk; %s: Internal storage, SD card, M.2 drive */
		snprintf(b, sizeof(b), _("Games on %s"), i == 0 ? _("internal storage") : i == 1 ? _("SD card") :
			 _("M.2 drive"));
		it = menu_add(m, MI_INFO, ID_ST_LIB0 + i, b);
		i18n_format_size(fr, fa, sizeof(fa));
		i18n_format_size(tot, ta, sizeof(ta));
		snprintf(it->value, sizeof(it->value), _("%s free of %s"), fa, ta);
	}
}

static void overlay_changed(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0)
		ui_power(ui, UI_REBOOT);
}

static void install_choice(struct ui *ui, int choice, void *user)
{
	bool keep_layout = user != NULL;

	if (keep_layout) {
		/* KEEP GAMES / ERASE / CANCEL */
		if (choice == 0)
			task_start(ui, TASK_INSTALL_KEEP);
		else if (choice == 1)
			task_start(ui, TASK_INSTALL);
		return;
	}
	/* WITH MY GAMES / WITHOUT / CANCEL */
	if (choice == 0)
		task_start(ui, TASK_INSTALL);
	else if (choice == 1)
		task_start(ui, TASK_INSTALL + 100);
}

static void confirm_task(struct ui *ui, int choice, void *user)
{
	if (choice == 1)
		task_start(ui, (int)(intptr_t)user);
}

static void sdstarter_ask(struct ui *ui)
{
	/* TRANSLATORS: dialog buttons, uppercase, short (CANCEL is selected) */
	const char *const buttons[] = { _("CANCEL"), _("CONTINUE"), NULL };

	/* TRANSLATORS: Settings > Storage > Use this SD card as starter card */
	dialog_select(dialog_open(ui, _("Make this SD card the starter card? It keeps its games and the "
					"start-up files; its own system is removed, and the console then "
					"runs from the internal storage. Keep the card in the slot. A "
					"freshly flashed card brings the SD system back."),
				  buttons, confirm_task, (void *)(intptr_t)TASK_SDSTARTER), 0);
}

static void install_ask(struct ui *ui)
{
	const struct power_status *bat = ui_battery(ui);
	struct screen *d;

	if (!S.st.emmc) {
		message_open(ui, has_overlay("emmc") ?
			     /* TRANSLATORS: Settings > Storage > Install: the eMMC switch is on, not applied yet */
			     _("Restart the console first: the internal storage appears after a restart.") :
			     /* TRANSLATORS: Settings > Storage > Install: the eMMC switch is off */
			     _("Turn on Internal eMMC above and restart the console first."));
		return;
	}
	if (bat && bat->battery_present && !bat->charger_online && bat->percent >= 0 && bat->percent < 30) {
		/* TRANSLATORS: Settings > Storage > Install */
		message_open(ui, _("Plug in the charger first (or charge the battery above 30 %)."));
		return;
	}
	if (!strcmp(S.st.emmc_state, "rsos")) {
		/* TRANSLATORS: dialog buttons, uppercase, short */
		const char *const buttons[] = { _("KEEP GAMES"), _("ERASE ALL"), _("CANCEL"), NULL };

		/* TRANSLATORS: Settings > Storage > Update internal storage */
		d = dialog_open(ui, _("The internal storage already has RetroStoneOS. Update its system with this one "
				      "and keep its games and saves, or erase everything on it and copy your settings, "
				      "saves and games from this SD card?"), buttons, install_choice, (void *)1);
	} else {
		/* TRANSLATORS: dialog buttons, uppercase, short */
		const char *const buttons[] = { _("WITH MY GAMES"), _("WITHOUT GAMES"), _("CANCEL"), NULL };
		char t[512];
		char a[32];

		size_text(S.st.emmc_mb, a, sizeof(a));
		/* TRANSLATORS: Settings > Storage > Install; %s: the eMMC's size ("15 GB") */
		snprintf(t, sizeof(t), _("Copy RetroStoneOS to the internal storage (%s), so the console starts "
					 "without the SD card? Everything on the internal storage is erased. Your "
					 "settings and saves come along, and your games too if you want. The SD card is "
					 "not changed."), a);
		d = dialog_open(ui, t, buttons, install_choice, NULL);
	}
	dialog_select(d, 2);
}

/* Move games between drives: the drives (the system disk's /data first).
 * Copies: a refresh while the dialog is open rewrites S.st.lib[]. */
static char g_move_roots[4][512];
static const char *g_move_labels[4];
static int g_move_n;

static void move_from_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice >= 0 && choice < g_move_n)
		import_open_drive(ui, g_move_roots[choice], g_move_labels[choice]);
}

static void move_ask(struct ui *ui)
{
	const char *buttons[6];
	int n = 0;

	read_status();
	strlcpy_(g_move_roots[n], ui->cfg.transfer && ui->cfg.transfer->data_root ?
		 ui->cfg.transfer->data_root : "/data", sizeof(g_move_roots[n]));
	g_move_labels[n++] = booted_from_emmc() ? _("Internal storage") : _("SD card");
	for (int i = 0; i < 3 && n < 4; i++) {
		if (!S.st.lib[i][0])
			continue;
		strlcpy_(g_move_roots[n], S.st.lib[i], sizeof(g_move_roots[n]));
		g_move_labels[n++] = i == 0 ? _("Internal storage") : i == 1 ? _("SD card") : _("M.2 drive");
	}
	g_move_n = n;
	for (int i = 0; i < n; i++)
		buttons[i] = g_move_labels[i];
	/* TRANSLATORS: dialog button, uppercase, short */
	buttons[n] = _("CANCEL");
	buttons[n + 1] = NULL;
	/* TRANSLATORS: Settings > Storage > Move games between drives: the source */
	dialog_select(dialog_open(ui, _("Copy or move games from which drive?"), buttons, move_from_choice, NULL), n);
}

bool storage_menu_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	(void)m;
	if (!S.on || it->id < ID_ST_EMMC || it->id > ID_ST_REDETECT)
		return false;
	switch (it->id) {
	case ID_ST_REDETECT:
		if (!dir) {
			/* TRANSLATORS: dialog buttons, uppercase, short */
			const char *const buttons[] = { _("RESTART NOW"), _("LATER"), NULL };

			if (job("redetect", NULL, NULL, UI_JOB_STORAGE) < 0) {
				message_open(ui, _("The setting could not be saved."));
				return true;
			}
			S.overlay_job = true;
			/* TRANSLATORS: Settings > Storage > Detect the model again */
			dialog_open(ui, _("The console looks for its internal storage at the next start, and shows the "
					  "options of the model it finds."), buttons, overlay_changed, NULL);
		}
		return true;
	case ID_ST_EMMC:
	case ID_ST_SATA: {
		/* TRANSLATORS: dialog buttons, uppercase, short */
		const char *const buttons[] = { _("RESTART NOW"), _("LATER"), NULL };

		if (job("overlay", it->id == ID_ST_EMMC ? "emmc" : "sata", it->on ? "on" : "off", UI_JOB_STORAGE) < 0) {
			it->on = !it->on;
			message_open(ui, _("The setting could not be saved."));
			return true;
		}
		S.overlay_job = true;
		dialog_open(ui, _("Restart required: the change applies at the next start."), buttons,
			    overlay_changed, NULL);
		return true;
	}
	case ID_ST_INSTALL:
		if (!dir)
			install_ask(ui);
		return true;
	case ID_ST_SDSTARTER:
		if (!dir)
			sdstarter_ask(ui);
		return true;
	case ID_ST_MOVE:
		if (!dir)
			move_ask(ui);
		return true;
	case ID_ST_SDFULL:
		if (!dir) {
			/* TRANSLATORS: dialog buttons, uppercase, short (CANCEL is selected) */
			const char *const buttons[] = { _("CANCEL"), _("CONTINUE"), NULL };

			/* TRANSLATORS: Settings > Storage > Make this SD card a full system again */
			dialog_select(dialog_open(ui, _("Put the system back on this SD card? The system that runs now "
							"and your current settings and saves are copied to it (its games "
							"stay). The console then starts from the SD card again; the "
							"internal storage keeps RetroStoneOS."),
						  buttons, confirm_task, (void *)(intptr_t)TASK_SDFULL), 0);
		}
		return true;
	case ID_ST_SDGAMES:
		if (!dir) {
			/* TRANSLATORS: dialog buttons, uppercase, short (CANCEL is selected) */
			const char *const buttons[] = { _("CANCEL"), _("CONTINUE"), NULL };

			/* TRANSLATORS: Settings > Storage > Turn this SD card into a games card */
			dialog_select(dialog_open(ui, _("Remove RetroStoneOS from this SD card and keep it for games? Its "
							"games stay on it and appear in the menu. Its settings and saves "
							"are not used any more. The console will no longer start from it."),
						  buttons, confirm_task, (void *)(intptr_t)TASK_SDGAMES), 0);
		}
		return true;
	case ID_ST_SDFORMAT:
	case ID_ST_M2FORMAT:
		if (!dir) {
			const char *const buttons[] = { _("CANCEL"), _("ERASE"), NULL };

			/* TRANSLATORS: Settings > Storage > Erase it and use it for games */
			dialog_select(dialog_open(ui, _("Erase everything on this drive and prepare it for games? This "
							"cannot be undone."), buttons, confirm_task,
						  (void *)(intptr_t)(it->id == ID_ST_SDFORMAT ? TASK_FORMAT_SD :
								      TASK_FORMAT_M2)), 0);
		}
		return true;
	default:
		return true;
	}
}

static void status_text(char *out, size_t n)
{
	FILE *f = fopen(ST_STATUS, "r");
	size_t k = 0;

	if (f) {
		k = fread(out, 1, n - 1, f);
		fclose(f);
	}
	out[k] = 0;
}

bool storage_menu_changed(void)
{
	char now[1024];

	if (!S.on)
		return false;
	status_text(now, sizeof(now));
	if (!strcmp(now, g_menu_status))
		return false;
	strlcpy_(g_menu_status, now, sizeof(g_menu_status));
	return true;
}

int storage_timeout(const struct ui *ui, int64_t now_ms)
{
	(void)ui;
	if (!S.on)
		return -1;
	return S.next_poll > now_ms ? (int)MIN(S.next_poll - now_ms, ST_POLL_MS) : 0;
}

/* ------------------------------------------- import destinations */
int storage_import_targets(struct ui *ui, const char **roots, const char **labels, int max)
{
	int n = 0;

	(void)ui;
	if (!S.on || max < 2)
		return 0;
	read_status();
	/* first the system disk's /data (NULL): where the console started from */
	roots[n] = NULL;
	labels[n++] = booted_from_emmc() ? _("Internal storage") : _("SD card");
	for (int i = 0; i < 3 && n < max; i++) {
		if (!S.st.lib[i][0])
			continue;
		roots[n] = S.st.lib[i];
		labels[n] = i == 0 ? _("Internal storage") : i == 1 ? _("SD card") : _("M.2 drive");
		n++;
	}
	return n > 1 ? n : 0;
}

/* ------------------------------------------------------- game covers */
static void covers_confirm(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 1)
		task_start(ui, TASK_COVERS);
}

void covers_start(struct ui *ui)
{
	char ip[64], t[512];
	int n = 0, nsys = 0;
	FILE *f;

	if (S.job && S.kind == TASK_COVERS) {
		task_page_open(ui);      /* running in the background: show it */
		return;
	}

	if (access(COVERS_HELPER, X_OK) != 0) {
		/* TRANSLATORS: Settings > Game lists > Download game covers, on a system without the helper */
		message_open(ui, _("Cover downloads are not available on this system."));
		return;
	}
	if (!hw_has_ipv4(ip, sizeof(ip))) {
		/* TRANSLATORS: Settings > Game lists > Download game covers, without a network */
		message_open(ui, _("Connect to WiFi first (Settings > Network), then try again."));
		return;
	}
	loader_complete_all(ui);
	f = fopen(S.p_request, "w");
	if (!f) {
		message_open(ui, _("This could not start."));
		return;
	}
	for (int i = 0; i < ui->nsys; i++) {
		const struct sysent *se = &ui->sys[i];
		int before = n;

		if (se->is_collection || !se->games)
			continue;
		for (int j = 0; j < se->games->n; j++) {
			const struct game *g = &se->games->games[j];
			size_t lp, lr;

			/* games with a picture, and the read-only built-in games */
			if (g->image || !g->path || !g->rel || !strncmp(g->path, "/usr/", 5))
				continue;
			lp = strlen(g->path);
			lr = strlen(g->rel);
			if (lr + 1 >= lp || strcmp(g->path + lp - lr, g->rel) || g->path[lp - lr - 1] != '/')
				continue;
			fprintf(f, "%s\t%.*s\t%s\n", systems_effective(g->system, g->path), (int)(lp - lr - 1), g->path, g->path);
			n++;
		}
		nsys += n > before;
	}
	fclose(f);
	if (!n) {
		/* TRANSLATORS: Settings > Game lists > Download game covers */
		message_open(ui, _("All your games already have a picture."));
		return;
	}
	{
		/* TRANSLATORS: dialog buttons, uppercase, short (CANCEL is selected) */
		const char *const buttons[] = { _("CANCEL"), _("DOWNLOAD"), NULL };

		/* TRANSLATORS: Settings > Game lists > Download game covers: %d games, %d systems */
		snprintf(t, sizeof(t), _("Download box covers for %d games (%d systems) from the libretro thumbnail "
					 "server, as RetroArch does? Games that already have a picture keep it. "
					 "This can take a few minutes."), n, nsys);
		dialog_select(dialog_open(ui, t, buttons, covers_confirm, NULL), 0);
	}
}

/* ------------------------------------------------- multi-disc games */
struct disc {
	char key[1024];              /* folder + name without "(Disc N)" */
	char file[256];              /* the disc's file name */
	char base[256];              /* the .m3u's name, without .m3u */
	int num;
};

static int cmp_disc(const void *a, const void *b)
{
	const struct disc *x = a, *y = b;
	int c = strcmp(x->key, y->key);

	return c ? c : x->num - y->num;
}

/* "Name (USA) (Disc 2).cue" -> base "Name (USA)", disc 2; false when the
 * name has no "(Disc N)" / "(Disk N)" tag */
static bool disc_tag(const char *file, char *base, size_t n, int *num)
{
	const char *p = file, *tag = NULL, *ext = strrchr(file, '.');
	size_t l;

	while ((p = strchr(p, '('))) {
		if (!strncasecmp(p, "(Disc ", 6) || !strncasecmp(p, "(Disk ", 6)) {
			tag = p;
			break;
		}
		p++;
	}
	if (!tag || !ext || ext < tag || !isdigit((unsigned char)tag[6]))
		return false;
	*num = atoi(tag + 6);
	/* the base: before the tag, then whatever follows the tag's ")" up to the
	 * extension ("Game (Disc 1) (USA).cue" -> "Game (USA)") */
	l = (size_t)(tag - file);
	while (l && file[l - 1] == ' ')
		l--;
	snprintf(base, n, "%.*s", (int)l, file);
	p = strchr(tag, ')');
	if (p && p + 1 < ext) {
		size_t bl = strlen(base);

		snprintf(base + bl, n - bl, "%.*s", (int)(ext - p - 1), p + 1);
	}
	return true;
}

void discs_group(struct ui *ui)
{
	struct disc *d = NULL;
	int n = 0, cap = 0, written = 0;
	char t[256];

	loader_complete_all(ui);
	for (int i = 0; i < ui->nsys; i++) {
		const struct sysent *se = &ui->sys[i];

		/* only where a core takes playlists: elsewhere the .m3u would hide
		 * the discs behind an entry that cannot start */
		if (se->is_collection || !se->games || !se->def || !systems_ext_ok(se->def->name, "m3u"))
			continue;
		for (int j = 0; j < se->games->n; j++) {
			const struct game *g = &se->games->games[j];
			const char *slash = g->path ? strrchr(g->path, '/') : NULL;
			const char *ext;
			char base[256];
			int num;

			if (!slash || !strncmp(g->path, "/usr/", 5))
				continue;
			ext = strrchr(slash, '.');
			if (ext && !strcasecmp(ext, ".m3u"))
				continue;
			if (!disc_tag(slash + 1, base, sizeof(base), &num))
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				d = xrealloc(d, sizeof(*d) * (size_t)cap);
			}
			snprintf(d[n].key, sizeof(d[n].key), "%.*s/%s", (int)(slash - g->path), g->path, base);
			strlcpy_(d[n].file, slash + 1, sizeof(d[n].file));
			strlcpy_(d[n].base, base, sizeof(d[n].base));
			d[n].num = num;
			n++;
		}
	}
	qsort(d, (size_t)n, sizeof(*d), cmp_disc);
	for (int i = 0; i < n;) {
		int j = i;
		char m3u[1100], tmp[1110];
		FILE *f;

		while (j < n && !strcmp(d[j].key, d[i].key))
			j++;
		snprintf(m3u, sizeof(m3u), "%s.m3u", d[i].key);
		snprintf(tmp, sizeof(tmp), "%s.tmp", m3u);
		/* two discs at least, and never over an existing playlist; a
		 * temporary file, synced, then renamed: never a half-written
		 * playlist after a power cut */
		if (j - i >= 2 && access(m3u, F_OK) != 0 && (f = fopen(tmp, "w"))) {
			bool ok = true;

			for (int k = i; k < j; k++)
				ok = fprintf(f, "%s\n", d[k].file) > 0 && ok;
			ok = fflush(f) == 0 && ok;
			ok = fsync(fileno(f)) == 0 && ok;
			ok = fclose(f) == 0 && ok;
			if (ok && rename(tmp, m3u) == 0) {
				written++;
				LOGI("discs: %s (%d discs)", m3u, j - i);
			} else {
				LOGW("discs: cannot write %s: %s", m3u, strerror(errno));
				unlink(tmp);
			}
		}
		i = j;
	}
	free(d);
	if (!written) {
		/* TRANSLATORS: Settings > Game lists > Group multi-disc games: nothing to do */
		message_open(ui, _("No multi-disc game without a playlist was found."));
		return;
	}
	/* TRANSLATORS: Settings > Game lists > Group multi-disc games: %d playlists written */
	snprintf(t, sizeof(t), _("%d multi-disc games grouped: each is one entry now. Change discs in the "
				 "in-game menu (SELECT + X)."), written);
	ui_reload_games(ui, true);
	message_open(ui, t);
}

/* ---------------------------------------------------- memory speed */
/* The RetroStone2's DRAM clock (Settings > Power > Memory speed): the AXP209
 * register that U-Boot's SPL reads, through /usr/libexec/rsos/axpstamp
 * (RSOS_AXPSTAMP overrides it: previews). See its dram-* commands. */
static const char *axp_tool(void)
{
	const char *e = getenv("RSOS_AXPSTAMP");

	return e && *e ? e : "/usr/libexec/rsos/axpstamp";
}

bool memspeed_available(void)
{
	return access(axp_tool(), X_OK) == 0;
}

int memspeed_get(bool *failed, int *mhz)
{
	char cmd[300], state[24] = "", line[96] = "";
	int lvl = 0, clk = 0;
	FILE *p;

	*failed = false;
	*mhz = 0;
	snprintf(cmd, sizeof(cmd), "'%s' dram-get 2>/dev/null", axp_tool());
	p = popen(cmd, "r");
	if (!p)
		return -1;
	if (!fgets(line, sizeof(line), p) || sscanf(line, "%23s %d %d", state, &lvl, &clk) != 3) {
		pclose(p);
		return -1;
	}
	pclose(p);
	*mhz = clk;
	if (!strcmp(state, "failed")) {
		*failed = true;
		return 0;
	}
	return !strcmp(state, "normal") ? 0 : lvl;
}

int memspeed_set(int level)
{
	char arg[4];
	pid_t pid;
	int st = 0;

	snprintf(arg, sizeof(arg), "%d", level);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl(axp_tool(), axp_tool(), "dram-set", arg, (char *)NULL);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st))
		return -1;
	LOGI("power: memory speed level %d requested (applies at the next start, as a trial)", level);
	return 0;
}

/* One game's cover (after Rename this game): the same task, a one-line
 * request. Quietly nothing without the helper or a network. */
bool covers_can_start(void)
{
	char ip[64];

	return access(COVERS_HELPER, X_OK) == 0 && !S.job && hw_has_ipv4(ip, sizeof(ip));
}

void covers_start_one(struct ui *ui, const char *system, const char *root, const char *path)
{
	FILE *f;

	if (!covers_can_start())
		return;
	f = fopen(S.p_request, "w");
	if (!f)
		return;
	fprintf(f, "%s\t%s\t%s\n", system, root, path);
	fclose(f);
	task_start(ui, TASK_COVERS);
}
