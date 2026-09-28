/*
 * bench.h - the in-game benchmark (N64): plans, results, ranking, report,
 * and writing the chosen settings. Pure functions (unit-tested in
 * test_host.c); the process orchestration is in bench_run.c and host.c.
 *
 * Flow (docs/host-design.md §18):
 *   game process (menu "Benchmark")  -- saves <tmp>/bench/start.state, writes
 *        the plan, exec -->  driver (--bench-driver PLAN): for each config
 *        fork+exec a step (--bench-step PLAN I: the game from start.state
 *        with the config's options, warm-up, measured window, no input,
 *        unthrottled), collect result-I.ini, rewrite the report on the SD
 *        card; then exec --> the game again from start.state with
 *        --bench-report PLAN: the results page, "Use this for this game".
 *
 * Shipped plans: <coreopts ship dir>/<system>.bench.ini
 *   [bench]  seconds = 25 ; warmup = 5
 *   [<id>]   label = ... ; core = <core id> (default: the running core) ;
 *            any other key = a core option (or rsos-glthread) for that run
 */
#ifndef RSOS_HOST_BENCH_H
#define RSOS_HOST_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ini.h"

#define BENCH_EXIT_ABORT 10       /* a step stopped by Select+Start */
#define BENCH_MAX_CONFIGS 16
#define BENCH_MAX_KEYS 12
#define BENCH_PATH 1024

struct bench_kv {
	char key[64];
	char value[64];
};

struct bench_config {
	char id[32];
	char label[64];
	char core[64];                /* core id */
	int nkv;
	struct bench_kv kv[BENCH_MAX_KEYS];
};

struct bench_plan {
	int seconds, warmup;
	int n;
	struct bench_config c[BENCH_MAX_CONFIGS];
	/* [run]: written when a benchmark starts */
	char game[256], system[64], rom_stem[256];
	char core_id[64], core_path[BENCH_PATH];
	char state[BENCH_PATH];       /* the start state */
	char state_core[64];          /* core that wrote it */
	char results[BENCH_PATH];     /* the report on the SD card (.txt) */
	char auto_state[BENCH_PATH];  /* <states>/<game>.state.auto (power-off) */
	char date[32];
};

/* Plan from a parsed ini ([bench], [run], one section per config). If
 * default_core is given, configs without `core` get it. 0 or -EINVAL (no
 * config). */
int bench_plan_parse(const struct ini *ini, const char *default_core, struct bench_plan *p);
int bench_plan_load(const char *path, const char *default_core, struct bench_plan *p);
int bench_plan_write(const char *path, const struct bench_plan *p);
/* Seconds the whole run takes (per config: load + warm-up + measure). */
int bench_estimate_s(const struct bench_plan *p);
/* result-<i>.ini next to the plan. */
void bench_result_path(const char *plan_path, int i, char *out, size_t n);

struct bench_result {
	char id[32], label[64], core[64];
	char status[16];              /* ok | crash | hang | abort | error */
	bool from_boot;               /* the state could not be used: another scene */
	bool blank;                   /* the last picture is one flat colour */
	double core_fps;              /* the core's nominal rate */
	double secs, speed, fps, eff_fps, vi;
	double ft_avg, ft_p95, ft_p99; /* frame time, ms */
	double core_ms, swap_ms, present_ms, readback_ms, proc_cpu;
	int ncpu;
	double cpu[4];
	unsigned long long runs, delivered, presented, skipped, dupes;
	char shot[BENCH_PATH];        /* the last picture (PNG) */
	char note[96];
};

int bench_result_format(const struct bench_result *r, char *buf, size_t n);
int bench_result_parse(const char *text, struct bench_result *r);
int bench_result_load(const char *path, struct bench_result *r);

/* A result that can be recommended: ran, same scene, a picture. */
bool bench_eligible(const struct bench_result *r);
/*
 * Ranks results: eligible ones first; among them the ones at full speed
 * (>= 98 %) first, ordered by effective fps (what the game shows at real
 * speed) then speed; then the others by speed. order[] gets all indices.
 * Returns the index of the best, or -1.
 */
int bench_rank(const struct bench_result *r, int n, int *order);

/* The report (plain text, for a PC). best < 0: none. */
int bench_format_report(const struct bench_plan *p, const struct bench_result *r, int n, int best,
			bool finished, char *buf, size_t size);

/* Fraction (0..1) of pixels in the most common colour (4 bits/channel):
 * close to 1 = a blank or single-colour picture. rgb = w*h*3. */
double bench_flat_fraction(const uint8_t *rgb, int w, int h);

/* Original arguments of the game process minus the benchmark/resume ones
 * (--load-state, --load-state-file, --bench-*, and --status-fd /
 * --poweroff-cmd when `for_step`). out gets at most max-1 pointers + NULL.
 * Returns the count. */
int bench_filter_args(int argc, char *const *argv, bool for_step, const char **out, int max);

/*
 * "Use this for this game": merges the config's keys into the per-game
 * core options file <user_dir>/<core>/<game>.ini (other keys kept), and, if
 * the config's core is not `current_core`, sets [<system>/<rom_stem>]
 * core = <core> in choices_path (/data/rsos/cores.ini). 0 or -errno.
 */
int bench_apply(const struct bench_config *c, const char *user_dir, const char *game,
		const char *choices_path, const char *system, const char *rom_stem,
		const char *current_core);
/* Merges key = "value" pairs into an options file (atomic). */
int bench_write_keys(const char *path, const char *header, const struct bench_kv *kv, int n);
/* Sets [section] key = value in an ini file, keeping the rest (atomic). */
int bench_ini_set_file(const char *path, const char *section, const char *key, const char *value);

/* The config a result belongs to (by id), or NULL. */
const struct bench_config *bench_find_config(const struct bench_plan *p, const char *id);

/* Makes a file-name-safe copy ("Super Mario 64 (U)" -> "Super_Mario_64_U"). */
void bench_safe_name(const char *in, char *out, size_t n);

/* bench_run.c: the driver process (--bench-driver PLAN). Never returns on
 * success (it re-executes the game); returns an enum host_exit otherwise. */
struct host_config;
int bench_driver_main(const struct host_config *cfg, const char *plan_path);

#endif
