/*
 * rsu.c - see rsu.h.
 */
#include "rsu.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "sha256.h"

/* ---------------------------------------------------------------- errors */
static const struct {
	const char *code, *text;
} g_errs[RSU_E_COUNT] = {
	[RSU_OK] = { "ok", "OK" },
	[RSU_E_FORMAT] = { "format", "This is not a RetroStoneOS update package, or it is damaged" },
	[RSU_E_UPDATER] = { "updater", "This update needs a newer updater: install the previous updates first" },
	[RSU_E_UNSIGNED] = { "unsigned", "This update package is not signed" },
	[RSU_E_BADSIG] = { "badsig", "This update package is not signed by RetroStoneOS, or it was modified" },
	[RSU_E_NOKEY] = { "nokey", "This system has no update key" },
	[RSU_E_BOARD] = { "board", "This update is for another console" },
	[RSU_E_BOOTLOADER] = { "bootloader", "This update needs a newer bootloader: please reflash the SD card" },
	[RSU_E_SAME] = { "same", "This version is already installed" },
	[RSU_E_OLDER] = { "older", "This update is older than the installed version" },
	[RSU_E_PAYLOAD] = { "payload", "The update package is damaged (its content does not match)" },
	[RSU_E_IMAGE] = { "image", "The new system did not write correctly: nothing was changed" },
	[RSU_E_IO] = { "io", "Read or write error" },
	[RSU_E_SPACE] = { "space", "Not enough free space on the SD card" },
	[RSU_E_NOAB] = { "noab", "This console cannot be updated in place: flash the new image to the SD card" },
	[RSU_E_SLOT] = { "slot", "The system slots could not be determined" },
	[RSU_E_UNCONFIRMED] = { "unconfirmed", "The system is still starting up: try again in a minute" },
	[RSU_E_RESTART] = { "restart", "An update is already installed: restart the console first" },
	[RSU_E_ENV] ={ "env", "The boot settings could not be written: nothing was changed" },
	[RSU_E_BATTERY] = { "battery", "Battery too low: charge it to 30% or plug in the charger" },
	[RSU_E_BUSY] = { "busy", "An update is already running" },
	[RSU_E_CLOCK] = { "clock", "The clock is not set: connect to WiFi and wait a minute, or set the date" },
	[RSU_E_NETWORK] = { "network", "No connection to the update server" },
	[RSU_E_TLS] = { "tls", "Secure connection failed" },
	[RSU_E_HTTP] = { "http", "The update server gave an unexpected answer" },
	[RSU_E_NOTFOUND] = { "notfound", "Not found" },
	[RSU_E_CANCELLED] = { "cancelled", "Cancelled" },
	[RSU_E_NOTLS] = { "notls", "This build cannot use HTTPS" },
	[RSU_E_INTERNAL] = { "internal", "Internal error" },
	[RSU_E_VARIANT] = { "variant", "This is a development build of RetroStoneOS: this console installs release updates only" },
};

const char *rsu_err_code(enum rsu_err e)
{
	return (unsigned)e < RSU_E_COUNT && g_errs[e].code ? g_errs[e].code : "internal";
}

const char *rsu_err_text(enum rsu_err e)
{
	return (unsigned)e < RSU_E_COUNT && g_errs[e].text ? g_errs[e].text : "Internal error";
}

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

static size_t cpy(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);

	if (n) {
		size_t c = l < n - 1 ? l : n - 1;

		memcpy(d, s, c);
		d[c] = 0;
	}
	return l;
}

/* -------------------------------------------------------------- manifest */
bool rsu_safe_token(const char *s)
{
	size_t n = 0;

	for (; s[n]; n++) {
		char c = s[n];

		if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '+' || c == '-'))
			return false;
	}
	return n >= 1 && n <= 63 && s[0] != '.' && s[0] != '-';
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *e;
	unsigned long long v;

	if (!isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoull(s, &e, 10);
	if (errno || *e)
		return -1;
	*out = v;
	return 0;
}

/* "\n", "\t", "\\" escapes of the changelog value */
static void unescape(const char *s, char *out, size_t n)
{
	size_t o = 0;

	for (; *s && o + 1 < n; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			out[o++] = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
		} else {
			out[o++] = *s;
		}
	}
	out[o] = 0;
}

enum {
	K_FORMAT = 1 << 0, K_BOARD = 1 << 1, K_VERSION = 1 << 2, K_VARIANT = 1 << 3, K_DATE = 1 << 4,
	K_TIME = 1 << 5, K_MINUPD = 1 << 6, K_BOOTL = 1 << 7, K_PAYLOAD = 1 << 8, K_COMP = 1 << 9,
	K_PSIZE = 1 << 10, K_PSHA = 1 << 11, K_ISIZE = 1 << 12, K_ISHA = 1 << 13, K_LOG = 1 << 14,
};
#define K_REQUIRED (K_FORMAT | K_BOARD | K_VERSION | K_TIME | K_PAYLOAD | K_COMP | K_PSIZE | K_PSHA | K_ISIZE | \
		    K_ISHA)

int rsu_manifest_parse(const char *text, size_t len, struct rsu_manifest *m, char *err, size_t errlen)
{
	char line[RSU_CHANGELOG_MAX * 2 + 64];
	size_t pos = 0;
	unsigned seen = 0;
	int lineno = 0;

	memset(m, 0, sizeof(*m));
	m->min_updater = 1;
	m->bootloader_min = 1;
	cpy(m->variant, "release", sizeof(m->variant));
	if (len > RSU_MANIFEST_MAX) {
		seterr(err, errlen, "manifest too large (%zu bytes)", len);
		return -1;
	}
	if (memchr(text, 0, len)) {
		seterr(err, errlen, "manifest: NUL byte");
		return -1;
	}
	while (pos < len) {
		const char *nl = memchr(text + pos, '\n', len - pos);
		size_t l = nl ? (size_t)(nl - (text + pos)) : len - pos;
		char *k, *v, *eq, *e;
		unsigned bit = 0;
		uint64_t u;

		lineno++;
		if (l >= sizeof(line)) {
			seterr(err, errlen, "manifest line %d too long", lineno);
			return -1;
		}
		memcpy(line, text + pos, l);
		line[l] = 0;
		pos += l + (nl ? 1 : 0);
		if (l && line[l - 1] == '\r')
			line[--l] = 0;
		k = line;
		while (*k == ' ' || *k == '\t')
			k++;
		if (!*k || *k == '#')
			continue;
		eq = strchr(k, '=');
		if (!eq) {
			seterr(err, errlen, "manifest line %d: no '='", lineno);
			return -1;
		}
		v = eq + 1;
		for (e = eq; e > k && (e[-1] == ' ' || e[-1] == '\t'); e--)
			;
		*e = 0;
		while (*v == ' ' || *v == '\t')
			v++;
		for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); e--)
			;
		*e = 0;

		if (!strcmp(k, "format")) {
			bit = K_FORMAT;
			if (parse_u64(v, &u) < 0 || u < 1 || u > 1000)
				goto bad;
			m->format = (int)u;
		} else if (!strcmp(k, "board")) {
			bit = K_BOARD;
			if (!rsu_safe_token(v))
				goto bad;
			cpy(m->board, v, sizeof(m->board));
		} else if (!strcmp(k, "version")) {
			bit = K_VERSION;
			if (!rsu_safe_token(v))
				goto bad;
			cpy(m->version, v, sizeof(m->version));
		} else if (!strcmp(k, "variant")) {
			bit = K_VARIANT;
			if (strcmp(v, "release") && strcmp(v, "dev"))
				goto bad;
			cpy(m->variant, v, sizeof(m->variant));
		} else if (!strcmp(k, "build_date")) {
			bit = K_DATE;
			if (strlen(v) >= sizeof(m->build_date))
				goto bad;
			cpy(m->build_date, v, sizeof(m->build_date));
		} else if (!strcmp(k, "build_time")) {
			bit = K_TIME;
			if (parse_u64(v, &u) < 0 || u == 0 || u > (uint64_t)INT64_MAX)
				goto bad;
			m->build_time = (int64_t)u;
		} else if (!strcmp(k, "min_updater")) {
			bit = K_MINUPD;
			if (parse_u64(v, &u) < 0 || u < 1 || u > 1000000)
				goto bad;
			m->min_updater = (int)u;
		} else if (!strcmp(k, "bootloader_min")) {
			bit = K_BOOTL;
			if (parse_u64(v, &u) < 0 || u < 1 || u > 1000000)
				goto bad;
			m->bootloader_min = (int)u;
		} else if (!strcmp(k, "payload")) {
			bit = K_PAYLOAD;
			if (!rsu_safe_token(v) || !strcmp(v, "manifest") || !strcmp(v, "manifest.sig"))
				goto bad;
			cpy(m->payload, v, sizeof(m->payload));
		} else if (!strcmp(k, "compression")) {
			bit = K_COMP;
			if (strcmp(v, "zstd") && strcmp(v, "none"))
				goto bad;
			cpy(m->compression, v, sizeof(m->compression));
		} else if (!strcmp(k, "payload_size")) {
			bit = K_PSIZE;
			if (parse_u64(v, &u) < 0 || u == 0 || u > (8ull << 30))
				goto bad;
			m->payload_size = u;
		} else if (!strcmp(k, "payload_sha256")) {
			bit = K_PSHA;
			if (sha256_parse_hex(v, m->payload_sha256) < 0)
				goto bad;
		} else if (!strcmp(k, "image_size")) {
			bit = K_ISIZE;
			/* whole 4 KiB blocks: the readback check uses O_DIRECT */
			if (parse_u64(v, &u) < 0 || u == 0 || u > (8ull << 30) || u % 4096)
				goto bad;
			m->image_size = u;
		} else if (!strcmp(k, "image_sha256")) {
			bit = K_ISHA;
			if (sha256_parse_hex(v, m->image_sha256) < 0)
				goto bad;
		} else if (!strcmp(k, "changelog")) {
			bit = K_LOG;
			unescape(v, m->changelog, sizeof(m->changelog));
		} else {
			continue;         /* unknown key: a later minor addition */
		}
		if (seen & bit) {
			seterr(err, errlen, "manifest: \"%s\" given twice", k);
			return -1;
		}
		seen |= bit;
		continue;
bad:
		seterr(err, errlen, "manifest: bad value for \"%s\"", k);
		return -1;
	}
	if ((seen & K_REQUIRED) != K_REQUIRED) {
		static const char *const names[] = { "format", "board", "version", "variant", "build_date",
			"build_time", "min_updater", "bootloader_min", "payload", "compression", "payload_size",
			"payload_sha256", "image_size", "image_sha256" };

		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if ((K_REQUIRED & (1u << i)) && !(seen & (1u << i))) {
				seterr(err, errlen, "manifest: \"%s\" missing", names[i]);
				break;
			}
		return -1;
	}
	if (!strcmp(m->compression, "none") && m->payload_size != m->image_size) {
		seterr(err, errlen, "manifest: uncompressed payload and image sizes differ");
		return -1;
	}
	return 0;
}

int rsu_manifest_format(const struct rsu_manifest *m, char *out, size_t n)
{
	char ph[65], ih[65];
	char log[RSU_CHANGELOG_MAX * 2 + 1];
	size_t o = 0;
	int r;

	for (const char *p = m->changelog; *p && o + 3 < sizeof(log); p++) {
		if (*p == '\n')
			log[o++] = '\\', log[o++] = 'n';
		else if (*p == '\t')
			log[o++] = '\\', log[o++] = 't';
		else if (*p == '\\')
			log[o++] = '\\', log[o++] = '\\';
		else if ((unsigned char)*p >= 0x20)   /* UTF-8 bytes too; control characters dropped */
			log[o++] = *p;
	}
	log[o] = 0;
	sha256_hex(m->payload_sha256, ph);
	sha256_hex(m->image_sha256, ih);
	r = snprintf(out, n,
		     "# RetroStoneOS update package (docs/updates.md)\n"
		     "format = %d\n"
		     "board = %s\n"
		     "version = %s\n"
		     "variant = %s\n"
		     "build_date = %s\n"
		     "build_time = %" PRId64 "\n"
		     "min_updater = %d\n"
		     "bootloader_min = %d\n"
		     "payload = %s\n"
		     "compression = %s\n"
		     "payload_size = %" PRIu64 "\n"
		     "payload_sha256 = %s\n"
		     "image_size = %" PRIu64 "\n"
		     "image_sha256 = %s\n"
		     "changelog = %s\n",
		     m->format, m->board, m->version, m->variant, m->build_date, m->build_time, m->min_updater,
		     m->bootloader_min, m->payload, m->compression, m->payload_size, ph, m->image_size, ih, log);
	if (r < 0 || (size_t)r >= n || r > RSU_MANIFEST_MAX)
		return -1;
	return r;
}

/* ---------------------------------------------------------------- base64 */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int rsu_b64_encode(const uint8_t *in, size_t n, char *out, size_t outn)
{
	size_t o = 0;

	if (outn < (n + 2) / 3 * 4 + 1)
		return -1;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < n)
			v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < n)
			v |= in[i + 2];
		out[o++] = B64[v >> 18 & 63];
		out[o++] = B64[v >> 12 & 63];
		out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? B64[v & 63] : '=';
	}
	out[o] = 0;
	return (int)o;
}

int rsu_b64_decode(const char *in, size_t n, uint8_t *out, size_t outn)
{
	uint32_t v = 0;
	int bits = 0, pad = 0;
	size_t o = 0;

	for (size_t i = 0; i < n; i++) {
		const char *p;

		if (in[i] == '=') {
			pad++;
			continue;
		}
		if (pad)
			return -1;             /* data after the padding */
		p = in[i] ? strchr(B64, in[i]) : NULL;
		if (!p)
			return -1;
		v = v << 6 | (uint32_t)(p - B64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= outn)
				return -1;
			out[o++] = (uint8_t)(v >> bits);
		}
	}
	if (pad > 2 || (n % 4) || bits >= 6)
		return -1;
	return (int)o;
}

/* ------------------------------------------------------------------ keys */
/* The base64 line of a signify file (after "untrusted comment: ...") */
static int signify_blob(const char *text, size_t len, uint8_t *out, size_t outn)
{
	static const char cmt[] = "untrusted comment: ";
	const char *nl, *b, *e;

	if (len < sizeof(cmt) - 1 || memcmp(text, cmt, sizeof(cmt) - 1))
		return -1;
	nl = memchr(text, '\n', len);
	if (!nl)
		return -1;
	b = nl + 1;
	e = memchr(b, '\n', len - (size_t)(b - text));
	if (!e)
		e = text + len;
	/* nothing but blank space after the key line */
	for (const char *p = e; p < text + len; p++)
		if (!isspace((unsigned char)*p))
			return -1;
	if (e > b && e[-1] == '\r')
		e--;
	return rsu_b64_decode(b, (size_t)(e - b), out, outn);
}

int rsu_pubkey_parse(const char *text, size_t len, struct rsu_pubkey *k)
{
	uint8_t b[42];

	if (signify_blob(text, len, b, sizeof(b)) != 42 || b[0] != 'E' || b[1] != 'd')
		return -1;
	memcpy(k->keynum, b + 2, 8);
	memcpy(k->pk, b + 10, 32);
	return 0;
}

int rsu_seckey_parse(const char *text, size_t len, struct rsu_seckey *k)
{
	uint8_t b[104], digest[64];
	struct rsu_pubkey pk;
	int r = -1;

	if (signify_blob(text, len, b, sizeof(b)) != 104 || memcmp(b, "EdBK", 4))
		goto out;
	/* kdfrounds (big-endian) 0: not encrypted (a CI secret has no passphrase) */
	if (b[4] | b[5] | b[6] | b[7])
		goto out;
	/* the checksum: the first 8 bytes of SHA-512(secret key) */
	crypto_sha512(digest, b + 40, 64);
	if (memcmp(digest, b + 24, 8))
		goto out;
	memcpy(k->keynum, b + 32, 8);
	memcpy(k->sk, b + 40, 64);
	/* the public half must be the one of the seed */
	rsu_seckey_public(k, &pk);
	if (memcmp(pk.pk, k->sk + 32, 32))
		goto out;
	r = 0;
out:
	crypto_wipe(b, sizeof(b));
	crypto_wipe(digest, sizeof(digest));
	return r;
}

int rsu_sig_parse(const char *text, size_t len, struct rsu_sig *s)
{
	uint8_t b[74];

	if (signify_blob(text, len, b, sizeof(b)) != 74 || b[0] != 'E' || b[1] != 'd')
		return -1;
	memcpy(s->keynum, b + 2, 8);
	memcpy(s->sig, b + 10, 64);
	return 0;
}

static char *read_small(const char *path, size_t max, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	char *buf;
	ssize_t r;

	if (fd < 0)
		return NULL;
	buf = malloc(max + 1);
	if (!buf) {
		close(fd);
		return NULL;
	}
	r = read(fd, buf, max + 1);
	close(fd);
	if (r < 0 || (size_t)r > max) {
		free(buf);
		return NULL;
	}
	buf[r] = 0;
	*len = (size_t)r;
	return buf;
}

int rsu_pubkey_load(const char *path, struct rsu_pubkey *k)
{
	size_t len;
	char *t = read_small(path, 4096, &len);
	int r;

	if (!t)
		return -1;
	r = rsu_pubkey_parse(t, len, k);
	free(t);
	return r;
}

int rsu_seckey_load(const char *path, struct rsu_seckey *k)
{
	size_t len;
	char *t = read_small(path, 4096, &len);
	int r;

	if (!t)
		return -1;
	r = rsu_seckey_parse(t, len, k);
	crypto_wipe(t, len);
	free(t);
	return r;
}

static int signify_format(const char *comment, const uint8_t *blob, size_t n, char *out, size_t outn)
{
	char b64[256];
	int r;

	if (rsu_b64_encode(blob, n, b64, sizeof(b64)) < 0)
		return -1;
	r = snprintf(out, outn, "untrusted comment: %s\n%s\n", comment, b64);
	crypto_wipe(b64, sizeof(b64));
	return r < 0 || (size_t)r >= outn ? -1 : r;
}

int rsu_pubkey_format(const struct rsu_pubkey *k, const char *comment, char *out, size_t n)
{
	uint8_t b[42] = { 'E', 'd' };

	memcpy(b + 2, k->keynum, 8);
	memcpy(b + 10, k->pk, 32);
	return signify_format(comment, b, sizeof(b), out, n);
}

int rsu_seckey_format(const struct rsu_seckey *k, const char *comment, char *out, size_t n)
{
	uint8_t b[104] = { 'E', 'd', 'B', 'K' };
	uint8_t digest[64];
	int r;

	/* kdfrounds 0, salt 0 (unused without a passphrase) */
	crypto_sha512(digest, k->sk, 64);
	memcpy(b + 24, digest, 8);
	memcpy(b + 32, k->keynum, 8);
	memcpy(b + 40, k->sk, 64);
	r = signify_format(comment, b, sizeof(b), out, n);
	crypto_wipe(b, sizeof(b));
	crypto_wipe(digest, sizeof(digest));
	return r;
}

int rsu_sig_format(const struct rsu_sig *s, const char *comment, char *out, size_t n)
{
	uint8_t b[74] = { 'E', 'd' };

	memcpy(b + 2, s->keynum, 8);
	memcpy(b + 10, s->sig, 64);
	return signify_format(comment, b, sizeof(b), out, n);
}

void rsu_keypair(const uint8_t seed[32], const uint8_t keynum[8], struct rsu_seckey *sk, struct rsu_pubkey *pk)
{
	uint8_t s[32];

	memcpy(s, seed, 32);
	crypto_ed25519_key_pair(sk->sk, pk->pk, s);   /* wipes s */
	memcpy(sk->keynum, keynum, 8);
	memcpy(pk->keynum, keynum, 8);
}

void rsu_seckey_public(const struct rsu_seckey *sk, struct rsu_pubkey *pk)
{
	uint8_t s[32], tmp[64];

	memcpy(s, sk->sk, 32);
	crypto_ed25519_key_pair(tmp, pk->pk, s);
	crypto_wipe(tmp, sizeof(tmp));
	memcpy(pk->keynum, sk->keynum, 8);
}

void rsu_sign(const struct rsu_seckey *k, const void *msg, size_t n, struct rsu_sig *out)
{
	crypto_ed25519_sign(out->sig, k->sk, msg, n);
	memcpy(out->keynum, k->keynum, 8);
}

bool rsu_verify(const struct rsu_pubkey *k, const struct rsu_sig *s, const void *msg, size_t n)
{
	if (memcmp(k->keynum, s->keynum, 8))
		return false;
	return crypto_ed25519_check(s->sig, k->pk, msg, n) == 0;
}

/* ------------------------------------------------------------- container */
static uint64_t tar_octal(const uint8_t *p, size_t n, bool *ok)
{
	uint64_t v = 0;
	size_t i = 0;

	while (i < n && p[i] == ' ')
		i++;
	if (i == n || p[i] < '0' || p[i] > '7') {
		*ok = false;
		return 0;
	}
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
		if (v >> 60) {
			*ok = false;
			return 0;
		}
		v = v << 3 | (uint64_t)(p[i] - '0');
	}
	for (; i < n; i++)
		if (p[i] != ' ' && p[i] != 0) {
			*ok = false;
			return 0;
		}
	return v;
}

static bool tar_block_ok(const uint8_t *b, char name[101], uint64_t *size)
{
	bool ok = true;
	uint64_t sum = 0, want;

	for (int i = 0; i < 512; i++)
		sum += (i >= 148 && i < 156) ? ' ' : b[i];
	want = tar_octal(b + 148, 8, &ok);
	if (!ok || want != sum)
		return false;
	if (memcmp(b + 257, "ustar", 5))
		return false;
	if (b[156] != '0' && b[156] != 0)
		return false;             /* regular files only */
	if (b[345])
		return false;             /* no prefix (member names are short) */
	memcpy(name, b, 100);
	name[100] = 0;
	if (memchr(b, 0, 100) == NULL)
		return false;
	*size = tar_octal(b + 124, 12, &ok);
	return ok;
}

static uint64_t pad512(uint64_t n)
{
	return (n + 511) & ~(uint64_t)511;
}

int rsu_header_parse(const uint8_t *buf, size_t len, struct rsu_header *h, char *err, size_t errlen)
{
	char name[101];
	uint64_t size, off = 0;

	memset(h, 0, sizeof(*h));
	if (len < 512 || !tar_block_ok(buf, name, &size) || strcmp(name, "manifest")) {
		seterr(err, errlen, "not an update package (no manifest)");
		return -1;
	}
	if (size == 0 || size > RSU_MANIFEST_MAX || 512 + size > len) {
		seterr(err, errlen, "manifest size %" PRIu64 " out of range", size);
		return -1;
	}
	memcpy(h->manifest_text, buf + 512, (size_t)size);
	h->manifest_text[size] = 0;
	h->manifest_len = (size_t)size;
	if (rsu_manifest_parse(h->manifest_text, h->manifest_len, &h->m, err, errlen) < 0)
		return -1;
	off = 512 + pad512(size);
	if (off + 512 > len || !tar_block_ok(buf + off, name, &size)) {
		seterr(err, errlen, "damaged package (member 2)");
		return -1;
	}
	if (!strcmp(name, "manifest.sig")) {
		if (size == 0 || size > RSU_SIG_MAX || off + 512 + size > len ||
		    rsu_sig_parse((const char *)buf + off + 512, (size_t)size, &h->sig) < 0) {
			seterr(err, errlen, "damaged signature");
			return -1;
		}
		h->has_sig = true;
		off += 512 + pad512(size);
		if (off + 512 > len || !tar_block_ok(buf + off, name, &size)) {
			seterr(err, errlen, "damaged package (member 3)");
			return -1;
		}
	}
	if (strcmp(name, h->m.payload) || size != h->m.payload_size) {
		seterr(err, errlen, "the payload member (%s, %" PRIu64 " bytes) does not match the manifest", name, size);
		return -1;
	}
	h->payload_offset = off + 512;
	return 0;
}

void rsu_tar_header(uint8_t blk[512], const char *name, uint64_t size, int64_t mtime)
{
	unsigned sum = 0;

	memset(blk, 0, 512);
	snprintf((char *)blk, 100, "%s", name);
	snprintf((char *)blk + 100, 8, "%07o", 0644);
	snprintf((char *)blk + 108, 8, "%07o", 0);
	snprintf((char *)blk + 116, 8, "%07o", 0);
	/* 11 octal digits: 8 GiB - 1 at most (a manifest never allows more) */
	snprintf((char *)blk + 124, 12, "%011" PRIo64, (uint64_t)(size & 077777777777ull));
	snprintf((char *)blk + 136, 12, "%011" PRIo64, (uint64_t)((uint64_t)(mtime > 0 ? mtime : 0) & 077777777777ull));
	memset(blk + 148, ' ', 8);
	blk[156] = '0';
	memcpy(blk + 257, "ustar", 6);      /* POSIX: "ustar\0" "00" */
	memcpy(blk + 263, "00", 2);
	snprintf((char *)blk + 265, 32, "root");
	snprintf((char *)blk + 297, 32, "root");
	for (int i = 0; i < 512; i++)
		sum += blk[i];
	snprintf((char *)blk + 148, 8, "%06o", sum);
	blk[155] = ' ';
}

/* -------------------------------------------------------------- versions */
struct ver {
	bool numeric;
	uint64_t n[8];
	int nn;
	const char *suffix;           /* after '-', "" if none */
};

static void ver_parse(const char *s, struct ver *v)
{
	memset(v, 0, sizeof(*v));
	v->suffix = "";
	if (*s == 'v' || *s == 'V')
		s++;
	if (!isdigit((unsigned char)*s))
		return;
	v->numeric = true;
	while (isdigit((unsigned char)*s)) {
		uint64_t x = 0;

		while (isdigit((unsigned char)*s)) {
			if (x < UINT64_MAX / 16)
				x = x * 10 + (uint64_t)(*s - '0');
			s++;
		}
		if (v->nn < 8)
			v->n[v->nn++] = x;
		if (*s == '.' && isdigit((unsigned char)s[1]))
			s++;
		else
			break;
	}
	if (*s == '-')
		v->suffix = s + 1;
}

/* natural order: digit runs compare as numbers; '+' (build metadata) ends it */
static int natcmp(const char *a, const char *b)
{
	while (*a && *a != '+' && *b && *b != '+') {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			uint64_t x = 0, y = 0;

			while (isdigit((unsigned char)*a))
				x = x < UINT64_MAX / 16 ? x * 10 + (uint64_t)(*a++ - '0') : (a++, x);
			while (isdigit((unsigned char)*b))
				y = y < UINT64_MAX / 16 ? y * 10 + (uint64_t)(*b++ - '0') : (b++, y);
			if (x != y)
				return x < y ? -1 : 1;
			continue;
		}
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return tolower((unsigned char)*a) < tolower((unsigned char)*b) ? -1 : 1;
		a++, b++;
	}
	{
		bool ea = !*a || *a == '+', eb = !*b || *b == '+';

		return ea == eb ? 0 : ea ? -1 : 1;
	}
}

int rsu_version_cmp(const char *a, int64_t ta, const char *b, int64_t tb)
{
	struct ver x, y;

	ver_parse(a, &x);
	ver_parse(b, &y);
	if (x.numeric && y.numeric) {
		int n = x.nn > y.nn ? x.nn : y.nn;
		bool sa, sb;

		for (int i = 0; i < n; i++) {
			uint64_t p = i < x.nn ? x.n[i] : 0, q = i < y.nn ? y.n[i] : 0;

			if (p != q)
				return p < q ? -1 : 1;
		}
		sa = x.suffix[0] && x.suffix[0] != '+';
		sb = y.suffix[0] && y.suffix[0] != '+';
		if (sa != sb)
			return sa ? -1 : 1;           /* a pre-release comes before its release */
		if (sa) {
			int c = natcmp(x.suffix, y.suffix);

			if (c)
				return c;
		}
	}
	return ta < tb ? -1 : ta > tb ? 1 : 0;
}

/* ---------------------------------------------------------------- policy */
enum rsu_err rsu_policy(const struct rsu_system *sys, const struct rsu_header *h, unsigned flags)
{
	int c;

	if (!h->has_sig) {
		if (!(flags & RSU_ALLOW_UNSIGNED) || sys->release)
			return RSU_E_UNSIGNED;
	} else {
		if (!sys->have_key)
			return RSU_E_NOKEY;
		if (!rsu_verify(&sys->key, &h->sig, h->manifest_text, h->manifest_len))
			return RSU_E_BADSIG;
	}
	/* from here on the manifest is trusted */
	if (h->m.format > RSU_FORMAT || h->m.min_updater > RSU_UPDATER_VERSION)
		return RSU_E_UPDATER;
	if (strcmp(h->m.board, sys->board))
		return RSU_E_BOARD;
	/* a release console never takes a development build (signed by the
	 * same key in CI) unless asked on the UART */
	if (sys->release && strcmp(h->m.variant, "release") && !(flags & RSU_ALLOW_DEV))
		return RSU_E_VARIANT;
	if (h->m.bootloader_min > (sys->bootloader > 0 ? sys->bootloader : 1))
		return RSU_E_BOOTLOADER;
	c = rsu_version_cmp(h->m.version, h->m.build_time, sys->version, sys->build_time);
	if (c < 0 && !(flags & RSU_FORCE))
		return RSU_E_OLDER;
	if (c == 0 && !(flags & RSU_FORCE))
		return RSU_E_SAME;
	return RSU_OK;
}
