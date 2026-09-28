/*
 * psys.h - tiny sysfs / file helpers for the power module.
 *
 * Every path is built from a root prefix ("/sys" on the target, a temp
 * directory in the unit tests), so the whole module runs against fake
 * sysfs trees on the build host.
 */
#ifndef RSOS_POWER_PSYS_H
#define RSOS_POWER_PSYS_H

#include <stdbool.h>
#include <stddef.h>

#define PSYS_PATH_MAX 512

/* Reads a whole small file (trailing whitespace stripped). 0 or -errno. */
int psys_read_str(const char *path, char *buf, size_t n);
/* Reads one integer (decimal). 0 or -errno (-EINVAL if not a number). */
int psys_read_long(const char *path, long *out);
/* Writes a string (no newline added). 0 or -errno. */
int psys_write_str(const char *path, const char *s);
int psys_write_long(const char *path, long v);

/* snprintf into a path buffer; returns false if truncated. */
bool psys_path(char *out, size_t n, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/*
 * Durable replace: write tmp, fsync, rename, fsync(dir). Returns 0 or
 * -errno. The directory must exist.
 */
int psys_write_atomic(const char *path, const void *data, size_t len);

#endif
