/*
 * screens.c - the unified settings menu (display, controls, network,
 * theme, game lists, storage, system info, power), the per-game options,
 * the button test screen and the controller configuration wizard.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ui_internal.h"

enum {
	K_MAIN = 1, K_DISPLAY, K_CONTROLS, K_PADCFG, K_NET, K_THEME, K_LISTS, K_STORAGE,
	K_INFO, K_POWER, K_GAME, K_TIME, K_TZ, K_GAMES, K_LANG,
};

enum {
	ID_DISPLAY = 1, ID_CONTROLS, ID_NET, ID_THEME, ID_LISTS, ID_STORAGE, ID_INFO, ID_POWER,
	ID_DATETIME,
	ID_BRIGHT = 10, ID_HDMI, ID_SCALE, ID_LISTVIEW, ID_GAMEBATT, ID_GAMEBATTPOS, ID_LCDHZ,
	ID_BTNTEST = 20, ID_PADCFG, ID_P1, ID_CZ, ID_PORT0,
	ID_WIFI = 30, ID_SSID, ID_PSK, ID_ETH, ID_BT, ID_NETSTATUS,
	ID_THEME0 = 40,
	ID_FAVFIRST = 70, ID_COLLECTIONS, ID_REFRESH,
	ID_EMMC = 75, ID_SATA, ID_IMPORT, ID_EJECT, ID_USBPROMPT, ID_FREE, ID_EXPORT, ID_SAVESBK,
	ID_WEBSHARE = 190, ID_HOSTNAME, ID_WEBIDLE, ID_WEBBG, ID_SMB,
	ID_REBOOT = 80, ID_POWEROFF, ID_DIM, ID_OFF, ID_GAUGE, ID_AUTOOFF,
	ID_TIME = 140, ID_TZ, ID_YEAR, ID_MONTH, ID_DAY, ID_HOUR, ID_MIN, ID_SETTIME, ID_NOW,
	ID_TZ0 = 160,
	ID_FAV = 90, ID_GAMECORE, ID_SYSCORE, ID_LAUNCH, ID_FILE,
	ID_PAD0 = 100,
	ID_INFO0 = 120,
	ID_GAMES = 200, ID_AUTOSAVE, ID_RESUMEMODE,
	ID_LANGUAGE = 210,
	ID_LANG0 = 300,
};

/* batch 2 (their own range) */
enum {
	ID_SEARCHALL = 400, ID_RESUMEBOOT, ID_FFSPEED, ID_SHOWHIDDEN, ID_SORT, ID_RUMBLE,
	ID_GSCALE = 410, ID_GCPU, ID_GSEARCH, ID_GHIDE, ID_GDELETE, ID_GPLAYTIME,
};

enum { JOB_WIFI = 1, JOB_ETH, JOB_BT };

static void search_all_open(struct ui *ui);

/* The *_labels tables are English (N_): set_choices() translates them. */
static const char *const g_hdmi_modes[] = { "auto", "720p", "1080p" };
/* TRANSLATORS: HDMI resolution choice: chosen by itself */
static const char *const g_hdmi_labels[] = { N_("Auto"), "720p", "1080p" };
static const char *const g_scale_modes[] = { "aspect", "integer" };
/* TRANSLATORS: game scaling: keep the picture's shape / whole-pixel steps */
static const char *const g_scale_labels[] = { N_("Aspect"), N_("Integer") };
static const char *const g_view_modes[] = { "auto", "basic", "detailed" };
/* TRANSLATORS: game list style: automatic, a plain list, list + picture and details */
static const char *const g_view_labels[] = { N_("Automatic"), N_("Basic"), N_("Detailed") };
/* in-game battery overlay (read by the game process at start, host-design.md §8) */
static const char *const g_corner_modes[] = { "top-right", "top-left", "bottom-right", "bottom-left" };
/* TRANSLATORS: corner of the screen */
static const char *const g_corner_labels[] = { N_("Top right"), N_("Top left"), N_("Bottom right"),
						      N_("Bottom left") };
/* LCD refresh (display-design.md §3.1): 78 Hz = the panel's DT mode, 60 Hz =
 * the same timings at 25.2 MHz; the default stays 78 until 60 is confirmed */
static const char *const g_lcdhz_modes[] = { "78", "60" };
/* TRANSLATORS: LCD refresh rate choice; "legacy" = the old, original setting */
static const char *const g_lcdhz_labels[] = { N_("78 Hz (legacy)"), "60 Hz" };
#define LCD_TRIAL_S 15
static const char *const g_p1_modes[] = { "auto", "builtin", "external" };
/* TRANSLATORS: who is player 1 (value shown right of "Player 1"): keep it short */
static const char *const g_p1_labels[] = { N_("Auto (the controller that starts the game)"),
						  N_("Built-in"), N_("External") };

static int index_of(const char *const *list, int n, const char *v)
{
	for (int i = 0; i < n; i++)
		if (v && !strcmp(list[i], v))
			return i;
	return 0;
}

/* labels: English (N_) or names; each is shown translated when the catalog
 * has it (a core or theme name is not, and stays as it is). */
static void set_choices(struct menu_item *it, const char *const *labels, int n, int sel)
{
	it->nchoices = MIN(n, 16);
	for (int i = 0; i < it->nchoices; i++)
		it->choices[i] = _(labels[i]);
	it->val = CLAMP(sel, 0, it->nchoices - 1);
	strlcpy_(it->value, it->choices[it->val], sizeof(it->value));
}

static void save_setting(struct ui *ui, const char *key, const char *value, bool notify)
{
	settings_set(ui->settings, key, value);
	if (settings_save(ui->settings) < 0)
		ui_toastf(ui, "%s", _("Could not save the settings"));
	if (notify)
		ui_setting_changed(ui, key, value);
}

/* ------------------------------------------------------------ brightness */
static int bright_level(struct ui *ui)
{
	int max, cur;

	if (ui->cfg.input) {
		cur = input_brightness_get(ui->cfg.input, &max);
		if (cur >= 0 && max > 0)
			return CLAMP((cur * 10 + max / 2) / max, 1, 10);
	}
	return CLAMP(settings_get_int(ui->settings, "brightness", 8), 1, 10);
}

static void bright_set(struct ui *ui, int level)
{
	char v[8];

	level = CLAMP(level, 1, 10);
	if (ui->cfg.input) {
		int max;

		if (input_brightness_get(ui->cfg.input, &max) >= 0 && max > 0)
			input_brightness_set(ui->cfg.input, MAX(1, level * max / 10));
	}
	snprintf(v, sizeof(v), "%d", level);
	settings_set(ui->settings, "brightness", v);
	settings_save(ui->settings);
}

void ui_apply_boot_settings(struct ui *ui)
{
	if (ui->cfg.input) {
		const char *p1 = settings_get(ui->settings, "p1", "auto");

		input_set_p1_policy(ui->cfg.input, (enum input_p1_policy)index_of(g_p1_modes, 3, p1));
		input_set_cz_buttons(ui->cfg.input, settings_get_bool(ui->settings, "cz_buttons", false));
		if (!ui->hdmi && settings_get(ui->settings, "brightness", NULL))
			bright_set(ui, settings_get_int(ui->settings, "brightness", 8));
	}
}

/* ----------------------------------------------------------------- display */
/* The LCD refresh in use (the trial value while the dialog is up). */
static int lcd_hz_now(struct ui *ui)
{
	if (!ui->lcd_hz)
		ui->lcd_hz = strcmp(settings_get(ui->settings, "lcd_refresh", "78"), "60") ? 78 : 60;
	return ui->lcd_hz;
}

static void display_refresh(struct ui *ui, struct menu *m)
{
	struct menu_item *it = menu_find(m, ID_BRIGHT);

	if (it) {
		it->val = bright_level(ui);
		it->disabled = ui->hdmi;
		if (ui->hdmi)
			/* TRANSLATORS: brightness value while on HDMI (the TV sets it) */
			strlcpy_(it->value, _("TV"), sizeof(it->value));
		else
			snprintf(it->value, sizeof(it->value), "%d", it->val);
	}
	it = menu_find(m, ID_LCDHZ);
	if (it) {
		set_choices(it, g_lcdhz_labels, 2, lcd_hz_now(ui) == 60 ? 1 : 0);
		it->disabled = ui->hdmi;          /* the trial needs the panel on */
	}
}

/* The Display menu shows the value in use again (after the trial). */
static void lcd_item_sync(struct ui *ui)
{
	struct screen *s = ui_top(ui);

	if (s && screen_is_menu(s) && ((struct menu *)s)->kind == K_DISPLAY)
		display_refresh(ui, (struct menu *)s);
	ui->dirty = true;
}

/* KEEP (0) saves lcd_refresh = 60; REVERT (1), B (-1) and the countdown
 * (1) go back to 78 Hz, nothing saved: a panel that shows nothing at 60 Hz
 * comes back by itself. */
static void lcd_trial_done(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0) {
		save_setting(ui, "lcd_refresh", g_lcdhz_modes[1], false);
		ui_log(UI_LOG_INFO, "display: LCD refresh 60 Hz kept");
		ui_toastf(ui, "%s", _("LCD refresh rate: 60 Hz"));
	} else {
		ui->lcd_hz = 78;
		ui_setting_changed(ui, "lcd_refresh", g_lcdhz_modes[0]);
		ui_log(UI_LOG_INFO, "display: LCD refresh trial %s: back to 78 Hz",
		       choice < 0 ? "cancelled" : "reverted");
		ui_toastf(ui, "%s", _("LCD refresh rate: back to 78 Hz"));
	}
	lcd_item_sync(ui);
}

static void lcd_hz_changed(struct ui *ui, struct menu *m, struct menu_item *it)
{
	const char *buttons[3];
	int want = it->val == 1 ? 60 : 78;
	struct screen *d;

	if (want == lcd_hz_now(ui))
		return;
	if (ui->hdmi) {
		display_refresh(ui, m);
		return;
	}
	ui->lcd_hz = want;
	if (want == 78) {
		/* back to the panel's own mode: known good, saved at once */
		save_setting(ui, "lcd_refresh", g_lcdhz_modes[0], true);
		return;
	}
	ui_setting_changed(ui, "lcd_refresh", g_lcdhz_modes[1]);   /* live, not saved yet */
	ui_log(UI_LOG_INFO, "display: LCD refresh 60 Hz on trial (%d s)", LCD_TRIAL_S);
	/* TRANSLATORS: dialog buttons, uppercase, short */
	buttons[0] = _("KEEP");
	buttons[1] = _("REVERT");
	buttons[2] = NULL;
	/* TRANSLATORS: %d is the number of seconds left (it counts down; keep "%d"
	 * exactly once) */
	d = dialog_open(ui, _("The screen now refreshes at 60 Hz.\nKeep this setting? Reverting in %d s"),
			buttons, lcd_trial_done, NULL);
	if (!d) {
		/* no dialog (screen stack full): no trial without its countdown */
		lcd_trial_done(ui, 1, NULL);
		return;
	}
	dialog_select(d, 1);                              /* a blind A press reverts */
	dialog_set_countdown(d, LCD_TRIAL_S, 1);
}

static void display_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	switch (it->id) {
	case ID_BRIGHT:
		bright_set(ui, it->val);
		display_refresh(ui, m);
		break;
	case ID_HDMI:
		save_setting(ui, "hdmi_mode", g_hdmi_modes[it->val], true);
		break;
	case ID_SCALE:
		save_setting(ui, "scaling", g_scale_modes[it->val], true);
		break;
	case ID_LISTVIEW:
		save_setting(ui, "gamelist_view", g_view_modes[it->val], false);
		break;
	case ID_GAMEBATT:
		save_setting(ui, "game_battery_overlay", it->on ? "on" : "off", false);
		break;
	case ID_GAMEBATTPOS:
		save_setting(ui, "game_battery_corner", g_corner_modes[it->val], false);
		break;
	case ID_LCDHZ:
		lcd_hz_changed(ui, m, it);
		break;
	}
	(void)dir;
}

static void open_display(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Display"), K_DISPLAY);
	struct menu_item *it;

	/* Built-in screen only (board profile: internal_display,
	 * internal_refresh_options) */
	if (ui->cfg.has_internal_display) {
		it = menu_add(m, MI_SLIDER, ID_BRIGHT, _("Brightness"));
		it->min = 1;
		it->max = 10;
		if (ui->cfg.lcd_refresh_choice) {
			it = menu_add(m, MI_CHOICE, ID_LCDHZ, _("LCD refresh rate"));
			set_choices(it, g_lcdhz_labels, 2, lcd_hz_now(ui) == 60 ? 1 : 0);
		}
	}
	it = menu_add(m, MI_CHOICE, ID_HDMI, _("HDMI resolution"));
	set_choices(it, g_hdmi_labels, 3,
		    index_of(g_hdmi_modes, 3, settings_get(ui->settings, "hdmi_mode", "auto")));
	it = menu_add(m, MI_CHOICE, ID_SCALE, _("Game scaling"));
	set_choices(it, g_scale_labels, 2,
		    index_of(g_scale_modes, 2, settings_get(ui->settings, "scaling", "aspect")));
	it = menu_add(m, MI_CHOICE, ID_LISTVIEW, _("Game list style"));
	set_choices(it, g_view_labels, 3,
		    index_of(g_view_modes, 3, settings_get(ui->settings, "gamelist_view", "auto")));
	it = menu_add(m, MI_TOGGLE, ID_GAMEBATT, _("Battery in games"));
	it->on = settings_get_bool(ui->settings, "game_battery_overlay", true);
	it = menu_add(m, MI_CHOICE, ID_GAMEBATTPOS, _("Battery position"));
	set_choices(it, g_corner_labels, 4,
		    index_of(g_corner_modes, 4, settings_get(ui->settings, "game_battery_corner", "top-right")));
	m->on_item = display_item;
	m->on_refresh = display_refresh;
	menu_open(ui, m);
}

/* ----------------------------------------------------------- button test */
struct btest {
	struct screen base;
	uint32_t held;
	int64_t both_since;
};

static void bt_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct btest *b = (struct btest *)scr;
	const struct menu_style *ms = &ui->ms;
	int W = ui->w, H = ui->h;
	int u = H / 16;                      /* grid unit */
	int cx = W / 2, cy = H / 2 + u / 2;
	gfx_color off = gfx_with_alpha(ms->text_dim, 110), on = ms->accent | 0xff000000u;
	struct font *f = font_get(ms->font_bold[0] ? ms->font_bold : NULL, ui_font_px(ui, 0.035f));
	struct font *ft = font_get(ms->font_bold[0] ? ms->font_bold : NULL, ui_font_px(ui, 0.05f));
	uint32_t h = b->held;
#define ON(btn) (h & (1u << (btn)))
#define BTN(btn, x, y, r, label) do { \
		gfx_fill_circle(s, x, y, r, ON(btn) ? on : off); \
		draw_text_box(ui, s, f, label, (x) - (r), (y) - (r), 2 * (r), 2 * (r), AL_CENTER, \
			      ON(btn) ? 0xffffffffu : ms->text); \
	} while (0)
#define PILL(btn, x, y, w, hh, label) do { \
		gfx_fill_round(s, x, y, w, hh, (hh) / 2, ON(btn) ? on : off); \
		draw_text_box(ui, s, f, label, x, y, w, hh, AL_CENTER, ON(btn) ? 0xffffffffu : ms->text); \
	} while (0)

	gfx_fill(s, 0, 0, W, H, (ms->bg & 0x00ffffffu) | 0xff000000u);
	draw_text_box(ui, s, ft, _("Button test"), 0, u / 2, W, u * 3 / 2, AL_CENTER, ms->title);
	/* body */
	gfx_stroke_round(s, cx - u * 9, cy - u * 4, u * 18, u * 8, u * 2, MAX(2, u / 8), off);
	/* shoulders */
	PILL(IN_L2, cx - u * 9, cy - u * 7, u * 3, u, "L2");
	PILL(IN_L, cx - u * 9, cy - u * 5 - u / 2, u * 4, u, "L1");
	PILL(IN_R2, cx + u * 6, cy - u * 7, u * 3, u, "R2");
	PILL(IN_R, cx + u * 5, cy - u * 5 - u / 2, u * 4, u, "R1");
	/* d-pad */
	{
		int dx = cx - u * 6, dy = cy - u / 2, a = u * 11 / 10;

		gfx_fill_round(s, dx - a / 2, dy - a * 3 / 2, a, a, 3, ON(IN_UP) ? on : off);
		gfx_fill_round(s, dx - a / 2, dy + a / 2, a, a, 3, ON(IN_DOWN) ? on : off);
		gfx_fill_round(s, dx - a * 3 / 2, dy - a / 2, a, a, 3, ON(IN_LEFT) ? on : off);
		gfx_fill_round(s, dx + a / 2, dy - a / 2, a, a, 3, ON(IN_RIGHT) ? on : off);
		gfx_fill_round(s, dx - a / 2, dy - a / 2, a, a, 0, off);
	}
	/* face buttons, positional: X top, Y left, A right, B bottom */
	{
		int fx = cx + u * 6, fy = cy - u / 2, r = u * 3 / 5, d = u * 13 / 10;

		BTN(IN_X, fx, fy - d, r, "X");
		BTN(IN_Y, fx - d, fy, r, "Y");
		BTN(IN_A, fx + d, fy, r, "A");
		BTN(IN_B, fx, fy + d, r, "B");
		/* optional C (top right) and Z (bottom left): RetroPad L3/R3 */
		BTN(IN_L3, fx + d, fy - d, r * 4 / 5, "C");
		BTN(IN_R3, fx - d, fy + d, r * 4 / 5, "Z");
	}
	PILL(IN_SELECT, cx - u * 3, cy + u * 2, u * 5 / 2, u * 4 / 5, "SELECT");
	PILL(IN_START, cx + u / 2, cy + u * 2, u * 5 / 2, u * 4 / 5, "START");
	/* analog stick */
	{
		int sx = cx - u * 2, sy = cy - u * 2, r = u;
		int x = 0, y = 0;
		bool fitted = false;

		if (ui->cfg.input)
			fitted = input_builtin_stick(ui->cfg.input, &x, &y);
		gfx_stroke_round(s, cx - r, sy - r, 2 * r, 2 * r, r, 2, off);
		if (fitted) {
			int px = cx + (int)((long)(x - 1500) * r / 1500);
			int py = sy + (int)((long)(y - 1500) * r / 1500);

			gfx_fill_circle(s, px, py, r / 3, on);
		} else {
			draw_text_box(ui, s, font_get(NULL, ui_font_px(ui, 0.025f)), _("no stick"),
				      cx - r * 2, sy - r / 2, r * 4, r, AL_CENTER, ms->text_dim);
		}
		(void)sx;
	}
	draw_text_box(ui, s, f, _("Hold SELECT + START to exit"), 0, H - u * 2, W, u, AL_CENTER,
		      ms->text_dim);
#undef ON
#undef BTN
#undef PILL
}

static void bt_button(struct ui *ui, struct screen *scr, enum input_btn btn, enum input_nav_type t)
{
	struct btest *b = (struct btest *)scr;
	uint32_t both = (1u << IN_SELECT) | (1u << IN_START);

	if (t == IN_NAV_PRESS)
		b->held |= 1u << btn;
	else if (t == IN_NAV_RELEASE)
		b->held &= ~(1u << btn);
	if ((b->held & both) == both) {
		if (!b->both_since)
			b->both_since = ui->now;
	} else {
		b->both_since = 0;
	}
	ui->dirty = true;
}

static bool bt_update(struct ui *ui, struct screen *scr)
{
	struct btest *b = (struct btest *)scr;

	if (b->both_since && ui->now - b->both_since > 800) {
		ui_pop(ui);
		return true;
	}
	/* the stick moves without nav events */
	return ui->cfg.input != NULL;
}

static int bt_timeout(struct ui *ui, struct screen *scr)
{
	struct btest *b = (struct btest *)scr;

	if (b->both_since)
		return 50;
	return ui->cfg.input ? 50 : -1;
}

static void bt_destroy(struct ui *ui, struct screen *s)
{
	(void)ui;
	free(s);
}

static void bt_relayout(struct ui *ui, struct screen *s)
{
	(void)ui;
	(void)s;
}

static const struct screen_ops bt_ops = {
	.button = bt_button,
	.update = bt_update,
	.render = bt_render,
	.relayout = bt_relayout,
	.destroy = bt_destroy,
	.timeout = bt_timeout,
	.opaque = true,
};

void buttontest_open(struct ui *ui)
{
	struct btest *b = xcalloc(1, sizeof(*b));

	b->base.ops = &bt_ops;
	ui_push(ui, &b->base);
}

/* ------------------------------------------------------ controller wizard */
struct wstep {
	const char *target;   /* SDL name */
	const char *prompt;
	bool optional;
	bool axis;            /* expects a stick axis */
};

/* TRANSLATORS: controller set-up wizard: what to press now (big text, one line) */
static const struct wstep g_steps[] = {
	{ "dpup", N_("Press D-pad UP"), false, false },
	{ "dpdown", N_("Press D-pad DOWN"), false, false },
	{ "dpleft", N_("Press D-pad LEFT"), false, false },
	{ "dpright", N_("Press D-pad RIGHT"), false, false },
	{ "b", N_("Press the RIGHT face button (A)"), false, false },
	{ "a", N_("Press the BOTTOM face button (B)"), false, false },
	{ "y", N_("Press the TOP face button (X)"), false, false },
	{ "x", N_("Press the LEFT face button (Y)"), false, false },
	{ "leftshoulder", N_("Press L1"), true, false },
	{ "rightshoulder", N_("Press R1"), true, false },
	{ "lefttrigger", N_("Press L2"), true, false },
	{ "righttrigger", N_("Press R2"), true, false },
	{ "back", N_("Press SELECT"), false, false },
	{ "start", N_("Press START"), false, false },
	{ "leftstick", N_("Press the left stick (L3)"), true, false },
	{ "rightstick", N_("Press the right stick (R3)"), true, false },
	{ "leftx", N_("Push the left stick RIGHT"), true, true },
	{ "lefty", N_("Push the left stick DOWN"), true, true },
	{ "rightx", N_("Push the right stick RIGHT"), true, true },
	{ "righty", N_("Push the right stick DOWN"), true, true },
};

#define WIZ_STEPS ((int)ARRAY_SIZE(g_steps))
#define WIZ_SKIP_MS 6000

struct wizard {
	struct screen base;
	int slot;
	char name[80];
	int step;
	int64_t step_t0;
	char result[WIZ_STEPS][24];
	bool done;
};

static void wiz_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct wizard *w = (struct wizard *)scr;
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : NULL,
				   ui_font_px(ui, ms->font_size * 1.2f));
	struct font *fs = font_get(ms->font_path, ui_font_px(ui, ms->small_size));
	int W = ui->w, H = ui->h, pw = W * 8 / 10, ph = H * 6 / 10;
	int x = (W - pw) / 2, y = (H - ph) / 2, lh = font_height(f) * 2;
	char buf[128];

	draw_panel(s, x, y, pw, ph, MAX(4, H / 60), ms->bg);
	draw_text_box(ui, s, f, _("Configure controller"), x, y + lh / 3, pw, lh, AL_CENTER, ms->title);
	draw_text_box(ui, s, fs, w->name, x, y + lh, pw, lh, AL_CENTER, ms->text_dim);
	if (w->step < WIZ_STEPS) {
		const struct wstep *st = &g_steps[w->step];
		int left = (int)((WIZ_SKIP_MS - (ui->now - w->step_t0)) / 1000);

		draw_text_box(ui, s, fb, _(st->prompt), x, y + ph / 2 - lh, pw, lh * 3 / 2, AL_CENTER,
			      ms->accent);
		snprintf(buf, sizeof(buf), _("Step %d of %d"), w->step + 1, WIZ_STEPS);
		draw_text_box(ui, s, fs, buf, x, y + ph / 2 + lh / 2, pw, lh, AL_CENTER, ms->text_dim);
		if (st->optional) {
			snprintf(buf, sizeof(buf), _("Not on this pad? Wait %d s to skip"), MAX(0, left));
			draw_text_box(ui, s, fs, buf, x, y + ph - lh * 3 / 2, pw, lh, AL_CENTER,
				      ms->text_dim);
		}
	}
}

static void wiz_finish(struct ui *ui, struct wizard *w)
{
	char map[1024] = "";
	int r = -1;

	for (int i = 0; i < WIZ_STEPS; i++) {
		char e[40];

		if (!w->result[i][0])
			continue;
		snprintf(e, sizeof(e), "%s%s:%s", map[0] ? "," : "", g_steps[i].target, w->result[i]);
		strncat(map, e, sizeof(map) - strlen(map) - 1);
	}
	if (ui->cfg.input) {
		input_capture_end(ui->cfg.input);
		r = input_save_user_mapping(ui->cfg.input, w->slot, map);
	}
	ui_pop(ui);
	if (r == 0)
		ui_toastf(ui, "%s", _("Controller saved"));
	else
		message_open(ui, _("The mapping could not be saved."));
}

static void wiz_record(struct ui *ui, struct wizard *w, const char *elem)
{
	const struct wstep *st = &g_steps[w->step];
	char v[24];

	if (st->axis) {
		/* "+a2" pushed right/down = normal axis; "-a2" = inverted */
		if (elem[0] != '+' && elem[0] != '-')
			return;
		snprintf(v, sizeof(v), "%s%s", elem + 1, elem[0] == '-' ? "~" : "");
	} else {
		/* buttons, hats and half axes ("+a5" for analog triggers) */
		if ((elem[0] == '+' || elem[0] == '-') && strcmp(st->target, "lefttrigger") &&
		    strcmp(st->target, "righttrigger") && strncmp(st->target, "dp", 2))
			return;
		strlcpy_(v, elem, sizeof(v));
	}
	/* refuse an element already used (bouncing buttons) */
	for (int i = 0; i < w->step; i++)
		if (!strcmp(w->result[i], v))
			return;
	strlcpy_(w->result[w->step], v, sizeof(w->result[0]));
	w->step++;
	w->step_t0 = ui->now;
}

static bool wiz_update(struct ui *ui, struct screen *scr)
{
	struct wizard *w = (struct wizard *)scr;
	char elem[16];
	bool redraw = false;

	if (!ui->cfg.input) {
		ui_pop(ui);
		return true;
	}
	while (w->step < WIZ_STEPS && input_capture_next(ui->cfg.input, elem, sizeof(elem))) {
		wiz_record(ui, w, elem);
		redraw = true;
	}
	if (w->step < WIZ_STEPS && g_steps[w->step].optional &&
	    ui->now - w->step_t0 > WIZ_SKIP_MS) {
		w->step++;
		w->step_t0 = ui->now;
		redraw = true;
	}
	if (w->step >= WIZ_STEPS) {
		wiz_finish(ui, w);
		return true;
	}
	return redraw || g_steps[w->step].optional;
}

static int wiz_timeout(struct ui *ui, struct screen *s)
{
	(void)ui;
	(void)s;
	return 50;
}

static void wiz_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	/* the device being configured is captured; the built-in pad can
	 * still cancel */
	if (t == IN_NAV_PRESS && b == IN_B) {
		if (ui->cfg.input)
			input_capture_end(ui->cfg.input);
		ui_pop(ui);
	}
	(void)scr;
}

static const struct screen_ops wiz_ops = {
	.button = wiz_button,
	.update = wiz_update,
	.render = wiz_render,
	.relayout = bt_relayout,
	.destroy = bt_destroy,
	.timeout = wiz_timeout,
	.opaque = false,
};

static void wizard_open(struct ui *ui, int slot)
{
	struct wizard *w = xcalloc(1, sizeof(*w));
	struct input_device_info di;

	w->base.ops = &wiz_ops;
	w->slot = slot;
	w->step_t0 = ui->now;
	if (ui->cfg.input) {
		input_device_info(ui->cfg.input, slot, &di);
		strlcpy_(w->name, di.name, sizeof(w->name));
		input_capture_begin(ui->cfg.input, slot);
	}
	ui_push(ui, &w->base);
}

static void padcfg_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	(void)m;
	if (dir == 0 && it->id >= ID_PAD0)
		wizard_open(ui, it->id - ID_PAD0);
}

static void open_padcfg(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Configure a controller"), K_PADCFG);
	int n = 0;

	if (ui->cfg.input) {
		for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
			struct input_device_info di;
			struct menu_item *it;

			input_device_info(ui->cfg.input, i, &di);
			if (!di.present || di.builtin || !strncmp(di.path, "", 1))
				continue;
			it = menu_add(m, MI_ACTION, ID_PAD0 + i, di.name);
				/* TRANSLATORS: how a controller is set up: your own mapping, from the
			 * database of known pads, the Linux standard, a keyboard, none */
			snprintf(it->value, sizeof(it->value), "%s",
				 di.source == IN_MAP_USER ? _("custom") :
				 di.source == IN_MAP_GCDB ? _("known pad") :
				 di.source == IN_MAP_GAMEPAD_SPEC ? _("standard") :
				 di.source == IN_MAP_KEYBOARD ? _("keyboard") : _("not configured"));
			n++;
		}
	}
	if (!n)
		menu_add(m, MI_INFO, 0, _("No external controller detected"))->disabled = true;
	m->on_item = padcfg_item;
	menu_open(ui, m);
}

/* ---------------------------------------------------------------- controls */
static void controls_refresh(struct ui *ui, struct menu *m)
{
	for (int p = 0; p < INPUT_MAX_PORTS; p++) {
		struct menu_item *it = menu_find(m, ID_PORT0 + p);
		struct input_port_info pi;

		if (!it)
			continue;
		if (!ui->cfg.input) {
			strlcpy_(it->value, p == 0 ? _("built-in") : "-", sizeof(it->value));
			continue;
		}
		input_port_info(ui->cfg.input, p, &pi);
		strlcpy_(it->value, pi.connected ? pi.name : "-", sizeof(it->value));
	}
}

static void controls_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	switch (it->id) {
	case ID_BTNTEST:
		if (dir == 0)
			buttontest_open(ui);
		break;
	case ID_PADCFG:
		if (dir == 0)
			open_padcfg(ui);
		break;
	case ID_P1:
		save_setting(ui, "p1", g_p1_modes[it->val], true);
		if (ui->cfg.input)
			input_set_p1_policy(ui->cfg.input, (enum input_p1_policy)it->val);
		controls_refresh(ui, m);
		break;
	case ID_CZ:
		save_setting(ui, "cz_buttons", it->on ? "1" : "0", true);
		if (ui->cfg.input)
			input_set_cz_buttons(ui->cfg.input, it->on);
		break;
	case ID_RUMBLE:
		/* read by the game process at start (rumble) */
		save_setting(ui, "rumble", it->on ? "1" : "0", false);
		break;
	}
}

static void open_controls(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Controls"), K_CONTROLS);
	struct menu_item *it;

	menu_add(m, MI_ACTION, ID_BTNTEST, _("Button test"));
	menu_add(m, MI_SUBMENU, ID_PADCFG, _("Configure a controller"));
	it = menu_add(m, MI_CHOICE, ID_P1, _("Player 1"));
	set_choices(it, g_p1_labels, 3, index_of(g_p1_modes, 3, settings_get(ui->settings, "p1", "auto")));
	it = menu_add(m, MI_TOGGLE, ID_CZ, _("Extra C/Z buttons fitted"));
	it->on = settings_get_bool(ui->settings, "cz_buttons", false);
	/* TRANSLATORS: Settings > Controls: games can make the USB or Bluetooth
	 * controllers that have motors vibrate (on/off) */
	it = menu_add(m, MI_TOGGLE, ID_RUMBLE, _("Controller vibration"));
	it->on = settings_get_bool(ui->settings, "rumble", true);
	menu_add(m, MI_HEADER, 0, _("Players"));
	for (int p = 0; p < INPUT_MAX_PORTS; p++) {
		char l[96];

		snprintf(l, sizeof(l), _("Player %d"), p + 1);
		menu_add(m, MI_INFO, ID_PORT0 + p, l);
	}
	m->on_item = controls_item;
	m->on_refresh = controls_refresh;
	m->refresh_every = 1000;
	menu_open(ui, m);
}

/* ---------------------------------------------------------------- network */
static const char *const g_net_names[3] = { "wifi", "eth", "bt" };
static const char *const g_idle_vals[] = { "10", "30", "60", "0" };
/* TRANSLATORS: "stop when idle for": durations, and never */
static const char *const g_idle_labels[] = { N_("10 min"), N_("30 min"), N_("1 hour"), N_("Never") };
static void hostname_done(struct ui *ui, const char *text, bool ok, void *user);

static void net_refresh(struct ui *ui, struct menu *m)
{
	static const int ids[3] = { ID_WIFI, ID_ETH, ID_BT };
	char ssid[64];
	struct menu_item *it;

	settings_reload(ui->settings);
	for (int i = 0; i < 3; i++) {
		it = menu_find(m, ids[i]);
		if (!it)
			continue;
		if (!ui->net_busy[i])
			it->on = settings_get_bool(ui->settings, g_net_names[i], false);
		strlcpy_(it->value, ui->net_busy[i] ? _("please wait...") : "", sizeof(it->value));
		it->disabled = ui->net_busy[i];
	}
	hw_read_wpa_ssid(ui->cfg.wpa_conf, ssid, sizeof(ssid));
	if ((it = menu_find(m, ID_SSID)))
		strlcpy_(it->value, ssid[0] ? ssid : _("not set"), sizeof(it->value));
	if ((it = menu_find(m, ID_HOSTNAME)))
		strlcpy_(it->value, settings_get(ui->settings, "hostname", "retrostone"), sizeof(it->value));
	if ((it = menu_find(m, ID_WEBSHARE)))
		strlcpy_(it->value, ui->cfg.transfer && ui->cfg.transfer->webshare_running() ?
			 /* TRANSLATORS: the network transfer is on */
			 _("running") : "", sizeof(it->value));
}

static void net_job(struct ui *ui, int which, bool on)
{
	const char *argv[] = { ui->cfg.net_helper, g_net_names[which], on ? "on" : "off", NULL };
	int r = hw_job_start(argv, JOB_WIFI + which);

	static const char *const msg[2][3] = {
		{ N_("WiFi: turning off, please wait..."), N_("Ethernet: turning off, please wait..."),
		  N_("Bluetooth: turning off, please wait...") },
		{ N_("WiFi: turning on, please wait..."), N_("Ethernet: turning on, please wait..."),
		  N_("Bluetooth: turning on, please wait...") },
	};

	if (r < 0) {
		ui_toastf(ui, _("Cannot run %s"), ui->cfg.net_helper);
		return;
	}
	ui->net_busy[which] = true;
	/* rsos-net can take up to a minute (it waits for a network apply that
	 * runs): it runs in the background, the menu says so */
	ui_toastf(ui, "%s", _(msg[on ? 1 : 0][which]));
}

static void ssid_done(struct ui *ui, const char *text, bool ok, void *user)
{
	char psk[128] = "";

	(void)user;
	if (!ok)
		return;
	/* keep the password when only the network name changes */
	{
		char *buf = file_read(ui->cfg.wpa_conf, NULL);

		if (buf) {
			char *p = strstr(buf, "psk=\"");

			if (p) {
				char *q = strrchr(p + 5, '"');

				if (q) {
					*q = 0;
					strlcpy_(psk, p + 5, sizeof(psk));
				}
			}
			free(buf);
		}
	}
	if (hw_write_wpa(ui->cfg.wpa_conf, text, psk) < 0)
		ui_toastf(ui, "%s", _("Could not save the WiFi settings"));
	else if (settings_get_bool(ui->settings, "wifi", false))
		net_job(ui, 0, true);
}

static void psk_done(struct ui *ui, const char *text, bool ok, void *user)
{
	char ssid[64];

	(void)user;
	if (!ok)
		return;
	if (text[0] && strlen(text) < 8) {
		message_open(ui, _("A WiFi password has at least 8 characters."));
		return;
	}
	hw_read_wpa_ssid(ui->cfg.wpa_conf, ssid, sizeof(ssid));
	if (!ssid[0]) {
		message_open(ui, _("Set the network name first."));
		return;
	}
	if (hw_write_wpa(ui->cfg.wpa_conf, ssid, text) < 0) {
		ui_toastf(ui, "%s", _("Could not save the WiFi settings"));
		return;
	}
	/* restart WiFi so wpa_supplicant reads the new file */
	if (settings_get_bool(ui->settings, "wifi", false)) {
		const char *argv[] = { "/bin/sh", "-c", "", NULL };
		char cmd[512];

		snprintf(cmd, sizeof(cmd), "%s wifi off; %s wifi on", ui->cfg.net_helper,
			 ui->cfg.net_helper);
		argv[2] = cmd;
		if (hw_job_start(argv, JOB_WIFI) > 0)
			ui->net_busy[0] = true;
	}
	ui_toastf(ui, "%s", _("WiFi password saved"));
}

static void net_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	char ssid[64];

	switch (it->id) {
	case ID_WIFI:
	case ID_ETH:
	case ID_BT:
		net_job(ui, it->id == ID_WIFI ? 0 : it->id == ID_ETH ? 1 : 2, it->on);
		break;
	case ID_SSID:
		if (dir == 0) {
			hw_read_wpa_ssid(ui->cfg.wpa_conf, ssid, sizeof(ssid));
			osk_open(ui, _("WiFi network name"), ssid, false, ssid_done, NULL);
		}
		break;
	case ID_PSK:
		if (dir == 0)
			osk_open(ui, _("WiFi password"), "", true, psk_done, NULL);
		break;
	case ID_WEBSHARE:
		if (dir == 0)
			webshare_open(ui);
		break;
	case ID_HOSTNAME:
		if (dir == 0)
			osk_open(ui, _("Console name"), settings_get(ui->settings, "hostname", "retrostone"),
				 false, hostname_done, NULL);
		break;
	case ID_WEBIDLE:
		save_setting(ui, "webshare_idle_min", g_idle_vals[it->val], false);
		break;
	case ID_WEBBG:
		save_setting(ui, "webshare_bg", it->on ? "1" : "0", false);
		break;
	case ID_SMB:
		/* with the network transfer: now if it runs, else when it starts */
		save_setting(ui, "smb", it->on ? "1" : "0", false);
		if (it->on)
			smb_start(ui);
		else
			smb_stop(ui);
		break;
	}
	net_refresh(ui, m);
}

static void hostname_done(struct ui *ui, const char *text, bool ok, void *user)
{
	char h[40];
	size_t n = 0;

	(void)user;
	if (!ok)
		return;
	for (const char *p = text; *p && n + 1 < sizeof(h) && n < 32; p++) {
		char c = *p;

		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && n))
			h[n++] = c;
	}
	h[n] = 0;
	if (!n) {
		message_open(ui, _("Use letters, digits and dashes."));
		return;
	}
	save_setting(ui, "hostname", h, true);
	ui_toastf(ui, _("Console name: %s (next transfer)"), h);
}

static void open_network(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Network"), K_NET);

	/* TRANSLATORS: the wireless network ("Wi-Fi" in French, "WLAN" in German) */
	menu_add(m, MI_TOGGLE, ID_WIFI, _("WiFi"));
	menu_add(m, MI_ACTION, ID_SSID, _("WiFi network"));
	menu_add(m, MI_ACTION, ID_PSK, _("WiFi password"));
	menu_add(m, MI_TOGGLE, ID_ETH, "Ethernet");
	menu_add(m, MI_TOGGLE, ID_BT, "Bluetooth");
	if (ui->cfg.transfer) {
		struct menu_item *it;

		menu_add(m, MI_HEADER, 0, _("Transfer games from a computer or phone"));
		menu_add(m, MI_ACTION, ID_WEBSHARE, _("Transfer over network"));
		menu_add(m, MI_ACTION, ID_HOSTNAME, _("Console name"));
		it = menu_add(m, MI_CHOICE, ID_WEBIDLE, _("Stop when idle for"));
		set_choices(it, g_idle_labels, 4, index_of(g_idle_vals, 4,
			    settings_get(ui->settings, "webshare_idle_min", "30")));
		it = menu_add(m, MI_TOGGLE, ID_WEBBG, _("Keep receiving in the background"));
		it->on = settings_get_bool(ui->settings, "webshare_bg", false);
		/* TRANSLATORS: not available yet ("later") */
		/* the ksmbd share, next to the network transfer (rsos-smb); hidden
		 * when the helper is not installed */
		if (smb_available(ui)) {
			/* TRANSLATORS: Settings > Network: also share the SD card as a
			 * Windows network folder (\\RETROSTONE) while "Transfer over
			 * network" runs */
			it = menu_add(m, MI_TOGGLE, ID_SMB, _("Windows file share"));
			it->on = settings_get_bool(ui->settings, "smb", false);
		}
	}
	m->on_item = net_item;
	m->on_refresh = net_refresh;
	m->refresh_every = 1000;
	menu_open(ui, m);
}

/* Called by ui.c when a helper job finishes. */
void screens_job_done(struct ui *ui, const struct hw_job_result *r);
void screens_job_done(struct ui *ui, const struct hw_job_result *r)
{
	if (r->tag == UI_JOB_SMB || r->tag == UI_JOB_SMB_STOP) {
		smb_job_done(ui, r);
		return;
	}
	if (r->tag >= JOB_WIFI && r->tag <= JOB_BT) {
		int i = r->tag - JOB_WIFI;
		static const char *const names[3] = { "WiFi", "Ethernet", "Bluetooth" };
		static const char *const done[2][3] = {
			{ N_("WiFi off"), N_("Ethernet off"), N_("Bluetooth off") },
			{ N_("WiFi on"), N_("Ethernet on"), N_("Bluetooth on") },
		};

		ui->net_busy[i] = false;
		settings_reload(ui->settings);
		if (r->status == 0 && i < 2 && !settings_get_bool(ui->settings, g_net_names[i], false))
			transfer_network_off(ui);
		if (r->status != 0)
			/* r->out: the helper's own message (English) */
			ui_toastf(ui, "%s: %s", names[i], r->out[0] ? r->out : _("failed"));
		else
			ui_toastf(ui, "%s", _(done[settings_get_bool(ui->settings, g_net_names[i], false) ? 1 : 0][i]));
	}
}

/* ------------------------------------------------------------------ themes */
static char g_theme_names[24][64];

static void theme_refresh(struct ui *ui, struct menu *m)
{
	for (int i = 0; i < m->n; i++) {
		struct menu_item *it = &m->items[i];

		if (it->id >= ID_THEME0 && it->id < ID_THEME0 + 24)
			strlcpy_(it->value, !strcmp(g_theme_names[it->id - ID_THEME0], ui->theme_name) ?
				 "\xe2\x9c\x93" : "", sizeof(it->value));
	}
}

static void theme_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	if (dir || it->id < ID_THEME0)
		return;
	ui_set_theme(ui, g_theme_names[it->id - ID_THEME0]);
	theme_refresh(ui, m);
}

static void open_theme(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Theme"), K_THEME);
	int n = ui_list_themes(ui, g_theme_names, 24);

	for (int i = 0; i < n; i++) {
		struct menu_item *it = menu_add(m, MI_ACTION, ID_THEME0 + i, g_theme_names[i]);

		if (!strcmp(g_theme_names[i], ui->theme_name))
			m->cursor = i;
		(void)it;
	}
	if (!n)
		menu_add(m, MI_INFO, 0, _("No theme found"))->disabled = true;
	m->on_item = theme_item;
	m->on_refresh = theme_refresh;
	menu_open(ui, m);
}

/* -------------------------------------------------------------- game lists */
static const char *const g_sort_modes[] = { "name", "playtime", "lastplayed" };
/* TRANSLATORS: Settings > Game lists > Sort games by: alphabetical, the most
 * played first (play time), the most recently played first */
static const char *const g_sort_labels[] = { N_("Name"), N_("Most played"), N_("Recently played") };

static void lists_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	(void)m;
	switch (it->id) {
	case ID_FAVFIRST:
		save_setting(ui, "favorites_first", it->on ? "1" : "0", false);
		ui_reload_games(ui, false);
		break;
	case ID_COLLECTIONS:
		save_setting(ui, "collections", it->on ? "1" : "0", false);
		ui_reload_games(ui, false);
		break;
	case ID_SHOWHIDDEN:
		save_setting(ui, "show_hidden", it->on ? "1" : "0", false);
		ui_reload_games(ui, false);
		break;
	case ID_SORT:
		save_setting(ui, "gamelist_sort", g_sort_modes[it->val], false);
		ui_reload_games(ui, false);
		break;
	case ID_REFRESH:
		if (dir == 0) {
			ui_reload_games(ui, true);
			ui_toastf(ui, "%s", _("Game lists refreshed"));
		}
		break;
	}
}

static void open_lists(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Game lists"), K_LISTS);
	struct menu_item *it;

	it = menu_add(m, MI_TOGGLE, ID_FAVFIRST, _("Favorites first"));
	it->on = settings_get_bool(ui->settings, "favorites_first", true);
	it = menu_add(m, MI_TOGGLE, ID_COLLECTIONS, _("Favorites & last played pages"));
	it->on = settings_get_bool(ui->settings, "collections", true);
	/* TRANSLATORS: Settings > Game lists: the order of the games in a list */
	it = menu_add(m, MI_CHOICE, ID_SORT, _("Sort games by"));
	set_choices(it, g_sort_labels, 3, index_of(g_sort_modes, 3, settings_get(ui->settings, "gamelist_sort", "name")));
	/* TRANSLATORS: Settings > Game lists: also list the games hidden with
	 * "Hide this game" (or in gamelist.xml) */
	it = menu_add(m, MI_TOGGLE, ID_SHOWHIDDEN, _("Show hidden games"));
	it->on = settings_get_bool(ui->settings, "show_hidden", false);
	menu_add(m, MI_ACTION, ID_REFRESH, _("Refresh game lists"));
	m->on_item = lists_item;
	menu_open(ui, m);
}

/* ------------------------------------------------------------------- games */
/*
 * Resume games: the game process writes .state.auto when the player leaves a
 * game (autosave_exit, read at game start; a power-off always writes it),
 * and the launch prompt uses resume_mode (host-design.md §7).
 */
static const char *const g_resume_modes[] = { "ask", "always", "never" };
/* TRANSLATORS: what launching a game that has an automatic save does */
static const char *const g_resume_labels[] = { N_("Ask"), N_("Always resume"), N_("Always start fresh") };

/* Resume on boot (resume_boot): the game running at the last power-off,
 * relaunched at once, offered (the 4-choice dialog, ui.c) or not. */
static const char *const g_rboot_modes[] = { "always", "ask", "never" };
/* TRANSLATORS: Settings > Games > Resume on boot: relaunch the game without
 * asking / ask / never (the menu comes up) */
static const char *const g_rboot_labels[] = { N_("Always"), N_("Ask"), N_("Never") };
/* Fast-forward speed of Select+R2 (ff_speed, read by the game process) */
static const char *const g_ff_modes[] = { "2", "3", "4" };
static const char *const g_ff_labels[] = { "2x", "3x", "4x" };

static void games_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	(void)m;
	(void)dir;
	switch (it->id) {
	case ID_AUTOSAVE:
		save_setting(ui, "autosave_exit", it->on ? "1" : "0", false);
		break;
	case ID_RESUMEMODE:
		save_setting(ui, "resume_mode", g_resume_modes[it->val], false);
		break;
	case ID_RESUMEBOOT:
		save_setting(ui, "resume_boot", g_rboot_modes[it->val], false);
		break;
	case ID_FFSPEED:
		save_setting(ui, "ff_speed", g_ff_modes[it->val], false);
		break;
	}
}

static void open_games(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Games"), K_GAMES);
	struct menu_item *it;

	menu_add(m, MI_HEADER, 0, _("Resume games"));
	it = menu_add(m, MI_TOGGLE, ID_AUTOSAVE, _("Auto-save on exit"));
	it->on = settings_get_bool(ui->settings, "autosave_exit", true);
	it = menu_add(m, MI_CHOICE, ID_RESUMEMODE, _("On launch"));
	set_choices(it, g_resume_labels, 3,
		    index_of(g_resume_modes, 3, settings_get(ui->settings, "resume_mode", "ask")));
	/* TRANSLATORS: Settings > Games: what happens at start-up after the
	 * console was turned off during a game */
	it = menu_add(m, MI_CHOICE, ID_RESUMEBOOT, _("Resume on boot"));
	set_choices(it, g_rboot_labels, 3,
		    index_of(g_rboot_modes, 3, settings_get(ui->settings, "resume_boot", "ask")));
	menu_add(m, MI_INFO, 0, _("Powering off in a game always saves it"))->disabled = true;
	/* TRANSLATORS: Settings > Games: section of the in-game shortcuts */
	menu_add(m, MI_HEADER, 0, _("In game"));
	/* TRANSLATORS: Settings > Games: how fast SELECT + R2 runs the game */
	it = menu_add(m, MI_CHOICE, ID_FFSPEED, _("Fast-forward speed"));
	set_choices(it, g_ff_labels, 3, index_of(g_ff_modes, 3, settings_get(ui->settings, "ff_speed", "3")));
	m->on_item = games_item;
	menu_open(ui, m);
}

/* ----------------------------------------------------------------- storage */
static void reboot_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0)
		ui_power(ui, UI_REBOOT);
}

static void storage_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	const char *ov = it->id == ID_EMMC ? "emmc" : "sata";
	const char *buttons[3];
	int r;

	(void)m;
	switch (it->id) {
	case ID_IMPORT:
	case ID_EXPORT:
	case ID_SAVESBK:
		if (!dir)
			transfer_usb_action(ui, it->id == ID_IMPORT ? USB_ACT_IMPORT :
					    it->id == ID_EXPORT ? USB_ACT_EXPORT : USB_ACT_SAVES);
		return;
	case ID_EJECT:
		if (!dir)
			transfer_eject_all(ui);
		return;
	case ID_USBPROMPT:
		save_setting(ui, "usb_import_prompt", it->on ? "1" : "0", false);
		return;
	default:
		break;
	}
	if (it->id != ID_EMMC && it->id != ID_SATA)
		return;
	r = hw_env_set_overlay(ui->cfg.boot_env, ov, it->on);
	if (r < 0) {
		it->on = !it->on;
		message_open(ui, _("Could not update /boot/rsos.env."));
		return;
	}
	/* TRANSLATORS: dialog buttons, uppercase, short */
	buttons[0] = _("REBOOT NOW");
	buttons[1] = _("LATER");
	buttons[2] = NULL;
	dialog_open(ui, _("Reboot required: the change applies at the next start."), buttons,
		    reboot_choice, NULL);
}

static void storage_refresh(struct ui *ui, struct menu *m)
{
	struct menu_item *it = menu_find(m, ID_FREE);
	uint64_t fr, tot;
	char a[32], b[32];

	if (it && hw_storage(ui->cfg.roms_dir, &fr, &tot)) {
		i18n_format_size(fr, a, sizeof(a));
		i18n_format_size(tot, b, sizeof(b));
		/* TRANSLATORS: storage: "12.5 GB free of 29.1 GB" */
		snprintf(it->value, sizeof(it->value), _("%s free of %s"), a, b);
	}
}

/* name is a word of ui->cfg.storage_overlays */
static bool has_storage_overlay(const struct ui *ui, const char *name)
{
	const char *s = ui->cfg.storage_overlays;
	size_t n = strlen(name);

	while (s && *s) {
		while (*s == ' ' || *s == ',' || *s == '\t')
			s++;
		if (!strncmp(s, name, n) && (!s[n] || s[n] == ' ' || s[n] == ',' || s[n] == '\t'))
			return true;
		while (*s && *s != ' ' && *s != ',' && *s != '\t')
			s++;
	}
	return false;
}

static void open_storage(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Storage"), K_STORAGE);
	struct menu_item *it;

	menu_add(m, MI_INFO, ID_FREE, _("Game storage"));
	if (ui->cfg.transfer) {
		menu_add(m, MI_HEADER, 0, _("USB drive"));
		menu_add(m, MI_ACTION, ID_IMPORT, _("Import games"));
		menu_add(m, MI_ACTION, ID_EXPORT, _("Export games"));
		menu_add(m, MI_ACTION, ID_SAVESBK, _("Back up saves"));
		menu_add(m, MI_ACTION, ID_EJECT, _("Eject USB drive"));
		it = menu_add(m, MI_TOGGLE, ID_USBPROMPT, _("Ask when a USB drive is plugged in"));
		it->on = settings_get_bool(ui->settings, "usb_import_prompt", true);
	}
	/* The board's opt-in device tree overlays (board profile:
	 * storage_overlays; the RetroStone2's eMMC and SATA) */
	if (has_storage_overlay(ui, "emmc") || has_storage_overlay(ui, "sata"))
		menu_add(m, MI_HEADER, 0, _("Advanced: off for a fast boot, restart needed"));
	if (has_storage_overlay(ui, "emmc")) {
		it = menu_add(m, MI_TOGGLE, ID_EMMC, _("Internal eMMC"));
		it->on = hw_env_has_overlay(ui->cfg.boot_env, "emmc");
	}
	if (has_storage_overlay(ui, "sata")) {
		it = menu_add(m, MI_TOGGLE, ID_SATA, _("M.2 SATA drive"));
		it->on = hw_env_has_overlay(ui->cfg.boot_env, "sata");
	}
	m->on_item = storage_item;
	m->on_refresh = storage_refresh;
	m->refresh_every = 3000;
	menu_open(ui, m);
}

/* ------------------------------------------------------------- system info */

static void info_refresh(struct ui *ui, struct menu *m)
{
	struct hw_battery bat;
	uint64_t fr, tot;
	char a[32], b[32];
	struct menu_item *it;
	int ngames = 0, nsys = 0;

	const struct power_status *st = ui_battery(ui);

	(void)bat;
	if ((it = menu_find(m, ID_INFO0 + 1))) {
		if (!st || !st->battery_present || st->percent < 0)
			strlcpy_(it->value, _("no battery"), sizeof(it->value));
		else if (st->minutes_left > 0 && st->state == BATT_CHARGING)
			/* TRANSLATORS: battery: percent, then the time until full: hours, minutes */
			snprintf(it->value, sizeof(it->value), _("%d %%, full in %d h %02d"), st->percent,
				 st->minutes_left / 60, st->minutes_left % 60);
		else if (st->minutes_left > 0)
			/* TRANSLATORS: battery: percent, then about how long it lasts: hours, minutes */
			snprintf(it->value, sizeof(it->value), _("%d %%, about %d h %02d"), st->percent,
				 st->minutes_left / 60, st->minutes_left % 60);
		else if (st->state == BATT_CHARGING)
			snprintf(it->value, sizeof(it->value), _("%d %%, charging"), st->percent);
		else if (st->state == BATT_FULL)
			snprintf(it->value, sizeof(it->value), _("%d %%, full"), st->percent);
		else
			/* TRANSLATORS: battery percent ("80 %") */
			snprintf(it->value, sizeof(it->value), _("%d %%"), st->percent);
	}
	if ((it = menu_find(m, ID_INFO0 + 2))) {
		if (st && st->voltage_mv > 0) {
			char v[32];

			i18n_format_number(st->voltage_mv / 1000.0, 2, v, sizeof(v));
			/* TRANSLATORS: power source and battery voltage ("charger, 4.12 V") */
			snprintf(it->value, sizeof(it->value), st->charger_online ? _("charger, %s V") :
				 _("battery, %s V"), v);
		} else {
			strlcpy_(it->value, st && st->charger_online ? _("charger connected") : _("battery"),
				 sizeof(it->value));
		}
	}
	if ((it = menu_find(m, ID_INFO0 + 7))) {
		if (st && st->temp_mc != POWER_TEMP_UNKNOWN)
			snprintf(it->value, sizeof(it->value), st->hot ? _("%d °C (hot)") : _("%d °C"),
				 st->temp_mc / 1000);
		else
			strlcpy_(it->value, "-", sizeof(it->value));
	}
	if ((it = menu_find(m, ID_INFO0 + 3))) {
		if (hw_storage(ui->cfg.roms_dir, &fr, &tot)) {
			i18n_format_size(fr, a, sizeof(a));
			i18n_format_size(tot, b, sizeof(b));
			snprintf(it->value, sizeof(it->value), _("%s free of %s"), a, b);
		} else {
			strlcpy_(it->value, _("not mounted"), sizeof(it->value));
		}
	}
	if ((it = menu_find(m, ID_INFO0 + 4)))
		/* TRANSLATORS: display: HDMI or LCD, then the menu's size in pixels */
		snprintf(it->value, sizeof(it->value), _("%s, UI %dx%d"), ui->hdmi ? "HDMI" : "LCD",
			 ui->w, ui->h);
	for (int i = 0; i < ui->nsys; i++) {
		if (ui->sys[i].is_collection)
			continue;
		nsys++;
		ngames += ui->sys[i].games ? ui->sys[i].games->n : ui->sys[i].count;
	}
	if ((it = menu_find(m, ID_INFO0 + 5))) {
		char sys[64];

		snprintf(sys, sizeof(sys), _n("%d system", "%d systems", nsys), nsys);
		/* TRANSLATORS: "25 games in 3 systems": %s is the "3 systems" part */
		snprintf(it->value, sizeof(it->value), _n("%d game in %s", "%d games in %s", ngames), ngames,
			 sys);
	}
	if ((it = menu_find(m, ID_INFO0 + 6)))
		snprintf(it->value, sizeof(it->value), "%d",
			 ui->cfg.input ? input_device_count(ui->cfg.input) : 0);
}

static void open_info(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("System information"), K_INFO);
	struct menu_item *it;
	char ver[96];

	hw_version(ui->cfg.version, ver, sizeof(ver));
	it = menu_add(m, MI_INFO, ID_INFO0, _("Version"));
	strlcpy_(it->value, ver, sizeof(it->value));
	menu_add(m, MI_INFO, ID_INFO0 + 1, _("Battery"));
	/* TRANSLATORS: where the power comes from (charger or battery) */
	menu_add(m, MI_INFO, ID_INFO0 + 2, C_("power source", "Power"));
	menu_add(m, MI_INFO, ID_INFO0 + 3, _("Storage"));
	menu_add(m, MI_INFO, ID_INFO0 + 4, _("Display"));
	menu_add(m, MI_INFO, ID_INFO0 + 5, _("Games"));
	menu_add(m, MI_INFO, ID_INFO0 + 6, _("Input devices"));
	menu_add(m, MI_INFO, ID_INFO0 + 7, _("CPU temperature"));
	m->on_refresh = info_refresh;
	m->refresh_every = 2000;
	menu_open(ui, m);
}

/* ------------------------------------------------------------------- power */
static void power_choice(struct ui *ui, int choice, void *user)
{
	if (choice == 0)
		ui_power(ui, (enum ui_power_action)(intptr_t)user);
}

/* power settings: settings.ini keys owned by the power module (docs/power.md §15).
 * No sleep entries: the power key powers off, there is no sleep mode (§5). */
static const char *const g_dim_vals[] = { "0", "1", "2", "5" };
/* TRANSLATORS: dim / turn off the screen after: never, or minutes */
static const char *const g_dim_labels[] = { N_("Never"), N_("1 min"), N_("2 min"), N_("5 min") };
static const char *const g_off_vals[] = { "0", "2", "5", "10" };
static const char *const g_off_labels[] = { N_("Never"), N_("2 min"), N_("5 min"), N_("10 min") };
/* the automatic power-off (docs/power.md "Idle power-off"): never, or minutes */
static const char *const g_autooff_vals[] = { "0", "5", "10", "15", "30", "60" };
static const char *const g_autooff_labels[] = { N_("Never"), N_("5 min"), N_("10 min"), N_("15 min"),
						 N_("30 min"), N_("60 min") };
static const char *const g_gauge_vals[] = { "auto", "voltage", "axp" };
/* TRANSLATORS: how the battery level is measured: automatic, from the voltage,
 * or the power chip's (AXP) fuel gauge */
static const char *const g_gauge_labels[] = { N_("Automatic"), N_("Voltage"), N_("AXP fuel gauge") };

static void power_setting(struct ui *ui, const char *key, const char *value)
{
	save_setting(ui, key, value, true);
	if (ui->cfg.power && ui->cfg.power->set_setting &&
	    ui->cfg.power->set_setting(key, value) < 0)
		LOGW("ui: the power module refused %s=%s", key, value);
}

static void power_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	const char *yesno[3];

	(void)m;
	/* TRANSLATORS: dialog buttons, uppercase, short */
	yesno[0] = _("YES");
	yesno[1] = _("NO");
	yesno[2] = NULL;
	switch (it->id) {
	case ID_REBOOT:
		if (!dir)
			dialog_open(ui, _("Restart the console?"), yesno, power_choice,
				    (void *)(intptr_t)UI_REBOOT);
		break;
	case ID_POWEROFF:
		if (!dir)
			dialog_open(ui, _("Power off the console?"), yesno, power_choice,
				    (void *)(intptr_t)UI_POWER_OFF);
		break;
	case ID_DIM:
		power_setting(ui, "idle_dim_min", g_dim_vals[it->val]);
		break;
	case ID_OFF:
		power_setting(ui, "idle_off_min", g_off_vals[it->val]);
		break;
	case ID_AUTOOFF:
		power_setting(ui, "idle_poweroff_min", g_autooff_vals[it->val]);
		break;
	case ID_GAUGE:
		power_setting(ui, "battery_gauge", g_gauge_vals[it->val]);
		break;
	}
}

static void open_power(struct ui *ui)
{
	/* TRANSLATORS: Settings section: restart, power off, battery saving */
	struct menu *m = menu_new(ui, C_("settings", "Power"), K_POWER);
	struct menu_item *it;

	menu_add(m, MI_ACTION, ID_REBOOT, _("Restart"));
	menu_add(m, MI_ACTION, ID_POWEROFF, _("Power off"));
	menu_add(m, MI_HEADER, 0, _("Battery saving"));
	it = menu_add(m, MI_CHOICE, ID_DIM, _("Dim the screen after"));
	set_choices(it, g_dim_labels, 4, index_of(g_dim_vals, 4,
		    settings_get(ui->settings, "idle_dim_min", "2")));
	it = menu_add(m, MI_CHOICE, ID_OFF, _("Screen off after"));
	set_choices(it, g_off_labels, 4, index_of(g_off_vals, 4,
		    settings_get(ui->settings, "idle_off_min", "5")));
	/* TRANSLATORS: Settings > Power: turn the console off by itself after
	 * this long without a button pressed (never while copying or updating) */
	it = menu_add(m, MI_CHOICE, ID_AUTOOFF, _("Power off after"));
	set_choices(it, g_autooff_labels, 6, index_of(g_autooff_vals, 6,
		    settings_get(ui->settings, "idle_poweroff_min", "5")));
	it = menu_add(m, MI_CHOICE, ID_GAUGE, _("Battery gauge"));
	set_choices(it, g_gauge_labels, 3, index_of(g_gauge_vals, 3,
		    settings_get(ui->settings, "battery_gauge", "auto")));
	m->on_item = power_item;
	menu_open(ui, m);
}

/* -------------------------------------------------------------- date & time */
static const struct power_tz g_utc_only[] = { { "UTC", "UTC0" } };

static const struct power_tz *tz_list(struct ui *ui, int *n)
{
	if (ui->cfg.power && ui->cfg.power->timezones) {
		const struct power_tz *t = ui->cfg.power->timezones(n);

		if (t && *n > 0)
			return t;
	}
	*n = 1;
	return g_utc_only;
}

static void tz_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	int n;
	const struct power_tz *tz = tz_list(ui, &n);
	int i = it->id - ID_TZ0;

	(void)m;
	if (dir || i < 0 || i >= n)
		return;
	save_setting(ui, "timezone", tz[i].name, true);
	if (ui->cfg.power && ui->cfg.power->set_timezone)
		ui->cfg.power->set_timezone(tz[i].name);
	else
		setenv("TZ", tz[i].posix, 1), tzset();
	ui_toastf(ui, _("Time zone: %s"), tz[i].name);
	ui_pop(ui);
}

static void open_tz(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Time zone"), K_TZ);
	int n;
	const struct power_tz *tz = tz_list(ui, &n);
	const char *cur = settings_get(ui->settings, "timezone", "UTC");

	for (int i = 0; i < n && i < (int)ARRAY_SIZE(m->items); i++) {
		menu_add(m, MI_ACTION, ID_TZ0 + i, tz[i].name);
		if (!strcmp(tz[i].name, cur))
			m->cursor = i;
	}
	m->on_item = tz_item;
	menu_open(ui, m);
}

static void time_refresh(struct ui *ui, struct menu *m)
{
	struct menu_item *it = menu_find(m, ID_NOW);
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	if (it) {
		char d[48], hms[16];

		i18n_format_date((int64_t)t, d, sizeof(d));
		strftime(hms, sizeof(hms), "%H:%M:%S", &tm);
		snprintf(it->value, sizeof(it->value), "%s  %s", d, hms);
	}
	if ((it = menu_find(m, ID_TZ)))
		strlcpy_(it->value, settings_get(ui->settings, "timezone", "UTC"), sizeof(it->value));
}

static void time_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	struct tm tm;
	time_t t;

	switch (it->id) {
	case ID_TZ:
		if (!dir)
			open_tz(ui);
		break;
	case ID_SETTIME:
		if (dir)
			break;
		memset(&tm, 0, sizeof(tm));
		tm.tm_year = menu_find(m, ID_YEAR)->val - 1900;
		tm.tm_mon = menu_find(m, ID_MONTH)->val - 1;
		tm.tm_mday = menu_find(m, ID_DAY)->val;
		tm.tm_hour = menu_find(m, ID_HOUR)->val;
		tm.tm_min = menu_find(m, ID_MIN)->val;
		tm.tm_isdst = -1;
		t = mktime(&tm);  /* local time, in the configured zone */
		if (t == (time_t)-1) {
			message_open(ui, _("That date is not valid."));
			break;
		}
		if (ui->cfg.power && ui->cfg.power->set_time) {
			if (ui->cfg.power->set_time(t) < 0)
				message_open(ui, _("The clock could not be set."));
			else
				ui_toastf(ui, "%s", _("Date and time set"));
		} else {
			ui_toastf(ui, "%s", _("Setting the clock needs the power module"));
		}
		time_refresh(ui, m);
		break;
	}
}

static void open_time(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Date & time"), K_TIME);
	time_t t = time(NULL);
	struct tm tm;
	struct menu_item *it;

	localtime_r(&t, &tm);
	/* TRANSLATORS: the date and time now */
	menu_add(m, MI_INFO, ID_NOW, _("Now"));
	menu_add(m, MI_SUBMENU, ID_TZ, _("Time zone"));
	menu_add(m, MI_HEADER, 0, _("Set the clock"));
	it = menu_add(m, MI_NUMBER, ID_YEAR, _("Year"));
	it->min = 2024;
	it->max = 2060;
	it->val = CLAMP(tm.tm_year + 1900, it->min, it->max);
	it = menu_add(m, MI_NUMBER, ID_MONTH, _("Month"));
	it->min = 1;
	it->max = 12;
	it->val = tm.tm_mon + 1;
	it = menu_add(m, MI_NUMBER, ID_DAY, _("Day"));
	it->min = 1;
	it->max = 31;
	it->val = tm.tm_mday;
	it = menu_add(m, MI_NUMBER, ID_HOUR, _("Hour"));
	it->min = 0;
	it->max = 23;
	it->val = tm.tm_hour;
	it = menu_add(m, MI_NUMBER, ID_MIN, _("Minute"));
	it->min = 0;
	it->max = 59;
	it->val = tm.tm_min;
	menu_add(m, MI_ACTION, ID_SETTIME, _("Set date and time"));
	m->cursor = 1;
	m->on_item = time_item;
	m->on_refresh = time_refresh;
	m->refresh_every = 1000;
	menu_open(ui, m);
}

/* ---------------------------------------------------------------- main menu */
/* Settings > System update (ui/update_ui.c, docs/updates.md) */
enum { ID_SYSUPDATE = 290 };

static void main_refresh(struct ui *ui, struct menu *m)
{
	struct menu_item *it = menu_find(m, ID_THEME);

	if (it)
		strlcpy_(it->value, ui->theme_name, sizeof(it->value));
	if ((it = menu_find(m, ID_SYSUPDATE)))
		strlcpy_(it->value, update_menu_value(ui), sizeof(it->value));
}

static void main_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	(void)m;
	if (dir)
		return;
	switch (it->id) {
	case ID_DISPLAY: open_display(ui); break;
	case ID_CONTROLS: open_controls(ui); break;
	case ID_NET: open_network(ui); break;
	case ID_THEME: open_theme(ui); break;
	case ID_LISTS: open_lists(ui); break;
	case ID_GAMES: open_games(ui); break;
	case ID_STORAGE: open_storage(ui); break;
	case ID_INFO: open_info(ui); break;
	case ID_SYSUPDATE: update_open(ui); break;
	case ID_DATETIME: open_time(ui); break;
	case ID_POWER: open_power(ui); break;
	case ID_LANGUAGE: language_open(ui, false); break;
	case ID_SEARCHALL: search_all_open(ui); break;
	}
}

/* "Language", plus "(Language)" in any other language: whoever switched to
 * a language they cannot read still finds the way back. */
static void language_label(char *out, size_t n)
{
	/* TRANSLATORS: Settings item that opens the list of languages */
	const char *l = _("Language");

	if (strcmp(l, "Language"))
		snprintf(out, n, "%s (Language)", l);
	else
		snprintf(out, n, "%s", l);
}

void settings_open_main(struct ui *ui)
{
	struct menu *m = menu_new(ui, _("Settings"), K_MAIN);
	char lang[96];

	menu_add(m, MI_SUBMENU, ID_DISPLAY, _("Display"));
	menu_add(m, MI_SUBMENU, ID_CONTROLS, _("Controls"));
	menu_add(m, MI_SUBMENU, ID_NET, _("Network"));
	menu_add(m, MI_SUBMENU, ID_THEME, _("Theme"));
	menu_add(m, MI_SUBMENU, ID_LISTS, _("Game lists"));
	menu_add(m, MI_SUBMENU, ID_GAMES, _("Games"));
	menu_add(m, MI_SUBMENU, ID_STORAGE, _("Storage"));
	menu_add(m, MI_SUBMENU, ID_INFO, _("System information"));
	if (update_available(ui))
		/* TRANSLATORS: Settings item: check for and install a new RetroStoneOS version */
		menu_add(m, MI_SUBMENU, ID_SYSUPDATE, _("System update"));
	menu_add(m, MI_SUBMENU, ID_DATETIME, _("Date & time"));
	menu_add(m, MI_SUBMENU, ID_POWER, C_("settings", "Power"));
	/* TRANSLATORS: Settings item: find a game by its name in every system */
	menu_add(m, MI_ACTION, ID_SEARCHALL, _("Search all games"));
	/* last: one press of UP from the top (the menu wraps), and the
	 * positions of the other items stay as they were */
	language_label(lang, sizeof(lang));
	menu_add(m, MI_SUBMENU, ID_LANGUAGE, lang);
	m->on_item = main_item;
	m->on_refresh = main_refresh;
	m->refresh_every = 500;
	menu_open(ui, m);
}

/* ---------------------------------------------------------------- language */
/*
 * Settings > Language: every language in its own name, the one in use
 * ticked; choosing one applies it at once (ui_set_language: catalog, fonts,
 * system names, themes, views) and reopens this menu in the new language.
 */
static void language_refresh(struct ui *ui, struct menu *m)
{
	(void)ui;
	for (int i = 0; i < m->n; i++) {
		const struct i18n_lang *l = i18n_lang_at(m->items[i].id - ID_LANG0);

		strlcpy_(m->items[i].value, l && !strcmp(l->code, i18n_language()) ? "\xe2\x9c\x93" : "",
			 sizeof(m->items[i].value));
	}
}

static void language_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	const struct i18n_lang *l = i18n_lang_at(it->id - ID_LANG0);

	(void)m;
	if (dir || !l)
		return;
	if (!strcmp(l->code, i18n_language()) && settings_get(ui->settings, "language", NULL))
		return;
	/* closes the menus (m is gone after this), reloads everything */
	if (ui_set_language(ui, l->code, true) < 0)
		ui_toastf(ui, _("%s is not installed"), l->native);
	settings_open_main(ui);
	if (ui_top(ui) && screen_is_menu(ui_top(ui))) {
		struct menu *mm = (struct menu *)ui_top(ui);
		struct menu_item *li = menu_find(mm, ID_LANGUAGE);

		if (li)
			mm->cursor = (int)(li - mm->items);
	}
	language_open(ui, false);
}

/* ---- the first-boot picker: full screen, big rows, must choose */
struct langpick {
	struct screen base;
	int cursor, top;
};

static void lp_preview(struct ui *ui, struct langpick *lp)
{
	/* the title and help in the language under the cursor, so a child who
	 * cannot read English recognises it; applied for real on A */
	const struct i18n_lang *l = i18n_lang_at(lp->cursor);

	if (!l)
		return;
	ui_assets_invalidate(ui, true);   /* the asset worker uses the fonts too */
	i18n_set_language(l->code);
	/* the views below hold fonts (game list): drop them first */
	for (int i = 0; i < ui->nstack; i++)
		if (ui->stack[i]->ops->relayout)
			ui->stack[i]->ops->relayout(ui, ui->stack[i]);
	font_cache_clear();
	font_setup_dir(ui->fonts_dir);
}

static void lp_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	struct langpick *lp = (struct langpick *)scr;
	const struct menu_style *ms = &ui->ms;
	int W = ui->w, H = ui->h;
	struct font *ft = font_get(ms->font_bold[0] ? ms->font_bold : NULL, ui_font_px(ui, 0.07f));
	struct font *f = font_get(ms->font_path, ui_font_px(ui, 0.058f));
	int n = i18n_lang_count();
	int row = (int)lroundf((float)font_height(f) * 1.7f);
	int list_y = H * 22 / 100, list_h = H * 64 / 100;
	int rows = MAX(3, list_h / row), lw = MIN(W * 8 / 10, H * 11 / 10), lx = (W - lw) / 2;
	/* TRANSLATORS: first-boot language picker: help bar, one short word each */
	static const struct help_prompt prompts[] = {
		{ "updown", N_("choose") }, { "a", N_("OK") },
	};

	if (lp->cursor < lp->top + 1)
		lp->top = lp->cursor - 1;
	if (lp->cursor > lp->top + rows - 2)
		lp->top = lp->cursor - rows + 2;
	lp->top = CLAMP(lp->top, 0, MAX(0, n - rows));
	gfx_fill(s, 0, 0, W, H, (ms->bg & 0x00ffffffu) | 0xff000000u);
	/* TRANSLATORS: first-boot language picker title (shown in each language as
	 * the cursor moves: keep it short and simple, children read it) */
	draw_text_box(ui, s, ft, _("Choose your language"), 0, H * 5 / 100, W, H * 13 / 100, AL_CENTER, ms->title);
	gfx_fill(s, lx, H * 19 / 100, lw, MAX(1, H / 240), ms->separator);
	for (int i = lp->top; i < n && i < lp->top + rows; i++) {
		const struct i18n_lang *l = i18n_lang_at(i);
		int y = list_y + (i - lp->top) * row;
		bool sel = i == lp->cursor;

		if (sel)
			gfx_fill_round(s, lx, y + 2, lw, row - 4, (row - 4) / 3, ms->selector);
		draw_text_box(ui, s, f, l->native, lx, y, lw, row, AL_CENTER, sel ? ms->sel_text : ms->text);
	}
	/* more above / below */
	if (lp->top > 0)
		draw_text_box(ui, s, f, "\xe2\x96\xb2", 0, list_y - row * 2 / 3, W, row * 2 / 3, AL_CENTER, ms->text_dim);
	if (lp->top + rows < n)
		draw_text_box(ui, s, f, "\xe2\x96\xbc", 0, list_y + rows * row, W, row * 2 / 3, AL_CENTER,
			      ms->text_dim);
	help_draw(ui, s, &ui->menu_help, prompts, 2);
}

static void lp_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	struct langpick *lp = (struct langpick *)scr;
	int n = i18n_lang_count();
	const struct i18n_lang *l;

	if (t == IN_NAV_RELEASE)
		return;
	switch (b) {
	case IN_UP:
	case IN_LEFT:
		lp->cursor = (lp->cursor + n - 1) % n;
		lp_preview(ui, lp);
		break;
	case IN_DOWN:
	case IN_RIGHT:
		lp->cursor = (lp->cursor + 1) % n;
		lp_preview(ui, lp);
		break;
	case IN_A:
	case IN_START:
		if (t != IN_NAV_PRESS)
			break;
		l = i18n_lang_at(lp->cursor);
		LOGI("ui: first boot: language %s chosen", l->code);
		ui->lang_prompt = false;
		ui->lang_picker = NULL;
		ui_pop(ui);           /* frees lp */
		ui_set_language(ui, l->code, true);
		return;
	default:
		break;            /* B, SELECT...: a language must be chosen */
	}
	ui->dirty = true;
}

static void lp_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	struct langpick *lp = (struct langpick *)scr;
	const struct i18n_lang *l = i18n_lang_at(lp->cursor);

	(void)ui;
	snprintf(buf, n, "language:%s|%s|%s", _("Choose your language"), l ? l->code : "", l ? l->native : "");
}

static void lp_destroy(struct ui *ui, struct screen *s)
{
	if (ui->lang_picker == s)
		ui->lang_picker = NULL;
	free(s);
}

static const struct screen_ops lp_ops = {
	.button = lp_button,
	.render = lp_render,
	.relayout = bt_relayout,
	.destroy = lp_destroy,
	.opaque = true,
	.describe = lp_describe,
};

void language_open(struct ui *ui, bool first_boot)
{
	struct menu *m;
	char title[96];

	if (first_boot) {
		struct langpick *lp = xcalloc(1, sizeof(*lp));
		const struct i18n_lang *en = i18n_lang_find("en");

		lp->base.ops = &lp_ops;
		for (int i = 0; i < i18n_lang_count(); i++)
			if (i18n_lang_at(i) == en)
				lp->cursor = i;             /* nothing chosen yet: English */
		ui->lang_picker = &lp->base;
		if (!ui_push(ui, &lp->base))
			ui->lang_picker = NULL;
		return;
	}
	language_label(title, sizeof(title));
	m = menu_new(ui, title, K_LANG);
	for (int i = 0; i < i18n_lang_count(); i++) {
		const struct i18n_lang *l = i18n_lang_at(i);

		menu_add(m, MI_ACTION, ID_LANG0 + i, l->native);
		if (!strcmp(l->code, i18n_language()))
			m->cursor = i;
	}
	m->on_item = language_item;
	m->on_refresh = language_refresh;
	menu_open(ui, m);
}

/* ------------------------------------------------------------ game options */
static char g_default_label[96];
static char g_sys_default_label[160];
static const char *g_core_ids[MAX_CORES_PER_SYSTEM + 1];
static char g_core_labels[MAX_CORES_PER_SYSTEM][128];

/* The game options menu keeps which game it is about, not a pointer into
 * the list: toggling Favorite re-sorts that list in place (review F-H4:
 * later actions went to whichever game moved into the slot). */
struct game_ref {
	char system[32];
	char path[1024];
};

static void game_ref_free(struct ui *ui, struct menu *m)
{
	(void)ui;
	free(m->ctx);
	m->ctx = NULL;
}

/* The game in the menu's list now (NULL: gone). */
static struct game *game_ref_find(struct menu *m)
{
	const struct game_ref *r = m->ctx;
	struct gamelist *gl = m->sys ? m->sys->games : NULL;

	if (!r || !gl)
		return NULL;
	for (int i = 0; i < gl->n; i++)
		if (!strcmp(gl->games[i].path, r->path) && !strcmp(gl->games[i].system, r->system))
			return &gl->games[i];
	return NULL;
}

/* Every other copy of the game (its system's list, Favorites, Last
 * played) gets the new core too. */
static void game_core_sync(struct ui *ui, const struct game *g)
{
	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *gl = ui->sys[i].games;

		for (int j = 0; gl && j < gl->n; j++)
			if (&gl->games[j] != g && !strcmp(gl->games[j].system, g->system) &&
			    !strcmp(gl->games[j].rel, g->rel))
				gl->games[j].core = g->core;
	}
}

/* ------------------------------------------------ game options (batch 2) */
static const char *const g_gscale_keys[] = { "", "aspect", "integer", "stretch" };
static const char *const g_gcpu_keys[] = { "", "performance", "powersave" };
static char g_gscale_default[96];

/* The other copies of the game (collections) get its per-game settings. */
static void game_opts_sync(struct ui *ui, const struct game *g)
{
	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *gl = ui->sys[i].games;

		for (int j = 0; gl && j < gl->n; j++)
			if (&gl->games[j] != g && !strcmp(gl->games[j].system, g->system) &&
			    !strcmp(gl->games[j].rel, g->rel)) {
				gl->games[j].scale = g->scale;
				gl->games[j].cpu = g->cpu;
				gl->games[j].hidden = g->hidden;
			}
	}
}

/* The list view under the menus (the game options' list), or NULL. */
static struct screen *list_below(struct ui *ui)
{
	for (int i = ui->nstack - 1; i >= 0; i--)
		if (ui->stack[i]->kind == SCR_GLVIEW)
			return ui->stack[i];
	return NULL;
}

/* Search this list: the keyboard over the list, the list filtered as the
 * text changes (the title counts the matches); OK keeps the filter, SELECT
 * (cancel) puts back the one there was, B in the list clears it. */
static char g_search_before[96];

static void search_title(struct ui *ui, struct screen *osk, struct screen *list)
{
	char t[96], n[32];
	int rows = 0;

	(void)ui;
	if (list && glview_search(list)[0]) {
		struct sysent *se = glview_system(list);

		for (int i = 0; se->games && i < se->games->n; i++)
			rows += games_match(se->games->games[i].name, glview_search(list));
		i18n_format_number(rows, 0, n, sizeof(n));
		/* TRANSLATORS: title of the search keyboard, with the number of games
		 * that match what is typed so far */
		snprintf(t, sizeof(t), _n("Search: %s game", "Search: %s games", rows), n);
	} else {
		/* TRANSLATORS: title of the search keyboard (nothing typed yet) */
		snprintf(t, sizeof(t), "%s", _("Search this list"));
	}
	osk_set_title(osk, t);
}

static void search_live(struct ui *ui, struct screen *osk, const char *text, void *user)
{
	struct screen *list = list_below(ui);

	(void)user;
	if (!list)
		return;
	glview_set_search(ui, list, text);
	search_title(ui, osk, list);
}

static void search_done(struct ui *ui, const char *text, bool ok, void *user)
{
	struct screen *list = list_below(ui);

	(void)user;
	if (list)
		glview_set_search(ui, list, ok ? text : g_search_before);
}

static void list_search_open(struct ui *ui)
{
	struct screen *list, *k;

	ui_pop(ui);                        /* the game options: the list is under the keyboard */
	list = list_below(ui);
	if (!list)
		return;
	strlcpy_(g_search_before, glview_search(list), sizeof(g_search_before));
	k = osk_open(ui, _("Search this list"), g_search_before, false, search_done, NULL);
	if (!k)
		return;
	osk_set_live(k, search_live);
	search_title(ui, k, list);
}

/* "Search all games" (Settings): every list, then a results view. */
static void search_all_done(struct ui *ui, const char *text, bool ok, void *user)
{
	struct screen *v;
	char q[96];

	(void)user;
	strlcpy_(q, text, sizeof(q));
	if (!ok || !*str_trim(q))
		return;
	loader_complete_all(ui);
	v = glview_create_search_all(ui, str_trim(q));
	if (!v) {
		/* TRANSLATORS: toast: "Search all games" found nothing; %s is the text searched */
		ui_toastf(ui, _("No game found for \"%s\""), str_trim(q));
		return;
	}
	/* the settings close; the results come over the view they were opened from */
	while (ui->nstack > 1 && !ui_top(ui)->ops->opaque)
		ui_pop(ui);
	ui_push(ui, v);
}

static void search_all_open(struct ui *ui)
{
	osk_open(ui, _("Search all games"), "", false, search_all_done, NULL);
}

/* Hide this game: out of the lists now (unless hidden games are shown) and
 * at every start (gamedb). */
static void game_hide(struct ui *ui, struct menu *m, struct game *g, bool hide)
{
	char system[32], path[1024];
	bool shown = settings_get_bool(ui->settings, "show_hidden", false);

	gamedb_set_hidden(ui->db, g, hide);
	game_opts_sync(ui, g);
	gamedb_save(ui->db);
	strlcpy_(system, g->system, sizeof(system));
	strlcpy_(path, g->path, sizeof(path));
	(void)m;
	if (hide && !shown) {
		ui_pop(ui);                /* the options of a game that is not listed any more */
		glview_game_removed(ui, system, path);
		/* TRANSLATORS: toast after "Hide this game"; the menu path must match
		 * your translations of Settings, Game lists and Show hidden games */
		ui_toast_long(ui, "%s", _("Game hidden: Settings > Game lists > Show hidden games shows it again"));
	} else {
		for (int i = 0; i < ui->nstack; i++)
			if (ui->stack[i]->kind == SCR_GLVIEW)
				glview_refresh(ui, ui->stack[i]);   /* the "(hidden)" mark */
		ui_invalidate_snapshot(ui);
	}
}

/*
 * Delete this game: "Delete <file>?" (the file name; CANCEL selected), then
 * only the selected file goes (a .m3u or .cue keeps the discs/tracks it
 * lists, and says so); if the game has saves or save states, "Also delete
 * its saves?" (NO selected, B = no). The game leaves the lists and gamedb.
 */
struct del_ctx {
	char system[32], path[1024], rel[1024], name[256], core_path[512];
};

static struct del_ctx g_del;

/* Saves and states of the game: count (remove = false) or delete them.
 * The launch callback's helper knows the exact names (a zip's inner file);
 * without it, <data>/saves|states/<system>/<ROM stem>.* */
static int game_saves(struct ui *ui, const struct del_ctx *d, bool remove)
{
	struct ui_launch req;
	char stem[256], root[1024], dir[1100], *dot;
	static const char *const kinds[2] = { "saves", "states" };
	int n = 0;

	memset(&req, 0, sizeof(req));
	req.rom_path = d->path;
	req.system = d->system;
	req.core_path = d->core_path;
	req.game_name = d->name;
	if (ui->cfg.cb.game_saves)
		return ui->cfg.cb.game_saves(&req, remove, ui->cfg.cb.user);
	strlcpy_(stem, path_basename(d->path), sizeof(stem));
	if ((dot = strrchr(stem, '.')) && dot != stem)
		*dot = 0;
	path_dirname(ui->cfg.data_dir, root, sizeof(root));
	for (int k = 0; k < 2; k++) {
		DIR *dh;
		struct dirent *de;

		snprintf(dir, sizeof(dir), "%s/%s/%s", root, kinds[k], d->system);
		dh = opendir(dir);
		while (dh && (de = readdir(dh))) {
			size_t l = strlen(stem);
			char p[1400];

			if (strncmp(de->d_name, stem, l) || de->d_name[l] != '.')
				continue;
			n++;
			snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
			if (remove && unlink(p) < 0)
				LOGW("ui: cannot delete %s: %s", p, strerror(errno));
		}
		if (dh)
			closedir(dh);
	}
	return n;
}

static void game_delete_finish(struct ui *ui, bool with_saves)
{
	if (with_saves) {
		int n = game_saves(ui, &g_del, true);

		if (n == -EEXIST) {
			/* another ROM got the same save name meanwhile: kept */
			message_open(ui, _("Game deleted. Its saves were kept: another game uses the same save files."));
			return;
		}
		LOGI("ui: deleted %d save/state files of %s", n, g_del.rel);
		/* TRANSLATORS: toast after deleting a game and its saves */
		ui_toastf(ui, "%s", _("Game and saves deleted"));
	} else {
		/* TRANSLATORS: toast after deleting a game (its saves are kept) */
		ui_toastf(ui, "%s", _("Game deleted"));
	}
}

static void delete_saves_answer(struct ui *ui, int choice, void *user)
{
	(void)user;
	game_delete_finish(ui, choice == 1);   /* 0 NO (selected), 1 YES, -1 (B) no */
}

static void delete_answer(struct ui *ui, int choice, void *user)
{
	const char *buttons[3];
	struct screen *d;
	int n;

	(void)user;
	if (choice != 0)
		return;                    /* CANCEL (selected) or B */
	if (unlink(g_del.path) < 0) {
		char msg[400];

		/* TRANSLATORS: the game file could not be deleted; %s is the system's reason */
		snprintf(msg, sizeof(msg), _("The game could not be deleted (%s)."), strerror(errno));
		message_open(ui, msg);
		return;
	}
	LOGI("ui: deleted %s", g_del.path);
	gamedb_forget(ui->db, g_del.system, g_del.rel);
	gamedb_save(ui->db);
	/* the options menu of the deleted game */
	while (ui->nstack > 1 && ui_top(ui)->kind != SCR_GLVIEW && screen_is_menu(ui_top(ui)))
		ui_pop(ui);
	glview_game_removed(ui, g_del.system, g_del.path);
	n = game_saves(ui, &g_del, false);
	if (n == -EEXIST) {
		/* Game.zip next to Game.sfc, the same name in two folders: the
		 * other game's saves are the same files (review) */
		LOGI("ui: saves of %s kept: another game uses the same save name", g_del.rel);
		/* TRANSLATORS: after "Delete this game": the saves question is not asked, because
		 * another game file has the same name (the saves belong to both) */
		message_open(ui, _("Game deleted. Its saves were kept: another game uses the same save files."));
		return;
	}
	if (n <= 0) {
		game_delete_finish(ui, false);
		return;
	}
	/* TRANSLATORS: dialog buttons, uppercase, short (NO is selected) */
	buttons[0] = _("NO");
	buttons[1] = _("YES");
	buttons[2] = NULL;
	/* TRANSLATORS: second question after deleting a game */
	d = dialog_open(ui, _("Also delete its saves and save states?"), buttons, delete_saves_answer, NULL);
	dialog_select(d, 0);
}

/* A game of a read-only folder of the system (the RetroStone VC games in
 * <builtin_games_dir>/<system>/): part of the image, never deleted. A path
 * prefix only counts up to a '/' ("/usr/share/rsos/games2" is not in it). */
static bool game_is_builtin(const struct ui *ui, const struct game *g)
{
	const char *b = ui->cfg.builtin_games_dir;
	size_t l = b ? strlen(b) : 0;

	return l && g->path && !strncmp(g->path, b, l) && g->path[l] == '/';
}

static void game_delete_ask(struct ui *ui, struct game *g)
{
	const char *buttons[3], *ext = strrchr(path_basename(g->path), '.');
	char text[900];
	const char *core_name;
	const struct core_info *ci = systems_core(ui_game_core(ui, g, &core_name));
	struct screen *d;

	if (game_is_builtin(ui, g))
		return;   /* no Delete item for it (game_options_open) */
	memset(&g_del, 0, sizeof(g_del));
	strlcpy_(g_del.system, g->system, sizeof(g_del.system));
	strlcpy_(g_del.path, g->path, sizeof(g_del.path));
	strlcpy_(g_del.rel, g->rel, sizeof(g_del.rel));
	strlcpy_(g_del.name, g->name, sizeof(g_del.name));
	strlcpy_(g_del.core_path, ci ? ci->library : "", sizeof(g_del.core_path));
	/* TRANSLATORS: delete confirmation; %s is the game's file name (French
	 * quotes: « %s ») */
	snprintf(text, sizeof(text), _("Delete \"%s\" from the SD card?"), path_basename(g->path));
	if (ext && (!strcasecmp(ext, ".m3u") || !strcasecmp(ext, ".cue")))
		snprintf(text + strlen(text), sizeof(text) - strlen(text), "\n%s",
			 !strcasecmp(ext, ".m3u") ?
			 /* TRANSLATORS: added to the delete confirmation of a multi-disc playlist */
			 _("Only this playlist is deleted: the disc files it lists stay on the SD card.") :
			 /* TRANSLATORS: added to the delete confirmation of a CD image's .cue file */
			 _("Only this .cue file is deleted: the track files it lists stay on the SD card."));
	/* TRANSLATORS: dialog buttons, uppercase, short (CANCEL is selected) */
	buttons[0] = _("DELETE");
	buttons[1] = _("CANCEL");
	buttons[2] = NULL;
	d = dialog_open(ui, text, buttons, delete_answer, NULL);
	dialog_select(d, 1);
}

static void game_item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	struct game *g = game_ref_find(m);
	struct sysent *se = m->sys;
	char key[64];

	if (!g) {
		ui_pop(ui);           /* frees m */
		ui_toast(ui, _("This game is not in the list any more"), UI_SEV_WARNING);
		return;
	}
	switch (it->id) {
	case ID_FAV:
		gamedb_set_favorite(ui->db, g, it->on);
		gamedb_save(ui->db);
		if (ui->nstack >= 2)
			glview_refresh(ui, ui->stack[ui->nstack - 2]);   /* re-sorts: g moves */
		ui_invalidate_snapshot(ui);
		break;
	case ID_GAMECORE:
		gamedb_set_core(ui->db, g, it->val == 0 ? NULL : g_core_ids[it->val]);
		game_core_sync(ui, g);
		gamedb_save(ui->db);
		break;
	case ID_SYSCORE:
		snprintf(key, sizeof(key), "core.%s", g->system);
		save_setting(ui, key, it->val == 0 ? "" : g_core_ids[it->val], false);
		break;
	case ID_LAUNCH:
		if (dir == 0) {
			ui_pop(ui);
			ui_launch_game(ui, se, g);
		}
		break;
	case ID_GSCALE:
		gamedb_set_scale(ui->db, g, g_gscale_keys[it->val]);
		game_opts_sync(ui, g);
		gamedb_save(ui->db);
		break;
	case ID_GCPU:
		gamedb_set_cpu(ui->db, g, g_gcpu_keys[it->val]);
		game_opts_sync(ui, g);
		gamedb_save(ui->db);
		break;
	case ID_GSEARCH:
		if (dir == 0)
			list_search_open(ui);
		break;
	case ID_GHIDE:
		game_hide(ui, m, g, it->on);
		break;
	case ID_GDELETE:
		if (dir == 0)
			game_delete_ask(ui, g);
		break;
	}
}

void game_options_open(struct ui *ui, struct sysent *se, struct game *g)
{
	struct menu *m = menu_new(ui, g->name, K_GAME);
	const struct core_info *cores[MAX_CORES_PER_SYSTEM];
	const char *labels[MAX_CORES_PER_SYSTEM + 1];
	int nc = systems_cores_for_file(g->system, g->path, cores, MAX_CORES_PER_SYSTEM);
	struct menu_item *it;
	char key[64];
	const char *sys_core;
	const struct sysdef *sd = systems_find(g->system);
	struct game_ref *ref = xcalloc(1, sizeof(*ref));

	strlcpy_(ref->system, g->system, sizeof(ref->system));
	strlcpy_(ref->path, g->path, sizeof(ref->path));
	m->ctx = ref;
	m->on_destroy = game_ref_free;
	m->sys = se;
	/* TRANSLATORS: the game is one of the favorites (on/off) */
	it = menu_add(m, MI_TOGGLE, ID_FAV, _("Favorite"));
	it->on = g->favorite;
	/* the list's own options first: one press of DOWN */
	if (ui->nstack >= 1 && ui_top(ui) && ui_top(ui)->kind == SCR_GLVIEW) {
		const char *q = glview_search(ui_top(ui));

		/* TRANSLATORS: game options: find games of this list by name (the
		 * keyboard opens; the list shows the matches as you type) */
		it = menu_add(m, MI_ACTION, ID_GSEARCH, _("Search this list"));
		strlcpy_(it->value, q, sizeof(it->value));
	}
	/* core choices: [0] = inherit */
	snprintf(key, sizeof(key), "core.%s", g->system);
	sys_core = settings_get(ui->settings, key, "");
	/* TRANSLATORS: core for this game: the one chosen for the whole system */
	snprintf(g_default_label, sizeof(g_default_label), "%s", _("System default"));
	for (int i = 0; i < nc; i++) {
		if (cores[i]->experimental && !strstr(cores[i]->name, "xperimental"))
			/* TRANSLATORS: a core (emulator) name, marked as not finished */
			snprintf(g_core_labels[i], sizeof(g_core_labels[i]), _("%s (experimental)"), cores[i]->name);
		else
			snprintf(g_core_labels[i], sizeof(g_core_labels[i]), "%s", cores[i]->name);
	}
	/* TRANSLATORS: the default core of a system, with its name */
	snprintf(g_sys_default_label, sizeof(g_sys_default_label), _("Default (%s)"),
		 /* TRANSLATORS: no core */
		 nc ? g_core_labels[0] : _("none"));
	g_core_ids[0] = "";
	for (int i = 0; i < nc; i++)
		g_core_ids[i + 1] = cores[i]->id;
	if (nc > 1) {
		int sel = 0;

		labels[0] = g_default_label;
		for (int i = 0; i < nc; i++) {
			labels[i + 1] = g_core_labels[i];
			if (g->core && !strcmp(g->core, cores[i]->id))
				sel = i + 1;
		}
		/* TRANSLATORS: "core" = the emulator used to run the game */
		it = menu_add(m, MI_CHOICE, ID_GAMECORE, _("Core for this game"));
		set_choices(it, labels, nc + 1, sel);
		labels[0] = g_sys_default_label;
		sel = 0;
		for (int i = 0; i < nc; i++)
			if (!strcmp(sys_core, cores[i]->id))
				sel = i + 1;
		{
			char l[96];

			/* TRANSLATORS: %s is a system name ("Core for all Game Boy games") */
			snprintf(l, sizeof(l), _("Core for all %s games"), sd ? C_("system", sd->fullname) : g->system);
			it = menu_add(m, MI_CHOICE, ID_SYSCORE, l);
		}
		set_choices(it, labels, nc + 1, sel);
	} else {
		it = menu_add(m, MI_INFO, 0, _("Core"));
		strlcpy_(it->value, nc ? g_core_labels[0] : _("none installed"), sizeof(it->value));
	}
	/* per-game scaling and CPU profile (gamedb; applied at launch, and the
	 * in-game menu changes them for this game too) */
	{
		const char *labels2[4];
		int sel = 0;

		snprintf(g_gscale_default, sizeof(g_gscale_default), _("Default (%s)"),
			 !strcmp(settings_get(ui->settings, "scaling", "aspect"), "integer") ? C_("scaling", "Integer") :
			 C_("scaling", "Aspect"));
		labels2[0] = g_gscale_default;
		labels2[1] = C_("scaling", "Aspect");
		labels2[2] = C_("scaling", "Integer");
		labels2[3] = C_("scaling", "Stretch");
		for (int i = 1; i < 4; i++)
			if (g->scale && !strcmp(g->scale, g_gscale_keys[i]))
				sel = i;
		/* TRANSLATORS: game options: how the picture fills the screen, for this game */
		it = menu_add(m, MI_CHOICE, ID_GSCALE, _("Scaling"));
		set_choices(it, labels2, 4, sel);
		labels2[0] = C_("cpu profile", "Automatic");
		labels2[1] = C_("cpu profile", "Performance");
		labels2[2] = C_("cpu profile", "Battery saver");
		sel = 0;
		for (int i = 1; i < 3; i++)
			if (g->cpu && !strcmp(g->cpu, g_gcpu_keys[i]))
				sel = i;
		/* TRANSLATORS: game options: how fast the processor runs for this game */
		it = menu_add(m, MI_CHOICE, ID_GCPU, _("CPU profile"));
		set_choices(it, labels2, 3, sel);
	}
	/* TRANSLATORS: start the game */
	menu_add(m, MI_ACTION, ID_LAUNCH, _("Launch"));
	/* TRANSLATORS: game options: take the game out of the lists (Settings >
	 * Game lists > Show hidden games brings it back) */
	it = menu_add(m, MI_TOGGLE, ID_GHIDE, _("Hide this game"));
	it->on = g->hidden;
	/* not for a game of a read-only folder of the system (the RetroStone VC
	 * games): it is part of the system, hiding it is the way */
	if (!game_is_builtin(ui, g))
		/* TRANSLATORS: game options: delete the game's file from the SD card (asks first) */
		menu_add(m, MI_ACTION, ID_GDELETE, _("Delete this game"));
	/* TRANSLATORS: game options: the time spent playing this game */
	it = menu_add(m, MI_INFO, ID_GPLAYTIME, _("Play time"));
	ui_format_playtime(g->playtime, it->value, sizeof(it->value));
	/* TRANSLATORS: the game's file name */
	it = menu_add(m, MI_INFO, ID_FILE, _("File"));
	strlcpy_(it->value, g->rel, sizeof(it->value));
	m->on_item = game_item;
	menu_open(ui, m);
}

/* ------------------------------------------------------ storage problem */
/*
 * /data could not be mounted (ui_data_problem, docs/ui-design.md "Storage
 * problem"): the system layer left the partition alone (a tmpfs stands in)
 * so nothing is lost yet. This opaque screen replaces the menu until the
 * unit powers off or reboots: B and Start do nothing, and data_problem_poll()
 * puts it back if anything closed it. "Turn off" powers off (the card can
 * then be read on a PC); "Format" asks again (CANCEL selected), then runs
 * the data-partition helper with --format-confirmed (a child process polled
 * with WNOHANG: the main loop and the watchdog go on) and reboots.
 */
enum { DP_ASK = 0, DP_FORMATTING, DP_FAILED, DP_DONE };

static void dp_text(struct ui *ui, char *out, size_t n)
{
	const char *p = ui->data_problem.problem, *arg = strchr(p, ' ');
	char fs[64] = "";

	if (arg) {
		strlcpy_(fs, arg + 1, sizeof(fs));
		fs[strcspn(fs, " \t")] = 0;
	}
	if (!strncmp(p, "readerror", 9))
		/* TRANSLATORS: storage problem screen (the data partition of the SD card could not be read) */
		strlcpy_(out, _("The SD card reports read errors: it may be failing."), n);
	else if (!strncmp(p, "foreign", 7) && fs[0])
		/* TRANSLATORS: storage problem screen; %s is a file system name ("ntfs", "ext4") */
		snprintf(out, n, _("The data partition uses a format this console does not use (%s)."), fs);
	else if (fs[0] && strcmp(fs, "unknown"))
		/* TRANSLATORS: storage problem screen; %s is a file system name ("exfat") */
		snprintf(out, n, _("The data partition (%s) could not be opened: it may be damaged."), fs);
	else
		/* TRANSLATORS: storage problem screen */
		strlcpy_(out, _("The data partition could not be opened: it may be damaged."), n);
}

static void dp_render(struct ui *ui, struct screen *scr, struct gfx_surface *s)
{
	const struct menu_style *ms = &ui->ms;
	struct font *f = font_get(ms->font_path, ui_font_px(ui, ms->font_size));
	struct font *fb = font_get(ms->font_bold[0] ? ms->font_bold : NULL, ui_font_px(ui, ms->font_size * 1.15f));
	int W = ui->w, H = ui->h, pad = MAX(8, H / 30), w = W - 4 * pad, x = 2 * pad, y = pad * 2;
	int lh = font_height(f) * 3 / 2, bh = font_height(f) * 2;
	char what[512], text[1024];
	int state = ui->data_problem.state;
	static const struct help_prompt prompts[] = { { "updown", N_("choose") }, { "a", N_("select") } };

	(void)scr;
	gfx_fill(s, 0, 0, W, H, 0xff000000u);       /* opaque, whatever the theme's panel alpha */
	gfx_fill(s, 0, 0, W, H, ms->bg);
	/* TRANSLATORS: title of the screen shown instead of the menu when the storage
	 * (the SD card's data partition: games, saves) cannot be read */
	draw_text_box(ui, s, fb, _("The storage could not be read"), x, y, w, lh, AL_CENTER, ms->title);
	y += lh * 3 / 2;
	if (state == DP_FORMATTING || state == DP_DONE) {
		strlcpy_(text, state == DP_DONE ?
			 /* TRANSLATORS: storage problem screen: formatted, the console restarts */
			 _("Done. Restarting...") :
			 /* TRANSLATORS: storage problem screen, while the format runs */
			 _("Formatting the storage, please wait. Do not turn the console off."), sizeof(text));
	} else {
		dp_text(ui, what, sizeof(what));
		snprintf(text, sizeof(text), "%s %s", what,
			 /* TRANSLATORS: storage problem screen, after what happened */
			 _("Nothing was erased: your games and saves may still be recoverable. Turn the console "
			   "off and read the SD card on a PC to copy them."));
		if (state == DP_FAILED) {
			char e[200];

			/* TRANSLATORS: storage problem screen: the format did not work; %d is its exit code */
			snprintf(e, sizeof(e), _("The storage could not be formatted (error %d)."), ui->data_problem.status);
			snprintf(text + strlen(text), sizeof(text) - strlen(text), " %s", e);
		}
	}
	{
		struct text_line lines[10];
		int n = font_wrap(f, text, w, lines, 10);

		for (int i = 0; i < n; i++)
			font_draw(s, f, x + (w - lines[i].width) / 2, font_baseline_in_box(f, y + i * lh, lh),
				  text + lines[i].start, lines[i].len, ms->text);
		y += MAX(n, 1) * lh + pad;
	}
	if (state == DP_FORMATTING || state == DP_DONE)
		return;
	for (int i = 0; i < 2; i++) {
		bool sel = i == ui->data_problem.sel;
		const char *label = i == 0 ?
			/* TRANSLATORS: storage problem screen: power off, to read the SD card on a computer */
			_("Turn off (to back up on a PC)") :
			/* TRANSLATORS: storage problem screen: erase the data partition and start again */
			_("Format the storage (erases everything)");

		gfx_fill_round(s, x + pad, y, w - 2 * pad, bh, bh / 4, sel ? ms->selector : gfx_with_alpha(ms->text_dim, 50));
		draw_text_box(ui, s, f, label, x + pad, y, w - 2 * pad, bh, AL_CENTER, sel ? ms->sel_text : ms->text);
		y += bh + pad / 2;
	}
	help_draw(ui, s, &ui->menu_help, prompts, 2);
}

/* The format helper: its own session (it finishes whatever happens to us),
 * SIGPIPE ignored (it may log to our stderr, a pipe). */
static void dp_format_start(struct ui *ui)
{
	const char *helper = ui->cfg.data_partition_helper;
	pid_t pid;

	if (!helper || !*helper || access(helper, X_OK) != 0) {
		LOGW("ui: storage: %s missing: cannot format", helper ? helper : "(none)");
		ui->data_problem.state = DP_FAILED;
		ui->data_problem.status = 127;
		ui->dirty = true;
		return;
	}
	pid = fork();
	if (pid < 0) {
		ui->data_problem.state = DP_FAILED;
		ui->data_problem.status = -errno;
		ui->dirty = true;
		return;
	}
	if (pid == 0) {
		sigset_t none;
		int fd = open("/dev/null", O_RDONLY);

		if (fd >= 0)
			dup2(fd, 0);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_IGN);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		setsid();
		execl(helper, helper, "--format-confirmed", (char *)NULL);
		_exit(127);
	}
	LOGI("ui: storage: formatting (%s --format-confirmed, pid %d)", helper, (int)pid);
	ui->data_problem.pid = (int)pid;
	ui->data_problem.state = DP_FORMATTING;
	ui->dirty = true;
}

static void dp_confirm(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice != 1) {
		LOGI("ui: storage: format cancelled");
		return;                 /* CANCEL (selected) or B: the screen again */
	}
	dp_format_start(ui);
}

static void dp_button(struct ui *ui, struct screen *scr, enum input_btn b, enum input_nav_type t)
{
	(void)scr;
	if (t == IN_NAV_RELEASE)
		return;
	if (ui->data_problem.state == DP_FORMATTING || ui->data_problem.state == DP_DONE)
		return;
	if (b == IN_UP || b == IN_DOWN) {
		ui->data_problem.sel = b == IN_UP ? 0 : 1;
		ui->dirty = true;
	} else if (b == IN_A && t == IN_NAV_PRESS) {
		if (ui->data_problem.sel == 0) {
			LOGI("ui: storage: turning off (to back up on a PC)");
			ui_power(ui, UI_POWER_OFF);
		} else {
			/* TRANSLATORS: dialog buttons, uppercase, short (CANCEL is selected) */
			const char *const buttons[] = { _("CANCEL"), _("FORMAT"), NULL };
			struct screen *d;

			/* TRANSLATORS: the second question before formatting the storage */
			d = dialog_open(ui, _("Erase everything on the storage? All games, saves and settings on it are "
					      "lost. This cannot be undone."), buttons, dp_confirm, NULL);
			dialog_select(d, 0);
		}
	}
	/* B, Start, Select: nothing (no menu while the storage is unreadable) */
}

static bool dp_update(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
	return false;
}

static int dp_timeout(struct ui *ui, struct screen *scr)
{
	(void)scr;
	return ui->data_problem.pid > 0 ? 250 : -1;
}

static void dp_relayout(struct ui *ui, struct screen *scr)
{
	(void)ui;
	(void)scr;
}

static void dp_destroy(struct ui *ui, struct screen *scr)
{
	if (ui->data_problem.scr == scr)
		ui->data_problem.scr = NULL;
	free(scr);
}

static void dp_describe(struct ui *ui, struct screen *scr, char *buf, size_t n)
{
	static const char *const states[] = { "ask", "formatting", "failed", "done" };

	(void)scr;
	snprintf(buf, n, "storage:%s %s|%s", states[ui->data_problem.state & 3], ui->data_problem.problem,
		 ui->data_problem.sel ? "Format" : "Turn off");
}

static const struct screen_ops dp_ops = {
	.button = dp_button,
	.update = dp_update,
	.render = dp_render,
	.relayout = dp_relayout,
	.destroy = dp_destroy,
	.timeout = dp_timeout,
	.opaque = true,
	.describe = dp_describe,
};

static void dp_push(struct ui *ui)
{
	struct screen *s = xcalloc(1, sizeof(*s));

	s->ops = &dp_ops;
	if (ui_push(ui, s))
		ui->data_problem.scr = s;
}

void ui_data_problem(struct ui *ui, const char *problem)
{
	if (!ui || !problem || ui->data_problem.active)
		return;
	ui->data_problem.active = true;
	strlcpy_(ui->data_problem.problem, problem, sizeof(ui->data_problem.problem));
	LOGI("ui: storage problem \"%s\": the storage screen instead of the menu", problem);
	/* over the carousel only: whatever was open goes */
	while (ui->nstack > 1)
		ui_pop(ui);
	dp_push(ui);
}

bool ui_data_problem_busy(const struct ui *ui)
{
	return ui && ui->data_problem.pid > 0;
}

void data_problem_poll(struct ui *ui)
{
	bool there = false;

	if (!ui->data_problem.active)
		return;
	if (ui->data_problem.pid > 0) {
		int st = 0;
		pid_t r = waitpid((pid_t)ui->data_problem.pid, &st, WNOHANG);

		if (r == (pid_t)ui->data_problem.pid || (r < 0 && errno == ECHILD)) {
			int code = r <= 0 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) :
				   WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;

			ui->data_problem.pid = 0;
			ui->dirty = true;
			if (code == 0) {
				LOGI("ui: storage: formatted, rebooting");
				ui->data_problem.state = DP_DONE;
				ui_power(ui, UI_REBOOT);
			} else {
				LOGW("ui: storage: the format failed (%d)", code);
				ui->data_problem.state = DP_FAILED;
				ui->data_problem.status = code;
			}
		}
	}
	for (int i = 0; i < ui->nstack; i++)
		there |= ui->stack[i] == ui->data_problem.scr;
	if (!there) {
		ui->data_problem.scr = NULL;
		dp_push(ui);
	}
}
