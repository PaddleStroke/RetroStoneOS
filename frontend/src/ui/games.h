/*
 * games.h - game lists: ROM folder scan, ES gamelist.xml (Skraper and
 * friends), a scan cache, and the per-game database (favorites, last
 * played, play count, per-game core).
 *
 * Scan cache: <cache_dir>/scan/<system>.bin holds the scanned list plus
 * the mtimes of every scanned directory and of gamelist.xml. When none
 * changed, loading a system is one small file read (no readdir, no XML).
 */
#ifndef RSOS_UI_GAMES_H
#define RSOS_UI_GAMES_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h"

struct game {
	const char *path;        /* absolute ROM path */
	const char *rel;         /* relative to the system ROM folder */
	const char *system;      /* folder name ("snes"); owner system */
	const char *name;        /* display name */
	const char *desc, *image, *thumbnail, *marquee, *video;
	const char *developer, *publisher, *genre, *players;
	float rating;            /* 0..1, < 0 unknown */
	int64_t releasedate;     /* unix time, 0 unknown */
	int64_t lastplayed;      /* unix time, 0 never */
	int playcount;
	bool favorite, hidden, kidgame;
	const char *core;        /* per-game core override, NULL = system default */
	/* per-game data from gamedb.tsv (batch 2) */
	int64_t playtime;        /* seconds played (menus and pauses excluded) */
	const char *scale;       /* scaling override: aspect/integer/stretch, NULL = Settings */
	const char *cpu;         /* CPU profile: performance/powersave, NULL = automatic */
};

/* Game list order (Settings > Game lists > Sort games by, key gamelist_sort). */
enum games_sort { GAMES_SORT_NAME = 0, GAMES_SORT_PLAYTIME, GAMES_SORT_LASTPLAYED };

struct gamelist {
	struct arena arena;
	char system[32];
	char rom_dir[512];
	struct game *games;      /* sorted */
	int n;
	bool has_media;          /* some game has an image or a description */
	bool from_cache;
	int64_t load_us;
	/* what the list was built from (change detection) */
	char **dirs;
	int64_t *dir_mtime;
	int ndirs;
	char glpath[1024];
	int64_t gl_mtime, gl_size;
	bool cache_written;      /* games_load() wrote the scan cache (sync it later) */
};

/* True if a scanned folder or the gamelist.xml changed since the list was
 * built (a few stat() calls; used when coming back to the system view). */
bool games_changed(const struct gamelist *gl);
/* Deletes the scan cache of a system ("Refresh game lists"). */
void games_invalidate(const char *cache_dir, const char *system);

/* Where the time of one games_load() went (boot instrumentation). */
struct games_load_stats {
	int64_t total_us;
	int64_t dir_us;          /* does the ROM folder exist (stat) */
	int64_t read_us;         /* scan cache: open + read */
	int64_t stat_us;         /* scan cache validation: stat of the folders, gamelist.xml */
	int64_t parse_us;        /* scan cache: decoding */
	int64_t scan_us;         /* cache miss: readdir of the folders */
	int64_t xml_us;          /* cache miss: gamelist.xml */
	int64_t save_us;         /* cache miss: writing the scan cache */
	int ndirs;
	bool from_cache;
};

/*
 * Loads a system's list: the cache when valid, else a scan (+ gamelist.xml
 * from rom_dir or from <alt_gamelists>/<system>/gamelist.xml). Returns NULL
 * if the folder does not exist; an empty list if it has no ROMs.
 * Thread-safe (the background loader runs it): it only reads the core
 * tables (read-only after systems_load_cores()) and touches no UI state.
 * st may be NULL.
 */
struct gamelist *games_load(const char *system, const char *rom_dir,
			    const char *cache_dir, const char *alt_gamelists,
			    struct games_load_stats *st);
/* Hash of the core extension lists of a system (part of the cache keys). */
uint64_t games_ext_key(const char *system);
/* Same games (paths, names, metadata) in the same order. */
bool games_equal(const struct gamelist *a, const struct gamelist *b);
/* Folds a list into a hash (tests and logs: "the final state equals a full load"). */
uint64_t games_digest(const struct gamelist *gl, uint64_t h);
void games_free(struct gamelist *gl);
void games_sort(struct gamelist *gl, bool favorites_first);
/* The same with an order: most played / recently played first, then the
 * name (favorites first when asked, as for the name order). */
void games_sort_by(struct gamelist *gl, bool favorites_first, enum games_sort how);
/* Removes the hidden games (gamelist.xml hidden=true or gamedb Hide) from a
 * list, in place. Returns how many were removed. */
int games_drop_hidden(struct gamelist *gl);
/* Removes one game (by path) from a list in place. true if found. */
bool games_remove(struct gamelist *gl, const char *path);
/* The letter group of a name for the jump-to-letter keys: its first letter
 * in uppercase ("A".."Z", accents folded: "É" -> "E"), "#" for a digit or a
 * sign; out holds one UTF-8 character. */
void games_letter(const char *name, char *out, size_t n);
/* Case- and accent-insensitive substring match (search). "" matches all. */
bool games_match(const char *name, const char *query);

/* Cleans a file name for display: no extension, no (tags) or [tags]. */
void games_clean_name(const char *file, char *out, size_t n);

/* ------------------------------------------------------ per-game data */
struct gamedb;
struct gamedb *gamedb_open(const char *path);   /* /data/rsos/gamedb.tsv */
void gamedb_close(struct gamedb *db);
/* Copies favorite/lastplayed/playcount/core into the list's games. */
void gamedb_apply(struct gamedb *db, struct gamelist *gl);
void gamedb_set_favorite(struct gamedb *db, struct game *g, bool fav);
void gamedb_set_core(struct gamedb *db, struct game *g, const char *core);
void gamedb_played(struct gamedb *db, struct game *g, int64_t when);
/* Batch 2: play time (seconds added), hidden, per-game scale / cpu ("" or
 * NULL = back to the default), and forgetting a deleted game. */
void gamedb_add_playtime(struct gamedb *db, struct game *g, int64_t seconds);
void gamedb_set_hidden(struct gamedb *db, struct game *g, bool hidden);
void gamedb_set_scale(struct gamedb *db, struct game *g, const char *scale);
void gamedb_set_cpu(struct gamedb *db, struct game *g, const char *cpu);
void gamedb_forget(struct gamedb *db, const char *system, const char *rel);
/* The stored play time of a game (0 if none), without a list entry. */
int64_t gamedb_playtime(struct gamedb *db, const char *system, const char *rel);
int gamedb_save(struct gamedb *db);
/* Iterates favorites / recently played (for the virtual systems). */
typedef void (*gamedb_iter_fn)(const char *system, const char *rel, int64_t lastplayed,
			       void *user);
void gamedb_each(struct gamedb *db, bool favorites, gamedb_iter_fn fn, void *user);

#endif
