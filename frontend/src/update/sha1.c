/* sha1.c - see sha1.h */
#include "sha1.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(struct sha1 *c, const uint8_t *p)
{
	uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], t;

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;

		if (i < 20) {
			f = (b & cc) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ cc ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & cc) | (b & d) | (cc & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ cc ^ d;
			k = 0xca62c1d6;
		}
		t = ROL(a, 5) + f + e + k + w[i];
		e = d;
		d = cc;
		cc = ROL(b, 30);
		b = a;
		a = t;
	}
	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
	c->h[4] += e;
}

void sha1_init(struct sha1 *c)
{
	c->h[0] = 0x67452301;
	c->h[1] = 0xefcdab89;
	c->h[2] = 0x98badcfe;
	c->h[3] = 0x10325476;
	c->h[4] = 0xc3d2e1f0;
	c->len = 0;
	c->n = 0;
}

void sha1_update(struct sha1 *c, const void *data, size_t n)
{
	const uint8_t *p = data;

	c->len += n;
	while (n) {
		size_t k = 64 - c->n;

		if (k > n)
			k = n;
		memcpy(c->buf + c->n, p, k);
		c->n += k;
		p += k;
		n -= k;
		if (c->n == 64) {
			block(c, c->buf);
			c->n = 0;
		}
	}
}

void sha1_final(struct sha1 *c, uint8_t out[20])
{
	uint64_t bits = c->len * 8;
	uint8_t pad = 0x80, zero = 0, lenb[8];

	sha1_update(c, &pad, 1);
	while (c->n != 56)
		sha1_update(c, &zero, 1);
	for (int i = 0; i < 8; i++)
		lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha1_update(c, lenb, 8);
	for (int i = 0; i < 5; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

void sha1_hex(const uint8_t d[20], char out[41])
{
	static const char hx[] = "0123456789abcdef";

	for (int i = 0; i < 20; i++) {
		out[2 * i] = hx[d[i] >> 4];
		out[2 * i + 1] = hx[d[i] & 15];
	}
	out[40] = 0;
}
