/*
 * batt_overlay.c - the in-game battery indicator picture (see
 * batt_overlay.h). Drawn at 1x with the 8x8 font (nothing to load in the
 * game process), then widened to 2x on large outputs.
 */
#include "batt_overlay.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../font8x8.h"

#define C_CLEAR 0x00000000u
#define C_BG    0xa8000000u   /* black, alpha only: premultiplied = straight */
#define C_WHITE 0xfff2f2f2u
#define C_AMBER 0xffffb020u
#define C_RED   0xffff4038u
#define C_BOLT  0xffffe040u

#define PAD 6         /* inside the pill, left and right */
#define ICON_W 16     /* body 14 + nub 2 */
#define ICON_H 9
#define GAP 4
#define BOLT_W 5
#define RADIUS 6

/* 5 x 7, bit 4 = leftmost pixel. */
static const uint8_t bolt[7] = { 0x03, 0x06, 0x0c, 0x1f, 0x06, 0x0c, 0x18 };

/* 8x8, bit 7 = leftmost pixel: a play triangle (OVL_GLYPH_PLAY). */
static const uint8_t play_glyph[8] = { 0x40, 0x60, 0x70, 0x78, 0x70, 0x60, 0x40, 0x00 };

static const uint8_t *strip_glyph(unsigned char c)
{
	return c == OVL_GLYPH_PLAY ? play_glyph : font8x8_glyph(c);
}

static void put(uint32_t *px, int stride, int x, int y, uint32_t c)
{
	if (x >= 0 && x < BOV_W && y >= 0 && y < BOV_H)
		px[(size_t)y * stride + x] = c;
}

static void rect(uint32_t *px, int stride, int x, int y, int w, int h, uint32_t c)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			put(px, stride, x + i, y + j, c);
}

/* Rounded pill [x0, x0 + w) x [0, BOV_H): corners cut on a circle. */
static void pill(uint32_t *px, int stride, int x0, int w)
{
	for (int y = 0; y < BOV_H; y++)
		for (int x = 0; x < w; x++) {
			int dx = x < RADIUS ? RADIUS - x : x >= w - RADIUS ? x - (w - RADIUS - 1) : 0;
			int dy = y < RADIUS ? RADIUS - y : y >= BOV_H - RADIUS ? y - (BOV_H - RADIUS - 1) : 0;

			if (dx * dx + dy * dy <= RADIUS * RADIUS)
				put(px, stride, x0 + x, y, C_BG);
		}
}

int bov_render(uint32_t *px, int stride, int scale, int pct, bool charging, bool right)
{
	char text[8];
	uint32_t fg;
	int n, w, x0, x, yi = (BOV_H - ICON_H) / 2, yt = (BOV_H - 8) / 2, fill;

	if (scale < 1)
		scale = 1;
	if (scale > BOV_MAX_SCALE)
		scale = BOV_MAX_SCALE;
	if (pct > 100)
		pct = 100;
	if (pct < 0)
		pct = 0;
	for (int y = 0; y < BOV_H * scale; y++)
		for (int i = 0; i < BOV_W * scale; i++)
			px[(size_t)y * stride + i] = C_CLEAR;

	n = snprintf(text, sizeof(text), "%d%%", pct);
	w = PAD + ICON_W + GAP + (charging ? BOLT_W + 2 : 0) + n * 8 + PAD - 1;
	x0 = right ? BOV_W - w : 0;
	fg = pct <= BOV_CRIT_PCT ? C_RED : pct <= BOV_LOW_PCT ? C_AMBER : C_WHITE;
	pill(px, stride, x0, w);

	/* battery: outline, nub, fill level */
	x = x0 + PAD;
	rect(px, stride, x, yi, 14, 1, fg);
	rect(px, stride, x, yi + ICON_H - 1, 14, 1, fg);
	rect(px, stride, x, yi, 1, ICON_H, fg);
	rect(px, stride, x + 13, yi, 1, ICON_H, fg);
	rect(px, stride, x + 14, yi + 2, 2, ICON_H - 4, fg);
	fill = (10 * pct + 50) / 100;
	if (pct > 0 && fill < 1)
		fill = 1;
	rect(px, stride, x + 2, yi + 2, fill, ICON_H - 4, fg);
	x += ICON_W + GAP;

	if (charging) {
		for (int j = 0; j < 7; j++)
			for (int i = 0; i < BOLT_W; i++)
				if (bolt[j] & (0x10 >> i))
					put(px, stride, x + i, yi + 1 + j, C_BOLT);
		x += BOLT_W + 2;
	}

	for (int k = 0; k < n; k++) {
		const uint8_t *g = font8x8_glyph((unsigned char)text[k]);

		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++)
				if (g[j] & (0x80 >> i))
					put(px, stride, x + k * 8 + i, yt + j, fg);
	}

	/* 2x: widen in place, from the bottom-right corner backwards */
	if (scale > 1) {
		for (int y = BOV_H * scale - 1; y >= 0; y--)
			for (int i = BOV_W * scale - 1; i >= 0; i--)
				px[(size_t)y * stride + i] = px[(size_t)(y / scale) * stride + i / scale];
	}
	return w * scale;
}

int bov_parse(const char *text, int *pct, bool *charging)
{
	int p, c;

	if (!text || sscanf(text, "%d %d", &p, &c) != 2 || p < -1 || p > 100 || (c != 0 && c != 1))
		return -1;
	*pct = p;
	*charging = c == 1;
	return 0;
}

int bov_read(const char *path, int *pct, bool *charging)
{
	char buf[32];
	ssize_t r;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -EIO;
	buf[r] = 0;
	return bov_parse(buf, pct, charging);
}

/* ------------------------------------------------------ overlay strip */

int ovl_strip_height(int nlines)
{
	int h = 6 + 10 * nlines;

	return h > BOV_H ? h : BOV_H;
}

int ovl_render_strip(uint32_t *px, int w1, int scale, const char *const *lines, int nlines, int pct,
		     bool charging, bool batt_right)
{
	int h1 = ovl_strip_height(nlines), stride = w1 * scale, tw = 0, tx, room;
	uint32_t batt[BOV_W * BOV_H];

	if (scale < 1)
		scale = 1;
	if (scale > BOV_MAX_SCALE)
		scale = BOV_MAX_SCALE;
	stride = w1 * scale;
	for (int y = 0; y < h1 * scale; y++)
		for (int x = 0; x < stride; x++)
			px[(size_t)y * stride + x] = C_CLEAR;
	/* battery pill in its corner */
	if (pct >= 0) {
		int bx = batt_right ? w1 - BOV_W : 0;

		bov_render(batt, BOV_W, 1, pct, charging, batt_right);
		for (int y = 0; y < BOV_H; y++)
			for (int x = 0; x < BOV_W; x++)
				if (batt[y * BOV_W + x] && bx + x >= 0 && bx + x < w1)
					px[(size_t)y * stride + bx + x] = batt[y * BOV_W + x];
	}
	/* text block on the other side */
	room = (w1 - (pct >= 0 ? BOV_W + 4 : 0) - 2 * PAD) / 8;
	for (int i = 0; i < nlines; i++) {
		int l = lines[i] ? (int)strlen(lines[i]) : 0;

		if (l > room)
			l = room;
		if (l * 8 > tw)
			tw = l * 8;
	}
	if (tw > 0) {
		int bw = tw + 2 * PAD, x0 = batt_right || pct < 0 ? 0 : w1 - bw;

		for (int y = 0; y < h1; y++)
			for (int x = 0; x < bw; x++)
				px[(size_t)y * stride + x0 + x] = C_BG;
		tx = x0 + PAD;
		for (int i = 0; i < nlines; i++) {
			const char *t = lines[i] ? lines[i] : "";
			int yt = 3 + 10 * i + (h1 - (6 + 10 * nlines)) / 2;

			for (int k = 0; t[k] && k < room; k++) {
				const uint8_t *g = strip_glyph((unsigned char)t[k]);

				for (int j = 0; j < 8; j++)
					for (int b = 0; b < 8; b++)
						if (g[j] & (0x80 >> b))
							px[(size_t)(yt + j) * stride + tx + k * 8 + b] =
								i == 0 ? C_WHITE : C_BOLT;
			}
		}
	}
	if (scale > 1)
		for (int y = h1 * scale - 1; y >= 0; y--)
			for (int x = stride - 1; x >= 0; x--)
				px[(size_t)y * stride + x] = px[(size_t)(y / scale) * stride + x / scale];
	return h1 * scale;
}
