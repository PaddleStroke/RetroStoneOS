/*
 * systems.h - the systems the frontend knows (ROM folder name, display
 * name, theme folder aliases) and the libretro cores installed, read from
 * /usr/share/rsos/cores/<core>.ini (see docs/cores.md). A system is shown
 * only if it has ROMs AND at least one installed core lists it.
 */
#ifndef RSOS_UI_SYSTEMS_H
#define RSOS_UI_SYSTEMS_H

#include <stdbool.h>

#define MAX_CORES_PER_SYSTEM 6

struct core_info {
	char id[32];              /* "snes9x2005" */
	char name[48];            /* "Snes9x 2005" */
	char library[256];        /* .so path */
	char systems[256];        /* comma list */
	char exts[256];           /* comma list, lowercase, no dots */
	bool block_extract;
	bool savestates;
	bool experimental;        /* "experimental = true": labelled, never the automatic default */
	/* optional CPU profile for the power module (proposed keys
	 * cpu_governor = schedutil, cpu_max_khz = 720000) */
	char cpu_governor[16];
	int cpu_max_khz;
};

struct sysdef {
	const char *name;         /* ROM folder, e.g. "megadrive" */
	const char *fullname;     /* "Mega Drive" */
	const char *maker;        /* "Sega" (subtitle in the rsos themes) */
	const char *theme[4];     /* theme folder aliases, NULL-terminated */
	const char *folders[4];   /* other ROM folder names (RetroPie migration) */
	const char *default_core;
	const char *exts;         /* fallback extensions when no core ini exists */
	int year;                 /* for the system info text */
};

/* Loads every *.ini in dir. Returns the number of cores. When none is
 * found, a built-in fallback table (docs/cores.md) is used so the UI still
 * works on a development host. */
int systems_load_cores(const char *dir);
void systems_free(void);

/* Systems that at least one installed core can run, in carousel order:
 * known ones first (table order), then any other system named in a core
 * .ini. Valid after systems_load_cores(). */
int systems_count(void);
const struct sysdef *systems_get(int i);
const struct sysdef *systems_find(const char *name);

/* Cores able to run a system; [0] is the default: the table default,
 * then stable cores, then experimental ones. */
int systems_cores_for(const char *system, const struct core_info **out, int max);
/* The same, keeping only the cores whose extensions match the file
 * (zip/7z also match cores that let the frontend extract archives). */
int systems_cores_for_file(const char *system, const char *path,
			   const struct core_info **out, int max);
const struct core_info *systems_core(const char *id);
/* True if ext (no dot, any case) is a ROM extension for the system. */
bool systems_ext_ok(const char *system, const char *ext);

#endif
