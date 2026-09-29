/*
 * hutil.c - see hutil.h.
 */
#include "hutil.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static enum hlog_level log_max = HLOG_INFO;

void hlog_set_level(enum hlog_level max)
{
	log_max = max;
}

bool hlog_enabled(enum hlog_level lvl)
{
	return lvl <= log_max;
}

bool hrate_take(struct hrate *r, int64_t now_ms, double rate, double burst, unsigned long *dropped)
{
	if (!r->started) {
		r->started = true;
		r->tokens = burst;
		r->last_ms = now_ms;
	}
	if (now_ms > r->last_ms) {
		r->tokens += (double)(now_ms - r->last_ms) * rate / 1000.0;
		if (r->tokens > burst)
			r->tokens = burst;
		r->last_ms = now_ms;
	}
	if (r->tokens < 1.0) {
		r->dropped++;
		return false;
	}
	r->tokens -= 1.0;
	*dropped = r->dropped;
	r->dropped = 0;
	return true;
}

void hlogv(enum hlog_level lvl, const char *fmt, va_list ap)
{
	static const char *tag[] = { "E", "W", "I", "D" };
	char buf[1024];
	int64_t t;
	size_t n;

	if (lvl > log_max)
		return;
	t = hnow_ms();
	vsnprintf(buf, sizeof(buf), fmt, ap);
	n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	fprintf(stderr, "[%6lld.%03lld] host %s: %s\n", (long long)(t / 1000),
		(long long)(t % 1000), tag[lvl], buf);
}

void hlog(enum hlog_level lvl, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	hlogv(lvl, fmt, ap);
	va_end(ap);
}

void hlog_once(const char *key, const char *fmt, ...)
{
	static uint64_t seen[256];
	static int nseen;
	uint64_t h = hhash64(key, strlen(key));
	va_list ap;

	for (int i = 0; i < nseen; i++)
		if (seen[i] == h)
			return;
	if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
		seen[nseen++] = h;
	va_start(ap, fmt);
	hlogv(HLOG_INFO, fmt, ap);
	va_end(ap);
}

int64_t hnow_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t hnow_ms(void)
{
	return hnow_us() / 1000;
}

size_t hstrlcpy(char *dst, const char *src, size_t n)
{
	size_t len = strlen(src);

	if (n) {
		size_t c = len < n - 1 ? len : n - 1;

		/* truncated: never cut a UTF-8 sequence (translated text) */
		if (c < len)
			while (c > 0 && ((unsigned char)src[c] & 0xc0) == 0x80)
				c--;
		memcpy(dst, src, c);
		dst[c] = 0;
	}
	return len;
}

void hutf8_trim(char *s)
{
	size_t n = strlen(s), i = n, need;
	unsigned char c;
	int k = 0;

	while (i > 0 && k < 4 && ((unsigned char)s[i - 1] & 0xc0) == 0x80) {
		i--;
		k++;
	}
	if (i == 0)
		return;
	c = (unsigned char)s[i - 1];
	if (c < 0xc0)
		return;
	need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
	if (n - (i - 1) < need)
		s[i - 1] = 0;
}

bool hpath(char *dst, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(dst, n, fmt, ap);
	va_end(ap);
	return r >= 0 && (size_t)r < n;
}

const char *hpath_base(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? s + 1 : path;
}

void hpath_stem(const char *path, char *out, size_t n)
{
	const char *b = hpath_base(path);
	const char *dot = strrchr(b, '.');
	size_t len = dot && dot != b ? (size_t)(dot - b) : strlen(b);

	if (!n)
		return;
	if (len > n - 1)
		len = n - 1;
	memcpy(out, b, len);
	out[len] = 0;
}

void hpath_ext(const char *path, char *out, size_t n)
{
	const char *b = hpath_base(path);
	const char *dot = strrchr(b, '.');
	size_t i = 0;

	if (!n)
		return;
	if (dot && dot != b)
		for (dot++; *dot && i < n - 1; dot++)
			out[i++] = (char)tolower((unsigned char)*dot);
	out[i] = 0;
}

void hpath_dir(const char *path, char *out, size_t n)
{
	const char *s = strrchr(path, '/');

	if (!s) {
		hstrlcpy(out, ".", n);
		return;
	}
	if (s == path) {
		hstrlcpy(out, "/", n);
		return;
	}
	if ((size_t)(s - path) + 1 > n) {
		if (n)
			out[0] = 0;
		return;
	}
	memcpy(out, path, (size_t)(s - path));
	out[s - path] = 0;
}

bool hlist_has(const char *list, const char *item)
{
	size_t il = strlen(item);
	const char *p = list;

	if (!list || !il)
		return false;
	while (*p) {
		const char *e;
		size_t l;

		while (*p == ' ' || *p == '|' || *p == ',' || *p == '\t')
			p++;
		e = p;
		while (*e && *e != '|' && *e != ',')
			e++;
		l = (size_t)(e - p);
		while (l && (p[l - 1] == ' ' || p[l - 1] == '\t'))
			l--;
		if (l == il && strncasecmp(p, item, il) == 0)
			return true;
		p = e;
	}
	return false;
}

int hmkdir_p(const char *path, unsigned mode)
{
	char buf[4096];
	size_t len = hstrlcpy(buf, path, sizeof(buf));

	if (len >= sizeof(buf))
		return -ENAMETOOLONG;
	if (!len)
		return -ENOENT;       /* review F-L18: buf + 1 was past the NUL */
	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(buf, mode) < 0 && errno != EEXIST)
			return -errno;
		*p = '/';
	}
	if (mkdir(buf, mode) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

bool hfile_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

long long hfile_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return -errno;
	return (long long)st.st_size;
}

void *hread_file(const char *path, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	char *buf;
	size_t got = 0;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) < 0 || st.st_size < 0) {
		close(fd);
		return NULL;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		close(fd);
		return NULL;
	}
	while (got < (size_t)st.st_size) {
		ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	if (got != (size_t)st.st_size) {
		free(buf);
		return NULL;
	}
	buf[got] = 0;
	if (size)
		*size = got;
	return buf;
}

long hread_file_into(const char *path, void *buf, size_t max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t got = 0;

	if (fd < 0)
		return -errno;
	while (got < max) {
		ssize_t r = read(fd, (char *)buf + got, max - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			int e = errno;

			close(fd);
			return -e;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	return (long)got;
}

static int write_all(int fd, const void *data, size_t size)
{
	const char *p = data;

	while (size) {
		ssize_t w = write(fd, p, size);

		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0)
			return -errno;
		p += w;
		size -= (size_t)w;
	}
	return 0;
}

static void fsync_dir_of(const char *path)
{
	char dir[4096];
	int fd;

	hpath_dir(path, dir, sizeof(dir));
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

int hwrite_atomic_v(const char *path, const struct wpiece *p, int n, bool backup)
{
	char tmp[4096], bak[4096];
	int fd, ret = 0;

	if (!hpath(tmp, sizeof(tmp), "%s.tmp", path) || !hpath(bak, sizeof(bak), "%s.bak", path))
		return -ENAMETOOLONG;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	for (int i = 0; i < n && !ret; i++)
		if (p[i].size)
			ret = write_all(fd, p[i].data, p[i].size);
	if (!ret && fsync(fd) < 0)
		ret = -errno;
	if (close(fd) < 0 && !ret)
		ret = -errno;
	if (ret) {
		unlink(tmp);
		return ret;
	}
	if (backup && hfile_exists(path)) {
		/* The backup is made or nothing is replaced (review F-M8: the
		 * rename to .bak was not checked, nothing was undone). */
		if (unlink(bak) < 0 && errno != ENOENT) {
			ret = -errno;
		} else if (link(path, bak) == 0) {
			/* hard links: path exists at every moment */
		} else if (errno != EPERM && errno != EOPNOTSUPP && errno != ENOSYS && errno != EMLINK) {
			ret = -errno;
		} else if (rename(path, bak) < 0) {   /* FAT/exFAT: two renames */
			ret = -errno;
		} else if (rename(tmp, path) < 0) {
			ret = -errno;
			rename(bak, path);                /* put the old one back */
		} else {
			fsync_dir_of(path);
			return 0;
		}
		if (ret) {
			unlink(tmp);
			return ret;
		}
	}
	if (rename(tmp, path) < 0) {
		ret = -errno;
		unlink(tmp);
		return ret;
	}
	fsync_dir_of(path);
	return 0;
}

int hwrite_atomic(const char *path, const void *data, size_t size, bool backup)
{
	struct wpiece p = { data, size };

	return hwrite_atomic_v(path, &p, 1, backup);
}

int hcopy_file(const char *src, const char *dst)
{
	size_t size;
	void *data = hread_file(src, &size);
	int ret;

	if (!data)
		return -errno ? -errno : -EIO;
	ret = hwrite_atomic(dst, data, size, false);
	free(data);
	return ret;
}

uint64_t hhash64_cont(uint64_t h, const void *data, size_t n)
{
	const unsigned char *p = data;
	size_t i = 0;

	/* 8 bytes per step (FNV-style mix on words), then the tail bytewise. */
	for (; i + 8 <= n; i += 8) {
		uint64_t w;

		memcpy(&w, p + i, 8);
		h ^= w;
		h *= 0x100000001b3ull;
		h ^= h >> 29;
	}
	for (; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

uint64_t hhash64(const void *data, size_t n)
{
	return hhash64_cont(HASH64_INIT, data, n);
}
