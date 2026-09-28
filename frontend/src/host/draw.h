/*
 * draw.h - the host's minimal software drawing: an XRGB8888 canvas for the
 * in-game menu, text, and helpers that work directly on a core frame in its
 * own pixel format (toasts, the FPS overlay, thumbnails).
 *
 * Text is TrueType (src/gfx/font.h: DejaVu Sans Condensed, then DejaVu Sans
 * and the CJK fallback chain, so every language of the menu UI renders) once
 * draw_set_fonts() found the fonts; without them it is the display layer's
 * 8x8 font (ASCII only, other characters as '?').
 *
 * Hook for the UI agent: the menu only calls the cv_* functions below. To
 * theme it, reimplement them in a draw_gfx.c on top of src/gfx (gfx_fill,
 * font_draw) and select that file in host.mk instead of draw.c.
 */
#ifndef RSOS_HOST_DRAW_H
#define RSOS_HOST_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct canvas {
	uint32_t *px;
	int w, h;
	int stride;   /* pixels */
};

/*
 * The fonts directory (/usr/share/rsos/fonts): sets up src/gfx/font.h
 * (font_setup_dir: default font and the CJK fallbacks ordered for the
 * language in use, so call it after i18n_set_language()). Returns false,
 * and the text stays 8x8, when the directory has no usable font.
 */
bool draw_set_fonts(const char *dir);
bool draw_ttf(void);   /* TrueType text in use */

/* argb: straight alpha; alpha 255 = opaque fill, else blended. */
void cv_fill(struct canvas *c, int x, int y, int w, int h, uint32_t argb);
/*
 * One line of UTF-8 text in the cell of the 8x8 font scaled by `scale`
 * (1, 2...): y is the top of that 8*scale cell (TrueType: about 11 px EM at
 * scale 1, 18 px at 2, the capitals centred in the cell, descenders below
 * it). Returns the advance.
 */
int cv_text(struct canvas *c, int x, int y, const char *s, uint32_t rgb, int scale);
int cv_text_width(const char *s, int scale);
/* s cut with "..." to fit max_w pixels (whole UTF-8 characters) into buf. */
void cv_text_ellipsize(const char *s, int scale, int max_w, char *buf, size_t n);
/* cv_text() of s cut to max_w pixels; returns the width drawn. */
int cv_text_fit(struct canvas *c, int x, int y, const char *s, uint32_t rgb, int scale, int max_w);
/* Nearest-neighbour scaled copy of a frame (RGB565 / XRGB1555 / XRGB8888
 * as DRM fourcc) into the rect, multiplied by dim/256. */
void cv_blit_frame(struct canvas *c, int dx, int dy, int dw, int dh, const void *src,
		   int sw, int sh, int pitch, uint32_t fmt, int dim);
/* Packed RGB24 image (thumbnails). */
void cv_blit_rgb(struct canvas *c, int dx, int dy, int dw, int dh, const uint8_t *rgb, int sw, int sh);

/* ------------------------------------------------ frames in core format */
int fmt_bpp(uint32_t fmt);
uint32_t fmt_pack(uint32_t fmt, uint32_t rgb);
uint32_t fmt_unpack(uint32_t fmt, const void *px); /* one pixel -> 0xRRGGBB */

/*
 * One line of UTF-8 text with an opaque background box, clipped to the
 * frame and cut with "..." at its right edge. TrueType when available
 * (drawn into a small XRGB8888 buffer, then packed into the frame's
 * format), else 8x8. The box is frame_text_height(scale) tall.
 */
void frame_text(void *px, int pitch, int w, int h, uint32_t fmt, int x, int y,
		const char *s, int scale, uint32_t fg, uint32_t bg);
int frame_text_height(int scale);
/* Always the 8x8 font (ASCII lines: the FPS overlay), box 10*scale tall. */
void frame_text_8x8(void *px, int pitch, int w, int h, uint32_t fmt, int x, int y,
		    const char *s, int scale, uint32_t fg, uint32_t bg);

/* RGB24 thumbnail, box-filtered down to at most max_w wide (malloc). */
uint8_t *frame_thumbnail(const void *src, int w, int h, int pitch, uint32_t fmt,
			 int max_w, int *tw, int *th);

#endif
