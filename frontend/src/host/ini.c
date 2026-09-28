/*
 * ini.c - see ini.h.
 */
#include "ini.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hutil.h"

static char *dup_range(const char *s, size_t n)
{
	char *d = malloc(n + 1);

	if (d) {
		memcpy(d, s, n);
		d[n] = 0;
	}
	return d;
}

static int add(struct ini *ini, const char *sec, size_t sl, const char *key, size_t kl,
	       const char *val, size_t vl)
{
	struct ini_entry *e;

	if (ini->n == ini->cap) {
		int nc = ini->cap ? ini->cap * 2 : 32;
		struct ini_entry *ne = realloc(ini->e, (size_t)nc * sizeof(*ne));

		if (!ne)
			return -ENOMEM;
		ini->e = ne;
		ini->cap = nc;
	}
	e = &ini->e[ini->n];
	e->section = dup_range(sec, sl);
	e->key = dup_range(key, kl);
	e->value = dup_range(val, vl);
	if (!e->section || !e->key || !e->value) {
		free(e->section);
		free(e->key);
		free(e->value);
		return -ENOMEM;
	}
	ini->n++;
	return 0;
}

int ini_parse(struct ini *ini, const char *text)
{
	const char *p = text;
	const char *sec = "";
	size_t sl = 0;

	while (*p) {
		const char *line = p, *end, *eq, *k, *v;
		size_t kl, vl;

		while (*p && *p != '\n')
			p++;
		end = p;
		if (*p)
			p++;
		while (line < end && isspace((unsigned char)*line))
			line++;
		while (end > line && isspace((unsigned char)end[-1]))
			end--;
		if (line == end || *line == ';' || *line == '#')
			continue;
		if (*line == '[') {
			const char *c = memchr(line, ']', (size_t)(end - line));

			if (c) {
				sec = line + 1;
				sl = (size_t)(c - sec);
			}
			continue;
		}
		eq = memchr(line, '=', (size_t)(end - line));
		if (!eq)
			continue;
		k = line;
		kl = (size_t)(eq - line);
		while (kl && isspace((unsigned char)k[kl - 1]))
			kl--;
		v = eq + 1;
		while (v < end && isspace((unsigned char)*v))
			v++;
		if (v < end && *v == '"') {
			const char *q = memchr(v + 1, '"', (size_t)(end - v - 1));

			v++;
			vl = q ? (size_t)(q - v) : (size_t)(end - v);
		} else {
			/* Inline comment: ';' or '#' preceded by a blank. */
			const char *c = v;

			for (; c < end; c++)
				if ((*c == ';' || *c == '#') && c > v && isspace((unsigned char)c[-1]))
					break;
			vl = (size_t)(c - v);
			while (vl && isspace((unsigned char)v[vl - 1]))
				vl--;
		}
		if (kl && add(ini, sec, sl, k, kl, v, vl) < 0)
			return -ENOMEM;
	}
	return 0;
}

int ini_load(struct ini *ini, const char *path)
{
	size_t size;
	char *text = hread_file(path, &size);
	int ret;

	if (!text)
		return errno ? -errno : -EIO;
	ret = ini_parse(ini, text);
	free(text);
	return ret;
}

void ini_free(struct ini *ini)
{
	for (int i = 0; i < ini->n; i++) {
		free(ini->e[i].section);
		free(ini->e[i].key);
		free(ini->e[i].value);
	}
	free(ini->e);
	ini->e = NULL;
	ini->n = ini->cap = 0;
}

const char *ini_get(const struct ini *ini, const char *section, const char *key)
{
	for (int i = ini->n - 1; i >= 0; i--)
		if (!strcasecmp(ini->e[i].section, section) && !strcasecmp(ini->e[i].key, key))
			return ini->e[i].value;
	return NULL;
}

bool ini_get_bool(const struct ini *ini, const char *section, const char *key, bool def)
{
	const char *v = ini_get(ini, section, key);

	if (!v || !*v)
		return def;
	if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1"))
		return true;
	if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0"))
		return false;
	return def;
}

int ini_get_int(const struct ini *ini, const char *section, const char *key, int def)
{
	const char *v = ini_get(ini, section, key);
	char *e;
	long l;

	if (!v || !*v)
		return def;
	l = strtol(v, &e, 0);
	return *e ? def : (int)l;
}

int ini_set(struct ini *ini, const char *section, const char *key, const char *value)
{
	for (int i = ini->n - 1; i >= 0; i--)
		if (!strcasecmp(ini->e[i].section, section) && !strcasecmp(ini->e[i].key, key)) {
			char *v = strdup(value);

			if (!v)
				return -ENOMEM;
			free(ini->e[i].value);
			ini->e[i].value = v;
			return 0;
		}
	return add(ini, section, strlen(section), key, strlen(key), value, strlen(value));
}
