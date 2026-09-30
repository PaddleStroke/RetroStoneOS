/*
 * loader.c - game lists: the carousel snapshot (systems.idx), the background
 * loader, on-demand loading, and keeping the carousel in sync.
 *
 * Boot with a snapshot (every boot but the first):
 *   1. <cache>/systems.idx gives the carousel as it was at the end of the
 *      last complete load (systems with games, their game and favorite
 *      counts, their ROM folders, the collections). The menu is built from
 *      it and drawn at once: no game list is needed for the carousel.
 *   2. After the first menu frame, one worker thread loads every system's
 *      list (scan cache when valid, else a scan) in the order the user is
 *      most likely to need them: the selected system, its neighbours, then
 *      the rest. The worker only does file I/O and parsing into lists it
 *      owns; results go through a mutex-protected queue and the main thread
 *      installs them in ui_update() (the UI is not thread-safe).
 *   3. A list for an entry that has none yet is installed at once. Anything
 *      that changes what the carousel shows (a system gained or lost its
 *      games, a list changed under a screen that uses it) waits until the
 *      carousel is the only screen, then the carousel is rebuilt keeping the
 *      selected system. Collections are built when every list is in.
 *   4. Opening a system whose list has not arrived loads it on the spot
 *      (or waits for the worker if it is loading that one right now);
 *      opening a collection first completes every list.
 * First boot (no snapshot, or the core set changed): the same worker, with
 * the "Preparing your console" screen until every list is in.
 *
 * Why a thread rather than steps between frames: on the device the time is
 * blocking I/O on the SD card, which cannot be cut into slices (one cold
 * exFAT folder is one blocking stat() of ~110 ms without fswarm), and the
 * carousel must keep animating and taking input meanwhile. The worker never
 * touches UI state; it is paused while a game runs and stopped before any
 * synchronous reload or exit.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../i18n/i18n.h"
#include "fswarm.h"
#include "ui_internal.h"


#define SNAP_FILE    "systems.idx"
#define SNAP_MAGIC   "rsos-systems"
#define SNAP_VERSION 1
#define POLL_MS      20       /* ui_timeout while lists are arriving */

enum { LS_TODO = 0, LS_BUSY, LS_DONE };

struct lres {
	int sidx;
	struct gamelist *gl;      /* NULL: no folder */
	struct games_load_stats st;
	bool by_main;             /* loaded on demand by the main thread */
	struct lres *next;
};

struct loader {
	/* ---- shared with the worker, under mu ---- */
	pthread_mutex_t mu;
	pthread_cond_t cv;
	pthread_t th;
	bool th_started, th_exited;
	bool stop, paused;
	int *order, norder, next;
	unsigned char *state;          /* per system: LS_* */
	struct lres *q_head, **q_tail;
	struct fswarm_stats warm;      /* the worker's warm-up */
	int64_t worker_us;
	/* ---- worker configuration (read-only while it runs) ---- */
	char roms_dir[1024], cache_dir[1024], alt[1024];
	char builtin_dir[1024];          /* read-only <dir>/<system>/ folders ("" = none) */
	bool use_cache;
	int64_t delay_us;              /* test hook: RSOS_LOADER_DELAY_MS per system */
	/* ---- main thread only ---- */
	int nsys;
	struct gamelist **lists;       /* validated lists (owned); NULL = no games */
	bool *done;
	struct gamelist **pend;        /* waiting for a safe moment */
	bool *has_pend;
	int ndone, nreplaced;
	bool started, want_worker, complete, first_boot, carousel_changed;
	struct gamelist *coll[2];      /* favorites, last played (owned) */
	/* snapshot */
	bool snap_ok;
	int *snap_count, *snap_nfav;   /* -1: not in the snapshot */
	char (*snap_dir)[512];
	int snap_coll[2];
	/* Systems whose folder had no games and no subfolder: its mtime is
	 * enough to know it still has none (no scan cache read). Written by
	 * the main thread before the worker starts, then read-only. */
	int64_t *empty_mt;             /* 0: unknown */
	char (*empty_dir)[512];
	int64_t *found_mt;             /* the same, found by this load (next snapshot) */
	char (*found_dir)[512];
	char *snap_text;               /* last loaded or written content */
	/* timings */
	int64_t t_start, t_worker0;
	struct ui_io io_start;
	struct games_load_stats sum;
	int64_t max_us;
	char max_name[32];
};

static const char *const g_coll_names[2] = { "favorites", "lastplayed" };

/* ---------------------------------------------------------------- helpers */
/* The ROM folder of a system (thread-safe): its read-only folder in the
 * root filesystem when it has one (the RetroStone VC games: always there,
 * whatever the card holds), then its name, then its aliases. */
static bool find_rom_folder(const char *builtin_dir, const char *roms_dir, const struct sysdef *sd,
			    char *out, size_t n)
{
	if (builtin_dir && *builtin_dir) {
		snprintf(out, n, "%s/%s", builtin_dir, sd->name);
		if (dir_exists(out))
			return true;
	}
	snprintf(out, n, "%s/%s", roms_dir, sd->name);
	if (dir_exists(out))
		return true;
	for (int i = 0; i < 4 && sd->folders[i]; i++) {
		snprintf(out, n, "%s/%s", roms_dir, sd->folders[i]);
		if (dir_exists(out)) {
			LOGI("ui: %s games read from %s", sd->name, out);
			return true;
		}
	}
	return false;
}

static struct lres *load_one(struct loader *L, int sidx)
{
	const struct sysdef *sd = systems_get(sidx);
	struct lres *r = xcalloc(1, sizeof(*r));
	char dir[1100];
	int64_t t0 = ui_now_us(), tf;

	r->sidx = sidx;
	if (!sd)
		return r;
	if (L->empty_mt[sidx]) {
		/* empty last time: one stat() says whether it still is (and
		 * one more that no read-only folder came with a system update) */
		struct stat st;

		snprintf(dir, sizeof(dir), "%s/%s", L->builtin_dir, sd->name);
		if ((!L->builtin_dir[0] || !dir_exists(dir)) &&
		    stat(L->empty_dir[sidx], &st) == 0 && S_ISDIR(st.st_mode) &&
		    (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec == L->empty_mt[sidx]) {
			r->gl = xcalloc(1, sizeof(*r->gl));
			arena_init(&r->gl->arena, 256);
			strlcpy_(r->gl->system, sd->name, sizeof(r->gl->system));
			strlcpy_(r->gl->rom_dir, L->empty_dir[sidx], sizeof(r->gl->rom_dir));
			r->gl->dirs = xcalloc(1, sizeof(char *));
			r->gl->dir_mtime = xcalloc(1, sizeof(int64_t));
			r->gl->dirs[0] = xstrdup(L->empty_dir[sidx]);
			r->gl->dir_mtime[0] = L->empty_mt[sidx];
			r->gl->ndirs = 1;
			r->st.stat_us = r->st.total_us = ui_now_us() - t0;
			r->st.ndirs = 1;
			r->st.from_cache = true;
			return r;
		}
	}
	if (!find_rom_folder(L->builtin_dir, L->roms_dir, sd, dir, sizeof(dir))) {
		r->st.dir_us = r->st.total_us = ui_now_us() - t0;
		return r;
	}
	tf = ui_now_us() - t0;
	r->gl = games_load(sd->name, dir, L->use_cache ? L->cache_dir : NULL, L->alt, &r->st);
	r->st.dir_us += tf;
	r->st.total_us = ui_now_us() - t0;   /* folder lookup included */
	return r;
}

static void queue_push(struct loader *L, struct lres *r)
{
	r->next = NULL;
	*L->q_tail = r;
	L->q_tail = &r->next;
}

static void lres_free(struct lres *r)
{
	while (r) {
		struct lres *n = r->next;

		games_free(r->gl);
		free(r);
		r = n;
	}
}

/* Warms the folders the loader reads: every ROM folder, the scan cache and
 * the alternative gamelists (thread-safe). */
static void warm_lists(struct loader *L, struct fswarm_stats *st)
{
	char scan[1100], *p[2];

	fswarm((const char *const[]){ L->roms_dir }, 1, FSW_CHILDREN, st);
	snprintf(scan, sizeof(scan), "%s/scan", L->cache_dir);
	p[0] = L->use_cache ? scan : NULL;
	p[1] = L->alt;
	fswarm((const char *const *)p, 2, 0, st);
}

/* ----------------------------------------------------------------- worker */
static void *worker(void *arg)
{
	struct loader *L = arg;
	int64_t t0 = ui_now_us();
	struct fswarm_stats ws = { 0 };

	warm_lists(L, &ws);
	pthread_mutex_lock(&L->mu);
	L->warm = ws;
	for (;;) {
		struct lres *r;
		int sidx = -1;

		while (L->paused && !L->stop)
			pthread_cond_wait(&L->cv, &L->mu);
		if (L->stop)
			break;
		while (L->next < L->norder) {
			int s = L->order[L->next++];

			if (L->state[s] == LS_TODO) {
				sidx = s;
				break;
			}
		}
		if (sidx < 0)
			break;
		L->state[sidx] = LS_BUSY;
		pthread_mutex_unlock(&L->mu);
		r = load_one(L, sidx);
		if (L->delay_us)
			usleep((useconds_t)L->delay_us);
		pthread_mutex_lock(&L->mu);
		L->state[sidx] = LS_DONE;
		queue_push(L, r);
		pthread_cond_broadcast(&L->cv);
	}
	L->worker_us = ui_now_us() - t0;
	L->th_exited = true;
	pthread_cond_broadcast(&L->cv);
	pthread_mutex_unlock(&L->mu);
	return NULL;
}

static void worker_start(struct ui *ui)
{
	struct loader *L = ui->ld;

	L->want_worker = false;
	L->t_worker0 = ui_now_us();
	L->th_exited = false;
	if (pthread_create(&L->th, NULL, worker, L) == 0) {
		L->th_started = true;
		return;
	}
	/* no thread: the main thread does it (first boot screen, or on demand) */
	LOGW("ui: cannot start the loader thread, loading in the foreground");
	for (int i = 0; i < L->norder; i++) {
		int s = L->order[i];

		if (L->state[s] == LS_TODO) {
			struct lres *r = load_one(L, s);

			L->state[s] = LS_DONE;
			queue_push(L, r);
		}
	}
}

/* Stops and joins the worker; drops every result not applied yet. */
static void worker_stop(struct loader *L)
{
	struct lres *q;

	if (L->th_started) {
		pthread_mutex_lock(&L->mu);
		L->stop = true;
		pthread_cond_broadcast(&L->cv);
		pthread_mutex_unlock(&L->mu);
		pthread_join(L->th, NULL);
		L->th_started = false;
	}
	pthread_mutex_lock(&L->mu);
	q = L->q_head;
	L->q_head = NULL;
	L->q_tail = &L->q_head;
	L->stop = false;
	pthread_mutex_unlock(&L->mu);
	lres_free(q);
}

/* Makes sure a result for sidx exists (queued or applied): loads it here,
 * or waits for the worker if it is loading it right now. */
static void load_now(struct loader *L, int sidx)
{
	struct lres *r;

	pthread_mutex_lock(&L->mu);
	while (L->state[sidx] == LS_BUSY)
		pthread_cond_wait(&L->cv, &L->mu);
	if (L->state[sidx] == LS_DONE) {
		pthread_mutex_unlock(&L->mu);
		return;
	}
	L->state[sidx] = LS_BUSY;
	pthread_mutex_unlock(&L->mu);
	r = load_one(L, sidx);
	r->by_main = true;
	pthread_mutex_lock(&L->mu);
	L->state[sidx] = LS_DONE;
	queue_push(L, r);
	pthread_cond_broadcast(&L->cv);
	pthread_mutex_unlock(&L->mu);
}

/* ------------------------------------------------------------------ lists */
static int count_fav(const struct gamelist *gl)
{
	int n = 0;

	for (int i = 0; gl && i < gl->n; i++)
		n += gl->games[i].favorite;
	return n;
}

/* Settings > Game lists > Sort games by (gamelist_sort: name, playtime,
 * lastplayed), favorites first when that setting is on. */
void ui_sort_games(struct ui *ui, struct gamelist *gl)
{
	const char *how = settings_get(ui->settings, "gamelist_sort", "name");

	games_sort_by(gl, settings_get_bool(ui->settings, "favorites_first", true),
		      !strcmp(how, "playtime") ? GAMES_SORT_PLAYTIME :
		      !strcmp(how, "lastplayed") ? GAMES_SORT_LASTPLAYED : GAMES_SORT_NAME);
}

/* Main thread: the finishing touches of a list from games_load(). */
static struct gamelist *prepare(struct ui *ui, int sidx, struct gamelist *gl)
{
	const struct sysdef *sd = systems_get(sidx);

	if (!gl)
		return NULL;
	if (gl->cache_written)
		img_note_written();   /* synced with the image cache when idle */
	/* games_load uses the folder name as system: point to the real one */
	strlcpy_(gl->system, sd->name, sizeof(gl->system));
	for (int k = 0; k < gl->n; k++)
		gl->games[k].system = gl->system;
	gamedb_apply(ui->db, gl);
	/* hidden games (gamelist.xml, or Hide in the game options) are left
	 * out unless Settings > Game lists > Show hidden games is on */
	if (!settings_get_bool(ui->settings, "show_hidden", false))
		games_drop_hidden(gl);
	if (!gl->n) {
		games_free(gl);
		return NULL;
	}
	ui_sort_games(ui, gl);
	return gl;
}

static void log_result(struct ui *ui, const struct lres *r, const struct gamelist *gl)
{
	struct loader *L = ui->ld;
	const struct games_load_stats *s = &r->st;
	const char *name = systems_get(r->sidx) ? systems_get(r->sidx)->name : "?";

	L->sum.total_us += s->total_us;
	L->sum.dir_us += s->dir_us;
	L->sum.read_us += s->read_us;
	L->sum.stat_us += s->stat_us;
	L->sum.parse_us += s->parse_us;
	L->sum.scan_us += s->scan_us;
	L->sum.xml_us += s->xml_us;
	L->sum.save_us += s->save_us;
	if (s->total_us > L->max_us) {
		L->max_us = s->total_us;
		strlcpy_(L->max_name, name, sizeof(L->max_name));
	}
	if (!r->gl) {
		LOGI("games: %s: no folder, %lld us%s", name, (long long)s->total_us, r->by_main ? " (on demand)" : "");
	} else if (s->from_cache && !s->read_us && !gl) {
		LOGI("games: %s: still an empty folder (same mtime), %lld us%s", name, (long long)s->total_us,
		     r->by_main ? " (on demand)" : "");
	} else if (s->from_cache) {
		LOGI("games: %s: %d games from the cache in %lld us (folder %lld, read %lld, stat %lld x%d, "
		     "parse %lld)%s", name, gl ? gl->n : 0, (long long)s->total_us, (long long)s->dir_us,
		     (long long)s->read_us, (long long)s->stat_us, s->ndirs, (long long)s->parse_us,
		     r->by_main ? " (on demand)" : "");
	} else {
		LOGI("games: %s: scanned %d games in %lld us (folder %lld, cache read %lld, readdir %lld x%d, "
		     "gamelist %lld, cache write %lld)%s", name, gl ? gl->n : 0, (long long)s->total_us,
		     (long long)s->dir_us, (long long)s->read_us, (long long)s->scan_us, s->ndirs,
		     (long long)s->xml_us, (long long)s->save_us, r->by_main ? " (on demand)" : "");
	}
}

/* ------------------------------------------------------------ collections */
static int cmp_last(const void *a, const void *b)
{
	const struct game *x = a, *y = b;

	return x->lastplayed < y->lastplayed ? 1 : x->lastplayed > y->lastplayed ? -1 : 0;
}

static void free_coll(struct loader *L)
{
	for (int k = 0; k < 2; k++) {
		if (!L->coll[k])
			continue;
		free(L->coll[k]->games);
		arena_free(&L->coll[k]->arena);
		free(L->coll[k]);
		L->coll[k] = NULL;
	}
}

/* Favorites and Last played, from every list (the games are copies that
 * point into the real lists' strings). */
static void build_coll(struct ui *ui)
{
	struct loader *L = ui->ld;
	int n[2] = { 0, 0 };

	free_coll(L);
	if (!settings_get_bool(ui->settings, "collections", true))
		return;
	for (int s = 0; s < L->nsys; s++)
		for (int k = 0; L->lists[s] && k < L->lists[s]->n; k++) {
			n[0] += L->lists[s]->games[k].favorite;
			n[1] += L->lists[s]->games[k].lastplayed > 0;
		}
	for (int c = 0; c < 2; c++) {
		struct gamelist *gl;

		if (!n[c])
			continue;
		gl = xcalloc(1, sizeof(*gl));
		arena_init(&gl->arena, 1024);
		strlcpy_(gl->system, g_coll_names[c], sizeof(gl->system));
		gl->games = xcalloc((size_t)n[c], sizeof(struct game));
		for (int s = 0; s < L->nsys; s++)
			for (int k = 0; L->lists[s] && k < L->lists[s]->n; k++) {
				const struct game *g = &L->lists[s]->games[k];

				if (c == 0 ? !g->favorite : g->lastplayed <= 0)
					continue;
				gl->games[gl->n++] = *g;
				gl->has_media |= g->image || g->desc;
			}
		if (c == 0) {
			games_sort(gl, false);
		} else {
			qsort(gl->games, (size_t)gl->n, sizeof(struct game), cmp_last);
			gl->n = MIN(gl->n, 50);
		}
		L->coll[c] = gl;
	}
}

/* --------------------------------------------------------------- carousel */
/* The name shown for an entry, in the language in use: the system's
 * regional name (systems.c marks them with NC_("system", ...): Japanese
 * "Super Nintendo" is a Super Famicom) or the collection's. */
static void set_fullname(struct sysent *se)
{
	const char *name;

	if (se->def)
		name = C_("system", se->def->fullname);
	else if (se->sidx == -1)
		/* TRANSLATORS: the favorite games collection in the system carousel
		 * (big text, uppercase in the built-in themes) and its list header */
		name = _("Favorites");
	else
		/* TRANSLATORS: the recently played games collection in the system
		 * carousel (big text, uppercase in the built-in themes) and its list header */
		name = _("Last played");
	strlcpy_(se->fullname, name, sizeof(se->fullname));
}

/* The language changed: every carousel entry gets its name in the new
 * one (the caller then reloads the theme and rebuilds the views). */
void loader_relabel(struct ui *ui)
{
	for (int i = 0; i < ui->nsys; i++)
		set_fullname(&ui->sys[i]);
}

static void fill_sysent(struct sysent *se, const struct sysdef *sd)
{
	memset(se, 0, sizeof(*se));
	se->def = sd;
	strlcpy_(se->name, sd->name, sizeof(se->name));
	set_fullname(se);
	for (int i = 0; i < 4 && sd->theme[i]; i++)
		se->theme_alias[i] = sd->theme[i];
	if (!se->theme_alias[0])
		se->theme_alias[0] = sd->name;
}

static void fill_coll(struct sysent *se, int c)
{
	memset(se, 0, sizeof(*se));
	se->sidx = -1 - c;
	strlcpy_(se->name, g_coll_names[c], sizeof(se->name));
	se->theme_alias[0] = c == 0 ? "auto-favorites" : "auto-lastplayed";
	se->theme_alias[1] = g_coll_names[c];
	se->is_collection = true;
	set_fullname(se);
}

struct want {
	int sidx;                 /* -1 favorites, -2 last played */
	const char *name;
};

/* What the carousel should show now: validated lists, the snapshot for the
 * systems not validated yet, collections once complete (placeholders from
 * the snapshot before). */
static int wanted(struct ui *ui, struct want *w, int max)
{
	struct loader *L = ui->ld;
	int n = 0;

	for (int s = 0; s < L->nsys && n < max; s++) {
		bool show = L->done[s] ? L->lists[s] != NULL : L->snap_count[s] > 0;

		if (show) {
			w[n].sidx = s;
			w[n++].name = systems_get(s)->name;
		}
	}
	if (!settings_get_bool(ui->settings, "collections", true))
		return n;
	for (int c = 0; c < 2 && n < max; c++) {
		bool show = L->complete ? L->coll[c] != NULL : L->snap_coll[c] > 0;

		if (show) {
			w[n].sidx = -1 - c;
			w[n++].name = g_coll_names[c];
		}
	}
	return n;
}

/* Games, counts and folder of an entry from what is known now. Returns
 * EU_LIST if its list changed, EU_COUNTS if what the carousel shows did. */
enum { EU_LIST = 1, EU_COUNTS = 2 };

static int entry_update(struct ui *ui, struct sysent *se)
{
	struct loader *L = ui->ld;
	struct gamelist *gl;
	int count, nfav;

	if (se->sidx >= 0) {
		int s = se->sidx;

		gl = L->done[s] ? L->lists[s] : NULL;
		count = gl ? gl->n : L->snap_count[s];
		nfav = gl ? count_fav(gl) : L->snap_nfav[s];
		if (gl && strcmp(se->rom_dir, gl->rom_dir)) {
			/* the games now come from another folder (alias): its
			 * theme.xml may differ (the asset worker may be reading
			 * this one: stopped first) */
			strlcpy_(se->rom_dir, gl->rom_dir, sizeof(se->rom_dir));
			if (se->theme)
				ui_assets_invalidate(ui, false);
			theme_free(se->theme);
			se->theme = NULL;
		}
	} else {
		int c = -1 - se->sidx;

		gl = L->complete ? L->coll[c] : NULL;
		count = gl ? gl->n : L->snap_coll[c];
		nfav = 0;
	}
	int r = (se->games != gl ? EU_LIST : 0) | (se->count != count || se->nfav != nfav ? EU_COUNTS : 0);

	se->games = gl;
	se->count = count;
	se->nfav = nfav;
	return r;
}

static void free_entry(struct sysent *se)
{
	theme_free(se->theme);
	se->theme = NULL;
}

#define CS_REBUILT 4

/*
 * Brings ui->sys in line with wanted(). Only when no screen but the
 * carousel refers to the entries (they move). force: rebuild the views even
 * if the entries are the same. Returns EU_* flags, | CS_REBUILT if the
 * entries changed (the carousel was rebuilt, keeping the selected system).
 */
static int carousel_sync(struct ui *ui, bool force)
{
	struct loader *L = ui->ld;
	int maxw = L->nsys + 2, nw, cursor, changed = 0;
	struct want *w = xcalloc((size_t)maxw, sizeof(*w));
	bool same;
	char keep[32] = "";
	struct sysent *ns;

	nw = wanted(ui, w, maxw);
	same = nw == ui->nsys;
	for (int i = 0; same && i < nw; i++)
		same = !strcmp(ui->sys[i].name, w[i].name);
	if (same && !force) {
		for (int i = 0; i < ui->nsys; i++)
			changed |= entry_update(ui, &ui->sys[i]);
		free(w);
		if (changed && ui->nstack)
			sysview_refresh_info(ui, ui->stack[0]);
		return changed;
	}
	/* the entries move (and some themes go): the asset worker stops first,
	 * the list views it built for the old entries go */
	ui_assets_invalidate(ui, false);
	if (ui->nsys)
		strlcpy_(keep, ui->sys[CLAMP(ui->sys_cursor, 0, ui->nsys - 1)].name, sizeof(keep));
	cursor = ui->sys_cursor;
	ns = xcalloc((size_t)MAX(nw, 1), sizeof(*ns));
	for (int i = 0; i < nw; i++) {
		struct sysent *old = NULL;

		for (int k = 0; k < ui->nsys; k++)
			if (!strcmp(ui->sys[k].name, w[i].name))
				old = &ui->sys[k];
		if (old) {
			ns[i] = *old;
			old->theme = NULL;          /* moved */
			old->name[0] = 0;
		} else if (w[i].sidx >= 0) {
			fill_sysent(&ns[i], systems_get(w[i].sidx));
			ns[i].sidx = w[i].sidx;
			if (L->done[w[i].sidx] && L->lists[w[i].sidx])
				strlcpy_(ns[i].rom_dir, L->lists[w[i].sidx]->rom_dir, sizeof(ns[i].rom_dir));
			else
				strlcpy_(ns[i].rom_dir, L->snap_dir[w[i].sidx], sizeof(ns[i].rom_dir));
		} else {
			fill_coll(&ns[i], -1 - w[i].sidx);
		}
		entry_update(ui, &ns[i]);
	}
	for (int k = 0; k < ui->nsys; k++)
		free_entry(&ui->sys[k]);
	free(ui->sys);
	ui->sys = ns;
	ui->nsys = nw;
	for (int i = 0; i < nw; i++)
		if (!strcmp(ns[i].name, keep))
			cursor = i;
	ui->sys_cursor = CLAMP(cursor, 0, MAX(0, nw - 1));
	ui_refresh_look(ui);
	if (ui->nstack)
		sysview_set_cursor(ui, ui->stack[0], ui->sys_cursor);
	ui_invalidate_snapshot(ui);
	free(w);
	return CS_REBUILT | EU_LIST | EU_COUNTS;
}

/* --------------------------------------------------------------- snapshot */
static void snap_path(struct ui *ui, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", ui->cfg.cache_dir, SNAP_FILE);
}

/* Anything that changes which systems exist or how their lists are built. */
static uint64_t snap_key(struct ui *ui)
{
	uint64_t h = hash64_str(ui->cfg.roms_dir, SNAP_VERSION);

	for (int i = 0; i < systems_count(); i++) {
		uint64_t e = games_ext_key(systems_get(i)->name);

		h = hash64_str(systems_get(i)->name, h);
		h = hash64(&e, sizeof(e), h);
	}
	return h;
}

static bool snap_load(struct ui *ui)
{
	struct loader *L = ui->ld;
	char path[1100], *buf, *save = NULL, *line;
	bool ok = false;

	if (!ui->cfg.cache_dir[0])
		return false;
	snap_path(ui, path, sizeof(path));
	buf = file_read(path, NULL);
	if (!buf)
		return false;
	L->snap_text = xstrdup(buf);
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char kw[16], name[64];
		int count = 0, nfav = 0, off = 0;
		long long mt = 0;
		unsigned long long key = 0;
		unsigned ver = 0;

		if (sscanf(line, "%15s", kw) != 1)
			continue;
		if (!strcmp(kw, SNAP_MAGIC)) {
			if (sscanf(line, "%*s %u %llx", &ver, &key) != 2 || ver != SNAP_VERSION ||
			    key != (unsigned long long)snap_key(ui))
				break;
			ok = true;
		} else if (ok && !strcmp(kw, "sys") &&
			   sscanf(line, "%*s %63s %d %d %n", name, &count, &nfav, &off) == 3 && off > 0) {
			for (int s = 0; s < L->nsys; s++) {
				if (strcmp(systems_get(s)->name, name))
					continue;
				L->snap_count[s] = count;
				L->snap_nfav[s] = nfav;
				strlcpy_(L->snap_dir[s], line + off, sizeof(L->snap_dir[s]));
			}
		} else if (ok && !strcmp(kw, "empty") &&
			   sscanf(line, "%*s %63s %lld %n", name, &mt, &off) == 2 && off > 0 && mt) {
			for (int s = 0; s < L->nsys; s++) {
				if (strcmp(systems_get(s)->name, name))
					continue;
				L->empty_mt[s] = (int64_t)mt;
				strlcpy_(L->empty_dir[s], line + off, sizeof(L->empty_dir[s]));
			}
		} else if (ok && !strcmp(kw, "coll") && sscanf(line, "%*s %63s %d", name, &count) == 2) {
			for (int c = 0; c < 2; c++)
				if (!strcmp(name, g_coll_names[c]))
					L->snap_coll[c] = count;
		}
	}
	free(buf);
	if (!ok) {
		LOGI("ui: %s is from another setup, not used", path);
		for (int s = 0; s < L->nsys; s++) {
			L->snap_count[s] = -1;
			L->empty_mt[s] = 0;
		}
		L->snap_coll[0] = L->snap_coll[1] = 0;
	}
	return ok;
}

/* The carousel as it is now (complete lists only). */
static char *snap_build(struct ui *ui)
{
	struct loader *L = ui->ld;
	size_t cap = 256 + (size_t)(ui->nsys + L->nsys) * 640, len = 0;
	char *s = xmalloc(cap);

	len += (size_t)snprintf(s + len, cap - len, "%s %d %016llx\n", SNAP_MAGIC, SNAP_VERSION,
				(unsigned long long)snap_key(ui));
	for (int i = 0; i < ui->nsys; i++) {
		const struct sysent *se = &ui->sys[i];

		/* live counts: favorites may have changed since the load */
		int count = se->games ? se->games->n : se->count;

		if (se->is_collection)
			len += (size_t)snprintf(s + len, cap - len, "coll %s %d\n", se->name, count);
		else
			len += (size_t)snprintf(s + len, cap - len, "sys %s %d %d %s\n", se->name, count,
						se->games ? count_fav(se->games) : se->nfav, se->rom_dir);
	}
	for (int k = 0; k < L->nsys; k++)
		if (L->done[k] && !L->lists[k] && L->found_mt[k])
			len += (size_t)snprintf(s + len, cap - len, "empty %s %lld %s\n", systems_get(k)->name,
						(long long)L->found_mt[k], L->found_dir[k]);
	return s;
}

void loader_save_snapshot(struct ui *ui)
{
	struct loader *L = ui->ld;
	char path[1100];
	char *s;

	if (!L || !L->complete || !ui->cfg.cache_dir[0])
		return;
	s = snap_build(ui);
	if (L->snap_text && !strcmp(s, L->snap_text)) {
		free(s);
		return;
	}
	snap_path(ui, path, sizeof(path));
	mkdir_p(ui->cfg.cache_dir);
	if (file_write_atomic_nosync(path, s, strlen(s)) == 0) {
		img_note_written();
		free(L->snap_text);
		L->snap_text = s;
		LOGI("ui: carousel snapshot saved (%d entries)", ui->nsys);
	} else {
		LOGW("ui: cannot write %s", path);
		free(s);
	}
}

/* ------------------------------------------------------------------- apply */
static bool all_done(const struct loader *L)
{
	return L->ndone == L->nsys;
}

static uint64_t state_digest(struct ui *ui, int *ngames)
{
	uint64_t h = 0x5157;

	*ngames = 0;
	for (int i = 0; i < ui->nsys; i++) {
		h = hash64_str(ui->sys[i].name, h);
		h = games_digest(ui->sys[i].games, h);
		if (!ui->sys[i].is_collection && ui->sys[i].games)
			*ngames += ui->sys[i].games->n;
	}
	return h;
}

static void finish(struct ui *ui)
{
	struct loader *L = ui->ld;
	struct ui_io io;
	int ngames, nwith = 0;
	uint64_t dg;
	int64_t t = ui_now_us();

	build_coll(ui);
	L->complete = true;
	if ((carousel_sync(ui, false) & (CS_REBUILT | EU_COUNTS)) && !L->first_boot)
		L->carousel_changed = true;
	for (int s = 0; s < L->nsys; s++)
		nwith += L->lists[s] != NULL;
	dg = state_digest(ui, &ngames);
	ui->timings.ngames = ngames;
	ui->timings.nsystems = ui->nsys;
	ui->timings.systems_us = t - L->t_start;
	ui_io_sample(ui, &io);
	LOGI("ui: game lists complete: %d systems checked, %d with games, %d games, in %lld ms "
	     "(worker %lld ms; warm-up %d folders, %d reads, %lld KiB, %lld ms); carousel %s, "
	     "%d lists replaced; io %lld reads, %lld KiB; digest %016llx",
	     L->nsys, nwith, ngames, (long long)(t - L->t_start) / 1000, (long long)L->worker_us / 1000,
	     L->warm.dirs, L->warm.reads, (long long)L->warm.bytes / 1024, (long long)L->warm.us / 1000,
	     L->first_boot ? "built" : L->carousel_changed ? "updated" : "unchanged", L->nreplaced,
	     (long long)(io.reads - L->io_start.reads), (long long)(io.sectors - L->io_start.sectors) / 2,
	     (unsigned long long)dg);
	LOGI("ui: game list time by step: folder %lld us, cache read %lld us, validation stat %lld us, "
	     "decode %lld us, readdir %lld us, gamelist.xml %lld us, cache write %lld us; slowest %s %lld us",
	     (long long)L->sum.dir_us, (long long)L->sum.read_us, (long long)L->sum.stat_us,
	     (long long)L->sum.parse_us, (long long)L->sum.scan_us, (long long)L->sum.xml_us,
	     (long long)L->sum.save_us, L->max_name, (long long)L->max_us);
	loader_save_snapshot(ui);
}

/*
 * Takes the finished results and installs what can be installed now.
 * Returns true if the screen changed.
 */
static bool apply(struct ui *ui)
{
	struct loader *L = ui->ld;
	struct lres *q, *r;
	bool safe = ui->nstack <= 1, changed = false;

	pthread_mutex_lock(&L->mu);
	q = L->q_head;
	L->q_head = NULL;
	L->q_tail = &L->q_head;
	pthread_mutex_unlock(&L->mu);
	for (r = q; r; r = r->next) {
		struct gamelist *gl;
		char primary[1100];

		/* an empty folder without subfolders or gamelist.xml, under the
		 * system's own name: remembered with its mtime in the snapshot */
		snprintf(primary, sizeof(primary), "%s/%s", L->roms_dir, systems_get(r->sidx)->name);
		L->found_mt[r->sidx] = 0;
		if (r->gl && !r->gl->n && r->gl->ndirs == 1 && !r->gl->glpath[0] &&
		    !strcmp(r->gl->rom_dir, primary)) {
			L->found_mt[r->sidx] = r->gl->dir_mtime[0];
			strlcpy_(L->found_dir[r->sidx], primary, sizeof(L->found_dir[0]));
		}
		gl = prepare(ui, r->sidx, r->gl);

		log_result(ui, r, gl);
		r->gl = NULL;
		if (L->has_pend[r->sidx])
			games_free(L->pend[r->sidx]);
		L->pend[r->sidx] = gl;
		L->has_pend[r->sidx] = true;
	}
	lres_free(q);
	for (int s = 0; s < L->nsys; s++) {
		struct gamelist *gl = L->pend[s], *old = L->lists[s];
		struct sysent *se = NULL;

		if (!L->has_pend[s])
			continue;
		for (int i = 0; i < ui->nsys; i++)
			if (ui->sys[i].sidx == s && !ui->sys[i].is_collection)
				se = &ui->sys[i];
		if (!L->done[s] && gl && se && !se->games) {
			/* the first list for an entry that shows its snapshot */
			L->lists[s] = gl;
		} else if (old && gl && games_equal(old, gl)) {
			games_free(gl);
		} else if (!old && !gl && (L->done[s] || !se)) {
			/* nothing to show, nothing shown */
		} else if (!safe) {
			continue;                  /* a screen uses the entries: later */
		} else {
			if (se)
				se->games = NULL;
			games_free(old);
			L->lists[s] = gl;
			if (old || se)
				L->nreplaced++;    /* differs from what was shown */
		}
		L->pend[s] = NULL;
		L->has_pend[s] = false;
		if (!L->done[s]) {
			L->done[s] = true;
			L->ndone++;
		}
		changed = true;
	}
	if (L->first_boot) {
		ui->load_step = L->ndone;
		return changed;
	}
	if (changed && safe) {
		if (carousel_sync(ui, false) & (CS_REBUILT | EU_COUNTS))
			L->carousel_changed = true;
	} else if (changed) {
		/* entries do not move while a screen uses them: counts only */
		int upd = 0;

		for (int i = 0; i < ui->nsys; i++)
			upd |= entry_update(ui, &ui->sys[i]);
		if (upd & EU_COUNTS)
			L->carousel_changed = true;
		if (upd && ui->nstack)
			sysview_refresh_info(ui, ui->stack[0]);
	}
	if (!L->complete && all_done(L) && safe)
		finish(ui);
	return changed;
}

/* ------------------------------------------------------------------ public */
void loader_init(struct ui *ui)
{
	struct loader *L = xcalloc(1, sizeof(*L));
	const char *d = getenv("RSOS_LOADER_DELAY_MS");

	pthread_mutex_init(&L->mu, NULL);
	pthread_cond_init(&L->cv, NULL);
	L->q_tail = &L->q_head;
	L->nsys = systems_count();
	L->order = xcalloc((size_t)MAX(L->nsys, 1), sizeof(int));
	L->state = xcalloc((size_t)MAX(L->nsys, 1), 1);
	L->lists = xcalloc((size_t)MAX(L->nsys, 1), sizeof(*L->lists));
	L->done = xcalloc((size_t)MAX(L->nsys, 1), sizeof(bool));
	L->pend = xcalloc((size_t)MAX(L->nsys, 1), sizeof(*L->pend));
	L->has_pend = xcalloc((size_t)MAX(L->nsys, 1), sizeof(bool));
	L->snap_count = xcalloc((size_t)MAX(L->nsys, 1), sizeof(int));
	L->snap_nfav = xcalloc((size_t)MAX(L->nsys, 1), sizeof(int));
	L->snap_dir = xcalloc((size_t)MAX(L->nsys, 1), sizeof(*L->snap_dir));
	L->empty_mt = xcalloc((size_t)MAX(L->nsys, 1), sizeof(*L->empty_mt));
	L->empty_dir = xcalloc((size_t)MAX(L->nsys, 1), sizeof(*L->empty_dir));
	L->found_mt = xcalloc((size_t)MAX(L->nsys, 1), sizeof(*L->found_mt));
	L->found_dir = xcalloc((size_t)MAX(L->nsys, 1), sizeof(*L->found_dir));
	for (int s = 0; s < L->nsys; s++)
		L->snap_count[s] = -1;
	strlcpy_(L->roms_dir, ui->cfg.roms_dir, sizeof(L->roms_dir));
	strlcpy_(L->builtin_dir, ui->cfg.builtin_games_dir ? ui->cfg.builtin_games_dir : "", sizeof(L->builtin_dir));
	strlcpy_(L->cache_dir, ui->cfg.cache_dir, sizeof(L->cache_dir));
	snprintf(L->alt, sizeof(L->alt), "%s/gamelists", ui->cfg.data_dir);
	L->use_cache = ui->cfg.cache_dir[0] != 0;
	L->delay_us = d ? (int64_t)atoi(d) * 1000 : 0;
	ui->ld = L;
}

/* Job order: the selected system, then its neighbours, then the rest. */
static void make_order(struct ui *ui, int first)
{
	struct loader *L = ui->ld;
	int n = 0;

	if (first >= 0 && first < L->nsys)
		L->order[n++] = first;
	for (int d = 1; first >= 0 && d <= 2; d++)
		for (int sgn = 1; sgn >= -1; sgn -= 2) {
			/* neighbours in the carousel = neighbours among the systems
			 * that have entries */
			int k = 0;

			for (int i = 0; i < ui->nsys; i++)
				if (ui->sys[i].sidx == first)
					k = i;
			k += sgn * d;
			if (ui->nsys && ui->sys[((k % ui->nsys) + ui->nsys) % ui->nsys].sidx >= 0) {
				int s = ui->sys[((k % ui->nsys) + ui->nsys) % ui->nsys].sidx;
				bool dup = false;

				for (int j = 0; j < n; j++)
					dup |= L->order[j] == s;
				if (!dup)
					L->order[n++] = s;
			}
		}
	for (int s = 0; s < L->nsys; s++) {
		bool dup = false;

		for (int j = 0; j < n; j++)
			dup |= L->order[j] == s;
		if (!dup)
			L->order[n++] = s;
	}
	L->norder = n;
	L->next = 0;
}

static int restore_cursor(struct ui *ui)
{
	const char *last = settings_get(ui->settings, "last_system", "");

	for (int i = 0; i < ui->nsys; i++)
		if (!strcmp(ui->sys[i].name, last))
			return i;
	return 0;
}

/* The first ui_update(): the carousel from the snapshot, or the loading
 * screen and the worker. */
static void begin(struct ui *ui)
{
	struct loader *L = ui->ld;
	int64_t t0 = ui_now_us();

	L->started = true;
	L->t_start = t0;
	ui->load_started = t0;
	ui_io_sample(ui, &L->io_start);
	L->snap_ok = snap_load(ui);
	if (L->snap_ok) {
		const char *warm[64];
		int nwarm = 0, ngames = 0;
		struct fswarm_stats ws = { 0 };
		struct ui_io io;

		/* the themes look for <ROM folder>/theme.xml: warm those folders
		 * before the first theme loads (in carousel_sync) */
		for (int s = 0; s < L->nsys && nwarm < 64; s++)
			if (L->snap_count[s] > 0 && L->snap_dir[s][0])
				warm[nwarm++] = L->snap_dir[s];
		fswarm(warm, nwarm, 0, &ws);
		carousel_sync(ui, true);
		ui->sys_cursor = restore_cursor(ui);
		for (int i = 0; i < ui->nsys; i++)
			if (!ui->sys[i].is_collection)
				ngames += ui->sys[i].count;
		ui_push(ui, sysview_create(ui));
		ui->loaded = true;
		ui_apply_boot_settings(ui);
		make_order(ui, ui->nsys ? ui->sys[ui->sys_cursor].sidx : -1);
		L->want_worker = true;          /* after the first menu frame */
		ui->timings.menu_us = ui_now_us() - t0;
		ui_io_sample(ui, &io);
		LOGI("ui: menu from the snapshot: %d entries, %d games, in %lld us (warm-up %d folders, "
		     "%d reads, %lld us; io %lld reads); checking %d systems in the background",
		     ui->nsys, ngames, (long long)(ui_now_us() - t0), ws.dirs, ws.reads, (long long)ws.us,
		     (long long)(io.reads - L->io_start.reads), L->nsys);
		return;
	}
	/* first boot: loading screen until every list is in */
	L->first_boot = true;
	make_order(ui, -1);
	LOGI("ui: no carousel snapshot: loading %d systems", L->nsys);
	worker_start(ui);
}

bool loader_step(struct ui *ui)
{
	struct loader *L = ui->ld;
	bool changed = false;

	if (!L->started) {
		begin(ui);
		changed = true;
	}
	if (L->want_worker && ui->first_frame_done)
		worker_start(ui);
	if (L->complete && !L->th_started) {
		bool empty;

		pthread_mutex_lock(&L->mu);
		empty = !L->q_head;
		pthread_mutex_unlock(&L->mu);
		if (empty)
			return changed;
	}
	changed |= apply(ui);
	if (L->first_boot && all_done(L) && !L->complete) {
		/* the carousel appears now */
		finish(ui);
		ui->timings.menu_us = ui_now_us() - L->t_start;
		ui->sys_cursor = restore_cursor(ui);
		ui_push(ui, sysview_create(ui));
		ui->loaded = true;
		ui_apply_boot_settings(ui);
		L->first_boot = false;
		changed = true;
	}
	if (L->th_started) {
		bool exited;

		pthread_mutex_lock(&L->mu);
		exited = L->th_exited;
		pthread_mutex_unlock(&L->mu);
		if (exited) {
			pthread_join(L->th, NULL);
			L->th_started = false;
		}
	}
	return changed;
}

/* Polls while results can arrive. Results that wait for the carousel (a
 * screen uses the entries) need no polling: going back to it redraws. */
int loader_timeout(const struct ui *ui)
{
	struct loader *L = ui->ld;
	bool queued;

	if (!L || !L->started)
		return 0;
	if (L->want_worker || L->th_started)
		return POLL_MS;
	pthread_mutex_lock(&L->mu);
	queued = L->q_head != NULL;
	pthread_mutex_unlock(&L->mu);
	return queued || (!L->complete && ui->nstack <= 1 && all_done(L)) ? 0 : -1;
}

bool ui_lists_complete(const struct ui *ui)
{
	return ui->ld && ui->ld->complete;
}

void loader_pause(struct ui *ui, bool pause)
{
	struct loader *L = ui->ld;

	if (!L)
		return;
	pthread_mutex_lock(&L->mu);
	L->paused = pause;
	pthread_cond_broadcast(&L->cv);
	pthread_mutex_unlock(&L->mu);
}

/* Every list, now (collections need them all): this thread takes every
 * job the worker has not started, then waits for the one it is on. */
static void complete_now(struct ui *ui)
{
	struct loader *L = ui->ld;
	int *mine = xcalloc((size_t)MAX(L->nsys, 1), sizeof(int)), n = 0;
	bool busy;

	pthread_mutex_lock(&L->mu);
	for (int s = 0; s < L->nsys; s++)
		if (L->state[s] == LS_TODO) {
			L->state[s] = LS_BUSY;
			mine[n++] = s;
		}
	pthread_mutex_unlock(&L->mu);
	for (int i = 0; i < n; i++) {
		struct lres *r = load_one(L, mine[i]);

		r->by_main = true;
		pthread_mutex_lock(&L->mu);
		L->state[mine[i]] = LS_DONE;
		queue_push(L, r);
		pthread_mutex_unlock(&L->mu);
	}
	free(mine);
	pthread_mutex_lock(&L->mu);
	do {
		busy = false;
		for (int s = 0; s < L->nsys; s++)
			busy |= L->state[s] == LS_BUSY;
		if (busy)
			pthread_cond_wait(&L->cv, &L->mu);
	} while (busy);
	pthread_mutex_unlock(&L->mu);
	apply(ui);
}

void loader_complete_all(struct ui *ui)
{
	if (ui->ld && !ui->ld->complete)
		complete_now(ui);
}

struct sysent *ui_system_ready(struct ui *ui, struct sysent *se)
{
	struct loader *L = ui->ld;
	char name[32];
	int64_t t0 = ui_now_us();

	if (!se || se->games)
		return se;
	strlcpy_(name, se->name, sizeof(name));
	if (se->is_collection) {
		complete_now(ui);
	} else {
		load_now(L, se->sidx);
		apply(ui);
	}
	LOGI("ui: %s opened before its list arrived: loaded in %lld us", name,
	     (long long)(ui_now_us() - t0));
	for (int i = 0; i < ui->nsys; i++)
		if (!strcmp(ui->sys[i].name, name))
			return &ui->sys[i];
	/* TRANSLATORS: toast when a system opened from the carousel has no
	 * game any more (removed while the console was off) */
	ui_toastf(ui, "%s", _("No games found any more in this folder"));
	return NULL;
}

void loader_check_changes(struct ui *ui)
{
	static int64_t roms_mtime;
	struct loader *L = ui->ld;
	struct stat st;
	bool changed = false;

	if (!L->complete)
		return;   /* the validation still running covers it */
	if (stat(ui->cfg.roms_dir, &st) == 0) {
		int64_t mt = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;

		if (roms_mtime && mt != roms_mtime)
			changed = true;
		roms_mtime = mt;
	}
	for (int s = 0; s < L->nsys && !changed; s++)
		if (L->lists[s] && games_changed(L->lists[s]))
			changed = true;
	if (changed) {
		LOGI("ui: ROM folders changed, reloading the game lists");
		ui_reload_games(ui, false);
	}
}

/* Reloads every list now (scan cache when valid) and rebuilds the views. */
void ui_reload_games(struct ui *ui, bool force)
{
	struct loader *L = ui->ld;
	int64_t t0 = ui_now_us();
	struct fswarm_stats ws = { 0 };

	/* the views reference the systems: back to the carousel first */
	while (ui->nstack > 1)
		ui_pop(ui);
	if (ui->nstack)
		ui->stack[0]->ops->relayout(ui, ui->stack[0]);
	worker_stop(L);
	for (int i = 0; i < ui->nsys; i++)
		ui->sys[i].games = NULL;
	for (int s = 0; s < L->nsys; s++) {
		games_free(L->lists[s]);
		games_free(L->pend[s]);
		L->lists[s] = L->pend[s] = NULL;
		L->has_pend[s] = L->done[s] = false;
		L->state[s] = LS_TODO;
		if (force && ui->cfg.cache_dir[0])
			games_invalidate(ui->cfg.cache_dir, systems_get(s)->name);
	}
	free_coll(L);
	L->ndone = L->nreplaced = 0;
	L->carousel_changed = false;
	L->complete = false;
	L->started = true;
	L->want_worker = false;
	L->first_boot = false;
	memset(&L->sum, 0, sizeof(L->sum));
	L->max_us = 0;
	warm_lists(L, &ws);
	for (int s = 0; s < L->nsys; s++) {
		struct lres *r = load_one(L, s);

		r->by_main = true;
		L->state[s] = LS_DONE;
		pthread_mutex_lock(&L->mu);
		queue_push(L, r);
		pthread_mutex_unlock(&L->mu);
	}
	L->t_start = t0;
	L->warm = ws;
	L->worker_us = 0;
	apply(ui);
	if (!L->complete)
		finish(ui);
	carousel_sync(ui, true);
	LOGI("ui: game lists reloaded in %lld ms", (long long)(ui_now_us() - t0) / 1000);
}

void ui_debug_state(struct ui *ui, char *buf, size_t n)
{
	struct screen *top = ui_top(ui);
	size_t len = 0;
	int ng;
	uint64_t dg = state_digest(ui, &ng);

#define PUT(...) do { if (len < n) len += (size_t)snprintf(buf + len, n - len, __VA_ARGS__); } while (0)
	PUT("carousel=");
	for (int i = 0; i < ui->nsys; i++)
		PUT("%s%s:%d", i ? "," : "", ui->sys[i].name,
		    ui->sys[i].games ? ui->sys[i].games->n : ui->sys[i].count);
	PUT(";cursor=%s", ui->nsys ? ui->sys[CLAMP(ui->sys_cursor, 0, ui->nsys - 1)].name : "");
	if (!top)
		PUT(";top=none");
	else if (top->kind == SCR_SYSVIEW)
		PUT(";top=carousel");
	else if (top->kind == SCR_GLVIEW)
		PUT(";top=list:%s:%d", glview_system(top)->name,
		    glview_system(top)->games ? glview_system(top)->games->n : -1);
	else
		PUT(";top=other");
	PUT(";complete=%d;digest=%016llx", ui->ld && ui->ld->complete, (unsigned long long)dg);
#undef PUT
}

void loader_destroy(struct ui *ui)
{
	struct loader *L = ui->ld;

	if (!L)
		return;
	worker_stop(L);
	for (int i = 0; i < ui->nsys; i++)
		free_entry(&ui->sys[i]);
	free(ui->sys);
	ui->sys = NULL;
	ui->nsys = 0;
	for (int s = 0; s < L->nsys; s++) {
		games_free(L->lists[s]);
		games_free(L->pend[s]);
	}
	free_coll(L);
	free(L->order);
	free(L->state);
	free(L->lists);
	free(L->done);
	free(L->pend);
	free(L->has_pend);
	free(L->snap_count);
	free(L->snap_nfav);
	free(L->snap_dir);
	free(L->empty_mt);
	free(L->empty_dir);
	free(L->found_mt);
	free(L->found_dir);
	free(L->snap_text);
	pthread_mutex_destroy(&L->mu);
	pthread_cond_destroy(&L->cv);
	free(L);
	ui->ld = NULL;
}
