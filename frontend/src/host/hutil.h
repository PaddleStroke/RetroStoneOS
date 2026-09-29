/*
 * hutil.h - small helpers for the libretro host: logging (with log-once),
 * monotonic time, paths, whole-file I/O and the durable atomic write used
 * for every save file (temp file, fsync, rename, fsync of the directory).
 *
 * Self-contained on purpose (libc only): the host also builds standalone
 * (rsos-run) without the UI's util.c.
 */
#ifndef RSOS_HOST_HUTIL_H
#define RSOS_HOST_HUTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hlog_level { HLOG_ERROR = 0, HLOG_WARN, HLOG_INFO, HLOG_DEBUG };

void hlog_set_level(enum hlog_level max);
bool hlog_enabled(enum hlog_level lvl);
void hlog(enum hlog_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hlogv(enum hlog_level lvl, const char *fmt, va_list ap);
/* Logs only the first time `key` is seen (per process). */
void hlog_once(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/*
 * Token bucket for log lines: `rate` lines a second on average, bursts of
 * up to `burst`. Returns true if this line may be logged; then *dropped is
 * the number of lines refused since the last one that passed (log it as a
 * "N lines suppressed" summary). Not thread-safe: the caller locks.
 */
struct hrate {
	double tokens;
	int64_t last_ms;
	unsigned long dropped;
	bool started;
};
bool hrate_take(struct hrate *r, int64_t now_ms, double rate, double burst, unsigned long *dropped);

int64_t hnow_us(void);   /* CLOCK_MONOTONIC */
int64_t hnow_ms(void);

/* Bounded string copy, always NUL-terminated, never cutting a UTF-8
 * sequence. Returns strlen(src). */
size_t hstrlcpy(char *dst, const char *src, size_t n);
/* Drops an incomplete UTF-8 sequence left at the end of s by a truncating
 * snprintf (translated text). */
void hutf8_trim(char *s);
/* snprintf that reports truncation as false. */
bool hpath(char *dst, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* "/a/b/game.v1.nes" -> "game.v1" */
void hpath_stem(const char *path, char *out, size_t n);
/* "/a/b/game.NES" -> "nes" (lowercase, no dot; "" if none) */
void hpath_ext(const char *path, char *out, size_t n);
/* "/a/b/game.nes" -> "/a/b" ("." if no slash) */
void hpath_dir(const char *path, char *out, size_t n);
/* "/a/b/game.nes" -> "game.nes" */
const char *hpath_base(const char *path);
/* Case-insensitive membership in a "a|b|c" or "a, b, c" list. */
bool hlist_has(const char *list, const char *item);

int hmkdir_p(const char *path, unsigned mode);
bool hfile_exists(const char *path);
long long hfile_size(const char *path);

/* Reads a whole file (malloc, NUL-terminated for convenience). */
void *hread_file(const char *path, size_t *size);
/* Reads at most `max` bytes into buf; returns bytes read or -errno. */
long hread_file_into(const char *path, void *buf, size_t max);

/*
 * Durable atomic write: <path>.tmp is written and fsync()ed, renamed over
 * <path>, then the directory is fsync()ed. If `backup` is set, an existing
 * <path> is first renamed to <path>.bak (undo for save states).
 * Returns 0 or -errno. A failed write never touches <path>.
 */
int hwrite_atomic(const char *path, const void *data, size_t size, bool backup);
/* Same, for data made of several pieces. */
struct wpiece { const void *data; size_t size; };
int hwrite_atomic_v(const char *path, const struct wpiece *p, int n, bool backup);

/* Copies a file (not atomic; used for system_files). */
int hcopy_file(const char *src, const char *dst);

/* 64-bit FNV-1a. */
uint64_t hhash64(const void *data, size_t n);
uint64_t hhash64_cont(uint64_t h, const void *data, size_t n);
#define HASH64_INIT 0xcbf29ce484222325ull

#endif
