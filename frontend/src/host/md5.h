/*
 * md5.h - MD5 (RFC 1321), for the BIOS checksums in the core .ini files.
 */
#ifndef RSOS_HOST_MD5_H
#define RSOS_HOST_MD5_H

#include <stddef.h>
#include <stdint.h>

struct md5_ctx {
	uint32_t a, b, c, d;
	uint64_t len;
	unsigned char buf[64];
};

void md5_init(struct md5_ctx *c);
void md5_update(struct md5_ctx *c, const void *data, size_t n);
void md5_final(struct md5_ctx *c, unsigned char out[16]);

/* Lowercase hex digest of a file into hex[33]. Returns 0 or -errno. */
int md5_file(const char *path, char hex[33]);

#endif
