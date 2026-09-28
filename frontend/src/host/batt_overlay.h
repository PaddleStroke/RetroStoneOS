/*
 * batt_overlay.h - the in-game battery indicator: a small ARGB8888 picture
 * (battery icon, "70%", a charging bolt) that the game process puts on the
 * display's overlay plane (display_set_overlay()). Pure drawing and
 * parsing here, the timing and the display calls are in host.c.
 * Design: docs/host-design.md §8 ("Battery overlay").
 */
#ifndef RSOS_BATT_OVERLAY_H
#define RSOS_BATT_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

/* The supervisor's published value: "<percent> <charging>\n" (main.c). */
#define BOV_FILE "/run/rsos/battery"

/* Buffer size at scale 1 (the widest content: bolt + "100%"). */
#define BOV_W 72
#define BOV_H 20
#define BOV_MAX_SCALE 2

#define BOV_LOW_PCT 15      /* amber at or below */
#define BOV_CRIT_PCT 7      /* red at or below */

/*
 * Draws the indicator into px (BOV_W*scale x BOV_H*scale, stride in
 * pixels): a dark translucent pill, the battery icon filled to pct, the
 * percentage in the 8x8 font, a bolt when charging. The pill hugs the
 * right edge of the buffer when right is true (right-hand corners), else
 * the left edge; the rest is transparent. Every pixel is either fully
 * transparent, fully opaque, or black with alpha (so it looks the same
 * whether the plane blends premultiplied or straight alpha).
 * Returns the pill width in pixels.
 */
int bov_render(uint32_t *px, int stride, int scale, int pct, bool charging, bool right);

/* Parses "<percent> <charging>". 0, or -1 if malformed. pct -1 = unknown. */
int bov_parse(const char *text, int *pct, bool *charging);

/* Reads and parses the file. 0 or -errno / -1. */
int bov_read(const char *path, int *pct, bool *charging);

/*
 * The overlay strip: the FPS/benchmark text (1-2 lines, first white, second
 * yellow) on a dark pill, and the battery pill (pct >= 0) at the other end,
 * so both share the one overlay plane. w1 = width at scale 1 (the screen
 * width minus the margins, divided by scale); the buffer is (w1 * scale) x
 * ovl_strip_height() * scale, rows of w1 * scale pixels. Text goes on the
 * side away from the battery (left when there is none). Returns the height.
 */
#define OVL_MAX_LINES 3
/* In the strip's text, byte 0x10 draws a play triangle (the fast-forward
 * indicator "\x10\x10 x3"). */
#define OVL_GLYPH_PLAY 0x10
int ovl_strip_height(int nlines);
int ovl_render_strip(uint32_t *px, int w1, int scale, const char *const *lines, int nlines, int pct,
		     bool charging, bool batt_right);

#endif
