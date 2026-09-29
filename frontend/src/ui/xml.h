/*
 * xml.h - a tiny, forgiving, in-place XML DOM parser.
 *
 * Enough for EmulationStation theme.xml and gamelist.xml files, which in
 * the wild contain comments before the <?xml?> declaration, Latin-1 bytes,
 * stray '&' characters and mismatched tags. It never fails on bad input:
 * it recovers and keeps what it could parse.
 *
 * Supported: elements, attributes (single/double quotes), text, CDATA,
 * comments, processing instructions and DOCTYPE (both skipped), the five
 * predefined entities and numeric character references (encoded as UTF-8).
 * Limits (broken files): 256 levels of nesting (a deeper element is kept
 * empty) and 64 KiB of text per element; parsing time is linear.
 */
#ifndef RSOS_UI_XML_H
#define RSOS_UI_XML_H

#include <stddef.h>

#include "util.h"

struct xml_attr {
	const char *name;
	const char *value;
	struct xml_attr *next;
};

struct xml_node {
	const char *name;
	const char *text;         /* concatenated, trimmed text content ("" if none) */
	struct xml_attr *attrs;
	struct xml_node *parent, *child, *last_child, *next;
	int line;
};

struct xml_doc {
	struct arena arena;
	char *buf;                /* owned copy of the input, parsed in place */
	struct xml_node *root;    /* first top-level element, NULL if none */
	struct xml_node top;      /* synthetic parent of all top-level elements */
};

/* Parses a copy of data. Returns NULL only on allocation failure. */
struct xml_doc *xml_parse(const char *data, size_t len);
/* Loads and parses a file. NULL if the file cannot be read. */
struct xml_doc *xml_load(const char *path);
void xml_free(struct xml_doc *doc);

const char *xml_attr(const struct xml_node *n, const char *name);
struct xml_node *xml_child(const struct xml_node *n, const char *name);
/* Text of the named child, or NULL. */
const char *xml_child_text(const struct xml_node *n, const char *name);

#endif
