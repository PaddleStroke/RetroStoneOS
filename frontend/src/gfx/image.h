/*
 * image.h - image loading (PNG/JPG/GIF/BMP/TGA with stb_image, SVG with
 * nanosvg), pre-scaling, tinting, and a two-level cache:
 *
 *  1. memory: images are shared and refcounted by key
 *     (path, target size, tint);
 *  2. disk: decoded, scaled and tinted pixels are written to
 *     <cache_dir>/<hash>.rpx (a 32-byte header + premultiplied ARGB),
 *     keyed by source path + mtime + size + target size + tint, and later
 *     mmap()ed directly: the second boot decodes nothing.
 *
 * The UI always asks for the exact pixel size it will draw at, so blits
 * never scale.
 */
#ifndef RSOS_GFX_IMAGE_H
#define RSOS_GFX_IMAGE_H

#include <stdbool.h>
#include "gfx.h"

/* Directory for .rpx files (created on demand). NULL/"" disables the
 * disk cache. Changing it does not flush the memory cache. */
void img_set_cache_dir(const char *dir);
const char *img_cache_dir(void);

/* Natural size of an image file (header only for rasters; SVG sizes are
 * cached on disk too). Returns false if the file cannot be read. */
bool img_info(const char *path, int *w, int *h);

/*
 * Returns the image scaled to exactly w x h (w or h <= 0: natural size,
 * or keep the aspect ratio if only one is given), multiplied by tint
 * (0xFFFFFFFF = none). NULL on error (logged once per path). The result
 * is shared: release it with img_put().
 */
struct gfx_image *img_get(const char *path, int w, int h, gfx_color tint);
/* Rasterizes an in-memory SVG document (built-in icons). Not shared. */
struct gfx_image *img_from_svg_string(const char *svg, int w, int h, gfx_color tint);
void img_put(struct gfx_image *img);
/* The same, for an image unlikely to be wanted again (a layer composited
 * into a cached backdrop): unreferenced, it is the first one evicted, so
 * the prefetch of every system does not push useful images out. */
void img_put_cold(struct gfx_image *img);
/* Adds a reference. */
struct gfx_image *img_ref(struct gfx_image *img);

/* Drops unreferenced images from memory. */
void img_trim(void);
/* Unreferenced images are kept up to this many bytes (pixels, heap or
 * mapped) and entries, least recently used evicted first (default 24 MB,
 * 256). Referenced images are never evicted. */
void img_set_cache_budget(size_t bytes, int entries);
/* Images in the memory cache, referenced or not (tests). */
int img_cache_entries(void);
/* Memory held by cached images (bytes). */
size_t img_mem_used(void);

/* Stats since start (for timing reports). */
struct img_stats {
	int decoded;       /* files decoded (cache misses) */
	int disk_hits;     /* .rpx files mapped */
	int mem_hits;
	int64_t decode_us; /* time spent decoding + scaling */
	int64_t disk_us;   /* time spent mapping .rpx files */
	int bd_hits;       /* composited backdrops read from their .rpx */
	int bd_built;      /* ... composited (cache miss) */
	int64_t bd_us;     /* time spent on backdrops (read or composite + save) */
};
void img_get_stats(struct img_stats *st);
void img_note_backdrop(bool hit, int64_t us);

/* Cache files are written without fsync; img_cache_flush() syncs the
 * cache filesystem once (call it when idle). img_note_written() counts a
 * write done by other caches (scan cache, backdrops). */
void img_cache_flush(void);
void img_note_written(void);

/* Generic raw-pixel cache helpers, also used for composited backdrops. */
struct gfx_image *rpx_load(const char *path, uint64_t key);
int rpx_save(const char *path, const struct gfx_image *img, uint64_t key);

/*
 * Threads (ui/prefetch.c): every function here may be called from the UI
 * thread and one worker at the same time, except img_set_cache_dir() and
 * img_trim(), which the UI calls with the worker stopped. SVG rasterizers
 * are per thread: a worker calls img_thread_exit() before it ends.
 */
void img_thread_exit(void);
/* Touches every page of a mapped (.rpx) image: the reads happen now, on
 * the calling thread, not later while drawing it. */
void img_prefault(const struct gfx_image *img);

#endif
