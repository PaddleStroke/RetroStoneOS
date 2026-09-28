/*
 * sha256.c - see sha256.h.
 */
#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, hh;

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
		       (uint32_t)p[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void sha256_init(struct sha256 *c)
{
	static const uint32_t iv[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};

	memcpy(c->h, iv, sizeof(iv));
	c->len = 0;
	c->fill = 0;
}

void sha256_update(struct sha256 *c, const void *data, size_t n)
{
	const uint8_t *p = data;

	c->len += n;
	if (c->fill) {
		size_t k = 64 - c->fill < n ? 64 - c->fill : n;

		memcpy(c->buf + c->fill, p, k);
		c->fill += k;
		p += k;
		n -= k;
		if (c->fill < 64)
			return;
		block(c->h, c->buf);
		c->fill = 0;
	}
	for (; n >= 64; p += 64, n -= 64)
		block(c->h, p);
	if (n) {
		memcpy(c->buf, p, n);
		c->fill = n;
	}
}

void sha256_final(struct sha256 *c, uint8_t out[32])
{
	uint64_t bits = c->len * 8;
	uint8_t pad[72];
	size_t np = (c->fill < 56 ? 56 : 120) - c->fill;

	memset(pad, 0, sizeof(pad));
	pad[0] = 0x80;
	for (int i = 0; i < 8; i++)
		pad[np + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_update(c, pad, np + 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

void sha256(const void *data, size_t n, uint8_t out[32])
{
	struct sha256 c;

	sha256_init(&c);
	sha256_update(&c, data, n);
	sha256_final(&c, out);
}

void sha256_hex(const uint8_t d[32], char out[65])
{
	static const char hx[] = "0123456789abcdef";

	for (int i = 0; i < 32; i++) {
		out[2 * i] = hx[d[i] >> 4];
		out[2 * i + 1] = hx[d[i] & 15];
	}
	out[64] = 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int sha256_parse_hex(const char *s, uint8_t d[32])
{
	for (int i = 0; i < 32; i++) {
		int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);

		if (a < 0 || b < 0)
			return -1;
		d[i] = (uint8_t)(a << 4 | b);
	}
	return s[64] ? -1 : 0;
}
