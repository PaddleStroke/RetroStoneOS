/*
 * battery.c - pure battery logic (see battery.h).
 */
#include "battery.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void batt_params_defaults(struct batt_params *p)
{
	memset(p, 0, sizeof(*p));
	p->gauge = BATT_GAUGE_AUTO;
	p->capacity_mah = 4000;
	p->r_int_mohm = 150;
	p->tau_s = 60;
	p->warn_pct = 15;
	p->warn2_pct = 7;
	p->warn_hyst_pct = 3;
	p->crit_mv = 3450;
	p->crit_hyst_mv = 50;
	p->crit_samples = 4;
	p->crit_min_ms = 6000;
	p->emerg_mv = 3300;
	p->axp_max_diff_pct = 30;
}

/*
 * Resting voltage of a typical 4.2 V Li-ion cell (0.1-0.2 C, 25 C).
 * The low end is steep: 3.45 V at rest is ~3 %, and under a 0.5 A load
 * the loaded voltage reaches it earlier, which is why the critical
 * detector uses the loaded voltage and not this curve.
 */
static const struct { int mv, pct; } ocv_table[] = {
	{ 3000, 0 }, { 3300, 1 }, { 3450, 3 }, { 3550, 6 }, { 3620, 10 },
	{ 3680, 17 }, { 3720, 25 }, { 3750, 32 }, { 3780, 40 }, { 3810, 48 },
	{ 3840, 55 }, { 3880, 62 }, { 3920, 69 }, { 3970, 76 }, { 4020, 82 },
	{ 4080, 89 }, { 4130, 94 }, { 4170, 98 }, { 4200, 100 },
};
#define OCV_N ((int)(sizeof(ocv_table) / sizeof(ocv_table[0])))

int batt_ocv_to_pct(int mv)
{
	int i;

	if (mv <= ocv_table[0].mv)
		return 0;
	if (mv >= ocv_table[OCV_N - 1].mv)
		return 100;
	for (i = 1; i < OCV_N; i++) {
		if (mv < ocv_table[i].mv) {
			int v0 = ocv_table[i - 1].mv, v1 = ocv_table[i].mv;
			int p0 = ocv_table[i - 1].pct, p1 = ocv_table[i].pct;
			return p0 + ((mv - v0) * (p1 - p0) + (v1 - v0) / 2) / (v1 - v0);
		}
	}
	return 100;
}

const char *batt_state_name(enum batt_state s)
{
	switch (s) {
	case BATT_DISCHARGING: return "discharging";
	case BATT_CHARGING: return "charging";
	case BATT_FULL: return "full";
	case BATT_NOT_CHARGING: return "not-charging";
	default: return "unknown";
	}
}

enum batt_state batt_state_parse(const char *s)
{
	if (!s)
		return BATT_UNKNOWN;
	if (!strcmp(s, "Charging"))
		return BATT_CHARGING;
	if (!strcmp(s, "Discharging"))
		return BATT_DISCHARGING;
	if (!strcmp(s, "Full"))
		return BATT_FULL;
	if (!strcmp(s, "Not charging"))
		return BATT_NOT_CHARGING;
	return BATT_UNKNOWN;
}

void batt_init(struct batt *b, const struct batt_params *p)
{
	memset(b, 0, sizeof(*b));
	if (p)
		b->p = *p;
	else
		batt_params_defaults(&b->p);
	b->shown = -1;
}

static int median_mv(const struct batt *b)
{
	int v[BATT_WIN], n = b->vn, i, j;

	for (i = 0; i < n; i++)
		v[i] = b->vwin[i];
	for (i = 1; i < n; i++) {
		int x = v[i];
		for (j = i - 1; j >= 0 && v[j] > x; j--)
			v[j + 1] = v[j];
		v[j + 1] = x;
	}
	/* even count: the lower middle, i.e. the pessimistic one */
	return n ? v[(n - 1) / 2] : 0;
}

static int clamp_pct(long v)
{
	return v < 0 ? 0 : v > 100 ? 100 : (int)v;
}

static void update_level(struct batt *b, bool charger, int pct, struct batt_out *o)
{
	const struct batt_params *p = &b->p;
	enum batt_level old = b->level, target;

	if (b->critical) {
		b->level = BATT_LEVEL_CRITICAL;
	} else if (charger) {
		b->level = BATT_LEVEL_OK;
	} else if (pct >= 0) {
		target = pct <= p->warn2_pct ? BATT_LEVEL_VERY_LOW :
			 pct <= p->warn_pct ? BATT_LEVEL_LOW : BATT_LEVEL_OK;
		if (b->level == BATT_LEVEL_CRITICAL)
			b->level = target;      /* critical was cleared by a charger */
		if (target > b->level)
			b->level = target;      /* worse: immediately */
		while (b->level > target) { /* better: only past the hysteresis */
			int thr = b->level == BATT_LEVEL_VERY_LOW ? p->warn2_pct : p->warn_pct;
			if (pct < thr + p->warn_hyst_pct)
				break;
			b->level--;
		}
	}
	o->level = b->level;
	o->level_changed = b->level != old;
}

void batt_update(struct batt *b, const struct batt_sample *s, struct batt_out *o)
{
	const struct batt_params *p = &b->p;
	bool ext = s->charger || s->state == BATT_CHARGING || s->state == BATT_FULL;
	int m, ocv, pv, est, dir, r;
	double alpha;

	memset(o, 0, sizeof(*o));
	o->percent = b->shown;
	o->pct_voltage = -1;
	o->minutes_left = -1;
	o->level = b->level;

	if (!s->present || s->mv <= 0) {
		o->mv_filtered = b->vn ? median_mv(b) : 0;
		return;
	}

	/* median filter on the loaded voltage */
	b->vwin[b->vpos] = s->mv;
	b->vpos = (b->vpos + 1) % BATT_WIN;
	if (b->vn < BATT_WIN)
		b->vn++;
	b->last_raw[1] = b->last_raw[0];
	b->last_raw[0] = s->mv;
	if (b->nraw < 2)
		b->nraw++;
	m = median_mv(b);

	alpha = 1.0;
	if (b->have) {
		double dt = (double)(s->t_ms - b->last_t);
		if (dt < 0)
			dt = 0;
		alpha = dt / (p->tau_s * 1000.0 + dt);
	}
	if (s->ma_valid)
		b->ma_avg = b->have ? b->ma_avg + alpha * (s->ma - b->ma_avg) : s->ma;

	/* open-circuit estimate: remove the I*R drop (ma > 0 when charging) */
	ocv = m - (int)lround(b->ma_avg * p->r_int_mohm / 1000.0);
	pv = batt_ocv_to_pct(ocv);

	est = pv;
	if (s->axp_pct >= 0 && s->axp_pct <= 100) {
		int d = abs(s->axp_pct - pv);

		/* auto: distrust at once on a big disagreement, trust again
		 * only after a run of close agreement (a gauge stuck at 100 %
		 * or 0 % never flip-flops with the voltage estimate) */
		if (d > p->axp_max_diff_pct) {
			b->axp_trust = false;
			b->axp_good = 0;
		} else if (d <= p->axp_max_diff_pct / 2) {
			if (!b->have || ++b->axp_good >= BATT_AXP_RETRUST)
				b->axp_trust = true;
		}
		if (p->gauge == BATT_GAUGE_AXP ||
		    (p->gauge == BATT_GAUGE_AUTO && b->axp_trust))
			est = s->axp_pct;
	} else {
		b->axp_trust = false;
		b->axp_good = 0;
	}
	if (s->state == BATT_FULL)
		est = 100;

	b->pct_f = b->have ? b->pct_f + alpha * (est - b->pct_f) : est;
	if (s->state == BATT_FULL)
		b->pct_f = 100;
	r = clamp_pct(lround(b->pct_f));

	dir = !s->charger ? -1 : s->state == BATT_CHARGING ? 1 : 0;
	if (!b->have || dir != b->dir)
		b->dir_since = s->t_ms;
	if (b->shown < 0 || dir == 0 || s->t_ms - b->dir_since < BATT_SETTLE_MS)
		b->shown = r;                       /* settling: follow freely */
	else if (dir < 0 && r < b->shown)
		b->shown = r;                       /* discharging: never up */
	else if (dir > 0 && r > b->shown)
		b->shown = r;                       /* charging: never down */
	b->dir = dir;
	b->have = true;
	b->last_t = s->t_ms;

	/* critical detector: loaded voltage, filtered, sustained */
	if (ext) {
		b->below_count = 0;
		b->below_since = 0;
		b->critical = false;    /* re-armed by any external power */
	} else {
		bool trig, emerg;

		if (m < p->crit_mv) {
			if (!b->below_count)
				b->below_since = s->t_ms;
			b->below_count++;
		} else if (m >= p->crit_mv + p->crit_hyst_mv) {
			b->below_count = 0;
			b->below_since = 0;
		}
		trig = b->below_count >= p->crit_samples &&
		       s->t_ms - b->below_since >= p->crit_min_ms;
		emerg = m < p->emerg_mv && b->nraw >= 2 &&
			b->last_raw[0] < p->emerg_mv && b->last_raw[1] < p->emerg_mv;
		if ((trig || emerg) && !b->critical) {
			b->critical = true;
			o->critical_now = true;
		}
	}

	update_level(b, s->charger, b->shown, o);

	o->percent = b->shown;
	o->pct_voltage = pv;
	o->mv_filtered = m;
	o->ocv_mv = ocv;
	o->ma_avg = (int)lround(b->ma_avg);
	if (dir < 0 && b->ma_avg < -30)
		o->minutes_left = (int)(p->capacity_mah * b->shown / 100.0 / -b->ma_avg * 60.0);
	else if (dir > 0 && b->ma_avg > 30)
		o->minutes_left = (int)(p->capacity_mah * (100 - b->shown) / 100.0 / b->ma_avg * 60.0);
}
