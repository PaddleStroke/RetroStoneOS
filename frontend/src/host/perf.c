/*
 * perf.c - see perf.h.
 */
#include "perf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int perf_parse_proc_stat(const char *text, struct perf_cpu *out)
{
	const char *p = text;

	memset(out, 0, sizeof(*out));
	while (p && *p) {
		unsigned long long v[10] = { 0 };
		int cpu, n;

		if (!strncmp(p, "cpu", 3) && p[3] >= '0' && p[3] <= '9') {
			n = sscanf(p, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1],
				   &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
			if (n >= 5 && cpu >= 0 && cpu < PERF_MAX_CPUS) {
				/* user nice system idle iowait irq softirq steal (guest
				 * time is already in user/nice) */
				uint64_t idle = v[3] + (n > 5 ? v[4] : 0);
				uint64_t total = 0;

				for (int i = 0; i < 8 && i < n - 1; i++)
					total += v[i];
				out->busy[cpu] = total - idle;
				out->total[cpu] = total;
				if (cpu + 1 > out->n)
					out->n = cpu + 1;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return out->n;
}

int perf_read_cpu(struct perf_cpu *out)
{
	char buf[4096];
	ssize_t r;
	int fd = open("/proc/stat", O_RDONLY | O_CLOEXEC);

	memset(out, 0, sizeof(*out));
	if (fd < 0)
		return -errno;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -EIO;
	buf[r] = 0;
	return perf_parse_proc_stat(buf, out);
}

int perf_cpu_busy(const struct perf_cpu *a, const struct perf_cpu *b, double *pct, int max)
{
	int n = a->n < b->n ? a->n : b->n;

	if (n > max)
		n = max;
	for (int i = 0; i < n; i++) {
		uint64_t dt = b->total[i] - a->total[i], db = b->busy[i] - a->busy[i];

		pct[i] = dt ? 100.0 * (double)db / (double)dt : 0;
		if (pct[i] > 100)
			pct[i] = 100;
	}
	return n;
}

double perf_eff_fps(uint64_t delivered, uint64_t runs, double core_fps, double speed_pct)
{
	double s = speed_pct / 100.0;

	if (!runs || core_fps <= 0)
		return 0;
	if (s > 1)
		s = 1;
	return (double)delivered / (double)runs * core_fps * s;
}

void perf_diff(const struct perf_counters *a, const struct perf_counters *b, double core_fps,
	       struct perf_report *r)
{
	double secs = (double)(b->t_us - a->t_us) / 1e6;

	memset(r, 0, sizeof(*r));
	if (secs <= 0)
		secs = 1e-6;
	r->secs = secs;
	r->runs = b->runs - a->runs;
	r->delivered = b->delivered - a->delivered;
	r->presented = b->presented - a->presented;
	r->skipped = b->skipped - a->skipped;
	r->dupes = b->dupes - a->dupes;
	r->dropped = b->dropped - a->dropped;
	r->underruns = b->underruns - a->underruns;
	r->vi_rate = (double)r->runs / secs;
	r->speed_pct = core_fps > 0 ? 100.0 * r->vi_rate / core_fps : 0;
	r->fps = (double)r->delivered / secs;
	r->shown_fps = (double)r->presented / secs;
	r->eff_fps = perf_eff_fps(r->delivered, r->runs, core_fps, r->speed_pct);
	r->core_ms_avg = r->runs ? (b->core_ms - a->core_ms) / (double)r->runs : 0;
	r->core_ms_max = b->core_ms_max;
	{
		uint64_t sw = b->gl_swaps - a->gl_swaps, rb = b->gl_readbacks - a->gl_readbacks;

		r->swap_ms = sw ? (double)(b->gl_swap_us - a->gl_swap_us) / 1000.0 / (double)sw : 0;
		r->present_ms = sw ? (double)(b->gl_present_us - a->gl_present_us) / 1000.0 / (double)sw : 0;
		r->readback_ms = rb ? (double)(b->gl_readback_us - a->gl_readback_us) / 1000.0 / (double)rb : 0;
	}
	r->proc_cpu_pct = 100.0 * (double)(b->proc_cpu_us - a->proc_cpu_us) / 1e6 / secs;
	r->ncpu = perf_cpu_busy(&a->cpu, &b->cpu, r->cpu, PERF_MAX_CPUS);
	r->gl = b->gl;
	r->gl_compiles = b->gl_compiles - a->gl_compiles;
	r->gl_compile_ms = (double)(b->gl_compile_us - a->gl_compile_us) / 1000.0;
	r->gl_links = b->gl_links - a->gl_links;
	r->gl_link_ms = (double)(b->gl_link_us - a->gl_link_us) / 1000.0;
	r->gl_slow_draws = b->gl_slow_draws - a->gl_slow_draws;
	r->gl_slow_draw_ms = (double)(b->gl_slow_draw_us - a->gl_slow_draw_us) / 1000.0;
	r->gl_tex = b->gl_tex - a->gl_tex;
	r->gl_tex_ms = (double)(b->gl_tex_us - a->gl_tex_us) / 1000.0;
	r->stalls = b->stalls - a->stalls;
	r->stall_ms = b->stall_ms - a->stall_ms;
	{
		int x = a->cpu_khz / 1000, y = b->cpu_khz / 1000;

		if (!x)
			x = y;
		if (!y)
			y = x;
		r->cpu_mhz_lo = x < y ? x : y;
		r->cpu_mhz_hi = x < y ? y : x;
	}
}

int perf_read_cpu_khz(const char *path)
{
	char buf[32];
	ssize_t n;
	int fd = open(path ? path : "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	return atoi(buf);
}

int perf_format_log(const struct perf_report *r, char *buf, size_t n)
{
	char cpu[64] = "", mhz[32] = "", gl[160] = "";
	size_t k = 0;

	for (int i = 0; i < r->ncpu && k + 8 < sizeof(cpu); i++)
		k += (size_t)snprintf(cpu + k, sizeof(cpu) - k, "%s%.0f", i ? "/" : "", r->cpu[i]);
	if (r->cpu_mhz_hi > 0 && r->cpu_mhz_lo != r->cpu_mhz_hi)
		snprintf(mhz, sizeof(mhz), " at %d-%d MHz", r->cpu_mhz_lo, r->cpu_mhz_hi);
	else if (r->cpu_mhz_hi > 0)
		snprintf(mhz, sizeof(mhz), " at %d MHz", r->cpu_mhz_hi);
	if (r->gl)
		snprintf(gl, sizeof(gl), ", shaders %llu (%.0f ms), links %llu (%.0f ms), slow draws %llu (%.0f ms), "
			 "tex %llu (%.0f ms)", (unsigned long long)r->gl_compiles, r->gl_compile_ms,
			 (unsigned long long)r->gl_links, r->gl_link_ms, (unsigned long long)r->gl_slow_draws,
			 r->gl_slow_draw_ms, (unsigned long long)r->gl_tex, r->gl_tex_ms);
	return snprintf(n ? buf : NULL, n,
			"%.1f s: speed %.1f %% (%.2f VI/s), %.1f fps (shown %.1f, effective %.1f), "
			"skipped %llu, dropped %llu, dupes %llu, core %.2f ms (max %.2f), "
			"gl swap %.2f ms, present %.2f ms, readback %.2f ms, cpu %s %%%s, process %.0f %%, underruns %u, "
			"stalls %llu (%.0f ms)%s",
			r->secs, r->speed_pct, r->vi_rate, r->fps, r->shown_fps, r->eff_fps,
			(unsigned long long)r->skipped, (unsigned long long)r->dropped, (unsigned long long)r->dupes,
			r->core_ms_avg, r->core_ms_max, r->swap_ms, r->present_ms, r->readback_ms,
			cpu[0] ? cpu : "-", mhz, r->proc_cpu_pct, r->underruns, (unsigned long long)r->stalls,
			r->stall_ms, gl);
}

int perf_format_overlay(const struct perf_report *r, uint64_t skipped_total, char *buf, size_t n)
{
	char cpu[32] = "";
	size_t k = 0;

	for (int i = 0; i < r->ncpu && i < 4 && k + 6 < sizeof(cpu); i++)
		k += (size_t)snprintf(cpu + k, sizeof(cpu) - k, "%s%.0f", i ? "/" : "", r->cpu[i]);
	if (r->swap_ms > 0)
		return snprintf(buf, n, "SPD %.0f%% FPS %.1f SKIP %llu CPU %s GPU %.1f", r->speed_pct, r->fps,
				(unsigned long long)skipped_total, cpu[0] ? cpu : "-", r->swap_ms);
	return snprintf(buf, n, "SPD %.0f%% FPS %.1f SKIP %llu CPU %s", r->speed_pct, r->fps,
			(unsigned long long)skipped_total, cpu[0] ? cpu : "-");
}

static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

double perf_percentile(float *v, size_t n, double p)
{
	double pos;
	size_t i;

	if (!n)
		return 0;
	qsort(v, n, sizeof(*v), cmp_float);
	if (p <= 0)
		return v[0];
	if (p >= 100)
		return v[n - 1];
	pos = p / 100.0 * (double)(n - 1);
	i = (size_t)pos;
	if (i + 1 >= n)
		return v[n - 1];
	return v[i] + (v[i + 1] - v[i]) * (pos - (double)i);
}
