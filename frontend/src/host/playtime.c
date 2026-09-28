/*
 * playtime.c - see playtime.h.
 */
#include "playtime.h"

#include <string.h>

void pt_start(struct playtime *p, int64_t now_ms)
{
	memset(p, 0, sizeof(*p));
	p->since_ms = now_ms;
	p->started = true;
}

bool pt_running(const struct playtime *p)
{
	return p->started && p->paused == 0;
}

void pt_pause(struct playtime *p, int64_t now_ms)
{
	if (!p->started)
		return;
	if (p->paused++ == 0 && now_ms > p->since_ms)
		p->total_ms += now_ms - p->since_ms;
}

void pt_resume(struct playtime *p, int64_t now_ms)
{
	if (!p->started || p->paused == 0)
		return;
	if (--p->paused == 0)
		p->since_ms = now_ms;
}

int64_t pt_total_ms(const struct playtime *p, int64_t now_ms)
{
	if (!p->started)
		return 0;
	if (p->paused == 0 && now_ms > p->since_ms)
		return p->total_ms + (now_ms - p->since_ms);
	return p->total_ms;
}
