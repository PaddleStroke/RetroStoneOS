/*
 * settings.c - see settings.h.
 */
#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

struct line {
	char *text;      /* full original line (without newline) */
	char *key;       /* NULL for comments/blank lines */
	char *value;
};

struct pending {
	char *key, *value;
};

struct settings {
	char path[1024];
	struct line *lines;
	int n, cap;
	struct pending *pend;
	int npend;
	/* the file exists but could not be read (the values may come from
	 * settings.ini.bak): never rewritten (review F-M7) */
	bool unreadable;
};

static void clear_lines(struct settings *s)
{
	for (int i = 0; i < s->n; i++) {
		free(s->lines[i].text);
		free(s->lines[i].key);
		free(s->lines[i].value);
	}
	s->n = 0;
}

static void add_line(struct settings *s, const char *text)
{
	struct line *l;
	char *eq;

	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 32;
		s->lines = xrealloc(s->lines, sizeof(*s->lines) * (size_t)s->cap);
	}
	l = &s->lines[s->n++];
	l->text = xstrdup(text);
	l->key = l->value = NULL;
	if (text[0] == '#' || text[0] == ';' || text[0] == '[')
		return;
	eq = strchr(text, '=');
	if (eq) {
		char *k = xstrdup(text), *v;

		k[eq - text] = 0;
		v = xstrdup(eq + 1);
		l->key = xstrdup(str_trim(k));
		l->value = xstrdup(str_trim(v));
		free(k);
		free(v);
		if (!l->key[0]) {
			free(l->key);
			l->key = NULL;
		}
	}
}

static void load(struct settings *s)
{
	int err;
	bool from_bak;
	char *buf = file_read_user(s->path, NULL, &err, &from_bak), *p, *nl;

	clear_lines(s);
	s->unreadable = err && err != -ENOENT;
	if (!buf)
		return;
	for (p = buf; *p; p = nl + 1) {
		nl = strchr(p, '\n');
		if (!nl) {
			if (*p) {
				size_t l = strlen(p);

				if (l && p[l - 1] == '\r')
					p[l - 1] = 0;
				add_line(s, p);
			}
			break;
		}
		*nl = 0;
		if (nl > p && nl[-1] == '\r')
			nl[-1] = 0;
		add_line(s, p);
	}
	free(buf);
}

struct settings *settings_open(const char *path)
{
	struct settings *s = xcalloc(1, sizeof(*s));

	strlcpy_(s->path, path, sizeof(s->path));
	load(s);
	return s;
}

void settings_close(struct settings *s)
{
	if (!s)
		return;
	clear_lines(s);
	free(s->lines);
	for (int i = 0; i < s->npend; i++) {
		free(s->pend[i].key);
		free(s->pend[i].value);
	}
	free(s->pend);
	free(s);
}

void settings_reload(struct settings *s)
{
	load(s);
}

const char *settings_get(const struct settings *s, const char *key, const char *def)
{
	/* pending changes first, then the last occurrence in the file */
	for (int i = s->npend - 1; i >= 0; i--)
		if (!strcmp(s->pend[i].key, key))
			return s->pend[i].value;
	for (int i = s->n - 1; i >= 0; i--)
		if (s->lines[i].key && !strcmp(s->lines[i].key, key))
			return s->lines[i].value;
	return def;
}

int settings_get_int(const struct settings *s, const char *key, int def)
{
	const char *v = settings_get(s, key, NULL);

	return v && *v ? atoi(v) : def;
}

bool settings_get_bool(const struct settings *s, const char *key, bool def)
{
	return parse_bool(settings_get(s, key, NULL), def);
}

void settings_set(struct settings *s, const char *key, const char *value)
{
	for (int i = 0; i < s->npend; i++) {
		if (!strcmp(s->pend[i].key, key)) {
			free(s->pend[i].value);
			s->pend[i].value = xstrdup(value);
			return;
		}
	}
	s->pend = xrealloc(s->pend, sizeof(*s->pend) * (size_t)(s->npend + 1));
	s->pend[s->npend].key = xstrdup(key);
	s->pend[s->npend].value = xstrdup(value);
	s->npend++;
}

void settings_set_int(struct settings *s, const char *key, int v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d", v);
	settings_set(s, key, buf);
}

int settings_save(struct settings *s)
{
	size_t cap = 4096, len = 0;
	char *out;
	int r;
	char dir[1024];

	if (!s->npend)
		return 0;
	load(s); /* pick up changes made by other programs */
	if (s->unreadable) {
		/* a failed read is not an empty file: the changes stay pending
		 * (in effect for this session), the file is left alone */
		LOGW("settings: %s could not be read: not rewritten", s->path);
		return -EIO;
	}
	for (int p = 0; p < s->npend; p++) {
		bool found = false;

		for (int i = 0; i < s->n; i++) {
			struct line *l = &s->lines[i];

			if (l->key && !strcmp(l->key, s->pend[p].key)) {
				char buf[2048];

				if (found) {
					/* drop duplicates */
					free(l->key);
					l->key = NULL;
					free(l->text);
					l->text = NULL;
					continue;
				}
				found = true;
				snprintf(buf, sizeof(buf), "%s=%s", l->key, s->pend[p].value);
				free(l->text);
				l->text = xstrdup(buf);
				free(l->value);
				l->value = xstrdup(s->pend[p].value);
			}
		}
		if (!found) {
			char buf[2048];

			snprintf(buf, sizeof(buf), "%s=%s", s->pend[p].key, s->pend[p].value);
			add_line(s, buf);
		}
	}
	out = xmalloc(cap);
	for (int i = 0; i < s->n; i++) {
		size_t l;

		if (!s->lines[i].text)
			continue;
		l = strlen(s->lines[i].text);
		if (len + l + 2 > cap) {
			cap = (len + l + 2) * 2;
			out = xrealloc(out, cap);
		}
		memcpy(out + len, s->lines[i].text, l);
		len += l;
		out[len++] = '\n';
	}
	path_dirname(s->path, dir, sizeof(dir));
	mkdir_p(dir);
	r = file_write_atomic_bak(s->path, out, len);
	free(out);
	if (r < 0) {
		LOGW("settings: cannot write %s", s->path);
		return r;
	}
	for (int i = 0; i < s->npend; i++) {
		free(s->pend[i].key);
		free(s->pend[i].value);
	}
	s->npend = 0;
	load(s);
	return 0;
}
