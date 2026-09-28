/*
 * gfx.c - primitives, blits and image helpers. See gfx.h.
 */
#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../ui/util.h"

/* ------------------------------------------------------------ blending */

/*
 * d' = s + d * (255 - sa) / 255, s premultiplied. Two channels per 32-bit
 * multiply: (d & 0x00ff00ff) holds R and B, (d >> 8) & 0x00ff00ff holds A
 * and G. The "+ 0x80, + (x >> 8), >> 8" sequence is the exact /255 rounding
 * trick. Written as a flat loop over plain arrays so GCC can vectorize it.
 */
static inline uint32_t mul_pixel(uint32_t d, uint32_t m)
{
	uint32_t rb = (d & 0x00ff00ffu) * m + 0x00800080u;
	uint32_t ag = ((d >> 8) & 0x00ff00ffu) * m + 0x00800080u;

	rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
	return rb | ag;
}

static inline uint32_t over(uint32_t s, uint32_t d)
{
	return s + mul_pixel(d, 255u - (s >> 24));
}

static void row_over_run(uint32_t *restrict d, const uint32_t *restrict s, int n)
{
	for (int i = 0; i < n; i++)
		d[i] = over(s[i], d[i]);
}

/* Blends a row of premultiplied pixels over dst. */
static void row_over(uint32_t *restrict d, const uint32_t *restrict s, int n)
{
	int i = 0;

	while (i < n) {
		uint32_t a = s[i] >> 24;

		if (a == 0) {
			/* skip a transparent span */
			i++;
			while (i < n && !(s[i] >> 24))
				i++;
			continue;
		}
		if (a == 255) {
			int j = i + 1;

			while (j < n && (s[j] >> 24) == 255)
				j++;
			memcpy(d + i, s + i, (size_t)(j - i) * 4);
			i = j;
			continue;
		}
		{
			/* a run of translucent pixels: a branch-free loop that GCC
			 * vectorizes (4 pixels per NEON op) */
			int j = i + 1;

			while (j < n && (s[j] >> 24) != 0 && (s[j] >> 24) != 255)
				j++;
			row_over_run(d + i, s + i, j - i);
			i = j;
		}
	}
}

/* Same with a global opacity 1..254. */
static void row_over_alpha(uint32_t *restrict d, const uint32_t *restrict s, int n,
			   uint32_t alpha)
{
	/* branch-free (vectorizable): a transparent pixel blends to d itself */
	for (int i = 0; i < n; i++)
		d[i] = over(mul_pixel(s[i], alpha), d[i]);
}

/* Blends one constant premultiplied color over a row. */
static void row_fill_blend(uint32_t *d, int n, uint32_t sp)
{
	uint32_t ia = 255u - (sp >> 24);

	for (int i = 0; i < n; i++)
		d[i] = sp + mul_pixel(d[i], ia);
}

static inline uint32_t premul(gfx_color c)
{
	uint32_t a = c >> 24;

	if (a == 255)
		return c;
	return (mul_pixel(c | 0xff000000u, a) & 0x00ffffffu) | (a << 24);
}

/* ------------------------------------------------------------ surfaces */
void gfx_surface_init(struct gfx_surface *s, uint32_t *pixels, int w, int h,
		      int stride_px)
{
	s->pixels = pixels;
	s->w = w;
	s->h = h;
	s->stride = stride_px;
	s->clip = (struct gfx_rect){ 0, 0, w, h };
}

void gfx_surface_from_image(struct gfx_surface *s, struct gfx_image *img)
{
	gfx_surface_init(s, img->px, img->w, img->h, img->stride);
}

bool gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b, struct gfx_rect *out)
{
	int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
	int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);

	if (x1 <= x0 || y1 <= y0) {
		*out = (struct gfx_rect){ x0, y0, 0, 0 };
		return false;
	}
	*out = (struct gfx_rect){ x0, y0, x1 - x0, y1 - y0 };
	return true;
}

struct gfx_rect gfx_clip_push(struct gfx_surface *s, struct gfx_rect r)
{
	struct gfx_rect old = s->clip;

	gfx_rect_intersect(old, r, &s->clip);
	return old;
}

void gfx_clip_pop(struct gfx_surface *s, struct gfx_rect old)
{
	s->clip = old;
}

/* Clips (x, y, w, h) to the surface clip; returns false if empty. */
static bool clip_rect(const struct gfx_surface *s, int *x, int *y, int *w, int *h)
{
	struct gfx_rect r;

	if (!gfx_rect_intersect(s->clip, (struct gfx_rect){ *x, *y, *w, *h }, &r))
		return false;
	*x = r.x;
	*y = r.y;
	*w = r.w;
	*h = r.h;
	return true;
}

/* ---------------------------------------------------------- primitives */
void gfx_fill(struct gfx_surface *s, int x, int y, int w, int h, gfx_color c)
{
	uint32_t a = c >> 24;

	if (!a || !clip_rect(s, &x, &y, &w, &h))
		return;
	if (a == 255) {
		for (int j = 0; j < h; j++) {
			uint32_t *d = s->pixels + (size_t)(y + j) * s->stride + x;

			for (int i = 0; i < w; i++)
				d[i] = c;
		}
		return;
	}
	uint32_t sp = premul(c);

	for (int j = 0; j < h; j++)
		row_fill_blend(s->pixels + (size_t)(y + j) * s->stride + x, w, sp);
}

gfx_color gfx_color_mix(gfx_color a, gfx_color b, int t)
{
	uint32_t r = 0;

	for (int sh = 0; sh < 32; sh += 8) {
		int ca = (int)((a >> sh) & 0xff), cb = (int)((b >> sh) & 0xff);
		int v = ca + (((cb - ca) * t) >> 8);

		r |= (uint32_t)CLAMP(v, 0, 255) << sh;
	}
	return r;
}

int gfx_color_luma(gfx_color c)
{
	return (int)((((c >> 16) & 0xff) * 77 + ((c >> 8) & 0xff) * 150 +
		      (c & 0xff) * 29) >> 8);
}

void gfx_fill_gradient(struct gfx_surface *s, int x, int y, int w, int h,
		       gfx_color c0, gfx_color c1, bool horizontal)
{
	int ox = x, oy = y, ow = w, oh = h;

	if (c0 == c1) {
		gfx_fill(s, x, y, w, h, c0);
		return;
	}
	if (!clip_rect(s, &x, &y, &w, &h))
		return;
	if (!horizontal) {
		for (int j = 0; j < h; j++) {
			int t = oh > 1 ? ((y + j - oy) * 256) / (oh - 1) : 0;

			gfx_fill(s, x, y + j, w, 1, gfx_color_mix(c0, c1, t));
		}
		return;
	}
	/* Horizontal: build one premultiplied row, then blend it per line. */
	uint32_t *row = xmalloc((size_t)w * 4);
	bool opaque = true;

	for (int i = 0; i < w; i++) {
		int t = ow > 1 ? ((x + i - ox) * 256) / (ow - 1) : 0;
		gfx_color c = gfx_color_mix(c0, c1, t);

		row[i] = premul(c);
		if ((c >> 24) != 255)
			opaque = false;
	}
	for (int j = 0; j < h; j++) {
		uint32_t *d = s->pixels + (size_t)(y + j) * s->stride + x;

		if (opaque)
			memcpy(d, row, (size_t)w * 4);
		else
			row_over(d, row, w);
	}
	free(row);
}

/* Coverage (0..255) of pixel (px, py) centre for a rounded rect corner. */
static inline int corner_cov(float dx, float dy, float r)
{
	float d = sqrtf(dx * dx + dy * dy) - r;

	if (d <= -0.5f)
		return 255;
	if (d >= 0.5f)
		return 0;
	return (int)((0.5f - d) * 255.0f + 0.5f);
}

/* Draws a rounded rect as coverage spans; inner radius for strokes. */
static void round_rect(struct gfx_surface *s, int x, int y, int w, int h,
		       int r, int thick, gfx_color c)
{
	uint32_t ca = c >> 24;
	float fr = (float)r, fr2 = (float)(r - thick);

	if (w <= 0 || h <= 0 || !ca)
		return;
	r = MIN(r, MIN(w, h) / 2);
	fr = (float)r;
	fr2 = (float)r - (float)thick;
	for (int j = 0; j < h; j++) {
		int py = y + j;

		if (py < s->clip.y || py >= s->clip.y + s->clip.h)
			continue;
		uint32_t *d = s->pixels + (size_t)py * s->stride;
		/* distance from the vertical centre band */
		float cy = (float)j + 0.5f;
		float dy = 0;

		if (cy < fr)
			dy = fr - cy;
		else if (cy > (float)h - fr)
			dy = cy - ((float)h - fr);
		for (int i = 0; i < w; i++) {
			int px = x + i;
			float cx = (float)i + 0.5f;
			float dx = 0;
			int cov;

			if (px < s->clip.x || px >= s->clip.x + s->clip.w)
				continue;
			if (cx < fr)
				dx = fr - cx;
			else if (cx > (float)w - fr)
				dx = cx - ((float)w - fr);
			if (dx > 0 && dy > 0)
				cov = corner_cov(dx, dy, fr);
			else
				cov = 255;
			if (thick > 0 && cov) {
				/* subtract the inner shape */
				float ix = (float)MIN(i, w - 1 - i) + 0.5f;
				float iy = (float)MIN(j, h - 1 - j) + 0.5f;
				int inner;

				if (ix < (float)thick || iy < (float)thick) {
					inner = 0;
				} else if (fr2 > 0 && dx > 0 && dy > 0) {
					inner = corner_cov(dx, dy, fr2);
				} else {
					inner = 255;
					if (ix < (float)thick + 1.0f && ix >= (float)thick)
						inner = (int)((ix - (float)thick) * 255.0f);
					if (iy < (float)thick + 1.0f && iy >= (float)thick)
						inner = MIN(inner, (int)((iy - (float)thick) * 255.0f));
				}
				cov = cov * (255 - inner) / 255;
			}
			if (!cov)
				continue;
			uint32_t a = (ca * (uint32_t)cov + 127) / 255;

			if (a == 255)
				d[px] = c | 0xff000000u;
			else if (a)
				d[px] = over(premul((c & 0xffffffu) | (a << 24)), d[px]);
		}
	}
}

void gfx_fill_round(struct gfx_surface *s, int x, int y, int w, int h,
		    int radius, gfx_color c)
{
	if (radius <= 0) {
		gfx_fill(s, x, y, w, h, c);
		return;
	}
	radius = MIN(radius, MIN(w, h) / 2);
	/* Middle band without corners: plain fill (fast). */
	gfx_fill(s, x, y + radius, w, h - 2 * radius, c);
	/* Top and bottom bands with corners. */
	{
		struct gfx_rect old = gfx_clip_push(s, (struct gfx_rect){ x, y, w, radius });

		round_rect(s, x, y, w, h, radius, 0, c);
		gfx_clip_pop(s, old);
		old = gfx_clip_push(s, (struct gfx_rect){ x, y + h - radius, w, radius });
		round_rect(s, x, y, w, h, radius, 0, c);
		gfx_clip_pop(s, old);
	}
}

void gfx_stroke_round(struct gfx_surface *s, int x, int y, int w, int h,
		      int radius, int thick, gfx_color c)
{
	if (thick <= 0)
		return;
	round_rect(s, x, y, w, h, radius, thick, c);
}

void gfx_fill_circle(struct gfx_surface *s, int cx, int cy, int r, gfx_color c)
{
	gfx_fill_round(s, cx - r, cy - r, 2 * r, 2 * r, r, c);
}

void gfx_mask(struct gfx_surface *s, const uint8_t *mask, int mstride,
	      int x, int y, int w, int h, gfx_color c)
{
	int x0 = x, y0 = y;
	uint32_t ca = c >> 24;
	uint32_t rgb = c & 0x00ffffffu;

	if (!ca || !clip_rect(s, &x, &y, &w, &h))
		return;
	mask += (size_t)(y - y0) * mstride + (x - x0);
	for (int j = 0; j < h; j++) {
		uint32_t *d = s->pixels + (size_t)(y + j) * s->stride + x;
		const uint8_t *m = mask + (size_t)j * mstride;

		for (int i = 0; i < w; i++) {
			uint32_t a = m[i];

			if (!a)
				continue;
			if (ca != 255)
				a = (a * ca + 127) / 255;
			if (a == 255) {
				d[i] = rgb | 0xff000000u;
			} else {
				uint32_t sp = mul_pixel(rgb | 0xff000000u, a);

				d[i] = sp + mul_pixel(d[i], 255u - a);
			}
		}
	}
}

/* --------------------------------------------------------------- blits */
void gfx_blit_sub(struct gfx_surface *s, const struct gfx_image *img,
		  struct gfx_rect src, int x, int y, int alpha)
{
	int w = src.w, h = src.h;
	int dx0 = x, dy0 = y;

	if (!img || alpha <= 0)
		return;
	if (!clip_rect(s, &x, &y, &w, &h))
		return;
	src.x += x - dx0;
	src.y += y - dy0;
	for (int j = 0; j < h; j++) {
		uint32_t *d = s->pixels + (size_t)(y + j) * s->stride + x;
		const uint32_t *sp = img->px + (size_t)(src.y + j) * img->stride + src.x;

		if (alpha >= 255) {
			if (img->flags & GFX_IMG_OPAQUE)
				memcpy(d, sp, (size_t)w * 4);
			else
				row_over(d, sp, w);
		} else {
			row_over_alpha(d, sp, w, (uint32_t)alpha);
		}
	}
}

void gfx_blit(struct gfx_surface *s, const struct gfx_image *img, int x, int y,
	      int alpha)
{
	if (!img)
		return;
	gfx_blit_sub(s, img, (struct gfx_rect){ 0, 0, img->w, img->h }, x, y, alpha);
}

void gfx_blit_scaled(struct gfx_surface *s, const struct gfx_image *img,
		     int x, int y, int w, int h, int alpha)
{
	int cx = x, cy = y, cw = w, ch = h;
	uint32_t *row;

	if (!img || w <= 0 || h <= 0 || alpha <= 0)
		return;
	if (w == img->w && h == img->h) {
		gfx_blit(s, img, x, y, alpha);
		return;
	}
	if (!clip_rect(s, &cx, &cy, &cw, &ch))
		return;
	uint32_t fx = (uint32_t)(((uint64_t)img->w << 16) / (unsigned)w);
	uint32_t fy = (uint32_t)(((uint64_t)img->h << 16) / (unsigned)h);

	row = xmalloc((size_t)cw * 4);
	for (int j = 0; j < ch; j++) {
		int sy = (int)(((uint32_t)(cy + j - y) * fy + fy / 2) >> 16);
		const uint32_t *sr = img->px + (size_t)MIN(sy, img->h - 1) * img->stride;
		uint32_t *d = s->pixels + (size_t)(cy + j) * s->stride + cx;
		uint32_t sx = (uint32_t)(cx - x) * fx + fx / 2;

		for (int i = 0; i < cw; i++, sx += fx)
			row[i] = sr[MIN((int)(sx >> 16), img->w - 1)];
		if (alpha >= 255)
			row_over(d, row, cw);
		else
			row_over_alpha(d, row, cw, (uint32_t)alpha);
	}
	free(row);
}

void gfx_blit_tiled(struct gfx_surface *s, const struct gfx_image *img,
		    int x, int y, int w, int h, int alpha)
{
	struct gfx_rect old;

	if (!img || img->w <= 0 || img->h <= 0)
		return;
	old = gfx_clip_push(s, (struct gfx_rect){ x, y, w, h });
	for (int ty = y; ty < y + h; ty += img->h) {
		if (ty + img->h <= s->clip.y || ty >= s->clip.y + s->clip.h)
			continue;
		for (int tx = x; tx < x + w; tx += img->w)
			gfx_blit(s, img, tx, ty, alpha);
	}
	gfx_clip_pop(s, old);
}

/* Scales the src sub-rect into dst rect (nearest). */
static void blit_part(struct gfx_surface *s, const struct gfx_image *img,
		      struct gfx_rect src, struct gfx_rect dst)
{
	struct gfx_image sub;

	if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
		return;
	sub = *img;
	sub.px = img->px + (size_t)src.y * img->stride + src.x;
	sub.w = src.w;
	sub.h = src.h;
	gfx_blit_scaled(s, &sub, dst.x, dst.y, dst.w, dst.h, 255);
}

void gfx_ninepatch(struct gfx_surface *s, const struct gfx_image *img,
		   int x, int y, int w, int h, int corner)
{
	int c = corner, iw, ih;

	if (!img)
		return;
	c = MIN(c, MIN(img->w, img->h) / 2);
	c = MIN(c, MIN(w, h) / 2);
	iw = img->w - 2 * c;
	ih = img->h - 2 * c;
	/* corners */
	gfx_blit_sub(s, img, (struct gfx_rect){ 0, 0, c, c }, x, y, 255);
	gfx_blit_sub(s, img, (struct gfx_rect){ img->w - c, 0, c, c }, x + w - c, y, 255);
	gfx_blit_sub(s, img, (struct gfx_rect){ 0, img->h - c, c, c }, x, y + h - c, 255);
	gfx_blit_sub(s, img, (struct gfx_rect){ img->w - c, img->h - c, c, c },
		     x + w - c, y + h - c, 255);
	/* edges */
	blit_part(s, img, (struct gfx_rect){ c, 0, iw, c },
		  (struct gfx_rect){ x + c, y, w - 2 * c, c });
	blit_part(s, img, (struct gfx_rect){ c, img->h - c, iw, c },
		  (struct gfx_rect){ x + c, y + h - c, w - 2 * c, c });
	blit_part(s, img, (struct gfx_rect){ 0, c, c, ih },
		  (struct gfx_rect){ x, y + c, c, h - 2 * c });
	blit_part(s, img, (struct gfx_rect){ img->w - c, c, c, ih },
		  (struct gfx_rect){ x + w - c, y + c, c, h - 2 * c });
	/* center */
	blit_part(s, img, (struct gfx_rect){ c, c, iw, ih },
		  (struct gfx_rect){ x + c, y + c, w - 2 * c, h - 2 * c });
}

/* --------------------------------------------------------------- images */
struct gfx_image *gfx_image_new(int w, int h)
{
	struct gfx_image *img = xcalloc(1, sizeof(*img));

	w = MAX(w, 1);
	h = MAX(h, 1);
	img->w = w;
	img->h = h;
	img->stride = w;
	img->px = xcalloc((size_t)w * h, 4);
	return img;
}

void gfx_image_free(struct gfx_image *img)
{
	if (!img)
		return;
	if (img->flags & GFX_IMG_MMAPPED)
		munmap(img->map, img->map_len);
	else if (!(img->flags & GFX_IMG_EXTERNAL))
		free(img->px);
	free(img);
}

void gfx_image_update_flags(struct gfx_image *img)
{
	bool opaque = true;

	for (int j = 0; j < img->h && opaque; j++) {
		const uint32_t *p = img->px + (size_t)j * img->stride;

		for (int i = 0; i < img->w; i++) {
			if ((p[i] >> 24) != 255) {
				opaque = false;
				break;
			}
		}
	}
	if (opaque)
		img->flags |= GFX_IMG_OPAQUE;
	else
		img->flags &= ~GFX_IMG_OPAQUE;
}

void gfx_image_from_rgba(struct gfx_image *img)
{
	for (int j = 0; j < img->h; j++) {
		uint32_t *p = img->px + (size_t)j * img->stride;
		const uint8_t *b = (const uint8_t *)p;

		for (int i = 0; i < img->w; i++) {
			uint32_t r = b[4 * i], g = b[4 * i + 1], bl = b[4 * i + 2],
				 a = b[4 * i + 3];
			uint32_t c = (a << 24) | (r << 16) | (g << 8) | bl;

			p[i] = a == 255 ? c : a == 0 ? 0 :
				(mul_pixel(c | 0xff000000u, a) & 0x00ffffffu) | (a << 24);
		}
	}
	gfx_image_update_flags(img);
}

void gfx_image_tint(struct gfx_image *img, gfx_color c)
{
	uint32_t tr = (c >> 16) & 0xff, tg = (c >> 8) & 0xff, tb = c & 0xff,
		 ta = c >> 24;

	if (c == 0xffffffffu)
		return;
	for (int j = 0; j < img->h; j++) {
		uint32_t *p = img->px + (size_t)j * img->stride;

		for (int i = 0; i < img->w; i++) {
			uint32_t v = p[i];
			uint32_t a = v >> 24, r = (v >> 16) & 0xff, g = (v >> 8) & 0xff,
				 b = v & 0xff;

			a = (a * ta + 127) / 255;
			r = (r * tr * ta + 32512) / 65025;
			g = (g * tg * ta + 32512) / 65025;
			b = (b * tb * ta + 32512) / 65025;
			p[i] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	gfx_image_update_flags(img);
}

/*
 * Separable resampler. For each output coordinate, a tent filter whose
 * radius is max(1, scale) gives an area-like average when shrinking and a
 * bilinear interpolation when enlarging. Weights are 14-bit fixed point.
 */
struct taps {
	int *start;      /* first source index per output pixel */
	int *count;
	int16_t *w;      /* count weights per output pixel, packed */
	int *off;
};

static void make_taps(struct taps *t, int src, int dst)
{
	float scale = (float)src / (float)dst;
	float radius = scale > 1.0f ? scale : 1.0f;
	int maxc = (int)ceilf(radius * 2.0f) + 2;
	int total = 0;

	t->start = xmalloc(sizeof(int) * (size_t)dst);
	t->count = xmalloc(sizeof(int) * (size_t)dst);
	t->off = xmalloc(sizeof(int) * (size_t)dst);
	t->w = xmalloc(sizeof(int16_t) * (size_t)dst * (size_t)maxc);
	for (int o = 0; o < dst; o++) {
		float c = ((float)o + 0.5f) * scale - 0.5f;
		int s0 = (int)floorf(c - radius) + 1, s1 = (int)floorf(c + radius);
		float wsum = 0;
		float wv[256];
		int n = 0, isum = 0;

		if (s1 - s0 + 1 > 256)
			s1 = s0 + 255;
		for (int si = s0; si <= s1; si++) {
			float d = fabsf((float)si - c) / radius;
			float wt = d < 1.0f ? 1.0f - d : 0.0f;

			wv[n++] = wt;
			wsum += wt;
		}
		if (wsum <= 0) {
			wv[0] = 1;
			wsum = 1;
			n = 1;
			s0 = (int)floorf(c + 0.5f);
		}
		/* clamp to the edges by folding the weights */
		t->start[o] = CLAMP(s0, 0, src - 1);
		t->off[o] = total;
		{
			int first = t->start[o];
			int last = CLAMP(s0 + n - 1, 0, src - 1);
			int cnt = last - first + 1;
			int16_t *w = t->w + total;

			memset(w, 0, sizeof(int16_t) * (size_t)cnt);
			for (int k = 0; k < n; k++) {
				int si = CLAMP(s0 + k, 0, src - 1);
				int iw = (int)(wv[k] / wsum * 16384.0f + 0.5f);

				w[si - first] = (int16_t)(w[si - first] + iw);
				isum += iw;
			}
			/* put the rounding error on the largest tap */
			{
				int best = 0;

				for (int k = 1; k < cnt; k++)
					if (w[k] > w[best])
						best = k;
				w[best] = (int16_t)(w[best] + 16384 - isum);
			}
			t->count[o] = cnt;
			total += cnt;
		}
	}
}

static void free_taps(struct taps *t)
{
	free(t->start);
	free(t->count);
	free(t->w);
	free(t->off);
}

struct gfx_image *gfx_image_scale(const struct gfx_image *src, int w, int h)
{
	struct gfx_image *dst;
	struct taps tx, ty;
	int32_t *tmp;  /* src->h rows x w columns x 4 channels, 8.? fixed */

	if (w <= 0 || h <= 0)
		return NULL;
	dst = gfx_image_new(w, h);
	if (w == src->w && h == src->h) {
		for (int j = 0; j < h; j++)
			memcpy(dst->px + (size_t)j * dst->stride,
			       src->px + (size_t)j * src->stride, (size_t)w * 4);
		dst->flags = src->flags & GFX_IMG_OPAQUE;
		return dst;
	}
	make_taps(&tx, src->w, w);
	make_taps(&ty, src->h, h);
	tmp = xmalloc(sizeof(int32_t) * 4 * (size_t)w * (size_t)src->h);
	/* horizontal pass: channels scaled by 16384 >> 6 = keeps 8 extra bits */
	for (int j = 0; j < src->h; j++) {
		const uint32_t *sr = src->px + (size_t)j * src->stride;
		int32_t *tr = tmp + (size_t)j * w * 4;

		for (int o = 0; o < w; o++) {
			const int16_t *wt = tx.w + tx.off[o];
			const uint32_t *sp = sr + tx.start[o];
			int32_t a = 0, r = 0, g = 0, b = 0;

			for (int k = 0; k < tx.count[o]; k++) {
				uint32_t p = sp[k];
				int32_t wk = wt[k];

				a += (int32_t)(p >> 24) * wk;
				r += (int32_t)((p >> 16) & 0xff) * wk;
				g += (int32_t)((p >> 8) & 0xff) * wk;
				b += (int32_t)(p & 0xff) * wk;
			}
			tr[4 * o] = a >> 6;
			tr[4 * o + 1] = r >> 6;
			tr[4 * o + 2] = g >> 6;
			tr[4 * o + 3] = b >> 6;
		}
	}
	/* vertical pass */
	for (int o = 0; o < h; o++) {
		const int16_t *wt = ty.w + ty.off[o];
		uint32_t *dr = dst->px + (size_t)o * dst->stride;

		for (int i = 0; i < w; i++) {
			int32_t a = 0, r = 0, g = 0, b = 0;

			for (int k = 0; k < ty.count[o]; k++) {
				const int32_t *tp = tmp + ((size_t)(ty.start[o] + k) * w + i) * 4;
				int32_t wk = wt[k];

				a += tp[0] * wk;
				r += tp[1] * wk;
				g += tp[2] * wk;
				b += tp[3] * wk;
			}
			/* horizontal pass left c * 2^8, vertical weights sum to
			 * 2^14: total c * 2^22 (fits in int32 for c <= 255) */
			int32_t A = (a + (1 << 21)) >> 22;
			int32_t R = (r + (1 << 21)) >> 22;
			int32_t G = (g + (1 << 21)) >> 22;
			int32_t B = (b + (1 << 21)) >> 22;

			A = CLAMP(A, 0, 255);
			R = CLAMP(R, 0, A);
			G = CLAMP(G, 0, A);
			B = CLAMP(B, 0, A);
			dr[i] = ((uint32_t)A << 24) | ((uint32_t)R << 16) |
				((uint32_t)G << 8) | (uint32_t)B;
		}
	}
	free(tmp);
	free_taps(&tx);
	free_taps(&ty);
	gfx_image_update_flags(dst);
	return dst;
}

/* ---------------------------------------------------------------- color */
static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool gfx_parse_color(const char *s, gfx_color *out)
{
	uint32_t v = 0;
	int n = 0;

	if (!s)
		return false;
	while (*s == ' ' || *s == '#')
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (s[n] && hexval(s[n]) >= 0)
		n++;
	if (n != 6 && n != 8)
		return false;
	for (int i = 0; i < n; i++)
		v = (v << 4) | (uint32_t)hexval(s[i]);
	if (n == 6)
		*out = 0xff000000u | v;
	else
		*out = (v >> 8) | (v << 24);
	return true;
}

void gfx_mask_erase(struct gfx_surface *s, const uint8_t *mask, int mstride,
		    int x, int y, int w, int h)
{
	int x0 = x, y0 = y;

	if (!clip_rect(s, &x, &y, &w, &h))
		return;
	mask += (size_t)(y - y0) * mstride + (x - x0);
	for (int j = 0; j < h; j++) {
		uint32_t *d = s->pixels + (size_t)(y + j) * s->stride + x;
		const uint8_t *m = mask + (size_t)j * mstride;

		for (int i = 0; i < w; i++)
			if (m[i])
				d[i] = mul_pixel(d[i], 255u - m[i]);
	}
}
