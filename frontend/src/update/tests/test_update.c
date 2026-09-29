/*
 * test_update.c - unit and integration tests of the updater (src/update,
 * docs/updates.md), run by `make check-update` (part of `make check`):
 *
 *   SHA-256 (NIST vectors), base64, signify keys and signatures, the
 *   manifest parser, version ordering, the tar container, the policy (good /
 *   bad signature / tampered / wrong board / older / same / forced /
 *   unsigned on release and development builds / bootloader / updater),
 *   JSON, slot selection from the kernel command line, a full install into
 *   a file "slot" with a fake fw_printenv/fw_setenv (content, environment,
 *   state file), a tampered payload and a bad signature (nothing written),
 *   an interrupted install (the environment untouched, the slot invalid),
 *   downloads from a local HTTP server (resume with Range after a dropped
 *   connection, a server ignoring Range, a changed file), the GitHub
 *   release check (newer, up to date, another board, unsigned, clock) and
 *   the after-restart events.
 *
 *   test_update <work dir>
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <zstd.h>

#include "update/http.h"
#include "update/json.h"
#include "update/rsu.h"
#include "update/sha256.h"
#include "update/update.h"

static int g_pass, g_fail;
static char W[300];

#define CHECK(cond, ...) do { \
	if (cond) { g_pass++; } else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); } } while (0)

static void quiet_log(const char *line, void *user)
{
	(void)user;
	if (getenv("TEST_UPDATE_VERBOSE"))
		printf("    log: %s\n", line);
}

static void wpath(char *out, size_t n, const char *rel)
{
	snprintf(out, n, "%s/%s", W, rel);
}

static void mkdirs(const char *p)
{
	char b[1024];

	snprintf(b, sizeof(b), "%s", p);
	for (char *s = b + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(b, 0755);
			*s = '/';
		}
	mkdir(b, 0755);
}

static void put_file(const char *path, const void *data, size_t n)
{
	char d[1024], *s;
	int fd;

	snprintf(d, sizeof(d), "%s", path);
	s = strrchr(d, '/');
	if (s) {
		*s = 0;
		mkdirs(d);
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0 || write(fd, data, n) != (ssize_t)n) {
		printf("cannot write %s\n", path);
		exit(2);
	}
	close(fd);
}

static void put_text(const char *path, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put_text(const char *path, const char *fmt, ...)
{
	char b[8192];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	put_file(path, b, (size_t)n);
}

static char *get_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *b;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return NULL;
	}
	b[n] = 0;
	fclose(f);
	if (len)
		*len = (size_t)n;
	return b;
}

static void hex(const uint8_t *d, char *out)
{
	sha256_hex(d, out);
}

/* ------------------------------------------------------------- sha256 */
static void test_sha256(void)
{
	static const struct {
		const char *in, *out;
	} v[] = {
		{ "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
		{ "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
		{ "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrst"
		  "nopqrstu",
		  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
	};
	uint8_t d[32];
	char h[65];
	struct sha256 s;
	char *mil = malloc(1000000);

	printf("== SHA-256\n");
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		sha256(v[i].in, strlen(v[i].in), d);
		hex(d, h);
		CHECK(!strcmp(h, v[i].out), "sha256(\"%.10s...\") = %s", v[i].in, h);
	}
	memset(mil, 'a', 1000000);
	sha256_init(&s);
	for (size_t o = 0; o < 1000000;) {
		size_t k = (o * 7 + 13) % 997 + 1;     /* odd chunk sizes */

		if (o + k > 1000000)
			k = 1000000 - o;
		sha256_update(&s, mil + o, k);
		o += k;
	}
	sha256_final(&s, d);
	hex(d, h);
	CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "1M a: %s", h);
	CHECK(sha256_parse_hex(h, d) == 0 && sha256_parse_hex("12", d) < 0, "hex parse");
	free(mil);
}

/* ------------------------------------------------------------- keys */
static struct rsu_seckey g_sk, g_sk2;
static struct rsu_pubkey g_pk, g_pk2;

static void test_keys(void)
{
	uint8_t seed[32], num[8], out[64];
	char text[512], b64[128];
	struct rsu_seckey sk;
	struct rsu_pubkey pk;
	struct rsu_sig s, s2;
	const char msg[] = "the manifest";

	printf("== base64, signify keys, signatures\n");
	CHECK(rsu_b64_encode((const uint8_t *)"foobar", 6, b64, sizeof(b64)) == 8 && !strcmp(b64, "Zm9vYmFy"), "b64 enc");
	CHECK(rsu_b64_encode((const uint8_t *)"fooba", 5, b64, sizeof(b64)) == 8 && !strcmp(b64, "Zm9vYmE="), "b64 pad");
	CHECK(rsu_b64_decode("Zm9vYg==", 8, out, sizeof(out)) == 4 && !memcmp(out, "foob", 4), "b64 dec");
	CHECK(rsu_b64_decode("Zm9v!mE=", 8, out, sizeof(out)) < 0, "b64 bad char");
	CHECK(rsu_b64_decode("Zm9=vmE=", 8, out, sizeof(out)) < 0, "b64 data after padding");
	CHECK(rsu_b64_decode("Zm9", 3, out, sizeof(out)) < 0, "b64 truncated");

	for (int i = 0; i < 32; i++)
		seed[i] = (uint8_t)(i * 7 + 1);
	memcpy(num, "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
	rsu_keypair(seed, num, &g_sk, &g_pk);
	seed[0] ^= 0xff;
	memcpy(num, "\x11\x12\x13\x14\x15\x16\x17\x18", 8);
	rsu_keypair(seed, num, &g_sk2, &g_pk2);

	CHECK(rsu_pubkey_format(&g_pk, "test public key", text, sizeof(text)) > 0 &&
	      !strncmp(text, "untrusted comment: test public key\nRWQBAgMEBQYHC", 44), "pubkey text: %s", text);
	CHECK(rsu_pubkey_parse(text, strlen(text), &pk) == 0 && !memcmp(&pk, &g_pk, sizeof(pk)), "pubkey round trip");
	CHECK(rsu_seckey_format(&g_sk, "test secret key", text, sizeof(text)) > 0, "seckey text");
	CHECK(rsu_seckey_parse(text, strlen(text), &sk) == 0 && !memcmp(&sk, &g_sk, sizeof(sk)), "seckey round trip");
	{
		/* a flipped bit in the secret key: the checksum catches it */
		char *p = strchr(text, '\n') + 1 + 60;

		*p = *p == 'A' ? 'B' : 'A';
		CHECK(rsu_seckey_parse(text, strlen(text), &sk) < 0, "damaged secret key refused");
	}
	CHECK(rsu_pubkey_parse("no comment\nRWQ=\n", 16, &pk) < 0, "not a signify file");
	rsu_seckey_public(&g_sk, &pk);
	CHECK(!memcmp(&pk, &g_pk, sizeof(pk)), "public half of the secret key");

	rsu_sign(&g_sk, msg, strlen(msg), &s);
	CHECK(rsu_verify(&g_pk, &s, msg, strlen(msg)), "good signature");
	CHECK(!rsu_verify(&g_pk, &s, "the manifesT", strlen(msg)), "modified message");
	CHECK(!rsu_verify(&g_pk2, &s, msg, strlen(msg)), "another key (key number)");
	pk = g_pk2;
	memcpy(pk.keynum, g_pk.keynum, 8);
	CHECK(!rsu_verify(&pk, &s, msg, strlen(msg)), "another key with the same key number");
	rsu_sig_format(&s, "verify with update.pub", text, sizeof(text));
	CHECK(rsu_sig_parse(text, strlen(text), &s2) == 0 && !memcmp(&s, &s2, sizeof(s)), "signature text round trip");
	s2.sig[5] ^= 1;
	CHECK(!rsu_verify(&g_pk, &s2, msg, strlen(msg)), "damaged signature");
}

/* ----------------------------------------------------------- manifest */
static void fill_manifest(struct rsu_manifest *m, const char *board, const char *version, int64_t t)
{
	memset(m, 0, sizeof(*m));
	m->format = 1;
	snprintf(m->board, sizeof(m->board), "%s", board);
	snprintf(m->version, sizeof(m->version), "%s", version);
	snprintf(m->variant, sizeof(m->variant), "release");
	snprintf(m->build_date, sizeof(m->build_date), "2026-10-01");
	m->build_time = t;
	m->min_updater = 1;
	m->bootloader_min = 1;
	snprintf(m->payload, sizeof(m->payload), "rootfs.ext4.zst");
	snprintf(m->compression, sizeof(m->compression), "zstd");
	m->payload_size = 1000;
	m->image_size = 8192;
	snprintf(m->changelog, sizeof(m->changelog), "- line one\n- tab\there \\ backslash");
}

static void test_manifest(void)
{
	struct rsu_manifest m, p;
	char text[RSU_MANIFEST_MAX], err[256];
	int n;

	printf("== manifest\n");
	fill_manifest(&m, "retrostone2", "0.2.0", 1790000000);
	m.payload_sha256[3] = 0xab;
	n = rsu_manifest_format(&m, text, sizeof(text));
	CHECK(n > 0 && rsu_manifest_parse(text, (size_t)n, &p, err, sizeof(err)) == 0, "round trip: %s", err);
	CHECK(!memcmp(&m, &p, sizeof(m)), "same fields after the round trip (changelog escapes)");
#define BAD(txt, what) CHECK(rsu_manifest_parse(txt, strlen(txt), &p, err, sizeof(err)) < 0, what ": %s", err)
	BAD("format = 1\n", "missing keys");
	{
		char t2[RSU_MANIFEST_MAX + 64];

		snprintf(t2, sizeof(t2), "%sboard = other\n", text);
		BAD(t2, "duplicate key");
		snprintf(t2, sizeof(t2), "%s", text);
		*strstr(t2, "board = retrostone2") = '#';
		BAD(t2, "board commented out");
		snprintf(t2, sizeof(t2), "%s", text);
		memcpy(strstr(t2, "version = 0.2.0"), "version = ../x", 14);
		BAD(t2, "unsafe version");
		snprintf(t2, sizeof(t2), "%s", text);
		memcpy(strstr(t2, "compression = zstd"), "compression = lzma", 18);
		BAD(t2, "unknown compression");
		snprintf(t2, sizeof(t2), "%s", text);
		memcpy(strstr(t2, "image_size = 8192"), "image_size = 8193", 17);
		BAD(t2, "image size not a multiple of 4096");
		snprintf(t2, sizeof(t2), "%snew_key = tolerated\n", text);
		CHECK(rsu_manifest_parse(t2, strlen(t2), &p, err, sizeof(err)) == 0, "unknown key ignored");
		snprintf(t2, sizeof(t2), "%s", text);
		t2[10] = 0;
		CHECK(rsu_manifest_parse(t2, n, &p, err, sizeof(err)) < 0, "NUL byte refused");
	}
	CHECK(rsu_safe_token("retrostone2") && rsu_safe_token("0.2.0-rc1") && !rsu_safe_token("a/b") &&
	      !rsu_safe_token("") && !rsu_safe_token("-x") && !rsu_safe_token("..."), "safe tokens");
#undef BAD
}

static void test_versions(void)
{
	static const struct {
		const char *a;
		int64_t ta;
		const char *b;
		int64_t tb;
		int want;
	} v[] = {
		{ "0.2.0", 0, "0.1.0", 0, 1 },
		{ "0.1", 5, "0.1.0", 5, 0 },
		{ "v0.2", 0, "0.2.0", 0, 0 },
		{ "0.2.10", 0, "0.2.9", 0, 1 },
		{ "0.2", 0, "0.2-rc2", 0, 1 },
		{ "0.2-rc10", 0, "0.2-rc9", 0, 1 },
		{ "0.2-rc1", 0, "0.2-dev", 0, 1 },
		{ "0.1.0", 0, "0.1-dev", 99, 1 },
		{ "0.1-dev", 10, "0.1-dev", 9, 1 },
		{ "0.1.1-test", 0, "0.1-dev", 99, 1 },
		{ "main-1a2b3c4d", 10, "0.1", 20, -1 },
		{ "1.0", 1, "0.9.9", 2, 1 },
		{ "0.2+build5", 1, "0.2", 1, 0 },
	};

	printf("== version order\n");
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		int c = rsu_version_cmp(v[i].a, v[i].ta, v[i].b, v[i].tb);
		int r = rsu_version_cmp(v[i].b, v[i].tb, v[i].a, v[i].ta);

		c = c > 0 ? 1 : c < 0 ? -1 : 0;
		r = r > 0 ? 1 : r < 0 ? -1 : 0;
		CHECK(c == v[i].want && r == -v[i].want, "%s@%lld vs %s@%lld: %d (want %d), reverse %d", v[i].a,
		      (long long)v[i].ta, v[i].b, (long long)v[i].tb, c, v[i].want, r);
	}
}

/* ----------------------------------------------------------- packages */
struct pkg {
	uint8_t *data;
	size_t len;
	uint8_t *image;
	size_t image_len;
};

static void append(struct pkg *p, const void *d, size_t n)
{
	p->data = realloc(p->data, p->len + n);
	memcpy(p->data + p->len, d, n);
	p->len += n;
}

static void append_member(struct pkg *p, const char *name, const void *d, size_t n)
{
	uint8_t blk[512];
	static const uint8_t z[512];

	rsu_tar_header(blk, name, n, 1790000000);
	append(p, blk, 512);
	append(p, d, n);
	if (n % 512)
		append(p, z, 512 - n % 512);
}

/* the manifest variant of the packages make_pkg makes */
static const char *g_pkg_variant = "release";

/* A package of a pseudo-random "image" (image_len bytes, compressible). */
static void make_pkg(struct pkg *p, const char *board, const char *version, int64_t t, size_t image_len,
		     const struct rsu_seckey *sk, unsigned seed, int bootloader_min, int min_updater)
{
	struct rsu_manifest m;
	char text[RSU_MANIFEST_MAX], sig[512];
	size_t cap = ZSTD_compressBound(image_len);
	uint8_t *z = malloc(cap);
	size_t zl;
	static const uint8_t zero[1024];

	memset(p, 0, sizeof(*p));
	p->image = malloc(image_len);
	p->image_len = image_len;
	for (size_t i = 0; i < image_len; i++) {
		seed = seed * 1103515245u + 12345u;
		/* runs of zeros (an ext4 image is mostly empty) and data */
		p->image[i] = (i / 4096) % 3 == 0 ? 0 : (uint8_t)(seed >> 16);
	}
	zl = ZSTD_compress(z, cap, p->image, image_len, 3);
	fill_manifest(&m, board, version, t);
	snprintf(m.variant, sizeof(m.variant), "%s", g_pkg_variant);
	m.payload_size = zl;
	m.image_size = image_len;
	m.bootloader_min = bootloader_min;
	m.min_updater = min_updater;
	sha256(z, zl, m.payload_sha256);
	sha256(p->image, image_len, m.image_sha256);
	snprintf(m.changelog, sizeof(m.changelog), "Version %s\n- things", version);
	{
		int n = rsu_manifest_format(&m, text, sizeof(text));

		append_member(p, "manifest", text, (size_t)n);
		if (sk) {
			struct rsu_sig s;

			rsu_sign(sk, text, (size_t)n, &s);
			rsu_sig_format(&s, "verify with update.pub", sig, sizeof(sig));
			append_member(p, "manifest.sig", sig, strlen(sig));
		}
	}
	append_member(p, "rootfs.ext4.zst", z, zl);
	append(p, zero, sizeof(zero));
	free(z);
}

static void free_pkg(struct pkg *p)
{
	free(p->data);
	free(p->image);
}

static void test_container(void)
{
	struct pkg p;
	struct rsu_header *h = malloc(sizeof(*h));
	char err[256];
	struct rsu_system sys;

	printf("== container and policy\n");
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 256 * 1024, &g_sk, 1, 1, 1);
	CHECK(rsu_header_parse(p.data, p.len, h, err, sizeof(err)) == 0 && h->has_sig &&
	      h->payload_offset % 512 == 0 && !strcmp(h->m.version, "0.2.0"), "parse: %s", err);

	memset(&sys, 0, sizeof(sys));
	snprintf(sys.board, sizeof(sys.board), "retrostone2");
	snprintf(sys.version, sizeof(sys.version), "0.1.0");
	sys.build_time = 1780000000;
	sys.release = true;
	sys.bootloader = 1;
	sys.have_key = true;
	sys.key = g_pk;
	CHECK(rsu_policy(&sys, h, 0) == RSU_OK, "installable");
	sys.key = g_pk2;
	CHECK(rsu_policy(&sys, h, 0) == RSU_E_BADSIG, "another key: badsig");
	sys.key = g_pk;
	sys.have_key = false;
	CHECK(rsu_policy(&sys, h, 0) == RSU_E_NOKEY, "no key");
	sys.have_key = true;
	h->manifest_text[h->manifest_len - 3] ^= 1;         /* tampered changelog */
	CHECK(rsu_policy(&sys, h, 0) == RSU_E_BADSIG, "tampered manifest: badsig");
	h->manifest_text[h->manifest_len - 3] ^= 1;
	snprintf(sys.board, sizeof(sys.board), "rpi4-64");
	CHECK(rsu_policy(&sys, h, RSU_FORCE) == RSU_E_BOARD, "wrong board, even forced");
	snprintf(sys.board, sizeof(sys.board), "retrostone2");
	snprintf(sys.version, sizeof(sys.version), "0.3.0");
	CHECK(rsu_policy(&sys, h, 0) == RSU_E_OLDER, "older: refused");
	CHECK(rsu_policy(&sys, h, RSU_FORCE) == RSU_OK, "older: forced (UART)");
	snprintf(sys.version, sizeof(sys.version), "0.2.0");
	sys.build_time = 1790000000;
	CHECK(rsu_policy(&sys, h, 0) == RSU_E_SAME, "same version");
	snprintf(sys.version, sizeof(sys.version), "0.1.0");
	free_pkg(&p);

	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 64 * 1024, &g_sk, 1, 2, 1);
	rsu_header_parse(p.data, p.len, h, err, sizeof(err));
	CHECK(rsu_policy(&sys, h, RSU_FORCE) == RSU_E_BOOTLOADER, "needs bootloader 2");
	sys.bootloader = 2;
	CHECK(rsu_policy(&sys, h, 0) == RSU_OK, "bootloader 2 present");
	sys.bootloader = 1;
	free_pkg(&p);

	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 64 * 1024, &g_sk, 1, 1, 2);
	rsu_header_parse(p.data, p.len, h, err, sizeof(err));
	CHECK(rsu_policy(&sys, h, 0) == RSU_E_UPDATER, "needs updater 2");
	free_pkg(&p);

	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 64 * 1024, NULL, 1, 1, 1);
	CHECK(rsu_header_parse(p.data, p.len, h, err, sizeof(err)) == 0 && !h->has_sig, "unsigned parses");
	CHECK(rsu_policy(&sys, h, RSU_ALLOW_UNSIGNED) == RSU_E_UNSIGNED, "unsigned: never on a release build");
	sys.release = false;
	CHECK(rsu_policy(&sys, h, 0) == RSU_E_UNSIGNED, "unsigned on a dev build: only when allowed");
	CHECK(rsu_policy(&sys, h, RSU_ALLOW_UNSIGNED) == RSU_OK, "unsigned on a dev build, allowed");
	sys.release = true;
	{
		struct pkg d;

		/* (review) a signed development package: never on a release
		 * build unless asked (--allow-dev); fine on a dev build */
		g_pkg_variant = "dev";
		make_pkg(&d, "retrostone2", "0.2.0", 1790000000, 64 * 1024, &g_sk, 1, 1, 1);
		g_pkg_variant = "release";
		rsu_header_parse(d.data, d.len, h, err, sizeof(err));
		CHECK(!strcmp(h->m.variant, "dev") && rsu_policy(&sys, h, 0) == RSU_E_VARIANT,
		      "signed dev package on a release build: refused");
		CHECK(rsu_policy(&sys, h, RSU_FORCE | RSU_ALLOW_UNSIGNED) == RSU_E_VARIANT, "... even forced");
		CHECK(rsu_policy(&sys, h, RSU_ALLOW_DEV) == RSU_OK, "... unless --allow-dev");
		CHECK(!strcmp(rsu_err_code(RSU_E_VARIANT), "variant"), "error code 'variant'");
		sys.release = false;
		CHECK(rsu_policy(&sys, h, 0) == RSU_OK, "dev package on a dev build");
		sys.release = true;
		free_pkg(&d);
		rsu_header_parse(p.data, p.len, h, err, sizeof(err));
	}
	/* damaged containers */
	p.data[148] ^= 1;
	CHECK(rsu_header_parse(p.data, p.len, h, err, sizeof(err)) < 0, "bad tar checksum");
	p.data[148] ^= 1;
	CHECK(rsu_header_parse(p.data, 700, h, err, sizeof(err)) < 0, "truncated head");
	CHECK(rsu_header_parse((const uint8_t *)"hello", 5, h, err, sizeof(err)) < 0, "not a package");
	free_pkg(&p);
	free(h);
}

static void test_json(void)
{
	const char *t = "[{\"tag_name\":\"v0.2.0\",\"draft\":false,\"body\":\"caf\\u00e9 \\ud83d\\ude00\\n\\\"x\\\"\","
			"\"assets\":[{\"name\":\"a\",\"size\":12.0}],\"n\":null,\"t\":true}]";
	char err[128];
	struct json *j;

	printf("== JSON\n");
	j = json_parse(t, strlen(t), err, sizeof(err));
	CHECK(j && j->type == JSON_ARR && j->child, "parse: %s", err);
	if (j && j->child) {
		const struct json *r = j->child;

		CHECK(!strcmp(json_str(r, "tag_name"), "v0.2.0") && !json_bool(r, "draft", true) &&
		      json_bool(r, "t", false), "members");
		CHECK(!strcmp(json_str(r, "body"), "caf\xc3\xa9 \xf0\x9f\x98\x80\n\"x\""), "escapes and UTF-8");
		CHECK(json_num(json_get(r, "assets")->child, "size", 0) == 12.0, "number");
	}
	json_free(j);
	CHECK(!json_parse("[1,2", 4, err, sizeof(err)), "unterminated");
	CHECK(!json_parse("{\"a\":1}x", 8, err, sizeof(err)), "trailing data");
	{
		char deep[300];

		memset(deep, '[', 200);
		deep[200] = 0;
		CHECK(!json_parse(deep, 200, err, sizeof(err)), "too deep");
	}
}

/* ------------------------------------------------ the fake system tree */
/* fw_printenv / fw_setenv on a text file ("name=value" lines) */
static void make_fake_env_tools(const char *root)
{
	char p[1024];

	snprintf(p, sizeof(p), "%s/bin/fw_printenv", root);
	put_text(p,
		 "#!/bin/sh\n"
		 "# fake fw_printenv -c CONFIG -n NAME (test_update.c)\n"
		 "E=%s/env.txt\n"
		 "[ -f \"$E\" ] || exit 1\n"
		 "v=$(grep \"^$4=\" \"$E\" | tail -n 1)\n"
		 "[ -n \"$v\" ] || exit 1\n"
		 "echo \"${v#*=}\"\n", root);
	snprintf(p, sizeof(p), "%s/bin/fw_setenv", root);
	put_text(p,
		 "#!/bin/sh\n"
		 "# fake fw_setenv -c CONFIG -s FILE: one atomic rewrite\n"
		 "E=%s/env.txt\n"
		 "cp \"$E\" \"$E.new\" || exit 1\n"
		 "while read -r n v; do\n"
		 "  grep -v \"^$n=\" \"$E.new\" > \"$E.tmp\"; mv \"$E.tmp\" \"$E.new\"\n"
		 "  [ -n \"$v\" ] && echo \"$n=$v\" >> \"$E.new\"\n"
		 "done < \"$4\"\n"
		 "echo \"$4\" >> %s/setenv.calls\n"
		 "mv \"$E.new\" \"$E\"\n", root, root);
}

static void init_ctx(struct upd_ctx *c, const char *root)
{
	upd_ctx_init(c, root);
	snprintf(c->fw_printenv, sizeof(c->fw_printenv), "%s/bin/fw_printenv", root);
	snprintf(c->fw_setenv, sizeof(c->fw_setenv), "%s/bin/fw_setenv", root);
	c->ignore_battery = false;
}

/* a system at version ver, booted from slot, with env */
static void make_system(const char *root, const char *ver, const char *variant, const char *cmdline,
			const char *env, bool ab)
{
	char p[1024], key[512];

	snprintf(p, sizeof(p), "rm -rf '%s'", root);
	if (system(p) != 0)
		exit(2);
	mkdirs(root);
	snprintf(p, sizeof(p), "%s/etc/rsos/version.env", root);
	put_text(p, "RSOS_VERSION='%s'\nRSOS_VARIANT='%s'\nRSOS_BUILD_TIME='1780000000'\nRSOS_BUILD_DATE='2026-05-28'\n"
		 "RSOS_BOARD_ID='retrostone2'\n", ver, variant);
	snprintf(p, sizeof(p), "%s/etc/rsos/board.ini", root);
	put_text(p, "name = RetroStone2\nab_update = %s  # test\n", ab ? "auto" : "0");
	snprintf(p, sizeof(p), "%s/etc/fw_env.config", root);
	put_text(p, "/dev/null 0x0 0x10000\n");
	snprintf(p, sizeof(p), "%s/proc/cmdline", root);
	put_text(p, "%s\n", cmdline);
	snprintf(p, sizeof(p), "%s/env.txt", root);
	put_text(p, "%s", env);
	rsu_pubkey_format(&g_pk, "test", key, sizeof(key));
	snprintf(p, sizeof(p), "%s/usr/share/rsos/update.pub", root);
	put_text(p, "%s", key);
	snprintf(p, sizeof(p), "%s/data/rsos", root);
	mkdirs(p);
	snprintf(p, sizeof(p), "%s/run/rsos", root);
	mkdirs(p);
	snprintf(p, sizeof(p), "%s/sys/class/power_supply/bat", root);
	mkdirs(p);
	snprintf(p, sizeof(p), "%s/sys/class/power_supply/bat/type", root);
	put_text(p, "Battery\n");
	snprintf(p, sizeof(p), "%s/sys/class/power_supply/bat/capacity", root);
	put_text(p, "80\n");
	snprintf(p, sizeof(p), "%s/sys/class/power_supply/bat/status", root);
	put_text(p, "Discharging\n");
	make_fake_env_tools(root);
}

static bool env_is(const char *root, const char *name, const char *want)
{
	char p[1024], *t, line[256];
	bool r;

	snprintf(p, sizeof(p), "%s/env.txt", root);
	t = get_file(p, NULL);
	if (!t)
		return false;
	{
		/* a leading newline makes the first line match too; want "" = unset */
		size_t n = strlen(t);
		char *b = malloc(n + 2);

		b[0] = '\n';
		memcpy(b + 1, t, n + 1);
		if (want[0])
			snprintf(line, sizeof(line), "\n%s=%s\n", name, want);
		else
			snprintf(line, sizeof(line), "\n%s=", name);
		r = want[0] ? strstr(b, line) != NULL : strstr(b, line) == NULL;
		free(b);
	}
	free(t);
	return r;
}

static void test_slots(void)
{
	char root[400];
	struct upd_ctx c;
	static const struct {
		const char *cmdline;
		const char *env;
		enum rsu_err want;
		const char *target;
	} v[] = {
		{ "root=/dev/mmcblk0p2 rootwait ro rsos.slot=a rsos.boot=pending", "rsos_slot=a\nrsos_ok=1\n", RSU_OK,
		  "/dev/mmcblk0p3" },
		{ "console=ttyS0 root=/dev/mmcblk0p3 rsos.slot=b", "rsos_slot=b\nrsos_ok=1\nrsos_fails=1\n", RSU_OK,
		  "/dev/mmcblk0p2" },
		{ "root=/dev/sda3 rsos.slot=b", "rsos_slot=b\nrsos_ok=1\n", RSU_OK, "/dev/sda2" },
		{ "root=/dev/nvme0n1p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", RSU_OK, "/dev/nvme0n1p3" },
		{ "root=/dev/mmcblk0p3 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", RSU_E_SLOT, "" },
		{ "root=PARTUUID=1234-02 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", RSU_E_SLOT, "" },
		{ "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=0\nrsos_tries=2\n", RSU_E_UNCONFIRMED, "" },
		{ "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=b\nrsos_ok=0\nrsos_tries=3\n", RSU_E_RESTART, "" },
		{ "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=b\nrsos_ok=1\n", RSU_E_UNCONFIRMED, "" },
		{ "root=/dev/mmcblk0p2 rsos.slot=a", "", RSU_E_ENV, "" },
	};

	printf("== slot selection\n");
	wpath(root, sizeof(root), "slots");
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		enum rsu_err e;

		make_system(root, "0.1.0", "release", v[i].cmdline, v[i].env, true);
		init_ctx(&c, root);
		upd_load_system(&c);
		e = upd_slots(&c);
		CHECK(e == v[i].want && (e || !strcmp(c.target, v[i].target)), "%s | %s -> %s %s (want %s %s)",
		      v[i].cmdline, v[i].env, rsu_err_code(e), c.target, rsu_err_code(v[i].want), v[i].target);
	}
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2", "", true);
	init_ctx(&c, root);
	upd_load_system(&c);
	CHECK(!c.ab && strstr(c.ab_reason, "rsos.slot"), "no rsos.slot=: no A/B (%s)", c.ab_reason);
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\n", false);
	init_ctx(&c, root);
	upd_load_system(&c);
	CHECK(!c.ab && strstr(c.ab_reason, "ab_update"), "board.ini ab_update = 0 (%s)", c.ab_reason);
}

/* ------------------------------------------------------------- install */
static void write_pkg(const char *path, const struct pkg *p)
{
	put_file(path, p->data, p->len);
}

/* a 1 MiB "slot" full of the old system (0x55) */
static void make_slot(const char *path, size_t size)
{
	uint8_t *b = malloc(size);

	memset(b, 0x55, size);
	put_file(path, b, size);
	free(b);
}

static bool slot_equals(const char *path, const uint8_t *img, size_t n)
{
	size_t len;
	char *t = get_file(path, &len);
	bool r = t && len >= n && !memcmp(t, img, n);

	free(t);
	return r;
}

static bool slot_untouched(const char *path, size_t n)
{
	size_t len;
	uint8_t *t = (uint8_t *)get_file(path, &len);
	bool r = t && len == n;

	for (size_t i = 0; r && i < n; i++)
		r = t[i] == 0x55;
	free(t);
	return r;
}

static void test_install(void)
{
	char root[400], pkgp[1024], slot[1024], st[1024];
	struct upd_ctx c;
	struct pkg p;
	struct rsu_header *h = malloc(sizeof(*h));
	enum rsu_err e;
	const char *env_a = "rsos_slot=a\nrsos_ok=1\nrsos_tries=0\nrsos_fails=1\nrsos_fallback=b\n";
	const size_t SLOT = 2u << 20;
	char *t;

	printf("== install into a file slot\n");
	wpath(root, sizeof(root), "sys");
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a rsos.boot=pending", env_a, true);
	snprintf(slot, sizeof(slot), "%s/slot-b.img", root);
	make_slot(slot, SLOT);
	snprintf(pkgp, sizeof(pkgp), "%s/data/update/retrostoneos-0.2.0-retrostone2.rsu", root);
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 1u << 20, &g_sk, 7, 1, 1);
	write_pkg(pkgp, &p);

	/* the local scan finds it */
	{
		struct upd_found *f = malloc(sizeof(*f));

		init_ctx(&c, root);
		upd_load_system(&c);
		e = upd_scan_local(&c, NULL, f);
		CHECK(e == RSU_OK && f->valid && f->installable && !strcmp(f->version, "0.2.0") && f->is_signed &&
		      strstr(f->notes, "things"), "local scan: %s %s", rsu_err_code(e), f->version);
		free(f);
	}

	/* battery low without a charger: refused, nothing written */
	snprintf(st, sizeof(st), "%s/sys/class/power_supply/bat/capacity", root);
	put_text(st, "20\n");
	init_ctx(&c, root);
	snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
	upd_load_system(&c);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_BATTERY && slot_untouched(slot, SLOT), "battery 20%%: %s", rsu_err_code(e));
	snprintf(st, sizeof(st), "%s/sys/class/power_supply/ac/type", root);
	put_text(st, "Mains\n");
	snprintf(st, sizeof(st), "%s/sys/class/power_supply/ac/online", root);
	put_text(st, "1\n");
	{
		int pct;
		bool ac;

		CHECK(upd_battery_ok(&c, &pct, &ac) && ac && pct == 20, "20%% with the charger: fine");
	}

	/* a good install; slot b was marked bad by U-Boot (an earlier update
	 * that never started), and a confirmation of slot a (rsos-boot-ok)
	 * holds the boot state lock for a second meanwhile */
	snprintf(st, sizeof(st), "%s/env.txt", root);
	put_text(st, "%srsos_bad=b\nrsos_rounds=1\n", env_a);
	init_ctx(&c, root);
	snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
	upd_load_system(&c);
	{
		char lp[1024];
		struct timespec t0, t1;
		pid_t pid;
		int status = 0;

		snprintf(lp, sizeof(lp), "%s/run/rsos/bootenv.lock", root);
		pid = fork();
		if (pid == 0) {
			int fd = open(lp, O_WRONLY | O_CREAT, 0644);

			if (fd < 0 || flock(fd, LOCK_EX) < 0)
				_exit(1);
			usleep(1000000);
			/* what rsos-boot-ok writes for slot a */
			put_text(st, "%srsos_bad=b\nrsos_good=a\n", "rsos_slot=a\nrsos_ok=1\nrsos_tries=0\nrsos_fails=0\n");
			_exit(0);
		}
		usleep(200000);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		e = upd_apply_file(&c, pkgp, false, h);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		waitpid(pid, &status, 0);
		CHECK(e == RSU_OK, "install: %s (%s)", rsu_err_code(e), c.err);
		CHECK((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 600 &&
		      WIFEXITED(status) && WEXITSTATUS(status) == 0,
		      "(review) the slot switch waited for rsos-boot-ok's lock (bootenv.lock)");
	}
	CHECK(slot_equals(slot, p.image, p.image_len), "slot b holds the new image");
	CHECK(env_is(root, "rsos_slot", "b") && env_is(root, "rsos_ok", "0") && env_is(root, "rsos_tries", "3") &&
	      env_is(root, "rsos_fails", "0") && env_is(root, "rsos_fallback", ""), "environment switched to b, on trial");
	CHECK(env_is(root, "rsos_bad", "") && env_is(root, "rsos_rounds", "") && env_is(root, "rsos_good", "a"),
	      "(review) the updater's write clears the bad mark and the failure budget");
	snprintf(st, sizeof(st), "%s/setenv.calls", root);
	t = get_file(st, NULL);
	CHECK(t && strchr(t, '\n') == t + strlen(t) - 1, "exactly one fw_setenv call");
	free(t);
	upd_state_path(&c, st, sizeof(st));
	t = get_file(st, NULL);
	CHECK(t && strstr(t, "version=0.2.0\n") && strstr(t, "slot=b\n") && strstr(t, "file=\n"), "state file: %s", t);
	free(t);
	CHECK(access(pkgp, F_OK) == 0, "the user's package is kept");

	/* the same again before the restart: refused */
	init_ctx(&c, root);
	snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
	upd_load_system(&c);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_RESTART, "second install before the restart: %s", rsu_err_code(e));
	{
		char ev[32], ver[64];

		upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));
		CHECK(!strcmp(ev, "pending"), "boot event before the restart: %s", ev);
	}

	/* a tampered payload: refused before anything is written */
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", env_a, true);
	make_slot(slot, SLOT);
	p.data[p.len - 1024 - 700] ^= 0x40;       /* inside the compressed payload */
	write_pkg(pkgp, &p);
	init_ctx(&c, root);
	snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
	upd_load_system(&c);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_PAYLOAD && slot_untouched(slot, SLOT) && env_is(root, "rsos_slot", "a"),
	      "tampered payload: %s, slot and environment untouched", rsu_err_code(e));
	p.data[p.len - 1024 - 700] ^= 0x40;
	free_pkg(&p);

	/* a package signed by another key */
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 1u << 20, &g_sk2, 7, 1, 1);
	write_pkg(pkgp, &p);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_BADSIG && slot_untouched(slot, SLOT) && env_is(root, "rsos_slot", "a"),
	      "bad signature: %s, nothing written", rsu_err_code(e));
	free_pkg(&p);

	/* wrong board / older */
	make_pkg(&p, "orangepi5", "0.2.0", 1790000000, 1u << 20, &g_sk, 7, 1, 1);
	write_pkg(pkgp, &p);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_BOARD && slot_untouched(slot, SLOT), "wrong board: %s", rsu_err_code(e));
	free_pkg(&p);
	make_pkg(&p, "retrostone2", "0.0.9", 1790000000, 1u << 20, &g_sk, 7, 1, 1);
	write_pkg(pkgp, &p);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_OLDER && slot_untouched(slot, SLOT), "downgrade: %s", rsu_err_code(e));
	c.flags = RSU_FORCE;
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_OK && slot_equals(slot, p.image, p.image_len), "downgrade forced (UART): %s", rsu_err_code(e));
	free_pkg(&p);

	/* an unsigned package on a development build */
	make_system(root, "0.1-dev", "dev", "root=/dev/mmcblk0p2 rsos.slot=a", env_a, true);
	make_slot(slot, SLOT);
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 1u << 20, NULL, 9, 1, 1);
	write_pkg(pkgp, &p);
	init_ctx(&c, root);
	snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
	upd_load_system(&c);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_UNSIGNED && slot_untouched(slot, SLOT), "unsigned, not allowed: %s", rsu_err_code(e));
	c.flags = RSU_ALLOW_UNSIGNED;
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_OK && slot_equals(slot, p.image, p.image_len), "unsigned, allowed on dev: %s", rsu_err_code(e));
	free_pkg(&p);

	/* an interrupted install ("power cut" after 1.5 MiB of the image) */
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", env_a, true);
	make_slot(slot, SLOT);
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 2u << 20, &g_sk, 11, 1, 1);
	write_pkg(pkgp, &p);
	{
		pid_t pid = fork();
		int status = 0;

		if (pid == 0) {
			setenv("RSOS_UPDATE_TEST_ABORT", "write:1572864", 1);
			init_ctx(&c, root);
			snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
			upd_load_system(&c);
			upd_apply_file(&c, pkgp, false, h);
			_exit(0);
		}
		waitpid(pid, &status, 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 99, "the child stopped in the middle (status %d)", status);
	}
	{
		size_t len;
		uint8_t *s = (uint8_t *)get_file(slot, &len);
		bool zero = s != NULL;

		for (size_t i = 0; zero && i < 65536; i++)
			zero = s[i] == 0;
		CHECK(zero, "the interrupted slot has no superblock (never a fallback target)");
		free(s);
	}
	CHECK(env_is(root, "rsos_slot", "a") && env_is(root, "rsos_ok", "1") && env_is(root, "rsos_fallback", "b"),
	      "the environment is untouched: slot a still boots");
	upd_state_path(&c, st, sizeof(st));
	CHECK(access(st, F_OK) < 0, "no state file");
	/* and the next attempt completes */
	init_ctx(&c, root);
	snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
	upd_load_system(&c);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_OK && slot_equals(slot, p.image, p.image_len) && env_is(root, "rsos_slot", "b"),
	      "the retry installs: %s", rsu_err_code(e));
	free_pkg(&p);

	/* a slot smaller than the image */
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", env_a, true);
	make_slot(slot, 512 * 1024);
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 1u << 20, &g_sk, 7, 1, 1);
	write_pkg(pkgp, &p);
	init_ctx(&c, root);
	snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
	upd_load_system(&c);
	e = upd_apply_file(&c, pkgp, false, h);
	CHECK(e == RSU_E_SPACE && slot_untouched(slot, 512 * 1024), "slot too small: %s", rsu_err_code(e));
	/* verify only (the CLI's verify --full) */
	e = upd_verify_file(&c, pkgp, true, h);
	CHECK(e == RSU_OK, "verify --full: %s", rsu_err_code(e));
	free_pkg(&p);
	free(h);
}

/* ----------------------------------------------------- after a restart */
static void test_boot(void)
{
	char root[400], st[1024], dl[1024], pth[1024], ev[32], ver[64];
	struct upd_ctx c;

	printf("== after the restart\n");
	wpath(root, sizeof(root), "boot");
	/* the new system runs, not confirmed yet */
	make_system(root, "0.2.0", "release", "root=/dev/mmcblk0p3 rsos.slot=b rsos.boot=pending",
		    "rsos_slot=b\nrsos_ok=0\nrsos_tries=2\n", true);
	snprintf(dl, sizeof(dl), "%s/data/rsos/update/x.rsu", root);
	put_text(dl, "package");
	snprintf(st, sizeof(st), "%s/data/rsos/update-state.ini", root);
	put_text(st, "version=0.2.0\nslot=b\nfrom_version=0.1.0\nfrom_slot=a\nfile=%s\nannounced=0\n", dl);
	snprintf(pth, sizeof(pth), "%s/run/rsos/boot-state", root);
	put_text(pth, "pending\n");
	init_ctx(&c, root);
	upd_load_system(&c);
	upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));
	CHECK(!strcmp(ev, "updated") && !strcmp(ver, "0.2.0"), "first boot: %s %s", ev, ver);
	upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));
	CHECK(!strcmp(ev, "updated-waiting") && access(dl, F_OK) == 0, "said once, download kept: %s", ev);
	/* rsos-boot-ok confirmed it */
	snprintf(st, sizeof(st), "%s/run/rsos/boot-state", root);
	put_text(st, "confirmed\n");
	snprintf(st, sizeof(st), "%s/env.txt", root);
	put_text(st, "rsos_slot=b\nrsos_ok=1\nrsos_tries=0\nrsos_fails=0\n");
	upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));
	CHECK(!strcmp(ev, "confirmed") && access(dl, F_OK) < 0, "confirmed: download removed (%s)", ev);
	upd_state_path(&c, st, sizeof(st));
	CHECK(access(st, F_OK) < 0, "state file removed");
	upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));
	CHECK(!strcmp(ev, "none"), "nothing more: %s", ev);

	/* (review) the boot environment cannot be read (fw_printenv fails):
	 * nothing is decided nor removed, the menu asks again later */
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a rsos.boot=pending", "", true);
	put_text(dl, "package");
	snprintf(st, sizeof(st), "%s/data/rsos/update-state.ini", root);
	put_text(st, "version=0.2.0\nslot=b\nfile=%s\nannounced=0\n", dl);
	init_ctx(&c, root);
	upd_load_system(&c);
	{
		enum rsu_err e = upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));

		CHECK(e == RSU_E_ENV && !strcmp(ev, "unknown") && access(dl, F_OK) == 0 && access(st, F_OK) == 0,
		      "environment unreadable: %s %s, state and download kept", rsu_err_code(e), ev);
	}

	/* the new system did not start: U-Boot fell back to a */
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a rsos.boot=pending",
		    "rsos_slot=a\nrsos_ok=1\nrsos_fails=1\nrsos_fallback=b\nrsos_bad=b\n", true);
	put_text(dl, "package");
	snprintf(st, sizeof(st), "%s/data/rsos/update-state.ini", root);
	put_text(st, "version=0.2.0\nslot=b\nfile=%s\nannounced=0\n", dl);
	init_ctx(&c, root);
	upd_load_system(&c);
	upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));
	CHECK(!strcmp(ev, "failed") && access(dl, F_OK) < 0 && access(st, F_OK) < 0, "fallback: %s", ev);

	/* a file outside the download folder is never removed */
	put_text(st, "version=0.2.0\nslot=a\nfile=%s/data/update/mine.rsu\nannounced=1\n", root);
	snprintf(dl, sizeof(dl), "%s/data/update/mine.rsu", root);
	put_text(dl, "package");
	snprintf(pth, sizeof(pth), "%s/run/rsos/boot-state", root);
	put_text(pth, "confirmed\n");
	snprintf(pth, sizeof(pth), "%s/env.txt", root);
	put_text(pth, "rsos_slot=a\nrsos_ok=1\n");
	upd_boot(&c, ev, sizeof(ev), ver, sizeof(ver));
	CHECK(!strcmp(ev, "confirmed") && access(dl, F_OK) == 0, "a user's file stays: %s", ev);
}

/* -------------------------------------------------- a local HTTP server */
struct route {
	const char *path;
	const uint8_t *data;
	size_t len;
	bool ignore_range;
	bool chunked;
	size_t drop_after;            /* close after this many body bytes, once (0: never) */
	int requests;
	int ranged;                   /* requests with a Range header */
	int redirect_to;              /* >= 0: 302 to route index */
};

static struct route g_routes[8];
static int g_nroutes;
static int g_srv_fd = -1;
static int g_port;

static void *server(void *arg)
{
	(void)arg;
	for (;;) {
		char req[4096], out[512];
		int fd = accept(g_srv_fd, NULL, NULL);
		ssize_t n;
		size_t got = 0;
		char path[512];
		long long from = -1, to = -1;
		struct route *r = NULL;

		if (fd < 0)
			break;
		while (got < sizeof(req) - 1 && (n = recv(fd, req + got, sizeof(req) - 1 - got, 0)) > 0) {
			got += (size_t)n;
			req[got] = 0;
			if (strstr(req, "\r\n\r\n"))
				break;
		}
		req[got] = 0;
		if (sscanf(req, "GET %511s", path) != 1) {
			close(fd);
			continue;
		}
		{
			char *rg = strstr(req, "\r\nRange: bytes=");

			if (rg && sscanf(rg + 15, "%lld-%lld", &from, &to) < 1)
				from = -1;
		}
		for (int i = 0; i < g_nroutes; i++)
			if (!strcmp(g_routes[i].path, path))
				r = &g_routes[i];
		if (!r) {
			n = snprintf(out, sizeof(out), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
			send(fd, out, (size_t)n, MSG_NOSIGNAL);
			close(fd);
			continue;
		}
		r->requests++;
		if (from >= 0)
			r->ranged++;
		if (r->redirect_to >= 0) {
			n = snprintf(out, sizeof(out), "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d%s\r\n"
				     "Content-Length: 0\r\n\r\n", g_port, g_routes[r->redirect_to].path);
			send(fd, out, (size_t)n, MSG_NOSIGNAL);
			close(fd);
			continue;
		}
		{
			size_t a = 0, b = r->len;
			bool partial = from >= 0 && !r->ignore_range;
			size_t drop = r->drop_after;

			if (partial) {
				a = (size_t)from;
				if (to >= 0 && (size_t)to + 1 < r->len)
					b = (size_t)to + 1;
				if (a >= r->len) {
					n = snprintf(out, sizeof(out), "HTTP/1.1 416 Range Not Satisfiable\r\n"
						     "Content-Range: bytes */%zu\r\nContent-Length: 0\r\n\r\n", r->len);
					send(fd, out, (size_t)n, MSG_NOSIGNAL);
					close(fd);
					continue;
				}
				n = snprintf(out, sizeof(out), "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %zu-%zu/%zu\r\n"
					     "Content-Length: %zu\r\n\r\n", a, b - 1, r->len, b - a);
			} else if (r->chunked) {
				n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
			} else {
				n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", r->len);
			}
			send(fd, out, (size_t)n, MSG_NOSIGNAL);
			r->drop_after = 0;
			if (r->chunked && !partial) {
				for (size_t o = a; o < b;) {
					size_t k = b - o < 777 ? b - o : 777;

					n = snprintf(out, sizeof(out), "%zx;ext=1\r\n", k);
					send(fd, out, (size_t)n, MSG_NOSIGNAL);
					send(fd, r->data + o, k, MSG_NOSIGNAL);
					send(fd, "\r\n", 2, MSG_NOSIGNAL);
					o += k;
				}
				send(fd, "0\r\n\r\n", 5, MSG_NOSIGNAL);
			} else {
				size_t end = drop && a + drop < b ? a + drop : b;

				for (size_t o = a; o < end;) {
					ssize_t w = send(fd, r->data + o, end - o < 65536 ? end - o : 65536, MSG_NOSIGNAL);

					if (w <= 0)
						break;
					o += (size_t)w;
				}
			}
		}
		shutdown(fd, SHUT_RDWR);
		close(fd);
	}
	return NULL;
}

static struct route *add_route(const char *path, const void *data, size_t len)
{
	struct route *r = &g_routes[g_nroutes++];

	memset(r, 0, sizeof(*r));
	r->path = path;
	r->data = data;
	r->len = len;
	r->redirect_to = -1;
	return r;
}

static void start_server(void)
{
	struct sockaddr_in a = { 0 };
	socklen_t al = sizeof(a);
	pthread_t t;
	int one = 1;

	g_srv_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	setsockopt(g_srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(g_srv_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_srv_fd, 8) < 0) {
		printf("cannot start the test server\n");
		exit(2);
	}
	getsockname(g_srv_fd, (struct sockaddr *)&a, &al);
	g_port = ntohs(a.sin_port);
	pthread_create(&t, NULL, server, NULL);
	pthread_detach(t);
}

static void die_halfway(struct upd_ctx *c, const char *phase, uint64_t done, uint64_t total)
{
	(void)c;
	(void)phase;
	if (done > total / 2)
		_exit(42);
}

static void test_download(void)
{
	char root[400], url[256], path[1024], part[1040];
	struct upd_ctx c;
	struct pkg p;
	struct route *r;
	enum rsu_err e;
	size_t len;
	char *got;

	printf("== downloads (local HTTP server)\n");
	wpath(root, sizeof(root), "dl");
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", true);
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 3u << 20, &g_sk, 3, 1, 1);
	r = add_route("/dl/pkg.rsu", p.data, p.len);
	r->drop_after = p.len / 3;            /* the first connection breaks after a third */
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/dl/pkg.rsu", g_port);

	init_ctx(&c, root);
	c.allow_http = true;
	upd_load_system(&c);
	e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
	got = get_file(path, &len);
	CHECK(e == RSU_OK && got && len == p.len && !memcmp(got, p.data, len), "download completed: %s (%s)",
	      rsu_err_code(e), c.err);
	CHECK(r->requests == 2 && r->ranged == 1, "resumed with a Range request (%d requests, %d ranged)", r->requests,
	      r->ranged);
	free(got);
	snprintf(part, sizeof(part), "%s.part", path);
	CHECK(access(part, F_OK) < 0, "no .part left");
	e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
	CHECK(e == RSU_OK && r->requests == 2, "already downloaded: no request");

	/* (review) a damaged file of the right size (a bad resume, a disk
	 * error) is taken as downloaded; the install finds it damaged and
	 * deletes it, so the next attempt downloads it again */
	{
		char slot[1024];
		struct rsu_header *h = malloc(sizeof(*h));
		uint8_t *bad = malloc(p.len);

		memcpy(bad, p.data, p.len);
		bad[p.len - 1024 - 700] ^= 0x40;          /* inside the compressed payload */
		put_file(path, bad, p.len);
		snprintf(slot, sizeof(slot), "%s/slot-b.img", root);
		make_slot(slot, 4u << 20);
		snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
		e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
		e = e ? e : upd_apply_file(&c, path, true, h);
		CHECK(e == RSU_E_PAYLOAD && access(path, F_OK) < 0 && slot_untouched(slot, 4u << 20),
		      "damaged download: %s, deleted, nothing written", rsu_err_code(e));
		r->requests = 0;
		e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
		got = get_file(path, &len);
		CHECK(e == RSU_OK && r->requests >= 1 && got && len == p.len && !memcmp(got, p.data, len),
		      "downloaded again: %s (%d requests)", rsu_err_code(e), r->requests);
		free(got);
		/* a user's file (not ours) is never deleted */
		put_file(path, bad, p.len);
		e = upd_apply_file(&c, path, false, h);
		CHECK(e == RSU_E_PAYLOAD && access(path, F_OK) == 0, "a damaged user's file is kept: %s", rsu_err_code(e));
		c.target_dev[0] = 0;
		free(bad);
		free(h);
	}
	unlink(path);

	/* (review) the download takes the update lock: not while another
	 * rsos-update downloads or installs */
	{
		char lp[1024];
		pid_t pid;
		int status = 0;

		snprintf(lp, sizeof(lp), "%s/run/rsos/update.lock", root);
		pid = fork();
		if (pid == 0) {
			int fd = open(lp, O_RDWR | O_CREAT, 0644);

			if (fd < 0 || flock(fd, LOCK_EX) < 0)
				_exit(1);
			usleep(800000);
			_exit(0);
		}
		usleep(200000);
		r->requests = 0;
		e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
		CHECK(e == RSU_E_BUSY && r->requests == 0 && access(path, F_OK) < 0, "busy: %s", rsu_err_code(e));
		waitpid(pid, &status, 0);
		CHECK(upd_lock(&c) == RSU_OK && c.lock_fd >= 0, "then free: taken");
		e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
		CHECK(e == RSU_OK && c.lock_fd >= 0, "a download under the caller's lock keeps it held");
		upd_unlock(&c);
		unlink(path);
	}

	/* the process dies in the middle (power cut, killed): the next run
	 * resumes the .part with a Range request */
	r->requests = r->ranged = 0;
	{
		pid_t pid = fork();
		int status = 0;

		if (pid == 0) {
			c.progress = die_halfway;
			upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
			_exit(0);
		}
		waitpid(pid, &status, 0);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42 && access(part, F_OK) == 0,
		      "the first process died halfway, the part is there");
	}
	e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
	got = get_file(path, &len);
	CHECK(e == RSU_OK && got && len == p.len && !memcmp(got, p.data, len) && r->ranged == 1,
	      "resumed by the next run (%d ranged requests)", r->ranged);
	free(got);
	unlink(path);

	/* cancelled (the menu's STOP): the part stays */
	c.cancel = 1;
	e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
	CHECK(e == RSU_E_CANCELLED, "cancelled: %s", rsu_err_code(e));
	c.cancel = 0;

	/* a server that ignores Range: starts over, still correct */
	r->ignore_range = true;
	r->drop_after = p.len / 4;
	r->requests = r->ranged = 0;
	e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
	got = get_file(path, &len);
	CHECK(e == RSU_OK && got && len == p.len && !memcmp(got, p.data, len) && r->requests == 2,
	      "server without Range: restarted (%s, %d requests)", rsu_err_code(e), r->requests);
	free(got);
	unlink(path);
	r->ignore_range = false;

	/* the file on the server has another size (a new build): refused */
	e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len + 5, path, sizeof(path));
	CHECK(e == RSU_E_HTTP, "size mismatch: %s", rsu_err_code(e));

	/* not enough space: statvfs says so (fake by a huge size) */
	e = upd_download(&c, url, "retrostoneos-9.rsu", (uint64_t)1 << 50, path, sizeof(path));
	CHECK(e == RSU_E_SPACE, "no space: %s", rsu_err_code(e));
	/* 404 */
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/nothing.rsu", g_port);
	e = upd_download(&c, url, "retrostoneos-0.3.rsu", 1000, path, sizeof(path));
	CHECK(e == RSU_E_NOTFOUND, "404: %s", rsu_err_code(e));
	/* https refused when the build has no TLS or the host is plain */
	c.allow_http = false;
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/dl/pkg.rsu", g_port);
	e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
	CHECK(e == RSU_E_TLS, "plain http refused on the device: %s", rsu_err_code(e));
	/* the package itself: the full install from the download */
	c.allow_http = true;
	{
		char slot[1024];
		struct rsu_header *h = malloc(sizeof(*h));

		e = upd_download(&c, url, "retrostoneos-0.2.0-retrostone2.rsu", p.len, path, sizeof(path));
		snprintf(slot, sizeof(slot), "%s/slot-b.img", root);
		make_slot(slot, 4u << 20);
		snprintf(c.target_dev, sizeof(c.target_dev), "%s", slot);
		e = e ? e : upd_apply_file(&c, path, true, h);
		CHECK(e == RSU_OK && slot_equals(slot, p.image, p.image_len), "downloaded package installed: %s",
		      rsu_err_code(e));
		{
			char st[1024], *t;

			upd_state_path(&c, st, sizeof(st));
			t = get_file(st, NULL);
			CHECK(t && strstr(t, "retrostoneos-0.2.0-retrostone2.rsu"), "state names the download to remove");
			free(t);
		}
		free(h);
	}
	free_pkg(&p);
	g_nroutes = 0;
}

static void test_check_net(void)
{
	char root[400], api[256], json[8192];
	struct upd_ctx c;
	struct upd_found *f = malloc(sizeof(*f));
	struct pkg p, pu;
	enum rsu_err e;
	struct route *rel, *pk, *pku, *red;

	printf("== release check (GitHub API on the local server)\n");
	wpath(root, sizeof(root), "net");
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", true);
	make_pkg(&p, "retrostone2", "0.2.0", 1790000000, 1u << 20, &g_sk, 3, 1, 1);
	make_pkg(&pu, "retrostone2", "0.3.0", 1795000000, 1u << 20, NULL, 3, 1, 1);
	snprintf(json, sizeof(json),
		 "[{\"tag_name\":\"v0.3.0-rc1\",\"prerelease\":true,\"draft\":false,\"body\":\"pre\",\"assets\":[]},"
		 " {\"tag_name\":\"v0.2.0\",\"prerelease\":false,\"draft\":false,\"html_url\":\"https://x/r/v0.2.0\","
		 "  \"body\":\"## What's new\\r\\n- faster\",\"assets\":["
		 "   {\"name\":\"retrostoneos-0.2.0-rpi4-64.rsu\",\"size\":1,\"browser_download_url\":\"http://127.0.0.1:%d/nope\"},"
		 "   {\"name\":\"retrostoneos-0.2.0-retrostone2.img.xz\",\"size\":2,\"browser_download_url\":\"http://127.0.0.1:%d/img\"},"
		 "   {\"name\":\"retrostoneos-0.2.0-retrostone2.rsu\",\"size\":%zu,\"browser_download_url\":\"http://127.0.0.1:%d/r/pkg\"}]},"
		 " {\"tag_name\":\"v0.1.0\",\"draft\":false,\"assets\":[{\"name\":\"retrostoneos-0.1.0-retrostone2.rsu\",\"size\":5}]}]",
		 g_port, g_port, p.len, g_port);
	rel = add_route("/releases", json, strlen(json));
	rel->chunked = true;
	red = add_route("/r/pkg", NULL, 0);
	pk = add_route("/pkg", p.data, p.len);
	red->redirect_to = 2;
	pku = add_route("/pkg-unsigned", pu.data, pu.len);
	(void)pku;
	snprintf(api, sizeof(api), "http://127.0.0.1:%d/releases", g_port);

	init_ctx(&c, root);
	c.allow_http = true;
	snprintf(c.api_url, sizeof(c.api_url), "%s", api);
	upd_load_system(&c);
	e = upd_check_net(&c, f);
	CHECK(e == RSU_OK && f->installable && !strcmp(f->version, "0.2.0") && f->size == p.len && f->is_signed &&
	      strstr(f->where, "/r/pkg") && strstr(f->notes, "faster"), "newer release: %s %s %s", rsu_err_code(e),
	      f->version, c.err);
	CHECK(pk->requests == 1 && pk->ranged == 1, "only the package head was fetched (Range after the redirect)");

	/* with pre-releases: 0.3.0-rc1 has no file for this board */
	c.prerelease = true;
	e = upd_check_net(&c, f);
	CHECK(e == RSU_OK && !strcmp(f->version, "0.2.0"), "a pre-release without files is skipped");
	c.prerelease = false;

	/* up to date */
	make_system(root, "0.2.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", true);
	upd_load_system(&c);
	e = upd_check_net(&c, f);
	CHECK(e == RSU_E_SAME && !strcmp(f->version, "0.2.0"), "up to date: %s", rsu_err_code(e));

	/* no A/B (Raspberry Pi): informed, not installable, the image named */
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", false);
	upd_load_system(&c);
	e = upd_check_net(&c, f);
	CHECK(e == RSU_OK && !f->installable && f->verdict == RSU_E_NOAB && strstr(f->name, ".img.xz"),
	      "non-A/B board: %s %s %s", rsu_err_code(e), rsu_err_code(f->verdict), f->name);

	/* a clock before the build: TLS would fail, said clearly */
	make_system(root, "0.1.0", "release", "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", true);
	{
		char pth[1024];

		snprintf(pth, sizeof(pth), "%s/etc/rsos/version.env", root);
		put_text(pth, "RSOS_VERSION='0.1.0'\nRSOS_VARIANT='release'\nRSOS_BUILD_TIME='%lld'\nRSOS_BOARD_ID='retrostone2'\n",
			 (long long)time(NULL) + 30 * 86400);
	}
	upd_load_system(&c);
	e = upd_check_net(&c, f);
	CHECK(e == RSU_E_CLOCK, "clock not set: %s", rsu_err_code(e));

	/* an unsigned package on the server: never offered */
	make_system(root, "0.1-dev", "dev", "root=/dev/mmcblk0p2 rsos.slot=a", "rsos_slot=a\nrsos_ok=1\n", true);
	pk->data = pu.data;
	pk->len = pu.len;
	snprintf(json, sizeof(json),
		 "[{\"tag_name\":\"v0.3.0\",\"draft\":false,\"body\":\"\",\"assets\":["
		 "{\"name\":\"retrostoneos-0.3.0-retrostone2.rsu\",\"size\":%zu,\"browser_download_url\":\"http://127.0.0.1:%d/pkg\"}]}]",
		 pu.len, g_port);
	rel->len = strlen(json);
	upd_load_system(&c);
	e = upd_check_net(&c, f);
	CHECK(e == RSU_OK && !f->installable && f->verdict == RSU_E_UNSIGNED, "unsigned from the network: %s",
	      rsu_err_code(f->verdict));
	/* (review) a nonsense asset size (negative, infinite, below one
	 * byte): never cast to an integer (undefined behaviour), not offered */
	{
		static const char *const sizes[] = { "-5", "1e999", "-1e999", "0.5", "1e300" };

		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			snprintf(json, sizeof(json),
				 "[{\"tag_name\":\"v0.3.0\",\"draft\":false,\"body\":\"\",\"assets\":["
				 "{\"name\":\"retrostoneos-0.3.0-retrostone2.rsu\",\"size\":%s,"
				 "\"browser_download_url\":\"http://127.0.0.1:%d/pkg\"}]}]", sizes[i], g_port);
			rel->len = strlen(json);
			e = upd_check_net(&c, f);
			CHECK(e == RSU_OK && !f->installable && f->size == 0 && f->verdict == RSU_E_HTTP,
			      "size %s: %s, size %" PRIu64 ", %s", sizes[i], rsu_err_code(e), f->size,
			      rsu_err_code(f->verdict));
		}
	}
	/* a server that is not there */
	snprintf(c.api_url, sizeof(c.api_url), "http://127.0.0.1:1/releases");
	e = upd_check_net(&c, f);
	CHECK(e == RSU_E_NETWORK, "no server: %s", rsu_err_code(e));
	free_pkg(&p);
	free_pkg(&pu);
	free(f);
	g_nroutes = 0;
}

static void test_url(void)
{
	char s[8], h[64], p[256];
	int port;

	printf("== URLs\n");
	CHECK(http_parse_url("https://api.github.com/repos/x/releases?per_page=10", s, sizeof(s), h, sizeof(h), &port, p,
			     sizeof(p)) == 0 && !strcmp(s, "https") && !strcmp(h, "api.github.com") && port == 443 &&
	      !strcmp(p, "/repos/x/releases?per_page=10"), "https URL");
	CHECK(http_parse_url("http://127.0.0.1:8080", s, sizeof(s), h, sizeof(h), &port, p, sizeof(p)) == 0 &&
	      port == 8080 && !strcmp(p, "/"), "port, no path");
	CHECK(http_parse_url("ftp://x/y", s, sizeof(s), h, sizeof(h), &port, p, sizeof(p)) < 0, "ftp refused");
	CHECK(http_parse_url("https://user@x/y", s, sizeof(s), h, sizeof(h), &port, p, sizeof(p)) < 0, "credentials refused");
	CHECK(http_parse_url("https://x/a b", s, sizeof(s), h, sizeof(h), &port, p, sizeof(p)) < 0, "space refused");
}

int main(int argc, char **argv)
{
	char cmd[1100];

	if (argc < 2) {
		fprintf(stderr, "usage: test_update <work dir>\n");
		return 2;
	}
	snprintf(W, sizeof(W), "%s", argv[1]);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", W, W);
	if (system(cmd) != 0)
		return 2;
	signal(SIGPIPE, SIG_IGN);
	upd_set_log(quiet_log, NULL);
	start_server();
	test_sha256();
	test_keys();
	test_manifest();
	test_versions();
	test_container();
	test_json();
	test_url();
	test_slots();
	test_install();
	test_boot();
	test_download();
	test_check_net();
	printf("test_update: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
