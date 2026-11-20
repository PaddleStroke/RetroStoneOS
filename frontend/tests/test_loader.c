/*
 * test_loader.c - the carousel snapshot and the background game list loader
 * (src/ui/loader.c), on a library shaped like the owner's: 34 ROM folders,
 * five of them with games.
 *
 *   test_loader WORKDIR RES_DIR THEMES_DIR
 *
 * 1. first boot (no snapshot): the carousel only once every list is in;
 *    systems.idx is written
 * 2. normal boot: the carousel comes from the snapshot before the lists
 *    (the loader is slowed down to make that visible), at the saved
 *    position; the final state equals the full load of 1
 * 3. the library changed while off (a system gained games, one lost them,
 *    a gamelist.xml changed): the stale carousel first, then the carousel is
 *    rebuilt keeping the selected system, and the final state equals a full
 *    load without snapshot (4)
 * 5. opening a system before its list arrived loads it on the spot; opening
 *    a collection completes every list first
 * 6. image cache files (.rpx): run-length coded when it pays (backdrops),
 *    raw otherwise; corrupt files are cache misses
 * 7-10. image memory cache, per-game cores, legacy folders, games copied
 *    from a PC
 * 11. Refresh game lists rescans even what the caches call unchanged
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gfx/image.h"
#include "ui/games.h"
#include "ui/systems.h"
#include "ui/ui.h"

static const char *const g_systems[][2] = {
	{ "nes", "nes" }, { "fds", "fds" }, { "snes", "sfc" }, { "n64", "z64" }, { "gb", "gb" },
	{ "gbc", "gbc" }, { "gba", "gba" }, { "sg1000", "sg" }, { "mastersystem", "sms" },
	{ "megadrive", "md" }, { "segacd", "chd" }, { "sega32x", "32x" }, { "pico", "pco" },
	{ "gamegear", "gg" }, { "psx", "pbp" }, { "pcengine", "pce" }, { "pcenginecd", "ccd" },
	{ "ngp", "ngp" }, { "ngpc", "ngc" }, { "wonderswan", "ws" }, { "wonderswancolor", "wsc" },
	{ "atari2600", "a26" }, { "atari7800", "a78" }, { "atarilynx", "lnx" }, { "coleco", "col" },
	{ "msx", "mx1" }, { "doom", "wad" }, { "cavestory", "exe" }, { "arcade", "zip" }, { "fbneo", "7z" }, { "neogeo", "neo" }, { "neocd", "cue" },
};

/* src/ui/ui_internal.h: Settings > Game lists > Refresh game lists (force) */
void ui_reload_games(struct ui *ui, bool force);

static char W[512];
static const char *g_res, *g_themes;
static int g_fail;

#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); g_fail++; } else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *rel, const char *text)
{
	char p[1024];
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", W, rel);
	f = fopen(p, "w");
	if (!f) {
		printf("cannot write %s: %s\n", p, strerror(errno));
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

static void dir(const char *rel)
{
	char p[1024];

	snprintf(p, sizeof(p), "%s/%s", W, rel);
	mkdir(p, 0755);
}

/* What Windows does on an exFAT card: a file added to (name) or removed from
 * (NULL: the last one added here) a folder, the folder's mtime left as it was. */
static void copied_from_pc(const char *folder, const char *name)
{
	static char last[1024];
	char d[1024];
	struct stat st;
	struct timespec keep[2];

	snprintf(d, sizeof(d), "%s/%s", W, folder);
	if (stat(d, &st) < 0) {
		printf("no folder %s\n", d);
		exit(2);
	}
	keep[0] = st.st_atim;
	keep[1] = st.st_mtim;
	if (name) {
		snprintf(last, sizeof(last), "%s/%s", folder, name);
		put(last, "x");
	} else {
		char p[1100];

		snprintf(p, sizeof(p), "%s/%s", W, last);
		unlink(p);
	}
	utimensat(AT_FDCWD, d, keep, 0);
}

static void make_tree(void)
{
	char p[256], ini[512];

	dir("data");
	dir("data/roms");
	dir("data/rsos");
	dir("data/themes");
	dir("cores");
	for (size_t i = 0; i < sizeof(g_systems) / sizeof(g_systems[0]); i++) {
		snprintf(p, sizeof(p), "data/roms/%s", g_systems[i][0]);
		dir(p);
		snprintf(p, sizeof(p), "cores/c_%s.ini", g_systems[i][0]);
		snprintf(ini, sizeof(ini), "[core]\nid = c_%s\nsystems = %s\nextensions = %s\n",
			 g_systems[i][0], g_systems[i][0], g_systems[i][1]);
		put(p, ini);
	}
	/* folders no core uses (the owner's card has a few) */
	dir("data/roms/pcfx");
	dir("data/roms/vectrex");
	dir("data/roms/c64");
	dir("data/roms/amiga");
	put("data/roms/nes/Alter Ego.nes", "x");
	put("data/roms/nes/Blade Buster.nes", "x");
	put("data/roms/nes/gamelist.xml", "<gameList><game><path>./Alter Ego.nes</path><name>Alter Ego</name>"
	    "<desc>A test.</desc></game></gameList>\n");
	put("data/roms/gb/Espionage.gb", "x");
	put("data/roms/gb/Retroid.gb", "x");
	put("data/roms/gb/Tobu.gb", "x");
	put("data/roms/gb/Deadeus.gb", "x");
	put("data/roms/gb/Dangan.gb", "x");
	put("data/roms/gbc/Tuff.gbc", "x");
	put("data/roms/megadrive/Old Towers.md", "x");
	put("data/roms/megadrive/Liquid Space Dodger.md", "x");
	put("data/roms/megadrive/Tanglewood.md", "x");
	put("data/roms/snes/Dottie.sfc", "x");
	put("data/roms/snes/Nekotako.sfc", "x");
	put("data/roms/snes/N-Warp.sfc", "x");
	put("data/roms/snes/Boss Gaiden.sfc", "x");
	put("data/rsos/settings.ini", "last_system=megadrive\n");
	put("data/rsos/gamedb.tsv", "gb\tRetroid.gb\t1\t1790000000\t3\t\n");
}

struct boot {
	char at_menu[2048];      /* ui_debug_state() when the carousel first showed */
	char final[2048];        /* ... once every list was in */
	int64_t menu_us, all_us;
};

static uint32_t g_fb[640 * 480];

static struct ui *start(const char *delay_ms)
{
	static char roms[600], data[600], cache[600], tu[600], cores[600];
	struct ui_config c;
	struct ui *ui;

	snprintf(roms, sizeof(roms), "%s/data/roms", W);
	snprintf(data, sizeof(data), "%s/data/rsos", W);
	snprintf(cache, sizeof(cache), "%s/data/rsos/cache", W);
	snprintf(tu, sizeof(tu), "%s/data/themes", W);
	snprintf(cores, sizeof(cores), "%s/cores", W);
	if (delay_ms)
		setenv("RSOS_LOADER_DELAY_MS", delay_ms, 1);
	else
		unsetenv("RSOS_LOADER_DELAY_MS");
	ui_config_defaults(&c);
	c.roms_dir = roms;
	c.data_dir = data;
	c.cache_dir = cache;
	c.themes_user = tu;
	c.themes_builtin = g_themes;
	c.res_dir = g_res;
	c.cores_dir = cores;
	c.boot_env = "";
	c.net_helper = "/bin/false";
	ui = ui_create(&c);
	ui_set_size(ui, 640, 480);
	return ui;
}

static void frame(struct ui *ui, int64_t *now)
{
	struct gfx_surface s;

	*now += 16;
	gfx_surface_init(&s, g_fb, 640, 480, 640);
	if (ui_update(ui, *now))
		ui_render(ui, &s);
}

static int64_t us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Boots until the carousel shows, then until every list is in. */
static void boot(struct boot *b, const char *delay_ms, struct ui **keep)
{
	int64_t now = 1000, t0 = us();
	struct ui *ui = start(delay_ms);

	while (!ui_is_loaded(ui) && us() - t0 < 20000000) {
		frame(ui, &now);
		usleep(500);
	}
	frame(ui, &now);             /* the first carousel frame */
	b->menu_us = us() - t0;
	ui_debug_state(ui, b->at_menu, sizeof(b->at_menu));
	if (keep) {
		*keep = ui;
		return;
	}
	while (!ui_lists_complete(ui) && us() - t0 < 20000000) {
		frame(ui, &now);
		usleep(500);
	}
	frame(ui, &now);
	b->all_us = us() - t0;
	ui_debug_state(ui, b->final, sizeof(b->final));
	ui_destroy(ui);
}

/* "key=value" from a ui_debug_state() string. */
static const char *field(const char *st, const char *key, char *out, size_t n)
{
	char k[32];
	const char *p;
	size_t l;

	snprintf(k, sizeof(k), "%s=", key);
	p = strstr(st, k);
	if (!p) {
		out[0] = 0;
		return out;
	}
	p += strlen(k);
	l = strcspn(p, ";");
	if (l >= n)
		l = n - 1;
	memcpy(out, p, l);
	out[l] = 0;
	return out;
}

static bool same_field(const char *a, const char *b, const char *key)
{
	char x[1024], y[1024];

	return !strcmp(field(a, key, x, sizeof(x)), field(b, key, y, sizeof(y)));
}

int main(int argc, char **argv)
{
	struct boot a, b, c, d;
	char f1[1024], f2[1024], snap[700];
	struct stat st;

	if (argc < 4) {
		fprintf(stderr, "usage: test_loader WORKDIR RES_DIR THEMES_DIR\n");
		return 2;
	}
	snprintf(W, sizeof(W), "%s", argv[1]);
	g_res = argv[2];
	g_themes = argv[3];
	mkdir(W, 0755);
	make_tree();
	snprintf(snap, sizeof(snap), "%s/data/rsos/cache/systems.idx", W);

	printf("1. first boot, no snapshot\n");
	boot(&a, NULL, NULL);
	printf("  menu %lld ms: %s\n", (long long)a.menu_us / 1000, a.at_menu);
	CHECK(strstr(a.at_menu, "complete=1"), "the first-boot carousel appears with every list in");
	CHECK(strstr(a.final, "carousel=nes:2,snes:4,gb:5,gbc:1,megadrive:3,favorites:1,lastplayed:1"),
	      "carousel %s", field(a.final, "carousel", f1, sizeof(f1)));
	CHECK(strstr(a.final, "cursor=megadrive"), "cursor at the saved system");
	CHECK(stat(snap, &st) == 0 && st.st_size > 0, "systems.idx written");

	printf("2. normal boot from the snapshot (loader slowed down to 20 ms per system)\n");
	boot(&b, "20", NULL);
	printf("  menu %lld ms, every list %lld ms\n", (long long)b.menu_us / 1000, (long long)b.all_us / 1000);
	CHECK(strstr(b.at_menu, "complete=0"), "the carousel is up before the lists (%s)",
	      field(b.at_menu, "complete", f1, sizeof(f1)));
	CHECK(same_field(b.at_menu, a.final, "carousel"), "same carousel as the full load, from the snapshot");
	CHECK(strstr(b.at_menu, "cursor=megadrive"), "at the saved position");
	CHECK(b.menu_us < b.all_us / 2, "menu (%lld ms) well before the lists (%lld ms)",
	      (long long)b.menu_us / 1000, (long long)b.all_us / 1000);
	CHECK(same_field(b.final, a.final, "digest"), "final state = full load (digest %s)",
	      field(b.final, "digest", f1, sizeof(f1)));

	printf("3. the library changed while off\n");
	{
		char p[700];

		put("data/roms/gba/Motocross.gba", "x");
		put("data/roms/gba/Anguna.gba", "x");
		snprintf(p, sizeof(p), "%s/data/roms/gbc/Tuff.gbc", W);
		unlink(p);
		put("data/roms/nes/gamelist.xml", "<gameList><game><path>./Alter Ego.nes</path><name>Alter Ego DX"
		    "</name><desc>A test.</desc></game><game><path>./Blade Buster.nes</path><name>Blade Buster"
		    "</name></game></gameList>\n");
	}
	boot(&c, "20", NULL);
	CHECK(same_field(c.at_menu, a.final, "carousel"), "stale carousel first: %s",
	      field(c.at_menu, "carousel", f1, sizeof(f1)));
	CHECK(strstr(c.final, "carousel=nes:2,snes:4,gb:5,gba:2,megadrive:3,favorites:1,lastplayed:1"),
	      "then updated: %s", field(c.final, "carousel", f1, sizeof(f1)));
	CHECK(strstr(c.final, "cursor=megadrive"), "the selected system kept (%s)",
	      field(c.final, "cursor", f1, sizeof(f1)));

	printf("4. the same library, full load without snapshot\n");
	unlink(snap);
	boot(&d, NULL, NULL);
	CHECK(same_field(c.final, d.final, "carousel") && same_field(c.final, d.final, "digest"),
	      "validated snapshot boot = full load (%s / %s)", field(c.final, "digest", f1, sizeof(f1)),
	      field(d.final, "digest", f2, sizeof(f2)));

	printf("5. opening lists before they arrive (loader slowed down to 300 ms per system)\n");
	{
		struct boot e;
		struct ui *ui;
		int64_t now = 100000, t0;
		char st2[2048];

		boot(&e, "300", &ui);
		CHECK(strstr(e.at_menu, "complete=0"), "carousel up, lists loading");
		/* megadrive is selected: its list is the worker's first job */
		t0 = us();
		ui_button(ui, IN_A, IN_NAV_PRESS);
		ui_button(ui, IN_A, IN_NAV_RELEASE);
		frame(ui, &now);
		ui_debug_state(ui, st2, sizeof(st2));
		CHECK(strstr(st2, "top=list:megadrive:3"), "megadrive opened with its 3 games in %lld ms (%s)",
		      (long long)(us() - t0) / 1000, field(st2, "top", f1, sizeof(f1)));
		ui_button(ui, IN_B, IN_NAV_PRESS);
		ui_button(ui, IN_B, IN_NAV_RELEASE);
		frame(ui, &now);
		/* three to the left: snes, far in the worker's order (selected
		 * system, neighbours, then table order), so still to do: loaded
		 * here, on the spot */
		for (int k = 0; k < 3; k++) {
			ui_button(ui, IN_LEFT, IN_NAV_PRESS);
			ui_button(ui, IN_LEFT, IN_NAV_RELEASE);
			now += 400;
			frame(ui, &now);
		}
		t0 = us();
		ui_button(ui, IN_A, IN_NAV_PRESS);
		ui_button(ui, IN_A, IN_NAV_RELEASE);
		frame(ui, &now);
		ui_debug_state(ui, st2, sizeof(st2));
		CHECK(strstr(st2, "top=list:snes:4") && us() - t0 < 150000,
		      "snes opened with its 4 games in %lld ms, loaded on the spot (%s)",
		      (long long)(us() - t0) / 1000, field(st2, "top", f1, sizeof(f1)));
		ui_button(ui, IN_B, IN_NAV_PRESS);
		ui_button(ui, IN_B, IN_NAV_RELEASE);
		frame(ui, &now);
		/* favorites: every list first */
		for (int k = 0; k < 4; k++) {
			ui_button(ui, IN_RIGHT, IN_NAV_PRESS);
			ui_button(ui, IN_RIGHT, IN_NAV_RELEASE);
			now += 400;
			frame(ui, &now);
		}
		ui_debug_state(ui, st2, sizeof(st2));
		CHECK(strstr(st2, "cursor=favorites"), "on the favorites entry (%s)",
		      field(st2, "cursor", f1, sizeof(f1)));
		t0 = us();
		ui_button(ui, IN_A, IN_NAV_PRESS);
		ui_button(ui, IN_A, IN_NAV_RELEASE);
		frame(ui, &now);
		ui_debug_state(ui, st2, sizeof(st2));
		CHECK(strstr(st2, "top=list:favorites:1") && strstr(st2, "complete=1") && us() - t0 < 1000000,
		      "favorites opened with every list in, %lld ms: the rest loaded here, at most one "
		      "wait for the worker (%s)", (long long)(us() - t0) / 1000, field(st2, "top", f1, sizeof(f1)));
		CHECK(same_field(st2, d.final, "digest"), "and the state is the full load's");
		ui_destroy(ui);
	}

	printf("6. image cache files: run-length coded when it pays, raw otherwise\n");
	{
		struct gfx_image *img = gfx_image_new(640, 480), *back;
		char p[700];
		struct stat s1;
		uint32_t seed = 1;
		bool same = true;

		/* a backdrop-like image: gradient rows, a band, a few noisy pixels */
		for (int y = 0; y < 480; y++)
			for (int x = 0; x < 640; x++) {
				uint32_t v = 0xff000000u | (uint32_t)(y / 2) << 8;

				if (x > y && x < y + 40)
					v += 0x00100000u;
				if ((x * 7 + y * 13) % 997 == 0)
					v ^= (seed = seed * 1103515245u + 12345u) & 0xffffffu;
				img->px[y * 640 + x] = v;
			}
		img->flags |= GFX_IMG_OPAQUE;
		snprintf(p, sizeof(p), "%s/bd-test.rpx", W);
		CHECK(rpx_save(p, img, 42) == 0 && stat(p, &s1) == 0 && s1.st_size < 640 * 480 * 4 / 5,
		      "backdrop-like image: %lld bytes instead of %d", (long long)s1.st_size, 640 * 480 * 4 + 32);
		back = rpx_load(p, 42);
		for (int i = 0; back && i < 640 * 480; i++)
			same &= back->px[i] == img->px[i];
		CHECK(back && same && (back->flags & GFX_IMG_OPAQUE), "decodes to the same pixels");
		gfx_image_free(back);
		CHECK(!rpx_load(p, 43), "wrong key: rejected");
		/* noise: stays raw (mmap()ed) */
		for (int i = 0; i < 640 * 480; i++)
			img->px[i] = (seed = seed * 1103515245u + 12345u);
		rpx_save(p, img, 7);
		stat(p, &s1);
		back = rpx_load(p, 7);
		same = back != NULL;
		for (int i = 0; back && i < 640 * 480; i++)
			same &= back->px[i] == img->px[i];
		CHECK(s1.st_size == 640 * 480 * 4 + 32 && same && (back->flags & GFX_IMG_MMAPPED),
		      "noise: raw, %lld bytes, mmap()ed", (long long)s1.st_size);
		gfx_image_free(back);
		/* a truncated coded file is a cache miss, never garbage */
		for (int i = 0; i < 640 * 480; i++)
			img->px[i] = 0xff000000u | (uint32_t)(i / 6400);
		rpx_save(p, img, 9);
		stat(p, &s1);
		if (truncate(p, s1.st_size - 8) == 0)
			CHECK(!rpx_load(p, 9), "truncated coded file: rejected");
		gfx_image_free(img);
	}

	/* Review F-H5: every picture shown used to stay decoded for the
	 * whole session. Unreferenced images now go least recently used
	 * first past the budget; images in use are never evicted. */
	printf("7. image memory cache: bounded, LRU, images in use kept\n");
	{
		char p[700], svg[256];
		struct gfx_image *held, *again;
		int before, max_seen = 0;
		uint32_t px0;

		img_trim();
		before = img_cache_entries();
		img_set_cache_dir(NULL);              /* decode every time: heap entries */
		dir("imgs");
		for (int i = 0; i < 300; i++) {
			snprintf(p, sizeof(p), "imgs/i%03d.svg", i);
			snprintf(svg, sizeof(svg), "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
				 "<rect width='64' height='64' fill='#%06x'/></svg>", i * 811 & 0xffffff);
			put(p, svg);
		}
		img_set_cache_budget((size_t)64 * 64 * 4 * 20, 20);  /* 20 idle pictures */
		snprintf(p, sizeof(p), "%s/imgs/i000.svg", W);
		held = img_get(p, 64, 64, 0xffffffffu);
		px0 = held ? held->px[32 * 64 + 32] : 0;
		for (int i = 1; i < 300; i++) {
			struct gfx_image *g;

			snprintf(p, sizeof(p), "%s/imgs/i%03d.svg", W, i);
			g = img_get(p, 64, 64, 0xffffffffu);
			img_put(g);
			if (img_cache_entries() - before > max_seen)
				max_seen = img_cache_entries() - before;
		}
		CHECK(held && max_seen <= 22, "300 pictures browsed: at most %d cached (20 idle + 1 in use)",
		      max_seen);
		CHECK(held && held->px[32 * 64 + 32] == px0, "the picture in use survived (no eviction while referenced)");
		/* the most recent idle one is still a memory hit */
		{
			struct img_stats a, b;

			img_get_stats(&a);
			snprintf(p, sizeof(p), "%s/imgs/i299.svg", W);
			again = img_get(p, 64, 64, 0xffffffffu);
			img_get_stats(&b);
			CHECK(again && b.mem_hits == a.mem_hits + 1 && b.decoded == a.decoded,
			      "the last picture shown is still cached (LRU)");
			img_put(again);
			snprintf(p, sizeof(p), "%s/imgs/i001.svg", W);
			img_get_stats(&a);
			again = img_get(p, 64, 64, 0xffffffffu);
			img_get_stats(&b);
			CHECK(again && b.decoded == a.decoded + 1, "an old one was evicted (decoded again)");
			img_put(again);
		}
		img_put(held);
		img_trim();
		img_set_cache_budget((size_t)24 << 20, 256);
	}

	/* Review F-H2: the system list and the Favorites / Last played copies
	 * share the per-game core string; changing the core freed it. */
	printf("8. per-game core: changing it never frees what another copy of the game uses\n");
	{
		char romdir[700], cores[700], db_path[700];
		struct gamelist *a, *b;
		struct gamedb *db;
		struct game *ga = NULL, *gb = NULL;

		snprintf(cores, sizeof(cores), "%s/cores", W);
		snprintf(romdir, sizeof(romdir), "%s/data/roms/nes", W);
		snprintf(db_path, sizeof(db_path), "%s/gamedb-core.tsv", W);
		put("gamedb-core.tsv", "nes\tAlter Ego.nes\t1\t0\t0\tc_old\n");
		systems_load_cores(cores);
		a = games_load("nes", romdir, NULL, NULL, NULL);
		b = games_load("nes", romdir, NULL, NULL, NULL);      /* e.g. the Favorites copy */
		db = gamedb_open(db_path);
		gamedb_apply(db, a);
		gamedb_apply(db, b);
		for (int i = 0; a && i < a->n; i++)
			if (!strcmp(a->games[i].rel, "Alter Ego.nes"))
				ga = &a->games[i];
		for (int i = 0; b && i < b->n; i++)
			if (!strcmp(b->games[i].rel, "Alter Ego.nes"))
				gb = &b->games[i];
		CHECK(ga && gb && ga->core && gb->core && !strcmp(gb->core, "c_old"),
		      "both copies use the core from gamedb");
		if (ga && gb && gb->core) {
			gamedb_set_core(db, ga, "c_new");
			CHECK(!strcmp(ga->core, "c_new") && !strcmp(gb->core, "c_old"),
			      "set on one copy: the other one's string is still valid (ASan: no use after free)");
			gamedb_set_core(db, ga, NULL);
			CHECK(!ga->core && !strcmp(gb->core, "c_old"), "cleared: the same");
			gamedb_set_core(db, ga, "c_old");
			CHECK(ga->core == gb->core, "the same core id is one interned string");
		}
		CHECK(gamedb_save(db) == 0, "saved");
		gamedb_close(db);
		games_free(a);
		games_free(b);
		systems_free();
	}

	printf("9. legacy ROM folders beside empty image-created folders\n");
	{
		struct boot e, f;
		char cache[700];
		struct stat before, after;

		/* The previous snapshot remembers mastersystem/ as empty. Adding
		 * sms/ changes neither that folder nor its mtime. */
		dir("data/roms/sms");
		dir("data/roms/sms/subfolder");
		put("data/roms/sms/subfolder/Legacy.sms", "x");
		boot(&e, NULL, NULL);
		CHECK(strstr(e.final, "mastersystem:1"), "a new alias is found despite the empty-folder snapshot");
		snprintf(cache, sizeof(cache), "%s/data/rsos/cache/scan/mastersystem.bin", W);
		CHECK(stat(cache, &before) == 0, "the populated alias has a scan cache");
		boot(&f, NULL, NULL);
		CHECK(strstr(f.final, "mastersystem:1") && same_field(e.final, f.final, "digest"),
		      "the alias survives the next cached boot");
		CHECK(stat(cache, &after) == 0 && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
		      before.st_mtim.tv_nsec == after.st_mtim.tv_nsec,
		      "the empty primary does not overwrite the alias cache and force another scan");
		unlink(snap);
		boot(&f, NULL, NULL);
		CHECK(strstr(f.final, "mastersystem:1") && same_field(e.final, f.final, "digest"),
		      "the alias is also found without a snapshot");
		put("data/roms/mastersystem/Primary.sms", "x");
		put("data/roms/mastersystem/Primary2.sms", "x");
		boot(&f, NULL, NULL);
		CHECK(strstr(f.final, "mastersystem:2"), "a populated primary folder keeps precedence");
	}

	printf("10. games copied from a PC: Windows leaves an exFAT folder's mtime unchanged\n");
	{
		struct boot e, f;
		char c1[512];

		/* gamegear/ is empty (remembered so by the snapshot), gba/ has two
		 * games (their list in the scan cache): a file copied into each, then the
		 * folder's old mtime put back, as Windows does on the card */
		char *idx;

		boot(&e, NULL, NULL);
		CHECK(!strstr(e.final, "gamegear:") && strstr(e.final, "gba:2"), "gamegear empty, gba with two games: %s",
		      field(e.final, "carousel", c1, sizeof(c1)));
		idx = file_read(snap, NULL);
		CHECK(idx && strstr(idx, "\nempty gamegear "), "the snapshot remembers gamegear as an empty folder");
		free(idx);
		copied_from_pc("data/roms/gamegear", "Copied.gg");
		copied_from_pc("data/roms/gba", "Copied.gba");
		boot(&f, NULL, NULL);
		CHECK(strstr(f.final, "gamegear:1"), "a game copied into an empty folder is found (its mtime did not change)");
		CHECK(strstr(f.final, "gba:3"), "a game copied into a folder with a cached list is found");
		boot(&e, NULL, NULL);
		CHECK(strstr(e.final, "gamegear:1") && strstr(e.final, "gba:3") && same_field(e.final, f.final, "digest"),
		      "the next boot keeps them (from the caches)");
		copied_from_pc("data/roms/gba", NULL);
		boot(&f, NULL, NULL);
		CHECK(strstr(f.final, "gba:2"), "a game deleted on a PC goes away too");
	}

	printf("11. Settings > Game lists > Refresh game lists trusts no cache\n");
	{
		struct boot e;
		struct ui *ui;
		int64_t now = 100000, t0 = us();
		char p[700], line[1400], st2[2048];
		struct stat st;
		FILE *f;

		/* ngp/ (no folder alias, so the empty-folder shortcut
		 * applies) gets a game; then the snapshot is made to remember it
		 * as empty with its current stamp, like a stale cache would */
		put("data/roms/ngp/Pocket Tennis.ngp", "x");
		boot(&e, NULL, NULL);
		CHECK(strstr(e.final, "ngp:1"), "a Neo Geo Pocket game is listed");
		snprintf(p, sizeof(p), "%s/data/roms/ngp", W);
		f = fopen(snap, "a");
		if (stat(p, &st) == 0 && f) {
			snprintf(line, sizeof(line), "empty ngp %lld %s\n",
				 (long long)games_dir_stamp(p, &st), p);
			fputs(line, f);
		}
		if (f)
			fclose(f);
		boot(&e, NULL, &ui);
		while (!ui_lists_complete(ui) && us() - t0 < 20000000) {
			frame(ui, &now);
			usleep(500);
		}
		frame(ui, &now);
		ui_debug_state(ui, st2, sizeof(st2));
		CHECK(!strstr(st2, "ngp:"), "the stale snapshot hides it (%s)",
		      field(st2, "carousel", f1, sizeof(f1)));
		ui_reload_games(ui, true);
		frame(ui, &now);
		ui_debug_state(ui, st2, sizeof(st2));
		CHECK(strstr(st2, "ngp:1") && strstr(st2, "complete=1"),
		      "Refresh game lists finds it again (%s)", field(st2, "carousel", f1, sizeof(f1)));
		ui_destroy(ui);
	}

	printf("12. Doom, Cave Story and the native games are one Ports list\n");
	{
		struct boot e;
		char c1[512];

		dir("data/roms/native");
		dir("data/roms/ports");
		put("data/roms/doom/E1M1.wad", "x");
		put("data/roms/cavestory/Doukutsu.exe", "x");
		put("data/roms/native/Test Game.sh", "x");
		put("data/roms/ports/Other.wad", "x");
		boot(&e, NULL, NULL);
		CHECK(strstr(e.final, "ports:4"), "one Ports entry with the four games: %s",
		      field(e.final, "carousel", c1, sizeof(c1)));
		CHECK(!strstr(e.final, "doom:") && !strstr(e.final, "cavestory:") && !strstr(e.final, "native:"),
		      "the three are not shown on their own");
		CHECK(!strcmp(systems_effective("ports", "/r/native/Test Game.sh"), "native"), "a .sh launches as a native game");
		CHECK(!strcmp(systems_effective("ports", "/r/ports/Doukutsu.EXE"), "cavestory"), "an .exe as Cave Story");
		CHECK(!strcmp(systems_effective("ports", "/r/doom/E1M1.wad"), "doom"), "a .wad as Doom");
		CHECK(!strcmp(systems_effective("snes", "/r/snes/x.sfc"), "snes"), "any other system is unchanged");
	}

	printf(g_fail ? "test_loader: %d FAILED\n" : "test_loader: all passed\n", g_fail);
	return g_fail ? 1 : 0;
}
