/*
 * ui_internal.h - shared by the files of src/ui. Not a public API.
 */
#ifndef RSOS_UI_INTERNAL_H
#define RSOS_UI_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "../gfx/font.h"
#include "../gfx/gfx.h"
#include "../gfx/image.h"
#include "../i18n/i18n.h"
#include "../theme/theme.h"
#include "games.h"
#include "hw.h"
#include "settings.h"
#include "systems.h"
#include "ui.h"
#include "util.h"

/* ------------------------------------------------------------ elements */
enum elem_kind { EK_IMAGE = 0, EK_TEXT, EK_DATETIME, EK_RATING, EK_NINEPATCH };

/* ES ThemeFlags subset: which properties a view lets the theme change. */
#define TF_POS     0x001u
#define TF_SIZE    0x002u
#define TF_ORIGIN  0x004u
#define TF_PATH    0x008u
#define TF_COLOR   0x010u
#define TF_FONT    0x020u   /* fontPath + fontSize */
#define TF_TEXT    0x040u
#define TF_STYLE   0x080u   /* alignment, uppercase, lineSpacing, bg color */
#define TF_ZINDEX  0x100u
#define TF_VISIBLE 0x200u
#define TF_ALL     0xfffu

enum { AL_LEFT = 0, AL_CENTER, AL_RIGHT };

/* One positioned element resolved from a theme element (or defaults). */
struct elem {
	enum elem_kind kind;
	char name[48];
	int order;
	bool extra;
	bool visible;
	float z;
	/* normalized geometry (0..1 of the screen) */
	float pos[2], size[2], maxsize[2], origin[2];
	bool has_maxsize;
	/* image / ninepatch */
	char path[1024];
	char def_path[1024];
	bool tile;
	gfx_color color, color_end;
	bool grad_h;
	/* text / datetime */
	char *text;               /* owned */
	gfx_color fg, bg;
	char font_path[1024];
	float font_size;          /* fraction of the screen height */
	int align;
	bool upper;
	float line_spacing;
	char fmt[32];
	bool relative;
	int64_t time;
	/* rating */
	float rating;
	char filled[1024], unfilled[1024];
	/* resolved, in pixels */
	struct gfx_image *img;    /* shared (img_get) */
	struct gfx_image *img2;   /* shared (rating unfilled) */
	bool icon_img, icon_img2; /* img/img2 are built-in stars owned by the element */
	struct gfx_image *cache;  /* owned (rendered text) */
	int x, y, w, h;
	bool laid_out;
};

void elem_init(struct elem *e, enum elem_kind kind, const char *name);
/* Applies the theme element's properties allowed by flags. */
void elem_apply(struct elem *e, const struct theme_elem *te, unsigned flags);
void elem_set_text(struct elem *e, const char *text);
void elem_set_path(struct elem *e, const char *path);
/* Computes the pixel rectangle and loads/renders the content. */
void elem_layout(struct ui *ui, struct elem *e);
void elem_draw(struct ui *ui, struct gfx_surface *s, struct elem *e, int dx, int dy);
void elem_release(struct elem *e);   /* drops images, keeps settings */
void elem_free(struct elem *e);      /* elem_release + text */
/* Builds an elem from any theme element type (extras). false if unsupported. */
bool elem_from_theme(struct elem *e, const struct theme_elem *te);
/* Text size of an elem after layout (for md value placement). */

/* ------------------------------------------------------------------ help */
struct help_style {
	float pos[2], origin[2];
	gfx_color text, icon;
	char font_path[1024];
	float font_size;
};

struct help_prompt {
	const char *icon;   /* "a" "b" "x" "y" "l" "r" "start" "select" "updown" "leftright" "dpad" */
	const char *label;
};

void help_style_default(struct help_style *hs);
void help_style_apply(struct help_style *hs, const struct theme_elem *te);
void help_draw(struct ui *ui, struct gfx_surface *s, const struct help_style *hs,
	       const struct help_prompt *p, int n);

/* A help bar icon rendered ahead by the asset worker (help_icons_ahead(),
 * thread-safe: ui is the worker's stand-in), put in the UI's icon table on
 * the UI thread (ui_icon_adopt(), which takes img). */
struct ui_icon_ahead {
	char name[16];
	int px;
	gfx_color color;
	struct gfx_image *img;
};
#define HELP_ICONS_AHEAD 8
struct gfx_image *icon_render(const char *name, int px, gfx_color c);
int help_icons_ahead(struct ui *ui, const struct help_style *hs, const struct help_prompt *p, int n,
		     struct ui_icon_ahead *out, int max);
void ui_icon_adopt(struct ui *ui, const struct ui_icon_ahead *ic);

/* ------------------------------------------------------------ menu look */
struct menu_style {
	gfx_color bg;          /* panel background */
	gfx_color dim;         /* overlay over the screen below */
	gfx_color title;
	gfx_color text;
	gfx_color text_dim;    /* values, info */
	gfx_color sel_text;
	gfx_color selector;
	gfx_color separator;
	gfx_color accent;
	char font_path[1024];
	char font_bold[1024];
	float font_size;       /* fraction of the height */
	float small_size;
};

void menu_style_from_theme(struct ui *ui, struct menu_style *ms);

/* ------------------------------------------------------------- systems */
struct sysent {
	const struct sysdef *def;     /* NULL for collections */
	int sidx;                     /* systems_get() index; -1 favorites, -2 last played */
	char name[32];                /* folder name or "favorites"/"lastplayed" */
	char fullname[96];           /* display name, in the language in use (loader_relabel) */
	char theme_name[32];          /* theme folder actually used */
	const char *theme_alias[6];
	char rom_dir[512];
	/* The list (owned by loader.c). NULL while the carousel shows the
	 * snapshot's counts: open it through ui_system_ready(). */
	struct gamelist *games;
	int count, nfav;              /* shown in the carousel */
	bool is_collection;
	struct theme *theme;          /* lazily loaded */
	int64_t theme_us;
};

/* Read requests of the block device holding /data (instrumentation). */
struct ui_io {
	int64_t reads, sectors;
};

/* --------------------------------------------------------------- screens */
struct screen;
struct screen_ops {
	void (*button)(struct ui *ui, struct screen *s, enum input_btn b, enum input_nav_type t);
	/* returns true if it needs a redraw */
	bool (*update)(struct ui *ui, struct screen *s);
	void (*render)(struct ui *ui, struct screen *s, struct gfx_surface *surf);
	/* the logical size changed: drop pixel caches */
	void (*relayout)(struct ui *ui, struct screen *s);
	void (*destroy)(struct ui *ui, struct screen *s);
	/* ms until the next wanted update, -1 none */
	int (*timeout)(struct ui *ui, struct screen *s);
	bool opaque;                  /* covers the whole screen */
	/* optional, tests (ui_debug_screen): "menu:<title>", "dialog:<text>"... */
	void (*describe)(struct ui *ui, struct screen *s, char *buf, size_t n);
};

enum { SCR_OTHER = 0, SCR_SYSVIEW, SCR_GLVIEW };

struct screen {
	const struct screen_ops *ops;
	int kind;
};

#define MAX_SCREENS 12

/* ------------------------------------------------------------ prefetch */
/* prefetch.c - the asset worker (docs/ui-design.md §4.2). A job is filled
 * by a view on the UI thread; run() does the heavy work on the worker with
 * its own copies only; apply() installs the result on the UI thread (only
 * if nothing was invalidated meanwhile); drop() frees the job and whatever
 * apply() did not take, on the UI thread. */
enum { PF_URGENT = 0, PF_NEAR, PF_BG, PF_NPRIO };

struct pf_job {
	void (*run)(struct pf_job *j);
	void (*apply)(struct ui *ui, struct pf_job *j);
	void (*drop)(struct pf_job *j);
	int prio;
	unsigned gen;                 /* prefetch_gen() when submitted */
	bool cancelled;               /* run() may stop early: read with pf_cancelled() */
	struct pf_job *next;
};

static inline bool pf_cancelled(const struct pf_job *j)
{
	return __atomic_load_n(&j->cancelled, __ATOMIC_RELAXED);
}

struct prefetch;
void prefetch_init(struct ui *ui);
void prefetch_destroy(struct ui *ui);
/* The worker can take jobs (after the first frame, when it could start). */
bool prefetch_available(struct ui *ui);
/* Bumped by prefetch_quiesce(): marks of jobs in flight older than this
 * are stale. */
unsigned prefetch_gen(const struct ui *ui);
/* Queues j (its prio set). Without a worker: PF_URGENT runs and applies at
 * once, anything else is dropped; returns false then (j is gone). */
bool prefetch_submit(struct ui *ui, struct pf_job *j);
void prefetch_raise(struct ui *ui, struct pf_job *j, int prio);
/* From ui_update(): installs finished jobs; true if any was applied. */
bool prefetch_poll(struct ui *ui);
/* Before freeing anything a job may read (themes, fonts, the cache
 * directory, sysents): drops the queue, waits for the running job, drops
 * every result; a new generation. */
void prefetch_quiesce(struct ui *ui);
void prefetch_pause(struct ui *ui, bool pause);       /* a game runs */
void prefetch_input(struct ui *ui);                   /* a button: PF_BG waits */
void prefetch_allow_background(struct ui *ui);        /* menu up, lists in */
bool prefetch_background_allowed(const struct ui *ui);
bool prefetch_busy(const struct ui *ui);
bool prefetch_urgent_busy(const struct ui *ui);
int prefetch_timeout(const struct ui *ui);
bool prefetch_wait(struct ui *ui, int prio, int max_ms);
void prefetch_set_polling(struct ui *ui, bool on);
void prefetch_stats(const struct ui *ui, int *jobs, int64_t *work_us, int *queued);

/* ------------------------------------------------------------------ ui */
#define GL_PREBUILT 3                 /* game list views built ahead */

struct ui {
	struct ui_config cfg;
	char paths[16][1024];         /* storage for the config strings */
	int w, h;
	int64_t now;
	bool dirty;
	bool hdmi;
	struct settings *settings;
	struct gamedb *db;
	char settings_path[1024];

	/* themes */
	char theme_name[64];
	char theme_dir[1024];
	struct menu_style ms;
	struct help_style menu_help;

	/* systems shown in the carousel */
	struct sysent *sys;
	int nsys;
	int sys_cursor;
	/* loading (loader.c) */
	struct loader *ld;
	int load_step;
	bool loaded;
	int64_t load_started;
	char load_msg[256];
	dev_t data_dev;               /* for ui_io_sample() */
	struct ui_io io_created;

	/* screens */
	struct screen *stack[MAX_SCREENS];
	int nstack;
	struct gfx_image *snapshot;   /* dimmed render of what is below a modal */
	int snapshot_depth;

	/* overlays */
	int64_t bright_until;
	char toast[256];
	int64_t toast_until;
	int toast_sev;                 /* enum ui_severity */
	int toast_pending_ms;          /* ui_toast(): starts at the next update */
	/* game waiting for the "Resume?" answer */
	struct sysent *pending_se;
	struct game *pending_game;
	/* the game the unit powered off in, offered at boot (ui_offer_resume) */
	struct {
		bool active;               /* the dialog is up */
		char name[128], rom[1024], system[32], core[64], core_path[1024];
		void (*answered)(bool resume, void *user);
		void (*start_fresh)(const struct ui_resume_offer *offer, void *user);
		void *user;
	} boot_resume;
	/* /data unreadable (ui_data_problem, screens.c): the storage screen */
	struct {
		bool active;
		char problem[128];
		struct screen *scr;        /* on the stack (re-pushed if something closed it) */
		int state;                 /* DP_ASK, DP_FORMATTING, DP_FAILED, DP_DONE */
		int sel;                   /* 0 turn off, 1 format */
		int pid;                   /* the format helper, while it runs */
		int status;                /* its exit status (DP_FAILED) */
	} data_problem;
	/* the game switcher's choice (ui_switch_to), launched at the next
	 * ui_update() once the launch callback has returned */
	struct {
		bool active;
		char system[32], rom[1024], core[64];
	} switch_req;
	int64_t power_down;

	/* help/button icons, rendered on demand */
	/* Least recently used goes first when full; a pointer is only good
	 * until the next few ui_icon() calls (help_draw: 12 at most), so an
	 * element keeping an icon across frames must own a copy (F-M4). */
	struct {
		char key[48];
		struct gfx_image *img;
		uint64_t used;
	} icons[96];
	int nicons;
	uint64_t icon_tick;

	/* network status cache */
	bool net_state[3];            /* wifi, eth, bt as last reported */
	bool net_busy[3];

	struct ui_timings timings;
	int64_t created_at;
	bool first_frame_done;
	bool check_changes;
	bool in_snapshot;              /* rendering the view under a modal */
	/* transfer */
	bool in_game;                  /* inside the launch callback */
	char usb_pending[64];          /* a drive plugged during a game: dialog after it */
	char usb_dialog_mp[64];        /* the drive the USB dialog is about ("" = none) */
	bool reload_pending;           /* game folders changed by a transfer */
	unsigned scan_orphan;          /* USB scans whose screen was closed (transfer_ui.c) */
	bool transfer_watch;           /* a copy runs under another screen: poll it (transfer_poll) */
	struct screen *usb_dialog;     /* the USB dialog, while it is open */
	int64_t next_share_poll;
	/* language: the first-boot picker is due (no language in settings.ini)
	 * / up; the fonts directory (font_setup_dir after a switch) */
	bool lang_prompt;
	struct screen *lang_picker;
	char fonts_dir[1024];
	/* power */
	bool charge_mode;
	/* LCD refresh now in use (60 or 78; 0 = not read yet): may be on its 15 s
	 * trial, the setting (lcd_refresh) is only saved on KEEP */
	int lcd_hz;
	bool battery_banner;           /* very low battery */
	/* the Windows file share (rsos-smb): 0 off, 1 starting, 2 on, -1 failed */
	int smb_state;
	char big_msg[256];             /* critical / shutting down */
	struct power_status bat_fallback;
	int64_t bat_read_at;
	int64_t flush_at;             /* sync the caches when idle */
	/* the asset worker (prefetch.c) and the game list views it built
	 * ahead (view_gamelist.c: glview_create() takes them) */
	struct prefetch *pf;
	struct screen *gl_pre[GL_PREBUILT];
	unsigned look_gen;            /* bumped by every theme, language or size change */
};

/* ui.c */
/* false: the stack was full and s has been destroyed (do not use it) */
bool ui_push(struct ui *ui, struct screen *s);
void ui_pop(struct ui *ui);
struct screen *ui_top(struct ui *ui);
void ui_invalidate_snapshot(struct ui *ui);
void ui_toastf(struct ui *ui, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* A toast that asks the user to do something ("you can unplug it"): 6 s. */
void ui_toast_long(struct ui *ui, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_launch_game(struct ui *ui, struct sysent *se, struct game *g);
const char *ui_game_core(struct ui *ui, const struct game *g, const char **name);
void ui_set_theme(struct ui *ui, const char *name);
int ui_list_themes(struct ui *ui, char names[][64], int max);
struct theme *ui_sys_theme(struct ui *ui, struct sysent *se);
void ui_refresh_look(struct ui *ui);
/* Before freeing what the asset worker reads (themes, fonts, sysents, the
 * image cache folder): the worker quiesced, the list views built ahead
 * dropped; look: the theme, language or size changes (ui->look_gen). */
void ui_assets_invalidate(struct ui *ui, bool look);
void ui_io_sample(struct ui *ui, struct ui_io *io);

/* loader.c - game lists: snapshot, background loading, carousel */
void loader_init(struct ui *ui);
bool loader_step(struct ui *ui);          /* from ui_update(); true: redraw */
int loader_timeout(const struct ui *ui);
void loader_pause(struct ui *ui, bool pause);
void loader_check_changes(struct ui *ui); /* back on the carousel */
void loader_save_snapshot(struct ui *ui);
void loader_destroy(struct ui *ui);
/* The carousel entries' display names (system names, "Favorites"...) again,
 * for the language now in use. */
void loader_relabel(struct ui *ui);
/* widgets.c: the theme set is one of ours (rsos-*): its texts are translated */
bool ui_theme_is_ours(const struct ui *ui);
/* C_("theme", text) of an element's text when the theme is ours */
void elem_translate_theme_text(struct ui *ui, struct elem *e);
/* Before opening a list: loads it now if it has not arrived (every list
 * for a collection). Call it with only the carousel on the stack. Returns
 * the entry to open (the carousel may have been rebuilt), NULL if it has
 * no games any more. */
struct sysent *ui_system_ready(struct ui *ui, struct sysent *se);
void ui_reload_games(struct ui *ui, bool force);
/* loader.c: a list in the order of Settings > Game lists (sort, favorites) */
void ui_sort_games(struct ui *ui, struct gamelist *gl);
/* loader.c: every list loaded now (Search all games) */
void loader_complete_all(struct ui *ui);
void ui_power(struct ui *ui, enum ui_power_action a);
void ui_setting_changed(struct ui *ui, const char *key, const char *value);
int ui_font_px(const struct ui *ui, float frac);
/* Battery status: the power module, or sysfs when there is none. */
const struct power_status *ui_battery(struct ui *ui);
/* Draws the battery indicator right-aligned at (right, y), height h;
 * returns its width. */
int draw_battery(struct ui *ui, struct gfx_surface *s, int right, int y, int h, gfx_color c,
		 struct font *f);
/* Button icon (help bar) of height px in color c. */
struct gfx_image *ui_icon(struct ui *ui, const char *name, int px, gfx_color c);

/* view_system.c */
struct screen *sysview_create(struct ui *ui);
void sysview_set_cursor(struct ui *ui, struct screen *s, int idx);
void sysview_jump(struct ui *ui, struct screen *s, int idx);
void sysview_refresh_info(struct ui *ui, struct screen *s);   /* counts changed */
/* Queues what the carousel wants built by the asset worker (ui_update). */
void sysview_prefetch(struct ui *ui, struct screen *s);
void sysview_describe_assets(struct ui *ui, struct screen *s, char *buf, size_t n);

/* view_gamelist.c */
struct screen *glview_create(struct ui *ui, struct sysent *se);
struct sysent *glview_system(struct screen *s);
void glview_refresh(struct ui *ui, struct screen *s);
/* batch 2: search (live filter of the rows; "" = all), the text searched,
 * the test description, a game gone from the lists, "Search all games" */
void glview_set_search(struct ui *ui, struct screen *s, const char *text);
const char *glview_search(struct screen *s);
void glview_describe(struct screen *s, char *buf, size_t n);
void glview_game_removed(struct ui *ui, const char *system, const char *path);
struct screen *glview_create_search_all(struct ui *ui, const char *text);
/* The list view of se built ahead by the asset worker (glview_create()
 * takes it); dropped when the look changes or the carousel is rebuilt. */
void glview_prefetch(struct ui *ui, struct sysent *se, int prio);
void glview_prebuilt_drop(struct ui *ui);
void glview_describe_prebuilt(struct ui *ui, char *buf, size_t n);
/* "3 h 12 min", "25 min", "-" */
void ui_format_playtime(int64_t seconds, char *out, size_t n);

/* menu.c - generic list menu, dialogs, on-screen keyboard */
enum mi_type { MI_ACTION = 0, MI_SUBMENU, MI_TOGGLE, MI_CHOICE, MI_SLIDER, MI_INFO, MI_HEADER,
	       MI_NUMBER /* val in [min, max], shown as a choice */ };

struct menu_item {
	enum mi_type type;
	int id;
	char label[96];              /* translated: room for long languages and CJK */
	char value[96];               /* display value (info/choice) */
	bool on;                      /* toggle */
	int val, min, max;            /* slider / choice index */
	const char *choices[16];
	int nchoices;
	bool disabled;
};

struct menu;
typedef void (*menu_fn)(struct ui *ui, struct menu *m, struct menu_item *it, int dir);

struct menu {
	struct screen base;
	char title[96];
	struct menu_item items[96];
	int n;
	int cursor, top;
	int kind;                     /* which menu (screens.c) */
	void *ctx;                    /* e.g. the game for game options */
	struct sysent *sys;
	menu_fn on_item;              /* A (dir 0) or left/right (dir -1/+1) */
	void (*on_refresh)(struct ui *ui, struct menu *m);
	void (*on_destroy)(struct ui *ui, struct menu *m);   /* frees ctx */
	int64_t refresh_every;        /* ms, 0 = never */
	int64_t next_refresh;
};

struct menu *menu_new(struct ui *ui, const char *title, int kind);
struct menu_item *menu_add(struct menu *m, enum mi_type t, int id, const char *label);
void menu_open(struct ui *ui, struct menu *m);
struct menu_item *menu_find(struct menu *m, int id);

typedef void (*dialog_fn)(struct ui *ui, int choice, void *user);
/* buttons: NULL-terminated labels, e.g. {"YES", "NO", NULL}. Returns NULL
 * when the screen stack is full: nothing is shown and cb is never called. */
struct screen *dialog_open(struct ui *ui, const char *text, const char *const *buttons,
			    dialog_fn cb, void *user);
void message_open(struct ui *ui, const char *text);
/* A dialog that answers by itself: "%d" in its text shows the seconds left;
 * after `seconds` the callback gets timeout_choice (the dialog is closed). */
void dialog_set_countdown(struct screen *dialog, int seconds, int timeout_choice);
/* The button selected when the dialog opens. Both accept NULL and ignore a
 * screen that is not a dialog. */
void dialog_select(struct screen *dialog, int button);
/* The screen is a dialog (dialog_open) / the on-screen keyboard. */
bool screen_is_dialog(const struct screen *s);
bool screen_is_osk(const struct screen *s);
bool screen_is_menu(const struct screen *s);

typedef void (*osk_fn)(struct ui *ui, const char *text, bool ok, void *user);
/* Returns the keyboard (NULL when the screen stack is full). */
struct screen *osk_open(struct ui *ui, const char *title, const char *initial, bool password,
			osk_fn cb, void *user);
/* Live keyboard (search): on_change after each change of the text; the
 * title can be updated from it (the number of games found). */
void osk_set_live(struct screen *osk, void (*on_change)(struct ui *ui, struct screen *osk, const char *text,
							 void *user));
void osk_set_title(struct screen *osk, const char *title);

/* screens.c */
void settings_open_main(struct ui *ui);
/* The language list: Settings > Language, or the first-boot picker (full
 * screen, the language previewed as the cursor moves, must choose). */
void language_open(struct ui *ui, bool first_boot);
void game_options_open(struct ui *ui, struct sysent *se, struct game *g);
void buttontest_open(struct ui *ui);
void ui_apply_boot_settings(struct ui *ui);

/* transfer_ui.c */
void import_open(struct ui *ui, const char *mp, const char *label);
/* Settings > Storage: pick a drive (if several), then the flow */
enum { USB_ACT_IMPORT = 0, USB_ACT_EXPORT, USB_ACT_SAVES };
void transfer_usb_action(struct ui *ui, int action);
void transfer_eject_all(struct ui *ui);
/* After a game: the USB dialog of a drive plugged meanwhile. */
void transfer_after_game(struct ui *ui);
void transfer_before_game(struct ui *ui);
void transfer_network_off(struct ui *ui);
void transfer_poll(struct ui *ui);
/* screens.c: the storage problem screen kept up, its format helper polled
 * (every ui_update) */
void data_problem_poll(struct ui *ui);
void webshare_open(struct ui *ui);
/* The Windows file share (ui->cfg.smb_helper), next to the network
 * transfer: available (the helper is installed), start (with the transfer's
 * PIN), stop; the helper's end comes back as a job (UI_JOB_SMB*). */
enum { UI_JOB_SMB = 40, UI_JOB_SMB_STOP = 41 };
bool smb_available(struct ui *ui);
void smb_start(struct ui *ui);
void smb_stop(struct ui *ui);
void smb_job_done(struct ui *ui, const struct hw_job_result *r);

/* update_ui.c - Settings > System update (docs/updates.md): the menu runs
 * the rsos-update helper (cfg.update_helper) and reads its lines. */
bool update_available(const struct ui *ui);      /* a helper is installed */
void update_open(struct ui *ui);
/* the Settings item's value: "0.2.0 available" after a check, else "" */
const char *update_menu_value(struct ui *ui);
/* from ui_update(): helper output, the daily check, the after-restart note */
void update_poll(struct ui *ui);
int update_timeout(const struct ui *ui);
/* The USB dialog: a *.rsu at the drive's root or in RetroStoneOS/ */
bool update_usb_has_package(const char *mountpoint);
void update_open_usb(struct ui *ui, const char *mountpoint);

/* widgets.c - drawing helpers */
void draw_text_box(struct ui *ui, struct gfx_surface *s, struct font *f, const char *text,
		   int x, int y, int w, int h, int align, gfx_color c);
void draw_panel(struct gfx_surface *s, int x, int y, int w, int h, int radius, gfx_color c);
struct gfx_image *render_text_image(struct font *f, const char *text, int max_w,
				    int max_lines, int align, float line_spacing,
				    gfx_color c, int *out_w, int *out_h);
void format_datetime(const struct elem *e, int64_t now, char *out, size_t n);
void format_bytes(uint64_t b, char *out, size_t n);

#endif
