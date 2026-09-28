/*
 * view_gamelist.c - the ES "basic", "detailed" and "video" game list
 * views (video is drawn like detailed: md_video shows the game's image).
 *
 * Layout follows ES (ISimpleGameListView, BasicGameListView,
 * DetailedGameListView, VideoGameListView, TextListComponent): default
 * positions, then the theme overrides, then values placed right of their
 * labels, the description under the lowest value.
 *
 * Speed: the static layers under the list (background, extras) are
 * composited once into a backdrop; a frame is one copy, the visible rows
 * (glyph cache) and the cached metadata texts/images. Game images are
 * loaded when the cursor rests (or immediately on a single press).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../i18n/i18n.h"
#include "ui_internal.h"


#define FONT_SMALL 0.035f
#define FONT_MEDIUM 0.045f
#define MD_DELAY_MS 150
#define DESC_SCROLL_DELAY 3000
#define DESC_SCROLL_PX_S 12
#define JUMP_SHOW_MS 700      /* the big letter after L1/R1 */

enum { LBL_RATING = 0, LBL_RELEASE, LBL_DEV, LBL_PUB, LBL_GENRE, LBL_PLAYERS,
       LBL_LAST, LBL_COUNT, NLBL };

static const char *const g_lbl_names[NLBL] = {
	"md_lbl_rating", "md_lbl_releasedate", "md_lbl_developer", "md_lbl_publisher",
	"md_lbl_genre", "md_lbl_players", "md_lbl_lastplayed", "md_lbl_playcount",
};
/* EmulationStation's default label texts (themes that place the labels
 * without giving a text: carbon, simple...); translated where shown. */
static const char *const g_lbl_text[NLBL] = {
	/* TRANSLATORS: game metadata labels of the game list, followed by the
	 * value on the same line: keep the trailing ": " (French " : ") */
	N_("Rating: "), N_("Released: "), N_("Developer: "), N_("Publisher: "), N_("Genre: "),
	/* TRANSLATORS: game metadata labels of the game list, followed by the
	 * value on the same line: keep the trailing ": " (French " : ") */
	N_("Players: "), N_("Last played: "), N_("Times played: "),
};

/* The texts of the themes/rsos-dark and themes/rsos-light theme.xml files
 * (and their system folders' taglines): the extractor cannot see them in the XML, so they are listed
 * here; shown through elem_translate_theme_text() (C_("theme", text)) when
 * the theme set is one of ours. Keep this list in sync with the themes. */
static const char *const g_rsos_theme_texts[] __attribute__((unused)) = {
	/* TRANSLATORS: game metadata label of the built-in themes, uppercase,
	 * ~7 characters wide (longer ones are cut with "...") */
	NC_("theme", "Year"),
	/* TRANSLATORS: game metadata label of the built-in themes, uppercase,
	 * ~7 characters wide (longer ones are cut with "...") */
	NC_("theme", "Genre"),
	/* TRANSLATORS: game metadata label of the built-in themes, uppercase,
	 * ~7 characters wide (longer ones are cut with "...") */
	NC_("theme", "Players"),
	/* TRANSLATORS: game metadata label of the built-in themes, uppercase,
	 * ~7 characters wide (longer ones are cut with "...") */
	NC_("theme", "Rating"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes
	 * (uppercase, one line, ~35 characters) */
	NC_("theme", "Your favorite games"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes
	 * (uppercase, one line, ~35 characters) */
	NC_("theme", "Recently played"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes
	 * (uppercase, one line, ~35 characters) */
	NC_("theme", "Every game on the console"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes: the
	 * MSX, a home computer of 1983 (uppercase, one line, ~35 characters) */
	NC_("theme", "Home computer · 1983"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes: an
	 * emulator name follows "Arcade" (uppercase, one line) */
	NC_("theme", "Arcade · MAME 2003-Plus"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes: an
	 * emulator name follows "Arcade" (uppercase, one line) */
	NC_("theme", "Arcade · FinalBurn Neo"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes: the
	 * Doom games run on id Software's engine (uppercase, one line) */
	NC_("theme", "id Software engine · 1993"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes: PICO-8
	 * is a made-up console for small games (uppercase, one line) */
	NC_("theme", "Fantasy console · 2015"),
	/* TRANSLATORS: tagline under the carousel of the built-in themes:
	 * ScummVM runs the classic point-and-click adventure games (uppercase) */
	NC_("theme", "Adventure engines · 2001"),
	/* TRANSLATORS: game metadata label of the built-in themes, uppercase,
	 * ~7 characters wide: the time spent playing the game */
	NC_("theme", "Played"),
};
static const char *const g_val_names[NLBL] = {
	"md_rating", "md_releasedate", "md_developer", "md_publisher", "md_genre",
	"md_players", "md_lastplayed", "md_playcount",
};

struct textlist {
	float pos[2], size[2], origin[2];
	gfx_color selector, selector_end, selected, primary, secondary;
	bool sel_grad_h;
	char font_path[1024];
	float font_size;
	int align;
	float hmargin;
	bool upper;
	float line_spacing;
	float sel_height, sel_offset;
	bool has_sel_height;
	char sel_image[1024];
	bool sel_tile;
	float z;
	/* pixels */
	int x, y, w, h;
	struct font *font;
	float entry;
	int sel_h, sel_off, margin;
	struct gfx_image *sel_img;
};

/* a drawable in z order */
struct layer {
	struct elem *e;
	bool is_list;
	bool is_desc;
	float z;
	int order;
};

struct glview {
	struct screen base;
	struct sysent *se;
	char view[16];
	bool detailed;
	bool laid_out;
	unsigned i18n_gen;            /* language the texts were built in */
	/* elements */
	struct elem background, logo, logotext;
	struct elem *extras;
	int nextras;
	struct textlist tl;
	struct elem md_image, md_marquee, md_video, md_thumbnail, md_favorite;
	bool has_marquee, has_video, has_thumbnail, has_favorite;
	struct elem lbl[NLBL], val[NLBL];
	struct elem desc, name;
	/* play time (batch 2): md_lbl_playtime / md_playtime when the theme
	 * places them (ours do), else added to md_playcount's value */
	struct elem pt_lbl, pt_val;
	bool has_pt;
	struct help_style help;
	/* z-sorted draw list */
	struct layer *layers;
	int nlayers;
	int first_live;               /* layers [0..first_live) are in the backdrop */
	struct gfx_image *backdrop;
	/* list: the rows shown are vis[0..nvis) (indices into se->games), all
	 * of them or the search's matches */
	int *vis;
	int nvis;
	char search[96];              /* "" = no search */
	char **names;                 /* display names of the rows (uppercased if needed) */
	int nnames;
	int cursor;                   /* row */
	int md_for;                   /* row whose metadata is shown */
	/* jump to letter (L1/R1): the letter shown big for a moment */
	char jump[8];
	int64_t jump_until;
	/* "Search all games": the view owns its entry and list */
	bool owns_se;
	bool md_images_for_cursor;
	int64_t moved_at;
	/* description scrolling */
	struct gfx_image *desc_img;
	int desc_scroll;
	int64_t desc_t0;
};

static struct glview *GV(struct screen *s)
{
	return (struct glview *)s;
}

/* The rows: every game of the list, or the ones matching the search. */
static void build_vis(struct glview *v)
{
	struct gamelist *gl = v->se->games;
	int n = gl ? gl->n : 0;

	free(v->vis);
	v->vis = xcalloc((size_t)MAX(n, 1), sizeof(int));
	v->nvis = 0;
	for (int i = 0; i < n; i++)
		if (games_match(gl->games[i].name, v->search))
			v->vis[v->nvis++] = i;
}

static struct game *row_game(struct glview *v, int row)
{
	struct gamelist *gl = v->se->games;

	if (!gl || !v->vis || row < 0 || row >= v->nvis || v->vis[row] >= gl->n)
		return NULL;
	return &gl->games[v->vis[row]];
}

static struct game *cur_game(struct glview *v)
{
	if (!v->nvis)
		return NULL;
	return row_game(v, CLAMP(v->cursor, 0, v->nvis - 1));
}

/* ------------------------------------------------------------ building */
static void textlist_defaults(struct textlist *tl, bool detailed)
{
	memset(tl, 0, sizeof(*tl));
	tl->pos[0] = detailed ? 0.51f : 0.0f;
	tl->pos[1] = 0.2f;
	tl->size[0] = detailed ? 0.49f : 1.0f;
	tl->size[1] = 0.8f;
	tl->selector = tl->selector_end = 0xff000000u;
	tl->selected = 0;
	tl->primary = 0xff0000ffu;
	tl->secondary = 0xff00ff00u;
	tl->sel_grad_h = true;
	tl->font_size = FONT_MEDIUM;
	tl->align = detailed ? AL_LEFT : AL_CENTER;
	tl->line_spacing = 1.5f;
	tl->z = 20;
}

static void textlist_apply(struct textlist *tl, const struct theme_elem *te)
{
	float x, y;
	const char *v;
	gfx_color c;
	bool b;

	if (!te)
		return;
	if (theme_get_pair(te, "pos", &x, &y)) {
		tl->pos[0] = x;
		tl->pos[1] = y;
	}
	if (theme_get_pair(te, "size", &x, &y)) {
		tl->size[0] = x;
		tl->size[1] = y;
	}
	if (theme_get_pair(te, "origin", &x, &y)) {
		tl->origin[0] = x;
		tl->origin[1] = y;
	}
	if (theme_get_color(te, "selectorColor", &c))
		tl->selector = tl->selector_end = c;
	if (theme_get_color(te, "selectorColorEnd", &c))
		tl->selector_end = c;
	if ((v = theme_get_str(te, "selectorGradientType")))
		tl->sel_grad_h = strcmp(v, "vertical") != 0;
	if (theme_get_color(te, "selectedColor", &c))
		tl->selected = c;
	if (theme_get_color(te, "primaryColor", &c))
		tl->primary = c;
	if (theme_get_color(te, "secondaryColor", &c))
		tl->secondary = c;
	if ((v = theme_get_str(te, "fontPath")))
		strlcpy_(tl->font_path, v, sizeof(tl->font_path));
	if (theme_get_float(te, "fontSize", &x) && x > 0)
		tl->font_size = x;
	if ((v = theme_get_str(te, "alignment")))
		tl->align = !strcmp(v, "center") ? AL_CENTER : !strcmp(v, "right") ? AL_RIGHT : AL_LEFT;
	if (theme_get_float(te, "horizontalMargin", &x))
		tl->hmargin = x;
	b = tl->upper;
	if (theme_get_bool(te, "forceUppercase", &b))
		tl->upper = b;
	if (theme_get_float(te, "lineSpacing", &x) && x > 0)
		tl->line_spacing = x;
	if (theme_get_float(te, "selectorHeight", &x) && x > 0) {
		tl->sel_height = x;
		tl->has_sel_height = true;
	}
	if (theme_get_float(te, "selectorOffsetY", &x))
		tl->sel_offset = x;
	if ((v = theme_get_str(te, "selectorImagePath")))
		strlcpy_(tl->sel_image, v, sizeof(tl->sel_image));
	b = tl->sel_tile;
	if (theme_get_bool(te, "selectorImageTile", &b))
		tl->sel_tile = b;
	theme_get_float(te, "zIndex", &tl->z);
}

static void textlist_layout(struct ui *ui, struct textlist *tl)
{
	float W = (float)ui->w, H = (float)ui->h;
	int fpx = ui_font_px(ui, tl->font_size);

	tl->font = font_get(tl->font_path, fpx);
	tl->w = (int)lroundf(tl->size[0] * W);
	tl->h = (int)lroundf(tl->size[1] * H);
	tl->x = (int)lroundf(tl->pos[0] * W - tl->origin[0] * (float)tl->w);
	tl->y = (int)lroundf(tl->pos[1] * H - tl->origin[1] * (float)tl->h);
	tl->entry = (float)MAX(font_height(tl->font), fpx) * tl->line_spacing;
	tl->sel_h = tl->has_sel_height ? (int)lroundf(tl->sel_height * H) : (int)lroundf((float)fpx * 1.5f);
	tl->sel_off = (int)lroundf(tl->sel_offset * H);
	tl->margin = (int)lroundf(tl->hmargin * W);
	img_put(tl->sel_img);
	tl->sel_img = NULL;
	if (tl->sel_image[0]) {
		if (tl->sel_tile)
			tl->sel_img = img_get(tl->sel_image, 0, 0, 0xffffffffu);
		else
			tl->sel_img = img_get(tl->sel_image, MAX(1, tl->w), MAX(1, tl->sel_h), 0xffffffffu);
	}
}

static void choose_view(struct ui *ui, struct glview *v)
{
	const struct theme *t = ui_sys_theme(ui, v->se);
	bool media = v->se->games && v->se->games->has_media;
	const char *pref = settings_get(ui->settings, "gamelist_view", "auto");

	if (!strcmp(pref, "basic"))
		media = false;
	else if (!strcmp(pref, "detailed"))
		media = true;
	if (media && theme_view(t, "detailed"))
		strlcpy_(v->view, "detailed", sizeof(v->view));
	else if (media && theme_view(t, "video"))
		strlcpy_(v->view, "video", sizeof(v->view));
	else if (media)
		strlcpy_(v->view, "detailed", sizeof(v->view));
	else
		strlcpy_(v->view, "basic", sizeof(v->view));
	v->detailed = strcmp(v->view, "basic") != 0;
}

static const struct theme_elem *TE(struct ui *ui, struct glview *v, const char *name,
				   const char *type)
{
	return theme_elem(ui_sys_theme(ui, v->se), v->view, name, type);
}

static int cmp_layer(const void *a, const void *b)
{
	const struct layer *x = a, *y = b;

	if (x->z != y->z)
		return x->z < y->z ? -1 : 1;
	return x->order - y->order;
}

static void add_layer(struct glview *v, struct elem *e, bool list, bool desc, float z, int order)
{
	struct layer *l = &v->layers[v->nlayers++];

	l->e = e;
	l->is_list = list;
	l->is_desc = desc;
	l->z = z;
	l->order = order;
}

/* Text box height used by ES for one line of an autosized text. */
static float line_h_norm(struct ui *ui, const struct elem *e)
{
	struct font *f = font_get(e->font_path, ui_font_px(ui, e->font_size));

	return (float)font_height(f) * e->line_spacing / (float)ui->h;
}

static void build(struct ui *ui, struct glview *v)
{
	const struct theme *t = ui_sys_theme(ui, v->se);
	const struct theme_view *tv;
	float W = (float)ui->w, H = (float)ui->h;
	const float pad = 0.01f;

	choose_view(ui, v);
	tv = theme_view(t, v->view);

	/* background */
	elem_init(&v->background, EK_IMAGE, "background");
	v->background.size[0] = v->background.size[1] = 1.0f;
	v->background.z = 0;
	elem_apply(&v->background, TE(ui, v, "background", "image"), TF_ALL);

	/* header: logo image or logoText */
	elem_init(&v->logo, EK_IMAGE, "logo");
	v->logo.size[1] = 0.185f;
	v->logo.origin[0] = 0.5f;
	v->logo.pos[0] = 0.5f;
	v->logo.z = 50;
	elem_apply(&v->logo, TE(ui, v, "logo", "image"), TF_ALL);
	elem_init(&v->logotext, EK_TEXT, "logoText");
	v->logotext.size[0] = 1.0f;
	v->logotext.align = AL_CENTER;
	v->logotext.z = 50;
	elem_set_text(&v->logotext, v->se->fullname);
	elem_apply(&v->logotext, TE(ui, v, "logoText", "text"), TF_ALL);
	elem_translate_theme_text(ui, &v->logotext);
	if (!(v->logo.path[0] && file_exists(v->logo.path)))
		v->logo.visible = false;
	else
		v->logotext.visible = false;

	/* extras */
	v->nextras = 0;
	if (tv) {
		v->extras = xcalloc((size_t)tv->n + 1, sizeof(struct elem));
		for (int i = 0; i < tv->n; i++)
			if (tv->elems[i]->extra && elem_from_theme(&v->extras[v->nextras], tv->elems[i])) {
				elem_translate_theme_text(ui, &v->extras[v->nextras]);
				v->nextras++;
			}
	}

	/* the list */
	textlist_defaults(&v->tl, v->detailed);
	textlist_apply(&v->tl, TE(ui, v, "gamelist", "textlist"));

	/* help */
	help_style_default(&v->help);
	help_style_apply(&v->help, TE(ui, v, "help", "helpsystem"));

	if (v->detailed) {
		float list_y = v->tl.pos[1];
		float col = 0.48f / 2.0f;
		float start_x = 0.01f, start_y = 0.625f;
		float bottom = 0;

		elem_init(&v->md_image, EK_IMAGE, "md_image");
		v->md_image.origin[0] = v->md_image.origin[1] = 0.5f;
		v->md_image.pos[0] = 0.25f;
		v->md_image.pos[1] = 0.2f + 0.2125f;
		(void)list_y;
		v->md_image.maxsize[0] = 0.5f - 2 * pad;
		v->md_image.maxsize[1] = 0.4f;
		v->md_image.has_maxsize = true;
		v->md_image.z = 30;
		elem_apply(&v->md_image, TE(ui, v, "md_image", "image"),
			   TF_POS | TF_SIZE | TF_ZINDEX | TF_VISIBLE | TF_PATH);
		v->md_image.path[0] = 0;

		/* video view: md_video shows the snapshot, md_marquee */
		elem_init(&v->md_video, EK_IMAGE, "md_video");
		v->md_video.origin[0] = v->md_video.origin[1] = 0.5f;
		v->md_video.pos[0] = 0.25f;
		v->md_video.pos[1] = 0.2f + 0.2125f;
		v->md_video.maxsize[0] = 0.5f - 2 * pad;
		v->md_video.maxsize[1] = 0.4f;
		v->md_video.has_maxsize = true;
		v->md_video.z = 30;
		v->has_video = !strcmp(v->view, "video") && TE(ui, v, "md_video", "video");
		elem_apply(&v->md_video, TE(ui, v, "md_video", "video"),
			   TF_POS | TF_SIZE | TF_ZINDEX | TF_VISIBLE);
		elem_init(&v->md_marquee, EK_IMAGE, "md_marquee");
		v->md_marquee.origin[0] = v->md_marquee.origin[1] = 0.5f;
		v->md_marquee.pos[0] = 0.25f;
		v->md_marquee.pos[1] = 0.10f;
		v->md_marquee.maxsize[0] = 0.5f - 2 * pad;
		v->md_marquee.maxsize[1] = 0.18f;
		v->md_marquee.has_maxsize = true;
		v->md_marquee.z = 35;
		v->has_marquee = TE(ui, v, "md_marquee", "image") != NULL;
		elem_apply(&v->md_marquee, TE(ui, v, "md_marquee", "image"),
			   TF_POS | TF_SIZE | TF_ZINDEX | TF_VISIBLE | TF_PATH);
		v->md_marquee.path[0] = 0;
		elem_init(&v->md_thumbnail, EK_IMAGE, "md_thumbnail");
		v->md_thumbnail.pos[0] = 2.0f;
		v->md_thumbnail.pos[1] = 2.0f;
		v->md_thumbnail.z = 35;
		v->has_thumbnail = TE(ui, v, "md_thumbnail", "image") != NULL;
		elem_apply(&v->md_thumbnail, TE(ui, v, "md_thumbnail", "image"),
			   TF_POS | TF_SIZE | TF_ZINDEX | TF_VISIBLE | TF_PATH);
		v->md_thumbnail.path[0] = 0;
		/* favorite badge (not in upstream ES; used by carbon, gbz35...) */
		elem_init(&v->md_favorite, EK_IMAGE, "md_favorite");
		v->md_favorite.z = 40;
		v->has_favorite = TE(ui, v, "md_favorite", "image") != NULL;
		elem_apply(&v->md_favorite, TE(ui, v, "md_favorite", "image"), TF_ALL);

		/* labels: ES default grid, 2 columns x 4 rows, font small */
		for (int i = 0; i < NLBL; i++) {
			struct elem *l = &v->lbl[i];

			elem_init(l, EK_TEXT, g_lbl_names[i]);
			l->font_size = FONT_SMALL;
			l->z = 40;
			elem_set_text(l, _(g_lbl_text[i]));
			if (i % 4 == 0) {
				l->pos[0] = start_x + col * (float)(i / 4);
				l->pos[1] = start_y;
			} else {
				struct elem *p = &v->lbl[i - 1];

				l->pos[0] = p->pos[0];
				l->pos[1] = p->pos[1] + line_h_norm(ui, p) + 0.01f;
			}
			elem_apply(l, TE(ui, v, g_lbl_names[i], "text"), TF_ALL);
			elem_translate_theme_text(ui, l);   /* rsos themes' "Year"... */
		}
		/* values right of their labels */
		for (int i = 0; i < NLBL; i++) {
			struct elem *l = &v->lbl[i], *val = &v->val[i];
			const char *type = i == LBL_RATING ? "rating" :
				(i == LBL_RELEASE || i == LBL_LAST) ? "datetime" : "text";
			float lw, lh, vh;

			elem_init(val, i == LBL_RATING ? EK_RATING : !strcmp(type, "datetime") ?
				  EK_DATETIME : EK_TEXT, g_val_names[i]);
			val->font_size = FONT_SMALL;
			val->z = 40;
			if (i == LBL_LAST)
				val->relative = true;
			/* label size: explicit or autosized */
			if (l->size[0] > 0) {
				lw = l->size[0];
			} else {
				struct font *f = font_get(l->font_path, ui_font_px(ui, l->font_size));
				const char *txt = l->text ? l->text : "";
				char up[256];

				/* the text shown (the theme's or ours, translated) */
				if (l->upper)
					utf8_upper(txt, up, sizeof(up));
				else
					strlcpy_(up, txt, sizeof(up));
				lw = (float)font_text_width(f, up, -1) / W;
			}
			lh = l->size[1] > 0 ? l->size[1] : line_h_norm(ui, l);
			if (i == LBL_RATING) {
				struct font *f = font_get(NULL, ui_font_px(ui, FONT_SMALL));

				vh = (float)font_height(f) / H;
				val->size[1] = vh;
				val->size[0] = 0;
			} else {
				vh = line_h_norm(ui, val);
				val->size[0] = MAX(0.0f, col - lw);
				val->size[1] = 0;
			}
			val->pos[0] = l->pos[0] + lw;
			val->pos[1] = l->pos[1] + (lh - vh) / 2.0f;
			bottom = MAX(bottom, val->pos[1] + vh);
			elem_apply(val, TE(ui, v, g_val_names[i], type), TF_ALL & ~TF_TEXT);
			if (i == LBL_RATING)
				elem_apply(val, TE(ui, v, g_val_names[i], "rating"), TF_PATH | TF_COLOR);
		}
		/* description */
		elem_init(&v->desc, EK_TEXT, "md_description");
		v->desc.font_size = FONT_SMALL;
		v->desc.pos[0] = pad;
		v->desc.pos[1] = bottom + 0.01f;
		v->desc.size[0] = 0.5f - 2 * pad;
		v->desc.size[1] = 1.0f - v->desc.pos[1];
		v->desc.z = 40;
		elem_apply(&v->desc, TE(ui, v, "md_description", "text"),
			   TF_POS | TF_SIZE | TF_ZINDEX | TF_VISIBLE | TF_FONT | TF_COLOR | TF_STYLE);
		/* name: off screen unless the theme places it */
		elem_init(&v->name, EK_TEXT, "md_name");
		v->name.pos[0] = 1.0f;
		v->name.pos[1] = 1.0f;
		v->name.fg = 0xffaaaaaau;
		v->name.align = AL_CENTER;
		v->name.z = 40;
		elem_apply(&v->name, TE(ui, v, "md_name", "text"), TF_ALL & ~TF_TEXT);
		/* play time (ours, not ES): shown where the theme places
		 * md_playtime (rsos-dark/-light); other themes get it after the
		 * play count */
		elem_init(&v->pt_lbl, EK_TEXT, "md_lbl_playtime");
		elem_init(&v->pt_val, EK_TEXT, "md_playtime");
		v->pt_lbl.pos[0] = v->pt_val.pos[0] = 1.0f;
		v->pt_lbl.pos[1] = v->pt_val.pos[1] = 1.0f;
		v->pt_lbl.font_size = v->pt_val.font_size = FONT_SMALL;
		v->pt_lbl.z = v->pt_val.z = 40;
		/* TRANSLATORS: game metadata label (as "Times played: "): the time
		 * spent playing the game; keep the trailing ": " (French " : ") */
		elem_set_text(&v->pt_lbl, _("Played: "));
		v->has_pt = TE(ui, v, "md_playtime", "text") != NULL;
		elem_apply(&v->pt_lbl, TE(ui, v, "md_lbl_playtime", "text"), TF_ALL);
		elem_translate_theme_text(ui, &v->pt_lbl);
		elem_apply(&v->pt_val, TE(ui, v, "md_playtime", "text"), TF_ALL & ~TF_TEXT);
	}

	/* z-ordered layers */
	v->layers = xcalloc((size_t)(v->nextras + 40), sizeof(struct layer));
	v->nlayers = 0;
	add_layer(v, &v->background, false, false, v->background.z, -100);
	for (int i = 0; i < v->nextras; i++)
		add_layer(v, &v->extras[i], false, false, v->extras[i].z, v->extras[i].order);
	add_layer(v, NULL, true, false, v->tl.z, 1000);
	add_layer(v, &v->logo, false, false, v->logo.z, 2000);
	add_layer(v, &v->logotext, false, false, v->logotext.z, 2001);
	if (v->detailed) {
		if (v->has_video)
			add_layer(v, &v->md_video, false, false, v->md_video.z, 3000);
		else
			add_layer(v, &v->md_image, false, false, v->md_image.z, 3000);
		if (v->has_marquee)
			add_layer(v, &v->md_marquee, false, false, v->md_marquee.z, 3001);
		if (v->has_thumbnail)
			add_layer(v, &v->md_thumbnail, false, false, v->md_thumbnail.z, 3002);
		if (v->has_favorite)
			add_layer(v, &v->md_favorite, false, false, v->md_favorite.z, 3003);
		for (int i = 0; i < NLBL; i++) {
			add_layer(v, &v->lbl[i], false, false, v->lbl[i].z, 4000 + i);
			add_layer(v, &v->val[i], false, false, v->val[i].z, 4100 + i);
		}
		add_layer(v, &v->desc, false, true, v->desc.z, 4200);
		add_layer(v, &v->name, false, false, v->name.z, 4201);
		if (v->has_pt) {
			add_layer(v, &v->pt_lbl, false, false, v->pt_lbl.z, 4202);
			add_layer(v, &v->pt_val, false, false, v->pt_val.z, 4203);
		}
	}
	qsort(v->layers, (size_t)v->nlayers, sizeof(struct layer), cmp_layer);
	/* the backdrop takes every static layer below the first dynamic one */
	v->first_live = 0;
	for (int i = 0; i < v->nlayers; i++) {
		struct layer *l = &v->layers[i];

		if (l->is_list || !l->e ||
		    (l->e != &v->background && !(l->e >= v->extras && l->e < v->extras + v->nextras)))
			break;
		v->first_live = i + 1;
	}
	v->i18n_gen = i18n_generation();
	v->md_for = -1;
	v->laid_out = true;
}

static void make_names(struct glview *v)
{
	build_vis(v);
	v->names = xcalloc((size_t)MAX(v->nvis, 1), sizeof(char *));
	v->nnames = v->nvis;
	for (int i = 0; i < v->nvis; i++) {
		const struct game *g = row_game(v, i);
		char buf[512], name[480];
		const char *star = g->favorite ? "\xe2\x98\x85 " : "";

		if (g->hidden)
			/* TRANSLATORS: a hidden game's row in a list (Settings > Game
			 * lists > Show hidden games is on); %s is its name */
			snprintf(name, sizeof(name), _("%s (hidden)"), g->name);
		else
			strlcpy_(name, g->name, sizeof(name));
		if (v->tl.upper) {
			char up[480];

			utf8_upper(name, up, sizeof(up));
			snprintf(buf, sizeof(buf), "%s%s", star, up);
		} else {
			snprintf(buf, sizeof(buf), "%s%s", star, name);
		}
		v->names[i] = xstrdup(buf);
	}
}

static void free_names(struct glview *v)
{
	if (!v->names)
		return;
	for (int i = 0; i < v->nnames; i++)
		free(v->names[i]);
	free(v->names);
	v->names = NULL;
	v->nnames = 0;
}

/* "3 h 12 min", "25 min", "-" (never played). */
void ui_format_playtime(int64_t s, char *out, size_t n)
{
	int64_t m = (s + 30) / 60;

	if (s <= 0) {
		snprintf(out, n, "-");
	} else if (m < 60) {
		/* TRANSLATORS: play time under an hour ("25 min"); at least 1 */
		snprintf(out, n, _n("%d min", "%d min", (int)MAX(m, 1)), (int)MAX(m, 1));
	} else {
		/* TRANSLATORS: play time: hours and minutes ("3 h 12 min") */
		snprintf(out, n, _("%d h %02d min"), (int)(m / 60), (int)(m % 60));
	}
}

/* ------------------------------------------------------------- backdrop */
/* A search in progress replaces the list's title (logo or logoText). */
static bool hides_header(const struct glview *v, const struct elem *e)
{
	return v->search[0] && e && (e == &v->logo || e == &v->logotext) && e->visible;
}

/* The header's place in pixels (the union of the visible logo and
 * logoText), false when the theme shows neither. */
static bool header_rect(struct ui *ui, struct glview *v, int *x, int *y, int *w, int *h)
{
	struct elem *es[2] = { &v->logo, &v->logotext };
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool any = false;

	for (int i = 0; i < 2; i++) {
		struct elem *e = es[i];

		if (!e->visible)
			continue;
		if (!e->laid_out)
			elem_layout(ui, e);
		if (e->w <= 0 || e->h <= 0)
			continue;
		if (!any) {
			x0 = e->x;
			y0 = e->y;
			x1 = e->x + e->w;
			y1 = e->y + e->h;
		} else {
			x0 = MIN(x0, e->x);
			y0 = MIN(y0, e->y);
			x1 = MAX(x1, e->x + e->w);
			y1 = MAX(y1, e->y + e->h);
		}
		any = true;
	}
	x0 = MAX(x0, 0);
	y0 = MAX(y0, 0);
	x1 = MIN(x1, ui->w);
	y1 = MIN(y1, ui->h);
	if (!any || x1 - x0 < ui->w / 5 || y1 <= y0)
		return false;
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
	return true;
}

static void build_backdrop(struct ui *ui, struct glview *v)
{
	struct gfx_surface s;

	gfx_image_free(v->backdrop);
	v->backdrop = gfx_image_new(ui->w, ui->h);
	gfx_surface_from_image(&s, v->backdrop);
	gfx_fill(&s, 0, 0, ui->w, ui->h, 0xff000000u);
	for (int i = 0; i < v->first_live; i++) {
		if (hides_header(v, v->layers[i].e))
			continue;
		elem_draw(ui, &s, v->layers[i].e, 0, 0);
		elem_release(v->layers[i].e);
	}
	v->backdrop->flags |= GFX_IMG_OPAQUE;
}

/* -------------------------------------------------------------- metadata */
static const char *gpath(const char *p)
{
	return p && *p ? p : "";
}

static void set_md(struct ui *ui, struct glview *v, bool with_images)
{
	struct game *g = cur_game(v);
	char buf[128];

	if (!v->detailed)
		return;
	if (v->md_for != v->cursor) {
		/* TRANSLATORS: a game metadata value missing from gamelist.xml
		 * (developer, publisher, genre) */
		const char *unknown = C_("metadata", "unknown");

		v->md_for = v->cursor;
		v->md_images_for_cursor = false;
		elem_set_text(&v->val[LBL_DEV], g && g->developer ? g->developer : unknown);
		elem_set_text(&v->val[LBL_PUB], g && g->publisher ? g->publisher : unknown);
		elem_set_text(&v->val[LBL_GENRE], g && g->genre ? g->genre : unknown);
		elem_set_text(&v->val[LBL_PLAYERS], g && g->players ? g->players : "1");
		i18n_format_number(g ? g->playcount : 0, 0, buf, sizeof(buf));
		{
			char pt[48];

			ui_format_playtime(g ? g->playtime : 0, pt, sizeof(pt));
			if (v->has_pt) {
				elem_set_text(&v->pt_val, pt);
			} else if (g && g->playtime > 0) {
				/* no place for it in the theme: after the play count */
				char both[200];

				snprintf(both, sizeof(both), "%s (%s)", buf, pt);
				strlcpy_(buf, both, sizeof(buf));
			}
		}
		elem_set_text(&v->val[LBL_COUNT], buf);
		v->val[LBL_RELEASE].time = g ? g->releasedate : 0;
		v->val[LBL_RELEASE].laid_out = false;
		v->val[LBL_LAST].time = g ? g->lastplayed : 0;
		v->val[LBL_LAST].laid_out = false;
		v->val[LBL_RATING].rating = g && g->rating > 0 ? g->rating : 0;
		elem_set_text(&v->name, g ? g->name : "");
		gfx_image_free(v->desc_img);
		v->desc_img = NULL;
		v->desc_scroll = 0;
		v->desc_t0 = ui->now;
		if (v->has_favorite)
			v->md_favorite.visible = g && g->favorite;
		/* images: dropped now, loaded when the cursor rests */
		elem_set_path(&v->md_image, "");
		elem_set_path(&v->md_video, "");
		elem_set_path(&v->md_marquee, "");
		elem_set_path(&v->md_thumbnail, "");
		elem_release(&v->md_image);
		elem_release(&v->md_video);
		elem_release(&v->md_marquee);
		elem_release(&v->md_thumbnail);
		v->md_image.laid_out = v->md_video.laid_out = true;
		v->md_marquee.laid_out = v->md_thumbnail.laid_out = true;
	}
	if (with_images && !v->md_images_for_cursor && g) {
		v->md_images_for_cursor = true;
		elem_set_path(&v->md_image, gpath(g->image));
		elem_set_path(&v->md_video, gpath(g->image));
		elem_set_path(&v->md_marquee, gpath(g->marquee));
		elem_set_path(&v->md_thumbnail, gpath(g->thumbnail));
		ui->dirty = true;
	}
}

static void draw_desc(struct ui *ui, struct glview *v, struct gfx_surface *s)
{
	struct elem *e = &v->desc;
	struct game *g = cur_game(v);
	int bw = (int)lroundf(e->size[0] * (float)ui->w);
	int bh = (int)lroundf(e->size[1] * (float)ui->h);
	int x = (int)lroundf(e->pos[0] * (float)ui->w - e->origin[0] * (float)bw);
	int y = (int)lroundf(e->pos[1] * (float)ui->h - e->origin[1] * (float)bh);

	if (!e->visible || !g || !g->desc || bw <= 0)
		return;
	if (!v->desc_img) {
		struct font *f = font_get(e->font_path, ui_font_px(ui, e->font_size));
		char *up = NULL;
		const char *txt = g->desc;
		int th;

		if (e->upper) {
			up = xmalloc(strlen(txt) * 2 + 8);
			utf8_upper(txt, up, strlen(txt) * 2 + 8);
			txt = up;
		}
		v->desc_img = render_text_image(f, txt, bw, 64, e->align, e->line_spacing, e->fg,
						NULL, &th);
		free(up);
	}
	if (bh <= 0)
		bh = v->desc_img->h;
	{
		int sy = MIN(v->desc_scroll, MAX(0, v->desc_img->h - bh));
		int hh = MIN(bh, v->desc_img->h - sy);
		int ox = e->align == AL_CENTER ? (bw - v->desc_img->w) / 2 :
			 e->align == AL_RIGHT ? bw - v->desc_img->w : 0;

		gfx_blit_sub(s, v->desc_img, (struct gfx_rect){ 0, sy, v->desc_img->w, hh },
			     x + MAX(0, ox), y, 255);
	}
}

/* ----------------------------------------------------------------- list */
static void draw_list(struct ui *ui, struct glview *v, struct gfx_surface *s)
{
	struct textlist *tl = &v->tl;
	int n = v->names ? v->nnames : 0;
	int screen_count, start = 0, cut;
	struct gfx_rect old;
	int fpx;

	(void)ui;
	if (!n) {
		/* TRANSLATORS: an empty game list (one line, uppercase in some themes) */
		draw_text_box(ui, s, tl->font, v->search[0] ? _("No game matches this search") : _("no games"),
			      tl->x, tl->y, tl->w, (int)tl->entry, AL_CENTER, tl->primary);
		return;
	}
	screen_count = (int)((float)tl->h / tl->entry + 0.5f);
	screen_count = MAX(screen_count, 1);
	if (n >= screen_count) {
		start = v->cursor - screen_count / 2;
		if (start < 0)
			start = 0;
		if (start >= n - screen_count)
			start = n - screen_count;
	}
	cut = MIN(start + screen_count, n);
	/* selector */
	{
		int sy = tl->y + (int)lroundf((float)(v->cursor - start) * tl->entry) + tl->sel_off;

		if (tl->sel_img) {
			if (tl->sel_tile)
				gfx_blit_tiled(s, tl->sel_img, tl->x, sy, tl->w, tl->sel_h, 255);
			else
				gfx_blit(s, tl->sel_img, tl->x, sy, 255);
		} else {
			gfx_fill_gradient(s, tl->x, sy, tl->w, tl->sel_h, tl->selector,
					  tl->selector_end, tl->sel_grad_h);
		}
	}
	old = gfx_clip_push(s, (struct gfx_rect){ tl->x + tl->margin, tl->y,
						  tl->w - 2 * tl->margin, tl->h });
	fpx = font_height(tl->font);
	for (int i = start; i < cut; i++) {
		const char *name = v->names[i];
		int tw = font_text_width(tl->font, name, -1);
		int row = tl->y + (int)lroundf((float)(i - start) * tl->entry);
		/* ES: the text cache is built with lineSpacing 1.5 */
		int base = row + ((int)lroundf((float)fpx * 1.5f) + font_cap_height(tl->font)) / 2;
		int x;
		gfx_color c = tl->primary;

		if (i == v->cursor && (tl->selected & 0x00ffffffu || (tl->selected >> 24)))
			c = tl->selected;
		switch (tl->align) {
		case AL_CENTER:
			x = tl->x + (tl->w - tw) / 2;
			break;
		case AL_RIGHT:
			x = tl->x + tl->w - tw - tl->margin;
			break;
		default:
			x = tl->x + tl->margin;
		}
		font_draw(s, tl->font, x, base, name, -1, c);
	}
	gfx_clip_pop(s, old);
}

static void free_all(struct glview *v);

/* --------------------------------------------------------------- render */
static void ensure_layout(struct ui *ui, struct glview *v)
{
	/* the language changed (Settings > Language): rebuild every text,
	 * keeping the cursor */
	if (v->laid_out && v->i18n_gen != i18n_generation()) {
		free_all(v);
		v->md_images_for_cursor = false;
	}
	if (!v->laid_out) {
		build(ui, v);
		make_names(v);
	}
	if (!v->tl.font)
		textlist_layout(ui, &v->tl);
	if (!v->backdrop)
		build_backdrop(ui, v);
}

static void gv_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct glview *v = GV(scr);
	/* TRANSLATORS: help bar labels (uppercase, short: ~10 characters) */
	static const struct help_prompt prompts[] = {
		{ "updown", N_("choose") }, { "a", N_("launch") }, { "b", N_("back") },
		/* TRANSLATORS: help bar labels (uppercase, short: ~10 characters) */
		{ "select", N_("options") }, { "x", N_("favorite") }, { "start", N_("menu") },
	};

	ensure_layout(ui, v);
	set_md(ui, v, v->md_images_for_cursor);
	gfx_blit(s, v->backdrop, 0, 0, 255);
	for (int i = v->first_live; i < v->nlayers; i++) {
		struct layer *l = &v->layers[i];

		if (l->is_list)
			draw_list(ui, v, s);
		else if (l->is_desc)
			draw_desc(ui, v, s);
		else if (!hides_header(v, l->e))
			elem_draw(ui, s, l->e, 0, 0);
	}
	help_draw(ui, s, &v->help, prompts, 6);
	/* a search in progress: what is searched and how many match, in the
	 * place of the list's title (the theme's font and color for it), or at
	 * the top of the screen when the theme has no title (B clears it) */
	if (v->search[0]) {
		const struct menu_style *ms = &ui->ms;
		char text[192], count[32];
		int hx, hy, hw, hh;

		i18n_format_number(v->nvis, 0, count, sizeof(count));
		/* TRANSLATORS: a game list filtered by a search: the text searched,
		 * then the number of games found (French: « Recherche : mario (3) ») */
		snprintf(text, sizeof(text), _("Search: %s (%s)"), v->search, count);
		if (header_rect(ui, v, &hx, &hy, &hw, &hh)) {
			const struct elem *lt = &v->logotext;
			bool own = lt->visible && lt->font_path[0];
			float frac = own && lt->font_size > 0.0f ? lt->font_size : 0.05f;
			int px = MIN(ui_font_px(ui, frac), hh);
			struct font *f = font_get(own ? lt->font_path : ms->font_path, MAX(px, ui_font_px(ui, 0.03f)));

			draw_text_box(ui, s, f, text, hx, hy, hw, hh, AL_CENTER,
				      own ? (lt->fg | 0xff000000u) : 0xfff0f0f0u);
		} else {
			struct font *f = font_get(ms->font_path, ui_font_px(ui, 0.04f));
			int w, h = font_height(f) * 2, pad = h / 2, x;

			w = MIN(ui->w - 20, font_text_width(f, text, -1) + 2 * pad);
			x = (ui->w - w) / 2;
			gfx_fill_round(s, x, ui->h / 60, w, h, h / 2, 0xe0202226u);
			draw_text_box(ui, s, f, text, x + pad, ui->h / 60, w - 2 * pad, h, AL_CENTER, 0xfff0f0f0u);
		}
	}
	/* jump to letter: the letter reached, big, for a moment */
	if (v->jump[0] && v->jump_until && ui->now < v->jump_until && !ui->in_snapshot) {
		const struct menu_style *ms = &ui->ms;
		struct font *f = font_get(ms->font_bold[0] ? ms->font_bold : ms->font_path, ui_font_px(ui, 0.2f));
		int sz = ui->h * 32 / 100;
		/* over the middle of the list (the right side in a detailed view) */
		int cx = v->tl.w > sz ? v->tl.x + v->tl.w / 2 : ui->w / 2;
		int cy = v->tl.h > sz ? v->tl.y + v->tl.h / 2 : ui->h / 2;
		int lx = CLAMP(cx - sz / 2, 0, ui->w - sz), ly = CLAMP(cy - sz / 2, 0, ui->h - sz);

		gfx_fill_round(s, lx, ly, sz, sz, sz / 6, 0xd8202226u);
		draw_text_box(ui, s, f, v->jump, lx, ly, sz, sz, AL_CENTER, (ms->accent & 0x00ffffffu) | 0xff000000u);
	}
}

/* -------------------------------------------------------------- input */
static void move_to(struct ui *ui, struct glview *v, int c, bool now)
{
	if (!v->nvis)
		return;
	v->cursor = CLAMP(c, 0, v->nvis - 1);
	v->moved_at = ui->now;
	v->jump[0] = 0;               /* (jump_letter sets it again) */
	v->jump_until = 0;
	set_md(ui, v, now);
	ui->dirty = true;
}

/* The letter group of a row (games_letter; the favorites at the top of a
 * "favorites first" list are one group, "★"). */
static void row_group(struct ui *ui, struct glview *v, int row, char *out, size_t n)
{
	const struct game *g = row_game(v, row);

	if (!g) {
		snprintf(out, n, "?");
		return;
	}
	if (g->favorite && settings_get_bool(ui->settings, "favorites_first", true) && !v->se->is_collection) {
		snprintf(out, n, "\xe2\x98\x85");
		return;
	}
	games_letter(g->name, out, n);
}

/*
 * L1 / R1 in a list sorted by name: the previous / next letter group (ES:
 * the first game of the next letter; back: the start of this letter, or of
 * the previous one when already there), wrapping around. The letter is shown
 * big for a moment. In the other sort orders they page, as L2 / R2.
 */
static void jump_letter(struct ui *ui, struct glview *v, int dir)
{
	char cur[8], g[8];
	int c = v->cursor, n = v->nvis;

	if (n < 2)
		return;
	row_group(ui, v, c, cur, sizeof(cur));
	if (dir > 0) {
		int i = c + 1;

		while (i < n && (row_group(ui, v, i, g, sizeof(g)), !strcmp(g, cur)))
			i++;
		c = i < n ? i : 0;
	} else {
		int start = c;

		while (start > 0 && (row_group(ui, v, start - 1, g, sizeof(g)), !strcmp(g, cur)))
			start--;
		if (start < c) {
			c = start;
		} else {
			/* already at the start of the group: the previous group's start */
			int i = start > 0 ? start - 1 : n - 1;
			char prev[8];

			row_group(ui, v, i, prev, sizeof(prev));
			while (i > 0 && (row_group(ui, v, i - 1, g, sizeof(g)), !strcmp(g, prev)))
				i--;
			c = i;
		}
	}
	move_to(ui, v, c, false);
	row_group(ui, v, c, v->jump, sizeof(v->jump));
	v->jump_until = ui->now + JUMP_SHOW_MS;
}

static bool sorted_by_name(struct ui *ui, struct glview *v)
{
	/* the collections have their own order (favorites by name: yes) */
	if (v->se->is_collection)
		return v->se->sidx == -1 || v->owns_se;
	return !strcmp(settings_get(ui->settings, "gamelist_sort", "name"), "name");
}

static void switch_system(struct ui *ui, struct glview *v, int d)
{
	int n = ui->nsys, idx = -1;

	for (int i = 0; i < n; i++)
		if (&ui->sys[i] == v->se)
			idx = i;
	if (idx < 0 || n < 2)
		return;
	idx = ((idx + d) % n + n) % n;
	ui->sys_cursor = idx;
	ui_pop(ui);        /* destroys v */
	/* keep the carousel in sync, then open the new list (loaded now if
	 * it has not arrived yet; that may rebuild the carousel) */
	if (ui_top(ui))
		sysview_jump(ui, ui_top(ui), idx);
	{
		struct sysent *se = ui_system_ready(ui, &ui->sys[idx]);

		if (se)
			ui_push(ui, glview_create(ui, se));
	}
}

static void gv_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct glview *v = GV(scr);
	int n = v->nvis;
	int page = MAX(1, (int)((float)v->tl.h / MAX(1.0f, v->tl.entry) + 0.5f));
	bool rep = t == IN_NAV_REPEAT;

	if (t == IN_NAV_RELEASE)
		return;
	switch (b) {
	case IN_UP:
		if (n)
			move_to(ui, v, v->cursor > 0 ? v->cursor - 1 : rep ? 0 : n - 1, !rep);
		break;
	case IN_DOWN:
		if (n)
			move_to(ui, v, v->cursor < n - 1 ? v->cursor + 1 : rep ? n - 1 : 0, !rep);
		break;
	case IN_L:
		if (n && sorted_by_name(ui, v)) {
			jump_letter(ui, v, -1);
			break;
		}
		/* other orders: page up */
		__attribute__((fallthrough));
	case IN_L2:
		if (n)
			move_to(ui, v, MAX(0, v->cursor - page), !rep);
		break;
	case IN_R:
		if (n && sorted_by_name(ui, v)) {
			jump_letter(ui, v, 1);
			break;
		}
		/* other orders: page down */
		__attribute__((fallthrough));
	case IN_R2:
		if (n)
			move_to(ui, v, MIN(n - 1, v->cursor + page), !rep);
		break;
	case IN_LEFT:
	case IN_RIGHT:
		if (t == IN_NAV_PRESS)
			switch_system(ui, v, b == IN_LEFT ? -1 : 1);
		break;
	case IN_A:
		if (t == IN_NAV_PRESS && cur_game(v))
			ui_launch_game(ui, v->se, cur_game(v));
		break;
	case IN_B:
		/* B clears a search first, then leaves */
		if (t == IN_NAV_PRESS && v->search[0])
			glview_set_search(ui, scr, "");
		else if (t == IN_NAV_PRESS)
			ui_pop(ui);
		break;
	case IN_X:
		if (t == IN_NAV_PRESS && cur_game(v)) {
			struct game *g = cur_game(v);

			gamedb_set_favorite(ui->db, g, !g->favorite);
			gamedb_save(ui->db);
			/* TRANSLATORS: toast after X in a game list */
			ui_toastf(ui, "%s", g->favorite ? _("Added to favorites") :
				  _("Removed from favorites"));
			glview_refresh(ui, scr);
		}
		break;
	case IN_SELECT:
		if (t == IN_NAV_PRESS && cur_game(v))
			game_options_open(ui, v->se, cur_game(v));
		break;
	case IN_START:
		if (t == IN_NAV_PRESS)
			settings_open_main(ui);
		break;
	default:
		break;
	}
}

static bool gv_update(struct ui *ui, struct screen *scr)
{
	struct glview *v = GV(scr);
	bool redraw = false;

	if (!v->laid_out)
		return false;
	if (v->jump_until && ui->now >= v->jump_until) {
		v->jump_until = 0;       /* the big letter goes (jump[] stays: tests) */
		redraw = true;
	}
	if (v->detailed && !v->md_images_for_cursor && ui->now - v->moved_at >= MD_DELAY_MS) {
		set_md(ui, v, true);
		redraw = true;
	}
	/* slow auto-scroll of long descriptions (like ES) */
	if (v->desc_img && v->desc.visible) {
		int bh = (int)lroundf(v->desc.size[1] * (float)ui->h);
		int over = v->desc_img->h - bh;

		if (bh > 0 && over > 0 && ui->now - v->desc_t0 > DESC_SCROLL_DELAY) {
			int64_t dt = ui->now - v->desc_t0 - DESC_SCROLL_DELAY;
			int s = (int)(dt * DESC_SCROLL_PX_S / 1000);

			if (s > over + DESC_SCROLL_PX_S * 3) {
				v->desc_t0 = ui->now;
				s = 0;
			}
			s = MIN(s, over);
			if (s != v->desc_scroll) {
				v->desc_scroll = s;
				redraw = true;
			}
		}
	}
	return redraw;
}

static int gv_timeout(struct ui *ui, struct screen *scr)
{
	struct glview *v = GV(scr);
	int t = -1;

	if (v->detailed && !v->md_images_for_cursor)
		t = (int)MAX(0, MD_DELAY_MS - (ui->now - v->moved_at));
	if (v->jump_until) {
		int j = (int)MAX(0, v->jump_until - ui->now);

		t = t < 0 ? j : MIN(t, j);
	}
	if (v->desc_img && v->desc.visible &&
	    v->desc_img->h > (int)lroundf(v->desc.size[1] * (float)ui->h)) {
		int64_t since = ui->now - v->desc_t0;
		int d = since < DESC_SCROLL_DELAY ? (int)(DESC_SCROLL_DELAY - since) :
			1000 / DESC_SCROLL_PX_S;

		t = t < 0 ? d : MIN(t, d);
	}
	return t;
}

static void release_all(struct glview *v)
{
	elem_release(&v->background);
	elem_release(&v->logo);
	elem_release(&v->logotext);
	for (int i = 0; i < v->nextras; i++)
		elem_release(&v->extras[i]);
	if (v->detailed) {
		elem_release(&v->md_image);
		elem_release(&v->md_video);
		elem_release(&v->md_marquee);
		elem_release(&v->md_thumbnail);
		elem_release(&v->md_favorite);
		for (int i = 0; i < NLBL; i++) {
			elem_release(&v->lbl[i]);
			elem_release(&v->val[i]);
		}
		elem_release(&v->desc);
		elem_release(&v->name);
		elem_release(&v->pt_lbl);
		elem_release(&v->pt_val);
	}
	img_put(v->tl.sel_img);
	v->tl.sel_img = NULL;
	v->tl.font = NULL;
	gfx_image_free(v->backdrop);
	v->backdrop = NULL;
	gfx_image_free(v->desc_img);
	v->desc_img = NULL;
}

static void gv_relayout(struct ui *ui, struct screen *scr)
{
	struct glview *v = GV(scr);

	(void)ui;
	release_all(v);
	v->md_for = -1;
	v->md_images_for_cursor = false;
}

static void free_all(struct glview *v)
{
	release_all(v);
	elem_free(&v->background);
	elem_free(&v->logo);
	elem_free(&v->logotext);
	for (int i = 0; i < v->nextras; i++)
		elem_free(&v->extras[i]);
	free(v->extras);
	v->extras = NULL;
	v->nextras = 0;
	if (v->detailed) {
		elem_free(&v->md_image);
		elem_free(&v->md_video);
		elem_free(&v->md_marquee);
		elem_free(&v->md_thumbnail);
		elem_free(&v->md_favorite);
		for (int i = 0; i < NLBL; i++) {
			elem_free(&v->lbl[i]);
			elem_free(&v->val[i]);
		}
		elem_free(&v->desc);
		elem_free(&v->name);
		elem_free(&v->pt_lbl);
		elem_free(&v->pt_val);
	}
	free(v->layers);
	v->layers = NULL;
	free_names(v);
	v->laid_out = false;
}

static void free_owned_se(struct glview *v)
{
	struct gamelist *gl;

	if (!v->owns_se || !v->se)
		return;
	gl = v->se->games;
	if (gl) {
		free(gl->games);
		arena_free(&gl->arena);
		free(gl);
	}
	theme_free(v->se->theme);
	free(v->se);
	v->se = NULL;
}

static void gv_destroy(struct ui *ui, struct screen *scr)
{
	struct glview *v = GV(scr);

	(void)ui;
	free_all(v);
	free(v->vis);
	free_owned_se(v);
	free(v);
}

static const struct screen_ops gv_ops = {
	.button = gv_button,
	.update = gv_update,
	.render = gv_render,
	.relayout = gv_relayout,
	.destroy = gv_destroy,
	.timeout = gv_timeout,
	.opaque = true,
};

struct screen *glview_create(struct ui *ui, struct sysent *se)
{
	struct glview *v = xcalloc(1, sizeof(*v));

	(void)ui;
	v->base.ops = &gv_ops;
	v->base.kind = SCR_GLVIEW;
	v->se = se;
	v->md_for = -1;
	v->moved_at = -1000000;
	return &v->base;
}

struct sysent *glview_system(struct screen *s)
{
	return GV(s)->se;
}

/* After the rows changed: the cursor on the game at `keep` (a path) if it
 * is still shown, else on row `fallback`. */
static void cursor_to(struct ui *ui, struct glview *v, const char *keep, int fallback)
{
	v->cursor = CLAMP(fallback, 0, MAX(0, v->nvis - 1));
	for (int i = 0; keep && i < v->nvis; i++)
		if (!strcmp(row_game(v, i)->path, keep))
			v->cursor = i;
	v->md_for = -1;
	v->md_images_for_cursor = false;
	v->moved_at = ui->now - MD_DELAY_MS;
	ui->dirty = true;
}

/* The game list or the theme changed: rebuild, keeping the selected game. */
void glview_refresh(struct ui *ui, struct screen *scr)
{
	struct glview *v = GV(scr);
	const char *keep = NULL;
	char path[1024] = "";
	int row = v->cursor;

	if (cur_game(v)) {
		strlcpy_(path, cur_game(v)->path, sizeof(path));
		keep = path;
	}
	free_all(v);
	/* the collections keep their own order (loader.c: favorites by name,
	 * last played by date) */
	if (!v->se->is_collection || v->owns_se)
		ui_sort_games(ui, v->se->games);
	build_vis(v);
	cursor_to(ui, v, keep, row);
}

/* The search (the list's options > Search): only the games whose name
 * contains text (case and accents ignored); "" shows them all again. The
 * cursor stays on its game when it still matches. */
void glview_set_search(struct ui *ui, struct screen *scr, const char *text)
{
	struct glview *v = GV(scr);
	char path[1024] = "";
	const char *keep = NULL;

	if (!strcmp(v->search, text ? text : ""))
		return;
	if (cur_game(v)) {
		strlcpy_(path, cur_game(v)->path, sizeof(path));
		keep = path;
	}
	if (!v->search[0] != !(text && *text)) {
		/* the search takes the header's place (or gives it back): the
		 * backdrop is made again with or without the list's title */
		gfx_image_free(v->backdrop);
		v->backdrop = NULL;
	}
	strlcpy_(v->search, text ? text : "", sizeof(v->search));
	free_names(v);
	if (v->laid_out)
		make_names(v);
	else
		build_vis(v);
	cursor_to(ui, v, keep, 0);
	LOGI("ui: %s: search \"%s\": %d of %d games", v->se->name, v->search, v->nvis,
	     v->se->games ? v->se->games->n : 0);
	ui_invalidate_snapshot(ui);
}

const char *glview_search(struct screen *scr)
{
	return GV(scr)->search;
}

/* Tests (ui_debug_screen): " search=<text>(<rows>)", the game under the
 * cursor and the letter shown by a jump. */
void glview_describe(struct screen *scr, char *buf, size_t n)
{
	struct glview *v = GV(scr);
	const struct game *g = cur_game(v);
	size_t l = 0;

	buf[0] = 0;
	if (v->search[0])
		l += (size_t)snprintf(buf + l, n - l, " search=%s(%d)", v->search, v->nvis);
	if (l < n && g)
		l += (size_t)snprintf(buf + l, n - l, " game=%s", g->name);
	if (l < n && v->jump[0])
		snprintf(buf + l, n - l, " letter=%s", v->jump);
}

/*
 * A game left the lists (hidden, deleted): out of its system's list, the
 * collections and every open list view (the "Search all games" results own
 * copies), with the carousel counts updated. The views keep their cursor
 * row.
 */
void glview_game_removed(struct ui *ui, const char *system, const char *path)
{
	for (int i = 0; i < ui->nsys; i++) {
		struct sysent *se = &ui->sys[i];

		if (se->games && games_remove(se->games, path)) {
			se->count = se->games->n;
			se->nfav = 0;
			for (int k = 0; k < se->games->n; k++)
				se->nfav += se->games->games[k].favorite;
		}
	}
	for (int i = 0; i < ui->nstack; i++) {
		struct screen *s = ui->stack[i];

		if (s->kind != SCR_GLVIEW)
			continue;
		if (GV(s)->owns_se)
			games_remove(GV(s)->se->games, path);
		glview_refresh(ui, s);
	}
	for (int i = 0; i < ui->nstack; i++)
		if (ui->stack[i]->kind == SCR_SYSVIEW)
			sysview_refresh_info(ui, ui->stack[i]);
	LOGI("ui: %s/%s left the lists", system, path_basename(path));
	ui_invalidate_snapshot(ui);
}

/*
 * "Search all games" (Settings): a list of every game, in every system,
 * whose name matches text, as a collection view of its own (the Carbon
 * "all games" art); its entry and list belong to the view. NULL if nothing
 * matches.
 */
struct screen *glview_create_search_all(struct ui *ui, const char *text)
{
	struct glview *v;
	struct sysent *se;
	struct gamelist *gl;
	int n = 0;

	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *l = ui->sys[i].games;

		for (int k = 0; !ui->sys[i].is_collection && l && k < l->n; k++)
			n += games_match(l->games[k].name, text);
	}
	LOGI("ui: search all games \"%s\": %d found", text, n);
	if (!n)
		return NULL;
	gl = xcalloc(1, sizeof(*gl));
	arena_init(&gl->arena, 1024);
	strlcpy_(gl->system, "search", sizeof(gl->system));
	gl->games = xcalloc((size_t)n, sizeof(struct game));
	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *l = ui->sys[i].games;

		for (int k = 0; !ui->sys[i].is_collection && l && k < l->n; k++)
			if (games_match(l->games[k].name, text)) {
				gl->games[gl->n++] = l->games[k];   /* strings stay in the list's arena */
				gl->has_media |= l->games[k].image || l->games[k].desc;
			}
	}
	games_sort(gl, false);
	se = xcalloc(1, sizeof(*se));
	se->sidx = -3;
	strlcpy_(se->name, "search", sizeof(se->name));
	/* TRANSLATORS: header of the "Search all games" results; %s is the text searched */
	snprintf(se->fullname, sizeof(se->fullname), _("Search: %s"), text);
	se->theme_alias[0] = "auto-allgames";
	se->theme_alias[1] = "all";
	se->is_collection = true;
	se->games = gl;
	se->count = gl->n;
	v = (struct glview *)glview_create(ui, se);
	v->owns_se = true;
	return &v->base;
}
