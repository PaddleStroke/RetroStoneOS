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
#include <pthread.h>
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

/*
 * Threads: the UI thread and the prefetch worker (ui/prefetch.c) both load
 * images. g_mu guards the cache list, the budget, the stats and the cache
 * directory state; decoding and cache file I/O run outside it, so the UI
 * thread never waits for the worker's decode (two threads decoding the same
 * key: the second insert finds the first and drops its copy). The cache
 * directory itself only changes while the worker is stopped
 * (prefetch_quiesce() before img_set_cache_dir()).
 */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct centry *g_cache;
static char g_dir[1024];
static bool g_dir_ok;
static struct img_stats g_stats;
static __thread NSVGrasterizer *t_rast;   /* one per thread (img_thread_exit) */
static int g_unsynced;

void img_set_cache_dir(const char *dir)
{
	pthread_mutex_lock(&g_mu);
	strlcpy_(g_dir, dir ? dir : "", sizeof(g_dir));
	g_dir_ok = false;
	pthread_mutex_unlock(&g_mu);
}

const char *img_cache_dir(void)
{
	return g_dir;
}

static bool cache_dir_ready(void)
{
	bool ok;

	pthread_mutex_lock(&g_mu);
	if (g_dir[0] && !g_dir_ok) {
		if (mkdir_p(g_dir) < 0)
			ui_log_once(g_dir, "image cache: cannot create %s", g_dir);
		else
			g_dir_ok = true;
	}
	ok = g_dir[0] && g_dir_ok;
	pthread_mutex_unlock(&g_mu);
	return ok;
}

void img_get_stats(struct img_stats *st)
{
	pthread_mutex_lock(&g_mu);
	*st = g_stats;
	pthread_mutex_unlock(&g_mu);
}

void img_note_backdrop(bool hit, int64_t us)
{
	pthread_mutex_lock(&g_mu);
	if (hit)
		g_stats.bd_hits++;
	else
		g_stats.bd_built++;
	g_stats.bd_us += us;
	pthread_mutex_unlock(&g_mu);
}

size_t img_mem_used(void)
{
	size_t n = 0;

	pthread_mutex_lock(&g_mu);
	for (struct centry *e = g_cache; e; e = e->next)
		n += e->bytes;
	pthread_mutex_unlock(&g_mu);
	return n;
}

void img_thread_exit(void)
{
	if (t_rast) {
		nsvgDeleteRasterizer(t_rast);
		t_rast = NULL;
	}
}

/* Reads every page of a mapped image once, so that drawing it later takes
 * no page fault: on the SD card a raw 640x480 backdrop is 1.2 MB, ~60 ms of
 * reads, which the prefetch worker pays instead of the UI thread. */
void img_prefault(const struct gfx_image *img)
{
	const volatile unsigned char *p;
	size_t len;
	unsigned sum = 0;

	if (!img || !(img->flags & GFX_IMG_MMAPPED) || !img->map)
		return;
	p = img->map;
	len = img->map_len;
	for (size_t o = 0; o < len; o += 4096)
		sum += p[o];
	if (len)
		sum += p[len - 1];
	(void)sum;
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

static struct gfx_image *rpx_load_(const char *path, uint64_t key)
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

struct gfx_image *rpx_load(const char *path, uint64_t key)
{
	struct ui_cost_scope cs;
	struct gfx_image *img;

	ui_cost_begin(&cs, UI_COST_CACHE);
	img = rpx_load_(path, key);
	ui_cost_end(&cs);
	return img;
}

int rpx_save(const char *path, const struct gfx_image *img, uint64_t key)
{
	struct rpx_hdr h;
	size_t npx = (size_t)img->w * img->h, len = sizeof(h) + npx * 4, nw;
	char *buf = xmalloc(len);
	int r;
	struct ui_cost_scope cs;

	ui_cost_begin(&cs, UI_COST_CACHE);
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
	ui_cost_end(&cs);
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

static bool img_info_(const char *path, int *w, int *h)
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
		bool dir = cache_dir_ready();
		NSVGimage *im;
		struct ui_cost_scope cs;

		if (dir) {
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
		ui_cost_begin(&cs, UI_COST_DECODE);
		im = svg_parse_file(path);
		ui_cost_end(&cs);
		if (!im)
			return false;
		/* width="1e30" or NaN: not an int, not drawable */
		*w = im->width > 0 && im->width <= GFX_IMAGE_MAX_DIM ? (int)ceilf(im->width) : 0;
		*h = im->height > 0 && im->height <= GFX_IMAGE_MAX_DIM ? (int)ceilf(im->height) : 0;
		nsvgDelete(im);
		if (*w <= 0 || *h <= 0)
			return false;
		if (dir) {
			dims[0] = *w;
			dims[1] = *h;
			file_write_atomic_nosync(cp, dims, sizeof(dims));
		}
		return true;
	}
}

/* Natural sizes already read (a direct-mapped memo under g_mu): the layout
 * of an image the worker prepared asks for its size again on the UI
 * thread; this answers without opening the file (a stat() only). */
#define INFO_SLOTS 512
static struct {
	uint64_t key;
	int32_t w, h;
} g_info[INFO_SLOTS];

bool img_info(const char *path, int *w, int *h)
{
	struct ui_cost_scope cs;
	struct src_id id;
	uint64_t key;
	unsigned slot;
	bool r;

	if (!path || !*path || !src_stat(path, &id))
		return false;
	key = make_key(path, &id, -2, -2, 0) | 1;
	slot = (unsigned)(key % INFO_SLOTS);
	pthread_mutex_lock(&g_mu);
	if (g_info[slot].key == key) {
		*w = g_info[slot].w;
		*h = g_info[slot].h;
		pthread_mutex_unlock(&g_mu);
		return true;
	}
	pthread_mutex_unlock(&g_mu);
	ui_cost_begin(&cs, UI_COST_CACHE);
	r = img_info_(path, w, h);
	ui_cost_end(&cs);
	if (r) {
		pthread_mutex_lock(&g_mu);
		g_info[slot].key = key;
		g_info[slot].w = *w;
		g_info[slot].h = *h;
		pthread_mutex_unlock(&g_mu);
	}
	return r;
}

/* --------------------------------------------------------------- decode */
/* The largest SVG raster, a side: more than any screen, and a broken size
 * (width="1e30", an extreme aspect ratio) must not reach gfx_image_new(). */
#define SVG_MAX_DIM 4096

static bool svg_size_ok(float v)
{
	return isfinite(v) && v > 0 && v <= SVG_MAX_DIM;
}

static struct gfx_image *svg_raster(NSVGimage *im, int w, int h)
{
	struct gfx_image *img;
	float sx, sy, s;
	int rw, rh;

	if (!t_rast)
		t_rast = nsvgCreateRasterizer();
	if (!(im->width > 0) || !(im->height > 0) || !isfinite(im->width) || !isfinite(im->height) ||
	    w <= 0 || h <= 0 || w > SVG_MAX_DIM || h > SVG_MAX_DIM)
		return NULL;
	sx = (float)w / im->width;
	sy = (float)h / im->height;
	/* nanosvg scales uniformly: rasterize at the larger scale, then
	 * resample the other axis if the element stretches the SVG. */
	s = sx > sy ? sx : sy;
	if (fabsf(sx - sy) < 0.01f) {
		rw = w;
		rh = h;
		s = sx;
	} else {
		/* an extreme stretch: rasterize smaller, the resample does the rest */
		if (!svg_size_ok(im->width * s) || !svg_size_ok(im->height * s))
			s = (float)(SVG_MAX_DIM - 1) / (im->width > im->height ? im->width : im->height);
		if (!(im->width * s <= SVG_MAX_DIM) || !(im->height * s <= SVG_MAX_DIM))
			return NULL;  /* a denormal size: s overflowed (or NaN) */
		rw = MAX(1, (int)ceilf(im->width * s));
		rh = MAX(1, (int)ceilf(im->height * s));
	}
	img = gfx_image_new(rw, rh);
	if (!img)
		return NULL;
	nsvgRasterize(t_rast, im, 0, 0, s, (unsigned char *)img->px, rw, rh, rw * 4);
	gfx_image_from_rgba(img);
	if (rw != w || rh != h) {
		struct gfx_image *sc = gfx_image_scale(img, w, h);

		gfx_image_free(img);
		img = sc;
	}
	return img;
}

static struct gfx_image *decode_(const char *path, int w, int h, gfx_color tint)
{
	struct gfx_image *img = NULL;

	if (is_svg(path)) {
		NSVGimage *im = svg_parse_file(path);

		if (!im)
			return NULL;
		if (w <= 0)
			w = svg_size_ok(im->width) ? (int)ceilf(im->width) : 0;
		if (h <= 0)
			h = svg_size_ok(im->height) ? (int)ceilf(im->height) : 0;
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

static struct gfx_image *decode(const char *path, int w, int h, gfx_color tint)
{
	struct ui_cost_scope cs;
	struct gfx_image *img;

	ui_cost_begin(&cs, UI_COST_DECODE);
	img = decode_(path, w, h, tint);
	ui_cost_end(&cs);
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
 * the budget. Referenced images are never touched. Under g_mu. */
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
	pthread_mutex_lock(&g_mu);
	g_budget_bytes = bytes;
	g_budget_entries = entries < 0 ? 0 : entries;
	cache_fit_budget();
	pthread_mutex_unlock(&g_mu);
}

int img_cache_entries(void)
{
	int n = 0;

	pthread_mutex_lock(&g_mu);
	for (struct centry *e = g_cache; e; e = e->next)
		n++;
	pthread_mutex_unlock(&g_mu);
	return n;
}

/* Under g_mu: the entry with this key, moved to the front, referenced. */
static struct gfx_image *cache_find(uint64_t key)
{
	struct centry *e;

	for (struct centry **pp = &g_cache; (e = *pp); pp = &e->next) {
		if (e->key == key) {
			e->refs++;
			e->used = ++g_tick;
			/* move to the front: the next lookup is short */
			*pp = e->next;
			e->next = g_cache;
			g_cache = e;
			return &e->img;
		}
	}
	return NULL;
}

/* Under g_mu. Another thread may have inserted the same key meanwhile:
 * then that one is returned and img is freed. */
static struct gfx_image *cache_insert(struct gfx_image *img, uint64_t key)
{
	struct gfx_image *have = cache_find(key);
	struct centry *e;

	if (have) {
		gfx_image_free(img);
		return have;
	}
	e = xcalloc(1, sizeof(*e));
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
	struct gfx_image *img;
	char cp[1200];
	int64_t t0;
	bool dir;

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
	pthread_mutex_lock(&g_mu);
	img = cache_find(key);
	if (img)
		g_stats.mem_hits++;
	pthread_mutex_unlock(&g_mu);
	if (img)
		return img;
	dir = cache_dir_ready();
	if (dir) {
		t0 = ui_now_us();
		cache_path(key, "rpx", cp, sizeof(cp));
		img = rpx_load(cp, key);
		if (img) {
			pthread_mutex_lock(&g_mu);
			g_stats.disk_hits++;
			g_stats.disk_us += ui_now_us() - t0;
			img = cache_insert(img, key);
			pthread_mutex_unlock(&g_mu);
			return img;
		}
	}
	t0 = ui_now_us();
	img = decode(path, w, h, tint);
	t0 = ui_now_us() - t0;
	if (!img) {
		ui_log_once(path, "image: cannot decode %s", path);
		return NULL;
	}
	if (dir) {
		int r = rpx_save(cp, img, key);

		if (r < 0)
			ui_log_once(g_dir, "image cache: cannot write %s: %s", cp, strerror(-r));
	}
	pthread_mutex_lock(&g_mu);
	g_stats.decode_us += t0;
	g_stats.decoded++;
	img = cache_insert(img, key);
	pthread_mutex_unlock(&g_mu);
	return img;
}

struct gfx_image *img_from_svg_string(const char *svg, int w, int h, gfx_color tint)
{
	char *buf = xstrdup(svg);
	NSVGimage *im;
	struct gfx_image *img = NULL;
	struct ui_cost_scope cs;

	ui_cost_begin(&cs, UI_COST_DECODE);
	im = nsvgParse(buf, "px", 96.0f);
	free(buf);
	if (im) {
		if (w <= 0 && h > 0 && im->height > 0 && svg_size_ok(im->width * (float)h / im->height))
			w = MAX(1, (int)lroundf(im->width * (float)h / im->height));
		if (h <= 0 && w > 0 && im->width > 0 && svg_size_ok(im->height * (float)w / im->width))
			h = MAX(1, (int)lroundf(im->height * (float)w / im->width));
		if (w > 0 && h > 0)
			img = svg_raster(im, w, h);
		nsvgDelete(im);
		if (img && tint != 0xffffffffu)
			gfx_image_tint(img, tint);
	}
	ui_cost_end(&cs);
	return img;
}

struct gfx_image *img_ref(struct gfx_image *img)
{
	if (img) {
		pthread_mutex_lock(&g_mu);
		((struct centry *)img)->refs++;
		pthread_mutex_unlock(&g_mu);
	}
	return img;
}

void img_put(struct gfx_image *img)
{
	struct centry *e = (struct centry *)img;

	if (!e)
		return;
	pthread_mutex_lock(&g_mu);
	e->used = ++g_tick;
	if (--e->refs <= 0)
		cache_fit_budget();
	pthread_mutex_unlock(&g_mu);
}

void img_put_cold(struct gfx_image *img)
{
	struct centry *e = (struct centry *)img;

	if (!e)
		return;
	pthread_mutex_lock(&g_mu);
	if (--e->refs <= 0) {
		e->used = 0;             /* the first one to go */
		cache_fit_budget();
	} else {
		e->used = ++g_tick;
	}
	pthread_mutex_unlock(&g_mu);
}

void img_trim(void)
{
	struct centry **pp = &g_cache;

	pthread_mutex_lock(&g_mu);
	while (*pp) {
		struct centry *e = *pp;

		if (e->refs <= 0) {
			*pp = e->next;
			centry_free(e);
		} else {
			pp = &e->next;
		}
	}
	pthread_mutex_unlock(&g_mu);
}

/* Cache files are written without fsync (fast first boot); this flushes
 * them all at once, when the UI is idle. */
void img_note_written(void)
{
	pthread_mutex_lock(&g_mu);
	g_unsynced++;
	pthread_mutex_unlock(&g_mu);
}

void img_cache_flush(void)
{
	int fd, n;

	pthread_mutex_lock(&g_mu);
	n = g_unsynced;
	g_unsynced = 0;
	pthread_mutex_unlock(&g_mu);
	if (!n || !g_dir[0])
		return;
	fd = open(g_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		syncfs(fd);
		close(fd);
	}
}
