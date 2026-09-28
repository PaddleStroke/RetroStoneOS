/*
 * rsos-uipreview - runs the RetroStoneOS UI on a development host, without
 * DRM or input devices: scripted buttons in, PNG screenshots out.
 *
 *   rsos-uipreview --root ~/rsos/ui-test --theme rsos-dark \
 *       --keys "right right a down shot:list.png b start shot:menu.png" \
 *       --out final.png
 *
 * Script tokens (space separated):
 *   up down left right a b x y l1 r1 l2 r2 select start c z   press + release
 *   hold:<button>:<ms>     press, keep it down (key repeat runs), release
 *   combo:<b1>+<b2>:<ms>   hold two buttons together (button test exit)
 *   wait:<ms>              let virtual time pass
 *   shot:<file.png>        screenshot now
 *   size:<W>x<H>           switch the logical size (LCD <-> HDMI relayout)
 *   theme:<name>           switch theme
 *   hdmi / lcd             output type (brightness is LCD-only)
 *   usb / unplug           a USB stick appears / is pulled (with --fake-transfer)
 *   usbtrees:N, usbfs:NAME  libraries on the fake stick (3), its filesystem (exfat)
 *   idle:<ms>              time passes WITHOUT ui_update(), like the device's
 *                          poll() sleep before an event (then the next token)
 *   expect:<text>          the top screen (ui_debug_screen, _ = space) must
 *                          contain text, else the run fails
 *   charge / nocharge      charge mode screen on/off
 *   toast:<sev>:<text>     ui_toast() (sev: info, warning, error; _ = space)
 *   lang:<code>            switch the language live (Settings > Language), saved
 * Time is virtual: animations run to completion between tokens.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../gfx/gfx.h"
#include "../gfx/image.h"
#include "../ui/ui.h"
#include "../ui/util.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ZLIB_COMPRESS_LEVEL 9
#include "../../third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop

struct preview {
	struct ui *ui;
	uint32_t *fb;
	int w, h;
	struct gfx_surface s;
	int64_t now;
	int frames;
	int64_t render_us;
	int64_t render_max_us;
	int png_bits;          /* 8 = exact; 5..7: posterize (smaller doc PNGs) */
};

static void usage(void)
{
	fprintf(stderr,
		"usage: rsos-uipreview [--root DIR] [--roms DIR] [--data DIR] [--cache DIR|none]\n"
		"                      [--themes DIR] [--themes-user DIR] [--res DIR] [--cores DIR]\n"
		"                      [--theme NAME] [--size WxH] [--keys SCRIPT] [--out FILE.png]\n"
		"                      [--timing] [--bench] [--quiet]\n"
		"                      [--lang CODE] [--locale DIR] [--language-prompt]\n"
		"  --lang CODE          language (fr, pt_BR...; default: settings.ini language=)\n"
		"  --locale DIR         compiled catalogs <code>.cat (default /usr/share/rsos/locale)\n"
		"  --language-prompt    the first-boot language picker when settings.ini has none\n");
}

static void alloc_fb(struct preview *p, int w, int h)
{
	free(p->fb);
	p->w = w;
	p->h = h;
	p->fb = xcalloc((size_t)w * h, 4);
	gfx_surface_init(&p->s, p->fb, w, h, w);
	ui_set_size(p->ui, w, h);
}

static void render(struct preview *p)
{
	int64_t t0 = ui_now_us(), dt;

	ui_render(p->ui, &p->s);
	dt = ui_now_us() - t0;
	p->frames++;
	p->render_us += dt;
	if (dt > p->render_max_us)
		p->render_max_us = dt;
}

/* Advances virtual time until the UI is idle (or max_ms passed). */
static void settle(struct preview *p, int max_ms)
{
	int64_t end = p->now + max_ms;

	for (;;) {
		int t;

		if (ui_update(p->ui, p->now))
			render(p);
		t = ui_timeout_ms(p->ui, p->now);
		if (t < 0 || p->now >= end)
			break;
		p->now += t < 16 ? 16 : t;
		if (p->now > end)
			p->now = end;
	}
}

static int shot(struct preview *p, const char *file)
{
	unsigned char *rgb = xmalloc((size_t)p->w * p->h * 3);
	int ok;

	/* make sure the frame is current */
	ui_update(p->ui, p->now);
	render(p);
	for (int i = 0; i < p->w * p->h; i++) {
		rgb[3 * i] = (unsigned char)(p->fb[i] >> 16);
		rgb[3 * i + 1] = (unsigned char)(p->fb[i] >> 8);
		rgb[3 * i + 2] = (unsigned char)p->fb[i];
	}
	if (p->png_bits > 0 && p->png_bits < 8) {
		int sh = 8 - p->png_bits;

		for (int i = 0; i < p->w * p->h * 3; i++) {
			int v = ((rgb[i] + (1 << (sh - 1))) >> sh) << sh;

			rgb[i] = (unsigned char)(v > 255 ? 255 : v);
		}
	}
	ok = stbi_write_png(file, p->w, p->h, 3, rgb, p->w * 3);
	free(rgb);
	if (!ok) {
		fprintf(stderr, "uipreview: cannot write %s\n", file);
		return -1;
	}
	printf("uipreview: wrote %s (%dx%d)\n", file, p->w, p->h);
	return 0;
}

static int btn_from_name(const char *s)
{
	static const struct { const char *n; int b; } map[] = {
		{ "up", IN_UP }, { "down", IN_DOWN }, { "left", IN_LEFT }, { "right", IN_RIGHT },
		{ "a", IN_A }, { "b", IN_B }, { "x", IN_X }, { "y", IN_Y },
		{ "l", IN_L }, { "r", IN_R }, { "l1", IN_L }, { "r1", IN_R },
		{ "l2", IN_L2 }, { "r2", IN_R2 }, { "select", IN_SELECT }, { "start", IN_START },
		{ "c", IN_L3 }, { "z", IN_R3 }, { "l3", IN_L3 }, { "r3", IN_R3 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(map); i++)
		if (!strcmp(s, map[i].n))
			return map[i].b;
	return -1;
}

static void press(struct preview *p, int b, int hold_ms)
{
	ui_button(p->ui, (enum input_btn)b, IN_NAV_PRESS);
	settle(p, 30);
	if (hold_ms > 0) {
		/* emulate the input layer's key repeat: 400 ms, then every 70 ms */
		int t = 400;

		p->now += 400;
		settle(p, 0);
		while (t < hold_ms) {
			ui_button(p->ui, (enum input_btn)b, IN_NAV_REPEAT);
			p->now += 70;
			t += 70;
			settle(p, 0);
		}
	}
	ui_button(p->ui, (enum input_btn)b, IN_NAV_RELEASE);
	settle(p, 2000);
}

static int fk_usb_drives(struct transfer_usb_drive *out, int max);
static int g_fake_ntrees;          /* (defined with the fake transfer module) */
static char g_fake_fs[16];

static int run_script(struct preview *p, const char *script)
{
	char *buf = xstrdup(script), *save = NULL, *tok;
	int r = 0;

	for (tok = strtok_r(buf, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
		int b;

		if (!strncmp(tok, "shot:", 5)) {
			r |= shot(p, tok + 5);
		} else if (!strncmp(tok, "wait:", 5)) {
			int64_t end = p->now + atoi(tok + 5);

			settle(p, atoi(tok + 5));
			if (p->now < end)
				p->now = end;
			settle(p, 16);
		} else if (!strncmp(tok, "size:", 5)) {
			int w, h;

			if (sscanf(tok + 5, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				alloc_fb(p, w, h);
				settle(p, 500);
			}
		} else if (!strncmp(tok, "lang:", 5)) {
			ui_set_language(p->ui, tok + 5, true);
			settle(p, 500);
		} else if (!strncmp(tok, "theme:", 6)) {
			ui_select_theme(p->ui, tok + 6);
			settle(p, 500);
		} else if (!strncmp(tok, "toast:", 6)) {
			/* toast:<info|warning|error>:<text with _ for spaces> */
			char sev[16], text[200];

			if (sscanf(tok + 6, "%15[^:]:%199s", sev, text) == 2) {
				for (char *c = text; *c; c++)
					if (*c == '_')
						*c = ' ';
				ui_toast(p->ui, text, !strcmp(sev, "error") ? UI_SEV_ERROR :
					 !strcmp(sev, "warning") ? UI_SEV_WARNING : UI_SEV_INFO);
				settle(p, 50);
			}
		} else if (!strcmp(tok, "usb") || !strcmp(tok, "unplug")) {
			/* a USB stick was mounted / pulled (needs --fake-transfer) */
			struct transfer_usb_event ev;

			memset(&ev, 0, sizeof(ev));
			ev.type = !strcmp(tok, "usb") ? TRANSFER_USB_MOUNTED : TRANSFER_USB_REMOVED;
			fk_usb_drives(&ev.drive, 1);
			ui_usb_event(p->ui, &ev);
			settle(p, 100);
		} else if (!strncmp(tok, "idle:", 5)) {
			p->now += atoi(tok + 5);
		} else if (!strncmp(tok, "usbtrees:", 9)) {
			g_fake_ntrees = atoi(tok + 9);
		} else if (!strncmp(tok, "usbfs:", 6)) {
			snprintf(g_fake_fs, sizeof(g_fake_fs), "%s", tok + 6);
		} else if (!strncmp(tok, "expect:", 7)) {
			char want[200], got[600];

			snprintf(want, sizeof(want), "%s", tok + 7);
			for (char *c = want; *c; c++)
				if (*c == '_')
					*c = ' ';
			ui_debug_screen(p->ui, got, sizeof(got));
			if (strstr(got, want)) {
				printf("uipreview: expect ok: %s\n", want);
			} else {
				fprintf(stderr, "uipreview: EXPECT FAILED: want \"%s\", screen \"%s\"\n", want, got);
				r = -1;
			}
		} else if (!strcmp(tok, "charge") || !strcmp(tok, "nocharge")) {
			ui_set_charge_mode(p->ui, !strcmp(tok, "charge"));
			settle(p, 100);
		} else if (!strcmp(tok, "hdmi") || !strcmp(tok, "lcd")) {
			ui_set_output(p->ui, !strcmp(tok, "hdmi"));
			settle(p, 100);
		} else if (!strncmp(tok, "combo:", 6)) {
			/* combo:<b1>+<b2>:<ms>: both held together */
			char n1[16], n2[16];
			int ms = 0, b1, b2;

			if (sscanf(tok + 6, "%15[^+]+%15[^:]:%d", n1, n2, &ms) == 3 &&
			    (b1 = btn_from_name(n1)) >= 0 && (b2 = btn_from_name(n2)) >= 0) {
				ui_button(p->ui, (enum input_btn)b1, IN_NAV_PRESS);
				ui_button(p->ui, (enum input_btn)b2, IN_NAV_PRESS);
				for (int t = 0; t < ms; t += 50) {
					p->now += 50;
					settle(p, 0);
				}
				ui_button(p->ui, (enum input_btn)b2, IN_NAV_RELEASE);
				ui_button(p->ui, (enum input_btn)b1, IN_NAV_RELEASE);
				settle(p, 2000);
			} else {
				fprintf(stderr, "uipreview: bad token %s\n", tok);
			}
		} else if (!strncmp(tok, "tap:", 4) && (b = btn_from_name(tok + 4)) >= 0) {
			/* a press and release with no settling after it: what shows for
			 * a moment (the jump-to-letter overlay) is still on screen */
			ui_button(p->ui, (enum input_btn)b, IN_NAV_PRESS);
			ui_button(p->ui, (enum input_btn)b, IN_NAV_RELEASE);
			p->now += 16;
			if (ui_update(p->ui, p->now))
				render(p);
		} else if (!strncmp(tok, "hold:", 5)) {
			char name[16];
			int ms = 0;

			if (sscanf(tok + 5, "%15[^:]:%d", name, &ms) == 2 && (b = btn_from_name(name)) >= 0)
				press(p, b, ms);
			else
				fprintf(stderr, "uipreview: bad token %s\n", tok);
		} else if ((b = btn_from_name(tok)) >= 0) {
			press(p, b, 0);
		} else {
			fprintf(stderr, "uipreview: unknown token %s\n", tok);
			r = -1;
		}
	}
	free(buf);
	return r;
}

/* ------------------------------------------------ fake transfer module */
/* --fake-transfer: a canned USB drive, library search, import plan and
 * progress (with four "already on the console" questions), export games and
 * saves backup, and web share status, to render and test the transfer
 * screens without hardware. Tokens: usbtrees:N (libraries found, default
 * 3), usbfs:NAME (the drive's filesystem, default exfat). Answers and
 * remounts are printed (ANSWER / REMOUNT lines) for the checks. */
struct transfer_backup {
	struct transfer_backup_info info;
};

static struct transfer_plan g_fake_plan;
static struct transfer_backup g_fake_bk;
static int g_fake_scan_polls, g_fake_import_polls, g_fake_tree_polls, g_fake_bk_polls, g_fake_bks_polls;
static bool g_fake_importing, g_fake_share, g_fake_backing;
static int g_fake_ntrees = 3;
static char g_fake_fs[16] = "exfat";
static int g_fake_q, g_fake_answered;          /* the question asked, the last answered */
static struct transfer_backup_opts g_fake_bk_opts;

static int fk_usb_drives(struct transfer_usb_drive *out, int max)
{
	if (max < 1)
		return 0;
	memset(out, 0, sizeof(*out));
	strcpy(out->dev, "sda1");
	/* RSOS_FAKE_USB_MP: a real folder as the stick (the update tests put a .rsu there) */
	snprintf(out->mountpoint, sizeof(out->mountpoint), "%s",
		 getenv("RSOS_FAKE_USB_MP") ? getenv("RSOS_FAKE_USB_MP") : "/media/usb0");
	snprintf(out->fstype, sizeof(out->fstype), "%s", g_fake_fs);
	strcpy(out->label, "GAMES");
	strcpy(out->vendor, "SanDisk Cruzer");
	out->size_bytes = 32ull << 30;
	return 1;
}

static int fk_usb_eject(const char *mp)
{
	printf("EJECT %s\n", mp);
	return 0;
}

static int fk_usb_remount(const char *mp, bool rw)
{
	printf("REMOUNT %s %s\n", mp, rw ? "rw" : "ro");
	return 0;
}

static int fk_trees_start(const char *stick)
{
	(void)stick;
	g_fake_tree_polls = 0;
	return 0;
}

static int fk_trees_poll(struct transfer_tree *out, int max, int *n)
{
	static const struct {
		const char *rel, *systems;
		bool backup;
		int nsys, games, files;
		uint64_t bytes;
	} t[] = {
		{ "", "snes, megadrive, psx", false, 3, 20, 26, 1344ull << 20 },
		{ "RetroPie", "gba, nes", false, 2, 45, 47, 310ull << 20 },
		{ "Backups/RetroStone2", "snes, gba, nes, psx...", true, 11, 58, 131, 5222ull << 20 },
	};

	if (++g_fake_tree_polls < 3)
		return 0;
	*n = 0;
	for (int i = 0; i < g_fake_ntrees && i < 3 && i < max; i++) {
		struct transfer_tree *o = &out[(*n)++];

		memset(o, 0, sizeof(*o));
		snprintf(o->path, sizeof(o->path), "/media/usb0%s%s", t[i].rel[0] ? "/" : "", t[i].rel);
		snprintf(o->rel, sizeof(o->rel), "%s", t[i].rel);
		snprintf(o->systems, sizeof(o->systems), "%s", t[i].systems);
		o->backup = t[i].backup;
		o->nsys = t[i].nsys;
		o->games = t[i].games;
		o->files = t[i].files;
		o->bytes = t[i].bytes;
	}
	return 1;
}

static int fk_plan_start_trees(const char *const *roots, int n, const char *dst)
{
	struct transfer_plan *p = &g_fake_plan;

	printf("PLAN %d root%s: %s%s\n", n, n == 1 ? "" : "s", roots[0], n > 1 ? " ..." : "");
	memset(p, 0, sizeof(*p));
	snprintf(p->src_root, sizeof(p->src_root), "%s", roots[0]);
	snprintf(p->dst_root, sizeof(p->dst_root), "%s", dst);
	strcpy(p->layout, "roms");
	p->nsys = 3;
	strcpy(p->sys[0].id, "snes");
	p->sys[0].files = 15;
	p->sys[0].to_copy = 12;
	p->sys[0].bytes = 38ull << 20;
	p->sys[0].saves = 4;
	strcpy(p->sys[1].id, "megadrive");
	p->sys[1].files = 3;
	p->sys[1].to_copy = 3;
	p->sys[1].bytes = 6ull << 20;
	strcpy(p->sys[2].id, "psx");
	p->sys[2].files = 2;
	p->sys[2].to_copy = 2;
	p->sys[2].bytes = 1300ull << 20;
	p->rom_files = 20;
	p->rom_copy = 17;
	p->rom_bytes = 1344ull << 20;
	p->bios_files = 2;
	p->bios_copy = 1;
	p->bios_bytes = 512 << 10;
	p->save_files = 4;
	p->save_copy = 3;
	p->save_conflicts = 1;
	p->identical = 3;
	p->replace = 3;
	p->bytes_to_copy = p->rom_bytes + p->bios_bytes + (64 << 10);
	p->dst_free = 20ull << 30;
	p->fits = true;
	strcpy(p->unknown[0], "dreamcast");
	p->unknown_files[0] = 132;
	p->nunknown = 1;
	g_fake_scan_polls = 0;
	return 0;
}

static int fk_plan_poll(struct transfer_plan **out)
{
	if (++g_fake_scan_polls < 3)
		return 0;
	*out = &g_fake_plan;
	return 1;
}

static void fk_plan_free(struct transfer_plan *p)
{
	(void)p;
}

static void fk_import_defaults(struct transfer_import_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->roms = o->bios = o->saves = o->themes = o->screenshots = true;
}

static int fk_import_start(struct transfer_plan *p, const struct transfer_import_opts *o)
{
	(void)p;
	(void)o;
	g_fake_importing = true;
	g_fake_import_polls = 0;
	g_fake_q = g_fake_answered = 0;
	return 0;
}

/* Four differing files, asked at polls 3, 6, 9, 12. */
static const struct {
	const char *path;
	bool save;
	uint64_t src, dst;
} g_fake_dups[4] = {
	{ "roms/snes/Aurora Quest (USA).sfc", false, 2u << 20, 1u << 20 },
	{ "roms/snes/Crystal Maze (Europe).sfc", false, 1u << 20, 1u << 20 },
	{ "saves/snes/Aurora Quest (USA).srm", true, 8192, 8192 },
	{ "roms/psx/Epic Saga (USA) (Disc 1).chd", false, 600u << 20, 590u << 20 },
};

static enum transfer_state fk_import_status(struct transfer_progress *out)
{
	int k = g_fake_import_polls;

	memset(out, 0, sizeof(*out));
	if (!g_fake_importing)
		return out->state = TRANSFER_IDLE;
	if (g_fake_q > g_fake_answered) {
		/* waiting for the answer */
		const int i = g_fake_q - 1;

		out->asking = true;
		out->question.seq = g_fake_q;
		out->question.kind = g_fake_dups[i].save ? TRANSFER_SAVE : TRANSFER_ROM;
		out->question.save = g_fake_dups[i].save;
		snprintf(out->question.path, sizeof(out->question.path), "%s", g_fake_dups[i].path);
		out->question.src_size = g_fake_dups[i].src;
		out->question.dst_size = g_fake_dups[i].dst;
		out->question.src_mtime = 1790000000;
		out->question.dst_mtime = 1760000000;
	} else {
		g_fake_import_polls++;
		if (k > 0 && k % 3 == 0 && k / 3 <= 4 && g_fake_q < k / 3)
			g_fake_q = k / 3;
	}
	out->files_total = 21;
	out->bytes_total = g_fake_plan.bytes_to_copy;
	out->files_done = MIN(21, k);
	out->bytes_done = out->bytes_total * (uint64_t)MIN(k, 20) / 20;
	snprintf(out->current, sizeof(out->current), "roms/psx/Epic Saga (USA) (Disc %d).chd", 1 + k % 2);
	out->rate_kbs = 9400;
	out->eta_s = MAX(0, (20 - k) * 7);
	out->copied = MIN(k, 17);
	out->replaced = 2;
	out->identical = 3;
	out->kept = 2;
	out->skipped = 5;
	out->state = k >= 20 && g_fake_q == g_fake_answered ? TRANSFER_DONE : TRANSFER_RUNNING;
	return out->state;
}

static void fk_import_answer(enum transfer_answer a)
{
	static const char *const names[] = { "skip", "replace", "skip-all", "replace-all" };

	printf("ANSWER %d %s\n", g_fake_q, names[a & 3]);
	g_fake_answered = g_fake_q;
}

static void fk_import_cancel(void)
{
	g_fake_import_polls = 20;
	g_fake_answered = g_fake_q = 4;
}

static int fk_import_finish(char out[][TRANSFER_SYSID_MAX], int max)
{
	g_fake_importing = false;
	if (max < 1)
		return 0;
	strcpy(out[0], "snes");
	return 1;
}

static int fk_backup_scan_start(const char *data, const char *stick, const char *fs,
				enum transfer_backup_mode mode)
{
	struct transfer_backup_info *in = &g_fake_bk.info;

	memset(&g_fake_bk, 0, sizeof(g_fake_bk));
	in->mode = mode;
	snprintf(in->data_root, sizeof(in->data_root), "%s", data);
	snprintf(in->stick_root, sizeof(in->stick_root), "%s", stick);
	strcpy(in->folder, TRANSFER_BACKUP_DIR);
	in->exists = true;
	in->fat32 = !strcmp(fs, "vfat");
	in->free_bytes = 9ull << 30;
	in->cluster = 32768;
	in->files[TRANSFER_BK_ROMS] = 58;
	in->bytes[TRANSFER_BK_ROMS] = 5222ull << 20;
	in->files[TRANSFER_BK_BIOS] = 6;
	in->bytes[TRANSFER_BK_BIOS] = 20ull << 20;
	in->files[TRANSFER_BK_SAVES] = 38;
	in->bytes[TRANSFER_BK_SAVES] = 11ull << 20;
	in->files[TRANSFER_BK_CONFIG] = 5;
	if (in->fat32 && mode == TRANSFER_EXPORT_GAMES) {
		in->nbig = 2;
		strcpy(in->big[0], "roms/psx/Final Quest (USA) (Disc 1).bin");
		strcpy(in->big[1], "roms/psx/Final Quest (USA) (Disc 2).bin");
	}
	g_fake_bks_polls = 0;
	return 0;
}

static int fk_backup_scan_poll(struct transfer_backup **out)
{
	if (++g_fake_bks_polls < 3)
		return 0;
	*out = &g_fake_bk;
	return 1;
}

static const struct transfer_backup_info *fk_backup_info(const struct transfer_backup *b)
{
	return &b->info;
}

static void fk_backup_totals(const struct transfer_backup *b, const struct transfer_backup_opts *o,
			     struct transfer_backup_totals *t)
{
	memset(t, 0, sizeof(*t));
	if (b->info.mode == TRANSFER_EXPORT_GAMES) {
		t->files = 12 + (o->bios ? 1 : 0);
		t->bytes = (812ull << 20) + (o->bios ? 4u << 20 : 0);
		t->unchanged = 44 + (o->bios ? 5 : 0);
		t->replace = 1;
		t->too_big = b->info.nbig;
	} else if (o->game_rom[0]) {
		t->files = 2 + (o->settings ? 1 : 0);
		t->bytes = 96u << 10;
		t->unchanged = 2;
	} else {
		t->files = 7 + (o->settings ? 3 : 0);
		t->bytes = 3u << 20;
		t->unchanged = 31 + (o->settings ? 2 : 0);
		t->replace = 4;
	}
	t->need = t->bytes + (2u << 20);
	t->fits = t->need <= b->info.free_bytes;
}

static void fk_backup_free(struct transfer_backup *b)
{
	(void)b;
}

static int fk_backup_start(struct transfer_backup *b, const struct transfer_backup_opts *o)
{
	(void)b;
	g_fake_bk_opts = *o;
	g_fake_backing = true;
	g_fake_bk_polls = 0;
	printf("BACKUP %s%s%s%s\n", o->mode == TRANSFER_EXPORT_GAMES ? "export" : "saves",
	       o->bios && o->mode == TRANSFER_EXPORT_GAMES ? " bios" : "",
	       o->settings && o->mode == TRANSFER_BACKUP_SAVES ? " settings" : "",
	       o->game_rom[0] ? " last-played" : "");
	return 0;
}

static enum transfer_state fk_backup_status(struct transfer_progress *out)
{
	struct transfer_backup_totals t;
	int k = g_fake_bk_polls;

	memset(out, 0, sizeof(*out));
	if (!g_fake_backing)
		return out->state = TRANSFER_IDLE;
	g_fake_bk_polls++;
	fk_backup_totals(&g_fake_bk, &g_fake_bk_opts, &t);
	out->files_total = t.files;
	out->bytes_total = t.bytes;
	out->files_done = t.files * MIN(k, 30) / 30;
	out->bytes_done = t.bytes * (uint64_t)MIN(k, 30) / 30;
	out->copied = out->files_done;
	out->identical = t.unchanged;
	out->skipped = t.too_big;
	strcpy(out->folder, TRANSFER_BACKUP_DIR);
	snprintf(out->current, sizeof(out->current), "%s",
		 g_fake_bk.info.mode == TRANSFER_EXPORT_GAMES ? "roms/snes/Aurora Quest (USA).sfc" :
		 "saves/snes/Aurora Quest (USA).srm");
	out->rate_kbs = 11200;
	out->eta_s = MAX(0, (30 - k) * 5);
	if (g_fake_bk_opts.game_rom[0]) {
		/* the host's names: the ROM's stem */
		const char *b = strrchr(g_fake_bk_opts.game_rom, '/'), *dot;
		char stem[80];

		b = b ? b + 1 : g_fake_bk_opts.game_rom;
		dot = strrchr(b, '.');
		snprintf(stem, sizeof(stem), "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
		snprintf(out->copied_names[0], sizeof(out->copied_names[0]), "saves/%.20s/%.60s.srm",
			 g_fake_bk_opts.game_system, stem);
		snprintf(out->copied_names[1], sizeof(out->copied_names[1]), "states/%.20s/%.50s.state.auto",
			 g_fake_bk_opts.game_system, stem);
		out->ncopied_names = MIN(out->copied, 2);
	}
	out->state = k >= 30 ? TRANSFER_DONE : TRANSFER_RUNNING;
	return out->state;
}

static void fk_backup_cancel(void)
{
	g_fake_bk_polls = 30;
}

static void fk_backup_finish(void)
{
	g_fake_backing = false;
}

static int fk_webshare_start(const struct webshare_config *cfg)
{
	(void)cfg;
	g_fake_share = true;
	return 0;
}

static void fk_webshare_stop(void)
{
	g_fake_share = false;
}

static bool fk_webshare_running(void)
{
	return g_fake_share;
}

static void fk_webshare_status(struct webshare_status *st)
{
	memset(st, 0, sizeof(*st));
	st->running = g_fake_share;
	st->port = 80;
	strcpy(st->pin, "482913");
	strcpy(st->urls[0], "http://192.168.1.23/");
	st->nurls = 1;
	strcpy(st->mdns_url, "http://retrostone.local/");
	strcpy(st->qr_text, "http://192.168.1.23/#pin=482913");
	st->clients = 1;
	st->sessions = 1;
	st->files_received = 3;
	strcpy(st->current, "Aurora Quest (USA).sfc");
	st->cur_done = 420 << 10;
	st->cur_total = 1 << 20;
}

static int fk_take_changes(char out[][TRANSFER_SYSID_MAX], int max)
{
	(void)out;
	(void)max;
	return 0;
}

static void fk_netnames_defaults(struct netnames_config *c)
{
	memset(c, 0, sizeof(*c));
	c->hostname = "retrostone";
}

static int fk_netnames_start(const struct netnames_config *c)
{
	(void)c;
	return 0;
}

static void fk_netnames_stop(void)
{
}

static const struct ui_transfer_api g_fake_transfer = {
	.usb_drives = fk_usb_drives,
	.usb_eject = fk_usb_eject,
	.usb_remount = fk_usb_remount,
	.trees_start = fk_trees_start,
	.trees_poll = fk_trees_poll,
	.plan_start_trees = fk_plan_start_trees,
	.plan_poll = fk_plan_poll,
	.plan_free = fk_plan_free,
	.import_defaults = fk_import_defaults,
	.import_start = fk_import_start,
	.import_status = fk_import_status,
	.import_answer = fk_import_answer,
	.import_cancel = fk_import_cancel,
	.import_finish = fk_import_finish,
	.backup_scan_start = fk_backup_scan_start,
	.backup_scan_poll = fk_backup_scan_poll,
	.backup_info = fk_backup_info,
	.backup_totals = fk_backup_totals,
	.backup_free = fk_backup_free,
	.backup_start = fk_backup_start,
	.backup_status = fk_backup_status,
	.backup_cancel = fk_backup_cancel,
	.backup_finish = fk_backup_finish,
	.webshare_start = fk_webshare_start,
	.webshare_stop = fk_webshare_stop,
	.webshare_running = fk_webshare_running,
	.webshare_get_status = fk_webshare_status,
	.webshare_take_changes = fk_take_changes,
	.netnames_defaults = fk_netnames_defaults,
	.netnames_start = fk_netnames_start,
	.netnames_stop = fk_netnames_stop,
	.qr_encode = qr_encode,
	.data_root = NULL,
};

static bool g_resume_all;   /* --resume: every game has an auto save state */

static int launch_stub(const struct ui_launch *req, void *user)
{
	(void)user;
	printf("LAUNCH system=%s core=%s core_path=%s rom=%s resume=%d\n", req->system, req->core,
	       req->core_path, req->rom_path, req->resume);
	/* exercise the host-message path once */
	if (strstr(req->rom_path, "Crystal Maze") && req->message && req->message_size)
		snprintf(req->message, req->message_size, "Preview: a message from the host.");
	return 0;
}

static bool has_resume_stub(const struct ui_launch *req, void *user)
{
	(void)req;
	(void)user;
	return g_resume_all;
}

static void setting_stub(const char *key, const char *value, void *user)
{
	(void)user;
	printf("SETTING %s=%s\n", key, value);
}

static void power_stub(enum ui_power_action a, void *user)
{
	(void)user;
	printf("POWER %s\n", a == UI_REBOOT ? "reboot" : "off");
}

static void bench(struct preview *p)
{
	int64_t t0;
	int n = 60;

	/* static frame of the current screen */
	t0 = ui_now_us();
	for (int i = 0; i < n; i++)
		ui_render(p->ui, &p->s);
	printf("bench: full frame %.2f ms\n", (double)(ui_now_us() - t0) / n / 1000.0);
	/* carousel animation frames */
	ui_button(p->ui, IN_RIGHT, IN_NAV_PRESS);
	ui_button(p->ui, IN_RIGHT, IN_NAV_RELEASE);
	t0 = ui_now_us();
	for (int i = 0; i < 14; i++) {
		p->now += 16;
		ui_update(p->ui, p->now);
		ui_render(p->ui, &p->s);
	}
	printf("bench: carousel animation frame %.2f ms (includes loading the next system)\n",
	       (double)(ui_now_us() - t0) / 14 / 1000.0);
	settle(p, 1000);
	ui_button(p->ui, IN_LEFT, IN_NAV_PRESS);
	ui_button(p->ui, IN_LEFT, IN_NAV_RELEASE);
	settle(p, 1000);
	ui_button(p->ui, IN_RIGHT, IN_NAV_PRESS);
	ui_button(p->ui, IN_RIGHT, IN_NAV_RELEASE);
	t0 = ui_now_us();
	for (int i = 0; i < 14; i++) {
		p->now += 16;
		ui_update(p->ui, p->now);
		ui_render(p->ui, &p->s);
	}
	printf("bench: carousel animation frame, warm %.2f ms\n",
	       (double)(ui_now_us() - t0) / 14 / 1000.0);
	settle(p, 1000);
	/* game list scrolling (key repeat: images deferred) */
	ui_button(p->ui, IN_A, IN_NAV_PRESS);
	ui_button(p->ui, IN_A, IN_NAV_RELEASE);
	settle(p, 1000);
	t0 = ui_now_us();
	for (int i = 0; i < 30; i++) {
		ui_button(p->ui, IN_DOWN, IN_NAV_REPEAT);
		p->now += 70;
		ui_update(p->ui, p->now);
		ui_render(p->ui, &p->s);
	}
	printf("bench: game list scroll frame %.2f ms\n", (double)(ui_now_us() - t0) / 30 / 1000.0);
	ui_button(p->ui, IN_DOWN, IN_NAV_RELEASE);
	settle(p, 1000);
	ui_button(p->ui, IN_B, IN_NAV_PRESS);
	ui_button(p->ui, IN_B, IN_NAV_RELEASE);
	settle(p, 1000);
}

int main(int argc, char **argv)
{
	struct preview p;
	struct ui_config cfg;
	char roms[1024], data[1024], cache[1100], themes_user[1024], cores[1024];
	const char *root = NULL, *keys = NULL, *out = NULL, *theme = NULL;
	int w = 640, h = 480;
	bool timing = false, do_bench = false, quiet = false;
	int64_t t_start = ui_now_us(), t_menu, t_lists;
	int rc = 0;

	memset(&p, 0, sizeof(p));
	ui_config_defaults(&cfg);
	/* repository layout (run from frontend/) */
	cfg.themes_builtin = "themes";
	cfg.res_dir = "third_party";
	cfg.version = "RetroStoneOS preview";
	cfg.net_helper = "/bin/false";
	cfg.update_helper = "";          /* --update-helper: Settings > System update */
	cfg.smb_helper = "";             /* --smb-helper: the Windows file share */
	cfg.boot_env = "";
	roms[0] = data[0] = cache[0] = themes_user[0] = cores[0] = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

#define OPT(name) (!strcmp(a, name) && v && ++i)
		if (OPT("--root"))
			root = v;
		else if (OPT("--roms"))
			strlcpy_(roms, v, sizeof(roms));
		else if (OPT("--data"))
			strlcpy_(data, v, sizeof(data));
		else if (OPT("--cache"))
			strlcpy_(cache, v, sizeof(cache));
		else if (OPT("--themes"))
			cfg.themes_builtin = v;
		else if (OPT("--themes-user"))
			strlcpy_(themes_user, v, sizeof(themes_user));
		else if (OPT("--res"))
			cfg.res_dir = v;
		else if (OPT("--cores"))
			strlcpy_(cores, v, sizeof(cores));
		else if (OPT("--theme"))
			theme = v;
		else if (OPT("--keys"))
			keys = v;
		else if (OPT("--out"))
			out = v;
		else if (OPT("--size")) {
			if (sscanf(v, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
				usage();
				return 2;
			}
		} else if (!strcmp(a, "--timing"))
			timing = true;
		else if (!strcmp(a, "--bench"))
			do_bench = true;
		else if (!strcmp(a, "--quiet"))
			quiet = true;
		else if (!strcmp(a, "--resume"))
			g_resume_all = true;
		else if (!strcmp(a, "--fake-transfer"))
			cfg.transfer = &g_fake_transfer;
		else if (OPT("--png-bits"))
			p.png_bits = atoi(v);
		else if (OPT("--lang"))
			cfg.language = v;
		else if (OPT("--locale"))
			cfg.locale_dir = v;
		else if (!strcmp(a, "--language-prompt"))
			cfg.language_prompt = true;
		else if (OPT("--update-helper"))
			cfg.update_helper = v;    /* e.g. tests/fake-rsos-update.sh */
		else if (OPT("--smb-helper"))
			cfg.smb_helper = v;       /* Settings > Network > Windows file share */
		else {
			usage();
			return 2;
		}
#undef OPT
	}
	if (root) {
		if (!roms[0])
			snprintf(roms, sizeof(roms), "%s/data/roms", root);
		if (!data[0])
			snprintf(data, sizeof(data), "%s/data/rsos", root);
		if (!themes_user[0])
			snprintf(themes_user, sizeof(themes_user), "%s/data/themes", root);
		if (!cores[0])
			snprintf(cores, sizeof(cores), "%s/usr/share/rsos/cores", root);
		{
			static char env[1024], ps[1024], wpa[1024];

			snprintf(env, sizeof(env), "%s/boot/rsos.env", root);
			snprintf(ps, sizeof(ps), "%s/sys/class/power_supply", root);
			snprintf(wpa, sizeof(wpa), "%s/data/rsos/wpa_supplicant.conf", root);
			cfg.boot_env = env;
			cfg.power_supply_dir = ps;
			cfg.wpa_conf = wpa;
		}
	}
	if (!cache[0] && data[0])
		snprintf(cache, sizeof(cache), "%s/cache", data);
	if (!strcmp(cache, "none"))
		cache[0] = 0;
	if (roms[0])
		cfg.roms_dir = roms;
	if (data[0])
		cfg.data_dir = data;
	cfg.cache_dir = cache;
	if (themes_user[0])
		cfg.themes_user = themes_user;
	if (cores[0])
		cfg.cores_dir = cores;
	cfg.cb.launch = launch_stub;
	cfg.cb.has_resume = has_resume_stub;
	cfg.cb.setting_changed = setting_stub;
	cfg.cb.power = power_stub;
	if (quiet)
		ui_log_set(NULL, NULL, UI_LOG_WARN);

	p.ui = ui_create(&cfg);
	p.now = 1000;
	alloc_fb(&p, w, h);
	if (theme)
		ui_select_theme(p.ui, theme);
	/* the lists load on a background thread, like on the device: frames
	 * go on meanwhile (the carousel comes first when a snapshot exists) */
	while (!ui_is_loaded(p.ui)) {
		if (ui_update(p.ui, p.now))
			render(&p);
		p.now += 16;
		usleep(1000);
	}
	t_menu = ui_now_us();
	if (ui_update(p.ui, p.now))
		render(&p);
	/* (the first-boot language picker holds carousel changes back until it is
	 * answered, as on the device: do not wait for them then) */
	while (!ui_lists_complete(p.ui) && !ui_first_boot_busy(p.ui)) {
		if (ui_update(p.ui, p.now))
			render(&p);
		p.now += 16;
		usleep(1000);
	}
	t_lists = ui_now_us();
	settle(&p, 1000);
	if (timing) {
		struct ui_timings t;
		struct img_stats st;

		ui_get_timings(p.ui, &t);
		img_get_stats(&st);
		printf("timing: process start -> carousel frame %.1f ms, -> every list %.1f ms\n",
		       (double)(t_menu - t_start) / 1000.0, (double)(t_lists - t_start) / 1000.0);
		printf("timing: ui_create %.1f ms, carousel built %.1f ms, game lists %.1f ms (%d games, "
		       "%d entries), theme XML %.1f ms, first frame %.1f ms\n",
		       (double)t.create_us / 1000.0, (double)t.menu_us / 1000.0,
		       (double)t.systems_us / 1000.0, t.ngames, t.nsystems, (double)t.theme_us / 1000.0,
		       (double)t.first_frame_us / 1000.0);
		printf("timing: images decoded %d (%.1f ms), from disk cache %d (%.1f ms), "
		       "memory hits %d, %.1f MB in memory\n",
		       st.decoded, (double)st.decode_us / 1000.0, st.disk_hits,
		       (double)st.disk_us / 1000.0, st.mem_hits, (double)img_mem_used() / 1048576.0);
	}
	if (do_bench)
		bench(&p);
	if (keys)
		rc |= run_script(&p, keys);
	if (out)
		rc |= shot(&p, out);
	if (timing)
		printf("timing: %d frames rendered, average %.2f ms, max %.2f ms\n", p.frames,
		       p.frames ? (double)p.render_us / p.frames / 1000.0 : 0.0,
		       (double)p.render_max_us / 1000.0);
	ui_destroy(p.ui);
	free(p.fb);
	return rc ? 1 : 0;
}
