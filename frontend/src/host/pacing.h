/*
 * pacing.h - frame pacing policy and dynamic rate control (DRC).
 * Pure maths (no I/O), unit-tested in tests/test_pacing.c.
 *
 * Modes:
 *   PACE_VSYNC  |disp_hz - core_fps| / core_fps <= tolerance (1 %):
 *               one retro_run() per vblank, paced by the blocking page
 *               flip. The core then runs at disp_hz instead of core_fps, so
 *               it produces core_rate * disp_hz / core_fps samples per
 *               second; the resampler converts that to AUDIO_RATE:
 *                   base = AUDIO_RATE / core_rate * core_fps / disp_hz
 *               and DRC corrects the remaining clock drift from the
 *               buffer fill f (target 1/2):
 *                   ratio = base * (1 + d * clamp((0.5 - f) / 0.5, -1, 1))
 *               with d = 0.005 (the pitch change is inaudible). Neither
 *               audio nor video ever drops.
 *   PACE_AUDIO  otherwise, with audio: the core runs at its own fps. Before
 *               each retro_run() the loop waits until the ALSA queue is
 *               down to the target; video uses the display's latest-frame
 *               semantics (a frame is shown at the next vblank; if no
 *               buffer is free it is skipped, and the display repeats the
 *               last one when the core is slower than the screen).
 *               ratio = AUDIO_RATE / core_rate (no DRC: audio is the clock).
 *   PACE_TIMER  no audio device: CLOCK_MONOTONIC at core_fps.
 *   PACE_FREE   headless tests: as fast as possible.
 */
#ifndef RSOS_HOST_PACING_H
#define RSOS_HOST_PACING_H

#include <stdbool.h>
#include <stdint.h>

enum pace_mode { PACE_FREE = 0, PACE_VSYNC, PACE_AUDIO, PACE_TIMER };

struct pacing {
	enum pace_mode mode;
	double core_fps;
	double core_rate;
	double disp_hz;       /* 0 = no display */
	double out_rate;
	double base_ratio;    /* output frames per input frame, before DRC */
	double max_delta;     /* DRC range, default 0.005 */
	double tolerance;     /* vsync window, default 0.01 */
	double target_fill;   /* 0.5 */
};

void pacing_setup(struct pacing *p, double core_fps, double core_rate, double disp_hz,
		  double out_rate, bool have_display, bool have_audio, double tolerance);

/* Resample ratio for this frame, given the buffer fill (0..1). */
double pacing_ratio(const struct pacing *p, double fill);

/* The rate the core is actually driven at (GET_TARGET_REFRESH_RATE). */
double pacing_run_hz(const struct pacing *p);

/* PACE_AUDIO: microseconds to wait before the next retro_run(), given the
 * frames queued in ALSA and the target (frames). */
int64_t pacing_audio_wait_us(const struct pacing *p, int queued, int target);

const char *pacing_mode_name(enum pace_mode m);

#endif
