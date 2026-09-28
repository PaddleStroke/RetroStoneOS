/*
 * ini.h - minimal INI reader/writer for the host.
 *
 * Accepts the core metadata format (docs/cores.md: `[section]`, `key = value`,
 * `;` or `#` comments, also after a value when preceded by a blank) and the
 * RetroArch .opt format (`key = "value"`), so RetroArch core option files
 * can be copied as-is. Keys before any section belong to section "".
 */
#ifndef RSOS_HOST_INI_H
#define RSOS_HOST_INI_H

#include <stdbool.h>

struct ini_entry {
	char *section;
	char *key;
	char *value;
};

struct ini {
	struct ini_entry *e;
	int n, cap;
};

/* 0 on success, -errno if the file cannot be read (ini stays empty). */
int ini_load(struct ini *ini, const char *path);
/* Parses text (modified in place is not required: copied). */
int ini_parse(struct ini *ini, const char *text);
void ini_free(struct ini *ini);

/* Last value for (section, key), NULL if absent. Case-insensitive keys. */
const char *ini_get(const struct ini *ini, const char *section, const char *key);
bool ini_get_bool(const struct ini *ini, const char *section, const char *key, bool def);
int ini_get_int(const struct ini *ini, const char *section, const char *key, int def);

/* Adds or replaces. */
int ini_set(struct ini *ini, const char *section, const char *key, const char *value);

#endif
