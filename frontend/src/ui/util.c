/*
 * util.c - see util.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ log */
static ui_log_fn g_log_fn;
static void *g_log_user;
static enum ui_log_level g_log_max = UI_LOG_INFO;

void ui_log_set(ui_log_fn fn, void *user, enum ui_log_level max_level)
{
	g_log_fn = fn;
	g_log_user = user;
	g_log_max = max_level;
}

static void log_emit(enum ui_log_level lvl, const char *msg)
{
	static const char *const names[] = { "E", "W", "I", "D" };

	if (lvl > g_log_max)
		return;
	if (g_log_fn) {
		g_log_fn(lvl, msg, g_log_user);
		return;
	}
	fprintf(stderr, "[%8.3f] ui %s: %s\n", (double)ui_now_ms() / 1000.0,
		names[lvl], msg);
}

void ui_log(enum ui_log_level lvl, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	if (lvl > g_log_max)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_emit(lvl, buf);
}

/* A small open-addressing set of 64-bit key hashes. */
#define ONCE_SLOTS 1024
static uint64_t g_once[ONCE_SLOTS];

void ui_log_once(const char *key, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	uint64_t h = hash64_str(key, 0x5eed) | 1;
	unsigned i = (unsigned)h & (ONCE_SLOTS - 1);
	unsigned n;

	for (n = 0; n < ONCE_SLOTS; n++, i = (i + 1) & (ONCE_SLOTS - 1)) {
		if (g_once[i] == h)
			return;
		if (!g_once[i]) {
			g_once[i] = h;
			break;
		}
	}
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_emit(UI_LOG_INFO, buf);
}

/* ----------------------------------------------------------------- time */
int64_t ui_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t ui_now_ms(void)
{
	return ui_now_us() / 1000;
}

/* ---------------------------------------------------------------- arena */
struct arena_chunk {
	struct arena_chunk *next;
	size_t used, size;
	/* data follows, 8-aligned */
	uint64_t data[];
};

void arena_init(struct arena *a, size_t chunk_size)
{
	a->head = NULL;
	a->chunk_size = chunk_size ? chunk_size : 16384;
}

void *arena_alloc(struct arena *a, size_t n)
{
	struct arena_chunk *c = a->head;
	void *p;

	n = (n + 7) & ~(size_t)7;
	if (!c || c->used + n > c->size) {
		size_t sz = a->chunk_size;

		if (n > sz / 4) {
			/* Big block: its own chunk, inserted behind the head
			 * so the head keeps serving small allocations. */
			c = xmalloc(sizeof(*c) + n);
			c->size = n;
			c->used = n;
			if (a->head) {
				c->next = a->head->next;
				a->head->next = c;
			} else {
				c->next = NULL;
				a->head = c;
			}
			memset(c->data, 0, n);
			return c->data;
		}
		c = xmalloc(sizeof(*c) + sz);
		c->size = sz;
		c->used = 0;
		c->next = a->head;
		a->head = c;
	}
	p = (char *)c->data + c->used;
	c->used += n;
	memset(p, 0, n);
	return p;
}

char *arena_strndup(struct arena *a, const char *s, size_t n)
{
	char *d = arena_alloc(a, n + 1);

	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

char *arena_strdup(struct arena *a, const char *s)
{
	return s ? arena_strndup(a, s, strlen(s)) : NULL;
}

char *arena_printf(struct arena *a, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return arena_strdup(a, buf);
}

void arena_free(struct arena *a)
{
	struct arena_chunk *c = a->head, *n;

	while (c) {
		n = c->next;
		free(c);
		c = n;
	}
	a->head = NULL;
}

/* -------------------------------------------------------------- strings */
void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		fprintf(stderr, "ui: out of memory (%zu bytes)\n", n);
		abort();
	}
	return p;
}

void *xcalloc(size_t n, size_t m)
{
	void *p = calloc(n ? n : 1, m ? m : 1);

	if (!p) {
		fprintf(stderr, "ui: out of memory\n");
		abort();
	}
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) {
		fprintf(stderr, "ui: out of memory\n");
		abort();
	}
	return p;
}

char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = xmalloc(n);

	memcpy(d, s, n);
	return d;
}

size_t strlcpy_(char *dst, const char *src, size_t n)
{
	size_t l = strlen(src);

	if (n) {
		size_t c = l < n - 1 ? l : n - 1;

		/* cut: never in the middle of a UTF-8 character (translated
		 * texts, names) */
		if (c < l)
			for (int k = 0; k < 3 && c > 0 && ((unsigned char)src[c] & 0xc0) == 0x80; k++)
				c--;
		memcpy(dst, src, c);
		dst[c] = 0;
	}
	return l;
}

bool str_ieq(const char *a, const char *b)
{
	return strcasecmp(a, b) == 0;
}

bool str_starts(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends_i(const char *s, const char *suffix)
{
	size_t ls = strlen(s), lx = strlen(suffix);

	return ls >= lx && strcasecmp(s + ls - lx, suffix) == 0;
}

char *str_trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		e--;
	*e = 0;
	return s;
}

int str_casecmp_natural(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			unsigned long x = strtoul(a, (char **)&a, 10);
			unsigned long y = strtoul(b, (char **)&b, 10);

			if (x != y)
				return x < y ? -1 : 1;
			continue;
		}
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);

		if (ca != cb)
			return ca - cb;
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

bool parse_bool(const char *s, bool def)
{
	if (!s || !*s)
		return def;
	if (!strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "yes") ||
	    !strcasecmp(s, "on"))
		return true;
	if (!strcasecmp(s, "false") || !strcmp(s, "0") || !strcasecmp(s, "no") ||
	    !strcasecmp(s, "off"))
		return false;
	return def;
}

int str_split_list(const char *s, char out[][64], int max)
{
	int n = 0;

	while (*s && n < max) {
		size_t l;

		while (*s && (isspace((unsigned char)*s) || *s == ','))
			s++;
		if (!*s)
			break;
		l = strcspn(s, " \t\r\n,");
		if (l > 63)
			l = 63;
		memcpy(out[n], s, l);
		out[n][l] = 0;
		n++;
		s += strcspn(s, " \t\r\n,");
	}
	return n;
}

/* ------------------------------------------------------------------ io */
char *file_read_err(const char *path, size_t *len, int *err)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	char *buf;
	size_t got = 0;
	int e = 0;

	*err = 0;
	if (fd < 0) {
		*err = -errno;
		return NULL;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		*err = S_ISDIR(st.st_mode) ? -EISDIR : -EINVAL;
		close(fd);
		return NULL;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		close(fd);
		*err = -ENOMEM;
		return NULL;
	}
	while (got < (size_t)st.st_size) {
		ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			e = -errno;
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	/* A read error is an error, never a shorter file (review F-M7: the next
	 * save of settings/gamedb rewrote the file with what was read). An
	 * early end of file is not: sysfs attributes say 4096 bytes and hold a
	 * few. */
	if (e) {
		free(buf);
		*err = e;
		return NULL;
	}
	buf[got] = 0;
	if (len)
		*len = got;
	return buf;
}

char *file_read(const char *path, size_t *len)
{
	int e;

	return file_read_err(path, len, &e);
}

char *file_read_user(const char *path, size_t *len, int *err, bool *from_bak)
{
	char bak[4096];
	char *buf = file_read_err(path, len, err);
	int e2;

	*from_bak = false;
	if (buf || *err == -ENOENT)
		return buf;           /* no file: the defaults, not an old backup */
	/* unreadable: the previous version, read-only (the caller must not
	 * overwrite the file it could not read) */
	if (snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak))
		return NULL;
	buf = file_read_err(bak, len, &e2);
	if (buf) {
		*from_bak = true;
		LOGW("%s: %s: using %s", path, strerror(-*err), bak);
	} else if (*err != -ENOENT) {
		LOGW("%s: cannot read it: %s (no usable .bak)", path, strerror(-*err));
	}
	return buf;
}

bool file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int write_atomic(const char *path, const void *data, size_t len, bool backup);

/* Moves tmp over path, the current path kept as path.bak. These files are
 * small: the .bak is a durable copy, then one atomic rename, so path
 * exists at every moment, also on FAT/exFAT (no hard links, and two
 * renames would leave a window without the file). 0 or -errno; on failure
 * path is untouched (review F-M8). */
static int keep_backup(const char *path, const char *tmp)
{
	char bak[4096], *old;
	size_t n;
	int e;

	if (snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak))
		return -ENAMETOOLONG;
	old = file_read_err(path, &n, &e);
	if (!old && e != -ENOENT)
		return e;             /* cannot back it up: do not replace it */
	if (old) {
		e = write_atomic(bak, old, n, false);
		free(old);
		if (e < 0)
			return e;
	}
	return rename(tmp, path) < 0 ? -errno : 0;
}

static int write_atomic(const char *path, const void *data, size_t len, bool backup)
{
	char tmp[4096];
	int fd;
	size_t done = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp%d", path, (int)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	while (done < len) {
		ssize_t w = write(fd, (const char *)data + done, len - done);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			int e = errno ? errno : EIO;

			close(fd);
			unlink(tmp);
			return -e;
		}
		done += (size_t)w;
	}
	/* /data is exFAT (no journal) and the battery can die at any time:
	 * data on disk first, then the rename, then the directory entry. The
	 * fd is closed on every path (review F-L13: an fsync error leaked it). */
	{
		int e = fsync(fd) < 0 ? errno : 0;

		if (close(fd) < 0 && !e)
			e = errno;
		if (!e && backup)
			e = -keep_backup(path, tmp);
		else if (!e && rename(tmp, path) < 0)
			e = errno;
		if (e) {
			unlink(tmp);
			return -e;
		}
	}
	{
		char dir[4096];
		int dfd;

		path_dirname(path, dir, sizeof(dir));
		dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dfd >= 0) {
			fsync(dfd);
			close(dfd);
		}
	}
	return 0;
}

int file_write_atomic(const char *path, const void *data, size_t len)
{
	return write_atomic(path, data, len, false);
}

int file_write_atomic_bak(const char *path, const void *data, size_t len)
{
	return write_atomic(path, data, len, true);
}

int file_write_atomic_nosync(const char *path, const void *data, size_t len)
{
	char tmp[4096];
	int fd;
	size_t done = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp%d", path, (int)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	while (done < len) {
		ssize_t w = write(fd, (const char *)data + done, len - done);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			int e = errno ? errno : EIO;

			close(fd);
			unlink(tmp);
			return -e;
		}
		done += (size_t)w;
	}
	if (close(fd) < 0 || rename(tmp, path) < 0) {
		int e = errno;

		unlink(tmp);
		return -e;
	}
	return 0;
}

int mkdir_p(const char *path)
{
	char buf[4096];
	char *p;

	strlcpy_(buf, path, sizeof(buf));
	for (p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0755) < 0 && errno != EEXIST)
				return -errno;
			*p = '/';
		}
	}
	if (mkdir(buf, 0755) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

/* --------------------------------------------------------------- paths */
void path_dirname(const char *path, char *out, size_t n)
{
	const char *s = strrchr(path, '/');

	if (!s) {
		strlcpy_(out, ".", n);
		return;
	}
	if (s == path) {
		strlcpy_(out, "/", n);
		return;
	}
	if ((size_t)(s - path) + 1 < n) {
		memcpy(out, path, (size_t)(s - path));
		out[s - path] = 0;
	} else {
		strlcpy_(out, path, n);
	}
}

const char *path_basename(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? s + 1 : path;
}

void path_normalize(char *p)
{
	/* Collapse "//", "/./" and "x/../". Works on a copy: segments are
	 * written back into p, never past the end of the input. */
	char in[4096];
	char *src = in, *dst = p;
	bool abs = *p == '/';
	size_t seg_start[256];
	int nseg = 0;

	strlcpy_(in, p, sizeof(in));
	if (abs)
		*dst++ = '/';
	while (*src) {
		char *s;
		size_t l;

		while (*src == '/')
			src++;
		if (!*src)
			break;
		s = src;
		while (*src && *src != '/')
			src++;
		l = (size_t)(src - s);
		if (l == 1 && s[0] == '.')
			continue;
		if (l == 2 && s[0] == '.' && s[1] == '.') {
			if (nseg > 0 && strncmp(p + seg_start[nseg - 1], "../", 3)) {
				dst = p + seg_start[--nseg];
				continue;
			}
			if (abs)
				continue;
		}
		if (nseg < 256)
			seg_start[nseg++] = (size_t)(dst - p);
		memcpy(dst, s, l);
		dst += l;
		*dst++ = '/';
	}
	if (dst > p + 1 && dst[-1] == '/')
		dst--;
	if (dst == p)
		*dst++ = abs ? '/' : '.';
	*dst = 0;
}

void path_join(const char *dir, const char *rel, char *out, size_t n)
{
	if (rel[0] == '/')
		snprintf(out, n, "%s", rel);
	else
		snprintf(out, n, "%s/%s", dir, rel);
	path_normalize(out);
}

/* -------------------------------------------------------------- hashing */
/* FNV-1a 64 with a final avalanche (good enough for cache keys). */
uint64_t hash64(const void *data, size_t len, uint64_t seed)
{
	const unsigned char *p = data;
	uint64_t h = 0xcbf29ce484222325ull ^ seed;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return h;
}

uint64_t hash64_str(const char *s, uint64_t seed)
{
	return hash64(s, strlen(s), seed);
}
