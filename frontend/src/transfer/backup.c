/*
 * backup.c - "Export games" and "Back up saves" (docs/rom-transfer.md §2.5).
 *
 * Console -> stick, into RetroStone2/ at the stick root, in the layout the
 * import reads (roms/<system>/..., bios/, saves/<system>/...,
 * states/<system>/..., rsos/coreopts/, rsos/remaps/), so any RetroStone2 can
 * import it back. Incremental: a file is copied only when it is missing or
 * different on the stick (games and BIOS: same size and mtime within 2 s,
 * the FAT resolution, means identical; saves and settings are compared by
 * content). Nothing is ever deleted on the stick; a save or settings file
 * replaced there keeps the previous stick version as <name>.bak.
 *
 * Safety: every file goes through the sink (".<name>.<random>.rsos-part", fsync,
 * rename, fsync of the folder), so a cancel, a full stick or a pulled stick
 * leaves only complete files; the partial one is removed. Two markers are
 * (re)written at the end: .rsos-backup (console id, dates: how the import
 * recognises the folder) and RetroStone2-backup.txt (the same for humans).
 * The UI remounts the stick read-write only around the copy
 * (transfer_usb_remount).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "tr_internal.h"

#define BK_DEPTH 12
#define MARKER_TXT "RetroStone2-backup.txt"

enum { BK_NEW = 0, BK_SAME, BK_CHANGED };

struct bk_item {
	char *src;           /* absolute */
	char *rel;           /* "roms/snes/Game.sfc", relative to the backup folder */
	uint64_t size;
	int64_t mtime_s;
	long mtime_ns;
	uint8_t cat;
	uint8_t state;       /* BK_NEW / BK_SAME / BK_CHANGED against the stick */
	bool big;            /* over the FAT32 limit on a FAT32 stick */
};

struct transfer_backup {
	struct transfer_backup_info info;
	struct bk_item *items;
	int n, cap;
};

static const struct {
	const char *dir;
	int cat;
} g_dirs[] = {
	{ "roms", TRANSFER_BK_ROMS }, { "bios", TRANSFER_BK_BIOS },
	{ "saves", TRANSFER_BK_SAVES }, { "states", TRANSFER_BK_SAVES },
	{ "rsos/coreopts", TRANSFER_BK_CONFIG }, { "rsos/remaps", TRANSFER_BK_CONFIG },
};

void transfer_backup_defaults(struct transfer_backup_opts *o, enum transfer_backup_mode mode)
{
	memset(o, 0, sizeof(*o));
	o->mode = mode;
	o->bios = true;
	o->settings = false;
}

static bool precious(int cat)
{
	return cat == TRANSFER_BK_SAVES || cat == TRANSFER_BK_CONFIG;
}

static bool in_mode(int cat, enum transfer_backup_mode mode)
{
	return mode == TRANSFER_EXPORT_GAMES ? cat == TRANSFER_BK_ROMS || cat == TRANSFER_BK_BIOS :
	       precious(cat);
}

/* ------------------------------------------------------------ scan */

static uint64_t round_up(uint64_t v, uint32_t unit)
{
	return unit ? (v + unit - 1) / unit * unit : v;
}

/* The copy on the stick against the console's file. */
static uint8_t compare_dst(const char *dst, const struct bk_item *it)
{
	struct stat st;
	int64_t dt;

	if (lstat(dst, &st) < 0 || !S_ISREG(st.st_mode))
		return BK_NEW;
	if ((uint64_t)st.st_size != it->size)
		return BK_CHANGED;
	if (precious(it->cat)) {
		/* small and precious: the content decides, whatever the dates */
		int a = open(dst, O_RDONLY | O_CLOEXEC), b = open(it->src, O_RDONLY | O_CLOEXEC);
		int same = a >= 0 && b >= 0 ? tr_same_content(a, b) : 0;

		if (a >= 0)
			close(a);
		if (b >= 0)
			close(b);
		return same == 1 ? BK_SAME : BK_CHANGED;
	}
	dt = (int64_t)st.st_mtim.tv_sec - it->mtime_s;
	return dt >= -2 && dt <= 2 ? BK_SAME : BK_CHANGED;
}

static int bk_add(struct transfer_backup *b, const char *src, const char *rel, const struct stat *st,
		  int cat)
{
	struct transfer_backup_info *in = &b->info;
	struct bk_item *it;
	uint64_t size = (uint64_t)st->st_size;

	if (b->n == b->cap) {
		int cap = b->cap ? b->cap * 2 : 256;
		struct bk_item *n = realloc(b->items, sizeof(*n) * (size_t)cap);

		if (!n)
			return -ENOMEM;
		b->items = n;
		b->cap = cap;
	}
	it = &b->items[b->n];
	memset(it, 0, sizeof(*it));
	it->src = strdup(src);
	it->rel = strdup(rel);
	if (!it->src || !it->rel) {
		free(it->src);
		free(it->rel);
		return -ENOMEM;
	}
	it->size = size;
	it->mtime_s = st->st_mtim.tv_sec;
	it->mtime_ns = st->st_mtim.tv_nsec;
	it->cat = (uint8_t)cat;
	it->big = in->fat32 && size > TRANSFER_FAT32_MAX;
	it->state = BK_NEW;
	if (!it->big && in->exists) {
		char dst[TRANSFER_PATH_MAX * 2];

		if (tr_snprintf(dst, sizeof(dst), "%s/%s/%s", in->stick_root, in->folder, rel) == 0)
			it->state = compare_dst(dst, it);
	}
	b->n++;
	in->files[cat]++;
	in->bytes[cat] += size;
	if (it->big && in->nbig < 8)
		tr_strlcpy(in->big[in->nbig], rel, sizeof(in->big[0]));
	in->nbig += it->big;
	return 0;
}

static int bk_walk(struct transfer_backup *b, const char *dir, const char *rel, int cat, int depth)
{
	DIR *d;
	struct dirent *de;
	int r = 0;

	if (depth > BK_DEPTH || !(d = opendir(dir)))
		return 0;
	while ((de = readdir(d)) && r == 0) {
		char src[TRANSFER_PATH_MAX], nrel[TRANSFER_PATH_MAX];
		struct stat st;

		/* dotfiles (our own .rsos-part temp files included) and junk */
		if (tr_is_junk(de->d_name) ||
		    tr_snprintf(src, sizeof(src), "%s/%s", dir, de->d_name) < 0 ||
		    tr_snprintf(nrel, sizeof(nrel), "%s/%s", rel, de->d_name) < 0 || lstat(src, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode))
			r = bk_walk(b, src, nrel, cat, depth + 1);
		else if (S_ISREG(st.st_mode))
			r = bk_add(b, src, nrel, &st, cat);
	}
	closedir(d);
	return r;
}

int transfer_backup_scan(const char *data_root, const char *stick_root, const char *fstype,
			 enum transfer_backup_mode mode, struct transfer_backup **out)
{
	struct transfer_backup *b;
	struct transfer_backup_info *in;
	struct statvfs v;
	struct stat st;
	char p[TRANSFER_PATH_MAX * 2];
	int64_t t0 = tr_now_ms();
	int r = 0, same = 0, changed = 0, fresh = 0;

	*out = NULL;
	if (statvfs(stick_root, &v) < 0)
		return -errno;
	b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	in = &b->info;
	in->mode = mode;
	tr_strlcpy(in->data_root, data_root, sizeof(in->data_root));
	tr_strlcpy(in->stick_root, stick_root, sizeof(in->stick_root));
	tr_strlcpy(in->folder, TRANSFER_BACKUP_DIR, sizeof(in->folder));
	in->fat32 = fstype && (!strcmp(fstype, "vfat") || !strcmp(fstype, "msdos") || !strcmp(fstype, "fat"));
	in->free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
	in->cluster = (uint32_t)(v.f_bsize ? v.f_bsize : v.f_frsize);
	if (in->cluster < 512 || in->cluster > (1u << 20))
		in->cluster = 32768;
	snprintf(p, sizeof(p), "%s/%s", stick_root, in->folder);
	in->exists = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
	for (size_t i = 0; i < sizeof(g_dirs) / sizeof(g_dirs[0]) && r == 0; i++) {
		if (!in_mode(g_dirs[i].cat, mode))
			continue;
		if (tr_snprintf(p, sizeof(p), "%s/%s", data_root, g_dirs[i].dir) == 0)
			r = bk_walk(b, p, g_dirs[i].dir, g_dirs[i].cat, 0);
	}
	if (r < 0) {
		transfer_backup_free(b);
		return r;
	}
	for (int i = 0; i < b->n; i++) {
		same += b->items[i].state == BK_SAME;
		changed += b->items[i].state == BK_CHANGED;
		fresh += b->items[i].state == BK_NEW;
	}
	{
		char s[TRANSFER_BK_N][24], fr[24];

		for (int c = 0; c < TRANSFER_BK_N; c++)
			tr_fmt_bytes(in->bytes[c], s[c], sizeof(s[c]));
		tr_fmt_bytes(in->free_bytes, fr, sizeof(fr));
		if (mode == TRANSFER_EXPORT_GAMES)
			tr_log("export scan: console %s: roms %d files %s, bios %d %s", data_root,
			       in->files[TRANSFER_BK_ROMS], s[TRANSFER_BK_ROMS], in->files[TRANSFER_BK_BIOS],
			       s[TRANSFER_BK_BIOS]);
		else
			tr_log("saves scan: console %s: saves+states %d files %s, settings %d %s", data_root,
			       in->files[TRANSFER_BK_SAVES], s[TRANSFER_BK_SAVES], in->files[TRANSFER_BK_CONFIG],
			       s[TRANSFER_BK_CONFIG]);
		tr_log("  stick %s (%s, cluster %u, %s free), %s/%s: %d missing, %d different, "
		       "%d identical; %d too big for FAT32; %lld ms", stick_root, fstype ? fstype : "?",
		       in->cluster, fr, in->folder, in->exists ? "" : " (new)", fresh, changed, same, in->nbig,
		       (long long)(tr_now_ms() - t0));
	}
	*out = b;
	return 0;
}

const struct transfer_backup_info *transfer_backup_get_info(const struct transfer_backup *b)
{
	return &b->info;
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

/* rel starts with "<dir>/<system>/<stem>." (case-insensitive: exFAT) */
static bool game_file(const char *rel, const char *dir, const char *sys, const char *stem)
{
	char pre[400];

	snprintf(pre, sizeof(pre), "%s/%s/%s.", dir, sys, stem);
	return !strncasecmp(rel, pre, strlen(pre));
}

/* Is the item part of this copy? */
static bool selected(const struct bk_item *it, const struct transfer_backup_opts *o)
{
	char stem[256], cz[64];
	const char *rb, *dot, *r;
	size_t sl;

	if (!in_mode(it->cat, o->mode))
		return false;
	if (o->mode == TRANSFER_EXPORT_GAMES)
		return it->cat == TRANSFER_BK_ROMS || o->bios;
	if (it->cat == TRANSFER_BK_CONFIG && !o->settings)
		return false;
	if (!o->game_rom[0])
		return true;                          /* every game */
	/* one game: the host names its files after the ROM's stem */
	rb = base_name(o->game_rom);
	dot = strrchr(rb, '.');
	sl = dot && dot != rb ? (size_t)(dot - rb) : strlen(rb);
	if (sl == 0 || sl >= sizeof(stem))
		return false;
	memcpy(stem, rb, sl);
	stem[sl] = 0;
	if (it->cat == TRANSFER_BK_SAVES)
		return game_file(it->rel, "saves", o->game_system, stem) ||
		       game_file(it->rel, "states", o->game_system, stem);
	/* settings: rsos/coreopts/<core>/<stem>.ini, rsos/remaps/<system>[-cz]/<stem>.ini */
	snprintf(cz, sizeof(cz), "%s-cz", o->game_system);
	if (game_file(it->rel, "rsos/remaps", o->game_system, stem) ||
	    game_file(it->rel, "rsos/remaps", cz, stem))
		return true;
	if (strncmp(it->rel, "rsos/coreopts/", 14) || !(r = strchr(it->rel + 14, '/')))
		return false;
	return !strchr(r + 1, '/') && !strncasecmp(r + 1, stem, sl) && r[1 + sl] == '.';
}

void transfer_backup_totals(const struct transfer_backup *b, const struct transfer_backup_opts *o,
			    struct transfer_backup_totals *t)
{
	const struct transfer_backup_info *in = &b->info;

	memset(t, 0, sizeof(*t));
	for (int i = 0; i < b->n; i++) {
		const struct bk_item *it = &b->items[i];

		if (!selected(it, o))
			continue;
		if (it->big) {
			t->too_big++;
		} else if (it->state == BK_SAME) {
			t->unchanged++;
			t->unchanged_bytes += it->size;
		} else {
			t->files++;
			t->bytes += it->size;
			t->replace += it->state == BK_CHANGED;
			/* the new copy sits next to the old one until the rename
			 * (and an old save stays as .bak) */
			t->need += round_up(it->size, in->cluster) + in->cluster / 8;
		}
	}
	/* folders, markers, and a margin */
	t->need += 8ull * in->cluster + (1u << 20);
	t->fits = t->need <= in->free_bytes;
}

void transfer_backup_free(struct transfer_backup *b)
{
	if (!b)
		return;
	for (int i = 0; i < b->n; i++) {
		free(b->items[i].src);
		free(b->items[i].rel);
	}
	free(b->items);
	free(b);
}

/* ------------------------------------------------------------ run */

struct bk_run {
	struct transfer_progress *pr;
	transfer_progress_fn cb;
	void *user;
	int64_t t0, last_cb, rate_t;
	uint64_t rate_bytes, last_cb_bytes;
	/* the copy engine and the per-folder throughput log */
	struct tr_copier cp;
	struct tr_group grp;
};

static void bk_report(struct bk_run *r, bool force)
{
	int64_t now = tr_now_ms();
	struct transfer_progress *pr = r->pr;

	pr->elapsed_ms = now - r->t0;
	if (now - r->rate_t >= 500) {
		uint64_t d = pr->bytes_done - r->rate_bytes;
		uint32_t inst = (uint32_t)(d * 1000 / 1024 / (uint64_t)(now - r->rate_t));

		pr->rate_kbs = pr->rate_kbs ? (pr->rate_kbs * 3 + inst) / 4 : inst;
		r->rate_t = now;
		r->rate_bytes = pr->bytes_done;
		pr->eta_s = pr->rate_kbs ? (int)((pr->bytes_total - pr->bytes_done) / 1024 / pr->rate_kbs) : -1;
	}
	if (r->cb && (force || now - r->last_cb >= 100 || pr->bytes_done - r->last_cb_bytes >= (1u << 20))) {
		r->last_cb = now;
		r->last_cb_bytes = pr->bytes_done;
		r->cb(pr, r->user);
	}
}

static void bk_tick(void *arg)
{
	bk_report(arg, false);
}

static bool bk_fatal(int e)
{
	return e == -ENOSPC || e == -EIO || e == -ENODEV || e == -EROFS || e == -ENXIO ||
	       e == -EDQUOT || e == -ENOTCONN;
}

static int copy_one(int folderfd, const struct bk_item *it, struct bk_run *r, volatile int *cancel)
{
	char dir[TRANSFER_PATH_MAX], base[256];
	struct tr_sink sink;
	struct stat st;
	int sfd, dfd, e;

	sfd = open(it->src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (sfd < 0)
		return -errno;
	if (fstat(sfd, &st) < 0)
		st.st_size = 0;
	tr_split_path(it->rel, dir, sizeof(dir), base, sizeof(base));
	dfd = tr_open_dir_chain(folderfd, dir, true);
	if (dfd < 0) {
		close(sfd);
		return dfd;
	}
	e = tr_sink_open(&sink, dfd, base);
	if (e == 0) {
		/* on failure, back to before this file (review F-L8: subtracting
		 * sink.written could wrap bytes_done) */
		uint64_t before = r->pr->bytes_done;

		e = tr_copy_file(&r->cp, sfd, (uint64_t)st.st_size, &sink, &r->pr->bytes_done, cancel, bk_tick, r);
		if (e < 0) {
			r->pr->bytes_done = before;
			tr_sink_abort(&sink);            /* no partial file */
		} else {
			/* a save replaced on the stick keeps its previous version */
			e = tr_copier_commit(&r->cp, &sink, it->mtime_s, it->mtime_ns,
					     it->state == BK_CHANGED && precious(it->cat));
			if (e < 0)
				r->pr->bytes_done = before;
		}
	}
	close(dfd);
	close(sfd);
	return e;
}

/* An id for this console, made once: <data>/rsos/console-id. */
static void console_id(const char *data_root, char *out, size_t n)
{
	char p[TRANSFER_PATH_MAX + 32], id[32] = "";
	FILE *f;

	tr_snprintf(p, sizeof(p), "%s/rsos/console-id", data_root);
	f = fopen(p, "re");
	if (f) {
		if (fgets(id, sizeof(id), f))
			id[strcspn(id, "\r\n")] = 0;
		fclose(f);
	}
	if (!id[0]) {
		uint8_t rnd[4] = { 0 };

		tr_random(rnd, sizeof(rnd));
		snprintf(id, sizeof(id), "rs2-%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3]);
		tr_snprintf(p, sizeof(p), "%s/rsos", data_root);
		mkdir(p, 0755);
		tr_snprintf(p, sizeof(p), "%s/rsos/console-id", data_root);
		f = fopen(p, "we");
		if (f) {
			fprintf(f, "%s\n", id);
			fclose(f);
		}
	}
	tr_strlcpy(out, id, n);
}

static void write_text(int dirfd, const char *name, const char *text)
{
	struct tr_sink s;

	if (tr_sink_open(&s, dirfd, name) < 0)
		return;
	if (tr_sink_write(&s, text, strlen(text)) < 0 || tr_sink_commit(&s, -1, 0, false) < 0)
		tr_sink_abort(&s);
}

/* "key=value" lines of the old marker */
static long long marker_get(const char *text, const char *key)
{
	char k[40];
	const char *c;

	snprintf(k, sizeof(k), "\n%s=", key);
	c = strstr(text, k);
	return c ? atoll(c + strlen(k)) : 0;
}

static void fmt_when(long long t, char *out, size_t n)
{
	time_t tt = (time_t)t;
	struct tm tm;

	if (!t) {
		snprintf(out, n, "never");
		return;
	}
	localtime_r(&tt, &tm);
	strftime(out, n, "%Y-%m-%d %H:%M", &tm);
}

static void write_markers(int folderfd, const struct transfer_backup *b, const struct transfer_backup_opts *o,
			  const struct transfer_progress *pr, const char *status)
{
	char text[1800], old[1024] = "\n", ver[128] = "RetroStoneOS", sz[32], id[32], w1[32], w2[32];
	long long created, games, saves, now = (long long)time(NULL);
	FILE *f = fopen("/etc/rsos-version", "re");
	size_t n = 0;
	int fd;

	if (f) {
		if (fgets(ver, sizeof(ver), f))
			ver[strcspn(ver, "\r\n")] = 0;
		fclose(f);
	}
	/* keep the dates of the other kind of copy across updates */
	fd = openat(folderfd, TRANSFER_BACKUP_MARKER, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t rn = read(fd, old + 1, sizeof(old) - 2);

		close(fd);
		old[rn > 0 ? rn + 1 : 1] = 0;
	}
	created = marker_get(old, "created");
	games = marker_get(old, "games_updated");
	saves = marker_get(old, "saves_updated");
	if (!created)
		created = now;
	if (o->mode == TRANSFER_EXPORT_GAMES)
		games = now;
	else
		saves = now;
	console_id(b->info.data_root, id, sizeof(id));
	/* the machine marker: how this folder is recognised (hidden) */
	snprintf(text, sizeof(text),
		 "format=1\nconsole=%s\ncreated=%lld\nupdated=%lld\ngames_updated=%lld\nsaves_updated=%lld\n"
		 "last=%s\nstatus=%s\n",
		 id, created, now, games, saves, o->mode == TRANSFER_EXPORT_GAMES ? "export games" : "back up saves",
		 status);
	write_text(folderfd, TRANSFER_BACKUP_MARKER, text);
	/* the same for humans; CRLF for Windows Notepad */
	tr_fmt_bytes(pr->bytes_done, sz, sizeof(sz));
	fmt_when(games, w1, sizeof(w1));
	fmt_when(saves, w2, sizeof(w2));
	n += (size_t)snprintf(text + n, sizeof(text) - n,
			      "RetroStone2 backup (console %s, %s)\r\n"
			      "Games exported: %s\r\n"
			      "Saves backed up: %s\r\n"
			      "Last copy: %s, %d files copied (%s), %d already here, status: %s\r\n"
			      "\r\n"
			      "Files are copied here when they are missing or different; nothing here\r\n"
			      "is ever deleted. A replaced save keeps its previous version as <name>.bak.\r\n",
			      id, ver, w1, w2, o->mode == TRANSFER_EXPORT_GAMES ? "export games" : "back up saves",
			      pr->copied, sz, pr->identical, status);
	for (int i = 0; i < b->info.nbig && i < 8 && n < sizeof(text) - 200; i++)
		n += (size_t)snprintf(text + n, sizeof(text) - n, "Not copied (over 4 GB, FAT32 limit): %s\r\n",
				      b->info.big[i]);
	snprintf(text + n, sizeof(text) - n,
		 "\r\nTo restore: plug this drive into a RetroStone2 and choose \"Import games\".\r\n");
	write_text(folderfd, MARKER_TXT, text);
}

static int run_backup(struct transfer_backup *b, const struct transfer_backup_opts *o, struct bk_run *r,
		      volatile int *cancel)
{
	const char *what = o->mode == TRANSFER_EXPORT_GAMES ? "export" : "saves backup";
	struct transfer_progress *pr = r->pr;
	struct transfer_backup_totals t;
	char a[32];
	int stickfd, folderfd = -1, fatal = 0, cpe;

	memset(pr, 0, sizeof(*pr));
	pr->state = TRANSFER_RUNNING;
	pr->eta_s = -1;
	transfer_backup_totals(b, o, &t);
	pr->files_total = t.files;
	pr->bytes_total = t.bytes;
	pr->skipped = t.too_big;
	tr_strlcpy(pr->folder, b->info.folder, sizeof(pr->folder));
	r->t0 = r->rate_t = tr_now_ms();
	stickfd = open(b->info.stick_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (stickfd < 0) {
		fatal = -errno;
		goto done;
	}
	if (o->mode != b->info.mode) {
		fatal = -EINVAL;                 /* scanned for the other kind of copy */
		goto done;
	}
	folderfd = tr_open_dir_chain(stickfd, b->info.folder, true);
	cpe = tr_copier_init(&r->cp);
	memset(&r->grp, 0, sizeof(r->grp));
	if (folderfd < 0 || cpe < 0) {
		fatal = folderfd < 0 ? folderfd : cpe;
		if (folderfd < 0)
			folderfd = -1;
		goto done;
	}
	tr_fmt_bytes(t.bytes, a, sizeof(a));
	tr_log("%s started to %s/%s%s%s: %d files to copy (%s, %d different), %d already there%s",
	       what, b->info.stick_root, b->info.folder, o->game_rom[0] ? ", only " : "",
	       o->game_rom[0] ? base_name(o->game_rom) : "", t.files, a, t.replace, t.unchanged,
	       t.too_big ? ", files over 4 GB skipped (FAT32)" : "");
	bk_report(r, true);
	for (int i = 0; i < b->n; i++) {
		const struct bk_item *it = &b->items[i];
		int e;

		if (!selected(it, o) || it->big)
			continue;
		if (it->state == BK_SAME) {
			pr->identical++;
			continue;
		}
		if (cancel && *cancel) {
			fatal = -ECANCELED;
			break;
		}
		tr_strlcpy(pr->current, it->rel, sizeof(pr->current));
		{
			/* the next file to copy is read ahead while this one is flushed */
			int j = i + 1;

			while (j < b->n && (!selected(&b->items[j], o) || b->items[j].big ||
					    b->items[j].state == BK_SAME))
				j++;
			tr_copier_hint_next(&r->cp, j < b->n ? b->items[j].src : NULL, j < b->n ? b->items[j].size : 0);
		}
		tr_group_step(&r->grp, &r->cp, what, it->rel);
		e = copy_one(folderfd, it, r, cancel);
		if (e == 0) {
			pr->copied++;
			pr->replaced += it->state == BK_CHANGED;
			if (pr->ncopied_names < 6)
				tr_strlcpy(pr->copied_names[pr->ncopied_names++], it->rel,
					   sizeof(pr->copied_names[0]));
		} else if (e == -ECANCELED || bk_fatal(e)) {
			fatal = e;
			break;
		} else {
			pr->failed++;
			snprintf(pr->errmsg, sizeof(pr->errmsg), "%s: %s", it->rel, strerror(-e));
			tr_log("%s %s: %s", what, it->rel, strerror(-e));
		}
		pr->files_done++;
		bk_report(r, true);
	}
done:
	if (r->cp.buf) {
		tr_group_end(&r->grp, &r->cp);
		tr_copier_free(&r->cp);
	}
	pr->current[0] = 0;
	pr->err = fatal;
	if (fatal == -ECANCELED) {
		pr->state = TRANSFER_CANCELLED;
	} else if (fatal) {
		pr->state = TRANSFER_FAILED;
		if (fatal == -ENOSPC || fatal == -EDQUOT)
			snprintf(pr->errmsg, sizeof(pr->errmsg), "The USB drive is full");
		else if (fatal == -EROFS)
			snprintf(pr->errmsg, sizeof(pr->errmsg), "The USB drive is read-only");
		else if (fatal == -EIO || fatal == -ENODEV || fatal == -ENXIO || fatal == -ENOTCONN)
			snprintf(pr->errmsg, sizeof(pr->errmsg), "Write error: was the USB drive removed?");
		else
			snprintf(pr->errmsg, sizeof(pr->errmsg), "%s", strerror(-fatal));
	} else {
		pr->state = TRANSFER_DONE;
	}
	if (folderfd >= 0) {
		write_markers(folderfd, b, o, pr, pr->state == TRANSFER_DONE ? "complete" :
			      pr->state == TRANSFER_CANCELLED ? "INCOMPLETE (cancelled)" : "INCOMPLETE (stopped)");
		syncfs(folderfd);
		close(folderfd);
	}
	if (stickfd >= 0) {
		fsync(stickfd);
		close(stickfd);
	}
	pr->eta_s = 0;
	bk_report(r, true);
	{
		int64_t ms = pr->elapsed_ms > 0 ? pr->elapsed_ms : 1;

		tr_fmt_bytes(pr->bytes_done, a, sizeof(a));
		tr_log("%s %s: copied %d files (%s, %d replaced), already there %d, failed %d, too big %d; "
		       "%lld.%01lld s (%.1f MB/s)%s%s",
		       what, pr->state == TRANSFER_DONE ? "done" : pr->state == TRANSFER_CANCELLED ? "cancelled" :
		       "failed", pr->copied, a, pr->replaced, pr->identical, pr->failed, pr->skipped,
		       (long long)(ms / 1000), (long long)(ms % 1000 / 100),
		       (double)pr->bytes_done / 1048576.0 / ((double)ms / 1000.0),
		       pr->state == TRANSFER_FAILED ? ": " : "", pr->state == TRANSFER_FAILED ? pr->errmsg : "");
	}
	return fatal == -ECANCELED ? 0 : fatal;
}

int transfer_backup_run(struct transfer_backup *b, const struct transfer_backup_opts *o,
			transfer_progress_fn cb, void *user, volatile int *cancel,
			struct transfer_progress *result)
{
	struct transfer_progress pr;
	struct transfer_backup_opts def;
	struct bk_run r = { .pr = &pr, .cb = cb, .user = user };
	int e;

	if (!o) {
		transfer_backup_defaults(&def, b->info.mode);
		o = &def;
	}
	e = run_backup(b, o, &r, cancel);
	if (result)
		*result = pr;
	return e;
}

/* ------------------------------------------------------------ async */

static struct {
	pthread_mutex_t lock;
	pthread_t th;
	bool started;
	struct transfer_backup *b;
	struct transfer_backup_opts opts;
	struct transfer_progress shared, local;
	volatile int cancel;
	struct bk_run r;
} K = { .lock = PTHREAD_MUTEX_INITIALIZER };

bool tr_backup_busy(void)
{
	return K.started;
}

static void bk_async_cb(const struct transfer_progress *p, void *user)
{
	(void)user;
	pthread_mutex_lock(&K.lock);
	K.shared = *p;
	pthread_mutex_unlock(&K.lock);
}

static void *backup_thread(void *arg)
{
	(void)arg;
	run_backup(K.b, &K.opts, &K.r, &K.cancel);
	pthread_mutex_lock(&K.lock);
	K.shared = K.local;
	pthread_mutex_unlock(&K.lock);
	return NULL;
}

int transfer_backup_start(struct transfer_backup *b, const struct transfer_backup_opts *o)
{
	if (K.started || tr_import_busy())
		return -EBUSY;
	K.b = b;
	if (o) {
		K.opts = *o;
	} else {
		transfer_backup_defaults(&K.opts, b->info.mode);
	}
	K.cancel = 0;
	memset(&K.r, 0, sizeof(K.r));
	K.r.pr = &K.local;
	K.r.cb = bk_async_cb;
	pthread_mutex_lock(&K.lock);
	memset(&K.shared, 0, sizeof(K.shared));
	K.shared.state = TRANSFER_RUNNING;
	K.shared.eta_s = -1;
	pthread_mutex_unlock(&K.lock);
	if (pthread_create(&K.th, NULL, backup_thread, NULL) != 0) {
		K.b = NULL;
		return -EAGAIN;
	}
	K.started = true;
	return 0;
}

enum transfer_state transfer_backup_status(struct transfer_progress *out)
{
	enum transfer_state s;

	pthread_mutex_lock(&K.lock);
	if (out)
		*out = K.shared;
	s = K.started ? K.shared.state : TRANSFER_IDLE;
	pthread_mutex_unlock(&K.lock);
	return s;
}

void transfer_backup_cancel(void)
{
	K.cancel = 1;
}

void transfer_backup_finish(void)
{
	if (!K.started)
		return;
	pthread_join(K.th, NULL);
	K.started = false;
	transfer_backup_free(K.b);
	K.b = NULL;
	pthread_mutex_lock(&K.lock);
	K.shared.state = TRANSFER_IDLE;
	pthread_mutex_unlock(&K.lock);
}

/* The scan in a thread (cold folders on the SD card, a stat per file on the
 * stick). */
static struct {
	pthread_t th;
	bool started;
	volatile int done;
	char data[TRANSFER_PATH_MAX], stick[TRANSFER_PATH_MAX], fs[16];
	enum transfer_backup_mode mode;
	struct transfer_backup *b;
	int err;
} S;

static void *scan_thread(void *arg)
{
	(void)arg;
	S.err = transfer_backup_scan(S.data, S.stick, S.fs, S.mode, &S.b);
	__atomic_store_n(&S.done, 1, __ATOMIC_RELEASE);
	return NULL;
}

int transfer_backup_scan_start(const char *data_root, const char *stick_root, const char *fstype,
			       enum transfer_backup_mode mode)
{
	if (S.started)
		return -EBUSY;
	tr_strlcpy(S.data, data_root, sizeof(S.data));
	tr_strlcpy(S.stick, stick_root, sizeof(S.stick));
	tr_strlcpy(S.fs, fstype ? fstype : "", sizeof(S.fs));
	S.mode = mode;
	S.b = NULL;
	S.err = 0;
	S.done = 0;
	if (pthread_create(&S.th, NULL, scan_thread, NULL) != 0)
		return -EAGAIN;
	S.started = true;
	return 0;
}

int transfer_backup_scan_poll(struct transfer_backup **out)
{
	if (!S.started)
		return -EINVAL;
	if (!__atomic_load_n(&S.done, __ATOMIC_ACQUIRE))
		return 0;
	pthread_join(S.th, NULL);
	S.started = false;
	*out = S.b;
	return S.err < 0 ? S.err : 1;
}
