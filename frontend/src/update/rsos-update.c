/*
 * rsos-update - the RetroStoneOS system updater (docs/updates.md).
 *
 * The menu (Settings > System update) runs it as a helper process with
 * --machine and reads its lines; on the UART it is a plain command:
 *
 *   rsos-update info                 this system: version, board, slots
 *   rsos-update check                newer version online (GitHub) or on the SD
 *                                    card / a USB drive?
 *   rsos-update apply FILE           install a .rsu package
 *   rsos-update apply auto           the newest version found by check
 *   rsos-update verify FILE          check a package without installing it
 *   rsos-update boot                 after a restart: report and clean up
 *   rsos-update status               the boot slots and the update state
 *
 * Nothing is written before the package's signature and payload hash are
 * verified; the running slot and /data are never written; the boot slot
 * changes in one fw_setenv call at the very end.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "rsu.h"
#include "update.h"

static struct upd_ctx C;
static bool g_machine;
static FILE *g_logf;
static bool g_quiet_log;

/* ------------------------------------------------------------ output */
/* machine lines: "type<TAB>key=value<TAB>..." with \\ \t \n escaped */
static void mfield(const char *key, const char *val)
{
	fprintf(stdout, "\t%s=", key);
	for (const char *p = val ? val : ""; *p; p++) {
		if (*p == '\\')
			fputs("\\\\", stdout);
		else if (*p == '\t')
			fputs("\\t", stdout);
		else if (*p == '\n')
			fputs("\\n", stdout);
		else if (*p != '\r')
			fputc(*p, stdout);
	}
}

static void mfieldf(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void mfieldf(const char *key, const char *fmt, ...)
{
	char b[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	mfield(key, b);
}

static void mend(void)
{
	fputc('\n', stdout);
	fflush(stdout);
}

static void on_log(const char *line, void *user)
{
	(void)user;
	if (g_logf) {
		time_t t = time(NULL);
		struct tm tm;
		char ts[32];

		localtime_r(&t, &tm);
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
		fprintf(g_logf, "%s %s\n", ts, line);
		fflush(g_logf);
	}
	if (!g_machine && !g_quiet_log)
		fprintf(stderr, "rsos-update: %s\n", line);
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_progress(struct upd_ctx *c, const char *phase, uint64_t done, uint64_t total)
{
	static char last_phase[16];
	static int64_t last_ms;
	static int last_pct = -1;
	int64_t t = now_ms();
	int pct = total ? (int)(done * 100 / total) : 0;
	bool changed = strcmp(last_phase, phase) != 0;

	(void)c;
	if (!changed && done != total && t - last_ms < 250)
		return;
	last_ms = t;
	snprintf(last_phase, sizeof(last_phase), "%s", phase);
	if (g_machine) {
		fputs("progress", stdout);
		mfield("phase", phase);
		mfieldf("done", "%" PRIu64, done);
		mfieldf("total", "%" PRIu64, total);
		mend();
	} else if (changed || pct != last_pct) {
		fprintf(stderr, "\r%-9s %3d %%  (%" PRIu64 " / %" PRIu64 " MB)%s", phase, pct, done >> 20, total >> 20,
			done == total ? "\n" : "");
	}
	last_pct = pct;
}

static int fail(enum rsu_err e)
{
	if (g_machine) {
		fputs("error", stdout);
		mfield("code", rsu_err_code(e));
		mfield("msg", rsu_err_text(e));
		mfield("detail", C.err);
		mend();
	} else {
		fprintf(stderr, "rsos-update: %s%s%s\n", rsu_err_text(e), C.err[0] ? ": " : "", C.err);
	}
	return e == RSU_E_CANCELLED ? 3 : 1;
}

static void print_found(const char *type, const char *status, const struct upd_found *f)
{
	if (g_machine) {
		fputs(type, stdout);
		mfield("status", status);
		if (f && f->valid) {
			mfield("source", f->source);
			mfield("version", f->version);
			mfieldf("size", "%" PRIu64, f->size);
			mfield("where", f->where);
			mfield("name", f->name);
			mfield("page", f->page);
			mfieldf("signed", "%d", f->is_signed);
			mfieldf("installable", "%d", f->installable);
			mfield("verdict", rsu_err_code(f->verdict));
			mfield("notes", f->notes);
		}
		mend();
		return;
	}
	if (!f || !f->valid) {
		printf("%s: %s\n", type, status);
		return;
	}
	printf("%s: %s: RetroStoneOS %s (%s, %" PRIu64 " MB)\n  %s\n", type, status, f->version,
	       f->is_signed ? "signed" : "not signed", f->size >> 20, f->where);
	if (!f->installable && f->verdict)
		printf("  cannot be installed: %s\n", rsu_err_text(f->verdict));
	if (f->notes[0])
		printf("  notes:\n%s\n", f->notes);
}

static void print_info(void)
{
	if (g_machine) {
		fputs("info", stdout);
		mfield("version", C.sys.version);
		mfield("board", C.sys.board);
		mfield("variant", C.variant);
		mfield("build_date", C.build_date);
		mfieldf("ab", "%d", C.ab);
		mfield("reason", C.ab_reason);
		mfieldf("slot", "%c", C.booted ? C.booted : '-');
		mfieldf("key", "%d", C.sys.have_key);
		mfieldf("bootloader", "%d", C.sys.bootloader);
		mfieldf("tls", "%d", http_have_tls());
		mend();
		return;
	}
	printf("RetroStoneOS %s (%s build%s%s), board %s\n", C.sys.version, C.variant, C.build_date[0] ? ", " : "",
	       C.build_date, C.sys.board[0] ? C.sys.board : "unknown");
	printf("in-place updates: %s%s%s\n", C.ab ? "yes" : "no", C.ab ? "" : ": ", C.ab ? "" : C.ab_reason);
	if (C.booted)
		printf("running slot %c (%s), bootloader level %d\n", C.booted, C.root_dev, C.sys.bootloader);
	printf("update key: %s, HTTPS: %s\n", C.sys.have_key ? C.pubkey : "none", http_have_tls() ? "yes" : "no");
}

/* ------------------------------------------------------------ commands */
static int cmd_check(const char *dir, bool local, bool net, struct upd_found *pick)
{
	static struct upd_found lf, nf, best;
	enum rsu_err e;

	if (!pick)
		pick = &best;
	memset(pick, 0, sizeof(*pick));
	print_info();
	if (local) {
		e = upd_scan_local(&C, dir, &lf);
		print_found("local", e == RSU_E_NOTFOUND ? "none" : lf.installable ? "found" : "refused", &lf);
		if (lf.installable)
			*pick = lf;
	}
	if (net) {
		e = upd_check_net(&C, &nf);
		if (e == RSU_OK) {
			print_found("net", "found", &nf);
			if (nf.installable &&
			    (!pick->valid || rsu_version_cmp(nf.version, nf.build_time, pick->version, pick->build_time) > 0))
				*pick = nf;
		} else if (e == RSU_E_SAME) {
			print_found("net", "uptodate", &nf);
		} else if (e == RSU_E_NOTFOUND) {
			print_found("net", "none", NULL);
		} else {
			if (g_machine) {
				fputs("net", stdout);
				mfield("status", "error");
				mfield("code", rsu_err_code(e));
				mfield("msg", rsu_err_text(e));
				mfield("detail", C.err);
				mend();
			} else {
				printf("net: %s (%s)\n", rsu_err_text(e), C.err);
			}
		}
	}
	/* the one to install: the newest installable package (a local file
	 * wins over a download of the same version) */
	print_found("best", pick->valid ? "found" : "none", pick);
	return 0;
}

static int apply_path(const char *path, bool downloaded)
{
	static struct rsu_header h;
	enum rsu_err e;

	if (g_machine) {
		fputs("state", stdout);
		mfield("phase", "verify");
		mend();
	}
	e = upd_apply_file(&C, path, downloaded, &h);
	if (e)
		return fail(e);
	if (g_machine) {
		fputs("done", stdout);
		mfield("version", h.m.version);
		mfieldf("slot", "%c", C.target_slot);
		mend();
	} else {
		printf("RetroStoneOS %s is installed in slot %c. Restart to use it (reboot).\n", h.m.version,
		       C.target_slot);
	}
	return 0;
}

static int apply_url(const char *url, const char *name, uint64_t size)
{
	char path[4096];
	enum rsu_err e;

	if (!C.ab) {
		snprintf(C.err, sizeof(C.err), "%s", C.ab_reason);
		return fail(RSU_E_NOAB);
	}
	if (g_machine) {
		fputs("state", stdout);
		mfield("phase", "download");
		mend();
	}
	/* one lock from the download to the end of the install: another
	 * rsos-update can neither write into the same .part nor install or
	 * delete the file in between */
	e = upd_lock(&C);
	if (e)
		return fail(e);
	e = upd_download(&C, url, name, size, path, sizeof(path));
	if (e) {
		upd_unlock(&C);
		return fail(e);
	}
	{
		int r = apply_path(path, true);

		upd_unlock(&C);
		return r;
	}
}

static int cmd_verify(const char *path, bool full)
{
	static struct rsu_header h;
	enum rsu_err e = upd_verify_file(&C, path, full, &h);
	int c;

	if (e)
		return fail(e);
	c = rsu_version_cmp(h.m.version, h.m.build_time, C.sys.version, C.sys.build_time);
	if (g_machine) {
		fputs("verified", stdout);
		mfield("version", h.m.version);
		mfield("board", h.m.board);
		mfieldf("signed", "%d", h.has_sig);
		mfield("compare", c > 0 ? "newer" : c < 0 ? "older" : "same");
		mend();
	} else {
		printf("%s: OK: RetroStoneOS %s for %s, %s, %s than the running %s%s\n", path, h.m.version, h.m.board,
		       h.has_sig ? "signed with this system's key" : "NOT SIGNED", c > 0 ? "newer" : c < 0 ? "older" : "the same",
		       C.sys.version, full ? " (image checked)" : "");
	}
	return 0;
}

static int cmd_boot(void)
{
	char ev[32], ver[64];

	upd_boot(&C, ev, sizeof(ev), ver, sizeof(ver));
	if (g_machine) {
		fputs("boot", stdout);
		mfield("event", ev);
		mfield("version", ver);
		mend();
	} else {
		printf("%s%s%s\n", ev, ver[0] ? " " : "", ver);
	}
	return 0;
}

static int cmd_status(void)
{
	static const char *const vars[] = { "rsos_slot", "rsos_ok", "rsos_tries", "rsos_fails", "rsos_fallback",
					     "rsos_bad", "rsos_good", "rsos_rounds", "rsos_bootloader" };
	char v[128], sp[4096];
	FILE *f;

	print_info();
	for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
		if (upd_env_get(&C, vars[i], v, sizeof(v)) == 0)
			printf("%s=%s\n", vars[i], v);
	upd_state_path(&C, sp, sizeof(sp));
	f = fopen(sp, "re");
	if (f) {
		char line[512];

		printf("installed update (%s):\n", sp);
		while (fgets(line, sizeof(line), f))
			if (line[0] != '#')
				printf("  %s", line);
		fclose(f);
	}
	return 0;
}

static void on_signal(int sig)
{
	(void)sig;
	C.cancel = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: rsos-update [options] info | check | apply FILE|auto | verify FILE | boot | status\n"
		"  check [--local-only | --net-only] [--dir DIR]\n"
		"  apply FILE          install a .rsu package (a file on the SD card or a USB drive)\n"
		"  apply auto          download and install the newest version that check finds\n"
		"  apply --url URL --name NAME --size BYTES   (the menu) download, then install\n"
		"  verify [--full] FILE\n"
		"options:\n"
		"  --machine           one line per event, for the menu\n"
		"  --allow-unsigned    development builds: accept a package without signature\n"
		"  --allow-dev         release builds: accept a signed development package (UART only)\n"
		"  --force             install the same or an older version\n"
		"  --ignore-battery    no battery check\n"
		"  --prerelease        also consider pre-releases\n"
		"  --log FILE          (default /data/rsos/logs/update.log, else /run/rsos/update.log)\n"
		"tests: --root DIR --target DEV --fw-printenv PATH --fw-setenv PATH --api-url URL\n"
		"       --allow-http --ca-file FILE --pubkey FILE\n");
}

int main(int argc, char **argv)
{
	const char *root = getenv("RSOS_UPDATE_ROOT"), *logpath = NULL, *cmd = NULL, *arg = NULL;
	const char *dir = NULL, *url = NULL, *name = NULL;
	const char *o_target = NULL, *o_printenv = NULL, *o_setenv = NULL, *o_api = NULL, *o_ca = NULL, *o_key = NULL;
	bool local = true, net = true, full = false, allow_http = false, prerelease = false, ignore_batt = false;
	unsigned flags = 0;
	uint64_t size = 0;
	struct sigaction sa;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--machine"))
			g_machine = true;
		else if (!strcmp(a, "--allow-unsigned"))
			flags |= RSU_ALLOW_UNSIGNED;
		else if (!strcmp(a, "--allow-dev"))
			flags |= RSU_ALLOW_DEV;
		else if (!strcmp(a, "--force"))
			flags |= RSU_FORCE;
		else if (!strcmp(a, "--ignore-battery"))
			ignore_batt = true;
		else if (!strcmp(a, "--prerelease"))
			prerelease = true;
		else if (!strcmp(a, "--local-only"))
			net = false;
		else if (!strcmp(a, "--net-only"))
			local = false;
		else if (!strcmp(a, "--full"))
			full = true;
		else if (!strcmp(a, "--allow-http"))
			allow_http = true;
		else if (!strcmp(a, "--quiet"))
			g_quiet_log = true;
		else if (v && !strcmp(a, "--log"))
			logpath = v, i++;
		else if (v && !strcmp(a, "--root"))
			root = v, i++;
		else if (v && !strcmp(a, "--dir"))
			dir = v, i++;
		else if (v && !strcmp(a, "--url"))
			url = v, i++;
		else if (v && !strcmp(a, "--name"))
			name = v, i++;
		else if (v && !strcmp(a, "--size"))
			size = strtoull(v, NULL, 10), i++;
		else if (v && !strcmp(a, "--target"))
			o_target = v, i++;
		else if (v && !strcmp(a, "--fw-printenv"))
			o_printenv = v, i++;
		else if (v && !strcmp(a, "--fw-setenv"))
			o_setenv = v, i++;
		else if (v && !strcmp(a, "--api-url"))
			o_api = v, i++;
		else if (v && !strcmp(a, "--ca-file"))
			o_ca = v, i++;
		else if (v && !strcmp(a, "--pubkey"))
			o_key = v, i++;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else if (a[0] == '-') {
			fprintf(stderr, "rsos-update: unknown option %s\n", a);
			usage();
			return 2;
		} else if (!cmd)
			cmd = a;
		else if (!arg)
			arg = a;
		else {
			usage();
			return 2;
		}
	}
	if (!cmd) {
		usage();
		return 2;
	}
	upd_ctx_init(&C, root);
	if (o_target)
		snprintf(C.target_dev, sizeof(C.target_dev), "%s", o_target);
	if (o_printenv)
		snprintf(C.fw_printenv, sizeof(C.fw_printenv), "%s", o_printenv);
	if (o_setenv)
		snprintf(C.fw_setenv, sizeof(C.fw_setenv), "%s", o_setenv);
	if (o_api)
		snprintf(C.api_url, sizeof(C.api_url), "%s", o_api);
	if (o_ca)
		snprintf(C.ca_file, sizeof(C.ca_file), "%s", o_ca);
	if (o_key)
		snprintf(C.pubkey, sizeof(C.pubkey), "%s", o_key);
	C.allow_http = allow_http;
	C.prerelease = prerelease;
	C.ignore_battery = ignore_batt;
	C.flags = flags;
	C.progress = on_progress;

	{
		char lp[4096], old[4200];
		struct stat st;

		/* on the RETROSTONE drive (rsos/logs/update.log: it survives the
		 * restart and a user can send it), else /run/rsos */
		if (!logpath) {
			snprintf(lp, sizeof(lp), "%s/rsos/logs", C.data_dir);
			if (stat(lp, &st) == 0 && S_ISDIR(st.st_mode))
				snprintf(lp, sizeof(lp), "%s/rsos/logs/update.log", C.data_dir);
			else
				snprintf(lp, sizeof(lp), "%s/update.log", C.run_dir);
			logpath = lp;
		}
		if (stat(logpath, &st) == 0 && st.st_size > 256 * 1024) {
			snprintf(old, sizeof(old), "%s.old", logpath);
			rename(logpath, old);
		}
		g_logf = fopen(logpath, "ae");
		upd_set_log(on_log, NULL);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);            /* the menu may go away: carry on */
	upd_load_system(&C);
	upd_logf("rsos-update %s%s%s (%s %s, board %s, slot %c)", cmd, arg ? " " : "", arg ? arg : "", C.sys.version,
		 C.variant, C.sys.board[0] ? C.sys.board : "?", C.booted ? C.booted : '-');

	if (!strcmp(cmd, "info")) {
		print_info();
		return 0;
	}
	if (!strcmp(cmd, "check"))
		return cmd_check(dir, local, net, NULL);
	if (!strcmp(cmd, "boot"))
		return cmd_boot();
	if (!strcmp(cmd, "status"))
		return cmd_status();
	if (!strcmp(cmd, "verify")) {
		if (!arg) {
			usage();
			return 2;
		}
		return cmd_verify(arg, full);
	}
	if (!strcmp(cmd, "apply")) {
		if (url) {
			if (!name || !size) {
				fprintf(stderr, "rsos-update: --url needs --name and --size\n");
				return 2;
			}
			return apply_url(url, name, size);
		}
		if (!arg) {
			usage();
			return 2;
		}
		if (!strcmp(arg, "auto")) {
			static struct upd_found f;

			g_quiet_log = true;
			cmd_check(NULL, true, true, &f);
			g_quiet_log = false;
			if (!f.valid) {
				snprintf(C.err, sizeof(C.err), "no newer version that this system can install");
				return fail(RSU_E_NOTFOUND);
			}
			if (!strcmp(f.source, "file"))
				return apply_path(f.where, false);
			return apply_url(f.where, f.name, f.size);
		}
		return apply_path(arg, false);
	}
	usage();
	return 2;
}
