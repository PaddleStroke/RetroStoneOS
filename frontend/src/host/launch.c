/*
 * launch.c - UI side: start one game in a child process and wait for it.
 * See host.h (host_launch) and docs/host-design.md, "Process model".
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "host_internal.h"
#include "hutil.h"
#include "../i18n/i18n.h"

#define MAX_ARGS 80

static volatile pid_t child_pid;

pid_t host_child_pid(void)
{
	return child_pid;
}

static void parse_line(char *line, const struct host_launch_opts *opts, struct host_launch_result *res)
{
	char *arg = strchr(line, ' ');

	if (!strncmp(line, "error ", 6))
		hstrlcpy(res->message, line + 6, sizeof(res->message));
	else if (!strncmp(line, "warn ", 5))
		hstrlcpy(res->warning, line + 5, sizeof(res->warning));
	else if (!strncmp(line, "autostate ", 10)) {
		long long bytes = 0, ms = 0;

		res->auto_state_saved = true;
		if (sscanf(line + 10, "%lld %lld", &bytes, &ms) == 2) {
			res->auto_state_bytes = bytes;
			res->auto_state_ms = (int)ms;
		}
	} else if (!strncmp(line, "switch ", 7)) {
		int n = atoi(line + 7);

		res->switch_to = n >= 0 && n < HOST_SWITCHER_MAX ? n : -1;
	} else if (!strncmp(line, "playtime ", 9)) {
		long long s = atoll(line + 9);

		if (s >= 0)
			res->playtime_s = s;
	}
	if (opts->on_status && *line) {
		if (arg)
			*arg++ = 0;
		opts->on_status(line, arg ? arg : "", opts->user);
	}
}

/* The complete lines of buf[0..*len), in order; the incomplete tail is
 * moved to the start (a long game sends a line every few minutes: the
 * buffer never fills up with old lines). */
static void parse_status(char *buf, size_t *len, const struct host_launch_opts *opts,
			 struct host_launch_result *res)
{
	char *line = buf, *nl;
	size_t rest;

	buf[*len] = 0;
	while ((nl = strchr(line, '\n'))) {
		*nl = 0;
		parse_line(line, opts, res);
		line = nl + 1;
	}
	rest = *len - (size_t)(line - buf);
	memmove(buf, line, rest);
	*len = rest;
	buf[rest] = 0;
}

/* ------------------------------------------------------ the switcher file */
/* One entry per line: current, name, system, rom, core, core_path, thumb,
 * separated by tabs (none of them has a tab or a newline: they are cut). */
static void put_field(FILE *f, const char *s, bool last)
{
	for (; s && *s; s++)
		fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
	fputc(last ? '\n' : '\t', f);
}

int host_switcher_write(const char *path, const struct host_switch_entry *e, int n)
{
	char tmp[PATH_MAX + 8];
	FILE *f;
	int r = 0;

	if (!hpath(tmp, sizeof(tmp), "%s.tmp", path))
		return -ENAMETOOLONG;
	f = fopen(tmp, "we");
	if (!f)
		return -errno;
	fputs("# RetroStoneOS game switcher: current, name, system, rom, core, core_path, thumbnail\n", f);
	for (int i = 0; i < n && i < HOST_SWITCHER_MAX; i++) {
		fputs(e[i].current ? "1\t" : "0\t", f);
		put_field(f, e[i].name, false);
		put_field(f, e[i].system, false);
		put_field(f, e[i].rom, false);
		put_field(f, e[i].core, false);
		put_field(f, e[i].core_path, false);
		put_field(f, e[i].thumb, true);
	}
	if (fclose(f) != 0)
		r = -EIO;
	if (!r && rename(tmp, path) < 0)
		r = -errno;
	if (r)
		unlink(tmp);
	return r;
}

int host_switcher_read(const char *path, struct host_switch_entry *e, int max)
{
	char *txt = path ? hread_file(path, NULL) : NULL, *save = NULL, *line;
	int n = 0;

	if (!txt)
		return 0;
	for (line = strtok_r(txt, "\n", &save); line && n < max; line = strtok_r(NULL, "\n", &save)) {
		char *f[7];
		int k = 0;

		if (line[0] == '#')
			continue;
		f[k++] = line;
		for (char *p = line; *p && k < 7; p++)
			if (*p == '\t') {
				*p = 0;
				f[k++] = p + 1;
			}
		if (k < 7 || !f[3][0])
			continue;
		memset(&e[n], 0, sizeof(e[n]));
		e[n].current = f[0][0] == '1';
		hstrlcpy(e[n].name, f[1], sizeof(e[n].name));
		hstrlcpy(e[n].system, f[2], sizeof(e[n].system));
		hstrlcpy(e[n].rom, f[3], sizeof(e[n].rom));
		hstrlcpy(e[n].core, f[4], sizeof(e[n].core));
		hstrlcpy(e[n].core_path, f[5], sizeof(e[n].core_path));
		hstrlcpy(e[n].thumb, f[6], sizeof(e[n].thumb));
		n++;
	}
	free(txt);
	return n;
}

int host_launch(const char *core_path, const char *rom_path, const char *system,
		const struct host_launch_opts *opts, struct host_launch_result *res)
{
	static const struct host_launch_opts defaults = { 0 };
	const char *argv[MAX_ARGS];
	char status[4096], hdr[1200];
	size_t slen = 0;
	int pfd[2], argc = 0, wstatus = 0, hdr_len = 0;
	int log_flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
	pid_t pid;

	if (!opts)
		opts = &defaults;
	memset(res, 0, sizeof(*res));
	res->switch_to = -1;
	res->playtime_s = -1;
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -errno;

	argv[argc++] = opts->exe ? opts->exe : "/proc/self/exe";
	if (!opts->exe)
		argv[argc++] = "--run";
	argv[argc++] = "--core";
	argv[argc++] = core_path;
	if (rom_path) {
		argv[argc++] = "--rom";
		argv[argc++] = rom_path;
	}
	if (system && *system) {
		argv[argc++] = "--system";
		argv[argc++] = system;
	}
	argv[argc++] = "--status-fd";
	argv[argc++] = "3";
	if (!opts->manage_governor)
		argv[argc++] = "--no-governor";
	if (opts->resume) {
		argv[argc++] = "--load-state";
		argv[argc++] = "auto";
	}
	for (int i = 0; opts->extra_args && opts->extra_args[i] && argc < MAX_ARGS - 1; i++)
		argv[argc++] = opts->extra_args[i];
	argv[argc] = NULL;

	/* Appended log: a header per launch, made here (the child only calls
	 * async-signal-safe functions before exec). */
	if (opts->log_append) {
		const char *log = opts->log_path ? opts->log_path : "/tmp/rsos-game.log";
		struct stat lst;
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		hdr_len = snprintf(hdr, sizeof(hdr), "\n=== [%5lld.%03ld] launch: %s %s%s%s\n",
				   (long long)ts.tv_sec, ts.tv_nsec / 1000000, core_path,
				   rom_path ? rom_path : "", system && *system ? " system " : "",
				   system && *system ? system : "");
		if (hdr_len < 0)
			hdr_len = 0;
		if (hdr_len > (int)sizeof(hdr) - 1)
			hdr_len = (int)sizeof(hdr) - 1;
		log_flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
		if (stat(log, &lst) == 0 && lst.st_size > (1 << 20))
			log_flags |= O_TRUNC;      /* keep /run small */
	}

	pid = fork();
	if (pid < 0) {
		int e = errno;

		close(pfd[0]);
		close(pfd[1]);
		return -e;
	}
	if (pid == 0) {
		const char *log = opts->log_path ? opts->log_path : "/tmp/rsos-game.log";
		sigset_t none;
		int fd;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		if (pfd[1] == 3)
			fcntl(3, F_SETFD, 0);
		else if (dup2(pfd[1], 3) < 0)
			_exit(127);
		fd = open(log, log_flags, 0644);
		if (fd >= 0) {
			if (hdr_len > 0 && write(fd, hdr, (size_t)hdr_len) < 0) {
				/* the game runs without its header */
			}
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	child_pid = pid;
	close(pfd[1]);
	hlog(HLOG_INFO, "game process %d started: %s %s", (int)pid, core_path, rom_path ? rom_path : "");

	for (;;) {
		struct pollfd p = { .fd = pfd[0], .events = POLLIN };
		pid_t w;

		if (poll(&p, 1, 100) > 0 && (p.revents & (POLLIN | POLLHUP))) {
			char scratch[256];
			/* a full status buffer: read the rest away, never read(0) in
			 * a loop that poll() keeps waking (review, small items) */
			bool full = slen >= sizeof(status) - 1;
			ssize_t r = full ? read(pfd[0], scratch, sizeof(scratch)) :
				    read(pfd[0], status + slen, sizeof(status) - 1 - slen);

			if (r > 0 && !full) {
				slen += (size_t)r;
				/* complete lines now: on_status sees them while the
				 * game runs (play time, per-game settings) */
				parse_status(status, &slen, opts, res);
			}
		}
		w = waitpid(pid, &wstatus, WNOHANG);
		if (w == pid)
			break;
		if (w < 0 && errno != EINTR)
			break;
		if (opts->idle && opts->idle(opts->user)) {
			hlog(HLOG_WARN, "killing game process %d on request", (int)pid);
			kill(pid, SIGKILL);
		}
	}
	/* Whatever is left in the pipe, without blocking: a descendant of the
	 * game could keep it open (the menu would freeze, and the watchdog
	 * reboot the unit). */
	while (slen < sizeof(status) - 1) {
		struct pollfd p = { .fd = pfd[0], .events = POLLIN };
		ssize_t r;

		if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN))
			break;
		r = read(pfd[0], status + slen, sizeof(status) - 1 - slen);
		if (r <= 0)
			break;
		slen += (size_t)r;
	}
	close(pfd[0]);
	child_pid = 0;
	/* a last line without its newline counts too */
	if (slen > 0 && slen < sizeof(status) - 1 && status[slen - 1] != '\n')
		status[slen++] = '\n';
	parse_status(status, &slen, opts, res);

	/* A crashed child may have left the CPU at full clock. */
	if (opts->manage_governor)
		sys_governor_performance(false);

	if (WIFSIGNALED(wstatus)) {
		res->crashed = true;
		res->signal = WTERMSIG(wstatus);
		res->status = HOST_EXIT_FAIL;
		/* Messages for the menu UI (this runs in the UI process: its
		 * language). */
		if (res->signal == SIGKILL) {
			snprintf(res->message, sizeof(res->message), "%s", _("The game was stopped"));
		} else {
			/* TRANSLATORS: shown by the menu after a crash; %s is the
			 * signal name in English ("Segmentation fault") */
			snprintf(res->message, sizeof(res->message),
				 _("The emulator crashed (%s). Game saves up to a few seconds ago are kept."),
				 strsignal(res->signal));
			hutf8_trim(res->message);
		}
		hlog(HLOG_ERROR, "game process %d killed by signal %d", (int)pid, res->signal);
		return 0;
	}
	res->exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
	switch (res->exit_code) {
	case HOST_EXIT_OK:
		res->status = HOST_EXIT_OK;
		res->message[0] = 0;
		break;
	case HOST_EXIT_ERROR:
		res->status = HOST_EXIT_ERROR;
		if (!res->message[0])
			snprintf(res->message, sizeof(res->message), "%s", _("Could not start the game"));
		break;
	case HOST_EXIT_POWEROFF:
		res->status = HOST_EXIT_POWEROFF;
		break;
	case HOST_EXIT_SWITCH:
		/* the game switcher: the menu launches entry res->switch_to */
		res->status = HOST_EXIT_SWITCH;
		res->message[0] = 0;
		break;
	case HOST_EXIT_HANG:
		res->status = HOST_EXIT_HANG;
		snprintf(res->message, sizeof(res->message),
			 "%s", _("The emulator stopped responding. Game saves up to a few seconds ago are kept."));
		break;
	case 127:
		res->status = HOST_EXIT_FAIL;
		snprintf(res->message, sizeof(res->message), "%s", _("Cannot start the game process"));
		break;
	default:
		res->status = HOST_EXIT_FAIL;
		if (!res->message[0])
			snprintf(res->message, sizeof(res->message), _("The emulator failed (code %d)"), res->exit_code);
		break;
	}
	hutf8_trim(res->message);
	hlog(HLOG_INFO, "game process %d exited: %d%s%s", (int)pid, res->exit_code,
	     res->message[0] ? ", " : "", res->message);
	return 0;
}
