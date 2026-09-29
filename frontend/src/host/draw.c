/*
 * draw.c - see draw.h.
 */
#include "draw.h"

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../font8x8.h"
#include "../gfx/font.h"
#include "../gfx/gfx.h"
#include "hutil.h"

static inline uint32_t blend(uint32_t d, uint32_t s, unsigned a)
{
	unsigned ia = 255 - a;
	uint32_t rb = (((s & 0xff00ff) * a + (d & 0xff00ff) * ia) >> 8) & 0xff00ff;
	uint32_t g = (((s & 0x00ff00) * a + (d & 0x00ff00) * ia) >> 8) & 0x00ff00;

	return rb | g;
}

static bool clip(const struct canvas *c, int *x, int *y, int *w, int *h)
{
	if (*x < 0) {
		*w += *x;
		*x = 0;
	}
	if (*y < 0) {
		*h += *y;
		*y = 0;
	}
	if (*x + *w > c->w)
		*w = c->w - *x;
	if (*y + *h > c->h)
		*h = c->h - *y;
	return *w > 0 && *h > 0;
}

void cv_fill(struct canvas *c, int x, int y, int w, int h, uint32_t argb)
{
	unsigned a = argb >> 24;

	if (!a || !clip(c, &x, &y, &w, &h))
		return;
	for (int j = 0; j < h; j++) {
		uint32_t *p = c->px + (size_t)(y + j) * (size_t)c->stride + x;

		if (a == 255)
			for (int i = 0; i < w; i++)
				p[i] = argb & 0xffffff;
		else
			for (int i = 0; i < w; i++)
				p[i] = blend(p[i], argb, a);
	}
}

/* ------------------------------------------------------------------ fonts */

static struct {
	bool ok;
	char face[1100];   /* the menu face ("" = the default font) */
} DF;

bool draw_set_fonts(const char *dir)
{
	char p[1100];

	DF.ok = false;
	DF.face[0] = 0;
	if (!dir || !*dir)
		return false;
	snprintf(p, sizeof(p), "%s/DejaVuSans.ttf", dir);
	if (access(p, R_OK) != 0) {
		hlog(HLOG_WARN, "fonts: no %s, 8x8 text (ASCII only)", p);
		return false;
	}
	font_setup_dir(dir);
	/* narrower than DejaVu Sans: long translations fit the menu rows */
	snprintf(p, sizeof(p), "%s/DejaVuSansCondensed.ttf", dir);
	if (access(p, R_OK) == 0)
		hstrlcpy(DF.face, p, sizeof(DF.face));
	DF.ok = font_get(DF.face[0] ? DF.face : NULL, 18) != NULL;
	hlog(DF.ok ? HLOG_INFO : HLOG_WARN, "fonts: %s%s, %d fallback font(s)%s", DF.face[0] ? DF.face : p,
	     DF.face[0] ? "" : " (no condensed face)", font_fallback_count(), DF.ok ? "" : ": unusable, 8x8 text");
	return DF.ok;
}

bool draw_ttf(void)
{
	return DF.ok;
}

/* The menu's text: the 8x8 cell at scale 1 / 2 is about an 11 / 18 px EM. */
static struct font *cv_font(int scale)
{
	if (!DF.ok)
		return NULL;
	return font_get(DF.face[0] ? DF.face : NULL, scale <= 1 ? 11 : 9 * scale);
}

/* 8x8 fallback: one '?' per non-ASCII character. */
static void ascii_only(const char *s, char *out, size_t n)
{
	size_t o = 0;

	while (*s && o + 1 < n) {
		unsigned c = utf8_next(&s);

		out[o++] = c >= 32 && c < 127 ? (char)c : '?';
	}
	out[o] = 0;
}

static int cps(const char *s)
{
	int n = 0;

	for (; *s; s++)
		n += ((unsigned char)*s & 0xc0) != 0x80;
	return n;
}

int cv_text_width(const char *s, int scale)
{
	struct font *f = cv_font(scale);

	if (f)
		return font_text_width(f, s, -1);
	return cps(s) * 8 * scale;
}

void cv_text_ellipsize(const char *s, int scale, int max_w, char *buf, size_t n)
{
	struct font *f = cv_font(scale);
	int max_c, k;
	size_t o;

	if (!n)
		return;
	if (f) {
		font_ellipsize(f, s, max_w, buf, n);
		return;
	}
	max_c = max_w / (8 * scale);
	if (cps(s) <= max_c || max_w <= 0) {
		hstrlcpy(buf, s, n);
		return;
	}
	/* keep max_c - 3 characters, then "..." */
	max_c = max_c > 3 ? max_c - 3 : 0;
	for (o = 0, k = 0; s[o] && o + 4 < n; o++) {
		if (((unsigned char)s[o] & 0xc0) != 0x80 && k++ == max_c)
			break;
	}
	while (o > 0 && ((unsigned char)s[o] & 0xc0) == 0x80)
		o--;
	memcpy(buf, s, o);
	memcpy(buf + o, "...", 4);
}

int cv_text_fit(struct canvas *c, int x, int y, const char *s, uint32_t rgb, int scale, int max_w)
{
	char buf[512];

	cv_text_ellipsize(s, scale, max_w, buf, sizeof(buf));
	return cv_text(c, x, y, buf, rgb, scale);
}

static int cv_text_8x8(struct canvas *c, int x, int y, const char *s, uint32_t rgb, int scale);

int cv_text(struct canvas *c, int x, int y, const char *s, uint32_t rgb, int scale)
{
	struct font *f = cv_font(scale);
	struct gfx_surface surf;

	if (!f) {
		char a[512];

		ascii_only(s, a, sizeof(a));
		return cv_text_8x8(c, x, y, a, rgb, scale);
	}
	gfx_surface_init(&surf, c->px, c->w, c->h, c->stride);
	return font_draw(&surf, f, x, font_baseline_in_box(f, y, 8 * scale), s, -1, 0xff000000u | rgb);
}

static int cv_text_8x8(struct canvas *c, int x, int y, const char *s, uint32_t rgb, int scale)
{
	int x0 = x;

	for (; *s; s++, x += 8 * scale) {
		const uint8_t *g = font8x8_glyph((unsigned char)*s);

		for (int row = 0; row < 8; row++)
			for (int col = 0; col < 8; col++) {
				if (!(g[row] & (0x80 >> col)))
					continue;
				for (int sy = 0; sy < scale; sy++) {
					int py = y + row * scale + sy;
					uint32_t *p;

					if (py < 0 || py >= c->h)
						continue;
					p = c->px + (size_t)py * (size_t)c->stride;
					for (int sx = 0; sx < scale; sx++) {
						int pxx = x + col * scale + sx;

						if (pxx >= 0 && pxx < c->w)
							p[pxx] = rgb & 0xffffff;
					}
				}
			}
	}
	return x - x0;
}

int fmt_bpp(uint32_t fmt)
{
	return fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_ARGB8888 ? 4 : 2;
}

uint32_t fmt_unpack(uint32_t fmt, const void *px)
{
	uint32_t v, r, g, b;

	switch (fmt) {
	case DRM_FORMAT_RGB565:
		v = *(const uint16_t *)px;
		r = (v >> 11) & 31;
		g = (v >> 5) & 63;
		b = v & 31;
		return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
	case DRM_FORMAT_XRGB1555:
		v = *(const uint16_t *)px;
		r = (v >> 10) & 31;
		g = (v >> 5) & 31;
		b = v & 31;
		return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
	default:
		return *(const uint32_t *)px & 0xffffff;
	}
}

uint32_t fmt_pack(uint32_t fmt, uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;

	switch (fmt) {
	case DRM_FORMAT_RGB565:
		return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
	case DRM_FORMAT_XRGB1555:
		return (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
	default:
		return rgb & 0xffffff;
	}
}

void cv_blit_frame(struct canvas *c, int dx, int dy, int dw, int dh, const void *src,
		   int sw, int sh, int pitch, uint32_t fmt, int dim)
{
	int bpp = fmt_bpp(fmt);

	if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	for (int j = 0; j < dh; j++) {
		/* 64-bit: j * sh overflows int for a huge source */
		int y = dy + j, sy = (int)((int64_t)j * sh / dh);
		const uint8_t *line = (const uint8_t *)src + (size_t)sy * (size_t)pitch;
		uint32_t *p;

		if (y < 0 || y >= c->h)
			continue;
		p = c->px + (size_t)y * (size_t)c->stride;
		for (int i = 0; i < dw; i++) {
			int x = dx + i;
			uint32_t v;

			if (x < 0 || x >= c->w)
				continue;
			v = fmt_unpack(fmt, line + (size_t)((int64_t)i * sw / dw) * (size_t)bpp);
			if (dim < 256)
				v = ((((v & 0xff00ff) * (unsigned)dim) >> 8) & 0xff00ff) |
				    ((((v & 0x00ff00) * (unsigned)dim) >> 8) & 0x00ff00);
			p[x] = v;
		}
	}
}

void cv_blit_rgb(struct canvas *c, int dx, int dy, int dw, int dh, const uint8_t *rgb, int sw, int sh)
{
	if (!rgb || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	for (int j = 0; j < dh; j++) {
		int y = dy + j;
		/* 64-bit index math: i * sw overflows int for a 12000000x1
		 * picture (review: a crafted thumbnail) */
		const uint8_t *line = rgb + (size_t)((int64_t)j * sh / dh) * (size_t)sw * 3;

		if (y < 0 || y >= c->h)
			continue;
		for (int i = 0; i < dw; i++) {
			int x = dx + i;
			const uint8_t *s = line + (size_t)((int64_t)i * sw / dw) * 3;

			if (x >= 0 && x < c->w)
				c->px[(size_t)y * (size_t)c->stride + x] =
					(uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
		}
	}
}

static inline void put(void *px, int pitch, uint32_t fmt, int x, int y, uint32_t v)
{
	uint8_t *line = (uint8_t *)px + (size_t)y * (size_t)pitch;

	if (fmt_bpp(fmt) == 4)
		((uint32_t *)line)[x] = v;
	else
		((uint16_t *)line)[x] = (uint16_t)v;
}

/* Toasts: an 11 px EM on the usual 256-320 px wide frames, 20 px from 512. */
static int frame_px(int scale)
{
	return scale <= 1 ? 11 : 10 * scale;
}

int frame_text_height(int scale)
{
	return DF.ok ? frame_px(scale) + 2 * scale : 10 * scale;
}

void frame_text(void *px, int pitch, int w, int h, uint32_t fmt, int x, int y,
		const char *s, int scale, uint32_t fg, uint32_t bg)
{
	static uint32_t *tb;
	static size_t tcap;
	struct font *f = DF.ok ? font_get(DF.face[0] ? DF.face : NULL, frame_px(scale)) : NULL;
	struct gfx_surface surf;
	char buf[512];
	int pad = scale, bh = frame_text_height(scale), bw;

	if (!f || w <= 0 || h <= 0) {
		frame_text_8x8(px, pitch, w, h, fmt, x, y, s, scale, fg, bg);
		return;
	}
	/* cut at the right edge of the frame */
	font_ellipsize(f, s, w - (x > 0 ? x : 0) - 2 * pad, buf, sizeof(buf));
	bw = font_text_width(f, buf, -1) + 2 * pad;
	if ((size_t)bw * (size_t)bh > tcap) {
		uint32_t *n = realloc(tb, (size_t)bw * (size_t)bh * sizeof(*tb));

		if (!n)
			return;
		tb = n;
		tcap = (size_t)bw * (size_t)bh;
	}
	/* the text on its box in XRGB8888, then packed into the frame */
	for (int i = 0; i < bw * bh; i++)
		tb[i] = bg & 0xffffff;
	gfx_surface_init(&surf, tb, bw, bh, bw);
	font_draw(&surf, f, pad, font_baseline_in_box(f, 0, bh), buf, -1, 0xff000000u | fg);
	for (int j = 0; j < bh; j++) {
		int yy = y + j;

		if (yy < 0 || yy >= h)
			continue;
		for (int i = 0; i < bw; i++) {
			int xx = x + i;

			if (xx >= 0 && xx < w)
				put(px, pitch, fmt, xx, yy, fmt_pack(fmt, tb[j * bw + i]));
		}
	}
}

void frame_text_8x8(void *px, int pitch, int w, int h, uint32_t fmt, int x, int y,
		    const char *text, int scale, uint32_t fg, uint32_t bg)
{
	uint32_t f = fmt_pack(fmt, fg), b = fmt_pack(fmt, bg);
	char s[256];
	int len, bw, bh = 8 * scale + 2 * scale;

	ascii_only(text, s, sizeof(s));
	len = (int)strlen(s);
	bw = len * 8 * scale + 2 * scale;

	for (int j = 0; j < bh; j++) {
		int yy = y + j;

		if (yy < 0 || yy >= h)
			continue;
		for (int i = 0; i < bw; i++) {
			int xx = x + i, gx = i - scale, gy = j - scale;
			bool on = false;

			if (xx < 0 || xx >= w)
				continue;
			if (gx >= 0 && gy >= 0 && gx < len * 8 * scale && gy < 8 * scale) {
				const uint8_t *g = font8x8_glyph((unsigned char)s[gx / (8 * scale)]);
				int col = (gx / scale) % 8, row = gy / scale;

				on = g[row] & (0x80 >> col);
			}
			put(px, pitch, fmt, xx, yy, on ? f : b);
		}
	}
}

uint8_t *frame_thumbnail(const void *src, int w, int h, int pitch, uint32_t fmt,
			 int max_w, int *tw, int *th)
{
	int n = 1, bpp = fmt_bpp(fmt);
	uint8_t *out;

	if (!src || w <= 0 || h <= 0)
		return NULL;
	while (w / n > max_w)
		n++;
	*tw = w / n;
	*th = h / n;
	if (*tw <= 0 || *th <= 0)
		return NULL;
	out = malloc((size_t)*tw * (size_t)*th * 3);
	if (!out)
		return NULL;
	for (int y = 0; y < *th; y++)
		for (int x = 0; x < *tw; x++) {
			unsigned r = 0, g = 0, b = 0;

			for (int j = 0; j < n; j++) {
				const uint8_t *line = (const uint8_t *)src + (size_t)(y * n + j) * (size_t)pitch;

				for (int i = 0; i < n; i++) {
					uint32_t v = fmt_unpack(fmt, line + (size_t)(x * n + i) * (size_t)bpp);

					r += (v >> 16) & 255;
					g += (v >> 8) & 255;
					b += v & 255;
				}
			}
			out[(y * *tw + x) * 3] = (uint8_t)(r / (unsigned)(n * n));
			out[(y * *tw + x) * 3 + 1] = (uint8_t)(g / (unsigned)(n * n));
			out[(y * *tw + x) * 3 + 2] = (uint8_t)(b / (unsigned)(n * n));
		}
	return out;
}
