/*
 * switcher.c - the game switcher (Select+Y, docs/host-design.md §11.1): the
 * recently played games as cards with their auto-state pictures, the running
 * one first (its live picture). A on another game: the game process saves
 * this one (auto state) and exits with HOST_EXIT_SWITCH + "switch <n>"; the
 * menu launches entry n, resumed from its own auto state. A on the running
 * game, B, START or Y: back to the game.
 *
 * The list comes from the file the menu writes before each launch
 * (--switcher, host_switcher_read()). Drawn like the in-game menu (draw.h on
 * a 640x480 XRGB8888 canvas over the dimmed last frame); the game is paused
 * meanwhile (no retro_run, audio faded, the play-time clock stopped).
 */
#include <drm_fourcc.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../audio/audio.h"
#include "draw.h"
#include "host_input.h"
#include "host_internal.h"
#include "host_png.h"
#include "hutil.h"
#include "../i18n/i18n.h"

#define CW 640
#define CH 480
#define COLS 4
#define CARD_W 140
#define CARD_H 150
#define THUMB_W 128
#define THUMB_H 96
#define GAP 12
#define GRID_Y 62

#define COL_BG      0xe8101828u
#define COL_CARD    0xe0182030u
#define COL_SEL     0xff3050a0u
#define COL_TEXT    0xe8e8e8u
#define COL_DIM     0x8890a0u
#define COL_ACCENT  0xffd060u
#define COL_TITLE   0xffffffu

static struct {
	struct host_switch_entry e[HOST_SWITCHER_MAX];
	int n;
	uint8_t *thumb[HOST_SWITCHER_MAX];
	int tw[HOST_SWITCHER_MAX], th[HOST_SWITCHER_MAX];
	int sel;
	uint32_t *px, *bg;
	struct canvas cv;
} S;

static void load(void)
{
	for (int i = 0; i < S.n; i++) {
		host_png_free(S.thumb[i]);
		S.thumb[i] = NULL;
	}
	S.n = H.cfg.switcher_path ? host_switcher_read(H.cfg.switcher_path, S.e, HOST_SWITCHER_MAX) : 0;
	for (int i = 0; i < S.n; i++)
		if (!S.e[i].current && S.e[i].thumb[0])
			S.thumb[i] = host_png_read_rgb(S.e[i].thumb, &S.tw[i], &S.th[i]);
}

static void unload(void)
{
	for (int i = 0; i < S.n; i++) {
		host_png_free(S.thumb[i]);
		S.thumb[i] = NULL;
	}
	S.n = 0;
}

bool host_switcher_available(void)
{
	struct host_switch_entry e[HOST_SWITCHER_MAX];
	int n = H.cfg.switcher_path ? host_switcher_read(H.cfg.switcher_path, e, HOST_SWITCHER_MAX) : 0;

	for (int i = 0; i < n; i++)
		if (!e[i].current)
			return true;
	return false;
}

static bool alloc_canvas(void)
{
	if (!S.px) {
		S.px = malloc((size_t)CW * CH * 4);
		S.bg = malloc((size_t)CW * CH * 4);
		if (!S.px || !S.bg) {
			free(S.px);
			free(S.bg);
			S.px = S.bg = NULL;
			return false;
		}
	}
	S.cv = (struct canvas){ S.px, CW, CH, CW };
	return true;
}

/* The dimmed game picture behind the cards (as the in-game menu). */
static void make_background(void)
{
	uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;
	struct canvas bg = { S.bg, CW, CH, CW };
	double ar;
	int w = CW, h;

	host_capture_last();
	ar = H.av.geometry.aspect_ratio > 0 ? H.av.geometry.aspect_ratio :
	     H.last_h ? (double)H.last_w / H.last_h : 4.0 / 3;
	h = (int)(CW / ar);
	memset(S.bg, 0, (size_t)CW * CH * 4);
	if (h > CH) {
		h = CH;
		w = (int)(CH * ar);
	}
	if (H.last_frame)
		cv_blit_frame(&bg, (CW - w) / 2, (CH - h) / 2, w, h, H.last_frame, (int)H.last_w, (int)H.last_h,
			      (int)H.last_pitch, fmt, 70);
}

static void card_rect(int i, int *x, int *y)
{
	int cols = S.n < COLS ? S.n : COLS;
	int rows = (S.n + COLS - 1) / COLS;
	int gw = cols * CARD_W + (cols - 1) * GAP;
	int gh = rows * CARD_H + (rows - 1) * GAP;
	int y0 = GRID_Y + (CH - 84 - GRID_Y - gh) / 2;   /* above the name line */

	if (y0 < GRID_Y)
		y0 = GRID_Y;
	*x = (CW - gw) / 2 + (i % COLS) * (CARD_W + GAP);
	*y = y0 + (i / COLS) * (CARD_H + GAP);
}

static void draw(void)
{
	struct canvas *c = &S.cv;
	uint32_t fmt = H.hw_requested ? DRM_FORMAT_XRGB8888 : H.drm_format;
	char buf[256];

	memcpy(S.px, S.bg, (size_t)CW * CH * 4);
	cv_fill(c, 12, 12, CW - 24, CH - 24, COL_BG);
	/* TRANSLATORS: title of the game switcher (Select+Y in a game): the
	 * games played last, to jump to one (640 px, large text) */
	cv_text_fit(c, 24, 24, _("Recent games"), COL_TITLE, 2, CW - 48);
	cv_fill(c, 24, 50, CW - 48, 2, 0xff3050a0u);
	for (int i = 0; i < S.n; i++) {
		const struct host_switch_entry *e = &S.e[i];
		int x, y, tx, ty = 0;
		bool sel = i == S.sel;

		card_rect(i, &x, &y);
		if (sel)
			cv_fill(c, x - 4, y - 4, CARD_W + 8, CARD_H + 8, COL_SEL);
		cv_fill(c, x, y, CARD_W, CARD_H, COL_CARD);
		tx = x + (CARD_W - THUMB_W) / 2;
		ty = y + 6;
		cv_fill(c, tx, ty, THUMB_W, THUMB_H, 0xff000000u);
		if (e->current && H.last_frame) {
			/* the running game: its picture now */
			cv_blit_frame(c, tx, ty, THUMB_W, THUMB_H, H.last_frame, (int)H.last_w, (int)H.last_h,
				      (int)H.last_pitch, fmt, 256);
		} else if (S.thumb[i]) {
			cv_blit_rgb(c, tx, ty, THUMB_W, THUMB_H, S.thumb[i], S.tw[i], S.th[i]);
		} else {
			/* TRANSLATORS: game switcher: the game has no saved picture
			 * (it will start from the beginning); ~16 characters */
			cv_text_ellipsize(_("No picture"), 1, THUMB_W - 8, buf, sizeof(buf));
			cv_text(c, tx + (THUMB_W - cv_text_width(buf, 1)) / 2, ty + THUMB_H / 2 - 4, buf, COL_DIM, 1);
		}
		cv_text_ellipsize(e->name, 1, CARD_W - 10, buf, sizeof(buf));
		cv_text(c, x + (CARD_W - cv_text_width(buf, 1)) / 2, ty + THUMB_H + 10, buf, COL_TEXT, 1);
		if (e->current)
			/* TRANSLATORS: game switcher: under the running game's card (~18 characters) */
			cv_text_ellipsize(_("Now playing"), 1, CARD_W - 10, buf, sizeof(buf));
		else
			cv_text_ellipsize(system_display_name(e->system), 1, CARD_W - 10, buf, sizeof(buf));
		cv_text(c, x + (CARD_W - cv_text_width(buf, 1)) / 2, ty + THUMB_H + 30, buf,
			e->current ? COL_ACCENT : COL_DIM, 1);
	}
	/* the full name of the selected game */
	if (S.n)
		cv_text_fit(c, 24, CH - 72, S.e[S.sel].name, COL_TEXT, 2, CW - 48);
	/* TRANSLATORS: game switcher button help (one 592 px line) */
	cv_text_fit(c, 24, CH - 40, _("A: play  B: back to the game"), COL_DIM, 2, CW - 48);
}

static void move(int d)
{
	int n = S.sel + d;

	if (n >= 0 && n < S.n)
		S.sel = n;
}

int host_switcher_run(void)
{
	int chosen = -1;
	bool done = false, dirty = true;

	if (!H.display_ok || !alloc_canvas())
		return -1;
	load();
	if (S.n < 1) {
		unload();
		return -1;
	}
	/* the first other game: the one played before this */
	S.sel = S.n > 1 ? 1 : 0;
	hlog(HLOG_INFO, "switcher open (%d games)", S.n);
	audio_pause(true);
	hin_set_game_mode(false);
	make_background();
	host_video_invalidate();
	if (!display_set_game_surface(CW, CH, DRM_FORMAT_XRGB8888, 0)) {
		hin_set_game_mode(true);
		audio_pause(false);
		unload();
		return -1;
	}
	display_set_scaling(DISPLAY_SCALE_ASPECT, 4.0 / 3.0);
	while (!done && !H.quit) {
		struct input_nav ev;
		enum input_hotkey hk;
		struct pollfd pfd[2];
		int nfd = 0, to = hin_timeout_ms();

		host_heartbeat();
		if (dirty) {
			draw();
			display_present_copy(S.px, CW * 4);
			dirty = false;
		}
		pfd[nfd].fd = display_get_fd();
		pfd[nfd++].events = POLLIN;
		if (hin_fd() >= 0) {
			pfd[nfd].fd = hin_fd();
			pfd[nfd++].events = POLLIN;
		}
		if (to < 0 || to > 20)
			to = 20; /* audio keepalive */
		poll(pfd, (nfds_t)nfd, to);
		display_handle_events();
		audio_keepalive();
		hin_poll();
		host_poll_signals();
		while (hin_next_hotkey(&hk))
			if (hk == IN_HK_POWER_OFF && H.cfg.status_fd < 0) {
				H.poweroff = true;
				host_request_quit(HOST_EXIT_POWEROFF);
			}
		while (hin_next_nav(&ev)) {
			if (ev.type == IN_NAV_RELEASE)
				continue;
			dirty = true;
			switch (ev.btn) {
			case IN_LEFT:
				move(-1);
				break;
			case IN_RIGHT:
				move(1);
				break;
			case IN_UP:
				move(-COLS);
				break;
			case IN_DOWN:
				move(COLS);
				break;
			case IN_A:
				if (ev.type == IN_NAV_PRESS) {
					chosen = S.e[S.sel].current ? -1 : S.sel;
					done = true;
				}
				break;
			case IN_B:
			case IN_START:
			case IN_Y:
				if (ev.type == IN_NAV_PRESS)
					done = true;
				break;
			default:
				break;
			}
		}
	}
	hlog(HLOG_INFO, "switcher closed%s%s", chosen > 0 ? ": " : "", chosen > 0 ? S.e[chosen].name : "");
	unload();
	hin_set_game_mode(true);
	host_video_invalidate();
	host_video_setup();
	host_present_last();
	audio_pause(false);
	H.next_frame_us = 0;
	return chosen;
}

/* Tests (rsos-run --switcher-shot): the switcher as it opens, to PNG,
 * without a display. */
int host_switcher_screenshot(const char *path)
{
	uint8_t *rgb;
	void *png;
	size_t size;
	int ret = -1;

	if (!alloc_canvas())
		return -1;
	load();
	if (S.n < 1) {
		unload();
		return -1;
	}
	S.sel = S.n > 1 ? 1 : 0;
	make_background();
	draw();
	rgb = malloc((size_t)CW * CH * 3);
	if (rgb) {
		for (int i = 0; i < CW * CH; i++) {
			rgb[3 * i] = (uint8_t)(S.px[i] >> 16);
			rgb[3 * i + 1] = (uint8_t)(S.px[i] >> 8);
			rgb[3 * i + 2] = (uint8_t)S.px[i];
		}
		png = host_png_encode(rgb, CW, CH, &size);
		if (png)
			ret = hwrite_atomic(path, png, size, false);
		free(png);
		free(rgb);
	}
	unload();
	return ret;
}
