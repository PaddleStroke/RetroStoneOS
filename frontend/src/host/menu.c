/*
 * menu.c - the in-game menu (Select+X), replacing RetroArch's RGUI.
 *
 * The game is paused (no retro_run, audio faded to silence) and the game
 * plane is switched to a 640x480 XRGB8888 canvas (1:1 on the LCD, scaled
 * by the plane on HDMI); the game surface is restored on resume. Drawing
 * goes through draw.h only (TrueType text in the 8x8 font's cells at 2x,
 * or that font itself without fonts), so the UI agent can re-skin it by
 * reimplementing the cv_* functions on top of src/gfx. Every text is
 * translated (i18n.h) and cut to its place in pixels ("...").
 *
 * Pages: main (resume, save/load state with a slot picker and thumbnail,
 * disc, reset, core options, scaling, CPU profile, FPS, fast-forward,
 * benchmark, controls, exit), core options (edit, save per game or per
 * system, reset; changes are saved for this game when the page or the menu
 * is left), controls, benchmark (start) and benchmark results ("Use this").
 * Scaling and the CPU profile changed here are sent to the menu process
 * ("setting scale|cpu <value>" status lines) when the menu closes, which
 * keeps them for this game (gamedb.tsv).
 */
#include <drm_fourcc.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../audio/audio.h"
#include "draw.h"
#include "host_input.h"
#include "host_internal.h"
#include "bench.h"
#include "host_png.h"
#include "hutil.h"
#include "../i18n/i18n.h"
#include "options.h"
#include "saves.h"

#define CW 640
#define CH 480
#define FS 2              /* font scale */
#define ROW 26
#define LIST_X 24
#define LIST_Y 64
#define LIST_W 372
#define VIS_ROWS 14

#define COL_BG      0xe8101828u
#define COL_PANEL   0xe0182030u
#define COL_SEL     0xff3050a0u
#define COL_TEXT    0xe8e8e8u
#define COL_DIM     0x8890a0u
#define COL_VALUE   0xffd060u
#define COL_TITLE   0xffffffu

enum page { PG_MAIN = 0, PG_OPTIONS, PG_CONTROLS, PG_BENCH, PG_BENCH_RESULTS };

enum item_id {
	IT_RESUME, IT_SAVE, IT_LOAD, IT_DISC, IT_RESET, IT_OPTIONS, IT_SCALE, IT_FPS,
	IT_FF, IT_CONTROLS, IT_EXIT, IT_BENCH, IT_BENCH_GO, IT_BENCH_USE,
	IT_OPT_SAVE_GAME, IT_OPT_SAVE_SYS, IT_OPT_RESET, IT_DPAD_ANALOG, IT_INFO, IT_CPU,
	/* Core option i is IT_OPT_BASE + i: this range must stay last (review
	 * F-H3: IT_DPAD_ANALOG and IT_INFO used to equal options #1 and #2). */
	IT_OPT_BASE = 1000,
};
#define IT_OPT_MAX 900
_Static_assert(IT_INFO < IT_OPT_BASE && IT_DPAD_ANALOG < IT_OPT_BASE, "menu item ids overlap the core options");

static bool is_option(int id)
{
	return id >= IT_OPT_BASE && id < IT_OPT_BASE + IT_OPT_MAX;
}

/* Labels and values are translated UTF-8 (up to ~40 % longer than English,
 * 3 bytes per CJK character); draw() cuts them to the row in pixels. */
struct item {
	int id;
	char label[128];
	char value[128];
	const char *en;   /* English msgid of a fixed label (menu scripts), or NULL */
};

static struct {
	uint32_t *px;
	uint32_t *bg;
	struct canvas cv;
	enum page page;
	int sel, top;
	struct item items[160];
	int n;
	bool close;
	int disc;
	char status[256];
	int thumb_slot;
	uint8_t *thumb;
	int tw, th;
	bool thumb_none;
	bool dpad_analog;
	/* per-game settings when the menu opened: a change is reported to the
	 * menu UI when it closes (saved for this game) */
	int scale0;
	char cpu0[16];
} M;

static const char *scale_name(int s)
{
	/* TRANSLATORS: in-game menu > Scaling values (right-aligned on a 640 px row) */
	return s == DISPLAY_SCALE_INTEGER ? C_("scaling", "Integer") :
	       s == DISPLAY_SCALE_STRETCH ? C_("scaling", "Stretch") : C_("scaling", "Aspect");
}

static const char *scale_key(int s)
{
	return s == DISPLAY_SCALE_INTEGER ? "integer" : s == DISPLAY_SCALE_STRETCH ? "stretch" : "aspect";
}

/* The per-game CPU profiles (the menu UI applies them: power_set_game_cpu). */
static const char *const g_cpu_keys[] = { "auto", "performance", "powersave" };

static const char *cpu_name(const char *key)
{
	/* TRANSLATORS: in-game menu > CPU profile values: the emulator's own
	 * setting / full speed / slower and cooler, saves battery */
	return !strcmp(key, "performance") ? C_("cpu profile", "Performance") :
	       !strcmp(key, "powersave") ? C_("cpu profile", "Battery saver") : C_("cpu profile", "Automatic");
}

/* "< On >" / "< Off >" (the arrows: Left/Right change the value) into buf. */
static const char *on_off(bool on, char *buf, size_t n)
{
	/* TRANSLATORS: in-game menu switch values, shown as "< On >" / "< Off >" */
	snprintf(buf, n, "< %s >", on ? C_("switch", "On") : C_("switch", "Off"));
	return buf;
}

/* The core option's value source, after its value (docs/host-design.md §5). */
static const char *source_tag(enum opt_source s)
{
	switch (s) {
	case OPT_SRC_UNSAVED:
		return "*";
	case OPT_SRC_GAME:
		/* TRANSLATORS: after a core option's value: saved for this game
		 * (keep it short; the help line of the page explains it) */
		return C_("option source", "(game)");
	case OPT_SRC_SYSTEM:
		/* TRANSLATORS: after a core option's value: saved for all games of this core */
		return C_("option source", "(all)");
	case OPT_SRC_OVERRIDE:
		/* TRANSLATORS: after a core option's value: set by a benchmark run */
		return C_("option source", "(bench)");
	default:
		return "";
	}
}

/* The status words of a benchmark run (bench.h), for the results page. */
static const char *bench_status_name(const char *st)
{
	/* TRANSLATORS: benchmark results page: what happened to a run */
	return !strcmp(st, "crash") ? C_("benchmark", "crashed") :
	       !strcmp(st, "hang") ? C_("benchmark", "hung") :
	       !strcmp(st, "abort") ? C_("benchmark", "stopped") :
	       !strcmp(st, "error") ? C_("benchmark", "failed") : st;
}

static struct item *add(int id, const char *label, const char *value)
{
	struct item *it;

	if (M.n >= (int)(sizeof(M.items) / sizeof(M.items[0])))
		return NULL;
	it = &M.items[M.n++];
	it->id = id;
	it->en = NULL;
	hstrlcpy(it->label, label, sizeof(it->label));
	hstrlcpy(it->value, value ? value : "", sizeof(it->value));
	return it;
}

/* A fixed label: msgid is the English text (marked with N_()). */
static void add_t(int id, const char *msgid, const char *value)
{
	struct item *it = add(id, _(msgid), value);

	if (it)
		it->en = msgid;
}

static void build(void)
{
	char v[128], t[96];

	M.n = 0;
	switch (M.page) {
	case PG_MAIN:
		/* TRANSLATORS: in-game menu (Select+X) rows: the label on the left
		 * of a 372 px row, a value like "< Slot 1 >" on the right; longer
		 * text is cut with "..." */
		add_t(IT_RESUME, N_("Resume"), NULL);
		/* TRANSLATORS: in-game menu value, shown as "< Slot 3 >" */
		snprintf(t, sizeof(t), _("Slot %d"), H.slot);
		snprintf(v, sizeof(v), "< %s >", t);
		add_t(IT_SAVE, N_("Save state"), v);
		add_t(IT_LOAD, N_("Load state"), v);
		if (H.has_disk && H.disk.get_num_images) {
			snprintf(v, sizeof(v), "< %d / %u >", M.disc + 1, H.disk.get_num_images());
			/* TRANSLATORS: in-game menu: the CD of a multi-disc game */
			add_t(IT_DISC, N_("Disc"), v);
		}
		/* TRANSLATORS: in-game menu: restart the game (as the console's reset button) */
		add_t(IT_RESET, N_("Reset"), NULL);
		if (opts_count())
			add_t(IT_OPTIONS, N_("Core options"), ">");
		snprintf(v, sizeof(v), "< %s >", scale_name(H.scale));
		add_t(IT_SCALE, N_("Scaling"), v);
		/* saved for this game by the menu UI, like the scaling */
		snprintf(v, sizeof(v), "< %s >", cpu_name(H.cpu_profile));
		/* TRANSLATORS: in-game menu: how fast the processor runs for this game */
		add_t(IT_CPU, N_("CPU profile"), v);
		add_t(IT_FPS, N_("Show FPS"), on_off(H.show_stats, t, sizeof(t)));
		if (H.ff_speed > 1)
			snprintf(v, sizeof(v), "< x%d >", H.ff_speed);
		else
			on_off(false, v, sizeof(v));
		add_t(IT_FF, N_("Fast-forward"), v);
		if (host_bench_available(NULL, 0))
			add_t(IT_BENCH, N_("Benchmark this game"), ">");
		add_t(IT_CONTROLS, N_("Controls"), ">");
		add_t(IT_EXIT, N_("Exit game"), NULL);
		break;
	case PG_OPTIONS:
		for (int i = 0; i < opts_count(); i++) {
			const struct core_opt *o = opts_at(i);
			const char *tag;

			if (!o->visible || i >= IT_OPT_MAX)
				continue;
			/* the core's label and value (not translated), then ours */
			tag = source_tag(opts_source(i));
			snprintf(v, sizeof(v), "%s%s%s", o->vals[o->cur].label, *tag ? " " : "", tag);
			hutf8_trim(v);
			add(IT_OPT_BASE + i, o->desc, v);
		}
		/* TRANSLATORS: in-game menu > Core options (640 px rows) */
		add_t(IT_OPT_SAVE_GAME, N_("Save for this game"), NULL);
		add_t(IT_OPT_SAVE_SYS, N_("Save for all games (this core)"), NULL);
		add_t(IT_OPT_RESET, N_("Reset to defaults"), NULL);
		break;
	case PG_CONTROLS:
		for (int p = 0; p < HOST_MAX_PORTS; p++) {
			struct input_port_info pi;
			char l[64];

			hin_port_info(p, &pi);
			/* TRANSLATORS: in-game menu > Controls: the row of a player */
			snprintf(l, sizeof(l), _("Player %d"), p + 1);
			hutf8_trim(l);
			if (!pi.connected)
				hstrlcpy(v, "-", sizeof(v));
			else if (pi.has_analog)
				/* TRANSLATORS: in-game menu > Controls: a controller's name, and it has an analog stick */
				snprintf(v, sizeof(v), _("%s (stick)"), pi.name);
			else
				hstrlcpy(v, pi.name, sizeof(v));
			hutf8_trim(v);
			add(IT_INFO, l, v);
		}
		add_t(IT_DPAD_ANALOG, N_("D-pad as left stick (P1)"), on_off(M.dpad_analog, t, sizeof(t)));
		for (int i = 0; i < H.ndesc; i++) {
			/* the buttons' printed names are not translated, the d-pad
			 * directions are */
			static const char *const names[16] = {
				"B", "Y", "Select", "Start", NC_("d-pad", "Up"), NC_("d-pad", "Down"),
				NC_("d-pad", "Left"), NC_("d-pad", "Right"), "A", "X", "L", "R", "L2", "R2", "L3", "R3",
			};
			const char *bn;
			char l[96];

			if (H.desc[i].port != 0)
				continue;
			if (H.desc[i].device == RETRO_DEVICE_JOYPAD && H.desc[i].id < 16)
				bn = H.desc[i].id >= 4 && H.desc[i].id <= 7 ? C_("d-pad", names[H.desc[i].id]) :
									       names[H.desc[i].id];
			else if (H.desc[i].device == RETRO_DEVICE_ANALOG)
				bn = H.desc[i].index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? _("Left stick") : _("Right stick");
			else
				continue;
			snprintf(l, sizeof(l), "  %s", bn);
			add(IT_INFO, l, H.desc[i].desc);
		}
		break;
	case PG_BENCH: {
		char d[96];

		host_bench_available(d, sizeof(d));
		/* TRANSLATORS: in-game menu > Benchmark this game: info rows
		 * (label on the left, value on the right of a 640 px row) */
		add_t(IT_INFO, N_("Runs"), d);
		add_t(IT_INFO, N_("Each run"), _("from now, no input"));
		/* TRANSLATORS: benchmark info row; its value is the key combination that stops it */
		add_t(IT_INFO, N_("Stop"), "Select+Start");
		/* TRANSLATORS: where the benchmark report is written: keep the path */
		hstrlcpy(v, _("/rsos/logs (SD card)"), sizeof(v));
		add_t(IT_INFO, N_("Results"), v);
		add_t(IT_BENCH_GO, N_("Start benchmark"), NULL);
		break;
	}
	case PG_BENCH_RESULTS: {
		const struct host_bench_report *br = host_bench_report();

		for (int k = 0; br && k < br->n && k < 10; k++) {
			const struct bench_result *r = &br->r[br->order[k]];
			char l[96];

			/* the configuration's label comes from the plan (English) */
			snprintf(l, sizeof(l), "%d. %s", k + 1, r->label);
			hutf8_trim(l);
			if (bench_eligible(r)) {
				char fps[32];

				i18n_format_number(r->eff_fps, 1, fps, sizeof(fps));
				snprintf(v, sizeof(v), "%.0f%% %sfps", r->speed, fps);
			} else {
				/* TRANSLATORS: benchmark results: a run that started from
				 * the game's boot (not comparable), or whose last picture
				 * was one colour */
				snprintf(v, sizeof(v), "%s", strcmp(r->status, "ok") ? bench_status_name(r->status) :
							      r->from_boot ? _("not ranked") : _("blank?"));
			}
			add(IT_INFO, l, v);
		}
		if (br && br->best >= 0)
			add_t(IT_BENCH_USE, N_("Use this for this game"), br->r[br->best].id);
		add_t(IT_RESUME, N_("Resume game"), NULL);
		break;
	}
	}
	if (M.sel >= M.n)
		M.sel = M.n - 1;
	if (M.sel < 0)
		M.sel = 0;
}

static void load_thumb(int slot)
{
	char path[PATH_MAX + 8], sp[PATH_MAX];

	if (slot == M.thumb_slot && (M.thumb || M.thumb_none))
		return;
	host_png_free(M.thumb);
	M.thumb = NULL;
	M.thumb_none = false;
	M.thumb_slot = slot;
	state_path(slot, sp, sizeof(sp));
	if (hpath(path, sizeof(path), "%s.png", sp))
		M.thumb = host_png_read_rgb(path, &M.tw, &M.th);
	M.thumb_none = !M.thumb;
}

/* One row: the label on the left, the value right-aligned; each cut with
 * "..." in pixels so they never overlap (the value keeps at least half of
 * the row when both are too long). */
static void draw_row(struct canvas *c, const struct item *it, int x, int y, int w)
{
	char val[256];
	int vw = 0, gap = 12;

	if (it->value[0]) {
		int lw = cv_text_width(it->label, FS), vmax;

		vw = cv_text_width(it->value, FS);
		vmax = w - lw - gap;
		if (vmax < w / 2)
			vmax = w / 2;
		cv_text_ellipsize(it->value, FS, vmax, val, sizeof(val));
		vw = cv_text_width(val, FS);
		cv_text(c, x + w - vw, y, val, COL_VALUE, FS);
		vw += gap;
	}
	cv_text_fit(c, x, y, it->label, it->id == IT_INFO ? COL_DIM : COL_TEXT, FS, w - vw);
}

static void draw(void)
{
	struct canvas *c = &M.cv;
	const char *title = M.page == PG_OPTIONS ? _("Core options") : M.page == PG_CONTROLS ? _("Controls") :
			    /* TRANSLATORS: in-game menu page titles (640 px, large text) */
			    M.page == PG_BENCH ? _("Benchmark") : M.page == PG_BENCH_RESULTS ? _("Benchmark results") :
			    H.game;
	char buf[512];
	int cw;

	memcpy(M.px, M.bg, (size_t)CW * CH * 4);
	cv_fill(c, 12, 12, CW - 24, CH - 24, COL_BG);
	/* the core's name on the right, the title cut before it */
	snprintf(buf, sizeof(buf), "%s%s", H.info.display_name[0] ? H.info.display_name : H.core_id,
		 M.page == PG_OPTIONS && opts_dirty() ? " *" : "");
	cw = cv_text_fit(c, CW - 24 - (cv_text_width(buf, 1) < 200 ? cv_text_width(buf, 1) : 200), 28, buf,
			 COL_DIM, 1, 200);
	cv_text_fit(c, LIST_X, 24, title, COL_TITLE, FS, CW - 24 - cw - 16 - LIST_X);
	cv_fill(c, LIST_X, 50, CW - 2 * LIST_X, 2, 0xff3050a0u);

	if (M.sel < M.top)
		M.top = M.sel;
	if (M.sel >= M.top + VIS_ROWS)
		M.top = M.sel - VIS_ROWS + 1;
	for (int r = 0; r < VIS_ROWS && M.top + r < M.n; r++) {
		int y = LIST_Y + r * ROW;
		int w = M.page == PG_MAIN ? LIST_W : CW - 2 * LIST_X;

		if (M.top + r == M.sel)
			cv_fill(c, LIST_X - 6, y - 5, w + 12, ROW, COL_SEL);
		draw_row(c, &M.items[M.top + r], LIST_X, y, w);
	}
	if (M.top > 0)
		cv_text(c, CW / 2 - 8, LIST_Y - 14, "^", COL_DIM, FS);
	if (M.top + VIS_ROWS < M.n)
		cv_text(c, CW / 2 - 8, LIST_Y + VIS_ROWS * ROW - 8, "v", COL_DIM, FS);

	/* Right panel on the main page: the slot preview. */
	if (M.page == PG_MAIN) {
		int px = LIST_X + LIST_W + 24, pw = CW - px - 24, ph = pw * 3 / 4;
		time_t mt;

		cv_fill(c, px - 4, LIST_Y - 4, pw + 8, ph + 8, COL_PANEL);
		load_thumb(H.slot);
		if (M.thumb) {
			cv_blit_rgb(c, px, LIST_Y, pw, ph, M.thumb, M.tw, M.th);
		} else {
			/* TRANSLATORS: in-game menu: the save state slot holds nothing
			 * (centred in a 176 px preview box) */
			cv_text_ellipsize(_("Empty"), FS, pw - 8, buf, sizeof(buf));
			cv_text(c, px + pw / 2 - cv_text_width(buf, FS) / 2, LIST_Y + ph / 2 - 8, buf, COL_DIM, FS);
		}
		/* TRANSLATORS: in-game menu: under the preview (176 px) */
		snprintf(buf, sizeof(buf), _("Slot %d"), H.slot);
		cv_text_fit(c, px, LIST_Y + ph + 12, buf, COL_TEXT, FS, pw);
		if (state_exists(H.slot, &mt)) {
			i18n_format_datetime((int64_t)mt, buf, sizeof(buf));
			cv_text_fit(c, px, LIST_Y + ph + 36, buf, COL_DIM, 1, pw);
		}
	}
	/* Info line: option help, or the last status. */
	buf[0] = 0;
	if (M.page == PG_OPTIONS && M.n && is_option(M.items[M.sel].id)) {
		const struct core_opt *o = opts_at(M.items[M.sel].id - IT_OPT_BASE);

		if (o && o->info && *o->info)
			hstrlcpy(buf, o->info, sizeof(buf));
	}
	if (!buf[0] && M.page == PG_OPTIONS && !M.status[0])
		/* TRANSLATORS: in-game menu > Core options, help line (one 592 px
		 * line in small text): "(game)", "(all)" and "*" must match the
		 * tags shown after the values */
		hstrlcpy(buf, _("(game) saved for this game, (all) for all games, * saved on leaving"), sizeof(buf));
	if (!buf[0] && M.status[0])
		hstrlcpy(buf, M.status, sizeof(buf));
	cv_text_fit(c, LIST_X, CH - 58, buf, COL_DIM, 1, CW - 2 * LIST_X);
	/* TRANSLATORS: in-game menu, button help (one 592 px line): A and B are
	 * the button names, "</>" the d-pad Left/Right */
	cv_text_fit(c, LIST_X, CH - 40, M.page == PG_MAIN ? _("A: select  B: resume  </>: change") :
			  _("A: select  B: back  </>: change"), COL_DIM, FS, CW - 2 * LIST_X);
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(M.status, sizeof(M.status), fmt, ap);
	va_end(ap);
	hutf8_trim(M.status);
}

static void change(int dir)
{
	struct item *it = &M.items[M.sel];

	if (!M.n || it->id == IT_INFO)
		return;               /* info rows: nothing to change */
	switch (it->id) {
	case IT_SAVE:
	case IT_LOAD:
		H.slot = (H.slot + (dir > 0 ? 1 : 9)) % 10;
		break;
	case IT_DISC: {
		int n = (int)H.disk.get_num_images();

		if (n > 0)
			M.disc = (M.disc + (dir > 0 ? 1 : n - 1)) % n;
		break;
	}
	case IT_SCALE:
		H.scale = (H.scale + (dir > 0 ? 1 : 2)) % 3;
		break;
	case IT_CPU: {
		int i = 0;

		while (i < 2 && strcmp(g_cpu_keys[i], H.cpu_profile))
			i++;
		hstrlcpy(H.cpu_profile, g_cpu_keys[(i + (dir > 0 ? 1 : 2)) % 3], sizeof(H.cpu_profile));
		break;
	}
	case IT_FPS:
		H.show_stats = !H.show_stats;
		break;
	case IT_FF: {
		static const int steps[] = { 0, 2, 3, 4 };
		int i = 0;

		while (i < 3 && steps[i] != H.ff_speed)
			i++;
		host_set_ff(steps[(i + (dir > 0 ? 1 : 3)) % 4]);
		break;
	}
	case IT_DPAD_ANALOG:
		M.dpad_analog = !M.dpad_analog;
		hin_set_dpad_to_analog(0, M.dpad_analog);
		break;
	default:
		if (is_option(it->id))
			opts_step(it->id - IT_OPT_BASE, dir);
		break;
	}
	build();
}

static void activate(void)
{
	struct item *it = &M.items[M.sel];
	int r;

	if (!M.n || it->id == IT_INFO)
		return;               /* info rows do nothing */
	switch (it->id) {
	case IT_RESUME:
		M.close = true;
		break;
	case IT_SAVE:
		host_do_hotkey_save(H.slot);
		/* TRANSLATORS: in-game menu status line (one 592 px line, small text) */
		set_status(_("Saving slot %d..."), H.slot);
		M.thumb_slot = -99; /* reload the preview */
		break;
	case IT_LOAD:
		if (!state_exists(H.slot, NULL)) {
			set_status(_("Slot %d is empty"), H.slot);
			break;
		}
		host_do_hotkey_load(H.slot);
		M.close = true;
		break;
	case IT_DISC:
		if (H.disk.set_eject_state && H.disk.set_image_index) {
			H.disk.set_eject_state(true);
			r = H.disk.set_image_index((unsigned)M.disc);
			H.disk.set_eject_state(false);
			if (r)
				host_toast(_("Disc %d inserted"), M.disc + 1);
			else
				host_toast(_("Cannot change to disc %d"), M.disc + 1);
			M.close = r;
		}
		break;
	case IT_RESET:
		H.core.reset();
		host_toast("%s", C_("toast", "Reset"));
		M.close = true;
		break;
	case IT_OPTIONS:
		opts_refresh_display(); /* the core hides options that do not apply */
		M.page = PG_OPTIONS;
		M.sel = M.top = 0;
		break;
	case IT_CONTROLS:
		M.page = PG_CONTROLS;
		M.sel = M.top = 0;
		break;
	case IT_EXIT:
		host_request_quit(HOST_EXIT_OK);
		M.close = true;
		break;
	case IT_OPT_SAVE_GAME:
		r = opts_save(true);
		if (r)
			set_status("%s", _("Could not save the options"));
		else
			/* TRANSLATORS: status line; %s is the game's name */
			set_status(_("Saved for %s"), H.game);
		break;
	case IT_OPT_SAVE_SYS:
		r = opts_save(false);
		if (r)
			set_status("%s", _("Could not save the options"));
		else
			/* TRANSLATORS: status line; %s is the emulator core's name ("snes9x") */
			set_status(_("Saved for all %s games"), H.core_id);
		break;
	case IT_OPT_RESET:
		opts_reset();
		set_status("%s", _("Defaults restored (saved when you leave)"));
		break;
	case IT_BENCH:
		M.page = PG_BENCH;
		M.sel = M.top = 0;
		break;
	case IT_BENCH_GO:
		/* host.c starts it once the menu is closed (the process is
		 * replaced by the benchmark driver) */
		host_bench_request();
		M.close = true;
		break;
	case IT_BENCH_USE: {
		char msg[96];

		host_bench_use_best(msg, sizeof(msg));
		set_status("%s", msg);
		break;
	}
	case IT_SCALE:
	case IT_CPU:
	case IT_FPS:
	case IT_FF:
	case IT_DPAD_ANALOG:
	default:
		change(+1);
		return;
	}
	build();
}

/* Menu changes to core options are written for this game when the
 * options page or the menu is left (they used to be lost unless "Save for
 * this game" was chosen). */
static void autosave(void)
{
	int r = opts_autosave();

	if (r > 0)
		set_status("%s", _("Options saved for this game"));
	else if (r < 0)
		set_status("%s", _("Could not save the options"));
}

/* Scaling and CPU profile are saved for this game by the menu UI ("setting"
 * status lines) when the menu closes with another value than it opened with. */
static void settings_snapshot(void)
{
	M.scale0 = H.scale;
	hstrlcpy(M.cpu0, H.cpu_profile, sizeof(M.cpu0));
}

static void settings_report(void)
{
	if (H.scale != M.scale0)
		host_setting_changed("scale", scale_key(H.scale));
	if (strcmp(H.cpu_profile, M.cpu0))
		host_setting_changed("cpu", H.cpu_profile);
	settings_snapshot();
}

static void back(void)
{
	if (M.page == PG_OPTIONS)
		autosave();
	if (M.page == PG_MAIN || M.page == PG_BENCH_RESULTS) {
		M.close = true;
	} else {
		int from = M.page == PG_OPTIONS ? IT_OPTIONS : M.page == PG_BENCH ? IT_BENCH : IT_CONTROLS;

		M.page = PG_MAIN;
		build();
		for (int i = 0; i < M.n; i++)
			if (M.items[i].id == from)
				M.sel = i;
	}
}

static void make_background(void)
{
	host_capture_last();
	uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;
	struct canvas bg = { M.bg, CW, CH, CW };
	double ar = H.av.geometry.aspect_ratio > 0 ? H.av.geometry.aspect_ratio :
		    H.last_h ? (double)H.last_w / H.last_h : 4.0 / 3;
	int w = CW, h = (int)(CW / ar);

	memset(M.bg, 0, (size_t)CW * CH * 4);
	if (h > CH) {
		h = CH;
		w = (int)(CH * ar);
	}
	if (H.last_frame)
		cv_blit_frame(&bg, (CW - w) / 2, (CH - h) / 2, w, h, H.last_frame, (int)H.last_w,
			      (int)H.last_h, (int)H.last_pitch, fmt, 80);
}

static void menu_run(enum page start);

void host_menu_run(void)
{
	menu_run(PG_MAIN);
}

void host_menu_run_bench_results(void)
{
	menu_run(PG_BENCH_RESULTS);
}

static void menu_run(enum page start)
{
	char msg[128];
	bool err, dirty = true;

	if (!H.display_ok)
		return;
	if (!M.px) {
		M.px = malloc((size_t)CW * CH * 4);
		M.bg = malloc((size_t)CW * CH * 4);
		if (!M.px || !M.bg) {
			free(M.px);
			free(M.bg);
			M.px = M.bg = NULL;
			return;
		}
	}
	M.cv = (struct canvas){ M.px, CW, CH, CW };
	M.page = start;
	M.sel = M.top = 0;
	M.close = false;
	M.status[0] = 0;
	M.thumb_slot = -99;
	if (start == PG_BENCH_RESULTS) {
		const struct host_bench_report *br = host_bench_report();

		if (br && br->path[0])
			/* TRANSLATORS: benchmark results page, status line: the report's path */
			set_status(_("Table: %s"), br->path);
	}
	{
		static bool once;

		if (!once)
			M.dpad_analog = !strcasecmp(H.system, "n64") && !hin_has_analog(0);
		once = true;
	}
	if (H.has_disk && H.disk.get_image_index)
		M.disc = (int)H.disk.get_image_index();
	settings_snapshot();
	hlog(HLOG_INFO, "menu open");

	audio_pause(true);
	hin_set_game_mode(false);
	make_background();
	host_video_invalidate();
	if (!display_set_game_surface(CW, CH, DRM_FORMAT_XRGB8888, 0)) {
		hin_set_game_mode(true);
		audio_pause(false);
		return;
	}
	display_set_scaling(DISPLAY_SCALE_ASPECT, 4.0 / 3.0);
	build();

	while (!M.close && !H.quit) {
		struct input_nav ev;
		struct pollfd pfd[2];
		int n = 0, to = hin_timeout_ms();

		host_heartbeat();
		if (dirty) {
			draw();
			display_present_copy(M.px, CW * 4);
			dirty = false;
		}
		pfd[n].fd = display_get_fd();
		pfd[n++].events = POLLIN;
		if (hin_fd() >= 0) {
			pfd[n].fd = hin_fd();
			pfd[n++].events = POLLIN;
		}
		if (to < 0 || to > 20)
			to = 20; /* audio keepalive */
		poll(pfd, (nfds_t)n, to);
		display_handle_events();
		audio_keepalive();
		hin_poll();
		host_poll_signals();
		while (saves_next_message(msg, sizeof(msg), &err)) {
			set_status("%s", msg);
			M.thumb_slot = -99;
			dirty = true;
		}
		{
			enum input_hotkey hk;

			while (hin_next_hotkey(&hk))
				if (hk == IN_HK_POWER_OFF && H.cfg.status_fd < 0) {
					H.poweroff = true;
					host_request_quit(HOST_EXIT_POWEROFF);
				}
		}
		while (hin_next_nav(&ev)) {
			if (ev.type == IN_NAV_RELEASE)
				continue;
			dirty = true;
			switch (ev.btn) {
			case IN_UP:
				M.sel = (M.sel + M.n - 1) % M.n;
				break;
			case IN_DOWN:
				M.sel = (M.sel + 1) % M.n;
				break;
			case IN_L:
				M.sel = M.sel >= VIS_ROWS ? M.sel - VIS_ROWS : 0;
				break;
			case IN_R:
				M.sel = M.sel + VIS_ROWS < M.n ? M.sel + VIS_ROWS : M.n - 1;
				break;
			case IN_LEFT:
				change(-1);
				break;
			case IN_RIGHT:
				change(+1);
				break;
			case IN_A:
				if (ev.type == IN_NAV_PRESS)
					activate();
				break;
			case IN_B:
			case IN_START:
				if (ev.type == IN_NAV_PRESS)
					back();
				break;
			default:
				break;
			}
		}
	}

	autosave();
	settings_report();
	hlog(HLOG_INFO, "menu closed");
	host_png_free(M.thumb);
	M.thumb = NULL;
	hin_set_game_mode(true);
	host_video_invalidate();
	host_video_setup();
	host_present_last();
	audio_pause(false);
	H.next_frame_us = 0;
}

/* Test aid (rsos-run --menu-shot): renders the main page and the core
 * options page to PNG files without a display. */
static int shot(const char *path)
{
	uint8_t *rgb = malloc((size_t)CW * CH * 3);
	size_t size;
	void *png;
	int ret = -1;

	if (!rgb)
		return -1;
	for (int i = 0; i < CW * CH; i++) {
		rgb[3 * i] = (uint8_t)(M.px[i] >> 16);
		rgb[3 * i + 1] = (uint8_t)(M.px[i] >> 8);
		rgb[3 * i + 2] = (uint8_t)M.px[i];
	}
	png = host_png_encode(rgb, CW, CH, &size);
	if (png)
		ret = hwrite_atomic(path, png, size, false);
	free(png);
	free(rgb);
	return ret;
}

int host_menu_screenshot(const char *path)
{
	char p2[PATH_MAX + 16];
	int ret;

	if (!M.px) {
		M.px = malloc((size_t)CW * CH * 4);
		M.bg = malloc((size_t)CW * CH * 4);
		if (!M.px || !M.bg)
			return -1;
	}
	M.cv = (struct canvas){ M.px, CW, CH, CW };
	M.thumb_slot = -99;
	make_background();
	M.page = PG_MAIN;
	M.sel = 1;
	M.top = 0;
	build();
	draw();
	ret = shot(path);
	opts_refresh_display();
	M.page = PG_OPTIONS;
	M.sel = M.top = 0;
	build();
	draw();
	hpath(p2, sizeof(p2), "%.*s-options.png", (int)(strlen(path) > 4 ? strlen(path) - 4 : strlen(path)), path);
	ret |= shot(p2);
	host_png_free(M.thumb);
	M.thumb = NULL;
	return ret;
}

/*
 * Test aid (rsos-run --menu-script): drives the menu without a display, as
 * the pad would. Comma-separated steps: sel=<label prefix> (select the
 * first matching item of the current page), a, b, l, r (left/right), u, d.
 * The menu is then closed as with B, with the same auto-save.
 */
int host_menu_script(const char *script)
{
	char buf[512], *save = NULL, *tok;
	int ret = 0;

	M.page = PG_MAIN;
	M.sel = M.top = 0;
	M.close = false;
	M.status[0] = 0;
	M.thumb_slot = -99;
	settings_snapshot();
	build();
	hstrlcpy(buf, script, sizeof(buf));
	for (tok = strtok_r(buf, ",", &save); tok && !M.close; tok = strtok_r(NULL, ",", &save)) {
		if (!strncmp(tok, "sel=", 4)) {
			int found = -1;

			for (int i = 0; i < M.n && found < 0; i++)
				/* the label shown, or its English text (any --lang) */
				if (!strncasecmp(M.items[i].label, tok + 4, strlen(tok + 4)) ||
				    (M.items[i].en && !strncasecmp(M.items[i].en, tok + 4, strlen(tok + 4))))
					found = i;
			if (found < 0) {
				hlog(HLOG_ERROR, "menu script: no item \"%s\" on this page", tok + 4);
				ret = -1;
				break;
			}
			M.sel = found;
		} else if (!strcmp(tok, "a")) {
			activate();
		} else if (!strcmp(tok, "b")) {
			back();
		} else if (!strcmp(tok, "l") || !strcmp(tok, "r")) {
			change(tok[0] == 'r' ? +1 : -1);
		} else if (!strcmp(tok, "u") || !strcmp(tok, "d")) {
			M.sel = (M.sel + (tok[0] == 'd' ? 1 : M.n - 1)) % M.n;
		} else {
			hlog(HLOG_ERROR, "menu script: unknown step \"%s\"", tok);
			ret = -1;
			break;
		}
		hlog(HLOG_INFO, "menu script: %s -> page %d, item \"%s\" = \"%s\"%s%s", tok, (int)M.page,
		     M.n ? M.items[M.sel].label : "", M.n ? M.items[M.sel].value : "", M.status[0] ? ", " : "",
		     M.status);
	}
	autosave();
	settings_report();
	hlog(HLOG_INFO, "menu script done%s%s", M.status[0] ? ": " : "", M.status);
	return ret;
}
