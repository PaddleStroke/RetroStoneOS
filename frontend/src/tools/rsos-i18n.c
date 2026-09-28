/*
 * rsos-i18n - the build-time side of src/i18n (docs/translating.md). Runs on
 * the build machine (CC_FOR_BUILD), needs nothing but libc and the vendored
 * stb_truetype.
 *
 *   rsos-i18n extract -o rsos.pot FILE.c...
 *       every string marked with _() N_() C_() NC_() _n() C_n() (literal
 *       arguments, adjacent literals joined), with its locations, the
 *       "TRANSLATORS:" comment before it and the c-format flag
 *   rsos-i18n compile -o fr.cat fr.po
 *       the binary catalog (i18n_cat.h): translated, non-fuzzy entries whose
 *       printf conversions match the English (others are dropped, with a
 *       warning: the English text is shown for them)
 *   rsos-i18n check [--strict] --pot rsos.pot LANG.po...
 *       per language: translated / fuzzy / missing counts, format or plural
 *       errors, entries missing from the .po or no longer in the .pot.
 *       Exit 1 on an error (a bad format, a bad Plural-Forms, a .po that
 *       does not parse); --strict also on missing entries
 *   rsos-i18n check-pot FRESH.pot COMMITTED.pot
 *       exit 1 if a string of FRESH (just extracted) is not in COMMITTED
 *   rsos-i18n chars LANG.po...
 *       every code point used by the translations, UTF-8, one line (font
 *       subsetting)
 *   rsos-i18n fontcheck FONT... -- LANG.po...
 *       every code point of the translations must be in one of the fonts
 *       (the renderer's fallback chain); lists the missing ones, exit 1
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../i18n/i18n_cat.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

static int g_errors, g_warnings;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("rsos-i18n: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(2);
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

static char *xstrdup(const char *s)
{
	char *d = strdup(s ? s : "");

	if (!d)
		die("out of memory");
	return d;
}

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf = NULL;
	size_t n = 0, cap = 0, r;

	if (!f)
		die("%s: %s", path, strerror(errno));
	do {
		if (n + 65536 + 1 > cap) {
			cap = (n + 65536 + 1) * 2;
			buf = xrealloc(buf, cap);
		}
		r = fread(buf + n, 1, 65536, f);
		n += r;
	} while (r > 0);
	if (ferror(f))
		die("%s: read error", path);
	fclose(f);
	buf[n] = 0;
	if (len)
		*len = n;
	return buf;
}

/* ---------------------------------------------------------- growable str */
struct sb {
	char *s;
	size_t n, cap;
};

static void sb_addn(struct sb *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->s = xrealloc(b->s, b->cap);
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}

static void sb_add(struct sb *b, const char *s)
{
	sb_addn(b, s, strlen(s));
}

static void sb_addc(struct sb *b, char c)
{
	sb_addn(b, &c, 1);
}

static char *sb_take(struct sb *b)
{
	char *s = b->s ? b->s : xstrdup("");

	memset(b, 0, sizeof(*b));
	return s;
}

/* ---------------------------------------------------------------- entries */
struct entry {
	char *ctx;              /* NULL = none */
	char *id;
	char *plural;           /* NULL = not a plural entry */
	char *str[I18N_CAT_MAX_FORMS];
	int nstr;
	bool fuzzy, c_format, no_c_format;
	char *comments;         /* "#." lines (extracted), joined with \n */
	char *refs;             /* "#:" locations, space separated */
	int line;               /* in the .po */
};

struct entries {
	struct entry *v;
	int n, cap;
};

static struct entry *entries_add(struct entries *es)
{
	if (es->n == es->cap) {
		es->cap = es->cap ? es->cap * 2 : 256;
		es->v = xrealloc(es->v, sizeof(*es->v) * (size_t)es->cap);
	}
	memset(&es->v[es->n], 0, sizeof(es->v[0]));
	return &es->v[es->n++];
}

static bool same_key(const struct entry *e, const char *ctx, const char *id)
{
	if ((e->ctx == NULL) != (ctx == NULL))
		return false;
	return (!ctx || !strcmp(e->ctx, ctx)) && !strcmp(e->id, id);
}

static struct entry *entries_find(struct entries *es, const char *ctx, const char *id)
{
	for (int i = 0; i < es->n; i++)
		if (same_key(&es->v[i], ctx, id))
			return &es->v[i];
	return NULL;
}

/* ------------------------------------------------------------ printf check */
/* One conversion: its argument number (1-based) and a type class. */
struct conv {
	int arg;
	char type[8];   /* length modifier + normalized conversion: "d", "ld", "s", "f"... */
};

/* Parses the printf conversions of s. Returns the count, -1 if malformed
 * (a '%' that is not a valid conversion, or positional and sequential
 * arguments mixed). *positional tells which kind. */
static int parse_convs(const char *s, struct conv *out, int max, bool *positional)
{
	int n = 0, seq = 0;
	int kind = 0;   /* 0 none yet, 1 sequential, 2 positional */

	for (const char *p = s; *p; p++) {
		int arg = 0;
		char len[4] = "";
		size_t li = 0;
		char c;

		if (*p != '%')
			continue;
		p++;
		if (*p == '%')
			continue;
		/* argument number */
		{
			const char *q = p;
			int v = 0;

			while (*q >= '0' && *q <= '9')
				v = v * 10 + (*q++ - '0');
			if (q > p && *q == '$') {
				arg = v;
				p = q + 1;
			}
		}
		while (*p && strchr("-+ #0'I", *p))
			p++;
		if (*p == '*')
			return -1;   /* no '*' in our messages */
		while (*p >= '0' && *p <= '9')
			p++;
		if (*p == '.') {
			p++;
			if (*p == '*')
				return -1;
			while (*p >= '0' && *p <= '9')
				p++;
		}
		while (*p && strchr("hlLqjzt", *p) && li < 3)
			len[li++] = *p++;
		len[li] = 0;
		c = *p;
		if (!c || !strchr("diouxXeEfFgGaAcsp", c))
			return -1;
		if (c == 'i')
			c = 'd';
		if (strchr("oxX", c))
			c = 'u';
		if (strchr("eEFgGaA", c))
			c = 'f';
		if (arg) {
			if (kind == 1)
				return -1;
			kind = 2;
		} else {
			if (kind == 2)
				return -1;
			kind = 1;
			arg = ++seq;
		}
		if (n >= max)
			return -1;
		out[n].arg = arg;
		snprintf(out[n].type, sizeof(out[n].type), "%s%c", len, c);
		n++;
	}
	if (positional)
		*positional = kind == 2;
	return n;
}

/* The type of argument `arg` in convs, "" if unused, NULL if used twice
 * with different types. */
static const char *arg_type(const struct conv *c, int n, int arg)
{
	const char *t = "";

	for (int i = 0; i < n; i++)
		if (c[i].arg == arg) {
			if (*t && strcmp(t, c[i].type))
				return NULL;
			t = c[i].type;
		}
	return t;
}

/*
 * Is `tr` safe as a translation of the English format `en`? Every argument
 * the translation uses must have the English type; it must use them all,
 * except that a plural form may drop trailing ones (the "%d" of "one
 * game"): extra printf arguments are ignored, missing or mistyped ones are
 * not. Writes the reason into why.
 */
static bool format_ok(const char *en, const char *tr, bool plural_form, char *why, size_t whyn)
{
	struct conv ce[32], ct[32];
	int ne = parse_convs(en, ce, 32, NULL), nt = parse_convs(tr, ct, 32, NULL);
	int maxe = 0, maxt = 0;

	if (ne < 0) {
		snprintf(why, whyn, "the English format is not a plain printf format");
		return false;
	}
	if (nt < 0) {
		snprintf(why, whyn, "invalid %% conversion (write %%%% for a %% sign)");
		return false;
	}
	for (int i = 0; i < ne; i++)
		maxe = ce[i].arg > maxe ? ce[i].arg : maxe;
	for (int i = 0; i < nt; i++)
		maxt = ct[i].arg > maxt ? ct[i].arg : maxt;
	if (maxt > maxe) {
		snprintf(why, whyn, "uses argument %d, the English has %d", maxt, maxe);
		return false;
	}
	for (int a = 1; a <= maxe; a++) {
		const char *te = arg_type(ce, ne, a), *tt = arg_type(ct, nt, a);

		if (!te || !tt) {
			snprintf(why, whyn, "argument %d used with two types", a);
			return false;
		}
		if (!*tt) {
			/* unused: only trailing arguments of a plural form */
			bool later = false;

			for (int b = a + 1; b <= maxe; b++)
				if (*arg_type(ct, nt, b))
					later = true;
			if (later || !plural_form) {
				snprintf(why, whyn, "argument %d (%%%s) is missing", a, te);
				return false;
			}
			continue;
		}
		if (strcmp(te, tt)) {
			snprintf(why, whyn, "argument %d is %%%s in English, %%%s here", a, te, tt);
			return false;
		}
	}
	return true;
}

static bool has_conversion(const char *s)
{
	struct conv c[32];
	int n = parse_convs(s, c, 32, NULL);

	return n > 0;
}

/* ----------------------------------------------------------------- .po */
static bool po_unquote(const char *p, struct sb *out, const char *file, int line)
{
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '"') {
		fprintf(stderr, "%s:%d: expected a quoted string\n", file, line);
		return false;
	}
	for (p++; *p && *p != '"'; p++) {
		if (*p != '\\') {
			sb_addc(out, *p);
			continue;
		}
		p++;
		switch (*p) {
		case 'n': sb_addc(out, '\n'); break;
		case 't': sb_addc(out, '\t'); break;
		case 'r': sb_addc(out, '\r'); break;
		case 'a': sb_addc(out, '\a'); break;
		case 'b': sb_addc(out, '\b'); break;
		case 'f': sb_addc(out, '\f'); break;
		case 'v': sb_addc(out, '\v'); break;
		case '\\': sb_addc(out, '\\'); break;
		case '"': sb_addc(out, '"'); break;
		case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
			int v = 0;

			for (int k = 0; k < 3 && *p >= '0' && *p <= '7'; k++)
				v = v * 8 + (*p++ - '0');
			p--;
			sb_addc(out, (char)v);
			break;
		}
		default:
			fprintf(stderr, "%s:%d: unknown escape \\%c\n", file, line, *p ? *p : '0');
			return false;
		}
	}
	if (*p != '"') {
		fprintf(stderr, "%s:%d: unterminated string\n", file, line);
		return false;
	}
	return true;
}

struct po {
	struct entries e;
	char *header;       /* the msgstr of msgid "" */
	char plural[256];   /* the Plural-Forms expression, "" = none */
	int nplurals;
	char lang[32];
};

static void header_field(const char *header, const char *name, char *out, size_t n)
{
	const char *p = header;
	size_t l = strlen(name);

	out[0] = 0;
	while (p && *p) {
		if (!strncasecmp(p, name, l) && p[l] == ':') {
			const char *v = p + l + 1, *e;

			while (*v == ' ')
				v++;
			e = strchr(v, '\n');
			if (!e)
				e = v + strlen(v);
			snprintf(out, n, "%.*s", (int)(e - v), v);
			return;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

static bool po_parse(const char *path, struct po *po)
{
	char *buf = read_file(path, NULL), *save = NULL, *ln;
	struct entry cur;
	struct sb *target = NULL, s_ctx = { 0 }, s_id = { 0 }, s_pl = { 0 }, s_str[I18N_CAT_MAX_FORMS] = { { 0 } };
	struct sb s_com = { 0 }, s_ref = { 0 };
	bool has_ctx = false, has_id = false, has_pl = false, obsolete = false, ok = true;
	int nstr = 0, line = 0, start = 0;
	bool fuzzy = false, cfmt = false, nocfmt = false;

	memset(po, 0, sizeof(*po));
	memset(&cur, 0, sizeof(cur));

#define FLUSH() do { \
		if (has_id && !obsolete) { \
			char *id = sb_take(&s_id); \
			if (!id[0] && !has_ctx) { \
				po->header = nstr ? sb_take(&s_str[0]) : xstrdup(""); \
				free(id); \
			} else { \
				struct entry *e = entries_add(&po->e); \
				e->ctx = has_ctx ? sb_take(&s_ctx) : NULL; \
				e->id = id; \
				e->plural = has_pl ? sb_take(&s_pl) : NULL; \
				e->nstr = nstr; \
				for (int _k = 0; _k < nstr; _k++) \
					e->str[_k] = sb_take(&s_str[_k]); \
				e->fuzzy = fuzzy; e->c_format = cfmt; e->no_c_format = nocfmt; \
				e->comments = sb_take(&s_com); e->refs = sb_take(&s_ref); \
				e->line = start; \
			} \
		} \
		free(sb_take(&s_ctx)); free(sb_take(&s_id)); free(sb_take(&s_pl)); \
		for (int _k = 0; _k < I18N_CAT_MAX_FORMS; _k++) free(sb_take(&s_str[_k])); \
		free(sb_take(&s_com)); free(sb_take(&s_ref)); \
		has_ctx = has_id = has_pl = obsolete = fuzzy = cfmt = nocfmt = false; \
		nstr = 0; target = NULL; \
	} while (0)

	for (ln = buf; ln; ln = save) {
		char *nl = strchr(ln, '\n');

		save = nl ? nl + 1 : NULL;
		if (nl)
			*nl = 0;
		line++;
		if (nl && nl > ln && nl[-1] == '\r')
			nl[-1] = 0;
		while (*ln == ' ' || *ln == '\t')
			ln++;
		if (!*ln)
			continue;
		if (ln[0] == '#') {
			if (ln[1] == '~') {
				if (has_id && !obsolete)
					FLUSH();
				obsolete = true;
				has_id = true;   /* dropped at the flush */
				continue;
			}
			if (obsolete || (has_id && (nstr || has_pl)))
				FLUSH();
			if (ln[1] == ',') {
				fuzzy |= strstr(ln, "fuzzy") != NULL;
				nocfmt |= strstr(ln, "no-c-format") != NULL;
				cfmt |= strstr(ln, " c-format") != NULL || strstr(ln, ",c-format") != NULL;
			} else if (ln[1] == '.') {
				if (s_com.n)
					sb_addc(&s_com, '\n');
				sb_add(&s_com, ln[2] == ' ' ? ln + 3 : ln + 2);
			} else if (ln[1] == ':') {
				if (s_ref.n)
					sb_addc(&s_ref, ' ');
				sb_add(&s_ref, ln[2] == ' ' ? ln + 3 : ln + 2);
			}
			continue;
		}
		if (!strncmp(ln, "msgctxt", 7) && (ln[7] == ' ' || ln[7] == '"')) {
			if (has_id)
				FLUSH();
			start = line;
			has_ctx = true;
			target = &s_ctx;
			ok &= po_unquote(ln + 7, target, path, line);
		} else if (!strncmp(ln, "msgid_plural", 12)) {
			has_pl = true;
			target = &s_pl;
			ok &= po_unquote(ln + 12, target, path, line);
		} else if (!strncmp(ln, "msgid", 5) && (ln[5] == ' ' || ln[5] == '"')) {
			if (has_id)
				FLUSH();
			if (!has_ctx)
				start = line;
			has_id = true;
			target = &s_id;
			ok &= po_unquote(ln + 5, target, path, line);
		} else if (!strncmp(ln, "msgstr[", 7)) {
			int k = atoi(ln + 7);
			char *q = strchr(ln, ']');

			if (!q || k < 0 || k >= I18N_CAT_MAX_FORMS || k != nstr) {
				fprintf(stderr, "%s:%d: bad msgstr index\n", path, line);
				ok = false;
				continue;
			}
			nstr = k + 1;
			target = &s_str[k];
			ok &= po_unquote(q + 1, target, path, line);
		} else if (!strncmp(ln, "msgstr", 6) && (ln[6] == ' ' || ln[6] == '"')) {
			nstr = 1;
			target = &s_str[0];
			ok &= po_unquote(ln + 6, target, path, line);
		} else if (ln[0] == '"' && target) {
			ok &= po_unquote(ln, target, path, line);
		} else {
			fprintf(stderr, "%s:%d: cannot parse: %.40s\n", path, line, ln);
			ok = false;
		}
	}
	FLUSH();
#undef FLUSH
	free(buf);
	if (po->header) {
		char pf[512];
		const char *p;

		header_field(po->header, "Language", po->lang, sizeof(po->lang));
		header_field(po->header, "Plural-Forms", pf, sizeof(pf));
		if ((p = strstr(pf, "nplurals=")))
			po->nplurals = atoi(p + 9);
		if ((p = strstr(pf, "plural="))) {
			const char *e = strrchr(p, ';');

			snprintf(po->plural, sizeof(po->plural), "%.*s", (int)(e && e > p ? e - p - 7 : (long)strlen(p + 7)),
				 p + 7);
		}
	}
	return ok;
}

/* Checks the Plural-Forms of a .po: 1..6 forms, an expression that gives
 * 0..nplurals-1 for n = 0..1000. */
static bool plural_ok(const struct po *po, const char *path)
{
	if (po->nplurals < 1 || po->nplurals > I18N_CAT_MAX_FORMS) {
		fprintf(stderr, "%s: Plural-Forms: nplurals=%d is not 1..%d\n", path, po->nplurals,
			I18N_CAT_MAX_FORMS);
		return false;
	}
	if (!po->plural[0]) {
		fprintf(stderr, "%s: Plural-Forms has no plural= expression\n", path);
		return false;
	}
	for (unsigned long n = 0; n <= 1000; n++) {
		long f = i18n_plural_eval(po->plural, n);

		if (f < 0 || f >= po->nplurals) {
			fprintf(stderr, "%s: Plural-Forms: plural=%s gives %ld for n=%lu\n", path, po->plural, f, n);
			return false;
		}
	}
	return true;
}

/* Is this entry usable? Writes why not. Formats are checked when the entry
 * says c-format or the English contains a conversion (and not no-c-format). */
static bool entry_usable(const struct entry *e, const struct po *po, char *why, size_t whyn, bool *error)
{
	bool check;

	*error = false;
	why[0] = 0;
	if (e->fuzzy) {
		snprintf(why, whyn, "fuzzy");
		return false;
	}
	for (int k = 0; k < e->nstr; k++)
		if (!e->str[k][0]) {
			snprintf(why, whyn, "untranslated");
			return false;
		}
	if (!e->nstr) {
		snprintf(why, whyn, "untranslated");
		return false;
	}
	if (e->plural && e->nstr != po->nplurals) {
		snprintf(why, whyn, "%d plural forms, Plural-Forms says %d", e->nstr, po->nplurals);
		*error = true;
		return false;
	}
	if (!e->plural && e->nstr != 1) {
		snprintf(why, whyn, "msgstr[] without msgid_plural");
		*error = true;
		return false;
	}
	check = !e->no_c_format && (e->c_format || has_conversion(e->id) || (e->plural && has_conversion(e->plural)));
	if (check) {
		for (int k = 0; k < e->nstr; k++) {
			/* plural forms follow msgid_plural (same conversions as msgid) */
			const char *en = e->plural ? e->plural : e->id;

			if (!format_ok(en, e->str[k], e->plural != NULL, why, whyn)) {
				*error = true;
				return false;
			}
		}
	} else {
		/* no conversion in English: a '%' in the translation is harmless
		 * only if nothing formats it, which is not known here: refuse a
		 * conversion that the English does not have */
		for (int k = 0; k < e->nstr; k++)
			if (!e->no_c_format && has_conversion(e->str[k])) {
				snprintf(why, whyn, "has a %% conversion, the English has none (write %%%%)");
				*error = true;
				return false;
			}
	}
	return true;
}

static void entry_name(const struct entry *e, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s\"%.60s\"", e->ctx ? "[" : "", e->ctx ? e->ctx : "", e->ctx ? "] " : "", e->id);
	for (char *p = out; *p; p++)
		if (*p == '\n')
			*p = ' ';
}

/* ------------------------------------------------------------- compile */
static void put32(struct sb *b, uint32_t v)
{
	sb_addn(b, (const char *)&v, 4);
}

static int cmd_compile(int argc, char **argv)
{
	const char *out = NULL, *in = NULL;
	struct po po;
	struct entry **ok = NULL;
	uint32_t n = 0, nb = 16, *bk, *next, *hash;
	struct sb strings = { 0 }, file = { 0 };
	struct i18n_cat_header h;
	uint32_t *koff, *voff, *vlen;
	char lang[32];

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else
			in = argv[i];
	}
	if (!out || !in)
		die("usage: rsos-i18n compile -o LANG.cat LANG.po");
	if (!po_parse(in, &po))
		die("%s: parse errors", in);
	if (!plural_ok(&po, in))
		die("%s: invalid Plural-Forms", in);
	ok = xrealloc(NULL, sizeof(*ok) * (size_t)(po.e.n + 1));
	for (int i = 0; i < po.e.n; i++) {
		struct entry *e = &po.e.v[i];
		char why[160], name[128];
		bool err;

		if (entry_usable(e, &po, why, sizeof(why), &err)) {
			ok[n++] = e;
		} else if (err) {
			entry_name(e, name, sizeof(name));
			fprintf(stderr, "%s:%d: warning: %s dropped (English shown): %s\n", in, e->line, name, why);
			g_warnings++;
		}
	}
	while (nb < n * 2)
		nb *= 2;
	bk = calloc(nb, 4);
	next = calloc(n + 1, 4);
	hash = calloc(n + 1, 4);
	koff = calloc(n + 1, 4);
	voff = calloc(n + 1, 4);
	vlen = calloc(n + 1, 4);
	if (!bk || !next || !hash || !koff || !voff || !vlen)
		die("out of memory");
	/* strings first (relative offsets, fixed up below) */
	for (uint32_t i = 0; i < n; i++) {
		struct entry *e = ok[i];
		uint32_t h32 = I18N_HASH_INIT;

		koff[i] = (uint32_t)strings.n;
		if (e->ctx) {
			sb_add(&strings, e->ctx);
			sb_addc(&strings, '\004');
			h32 = i18n_hash_step(h32, e->ctx, (uint32_t)strlen(e->ctx));
			h32 = i18n_hash_step(h32, "\004", 1);
		}
		sb_add(&strings, e->id);
		h32 = i18n_hash_step(h32, e->id, (uint32_t)strlen(e->id));
		sb_addn(&strings, "", 1);
		hash[i] = h32;
		voff[i] = (uint32_t)strings.n;
		for (int k = 0; k < e->nstr; k++) {
			sb_add(&strings, e->str[k]);
			sb_addn(&strings, "", 1);
		}
		vlen[i] = (uint32_t)strings.n - voff[i];
		/* chain: prepend */
		next[i] = bk[h32 & (nb - 1)];
		bk[h32 & (nb - 1)] = i + 1;
	}
	snprintf(lang, sizeof(lang), "%s", po.lang[0] ? po.lang : "");
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, I18N_CAT_MAGIC, 8);
	h.endian = I18N_CAT_ENDIAN;
	h.count = n;
	h.nbuckets = nb;
	h.buckets = sizeof(h);
	h.entries = h.buckets + nb * 4;
	{
		uint32_t sbase = h.entries + n * (uint32_t)sizeof(struct i18n_cat_entry);

		h.plural = sbase + (uint32_t)strings.n;
		h.lang = h.plural + (uint32_t)strlen(po.plural) + 1;
		h.size = h.lang + (uint32_t)strlen(lang) + 1;
		h.nplurals = (uint32_t)po.nplurals;
		sb_addn(&file, (const char *)&h, sizeof(h));
		for (uint32_t i = 0; i < nb; i++)
			put32(&file, bk[i]);
		for (uint32_t i = 0; i < n; i++) {
			struct i18n_cat_entry ce;

			ce.hash = hash[i];
			ce.next = next[i];
			ce.key = sbase + koff[i];
			ce.key_len = (ok[i]->ctx ? (uint32_t)strlen(ok[i]->ctx) + 1 : 0) + (uint32_t)strlen(ok[i]->id);
			ce.val = sbase + voff[i];
			ce.val_len = vlen[i];
			ce.nforms = (uint32_t)ok[i]->nstr;
			sb_addn(&file, (const char *)&ce, sizeof(ce));
		}
		sb_addn(&file, strings.s ? strings.s : "", strings.n);
		sb_addn(&file, po.plural, strlen(po.plural) + 1);
		sb_addn(&file, lang, strlen(lang) + 1);
	}
	{
		FILE *f;
		char tmp[1024];

		snprintf(tmp, sizeof(tmp), "%s.tmp", out);
		f = fopen(tmp, "wb");
		if (!f || fwrite(file.s, 1, file.n, f) != file.n || fclose(f))
			die("%s: %s", tmp, strerror(errno));
		if (rename(tmp, out))
			die("%s: %s", out, strerror(errno));
	}
	fprintf(stderr, "rsos-i18n: %s: %u of %d entries, %zu bytes%s\n", out, n, po.e.n, file.n,
		g_warnings ? " (warnings above)" : "");
	return 0;
}

/* ---------------------------------------------------------------- check */
static int cmd_check(int argc, char **argv)
{
	const char *potpath = NULL;
	bool strict = false;
	struct po pot;
	bool have_pot = false;
	int files = 0;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--pot") && i + 1 < argc) {
			potpath = argv[++i];
			if (!po_parse(potpath, &pot))
				die("%s: parse errors", potpath);
			have_pot = true;
		}
	}
	for (int i = 0; i < argc; i++) {
		struct po po;
		int tr = 0, fz = 0, miss = 0, stale = 0, bad = 0;

		if (!strcmp(argv[i], "--pot")) {
			i++;
			continue;
		}
		if (!strcmp(argv[i], "--strict")) {
			strict = true;
			continue;
		}
		files++;
		if (!po_parse(argv[i], &po)) {
			g_errors++;
			continue;
		}
		if (!po.header || !strstr(po.header, "charset=UTF-8")) {
			fprintf(stderr, "%s: the header must say charset=UTF-8\n", argv[i]);
			g_errors++;
		}
		if (!plural_ok(&po, argv[i]))
			g_errors++;
		for (int k = 0; k < po.e.n; k++) {
			struct entry *e = &po.e.v[k];
			char why[160], name[128];
			bool err;

			if (entry_usable(e, &po, why, sizeof(why), &err)) {
				tr++;
			} else if (err) {
				entry_name(e, name, sizeof(name));
				fprintf(stderr, "%s:%d: error: %s: %s\n", argv[i], e->line, name, why);
				bad++;
				g_errors++;
			} else if (e->fuzzy) {
				fz++;
			}
			if (have_pot && !entries_find(&pot.e, e->ctx, e->id)) {
				entry_name(e, name, sizeof(name));
				fprintf(stderr, "%s:%d: note: %s is no longer in %s (run make po-update)\n", argv[i],
					e->line, name, potpath);
				stale++;
			}
		}
		if (have_pot)
			for (int k = 0; k < pot.e.n; k++) {
				struct entry *pe = &pot.e.v[k];
				struct entry *e = entries_find(&po.e, pe->ctx, pe->id);
				char name[128];

				if (!e) {
					entry_name(pe, name, sizeof(name));
					if (strict)
						fprintf(stderr, "%s: missing %s (run make po-update)\n", argv[i], name);
					miss++;
				} else if ((pe->plural == NULL) != (e->plural == NULL)) {
					entry_name(pe, name, sizeof(name));
					fprintf(stderr, "%s:%d: error: %s: plural in one file, not in the other\n",
						argv[i], e->line, name);
					g_errors++;
				}
			}
		printf("%-12s %4d translated, %3d fuzzy, %3d not in the .po, %3d stale, %d errors\n",
		       argv[i], tr, fz, miss, stale, bad);
		if (strict && miss)
			g_errors++;
	}
	if (!files)
		die("usage: rsos-i18n check [--strict] [--pot rsos.pot] LANG.po...");
	return g_errors ? 1 : 0;
}

static int cmd_check_pot(int argc, char **argv)
{
	struct po fresh, committed;
	int missing = 0;

	if (argc != 2)
		die("usage: rsos-i18n check-pot FRESH.pot COMMITTED.pot");
	if (!po_parse(argv[0], &fresh) || !po_parse(argv[1], &committed))
		die("parse errors");
	for (int i = 0; i < fresh.e.n; i++) {
		struct entry *e = &fresh.e.v[i];

		if (!entries_find(&committed.e, e->ctx, e->id)) {
			char name[128];

			entry_name(e, name, sizeof(name));
			fprintf(stderr, "%s: %s (%s) is not in %s\n", argv[0], name, e->refs, argv[1]);
			missing++;
		}
	}
	if (missing) {
		fprintf(stderr, "rsos-i18n: %d marked string(s) missing from %s: run `make pot` "
			"(and `make po-update`)\n", missing, argv[1]);
		return 1;
	}
	printf("rsos-i18n: %s: all %d marked strings are in %s\n", argv[0], fresh.e.n, argv[1]);
	return 0;
}

/* ---------------------------------------------------------------- chars */
static unsigned utf8_decode(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	unsigned c = *s;
	int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;

	if (n <= 0) {
		*ps += 1;
		return n < 0 ? 0xfffd : c;
	}
	c &= 0x3f >> n;
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*ps += 1;
			return 0xfffd;
		}
		c = (c << 6) | (s[i] & 0x3f);
	}
	*ps += n + 1;
	return c;
}

static void utf8_put(FILE *f, unsigned c)
{
	if (c < 0x80) {
		fputc((int)c, f);
	} else if (c < 0x800) {
		fputc((int)(0xc0 | (c >> 6)), f);
		fputc((int)(0x80 | (c & 0x3f)), f);
	} else if (c < 0x10000) {
		fputc((int)(0xe0 | (c >> 12)), f);
		fputc((int)(0x80 | ((c >> 6) & 0x3f)), f);
		fputc((int)(0x80 | (c & 0x3f)), f);
	} else {
		fputc((int)(0xf0 | (c >> 18)), f);
		fputc((int)(0x80 | ((c >> 12) & 0x3f)), f);
		fputc((int)(0x80 | ((c >> 6) & 0x3f)), f);
		fputc((int)(0x80 | (c & 0x3f)), f);
	}
}

/* Marks every code point of the translations of these .po files. */
static uint8_t *collect_chars(int argc, char **argv)
{
	uint8_t *seen = calloc(0x110000, 1);

	if (!seen)
		die("out of memory");
	for (int i = 0; i < argc; i++) {
		struct po po;

		if (!po_parse(argv[i], &po))
			die("%s: parse errors", argv[i]);
		for (int k = 0; k < po.e.n; k++)
			for (int s = 0; s < po.e.v[k].nstr; s++)
				for (const char *p = po.e.v[k].str[s]; *p;) {
					unsigned c = utf8_decode(&p);

					if (c < 0x110000 && c >= 0x20)
						seen[c] = 1;
				}
	}
	return seen;
}

static int cmd_chars(int argc, char **argv)
{
	uint8_t *seen = collect_chars(argc, argv);

	for (unsigned c = 0x20; c < 0x110000; c++)
		if (seen[c])
			utf8_put(stdout, c);
	putchar('\n');
	free(seen);
	return 0;
}

static int cmd_fontcheck(int argc, char **argv)
{
	stbtt_fontinfo fi[8];
	int nf = 0, sep = -1, missing = 0;
	uint8_t *seen;

	for (int i = 0; i < argc; i++)
		if (!strcmp(argv[i], "--")) {
			sep = i;
			break;
		}
	if (sep < 1 || sep == argc - 1)
		die("usage: rsos-i18n fontcheck FONT... -- LANG.po...");
	for (int i = 0; i < sep && nf < 8; i++) {
		size_t len;
		unsigned char *data = (unsigned char *)read_file(argv[i], &len);
		int off = stbtt_GetFontOffsetForIndex(data, 0);

		if (off < 0 || !stbtt_InitFont(&fi[nf], data, off))
			die("%s: not a font stb_truetype can read", argv[i]);
		nf++;
	}
	seen = collect_chars(argc - sep - 1, argv + sep + 1);
	for (unsigned c = 0x21; c < 0x110000; c++) {
		bool found = false;

		if (!seen[c] || c == 0xa0 || c == 0x202f || (c >= 0x2000 && c <= 0x200f))
			continue;   /* spaces: advance only */
		for (int k = 0; k < nf && !found; k++)
			found = stbtt_FindGlyphIndex(&fi[k], (int)c) != 0;
		if (!found) {
			fprintf(stderr, "rsos-i18n: no font has U+%04X '", c);
			utf8_put(stderr, c);
			fprintf(stderr, "'\n");
			missing++;
		}
	}
	free(seen);
	if (missing) {
		fprintf(stderr, "rsos-i18n: %d code point(s) missing from the fonts\n", missing);
		return 1;
	}
	printf("rsos-i18n: every character of the translations is in the fonts\n");
	return 0;
}

/* -------------------------------------------------------------- extract */
struct src {
	const char *p, *end;
	const char *file;
	int line;
	/* the last TRANSLATORS comment and the line it ended on */
	char *tcomment;
	int tline;
};

static void src_advance(struct src *s, size_t n)
{
	for (size_t i = 0; i < n && s->p < s->end; i++, s->p++)
		if (*s->p == '\n')
			s->line++;
}

/* Skips whitespace, comments and preprocessor-continuation newlines;
 * remembers TRANSLATORS comments. */
static void skip_ws(struct src *s)
{
	for (;;) {
		if (s->p >= s->end)
			return;
		if (isspace((unsigned char)*s->p)) {
			src_advance(s, 1);
		} else if (s->p[0] == '/' && s->p + 1 < s->end && s->p[1] == '*') {
			const char *e = strstr(s->p + 2, "*/");
			const char *t;
			size_t len = e ? (size_t)(e + 2 - s->p) : (size_t)(s->end - s->p);

			t = memmem(s->p, len, "TRANSLATORS:", 12);
			if (t) {
				struct sb b = { 0 };
				const char *q = t;
				const char *stop = e ? e : s->end;

				/* the comment text, one line each, without the " * " */
				while (q < stop) {
					const char *nl = memchr(q, '\n', (size_t)(stop - q));
					const char *le = nl ? nl : stop;
					const char *a = q;

					while (a < le && (*a == ' ' || *a == '\t' || *a == '*'))
						a++;
					if (le > a) {
						const char *z = le;

						while (z > a && (z[-1] == ' ' || z[-1] == '\t'))
							z--;
						if (b.n)
							sb_addc(&b, '\n');
						sb_addn(&b, a, (size_t)(z - a));
					}
					q = nl ? nl + 1 : stop;
				}
				free(s->tcomment);
				s->tcomment = sb_take(&b);
			}
			src_advance(s, len);
			if (t)
				s->tline = s->line;
		} else if (s->p[0] == '/' && s->p + 1 < s->end && s->p[1] == '/') {
			const char *nl = memchr(s->p, '\n', (size_t)(s->end - s->p));
			size_t len = nl ? (size_t)(nl - s->p) : (size_t)(s->end - s->p);
			const char *t = memmem(s->p, len, "TRANSLATORS:", 12);

			if (t) {
				free(s->tcomment);
				s->tcomment = strndup(t, (size_t)(s->p + len - t));
			}
			src_advance(s, len);
			if (t)
				s->tline = s->line;
		} else {
			return;
		}
	}
}

/* A string literal (adjacent ones joined) at s->p; false if there is none. */
static bool read_literal(struct src *s, char **out)
{
	struct sb b = { 0 };
	bool any = false;

	for (;;) {
		skip_ws(s);
		if (s->p >= s->end || *s->p != '"')
			break;
		any = true;
		src_advance(s, 1);
		while (s->p < s->end && *s->p != '"') {
			char c = *s->p;

			if (c == '\\' && s->p + 1 < s->end) {
				char e = s->p[1];

				src_advance(s, 2);
				switch (e) {
				case 'n': sb_addc(&b, '\n'); break;
				case 't': sb_addc(&b, '\t'); break;
				case 'r': sb_addc(&b, '\r'); break;
				case '\\': sb_addc(&b, '\\'); break;
				case '"': sb_addc(&b, '"'); break;
				case '\'': sb_addc(&b, '\''); break;
				case 'x': {
					int v = 0, k = 0;

					while (k < 2 && s->p < s->end && isxdigit((unsigned char)*s->p)) {
						v = v * 16 + (isdigit((unsigned char)*s->p) ? *s->p - '0' :
							      (tolower((unsigned char)*s->p) - 'a' + 10));
						src_advance(s, 1);
						k++;
					}
					sb_addc(&b, (char)v);
					break;
				}
				case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
					int v = e - '0', k = 1;

					while (k < 3 && s->p < s->end && *s->p >= '0' && *s->p <= '7') {
						v = v * 8 + (*s->p - '0');
						src_advance(s, 1);
						k++;
					}
					sb_addc(&b, (char)v);
					break;
				}
				default:
					sb_addc(&b, e);
					break;
				}
				continue;
			}
			sb_addc(&b, c);
			src_advance(s, 1);
		}
		src_advance(s, 1);   /* closing quote */
	}
	if (!any) {
		free(b.s);
		return false;
	}
	*out = sb_take(&b);
	return true;
}

static bool eat(struct src *s, char c)
{
	skip_ws(s);
	if (s->p < s->end && *s->p == c) {
		src_advance(s, 1);
		return true;
	}
	return false;
}

static void add_extracted(struct entries *es, const char *ctx, char *id, char *plural, const char *file, int line,
			  const char *comment)
{
	struct entry *e = entries_find(es, ctx, id);
	char ref[512];

	snprintf(ref, sizeof(ref), "%s:%d", file, line);
	if (e) {
		struct sb b = { 0 };

		sb_add(&b, e->refs);
		sb_addc(&b, ' ');
		sb_add(&b, ref);
		free(e->refs);
		e->refs = sb_take(&b);
		if (comment && (!e->comments[0] || !strstr(e->comments, comment))) {
			struct sb c = { 0 };

			sb_add(&c, e->comments);
			if (c.n)
				sb_addc(&c, '\n');
			sb_add(&c, comment);
			free(e->comments);
			e->comments = sb_take(&c);
		}
		if (plural && !e->plural)
			e->plural = plural;
		else
			free(plural);
		free(id);
		return;
	}
	e = entries_add(es);
	e->ctx = ctx ? xstrdup(ctx) : NULL;
	e->id = id;
	e->plural = plural;
	e->refs = xstrdup(ref);
	e->comments = xstrdup(comment ? comment : "");
	/* strftime formats are not printf formats */
	e->no_c_format = ctx && !strcmp(ctx, "strftime");
	e->c_format = !e->no_c_format && (has_conversion(id) || (plural && has_conversion(plural)));
}

static bool ident_char(char c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static void extract_file(struct entries *es, const char *path, const char *display)
{
	size_t len;
	char *buf = read_file(path, &len);
	struct src s = { buf, buf + len, display, 1, NULL, -100 };
	static const struct {
		const char *kw;
		bool ctx, plural;
	} kws[] = {
		{ "C_n", true, true }, { "NC_", true, false }, { "C_", true, false },
		{ "_n", false, true }, { "N_", false, false }, { "_", false, false },
	};

	while (s.p < s.end) {
		const char *p = s.p;
		int k;

		skip_ws(&s);
		if (s.p >= s.end)
			break;
		p = s.p;
		if (*p == '"') {
			char *junk;

			if (read_literal(&s, &junk))
				free(junk);
			continue;
		}
		if (*p == '\'') {
			src_advance(&s, 1);
			while (s.p < s.end && *s.p != '\'') {
				if (*s.p == '\\')
					src_advance(&s, 1);
				src_advance(&s, 1);
			}
			src_advance(&s, 1);
			continue;
		}
		if (!ident_char(*p)) {
			src_advance(&s, 1);
			continue;
		}
		/* an identifier: is it one of the keywords, called? */
		{
			const char *q = p;

			while (q < s.end && ident_char(*q))
				q++;
			for (k = 0; k < (int)(sizeof(kws) / sizeof(kws[0])); k++)
				if ((size_t)(q - p) == strlen(kws[k].kw) && !strncmp(p, kws[k].kw, (size_t)(q - p)))
					break;
			src_advance(&s, (size_t)(q - p));
			if (k == (int)(sizeof(kws) / sizeof(kws[0])))
				continue;
		}
		{
			int line = s.line;
			char *ctx = NULL, *id = NULL, *pl = NULL;
			const char *comment = s.tcomment && line - s.tline <= 2 ? s.tcomment : NULL;

			if (!eat(&s, '('))
				continue;
			if (kws[k].ctx) {
				if (!read_literal(&s, &ctx) || !eat(&s, ','))
					goto skip;
			}
			if (!read_literal(&s, &id))
				goto skip;
			if (kws[k].plural) {
				if (!eat(&s, ',') || !read_literal(&s, &pl))
					goto skip;
			}
			if (!id[0]) {
				fprintf(stderr, "%s:%d: warning: empty string marked for translation\n", display, line);
				goto skip;
			}
			add_extracted(es, ctx, id, pl, display, line, comment);
			id = pl = NULL;
			if (comment) {
				free(s.tcomment);
				s.tcomment = NULL;
			}
skip:
			free(ctx);
			free(id);
			free(pl);
		}
	}
	free(s.tcomment);
	free(buf);
}

static void po_write_string(FILE *f, const char *kw, const char *s)
{
	bool multi = strchr(s, '\n') && strchr(s, '\n')[1];

	fprintf(f, "%s ", kw);
	if (multi)
		fputs("\"\"\n", f);
	fputc('"', f);
	for (const char *p = s; *p; p++) {
		switch (*p) {
		case '\n':
			fputs("\\n", f);
			if (multi && p[1])
				fputs("\"\n\"", f);
			break;
		case '\t': fputs("\\t", f); break;
		case '\r': fputs("\\r", f); break;
		case '"': fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		default: fputc(*p, f); break;
		}
	}
	fputs("\"\n", f);
}

static int cmd_extract(int argc, char **argv)
{
	const char *out = NULL, *strip = "";
	struct entries es = { 0 };
	FILE *f;
	char tmp[1024];

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "--strip") && i + 1 < argc)
			strip = argv[++i];
		else {
			const char *d = argv[i];

			if (*strip && !strncmp(d, strip, strlen(strip)))
				d += strlen(strip);
			extract_file(&es, argv[i], d);
		}
	}
	if (!out)
		die("usage: rsos-i18n extract -o rsos.pot [--strip PREFIX] FILE.c...");
	snprintf(tmp, sizeof(tmp), "%s.tmp", out);
	f = fopen(tmp, "w");
	if (!f)
		die("%s: %s", tmp, strerror(errno));
	fputs("# RetroStoneOS frontend: every translatable string.\n"
	      "# Generated by `make pot` (rsos-i18n extract): do not edit. Translations go in\n"
	      "# frontend/po/<language>.po (docs/translating.md).\n"
	      "msgid \"\"\n"
	      "msgstr \"\"\n"
	      "\"Project-Id-Version: RetroStoneOS\\n\"\n"
	      "\"Language: \\n\"\n"
	      "\"MIME-Version: 1.0\\n\"\n"
	      "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
	      "\"Content-Transfer-Encoding: 8bit\\n\"\n"
	      "\"Plural-Forms: nplurals=2; plural=(n != 1);\\n\"\n", f);
	for (int i = 0; i < es.n; i++) {
		struct entry *e = &es.v[i];

		fputc('\n', f);
		if (e->comments[0]) {
			const char *c = e->comments;

			while (*c) {
				const char *nl = strchr(c, '\n');
				size_t l = nl ? (size_t)(nl - c) : strlen(c);

				fprintf(f, "#. %.*s\n", (int)l, c);
				c += l + (nl ? 1 : 0);
			}
		}
		{
			/* locations, wrapped */
			const char *r = e->refs;
			int col = 0;

			while (*r) {
				size_t l = strcspn(r, " ");

				if (!col) {
					fputs("#:", f);
					col = 2;
				}
				fprintf(f, " %.*s", (int)l, r);
				col += (int)l + 1;
				r += l;
				while (*r == ' ')
					r++;
				if (col > 70 || !*r) {
					fputc('\n', f);
					col = 0;
				}
			}
		}
		if (e->c_format)
			fputs("#, c-format\n", f);
		else if (e->no_c_format)
			fputs("#, no-c-format\n", f);
		if (e->ctx)
			po_write_string(f, "msgctxt", e->ctx);
		po_write_string(f, "msgid", e->id);
		if (e->plural) {
			po_write_string(f, "msgid_plural", e->plural);
			fputs("msgstr[0] \"\"\nmsgstr[1] \"\"\n", f);
		} else {
			fputs("msgstr \"\"\n", f);
		}
	}
	if (fclose(f))
		die("%s: %s", tmp, strerror(errno));
	if (rename(tmp, out))
		die("%s: %s", out, strerror(errno));
	fprintf(stderr, "rsos-i18n: %s: %d strings\n", out, es.n);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		die("usage: rsos-i18n extract|compile|check|check-pot|chars|fontcheck ...");
	if (!strcmp(argv[1], "extract"))
		return cmd_extract(argc - 2, argv + 2);
	if (!strcmp(argv[1], "compile"))
		return cmd_compile(argc - 2, argv + 2);
	if (!strcmp(argv[1], "check"))
		return cmd_check(argc - 2, argv + 2);
	if (!strcmp(argv[1], "check-pot"))
		return cmd_check_pot(argc - 2, argv + 2);
	if (!strcmp(argv[1], "chars"))
		return cmd_chars(argc - 2, argv + 2);
	if (!strcmp(argv[1], "fontcheck"))
		return cmd_fontcheck(argc - 2, argv + 2);
	die("unknown command %s", argv[1]);
}
