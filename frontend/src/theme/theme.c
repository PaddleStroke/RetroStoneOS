/*
 * theme.c - see theme.h.
 */
#include "theme.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ui/xml.h"

static char g_res_dir[1024] = "/usr/share/rsos";

void theme_set_resource_dir(const char *dir)
{
	strlcpy_(g_res_dir, dir, sizeof(g_res_dir));
}

/*
 * What the renderer does with each element type and property. Anything not
 * listed is ignored and logged once. "partial" properties are parsed but
 * only approximated (see docs/ui-design.md, theme support matrix).
 */
struct support {
	const char *type;
	const char *props;    /* supported */
	const char *partial;  /* accepted, approximated or ignored on purpose */
};

#define COMMON "pos size origin visible zIndex "
static const struct support g_support[] = {
	{ "image", COMMON "maxSize path default tile color colorEnd gradientType",
	  "rotation rotationOrigin flipX flipY" },
	{ "text", COMMON "text color backgroundColor fontPath fontSize alignment "
		  "forceUppercase lineSpacing value", "rotation rotationOrigin" },
	{ "datetime", COMMON "color backgroundColor fontPath fontSize alignment "
		      "forceUppercase lineSpacing format displayRelative",
	  "rotation rotationOrigin" },
	{ "textlist", COMMON "selectorColor selectorColorEnd selectorGradientType "
		      "selectorImagePath selectorImageTile selectorHeight selectorOffsetY "
		      "selectedColor primaryColor secondaryColor fontPath fontSize "
		      "alignment horizontalMargin forceUppercase lineSpacing",
	  "scrollSound" },
	{ "rating", COMMON "filledPath unfilledPath color", "rotation rotationOrigin" },
	{ "helpsystem", "pos origin textColor iconColor fontPath fontSize",
	  "entrySpacing iconTextSpacing textStyle" },
	{ "carousel", "type size pos origin color colorEnd gradientType logoSize "
		      "logoScale logoAlignment maxLogoCount zIndex",
	  "logoRotation logoRotationOrigin defaultTransition" },
	{ "video", COMMON "maxSize showSnapshotNoVideo",
	  "default delay showSnapshotDelay rotation rotationOrigin" },
	{ "ninepatch", COMMON "path color", "" },
	/* Menu styling (Batocera-style "menu" view); a subset. */
	{ "menuBackground", "color path", "fadePath" },
	{ "menuText", "fontPath fontSize color selectedColor selectorColor separatorColor", "" },
	{ "menuTextSmall", "fontPath fontSize color", "" },
	{ "menuGroup", "fontPath fontSize color backgroundColor", "" },
	{ "menuSwitch", "", "pathOn pathOff" },
	{ "menuSlider", "", "path" },
	{ "menuButton", "", "path filledPath" },
	{ "menuIcons", "", "" },
};

static bool word_in(const char *list, const char *w)
{
	size_t l = strlen(w);

	while (*list) {
		while (*list == ' ')
			list++;
		if (!strncmp(list, w, l) && (list[l] == ' ' || list[l] == 0))
			return true;
		while (*list && *list != ' ')
			list++;
	}
	return false;
}

static const struct support *support_for(const char *type)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_support); i++)
		if (!strcmp(g_support[i].type, type))
			return &g_support[i];
	return NULL;
}

static void check_support(const char *type, const char *prop)
{
	const struct support *s = support_for(type);
	char key[160];

	if (!s)
		return; /* the element type itself was already reported */
	if (word_in(s->props, prop))
		return;
	snprintf(key, sizeof(key), "theme-prop:%s:%s", type, prop);
	if (word_in(s->partial, prop))
		ui_log_once(key, "theme: <%s> property <%s> is only partially supported (ignored)",
			    type, prop);
	else
		ui_log_once(key, "theme: <%s> property <%s> is not supported (ignored)",
			    type, prop);
}

static bool is_path_prop(const char *p)
{
	static const char *const names[] = {
		"path", "default", "fontPath", "filledPath", "unfilledPath",
		"selectorImagePath", "scrollSound", "backgroundImage", "gameImage",
		"folderImage", "fadePath", "pathOn", "pathOff",
	};

	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (!strcmp(p, names[i]))
			return true;
	return false;
}

/* ------------------------------------------------------------- building */
static struct theme_view *get_view(struct theme *t, const char *name, bool create)
{
	for (int i = 0; i < t->nviews; i++)
		if (!strcmp(t->views[i].name, name))
			return &t->views[i];
	if (!create)
		return NULL;
	if (t->nviews == t->cap_views) {
		t->cap_views = t->cap_views ? t->cap_views * 2 : 8;
		t->views = xrealloc(t->views, sizeof(*t->views) * (size_t)t->cap_views);
	}
	memset(&t->views[t->nviews], 0, sizeof(t->views[0]));
	t->views[t->nviews].name = arena_strdup(&t->arena, name);
	return &t->views[t->nviews++];
}

static struct theme_elem *get_elem(struct theme *t, struct theme_view *v,
				   const char *name, const char *type, bool extra)
{
	struct theme_elem *e;

	for (int i = 0; i < v->n; i++) {
		e = v->elems[i];
		if (!strcmp(e->name, name)) {
			if (strcmp(e->type, type)) {
				/* ES refuses this; we let the last type win. */
				ui_log_once(name, "theme: element \"%s\" redefined as <%s> (was <%s>)",
					    name, type, e->type);
				e->type = arena_strdup(&t->arena, type);
				e->nprops = 0;
			}
			if (extra)
				e->extra = true;
			return e;
		}
	}
	if (v->n == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 16;
		v->elems = xrealloc(v->elems, sizeof(*v->elems) * (size_t)v->cap);
	}
	e = arena_alloc(&t->arena, sizeof(*e));
	e->type = arena_strdup(&t->arena, type);
	e->name = arena_strdup(&t->arena, name);
	e->extra = extra;
	e->order = t->next_order++;
	v->elems[v->n++] = e;
	return e;
}

static void set_prop(struct theme *t, struct theme_elem *e, const char *name,
		     const char *value)
{
	for (int i = 0; i < e->nprops; i++) {
		if (!strcmp(e->props[i].name, name)) {
			e->props[i].value = arena_strdup(&t->arena, value);
			return;
		}
	}
	if (e->nprops == e->cap) {
		struct theme_prop *np;
		int nc = e->cap ? e->cap * 2 : 8;

		np = arena_alloc(&t->arena, sizeof(*np) * (size_t)nc);
		if (e->nprops)
			memcpy(np, e->props, sizeof(*np) * (size_t)e->nprops);
		e->props = np;
		e->cap = nc;
	}
	e->props[e->nprops].name = arena_strdup(&t->arena, name);
	e->props[e->nprops].value = arena_strdup(&t->arena, value);
	e->nprops++;
}

/* ------------------------------------------------------------ variables */
static const char *var_get(const struct theme *t, const char *name, size_t l)
{
	for (int i = 0; i < t->nvars; i++)
		if (strlen(t->vars[i].name) == l && !strncmp(t->vars[i].name, name, l))
			return t->vars[i].value;
	return NULL;
}

static void var_set(struct theme *t, const char *name, const char *value)
{
	/* First definition wins (ES inserts into a std::map). */
	if (var_get(t, name, strlen(name)))
		return;
	if (t->nvars == t->cap_vars) {
		t->cap_vars = t->cap_vars ? t->cap_vars * 2 : 16;
		t->vars = xrealloc(t->vars, sizeof(*t->vars) * (size_t)t->cap_vars);
	}
	t->vars[t->nvars].name = arena_strdup(&t->arena, name);
	t->vars[t->nvars].value = arena_strdup(&t->arena, value);
	t->nvars++;
}

static void expand_vars(const struct theme *t, const char *in, char *out, size_t n)
{
	size_t o = 0;

	while (*in && o + 1 < n) {
		if (in[0] == '$' && in[1] == '{') {
			const char *end = strchr(in + 2, '}');

			if (end) {
				const char *v = var_get(t, in + 2, (size_t)(end - in - 2));

				if (!v) {
					char key[128];

					snprintf(key, sizeof(key), "theme-var:%.*s", (int)(end - in - 2), in + 2);
					ui_log_once(key, "theme: undefined variable ${%.*s}",
						    (int)(end - in - 2), in + 2);
					v = "";
				}
				while (*v && o + 1 < n)
					out[o++] = *v++;
				in = end + 1;
				continue;
			}
		}
		out[o++] = *in++;
	}
	out[o] = 0;
}

static void resolve_path(const char *dir, const char *in, char *out, size_t n)
{
	if (!*in) {
		out[0] = 0;
		return;
	}
	if (in[0] == '~' && (in[1] == '/' || !in[1])) {
		const char *home = getenv("HOME");

		snprintf(out, n, "%s%s", home ? home : "", in + 1);
	} else if (in[0] == ':' && in[1] == '/') {
		snprintf(out, n, "%s/%s", g_res_dir, in + 2);
	} else if (in[0] == '/') {
		snprintf(out, n, "%s", in);
	} else {
		/* "./x", "../x" and bare names: relative to the theme file. */
		snprintf(out, n, "%s/%s", dir, in);
	}
	path_normalize(out);
}

/* --------------------------------------------------------------- parser */
/* A theme that includes itself, or a few files that include each other
 * several times, would parse an exponential number of files: the files
 * being parsed are on a stack (an include of one of them is skipped), and
 * a theme reads at most THEME_MAX_FILES files in all. */
#define THEME_MAX_DEPTH 16
#define THEME_MAX_FILES 64

struct load_ctx {
	struct theme *t;
	int depth;
	int nfiles;
	const char *open[THEME_MAX_DEPTH + 1];  /* the files being parsed */
};

static void parse_file(struct load_ctx *c, const char *path);

static void parse_element(struct load_ctx *c, struct theme_view *v,
			  const struct xml_node *en, const char *dir)
{
	struct theme *t = c->t;
	char names[16][64];
	const char *nattr = xml_attr(en, "name");
	bool extra = parse_bool(xml_attr(en, "extra"), false);
	int nn;

	if (!support_for(en->name)) {
		char key[96];

		snprintf(key, sizeof(key), "theme-type:%s", en->name);
		if (!strcmp(en->name, "sound"))
			ui_log_once(key, "theme: <sound> elements are ignored (no UI sounds)");
		else
			ui_log_once(key, "theme: element type <%s> is not supported (ignored)", en->name);
		return;
	}
	if (!nattr) {
		ui_log_once(en->name, "theme: <%s> without a name attribute (line %d) ignored",
			    en->name, en->line);
		return;
	}
	nn = str_split_list(nattr, names, 16);
	for (int k = 0; k < nn; k++) {
		struct theme_elem *e = get_elem(t, v, names[k], en->name, extra);

		for (const struct xml_node *p = en->child; p; p = p->next) {
			char val[2048], rp[2048];

			expand_vars(t, p->text, val, sizeof(val));
			check_support(en->name, p->name);
			if (is_path_prop(p->name)) {
				resolve_path(dir, str_trim(val), rp, sizeof(rp));
				set_prop(t, e, p->name, rp);
			} else {
				set_prop(t, e, p->name, str_trim(val));
			}
		}
	}
}

static void parse_views(struct load_ctx *c, const struct xml_node *parent, const char *dir)
{
	for (const struct xml_node *n = parent->child; n; n = n->next) {
		char vnames[16][64];
		int nv;

		if (strcmp(n->name, "view"))
			continue;
		nv = str_split_list(xml_attr(n, "name") ? xml_attr(n, "name") : "", vnames, 16);
		for (int i = 0; i < nv; i++) {
			struct theme_view *v = get_view(c->t, vnames[i], true);

			for (const struct xml_node *en = n->child; en; en = en->next)
				parse_element(c, v, en, dir);
		}
	}
}

/* Converts RESOLUTION_* values to normalized ones in place. */
static void apply_resolution(struct theme *t)
{
	static const char *const pairs[] = { "pos", "size", "maxSize", "margin" };
	static const char *const heights[] = { "fontSize", "selectorHeight", "selectorOffsetY" };

	if (t->res_w <= 0 || t->res_h <= 0)
		return;
	for (int i = 0; i < t->nviews; i++) {
		struct theme_view *v = &t->views[i];

		for (int j = 0; j < v->n; j++) {
			struct theme_elem *e = v->elems[j];

			for (int k = 0; k < e->nprops; k++) {
				struct theme_prop *p = &e->props[k];
				char buf[64];
				float x, y;

				for (size_t q = 0; q < ARRAY_SIZE(pairs); q++) {
					if (!strcmp(p->name, pairs[q]) &&
					    sscanf(p->value, "%f %f", &x, &y) == 2) {
						snprintf(buf, sizeof(buf), "%g %g", x / t->res_w, y / t->res_h);
						p->value = arena_strdup(&t->arena, buf);
					}
				}
				for (size_t q = 0; q < ARRAY_SIZE(heights); q++) {
					if (!strcmp(p->name, heights[q]) && sscanf(p->value, "%f", &x) == 1) {
						snprintf(buf, sizeof(buf), "%g", x / t->res_h);
						p->value = arena_strdup(&t->arena, buf);
					}
				}
				if (!strcmp(p->name, "horizontalMargin") && sscanf(p->value, "%f", &x) == 1) {
					snprintf(buf, sizeof(buf), "%g", x / t->res_w);
					p->value = arena_strdup(&t->arena, buf);
				}
			}
		}
	}
	t->res_w = t->res_h = 0;
}

static void parse_file(struct load_ctx *c, const char *path)
{
	struct xml_doc *doc;
	const struct xml_node *root, *n;
	char dir[1024];

	if (c->depth > THEME_MAX_DEPTH) {
		LOGW("theme: include depth exceeded at %s", path);
		return;
	}
	for (int i = 0; i < c->depth; i++) {
		if (!strcmp(c->open[i], path)) {
			ui_log_once(path, "theme: %s includes itself, include skipped", path);
			return;
		}
	}
	if (++c->nfiles > THEME_MAX_FILES) {
		ui_log_once(path, "theme: more than %d included files, %s skipped", THEME_MAX_FILES, path);
		return;
	}
	c->open[c->depth] = path;
	doc = xml_load(path);
	if (!doc) {
		ui_log_once(path, "theme: cannot read %s", path);
		return;
	}
	root = doc->root;
	while (root && strcmp(root->name, "theme"))
		root = root->next;
	if (!root) {
		ui_log_once(path, "theme: %s has no <theme> element", path);
		xml_free(doc);
		return;
	}
	path_dirname(path, dir, sizeof(dir));
	{
		const char *fv = xml_child_text(root, "formatVersion");

		if (fv && !c->t->version)
			c->t->version = atoi(fv);
	}
	/* 1. variables (first definition wins) */
	for (n = root->child; n; n = n->next) {
		if (strcmp(n->name, "variables"))
			continue;
		for (const struct xml_node *v = n->child; v; v = v->next) {
			char val[1024];

			expand_vars(c->t, v->text, val, sizeof(val));
			var_set(c->t, v->name, val);
		}
	}
	/* 2. includes, in order */
	for (n = root->child; n; n = n->next) {
		char inc[2048], ip[2048];

		if (strcmp(n->name, "include"))
			continue;
		expand_vars(c->t, n->text, inc, sizeof(inc));
		resolve_path(dir, str_trim(inc), ip, sizeof(ip));
		c->depth++;
		parse_file(c, ip);
		c->depth--;
	}
	/* 3. resolution */
	{
		const char *res = xml_child_text(root, "resolution");
		float w, h;

		if (res && sscanf(res, "%f %f", &w, &h) == 2 && w > 0 && h > 0) {
			c->t->res_w = w;
			c->t->res_h = h;
		}
	}
	/* 4. views, then 5. features (all supported: carousel, z-index,
	 * video - we render video views with snapshots) */
	parse_views(c, root, dir);
	for (n = root->child; n; n = n->next)
		if (!strcmp(n->name, "feature"))
			parse_views(c, n, dir);
	/* the resolution tag is per theme: normalize what we have so far */
	apply_resolution(c->t);
	xml_free(doc);
}

static struct theme *theme_new(const struct theme_sysinfo *si)
{
	struct theme *t = xcalloc(1, sizeof(*t));

	arena_init(&t->arena, 16384);
	if (si) {
		var_set(t, "system.name", si->name ? si->name : "");
		var_set(t, "system.fullName", si->fullname ? si->fullname : "");
		var_set(t, "system.theme", si->theme ? si->theme : "");
	}
	return t;
}

struct theme *theme_load_file(const char *path, const struct theme_sysinfo *si)
{
	struct theme *t = theme_new(si);
	struct load_ctx c = { .t = t };

	if (path && *path) {
		strlcpy_(t->path, path, sizeof(t->path));
		parse_file(&c, path);
	}
	return t;
}

/*
 * Systems that many ES theme sets have no folder for (their system list
 * predates them), and the folder of the console they extend. Used only when
 * none of the system's own names has a folder: without it, such a system
 * would get the set's default theme or none at all (gbz35 has no
 * pcenginecd, neocd, wonderswancolor, pico folders and no default theme).
 */
static const char *const g_related[][4] = {
	{ "pcenginecd", "pcengine", "tg16", NULL },
	{ "neocd", "neogeo", NULL },
	{ "wonderswancolor", "wonderswan", NULL },
	{ "ngpc", "ngp", NULL },
	{ "gbc", "gb", NULL },
	{ "pico", "megadrive", "genesis", NULL },
	{ "segacd", "megadrive", "genesis", NULL },
	{ "sega32x", "megadrive", "genesis", NULL },
	{ "neogeo", "fba", "arcade", NULL },
	{ "atari7800", "atari2600", NULL },
};

static const char *related_folder(const char *set_dir, const char *const *theme_names,
				  char *p, size_t n)
{
	for (int i = 0; theme_names && theme_names[i]; i++)
		for (size_t r = 0; r < ARRAY_SIZE(g_related); r++) {
			if (strcmp(g_related[r][0], theme_names[i]))
				continue;
			for (int k = 1; g_related[r][k]; k++) {
				snprintf(p, n, "%s/%s/theme.xml", set_dir, g_related[r][k]);
				if (file_exists(p))
					return g_related[r][k];
			}
		}
	return NULL;
}

struct theme *theme_load_system(const char *set_dir, const char *rom_dir,
				const char *const *theme_names,
				struct theme_sysinfo *si)
{
	char p[2048];
	const char *rel;

	if (rom_dir) {
		snprintf(p, sizeof(p), "%s/theme.xml", rom_dir);
		if (file_exists(p))
			return theme_load_file(p, si);
	}
	if (set_dir && *set_dir) {
		for (int i = 0; theme_names && theme_names[i]; i++) {
			snprintf(p, sizeof(p), "%s/%s/theme.xml", set_dir, theme_names[i]);
			if (file_exists(p)) {
				si->theme = theme_names[i];
				return theme_load_file(p, si);
			}
		}
		if ((rel = related_folder(set_dir, theme_names, p, sizeof(p)))) {
			si->theme = rel;
			return theme_load_file(p, si);
		}
		snprintf(p, sizeof(p), "%s/theme.xml", set_dir);
		if (file_exists(p))
			return theme_load_file(p, si);
	}
	return theme_load_file(NULL, si);
}

void theme_free(struct theme *t)
{
	if (!t)
		return;
	for (int i = 0; i < t->nviews; i++)
		free(t->views[i].elems);
	free(t->views);
	free(t->vars);
	arena_free(&t->arena);
	free(t);
}

bool theme_is_set_dir(const char *dir)
{
	char p[2048];
	static const char *const probes[] = { "nes", "snes", "megadrive", "gba", "psx", "arcade" };

	snprintf(p, sizeof(p), "%s/theme.xml", dir);
	if (file_exists(p))
		return true;
	for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
		snprintf(p, sizeof(p), "%s/%s/theme.xml", dir, probes[i]);
		if (file_exists(p))
			return true;
	}
	return false;
}

/* -------------------------------------------------------------- queries */
const struct theme_view *theme_view(const struct theme *t, const char *view)
{
	if (!t)
		return NULL;
	for (int i = 0; i < t->nviews; i++)
		if (!strcmp(t->views[i].name, view))
			return &t->views[i];
	return NULL;
}

const struct theme_elem *theme_elem(const struct theme *t, const char *view,
				    const char *name, const char *type)
{
	const struct theme_view *v = theme_view(t, view);

	if (!v)
		return NULL;
	for (int i = 0; i < v->n; i++) {
		const struct theme_elem *e = v->elems[i];

		if (!strcmp(e->name, name) && (!type || !strcmp(e->type, type)))
			return e;
	}
	return NULL;
}

const char *theme_prop(const struct theme_elem *e, const char *prop)
{
	if (!e)
		return NULL;
	for (int i = 0; i < e->nprops; i++)
		if (!strcmp(e->props[i].name, prop))
			return e->props[i].value;
	return NULL;
}

bool theme_has(const struct theme_elem *e, const char *prop)
{
	return theme_prop(e, prop) != NULL;
}

bool theme_get_pair(const struct theme_elem *e, const char *prop, float *x, float *y)
{
	const char *v = theme_prop(e, prop);
	float a, b;

	if (!v || sscanf(v, "%f %f", &a, &b) != 2)
		return false;
	*x = a;
	*y = b;
	return true;
}

bool theme_get_float(const struct theme_elem *e, const char *prop, float *out)
{
	const char *v = theme_prop(e, prop);
	float a;

	if (!v || sscanf(v, "%f", &a) != 1)
		return false;
	*out = a;
	return true;
}

bool theme_get_color(const struct theme_elem *e, const char *prop, gfx_color *c)
{
	const char *v = theme_prop(e, prop);

	if (!v)
		return false;
	if (!gfx_parse_color(v, c)) {
		char key[160];

		snprintf(key, sizeof(key), "theme-color:%s", v);
		ui_log_once(key, "theme: bad color \"%s\" for %s.%s", v, e->name, prop);
		return false;
	}
	return true;
}

bool theme_get_bool(const struct theme_elem *e, const char *prop, bool *b)
{
	const char *v = theme_prop(e, prop);

	if (!v || !*v)
		return false;
	*b = parse_bool(v, *b);
	return true;
}

const char *theme_get_str(const struct theme_elem *e, const char *prop)
{
	return theme_prop(e, prop);
}
