/*
 * saves.c - see saves.h.
 */
#include "saves.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "host_internal.h"
#include "host_png.h"
#include "hutil.h"
#include "../i18n/i18n.h"
#include "../../third_party/miniz/miniz.h"

#define SRAM_CHECK_MS 1000
#define SRAM_STABLE_MS 1000
#define SRAM_MAX_DIRTY_MS 30000
#define QMAX 8

enum job_kind { JOB_SRAM, JOB_STATE };

struct job {
	enum job_kind kind;
	char path[PATH_MAX];
	void *data;
	size_t size;
	uint8_t *thumb;
	int tw, th;
	int slot;
	int mem;           /* RETRO_MEMORY_* for JOB_SRAM */
	uint64_t hash;
	bool bak;          /* JOB_SRAM: keep the file on the card as <file>.bak first */
};

struct msg {
	char text[192];   /* translated (UTF-8) */
	bool err;
};

/* One tracked memory region (.srm, .rtc). */
struct region {
	unsigned id;
	const char *ext;
	uint64_t written;   /* hash of what is on disk (or loaded) */
	uint64_t seen;
	int64_t seen_at;
	int64_t dirty_since;
	bool disabled;      /* the file exists but could not be read: never overwrite */
	/* The file was loaded into the core (or there was none). A region the
	 * core did not expose at sram_load() is loaded when it first appears,
	 * and never written before (review: fresh memory over a save never
	 * loaded). */
	bool loaded;
	/* A state was loaded: the next write keeps the file as <file>.bak (the
	 * state's copy of the save may be older than the file). */
	bool bak_next;
	int pending;        /* writes queued */
	/* failed writes (full or read-only card): back off, tell the user
	 * once per error (review F-M11: every second, forever) */
	int failures;
	int last_err;
	int64_t retry_at;   /* hnow_ms() */
};

static struct {
	bool inited;
	char save_dir[PATH_MAX], state_dir[PATH_MAX], game[256];
	pthread_t thr;
	pthread_mutex_t mu;
	pthread_cond_t cv, idle_cv;
	struct job q[QMAX];
	int qhead, qn;
	bool busy, stop;
	struct msg msgs[16];
	int mhead, mn;
	struct region reg[2];
	int64_t last_check;
	bool readonly;      /* benchmark runs: never write SRAM or states */
	char cur_path[PATH_MAX];   /* the job being written (S.busy) */
} S = {
	.mu = PTHREAD_MUTEX_INITIALIZER,
	.cv = PTHREAD_COND_INITIALIZER,
	.idle_cv = PTHREAD_COND_INITIALIZER,
};

static void post_msg(bool err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void post_msg(bool err, const char *fmt, ...)
{
	va_list ap;
	struct msg *m;

	/* Called with S.mu held. */
	if (S.mn == 16) {
		S.mhead = (S.mhead + 1) % 16;
		S.mn--;
	}
	m = &S.msgs[(S.mhead + S.mn++) % 16];
	va_start(ap, fmt);
	vsnprintf(m->text, sizeof(m->text), fmt, ap);
	va_end(ap);
	hutf8_trim(m->text);
	m->err = err;
}

static void saves_ref_write(const char *state, uint64_t state_hash, size_t state_size);

static int do_job(struct job *j)
{
	char ref[PATH_MAX + 8];
	bool auto_ref = j->kind == JOB_STATE && j->slot == STATE_SLOT_AUTO &&
			hpath(ref, sizeof(ref), "%s%s", j->path, STATE_SAVES_REF_EXT);
	int ret;

	/* The auto state's saves reference: gone while the state is replaced
	 * (a power cut in between leaves no reference, never a wrong one). */
	if (auto_ref)
		unlink(ref);
	ret = hwrite_atomic(j->path, j->data, j->size, j->kind == JOB_STATE || j->bak);
	if (!ret && auto_ref)
		saves_ref_write(j->path, hhash64(j->data, j->size), j->size);
	if (!ret && j->kind == JOB_STATE && j->thumb) {
		size_t psize;
		void *png = host_png_encode(j->thumb, j->tw, j->th, &psize);
		char tpath[PATH_MAX + 8];

		if (png && hpath(tpath, sizeof(tpath), "%s.png", j->path))
			hwrite_atomic(tpath, png, psize, false);
		free(png);
	}
	return ret;
}

static void finish_job(struct job *j, int ret)
{
	/* S.mu held. */
	if (j->kind == JOB_SRAM) {
		struct region *r = &S.reg[j->mem == RETRO_MEMORY_RTC ? 1 : 0];

		r->pending--;
		if (ret) {
			/* retry later: 2 s, 4 s ... up to 5 min; one message and one
			 * log line per kind of error, not one per second */
			int shift = r->failures < 8 ? r->failures : 8;
			int64_t wait = (int64_t)SRAM_CHECK_MS * 2 << shift;

			r->written = 0;
			r->failures++;
			r->retry_at = hnow_ms() + (wait > 300000 ? 300000 : wait);
			if (ret != r->last_err) {
				/* TRANSLATORS: toast (one line at the bottom of the game
				 * picture); %s is the system's English error text */
				post_msg(true, _("Game save failed: %s (retrying)"), strerror(-ret));
				hlog(HLOG_ERROR, "saved %s (%zu bytes): %s (retrying with back-off)", j->path,
				     j->size, strerror(-ret));
			}
			r->last_err = ret;
		} else {
			if (!r->pending)
				r->written = j->hash;
			if (r->failures)
				/* TRANSLATORS: toast after a failed game save finally worked */
				post_msg(false, "%s", _("Game saved"));
			r->failures = 0;
			r->last_err = 0;
			r->retry_at = 0;
			hlog(HLOG_INFO, "saved %s (%zu bytes)", j->path, j->size);
		}
	} else {
		/* toasts (the in-game menu status line too) */
		if (j->slot == STATE_SLOT_AUTO && ret)
			post_msg(true, _("Auto state NOT saved: %s"), strerror(-ret));
		else if (j->slot == STATE_SLOT_AUTO)
			post_msg(false, "%s", _("Auto state saved"));
		else if (ret)
			/* TRANSLATORS: toast; %s is the system's English error text */
			post_msg(true, _("State %d NOT saved: %s"), j->slot, strerror(-ret));
		else
			post_msg(false, _("State saved, slot %d"), j->slot);
		hlog(ret ? HLOG_ERROR : HLOG_INFO, "state %s (%zu bytes)%s%s", j->path, j->size,
		     ret ? ": " : "", ret ? strerror(-ret) : "");
	}
	free(j->data);
	free(j->thumb);
	j->data = NULL;
	j->thumb = NULL;
}

static void *worker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&S.mu);
	for (;;) {
		struct job j;
		int ret;

		while (!S.qn && !S.stop)
			pthread_cond_wait(&S.cv, &S.mu);
		if (!S.qn && S.stop)
			break;
		j = S.q[S.qhead];
		S.qhead = (S.qhead + 1) % QMAX;
		S.qn--;
		S.busy = true;
		hstrlcpy(S.cur_path, j.path, sizeof(S.cur_path));
		pthread_mutex_unlock(&S.mu);
		ret = do_job(&j);
		pthread_mutex_lock(&S.mu);
		finish_job(&j, ret);
		S.busy = false;
		S.cur_path[0] = 0;
		pthread_cond_broadcast(&S.idle_cv);
	}
	pthread_mutex_unlock(&S.mu);
	return NULL;
}

static void wait_idle(void)
{
	pthread_mutex_lock(&S.mu);
	while (S.qn || S.busy)
		pthread_cond_wait(&S.idle_cv, &S.mu);
	pthread_mutex_unlock(&S.mu);
}

/* Queues a job, or runs it here if sync or the queue is full. */
static int submit(struct job *j, bool sync)
{
	if (!sync && S.inited) {
		pthread_mutex_lock(&S.mu);
		if (S.qn < QMAX) {
			S.q[(S.qhead + S.qn++) % QMAX] = *j;
			pthread_cond_signal(&S.cv);
			pthread_mutex_unlock(&S.mu);
			return 0;
		}
		pthread_mutex_unlock(&S.mu);
	}
	if (S.inited)
		wait_idle(); /* keep the write order */
	{
		int ret = do_job(j);

		pthread_mutex_lock(&S.mu);
		finish_job(j, ret);
		pthread_mutex_unlock(&S.mu);
		return ret;
	}
}

int saves_init(const char *save_dir, const char *state_dir, const char *game)
{
	hstrlcpy(S.save_dir, save_dir, sizeof(S.save_dir));
	hstrlcpy(S.state_dir, state_dir, sizeof(S.state_dir));
	hstrlcpy(S.game, game, sizeof(S.game));
	hmkdir_p(save_dir, 0755);
	hmkdir_p(state_dir, 0755);
	memset(S.reg, 0, sizeof(S.reg));
	S.reg[0].id = RETRO_MEMORY_SAVE_RAM;
	S.reg[0].ext = "srm";
	S.reg[1].id = RETRO_MEMORY_RTC;
	S.reg[1].ext = "rtc";
	S.stop = false;
	if (pthread_create(&S.thr, NULL, worker, NULL) != 0)
		return -errno;
	S.inited = true;
	return 0;
}

void saves_shutdown(void)
{
	if (!S.inited)
		return;
	pthread_mutex_lock(&S.mu);
	S.stop = true;
	pthread_cond_signal(&S.cv);
	pthread_mutex_unlock(&S.mu);
	pthread_join(S.thr, NULL);
	S.inited = false;
}

/* ----------------------------------------------------------------- SRAM */

static void region_path(const struct region *r, char *out, size_t n)
{
	hpath(out, n, "%s/%s.%s", S.save_dir, S.game, r->ext);
}

static void *region_mem(const struct region *r, size_t *size)
{
	void *p;

	if (!H.core.get_memory_data || !H.core.get_memory_size)
		return NULL;
	*size = H.core.get_memory_size(r->id);
	p = *size ? H.core.get_memory_data(r->id) : NULL;
	return p;
}

/* Loads one region's file into the core's memory, once, as soon as the
 * core exposes that memory (at sram_load(), or later: a core may expose it
 * only after its first frame). Until then the region is never written. */
static void region_load(struct region *r, bool late)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	size_t size;
	void *mem = region_mem(r, &size);
	uint64_t h;
	long got;

	if (r->loaded || !mem)
		return;
	r->loaded = true;
	region_path(r, path, sizeof(path));
	hpath(tmp, sizeof(tmp), "%s.tmp", path);
	if (!hfile_exists(path) && hfile_exists(tmp) && hfile_size(tmp) == (long long)size) {
		/* Power cut between fsync and rename: the temp file is complete. */
		hlog(HLOG_WARN, "recovering %s from %s", path, tmp);
		rename(tmp, path);
	}
	if (!hfile_exists(path)) {
		/* Power cut between the two renames of a replacement that kept
		 * a backup (web/USB upload of a save on FAT/exFAT, F-M8). */
		char bak[PATH_MAX + 8];

		if (hpath(bak, sizeof(bak), "%s.bak", path) && hfile_exists(bak)) {
			hlog(HLOG_WARN, "recovering %s from %s", path, bak);
			rename(bak, path);
		}
	}
	if (!hfile_exists(path)) {
		h = hhash64(mem, size);
		pthread_mutex_lock(&S.mu);
		r->written = r->seen = h;
		pthread_mutex_unlock(&S.mu);
		hlog(HLOG_INFO, "%s: no %s file yet (%zu bytes%s)", S.game, r->ext, size,
		     late ? ", memory exposed after the start" : "");
		return;
	}
	got = hread_file_into(path, mem, size);
	if (got < 0) {
		/* Never overwrite a save we could not read. */
		r->disabled = true;
		hlog(HLOG_ERROR, "cannot read %s: %s: saving disabled for this session", path,
		     strerror((int)-got));
		host_toast("%s", _("Save file unreadable: saving disabled"));
		return;
	}
	if ((long long)got != (long long)size || hfile_size(path) != (long long)size)
		hlog(HLOG_WARN, "%s: file %lld bytes, core %zu bytes", path, hfile_size(path), size);
	h = hhash64(mem, size);
	pthread_mutex_lock(&S.mu);
	r->written = r->seen = h;
	pthread_mutex_unlock(&S.mu);
	hlog(HLOG_INFO, "loaded %s (%ld bytes)%s", path, got,
	     late ? " when the core exposed the memory, after the start" : "");
}

void sram_load(void)
{
	for (int i = 0; i < 2; i++)
		region_load(&S.reg[i], false);
}

static int flush_region(struct region *r, bool sync, bool force_now)
{
	size_t size;
	void *mem = region_mem(r, &size);
	uint64_t h;
	struct job j;

	(void)force_now;
	if (!r->loaded) {
		/* never written before its file was loaded (it may appear now) */
		region_load(r, true);
		return 0;
	}
	if (!mem || r->disabled || S.readonly)
		return 0;
	h = hhash64(mem, size);
	pthread_mutex_lock(&S.mu);
	if (h == r->written) {
		pthread_mutex_unlock(&S.mu);
		return 0;
	}
	/* If an older copy is still queued, this one follows it (FIFO). */
	r->pending++;
	r->written = h; /* optimistic: a failure resets it (finish_job) */
	pthread_mutex_unlock(&S.mu);
	memset(&j, 0, sizeof(j));
	j.kind = JOB_SRAM;
	j.mem = (int)r->id;
	j.hash = h;
	/* the first write after a state load keeps the file as .bak */
	j.bak = r->bak_next;
	if (j.bak)
		hlog(HLOG_INFO, "%s.%s: first write since a state was loaded: the file is kept as .bak", S.game,
		     r->ext);
	r->bak_next = false;
	region_path(r, j.path, sizeof(j.path));
	j.data = malloc(size);
	if (!j.data)
		return -ENOMEM;
	memcpy(j.data, mem, size);
	j.size = size;
	r->dirty_since = 0;
	return submit(&j, sync);
}

void sram_tick(int64_t now)
{
	if (!S.inited)
		return;
	/* a region missing at load: loaded on the first frame it exists */
	for (int i = 0; i < 2; i++)
		if (!S.reg[i].loaded)
			region_load(&S.reg[i], true);
	if (now - S.last_check < SRAM_CHECK_MS)
		return;
	S.last_check = now;
	for (int i = 0; i < 2; i++) {
		struct region *r = &S.reg[i];
		size_t size;
		void *mem = region_mem(r, &size);
		uint64_t h, written;
		int64_t retry_at;
		int pending;

		if (!mem || r->disabled || !r->loaded)
			continue;
		h = hhash64(mem, size);
		pthread_mutex_lock(&S.mu);
		pending = r->pending;
		written = r->written;
		retry_at = r->retry_at;
		pthread_mutex_unlock(&S.mu);
		if (pending || (retry_at && hnow_ms() < retry_at))
			continue;
		if (h == written) {
			r->dirty_since = 0;
			r->seen = h;
			continue;
		}
		if (!r->dirty_since)
			r->dirty_since = now;
		if (h != r->seen) {
			r->seen = h;
			r->seen_at = now;
			if (now - r->dirty_since < SRAM_MAX_DIRTY_MS)
				continue;
		} else if (now - r->seen_at < SRAM_STABLE_MS && now - r->dirty_since < SRAM_MAX_DIRTY_MS) {
			continue;
		}
		/* Changed and stable for a second (or dirty for 30 s): write. */
		flush_region(r, false, false);
	}
}

int sram_flush(bool sync)
{
	int ret = 0;

	for (int i = 0; i < 2; i++) {
		int r = flush_region(&S.reg[i], sync, true);

		if (r && !ret)
			ret = r;
	}
	if (sync && S.inited)
		wait_idle();
	return ret;
}

/* ------------------------------------------- the auto state's saves reference */
/*
 * <state>.sram, next to the auto state (docs/host-design.md, "Resume and
 * battery saves"): what the .srm and .rtc on the card were when the state
 * was written, and the state itself (so a reference left next to another
 * state - a restored backup, a .bak - is recognised as not its own):
 *
 *   state <hash> <bytes>
 *   srm <hash> <bytes>      or   srm none
 *   rtc <hash> <bytes>      or   rtc none
 *
 * The state holds the core's copy of the battery save. If the file on the
 * card changed after the state was written (an in-game save after "Start
 * fresh", a session that crashed or was killed, auto-save on exit off, a
 * failed state write), resuming must not turn the card's newer save back
 * into the state's older copy.
 */
struct ref_entry {
	bool known;       /* the line is there */
	bool present;     /* the file existed */
	uint64_t hash;
	long long size;
};

static bool file_hash(const char *path, uint64_t *hash, long long *size)
{
	size_t n;
	void *data = hread_file(path, &n);

	if (!data)
		return false;
	*hash = hhash64(data, n);
	*size = (long long)n;
	free(data);
	return true;
}

static void saves_ref_write(const char *state, uint64_t state_hash, size_t state_size)
{
	char ref[PATH_MAX + 8], buf[512];
	int len;

	if (!hpath(ref, sizeof(ref), "%s%s", state, STATE_SAVES_REF_EXT))
		return;
	len = snprintf(buf, sizeof(buf),
		       "# RetroStoneOS: the battery saves on the card when this state was written\n"
		       "state %016llx %lld\n", (unsigned long long)state_hash, (long long)state_size);
	for (int i = 0; i < 2 && len > 0 && len < (int)sizeof(buf); i++) {
		char path[PATH_MAX];
		uint64_t h;
		long long sz;

		region_path(&S.reg[i], path, sizeof(path));
		if (file_hash(path, &h, &sz))
			len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s %016llx %lld\n", S.reg[i].ext,
					(unsigned long long)h, sz);
		else
			len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s none\n", S.reg[i].ext);
	}
	if (len <= 0 || len >= (int)sizeof(buf) || hwrite_atomic(ref, buf, (size_t)len, false) != 0)
		hlog(HLOG_WARN, "cannot write %s: the next resume compares the files' dates instead", ref);
}

int state_write_saves_ref(const char *state)
{
	uint64_t h;
	long long size;

	if (!file_hash(state, &h, &size))
		return -errno ? -errno : -EIO;
	saves_ref_write(state, h, (size_t)size);
	return 0;
}

/* Reads <state>.sram; true if it belongs to this state (hash and size). */
static bool ref_read(const char *state, uint64_t state_hash, size_t state_size, struct ref_entry out[2])
{
	char ref[PATH_MAX + 8], *txt, *line, *save = NULL;
	bool mine = false;

	memset(out, 0, 2 * sizeof(*out));
	if (!hpath(ref, sizeof(ref), "%s%s", state, STATE_SAVES_REF_EXT) || !(txt = hread_file(ref, NULL)))
		return false;
	for (line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char key[16];
		unsigned long long h;
		long long sz;
		int n = sscanf(line, "%15s %llx %lld", key, &h, &sz);

		if (n < 2 && !(sscanf(line, "%15s", key) == 1 && strstr(line, " none")))
			continue;
		if (!strcmp(key, "state")) {
			mine = n == 3 && h == state_hash && sz == (long long)state_size;
			continue;
		}
		for (int i = 0; i < 2; i++) {
			if (strcmp(key, S.reg[i].ext))
				continue;
			out[i].known = true;
			out[i].present = n == 3;
			out[i].hash = n == 3 ? h : 0;
			out[i].size = n == 3 ? sz : 0;
		}
	}
	free(txt);
	return mine;
}

/*
 * Before the auto state is loaded: which regions' files changed on the card
 * since the state was written (bit i = S.reg[i]). Each one is kept as
 * <file>.bak now, before any SRAM write. `state` is the file being loaded
 * (the auto state or its .bak), `ref_state` the auto state's path.
 */
static unsigned saves_changed_since(const char *state, const char *ref_state, uint64_t state_hash,
				    size_t state_size)
{
	struct ref_entry e[2];
	bool have_ref = ref_read(ref_state, state_hash, state_size, e);
	struct stat sst;
	unsigned mask = 0;

	if (stat(state, &sst) < 0)
		memset(&sst, 0, sizeof(sst));
	for (int i = 0; i < 2; i++) {
		struct region *r = &S.reg[i];
		char path[PATH_MAX], bak[PATH_MAX + 8];
		struct stat st;
		uint64_t h = 0;
		long long sz = 0;
		bool changed;

		if (r->disabled)
			continue;
		region_path(r, path, sizeof(path));
		if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
			continue;                  /* no file on the card: nothing to protect */
		if (have_ref && e[i].known)
			changed = !e[i].present || !file_hash(path, &h, &sz) || h != e[i].hash || sz != e[i].size;
		else
			/* no reference (a state from before it existed, copied, or
			 * from RetroArch): the dates. The save is flushed before the
			 * state is written, so a newer file was written after it. */
			changed = st.st_mtim.tv_sec > sst.st_mtim.tv_sec ||
				  (st.st_mtim.tv_sec == sst.st_mtim.tv_sec && st.st_mtim.tv_nsec > sst.st_mtim.tv_nsec);
		if (!changed)
			continue;
		mask |= 1u << i;
		if (hpath(bak, sizeof(bak), "%s.bak", path) && hcopy_file(path, bak) == 0)
			hlog(HLOG_WARN, "resume: %s changed after the auto state was written (%s): kept as %s", path,
			     have_ref && e[i].known ? "saves reference" : "newer than the state", bak);
		else
			hlog(HLOG_ERROR, "resume: %s changed after the auto state was written, and no backup could "
			     "be made", path);
	}
	return mask;
}

/* After the auto state was loaded: the newer files go back into the core
 * (the state's copy of the save is older). */
static void saves_reapply(unsigned mask)
{
	for (int i = 0; i < 2; i++) {
		struct region *r = &S.reg[i];
		char path[PATH_MAX];
		size_t size;
		void *mem;
		long got;
		uint64_t h;

		if (!(mask & (1u << i)))
			continue;
		region_path(r, path, sizeof(path));
		mem = region_mem(r, &size);
		if (!mem) {
			/* loaded when the core exposes it (region_load) */
			if (r->loaded)
				hlog(HLOG_WARN, "resume: the core gives no %s memory: %s stays as it is on the card",
				     r->ext, path);
			continue;
		}
		got = hread_file_into(path, mem, size);
		if (got < 0) {
			hlog(HLOG_ERROR, "resume: cannot read %s again: %s (the backup is %s.bak)", path,
			     strerror((int)-got), path);
			continue;
		}
		h = hhash64(mem, size);
		pthread_mutex_lock(&S.mu);
		r->written = r->seen = h;
		pthread_mutex_unlock(&S.mu);
		r->loaded = true;
		hlog(HLOG_WARN, "resume: %s is newer than the auto state: given back to the core after the state "
		     "(%ld bytes)", path, got);
	}
}

/* --------------------------------------------------------------- states */

void state_path(int slot, char *out, size_t n)
{
	if (slot == STATE_SLOT_AUTO)
		hpath(out, n, "%s/%s.state.auto", S.state_dir, S.game);
	else if (slot == 0)
		hpath(out, n, "%s/%s.state", S.state_dir, S.game);
	else
		hpath(out, n, "%s/%s.state%d", S.state_dir, S.game, slot);
}

/* A state for this path is queued or being written (review F-M10: the
 * menu said "slot empty" for a first save still in the queue). */
static bool state_queued(const char *path)
{
	bool q = false;

	if (!S.inited)
		return false;
	pthread_mutex_lock(&S.mu);
	q = S.busy && !strcmp(S.cur_path, path);
	for (int i = 0; i < S.qn && !q; i++)
		q = !strcmp(S.q[(S.qhead + i) % QMAX].path, path);
	pthread_mutex_unlock(&S.mu);
	return q;
}

bool state_exists(int slot, time_t *mtime)
{
	char path[PATH_MAX], bak[PATH_MAX + 8];
	struct stat st;

	state_path(slot, path, sizeof(path));
	if (state_queued(path)) {
		if (mtime)
			*mtime = time(NULL);
		return true;
	}
	/* a power cut between the two renames of an overwrite (FAT) leaves
	 * only the .bak: state_load() uses it */
	if ((stat(path, &st) < 0 || !S_ISREG(st.st_mode)) &&
	    (!hpath(bak, sizeof(bak), "%s.bak", path) || stat(bak, &st) < 0 || !S_ISREG(st.st_mode)))
		return false;
	if (mtime)
		*mtime = st.st_mtime;
	return true;
}

static void *serialize(size_t *size)
{
	void *buf;

	if (!H.core.serialize_size || !H.core.serialize)
		return NULL;
	if ((H.quirks & RETRO_SERIALIZATION_QUIRK_INCOMPLETE) && H.frame == 0)
		return NULL;
	*size = H.core.serialize_size();
	if (!*size)
		return NULL;
	buf = malloc(*size);
	if (!buf)
		return NULL;
	if (!H.core.serialize(buf, *size)) {
		free(buf);
		return NULL;
	}
	return buf;
}

int state_save(int slot, const uint8_t *thumb, int tw, int th, bool sync)
{
	struct job j;

	if (S.readonly)
		return -EPERM;
	memset(&j, 0, sizeof(j));
	j.kind = JOB_STATE;
	j.slot = slot;
	state_path(slot, j.path, sizeof(j.path));
	j.data = serialize(&j.size);
	if (!j.data) {
		hlog(HLOG_WARN, "the core cannot save a state now");
		return -ENOTSUP;
	}
	if (thumb && tw > 0 && th > 0) {
		j.thumb = malloc((size_t)tw * (size_t)th * 3);
		if (j.thumb) {
			memcpy(j.thumb, thumb, (size_t)tw * (size_t)th * 3);
			j.tw = tw;
			j.th = th;
		}
	}
	return submit(&j, sync);
}

/* RetroArch "#RZIPv1#" compressed states (migration). */
static void *rzip_decode(const uint8_t *in, size_t n, size_t *out_size)
{
	uint64_t total;
	uint8_t *out;
	size_t pos = 20, done = 0;

	if (n < 20 || memcmp(in, "#RZIPv", 6) != 0)
		return NULL;
	total = 0;
	for (int i = 0; i < 8; i++)
		total |= (uint64_t)in[12 + i] << (8 * i);
	if (!total || total > 256u * 1024 * 1024)
		return NULL;
	out = malloc((size_t)total);
	if (!out)
		return NULL;
	while (pos + 4 <= n && done < total) {
		uint32_t csize = (uint32_t)in[pos] | (uint32_t)in[pos + 1] << 8 |
				 (uint32_t)in[pos + 2] << 16 | (uint32_t)in[pos + 3] << 24;
		mz_ulong dlen = (mz_ulong)(total - done);

		pos += 4;
		/* csize > n - pos, never pos + csize > n: that wraps on 32-bit
		 * (review F-M6: a crafted .state read out of bounds) */
		if (csize > n - pos || mz_uncompress(out + done, &dlen, in + pos, csize) != MZ_OK) {
			free(out);
			return NULL;
		}
		done += dlen;
		pos += csize;
	}
	if (done != total) {
		free(out);
		return NULL;
	}
	*out_size = (size_t)total;
	return out;
}

/* ref_state: the auto state's path when resuming from it (path is that file
 * or its .bak): a newer battery save on the card wins over the state's copy
 * (docs/host-design.md, "Resume and battery saves"). */
static int load_file(const char *path, const char *ref_state)
{
	size_t size;
	uint8_t *data = hread_file(path, &size);
	unsigned changed = 0;
	bool ok;

	if (!data)
		return -errno ? -errno : -EIO;
	if (ref_state)
		changed = saves_changed_since(path, ref_state, hhash64(data, size), size);
	if (size >= 8 && !memcmp(data, "#RZIPv", 6)) {
		size_t dsize;
		void *d = rzip_decode(data, size, &dsize);

		free(data);
		if (!d) {
			hlog(HLOG_ERROR, "%s: bad RetroArch compressed state", path);
			return -EINVAL;
		}
		hlog(HLOG_INFO, "%s: RetroArch compressed state, %zu bytes", path, dsize);
		data = d;
		size = dsize;
	}
	ok = H.core.unserialize && H.core.unserialize(data, size);
	free(data);
	hlog(ok ? HLOG_INFO : HLOG_ERROR, "load state %s (%zu bytes): %s", path, size, ok ? "ok" : "refused by the core");
	if (ok) {
		/* the core's save memory may now be the state's (older) copy:
		 * the next write of each file keeps it as .bak first */
		for (int i = 0; i < 2; i++)
			S.reg[i].bak_next = true;
		saves_reapply(changed);
	}
	return ok ? 0 : -EINVAL;
}

int state_load(int slot)
{
	char path[PATH_MAX], bak[PATH_MAX + 8], auto_path[PATH_MAX];

	state_path(slot, path, sizeof(path));
	hstrlcpy(auto_path, path, sizeof(auto_path));
	/* a save of this slot may still be queued: load what was saved last
	 * (review F-M10), not the previous file or its .bak */
	if (S.inited)
		wait_idle();
	if (!hfile_exists(path)) {
		hpath(bak, sizeof(bak), "%s.bak", path);
		if (!hfile_exists(bak))
			return -ENOENT;
		/* Power cut between the two renames of an overwrite. */
		hstrlcpy(path, bak, sizeof(path));
	}
	return load_file(path, slot == STATE_SLOT_AUTO ? auto_path : NULL);
}

int state_save_to(const char *path)
{
	size_t size;
	void *buf = serialize(&size);
	int ret;

	if (!buf)
		return -ENOTSUP;
	ret = hwrite_atomic(path, buf, size, false);
	free(buf);
	return ret;
}

int state_load_from(const char *path)
{
	return load_file(path, NULL);
}

bool saves_next_message(char *buf, size_t n, bool *is_error)
{
	bool got = false;

	pthread_mutex_lock(&S.mu);
	if (S.mn) {
		hstrlcpy(buf, S.msgs[S.mhead].text, n);
		if (is_error)
			*is_error = S.msgs[S.mhead].err;
		S.mhead = (S.mhead + 1) % 16;
		S.mn--;
		got = true;
	}
	pthread_mutex_unlock(&S.mu);
	return got;
}

void saves_set_readonly(bool ro)
{
	S.readonly = ro;
}
