/*
 * import.c - "Import from USB": find the ROM libraries on a mounted stick,
 * build a plan, copy.
 *
 * Layouts recognised by transfer_plan_build() on the stick (every name
 * case-insensitive):
 *   roms/<system>/...                  ours, Batocera/KNULLI "share", EmuDeck
 *   RetroPie/roms/<system>/...         a copy of a RetroPie home folder
 *   retropie-mount/roms/<system>/...   RetroPie's "USB ROM service" layout
 *   share/roms, home/pi/RetroPie/roms, userdata/roms, Roms (Onion/MinUI)
 *   <system>/...                       system folders at the stick root
 * The UI instead searches the stick for libraries (transfer_find_trees(),
 * which finds those nested layouts as libraries of their own) and plans over
 * the chosen ones (transfer_plan_build_trees(): their direct contents only).
 * <system> may be any alias known to sysmap.c ("genesis", "SFC",
 * "Game Boy (GB)"...). BIOS: bios/, BIOS/, RetroPie/BIOS/,
 * retropie-mount/BIOS/, share/bios/. Our own saves/<system>/,
 * states/<system>/ and screenshots/ folders (a backup) and themes/ are
 * imported too.
 *
 * Saves and states found next to the ROMs (RetroPie kept .srm/.state
 * there) go to /data/saves/<system>/ and /data/states/<system>/.
 *
 * Copy rules: identical files (same size and mtime within 2 s, or same
 * content) are skipped and counted as "already there". A file that exists
 * with other content is asked about (Skip / Replace / Skip all / Replace
 * all; "all" is remembered separately for saves and for everything else);
 * a replaced save or state keeps the old one as <name>.bak.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tr_internal.h"

#define RESERVE_BYTES (64ull << 20)   /* keep free on /data for saves/states */
#define MAX_DEPTH 6
#define TREE_DEPTH 4                  /* library search below the stick root */

struct transfer_item {
	char *src;           /* absolute */
	char *dst;           /* relative to dst_root */
	uint64_t size;
	int64_t mtime_s;
	long mtime_ns;
	int seq;
	int16_t sys;         /* index in plan->sys[], -1 for BIOS/themes */
	uint8_t kind;        /* enum transfer_kind */
	uint8_t action;      /* enum transfer_action */
	bool game;           /* a ROM that is a game (not gamelist.xml, media...) */
};

static bool savelike(enum transfer_kind k)
{
	return k == TRANSFER_SAVE || k == TRANSFER_STATE || k == TRANSFER_CONFIG;
}

/* Files of a ROM folder that are not games: gamelist.xml, scraped media,
 * notes. rel is the path inside the system folder. */
static bool is_game_file(const char *rel)
{
	static const char *const media[] = {
		"media", "images", "videos", "snap", "snaps", "manuals", "downloaded_images",
		"downloaded_videos", "boxart", "marquees", "wheel", "titles", "screenshots",
	};
	static const char *const ext[] = {
		"xml", "txt", "png", "jpg", "jpeg", "gif", "bmp", "webp", "mp4", "mkv", "avi", "pdf",
		"nfo", "db", "ini", "cfg",
	};
	const char *base = strrchr(rel, '/'), *dot;

	for (const char *p = rel; p && p < (base ? base : rel); ) {
		size_t l = strcspn(p, "/");

		for (size_t i = 0; i < sizeof(media) / sizeof(media[0]); i++)
			if (strlen(media[i]) == l && !strncasecmp(p, media[i], l))
				return false;
		p = p[l] ? p + l + 1 : NULL;
	}
	base = base ? base + 1 : rel;
	dot = strrchr(base, '.');
	if (dot)
		for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
			if (tr_str_ieq(dot + 1, ext[i]))
				return false;
	return true;
}

/* ------------------------------------------------------------ plan build */

static int add_item(struct transfer_plan *p, const char *src, const char *dst,
		    const struct stat *st, enum transfer_kind kind, int sys)
{
	struct transfer_item *it;

	if (p->nitems == p->cap) {
		int cap = p->cap ? p->cap * 2 : 256;
		struct transfer_item *n = realloc(p->items, sizeof(*n) * (size_t)cap);

		if (!n)
			return -ENOMEM;
		p->items = n;
		p->cap = cap;
	}
	it = &p->items[p->nitems];
	memset(it, 0, sizeof(*it));
	it->src = strdup(src);
	it->dst = strdup(dst);
	if (!it->src || !it->dst) {
		free(it->src);
		free(it->dst);
		return -ENOMEM;
	}
	it->size = (uint64_t)st->st_size;
	it->mtime_s = st->st_mtim.tv_sec;
	it->mtime_ns = st->st_mtim.tv_nsec;
	it->kind = (uint8_t)kind;
	it->sys = (int16_t)sys;
	it->seq = p->nitems;
	p->nitems++;
	return 0;
}

/* Finds a child of dir whose name matches name case-insensitively. */
static bool find_ci(const char *dir, const char *name, char *out, size_t n)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	bool ok = false;

	if (!d)
		return false;
	while ((de = readdir(d))) {
		if (tr_str_ieq(de->d_name, name)) {
			ok = tr_snprintf(out, n, "%s/%s", dir, de->d_name) == 0;
			break;
		}
	}
	closedir(d);
	return ok;
}

/* Resolves "a/b/c" under base, case-insensitively, to an existing dir. */
static bool resolve_dir_ci(const char *base, const char *rel, char *out, size_t n)
{
	char cur[TRANSFER_PATH_MAX], comp[128];
	const char *p = rel;
	struct stat st;

	tr_strlcpy(cur, base, sizeof(cur));
	while (*p) {
		size_t l = strcspn(p, "/");
		char next[TRANSFER_PATH_MAX];

		if (l >= sizeof(comp))
			return false;
		memcpy(comp, p, l);
		comp[l] = 0;
		p += l;
		if (*p == '/')
			p++;
		if (!find_ci(cur, comp, next, sizeof(next)))
			return false;
		/* every component a real folder: a symlink on the stick
		 * ("bios -> ../../..") never leads out of it (review F-M16) */
		if (lstat(next, &st) < 0 || !S_ISDIR(st.st_mode))
			return false;
		tr_strlcpy(cur, next, sizeof(cur));
	}
	/* base itself is ours (the mount point, or a folder already checked) */
	if (!*rel && (stat(cur, &st) < 0 || !S_ISDIR(st.st_mode)))
		return false;
	tr_strlcpy(out, cur, n);
	return true;
}

static int plan_sys_index(struct transfer_plan *p, const char *id, const char *src_folder)
{
	for (int i = 0; i < p->nsys; i++)
		if (!strcmp(p->sys[i].id, id))
			return i;
	if (p->nsys == TRANSFER_SYS_MAX)
		return -1;
	memset(&p->sys[p->nsys], 0, sizeof(p->sys[0]));
	tr_strlcpy(p->sys[p->nsys].id, id, sizeof(p->sys[0].id));
	tr_strlcpy(p->sys[p->nsys].src_folder, src_folder, sizeof(p->sys[0].src_folder));
	return p->nsys++;
}

/* Where a ROM-folder file goes: roms/<id>/<rel>, or saves/states. */
static int walk(struct transfer_plan *p, const char *src_dir, const char *dst_prefix,
		const char *rel, int depth, enum transfer_kind kind, int sys, const char *sys_id)
{
	DIR *d;
	struct dirent *de;
	int r = 0;

	if (depth > MAX_DEPTH)
		return 0;
	d = opendir(src_dir);
	if (!d)
		return 0;
	while ((de = readdir(d)) && r == 0) {
		char src[TRANSFER_PATH_MAX], name[256], nrel[TRANSFER_PATH_MAX], dst[TRANSFER_PATH_MAX];
		struct stat st;

		if (tr_is_junk(de->d_name))
			continue;
		if (tr_snprintf(src, sizeof(src), "%s/%s", src_dir, de->d_name) < 0 ||
		    lstat(src, &st) < 0)
			continue;
		if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
			continue;                        /* no symlinks, devices... */
		if (transfer_name_sanitize(de->d_name, name, sizeof(name)) < 0) {
			p->skipped_names++;
			continue;
		}
		if (tr_snprintf(nrel, sizeof(nrel), "%s%s%s", rel, rel[0] ? "/" : "", name) < 0) {
			p->skipped_names++;
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			r = walk(p, src, dst_prefix, nrel, depth + 1, kind, sys, sys_id);
			continue;
		}
		if (savelike(kind) && strlen(name) > 4 && tr_str_ieq(name + strlen(name) - 4, ".bak"))
			continue;                        /* a backup's safety copies stay there */
		if (kind == TRANSFER_ROM && tr_is_save_name(name)) {
			if (tr_snprintf(dst, sizeof(dst), "saves/%s/%s", sys_id, name) == 0)
				r = add_item(p, src, dst, &st, TRANSFER_SAVE, sys);
		} else if (kind == TRANSFER_ROM && tr_is_state_name(name)) {
			if (tr_snprintf(dst, sizeof(dst), "states/%s/%s", sys_id, name) == 0)
				r = add_item(p, src, dst, &st, TRANSFER_STATE, sys);
		} else if (tr_snprintf(dst, sizeof(dst), "%s/%s", dst_prefix, nrel) == 0 &&
			   transfer_relpath_check(dst) == 0) {
			r = add_item(p, src, dst, &st, kind, sys);
			if (r == 0 && kind == TRANSFER_ROM)
				p->items[p->nitems - 1].game = is_game_file(nrel);
		} else {
			p->skipped_names++;
		}
	}
	closedir(d);
	return r;
}

/* Regular non-junk files below dir (up to 3 levels), stops at limit. */
static int count_files_n(const char *dir, int depth, int limit)
{
	DIR *d;
	struct dirent *de;
	int n = 0;

	if (depth > 3 || !(d = opendir(dir)))
		return 0;
	while ((de = readdir(d)) && n < limit) {
		char p[TRANSFER_PATH_MAX];
		struct stat st;

		if (tr_is_junk(de->d_name) || tr_snprintf(p, sizeof(p), "%s/%s", dir, de->d_name) < 0 ||
		    lstat(p, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode))
			n += count_files_n(p, depth + 1, limit - n);
		else if (S_ISREG(st.st_mode))
			n++;
	}
	closedir(d);
	return n;
}

static int count_files(const char *dir, int depth)
{
	return count_files_n(dir, depth, 100000);
}

/* Scans a folder whose children are system folders. Returns the number of
 * system folders recognised. */
static int scan_systems_dir(struct transfer_plan *p, const char *dir, bool record_unknown,
			    enum transfer_kind kind_override, int *err)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int found = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)) && !*err) {
		char src[TRANSFER_PATH_MAX], prefix[64];
		const char *id;
		struct stat st;
		int si;

		if (tr_is_junk(de->d_name) ||
		    tr_snprintf(src, sizeof(src), "%s/%s", dir, de->d_name) < 0 ||
		    lstat(src, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;
		id = transfer_system_canon(de->d_name);
		if (!id) {
			if (record_unknown && p->nunknown < 16) {
				int n = count_files(src, 0);

				if (n > 0) {
					tr_strlcpy(p->unknown[p->nunknown], de->d_name,
						   sizeof(p->unknown[0]));
					p->unknown_files[p->nunknown++] = n;
				}
			}
			continue;
		}
		si = plan_sys_index(p, id, de->d_name);
		found++;
		if (kind_override == TRANSFER_SAVE)
			snprintf(prefix, sizeof(prefix), "saves/%s", id);
		else if (kind_override == TRANSFER_STATE)
			snprintf(prefix, sizeof(prefix), "states/%s", id);
		else
			snprintf(prefix, sizeof(prefix), "roms/%s", id);
		*err = walk(p, src, prefix, "", 0, kind_override, si, id);
	}
	closedir(d);
	return found;
}

static int cmp_dst(const void *a, const void *b)
{
	const struct transfer_item *x = a, *y = b;
	int c = strcasecmp(x->dst, y->dst);   /* exFAT is case-insensitive */

	return c ? c : x->seq - y->seq;
}

static void classify(struct transfer_plan *p, struct transfer_item *it)
{
	char path[TRANSFER_PATH_MAX * 2];
	struct stat st;
	bool save = savelike(it->kind);
	int a, b, same;

	snprintf(path, sizeof(path), "%s/%s", p->dst_root, it->dst);
	if (lstat(path, &st) < 0) {
		it->action = TRANSFER_NEW;
		return;
	}
	if ((uint64_t)st.st_size != it->size) {
		it->action = save ? TRANSFER_CONFLICT : TRANSFER_REPLACE;
		return;
	}
	{
		int64_t dt = (int64_t)st.st_mtim.tv_sec - it->mtime_s;

		if (dt >= -2 && dt <= 2) {
			it->action = TRANSFER_SAME;
			return;
		}
	}
	/* Same size, other date: look at the content. Saves are small and
	 * precious: compared in full now. Other files: 16 samples now (a
	 * difference there is certain), a full compare while copying when the
	 * samples match. */
	a = open(path, O_RDONLY | O_CLOEXEC);
	b = open(it->src, O_RDONLY | O_CLOEXEC);
	if (a < 0 || b < 0)
		same = -EIO;
	else if (save && it->size <= (16u << 20))
		same = tr_same_content(a, b);
	else
		same = tr_same_sampled(a, b, it->size);
	if (a >= 0)
		close(a);
	if (b >= 0)
		close(b);
	if (same == 0)
		it->action = save ? TRANSFER_CONFLICT : TRANSFER_REPLACE;
	else if (same == 1 && save && it->size <= (16u << 20))
		it->action = TRANSFER_SAME;
	else
		it->action = TRANSFER_CHECK;     /* samples equal, or unreadable now */
}

static const char *const g_rom_roots[] = {
	"roms", "RetroPie/roms", "retropie-mount/roms", "share/roms",
	"home/pi/RetroPie/roms", "userdata/roms", NULL,
};
static const char *const g_bios_roots[] = {
	"bios", "RetroPie/BIOS", "retropie-mount/BIOS", "share/bios",
	"home/pi/RetroPie/BIOS", "userdata/bios", NULL,
};

static struct transfer_plan *plan_new(const char *src_root, const char *dst_root)
{
	struct transfer_plan *p = calloc(1, sizeof(*p));

	if (!p)
		return NULL;
	tr_strlcpy(p->src_root, src_root, sizeof(p->src_root));
	tr_strlcpy(p->dst_root, dst_root ? dst_root : "", sizeof(p->dst_root));
	return p;
}

/*
 * Adds one root's files to the plan. layouts: also the nested layouts of
 * g_rom_roots/g_bios_roots (transfer_plan_build); else only the root's own
 * roms/, system folders and bios/ (a library found by the tree search).
 */
static int plan_scan_root(struct transfer_plan *p, const char *root, bool layouts, const char *label)
{
	char dir[TRANSFER_PATH_MAX];
	int err = 0;
	struct stat st;

	/* the root is the mount point (a symlink in the headless tests) or a
	 * folder the tree search found with lstat(): what is under it is
	 * checked component by component (resolve_dir_ci, walk) */
	if (stat(root, &st) < 0 || !S_ISDIR(st.st_mode))
		return -ENOENT;
	for (int i = 0; g_rom_roots[i] && !err && (layouts || i == 0); i++) {
		if (!resolve_dir_ci(root, g_rom_roots[i], dir, sizeof(dir)))
			continue;
		if (scan_systems_dir(p, dir, true, TRANSFER_ROM, &err) > 0 && !p->layout[0])
			snprintf(p->layout, sizeof(p->layout), "%s%s%s", label, label[0] ? "/" : "",
				 g_rom_roots[i]);
	}
	/* system folders at the root */
	if (!err && scan_systems_dir(p, root, false, TRANSFER_ROM, &err) > 0 && !p->layout[0])
		tr_strlcpy(p->layout, label[0] ? label : "(root)", sizeof(p->layout));

	for (int i = 0; g_bios_roots[i] && !err && (layouts || i == 0); i++)
		if (resolve_dir_ci(root, g_bios_roots[i], dir, sizeof(dir)))
			err = walk(p, dir, "bios", "", 0, TRANSFER_BIOS, -1, NULL);
	if (!err && resolve_dir_ci(root, "saves", dir, sizeof(dir)))
		scan_systems_dir(p, dir, false, TRANSFER_SAVE, &err);
	if (!err && resolve_dir_ci(root, "states", dir, sizeof(dir)))
		scan_systems_dir(p, dir, false, TRANSFER_STATE, &err);
	if (!err && resolve_dir_ci(root, "themes", dir, sizeof(dir)))
		err = walk(p, dir, "themes", "", 0, TRANSFER_THEME, -1, NULL);
	if (!err && resolve_dir_ci(root, "screenshots", dir, sizeof(dir)))
		err = walk(p, dir, "screenshots", "", 0, TRANSFER_SHOT, -1, NULL);
	/* a "Back up saves" with its settings: core options and remaps */
	if (!err && resolve_dir_ci(root, "rsos/coreopts", dir, sizeof(dir)))
		err = walk(p, dir, "rsos/coreopts", "", 0, TRANSFER_CONFIG, -1, NULL);
	if (!err && resolve_dir_ci(root, "rsos/remaps", dir, sizeof(dir)))
		err = walk(p, dir, "rsos/remaps", "", 0, TRANSFER_CONFIG, -1, NULL);
	return err;
}

/* Dedupe by destination, classify against dst_root (if classify) and
 * count. */
static void plan_finish(struct transfer_plan *p, bool classify_dst)
{
	int w = 0;

	/* The same destination from two source folders ("genesis" and
	 * "megadrive", or two libraries): keep the first one found. */
	if (p->nitems > 1) {
		qsort(p->items, (size_t)p->nitems, sizeof(p->items[0]), cmp_dst);
		for (int r = 0; r < p->nitems; r++) {
			if (w > 0 && !strcasecmp(p->items[w - 1].dst, p->items[r].dst)) {
				free(p->items[r].src);
				free(p->items[r].dst);
				continue;
			}
			p->items[w++] = p->items[r];
		}
		p->nitems = w;
	}

	for (int i = 0; i < p->nitems; i++) {
		struct transfer_item *it = &p->items[i];
		bool copy;

		if (classify_dst)
			classify(p, it);
		else
			it->action = TRANSFER_NEW;
		copy = it->action == TRANSFER_NEW || it->action == TRANSFER_REPLACE ||
		       it->action == TRANSFER_CHECK;
		p->total_bytes += it->size;
		if (it->action == TRANSFER_SAME)
			p->identical++;
		if (it->action == TRANSFER_REPLACE)
			p->replace++;
		switch (it->kind) {
		case TRANSFER_ROM:
			/* games only in the counts; gamelist.xml and media in the bytes */
			p->rom_files += it->game;
			if (it->sys >= 0) {
				p->sys[it->sys].files += it->game;
				if (copy) {
					p->sys[it->sys].to_copy += it->game;
					p->sys[it->sys].bytes += it->size;
				}
			}
			if (copy) {
				p->rom_copy++;
				p->rom_bytes += it->size;
			}
			break;
		case TRANSFER_BIOS:
			p->bios_files++;
			if (copy) {
				p->bios_copy++;
				p->bios_bytes += it->size;
			}
			break;
		case TRANSFER_SAVE:
		case TRANSFER_STATE:
		case TRANSFER_CONFIG:
			p->save_files++;
			if (it->sys >= 0)
				p->sys[it->sys].saves++;
			if (copy) {
				p->save_copy++;
				p->save_bytes += it->size;
			}
			if (it->action == TRANSFER_CONFLICT)
				p->save_conflicts++;
			break;
		case TRANSFER_THEME:
			p->theme_files++;
			if (copy) {
				p->theme_copy++;
				p->theme_bytes += it->size;
			}
			break;
		case TRANSFER_SHOT:
			p->shot_files++;
			if (copy) {
				p->shot_copy++;
				p->shot_bytes += it->size;
			}
			break;
		}
		/* differing saves count too: the user may replace them */
		if (copy || it->action == TRANSFER_CONFLICT)
			p->bytes_to_copy += it->size;
	}
	if (classify_dst) {
		p->dst_free = tr_free_bytes(p->dst_root);
		p->fits = p->bytes_to_copy + RESERVE_BYTES <= p->dst_free;
	}
}

static void log_plan(const struct transfer_plan *p, int nroots, int64_t t0)
{
	char a[32], b[32];

	tr_fmt_bytes(p->bytes_to_copy, a, sizeof(a));
	tr_fmt_bytes(p->dst_free, b, sizeof(b));
	tr_log("plan: %d folder%s from %s: %d systems, %d games (%d to copy), bios %d, saves %d "
	       "(%d differ), themes %d, screenshots %d; %d already there, %d differ; %s to copy, "
	       "%s free%s; %lld ms",
	       nroots, nroots == 1 ? "" : "s", p->src_root, p->nsys, p->rom_files, p->rom_copy,
	       p->bios_files, p->save_files, p->save_conflicts, p->theme_files, p->shot_files,
	       p->identical, p->replace + p->save_conflicts, a, b, p->fits ? "" : " (does not fit)",
	       (long long)(tr_now_ms() - t0));
}

int transfer_plan_build(const char *src_root, const char *dst_root, struct transfer_plan **out)
{
	struct transfer_plan *p;
	int64_t t0 = tr_now_ms();
	int err;

	*out = NULL;
	if (!(p = plan_new(src_root, dst_root)))
		return -ENOMEM;
	err = plan_scan_root(p, src_root, true, "");
	if (err) {
		transfer_plan_free(p);
		return err;
	}
	plan_finish(p, true);
	log_plan(p, 1, t0);
	*out = p;
	return 0;
}

int transfer_plan_build_trees(const char *const *roots, int n, const char *dst_root,
			      struct transfer_plan **out)
{
	struct transfer_plan *p;
	int64_t t0 = tr_now_ms();
	int err = 0, ok = 0;

	*out = NULL;
	if (n < 1)
		return -EINVAL;
	if (!(p = plan_new(roots[0], dst_root)))
		return -ENOMEM;
	for (int i = 0; i < n && (!err || err == -ENOENT); i++) {
		const char *label = strrchr(roots[i], '/');

		err = plan_scan_root(p, roots[i], false, n > 1 && label ? label + 1 : "");
		ok += err == 0;
	}
	if ((err && err != -ENOENT) || !ok) {
		transfer_plan_free(p);
		return err ? err : -ENOENT;
	}
	plan_finish(p, true);
	log_plan(p, n, t0);
	*out = p;
	return 0;
}

static int find_in(const char *dir, struct transfer_rom_dir *out, int n, int max)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d)
		return n;
	while ((de = readdir(d)) && n < max) {
		const char *id;
		struct stat st;

		if (tr_is_junk(de->d_name) || !(id = transfer_system_canon(de->d_name)) ||
		    tr_snprintf(out[n].path, sizeof(out[n].path), "%s/%s", dir, de->d_name) < 0 ||
		    lstat(out[n].path, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;
		tr_strlcpy(out[n].system, id, sizeof(out[n].system));
		n++;
	}
	closedir(d);
	return n;
}

int transfer_find_rom_dirs(const char *src_root, struct transfer_rom_dir *out, int max)
{
	char dir[TRANSFER_PATH_MAX];
	int n = 0;

	for (int i = 0; g_rom_roots[i]; i++)
		if (resolve_dir_ci(src_root, g_rom_roots[i], dir, sizeof(dir)))
			n = find_in(dir, out, n, max);
	return find_in(src_root, out, n, max);
}

void transfer_plan_free(struct transfer_plan *p)
{
	if (!p)
		return;
	for (int i = 0; i < p->nitems; i++) {
		free(p->items[i].src);
		free(p->items[i].dst);
	}
	free(p->items);
	free(p);
}

/* ------------------------------------------------------------ trees */

/* Folders never searched for libraries (besides dotfiles and tr_is_junk). */
static bool skip_folder(const char *name)
{
	static const char *const sys[] = {
		"LOST.DIR", "Android", "DCIM", "$Extend", "FOUND.001", "FOUND.002", "Windows",
		"Program Files", "Program Files (x86)", "ProgramData", "System", "boot", "EFI",
	};

	if (tr_is_junk(name))
		return true;
	for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++)
		if (tr_str_ieq(name, sys[i]))
			return true;
	return false;
}

/* Known system folders among dir's children that hold at least one file. */
static int systems_with_files(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		char p[TRANSFER_PATH_MAX];
		struct stat st;

		if (tr_is_junk(de->d_name) || !transfer_system_canon(de->d_name) ||
		    tr_snprintf(p, sizeof(p), "%s/%s", dir, de->d_name) < 0 || lstat(p, &st) < 0 ||
		    !S_ISDIR(st.st_mode))
			continue;
		if (count_files_n(p, 0, 1) > 0)
			n++;
	}
	closedir(d);
	return n;
}

static bool has_child_with_files(const char *dir, const char *name)
{
	char p[TRANSFER_PATH_MAX];

	return resolve_dir_ci(dir, name, p, sizeof(p)) && count_files_n(p, 0, 1) > 0;
}

/* A RetroStone2 backup: its markers, or its name (RetroStone2,
 * RetroStone2-YYYYMMDD-...) with the folders a backup has. */
static bool is_backup_dir(const char *dir, const char *name, int64_t *when)
{
	char p[TRANSFER_PATH_MAX];
	struct stat st;
	bool yes = false;

	*when = 0;
	if (find_ci(dir, TRANSFER_BACKUP_MARKER, p, sizeof(p))) {
		/* "updated=<seconds>"; a regular file only, never followed
		 * (review F-M16: a symlink to /proc/kmsg blocked the scan) */
		int fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		FILE *f = NULL;
		char line[128];

		if (fd >= 0 && (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || !(f = fdopen(fd, "r"))))
			close(fd);

		while (f && fgets(line, sizeof(line), f))
			if (!strncmp(line, "updated=", 8))
				*when = atoll(line + 8);
		if (f)
			fclose(f);
		yes = true;
	} else if (find_ci(dir, "RetroStone2-backup.txt", p, sizeof(p))) {
		yes = true;
	} else if (tr_str_ieq(name, TRANSFER_BACKUP_DIR) || !strncasecmp(name, "RetroStone2-", 12)) {
		yes = resolve_dir_ci(dir, "roms", p, sizeof(p)) || resolve_dir_ci(dir, "saves", p, sizeof(p)) ||
		      resolve_dir_ci(dir, "states", p, sizeof(p)) || resolve_dir_ci(dir, "bios", p, sizeof(p));
	}
	if (yes && !*when && stat(dir, &st) == 0)
		*when = st.st_mtim.tv_sec;
	return yes;
}

/* Children a library consumes: not searched again. */
static bool consumed(const char *name)
{
	static const char *const own[] = { "roms", "bios", "saves", "states", "themes", "screenshots", "rsos" };

	for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++)
		if (tr_str_ieq(name, own[i]))
			return true;
	return transfer_system_canon(name) != NULL;
}

struct tree_search {
	struct transfer_tree *out;
	int max, n;
};

static void summarize(struct transfer_tree *t)
{
	struct transfer_plan *p = plan_new(t->path, NULL);
	size_t o = 0;

	if (!p)
		return;
	if (plan_scan_root(p, t->path, false, "") == 0) {
		plan_finish(p, false);
		t->games = p->rom_files;
		t->files = p->nitems;
		t->bytes = p->total_bytes;
		for (int i = 0; i < p->nsys; i++) {
			if (!p->sys[i].files)
				continue;
			t->nsys++;
			if (o + strlen(p->sys[i].id) + 6 < sizeof(t->systems))
				o += (size_t)snprintf(t->systems + o, sizeof(t->systems) - o, "%s%s",
						      o ? ", " : "", p->sys[i].id);
			else if (o + 4 < sizeof(t->systems) && !strstr(t->systems, "..."))
				o += (size_t)snprintf(t->systems + o, sizeof(t->systems) - o, "...");
		}
	}
	transfer_plan_free(p);
}

static void tree_walk(struct tree_search *s, const char *dir, const char *rel, const char *name,
		      int depth)
{
	bool is_tree = false, backup = false;
	char roms[TRANSFER_PATH_MAX];
	int64_t when = 0;
	DIR *d;
	struct dirent *de;

	if (s->n >= s->max)
		return;
	if (depth > 0 && is_backup_dir(dir, name, &when)) {
		is_tree = backup = true;
	} else if (resolve_dir_ci(dir, "roms", roms, sizeof(roms)) && systems_with_files(roms) > 0) {
		is_tree = true;
	} else if (systems_with_files(dir) > 0) {
		is_tree = true;
	} else if (depth == 0) {
		/* a stick with only BIOS files, saves or themes at its root */
		is_tree = has_child_with_files(dir, "bios") || has_child_with_files(dir, "saves") ||
			  has_child_with_files(dir, "states") || has_child_with_files(dir, "themes");
	}
	if (is_tree) {
		struct transfer_tree *t = &s->out[s->n++];

		memset(t, 0, sizeof(*t));
		tr_strlcpy(t->path, dir, sizeof(t->path));
		tr_strlcpy(t->rel, rel, sizeof(t->rel));
		t->backup = backup;
		t->backup_time = when;
		summarize(t);
		if (backup)
			return;                      /* nothing else inside a backup */
	}
	if (depth >= TREE_DEPTH || !(d = opendir(dir)))
		return;
	while ((de = readdir(d)) && s->n < s->max) {
		char p[TRANSFER_PATH_MAX], r[256];
		struct stat st;

		if (skip_folder(de->d_name) || (is_tree && consumed(de->d_name)) ||
		    tr_snprintf(p, sizeof(p), "%s/%s", dir, de->d_name) < 0 ||
		    lstat(p, &st) < 0 || !S_ISDIR(st.st_mode) ||
		    tr_snprintf(r, sizeof(r), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name) < 0)
			continue;
		/* a "roms" folder is the library of its parent: seen from there */
		if (tr_str_ieq(de->d_name, "roms") && !is_tree)
			continue;
		tree_walk(s, p, r, de->d_name, depth + 1);
	}
	closedir(d);
}

static int cmp_tree(const void *a, const void *b)
{
	const struct transfer_tree *x = a, *y = b;

	return strcasecmp(x->rel, y->rel);
}

int transfer_find_trees(const char *stick_root, struct transfer_tree *out, int max)
{
	struct tree_search s = { out, max, 0 };
	int64_t t0 = tr_now_ms();
	struct stat st;

	if (stat(stick_root, &st) < 0 || !S_ISDIR(st.st_mode))
		return -ENOENT;
	tree_walk(&s, stick_root, "", "", 0);
	if (s.n > 1)
		qsort(out, (size_t)s.n, sizeof(out[0]), cmp_tree);
	{
		char list[300] = "";
		size_t o = 0;

		for (int i = 0; i < s.n && o + 4 < sizeof(list); i++)
			o += (size_t)snprintf(list + o, sizeof(list) - o, "%s%s%s (%d games)",
					      i ? ", " : "", out[i].rel[0] ? out[i].rel : "/",
					      out[i].backup ? " [backup]" : "", out[i].games);
		tr_log("libraries on %s: %d%s%s (%lld ms)", stick_root, s.n, s.n ? ": " : "", list,
		       (long long)(tr_now_ms() - t0));
	}
	return s.n;
}

/* ------------------------------------------------------------ async scans */

static struct {
	pthread_t th;
	bool started;
	volatile int done;
	int mode;                        /* 0 plan (layouts), 1 plan over trees, 2 trees */
	char src[8][TRANSFER_PATH_MAX];
	int nsrc;
	char dst[TRANSFER_PATH_MAX];
	struct transfer_plan *plan;
	struct transfer_tree trees[TRANSFER_TREES_MAX];
	int ntrees;
	int err;
} P;

static void *plan_thread(void *arg)
{
	(void)arg;
	if (P.mode == 2) {
		P.ntrees = transfer_find_trees(P.src[0], P.trees, TRANSFER_TREES_MAX);
		P.err = P.ntrees < 0 ? P.ntrees : 0;
	} else if (P.mode == 1) {
		const char *roots[8];

		for (int i = 0; i < P.nsrc; i++)
			roots[i] = P.src[i];
		P.err = transfer_plan_build_trees(roots, P.nsrc, P.dst, &P.plan);
	} else {
		P.err = transfer_plan_build(P.src[0], P.dst, &P.plan);
	}
	__atomic_store_n(&P.done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static int async_start(int mode, const char *const *roots, int n, const char *dst_root)
{
	if (P.started)
		return -EBUSY;
	if (n < 1 || n > 8)
		return -EINVAL;
	P.mode = mode;
	P.nsrc = n;
	for (int i = 0; i < n; i++)
		tr_strlcpy(P.src[i], roots[i], sizeof(P.src[i]));
	tr_strlcpy(P.dst, dst_root ? dst_root : "", sizeof(P.dst));
	P.plan = NULL;
	P.ntrees = 0;
	P.err = 0;
	P.done = 0;
	if (pthread_create(&P.th, NULL, plan_thread, NULL) != 0)
		return -EAGAIN;
	P.started = true;
	return 0;
}

int transfer_plan_start(const char *src_root, const char *dst_root)
{
	return async_start(0, &src_root, 1, dst_root);
}

int transfer_plan_start_trees(const char *const *roots, int n, const char *dst_root)
{
	return async_start(1, roots, n, dst_root);
}

int transfer_trees_start(const char *stick_root)
{
	return async_start(2, &stick_root, 1, NULL);
}

static int async_join(int mode)
{
	if (!P.started || (P.mode == 2) != (mode == 2))
		return -EINVAL;
	if (!__atomic_load_n(&P.done, __ATOMIC_ACQUIRE))
		return 0;
	pthread_join(P.th, NULL);
	P.started = false;
	return 1;
}

int transfer_plan_poll(struct transfer_plan **out)
{
	int r = async_join(0);

	if (r <= 0)
		return r;
	*out = P.plan;
	return P.err < 0 ? P.err : 1;
}

int transfer_trees_poll(struct transfer_tree *out, int max, int *n)
{
	int r = async_join(2);

	if (r <= 0)
		return r;
	if (P.err < 0)
		return P.err;
	*n = P.ntrees < max ? P.ntrees : max;
	memcpy(out, P.trees, sizeof(out[0]) * (size_t)*n);
	return 1;
}

/* ------------------------------------------------------------ copy */

void transfer_import_defaults(struct transfer_import_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->roms = o->bios = o->saves = o->themes = o->screenshots = true;
	o->dup_files = o->dup_saves = TRANSFER_DUP_ASK;
}

static bool wanted(const struct transfer_item *it, const struct transfer_import_opts *o)
{
	switch (it->kind) {
	case TRANSFER_ROM: return o->roms;
	case TRANSFER_BIOS: return o->bios;
	case TRANSFER_SAVE:
	case TRANSFER_STATE:
	case TRANSFER_CONFIG: return o->saves;
	case TRANSFER_THEME: return o->themes;
	case TRANSFER_SHOT: return o->screenshots;
	}
	return false;
}

struct run {
	struct transfer_progress *pr;
	transfer_progress_fn cb;
	void *user;
	int64_t t0, last_cb, rate_t, wait_ms;
	uint64_t rate_bytes, last_cb_bytes;
	/* differing files: the policies, updated by the "all" answers */
	enum transfer_dup dup_files, dup_saves;
	transfer_ask_fn ask;
	void *ask_user;
	int qseq;
	/* systems whose folders changed: bit per plan->sys index, + bios/themes */
	uint64_t changed;
	bool bios_changed, themes_changed;
	/* the copy engine and the per-folder throughput log */
	struct tr_copier cp;
	struct tr_group grp;
	/* the stick, opened once: every source is opened under it (review
	 * F-M17: by absolute path, a stick plugged in at the same /media/usbN
	 * during the import was read against the old plan) */
	int srcfd;
	dev_t src_dev;
	const char *src_root;
};

/* The stick of this import is gone (pulled, unmounted, another one now). */
static bool stick_gone(const struct run *r)
{
	struct stat st;

	return stat(r->src_root, &st) < 0 || st.st_dev != r->src_dev;
}

/* Opens an item's source under the stick's root fd. */
static int open_source(const struct run *r, const char *src)
{
	size_t l = strlen(r->src_root);
	int fd;

	if (r->srcfd < 0 || strncmp(src, r->src_root, l) || src[l] != '/')
		return -EINVAL;
	fd = openat(r->srcfd, src + l + 1, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0)
		return fd;
	fd = -errno;
	/* a file that went away, or the whole stick? */
	if ((fd == -ENOENT || fd == -ENOTDIR || fd == -EIO) && stick_gone(r))
		return -ENODEV;
	return fd;
}

static void report(struct run *r, bool force)
{
	int64_t now = tr_now_ms();

	r->pr->elapsed_ms = now - r->t0 - r->wait_ms;
	if (now - r->rate_t >= 500) {
		uint64_t d = r->pr->bytes_done - r->rate_bytes;
		uint32_t inst = (uint32_t)(d * 1000 / 1024 / (uint64_t)(now - r->rate_t));

		r->pr->rate_kbs = r->pr->rate_kbs ? (r->pr->rate_kbs * 3 + inst) / 4 : inst;
		r->rate_t = now;
		r->rate_bytes = r->pr->bytes_done;
		r->pr->eta_s = r->pr->rate_kbs ?
			(int)((r->pr->bytes_total - r->pr->bytes_done) / 1024 / r->pr->rate_kbs) : -1;
	}
	if (r->cb && (force || now - r->last_cb >= 100 ||
		      r->pr->bytes_done - r->last_cb_bytes >= (1u << 20))) {
		r->last_cb = now;
		r->last_cb_bytes = r->pr->bytes_done;
		r->cb(r->pr, r->user);
	}
}

static void report_tick(void *arg)
{
	report(arg, false);
}

static bool fatal_errno(int e)
{
	return e == -ENOSPC || e == -EIO || e == -ENODEV || e == -EROFS || e == -ENXIO ||
	       e == -EDQUOT || e == -ENOTCONN;
}

/*
 * A file exists on /data with other content: 1 replace, 0 keep,
 * -ECANCELED. Asks (r->ask) unless an "all" answer or the options decided.
 */
static int decide(struct run *r, const struct transfer_item *it, const struct stat *dst,
		  volatile int *cancel)
{
	bool save = savelike(it->kind);
	enum transfer_dup *pol = save ? &r->dup_saves : &r->dup_files;
	struct transfer_question q;
	enum transfer_answer a;
	int64_t t0;

	if (*pol == TRANSFER_DUP_REPLACE)
		return 1;
	if (*pol == TRANSFER_DUP_SKIP)
		return 0;
	if (!r->ask)
		return save ? 0 : 1;             /* no one to ask: the old rules */
	memset(&q, 0, sizeof(q));
	q.seq = ++r->qseq;
	q.kind = (enum transfer_kind)it->kind;
	q.save = save;
	tr_strlcpy(q.path, it->dst, sizeof(q.path));
	q.src_size = it->size;
	q.src_mtime = it->mtime_s;
	q.dst_size = dst ? (uint64_t)dst->st_size : 0;
	q.dst_mtime = dst ? (int64_t)dst->st_mtim.tv_sec : 0;
	t0 = tr_now_ms();
	a = r->ask(&q, r->ask_user);
	r->wait_ms += tr_now_ms() - t0;
	r->grp.pause_ms += tr_now_ms() - t0;     /* not throughput either */
	r->rate_t = tr_now_ms();                 /* the wait is not throughput */
	r->rate_bytes = r->pr->bytes_done;
	if (cancel && *cancel)
		return -ECANCELED;
	switch (a) {
	case TRANSFER_ANSWER_SKIP_ALL:
		*pol = TRANSFER_DUP_SKIP;
		/* fall through */
	case TRANSFER_ANSWER_SKIP:
		tr_log("import: %s differs: kept%s", it->dst, a == TRANSFER_ANSWER_SKIP_ALL ?
		       (save ? " (and every next differing save)" : " (and every next differing file)") : "");
		return 0;
	case TRANSFER_ANSWER_REPLACE_ALL:
		*pol = TRANSFER_DUP_REPLACE;
		/* fall through */
	case TRANSFER_ANSWER_REPLACE:
	default:
		tr_log("import: %s differs: replaced%s", it->dst, a == TRANSFER_ANSWER_REPLACE_ALL ?
		       (save ? " (and every next differing save)" : " (and every next differing file)") : "");
		return 1;
	}
}

/*
 * Handles one item. Returns 1 written, 0 not written (identical or kept),
 * <0 error (-ECANCELED included).
 */
static int copy_item(int rootfd, struct transfer_item *it, struct run *r, volatile int *cancel)
{
	char dir[TRANSFER_PATH_MAX], base[256];
	struct tr_sink sink;
	struct stat st, dst;
	bool have_dst = false;
	int sfd, dfd, e;
	uint64_t before = 0;           /* bytes_done before this file */

	sfd = open_source(r, it->src);
	if (sfd < 0)
		return sfd;
	if (fstat(sfd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(sfd);
		return -EINVAL;
	}
	tr_split_path(it->dst, dir, sizeof(dir), base, sizeof(base));
	dfd = tr_open_dir_chain(rootfd, dir, true);
	if (dfd < 0) {
		close(sfd);
		return dfd;
	}
	if (it->action != TRANSFER_NEW && fstatat(dfd, base, &dst, AT_SYMLINK_NOFOLLOW) == 0 &&
	    S_ISREG(dst.st_mode))
		have_dst = true;
	if (it->action == TRANSFER_CHECK && have_dst) {
		int old = openat(dfd, base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		int same = old >= 0 ? tr_same_content(sfd, old) : 0;

		if (old >= 0)
			close(old);
		if (same == 1) {
			struct timespec ts[2] = {
				{ .tv_nsec = UTIME_OMIT },
				{ .tv_sec = (time_t)it->mtime_s, .tv_nsec = it->mtime_ns },
			};

			/* next time the size+mtime test says SAME */
			utimensat(dfd, base, ts, AT_SYMLINK_NOFOLLOW);
			r->pr->bytes_done += it->size;
			r->pr->identical++;
			e = 0;
			goto out;
		}
	}
	if (have_dst && it->action != TRANSFER_NEW) {
		/* the console has a different file: ask (or apply the policy) */
		e = decide(r, it, &dst, cancel);
		if (e <= 0) {
			if (e == 0) {
				r->pr->bytes_done += it->size;
				r->pr->kept++;
				if (savelike(it->kind))
					r->pr->conflicts_kept++;
			}
			goto out;
		}
	}
	e = tr_sink_open(&sink, dfd, base);
	if (e < 0)
		goto out;
	if (lseek(sfd, 0, SEEK_SET) < 0) {           /* the CHECK compare read it */
		e = -errno;
		tr_sink_abort(&sink);
		goto out;
	}
	before = r->pr->bytes_done;
	e = tr_copy_file(&r->cp, sfd, (uint64_t)st.st_size, &sink, &r->pr->bytes_done, cancel, report_tick, r);
	if (e < 0) {
		tr_sink_abort(&sink);
		/* back to before this file: sink.written can count bytes the
		 * progress never got (review F-L8: bytes_done wrapped) */
		r->pr->bytes_done = before;
		goto out;
	}
	e = tr_copier_commit(&r->cp, &sink, it->mtime_s, it->mtime_ns, have_dst && savelike(it->kind));
	if (e == 0) {
		e = 1;
		if (have_dst)
			r->pr->replaced++;
		if (it->kind == TRANSFER_ROM && it->sys >= 0 && it->sys < 64)
			r->changed |= 1ull << it->sys;
		if (it->kind == TRANSFER_BIOS)
			r->bios_changed = true;
		if (it->kind == TRANSFER_THEME)
			r->themes_changed = true;
	} else {
		r->pr->bytes_done = before;      /* not written after all */
	}
out:
	close(dfd);
	close(sfd);
	return e;
}

static int run_import(struct transfer_plan *p, const struct transfer_import_opts *o,
		      struct run *r, volatile int *cancel)
{
	struct transfer_progress *pr = r->pr;
	char a[32];
	int rootfd, fatal = 0, cpe;

	memset(pr, 0, sizeof(*pr));
	pr->state = TRANSFER_RUNNING;
	pr->eta_s = -1;
	r->dup_files = o->dup_files;
	r->dup_saves = o->overwrite_saves ? TRANSFER_DUP_REPLACE : o->dup_saves;
	if (!r->ask) {
		r->ask = o->ask;
		r->ask_user = o->ask_user;
	}
	for (int i = 0; i < p->nitems; i++) {
		if (!wanted(&p->items[i], o))
			continue;
		pr->files_total++;
		if (p->items[i].action != TRANSFER_SAME)
			pr->bytes_total += p->items[i].size;
	}
	r->t0 = r->rate_t = tr_now_ms();
	tr_fmt_bytes(pr->bytes_total, a, sizeof(a));
	tr_log("import started: %d files, %s, from %s", pr->files_total, a, p->src_root);
	rootfd = open(p->dst_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	fatal = rootfd < 0 ? -errno : 0;
	r->src_root = p->src_root;
	r->srcfd = open(p->src_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	{
		struct stat st;

		if (r->srcfd < 0 || fstat(r->srcfd, &st) < 0) {
			if (!fatal)
				fatal = -ENODEV;             /* the stick is not there */
		} else {
			r->src_dev = st.st_dev;
		}
	}
	cpe = tr_copier_init(&r->cp);
	memset(&r->grp, 0, sizeof(r->grp));
	if (fatal || cpe < 0) {
		if (!fatal)
			fatal = cpe;
		goto done;
	}
	report(r, true);
	for (int i = 0; i < p->nitems; i++) {
		struct transfer_item *it = &p->items[i];
		int e;

		if (!wanted(it, o))
			continue;
		if (cancel && *cancel) {
			fatal = -ECANCELED;
			break;
		}
		tr_strlcpy(pr->current, it->dst, sizeof(pr->current));
		if (it->action != TRANSFER_SAME) {
			/* the next file to copy is read ahead while this one is flushed */
			int j = i + 1;

			while (j < p->nitems && (!wanted(&p->items[j], o) || p->items[j].action == TRANSFER_SAME))
				j++;
			tr_copier_hint_next(&r->cp, j < p->nitems ? p->items[j].src : NULL,
					    j < p->nitems ? p->items[j].size : 0);
			tr_group_step(&r->grp, &r->cp, "import", it->dst);
		}
		if (it->action == TRANSFER_SAME) {
			pr->identical++;
		} else if ((e = copy_item(rootfd, it, r, cancel)) >= 0) {
			if (e == 1)
				pr->copied++;
		} else if (e == -ECANCELED) {
			fatal = e;
			break;
		} else {
			pr->failed++;
			snprintf(pr->errmsg, sizeof(pr->errmsg), "%s: %s", it->dst, strerror(-e));
			tr_log("import %s -> %s: %s", it->src, it->dst, strerror(-e));
			if (fatal_errno(e)) {
				fatal = e;
				break;
			}
		}
		pr->skipped = pr->identical + pr->kept;
		pr->files_done++;
		report(r, true);
	}
done:
	if (r->cp.buf) {
		tr_group_end(&r->grp, &r->cp);
		tr_copier_free(&r->cp);
	}
	if (rootfd >= 0) {
		fsync(rootfd);
		close(rootfd);
	}
	if (r->srcfd >= 0)
		close(r->srcfd);
	r->srcfd = -1;
	pr->current[0] = 0;
	pr->err = fatal;
	pr->skipped = pr->identical + pr->kept;
	if (fatal == -ECANCELED) {
		pr->state = TRANSFER_CANCELLED;
	} else if (fatal) {
		pr->state = TRANSFER_FAILED;
		if (fatal == -ENOSPC)
			snprintf(pr->errmsg, sizeof(pr->errmsg), "The SD card is full");
		else if (fatal == -ENODEV)
			snprintf(pr->errmsg, sizeof(pr->errmsg), "The USB drive was removed");
		else if (fatal == -EIO || fatal == -ENXIO)
			snprintf(pr->errmsg, sizeof(pr->errmsg),
				 "Read error: was the USB drive removed?");
		else if (!pr->errmsg[0])
			snprintf(pr->errmsg, sizeof(pr->errmsg), "%s", strerror(-fatal));
	} else {
		pr->state = TRANSFER_DONE;
	}
	pr->eta_s = 0;
	report(r, true);
	{
		int64_t ms = pr->elapsed_ms > 0 ? pr->elapsed_ms : 1;

		tr_fmt_bytes(pr->bytes_done, a, sizeof(a));
		tr_log("import %s: imported %d, already there %d, skipped %d, replaced %d, failed %d; "
		       "%s in %lld.%01lld s (%.1f MB/s; waiting for the source %.1f s, writing %.1f s, "
		       "flushing %.1f s)%s%s",
		       pr->state == TRANSFER_DONE ? "done" : pr->state == TRANSFER_CANCELLED ? "cancelled" :
		       "failed", pr->copied - pr->replaced, pr->identical, pr->kept, pr->replaced,
		       pr->failed, a, (long long)(ms / 1000), (long long)(ms % 1000 / 100),
		       (double)pr->bytes_done / 1048576.0 / ((double)ms / 1000.0), (double)r->cp.wait_us / 1e6,
		       (double)r->cp.write_us / 1e6, (double)r->cp.sync_us / 1e6,
		       pr->state == TRANSFER_FAILED ? ": " : "", pr->state == TRANSFER_FAILED ? pr->errmsg : "");
	}
	return fatal == -ECANCELED ? 0 : fatal;
}

int transfer_import_run(struct transfer_plan *p, const struct transfer_import_opts *o,
			transfer_progress_fn cb, void *user, volatile int *cancel,
			struct transfer_progress *result)
{
	struct transfer_progress pr;
	struct transfer_import_opts def;
	struct run r = { .pr = &pr, .cb = cb, .user = user };
	int e;

	if (!o) {
		transfer_import_defaults(&def);
		o = &def;
	}
	e = run_import(p, o, &r, cancel);
	if (result)
		*result = pr;
	return e;
}

/* ------------------------------------------------------------ async */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t th;
	bool started;              /* thread exists (until finish) */
	struct transfer_plan *plan;
	struct transfer_import_opts opts;
	struct transfer_progress shared;
	volatile int cancel;
	bool answered;
	enum transfer_answer answer;
	struct run r;
	struct transfer_progress local;
} A = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void async_cb(const struct transfer_progress *p, void *user)
{
	(void)user;
	pthread_mutex_lock(&A.lock);
	A.shared = *p;
	pthread_mutex_unlock(&A.lock);
}

/* The import thread waits here until the UI answers (or cancels). */
static enum transfer_answer async_ask(const struct transfer_question *q, void *user)
{
	enum transfer_answer a;

	(void)user;
	pthread_mutex_lock(&A.lock);
	A.local.asking = true;
	A.local.question = *q;
	A.shared = A.local;
	A.answered = false;
	while (!A.answered && !A.cancel)
		pthread_cond_wait(&A.cond, &A.lock);
	a = A.answered ? A.answer : TRANSFER_ANSWER_SKIP;
	A.local.asking = false;
	A.shared = A.local;
	pthread_mutex_unlock(&A.lock);
	return a;
}

static void *import_thread(void *arg)
{
	(void)arg;
	run_import(A.plan, &A.opts, &A.r, &A.cancel);
	pthread_mutex_lock(&A.lock);
	A.shared = A.local;
	pthread_mutex_unlock(&A.lock);
	return NULL;
}

bool tr_import_busy(void)
{
	return A.started;
}

int transfer_import_start(struct transfer_plan *p, const struct transfer_import_opts *o)
{
	if (A.started || tr_backup_busy())
		return -EBUSY;
	A.plan = p;
	if (o)
		A.opts = *o;
	else
		transfer_import_defaults(&A.opts);
	A.opts.ask = NULL;                       /* the UI answers through the API */
	A.cancel = 0;
	A.answered = false;
	memset(&A.r, 0, sizeof(A.r));
	A.r.pr = &A.local;
	A.r.cb = async_cb;
	A.r.ask = async_ask;
	pthread_mutex_lock(&A.lock);
	memset(&A.shared, 0, sizeof(A.shared));
	A.shared.state = TRANSFER_RUNNING;
	A.shared.eta_s = -1;
	pthread_mutex_unlock(&A.lock);
	if (pthread_create(&A.th, NULL, import_thread, NULL) != 0) {
		A.plan = NULL;
		return -EAGAIN;
	}
	A.started = true;
	return 0;
}

enum transfer_state transfer_import_status(struct transfer_progress *out)
{
	enum transfer_state s;

	pthread_mutex_lock(&A.lock);
	if (out)
		*out = A.shared;
	s = A.started ? A.shared.state : TRANSFER_IDLE;
	pthread_mutex_unlock(&A.lock);
	return s;
}

void transfer_import_answer(enum transfer_answer a)
{
	pthread_mutex_lock(&A.lock);
	A.answer = a;
	A.answered = true;
	pthread_cond_broadcast(&A.cond);
	pthread_mutex_unlock(&A.lock);
}

void transfer_import_cancel(void)
{
	pthread_mutex_lock(&A.lock);
	A.cancel = 1;
	pthread_cond_broadcast(&A.cond);
	pthread_mutex_unlock(&A.lock);
}

int transfer_import_finish(char out[][TRANSFER_SYSID_MAX], int max)
{
	int n = 0;

	if (!A.started)
		return 0;
	pthread_join(A.th, NULL);
	A.started = false;
	for (int i = 0; i < A.plan->nsys && i < 64 && n < max; i++)
		if (A.r.changed & (1ull << i))
			tr_strlcpy(out[n++], A.plan->sys[i].id, TRANSFER_SYSID_MAX);
	if (A.r.bios_changed && n < max)
		tr_strlcpy(out[n++], "bios", TRANSFER_SYSID_MAX);
	if (A.r.themes_changed && n < max)
		tr_strlcpy(out[n++], "themes", TRANSFER_SYSID_MAX);
	transfer_plan_free(A.plan);
	A.plan = NULL;
	pthread_mutex_lock(&A.lock);
	A.shared.state = TRANSFER_IDLE;
	pthread_mutex_unlock(&A.lock);
	return n;
}
