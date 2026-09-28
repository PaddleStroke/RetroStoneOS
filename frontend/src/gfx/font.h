/*
 * font.h - TrueType text with stb_truetype and a glyph cache.
 *
 * A struct font is one face file at one pixel size (the EM size, as
 * FreeType's FT_Set_Pixel_Sizes does, so ES theme font sizes match).
 * Fonts are shared: font_get() returns the same object for the same
 * (path, size). Glyphs are rasterized once, on first use, into 8-bit
 * coverage masks kept in a per-font cache. Missing glyphs fall back to the
 * default font (DejaVu Sans), then to the fallback chain (CJK), then '?'.
 * Lines wrap at spaces, and between CJK characters.
 *
 * Text layout follows EmulationStation: a line box is height * lineSpacing
 * and the capital height is centred in it.
 */
#ifndef RSOS_GFX_FONT_H
#define RSOS_GFX_FONT_H

#include <stdbool.h>
#include "gfx.h"

struct font;

/* Default font files; must be set before the first font_get(NULL, ...). */
void font_set_default(const char *regular_path, const char *bold_path);
const char *font_default_path(bool bold);

/* Directory of the vendored fonts used to replace unreadable theme fonts. */
void font_set_substitute_dir(const char *dir);

/*
 * Per-glyph fallback chain: a character the requested font lacks is taken
 * from the default font (DejaVu Sans: Latin, Greek, Cyrillic), then from
 * these fonts in order (the CJK subsets), else drawn as '?'. At most 6.
 */
void font_set_fallbacks(const char *const *paths, int n);
int font_fallback_count(void);
/* The standard set-up from the fonts directory (/usr/share/rsos/fonts):
 * the defaults, the substitutes and the CJK fallbacks ordered for the
 * language in use (i18n_language()). Call again after a language switch,
 * then font_cache_clear() (glyphs are cached per font). */
void font_setup_dir(const char *dir);

/* path NULL/"" = default regular font. px is clamped to 4..256.
 * Never returns NULL once a default font exists. */
struct font *font_get(const char *path, int px);
/* Frees every font and the file mappings (on relayout or exit). */
void font_cache_clear(void);

int font_px(const struct font *f);
int font_height(const struct font *f);     /* ES mMaxGlyphHeight */
int font_ascent(const struct font *f);     /* baseline offset from line top */
int font_cap_height(const struct font *f); /* height of 'S' above baseline */

/* Width of the first len bytes of utf8 text (len < 0: whole string). */
int font_text_width(struct font *f, const char *s, int len);

/* Draws one line; y is the baseline. Returns the advance in pixels. */
int font_draw(struct gfx_surface *s, struct font *f, int x, int y,
	      const char *text, int len, gfx_color c);

/* Punches the text out of the surface (alpha too), for icons. */
int font_erase(struct gfx_surface *s, struct font *f, int x, int y,
	       const char *text, int len);

/* Baseline of a line whose box starts at top and is h tall (ES rule). */
int font_baseline_in_box(const struct font *f, int top, int h);

/* --------------------------------------------------------------- layout */
enum font_align { FONT_LEFT = 0, FONT_CENTER, FONT_RIGHT };

struct text_line {
	int start, len;   /* byte range in the source text */
	int width;
};

/*
 * Word-wraps text to max_w (0 = no wrapping, only explicit newlines).
 * Returns the number of lines written to out (at most max_lines).
 */
int font_wrap(struct font *f, const char *text, int max_w,
	      struct text_line *out, int max_lines);

/* Writes text truncated with "..." to fit max_w into buf. */
void font_ellipsize(struct font *f, const char *text, int max_w,
		    char *buf, size_t n);

/* UTF-8 helpers */
unsigned utf8_next(const char **s);
/* Uppercase of UTF-8 text into buf, for the language in use (i18n_upper). */
void utf8_upper(const char *in, char *buf, size_t n);

#endif
