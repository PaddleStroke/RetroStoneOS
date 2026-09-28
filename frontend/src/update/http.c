/*
 * http.c - see http.h.
 */
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef RSOS_UPDATE_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif
#endif

#define DEF_CA "/etc/ssl/certs/ca-certificates.crt"
#define HDR_MAX 16384

static void seterr(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t n, const char *fmt, ...)
{
	va_list ap;

	if (!err || !n)
		return;
	va_start(ap, fmt);
	vsnprintf(err, n, fmt, ap);
	va_end(ap);
}

bool http_have_tls(void)
{
#ifdef RSOS_UPDATE_TLS
	return true;
#else
	return false;
#endif
}

int http_parse_url(const char *url, char *scheme, size_t sn, char *host, size_t hn, int *port,
		   char *path, size_t pn)
{
	const char *p = strstr(url, "://"), *h, *e, *c;
	size_t l;

	if (!p || (size_t)(p - url) >= sn)
		return -1;
	memcpy(scheme, url, (size_t)(p - url));
	scheme[p - url] = 0;
	for (char *q = scheme; *q; q++)
		*q = (char)tolower((unsigned char)*q);
	if (strcmp(scheme, "http") && strcmp(scheme, "https"))
		return -1;
	h = p + 3;
	e = h + strcspn(h, "/?#");
	if (memchr(h, '@', (size_t)(e - h)) || memchr(h, '[', (size_t)(e - h)))
		return -1;                /* no credentials, no IPv6 literals */
	c = memchr(h, ':', (size_t)(e - h));
	l = (size_t)((c ? c : e) - h);
	if (!l || l >= hn)
		return -1;
	memcpy(host, h, l);
	host[l] = 0;
	for (size_t i = 0; i < l; i++)
		if (!(isalnum((unsigned char)host[i]) || host[i] == '.' || host[i] == '-' || host[i] == '_'))
			return -1;
	*port = !strcmp(scheme, "https") ? 443 : 80;
	if (c) {
		char *end;
		long v = strtol(c + 1, &end, 10);

		if (end != e || v <= 0 || v > 65535)
			return -1;
		*port = (int)v;
	}
	if (!*e || *e == '#') {
		if (pn < 2)
			return -1;
		strcpy(path, "/");
	} else {
		const char *hash = strchr(e, '#');

		l = hash ? (size_t)(hash - e) : strlen(e);
		if (l + 2 > pn)
			return -1;
		if (*e == '?') {
			path[0] = '/';
			memcpy(path + 1, e, l);
			path[l + 1] = 0;
		} else {
			memcpy(path, e, l);
			path[l] = 0;
		}
	}
	for (const char *q = path; *q; q++)
		if ((unsigned char)*q <= ' ' || *q == 0x7f)
			return -1;            /* no spaces or control characters in the request line */
	return 0;
}

/* ------------------------------------------------------------ transport */
struct conn {
	int fd;
	int timeout_ms;
	volatile int *cancel;
	bool tls;
#ifdef RSOS_UPDATE_TLS
	mbedtls_ssl_context ssl;
	bool ssl_init;
#endif
};

static bool cancelled(const struct conn *c)
{
	return c->cancel && *c->cancel;
}

/* Waits for fd readiness in slices (cancellation). 1 ready, 0 timeout, -1 error, -2 cancelled */
static int wait_fd(struct conn *c, short ev, int timeout_ms)
{
	int left = timeout_ms;

	while (left > 0) {
		struct pollfd pfd = { c->fd, ev, 0 };
		int slice = left < 250 ? left : 250;
		int r = poll(&pfd, 1, slice);

		if (cancelled(c))
			return -2;
		if (r > 0)
			return 1;
		if (r < 0 && errno != EINTR)
			return -1;
		if (r == 0)
			left -= slice;
	}
	return 0;
}

static int sock_connect(struct conn *c, const char *host, int port, char *err, size_t errlen)
{
	struct addrinfo hints, *res = NULL, *a;
	char ps[8];
	int r, last = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(ps, sizeof(ps), "%d", port);
	r = getaddrinfo(host, ps, &hints, &res);
	if (r) {
		seterr(err, errlen, "cannot resolve %s: %s", host, gai_strerror(r));
		return -1;
	}
	for (a = res; a; a = a->ai_next) {
		int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
		int w;
		socklen_t sl = sizeof(last);

		if (fd < 0) {
			last = errno;
			continue;
		}
		c->fd = fd;
		if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
			break;
		if (errno != EINPROGRESS) {
			last = errno;
			close(fd);
			c->fd = -1;
			continue;
		}
		w = wait_fd(c, POLLOUT, c->timeout_ms);
		if (w == -2) {
			close(fd);
			c->fd = -1;
			freeaddrinfo(res);
			return -2;
		}
		if (w == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &last, &sl) == 0 && last == 0)
			break;
		if (w == 0)
			last = ETIMEDOUT;
		close(fd);
		c->fd = -1;
	}
	freeaddrinfo(res);
	if (c->fd < 0) {
		seterr(err, errlen, "cannot connect to %s:%d: %s", host, port, strerror(last ? last : ECONNREFUSED));
		return -1;
	}
	return 0;
}

/* plain socket I/O: >0 bytes, 0 EOF, -1 error, -2 cancelled, -3 timeout */
static int sock_read(struct conn *c, void *buf, size_t n)
{
	for (;;) {
		ssize_t r = recv(c->fd, buf, n, 0);
		int w;

		if (r >= 0)
			return (int)r;
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		w = wait_fd(c, POLLIN, c->timeout_ms);
		if (w == 0)
			return -3;
		if (w < 0)
			return w;
	}
}

static int sock_write(struct conn *c, const void *buf, size_t n)
{
	for (;;) {
		ssize_t r = send(c->fd, buf, n, MSG_NOSIGNAL);
		int w;

		if (r >= 0)
			return (int)r;
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return -1;
		w = wait_fd(c, POLLOUT, c->timeout_ms);
		if (w == 0)
			return -3;
		if (w < 0)
			return w;
	}
}

#ifdef RSOS_UPDATE_TLS
struct tls_global {
	bool init, ok;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt ca;
	mbedtls_ssl_config conf;
	char ca_file[256];
};
static struct tls_global g_tls;

static int bio_send(void *ctx, const unsigned char *buf, size_t n)
{
	int r = sock_write(ctx, buf, n);

	if (r >= 0)
		return r;
	return r == -3 ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t n)
{
	int r = sock_read(ctx, buf, n);

	if (r >= 0)
		return r;
	return r == -3 ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
}

static int tls_global_init(const char *ca_file, char *err, size_t errlen)
{
	int r;

	if (g_tls.init)
		return g_tls.ok && !strcmp(g_tls.ca_file, ca_file) ? 0 : -1;
	g_tls.init = true;
	snprintf(g_tls.ca_file, sizeof(g_tls.ca_file), "%s", ca_file);
#if defined(MBEDTLS_PSA_CRYPTO_C)
	if (psa_crypto_init() != PSA_SUCCESS) {
		seterr(err, errlen, "TLS: crypto init failed");
		return -1;
	}
#endif
	mbedtls_entropy_init(&g_tls.entropy);
	mbedtls_ctr_drbg_init(&g_tls.drbg);
	mbedtls_x509_crt_init(&g_tls.ca);
	mbedtls_ssl_config_init(&g_tls.conf);
	r = mbedtls_ctr_drbg_seed(&g_tls.drbg, mbedtls_entropy_func, &g_tls.entropy,
				  (const unsigned char *)"rsos-update", 11);
	if (r) {
		seterr(err, errlen, "TLS: random generator: -0x%04x", -r);
		return -1;
	}
	r = mbedtls_x509_crt_parse_file(&g_tls.ca, ca_file);
	if (r < 0) {
		seterr(err, errlen, "TLS: cannot read the certificates in %s (-0x%04x)", ca_file, -r);
		return -1;
	}
	r = mbedtls_ssl_config_defaults(&g_tls.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT);
	if (r) {
		seterr(err, errlen, "TLS: config: -0x%04x", -r);
		return -1;
	}
	mbedtls_ssl_conf_authmode(&g_tls.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&g_tls.conf, &g_tls.ca, NULL);
	mbedtls_ssl_conf_rng(&g_tls.conf, mbedtls_ctr_drbg_random, &g_tls.drbg);
	g_tls.ok = true;
	return 0;
}

static enum rsu_err tls_start(struct conn *c, const char *host, const char *ca_file, char *err, size_t errlen)
{
	int r;

	if (tls_global_init(ca_file, err, errlen) < 0)
		return RSU_E_TLS;
	mbedtls_ssl_init(&c->ssl);
	c->ssl_init = true;
	c->tls = true;
	r = mbedtls_ssl_setup(&c->ssl, &g_tls.conf);
	if (!r)
		r = mbedtls_ssl_set_hostname(&c->ssl, host);
	if (r) {
		seterr(err, errlen, "TLS setup: -0x%04x", -r);
		return RSU_E_TLS;
	}
	mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		uint32_t flags;
		char vb[256];

		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (cancelled(c))
			return RSU_E_CANCELLED;
		flags = mbedtls_ssl_get_verify_result(&c->ssl);
		if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != (uint32_t)-1 && flags) {
			vb[0] = 0;
			mbedtls_x509_crt_verify_info(vb, sizeof(vb), "", flags);
			for (char *p = vb; *p; p++)
				if (*p == '\n')
					*p = ' ';
			for (size_t l = strlen(vb); l && vb[l - 1] == ' ';)
				vb[--l] = 0;
			seterr(err, errlen, "certificate of %s not accepted: %s", host, vb);
			if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
				return RSU_E_CLOCK;
			return RSU_E_TLS;
		}
		if (r == MBEDTLS_ERR_SSL_TIMEOUT) {
			seterr(err, errlen, "TLS handshake with %s timed out", host);
			return RSU_E_NETWORK;
		}
		mbedtls_strerror(r, vb, sizeof(vb));
		seterr(err, errlen, "TLS handshake with %s failed: %s (-0x%04x)", host, vb, -r);
		return RSU_E_TLS;
	}
	return RSU_OK;
}
#endif

static int conn_read(struct conn *c, void *buf, size_t n)
{
#ifdef RSOS_UPDATE_TLS
	if (c->tls) {
		for (;;) {
			int r = mbedtls_ssl_read(&c->ssl, buf, n);

			if (r >= 0)
				return r;
			if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
			if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
				continue;
#endif
			if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
				return 0;
			if (cancelled(c))
				return -2;
			return r == MBEDTLS_ERR_SSL_TIMEOUT ? -3 : -1;
		}
	}
#endif
	return sock_read(c, buf, n);
}

static int conn_write_all(struct conn *c, const void *buf, size_t n)
{
	const unsigned char *p = buf;

	while (n) {
		int r;
#ifdef RSOS_UPDATE_TLS
		if (c->tls) {
			r = mbedtls_ssl_write(&c->ssl, p, n);
			if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			if (r < 0)
				return cancelled(c) ? -2 : -1;
		} else
#endif
		{
			r = sock_write(c, p, n);
			if (r < 0)
				return r;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static void conn_close(struct conn *c)
{
#ifdef RSOS_UPDATE_TLS
	if (c->ssl_init) {
		mbedtls_ssl_free(&c->ssl);
		c->ssl_init = false;
	}
#endif
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->tls = false;
}

/* ---------------------------------------------------------------- request */
struct rbuf {
	char b[HDR_MAX + 1];
	size_t len, pos;             /* buffered body bytes: b[pos..len) */
};

static enum rsu_err io_err(int r, const char *what, char *err, size_t errlen)
{
	if (r == -2)
		return RSU_E_CANCELLED;
	seterr(err, errlen, "%s: %s", what, r == -3 ? "timed out" : r == 0 ? "connection closed" : "connection lost");
	return RSU_E_NETWORK;
}

/* fills rb until the end of the headers; returns the header length, or <0 */
static int read_headers(struct conn *c, struct rbuf *rb, char *err, size_t errlen, enum rsu_err *e)
{
	rb->len = rb->pos = 0;
	for (;;) {
		char *end;
		int r;

		rb->b[rb->len] = 0;
		end = strstr(rb->b, "\r\n\r\n");
		if (end) {
			rb->pos = (size_t)(end - rb->b) + 4;
			return (int)rb->pos;
		}
		if (rb->len >= HDR_MAX) {
			seterr(err, errlen, "response headers too large");
			*e = RSU_E_HTTP;
			return -1;
		}
		r = conn_read(c, rb->b + rb->len, HDR_MAX - rb->len);
		if (r <= 0) {
			*e = io_err(r, "reading the response", err, errlen);
			return -1;
		}
		rb->len += (size_t)r;
	}
}

/* header value (case-insensitive name) from the header block, "" if none */
static void header(const char *hdrs, const char *name, char *out, size_t n)
{
	size_t nl = strlen(name);
	const char *p = strstr(hdrs, "\r\n");

	out[0] = 0;
	while (p && p[2] && !(p[2] == '\r' && p[3] == '\n')) {
		const char *line = p + 2, *e = strstr(line, "\r\n");

		if (!e)
			break;
		if ((size_t)(e - line) > nl && !strncasecmp(line, name, nl) && line[nl] == ':') {
			const char *v = line + nl + 1;
			size_t l;

			while (*v == ' ' || *v == '\t')
				v++;
			l = (size_t)(e - v);
			while (l && (v[l - 1] == ' ' || v[l - 1] == '\t'))
				l--;
			if (l >= n)
				l = n - 1;
			memcpy(out, v, l);
			out[l] = 0;
			return;
		}
		p = e;
	}
}

/* next body bytes: buffered first, then the connection */
static int body_read(struct conn *c, struct rbuf *rb, void *buf, size_t n)
{
	if (rb->pos < rb->len) {
		size_t k = rb->len - rb->pos < n ? rb->len - rb->pos : n;

		memcpy(buf, rb->b + rb->pos, k);
		rb->pos += k;
		return (int)k;
	}
	return conn_read(c, buf, n);
}

/* one body byte: 0-255, or the conn_read() code (0 EOF, <0 error) - 1000 */
static int body_getc(struct conn *c, struct rbuf *rb)
{
	unsigned char ch;
	int r = body_read(c, rb, &ch, 1);

	return r == 1 ? ch : r - 1000;
}

/* a chunk-size line (hex, extensions ignored); -1 on error (*rc: conn code) */
static int64_t chunk_size(struct conn *c, struct rbuf *rb, int *rc)
{
	char line[128];
	size_t n = 0;
	int ch;
	char *e;
	long long v;

	*rc = 1;
	while ((ch = body_getc(c, rb)) >= 0 && ch != '\n')
		if (n + 1 < sizeof(line))
			line[n++] = (char)ch;
	if (ch < 0) {
		*rc = ch + 1000;
		return -1;
	}
	line[n] = 0;
	errno = 0;
	v = strtoll(line, &e, 16);
	if (e == line || errno || v < 0)
		return -1;
	return v;
}

static enum rsu_err read_body(struct conn *c, struct rbuf *rb, const struct http_resp *resp, bool chunked,
			      http_data_fn data, void *user, char *err, size_t errlen)
{
	static char buf[65536];
	int64_t left = resp->length;

	if (chunked) {
		for (;;) {
			int rc;
			int64_t sz = chunk_size(c, rb, &rc);

			if (sz < 0) {
				if (rc <= 0)
					return io_err(rc, "reading the body", err, errlen);
				seterr(err, errlen, "bad chunked body");
				return RSU_E_NETWORK;
			}
			if (sz == 0)
				return RSU_OK;          /* trailers are not read (Connection: close) */
			while (sz > 0) {
				int r = body_read(c, rb, buf, (size_t)(sz < (int64_t)sizeof(buf) ? sz : (int64_t)sizeof(buf)));

				if (r <= 0)
					return io_err(r, "reading the body", err, errlen);
				if (data(buf, (size_t)r, user) < 0)
					return RSU_E_CANCELLED;
				sz -= r;
			}
			/* the CRLF after the chunk */
			if (body_getc(c, rb) != '\r' || body_getc(c, rb) != '\n') {
				seterr(err, errlen, "bad chunked body");
				return RSU_E_NETWORK;
			}
		}
	}
	for (;;) {
		size_t want = sizeof(buf);
		int r;

		if (left >= 0 && (int64_t)want > left)
			want = (size_t)left;
		if (left == 0)
			return RSU_OK;
		r = body_read(c, rb, buf, want);
		if (r == 0 && left < 0)
			return RSU_OK;          /* until the connection closes */
		if (r <= 0)
			return io_err(r, "reading the body", err, errlen);
		if (data(buf, (size_t)r, user) < 0)
			return RSU_E_CANCELLED;
		if (left > 0)
			left -= r;
	}
}

/* Location relative to the request URL */
static int resolve(const char *base, const char *loc, char *out, size_t n)
{
	if (strstr(loc, "://")) {
		if ((size_t)snprintf(out, n, "%s", loc) >= n)
			return -1;
		return 0;
	}
	if (loc[0] == '/' && loc[1] != '/') {
		const char *p = strstr(base, "://");
		const char *e = p ? p + 3 + strcspn(p + 3, "/?#") : NULL;

		if (!e || (size_t)snprintf(out, n, "%.*s%s", (int)(e - base), base, loc) >= n)
			return -1;
		return 0;
	}
	return -1;
}

enum rsu_err http_get(const struct http_opts *o, const char *url_in, int64_t from, int64_t to,
		      http_head_fn head, http_data_fn data, void *user, struct http_resp *resp,
		      char *err, size_t errlen)
{
	struct http_opts def = { 0 };
	char url[2048];
	int redirects = 0;
	static struct rbuf rb;

	if (!o)
		o = &def;
	memset(resp, 0, sizeof(*resp));
	if ((size_t)snprintf(url, sizeof(url), "%s", url_in) >= sizeof(url)) {
		seterr(err, errlen, "URL too long");
		return RSU_E_HTTP;
	}
	for (;;) {
		char scheme[8], host[256], path[2048], req[4096 + 512], hv[512], loc[2048];
		int port, n, status = 0;
		struct conn c = { .fd = -1 };
		enum rsu_err e = RSU_OK;
		bool chunked;

		c.timeout_ms = o->timeout_ms > 0 ? o->timeout_ms : 30000;
		c.cancel = o->cancel;
		if (http_parse_url(url, scheme, sizeof(scheme), host, sizeof(host), &port, path, sizeof(path)) < 0) {
			seterr(err, errlen, "bad URL: %s", url);
			return RSU_E_HTTP;
		}
		if (!strcmp(scheme, "http") && !o->allow_http) {
			seterr(err, errlen, "plain http:// refused: %s", url);
			return RSU_E_TLS;
		}
#ifndef RSOS_UPDATE_TLS
		if (!strcmp(scheme, "https")) {
			seterr(err, errlen, "this build has no HTTPS support");
			return RSU_E_NOTLS;
		}
#endif
		n = sock_connect(&c, host, port, err, errlen);
		if (n == -2)
			return RSU_E_CANCELLED;
		if (n < 0)
			return RSU_E_NETWORK;
#ifdef RSOS_UPDATE_TLS
		if (!strcmp(scheme, "https")) {
			e = tls_start(&c, host, o->ca_file ? o->ca_file : DEF_CA, err, errlen);
			if (e) {
				conn_close(&c);
				return e;
			}
		}
#endif
		n = snprintf(req, sizeof(req),
			     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\n"
			     "Accept-Encoding: identity\r\n",
			     path, host, o->user_agent ? o->user_agent : "RetroStoneOS-updater");
		if (o->accept)
			n += snprintf(req + n, sizeof(req) - (size_t)n, "Accept: %s\r\n", o->accept);
		if (to >= 0)
			n += snprintf(req + n, sizeof(req) - (size_t)n, "Range: bytes=%" PRId64 "-%" PRId64 "\r\n",
				      from > 0 ? from : 0, to);
		else if (from > 0)
			n += snprintf(req + n, sizeof(req) - (size_t)n, "Range: bytes=%" PRId64 "-\r\n", from);
		n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
		if (n <= 0 || (size_t)n >= sizeof(req)) {
			conn_close(&c);
			seterr(err, errlen, "request too long");
			return RSU_E_HTTP;
		}
		if ((n = conn_write_all(&c, req, (size_t)n)) < 0) {
			conn_close(&c);
			return io_err(n, "sending the request", err, errlen);
		}
		if (read_headers(&c, &rb, err, errlen, &e) < 0) {
			conn_close(&c);
			return e;
		}
		if (sscanf(rb.b, "HTTP/1.%*d %d", &status) != 1 || status < 100 || status > 599) {
			conn_close(&c);
			seterr(err, errlen, "not an HTTP answer from %s", host);
			return RSU_E_HTTP;
		}
		{
			/* the header block alone, NUL-terminated, for header() */
			char saved = rb.b[rb.pos - 2];

			rb.b[rb.pos - 2] = 0;
			if (status >= 300 && status < 400)
				header(rb.b, "Location", loc, sizeof(loc));
			else
				loc[0] = 0;
			resp->status = status;
			resp->length = -1;
			resp->total = -1;
			resp->range_start = 0;
			header(rb.b, "Content-Length", hv, sizeof(hv));
			if (hv[0])
				resp->length = strtoll(hv, NULL, 10);
			header(rb.b, "Transfer-Encoding", hv, sizeof(hv));
			chunked = strcasestr(hv, "chunked") != NULL;
			if (chunked)
				resp->length = -1;
			header(rb.b, "Content-Range", hv, sizeof(hv));
			if (status == 206 && hv[0]) {
				long long a = -1, b2 = -1, t = -1;

				if (sscanf(hv, "bytes %lld-%lld/%lld", &a, &b2, &t) >= 2) {
					resp->range_start = a;
					resp->total = t;
				}
			} else if (status == 200) {
				resp->total = resp->length;
			}
			rb.b[rb.pos - 2] = saved;
		}
		snprintf(resp->url, sizeof(resp->url), "%s", url);
		if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && loc[0]) {
			char next[2048];

			conn_close(&c);
			if (++redirects > (o->max_redirects > 0 ? o->max_redirects : 5)) {
				seterr(err, errlen, "too many redirects");
				return RSU_E_HTTP;
			}
			if (resolve(url, loc, next, sizeof(next)) < 0) {
				seterr(err, errlen, "bad redirect to %.200s", loc);
				return RSU_E_HTTP;
			}
			if (!strncmp(url, "https:", 6) && strncmp(next, "https:", 6)) {
				seterr(err, errlen, "redirect from https to %.200s refused", next);
				return RSU_E_TLS;
			}
			snprintf(url, sizeof(url), "%s", next);
			continue;
		}
		if (head && head(resp, user) < 0) {
			conn_close(&c);
			return RSU_E_CANCELLED;
		}
		if (status < 200 || status > 299) {
			conn_close(&c);
			seterr(err, errlen, "HTTP %d from %s", status, host);
			return status == 404 ? RSU_E_NOTFOUND : RSU_E_HTTP;
		}
		e = data ? read_body(&c, &rb, resp, chunked, data, user, err, errlen) : RSU_OK;
		conn_close(&c);
		return e;
	}
}
