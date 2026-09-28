/*
 * resampler.h - stereo S16 sample-rate converter for the libretro host.
 *
 * Converts the core's rate (32040.5 Hz SNES, 32768 Hz GBA, 44.1/48 kHz...)
 * to the fixed 48 kHz output. The ratio (output/input frames) can change on
 * every call: that is how dynamic rate control steers the audio clock.
 *
 * Two qualities:
 *  - RESAMPLER_SINC: 16-tap Kaiser-windowed sinc, 128 phases with linear
 *    interpolation between phases, cutoff at min(1, ratio) x 0.91 of the
 *    input Nyquist (so downsampling does not alias). Float, NEON path
 *    (4 x vmlaq per channel per output frame). ~1.5 M MAC/s at 48 kHz.
 *  - RESAMPLER_LINEAR: two-tap interpolation, for debugging / tiny CPUs.
 *
 * Single-threaded, no allocation after resampler_new().
 */
#ifndef RSOS_AUDIO_RESAMPLER_H
#define RSOS_AUDIO_RESAMPLER_H

#include <stdint.h>

enum resampler_quality { RESAMPLER_LINEAR = 0, RESAMPLER_SINC };

struct resampler;

/* max_in: largest input chunk (frames) passed to resampler_run(). */
struct resampler *resampler_new(enum resampler_quality q, int max_in);
void resampler_free(struct resampler *r);
void resampler_reset(struct resampler *r);

/*
 * Consumes all `n_in` stereo frames and writes up to out_cap stereo frames
 * (out_cap >= n_in * ratio + 4 is always enough). ratio = out_rate / in_rate
 * (DRC included), clamped to [1/8, 8]. Returns the frames written.
 */
int resampler_run(struct resampler *r, const int16_t *in, int n_in,
		  int16_t *out, int out_cap, double ratio);

#endif
