/*
 * battery.h - pure battery logic for the power module: open-circuit
 * voltage curve, smoothing, low-battery levels with hysteresis and the
 * critical-battery detector. No I/O, so it is unit tested with simulated
 * discharge curves (src/power/tests/power_test.c).
 *
 * Sign convention: current in mA, positive = charging, negative =
 * discharging (the axp20x-battery current_now convention).
 */
#ifndef RSOS_POWER_BATTERY_H
#define RSOS_POWER_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/* Where the displayed percentage comes from. */
enum batt_gauge {
	BATT_GAUGE_AUTO = 0,   /* AXP fuel gauge when plausible, else voltage */
	BATT_GAUGE_AXP,        /* AXP209 REG B9 only */
	BATT_GAUGE_VOLTAGE,    /* voltage curve (IR-compensated) only */
};

enum batt_state {
	BATT_UNKNOWN = 0,
	BATT_DISCHARGING,
	BATT_CHARGING,
	BATT_FULL,
	BATT_NOT_CHARGING,     /* charger present, not charging (e.g. too hot) */
};

/* Ordered by severity. */
enum batt_level {
	BATT_LEVEL_OK = 0,
	BATT_LEVEL_LOW,        /* <= warn_pct (15 %): a toast */
	BATT_LEVEL_VERY_LOW,   /* <= warn2_pct (7 %): a persistent warning */
	BATT_LEVEL_CRITICAL,   /* emergency save + power-off */
};

struct batt_params {
	enum batt_gauge gauge;
	int capacity_mah;      /* 4000, for the time-left estimate */
	int r_int_mohm;        /* cell + protection + wiring, IR compensation (150) */
	int tau_s;             /* smoothing time constant (60 s) */
	int warn_pct;          /* 15 */
	int warn2_pct;         /* 7 */
	int warn_hyst_pct;     /* 3: a level clears only above threshold + hyst */
	int crit_mv;           /* 3450: loaded voltage that starts the countdown */
	int crit_hyst_mv;      /* 50: the countdown resets only above crit + hyst */
	int crit_samples;      /* 4 filtered samples below crit_mv ... */
	int crit_min_ms;       /* ... spanning at least 6000 ms */
	int emerg_mv;          /* 3300: filtered AND the last 2 raw samples below: now */
	int axp_max_diff_pct;  /* auto gauge: AXP value rejected if it differs more (30) */
};

void batt_params_defaults(struct batt_params *p);

struct batt_sample {
	int64_t t_ms;          /* monotonic */
	bool present;
	bool charger;          /* external power online (ACIN/USB) */
	enum batt_state state;
	int mv;                /* loaded battery voltage, <= 0 = read failed */
	int ma;                /* signed current, see above */
	bool ma_valid;
	int axp_pct;           /* AXP fuel gauge, -1 = not available */
};

#define BATT_WIN 5
/* After boot or a charger plug/unplug the displayed % follows the estimate
 * freely for this long (surface charge, boot load); then it only moves in
 * the direction of the current (down when discharging, up when charging). */
#define BATT_SETTLE_MS 180000
/* Auto gauge: agreeing samples (|axp - voltage| <= diff/2) needed to trust
 * the AXP value again after a disagreement (> diff). */
#define BATT_AXP_RETRUST 10

struct batt {
	struct batt_params p;
	bool have;
	double pct_f;          /* smoothed estimate */
	int shown;             /* what the icon shows (monotonic per direction) */
	int dir;               /* -1 discharging, +1 charging, 0 other */
	int64_t dir_since;     /* start of the current direction (settling window) */
	bool axp_trust;        /* auto gauge: the AXP value agrees with the voltage */
	int axp_good;          /* consecutive agreeing samples (re-trust) */
	int64_t last_t;
	double ma_avg;
	int vwin[BATT_WIN];
	int vn, vpos;
	int last_raw[2];       /* last two raw voltages (emergency rule) */
	int nraw;
	/* critical detector */
	int below_count;
	int64_t below_since;
	bool critical;
	enum batt_level level;
};

struct batt_out {
	int percent;           /* displayed, 0..100, -1 unknown */
	int pct_voltage;       /* voltage-curve estimate of this sample, -1 */
	int mv_filtered;       /* median of the last BATT_WIN loaded voltages */
	int ocv_mv;            /* IR-compensated open-circuit estimate */
	int ma_avg;
	int minutes_left;      /* to empty (discharging) or full (charging), -1 */
	enum batt_level level;
	bool level_changed;
	bool critical_now;     /* true exactly once, on the sample that triggers */
};

void batt_init(struct batt *b, const struct batt_params *p);
/* Feeds one sample. Samples with mv <= 0 are ignored (out is still filled). */
void batt_update(struct batt *b, const struct batt_sample *s, struct batt_out *out);

/* Typical Li-ion (LiCoO2/NMC) open-circuit voltage to % at room temperature.
 * TODO(hw): calibrate against a logged full discharge of the real cell. */
int batt_ocv_to_pct(int ocv_mv);

const char *batt_state_name(enum batt_state s);
enum batt_state batt_state_parse(const char *sysfs_status);

#endif
