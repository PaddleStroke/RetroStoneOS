/*
 * fswarm.c - see fswarm.h.
 *
 * exFAT layout used here (exFAT specification, sections 3 and 6-7):
 *   boot sector: "EXFAT   " at 3, FatOffset (sectors) at 80, ClusterHeapOffset
 *   (sectors) at 88, ClusterCount at 92, FirstClusterOfRootDirectory at 96,
 *   BytesPerSectorShift at 108, SectorsPerClusterShift at 109.
 *   cluster N (N >= 2) starts at ClusterHeapOffset + (N - 2) * cluster size.
 *   FAT entry N: 32-bit next cluster, >= 0xFFFFFFF8 = end of chain.
 *   directory: 32-byte entries; 0x00 ends the directory; an in-use file is a
 *   set: 0x85 (secondary count at 1, attributes at 4, 0x10 = directory), then
 *   0xC0 stream extension (flags at 1, bit 1 = NoFatChain: contiguous; name
 *   length at 3; first cluster at 20; data length at 24), then 0xC1 name
 *   entries (15 UTF-16LE characters at 2).
 * The root directory has no stream entry and is always FAT-chained.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* offsets on the card go past 2 GiB (no off_t crosses this file's API) */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "fswarm.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "util.h"

#define MAX_VOLS      4            /* /data is the one that matters */
#define MAX_DIR_BYTES (4u << 20)   /* larger directories are read partly */
#define MAX_READ      (1u << 20)   /* one pread() at most */
#define MAX_RUNS      4096

struct vol {
	int fd;
	unsigned maj, min;
	char mnt[512];
	unsigned clu_shift;
	uint64_t fat_off, heap_off;
	uint32_t clu_count, root;
};

struct loc {
	uint32_t first;
	bool nofat;
	uint64_t size;               /* 0: unknown (root), follow the FAT */
};

struct run {
	uint32_t clu, n;
};

struct warm {
	struct vol *v;
	struct run *runs;
	int nruns;
	struct fswarm_stats st;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vol g_vols[MAX_VOLS];
static int g_nvols;

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* ------------------------------------------------------------- volumes */
/* "\040" escapes of /proc/self/mountinfo, in place. */
static void unescape(char *s)
{
	char *w = s;

	for (; *s; s++) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
		    s[3] >= '0' && s[3] <= '7') {
			*w++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
			s += 3;
		} else {
			*w++ = *s;
		}
	}
	*w = 0;
}

static bool prefix_of(const char *mnt, const char *path)
{
	size_t l = strlen(mnt);

	if (!strcmp(mnt, "/"))
		return path[0] == '/';
	return !strncmp(mnt, path, l) && (path[l] == '/' || path[l] == 0);
}

static bool vol_open(struct vol *v, const char *dev)
{
	uint8_t b[512];
	struct stat st;
	unsigned bps, spc;

	/* the node must really be the mounted device (never a stale name) */
	if (stat(dev, &st) < 0 || !S_ISBLK(st.st_mode) || major(st.st_rdev) != v->maj ||
	    minor(st.st_rdev) != v->min)
		return false;
	v->fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (v->fd < 0)
		return false;
	if (pread(v->fd, b, sizeof(b), 0) != (ssize_t)sizeof(b) || memcmp(b + 3, "EXFAT   ", 8))
		goto bad;
	bps = b[108];
	spc = b[109];
	if (bps < 9 || bps > 12 || bps + spc > 25)
		goto bad;
	v->clu_shift = bps + spc;
	v->fat_off = (uint64_t)le32(b + 80) << bps;
	v->heap_off = (uint64_t)le32(b + 88) << bps;
	v->clu_count = le32(b + 92);
	v->root = le32(b + 96);
	if (v->root < 2 || v->root >= v->clu_count + 2)
		goto bad;
	return true;
bad:
	close(v->fd);
	v->fd = -1;
	return false;
}

/* A /proc file (their size is 0: read to EOF). */
static char *proc_read(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t len = 0, cap = 8192;
	char *buf;

	if (fd < 0)
		return NULL;
	buf = xmalloc(cap);
	for (;;) {
		ssize_t r;

		if (len + 1 >= cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		r = read(fd, buf + len, cap - 1 - len);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += (size_t)r;
	}
	close(fd);
	buf[len] = 0;
	return buf;
}

/* The exFAT volume holding path, from /proc/self/mountinfo: never a stat()
 * of the path itself (that is the lookup this module is here to avoid). */
static struct vol *vol_for(const char *path, const char **rel)
{
	char *buf = proc_read("/proc/self/mountinfo"), *save = NULL, *line;
	char best_mnt[512] = "", best_dev[512] = "";
	unsigned best_maj = 0, best_min = 0;
	struct vol *v = NULL;

	if (!buf)
		return NULL;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[12], *s = line, *dash;
		int nf = 0;
		unsigned maj, min;

		dash = strstr(line, " - ");
		if (!dash)
			continue;
		*dash = 0;
		while (nf < 6 && (f[nf] = strsep(&s, " ")))
			nf++;
		if (nf < 5 || sscanf(f[2], "%u:%u", &maj, &min) != 2 || strcmp(f[3], "/"))
			continue;
		s = dash + 3;
		f[6] = strsep(&s, " ");          /* fstype */
		f[7] = s ? strsep(&s, " ") : NULL; /* source */
		if (!f[6] || !f[7] || strcmp(f[6], "exfat"))
			continue;
		unescape(f[4]);
		unescape(f[7]);
		if (!prefix_of(f[4], path) || strlen(f[4]) < strlen(best_mnt) || strlen(f[4]) >= sizeof(best_mnt))
			continue;
		strlcpy_(best_mnt, f[4], sizeof(best_mnt));
		strlcpy_(best_dev, f[7], sizeof(best_dev));
		best_maj = maj;
		best_min = min;
	}
	free(buf);
	if (!best_mnt[0])
		return NULL;
	for (int i = 0; i < g_nvols; i++)
		if (g_vols[i].maj == best_maj && g_vols[i].min == best_min && !strcmp(g_vols[i].mnt, best_mnt))
			v = &g_vols[i];
	if (!v) {
		struct vol nv;

		memset(&nv, 0, sizeof(nv));
		nv.fd = -1;
		nv.maj = best_maj;
		nv.min = best_min;
		strlcpy_(nv.mnt, best_mnt, sizeof(nv.mnt));
		if (!vol_open(&nv, best_dev))
			return NULL;
		if (g_nvols == MAX_VOLS) {
			close(g_vols[0].fd);
			memmove(&g_vols[0], &g_vols[1], sizeof(g_vols[0]) * (MAX_VOLS - 1));
			g_nvols--;
		}
		g_vols[g_nvols] = nv;
		v = &g_vols[g_nvols++];
	}
	*rel = path + (strcmp(v->mnt, "/") ? strlen(v->mnt) : 0);
	return v;
}

/* --------------------------------------------------------------- reads */
static bool clu_ok(const struct vol *v, uint32_t c)
{
	return c >= 2 && c < v->clu_count + 2;
}

static uint64_t clu_off(const struct vol *v, uint32_t c)
{
	return v->heap_off + ((uint64_t)(c - 2) << v->clu_shift);
}

static bool fat_next(struct warm *w, uint32_t c, uint32_t *next)
{
	uint8_t b[4];

	if (pread(w->v->fd, b, 4, (off_t)(w->v->fat_off + 4ull * c)) != 4)
		return false;
	*next = le32(b);
	return true;
}

/* Reads [clu, clu + n) into buf (may be NULL: warm only). */
static bool read_clusters(struct warm *w, uint32_t clu, uint32_t n, uint8_t *buf)
{
	uint64_t off = clu_off(w->v, clu), len = (uint64_t)n << w->v->clu_shift, done = 0;
	static uint8_t scratch[MAX_READ];   /* under g_lock */

	while (done < len) {
		size_t chunk = (size_t)MIN((uint64_t)MAX_READ, len - done);
		ssize_t r = pread(w->v->fd, buf ? buf + done : scratch, chunk, (off_t)(off + done));

		w->st.reads++;
		if (r <= 0)
			return false;
		w->st.bytes += r;
		done += (uint64_t)r;
	}
	return true;
}

/* The clusters of a directory as runs (at most MAX_DIR_BYTES). */
static int dir_runs(struct warm *w, const struct loc *l, struct run *out, int max)
{
	uint32_t maxclu = MAX_DIR_BYTES >> w->v->clu_shift, c = l->first, total = 0;
	int n = 0;

	if (!clu_ok(w->v, c))
		return 0;
	if (l->nofat) {
		uint64_t nclu = (l->size + (1ull << w->v->clu_shift) - 1) >> w->v->clu_shift;

		if (!nclu)
			return 0;
		out[0].clu = c;
		out[0].n = (uint32_t)MIN(nclu, (uint64_t)maxclu);
		if (out[0].clu + out[0].n > w->v->clu_count + 2)
			out[0].n = w->v->clu_count + 2 - out[0].clu;
		return 1;
	}
	while (clu_ok(w->v, c) && total < maxclu) {
		uint32_t next;

		if (n && out[n - 1].clu + out[n - 1].n == c) {
			out[n - 1].n++;
		} else {
			if (n == max)
				break;
			out[n].clu = c;
			out[n++].n = 1;
		}
		total++;
		if (!fat_next(w, c, &next))
			break;
		c = next;
	}
	return n;
}

/* Reads a whole directory into memory (caller frees). */
static uint8_t *dir_read(struct warm *w, const struct loc *l, size_t *len)
{
	struct run r[64];
	int n = dir_runs(w, l, r, 64);
	size_t total = 0, off = 0;
	uint8_t *buf;

	for (int i = 0; i < n; i++)
		total += (size_t)r[i].n << w->v->clu_shift;
	if (!total)
		return NULL;
	buf = xmalloc(total);
	for (int i = 0; i < n; i++) {
		if (!read_clusters(w, r[i].clu, r[i].n, buf + off)) {
			free(buf);
			return NULL;
		}
		off += (size_t)r[i].n << w->v->clu_shift;
	}
	w->st.dirs++;
	*len = total;
	return buf;
}

/* ------------------------------------------------------------- entries */
typedef bool (*entry_fn)(const char *name, bool dir, const struct loc *l, void *user);

/* Calls fn for each in-use file entry set; stops when fn returns true. */
static bool dir_each(const uint8_t *d, size_t len, entry_fn fn, void *user)
{
	for (size_t off = 0; off + 32 <= len; off += 32) {
		const uint8_t *e = d + off, *s;
		unsigned sec, nlen, k = 0;
		char name[1024];
		size_t o = 0;
		struct loc l;

		if (e[0] == 0x00)
			break;                  /* end of the directory */
		if (e[0] != 0x85)
			continue;
		sec = e[1];
		if (sec < 2 || off + 32 * (size_t)(1 + sec) > len)
			continue;
		s = e + 32;
		if (s[0] != 0xC0)
			continue;
		nlen = s[3];
		l.first = le32(s + 20);
		l.size = le64(s + 24);
		l.nofat = (s[1] & 2) != 0;
		/* UTF-16LE name -> UTF-8 (surrogate pairs as they come; our own
		 * paths are ASCII) */
		for (unsigned i = 2; i <= sec && k < nlen; i++) {
			const uint8_t *ne = e + 32 * i;

			if (ne[0] != 0xC1)
				break;
			for (unsigned c = 0; c < 15 && k < nlen; c++, k++) {
				unsigned u = ne[2 + 2 * c] | (unsigned)ne[3 + 2 * c] << 8;

				if (o + 4 >= sizeof(name))
					break;
				if (u < 0x80) {
					name[o++] = (char)u;
				} else if (u < 0x800) {
					name[o++] = (char)(0xC0 | u >> 6);
					name[o++] = (char)(0x80 | (u & 0x3F));
				} else {
					name[o++] = (char)(0xE0 | u >> 12);
					name[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
					name[o++] = (char)(0x80 | (u & 0x3F));
				}
			}
		}
		name[o] = 0;
		if (fn(name, (e[4] & 0x10) != 0, &l, user))
			return true;
		off += 32 * (size_t)sec;
	}
	return false;
}

struct find_ctx {
	const char *want;
	size_t wlen;
	struct loc found;
	bool ok;
};

static bool find_fn(const char *name, bool dir, const struct loc *l, void *user)
{
	struct find_ctx *c = user;

	if (!dir || strlen(name) != c->wlen || strncasecmp(name, c->want, c->wlen))
		return false;
	c->found = *l;
	c->ok = true;
	return true;
}

static void add_runs(struct warm *w, const struct loc *l)
{
	struct run r[64];
	int n = dir_runs(w, l, r, 64);

	for (int i = 0; i < n && w->nruns < MAX_RUNS; i++)
		w->runs[w->nruns++] = r[i];
	w->st.dirs++;
}

static bool child_fn(const char *name, bool dir, const struct loc *l, void *user)
{
	(void)name;
	if (dir)
		add_runs(user, l);
	return false;
}

/* Walks rel ("/rsos/cache") from the root: every directory on the way is
 * read (so the driver's lookups are cached); the last one is queued (and
 * its children with FSW_CHILDREN). */
static void walk(struct warm *w, const char *rel, unsigned flags)
{
	struct loc cur = { w->v->root, false, 0 };
	const char *p = rel;

	for (;;) {
		const char *comp, *end;
		struct find_ctx fc;
		uint8_t *d;
		size_t len;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		comp = p;
		end = strchr(p, '/');
		if (!end)
			end = p + strlen(p);
		p = end;
		d = dir_read(w, &cur, &len);
		if (!d)
			return;
		memset(&fc, 0, sizeof(fc));
		fc.want = comp;
		fc.wlen = (size_t)(end - comp);
		dir_each(d, len, find_fn, &fc);
		free(d);
		if (!fc.ok)
			return;
		cur = fc.found;
	}
	if (flags & FSW_CHILDREN) {
		uint8_t *d;
		size_t len;

		d = dir_read(w, &cur, &len);
		if (d) {
			dir_each(d, len, child_fn, w);
			free(d);
		}
	} else {
		add_runs(w, &cur);
	}
}

static int cmp_run(const void *a, const void *b)
{
	const struct run *x = a, *y = b;

	return x->clu < y->clu ? -1 : x->clu > y->clu;
}

/* Reads the queued runs in cluster order, merging neighbours (a gap of one
 * cluster is cheaper to read than a second request). */
static void flush_runs(struct warm *w)
{
	int i = 0;

	qsort(w->runs, (size_t)w->nruns, sizeof(w->runs[0]), cmp_run);
	while (i < w->nruns) {
		uint32_t start = w->runs[i].clu, end = start + w->runs[i].n;
		int j = i + 1;

		while (j < w->nruns && w->runs[j].clu <= end + 1) {
			end = MAX(end, w->runs[j].clu + w->runs[j].n);
			j++;
		}
		if (!read_clusters(w, start, end - start, NULL))
			break;
		i = j;
	}
	w->nruns = 0;
}

int fswarm(const char *const *paths, int n, unsigned flags, struct fswarm_stats *st)
{
	struct warm w;
	int64_t t0 = ui_now_us();
	int dirs;

	memset(&w, 0, sizeof(w));
	pthread_mutex_lock(&g_lock);
	w.runs = xmalloc(sizeof(*w.runs) * MAX_RUNS);
	for (int i = 0; i < n; i++) {
		const char *rel;
		struct vol *v;

		if (!paths[i] || paths[i][0] != '/')
			continue;
		v = vol_for(paths[i], &rel);
		if (!v)
			continue;
		if (w.v && v != w.v)
			flush_runs(&w);   /* runs are per volume */
		w.v = v;
		walk(&w, rel, flags);
	}
	if (w.v)
		flush_runs(&w);   /* every queued folder, merged in cluster order */
	free(w.runs);
	pthread_mutex_unlock(&g_lock);
	w.st.us = ui_now_us() - t0;
	dirs = w.st.dirs;
	if (st) {
		st->dirs += w.st.dirs;
		st->reads += w.st.reads;
		st->bytes += w.st.bytes;
		st->us += w.st.us;
	}
	return dirs;
}

void fswarm_close(void)
{
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < g_nvols; i++)
		if (g_vols[i].fd >= 0)
			close(g_vols[i].fd);
	g_nvols = 0;
	pthread_mutex_unlock(&g_lock);
}
