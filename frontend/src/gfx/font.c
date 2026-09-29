/*
 * font.c - see font.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "font.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../third_party/stb/stb_truetype.h"
#include "../i18n/i18n.h"
#include "../ui/util.h"

struct face {
	char *path;
	void *data;
	size_t len;
	stbtt_fontinfo info;
	struct face *next;
};

struct glyph {
	uint32_t cp;          /* 0 = empty slot */
	int16_t x0, y0;       /* bitmap offset from pen / baseline */
	int16_t w, h;
	float adv;
	uint32_t off;         /* in font->bits */
};

struct font {
	struct face *face;
	int px;
	float scale;
	int ascent, descent, height, cap;
	struct glyph *glyphs;  /* open addressing */
	unsigned nslots, nused;
	uint8_t *bits;
	size_t bits_len, bits_cap;
	struct font *next;
};

/*
 * Threads: the UI thread and the prefetch worker (ui/prefetch.c) both draw
 * text. g_fmu guards the face and font lists and every glyph table; it is
 * taken per glyph in the drawing and measuring loops (a glyph pointer and
 * f->bits are only valid until the next glyph is added), so the UI thread
 * waits at most for one glyph rasterization of the worker, never for a
 * whole text. The setters (font_set_*, font_setup_dir) and
 * font_cache_clear() run with the worker stopped.
 */
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static struct face *g_faces;
static struct font *g_fonts;
static char g_default_path[2][1024];
/* The per-glyph fallback chain after the requested font (font_set_fallbacks). */
#define MAX_FALLBACKS 6
static char g_fallback[MAX_FALLBACKS][1024];
static int g_nfallback;

void font_set_default(const char *regular_path, const char *bold_path)
{
	strlcpy_(g_default_path[0], regular_path ? regular_path : "", sizeof(g_default_path[0]));
	strlcpy_(g_default_path[1], bold_path ? bold_path : regular_path ? regular_path : "",
		 sizeof(g_default_path[1]));
}

const char *font_default_path(bool bold)
{
	return g_default_path[bold ? 1 : 0];
}

void font_set_fallbacks(const char *const *paths, int n)
{
	g_nfallback = 0;
	for (int i = 0; i < n && g_nfallback < MAX_FALLBACKS; i++)
		if (paths[i] && *paths[i])
			strlcpy_(g_fallback[g_nfallback++], paths[i], sizeof(g_fallback[0]));
}

int font_fallback_count(void)
{
	return g_nfallback;
}

/*
 * The CJK subsets (third_party/fonts/RSOS-CJK-{JP,SC,KR}.otf, made by
 * po/mkcjkfont.py): the one of the language in use first, so a character
 * the three share (most Han characters) gets that language's glyph shape.
 */
void font_setup_dir(const char *dir)
{
	static const char *const order_ja[] = { "JP", "SC", "KR" };
	static const char *const order_zh[] = { "SC", "JP", "KR" };
	static const char *const order_ko[] = { "KR", "SC", "JP" };
	const char *lang = i18n_language();
	const char *const *order = !strcmp(lang, "zh_CN") ? order_zh : !strcmp(lang, "ko") ? order_ko : order_ja;
	char p[3][1100], reg[1100], bold[1100], subst[1100];
	const char *paths[3];
	int n = 0;

	if (!dir || !*dir)
		return;
	snprintf(reg, sizeof(reg), "%s/DejaVuSans.ttf", dir);
	snprintf(bold, sizeof(bold), "%s/DejaVuSansCondensed-Bold.ttf", dir);
	if (access(bold, R_OK) != 0)
		snprintf(bold, sizeof(bold), "%s/DejaVuSans-Bold.ttf", dir);
	font_set_default(reg, access(bold, R_OK) == 0 ? bold : reg);
	snprintf(subst, sizeof(subst), "%s", dir);
	font_set_substitute_dir(subst);
	for (int i = 0; i < 3; i++) {
		snprintf(p[n], sizeof(p[n]), "%s/RSOS-CJK-%s.otf", dir, order[i]);
		if (access(p[n], R_OK) == 0) {
			paths[n] = p[n];
			n++;
		}
	}
	font_set_fallbacks(paths, n);
}

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

/*
 * stb_truetype trusts the file: every table of the font directory must lie
 * in the file, the tables stbtt_InitFont() reads fixed fields of must be
 * long enough, and the cmap encoding records must lie in the cmap table.
 * (A theme font from a USB stick may be truncated or not a font at all.)
 */
static bool font_tables_ok(const uint8_t *d, size_t len, int off)
{
	const uint8_t *dir, *head = NULL, *hhea = NULL, *maxp = NULL, *loca = NULL;
	uint32_t n, hmtx_l = 0, loca_l = 0, glyf_l = 0;
	bool glyf = false;

	if (off < 0 || (size_t)off > len || len - (size_t)off < 12)
		return false;
	dir = d + off;
	n = be16(dir + 4);
	if ((len - (size_t)off - 12) / 16 < n)
		return false;
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *r = dir + 12 + 16 * i;
		uint32_t o = be32(r + 8), l = be32(r + 12);  /* from the file start */

		if ((uint64_t)o + l > len)
			return false;
		if ((!memcmp(r, "head", 4) && l < 54) || (!memcmp(r, "hhea", 4) && l < 36) ||
		    (!memcmp(r, "maxp", 4) && l < 6))
			return false;
		if (!memcmp(r, "head", 4))
			head = d + o;
		else if (!memcmp(r, "hhea", 4))
			hhea = d + o;
		else if (!memcmp(r, "maxp", 4))
			maxp = d + o;
		else if (!memcmp(r, "hmtx", 4))
			hmtx_l = l;
		else if (!memcmp(r, "loca", 4))
			loca = d + o, loca_l = l;
		else if (!memcmp(r, "glyf", 4))
			glyf = true, glyf_l = l;
		if (!memcmp(r, "cmap", 4)) {
			const uint8_t *t = d + o;
			uint32_t nt;

			if (l < 4 || (l - 4) / 8 < (nt = be16(t + 2)))
				return false;
			for (uint32_t k = 0; k < nt; k++)
				if (be32(t + 4 + 8 * k + 4) > l - 4)
					return false;  /* the subtable's format and length */
		}
	}
	/* the long horizontal metrics, and (TrueType outlines) the glyph
	 * offsets of loca within glyf; the outlines themselves are not checked */
	if (hhea && (uint64_t)be16(hhea + 34) * 4 > hmtx_l)
		return false;
	if (head && maxp && glyf) {
		uint32_t ng = be16(maxp + 4), lf = be16(head + 50);

		if (!loca || (uint64_t)(ng + 1) * (lf ? 4 : 2) > loca_l)
			return false;
		for (uint32_t g = 0; g <= ng; g++)
			if ((lf ? be32(loca + 4 * g) : 2 * be16(loca + 2 * g)) > glyf_l)
				return false;
	}
	return true;
}

static struct face *face_get(const char *path)
{
	struct face *f;
	int fd;
	struct stat st;
	void *m;
	int off;

	for (f = g_faces; f; f = f->next)
		if (!strcmp(f->path, path))
			return f->data ? f : NULL;

	f = xcalloc(1, sizeof(*f));
	f->path = xstrdup(path);
	f->next = g_faces;
	g_faces = f;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		ui_log_once(path, "font: cannot open %s", path);
		return NULL;
	}
	/* 16: stbtt_GetFontOffsetForIndex() reads that much of a collection */
	if (fstat(fd, &st) < 0 || st.st_size < 16) {
		close(fd);
		ui_log_once(path, "font: %s is not a TrueType/OpenType font", path);
		return NULL;
	}
	m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return NULL;
	off = stbtt_GetFontOffsetForIndex(m, 0);
	if (off < 0 || !font_tables_ok(m, (size_t)st.st_size, off) || !stbtt_InitFont(&f->info, m, off)) {
		ui_log_once(path, "font: %s is not a TrueType/OpenType font", path);
		munmap(m, (size_t)st.st_size);
		return NULL;
	}
	f->data = m;
	f->len = (size_t)st.st_size;
	return f;
}

static struct glyph *glyph_slot(struct font *f, uint32_t cp)
{
	unsigned i = (cp * 2654435761u) & (f->nslots - 1);

	while (f->glyphs[i].cp && f->glyphs[i].cp != cp)
		i = (i + 1) & (f->nslots - 1);
	return &f->glyphs[i];
}

static void glyph_grow(struct font *f)
{
	struct glyph *old = f->glyphs;
	unsigned n = f->nslots;

	f->nslots = n ? n * 2 : 256;
	f->glyphs = xcalloc(f->nslots, sizeof(struct glyph));
	for (unsigned i = 0; i < n; i++)
		if (old[i].cp)
			*glyph_slot(f, old[i].cp) = old[i];
	free(old);
}

static struct font *default_font_for(int px);
static struct font *font_lookup(const char *path, int px);

/* Spaces that fonts may lack: drawn as an advance only. */
static bool is_space(uint32_t cp)
{
	return cp == 0xa0 || cp == 0x202f || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200b);
}

/* The first font of the chain (default font, then the fallbacks) that has
 * cp, at the size of f; NULL if none. */
static struct font *fallback_for(struct font *f, uint32_t cp, int *gi)
{
	struct font *df = default_font_for(f->px);

	if (df && df->face != f->face && (*gi = stbtt_FindGlyphIndex(&df->face->info, (int)cp)))
		return df;
	for (int i = 0; i < g_nfallback; i++) {
		struct font *ff = font_lookup(g_fallback[i], f->px);

		if (ff && ff->face != f->face && (*gi = stbtt_FindGlyphIndex(&ff->face->info, (int)cp)))
			return ff;
	}
	return NULL;
}

static const struct glyph *glyph_get(struct font *f, uint32_t cp)
{
	struct glyph *g;
	struct face *face = f->face;
	float scale = f->scale;
	int gi;

	if (cp == 0)
		cp = '?';
	if (f->nused * 2 >= f->nslots)
		glyph_grow(f);
	g = glyph_slot(f, cp);
	if (g->cp)
		return g;

	gi = stbtt_FindGlyphIndex(&face->info, (int)cp);
	if (!gi && is_space(cp)) {
		/* an empty glyph as wide as a space (an ideographic space: 1 em) */
		const struct glyph *sp = glyph_get(f, ' ');
		float adv = cp == 0x3000 ? (float)f->px : cp == 0x200b ? 0.0f : sp->adv;

		g = glyph_slot(f, cp); /* table may have grown */
		memset(g, 0, sizeof(*g));
		g->cp = cp;
		g->adv = adv;
		g->off = (uint32_t)f->bits_len;
		f->nused++;
		return g;
	}
	if (!gi && cp != ' ') {
		int fgi = 0;
		struct font *df = fallback_for(f, cp, &fgi);

		if (df) {
			face = df->face;
			scale = df->scale;
			gi = fgi;
			g = glyph_slot(f, cp); /* the fallback's font_new may not touch f, but be safe */
		} else if (cp != '?') {
			const struct glyph *q = glyph_get(f, '?');
			struct glyph copy = *q;

			g = glyph_slot(f, cp); /* table may have grown */
			*g = copy;
			g->cp = cp;
			f->nused++;
			return g;
		}
	}
	{
		int adv, lsb, x0, y0, x1, y1;
		int w, h;
		struct ui_cost_scope cs;

		ui_cost_begin(&cs, UI_COST_TEXT);
		stbtt_GetGlyphHMetrics(&face->info, gi, &adv, &lsb);
		stbtt_GetGlyphBitmapBox(&face->info, gi, scale, scale, &x0, &y0, &x1, &y1);
		w = x1 - x0;
		h = y1 - y0;
		g->cp = cp;
		g->x0 = (int16_t)x0;
		g->y0 = (int16_t)y0;
		g->adv = (float)adv * scale;
		g->off = (uint32_t)f->bits_len;
		if (w > 0 && h > 0 && w < 1024 && h < 1024) {
			size_t need = f->bits_len + (size_t)w * h;

			if (need > f->bits_cap) {
				f->bits_cap = MAX(need, f->bits_cap * 2);
				f->bits = xrealloc(f->bits, f->bits_cap);
			}
			stbtt_MakeGlyphBitmap(&face->info, f->bits + f->bits_len, w, h, w,
					      scale, scale, gi);
			f->bits_len = need;
			g->w = (int16_t)w;
			g->h = (int16_t)h;
		} else {
			g->w = g->h = 0;
		}
		f->nused++;
		ui_cost_end(&cs);
	}
	return g;
}

/* The advance of one glyph (rasterized if new), under the lock. */
static float glyph_adv(struct font *f, uint32_t cp)
{
	float a;

	if (!f)
		return 0;  /* no usable font (font_get): text has no width */
	pthread_mutex_lock(&g_fmu);
	a = glyph_get(f, cp)->adv;
	pthread_mutex_unlock(&g_fmu);
	return a;
}

static struct font *font_new(struct face *face, int px)
{
	struct font *f = xcalloc(1, sizeof(*f));
	int asc, desc, gap;
	int maxh = 0;

	f->face = face;
	f->px = px;
	f->scale = stbtt_ScaleForMappingEmToPixels(&face->info, (float)px);
	stbtt_GetFontVMetrics(&face->info, &asc, &desc, &gap);
	f->ascent = (int)ceilf((float)asc * f->scale);
	f->descent = (int)floorf((float)desc * f->scale);
	/* ES: max bitmap height over printable ASCII */
	for (int c = 32; c < 127; c++) {
		int x0, y0, x1, y1;

		stbtt_GetCodepointBitmapBox(&face->info, c, f->scale, f->scale,
					    &x0, &y0, &x1, &y1);
		if (y1 - y0 > maxh)
			maxh = y1 - y0;
	}
	f->height = maxh > 0 ? maxh : px;
	{
		int x0, y0, x1, y1;

		stbtt_GetCodepointBitmapBox(&face->info, 'S', f->scale, f->scale,
					    &x0, &y0, &x1, &y1);
		f->cap = -y0 > 0 ? -y0 : (px * 7) / 10;
	}
	glyph_grow(f);
	f->next = g_fonts;
	g_fonts = f;
	return f;
}

static struct font *font_lookup(const char *path, int px)
{
	struct face *face = face_get(path);
	struct font *f;

	if (!face)
		return NULL;
	for (f = g_fonts; f; f = f->next)
		if (f->face == face && f->px == px)
			return f;
	return font_new(face, px);
}

static struct font *default_font_for(int px)
{
	if (!g_default_path[0][0])
		return NULL;
	return font_lookup(g_default_path[0], px);
}

static char g_subst_dir[1024];

void font_set_substitute_dir(const char *dir)
{
	strlcpy_(g_subst_dir, dir ? dir : "", sizeof(g_subst_dir));
}

/*
 * Theme fonts that stb_truetype cannot read (Type 1 files renamed .ttf, as in
 * gbz35, or broken files) are replaced by the closest vendored font.
 */
static struct font *substitute(const char *path, int px)
{
	char low[256], p[1200];
	const char *file;
	size_t i;

	if (!g_subst_dir[0])
		return NULL;
	strlcpy_(low, path_basename(path), sizeof(low));
	for (i = 0; low[i]; i++)
		if (low[i] >= 'A' && low[i] <= 'Z')
			low[i] = (char)(low[i] + 32);
	if (strstr(low, "condensed") || strstr(low, "roboto") || strstr(low, "narrow"))
		file = strstr(low, "bold") ? "RobotoCondensed-Bold.ttf" :
		       strstr(low, "light") || strstr(low, "thin") ? "RobotoCondensed-Light.ttf" :
		       "RobotoCondensed-Regular.ttf";
	else if (strstr(low, "bold") || strstr(low, "black") || strstr(low, "heavy"))
		file = "DejaVuSansCondensed-Bold.ttf";
	else
		return NULL;
	snprintf(p, sizeof(p), "%s/%s", g_subst_dir, file);
	ui_log_once(path, "font: %s unreadable (Type 1 or broken?), using %s", path, file);
	return font_lookup(p, px);
}

struct font *font_get(const char *path, int px)
{
	struct font *f = NULL;

	px = CLAMP(px, 4, 256);
	pthread_mutex_lock(&g_fmu);
	if (path && *path) {
		f = font_lookup(path, px);
		if (!f)
			f = substitute(path, px);
	}
	if (!f)
		f = default_font_for(px);
	pthread_mutex_unlock(&g_fmu);
	/* No default font either (missing or corrupt file): NULL, which every
	 * font_* function takes (nothing drawn, zero sizes) rather than a
	 * crash at the first text. */
	if (!f)
		ui_log_once("font: no usable font", "font: no usable font (%s, default %s): text not drawn",
			    path && *path ? path : "-", g_default_path[0][0] ? g_default_path[0] : "unset");
	return f;
}

void font_cache_clear(void)
{
	struct font *f, *nf;
	struct face *fa, *nfa;

	pthread_mutex_lock(&g_fmu);
	f = g_fonts;
	fa = g_faces;

	while (f) {
		nf = f->next;
		free(f->glyphs);
		free(f->bits);
		free(f);
		f = nf;
	}
	g_fonts = NULL;
	while (fa) {
		nfa = fa->next;
		if (fa->data)
			munmap(fa->data, fa->len);
		free(fa->path);
		free(fa);
		fa = nfa;
	}
	g_faces = NULL;
	pthread_mutex_unlock(&g_fmu);
}

/* f may be NULL (font_get() found no usable font): zero sizes. */
int font_px(const struct font *f) { return f ? f->px : 0; }
int font_height(const struct font *f) { return f ? f->height : 0; }
int font_ascent(const struct font *f) { return f ? f->ascent : 0; }
int font_cap_height(const struct font *f) { return f ? f->cap : 0; }

int font_baseline_in_box(const struct font *f, int top, int h)
{
	return top + (h + font_cap_height(f)) / 2;
}

unsigned utf8_next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	unsigned c = *s;
	int n;

	if (c < 0x80) {
		*ps += 1;
		return c;
	}
	if ((c & 0xe0) == 0xc0) {
		n = 1;
		c &= 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		n = 2;
		c &= 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		n = 3;
		c &= 0x07;
	} else {
		/* stray byte: interpret as Latin-1 (old theme/gamelist files) */
		*ps += 1;
		return c;
	}
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*ps += 1;
			return s[0]; /* invalid sequence: Latin-1 */
		}
		c = (c << 6) | (s[i] & 0x3f);
	}
	*ps += n + 1;
	return c;
}

void utf8_upper(const char *in, char *buf, size_t n)
{
	/* Latin, Greek, Cyrillic, the Turkish i (the language in use) */
	i18n_upper(in, buf, n);
}

/* A line may break before or after this character without a space (Han,
 * kana, Hangul, fullwidth forms). */
static bool cjk_breakable(unsigned c)
{
	return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) || (c >= 0xf900 && c <= 0xfaff) ||
	       (c >= 0xff00 && c <= 0xffef) || (c >= 0x20000 && c <= 0x2fa1f);
}

/* Characters a line must not start with (Japanese/Chinese closing
 * punctuation, small kana, the prolonged sound mark). */
static bool no_line_start(unsigned c)
{
	static const unsigned list[] = {
		0x3001, 0x3002, 0xff0c, 0xff0e, 0xff01, 0xff1f, 0xff1a, 0xff1b, 0xff09, 0x300d, 0x300f,
		0x3011, 0x3015, 0x3009, 0x300b, 0x30fc, 0x2026, 0x3063, 0x30c3, 0x3083, 0x3085, 0x3087,
		0x30e3, 0x30e5, 0x30e7, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x30a1, 0x30a3, 0x30a5,
		0x30a7, 0x30a9, 0x30fb, '.', ',', '!', '?', ')', ':', ';',
	};

	for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
		if (list[i] == c)
			return true;
	return false;
}

int font_text_width(struct font *f, const char *s, int len)
{
	const char *e = len < 0 ? s + strlen(s) : s + len;
	float x = 0;

	while (s < e && *s) {
		unsigned c = utf8_next(&s);

		if (c == '\n')
			break;
		x += glyph_adv(f, c);
	}
	return (int)ceilf(x);
}

static int draw_mode(struct gfx_surface *s, struct font *f, int x, int y,
		     const char *text, int len, gfx_color c, bool erase)
{
	const char *e;
	float pen = (float)x;

	if (!f || !text)
		return 0;
	e = len < 0 ? text + strlen(text) : text + len;
	while (text < e && *text) {
		unsigned cp = utf8_next(&text);
		const struct glyph *g;

		if (cp == '\n')
			break;
		pthread_mutex_lock(&g_fmu);
		g = glyph_get(f, cp);
		if (g->w) {
			int gx = (int)floorf(pen + 0.5f) + g->x0;
			int gy = y + g->y0;

			if (gx < s->clip.x + s->clip.w && gx + g->w > s->clip.x &&
			    gy < s->clip.y + s->clip.h && gy + g->h > s->clip.y)
			{
				if (erase)
					gfx_mask_erase(s, f->bits + g->off, g->w, gx, gy, g->w, g->h);
				else
					gfx_mask(s, f->bits + g->off, g->w, gx, gy, g->w, g->h, c);
			}
		}
		pen += g->adv;
		pthread_mutex_unlock(&g_fmu);
	}
	return (int)ceilf(pen) - x;
}

int font_wrap(struct font *f, const char *text, int max_w,
	      struct text_line *out, int max_lines)
{
	int n = 0;
	const char *line = text;

	while (*line && n < max_lines) {
		/* last_break: where the line can end; break_next: where the next
		 * one then starts (after a space, or right there between CJK
		 * characters, which need no space) */
		const char *p = line, *last_break = NULL, *break_next = NULL;
		float w = 0, w_at_break = 0;
		unsigned prev = 0;

		for (;;) {
			const char *q = p;
			unsigned c;
			float adv;

			if (!*p || *p == '\n')
				break;
			c = utf8_next(&q);
			adv = glyph_adv(f, c);
			if (c == ' ') {
				last_break = p;
				break_next = q;
				w_at_break = w;
			} else if (p > line && prev != ' ' && (cjk_breakable(c) || cjk_breakable(prev)) &&
				   !no_line_start(c)) {
				last_break = p;
				break_next = p;
				w_at_break = w;
			}
			if (max_w > 0 && w + adv > (float)max_w && p > line) {
				if (last_break && last_break > line) {
					out[n].start = (int)(line - text);
					out[n].len = (int)(last_break - line);
					out[n].width = (int)ceilf(w_at_break);
					n++;
					line = break_next;
				} else {
					out[n].start = (int)(line - text);
					out[n].len = (int)(p - line);
					out[n].width = (int)ceilf(w);
					n++;
					line = p;
				}
				goto next_line;
			}
			w += adv;
			prev = c;
			p = q;
		}
		out[n].start = (int)(line - text);
		out[n].len = (int)(p - line);
		out[n].width = (int)ceilf(w);
		n++;
		line = *p == '\n' ? p + 1 : p;
		if (!*line)
			break;
next_line:
		while (*line == ' ')
			line++;
	}
	return n;
}

void font_ellipsize(struct font *f, const char *text, int max_w, char *buf, size_t n)
{
	int w = font_text_width(f, text, -1);
	const char *p = text;
	float x, ell;
	size_t o = 0;

	if (w <= max_w || max_w <= 0) {
		/* stop at the first newline */
		size_t l = strcspn(text, "\n");

		if (l >= n) {
			/* never cut a UTF-8 sequence */
			l = n - 1;
			while (l > 0 && ((unsigned char)text[l] & 0xc0) == 0x80)
				l--;
		}
		memcpy(buf, text, l);
		buf[l] = 0;
		return;
	}
	ell = (float)font_text_width(f, "...", 3);
	x = 0;
	while (*p && *p != '\n') {
		const char *q = p;
		unsigned c = utf8_next(&q);
		float adv = glyph_adv(f, c);

		if (x + adv + ell > (float)max_w)
			break;
		if (o + (size_t)(q - p) + 4 >= n)
			break;
		memcpy(buf + o, p, (size_t)(q - p));
		o += (size_t)(q - p);
		x += adv;
		p = q;
	}
	while (o > 0 && buf[o - 1] == ' ')
		o--;
	memcpy(buf + o, "...", 4);
}

int font_draw(struct gfx_surface *s, struct font *f, int x, int y,
	      const char *text, int len, gfx_color c)
{
	return draw_mode(s, f, x, y, text, len, c, false);
}

int font_erase(struct gfx_surface *s, struct font *f, int x, int y,
	       const char *text, int len)
{
	return draw_mode(s, f, x, y, text, len, 0, true);
}
