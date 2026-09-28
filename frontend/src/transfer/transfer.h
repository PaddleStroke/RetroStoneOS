/*
 * transfer.h - getting ROMs, BIOS files, saves and themes onto the console.
 *
 * Public API of frontend/src/transfer/ (see docs/rom-transfer.md):
 *
 *   1. System folders: canonical ROM folder names and the aliases other
 *      firmwares use (RetroPie, Batocera, Onion, MinUI...).
 *   2. USB mass storage: hotplug detection without udev (netlink uevents +
 *      /sys/block), read-only mount under /media/usbN.
 *   3. Import: find every ROM library on a mounted stick (roms/<system>/,
 *      system folders, RetroStone2 backups, up to 4 levels deep), build a
 *      plan (counts, sizes, differing files, free space), then copy it to
 *      /data with progress, skipping identical files and asking before
 *      replacing a different one (Skip / Replace / Skip all / Replace all).
 *   3b. Backup: copy the console's library to a new folder on a stick
 *      (RetroStone2-YYYYMMDD-HHMMSS/) in the layout the import reads.
 *   4. Web share: a tiny HTTP server (one embedded page) for drag-and-drop
 *      uploads from a phone or PC browser, protected by a PIN.
 *   5. Name services: a minimal mDNS / LLMNR / NetBIOS name responder so
 *      "retrostone.local" and \\RETROSTONE resolve on the LAN.
 *   6. QR code encoder (byte mode) for showing the web share URL.
 *
 * Threads: the import and the web share each run in their own thread. The
 * UI never gets callbacks from those threads; it polls a status struct
 * (mutex protected) once per frame. Everything else is called from the UI
 * thread only.
 */
#ifndef RSOS_TRANSFER_H
#define RSOS_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRANSFER_PATH_MAX 1024
#define TRANSFER_SYS_MAX 48      /* max systems in a plan / change list */
#define TRANSFER_SYSID_MAX 24    /* "mastersystem" + NUL fits */

/* ================================================================ systems */

/* Number of canonical systems (the ROM folders under /data/roms). */
int transfer_system_count(void);
const char *transfer_system_id(int i);     /* "megadrive" */
const char *transfer_system_name(int i);   /* "Mega Drive / Genesis" */
/* Index of a canonical id, -1 if unknown. */
int transfer_system_index(const char *id);
/*
 * Maps a folder name found on a stick or card to a canonical id:
 * exact ids, aliases ("genesis" -> "megadrive", "mame2003" -> "arcade",
 * "FC" -> "nes") and MinUI-style "Game Boy (GB)" tags, case-insensitive.
 * NULL if unknown.
 */
const char *transfer_system_canon(const char *folder);

/* ============================================================ path safety */

/*
 * Checks one file or folder name received from the network or found on a
 * stick. 0 if acceptable, else a negative errno (-EINVAL, -ENAMETOOLONG).
 * Rejected: empty, "." and "..", a leading '.', a trailing space or dot,
 * control characters, / \ : * ? " < > |, invalid UTF-8, more than 255
 * UTF-16 units (the exFAT limit), and Windows device names (CON, NUL...).
 */
int transfer_name_check(const char *name);

/*
 * Checks a relative path ("sub/dir/Game (USA).sfc"): '/' separated, no
 * leading '/', no empty component, every component passes
 * transfer_name_check(), at most 8 levels and TRANSFER_PATH_MAX bytes.
 * 0 or a negative errno.
 */
int transfer_relpath_check(const char *rel);

/*
 * Makes a name found on a stick acceptable for exFAT/Windows: invalid
 * characters become '_', trailing dots and spaces are dropped. Returns 0
 * and writes out, or -EINVAL if nothing usable is left (e.g. "..").
 */
int transfer_name_sanitize(const char *in, char *out, size_t n);

/* ==================================================================== USB */

enum transfer_usb_event_type {
	TRANSFER_USB_MOUNTED,       /* a stick partition was mounted read-only */
	TRANSFER_USB_REMOVED,       /* it was pulled out (already unmounted) */
	TRANSFER_USB_EJECTED,       /* transfer_usb_eject() succeeded */
	TRANSFER_USB_UNSUPPORTED,   /* a USB disk with no FAT/exFAT/NTFS partition */
	TRANSFER_USB_MOUNT_FAILED,  /* supported filesystem, mount() failed (err) */
};

struct transfer_usb_drive {
	char dev[32];          /* "sda1" (or "sda" for a partitionless stick) */
	char disk[32];         /* "sda" */
	char mountpoint[64];   /* "/media/usb0" */
	char fstype[16];       /* "vfat", "exfat", "ntfs3" */
	char label[48];        /* volume label, UTF-8, may be empty */
	char vendor[48];       /* "SanDisk Cruzer" from sysfs, may be empty */
	uint64_t size_bytes;   /* partition size */
	int index;             /* N of /media/usbN */
};

struct transfer_usb_event {
	enum transfer_usb_event_type type;
	struct transfer_usb_drive drive;
	int err;               /* negative errno for MOUNT_FAILED */
};

/* Mount hook, for tests. Same contract as mount(2): 0 or -1 + errno. */
typedef int (*transfer_mount_fn)(const char *src, const char *target,
				 const char *fstype, unsigned long flags,
				 const char *data);
typedef int (*transfer_umount_fn)(const char *target, int flags);

struct transfer_usb_config {
	const char *mount_base;  /* default "/media" (a symlink to /run/media) */
	const char *sys_block;   /* default "/sys/block" */
	const char *dev_dir;     /* default "/dev" */
	int settle_ms;           /* wait after a disk appears, default 700 */
	int rescan_ms;           /* periodic /sys/block rescan when the uevent
				    socket is unavailable, default 1000 */
	bool no_netlink;         /* tests: do not open the uevent socket */
	bool watch_sys_block;    /* tests: an inotify watch on sys_block stands in
				    for the uevent socket (event driven, like
				    the device; implies no_netlink) */
	transfer_mount_fn mount_fn;    /* NULL = mount(2) */
	transfer_umount_fn umount_fn;  /* NULL = umount2(2) */
};

/*
 * Starts USB detection. Cheap (one socket, one readdir of /sys/block), but
 * call it after the first frame, not in the boot path. Sticks already
 * plugged in at boot are reported by the first transfer_usb_poll() calls
 * once they have settled. 0 or -errno.
 */
int transfer_usb_init(const struct transfer_usb_config *cfg);
/* The uevent socket to add to the main loop's poll() set (POLLIN), or -1. */
int transfer_usb_fd(void);
/*
 * Call when transfer_usb_fd() is readable and at least once a second (for
 * the settle delay). Returns 1 and fills ev for each pending event, 0 when
 * there is nothing more. Never blocks.
 */
int transfer_usb_poll(struct transfer_usb_event *ev);
/*
 * ms until transfer_usb_poll() must run again even without a readable fd:
 * a disk waiting for its settle delay (200), the /sys/block rescan when
 * there is no event fd, else -1. Add it to the poll() timeout.
 */
int transfer_usb_timeout_ms(void);
/* Currently mounted drives. Returns the count (<= max). */
int transfer_usb_drives(struct transfer_usb_drive *out, int max);
/* Unmounts one drive (read-only, so it is always safe to pull afterwards).
 * 0 or -errno (-EBUSY while an import reads from it). */
int transfer_usb_eject(const char *mountpoint);
/*
 * Remounts a drive read-write (for a backup) or back to read-only (MS_REMOUNT,
 * same options). 0 or -errno. Go back to read-only (or eject) as soon as the
 * writing is done: a read-only stick is always safe to pull.
 */
int transfer_usb_remount(const char *mountpoint, bool writable);
/* Unmounts everything and closes the socket. */
void transfer_usb_shutdown(void);

/*
 * Filesystem probe used by the detection (exposed for tests): reads the
 * boot sector of a block device or image. Returns "vfat", "exfat", "ntfs"
 * or NULL, and the volume label if label != NULL.
 */
const char *transfer_fs_probe(const char *path, char *label, size_t label_n);

/* ================================================================= import */

enum transfer_kind {
	TRANSFER_ROM,     /* roms/<sys>/<rel>          (gamelist.xml, media/ too) */
	TRANSFER_BIOS,    /* bios/<rel> */
	TRANSFER_SAVE,    /* saves/<sys>/<name>   (.srm .sav .rtc ... ) */
	TRANSFER_STATE,   /* states/<sys>/<name>  (.state .state1 .state.auto ...) */
	TRANSFER_THEME,   /* themes/<rel> */
	TRANSFER_SHOT,    /* screenshots/<rel> */
	TRANSFER_CONFIG,  /* rsos/coreopts/<rel>, rsos/remaps/<rel> (a "Back up saves" with
			     settings); imported with the saves, handled like them */
};

enum transfer_action {
	TRANSFER_NEW,       /* destination does not exist */
	TRANSFER_SAME,      /* identical (size + mtime, or content): skipped */
	TRANSFER_CHECK,     /* same size, other mtime, samples equal: compared
			       in full while copying */
	TRANSFER_REPLACE,   /* differs; ROM/BIOS/theme/screenshot (asked) */
	TRANSFER_CONFLICT,  /* differs; save/state (asked, .bak kept) */
};

struct transfer_plan_system {
	char id[TRANSFER_SYSID_MAX];
	char src_folder[64];     /* folder name on the stick ("genesis") */
	int files;               /* ROM files found (all actions) */
	int to_copy;             /* NEW + REPLACE + CHECK */
	uint64_t bytes;          /* bytes of to_copy */
	int saves;               /* saves + states found for this system */
};

struct transfer_item;        /* opaque */

struct transfer_plan {
	char src_root[TRANSFER_PATH_MAX];  /* the stick mount point */
	char dst_root[TRANSFER_PATH_MAX];  /* "/data" */
	char layout[128];        /* where ROMs were found: "roms", "RetroPie/roms",
				    "retropie-mount/roms", "(root)" */

	struct transfer_plan_system sys[TRANSFER_SYS_MAX];
	int nsys;

	/* totals per kind; "copy" = NEW + REPLACE + CHECK */
	int rom_files, rom_copy;       uint64_t rom_bytes;
	int bios_files, bios_copy;     uint64_t bios_bytes;
	int save_files, save_copy;     uint64_t save_bytes;
	int save_conflicts;            /* saves/states that differ from /data */
	int theme_files, theme_copy;   uint64_t theme_bytes;
	int shot_files, shot_copy;     uint64_t shot_bytes;
	int identical;                 /* already on /data, skipped */
	int replace;                   /* ROM/BIOS/theme files that differ (asked) */
	uint64_t total_bytes;          /* every file found, copied or not */

	uint64_t bytes_to_copy;        /* everything that could be written (new,
					  to check, differing incl. saves) */
	uint64_t dst_free;             /* free bytes on /data at plan time */
	bool fits;                     /* bytes_to_copy + reserve <= dst_free */

	/* folders that look like systems we have no core for, for the UI:
	   "dreamcast (132 files) not supported" */
	char unknown[16][64];
	int unknown_files[16];
	int nunknown;

	int skipped_names;             /* names that could not be made valid */

	struct transfer_item *items;   /* private */
	int nitems, cap;
};

/*
 * Scans src_root (a mounted stick) and compares with dst_root. Never writes.
 * Returns 0 and *out (free with transfer_plan_free), or -errno. A plan with
 * nothing to copy is valid (all zeros): the UI says "Nothing new on this
 * drive".
 */
int transfer_plan_build(const char *src_root, const char *dst_root,
			struct transfer_plan **out);
void transfer_plan_free(struct transfer_plan *p);

/*
 * The same scan in a thread (a big stick takes seconds on USB 2.0): start,
 * then poll once per frame. transfer_plan_poll returns 0 while scanning,
 * 1 with *out set when done, or a negative errno.
 */
int transfer_plan_start(const char *src_root, const char *dst_root);
int transfer_plan_poll(struct transfer_plan **out);

/*
 * ROM libraries on a stick. A folder (the stick root and up to 4 levels
 * below it, hidden and system folders skipped) is a library when it holds
 * roms/<known system>/ with files, or known system folders (or aliases) with
 * files, or is a RetroStone2 backup (a .rsos-backup or RetroStone2-backup.txt
 * marker, or a folder named RetroStone2 or RetroStone2-* with a roms/, saves/
 * states/ or bios/ folder). The stick root also
 * counts when it only has bios/, saves/, states/ or themes/ with files.
 * The folders a library consumes (roms, system folders, bios...) are not
 * searched again; its other subfolders are. Each library gets a summary
 * (systems, games, size) for the picker.
 */
#define TRANSFER_TREES_MAX 16
struct transfer_tree {
	char path[TRANSFER_PATH_MAX];   /* absolute */
	char rel[256];                  /* from the stick root, "" = the root */
	bool backup;                    /* a RetroStone2 backup folder */
	int64_t backup_time;            /* backups: last update (marker, else the folder
					   mtime), seconds since the epoch */
	int nsys;                       /* systems with games */
	char systems[96];               /* "snes, gba, megadrive..." */
	int games;                      /* ROM files */
	int files;                      /* every importable file */
	uint64_t bytes;                 /* their size */
};
/* Synchronous. Returns the number found (<= max), 0 if none, or -errno. */
int transfer_find_trees(const char *stick_root, struct transfer_tree *out, int max);
/* The same in a thread: poll returns 0 while scanning, 1 when done (*n set,
 * out filled), or -errno. */
int transfer_trees_start(const char *stick_root);
int transfer_trees_poll(struct transfer_tree *out, int max, int *n);

/*
 * A plan over the chosen libraries only (their own roms/, system folders,
 * bios/, saves/, states/, themes/, screenshots/; not the nested RetroPie-
 * style layouts, which the tree search finds as libraries of their own).
 * The same destination from two libraries: the first one wins.
 */
int transfer_plan_build_trees(const char *const *roots, int n, const char *dst_root,
			      struct transfer_plan **out);
int transfer_plan_start_trees(const char *const *roots, int n, const char *dst_root);

/*
 * "Play from USB without copying" (optional, see the doc): the system
 * folders found on a mounted stick, with the same layouts and aliases as
 * the import. A system may appear twice ("genesis" and "megadrive").
 * Returns the count (<= max).
 */
struct transfer_rom_dir {
	char system[TRANSFER_SYSID_MAX];
	char path[TRANSFER_PATH_MAX];
};
int transfer_find_rom_dirs(const char *src_root, struct transfer_rom_dir *out, int max);

/* What to do with a file that is already on /data with other content. */
enum transfer_dup {
	TRANSFER_DUP_ASK = 0,    /* ask (opts.ask, or the UI through the async API) */
	TRANSFER_DUP_SKIP,       /* keep the console's file */
	TRANSFER_DUP_REPLACE,    /* replace it (a save keeps the old one as .bak) */
};
enum transfer_answer {
	TRANSFER_ANSWER_SKIP = 0,
	TRANSFER_ANSWER_REPLACE,
	TRANSFER_ANSWER_SKIP_ALL,      /* skip this one and every next differing
					  file of the same class (saves or not) */
	TRANSFER_ANSWER_REPLACE_ALL,
};
/* One differing file, for the Skip / Replace / Skip all / Replace all prompt. */
struct transfer_question {
	int seq;                 /* 1, 2, 3... per import */
	enum transfer_kind kind;
	bool save;               /* a save, save state or settings file (precious:
				    the console's copy is kept as .bak) */
	char path[256];          /* destination, relative to /data */
	uint64_t src_size, dst_size;
	int64_t src_mtime, dst_mtime;   /* seconds since the epoch */
};
typedef enum transfer_answer (*transfer_ask_fn)(const struct transfer_question *q, void *user);

struct transfer_import_opts {
	bool roms;               /* default true (set by transfer_import_defaults) */
	bool bios;               /* true */
	bool saves;              /* true: saves and states (and settings files) */
	bool themes;             /* true */
	bool screenshots;        /* true (a backup's screenshots/) */
	/* Differing files. "All" answers are remembered separately for saves
	 * and for everything else, for the rest of the import. */
	enum transfer_dup dup_files;   /* ASK */
	enum transfer_dup dup_saves;   /* ASK */
	bool overwrite_saves;    /* legacy: true = dup_saves REPLACE */
	/* Synchronous runs: called for ASK. NULL: files are replaced and saves
	 * kept (the behaviour before the prompt existed). The async import asks
	 * the UI instead (progress.asking, transfer_import_answer()). */
	transfer_ask_fn ask;
	void *ask_user;
};
void transfer_import_defaults(struct transfer_import_opts *o);

enum transfer_state {
	TRANSFER_IDLE = 0,
	TRANSFER_RUNNING,
	TRANSFER_DONE,           /* finished (maybe with per-file failures) */
	TRANSFER_CANCELLED,
	TRANSFER_FAILED,         /* stopped: disk full, stick removed, ... */
};

struct transfer_progress {
	enum transfer_state state;
	int files_done, files_total;   /* items processed / to process */
	uint64_t bytes_done, bytes_total;
	char current[256];             /* relative destination path being copied */
	int copied;                    /* files written (new + replaced) */
	int replaced;                  /* of which over a different file */
	int identical;                 /* already there (same content): not written */
	int kept;                      /* differing files kept (Skip) */
	int skipped;                   /* identical + kept (+ too big for a backup) */
	int failed;
	int conflicts_kept;            /* saves kept (subset of kept) */
	int err;                       /* negative errno when FAILED */
	char errmsg[160];              /* human readable, for FAILED / last failure */
	uint32_t rate_kbs;             /* recent throughput */
	int eta_s;                     /* -1 unknown */
	int64_t elapsed_ms;            /* copying time (time spent asking excluded) */
	/* import: waiting for transfer_import_answer() about this file */
	bool asking;
	struct transfer_question question;
	/* backup: the folder written on the stick ("RetroStone2") */
	char folder[128];
	/* backup: the first files copied (for the "last played game" summary) */
	char copied_names[6][96];       /* (relative to the folder) */
	int ncopied_names;
};

typedef void (*transfer_progress_fn)(const struct transfer_progress *p, void *user);

/*
 * Synchronous import (tests, tools). cb (optional) is called from this
 * thread about every 64 KiB..1 MiB and after each file. *cancel != 0 stops
 * after the current block (the partial file is removed). Returns 0 or a
 * negative errno for a fatal error (ENOSPC, EIO...).
 */
int transfer_import_run(struct transfer_plan *p, const struct transfer_import_opts *o,
			transfer_progress_fn cb, void *user, volatile int *cancel,
			struct transfer_progress *result);

/*
 * Asynchronous import in a thread. Takes ownership of the plan (freed by
 * transfer_import_finish). Only one import at a time: -EBUSY otherwise.
 */
int transfer_import_start(struct transfer_plan *p, const struct transfer_import_opts *o);
/* Copies the current progress (any thread may be writing it). */
enum transfer_state transfer_import_status(struct transfer_progress *out);
/* Answers progress.question (while progress.asking). */
void transfer_import_answer(enum transfer_answer a);
void transfer_import_cancel(void);
/*
 * After DONE/CANCELLED/FAILED: joins the thread, frees the plan, and lists
 * the system ids whose ROM folders changed (for games_invalidate()).
 * Returns the number written to out (<= max). Returns 0 if nothing ran.
 */
int transfer_import_finish(char out[][TRANSFER_SYSID_MAX], int max);


/* ================================================================= backup */

/*
 * Console -> stick, into RetroStone2/ at the stick root (created the first
 * time), in the layout the import reads, so any RetroStone2 can import it:
 *   "Export games": roms/<system>/... and (option, default on) bios/.
 *   "Back up saves": saves/<system>/..., states/<system>/... (the host's
 *     .srm .rtc .state .stateN .state.auto and their .png thumbnails) and
 *     (option, default off) the settings: rsos/coreopts/ (core options, per
 *     core and per game) and rsos/remaps/. For every game, or only one (the
 *     last played).
 * Both are incremental: a file is copied only when it is missing or
 * different on the stick (games and BIOS: same size and mtime within 2 s,
 * the FAT resolution, means identical; saves and settings, small and
 * precious, are compared by content). Nothing is ever deleted on the stick.
 * A save or settings file replaced there keeps the previous stick version as
 * <name>.bak (one). Markers: .rsos-backup (console id, dates: how the import
 * recognises the folder) and RetroStone2-backup.txt (for humans).
 *
 * Every file is written under a temporary name, fsync'ed and renamed, so a
 * cancel, a full or a pulled stick leaves only complete files. The stick
 * must be remounted read-write first (transfer_usb_remount) and read-only
 * after.
 */
enum { TRANSFER_BK_ROMS = 0, TRANSFER_BK_BIOS, TRANSFER_BK_SAVES, TRANSFER_BK_CONFIG, TRANSFER_BK_N };

enum transfer_backup_mode {
	TRANSFER_EXPORT_GAMES = 0,   /* roms (+ bios) */
	TRANSFER_BACKUP_SAVES,       /* saves and states (+ settings) */
};

#define TRANSFER_BACKUP_DIR "RetroStone2"     /* the folder on the stick */
#define TRANSFER_BACKUP_MARKER ".rsos-backup"  /* in it: marks a backup */

struct transfer_backup_opts {
	enum transfer_backup_mode mode;
	bool bios;                  /* export: bios/ too (default on) */
	bool settings;              /* saves: core options and remaps too (default off) */
	/* saves: only this game's files ("Last played game"); "" = every game.
	 * The ROM path (absolute) and its ROM folder name: its files are
	 * saves/<system>/<stem>.*, states/<system>/<stem>.*, and with settings
	 * rsos/coreopts/<core>/<stem>.ini and rsos/remaps/<system>[-cz]/<stem>.ini. */
	char game_rom[TRANSFER_PATH_MAX];
	char game_system[TRANSFER_SYSID_MAX];
};
void transfer_backup_defaults(struct transfer_backup_opts *o, enum transfer_backup_mode mode);

#define TRANSFER_FAT32_MAX 0xffffffffull   /* largest file FAT32 can hold */

struct transfer_backup_info {
	enum transfer_backup_mode mode;
	char data_root[TRANSFER_PATH_MAX];
	char stick_root[TRANSFER_PATH_MAX];
	char folder[64];                 /* "RetroStone2" */
	bool exists;                     /* already on the stick (an update) */
	int files[TRANSFER_BK_N];        /* on the console */
	uint64_t bytes[TRANSFER_BK_N];
	bool fat32;                      /* the stick is FAT: no file > 4 GiB - 1 */
	int nbig;                        /* files too big for it */
	char big[8][128];                /* their names (the first 8) */
	uint64_t free_bytes;             /* free on the stick at scan time */
	uint32_t cluster;                /* the stick's allocation unit */
};

struct transfer_backup_totals {
	int files;                       /* files to copy (missing or different) */
	uint64_t bytes;
	int replace;                     /* of which already there but different */
	int unchanged;                   /* already on the stick, identical */
	uint64_t unchanged_bytes;
	uint64_t need;                   /* space needed on the stick (+ margin) */
	int too_big;                     /* skipped: over the FAT32 limit */
	bool fits;
};

struct transfer_backup;                  /* opaque: info + the file list */

/*
 * Lists what /data holds for the mode and compares it with RetroStone2/ on
 * the stick (a stat per file; the content of saves and settings of the same
 * size). fstype is the stick's ("vfat" = FAT32 limit). Never writes.
 */
int transfer_backup_scan(const char *data_root, const char *stick_root, const char *fstype,
			 enum transfer_backup_mode mode, struct transfer_backup **out);
const struct transfer_backup_info *transfer_backup_get_info(const struct transfer_backup *b);
void transfer_backup_totals(const struct transfer_backup *b, const struct transfer_backup_opts *o,
			    struct transfer_backup_totals *t);
void transfer_backup_free(struct transfer_backup *b);
/* The scan in a thread: poll returns 0 while running, 1 with *out, -errno. */
int transfer_backup_scan_start(const char *data_root, const char *stick_root, const char *fstype,
			       enum transfer_backup_mode mode);
int transfer_backup_scan_poll(struct transfer_backup **out);

/* Synchronous (tests). Same contract as transfer_import_run. A full stick
 * stops it (FAILED, -ENOSPC, "The USB drive is full") with the partial file
 * removed; progress.copied tells what was saved, progress.identical what was
 * already there. opts.mode must be the scan's. */
int transfer_backup_run(struct transfer_backup *b, const struct transfer_backup_opts *o,
			transfer_progress_fn cb, void *user, volatile int *cancel,
			struct transfer_progress *result);
/* Asynchronous: takes ownership of b. -EBUSY while an import or another
 * backup runs. */
int transfer_backup_start(struct transfer_backup *b, const struct transfer_backup_opts *o);
enum transfer_state transfer_backup_status(struct transfer_progress *out);
void transfer_backup_cancel(void);
/* After DONE/CANCELLED/FAILED: joins the thread and frees the backup. */
void transfer_backup_finish(void);

/* ============================================================== web share */

struct webshare_config {
	const char *data_root;   /* default "/data" */
	uint16_t port;           /* default 80 */
	const char *bind_addr;   /* IPv4 literal, default any (0.0.0.0) */
	const char *hostname;    /* default "retrostone" (Host header check) */
	const char *pin;         /* NULL: a fresh random 6-digit PIN */
	int idle_timeout_s;      /* stop after this long with no request; 0 = never.
				    Default (-1): 1800 */
	int max_clients;         /* concurrent connections, default 4 */
	bool allow_public_peers; /* tests only: accept non-private client IPs */
};

struct webshare_status {
	bool running;
	uint16_t port;
	char pin[12];
	char urls[4][64];        /* "http://192.168.1.23/" per interface */
	int nurls;
	char mdns_url[96];       /* "http://retrostone.local/" (63-char host + port) */
	char qr_text[96];        /* what to encode in the QR code: first URL +
				    "#pin=NNNNNN" (auto-login) */
	int clients;             /* open connections */
	int sessions;            /* browsers that entered the right PIN */
	uint64_t bytes_received;
	int files_received, files_deleted;
	char current[256];       /* upload in progress, "" if none */
	uint64_t cur_done, cur_total;
	int auth_failures;
	int lockout_s;           /* > 0 while wrong-PIN lockout is active */
	char stop_reason[64];    /* why it stopped by itself ("idle", error...) */
};

/* Binds and starts the server thread. 0 or -errno (-EADDRINUSE, -EACCES). */
int webshare_start(const struct webshare_config *cfg);
/* Stops accepting, aborts uploads in progress (partial files removed),
 * joins the thread. Safe to call when not running. */
void webshare_stop(void);
bool webshare_running(void);
void webshare_get_status(struct webshare_status *st);
/* Refreshes urls[] (call after the IP changed). */
void webshare_refresh_urls(void);
/* System ids whose ROM folders changed through the web share since the last
 * call ("bios" and "themes" are reported as such). Returns the count. */
int webshare_take_changes(char out[][TRANSFER_SYSID_MAX], int max);

/* ============================================================ name service */

struct netnames_config {
	const char *hostname;    /* default "retrostone" */
	bool mdns, llmnr, nbns;  /* which responders (all true by default via
				    netnames_defaults) */
	uint16_t mdns_port, llmnr_port, nbns_port;  /* 0 = 5353, 5355, 137 */
	bool no_multicast;       /* tests: do not join groups */
};
void netnames_defaults(struct netnames_config *c);
int netnames_start(const struct netnames_config *c);
void netnames_stop(void);
bool netnames_running(void);

enum netnames_proto { NETNAMES_MDNS, NETNAMES_LLMNR, NETNAMES_NBNS };
/*
 * Builds the reply to one query packet (exposed for tests). ip is the IPv4
 * address to answer with (network byte order). Returns the reply length,
 * 0 when the packet is not a query for our name, -1 on malformed input.
 * *unicast is set when the reply must go back to the sender only.
 */
int netnames_reply(enum netnames_proto proto, const uint8_t *q, size_t qlen,
		   uint32_t ip, const char *hostname, bool src_port_is_mdns,
		   uint8_t *out, size_t outmax, bool *unicast);

/* ================================================================ QR code */

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE (17 + 4 * QR_MAX_VERSION)   /* 57 modules */

struct qr_code {
	int version;         /* 1..10 */
	int size;            /* modules per side (17 + 4 * version) */
	int mask;
	uint8_t m[QR_MAX_SIZE][QR_MAX_SIZE];   /* 1 = dark, [y][x] */
};

/* Encodes text (byte mode, ECC level M, smallest version that fits, best
 * mask). 0 or -EMSGSIZE (> 213 bytes). Draw with a 4-module light border. */
int qr_encode(const char *text, struct qr_code *qr);

#endif
