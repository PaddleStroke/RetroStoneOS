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
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../i18n/i18n.h"
#include "ui_internal.h"


#define ANIM_MS 220
#define KEEP_BACKDROPS 2   /* systems kept on each side of the cursor */

enum { CT_HORIZONTAL = 0, CT_VERTICAL };

struct sv_item {
	struct sysent *se;
	struct elem *extras;
	int nextras;
	int first_live;              /* extras [first_live..] are drawn live */
	bool has_logo;
	struct elem logo;            /* image path/color from the theme */
	struct elem logotext;        /* text style */
	struct gfx_image *img[2];    /* [0] normal, [1] selected */
	bool img_owned[2];
	struct gfx_image *backdrop;
	bool built, logos_built;
};

struct sysview {
	struct screen base;
	struct sv_item *items;
	int n;
	bool configured;
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
	unsigned i18n_gen;           /* language the texts were built in */
	/* animation (cam in item units, not wrapped) */
	float cam, cam_from;
	int target;
	int64_t t0;
	bool animating;
};

static struct sysview *SV(struct screen *s)
{
	return (struct sysview *)s;
}

static int wrap(int i, int n)
{
	return n ? ((i % n) + n) % n : 0;
}

/* ----------------------------------------------------------- config */
static void configure(struct ui *ui, struct sysview *v)
{
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
	v->configured = true;
}

/* ------------------------------------------------------------ backdrops */
static int cmp_z(const void *a, const void *b)
{
	const struct elem *x = a, *y = b;

	if (x->z != y->z)
		return x->z < y->z ? -1 : 1;
	return x->order - y->order;
}

static void build_extras(struct ui *ui, struct sysview *v, struct sv_item *it)
{
	const struct theme *t = ui_sys_theme(ui, it->se);
	const struct theme_view *tv = theme_view(t, "system");
	const struct theme_elem *te;

	it->nextras = 0;
	if (tv) {
		it->extras = xcalloc((size_t)tv->n + 1, sizeof(struct elem));
		for (int i = 0; i < tv->n; i++) {
			if (!tv->elems[i]->extra)
				continue;
			if (elem_from_theme(&it->extras[it->nextras], tv->elems[i])) {
				/* our themes' texts (taglines) are translated */
				elem_translate_theme_text(ui, &it->extras[it->nextras]);
				it->nextras++;
			}
		}
		qsort(it->extras, (size_t)it->nextras, sizeof(struct elem), cmp_z);
	}
	it->first_live = it->nextras;
	for (int i = 0; i < it->nextras; i++)
		if (it->extras[i].z >= v->z) {
			it->first_live = i;
			break;
		}
	/* logo */
	elem_init(&it->logo, EK_IMAGE, "logo");
	te = theme_elem(t, "system", "logo", "image");
	elem_apply(&it->logo, te, TF_PATH | TF_COLOR);
	it->has_logo = (it->logo.path[0] && file_exists(it->logo.path)) ||
		       (it->logo.def_path[0] && file_exists(it->logo.def_path));
	elem_init(&it->logotext, EK_TEXT, "logoText");
	it->logotext.font_size = 0.085f;
	it->logotext.align = AL_CENTER;
	it->logotext.fg = 0xff000000u;
	elem_set_text(&it->logotext, it->se->fullname);
	elem_apply(&it->logotext, theme_elem(t, "system", "logoText", "text"),
		   TF_FONT | TF_COLOR | TF_STYLE | TF_TEXT);
	elem_translate_theme_text(ui, &it->logotext);
	it->built = true;
}

/* Cache key of a backdrop: every property of the extras drawn into it,
 * the mtimes of the files they use, and the size. */
static uint64_t backdrop_key(struct ui *ui, struct sv_item *it)
{
	const struct theme *t = ui_sys_theme(ui, it->se);
	const struct theme_view *tv = theme_view(t, "system");
	uint64_t h = hash64_str(ui->theme_dir, (uint64_t)ui->w * 7919 + (uint64_t)ui->h);

	h = hash64_str(it->se->name, h ^ 0xb4c0);
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
	return h ^ 3; /* bump to invalidate old backdrops */
}

static void build_backdrop(struct ui *ui, struct sysview *v, struct sv_item *it)
{
	char path[1200];
	uint64_t key;
	struct gfx_surface s;
	int64_t t0 = ui_now_us();

	if (!it->built)
		build_extras(ui, v, it);
	key = backdrop_key(ui, it);
	if (img_cache_dir()[0]) {
		snprintf(path, sizeof(path), "%s/bd-%016llx.rpx", img_cache_dir(),
			 (unsigned long long)key);
		it->backdrop = rpx_load(path, key);
		if (it->backdrop && (it->backdrop->w != ui->w || it->backdrop->h != ui->h)) {
			gfx_image_free(it->backdrop);
			it->backdrop = NULL;
		}
		if (it->backdrop) {
			img_note_backdrop(true, ui_now_us() - t0);
			return;
		}
	}
	it->backdrop = gfx_image_new(ui->w, ui->h);
	gfx_surface_from_image(&s, it->backdrop);
	gfx_fill(&s, 0, 0, ui->w, ui->h, 0xff000000u);
	for (int i = 0; i < it->first_live; i++) {
		elem_draw(ui, &s, &it->extras[i], 0, 0);
		elem_release(&it->extras[i]); /* the pixels live in the backdrop now */
	}
	it->backdrop->flags |= GFX_IMG_OPAQUE;
	if (img_cache_dir()[0] && rpx_save(path, it->backdrop, key) == 0)
		img_note_written();
	img_note_backdrop(false, ui_now_us() - t0);
}

static void drop_far_backdrops(struct sysview *v, int center)
{
	for (int i = 0; i < v->n; i++) {
		int d = abs(i - center);

		d = MIN(d, v->n - d);
		if (d > KEEP_BACKDROPS && v->items[i].backdrop) {
			gfx_image_free(v->items[i].backdrop);
			v->items[i].backdrop = NULL;
		}
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

static void build_logos(struct ui *ui, struct sysview *v, struct sv_item *it)
{
	float W = (float)ui->w, H = (float)ui->h;
	float bw = v->logo_size[0] * v->logo_scale * W, bh = v->logo_size[1] * v->logo_scale * H;

	if (!it->built)
		build_extras(ui, v, it);
	free_logos(it);
	if (it->has_logo) {
		const char *p = it->logo.path[0] && file_exists(it->logo.path) ? it->logo.path :
				it->logo.def_path;
		int nw, nh;

		if (img_info(p, &nw, &nh)) {
			float s = MIN(bw / (float)nw, bh / (float)nh);
			int w1 = MAX(1, (int)lroundf((float)nw * s)), h1 = MAX(1, (int)lroundf((float)nh * s));
			int w0 = MAX(1, (int)lroundf((float)w1 / v->logo_scale));
			int h0 = MAX(1, (int)lroundf((float)h1 / v->logo_scale));

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
		const char *txt = t->text ? t->text : it->se->fullname;
		struct gfx_image *img;
		struct gfx_surface s;
		struct text_line lines[4];
		int lh = (int)lroundf((float)font_height(f) * t->line_spacing);
		int n, bwi = MAX(1, (int)bw), bhi = MAX(1, (int)bh), y0;

		if (t->upper) {
			utf8_upper(txt, up, sizeof(up));
			txt = up;
		}
		/* a word wider than the box: shrink the font rather than cut it
		 * (CJK needs no space to wrap: each character counts alone) */
		{
			int widest = 0, px = ui_font_px(ui, t->font_size);
			const char *w0 = txt, *c = txt;

			for (;;) {
				const char *q = c;
				unsigned cp = *c ? utf8_next(&q) : 0;
				bool cjk = cp >= 0x2e80;

				if (!cp || cp == ' ' || cjk) {
					widest = MAX(widest, font_text_width(f, w0, (int)(c - w0)));
					if (cjk)
						widest = MAX(widest, font_text_width(f, c, (int)(q - c)));
					if (!cp)
						break;
					w0 = q;
				}
				c = q;
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
		it->img[0] = gfx_image_scale(img, MAX(1, (int)lroundf((float)bwi / v->logo_scale)),
					     MAX(1, (int)lroundf((float)bhi / v->logo_scale)));
		it->img_owned[0] = true;
	}
	it->logos_built = true;
}

/* ------------------------------------------------------------ rendering */
static float ease(float t)
{
	t = 1.0f - t;
	return 1.0f - t * t * t;
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
		if (!it->backdrop)
			build_backdrop(ui, v, it);
		gfx_blit(s, it->backdrop, dx, dy, 255);
	} else {
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
		if (!it->logos_built)
			build_logos(ui, v, it);
		if (!it->img[1])
			continue;
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

static void sv_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct sysview *v = SV(scr);
	int base, next;
	float frac;
	/* TRANSLATORS: help bar labels (uppercase, short: ~10 characters) */
	static const struct help_prompt prompts_h[] = {
		{ "leftright", N_("choose") }, { "a", N_("select") }, { "start", N_("menu") },
	};
	/* TRANSLATORS: help bar labels (uppercase, short: ~10 characters) */
	static const struct help_prompt prompts_v[] = {
		{ "updown", N_("choose") }, { "a", N_("select") }, { "start", N_("menu") },
	};

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
	{
		struct help_style hs;
		const struct theme *t = ui_sys_theme(ui, v->items[wrap((int)lroundf(v->cam), v->n)].se);

		help_style_default(&hs);
		help_style_apply(&hs, theme_elem(t, "system", "help", "helpsystem"));
		help_draw(ui, s, &hs, v->type == CT_VERTICAL ? prompts_v : prompts_h, 3);
	}
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
	ui->sys_cursor = wrap(v->target, v->n);
	ui->dirty = true;
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
	return SV(scr)->animating ? 16 : -1;
}

static void sv_relayout(struct ui *ui, struct screen *scr)
{
	struct sysview *v = SV(scr);

	(void)ui;
	for (int i = 0; i < v->n; i++) {
		struct sv_item *it = &v->items[i];

		free_logos(it);
		gfx_image_free(it->backdrop);
		it->backdrop = NULL;
		for (int k = 0; k < it->nextras; k++)
			elem_release(&it->extras[k]);
	}
	elem_release(&v->info);
	v->info_for = -1;
}

static void free_items(struct sysview *v)
{
	for (int i = 0; i < v->n; i++) {
		struct sv_item *it = &v->items[i];

		free_logos(it);
		gfx_image_free(it->backdrop);
		for (int k = 0; k < it->nextras; k++)
			elem_free(&it->extras[k]);
		free(it->extras);
		elem_free(&it->logo);
		elem_free(&it->logotext);
	}
	free(v->items);
	v->items = NULL;
	v->n = 0;
}

static void sv_destroy(struct ui *ui, struct screen *scr)
{
	struct sysview *v = SV(scr);

	(void)ui;
	free_items(v);
	elem_free(&v->info);
	free(v);
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
	for (int i = 0; i < ui->nsys; i++)
		v->items[i].se = &ui->sys[i];
	v->i18n_gen = i18n_generation();
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

/* Rebuilds after the system list or the theme changed. */
void sysview_set_cursor(struct ui *ui, struct screen *s, int idx)
{
	struct sysview *v = SV(s);

	free_items(v);
	elem_free(&v->info);
	v->configured = false;
	fill_items(ui, v);
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
	v->target = wrap(idx, v->n);
	v->cam = (float)v->target;
	v->animating = false;
	ui->sys_cursor = v->target;
	drop_far_backdrops(v, v->target);
}
