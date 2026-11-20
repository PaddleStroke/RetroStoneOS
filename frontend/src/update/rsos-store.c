/*
 * rsos-store - the game store helper (docs/store-design.md). The menu runs it as
 * a helper process (like rsos-covers / rsos-update) and reads its lines:
 *
 *   rsos-store [options] list [SYSTEM]       the signed catalog (online, else the cached copy)
 *   rsos-store [options] get ID              download, verify, install
 *   rsos-store [options] remove ID           delete what the store installed for ID
 *   rsos-store [options] remove-path PATH    the same for the game a file belongs to (the menu's Delete
 *                                            of a launcher; error code=notstore if no game owns it)
 *   rsos-store [options] installed           what is installed
 *   rsos-store [options] find-rom ID         the player's ROM for a port (by SHA-1), if there is one
 *   rsos-store [options] install-port ID     download, make the game data from the ROM, install
 *
 * Options: --roms DIR (default /data/roms)  --state DIR (/data/rsos/store)
 *          --pubkey FILE (/usr/share/rsos/store.pub)  --url URL | --catalog FILE (local, tests)
 *          --arch NAME (armhf)  --allow-http  --cover-dir DIR  --max-bytes N
 *          --scan DIR (more places to look for ROMs, repeatable)  --rom FILE (a ROM to use, install-port)
 *          --out FILE (the lines go there, not to stdout: the menu reads the file)
 *
 * What is installed is recorded in <state>/installed.tsv for the console's own roms folder (/data/roms). For
 * another --roms (a test install into a scratch folder) the record is <roms>/.rsos-store/installed.tsv,
 * so that install never shows up in the menu's store as installed, nor a remove there touches it.
 *
 * Output: lines "type<TAB>key=value<TAB>..." (\\ \t \n escaped), the same shape as rsos-update --machine:
 *   entry  id name system version author license desc size kind installed update
 *   progress phase=download|verify|install done total
 *   done / ok / error code=... text=...
 *
 * Nothing is trusted before the catalog's Ed25519 signature checks out, and
 * nothing reaches the games folders before the size and SHA-256 of the
 * download match the signed catalog entry.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

#include "http.h"
#include "json.h"
#include "rsu.h"
#include "sha1.h"
#include "sha256.h"

#ifndef STORE_URL
#define STORE_URL "https://raw.githubusercontent.com/KurdtNervanna/RetroStoneOS-Store/main/catalog.json"
#endif
#ifndef STORE_ARCH
#define STORE_ARCH "armhf"
#endif

#define CATALOG_MAX (4 << 20)
#define COVER_MAX (2 << 20)
#define NAME_MAX_LEN 120

struct cfg {
	char roms[512], state[512], record[1100], pubkey[512], url[1024], catalog_file[512], arch[32], cover_dir[512];
	bool allow_http;
	int64_t max_bytes;
	char scan[4][512];
	int nscan;
	char rom_arg[512];
};

static struct cfg C;
static volatile int g_cancel;

static void on_term(int sig)
{
	(void)sig;
	g_cancel = 1;
}

/* ------------------------------------------------------------- output */
static void field(const char *k, const char *v)
{
	printf("\t%s=", k);
	for (const char *p = v ? v : ""; *p; p++) {
		if (*p == '\\')
			fputs("\\\\", stdout);
		else if (*p == '\t')
			fputs("\\t", stdout);
		else if (*p == '\n')
			fputs("\\n", stdout);
		else if (*p != '\r')
			fputc(*p, stdout);
	}
}

static void fieldf(const char *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fieldf(const char *k, const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	field(k, b);
}

static void endl(void)
{
	fputc('\n', stdout);
	fflush(stdout);
}

static int fail(const char *code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(const char *code, const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	printf("error");
	field("code", code);
	field("text", b);
	endl();
	return 1;
}

static void progress(const char *phase, uint64_t done, uint64_t total)
{
	static time_t last;
	time_t now = time(NULL);

	if (done != total && now == last)
		return;
	last = now;
	printf("progress");
	field("phase", phase);
	fieldf("done", "%" PRIu64, done);
	fieldf("total", "%" PRIu64, total);
	endl();
}

/* ------------------------------------------------------------ helpers */
static void cpy(char *d, size_t n, const char *s)
{
	snprintf(d, n, "%s", s ? s : "");
}

static int mkdir_p(const char *path)
{
	char tmp[1024], *p;

	cpy(tmp, sizeof(tmp), path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/* a file name for the games folder: no path, no leading dot, plain characters */
static bool safe_name(const char *s)
{
	size_t n = s ? strlen(s) : 0;

	if (n == 0 || n > NAME_MAX_LEN || s[0] == '.')
		return false;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
		    c == '<' || c == '>' || c == '|' || c == 0x7f)
			return false;
	}
	return true;
}

/* a relative archive path: every component safe, no "..", no absolute path */
static bool safe_relpath(const char *s)
{
	char tmp[512], *p, *q;

	if (!s || !*s || strlen(s) >= sizeof(tmp) || s[0] == '/')
		return false;
	cpy(tmp, sizeof(tmp), s);
	for (p = tmp; p; p = q) {
		q = strchr(p, '/');
		if (q)
			*q++ = 0;
		if (!*p) {
			if (q)
				return false;   /* "a//b" */
			break;                  /* trailing slash */
		}
		if (!strcmp(p, ".") || !strcmp(p, "..") || !safe_name(p))
			return false;
	}
	return true;
}

static int read_all(const char *path, char **out, size_t *len, size_t max)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *b;

	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) < 0 || (n = ftell(f)) < 0 || (size_t)n > max || fseek(f, 0, SEEK_SET) < 0) {
		fclose(f);
		return -1;
	}
	b = malloc((size_t)n + 1);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return -1;
	}
	fclose(f);
	b[n] = 0;
	*out = b;
	*len = (size_t)n;
	return 0;
}

static int write_atomic(const char *path, const void *data, size_t n)
{
	char tmp[1100];
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (write(fd, data, n) != (ssize_t)n || fsync(fd) < 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	close(fd);
	return rename(tmp, path);
}

static void http_opts(struct http_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->timeout_ms = 30000;
	o->max_redirects = 5;
	o->allow_http = C.allow_http;
	o->user_agent = "RetroStoneOS-store";
	o->cancel = &g_cancel;
}

/* ---------------------------------------------------------------- fetch */
struct membuf {
	char *b;
	size_t len, cap, max;
};

static int mem_data(const void *data, size_t n, void *user)
{
	struct membuf *m = user;

	if (m->len + n > m->max)
		return -1;
	if (m->len + n + 1 > m->cap) {
		size_t c = (m->len + n + 1) * 2;
		char *nb = realloc(m->b, c);

		if (!nb)
			return -1;
		m->b = nb;
		m->cap = c;
	}
	memcpy(m->b + m->len, data, n);
	m->len += n;
	m->b[m->len] = 0;
	return 0;
}

static int fetch_mem(const char *url, struct membuf *m, size_t max, char *err, size_t en)
{
	struct http_opts o;
	struct http_resp r;

	http_opts(&o);
	memset(m, 0, sizeof(*m));
	m->max = max;
	if (http_get(&o, url, 0, -1, NULL, mem_data, m, &r, err, en) != RSU_OK) {
		free(m->b);
		m->b = NULL;
		return -1;
	}
	if (!m->b)
		m->b = calloc(1, 1);
	return 0;
}

/* ------------------------------------------------------------- catalog */
struct catalog {
	char *text;                     /* the verified catalog.json, owned */
	size_t len;
	struct json *root;
	const struct json *entries;
	bool cached;                    /* the offline copy was used */
};

static void catalog_free(struct catalog *c)
{
	json_free(c->root);
	free(c->text);
	memset(c, 0, sizeof(*c));
}

static int verify_catalog(const char *text, size_t len, const char *sigtext, size_t siglen)
{
	struct rsu_pubkey pk;
	struct rsu_sig sg;

	if (rsu_pubkey_load(C.pubkey, &pk) < 0)
		return -1;
	if (rsu_sig_parse(sigtext, siglen, &sg) < 0)
		return -1;
	return rsu_verify(&pk, &sg, text, len) ? 0 : -1;
}

static int catalog_open(struct catalog *c, char *err, size_t en)
{
	char cpath[1100], spath[1100], err2[200] = "";
	struct membuf cat = { 0 }, sig = { 0 };
	char *text = NULL, *st = NULL;
	size_t tl = 0, sl = 0;

	memset(c, 0, sizeof(*c));
	snprintf(cpath, sizeof(cpath), "%s/catalog.json", C.state);
	snprintf(spath, sizeof(spath), "%s/catalog.json.sig", C.state);

	if (C.catalog_file[0]) {
		char sf[600];

		snprintf(sf, sizeof(sf), "%s.sig", C.catalog_file);
		if (read_all(C.catalog_file, &text, &tl, CATALOG_MAX) < 0 || read_all(sf, &st, &sl, 4096) < 0) {
			snprintf(err, en, "cannot read %s or its .sig", C.catalog_file);
			free(text);
			free(st);
			return -1;
		}
	} else {
		char url[1100];

		snprintf(url, sizeof(url), "%s", C.url);
		if (fetch_mem(url, &cat, CATALOG_MAX, err2, sizeof(err2)) == 0) {
			snprintf(url, sizeof(url), "%s.sig", C.url);
			if (fetch_mem(url, &sig, 4096, err2, sizeof(err2)) == 0) {
				text = cat.b;
				tl = cat.len;
				st = sig.b;
				sl = sig.len;
				cat.b = sig.b = NULL;
			}
		}
		free(cat.b);
		free(sig.b);
		if (!text) {
			/* offline: the last verified copy */
			if (read_all(cpath, &text, &tl, CATALOG_MAX) < 0 || read_all(spath, &st, &sl, 4096) < 0) {
				snprintf(err, en, "no network and no saved catalog (%s)", err2);
				free(text);
				free(st);
				return -1;
			}
			c->cached = true;
		}
	}
	if (verify_catalog(text, tl, st, sl) < 0) {
		snprintf(err, en, "the catalog's signature is not valid");
		free(text);
		free(st);
		return -1;
	}
	if (!c->cached && !C.catalog_file[0] && mkdir_p(C.state) == 0) {
		write_atomic(cpath, text, tl);
		write_atomic(spath, st, sl);
	}
	free(st);
	c->text = text;
	c->len = tl;
	c->root = json_parse(text, tl, err, en);
	if (!c->root) {
		catalog_free(c);
		return -1;
	}
	if ((int)json_num(c->root, "format", 0) != 1) {
		snprintf(err, en, "unknown catalog format");
		catalog_free(c);
		return -1;
	}
	c->entries = json_get(c->root, "entries");
	if (!c->entries || c->entries->type != JSON_ARR) {
		snprintf(err, en, "the catalog has no entries");
		catalog_free(c);
		return -1;
	}
	return 0;
}

enum kind { K_ROM, K_TAR, K_NATIVE, K_PORT };

/* A port made from the player's own ROM (kind "port"): which ROM, and what to do with it. */
struct rominfo {
	char name[96], mode[8], dest[96], copy_as[96];
	char sha1[6][41];
	int nsha;
	int64_t size[4];
	int nsize;
	char output[4][96];
	int nout;
	char system[2][32];
	int nsys;
	int need_ram_mb, est_min, need_disk_mb;
};

struct entry {
	const char *id, *name, *system, *version, *author, *license, *desc, *url, *sha, *file, *cover, *launcher;
	enum kind kind;
	int64_t size, unpacked;
	struct rominfo rom;
};


/* the "rom" object of a port entry; false when anything the install needs is missing or unsafe */
static bool rom_parse(const struct json *j, struct rominfo *r)
{
	const struct json *o = json_get(j, "rom"), *a;
	const char *m;

	memset(r, 0, sizeof(*r));
	if (!o || o->type != JSON_OBJ)
		return false;
	cpy(r->name, sizeof(r->name), json_str(o, "name"));
	m = json_str(o, "mode");
	if (!m || (strcmp(m, "extract") && strcmp(m, "copy")))
		return false;
	cpy(r->mode, sizeof(r->mode), m);
	cpy(r->dest, sizeof(r->dest), json_str(o, "dest"));
	if (!safe_relpath(r->dest))
		return false;
	a = json_get(o, "sha1");
	for (const struct json *x = a && a->type == JSON_ARR ? a->child : NULL; x && r->nsha < 6; x = x->next) {
		if (x->type != JSON_STR || !x->str || strlen(x->str) != 40)
			return false;
		for (int i = 0; i < 40; i++) {
			if (!isxdigit((unsigned char)x->str[i]))
				return false;
			r->sha1[r->nsha][i] = (char)tolower((unsigned char)x->str[i]);
		}
		r->sha1[r->nsha][40] = 0;
		r->nsha++;
	}
	if (!r->nsha)
		return false;
	a = json_get(o, "size");
	for (const struct json *x = a && a->type == JSON_ARR ? a->child : NULL; x && r->nsize < 4; x = x->next)
		if (x->type == JSON_NUM && x->num > 0)
			r->size[r->nsize++] = (int64_t)x->num;
	a = json_get(o, "output");
	for (const struct json *x = a && a->type == JSON_ARR ? a->child : NULL; x && r->nout < 4; x = x->next) {
		if (x->type != JSON_STR || !safe_relpath(x->str))
			return false;
		cpy(r->output[r->nout++], sizeof(r->output[0]), x->str);
	}
	if (!strcmp(r->mode, "copy")) {
		cpy(r->copy_as, sizeof(r->copy_as), json_str(o, "copy_as"));
		if (!safe_relpath(r->copy_as))
			return false;
		cpy(r->output[0], sizeof(r->output[0]), r->copy_as);
		r->nout = 1;
	} else if (!r->nout) {
		return false;
	}
	a = json_get(o, "systems");
	for (const struct json *x = a && a->type == JSON_ARR ? a->child : NULL; x && r->nsys < 2; x = x->next)
		if (x->type == JSON_STR && rsu_safe_token(x->str))
			cpy(r->system[r->nsys++], sizeof(r->system[0]), x->str);
	if (!r->nsys)
		cpy(r->system[r->nsys++], sizeof(r->system[0]), "n64");
	r->need_ram_mb = (int)json_num(o, "needs_ram_mb", 0);
	r->est_min = (int)json_num(o, "est_minutes", 5);
	r->need_disk_mb = (int)json_num(o, "needs_disk_mb", 400);
	if (r->est_min < 1)
		r->est_min = 1;
	return true;
}

/* A usable entry: every field the install needs, safely spelled. */
static bool entry_parse(const struct json *j, struct entry *e)
{
	const char *k;

	memset(e, 0, sizeof(*e));
	e->id = json_str(j, "id");
	e->name = json_str(j, "name");
	e->system = json_str(j, "system");
	e->version = json_str(j, "version");
	e->author = json_str(j, "author");
	e->license = json_str(j, "license");
	e->desc = json_str(j, "desc");
	e->url = json_str(j, "url");
	e->sha = json_str(j, "sha256");
	e->file = json_str(j, "file");
	e->cover = json_str(j, "cover");
	e->launcher = json_str(j, "launcher");
	e->size = (int64_t)json_num(j, "size", -1);
	e->unpacked = (int64_t)json_num(j, "unpacked", (double)e->size);
	k = json_str(j, "kind");
	if (!e->id || !e->name || !e->system || !e->version || !e->url || !e->sha || !k || !e->license)
		return false;
	if (!rsu_safe_token(e->id) || !rsu_safe_token(e->system) || !rsu_safe_token(e->version))
		return false;
	if (e->size <= 0 || e->unpacked <= 0 || strlen(e->sha) != 64)
		return false;
	if (!strcmp(k, "rom")) {
		e->kind = K_ROM;
		if (!safe_name(e->file))
			return false;
	} else if (!strcmp(k, "tar")) {
		e->kind = K_TAR;
	} else if (!strcmp(k, "native")) {
		e->kind = K_NATIVE;
		if (!safe_name(e->launcher))
			return false;
		/* the code runs on the console: only for this CPU */
		if (!json_str(j, "arch") || strcmp(json_str(j, "arch"), C.arch))
			return false;
	} else if (!strcmp(k, "port")) {
		e->kind = K_PORT;
		if (!json_str(j, "arch") || strcmp(json_str(j, "arch"), C.arch))
			return false;
		if (!rom_parse(j, &e->rom))
			return false;
	} else {
		return false;
	}
	return true;
}

static const struct json *catalog_find(const struct catalog *c, const char *id, struct entry *e)
{
	for (const struct json *j = c->entries->child; j; j = j->next) {
		if (j->type == JSON_OBJ && json_str(j, "id") && !strcmp(json_str(j, "id"), id))
			return entry_parse(j, e) ? j : NULL;
	}
	return NULL;
}

/* ------------------------------------------------------ installed record */
/* installed.tsv: "id<TAB>version<TAB>relative path" per top-level path installed */
static void installed_path(char *out, size_t n)
{
	cpy(out, n, C.record);
}

/* the folder of the record (made before it is written) */
static int record_dir(void)
{
	char d[1100], *sl;

	cpy(d, sizeof(d), C.record);
	sl = strrchr(d, '/');
	if (sl && sl != d)
		*sl = 0;
	return mkdir_p(d);
}

/* --roms names the console's own roms folder (the default), whatever its spelling */
static bool roms_is_default(void)
{
	char a[PATH_MAX], b[PATH_MAX];
	size_t l;

	if (realpath(C.roms, a) && realpath("/data/roms", b))
		return !strcmp(a, b);
	cpy(a, sizeof(a), C.roms);
	l = strlen(a);
	while (l > 1 && a[l - 1] == '/')
		a[--l] = 0;
	return !strcmp(a, "/data/roms");
}

static bool installed_version(const char *id, char *ver, size_t vn)
{
	char path[1100], line[1500];
	FILE *f;
	bool found = false;

	installed_path(path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char *a = strtok(line, "\t"), *b = strtok(NULL, "\t");

		if (a && b && !strcmp(a, id)) {
			cpy(ver, vn, b);
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Rewrites the record without ID's lines, then adds the new ones. */
static int installed_update(const char *id, const char *version, char **paths, int np)
{
	char path[1100], line[1500];
	char *out = NULL;
	size_t len = 0, cap = 0;
	FILE *f;
	int rc;

	installed_path(path, sizeof(path));
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char cp[1500], *a;
			size_t l = strlen(line);

			cpy(cp, sizeof(cp), line);
			a = strtok(cp, "\t");
			if (a && !strcmp(a, id))
				continue;
			if (len + l + 1 > cap) {
				cap = (len + l + 1) * 2;
				out = realloc(out, cap);
				if (!out) {
					fclose(f);
					return -1;
				}
			}
			memcpy(out + len, line, l);
			len += l;
		}
		fclose(f);
	}
	for (int i = 0; i < np; i++) {
		char l[1500];
		int ln = snprintf(l, sizeof(l), "%s\t%s\t%s\n", id, version, paths[i]);

		if (len + (size_t)ln + 1 > cap) {
			cap = (len + (size_t)ln + 1) * 2;
			out = realloc(out, cap);
			if (!out)
				return -1;
		}
		memcpy(out + len, l, (size_t)ln);
		len += (size_t)ln;
	}
	if (record_dir() < 0) {
		free(out);
		return -1;
	}
	rc = write_atomic(path, out ? out : "", len);
	free(out);
	return rc;
}

static void rm_tree(const char *path)
{
	struct stat st;

	if (lstat(path, &st) < 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *de;

		if (d) {
			while ((de = readdir(d))) {
				char sub[1100];

				if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
					continue;
				snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
				rm_tree(sub);
			}
			closedir(d);
		}
		rmdir(path);
	} else {
		unlink(path);
	}
}

/* remove ID's files; returns how many top-level paths went */
static int installed_remove(const char *id)
{
	char path[1100], line[1500];
	FILE *f;
	int n = 0;

	installed_path(path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *p = strtok(NULL, "\n");
		char full[1100];

		if (!a || !b || !p || strcmp(a, id) || !safe_relpath(p))
			continue;
		snprintf(full, sizeof(full), "%s/%s", C.roms, p);
		rm_tree(full);
		/* a port's files are recorded one by one: the folders the store made go when they are empty
		 * (saves and settings the game wrote keep theirs) */
		for (char *sl = strrchr(p, '/'); sl; sl = strrchr(p, '/')) {
			*sl = 0;
			snprintf(full, sizeof(full), "%s/%s", C.roms, p);
			if (rmdir(full) < 0)
				break;
		}
		n++;
	}
	fclose(f);
	installed_update(id, "", NULL, 0);
	return n;
}

/* --------------------------------------------------------------- list */
static int cmd_list(const char *system)
{
	struct catalog c;
	char err[256], iv[64];
	int count = 0;

	if (catalog_open(&c, err, sizeof(err)) < 0)
		return fail("catalog", "%s", err);
	for (const struct json *j = c.entries->child; j; j = j->next) {
		struct entry e;
		bool inst;

		if (j->type != JSON_OBJ || !entry_parse(j, &e))
			continue;
		if (system && strcmp(system, e.system))
			continue;
		inst = installed_version(e.id, iv, sizeof(iv));
		printf("entry");
		field("id", e.id);
		field("name", e.name);
		field("system", e.system);
		field("version", e.version);
		field("author", e.author);
		field("license", e.license);
		field("desc", e.desc);
		fieldf("size", "%" PRId64, e.size);
		field("kind", e.kind == K_ROM ? "rom" : e.kind == K_TAR ? "tar" : e.kind == K_PORT ? "port" : "native");
		field("installed", inst ? iv : "");
		field("update", inst && rsu_version_cmp(e.version, 0, iv, 0) > 0 ? "1" : "0");
		if (e.kind == K_PORT) {
			field("rom", e.rom.name);
			field("mode", e.rom.mode);
			fieldf("minutes", "%d", e.rom.est_min);
		}
		endl();
		count++;
	}
	printf("done");
	fieldf("count", "%d", count);
	field("source", c.cached ? "cache" : "net");
	endl();
	catalog_free(&c);
	return 0;
}

static int cmd_installed(void)
{
	char path[1100], line[1500], last[128] = "";
	FILE *f;
	int n = 0;

	installed_path(path, sizeof(path));
	f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char *a = strtok(line, "\t"), *b = strtok(NULL, "\t");

			if (a && b && strcmp(a, last)) {
				printf("installed");
				field("id", a);
				field("version", b);
				endl();
				cpy(last, sizeof(last), a);
				n++;
			}
		}
		fclose(f);
	}
	printf("done");
	fieldf("count", "%d", n);
	endl();
	return 0;
}

/* ----------------------------------------------------------- download */
struct dl {
	int fd;
	struct sha256 sha;
	int64_t got, max, total;
	int status;
};

static int dl_head(const struct http_resp *r, void *user)
{
	struct dl *d = user;

	d->status = r->status;
	return 0;
}

static int dl_data(const void *data, size_t n, void *user)
{
	struct dl *d = user;
	const uint8_t *p = data;

	if (d->got + (int64_t)n > d->max)
		return -1;              /* more than the signed size: stop */
	while (n) {
		ssize_t w = write(d->fd, p, n);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		sha256_update(&d->sha, p, (size_t)w);
		d->got += w;
		p += w;
		n -= (size_t)w;
	}
	progress("download", (uint64_t)d->got, (uint64_t)d->total);
	return g_cancel ? -1 : 0;
}

static int download(const char *url, const char *dest, int64_t size, uint8_t digest[32], char *err, size_t en)
{
	struct http_opts o;
	struct http_resp r;
	struct dl d;
	enum rsu_err rc;

	memset(&d, 0, sizeof(d));
	d.fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (d.fd < 0) {
		snprintf(err, en, "cannot write %s: %s", dest, strerror(errno));
		return -1;
	}
	d.max = size;
	d.total = size;
	sha256_init(&d.sha);
	http_opts(&o);
	rc = http_get(&o, url, 0, -1, dl_head, dl_data, &d, &r, err, en);
	if (close(d.fd) < 0 && rc == RSU_OK)
		rc = RSU_E_IO;
	if (rc != RSU_OK) {
		if (!*err)
			snprintf(err, en, "download failed (%s)", rsu_err_code(rc));
		unlink(dest);
		return g_cancel ? -2 : -1;
	}
	if (d.got != size) {
		snprintf(err, en, "the download is %" PRId64 " bytes, the catalog says %" PRId64, d.got, size);
		unlink(dest);
		return -1;
	}
	sha256_final(&d.sha, digest);
	return 0;
}

/* ------------------------------------------------------------ tar unpack */
static uint64_t tar_num(const char *p, size_t n)
{
	uint64_t v = 0;

	if ((unsigned char)p[0] & 0x80) {   /* base-256 */
		v = (unsigned char)p[0] & 0x7f;
		for (size_t i = 1; i < n; i++)
			v = (v << 8) | (unsigned char)p[i];
		return v;
	}
	for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++)
		v = (v << 3) | (uint64_t)(p[i] - '0');
	return v;
}

/*
 * Unpack the plain ustar archive FILE into DIR. Regular files and directories only; every name
 * must be a safe relative path; the unpacked bytes may not exceed LIMIT. Top-level names are
 * collected into *tops (malloc'd strings).
 */
static int untar(const char *file, const char *dir, int64_t limit, char ***tops, int *ntops, char *err, size_t en)
{
	FILE *f = fopen(file, "rb");
	uint8_t blk[512];
	int64_t total = 0;
	int cap = 0;

	*tops = NULL;
	*ntops = 0;
	if (!f) {
		snprintf(err, en, "cannot open the archive");
		return -1;
	}
	while (fread(blk, 1, 512, f) == 512) {
		char name[300], path[1200];
		uint64_t size;
		char type = (char)blk[156];
		bool zero = true;

		for (int i = 0; i < 512; i++)
			if (blk[i]) {
				zero = false;
				break;
			}
		if (zero)
			break;
		if (memcmp(blk + 257, "ustar", 5)) {
			snprintf(err, en, "not a ustar archive");
			goto bad;
		}
		memcpy(name, blk, 100);
		name[100] = 0;
		if (blk[345]) {            /* prefix field */
			char pre[156];

			memcpy(pre, blk + 345, 155);
			pre[155] = 0;
			snprintf(name, sizeof(name), "%s/%.100s", pre, (char *)blk);
		}
		size = tar_num((const char *)blk + 124, 12);
		if (!safe_relpath(name)) {
			snprintf(err, en, "unsafe path in the archive");
			goto bad;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (type == '5') {
			if (mkdir_p(path) < 0) {
				snprintf(err, en, "cannot create a folder");
				goto bad;
			}
		} else if (type == '0' || type == 0) {
			int fd;
			uint64_t left = size;
			char *slash;

			total += (int64_t)size;
			if (total > limit || size > (uint64_t)limit) {
				snprintf(err, en, "the archive unpacks to more than the catalog says");
				goto bad;
			}
			slash = strrchr(path, '/');
			*slash = 0;
			mkdir_p(path);
			*slash = '/';
			fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
			if (fd < 0) {
				snprintf(err, en, "cannot create %s", name);
				goto bad;
			}
			while (left) {
				uint8_t buf[8192];
				size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);

				if (fread(buf, 1, want, f) != want || write(fd, buf, want) != (ssize_t)want) {
					close(fd);
					snprintf(err, en, "the archive is cut short");
					goto bad;
				}
				left -= want;
			}
			close(fd);
			if (size % 512)
				fseek(f, (long)(512 - size % 512), SEEK_CUR);
		} else {
			snprintf(err, en, "the archive holds a link or special file");
			goto bad;
		}
		/* top-level name */
		{
			char top[300], *s;
			bool have = false;

			cpy(top, sizeof(top), name);
			s = strchr(top, '/');
			if (s)
				*s = 0;
			for (int i = 0; i < *ntops; i++)
				if (!strcmp((*tops)[i], top))
					have = true;
			if (!have) {
				if (*ntops == cap) {
					cap = cap ? cap * 2 : 8;
					*tops = realloc(*tops, (size_t)cap * sizeof(char *));
				}
				(*tops)[(*ntops)++] = strdup(top);
			}
		}
	}
	fclose(f);
	if (!*ntops) {
		snprintf(err, en, "the archive is empty");
		return -1;
	}
	return 0;
bad:
	fclose(f);
	return -1;
}


/* ------------------------------------------------- zstd-compressed packages */
/* A package may be a .tar.zst (the downloads are much smaller): the same SHA-256 and size of the file as
 * downloaded; unpacked to a plain tar next to it, within the catalog's unpacked size (plus the tar headers). */
static bool is_zstd(const char *path)
{
	uint8_t m[4];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	bool r;

	if (fd < 0)
		return false;
	r = read(fd, m, 4) == 4 && m[0] == 0x28 && m[1] == 0xb5 && m[2] == 0x2f && m[3] == 0xfd;
	close(fd);
	return r;
}

static int unzstd(const char *src, const char *dst, int64_t limit, char *err, size_t en)
{
	static uint8_t in_buf[1 << 16], out_buf[1 << 17];
	int in = open(src, O_RDONLY | O_CLOEXEC), out;
	ZSTD_DCtx *z = ZSTD_createDCtx();
	int64_t total = 0;
	int rc = -1;
	ssize_t r;

	if (in < 0 || !z) {
		snprintf(err, en, "cannot unpack the package");
		goto done;
	}
	ZSTD_DCtx_setParameter(z, ZSTD_d_windowLogMax, 25);
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		snprintf(err, en, "cannot write %s", dst);
		goto done;
	}
	while ((r = read(in, in_buf, sizeof(in_buf))) > 0) {
		ZSTD_inBuffer ib = { in_buf, (size_t)r, 0 };

		while (ib.pos < ib.size) {
			ZSTD_outBuffer ob = { out_buf, sizeof(out_buf), 0 };
			size_t zr = ZSTD_decompressStream(z, &ob, &ib);

			if (ZSTD_isError(zr)) {
				snprintf(err, en, "the package is damaged (%s)", ZSTD_getErrorName(zr));
				close(out);
				goto done;
			}
			total += (int64_t)ob.pos;
			if (total > limit) {
				snprintf(err, en, "the package unpacks to more than the catalog says");
				close(out);
				goto done;
			}
			if (ob.pos && write(out, out_buf, ob.pos) != (ssize_t)ob.pos) {
				snprintf(err, en, "cannot write %s", dst);
				close(out);
				goto done;
			}
		}
		if (g_cancel) {
			snprintf(err, en, "cancelled");
			close(out);
			goto done;
		}
	}
	if (close(out) < 0 || r < 0) {
		snprintf(err, en, "cannot write %s", dst);
		goto done;
	}
	rc = 0;
done:
	if (z)
		ZSTD_freeDCtx(z);
	if (in >= 0)
		close(in);
	if (rc < 0)
		unlink(dst);
	return rc;
}

/* untar() of a plain or zstd-compressed tar */
static int untar_any(const char *file, const char *dir, int64_t limit, char ***tops, int *ntops, char *err, size_t en)
{
	char plain[2200];
	int rc;

	if (!is_zstd(file))
		return untar(file, dir, limit, tops, ntops, err, en);
	snprintf(plain, sizeof(plain), "%s.tar", file);
	if (unzstd(file, plain, limit + limit / 8 + (4 << 20), err, en) < 0)
		return -1;
	rc = untar(plain, dir, limit, tops, ntops, err, en);
	unlink(plain);
	return rc;
}

static void free_tops(char **tops, int n)
{
	for (int i = 0; i < n; i++)
		free(tops[i]);
	free(tops);
}

/* ---------------------------------------------------------------- get */
static int cmd_get(const char *id)
{
	struct catalog c;
	struct entry e;
	char err[300] = "", stage[2048], part[2048], target[2048], iv[64], hex[65];
	uint8_t want[32], got[32];
	char **tops = NULL, *single[1];
	int ntops = 0, rc;
	struct statvfs vf;

	if (catalog_open(&c, err, sizeof(err)) < 0)
		return fail("catalog", "%s", err);
	if (!catalog_find(&c, id, &e)) {
		catalog_free(&c);
		return fail("notfound", "no usable entry %s in the catalog", id);
	}
	if (sha256_parse_hex(e.sha, want) < 0) {
		catalog_free(&c);
		return fail("catalog", "bad hash for %s", id);
	}
	if (C.max_bytes > 0 && e.size > C.max_bytes) {
		catalog_free(&c);
		return fail("size", "%s is larger than allowed", id);
	}

	mkdir_p(C.roms);
	/* room for the download and its unpacked copy, plus a margin */
	if (statvfs(C.roms, &vf) == 0 &&
	    (int64_t)vf.f_bavail * (int64_t)vf.f_frsize < e.size + e.unpacked + (4 << 20)) {
		catalog_free(&c);
		return fail("space", "not enough free space for %s", e.name);
	}

	snprintf(stage, sizeof(stage), "%s/.store-%s", C.roms, e.id);
	snprintf(part, sizeof(part), "%s/.store-%s.part", C.roms, e.id);
	rm_tree(stage);
	unlink(part);

	rc = download(e.url, part, e.size, got, err, sizeof(err));
	if (rc < 0) {
		catalog_free(&c);
		return fail(rc == -2 ? "cancelled" : "download", "%s", err);
	}
	progress("verify", 1, 1);
	sha256_hex(got, hex);
	if (memcmp(got, want, 32)) {
		unlink(part);
		catalog_free(&c);
		return fail("hash", "the download does not match the catalog's SHA-256");
	}

	progress("install", 0, 1);
	if (e.kind == K_ROM) {
		/* DIR/<system>/<file>, replacing the store's own earlier copy */
		char dir[2048], rel[600];

		snprintf(dir, sizeof(dir), "%s/%s", C.roms, e.system);
		mkdir_p(dir);
		snprintf(rel, sizeof(rel), "%s/%s", e.system, e.file);
		snprintf(target, sizeof(target), "%s/%s", C.roms, rel);
		if (access(target, F_OK) == 0 && !installed_version(e.id, iv, sizeof(iv))) {
			unlink(part);
			catalog_free(&c);
			return fail("exists", "%s already exists and is not from the store", e.file);
		}
		if (rename(part, target) < 0) {
			unlink(part);
			catalog_free(&c);
			return fail("io", "cannot place %s: %s", e.file, strerror(errno));
		}
		single[0] = rel;
		installed_update(e.id, e.version, single, 1);
	} else {
		const char *sub = e.kind == K_NATIVE ? "native" : e.system;
		char dir[2048];

		mkdir_p(stage);
		if (untar_any(part, stage, e.unpacked, &tops, &ntops, err, sizeof(err)) < 0) {
			rm_tree(stage);
			unlink(part);
			free_tops(tops, ntops);
			catalog_free(&c);
			return fail("archive", "%s", err);
		}
		unlink(part);
		snprintf(dir, sizeof(dir), "%s/%s", C.roms, sub);
		mkdir_p(dir);
		/* refuse to overwrite anything the store did not put there */
		bool mine = installed_version(e.id, iv, sizeof(iv));

		for (int i = 0; i < ntops; i++) {
			snprintf(target, sizeof(target), "%s/%s", dir, tops[i]);
			if (access(target, F_OK) == 0 && !mine) {
				rm_tree(stage);
				fail("exists", "%s already exists and is not from the store", tops[i]);
				free_tops(tops, ntops);
				catalog_free(&c);
				return 1;
			}
		}
		if (mine)
			installed_remove(e.id);
		char **rels = calloc((size_t)ntops, sizeof(char *));

		for (int i = 0; i < ntops; i++) {
			char from[2048], rel[600];

			snprintf(from, sizeof(from), "%s/%s", stage, tops[i]);
			snprintf(target, sizeof(target), "%s/%s", dir, tops[i]);
			snprintf(rel, sizeof(rel), "%s/%s", sub, tops[i]);
			rels[i] = strdup(rel);
			if (rename(from, target) < 0) {
				fail("io", "cannot place %s: %s", tops[i], strerror(errno));
				for (int k = 0; k < i; k++) {
					char t[1100];

					snprintf(t, sizeof(t), "%s/%s", C.roms, rels[k]);
					rm_tree(t);
				}
				for (int k = 0; k <= i; k++)
					free(rels[k]);
				free(rels);
				rm_tree(stage);
				free_tops(tops, ntops);
				catalog_free(&c);
				return 1;
			}
		}
		rm_tree(stage);
		installed_update(e.id, e.version, rels, ntops);
		for (int i = 0; i < ntops; i++)
			free(rels[i]);
		free(rels);
		free_tops(tops, ntops);
	}
	progress("install", 1, 1);

	/* a cover for a native game (best effort): <cover-dir>/<launcher stem>.png */
	if (e.kind == K_NATIVE && e.cover && C.cover_dir[0] && !g_cancel) {
		struct membuf m;
		char stem[160], cp[1300], cerr[200];

		cpy(stem, sizeof(stem), e.launcher);
		if (strlen(stem) > 3 && !strcmp(stem + strlen(stem) - 3, ".sh"))
			stem[strlen(stem) - 3] = 0;
		if (fetch_mem(e.cover, &m, COVER_MAX, cerr, sizeof(cerr)) == 0) {
			if (m.len > 8 && !memcmp(m.b, "\x89PNG\r\n\x1a\n", 8)) {
				mkdir_p(C.cover_dir);
				snprintf(cp, sizeof(cp), "%s/%s.png", C.cover_dir, stem);
				write_atomic(cp, m.b, m.len);
			}
			free(m.b);
		}
	}
	printf("ok");
	field("id", e.id);
	field("version", e.version);
	endl();
	catalog_free(&c);
	return 0;
}

/* the port's files an install moved aside (native/.store-replaced/<id>/, see cmd_install_port) */
static void replaced_remove(const char *id)
{
	char keep[1300];

	snprintf(keep, sizeof(keep), "%s/native/.store-replaced/%s", C.roms, id);
	rm_tree(keep);
	snprintf(keep, sizeof(keep), "%s/native/.store-replaced", C.roms);
	rmdir(keep);
}

static int cmd_remove(const char *id)
{
	char iv[64];
	int n;

	if (!rsu_safe_token(id))
		return fail("notfound", "bad id");
	if (!installed_version(id, iv, sizeof(iv)))
		return fail("notfound", "%s is not installed from the store", id);
	n = installed_remove(id);
	replaced_remove(id);
	printf("ok");
	field("id", id);
	fieldf("removed", "%d", n);
	endl();
	return 0;
}

/* The game that PATH (relative to the roms folder, or a full path in it) belongs to, removed like
 * remove ID: the menu's "Delete this game" on a store port's launcher removes the whole port, not
 * the launcher alone (its saves and settings stay, as with remove). */
static int cmd_remove_path(const char *arg)
{
	char path[1100], line[1500], id[128] = "";
	const char *rel = arg;
	size_t rl = strlen(C.roms);
	FILE *f;

	while (rl > 1 && C.roms[rl - 1] == '/')
		rl--;
	if (!strncmp(arg, C.roms, rl) && arg[rl] == '/')
		rel = arg + rl + 1;
	if (!safe_relpath(rel))
		return fail("notstore", "%s is not in the roms folder", arg);
	installed_path(path, sizeof(path));
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *p = strtok(NULL, "\n");

		if (a && b && p && !strcmp(p, rel)) {
			cpy(id, sizeof(id), a);
			break;
		}
	}
	if (f)
		fclose(f);
	if (!id[0] || !rsu_safe_token(id))
		return fail("notstore", "%s was not installed from the store", rel);
	return cmd_remove(id);
}


/* ============================================================ ports from the player's ROM */
/* The ROM formats of the N64 dumps: z64 big-endian, v64 swapped in pairs, n64 little-endian words. */
enum { RF_Z64 = 0, RF_V64, RF_N64, RF_UNKNOWN = -1 };

static int rom_format(const uint8_t *h)
{
	if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40)
		return RF_Z64;
	if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12)
		return RF_V64;
	if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80)
		return RF_N64;
	return RF_UNKNOWN;
}

/* to z64 order, in place; n is a multiple of 4 */
static void rom_normalize(int fmt, uint8_t *b, size_t n)
{
	if (fmt == RF_V64) {
		for (size_t i = 0; i + 1 < n; i += 2) {
			uint8_t t = b[i];

			b[i] = b[i + 1];
			b[i + 1] = t;
		}
	} else if (fmt == RF_N64) {
		for (size_t i = 0; i + 3 < n; i += 4) {
			uint8_t t = b[i];

			b[i] = b[i + 3];
			b[i + 3] = t;
			t = b[i + 1];
			b[i + 1] = b[i + 2];
			b[i + 2] = t;
		}
	}
}

/* SHA-1 of the file in z64 order (what the published checksums are); false: not an N64 dump */
static bool rom_hash(const char *path, char sha[41], int *fmt_out)
{
	static uint8_t buf[1 << 16];
	struct sha1 c;
	uint8_t d[20];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int fmt = RF_UNKNOWN;
	ssize_t r;
	size_t have = 0;

	if (fd < 0)
		return false;
	sha1_init(&c);
	for (;;) {
		r = read(fd, buf + have, sizeof(buf) - have);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		have += (size_t)r;
		if (have == sizeof(buf) || have % 4 == 0) {
			size_t use = have - have % 4;

			if (fmt == RF_UNKNOWN) {
				if (use < 4 || (fmt = rom_format(buf)) == RF_UNKNOWN) {
					close(fd);
					return false;
				}
			}
			rom_normalize(fmt, buf, use);
			sha1_update(&c, buf, use);
			memmove(buf, buf + use, have - use);
			have -= use;
		}
		if (g_cancel) {
			close(fd);
			return false;
		}
	}
	close(fd);
	if (r < 0 || have || fmt == RF_UNKNOWN)
		return false;
	sha1_final(&c, d);
	sha1_hex(d, sha);
	if (fmt_out)
		*fmt_out = fmt;
	return true;
}

/* hashes already made: "size<TAB>mtime<TAB>sha1<TAB>path" */
static bool hash_cached(const char *path, int64_t size, int64_t mtime, char sha[41])
{
	char cp[1100], line[1400];
	FILE *f;
	bool found = false;

	snprintf(cp, sizeof(cp), "%s/romhash.tsv", C.state);
	f = fopen(cp, "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		long long sz, mt;
		char h[64], *p;

		if (sscanf(line, "%lld\t%lld\t%63[0-9a-f]\t", &sz, &mt, h) != 3 || strlen(h) != 40)
			continue;
		p = strchr(strchr(strchr(line, '\t') + 1, '\t') + 1, '\t') + 1;
		p[strcspn(p, "\n")] = 0;
		if (sz == size && mt == mtime && !strcmp(p, path)) {
			cpy(sha, 41, h);
			found = true;
		}
	}
	fclose(f);
	return found;
}

static void hash_remember(const char *path, int64_t size, int64_t mtime, const char *sha)
{
	char cp[1100];
	FILE *f;

	if (mkdir_p(C.state) < 0)
		return;
	snprintf(cp, sizeof(cp), "%s/romhash.tsv", C.state);
	f = fopen(cp, "a");
	if (f) {
		fprintf(f, "%lld\t%lld\t%s\t%s\n", (long long)size, (long long)mtime, sha, path);
		fclose(f);
	}
}

static bool has_rom_ext(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && (!strcasecmp(dot, ".z64") || !strcasecmp(dot, ".v64") || !strcasecmp(dot, ".n64"));
}

static bool size_ok(const struct rominfo *r, int64_t size)
{
	if (!r->nsize)
		return true;
	for (int i = 0; i < r->nsize; i++)
		if (r->size[i] == size)
			return true;
	return false;
}

/* does this file hold one of the accepted dumps? */
static bool rom_matches(const struct rominfo *r, const char *path, char sha[41])
{
	struct stat st;

	if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || !size_ok(r, st.st_size))
		return false;
	if (!hash_cached(path, st.st_size, st.st_mtime, sha)) {
		if (!rom_hash(path, sha, NULL))
			return false;
		hash_remember(path, st.st_size, st.st_mtime, sha);
	}
	for (int i = 0; i < r->nsha; i++)
		if (!strcmp(sha, r->sha1[i]))
			return true;
	return false;
}

/* the first dump of the game in the places a player keeps ROMs: the games folders (<roms>/n64, ...) of
 * every drive, and anything given with --scan (a USB drive's root and its n64 folder) */
static bool find_rom(const struct rominfo *r, char *path_out, size_t pn, char sha[41])
{
	const char *bases[5];
	int nb = 0;

	bases[nb++] = C.roms;
	for (int i = 0; i < C.nscan; i++)
		bases[nb++] = C.scan[i];
	for (int b = 0; b < nb; b++) {
		for (int s = -1; s < r->nsys; s++) {
			char dir[1100];
			DIR *d;
			struct dirent *de;

			if (s < 0 && b == 0)
				continue;       /* the games folder itself holds system folders, not ROMs */
			if (s < 0)
				cpy(dir, sizeof(dir), bases[b]);
			else
				snprintf(dir, sizeof(dir), "%s/%s", bases[b], r->system[s]);
			d = opendir(dir);
			if (!d)
				continue;
			while ((de = readdir(d))) {
				char full[2048];

				if (de->d_name[0] == '.' || !has_rom_ext(de->d_name))
					continue;
				snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
				if (rom_matches(r, full, sha)) {
					cpy(path_out, pn, full);
					closedir(d);
					return true;
				}
				if (g_cancel) {
					closedir(d);
					return false;
				}
			}
			closedir(d);
		}
	}
	return false;
}

static int cmd_find_rom(const char *id)
{
	struct catalog c;
	struct entry e;
	char err[256], path[1100], sha[41];

	if (catalog_open(&c, err, sizeof(err)) < 0)
		return fail("catalog", "%s", err);
	if (!catalog_find(&c, id, &e) || e.kind != K_PORT) {
		catalog_free(&c);
		return fail("notfound", "no port %s in the catalog", id);
	}
	if (find_rom(&e.rom, path, sizeof(path), sha)) {
		printf("rom");
		field("path", path);
		field("sha1", sha);
		field("name", e.rom.name);
		endl();
		printf("done");
		field("count", "1");
		endl();
		catalog_free(&c);
		return 0;
	}
	catalog_free(&c);
	return fail("norom", "no %s ROM found", e.rom.name);
}

/* MemAvailable in kB (RSOS_MEMINFO: a file for the tests), -1 when unknown */
static long mem_available_kb(void)
{
	const char *mi = getenv("RSOS_MEMINFO");
	FILE *f = fopen(mi ? mi : "/proc/meminfo", "r");
	char line[200];
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemAvailable: %ld kB", &v) == 1)
			break;
	fclose(f);
	return v;
}

/*
 * Runs the package's extract.sh as a guarded child: its own process group, the first thing the kernel kills when
 * memory runs out (oom_score_adj 1000), an address-space limit, killed by us when free memory falls below the
 * floor or after the time allowed, or on cancel. Its stdout lines "progress<TAB>done=N<TAB>total=M" are passed on.
 * Returns 0 or a code: "extract" (it failed), "memory", "timeout", "cancelled".
 */
static const char *run_guarded(const char *script, const char *cwd, const char *rom, const char *out, const char *tmp,
			       const char *logfile, int est_min)
{
	int pfd[2];
	pid_t pid;
	time_t start = time(NULL);
	long floor_kb = getenv("RSOS_MEMFLOOR_KB") ? atol(getenv("RSOS_MEMFLOOR_KB")) : 90000;
	long limit_s = getenv("RSOS_TIMEOUT_S") ? atol(getenv("RSOS_TIMEOUT_S")) : 3L * est_min * 60;
	const char *why = NULL;
	char buf[512];
	size_t have = 0;
	int status = 0;

	if (pipe2(pfd, O_CLOEXEC) < 0)
		return "extract";
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return "extract";
	}
	if (pid == 0) {
		struct rlimit rl = { 1800UL << 20, 1800UL << 20 };
		int fd, lg;

		setsid();
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		fd = open("/proc/self/oom_score_adj", O_WRONLY);
		if (fd >= 0) {
			if (write(fd, "1000", 4) < 0) {
				/* not fatal */
			}
			close(fd);
		}
		if (!getenv("RSOS_NO_RLIMIT"))
			setrlimit(RLIMIT_AS, &rl);
		dup2(pfd[1], 1);
		lg = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (lg >= 0)
			dup2(lg, 2);
		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0)
			dup2(fd, 0);
		if (chdir(cwd) < 0)
			_exit(126);
		setenv("RSOS_ROM", rom, 1);
		setenv("RSOS_OUT", out, 1);
		setenv("TMPDIR", tmp, 1);
		execl("/bin/sh", "sh", script, (char *)NULL);
		_exit(127);
	}
	close(pfd[1]);
	for (;;) {
		struct pollfd p = { pfd[0], POLLIN, 0 };
		long avail;
		pid_t w;

		if (poll(&p, 1, 500) > 0 && (p.revents & POLLIN)) {
			ssize_t r = read(pfd[0], buf + have, sizeof(buf) - 1 - have);

			if (r > 0) {
				char *nl;

				have += (size_t)r;
				buf[have] = 0;
				while ((nl = strchr(buf, '\n'))) {
					long long dn, tt;

					*nl = 0;
					if (sscanf(buf, "progress\tdone=%lld\ttotal=%lld", &dn, &tt) == 2 && tt > 0)
						progress("extract", (uint64_t)dn, (uint64_t)tt);
					memmove(buf, nl + 1, have - (size_t)(nl + 1 - buf));
					have -= (size_t)(nl + 1 - buf);
					buf[have] = 0;
				}
				if (have >= sizeof(buf) - 1)
					have = 0;
			}
		}
		w = waitpid(pid, &status, WNOHANG);
		if (w == pid)
			break;
		if (!why) {
			avail = mem_available_kb();
			if (g_cancel)
				why = "cancelled";
			else if (avail >= 0 && avail < floor_kb)
				why = "memory";
			else if (time(NULL) - start > limit_s)
				why = "timeout";
			if (why) {
				kill(-pid, SIGKILL);
				kill(pid, SIGKILL);
			}
		}
		/* no progress lines from the script: show the time against the estimate */
		if (!why)
			progress("extract", (uint64_t)(time(NULL) - start), (uint64_t)est_min * 60);
	}
	close(pfd[0]);
	kill(-pid, SIGKILL);            /* stragglers of its group */
	if (why)
		return why;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? NULL : "extract";
}

/* the ROM in z64 order, written to dest */
static bool rom_write_z64(const char *src, const char *dest)
{
	static uint8_t buf[1 << 16];
	int in = open(src, O_RDONLY | O_CLOEXEC), out;
	int fmt = RF_UNKNOWN;
	size_t have = 0;
	ssize_t r;
	bool ok = true;

	if (in < 0)
		return false;
	out = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		close(in);
		return false;
	}
	for (;;) {
		r = read(in, buf + have, sizeof(buf) - have);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		have += (size_t)r;
		if (have == sizeof(buf)) {
			if (fmt == RF_UNKNOWN && (fmt = rom_format(buf)) == RF_UNKNOWN) {
				ok = false;
				break;
			}
			rom_normalize(fmt, buf, have);
			if (write(out, buf, have) != (ssize_t)have) {
				ok = false;
				break;
			}
			have = 0;
		}
	}
	if (ok && r == 0 && have) {
		if (fmt == RF_UNKNOWN && (fmt = rom_format(buf)) == RF_UNKNOWN)
			ok = false;
		else {
			size_t use = have - have % 4;

			rom_normalize(fmt, buf, use);
			ok = write(out, buf, use) == (ssize_t)use;
		}
	} else if (r < 0) {
		ok = false;
	}
	close(in);
	if (close(out) < 0)
		ok = false;
	return ok;
}

struct pathlist {
	char **p;
	int n, cap;
};

static void pl_add(struct pathlist *l, const char *s)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 32;
		l->p = realloc(l->p, (size_t)l->cap * sizeof(char *));
	}
	l->p[l->n++] = strdup(s);
}

static void pl_free(struct pathlist *l)
{
	for (int i = 0; i < l->n; i++)
		free(l->p[i]);
	free(l->p);
	memset(l, 0, sizeof(*l));
}

/* every regular file below root/rel, as paths relative to root */
static void walk_files(const char *root, const char *rel, struct pathlist *l)
{
	char dir[2048];
	DIR *d;
	struct dirent *de;

	snprintf(dir, sizeof(dir), "%s%s%s", root, rel[0] ? "/" : "", rel);
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		char sub[1100], full[2200];
		struct stat st;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
		snprintf(full, sizeof(full), "%s/%s", root, sub);
		if (lstat(full, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode))
			walk_files(root, sub, l);
		else if (S_ISREG(st.st_mode))
			pl_add(l, sub);
	}
	closedir(d);
}

static int cmd_install_port(const char *id)
{
	struct catalog c;
	struct entry e;
	char err[300] = "", stage[2048], part[2048], pkg[2200], out[2200], tmp[2200], rom[1100], sha[41], iv[64], hex[65];
	char zrom[2200], inst[2300], script[2300], logf[2200], target[2600], from[2600], native[1100];
	uint8_t want[32], got[32];
	struct pathlist files = { 0 }, moved = { 0 };
	char **tops = NULL;
	int ntops = 0, rc, result = 1;
	long avail;
	struct statvfs vf;
	const char *why;
	bool mine;

	if (catalog_open(&c, err, sizeof(err)) < 0)
		return fail("catalog", "%s", err);
	if (!catalog_find(&c, id, &e) || e.kind != K_PORT) {
		catalog_free(&c);
		return fail("notfound", "no port %s in the catalog", id);
	}
	if (sha256_parse_hex(e.sha, want) < 0) {
		catalog_free(&c);
		return fail("catalog", "bad hash for %s", id);
	}
	snprintf(native, sizeof(native), "%s/native", C.roms);
	snprintf(stage, sizeof(stage), "%s/.store-%s", C.roms, e.id);
	snprintf(part, sizeof(part), "%s/.store-%s.part", C.roms, e.id);
	snprintf(pkg, sizeof(pkg), "%s/pkg", stage);
	snprintf(out, sizeof(out), "%s/out", stage);
	snprintf(tmp, sizeof(tmp), "%s/tmp", stage);
	snprintf(zrom, sizeof(zrom), "%s/rom.z64", stage);
	snprintf(logf, sizeof(logf), "%s/extract.log", stage);

	/* the ROM first: nothing is downloaded for a game the player cannot install */
	if (C.rom_arg[0]) {
		if (!rom_matches(&e.rom, C.rom_arg, sha)) {
			fail("norom", "%s is not a %s ROM", C.rom_arg, e.rom.name);
			goto done;
		}
		cpy(rom, sizeof(rom), C.rom_arg);
	} else {
		progress("rom", 0, 1);
		if (!find_rom(&e.rom, rom, sizeof(rom), sha)) {
			fail(g_cancel ? "cancelled" : "norom", "no %s ROM found", e.rom.name);
			goto done;
		}
	}
	printf("rom");
	field("path", rom);
	field("sha1", sha);
	endl();

	/* room: free memory for the extractor, free disk for the package and what it makes */
	avail = mem_available_kb();
	if (!strcmp(e.rom.mode, "extract") && avail >= 0 && avail / 1024 < e.rom.need_ram_mb) {
		fail("memory", "needs %d MB of free memory, %ld MB available: restart the console and try again",
		     e.rom.need_ram_mb, avail / 1024);
		goto done;
	}
	mkdir_p(C.roms);
	if (statvfs(C.roms, &vf) == 0 &&
	    (int64_t)vf.f_bavail * (int64_t)vf.f_frsize < e.size + e.unpacked + ((int64_t)e.rom.need_disk_mb << 20)) {
		fail("space", "not enough free space for %s", e.name);
		goto done;
	}

	rm_tree(stage);
	unlink(part);
	if (mkdir_p(pkg) < 0 || mkdir_p(out) < 0 || mkdir_p(tmp) < 0) {
		fail("io", "cannot make the work folder");
		goto done;
	}
	rc = download(e.url, part, e.size, got, err, sizeof(err));
	if (rc < 0) {
		fail(rc == -2 ? "cancelled" : "download", "%s", err);
		goto done;
	}
	progress("verify", 1, 1);
	sha256_hex(got, hex);
	if (memcmp(got, want, 32)) {
		fail("hash", "the download does not match the catalog's SHA-256");
		goto done;
	}
	if (untar_any(part, pkg, e.unpacked, &tops, &ntops, err, sizeof(err)) < 0) {
		fail("archive", "%s", err);
		goto done;
	}
	unlink(part);
	snprintf(inst, sizeof(inst), "%s/install", pkg);
	snprintf(script, sizeof(script), "%s/extract/extract.sh", pkg);
	{
		struct stat st;

		if (stat(inst, &st) < 0 || !S_ISDIR(st.st_mode) ||
		    (!strcmp(e.rom.mode, "extract") && access(script, R_OK) < 0)) {
			fail("archive", "the package has no install folder or extract.sh");
			goto done;
		}
	}

	/* the game data from the ROM */
	progress("extract", 0, (uint64_t)e.rom.est_min * 60);
	if (!rom_write_z64(rom, zrom)) {
		fail("rom", "cannot read the ROM");
		goto done;
	}
	if (!strcmp(e.rom.mode, "extract")) {
		char cwd[2300];

		snprintf(cwd, sizeof(cwd), "%s/extract", pkg);
		why = run_guarded(script, cwd, zrom, out, tmp, logf, e.rom.est_min);
		if (why) {
			fail(why, "the game data could not be made (%s); see %s", why, logf);
			goto done;
		}
	} else {
		char dst[2400], dir[2400];

		snprintf(dst, sizeof(dst), "%s/%s", out, e.rom.copy_as);
		cpy(dir, sizeof(dir), dst);
		*strrchr(dir, '/') = 0;
		mkdir_p(dir);
		if (rename(zrom, dst) < 0) {
			fail("io", "cannot place the ROM: %s", strerror(errno));
			goto done;
		}
	}
	for (int i = 0; i < e.rom.nout; i++) {
		struct stat st;

		snprintf(from, sizeof(from), "%s/%s", out, e.rom.output[i]);
		if (stat(from, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
			fail("extract", "%s was not made", e.rom.output[i]);
			goto done;
		}
	}
	unlink(zrom);

	/* commit: the program files, then the made files, each moved into native/ (folders merge) */
	mine = installed_version(e.id, iv, sizeof(iv));
	walk_files(inst, "", &files);
	for (int i = 0; i < files.n; i++) {
		if (!safe_relpath(files.p[i])) {
			fail("archive", "unsafe path in the package");
			goto done;
		}
	}
	if (!mine) {
		/* Files of the port that the store did not put there: what "Delete this game"
		 * left behind (it deletes the launcher only), or a copy installed by hand. They
		 * are this port's own program files, never its saves (no package holds a save),
		 * so they are moved aside into native/.store-replaced/<id>/ (hidden from the game
		 * list; the previous one is replaced) instead of blocking the install. */
		char keep[1300];
		bool cleared = false;

		snprintf(keep, sizeof(keep), "%s/.store-replaced/%s", native, e.id);
		for (int i = 0; i < files.n; i++) {
			struct stat st;
			char to[2700], *sl;

			snprintf(target, sizeof(target), "%s/%s", native, files.p[i]);
			if (lstat(target, &st) < 0)
				continue;
			if (!cleared) {
				rm_tree(keep);
				cleared = true;
			}
			snprintf(to, sizeof(to), "%s/%s", keep, files.p[i]);
			sl = strrchr(to, '/');
			*sl = 0;
			mkdir_p(to);
			*sl = '/';
			if (rename(target, to) < 0) {
				fail("exists", "%s already exists and cannot be moved aside: %s", files.p[i], strerror(errno));
				goto done;
			}
			fprintf(stderr, "rsos-store: %s was not from the store: moved to %s\n", files.p[i], to);
		}
	}
	if (mine)
		installed_remove(e.id);
	for (int i = 0; i < files.n + e.rom.nout; i++) {
		char rel[1100], *sl;

		if (i < files.n) {
			cpy(rel, sizeof(rel), files.p[i]);
			snprintf(from, sizeof(from), "%s/%s", inst, rel);
		} else {
			snprintf(rel, sizeof(rel), "%s/%s", e.rom.dest, e.rom.output[i - files.n]);
			snprintf(from, sizeof(from), "%s/%s", out, e.rom.output[i - files.n]);
		}
		snprintf(target, sizeof(target), "%s/%s", native, rel);
		sl = strrchr(target, '/');
		*sl = 0;
		mkdir_p(target);
		*sl = '/';
		if (rename(from, target) < 0) {
			fail("io", "cannot place %s: %s", rel, strerror(errno));
			for (int k = 0; k < moved.n; k++) {
				snprintf(from, sizeof(from), "%s/%s", native, moved.p[k]);
				unlink(from);
			}
			goto done;
		}
		pl_add(&moved, rel);
	}
	{
		char **rels = calloc((size_t)moved.n + 1, sizeof(char *));

		for (int i = 0; i < moved.n; i++) {
			char r2[1200];

			snprintf(r2, sizeof(r2), "native/%s", moved.p[i]);
			rels[i] = strdup(r2);
		}
		installed_update(e.id, e.version, rels, moved.n);
		for (int i = 0; i < moved.n; i++)
			free(rels[i]);
		free(rels);
	}
	progress("install", 1, 1);
	printf("ok");
	field("id", e.id);
	field("version", e.version);
	endl();
	result = 0;
done:
	pl_free(&files);
	pl_free(&moved);
	free_tops(tops, ntops);
	rm_tree(stage);
	unlink(part);
	catalog_free(&c);
	return result;
}


/* <state>/store.ini: the player's own catalog (a home server): "url=https://host/catalog.json" and, for a
 * server without HTTPS, "allow_http=1". Options on the command line win. */
static void read_store_ini(bool url_given, bool http_given)
{
	char path[1100], line[1200];
	FILE *f;

	snprintf(path, sizeof(path), "%s/store.ini", C.state);
	f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!url_given && !strncmp(line, "url=", 4) && line[4])
			cpy(C.url, sizeof(C.url), line + 4);
		else if (!http_given && !strcmp(line, "allow_http=1"))
			C.allow_http = true;
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *cmd = NULL, *arg = NULL;
	bool url_given = false, http_given = false;

	cpy(C.roms, sizeof(C.roms), "/data/roms");
	cpy(C.state, sizeof(C.state), "/data/rsos/store");
	cpy(C.pubkey, sizeof(C.pubkey), "/usr/share/rsos/store.pub");
	cpy(C.url, sizeof(C.url), STORE_URL);
	cpy(C.arch, sizeof(C.arch), STORE_ARCH);
	cpy(C.cover_dir, sizeof(C.cover_dir), "");

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (v && !strcmp(a, "--roms")) {
			cpy(C.roms, sizeof(C.roms), v), i++;
		} else if (v && !strcmp(a, "--state")) {
			cpy(C.state, sizeof(C.state), v), i++;
		} else if (v && !strcmp(a, "--pubkey")) {
			cpy(C.pubkey, sizeof(C.pubkey), v), i++;
		} else if (v && !strcmp(a, "--url")) {
			cpy(C.url, sizeof(C.url), v), url_given = true, i++;
		} else if (v && !strcmp(a, "--catalog")) {
			cpy(C.catalog_file, sizeof(C.catalog_file), v), url_given = true, i++;
		} else if (v && !strcmp(a, "--arch")) {
			cpy(C.arch, sizeof(C.arch), v), i++;
		} else if (v && !strcmp(a, "--cover-dir")) {
			cpy(C.cover_dir, sizeof(C.cover_dir), v), i++;
		} else if (v && !strcmp(a, "--max-bytes")) {
			C.max_bytes = strtoll(v, NULL, 10), i++;
		} else if (v && !strcmp(a, "--out")) {
			if (!freopen(v, "w", stdout))
				return 1;
			setvbuf(stdout, NULL, _IOLBF, 0);
			i++;
		} else if (v && !strcmp(a, "--scan")) {
			if (C.nscan < 4)
				cpy(C.scan[C.nscan++], sizeof(C.scan[0]), v);
			i++;
		} else if (v && !strcmp(a, "--rom")) {
			cpy(C.rom_arg, sizeof(C.rom_arg), v), i++;
		} else if (!strcmp(a, "--allow-http")) {
			C.allow_http = true;
			http_given = true;
		} else if (!cmd) {
			cmd = a;
		} else if (!arg) {
			arg = a;
		} else {
			return fail("usage", "unexpected argument %s", a);
		}
	}
	read_store_ini(url_given, http_given);
	if (roms_is_default())
		snprintf(C.record, sizeof(C.record), "%s/installed.tsv", C.state);
	else
		snprintf(C.record, sizeof(C.record), "%s/.rsos-store/installed.tsv", C.roms);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGPIPE, SIG_IGN);
	if (!cmd)
		return fail("usage", "list [SYSTEM] | get ID | install-port ID | find-rom ID | remove ID | remove-path PATH | installed");
	if (!strcmp(cmd, "list"))
		return cmd_list(arg);
	if (!strcmp(cmd, "installed"))
		return cmd_installed();
	if (!strcmp(cmd, "get") && arg)
		return rsu_safe_token(arg) ? cmd_get(arg) : fail("notfound", "bad id");
	if (!strcmp(cmd, "find-rom") && arg)
		return rsu_safe_token(arg) ? cmd_find_rom(arg) : fail("notfound", "bad id");
	if (!strcmp(cmd, "install-port") && arg)
		return rsu_safe_token(arg) ? cmd_install_port(arg) : fail("notfound", "bad id");
	if (!strcmp(cmd, "remove") && arg)
		return cmd_remove(arg);
	if (!strcmp(cmd, "remove-path") && arg)
		return cmd_remove_path(arg);
	return fail("usage", "unknown command %s", cmd);
}
