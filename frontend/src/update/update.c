/*
 * update.c - see update.h and docs/updates.md.
 */
#include "update.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <zstd.h>

#include "http.h"
#include "json.h"
#include "sha256.h"

#ifndef RSOS_BUILD_EPOCH
#define RSOS_BUILD_EPOCH 0L
#endif

#define IOBUF (1u << 20)             /* 1 MiB reads and writes */
#define HEAD_HOLD 65536u             /* the image's first bytes, written last */
#define FLUSH_EVERY (8u << 20)       /* write back and drop from the cache every 8 MiB */
#define ZSTD_WINDOW_LOG_MAX 25       /* 32 MiB decoder window at most */
#define JSON_MAX (4u << 20)
#define STATE_NAME "update-state.ini"

/* ------------------------------------------------------------------ log */
static void (*g_log)(const char *line, void *user);
static void *g_log_user;

void upd_set_log(void (*fn)(const char *line, void *user), void *user)
{
	g_log = fn;
	g_log_user = user;
}

void upd_logf(const char *fmt, ...)
{
	char line[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (g_log)
		g_log(line, g_log_user);
	else
		fprintf(stderr, "rsos-update: %s\n", line);
}

static void seterr(struct upd_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void seterr(struct upd_ctx *c, const char *fmt, ...)
{
	va_list ap;
	int en = errno;           /* callers look at errno afterwards */

	va_start(ap, fmt);
	vsnprintf(c->err, sizeof(c->err), fmt, ap);
	va_end(ap);
	upd_logf("%s", c->err);
	errno = en;
}

/* -------------------------------------------------------------- helpers */
/* a copy that may cut s (no snprintf: GCC's truncation warnings) */
static void cpy(char *d, const char *s, size_t n)
{
	size_t l;

	if (!n)
		return;
	l = s ? strlen(s) : 0;
	if (l > n - 1)
		l = n - 1;
	if (l)
		memcpy(d, s, l);
	d[l] = 0;
}

static void under(char *out, size_t n, const char *root, const char *p)
{
	snprintf(out, n, "%s%s", root, p);
}

static char *slurp(const char *path, size_t max, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t cap = 4096, l = 0;
	char *b;

	if (fd < 0)
		return NULL;
	b = malloc(cap + 1);
	while (b) {
		ssize_t r;

		if (l == cap) {
			char *nb;

			if (cap >= max) {
				free(b);
				b = NULL;
				break;
			}
			cap = cap * 2 > max ? max : cap * 2;
			nb = realloc(b, cap + 1);
			if (!nb) {
				free(b);
				b = NULL;
				break;
			}
			b = nb;
		}
		r = read(fd, b + l, cap - l);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			free(b);
			b = NULL;
			break;
		}
		if (r == 0)
			break;
		l += (size_t)r;
	}
	close(fd);
	if (b) {
		b[l] = 0;
		if (len)
			*len = l;
	}
	return b;
}

/* KEY='value' (version.env, build.env: written by the build, shell quoting) */
static bool env_value(const char *text, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);

	for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
		const char *v;
		size_t o = 0;

		if (strncmp(p, key, kl) || p[kl] != '=')
			continue;
		v = p + kl + 1;
		while (*v && *v != '\n' && o + 1 < n) {
			if (*v == '\'') {
				/* '...' ; the '\'' sequence gives a quote */
				v++;
				while (*v && *v != '\'' && *v != '\n' && o + 1 < n)
					out[o++] = *v++;
				if (*v == '\'')
					v++;
				if (v[0] == '\\' && v[1] == '\'' && o + 1 < n) {
					out[o++] = '\'';
					v += 2;
				}
				continue;
			}
			if (*v == '"') {
				v++;
				continue;
			}
			out[o++] = *v++;
		}
		out[o] = 0;
		return true;
	}
	return false;
}

/* key = value (board.ini) */
static bool ini_value(const char *text, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);

	for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
		const char *q = p, *e;
		size_t l;

		while (*q == ' ' || *q == '\t')
			q++;
		if (strncmp(q, key, kl))
			continue;
		q += kl;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q != '=')
			continue;
		q++;
		while (*q == ' ' || *q == '\t')
			q++;
		e = q + strcspn(q, "#;\r\n");
		while (e > q && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		l = (size_t)(e - q) < n - 1 ? (size_t)(e - q) : n - 1;
		memcpy(out, q, l);
		out[l] = 0;
		return true;
	}
	return false;
}

/* argv[0] with stdout captured (stderr to /dev/null); exit status or -1 */
static int run_capture(char *const argv[], const char *stdin_file, char *out, size_t n)
{
	int pfd[2], st;
	pid_t pid;
	size_t l = 0;

	if (out && n)
		out[0] = 0;
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
		int in = stdin_file ? open(stdin_file, O_RDONLY | O_CLOEXEC) : nul;

		signal(SIGPIPE, SIG_DFL);
		if (in >= 0)
			dup2(in, 0);
		dup2(pfd[1], 1);
		if (nul >= 0)
			dup2(nul, 2);
		execv(argv[0], argv);
		_exit(127);
	}
	close(pfd[1]);
	for (;;) {
		char tmp[512];
		ssize_t r = read(pfd[0], tmp, sizeof(tmp));

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		if (out && l + 1 < n) {
			size_t k = (size_t)r < n - 1 - l ? (size_t)r : n - 1 - l;

			memcpy(out + l, tmp, k);
			l += k;
			out[l] = 0;
		}
	}
	close(pfd[0]);
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int upd_env_get(struct upd_ctx *c, const char *name, char *out, size_t n)
{
	char *argv[] = { c->fw_printenv, "-c", c->fw_config, "-n", (char *)name, NULL };
	char buf[512];
	char *e;

	if (access(c->fw_config, R_OK) < 0)
		return -1;
	if (run_capture(argv, NULL, buf, sizeof(buf)) != 0)
		return -1;
	e = buf + strlen(buf);
	while (e > buf && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
		*--e = 0;
	cpy(out, buf, n);
	return 0;
}

static int fsync_dir(const char *path)
{
	char d[PATH_MAX];
	char *s;
	int fd, r;

	cpy(d, path, sizeof(d));
	s = strrchr(d, '/');
	if (s == d)
		s[1] = 0;
	else if (s)
		*s = 0;
	else
		cpy(d, ".", sizeof(d));
	fd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	r = fsync(fd);
	close(fd);
	return r;
}

static int write_file_atomic(const char *path, const char *data, size_t len)
{
	char tmp[PATH_MAX];
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (write(fd, data, len) != (ssize_t)len || fsync(fd) < 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	close(fd);
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	fsync_dir(path);
	return 0;
}

static int mkdirs(const char *path)
{
	char p[PATH_MAX];

	cpy(p, path, sizeof(p));
	for (char *s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = 0;
		if (mkdir(p, 0755) < 0 && errno != EEXIST)
			return -1;
		*s = '/';
	}
	return mkdir(p, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

static bool test_abort(const char *point, uint64_t at)
{
	const char *a = getenv("RSOS_UPDATE_TEST_ABORT");
	size_t l = strlen(point);

	if (!a || strncmp(a, point, l) || (a[l] && a[l] != ':'))
		return false;
	return !a[l] || at >= strtoull(a + l + 1, NULL, 10);
}

static void maybe_abort(const char *point, uint64_t at)
{
	if (test_abort(point, at)) {
		upd_logf("test: RSOS_UPDATE_TEST_ABORT=%s: exiting as a power cut would stop us", getenv("RSOS_UPDATE_TEST_ABORT"));
		_exit(99);
	}
}

/* ----------------------------------------------------------------- init */
void upd_ctx_init(struct upd_ctx *c, const char *root)
{
	memset(c, 0, sizeof(*c));
	cpy(c->root, root ? root : "", sizeof(c->root));
	under(c->version_env, sizeof(c->version_env), c->root, "/etc/rsos/version.env");
	under(c->board_ini, sizeof(c->board_ini), c->root, "/etc/rsos/board.ini");
	under(c->cmdline, sizeof(c->cmdline), c->root, "/proc/cmdline");
	under(c->fw_config, sizeof(c->fw_config), c->root, "/etc/fw_env.config");
	cpy(c->fw_printenv, "/usr/sbin/fw_printenv", sizeof(c->fw_printenv));
	cpy(c->fw_setenv, "/usr/sbin/fw_setenv", sizeof(c->fw_setenv));
	under(c->pubkey, sizeof(c->pubkey), c->root, "/usr/share/rsos/update.pub");
	under(c->data_dir, sizeof(c->data_dir), c->root, "/data");
	under(c->run_dir, sizeof(c->run_dir), c->root, "/run/rsos");
	under(c->media_dir, sizeof(c->media_dir), c->root, "/media");
	under(c->power_dir, sizeof(c->power_dir), c->root, "/sys/class/power_supply");
	cpy(c->ca_file, "/etc/ssl/certs/ca-certificates.crt", sizeof(c->ca_file));
	cpy(c->api_url, UPD_REPO_API, sizeof(c->api_url));
}

static void cmdline_word(const char *cl, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);

	out[0] = 0;
	for (const char *p = cl; *p;) {
		size_t l;

		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		l = strcspn(p, " \t\n");
		if (l > kl && !strncmp(p, key, kl)) {
			size_t v = l - kl < n - 1 ? l - kl : n - 1;

			memcpy(out, p + kl, v);
			out[v] = 0;
		}
		p += l;
	}
}

void upd_load_system(struct upd_ctx *c)
{
	char *t, v[128];
	size_t len;

	memset(&c->sys, 0, sizeof(c->sys));
	cpy(c->sys.version, "unknown", sizeof(c->sys.version));
	cpy(c->variant, "dev", sizeof(c->variant));
	c->sys.bootloader = 1;
	c->ab = false;
	c->booted = 0;
	c->root_dev[0] = 0;

	t = slurp(c->version_env, 16384, &len);
	if (t) {
		if (env_value(t, "RSOS_VERSION", v, sizeof(v)) && rsu_safe_token(v))
			cpy(c->sys.version, v, sizeof(c->sys.version));
		if (env_value(t, "RSOS_BOARD_ID", v, sizeof(v)) && rsu_safe_token(v))
			cpy(c->sys.board, v, sizeof(c->sys.board));
		if (env_value(t, "RSOS_VARIANT", v, sizeof(v)) && (!strcmp(v, "release") || !strcmp(v, "dev")))
			cpy(c->variant, v, sizeof(c->variant));
		if (env_value(t, "RSOS_BUILD_TIME", v, sizeof(v)))
			c->sys.build_time = strtoll(v, NULL, 10);
		if (env_value(t, "RSOS_BUILD_DATE", v, sizeof(v)))
			cpy(c->build_date, v, sizeof(c->build_date));
		free(t);
	}
	c->sys.release = !strcmp(c->variant, "release");
	c->sys.have_key = rsu_pubkey_load(c->pubkey, &c->sys.key) == 0;

	t = slurp(c->cmdline, 8192, &len);
	if (t) {
		cmdline_word(t, "rsos.slot=", v, sizeof(v));
		if (!strcmp(v, "a") || !strcmp(v, "b"))
			c->booted = v[0];
		cmdline_word(t, "root=", c->root_dev, sizeof(c->root_dev));
		free(t);
	}

	/* A/B: board.ini "ab_update = auto | 1 | 0" (auto: a U-Boot environment
	 * and a kernel started by the A/B boot script) */
	cpy(v, "auto", sizeof(v));
	t = slurp(c->board_ini, 65536, &len);
	if (t) {
		ini_value(t, "ab_update", v, sizeof(v));
		free(t);
	}
	if (!c->sys.board[0]) {
		cpy(c->ab_reason, "unknown board (no /etc/rsos/version.env)", sizeof(c->ab_reason));
	} else if (!strcmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "false")) {
		cpy(c->ab_reason, "this board is updated by flashing the SD card (ab_update = 0)", sizeof(c->ab_reason));
	} else if (access(c->fw_config, R_OK) < 0) {
		cpy(c->ab_reason, "no boot environment (/etc/fw_env.config): no A/B slots", sizeof(c->ab_reason));
	} else if (!c->booted) {
		cpy(c->ab_reason, "not started by the A/B boot script (no rsos.slot=)", sizeof(c->ab_reason));
	} else {
		c->ab = true;
		c->ab_reason[0] = 0;
		if (upd_env_get(c, "rsos_bootloader", v, sizeof(v)) == 0 && atoi(v) > 0)
			c->sys.bootloader = atoi(v);
	}
}

bool upd_battery_ok(struct upd_ctx *c, int *percent, bool *ac)
{
	DIR *d = opendir(c->power_dir);
	struct dirent *de;
	bool present = false;

	*percent = -1;
	*ac = false;
	if (!d)
		return true;
	while ((de = readdir(d))) {
		char p[PATH_MAX], *s;

		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s/type", c->power_dir, de->d_name);
		s = slurp(p, 256, NULL);
		if (!s)
			continue;
		if (!strncmp(s, "Battery", 7)) {
			char *v;

			snprintf(p, sizeof(p), "%s/%s/present", c->power_dir, de->d_name);
			v = slurp(p, 64, NULL);
			if (!v || atoi(v)) {
				present = true;
				free(v);
				snprintf(p, sizeof(p), "%s/%s/capacity", c->power_dir, de->d_name);
				v = slurp(p, 64, NULL);
				if (v)
					*percent = atoi(v);
				free(v);
				snprintf(p, sizeof(p), "%s/%s/status", c->power_dir, de->d_name);
				v = slurp(p, 64, NULL);
				if (v && !strncmp(v, "Charging", 8))
					*ac = true;
			}
			free(v);
		} else if (!strncmp(s, "Mains", 5) || !strncmp(s, "USB", 3)) {
			char *v;

			snprintf(p, sizeof(p), "%s/%s/online", c->power_dir, de->d_name);
			v = slurp(p, 64, NULL);
			if (v && atoi(v))
				*ac = true;
			free(v);
		}
		free(s);
	}
	closedir(d);
	return !present || *ac || *percent < 0 || *percent >= UPD_BATTERY_MIN;
}

/* --------------------------------------------------------------- header */
enum rsu_err upd_read_header(struct upd_ctx *c, const char *path, struct rsu_header *h)
{
	static uint8_t buf[RSU_HEAD_MAX];
	char err[256];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t len = 0;

	if (fd < 0) {
		seterr(c, "%s: %s", path, strerror(errno));
		return errno == ENOENT ? RSU_E_NOTFOUND : RSU_E_IO;
	}
	while (len < sizeof(buf)) {
		ssize_t r = read(fd, buf + len, sizeof(buf) - len);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			seterr(c, "%s: %s", path, strerror(errno));
			close(fd);
			return RSU_E_IO;
		}
		if (r == 0)
			break;
		len += (size_t)r;
	}
	close(fd);
	if (rsu_header_parse(buf, len, h, err, sizeof(err)) < 0) {
		seterr(c, "%s: %s", path, err);
		return RSU_E_FORMAT;
	}
	return RSU_OK;
}

/* ---------------------------------------------------------------- local */
/* the most useful refusal to report when nothing can be installed */
static int refusal_rank(enum rsu_err e)
{
	switch (e) {
	case RSU_E_BOOTLOADER: return 9;
	case RSU_E_UPDATER: return 8;
	case RSU_E_BADSIG: return 7;
	case RSU_E_UNSIGNED: return 6;
	case RSU_E_NOKEY: return 6;
	case RSU_E_SAME: return 5;
	case RSU_E_OLDER: return 4;
	case RSU_E_BOARD: return 2;
	default: return 1;
	}
}

static void consider(struct upd_ctx *c, const char *path, struct upd_found *best, struct upd_found *refused,
		     int *seen)
{
	static struct rsu_header h;
	enum rsu_err e;
	struct stat st;
	unsigned flags = c->flags;

	if (upd_read_header(c, path, &h) != RSU_OK)
		return;
	(*seen)++;
	/* development builds list unsigned local packages (installing one asks) */
	if (!c->sys.release)
		flags |= RSU_ALLOW_UNSIGNED;
	e = rsu_policy(&c->sys, &h, flags);
	if (e == RSU_OK && !c->ab)
		e = RSU_E_NOAB;
	upd_logf("found %s: %s %s for %s (%s): %s", path, h.has_sig ? "signed" : "unsigned", h.m.version, h.m.board,
		 h.m.variant, e ? rsu_err_code(e) : "installable");
	if (e == RSU_OK) {
		if (best->valid && rsu_version_cmp(h.m.version, h.m.build_time, best->version, best->build_time) <= 0)
			return;
	} else {
		if (refused->valid && refusal_rank(e) <= refusal_rank(refused->verdict))
			return;
		best = refused;
	}
	memset(best, 0, sizeof(*best));
	best->valid = true;
	cpy(best->source, "file", sizeof(best->source));
	cpy(best->where, path, sizeof(best->where));
	cpy(best->name, strrchr(path, '/') ? strrchr(path, '/') + 1 : path, sizeof(best->name));
	cpy(best->version, h.m.version, sizeof(best->version));
	best->build_time = h.m.build_time;
	best->size = stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
	best->is_signed = h.has_sig;
	best->installable = e == RSU_OK;
	best->verdict = e;
	cpy(best->notes, h.m.changelog, sizeof(best->notes));
}

static void scan_dir(struct upd_ctx *c, const char *dir, struct upd_found *best, struct upd_found *refused,
		     int *seen)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d)
		return;
	while ((de = readdir(d))) {
		size_t l = strlen(de->d_name);
		char p[PATH_MAX];

		if (de->d_name[0] == '.' || l < 5 || strcasecmp(de->d_name + l - 4, ".rsu"))
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		consider(c, p, best, refused, seen);
	}
	closedir(d);
}

enum rsu_err upd_scan_local(struct upd_ctx *c, const char *dir, struct upd_found *best)
{
	struct upd_found *refused = calloc(1, sizeof(*refused));
	char p[PATH_MAX];
	int seen = 0;

	if (!refused)
		return RSU_E_INTERNAL;
	memset(best, 0, sizeof(*best));
	if (dir) {
		scan_dir(c, dir, best, refused, &seen);
		snprintf(p, sizeof(p), "%s/RetroStoneOS", dir);
		scan_dir(c, p, best, refused, &seen);
	} else {
		DIR *d;
		struct dirent *de;

		snprintf(p, sizeof(p), "%s/update", c->data_dir);
		scan_dir(c, p, best, refused, &seen);
		snprintf(p, sizeof(p), "%s/rsos/update", c->data_dir);
		scan_dir(c, p, best, refused, &seen);
		d = opendir(c->media_dir);
		while (d && (de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(p, sizeof(p), "%s/%s", c->media_dir, de->d_name);
			scan_dir(c, p, best, refused, &seen);
			snprintf(p, sizeof(p), "%s/%s/RetroStoneOS", c->media_dir, de->d_name);
			scan_dir(c, p, best, refused, &seen);
		}
		if (d)
			closedir(d);
	}
	if (!best->valid && refused->valid)
		*best = *refused;
	free(refused);
	return seen ? RSU_OK : RSU_E_NOTFOUND;
}

/* -------------------------------------------------------------- network */
struct membuf {
	char *b;
	size_t len, cap, max;
	bool full;
	volatile int *cancel;
};

static int mem_data(const void *data, size_t n, void *user)
{
	struct membuf *m = user;

	if (m->cancel && *m->cancel)
		return -1;
	if (m->len + n > m->max) {
		n = m->max - m->len;
		m->full = true;
	}
	if (m->len + n + 1 > m->cap) {
		size_t cap = m->cap ? m->cap : 65536;
		char *nb;

		while (cap < m->len + n + 1)
			cap *= 2;
		nb = realloc(m->b, cap);
		if (!nb)
			return -1;
		m->b = nb;
		m->cap = cap;
	}
	memcpy(m->b + m->len, data, n);
	m->len += n;
	m->b[m->len] = 0;
	return m->full ? -1 : 0;
}

static void http_defaults(struct upd_ctx *c, struct http_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->timeout_ms = 30000;
	o->allow_http = c->allow_http;
	o->ca_file = c->ca_file;
	o->cancel = &c->cancel;
	o->user_agent = "RetroStoneOS-updater/1";
}

static bool clock_sane(struct upd_ctx *c)
{
	time_t now = time(NULL);
	int64_t floor = RSOS_BUILD_EPOCH > c->sys.build_time ? RSOS_BUILD_EPOCH : c->sys.build_time;

	/* a day of margin for time zones and build hosts */
	return (int64_t)now >= floor - 86400;
}

static bool ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);

	return a >= b && !strcmp(s + a - b, suf);
}

/* Reads the first RSU_HEAD_MAX bytes of a remote package. */
static enum rsu_err fetch_head(struct upd_ctx *c, const char *url, struct rsu_header *h)
{
	struct http_opts o;
	struct http_resp r;
	struct membuf m = { .max = RSU_HEAD_MAX, .cancel = &c->cancel };
	char err[512];
	enum rsu_err e;

	http_defaults(c, &o);
	e = http_get(&o, url, 0, RSU_HEAD_MAX - 1, NULL, mem_data, &m, &r, err, sizeof(err));
	if (e == RSU_E_CANCELLED && m.full && !c->cancel)
		e = RSU_OK;                   /* the server sent the whole file: we stopped reading */
	if (e) {
		seterr(c, "package header: %s", err);
		free(m.b);
		return e;
	}
	if (rsu_header_parse((const uint8_t *)m.b, m.len, h, err, sizeof(err)) < 0) {
		seterr(c, "%s: %s", url, err);
		free(m.b);
		return RSU_E_FORMAT;
	}
	free(m.b);
	return RSU_OK;
}

enum rsu_err upd_check_net(struct upd_ctx *c, struct upd_found *out)
{
	struct http_opts o;
	struct http_resp r;
	struct membuf m = { .max = JSON_MAX, .cancel = &c->cancel };
	char err[512], suf_rsu[96], suf_img[96];
	struct json *root;
	const struct json *best = NULL, *best_rsu = NULL, *best_img = NULL;
	const char *best_ver = NULL;
	enum rsu_err e;

	memset(out, 0, sizeof(*out));
	if (!c->sys.board[0]) {
		seterr(c, "unknown board: no /etc/rsos/version.env");
		return RSU_E_INTERNAL;
	}
	if (!c->allow_http && !http_have_tls()) {
		seterr(c, "no HTTPS support in this build");
		return RSU_E_NOTLS;
	}
	if (!clock_sane(c)) {
		seterr(c, "the clock (%lld) is before this system was built: not set yet", (long long)time(NULL));
		return RSU_E_CLOCK;
	}
	http_defaults(c, &o);
	o.accept = "application/vnd.github+json";
	upd_logf("checking %s", c->api_url);
	e = http_get(&o, c->api_url, 0, -1, NULL, mem_data, &m, &r, err, sizeof(err));
	if (e) {
		if (r.status == 403)
			snprintf(err + strlen(err), sizeof(err) - strlen(err), " (GitHub rate limit? try again later)");
		seterr(c, "%s", err);
		free(m.b);
		return e;
	}
	root = json_parse(m.b ? m.b : "", m.len, err, sizeof(err));
	free(m.b);
	if (!root || root->type != JSON_ARR) {
		seterr(c, "releases: %s", root ? "not a list" : err);
		json_free(root);
		return RSU_E_HTTP;
	}
	snprintf(suf_rsu, sizeof(suf_rsu), "-%s.rsu", c->sys.board);
	snprintf(suf_img, sizeof(suf_img), "-%s.img.xz", c->sys.board);
	for (const struct json *rel = root->child; rel; rel = rel->next) {
		const char *tag = json_str(rel, "tag_name");
		const struct json *assets = json_get(rel, "assets"), *rsu = NULL, *img = NULL;
		const char *ver;

		if (!tag || json_bool(rel, "draft", false) || (json_bool(rel, "prerelease", false) && !c->prerelease))
			continue;
		ver = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
		if (!rsu_safe_token(ver))
			continue;
		for (const struct json *a = assets && assets->type == JSON_ARR ? assets->child : NULL; a; a = a->next) {
			const char *name = json_str(a, "name");

			if (!name || strncmp(name, "retrostoneos-", 13))
				continue;
			if (ends_with(name, suf_rsu))
				rsu = a;
			else if (ends_with(name, suf_img))
				img = a;
		}
		if (!rsu && !img)
			continue;
		if (!best || rsu_version_cmp(ver, 0, best_ver, 0) > 0) {
			best = rel;
			best_ver = ver;
			best_rsu = rsu;
			best_img = img;
		}
	}
	if (!best) {
		seterr(c, "no release with a file for %s", c->sys.board);
		json_free(root);
		return RSU_E_NOTFOUND;
	}
	out->valid = true;
	cpy(out->source, "net", sizeof(out->source));
	cpy(out->version, best_ver, sizeof(out->version));
	cpy(out->page, json_str(best, "html_url"), sizeof(out->page));
	cpy(out->notes, json_str(best, "body"), sizeof(out->notes));
	out->is_signed = false;
	out->verdict = RSU_OK;
	if (rsu_version_cmp(best_ver, 0, c->sys.version, c->sys.build_time) <= 0) {
		upd_logf("up to date: newest release %s, running %s", best_ver, c->sys.version);
		json_free(root);
		return RSU_E_SAME;
	}
	if (c->ab && best_rsu) {
		static struct rsu_header h;
		const char *url = json_str(best_rsu, "browser_download_url");

		cpy(out->where, url, sizeof(out->where));
		cpy(out->name, json_str(best_rsu, "name"), sizeof(out->name));
		out->size = (uint64_t)json_num(best_rsu, "size", 0);
		e = url ? fetch_head(c, url, &h) : RSU_E_HTTP;
		if (e == RSU_OK) {
			/* never an unsigned package from the network */
			e = rsu_policy(&c->sys, &h, c->flags & ~(unsigned)RSU_ALLOW_UNSIGNED);
			out->is_signed = h.has_sig;
			out->build_time = h.m.build_time;
			if (strcmp(h.m.version, best_ver))
				upd_logf("note: release %s holds a package of version %s", best_ver, h.m.version);
			cpy(out->version, h.m.version, sizeof(out->version));
			if (!out->notes[0])
				cpy(out->notes, h.m.changelog, sizeof(out->notes));
		}
		if (e == RSU_E_CANCELLED || e == RSU_E_NETWORK || e == RSU_E_TLS || e == RSU_E_CLOCK) {
			json_free(root);
			return e;
		}
		out->verdict = e;
		out->installable = e == RSU_OK;
		upd_logf("release %s: %s (%" PRIu64 " bytes): %s", best_ver, out->name, out->size,
			 e ? rsu_err_code(e) : "installable");
	} else {
		/* no in-place update: tell the user where the image is */
		out->verdict = c->ab ? RSU_E_NOTFOUND : RSU_E_NOAB;
		if (best_img) {
			cpy(out->where, json_str(best_img, "browser_download_url"), sizeof(out->where));
			cpy(out->name, json_str(best_img, "name"), sizeof(out->name));
			out->size = (uint64_t)json_num(best_img, "size", 0);
		}
		upd_logf("release %s: %s", best_ver, c->ab ? "no update package for this board" : "reflash needed");
	}
	json_free(root);
	return RSU_OK;
}

/* ------------------------------------------------------------- download */
struct dl {
	struct upd_ctx *c;
	int fd;
	uint64_t off, from, size;
	bool bad;
	char why[256];
};

static int dl_head(const struct http_resp *r, void *user)
{
	struct dl *d = user;

	if (r->status == 206 && r->range_start == (int64_t)d->from) {
		d->off = d->from;
	} else if (r->status == 200) {
		if (d->from)
			upd_logf("the server restarts the download from the beginning");
		d->off = 0;
		if (ftruncate(d->fd, 0) < 0) {
			snprintf(d->why, sizeof(d->why), "truncate: %s", strerror(errno));
			d->bad = true;
			return -1;
		}
	} else if (r->status >= 200 && r->status < 300) {
		snprintf(d->why, sizeof(d->why), "unexpected HTTP %d", r->status);
		d->bad = true;
		return -1;
	}
	if (r->status >= 200 && r->status < 300 && r->total >= 0 && (uint64_t)r->total != d->size) {
		snprintf(d->why, sizeof(d->why), "the file on the server has %lld bytes, not %" PRIu64,
			 (long long)r->total, d->size);
		d->bad = true;
		return -1;
	}
	return 0;
}

static int dl_data(const void *data, size_t n, void *user)
{
	struct dl *d = user;
	const char *p = data;

	if (d->c->cancel)
		return -1;
	if (d->off + n > d->size) {
		snprintf(d->why, sizeof(d->why), "more data than expected");
		d->bad = true;
		return -1;
	}
	while (n) {
		ssize_t w = pwrite(d->fd, p, n, (off_t)d->off);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			snprintf(d->why, sizeof(d->why), "write: %s", w < 0 ? strerror(errno) : "short write");
			d->bad = true;
			return -1;
		}
		p += w;
		n -= (size_t)w;
		d->off += (uint64_t)w;
	}
	if (d->c->progress)
		d->c->progress(d->c, "download", d->off, d->size);
	return 0;
}

enum rsu_err upd_download(struct upd_ctx *c, const char *url, const char *name, uint64_t size,
			  char *path_out, size_t n)
{
	char dir[PATH_MAX - 256], fin[PATH_MAX], part[PATH_MAX + 16], info[PATH_MAX + 16], want[512], err[512];
	struct stat st;
	struct statvfs sv;
	struct http_opts o;
	struct http_resp r;
	struct dl d = { .c = c, .fd = -1, .size = size };
	enum rsu_err e = RSU_OK;
	char *have;
	int tries;

	if (!rsu_safe_token(name) || !ends_with(name, ".rsu") || !size) {
		seterr(c, "bad download name or size: %s", name);
		return RSU_E_INTERNAL;
	}
	snprintf(dir, sizeof(dir), "%s/rsos/update", c->data_dir);
	if (mkdirs(dir) < 0) {
		seterr(c, "%s: %s", dir, strerror(errno));
		return RSU_E_IO;
	}
	snprintf(fin, sizeof(fin), "%s/%s", dir, name);
	snprintf(part, sizeof(part), "%s.part", fin);
	snprintf(info, sizeof(info), "%s.part.info", fin);
	cpy(path_out, fin, n);
	if (stat(fin, &st) == 0 && (uint64_t)st.st_size == size) {
		upd_logf("%s is already downloaded", fin);
		return RSU_OK;
	}
	/* resume only the same file (name and size) */
	snprintf(want, sizeof(want), "name=%s\nsize=%" PRIu64 "\n", name, size);
	have = slurp(info, 4096, NULL);
	if (have && !strcmp(have, want) && stat(part, &st) == 0 && (uint64_t)st.st_size <= size)
		d.from = (uint64_t)st.st_size;
	free(have);
	if (statvfs(dir, &sv) == 0 &&
	    (uint64_t)sv.f_bavail * sv.f_frsize < size - d.from + (16u << 20)) {
		seterr(c, "%s: %" PRIu64 " MB free, %" PRIu64 " MB needed", dir,
		       (uint64_t)sv.f_bavail * sv.f_frsize >> 20, (size - d.from + (16u << 20)) >> 20);
		return RSU_E_SPACE;
	}
	if (write_file_atomic(info, want, strlen(want)) < 0) {
		seterr(c, "%s: %s", info, strerror(errno));
		return RSU_E_IO;
	}
	d.fd = open(part, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (d.fd < 0) {
		seterr(c, "%s: %s", part, strerror(errno));
		return RSU_E_IO;
	}
	http_defaults(c, &o);
	for (tries = 0; tries < 4; tries++) {
		if (d.from == size)
			break;
		upd_logf("downloading %s from byte %" PRIu64 " of %" PRIu64 "%s", url, d.from, size,
			 tries ? " (retry)" : "");
		d.off = d.from;
		d.bad = false;
		e = http_get(&o, url, (int64_t)d.from, -1, dl_head, dl_data, &d, &r, err, sizeof(err));
		if (d.bad) {
			seterr(c, "download: %s", d.why);
			e = RSU_E_HTTP;
			break;
		}
		d.from = d.off;
		if (e == RSU_OK && d.off == size)
			break;
		if (e == RSU_OK) {
			snprintf(err, sizeof(err), "the download ended after %" PRIu64 " of %" PRIu64 " bytes", d.off, size);
			e = RSU_E_NETWORK;
		}
		if (e == RSU_E_HTTP && r.status == 416 && d.from == size)
			break;                      /* nothing left to get */
		seterr(c, "download: %s", err);
		if (e != RSU_E_NETWORK || c->cancel)
			break;
		fdatasync(d.fd);
		sleep(tries + 1);
	}
	if (d.from == size && !d.bad && !c->cancel)
		e = RSU_OK;
	if (e == RSU_OK && fsync(d.fd) < 0) {
		seterr(c, "%s: %s", part, strerror(errno));
		e = RSU_E_IO;
	}
	close(d.fd);
	if (e)
		return e;                         /* the part stays: the next try resumes */
	if (rename(part, fin) < 0) {
		seterr(c, "%s: %s", fin, strerror(errno));
		return RSU_E_IO;
	}
	unlink(info);
	fsync_dir(fin);
	upd_logf("downloaded %s (%" PRIu64 " bytes)", fin, size);
	return RSU_OK;
}

/* ---------------------------------------------------------------- slots */
/* "/dev/mmcblk0p2" -> disk "/dev/mmcblk0", separator "p", partition 2 */
static int split_part(const char *dev, char *disk, size_t n, char *sep, int *part)
{
	size_t l = strlen(dev), i = l;

	while (i > 0 && isdigit((unsigned char)dev[i - 1]))
		i--;
	if (i == l || i == 0 || l - i > 3)
		return -1;
	*part = atoi(dev + i);
	*sep = 0;
	if (dev[i - 1] == 'p' && i >= 2 && isdigit((unsigned char)dev[i - 2]))
		*sep = 'p', i--;
	if (i >= n)
		return -1;
	memcpy(disk, dev, i);
	disk[i] = 0;
	return 0;
}

enum rsu_err upd_slots(struct upd_ctx *c)
{
	char disk[256], sep, v[64], ok[64];
	int part;

	c->target[0] = 0;
	c->target_slot = 0;
	if (!c->ab) {
		seterr(c, "no A/B update on this system: %s", c->ab_reason);
		return RSU_E_NOAB;
	}
	if (strncmp(c->root_dev, "/dev/", 5) || split_part(c->root_dev, disk, sizeof(disk), &sep, &part) < 0) {
		seterr(c, "root=%s: not a partition of a disk", c->root_dev);
		return RSU_E_SLOT;
	}
	if ((c->booted == 'a' && part != 2) || (c->booted == 'b' && part != 3)) {
		seterr(c, "rsos.slot=%c does not match root=%s", c->booted, c->root_dev);
		return RSU_E_SLOT;
	}
	c->target_slot = c->booted == 'a' ? 'b' : 'a';
	if (c->target_dev[0])
		cpy(c->target, c->target_dev, sizeof(c->target));
	else if (sep)
		snprintf(c->target, sizeof(c->target), "%sp%d", disk, c->booted == 'a' ? 3 : 2);
	else
		snprintf(c->target, sizeof(c->target), "%s%d", disk, c->booted == 'a' ? 3 : 2);
	if (upd_env_get(c, "rsos_slot", v, sizeof(v)) < 0) {
		seterr(c, "cannot read the boot environment (%s)", c->fw_config);
		return RSU_E_ENV;
	}
	if (v[0] != c->booted || v[1]) {
		if (v[0] == c->target_slot && !v[1] && upd_env_get(c, "rsos_ok", ok, sizeof(ok)) == 0 && !strcmp(ok, "0")) {
			seterr(c, "an update is installed in slot %s: restart first", v);
			return RSU_E_RESTART;
		}
		seterr(c, "the boot environment selects slot %s, but slot %c runs", v, c->booted);
		return RSU_E_UNCONFIRMED;
	}
	/* a system on trial (just updated) is the only one that boots: its
	 * fallback, the other slot, must not be overwritten before it is
	 * confirmed (rsos-boot-ok, about a minute after the menu is up) */
	if (upd_env_get(c, "rsos_ok", ok, sizeof(ok)) == 0 && strcmp(ok, "1")) {
		seterr(c, "slot %c is not confirmed yet (rsos_ok=%s)", c->booted, ok);
		return RSU_E_UNCONFIRMED;
	}
	upd_logf("running slot %c (%s), installing into slot %c (%s)", c->booted, c->root_dev, c->target_slot,
		 c->target);
	return RSU_OK;
}

/* ------------------------------------------------------------------ apply */
static int lock_updates(struct upd_ctx *c)
{
	char p[PATH_MAX];
	int fd;

	mkdirs(c->run_dir);
	snprintf(p, sizeof(p), "%s/update.lock", c->run_dir);
	fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		close(fd);
		return -2;
	}
	return fd;
}

static ssize_t read_full(int fd, void *buf, size_t n, off_t off)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = pread(fd, (char *)buf + got, n - got, off + (off_t)got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			return -1;
		if (r == 0)
			break;
		got += (size_t)r;
	}
	return (ssize_t)got;
}

static int write_full(int fd, const void *buf, size_t n, off_t off)
{
	size_t done = 0;

	while (done < n) {
		ssize_t w = pwrite(fd, (const char *)buf + done, n - done, off + (off_t)done);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		done += (size_t)w;
	}
	return 0;
}

/* pass 1: the payload's hash, before anything is written */
static enum rsu_err verify_payload(struct upd_ctx *c, int fd, const struct rsu_header *h, uint8_t *buf)
{
	struct sha256 s;
	uint64_t done = 0;
	uint8_t d[32];

	sha256_init(&s);
	while (done < h->m.payload_size) {
		size_t want = h->m.payload_size - done < IOBUF ? (size_t)(h->m.payload_size - done) : IOBUF;
		ssize_t r = read_full(fd, buf, want, (off_t)(h->payload_offset + done));

		if (c->cancel)
			return RSU_E_CANCELLED;
		if (r < 0) {
			seterr(c, "reading the package: %s", strerror(errno));
			return RSU_E_IO;
		}
		if ((size_t)r != want) {
			seterr(c, "the package is truncated (%" PRIu64 " of %" PRIu64 " payload bytes)", done + (uint64_t)r,
			       h->m.payload_size);
			return RSU_E_PAYLOAD;
		}
		sha256_update(&s, buf, want);
		done += want;
		if (c->progress)
			c->progress(c, "verify", done, h->m.payload_size);
	}
	sha256_final(&s, d);
	if (memcmp(d, h->m.payload_sha256, 32)) {
		seterr(c, "the payload's SHA-256 does not match the signed manifest");
		return RSU_E_PAYLOAD;
	}
	upd_logf("payload verified (%" PRIu64 " bytes)", h->m.payload_size);
	return RSU_OK;
}

struct writer {
	struct upd_ctx *c;
	int fd;
	uint64_t off, size, flushed;
	uint8_t *head;                /* the image's first bytes, written last */
	size_t head_len;
	struct sha256 img;
};

static enum rsu_err writer_put(struct writer *w, const uint8_t *p, size_t n)
{
	if (w->off + n > w->size) {
		seterr(w->c, "the payload decompresses to more than %" PRIu64 " bytes", w->size);
		return RSU_E_PAYLOAD;
	}
	sha256_update(&w->img, p, n);
	if (w->off < w->head_len) {
		size_t k = w->head_len - w->off < n ? (size_t)(w->head_len - w->off) : n;

		memcpy(w->head + w->off, p, k);
		w->off += k;
		p += k;
		n -= k;
	}
	if (n && w->fd >= 0) {
		if (write_full(w->fd, p, n, (off_t)w->off) < 0) {
			seterr(w->c, "writing %s: %s", w->c->target, strerror(errno));
			return RSU_E_IO;
		}
		w->off += n;
	} else {
		w->off += n;
	}
	/* write back and drop from the page cache as we go: bounded dirty
	 * memory, a progress bar that shows the card's real progress, and the
	 * menu's cached files are not evicted by 512 MiB of image */
	if (w->fd >= 0 && w->off - w->flushed >= FLUSH_EVERY) {
		sync_file_range(w->fd, (off_t)w->flushed, (off_t)(w->off - w->flushed),
				SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
		posix_fadvise(w->fd, (off_t)w->flushed, (off_t)(w->off - w->flushed), POSIX_FADV_DONTNEED);
		w->flushed = w->off;
	}
	if (w->c->progress)
		w->c->progress(w->c, w->fd >= 0 ? "write" : "check", w->off, w->size);
	if (w->fd >= 0)
		maybe_abort("write", w->off);
	return RSU_OK;
}

/* pass 2: decompress, write, hash both sides */
static enum rsu_err write_image(struct upd_ctx *c, int pfd, const struct rsu_header *h, int tfd, uint8_t *inbuf,
				uint8_t *outbuf)
{
	struct writer w = { .c = c, .fd = tfd, .size = h->m.image_size };
	struct sha256 pay;
	uint64_t in_done = 0;
	ZSTD_DCtx *z = NULL;
	enum rsu_err e = RSU_OK;
	size_t zr = 1;
	bool zstd = !strcmp(h->m.compression, "zstd");
	uint8_t d[32];

	w.head_len = h->m.image_size < HEAD_HOLD ? (size_t)h->m.image_size : HEAD_HOLD;
	w.head = calloc(1, HEAD_HOLD);
	if (!w.head)
		return RSU_E_INTERNAL;
	sha256_init(&w.img);
	sha256_init(&pay);
	if (zstd) {
		z = ZSTD_createDCtx();
		if (!z || ZSTD_isError(ZSTD_DCtx_setParameter(z, ZSTD_d_windowLogMax, ZSTD_WINDOW_LOG_MAX))) {
			free(w.head);
			ZSTD_freeDCtx(z);
			return RSU_E_INTERNAL;
		}
	}
	/* 1. the slot is no longer a valid file system from now on: a power
	 * cut from here leaves a slot that U-Boot never falls back to (its
	 * superblock is gone; the boot script checks /boot/zImage first) */
	if (tfd >= 0 && (write_full(tfd, w.head, w.head_len, 0) < 0 || fdatasync(tfd) < 0)) {
		seterr(c, "writing %s: %s", c->target, strerror(errno));
		e = RSU_E_IO;
		goto out;
	}
	w.flushed = 0;
	/* 2. everything after the first 64 KiB */
	while (in_done < h->m.payload_size && !e) {
		size_t want = h->m.payload_size - in_done < IOBUF ? (size_t)(h->m.payload_size - in_done) : IOBUF;
		ssize_t r = read_full(pfd, inbuf, want, (off_t)(h->payload_offset + in_done));

		if (c->cancel) {
			e = RSU_E_CANCELLED;
			break;
		}
		if (r != (ssize_t)want) {
			seterr(c, "reading the package: %s", r < 0 ? strerror(errno) : "truncated");
			e = RSU_E_IO;
			break;
		}
		sha256_update(&pay, inbuf, want);
		in_done += want;
		if (!zstd) {
			e = writer_put(&w, inbuf, want);
			continue;
		}
		{
			ZSTD_inBuffer in = { inbuf, want, 0 };

			while (in.pos < in.size && !e) {
				ZSTD_outBuffer out = { outbuf, IOBUF, 0 };

				zr = ZSTD_decompressStream(z, &out, &in);
				if (ZSTD_isError(zr)) {
					seterr(c, "decompression: %s", ZSTD_getErrorName(zr));
					e = RSU_E_PAYLOAD;
					break;
				}
				if (out.pos)
					e = writer_put(&w, outbuf, out.pos);
			}
			/* flush what the decoder still holds */
			while (!e && in_done == h->m.payload_size && zr != 0) {
				ZSTD_outBuffer out = { outbuf, IOBUF, 0 };

				zr = ZSTD_decompressStream(z, &out, &in);
				if (ZSTD_isError(zr)) {
					seterr(c, "decompression: %s", ZSTD_getErrorName(zr));
					e = RSU_E_PAYLOAD;
					break;
				}
				if (!out.pos && zr != 0) {
					seterr(c, "the compressed payload is truncated");
					e = RSU_E_PAYLOAD;
					break;
				}
				if (out.pos)
					e = writer_put(&w, outbuf, out.pos);
			}
		}
	}
	if (e)
		goto out;
	sha256_final(&pay, d);
	if (memcmp(d, h->m.payload_sha256, 32)) {
		seterr(c, "the package changed while it was installed");
		e = RSU_E_PAYLOAD;
		goto out;
	}
	if (w.off != h->m.image_size) {
		seterr(c, "the payload decompressed to %" PRIu64 " bytes, not %" PRIu64, w.off, h->m.image_size);
		e = RSU_E_PAYLOAD;
		goto out;
	}
	sha256_final(&w.img, d);
	if (memcmp(d, h->m.image_sha256, 32)) {
		seterr(c, "the decompressed image does not match the manifest");
		e = RSU_E_PAYLOAD;
		goto out;
	}
	if (tfd < 0) {
		upd_logf("image checked (%" PRIu64 " bytes)", w.off);
		goto out;             /* dry run (upd_verify_file) */
	}
	if (fdatasync(tfd) < 0) {
		seterr(c, "flushing %s: %s", c->target, strerror(errno));
		e = RSU_E_IO;
		goto out;
	}
	/* 3. the first 64 KiB (the superblock) last: the slot becomes a valid
	 * file system only once everything else is on the card */
	maybe_abort("head", 0);
	if (write_full(tfd, w.head, w.head_len, 0) < 0 || fdatasync(tfd) < 0) {
		seterr(c, "writing %s: %s", c->target, strerror(errno));
		e = RSU_E_IO;
		goto out;
	}
	upd_logf("image written (%" PRIu64 " bytes)", w.off);
out:
	ZSTD_freeDCtx(z);
	free(w.head);
	return e;
}

/* pass 3: read the slot back from the card (not the page cache) */
static enum rsu_err readback(struct upd_ctx *c, const struct rsu_header *h, uint8_t *buf)
{
	struct sha256 s;
	uint64_t done = 0;
	uint8_t d[32];
	bool direct = true;
	int fd = open(c->target, O_RDONLY | O_CLOEXEC | O_DIRECT);

	if (fd < 0 && errno == EINVAL) {
		direct = false;
		fd = open(c->target, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	}
	if (fd < 0) {
		seterr(c, "%s: %s", c->target, strerror(errno));
		return RSU_E_IO;
	}
	sha256_init(&s);
	while (done < h->m.image_size) {
		size_t want = h->m.image_size - done < IOBUF ? (size_t)(h->m.image_size - done) : IOBUF;
		ssize_t r = read_full(fd, buf, want, (off_t)done);

		if (r < 0 && errno == EINVAL && direct && done == 0) {
			/* no O_DIRECT on this file system (tmpfs in the tests): drop
			 * the cached pages instead, so the reads come from the device */
			close(fd);
			direct = false;
			fd = open(c->target, O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				seterr(c, "%s: %s", c->target, strerror(errno));
				return RSU_E_IO;
			}
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
			continue;
		}
		if (c->cancel) {
			close(fd);
			return RSU_E_CANCELLED;
		}
		if (r != (ssize_t)want) {
			seterr(c, "reading %s back: %s", c->target, r < 0 ? strerror(errno) : "short read");
			close(fd);
			return RSU_E_IO;
		}
		sha256_update(&s, buf, want);
		done += want;
		if (c->progress)
			c->progress(c, "readback", done, h->m.image_size);
	}
	close(fd);
	sha256_final(&s, d);
	if (memcmp(d, h->m.image_sha256, 32)) {
		seterr(c, "the slot read back does not match: the card may be failing");
		return RSU_E_IMAGE;
	}
	upd_logf("slot %c read back: identical", c->target_slot);
	return RSU_OK;
}

/* the one boot environment write (docs/build.md "Updater contract") */
static enum rsu_err switch_slot(struct upd_ctx *c)
{
	char script[PATH_MAX], body[128], v[64], ok[64], tries[64];
	char *argv[] = { c->fw_setenv, "-c", c->fw_config, "-s", script, NULL };
	int r;

	snprintf(script, sizeof(script), "%s/update-env.txt", c->run_dir);
	snprintf(body, sizeof(body), "rsos_slot %c\nrsos_ok 0\nrsos_tries 3\nrsos_fails 0\nrsos_fallback\n",
		 c->target_slot);
	if (write_file_atomic(script, body, strlen(body)) < 0) {
		seterr(c, "%s: %s", script, strerror(errno));
		return RSU_E_IO;
	}
	maybe_abort("env", 0);
	r = run_capture(argv, NULL, NULL, 0);
	unlink(script);
	if (r != 0) {
		seterr(c, "fw_setenv failed (%d)", r);
		return RSU_E_ENV;
	}
	if (upd_env_get(c, "rsos_slot", v, sizeof(v)) < 0 || v[0] != c->target_slot ||
	    upd_env_get(c, "rsos_ok", ok, sizeof(ok)) < 0 || strcmp(ok, "0") ||
	    upd_env_get(c, "rsos_tries", tries, sizeof(tries)) < 0 || strcmp(tries, "3")) {
		seterr(c, "the boot environment does not read back as written");
		return RSU_E_ENV;
	}
	upd_logf("boot environment: rsos_slot=%c rsos_ok=0 rsos_tries=3", c->target_slot);
	return RSU_OK;
}

void upd_state_path(struct upd_ctx *c, char *out, size_t n)
{
	snprintf(out, n, "%s/rsos/" STATE_NAME, c->data_dir);
}

enum rsu_err upd_verify_file(struct upd_ctx *c, const char *path, bool full, struct rsu_header *h)
{
	uint8_t *buf = NULL, *out = NULL;
	enum rsu_err e;
	int fd;

	e = upd_read_header(c, path, h);
	if (e)
		return e;
	/* any version: this only checks the package */
	e = rsu_policy(&c->sys, h, c->flags | RSU_FORCE);
	if (e) {
		seterr(c, "%s: %s", path, rsu_err_text(e));
		return e;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	buf = aligned_alloc(4096, IOBUF);
	out = malloc(IOBUF);
	if (fd < 0 || !buf || !out) {
		seterr(c, "%s: %s", path, fd < 0 ? strerror(errno) : "out of memory");
		e = fd < 0 ? RSU_E_IO : RSU_E_INTERNAL;
	} else {
		e = verify_payload(c, fd, h, buf);
		if (!e && full)
			e = write_image(c, fd, h, -1, buf, out);
	}
	if (fd >= 0)
		close(fd);
	free(buf);
	free(out);
	return e;
}

enum rsu_err upd_apply_file(struct upd_ctx *c, const char *path, bool downloaded, struct rsu_header *h)
{
	uint8_t *buf = NULL, *out = NULL;
	int lock, pfd = -1, tfd = -1, pct;
	enum rsu_err e;
	struct stat st;
	uint64_t tsize = 0;
	bool ac;

	lock = lock_updates(c);
	if (lock == -2) {
		seterr(c, "another rsos-update is installing");
		return RSU_E_BUSY;
	}
	e = upd_read_header(c, path, h);
	if (e)
		goto out;
	e = rsu_policy(&c->sys, h, c->flags);
	if (e) {
		seterr(c, "%s: %s", path, rsu_err_text(e));
		goto out;
	}
	upd_logf("installing %s: %s %s (%s, %s)", path, h->m.board, h->m.version, h->m.variant,
		 h->has_sig ? "signed" : "UNSIGNED");
	if (!c->ignore_battery && !upd_battery_ok(c, &pct, &ac)) {
		seterr(c, "battery at %d%% without a charger", pct);
		e = RSU_E_BATTERY;
		goto out;
	}
	e = upd_slots(c);
	if (e)
		goto out;
	buf = aligned_alloc(4096, IOBUF);
	out = malloc(IOBUF);
	if (!buf || !out) {
		e = RSU_E_INTERNAL;
		goto out;
	}
	pfd = open(path, O_RDONLY | O_CLOEXEC);
	if (pfd < 0) {
		seterr(c, "%s: %s", path, strerror(errno));
		e = RSU_E_IO;
		goto out;
	}
	e = verify_payload(c, pfd, h, buf);
	if (e)
		goto out;
	maybe_abort("verified", 0);

	/* the inactive slot: never the running root, never mounted (O_EXCL on a
	 * block device fails while it is mounted or opened exclusively) */
	if (stat(c->target, &st) < 0) {
		seterr(c, "%s: %s", c->target, strerror(errno));
		e = RSU_E_SLOT;
		goto out;
	}
	{
		struct stat rs;

		if (S_ISBLK(st.st_mode) && stat("/", &rs) == 0 && rs.st_dev == st.st_rdev) {
			seterr(c, "%s is the running system", c->target);
			e = RSU_E_SLOT;
			goto out;
		}
	}
	if (!S_ISBLK(st.st_mode) && !(c->target_dev[0] && S_ISREG(st.st_mode))) {
		seterr(c, "%s is not a block device", c->target);
		e = RSU_E_SLOT;
		goto out;
	}
	tfd = open(c->target, O_RDWR | O_CLOEXEC | (S_ISBLK(st.st_mode) ? O_EXCL : 0));
	if (tfd < 0) {
		int en = errno;

		seterr(c, "%s: %s%s", c->target, strerror(en), en == EBUSY ? " (mounted?)" : "");
		e = en == EBUSY ? RSU_E_SLOT : RSU_E_IO;
		goto out;
	}
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(tfd, BLKGETSIZE64, &tsize) < 0)
			tsize = 0;
	} else {
		tsize = (uint64_t)st.st_size;
	}
	if (tsize < h->m.image_size) {
		seterr(c, "slot %c (%s) has %" PRIu64 " bytes, the image %" PRIu64, c->target_slot, c->target, tsize,
		       h->m.image_size);
		e = RSU_E_SPACE;
		goto out;
	}
	e = write_image(c, pfd, h, tfd, buf, out);
	close(tfd);
	tfd = -1;
	if (e)
		goto out;
	e = readback(c, h, buf);
	if (e)
		goto out;
	maybe_abort("before-env", 0);
	if (c->cancel) {
		e = RSU_E_CANCELLED;
		goto out;
	}
	if (c->progress)
		c->progress(c, "switch", 0, 1);
	e = switch_slot(c);
	if (e)
		goto out;
	{
		char sp[PATH_MAX], body[PATH_MAX + 512];
		int n;

		upd_state_path(c, sp, sizeof(sp));
		{
			char d[PATH_MAX];

			snprintf(d, sizeof(d), "%s/rsos", c->data_dir);
			mkdirs(d);
		}
		n = snprintf(body, sizeof(body),
			     "# rsos-update: an installed update, until its first confirmed boot\n"
			     "version=%s\nslot=%c\nfrom_version=%s\nfrom_slot=%c\ninstalled=%lld\nfile=%s\nannounced=0\n",
			     h->m.version, c->target_slot, c->sys.version, c->booted, (long long)time(NULL),
			     downloaded ? path : "");
		if (n > 0 && (size_t)n < sizeof(body) && write_file_atomic(sp, body, (size_t)n) < 0)
			upd_logf("warning: %s: %s (the \"updated\" message will not show)", sp, strerror(errno));
	}
	if (c->progress)
		c->progress(c, "switch", 1, 1);
	upd_logf("installed %s in slot %c: restart to use it", h->m.version, c->target_slot);
out:
	if (tfd >= 0)
		close(tfd);
	if (pfd >= 0)
		close(pfd);
	free(buf);
	free(out);
	if (lock >= 0)
		close(lock);
	return e;
}

/* ----------------------------------------------------------------- boot */
static void state_value(const char *t, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);

	out[0] = 0;
	for (const char *p = t; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t l = strcspn(p + kl + 1, "\n");

			if (l >= n)
				l = n - 1;
			memcpy(out, p + kl + 1, l);
			out[l] = 0;
			return;
		}
}

/* a downloaded package is only removed from our own download folder */
static void remove_download(struct upd_ctx *c, const char *file)
{
	char dir[PATH_MAX];
	size_t l;

	snprintf(dir, sizeof(dir), "%s/rsos/update/", c->data_dir);
	l = strlen(dir);
	if (!file[0] || strncmp(file, dir, l) || strchr(file + l, '/') || strstr(file, ".."))
		return;
	if (unlink(file) == 0)
		upd_logf("removed %s", file);
}

enum rsu_err upd_boot(struct upd_ctx *c, char *event, size_t n, char *version, size_t vn)
{
	char sp[PATH_MAX], slot[8], file[PATH_MAX], ann[8], bs[64] = "", ok[16] = "", envslot[16] = "";
	char *t, *bst;

	cpy(event, "none", n);
	version[0] = 0;
	upd_state_path(c, sp, sizeof(sp));
	t = slurp(sp, 65536, NULL);
	if (!t)
		return RSU_OK;
	state_value(t, "version", version, vn);
	state_value(t, "slot", slot, sizeof(slot));
	state_value(t, "file", file, sizeof(file));
	state_value(t, "announced", ann, sizeof(ann));
	{
		char p[PATH_MAX];

		snprintf(p, sizeof(p), "%s/boot-state", c->run_dir);
		bst = slurp(p, 256, NULL);
		if (bst) {
			sscanf(bst, "%63s", bs);
			free(bst);
		}
	}
	upd_env_get(c, "rsos_ok", ok, sizeof(ok));
	upd_env_get(c, "rsos_slot", envslot, sizeof(envslot));
	if (c->booted && slot[0] == c->booted) {
		/* the new system runs */
		if ((!strcmp(bs, "confirmed") || !strcmp(bs, "ok")) && !strcmp(ok, "1")) {
			remove_download(c, file);
			unlink(sp);
			fsync_dir(sp);
			cpy(event, strcmp(ann, "1") ? "updated" : "confirmed", n);
			upd_logf("version %s confirmed in slot %c: clean-up done", version, c->booted);
		} else if (strcmp(ann, "1")) {
			/* first boot of the new version: say so once */
			char *a = strstr(t, "\nannounced=0");

			if (a) {
				a[11] = '1';
				if (write_file_atomic(sp, t, strlen(t)) < 0)
					upd_logf("warning: %s: %s", sp, strerror(errno));
			}
			cpy(event, "updated", n);
		} else {
			cpy(event, "updated-waiting", n);
		}
	} else if (envslot[0] == slot[0] && !strcmp(ok, "0")) {
		cpy(event, "pending", n);             /* installed, the restart is still to come */
	} else {
		/* the other slot runs although the new one was selected: U-Boot
		 * fell back (the new system did not start three times) */
		upd_logf("version %s in slot %s did not start: slot %c runs", version, slot, c->booted ? c->booted : '?');
		remove_download(c, file);
		unlink(sp);
		fsync_dir(sp);
		cpy(event, "failed", n);
	}
	free(t);
	return RSU_OK;
}
