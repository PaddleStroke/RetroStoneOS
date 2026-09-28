/*
 * i18n.h - translations for every user-visible string of the frontend (menu
 * UI, dialogs, toasts, the in-game menu and OSD of the game process, the
 * splash messages). gettext-style, dependency-free (docs/translating.md):
 *
 *   _("Settings")                    the translation, or the English text
 *   N_("Settings")                   marks a string for extraction only
 *                                    (static tables): translate it with _()
 *                                    where it is shown
 *   C_("button", "Back")             with a context (same English, other
 *                                    meaning); NC_() = N_() with a context
 *   _n("%d game", "%d games", n)     plural forms (the .po Plural-Forms)
 *
 * The English text is the key (msgid). Catalogs are .po files
 * (frontend/po/<lang>.po) compiled at build time by rsos-i18n into a small
 * hashed binary (<lang>.cat, i18n_cat.h), installed in
 * /usr/share/rsos/locale and mmap()ed at run time. A missing catalog or
 * translation gives the English text.
 *
 * printf-style messages: the translated format has the same conversions as
 * the English one (rsos-i18n refuses a translation that differs, so a bad
 * .po cannot crash the frontend); translators may reorder them with
 * positional arguments ("%2$s ... %1$d"). GCC checks the English format
 * against the arguments (format_arg).
 *
 * Returned strings stay valid for the life of the process, also after a
 * language switch (catalogs are never unmapped). Lookups are thread-safe.
 * Log lines are never translated.
 */
#ifndef RSOS_I18N_H
#define RSOS_I18N_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

const char *i18n_gettext(const char *msgid) __attribute__((format_arg(1)));
const char *i18n_pgettext(const char *ctx, const char *msgid) __attribute__((format_arg(2)));
const char *i18n_ngettext(const char *msgid, const char *plural, unsigned long n)
	__attribute__((format_arg(1))) __attribute__((format_arg(2)));
const char *i18n_npgettext(const char *ctx, const char *msgid, const char *plural, unsigned long n)
	__attribute__((format_arg(2))) __attribute__((format_arg(3)));

#define _(s)              i18n_gettext(s)
#define N_(s)             (s)
#define C_(ctx, s)        i18n_pgettext(ctx, s)
#define NC_(ctx, s)       (s)
#define _n(s, p, n)       i18n_ngettext(s, p, (unsigned long)(n))
#define C_n(ctx, s, p, n) i18n_npgettext(ctx, s, p, (unsigned long)(n))

/* ------------------------------------------------------------ languages */
struct i18n_lang {
	const char *code;        /* "fr", "pt_BR": settings.ini language=, <code>.cat */
	const char *native;      /* "Français", in its own language (the picker) */
	const char *english;     /* "French" (logs, docs) */
	unsigned retro;          /* RETRO_LANGUAGE_* for GET_LANGUAGE */
};

/* Every language RetroStoneOS ships (English first). */
int i18n_lang_count(void);
const struct i18n_lang *i18n_lang_at(int i);
/* By code ("fr", "pt_BR"; "pt-BR" and case are accepted). NULL: unknown. */
const struct i18n_lang *i18n_lang_find(const char *code);

/* Where the <code>.cat files are (default /usr/share/rsos/locale). */
void i18n_set_dir(const char *dir);
const char *i18n_dir(void);

/*
 * Switches every lookup to that language. NULL, "" or "en" = English.
 * Returns 0, or -1 when the language is unknown or its catalog is missing
 * or invalid (English is used then, and the reason logged once to stderr).
 * Loading a catalog is one open + mmap; a language already loaded once
 * costs nothing.
 */
int i18n_set_language(const char *code);
/* The language in use ("en" when none). */
const char *i18n_language(void);
const struct i18n_lang *i18n_current(void);
/* RETRO_LANGUAGE_* of the language in use (English for unknown ones). */
unsigned i18n_retro_language(void);
/* Changes at every successful i18n_set_language() to another language:
 * code caching translated text compares it. */
unsigned i18n_generation(void);
/* Number of translated entries in the current catalog (0 for English). */
int i18n_catalog_entries(void);

/* ------------------------------------------------------- locale formats */
/*
 * The number, size and date conventions of the language in use, from its
 * catalog (contexts "number", "unit", "strftime": translators set them, see
 * docs/translating.md). English: "1.5 GB", "1,234", dates "%Y-%m-%d".
 */
/* "1,5 Go" (fr), "1.5 GB" (en): KB/MB/GB in binary units, one decimal
 * from 1 GB (sizes under 1 KB are shown as "1 KB"). */
void i18n_format_size(uint64_t bytes, char *out, size_t n);
/* v with `decimals` digits and the language's separators ("12 345,6"). */
void i18n_format_number(double v, int decimals, char *out, size_t n);
/* The language's short date / date + time (strftime), local time. */
void i18n_format_date(int64_t t, char *out, size_t n);
void i18n_format_datetime(int64_t t, char *out, size_t n);

/* ----------------------------------------------------------- utilities */
/* Uppercase for the language in use (Latin, Greek without accents,
 * Cyrillic; Turkish dotted/dotless i). CJK is unchanged. */
void i18n_upper(const char *in, char *buf, size_t n);

#endif
