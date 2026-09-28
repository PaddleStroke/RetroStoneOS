/*
 * sha256.h - SHA-256 (FIPS 180-4), for the update packages (docs/updates.md):
 * the payload and image hashes of a .rsu manifest. Plain C, no dependency;
 * checked against the NIST vectors and sha256sum in tests/test_update.c.
 */
#ifndef RSOS_UPDATE_SHA256_H
#define RSOS_UPDATE_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct sha256 {
	uint32_t h[8];
	uint64_t len;          /* bytes hashed so far */
	uint8_t buf[64];
	size_t fill;
};

void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t n);
void sha256_final(struct sha256 *c, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);

/* 64 lowercase hex digits + NUL */
void sha256_hex(const uint8_t d[32], char out[65]);
/* 0, or -1 if s is not exactly 64 hex digits */
int sha256_parse_hex(const char *s, uint8_t d[32]);

#endif
