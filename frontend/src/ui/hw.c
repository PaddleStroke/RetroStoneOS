/*
 * hw.c - see hw.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hw.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

/* -------------------------------------------------------------- battery */
static bool read_line(const char *dir, const char *name, const char *file,
		      char *out, size_t n)
{
	char p[1024];
	char *s;

	snprintf(p, sizeof(p), "%s/%s/%s", dir, name, file);
	s = file_read(p, NULL);
	if (!s)
		return false;
	strlcpy_(out, str_trim(s), n);
	free(s);
	return true;
}

void hw_battery(const char *ps_dir, struct hw_battery *b)
{
	DIR *d = opendir(ps_dir);
	struct dirent *de;

	memset(b, 0, sizeof(*b));
	b->percent = -1;
	if (!d)
		return;
	while ((de = readdir(d))) {
		char type[32], v[32];

		if (de->d_name[0] == '.')
			continue;
		if (!read_line(ps_dir, de->d_name, "type", type, sizeof(type)))
			continue;
		if (!strcmp(type, "Battery")) {
			if (read_line(ps_dir, de->d_name, "present", v, sizeof(v)) && !atoi(v))
				continue;
			b->present = true;
			if (read_line(ps_dir, de->d_name, "capacity", v, sizeof(v)))
				b->percent = CLAMP(atoi(v), 0, 100);
			if (read_line(ps_dir, de->d_name, "status", b->status, sizeof(b->status)))
				b->charging = !strcmp(b->status, "Charging");
		} else if (!strcmp(type, "Mains") || !strcmp(type, "USB")) {
			if (read_line(ps_dir, de->d_name, "online", v, sizeof(v)) && atoi(v))
				b->ac = true;
		}
	}
	closedir(d);
}

bool hw_storage(const char *path, uint64_t *free_bytes, uint64_t *total_bytes)
{
	struct statvfs st;

	if (statvfs(path, &st) < 0)
		return false;
	*free_bytes = (uint64_t)st.f_bavail * st.f_frsize;
	*total_bytes = (uint64_t)st.f_blocks * st.f_frsize;
	return true;
}

void hw_version(const char *fallback, char *out, size_t n)
{
	char *s = file_read("/etc/rsos-version", NULL);

	if (s) {
		strlcpy_(out, str_trim(s), n);
		free(s);
		return;
	}
	s = file_read("/etc/os-release", NULL);
	if (s) {
		char *p = strstr(s, "PRETTY_NAME=");

		if (p) {
			char *e;

			p += 12;
			if (*p == '"')
				p++;
			e = strpbrk(p, "\"\n");
			if (e)
				*e = 0;
			strlcpy_(out, p, n);
			free(s);
			return;
		}
		free(s);
	}
	strlcpy_(out, fallback ? fallback : "RetroStoneOS", n);
}

/* ------------------------------------------------------------ rsos.env */
static void env_overlays(const char *buf, char *out, size_t n)
{
	const char *p = buf;

	out[0] = 0;
	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t l = eol ? (size_t)(eol - p) : strlen(p);

		if (l > 9 && !strncmp(p, "overlays=", 9)) {
			size_t vl = MIN(l - 9, n - 1);

			memcpy(out, p + 9, vl);
			out[vl] = 0;
			str_trim(out);
		}
		p = eol ? eol + 1 : NULL;
	}
	/* strip quotes */
	if (out[0] == '"') {
		memmove(out, out + 1, strlen(out));
		if (*out && out[strlen(out) - 1] == '"')
			out[strlen(out) - 1] = 0;
	}
}

static bool word_in(const char *list, const char *w)
{
	char buf[512], *save = NULL, *t;

	strlcpy_(buf, list, sizeof(buf));
	for (t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save))
		if (!strcmp(t, w))
			return true;
	return false;
}

bool hw_env_has_overlay(const char *env_path, const char *name)
{
	int err;
	bool from_bak;
	char *buf = file_read_user(env_path, NULL, &err, &from_bak);
	char ov[512];
	bool r;

	if (!buf)
		return false;
	env_overlays(buf, ov, sizeof(ov));
	free(buf);
	r = word_in(ov, name);
	return r;
}

int hw_env_set_overlay(const char *env_path, const char *name, bool on)
{
	int err;
	bool from_bak;
	char *buf = file_read_user(env_path, NULL, &err, &from_bak);
	char ov[512], nov[512] = "", dir[1024];
	char *save = NULL, *t;
	size_t cap, len = 0;
	char *out;
	const char *p;
	bool remounted = false;
	struct statvfs st;
	int r;

	/* A read error is not an empty file: rsos.env would be rewritten with
	 * only overlays= (review F-M7). Nothing is written then. */
	if (err && err != -ENOENT) {
		LOGW("hw: %s cannot be read (%s): not rewritten", env_path, strerror(-err));
		free(buf);
		return err;
	}
	env_overlays(buf ? buf : "", ov, sizeof(ov));
	for (t = strtok_r(ov, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
		if (!strcmp(t, name))
			continue;
		if (nov[0])
			strncat(nov, " ", sizeof(nov) - strlen(nov) - 1);
		strncat(nov, t, sizeof(nov) - strlen(nov) - 1);
	}
	if (on) {
		if (nov[0])
			strncat(nov, " ", sizeof(nov) - strlen(nov) - 1);
		strncat(nov, name, sizeof(nov) - strlen(nov) - 1);
	}
	cap = (buf ? strlen(buf) : 0) + strlen(nov) + 64;
	out = xmalloc(cap);
	/* keep every other line, replace/append overlays= */
	for (p = buf; p && *p;) {
		const char *eol = strchr(p, '\n');
		size_t l = eol ? (size_t)(eol - p) : strlen(p);

		if (!(l >= 9 && !strncmp(p, "overlays=", 9))) {
			memcpy(out + len, p, l);
			len += l;
			out[len++] = '\n';
		}
		p = eol ? eol + 1 : NULL;
	}
	if (nov[0])
		len += (size_t)snprintf(out + len, cap - len, "overlays=%s\n", nov);
	free(buf);

	path_dirname(env_path, dir, sizeof(dir));
	/* The root filesystem (with /boot) is mounted read-only. */
	if (statvfs(dir, &st) == 0 && (st.f_flag & ST_RDONLY)) {
		if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) < 0) {
			r = -errno;
			LOGW("hw: cannot remount / read-write: %s", strerror(errno));
			free(out);
			return r;
		}
		remounted = true;
	}
	r = file_write_atomic_bak(env_path, out, len);
	free(out);
	if (remounted) {
		sync();
		if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL) < 0)
			LOGW("hw: cannot remount / read-only: %s", strerror(errno));
	}
	LOGI("hw: %s: overlays=%s (%s)", env_path, nov, r ? strerror(-r) : "ok");
	return r;
}

/* ----------------------------------------------------------------- wifi */
int hw_write_wpa(const char *path, const char *ssid, const char *psk)
{
	char buf[1024], dir[1024];
	int n;

	n = snprintf(buf, sizeof(buf),
		     "# written by the RetroStoneOS settings menu\n"
		     "ctrl_interface=/var/run/wpa_supplicant\n"
		     "update_config=1\n"
		     "network={\n"
		     "\tssid=\"%s\"\n", ssid);
	if (psk && *psk)
		n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\tpsk=\"%s\"\n", psk);
	else
		n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\tkey_mgmt=NONE\n");
	n += snprintf(buf + n, sizeof(buf) - (size_t)n, "}\n");
	path_dirname(path, dir, sizeof(dir));
	mkdir_p(dir);
	return file_write_atomic(path, buf, (size_t)MIN(n, (int)sizeof(buf) - 1));
}

void hw_read_wpa_ssid(const char *path, char *ssid, size_t n)
{
	char *buf = file_read(path, NULL), *p, *q;

	ssid[0] = 0;
	if (!buf)
		return;
	p = strstr(buf, "ssid=\"");
	if (p) {
		p += 6;
		q = strchr(p, '\n');
		if (q)
			*q = 0;
		q = strrchr(p, '"');
		if (q)
			*q = 0;
		strlcpy_(ssid, p, n);
	}
	free(buf);
}

/* ----------------------------------------------------------------- jobs */
struct job {
	int id, tag;
	pid_t pid;
	int fd;
	char out[256];
	size_t len;
	bool done;
	int status;
};

static struct job g_jobs[HW_MAX_JOBS];
static int g_next_id = 1;

int hw_job_start(const char *const argv[], int tag)
{
	int pfd[2];
	pid_t pid;
	struct job *j = NULL;

	for (int i = 0; i < HW_MAX_JOBS; i++)
		if (!g_jobs[i].id) {
			j = &g_jobs[i];
			break;
		}
	if (!j)
		return -EBUSY;
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -errno;
	pid = fork();
	if (pid < 0) {
		int e = errno;

		close(pfd[0]);
		close(pfd[1]);
		return -e;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
		sigset_t none;

		/* review F-L14: the helper (and the daemons it starts) get the
		 * default SIGPIPE and an empty signal mask, not the UI's */
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		if (devnull >= 0)
			dup2(devnull, 0);
		dup2(pfd[1], 1);
		dup2(pfd[1], 2);
		/* the helper must not inherit our realtime-ish priority */
		setsid();
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pfd[1]);
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	memset(j, 0, sizeof(*j));
	j->id = g_next_id++;
	j->tag = tag;
	j->pid = pid;
	j->fd = pfd[0];
	/* the helper and its command, then "on"/"off" or "...": a later
	 * argument can be a secret ("rsos-smb start PASSWORD", review: it was
	 * in the log) */
	{
		const char *a2 = argv[1] ? argv[2] : NULL;

		LOGI("hw: job %d started: %s %s %s", j->id, argv[0], argv[1] ? argv[1] : "",
		     !a2 ? "" : !strcmp(a2, "on") || !strcmp(a2, "off") ? a2 : "...");
	}
	return j->id;
}

static void job_read(struct job *j)
{
	char tmp[256];
	ssize_t r;

	while (j->fd >= 0 && (r = read(j->fd, tmp, sizeof(tmp))) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return;
			break;
		}
		if (j->len + 1 < sizeof(j->out)) {
			size_t c = MIN((size_t)r, sizeof(j->out) - 1 - j->len);

			memcpy(j->out + j->len, tmp, c);
			j->len += c;
			j->out[j->len] = 0;
		}
	}
	close(j->fd);
	j->fd = -1;
}

bool hw_job_poll(struct hw_job_result *r)
{
	for (int i = 0; i < HW_MAX_JOBS; i++) {
		struct job *j = &g_jobs[i];
		int st;
		pid_t p;

		if (!j->id)
			continue;
		job_read(j);
		p = waitpid(j->pid, &st, WNOHANG);
		if (p != j->pid)
			continue;
		if (j->fd >= 0) {
			/* what is left, without blocking: a daemon the helper
			 * started may keep the pipe open forever (a blocking read
			 * froze the menu, and now the watchdog would reboot) */
			job_read(j);
			if (j->fd >= 0) {
				close(j->fd);
				j->fd = -1;
			}
		}
		r->id = j->id;
		r->tag = j->tag;
		r->status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? -WTERMSIG(st) : -1;
		strlcpy_(r->out, str_trim(j->out), sizeof(r->out));
		LOGI("hw: job %d finished (%d): %s", j->id, r->status, r->out);
		memset(j, 0, sizeof(*j));
		return true;
	}
	return false;
}

int hw_jobs_running(void)
{
	int n = 0;

	for (int i = 0; i < HW_MAX_JOBS; i++)
		if (g_jobs[i].id)
			n++;
	return n;
}

bool hw_has_ipv4(char *addr, size_t n)
{
	struct ifaddrs *ifa, *i;
	bool found = false;

	if (addr && n)
		addr[0] = 0;
	if (getifaddrs(&ifa) < 0)
		return false;
	for (i = ifa; i && !found; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
		    !(i->ifa_flags & IFF_UP))
			continue;
		found = true;
		if (addr && n)
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, addr, (socklen_t)n);
	}
	freeifaddrs(ifa);
	return found;
}
