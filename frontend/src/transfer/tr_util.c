/*
 * tr_util.c - path safety, the fsync'ing file sink and small helpers for
 * frontend/src/transfer/.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "tr_internal.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

/* Files over 2 GB (PSX/N64 images, backups): -D_FILE_OFFSET_BITS=64 on
 * 32-bit ARM (Buildroot's CPPFLAGS, transfer.mk's own flags). */
_Static_assert(sizeof(off_t) == 8, "the transfer module needs a 64-bit off_t");

/* ------------------------------------------------------------ basics */

void tr_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int64_t t = tr_now_ms();

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* one write per line: the import and backup threads log too */
	fprintf(stderr, "[%5lld.%03lld] transfer: %s\n", (long long)(t / 1000), (long long)(t % 1000), buf);
}

void tr_fmt_bytes(uint64_t b, char *out, size_t n)
{
	if (b >= (1ull << 30))
		snprintf(out, n, "%.1f GB", (double)b / (double)(1ull << 30));
	else if (b >= (1ull << 20))
		snprintf(out, n, "%.0f MB", (double)b / (double)(1ull << 20));
	else
		snprintf(out, n, "%.0f KB", (double)b / 1024.0);
}

int64_t tr_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

size_t tr_strlcpy(char *dst, const char *src, size_t n)
{
	size_t l = strlen(src);

	if (n) {
		size_t c = l < n - 1 ? l : n - 1;

		memcpy(dst, src, c);
		dst[c] = 0;
	}
	return l;
}

bool tr_str_ieq(const char *a, const char *b)
{
	return strcasecmp(a, b) == 0;
}

int tr_snprintf(char *dst, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(dst, n, fmt, ap);
	va_end(ap);
	return (r < 0 || (size_t)r >= n) ? -ENAMETOOLONG : 0;
}

/* --------------------------------------------------------- name checks */

/*
 * Decodes one UTF-8 sequence. Returns its length (1..4) and the code point,
 * or 0 if invalid (overlong, surrogate, > U+10FFFF, truncated).
 */
static int utf8_next(const unsigned char *s, uint32_t *cp)
{
	uint32_t c = s[0];
	int len, i;

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if ((c & 0xe0) == 0xc0) {
		len = 2;
		c &= 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		len = 3;
		c &= 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		len = 4;
		c &= 0x07;
	} else {
		return 0;
	}
	for (i = 1; i < len; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return 0;
		c = (c << 6) | (s[i] & 0x3f);
	}
	if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) || (len == 4 && c < 0x10000) ||
	    c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		return 0;
	*cp = c;
	return len;
}

static bool is_bad_char(uint32_t c)
{
	return c < 0x20 || c == 0x7f || strchr("/\\:*?\"<>|", (int)c) != NULL;
}

/* CON, PRN, AUX, NUL, COM0-9, LPT0-9, with or without an extension. */
static bool is_dos_device(const char *name)
{
	static const char *const dev3[] = { "con", "prn", "aux", "nul" };
	size_t base = strcspn(name, ".");

	/* Windows also ignores trailing spaces before the dot: "NUL .txt" */
	while (base > 0 && name[base - 1] == ' ')
		base--;
	if (base == 3) {
		for (size_t i = 0; i < 4; i++)
			if (!strncasecmp(name, dev3[i], 3))
				return true;
	}
	if (base == 4 && (!strncasecmp(name, "com", 3) || !strncasecmp(name, "lpt", 3)) &&
	    isdigit((unsigned char)name[3]))
		return true;
	return false;
}

int transfer_name_check(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;
	size_t units = 0, len;

	if (!name || !name[0])
		return -EINVAL;
	len = strlen(name);
	if (len > 255 * 3)
		return -ENAMETOOLONG;
	if (name[0] == '.')                 /* ".", "..", hidden, our temp files */
		return -EINVAL;
	if (name[len - 1] == '.' || name[len - 1] == ' ')
		return -EINVAL;
	while (*p) {
		uint32_t cp;
		int n = utf8_next(p, &cp);

		if (!n || is_bad_char(cp))
			return -EINVAL;
		units += cp >= 0x10000 ? 2 : 1;   /* exFAT counts UTF-16 units */
		p += n;
	}
	if (units > 255)
		return -ENAMETOOLONG;
	if (is_dos_device(name))
		return -EINVAL;
	return 0;
}

int transfer_relpath_check(const char *rel)
{
	char comp[256 * 3 + 1];
	const char *p = rel;
	int depth = 0;

	if (!rel || !rel[0] || rel[0] == '/')
		return -EINVAL;
	if (strlen(rel) >= TRANSFER_PATH_MAX)
		return -ENAMETOOLONG;
	while (*p) {
		size_t n = strcspn(p, "/");
		int r;

		if (n == 0)                        /* "a//b" or trailing '/' */
			return -EINVAL;
		if (n >= sizeof(comp))
			return -ENAMETOOLONG;
		memcpy(comp, p, n);
		comp[n] = 0;
		if ((r = transfer_name_check(comp)) < 0)
			return r;
		if (++depth > 8)
			return -EINVAL;
		p += n;
		if (*p == '/') {
			p++;
			if (!*p)
				return -EINVAL;
		}
	}
	return 0;
}

int transfer_name_sanitize(const char *in, char *out, size_t n)
{
	const unsigned char *p = (const unsigned char *)in;
	size_t o = 0;

	if (!in || n < 2)
		return -EINVAL;
	while (*p && o + 5 < n) {
		uint32_t cp;
		int l = utf8_next(p, &cp);

		if (!l) {                          /* invalid byte (Latin-1 name) */
			out[o++] = '_';
			p++;
			continue;
		}
		if (is_bad_char(cp)) {
			out[o++] = '_';
		} else {
			memcpy(out + o, p, (size_t)l);
			o += (size_t)l;
		}
		p += l;
	}
	out[o] = 0;
	while (o > 0 && (out[o - 1] == '.' || out[o - 1] == ' '))
		out[--o] = 0;
	return transfer_name_check(out) == 0 ? 0 : -EINVAL;
}

bool tr_is_junk(const char *name)
{
	static const char *const junk[] = {
		"System Volume Information", "$RECYCLE.BIN", "RECYCLER", "Thumbs.db",
		"desktop.ini", "__MACOSX", "found.000", "lost+found",
	};

	if (name[0] == '.')
		return true;
	for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++)
		if (tr_str_ieq(name, junk[i]))
			return true;
	return false;
}

static const char *ext_of(const char *name)
{
	const char *d = strrchr(name, '.');

	return d && d != name ? d + 1 : "";
}

bool tr_is_save_name(const char *name)
{
	static const char *const ext[] = { "srm", "sav", "rtc", "eep", "sra", "fla", "mpk", "mcr" };
	const char *e = ext_of(name);

	for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
		if (tr_str_ieq(e, ext[i]))
			return true;
	return false;
}

/* ".state", ".state1".. ".state99", ".state.auto", each optionally + ".png" */
bool tr_is_state_name(const char *name)
{
	const char *p = name, *hit = NULL;

	while ((p = strcasestr(p, ".state")) != NULL) {
		hit = p;
		p += 6;
	}
	if (!hit || hit == name)
		return false;
	p = hit + 6;
	if (!strncasecmp(p, ".auto", 5))
		p += 5;
	else
		while (isdigit((unsigned char)*p))
			p++;
	if (!*p)
		return true;
	return tr_str_ieq(p, ".png");
}

/* ------------------------------------------------------------ dirs */

int tr_open_dir_chain(int dirfd, const char *rel, bool create)
{
	char comp[256 * 3 + 1];
	const char *p = rel;
	int fd = fcntl(dirfd, F_DUPFD_CLOEXEC, 0);   /* review F-L7: CLOEXEC */

	if (fd < 0)
		return -errno;
	while (*p) {
		size_t n = strcspn(p, "/");
		int nfd;

		if (n == 0 || n >= sizeof(comp)) {
			close(fd);
			return -EINVAL;
		}
		memcpy(comp, p, n);
		comp[n] = 0;
		p += n;
		if (*p == '/')
			p++;
		if (!strcmp(comp, ".") || !strcmp(comp, "..")) {
			close(fd);
			return -EINVAL;
		}
		nfd = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (nfd < 0 && errno == ENOENT && create) {
			if (mkdirat(fd, comp, 0755) < 0 && errno != EEXIST) {
				int e = -errno;

				close(fd);
				return e;
			}
			nfd = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		}
		if (nfd < 0) {
			int e = -errno;

			close(fd);
			return e == -ELOOP ? -EACCES : e;
		}
		close(fd);
		fd = nfd;
	}
	return fd;
}

void tr_split_path(const char *rel, char *dir, size_t dn, char *base, size_t bn)
{
	const char *s = strrchr(rel, '/');

	if (!s) {
		if (dn)
			dir[0] = 0;
		tr_strlcpy(base, rel, bn);
		return;
	}
	if (dn) {
		size_t l = (size_t)(s - rel);

		if (l >= dn)
			l = dn - 1;
		memcpy(dir, rel, l);
		dir[l] = 0;
	}
	tr_strlcpy(base, s + 1, bn);
}

uint64_t tr_free_bytes(const char *path)
{
	struct statvfs v;

	if (statvfs(path, &v) < 0)
		return 0;
	return (uint64_t)v.f_bavail * v.f_frsize;
}

uint64_t tr_free_bytes_fd(int fd)
{
	struct statvfs v;

	if (fstatvfs(fd, &v) < 0)
		return 0;
	return (uint64_t)v.f_bavail * v.f_frsize;
}

/* ------------------------------------------------------------ sink */

/*
 * Write-behind (docs/rom-transfer.md §2.3, "Copy engine"): every
 * SINK_WB_CHUNK written, start the writeback of that chunk
 * (sync_file_range WRITE, returns at once) and wait for the chunk before
 * it, then drop it from the page cache. The card always has a chunk in
 * flight while the next one fills, the dirty memory stays under 2 chunks,
 * and nothing waits for a whole-file flush until commit(). The old engine
 * did fdatasync() every 8 MiB: the copy stopped while the card wrote, and
 * on exFAT each of those also flushed the whole device (sync_blockdev).
 */
#define SINK_WB_CHUNK (4u << 20)
#ifndef EXFAT_SUPER_MAGIC
#define EXFAT_SUPER_MAGIC 0x2011BAB0
#endif

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* 32 random bits for a temp name (getrandom; a counter if it fails). */
static uint32_t tmp_suffix(void)
{
	static uint32_t counter;
	uint32_t v;

	if (tr_random(&v, sizeof(v)) < 0)
		v = (uint32_t)getpid() * 2654435761u ^ (uint32_t)now_us() ^
		    __atomic_add_fetch(&counter, 0x9e3779b9u, __ATOMIC_RELAXED);
	return v;
}

int tr_sink_open(struct tr_sink *s, int dirfd, const char *name)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->dirfd = dirfd;
	if (tr_strlcpy(s->name, name, sizeof(s->name)) >= sizeof(s->name))
		return -ENAMETOOLONG;
	/*
	 * Every writer gets its own temp file: a random suffix, created with
	 * O_EXCL (review F-H1: the fixed ".<name>.rsos-part" opened with O_TRUNC
	 * let two uploads of one path write into the same file, and the mix was
	 * committed). ".<name>.<8 hex>.rsos-part", or for long names (over 255
	 * bytes with the suffix) ".rsos-part-<name hash>-<8 hex>". Dotfiles are
	 * hidden from the game scanner and from the web listing.
	 */
	for (int tries = 0; tries < 16; tries++) {
		uint32_t rnd = tmp_suffix();

		if (strlen(name) + 21 <= 255) {
			snprintf(s->tmp, sizeof(s->tmp), ".%s.%08x.rsos-part", name, rnd);
		} else {
			uint32_t h = 2166136261u;

			for (const char *p = name; *p; p++)
				h = (h ^ (unsigned char)*p) * 16777619u;
			snprintf(s->tmp, sizeof(s->tmp), ".rsos-part-%08x-%08x", h, rnd);
		}
		s->fd = openat(dirfd, s->tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
		if (s->fd >= 0)
			return 0;
		if (errno != EEXIST)
			break;
	}
	{
		int e = -errno;

		s->tmp[0] = 0;               /* not ours: abort() must not unlink it */
		return e;
	}
}

int tr_sink_write(struct tr_sink *s, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t w = write(s->fd, p, len);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += w;
		len -= (size_t)w;
		s->written += (uint64_t)w;
	}
	if (s->written - s->wb_started >= SINK_WB_CHUNK) {
		int64_t t0 = now_us();

		if (!s->no_wb &&
		    sync_file_range(s->fd, (off_t)s->wb_started, (off_t)(s->written - s->wb_started),
				    SYNC_FILE_RANGE_WRITE) < 0) {
			if (errno != ENOSYS && errno != EINVAL && errno != ESPIPE && errno != EOPNOTSUPP)
				return -errno;
			s->no_wb = true;         /* not supported here: plain fdatasync */
		}
		if (s->no_wb) {
			if (fdatasync(s->fd) < 0)
				return -errno;
			posix_fadvise(s->fd, 0, (off_t)s->written, POSIX_FADV_DONTNEED);
			s->wb_done = s->written;
		} else if (s->wb_started > s->wb_done) {
			if (sync_file_range(s->fd, (off_t)s->wb_done, (off_t)(s->wb_started - s->wb_done),
					    SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
					    SYNC_FILE_RANGE_WAIT_AFTER) < 0)
				return -errno;
			posix_fadvise(s->fd, (off_t)s->wb_done, (off_t)(s->wb_started - s->wb_done),
				      POSIX_FADV_DONTNEED);
			s->wb_done = s->wb_started;
		}
		s->wb_started = s->written;
		s->sync_us += now_us() - t0;
	}
	return 0;
}

int tr_sink_reserve(struct tr_sink *s, uint64_t size)
{
	struct statfs sf;

	if (s->fd < 0 || !size)
		return 0;
	/* FAT and most filesystems: allocate without changing the size */
	if (fallocate(s->fd, FALLOC_FL_KEEP_SIZE, 0, (off_t)size) == 0)
		return 0;
	if (errno == ENOSPC || errno == EFBIG)
		return -errno;
	/* exFAT has no fallocate, but growing the file allocates its clusters
	 * in one go without writing them (valid_size stays at 0: nothing is
	 * zero-filled). commit() trims it if fewer bytes came. */
	if (fstatfs(s->fd, &sf) == 0 && (uint32_t)sf.f_type == EXFAT_SUPER_MAGIC) {
		if (ftruncate(s->fd, (off_t)size) < 0)
			return errno == ENOSPC || errno == EFBIG ? -errno : 0;
		s->reserved = size;
	}
	return 0;
}

void tr_sink_abort(struct tr_sink *s)
{
	if (s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
	if (s->tmp[0])
		unlinkat(s->dirfd, s->tmp, 0);
	s->tmp[0] = 0;
}

/* renameat() that fails with -EEXIST instead of replacing: RENAME_NOREPLACE
 * (vfat and exFAT have it), else a hard link (fails if the name exists),
 * else a check then a rename (the last resort is not atomic). */
static int rename_noreplace(int dfd, const char *from, const char *to)
{
	struct stat st;

#ifdef SYS_renameat2
	if (syscall(SYS_renameat2, dfd, from, dfd, to, RENAME_NOREPLACE) == 0)
		return 0;
	if (errno != ENOSYS && errno != EINVAL)
		return -errno;
#endif
	if (linkat(dfd, from, dfd, to, 0) == 0) {
		unlinkat(dfd, from, 0);
		return 0;
	}
	if (errno == EEXIST)
		return -EEXIST;
	if (fstatat(dfd, to, &st, AT_SYMLINK_NOFOLLOW) == 0)
		return -EEXIST;
	return renameat(dfd, from, dfd, to) < 0 ? -errno : 0;
}

int tr_replace_file(int dfd, const char *tmp, const char *name, bool keep_bak, bool no_replace)
{
	char bak[300];
	struct stat st;

	if (no_replace)
		return rename_noreplace(dfd, tmp, name);
	if (!keep_bak || fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0 || !S_ISREG(st.st_mode))
		return renameat(dfd, tmp, dfd, name) < 0 ? -errno : 0;
	/* The backup was promised: no backup, no replacement (review F-M8:
	 * the rename to .bak was not checked, nor skipped quietly). */
	if (tr_snprintf(bak, sizeof(bak), "%s.bak", name) < 0 || strlen(bak) > 255)
		return -ENAMETOOLONG;
	if (unlinkat(dfd, bak, 0) < 0 && errno != ENOENT)
		return -errno;
	/* 1. hard links (ext4...): the old file also becomes name.bak, then one
	 *    atomic rename: name exists at every moment */
	if (linkat(dfd, name, dfd, bak, 0) == 0)
		return renameat(dfd, tmp, dfd, name) < 0 ? -errno : 0;
	if (errno != EPERM && errno != EOPNOTSUPP && errno != ENOSYS && errno != EMLINK)
		return -errno;
	/* 2. FAT/exFAT (no hard links): two checked renames, the first one undone
	 *    if the second fails. A power cut in between leaves only name.bak:
	 *    the loaders fall back to it. */
	if (renameat(dfd, name, dfd, bak) < 0)
		return -errno;
	if (renameat(dfd, tmp, dfd, name) < 0) {
		int e = -errno;

		renameat(dfd, bak, dfd, name);
		return e;
	}
	return 0;
}

int tr_sink_commit(struct tr_sink *s, int64_t mtime_s, long mtime_ns, bool keep_bak)
{
	int64_t t0 = now_us();
	int e = 0;

	if (s->reserved > s->written && ftruncate(s->fd, (off_t)s->written) < 0) {
		e = -errno;
		tr_sink_abort(s);
		return e;
	}
	if (mtime_s >= 0) {
		struct timespec ts[2] = {
			{ .tv_sec = 0, .tv_nsec = UTIME_OMIT },
			{ .tv_sec = (time_t)mtime_s, .tv_nsec = mtime_ns },
		};

		futimens(s->fd, ts);            /* best effort */
	}
	/* the one full flush of this file (data and metadata) */
	if (fsync(s->fd) < 0)
		e = -errno;
	posix_fadvise(s->fd, 0, 0, POSIX_FADV_DONTNEED);
	if (close(s->fd) < 0 && !e)
		e = -errno;
	s->fd = -1;
	s->sync_us += now_us() - t0;
	if (e) {
		tr_sink_abort(s);
		return e;
	}
	e = tr_replace_file(s->dirfd, s->tmp, s->name, keep_bak, s->no_replace);
	if (e < 0) {
		tr_sink_abort(s);
		return e;
	}
	s->tmp[0] = 0;
	if (s->defer_dirsync) {
		s->dir_unsynced = true;      /* the caller fsyncs the folder later */
		return 0;
	}
	t0 = now_us();
	e = fsync(s->dirfd) < 0 && errno != EINVAL ? -errno : 0;
	s->sync_us += now_us() - t0;
	return e;
}

/* read() until n bytes or EOF: bytes read, or -errno. */
static ssize_t read_full(int fd, char *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, buf + got, n - got);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	return (ssize_t)got;
}

/*
 * Source pages already copied leave the page cache every SRC_DROP_EVERY.
 * Only the consumed range: the old engine dropped the whole file after
 * every 256 KiB block, which threw away the readahead the kernel had just
 * done beyond it (on USB it was read twice).
 */
#define SRC_DROP_EVERY (8u << 20)

int tr_copy_to_sink(int sfd, struct tr_sink *s, char *buf, size_t bn, uint64_t *progress,
		    volatile int *cancel, void (*tick)(void *arg), void *arg)
{
	uint64_t pos = 0, dropped = 0;

	posix_fadvise(sfd, 0, 0, POSIX_FADV_SEQUENTIAL);
	for (;;) {
		ssize_t n = read(sfd, buf, bn);
		int e;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return 0;
		if ((e = tr_sink_write(s, buf, (size_t)n)) < 0)
			return e;
		pos += (uint64_t)n;
		if (progress)
			*progress += (uint64_t)n;
		if (pos - dropped >= SRC_DROP_EVERY) {
			posix_fadvise(sfd, (off_t)dropped, (off_t)(pos - dropped), POSIX_FADV_DONTNEED);
			dropped = pos;
		}
		if (cancel && *cancel)
			return -ECANCELED;
		if (tick)
			tick(arg);
	}
}

/* ------------------------------------------------------------ copier */

int tr_copier_init(struct tr_copier *c)
{
	memset(c, 0, sizeof(*c));
	c->dirfd = -1;
	c->buf = malloc((size_t)TR_COPY_SLOTS * TR_COPY_BLOCK);
	if (!c->buf)
		return -ENOMEM;
	pthread_mutex_init(&c->mu, NULL);
	pthread_cond_init(&c->cv, NULL);
	return 0;
}

void tr_copier_free(struct tr_copier *c)
{
	int e;

	if (!c->buf)
		return;
	if ((e = tr_copier_sync_dir(c)) < 0)
		tr_log("folder sync after the copy: %s", strerror(-e));
	pthread_mutex_lock(&c->mu);             /* read-ahead threads (<= 16 MiB each) */
	while (c->prefetching > 0)
		pthread_cond_wait(&c->cv, &c->mu);
	pthread_mutex_unlock(&c->mu);
	free(c->buf);
	c->buf = NULL;
	pthread_mutex_destroy(&c->mu);
	pthread_cond_destroy(&c->cv);
}

static void prefetch_next(struct tr_copier *c);

/* The reader thread: fills free slots from the source, in order. */
static void *copier_reader(void *arg)
{
	struct tr_copier *c = arg;
	uint64_t pos = 0, dropped = 0;

	for (;;) {
		int slot;
		ssize_t n;

		pthread_mutex_lock(&c->mu);
		while (c->count == TR_COPY_SLOTS && !c->stop)
			pthread_cond_wait(&c->cv, &c->mu);
		if (c->stop) {
			pthread_mutex_unlock(&c->mu);
			break;
		}
		slot = (c->head + c->count) % TR_COPY_SLOTS;
		pthread_mutex_unlock(&c->mu);

		n = read_full(c->sfd, c->buf + (size_t)slot * TR_COPY_BLOCK, TR_COPY_BLOCK);
		if (n > 0) {
			pos += (uint64_t)n;
			if (pos - dropped >= SRC_DROP_EVERY) {
				posix_fadvise(c->sfd, (off_t)dropped, (off_t)(pos - dropped), POSIX_FADV_DONTNEED);
				dropped = pos;
			}
		}
		pthread_mutex_lock(&c->mu);
		c->len[slot] = n;
		c->count++;
		pthread_cond_broadcast(&c->cv);
		pthread_mutex_unlock(&c->mu);
		if (n >= 0 && n < (ssize_t)TR_COPY_BLOCK)
			prefetch_next(c);   /* read to the end: the next file can start */
		if (n <= 0 || n < (ssize_t)TR_COPY_BLOCK)
			break;              /* EOF (a short block is the last one) or error */
	}
	return NULL;
}

void tr_copier_hint_next(struct tr_copier *c, const char *path, uint64_t size)
{
	c->next[0] = 0;
	c->next_size = 0;
	if (path && tr_strlcpy(c->next, path, sizeof(c->next)) >= sizeof(c->next))
		c->next[0] = 0;
	c->next_size = size;
}

/*
 * The current source is read: read the start of the next one into the page
 * cache in a short-lived thread, while this file is written and flushed.
 * (POSIX_FADV_WILLNEED is no use here: the kernel clamps it to the
 * readahead window, 128-256 KiB.) tr_copier_free() waits for them.
 */
#define PREFETCH_MAX (16u << 20)

struct prefetch {
	struct tr_copier *c;
	uint64_t n;
	char path[];
};

static void *prefetch_thread(void *arg)
{
	struct prefetch *p = arg;
	struct tr_copier *c = p->c;
	int fd = open(p->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

	if (fd >= 0) {
		char sink[64 * 1024];            /* the data only has to reach the cache */
		uint64_t got = 0;
		ssize_t r;

		posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
		while (got < p->n && (r = read(fd, sink, sizeof(sink))) > 0)
			got += (uint64_t)r;
		close(fd);
	}
	free(p);
	pthread_mutex_lock(&c->mu);
	c->prefetching--;
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mu);
	return NULL;
}

static void prefetch_next(struct tr_copier *c)
{
	size_t len = strlen(c->next);
	struct prefetch *p;
	pthread_attr_t at;
	pthread_t th;

	if (!len)
		return;
	p = malloc(sizeof(*p) + len + 1);
	if (p) {
		p->c = c;
		p->n = c->next_size < PREFETCH_MAX ? c->next_size : PREFETCH_MAX;
		memcpy(p->path, c->next, len + 1);
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		pthread_mutex_lock(&c->mu);
		c->prefetching++;
		pthread_mutex_unlock(&c->mu);
		if (pthread_create(&th, &at, prefetch_thread, p) != 0) {
			pthread_mutex_lock(&c->mu);
			c->prefetching--;
			pthread_mutex_unlock(&c->mu);
			free(p);
		}
		pthread_attr_destroy(&at);
	}
	c->next[0] = 0;
}

int tr_copy_file(struct tr_copier *c, int sfd, uint64_t size, struct tr_sink *s, uint64_t *progress,
		 volatile int *cancel, void (*tick)(void *arg), void *arg)
{
	pthread_t th;
	int64_t t0, sync0 = s->sync_us;
	int e = 0;

	posix_fadvise(sfd, 0, 0, POSIX_FADV_SEQUENTIAL);
	if ((e = tr_sink_reserve(s, size)) < 0)
		return e;
	c->files++;
	/* Small files, or no thread: read and write in turn (one block). */
	if (size <= TR_COPY_BLOCK) {
		uint64_t before = s->written;

		t0 = now_us();
		e = tr_copy_to_sink(sfd, s, c->buf, TR_COPY_BLOCK, progress, cancel, tick, arg);
		c->write_us += now_us() - t0 - (s->sync_us - sync0);
		c->bytes += s->written - before;
		c->sync_us += s->sync_us - sync0;
		if (e == 0)
			prefetch_next(c);
		return e;
	}
	c->sfd = sfd;
	c->head = c->count = 0;
	c->stop = false;
	if (pthread_create(&th, NULL, copier_reader, c) != 0) {
		t0 = now_us();
		e = tr_copy_to_sink(sfd, s, c->buf, TR_COPY_BLOCK, progress, cancel, tick, arg);
		c->write_us += now_us() - t0;
		c->bytes += s->written;
		return e;
	}
	for (;;) {
		ssize_t n;
		int slot;

		t0 = now_us();
		pthread_mutex_lock(&c->mu);
		while (c->count == 0)
			pthread_cond_wait(&c->cv, &c->mu);
		slot = c->head;
		n = c->len[slot];
		pthread_mutex_unlock(&c->mu);
		c->wait_us += now_us() - t0;       /* waiting for the source */
		if (n <= 0) {
			e = (int)n;                    /* 0 = end of file */
			break;
		}
		t0 = now_us();
		e = tr_sink_write(s, c->buf + (size_t)slot * TR_COPY_BLOCK, (size_t)n);
		c->write_us += now_us() - t0;
		if (e < 0)
			break;
		c->bytes += (uint64_t)n;
		if (progress)
			*progress += (uint64_t)n;
		pthread_mutex_lock(&c->mu);
		c->head = (c->head + 1) % TR_COPY_SLOTS;
		c->count--;
		pthread_cond_broadcast(&c->cv);
		pthread_mutex_unlock(&c->mu);
		if (n < (ssize_t)TR_COPY_BLOCK)
			break;                         /* that was the last block */
		if (cancel && *cancel) {
			e = -ECANCELED;
			break;
		}
		if (tick)
			tick(arg);
	}
	pthread_mutex_lock(&c->mu);
	c->stop = true;
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mu);
	pthread_join(th, NULL);
	/* the sink's write-behind waits happened inside write_us: move them */
	c->sync_us += s->sync_us - sync0;
	c->write_us -= s->sync_us - sync0;
	if (e == 0)
		prefetch_next(c);
	return e;
}

/*
 * The folder fsync that makes a rename durable is done once per folder (when
 * the next file goes elsewhere, and at the end: tr_copier_sync_dir()), not
 * after every file: each file is still complete before its rename (its own
 * fsync), so a power cut leaves the old file or the new one, at worst the
 * hidden temp file of the last renames (copied again next time). On exFAT the
 * next file's fsync flushes the whole device anyway. A replaced save (.bak)
 * still gets its folder fsync at once.
 */
int tr_copier_sync_dir(struct tr_copier *c)
{
	int64_t t0;
	int e = 0;

	if (c->dirfd < 0)
		return 0;
	t0 = now_us();
	if (fsync(c->dirfd) < 0 && errno != EINVAL)
		e = -errno;
	close(c->dirfd);
	c->dirfd = -1;
	c->sync_us += now_us() - t0;
	return e;
}

int tr_copier_commit(struct tr_copier *c, struct tr_sink *s, int64_t mtime_s, long mtime_ns, bool keep_bak)
{
	int64_t sync0 = s->sync_us;
	struct stat st;
	int e;

	s->defer_dirsync = !keep_bak;
	e = tr_sink_commit(s, mtime_s, mtime_ns, keep_bak);
	c->sync_us += s->sync_us - sync0;
	if (e || !s->dir_unsynced)
		return e;
	if (fstat(s->dirfd, &st) < 0)
		return tr_copier_sync_dir(c);
	if (c->dirfd >= 0 && (st.st_dev != c->dir_dev || st.st_ino != c->dir_ino))
		e = tr_copier_sync_dir(c);          /* another folder: the previous one now */
	if (c->dirfd < 0) {
		c->dirfd = fcntl(s->dirfd, F_DUPFD_CLOEXEC, 0);
		c->dir_dev = st.st_dev;
		c->dir_ino = st.st_ino;
	}
	return e;
}

/* ------------------------------------------------------------ groups */

static void group_key(const char *dst, char *key, size_t n)
{
	const char *p = strchr(dst, '/');

	/* "roms/snes/x.sfc" -> "roms/snes"; "bios/x.bin" -> "bios" */
	if (p && strchr(p + 1, '/'))
		p = strchr(p + 1, '/');
	else if (!p)
		p = dst + strlen(dst);
	snprintf(key, n, "%.*s", (int)(p - dst), dst);
}

static void group_flush(struct tr_group *g, const struct tr_copier *c)
{
	uint64_t bytes = c->bytes - g->bytes0;
	int files = (int)(c->files - g->files0);
	int64_t ms = tr_now_ms() - g->t0_ms - g->pause_ms;
	char a[32];

	if (!g->key[0] || !files)
		return;
	if (ms < 1)
		ms = 1;
	tr_fmt_bytes(bytes, a, sizeof(a));
	tr_log("%s %s: %d files, %s in %lld.%01lld s = %.1f MB/s (waiting for the source %.1f s, "
	       "writing %.1f s, flushing %.1f s)", g->what, g->key, files, a, (long long)(ms / 1000),
	       (long long)(ms % 1000 / 100), (double)bytes / 1048576.0 / ((double)ms / 1000.0),
	       (double)(c->wait_us - g->wait0) / 1e6, (double)(c->write_us - g->write0) / 1e6,
	       (double)(c->sync_us - g->sync0) / 1e6);
}

static void group_start(struct tr_group *g, const struct tr_copier *c, const char *key)
{
	tr_strlcpy(g->key, key, sizeof(g->key));
	g->t0_ms = tr_now_ms();
	g->pause_ms = 0;
	g->files0 = c->files;
	g->bytes0 = c->bytes;
	g->wait0 = c->wait_us;
	g->write0 = c->write_us;
	g->sync0 = c->sync_us;
}

void tr_group_step(struct tr_group *g, const struct tr_copier *c, const char *what, const char *dst)
{
	char key[sizeof(g->key)];

	group_key(dst, key, sizeof(key));
	g->what = what;
	if (g->key[0] && !strcmp(key, g->key))
		return;
	group_flush(g, c);
	group_start(g, c, key);
}

void tr_group_end(struct tr_group *g, const struct tr_copier *c)
{
	group_flush(g, c);
	g->key[0] = 0;
}

static int pread_full(int fd, char *buf, size_t n, off_t off)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = pread(fd, buf + got, n - got, off + (off_t)got);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	return (int)got;
}

int tr_same_sampled(int fda, int fdb, uint64_t size)
{
	enum { BLK = 64 * 1024, NBLK = 16 };
	char *a, *b;
	int r = 1;

	if (size <= (1u << 20))
		return tr_same_content(fda, fdb);
	a = malloc(BLK);
	b = malloc(BLK);
	if (!a || !b) {
		free(a);
		free(b);
		return -ENOMEM;
	}
	for (int i = 0; i < NBLK && r == 1; i++) {
		off_t off = (off_t)((size - BLK) / (NBLK - 1) * (uint64_t)i);
		int na, nb;

		if (i == NBLK - 1)
			off = (off_t)(size - BLK);        /* the tail exactly */
		na = pread_full(fda, a, BLK, off);
		nb = pread_full(fdb, b, BLK, off);
		if (na < 0 || nb < 0)
			r = na < 0 ? na : nb;
		else if (na != nb || memcmp(a, b, (size_t)na))
			r = 0;
	}
	free(a);
	free(b);
	return r;
}

int tr_same_content(int fda, int fdb)
{
	enum { B = 64 * 1024 };
	char *a = malloc(B), *b = malloc(B);
	int r = 1;

	if (!a || !b) {
		free(a);
		free(b);
		return -ENOMEM;
	}
	if (lseek(fda, 0, SEEK_SET) < 0 || lseek(fdb, 0, SEEK_SET) < 0) {
		r = -errno;
		goto out;
	}
	for (;;) {
		ssize_t na = read(fda, a, B), nb, got = 0;

		if (na < 0) {
			r = -errno;
			break;
		}
		while (got < na) {                 /* read exactly na from b */
			nb = read(fdb, b + got, (size_t)(na - got));
			if (nb < 0) {
				r = -errno;
				goto out;
			}
			if (nb == 0)
				break;
			got += nb;
		}
		if (got != na || memcmp(a, b, (size_t)na)) {
			r = 0;
			break;
		}
		if (na == 0) {
			char c;

			r = read(fdb, &c, 1) == 0 ? 1 : 0;
			break;
		}
	}
out:
	lseek(fda, 0, SEEK_SET);
	lseek(fdb, 0, SEEK_SET);
	free(a);
	free(b);
	return r;
}

/* ------------------------------------------------------------ buffer */

void tr_buf_init(struct tr_buf *b)
{
	memset(b, 0, sizeof(*b));
}

void tr_buf_free(struct tr_buf *b)
{
	free(b->p);
	memset(b, 0, sizeof(*b));
}

void tr_buf_add(struct tr_buf *b, const char *s, size_t n)
{
	if (b->oom)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		char *p;

		while (cap < b->len + n + 1)
			cap *= 2;
		p = realloc(b->p, cap);
		if (!p) {
			b->oom = true;
			return;
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}

void tr_buf_puts(struct tr_buf *b, const char *s)
{
	tr_buf_add(b, s, strlen(s));
}

void tr_buf_printf(struct tr_buf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(tmp)) {
		tr_buf_add(b, tmp, (size_t)n);
	} else {
		char *big = malloc((size_t)n + 1);

		if (!big) {
			b->oom = true;
			return;
		}
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		tr_buf_add(b, big, (size_t)n);
		free(big);
	}
}

void tr_buf_json_str(struct tr_buf *b, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	tr_buf_add(b, "\"", 1);
	while (*p) {
		uint32_t cp;
		int n = utf8_next(p, &cp);

		if (!n) {
			tr_buf_add(b, "\xef\xbf\xbd", 3);   /* U+FFFD */
			p++;
			continue;
		}
		if (cp == '"' || cp == '\\') {
			char e[2] = { '\\', (char)cp };

			tr_buf_add(b, e, 2);
		} else if (cp < 0x20 || cp == 0x7f) {
			tr_buf_printf(b, "\\u%04x", (unsigned)cp);
		} else if (cp == '<') {
			tr_buf_add(b, "\\u003c", 6);         /* never "</script>" */
		} else {
			tr_buf_add(b, (const char *)p, (size_t)n);
		}
		p += n;
	}
	tr_buf_add(b, "\"", 1);
}

/* ------------------------------------------------------------ misc */

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int tr_url_decode(char *s)
{
	char *r = s, *w = s;

	while (*r) {
		if (*r == '%') {
			int h = hexval((unsigned char)r[1]), l = h >= 0 ? hexval((unsigned char)r[2]) : -1;

			if (h < 0 || l < 0 || (h == 0 && l == 0))
				return -EINVAL;
			*w++ = (char)(h * 16 + l);
			r += 3;
		} else if (*r == '+') {
			*w++ = ' ';
			r++;
		} else {
			*w++ = *r++;
		}
	}
	*w = 0;
	return 0;
}

int tr_random(void *buf, size_t n)
{
	unsigned char *p = buf;

	while (n) {
		ssize_t r = getrandom(p, n, 0);

		if (r < 0) {
			int fd;

			if (errno == EINTR)
				continue;
			fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
			if (fd < 0)
				return -errno;
			r = read(fd, p, n);
			close(fd);
			if (r <= 0)
				return -EIO;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}
