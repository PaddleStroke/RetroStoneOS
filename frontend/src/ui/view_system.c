/*
 * view_system.c - the ES "system" view: the carousel of systems, the
 * system info bar, and each system's extras (backgrounds...), which slide
 * with the carousel.
 *
 * Speed: each system's extras below the carousel are composited once into
 * a full-screen "backdrop" (kept in memory, and on disk as an .rpx keyed
 * by the theme properties and file mtimes), so a frame is one or two
 * opaque copies, a translucent band, a few logo blits and the texts.
 * Logos are pre-scaled at their normal and selected sizes.
 *
 * Who builds what (docs/ui-design.md §4.2): the first frame at boot builds
 * what it shows on the UI thread (nothing else is up yet). After it, a
 * system's assets - its theme, extras, logos and backdrop - are built by the
 * asset worker (prefetch.c) and moved into its item here: the window of
 * ±KEEP_BACKDROPS systems around the cursor (the direction of travel
 * first), then, once the menu is up and every list is in, every other
 * system (theme, logos, and its backdrop's cache file). A frame never
 * builds anything: what is not ready yet is drawn as a placeholder (a flat
 * colour for a backdrop, nothing for a logo) and fades in when it arrives.
 * Without a worker (it could not start) the old synchronous path runs.
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../i18n/i18n.h"
#include "ui_internal.h"


#define ANIM_MS 220
#define FADE_MS 120        /* a backdrop that arrives while shown */
#define KEEP_BACKDROPS 3   /* systems kept on each side of the cursor */
#define PLACEHOLDER 0xff101010u

enum { CT_HORIZONTAL = 0, CT_VERTICAL };

struct sv_job;

struct sv_item {
	struct sysent *se;
	char name[32];               /* se->name (se moves when the carousel is rebuilt) */
	struct elem *extras;
	int nextras;
	int first_live;              /* extras [first_live..] are drawn live */
	bool has_logo;
	struct elem logo;            /* image path/color from the theme */
	struct elem logotext;        /* text style */
	struct gfx_image *img[2];    /* [0] normal, [1] selected */
	bool img_owned[2];
	struct gfx_image *backdrop;
	gfx_color bd_color;          /* its average colour: the placeholder */
	bool built, logos_built;
	/* the asset worker: a job in flight (valid while job_gen is the
	 * prefetch generation), the backdrop's cache file known to exist, a
	 * backdrop fading in since fade_t0 */
	struct sv_job *job;
	unsigned job_gen;
	bool warm;
	int64_t fade_t0;
};

struct sysview {
	struct screen base;
	struct sv_item *items;
	int n;
	bool configured;
	unsigned items_gen;          /* bumped when items are rebuilt */
	unsigned look_gen;           /* ui->look_gen the items were built for */
	/* carousel */
	int type;
	float pos[2], size[2], origin[2];
	gfx_color color, color_end;
	bool grad_h;
	float logo_size[2], logo_scale;
	int logo_align;              /* -1 top/left, 0 center, 1 bottom/right */
	int max_logos;
	float z;
	/* system info */
	struct elem info;
	int info_for;                /* system index the info text was built for */
	float info_z;
	struct help_style help;
	int help_for;                /* system index help was styled for (-1 none) */
	unsigned i18n_gen;           /* language the texts were built in */
	/* animation (cam in item units, not wrapped) */
	float cam, cam_from;
	int target;
	int64_t t0;
	bool animating;
	int dir;                     /* last move: -1 / +1 */
	bool moved;                  /* the user moved it since it was built */
	bool fading;
};

/* Everything a system's assets are built from: on the UI thread from the
 * UI, on the worker from a job's copies (ui is then a stand-in with only
 * w, h, now and theme_name set: all that the element code reads). */
struct sv_ctx {
	struct ui *ui;
	const struct theme *theme;
	const char *theme_dir;
	const char *sys_name;
	const char *fullname;
	float z;                     /* carousel zIndex */
	float logo_size[2], logo_scale;
};

/* TRANSLATORS: help bar labels (uppercase, short: ~10 characters) */
static const struct help_prompt prompts_h[] = {
	{ "leftright", N_("choose") }, { "a", N_("select") }, { "start", N_("menu") },
};
/* TRANSLATORS: help bar labels (uppercase, short: ~10 characters) */
static const struct help_prompt prompts_v[] = {
	{ "updown", N_("choose") }, { "a", N_("select") }, { "start", N_("menu") },
};

static struct sysview *SV(struct screen *s)
{
	return (struct sysview *)s;
}

static int wrap(int i, int n)
{
	return n ? ((i % n) + n) % n : 0;
}

/* Distance of i from center on the ring. */
static int ring_dist(int i, int center, int n)
{
	int d = abs(i - center);

	return MIN(d, n - d);
}

/* ----------------------------------------------------------- config */
static void configure(struct ui *ui, struct sysview *v)
{
	/* once per carousel build: the first system's theme gives the
	 * carousel's geometry (parsed here if the worker has not yet) */
	const struct theme *t = v->n ? ui_sys_theme(ui, v->items[0].se) : NULL;
	const struct theme_elem *te;
	float x, y;
	const char *s;

	/* ES defaults */
	v->type = CT_HORIZONTAL;
	v->size[0] = 1.0f;
	v->size[1] = 0.2325f;
	v->pos[0] = 0;
	v->pos[1] = 0.5f * (1.0f - v->size[1]);
	v->origin[0] = v->origin[1] = 0;
	v->color = v->color_end = 0xd8ffffffu;
	v->grad_h = true;
	v->logo_size[0] = 0.25f;
	v->logo_size[1] = 0.155f;
	v->logo_scale = 1.2f;
	v->logo_align = 0;
	v->max_logos = 3;
	v->z = 40;
	te = theme_elem(t, "system", "systemcarousel", "carousel");
	if (te) {
		if ((s = theme_get_str(te, "type"))) {
			if (!strncmp(s, "vertical", 8))
				v->type = CT_VERTICAL;
			if (strstr(s, "wheel"))
				ui_log_once("wheel", "theme: carousel \"%s\" drawn as a straight carousel", s);
		}
		if (theme_get_pair(te, "size", &x, &y)) {
			v->size[0] = x;
			v->size[1] = y;
		}
		if (theme_get_pair(te, "pos", &x, &y)) {
			v->pos[0] = x;
			v->pos[1] = y;
		}
		if (theme_get_pair(te, "origin", &x, &y)) {
			v->origin[0] = x;
			v->origin[1] = y;
		}
		if (theme_get_color(te, "color", &v->color))
			v->color_end = v->color;
		theme_get_color(te, "colorEnd", &v->color_end);
		if ((s = theme_get_str(te, "gradientType")))
			v->grad_h = strcmp(s, "vertical") != 0;
		if (theme_get_pair(te, "logoSize", &x, &y)) {
			v->logo_size[0] = x;
			v->logo_size[1] = y;
		}
		if (theme_get_float(te, "logoScale", &x) && x > 0)
			v->logo_scale = x;
		if ((s = theme_get_str(te, "logoAlignment")))
			v->logo_align = (!strcmp(s, "top") || !strcmp(s, "left")) ? -1 :
					(!strcmp(s, "bottom") || !strcmp(s, "right")) ? 1 : 0;
		if (theme_get_float(te, "maxLogoCount", &x) && x >= 1)
			v->max_logos = (int)x;
		theme_get_float(te, "zIndex", &v->z);
	}
	/* system info bar */
	elem_init(&v->info, EK_TEXT, "systemInfo");
	v->info.font_size = 0.035f;
	v->info.pos[0] = 0;
	v->info.pos[1] = v->pos[1] + v->size[1] - v->origin[1] * v->size[1];
	v->info.size[0] = 1.0f;
	v->info.size[1] = 0.035f * 0.75f * 2.2f; /* letter height * 2.2 */
	v->info.bg = 0xd8ddddddu;
	v->info.fg = 0xff000000u;
	v->info.align = AL_CENTER;
	v->info.upper = true;
	v->info.z = 50;
	elem_apply(&v->info, theme_elem(t, "system", "systemInfo", "text"), TF_ALL & ~TF_TEXT);
	v->info_z = v->info.z;
	v->info_for = -1;
	v->help_for = -1;
	v->configured = true;
}

/* The context of an item on the UI thread (parses its theme if needed). */
static void ctx_ui(struct ui *ui, struct sysview *v, struct sv_item *it, struct sv_ctx *c)
{
	c->ui = ui;
	c->theme = ui_sys_theme(ui, it->se);
	c->theme_dir = ui->theme_dir;
	c->sys_name = it->se->name;
	c->fullname = it->se->fullname;
	c->z = v->z;
	c->logo_size[0] = v->logo_size[0];
	c->logo_size[1] = v->logo_size[1];
	c->logo_scale = v->logo_scale;
}

/* ------------------------------------------------------------ backdrops */
static int cmp_z(const void *a, const void *b)
{
	const struct elem *x = a, *y = b;

	if (x->z != y->z)
		return x->z < y->z ? -1 : 1;
	return x->order - y->order;
}

static void build_extras(const struct sv_ctx *c, struct sv_item *it)
{
	const struct theme_view *tv = theme_view(c->theme, "system");
	const struct theme_elem *te;
	struct ui_cost_scope cs;

	ui_cost_begin(&cs, UI_COST_THEME);
	it->nextras = 0;
	if (tv) {
		it->extras = xcalloc((size_t)tv->n + 1, sizeof(struct elem));
		for (int i = 0; i < tv->n; i++) {
			if (!tv->elems[i]->extra)
				continue;
			if (elem_from_theme(&it->extras[it->nextras], tv->elems[i])) {
				/* our themes' texts (taglines) are translated */
				elem_translate_theme_text(c->ui, &it->extras[it->nextras]);
				it->nextras++;
			}
		}
		qsort(it->extras, (size_t)it->nextras, sizeof(struct elem), cmp_z);
	}
	it->first_live = it->nextras;
	for (int i = 0; i < it->nextras; i++)
		if (it->extras[i].z >= c->z) {
			it->first_live = i;
			break;
		}
	/* logo */
	elem_init(&it->logo, EK_IMAGE, "logo");
	te = theme_elem(c->theme, "system", "logo", "image");
	elem_apply(&it->logo, te, TF_PATH | TF_COLOR);
	it->has_logo = (it->logo.path[0] && file_exists(it->logo.path)) ||
		       (it->logo.def_path[0] && file_exists(it->logo.def_path));
	elem_init(&it->logotext, EK_TEXT, "logoText");
	it->logotext.font_size = 0.085f;
	it->logotext.align = AL_CENTER;
	it->logotext.fg = 0xff000000u;
	elem_set_text(&it->logotext, c->fullname);
	elem_apply(&it->logotext, theme_elem(c->theme, "system", "logoText", "text"),
		   TF_FONT | TF_COLOR | TF_STYLE | TF_TEXT);
	elem_translate_theme_text(c->ui, &it->logotext);
	it->built = true;
	ui_cost_end(&cs);
}

/* Cache key of a backdrop: every property of the extras drawn into it,
 * the mtimes of the files they use, and the size. */
static uint64_t backdrop_key(const struct sv_ctx *c)
{
	const struct theme_view *tv = theme_view(c->theme, "system");
	uint64_t h = hash64_str(c->theme_dir, (uint64_t)c->ui->w * 7919 + (uint64_t)c->ui->h);
	struct ui_cost_scope cs;

	ui_cost_begin(&cs, UI_COST_THEME);
	h = hash64_str(c->sys_name, h ^ 0xb4c0);
	/* texts drawn into it: theme texts translated, ${system.fullName} (the
	 * English key stays as it was) */
	if (strcmp(i18n_language(), "en"))
		h = hash64_str(i18n_language(), h ^ 0x1a7);
	for (int i = 0; tv && i < tv->n; i++) {
		const struct theme_elem *te = tv->elems[i];

		if (!te->extra)
			continue;
		h = hash64_str(te->type, h);
		h = hash64_str(te->name, h);
		for (int k = 0; k < te->nprops; k++) {
			struct stat st;

			h = hash64_str(te->props[k].name, h);
			h = hash64_str(te->props[k].value, h);
			if (te->props[k].value[0] == '/' && stat(te->props[k].value, &st) == 0)
				h = hash64(&st.st_mtim, sizeof(st.st_mtim), h);
		}
	}
	ui_cost_end(&cs);
	return h ^ 3; /* bump to invalidate old backdrops */
}

static void backdrop_path(uint64_t key, char *out, size_t n)
{
	snprintf(out, n, "%s/bd-%016llx.rpx", img_cache_dir(), (unsigned long long)key);
}

/* Drops a composited layer's images: they will not be wanted again (the
 * backdrop is cached), so they are the first ones the image cache evicts. */
static void release_cold(struct elem *e)
{
	if (!e->icon_img) {
		img_put_cold(e->img);
		e->img = NULL;
	}
	if (!e->icon_img2) {
		img_put_cold(e->img2);
		e->img2 = NULL;
	}
	elem_release(e);
}

/* Reads the backdrop from its cache file, else composites it (and writes
 * the file). warm: only make sure the file exists and its pages are read
 * (the worker's pass over every system); nothing is kept. */
static void build_backdrop(const struct sv_ctx *c, struct sv_item *it, bool warm)
{
	char path[1200] = "";
	uint64_t key;
	struct gfx_surface s;
	struct gfx_image *bd;
	int64_t t0 = ui_now_us();
	struct ui_cost_scope cs;
	int W = c->ui->w, H = c->ui->h;

	if (!it->built)
		build_extras(c, it);
	key = backdrop_key(c);
	if (img_cache_dir()[0]) {
		backdrop_path(key, path, sizeof(path));
		if (warm) {
			/* read it into the page cache, no decoding (the window loads
			 * it again, from memory, when the cursor comes near) */
			int fd = open(path, O_RDONLY | O_CLOEXEC);

			if (fd >= 0) {
				char buf[16384];

				ui_cost_begin(&cs, UI_COST_CACHE);
				while (read(fd, buf, sizeof(buf)) > 0)
					;
				close(fd);
				ui_cost_end(&cs);
				return;
			}
		} else {
			bd = rpx_load(path, key);
			if (bd && (bd->w != W || bd->h != H)) {
				gfx_image_free(bd);
				bd = NULL;
			}
			if (bd) {
				it->backdrop = bd;
				img_note_backdrop(true, ui_now_us() - t0);
				return;
			}
		}
	}
	ui_cost_begin(&cs, UI_COST_BACKDROP);
	bd = gfx_image_new(W, H);
	gfx_surface_from_image(&s, bd);
	gfx_fill(&s, 0, 0, W, H, 0xff000000u);
	for (int i = 0; i < it->first_live; i++) {
		elem_draw(c->ui, &s, &it->extras[i], 0, 0);
		release_cold(&it->extras[i]); /* the pixels live in the backdrop now */
	}
	bd->flags |= GFX_IMG_OPAQUE;
	ui_cost_end(&cs);
	if (path[0])
		rpx_save(path, bd, key);
	img_note_backdrop(false, ui_now_us() - t0);
	if (warm)
		gfx_image_free(bd);
	else
		it->backdrop = bd;
}

/* The average colour of a backdrop (a sparse sample): what its place shows
 * before it arrives, and what its neighbours' placeholders use. */
static gfx_color average_color(const struct gfx_image *img)
{
	uint32_t r = 0, g = 0, b = 0, n = 0;

	if (!img || img->w <= 0 || img->h <= 0)
		return PLACEHOLDER;
	for (int y = img->h / 16; y < img->h; y += img->h / 8 + 1)
		for (int x = img->w / 32; x < img->w; x += img->w / 16 + 1) {
			uint32_t p = img->px[(size_t)y * img->stride + x];

			r += (p >> 16) & 0xff;
			g += (p >> 8) & 0xff;
			b += p & 0xff;
			n++;
		}
	if (!n)
		return PLACEHOLDER;
	return 0xff000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
}

static void drop_far_backdrops(struct sysview *v, int center)
{
	for (int i = 0; i < v->n; i++)
		if (ring_dist(i, center, v->n) > KEEP_BACKDROPS && v->items[i].backdrop) {
			gfx_image_free(v->items[i].backdrop);
			v->items[i].backdrop = NULL;
			v->items[i].fade_t0 = 0;
		}
}

/* ---------------------------------------------------------------- logos */
static void free_logos(struct sv_item *it)
{
	for (int k = 0; k < 2; k++) {
		if (it->img_owned[k])
			gfx_image_free(it->img[k]);
		else
			img_put(it->img[k]);
		it->img[k] = NULL;
		it->img_owned[k] = false;
	}
	it->logos_built = false;
}

static void build_logos(const struct sv_ctx *c, struct sv_item *it)
{
	struct ui *ui = c->ui;
	float W = (float)ui->w, H = (float)ui->h;
	float bw = c->logo_size[0] * c->logo_scale * W, bh = c->logo_size[1] * c->logo_scale * H;
	struct ui_cost_scope cs;

	if (!it->built)
		build_extras(c, it);
	free_logos(it);
	if (it->has_logo) {
		const char *p = it->logo.path[0] && file_exists(it->logo.path) ? it->logo.path :
				it->logo.def_path;
		int nw, nh;

		if (img_info(p, &nw, &nh)) {
			float s = MIN(bw / (float)nw, bh / (float)nh);
			int w1 = MAX(1, (int)lroundf((float)nw * s)), h1 = MAX(1, (int)lroundf((float)nh * s));
			int w0 = MAX(1, (int)lroundf((float)w1 / c->logo_scale));
			int h0 = MAX(1, (int)lroundf((float)h1 / c->logo_scale));

			it->img[1] = img_get(p, w1, h1, it->logo.color);
			it->img[0] = img_get(p, w0, h0, it->logo.color);
		}
	}
	if (!it->img[1]) {
		/* text logo: rendered in the selected box, scaled down for the
		 * normal size */
		struct elem *t = &it->logotext;
		struct font *f = font_get(t->font_path, ui_font_px(ui, t->font_size));
		char up[256];
		const char *txt = t->text ? t->text : c->fullname;
		struct gfx_image *img;
		struct gfx_surface s;
		struct text_line lines[4];
		int lh = (int)lroundf((float)font_height(f) * t->line_spacing);
		int n, bwi = MAX(1, (int)bw), bhi = MAX(1, (int)bh), y0;

		ui_cost_begin(&cs, UI_COST_TEXT);
		if (t->upper) {
			utf8_upper(txt, up, sizeof(up));
			txt = up;
		}
		/* a word wider than the box: shrink the font rather than cut it
		 * (CJK needs no space to wrap: each character counts alone) */
		{
			int widest = 0, px = ui_font_px(ui, t->font_size);
			const char *w0 = txt, *cp0 = txt;

			for (;;) {
				const char *q = cp0;
				unsigned cp = *cp0 ? utf8_next(&q) : 0;
				bool cjk = cp >= 0x2e80;

				if (!cp || cp == ' ' || cjk) {
					widest = MAX(widest, font_text_width(f, w0, (int)(cp0 - w0)));
					if (cjk)
						widest = MAX(widest, font_text_width(f, cp0, (int)(q - cp0)));
					if (!cp)
						break;
					w0 = q;
				}
				cp0 = q;
			}
			if (widest > bwi) {
				px = MAX(px / 2, px * bwi / widest);
				f = font_get(t->font_path, px);
				lh = (int)lroundf((float)font_height(f) * t->line_spacing);
			}
			/* too many lines for the box: shrink too */
			for (int k = 0; k < 8; k++) {
				n = font_wrap(f, txt, bwi, lines, 4);
				if (n * lh <= bhi || px <= ui_font_px(ui, t->font_size) / 2)
					break;
				px = px * 9 / 10;
				f = font_get(t->font_path, px);
				lh = (int)lroundf((float)font_height(f) * t->line_spacing);
			}
		}
		n = font_wrap(f, txt, bwi, lines, 4);
		img = gfx_image_new(bwi, bhi);
		gfx_surface_from_image(&s, img);
		y0 = (bhi - n * lh) / 2;
		for (int i = 0; i < n; i++) {
			int x = (bwi - lines[i].width) / 2;

			font_draw(&s, f, x, font_baseline_in_box(f, y0 + i * lh, lh),
				  txt + lines[i].start, lines[i].len, t->fg);
		}
		gfx_image_update_flags(img);
		it->img[1] = img;
		it->img_owned[1] = true;
		it->img[0] = gfx_image_scale(img, MAX(1, (int)lroundf((float)bwi / c->logo_scale)),
					     MAX(1, (int)lroundf((float)bhi / c->logo_scale)));
		it->img_owned[0] = true;
		ui_cost_end(&cs);
	}
	it->logos_built = true;
}

/* The live extras (drawn every frame over the carousel), laid out ahead:
 * their images decoded, their texts rendered. */
static void layout_live(const struct sv_ctx *c, struct sv_item *it)
{
	for (int i = it->first_live; i < it->nextras; i++)
		if (it->extras[i].visible && !it->extras[i].laid_out)
			elem_layout(c->ui, &it->extras[i]);
}

static void free_item_assets(struct sv_item *it)
{
	free_logos(it);
	gfx_image_free(it->backdrop);
	it->backdrop = NULL;
	for (int k = 0; k < it->nextras; k++)
		elem_free(&it->extras[k]);
	free(it->extras);
	it->extras = NULL;
	it->nextras = it->first_live = 0;
	if (it->built) {
		elem_free(&it->logo);
		elem_free(&it->logotext);
	}
	it->built = false;
}

/* --------------------------------------------------------- the worker */
struct sv_job {
	struct pf_job base;
	/* identity: where the result goes */
	struct sysview *v;
	unsigned items_gen;
	int idx;
	struct sysent *se;
	/* inputs (copies: the worker never reads the UI) */
	int w, h;
	int64_t now;
	char theme_name[64];
	char theme_dir[1024];
	char name[32], fullname[96], rom_dir[512];
	const char *alias[6];
	bool is_collection;
	float z, logo_size[2], logo_scale;
	const struct theme *theme;   /* the system's, read-only; NULL: parse one */
	bool want_item;              /* extras, logos, live extras laid out */
	bool want_backdrop;          /* the backdrop, in memory */
	bool warm;                   /* only the backdrop's cache file */
	bool vertical;               /* the carousel's help prompts */
	/* outputs */
	struct theme *own_theme;     /* parsed here */
	char own_theme_name[32];
	struct sv_item out;
	struct ui_icon_ahead icons[HELP_ICONS_AHEAD];   /* its help bar's */
	int nicons;
};

static void job_run(struct pf_job *pj)
{
	struct sv_job *j = (struct sv_job *)pj;
	struct ui *stand_in = xcalloc(1, sizeof(*stand_in));
	struct sv_ctx c;

	stand_in->w = j->w;
	stand_in->h = j->h;
	stand_in->now = j->now;
	strlcpy_(stand_in->theme_name, j->theme_name, sizeof(stand_in->theme_name));
	c.ui = stand_in;
	c.theme = j->theme;
	c.theme_dir = j->theme_dir;
	c.sys_name = j->name;
	c.fullname = j->fullname;
	c.z = j->z;
	c.logo_size[0] = j->logo_size[0];
	c.logo_size[1] = j->logo_size[1];
	c.logo_scale = j->logo_scale;
	if (!c.theme) {
		struct theme_sysinfo si = { j->name, j->fullname, j->alias[0] };
		struct ui_cost_scope cs;

		ui_cost_begin(&cs, UI_COST_THEME);
		j->own_theme = theme_load_system(j->theme_dir, j->is_collection ? NULL : j->rom_dir,
						 j->alias, &si);
		strlcpy_(j->own_theme_name, si.theme ? si.theme : "", sizeof(j->own_theme_name));
		ui_cost_end(&cs);
		c.theme = j->own_theme;
	}
	if (j->want_item && !pf_cancelled(pj)) {
		struct help_style hs;

		build_extras(&c, &j->out);
		build_logos(&c, &j->out);
		layout_live(&c, &j->out);
		/* its help bar's icons, in its theme's colours */
		help_style_default(&hs);
		help_style_apply(&hs, theme_elem(c.theme, "system", "help", "helpsystem"));
		j->nicons = help_icons_ahead(stand_in, &hs, j->vertical ? prompts_v : prompts_h, 3, j->icons,
					     HELP_ICONS_AHEAD);
	}
	if ((j->want_backdrop || j->warm) && !pf_cancelled(pj)) {
		build_backdrop(&c, &j->out, !j->want_backdrop);
		if (j->out.backdrop) {
			/* its pages now, not in the UI thread's first blit */
			img_prefault(j->out.backdrop);
			j->out.bd_color = average_color(j->out.backdrop);
		}
		/* extras built for the backdrop only: the live ones too */
		if (j->out.built && !pf_cancelled(pj))
			layout_live(&c, &j->out);
	}
	free(stand_in);
}

static struct sysview *job_target(struct ui *ui, struct sv_job *j)
{
	for (int i = 0; i < ui->nstack; i++) {
		struct screen *s = ui->stack[i];

		if (s->kind == SCR_SYSVIEW && SV(s) == j->v && j->v->items_gen == j->items_gen &&
		    j->idx < j->v->n && j->v->items[j->idx].se == j->se)
			return j->v;
	}
	return NULL;
}

static void job_apply(struct ui *ui, struct pf_job *pj)
{
	struct sv_job *j = (struct sv_job *)pj;
	struct sysview *v = job_target(ui, j);
	struct sv_item *it;
	bool visible_change = false;
	int center;

	if (!v)
		return;
	it = &v->items[j->idx];
	if (it->job == j)
		it->job = NULL;
	for (int k = 0; k < j->nicons; k++) {
		ui_icon_adopt(ui, &j->icons[k]);
		j->icons[k].img = NULL;
	}
	center = wrap(v->target, v->n);
	if (j->own_theme && !j->se->theme) {
		j->se->theme = j->own_theme;
		strlcpy_(j->se->theme_name, j->own_theme_name, sizeof(j->se->theme_name));
		j->own_theme = NULL;
	}
	if (j->out.built && !it->built) {
		it->extras = j->out.extras;
		it->nextras = j->out.nextras;
		it->first_live = j->out.first_live;
		it->has_logo = j->out.has_logo;
		it->logo = j->out.logo;
		it->logotext = j->out.logotext;
		it->built = true;
		j->out.extras = NULL;
		j->out.nextras = 0;
		j->out.built = false;
		visible_change = true;
	}
	if (j->out.logos_built && !it->logos_built) {
		for (int k = 0; k < 2; k++) {
			it->img[k] = j->out.img[k];
			it->img_owned[k] = j->out.img_owned[k];
			j->out.img[k] = NULL;
			j->out.img_owned[k] = false;
		}
		it->logos_built = true;
		j->out.logos_built = false;
		visible_change |= ring_dist(j->idx, center, v->n) <= v->max_logos / 2 + 1;
	}
	if (j->warm || j->want_backdrop)
		it->warm = true;
	if (j->out.backdrop && !it->backdrop && ring_dist(j->idx, center, v->n) <= KEEP_BACKDROPS) {
		float d = fabsf(v->cam - (float)j->idx);

		it->backdrop = j->out.backdrop;
		it->bd_color = j->out.bd_color;
		j->out.backdrop = NULL;
		/* on screen now (the cursor, or sliding in): fade it in (not
		 * under a menu: the view is a still snapshot there) */
		d = MIN(d, fabsf((float)v->n - d));
		if (d < 1.0f) {
			visible_change = true;
			if (ui_top(ui) == &v->base) {
				it->fade_t0 = ui->now;
				v->fading = true;
			}
		}
	}
	if (visible_change) {
		ui->dirty = true;
		if (ui_top(ui) != &v->base)
			ui_invalidate_snapshot(ui);   /* it is under a menu */
	}
}

static void job_drop(struct pf_job *pj)
{
	struct sv_job *j = (struct sv_job *)pj;

	free_item_assets(&j->out);
	theme_free(j->own_theme);
	for (int k = 0; k < j->nicons; k++)
		gfx_image_free(j->icons[k].img);
	free(j);
}

static bool in_flight(struct ui *ui, struct sv_item *it)
{
	return it->job && it->job_gen == prefetch_gen(ui);
}

enum { NEED_ITEM = 1, NEED_BD = 2, NEED_WARM = 4 };

/* Queues what item idx lacks of what = NEED_* (a backdrop request brings
 * the item too); prio: PF_URGENT (on screen), PF_NEAR, PF_BG. */
static void request(struct ui *ui, struct sysview *v, int idx, int what, int prio)
{
	struct sv_item *it = &v->items[idx];
	struct sysent *se = it->se;
	struct sv_job *j;
	bool want_item = (what & (NEED_ITEM | NEED_BD)) && (!it->built || !it->logos_built);
	bool want_bd = (what & NEED_BD) && !it->backdrop;
	bool warm = (what & NEED_WARM) && !it->warm && !it->backdrop;

	if (!want_item && !want_bd && !warm)
		return;
	if (in_flight(ui, it)) {
		struct sv_job *o = it->job;

		/* already asked: what it brings is enough (maybe more urgently
		 * now), else another job for the rest */
		if (o->want_item)
			want_item = false;
		if (o->want_backdrop || o->warm)
			warm = false;
		if (o->want_backdrop)
			want_bd = false;
		if (!want_item && !want_bd && !warm) {
			if (prio < o->base.prio)
				prefetch_raise(ui, &o->base, prio);
			return;
		}
	}
	j = xcalloc(1, sizeof(*j));
	j->base.run = job_run;
	j->base.apply = job_apply;
	j->base.drop = job_drop;
	j->base.prio = prio;
	j->v = v;
	j->items_gen = v->items_gen;
	j->idx = idx;
	j->se = se;
	j->w = ui->w;
	j->h = ui->h;
	j->now = ui->now;
	strlcpy_(j->theme_name, ui->theme_name, sizeof(j->theme_name));
	strlcpy_(j->theme_dir, ui->theme_dir, sizeof(j->theme_dir));
	strlcpy_(j->name, se->name, sizeof(j->name));
	strlcpy_(j->fullname, se->fullname, sizeof(j->fullname));
	strlcpy_(j->rom_dir, se->rom_dir, sizeof(j->rom_dir));
	memcpy(j->alias, se->theme_alias, sizeof(j->alias));
	j->is_collection = se->is_collection;
	j->z = v->z;
	j->logo_size[0] = v->logo_size[0];
	j->logo_size[1] = v->logo_size[1];
	j->logo_scale = v->logo_scale;
	j->theme = se->theme;
	j->want_item = want_item;
	j->want_backdrop = want_bd;
	j->warm = !want_bd && warm;
	j->vertical = v->type == CT_VERTICAL;
	it->job = j;
	it->job_gen = prefetch_gen(ui);
	prefetch_submit(ui, &j->base);   /* (no worker: done now if urgent) */
}

/*
 * What the carousel wants built, in order: on screen now (urgent), the
 * window of ±KEEP_BACKDROPS around the cursor in the direction of travel
 * (near, once the user moved it; before that it is speculative), then,
 * once the menu is up and every list is in, every other system (logos,
 * theme, the backdrop's cache file). Called from ui_update() and before a
 * frame; cheap when everything is there.
 */
static void want(struct ui *ui, struct sysview *v)
{
	int center, dir;
	int base = (int)floorf(v->cam);
	float frac = v->cam - (float)base;

	if (!v->n || !v->configured || !prefetch_available(ui) || v->look_gen != ui->look_gen)
		return;
	center = wrap(v->target, v->n);
	dir = v->dir ? v->dir : 1;
	/* on screen: the backdrops of the two systems the camera is between,
	 * the logos of the visible slots */
	request(ui, v, wrap(base, v->n), NEED_BD, PF_URGENT);
	if (frac > 0.001f)
		request(ui, v, wrap(base + 1, v->n), NEED_BD, PF_URGENT);
	request(ui, v, center, NEED_BD, PF_URGENT);
	{
		int span = MIN(v->max_logos, v->n) / 2 + 1;   /* as render_carousel */

		for (int i = base - span; i <= base + span; i++)
			request(ui, v, wrap(i, v->n), NEED_ITEM, PF_URGENT);
	}
	/* the window, the direction of travel first */
	for (int d = 1; d <= KEEP_BACKDROPS; d++) {
		int prio = v->moved ? PF_NEAR : PF_BG;

		request(ui, v, wrap(center + d * dir, v->n), NEED_BD, prio);
		request(ui, v, wrap(center - d * dir, v->n), NEED_BD, prio);
	}
	/* the game list of the system it rests on (A opens it) */
	if (!v->animating && ui_top(ui) == &v->base)
		glview_prefetch(ui, v->items[center].se, v->moved ? PF_NEAR : PF_BG);
	/* every other system, nearest first */
	if (!prefetch_background_allowed(ui))
		return;
	for (int d = KEEP_BACKDROPS + 1; d <= v->n / 2; d++) {
		request(ui, v, wrap(center + d * dir, v->n), NEED_ITEM | NEED_WARM, PF_BG);
		request(ui, v, wrap(center - d * dir, v->n), NEED_ITEM | NEED_WARM, PF_BG);
	}
}

void sysview_prefetch(struct ui *ui, struct screen *s)
{
	if (s && s->kind == SCR_SYSVIEW)
		want(ui, SV(s));
}

/* Synchronous path (the first frame at boot, or no worker). */
static bool sync_ok(struct ui *ui)
{
	return !prefetch_available(ui);
}

static void ensure_backdrop_sync(struct ui *ui, struct sysview *v, struct sv_item *it)
{
	struct sv_ctx c;

	ctx_ui(ui, v, it, &c);
	build_backdrop(&c, it, false);
	it->bd_color = average_color(it->backdrop);
	it->warm = true;
}

static void ensure_logos_sync(struct ui *ui, struct sysview *v, struct sv_item *it)
{
	struct sv_ctx c;

	ctx_ui(ui, v, it, &c);
	build_logos(&c, it);
}

/* ------------------------------------------------------------ rendering */
static float ease(float t)
{
	t = 1.0f - t;
	return 1.0f - t * t * t;
}

/* The placeholder of a backdrop that is not there yet: the colour of the
 * nearest one that is (so a flat field of the theme's tone), else dark. */
static gfx_color placeholder_color(struct sysview *v, int idx)
{
	for (int d = 1; d <= KEEP_BACKDROPS; d++) {
		struct sv_item *a = &v->items[wrap(idx - d, v->n)], *b = &v->items[wrap(idx + d, v->n)];

		if (a->backdrop)
			return a->bd_color;
		if (b->backdrop)
			return b->bd_color;
	}
	return PLACEHOLDER;
}

static void render_item_layers(struct ui *ui, struct sysview *v, struct gfx_surface *s,
			       int idx, float off, bool live)
{
	struct sv_item *it = &v->items[idx];
	int dx = 0, dy = 0;

	if (v->type == CT_HORIZONTAL)
		dx = (int)lroundf(off * (float)ui->w);
	else
		dy = (int)lroundf(off * (float)ui->h);
	if (!live) {
		if (!it->backdrop && sync_ok(ui))
			ensure_backdrop_sync(ui, v, it);
		if (!it->backdrop) {
			gfx_fill(s, dx, dy, ui->w, ui->h, placeholder_color(v, idx));
			return;
		}
		if (it->fade_t0) {
			int64_t t = ui->now - it->fade_t0;

			if (t < FADE_MS && !ui->in_snapshot) {
				gfx_fill(s, dx, dy, ui->w, ui->h, placeholder_color(v, idx));
				gfx_blit(s, it->backdrop, dx, dy, (int)CLAMP(t * 255 / FADE_MS, 1, 254));
				v->fading = true;
				return;
			}
			it->fade_t0 = 0;
		}
		gfx_blit(s, it->backdrop, dx, dy, 255);
	} else {
		if (!it->built)
			return;   /* (its job lays them out) */
		for (int i = it->first_live; i < it->nextras; i++)
			elem_draw(ui, s, &it->extras[i], dx, dy);
	}
}

static void render_carousel(struct ui *ui, struct sysview *v, struct gfx_surface *s)
{
	float W = (float)ui->w, H = (float)ui->h;
	float cw = v->size[0] * W, ch = v->size[1] * H;
	float cx = v->pos[0] * W - v->origin[0] * cw, cy = v->pos[1] * H - v->origin[1] * ch;
	float lw = v->logo_size[0] * W, lh = v->logo_size[1] * H;
	float spx = 0, spy = 0, xoff, yoff;
	int count = MIN(v->max_logos, v->n);
	int center = (int)floorf(v->cam);
	struct gfx_rect old;

	gfx_fill_gradient(s, (int)cx, (int)cy, (int)lroundf(cw), (int)lroundf(ch),
			  v->color, v->color_end, v->grad_h);
	if (v->type == CT_VERTICAL) {
		spy = (ch - lh * (float)v->max_logos) / (float)v->max_logos + lh;
		yoff = (ch - lh) / 2.0f - v->cam * spy;
		xoff = v->logo_align < 0 ? lw / 10.0f : v->logo_align > 0 ? cw - lw * 1.1f :
		       (cw - lw) / 2.0f;
	} else {
		spx = (cw - lw * (float)v->max_logos) / (float)v->max_logos + lw;
		xoff = (cw - lw) / 2.0f - v->cam * spx;
		yoff = v->logo_align < 0 ? lh / 10.0f : v->logo_align > 0 ? ch - lh * 1.1f :
		       (ch - lh) / 2.0f;
	}
	old = gfx_clip_push(s, (struct gfx_rect){ (int)cx, (int)cy, (int)lroundf(cw),
						  (int)lroundf(ch) });
	for (int i = center - count / 2 - 1; i <= center + count / 2 + 1; i++) {
		struct sv_item *it = &v->items[wrap(i, v->n)];
		float dist = fabsf((float)i - v->cam);
		float scale = 1.0f + (v->logo_scale - 1.0f) * (1.0f - dist);
		int alpha = (int)lroundf(128.0f + 127.0f * (1.0f - dist));
		/* logo centre (ES: origin 0.5 inside the logoSize box) */
		float ox = v->type == CT_VERTICAL && v->logo_align < 0 ? 0.0f :
			   v->type == CT_VERTICAL && v->logo_align > 0 ? 1.0f : 0.5f;
		float oy = v->type == CT_HORIZONTAL && v->logo_align < 0 ? 0.0f :
			   v->type == CT_HORIZONTAL && v->logo_align > 0 ? 1.0f : 0.5f;
		float px = cx + (float)i * spx + xoff + lw * ox;
		float py = cy + (float)i * spy + yoff + lh * oy;
		struct gfx_image *img;
		int w, h;

		if (count == 1 && i != center && i != center + 1)
			continue;
		scale = CLAMP(scale, 1.0f, v->logo_scale);
		alpha = MAX(alpha, 128);
		if (!it->logos_built && sync_ok(ui))
			ensure_logos_sync(ui, v, it);
		if (!it->logos_built || !it->img[1])
			continue;   /* not there yet (its job brings it) */
		if (scale >= v->logo_scale - 0.001f) {
			img = it->img[1];
			w = img->w;
			h = img->h;
		} else if (scale <= 1.001f) {
			img = it->img[0];
			w = img->w;
			h = img->h;
		} else {
			img = it->img[1];
			w = (int)lroundf((float)img->w * scale / v->logo_scale);
			h = (int)lroundf((float)img->h * scale / v->logo_scale);
		}
		{
			int x = (int)lroundf(px - ox * (float)w), y = (int)lroundf(py - oy * (float)h);

			if (w == img->w && h == img->h)
				gfx_blit(s, img, x, y, alpha);
			else
				gfx_blit_scaled(s, img, x, y, w, h, alpha);
		}
	}
	gfx_clip_pop(s, old);
}

static void render_info(struct ui *ui, struct sysview *v, struct gfx_surface *s)
{
	int cur = wrap((int)lroundf(v->cam), v->n);

	if (!v->n)
		return;
	if (v->info_for != cur) {
		struct sysent *se = v->items[cur].se;
		char buf[256], games[128], favs[96];
		/* before its list arrives: the counts of the carousel snapshot */
		int nfav = se->games ? 0 : se->nfav, n = se->games ? se->games->n : se->count;

		for (int i = 0; se->games && i < n; i++)
			nfav += se->games->games[i].favorite;
		/* TRANSLATORS: system carousel info bar (uppercase in most themes);
		 * the whole bar is ~50 characters wide at 640x480 */
		snprintf(games, sizeof(games), _n("%d game available", "%d games available", n), n);
		if (se->is_collection || !nfav) {
			elem_set_text(&v->info, games);
		} else {
			/* TRANSLATORS: system carousel info bar, after "N games available, " */
			snprintf(favs, sizeof(favs), _n("%d favorite", "%d favorites", nfav), nfav);
			/* TRANSLATORS: system carousel info bar: "%1$s" is "N games
			 * available", "%2$s" is "M favorites" ("12 games available, 3 favorites") */
			snprintf(buf, sizeof(buf), C_("system info", "%s, %s"), games, favs);
			elem_set_text(&v->info, buf);
		}
		v->info_for = cur;
	}
	elem_draw(ui, s, &v->info, 0, 0);
}

static void check_language(struct ui *ui, struct sysview *v);

/* The help bar styled by the current system's theme: that theme once it is
 * there (the worker brings it), the last style until then. */
static const struct help_style *help_for(struct ui *ui, struct sysview *v)
{
	int cur = wrap((int)lroundf(v->cam), v->n);
	struct sysent *se = v->items[cur].se;

	if (v->help_for != cur && (se->theme || sync_ok(ui) || v->help_for < 0)) {
		const struct theme *t = se->theme || sync_ok(ui) ? ui_sys_theme(ui, se) : NULL;

		help_style_default(&v->help);
		help_style_apply(&v->help, theme_elem(t, "system", "help", "helpsystem"));
		v->help_for = t ? cur : -1;
	}
	return &v->help;
}

static void sv_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct sysview *v = SV(scr);
	int base, next;
	float frac;

	check_language(ui, v);
	if (!v->configured)
		configure(ui, v);
	if (!v->n) {
		struct font *f = font_get(NULL, ui_font_px(ui, 0.045f));
		struct gfx_image *img;
		int w, h;

		gfx_fill(s, 0, 0, s->w, s->h, 0xff101010u);
		/* TRANSLATORS: the carousel with no game at all; keep the path
		 * /data/roms/<system>/ as it is. Wrapped on up to 3 lines. */
		img = render_text_image(f, _("No games found. Copy ROMs to /data/roms/<system>/"),
					s->w * 9 / 10, 3, AL_CENTER, 1.3f, 0xffe0e0e0u, &w, &h);
		gfx_blit(s, img, (s->w - w) / 2, (s->h - h) / 2, 255);
		gfx_image_free(img);
		return;
	}
	/* what this frame shows and does not have: to the worker, now */
	want(ui, v);
	v->fading = false;
	base = (int)floorf(v->cam);
	frac = v->cam - (float)base;
	next = base + 1;
	/* backdrops (below the carousel) */
	render_item_layers(ui, v, s, wrap(base, v->n), -frac, false);
	if (frac > 0.001f)
		render_item_layers(ui, v, s, wrap(next, v->n), 1.0f - frac, false);
	if (v->info_z < v->z) {
		render_info(ui, v, s);
		render_carousel(ui, v, s);
	} else {
		render_carousel(ui, v, s);
		render_info(ui, v, s);
	}
	/* live extras (zIndex above the carousel) */
	render_item_layers(ui, v, s, wrap(base, v->n), -frac, true);
	if (frac > 0.001f)
		render_item_layers(ui, v, s, wrap(next, v->n), 1.0f - frac, true);
	/* help, styled by the current system's theme */
	help_draw(ui, s, help_for(ui, v), v->type == CT_VERTICAL ? prompts_v : prompts_h, 3);
}

/* ------------------------------------------------------------ behaviour */
static void move(struct ui *ui, struct sysview *v, int d)
{
	if (!v->n)
		return;
	v->cam_from = v->cam;
	v->target += d;
	v->t0 = ui->now;
	v->animating = true;
	v->dir = d < 0 ? -1 : 1;
	v->moved = true;
	ui->sys_cursor = wrap(v->target, v->n);
	ui->dirty = true;
	/* the new window (the direction of travel first), before the frame */
	want(ui, v);
}

static void sv_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct sysview *v = SV(scr);
	bool vert = v->type == CT_VERTICAL;

	if (t == IN_NAV_RELEASE)
		return;
	if ((!vert && b == IN_LEFT) || (vert && b == IN_UP)) {
		move(ui, v, -1);
	} else if ((!vert && b == IN_RIGHT) || (vert && b == IN_DOWN)) {
		move(ui, v, 1);
	} else if (t == IN_NAV_PRESS && b == IN_A && v->n) {
		/* its list may still be on its way (the carousel came from the
		 * snapshot): load it now; the carousel may be rebuilt */
		struct sysent *se = ui_system_ready(ui, v->items[wrap(v->target, v->n)].se);

		if (se)
			ui_push(ui, glview_create(ui, se));
	} else if (t == IN_NAV_PRESS && b == IN_START) {
		settings_open_main(ui);
	}
}

static bool sv_update(struct ui *ui, struct screen *scr)
{
	struct sysview *v = SV(scr);

	if (v->fading)
		return true;
	if (!v->animating)
		return false;
	{
		float t = (float)(ui->now - v->t0) / (float)ANIM_MS;

		if (t >= 1.0f) {
			v->cam = (float)v->target;
			v->animating = false;
			/* normalize into [0, n) */
			if (v->n) {
				int w = wrap(v->target, v->n);

				v->cam = (float)w;
				v->target = w;
			}
			drop_far_backdrops(v, v->target);
		} else {
			v->cam = v->cam_from + ((float)v->target - v->cam_from) * ease(t);
		}
	}
	return true;
}

static int sv_timeout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	return SV(scr)->animating || SV(scr)->fading ? 16 : -1;
}

static void sv_relayout(struct ui *ui, struct screen *scr)
{
	struct sysview *v = SV(scr);

	/* every pixel asset of the old size goes; the worker (or the sync
	 * path) builds them again at the new one */
	for (int i = 0; i < v->n; i++) {
		struct sv_item *it = &v->items[i];

		free_item_assets(it);
		it->fade_t0 = 0;
		it->job = NULL;
		it->warm = false;
	}
	elem_release(&v->info);
	v->info_for = -1;
	v->items_gen++;
	v->look_gen = ui->look_gen;
}

static void free_items(struct sysview *v)
{
	for (int i = 0; i < v->n; i++)
		free_item_assets(&v->items[i]);
	free(v->items);
	v->items = NULL;
	v->n = 0;
	v->items_gen++;
}

static void sv_destroy(struct ui *ui, struct screen *scr)
{
	struct sysview *v = SV(scr);

	/* no job may land on it any more */
	prefetch_quiesce(ui);
	free_items(v);
	elem_free(&v->info);
	free(v);
}

/* Tests (ui_debug_assets): the carousel's system and whether its assets
 * are there: "sys=snes backdrop=ready logos=ready" (pending: not yet). */
static void sv_describe_assets(struct ui *ui, struct sysview *v, char *buf, size_t n)
{
	struct sv_item *it;
	int cur;

	(void)ui;
	if (!v->n) {
		snprintf(buf, n, "sys=none");
		return;
	}
	cur = wrap(v->target, v->n);
	it = &v->items[cur];
	snprintf(buf, n, "sys=%s backdrop=%s logos=%s", it->se->name,
		 it->backdrop && !it->fade_t0 ? "ready" : it->backdrop ? "fading" : "pending",
		 it->logos_built ? "ready" : "pending");
}

void sysview_describe_assets(struct ui *ui, struct screen *s, char *buf, size_t n)
{
	if (s->kind == SCR_SYSVIEW)
		sv_describe_assets(ui, SV(s), buf, n);
	else
		snprintf(buf, n, "sys=-");
}

static const struct screen_ops sv_ops = {
	.button = sv_button,
	.update = sv_update,
	.render = sv_render,
	.relayout = sv_relayout,
	.destroy = sv_destroy,
	.timeout = sv_timeout,
	.opaque = true,
};

static void fill_items(struct ui *ui, struct sysview *v)
{
	v->items = xcalloc((size_t)MAX(ui->nsys, 1), sizeof(struct sv_item));
	v->n = ui->nsys;
	for (int i = 0; i < ui->nsys; i++) {
		v->items[i].se = &ui->sys[i];
		strlcpy_(v->items[i].name, ui->sys[i].name, sizeof(v->items[i].name));
	}
	v->i18n_gen = i18n_generation();
	v->look_gen = ui->look_gen;
	v->items_gen++;
	v->help_for = -1;
}

/* The language changed (Settings > Language): rebuild every text (system
 * names, info bar, theme texts) and the backdrops holding them. */
static void check_language(struct ui *ui, struct sysview *v)
{
	if (v->i18n_gen == i18n_generation())
		return;
	free_items(v);
	elem_free(&v->info);
	v->configured = false;
	fill_items(ui, v);
	v->target = CLAMP(wrap(v->target, MAX(v->n, 1)), 0, MAX(0, v->n - 1));
	v->cam = (float)v->target;
	v->animating = false;
}

struct screen *sysview_create(struct ui *ui)
{
	struct sysview *v = xcalloc(1, sizeof(*v));

	v->base.ops = &sv_ops;
	v->base.kind = SCR_SYSVIEW;
	fill_items(ui, v);
	v->target = CLAMP(ui->sys_cursor, 0, MAX(0, v->n - 1));
	v->cam = (float)v->target;
	return &v->base;
}

/* Rebuilds after the system list or the theme changed. When only the list
 * of systems changed (the look is the same), the systems still there keep
 * their assets: no placeholder flashes when the loader rebuilds the
 * carousel at boot. The caller has quiesced the worker (their sysents move). */
void sysview_set_cursor(struct ui *ui, struct screen *s, int idx)
{
	struct sysview *v = SV(s);
	struct sv_item *old = v->items;
	int nold = v->n;
	bool same_look = v->look_gen == ui->look_gen && v->i18n_gen == i18n_generation();

	v->items = NULL;
	v->n = 0;
	elem_free(&v->info);
	v->configured = false;   /* (items[0]'s theme, moved along: no parse) */
	fill_items(ui, v);
	if (same_look) {
		for (int i = 0; i < v->n; i++)
			for (int k = 0; k < nold; k++) {
				struct sv_item *o = &old[k], *it = &v->items[i];

				/* (o->se points into the old, freed, ui->sys) */
				if (!o->name[0] || strcmp(o->name, it->name))
					continue;
				it->extras = o->extras;
				it->nextras = o->nextras;
				it->first_live = o->first_live;
				it->has_logo = o->has_logo;
				it->logo = o->logo;
				it->logotext = o->logotext;
				it->built = o->built;
				memcpy(it->img, o->img, sizeof(it->img));
				memcpy(it->img_owned, o->img_owned, sizeof(it->img_owned));
				it->logos_built = o->logos_built;
				it->backdrop = o->backdrop;
				it->bd_color = o->bd_color;
				it->warm = o->warm;
				memset(o, 0, sizeof(*o));
				break;
			}
	}
	for (int k = 0; k < nold; k++)
		free_item_assets(&old[k]);
	free(old);
	v->target = CLAMP(idx, 0, MAX(0, v->n - 1));
	v->cam = (float)v->target;
	v->animating = false;
	ui->sys_cursor = v->target;
}

/* The game counts changed (a list arrived): rebuild the info text. */
void sysview_refresh_info(struct ui *ui, struct screen *s)
{
	if (s->kind != SCR_SYSVIEW)
		return;
	SV(s)->info_for = -1;
	ui->dirty = true;
}

/* Moves the carousel without rebuilding anything (used when the game list
 * switches system with left/right). */
void sysview_jump(struct ui *ui, struct screen *s, int idx)
{
	struct sysview *v = SV(s);

	if (!v->n)
		return;
	v->dir = wrap(idx - v->target, v->n) <= v->n / 2 ? 1 : -1;
	v->moved = true;
	v->target = wrap(idx, v->n);
	v->cam = (float)v->target;
	v->animating = false;
	ui->sys_cursor = v->target;
	drop_far_backdrops(v, v->target);
}
