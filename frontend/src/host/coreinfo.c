/*
 * coreinfo.c - see coreinfo.h.
 */
#include "coreinfo.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ftw.h>
#include <strings.h>
#include <sys/stat.h>

#include "hutil.h"
#include "../i18n/i18n.h"
#include "md5.h"
#include "../../third_party/miniz/miniz.h"

#define CORES_DIR "/usr/share/rsos/cores"

void coreinfo_id_from_path(const char *core_path, char *id, size_t n)
{
	char *s;

	hpath_stem(core_path, id, n);
	s = strstr(id, "_libretro");
	if (s)
		*s = 0;
}

static void copy_val(const struct ini *ini, const char *sec, const char *key, char *dst, size_t n)
{
	const char *v = ini_get(ini, sec, key);

	if (v)
		hstrlcpy(dst, v, n);
}

int coreinfo_load(struct core_info *ci, const char *dir, const char *id)
{
	char path[4096];
	int ret;

	memset(ci, 0, sizeof(*ci));
	hstrlcpy(ci->id, id, sizeof(ci->id));
	hstrlcpy(ci->display_name, id, sizeof(ci->display_name));
	hstrlcpy(ci->renderer, "software", sizeof(ci->renderer));
	ci->savestates = true;
	if (!hpath(path, sizeof(path), "%s/%s.ini", dir ? dir : CORES_DIR, id))
		return -ENAMETOOLONG;
	ret = ini_load(&ci->ini, path);
	if (ret < 0)
		return ret;
	ci->loaded = true;
	copy_val(&ci->ini, "core", "display_name", ci->display_name, sizeof(ci->display_name));
	copy_val(&ci->ini, "core", "library", ci->library, sizeof(ci->library));
	copy_val(&ci->ini, "core", "systems", ci->systems, sizeof(ci->systems));
	copy_val(&ci->ini, "core", "extensions", ci->extensions, sizeof(ci->extensions));
	copy_val(&ci->ini, "core", "renderer", ci->renderer, sizeof(ci->renderer));
	copy_val(&ci->ini, "core", "system_files", ci->system_files, sizeof(ci->system_files));
	copy_val(&ci->ini, "core", "system_tree", ci->system_tree, sizeof(ci->system_tree));
	copy_val(&ci->ini, "core", "default_systems", ci->default_systems, sizeof(ci->default_systems));
	ci->need_fullpath = ini_get_bool(&ci->ini, "core", "need_fullpath", false);
	ci->block_extract = ini_get_bool(&ci->ini, "core", "block_extract", false);
	ci->savestates = ini_get_bool(&ci->ini, "core", "savestates", true);
	ci->experimental = ini_get_bool(&ci->ini, "core", "experimental", false);

	for (int i = 0; i < ci->ini.n && ci->nbios < COREINFO_MAX_BIOS; i++) {
		const struct ini_entry *e = &ci->ini.e[i];
		struct bios_entry *b;
		const char *file;

		if (strncasecmp(e->section, "bios:", 5) != 0)
			continue;
		file = e->section + 5;
		/* One bios_entry per section (entries of a section are contiguous
		 * in practice, but look it up to be safe). */
		b = NULL;
		for (int j = 0; j < ci->nbios; j++)
			if (!strcmp(ci->bios[j].file, file))
				b = &ci->bios[j];
		if (!b) {
			b = &ci->bios[ci->nbios++];
			memset(b, 0, sizeof(*b));
			hstrlcpy(b->file, file, sizeof(b->file));
			hstrlcpy(b->required, "no", sizeof(b->required));
		}
		if (!strcasecmp(e->key, "md5"))
			hstrlcpy(b->md5, e->value, sizeof(b->md5));
		else if (!strcasecmp(e->key, "required"))
			hstrlcpy(b->required, e->value, sizeof(b->required));
		else if (!strcasecmp(e->key, "description"))
			hstrlcpy(b->description, e->value, sizeof(b->description));
	}
	for (int j = 0; j < ci->nbios; j++)
		for (char *p = ci->bios[j].md5; *p; p++)
			if (*p >= 'A' && *p <= 'F')
				*p = (char)(*p - 'A' + 'a');
	return 0;
}

void coreinfo_free(struct core_info *ci)
{
	ini_free(&ci->ini);
	ci->loaded = false;
}

const char *system_display_name(const char *system)
{
	static const char *const map[][2] = {
		{ "nes", "NES" }, { "fds", "Famicom Disk System" }, { "snes", "SNES" },
		{ "megadrive", "Mega Drive" }, { "genesis", "Genesis" },
		{ "mastersystem", "Master System" }, { "gamegear", "Game Gear" },
		{ "sg1000", "SG-1000" }, { "sega32x", "32X" }, { "segacd", "Sega CD" },
		{ "pico", "Pico" }, { "gb", "Game Boy" }, { "gbc", "Game Boy Color" },
		{ "gba", "Game Boy Advance" }, { "psx", "PS1" }, { "arcade", "Arcade" },
		{ "fbneo", "Arcade" }, { "neogeo", "Neo Geo" }, { "n64", "N64" },
		{ "pcengine", "PC Engine" }, { "pcenginecd", "PC Engine CD" }, { "neocd", "Neo Geo CD" },
		{ "msx", "MSX" }, { "coleco", "ColecoVision" },
		/* third batch (docs/cores.md): names as the carousel shows them */
		{ "supergrafx", "SuperGrafx" }, { "pokemini", "Pok\xc3\xa9mon mini" }, { "dos", "MS-DOS" },
		{ "scummvm", "ScummVM" }, { "c64", "Commodore 64" }, { "zxspectrum", "ZX Spectrum" },
		{ "amstradcpc", "Amstrad CPC" }, { "pico8", "PICO-8" }, { "doom", "Doom" },
		{ "cavestory", "Cave Story" },
	};

	if (!system || !*system)
		/* TRANSLATORS: start of "<system> needs <BIOS file> in /bios" when the system is unknown */
		return _("This system");
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcasecmp(map[i][0], system))
			return map[i][1];
	return system;
}

/* "/data/bios" -> "/bios": what the user sees on the RETROSTONE card. */
static const char *user_path(const char *p)
{
	return strncmp(p, "/data/", 6) == 0 ? p + 5 : p;
}

/* Does this entry's `required` apply to (system, ext)? */
static bool is_required(const struct bios_entry *b, const char *system, const char *ext)
{
	if (!strcasecmp(b->required, "no") || !*b->required)
		return false;
	if (!strcasecmp(b->required, "yes"))
		return true;
	return hlist_has(b->required, system) || (*ext && hlist_has(b->required, ext));
}

enum bstate { B_MISSING = 0, B_OK, B_BADMD5 };

static enum bstate probe(const struct bios_entry *b, const char *bios_dir, const char *rom,
			 char *where, size_t wn)
{
	char path[4096], ext[16];
	char romdir[4096];
	const char *dirs[2];
	int nd = 0;

	hpath_ext(b->file, ext, sizeof(ext));
	dirs[nd++] = bios_dir;
	if (!b->md5[0] && !strcmp(ext, "zip") && rom && *rom) {
		/* Arcade BIOS sets live next to the games. */
		hpath_dir(rom, romdir, sizeof(romdir));
		dirs[nd++] = romdir;
	}
	for (int i = 0; i < nd; i++) {
		char hex[33];

		if (!hpath(path, sizeof(path), "%s/%s", dirs[i], b->file) || !hfile_exists(path))
			continue;
		if (where)
			hstrlcpy(where, path, wn);
		if (!b->md5[0])
			return B_OK;
		if (md5_file(path, hex) == 0 && !strcmp(hex, b->md5))
			return B_OK;
		return B_BADMD5;
	}
	return B_MISSING;
}

bool bios_check(const struct core_info *ci, const char *system, const char *rom,
		const char *bios_dir, char *err, size_t errn, char *warn, size_t warnn)
{
	char ext[16] = "";
	bool done[COREINFO_MAX_BIOS] = { false };
	const char *sname = system_display_name(system);

	if (err && errn)
		err[0] = 0;
	if (warn && warnn)
		warn[0] = 0;
	if (rom)
		hpath_ext(rom, ext, sizeof(ext));

	for (int i = 0; i < ci->nbios; i++) {
		const struct bios_entry *b = &ci->bios[i];
		bool any_ok = false, any_bad = false;
		char names[512] = "", names_log[512], where[4096] = "";
		int count = 0;
		char zipdir[4096];

		if (done[i] || !is_required(b, system, ext))
			continue;
		/* The group: every mandatory entry with the same `required`. */
		for (int j = i; j < ci->nbios; j++) {
			const struct bios_entry *o = &ci->bios[j];
			enum bstate st;

			if (done[j] || strcasecmp(o->required, b->required) != 0)
				continue;
			done[j] = true;
			st = probe(o, bios_dir, rom, where, sizeof(where));
			any_ok |= st == B_OK;
			any_bad |= st == B_BADMD5;
			count++;
			if (names[0])
				strncat(names, ", ", sizeof(names) - strlen(names) - 1);
			strncat(names, o->file, sizeof(names) - strlen(names) - 1);
		}
		if (any_ok)
			continue;
		if (any_bad) {
			/* Present but not the dump we know: let it run, warn. */
			hlog(HLOG_WARN, "BIOS %s: md5 differs from the known good dump", where);
			if (warn && warnn && !warn[0]) {
				/* TRANSLATORS: warning shown by the menu before a game starts:
				 * "PS1: scph5501.bin is not the expected BIOS dump" */
				snprintf(warn, warnn, _("%s: %s is not the expected BIOS dump"),
					 sname, hpath_base(where));
				hutf8_trim(warn);
			}
			continue;
		}
		hstrlcpy(names_log, names, sizeof(names_log));
		if (count > 1) {
			/* "a, b, c" -> "a, b or c" */
			char *last = strrchr(names, ','), head[512], tail[512];

			if (last) {
				hstrlcpy(tail, last + 2, sizeof(tail));
				*last = 0;
				hstrlcpy(head, names, sizeof(head));
				/* TRANSLATORS: the last two of a list of BIOS files the user
				 * can choose from: "bios_CD_U.bin, bios_CD_E.bin or bios_CD_J.bin" */
				snprintf(names, sizeof(names), _("%s or %s"), head, tail);
			}
		}
		{
			char e2[16];

			hpath_ext(b->file, e2, sizeof(e2));
			if (!b->md5[0] && !strcmp(e2, "zip") && rom && *rom)
				hpath_dir(rom, zipdir, sizeof(zipdir));
			else
				hstrlcpy(zipdir, bios_dir, sizeof(zipdir));
		}
		if (err && errn) {
			/* TRANSLATORS: error shown by the menu instead of starting the game:
			 * "PS1 needs scph5501.bin in /bios" (system, BIOS file(s), folder) */
			snprintf(err, errn, _("%s needs %s in %s"), sname, names, user_path(zipdir));
			hutf8_trim(err);
		}
		hlog(HLOG_ERROR, "BIOS check failed: %s needs %s (looked in %s)",
		     system && *system ? sname : "This system", names_log, zipdir);
		return false;
	}

	/* Optional files: a note for the log / UI, never blocking. */
	for (int i = 0; i < ci->nbios; i++) {
		const struct bios_entry *b = &ci->bios[i];

		if (is_required(b, system, ext))
			continue;
		if (!strcasecmp(b->required, "no") && probe(b, bios_dir, rom, NULL, 0) == B_BADMD5)
			hlog(HLOG_WARN, "optional BIOS %s/%s: md5 differs", bios_dir, b->file);
	}
	return true;
}

void coreinfo_install_system_files(const struct core_info *ci, const char *sys_dir)
{
	const char *p = ci->system_files;

	while (p && *p) {
		char src[1024], dst[4096];
		const char *e;
		size_t l;

		while (*p == ' ' || *p == ',')
			p++;
		e = p;
		while (*e && *e != ',')
			e++;
		l = (size_t)(e - p);
		while (l && p[l - 1] == ' ')
			l--;
		if (l && l < sizeof(src)) {
			memcpy(src, p, l);
			src[l] = 0;
			if (hpath(dst, sizeof(dst), "%s/%s", sys_dir, hpath_base(src)) && !hfile_exists(dst)) {
				int r = hcopy_file(src, dst);

				hlog(r ? HLOG_WARN : HLOG_INFO, "system file %s -> %s: %s", src, dst,
				     r ? strerror(-r) : "copied");
			}
		}
		p = e;
	}
}

/* ------------------------------------------------------- system_tree */

static const char *tree_src;
static const char *tree_dst;
static int tree_copied;

static int tree_visit(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	char dst[4096];
	const char *rel = path + strlen(tree_src);

	(void)st;
	(void)ftw;
	while (*rel == '/')
		rel++;
	if (!*rel)
		return 0;
	if (!hpath(dst, sizeof(dst), "%s/%s", tree_dst, rel))
		return 0;
	if (type == FTW_D) {
		hmkdir_p(dst, 0755);
	} else if (type == FTW_F && !hfile_exists(dst)) {
		int r = hcopy_file(path, dst);

		if (r)
			hlog(HLOG_WARN, "system tree: %s -> %s: %s", path, dst, strerror(-r));
		else
			tree_copied++;
	}
	return 0;
}

void coreinfo_install_system_tree(const struct core_info *ci, const char *sys_dir)
{
	struct stat st;

	if (!ci->system_tree[0] || stat(ci->system_tree, &st) < 0 || !S_ISDIR(st.st_mode))
		return;
	tree_src = ci->system_tree;
	tree_dst = sys_dir;
	tree_copied = 0;
	nftw(ci->system_tree, tree_visit, 16, FTW_PHYS);
	if (tree_copied)
		hlog(HLOG_INFO, "system tree %s: %d file(s) copied to %s", ci->system_tree, tree_copied, sys_dir);
}

/* ---------------------------------------------------------- core choice */

/* docs/cores.md, "ROM folders" (default column). */
static const char *const default_cores[][2] = {
	{ "nes", "fceumm" }, { "fds", "fceumm" }, { "snes", "snes9x2005" },
	{ "megadrive", "picodrive" }, { "mastersystem", "picodrive" }, { "gamegear", "picodrive" },
	{ "sg1000", "picodrive" }, { "sega32x", "picodrive" }, { "segacd", "picodrive" },
	{ "gb", "gambatte" }, { "gbc", "gambatte" }, { "gba", "gpsp" }, { "psx", "pcsx_rearmed" },
	{ "arcade", "mame2003_plus" }, { "fbneo", "fbneo" }, { "neogeo", "fbneo" },
	{ "n64", "parallel_n64" }, { "pcengine", "mednafen_pce_fast" }, { "pcenginecd", "mednafen_pce_fast" },
	{ "atari2600", "stella2014" }, { "atari7800", "prosystem" }, { "atarilynx", "handy" },
	{ "ngp", "mednafen_ngp" }, { "ngpc", "mednafen_ngp" }, { "wonderswan", "mednafen_wswan" },
	{ "wonderswancolor", "mednafen_wswan" }, { "coleco", "gearcoleco" }, { "msx", "bluemsx" },
	/* third batch (docs/cores.md "Third batch") */
	{ "supergrafx", "mednafen_supergrafx" }, { "pokemini", "pokemini" }, { "dos", "dosbox_pure" },
	{ "scummvm", "scummvm" }, { "c64", "vice_x64" }, { "zxspectrum", "fuse" }, { "amstradcpc", "cap32" },
	{ "pico8", "fake08" }, { "doom", "prboom" }, { "cavestory", "nxengine" },
	/* The one documented exception to "never an experimental default":
	 * neocd has no other core (docs/cores.md). */
	{ "neocd", "geolith" },
};

/* Can this core open the file? By extension; a .zip also matches a core
 * that does not take zips but whose extensions match a file inside it
 * (the host extracts it), unless the core is an arcade (block_extract). */
static bool core_accepts(const struct core_info *ci, const char *rom)
{
	char ext[32];
	mz_zip_archive za;
	bool ok = false;

	if (!rom || !*rom)
		return true;
	hpath_ext(rom, ext, sizeof(ext));
	if (!ci->extensions[0] || hlist_has(ci->extensions, ext))
		return true;
	if (strcmp(ext, "zip") != 0 || ci->block_extract)
		return false;
	memset(&za, 0, sizeof(za));
	if (!mz_zip_reader_init_file(&za, rom, 0))
		return false;
	for (mz_uint i = 0, n = mz_zip_reader_get_num_files(&za); i < n && !ok; i++) {
		mz_zip_archive_file_stat st;
		char e[32];

		if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory)
			continue;
		hpath_ext(st.m_filename, e, sizeof(e));
		ok = hlist_has(ci->extensions, e);
	}
	mz_zip_reader_end(&za);
	return ok;
}

static int cmp_cand(const void *a, const void *b)
{
	return strcmp(((const struct core_candidate *)a)->id, ((const struct core_candidate *)b)->id);
}

int coreinfo_candidates(const char *cores_dir, const char *system, const char *rom,
			struct core_candidate *out, int max)
{
	const char *dir = cores_dir ? cores_dir : CORES_DIR;
	DIR *d = opendir(dir);
	struct dirent *de;
	int n = 0, def = -1;

	while (d && (de = readdir(d)) && n < max) {
		struct core_info ci;
		char id[64];
		size_t l = strlen(de->d_name);

		if (l < 5 || strcmp(de->d_name + l - 4, ".ini"))
			continue;
		hpath_stem(de->d_name, id, sizeof(id));
		if (coreinfo_load(&ci, dir, id) < 0)
			continue;
		if (hlist_has(ci.systems, system) && core_accepts(&ci, rom)) {
			memset(&out[n], 0, sizeof(out[n]));
			hstrlcpy(out[n].id, id, sizeof(out[n].id));
			hstrlcpy(out[n].name, ci.display_name, sizeof(out[n].name));
			out[n].experimental = ci.experimental;
			out[n].default_listed = hlist_has(ci.default_systems, system);
			n++;
		}
		coreinfo_free(&ci);
	}
	if (d)
		closedir(d);
	qsort(out, (size_t)n, sizeof(*out), cmp_cand);
	/* The automatic default: never an experimental core. */
	for (int i = 0; i < n && def < 0; i++)
		if (out[i].default_listed && !out[i].experimental)
			def = i;
	for (size_t t = 0; t < sizeof(default_cores) / sizeof(default_cores[0]) && def < 0; t++)
		if (!strcasecmp(default_cores[t][0], system))
			for (int i = 0; i < n; i++)
				if (!strcmp(out[i].id, default_cores[t][1]))
					def = i; /* the table may name an experimental core (neocd) */
	for (int i = 0; i < n && def < 0; i++)
		if (!out[i].experimental)
			def = i;
	if (def >= 0)
		out[def].is_default = true;
	return n;
}

int coreinfo_pick(const char *cores_dir, const char *choices_path, const char *system,
		  const char *rom, char *core_id, size_t n)
{
	struct core_candidate c[32];
	int nc = coreinfo_candidates(cores_dir, system, rom, c, 32);
	struct ini ch = { 0 };
	char game[256] = "", sec[512];
	const char *v[2] = { NULL, NULL };

	if (rom && *rom)
		hpath_stem(rom, game, sizeof(game));
	if (choices_path && ini_load(&ch, choices_path) != 0) {
		/* unreadable, or lost between the two renames of a save on FAT:
		 * the previous version (bench_ini_set_file keeps one) */
		char bak[PATH_MAX + 8];

		ini_free(&ch);
		if (hpath(bak, sizeof(bak), "%s.bak", choices_path) && ini_load(&ch, bak) != 0)
			ini_free(&ch);
	}
	if (ch.n) {
		if (game[0] && hpath(sec, sizeof(sec), "%s/%s", system, game))
			v[0] = ini_get(&ch, sec, "core");
		v[1] = ini_get(&ch, system, "core");
	}
	/* The user's choice (experimental allowed), if it can open this file. */
	for (int k = 0; k < 2; k++)
		for (int i = 0; v[k] && i < nc; i++)
			if (!strcmp(c[i].id, v[k])) {
				hstrlcpy(core_id, c[i].id, n);
				ini_free(&ch);
				return 0;
			}
	ini_free(&ch);
	for (int i = 0; i < nc; i++)
		if (c[i].is_default) {
			hstrlcpy(core_id, c[i].id, n);
			return 0;
		}
	return -ENOENT;
}
