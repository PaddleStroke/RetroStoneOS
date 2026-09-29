/*
 * rsu.h - the RetroStoneOS update package (.rsu, docs/updates.md): its
 * manifest, the signify-compatible Ed25519 keys and signatures, the tar
 * container, version ordering and the install policy. No I/O except the
 * key loaders; no dependency beyond the vendored Monocypher and sha256.c.
 *
 * A package is a plain ustar archive (any tar tool lists it) with, in this
 * order:
 *   manifest          "key = value" text (below)
 *   manifest.sig      signify signature of the manifest (absent: unsigned)
 *   rootfs.ext4.zst   the payload: the new root filesystem image, compressed
 * The signature covers the manifest, which holds the payload's and the
 * image's size and SHA-256: one signature check protects everything.
 * `signify -V -p update.pub -x manifest.sig -m manifest` verifies it too.
 */
#ifndef RSOS_UPDATE_RSU_H
#define RSOS_UPDATE_RSU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSU_FORMAT 1              /* the manifest format this code reads */
#define RSU_UPDATER_VERSION 1     /* compared with a manifest's min_updater */
#define RSU_MANIFEST_MAX 32768
#define RSU_SIG_MAX 1024
/* Everything before the payload fits in the first RSU_HEAD_MAX bytes. */
#define RSU_HEAD_MAX 65536
#define RSU_CHANGELOG_MAX 8192

/* Result codes shared by the whole updater (update.h adds none: one list). */
enum rsu_err {
	RSU_OK = 0,
	RSU_E_FORMAT,       /* not an update package, or a damaged one */
	RSU_E_UPDATER,      /* needs a newer updater (format / min_updater) */
	RSU_E_UNSIGNED,     /* no signature, and unsigned packages are refused */
	RSU_E_BADSIG,       /* signed by another key, or modified */
	RSU_E_NOKEY,        /* this system has no update key */
	RSU_E_BOARD,        /* made for another board */
	RSU_E_BOOTLOADER,   /* needs a newer bootloader: reflash */
	RSU_E_SAME,         /* this version is installed */
	RSU_E_OLDER,        /* older than the installed version */
	RSU_E_PAYLOAD,      /* payload hash or size mismatch */
	RSU_E_IMAGE,        /* the written image does not match */
	RSU_E_IO,           /* read/write error */
	RSU_E_SPACE,        /* not enough free space */
	RSU_E_NOAB,         /* this board cannot be updated in place */
	RSU_E_SLOT,         /* the A/B slots cannot be determined */
	RSU_E_UNCONFIRMED,  /* the running system is still on trial */
	RSU_E_RESTART,      /* an update is installed: restart first */
	RSU_E_ENV,          /* the boot environment could not be written */
	RSU_E_BATTERY,      /* battery too low without a charger */
	RSU_E_BUSY,         /* another update runs */
	RSU_E_CLOCK,        /* the clock is not set: TLS cannot work */
	RSU_E_NETWORK,      /* no network, DNS or connection failure */
	RSU_E_TLS,          /* TLS handshake / certificate failure */
	RSU_E_HTTP,         /* unexpected HTTP answer */
	RSU_E_NOTFOUND,     /* no release / file */
	RSU_E_CANCELLED,
	RSU_E_NOTLS,        /* built without HTTPS support */
	RSU_E_INTERNAL,
	RSU_E_VARIANT,      /* a development package on a release build */
	RSU_E_COUNT
};
/* Machine code ("badsig") and English text of a result. */
const char *rsu_err_code(enum rsu_err e);
const char *rsu_err_text(enum rsu_err e);

/* ------------------------------------------------------------- manifest */
struct rsu_manifest {
	int format;
	char board[64];
	char version[64];
	char variant[16];             /* release | dev */
	char build_date[16];          /* YYYY-MM-DD */
	int64_t build_time;           /* seconds since the epoch */
	int min_updater;
	int bootloader_min;
	char payload[64];             /* tar member name of the payload */
	char compression[16];         /* zstd | none */
	uint64_t payload_size;
	uint8_t payload_sha256[32];
	uint64_t image_size;
	uint8_t image_sha256[32];
	char changelog[RSU_CHANGELOG_MAX];   /* unescaped, may be empty */
};

/* Strict parser: every required key, no duplicate, sane values. Unknown
 * keys are ignored (a newer format bumps "format"). 0 or -1 (err set). */
int rsu_manifest_parse(const char *text, size_t len, struct rsu_manifest *m, char *err, size_t errlen);
/* Writes the manifest text; returns its length, or -1 if it does not fit. */
int rsu_manifest_format(const struct rsu_manifest *m, char *out, size_t n);
/* A value usable in a file name and a log line: [A-Za-z0-9._+-], 1-63. */
bool rsu_safe_token(const char *s);

/* ------------------------------------------------ keys and signatures */
struct rsu_pubkey {
	uint8_t keynum[8];
	uint8_t pk[32];
};
struct rsu_seckey {
	uint8_t keynum[8];
	uint8_t sk[64];               /* seed + public key (Ed25519) */
};
struct rsu_sig {
	uint8_t keynum[8];
	uint8_t sig[64];
};

/* signify files: "untrusted comment: ...\n<base64>\n". 0 or -1. */
int rsu_pubkey_parse(const char *text, size_t len, struct rsu_pubkey *k);
int rsu_seckey_parse(const char *text, size_t len, struct rsu_seckey *k);   /* unencrypted only */
int rsu_sig_parse(const char *text, size_t len, struct rsu_sig *s);
int rsu_pubkey_load(const char *path, struct rsu_pubkey *k);
int rsu_seckey_load(const char *path, struct rsu_seckey *k);
/* The file texts; return the length or -1. */
int rsu_pubkey_format(const struct rsu_pubkey *k, const char *comment, char *out, size_t n);
int rsu_seckey_format(const struct rsu_seckey *k, const char *comment, char *out, size_t n);
int rsu_sig_format(const struct rsu_sig *s, const char *comment, char *out, size_t n);
/* A new key pair from 32 random bytes (seed, wiped) and 8 (keynum). */
void rsu_keypair(const uint8_t seed[32], const uint8_t keynum[8], struct rsu_seckey *sk, struct rsu_pubkey *pk);
void rsu_seckey_public(const struct rsu_seckey *sk, struct rsu_pubkey *pk);
void rsu_sign(const struct rsu_seckey *k, const void *msg, size_t n, struct rsu_sig *out);
/* True when the key numbers match and the signature is valid. */
bool rsu_verify(const struct rsu_pubkey *k, const struct rsu_sig *s, const void *msg, size_t n);

/* base64 (RFC 4648, with padding). encode: length written (NUL added) or -1;
 * decode: bytes written or -1. */
int rsu_b64_encode(const uint8_t *in, size_t n, char *out, size_t outn);
int rsu_b64_decode(const char *in, size_t n, uint8_t *out, size_t outn);

/* ------------------------------------------------------------ container */
struct rsu_header {
	char manifest_text[RSU_MANIFEST_MAX + 1];
	size_t manifest_len;
	bool has_sig;
	struct rsu_sig sig;
	struct rsu_manifest m;
	uint64_t payload_offset;      /* file offset of the payload's first byte */
};

/* Parses the tar members before the payload from the first bytes of a
 * package (RSU_HEAD_MAX, or the whole file if it is shorter). */
int rsu_header_parse(const uint8_t *buf, size_t len, struct rsu_header *h, char *err, size_t errlen);
/* One ustar header block for a regular file (mode 0644, uid/gid 0). */
void rsu_tar_header(uint8_t blk[512], const char *name, uint64_t size, int64_t mtime);

/* -------------------------------------------------------------- versions */
/*
 * Version order: the numbers ("0.2.10"; a leading "v" is ignored; missing
 * parts are 0), then a release before its pre-releases and development
 * builds ("0.2" > "0.2-rc2" > "0.2-rc1" > "0.2-dev"; suffixes compare
 * naturally), then the build time. A version without numbers (a branch
 * build, "main-1a2b3c4d") is ordered by its build time only.
 * <0, 0, >0 like strcmp.
 */
int rsu_version_cmp(const char *a, int64_t ta, const char *b, int64_t tb);

/* ---------------------------------------------------------------- policy */
struct rsu_system {
	char board[64];
	char version[64];
	int64_t build_time;
	bool release;                 /* release build: unsigned packages never */
	int bootloader;               /* bootloader level (rsos_bootloader, default 1) */
	bool have_key;
	struct rsu_pubkey key;
};

enum {
	RSU_ALLOW_UNSIGNED = 1,       /* development builds, local files, asked */
	RSU_FORCE = 2,                /* same or older version (UART only) */
	RSU_ALLOW_DEV = 4,            /* release build: accept a signed development package (UART only) */
};

/* Signature first, then format, board, variant (a release build takes
 * variant=release packages only, unless RSU_ALLOW_DEV), bootloader and
 * version. */
enum rsu_err rsu_policy(const struct rsu_system *sys, const struct rsu_header *h, unsigned flags);

#endif
