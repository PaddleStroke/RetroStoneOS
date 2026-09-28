/*
 * settings.h - /data/rsos/settings.ini: plain "key=value" lines.
 *
 * Other programs write this file too (rsos-net writes wifi=, eth=, bt=), so:
 *  - unknown keys, comments and line order are preserved;
 *  - settings_save() re-reads the file and only rewrites the keys this
 *    process changed since the last load/save (no lost updates).
 */
#ifndef RSOS_UI_SETTINGS_H
#define RSOS_UI_SETTINGS_H

#include <stdbool.h>

struct settings;

struct settings *settings_open(const char *path);   /* never NULL */
void settings_close(struct settings *s);
/* Re-reads the file (e.g. after rsos-net changed it). Pending changes stay. */
void settings_reload(struct settings *s);
const char *settings_get(const struct settings *s, const char *key, const char *def);
int settings_get_int(const struct settings *s, const char *key, int def);
bool settings_get_bool(const struct settings *s, const char *key, bool def);
void settings_set(struct settings *s, const char *key, const char *value);
void settings_set_int(struct settings *s, const char *key, int v);
/* Writes pending changes atomically. 0 or -errno. */
int settings_save(struct settings *s);

#endif
