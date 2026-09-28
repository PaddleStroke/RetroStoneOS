/*
 * pacing.c - see pacing.h.
 */
#include "pacing.h"

#include <math.h>

void pacing_setup(struct pacing *p, double core_fps, double core_rate, double disp_hz,
		  double out_rate, bool have_display, bool have_audio, double tolerance)
{
	if (core_fps <= 1 || core_fps > 500)
		core_fps = 60;
	if (core_rate <= 1000)
		core_rate = out_rate; /* cores with no audio (or bogus rates) */
	p->core_fps = core_fps;
	p->core_rate = core_rate;
	p->disp_hz = have_display ? disp_hz : 0;
	p->out_rate = out_rate;
	p->max_delta = 0.005;
	p->tolerance = tolerance > 0 ? tolerance : 0.01;
	p->target_fill = 0.5;

	if (!have_display && !have_audio)
		p->mode = PACE_FREE;
	else if (have_display && disp_hz > 1 && fabs(disp_hz - core_fps) / core_fps <= p->tolerance)
		p->mode = PACE_VSYNC;
	else if (have_audio)
		p->mode = PACE_AUDIO;
	else
		p->mode = PACE_TIMER;

	p->base_ratio = out_rate / core_rate;
	if (p->mode == PACE_VSYNC)
		p->base_ratio *= core_fps / disp_hz;
}

double pacing_ratio(const struct pacing *p, double fill)
{
	double d;

	if (p->mode != PACE_VSYNC)
		return p->base_ratio;
	d = (p->target_fill - fill) / p->target_fill;
	if (d > 1)
		d = 1;
	if (d < -1)
		d = -1;
	return p->base_ratio * (1.0 + p->max_delta * d);
}

double pacing_run_hz(const struct pacing *p)
{
	return p->mode == PACE_VSYNC ? p->disp_hz : p->core_fps;
}

int64_t pacing_audio_wait_us(const struct pacing *p, int queued, int target)
{
	if (queued <= target)
		return 0;
	return (int64_t)((double)(queued - target) * 1e6 / p->out_rate);
}

const char *pacing_mode_name(enum pace_mode m)
{
	switch (m) {
	case PACE_VSYNC: return "vsync+DRC";
	case PACE_AUDIO: return "audio clock";
	case PACE_TIMER: return "timer";
	default: return "free-run";
	}
}
