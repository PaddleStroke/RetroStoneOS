/*
 * update.h - the system updater engine (docs/updates.md), used by the
 * rsos-update program (the UI runs it as a helper process, the UART user
 * runs it by hand):
 *
 *   - the system: version, board, update key, A/B capability, booted slot;
 *   - finding updates: GitHub Releases (HTTPS), the data partition
 *     (/data/update, /data/rsos/update) and USB drives (/media/<x>/ and
 *     /media/<x>/RetroStoneOS/);
 *   - downloading (resumable, free-space check) and verifying (signature,
 *     then the payload hash) before anything is written;
 *   - writing the new root filesystem to the inactive slot, reading it back,
 *     then switching the boot slot in one fw_setenv call;
 *   - after the restart: "updated", clean-up once the new slot is confirmed.
 *
 * Every path is under a root prefix, so the tests run it on a fake tree.
 */
#ifndef RSOS_UPDATE_UPDATE_H
#define RSOS_UPDATE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rsu.h"

#define UPD_REPO_API "https://api.github.com/repos/PaddleStroke/RetroStoneOS/releases?per_page=10"
#define UPD_BATTERY_MIN 30

struct upd_ctx;
/* phase: "download", "verify", "write", "readback", "switch" */
typedef void (*upd_progress_fn)(struct upd_ctx *c, const char *phase, uint64_t done, uint64_t total);

struct upd_ctx {
	/* configuration (upd_ctx_init fills the device paths under root) */
	char root[512];
	char version_env[1024];       /* /etc/rsos/version.env */
	char board_ini[1024];         /* /etc/rsos/board.ini */
	char cmdline[1024];           /* /proc/cmdline */
	char fw_config[1024];         /* /etc/fw_env.config */
	char fw_printenv[1024];       /* /usr/sbin/fw_printenv (fw_setenv: the same binary) */
	char fw_setenv[1024];
	char pubkey[1024];            /* /usr/share/rsos/update.pub */
	char data_dir[1024];          /* /data */
	char run_dir[1024];           /* /run/rsos */
	char media_dir[1024];         /* /media */
	char power_dir[1024];         /* /sys/class/power_supply */
	char ca_file[1024];           /* /etc/ssl/certs/ca-certificates.crt */
	char api_url[1024];           /* UPD_REPO_API */
	char target_dev[1024];        /* tests: the inactive partition, instead of root='s sibling */
	bool allow_http;              /* tests: plain http:// URLs */
	bool prerelease;              /* also consider GitHub pre-releases */
	bool ignore_battery;
	unsigned flags;               /* RSU_ALLOW_UNSIGNED | RSU_FORCE */
	volatile int cancel;          /* set by a signal handler: stop at the next check */
	upd_progress_fn progress;
	void *user;

	/* the system (upd_load_system) */
	struct rsu_system sys;
	char variant[16];             /* release | dev */
	char build_date[16];
	bool ab;                      /* in-place (A/B) updates possible */
	char ab_reason[160];          /* why not */
	char booted;                  /* 'a' / 'b' / 0: rsos.slot= */
	char root_dev[256];           /* root= */
	char target[1024];            /* the inactive slot's partition (upd_slots) */
	char target_slot;
	int lock_fd;                  /* <run>/update.lock held (upd_lock), else -1 */

	char err[512];                /* details of the last error (English) */
};

/* A package (or release) that was found. */
struct upd_found {
	bool valid;
	char source[8];               /* "net" | "file" */
	char where[2048];             /* URL of the .rsu (net) or its path (file) */
	char page[512];               /* the release page (net) */
	char name[256];               /* file / asset name */
	char version[64];
	int64_t build_time;
	uint64_t size;                /* bytes to download / read */
	bool is_signed;
	bool installable;             /* an A/B package this system can install */
	enum rsu_err verdict;         /* why not installable (RSU_OK if it is) */
	char notes[16384];            /* release notes / changelog */
};

void upd_ctx_init(struct upd_ctx *c, const char *root);
/* Log sink for the engine (default: stderr). */
void upd_set_log(void (*fn)(const char *line, void *user), void *user);
void upd_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Reads version.env, the key, board.ini, the command line and the boot
 * environment. Never fails: missing parts leave ab false with a reason. */
void upd_load_system(struct upd_ctx *c);

/* The first bytes of a package file, parsed (no signature check). */
enum rsu_err upd_read_header(struct upd_ctx *c, const char *path, struct rsu_header *h);

/* Local packages: dir (and dir/RetroStoneOS), or every usual place when dir
 * is NULL. The newest installable one goes to *best (valid); without one,
 * *best describes the most useful refusal (a package for this board that is
 * older, needs a newer bootloader...), valid false. RSU_E_NOTFOUND: no
 * .rsu at all. */
enum rsu_err upd_scan_local(struct upd_ctx *c, const char *dir, struct upd_found *best);

/* GitHub Releases. RSU_OK: *out is newer than the running system (check
 * out->installable: false on boards without A/B, or a refused package);
 * RSU_E_SAME: up to date (out->version is the newest release);
 * RSU_E_NOTFOUND: no release for this board; else a network error. */
enum rsu_err upd_check_net(struct upd_ctx *c, struct upd_found *out);

/* The update lock (<run>/update.lock): one download or install at a time.
 * upd_download and upd_apply_file take it themselves when it is not held;
 * a caller that downloads then installs holds it across both. RSU_E_BUSY
 * when another process has it. */
enum rsu_err upd_lock(struct upd_ctx *c);
void upd_unlock(struct upd_ctx *c);

/* Downloads url (size bytes) to <data>/rsos/update/<name>, resuming a
 * previous partial download; path_out gets the file. A complete file there
 * is taken as it is: upd_apply_file checks it, and deletes it when it is
 * damaged (so the next attempt downloads it again). */
enum rsu_err upd_download(struct upd_ctx *c, const char *url, const char *name, uint64_t size,
			  char *path_out, size_t n);

/* Verifies and installs a package into the inactive slot, then switches the
 * boot slot. downloaded: the file is ours (deleted once the new system is
 * confirmed, or at once when it turns out damaged: format, signature or
 * payload hash); a user's file is never deleted. *h gets the package
 * header. */
enum rsu_err upd_apply_file(struct upd_ctx *c, const char *path, bool downloaded, struct rsu_header *h);

/* Checks a package without installing it: signature (any version), payload
 * hash, and with full the decompressed image's size and hash. */
enum rsu_err upd_verify_file(struct upd_ctx *c, const char *path, bool full, struct rsu_header *h);

/* After a restart: event gets "none", "updated" (first boot of the new
 * version: tell the user), "pending" (installed, restart not done yet),
 * "confirmed" (the new system is confirmed: the download was removed),
 * "failed" (the new system did not start: the old one runs) or "unknown"
 * (the boot environment could not be read: nothing is removed, ask again
 * later; RSU_E_ENV). */
enum rsu_err upd_boot(struct upd_ctx *c, char *event, size_t n, char *version, size_t vn);

/* Determines the booted slot and the inactive partition (c->target), and
 * checks that the running slot is confirmed. */
enum rsu_err upd_slots(struct upd_ctx *c);

/* Battery: false when a battery is present, below UPD_BATTERY_MIN % and
 * without external power. */
bool upd_battery_ok(struct upd_ctx *c, int *percent, bool *ac);

/* fw_printenv -n name; 0 or -1 (not set / no environment). */
int upd_env_get(struct upd_ctx *c, const char *name, char *out, size_t n);

/* The state file of an installed update (<data>/rsos/update-state.ini). */
void upd_state_path(struct upd_ctx *c, char *out, size_t n);

#endif
