/*
 * ui.c - the UI core: configuration, the first-boot "Preparing your
 * console" screen, the screen stack, rendering with modal snapshots,
 * overlays (toasts, brightness), themes and game launching. Game lists and
 * the carousel's systems are in loader.c. See ui.h.
 */
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "fswarm.h"
#include "ui_internal.h"

#ifndef RSOS_DEFAULT_THEME
#define RSOS_DEFAULT_THEME "rsos-dark"
#endif
#ifndef RSOS_VERSION
#define RSOS_VERSION "RetroStoneOS (development)"
#endif

#define TOAST_MS 3500       /* info; warning 5 s, error 6 s */
#define TOAST_LONG_MS 6000  /* a toast that asks the user to do something */
#define BRIGHT_MS 1500
#define FLUSH_IDLE_MS 1500

static void apply_power_settings(struct ui *ui);
static void render_charge(struct ui *ui, struct gfx_surface *s);
static void render_power_overlays(struct ui *ui, struct gfx_surface *s);

void screens_job_done(struct ui *ui, const struct hw_job_result *r);

/* ---------------------------------------------------------------- config */
void ui_config_defaults(struct ui_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->roms_dir = "/data/roms";
	cfg->data_dir = "/data/rsos";
	cfg->cache_dir = "/data/rsos/cache";
	cfg->themes_builtin = "/usr/share/rsos/themes";
	cfg->themes_user = "/data/themes";
	cfg->res_dir = "/usr/share/rsos";
	cfg->cores_dir = "/usr/share/rsos/cores";
	cfg->bios_dir = "/data/bios";
	cfg->boot_env = "/boot/rsos.env";
	cfg->power_supply_dir = "/sys/class/power_supply";
	cfg->backlight_dir = "/sys/class/backlight";
	cfg->net_helper = "/usr/bin/rsos-net";
	cfg->update_helper = "/usr/bin/rsos-update";   /* used only if it is there */
	cfg->smb_helper = "/usr/bin/rsos-smb";         /* the same */
	cfg->data_partition_helper = "/usr/libexec/rsos/data-partition";
	cfg->wpa_conf = "/data/rsos/wpa_supplicant.conf";
	cfg->version = RSOS_VERSION;
	cfg->default_theme = RSOS_DEFAULT_THEME;
	cfg->has_internal_display = true;
	cfg->lcd_refresh_choice = true;
	cfg->storage_overlays = "emmc sata";
	cfg->locale_dir = "/usr/share/rsos/locale";
}

void ui_pick_logical_size(int out_w, int out_h, int *w, int *h)
{
	int hh = MIN(480, out_h > 0 ? out_h : 480);
	double aspect = out_w > 0 && out_h > 0 ? (double)out_w / (double)out_h : 4.0 / 3.0;
	int ww = (int)lround((double)hh * aspect);

	ww += ww & 1;
	*w = MAX(ww, 2);
	*h = hh;
}

static const char *keep(struct ui *ui, int slot, const char *s, const char *def)
{
	strlcpy_(ui->paths[slot], s && *s ? s : def ? def : "", sizeof(ui->paths[0]));
	return ui->paths[slot];
}

/* ------------------------------------------------------------ the stack */
struct screen *ui_top(struct ui *ui)
{
	return ui->nstack ? ui->stack[ui->nstack - 1] : NULL;
}

void ui_invalidate_snapshot(struct ui *ui)
{
	gfx_image_free(ui->snapshot);
	ui->snapshot = NULL;
	ui->snapshot_depth = -1;
	ui->dirty = true;
}

/* Returns false when the stack is full: s is destroyed (freed) then, and
 * the caller must not use it any more. */
bool ui_push(struct ui *ui, struct screen *s)
{
	if (!s)
		return false;
	if (ui->nstack == MAX_SCREENS) {
		LOGW("ui: screen stack full");
		s->ops->destroy(ui, s);
		return false;
	}
	ui->stack[ui->nstack++] = s;
	if (s->ops->opaque)
		ui_invalidate_snapshot(ui);
	ui->dirty = true;
	return true;
}

void ui_pop(struct ui *ui)
{
	struct screen *s;
	bool was_opaque;

	if (ui->nstack <= 1)
		return; /* the system view stays */
	s = ui->stack[--ui->nstack];
	was_opaque = s->ops->opaque;
	s->ops->destroy(ui, s);
	if (was_opaque)
		ui_invalidate_snapshot(ui);
	ui->dirty = true;
	if (ui_top(ui) && ui_top(ui)->kind == SCR_SYSVIEW)
		ui->check_changes = true; /* done in ui_update() */
}

/* The time starts at the next ui_update(): the events that raise toasts
 * arrive after poll(), when ui->now is the time before the sleep (a deadline
 * computed from it could already be over: the missing "USB drive" toast). */
static void toast_v(struct ui *ui, int ms, const char *fmt, va_list ap) __attribute__((format(printf, 3, 0)));
static void toast_v(struct ui *ui, int ms, const char *fmt, va_list ap)
{
	vsnprintf(ui->toast, sizeof(ui->toast), fmt, ap);
	ui->toast_sev = UI_SEV_INFO;
	ui->toast_pending_ms = ms;
	ui->toast_until = 0;
	ui->dirty = true;
	LOGI("ui: toast: %s", ui->toast);
}

void ui_toastf(struct ui *ui, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	toast_v(ui, TOAST_MS, fmt, ap);
	va_end(ap);
}

void ui_toast_long(struct ui *ui, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	toast_v(ui, TOAST_LONG_MS, fmt, ap);
	va_end(ap);
}

void ui_message(struct ui *ui, const char *text)
{
	if (!ui || !text)
		return;
	LOGI("ui: message: %s", text);
	if (!dialog_open(ui, text, NULL, NULL, NULL))
		ui_toast(ui, text, UI_SEV_WARNING);   /* stack full */
}

void ui_toast(struct ui *ui, const char *text, enum ui_severity severity)
{
	static const int ms[] = { TOAST_MS, 5000, 6000 };

	if (!ui || !text)
		return;
	if ((unsigned)severity > UI_SEV_ERROR)
		severity = UI_SEV_INFO;
	strlcpy_(ui->toast, text, sizeof(ui->toast));
	ui->toast_sev = severity;
	/* the clock is the one of the next ui_update() (callers may run
	 * outside the UI loop, e.g. from a game launch) */
	ui->toast_pending_ms = ms[severity];
	ui->toast_until = 0;
	ui->dirty = true;
	LOGI("ui: toast (%s): %s", severity == UI_SEV_ERROR ? "error" :
	     severity == UI_SEV_WARNING ? "warning" : "info", text);
}

void ui_setting_changed(struct ui *ui, const char *key, const char *value)
{
	if (ui->cfg.cb.setting_changed)
		ui->cfg.cb.setting_changed(key, value, ui->cfg.cb.user);
}

void ui_power(struct ui *ui, enum ui_power_action a)
{
	LOGI("ui: %s requested", a == UI_REBOOT ? "reboot" : "power off");
	settings_save(ui->settings);
	gamedb_save(ui->db);
	loader_save_snapshot(ui);
	img_cache_flush();
	/* the power module saves, shows its message and powers off */
	if (ui->cfg.power && ui->cfg.power->request_shutdown) {
		ui->cfg.power->request_shutdown(a == UI_REBOOT ? POWER_REASON_REBOOT : POWER_REASON_USER);
		return;
	}
	if (ui->cfg.cb.power) {
		ui->cfg.cb.power(a, ui->cfg.cb.user);
		return;
	}
	sync();
	if (a == UI_REBOOT)
		execl("/sbin/reboot", "reboot", (char *)NULL);
	else
		execl("/sbin/poweroff", "poweroff", (char *)NULL);
	LOGE("ui: cannot exec reboot/poweroff");
}

/* ----------------------------------------------------------------- themes */
static bool find_theme_dir(struct ui *ui, const char *name, char *out, size_t n)
{
	const char *dirs[2] = { ui->cfg.themes_user, ui->cfg.themes_builtin };

	for (int i = 0; i < 2; i++) {
		if (!dirs[i] || !*dirs[i])
			continue;
		snprintf(out, n, "%s/%s", dirs[i], name);
		if (dir_exists(out) && theme_is_set_dir(out))
			return true;
	}
	return false;
}

static int cmp_name64(const void *a, const void *b)
{
	return strcasecmp((const char *)a, (const char *)b);
}

int ui_list_themes(struct ui *ui, char names[][64], int max)
{
	const char *dirs[2] = { ui->cfg.themes_builtin, ui->cfg.themes_user };
	int n = 0;

	for (int i = 0; i < 2; i++) {
		DIR *d = dirs[i] && *dirs[i] ? opendir(dirs[i]) : NULL;
		struct dirent *de;
		int first = n;

		if (!d)
			continue;
		while ((de = readdir(d)) && n < max) {
			char p[1024];
			bool dup = false;

			if (de->d_name[0] == '.')
				continue;
			snprintf(p, sizeof(p), "%s/%s", dirs[i], de->d_name);
			if (!dir_exists(p) || !theme_is_set_dir(p))
				continue;
			for (int k = 0; k < n; k++)
				if (!strcmp(names[k], de->d_name))
					dup = true;
			if (dup)
				continue;
			strlcpy_(names[n++], de->d_name, 64);
		}
		closedir(d);
		qsort(names[first], (size_t)(n - first), 64, cmp_name64);
	}
	return n;
}

void ui_assets_invalidate(struct ui *ui, bool look)
{
	prefetch_quiesce(ui);
	glview_prebuilt_drop(ui);
	if (look)
		ui->look_gen++;
}

static void free_themes(struct ui *ui)
{
	for (int i = 0; i < ui->nsys; i++) {
		theme_free(ui->sys[i].theme);
		ui->sys[i].theme = NULL;
	}
}

static void update_cache_dir(struct ui *ui)
{
	char p[1024];

	if (!ui->cfg.cache_dir || !*ui->cfg.cache_dir || !ui->w) {
		img_set_cache_dir(NULL);
		return;
	}
	snprintf(p, sizeof(p), "%s/%s/%dx%d", ui->cfg.cache_dir, ui->theme_name[0] ?
		 ui->theme_name : "none", ui->w, ui->h);
	img_set_cache_dir(p);
	/* the first frame opens its images there: one read instead of ~64
	 * per folder level (fswarm.h) */
	fswarm((const char *const[]){ p }, 1, 0, NULL);
}

static void select_theme(struct ui *ui, const char *name)
{
	char dir[1024];
	char list[24][64];

	if (!name || !find_theme_dir(ui, name, dir, sizeof(dir))) {
		const char *def = ui->cfg.default_theme;

		if (name)
			LOGW("ui: theme \"%s\" not found", name);
		name = NULL;
		if (def && find_theme_dir(ui, def, dir, sizeof(dir))) {
			name = def;
		} else if (ui_list_themes(ui, list, 24) > 0 &&
			   find_theme_dir(ui, list[0], dir, sizeof(dir))) {
			name = list[0];
		}
	}
	if (name) {
		strlcpy_(ui->theme_name, name, sizeof(ui->theme_name));
		strlcpy_(ui->theme_dir, dir, sizeof(ui->theme_dir));
	} else {
		LOGW("ui: no theme installed, using the built-in look");
		ui->theme_name[0] = 0;
		ui->theme_dir[0] = 0;
	}
	update_cache_dir(ui);
}

struct theme *ui_sys_theme(struct ui *ui, struct sysent *se)
{
	if (!se->theme) {
		struct theme_sysinfo si = { se->name, se->fullname, se->theme_alias[0] };
		int64_t t0 = ui_now_us();
		struct ui_cost_scope cs;

		ui_cost_begin(&cs, UI_COST_THEME);
		se->theme = theme_load_system(ui->theme_dir, se->is_collection ? NULL : se->rom_dir,
					      se->theme_alias, &si);
		strlcpy_(se->theme_name, si.theme ? si.theme : "", sizeof(se->theme_name));
		ui_cost_end(&cs);
		se->theme_us = ui_now_us() - t0;
		ui->timings.theme_us += se->theme_us;
	}
	return se->theme;
}

void ui_refresh_look(struct ui *ui)
{
	const struct theme *t = ui->nsys ? ui_sys_theme(ui, &ui->sys[0]) : NULL;

	menu_style_from_theme(ui, &ui->ms);
	LOGI("ui: menu style bg %08x dim %08x text %08x selector %08x", ui->ms.bg, ui->ms.dim, ui->ms.text, ui->ms.selector);
	/* menus use the help style of the game lists, as in ES */
	help_style_default(&ui->menu_help);
	help_style_apply(&ui->menu_help, theme_elem(t, "basic", "help", "helpsystem"));
	help_style_apply(&ui->menu_help, theme_elem(t, "system", "help", "helpsystem"));
	/* on top of a dimmed screen: make sure the help stays readable */
	if (gfx_color_luma(ui->menu_help.text) < 100)
		ui->menu_help.text = ui->menu_help.icon = 0xffe0e0e0u;
}

static void rebuild_views(struct ui *ui)
{
	for (int i = 0; i < ui->nstack; i++) {
		struct screen *s = ui->stack[i];

		if (s->kind == SCR_SYSVIEW)
			sysview_set_cursor(ui, s, ui->sys_cursor);
		else if (s->kind == SCR_GLVIEW)
			glview_refresh(ui, s);
		else if (s->ops->relayout)
			s->ops->relayout(ui, s);
	}
	ui_invalidate_snapshot(ui);
}

void ui_set_theme(struct ui *ui, const char *name)
{
	int64_t t0 = ui_now_us();

	if (!strcmp(name, ui->theme_name))
		return;
	/* the worker reads the old themes: stop it first. The new theme's
	 * assets are built by it again, the current system's first (the
	 * carousel shows placeholders for the few frames that takes) */
	ui_assets_invalidate(ui, true);
	/* views hold images of the old theme: release them first */
	for (int i = 0; i < ui->nstack; i++)
		if (ui->stack[i]->ops->relayout)
			ui->stack[i]->ops->relayout(ui, ui->stack[i]);
	free_themes(ui);
	select_theme(ui, name);
	settings_set(ui->settings, "theme", ui->theme_name);
	settings_save(ui->settings);
	for (int i = 0; i < ui->nicons; i++)
		gfx_image_free(ui->icons[i].img);
	ui->nicons = 0;
	ui_refresh_look(ui);
	rebuild_views(ui);
	img_trim();
	ui_setting_changed(ui, "theme", ui->theme_name);
	LOGI("ui: theme %s in %lld ms", ui->theme_name, (long long)(ui_now_us() - t0) / 1000);
}

/*
 * Live language switch: menus (their items hold translated copies) are
 * closed, then everything that holds text is rebuilt like for a theme
 * switch: carousel names, themes (${system.fullName}, our themes' labels),
 * views, icons, fonts (the CJK order follows the language). Dialogs and the
 * keyboard are left alone (their callbacks are pending): they close in the
 * old language.
 */
int ui_set_language(struct ui *ui, const char *code, bool save)
{
	int64_t t0 = ui_now_us();
	int r;

	ui_assets_invalidate(ui, true);   /* before the catalog and fonts change */
	r = i18n_set_language(code);
	while (ui->nstack > 1 && screen_is_menu(ui_top(ui)))
		ui_pop(ui);
	for (int i = 0; i < ui->nstack; i++)
		if (ui->stack[i]->ops->relayout)
			ui->stack[i]->ops->relayout(ui, ui->stack[i]);
	for (int i = 0; i < ui->nicons; i++)
		gfx_image_free(ui->icons[i].img);
	ui->nicons = 0;
	font_cache_clear();
	font_setup_dir(ui->fonts_dir);
	loader_relabel(ui);
	free_themes(ui);
	ui_refresh_look(ui);
	rebuild_views(ui);
	img_trim();
	if (save) {
		settings_set(ui->settings, "language", i18n_language());
		if (settings_save(ui->settings) < 0)
			ui_toastf(ui, "%s", _("Could not save the settings"));
	}
	ui->lang_prompt = false;
	ui->dirty = true;
	ui_setting_changed(ui, "language", i18n_language());
	LOGI("ui: language %s (%d translations)%s in %lld ms", i18n_language(), i18n_catalog_entries(),
	     r < 0 ? " (the one asked for has no catalog)" : "", (long long)(ui_now_us() - t0) / 1000);
	return r;
}

bool ui_first_boot_busy(const struct ui *ui)
{
	return ui->lang_prompt;
}

/* ---------------------------------------------------------------- launching */
const char *ui_game_core(struct ui *ui, const struct game *g, const char **name)
{
	const struct core_info *cores[MAX_CORES_PER_SYSTEM];
	int n = systems_cores_for_file(g->system, g->path, cores, MAX_CORES_PER_SYSTEM);
	char key[64];
	const char *want;
	const struct core_info *c = NULL;

	snprintf(key, sizeof(key), "core.%s", g->system);
	want = g->core && *g->core ? g->core : settings_get(ui->settings, key, "");
	for (int i = 0; i < n && want && *want; i++)
		if (!strcmp(cores[i]->id, want))
			c = cores[i];
	if (!c && n)
		c = cores[0];
	if (name)
		*name = c ? c->name : "";
	return c ? c->id : "";
}

static struct game *real_game(struct ui *ui, const struct game *g)
{
	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *gl = ui->sys[i].games;

		if (ui->sys[i].is_collection || !gl || strcmp(gl->system, g->system))
			continue;
		for (int k = 0; k < gl->n; k++)
			if (!strcmp(gl->games[k].path, g->path))
				return &gl->games[k];
	}
	return NULL;
}

static void fill_launch(struct ui *ui, const struct game *g, struct ui_launch *req)
{
	const char *core_name;
	const char *core = ui_game_core(ui, g, &core_name);
	const struct core_info *ci = systems_core(core);
	char key[64];
	const char *sys_core;

	memset(req, 0, sizeof(*req));
	req->rom_path = g->path;
	req->system = g->system;
	req->core = core;
	req->core_path = ci ? ci->library : "";
	req->game_name = g->name;
	req->bios_dir = ui->cfg.bios_dir;
	snprintf(key, sizeof(key), "core.%s", g->system);
	sys_core = settings_get(ui->settings, key, "");
	req->core_source = g->core && *g->core && !strcmp(g->core, core) ? "game" :
			   sys_core && *sys_core && !strcmp(sys_core, core) ? "system" : "default";
	req->scale = g->scale && *g->scale ? g->scale : NULL;
	req->cpu = g->cpu && *g->cpu ? g->cpu : NULL;
}

/*
 * A game's CPU profile (game options, in-game menu): "performance" = the
 * game governor without a cap, "powersave" = schedutil capped at 720 MHz
 * (TODO(hw): check the saving and that 8/16-bit cores keep full speed),
 * anything else = the core's .ini (cpu_governor, cpu_max_khz).
 */
#define CPU_POWERSAVE_KHZ 720000

static void apply_game_cpu(struct ui *ui, const char *profile, const struct core_info *ci)
{
	if (!ui->cfg.power || !ui->cfg.power->set_game_cpu)
		return;
	if (profile && !strcmp(profile, "performance"))
		ui->cfg.power->set_game_cpu("performance", 0);
	else if (profile && !strcmp(profile, "powersave"))
		ui->cfg.power->set_game_cpu("schedutil", CPU_POWERSAVE_KHZ);
	else
		ui->cfg.power->set_game_cpu(ci && ci->cpu_governor[0] ? ci->cpu_governor : NULL,
					    ci ? ci->cpu_max_khz : 0);
	LOGI("ui: CPU profile %s", profile && *profile ? profile : "auto (the core's)");
}

/* Runs the launch callback for a filled request (fill_launch, or the boot
 * resume offer with its own core), then records the play. */
static void launch_req(struct ui *ui, struct game *g, struct ui_launch *req_in)
{
	struct ui_launch req = *req_in;
	const struct core_info *ci;
	const char *core;
	int r = 0;
	struct game *rg;
	char launch_msg[256] = "";   /* INTEGRATION FIX: message from the launch callback */

	core = req.core;
	ci = systems_core(core);
	req.message = launch_msg;
	req.message_size = sizeof(launch_msg);
	if (ui->cfg.transfer)
		transfer_before_game(ui);
	LOGI("ui: launch \"%s\" (%s) with %s (%s)%s", g->name, g->path, core, req.core_source ? req.core_source : "",
	     req.resume ? ", resume" : "");
	if (ui->nsys > 0)
		settings_set(ui->settings, "last_system", ui->sys[CLAMP(ui->sys_cursor, 0, ui->nsys - 1)].name);
	settings_save(ui->settings);
	apply_game_cpu(ui, req.cpu, ci);   /* the game's profile, else the core's */
	if (ui->cfg.power && ui->cfg.power->set_game_running)
		ui->cfg.power->set_game_running(true);
	loader_pause(ui, true);   /* the SD card belongs to the game */
	prefetch_pause(ui, true); /* and the CPU (the job running ends first) */
	ui->in_game = true;              /* a USB drive plugged now waits for the end */
	if (ui->cfg.cb.launch)
		r = ui->cfg.cb.launch(&req, ui->cfg.cb.user);
	else
		ui_toastf(ui, "Launch stub: %s / %s", core, path_basename(g->path));
	ui->in_game = false;
	prefetch_pause(ui, false);
	loader_pause(ui, false);
	if (ui->cfg.transfer)
		transfer_after_game(ui);    /* under a launch error message, if any */
	if (ui->cfg.power && ui->cfg.power->set_game_running)
		ui->cfg.power->set_game_running(false);
	if (r < 0) {
		/* INTEGRATION FIX: the callback's own message when it gave one */
		message_open(ui, launch_msg[0] ? launch_msg : _("The game could not be started."));
		return;
	}
	if (launch_msg[0])   /* INTEGRATION FIX: e.g. the game crashed after starting */
		message_open(ui, launch_msg);
	rg = real_game(ui, g);
	gamedb_played(ui->db, rg ? rg : g, (int64_t)time(NULL));
	if (rg && rg != g) {
		g->lastplayed = rg->lastplayed;
		g->playcount = rg->playcount;
	}
	gamedb_save(ui->db);
	/* the display may have been reconfigured by the game: redraw all */
	ui_invalidate_snapshot(ui);
}

static void do_launch(struct ui *ui, struct game *g, bool resume, bool start_fresh)
{
	struct ui_launch req;

	fill_launch(ui, g, &req);
	req.resume = resume;
	req.start_fresh = start_fresh && !resume;
	launch_req(ui, g, &req);
}

/* ------------------------------------------------------ boot resume offer */
/* The list entry of a game (last played, play count), loading its list now
 * if the background loader has not reached it yet; else a stand-in entry
 * in *tmp (a game whose folder is an alias), rel in rel[]. */
static struct game *find_game(struct ui *ui, const char *system, const char *rom, const char *name,
			      struct game *tmp, char *rel, size_t reln, bool load)
{
	struct game *g = NULL;

	for (int i = 0; i < ui->nsys; i++) {
		struct sysent *se = &ui->sys[i];

		if (se->is_collection || strcmp(se->name, system))
			continue;
		/* during a game (play time, settings from the game process) only
		 * the lists already there: the loader is paused */
		if (load)
			se = ui_system_ready(ui, se);
		if (se && se->games)
			for (int k = 0; k < se->games->n && !g; k++)
				if (!strcmp(se->games->games[k].path, rom))
					g = &se->games->games[k];
		break;
	}
	if (!g && tmp) {
		const char *s = strstr(rom, "/roms/");

		memset(tmp, 0, sizeof(*tmp));
		if (s && (s = strchr(s + 6, '/')))
			strlcpy_(rel, s + 1, reln);
		else
			strlcpy_(rel, path_basename(rom), reln);
		tmp->path = rom;
		tmp->rel = rel;
		tmp->system = system;
		tmp->name = name ? name : path_basename(rom);
		tmp->rating = -1;
		g = tmp;
	}
	return g;
}

/* "Resume <game>?" at boot: 0 = resume, 1 = start fresh, -1 (B) = start
 * fresh, 2 = always resume (saved), 3 = never ask (saved, start fresh).
 * user != NULL: decided by Settings > Games > Resume on boot, no dialog. */
static void boot_resume_choice(struct ui *ui, int choice, void *user)
{
	struct ui_launch req;
	struct game tmp, *g = NULL;
	bool resume = choice == 0 || choice == 2;
	char rel[1024];

	ui->boot_resume.active = false;
	/* the player said START FRESH (or B): the old state goes aside */
	if (!user && (choice == 1 || choice == -1) && ui->boot_resume.start_fresh) {
		struct ui_resume_offer o = {
			.game_name = ui->boot_resume.name, .rom_path = ui->boot_resume.rom,
			.system = ui->boot_resume.system, .core = ui->boot_resume.core,
			.core_path = ui->boot_resume.core_path, .user = ui->boot_resume.user,
		};

		ui->boot_resume.start_fresh(&o, ui->boot_resume.user);
	}
	LOGI("ui: boot resume \"%s\": %s%s", ui->boot_resume.name, resume ? "resume" : "start fresh",
	     choice == 2 ? " (always, saved)" : choice == 3 ? " (never ask, saved)" : "");
	if (choice == 2 || choice == 3) {
		settings_set(ui->settings, "resume_boot", choice == 2 ? "always" : "never");
		if (settings_save(ui->settings) < 0)
			ui_toastf(ui, "%s", _("Could not save the settings"));
		else
			/* TRANSLATORS: toast after ALWAYS RESUME / NEVER ASK at boot; the
			 * path must match your translations of Settings and Games */
			ui_toast_long(ui, "%s", _("Setting saved: you can change it in Settings > Games"));
	}
	if (ui->boot_resume.answered)
		ui->boot_resume.answered(resume, ui->boot_resume.user);
	if (!resume)
		return;   /* the menu as it is (START FRESH / B: the state went aside above) */
	g = find_game(ui, ui->boot_resume.system, ui->boot_resume.rom, ui->boot_resume.name, &tmp, rel,
		      sizeof(rel), true);
	memset(&req, 0, sizeof(req));
	req.rom_path = ui->boot_resume.rom;
	req.system = ui->boot_resume.system;
	req.core = ui->boot_resume.core;          /* the core the state was saved with */
	req.core_path = ui->boot_resume.core_path;
	req.game_name = ui->boot_resume.name;
	req.bios_dir = ui->cfg.bios_dir;
	req.core_source = "resume";
	req.resume = true;
	req.boot_resume = true;
	req.scale = g->scale;
	req.cpu = g->cpu;
	launch_req(ui, g, &req);
}

void ui_offer_resume(struct ui *ui, const struct ui_resume_offer *o)
{
	const char *buttons[5];
	const char *mode;
	char text[320];
	struct screen *d;

	if (!ui || !o || !o->rom_path || ui->boot_resume.active)
		return;
	strlcpy_(ui->boot_resume.name, o->game_name && *o->game_name ? o->game_name :
		 path_basename(o->rom_path), sizeof(ui->boot_resume.name));
	strlcpy_(ui->boot_resume.rom, o->rom_path, sizeof(ui->boot_resume.rom));
	strlcpy_(ui->boot_resume.system, o->system ? o->system : "", sizeof(ui->boot_resume.system));
	strlcpy_(ui->boot_resume.core, o->core ? o->core : "", sizeof(ui->boot_resume.core));
	strlcpy_(ui->boot_resume.core_path, o->core_path ? o->core_path : "", sizeof(ui->boot_resume.core_path));
	ui->boot_resume.answered = o->answered;
	ui->boot_resume.start_fresh = o->start_fresh;
	ui->boot_resume.user = o->user;
	ui->boot_resume.active = true;
	/* Settings > Games > Resume on boot */
	mode = settings_get(ui->settings, "resume_boot", "ask");
	if (mode && !strcmp(mode, "never")) {
		LOGI("ui: boot resume \"%s\": resume_boot never, the menu (auto state kept)", ui->boot_resume.name);
		boot_resume_choice(ui, 1, ui);
		return;
	}
	if (mode && !strcmp(mode, "always")) {
		LOGI("ui: boot resume \"%s\": resume_boot always, resuming", ui->boot_resume.name);
		boot_resume_choice(ui, 0, ui);
		return;
	}
	/* TRANSLATORS: dialog buttons, uppercase, short: continue the saved game /
	 * start the game from the beginning */
	buttons[0] = _("RESUME");
	buttons[1] = _("START FRESH");
	/* TRANSLATORS: boot resume dialog buttons, uppercase, short: resume now and
	 * at every start from now on / start fresh now and never ask again (both
	 * saved in Settings > Games > Resume on boot) */
	buttons[2] = _("ALWAYS RESUME");
	buttons[3] = _("NEVER ASK");
	buttons[4] = NULL;
	/* TRANSLATORS: %s is a game name: continue it where it was left? */
	snprintf(text, sizeof(text), _("Resume %s?"), ui->boot_resume.name);
	LOGI("ui: boot resume offered: \"%s\" (%s, %s)", ui->boot_resume.name, o->rom_path, ui->boot_resume.core);
	d = dialog_open(ui, text, buttons, boot_resume_choice, NULL);
	dialog_select(d, 0);
	ui->dirty = true;
}

/* ------------------------------------------------ switcher, play time API */
static int cmp_recent_desc(const void *a, const void *b)
{
	const struct ui_recent_game *x = a, *y = b;

	return x->lastplayed < y->lastplayed ? 1 : x->lastplayed > y->lastplayed ? -1 : 0;
}

int ui_recent_games(struct ui *ui, struct ui_recent_game *out, int max)
{
	int n = 0, cap = 0;
	struct ui_recent_game *all = NULL;

	if (!ui || max <= 0)
		return 0;
	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *gl = ui->sys[i].games;

		for (int k = 0; !ui->sys[i].is_collection && gl && k < gl->n; k++) {
			const struct game *g = &gl->games[k];
			const struct core_info *ci;
			const char *cn;

			if (g->lastplayed <= 0)
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 32;
				all = xrealloc(all, sizeof(*all) * (size_t)cap);
			}
			memset(&all[n], 0, sizeof(all[n]));
			strlcpy_(all[n].name, g->name, sizeof(all[n].name));
			strlcpy_(all[n].system, g->system, sizeof(all[n].system));
			strlcpy_(all[n].rom, g->path, sizeof(all[n].rom));
			strlcpy_(all[n].core, ui_game_core(ui, g, &cn), sizeof(all[n].core));
			ci = systems_core(all[n].core);
			strlcpy_(all[n].core_path, ci ? ci->library : "", sizeof(all[n].core_path));
			all[n].lastplayed = g->lastplayed;
			n++;
		}
	}
	if (n > 1)
		qsort(all, (size_t)n, sizeof(*all), cmp_recent_desc);
	if (n > max)
		n = max;
	if (n)
		memcpy(out, all, sizeof(*out) * (size_t)n);
	free(all);
	return n;
}

void ui_switch_to(struct ui *ui, const char *system, const char *rom_path, const char *core)
{
	if (!ui || !system || !rom_path)
		return;
	strlcpy_(ui->switch_req.system, system, sizeof(ui->switch_req.system));
	strlcpy_(ui->switch_req.rom, rom_path, sizeof(ui->switch_req.rom));
	strlcpy_(ui->switch_req.core, core ? core : "", sizeof(ui->switch_req.core));
	ui->switch_req.active = true;
	ui->dirty = true;
	LOGI("ui: game switcher: %s (%s) next", rom_path, system);
}

/* The game the switcher chose: launched like from its list, resumed from
 * its auto state when it has one. */
static void switch_launch(struct ui *ui)
{
	struct ui_launch req;
	struct game tmp, *g;
	char rel[1024];

	ui->switch_req.active = false;
	g = find_game(ui, ui->switch_req.system, ui->switch_req.rom, NULL, &tmp, rel, sizeof(rel), true);
	fill_launch(ui, g, &req);
	if (!req.core[0] && ui->switch_req.core[0]) {
		const struct core_info *ci = systems_core(ui->switch_req.core);

		req.core = ui->switch_req.core;
		req.core_path = ci ? ci->library : "";
	}
	if (!req.core[0]) {
		message_open(ui, _("No emulator is installed for this system."));
		return;
	}
	req.resume = ui->cfg.cb.has_resume && ui->cfg.cb.has_resume(&req, ui->cfg.cb.user);
	req.switched = true;
	LOGI("ui: game switcher: launching \"%s\"%s", g->name, req.resume ? ", resuming" : " (no auto state)");
	launch_req(ui, g, &req);
}

/* Every copy of a game (its list, the collections) and gamedb: f(db, game). */
static struct game *game_for(struct ui *ui, const char *system, const char *rom, struct game *tmp, char *rel,
			     size_t reln)
{
	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *gl = ui->sys[i].games;

		for (int k = 0; !ui->sys[i].is_collection && gl && k < gl->n; k++)
			if (!strcmp(gl->games[k].system, system) && !strcmp(gl->games[k].path, rom))
				return &gl->games[k];
	}
	return find_game(ui, system, rom, NULL, tmp, rel, reln, false);
}

static void game_copies_sync(struct ui *ui, const struct game *g)
{
	for (int i = 0; i < ui->nsys; i++) {
		struct gamelist *gl = ui->sys[i].games;

		for (int k = 0; gl && k < gl->n; k++)
			if (&gl->games[k] != g && !strcmp(gl->games[k].system, g->system) &&
			    !strcmp(gl->games[k].rel, g->rel)) {
				gl->games[k].playtime = g->playtime;
				gl->games[k].scale = g->scale;
				gl->games[k].cpu = g->cpu;
			}
	}
}

void ui_game_add_playtime(struct ui *ui, const char *system, const char *rom_path, int64_t seconds)
{
	struct game tmp, *g;
	char rel[1024];

	if (!ui || !system || !rom_path || seconds <= 0)
		return;
	g = game_for(ui, system, rom_path, &tmp, rel, sizeof(rel));
	gamedb_add_playtime(ui->db, g, seconds);
	game_copies_sync(ui, g);
	if (gamedb_save(ui->db) < 0)
		LOGW("ui: play time of %s not saved", rom_path);
	LOGI("ui: play time %s: +%lld s, %lld s in all", g->rel, (long long)seconds, (long long)g->playtime);
}

void ui_game_set_option(struct ui *ui, const char *system, const char *rom_path, const char *key,
			const char *value)
{
	struct game tmp, *g;
	char rel[1024];

	if (!ui || !system || !rom_path || !key)
		return;
	g = game_for(ui, system, rom_path, &tmp, rel, sizeof(rel));
	if (!strcmp(key, "scale")) {
		gamedb_set_scale(ui->db, g, value);
	} else if (!strcmp(key, "cpu")) {
		gamedb_set_cpu(ui->db, g, value);
		/* the game runs: its new profile now */
		if (ui->in_game) {
			const char *cn;

			apply_game_cpu(ui, g->cpu, systems_core(ui_game_core(ui, g, &cn)));
		}
	} else {
		return;
	}
	game_copies_sync(ui, g);
	gamedb_save(ui->db);
	LOGI("ui: %s for %s: %s (saved for this game)", key, g->rel, value ? value : "");
}

/* "Resume where you left off?": 0 = resume, 1 = start fresh, -1 (B) = cancel. */
static void resume_choice(struct ui *ui, int choice, void *user)
{
	struct game *g = ui->pending_game;

	(void)user;
	ui->pending_game = NULL;
	ui->pending_se = NULL;
	if (!g || choice < 0)
		return;
	do_launch(ui, g, choice == 0, choice == 1);   /* START FRESH: the old state goes aside */
}

void ui_launch_game(struct ui *ui, struct sysent *se, struct game *g)
{
	const char *buttons[3];
	struct ui_launch req;

	buttons[0] = _("RESUME");
	buttons[1] = _("START FRESH");
	buttons[2] = NULL;
	fill_launch(ui, g, &req);
	if (!req.core[0]) {
		message_open(ui, _("No emulator is installed for this system."));
		return;
	}
	if (ui->cfg.transfer) {
		struct transfer_progress pr;

		/* a copy competes for the SD card and the CPU: finish it first */
		if (ui->cfg.transfer->import_status(&pr) == TRANSFER_RUNNING ||
		    (ui->cfg.transfer->backup_status &&
		     ui->cfg.transfer->backup_status(&pr) == TRANSFER_RUNNING)) {
			message_open(ui, _("Files are being copied. Wait for the copy to finish, or cancel it."));
			return;
		}
	}
	/* an auto save state exists (the game was left with Select+Start or a
	 * power-off): Settings > Games > On launch (resume_mode) says whether
	 * to ask (default), always resume or always start fresh. The game
	 * pointer stays valid while the dialog is up (lists are only reloaded
	 * from the carousel). */
	if (ui->cfg.cb.has_resume && ui->cfg.cb.has_resume(&req, ui->cfg.cb.user)) {
		const char *mode = settings_get(ui->settings, "resume_mode", "ask");

		if (mode && !strcmp(mode, "always")) {
			LOGI("ui: resume_mode always: resuming \"%s\"", g->name);
			do_launch(ui, g, true, false);
			return;
		}
		if (mode && !strcmp(mode, "never")) {
			LOGI("ui: resume_mode never: \"%s\" starts fresh", g->name);
			do_launch(ui, g, false, false);
			return;
		}
		ui->pending_se = se;
		ui->pending_game = g;
		dialog_open(ui, _("Resume where you left off?"), buttons, resume_choice, NULL);
		return;
	}
	do_launch(ui, g, false, false);
}

/* ------------------------------------------------------------------ create */
struct ui *ui_create(const struct ui_config *cfg)
{
	struct ui *ui = xcalloc(1, sizeof(*ui));
	struct ui_config def;
	char p[1100];
	int64_t t0 = ui_now_us();

	ui_config_defaults(&def);
	ui->cfg = cfg ? *cfg : def;
	ui->cfg.roms_dir = keep(ui, 0, ui->cfg.roms_dir, def.roms_dir);
	ui->cfg.data_dir = keep(ui, 1, ui->cfg.data_dir, def.data_dir);
	ui->cfg.cache_dir = keep(ui, 2, ui->cfg.cache_dir, "");
	ui->cfg.themes_builtin = keep(ui, 3, ui->cfg.themes_builtin, def.themes_builtin);
	ui->cfg.themes_user = keep(ui, 4, ui->cfg.themes_user, def.themes_user);
	ui->cfg.res_dir = keep(ui, 5, ui->cfg.res_dir, def.res_dir);
	ui->cfg.cores_dir = keep(ui, 6, ui->cfg.cores_dir, def.cores_dir);
	ui->cfg.bios_dir = keep(ui, 7, ui->cfg.bios_dir, def.bios_dir);
	ui->cfg.boot_env = keep(ui, 8, ui->cfg.boot_env, def.boot_env);
	ui->cfg.power_supply_dir = keep(ui, 9, ui->cfg.power_supply_dir, def.power_supply_dir);
	ui->cfg.backlight_dir = keep(ui, 10, ui->cfg.backlight_dir, def.backlight_dir);
	ui->cfg.net_helper = keep(ui, 11, ui->cfg.net_helper, def.net_helper);
	ui->cfg.wpa_conf = keep(ui, 12, ui->cfg.wpa_conf, def.wpa_conf);
	ui->cfg.version = keep(ui, 13, ui->cfg.version, def.version);
	ui->cfg.default_theme = keep(ui, 14, ui->cfg.default_theme, def.default_theme);

	/* The exFAT folders read before the first menu frame, in a few large
	 * reads (a cold folder costs ~110 ms otherwise, see fswarm.h). */
	{
		const char *warm[3] = { ui->cfg.data_dir, ui->cfg.cache_dir, ui->cfg.themes_user };
		struct fswarm_stats ws = { 0 };
		struct stat st;

		fswarm(warm, 3, 0, &ws);
		if (stat(ui->cfg.data_dir, &st) == 0)
			ui->data_dev = st.st_dev;
		ui_io_sample(ui, &ui->io_created);
		if (ws.dirs)
			LOGI("ui: warm-up: %d folders, %d reads, %lld KiB, %lld us", ws.dirs, ws.reads,
			     (long long)ws.bytes / 1024, (long long)ws.us);
	}
	snprintf(ui->settings_path, sizeof(ui->settings_path), "%s/settings.ini", ui->cfg.data_dir);
	ui->settings = settings_open(ui->settings_path);
	snprintf(p, sizeof(p), "%s/gamedb.tsv", ui->cfg.data_dir);
	ui->db = gamedb_open(p);

	/* the language first: the CJK font order depends on it */
	ui->cfg.locale_dir = keep(ui, 15, ui->cfg.locale_dir, "/usr/share/rsos/locale");
	i18n_set_dir(ui->cfg.locale_dir);
	{
		const char *lang = ui->cfg.language && *ui->cfg.language ? ui->cfg.language :
				   settings_get(ui->settings, "language", NULL);

		if (lang && *lang) {
			if (i18n_set_language(lang) < 0)
				LOGW("ui: language \"%s\": no catalog in %s, English", lang, ui->cfg.locale_dir);
		} else {
			i18n_set_language("en");
			/* nothing chosen yet: the picker before the menu */
			ui->lang_prompt = ui->cfg.language_prompt;
		}
		ui->cfg.language = NULL;   /* the caller's string need not outlive ui_create */
		LOGI("ui: language %s (%d translations)%s", i18n_language(), i18n_catalog_entries(),
		     ui->lang_prompt ? ", first boot: the language picker first" : "");
	}
	/* fonts: ":/" in themes is the resource dir; DejaVu Sans is the default
	 * and the first fallback, then the CJK subsets (gfx/font.h) */
	theme_set_resource_dir(ui->cfg.res_dir);
	snprintf(ui->fonts_dir, sizeof(ui->fonts_dir), "%s/fonts", ui->cfg.res_dir);
	snprintf(p, sizeof(p), "%s/DejaVuSans.ttf", ui->fonts_dir);
	if (!file_exists(p))
		LOGE("ui: default font %s missing", p);
	font_setup_dir(ui->fonts_dir);
	LOGI("ui: fonts %s, bold %s, %d fallback font(s)", font_default_path(false), font_default_path(true),
	     font_fallback_count());

	systems_load_cores(ui->cfg.cores_dir);
	apply_power_settings(ui);
	select_theme(ui, settings_get(ui->settings, "theme", NULL));
	/* the default menu look until a theme is loaded: with no game at all
	 * the carousel is never rebuilt, and menus and dialogs (the USB one
	 * included) would be drawn with an all-zero, invisible style */
	ui_refresh_look(ui);
	loader_init(ui);
	prefetch_init(ui);        /* its thread starts after the first frame */
	ui->snapshot_depth = -1;
	ui->created_at = ui_now_ms();
	ui->load_started = -1;
	ui->timings.create_us = ui_now_us() - t0;
	{
		struct ui_io io;

		ui_io_sample(ui, &io);
		LOGI("ui: created in %lld us, theme %s (io %lld reads, %lld KiB)",
		     (long long)ui->timings.create_us, ui->theme_name,
		     (long long)(io.reads - ui->io_created.reads),
		     (long long)(io.sectors - ui->io_created.sectors) / 2);
	}
	return ui;
}

void ui_destroy(struct ui *ui)
{
	if (!ui)
		return;
	glview_prebuilt_drop(ui);
	prefetch_destroy(ui);     /* joined before anything it reads goes */
	loader_save_snapshot(ui);
	while (ui->nstack) {
		struct screen *s = ui->stack[--ui->nstack];

		s->ops->destroy(ui, s);
	}
	gfx_image_free(ui->snapshot);
	for (int i = 0; i < ui->nicons; i++)
		gfx_image_free(ui->icons[i].img);
	loader_destroy(ui);
	settings_save(ui->settings);
	settings_close(ui->settings);
	gamedb_save(ui->db);
	gamedb_close(ui->db);
	img_trim();
	img_cache_flush();
	font_cache_clear();
	systems_free();
	free(ui);
}

void ui_get_timings(const struct ui *ui, struct ui_timings *t)
{
	*t = ui->timings;
}

/* ------------------------------------------------------------------ size */
void ui_set_size(struct ui *ui, int w, int h)
{
	if (w == ui->w && h == ui->h)
		return;
	LOGI("ui: logical size %dx%d", w, h);
	ui_assets_invalidate(ui, true);   /* fonts, the cache folder change */
	for (int i = 0; i < ui->nstack; i++)
		if (ui->stack[i]->ops->relayout)
			ui->stack[i]->ops->relayout(ui, ui->stack[i]);
	for (int i = 0; i < ui->nicons; i++)
		gfx_image_free(ui->icons[i].img);
	ui->nicons = 0;
	ui_invalidate_snapshot(ui);
	font_cache_clear();
	ui->w = w;
	ui->h = h;
	update_cache_dir(ui);
	img_trim();
	ui->dirty = true;
}

void ui_set_output(struct ui *ui, bool hdmi)
{
	ui->hdmi = hdmi;
	if (ui->cfg.input)
		input_set_docked(ui->cfg.input, hdmi);
	ui->dirty = true;
}

/* ------------------------------------------------------------------ input */
void ui_button(struct ui *ui, enum input_btn btn, enum input_nav_type type)
{
	struct screen *s = ui_top(ui);

	/* idle dimming / sleep: a key that wakes the screen is swallowed */
	if (ui->cfg.power && ui->cfg.power->on_input && ui->cfg.power->on_input())
		return;
	if (!ui->loaded || !s || ui->charge_mode || ui->big_msg[0])
		return;
	prefetch_input(ui);           /* speculative work waits for a quiet moment */
	s->ops->button(ui, s, btn, type);
	ui->dirty = true;
}

static void pad_configure(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0)
		settings_open_main(ui);
}

void ui_hotkey(struct ui *ui, enum input_hotkey hk)
{
	const char *buttons[3];
	char msg[320];

	/* TRANSLATORS: dialog buttons, uppercase, short */
	buttons[0] = _("SETTINGS");
	buttons[1] = _("LATER");
	buttons[2] = NULL;

	switch (hk) {
	case IN_HK_BRIGHTNESS:
		ui->bright_until = ui->now + BRIGHT_MS;
		ui->dirty = true;
		if (ui->cfg.input) {
			int max, cur = input_brightness_get(ui->cfg.input, &max);

			if (cur >= 0 && max > 0) {
				settings_set_int(ui->settings, "brightness",
						 CLAMP((cur * 10 + max / 2) / max, 1, 10));
				settings_save(ui->settings);
			}
		}
		break;
	case IN_HK_POWER_SHORT:
	case IN_HK_POWER_OFF:
		/* the power module owns the power key (docs/power.md §5) */
		break;
	case IN_HK_PAD_CONNECTED:
		ui_toastf(ui, _("Controller connected: %s"), ui->cfg.input ? input_last_pad(ui->cfg.input) : "");
		break;
	case IN_HK_PAD_DISCONNECTED:
		ui_toastf(ui, _("Controller removed: %s"), ui->cfg.input ? input_last_pad(ui->cfg.input) : "");
		break;
	case IN_HK_PAD_UNCONFIGURED:
		if (!ui->loaded)
			break;
		/* One question at a time: pads plugged while a dialog is up (or a
		 * full screen stack) get a toast, not a pile of dialogs (F-M3). */
		if (screen_is_dialog(ui_top(ui)) || screen_is_osk(ui_top(ui))) {
			/* TRANSLATORS: %s is the controller's name; "Settings > Controls" is
			 * the menu path: use your translations of "Settings" and "Controls" */
			ui_toastf(ui, _("New controller: %s (Settings > Controls)"),
				  ui->cfg.input ? input_last_pad(ui->cfg.input) : "");
			break;
		}
		snprintf(msg, sizeof(msg), _("New controller: %s. Configure it in Settings > Controls?"),
			 ui->cfg.input ? input_last_pad(ui->cfg.input) : "");
		if (!dialog_open(ui, msg, buttons, pad_configure, NULL))
			ui_toastf(ui, _("New controller: %s (Settings > Controls)"),
				  ui->cfg.input ? input_last_pad(ui->cfg.input) : "");
		break;
	case IN_HK_PORTS_CHANGED:
		if (ui->cfg.input && ui->loaded) {
			struct input_port_info pi;

			input_port_info(ui->cfg.input, 0, &pi);
			if (pi.connected)
				ui_toastf(ui, _("Player 1: %s"), pi.name);
		}
		break;
	default:
		break;
	}
}

/* ----------------------------------------------------------------- update */
void ui_io_sample(struct ui *ui, struct ui_io *io)
{
	char p[64];
	FILE *f;
	long long r, rm, s;

	io->reads = io->sectors = 0;
	if (!ui->data_dev)
		return;
	snprintf(p, sizeof(p), "/sys/dev/block/%u:%u/stat", major(ui->data_dev), minor(ui->data_dev));
	f = fopen(p, "re");
	if (!f)
		return;
	if (fscanf(f, "%lld %lld %lld", &r, &rm, &s) == 3) {
		io->reads = r;
		io->sectors = s;
	}
	fclose(f);
}

bool ui_update(struct ui *ui, int64_t now_ms)
{
	struct hw_job_result jr;
	struct screen *s;
	int step = ui->load_step;

	ui->now = now_ms;
	if (ui->charge_mode)
		return ui->dirty;
	if (loader_step(ui))
		ui->dirty = true;
	if (!ui->loaded) {
		/* first boot: "Preparing your console" while the lists load */
		if (ui->load_step != step || !ui->load_msg[0]) {
			const struct sysdef *sd = systems_get(MAX(0, ui->load_step - 1));

			/* TRANSLATORS: first boot, "Preparing your console": %s is a system name */
			snprintf(ui->load_msg, sizeof(ui->load_msg), _("Reading %s games"),
				 sd ? C_("system", sd->fullname) : "");
			ui->dirty = true;
		}
		return ui->dirty;
	}
	/* first boot (no language chosen yet): the picker, before anything else */
	if (ui->lang_prompt && !ui->lang_picker && ui_top(ui)) {
		language_open(ui, true);
		ui->dirty = true;
	}
	while (hw_job_poll(&jr)) {
		screens_job_done(ui, &jr);
		ui->dirty = true;
	}
	/* the asset worker: install what it built (the jobs set dirty when it
	 * shows); its speculative part starts once the menu is up and every
	 * list is in (never on the boot path) */
	prefetch_poll(ui);
	if (ui->first_frame_done && ui_lists_complete(ui) && !ui->lang_prompt)
		prefetch_allow_background(ui);
	if (ui->nstack)
		sysview_prefetch(ui, ui->stack[0]);
	/* the game switcher chose another game: it starts now (the launch
	 * callback of the one before has returned) */
	if (ui->switch_req.active && !ui->in_game) {
		switch_launch(ui);
		ui->dirty = true;
	}
	if (ui->check_changes) {
		ui->check_changes = false;
		/* only while the carousel is on top: a screen pushed meanwhile (the
		 * import progress, whose copy changes the folders) must not be
		 * popped by the reload */
		if (ui_top(ui) && ui_top(ui)->kind == SCR_SYSVIEW)
			loader_check_changes(ui);
	}
	transfer_poll(ui);
	update_poll(ui);                /* the system updater's helper (update_ui.c) */
	data_problem_poll(ui);          /* /data unreadable: the storage screen (screens.c) */
	if (ui->reload_pending && ui_top(ui) && ui_top(ui)->kind == SCR_SYSVIEW) {
		ui->reload_pending = false;
		ui_reload_games(ui, false);
	}
	s = ui_top(ui);
	if (s && s->ops->update && s->ops->update(ui, s))
		ui->dirty = true;
	if (ui->toast_pending_ms && ui->loaded) {
		ui->toast_until = ui->now + ui->toast_pending_ms;
		ui->toast_pending_ms = 0;
		ui->dirty = true;
	}
	if (ui->toast_until && ui->now >= ui->toast_until) {
		ui->toast_until = 0;
		ui->dirty = true;
	}
	if (ui->bright_until && ui->now >= ui->bright_until) {
		ui->bright_until = 0;
		ui->dirty = true;
	}
	if (ui->flush_at && ui->now >= ui->flush_at && !ui->dirty) {
		ui->flush_at = 0;
		img_cache_flush();
	}
	return ui->dirty;
}

int ui_timeout_ms(const struct ui *ui, int64_t now_ms)
{
	int t = -1;
	struct screen *s;

	if (ui->charge_mode)
		return ui->dirty ? 0 : 5000;
	if (ui->dirty)
		return 0;
	t = loader_timeout(ui);     /* lists arriving: poll for them */
	if (!ui->loaded)
		return t;
	s = ui->nstack ? ui->stack[ui->nstack - 1] : NULL;
#define MINT(v) do { int _v = (int)(v); if (_v >= 0 && (t < 0 || _v < t)) t = _v; } while (0)
	if (s && s->ops->timeout)
		MINT(s->ops->timeout((struct ui *)ui, s));
	if (ui->toast_until)
		MINT(MAX(0, ui->toast_until - now_ms));
	if (ui->toast_pending_ms)
		MINT(0);
	if (ui->bright_until)
		MINT(MAX(0, ui->bright_until - now_ms));
	if (hw_jobs_running())
		MINT(100);
	if (ui->flush_at)
		MINT(MAX(0, ui->flush_at - now_ms));
	if (ui->scan_orphan)
		MINT(200);
	MINT(prefetch_timeout(ui));     /* results of the asset worker to install */
	MINT(update_timeout(ui));
	if (ui->data_problem.pid > 0)
		MINT(250);                  /* the storage format helper: polled */
	if (ui->transfer_watch)
		MINT(500);                  /* a copy under a dialog: finished when it ends */
	if (ui->usb_pending[0] && !ui->in_game)
		MINT(0);                  /* the dialog of a drive plugged meanwhile */
	if (ui->cfg.transfer && ui->cfg.transfer->webshare_running())
		MINT(2000);
#undef MINT
	return t;
}

/* ------------------------------------------------------------------ render */
static void render_loading(struct ui *ui, struct gfx_surface *s)
{
	struct font *fb = font_get(font_default_path(true), ui_font_px(ui, 0.08f));
	struct font *f = font_get(NULL, ui_font_px(ui, 0.04f));
	int n = MAX(1, systems_count());
	int bw = ui->w / 2, bh = MAX(4, ui->h / 80);
	int bx = (ui->w - bw) / 2, by = ui->h * 62 / 100;

	gfx_fill_gradient(s, 0, 0, ui->w, ui->h, 0xff1a1d24u, 0xff0c0d10u, false);
	draw_text_box(ui, s, fb, "RetroStoneOS", 0, ui->h * 30 / 100, ui->w, ui->h / 6, AL_CENTER,
		      0xfff2f2f2u);
	/* TRANSLATORS: first boot, while the game lists are read */
	draw_text_box(ui, s, f, _("Preparing your console..."), 0, ui->h * 47 / 100, ui->w, ui->h / 12,
		      AL_CENTER, 0xffa8b0bcu);
	gfx_fill_round(s, bx, by, bw, bh, bh / 2, 0xff2c3038u);
	gfx_fill_round(s, bx, by, MAX(bh, bw * ui->load_step / n), bh, bh / 2, 0xff4c8dffu);
	draw_text_box(ui, s, f, ui->load_msg, 0, by + bh * 2, ui->w, ui->h / 12, AL_CENTER,
		      0xff6c7480u);
}

static void render_overlays(struct ui *ui, struct gfx_surface *s)
{
	const struct menu_style *ms = &ui->ms;

	if (ui->toast_until && ui->toast[0]) {
		static const gfx_color bg[] = { 0xe0202226u, 0xf0a0620cu, 0xf0b3261eu };
		struct font *f = font_get(ms->font_path, ui_font_px(ui, 0.04f));
		int tw = font_text_width(f, ui->toast, -1);
		int h = font_height(f) * 2, pad = h / 2;
		int w = MIN(ui->w - 20, tw + 2 * pad);
		int x = (ui->w - w) / 2, y = ui->h - h - ui->h / 9;

		gfx_fill_round(s, x, y, w, h, h / 2, bg[CLAMP(ui->toast_sev, 0, 2)]);
		draw_text_box(ui, s, f, ui->toast, x + pad, y, w - 2 * pad, h, AL_CENTER, 0xfff0f0f0u);
	}
	if (ui->bright_until && ui->cfg.input) {
		int max, cur = input_brightness_get(ui->cfg.input, &max);
		int w = ui->w / 3, h = MAX(6, ui->h / 48), x = (ui->w - w) / 2, y = ui->h / 12;
		int pad = h;

		if (cur >= 0 && max > 0) {
			struct font *f = font_get(NULL, ui_font_px(ui, 0.035f));

			gfx_fill_round(s, x - pad, y - pad * 3, w + 2 * pad, h + pad * 4, pad, 0xe0202226u);
			draw_text_box(ui, s, f, _("Brightness"), x, y - pad * 3, w, pad * 3, AL_CENTER,
				      0xfff0f0f0u);
			gfx_fill_round(s, x, y, w, h, h / 2, 0xff454a52u);
			gfx_fill_round(s, x, y, MAX(h, w * cur / max), h, h / 2, 0xfff0c040u);
		}
	}
}

void ui_render(struct ui *ui, struct gfx_surface *s)
{
	struct screen *top;
	int64_t t0 = ui_now_us(), theme0 = ui->timings.theme_us;
	struct img_stats is0 = { 0 };
	struct ui_io io0 = { 0 };

	if (!ui->first_frame_done && ui->loaded) {
		img_get_stats(&is0);
		ui_io_sample(ui, &io0);
	}
	ui->dirty = false;
	if (s->w != ui->w || s->h != ui->h)
		ui_set_size(ui, s->w, s->h);
	if (ui->charge_mode) {
		render_charge(ui, s);
		render_power_overlays(ui, s);
		return;
	}
	if (!ui->loaded) {
		render_loading(ui, s);
		return;
	}
	top = ui_top(ui);
	if (!top)
		return;
	if (top->ops->opaque) {
		top->ops->render(ui, top, s);
	} else {
		/* modal: the opaque screen below, dimmed, cached */
		int base = ui->nstack - 1;

		while (base > 0 && !ui->stack[base]->ops->opaque)
			base--;
		if (!ui->snapshot || ui->snapshot_depth != base) {
			struct gfx_surface ss;

			gfx_image_free(ui->snapshot);
			ui->snapshot = gfx_image_new(ui->w, ui->h);
			if (ui->snapshot) {
				gfx_surface_from_image(&ss, ui->snapshot);
				/* no help bar under a modal: the modal draws its own */
				ui->in_snapshot = true;
				ui->stack[base]->ops->render(ui, ui->stack[base], &ss);
				ui->in_snapshot = false;
				gfx_fill(&ss, 0, 0, ui->w, ui->h, ui->ms.dim);
				ui->snapshot->flags |= GFX_IMG_OPAQUE;
				ui->snapshot_depth = base;
			}
		}
		if (ui->snapshot) {
			gfx_blit(s, ui->snapshot, 0, 0, 255);
		} else {
			/* no snapshot (gfx_image_new refused the size): the screen
			 * below drawn straight, dimmed */
			ui->snapshot_depth = -1;
			ui->in_snapshot = true;
			ui->stack[base]->ops->render(ui, ui->stack[base], s);
			ui->in_snapshot = false;
			gfx_fill(s, 0, 0, s->w, s->h, ui->ms.dim);
		}
		top->ops->render(ui, top, s);
	}
	render_overlays(ui, s);
	render_power_overlays(ui, s);
	ui->flush_at = ui->now + FLUSH_IDLE_MS;
	if (!ui->first_frame_done) {
		struct img_stats is;
		struct ui_io io;

		ui->first_frame_done = true;
		ui->timings.first_frame_us = ui_now_us() - t0;
		img_get_stats(&is);
		ui_io_sample(ui, &io);
		/* where the first menu frame goes: themes, image decoding vs the
		 * .rpx cache (hits = no decode), and the SD card */
		LOGI("ui: first frame in %lld us (theme parsing %lld us, images: %d decoded in %lld us, "
		     "%d from the cache in %lld us, %d shared; backdrops: %d from the cache, %d composited, "
		     "%lld us; io %lld reads, %lld KiB)",
		     (long long)ui->timings.first_frame_us, (long long)(ui->timings.theme_us - theme0),
		     is.decoded - is0.decoded, (long long)(is.decode_us - is0.decode_us),
		     is.disk_hits - is0.disk_hits, (long long)(is.disk_us - is0.disk_us),
		     is.mem_hits - is0.mem_hits, is.bd_hits - is0.bd_hits, is.bd_built - is0.bd_built,
		     (long long)(is.bd_us - is0.bd_us), (long long)(io.reads - io0.reads),
		     (long long)(io.sectors - io0.sectors) / 2);
	}
}

void ui_set_now(struct ui *ui, int64_t now_ms)
{
	if (now_ms > ui->now)
		ui->now = now_ms;
}

void ui_debug_screen(struct ui *ui, char *buf, size_t n)
{
	struct screen *s = ui_top(ui);
	size_t l;

	if (!s)
		snprintf(buf, n, "none");
	else if (s->kind == SCR_SYSVIEW)
		snprintf(buf, n, "carousel");
	else if (s->kind == SCR_GLVIEW) {
		size_t k;

		snprintf(buf, n, "list:%s", glview_system(s)->name);
		/* " search=...(rows) game=<under the cursor> letter=<jump>" */
		k = strlen(buf);
		if (k < n)
			glview_describe(s, buf + k, n - k);
	} else if (s->ops->describe)
		s->ops->describe(ui, s, buf, n);
	else
		snprintf(buf, n, "screen");
	l = strlen(buf);
	if ((ui->toast_until || ui->toast_pending_ms) && ui->toast[0] && l + 8 < n)
		snprintf(buf + l, n - l, " toast=%s", ui->toast);
}

bool ui_is_loaded(const struct ui *ui)
{
	return ui->loaded;
}

void ui_pause_background(struct ui *ui, bool pause)
{
	loader_pause(ui, pause);
	prefetch_pause(ui, pause);
}

void ui_set_background_polling(struct ui *ui, bool on)
{
	prefetch_set_polling(ui, on);
}

bool ui_background_wait(struct ui *ui, bool all, int max_ms)
{
	bool r = prefetch_wait(ui, all ? PF_BG : PF_URGENT, max_ms);

	/* what the jobs asked for next (a window, the rest) */
	if (all && ui->nstack) {
		for (int k = 0; k < 64 && r; k++) {
			sysview_prefetch(ui, ui->stack[0]);
			if (!prefetch_busy(ui))
				break;
			r = prefetch_wait(ui, PF_BG, max_ms);
		}
	}
	return r;
}

void ui_debug_assets(struct ui *ui, char *buf, size_t n)
{
	size_t l;
	int jobs, queued;
	int64_t us;

	if (!ui->nstack) {
		snprintf(buf, n, "none");
		return;
	}
	sysview_describe_assets(ui, ui->stack[0], buf, n);
	l = strlen(buf);
	if (l < n)
		glview_describe_prebuilt(ui, buf + l, n - l);
	l = strlen(buf);
	prefetch_stats(ui, &jobs, &us, &queued);
	if (l < n)
		snprintf(buf + l, n - l, " worker=%s", queued ? "busy" : "idle");
}

void ui_select_theme(struct ui *ui, const char *name)
{
	ui_set_theme(ui, name);
}

/* ------------------------------------------------------------------ power */
static const char *const g_power_keys[] = {
	"sleep_timeout_min", "sleep_wake", "idle_dim_min", "idle_off_min", "idle_poweroff_min", "battery_gauge",
	"timezone",
};

/* settings.ini -> power module, at start (the module has its defaults). */
static void apply_power_settings(struct ui *ui)
{
	if (!ui->cfg.power || !ui->cfg.power->set_setting)
		return;
	for (size_t i = 0; i < ARRAY_SIZE(g_power_keys); i++) {
		const char *v = settings_get(ui->settings, g_power_keys[i], NULL);

		if (v && *v && ui->cfg.power->set_setting(g_power_keys[i], v) < 0)
			LOGW("ui: power setting %s=%s refused", g_power_keys[i], v);
	}
}

const struct power_status *ui_battery(struct ui *ui)
{
	struct hw_battery b;
	struct power_status *st = &ui->bat_fallback;

	if (ui->cfg.power && ui->cfg.power->get_status)
		return ui->cfg.power->get_status();
	/* no power module (preview): sysfs, at most every 5 s */
	if (ui->bat_read_at && ui_now_ms() - ui->bat_read_at < 5000)
		return st;
	ui->bat_read_at = ui_now_ms();
	hw_battery(ui->cfg.power_supply_dir, &b);
	memset(st, 0, sizeof(*st));
	st->valid = b.present;
	st->battery_present = b.present;
	st->charger_online = b.ac;
	st->percent = b.percent;
	st->percent_axp = b.percent;
	st->percent_voltage = -1;
	st->minutes_left = -1;
	st->state = b.charging ? BATT_CHARGING : !strcmp(b.status, "Full") ? BATT_FULL :
		    b.ac ? BATT_NOT_CHARGING : BATT_DISCHARGING;
	st->level = b.percent >= 0 && b.percent <= 7 && !b.ac ? POWER_LEVEL_VERY_LOW :
		    b.percent >= 0 && b.percent <= 15 && !b.ac ? POWER_LEVEL_LOW : POWER_LEVEL_OK;
	st->temp_mc = POWER_TEMP_UNKNOWN;
	return st;
}

void ui_power_event(struct ui *ui, enum ui_power_event ev)
{
	const struct power_status *st = ui_battery(ui);

	switch (ev) {
	case UI_PWR_STATUS:
		break;
	case UI_PWR_LOW:
		ui_toastf(ui, _("Battery low (%d %%)"), st ? st->percent : 15);
		break;
	case UI_PWR_VERY_LOW:
		ui->battery_banner = true;
		break;
	case UI_PWR_OK:
		ui->battery_banner = false;
		break;
	case UI_PWR_CRITICAL:
		strlcpy_(ui->big_msg, _("Battery empty, saving..."), sizeof(ui->big_msg));
		break;
	case UI_PWR_SHUTDOWN:
	case UI_PWR_REBOOT:
		if (!ui->big_msg[0])
			strlcpy_(ui->big_msg, ev == UI_PWR_REBOOT ? _("Restarting...") : _("Powering off..."),
				 sizeof(ui->big_msg));
		settings_save(ui->settings);
		gamedb_save(ui->db);
		loader_save_snapshot(ui);
		img_cache_flush();
		break;
	case UI_PWR_HOT:
		/* TRANSLATORS: the console is hot and runs slower to cool down */
		ui_toastf(ui, "%s", _("Hot, slowing down"));
		break;
	case UI_PWR_COOL:
		break;
	case UI_PWR_IDLE_WARN:
		/* TRANSLATORS: shown 10 s before the automatic power-off (no input
		 * for the time set in Settings > Power); any button cancels it */
		strlcpy_(ui->toast, _("Powering off in 10 s — press any button to cancel"), sizeof(ui->toast));
		ui->toast_sev = UI_SEV_WARNING;
		ui->toast_pending_ms = 10000;
		ui->toast_until = 0;
		LOGI("ui: toast (warning): %s", ui->toast);
		break;
	case UI_PWR_IDLE_CANCEL:
		if (!strcmp(ui->toast, _("Powering off in 10 s — press any button to cancel"))) {
			ui->toast[0] = 0;
			ui->toast_pending_ms = 0;
			ui->toast_until = 0;
		}
		/* an idle power-off cancelled after "Powering off..." (the game
		 * could not save): the menu is back */
		ui->big_msg[0] = 0;
		break;
	}
	ui->dirty = true;
}

void ui_set_charge_mode(struct ui *ui, bool on)
{
	ui->charge_mode = on;
	ui->dirty = true;
}

static void render_charge(struct ui *ui, struct gfx_surface *s)
{
	const struct power_status *st = ui_battery(ui);
	struct font *fb = font_get(font_default_path(true), ui_font_px(ui, 0.12f));
	struct font *f = font_get(NULL, ui_font_px(ui, 0.045f));
	int W = ui->w, H = ui->h;
	int bw = W * 30 / 100, bh = H * 22 / 100, x = (W - bw) / 2, y = H * 22 / 100;
	int pct = st && st->percent >= 0 ? CLAMP(st->percent, 0, 100) : -1;
	char buf[96];
	/* TRANSLATORS: charge screen (the console is off, on the charger) */
	const char *state = _("Charging");
	gfx_color c = 0xff7ed957u;

	gfx_fill(s, 0, 0, W, H, 0xff000000u);
	if (st && st->state == BATT_FULL) {
		state = _("Fully charged");
	} else if (st && !st->charger_online) {
		state = _("Charger unplugged");
		c = 0xffe0453au;
	} else if (st && st->state == BATT_NOT_CHARGING) {
		state = _("Not charging");
		c = 0xffd9a441u;
	}
	gfx_stroke_round(s, x, y, bw, bh, bh / 8, MAX(3, bh / 20), 0xffd0d0d0u);
	gfx_fill_round(s, x + bw, y + bh / 3, bw / 18, bh / 3, 3, 0xffd0d0d0u);
	if (pct > 0) {
		int pad = MAX(6, bh / 10);

		gfx_fill_round(s, x + pad, y + pad, (bw - 2 * pad) * pct / 100, bh - 2 * pad,
			       bh / 16, c);
	}
	if (pct >= 0)
		/* TRANSLATORS: the big battery percentage of the charge screen ("80%") */
		snprintf(buf, sizeof(buf), C_("battery", "%d%%"), pct);
	else
		snprintf(buf, sizeof(buf), "--");
	draw_text_box(ui, s, fb, buf, 0, y + bh + H / 30, W, H / 6, AL_CENTER, 0xfff0f0f0u);
	draw_text_box(ui, s, f, state, 0, y + bh + H / 5, W, H / 14, AL_CENTER, 0xffa0a0a0u);
	if (st && st->minutes_left > 0 && st->state == BATT_CHARGING) {
		/* TRANSLATORS: charge screen: time until the battery is full (hours, minutes) */
		snprintf(buf, sizeof(buf), _("Full in %d h %02d min"), st->minutes_left / 60,
			 st->minutes_left % 60);
		draw_text_box(ui, s, f, buf, 0, y + bh + H / 5 + H / 14, W, H / 14, AL_CENTER,
			      0xff808080u);
	}
	draw_text_box(ui, s, font_get(NULL, ui_font_px(ui, 0.035f)),
		      _("Press the power button to start"), 0, H - H / 9, W, H / 14, AL_CENTER,
		      0xff606060u);
}

static void render_power_overlays(struct ui *ui, struct gfx_surface *s)
{
	if (ui->battery_banner) {
		struct font *f = font_get(ui->ms.font_path, ui_font_px(ui, 0.038f));
		int h = font_height(f) * 2, w = ui->w * 8 / 10;

		gfx_fill_round(s, (ui->w - w) / 2, ui->h / 60, w, h, h / 2, 0xf0b3261eu);
		draw_text_box(ui, s, f, _("Battery very low: plug in the charger"), (ui->w - w) / 2,
			      ui->h / 60, w, h, AL_CENTER, 0xffffffffu);
	}
	if (ui->big_msg[0]) {
		struct font *f = font_get(font_default_path(true), ui_font_px(ui, 0.06f));

		gfx_fill(s, 0, 0, ui->w, ui->h, 0xe0000000u);
		draw_text_box(ui, s, f, ui->big_msg, 0, ui->h * 2 / 5, ui->w, ui->h / 5, AL_CENTER,
			      0xfff0f0f0u);
	}
}
