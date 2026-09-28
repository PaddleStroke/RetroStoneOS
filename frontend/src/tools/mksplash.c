/*
 * mksplash - build-time tool (runs on the build host): turns the console
 * logo (PNG with alpha) into /usr/share/rsos/splash.rle, the boot logo
 * rsos-frontend shows right after display_init() (format: src/splash.h).
 *
 *   mksplash [options] LOGO.png
 *     -o FILE.rle          output (splash file)
 *     --png FILE.png       also write the frame as a PNG (previews)
 *     --bg RRGGBB          background colour (default 2a2a35)
 *     --size WxH           frame size (default 640x480)
 *     --width-pct P        logo width, % of the frame width (default 85)
 *     --center-y-pct P     logo centre, % of the frame height (default 45)
 *     --smooth             area-average resampling with blended edges
 *                          (for logos that are not pixel art)
 *     --bench N            time N decodes of the result
 *     -q                   quiet
 *
 * Pixel art (the default). The RetroStone2 logo is a small drawing enlarged
 * about 8x with blended seams, and not on one exact grid (its edge phases
 * cohere only ~40 %), so rebuilding the original pixels is not reliable
 * and plain resampling by 0.85 would smear every edge. Instead:
 *   1. the logo's colours are reduced to its main colours (clusters that
 *      cover a real area; seam blends and noise map to the nearest one),
 *      alpha thresholded at 50 %;
 *   2. the scale is picked so one logo pixel (the dominant edge pitch,
 *      ~8.16 px here) becomes a whole number of screen pixels, the one
 *      closest to --width-pct (7: the logo is 553 px wide, 86 % of 640);
 *   3. every screen pixel takes the colour (or transparency) that covers
 *      most of its footprint in the source (a majority vote, not an
 *      average): edges stay one sharp step, no new colours appear, and
 *      the rows of each logo pixel mostly repeat, which the RLE stores as
 *      one byte each.
 * The frame is checked by decoding it again with the frontend's decoder
 * (src/splash.c).
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../splash.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../../third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop

#define MAXPAL 256
#define TRANSPARENT 0xffu   /* palette index for "no logo here" in the vote */

static bool quiet;

#define info(...) do { if (!quiet) fprintf(stderr, "mksplash: " __VA_ARGS__); } while (0)

static void die(const char *msg)
{
	fprintf(stderr, "mksplash: %s\n", msg);
	exit(1);
}

static void *xcalloc(size_t n, size_t sz)
{
	void *p = calloc(n ? n : 1, sz ? sz : 1);

	if (!p)
		die("out of memory");
	return p;
}

struct rgba {
	uint8_t *p;     /* straight RGBA */
	int w, h;
};

static const uint8_t *px(const struct rgba *im, int x, int y)
{
	return im->p + 4 * ((size_t)y * im->w + x);
}

static uint32_t rgb_of(const uint8_t *q)
{
	return (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
}

static int cdist(uint32_t a, uint32_t b)
{
	return abs((int)(a >> 16 & 255) - (int)(b >> 16 & 255)) + abs((int)(a >> 8 & 255) - (int)(b >> 8 & 255)) +
	       abs((int)(a & 255) - (int)(b & 255));
}

/* Colour difference of two straight-alpha pixels, premultiplied. */
static int pdiff(const uint8_t *a, const uint8_t *b)
{
	int d = abs(a[3] - b[3]);

	for (int i = 0; i < 3; i++)
		d += abs(a[i] * a[3] / 255 - b[i] * b[3] / 255);
	return d;
}

/* ------------------------------------------------------------ edge pitch */
/*
 * The dominant spacing of colour edges along x: the pitch whose complex
 * sum of edge phases is the most coherent. Every divisor of the true
 * pitch is as coherent, so the largest pitch within 85 % of the best wins.
 */
static double edge_pitch(const struct rgba *im, double *coherence)
{
	double *hist = xcalloc((size_t)im->w, sizeof(double)), total = 0, cmax = 0, best = 0;
	const double pmin = 3, pmax = 24, step = 0.001;
	int steps = (int)((pmax - pmin) / step);
	double *coh = xcalloc((size_t)steps + 1, sizeof(double));

	for (int y = 0; y < im->h; y++)
		for (int x = 1; x < im->w; x++)
			if (pdiff(px(im, x, y), px(im, x - 1, y)) > 90) {
				hist[x]++;
				total++;
			}
	*coherence = 0;
	if (total > 0) {
		for (int s = 0; s <= steps; s++) {
			double p = pmin + s * step, re = 0, imag = 0;

			for (int i = 0; i < im->w; i++)
				if (hist[i]) {
					re += hist[i] * cos(2 * M_PI * i / p);
					imag += hist[i] * sin(2 * M_PI * i / p);
				}
			coh[s] = sqrt(re * re + imag * imag) / total;
			if (coh[s] > cmax)
				cmax = coh[s];
		}
		for (int s = steps; s >= 0; s--)
			if (coh[s] >= 0.85 * cmax) {
				int b = s;

				for (int k = s; k >= 0 && k > s - 300; k--)
					if (coh[k] > coh[b])
						b = k;
				best = pmin + b * step;
				*coherence = coh[b];
				break;
			}
	}
	free(coh);
	free(hist);
	return best;
}

/* ---------------------------------------------------------------- palette */
struct pal {
	uint32_t c[MAXPAL];       /* XRGB */
	double sum[MAXPAL][3];
	long cnt[MAXPAL];
	int n;
};

/* Nearest entry from `first`; its distance in *d. */
static int pal_nearest(const struct pal *pl, uint32_t c, int first, int *d)
{
	int best = -1, bd = 1 << 30;

	for (int i = first; i < pl->n; i++) {
		int e = cdist(pl->c[i], c);

		if (e < bd) {
			bd = e;
			best = i;
		}
	}
	if (d)
		*d = bd;
	return best;
}

/* Adds c to the cluster within tol (from index `first`), or a new one; -1 if full. */
static int pal_add(struct pal *pl, uint32_t c, int tol, int first, long weight)
{
	int d, i = pal_nearest(pl, c, first, &d);

	if (i < 0 || d > tol) {
		if (pl->n >= MAXPAL)
			return -1;
		i = pl->n++;
		pl->c[i] = c;
		memset(pl->sum[i], 0, sizeof(pl->sum[i]));
		pl->cnt[i] = 0;
	}
	pl->sum[i][0] += (double)(c >> 16 & 255) * weight;
	pl->sum[i][1] += (double)(c >> 8 & 255) * weight;
	pl->sum[i][2] += (double)(c & 255) * weight;
	pl->cnt[i] += weight;
	return i;
}

static uint32_t cluster_mean(const struct pal *pl, int i)
{
	return (uint32_t)lround(pl->sum[i][0] / pl->cnt[i]) << 16 | (uint32_t)lround(pl->sum[i][1] / pl->cnt[i]) << 8 |
	       (uint32_t)lround(pl->sum[i][2] / pl->cnt[i]);
}

/* ----------------------------------------------------------------- logos */
/* The logo at its final size: palette indices (TRANSPARENT = none), or
 * straight colours + alpha for --smooth. */
struct logo {
	int w, h;
	uint8_t *idx;
	uint32_t *col;
	uint8_t *alpha;
};

/* Pixel art: main colours, then a per-pixel majority vote. */
static void scale_majority(const struct rgba *im, int w, int h, struct pal *major, struct logo *out)
{
	struct pal all;
	uint8_t *src = xcalloc((size_t)im->w * im->h, 1);
	long opaque = 0;
	double fx = (double)im->w / w, fy = (double)im->h / h;

	/* 1. colour clusters of the opaque pixels */
	memset(&all, 0, sizeof(all));
	for (int y = 0; y < im->h; y++)
		for (int x = 0; x < im->w; x++) {
			const uint8_t *q = px(im, x, y);

			if (q[3] >= 128) {
				opaque++;
				if (pal_add(&all, rgb_of(q), 40, 0, 1) < 0)
					die("too many colours for pixel art: use --smooth");
			}
		}
	/* 2. main colours: clusters covering a real area */
	memset(major, 0, sizeof(*major));
	for (int i = 0; i < all.n; i++)
		if (all.cnt[i] >= 20 && all.cnt[i] * 1000 >= opaque) {
			major->c[major->n] = cluster_mean(&all, i);
			major->cnt[major->n++] = all.cnt[i];
		}
	if (!major->n)
		die("the logo has no opaque area");
	for (int y = 0; y < im->h; y++)
		for (int x = 0; x < im->w; x++) {
			const uint8_t *q = px(im, x, y);

			src[(size_t)y * im->w + x] = q[3] >= 128 ? (uint8_t)pal_nearest(major, rgb_of(q), 0, NULL)
								 : TRANSPARENT;
		}
	/* 3. majority vote over each destination pixel's footprint */
	out->w = w;
	out->h = h;
	out->idx = xcalloc((size_t)w * h, 1);
	for (int y = 0; y < h; y++) {
		double y0 = y * fy, y1 = y0 + fy;

		for (int x = 0; x < w; x++) {
			double x0 = x * fx, x1 = x0 + fx, votes[MAXPAL] = { 0 }, vt = 0;
			int best = TRANSPARENT;

			for (int sy = (int)y0; sy < (int)ceil(y1) && sy < im->h; sy++) {
				double wy = fmin(y1, sy + 1) - fmax(y0, sy);

				for (int sx = (int)x0; sx < (int)ceil(x1) && sx < im->w; sx++) {
					double wgt = wy * (fmin(x1, sx + 1) - fmax(x0, sx));
					uint8_t k = src[(size_t)sy * im->w + sx];

					if (k == TRANSPARENT)
						vt += wgt;
					else
						votes[k] += wgt;
				}
			}
			for (int k = 0; k < major->n; k++)
				if (votes[k] > vt && (best == TRANSPARENT || votes[k] > votes[best]))
					best = k;
			out->idx[(size_t)y * w + x] = (uint8_t)best;
		}
	}
	free(src);
}

/* Area-average resampling (straight colour from premultiplied sums). */
static void scale_smooth(const struct rgba *im, int w, int h, struct logo *out)
{
	double fx = (double)im->w / w, fy = (double)im->h / h;

	out->w = w;
	out->h = h;
	out->col = xcalloc((size_t)w * h, 4);
	out->alpha = xcalloc((size_t)w * h, 1);
	for (int y = 0; y < h; y++) {
		double y0 = y * fy, y1 = y0 + fy;

		for (int x = 0; x < w; x++) {
			double x0 = x * fx, x1 = x0 + fx, s[4] = { 0, 0, 0, 0 }, area = 0;

			for (int sy = (int)y0; sy < (int)ceil(y1) && sy < im->h; sy++) {
				double wy = fmin(y1, sy + 1) - fmax(y0, sy);

				for (int sx = (int)x0; sx < (int)ceil(x1) && sx < im->w; sx++) {
					double wgt = wy * (fmin(x1, sx + 1) - fmax(x0, sx));
					const uint8_t *q = px(im, sx, sy);

					s[0] += wgt * q[0] * q[3];
					s[1] += wgt * q[1] * q[3];
					s[2] += wgt * q[2] * q[3];
					s[3] += wgt * q[3];
					area += wgt;
				}
			}
			if (s[3] > 0)
				out->col[(size_t)y * w + x] = (uint32_t)lround(s[0] / s[3]) << 16 |
							      (uint32_t)lround(s[1] / s[3]) << 8 |
							      (uint32_t)lround(s[2] / s[3]);
			out->alpha[(size_t)y * w + x] = (uint8_t)lround(area > 0 ? s[3] / area : 0);
		}
	}
}

/* ----------------------------------------------------------------- output */
static void put16(uint8_t **p, unsigned v)
{
	(*p)[0] = (uint8_t)v;
	(*p)[1] = (uint8_t)(v >> 8);
	*p += 2;
}

static void put32(uint8_t **p, uint32_t v)
{
	put16(p, v & 0xffff);
	put16(p, v >> 16);
}

/* idx: W x H palette indices. Returns the encoded size (buffer malloc'ed). */
static size_t encode(const uint8_t *idx, int W, int H, const struct pal *pl, uint32_t bg, uint8_t **out)
{
	size_t cap = SPLASH_HEADER + 4 * MAXPAL + (size_t)H * (3 + 3 * (size_t)W);
	uint8_t *buf = xcalloc(cap, 1), *p = buf;

	memcpy(p, SPLASH_MAGIC, 4);
	p += 4;
	put16(&p, SPLASH_VERSION);
	put16(&p, (unsigned)W);
	put16(&p, (unsigned)H);
	put16(&p, (unsigned)pl->n);
	put32(&p, bg);
	for (int i = 0; i < pl->n; i++)
		put32(&p, pl->c[i]);
	for (int y = 0; y < H; y++) {
		const uint8_t *r = idx + (size_t)y * W, *u = r - W;
		unsigned nruns = 0, nspans = 0;
		uint8_t *nr;

		if (y > 0 && !memcmp(r, u, (size_t)W)) {
			*p++ = 0;
			continue;
		}
		/* cost of both encodings: full runs (2 B each) or the changed
		 * spans against the row above (4 B each) */
		for (int x = 0; x < W;) {
			int e = x;

			while (e < W && r[e] == r[x] && e - x < 255)
				e++;
			nruns++;
			x = e;
		}
		for (int x = 0; y > 0 && x < W;) {
			int e;

			if (r[x] == u[x]) {
				x++;
				continue;
			}
			for (e = x; e < W && r[e] == r[x] && e - x < 255; e++)
				;
			nspans++;
			x = e;
		}
		if (y > 0 && 4 * nspans < 2 * nruns) {
			*p++ = 2;
			put16(&p, nspans);
			for (int x = 0; x < W;) {
				int e;

				if (r[x] == u[x]) {
					x++;
					continue;
				}
				for (e = x; e < W && r[e] == r[x] && e - x < 255; e++)
					;
				put16(&p, (unsigned)x);
				*p++ = (uint8_t)(e - x);
				*p++ = r[x];
				x = e;
			}
			continue;
		}
		*p++ = 1;
		nr = p;
		p += 2;
		for (int x = 0; x < W;) {
			int e = x;

			while (e < W && r[e] == r[x] && e - x < 255)
				e++;
			*p++ = (uint8_t)(e - x);
			*p++ = r[x];
			x = e;
		}
		put16(&nr, nruns);
	}
	*out = buf;
	return (size_t)(p - buf);
}

static uint32_t parse_rgb(const char *s)
{
	char *e;
	unsigned long v;

	if (*s == '#')
		s++;
	v = strtoul(s, &e, 16);
	if (*e || strlen(s) != 6)
		die("colours are RRGGBB");
	return (uint32_t)v;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(void)
{
	die("usage: mksplash [-o OUT.rle] [--png OUT.png] [--bg RRGGBB] [--size WxH] [--width-pct P] "
	    "[--center-y-pct P] [--smooth] [--bench N] [-q] LOGO.png");
}

int main(int argc, char **argv)
{
	const char *in = NULL, *out = NULL, *png = NULL;
	uint32_t bg = 0x2a2a35;
	int W = 640, H = 480, bench = 0, n, x0, y0, x1, y1, lw, lh, ox, oy;
	double width_pct = 85, cy_pct = 45, pitch, coh, f;
	bool smooth = false;
	struct rgba full, im;
	struct logo lg;
	struct pal pl, major;
	uint8_t *idx, *enc;
	uint32_t *frame;
	size_t size;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
		if (OPT("-o"))
			out = v;
		else if (OPT("--png"))
			png = v;
		else if (OPT("--bg"))
			bg = parse_rgb(v);
		else if (OPT("--size")) {
			if (sscanf(v, "%dx%d", &W, &H) != 2 || W < 16 || H < 16 || W > 4096 || H > 4096)
				die("bad --size");
		} else if (OPT("--width-pct"))
			width_pct = atof(v);
		else if (OPT("--center-y-pct"))
			cy_pct = atof(v);
		else if (OPT("--bench"))
			bench = atoi(v);
		else if (!strcmp(a, "--smooth"))
			smooth = true;
		else if (!strcmp(a, "-q"))
			quiet = true;
		else if (a[0] != '-' && !in)
			in = a;
		else
			usage();
#undef OPT
	}
	if (!in || (!out && !png) || width_pct <= 0 || width_pct > 100)
		usage();
	full.p = stbi_load(in, &full.w, &full.h, &n, 4);
	if (!full.p)
		die("cannot read the logo");
	info("%s: %dx%d\n", in, full.w, full.h);

	/* crop the transparent border */
	x0 = full.w;
	y0 = full.h;
	x1 = y1 = 0;
	for (int y = 0; y < full.h; y++)
		for (int x = 0; x < full.w; x++)
			if (px(&full, x, y)[3] >= 128) {
				if (x < x0) x0 = x;
				if (y < y0) y0 = y;
				if (x + 1 > x1) x1 = x + 1;
				if (y + 1 > y1) y1 = y + 1;
			}
	if (x1 <= x0)
		die("the logo is fully transparent");
	im.w = x1 - x0;
	im.h = y1 - y0;
	im.p = xcalloc((size_t)im.w * im.h, 4);
	for (int y = 0; y < im.h; y++)
		memcpy(im.p + (size_t)y * im.w * 4, px(&full, x0, y + y0), (size_t)im.w * 4);
	stbi_image_free(full.p);

	/* scale: whole screen pixels per logo pixel when the edges show a pitch */
	f = width_pct / 100.0 * W / im.w;
	pitch = edge_pitch(&im, &coh);
	if (!smooth && pitch >= 4 && coh >= 0.25) {
		long np = lround(f * pitch);

		if (np >= 1)
			f = (double)np / pitch;
		info("edge pitch %.3f px (coherence %.2f): %ld screen px per logo pixel\n", pitch, coh, np);
	}
	lw = (int)lround(im.w * f);
	lh = (int)lround(im.h * f);
	if (lw > W - 8 || lh > H * 9 / 10) {
		double g = fmin((double)(W - 8) / lw, (double)(H * 9 / 10) / lh);

		lw = (int)(lw * g);
		lh = (int)(lh * g);
	}
	info("logo %dx%d -> %dx%d (%.1f %% of the width), %s\n", im.w, im.h, lw, lh, 100.0 * lw / W,
	     smooth ? "area average" : "majority vote");

	memset(&lg, 0, sizeof(lg));
	memset(&pl, 0, sizeof(pl));
	pl.c[0] = bg;
	pl.cnt[0] = 1;
	pl.n = 1;
	ox = (W - lw) / 2;
	oy = (int)lround(H * cy_pct / 100.0 - lh / 2.0);
	if (oy < 0)
		oy = 0;
	if (oy + lh > H)
		oy = H - lh;
	idx = xcalloc((size_t)W * H, 1);
	frame = xcalloc((size_t)W * H, 4);

	if (!smooth) {
		scale_majority(&im, lw, lh, &major, &lg);
		for (int k = 0; k < major.n; k++)
			pl.c[pl.n++] = major.c[k];
		for (int y = 0; y < lh; y++)
			for (int x = 0; x < lw; x++) {
				uint8_t k = lg.idx[(size_t)y * lw + x];

				if (k != TRANSPARENT)
					idx[(size_t)(y + oy) * W + x + ox] = (uint8_t)(k + 1);
			}
	} else {
		scale_smooth(&im, lw, lh, &lg);
		for (int tol = 12;; tol = tol * 3 / 2) {
			bool fullpal = false;

			pl.n = 1;
			memset(idx, 0, (size_t)W * H);
			for (int y = 0; y < lh && !fullpal; y++)
				for (int x = 0; x < lw; x++) {
					size_t s = (size_t)y * lw + x;
					int a = lg.alpha[s], k;
					uint32_t c = lg.col[s];

					if (!a)
						continue;
					/* blend the edges over the background */
					c = (uint32_t)(((c >> 16 & 255) * a + (bg >> 16 & 255) * (255 - a) + 127) / 255) << 16 |
					    (uint32_t)(((c >> 8 & 255) * a + (bg >> 8 & 255) * (255 - a) + 127) / 255) << 8 |
					    (uint32_t)(((c & 255) * a + (bg & 255) * (255 - a) + 127) / 255);
					k = pal_add(&pl, c, tol, 0, 1);
					if (k < 0) {
						fullpal = true;
						break;
					}
					idx[(size_t)(y + oy) * W + x + ox] = (uint8_t)k;
				}
			if (!fullpal)
				break;
		}
		for (int i = 1; i < pl.n; i++)
			pl.c[i] = cluster_mean(&pl, i);
	}
	for (int i = 0; i < W * H; i++)
		frame[i] = pl.c[idx[i]];
	info("palette: %d colours (background included)\n", pl.n);

	size = encode(idx, W, H, &pl, bg, &enc);

	/* round trip through the frontend's decoder */
	{
		struct splash sp;
		uint32_t *chk = xcalloc((size_t)W * H, 4);

		if (splash_load_mem(enc, size, &sp) || splash_draw(&sp, chk, W, H, W) ||
		    memcmp(chk, frame, (size_t)W * H * 4))
			die("round trip failed: the decoder does not give the same frame");
		if (bench > 0) {
			uint32_t *big = xcalloc((size_t)854 * 480, 4);
			double t0 = now_s(), t1, t2;

			for (int i = 0; i < bench; i++)
				splash_draw(&sp, chk, W, H, W);
			t1 = now_s();
			for (int i = 0; i < bench; i++)
				splash_draw(&sp, big, 854, 480, 854);
			t2 = now_s();
			printf("mksplash: decode into %dx%d: %.1f us, into 854x480: %.1f us (mean of %d)\n", W, H,
			       (t1 - t0) / bench * 1e6, (t2 - t1) / bench * 1e6, bench);
			free(big);
		}
		splash_free(&sp);
		free(chk);
	}

	if (out) {
		FILE *fo = fopen(out, "wb");

		if (!fo || fwrite(enc, 1, size, fo) != size || fclose(fo))
			die("cannot write the output");
		info("%s: %zu bytes\n", out, size);
	}
	if (png) {
		uint8_t *rgb = xcalloc((size_t)W * H, 3);

		for (int i = 0; i < W * H; i++) {
			rgb[3 * i] = (uint8_t)(frame[i] >> 16);
			rgb[3 * i + 1] = (uint8_t)(frame[i] >> 8);
			rgb[3 * i + 2] = (uint8_t)frame[i];
		}
		if (!stbi_write_png(png, W, H, 3, rgb, W * 3))
			die("cannot write the PNG");
		free(rgb);
		info("%s written\n", png);
	}
	free(enc);
	free(idx);
	free(frame);
	free(lg.idx);
	free(lg.col);
	free(lg.alpha);
	free(im.p);
	return 0;
}
