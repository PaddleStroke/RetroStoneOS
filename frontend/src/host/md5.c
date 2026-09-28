/*
 * md5.c - straightforward RFC 1321 implementation (written for
 * RetroStoneOS from the RFC's description).
 */
#include "md5.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint32_t K[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const unsigned char S[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t rol(uint32_t x, unsigned n)
{
	return (x << n) | (x >> (32 - n));
}

static void block(struct md5_ctx *c, const unsigned char *p)
{
	uint32_t m[16], a = c->a, b = c->b, cc = c->c, d = c->d;

	for (int i = 0; i < 16; i++)
		m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 |
		       (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
	for (int i = 0; i < 64; i++) {
		uint32_t f;
		int g;

		if (i < 16) {
			f = (b & cc) | (~b & d);
			g = i;
		} else if (i < 32) {
			f = (d & b) | (~d & cc);
			g = (5 * i + 1) & 15;
		} else if (i < 48) {
			f = b ^ cc ^ d;
			g = (3 * i + 5) & 15;
		} else {
			f = cc ^ (b | ~d);
			g = (7 * i) & 15;
		}
		f = f + a + K[i] + m[g];
		a = d;
		d = cc;
		cc = b;
		b = b + rol(f, S[i]);
	}
	c->a += a;
	c->b += b;
	c->c += cc;
	c->d += d;
}

void md5_init(struct md5_ctx *c)
{
	c->a = 0x67452301;
	c->b = 0xefcdab89;
	c->c = 0x98badcfe;
	c->d = 0x10325476;
	c->len = 0;
}

void md5_update(struct md5_ctx *c, const void *data, size_t n)
{
	const unsigned char *p = data;
	size_t have = (size_t)(c->len & 63);

	c->len += n;
	if (have) {
		size_t take = 64 - have < n ? 64 - have : n;

		memcpy(c->buf + have, p, take);
		p += take;
		n -= take;
		if (have + take < 64)
			return;
		block(c, c->buf);
	}
	for (; n >= 64; p += 64, n -= 64)
		block(c, p);
	memcpy(c->buf, p, n);
}

void md5_final(struct md5_ctx *c, unsigned char out[16])
{
	uint64_t bits = c->len * 8;
	unsigned char pad[72] = { 0x80 };
	size_t have = (size_t)(c->len & 63);
	size_t padlen = have < 56 ? 56 - have : 120 - have;
	unsigned char lenb[8];

	for (int i = 0; i < 8; i++)
		lenb[i] = (unsigned char)(bits >> (8 * i));
	md5_update(c, pad, padlen);
	md5_update(c, lenb, 8);
	for (int i = 0; i < 4; i++) {
		uint32_t v = i == 0 ? c->a : i == 1 ? c->b : i == 2 ? c->c : c->d;

		for (int j = 0; j < 4; j++)
			out[i * 4 + j] = (unsigned char)(v >> (8 * j));
	}
}

int md5_file(const char *path, char hex[33])
{
	unsigned char buf[16384], dig[16];
	struct md5_ctx c;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	md5_init(&c);
	for (;;) {
		ssize_t r = read(fd, buf, sizeof(buf));

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			int e = errno;

			close(fd);
			return -e;
		}
		if (!r)
			break;
		md5_update(&c, buf, (size_t)r);
	}
	close(fd);
	md5_final(&c, dig);
	for (int i = 0; i < 16; i++)
		snprintf(hex + i * 2, 3, "%02x", dig[i]);
	return 0;
}
