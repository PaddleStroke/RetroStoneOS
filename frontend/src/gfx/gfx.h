/*
 * gfx.h - RetroStoneOS 2D software renderer.
 *
 * Target: a CPU-writable XRGB8888 buffer (struct gfx_surface), which the
 * display layer scales to the physical output in hardware. Nothing here
 * knows about DRM.
 *
 * Pixel formats
 *   - Surfaces: 0xXXRRGGBB (the X byte is kept as 0xFF when drawing onto a
 *     surface that wraps an image, so surfaces can also be render targets
 *     for premultiplied images).
 *   - Images (struct gfx_image): premultiplied 0xAARRGGBB. Premultiplied
 *     alpha makes blending one multiply per channel pair, and a color tint
 *     or a global opacity is the same operation.
 *   - API colors (gfx_color): straight (non-premultiplied) 0xAARRGGBB.
 *
 * Performance notes (Cortex-A7): every primitive works row by row on plain
 * pointers, with no per-pixel function calls. Blends use a 2-channels-per-
 * multiply SWAR trick, which GCC also vectorizes with NEON (-O2
 * -ftree-vectorize). Opaque images are copied with memcpy, fully
 * transparent spans are skipped. Scaling is done once at load time
 * (gfx_image_scale), not per frame.
 */
#ifndef RSOS_GFX_H
#define RSOS_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t gfx_color; /* straight 0xAARRGGBB */

struct gfx_rect {
	int x, y, w, h;
};

struct gfx_surface {
	uint32_t *pixels;
	int w, h;
	int stride;              /* in pixels (uint32_t), not bytes */
	struct gfx_rect clip;    /* always inside [0,w)x[0,h) */
};

#define GFX_IMG_OPAQUE   0x1u  /* every pixel has alpha 0xFF */
#define GFX_IMG_MMAPPED  0x2u  /* px points into an mmap()ed cache file */
#define GFX_IMG_EXTERNAL 0x4u  /* px is not owned */

struct gfx_image {
	uint32_t *px;            /* premultiplied ARGB */
	int w, h;
	int stride;              /* in pixels */
	unsigned flags;
	void *map;               /* mmap base (GFX_IMG_MMAPPED) */
	size_t map_len;
};

/* ------------------------------------------------------------ surfaces */
void gfx_surface_init(struct gfx_surface *s, uint32_t *pixels, int w, int h,
		      int stride_px);
/* A surface drawing into an image (for compositing and text caches). */
void gfx_surface_from_image(struct gfx_surface *s, struct gfx_image *img);
/* Sets the clip to r intersected with the surface, returns the old clip. */
struct gfx_rect gfx_clip_push(struct gfx_surface *s, struct gfx_rect r);
void gfx_clip_pop(struct gfx_surface *s, struct gfx_rect old);
bool gfx_rect_intersect(struct gfx_rect a, struct gfx_rect b, struct gfx_rect *out);

/* ---------------------------------------------------------- primitives */
/* Solid (alpha 255) or blended fill. */
void gfx_fill(struct gfx_surface *s, int x, int y, int w, int h, gfx_color c);
/* Two-color gradient, vertical (top->bottom) or horizontal (left->right). */
void gfx_fill_gradient(struct gfx_surface *s, int x, int y, int w, int h,
		       gfx_color c0, gfx_color c1, bool horizontal);
/* Anti-aliased rounded rectangle (radius in pixels, 0 = square). */
void gfx_fill_round(struct gfx_surface *s, int x, int y, int w, int h,
		    int radius, gfx_color c);
/* Rounded-rectangle outline of the given thickness. */
void gfx_stroke_round(struct gfx_surface *s, int x, int y, int w, int h,
		      int radius, int thick, gfx_color c);
/* Anti-aliased filled circle. */
void gfx_fill_circle(struct gfx_surface *s, int cx, int cy, int r, gfx_color c);

/* Blends an 8-bit coverage mask (glyphs) with color c at (x, y). */
void gfx_mask(struct gfx_surface *s, const uint8_t *mask, int mstride,
	      int x, int y, int w, int h, gfx_color c);
/* Multiplies the destination (all channels) by 1 - mask: punches glyphs
 * out of an image (button icons). */
void gfx_mask_erase(struct gfx_surface *s, const uint8_t *mask, int mstride,
		    int x, int y, int w, int h);

/* --------------------------------------------------------------- blits */
/* alpha: global opacity 0..255 (255 = as is). */
void gfx_blit(struct gfx_surface *s, const struct gfx_image *img, int x, int y,
	      int alpha);
/* Blits the src sub-rectangle of img. */
void gfx_blit_sub(struct gfx_surface *s, const struct gfx_image *img,
		  struct gfx_rect src, int x, int y, int alpha);
/* Nearest-neighbour scaled blit (for transient animations only). */
void gfx_blit_scaled(struct gfx_surface *s, const struct gfx_image *img,
		     int x, int y, int w, int h, int alpha);
/* Tiles img over the rectangle, starting at its top-left corner. */
void gfx_blit_tiled(struct gfx_surface *s, const struct gfx_image *img,
		    int x, int y, int w, int h, int alpha);
/* Nine-patch: corners of `corner` pixels stay unscaled, edges and center
 * are stretched (nearest) or tiled. */
void gfx_ninepatch(struct gfx_surface *s, const struct gfx_image *img,
		   int x, int y, int w, int h, int corner);

/* --------------------------------------------------------------- images */
struct gfx_image *gfx_image_new(int w, int h);            /* zeroed */
void gfx_image_free(struct gfx_image *img);
/* Takes straight RGBA bytes (stb_image / nanosvg order) and converts
 * them in place to premultiplied ARGB words. Sets GFX_IMG_OPAQUE. */
void gfx_image_from_rgba(struct gfx_image *img);
void gfx_image_update_flags(struct gfx_image *img);
/* High-quality separable resample (tent filter: area-like when shrinking,
 * bilinear when enlarging). Returns a new image. */
struct gfx_image *gfx_image_scale(const struct gfx_image *src, int w, int h);
/* Multiplies every pixel by a straight color (ES "color" property). */
void gfx_image_tint(struct gfx_image *img, gfx_color c);

/* ---------------------------------------------------------------- color */
static inline gfx_color gfx_rgba(unsigned r, unsigned g, unsigned b, unsigned a)
{
	return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline gfx_color gfx_with_alpha(gfx_color c, unsigned a)
{
	return (c & 0x00ffffffu) | ((((c >> 24) * a + 127) / 255) << 24);
}

/* Linear mix of two straight colors, t in 0..256. */
gfx_color gfx_color_mix(gfx_color a, gfx_color b, int t);
/* Perceived luminance 0..255. */
int gfx_color_luma(gfx_color c);
/* Parses ES "RRGGBB" / "RRGGBBAA" into 0xAARRGGBB. */
bool gfx_parse_color(const char *s, gfx_color *out);

#endif
