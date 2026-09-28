/*
 * bench_run.c - the benchmark driver (--bench-driver PLAN, docs/host-design.md
 * §18). It replaced the game process by exec (same pid: the UI supervisor
 * keeps waiting for it, its signals and its status pipe still apply), holds
 * no display, no audio and no core, and for each configuration of the plan
 * forks + execs a game process (--bench-step PLAN I) that runs it and writes
 * result-I.ini. After each run it rewrites the report on the SD card, so a
 * crash, a hang or a power cut keeps what was measured. At the end it execs
 * the game again from the start state with --bench-report PLAN.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench.h"
#include "coreinfo.h"
#include "host.h"
#include "hutil.h"

#if defined(__has_include)
#if __has_include("../power/power.h")
#include "../power/power.h"
#define DRV_SIG_POWEROFF RSOS_SIG_POWEROFF
#endif
#endif
#ifndef DRV_SIG_POWEROFF
#define DRV_SIG_POWEROFF SIGUSR2
#endif

static volatile sig_atomic_t stop_sig, poweroff_sig;
/* the run in progress: the sleep / wake requests go to it (review F-M12:
 * their default action killed the driver) */
static volatile pid_t g_step_pid;
static int g_sig_sleep, g_sig_wake;

static void on_sig(int sig)
{
	if (sig == g_sig_sleep || sig == g_sig_wake) {
		pid_t p = g_step_pid;

		if (p > 0)
			kill(p, sig);
	} else if (sig == DRV_SIG_POWEROFF) {
		poweroff_sig = 1;
	} else if (sig != SIGUSR1) {
		stop_sig = 1;
	}
}

/* argv for a child or for the final exec: the original arguments of the
 * game process (host_saved_args) minus the benchmark ones, plus `extra`. */
static int build_args(const char **out, int max, bool for_step, const char *const *extra)
{
	char *const *av;
	int ac = host_saved_args(&av), n = 0;

	if (ac > 0 && !strcmp(av[0], "--run")) {
		out[n++] = "rsos-frontend";
		out[n++] = "--run";
	} else {
		out[n++] = ac > 0 ? av[0] : "rsos-run";
	}
	n += bench_filter_args(ac > 0 ? ac - 1 : 0, ac > 0 ? av + 1 : av, for_step, out + n, max - n - 16);
	for (int i = 0; extra && extra[i] && n < max - 1; i++)
		out[n++] = extra[i];
	out[n] = NULL;
	return n;
}

static void core_path_for(const struct host_config *cfg, const struct bench_plan *p, const char *id,
			  char *out, size_t n)
{
	struct core_info ci;

	if (!strcmp(id, p->core_id) && p->core_path[0]) {
		hstrlcpy(out, p->core_path, n);
		return;
	}
	if (coreinfo_load(&ci, cfg->core_info_dir, id) == 0 && ci.library[0])
		hstrlcpy(out, ci.library, n);
	else
		snprintf(out, n, "/usr/lib/libretro/%s_libretro.so", id);
	coreinfo_free(&ci);
}

static void write_report(const struct bench_plan *p, const struct bench_result *r, int n, bool finished)
{
	size_t size = 32768;
	char *buf = malloc(size);
	int best, order[BENCH_MAX_CONFIGS], len;

	if (!buf)
		return;
	best = finished ? bench_rank(r, n, order) : -1;
	len = bench_format_report(p, r, n, best, finished, buf, size);
	if (len > 0 && hwrite_atomic(p->results, buf, (size_t)len, false) != 0)
		hlog(HLOG_ERROR, "bench: cannot write %s", p->results);
	free(buf);
}

/* A power-off during the benchmark: the game resumes, next time, from the
 * moment the benchmark started. */
static void save_auto_state(const struct bench_plan *p)
{
	char a[BENCH_PATH + 8], b[BENCH_PATH + 8];

	if (!p->auto_state[0] || hcopy_file(p->state, p->auto_state) != 0) {
		hlog(HLOG_ERROR, "bench: could not copy the start state to %s", p->auto_state);
		return;
	}
	hpath(a, sizeof(a), "%s.png", p->state);
	hpath(b, sizeof(b), "%s.png", p->auto_state);
	if (hfile_exists(a))
		hcopy_file(a, b);
	hlog(HLOG_INFO, "bench: start state saved as %s", p->auto_state);
}

int bench_driver_main(const struct host_config *cfg, const char *plan_path)
{
	static struct bench_plan p;
	static struct bench_result res[BENCH_MAX_CONFIGS];
	struct sigaction sa;
	int nres = 0;
	bool aborted = false;

	hlog_set_level((enum hlog_level)cfg->log_level);
	if (bench_plan_load(plan_path, NULL, &p) < 0) {
		hlog(HLOG_ERROR, "bench: cannot read %s", plan_path);
		return HOST_EXIT_FAIL;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sig;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(DRV_SIG_POWEROFF, &sa, NULL);
	host_supervisor_signals(true, &g_sig_sleep, &g_sig_wake, NULL);   /* numbers; still blocked */
	sigaction(g_sig_sleep, &sa, NULL);
	sigaction(g_sig_wake, &sa, NULL);
	/* blocked by the game process across its exec (host.c bench_start):
	 * the handlers exist now */
	host_supervisor_signals(false, NULL, NULL, NULL);
	hlog(HLOG_INFO, "bench driver: %d configurations, report %s", p.n, p.results);
	write_report(&p, res, 0, false);

	for (int i = 0; i < p.n && !stop_sig && !poweroff_sig && !aborted; i++) {
		const struct bench_config *c = &p.c[i];
		struct bench_result *r = &res[nres];
		char core[BENCH_PATH], idx[16], rpath[BENCH_PATH];
		const char *extra[8], *args[160];
		int st = 0;
		pid_t pid, w;
		bool killed = false;

		core_path_for(cfg, &p, c->core, core, sizeof(core));
		snprintf(idx, sizeof(idx), "%d", i);
		extra[0] = "--core";
		extra[1] = core;
		extra[2] = "--bench-step";
		extra[3] = plan_path;
		extra[4] = idx;
		extra[5] = NULL;
		build_args(args, 160, true, extra);
		bench_result_path(plan_path, i, rpath, sizeof(rpath));
		unlink(rpath);
		hlog(HLOG_INFO, "bench driver: run %d/%d \"%s\" (%s) on %s", i + 1, p.n, c->id, c->label, core);
		fflush(stderr);
		pid = fork();
		if (pid < 0) {
			hlog(HLOG_ERROR, "bench driver: fork: %s", strerror(errno));
			break;
		}
		if (pid == 0) {
			sigset_t none;

			prctl(PR_SET_PDEATHSIG, SIGKILL); /* the UI's hang killer kills us */
			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			signal(SIGTERM, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			/* a forwarded sleep before the run's handlers exist waits
			 * (host_run() unblocks them) */
			host_supervisor_signals(true, NULL, NULL, NULL);
			execv("/proc/self/exe", (char *const *)args);
			_exit(127);
		}
		g_step_pid = pid;
		for (;;) {
			w = waitpid(pid, &st, 0);
			if (w == pid)
				break;
			if (w < 0 && errno != EINTR)
				break;
			if ((stop_sig || poweroff_sig) && !killed) {
				kill(pid, SIGTERM);
				killed = true;
			}
		}
		g_step_pid = 0;
		memset(r, 0, sizeof(*r));
		if (bench_result_load(rpath, r) != 0) {
			memset(r, 0, sizeof(*r));
			hstrlcpy(r->id, c->id, sizeof(r->id));
			hstrlcpy(r->label, c->label, sizeof(r->label));
			hstrlcpy(r->core, c->core, sizeof(r->core));
			if (WIFSIGNALED(st)) {
				hstrlcpy(r->status, "crash", sizeof(r->status));
				snprintf(r->note, sizeof(r->note), "killed by signal %d (%s)", WTERMSIG(st),
					 strsignal(WTERMSIG(st)));
			} else if (WIFEXITED(st) && WEXITSTATUS(st) == BENCH_EXIT_ABORT) {
				hstrlcpy(r->status, "abort", sizeof(r->status));
				snprintf(r->note, sizeof(r->note), "stopped with Select+Start");
				aborted = true;
			} else if (WIFEXITED(st) && WEXITSTATUS(st) == HOST_EXIT_HANG) {
				hstrlcpy(r->status, "hang", sizeof(r->status));
				snprintf(r->note, sizeof(r->note), "the emulator stopped responding");
			} else if (stop_sig || poweroff_sig) {
				hstrlcpy(r->status, "abort", sizeof(r->status));
				snprintf(r->note, sizeof(r->note), "stopped");
			} else {
				hstrlcpy(r->status, "error", sizeof(r->status));
				snprintf(r->note, sizeof(r->note), "could not run (exit %d)",
					 WIFEXITED(st) ? WEXITSTATUS(st) : -1);
			}
			/* the results page reads it too */
			{
				char buf[4096];
				int len = bench_result_format(r, buf, sizeof(buf));

				if (len > 0)
					hwrite_atomic(rpath, buf, (size_t)len, false);
			}
		}
		hlog(HLOG_INFO, "bench driver: \"%s\": %s, speed %.1f %%, %.1f fps", r->id, r->status, r->speed,
		     r->fps);
		nres++;
		write_report(&p, res, nres, false);
	}
	write_report(&p, res, nres, true);
	hlog(HLOG_INFO, "bench driver: done (%d runs%s), report %s", nres, aborted ? ", stopped" : "", p.results);

	if (poweroff_sig) {
		save_auto_state(&p);
		if (cfg->status_fd >= 0) {
			static const char line[] = "poweroff \n";

			if (write(cfg->status_fd, line, sizeof(line) - 1) < 0)
				hlog(HLOG_WARN, "status fd: %s", strerror(errno));
		}
		return HOST_EXIT_POWEROFF;
	}
	if (stop_sig)
		return HOST_EXIT_OK;
	{
		const char *extra[6], *args[160];

		extra[0] = "--load-state-file";
		extra[1] = p.state;
		extra[2] = "--bench-report";
		extra[3] = plan_path;
		extra[4] = NULL;
		build_args(args, 160, false, extra);
		fflush(stderr);
		host_supervisor_signals(true, NULL, NULL, NULL);   /* until host_run() */
		execv("/proc/self/exe", (char *const *)args);
		host_supervisor_signals(false, NULL, NULL, NULL);
		hlog(HLOG_ERROR, "bench driver: cannot restart the game: %s", strerror(errno));
	}
	return HOST_EXIT_FAIL;
}
