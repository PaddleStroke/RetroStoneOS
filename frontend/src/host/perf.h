/*
 * perf.h - performance counters of the game process: emulated speed, frames
 * delivered/shown/skipped, core and present time, per-CPU load from
 * /proc/stat, GPU/present timing of the GLES path. One snapshot/diff model
 * used by the FPS overlay (1 s windows), the game.log summary (10 s windows
 * and the session) and the benchmark (the measured window).
 * Design: docs/host-design.md §8 ("Performance counters").
 */
#ifndef RSOS_HOST_PERF_H
#define RSOS_HOST_PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PERF_MAX_CPUS 8

/* Cumulative jiffies per CPU (from /proc/stat "cpuN" lines). */
struct perf_cpu {
	int n;
	uint64_t busy[PERF_MAX_CPUS];
	uint64_t total[PERF_MAX_CPUS];
};

/* Parses /proc/stat text; returns the number of CPUs found (0 = none). */
int perf_parse_proc_stat(const char *text, struct perf_cpu *out);
int perf_read_cpu(struct perf_cpu *out);
/* Busy percentage of each CPU between two samples; returns the count. */
int perf_cpu_busy(const struct perf_cpu *a, const struct perf_cpu *b, double *pct, int max);

/* Counters the host increments (fields of struct host, gathered here). */
struct perf_counters {
	int64_t t_us;              /* CLOCK_MONOTONIC */
	int64_t proc_cpu_us;       /* CLOCK_PROCESS_CPUTIME_ID */
	uint64_t runs;             /* retro_run() calls */
	uint64_t delivered;        /* video_refresh with a new frame (not a dupe) */
	uint64_t presented;        /* frames that reached the display */
	uint64_t skipped;          /* frames the host did not show (no free buffer) */
	uint64_t dupes;            /* video_refresh(NULL) */
	uint64_t dropped;          /* the display replaced before scanout */
	double core_ms;            /* sum of core time (retro_run minus present) */
	double core_ms_max;        /* max since the previous snapshot (reset by the host) */
	/* GLES path (hwrender.c), cumulative microseconds and counts */
	uint64_t gl_swap_us;       /* blit + eglSwapBuffers + lock front buffer */
	uint64_t gl_present_us;    /* display_present_fb() (includes the flip wait) */
	uint64_t gl_readback_us;   /* glReadPixels + swizzle */
	uint64_t gl_swaps, gl_readbacks;
	unsigned underruns;
	struct perf_cpu cpu;
	/* GL calls that can stall a frame (glprobe.h), cumulative; gl = probed */
	bool gl;
	uint64_t gl_compiles, gl_compile_us, gl_links, gl_link_us;
	uint64_t gl_slow_draws, gl_slow_draw_us, gl_tex, gl_tex_us;
	/* frames whose core time was over PERF_STALL_MS, and their total */
	uint64_t stalls;
	double stall_ms;
	int cpu_khz;               /* cpu0 clock at the snapshot (scaling_cur_freq), 0 = unknown */
};

/* A frame whose core time exceeds this is a stall (logged, counted). */
#define PERF_STALL_MS 50.0

/* cpu0's current clock in kHz from cpufreq (0 = unknown). path NULL = the
 * sysfs default. */
int perf_read_cpu_khz(const char *path);

struct perf_report {
	double secs;
	double speed_pct;          /* runs per second / core fps */
	double vi_rate;            /* runs per second */
	double fps;                /* delivered frames per second */
	double shown_fps;          /* presented per second */
	double eff_fps;            /* fps the game shows at real-time speed */
	double core_ms_avg, core_ms_max;
	double swap_ms, present_ms, readback_ms; /* per swap / per readback */
	double proc_cpu_pct;       /* this process, % of one CPU */
	uint64_t runs, delivered, presented, skipped, dupes, dropped;
	unsigned underruns;
	int ncpu;
	double cpu[PERF_MAX_CPUS];
	bool gl;
	uint64_t gl_compiles, gl_links, gl_slow_draws, gl_tex;
	double gl_compile_ms, gl_link_ms, gl_slow_draw_ms, gl_tex_ms;
	uint64_t stalls;
	double stall_ms;
	int cpu_mhz_lo, cpu_mhz_hi;  /* cpu0 clock at the two ends of the window */
};

/* b - a over the window; core_fps is the core's nominal rate (60 for N64). */
void perf_diff(const struct perf_counters *a, const struct perf_counters *b, double core_fps,
	       struct perf_report *r);

/* Effective fps: delivered frames per VI x core fps, capped at real time. */
double perf_eff_fps(uint64_t delivered, uint64_t runs, double core_fps, double speed_pct);

/* One line for game.log ("speed 97 %, 29.8 fps, ..."). */
int perf_format_log(const struct perf_report *r, char *buf, size_t n);
/* Short overlay line ("SPD 97% FPS 29.8 SKIP 12 CPU 95/31 GPU 4.1"). */
int perf_format_overlay(const struct perf_report *r, uint64_t skipped_total, char *buf, size_t n);

/* p-th percentile (0..100) of v[0..n) (sorts v in place); 0 if n = 0. */
double perf_percentile(float *v, size_t n, double p);

#endif
