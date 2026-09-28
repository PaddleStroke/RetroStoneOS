/*
 * image.c - see image.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../third_party/nanosvg/nanosvg.h"
#include "../../third_party/nanosvg/nanosvgrast.h"
#include "../../third_party/stb/stb_image.h"
#include "../ui/util.h"

#define RPX_MAGIC "RPX1"
#define CACHE_VERSION 3u   /* bump when the pixel pipeline changes */

struct rpx_hdr {
	char magic[4];
	uint32_t w, h, flags;
	uint64_t key;
	uint32_t reserved[2];
};

struct centry {
	struct gfx_image img;    /* handed out; must stay first */
	uint64_t key;
	int refs;
	size_t bytes;            /* heap pixels (img_mem_used) */
	size_t cost;             /* pixels, heap or mapped (the budget) */
	uint64_t used;           /* LRU tick of the last img_get / img_put */
	struct centry *next;
};

/*
 * Unreferenced images stay cached (a list scrolled back and forth decodes
 * nothing), but only up to a budget: past it the least recently used ones
 * go (review F-H5: the cache used to grow for the whole session, about
 * 250 KB per picture of a scraped library). Mapped .rpx files count too:
 * they cost address space and VMAs.
 */
#define IMG_BUDGET_BYTES_DEFAULT ((size_t)24 << 20)
#define IMG_BUDGET_ENTRIES_DEFAULT 256
static size_t g_budget_bytes = IMG_BUDGET_BYTES_DEFAULT;
static int g_budget_entries = IMG_BUDGET_ENTRIES_DEFAULT;
static uint64_t g_tick;

static struct centry *g_cache;
static char g_dir[1024];
static bool g_dir_ok;
static struct img_stats g_stats;
static NSVGrasterizer *g_rast;

void img_set_cache_dir(const char *dir)
{
	strlcpy_(g_dir, dir ? dir : "", sizeof(g_dir));
	g_dir_ok = false;
}

const char *img_cache_dir(void)
{
	return g_dir;
}

static bool cache_dir_ready(void)
{
	if (!g_dir[0])
		return false;
	if (!g_dir_ok) {
		if (mkdir_p(g_dir) < 0) {
			ui_log_once(g_dir, "image cache: cannot create %s", g_dir);
			return false;
		}
		g_dir_ok = true;
	}
	return true;
}

void img_get_stats(struct img_stats *st)
{
	*st = g_stats;
}

void img_note_backdrop(bool hit, int64_t us)
{
	if (hit)
		g_stats.bd_hits++;
	else
		g_stats.bd_built++;
	g_stats.bd_us += us;
}

size_t img_mem_used(void)
{
	size_t n = 0;

	for (struct centry *e = g_cache; e; e = e->next)
		n += e->bytes;
	return n;
}

/* ------------------------------------------------------------ rpx files */
/*
 * Run-length coding (header flag RPX_RLE) for the images that compress:
 * composited backdrops (gradients, flat panels, patterns) shrink ~10-30x,
 * which matters because the first carousel frame reads one full-screen
 * backdrop from the SD card (1.2 MB raw at 640x480). Words after the
 * header: a token t, then either one pixel repeated (t & 0x80000000) or
 * (t) literal pixels. Photos stay raw (and mmap()ed).
 */
#define RPX_RLE 0x100u
#define RLE_RUN 0x80000000u

/* Returns the number of words, 0 if the coded image would exceed max. */
static size_t rle_encode(const struct gfx_image *img, uint32_t *out, size_t max)
{
	size_t n = (size_t)img->w * img->h, i = 0, o = 0;

#define PX(k) img->px[((k) / (size_t)img->w) * (size_t)img->stride + (k) % (size_t)img->w]
	while (i < n) {
		uint32_t v = PX(i);
		size_t r = 1, start;

		while (i + r < n && r < 0x7fffffffu && PX(i + r) == v)
			r++;
		if (r >= 3) {
			if (o + 2 > max)
				return 0;
			out[o++] = RLE_RUN | (uint32_t)r;
			out[o++] = v;
			i += r;
			continue;
		}
		/* literals up to the next run of 3 */
		start = i;
		while (i < n && i - start < 0x7fffffffu) {
			uint32_t w = PX(i);
			size_t r2 = 1;

			while (i + r2 < n && r2 < 3 && PX(i + r2) == w)
				r2++;
			if (r2 >= 3)
				break;
			i += r2;
		}
		if (o + 1 + (i - start) > max)
			return 0;
		out[o++] = (uint32_t)(i - start);
		for (size_t k = start; k < i; k++)
			out[o++] = PX(k);
	}
#undef PX
	return o;
}

static bool rle_decode(const uint32_t *in, size_t nw, uint32_t *px, size_t npx)
{
	size_t k = 0, o = 0;

	while (k < nw) {
		uint32_t t = in[k++];
		size_t c = t & ~RLE_RUN;

		if (!c || c > npx - o)
			return false;
		if (t & RLE_RUN) {
			uint32_t v;

			if (k >= nw)
				return false;
			v = in[k++];
			for (size_t j = 0; j < c; j++)
				px[o + j] = v;
		} else {
			if (c > nw - k)
				return false;
			memcpy(px + o, in + k, c * 4);
			k += c;
		}
		o += c;
	}
	return o == npx;
}

struct gfx_image *rpx_load(const char *path, uint64_t key)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	struct rpx_hdr *h;
	void *m;
	struct gfx_image *img;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(*h)) {
		close(fd);
		return NULL;
	}
	m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return NULL;
	h = m;
	if (memcmp(h->magic, RPX_MAGIC, 4) || h->key != key || !h->w || !h->h || h->w > 8192 ||
	    h->h > 8192) {
		munmap(m, (size_t)st.st_size);
		return NULL;
	}
	if (h->flags & RPX_RLE) {
		size_t nw = ((size_t)st.st_size - sizeof(*h)) / 4;

		img = gfx_image_new((int)h->w, (int)h->h);
		if ((st.st_size - (off_t)sizeof(*h)) % 4 ||
		    !rle_decode((const uint32_t *)((char *)m + sizeof(*h)), nw, img->px,
				(size_t)h->w * h->h)) {
			gfx_image_free(img);
			img = NULL;
		} else {
			img->flags = h->flags & GFX_IMG_OPAQUE;
		}
		munmap(m, (size_t)st.st_size);
		return img;
	}
	if ((off_t)sizeof(*h) + (off_t)h->w * h->h * 4 != st.st_size) {
		munmap(m, (size_t)st.st_size);
		return NULL;
	}
	img = xcalloc(1, sizeof(*img));
	img->w = (int)h->w;
	img->h = (int)h->h;
	img->stride = img->w;
	img->px = (uint32_t *)((char *)m + sizeof(*h));
	img->flags = (h->flags & GFX_IMG_OPAQUE) | GFX_IMG_MMAPPED;
	img->map = m;
	img->map_len = (size_t)st.st_size;
	return img;
}

int rpx_save(const char *path, const struct gfx_image *img, uint64_t key)
{
	struct rpx_hdr h;
	size_t npx = (size_t)img->w * img->h, len = sizeof(h) + npx * 4, nw;
	char *buf = xmalloc(len);
	int r;

	memset(&h, 0, sizeof(h));
	memcpy(h.magic, RPX_MAGIC, 4);
	h.w = (uint32_t)img->w;
	h.h = (uint32_t)img->h;
	h.flags = img->flags & GFX_IMG_OPAQUE;
	h.key = key;
	/* run-length coded when that saves at least 40 % */
	nw = rle_encode(img, (uint32_t *)(buf + sizeof(h)), npx * 6 / 10);
	if (nw) {
		h.flags |= RPX_RLE;
		len = sizeof(h) + nw * 4;
	} else {
		for (int j = 0; j < img->h; j++)
			memcpy(buf + sizeof(h) + (size_t)j * img->w * 4,
			       img->px + (size_t)j * img->stride, (size_t)img->w * 4);
	}
	memcpy(buf, &h, sizeof(h));
	r = file_write_atomic_nosync(path, buf, len);
	if (r == 0)
		img_note_written();
	free(buf);
	return r;
}

/* ----------------------------------------------------------------- keys */
struct src_id {
	int64_t mtime_ns;
	int64_t size;
};

static bool src_stat(const char *path, struct src_id *id)
{
	struct stat st;

	if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
		return false;
	id->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
	id->size = (int64_t)st.st_size;
	return true;
}

static uint64_t make_key(const char *path, const struct src_id *id, int w, int h,
			 gfx_color tint)
{
	struct {
		int64_t mt, sz;
		int32_t w, h;
		uint32_t tint, ver;
	} k = { id->mtime_ns, id->size, w, h, tint, CACHE_VERSION };

	return hash64(&k, sizeof(k), hash64_str(path, 0));
}

static void cache_path(uint64_t key, const char *ext, char *out, size_t n)
{
	snprintf(out, n, "%s/%016llx.%s", g_dir, (unsigned long long)key, ext);
}

static bool is_svg(const char *path)
{
	return str_ends_i(path, ".svg");
}

/* ----------------------------------------------------------------- info */
static NSVGimage *svg_parse_file(const char *path)
{
	size_t len;
	char *buf = file_read(path, &len);
	NSVGimage *im;

	if (!buf)
		return NULL;
	im = nsvgParse(buf, "px", 96.0f);
	free(buf);
	return im;
}

bool img_info(const char *path, int *w, int *h)
{
	struct src_id id;

	if (!path || !*path || !src_stat(path, &id))
		return false;
	if (!is_svg(path)) {
		int comp;

		return stbi_info(path, w, h, &comp) && *w > 0 && *h > 0;
	}
	/* SVG: parsing is not free, so remember the size on disk. */
	{
		uint64_t key = make_key(path, &id, -1, -1, 0);
		char cp[1200];
		int32_t dims[2];
		int fd;
		NSVGimage *im;

		if (cache_dir_ready()) {
			cache_path(key, "inf", cp, sizeof(cp));
			fd = open(cp, O_RDONLY | O_CLOEXEC);
			if (fd >= 0) {
				ssize_t r = read(fd, dims, sizeof(dims));

				close(fd);
				if (r == (ssize_t)sizeof(dims) && dims[0] > 0 && dims[1] > 0) {
					*w = dims[0];
					*h = dims[1];
					return true;
				}
			}
		}
		im = svg_parse_file(path);
		if (!im)
			return false;
		*w = (int)ceilf(im->width);
		*h = (int)ceilf(im->height);
		nsvgDelete(im);
		if (*w <= 0 || *h <= 0)
			return false;
		if (g_dir_ok) {
			dims[0] = *w;
			dims[1] = *h;
			file_write_atomic_nosync(cp, dims, sizeof(dims));
		}
		return true;
	}
}

/* --------------------------------------------------------------- decode */
static struct gfx_image *svg_raster(NSVGimage *im, int w, int h)
{
	struct gfx_image *img;
	float sx, sy, s;
	int rw, rh;

	if (!g_rast)
		g_rast = nsvgCreateRasterizer();
	if (im->width <= 0 || im->height <= 0)
		return NULL;
	sx = (float)w / im->width;
	sy = (float)h / im->height;
	/* nanosvg scales uniformly: rasterize at the larger scale, then
	 * resample the other axis if the element stretches the SVG. */
	s = sx > sy ? sx : sy;
	rw = fabsf(sx - sy) < 0.01f ? w : (int)ceilf(im->width * s);
	rh = fabsf(sx - sy) < 0.01f ? h : (int)ceilf(im->height * s);
	if (fabsf(sx - sy) < 0.01f)
		s = sx;
	img = gfx_image_new(rw, rh);
	nsvgRasterize(g_rast, im, 0, 0, s, (unsigned char *)img->px, rw, rh, rw * 4);
	gfx_image_from_rgba(img);
	if (rw != w || rh != h) {
		struct gfx_image *sc = gfx_image_scale(img, w, h);

		gfx_image_free(img);
		img = sc;
	}
	return img;
}

static struct gfx_image *decode(const char *path, int w, int h, gfx_color tint)
{
	struct gfx_image *img = NULL;

	if (is_svg(path)) {
		NSVGimage *im = svg_parse_file(path);

		if (!im)
			return NULL;
		if (w <= 0)
			w = (int)ceilf(im->width);
		if (h <= 0)
			h = (int)ceilf(im->height);
		if (w > 0 && h > 0)
			img = svg_raster(im, w, h);
		nsvgDelete(im);
	} else {
		int iw, ih, comp;
		unsigned char *data = stbi_load(path, &iw, &ih, &comp, 4);

		if (!data)
			return NULL;
		img = xcalloc(1, sizeof(*img));
		img->px = (uint32_t *)data;  /* stbi uses malloc: free() works */
		img->w = iw;
		img->h = ih;
		img->stride = iw;
		gfx_image_from_rgba(img);
		if (w <= 0)
			w = iw;
		if (h <= 0)
			h = ih;
		if (w != iw || h != ih) {
			struct gfx_image *sc = gfx_image_scale(img, w, h);

			gfx_image_free(img);
			img = sc;
		}
	}
	if (img && tint != 0xffffffffu)
		gfx_image_tint(img, tint);
	return img;
}

/* ----------------------------------------------------------------- cache */
static void centry_free(struct centry *e)
{
	if (e->img.flags & GFX_IMG_MMAPPED)
		munmap(e->img.map, e->img.map_len);
	else
		free(e->img.px);
	free(e);
}

/* Evicts unreferenced entries, least recently used first, until those fit
 * the budget. Referenced images are never touched. */
static void cache_fit_budget(void)
{
	for (;;) {
		struct centry **victim = NULL;
		size_t idle_bytes = 0;
		int idle_n = 0;

		for (struct centry **pp = &g_cache; *pp; pp = &(*pp)->next) {
			if ((*pp)->refs > 0)
				continue;
			idle_bytes += (*pp)->cost;
			idle_n++;
			if (!victim || (*pp)->used < (*victim)->used)
				victim = pp;
		}
		if (!victim || (idle_bytes <= g_budget_bytes && idle_n <= g_budget_entries))
			return;
		{
			struct centry *e = *victim;

			*victim = e->next;
			centry_free(e);
		}
	}
}

void img_set_cache_budget(size_t bytes, int entries)
{
	g_budget_bytes = bytes;
	g_budget_entries = entries < 0 ? 0 : entries;
	cache_fit_budget();
}

int img_cache_entries(void)
{
	int n = 0;

	for (struct centry *e = g_cache; e; e = e->next)
		n++;
	return n;
}

static struct gfx_image *cache_insert(struct gfx_image *img, uint64_t key)
{
	struct centry *e = xcalloc(1, sizeof(*e));

	e->img = *img;
	free(img);  /* the struct shell only; pixels now belong to e->img */
	e->key = key;
	e->refs = 1;
	e->bytes = (e->img.flags & GFX_IMG_MMAPPED) ? 0 : (size_t)e->img.w * e->img.h * 4;
	e->cost = (size_t)e->img.w * e->img.h * 4;
	e->used = ++g_tick;
	e->next = g_cache;
	g_cache = e;
	return &e->img;
}

struct gfx_image *img_get(const char *path, int w, int h, gfx_color tint)
{
	struct src_id id;
	uint64_t key;
	struct centry *e;
	struct gfx_image *img;
	char cp[1200];
	int64_t t0;

	if (!path || !*path)
		return NULL;
	if (!src_stat(path, &id)) {
		ui_log_once(path, "image: %s not found", path);
		return NULL;
	}
	if (w <= 0 || h <= 0) {
		int nw, nh;

		if (!img_info(path, &nw, &nh)) {
			ui_log_once(path, "image: cannot read %s", path);
			return NULL;
		}
		if (w <= 0 && h <= 0) {
			w = nw;
			h = nh;
		} else if (w <= 0) {
			w = MAX(1, (int)lroundf((float)nw * (float)h / (float)nh));
		} else {
			h = MAX(1, (int)lroundf((float)nh * (float)w / (float)nw));
		}
	}
	/* Refuse absurd sizes (bad theme values). */
	if (w > 4096 || h > 4096) {
		ui_log_once(path, "image: %s requested at %dx%d, clamped", path, w, h);
		w = MIN(w, 4096);
		h = MIN(h, 4096);
	}
	key = make_key(path, &id, w, h, tint);
	for (struct centry **pp = &g_cache; (e = *pp); pp = &e->next) {
		if (e->key == key) {
			e->refs++;
			e->used = ++g_tick;
			g_stats.mem_hits++;
			/* move to the front: the next lookup is short */
			*pp = e->next;
			e->next = g_cache;
			g_cache = e;
			return &e->img;
		}
	}
	if (cache_dir_ready()) {
		t0 = ui_now_us();
		cache_path(key, "rpx", cp, sizeof(cp));
		img = rpx_load(cp, key);
		if (img) {
			g_stats.disk_hits++;
			g_stats.disk_us += ui_now_us() - t0;
			return cache_insert(img, key);
		}
	}
	t0 = ui_now_us();
	img = decode(path, w, h, tint);
	g_stats.decode_us += ui_now_us() - t0;
	if (!img) {
		ui_log_once(path, "image: cannot decode %s", path);
		return NULL;
	}
	g_stats.decoded++;
	if (g_dir_ok) {
		int r = rpx_save(cp, img, key);

		if (r < 0)
			ui_log_once(g_dir, "image cache: cannot write %s: %s", cp, strerror(-r));
	}
	return cache_insert(img, key);
}

struct gfx_image *img_from_svg_string(const char *svg, int w, int h, gfx_color tint)
{
	char *buf = xstrdup(svg);
	NSVGimage *im = nsvgParse(buf, "px", 96.0f);
	struct gfx_image *img = NULL;

	free(buf);
	if (!im)
		return NULL;
	if (w <= 0 && h > 0 && im->height > 0)
		w = MAX(1, (int)lroundf(im->width * (float)h / im->height));
	if (h <= 0 && w > 0 && im->width > 0)
		h = MAX(1, (int)lroundf(im->height * (float)w / im->width));
	if (w > 0 && h > 0)
		img = svg_raster(im, w, h);
	nsvgDelete(im);
	if (img && tint != 0xffffffffu)
		gfx_image_tint(img, tint);
	return img;
}

struct gfx_image *img_ref(struct gfx_image *img)
{
	if (img)
		((struct centry *)img)->refs++;
	return img;
}

void img_put(struct gfx_image *img)
{
	struct centry *e = (struct centry *)img;

	if (!e)
		return;
	e->used = ++g_tick;
	if (--e->refs <= 0)
		cache_fit_budget();
}

void img_trim(void)
{
	struct centry **pp = &g_cache;

	while (*pp) {
		struct centry *e = *pp;

		if (e->refs <= 0) {
			*pp = e->next;
			centry_free(e);
		} else {
			pp = &e->next;
		}
	}
}

/* Cache files are written without fsync (fast first boot); this flushes
 * them all at once, when the UI is idle. */
static int g_unsynced;

void img_note_written(void)
{
	g_unsynced++;
}

void img_cache_flush(void)
{
	int fd;

	if (!g_unsynced || !g_dir[0])
		return;
	fd = open(g_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		syncfs(fd);
		close(fd);
	}
	g_unsynced = 0;
}
