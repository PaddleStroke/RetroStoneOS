/*
 * webshare.c - "Transfer over network": a tiny HTTP/1.1 server for
 * drag-and-drop uploads from a browser on the same LAN.
 *
 * One embedded page (web/index.html, compiled in by transfer.mk) and a
 * small JSON API:
 *
 *   GET    /                          the page
 *   POST   /api/login                 body "pin=123456" -> session cookie
 *   GET    /api/info                  free space, systems and file counts
 *   GET    /api/list?target=&sys=     files of one folder (recursive)
 *   PUT    /api/upload?target=&sys=&path=&overwrite=&mtime=
 *                                     raw file body, streamed to disk
 *   DELETE /api/file?target=&sys=&path=
 *
 *   target: roms|saves|states (with sys=<canonical system id>), bios, themes
 *   path:   relative path inside the target folder, '/' separated,
 *           checked by transfer_relpath_check()
 *
 * Security model (docs/rom-transfer.md "Web share security"):
 *   - runs only while the user enabled it, and only when a network is up;
 *     binds while enabled, closes the socket when stopped or idle;
 *   - accepts clients from private / link-local / loopback IPv4 only;
 *   - a 6-digit PIN shown on the console screen, new at every start;
 *     5 wrong PINs lock that device out for 30 s, doubling up to 10 min;
 *     20 from any devices lock everybody out for 60 s (review F-L2);
 *   - after login, a random 128-bit session token in an HttpOnly,
 *     SameSite=Strict cookie (or the X-RSOS-Token header for scripts);
 *   - the login and state-changing requests also need "X-Requested-With: rsos", which a
 *     foreign web page cannot send without a CORS preflight we never allow
 *     (CSRF), and the Host header must be an IP literal or our own name
 *     (DNS rebinding);
 *   - paths never leave /data/<target>: names are validated, the target
 *     folder comes from a fixed table, and directories are opened one
 *     component at a time with O_NOFOLLOW;
 *   - uploads go to a hidden temp file, fsync, rename, fsync(dir): an
 *     aborted upload never leaves a half file that looks like a game.
 * It is plain HTTP: the PIN and files cross the LAN unencrypted, like SMB
 * guest shares on consoles and NAS boxes. Fine for a home LAN; the doc
 * tells users not to enable it on public WiFi.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "tr_internal.h"

extern const unsigned char webshare_index_html[];
extern const unsigned int webshare_index_html_len;

#define REQ_MAX 8192
#define BODY_BUF (256 * 1024)
#define MAX_CLIENTS 8
#define IO_TIMEOUT_S 30
#define HEAD_TIMEOUT_MS 10000    /* the request head, whole */
#define BODY_MIN_BPS 1024        /* uploads: average after the first minute */
#define UPLOAD_RESERVE (16ull << 20)
#define LIST_MAX 5000
#define WS_KEY_MAX (TRANSFER_PATH_MAX + 64 + 256 + 2)   /* "<chain>/<base>" */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t th;
	bool thread;             /* th must be joined */
	bool running;
	volatile int stopping;
	int lfd, pipe_r, pipe_w;
	int rootfd;
	char data_root[256];
	char hostname[64];
	uint16_t port;
	int idle_timeout_s;
	int max_clients;
	bool allow_public;
	char pin[12];
	char token[33];
	int client_fds[MAX_CLIENTS];
	int nclients;
	/* destinations being uploaded ("<target folder>/<rel>"): a second
	 * upload of the same path gets 409 busy (review F-H1) */
	char inflight[MAX_CLIENTS][WS_KEY_MAX];
	int busy;                /* authenticated requests being handled */
	/* PIN lockout (review F-L2): per peer address, so one LAN device only
	 * locks itself out; plus a global one against many devices, short and
	 * not doubling, so nobody can keep the owner out for long */
	struct peer_fail {
		uint32_t addr;       /* network order, 0 = free */
		int failures;
		int64_t lock_until, last;
	} peers[16];
	int failures;            /* global, since the last successful login */
	int64_t lock_until;      /* global */
	int64_t last_activity;
	uint64_t changed;        /* bit per system index */
	bool bios_changed, themes_changed;
	struct webshare_status st;
} S = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
	.lfd = -1, .pipe_r = -1, .pipe_w = -1, .rootfd = -1,
};

struct req {
	int fd;
	struct sockaddr_in peer;
	char buf[REQ_MAX + 1];
	size_t len, hdr_end;
	char *method, *path, *query;
	const char *host, *cookie, *xrw, *expect, *token_hdr, *te;
	int64_t clen;            /* -1 when absent */
	bool authed;
};

/* ------------------------------------------------------------ small I/O */

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static const char *reason(int code)
{
	switch (code) {
	case 100: return "Continue";
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 421: return "Misdirected Request";
	case 429: return "Too Many Requests";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	case 507: return "Insufficient Storage";
	}
	return "Error";
}

static void respond(struct req *r, int code, const char *ctype, const void *body, size_t len,
		    const char *extra)
{
	char h[1024];
	int n;

	n = snprintf(h, sizeof(h),
		     "HTTP/1.1 %d %s\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %zu\r\n"
		     "Connection: close\r\n"
		     "Cache-Control: no-store\r\n"
		     "X-Content-Type-Options: nosniff\r\n"
		     "X-Frame-Options: DENY\r\n"
		     "Referrer-Policy: no-referrer\r\n"
		     "%s\r\n",
		     code, reason(code), ctype, len, extra ? extra : "");
	if (n < 0 || (size_t)n >= sizeof(h))
		return;
	if (send_all(r->fd, h, (size_t)n) == 0 && len)
		send_all(r->fd, body, len);
}

static void respond_json(struct req *r, int code, const char *json)
{
	respond(r, code, "application/json", json, strlen(json), NULL);
}

static void respond_err(struct req *r, int code, const char *err)
{
	char b[160];

	snprintf(b, sizeof(b), "{\"error\":\"%s\"}", err);
	respond_json(r, code, b);
}

/* After an early error on a request with a body, let the client finish
 * sending (up to 2 s) so it reads our response instead of a TCP reset. */
static void linger_close(struct req *r)
{
	char tmp[16384];
	struct timeval tv = { .tv_sec = 2 };
	int64_t t0 = tr_now_ms();

	shutdown(r->fd, SHUT_WR);
	setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (tr_now_ms() - t0 < 2000 && recv(r->fd, tmp, sizeof(tmp), 0) > 0)
		;
}

/* ------------------------------------------------------------ parsing */

static bool is_private(uint32_t a)   /* host byte order */
{
	return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) ||
	       (a >> 16) == (192u << 8 | 168) || (a >> 16) == (169u << 8 | 254) ||
	       (a >> 22) == (100u << 2 | 1);   /* 100.64/10 (phone hotspots) */
}

static bool is_ipv4_literal(const char *s, size_t n)
{
	int dots = 0, digits = 0;

	for (size_t i = 0; i < n; i++) {
		if (s[i] == '.') {
			if (!digits)
				return false;
			dots++;
			digits = 0;
		} else if (s[i] >= '0' && s[i] <= '9') {
			if (++digits > 3)
				return false;
		} else {
			return false;
		}
	}
	return dots == 3 && digits;
}

/* DNS rebinding guard: the Host header must name us. */
static bool host_ok(const char *host)
{
	size_t n, hl;
	const char *colon;

	if (!host)
		return false;
	colon = strchr(host, ':');
	n = colon ? (size_t)(colon - host) : strlen(host);
	if (is_ipv4_literal(host, n))
		return true;
	if (n == 9 && !strncasecmp(host, "localhost", 9))
		return true;
	hl = strlen(S.hostname);
	if (n == hl && !strncasecmp(host, S.hostname, hl))
		return true;
	if (n == hl + 6 && !strncasecmp(host, S.hostname, hl) &&
	    !strncasecmp(host + hl, ".local", 6))
		return true;
	return false;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
	return s;
}

/* Reads the request head. 0, or an HTTP status to answer with, or -1 to
 * just close. */
static int read_head(struct req *r)
{
	char *line, *save = NULL;
	char *end;
	/* The whole head within HEAD_TIMEOUT_MS (review F-L1: a byte every 29 s
	 * kept a client slot for hours, before any authentication). */
	int64_t deadline = tr_now_ms() + HEAD_TIMEOUT_MS;

	for (;;) {
		ssize_t n;
		int64_t left;
		struct pollfd pf = { r->fd, POLLIN, 0 };

		r->buf[r->len] = 0;
		if ((end = strstr(r->buf, "\r\n\r\n")))
			break;
		if (r->len >= REQ_MAX)
			return 431;
		left = deadline - tr_now_ms();
		if (left <= 0 || S.stopping)
			return -1;
		n = poll(&pf, 1, (int)left);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		n = recv(r->fd, r->buf + r->len, REQ_MAX - r->len, 0);
		if (n <= 0)
			return -1;
		r->len += (size_t)n;
	}
	*end = 0;
	r->hdr_end = (size_t)(end - r->buf) + 4;
	r->clen = -1;

	line = strtok_r(r->buf, "\n", &save);
	if (!line)
		return 400;
	{
		char *sp1 = strchr(line, ' '), *sp2;

		if (!sp1)
			return 400;
		*sp1 = 0;
		sp2 = strchr(sp1 + 1, ' ');
		if (!sp2 || strncmp(sp2 + 1, "HTTP/1.", 7))
			return 400;
		*sp2 = 0;
		r->method = line;
		r->path = sp1 + 1;
		if (r->path[0] != '/')
			return 400;
		if ((r->query = strchr(r->path, '?')))
			*r->query++ = 0;
		else
			r->query = r->path + strlen(r->path);
	}
	while ((line = strtok_r(NULL, "\n", &save))) {
		char *c = strchr(line, ':'), *v;

		if (!c)
			continue;
		*c = 0;
		v = trim(c + 1);
		if (!strcasecmp(line, "Host"))
			r->host = v;
		else if (!strcasecmp(line, "Content-Length")) {
			char *e;

			r->clen = strtoll(v, &e, 10);
			if (*e || r->clen < 0)
				return 400;
		} else if (!strcasecmp(line, "Cookie"))
			r->cookie = v;
		else if (!strcasecmp(line, "X-Requested-With"))
			r->xrw = v;
		else if (!strcasecmp(line, "Expect"))
			r->expect = v;
		else if (!strcasecmp(line, "X-RSOS-Token"))
			r->token_hdr = v;
		else if (!strcasecmp(line, "Transfer-Encoding"))
			r->te = v;
	}
	return 0;
}

/* Gets a decoded query parameter. 0, -ENOENT, -EINVAL or -ENAMETOOLONG. */
static int get_param(const char *query, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = query;

	while (*p) {
		const char *amp = strchr(p, '&');
		size_t l = amp ? (size_t)(amp - p) : strlen(p);

		if (l > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			size_t vl = l - kl - 1;

			if (vl >= n)
				return -ENAMETOOLONG;
			memcpy(out, p + kl + 1, vl);
			out[vl] = 0;
			return tr_url_decode(out);
		}
		p += l;
		if (*p == '&')
			p++;
	}
	return -ENOENT;
}

static bool ct_equal(const char *a, const char *b, size_t n)
{
	unsigned char d = 0;

	for (size_t i = 0; i < n; i++)
		d |= (unsigned char)(a[i] ^ b[i]);
	return d == 0;
}

static bool check_auth(struct req *r)
{
	const char *t = NULL;

	if (r->token_hdr && strlen(r->token_hdr) == 32) {
		t = r->token_hdr;
	} else if (r->cookie) {
		const char *c = r->cookie;

		while ((c = strstr(c, "rsos_token=")) != NULL) {
			if (c == r->cookie || c[-1] == ' ' || c[-1] == ';') {
				t = c + 11;
				break;
			}
			c += 11;
		}
		if (t && strspn(t, "0123456789abcdef") != 32)
			t = NULL;
	}
	return t && ct_equal(t, S.token, 32);
}

/* ------------------------------------------------------------ targets */

/* Maps target + sys to a folder under data_root. Returns the system index
 * (>= 0), -2 for bios/themes, or -1 if invalid. */
static int target_dir(const char *target, const char *sys, char *out, size_t n)
{
	int si;

	if (!strcmp(target, "bios")) {
		tr_strlcpy(out, "bios", n);
		return -2;
	}
	if (!strcmp(target, "themes")) {
		tr_strlcpy(out, "themes", n);
		return -2;
	}
	if (strcmp(target, "roms") && strcmp(target, "saves") && strcmp(target, "states"))
		return -1;
	si = transfer_system_index(sys);
	if (si < 0)
		return -1;
	snprintf(out, n, "%s/%s", target, sys);
	return si;
}

static void mark_changed(const char *target, int si)
{
	pthread_mutex_lock(&S.lock);
	if (!strcmp(target, "roms") && si >= 0 && si < 64)
		S.changed |= 1ull << si;
	else if (!strcmp(target, "bios"))
		S.bios_changed = true;
	else if (!strcmp(target, "themes"))
		S.themes_changed = true;
	pthread_mutex_unlock(&S.lock);
}

/* Parses target/sys/path from the query. Fills dir (target folder) and
 * rel. Returns the system index or -2, or sends an error and returns -1. */
static int req_target(struct req *r, char *target, size_t tn, char *dir, size_t dn,
		      char *rel, size_t rn, bool need_path)
{
	char sys[TRANSFER_SYSID_MAX] = "";
	int si, e;

	if (get_param(r->query, "target", target, tn) < 0) {
		respond_err(r, 400, "bad_target");
		return -1;
	}
	get_param(r->query, "sys", sys, sizeof(sys));
	si = target_dir(target, sys, dir, dn);
	if (si == -1) {
		respond_err(r, 400, "bad_target");
		return -1;
	}
	if (need_path) {
		e = get_param(r->query, "path", rel, rn);
		if (e == 0)
			e = transfer_relpath_check(rel);
		if (e < 0) {
			respond_err(r, 400, "bad_name");
			return -1;
		}
	}
	return si;
}

/* ------------------------------------------------------------ handlers */

/* The peer's lockout entry (S.lock held): found, or a free / the least
 * recently used unlocked one taken over. A locked entry is never taken
 * over (review: 17 addresses reset each other's lock): with all of them
 * locked, NULL, and the newcomer waits like them (login_locked). */
static struct peer_fail *peer_entry(uint32_t addr, int64_t now)
{
	struct peer_fail *victim = NULL;

	for (size_t i = 0; i < sizeof(S.peers) / sizeof(S.peers[0]); i++) {
		struct peer_fail *p = &S.peers[i];

		if (p->addr == addr)
			return p;
		if (p->addr && p->lock_until > now)
			continue;
		if (!victim || !p->addr || (victim->addr && p->last < victim->last))
			victim = p;
	}
	if (victim) {
		memset(victim, 0, sizeof(*victim));
		victim->addr = addr;
	}
	return victim;
}

/* ms still locked for this peer (S.lock held), 0 = may try; p NULL (the
 * table full of locked peers): until the first of them is free */
static int64_t login_locked(struct peer_fail *p, int64_t now)
{
	int64_t l = S.lock_until;

	if (p) {
		if (p->lock_until > l)
			l = p->lock_until;
	} else {
		int64_t first = 0;

		for (size_t i = 0; i < sizeof(S.peers) / sizeof(S.peers[0]); i++)
			if (!first || S.peers[i].lock_until < first)
				first = S.peers[i].lock_until;
		if (first > l)
			l = first;
	}
	return l > now ? l - now : 0;
}

#define LOGIN_PEER_TRIES 5       /* then 30 s, doubling up to 10 min, per peer */
#define LOGIN_GLOBAL_TRIES 20    /* all peers: then 60 s for everyone */

static void h_login(struct req *r)
{
	char body[257], pin[16];
	size_t have = r->len - r->hdr_end;
	uint32_t addr = r->peer.sin_addr.s_addr ? r->peer.sin_addr.s_addr : 1;
	struct peer_fail *p;
	int64_t now, locked;
	bool ok;

	/* early answer while locked (the body is not even read) */
	pthread_mutex_lock(&S.lock);
	now = tr_now_ms();
	locked = login_locked(peer_entry(addr, now), now);
	pthread_mutex_unlock(&S.lock);
	if (locked) {
		snprintf(body, sizeof(body), "{\"error\":\"locked\",\"wait\":%d}", (int)((locked + 999) / 1000));
		respond_json(r, 429, body);
		return;
	}

	if (r->clen < 0 || r->clen > 256 || r->hdr_end + (size_t)r->clen > REQ_MAX) {
		respond_err(r, 400, "bad_request");
		return;
	}
	{
		/* the (tiny) body within the head's time budget too (F-L1) */
		int64_t deadline = tr_now_ms() + HEAD_TIMEOUT_MS;

		while (have < (size_t)r->clen) {
			struct pollfd pf = { r->fd, POLLIN, 0 };
			int64_t left = deadline - tr_now_ms();
			ssize_t n;

			if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
				return;
			n = recv(r->fd, r->buf + r->len, (size_t)r->clen - have, 0);
			if (n <= 0)
				return;
			r->len += (size_t)n;
			have += (size_t)n;
		}
	}
	memcpy(body, r->buf + r->hdr_end, (size_t)r->clen);
	body[r->clen] = 0;
	if (get_param(body, "pin", pin, sizeof(pin)) < 0)
		pin[0] = 0;

	/* The lock check and the verdict in one critical section (review F-L2:
	 * requests that all passed the early check each got a guess). */
	pthread_mutex_lock(&S.lock);
	now = tr_now_ms();
	p = peer_entry(addr, now);
	locked = login_locked(p, now);
	ok = !locked && strlen(pin) == strlen(S.pin) && ct_equal(pin, S.pin, strlen(S.pin));
	if (locked || !p) {
		/* no guess while locked (p is NULL only then) */
	} else if (ok) {
		p->last = now;
		p->failures = 0;
		S.failures = 0;
		S.st.sessions++;
		S.last_activity = now;
	} else {
		p->last = now;
		p->failures++;
		S.failures++;
		S.st.auth_failures++;
		if (p->failures >= LOGIN_PEER_TRIES) {
			int n = p->failures - LOGIN_PEER_TRIES;
			int64_t secs = 30ll << (n > 5 ? 5 : n);

			if (secs > 600)
				secs = 600;
			p->lock_until = now + secs * 1000;
		}
		if (S.failures >= LOGIN_GLOBAL_TRIES) {
			S.lock_until = now + 60 * 1000;
			S.failures = 0;
		}
	}
	pthread_mutex_unlock(&S.lock);
	if (locked) {
		snprintf(body, sizeof(body), "{\"error\":\"locked\",\"wait\":%d}", (int)((locked + 999) / 1000));
		respond_json(r, 429, body);
		return;
	}

	if (ok) {
		char cookie[128];

		snprintf(cookie, sizeof(cookie),
			 "Set-Cookie: rsos_token=%s; Path=/; HttpOnly; SameSite=Strict\r\n", S.token);
		respond(r, 200, "application/json", "{\"ok\":true}", 11, cookie);
	} else {
		respond_err(r, 403, "wrong_pin");
	}
}

static int count_entries(int dirfd, const char *rel)
{
	int fd = tr_open_dir_chain(dirfd, rel, false), n = 0;
	DIR *d;
	struct dirent *de;

	if (fd < 0)
		return 0;
	d = fdopendir(fd);
	if (!d) {
		close(fd);
		return 0;
	}
	while ((de = readdir(d)))
		if (!tr_is_junk(de->d_name) && strcasecmp(de->d_name, "gamelist.xml") &&
		    strcasecmp(de->d_name, "media"))
			n++;
	closedir(d);
	return n;
}

static void h_info(struct req *r)
{
	struct tr_buf b;
	struct statvfs v;
	uint64_t fr = 0, tot = 0;

	if (fstatvfs(S.rootfd, &v) == 0) {
		fr = (uint64_t)v.f_bavail * v.f_frsize;
		tot = (uint64_t)v.f_blocks * v.f_frsize;
	}
	tr_buf_init(&b);
	tr_buf_puts(&b, "{\"host\":");
	tr_buf_json_str(&b, S.hostname);
	tr_buf_printf(&b, ",\"free\":%llu,\"total\":%llu,\"systems\":[",
		      (unsigned long long)fr, (unsigned long long)tot);
	for (int i = 0; i < transfer_system_count(); i++) {
		char rel[64];

		snprintf(rel, sizeof(rel), "roms/%s", transfer_system_id(i));
		tr_buf_printf(&b, "%s{\"id\":\"%s\",\"name\":", i ? "," : "", transfer_system_id(i));
		tr_buf_json_str(&b, transfer_system_name(i));
		tr_buf_printf(&b, ",\"n\":%d}", count_entries(S.rootfd, rel));
	}
	tr_buf_printf(&b, "],\"bios\":%d,\"themes\":%d}", count_entries(S.rootfd, "bios"),
		      count_entries(S.rootfd, "themes"));
	if (b.oom)
		respond_err(r, 500, "oom");
	else
		respond_json(r, 200, b.p);
	tr_buf_free(&b);
}

static void list_dir(struct tr_buf *b, int dfd, const char *prefix, int depth, int *count)
{
	DIR *d;
	struct dirent *de;
	int fd = fcntl(dfd, F_DUPFD_CLOEXEC, 0);     /* review F-L7: CLOEXEC */

	if (fd < 0)
		return;
	d = fdopendir(fd);
	if (!d) {
		close(fd);
		return;
	}
	while ((de = readdir(d)) && *count < LIST_MAX) {
		struct stat st;
		char rel[TRANSFER_PATH_MAX];

		if (tr_is_junk(de->d_name) ||
		    fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
			continue;
		if (tr_snprintf(rel, sizeof(rel), "%s%s%s", prefix, prefix[0] ? "/" : "",
				de->d_name) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 4) {
				int sub = openat(dirfd(d), de->d_name,
						 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

				if (sub >= 0) {
					list_dir(b, sub, rel, depth + 1, count);
					close(sub);
				}
			}
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;
		tr_buf_puts(b, *count ? ",{\"p\":" : "{\"p\":");
		tr_buf_json_str(b, rel);
		tr_buf_printf(b, ",\"s\":%llu,\"t\":%lld}", (unsigned long long)st.st_size,
			      (long long)st.st_mtim.tv_sec);
		(*count)++;
	}
	closedir(d);
}

static void h_list(struct req *r)
{
	char target[16], dir[64], dummy[4];
	struct tr_buf b;
	int count = 0, dfd;

	if (req_target(r, target, sizeof(target), dir, sizeof(dir), dummy, sizeof(dummy), false) == -1)
		return;
	tr_buf_init(&b);
	tr_buf_puts(&b, "{\"files\":[");
	dfd = tr_open_dir_chain(S.rootfd, dir, false);
	if (dfd >= 0) {
		list_dir(&b, dfd, "", 0, &count);
		close(dfd);
	}
	tr_buf_printf(&b, "],\"truncated\":%s,\"free\":%llu}", count >= LIST_MAX ? "true" : "false",
		      (unsigned long long)tr_free_bytes_fd(S.rootfd));
	if (b.oom)
		respond_err(r, 500, "oom");
	else
		respond_json(r, 200, b.p);
	tr_buf_free(&b);
}

static void set_current(const char *name, uint64_t done, uint64_t total)
{
	pthread_mutex_lock(&S.lock);
	tr_strlcpy(S.st.current, name, sizeof(S.st.current));
	S.st.cur_done = done;
	S.st.cur_total = total;
	S.last_activity = tr_now_ms();
	pthread_mutex_unlock(&S.lock);
}

/* Claims a destination for one upload. Case-insensitive: /data is exFAT,
 * where "X.sfc" and "x.sfc" are the same file. */
static bool inflight_claim(const char *key)
{
	int free_slot = -1;

	pthread_mutex_lock(&S.lock);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (!S.inflight[i][0]) {
			if (free_slot < 0)
				free_slot = i;
		} else if (!strcasecmp(S.inflight[i], key)) {
			pthread_mutex_unlock(&S.lock);
			return false;
		}
	}
	if (free_slot >= 0)
		tr_strlcpy(S.inflight[free_slot], key, sizeof(S.inflight[0]));
	pthread_mutex_unlock(&S.lock);
	return free_slot >= 0;
}

static void inflight_release(const char *key)
{
	pthread_mutex_lock(&S.lock);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (S.inflight[i][0] && !strcasecmp(S.inflight[i], key)) {
			S.inflight[i][0] = 0;
			break;
		}
	pthread_mutex_unlock(&S.lock);
}

static void h_upload(struct req *r)
{
	char target[16], dir[64], rel[TRANSFER_PATH_MAX], chain[TRANSFER_PATH_MAX + 64];
	char sub[TRANSFER_PATH_MAX], base[256], tmp[32], key[WS_KEY_MAX];
	bool overwrite = false, existed = false, claimed = false;
	int64_t mtime = -1, t_body;
	uint64_t done = 0, total;
	struct tr_sink sink;
	struct stat st;
	char *body = NULL;
	int si, dfd, e;

	si = req_target(r, target, sizeof(target), dir, sizeof(dir), rel, sizeof(rel), true);
	if (si == -1)
		goto early;
	if (r->te) {                                /* chunked: not supported */
		respond_err(r, 411, "length_required");
		goto early;
	}
	if (r->clen < 0) {
		respond_err(r, 411, "length_required");
		goto early;
	}
	if (get_param(r->query, "overwrite", tmp, sizeof(tmp)) == 0)
		overwrite = !strcmp(tmp, "1");
	if (get_param(r->query, "mtime", tmp, sizeof(tmp)) == 0)
		mtime = strtoll(tmp, NULL, 10);
	total = (uint64_t)r->clen;
	if (total + UPLOAD_RESERVE > tr_free_bytes_fd(S.rootfd)) {
		respond_err(r, 507, "no_space");
		goto early;
	}
	if (total > 0xffffffffull) {
		struct statfs v;

		/* FAT32 fallback on /data: 4 GiB - 1 per file */
		if (fstatfs(S.rootfd, &v) == 0 && v.f_type == 0x4d44 /* MSDOS_SUPER_MAGIC */) {
			respond_err(r, 413, "too_large_for_fat32");
			goto early;
		}
	}
	tr_split_path(rel, sub, sizeof(sub), base, sizeof(base));
	snprintf(chain, sizeof(chain), "%s%s%s", dir, sub[0] ? "/" : "", sub);
	snprintf(key, sizeof(key), "%s/%s", chain, base);
	if (!inflight_claim(key)) {
		respond_err(r, 409, "busy");
		goto early;
	}
	claimed = true;
	dfd = tr_open_dir_chain(S.rootfd, chain, true);
	if (dfd < 0) {
		respond_err(r, dfd == -ENOTDIR ? 409 : 500, dfd == -ENOTDIR ? "not_a_folder" : "mkdir");
		goto early;
	}
	if (fstatat(dfd, base, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		existed = true;
		if (!S_ISREG(st.st_mode) || !overwrite) {
			close(dfd);
			respond_err(r, 409, S_ISREG(st.st_mode) ? "exists" : "not_a_file");
			goto early;
		}
	}
	if ((e = tr_sink_open(&sink, dfd, base)) < 0) {
		close(dfd);
		respond_err(r, 500, "open");
		goto early;
	}
	/* overwrite=0: the name must still be free at the commit (the USB
	 * import may have written it meanwhile), atomically */
	sink.no_replace = !overwrite;
	/* a replaced save or state: .bak, .bak2, .bak3 (review: two saves of
	 * one name in a dropped folder, or a retry, lost the console's copy) */
	sink.bak_gens = TR_BAK_GENERATIONS;
	if (r->expect && !strcasecmp(r->expect, "100-continue"))
		send_all(r->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);

	set_current(rel, 0, total);
	/* body bytes that came with the head */
	if (r->len > r->hdr_end) {
		size_t n = r->len - r->hdr_end;

		if (n > total)
			n = total;
		if ((e = tr_sink_write(&sink, r->buf + r->hdr_end, n)) < 0)
			goto fail;
		done = n;
	}
	body = malloc(BODY_BUF);
	if (!body) {
		e = -ENOMEM;
		goto fail;
	}
	t_body = tr_now_ms();
	while (done < total) {
		size_t want = total - done > BODY_BUF ? BODY_BUF : (size_t)(total - done);
		ssize_t n = recv(r->fd, body, want, 0);
		int64_t el;

		if (n <= 0 || S.stopping) {
			e = n == 0 || S.stopping ? -ECONNABORTED : -errno;
			goto fail;
		}
		/* a trickle keeps a slot (and the share) busy for nothing (F-L1) */
		el = tr_now_ms() - t_body;
		if (el > 60000 && (done + (uint64_t)n) * 1000 / (uint64_t)el < BODY_MIN_BPS) {
			tr_log("web share: upload of %s too slow, dropped", rel);
			e = -ETIMEDOUT;
			goto fail;
		}
		if ((e = tr_sink_write(&sink, body, (size_t)n)) < 0)
			goto fail;
		done += (uint64_t)n;
		pthread_mutex_lock(&S.lock);
		S.st.cur_done = done;
		S.st.bytes_received += (uint64_t)n;
		S.last_activity = tr_now_ms();
		pthread_mutex_unlock(&S.lock);
	}
	free(body);
	body = NULL;
	e = tr_sink_commit(&sink, mtime, 0,
			   existed && (!strcmp(target, "saves") || !strcmp(target, "states")));
	close(dfd);
	set_current("", 0, 0);
	inflight_release(key);
	if (e == -EEXIST) {
		respond_err(r, 409, "exists");
		return;
	}
	if (e < 0) {
		respond_err(r, e == -ENOSPC ? 507 : 500, e == -ENOSPC ? "no_space" : "write");
		return;
	}
	pthread_mutex_lock(&S.lock);
	S.st.files_received++;
	pthread_mutex_unlock(&S.lock);
	mark_changed(target, si);
	respond_json(r, 201, "{\"ok\":true}");
	return;

fail:
	free(body);
	tr_sink_abort(&sink);
	close(dfd);
	set_current("", 0, 0);
	inflight_release(key);
	if (e == -ECONNABORTED || e == -EAGAIN || e == -ECONNRESET || e == -ETIMEDOUT)
		return;                             /* client gone or timed out */
	respond_err(r, e == -ENOSPC ? 507 : 500, e == -ENOSPC ? "no_space" : "write");
	linger_close(r);
	return;
early:
	if (claimed)
		inflight_release(key);
	if (r->clen > 0 && r->len - r->hdr_end < (size_t)r->clen)
		linger_close(r);
}

static void h_delete(struct req *r)
{
	char target[16], dir[64], rel[TRANSFER_PATH_MAX], chain[TRANSFER_PATH_MAX + 64];
	char sub[TRANSFER_PATH_MAX], base[256];
	int si, dfd, e = 0;

	si = req_target(r, target, sizeof(target), dir, sizeof(dir), rel, sizeof(rel), true);
	if (si == -1)
		return;
	tr_split_path(rel, sub, sizeof(sub), base, sizeof(base));
	snprintf(chain, sizeof(chain), "%s%s%s", dir, sub[0] ? "/" : "", sub);
	dfd = tr_open_dir_chain(S.rootfd, chain, false);
	if (dfd < 0) {
		respond_err(r, 404, "not_found");
		return;
	}
	if (unlinkat(dfd, base, 0) < 0) {
		e = errno;
		if (e == EISDIR || e == EPERM)      /* only empty folders */
			e = unlinkat(dfd, base, AT_REMOVEDIR) < 0 ? errno : 0;
	} else if (!strcmp(target, "saves") || !strcmp(target, "states")) {
		/* its backups too: the game would load X.srm.bak back as the save
		 * (the loaders fall back to it) */
		char bak[300];

		for (int g = 1; g <= TR_BAK_GENERATIONS; g++)
			if (tr_bak_name(bak, sizeof(bak), base, g) == 0)
				unlinkat(dfd, bak, 0);
	}
	if (!e)
		fsync(dfd);
	close(dfd);
	if (e) {
		respond_err(r, e == ENOENT ? 404 : e == ENOTEMPTY ? 409 : 500,
			    e == ENOENT ? "not_found" : e == ENOTEMPTY ? "not_empty" : "delete");
		return;
	}
	pthread_mutex_lock(&S.lock);
	S.st.files_deleted++;
	S.last_activity = tr_now_ms();
	pthread_mutex_unlock(&S.lock);
	mark_changed(target, si);
	respond_json(r, 200, "{\"ok\":true}");
}

static void handle(struct req *r)
{
	int code = read_head(r);

	if (code < 0)
		return;
	if (code) {
		respond_err(r, code, "bad_request");
		return;
	}
	/* S.last_activity: authenticated requests and logins only (F-L1) */
	if (!host_ok(r->host)) {
		respond_err(r, 421, "bad_host");
		return;
	}
	if (!strcmp(r->method, "GET") && (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html"))) {
		respond(r, 200, "text/html; charset=utf-8", webshare_index_html,
			webshare_index_html_len,
			"Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
			"style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; "
			"form-action 'none'; frame-ancestors 'none'; base-uri 'none'\r\n");
		return;
	}
	if (!strcmp(r->path, "/favicon.ico")) {
		respond(r, 204, "text/plain", "", 0, NULL);
		return;
	}
	if (strncmp(r->path, "/api/", 5)) {
		respond_err(r, 404, "not_found");
		return;
	}
	if (!strcmp(r->path, "/api/login")) {
		if (strcmp(r->method, "POST"))
			respond_err(r, 405, "method");
		else if (!r->xrw || strcmp(r->xrw, "rsos")) {
			/* a foreign page in a LAN browser could post guesses and
			 * lock the owner out (review): the CSRF header here too */
			respond_err(r, 403, "csrf");
			if (r->clen > 0)
				linger_close(r);
		} else
			h_login(r);
		return;
	}
	r->authed = check_auth(r);
	if (!r->authed) {
		respond_err(r, 401, "login");
		if (r->clen > 0)
			linger_close(r);
		return;
	}
	if (strcmp(r->method, "GET") && (!r->xrw || strcmp(r->xrw, "rsos"))) {
		respond_err(r, 403, "csrf");
		if (r->clen > 0)
			linger_close(r);
		return;
	}
	/* Only authenticated work holds off the idle stop: unauthenticated
	 * connections (slow or hostile) never keep the share running (F-L1). */
	pthread_mutex_lock(&S.lock);
	S.busy++;
	S.last_activity = tr_now_ms();
	pthread_mutex_unlock(&S.lock);
	if (!strcmp(r->path, "/api/info") && !strcmp(r->method, "GET"))
		h_info(r);
	else if (!strcmp(r->path, "/api/list") && !strcmp(r->method, "GET"))
		h_list(r);
	else if (!strcmp(r->path, "/api/upload") && !strcmp(r->method, "PUT"))
		h_upload(r);
	else if (!strcmp(r->path, "/api/file") && !strcmp(r->method, "DELETE"))
		h_delete(r);
	else
		respond_err(r, 404, "not_found");
	pthread_mutex_lock(&S.lock);
	S.busy--;
	S.last_activity = tr_now_ms();
	pthread_mutex_unlock(&S.lock);
}

/* ------------------------------------------------------------ threads */

static void *worker(void *arg)
{
	struct req *r = arg;
	struct timeval tv = { .tv_sec = IO_TIMEOUT_S };

	setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(r->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	handle(r);

	pthread_mutex_lock(&S.lock);
	for (int i = 0; i < S.nclients; i++)
		if (S.client_fds[i] == r->fd) {
			S.client_fds[i] = S.client_fds[--S.nclients];
			break;
		}
	S.st.clients = S.nclients;
	pthread_cond_broadcast(&S.cond);
	pthread_mutex_unlock(&S.lock);
	close(r->fd);
	free(r);
	return NULL;
}

static void accept_one(void)
{
	struct sockaddr_in peer;
	socklen_t pl = sizeof(peer);
	int fd = accept4(S.lfd, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
	struct req *r;
	pthread_attr_t at;
	pthread_t th;
	bool full;

	if (fd < 0)
		return;
	if (peer.sin_family != AF_INET ||
	    (!S.allow_public && !is_private(ntohl(peer.sin_addr.s_addr)))) {
		close(fd);
		return;
	}
	pthread_mutex_lock(&S.lock);
	full = S.nclients >= S.max_clients;
	pthread_mutex_unlock(&S.lock);
	if (full) {
		static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
					   "Retry-After: 1\r\nConnection: close\r\n\r\n";

		send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
		close(fd);
		return;
	}
	r = calloc(1, sizeof(*r));
	if (!r) {
		close(fd);
		return;
	}
	r->fd = fd;
	r->peer = peer;
	pthread_mutex_lock(&S.lock);
	S.client_fds[S.nclients++] = fd;
	S.st.clients = S.nclients;
	pthread_mutex_unlock(&S.lock);

	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 256 * 1024);
	if (pthread_create(&th, &at, worker, r) != 0) {
		pthread_mutex_lock(&S.lock);
		S.nclients--;
		S.st.clients = S.nclients;
		pthread_mutex_unlock(&S.lock);
		close(fd);
		free(r);
	}
	pthread_attr_destroy(&at);
}

static void *server(void *arg)
{
	(void)arg;
	while (!S.stopping) {
		struct pollfd pf[2] = { { S.lfd, POLLIN, 0 }, { S.pipe_r, POLLIN, 0 } };
		int n = poll(pf, 2, 1000);

		if (n > 0 && (pf[1].revents & POLLIN))
			break;
		if (n > 0 && (pf[0].revents & POLLIN))
			accept_one();
		if (S.idle_timeout_s > 0) {
			bool idle;

			pthread_mutex_lock(&S.lock);
			idle = S.busy == 0 &&
			       tr_now_ms() - S.last_activity > (int64_t)S.idle_timeout_s * 1000;
			if (idle)
				tr_strlcpy(S.st.stop_reason, "idle", sizeof(S.st.stop_reason));
			pthread_mutex_unlock(&S.lock);
			if (idle)
				break;
		}
	}
	/* stop accepting, abort the connections still open, wait for them */
	close(S.lfd);
	S.lfd = -1;
	S.stopping = 1;
	pthread_mutex_lock(&S.lock);
	for (int i = 0; i < S.nclients; i++)
		shutdown(S.client_fds[i], SHUT_RDWR);
	while (S.nclients > 0)
		pthread_cond_wait(&S.cond, &S.lock);
	S.running = false;
	S.st.running = false;
	pthread_mutex_unlock(&S.lock);
	tr_log("web share stopped (%s)", S.st.stop_reason[0] ? S.st.stop_reason : "stop");
	return NULL;
}

/* ------------------------------------------------------------ API */

void webshare_refresh_urls(void)
{
	struct ifaddrs *ifa = NULL, *i;
	char port[8] = "";

	if (S.port != 80)
		snprintf(port, sizeof(port), ":%u", S.port);
	pthread_mutex_lock(&S.lock);
	S.st.nurls = 0;
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i && S.st.nurls < 4; i = i->ifa_next) {
			char ip[INET_ADDRSTRLEN];

			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
			    (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
				continue;
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
			snprintf(S.st.urls[S.st.nurls++], sizeof(S.st.urls[0]), "http://%s%s/", ip, port);
		}
		freeifaddrs(ifa);
	}
	snprintf(S.st.mdns_url, sizeof(S.st.mdns_url), "http://%s.local%s/", S.hostname, port);
	if (S.st.nurls)
		snprintf(S.st.qr_text, sizeof(S.st.qr_text), "%s#pin=%s", S.st.urls[0], S.pin);
	else
		S.st.qr_text[0] = 0;
	pthread_mutex_unlock(&S.lock);
}

int webshare_start(const struct webshare_config *cfg)
{
	struct webshare_config def = { 0 };
	struct sockaddr_in a;
	unsigned char rnd[16];
	int one = 1, fds[2], e;

	if (S.running)
		return -EALREADY;
	webshare_stop();                           /* stopped by itself (idle): reap */
	if (!cfg)
		cfg = &def;
	tr_strlcpy(S.data_root, cfg->data_root ? cfg->data_root : "/data", sizeof(S.data_root));
	tr_strlcpy(S.hostname, cfg->hostname ? cfg->hostname : "retrostone", sizeof(S.hostname));
	S.port = cfg->port ? cfg->port : 80;
	S.idle_timeout_s = cfg->idle_timeout_s < 0 ? 1800 : cfg->idle_timeout_s;
	if (cfg == &def)
		S.idle_timeout_s = 1800;
	S.max_clients = cfg->max_clients > 0 && cfg->max_clients <= MAX_CLIENTS ?
			cfg->max_clients : 4;
	S.allow_public = cfg->allow_public_peers;

	if (tr_random(rnd, sizeof(rnd)) < 0)
		return -EIO;
	for (int i = 0; i < 16; i++)
		snprintf(S.token + 2 * i, 3, "%02x", rnd[i]);
	if (cfg->pin && cfg->pin[0]) {
		tr_strlcpy(S.pin, cfg->pin, sizeof(S.pin));
	} else {
		uint32_t v;

		do {
			if (tr_random(&v, sizeof(v)) < 0)
				return -EIO;
		} while (v >= 4294000000u);        /* no modulo bias */
		snprintf(S.pin, sizeof(S.pin), "%06u", v % 1000000u);
	}

	S.rootfd = open(S.data_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (S.rootfd < 0)
		return -errno;
	S.lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (S.lfd < 0) {
		e = -errno;
		goto fail;
	}
	setsockopt(S.lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(S.port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (cfg->bind_addr && inet_pton(AF_INET, cfg->bind_addr, &a.sin_addr) != 1) {
		e = -EINVAL;
		goto fail;
	}
	if (bind(S.lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(S.lfd, 8) < 0) {
		e = -errno;
		goto fail;
	}
	if (pipe2(fds, O_CLOEXEC) < 0) {
		e = -errno;
		goto fail;
	}
	S.pipe_r = fds[0];
	S.pipe_w = fds[1];

	pthread_mutex_lock(&S.lock);
	memset(&S.st, 0, sizeof(S.st));
	S.st.running = true;
	S.st.port = S.port;
	tr_strlcpy(S.st.pin, S.pin, sizeof(S.st.pin));
	S.nclients = 0;
	S.busy = 0;
	memset(S.inflight, 0, sizeof(S.inflight));
	memset(S.peers, 0, sizeof(S.peers));
	S.failures = 0;
	S.lock_until = 0;
	S.last_activity = tr_now_ms();
	S.stopping = 0;
	S.running = true;
	pthread_mutex_unlock(&S.lock);
	webshare_refresh_urls();

	if (pthread_create(&S.th, NULL, server, NULL) != 0) {
		S.running = false;
		S.st.running = false;
		e = -EAGAIN;
		goto fail;
	}
	S.thread = true;
	tr_log("web share on port %u", S.port);
	return 0;

fail:
	if (S.lfd >= 0)
		close(S.lfd);
	if (S.pipe_r >= 0)
		close(S.pipe_r);
	if (S.pipe_w >= 0)
		close(S.pipe_w);
	close(S.rootfd);
	S.lfd = S.pipe_r = S.pipe_w = S.rootfd = -1;
	return e;
}

void webshare_stop(void)
{
	if (!S.thread)
		return;
	S.stopping = 1;
	if (S.pipe_w >= 0)
		(void)!write(S.pipe_w, "x", 1);     /* else the poll timeout catches it */
	pthread_join(S.th, NULL);
	S.thread = false;
	close(S.pipe_r);
	close(S.pipe_w);
	close(S.rootfd);
	S.pipe_r = S.pipe_w = S.rootfd = -1;
	memset(S.token, 0, sizeof(S.token));
}

bool webshare_running(void)
{
	bool r;

	pthread_mutex_lock(&S.lock);
	r = S.running;
	pthread_mutex_unlock(&S.lock);
	return r;
}

void webshare_get_status(struct webshare_status *st)
{
	int64_t now = tr_now_ms();

	pthread_mutex_lock(&S.lock);
	*st = S.st;
	{
		/* the longest lockout, global or of a device (shown on the console) */
		int64_t l = S.lock_until;

		for (size_t i = 0; i < sizeof(S.peers) / sizeof(S.peers[0]); i++)
			if (S.peers[i].addr && S.peers[i].lock_until > l)
				l = S.peers[i].lock_until;
		st->lockout_s = l > now ? (int)((l - now + 999) / 1000) : 0;
	}
	pthread_mutex_unlock(&S.lock);
}

int webshare_take_changes(char out[][TRANSFER_SYSID_MAX], int max)
{
	int n = 0;

	pthread_mutex_lock(&S.lock);
	for (int i = 0; i < transfer_system_count() && i < 64 && n < max; i++)
		if (S.changed & (1ull << i)) {
			tr_strlcpy(out[n++], transfer_system_id(i), TRANSFER_SYSID_MAX);
			S.changed &= ~(1ull << i);
		}
	if (S.bios_changed && n < max) {
		tr_strlcpy(out[n++], "bios", TRANSFER_SYSID_MAX);
		S.bios_changed = false;
	}
	if (S.themes_changed && n < max) {
		tr_strlcpy(out[n++], "themes", TRANSFER_SYSID_MAX);
		S.themes_changed = false;
	}
	pthread_mutex_unlock(&S.lock);
	return n;
}
