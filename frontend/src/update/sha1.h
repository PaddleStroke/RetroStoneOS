/*
 * sha1.h - SHA-1 (RFC 3174), only to recognise game dumps by the checksum the community publishes for
 * them (the game store's ROM finding). Never used for security: the catalog and the downloads are
 * checked with SHA-256 and signatures.
 */
#ifndef RSOS_UPDATE_SHA1_H
#define RSOS_UPDATE_SHA1_H

#include <stddef.h>
#include <stdint.h>

struct sha1 {
	uint32_t h[5];
	uint64_t len;
	uint8_t buf[64];
	size_t n;
};

void sha1_init(struct sha1 *c);
void sha1_update(struct sha1 *c, const void *data, size_t n);
void sha1_final(struct sha1 *c, uint8_t out[20]);
/* 40 lower-case hex characters and a NUL */
void sha1_hex(const uint8_t d[20], char out[41]);

#endif
