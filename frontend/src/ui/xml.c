/*
 * xml.c - see xml.h.
 */
#include "xml.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Limits against broken files (a gamelist or a theme from a USB stick):
 * elements nest at most XML_MAX_DEPTH deep (a deeper one is kept, but as
 * an empty element: its content goes to its parent) and an element keeps
 * at most XML_MAX_TEXT bytes of text. The text of the open elements grows
 * in per-depth buffers (geometric) and goes to the arena once, when the
 * element closes: linear, however many pieces (comments, child elements)
 * split it.
 */
#define XML_MAX_DEPTH 256
#define XML_MAX_TEXT (64 * 1024)
#define XML_CLOSE_SEARCH 64      /* open elements a close tag is matched against */

struct text_buf {
	char *s;
	size_t len, cap;
};

struct parser {
	struct xml_doc *doc;
	char *p, *end;
	int line;
	struct xml_node *cur;
	int depth;                               /* of cur, 0 = doc->top */
	struct text_buf text[XML_MAX_DEPTH + 1]; /* of the open elements, by depth */
};

static bool is_name_char(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' ||
	       c == ':' || (unsigned char)c >= 0x80;
}

static void skip_ws(struct parser *ps)
{
	while (ps->p < ps->end && isspace((unsigned char)*ps->p)) {
		if (*ps->p == '\n')
			ps->line++;
		ps->p++;
	}
}

static char *find_str(struct parser *ps, const char *s)
{
	size_t l = strlen(s);
	char *q = ps->p;

	while (q + l <= ps->end) {
		if (*q == s[0] && !memcmp(q, s, l))
			return q;
		if (*q == '\n')
			ps->line++;
		q++;
	}
	return NULL;
}

static int utf8_encode(char *out, unsigned cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	if (cp < 0x110000) {
		out[0] = (char)(0xf0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[3] = (char)(0x80 | (cp & 0x3f));
		return 4;
	}
	out[0] = '?';
	return 1;
}

/* Decodes entities of s[0..n) in place, returns the new length. */
static size_t decode_entities(char *s, size_t n)
{
	char *r = s, *w = s, *e = s + n;

	while (r < e) {
		if (*r == '&') {
			char *semi = memchr(r, ';', (size_t)(e - r) < 12 ? (size_t)(e - r) : 12);

			if (semi) {
				size_t l = (size_t)(semi - r - 1);
				const char *name = r + 1;
				int c = -1;

				if (l == 3 && !memcmp(name, "amp", 3))
					c = '&';
				else if (l == 2 && !memcmp(name, "lt", 2))
					c = '<';
				else if (l == 2 && !memcmp(name, "gt", 2))
					c = '>';
				else if (l == 4 && !memcmp(name, "quot", 4))
					c = '"';
				else if (l == 4 && !memcmp(name, "apos", 4))
					c = '\'';
				else if (l == 4 && !memcmp(name, "nbsp", 4))
					c = 0xa0;
				else if (l >= 2 && name[0] == '#') {
					unsigned long v;

					if (name[1] == 'x' || name[1] == 'X')
						v = strtoul(name + 2, NULL, 16);
					else
						v = strtoul(name + 1, NULL, 10);
					c = (int)(v > 0x10ffff ? '?' : v);
				}
				if (c >= 0) {
					w += utf8_encode(w, (unsigned)c);
					r = semi + 1;
					continue;
				}
			}
		}
		*w++ = *r++;
	}
	return (size_t)(w - s);
}

/* Appends s[0..n) to the text of the current element (up to XML_MAX_TEXT). */
static void text_append(struct parser *ps, const char *s, size_t n)
{
	struct text_buf *b = &ps->text[ps->depth];

	if (ps->depth == 0)
		return;
	if (n > XML_MAX_TEXT - b->len)
		n = XML_MAX_TEXT - b->len;
	if (!n)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 256;

		while (cap < b->len + n + 1)
			cap *= 2;
		b->s = xrealloc(b->s, cap);
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = 0;
}

static void add_text(struct parser *ps, char *s, size_t n)
{
	size_t i;
	bool blank = true;

	if (ps->depth == 0)
		return;
	for (i = 0; i < n; i++) {
		if (!isspace((unsigned char)s[i])) {
			blank = false;
			break;
		}
	}
	if (blank && !ps->text[ps->depth].len)
		return;
	n = decode_entities(s, n);
	text_append(ps, s, n);
}

static void close_node(struct parser *ps)
{
	struct xml_node *n = ps->cur;
	struct text_buf *b = &ps->text[ps->depth];

	if (ps->depth == 0)
		return;
	if (b->len) {
		char *s = str_trim(b->s);

		if (*s)
			n->text = arena_strdup(&ps->doc->arena, s);
		b->len = 0;
	}
	ps->depth--;
	ps->cur = n->parent ? n->parent : &ps->doc->top;
}

static void parse_tag(struct parser *ps)
{
	struct xml_node *n;
	char *name, *q;
	char save;
	bool self_close = false;
	struct xml_attr **ap;

	/* ps->p is just after '<' */
	name = ps->p;
	while (ps->p < ps->end && is_name_char(*ps->p))
		ps->p++;
	if (ps->p == name) {
		/* Not a tag: treat '<' as text. */
		add_text(ps, name - 1, 1);
		return;
	}
	n = arena_alloc(&ps->doc->arena, sizeof(*n));
	n->name = arena_strndup(&ps->doc->arena, name, (size_t)(ps->p - name));
	n->text = "";
	n->line = ps->line;
	n->parent = ps->cur;
	if (ps->cur->last_child)
		ps->cur->last_child->next = n;
	else
		ps->cur->child = n;
	ps->cur->last_child = n;
	if (ps->cur == &ps->doc->top && !ps->doc->root)
		ps->doc->root = n;
	ap = &n->attrs;

	/* attributes */
	for (;;) {
		char *an, *av;
		size_t anl;
		char quote;

		skip_ws(ps);
		if (ps->p >= ps->end)
			break;
		if (*ps->p == '>') {
			ps->p++;
			break;
		}
		if (*ps->p == '/' && ps->p + 1 < ps->end && ps->p[1] == '>') {
			self_close = true;
			ps->p += 2;
			break;
		}
		an = ps->p;
		while (ps->p < ps->end && is_name_char(*ps->p))
			ps->p++;
		anl = (size_t)(ps->p - an);
		if (!anl) {
			ps->p++; /* junk: skip a char */
			continue;
		}
		skip_ws(ps);
		av = NULL;
		if (ps->p < ps->end && *ps->p == '=') {
			ps->p++;
			skip_ws(ps);
			if (ps->p < ps->end && (*ps->p == '"' || *ps->p == '\'')) {
				quote = *ps->p++;
				av = ps->p;
				q = memchr(ps->p, quote, (size_t)(ps->end - ps->p));
				if (!q)
					q = ps->end;
				ps->p = q < ps->end ? q + 1 : q;
			} else {
				av = ps->p;
				while (ps->p < ps->end && !isspace((unsigned char)*ps->p) &&
				       *ps->p != '>' && *ps->p != '/')
					ps->p++;
				q = ps->p;
			}
			save = *q;
			(void)save;
			{
				size_t l = decode_entities(av, (size_t)(q - av));
				struct xml_attr *a = arena_alloc(&ps->doc->arena, sizeof(*a));

				a->name = arena_strndup(&ps->doc->arena, an, anl);
				a->value = arena_strndup(&ps->doc->arena, av, l);
				*ap = a;
				ap = &a->next;
			}
		} else {
			struct xml_attr *a = arena_alloc(&ps->doc->arena, sizeof(*a));

			a->name = arena_strndup(&ps->doc->arena, an, anl);
			a->value = "";
			*ap = a;
			ap = &a->next;
		}
	}
	if (self_close)
		return;
	if (ps->depth >= XML_MAX_DEPTH) {
		/* too deep: kept empty, its content goes to the parent */
		return;
	}
	ps->cur = n;
	ps->depth++;
	ps->text[ps->depth].len = 0;
}

static void parse_close(struct parser *ps)
{
	char *name = ps->p;
	size_t l;
	struct xml_node *n;
	char *gt;

	while (ps->p < ps->end && is_name_char(*ps->p))
		ps->p++;
	l = (size_t)(ps->p - name);
	gt = memchr(ps->p, '>', (size_t)(ps->end - ps->p));
	ps->p = gt ? gt + 1 : ps->end;
	/* Find the matching open element among the XML_CLOSE_SEARCH innermost
	 * ones (real files are a few levels deep); ignore unmatched close tags. */
	n = ps->cur;
	for (int d = ps->depth; d > 0 && d > ps->depth - XML_CLOSE_SEARCH; d--, n = n->parent) {
		if (strlen(n->name) == l && !memcmp(n->name, name, l))
			goto found;
	}
	return;
found:
	while (ps->cur != n)
		close_node(ps);
	close_node(ps);
}

struct xml_doc *xml_parse(const char *data, size_t len)
{
	struct xml_doc *doc = xcalloc(1, sizeof(*doc));
	struct parser ps;

	arena_init(&doc->arena, 32768);
	doc->buf = xmalloc(len + 1);
	memcpy(doc->buf, data, len);
	doc->buf[len] = 0;
	doc->top.name = "";
	doc->top.text = "";

	ps.doc = doc;
	ps.p = doc->buf;
	ps.end = doc->buf + len;
	ps.line = 1;
	ps.cur = &doc->top;
	ps.depth = 0;
	memset(ps.text, 0, sizeof(ps.text));

	/* UTF-8 BOM */
	if (len >= 3 && !memcmp(ps.p, "\xef\xbb\xbf", 3))
		ps.p += 3;

	while (ps.p < ps.end) {
		char *lt = memchr(ps.p, '<', (size_t)(ps.end - ps.p));
		char *q;

		if (!lt) {
			add_text(&ps, ps.p, (size_t)(ps.end - ps.p));
			break;
		}
		if (lt > ps.p) {
			for (q = ps.p; q < lt; q++)
				if (*q == '\n')
					ps.line++;
			add_text(&ps, ps.p, (size_t)(lt - ps.p));
		}
		ps.p = lt + 1;
		if (ps.p >= ps.end)
			break;
		if (*ps.p == '!') {
			if (ps.end - ps.p >= 3 && !memcmp(ps.p, "!--", 3)) {
				ps.p += 3;
				q = find_str(&ps, "-->");
				ps.p = q ? q + 3 : ps.end;
			} else if (ps.end - ps.p >= 8 && !memcmp(ps.p, "![CDATA[", 8)) {
				ps.p += 8;
				q = find_str(&ps, "]]>");
				if (!q)
					q = ps.end;
				/* CDATA is literal: no entity decoding. Protect '&'. */
				text_append(&ps, ps.p, (size_t)(q - ps.p));
				ps.p = q < ps.end ? q + 3 : ps.end;
			} else {
				q = memchr(ps.p, '>', (size_t)(ps.end - ps.p));
				ps.p = q ? q + 1 : ps.end;
			}
		} else if (*ps.p == '?') {
			q = find_str(&ps, "?>");
			ps.p = q ? q + 2 : ps.end;
		} else if (*ps.p == '/') {
			ps.p++;
			parse_close(&ps);
		} else {
			parse_tag(&ps);
		}
	}
	while (ps.depth > 0)
		close_node(&ps);
	for (int i = 0; i <= XML_MAX_DEPTH; i++)
		free(ps.text[i].s);
	return doc;
}

struct xml_doc *xml_load(const char *path)
{
	size_t len;
	char *buf = file_read(path, &len);
	struct xml_doc *doc;

	if (!buf)
		return NULL;
	doc = xml_parse(buf, len);
	free(buf);
	return doc;
}

void xml_free(struct xml_doc *doc)
{
	if (!doc)
		return;
	arena_free(&doc->arena);
	free(doc->buf);
	free(doc);
}

const char *xml_attr(const struct xml_node *n, const char *name)
{
	const struct xml_attr *a;

	for (a = n->attrs; a; a = a->next)
		if (!strcmp(a->name, name))
			return a->value;
	return NULL;
}

struct xml_node *xml_child(const struct xml_node *n, const char *name)
{
	struct xml_node *c;

	for (c = n->child; c; c = c->next)
		if (!strcmp(c->name, name))
			return c;
	return NULL;
}

const char *xml_child_text(const struct xml_node *n, const char *name)
{
	struct xml_node *c = xml_child(n, name);

	return c ? c->text : NULL;
}
