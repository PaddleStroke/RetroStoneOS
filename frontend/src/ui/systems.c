/*
 * systems.c - see systems.h.
 */
#include "systems.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../i18n/i18n.h"
#include "util.h"

/* Carousel order: the RetroStone VC games first (our own games), then by
 * maker and by age; the consoles and handhelds, then
 * the home computers (and the PC game engines), the arcade systems, the
 * fantasy console and the ports (third batch of cores, docs/cores.md). */
/* Display names: English, marked for the translations' per-language table of
 * regional names (C_("system", ...) where shown; most languages keep them). */
static const struct sysdef g_systems[] = {
	/* First: our own games (RetroStone VC, docs/vc-games.md). One core per
	 * game, each with its own entry extension; the entries are in the
	 * read-only /usr/share/rsos/games/retrostone/, not on the card. */
	{ "retrostone", NC_("system", "RetroStone"), "8BCraft", { "retrostone", NULL }, { NULL }, "", "", 2026 },
	{ "nes", NC_("system", "Nintendo Entertainment System"), "Nintendo", { "nes", "famicom", NULL }, { "famicom", NULL }, "fceumm", "nes,unf,unif", 1983 },
	{ "fds", NC_("system", "Famicom Disk System"), "Nintendo", { "fds", "famicom", "nes", NULL }, { NULL }, "fceumm", "fds", 1986 },
	{ "snes", NC_("system", "Super Nintendo"), "Nintendo", { "snes", "sfc", NULL }, { "sfc", "superfamicom", NULL }, "snes9x2005", "smc,sfc,fig,swc", 1990 },
	{ "n64", NC_("system", "Nintendo 64"), "Nintendo", { "n64", NULL }, { NULL }, "parallel_n64", "n64,z64,v64", 1996 },
	{ "gb", NC_("system", "Game Boy"), "Nintendo", { "gb", NULL }, { "gameboy", NULL }, "gambatte", "gb,dmg", 1989 },
	{ "gbc", NC_("system", "Game Boy Color"), "Nintendo", { "gbc", NULL }, { "gameboycolor", NULL }, "gambatte", "gbc", 1998 },
	{ "gba", NC_("system", "Game Boy Advance"), "Nintendo", { "gba", NULL }, { "gameboyadvance", NULL }, "gpsp", "gba", 2001 },
	{ "pokemini", NC_("system", "Pokémon mini"), "Nintendo", { "pokemini", NULL }, { "pokemonmini", NULL }, "pokemini", "min", 2001 },
	{ "sg1000", NC_("system", "SG-1000"), "Sega", { "sg-1000", "sg1000", NULL }, { "sg-1000", NULL }, "picodrive", "sg", 1983 },
	{ "mastersystem", NC_("system", "Master System"), "Sega", { "mastersystem", "sms", NULL }, { "sms", NULL }, "picodrive", "sms", 1985 },
	{ "megadrive", NC_("system", "Mega Drive"), "Sega", { "megadrive", "genesis", NULL }, { "genesis", "md", NULL }, "picodrive", "md,gen,smd,bin", 1988 },
	{ "segacd", NC_("system", "Mega-CD"), "Sega", { "segacd", "megacd", NULL }, { "megacd", NULL }, "picodrive", "cue,chd,iso,m3u", 1991 },
	{ "sega32x", NC_("system", "32X"), "Sega", { "sega32x", "32x", NULL }, { "32x", NULL }, "picodrive", "32x", 1994 },
	{ "pico", NC_("system", "Pico"), "Sega", { "pico", NULL }, { NULL }, "picodrive", "md,bin", 1993 },
	{ "gamegear", NC_("system", "Game Gear"), "Sega", { "gamegear", NULL }, { "gg", NULL }, "picodrive", "gg", 1990 },
	{ "psx", NC_("system", "PlayStation"), "Sony", { "psx", "ps1", NULL }, { "ps1", "playstation", NULL }, "pcsx_rearmed", "cue,bin,chd,pbp,m3u,iso,img", 1994 },
	{ "pcengine", NC_("system", "PC Engine"), "NEC", { "pcengine", "tg16", NULL }, { "tg16", NULL }, "mednafen_pce_fast", "pce", 1987 },
	{ "pcenginecd", NC_("system", "PC Engine CD"), "NEC", { "pcenginecd", "pce-cd", "tg-cd", NULL }, { "pce-cd", "tg-cd", NULL }, "mednafen_pce_fast", "cue,chd", 1988 },
	{ "supergrafx", NC_("system", "SuperGrafx"), "NEC", { "supergrafx", "sgfx", "pcengine", NULL }, { "sgfx", NULL }, "mednafen_supergrafx", "sgx,pce", 1989 },
	{ "ngp", NC_("system", "Neo Geo Pocket"), "SNK", { "ngp", NULL }, { NULL }, "mednafen_ngp", "ngp", 1998 },
	{ "ngpc", NC_("system", "Neo Geo Pocket Color"), "SNK", { "ngpc", NULL }, { NULL }, "mednafen_ngp", "ngc", 1999 },
	{ "wonderswan", NC_("system", "WonderSwan"), "Bandai", { "wonderswan", NULL }, { NULL }, "mednafen_wswan", "ws", 1999 },
	{ "wonderswancolor", NC_("system", "WonderSwan Color"), "Bandai", { "wonderswancolor", NULL }, { NULL }, "mednafen_wswan", "wsc", 2000 },
	{ "atari2600", NC_("system", "Atari 2600"), "Atari", { "atari2600", NULL }, { NULL }, "stella2014", "a26", 1977 },
	{ "atari7800", NC_("system", "Atari 7800"), "Atari", { "atari7800", NULL }, { NULL }, "prosystem", "a78", 1986 },
	{ "atarilynx", NC_("system", "Lynx"), "Atari", { "atarilynx", "lynx", NULL }, { "lynx", NULL }, "handy", "lnx", 1989 },
	{ "coleco", NC_("system", "ColecoVision"), "Coleco", { "coleco", "colecovision", NULL }, { "colecovision", NULL }, "gearcoleco", "col", 1982 },
	/* home computers, then the PC and its game engine */
	{ "c64", NC_("system", "Commodore 64"), "Commodore", { "c64", NULL }, { "commodore64", NULL }, "vice_x64", "d64,g64,t64,tap,prg,crt", 1982 },
	{ "zxspectrum", NC_("system", "ZX Spectrum"), "Sinclair", { "zxspectrum", "zx", NULL }, { "spectrum", "zx", NULL }, "fuse", "tzx,tap,z80,sna,szx,dsk,trd", 1982 },
	{ "msx", NC_("system", "MSX"), "Home computer", { "msx", NULL }, { "msx1", NULL }, "bluemsx", "rom,mx1,mx2,dsk", 1983 },
	{ "amstradcpc", NC_("system", "Amstrad CPC"), "Amstrad", { "amstradcpc", "cpc", NULL }, { "cpc", "amstrad", NULL }, "cap32", "dsk,sna,cdt,tap,cpr", 1984 },
	{ "dos", NC_("system", "MS-DOS"), "PC", { "dos", "pc", NULL }, { "pc", "msdos", NULL }, "dosbox_pure", "zip,dosz,iso,cue,chd,img", 1981 },
	{ "scummvm", NC_("system", "ScummVM"), "Game engine", { "scummvm", NULL }, { NULL }, "scummvm", "scummvm", 2001 },
	{ "arcade", NC_("system", "Arcade"), "MAME 2003-Plus", { "arcade", "mame-libretro", "mame", NULL }, { "mame-libretro", "mame2003", "mame", NULL }, "mame2003_plus", "zip", 0 },
	{ "fbneo", NC_("system", "FinalBurn Neo"), "Arcade", { "fbneo", "fba", "arcade", NULL }, { "fba", "fba_libretro", NULL }, "fbneo", "zip,7z", 0 },
	{ "neogeo", NC_("system", "Neo Geo"), "SNK", { "neogeo", NULL }, { NULL }, "fbneo", "zip,7z", 1990 },
	{ "neocd", NC_("system", "Neo Geo CD"), "SNK", { "neocd", "neogeocd", NULL }, { "neogeocd", NULL }, "geolith", "cue,chd", 1994 },
	/* fantasy console, then the ports (a game engine and its data) */
	{ "pico8", NC_("system", "PICO-8"), "Fantasy console", { "pico8", "pico-8", NULL }, { "pico-8", NULL }, "fake08", "p8,png", 2015 },
	{ "doom", NC_("system", "Doom"), "Ports", { "doom", "ports", NULL }, { "prboom", NULL }, "prboom", "wad,iwad,pwad", 1993 },
	{ "cavestory", NC_("system", "Cave Story"), "Ports", { "cavestory", "ports", NULL }, { "nxengine", NULL }, "nxengine", "exe", 2004 },
};

/* Used only when no core .ini is installed (development host). */
static const struct core_info g_fallback_cores[] = {
	{ "fceumm", "FCEUmm", "/usr/lib/libretro/fceumm_libretro.so", "nes,fds", "nes,fds,unf,unif", false, true, false, "", 0 },
	{ "snes9x2005", "Snes9x 2005", "/usr/lib/libretro/snes9x2005_libretro.so", "snes", "smc,fig,sfc,gd3,gd7,dx2,bsx,swc", false, true, false, "", 0 },
	{ "gambatte", "Gambatte", "/usr/lib/libretro/gambatte_libretro.so", "gb,gbc", "gb,gbc,dmg", false, true, false, "", 0 },
	{ "gpsp", "gpSP", "/usr/lib/libretro/gpsp_libretro.so", "gba", "gba,bin,agb,gbz,u1", false, true, false, "", 0 },
	{ "picodrive", "PicoDrive", "/usr/lib/libretro/picodrive_libretro.so", "megadrive,mastersystem,gamegear,sg1000,sega32x,segacd",
	  "bin,gen,smd,md,32x,cue,iso,chd,m3u,sms,gg,sg", false, true, false, "", 0 },
	{ "pcsx_rearmed", "PCSX ReARMed", "/usr/lib/libretro/pcsx_rearmed_libretro.so", "psx", "bin,cue,img,mdf,pbp,toc,cbn,m3u,chd,iso,exe", false, true, false, "", 0 },
	{ "mame2003_plus", "MAME 2003-Plus", "/usr/lib/libretro/mame2003_plus_libretro.so", "arcade", "zip", true, true, false, "", 0 },
	{ "fbneo", "FinalBurn Neo", "/usr/lib/libretro/fbneo_libretro.so", "fbneo,neogeo", "zip,7z", true, true, false, "", 0 },
};

static struct core_info *g_cores;
static int g_ncores;
/* the systems the installed cores can run */
static const struct sysdef **g_list;
static int g_nlist;
static struct sysdef *g_extra;   /* systems named in a .ini but not in g_systems */
static int g_nextra;
static char **g_extra_names;

static void build_list(void);

static bool list_has(const char *list, const char *item)
{
	size_t l = strlen(item);
	const char *p = list;

	while (*p) {
		while (*p == ',' || *p == ' ')
			p++;
		if (!strncasecmp(p, item, l) && (p[l] == ',' || p[l] == ' ' || p[l] == 0))
			return true;
		while (*p && *p != ',')
			p++;
	}
	return false;
}

/* Normalizes "a, b ,c" to "a,b,c" (lowercase). */
static void norm_list(char *s)
{
	char *r = s, *w = s;

	for (; *r; r++)
		if (*r != ' ' && *r != '\t')
			*w++ = (char)tolower((unsigned char)*r);
	*w = 0;
}

static void load_ini(const char *path)
{
	char *buf = file_read(path, NULL), *save = NULL, *line;
	struct core_info ci;
	bool in_core = false;

	if (!buf)
		return;
	memset(&ci, 0, sizeof(ci));
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *c = strchr(line, ';'), *eq, *k, *v;

		if (c)
			*c = 0;
		line = str_trim(line);
		if (line[0] == '[') {
			in_core = !strncmp(line, "[core]", 6);
			continue;
		}
		if (!in_core || !(eq = strchr(line, '=')))
			continue;
		*eq = 0;
		k = str_trim(line);
		v = str_trim(eq + 1);
		if (!strcmp(k, "id"))
			strlcpy_(ci.id, v, sizeof(ci.id));
		else if (!strcmp(k, "display_name"))
			strlcpy_(ci.name, v, sizeof(ci.name));
		else if (!strcmp(k, "library"))
			strlcpy_(ci.library, v, sizeof(ci.library));
		else if (!strcmp(k, "systems"))
			strlcpy_(ci.systems, v, sizeof(ci.systems));
		else if (!strcmp(k, "extensions"))
			strlcpy_(ci.exts, v, sizeof(ci.exts));
		else if (!strcmp(k, "block_extract"))
			ci.block_extract = parse_bool(v, false);
		else if (!strcmp(k, "savestates"))
			ci.savestates = parse_bool(v, false);
		else if (!strcmp(k, "experimental"))
			ci.experimental = parse_bool(v, false);
		else if (!strcmp(k, "cpu_governor"))
			strlcpy_(ci.cpu_governor, v, sizeof(ci.cpu_governor));
		else if (!strcmp(k, "cpu_max_khz"))
			ci.cpu_max_khz = atoi(v);
	}
	free(buf);
	if (!ci.id[0]) {
		LOGW("cores: %s has no id", path);
		return;
	}
	norm_list(ci.systems);
	norm_list(ci.exts);
	if (!ci.name[0])
		strlcpy_(ci.name, ci.id, sizeof(ci.name));
	g_cores = xrealloc(g_cores, sizeof(*g_cores) * (size_t)(g_ncores + 1));
	g_cores[g_ncores++] = ci;
}

static int cmp_core(const void *a, const void *b)
{
	return strcmp(((const struct core_info *)a)->id, ((const struct core_info *)b)->id);
}

int systems_load_cores(const char *dir)
{
	DIR *d = dir ? opendir(dir) : NULL;
	struct dirent *de;

	systems_free();
	if (d) {
		while ((de = readdir(d))) {
			char p[1024];

			if (!str_ends_i(de->d_name, ".ini"))
				continue;
			snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
			load_ini(p);
		}
		closedir(d);
	}
	if (!g_ncores) {
		LOGW("cores: no core .ini in %s, using the built-in table", dir ? dir : "(null)");
		g_ncores = (int)ARRAY_SIZE(g_fallback_cores);
		g_cores = xmalloc(sizeof(g_fallback_cores));
		memcpy(g_cores, g_fallback_cores, sizeof(g_fallback_cores));
	}
	qsort(g_cores, (size_t)g_ncores, sizeof(*g_cores), cmp_core);
	build_list();
	return g_ncores;
}

void systems_free(void)
{
	free(g_cores);
	g_cores = NULL;
	g_ncores = 0;
	for (int i = 0; i < g_nextra; i++)
		free(g_extra_names[i]);
	free(g_extra_names);
	free(g_extra);
	g_extra = NULL;
	g_extra_names = NULL;
	g_nextra = 0;
	g_nlist = 0;
}

int systems_count(void)
{
	return g_nlist;
}

const struct sysdef *systems_get(int i)
{
	return i >= 0 && i < g_nlist ? g_list[i] : NULL;
}

const struct sysdef *systems_find(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_systems); i++)
		if (!strcmp(g_systems[i].name, name))
			return &g_systems[i];
	for (int i = 0; i < g_nextra; i++)
		if (!strcmp(g_extra[i].name, name))
			return &g_extra[i];
	return NULL;
}

static bool any_core_runs(const char *system)
{
	for (int i = 0; i < g_ncores; i++)
		if (list_has(g_cores[i].systems, system))
			return true;
	return false;
}

static void build_list(void)
{
	g_list = xrealloc(g_list, sizeof(*g_list) * (ARRAY_SIZE(g_systems) + 64));
	g_nlist = 0;
	for (size_t i = 0; i < ARRAY_SIZE(g_systems); i++) {
		if (any_core_runs(g_systems[i].name))
			g_list[g_nlist++] = &g_systems[i];
		else
			LOGD("systems: no core for %s", g_systems[i].name);
	}
	/* systems only known from a core .ini */
	for (int c = 0; c < g_ncores; c++) {
		char buf[256], *save = NULL, *tok;

		strlcpy_(buf, g_cores[c].systems, sizeof(buf));
		for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
			struct sysdef *sd;
			char *nm;

			if (!*tok || systems_find(tok) || g_nextra >= 64)
				continue;
			g_extra = xrealloc(g_extra, sizeof(*g_extra) * 64);
			g_extra_names = xrealloc(g_extra_names, sizeof(char *) * 64);
			nm = xstrdup(tok);
			g_extra_names[g_nextra] = nm;
			sd = &g_extra[g_nextra++];
			memset(sd, 0, sizeof(*sd));
			sd->name = nm;
			sd->fullname = nm;
			sd->maker = "";
			sd->theme[0] = nm;
			sd->default_core = g_cores[c].id;
			sd->exts = "";
			LOGI("systems: %s (from %s.ini) has no built-in description", nm, g_cores[c].id);
		}
	}
	/* g_extra may have moved: rebuild the tail of the list */
	for (int i = 0; i < g_nextra; i++)
		g_list[g_nlist++] = &g_extra[i];
}

int systems_cores_for(const char *system, const struct core_info **out, int max)
{
	const struct sysdef *sd = systems_find(system);
	int n = 0;

	/* 1. the table default (explicit, may be experimental if it is the
	 *    only core, e.g. neocd) */
	if (sd && sd->default_core[0]) {
		const struct core_info *c = systems_core(sd->default_core);

		if (c && list_has(c->systems, system) && n < max)
			out[n++] = c;
	}
	/* 2. stable cores, 3. experimental cores */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < g_ncores && n < max; i++) {
			bool dup = false;

			if (g_cores[i].experimental != (pass == 1) ||
			    !list_has(g_cores[i].systems, system))
				continue;
			for (int k = 0; k < n; k++)
				if (out[k] == &g_cores[i])
					dup = true;
			if (!dup)
				out[n++] = &g_cores[i];
		}
	}
	return n;
}

int systems_cores_for_file(const char *system, const char *path,
			   const struct core_info **out, int max)
{
	const struct core_info *all[MAX_CORES_PER_SYSTEM];
	int n = systems_cores_for(system, all, MAX_CORES_PER_SYSTEM), k = 0;
	const char *dot = strrchr(path_basename(path), '.');
	const char *ext = dot ? dot + 1 : "";
	bool archive = !strcasecmp(ext, "zip") || !strcasecmp(ext, "7z");

	for (int i = 0; i < n && k < max; i++)
		if (list_has(all[i]->exts, ext) || (archive && !all[i]->block_extract))
			out[k++] = all[i];
	return k;
}

const struct core_info *systems_core(const char *id)
{
	for (int i = 0; i < g_ncores; i++)
		if (!strcmp(g_cores[i].id, id))
			return &g_cores[i];
	return NULL;
}

bool systems_ext_ok(const char *system, const char *ext)
{
	const struct core_info *cs[MAX_CORES_PER_SYSTEM];
	int n = systems_cores_for(system, cs, MAX_CORES_PER_SYSTEM);

	for (int i = 0; i < n; i++)
		if (list_has(cs[i]->exts, ext))
			return true;
	/* Cartridge collections are often zipped: the libretro host extracts
	 * them for cores with block_extract = false. */
	if (n && !cs[0]->block_extract && (!strcasecmp(ext, "zip") || !strcasecmp(ext, "7z")))
		return true;
	return false;
}
