/*
 * prefetch.c - the asset worker (docs/ui-design.md §4.2).
 *
 * One low-priority thread builds theme assets ahead of the UI thread: it
 * parses a system's theme, reads or composites its carousel backdrop,
 * rasterizes or reads its logos, and prebuilds game list views (their
 * backdrop, the first game's box art and marquee). The UI thread only
 * installs the results (pointer moves) in ui_update(), so no navigation
 * frame decodes, composites, reads a cache file or parses a theme.
 *
 * Jobs (struct pf_job, filled by the views) come in three priorities:
 *   PF_URGENT  something on screen now shows a placeholder (a navigation got
 *              ahead of the worker, a theme switch): runs at once;
 *   PF_NEAR    the window around the carousel cursor after the user moved it
 *              (±2, the direction of travel first): runs at once;
 *   PF_BG      the speculative part: the neighbours of the cursor before any
 *              move, then every other system. Only once the menu is up and
 *              every game list is in (never on the boot path), and only
 *              when no button was pressed for PF_INPUT_QUIET_MS.
 * The thread runs at nice +10 with the lowest best-effort I/O priority; it
 * is paused while a game runs, and quiesced (queue dropped, the running job
 * waited for, its result dropped) before anything it reads is freed: theme
 * or language switch, size change, carousel rebuild, exit.
 *
 * Memory: at most PF_MAX_DONE results wait for the UI thread (the worker
 * stops meanwhile); a job's own buffers are freed by its drop callback. What
 * the views keep is bounded by them: the carousel keeps the backdrops of
 * ±3 systems (7 x 1.2 MB at 640x480) and every system's logos (~100 KB
 * each), the game lists at most GL_PREBUILT prebuilt views (~1.3 MB each);
 * layers composited into a cached backdrop leave the image cache first
 * (img_put_cold), so prefetching every system does not evict the images
 * of its 24 MB budget that are in use.
 *
 * Threads: the worker never touches struct ui; a job's run() only sees its
 * own copies (and themes, read-only, which the UI thread frees only after
 * prefetch_quiesce()). The image, font, theme and log code it calls are
 * thread-safe (image.c, font.c, util.c).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "ui_internal.h"

#define PF_POLL_MS          20    /* ui_timeout while jobs are in flight */
#define PF_INPUT_QUIET_MS   250   /* PF_BG waits this long after a button */
#define PF_MAX_DONE         8     /* results waiting for the UI thread (memory:
				   * at most 8 backdrops, ~10-13 MB, for a moment) */
#define PF_NICE             10

struct prefetch {
	pthread_mutex_t mu;
	pthread_cond_t cv;             /* the worker: a job, room, resume, stop */
	pthread_cond_t idle_cv;        /* quiesce / wait: the running job ended */
	pthread_t th;
	bool started, failed, stop, paused;
	bool no_poll;                  /* the preview's virtual clock: no timeouts */
	bool trace;                    /* RSOS_PREFETCH_TRACE=1: jobs on stderr */
	bool allow_bg;                 /* menu up and every list in */
	int64_t quiet_until_us;        /* PF_BG waits (an input burst), real time */
	struct pf_job *q[PF_NPRIO], **q_tail[PF_NPRIO];
	struct pf_job *running;
	struct pf_job *done, **done_tail;
	int ndone;
	unsigned gen;
	int64_t delay_us;              /* test hook: RSOS_PREFETCH_DELAY_MS per job */
	/* stats (under mu) */
	int jobs_done;
	int64_t work_us;
	int64_t bg_started_ms;         /* ui time the speculative part began */
	bool bg_logged;
};

static int64_t mono_us(void)
{
	return ui_now_us();
}

static void q_push(struct prefetch *P, struct pf_job *j)
{
	j->next = NULL;
	*P->q_tail[j->prio] = j;
	P->q_tail[j->prio] = &j->next;
}

static void q_reset(struct prefetch *P, int p)
{
	P->q[p] = NULL;
	P->q_tail[p] = &P->q[p];
}

/* Under mu: the next job the worker may take now, removed from its queue. */
static struct pf_job *q_pick(struct prefetch *P, int64_t now_us)
{
	for (int p = 0; p < PF_NPRIO; p++) {
		struct pf_job *j = P->q[p];

		if (!j)
			continue;
		if (p == PF_BG && (!P->allow_bg || now_us < P->quiet_until_us))
			continue;
		P->q[p] = j->next;
		if (!P->q[p])
			P->q_tail[p] = &P->q[p];
		j->next = NULL;
		return j;
	}
	return NULL;
}

static void lower_priority(void)
{
	pid_t tid = (pid_t)syscall(SYS_gettid);

	/* nice +10 for this thread only (Linux: per-thread nice) */
	setpriority(PRIO_PROCESS, (id_t)tid, PF_NICE);
#ifdef SYS_ioprio_set
	/* IOPRIO_CLASS_BE (2) << 13 | level 7: the lowest best-effort I/O
	 * priority, so the SD card serves the UI and the loader first */
	syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, (int)tid, (2 << 13) | 7);
#endif
}

static void *worker(void *arg)
{
	struct prefetch *P = arg;

	lower_priority();
	pthread_mutex_lock(&P->mu);
	for (;;) {
		struct pf_job *j = NULL;
		int64_t t0;

		while (!P->stop) {
			int64_t now = mono_us();

			if (!P->paused && P->ndone < PF_MAX_DONE && (j = q_pick(P, now)))
				break;
			if (!P->paused && P->ndone < PF_MAX_DONE && P->q[PF_BG] && P->allow_bg &&
			    now < P->quiet_until_us) {
				/* only PF_BG work left, held back by an input burst */
				struct timespec ts;
				int64_t until = P->quiet_until_us;

				clock_gettime(CLOCK_REALTIME, &ts);
				until = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + (until - now) + 1000;
				ts.tv_sec = (time_t)(until / 1000000);
				ts.tv_nsec = (long)(until % 1000000) * 1000;
				pthread_cond_timedwait(&P->cv, &P->mu, &ts);
			} else {
				pthread_cond_wait(&P->cv, &P->mu);
			}
		}
		if (P->stop)
			break;
		P->running = j;
		pthread_mutex_unlock(&P->mu);
		if (P->trace)
			fprintf(stderr, "prefetch: run %p prio %d\n", (void *)j, j->prio);
		t0 = mono_us();
		if (P->delay_us)
			usleep((useconds_t)P->delay_us);
		if (!pf_cancelled(j))
			j->run(j);
		t0 = mono_us() - t0;
		pthread_mutex_lock(&P->mu);
		P->running = NULL;
		P->jobs_done++;
		P->work_us += t0;
		j->next = NULL;
		*P->done_tail = j;
		P->done_tail = &j->next;
		P->ndone++;
		pthread_cond_broadcast(&P->idle_cv);
	}
	pthread_mutex_unlock(&P->mu);
	img_thread_exit();
	return NULL;
}

void prefetch_init(struct ui *ui)
{
	struct prefetch *P = xcalloc(1, sizeof(*P));
	const char *d = getenv("RSOS_PREFETCH_DELAY_MS");

	pthread_mutex_init(&P->mu, NULL);
	pthread_cond_init(&P->cv, NULL);
	pthread_cond_init(&P->idle_cv, NULL);
	for (int p = 0; p < PF_NPRIO; p++)
		q_reset(P, p);
	P->done_tail = &P->done;
	P->gen = 1;
	if (d && atoi(d) > 0)
		P->delay_us = (int64_t)atoi(d) * 1000;
	/* RSOS_PREFETCH=0: no worker (the old synchronous loading; tests) */
	d = getenv("RSOS_PREFETCH");
	if (d && !strcmp(d, "0"))
		P->failed = true;
	d = getenv("RSOS_PREFETCH_TRACE");
	P->trace = d && *d == '1';
	ui->pf = P;
}

static void drop_list(struct pf_job *j)
{
	while (j) {
		struct pf_job *n = j->next;

		j->drop(j);
		j = n;
	}
}

static bool start(struct ui *ui)
{
	struct prefetch *P = ui->pf;

	if (P->started)
		return true;
	if (P->failed || !ui->first_frame_done)
		return false;
	if (pthread_create(&P->th, NULL, worker, P) != 0) {
		LOGW("ui: cannot start the asset worker: assets load on the UI thread");
		P->failed = true;
		return false;
	}
	P->started = true;
	return true;
}

bool prefetch_available(struct ui *ui)
{
	return ui->pf && !ui->pf->failed && ui->first_frame_done;
}

unsigned prefetch_gen(const struct ui *ui)
{
	return ui->pf ? ui->pf->gen : 0;
}

bool prefetch_submit(struct ui *ui, struct pf_job *j)
{
	struct prefetch *P = ui->pf;

	j->gen = P->gen;
	j->cancelled = false;
	if (!start(ui)) {
		/* no worker (before the first frame, or none could start):
		 * urgent work is done here, as before the worker existed;
		 * speculative work is not done at all, and the callers' marks
		 * of jobs in flight become stale (a new generation) */
		if (j->prio == PF_URGENT) {
			j->run(j);
			j->apply(ui, j);
		} else {
			P->gen++;
		}
		j->drop(j);
		return false;
	}
	pthread_mutex_lock(&P->mu);
	q_push(P, j);
	pthread_cond_broadcast(&P->cv);
	pthread_mutex_unlock(&P->mu);
	if (P->trace)
		fprintf(stderr, "prefetch: queued %p prio %d\n", (void *)j, j->prio);
	return true;
}

/* A queued job becomes more urgent (it is on screen now). j must be a job
 * of this generation that was not applied yet (the UI thread frees jobs,
 * so it is still allocated); running or finished ones are left alone. */
void prefetch_raise(struct ui *ui, struct pf_job *j, int prio)
{
	struct prefetch *P = ui->pf;
	struct pf_job **pp;
	int old;

	if (!P || !P->started)
		return;
	pthread_mutex_lock(&P->mu);
	old = j->prio;
	if (old <= prio || j->gen != P->gen) {
		pthread_mutex_unlock(&P->mu);
		return;
	}
	if (P->running == j) {
		/* already running: it counts as urgent now (prefetch_wait) */
		j->prio = prio;
		pthread_mutex_unlock(&P->mu);
		return;
	}
	pp = &P->q[old];
	while (*pp && *pp != j)
		pp = &(*pp)->next;
	if (*pp) {
		*pp = j->next;
		P->q_tail[old] = &P->q[old];
		for (struct pf_job *k = P->q[old]; k; k = k->next)
			P->q_tail[old] = &k->next;
		j->prio = prio;
		q_push(P, j);
		pthread_cond_broadcast(&P->cv);
		if (P->trace)
			fprintf(stderr, "prefetch: raised %p to %d\n", (void *)j, prio);
	}
	pthread_mutex_unlock(&P->mu);
}

bool prefetch_poll(struct ui *ui)
{
	struct prefetch *P = ui->pf;
	struct pf_job *d, *n;
	bool any = false;

	if (!P || !P->started)
		return false;
	pthread_mutex_lock(&P->mu);
	d = P->done;
	P->done = NULL;
	P->done_tail = &P->done;
	P->ndone = 0;
	if (d)
		pthread_cond_broadcast(&P->cv);   /* room again */
	pthread_mutex_unlock(&P->mu);
	for (; d; d = n) {
		n = d->next;
		if (d->gen == P->gen && !pf_cancelled(d)) {
			d->apply(ui, d);
			any = true;
		}
		d->drop(d);
	}
	if (P->bg_started_ms && !P->bg_logged && !prefetch_busy(ui)) {
		P->bg_logged = true;
		pthread_mutex_lock(&P->mu);
		LOGI("ui: assets prefetched %lld ms after the lists (%d jobs, %lld ms of worker time)",
		     (long long)(ui->now - P->bg_started_ms), P->jobs_done, (long long)P->work_us / 1000);
		pthread_mutex_unlock(&P->mu);
	}
	return any;
}

void prefetch_quiesce(struct ui *ui)
{
	struct prefetch *P = ui->pf;
	struct pf_job *drop = NULL, **tail = &drop;

	if (!P)
		return;
	if (P->trace)
		fprintf(stderr, "prefetch: quiesce\n");
	pthread_mutex_lock(&P->mu);
	P->gen++;
	for (int p = 0; p < PF_NPRIO; p++) {
		if (P->q[p]) {
			*tail = P->q[p];
			tail = P->q_tail[p];
		}
		q_reset(P, p);
	}
	if (P->running)
		__atomic_store_n(&P->running->cancelled, true, __ATOMIC_RELAXED);
	while (P->running)
		pthread_cond_wait(&P->idle_cv, &P->mu);
	*tail = P->done;
	P->done = NULL;
	P->done_tail = &P->done;
	P->ndone = 0;
	pthread_cond_broadcast(&P->cv);
	pthread_mutex_unlock(&P->mu);
	drop_list(drop);
}

void prefetch_pause(struct ui *ui, bool pause)
{
	struct prefetch *P = ui->pf;

	if (!P)
		return;
	pthread_mutex_lock(&P->mu);
	P->paused = pause;
	pthread_cond_broadcast(&P->cv);
	pthread_mutex_unlock(&P->mu);
}

void prefetch_input(struct ui *ui)
{
	struct prefetch *P = ui->pf;

	if (!P || !P->started)
		return;
	pthread_mutex_lock(&P->mu);
	P->quiet_until_us = mono_us() + PF_INPUT_QUIET_MS * 1000;
	pthread_mutex_unlock(&P->mu);
}

void prefetch_allow_background(struct ui *ui)
{
	struct prefetch *P = ui->pf;

	if (!P || P->allow_bg)
		return;
	pthread_mutex_lock(&P->mu);
	P->allow_bg = true;
	P->bg_started_ms = ui->now ? ui->now : 1;
	pthread_cond_broadcast(&P->cv);
	pthread_mutex_unlock(&P->mu);
}

bool prefetch_background_allowed(const struct ui *ui)
{
	return ui->pf && ui->pf->allow_bg;
}

/* Jobs queued, running or waiting for the UI thread (at or above prio). */
static bool busy_at(const struct ui *ui, int prio)
{
	struct prefetch *P = ui->pf;
	bool b;

	if (!P || !P->started)
		return false;
	pthread_mutex_lock(&P->mu);
	b = P->done || (P->running && P->running->prio <= prio);
	for (int p = 0; p <= prio && p < PF_NPRIO && !b; p++)
		b = P->q[p] != NULL;
	pthread_mutex_unlock(&P->mu);
	return b;
}

bool prefetch_busy(const struct ui *ui)
{
	return busy_at(ui, PF_BG);
}

bool prefetch_urgent_busy(const struct ui *ui)
{
	return busy_at(ui, PF_URGENT);
}

int prefetch_timeout(const struct ui *ui)
{
	struct prefetch *P = ui->pf;
	bool b;

	if (!P || !P->started || P->no_poll)
		return -1;
	pthread_mutex_lock(&P->mu);
	/* results to install, or work that will produce some; PF_BG held
	 * back (no lists yet, input burst) does not need polling */
	b = P->done || P->running || P->q[PF_URGENT] || P->q[PF_NEAR] ||
	    (P->q[PF_BG] && P->allow_bg && !P->paused);
	pthread_mutex_unlock(&P->mu);
	return b && !P->paused ? PF_POLL_MS : -1;
}

/* Tests: blocks (real time) until nothing at or above prio is in flight,
 * installing results; false after max_ms. */
bool prefetch_wait(struct ui *ui, int prio, int max_ms)
{
	int64_t end = mono_us() + (int64_t)max_ms * 1000;

	for (;;) {
		prefetch_poll(ui);
		if (!busy_at(ui, prio))
			return true;
		if (mono_us() >= end)
			return false;
		usleep(1000);
	}
}

void prefetch_set_polling(struct ui *ui, bool on)
{
	if (ui->pf)
		ui->pf->no_poll = !on;
}

void prefetch_stats(const struct ui *ui, int *jobs, int64_t *work_us, int *queued)
{
	struct prefetch *P = ui->pf;
	int n = 0;

	*jobs = 0;
	*work_us = 0;
	if (queued)
		*queued = 0;
	if (!P)
		return;
	pthread_mutex_lock(&P->mu);
	*jobs = P->jobs_done;
	*work_us = P->work_us;
	for (int p = 0; p < PF_NPRIO; p++)
		for (struct pf_job *j = P->q[p]; j; j = j->next)
			n++;
	if (queued)
		*queued = n + (P->running ? 1 : 0);
	pthread_mutex_unlock(&P->mu);
}

void prefetch_destroy(struct ui *ui)
{
	struct prefetch *P = ui->pf;

	if (!P)
		return;
	prefetch_quiesce(ui);
	if (P->started) {
		pthread_mutex_lock(&P->mu);
		P->stop = true;
		pthread_cond_broadcast(&P->cv);
		pthread_mutex_unlock(&P->mu);
		pthread_join(P->th, NULL);
	}
	pthread_mutex_destroy(&P->mu);
	pthread_cond_destroy(&P->cv);
	pthread_cond_destroy(&P->idle_cv);
	free(P);
	ui->pf = NULL;
}
