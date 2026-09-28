/*
 * splash.h - the boot logo: a pre-rendered XRGB8888 frame, RLE-compressed
 * at build time by tools/mksplash.c (/usr/share/rsos/splash.rle), drawn by
 * rsos-frontend right after display_init() and by `rsos-frontend --splash`.
 *
 * File format (little-endian), version 1:
 *   0   "RSPL"
 *   4   u16 version (1)
 *   6   u16 width, u16 height
 *   10  u16 palette entries n (1..256)
 *   12  u32 background (XRGB8888): fills a surface larger than the frame
 *   16  n x u32 palette (XRGB8888)
 *   then one record per row:
 *     u8 0                         same pixels as the previous row
 *     u8 1, u16 nruns, nruns x { u8 length (1..255), u8 palette index }
 *                                  (the lengths add up to width; longer
 *                                  runs are split)
 *     u8 2, u16 n, n x { u16 x, u8 length (1..255), u8 palette index }
 *                                  the previous row with n spans changed
 *                                  (edges that move by a pixel or two)
 * A flat background with the pixel-art logo is about 11 KB, and decoding
 * is fills and row copies (about 20 us for 640x480 on a PC).
 */
#ifndef RSOS_SPLASH_H
#define RSOS_SPLASH_H

#include <stddef.h>
#include <stdint.h>

#define SPLASH_MAGIC "RSPL"
#define SPLASH_VERSION 1
#define SPLASH_HEADER 16

struct splash {
	uint8_t *data;       /* the whole file */
	size_t size;
	int width, height;
	uint32_t bg;
};

/* Reads and checks the header. 0 or -errno (-EINVAL: not a splash file). */
int splash_load(const char *path, struct splash *s);
/* Same, from memory (the buffer is copied). */
int splash_load_mem(const void *buf, size_t size, struct splash *s);
void splash_free(struct splash *s);

/*
 * Draws the frame centred in a dw x dh XRGB8888 surface (stride in
 * pixels), cropped if the surface is smaller, the rest filled with the
 * background. 0, or -EINVAL if the data is corrupt (the surface is then
 * partly drawn).
 */
int splash_draw(const struct splash *s, uint32_t *dst, int dw, int dh, int stride_px);

#endif
