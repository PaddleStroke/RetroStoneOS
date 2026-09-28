/*
 * tr_internal.h - helpers shared inside frontend/src/transfer/ only.
 */
#ifndef RSOS_TR_INTERNAL_H
#define RSOS_TR_INTERNAL_H

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "transfer.h"

/* Logging to stderr with a "[uptime] transfer:" prefix (the frontend's
 * stderr goes to its log file). */
void tr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int64_t tr_now_ms(void);
size_t tr_strlcpy(char *dst, const char *src, size_t n);
bool tr_str_ieq(const char *a, const char *b);
/* snprintf that reports truncation: 0 or -ENAMETOOLONG. */
int tr_snprintf(char *dst, size_t n, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/*
 * Opens (and with create, makes) the directory chain rel under dirfd, one
 * component at a time with O_NOFOLLOW, so nothing can escape dirfd through
 * a symlink. rel must have passed transfer_relpath_check() (or be ""), and
 * is a directory path (no file name). Returns a new fd or -errno.
 */
int tr_open_dir_chain(int dirfd, const char *rel, bool create);

/* Splits "a/b/c.sfc" into dir "a/b" and base "c.sfc" (dir "" if none). */
void tr_split_path(const char *rel, char *dir, size_t dn, char *base, size_t bn);

/* Free bytes for an unprivileged writer (f_bavail), 0 on error. */
uint64_t tr_free_bytes(const char *path);
uint64_t tr_free_bytes_fd(int fd);

/*
 * File sink: writes "<dir>/.<name>.<8 hex>.rsos-part" (O_EXCL, one per writer),
 * then commit() fsyncs it,
 * sets the mtime, renames it over <name> (optionally keeping the old file
 * as <name>.bak) and fsyncs the directory. abort() removes the temp file.
 * Write-behind every 4 MiB (sync_file_range: start the new chunk, wait for
 * the previous one, drop it from the cache) keeps the A20's 1 GB from
 * filling with dirty pages without stopping the copy; the one fsync per
 * file is in commit().
 */
struct tr_sink {
	int dirfd;          /* borrowed */
	int fd;
	char name[256];
	char tmp[300];
	uint64_t written;
	uint64_t wb_started, wb_done;   /* write-behind: started up to, finished up to */
	uint64_t reserved;              /* exFAT: file grown to this size by reserve() */
	bool no_wb;                     /* sync_file_range unsupported: fdatasync */
	int64_t sync_us;                /* time waiting for the card (write-behind, fsyncs) */
	bool defer_dirsync;             /* commit(): leave the folder fsync to the caller */
	bool dir_unsynced;              /* ... and it was left */
	bool no_replace;                /* commit(): -EEXIST if the name exists (atomic) */
};

/* Moves dirfd/tmp to dirfd/name. keep_bak: an existing regular name is kept
 * as name.bak (hard link + one rename where possible, else two checked
 * renames undone on failure; -ENAMETOOLONG if name.bak cannot exist).
 * no_replace: fails with -EEXIST instead of replacing. 0 or -errno. */
int tr_replace_file(int dirfd, const char *tmp, const char *name, bool keep_bak, bool no_replace);

int tr_sink_open(struct tr_sink *s, int dirfd, const char *name);
int tr_sink_write(struct tr_sink *s, const void *buf, size_t len);
/* Pre-allocates `size` bytes (fallocate KEEP_SIZE; exFAT: ftruncate, which
 * allocates without zero-filling). 0, or -ENOSPC/-EFBIG (does not fit). */
int tr_sink_reserve(struct tr_sink *s, uint64_t size);
/* mtime_s < 0: keep "now". keep_bak: rename an existing file to name.bak
 * first. Returns 0 or -errno (the temp file is removed on failure). */
int tr_sink_commit(struct tr_sink *s, int64_t mtime_s, long mtime_ns, bool keep_bak);
void tr_sink_abort(struct tr_sink *s);

/*
 * Copies sfd (from its current offset) into an open sink with
 * posix_fadvise(DONTNEED) on the source. *progress grows with every block;
 * tick(arg) is called after each block. Returns 0, -ECANCELED when *cancel
 * became non-zero, or -errno (the caller aborts the sink).
 */
int tr_copy_to_sink(int sfd, struct tr_sink *s, char *buf, size_t bn, uint64_t *progress,
		    volatile int *cancel, void (*tick)(void *arg), void *arg);

/*
 * The copy engine (docs/rom-transfer.md §2.3): a reader thread fills
 * TR_COPY_SLOTS blocks of TR_COPY_BLOCK from the source (sequential
 * readahead, copied pages dropped) while the caller writes the previous
 * ones to the sink, so the stick and the card work at the same time. Files
 * up to one block are copied in turn without the thread. One per run
 * (buffers allocated once); its statistics are cumulative.
 */
#define TR_COPY_BLOCK (1u << 20)
#define TR_COPY_SLOTS 4

struct tr_copier {
	char *buf;                   /* TR_COPY_SLOTS x TR_COPY_BLOCK */
	/* statistics */
	uint64_t files, bytes;
	int64_t wait_us;             /* the writer waited for the source */
	int64_t write_us;            /* in write() (page cache, dirty throttling) */
	int64_t sync_us;             /* waiting for the destination to write back */
	/* pipeline of the current file */
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int sfd, head, count;
	ssize_t len[TR_COPY_SLOTS];
	bool stop;
	/* the folder whose fsync is pending (tr_copier_commit) */
	int dirfd;
	dev_t dir_dev;
	ino_t dir_ino;
	/* the next source (tr_copier_hint_next) and the read-ahead threads running */
	char next[TRANSFER_PATH_MAX];
	uint64_t next_size;
	int prefetching;
};

int tr_copier_init(struct tr_copier *c);
void tr_copier_free(struct tr_copier *c);
/* Reserves `size` in the sink, then copies sfd (from offset 0) into it.
 * Returns 0, -ECANCELED or -errno (the caller aborts the sink). */
int tr_copy_file(struct tr_copier *c, int sfd, uint64_t size, struct tr_sink *s, uint64_t *progress,
		 volatile int *cancel, void (*tick)(void *arg), void *arg);
/* tr_sink_commit(), its flush time counted in c->sync_us; the folder fsync is
 * deferred until another folder or tr_copier_sync_dir() (keep_bak: at once). */
int tr_copier_commit(struct tr_copier *c, struct tr_sink *s, int64_t mtime_s, long mtime_ns, bool keep_bak);
/* The pending folder fsync (also done by tr_copier_free()). 0 or -errno. */
int tr_copier_sync_dir(struct tr_copier *c);
/* The file the caller copies next: once the current file is read, its first
 * 16 MiB are read ahead (POSIX_FADV_WILLNEED) while the current file is
 * written and flushed, so the source and the destination overlap across
 * files too (small files: the read then costs nothing). NULL = none. */
void tr_copier_hint_next(struct tr_copier *c, const char *path, uint64_t size);

/*
 * Throughput per group of files (the destination's first two path levels,
 * "roms/snes", "bios"): one log line when the group changes and at the end,
 * "import roms/snes: 15 files, 64 MB in 5.2 s = 12.3 MB/s (waiting for the
 * source 0.3 s, writing 3.9 s, flushing 1.0 s)". pause_ms: time to leave
 * out (a question on screen).
 */
struct tr_group {
	const char *what;
	char key[64];
	int64_t t0_ms, pause_ms;
	uint64_t files0, bytes0;
	int64_t wait0, write0, sync0;
};
void tr_group_step(struct tr_group *g, const struct tr_copier *c, const char *what, const char *dst);
void tr_group_end(struct tr_group *g, const struct tr_copier *c);

/* True if the two open files have the same content (reads both). */
int tr_same_content(int fda, int fdb);
/*
 * Cheap equality test for two files of the same size: files up to 1 MiB are
 * compared in full, bigger ones by 16 blocks of 64 KiB spread over the file
 * (head and tail included). 1 = the samples match (the files are probably
 * identical: compare in full before trusting it), 0 = they differ, -errno.
 */
int tr_same_sampled(int fda, int fdb, uint64_t size);

/* "1.2 GB", "340 MB", "12 KB" (for logs). */
void tr_fmt_bytes(uint64_t b, char *out, size_t n);

/* Junk that is never imported, listed or counted as a game. */
bool tr_is_junk(const char *name);

/* Growable string buffer (JSON output). */
struct tr_buf {
	char *p;
	size_t len, cap;
	bool oom;
};
void tr_buf_init(struct tr_buf *b);
void tr_buf_free(struct tr_buf *b);
void tr_buf_add(struct tr_buf *b, const char *s, size_t n);
void tr_buf_puts(struct tr_buf *b, const char *s);
void tr_buf_printf(struct tr_buf *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
/* Appends s as a JSON string literal (with quotes); invalid UTF-8 bytes
 * become U+FFFD. */
void tr_buf_json_str(struct tr_buf *b, const char *s);

/* Decodes %XX and '+' in place. Returns -EINVAL for a bad escape or %00. */
int tr_url_decode(char *s);

/* Random bytes (getrandom, /dev/urandom fallback). 0 or -errno. */
int tr_random(void *buf, size_t n);

/* An import / a backup thread exists (they exclude each other). */
bool tr_import_busy(void);
bool tr_backup_busy(void);

/* Save/state extension checks used by import and the web share. */
bool tr_is_save_name(const char *name);
bool tr_is_state_name(const char *name);

#endif
