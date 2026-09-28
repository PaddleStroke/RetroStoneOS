/*
 * util.h - small helpers shared by the UI, theme and gfx code:
 * logging (with log-once), monotonic time, a bump arena, string helpers,
 * whole-file reading, hashing and path helpers.
 */
#ifndef RSOS_UI_UTIL_H
#define RSOS_UI_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ log */
enum ui_log_level {
	UI_LOG_ERROR = 0,
	UI_LOG_WARN,
	UI_LOG_INFO,
	UI_LOG_DEBUG,
};

typedef void (*ui_log_fn)(enum ui_log_level lvl, const char *msg, void *user);

/* NULL = stderr. Messages above max_level are dropped. */
void ui_log_set(ui_log_fn fn, void *user, enum ui_log_level max_level);
void ui_log(enum ui_log_level lvl, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* Logs only the first time a given key is seen (for "unsupported X"). */
void ui_log_once(const char *key, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define LOGE(...) ui_log(UI_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) ui_log(UI_LOG_WARN, __VA_ARGS__)
#define LOGI(...) ui_log(UI_LOG_INFO, __VA_ARGS__)
#define LOGD(...) ui_log(UI_LOG_DEBUG, __VA_ARGS__)

/* ----------------------------------------------------------------- time */
int64_t ui_now_ms(void);
int64_t ui_now_us(void);

/* ---------------------------------------------------------------- arena */
struct arena_chunk;
struct arena {
	struct arena_chunk *head;
	size_t chunk_size;
};

void arena_init(struct arena *a, size_t chunk_size);
void *arena_alloc(struct arena *a, size_t n);    /* zeroed, 8-aligned */
char *arena_strdup(struct arena *a, const char *s);
char *arena_strndup(struct arena *a, const char *s, size_t n);
char *arena_printf(struct arena *a, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void arena_free(struct arena *a);

/* -------------------------------------------------------------- strings */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
size_t strlcpy_(char *dst, const char *src, size_t n);
bool str_ieq(const char *a, const char *b);
bool str_starts(const char *s, const char *prefix);
bool str_ends_i(const char *s, const char *suffix);
char *str_trim(char *s);                 /* in place, returns start */
int str_casecmp_natural(const char *a, const char *b); /* for sorting names */
bool parse_bool(const char *s, bool def);
/* Splits a name list on whitespace and commas (ES view/element lists). */
int str_split_list(const char *s, char out[][64], int max);

/* ------------------------------------------------------------------ io */
/* Reads a whole file, NUL terminated. *len excludes the NUL. NULL on a read
 * error (never the part before it); an early end of file is fine (sysfs). */
char *file_read(const char *path, size_t *len);
/* Same; *err = 0, -ENOENT (no file) or the read error. */
char *file_read_err(const char *path, size_t *len, int *err);
/* User data written with file_write_atomic_bak(): the file, else on a read
 * error path.bak (*from_bak); NULL with *err = -ENOENT when there is no
 * file. *err is the main file's: a caller must not overwrite the file when
 * *err is neither 0 nor -ENOENT ("never overwrite what could not be read"). */
char *file_read_user(const char *path, size_t *len, int *err, bool *from_bak);
bool file_exists(const char *path);
bool dir_exists(const char *path);
/* Writes atomically and durably: path.tmp, fsync, rename, fsync(dir).
 * For user data (settings, favorites, mappings). */
int file_write_atomic(const char *path, const void *data, size_t len);
/* Same, the previous version kept as path.bak (settings, gamedb, rsos.env). */
int file_write_atomic_bak(const char *path, const void *data, size_t len);
/* Same without the fsyncs: for caches that can be rebuilt (a torn cache
 * file is detected by its header/size and simply regenerated). */
int file_write_atomic_nosync(const char *path, const void *data, size_t len);
int mkdir_p(const char *path);

/* --------------------------------------------------------------- paths */
/* dir of path into out ("a/b/c.xml" -> "a/b"). */
void path_dirname(const char *path, char *out, size_t n);
const char *path_basename(const char *path);
/* Joins and normalizes "." and ".." components. */
void path_join(const char *dir, const char *rel, char *out, size_t n);
void path_normalize(char *p);

/* -------------------------------------------------------------- hashing */
uint64_t hash64(const void *data, size_t len, uint64_t seed);
uint64_t hash64_str(const char *s, uint64_t seed);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

#endif
