/*
 * http.h - a small HTTP/1.1 GET client for the updater: https:// through
 * mbedTLS (certificate chain checked against the system CA bundle, host
 * name checked), plain http:// only when allowed (tests), redirects,
 * Range requests (resumed downloads), chunked or sized bodies, timeouts and
 * cancellation. One request per connection (Connection: close).
 *
 * Built with RSOS_UPDATE_TLS (mbedTLS) on the device; without it https://
 * URLs fail with RSU_E_NOTLS.
 */
#ifndef RSOS_UPDATE_HTTP_H
#define RSOS_UPDATE_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rsu.h"

struct http_opts {
	int timeout_ms;               /* connect / each read (default 30000) */
	int max_redirects;            /* default 5 */
	bool allow_http;              /* plain http:// (tests) */
	const char *ca_file;          /* PEM bundle (default /etc/ssl/certs/ca-certificates.crt) */
	const char *user_agent;       /* default "RetroStoneOS-updater" */
	const char *accept;           /* Accept header, NULL: none */
	volatile int *cancel;         /* checked while waiting: non-zero stops */
};

struct http_resp {
	int status;
	int64_t length;               /* body bytes announced, -1 unknown */
	int64_t range_start;          /* 206: offset of the first byte, else 0 */
	int64_t total;                /* the whole resource's size when known, else -1 */
	char url[2048];               /* the final URL (after redirects) */
};

/* The final answer's headers (any status); < 0 aborts (RSU_E_CANCELLED). */
typedef int (*http_head_fn)(const struct http_resp *r, void *user);
/* Body bytes of a 2xx answer; < 0 aborts. */
typedef int (*http_data_fn)(const void *data, size_t n, void *user);

/*
 * GET url, with "Range: bytes=from-" (from > 0, to < 0) or "bytes=from-to"
 * (to >= 0); no Range when from <= 0 and to < 0. A 2xx answer's body goes
 * to data(); other answers are not read (head() still sees the status).
 * Returns RSU_OK, or RSU_E_NETWORK / RSU_E_TLS / RSU_E_CLOCK /
 * RSU_E_HTTP (status >= 400, err says which) / RSU_E_CANCELLED / RSU_E_NOTLS
 * with a message in err.
 */
enum rsu_err http_get(const struct http_opts *o, const char *url, int64_t from, int64_t to,
		      http_head_fn head, http_data_fn data, void *user, struct http_resp *resp,
		      char *err, size_t errlen);

/* Splits "scheme://host[:port]/path"; 0 or -1. */
int http_parse_url(const char *url, char *scheme, size_t sn, char *host, size_t hn, int *port,
		   char *path, size_t pn);

/* True when the build has HTTPS. */
bool http_have_tls(void);

#endif
