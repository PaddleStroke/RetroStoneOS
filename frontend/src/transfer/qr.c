/*
 * qr.c - a small QR Code encoder (ISO/IEC 18004), enough for a URL:
 * byte mode, error correction level M, versions 1-10 (up to 213 bytes),
 * automatic mask choice. Written for RetroStoneOS following the structure
 * of the well-known public algorithm description (finder/timing/alignment
 * patterns, Reed-Solomon over GF(256) with 0x11d, block interleaving,
 * zig-zag placement, 8 masks with the 4 penalty rules).
 *
 * The UI draws qr->m[y][x] (1 = dark) scaled to whole pixels with a
 * 4-module light margin: version 3 (29 modules) at 4 px = 148 px.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "transfer.h"

/* Level M, index = version */
static const uint8_t ECC_PER_BLOCK[11] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
static const uint8_t NUM_BLOCKS[11] = { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };
#define FORMAT_ECL_M 0      /* the 2 ECC bits of the format word for M */

struct qrb {
	int size;
	uint8_t (*m)[QR_MAX_SIZE];
	uint8_t fn[QR_MAX_SIZE][QR_MAX_SIZE];   /* function module */
};

static int raw_modules(int ver)
{
	int r = (16 * ver + 128) * ver + 64;

	if (ver >= 2) {
		int na = ver / 7 + 2;

		r -= (25 * na - 10) * na - 55;
		if (ver >= 7)
			r -= 36;
	}
	return r;
}

static int data_codewords(int ver)
{
	return raw_modules(ver) / 8 - ECC_PER_BLOCK[ver] * NUM_BLOCKS[ver];
}

/* ------------------------------------------------------------ GF(256) */

static uint8_t gf_mul(uint8_t x, uint8_t y)
{
	int z = 0;

	for (int i = 7; i >= 0; i--) {
		z = (z << 1) ^ ((z >> 7) * 0x11d);
		z ^= ((y >> i) & 1) * x;
	}
	return (uint8_t)z;
}

/* Remainder of data * x^degree divided by the generator polynomial.
 * Exposed (not in the header) for the unit test. */
void qr_rs_remainder(const uint8_t *data, int len, int degree, uint8_t *out);
void qr_rs_remainder(const uint8_t *data, int len, int degree, uint8_t *out)
{
	uint8_t div[30];
	uint8_t root = 1;

	memset(div, 0, sizeof(div));
	div[degree - 1] = 1;
	for (int i = 0; i < degree; i++) {
		for (int j = 0; j < degree; j++) {
			div[j] = gf_mul(div[j], root);
			if (j + 1 < degree)
				div[j] ^= div[j + 1];
		}
		root = gf_mul(root, 0x02);
	}
	memset(out, 0, (size_t)degree);
	for (int i = 0; i < len; i++) {
		uint8_t f = data[i] ^ out[0];

		memmove(out, out + 1, (size_t)degree - 1);
		out[degree - 1] = 0;
		for (int j = 0; j < degree; j++)
			out[j] ^= gf_mul(div[j], f);
	}
}

/* ------------------------------------------------------------ patterns */

static void set_fn(struct qrb *q, int x, int y, bool dark)
{
	q->m[y][x] = dark;
	q->fn[y][x] = 1;
}

/* Format word for level M and a mask (15 bits, BCH + XOR mask). Exposed
 * for the unit test. */
int qr_format_bits(int mask);
int qr_format_bits(int mask)
{
	int data = FORMAT_ECL_M << 3 | mask, rem = data;

	for (int i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	return ((data << 10) | rem) ^ 0x5412;
}

static void draw_format(struct qrb *q, int mask)
{
	int bits = qr_format_bits(mask), s = q->size;

#define BIT(i) (((bits >> (i)) & 1) != 0)
	for (int i = 0; i <= 5; i++)
		set_fn(q, 8, i, BIT(i));
	set_fn(q, 8, 7, BIT(6));
	set_fn(q, 8, 8, BIT(7));
	set_fn(q, 7, 8, BIT(8));
	for (int i = 9; i < 15; i++)
		set_fn(q, 14 - i, 8, BIT(i));
	for (int i = 0; i < 8; i++)
		set_fn(q, s - 1 - i, 8, BIT(i));
	for (int i = 8; i < 15; i++)
		set_fn(q, 8, s - 15 + i, BIT(i));
	set_fn(q, 8, s - 8, true);                  /* the dark module */
#undef BIT
}

static void draw_version(struct qrb *q, int ver)
{
	int rem = ver;
	long bits;

	if (ver < 7)
		return;
	for (int i = 0; i < 12; i++)
		rem = (rem << 1) ^ ((rem >> 11) * 0x1f25);
	bits = (long)ver << 12 | rem;
	for (int i = 0; i < 18; i++) {
		bool b = ((bits >> i) & 1) != 0;
		int a = q->size - 11 + i % 3, c = i / 3;

		set_fn(q, a, c, b);
		set_fn(q, c, a, b);
	}
}

static int align_positions(int ver, int size, int *pos)
{
	int n, step;

	if (ver == 1)
		return 0;
	n = ver / 7 + 2;
	step = (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
	pos[0] = 6;
	for (int i = n - 1, p = size - 7; i >= 1; i--, p -= step)
		pos[i] = p;
	return n;
}

static void draw_function_patterns(struct qrb *q, int ver)
{
	int s = q->size, pos[7], n;
	const int fx[3] = { 3, s - 4, 3 }, fy[3] = { 3, 3, s - 4 };

	for (int i = 0; i < s; i++) {
		set_fn(q, 6, i, i % 2 == 0);
		set_fn(q, i, 6, i % 2 == 0);
	}
	for (int f = 0; f < 3; f++)
		for (int dy = -4; dy <= 4; dy++)
			for (int dx = -4; dx <= 4; dx++) {
				int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
				int x = fx[f] + dx, y = fy[f] + dy;

				if (x >= 0 && x < s && y >= 0 && y < s)
					set_fn(q, x, y, d != 2 && d != 4);
			}
	n = align_positions(ver, s, pos);
	for (int i = 0; i < n; i++)
		for (int j = 0; j < n; j++) {
			if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))
				continue;
			for (int dy = -2; dy <= 2; dy++)
				for (int dx = -2; dx <= 2; dx++) {
					int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);

					set_fn(q, pos[i] + dx, pos[j] + dy, d != 1);
				}
		}
	draw_format(q, 0);                          /* reserve the area */
	draw_version(q, ver);
}

/* ------------------------------------------------------------ data */

static void draw_codewords(struct qrb *q, const uint8_t *data, int len)
{
	int s = q->size, i = 0;

	for (int right = s - 1; right >= 1; right -= 2) {
		if (right == 6)
			right = 5;
		for (int vert = 0; vert < s; vert++)
			for (int j = 0; j < 2; j++) {
				int x = right - j;
				bool up = ((right + 1) & 2) == 0;
				int y = up ? s - 1 - vert : vert;

				if (!q->fn[y][x] && i < len * 8) {
					q->m[y][x] = (data[i >> 3] >> (7 - (i & 7))) & 1;
					i++;
				}
			}
	}
}

static bool mask_bit(int mask, int x, int y)
{
	switch (mask) {
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (x / 3 + y / 2) % 2 == 0;
	case 5: return x * y % 2 + x * y % 3 == 0;
	case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
	default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
	}
}

static void apply_mask(struct qrb *q, int mask)
{
	for (int y = 0; y < q->size; y++)
		for (int x = 0; x < q->size; x++)
			if (!q->fn[y][x] && mask_bit(mask, x, y))
				q->m[y][x] ^= 1;
}

static int penalty(const struct qrb *q)
{
	int s = q->size, p = 0, dark = 0;
	static const uint8_t f1[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 };
	static const uint8_t f2[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };

	for (int pass = 0; pass < 2; pass++)          /* rows, then columns */
		for (int a = 0; a < s; a++) {
			int run = 1;

			for (int b = 1; b <= s; b++) {
				int cur = b < s ? (pass ? q->m[b][a] : q->m[a][b]) : -1;
				int prev = pass ? q->m[b - 1][a] : q->m[a][b - 1];

				if (cur == prev) {
					run++;
				} else {
					if (run >= 5)
						p += 3 + run - 5;
					run = 1;
				}
			}
			for (int b = 0; b + 11 <= s; b++) {
				bool m1 = true, m2 = true;

				for (int k = 0; k < 11; k++) {
					int v = pass ? q->m[b + k][a] : q->m[a][b + k];

					m1 &= v == f1[k];
					m2 &= v == f2[k];
				}
				p += (m1 ? 40 : 0) + (m2 ? 40 : 0);
			}
		}
	for (int y = 0; y + 1 < s; y++)
		for (int x = 0; x + 1 < s; x++) {
			int c = q->m[y][x];

			if (c == q->m[y][x + 1] && c == q->m[y + 1][x] && c == q->m[y + 1][x + 1])
				p += 3;
		}
	for (int y = 0; y < s; y++)
		for (int x = 0; x < s; x++)
			dark += q->m[y][x];
	p += abs(dark * 100 / (s * s) - 50) / 5 * 10;
	return p;
}

int qr_encode(const char *text, struct qr_code *out)
{
	size_t len = strlen(text);
	uint8_t data[400], all[400], ecc[30];
	int ver, cap, bitlen = 0, nb, eccl, raw, nshort, shortlen, k = 0, best = 0, bestp = -1;
	struct qrb q;

	for (ver = 1; ver <= QR_MAX_VERSION; ver++) {
		int ccbits = ver < 10 ? 8 : 16;

		if (4 + ccbits + 8 * (int)len <= data_codewords(ver) * 8)
			break;
	}
	if (ver > QR_MAX_VERSION)
		return -EMSGSIZE;
	cap = data_codewords(ver);

	/* bit stream: mode 0100, count, bytes, terminator, padding */
	memset(data, 0, sizeof(data));
#define PUT(val, n)                                                                   \
	do {                                                                          \
		for (int b_ = (n) - 1; b_ >= 0; b_--, bitlen++)                        \
			data[bitlen >> 3] |= (uint8_t)((((val) >> b_) & 1) << (7 - (bitlen & 7))); \
	} while (0)
	PUT(4, 4);
	PUT((int)len, ver < 10 ? 8 : 16);
	for (size_t i = 0; i < len; i++)
		PUT((unsigned char)text[i], 8);
	bitlen += cap * 8 - bitlen < 4 ? cap * 8 - bitlen : 4;
	bitlen = (bitlen + 7) & ~7;
	for (int i = bitlen / 8, pad = 0xec; i < cap; i++, pad ^= 0xec ^ 0x11)
		data[i] = (uint8_t)pad;
#undef PUT

	/* split into blocks, add ECC, interleave */
	nb = NUM_BLOCKS[ver];
	eccl = ECC_PER_BLOCK[ver];
	raw = raw_modules(ver) / 8;
	nshort = nb - raw % nb;
	shortlen = raw / nb;
	{
		uint8_t blocks[5][160];
		int dlen[5], off = 0;

		for (int i = 0; i < nb; i++) {
			dlen[i] = shortlen - eccl + (i < nshort ? 0 : 1);
			memcpy(blocks[i], data + off, (size_t)dlen[i]);
			qr_rs_remainder(data + off, dlen[i], eccl, ecc);
			memcpy(blocks[i] + dlen[i], ecc, (size_t)eccl);
			off += dlen[i];
		}
		for (int i = 0; i < shortlen + 1; i++)
			for (int j = 0; j < nb; j++) {
				int blen = dlen[j] + eccl;
				int idx;

				/* short blocks have no data byte at shortlen - eccl */
				if (j < nshort && i == shortlen - eccl)
					continue;
				idx = (j < nshort && i > shortlen - eccl) ? i - 1 : i;
				if (idx < blen)
					all[k++] = blocks[j][idx];
			}
	}

	memset(out, 0, sizeof(*out));
	memset(&q, 0, sizeof(q));
	q.size = 17 + 4 * ver;
	q.m = out->m;
	draw_function_patterns(&q, ver);
	draw_codewords(&q, all, k);
	for (int mask = 0; mask < 8; mask++) {
		int p;

		apply_mask(&q, mask);
		draw_format(&q, mask);
		p = penalty(&q);
		if (bestp < 0 || p < bestp) {
			bestp = p;
			best = mask;
		}
		apply_mask(&q, mask);                   /* undo (XOR) */
	}
	apply_mask(&q, best);
	draw_format(&q, best);
	out->version = ver;
	out->size = q.size;
	out->mask = best;
	return 0;
}
