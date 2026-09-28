/*
 * i18n_cat.h - the compiled catalog (<lang>.cat), written by rsos-i18n
 * (src/tools/rsos-i18n.c) and mmap()ed by i18n.c. Native byte order (the
 * build host and the targets are little-endian; `endian` catches a mix-up).
 *
 *   header | buckets: uint32[nbuckets] | entries: struct i18n_cat_entry[count] | strings
 *
 * An entry's key is the msgid, or "context\004msgid" (the gettext
 * convention); its hash is FNV-1a 32 of the key. buckets[hash & (nbuckets-1)]
 * is the first entry index + 1 of that chain (0 = empty), entry.next the
 * next one. Values are the translation, NUL-terminated; a plural entry holds
 * nforms translations one after the other, each NUL-terminated. Every
 * offset is from the start of the file and checked at load.
 */
#ifndef RSOS_I18N_CAT_H
#define RSOS_I18N_CAT_H

#include <stdint.h>

#define I18N_CAT_MAGIC   "RSOSCAT1"
#define I18N_CAT_ENDIAN  0x01020304u
#define I18N_CAT_MAX_FORMS 6

struct i18n_cat_header {
	char magic[8];
	uint32_t endian;
	uint32_t size;          /* the whole file */
	uint32_t count;         /* entries */
	uint32_t nbuckets;      /* a power of two */
	uint32_t buckets;       /* offset of uint32_t[nbuckets] */
	uint32_t entries;       /* offset of struct i18n_cat_entry[count] */
	uint32_t plural;        /* offset of the plural expression (C syntax, NUL-terminated) */
	uint32_t nplurals;      /* 1..I18N_CAT_MAX_FORMS */
	uint32_t lang;          /* offset of the language code, NUL-terminated */
	uint32_t reserved[3];
};

struct i18n_cat_entry {
	uint32_t hash;
	uint32_t next;          /* next entry index + 1 in the chain, 0 = end */
	uint32_t key, key_len;  /* key_len excludes the NUL */
	uint32_t val, val_len;  /* all forms, their NULs included */
	uint32_t nforms;        /* 1, or nplurals for a plural entry */
};

static inline uint32_t i18n_hash_step(uint32_t h, const char *s, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		h ^= (unsigned char)s[i];
		h *= 16777619u;
	}
	return h;
}

#define I18N_HASH_INIT 2166136261u

/* Evaluates a Plural-Forms expression for n. Returns the form index, or -1
 * if the expression is invalid. (i18n.c; also used by rsos-i18n.) */
long i18n_plural_eval(const char *expr, unsigned long n);

#endif
