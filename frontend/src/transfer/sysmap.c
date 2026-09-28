/*
 * sysmap.c - canonical ROM folder names and the folder aliases used by
 * other firmwares (RetroPie, Batocera/KNULLI/ROCKNIX, Onion, MinUI, muOS,
 * EmuDeck).
 *
 * The canonical list must match the UI's system table (src/ui/systems.c)
 * and the folders created on /data (docs/cores.md "ROM folders").
 */
#include <ctype.h>
#include <string.h>

#include "tr_internal.h"

struct sysmap {
	const char *id;
	const char *name;
	/* normalized aliases (lowercase, only [a-z0-9]), NULL-terminated */
	const char *alias[10];
};

static const struct sysmap g_sys[] = {
	{ "nes", "NES / Famicom", { "famicom", "fc", NULL } },
	{ "fds", "Famicom Disk System", { "famicomdisksystem", "disksystem", NULL } },
	{ "snes", "Super Nintendo", { "sfc", "superfamicom", "supernintendo", "supernes", "snesna", "snesmsu1",
				       NULL } },
	{ "n64", "Nintendo 64", { "nintendo64", NULL } },
	{ "gb", "Game Boy", { "gameboy", NULL } },
	{ "gbc", "Game Boy Color", { "gameboycolor", NULL } },
	{ "gba", "Game Boy Advance", { "gameboyadvance", NULL } },
	{ "sg1000", "SG-1000", { "sg", "segasg1000", NULL } },
	{ "mastersystem", "Master System", { "sms", "ms", "segamastersystem", "markiii", NULL } },
	{ "megadrive", "Mega Drive / Genesis", { "genesis", "md", "segagenesis", "segamegadrive",
						 "megadrivejapan", NULL } },
	{ "segacd", "Mega-CD / Sega CD", { "megacd", "segamegacd", "scd", NULL } },
	{ "sega32x", "32X", { "32x", "thirtytwox", "sega32", NULL } },
	{ "gamegear", "Game Gear", { "gg", "segagamegear", NULL } },
	{ "pico", "Sega Pico", { "segapico", NULL } },
	{ "psx", "PlayStation", { "ps1", "ps", "playstation", "psone", "sonyplaystation", NULL } },
	{ "pcengine", "PC Engine", { "pce", "tg16", "turbografx16", "turbografx", "pcenginecd", NULL } },
	{ "atari2600", "Atari 2600", { "a2600", "2600", "atarivcs", NULL } },
	{ "arcade", "Arcade (MAME 2003-Plus)", { "mame", "mamelibretro", "mame2003", "mame2003plus",
						 "mamemame4all", "mame4all", "mame078", NULL } },
	{ "fbneo", "Arcade (FinalBurn Neo)", { "fba", "fbalpha", "finalburnneo", "finalburnalpha",
					       "fbneoarcade", NULL } },
	{ "neogeo", "Neo Geo", { "snkneogeo", NULL } },
	/* third batch (docs/cores.md "Third batch"): the folder names of RetroPie,
	 * Batocera/KNULLI/ROCKNIX, Onion/MinUI tags and EmuDeck */
	{ "supergrafx", "SuperGrafx", { "sgfx", "pcenginesupergrafx", "sgx", NULL } },
	{ "pokemini", "Pok\xc3\xa9mon mini", { "pokemonmini", "pkm", NULL } },
	{ "c64", "Commodore 64", { "commodore64", "commodore", "c64cart", NULL } },
	{ "zxspectrum", "ZX Spectrum", { "spectrum", "zx", "sinclairzxspectrum", "zxs", NULL } },
	{ "amstradcpc", "Amstrad CPC", { "cpc", "amstrad", NULL } },
	{ "dos", "MS-DOS", { "pc", "msdos", "dosbox", "ibmpc", NULL } },
	{ "scummvm", "ScummVM", { "scumm", NULL } },
	/* not "pico": that is the Sega Pico's folder */
	{ "pico8", "PICO-8", { "p8", "fake08", NULL } },
	{ "doom", "Doom", { "prboom", "gzdoom", NULL } },
	{ "cavestory", "Cave Story", { "nxengine", "doukutsu", NULL } },
};

#define NSYS ((int)(sizeof(g_sys) / sizeof(g_sys[0])))

int transfer_system_count(void)
{
	return NSYS;
}

const char *transfer_system_id(int i)
{
	return i >= 0 && i < NSYS ? g_sys[i].id : NULL;
}

const char *transfer_system_name(int i)
{
	return i >= 0 && i < NSYS ? g_sys[i].name : NULL;
}

int transfer_system_index(const char *id)
{
	if (!id)
		return -1;
	for (int i = 0; i < NSYS; i++)
		if (!strcmp(g_sys[i].id, id))
			return i;
	return -1;
}

/* Lowercase, keep only [a-z0-9]. */
static void normalize(const char *in, size_t len, char *out, size_t n)
{
	size_t o = 0;

	for (size_t i = 0; i < len && in[i] && o + 1 < n; i++) {
		unsigned char c = (unsigned char)in[i];

		if (isalnum(c) && c < 0x80)
			out[o++] = (char)tolower(c);
	}
	out[o] = 0;
}

static const char *lookup(const char *norm)
{
	if (!norm[0])
		return NULL;
	for (int i = 0; i < NSYS; i++) {
		char id[TRANSFER_SYSID_MAX];

		normalize(g_sys[i].id, strlen(g_sys[i].id), id, sizeof(id));
		if (!strcmp(id, norm))
			return g_sys[i].id;
		for (int a = 0; g_sys[i].alias[a]; a++)
			if (!strcmp(g_sys[i].alias[a], norm))
				return g_sys[i].id;
	}
	return NULL;
}

const char *transfer_system_canon(const char *folder)
{
	char norm[96];
	const char *id, *open, *close;

	if (!folder || !folder[0])
		return NULL;
	normalize(folder, strlen(folder), norm, sizeof(norm));
	if ((id = lookup(norm)))
		return id;
	/* MinUI / Onion style: "Super Nintendo Entertainment System (SFC)" */
	close = strrchr(folder, ')');
	open = close ? close : NULL;
	while (open && open > folder && *open != '(')
		open--;
	if (close && open && *open == '(' && close > open + 1) {
		normalize(open + 1, (size_t)(close - open - 1), norm, sizeof(norm));
		if ((id = lookup(norm)))
			return id;
	}
	return NULL;
}
