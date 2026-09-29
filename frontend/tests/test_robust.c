/*
 * test_robust.c - broken or odd files (a theme, a gamelist, a font from a
 * USB stick) must not freeze or crash the menu. Small reproducers of the
 * review findings; each section runs under a watchdog (a hang fails).
 *
 *   test_robust WORKDIR FONTS_DIR
 *
 * 1. SVG sizes: an extreme aspect ratio or a huge width is rasterized
 *    bounded; gfx_image_new() refuses sizes that would wrap on 32-bit
 * 2. a theme that includes itself (4 times) and a fan-out of includes
 * 3. XML text split in 200,000 pieces: linear, capped at 64 KiB
 * 4. XML nested 100,000 deep with 100,000 unmatched close tags: bounded
 * 5. no usable font (missing default, truncated theme font): text is not
 *    drawn, nothing crashes
 * 6. a catalog whose hash chain loops on itself: lookups end
 */
#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gfx/font.h"
#include "gfx/gfx.h"
#include "gfx/image.h"
#include "i18n/i18n.h"
#include "i18n/i18n_cat.h"
#include "theme/theme.h"
#include "ui/util.h"
#include "ui/xml.h"

static char W[512];
static const char *g_fonts;
static int g_fail;
static const char *g_section = "";

#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); g_fail++; } else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void on_alarm(int sig)
{
	(void)sig;
	printf("  FAIL: %s did not finish (a hang)\n", g_section);
	fflush(stdout);
	_exit(1);
}

/* Each section must end within 60 s (ASan included; linear code takes
 * well under a second, the old quadratic or exponential code hours). */
static void section(const char *name)
{
	g_section = name;
	printf("%s\n", name);
	fflush(stdout);
	alarm(60);
}

static double now_s(void)
{
	return (double)ui_now_us() / 1e6;
}

static void put(const char *rel, const void *data, size_t n)
{
	char p[1024];
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", W, rel);
	f = fopen(p, "wb");
	if (!f || fwrite(data, 1, n, f) != n) {
		printf("cannot write %s: %s\n", p, strerror(errno));
		exit(2);
	}
	fclose(f);
}

static void puts_(const char *rel, const char *text)
{
	put(rel, text, strlen(text));
}

/* --------------------------------------------------------------- 1. SVG */
static void test_svg(void)
{
	struct gfx_image *img;
	struct gfx_surface s;

	section("1. SVG sizes and gfx_image_new()");
	img = img_from_svg_string("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4096\" height=\"4000000000\">"
				  "<rect width=\"10\" height=\"10\"/></svg>", 64, 48, 0xffffffffu);
	CHECK(img && img->w == 64 && img->h == 48, "a 4096x4e9 SVG in a 64x48 element: a 64x48 image, bounded");
	gfx_image_free(img);
	img = img_from_svg_string("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1e30\" height=\"1\">"
				  "<rect width=\"10\" height=\"10\"/></svg>", 64, 0, 0xffffffffu);
	CHECK(!img || (img->w == 64 && img->h >= 1), "width=1e30: no crash (%dx%d)", img ? img->w : 0,
	      img ? img->h : 0);
	gfx_image_free(img);
	img = img_from_svg_string("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"2\">"
				  "<rect width=\"1000\" height=\"2\" fill=\"#fff\"/></svg>", 1280, 10, 0xffffffffu);
	CHECK(img && img->w == 1280 && img->h == 10, "a stretched separator line still renders (1280x10)");
	gfx_image_free(img);
	img = img_from_svg_string("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"/>",
				  100000, 100000, 0xffffffffu);
	CHECK(!img, "a 100000x100000 SVG element is refused");
	gfx_image_free(img);

	CHECK(!gfx_image_new(GFX_IMAGE_MAX_DIM + 1, 1), "gfx_image_new(%d, 1) refused", GFX_IMAGE_MAX_DIM + 1);
	CHECK(!gfx_image_new(1, 1 << 20), "gfx_image_new(1, 1 << 20) refused");
	CHECK(!gfx_image_new(65536, 65536), "gfx_image_new(65536, 65536) refused (wraps on 32-bit)");
	img = gfx_image_new(100, 100);
	CHECK(img && img->w == 100 && img->h == 100, "gfx_image_new(100, 100) works");
	gfx_image_free(img);
	/* a refused image: the helpers and a surface on it draw nothing */
	gfx_surface_from_image(&s, NULL);
	gfx_fill(&s, 0, 0, 100, 100, 0xffffffffu);
	gfx_fill_round(&s, 0, 0, 100, 100, 8, 0xffffffffu);
	gfx_image_update_flags(NULL);
	gfx_image_tint(NULL, 0x80808080u);
	CHECK(!gfx_image_scale(NULL, 10, 10), "NULL images are accepted");
	img_thread_exit();
}

/* ------------------------------------------------------------- 2. theme */
static void test_theme(void)
{
	char p[1024];
	struct theme *t;
	double t0;
	const struct theme_view *v;

	section("2. theme includes");
	puts_("t.xml", "<theme><formatVersion>7</formatVersion>"
	      "<include>./t.xml</include><include>./t.xml</include>"
	      "<include>./t.xml</include><include>./t.xml</include>"
	      "<view name=\"system\"><text name=\"x\"><pos>0.25 0.5</pos></text></view></theme>");
	snprintf(p, sizeof(p), "%s/t.xml", W);
	t0 = now_s();
	t = theme_load_file(p, NULL);
	t0 = now_s() - t0;
	v = theme_view(t, "system");
	CHECK(v && v->n == 1, "a theme including itself 4 times loads (%.3f s)", t0);
	theme_free(t);

	/* no cycle, but 8 x 8 x 8 includes: at most 64 files are read */
	puts_("a.xml", "<theme><include>b.xml</include><include>b.xml</include><include>b.xml</include>"
	      "<include>b.xml</include><include>b.xml</include><include>b.xml</include><include>b.xml</include>"
	      "<include>b.xml</include><view name=\"system\"><text name=\"a\"/></view></theme>");
	puts_("b.xml", "<theme><include>c.xml</include><include>c.xml</include><include>c.xml</include>"
	      "<include>c.xml</include><include>c.xml</include><include>c.xml</include><include>c.xml</include>"
	      "<include>c.xml</include><view name=\"system\"><text name=\"b\"/></view></theme>");
	puts_("c.xml", "<theme><include>d.xml</include><include>d.xml</include><include>d.xml</include>"
	      "<include>d.xml</include><include>d.xml</include><include>d.xml</include><include>d.xml</include>"
	      "<include>d.xml</include><view name=\"system\"><text name=\"c\"/></view></theme>");
	puts_("d.xml", "<theme><include>c.xml</include><view name=\"system\"><text name=\"d\"/></view></theme>");
	snprintf(p, sizeof(p), "%s/a.xml", W);
	t0 = now_s();
	t = theme_load_file(p, NULL);
	t0 = now_s() - t0;
	v = theme_view(t, "system");
	CHECK(v && v->n == 4, "an 8x8x8 include fan-out (and a c/d cycle) loads, 4 elements (%.3f s)", t0);
	theme_free(t);
}

/* --------------------------------------------------------------- 3/4. XML */
static int max_depth(const struct xml_node *n, int d)
{
	int m = d;

	for (; n; n = n->next) {
		int c = n->child ? max_depth(n->child, d + 1) : d;

		if (c > m)
			m = c;
	}
	return m;
}

static void test_xml(void)
{
	const char *head = "<gameList><game><desc>", *tail = "</desc></game></gameList>";
	const int pieces = 200000;
	size_t n = strlen(head) + (size_t)pieces * 9 + strlen(tail), o = 0;
	char *buf = malloc(n + 1);
	struct xml_doc *doc;
	const char *d;
	double t0;

	section("3. XML text in 200,000 pieces");
	memcpy(buf, head, strlen(head));
	o = strlen(head);
	for (int i = 0; i < pieces; i++, o += 9)
		memcpy(buf + o, "xx<!---->", 9);
	memcpy(buf + o, tail, strlen(tail) + 1);
	t0 = now_s();
	doc = xml_parse(buf, n);
	t0 = now_s() - t0;
	d = doc->root ? xml_child_text(xml_child(doc->root, "game"), "desc") : NULL;
	CHECK(d && strlen(d) == 64 * 1024 && d[0] == 'x', "parsed in %.3f s, the text capped at 64 KiB (%zu)", t0,
	      d ? strlen(d) : 0);
	xml_free(doc);
	free(buf);

	/* the usual cases still parse as before */
	d = "<a> x <b/> y <![CDATA[&lt;]]>&amp;</a><c><d>t</c><e/>";
	doc = xml_parse(d, strlen(d));
	d = xml_child_text(&doc->top, "a");
	CHECK(d && !strcmp(d, "x  y &lt;&"), "mixed text, CDATA, entities: \"%s\"", d ? d : "(null)");
	CHECK(xml_child(&doc->top, "e") && xml_child_text(xml_child(&doc->top, "c"), "d") &&
	      !strcmp(xml_child_text(xml_child(&doc->top, "c"), "d"), "t"),
	      "a close tag closes the open inner elements");
	xml_free(doc);

	section("4. XML 100,000 deep, 100,000 unmatched close tags");
	n = 100000 * 3 + 100000 * 4;
	buf = malloc(n + 1);
	for (int i = 0; i < 100000; i++)
		memcpy(buf + 3 * i, "<a>", 3);
	for (int i = 0; i < 100000; i++)
		memcpy(buf + 300000 + 4 * i, "</b>", 4);
	buf[n] = 0;
	t0 = now_s();
	doc = xml_parse(buf, n);
	t0 = now_s() - t0;
	CHECK(max_depth(doc->top.child, 1) == 257, "parsed in %.3f s, 256 levels deep at most (%d)", t0,
	      max_depth(doc->top.child, 1));
	xml_free(doc);
	free(buf);
}

/* -------------------------------------------------------------- 5. fonts */
static void test_fonts(void)
{
	struct gfx_image *img = gfx_image_new(64, 32);
	struct gfx_surface s;
	struct text_line lines[4];
	struct font *f;
	char p[1024], trunc[1024], buf[64];
	size_t len;
	char *data;

	section("5. no usable font");
	gfx_surface_from_image(&s, img);
	font_cache_clear();
	font_set_default("/nonexistent/DejaVuSans.ttf", NULL);
	font_set_substitute_dir(NULL);
	f = font_get(NULL, 20);
	CHECK(!f, "no default font: font_get() gives NULL");
	CHECK(font_text_width(f, "abc", -1) == 0 && font_height(f) == 0, "NULL font: zero sizes");
	font_draw(&s, f, 0, 20, "abc", -1, 0xffffffffu);
	font_erase(&s, f, 0, 20, "abc", -1);
	font_ellipsize(f, "a long text", 10, buf, sizeof(buf));
	CHECK(font_wrap(f, "a b c", 10, lines, 4) >= 1 && font_baseline_in_box(f, 0, 10) == 5,
	      "NULL font: draw, erase, wrap and ellipsize do nothing");

	/* a theme font cut short (a table past the end of the file) */
	snprintf(p, sizeof(p), "%s/DejaVuSans.ttf", g_fonts);
	data = file_read(p, &len);
	CHECK(data && len > 4096, "%s read", p);
	if (data) {
		put("cut.ttf", data, 4096);
		snprintf(trunc, sizeof(trunc), "%s/cut.ttf", W);
		f = font_get(trunc, 20);
		CHECK(!f, "a truncated font is refused (no default: NULL)");
		font_set_default(p, NULL);
		f = font_get(trunc, 20);
		CHECK(f && font_text_width(f, "abc", -1) > 0, "a truncated font falls back to the default font");
		font_draw(&s, f, 0, 20, "abc", -1, 0xffffffffu);
		free(data);
	}
	font_cache_clear();
	gfx_image_free(img);
}

/* ---------------------------------------------------------------- 6. i18n */
static void test_i18n(void)
{
	/* header | 1 bucket | 1 entry (next = itself) | "zzz" | "v" */
	struct {
		struct i18n_cat_header h;
		uint32_t bucket;
		struct i18n_cat_entry e;
		char key[4];
		char val[2];
	} __attribute__((packed)) c;
	char dir[700];
	double t0;
	const char *r;

	section("6. i18n catalog with a looping chain");
	memset(&c, 0, sizeof(c));
	memcpy(c.h.magic, I18N_CAT_MAGIC, 8);
	c.h.endian = I18N_CAT_ENDIAN;
	c.h.size = sizeof(c);
	c.h.count = 1;
	c.h.nbuckets = 1;
	c.h.buckets = offsetof(__typeof__(c), bucket);
	c.h.entries = offsetof(__typeof__(c), e);
	c.h.nplurals = 1;
	c.bucket = 1;
	c.e.hash = 0xdeadbeef;
	c.e.next = 1;
	c.e.key = offsetof(__typeof__(c), key);
	c.e.key_len = 3;
	c.e.val = offsetof(__typeof__(c), val);
	c.e.val_len = 2;
	c.e.nforms = 1;
	memcpy(c.key, "zzz", 4);
	memcpy(c.val, "v", 2);
	snprintf(dir, sizeof(dir), "%s/locale", W);
	mkdir(dir, 0755);
	put("locale/fr.cat", &c, sizeof(c));
	i18n_set_dir(dir);
	CHECK(i18n_set_language("fr") == 0, "the catalog loads");
	t0 = now_s();
	r = i18n_gettext("hello");
	CHECK(!strcmp(r, "hello"), "a lookup ends (%.3f s)", now_s() - t0);
	i18n_set_language("en");
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: test_robust WORKDIR FONTS_DIR\n");
		return 2;
	}
	snprintf(W, sizeof(W), "%s", argv[1]);
	g_fonts = argv[2];
	mkdir(W, 0755);
	signal(SIGALRM, on_alarm);
	ui_log_set(NULL, NULL, UI_LOG_WARN);

	test_svg();
	test_theme();
	test_xml();
	test_fonts();
	test_i18n();
	alarm(0);
	printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
	return g_fail ? 1 : 0;
}
