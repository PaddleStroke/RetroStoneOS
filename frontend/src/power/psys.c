/*
 * psys.c - sysfs / file helpers for the power module (see psys.h).
 */
#include "psys.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int psys_read_str(const char *path, char *buf, size_t n)
{
	int fd;
	ssize_t r;

	if (!n)
		return -EINVAL;
	buf[0] = '\0';
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do {
		r = read(fd, buf, n - 1);
	} while (r < 0 && errno == EINTR);
	if (r < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	close(fd);
	buf[r] = '\0';
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ' ||
			 buf[r - 1] == '\t' || buf[r - 1] == '\r'))
		buf[--r] = '\0';
	return 0;
}

int psys_read_long(const char *path, long *out)
{
	char buf[64], *end;
	long v;
	int ret = psys_read_str(path, buf, sizeof(buf));

	if (ret)
		return ret;
	errno = 0;
	v = strtol(buf, &end, 10);
	if (end == buf || errno)
		return -EINVAL;
	*out = v;
	return 0;
}

int psys_write_str(const char *path, const char *s)
{
	size_t len = strlen(s);
	ssize_t w;
	/* O_TRUNC is what "echo x > attr" does: ignored by sysfs, needed for
	 * the regular files of the test trees. */
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	do {
		w = write(fd, s, len);
	} while (w < 0 && errno == EINTR);
	if (w < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	if (close(fd) < 0)
		return -errno;
	return (size_t)w == len ? 0 : -EIO;
}

int psys_write_long(const char *path, long v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%ld", v);
	return psys_write_str(path, buf);
}

bool psys_path(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(out, n, fmt, ap);
	va_end(ap);
	return r >= 0 && (size_t)r < n;
}

int psys_write_atomic(const char *path, const void *data, size_t len)
{
	char tmp[PSYS_PATH_MAX], dir[PSYS_PATH_MAX];
	const char *slash;
	const char *p = data;
	size_t left = len;
	int fd, dfd, e = 0;

	if (!psys_path(tmp, sizeof(tmp), "%s.tmp", path))
		return -ENAMETOOLONG;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	while (left) {
		ssize_t w = write(fd, p, left);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			e = -errno;
			break;
		}
		p += w;
		left -= (size_t)w;
	}
	if (!e && fsync(fd) < 0)
		e = -errno;
	if (close(fd) < 0 && !e)
		e = -errno;
	if (!e && rename(tmp, path) < 0)
		e = -errno;
	if (e) {
		unlink(tmp);
		return e;
	}
	/* fsync the directory so the rename itself survives a power cut */
	slash = strrchr(path, '/');
	if (!slash)
		snprintf(dir, sizeof(dir), ".");
	else if (slash == path)
		snprintf(dir, sizeof(dir), "/");
	else
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	return 0;
}
