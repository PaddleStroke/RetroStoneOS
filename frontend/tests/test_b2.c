/*
 * test_b2.c - batch 2 unit tests of the UI data layer: the new systems'
 * carousel order and names (the real core .ini files), the letter groups of
 * the jump-to-letter keys, the search match (case and accents ignored), the
 * per-game data in gamedb.tsv (play time, hidden, scaling, CPU profile; an
 * older 6-column file still reads), the sort orders and the hidden games.
 *
 * usage: test_b2 WORKDIR CORES_DIR   (CORES_DIR: the core .ini files)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/i18n/i18n.h"
#include "../src/ui/games.h"
#include "../src/ui/systems.h"
#include "../src/ui/util.h"

static int failures;

#define CHECK(cond, ...)                                           \
	do {                                                       \
		int ok_ = (cond);                                  \
		printf(ok_ ? "  ok    " : "  FAIL  ");             \
		if (!ok_)                                          \
			failures++;                                \
		printf(__VA_ARGS__);                               \
		printf("\n");                                      \
	} while (0)

static int pos(const char *name)
{
	for (int i = 0; i < systems_count(); i++)
		if (!strcmp(systems_get(i)->name, name))
			return i;
	return -1;
}

static void test_systems(const char *cores)
{
	static const struct { const char *name, *full, *core; } nw[] = {
		{ "supergrafx", "SuperGrafx", "mednafen_supergrafx" }, { "pokemini", "Pokémon mini", "pokemini" },
		{ "dos", "MS-DOS", "dosbox_pure" }, { "scummvm", "ScummVM", "scummvm" },
		{ "c64", "Commodore 64", "vice_x64" }, { "zxspectrum", "ZX Spectrum", "fuse" },
		{ "amstradcpc", "Amstrad CPC", "cap32" }, { "pico8", "PICO-8", "fake08" },
		{ "doom", "Doom", "prboom" }, { "cavestory", "Cave Story", "nxengine" },
	};
	char order[512] = "";

	printf("the new systems (third batch of cores): names, cores, carousel order\n");
	CHECK(systems_load_cores(cores) >= 30, "%d cores from %s", systems_count(), cores);
	for (size_t i = 0; i < sizeof(nw) / sizeof(nw[0]); i++) {
		const struct sysdef *sd = systems_find(nw[i].name);
		const struct core_info *cs[MAX_CORES_PER_SYSTEM];
		int n = systems_cores_for(nw[i].name, cs, MAX_CORES_PER_SYSTEM);

		CHECK(sd && !strcmp(sd->fullname, nw[i].full) && sd->maker[0] && sd->year > 1900 && n > 0 &&
		      !strcmp(cs[0]->id, nw[i].core) && pos(nw[i].name) >= 0,
		      "%s: \"%s\" (%s, %d), default core %s, in the carousel at %d", nw[i].name, sd ? sd->fullname : "?",
		      sd ? sd->maker : "?", sd ? sd->year : 0, n ? cs[0]->id : "none", pos(nw[i].name));
	}
	for (int i = 0; i < systems_count(); i++)
		snprintf(order + strlen(order), sizeof(order) - strlen(order), "%s%s", i ? " " : "", systems_get(i)->name);
	printf("        order: %s\n", order);
	CHECK(pos("pokemini") == pos("gba") + 1, "Pokémon mini right after the Game Boy Advance");
	CHECK(pos("supergrafx") == pos("pcenginecd") + 1, "SuperGrafx right after the PC Engine CD");
	CHECK(pos("c64") < pos("zxspectrum") && pos("zxspectrum") < pos("msx") && pos("msx") < pos("amstradcpc") &&
	      pos("amstradcpc") < pos("dos") && pos("dos") < pos("scummvm") && pos("c64") > pos("coleco") &&
	      pos("scummvm") < pos("arcade"),
	      "the computers together (C64, Spectrum, MSX, CPC, then DOS and ScummVM), before the arcade systems");
	CHECK(pos("pico8") > pos("neocd") && pos("doom") == pos("pico8") + 1 && pos("cavestory") == pos("doom") + 1,
	      "PICO-8, then the ports (Doom, Cave Story), after the arcade systems");
	CHECK(systems_find("dos")->theme[1] && !strcmp(systems_find("dos")->theme[1], "pc") &&
	      !strcmp(systems_find("doom")->theme[1], "ports") && !strcmp(systems_find("supergrafx")->theme[2], "pcengine"),
	      "theme folders: dos -> pc, doom -> ports, supergrafx -> pcengine (sets without their own)");
	CHECK(!strcmp(systems_find("dos")->folders[0], "pc"), "the RetroPie folder pc/ is read as dos");
}

static void test_letters(void)
{
	static const struct { const char *name, *letter; } t[] = {
		{ "Adventure Island", "A" }, { "adventure", "A" }, { "Écran", "E" }, { "'Splosion Man", "S" },
		{ "1942", "#" }, { "(Beta) Zelda", "B" }, { "ÖDE", "O" }, { "Łódź", "L" }, { "ドンキー", "ド" },
		{ "", "#" },
	};

	printf("jump to letter: the letter groups\n");
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		char l[8];

		games_letter(t[i].name, l, sizeof(l));
		CHECK(!strcmp(l, t[i].letter), "\"%s\" -> %s", t[i].name, l);
	}
}

static void test_search(void)
{
	printf("search: case and accents ignored, anywhere in the name\n");
	CHECK(games_match("Super Mario Bros.", "mario"), "mario in Super Mario Bros.");
	CHECK(games_match("Pokémon Pinball", "POKEMON"), "POKEMON in Pokémon Pinball");
	CHECK(games_match("Pokemon Pinball", "pokém"), "pokém in Pokemon Pinball");
	CHECK(!games_match("Tetris", "mario"), "mario not in Tetris");
	CHECK(games_match("Tetris", ""), "an empty search matches everything");
	CHECK(games_match("Ōkami", "oka") && games_match("Straße", "strasse") == false,
	      "Ō folds to o; ß is not split (a simple fold)");
	CHECK(games_match("ドンキーコング", "キー"), "other scripts: the characters themselves");
}

static struct game mkgame(const char *sys, const char *rel, const char *name, int64_t pt, int64_t last, bool fav)
{
	struct game g;

	memset(&g, 0, sizeof(g));
	g.system = sys;
	g.rel = rel;
	g.path = rel;
	g.name = name;
	g.playtime = pt;
	g.lastplayed = last;
	g.favorite = fav;
	g.rating = -1;
	return g;
}

static void test_gamedb(const char *dir)
{
	char path[600], *txt;
	struct gamedb *db;
	struct gamelist gl;
	struct game gs[4];

	printf("gamedb.tsv: play time, hidden, per-game scaling and CPU profile\n");
	snprintf(path, sizeof(path), "%s/gamedb.tsv", dir);
	/* an older file (6 columns) reads as before */
	{
		FILE *f = fopen(path, "w");

		fputs("# RetroStoneOS per-game data: system, path, favorite, lastplayed, playcount, core\n"
		      "nes\tA.nes\t1\t100\t3\tfceumm\n", f);
		fclose(f);
	}
	db = gamedb_open(path);
	gs[0] = mkgame("nes", "A.nes", "A", 0, 0, false);
	gs[1] = mkgame("nes", "B.nes", "B", 0, 0, false);
	gs[2] = mkgame("nes", "C.nes", "C", 0, 0, false);
	memset(&gl, 0, sizeof(gl));
	gl.games = gs;
	gl.n = 3;
	gamedb_apply(db, &gl);
	CHECK(gs[0].favorite && gs[0].playcount == 3 && gs[0].playtime == 0 && !gs[0].hidden && !gs[0].scale,
	      "a 6-column line: favorite, play count, no play time");
	gamedb_add_playtime(db, &gs[0], 3600 + 2 * 60);
	gamedb_add_playtime(db, &gs[0], 60);
	gamedb_add_playtime(db, &gs[1], -5);          /* ignored */
	gamedb_set_hidden(db, &gs[2], true);
	gamedb_set_scale(db, &gs[1], "integer");
	gamedb_set_cpu(db, &gs[1], "powersave");
	gamedb_set_cpu(db, &gs[0], "auto");           /* = no override */
	CHECK(gamedb_save(db) == 0, "saved");
	gamedb_close(db);
	txt = file_read(path, NULL);
	CHECK(txt && strstr(txt, "nes\tA.nes\t1\t100\t3\tfceumm\t3780\t0\t\t\n") &&
	      strstr(txt, "nes\tB.nes\t0\t0\t0\t\t0\t0\tinteger\tpowersave\n") && strstr(txt, "nes\tC.nes\t0\t0\t0\t\t0\t1\t\t\n"),
	      "10 columns written");
	free(txt);
	db = gamedb_open(path);
	for (int i = 0; i < 3; i++)
		gs[i] = mkgame("nes", i == 0 ? "A.nes" : i == 1 ? "B.nes" : "C.nes", i == 0 ? "A" : i == 1 ? "B" : "C", 0, 0,
			       false);
	gamedb_apply(db, &gl);
	CHECK(gs[0].playtime == 3780 && gamedb_playtime(db, "nes", "A.nes") == 3780 && !gs[0].cpu,
	      "read back: 1 h 03 min of play");
	CHECK(gs[1].scale && !strcmp(gs[1].scale, "integer") && gs[1].cpu && !strcmp(gs[1].cpu, "powersave"),
	      "read back: B integer, powersave");
	CHECK(gs[2].hidden && games_drop_hidden(&gl) == 1 && gl.n == 2, "C hidden: dropped from the list");
	gamedb_forget(db, "nes", "A.nes");
	CHECK(gamedb_playtime(db, "nes", "A.nes") == 0, "a deleted game is forgotten");
	gamedb_close(db);
}

static void test_sort(void)
{
	struct gamelist gl;
	struct game gs[4];

	printf("sort orders: name, most played, recently played (favorites first)\n");
	gs[0] = mkgame("nes", "a", "Alpha", 60, 5, false);
	gs[1] = mkgame("nes", "b", "Bravo", 7200, 1, false);
	gs[2] = mkgame("nes", "c", "Charlie", 600, 9, true);
	gs[3] = mkgame("nes", "d", "Delta", 7200, 3, false);
	memset(&gl, 0, sizeof(gl));
	gl.games = gs;
	gl.n = 4;
	games_sort_by(&gl, false, GAMES_SORT_PLAYTIME);
	CHECK(!strcmp(gs[0].name, "Bravo") && !strcmp(gs[1].name, "Delta") && !strcmp(gs[2].name, "Charlie") &&
	      !strcmp(gs[3].name, "Alpha"), "most played: Bravo, Delta (same time: by name), Charlie, Alpha");
	games_sort_by(&gl, true, GAMES_SORT_PLAYTIME);
	CHECK(!strcmp(gs[0].name, "Charlie") && !strcmp(gs[1].name, "Bravo"), "favorites first: Charlie, then Bravo");
	games_sort_by(&gl, false, GAMES_SORT_LASTPLAYED);
	CHECK(!strcmp(gs[0].name, "Charlie") && !strcmp(gs[1].name, "Alpha") && !strcmp(gs[3].name, "Bravo"),
	      "recently played: Charlie, Alpha, Delta, Bravo");
	games_sort_by(&gl, false, GAMES_SORT_NAME);
	CHECK(!strcmp(gs[0].name, "Alpha") && !strcmp(gs[3].name, "Delta"), "by name");
	CHECK(games_remove(&gl, "b") && gl.n == 3 && !strcmp(gs[1].name, "Charlie") && !games_remove(&gl, "zz"),
	      "a game removed from a list");
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s WORKDIR CORES_DIR\n", argv[0]);
		return 2;
	}
	mkdir(argv[1], 0755);
	ui_log_set(NULL, NULL, UI_LOG_WARN);
	test_systems(argv[2]);
	test_letters();
	test_search();
	test_gamedb(argv[1]);
	test_sort();
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
