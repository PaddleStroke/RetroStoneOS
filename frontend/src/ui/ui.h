/*
 * ui.h - RetroStoneOS user interface: system carousel, game lists, the
 * unified settings menu, dialogs, on-screen keyboard, button test and
 * controller screens, all drawn with the ES theme engine into a plain
 * XRGB8888 buffer. No DRM, no input devices: the caller feeds buttons and
 * provides the buffer (see docs/ui-design.md, "Integration").
 *
 * Main loop sketch (single-threaded):
 *
 *   ui = ui_create(&cfg);
 *   ui_pick_logical_size(out_w, out_h, &w, &h); ui_set_size(ui, w, h);
 *   for (;;) {
 *       poll(display fd, input fd, timeout = min(ui_timeout_ms(ui, now),
 *                                                input_timeout_ms(in)));
 *       display_handle_events();            // may call ui_set_size() on hotplug
 *       input_poll(in);
 *       while (input_next_nav(in, &ev)) ui_button(ui, ev.btn, ev.type);
 *       while (input_next_hotkey(in, &hk)) ui_hotkey(ui, hk);
 *       if (ui_update(ui, now_ms)) {        // true: something changed
 *           i = display_begin_frame(-1);
 *           ui_render(ui, &surface_of(buffer i));
 *           display_present();
 *       }
 *   }
 */
#ifndef RSOS_UI_H
#define RSOS_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../gfx/gfx.h"
#include "../input/input.h"
#include "../power/power.h"
#include "../transfer/transfer.h"

struct ui;

/* What the libretro host needs to start a game. Strings are valid during
 * the callback only. */
struct ui_launch {
	const char *rom_path;    /* absolute */
	const char *system;      /* ROM folder name, e.g. "snes" */
	const char *core;        /* core id, e.g. "snes9x2005" (game > system > default) */
	const char *core_path;   /* .so from the core .ini ("" if unknown) */
	const char *game_name;   /* display name */
	const char *bios_dir;    /* "/data/bios" */
	/* INTEGRATION FIX: the callback may write a message here ("PS1 needs
	 * scph5501.bin in /bios", "The emulator crashed..."); the UI shows it
	 * after the callback returns, instead of its generic error. */
	char *message;
	size_t message_size;
	/* true: continue from the auto save state (the player chose "Resume"
	 * in the prompt shown when cb.has_resume() said one exists). Forward
	 * it to host_launch_opts.resume. */
	bool resume;
	/* Where the core choice came from: "game" (this game's choice),
	 * "system" (Settings core.<system>), "default"; "resume" for the boot
	 * resume offer (the core the game was saved with). */
	const char *core_source;
	/* true: launched from the boot "Resume <game>?" dialog. */
	bool boot_resume;
	/* Batch 2, per game (game options / in-game menu, gamedb.tsv): the
	 * scaling ("aspect", "integer", "stretch"; NULL = Settings > Display)
	 * and the CPU profile ("performance", "powersave"; NULL = automatic,
	 * the core's .ini), which the UI has applied (power_set_game_cpu). */
	const char *scale;
	const char *cpu;
	/* true: launched by the game switcher (the game before asked for it) */
	bool switched;
};

/* A recently played game (ui_recent_games), for the game switcher. */
struct ui_recent_game {
	char name[128];
	char system[32];
	char rom[1024];
	char core[64];
	char core_path[512];
	int64_t lastplayed;
};

/*
 * The boot resume offer (ui_offer_resume): the unit powered off while this
 * game ran and its auto state was written. Strings are copied.
 */
struct ui_resume_offer {
	const char *game_name;   /* display name */
	const char *rom_path;
	const char *system;
	const char *core;        /* core id the state was saved with */
	const char *core_path;   /* its .so */
	/* Called when the player answers, before the game starts (resume =
	 * true) or with the menu left as it is (false: "Start fresh" or B). The
	 * caller forgets the offer there (resume.ini deleted). */
	void (*answered)(bool resume, void *user);
	void *user;
};

enum ui_power_action { UI_POWER_OFF = 0, UI_REBOOT };

struct ui_callbacks {
	/*
	 * Runs the game. May block until the game exits (the UI does nothing
	 * meanwhile); return 0 on success, < 0 if the game could not start
	 * (the UI then shows an error). NULL: log only (stub).
	 */
	int (*launch)(const struct ui_launch *req, void *user);
	/* "lcd_refresh" ("60" / "78"): apply to the LCD now (display_set_lcd_refresh);
	 * the UI saves it only when the player keeps it (15 s trial), and sends
	 * "78" again on revert. */
	/* A setting that another layer applies changed: "hdmi_mode"
	 * (auto|720p|1080p), "scaling" (aspect|integer), "p1" (auto|builtin|
	 * external), "cz_buttons" (0|1), "theme". Already saved. */
	void (*setting_changed)(const char *key, const char *value, void *user);
	/* Optional: true if the game has an auto save state to resume from
	 * (host_has_resume_state(req->core_path, req->rom_path, req->system)).
	 * The UI then asks "Resume where you left off?" (setting resume_mode:
	 * ask, the default; "always" / "never" decide without asking) and sets
	 * req->resume. Called just before launch(), with the same req. */
	bool (*has_resume)(const struct ui_launch *req, void *user);
	/* Reboot / power off, used when there is no power API (cfg.power).
	 * NULL: the UI runs /sbin/reboot or /sbin/poweroff. */
	void (*power)(enum ui_power_action action, void *user);
	/* Optional (Delete this game): the game's saves and save states (its
	 * .srm/.rtc and .state* files, the names the game process uses);
	 * remove = false counts them, true deletes them. Returns the number of
	 * files. NULL: the UI looks for <ROM stem>.* in <data>/saves|states. */
	int (*game_saves)(const struct ui_launch *req, bool remove, void *user);
	void *user;
};

/*
 * The power module (src/power, docs/power.md) as seen by the UI. The lead
 * fills it with ui_power_api_from_module(); the preview tool leaves it
 * NULL and the UI then reads the battery from sysfs and does nothing else.
 */
struct ui_power_api {
	const struct power_status *(*get_status)(void);
	bool (*on_input)(void);               /* true: drop the event (it woke the screen) */
	int (*set_setting)(const char *key, const char *value);
	int (*set_time)(time_t t);
	int (*set_timezone)(const char *name);
	const struct power_tz *(*timezones)(int *count);
	void (*request_shutdown)(enum power_reason why);
	void (*set_game_running)(bool running);
	void (*set_game_cpu)(const char *governor, int max_khz);
};

/* Only compiled where it is called (the frontend main): links src/power. */
static inline void ui_power_api_from_module(struct ui_power_api *a)
{
	a->get_status = power_get_status;
	a->on_input = power_on_input;
	a->set_setting = power_set_setting;
	a->set_time = power_set_time;
	a->set_timezone = power_set_timezone;
	a->timezones = power_timezones;
	a->request_shutdown = power_request_shutdown;
	a->set_game_running = power_set_game_running;
	a->set_game_cpu = power_set_game_cpu;
}

/*
 * The transfer module (src/transfer, docs/rom-transfer.md): USB import and
 * the web share. Filled by ui_transfer_api_from_module(); NULL hides the
 * transfer screens (the preview tool passes a fake one).
 */
struct ui_transfer_api {
	int (*usb_drives)(struct transfer_usb_drive *out, int max);
	int (*usb_eject)(const char *mountpoint);
	int (*usb_remount)(const char *mountpoint, bool writable);
	int (*trees_start)(const char *stick_root);
	int (*trees_poll)(struct transfer_tree *out, int max, int *n);
	int (*plan_start_trees)(const char *const *roots, int n, const char *dst_root);
	int (*plan_poll)(struct transfer_plan **out);
	void (*plan_free)(struct transfer_plan *p);
	void (*import_defaults)(struct transfer_import_opts *o);
	int (*import_start)(struct transfer_plan *p, const struct transfer_import_opts *o);
	enum transfer_state (*import_status)(struct transfer_progress *out);
	void (*import_answer)(enum transfer_answer a);
	void (*import_cancel)(void);
	int (*import_finish)(char out[][TRANSFER_SYSID_MAX], int max);
	/* "Export games" / "Back up saves" (console -> stick) */
	int (*backup_scan_start)(const char *data_root, const char *stick_root, const char *fstype,
				 enum transfer_backup_mode mode);
	int (*backup_scan_poll)(struct transfer_backup **out);
	const struct transfer_backup_info *(*backup_info)(const struct transfer_backup *b);
	void (*backup_totals)(const struct transfer_backup *b, const struct transfer_backup_opts *o,
			      struct transfer_backup_totals *t);
	void (*backup_free)(struct transfer_backup *b);
	int (*backup_start)(struct transfer_backup *b, const struct transfer_backup_opts *o);
	enum transfer_state (*backup_status)(struct transfer_progress *out);
	void (*backup_cancel)(void);
	void (*backup_finish)(void);
	int (*webshare_start)(const struct webshare_config *cfg);
	void (*webshare_stop)(void);
	bool (*webshare_running)(void);
	void (*webshare_get_status)(struct webshare_status *st);
	int (*webshare_take_changes)(char out[][TRANSFER_SYSID_MAX], int max);
	void (*netnames_defaults)(struct netnames_config *c);
	int (*netnames_start)(const struct netnames_config *c);
	void (*netnames_stop)(void);
	int (*qr_encode)(const char *text, struct qr_code *qr);
	const char *data_root;          /* "/data" */
};

static inline void ui_transfer_api_from_module(struct ui_transfer_api *a)
{
	a->usb_drives = transfer_usb_drives;
	a->usb_eject = transfer_usb_eject;
	a->usb_remount = transfer_usb_remount;
	a->trees_start = transfer_trees_start;
	a->trees_poll = transfer_trees_poll;
	a->plan_start_trees = transfer_plan_start_trees;
	a->plan_poll = transfer_plan_poll;
	a->plan_free = transfer_plan_free;
	a->import_defaults = transfer_import_defaults;
	a->import_start = transfer_import_start;
	a->import_status = transfer_import_status;
	a->import_answer = transfer_import_answer;
	a->import_cancel = transfer_import_cancel;
	a->import_finish = transfer_import_finish;
	a->backup_scan_start = transfer_backup_scan_start;
	a->backup_scan_poll = transfer_backup_scan_poll;
	a->backup_info = transfer_backup_get_info;
	a->backup_totals = transfer_backup_totals;
	a->backup_free = transfer_backup_free;
	a->backup_start = transfer_backup_start;
	a->backup_status = transfer_backup_status;
	a->backup_cancel = transfer_backup_cancel;
	a->backup_finish = transfer_backup_finish;
	a->webshare_start = webshare_start;
	a->webshare_stop = webshare_stop;
	a->webshare_running = webshare_running;
	a->webshare_get_status = webshare_get_status;
	a->webshare_take_changes = webshare_take_changes;
	a->netnames_defaults = netnames_defaults;
	a->netnames_start = netnames_start;
	a->netnames_stop = netnames_stop;
	a->qr_encode = qr_encode;
	a->data_root = "/data";
}

struct ui_config {
	const char *roms_dir;        /* "/data/roms" */
	const char *data_dir;        /* "/data/rsos" (settings.ini, gamedb, input/) */
	const char *cache_dir;       /* "/data/rsos/cache" */
	const char *themes_builtin;  /* "/usr/share/rsos/themes" */
	const char *themes_user;     /* "/data/themes" */
	const char *res_dir;         /* "/usr/share/rsos" (fonts/, ":/" in themes) */
	const char *cores_dir;       /* "/usr/share/rsos/cores" */
	const char *bios_dir;        /* "/data/bios" */
	const char *boot_env;        /* "/boot/rsos.env" */
	const char *power_supply_dir;/* "/sys/class/power_supply" */
	const char *backlight_dir;   /* "/sys/class/backlight" (without input) */
	const char *net_helper;      /* "/usr/bin/rsos-net" */
	/* the system updater (docs/updates.md), run as a helper process;
	 * NULL or "" (tests, tools): no Settings > System update. The string
	 * must outlive the UI. */
	const char *update_helper;   /* "/usr/bin/rsos-update" */
	/* the Windows file share (ksmbd, docs/rom-transfer.md §3.3), started
	 * with the network transfer when Settings > Network > Windows file
	 * share is on; missing / NULL / "": the item is hidden. The string must
	 * outlive the UI. */
	const char *smb_helper;      /* "/usr/bin/rsos-smb" */
	const char *wpa_conf;        /* "/data/rsos/wpa_supplicant.conf" */
	const char *version;         /* fallback version string */
	const char *default_theme;   /* "rsos-dark" (RSOS_DEFAULT_THEME) */
	/* The board (src/board.h; the defaults are the RetroStone2's):
	 * a built-in screen (Brightness in Settings > Display), its
	 * 78/60 Hz choice (LCD refresh rate), and the opt-in storage
	 * overlays offered in Settings > Storage ("emmc sata"; "" = none). */
	bool has_internal_display;   /* true */
	bool lcd_refresh_choice;     /* true */
	const char *storage_overlays;/* "emmc sata" */
	/* Translations (src/i18n, docs/translating.md): the compiled catalogs,
	 * and the language (NULL: the `language` key of settings.ini; a value
	 * here wins and is not saved). language_prompt: with no language in
	 * settings.ini, the first-boot language picker comes before the menu
	 * (the device sets it; false by default, for tests and tools). */
	const char *locale_dir;      /* "/usr/share/rsos/locale" */
	const char *language;        /* NULL */
	bool language_prompt;        /* false */
	struct input *input;         /* optional: brightness, stick, controller screens */
	const struct ui_power_api *power; /* optional: battery, sleep, clock (NULL: sysfs only) */
	const struct ui_transfer_api *transfer; /* optional: USB import, web share */
	struct ui_callbacks cb;
};

/* Fills cfg with the target defaults above. */
void ui_config_defaults(struct ui_config *cfg);

/* Cheap: reads settings and the core list. The first ui_update() builds the
 * carousel from the snapshot of the last boot and a worker thread loads the
 * game lists after the first frame; without a snapshot (first boot) the
 * "Preparing your console" screen shows until every list is in. */
struct ui *ui_create(const struct ui_config *cfg);
void ui_destroy(struct ui *ui);

/* Logical UI size for an output: at most 480 lines, the output's aspect
 * ratio, even width (640x480 LCD -> 640x480, 1280x720 -> 854x480). */
void ui_pick_logical_size(int out_w, int out_h, int *w, int *h);
/* (Re)lays everything out for a new logical size (output switch). */
void ui_set_size(struct ui *ui, int w, int h);
/* HDMI (docked) or LCD: brightness is LCD-only. */
void ui_set_output(struct ui *ui, bool hdmi);

/* Navigation input (A = confirm, B = back). */
void ui_button(struct ui *ui, enum input_btn btn, enum input_nav_type type);
/* Global events: brightness overlay, power key, pad toasts. */
void ui_hotkey(struct ui *ui, enum input_hotkey hk);

/* Advances loading, animations and background jobs to now_ms.
 * Returns true when the screen must be redrawn. */
bool ui_update(struct ui *ui, int64_t now_ms);
/*
 * The time of the events about to be delivered (ui_button, ui_hotkey,
 * ui_usb_event, ui_power_event). Call it after poll() returns: the UI's
 * clock is otherwise the one of the last ui_update(), before the sleep, and
 * a deadline set by an event (toast, overlay) would start in the past.
 */
void ui_set_now(struct ui *ui, int64_t now_ms);
/* ms until the UI needs ui_update() again (-1 = only on input). */
int ui_timeout_ms(const struct ui *ui, int64_t now_ms);
/* Draws a full frame (s->w x s->h must be the size given to ui_set_size). */
void ui_render(struct ui *ui, struct gfx_surface *s);

/*
 * Power module events, from the power callbacks (docs/power.md §10):
 *   on_status           -> ui_power_event(ui, UI_PWR_STATUS)
 *   on_warning(level)   -> UI_PWR_LOW / UI_PWR_VERY_LOW / UI_PWR_OK
 *   on_critical         -> UI_PWR_CRITICAL ("Battery empty, saving...")
 *   on_shutdown_request -> UI_PWR_SHUTDOWN ("Powering off..."), or
 *                          UI_PWR_REBOOT ("Restarting...") for a reboot
 *   on_thermal(hot)     -> UI_PWR_HOT / UI_PWR_COOL
 *   on_screen(ON)       -> UI_PWR_STATUS (redraw)
 *   on_idle_poweroff    -> UI_PWR_IDLE_WARN ("Powering off in 10 s ..." for
 *                          10 s) / UI_PWR_IDLE_CANCEL (taken down; also after
 *                          an idle power-off that was cancelled in a game)
 */
enum ui_power_event {
	UI_PWR_STATUS = 0, UI_PWR_LOW, UI_PWR_VERY_LOW, UI_PWR_OK, UI_PWR_CRITICAL,
	UI_PWR_SHUTDOWN, UI_PWR_HOT, UI_PWR_COOL, UI_PWR_REBOOT,
	UI_PWR_IDLE_WARN, UI_PWR_IDLE_CANCEL,
};
void ui_power_event(struct ui *ui, enum ui_power_event ev);
/* Toasts for other modules (host warnings, ...): a short message at the
 * bottom of the screen, over whatever is shown. Severity sets the color and
 * how long it stays (info 3.5 s, warning 5 s, error 6 s; a toast that asks
 * the user to do something 6 s). The time starts at the next ui_update(),
 * so it is safe to call at any time, also before the menu is loaded (it
 * shows once the menu is up) or after a long sleep. */
enum ui_severity { UI_SEV_INFO = 0, UI_SEV_WARNING, UI_SEV_ERROR };
void ui_toast(struct ui *ui, const char *text, enum ui_severity severity);
/* A message the user dismisses (an OK dialog), e.g. the boot notes. */
void ui_message(struct ui *ui, const char *text);

/* USB drive events from transfer_usb_poll(): on a new drive, the "what do
 * you want to do" dialog (Import games / Export games / Back up saves /
 * Nothing), or a toast (setting usb_import_prompt=0, or a copy running);
 * deferred while a game runs or before the menu is up. */
void ui_usb_event(struct ui *ui, const struct transfer_usb_event *ev);

/* Tests: what the top screen shows, e.g. "dialog:USB drive GAMES...",
 * "menu:Import from GAMES", "screen:progress", "carousel", plus
 * " toast=<text>" while a toast is visible. */
void ui_debug_screen(struct ui *ui, char *buf, size_t n);

/*
 * The boot resume offer, over whatever is shown: call it once the menu is up
 * (ui_is_loaded), never in charge mode. Settings > Games > Resume on boot
 * (resume_boot) decides: "always" launches the game at once, resumed (no
 * dialog); "never" answers false at once (the menu; the auto state stays);
 * "ask" (default) shows "Resume <game>?" [RESUME] [START FRESH] [ALWAYS
 * RESUME] [NEVER ASK] (B = start fresh): the last two also save
 * resume_boot. Resume launches the game through the normal launch flow
 * (launch callback, player 1 = the pad that pressed A) with req->resume and
 * the offer's core; Start fresh leaves the menu (the auto state stays, so
 * the game's own "Resume where you left off?" still works).
 */
void ui_offer_resume(struct ui *ui, const struct ui_resume_offer *offer);

/*
 * Batch 2, for the launch callback (main.c):
 *  - ui_recent_games(): the most recently played games (last played first),
 *    for the game switcher's file; returns how many.
 *  - ui_switch_to(): the game switcher chose this game; the UI launches it
 *    (resumed when it has an auto state) at the next ui_update(), after the
 *    launch callback has returned.
 *  - ui_game_add_playtime(): seconds played, added to the game's play time
 *    (gamedb.tsv, saved now; "playtime" lines of the game process).
 *  - ui_game_set_option(): "scale" / "cpu" changed in the in-game menu:
 *    saved for this game.
 */
int ui_recent_games(struct ui *ui, struct ui_recent_game *out, int max);
void ui_switch_to(struct ui *ui, const char *system, const char *rom_path, const char *core);
void ui_game_add_playtime(struct ui *ui, const char *system, const char *rom_path, int64_t seconds);
void ui_game_set_option(struct ui *ui, const char *system, const char *rom_path, const char *key,
			const char *value);

/* Charge mode (booted by the charger): only the charging screen, no game
 * list loading. Leave it from on_charge_exit(). */
void ui_set_charge_mode(struct ui *ui, bool on);

/* True once the carousel is up: at once from the carousel snapshot
 * (<cache>/systems.idx) on a normal boot, after every list on the first. */
bool ui_is_loaded(const struct ui *ui);
/* An OS update is being downloaded or installed (no idle power-off). */
bool ui_update_busy(const struct ui *ui);
/* True once every game list has been loaded/validated (the background
 * loader is done and the carousel matches a full load). */
bool ui_lists_complete(const struct ui *ui);
/* The background loader stops touching the SD card while a game runs (the
 * UI calls it around its launch callback itself). */
void ui_pause_background(struct ui *ui, bool pause);
/* Tests: "carousel=nes:2,gb:5,...;cursor=gb;top=list:gb:5;complete=1;
 * digest=<hex>" (top=carousel|list:<system>:<games>|other; the digest covers
 * every entry and game, and is only meaningful once complete). */
void ui_debug_state(struct ui *ui, char *buf, size_t n);
/* Switches the theme (like Settings > Theme) and saves it. */
void ui_select_theme(struct ui *ui, const char *name);
/*
 * Switches the language live (like Settings > Language): the catalog, the
 * CJK font order, system names, themes and views are reloaded; open menus
 * are closed. save: also write language=<code> to settings.ini. Returns 0,
 * -1 if the language is unknown or has no catalog (English then).
 */
int ui_set_language(struct ui *ui, const char *code, bool save);
/* True while the first-boot language picker is up (or about to be): the
 * boot notes and the resume offer wait for it. */
bool ui_first_boot_busy(const struct ui *ui);

/* Timings of the last load, for logs and docs. */
struct ui_timings {
	int64_t create_us;
	int64_t menu_us;          /* first ui_update() -> carousel built (snapshot or full load) */
	int64_t systems_us;       /* first ui_update() -> every list loaded/validated */
	int64_t theme_us;         /* theme XML parsing */
	int64_t first_frame_us;   /* first system view frame (images included) */
	int ngames, nsystems;
};
void ui_get_timings(const struct ui *ui, struct ui_timings *t);

#endif
