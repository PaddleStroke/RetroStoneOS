/*
 * json.c - see json.h.
 */
#include "json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

struct parser {
	const char *p, *end;
	int depth;
	char *err;
	size_t errlen;
};

static void fail(struct parser *ps, const char *what)
{
	if (ps->err && ps->errlen && !ps->err[0])
		snprintf(ps->err, ps->errlen, "json: %s at offset %ld", what, (long)(ps->end - ps->p));
}

static void ws(struct parser *ps)
{
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static struct json *node(enum json_type t)
{
	struct json *j = calloc(1, sizeof(*j));

	if (j)
		j->type = t;
	return j;
}

void json_free(struct json *j)
{
	while (j) {
		struct json *n = j->next;

		json_free(j->child);
		free(j->key);
		free(j->str);
		free(j);
		j = n;
	}
}

static int hex4(const char *p, unsigned *out)
{
	unsigned v = 0;

	for (int i = 0; i < 4; i++) {
		char c = p[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned)(c - 'A' + 10);
		else
			return -1;
	}
	*out = v;
	return 0;
}

static size_t utf8_put(char *o, unsigned cp)
{
	if (cp < 0x80) {
		o[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = (char)(0xc0 | cp >> 6);
		o[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (char)(0xe0 | cp >> 12);
		o[1] = (char)(0x80 | (cp >> 6 & 0x3f));
		o[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	o[0] = (char)(0xf0 | cp >> 18);
	o[1] = (char)(0x80 | (cp >> 12 & 0x3f));
	o[2] = (char)(0x80 | (cp >> 6 & 0x3f));
	o[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/* at the opening quote; returns a malloc'ed UTF-8 string */
static char *string(struct parser *ps)
{
	const char *s = ++ps->p;
	size_t cap = 0;
	char *out, *o;

	/* the unescaped string is never longer than the escaped one */
	while (ps->p < ps->end && *ps->p != '"') {
		if ((unsigned char)*ps->p < 0x20) {
			fail(ps, "control character in a string");
			return NULL;
		}
		if (*ps->p == '\\')
			ps->p++;
		ps->p++;
	}
	if (ps->p >= ps->end) {
		fail(ps, "unterminated string");
		return NULL;
	}
	cap = (size_t)(ps->p - s) + 1;
	out = malloc(cap);
	if (!out) {
		fail(ps, "out of memory");
		return NULL;
	}
	o = out;
	for (const char *q = s; q < ps->p; q++) {
		unsigned cp, lo;

		if (*q != '\\') {
			*o++ = *q;
			continue;
		}
		q++;
		switch (*q) {
		case '"': *o++ = '"'; break;
		case '\\': *o++ = '\\'; break;
		case '/': *o++ = '/'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'n': *o++ = '\n'; break;
		case 'r': *o++ = '\r'; break;
		case 't': *o++ = '\t'; break;
		case 'u':
			if (ps->p - q < 5 || hex4(q + 1, &cp) < 0)
				goto bad;
			q += 4;
			if (cp >= 0xd800 && cp < 0xdc00) {
				if (ps->p - q < 7 || q[1] != '\\' || q[2] != 'u' || hex4(q + 3, &lo) < 0 ||
				    lo < 0xdc00 || lo > 0xdfff)
					cp = 0xfffd;
				else {
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
					q += 6;
				}
			} else if (cp >= 0xdc00 && cp < 0xe000) {
				cp = 0xfffd;
			}
			if (cp == 0)
				cp = 0xfffd;              /* no NUL inside C strings */
			/* \uXXXX is 6 bytes, its UTF-8 at most 3 (4 for a 12-byte pair) */
			o += utf8_put(o, cp);
			break;
		default:
			goto bad;
		}
	}
	*o = 0;
	ps->p++;
	return out;
bad:
	free(out);
	fail(ps, "bad escape");
	return NULL;
}

static struct json *value(struct parser *ps);

static struct json *container(struct parser *ps, bool obj)
{
	struct json *j = node(obj ? JSON_OBJ : JSON_ARR), **tail;

	if (!j) {
		fail(ps, "out of memory");
		return NULL;
	}
	if (++ps->depth > JSON_MAX_DEPTH) {
		fail(ps, "nested too deep");
		free(j);
		return NULL;
	}
	tail = &j->child;
	ps->p++;
	ws(ps);
	if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
		ps->p++;
		ps->depth--;
		return j;
	}
	for (;;) {
		char *key = NULL;
		struct json *v;

		ws(ps);
		if (obj) {
			if (ps->p >= ps->end || *ps->p != '"') {
				fail(ps, "member name expected");
				goto err;
			}
			key = string(ps);
			if (!key)
				goto err;
			ws(ps);
			if (ps->p >= ps->end || *ps->p != ':') {
				free(key);
				fail(ps, "':' expected");
				goto err;
			}
			ps->p++;
		}
		v = value(ps);
		if (!v) {
			free(key);
			goto err;
		}
		v->key = key;
		*tail = v;
		tail = &v->next;
		ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
			ps->p++;
			break;
		}
		fail(ps, obj ? "',' or '}' expected" : "',' or ']' expected");
		goto err;
	}
	ps->depth--;
	return j;
err:
	json_free(j);
	return NULL;
}

static bool word(struct parser *ps, const char *w)
{
	size_t n = strlen(w);

	if ((size_t)(ps->end - ps->p) >= n && !memcmp(ps->p, w, n)) {
		ps->p += n;
		return true;
	}
	return false;
}

static struct json *value(struct parser *ps)
{
	struct json *j;

	ws(ps);
	if (ps->p >= ps->end) {
		fail(ps, "value expected");
		return NULL;
	}
	switch (*ps->p) {
	case '{':
		return container(ps, true);
	case '[':
		return container(ps, false);
	case '"': {
		char *s = string(ps);

		if (!s)
			return NULL;
		j = node(JSON_STR);
		if (!j) {
			free(s);
			return NULL;
		}
		j->str = s;
		return j;
	}
	case 't':
	case 'f':
	case 'n':
		if (word(ps, "true") || word(ps, "false")) {
			j = node(JSON_BOOL);
			if (j)
				j->b = ps->p[-1] == 'e' && ps->p[-2] == 'u';
			return j;
		}
		if (word(ps, "null"))
			return node(JSON_NULL);
		fail(ps, "bad literal");
		return NULL;
	default: {
		char buf[64];
		size_t n = 0;
		char *e;

		while (ps->p + n < ps->end && n < sizeof(buf) - 1 &&
		       strchr("+-0123456789.eE", ps->p[n]))
			n++;
		if (!n) {
			fail(ps, "unexpected character");
			return NULL;
		}
		memcpy(buf, ps->p, n);
		buf[n] = 0;
		j = node(JSON_NUM);
		if (!j)
			return NULL;
		j->num = strtod(buf, &e);
		if (*e) {
			free(j);
			fail(ps, "bad number");
			return NULL;
		}
		ps->p += n;
		return j;
	}
	}
}

struct json *json_parse(const char *text, size_t len, char *err, size_t errlen)
{
	struct parser ps = { text, text + len, 0, err, errlen };
	struct json *j;

	if (err && errlen)
		err[0] = 0;
	j = value(&ps);
	if (!j)
		return NULL;
	ws(&ps);
	if (ps.p != ps.end) {
		fail(&ps, "trailing data");
		json_free(j);
		return NULL;
	}
	return j;
}

const struct json *json_get(const struct json *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJ)
		return NULL;
	for (const struct json *c = obj->child; c; c = c->next)
		if (c->key && !strcmp(c->key, key))
			return c;
	return NULL;
}

const char *json_str(const struct json *obj, const char *key)
{
	const struct json *j = json_get(obj, key);

	return j && j->type == JSON_STR ? j->str : NULL;
}

double json_num(const struct json *obj, const char *key, double def)
{
	const struct json *j = json_get(obj, key);

	return j && j->type == JSON_NUM ? j->num : def;
}

bool json_bool(const struct json *obj, const char *key, bool def)
{
	const struct json *j = json_get(obj, key);

	return j && j->type == JSON_BOOL ? j->b : def;
}
