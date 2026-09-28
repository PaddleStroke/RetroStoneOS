/*
 * rsos-mkupdate - build machine tool for RetroStoneOS update packages
 * (.rsu, docs/updates.md). The same code as the device (src/update/rsu.c,
 * Monocypher), no other dependency; the payload is compressed beforehand
 * (zstd -19, scripts/ci/make-rsu.sh).
 *
 *   rsos-mkupdate keygen -s SECKEY -p PUBKEY [-c COMMENT]
 *   rsos-mkupdate pubkey (-s SECKEY | --key-env VAR)       print the public key
 *   rsos-mkupdate pack -o OUT.rsu --image rootfs.ext4 --payload rootfs.ext4.zst
 *                 --board ID --version V --build-time EPOCH [--build-date D]
 *                 [--variant release|dev] [--changelog-file F] [--bootloader-min N]
 *                 [--min-updater N] [--compression zstd|none] [-s SECKEY | --key-env VAR]
 *   rsos-mkupdate sign (-s SECKEY | --key-env VAR) -o OUT.rsu IN.rsu
 *   rsos-mkupdate verify -p PUBKEY FILE.rsu
 *   rsos-mkupdate info FILE.rsu
 *
 * Keys are signify keys without a passphrase (a CI secret cannot type
 * one): `signify -V -p update.pub -x manifest.sig -m manifest` checks a
 * package's manifest after `tar xf package.rsu manifest manifest.sig`.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "monocypher.h"
#include "update/rsu.h"
#include "update/sha256.h"

static int die(const char *fmt, const char *a)
{
	fprintf(stderr, "rsos-mkupdate: ");
	fprintf(stderr, fmt, a ? a : "");
	fprintf(stderr, "\n");
	return 1;
}

static int random_bytes(uint8_t *p, size_t n)
{
	while (n) {
		ssize_t r = getrandom(p, n, 0);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static int write_new(const char *path, const char *text, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
	size_t n = strlen(text);

	if (fd < 0)
		return -1;
	if (write(fd, text, n) != (ssize_t)n || fsync(fd) < 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	return close(fd);
}

static int load_seckey(const char *file, const char *env, struct rsu_seckey *k)
{
	if (env) {
		const char *t = getenv(env);

		if (!t || !*t)
			return die("the environment variable %s is empty", env), -1;
		if (rsu_seckey_parse(t, strlen(t), k) < 0)
			return die("%s does not hold an unencrypted signify secret key", env), -1;
		return 0;
	}
	if (rsu_seckey_load(file, k) < 0)
		return die("%s: not an unencrypted signify secret key", file), -1;
	return 0;
}

/* sha256 and size of a whole file */
static int hash_file(const char *path, uint8_t d[32], uint64_t *size)
{
	static uint8_t buf[1 << 20];
	struct sha256 s;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	*size = 0;
	sha256_init(&s);
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		sha256_update(&s, buf, (size_t)r);
		*size += (uint64_t)r;
	}
	close(fd);
	if (r < 0)
		return -1;
	sha256_final(&s, d);
	return 0;
}

static int put(FILE *f, const void *p, size_t n)
{
	return fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int put_pad(FILE *f, uint64_t n)
{
	static const uint8_t z[512];
	size_t pad = (size_t)((512 - n % 512) % 512);

	return pad ? put(f, z, pad) : 0;
}

/* manifest [+ signature] + payload copied from an open file */
static int write_package(const char *out, const char *manifest, size_t mlen, const char *sigtext,
			 const char *payload_name, int pfd, uint64_t poff, uint64_t psize, int64_t mtime)
{
	static uint8_t buf[1 << 20];
	char tmp[4096];
	uint8_t blk[512];
	uint64_t left = psize;
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", out);
	f = fopen(tmp, "wbe");
	if (!f)
		return -1;
	rsu_tar_header(blk, "manifest", mlen, mtime);
	if (put(f, blk, 512) || put(f, manifest, mlen) || put_pad(f, mlen))
		goto err;
	if (sigtext) {
		size_t sl = strlen(sigtext);

		rsu_tar_header(blk, "manifest.sig", sl, mtime);
		if (put(f, blk, 512) || put(f, sigtext, sl) || put_pad(f, sl))
			goto err;
	}
	rsu_tar_header(blk, payload_name, psize, mtime);
	if (put(f, blk, 512))
		goto err;
	while (left) {
		size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
		ssize_t r = pread(pfd, buf, want, (off_t)(poff + psize - left));

		if (r <= 0 || put(f, buf, (size_t)r))
			goto err;
		left -= (uint64_t)r;
	}
	memset(buf, 0, 1024);
	if (put_pad(f, psize) || put(f, buf, 1024) || fflush(f) || fsync(fileno(f)))
		goto err;
	if (fclose(f)) {
		unlink(tmp);
		return -1;
	}
	return rename(tmp, out);
err:
	fclose(f);
	unlink(tmp);
	return -1;
}

static void sign_text(const struct rsu_seckey *k, const char *manifest, size_t mlen, char *out, size_t n)
{
	struct rsu_sig s;

	rsu_sign(k, manifest, mlen, &s);
	rsu_sig_format(&s, "verify with update.pub (RetroStoneOS update key)", out, n);
}

static int read_head(const char *path, struct rsu_header *h, int *fd_out)
{
	static uint8_t buf[RSU_HEAD_MAX];
	char err[256];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return die("cannot open %s", path);
	n = pread(fd, buf, sizeof(buf), 0);
	if (n < 0 || rsu_header_parse(buf, (size_t)n, h, err, sizeof(err)) < 0) {
		close(fd);
		fprintf(stderr, "rsos-mkupdate: %s: %s\n", path, n < 0 ? strerror(errno) : err);
		return 1;
	}
	*fd_out = fd;
	return 0;
}

/* ------------------------------------------------------------ commands */
static int cmd_keygen(const char *sk_path, const char *pk_path, const char *comment)
{
	uint8_t seed[32], keynum[8];
	struct rsu_seckey sk;
	struct rsu_pubkey pk;
	char text[512], c1[256], c2[256];

	if (!sk_path || !pk_path)
		return die("keygen needs -s SECKEY and -p PUBKEY%s", "");
	if (random_bytes(seed, 32) < 0 || random_bytes(keynum, 8) < 0)
		return die("no random numbers%s", "");
	rsu_keypair(seed, keynum, &sk, &pk);
	crypto_wipe(seed, sizeof(seed));
	snprintf(c1, sizeof(c1), "%s secret key", comment ? comment : "RetroStoneOS update");
	snprintf(c2, sizeof(c2), "%s public key", comment ? comment : "RetroStoneOS update");
	if (rsu_seckey_format(&sk, c1, text, sizeof(text)) < 0 || write_new(sk_path, text, 0600) < 0) {
		crypto_wipe(&sk, sizeof(sk));
		crypto_wipe(text, sizeof(text));
		return die("cannot write %s (it must not exist yet)", sk_path);
	}
	crypto_wipe(&sk, sizeof(sk));
	crypto_wipe(text, sizeof(text));
	if (rsu_pubkey_format(&pk, c2, text, sizeof(text)) < 0 || write_new(pk_path, text, 0644) < 0)
		return die("cannot write %s (it must not exist yet)", pk_path);
	printf("secret key: %s (keep it private, back it up)\npublic key: %s\n", sk_path, pk_path);
	return 0;
}

static int cmd_pubkey(const char *sk_path, const char *env)
{
	struct rsu_seckey sk;
	struct rsu_pubkey pk;
	char text[512];

	if (load_seckey(sk_path, env, &sk) < 0)
		return 1;
	rsu_seckey_public(&sk, &pk);
	crypto_wipe(&sk, sizeof(sk));
	if (rsu_pubkey_format(&pk, "RetroStoneOS update public key", text, sizeof(text)) < 0)
		return 1;
	fputs(text, stdout);
	return 0;
}

struct pack_opts {
	const char *out, *image, *payload, *board, *version, *variant, *date, *changelog, *comp;
	const char *sk, *key_env;
	int64_t build_time;
	int bootloader_min, min_updater;
};

static int cmd_pack(const struct pack_opts *o)
{
	static struct rsu_manifest m;
	static char manifest[RSU_MANIFEST_MAX + 1], sig[1024], err[256];
	struct rsu_manifest check;
	struct rsu_seckey sk;
	uint64_t isize, psize;
	int pfd, r, mlen;

	if (!o->out || !o->image || !o->payload || !o->board || !o->version || o->build_time <= 0)
		return die("pack needs -o, --image, --payload, --board, --version and --build-time%s", "");
	memset(&m, 0, sizeof(m));
	m.format = RSU_FORMAT;
	snprintf(m.board, sizeof(m.board), "%s", o->board);
	snprintf(m.version, sizeof(m.version), "%s", o->version);
	snprintf(m.variant, sizeof(m.variant), "%s", o->variant ? o->variant : "release");
	m.build_time = o->build_time;
	if (o->date) {
		snprintf(m.build_date, sizeof(m.build_date), "%s", o->date);
	} else {
		time_t t = (time_t)o->build_time;
		struct tm tm;

		gmtime_r(&t, &tm);
		strftime(m.build_date, sizeof(m.build_date), "%Y-%m-%d", &tm);
	}
	m.min_updater = o->min_updater > 0 ? o->min_updater : 1;
	m.bootloader_min = o->bootloader_min > 0 ? o->bootloader_min : 1;
	snprintf(m.compression, sizeof(m.compression), "%s", o->comp ? o->comp : "zstd");
	snprintf(m.payload, sizeof(m.payload), "%s", !strcmp(m.compression, "none") ? "rootfs.ext4" : "rootfs.ext4.zst");
	if (o->changelog) {
		FILE *f = fopen(o->changelog, "re");
		size_t n;

		if (!f)
			return die("cannot read %s", o->changelog);
		n = fread(m.changelog, 1, sizeof(m.changelog) - 1, f);
		m.changelog[n] = 0;
		fclose(f);
		while (n && (m.changelog[n - 1] == '\n' || m.changelog[n - 1] == ' '))
			m.changelog[--n] = 0;
	}
	if (hash_file(o->image, m.image_sha256, &isize) < 0)
		return die("cannot read %s", o->image);
	if (hash_file(o->payload, m.payload_sha256, &psize) < 0)
		return die("cannot read %s", o->payload);
	m.image_size = isize;
	m.payload_size = psize;
	mlen = rsu_manifest_format(&m, manifest, sizeof(manifest));
	if (mlen < 0)
		return die("the manifest is too large (changelog?)%s", "");
	/* what we write must be what the device reads */
	if (rsu_manifest_parse(manifest, (size_t)mlen, &check, err, sizeof(err)) < 0)
		return die("%s", err);
	if (o->sk || o->key_env) {
		if (load_seckey(o->sk, o->key_env, &sk) < 0)
			return 1;
		sign_text(&sk, manifest, (size_t)mlen, sig, sizeof(sig));
		crypto_wipe(&sk, sizeof(sk));
	}
	pfd = open(o->payload, O_RDONLY | O_CLOEXEC);
	if (pfd < 0)
		return die("cannot open %s", o->payload);
	r = write_package(o->out, manifest, (size_t)mlen, (o->sk || o->key_env) ? sig : NULL, m.payload, pfd, 0, psize,
			  o->build_time);
	close(pfd);
	if (r < 0)
		return die("cannot write %s", o->out);
	printf("%s: RetroStoneOS %s for %s, %s, payload %" PRIu64 " bytes, image %" PRIu64 " bytes\n", o->out,
	       m.version, m.board, (o->sk || o->key_env) ? "signed" : "NOT SIGNED", psize, isize);
	return 0;
}

static int cmd_sign(const char *sk_path, const char *env, const char *out, const char *in)
{
	static struct rsu_header h;
	static char sig[1024];
	struct rsu_seckey sk;
	int fd, r;

	if (!out || !in)
		return die("sign needs -o OUT and a package%s", "");
	if (read_head(in, &h, &fd))
		return 1;
	if (load_seckey(sk_path, env, &sk) < 0) {
		close(fd);
		return 1;
	}
	sign_text(&sk, h.manifest_text, h.manifest_len, sig, sizeof(sig));
	crypto_wipe(&sk, sizeof(sk));
	r = write_package(out, h.manifest_text, h.manifest_len, sig, h.m.payload, fd, h.payload_offset,
			  h.m.payload_size, h.m.build_time);
	close(fd);
	if (r < 0)
		return die("cannot write %s", out);
	printf("%s: signed\n", out);
	return 0;
}

static int cmd_verify(const char *pk_path, const char *path)
{
	static struct rsu_header h;
	static uint8_t buf[1 << 20];
	struct rsu_pubkey pk;
	struct sha256 s;
	uint64_t left;
	uint8_t d[32];
	int fd;

	if (!pk_path || rsu_pubkey_load(pk_path, &pk) < 0)
		return die("verify needs -p PUBKEY (a signify public key)%s", "");
	if (read_head(path, &h, &fd))
		return 1;
	if (!h.has_sig) {
		close(fd);
		return die("%s is not signed", path);
	}
	if (!rsu_verify(&pk, &h.sig, h.manifest_text, h.manifest_len)) {
		close(fd);
		return die("%s: BAD SIGNATURE (another key, or a modified manifest)", path);
	}
	sha256_init(&s);
	for (left = h.m.payload_size; left;) {
		size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
		ssize_t r = pread(fd, buf, want, (off_t)(h.payload_offset + h.m.payload_size - left));

		if (r <= 0) {
			close(fd);
			return die("%s: truncated", path);
		}
		sha256_update(&s, buf, (size_t)r);
		left -= (uint64_t)r;
	}
	close(fd);
	sha256_final(&s, d);
	if (memcmp(d, h.m.payload_sha256, 32))
		return die("%s: the payload does not match its manifest", path);
	printf("%s: signature and payload OK (RetroStoneOS %s for %s)\n", path, h.m.version, h.m.board);
	return 0;
}

static int cmd_info(const char *path)
{
	static struct rsu_header h;
	int fd;

	if (read_head(path, &h, &fd))
		return 1;
	close(fd);
	fwrite(h.manifest_text, 1, h.manifest_len, stdout);
	printf("# %s, payload at byte %" PRIu64 "\n", h.has_sig ? "signed" : "NOT SIGNED", h.payload_offset);
	return 0;
}

int main(int argc, char **argv)
{
	struct pack_opts po = { 0 };
	const char *cmd = argc > 1 ? argv[1] : NULL, *pk = NULL, *comment = NULL, *pos = NULL;

	for (int i = 2; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

#define OPT(name, dst) else if (v && !strcmp(a, name)) { dst = v; i++; }
		if (0) {
		}
		OPT("-o", po.out)
		OPT("-s", po.sk)
		OPT("-p", pk)
		OPT("-c", comment)
		OPT("--key-env", po.key_env)
		OPT("--image", po.image)
		OPT("--payload", po.payload)
		OPT("--board", po.board)
		OPT("--version", po.version)
		OPT("--variant", po.variant)
		OPT("--build-date", po.date)
		OPT("--changelog-file", po.changelog)
		OPT("--compression", po.comp)
		else if (v && !strcmp(a, "--build-time")) {
			po.build_time = strtoll(v, NULL, 10);
			i++;
		} else if (v && !strcmp(a, "--bootloader-min")) {
			po.bootloader_min = atoi(v);
			i++;
		} else if (v && !strcmp(a, "--min-updater")) {
			po.min_updater = atoi(v);
			i++;
		} else if (a[0] != '-' && !pos) {
			pos = a;
		} else {
			return die("unexpected argument %s (see the comment at the top of rsos-mkupdate.c)", a);
		}
#undef OPT
	}
	if (!cmd)
		return die("usage: rsos-mkupdate keygen|pubkey|pack|sign|verify|info ...%s", "");
	if (!strcmp(cmd, "keygen"))
		return cmd_keygen(po.sk, pk, comment);
	if (!strcmp(cmd, "pubkey"))
		return cmd_pubkey(po.sk, po.key_env);
	if (!strcmp(cmd, "pack"))
		return cmd_pack(&po);
	if (!strcmp(cmd, "sign"))
		return cmd_sign(po.sk, po.key_env, po.out, pos);
	if (!strcmp(cmd, "verify") && pos)
		return cmd_verify(pk, pos);
	if (!strcmp(cmd, "info") && pos)
		return cmd_info(pos);
	return die("unknown command %s", cmd);
}
