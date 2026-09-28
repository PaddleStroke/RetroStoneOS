/*
 * coreinfo.h - RetroStoneOS core metadata (/usr/share/rsos/cores/<id>.ini,
 * format in docs/cores.md) and the BIOS check done before a launch.
 */
#ifndef RSOS_HOST_COREINFO_H
#define RSOS_HOST_COREINFO_H

#include <stdbool.h>
#include <stddef.h>

#include "ini.h"

#define COREINFO_MAX_BIOS 24

struct bios_entry {
	char file[128];
	char md5[33];          /* "" = not checked (arcade zip sets) */
	char required[128];    /* "no", "yes" or a list of systems/extensions */
	char description[160];
};

struct core_info {
	bool loaded;           /* false: no .ini found, defaults below */
	char id[64];
	char display_name[64];
	char library[256];
	char systems[256];
	char extensions[256];
	bool need_fullpath;
	bool block_extract;
	bool savestates;
	bool experimental;     /* never the automatic default (docs/cores.md) */
	char renderer[16];     /* "software" | "gles2" */
	char system_files[512];
	char system_tree[512];  /* directory copied into the system dir (bluemsx) */
	char default_systems[256]; /* optional: systems this core is the default for */
	struct bios_entry bios[COREINFO_MAX_BIOS];
	int nbios;
	struct ini ini;        /* whole file ([options] is read by options.c) */
};

/* "/usr/lib/libretro/fceumm_libretro.so" -> "fceumm" */
void coreinfo_id_from_path(const char *core_path, char *id, size_t n);

/* Loads <dir>/<id>.ini (dir NULL = /usr/share/rsos/cores). Missing file:
 * returns -ENOENT with ci->loaded = false and sane defaults. */
int coreinfo_load(struct core_info *ci, const char *dir, const char *id);
void coreinfo_free(struct core_info *ci);

/* Short user-facing system name: "psx" -> "PS1", "segacd" -> "Sega CD". */
const char *system_display_name(const char *system);

/*
 * Checks the BIOS files for (system, rom). Files are looked up in
 * bios_dir; zip sets without md5 (arcade) also next to the ROM.
 * Several files mandatory for the same condition (Sega CD US/EU/JP) are
 * alternatives: one of them is enough.
 * Returns true when the game can start. On false, err holds the message
 * for the UI ("PS1 needs scph5501.bin in /bios"). warn (may be NULL) gets
 * a non-blocking note (wrong md5, optional BIOS missing).
 */
bool bios_check(const struct core_info *ci, const char *system, const char *rom,
		const char *bios_dir, char *err, size_t errn, char *warn, size_t warnn);

/* Copies the ini's system_files into sys_dir when they are missing. */
void coreinfo_install_system_files(const struct core_info *ci, const char *sys_dir);
/* Copies the ini's system_tree below sys_dir, keeping relative paths,
 * creating directories, never overwriting (the user's files win). */
void coreinfo_install_system_tree(const struct core_info *ci, const char *sys_dir);

struct core_candidate {
	char id[64];
	char name[64];
	bool experimental;     /* show "(experimental)" */
	bool default_listed;   /* the .ini `default_systems` lists this system */
	bool is_default;       /* the automatic choice for this system and file */
};

/*
 * Cores that can run `rom` in `system`: `systems` lists the system and
 * `extensions` the file's extension (a .zip also matches a non-arcade core
 * whose extensions match a file inside it). Sorted by id. The automatic
 * default (is_default): first a non-experimental core whose `default_systems`
 * lists the system, then the docs/cores.md "ROM folders" table (which names
 * an experimental core only for neocd, the documented exception), then the
 * first non-experimental candidate.
 */
int coreinfo_candidates(const char *cores_dir, const char *system, const char *rom,
			struct core_candidate *out, int max);

/*
 * Which core runs (system, rom): the per-game choice, then the per-system
 * choice from choices_path (/data/rsos/cores.ini: `[snes] core = snes9x2010`,
 * `[snes/<rom stem>] core = ...`; experimental cores allowed there), if
 * that core accepts the file; else the automatic default above.
 * Writes the core id; returns 0 or -ENOENT (no non-experimental core can
 * open this file: the UI offers coreinfo_candidates()).
 */
int coreinfo_pick(const char *cores_dir, const char *choices_path, const char *system,
		  const char *rom, char *core_id, size_t n);

#endif
