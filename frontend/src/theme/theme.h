/*
 * theme.h - EmulationStation (RetroPie format, formatVersion 3..7) theme
 * loading: theme.xml parsing, <include>, ${variables}, <feature>,
 * <resolution>, view/element name lists, path resolution and the
 * per-system theme folder lookup.
 *
 * The result is a plain property store: views -> elements -> string
 * properties, with the ES merge rules (later definitions override single
 * properties, extras keep their first-definition order). Interpreting the
 * properties (sizes, fonts, colors) is up to the UI (ui/view_*.c), which
 * works at the current logical resolution.
 */
#ifndef RSOS_THEME_H
#define RSOS_THEME_H

#include <stdbool.h>
#include "../gfx/gfx.h"
#include "../ui/util.h"

struct theme_prop {
	const char *name;
	const char *value;          /* variables expanded, paths absolute */
};

struct theme_elem {
	const char *type;           /* image, text, textlist, carousel, ... */
	const char *name;
	bool extra;
	int order;                  /* first-definition order (extras) */
	struct theme_prop *props;
	int nprops, cap;
};

struct theme_view {
	const char *name;
	struct theme_elem **elems;
	int n, cap;
};

struct theme_var {
	const char *name, *value;
};

struct theme {
	struct arena arena;
	char path[1024];            /* main theme.xml ("" = no theme) */
	struct theme_view *views;
	int nviews, cap_views;
	struct theme_var *vars;
	int nvars, cap_vars;
	int version;                /* formatVersion */
	float res_w, res_h;         /* <resolution>, 0 = normalized */
	int next_order;
};

/* System variables (${system.name} etc.). */
struct theme_sysinfo {
	const char *name;           /* folder name, e.g. "snes" */
	const char *fullname;       /* "Super Nintendo" */
	const char *theme;          /* theme folder actually used */
};

/* Directory of ":/" resources (fonts). */
void theme_set_resource_dir(const char *dir);

/* Loads one theme file (and its includes). Never NULL: a missing or broken
 * file gives an empty theme (and a log line). */
struct theme *theme_load_file(const char *path, const struct theme_sysinfo *si);

/*
 * ES lookup order for a system:
 *   1. <rom_dir>/theme.xml
 *   2. <set_dir>/<name>/theme.xml for each name in theme_names (aliases)
 *   2b. (ours) the folder of a related console for systems that sets often
 *      lack: pcenginecd -> pcengine, neocd -> neogeo, wonderswancolor ->
 *      wonderswan, pico -> megadrive... (table in theme.c)
 *   3. <set_dir>/theme.xml (the set's default theme)
 * si->theme is updated to the folder that was used.
 */
struct theme *theme_load_system(const char *set_dir, const char *rom_dir,
				const char *const *theme_names,
				struct theme_sysinfo *si);
void theme_free(struct theme *t);

const struct theme_view *theme_view(const struct theme *t, const char *view);
/* type may be NULL (any). */
const struct theme_elem *theme_elem(const struct theme *t, const char *view,
				    const char *name, const char *type);
const char *theme_prop(const struct theme_elem *e, const char *prop);
bool theme_has(const struct theme_elem *e, const char *prop);

/* Typed accessors: return false (and leave *out alone) if absent/invalid. */
bool theme_get_pair(const struct theme_elem *e, const char *prop, float *x, float *y);
bool theme_get_float(const struct theme_elem *e, const char *prop, float *v);
bool theme_get_color(const struct theme_elem *e, const char *prop, gfx_color *c);
bool theme_get_bool(const struct theme_elem *e, const char *prop, bool *b);
const char *theme_get_str(const struct theme_elem *e, const char *prop);

/* Theme set discovery: a set is a directory holding a theme.xml or at least
 * one <system>/theme.xml. */
bool theme_is_set_dir(const char *dir);

#endif
