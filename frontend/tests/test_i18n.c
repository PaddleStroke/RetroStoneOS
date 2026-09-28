/*
 * test_i18n - translations and the text renderer's fallback chain
 * (make check-i18n).
 *
 *   test_i18n CHECK_DIR LOCALE_DIR FONTS_DIR
 *
 * CHECK_DIR/cat/da.cat is tests/i18n-test.po compiled by rsos-i18n (the
 * Makefile does it): catalog round trip, contexts, escapes, a positional
 * format, entries the compiler must have dropped, fallback to English.
 * LOCALE_DIR holds the real catalogs: plural forms of fr, pl, ru (and cs,
 * uk), the locale formats of fr, the language list. FONTS_DIR: a string in
 * Japanese, Chinese and Korean rendered with DejaVu Sans + the CJK subsets
 * must give real glyphs (non-empty boxes, not the '?' of a missing glyph),
 * and CJK text wraps without spaces.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gfx/font.h"
#include "../src/gfx/gfx.h"
#include "../src/i18n/i18n.h"
#include "../src/i18n/i18n_cat.h"

static int g_fail, g_ok;

#define CHECK(cond, ...) do { \
		if (cond) { \
			g_ok++; \
		} else { \
			g_fail++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static void test_catalog(const char *dir)
{
	const char *s;
	char buf[128];

	i18n_set_dir(dir);
	CHECK(i18n_set_language("da") == 0, "the test catalog da.cat loads from %s", dir);
	CHECK(!strcmp(i18n_language(), "da"), "language is da (%s)", i18n_language());
	CHECK(i18n_catalog_entries() == 7, "7 usable entries (2 bad formats, 1 fuzzy, 1 empty dropped): %d",
	      i18n_catalog_entries());
	CHECK(!strcmp(_("Settings"), "Indstillinger"), "a plain entry");
	CHECK(!strcmp(C_("settings", "Power"), "Tænd/sluk") && !strcmp(C_("power source", "Power"), "Strømkilde"),
	      "two contexts of one English word");
	CHECK(!strcmp(_("Power"), "Power"), "no context: not translated (English)");
	CHECK(!strcmp(_("Line one\nLine \"two\""), "Linje et\nLinje \"to\""), "escapes");
	CHECK(!strcmp(_("Unicode"), "日本語 · Ελληνικά · Русский · ÆØÅ"), "UTF-8 text");
	/* a positional format */
	snprintf(buf, sizeof(buf), _("Copied %d files to %s"), 12, "STICK");
	CHECK(!strcmp(buf, "Til STICK: 12 filer"), "positional arguments: %s", buf);
	/* dropped by the compiler: a %s for a %d, an extra conversion; fuzzy; empty */
	s = "Slot %d is empty";
	CHECK(_(s) == s, "a wrong conversion type was dropped: English");
	CHECK(!strcmp(_("Hot, slowing down"), "Hot, slowing down"), "an added conversion was dropped");
	CHECK(!strcmp(_("Restart"), "Restart"), "fuzzy entries are ignored");
	CHECK(!strcmp(_("Power off"), "Power off"), "an empty translation is English");
	s = "Not in the catalog at all";
	CHECK(_(s) == s, "a missing string returns the English pointer itself");
	/* Polish plural rule of the test catalog */
	{
		static const struct { unsigned long n; const char *want; } pl[] = {
			{ 1, "(one)" }, { 2, "(few)" }, { 4, "(few)" }, { 5, "(many)" }, { 11, "(many)" },
			{ 12, "(many)" }, { 21, "(many)" }, { 22, "(few)" }, { 25, "(many)" }, { 0, "(many)" },
			{ 104, "(few)" }, { 112, "(many)" },
		};

		for (size_t i = 0; i < sizeof(pl) / sizeof(pl[0]); i++) {
			snprintf(buf, sizeof(buf), _n("%d game in %s", "%d games in %s", pl[i].n), (int)pl[i].n, "x");
			CHECK(strstr(buf, pl[i].want) != NULL, "Polish rule, n=%lu: %s", pl[i].n, buf);
		}
	}
	/* back to English, unknown and missing languages */
	CHECK(i18n_set_language("en") == 0 && !strcmp(_("Settings"), "Settings"), "English");
	CHECK(i18n_set_language("xx") < 0 && !strcmp(i18n_language(), "en"), "unknown language: English");
	CHECK(i18n_set_language("fi") < 0 && !strcmp(_("Settings"), "Settings"),
	      "a known language without a catalog: English");
	CHECK(i18n_set_language("da") == 0 && !strcmp(_("Settings"), "Indstillinger"),
	      "a catalog loaded before is reused");
	s = _("Settings");
	i18n_set_language("en");
	CHECK(!strcmp(s, "Indstillinger"), "a translation stays valid after a switch (never unmapped)");
}

static void test_plural_eval(void)
{
	/* the expressions of the shipped languages, in the evaluator itself */
	const char *cs = "(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2";
	const char *ru = "(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2)";

	CHECK(i18n_plural_eval("(n > 1)", 0) == 0 && i18n_plural_eval("(n > 1)", 1) == 0 &&
	      i18n_plural_eval("(n > 1)", 2) == 1, "French: 0 and 1 singular");
	CHECK(i18n_plural_eval(cs, 1) == 0 && i18n_plural_eval(cs, 3) == 1 && i18n_plural_eval(cs, 5) == 2 &&
	      i18n_plural_eval(cs, 22) == 2, "Czech");
	CHECK(i18n_plural_eval(ru, 1) == 0 && i18n_plural_eval(ru, 21) == 0 && i18n_plural_eval(ru, 11) == 2 &&
	      i18n_plural_eval(ru, 3) == 1 && i18n_plural_eval(ru, 23) == 1 && i18n_plural_eval(ru, 13) == 2 &&
	      i18n_plural_eval(ru, 0) == 2, "Russian");
	CHECK(i18n_plural_eval("0", 7) == 0, "one form");
	CHECK(i18n_plural_eval("(n != 1", 2) < 0 && i18n_plural_eval("n %% 0", 2) < 0 &&
	      i18n_plural_eval("n / 0", 2) < 0 && i18n_plural_eval("x", 2) < 0, "invalid expressions");
}

/* The forms of the real catalogs: which n share a translation. */
static void same_forms(const char *lang, const unsigned long *groups, size_t n, const char *msg1,
		       const char *msgn)
{
	char a[160], b[160];

	if (i18n_set_language(lang) < 0) {
		CHECK(0, "catalog %s missing", lang);
		return;
	}
	CHECK(strcmp(_n(msg1, msgn, 5), msgn) != 0, "%s: \"%s\" is translated", lang, msgn);
	/* groups: pairs (n1, n2, same?) */
	for (size_t i = 0; i + 2 < n; i += 3) {
		snprintf(a, sizeof(a), "%s", _n(msg1, msgn, groups[i]));
		snprintf(b, sizeof(b), "%s", _n(msg1, msgn, groups[i + 1]));
		CHECK((strcmp(a, b) == 0) == (groups[i + 2] != 0), "%s: forms of %lu and %lu should %sbe the same "
		      "(\"%s\" / \"%s\")", lang, groups[i], groups[i + 1], groups[i + 2] ? "" : "not ", a, b);
	}
}

static void test_real_catalogs(const char *dir)
{
	/* n1, n2, same? */
	static const unsigned long fr[] = { 0, 1, 1, 1, 2, 0, 2, 5, 1 };
	static const unsigned long pl[] = { 1, 2, 0, 2, 3, 1, 2, 5, 0, 5, 12, 1, 22, 2, 1, 21, 5, 1, 12, 22, 0 };
	static const unsigned long ru[] = { 1, 21, 1, 1, 2, 0, 2, 24, 1, 2, 5, 0, 5, 11, 1, 11, 21, 0, 12, 22, 0 };
	static const unsigned long cs[] = { 1, 2, 0, 2, 4, 1, 4, 5, 0, 5, 22, 1 };
	char buf[64];

	i18n_set_dir(dir);
	same_forms("fr", fr, sizeof(fr) / sizeof(fr[0]), "%d game available", "%d games available");
	same_forms("pl", pl, sizeof(pl) / sizeof(pl[0]), "%d game available", "%d games available");
	same_forms("ru", ru, sizeof(ru) / sizeof(ru[0]), "%d game available", "%d games available");
	same_forms("uk", ru, sizeof(ru) / sizeof(ru[0]), "%d game available", "%d games available");
	same_forms("cs", cs, sizeof(cs) / sizeof(cs[0]), "%d game available", "%d games available");
	/* one form: every n the same, translated (not the English plural) */
	{
		static const unsigned long one[] = { 1, 5, 1, 0, 21, 1 };

		same_forms("ja", one, sizeof(one) / sizeof(one[0]), "%d game available", "%d games available");
		same_forms("id", one, sizeof(one) / sizeof(one[0]), "%d game available", "%d games available");
	}
	/* French conventions */
	i18n_set_language("fr");
	i18n_format_size(1610612736ull, buf, sizeof(buf));
	CHECK(!strcmp(buf, "1,5 Go"), "fr size: %s", buf);
	i18n_format_number(12345.5, 1, buf, sizeof(buf));
	CHECK(!strcmp(buf, "12\xe2\x80\xaf" "345,5"), "fr number: %s", buf);
	CHECK(!strcmp(_("Settings"), "Param\xc3\xa8tres"), "fr: Settings");
	CHECK(i18n_retro_language() == 2, "fr: RETRO_LANGUAGE_FRENCH");
	i18n_set_language("en");
	i18n_format_size(1610612736ull, buf, sizeof(buf));
	CHECK(!strcmp(buf, "1.5 GB"), "en size: %s", buf);
	i18n_format_number(12345.5, 1, buf, sizeof(buf));
	CHECK(!strcmp(buf, "12,345.5"), "en number: %s", buf);
	/* every shipped language has a catalog that loads */
	for (int i = 0; i < i18n_lang_count(); i++) {
		const struct i18n_lang *l = i18n_lang_at(i);

		if (!strcmp(l->code, "en"))
			continue;
		CHECK(i18n_set_language(l->code) == 0 && i18n_catalog_entries() > 400, "%s (%s): %d entries",
		      l->code, l->english, i18n_catalog_entries());
	}
	CHECK(i18n_lang_find("pt-BR") && !strcmp(i18n_lang_find("pt-BR")->code, "pt_BR") &&
	      i18n_lang_find("fr_FR.UTF-8") && !strcmp(i18n_lang_find("fr_FR.UTF-8")->code, "fr"),
	      "language codes: pt-BR, fr_FR.UTF-8");
	i18n_set_language("en");
}

static void test_upper(void)
{
	char buf[128];

	i18n_set_language("en");
	i18n_upper("été à l'œuf", buf, sizeof(buf));
	CHECK(!strcmp(buf, "ÉTÉ À L'ŒUF"), "French uppercase: %s", buf);
	i18n_upper("Ρυθμίσεις ώρας", buf, sizeof(buf));
	CHECK(!strcmp(buf, "ΡΥΘΜΙΣΕΙΣ ΩΡΑΣ"), "Greek uppercase drops the accents: %s", buf);
	i18n_upper("настройки ґ ї", buf, sizeof(buf));
	CHECK(!strcmp(buf, "НАСТРОЙКИ Ґ Ї"), "Cyrillic uppercase: %s", buf);
	i18n_upper("zażółć", buf, sizeof(buf));
	CHECK(!strcmp(buf, "ZAŻÓŁĆ"), "Polish uppercase: %s", buf);
	i18n_upper("日本語", buf, sizeof(buf));
	CHECK(!strcmp(buf, "日本語"), "CJK unchanged");
	if (i18n_set_language("tr") == 0) {
		i18n_upper("ayarlar istasyon ılık", buf, sizeof(buf));
		CHECK(!strcmp(buf, "AYARLAR İSTASYON ILIK"), "Turkish dotted and dotless i: %s", buf);
	}
	i18n_set_language("en");
}

/* Lit pixels and bounding box of one character drawn alone. */
static int draw_count(struct font *f, const char *s, int *bw, int *bh)
{
	enum { W = 96, H = 96 };
	static uint32_t px[W * H];
	struct gfx_surface surf;
	int n = 0, x0 = W, y0 = H, x1 = -1, y1 = -1;

	memset(px, 0, sizeof(px));
	gfx_surface_init(&surf, px, W, H, W);
	font_draw(&surf, f, 8, 64, s, -1, 0xffffffffu);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if ((px[y * W + x] & 0xff) > 64) {
				n++;
				x0 = x < x0 ? x : x0;
				x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0;
				y1 = y > y1 ? y : y1;
			}
	*bw = x1 >= x0 ? x1 - x0 + 1 : 0;
	*bh = y1 >= y0 ? y1 - y0 + 1 : 0;
	return n;
}

static void test_glyphs(const char *fonts)
{
	/* Japanese (kana + kanji), Chinese, Korean, Greek, Cyrillic */
	static const char *const chars[] = {
		"日", "本", "語", "設", "定", "ゲ", "ー", "ム", "を", "简", "体", "中", "文", "设", "置",
		"한", "국", "어", "설", "정", "Ω", "Ж", "ї",
	};
	struct font *f;
	int qn, qw, qh;

	i18n_set_language("ja");
	font_setup_dir(fonts);
	CHECK(font_fallback_count() == 3, "three CJK fallback fonts in %s: %d", fonts, font_fallback_count());
	/* the theme font of rsos-dark: no CJK of its own */
	{
		char p[1024];

		snprintf(p, sizeof(p), "%s/RobotoCondensed-Regular.ttf", fonts);
		f = font_get(p, 32);
	}
	CHECK(f != NULL, "Roboto Condensed loads");
	if (!f)
		return;
	qn = draw_count(f, "?", &qw, &qh);
	for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
		int w, h, n = draw_count(f, chars[i], &w, &h);

		CHECK(n > 20 && w > 0 && h > 0 && !(w == qw && h == qh && n == qn),
		      "%s: a real glyph (%d px, %dx%d box; '?' is %d px, %dx%d)", chars[i], n, w, h, qn, qw, qh);
	}
	CHECK(font_text_width(f, "日本語", -1) >= 3 * 26, "CJK advance ~1 em: %d", font_text_width(f, "日本語", -1));
	/* wrapping without spaces */
	{
		struct text_line lines[8];
		const char *t = "言語を選んでください。設定はあとで変更できます。";
		int n = font_wrap(f, t, 200, lines, 8);
		bool ok = n >= 3;

		for (int i = 0; i < n; i++) {
			ok &= lines[i].width <= 200;
			/* no line starts with the closing punctuation */
			ok &= strncmp(t + lines[i].start, "。", 3) != 0;
		}
		CHECK(ok, "CJK text wraps on %d lines within 200 px", n);
	}
	font_cache_clear();
	i18n_set_language("en");
}

int main(int argc, char **argv)
{
	char cat[1100];

	if (argc != 4) {
		fprintf(stderr, "usage: test_i18n CHECK_DIR LOCALE_DIR FONTS_DIR\n");
		return 2;
	}
	snprintf(cat, sizeof(cat), "%s/cat", argv[1]);
	test_catalog(cat);
	test_plural_eval();
	test_real_catalogs(argv[2]);
	test_upper();
	test_glyphs(argv[3]);
	printf("test_i18n: %d checks, %d failed\n", g_ok + g_fail, g_fail);
	return g_fail ? 1 : 0;
}
