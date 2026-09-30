/*
 * content.c - see content.h.
 */
#include "content.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "host_internal.h"
#include "hutil.h"
#include "../i18n/i18n.h"
#include "../../third_party/miniz/miniz.h"

#define MAX_ARCHIVE_MEMBER (64ull * 1024 * 1024)

static struct {
	char extracted[PATH_MAX];   /* file we created in tmp_dir ("" = none) */
	char tmpdir[PATH_MAX];
	char archive[PATH_MAX];
	char member[512];
	char dir[PATH_MAX], name[256], ext[32];
	struct retro_game_info_ext ext_info;
	bool have_ext;
} C;

static const char *valid_exts(void)
{
	if (H.sysinfo.valid_extensions && *H.sysinfo.valid_extensions)
		return H.sysinfo.valid_extensions;
	return H.info.extensions;
}

static bool need_fullpath_for(const char *ext)
{
	for (int i = 0; i < H.noverrides; i++)
		if (H.overrides[i].extensions && hlist_has(H.overrides[i].extensions, ext))
			return H.overrides[i].need_fullpath;
	return H.sysinfo.need_fullpath;
}

static int extract_zip(const char *zip, char *err, size_t n)
{
	mz_zip_archive za;
	mz_uint count, pick = (mz_uint)-1, files = 0, only = 0;
	mz_zip_archive_file_stat st;
	const char *valid = valid_exts();
	char out[PATH_MAX];

	memset(&za, 0, sizeof(za));
	if (!mz_zip_reader_init_file(&za, zip, 0)) {
		/* TRANSLATORS: game start error; %s is the zip library's English reason */
		snprintf(err, n, _("Cannot open the zip file (%s)"),
			 mz_zip_get_error_string(mz_zip_get_last_error(&za)));
		return -EINVAL;
	}
	count = mz_zip_reader_get_num_files(&za);
	for (mz_uint i = 0; i < count; i++) {
		char ext[32];

		if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory)
			continue;
		files++;
		only = i;
		hpath_ext(st.m_filename, ext, sizeof(ext));
		if (hlist_has(valid, ext)) {
			pick = i;
			break;
		}
	}
	if (pick == (mz_uint)-1 && files == 1)
		pick = only;
	if (pick == (mz_uint)-1) {
		snprintf(err, n, "%s", _("No playable file in this zip"));
		mz_zip_reader_end(&za);
		return -ENOENT;
	}
	mz_zip_reader_file_stat(&za, pick, &st);
	if (st.m_uncomp_size > MAX_ARCHIVE_MEMBER) {
		snprintf(err, n, _("The file in this zip is too large (%llu MB)"),
			 (unsigned long long)(st.m_uncomp_size >> 20));
		mz_zip_reader_end(&za);
		return -EFBIG;
	}
	hpath(C.tmpdir, sizeof(C.tmpdir), "%s/%d", H.cfg.tmp_dir, (int)getpid());
	hmkdir_p(C.tmpdir, 0700);
	hstrlcpy(C.member, st.m_filename, sizeof(C.member));
	if (!hpath(out, sizeof(out), "%s/%s", C.tmpdir, hpath_base(st.m_filename))) {
		mz_zip_reader_end(&za);
		return -ENAMETOOLONG;
	}
	if (!mz_zip_reader_extract_to_file(&za, pick, out, 0)) {
		/* TRANSLATORS: game start error: file name, then the zip library's English reason */
		snprintf(err, n, _("Cannot extract %s from the zip (%s)"), hpath_base(st.m_filename),
			 mz_zip_get_error_string(mz_zip_get_last_error(&za)));
		mz_zip_reader_end(&za);
		unlink(out);
		return -EIO;
	}
	mz_zip_reader_end(&za);
	hstrlcpy(C.extracted, out, sizeof(C.extracted));
	hstrlcpy(C.archive, zip, sizeof(C.archive));
	hstrlcpy(H.content_path, out, sizeof(H.content_path));
	hlog(HLOG_INFO, "extracted %s from %s (%llu bytes)", st.m_filename, zip,
	     (unsigned long long)st.m_uncomp_size);
	return 0;
}

int content_prepare(char *err, size_t n)
{
	char ext[32];

	memset(&C, 0, sizeof(C));
	if (!H.rom_path[0]) {
		H.content_path[0] = 0;
		hstrlcpy(H.game, H.core_id, sizeof(H.game));
		return 0;
	}
	if (!hfile_exists(H.rom_path)) {
		snprintf(err, n, "%s", _("Game file not found"));
		return -ENOENT;
	}
	if (H.info.no_content) {
		/* A game built into the core (RetroStone VC, docs/vc-games.md):
		 * the file is only its menu entry. retro_load_game(NULL); the
		 * saves, states and resume are named after the entry, as for a
		 * ROM ("Bomber Mole.bombermole" -> "Bomber Mole.srm"). */
		H.content_path[0] = 0;
		hpath_stem(H.rom_path, H.game, sizeof(H.game));
		hlog(HLOG_INFO, "content: none (%s is a menu entry: the game is built into the core)",
		     hpath_base(H.rom_path));
		return 0;
	}
	hstrlcpy(H.content_path, H.rom_path, sizeof(H.content_path));
	hpath_stem(H.rom_path, H.game, sizeof(H.game));
	hpath_ext(H.rom_path, ext, sizeof(ext));
	if ((!strcmp(ext, "zip") || !strcmp(ext, "7z")) && !H.info.block_extract &&
	    !hlist_has(valid_exts(), ext)) {
		int ret;

		if (!strcmp(ext, "7z")) {
			snprintf(err, n, "%s", _("7z archives are not supported here: use .zip or unpack it"));
			return -ENOTSUP;
		}
		ret = extract_zip(H.rom_path, err, n);
		if (ret)
			return ret;
		/* RetroArch names saves after the file inside the archive. */
		hpath_stem(H.content_path, H.game, sizeof(H.game));
	}
	return 0;
}

int content_load(struct retro_game_info *gi, char *err, size_t n)
{
	char ext[32];

	memset(gi, 0, sizeof(*gi));
	if (!H.content_path[0])
		return 0;
	hpath_ext(H.content_path, ext, sizeof(ext));
	gi->path = H.content_path;
	if (!need_fullpath_for(ext)) {
		H.content_data = hread_file(H.content_path, &H.content_size);
		if (!H.content_data) {
			snprintf(err, n, _("Cannot read the game file (%s)"), strerror(errno));
			return -EIO;
		}
		gi->data = H.content_data;
		gi->size = H.content_size;
		hlog(HLOG_INFO, "content in memory: %zu bytes", H.content_size);
	}

	/* GET_GAME_INFO_EXT */
	hpath_dir(C.archive[0] ? C.archive : H.content_path, C.dir, sizeof(C.dir));
	hpath_stem(H.content_path, C.name, sizeof(C.name));
	hstrlcpy(C.ext, ext, sizeof(C.ext));
	memset(&C.ext_info, 0, sizeof(C.ext_info));
	C.ext_info.full_path = C.extracted[0] ? NULL : H.content_path;
	C.ext_info.archive_path = C.archive[0] ? C.archive : NULL;
	C.ext_info.archive_file = C.archive[0] ? C.member : NULL;
	C.ext_info.dir = C.dir;
	C.ext_info.name = C.name;
	C.ext_info.ext = C.ext;
	C.ext_info.data = gi->data;
	C.ext_info.size = gi->size;
	C.ext_info.file_in_archive = C.archive[0] != 0;
	C.ext_info.persistent_data = false;
	if (C.extracted[0])
		C.ext_info.full_path = H.content_path; /* extracted copy on tmpfs */
	C.have_ext = true;
	return 0;
}

bool content_game_info_ext(const struct retro_game_info_ext **ext)
{
	if (!C.have_ext || !ext)
		return false;
	*ext = &C.ext_info;
	return true;
}

void content_cleanup(void)
{
	free(H.content_data);
	H.content_data = NULL;
	H.content_size = 0;
	if (C.extracted[0]) {
		unlink(C.extracted);
		rmdir(C.tmpdir);
		C.extracted[0] = 0;
	}
	C.have_ext = false;
}

/* ------------------------------------------------------- UI helpers */

void host_game_name(const char *core_path, const char *rom_path, char *name, size_t n)
{
	char id[64], ext[16];
	struct core_info ci;
	mz_zip_archive za;

	hpath_stem(rom_path, name, n);
	hpath_ext(rom_path, ext, sizeof(ext));
	if (strcmp(ext, "zip") != 0)
		return;
	coreinfo_id_from_path(core_path, id, sizeof(id));
	coreinfo_load(&ci, NULL, id);
	if (!ci.block_extract && !hlist_has(ci.extensions, "zip")) {
		memset(&za, 0, sizeof(za));
		if (mz_zip_reader_init_file(&za, rom_path, 0)) {
			mz_uint count = mz_zip_reader_get_num_files(&za), files = 0;
			mz_zip_archive_file_stat st, only;

			for (mz_uint i = 0; i < count; i++) {
				char e[32];

				if (!mz_zip_reader_file_stat(&za, i, &st) || st.m_is_directory)
					continue;
				files++;
				only = st;
				hpath_ext(st.m_filename, e, sizeof(e));
				if (hlist_has(ci.extensions, e)) {
					hpath_stem(st.m_filename, name, n);
					files = 0;
					break;
				}
			}
			if (files == 1)
				hpath_stem(only.m_filename, name, n);
			mz_zip_reader_end(&za);
		}
	}
	coreinfo_free(&ci);
}

bool host_auto_state_path(const char *states_root, const char *core_path, const char *rom_path,
			  const char *system, char *path, size_t n)
{
	char game[256], sys[64], dir[PATH_MAX];

	if (n)
		path[0] = 0;
	if (!rom_path)
		return false;
	host_game_name(core_path, rom_path, game, sizeof(game));
	if (system && *system) {
		hstrlcpy(sys, system, sizeof(sys));
	} else {
		hpath_dir(rom_path, dir, sizeof(dir));
		hstrlcpy(sys, hpath_base(dir), sizeof(sys));
	}
	return hpath(path, n, "%s/%s/%s.state.auto", states_root && *states_root ? states_root : "/data/states",
		     sys, game) && hfile_exists(path);
}

bool host_has_resume_state(const char *core_path, const char *rom_path, const char *system)
{
	char p[PATH_MAX];

	return host_auto_state_path(NULL, core_path, rom_path, system, p, sizeof(p));
}
