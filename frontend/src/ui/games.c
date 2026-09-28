/*
 * games.c - see games.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "games.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fswarm.h"
#include "systems.h"
#include "xml.h"

#define CACHE_MAGIC "RSGC"
/* 6: gamelist.xml hidden games are kept in the list, flagged (the loader
 * drops them unless Settings > Game lists > Show hidden games is on) */
#define CACHE_VERSION 6u
#define MAX_DIRS 256

/* ------------------------------------------------------------- helpers */
void games_clean_name(const char *file, char *out, size_t n)
{
	const char *base = path_basename(file);
	const char *dot = strrchr(base, '.');
	size_t len = dot && dot != base ? (size_t)(dot - base) : strlen(base);
	size_t o = 0;
	int depth = 0;
	bool space = false;

	for (size_t i = 0; i < len && o + 1 < n; i++) {
		char c = base[i];

		if (c == '(' || c == '[') {
			depth++;
			continue;
		}
		if ((c == ')' || c == ']') && depth > 0) {
			depth--;
			continue;
		}
		if (depth)
			continue;
		if (c == '_')
			c = ' ';
		if (c == ' ') {
			space = o > 0;
			continue;
		}
		if (space && o + 2 < n)
			out[o++] = ' ';
		space = false;
		out[o++] = c;
	}
	out[o] = 0;
	if (!o) {
		/* only tags: keep the raw name */
		if (len >= n)
			len = n - 1;
		memcpy(out, base, len);
		out[len] = 0;
	}
}

static int64_t mtime_ns(const struct stat *st)
{
	return (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
}

static int64_t parse_es_date(const char *s)
{
	struct tm tm;
	int y, mo, d, h = 0, mi = 0, se = 0;

	if (!s || strlen(s) < 8 || sscanf(s, "%4d%2d%2d", &y, &mo, &d) != 3)
		return 0;
	if (s[8] == 'T')
		sscanf(s + 9, "%2d%2d%2d", &h, &mi, &se);
	if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31)
		return 0;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = se;
	return (int64_t)timegm(&tm);
}

/* ---------------------------------------------------------------- scan */
struct scan {
	const char *system;
	const char *root;
	struct arena *a;
	struct game *games;
	int n, cap;
	/* directories seen, with mtimes (cache validation) */
	char *dirs[MAX_DIRS];
	int64_t dir_mtime[MAX_DIRS];
	int ndirs;
	/* files referenced by .cue/.m3u, hidden from the list */
	char **hide;
	int nhide, caphide;
};

static void hide_add(struct scan *sc, const char *dir, const char *rel)
{
	char p[1024];

	path_join(dir, rel, p, sizeof(p));
	if (sc->nhide == sc->caphide) {
		sc->caphide = sc->caphide ? sc->caphide * 2 : 32;
		sc->hide = xrealloc(sc->hide, sizeof(char *) * (size_t)sc->caphide);
	}
	sc->hide[sc->nhide++] = xstrdup(p);
}

static bool hidden_file(const struct scan *sc, const char *path)
{
	for (int i = 0; i < sc->nhide; i++)
		if (!strcmp(sc->hide[i], path))
			return true;
	return false;
}

/* Remembers the tracks of a .cue and the discs of a .m3u. */
static void parse_playlist(struct scan *sc, const char *dir, const char *path, bool cue)
{
	char *buf = file_read(path, NULL), *save = NULL, *line;

	if (!buf)
		return;
	for (line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		line = str_trim(line);
		if (cue) {
			char *q1, *q2;

			if (strncasecmp(line, "FILE", 4))
				continue;
			q1 = strchr(line, '"');
			q2 = q1 ? strchr(q1 + 1, '"') : NULL;
			if (q1 && q2) {
				*q2 = 0;
				hide_add(sc, dir, q1 + 1);
			}
		} else if (line[0] && line[0] != '#') {
			hide_add(sc, dir, line);
		}
	}
	free(buf);
}

/* BIOS and device sets that live next to arcade games but are not games. */
static bool arcade_bios(const char *system, const char *file)
{
	static const char *const bios[] = {
		"neogeo.zip", "aes.zip", "pgm.zip", "skns.zip", "stvbios.zip", "decocass.zip",
		"naomi.zip", "awbios.zip", "cvs.zip", "konamigx.zip", "nmk004.zip", "ym2608.zip",
		"isgsm.zip", "bubsys.zip", "qsound.zip", "hng64.zip", "neocdz.zip", "coleco.zip",
		"neogeo.7z", "pgm.7z", "skns.7z",
	};

	if (strcmp(system, "arcade") && strcmp(system, "fbneo") && strcmp(system, "neogeo"))
		return false;
	for (size_t i = 0; i < ARRAY_SIZE(bios); i++)
		if (!strcasecmp(file, bios[i]))
			return true;
	return false;
}

static bool skip_dir(const char *name)
{
	static const char *const skip[] = {
		"images", "videos", "media", "snap", "snaps", "manuals", "downloaded_images",
		"downloaded_media", "marquees", "boxart", "screenshots", "wheel", "titles",
	};

	if (name[0] == '.')
		return true;
	for (size_t i = 0; i < ARRAY_SIZE(skip); i++)
		if (!strcasecmp(name, skip[i]))
			return true;
	return false;
}

static void scan_dir(struct scan *sc, const char *dir, int depth)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	char **files = NULL;
	int nf = 0, cf = 0;

	if (depth > 4 || sc->ndirs >= MAX_DIRS || stat(dir, &st) < 0)
		return;
	sc->dirs[sc->ndirs] = xstrdup(dir);
	sc->dir_mtime[sc->ndirs++] = mtime_ns(&st);
	d = opendir(dir);
	if (!d)
		return;
	/* first pass: playlists (so their tracks can be hidden) */
	while ((de = readdir(d))) {
		char p[1024];
		const char *ext;

		/* hidden files, macOS "._x" / .DS_Store / .Trashes / .fseventsd /
		 * .Spotlight-V100, and Windows junk */
		if (de->d_name[0] == '.' || !strcasecmp(de->d_name, "System Volume Information") ||
		    !strcasecmp(de->d_name, "Thumbs.db") || !strcasecmp(de->d_name, "desktop.ini") ||
		    !strcasecmp(de->d_name, "$RECYCLE.BIN"))
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		if (de->d_type == DT_DIR || (de->d_type == DT_UNKNOWN && dir_exists(p))) {
			if (!skip_dir(de->d_name))
				scan_dir(sc, p, depth + 1);
			continue;
		}
		ext = strrchr(de->d_name, '.');
		if (!ext)
			continue;
		ext++;
		if (!strcasecmp(ext, "cue"))
			parse_playlist(sc, dir, p, true);
		else if (!strcasecmp(ext, "m3u"))
			parse_playlist(sc, dir, p, false);
		if (!systems_ext_ok(sc->system, ext) || arcade_bios(sc->system, de->d_name))
			continue;
		if (nf == cf) {
			cf = cf ? cf * 2 : 64;
			files = xrealloc(files, sizeof(char *) * (size_t)cf);
		}
		files[nf++] = xstrdup(p);
	}
	closedir(d);
	for (int i = 0; i < nf; i++) {
		if (!hidden_file(sc, files[i])) {
			struct game *g;
			char name[256];

			if (sc->n == sc->cap) {
				sc->cap = sc->cap ? sc->cap * 2 : 128;
				sc->games = xrealloc(sc->games, sizeof(*sc->games) * (size_t)sc->cap);
			}
			g = &sc->games[sc->n++];
			memset(g, 0, sizeof(*g));
			g->path = arena_strdup(sc->a, files[i]);
			g->rel = g->path + strlen(sc->root) + 1;
			games_clean_name(files[i], name, sizeof(name));
			g->name = arena_strdup(sc->a, name);
			g->rating = -1;
		}
		free(files[i]);
	}
	free(files);
}

/* Resolves a gamelist path ("./x", "~/x", absolute) against the ROM dir. */
static const char *gl_path(struct arena *a, const char *root, const char *p)
{
	char out[1024];

	if (!p || !*p)
		return NULL;
	if (p[0] == '~' && p[1] == '/') {
		const char *home = getenv("HOME");

		snprintf(out, sizeof(out), "%s%s", home ? home : "", p + 1);
	} else if (p[0] == '/') {
		snprintf(out, sizeof(out), "%s", p);
	} else {
		snprintf(out, sizeof(out), "%s/%s", root, p);
	}
	path_normalize(out);
	return arena_strdup(a, out);
}

static struct game *find_game(struct scan *sc, const char *abs)
{
	/* linear with a quick basename check; gamelists are loaded once and
	 * then cached, so this is fine even for a few thousand games */
	for (int i = 0; i < sc->n; i++)
		if (!strcmp(sc->games[i].path, abs))
			return &sc->games[i];
	/* absolute path from another machine (/home/pi/RetroPie/roms/nes/x):
	 * match on the part after "/<system>/" */
	{
		char key[64];
		const char *p;

		snprintf(key, sizeof(key), "/%s/", sc->system);
		p = strstr(abs, key);
		if (p) {
			p += strlen(key);
			for (int i = 0; i < sc->n; i++)
				if (!strcmp(sc->games[i].rel, p))
					return &sc->games[i];
		}
	}
	return NULL;
}

static void apply_gamelist(struct scan *sc, const char *xml_path)
{
	struct xml_doc *doc = xml_load(xml_path);
	const struct xml_node *root, *n;
	int matched = 0;

	if (!doc)
		return;
	root = doc->root;
	while (root && strcmp(root->name, "gameList"))
		root = root->next;
	if (!root) {
		LOGW("games: %s has no <gameList>", xml_path);
		xml_free(doc);
		return;
	}
	for (n = root->child; n; n = n->next) {
		const char *p, *v;
		struct game *g;
		const char *abs;

		if (strcmp(n->name, "game"))
			continue;
		p = xml_child_text(n, "path");
		if (!p || !*p)
			continue;
		abs = gl_path(sc->a, sc->root, p);
		g = find_game(sc, abs);
		if (!g)
			continue; /* ES also drops entries whose file is missing */
		matched++;
		if ((v = xml_child_text(n, "name")) && *v)
			g->name = arena_strdup(sc->a, v);
#define STR(field, tag) \
		if ((v = xml_child_text(n, tag)) && *v) g->field = arena_strdup(sc->a, v)
#define PATH(field, tag) \
		if ((v = xml_child_text(n, tag)) && *v) g->field = gl_path(sc->a, sc->root, v)
		STR(desc, "desc");
		PATH(image, "image");
		PATH(thumbnail, "thumbnail");
		PATH(marquee, "marquee");
		PATH(video, "video");
		STR(developer, "developer");
		STR(publisher, "publisher");
		STR(genre, "genre");
		STR(players, "players");
#undef STR
#undef PATH
		if ((v = xml_child_text(n, "rating")) && *v)
			g->rating = (float)atof(v);
		g->releasedate = parse_es_date(xml_child_text(n, "releasedate"));
		g->lastplayed = parse_es_date(xml_child_text(n, "lastplayed"));
		if ((v = xml_child_text(n, "playcount")))
			g->playcount = atoi(v);
		g->favorite = parse_bool(xml_child_text(n, "favorite"), false);
		g->hidden = parse_bool(xml_child_text(n, "hidden"), false);
		g->kidgame = parse_bool(xml_child_text(n, "kidgame"), false);
	}
	LOGI("games: %s: %d entries matched", xml_path, matched);
	xml_free(doc);
}

/* --------------------------------------------------------------- cache */
static void gl_add_dir(struct gamelist *gl, const char *d, int64_t mt)
{
	gl->dirs = xrealloc(gl->dirs, sizeof(char *) * (size_t)(gl->ndirs + 1));
	gl->dir_mtime = xrealloc(gl->dir_mtime, sizeof(int64_t) * (size_t)(gl->ndirs + 1));
	gl->dirs[gl->ndirs] = xstrdup(d);
	gl->dir_mtime[gl->ndirs++] = mt;
}

struct wbuf {
	char *p;
	size_t len, cap;
};

static void wb_put(struct wbuf *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2 + 4096;
		b->p = xrealloc(b->p, b->cap);
	}
	memcpy(b->p + b->len, d, n);
	b->len += n;
}

static void wb_u32(struct wbuf *b, uint32_t v) { wb_put(b, &v, 4); }
static void wb_i64(struct wbuf *b, int64_t v) { wb_put(b, &v, 8); }
static void wb_str(struct wbuf *b, const char *s)
{
	uint32_t l = s ? (uint32_t)strlen(s) : 0xffffffffu;

	wb_u32(b, l);
	if (s)
		wb_put(b, s, l);
}

struct rbuf {
	const char *p, *end;
	bool bad;
};

static bool rb_get(struct rbuf *b, void *d, size_t n)
{
	if (b->bad || (size_t)(b->end - b->p) < n) {
		b->bad = true;
		return false;
	}
	memcpy(d, b->p, n);
	b->p += n;
	return true;
}

static uint32_t rb_u32(struct rbuf *b) { uint32_t v = 0; rb_get(b, &v, 4); return v; }
static int64_t rb_i64(struct rbuf *b) { int64_t v = 0; rb_get(b, &v, 8); return v; }
static const char *rb_str(struct rbuf *b, struct arena *a)
{
	uint32_t l = rb_u32(b);
	const char *s;

	if (b->bad || l == 0xffffffffu)
		return NULL;
	if ((size_t)(b->end - b->p) < l) {
		b->bad = true;
		return NULL;
	}
	s = arena_strndup(a, b->p, l);
	b->p += l;
	return s;
}

uint64_t games_ext_key(const char *system)
{
	const struct core_info *cs[MAX_CORES_PER_SYSTEM];
	int n = systems_cores_for(system, cs, MAX_CORES_PER_SYSTEM);
	uint64_t h = 0x1234;

	for (int i = 0; i < n; i++) {
		h = hash64_str(cs[i]->exts, h);
		h ^= cs[i]->block_extract;
	}
	return h;
}

static void cache_file(const char *cache_dir, const char *system, char *out, size_t n)
{
	snprintf(out, n, "%s/scan/%s.bin", cache_dir, system);
}

static void gamelist_stat(const char *path, int64_t *mt, int64_t *sz)
{
	struct stat st;

	if (path && stat(path, &st) == 0) {
		*mt = mtime_ns(&st);
		*sz = st.st_size;
	} else {
		*mt = -1;
		*sz = -1;
	}
}

/* Returns true if the file was written (the caller syncs it later). */
static bool cache_save(const char *cache_dir, const struct scan *sc, const char *glpath,
		       const struct gamelist *gl)
{
	struct wbuf b = { 0 };
	char path[1024], dir[1024];
	int64_t mt, sz;
	bool ok;

	wb_put(&b, CACHE_MAGIC, 4);
	wb_u32(&b, CACHE_VERSION);
	wb_i64(&b, (int64_t)games_ext_key(sc->system));
	wb_u32(&b, (uint32_t)sc->ndirs);
	for (int i = 0; i < sc->ndirs; i++) {
		wb_str(&b, sc->dirs[i]);
		wb_i64(&b, sc->dir_mtime[i]);
	}
	gamelist_stat(glpath, &mt, &sz);
	wb_str(&b, glpath);
	wb_i64(&b, mt);
	wb_i64(&b, sz);
	wb_u32(&b, (uint32_t)gl->n);
	for (int i = 0; i < gl->n; i++) {
		const struct game *g = &gl->games[i];
		uint32_t flags = (g->hidden ? 1u : 0) | (g->kidgame ? 2u : 0) | (g->favorite ? 4u : 0);
		int32_t rating = (int32_t)(g->rating * 100000.0f);

		wb_str(&b, g->rel);
		wb_str(&b, g->name);
		wb_str(&b, g->desc);
		wb_str(&b, g->image);
		wb_str(&b, g->thumbnail);
		wb_str(&b, g->marquee);
		wb_str(&b, g->video);
		wb_str(&b, g->developer);
		wb_str(&b, g->publisher);
		wb_str(&b, g->genre);
		wb_str(&b, g->players);
		wb_put(&b, &rating, 4);
		wb_i64(&b, g->releasedate);
		wb_i64(&b, g->lastplayed);
		wb_u32(&b, (uint32_t)g->playcount);
		wb_u32(&b, flags);
	}
	cache_file(cache_dir, sc->system, path, sizeof(path));
	path_dirname(path, dir, sizeof(dir));
	mkdir_p(dir);
	ok = file_write_atomic_nosync(path, b.p, b.len) == 0;
	if (!ok)
		LOGW("games: cannot write the scan cache %s", path);
	free(b.p);
	return ok;
}

static struct gamelist *cache_load(const char *cache_dir, const char *system,
				   const char *rom_dir, struct games_load_stats *st)
{
	char path[1024];
	size_t len;
	char *buf;
	struct rbuf b;
	struct gamelist *gl = NULL;
	char magic[4];
	uint32_t ndirs, ngames;
	struct arena tmp;
	const char *dirs[MAX_DIRS];
	int64_t mts[MAX_DIRS], t = ui_now_us();

	cache_file(cache_dir, system, path, sizeof(path));
	buf = file_read(path, &len);
	st->read_us += ui_now_us() - t;
	if (!buf)
		return NULL;
	t = ui_now_us();
	b.p = buf;
	b.end = buf + len;
	b.bad = false;
	arena_init(&tmp, 4096);
	if (!rb_get(&b, magic, 4) || memcmp(magic, CACHE_MAGIC, 4) ||
	    rb_u32(&b) != CACHE_VERSION || (uint64_t)rb_i64(&b) != games_ext_key(system))
		goto out;
	ndirs = rb_u32(&b);
	if (b.bad || ndirs > MAX_DIRS)
		goto out;
	gl = xcalloc(1, sizeof(*gl));
	arena_init(&gl->arena, 65536);
	strlcpy_(gl->system, system, sizeof(gl->system));
	strlcpy_(gl->rom_dir, rom_dir, sizeof(gl->rom_dir));
	for (uint32_t i = 0; i < ndirs; i++) {
		dirs[i] = rb_str(&b, &tmp);
		mts[i] = rb_i64(&b);
		if (b.bad || !dirs[i] || (i == 0 && strcmp(dirs[i], rom_dir)))
			goto fail;
	}
	st->parse_us += ui_now_us() - t;
	/* validation: one stat() per scanned folder. Subfolders (games in
	 * folders) are warmed first: a cold exFAT folder costs ~110 ms. */
	t = ui_now_us();
	if (ndirs > 1)
		fswarm(dirs + 1, (int)ndirs - 1, 0, NULL);
	for (uint32_t i = 0; i < ndirs; i++) {
		struct stat s;

		if (stat(dirs[i], &s) < 0 || mtime_ns(&s) != mts[i]) {
			st->stat_us += ui_now_us() - t;
			goto fail;
		}
		gl_add_dir(gl, dirs[i], mts[i]);
	}
	st->ndirs = (int)ndirs;
	{
		const char *glp = rb_str(&b, &tmp);
		int64_t mt = rb_i64(&b), sz = rb_i64(&b), mt2, sz2;

		if (b.bad) {
			st->stat_us += ui_now_us() - t;
			goto fail;
		}
		gamelist_stat(glp, &mt2, &sz2);
		st->stat_us += ui_now_us() - t;
		if (mt != mt2 || sz != sz2)
			goto fail;
		strlcpy_(gl->glpath, glp ? glp : "", sizeof(gl->glpath));
		gl->gl_mtime = mt;
		gl->gl_size = sz;
	}
	t = ui_now_us();
	ngames = rb_u32(&b);
	if (b.bad || ngames > 1000000)
		goto fail;
	gl->games = xcalloc(ngames ? ngames : 1, sizeof(struct game));
	gl->n = (int)ngames;
	for (uint32_t i = 0; i < ngames && !b.bad; i++) {
		struct game *g = &gl->games[i];
		int32_t rating = 0;
		uint32_t flags;
		char p[1024];

		g->rel = rb_str(&b, &gl->arena);
		g->name = rb_str(&b, &gl->arena);
		g->desc = rb_str(&b, &gl->arena);
		g->image = rb_str(&b, &gl->arena);
		g->thumbnail = rb_str(&b, &gl->arena);
		g->marquee = rb_str(&b, &gl->arena);
		g->video = rb_str(&b, &gl->arena);
		g->developer = rb_str(&b, &gl->arena);
		g->publisher = rb_str(&b, &gl->arena);
		g->genre = rb_str(&b, &gl->arena);
		g->players = rb_str(&b, &gl->arena);
		rb_get(&b, &rating, 4);
		g->rating = (float)rating / 100000.0f;
		g->releasedate = rb_i64(&b);
		g->lastplayed = rb_i64(&b);
		g->playcount = (int)rb_u32(&b);
		flags = rb_u32(&b);
		g->hidden = flags & 1;
		g->kidgame = flags & 2;
		g->favorite = flags & 4;
		if (!g->rel || !g->name) {
			b.bad = true;
			break;
		}
		snprintf(p, sizeof(p), "%s/%s", rom_dir, g->rel);
		g->path = arena_strdup(&gl->arena, p);
		g->system = gl->system;
	}
	st->parse_us += ui_now_us() - t;
	if (!b.bad) {
		gl->from_cache = true;
		goto out;
	}
fail:
	games_free(gl);
	gl = NULL;
out:
	arena_free(&tmp);
	free(buf);
	return gl;
}

/* ---------------------------------------------------------------- load */
static int cmp_name(const void *a, const void *b)
{
	const struct game *x = a, *y = b;

	return str_casecmp_natural(x->name, y->name);
}

static int cmp_fav_name(const void *a, const void *b)
{
	const struct game *x = a, *y = b;

	if (x->favorite != y->favorite)
		return x->favorite ? -1 : 1;
	return str_casecmp_natural(x->name, y->name);
}

void games_sort(struct gamelist *gl, bool favorites_first)
{
	if (gl && gl->n > 1)
		qsort(gl->games, (size_t)gl->n, sizeof(struct game),
		      favorites_first ? cmp_fav_name : cmp_name);
}

static int cmp_playtime(const void *a, const void *b)
{
	const struct game *x = a, *y = b;

	if (x->playtime != y->playtime)
		return x->playtime > y->playtime ? -1 : 1;
	return str_casecmp_natural(x->name, y->name);
}

static int cmp_fav_playtime(const void *a, const void *b)
{
	const struct game *x = a, *y = b;

	if (x->favorite != y->favorite)
		return x->favorite ? -1 : 1;
	return cmp_playtime(a, b);
}

static int cmp_recent(const void *a, const void *b)
{
	const struct game *x = a, *y = b;

	if (x->lastplayed != y->lastplayed)
		return x->lastplayed > y->lastplayed ? -1 : 1;
	return str_casecmp_natural(x->name, y->name);
}

static int cmp_fav_recent(const void *a, const void *b)
{
	const struct game *x = a, *y = b;

	if (x->favorite != y->favorite)
		return x->favorite ? -1 : 1;
	return cmp_recent(a, b);
}

void games_sort_by(struct gamelist *gl, bool favorites_first, enum games_sort how)
{
	int (*cmp)(const void *, const void *);

	if (!gl || gl->n < 2)
		return;
	switch (how) {
	case GAMES_SORT_PLAYTIME:
		cmp = favorites_first ? cmp_fav_playtime : cmp_playtime;
		break;
	case GAMES_SORT_LASTPLAYED:
		cmp = favorites_first ? cmp_fav_recent : cmp_recent;
		break;
	default:
		cmp = favorites_first ? cmp_fav_name : cmp_name;
		break;
	}
	qsort(gl->games, (size_t)gl->n, sizeof(struct game), cmp);
}

int games_drop_hidden(struct gamelist *gl)
{
	int w = 0, n;

	if (!gl)
		return 0;
	for (int i = 0; i < gl->n; i++)
		if (!gl->games[i].hidden)
			gl->games[w++] = gl->games[i];
	n = gl->n - w;
	gl->n = w;
	return n;
}

bool games_remove(struct gamelist *gl, const char *path)
{
	for (int i = 0; gl && i < gl->n; i++)
		if (!strcmp(gl->games[i].path, path)) {
			memmove(&gl->games[i], &gl->games[i + 1], sizeof(struct game) * (size_t)(gl->n - i - 1));
			gl->n--;
			return true;
		}
	return false;
}

/* Latin-1 Supplement and Latin Extended-A letters without their accent
 * (lowercase); 0 = not a letter we fold. */
static char fold_accent(uint32_t cp)
{
	static const char l1[] =   /* U+00C0..U+00FF */
		"aaaaaaaceeeeiiii" "dnooooo\0ouuuuyts"
		"aaaaaaaceeeeiiii" "dnooooo\0ouuuuyty";
	static const char la[] =   /* U+0100..U+017F */
		"aaaaaaccccccccdd" "ddeeeeeeeeeegggg" "gggghhhhiiiiiiii" "iijjjjkkklllllll"
		"lllnnnnnnnnnoooo" "oooorrrrrrssssss" "ssttttttuuuuuuuu" "uuuuwwyyyzzzzzzs";

	if (cp >= 0xc0 && cp <= 0xff)
		return l1[cp - 0xc0];
	if (cp >= 0x100 && cp <= 0x17f)
		return la[cp - 0x100];
	return 0;
}

static uint32_t utf8_cp(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p;
	int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

	if (!c)
		return 0;
	if (n)
		c &= 0x3f >> n;
	(*s)++;
	while (n-- > 0 && ((unsigned char)**s & 0xc0) == 0x80) {
		c = (c << 6) | ((unsigned char)**s & 0x3f);
		(*s)++;
	}
	return c;
}

/* The next character of s, lowercased and without its accent (ASCII), or
 * the code point itself for other scripts. */
static uint32_t fold_next(const char **s)
{
	uint32_t c = utf8_cp(s);
	char f;

	if (c >= 'A' && c <= 'Z')
		return c + 32;
	if (c >= 0x80 && (f = fold_accent(c)))
		return (uint32_t)(unsigned char)f;
	return c;
}

void games_letter(const char *name, char *out, size_t n)
{
	const char *s = name ? name : "";
	uint32_t c;

	/* the first letter or digit: "'Splosion Man" is under S, "#" too */
	do {
		c = fold_next(&s);
	} while (c && c < 0x80 && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')));
	if (!n)
		return;
	if (c >= 'a' && c <= 'z') {
		snprintf(out, n, "%c", (char)(c - 32));
	} else if (c >= 0x80) {
		/* another script: the character itself */
		const char *p = name, *start;

		do {
			start = p;
		} while (fold_next(&p) != c && *p);
		snprintf(out, n, "%.*s", (int)(p - start), start);
	} else {
		snprintf(out, n, "#");
	}
}

bool games_match(const char *name, const char *query)
{
	if (!query || !*query)
		return true;
	for (const char *h = name ? name : ""; *h; ) {
		const char *a = h, *b = query;
		uint32_t ca, cb;

		for (;;) {
			const char *bn = b;

			cb = fold_next(&bn);
			if (!cb)
				return true;
			ca = fold_next(&a);
			if (ca != cb)
				break;
			b = bn;
		}
		fold_next(&h);
	}
	return false;
}

struct gamelist *games_load(const char *system, const char *rom_dir,
			    const char *cache_dir, const char *alt_gamelists,
			    struct games_load_stats *st)
{
	struct gamelist *gl;
	struct scan sc;
	char glpath[1024];
	int64_t t0 = ui_now_us(), t;
	bool have_gl = false;
	struct games_load_stats dummy;

	if (!st)
		st = &dummy;
	memset(st, 0, sizeof(*st));
	if (!dir_exists(rom_dir)) {
		st->dir_us = st->total_us = ui_now_us() - t0;
		return NULL;
	}
	st->dir_us = ui_now_us() - t0;
	if (cache_dir && (gl = cache_load(cache_dir, system, rom_dir, st))) {
		gl->load_us = st->total_us = ui_now_us() - t0;
		st->from_cache = true;
		for (int i = 0; i < gl->n; i++)
			if (gl->games[i].image || gl->games[i].desc)
				gl->has_media = true;
		return gl;
	}
	gl = xcalloc(1, sizeof(*gl));
	arena_init(&gl->arena, 65536);
	strlcpy_(gl->system, system, sizeof(gl->system));
	strlcpy_(gl->rom_dir, rom_dir, sizeof(gl->rom_dir));
	memset(&sc, 0, sizeof(sc));
	sc.system = gl->system;
	sc.root = gl->rom_dir;
	sc.a = &gl->arena;
	t = ui_now_us();
	scan_dir(&sc, rom_dir, 0);
	st->scan_us = ui_now_us() - t;
	st->ndirs = sc.ndirs;

	t = ui_now_us();
	snprintf(glpath, sizeof(glpath), "%s/gamelist.xml", rom_dir);
	if (file_exists(glpath)) {
		have_gl = true;
	} else if (alt_gamelists) {
		snprintf(glpath, sizeof(glpath), "%s/%s/gamelist.xml", alt_gamelists, system);
		have_gl = file_exists(glpath);
	}
	if (have_gl)
		apply_gamelist(&sc, glpath);
	st->xml_us = ui_now_us() - t;
	/* hidden games (gamelist.xml hidden=true) stay in the list, flagged: the
	 * loader drops them unless "Show hidden games" is on (ES) */
	for (int i = 0; i < sc.n; i++)
		sc.games[i].system = gl->system;
	gl->games = sc.games;
	gl->n = sc.n;
	games_sort(gl, false);
	for (int i = 0; i < gl->n; i++)
		if (gl->games[i].image || gl->games[i].desc)
			gl->has_media = true;
	if (cache_dir) {
		t = ui_now_us();
		gl->cache_written = cache_save(cache_dir, &sc, have_gl ? glpath : NULL, gl);
		st->save_us = ui_now_us() - t;
	}
	for (int i = 0; i < sc.ndirs; i++) {
		gl_add_dir(gl, sc.dirs[i], sc.dir_mtime[i]);
		free(sc.dirs[i]);
	}
	if (have_gl) {
		strlcpy_(gl->glpath, glpath, sizeof(gl->glpath));
		gamelist_stat(glpath, &gl->gl_mtime, &gl->gl_size);
	} else {
		gl->gl_mtime = gl->gl_size = -1;
	}
	for (int i = 0; i < sc.nhide; i++)
		free(sc.hide[i]);
	free(sc.hide);
	gl->load_us = st->total_us = ui_now_us() - t0;
	return gl;
}

void games_free(struct gamelist *gl)
{
	if (!gl)
		return;
	for (int i = 0; i < gl->ndirs; i++)
		free(gl->dirs[i]);
	free(gl->dirs);
	free(gl->dir_mtime);
	free(gl->games);
	arena_free(&gl->arena);
	free(gl);
}

static bool str_eq(const char *a, const char *b)
{
	return a == b || (a && b && !strcmp(a, b));
}

static int32_t rating_q(float r)
{
	return (int32_t)(r * 100000.0f + (r < 0 ? -0.5f : 0.5f));
}

static bool game_eq(const struct game *x, const struct game *y)
{
	return str_eq(x->path, y->path) && str_eq(x->name, y->name) && str_eq(x->desc, y->desc) &&
	       str_eq(x->image, y->image) && str_eq(x->thumbnail, y->thumbnail) &&
	       str_eq(x->marquee, y->marquee) && str_eq(x->video, y->video) &&
	       str_eq(x->developer, y->developer) && str_eq(x->publisher, y->publisher) &&
	       str_eq(x->genre, y->genre) && str_eq(x->players, y->players) &&
	       /* the scan cache keeps ratings in 1/100000 */
	       rating_q(x->rating) == rating_q(y->rating) &&
	       x->releasedate == y->releasedate &&
	       x->lastplayed == y->lastplayed && x->playcount == y->playcount &&
	       x->favorite == y->favorite && x->hidden == y->hidden && x->kidgame == y->kidgame &&
	       str_eq(x->core, y->core);
}

bool games_equal(const struct gamelist *a, const struct gamelist *b)
{
	if (!a || !b)
		return a == b;
	if (a->n != b->n)
		return false;
	for (int i = 0; i < a->n; i++)
		if (!game_eq(&a->games[i], &b->games[i]))
			return false;
	return true;
}

uint64_t games_digest(const struct gamelist *gl, uint64_t h)
{
	if (!gl)
		return hash64("", 0, h);
	h = hash64(&gl->n, sizeof(gl->n), h);
	for (int i = 0; i < gl->n; i++) {
		const struct game *g = &gl->games[i];
		int flags = g->favorite | g->kidgame << 1 | g->hidden << 2;

		h = hash64_str(g->path, h);
		h = hash64_str(g->name, h);
		h = hash64_str(g->desc ? g->desc : "", h);
		h = hash64_str(g->image ? g->image : "", h);
		h = hash64(&flags, sizeof(flags), h);
		h = hash64(&g->playcount, sizeof(g->playcount), h);
		h = hash64(&g->lastplayed, sizeof(g->lastplayed), h);
	}
	return h;
}

/* --------------------------------------------------------------- gamedb */
struct dbent {
	char *system, *rel;
	const char *core;             /* interned (gamedb.cores), never freed alone */
	int64_t lastplayed;
	int playcount;
	bool favorite;
	/* batch 2 */
	int64_t playtime;             /* seconds */
	bool hidden;
	const char *scale, *cpu;      /* interned like core */
};

struct gamedb {
	char path[1024];
	struct dbent *e;
	int n, cap;
	bool dirty;
	/* Core ids, interned for the database's lifetime: list entries and
	 * the Favorites / Last played copies all point at them (review F-H2:
	 * changing a game's core freed a string the copies still used). */
	char **cores;
	int ncores, cap_cores;
	bool read_failed;             /* the file exists but could not be read */
};

static const char *db_intern_core(struct gamedb *db, const char *core)
{
	for (int i = 0; i < db->ncores; i++)
		if (!strcmp(db->cores[i], core))
			return db->cores[i];
	if (db->ncores == db->cap_cores) {
		db->cap_cores = db->cap_cores ? db->cap_cores * 2 : 16;
		db->cores = xrealloc(db->cores, sizeof(*db->cores) * (size_t)db->cap_cores);
	}
	db->cores[db->ncores] = xstrdup(core);
	return db->cores[db->ncores++];
}

static struct dbent *db_find(struct gamedb *db, const char *system, const char *rel, bool create)
{
	for (int i = 0; i < db->n; i++)
		if (!strcmp(db->e[i].system, system) && !strcmp(db->e[i].rel, rel))
			return &db->e[i];
	if (!create)
		return NULL;
	if (db->n == db->cap) {
		db->cap = db->cap ? db->cap * 2 : 64;
		db->e = xrealloc(db->e, sizeof(*db->e) * (size_t)db->cap);
	}
	memset(&db->e[db->n], 0, sizeof(db->e[0]));
	db->e[db->n].system = xstrdup(system);
	db->e[db->n].rel = xstrdup(rel);
	return &db->e[db->n++];
}

struct gamedb *gamedb_open(const char *path)
{
	struct gamedb *db = xcalloc(1, sizeof(*db));
	char *buf, *save = NULL, *line;
	bool from_bak;
	int err;

	strlcpy_(db->path, path, sizeof(db->path));
	buf = file_read_user(path, NULL, &err, &from_bak);
	/* read error: the favourites and history come from the .bak if it can
	 * be read, and the file is never rewritten (review F-M7: the next save
	 * used to erase them all) */
	db->read_failed = err && err != -ENOENT;
	if (!buf)
		return db;
	/* system \t rel \t favorite \t lastplayed \t playcount \t core
	 * [\t playtime \t hidden \t scale \t cpu] (batch 2; older files have 6) */
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[10] = { 0 };
		int nf = 0;
		char *p = line;
		struct dbent *e;

		if (line[0] == '#')
			continue;
		while (nf < 10) {
			f[nf++] = p;
			p = strchr(p, '\t');
			if (!p)
				break;
			*p++ = 0;
		}
		if (nf < 2 || !*f[0] || !*f[1])
			continue;
		e = db_find(db, f[0], f[1], true);
		e->favorite = nf > 2 && atoi(f[2]);
		e->lastplayed = nf > 3 ? strtoll(f[3], NULL, 10) : 0;
		e->playcount = nf > 4 ? atoi(f[4]) : 0;
		if (nf > 5 && *str_trim(f[5]))
			e->core = db_intern_core(db, str_trim(f[5]));
		e->playtime = nf > 6 ? MAX(0, strtoll(f[6], NULL, 10)) : 0;
		e->hidden = nf > 7 && atoi(f[7]);
		if (nf > 8 && *str_trim(f[8]))
			e->scale = db_intern_core(db, str_trim(f[8]));
		if (nf > 9 && *str_trim(f[9]))
			e->cpu = db_intern_core(db, str_trim(f[9]));
	}
	free(buf);
	return db;
}

void gamedb_close(struct gamedb *db)
{
	if (!db)
		return;
	for (int i = 0; i < db->n; i++) {
		free(db->e[i].system);
		free(db->e[i].rel);
	}
	for (int i = 0; i < db->ncores; i++)
		free(db->cores[i]);
	free(db->cores);
	free(db->e);
	free(db);
}

void gamedb_apply(struct gamedb *db, struct gamelist *gl)
{
	if (!db || !gl)
		return;
	for (int i = 0; i < gl->n; i++) {
		struct game *g = &gl->games[i];
		struct dbent *e = db_find(db, g->system, g->rel, false);

		if (!e)
			continue;
		g->favorite = e->favorite || g->favorite;
		if (e->lastplayed > g->lastplayed)
			g->lastplayed = e->lastplayed;
		if (e->playcount > g->playcount)
			g->playcount = e->playcount;
		g->core = e->core;
		g->playtime = e->playtime;
		g->hidden = e->hidden || g->hidden;
		g->scale = e->scale;
		g->cpu = e->cpu;
	}
}

void gamedb_add_playtime(struct gamedb *db, struct game *g, int64_t seconds)
{
	struct dbent *e;

	if (seconds <= 0)
		return;
	e = db_find(db, g->system, g->rel, true);
	e->playtime += seconds;
	g->playtime = e->playtime;
	db->dirty = true;
}

int64_t gamedb_playtime(struct gamedb *db, const char *system, const char *rel)
{
	struct dbent *e = db_find(db, system, rel, false);

	return e ? e->playtime : 0;
}

void gamedb_set_hidden(struct gamedb *db, struct game *g, bool hidden)
{
	struct dbent *e = db_find(db, g->system, g->rel, true);

	e->hidden = hidden;
	g->hidden = hidden;
	db->dirty = true;
}

void gamedb_set_scale(struct gamedb *db, struct game *g, const char *scale)
{
	struct dbent *e = db_find(db, g->system, g->rel, true);

	e->scale = scale && *scale ? db_intern_core(db, scale) : NULL;
	g->scale = e->scale;
	db->dirty = true;
}

void gamedb_set_cpu(struct gamedb *db, struct game *g, const char *cpu)
{
	struct dbent *e = db_find(db, g->system, g->rel, true);

	e->cpu = cpu && *cpu && strcmp(cpu, "auto") ? db_intern_core(db, cpu) : NULL;
	g->cpu = e->cpu;
	db->dirty = true;
}

/* A deleted game: its line goes (the play time, the favorite...). */
void gamedb_forget(struct gamedb *db, const char *system, const char *rel)
{
	struct dbent *e = db_find(db, system, rel, false);

	if (!e)
		return;
	e->favorite = e->hidden = false;
	e->lastplayed = e->playtime = 0;
	e->playcount = 0;
	e->core = e->scale = e->cpu = NULL;
	db->dirty = true;
}

void gamedb_set_favorite(struct gamedb *db, struct game *g, bool fav)
{
	struct dbent *e = db_find(db, g->system, g->rel, true);

	e->favorite = fav;
	g->favorite = fav;
	db->dirty = true;
}

void gamedb_set_core(struct gamedb *db, struct game *g, const char *core)
{
	struct dbent *e = db_find(db, g->system, g->rel, true);

	/* the old string stays valid: other copies of this game may use it
	 * until the lists are rebuilt (gamedb_apply) */
	e->core = core && *core ? db_intern_core(db, core) : NULL;
	g->core = e->core;
	db->dirty = true;
}

void gamedb_played(struct gamedb *db, struct game *g, int64_t when)
{
	struct dbent *e = db_find(db, g->system, g->rel, true);

	e->lastplayed = when;
	e->playcount = MAX(e->playcount, g->playcount) + 1;
	g->lastplayed = when;
	g->playcount = e->playcount;
	db->dirty = true;
}

int gamedb_save(struct gamedb *db)
{
	struct wbuf b = { 0 };
	char dir[1024];
	int r;

	if (!db->dirty)
		return 0;
	if (db->read_failed) {
		LOGW("gamedb: %s could not be read at start: not rewritten", db->path);
		return -EIO;
	}
	{
		static const char hdr[] = "# RetroStoneOS per-game data: system, path, favorite, lastplayed, playcount, "
					  "core, playtime, hidden, scale, cpu\n";

		wb_put(&b, hdr, sizeof(hdr) - 1);
	}
	for (int i = 0; i < db->n; i++) {
		struct dbent *e = &db->e[i];
		char line[2048];
		int l;

		if (!e->favorite && !e->lastplayed && !e->playcount && !e->core && !e->playtime && !e->hidden &&
		    !e->scale && !e->cpu)
			continue;
		l = snprintf(line, sizeof(line), "%s\t%s\t%d\t%lld\t%d\t%s\t%lld\t%d\t%s\t%s\n", e->system, e->rel,
			     e->favorite ? 1 : 0, (long long)e->lastplayed, e->playcount,
			     e->core ? e->core : "", (long long)e->playtime, e->hidden ? 1 : 0,
			     e->scale ? e->scale : "", e->cpu ? e->cpu : "");
		wb_put(&b, line, (size_t)MIN(l, (int)sizeof(line) - 1));
	}
	path_dirname(db->path, dir, sizeof(dir));
	mkdir_p(dir);
	r = file_write_atomic_bak(db->path, b.p, b.len);
	free(b.p);
	if (r == 0)
		db->dirty = false;
	return r;
}

void gamedb_each(struct gamedb *db, bool favorites, gamedb_iter_fn fn, void *user)
{
	for (int i = 0; i < db->n; i++) {
		struct dbent *e = &db->e[i];

		if (favorites ? e->favorite : e->lastplayed > 0)
			fn(e->system, e->rel, e->lastplayed, user);
	}
}

bool games_changed(const struct gamelist *gl)
{
	struct stat st;
	int64_t mt, sz;

	if (!gl)
		return false;
	for (int i = 0; i < gl->ndirs; i++)
		if (stat(gl->dirs[i], &st) < 0 || mtime_ns(&st) != gl->dir_mtime[i])
			return true;
	gamelist_stat(gl->glpath[0] ? gl->glpath : NULL, &mt, &sz);
	return mt != gl->gl_mtime || sz != gl->gl_size;
}

void games_invalidate(const char *cache_dir, const char *system)
{
	char p[1024];

	cache_file(cache_dir, system, p, sizeof(p));
	unlink(p);
}
