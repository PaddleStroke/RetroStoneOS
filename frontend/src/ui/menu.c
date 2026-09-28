/*
 * menu.c - the generic list menu used by every settings screen, message
 * and confirmation dialogs, and the on-screen keyboard. Colors and fonts
 * come from the theme (struct menu_style, see widgets.c).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_internal.h"

/* ----------------------------------------------------------- geometry */
struct panel_geo {
	int x, y, w, h;
	int title_h, row_h, pad, radius;
	int rows;               /* visible rows */
	struct font *f, *fb, *fs;
};

static void panel_geo(struct ui *ui, int nrows, struct panel_geo *g)
{
	const struct menu_style *ms = &ui->ms;
	int maxh;

	g->f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	g->fb = font_get(ms->font_bold[0] ? ms->font_bold : ms->font_path,
			 ui_font_px(ui, ms->font_size * 1.1f));
	g->fs = font_get(ms->font_path, ui_font_px(ui, ms->small_size));
	g->row_h = (int)lroundf((float)font_height(g->f) * 1.5f);
	g->title_h = (int)lroundf((float)font_height(g->fb) * 2.0f);
	g->pad = MAX(6, ui->h / 60);
	g->radius = MAX(4, ui->h / 60);
	g->w = MIN((int)lroundf((float)ui->w * 0.9f), (int)lroundf((float)ui->h * 1.25f));
	maxh = (int)lroundf((float)ui->h * 0.86f);
	g->rows = MAX(1, MIN(nrows, (maxh - g->title_h - 2 * g->pad) / g->row_h));
	g->h = g->title_h + g->rows * g->row_h + 2 * g->pad;
	g->x = (ui->w - g->w) / 2;
	g->y = (int)lroundf((float)(ui->h - g->h) * 0.42f);
}

static void menu_help(struct ui *ui, struct gfx_surface *s, const struct help_prompt *p, int n)
{
	struct help_style hs = ui->menu_help;

	help_draw(ui, s, &hs, p, n);
}

/* --------------------------------------------------------------- menus */
static const struct screen_ops menu_ops;

static struct menu *M(struct screen *s)
{
	return (struct menu *)s;
}

static bool selectable(const struct menu_item *it)
{
	return it->type != MI_HEADER && !(it->type == MI_INFO && it->disabled);
}

static void clamp_cursor(struct menu *m, int dir)
{
	if (!m->n)
		return;
	m->cursor = CLAMP(m->cursor, 0, m->n - 1);
	for (int k = 0; k < m->n && !selectable(&m->items[m->cursor]); k++)
		m->cursor = ((m->cursor + (dir >= 0 ? 1 : -1)) % m->n + m->n) % m->n;
}

struct menu_item *menu_find(struct menu *m, int id)
{
	for (int i = 0; i < m->n; i++)
		if (m->items[i].id == id)
			return &m->items[i];
	return NULL;
}

static void draw_toggle(struct gfx_surface *s, int x, int y, int h, bool on,
			const struct menu_style *ms, bool sel)
{
	int w = h * 2;
	gfx_color track = on ? (sel ? ms->sel_text : ms->accent) : gfx_with_alpha(ms->text_dim, 110);
	gfx_color knob = on && sel ? ms->selector : 0xffffffffu;

	gfx_fill_round(s, x, y, w, h, h / 2, track | 0xff000000u);
	gfx_fill_circle(s, on ? x + w - h / 2 : x + h / 2, y + h / 2, h / 2 - MAX(2, h / 8),
			knob | 0xff000000u);
}

static void m_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct menu *m = M(scr);
	const struct menu_style *ms = &ui->ms;
	struct panel_geo g;
	int y;
	/* TRANSLATORS: help bar at the bottom of the screen: one short word each */
	static const struct help_prompt prompts[] = {
		{ "updown", N_("choose") }, { "leftright", N_("change") }, { "a", N_("select") },
		{ "b", N_("back") },
	};

	panel_geo(ui, m->n, &g);
	if (m->cursor < m->top)
		m->top = m->cursor;
	if (m->cursor >= m->top + g.rows)
		m->top = m->cursor - g.rows + 1;
	m->top = CLAMP(m->top, 0, MAX(0, m->n - g.rows));

	draw_panel(s, g.x, g.y, g.w, g.h, g.radius, ms->bg);
	draw_text_box(ui, s, g.fb, m->title, g.x + g.pad, g.y, g.w - 2 * g.pad, g.title_h,
		      AL_CENTER, ms->title);
	gfx_fill(s, g.x + g.pad, g.y + g.title_h - 1, g.w - 2 * g.pad, MAX(1, ui->h / 240),
		 ms->separator);
	y = g.y + g.title_h + g.pad;
	for (int i = m->top; i < m->n && i < m->top + g.rows; i++, y += g.row_h) {
		struct menu_item *it = &m->items[i];
		bool sel = i == m->cursor;
		gfx_color tc = it->disabled ? ms->text_dim : sel ? ms->sel_text : ms->text;
		gfx_color vc = sel ? ms->sel_text : ms->text_dim;
		int lx = g.x + g.pad * 2, rx = g.x + g.w - g.pad * 2;
		int vw = 0;
		char buf[128];

		if (sel)
			gfx_fill_round(s, g.x + g.pad, y + 2, g.w - 2 * g.pad, g.row_h - 4,
				       g.radius / 2 + 2, ms->selector);
		if (it->type == MI_HEADER) {
			char up[96];

			utf8_upper(it->label, up, sizeof(up));
			draw_text_box(ui, s, g.fs, up, lx, y + g.row_h / 4, g.w - 4 * g.pad,
				      g.row_h * 3 / 4, AL_LEFT, ms->accent);
			continue;
		}
		switch (it->type) {
		case MI_SUBMENU:
			vw = font_text_width(g.f, "\xe2\x80\xba", -1);
			font_draw(s, g.f, rx - vw, font_baseline_in_box(g.f, y, g.row_h),
				  "\xe2\x80\xba", -1, vc);
			vw += g.pad;
			if (it->value[0]) {
				int w2 = font_text_width(g.fs, it->value, -1);

				font_draw(s, g.fs, rx - vw - w2, font_baseline_in_box(g.fs, y, g.row_h),
					  it->value, -1, vc);
				vw += w2 + g.pad;
			}
			break;
		case MI_TOGGLE: {
			int th = g.row_h / 2;

			draw_toggle(s, rx - th * 2, y + (g.row_h - th) / 2, th, it->on, ms, sel);
			vw = th * 2 + g.pad;
			break;
		}
		case MI_NUMBER:
			snprintf(it->value, sizeof(it->value), it->max >= 1000 ? "%d" : "%02d", it->val);
			/* fall through */
		case MI_CHOICE:
			if (sel)
				snprintf(buf, sizeof(buf), "\xe2\x80\xb9 %s \xe2\x80\xba", it->value);
			else
				snprintf(buf, sizeof(buf), "%s", it->value);
			vw = font_text_width(g.f, buf, -1);
			font_draw(s, g.f, rx - vw, font_baseline_in_box(g.f, y, g.row_h), buf, -1, vc);
			vw += g.pad;
			break;
		case MI_SLIDER: {
			int bw = g.w / 3, bh = MAX(4, g.row_h / 7);
			int bx = rx - bw, by = y + (g.row_h - bh) / 2;
			int fill = it->max > it->min ?
				(bw * (it->val - it->min)) / (it->max - it->min) : 0;

			gfx_fill_round(s, bx, by, bw, bh, bh / 2, gfx_with_alpha(vc, 90));
			gfx_fill_round(s, bx, by, MAX(bh, fill), bh, bh / 2,
				       (sel ? ms->sel_text : ms->accent) | 0xff000000u);
			gfx_fill_circle(s, bx + fill, by + bh / 2, bh * 3 / 2,
					(sel ? ms->sel_text : 0xffffffffu) | 0xff000000u);
			snprintf(buf, sizeof(buf), "%s", it->value);
			vw = bw + g.pad * 2 + font_text_width(g.fs, buf, -1);
			font_draw(s, g.fs, bx - g.pad * 2 - font_text_width(g.fs, buf, -1),
				  font_baseline_in_box(g.fs, y, g.row_h), buf, -1, vc);
			break;
		}
		case MI_INFO:
		case MI_ACTION:
			if (it->value[0]) {
				char el[128];
				int maxw = (g.w - 4 * g.pad) / 2;

				font_ellipsize(g.fs, it->value, maxw, el, sizeof(el));
				vw = font_text_width(g.fs, el, -1);
				font_draw(s, g.fs, rx - vw, font_baseline_in_box(g.fs, y, g.row_h), el,
					  -1, vc);
				vw += g.pad;
			}
			break;
		default:
			break;
		}
		draw_text_box(ui, s, g.f, it->label, lx, y, MAX(10, rx - vw - lx), g.row_h,
			      AL_LEFT, tc);
	}
	/* scrollbar */
	if (m->n > g.rows) {
		int track = g.rows * g.row_h;
		int th = MAX(g.row_h / 2, track * g.rows / m->n);
		int ty = g.y + g.title_h + g.pad + (track - th) * m->top / MAX(1, m->n - g.rows);

		gfx_fill_round(s, g.x + g.w - g.pad + 1, ty, MAX(2, g.pad / 3), th, 2, ms->separator);
	}
	menu_help(ui, s, prompts, 4);
}

static void m_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct menu *m = M(scr);
	struct menu_item *it;

	if (t == IN_NAV_RELEASE || !m->n) {
		if (!m->n && t == IN_NAV_PRESS && (b == IN_B || b == IN_A))
			ui_pop(ui);
		return;
	}
	it = &m->items[m->cursor];
	switch (b) {
	case IN_UP:
		if (m->cursor == 0 && t == IN_NAV_REPEAT)
			break;
		m->cursor = (m->cursor - 1 + m->n) % m->n;
		clamp_cursor(m, -1);
		break;
	case IN_DOWN:
		if (m->cursor == m->n - 1 && t == IN_NAV_REPEAT)
			break;
		m->cursor = (m->cursor + 1) % m->n;
		clamp_cursor(m, 1);
		break;
	case IN_L:
		m->cursor = MAX(0, m->cursor - 5);
		clamp_cursor(m, -1);
		break;
	case IN_R:
		m->cursor = MIN(m->n - 1, m->cursor + 5);
		clamp_cursor(m, 1);
		break;
	case IN_LEFT:
	case IN_RIGHT:
		if (it->disabled)
			break;
		if (it->type == MI_TOGGLE || it->type == MI_CHOICE || it->type == MI_SLIDER ||
		    it->type == MI_NUMBER) {
			int d = b == IN_LEFT ? -1 : 1;

			if (it->type == MI_TOGGLE) {
				if (t != IN_NAV_PRESS)
					break;
				it->on = !it->on;
			} else if (it->type == MI_CHOICE && it->nchoices > 0) {
				it->val = (it->val + d + it->nchoices) % it->nchoices;
				strlcpy_(it->value, it->choices[it->val], sizeof(it->value));
			} else if (it->type == MI_SLIDER) {
				it->val = CLAMP(it->val + d, it->min, it->max);
			} else if (it->type == MI_NUMBER) {
				it->val += d;
				if (it->val > it->max)
					it->val = it->min;
				if (it->val < it->min)
					it->val = it->max;
			}
			if (m->on_item)
				m->on_item(ui, m, it, d);
		}
		break;
	case IN_A:
		if (t != IN_NAV_PRESS || it->disabled)
			break;
		if (it->type == MI_TOGGLE)
			it->on = !it->on;
		else if (it->type == MI_CHOICE && it->nchoices > 0) {
			it->val = (it->val + 1) % it->nchoices;
			strlcpy_(it->value, it->choices[it->val], sizeof(it->value));
		}
		if (m->on_item)
			m->on_item(ui, m, it, 0);
		break;
	case IN_B:
		if (t == IN_NAV_PRESS)
			ui_pop(ui);
		return;
	case IN_START:
		/* START closes the whole menu stack down to the views */
		if (t == IN_NAV_PRESS) {
			while (ui->nstack > 0 && !ui_top(ui)->ops->opaque)
				ui_pop(ui);
		}
		return;
	default:
		return;
	}
	ui->dirty = true;
}

static bool m_update(struct ui *ui, struct screen *scr)
{
	struct menu *m = M(scr);

	if (m->refresh_every && ui->now >= m->next_refresh && m->on_refresh) {
		m->next_refresh = ui->now + m->refresh_every;
		m->on_refresh(ui, m);
		return true;
	}
	return false;
}

static int m_timeout(struct ui *ui, struct screen *scr)
{
	struct menu *m = M(scr);

	if (!m->refresh_every)
		return -1;
	return (int)MAX(0, m->next_refresh - ui->now);
}

static void m_destroy(struct ui *ui, struct screen *scr)
{
	struct menu *m = M(scr);

	if (scr->ops == &menu_ops && m->on_destroy)
		m->on_destroy(ui, m);
	free(scr);
}

static void m_relayout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
}

static void m_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	struct menu *m = M(scr);

	(void)ui;
	snprintf(buf, n, "menu:%s|%s", m->title, m->n ? m->items[m->cursor].label : "");
}

static const struct screen_ops menu_ops = {
	.button = m_button,
	.update = m_update,
	.render = m_render,
	.relayout = m_relayout,
	.destroy = m_destroy,
	.timeout = m_timeout,
	.opaque = false,
	.describe = m_describe,
};

bool screen_is_menu(const struct screen *s)
{
	return s && s->ops == &menu_ops;
}

struct menu *menu_new(struct ui *ui, const char *title, int kind)
{
	struct menu *m = xcalloc(1, sizeof(*m));

	(void)ui;
	m->base.ops = &menu_ops;
	strlcpy_(m->title, title, sizeof(m->title));
	m->kind = kind;
	return m;
}

struct menu_item *menu_add(struct menu *m, enum mi_type t, int id, const char *label)
{
	struct menu_item *it;

	if (m->n >= (int)ARRAY_SIZE(m->items))
		return &m->items[m->n - 1];
	it = &m->items[m->n++];
	memset(it, 0, sizeof(*it));
	it->type = t;
	it->id = id;
	strlcpy_(it->label, label, sizeof(it->label));
	return it;
}

void menu_open(struct ui *ui, struct menu *m)
{
	clamp_cursor(m, 1);
	if (m->on_refresh)
		m->on_refresh(ui, m);
	m->next_refresh = ui->now + m->refresh_every;
	ui_push(ui, &m->base);
}

/* ------------------------------------------------------------- dialogs */
#define DIALOG_MAX_BUTTONS 6

struct dialog {
	struct screen base;
	char text[768];
	/* up to 6 buttons: the USB dialog has 5 with "Install system update" */
	const char *buttons[DIALOG_MAX_BUTTONS];
	char labels[DIALOG_MAX_BUTTONS][64];
	int nbuttons, sel;
	int cols;                     /* button grid, from the last render */
	dialog_fn cb;
	void *user;
	/* dialog_set_countdown(): text[] is re-made from tmpl every second */
	char tmpl[768];
	int countdown_s, timeout_choice, shown_s;
	int64_t deadline;             /* 0: not started (set at the first update) */
};

/* Screen ops are never shared between screen types: every dialog function
 * that can be reached with another screen (the public ones) checks the ops
 * pointer first (review F-C1: the keyboard ran the dialog's update on its
 * much smaller struct). */
static const struct screen_ops dialog_ops;
static const struct screen_ops osk_ops;

bool screen_is_dialog(const struct screen *s)
{
	return s && s->ops == &dialog_ops;
}

bool screen_is_osk(const struct screen *s)
{
	return s && s->ops == &osk_ops;
}

static struct dialog *D(struct screen *s)
{
	return screen_is_dialog(s) ? (struct dialog *)s : NULL;
}

/* Buttons side by side when their labels fit, else one per row (up to 3)
 * or a 2-column grid. */
static int d_cols(struct dialog *d, struct font *f, int avail, int pad)
{
	int widest = 0;

	/* the buttons share the width equally: the widest label decides */
	for (int i = 0; i < d->nbuttons; i++)
		widest = MAX(widest, font_text_width(f, d->labels[i], -1) + 2 * pad);
	if (widest * d->nbuttons + (d->nbuttons - 1) * pad <= avail || d->nbuttons == 1)
		return d->nbuttons;
	return d->nbuttons <= 3 || widest * 2 + pad > avail ? 1 : 2;
}

static void d_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct dialog *d = (struct dialog *)scr;
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	int pad = MAX(8, ui->h / 40);
	int w = MIN((int)lroundf((float)ui->w * 0.8f), (int)lroundf((float)ui->h * 1.1f));
	struct text_line lines[12];
	int n = font_wrap(f, d->text, w - 2 * pad, lines, 12);
	int lh = (int)lroundf((float)font_height(f) * 1.4f);
	int bh = (int)lroundf((float)font_height(f) * 1.9f);
	int cols = d_cols(d, f, w - 2 * pad, pad);
	int rows = (d->nbuttons + cols - 1) / cols;
	int h = pad * 3 + n * lh + rows * bh + (rows - 1) * pad / 2;
	int x = (ui->w - w) / 2, y = (ui->h - h) / 2;
	int bw = (w - 2 * pad - (cols - 1) * pad) / MAX(1, cols);
	static const struct help_prompt prompts[] = {
		{ "leftright", N_("choose") }, { "a", N_("select") }, { "b", N_("back") },
	};
	static const struct help_prompt prompts_v[] = {
		{ "updown", N_("choose") }, { "a", N_("select") }, { "b", N_("back") },
	};

	d->cols = cols;
	draw_panel(s, x, y, w, h, MAX(4, ui->h / 60), ms->bg);
	for (int i = 0; i < n; i++)
		font_draw(s, f, x + (w - lines[i].width) / 2,
			  font_baseline_in_box(f, y + pad + i * lh, lh), d->text + lines[i].start,
			  lines[i].len, ms->text);
	for (int i = 0; i < d->nbuttons; i++) {
		int r = i / cols, c = i % cols;
		int bx = x + pad + c * (bw + pad);
		int by = y + pad * 2 + n * lh + r * (bh + pad / 2);
		bool sel = i == d->sel;

		gfx_fill_round(s, bx, by, bw, bh, bh / 4, sel ? ms->selector : gfx_with_alpha(ms->text_dim, 50));
		draw_text_box(ui, s, f, d->labels[i], bx, by, bw, bh, AL_CENTER,
			      sel ? ms->sel_text : ms->text);
	}
	menu_help(ui, s, cols == 1 && d->nbuttons > 1 ? prompts_v : prompts, d->nbuttons > 1 ? 3 : 2);
}

static void d_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	struct dialog *d = (struct dialog *)scr;

	(void)ui;
	snprintf(buf, n, "dialog:%s|%s", d->text, d->labels[d->sel]);
	for (char *p = buf; *p; p++)
		if (*p == '\n')
			*p = ' ';
}

static void d_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct dialog *d = (struct dialog *)scr;
	dialog_fn cb = d->cb;
	void *user = d->user;
	int cols = d->cols > 0 ? d->cols : d->nbuttons;
	int choice;

	if (t == IN_NAV_RELEASE)
		return;
	if (b == IN_LEFT && d->sel % cols > 0)
		d->sel--;
	else if (b == IN_RIGHT && d->sel % cols < cols - 1 && d->sel < d->nbuttons - 1)
		d->sel++;
	else if (b == IN_UP && d->sel >= cols)
		d->sel -= cols;
	else if (b == IN_DOWN && d->sel + cols < d->nbuttons)
		d->sel += cols;
	else if (b == IN_DOWN && cols > 1 && d->sel / cols < (d->nbuttons - 1) / cols)
		d->sel = d->nbuttons - 1;             /* the last row is shorter */
	else if (t == IN_NAV_PRESS && (b == IN_A || b == IN_B)) {
		choice = b == IN_A ? d->sel : -1;
		ui_pop(ui);           /* frees d */
		if (cb)
			cb(ui, choice, user);
		return;
	}
	ui->dirty = true;
}

/* "%d" in the template = the seconds left. */
static void d_countdown_text(struct dialog *d, int left)
{
	const char *p = strstr(d->tmpl, "%d");
	size_t skip = 2;

	if (!p && (p = strstr(d->tmpl, "%1$d")))   /* a translation may number it */
		skip = 4;
	d->shown_s = left;
	if (!p) {
		strlcpy_(d->text, d->tmpl, sizeof(d->text));
		return;
	}
	snprintf(d->text, sizeof(d->text), "%.*s%d%s", (int)(p - d->tmpl), d->tmpl, left, p + skip);
}

static bool d_update(struct ui *ui, struct screen *s)
{
	struct dialog *d = D(s);
	int left;

	if (!d || !d->countdown_s)
		return false;
	if (!d->deadline)
		d->deadline = ui->now + (int64_t)d->countdown_s * 1000;
	if (ui->now >= d->deadline) {
		dialog_fn cb = d->cb;
		void *user = d->user;
		int choice = d->timeout_choice;

		ui_pop(ui);           /* frees d */
		if (cb)
			cb(ui, choice, user);
		return true;
	}
	left = (int)((d->deadline - ui->now + 999) / 1000);
	if (left != d->shown_s) {
		d_countdown_text(d, left);
		return true;
	}
	return false;
}

static int d_timeout(struct ui *ui, struct screen *s)
{
	struct dialog *d = D(s);
	int64_t rest;

	if (!d || !d->countdown_s)
		return -1;
	if (!d->deadline)
		return 0;
	rest = d->deadline - ui->now;
	if (rest <= 0)
		return 0;
	/* wake at the next whole second (the number changes) or at the end */
	return (int)(rest % 1000 ? rest % 1000 : 1000);
}

void dialog_set_countdown(struct screen *s, int seconds, int timeout_choice)
{
	struct dialog *d = D(s);

	if (!d || seconds <= 0)
		return;
	strlcpy_(d->tmpl, d->text, sizeof(d->tmpl));
	d->countdown_s = seconds;
	d->timeout_choice = timeout_choice;
	d->deadline = 0;
	d_countdown_text(d, seconds);
}

static const struct screen_ops dialog_ops = {
	.button = d_button,
	.update = d_update,
	.render = d_render,
	.relayout = m_relayout,
	.destroy = m_destroy,
	.timeout = d_timeout,
	.opaque = false,
	.describe = d_describe,
};

struct screen *dialog_open(struct ui *ui, const char *text, const char *const *buttons,
			    dialog_fn cb, void *user)
{
	struct dialog *d = xcalloc(1, sizeof(*d));

	d->base.ops = &dialog_ops;
	strlcpy_(d->text, text, sizeof(d->text));
	for (int i = 0; buttons && buttons[i] && i < DIALOG_MAX_BUTTONS; i++) {
		strlcpy_(d->labels[i], buttons[i], sizeof(d->labels[i]));
		d->nbuttons++;
	}
	if (!d->nbuttons) {
		/* TRANSLATORS: dialog button, uppercase, short */
		strlcpy_(d->labels[0], _("OK"), sizeof(d->labels[0]));
		d->nbuttons = 1;
	}
	d->cb = cb;
	d->user = user;
	if (!ui_push(ui, &d->base))
		return NULL;          /* stack full: d is freed (review F-M3) */
	return &d->base;
}

void dialog_select(struct screen *s, int button)
{
	struct dialog *d = D(s);

	if (d && button >= 0 && button < d->nbuttons)
		d->sel = button;
}

void message_open(struct ui *ui, const char *text)
{
	dialog_open(ui, text, NULL, NULL, NULL);
}

/* ------------------------------------------------------ on-screen keyboard */
/*
 * Pages: letters (qwerty; SHIFT or Y = capitals), symbols (#+=) and two
 * pages of accented letters (àé, or R: French, German, Spanish, Portuguese,
 * Italian, Nordic; then Polish, Czech, Turkish, Romanian...), for names and
 * searches. A password field keeps the ASCII pages (WiFi passphrases).
 */
#define OSK_ROWS 5
#define OSK_COLS 10
#define OSK_MAX_CHARS 63

enum { PG_LETTERS = 0, PG_SYMBOLS, PG_ACCENTS1, PG_ACCENTS2, PG_COUNT };

static const char *const osk_lower[4] = { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" };
static const char *const osk_upper[4] = { "!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:_" };
static const char *const osk_symbols[4] = { "1234567890", "+=/\\|<>[]{", "}~`?#$%&*@", "!^_;:\"()'," };
/* 10 characters (UTF-8) per row; SHIFT gives the capitals (utf8_upper) */
static const char *const osk_accents[2][4] = {
	{ "àáâäãåçèéê", "ëìíîïñòóôö", "õøùúûüýÿæœ", "ß¿¡«»€£°©·" },
	{ "ąćęłńśźżőű", "čďěňřšťůžľ", "ğışășțāēīō", "ūėįųđþð½¼¾" },
};

/* bottom row: SHIFT(2) #+=(1) àé(1) SPACE(3) DEL(2) OK(1) = 10 columns */
enum { K_SHIFT = 0, K_SYM, K_ACC, K_SPACE, K_DEL, K_OK, K_COUNT };
static const int g_key_start[K_COUNT] = { 0, 2, 3, 4, 7, 9 };
static const int g_key_span[K_COUNT] = { 2, 1, 1, 3, 2, 1 };

struct osk {
	struct screen base;
	char title[96];
	char text[256];               /* at most OSK_MAX_CHARS characters */
	bool password, shift, show;
	int page;
	int row, col;
	osk_fn cb;
	void *user;
	/* osk_set_live(): called after each change of the text (search) */
	void (*on_change)(struct ui *ui, struct screen *osk, const char *text, void *user);
};

/* The character of key (r, c) of the page in use, UTF-8, into out. */
static void osk_key(const struct osk *k, int r, int c, char *out, size_t n)
{
	if (k->page >= PG_ACCENTS1) {
		const char *s = osk_accents[k->page - PG_ACCENTS1][r], *e;
		char one[8];

		for (int i = 0; i < c && *s; i++)
			utf8_next(&s);
		e = s;
		if (*e)
			utf8_next(&e);
		snprintf(one, sizeof(one), "%.*s", (int)(e - s), s);
		if (k->shift)
			utf8_upper(one, out, n);
		else
			snprintf(out, n, "%s", one);
		return;
	}
	out[0] = (k->page == PG_SYMBOLS ? osk_symbols[r] : k->shift ? osk_upper[r] : osk_lower[r])[c];
	out[1] = 0;
}

static int bottom_key(int col)
{
	for (int b = K_COUNT - 1; b > 0; b--)
		if (col >= g_key_start[b])
			return b;
	return K_SHIFT;
}

static int utf8_count(const char *s)
{
	int n = 0;

	for (; *s; s++)
		n += ((unsigned char)*s & 0xc0) != 0x80;
	return n;
}

static void osk_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct osk *k = (struct osk *)scr;
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : ms->font_path,
				   ui_font_px(ui, ms->font_size));
	struct font *fsm = font_get(ms->font_path, ui_font_px(ui, ms->small_size));
	int pad = MAX(6, ui->h / 60);
	int w = MIN(ui->w - 2 * pad, (int)lroundf((float)ui->h * 1.3f));
	int kw = (w - 2 * pad) / OSK_COLS;
	/* live (search): flatter keys at the bottom, the list's first rows and
	 * its search line stay visible above */
	int kh = (int)lroundf((float)kw * (k->on_change ? 0.6f : 0.82f));
	int fh = (int)lroundf((float)font_height(f) * 1.8f);
	/* live: no title row (the list's header shows the search and its count) */
	int th = k->on_change ? 0 : fh + pad;
	int h = pad * 3 + th + fh + OSK_ROWS * kh;
	int x = (ui->w - w) / 2, y = k->on_change ? MAX(pad, ui->h - h - ui->h / 14) :
					     MAX(pad, (ui->h - h) / 2 - pad * 2);
	char shown[300];
	/* TRANSLATORS: help bar of the on-screen keyboard: one short word each */
	static const struct help_prompt prompts[] = {
		{ "a", N_("type") }, { "b", N_("delete") }, { "x", N_("space") }, { "y", N_("shift") },
		{ "start", N_("ok") }, { "select", N_("cancel") },
	};

	draw_panel(s, x, y, w, h, MAX(4, ui->h / 60), ms->bg);
	if (th)
		draw_text_box(ui, s, fb, k->title, x + pad, y + pad, w - 2 * pad, fh, AL_CENTER, ms->title);
	/* text field */
	{
		int fx = x + pad, fy = y + pad + (th ? fh : 0), fw = w - 2 * pad;

		gfx_fill_round(s, fx, fy, fw, fh, 4, gfx_with_alpha(ms->text_dim, 60));
		if (k->password && !k->show) {
			int i, nc = utf8_count(k->text);

			for (i = 0; i < nc && i < (int)sizeof(shown) - 2; i++)
				shown[i] = '*';
			shown[i] = 0;
		} else {
			strlcpy_(shown, k->text, sizeof(shown));
		}
		strncat(shown, "_", sizeof(shown) - strlen(shown) - 1);
		draw_text_box(ui, s, f, shown, fx + pad, fy, fw - 2 * pad, fh, AL_LEFT, ms->text);
	}
	/* keys */
	for (int r = 0; r < OSK_ROWS; r++) {
		int ky = y + pad * 2 + th + fh + r * kh;

		if (r < 4) {
			for (int c = 0; c < OSK_COLS; c++) {
				int kx = x + pad + c * kw;
				bool sel = k->row == r && k->col == c;
				char label[16];

				osk_key(k, r, c, label, sizeof(label));
				gfx_fill_round(s, kx + 2, ky + 2, kw - 4, kh - 4, 4,
					       sel ? ms->selector : gfx_with_alpha(ms->text_dim, 45));
				draw_text_box(ui, s, f, label, kx, ky, kw, kh, AL_CENTER,
					      sel ? ms->sel_text : ms->text);
			}
		} else {
			const char *names[K_COUNT];

			/* TRANSLATORS: on-screen keyboard key (2 keys wide): at most ~6 letters */
			names[K_SHIFT] = C_("keyboard", "SHIFT");
			names[K_SYM] = k->page == PG_SYMBOLS ? "abc" : "#+=";
			names[K_ACC] = k->page >= PG_ACCENTS1 ? "abc" : "\xc3\xa0\xc3\xa9";   /* "àé" */
			/* TRANSLATORS: on-screen keyboard key (3 keys wide) */
			names[K_SPACE] = C_("keyboard", "SPACE");
			/* TRANSLATORS: on-screen keyboard key, deletes a character (2 keys wide) */
			names[K_DEL] = C_("keyboard", "DEL");
			/* TRANSLATORS: on-screen keyboard key, confirms (1 key wide: 2-3 letters) */
			names[K_OK] = C_("keyboard", "OK");
			for (int b = 0; b < K_COUNT; b++) {
				int kx = x + pad + g_key_start[b] * kw;
				bool sel = k->row == 4 && bottom_key(k->col) == b;
				bool active = (b == K_SHIFT && k->shift) || (b == K_SYM && k->page == PG_SYMBOLS) ||
					      (b == K_ACC && k->page >= PG_ACCENTS1);
				bool off = b == K_ACC && k->password;
				gfx_color bgc = sel ? ms->selector :
					active ? gfx_with_alpha(ms->accent, 160) : gfx_with_alpha(ms->text_dim, off ? 20 : 45);

				gfx_fill_round(s, kx + 2, ky + 2, g_key_span[b] * kw - 4, kh - 4, 4, bgc);
				draw_text_box(ui, s, fsm, names[b], kx + 2, ky, g_key_span[b] * kw - 4, kh,
					      AL_CENTER, sel ? ms->sel_text : off ? ms->text_dim : ms->text);
			}
		}
	}
	menu_help(ui, s, prompts, 6);
}

static void osk_finish(struct ui *ui, struct osk *k, bool ok)
{
	osk_fn cb = k->cb;
	void *user = k->user;
	char text[256];

	strlcpy_(text, k->text, sizeof(text));
	ui_pop(ui);
	if (cb)
		cb(ui, text, ok, user);
}

static void osk_type(struct osk *k, const char *c)
{
	size_t l = strlen(k->text), cl = strlen(c);

	if (cl && l + cl < sizeof(k->text) && utf8_count(k->text) < OSK_MAX_CHARS)
		memcpy(k->text + l, c, cl + 1);
}

/* Deletes the last character (all its UTF-8 bytes). */
static void osk_backspace(struct osk *k)
{
	size_t l = strlen(k->text);

	while (l > 0 && ((unsigned char)k->text[l - 1] & 0xc0) == 0x80)
		l--;
	if (l > 0)
		l--;
	k->text[l] = 0;
}

/* R, or the àé key: letters -> accents 1 -> accents 2 -> letters. */
static void osk_next_accents(struct osk *k)
{
	if (k->password)
		return;
	k->page = k->page == PG_ACCENTS1 ? PG_ACCENTS2 : k->page == PG_ACCENTS2 ? PG_LETTERS : PG_ACCENTS1;
}

static void osk_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct osk *k = (struct osk *)scr;
	char key[16], before[256];

	if (t == IN_NAV_RELEASE)
		return;
	strlcpy_(before, k->text, sizeof(before));
	switch (b) {
	case IN_UP:
		k->row = (k->row + OSK_ROWS - 1) % OSK_ROWS;
		break;
	case IN_DOWN:
		k->row = (k->row + 1) % OSK_ROWS;
		break;
	case IN_LEFT:
		if (k->row == 4)
			k->col = g_key_start[(bottom_key(k->col) + K_COUNT - 1) % K_COUNT];
		else
			k->col = (k->col + OSK_COLS - 1) % OSK_COLS;
		break;
	case IN_RIGHT:
		if (k->row == 4)
			k->col = g_key_start[(bottom_key(k->col) + 1) % K_COUNT];
		else
			k->col = (k->col + 1) % OSK_COLS;
		break;
	case IN_A:
		if (k->row < 4) {
			osk_key(k, k->row, k->col, key, sizeof(key));
			osk_type(k, key);
			if (k->shift && k->page != PG_SYMBOLS)
				k->shift = false;
		} else {
			switch (bottom_key(k->col)) {
			case K_SHIFT:
				k->shift = !k->shift;
				if (k->page == PG_SYMBOLS)
					k->page = PG_LETTERS;
				break;
			case K_SYM:
				k->page = k->page == PG_SYMBOLS ? PG_LETTERS : PG_SYMBOLS;
				break;
			case K_ACC:
				osk_next_accents(k);
				break;
			case K_SPACE:
				osk_type(k, " ");
				break;
			case K_DEL:
				osk_backspace(k);
				break;
			case K_OK:
				osk_finish(ui, k, true);
				return;
			}
		}
		break;
	case IN_B:
		osk_backspace(k);
		break;
	case IN_X:
		osk_type(k, " ");
		break;
	case IN_Y:
		k->shift = !k->shift;
		break;
	case IN_L:
		k->show = !k->show;
		break;
	case IN_R:
		if (t == IN_NAV_PRESS)
			osk_next_accents(k);
		break;
	case IN_START:
		if (t == IN_NAV_PRESS) {
			osk_finish(ui, k, true);
			return;
		}
		break;
	case IN_SELECT:
		if (t == IN_NAV_PRESS) {
			osk_finish(ui, k, false);
			return;
		}
		break;
	default:
		return;
	}
	if (k->on_change && strcmp(before, k->text))
		k->on_change(ui, scr, k->text, k->user);
	ui->dirty = true;
}

void osk_set_live(struct screen *s, void (*on_change)(struct ui *ui, struct screen *osk, const char *text,
						       void *user))
{
	if (screen_is_osk(s))
		((struct osk *)s)->on_change = on_change;
}

void osk_set_title(struct screen *s, const char *title)
{
	if (screen_is_osk(s))
		strlcpy_(((struct osk *)s)->title, title, sizeof(((struct osk *)s)->title));
}

/* The keyboard has no timer: no update, no timeout (never the dialog's). */
static bool osk_update(struct ui *ui, struct screen *s)
{
	(void)ui;
	(void)s;
	return false;
}

static int osk_timeout(struct ui *ui, struct screen *s)
{
	(void)ui;
	(void)s;
	return -1;
}

static void osk_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	struct osk *k = (struct osk *)scr;

	(void)ui;
	snprintf(buf, n, "osk:%s|%s", k->title, k->password ? "" : k->text);
}

static const struct screen_ops osk_ops = {
	.button = osk_button,
	.update = osk_update,
	.render = osk_render,
	.relayout = m_relayout,
	.destroy = m_destroy,
	.timeout = osk_timeout,
	.opaque = false,
	.describe = osk_describe,
};

struct screen *osk_open(struct ui *ui, const char *title, const char *initial, bool password,
			osk_fn cb, void *user)
{
	struct osk *k = xcalloc(1, sizeof(*k));

	k->base.ops = &osk_ops;
	strlcpy_(k->title, title, sizeof(k->title));
	strlcpy_(k->text, initial ? initial : "", sizeof(k->text));
	k->password = password;
	k->row = 1;
	k->cb = cb;
	k->user = user;
	if (!ui_push(ui, &k->base))
		return NULL;
	return &k->base;
}
