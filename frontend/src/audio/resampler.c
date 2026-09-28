/*
 * resampler.c - see resampler.h.
 *
 * Polyphase windowed sinc. For an output frame at input position
 * t = i + f (0 <= f < 1):
 *
 *     y(t) = sum_{k=0}^{15} x[i - 7 + k] * h(k - 7 - f)
 *     h(x) = fc * sinc(fc * x) * kaiser(x / 8, beta = 6)
 *
 * h is tabulated for f = p / 128 (p = 0..128) and each row is normalised
 * to a DC gain of 1; between two rows the coefficients are interpolated
 * linearly. The input history is kept deinterleaved (L[] and R[] floats)
 * so the NEON dot products are straight 4-wide loads.
 */
#include "resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define RS_NEON 1
#endif

#define TAPS   16
#define HALF   8          /* taps before/after: indices i-7 .. i+8 */
#define PHASES 128
#define HIST   (HALF - 1) /* frames kept before the current position */

struct resampler {
	enum resampler_quality q;
	int cap;              /* capacity of L/R in frames */
	int n;                /* valid frames */
	double pos;           /* next output position (frames, into L/R) */
	float *L, *R;
	float fc;             /* cutoff of the current table */
	float table[(PHASES + 1) * TAPS] __attribute__((aligned(16)));
};

static double bessel_i0(double x)
{
	double sum = 1, term = 1;

	for (int k = 1; k < 30; k++) {
		term *= (x / (2 * k)) * (x / (2 * k));
		sum += term;
		if (term < 1e-12 * sum)
			break;
	}
	return sum;
}

static void build_table(struct resampler *r, float fc)
{
	const double beta = 6.0;
	const double i0b = bessel_i0(beta);

	for (int p = 0; p <= PHASES; p++) {
		double f = (double)p / PHASES, sum = 0;
		float *row = &r->table[p * TAPS];

		for (int k = 0; k < TAPS; k++) {
			double x = (double)k - (HALF - 1) - f;   /* -8 .. 8 */
			double w, s, u = x / HALF;

			w = fabs(u) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - u * u)) / i0b;
			s = x == 0.0 ? 1.0 : sin(M_PI * fc * x) / (M_PI * fc * x);
			row[k] = (float)(fc * s * w);
			sum += row[k];
		}
		for (int k = 0; k < TAPS; k++)
			row[k] = (float)(row[k] / sum);
	}
	r->fc = fc;
}

struct resampler *resampler_new(enum resampler_quality q, int max_in)
{
	struct resampler *r;

	if (max_in < 64)
		max_in = 64;
	if (posix_memalign((void **)&r, 16, sizeof(*r)))
		return NULL;
	memset(r, 0, sizeof(*r));
	r->q = q;
	r->cap = max_in + TAPS + 16;
	r->L = calloc((size_t)r->cap, sizeof(float));
	r->R = calloc((size_t)r->cap, sizeof(float));
	if (!r->L || !r->R) {
		resampler_free(r);
		return NULL;
	}
	build_table(r, 0.91f);
	resampler_reset(r);
	return r;
}

void resampler_free(struct resampler *r)
{
	if (!r)
		return;
	free(r->L);
	free(r->R);
	free(r);
}

void resampler_reset(struct resampler *r)
{
	memset(r->L, 0, (size_t)r->cap * sizeof(float));
	memset(r->R, 0, (size_t)r->cap * sizeof(float));
	r->n = HIST;
	r->pos = HIST;
}

static inline int16_t clamp16(float v)
{
	v *= 32768.0f;
	if (v >= 32767.0f)
		return 32767;
	if (v <= -32768.0f)
		return -32768;
	return (int16_t)lrintf(v);
}

static inline void sinc_frame(const struct resampler *r, int i, float f, float *outl, float *outr)
{
	float pf = f * PHASES;
	int p = (int)pf;
	float mu = pf - (float)p;
	const float *c0;

	/* A fraction just below 1 rounds to 1.0f: p = PHASES, and c1 would be
	 * the row after the table (review F-M1). Row PHASES is the end point. */
	if (p >= PHASES) {
		p = PHASES - 1;
		mu = 1.0f;
	} else if (p < 0) {
		p = 0;
		mu = 0.0f;
	}
	c0 = &r->table[p * TAPS];
	const float *c1 = c0 + TAPS;
	const float *xl = &r->L[i - (HALF - 1)];
	const float *xr = &r->R[i - (HALF - 1)];
#ifdef RS_NEON
	float32x4_t accl = vdupq_n_f32(0), accr = vdupq_n_f32(0);
	float32x4_t vmu = vdupq_n_f32(mu);

	for (int k = 0; k < TAPS; k += 4) {
		float32x4_t a = vld1q_f32(c0 + k);
		float32x4_t b = vld1q_f32(c1 + k);
		float32x4_t c = vmlaq_f32(a, vsubq_f32(b, a), vmu);

		accl = vmlaq_f32(accl, vld1q_f32(xl + k), c);
		accr = vmlaq_f32(accr, vld1q_f32(xr + k), c);
	}
	{
		float32x2_t l2 = vadd_f32(vget_low_f32(accl), vget_high_f32(accl));
		float32x2_t r2 = vadd_f32(vget_low_f32(accr), vget_high_f32(accr));
		float32x2_t s = vpadd_f32(l2, r2);

		*outl = vget_lane_f32(s, 0);
		*outr = vget_lane_f32(s, 1);
	}
#else
	float sl = 0, sr = 0;

	for (int k = 0; k < TAPS; k++) {
		float c = c0[k] + (c1[k] - c0[k]) * mu;

		sl += xl[k] * c;
		sr += xr[k] * c;
	}
	*outl = sl;
	*outr = sr;
#endif
}

int resampler_run(struct resampler *r, const int16_t *in, int n_in,
		  int16_t *out, int out_cap, double ratio)
{
	int produced = 0;
	double step;

	if (ratio < 0.125)
		ratio = 0.125;
	if (ratio > 8)
		ratio = 8;
	step = 1.0 / ratio;

	if (r->q == RESAMPLER_SINC) {
		float fc = ratio < 1.0 ? (float)(0.91 * ratio) : 0.91f;

		if (fabsf(fc - r->fc) > 0.02f * r->fc)
			build_table(r, fc);
	}

	while (n_in > 0) {
		int take = r->cap - r->n;

		if (take > n_in)
			take = n_in;
		for (int k = 0; k < take; k++) {
			r->L[r->n + k] = in[2 * k] * (1.0f / 32768.0f);
			r->R[r->n + k] = in[2 * k + 1] * (1.0f / 32768.0f);
		}
		r->n += take;
		in += 2 * take;
		n_in -= take;

		/* Produce everything the history allows. */
		for (;;) {
			int i = (int)r->pos;
			float f = (float)(r->pos - i), l, rr;

			if (i + HALF >= r->n || produced >= out_cap)
				break;
			if (r->q == RESAMPLER_SINC) {
				sinc_frame(r, i, f, &l, &rr);
			} else {
				l = r->L[i] + (r->L[i + 1] - r->L[i]) * f;
				rr = r->R[i] + (r->R[i + 1] - r->R[i]) * f;
			}
			out[2 * produced] = clamp16(l);
			out[2 * produced + 1] = clamp16(rr);
			produced++;
			r->pos += step;
		}

		/* Drop what is no longer needed, keep HIST frames of history. */
		{
			int drop = (int)r->pos - HIST;

			if (drop > r->n)
				drop = r->n;
			if (drop > 0) {
				memmove(r->L, r->L + drop, (size_t)(r->n - drop) * sizeof(float));
				memmove(r->R, r->R + drop, (size_t)(r->n - drop) * sizeof(float));
				r->n -= drop;
				r->pos -= drop;
			}
		}
		if (produced >= out_cap)
			break;
	}
	return produced;
}
