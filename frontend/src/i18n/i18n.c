/*
 * i18n.c - see i18n.h and i18n_cat.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "i18n.h"
#include "i18n_cat.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ languages */
/* RETRO_LANGUAGE_* values (libretro.h), kept here so this file needs no
 * libretro header. Danish has none: English. Alphabetical by native name,
 * Latin scripts first (the order of the language picker). */
static const struct i18n_lang g_langs[] = {
	{ "id",    "Bahasa Indonesia",     "Indonesian",            24 },
	{ "cs",    "\xc4\x8c" "e\xc5\xa1tina", "Czech",             27 },
	{ "da",    "Dansk",                "Danish",                 0 },
	{ "de",    "Deutsch",              "German",                 4 },
	{ "en",    "English",              "English",                0 },
	{ "es",    "Espa\xc3\xb1ol",       "Spanish",                3 },
	{ "fr",    "Fran\xc3\xa7" "ais",   "French",                 2 },
	{ "it",    "Italiano",             "Italian",                5 },
	{ "nl",    "Nederlands",           "Dutch",                  6 },
	{ "nb",    "Norsk bokm\xc3\xa5l",  "Norwegian Bokmal",      34 },
	{ "pl",    "Polski",               "Polish",                14 },
	{ "pt_BR", "Portugu\xc3\xaas (Brasil)",   "Portuguese (Brazil)",   7 },
	{ "pt_PT", "Portugu\xc3\xaas (Portugal)", "Portuguese (Portugal)", 8 },
	{ "fi",    "Suomi",                "Finnish",               23 },
	{ "sv",    "Svenska",              "Swedish",               25 },
	{ "tr",    "T\xc3\xbcrk\xc3\xa7" "e", "Turkish",            18 },
	{ "el",    "\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac", "Greek", 17 },
	{ "ru",    "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9", "Russian", 9 },
	{ "uk",    "\xd0\xa3\xd0\xba\xd1\x80\xd0\xb0\xd1\x97\xd0\xbd\xd1\x81\xd1\x8c\xd0\xba\xd0\xb0",
		   "Ukrainian", 26 },
	{ "ja",    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "Japanese", 1 },
	{ "zh_CN", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87", "Chinese (Simplified)", 12 },
	{ "ko",    "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4", "Korean", 10 },
};

#define NLANGS ((int)(sizeof(g_langs) / sizeof(g_langs[0])))
#define LANG_EN (&g_langs[4])
_Static_assert(sizeof(g_langs) / sizeof(g_langs[0]) > 4, "g_langs[4] is English");

int i18n_lang_count(void)
{
	return NLANGS;
}

const struct i18n_lang *i18n_lang_at(int i)
{
	return i >= 0 && i < NLANGS ? &g_langs[i] : NULL;
}

const struct i18n_lang *i18n_lang_find(const char *code)
{
	char c[16];
	size_t i;

	if (!code || !*code)
		return NULL;
	for (i = 0; code[i] && i + 1 < sizeof(c); i++)
		c[i] = code[i] == '-' ? '_' : code[i];
	c[i] = 0;
	for (int k = 0; k < NLANGS; k++)
		if (!strcasecmp(g_langs[k].code, c))
			return &g_langs[k];
	/* "fr_FR", "de_AT"...: the language alone */
	if ((i = strcspn(c, "_.@")) && c[i]) {
		c[i] = 0;
		for (int k = 0; k < NLANGS; k++)
			if (!strcasecmp(g_langs[k].code, c))
				return &g_langs[k];
	}
	return NULL;
}

/* ------------------------------------------------------------- catalogs */
struct cat {
	const struct i18n_lang *lang;
	char path[640];                    /* reused only from the same file */
	const unsigned char *map;
	size_t len;
	const struct i18n_cat_header *h;
	const uint32_t *buckets;
	const struct i18n_cat_entry *e;
	const char *plural;
	struct cat *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cat *g_cats;              /* every catalog loaded (never unmapped) */
static struct cat *g_cur;               /* atomic: NULL = English */
static const struct i18n_lang *g_cur_lang;
static unsigned g_gen;
static char g_dir[512] = "/usr/share/rsos/locale";

void i18n_set_dir(const char *dir)
{
	pthread_mutex_lock(&g_lock);
	snprintf(g_dir, sizeof(g_dir), "%s", dir && *dir ? dir : "/usr/share/rsos/locale");
	pthread_mutex_unlock(&g_lock);
}

const char *i18n_dir(void)
{
	return g_dir;
}

static bool in_file(const struct cat *c, uint32_t off, uint64_t len)
{
	return (uint64_t)off + len <= c->len;
}

/* Every offset and string of the file, once, so lookups need no checks. */
static bool cat_valid(struct cat *c)
{
	const struct i18n_cat_header *h = (const void *)c->map;

	if (c->len < sizeof(*h) || memcmp(h->magic, I18N_CAT_MAGIC, 8) || h->endian != I18N_CAT_ENDIAN ||
	    h->size != c->len || !h->nbuckets || (h->nbuckets & (h->nbuckets - 1)) ||
	    h->nplurals < 1 || h->nplurals > I18N_CAT_MAX_FORMS || h->buckets % 4 || h->entries % 4 ||
	    !in_file(c, h->buckets, (uint64_t)h->nbuckets * 4) ||
	    !in_file(c, h->entries, (uint64_t)h->count * sizeof(struct i18n_cat_entry)))
		return false;
	c->h = h;
	c->buckets = (const uint32_t *)(c->map + h->buckets);
	c->e = (const struct i18n_cat_entry *)(c->map + h->entries);
	for (uint32_t i = 0; i < h->nbuckets; i++)
		if (c->buckets[i] > h->count)
			return false;
	for (uint32_t i = 0; i < h->count; i++) {
		const struct i18n_cat_entry *e = &c->e[i];
		uint32_t forms = 0;

		if (e->next > h->count || e->nforms < 1 || e->nforms > I18N_CAT_MAX_FORMS ||
		    !in_file(c, e->key, (uint64_t)e->key_len + 1) || c->map[e->key + e->key_len] ||
		    !e->val_len || !in_file(c, e->val, e->val_len) || c->map[e->val + e->val_len - 1])
			return false;
		for (uint32_t k = 0; k < e->val_len; k++)
			forms += !c->map[e->val + k];
		if (forms != e->nforms)
			return false;
	}
	if (h->plural) {
		if (!in_file(c, h->plural, 1) || !memchr(c->map + h->plural, 0, c->len - h->plural))
			return false;
		c->plural = (const char *)c->map + h->plural;
		if (i18n_plural_eval(c->plural, 1) < 0)
			return false;
	}
	return true;
}

static struct cat *cat_load(const struct i18n_lang *lang, char *err, size_t errn)
{
	char path[640];
	struct stat st;
	struct cat *c;
	void *m;
	int fd;

	snprintf(path, sizeof(path), "%s/%s.cat", g_dir, lang->code);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errn, "%s: %s", path, strerror(errno));
		return NULL;
	}
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(struct i18n_cat_header) ||
	    st.st_size > 64 * 1024 * 1024) {
		close(fd);
		snprintf(err, errn, "%s: bad size", path);
		return NULL;
	}
	m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		snprintf(err, errn, "%s: mmap: %s", path, strerror(errno));
		return NULL;
	}
	c = calloc(1, sizeof(*c));
	if (!c) {
		munmap(m, (size_t)st.st_size);
		snprintf(err, errn, "out of memory");
		return NULL;
	}
	c->lang = lang;
	snprintf(c->path, sizeof(c->path), "%s", path);
	c->map = m;
	c->len = (size_t)st.st_size;
	if (!cat_valid(c)) {
		munmap(m, (size_t)st.st_size);
		free(c);
		snprintf(err, errn, "%s: not a valid catalog (rebuild it with rsos-i18n)", path);
		return NULL;
	}
	return c;
}

int i18n_set_language(const char *code)
{
	const struct i18n_lang *lang = code && *code ? i18n_lang_find(code) : LANG_EN;
	struct cat *c = NULL;
	char err[768] = "";
	int ret = 0;

	pthread_mutex_lock(&g_lock);
	if (!lang) {
		snprintf(err, sizeof(err), "unknown language \"%s\"", code);
		lang = LANG_EN;
		ret = -1;
	}
	if (strcmp(lang->code, "en")) {
		char path[640];

		snprintf(path, sizeof(path), "%s/%s.cat", g_dir, lang->code);
		for (c = g_cats; c && (c->lang != lang || strcmp(c->path, path)); c = c->next)
			;
		if (!c && (c = cat_load(lang, err, sizeof(err)))) {
			c->next = g_cats;
			g_cats = c;
		}
		if (!c) {
			lang = LANG_EN;
			ret = -1;
		}
	}
	if (lang != g_cur_lang && (g_cur_lang || strcmp(lang->code, "en")))
		g_gen++;
	g_cur_lang = lang;
	__atomic_store_n(&g_cur, c, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&g_lock);
	if (err[0])
		fprintf(stderr, "i18n: %s: using English\n", err);
	return ret;
}

const struct i18n_lang *i18n_current(void)
{
	struct cat *c = __atomic_load_n(&g_cur, __ATOMIC_ACQUIRE);

	return c ? c->lang : LANG_EN;
}

const char *i18n_language(void)
{
	return i18n_current()->code;
}

unsigned i18n_retro_language(void)
{
	return i18n_current()->retro;
}

unsigned i18n_generation(void)
{
	return __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
}

int i18n_catalog_entries(void)
{
	struct cat *c = __atomic_load_n(&g_cur, __ATOMIC_ACQUIRE);

	return c ? (int)c->h->count : 0;
}

static const struct i18n_cat_entry *lookup(const struct cat *c, const char *ctx, const char *msgid)
{
	size_t cl = ctx ? strlen(ctx) : 0, ml = strlen(msgid);
	uint32_t h = I18N_HASH_INIT;
	uint32_t i;

	if (ctx) {
		h = i18n_hash_step(h, ctx, (uint32_t)cl);
		h = i18n_hash_step(h, "\004", 1);
	}
	h = i18n_hash_step(h, msgid, (uint32_t)ml);
	for (i = c->buckets[h & (c->h->nbuckets - 1)]; i; i = c->e[i - 1].next) {
		const struct i18n_cat_entry *e = &c->e[i - 1];
		const char *k = (const char *)c->map + e->key;

		if (e->hash != h || e->key_len != (ctx ? cl + 1 : 0) + ml)
			continue;
		if (ctx && (memcmp(k, ctx, cl) || k[cl] != '\004'))
			continue;
		if (!memcmp(k + (ctx ? cl + 1 : 0), msgid, ml))
			return e;
	}
	return NULL;
}

static const char *translate(const char *ctx, const char *msgid, const char *plural, unsigned long n)
{
	struct cat *c = __atomic_load_n(&g_cur, __ATOMIC_ACQUIRE);
	const struct i18n_cat_entry *e;
	const char *v;
	long form = 0;

	if (!msgid)
		return "";
	if (c && (e = lookup(c, ctx, msgid))) {
		v = (const char *)c->map + e->val;
		if (!plural)
			return v;   /* (a plural entry used as a singular: form 0) */
		/* a plural entry has the catalog's number of forms (1 in
		 * Japanese, Chinese, Korean, Indonesian) */
		if (e->nforms == c->h->nplurals) {
			form = c->plural ? i18n_plural_eval(c->plural, n) : n != 1;
			if (form < 0 || form >= (long)e->nforms)
				form = n != 1 && e->nforms > 1;
			while (form-- > 0)
				v += strlen(v) + 1;
			return v;
		}
		/* a singular translation for a plural call: English rule */
		if (n == 1)
			return v;
	}
	return plural && n != 1 ? plural : msgid;
}

const char *i18n_gettext(const char *msgid)
{
	return translate(NULL, msgid, NULL, 1);
}

const char *i18n_pgettext(const char *ctx, const char *msgid)
{
	return translate(ctx, msgid, NULL, 1);
}

const char *i18n_ngettext(const char *msgid, const char *plural, unsigned long n)
{
	return translate(NULL, msgid, plural, n);
}

const char *i18n_npgettext(const char *ctx, const char *msgid, const char *plural, unsigned long n)
{
	return translate(ctx, msgid, plural, n);
}

/* --------------------------------------------------- plural expressions */
/* The C subset of Plural-Forms: n, integers, ! - * / % + - < <= > >= ==
 * != && || ?: and parentheses. Recursive descent over the string. */
struct pe {
	const char *s;
	unsigned long n;
	int depth;
	bool err;
};

static unsigned long pe_cond(struct pe *p);

static void pe_ws(struct pe *p)
{
	while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')
		p->s++;
}

static bool pe_eat(struct pe *p, const char *tok)
{
	size_t l = strlen(tok);

	pe_ws(p);
	if (strncmp(p->s, tok, l))
		return false;
	/* "<" must not eat "<=", "!" not "!=", "|"/"&" are only doubled */
	if (l == 1 && (tok[0] == '<' || tok[0] == '>' || tok[0] == '!' || tok[0] == '=') && p->s[1] == '=')
		return false;
	p->s += l;
	return true;
}

static unsigned long pe_primary(struct pe *p)
{
	unsigned long v = 0;

	pe_ws(p);
	if (++p->depth > 64) {
		p->err = true;
		return 0;
	}
	if (*p->s == 'n') {
		p->s++;
		v = p->n;
	} else if (*p->s >= '0' && *p->s <= '9') {
		while (*p->s >= '0' && *p->s <= '9')
			v = v * 10 + (unsigned long)(*p->s++ - '0');
	} else if (pe_eat(p, "(")) {
		v = pe_cond(p);
		if (!pe_eat(p, ")"))
			p->err = true;
	} else if (pe_eat(p, "!")) {
		v = !pe_primary(p);
	} else if (pe_eat(p, "-")) {
		v = -pe_primary(p);
	} else {
		p->err = true;
	}
	p->depth--;
	return v;
}

static unsigned long pe_mul(struct pe *p)
{
	unsigned long v = pe_primary(p);

	for (;;) {
		if (pe_eat(p, "*")) {
			v *= pe_primary(p);
		} else if (pe_eat(p, "/") || pe_eat(p, "%")) {
			char op = p->s[-1];
			unsigned long d = pe_primary(p);

			if (!d) {
				p->err = true;
				return 0;
			}
			v = op == '/' ? v / d : v % d;
		} else {
			return v;
		}
	}
}

static unsigned long pe_add(struct pe *p)
{
	unsigned long v = pe_mul(p);

	for (;;) {
		if (pe_eat(p, "+"))
			v += pe_mul(p);
		else if (pe_eat(p, "-"))
			v -= pe_mul(p);
		else
			return v;
	}
}

static unsigned long pe_rel(struct pe *p)
{
	unsigned long v = pe_add(p);

	for (;;) {
		if (pe_eat(p, "<="))
			v = v <= pe_add(p);
		else if (pe_eat(p, ">="))
			v = v >= pe_add(p);
		else if (pe_eat(p, "<"))
			v = v < pe_add(p);
		else if (pe_eat(p, ">"))
			v = v > pe_add(p);
		else
			return v;
	}
}

static unsigned long pe_eq(struct pe *p)
{
	unsigned long v = pe_rel(p);

	for (;;) {
		if (pe_eat(p, "=="))
			v = v == pe_rel(p);
		else if (pe_eat(p, "!="))
			v = v != pe_rel(p);
		else
			return v;
	}
}

static unsigned long pe_and(struct pe *p)
{
	unsigned long v = pe_eq(p);

	while (pe_eat(p, "&&")) {
		unsigned long r = pe_eq(p);

		v = v && r;
	}
	return v;
}

static unsigned long pe_or(struct pe *p)
{
	unsigned long v = pe_and(p);

	while (pe_eat(p, "||")) {
		unsigned long r = pe_and(p);

		v = v || r;
	}
	return v;
}

static unsigned long pe_cond(struct pe *p)
{
	unsigned long c = pe_or(p), a, b;

	if (!pe_eat(p, "?"))
		return c;
	if (++p->depth > 64) {
		p->err = true;
		return 0;
	}
	a = pe_cond(p);
	if (!pe_eat(p, ":")) {
		p->err = true;
		return 0;
	}
	b = pe_cond(p);
	p->depth--;
	return c ? a : b;
}

long i18n_plural_eval(const char *expr, unsigned long n)
{
	struct pe p = { expr, n, 0, false };
	unsigned long v;

	if (!expr)
		return n != 1;
	v = pe_cond(&p);
	pe_ws(&p);
	if (p.err || (*p.s && *p.s != ';') || v > 1000)
		return -1;
	return (long)v;
}

/* ------------------------------------------------------- locale formats */
/* The number separators, from the catalog (English "." and ","). */
static void separators(const char **dec, const char **thou)
{
	/* TRANSLATORS: the decimal separator of numbers ("1.5 GB"): "," in French. */
	*dec = C_("decimal separator", ".");
	/* TRANSLATORS: the thousands separator ("12,345 games"): a (narrow) no-break
	 * space in French, "." in German. Empty: none. */
	*thou = C_("thousands separator", ",");
}

void i18n_format_number(double v, int decimals, char *out, size_t n)
{
	const char *dec, *thou;
	char raw[64], *p, *dot;
	size_t o = 0, ilen;
	bool neg;

	if (!n)
		return;
	separators(&dec, &thou);
	decimals = decimals < 0 ? 0 : decimals > 6 ? 6 : decimals;
	snprintf(raw, sizeof(raw), "%.*f", decimals, v);
	p = raw;
	neg = *p == '-';
	if (neg)
		p++;
	dot = strchr(p, '.');
	ilen = dot ? (size_t)(dot - p) : strlen(p);
#define PUT(s, l) do { size_t _l = (l); if (o + _l < n) { memcpy(out + o, (s), _l); o += _l; } } while (0)
	if (neg)
		PUT("-", 1);
	for (size_t i = 0; i < ilen; i++) {
		PUT(p + i, 1);
		if (ilen - i - 1 > 0 && (ilen - i - 1) % 3 == 0 && ilen >= 5)
			PUT(thou, strlen(thou));   /* 1234 stays as it is, 12,345 */
	}
	if (dot) {
		PUT(dec, strlen(dec));
		PUT(dot + 1, strlen(dot + 1));
	}
#undef PUT
	out[o] = 0;
}

void i18n_format_size(uint64_t b, char *out, size_t n)
{
	char num[48];
	const char *unit;

	if (b >= (1ull << 30)) {
		i18n_format_number((double)b / (double)(1ull << 30), 1, num, sizeof(num));
		/* TRANSLATORS: gigabytes ("Go" in French) */
		unit = C_("unit", "GB");
	} else if (b >= (1ull << 20)) {
		i18n_format_number((double)b / (double)(1ull << 20), 0, num, sizeof(num));
		/* TRANSLATORS: megabytes ("Mo" in French) */
		unit = C_("unit", "MB");
	} else {
		i18n_format_number(b < 1024 && b ? 1.0 : (double)b / 1024.0, 0, num, sizeof(num));
		/* TRANSLATORS: kilobytes ("Ko" in French) */
		unit = C_("unit", "KB");
	}
	/* TRANSLATORS: a size: number, then unit ("1,5 Go"). */
	snprintf(out, n, C_("unit", "%s %s"), num, unit);
}

static void format_time(int64_t t, const char *fmt, char *out, size_t n)
{
	time_t tt = (time_t)t;
	struct tm tm;

	if (!n)
		return;
	if (!localtime_r(&tt, &tm) || !strftime(out, n, fmt, &tm))
		out[0] = 0;
}

void i18n_format_date(int64_t t, char *out, size_t n)
{
	/* TRANSLATORS: short date (strftime): "%d/%m/%Y" in French. */
	format_time(t, C_("strftime", "%Y-%m-%d"), out, n);
}

void i18n_format_datetime(int64_t t, char *out, size_t n)
{
	/* TRANSLATORS: date and time (strftime): "%d/%m/%Y %H:%M" in French. */
	format_time(t, C_("strftime", "%Y-%m-%d %H:%M"), out, n);
}

/* ------------------------------------------------------------ uppercase */
static unsigned utf8_get(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	unsigned c = *s;
	int n;

	if (c < 0x80) {
		*ps += 1;
		return c;
	}
	n = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
	if (!n) {
		*ps += 1;
		return c;   /* stray byte: Latin-1 */
	}
	c &= 0x3f >> n;
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*ps += 1;
			return s[0];
		}
		c = (c << 6) | (s[i] & 0x3f);
	}
	*ps += n + 1;
	return c;
}

static unsigned upper_cp(unsigned c, bool turkish)
{
	if (c >= 'a' && c <= 'z')
		return turkish && c == 'i' ? 0x130 : c - 32;
	if (c < 0x80)
		return c;
	if (c == 0x131)
		return 'I';
	if ((c >= 0xe0 && c <= 0xfe && c != 0xf7))
		return c - 0x20;
	if (c == 0xff)
		return 0x178;
	if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177) || (c >= 0x218 && c <= 0x21b) ||
	    (c >= 0x460 && c <= 0x481) || (c >= 0x48a && c <= 0x4bf) || (c >= 0x4d0 && c <= 0x4ff))
		return c & ~1u;
	if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
		return (c & 1) ? c : c - 1;
	if (c == 0x17f)
		return 'S';
	/* Greek: capitals carry no accent */
	switch (c) {
	case 0x3ac: case 0x386: return 0x391;
	case 0x3ad: case 0x388: return 0x395;
	case 0x3ae: case 0x389: return 0x397;
	case 0x3af: case 0x38a: return 0x399;
	case 0x3cc: case 0x38c: return 0x39f;
	case 0x3cd: case 0x38e: return 0x3a5;
	case 0x3ce: case 0x38f: return 0x3a9;
	case 0x390: case 0x3ca: return 0x3aa;
	case 0x3b0: case 0x3cb: return 0x3ab;
	case 0x3c2: return 0x3a3;
	default: break;
	}
	if (c >= 0x3b1 && c <= 0x3c9)
		return c - 0x20;
	if (c >= 0x430 && c <= 0x44f)
		return c - 0x20;
	if (c >= 0x450 && c <= 0x45f)
		return c - 0x50;
	return c;
}

void i18n_upper(const char *in, char *buf, size_t n)
{
	bool tr = !strcmp(i18n_language(), "tr");
	size_t o = 0;

	if (!n)
		return;
	while (*in) {
		const char *p = in;
		unsigned c = utf8_get(&p);
		unsigned u = upper_cp(c, tr);
		unsigned char e[4];
		size_t l;

		if (u == c && (unsigned char)*in >= 0x80) {
			/* unchanged: the original bytes (also keeps stray Latin-1 bytes) */
			l = (size_t)(p - in);
			if (o + l >= n)
				break;
			memcpy(buf + o, in, l);
		} else {
			if (u < 0x80) {
				e[0] = (unsigned char)u;
				l = 1;
			} else if (u < 0x800) {
				e[0] = (unsigned char)(0xc0 | (u >> 6));
				e[1] = (unsigned char)(0x80 | (u & 0x3f));
				l = 2;
			} else if (u < 0x10000) {
				e[0] = (unsigned char)(0xe0 | (u >> 12));
				e[1] = (unsigned char)(0x80 | ((u >> 6) & 0x3f));
				e[2] = (unsigned char)(0x80 | (u & 0x3f));
				l = 3;
			} else {
				e[0] = (unsigned char)(0xf0 | (u >> 18));
				e[1] = (unsigned char)(0x80 | ((u >> 12) & 0x3f));
				e[2] = (unsigned char)(0x80 | ((u >> 6) & 0x3f));
				e[3] = (unsigned char)(0x80 | (u & 0x3f));
				l = 4;
			}
			if (o + l >= n)
				break;
			memcpy(buf + o, e, l);
		}
		o += l;
		in = p;
	}
	buf[o] = 0;
}
