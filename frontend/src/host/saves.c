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

static int do_job(struct job *j)
{
	int ret = hwrite_atomic(j->path, j->data, j->size, j->kind == JOB_STATE);

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

void sram_load(void)
{
	for (int i = 0; i < 2; i++) {
		struct region *r = &S.reg[i];
		char path[PATH_MAX], tmp[PATH_MAX + 8];
		size_t size;
		void *mem = region_mem(r, &size);
		long got;

		if (!mem)
			continue;
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
			r->written = r->seen = hhash64(mem, size);
			hlog(HLOG_INFO, "%s: no %s file yet (%zu bytes)", S.game, r->ext, size);
			continue;
		}
		got = hread_file_into(path, mem, size);
		if (got < 0) {
			/* Never overwrite a save we could not read. */
			r->disabled = true;
			hlog(HLOG_ERROR, "cannot read %s: %s: saving disabled for this session", path,
			     strerror((int)-got));
			host_toast("%s", _("Save file unreadable: saving disabled"));
			continue;
		}
		if ((long long)got != (long long)size || hfile_size(path) != (long long)size)
			hlog(HLOG_WARN, "%s: file %lld bytes, core %zu bytes", path, hfile_size(path), size);
		r->written = r->seen = hhash64(mem, size);
		hlog(HLOG_INFO, "loaded %s (%ld bytes)", path, got);
	}
}

static int flush_region(struct region *r, bool sync, bool force_now)
{
	size_t size;
	void *mem = region_mem(r, &size);
	uint64_t h;
	struct job j;

	(void)force_now;
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
	if (!S.inited || now - S.last_check < SRAM_CHECK_MS)
		return;
	S.last_check = now;
	for (int i = 0; i < 2; i++) {
		struct region *r = &S.reg[i];
		size_t size;
		void *mem = region_mem(r, &size);
		uint64_t h, written;
		int64_t retry_at;
		int pending;

		if (!mem || r->disabled)
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

static int load_file(const char *path)
{
	size_t size;
	uint8_t *data = hread_file(path, &size);
	bool ok;

	if (!data)
		return -errno ? -errno : -EIO;
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
	return ok ? 0 : -EINVAL;
}

int state_load(int slot)
{
	char path[PATH_MAX], bak[PATH_MAX + 8];

	state_path(slot, path, sizeof(path));
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
	return load_file(path);
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
	return load_file(path);
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
