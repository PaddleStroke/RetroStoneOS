/*
 * json.h - a small JSON reader (RFC 8259) for the GitHub releases answer:
 * a tree of nodes, strings unescaped to UTF-8, bounded nesting. Not for
 * writing JSON.
 */
#ifndef RSOS_UPDATE_JSON_H
#define RSOS_UPDATE_JSON_H

#include <stdbool.h>
#include <stddef.h>

enum json_type { JSON_NULL = 0, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };

struct json {
	enum json_type type;
	char *key;                    /* member name inside an object */
	char *str;                    /* JSON_STR */
	double num;                   /* JSON_NUM */
	bool b;                       /* JSON_BOOL */
	struct json *child;           /* JSON_ARR / JSON_OBJ: first element */
	struct json *next;            /* next sibling */
};

/* NULL on a syntax error (err set). Free with json_free(). */
struct json *json_parse(const char *text, size_t len, char *err, size_t errlen);
void json_free(struct json *j);

/* Object member lookups; NULL / the default when missing or of another type. */
const struct json *json_get(const struct json *obj, const char *key);
const char *json_str(const struct json *obj, const char *key);
double json_num(const struct json *obj, const char *key, double def);
bool json_bool(const struct json *obj, const char *key, bool def);

#endif
