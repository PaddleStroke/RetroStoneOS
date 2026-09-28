/*
 * widgets.c - theme elements (image, text, datetime, rating, ninepatch)
 * resolved to pixels, the help bar, button icons and the menu style.
 *
 * Every element is laid out once per (content, logical size): images are
 * loaded pre-scaled to their final pixel size and texts are rendered into
 * cached images, so drawing is a plain blit.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../i18n/i18n.h"
#include "ui_internal.h"

/* ES font sizes (fraction of min(w, h)) */
#define FONT_SMALL 0.035f
#define FONT_MEDIUM 0.045f
#define FONT_LARGE 0.085f

/* -------------------------------------------------------------- helpers */
int ui_font_px(const struct ui *ui, float frac)
{
	int m = MIN(ui->w, ui->h);

	return MAX(6, (int)(frac * (float)m));
}

void draw_panel(struct gfx_surface *s, int x, int y, int w, int h, int radius, gfx_color c)
{
	gfx_fill_round(s, x, y, w, h, radius, c);
}

struct gfx_image *render_text_image(struct font *f, const char *text, int max_w,
				    int max_lines, int align, float line_spacing,
				    gfx_color c, int *out_w, int *out_h)
{
	struct text_line lines[64];
	int n, lh, w = 0, h;
	struct gfx_image *img;
	struct gfx_surface s;
	struct ui_cost_scope cs;

	ui_cost_begin(&cs, UI_COST_TEXT);
	if (max_lines > 64)
		max_lines = 64;
	n = font_wrap(f, text, max_w, lines, max_lines);
	lh = (int)lroundf((float)font_height(f) * line_spacing);
	for (int i = 0; i < n; i++)
		w = MAX(w, lines[i].width);
	if (max_w > 0 && align != AL_LEFT)
		w = max_w;
	h = MAX(1, n) * lh;
	img = gfx_image_new(MAX(1, w), MAX(1, h));
	gfx_surface_from_image(&s, img);
	for (int i = 0; i < n; i++) {
		int x = align == AL_CENTER ? (w - lines[i].width) / 2 :
			align == AL_RIGHT ? w - lines[i].width : 0;

		font_draw(&s, f, x, font_baseline_in_box(f, i * lh, lh), text + lines[i].start,
			  lines[i].len, c);
	}
	gfx_image_update_flags(img);
	if (out_w)
		*out_w = w;
	if (out_h)
		*out_h = h;
	ui_cost_end(&cs);
	return img;
}

void draw_text_box(struct ui *ui, struct gfx_surface *s, struct font *f, const char *text,
		   int x, int y, int w, int h, int align, gfx_color c)
{
	char buf[512];
	int tw, tx;

	(void)ui;
	font_ellipsize(f, text, w, buf, sizeof(buf));
	tw = font_text_width(f, buf, -1);
	tx = align == AL_CENTER ? x + (w - tw) / 2 : align == AL_RIGHT ? x + w - tw : x;
	font_draw(s, f, tx, font_baseline_in_box(f, y, h), buf, -1, c);
}

/* Our own theme sets (rsos-dark, rsos-light...): their literal texts are
 * translated (C_("theme", ...), listed in view_gamelist.c); third-party
 * themes keep theirs. */
bool ui_theme_is_ours(const struct ui *ui)
{
	return !strncmp(ui->theme_name, "rsos-", 5);
}

/* A text element whose text came from the theme: translated when the theme
 * is one of ours. */
void elem_translate_theme_text(struct ui *ui, struct elem *e)
{
	if (!ui_theme_is_ours(ui) || e->kind != EK_TEXT || !e->text || !e->text[0])
		return;
	/* elem_set_text() ignores the same text (C_() returns its argument
	 * when there is no translation) */
	elem_set_text(e, C_("theme", e->text));
}

void format_datetime(const struct elem *e, int64_t now, char *out, size_t n)
{
	time_t t = (time_t)e->time;
	struct tm tm;

	(void)now;
	if (!e->time) {
		/* TRANSLATORS: game metadata date: never played (last played) or no
		 * release date known. Short (~15 characters). */
		strlcpy_(out, e->relative ? C_("date", "never") : C_("date", "unknown"), n);
		return;
	}
	if (e->relative) {
		int64_t d = (int64_t)time(NULL) - e->time;
		int k;

		if (d < 0)
			d = 0;
		if (d < 60) {
			k = (int)d;
			/* TRANSLATORS: when a game was last played. Short (~15 characters). */
			snprintf(out, n, _n("%d sec ago", "%d secs ago", k), k);
		} else if (d < 3600) {
			k = (int)(d / 60);
			/* TRANSLATORS: when a game was last played. Short (~15 characters). */
			snprintf(out, n, _n("%d minute ago", "%d minutes ago", k), k);
		} else if (d < 86400) {
			k = (int)(d / 3600);
			/* TRANSLATORS: when a game was last played. Short (~15 characters). */
			snprintf(out, n, _n("%d hour ago", "%d hours ago", k), k);
		} else {
			k = (int)(d / 86400);
			/* TRANSLATORS: when a game was last played. Short (~15 characters). */
			snprintf(out, n, _n("%d day ago", "%d days ago", k), k);
		}
		return;
	}
	/* dates without a time zone (gamelist.xml): UTC on purpose */
	gmtime_r(&t, &tm);
	/* TRANSLATORS: default date of theme metadata (release date, last
	 * played) when the theme sets no format; strftime; French %d/%m/%Y */
	if (!strftime(out, n, e->fmt[0] ? e->fmt : C_("strftime", "%m/%d/%Y"), &tm))
		strlcpy_(out, "?", n);
}

/* ------------------------------------------------------------- elements */
void elem_init(struct elem *e, enum elem_kind kind, const char *name)
{
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	strlcpy_(e->name, name ? name : "", sizeof(e->name));
	e->visible = true;
	e->color = e->color_end = 0xffffffffu;
	e->fg = 0xff000000u;
	e->font_size = FONT_MEDIUM;
	e->line_spacing = 1.5f;
	e->fmt[0] = 0;   /* date format: the language's default (format_datetime) */
}

void elem_set_text(struct elem *e, const char *text)
{
	if (e->text && text && !strcmp(e->text, text))
		return;
	free(e->text);
	e->text = text ? xstrdup(text) : NULL;
	e->laid_out = false;
}

void elem_set_path(struct elem *e, const char *path)
{
	if (!strcmp(e->path, path ? path : ""))
		return;
	strlcpy_(e->path, path ? path : "", sizeof(e->path));
	e->laid_out = false;
}

void elem_apply(struct elem *e, const struct theme_elem *te, unsigned flags)
{
	const char *v;
	float x, y;
	bool b;

	if (!te)
		return;
	if (flags & TF_POS) {
		if (theme_get_pair(te, "pos", &x, &y)) {
			e->pos[0] = x;
			e->pos[1] = y;
		}
		if (theme_get_pair(te, "origin", &x, &y)) {
			e->origin[0] = x;
			e->origin[1] = y;
		}
	}
	if (flags & TF_ORIGIN && theme_get_pair(te, "origin", &x, &y)) {
		e->origin[0] = x;
		e->origin[1] = y;
	}
	if (flags & TF_SIZE) {
		if (theme_get_pair(te, "size", &x, &y)) {
			e->size[0] = x;
			e->size[1] = y;
			e->has_maxsize = false;
		} else if (theme_get_pair(te, "maxSize", &x, &y)) {
			e->maxsize[0] = x;
			e->maxsize[1] = y;
			e->has_maxsize = true;
			e->size[0] = e->size[1] = 0;
		}
	}
	if (flags & TF_PATH) {
		if ((v = theme_get_str(te, "path")))
			strlcpy_(e->path, v, sizeof(e->path));
		if ((v = theme_get_str(te, "default")))
			strlcpy_(e->def_path, v, sizeof(e->def_path));
		if ((v = theme_get_str(te, "filledPath")))
			strlcpy_(e->filled, v, sizeof(e->filled));
		if ((v = theme_get_str(te, "unfilledPath")))
			strlcpy_(e->unfilled, v, sizeof(e->unfilled));
		b = e->tile;
		if (theme_get_bool(te, "tile", &b))
			e->tile = b;
	}
	if (flags & TF_COLOR) {
		gfx_color c;

		if (e->kind == EK_TEXT || e->kind == EK_DATETIME) {
			if (theme_get_color(te, "color", &c))
				e->fg = c;
			if (theme_get_color(te, "backgroundColor", &c))
				e->bg = c;
		} else {
			if (theme_get_color(te, "color", &c))
				e->color = e->color_end = c;
			if (theme_get_color(te, "colorEnd", &c))
				e->color_end = c;
			if ((v = theme_get_str(te, "gradientType")))
				e->grad_h = !strcmp(v, "horizontal");
		}
	}
	if (flags & TF_FONT) {
		if ((v = theme_get_str(te, "fontPath")))
			strlcpy_(e->font_path, v, sizeof(e->font_path));
		if (theme_get_float(te, "fontSize", &x) && x > 0)
			e->font_size = x;
	}
	if (flags & TF_TEXT) {
		if ((v = theme_get_str(te, "text")) || (v = theme_get_str(te, "value")))
			elem_set_text(e, v);
	}
	if (flags & TF_STYLE) {
		if ((v = theme_get_str(te, "alignment")))
			e->align = !strcmp(v, "center") ? AL_CENTER : !strcmp(v, "right") ? AL_RIGHT : AL_LEFT;
		b = e->upper;
		if (theme_get_bool(te, "forceUppercase", &b))
			e->upper = b;
		if (theme_get_float(te, "lineSpacing", &x) && x > 0)
			e->line_spacing = x;
		if ((v = theme_get_str(te, "format")))
			strlcpy_(e->fmt, v, sizeof(e->fmt));
		b = e->relative;
		if (theme_get_bool(te, "displayRelative", &b))
			e->relative = b;
	}
	if (flags & TF_ZINDEX && theme_get_float(te, "zIndex", &x))
		e->z = x;
	if (flags & TF_VISIBLE) {
		b = e->visible;
		if (theme_get_bool(te, "visible", &b))
			e->visible = b;
	}
	e->laid_out = false;
}

bool elem_from_theme(struct elem *e, const struct theme_elem *te)
{
	enum elem_kind k;

	if (!strcmp(te->type, "image") || !strcmp(te->type, "video"))
		k = EK_IMAGE;
	else if (!strcmp(te->type, "text"))
		k = EK_TEXT;
	else if (!strcmp(te->type, "datetime"))
		k = EK_DATETIME;
	else if (!strcmp(te->type, "rating"))
		k = EK_RATING;
	else if (!strcmp(te->type, "ninepatch"))
		k = EK_NINEPATCH;
	else
		return false;
	elem_init(e, k, te->name);
	e->extra = te->extra;
	e->order = te->order;
	e->z = 10; /* extras default */
	elem_apply(e, te, TF_ALL);
	return true;
}

void elem_release(struct elem *e)
{
	if (e->icon_img)
		gfx_image_free(e->img);   /* built-in star, the element's own */
	else
		img_put(e->img);
	if (e->icon_img2)
		gfx_image_free(e->img2);
	else
		img_put(e->img2);
	e->img = e->img2 = NULL;
	e->icon_img = e->icon_img2 = false;
	gfx_image_free(e->cache);
	e->cache = NULL;
	e->laid_out = false;
}

void elem_free(struct elem *e)
{
	elem_release(e);
	free(e->text);
	e->text = NULL;
}

static void place(struct ui *ui, struct elem *e, int w, int h)
{
	e->w = w;
	e->h = h;
	e->x = (int)lroundf(e->pos[0] * (float)ui->w - e->origin[0] * (float)w);
	e->y = (int)lroundf(e->pos[1] * (float)ui->h - e->origin[1] * (float)h);
}

static void layout_image(struct ui *ui, struct elem *e)
{
	const char *path = e->path[0] ? e->path : e->def_path;
	int nw = 0, nh = 0;
	float W = (float)ui->w, H = (float)ui->h;
	float w, h;

	if (!path[0] || !img_info(path, &nw, &nh)) {
		if (path[0] && e->def_path[0] && path != e->def_path &&
		    img_info(e->def_path, &nw, &nh)) {
			path = e->def_path;
		} else {
			place(ui, e, (int)(e->size[0] * W), (int)(e->size[1] * H));
			return;
		}
	}
	if (e->kind == EK_NINEPATCH) {
		e->img = img_get(path, 0, 0, e->color);
		place(ui, e, (int)lroundf(e->size[0] * W), (int)lroundf(e->size[1] * H));
		return;
	}
	if (e->tile) {
		/* tiles keep the image's pixel size (like ES) */
		e->img = img_get(path, 0, 0, e->color);
		w = e->size[0] > 0 ? e->size[0] * W : (float)nw;
		h = e->size[1] > 0 ? e->size[1] * H : (float)nh;
		place(ui, e, (int)lroundf(w), (int)lroundf(h));
		return;
	}
	if (e->has_maxsize && e->maxsize[0] > 0 && e->maxsize[1] > 0) {
		float mw = e->maxsize[0] * W, mh = e->maxsize[1] * H;
		float sx = mw / (float)nw, sy = mh / (float)nh;

		if (sx < sy) {
			w = mw;
			h = (float)nh * sx;
		} else {
			w = (float)nw * sy;
			h = mh;
		}
	} else if (e->size[0] > 0 || e->size[1] > 0) {
		w = e->size[0] * W;
		h = e->size[1] * H;
		if (w <= 0)
			w = h * (float)nw / (float)nh;
		if (h <= 0)
			h = w * (float)nh / (float)nw;
	} else {
		w = (float)nw;
		h = (float)nh;
	}
	w = MAX(1.0f, roundf(w));
	h = MAX(1.0f, roundf(h));
	e->img = img_get(path, (int)w, (int)h, e->color);
	if (e->color_end != e->color)
		ui_log_once("colorEnd", "theme: image colorEnd gradients are drawn with the start color");
	place(ui, e, (int)w, (int)h);
}

static void layout_text(struct ui *ui, struct elem *e)
{
	struct font *f = font_get(e->font_path, ui_font_px(ui, e->font_size));
	char *text = e->text ? e->text : "";
	char up[4096];
	char tmp[128];
	float W = (float)ui->w, H = (float)ui->h;
	int bw = (int)lroundf(e->size[0] * W), bh = (int)lroundf(e->size[1] * H);
	int fh = font_height(f);
	int lh = (int)lroundf((float)fh * e->line_spacing);
	struct gfx_surface s;

	if (e->kind == EK_DATETIME) {
		format_datetime(e, ui->now, tmp, sizeof(tmp));
		text = tmp;
	}
	if (e->upper) {
		utf8_upper(text, up, sizeof(up));
		text = up;
	}
	if (bw <= 0 && bh <= 0) {
		/* one line, autosized */
		bw = MAX(1, font_text_width(f, text, -1));
		bh = lh;
		e->cache = gfx_image_new(bw, bh);
		gfx_surface_from_image(&s, e->cache);
		if (e->bg >> 24)
			gfx_fill(&s, 0, 0, bw, bh, e->bg);
		font_draw(&s, f, 0, font_baseline_in_box(f, 0, bh), text, -1, e->fg);
	} else if (bh > 0 && (float)bh <= (float)fh * 1.2f + 0.5f) {
		/* single line box: ellipsis */
		char buf[1024];
		int tw, x;

		if (bw <= 0)
			bw = MAX(1, font_text_width(f, text, -1));
		font_ellipsize(f, text, bw, buf, sizeof(buf));
		tw = font_text_width(f, buf, -1);
		x = e->align == AL_CENTER ? (bw - tw) / 2 : e->align == AL_RIGHT ? bw - tw : 0;
		e->cache = gfx_image_new(bw, bh);
		gfx_surface_from_image(&s, e->cache);
		if (e->bg >> 24)
			gfx_fill(&s, 0, 0, bw, bh, e->bg);
		font_draw(&s, f, x, font_baseline_in_box(f, 0, bh), buf, -1, e->fg);
	} else {
		struct text_line lines[128];
		int n, th, y0;

		if (bw <= 0)
			bw = ui->w;
		n = font_wrap(f, text, bw, lines, 128);
		th = n * lh;
		if (bh <= 0)
			bh = MAX(th, 1);
		/* vertically centred, top aligned when it overflows */
		y0 = th < bh ? (bh - th) / 2 : 0;
		e->cache = gfx_image_new(bw, bh);
		gfx_surface_from_image(&s, e->cache);
		if (e->bg >> 24)
			gfx_fill(&s, 0, 0, bw, bh, e->bg);
		for (int i = 0; i < n; i++) {
			int y = y0 + i * lh;
			int x = e->align == AL_CENTER ? (bw - lines[i].width) / 2 :
				e->align == AL_RIGHT ? bw - lines[i].width : 0;

			if (y >= bh)
				break;
			font_draw(&s, f, x, font_baseline_in_box(f, y, lh), text + lines[i].start,
				  lines[i].len, e->fg);
		}
	}
	gfx_image_update_flags(e->cache);
	place(ui, e, bw, bh);
}

static const char *const svg_star =
	"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' width='24' height='24'>"
	"<path fill='#ffffff' d='M12 2.2l2.9 6.3 6.9.7-5.2 4.7 1.5 6.8L12 17.2l-6.1 3.5 1.5-6.8-5.2-4.7"
	" 6.9-.7z'/></svg>";
static const char *const svg_star_empty =
	"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' width='24' height='24'>"
	"<path fill='none' stroke='#ffffff' stroke-width='1.6' stroke-linejoin='round'"
	" d='M12 3.4l2.5 5.5 6 .6-4.5 4.1 1.3 5.9L12 16.4l-5.3 3.1 1.3-5.9-4.5-4.1 6-.6z'/></svg>";

static void layout_rating(struct ui *ui, struct elem *e)
{
	float W = (float)ui->w, H = (float)ui->h;
	int sh = e->size[1] > 0 ? (int)lroundf(e->size[1] * H) :
		 e->size[0] > 0 ? (int)lroundf(e->size[0] * W / 5.0f) :
		 font_height(font_get(NULL, ui_font_px(ui, FONT_SMALL)));

	sh = MAX(sh, 4);
	if (e->filled[0])
		e->img = img_get(e->filled, sh, sh, e->color);
	if (e->unfilled[0])
		e->img2 = img_get(e->unfilled, sh, sh, e->color);
	/* built-in stars: the element's own copies (the icon table may
	 * recycle its entries while the element keeps them, review F-M4) */
	if (!e->img) {
		e->img = img_from_svg_string(svg_star, sh, sh, e->color);
		e->icon_img = e->img != NULL;
	}
	if (!e->img2) {
		e->img2 = img_from_svg_string(svg_star_empty, sh, sh, e->color);
		e->icon_img2 = e->img2 != NULL;
	}
	place(ui, e, sh * 5, sh);
}

void elem_layout(struct ui *ui, struct elem *e)
{
	elem_release(e);
	switch (e->kind) {
	case EK_IMAGE:
	case EK_NINEPATCH:
		layout_image(ui, e);
		break;
	case EK_TEXT:
	case EK_DATETIME: {
		struct ui_cost_scope cs;

		ui_cost_begin(&cs, UI_COST_TEXT);
		layout_text(ui, e);
		ui_cost_end(&cs);
		break;
	}
	case EK_RATING:
		layout_rating(ui, e);
		break;
	}
	e->laid_out = true;
}

void elem_draw(struct ui *ui, struct gfx_surface *s, struct elem *e, int dx, int dy)
{
	if (!e->visible)
		return;
	if (!e->laid_out)
		elem_layout(ui, e);
	switch (e->kind) {
	case EK_IMAGE:
		if (!e->img)
			return;
		if (e->tile)
			gfx_blit_tiled(s, e->img, e->x + dx, e->y + dy, e->w, e->h, 255);
		else
			gfx_blit(s, e->img, e->x + dx, e->y + dy, 255);
		break;
	case EK_NINEPATCH:
		if (e->img)
			gfx_ninepatch(s, e->img, e->x + dx, e->y + dy, e->w, e->h,
				      MAX(1, MIN(e->img->w, e->img->h) / 3));
		break;
	case EK_TEXT:
	case EK_DATETIME:
		gfx_blit(s, e->cache, e->x + dx, e->y + dy, 255);
		break;
	case EK_RATING: {
		int sh = e->h;
		int filled_w = (int)lroundf(CLAMP(e->rating, 0.0f, 1.0f) * 5.0f * (float)sh);

		for (int i = 0; i < 5; i++) {
			int sx = e->x + dx + i * sh;
			int fw = CLAMP(filled_w - i * sh, 0, sh);

			if (fw > 0 && e->img)
				gfx_blit_sub(s, e->img, (struct gfx_rect){ 0, 0, fw, sh }, sx, e->y + dy, 255);
			if (fw < sh && e->img2)
				gfx_blit_sub(s, e->img2, (struct gfx_rect){ fw, 0, sh - fw, sh },
					     sx + fw, e->y + dy, 255);
		}
		break;
	}
	}
}

/* ------------------------------------------------------------------ icons */
static const char *const svg_bolt =
	"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 12 20' width='12' height='20'>"
	"<path fill='#ffffff' d='M7 0L0 11.5h5L3.5 20L12 7.5H6.8z'/></svg>";
/* d-pad: arms at 45 % opacity, the highlighted ones opaque */
static void svg_dpad(char *out, size_t n, bool up, bool down, bool left, bool right)
{
	snprintf(out, n,
		 "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' width='24' height='24'>"
		 "<rect x='9' y='1' width='6' height='8' rx='1.2' fill='#fff' fill-opacity='%s'/>"
		 "<rect x='9' y='15' width='6' height='8' rx='1.2' fill='#fff' fill-opacity='%s'/>"
		 "<rect x='1' y='9' width='8' height='6' rx='1.2' fill='#fff' fill-opacity='%s'/>"
		 "<rect x='15' y='9' width='8' height='6' rx='1.2' fill='#fff' fill-opacity='%s'/>"
		 "<rect x='9' y='9' width='6' height='6' fill='#fff' fill-opacity='0.45'/></svg>",
		 up ? "1" : "0.45", down ? "1" : "0.45", left ? "1" : "0.45", right ? "1" : "0.45");
}

/* Renders a button icon (no cache; thread-safe: the asset worker renders
 * the help bar icons of a view ahead, ui_icon_adopt()). */
struct gfx_image *icon_render(const char *name, int px, gfx_color c)
{
	struct gfx_image *img = NULL;
	struct gfx_surface s;
	char svg[1024];

	if (!strcmp(name, "bolt")) {
		img = img_from_svg_string(svg_bolt, 0, px, c);
	} else if (!strcmp(name, "star_filled")) {
		img = img_from_svg_string(svg_star, px, px, c);
	} else if (!strcmp(name, "star_unfilled")) {
		img = img_from_svg_string(svg_star_empty, px, px, c);
	} else if (!strcmp(name, "updown") || !strcmp(name, "leftright") || !strcmp(name, "dpad")) {
		bool ud = strcmp(name, "leftright") != 0, lr = strcmp(name, "updown") != 0;

		svg_dpad(svg, sizeof(svg), ud, ud, lr, lr);
		img = img_from_svg_string(svg, px, px, c);
	} else {
		/* buttons: a shape in color c with the label punched out */
		const char *label = name;
		char upper[16];
		struct font *f;
		int w = px, tw, r;

		utf8_upper(label, upper, sizeof(upper));
		if (!strcmp(name, "start") || !strcmp(name, "select")) {
			f = font_get(font_default_path(true), MAX(6, px * 45 / 100));
			tw = font_text_width(f, upper, -1);
			w = tw + px * 2 / 3;
			r = px / 2;
		} else if (!strcmp(name, "l") || !strcmp(name, "r") || !strcmp(name, "l2") ||
			   !strcmp(name, "r2")) {
			f = font_get(font_default_path(true), MAX(6, px * 6 / 10));
			tw = font_text_width(f, upper, -1);
			w = MAX(px * 13 / 10, tw + px / 2);
			r = px / 4;
		} else {
			f = font_get(font_default_path(true), MAX(6, px * 7 / 10));
			tw = font_text_width(f, upper, -1);
			r = px / 2;
		}
		img = gfx_image_new(w, px);
		gfx_surface_from_image(&s, img);
		gfx_fill_round(&s, 0, 0, w, px, r, c);
		font_erase(&s, f, (w - tw) / 2, font_baseline_in_box(f, 0, px), upper, -1);
		gfx_image_update_flags(img);
	}
	return img;
}

static void icon_key(const char *name, int px, gfx_color c, char *key, size_t n)
{
	snprintf(key, n, "%s/%d/%08x", name, px, c);
}

/* Into the icon table (img is the table's then). */
static struct gfx_image *icon_insert(struct ui *ui, const char *key, struct gfx_image *img)
{
	/* The icon table is small: recycle the least recently used entry.
	 * Callers use an icon within one draw (help_draw fetches its <= 12
	 * icons, then draws them): those are the most recently used, so they
	 * are never the victim (review F-M4: the oldest-inserted one was). */
	if (ui->nicons == (int)ARRAY_SIZE(ui->icons)) {
		int victim = 0;

		for (int i = 1; i < ui->nicons; i++)
			if (ui->icons[i].used < ui->icons[victim].used)
				victim = i;
		gfx_image_free(ui->icons[victim].img);
		ui->icons[victim] = ui->icons[--ui->nicons];
	}
	strlcpy_(ui->icons[ui->nicons].key, key, sizeof(ui->icons[0].key));
	ui->icons[ui->nicons].img = img;
	ui->icons[ui->nicons].used = ++ui->icon_tick;
	ui->nicons++;
	return img;
}

static struct gfx_image *icon_find(struct ui *ui, const char *key)
{
	for (int i = 0; i < ui->nicons; i++)
		if (!strcmp(ui->icons[i].key, key)) {
			ui->icons[i].used = ++ui->icon_tick;
			return ui->icons[i].img;
		}
	return NULL;
}

struct gfx_image *ui_icon(struct ui *ui, const char *name, int px, gfx_color c)
{
	char key[48];
	struct gfx_image *img;

	icon_key(name, px, c, key, sizeof(key));
	if ((img = icon_find(ui, key)))
		return img;
	img = icon_render(name, px, c);
	return img ? icon_insert(ui, key, img) : NULL;
}

void ui_icon_adopt(struct ui *ui, const struct ui_icon_ahead *ic)
{
	char key[48];

	if (!ic->img)
		return;
	icon_key(ic->name, ic->px, ic->color, key, sizeof(key));
	if (icon_find(ui, key))
		gfx_image_free(ic->img);
	else
		icon_insert(ui, key, ic->img);
}

/* The icon height help_draw() uses for a style. */
static int help_icon_px(struct ui *ui, const struct help_style *hs)
{
	struct font *f = font_get(hs->font_path, ui_font_px(ui, hs->font_size));

	return MAX(8, (int)lroundf((float)font_cap_height(f) * 1.25f));
}

int help_icons_ahead(struct ui *ui, const struct help_style *hs, const struct help_prompt *p, int n,
		     struct ui_icon_ahead *out, int max)
{
	int px = help_icon_px(ui, hs), k = 0;

	for (int i = 0; i < n && k < max; i++) {
		strlcpy_(out[k].name, p[i].icon, sizeof(out[k].name));
		out[k].px = px;
		out[k].color = hs->icon;
		out[k].img = icon_render(p[i].icon, px, hs->icon);
		k++;
	}
	return k;
}

/* ------------------------------------------------------------------- help */
/* The battery level ("85%", "85 %" in French). */
static void battery_text(char *out, size_t n, int pct)
{
	/* TRANSLATORS: battery level in the help bar ("%d %%" in French) */
	snprintf(out, n, C_("battery", "%d%%"), pct);
}

void help_style_default(struct help_style *hs)
{
	memset(hs, 0, sizeof(*hs));
	hs->pos[0] = 0.012f;
	hs->pos[1] = 0.9515f;
	hs->text = 0xff777777u;
	hs->icon = 0xff777777u;
	hs->font_size = FONT_SMALL;
}

void help_style_apply(struct help_style *hs, const struct theme_elem *te)
{
	const char *v;
	float x, y;
	gfx_color c;

	if (!te)
		return;
	if (theme_get_pair(te, "pos", &x, &y)) {
		hs->pos[0] = x;
		hs->pos[1] = y;
	}
	if (theme_get_pair(te, "origin", &x, &y)) {
		hs->origin[0] = x;
		hs->origin[1] = y;
	}
	if (theme_get_color(te, "textColor", &c))
		hs->text = c;
	if (theme_get_color(te, "iconColor", &c))
		hs->icon = c;
	if ((v = theme_get_str(te, "fontPath")))
		strlcpy_(hs->font_path, v, sizeof(hs->font_path));
	if (theme_get_float(te, "fontSize", &x) && x > 0)
		hs->font_size = x;
}

void help_draw(struct ui *ui, struct gfx_surface *s, const struct help_style *hs,
	       const struct help_prompt *p, int n)
{
	struct font *f = font_get(hs->font_path, ui_font_px(ui, hs->font_size));
	int ih = MAX(8, (int)lroundf((float)font_cap_height(f) * 1.25f));
	int gap_icon = MAX(2, (int)lroundf((float)ui->w * 0.0042f));
	int gap_entry = MAX(4, (int)lroundf((float)ui->w * 0.0208f));
	int total = 0, x, y;
	char labels[12][96];         /* translated and uppercased */
	struct gfx_image *icons[12];

	struct help_prompt pp[12];
	int margin = (int)lroundf((float)ui->w * MIN(hs->pos[0], 0.1f));
	const struct power_status *bat = ui_battery(ui);
	bool show_bat = bat && bat->valid && bat->battery_present && bat->percent >= 0;
	char full[32];
	int avail;

	battery_text(full, sizeof(full), 100);
	avail = ui->w - 2 * margin - (show_bat ? ih * 3 + font_text_width(f, full, -1) + gap_entry : 0);

	if (ui->in_snapshot)
		return;
	n = MIN(n, 12);
	memcpy(pp, p, sizeof(pp[0]) * (size_t)n);
	p = pp;
	for (;;) {
		total = 0;
		for (int i = 0; i < n; i++) {
			/* the callers' tables mark their labels with N_() */
			utf8_upper(_(p[i].label), labels[i], sizeof(labels[i]));
			icons[i] = ui_icon(ui, p[i].icon, ih, hs->icon);
			total += (icons[i] ? icons[i]->w : ih) + gap_icon +
				 font_text_width(f, labels[i], -1);
			if (i + 1 < n)
				total += gap_entry;
		}
		/* too wide for the screen: drop the least useful prompts (the
		 * ones before the last, which is "menu") */
		if (total <= avail || n <= 2)
			break;
		pp[n - 2] = pp[n - 1];
		n--;
	}
	x = (int)lroundf(hs->pos[0] * (float)ui->w - hs->origin[0] * (float)total);
	y = (int)lroundf(hs->pos[1] * (float)ui->h - hs->origin[1] * (float)ih);
	for (int i = 0; i < n; i++) {
		if (icons[i]) {
			gfx_blit(s, icons[i], x, y, 255);
			x += icons[i]->w;
		} else {
			x += ih;
		}
		x += gap_icon;
		x += font_draw(s, f, x, font_baseline_in_box(f, y, ih), labels[i], -1, hs->text);
		x += gap_entry;
	}
	if (show_bat)
		draw_battery(ui, s, ui->w - MAX(margin, ui->w / 60), y, ih, hs->icon, f);
}

/* ------------------------------------------------------------ menu style */
void menu_style_from_theme(struct ui *ui, struct menu_style *ms)
{
	const struct theme *t = NULL;
	const struct theme_elem *te;
	gfx_color c;
	const char *v;
	float f;

	memset(ms, 0, sizeof(*ms));
	/* defaults: a neutral dark panel */
	ms->bg = 0xf0202226u;
	ms->dim = 0x99000000u;
	ms->title = 0xffffffffu;
	ms->text = 0xffe8e8e8u;
	ms->text_dim = 0xff9aa0a6u;
	ms->sel_text = 0xffffffffu;
	ms->selector = 0xff3d7bd9u;
	ms->separator = 0x30ffffffu;
	ms->accent = 0xff3d7bd9u;
	ms->font_size = 0.05f;
	ms->small_size = 0.038f;
	strlcpy_(ms->font_bold, font_default_path(true), sizeof(ms->font_bold));

	for (int i = 0; i < ui->nsys && !t; i++)
		if (ui->sys[i].theme)
			t = ui->sys[i].theme;
	if (!t)
		return;

	/* 1. explicit "menu" view (Batocera-style subset) */
	if (theme_view(t, "menu")) {
		if ((te = theme_elem(t, "menu", "menubg", "menuBackground")) ||
		    (te = theme_elem(t, "menu", "menu", "menuBackground"))) {
			if (theme_get_color(te, "color", &c))
				ms->bg = c;
		}
		te = theme_elem(t, "menu", "menutext", "menuText");
		if (!te)
			te = theme_elem(t, "menu", "menutext", NULL);
		if (te) {
			if (theme_get_color(te, "color", &c))
				ms->text = ms->title = c;
			if (theme_get_color(te, "selectedColor", &c))
				ms->sel_text = c;
			if (theme_get_color(te, "selectorColor", &c))
				ms->selector = ms->accent = c;
			if (theme_get_color(te, "separatorColor", &c))
				ms->separator = c;
			if ((v = theme_get_str(te, "fontPath")))
				strlcpy_(ms->font_path, v, sizeof(ms->font_path));
			if (theme_get_float(te, "fontSize", &f) && f > 0)
				ms->font_size = f;
		}
		te = theme_elem(t, "menu", "menutextsmall", NULL);
		if (te) {
			if (theme_get_color(te, "color", &c))
				ms->text_dim = c;
			if (theme_get_float(te, "fontSize", &f) && f > 0)
				ms->small_size = f;
		}
		te = theme_elem(t, "menu", "menugroup", NULL);
		if (te && (v = theme_get_str(te, "fontPath")))
			strlcpy_(ms->font_bold, v, sizeof(ms->font_bold));
		return;
	}
	/* 2. derived from the game list colors */
	te = theme_elem(t, "detailed", "gamelist", "textlist");
	if (!te)
		te = theme_elem(t, "basic", "gamelist", "textlist");
	if (te) {
		gfx_color prim = 0, sel = 0, selr = 0;
		bool hp = theme_get_color(te, "primaryColor", &prim);
		bool hs = theme_get_color(te, "selectedColor", &sel);
		bool hr = theme_get_color(te, "selectorColor", &selr);

		if (hp) {
			bool light_text = gfx_color_luma(prim) > 128;

			ms->text = ms->title = prim | 0xff000000u;
			ms->bg = light_text ? 0xf01c1d20u : 0xf0f4f4f4u;
			ms->text_dim = gfx_color_mix(ms->text, ms->bg | 0xff000000u, 90);
			ms->separator = light_text ? 0x30ffffffu : 0x30000000u;
		}
		if (hr && (selr >> 24) > 0x40) {
			ms->selector = ms->accent = selr | 0xff000000u;
			ms->sel_text = hs ? sel | 0xff000000u :
				(gfx_color_luma(selr) > 140 ? 0xff000000u : 0xffffffffu);
		} else if (hs) {
			/* transparent selector: highlight with the selected color */
			ms->selector = ms->accent = sel | 0xff000000u;
			ms->sel_text = gfx_color_luma(sel) > 140 ? 0xff000000u : 0xffffffffu;
		}
		if ((v = theme_get_str(te, "fontPath")))
			strlcpy_(ms->font_path, v, sizeof(ms->font_path));
	}
}

/* --------------------------------------------------------------- battery */
int draw_battery(struct ui *ui, struct gfx_surface *s, int right, int y, int h, gfx_color c,
		 struct font *f)
{
	const struct power_status *st = ui_battery(ui);
	int bw = h * 17 / 10, bh = h * 8 / 10, tip = MAX(2, h / 8);
	int x, by = y + (h - bh) / 2, w = 0, pct;
	char txt[32];
	bool charging;
	gfx_color fill = c;

	if (!st || !st->valid || !st->battery_present || st->percent < 0)
		return 0;
	pct = CLAMP(st->percent, 0, 100);
	charging = st->state == BATT_CHARGING || (st->charger_online && st->state != BATT_DISCHARGING);
	if (!charging && st->level >= POWER_LEVEL_LOW)
		fill = 0xffe0453au;
	battery_text(txt, sizeof(txt), pct);
	w = bw + tip + h / 3 + font_text_width(f, txt, -1);
	x = right - w;
	font_draw(s, f, x, font_baseline_in_box(f, y, h), txt, -1, c);
	x += font_text_width(f, txt, -1) + h / 3;
	gfx_stroke_round(s, x, by, bw, bh, MAX(2, h / 6), MAX(1, h / 10), c);
	gfx_fill(s, x + bw, by + bh / 3, tip, bh / 3, c);
	{
		int pad = MAX(2, h / 6);
		int iw = bw - 2 * pad, fw = iw * pct / 100;

		if (fw > 0)
			gfx_fill(s, x + pad, by + pad, fw, bh - 2 * pad, fill);
	}
	if (charging) {
		struct gfx_image *bolt = ui_icon(ui, "bolt", bh + 2,
						 gfx_color_luma(fill) > 140 ? 0xff1a1a1au : 0xffffffffu);

		if (bolt) {
			/* outline the bolt with the text color for contrast */
			gfx_blit(s, bolt, x + (bw - bolt->w) / 2, by - 1, 255);
		}
	}
	return w;
}

/* "1.5 GB", "1,5 Go" in French: the language's units and separators. */
void format_bytes(uint64_t b, char *out, size_t n)
{
	i18n_format_size(b, out, n);
}
