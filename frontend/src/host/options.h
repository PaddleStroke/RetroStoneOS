/*
 * options.h - libretro core options (SET_VARIABLES, the v1 and v2 core
 * options API, GET_VARIABLE/GET_VARIABLE_UPDATE, SET_CORE_OPTIONS_DISPLAY)
 * with RetroStoneOS layering. Effective value of a key, first found wins:
 *
 *   5. session overrides (benchmark runs only, never written)
 *   4. /data/rsos/coreopts/<core>/<game>.ini   per game   (user, in-game menu)
 *   3. /data/rsos/coreopts/<core>.ini          per system (user, in-game menu)
 *   2. /usr/share/rsos/coreopts/<core>.ini     RetroStone defaults (shipped)
 *   1. [options] of /usr/share/rsos/cores/<core>.ini (core package defaults)
 *   0. the core's own default
 *
 * Files use `key = "value"` (RetroArch .opt syntax, unquoted accepted). A
 * layer value that is not one of the option's allowed values is ignored
 * (logged once). User files only store the keys that differ from the layers
 * below, so shipped default updates still reach users.
 */
#ifndef RSOS_HOST_OPTIONS_H
#define RSOS_HOST_OPTIONS_H

#include <stdbool.h>

#include "../../third_party/libretro/libretro.h"
#include "ini.h"

struct opt_value {
	char *value;
	char *label;           /* display label (value if the core gave none) */
};

struct core_opt {
	char *key;
	char *desc;
	char *info;
	char *category;        /* category description ("" if none) */
	struct opt_value *vals;
	int nvals;
	int def;               /* index of the core default */
	int cur;               /* index of the current value */
	bool visible;
};

struct opts_paths {
	const char *ship_dir;  /* /usr/share/rsos/coreopts */
	const char *user_dir;  /* /data/rsos/coreopts */
};

void opts_init(const char *core_id, const char *game, const struct ini *core_ini,
	       const struct opts_paths *paths);
void opts_free(void);

/* Environment handlers (return what the environment callback returns). */
bool opts_env_set_variables(const struct retro_variable *vars);
bool opts_env_set_v1(const struct retro_core_option_definition *defs);
bool opts_env_set_v1_intl(const struct retro_core_options_intl *intl);
bool opts_env_set_v2(const struct retro_core_options_v2 *v2);
bool opts_env_set_v2_intl(const struct retro_core_options_v2_intl *intl);
bool opts_env_set_display(const struct retro_core_option_display *d);
bool opts_env_get_variable(struct retro_variable *var);
bool opts_env_set_variable(const struct retro_variable *var);
bool opts_env_get_update(bool *updated);
void opts_env_set_update_display_cb(retro_core_options_update_display_callback_t cb);
/* Lets the core update option visibility (before showing the list). */
void opts_refresh_display(void);

/* In-game menu. */
int opts_count(void);
const struct core_opt *opts_at(int i);
void opts_step(int i, int dir);    /* next/previous value (wraps) */
bool opts_dirty(void);             /* changed since the last save */
/* Saves the differences to the per-game (true) or per-system file. */
int opts_save(bool per_game);
/* Back to the shipped defaults (layers 0-2) for this session. */
void opts_reset(void);
/* True if a per-game file exists (menu hint). */
bool opts_has_game_file(void);

/*
 * Where the current value of option i comes from (shown in the menu). A
 * value changed in the menu is UNSAVED until opts_autosave() (menu closed,
 * options page left, game exit) or an explicit save.
 */
enum opt_source {
	OPT_SRC_CORE = 0,      /* the core's own default */
	OPT_SRC_DEFAULT,       /* RetroStoneOS defaults (layers 1-2) */
	OPT_SRC_SYSTEM,        /* saved for all games of this core */
	OPT_SRC_GAME,          /* saved for this game */
	OPT_SRC_OVERRIDE,      /* session override (benchmark run) */
	OPT_SRC_UNSAVED,       /* changed in the menu, not written yet */
};
enum opt_source opts_source(int i);
const char *opts_source_tag(enum opt_source s);   /* " (game)", " (all)", " *", "" */
/* Saves menu changes for this game if there are any: 1 saved, 0 nothing
 * to do, -errno. */
int opts_autosave(void);
/* Re-reads the per-game file (written behind our back, e.g. by the
 * benchmark's "Use this") and recomputes the values, without telling the
 * core (they apply at the next start). */
void opts_reload_game(void);
/* Session override (layer 5, above the user files, never saved): the
 * benchmark runs one configuration this way. Call before the core
 * declares its options. */
int opts_set_override(const char *key, const char *value);
/* Effective value of a key (declared or not), NULL if none. */
const char *opts_value(const char *key);
/* Logs the options that differ from the core default, plus every option
 * whose key contains one of `always` (NULL-terminated; "rsos-*" keys are
 * looked up in the files). */
void opts_log_effective(const char *const *always);

#endif
