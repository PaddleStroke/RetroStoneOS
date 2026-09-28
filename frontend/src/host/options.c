/*
 * options.c - see options.h.
 */
#include "options.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hutil.h"

/* 0 = core default (implicit), 1..4 = files, 5 = session overrides (the
 * benchmark's configuration: never written to a file). */
#define NLAYERS 6
#define L_GAME 4
#define L_OVERRIDE 5

static struct {
	struct core_opt *o;
	int n;
	struct ini layer[NLAYERS];  /* [1..5] used */
	/* the file exists but could not be read: never rewritten this session
	 * (review F-L15: it was replaced by only the new keys) */
	bool unreadable[NLAYERS];
	char core_id[64];
	char game[256];
	char user_dir[1024];
	char game_path[4096];
	char sys_path[4096];
	bool updated;
	bool dirty;
	retro_core_options_update_display_callback_t update_cb;
} O;

static char *xstrdup(const char *s)
{
	char *d = strdup(s ? s : "");

	if (!d) {
		hlog(HLOG_ERROR, "out of memory");
		abort();
	}
	return d;
}

static const char *layer_get(int l, const char *key)
{
	const char *v;

	if (l < 1 || l >= NLAYERS)
		return NULL;
	v = ini_get(&O.layer[l], "", key);
	if (!v)
		v = ini_get(&O.layer[l], "options", key);
	return v;
}

/* A user layer (saved by us): "no file" is empty, a read error is marked. */
static void load_user_layer(int l, const char *path)
{
	int r = ini_load(&O.layer[l], path);

	O.unreadable[l] = r < 0 && r != -ENOENT;
	if (O.unreadable[l])
		hlog(HLOG_ERROR, "core options: cannot read %s (%s): it will not be rewritten", path,
		     strerror(-r));
}

void opts_init(const char *core_id, const char *game, const struct ini *core_ini,
	       const struct opts_paths *paths)
{
	char p[4096];

	opts_free();
	memset(O.unreadable, 0, sizeof(O.unreadable));
	hstrlcpy(O.core_id, core_id, sizeof(O.core_id));
	hstrlcpy(O.game, game ? game : "", sizeof(O.game));
	hstrlcpy(O.user_dir, paths->user_dir, sizeof(O.user_dir));
	/* Layer 1: the core package's [options]. */
	if (core_ini)
		for (int i = 0; i < core_ini->n; i++)
			if (!strcasecmp(core_ini->e[i].section, "options"))
				ini_set(&O.layer[1], "", core_ini->e[i].key, core_ini->e[i].value);
	if (hpath(p, sizeof(p), "%s/%s.ini", paths->ship_dir, core_id))
		ini_load(&O.layer[2], p);
	hpath(O.sys_path, sizeof(O.sys_path), "%s/%s.ini", paths->user_dir, core_id);
	load_user_layer(3, O.sys_path);
	if (O.game[0]) {
		hpath(O.game_path, sizeof(O.game_path), "%s/%s/%s.ini", paths->user_dir, core_id, O.game);
		load_user_layer(4, O.game_path);
	}
	hlog(HLOG_INFO, "core options: %d package, %d shipped, %d system, %d game overrides",
	     O.layer[1].n, O.layer[2].n, O.layer[3].n, O.layer[4].n);
}

static void free_opt(struct core_opt *o)
{
	free(o->key);
	free(o->desc);
	free(o->info);
	free(o->category);
	for (int j = 0; j < o->nvals; j++) {
		free(o->vals[j].value);
		free(o->vals[j].label);
	}
	free(o->vals);
}

static void free_list(void)
{
	for (int i = 0; i < O.n; i++)
		free_opt(&O.o[i]);
	free(O.o);
	O.o = NULL;
	O.n = 0;
}

void opts_free(void)
{
	free_list();
	for (int l = 0; l < NLAYERS; l++)
		ini_free(&O.layer[l]);
	memset(&O, 0, sizeof(O));
}

static int find_val(const struct core_opt *o, const char *v)
{
	for (int j = 0; j < o->nvals; j++)
		if (!strcmp(o->vals[j].value, v))
			return j;
	return -1;
}

/* Value index from layers [1..top], else the core default. */
static int layered_index(const struct core_opt *o, int top)
{
	for (int l = top; l >= 1; l--) {
		const char *v = layer_get(l, o->key);
		int j;

		if (!v)
			continue;
		j = find_val(o, v);
		if (j >= 0)
			return j;
		{
			char k[256];

			snprintf(k, sizeof(k), "badopt:%d:%s", l, o->key);
			hlog_once(k, "option %s: value \"%s\" (layer %d) is not allowed, ignored", o->key, v, l);
		}
	}
	return o->def;
}

/*
 * Replaces the option list, keeping current values of keys that survive
 * (cores may declare their options again).
 */
static void install(struct core_opt *list, int n)
{
	for (int i = 0; i < n; i++) {
		struct core_opt *o = &list[i];
		int keep = -1;

		for (int k = 0; k < O.n; k++)
			if (!strcmp(O.o[k].key, o->key)) {
				keep = find_val(o, O.o[k].vals[O.o[k].cur].value);
				o->visible = O.o[k].visible;
			}
		o->cur = keep >= 0 ? keep : layered_index(o, NLAYERS - 1);
	}
	free_list();
	O.o = list;
	O.n = n;
	O.updated = true;
	hlog(HLOG_INFO, "core declared %d options", n);
}

static bool add_values(struct core_opt *o, const struct retro_core_option_value *vals,
		       const char *def)
{
	int n = 0;

	while (n < RETRO_NUM_CORE_OPTION_VALUES_MAX && vals[n].value)
		n++;
	if (!n)
		return false;
	o->vals = calloc((size_t)n, sizeof(*o->vals));
	if (!o->vals)
		return false;
	o->nvals = n;
	for (int j = 0; j < n; j++) {
		o->vals[j].value = xstrdup(vals[j].value);
		o->vals[j].label = xstrdup(vals[j].label && *vals[j].label ? vals[j].label : vals[j].value);
	}
	o->def = def ? find_val(o, def) : 0;
	if (o->def < 0)
		o->def = 0;
	return true;
}

bool opts_env_set_variables(const struct retro_variable *vars)
{
	int n = 0, m = 0;
	struct core_opt *list;

	if (!vars)
		return false;
	while (vars[n].key)
		n++;
	list = calloc((size_t)n + 1, sizeof(*list));
	if (!list)
		return false;
	for (int i = 0; i < n; i++) {
		/* "Description; a|b|c" */
		const char *v = vars[i].value ? vars[i].value : "";
		const char *semi = strchr(v, ';');
		struct core_opt *o = &list[m];
		const char *p;
		int cnt = 1;

		if (!semi)
			continue;
		o->key = xstrdup(vars[i].key);
		o->desc = strndup(v, (size_t)(semi - v));
		o->info = xstrdup("");
		o->category = xstrdup("");
		o->visible = true;
		p = semi + 1;
		while (*p == ' ')
			p++;
		for (const char *q = p; *q; q++)
			cnt += *q == '|';
		o->vals = calloc((size_t)cnt, sizeof(*o->vals));
		if (!o->desc || !o->vals) {
			free_opt(o);
			continue;
		}
		for (int j = 0; j < cnt; j++) {
			const char *e = strchr(p, '|');
			size_t l = e ? (size_t)(e - p) : strlen(p);

			o->vals[j].value = strndup(p, l);
			o->vals[j].label = strndup(p, l);
			o->nvals++;
			p = e ? e + 1 : p + l;
		}
		o->def = 0;
		m++;
	}
	install(list, m);
	return true;
}

bool opts_env_set_v1(const struct retro_core_option_definition *defs)
{
	int n = 0, m = 0;
	struct core_opt *list;

	if (!defs)
		return false;
	while (defs[n].key)
		n++;
	list = calloc((size_t)n + 1, sizeof(*list));
	if (!list)
		return false;
	for (int i = 0; i < n; i++) {
		struct core_opt *o = &list[m];

		o->key = xstrdup(defs[i].key);
		o->desc = xstrdup(defs[i].desc);
		o->info = xstrdup(defs[i].info);
		o->category = xstrdup("");
		o->visible = true;
		if (!add_values(o, defs[i].values, defs[i].default_value)) {
			free_opt(o);
			memset(o, 0, sizeof(*o));
			continue;
		}
		m++;
	}
	install(list, m);
	return true;
}

bool opts_env_set_v1_intl(const struct retro_core_options_intl *intl)
{
	/* English UI: always the `us` definitions. */
	return intl && opts_env_set_v1(intl->us);
}

bool opts_env_set_v2(const struct retro_core_options_v2 *v2)
{
	const struct retro_core_option_v2_definition *defs;
	int n = 0, m = 0;
	struct core_opt *list;

	if (!v2 || !v2->definitions)
		return false;
	defs = v2->definitions;
	while (defs[n].key)
		n++;
	list = calloc((size_t)n + 1, sizeof(*list));
	if (!list)
		return false;
	for (int i = 0; i < n; i++) {
		struct core_opt *o = &list[m];
		const char *cat = "";

		if (defs[i].category_key && v2->categories)
			for (const struct retro_core_option_v2_category *c = v2->categories; c->key; c++)
				if (!strcmp(c->key, defs[i].category_key))
					cat = c->desc ? c->desc : "";
		o->key = xstrdup(defs[i].key);
		o->desc = xstrdup(defs[i].desc);
		o->info = xstrdup(defs[i].info);
		o->category = xstrdup(cat);
		o->visible = true;
		if (!add_values(o, defs[i].values, defs[i].default_value)) {
			free_opt(o);
			memset(o, 0, sizeof(*o));
			continue;
		}
		m++;
	}
	install(list, m);
	return true;
}

bool opts_env_set_v2_intl(const struct retro_core_options_v2_intl *intl)
{
	return intl && opts_env_set_v2(intl->us);
}

bool opts_env_set_display(const struct retro_core_option_display *d)
{
	if (!d || !d->key)
		return false;
	for (int i = 0; i < O.n; i++)
		if (!strcmp(O.o[i].key, d->key)) {
			O.o[i].visible = d->visible;
			return true;
		}
	return false;
}

bool opts_env_get_variable(struct retro_variable *var)
{
	if (!var || !var->key)
		return false;
	for (int i = 0; i < O.n; i++)
		if (!strcmp(O.o[i].key, var->key)) {
			var->value = O.o[i].vals[O.o[i].cur].value;
			return true;
		}
	/* Not declared: a value from our files still answers (some cores
	 * query hidden keys). */
	for (int l = NLAYERS - 1; l >= 1; l--) {
		const char *v = layer_get(l, var->key);

		if (v) {
			var->value = v;
			return true;
		}
	}
	var->value = NULL;
	return false;
}

bool opts_env_set_variable(const struct retro_variable *var)
{
	if (!var)
		return true; /* "is SET_VARIABLE supported?" */
	if (!var->key || !var->value)
		return false;
	for (int i = 0; i < O.n; i++)
		if (!strcmp(O.o[i].key, var->key)) {
			int j = find_val(&O.o[i], var->value);

			if (j < 0)
				return false;
			O.o[i].cur = j;
			O.updated = true;
			return true;
		}
	return false;
}

bool opts_env_get_update(bool *updated)
{
	if (!updated)
		return false;
	*updated = O.updated;
	O.updated = false;
	return true;
}

void opts_env_set_update_display_cb(retro_core_options_update_display_callback_t cb)
{
	O.update_cb = cb;
}

int opts_count(void)
{
	return O.n;
}

const struct core_opt *opts_at(int i)
{
	return i >= 0 && i < O.n ? &O.o[i] : NULL;
}

void opts_step(int i, int dir)
{
	struct core_opt *o;

	if (i < 0 || i >= O.n)
		return;
	o = &O.o[i];
	o->cur = (o->cur + (dir < 0 ? o->nvals - 1 : 1)) % o->nvals;
	O.updated = true;
	O.dirty = true;
	if (O.update_cb)
		O.update_cb();
}

bool opts_dirty(void)
{
	return O.dirty;
}

bool opts_has_game_file(void)
{
	return O.game_path[0] && hfile_exists(O.game_path);
}

static int write_layer(const char *path, int layer, int below)
{
	char *buf;
	size_t cap = 4096, len = 0;
	int count = 0, ret;
	struct ini *ini = &O.layer[layer];

	if (O.unreadable[layer]) {
		hlog(HLOG_ERROR, "core options: %s could not be read: not overwritten", path);
		return -EIO;
	}
	buf = malloc(cap);
	if (!buf)
		return -ENOMEM;
	len += (size_t)snprintf(buf, cap, "; RetroStoneOS core options (%s)\n", O.core_id);
	/* Keys of this file the core did not declare this time are kept. */
	for (int e = 0; e < ini->n; e++) {
		bool declared = false;

		for (int i = 0; i < O.n; i++)
			declared |= !strcmp(O.o[i].key, ini->e[e].key);
		if (declared)
			continue;
		while (len + strlen(ini->e[e].key) + strlen(ini->e[e].value) + 16 > cap) {
			char *nb = realloc(buf, cap *= 2);

			if (!nb) {
				free(buf);
				return -ENOMEM;
			}
			buf = nb;
		}
		len += (size_t)sprintf(buf + len, "%s = \"%s\"\n", ini->e[e].key, ini->e[e].value);
	}
	for (int i = 0; i < O.n; i++) {
		const struct core_opt *o = &O.o[i];

		if (layer_get(L_OVERRIDE, o->key) && o->cur == layered_index(o, NLAYERS - 1)) {
			/* a session override (benchmark run): never saved, the
			 * file keeps what it had for this key */
			const char *keep = ini_get(ini, "", o->key);

			if (keep) {
				while (len + strlen(o->key) + strlen(keep) + 16 > cap) {
					char *nb = realloc(buf, cap *= 2);

					if (!nb) {
						free(buf);
						return -ENOMEM;
					}
					buf = nb;
				}
				len += (size_t)sprintf(buf + len, "%s = \"%s\"\n", o->key, keep);
			}
			continue;
		}
		if (o->cur == layered_index(o, below))
			continue;
		while (len + strlen(o->key) + strlen(o->vals[o->cur].value) + 16 > cap) {
			char *nb = realloc(buf, cap *= 2);

			if (!nb) {
				free(buf);
				return -ENOMEM;
			}
			buf = nb;
		}
		len += (size_t)sprintf(buf + len, "%s = \"%s\"\n", o->key, o->vals[o->cur].value);
		count++;
	}
	ret = hwrite_atomic(path, buf, len, false);
	if (!ret) {
		ini_free(ini);
		ini_parse(ini, buf);
	}
	free(buf);
	hlog(ret ? HLOG_ERROR : HLOG_INFO, "saved %d option(s) to %s%s%s", count, path,
	     ret ? ": " : "", ret ? strerror(-ret) : "");
	return ret;
}

int opts_save(bool per_game)
{
	char dir[4096];
	int ret;

	if (per_game) {
		if (!O.game_path[0])
			return -EINVAL;
		hpath_dir(O.game_path, dir, sizeof(dir));
		hmkdir_p(dir, 0755);
		ret = write_layer(O.game_path, 4, 3);
	} else {
		hmkdir_p(O.user_dir, 0755);
		ret = write_layer(O.sys_path, 3, 2);
		/* A per-game value would still win over what was just saved
		 * for all games: rewrite the per-game file as the difference
		 * from the new per-system values (normally empty). */
		if (!ret && O.game_path[0] && (O.layer[L_GAME].n || hfile_exists(O.game_path)))
			ret = write_layer(O.game_path, L_GAME, 3);
	}
	if (!ret)
		O.dirty = false;
	return ret;
}

void opts_reset(void)
{
	for (int i = 0; i < O.n; i++)
		O.o[i].cur = layered_index(&O.o[i], 2);
	O.updated = true;
	O.dirty = true;
	if (O.update_cb)
		O.update_cb();
}

void opts_refresh_display(void)
{
	if (O.update_cb)
		O.update_cb();
}

/* ------------------------------------------------ RetroStoneOS additions */

int opts_set_override(const char *key, const char *value)
{
	if (!key || !value)
		return -EINVAL;
	return ini_set(&O.layer[L_OVERRIDE], "", key, value);
}

enum opt_source opts_source(int i)
{
	const struct core_opt *o = opts_at(i);

	if (!o)
		return OPT_SRC_CORE;
	if (o->cur != layered_index(o, NLAYERS - 1))
		return OPT_SRC_UNSAVED;
	for (int l = NLAYERS - 1; l >= 1; l--) {
		const char *v = layer_get(l, o->key);

		if (v && find_val(o, v) >= 0)
			return l == L_OVERRIDE ? OPT_SRC_OVERRIDE : l == L_GAME ? OPT_SRC_GAME :
			       l == 3 ? OPT_SRC_SYSTEM : OPT_SRC_DEFAULT;
	}
	return OPT_SRC_CORE;
}

const char *opts_source_tag(enum opt_source s)
{
	switch (s) {
	case OPT_SRC_UNSAVED:
		return " *";
	case OPT_SRC_GAME:
		return " (game)";
	case OPT_SRC_SYSTEM:
		return " (all)";
	case OPT_SRC_OVERRIDE:
		return " (bench)";
	default:
		return "";
	}
}

int opts_autosave(void)
{
	int r;

	if (!O.dirty || !O.game_path[0])
		return 0;
	r = opts_save(true);
	hlog(r ? HLOG_ERROR : HLOG_INFO, "core options: changes saved for this game%s", r ? " FAILED" : "");
	return r ? r : 1;
}

void opts_reload_game(void)
{
	if (!O.game_path[0])
		return;
	ini_free(&O.layer[L_GAME]);
	load_user_layer(L_GAME, O.game_path);
	for (int i = 0; i < O.n; i++)
		O.o[i].cur = layered_index(&O.o[i], NLAYERS - 1);
	O.dirty = false;
}

const char *opts_value(const char *key)
{
	struct retro_variable v = { key, NULL };

	return opts_env_get_variable(&v) ? v.value : NULL;
}

void opts_log_effective(const char *const *always)
{
	for (int i = 0; i < O.n; i++) {
		const struct core_opt *o = &O.o[i];
		bool show = o->cur != o->def;

		for (int k = 0; always && always[k] && !show; k++)
			show = strstr(o->key, always[k]) != NULL;
		if (show)
			hlog(HLOG_INFO, "option %s = %s%s%s", o->key, o->vals[o->cur].value,
			     o->cur == o->def ? " (core default)" : "", opts_source_tag(opts_source(i)));
	}
	/* host-level keys (not declared by the core) */
	for (int k = 0; always && always[k]; k++)
		if (!strncmp(always[k], "rsos-", 5)) {
			const char *v = opts_value(always[k]);

			if (v)
				hlog(HLOG_INFO, "option %s = %s (host)", always[k], v);
		}
}
