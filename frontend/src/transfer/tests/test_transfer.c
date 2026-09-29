/*
 * test_transfer.c - unit tests for frontend/src/transfer/ (host, no root
 * needed): name/path checks, URL decoding, the directory walker, the file
 * sink, filesystem probing, the import plan and copy on temp dirs, USB
 * detection on a fake /sys/block, the name responder packets and the QR
 * encoder.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "transfer.h"
#include "tr_internal.h"

void qr_rs_remainder(const uint8_t *data, int len, int degree, uint8_t *out);
int qr_format_bits(int mask);

static int g_fail, g_pass;

#define CHECK(c)                                                                  \
	do {                                                                      \
		if (c) {                                                          \
			g_pass++;                                                 \
		} else {                                                          \
			g_fail++;                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		}                                                                 \
	} while (0)

static char g_tmp[256];

/* ------------------------------------------------------------ fs helpers */

static void pathf(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pathf(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(out, n, fmt, ap);
	va_end(ap);
}

static void mkdirs(const char *p)
{
	char t[1024];

	snprintf(t, sizeof(t), "%s", p);
	for (char *c = t + 1; *c; c++)
		if (*c == '/') {
			*c = 0;
			mkdir(t, 0755);
			*c = '/';
		}
	mkdir(t, 0755);
}

/* Writes size bytes of a pattern seeded by seed; mtime if >= 0. */
static void put_file(const char *root, const char *rel, size_t size, int seed, time_t mtime)
{
	char p[1024], d[1024];
	FILE *f;

	pathf(p, sizeof(p), "%s/%s", root, rel);
	snprintf(d, sizeof(d), "%s", p);
	*strrchr(d, '/') = 0;
	mkdirs(d);
	f = fopen(p, "wb");
	for (size_t i = 0; i < size; i++)
		fputc((int)((i * 31 + (size_t)seed * 7) & 0xff), f);
	fclose(f);
	if (mtime >= 0) {
		struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } };

		utimensat(AT_FDCWD, p, ts, 0);
	}
}

static bool same_files(const char *a, const char *b)
{
	int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY), r;

	if (fa < 0 || fb < 0) {
		if (fa >= 0)
			close(fa);
		if (fb >= 0)
			close(fb);
		return false;
	}
	r = tr_same_content(fa, fb);
	close(fa);
	close(fb);
	return r == 1;
}

static bool exists(const char *root, const char *rel)
{
	char p[1024];
	struct stat st;

	pathf(p, sizeof(p), "%s/%s", root, rel);
	return stat(p, &st) == 0;
}

static int count_parts(const char *dir)
{
	/* ".rsos-part" leftovers anywhere below dir */
	char cmd[1200];
	FILE *f;
	int n = 0;

	pathf(cmd, sizeof(cmd), "find '%s' -name '*rsos-part*' | wc -l", dir);
	f = popen(cmd, "r");
	if (f) {
		if (fscanf(f, "%d", &n) != 1)
			n = -1;
		pclose(f);
	}
	return n;
}

/* ------------------------------------------------------------ names */

static void test_names(void)
{
	char out[512];
	char longname[300];

	CHECK(transfer_name_check("Super Mario World (USA).sfc") == 0);
	CHECK(transfer_name_check("Pok\xc3\xa9mon - Version Or (France).gbc") == 0);
	CHECK(transfer_name_check("\xe3\x83\x9d\xe3\x82\xb1\xe3\x83\xa2\xe3\x83\xb3.gb") == 0);
	CHECK(transfer_name_check("Rock & Roll, 'Racing' #1.md") == 0);
	CHECK(transfer_name_check("a b") == 0);
	CHECK(transfer_name_check("") < 0);
	CHECK(transfer_name_check(".") < 0);
	CHECK(transfer_name_check("..") < 0);
	CHECK(transfer_name_check(".hidden") < 0);
	CHECK(transfer_name_check(".x.rsos-part") < 0);
	CHECK(transfer_name_check("a/b") < 0);
	CHECK(transfer_name_check("a\\b") < 0);
	CHECK(transfer_name_check("a:b") < 0);
	CHECK(transfer_name_check("a*b") < 0);
	CHECK(transfer_name_check("a?b") < 0);
	CHECK(transfer_name_check("a\"b") < 0);
	CHECK(transfer_name_check("a<b>") < 0);
	CHECK(transfer_name_check("a|b") < 0);
	CHECK(transfer_name_check("tab\there") < 0);
	CHECK(transfer_name_check("del\x7f") < 0);
	CHECK(transfer_name_check("trailing ") < 0);
	CHECK(transfer_name_check("trailing.") < 0);
	CHECK(transfer_name_check("CON") < 0);
	CHECK(transfer_name_check("con.txt") < 0);
	CHECK(transfer_name_check("Nul .srm") < 0);
	CHECK(transfer_name_check("COM1.sav") < 0);
	CHECK(transfer_name_check("lpt9") < 0);
	CHECK(transfer_name_check("console.nes") == 0);
	CHECK(transfer_name_check("COM10") == 0);
	CHECK(transfer_name_check("\xff\xfe") < 0);            /* invalid UTF-8 */
	CHECK(transfer_name_check("\xc0\xaf") < 0);            /* overlong '/' */
	CHECK(transfer_name_check("\xed\xa0\x80") < 0);        /* surrogate */
	CHECK(transfer_name_check("\xe3\x83") < 0);            /* truncated */
	memset(longname, 'a', 255);
	longname[255] = 0;
	CHECK(transfer_name_check(longname) == 0);
	longname[255] = 'a';
	longname[256] = 0;
	CHECK(transfer_name_check(longname) == -ENAMETOOLONG);

	CHECK(transfer_relpath_check("a.sfc") == 0);
	CHECK(transfer_relpath_check("Final Fantasy VII/FF7 (Disc 1).cue") == 0);
	CHECK(transfer_relpath_check("/etc/passwd") < 0);
	CHECK(transfer_relpath_check("../x") < 0);
	CHECK(transfer_relpath_check("a/../../x") < 0);
	CHECK(transfer_relpath_check("a/./b") < 0);
	CHECK(transfer_relpath_check("a//b") < 0);
	CHECK(transfer_relpath_check("a/") < 0);
	CHECK(transfer_relpath_check("") < 0);
	CHECK(transfer_relpath_check("a\\..\\b") < 0);
	CHECK(transfer_relpath_check("1/2/3/4/5/6/7/8") == 0);
	CHECK(transfer_relpath_check("1/2/3/4/5/6/7/8/9") < 0);

	CHECK(transfer_name_sanitize("a:b?.sfc", out, sizeof(out)) == 0 && !strcmp(out, "a_b_.sfc"));
	CHECK(transfer_name_sanitize("\xe9t\xe9.nes", out, sizeof(out)) == 0 &&
	      !strcmp(out, "_t_.nes"));
	CHECK(transfer_name_sanitize("name. . ", out, sizeof(out)) == 0 && !strcmp(out, "name"));
	CHECK(transfer_name_sanitize("..", out, sizeof(out)) < 0);
	CHECK(transfer_name_sanitize("CON", out, sizeof(out)) < 0);

	strcpy(out, "a%20b+c%2Fd");
	CHECK(tr_url_decode(out) == 0 && !strcmp(out, "a b c/d"));
	strcpy(out, "%2e%2e%2fetc");
	CHECK(tr_url_decode(out) == 0 && transfer_relpath_check(out) < 0);
	strcpy(out, "a%00b");
	CHECK(tr_url_decode(out) < 0);
	strcpy(out, "a%zzb");
	CHECK(tr_url_decode(out) < 0);
	strcpy(out, "a%2");
	CHECK(tr_url_decode(out) < 0);

	CHECK(tr_is_save_name("Zelda.srm") && tr_is_save_name("x.SAV") && !tr_is_save_name("x.sfc"));
	CHECK(tr_is_state_name("Zelda.state") && tr_is_state_name("Zelda.state3") &&
	      tr_is_state_name("Zelda.state.auto") && tr_is_state_name("Zelda.state.auto.png") &&
	      tr_is_state_name("Zelda.state12.png") && !tr_is_state_name("statefile.sfc") &&
	      !tr_is_state_name("Zelda.states") && !tr_is_state_name(".state"));
	CHECK(tr_is_junk("._Zelda.sfc") && tr_is_junk(".DS_Store") && tr_is_junk("Thumbs.db") &&
	      tr_is_junk("System Volume Information") && tr_is_junk("$RECYCLE.BIN") &&
	      !tr_is_junk("Zelda.sfc"));
}

static void test_sysmap(void)
{
	struct { const char *in, *out; } t[] = {
		{ "snes", "snes" }, { "SNES", "snes" }, { "genesis", "megadrive" },
		{ "Mega Drive", "megadrive" }, { "sg-1000", "sg1000" }, { "fba", "fbneo" },
		{ "mame-libretro", "arcade" }, { "mame2003-plus", "arcade" }, { "psx", "psx" },
		{ "PS", "psx" }, { "Game Boy (GB)", "gb" }, { "Game Boy Color (GBC)", "gbc" },
		{ "Super Nintendo Entertainment System (SFC)", "snes" }, { "FC", "nes" },
		{ "Sega 32X (32X)", "sega32x" }, { "THIRTYTWOX", "sega32x" }, { "gamegear", "gamegear" },
		{ "tg16", "pcengine" }, { "neogeo", "neogeo" }, { "n64", "n64" },
		{ "dreamcast", NULL }, { "ports", NULL }, { "", NULL }, { "Foo (BAR)", NULL },
		/* third batch of cores: the new folders and their aliases */
		{ "pc", "dos" }, { "MS-DOS", "dos" }, { "dos", "dos" }, { "spectrum", "zxspectrum" },
		{ "cpc", "amstradcpc" }, { "Commodore 64", "c64" }, { "pico-8", "pico8" },
		{ "pico", "pico" }, { "pokemonmini", "pokemini" }, { "prboom", "doom" },
		{ "SuperGrafx (SGFX)", "supergrafx" }, { "scummvm", "scummvm" }, { "nxengine", "cavestory" },
	};

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		const char *r = transfer_system_canon(t[i].in);
		bool ok = t[i].out ? (r && !strcmp(r, t[i].out)) : r == NULL;

		if (!ok)
			fprintf(stderr, "  canon(\"%s\") = %s, want %s\n", t[i].in, r ? r : "NULL",
				t[i].out ? t[i].out : "NULL");
		CHECK(ok);
	}
	CHECK(transfer_system_index("megadrive") >= 0 && transfer_system_index("genesis") < 0);
	for (int i = 0; i < transfer_system_count(); i++)
		CHECK(strlen(transfer_system_id(i)) < TRANSFER_SYSID_MAX);
}

/* ------------------------------------------------------------ dirs, sink */

static void test_dirs_and_sink(void)
{
	char root[512], p[1024], buf[64];
	int rfd, dfd, fd;
	struct tr_sink s;
	struct stat st;

	pathf(root, sizeof(root), "%s/sink", g_tmp);
	mkdirs(root);
	rfd = open(root, O_RDONLY | O_DIRECTORY);

	dfd = tr_open_dir_chain(rfd, "roms/snes/sub", true);
	CHECK(dfd >= 0);
	pathf(p, sizeof(p), "%s/roms/snes/sub", root);
	CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode));

	/* a symlink inside the tree must not be followed */
	pathf(p, sizeof(p), "%s/roms/evil", root);
	CHECK(symlink("/tmp", p) == 0);
	CHECK(tr_open_dir_chain(rfd, "roms/evil", false) < 0);
	CHECK(tr_open_dir_chain(rfd, "roms/evil/x", true) < 0);
	CHECK(tr_open_dir_chain(rfd, "roms/../..", false) < 0);
	CHECK(tr_open_dir_chain(rfd, "roms/missing", false) == -ENOENT);

	CHECK(tr_sink_open(&s, dfd, "Game.sfc") == 0);
	CHECK(tr_sink_write(&s, "hello", 5) == 0);
	/* ".Game.sfc.<8 hex>.rsos-part" */
	CHECK(!strncmp(s.tmp, ".Game.sfc.", 10) && strlen(s.tmp) == 10 + 8 + 10 &&
	      !strcmp(s.tmp + 18, ".rsos-part"));
	strcpy(p, s.tmp);
	CHECK(faccessat(dfd, p, F_OK, 0) == 0);
	CHECK(faccessat(dfd, "Game.sfc", F_OK, 0) != 0);
	CHECK(tr_sink_commit(&s, 1000000000, 0, false) == 0);
	CHECK(fstatat(dfd, "Game.sfc", &st, 0) == 0 && st.st_size == 5 &&
	      st.st_mtim.tv_sec == 1000000000);
	CHECK(faccessat(dfd, p, F_OK, 0) != 0);

	/* replace with a backup */
	CHECK(tr_sink_open(&s, dfd, "Game.sfc") == 0);
	CHECK(tr_sink_write(&s, "world!", 6) == 0);
	CHECK(tr_sink_commit(&s, -1, 0, true) == 0);
	fd = openat(dfd, "Game.sfc.bak", O_RDONLY);
	CHECK(fd >= 0 && read(fd, buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5));
	if (fd >= 0)
		close(fd);
	CHECK(fstatat(dfd, "Game.sfc", &st, 0) == 0 && st.st_size == 6);

	/* abort removes the temp file and leaves the original */
	CHECK(tr_sink_open(&s, dfd, "Game.sfc") == 0);
	tr_sink_write(&s, "xx", 2);
	strcpy(p, s.tmp);
	tr_sink_abort(&s);
	CHECK(faccessat(dfd, p, F_OK, 0) != 0);
	CHECK(fstatat(dfd, "Game.sfc", &st, 0) == 0 && st.st_size == 6);

	/*
	 * Review F-H1: two writers of the same name (two uploads of one path,
	 * or the USB import and the web share) shared one temp file opened with
	 * O_TRUNC: the committed file was a mix of both. Each writer now has its
	 * own O_EXCL temp file, and the result is one of the two, whole.
	 */
	{
		struct tr_sink a, b;
		char ta[300], tb[300], big[4096];
		int bytes_a = 0, bytes_b = 0;

		CHECK(tr_sink_open(&a, dfd, "Race.sfc") == 0 && tr_sink_open(&b, dfd, "Race.sfc") == 0);
		CHECK(strcmp(a.tmp, b.tmp) != 0);
		strcpy(ta, a.tmp);
		strcpy(tb, b.tmp);
		for (int i = 0; i < 64; i++) {       /* interleaved writes */
			memset(big, 'A', sizeof(big));
			CHECK(tr_sink_write(&a, big, sizeof(big)) == 0);
			memset(big, 'B', sizeof(big));
			CHECK(tr_sink_write(&b, big, i < 32 ? sizeof(big) : 1) == 0);
		}
		CHECK(tr_sink_commit(&a, -1, 0, false) == 0);
		CHECK(tr_sink_commit(&b, -1, 0, false) == 0);
		fd = openat(dfd, "Race.sfc", O_RDONLY);
		while (fd >= 0) {
			ssize_t n = read(fd, big, sizeof(big));

			if (n <= 0)
				break;
			for (ssize_t i = 0; i < n; i++) {
				bytes_a += big[i] == 'A';
				bytes_b += big[i] == 'B';
			}
		}
		if (fd >= 0)
			close(fd);
		CHECK(bytes_a == 0 && bytes_b == 32 * 4096 + 32);   /* b, whole (committed last) */
		CHECK(faccessat(dfd, ta, F_OK, 0) != 0 && faccessat(dfd, tb, F_OK, 0) != 0);

		/* no_replace (web upload with overwrite=0): the second one loses
		 * atomically, the first one's file is untouched */
		CHECK(tr_sink_open(&a, dfd, "Once.sfc") == 0 && tr_sink_open(&b, dfd, "Once.sfc") == 0);
		a.no_replace = b.no_replace = true;
		CHECK(tr_sink_write(&a, "first", 5) == 0 && tr_sink_write(&b, "second", 6) == 0);
		strcpy(tb, b.tmp);
		CHECK(tr_sink_commit(&a, -1, 0, false) == 0);
		CHECK(tr_sink_commit(&b, -1, 0, false) == -EEXIST);
		CHECK(fstatat(dfd, "Once.sfc", &st, 0) == 0 && st.st_size == 5);
		CHECK(faccessat(dfd, tb, F_OK, 0) != 0);

		/* keep_bak (review F-M8): the backup is made or nothing is replaced */
		CHECK(mkdirat(dfd, "Dir.srm.bak", 0755) == 0);  /* .bak cannot be made */
		fd = openat(dfd, "Dir.srm", O_WRONLY | O_CREAT, 0644);
		if (fd >= 0) {
			CHECK(write(fd, "old", 3) == 3);
			close(fd);
		}
		CHECK(tr_sink_open(&a, dfd, "Dir.srm") == 0 && tr_sink_write(&a, "newer", 5) == 0);
		CHECK(tr_sink_commit(&a, -1, 0, true) < 0);
		CHECK(fstatat(dfd, "Dir.srm", &st, 0) == 0 && st.st_size == 3);   /* old file kept */
		CHECK(count_parts(root) == 0);
	}

	/* very long names use the hashed temp name */
	{
		char name[256];

		memset(name, 'x', 250);
		strcpy(name + 250, ".sfc");
		CHECK(tr_sink_open(&s, dfd, name) == 0);
		CHECK(!strncmp(s.tmp, ".rsos-part-", 11));
		CHECK(tr_sink_commit(&s, -1, 0, false) == 0);
		CHECK(faccessat(dfd, name, F_OK, 0) == 0);
	}
	close(dfd);
	close(rfd);
}

/* ------------------------------------------------------------ fs probe */

static void write_img(const char *path, const unsigned char *data, size_t n, size_t total)
{
	FILE *f = fopen(path, "wb");

	fwrite(data, 1, n, f);
	for (size_t i = n; i < total; i++)
		fputc(0, f);
	fclose(f);
}

static void make_fat32(unsigned char *bs, const char *label)
{
	memset(bs, 0, 512);
	bs[0] = 0xeb;
	bs[1] = 0x58;
	bs[2] = 0x90;
	memcpy(bs + 3, "mkfs.fat", 8);
	bs[11] = 0x00;                 /* 512 bytes/sector */
	bs[12] = 0x02;
	bs[13] = 8;                    /* sectors/cluster */
	bs[14] = 32;                   /* reserved */
	bs[16] = 2;                    /* FATs */
	memcpy(bs + 71, label, 11);
	memcpy(bs + 82, "FAT32   ", 8);
	bs[510] = 0x55;
	bs[511] = 0xaa;
}

static void test_probe(void)
{
	unsigned char img[8192];
	char p[512], label[64];
	const char *t;

	/* FAT32 */
	make_fat32(img, "ROMS       ");
	pathf(p, sizeof(p), "%s/fat32.img", g_tmp);
	write_img(p, img, 512, 4096);
	t = transfer_fs_probe(p, label, sizeof(label));
	CHECK(t && !strcmp(t, "vfat") && !strcmp(label, "ROMS"));

	make_fat32(img, "NO NAME    ");
	write_img(p, img, 512, 4096);
	t = transfer_fs_probe(p, label, sizeof(label));
	CHECK(t && !strcmp(t, "vfat") && label[0] == 0);

	/* FAT16 */
	memset(img, 0, sizeof(img));
	img[0] = 0xeb;
	img[11] = 0x00;
	img[12] = 0x02;
	img[13] = 4;
	img[14] = 1;
	img[16] = 2;
	memcpy(img + 43, "STICK      ", 11);
	memcpy(img + 54, "FAT16   ", 8);
	img[510] = 0x55;
	img[511] = 0xaa;
	write_img(p, img, 512, 4096);
	t = transfer_fs_probe(p, label, sizeof(label));
	CHECK(t && !strcmp(t, "vfat") && !strcmp(label, "STICK"));

	/* an MBR (partition table only) is not a filesystem */
	memset(img, 0, sizeof(img));
	img[446 + 4] = 0x0c;
	img[510] = 0x55;
	img[511] = 0xaa;
	write_img(p, img, 512, 4096);
	CHECK(transfer_fs_probe(p, label, sizeof(label)) == NULL && label[0] == 0);

	/* exFAT with a label in the root directory (cluster 4) */
	memset(img, 0, sizeof(img));
	img[0] = 0xeb;
	img[1] = 0x76;
	img[2] = 0x90;
	memcpy(img + 3, "EXFAT   ", 8);
	img[88] = 4;                   /* cluster heap at sector 4 */
	img[96] = 4;                   /* root dir cluster 4 */
	img[108] = 9;                  /* 512-byte sectors */
	img[109] = 0;                  /* 1 sector per cluster */
	img[510] = 0x55;
	img[511] = 0xaa;
	{
		/* root dir = heap + (4-2) clusters = sector 6 */
		unsigned char *e = img + 6 * 512 + 32;   /* 2nd entry (1st: bitmap) */
		const char *lbl = "RETROSTONE";

		img[6 * 512] = 0x81;                     /* allocation bitmap entry */
		e[0] = 0x83;
		e[1] = (unsigned char)strlen(lbl);
		for (size_t i = 0; lbl[i]; i++)
			e[2 + 2 * i] = (unsigned char)lbl[i];
	}
	pathf(p, sizeof(p), "%s/exfat.img", g_tmp);
	write_img(p, img, sizeof(img), sizeof(img));
	t = transfer_fs_probe(p, label, sizeof(label));
	CHECK(t && !strcmp(t, "exfat") && !strcmp(label, "RETROSTONE"));

	/* NTFS */
	memset(img, 0, sizeof(img));
	memcpy(img + 3, "NTFS    ", 8);
	img[510] = 0x55;
	img[511] = 0xaa;
	write_img(p, img, 512, 4096);
	t = transfer_fs_probe(p, label, sizeof(label));
	CHECK(t && !strcmp(t, "ntfs"));

	/* ext4-ish / random */
	memset(img, 0x5a, sizeof(img));
	write_img(p, img, 4096, 4096);
	CHECK(transfer_fs_probe(p, NULL, 0) == NULL);
	CHECK(transfer_fs_probe("/nonexistent/dev", NULL, 0) == NULL);
}

/* ------------------------------------------------------------ import */

static int g_cb_calls;
static volatile int g_cancel;

static void cancel_cb(const struct transfer_progress *p, void *user)
{
	(void)user;
	g_cb_calls++;
	if (p->bytes_done > 0)
		g_cancel = 1;
}

/* The stick changes under a running import (first progress report). */
static int g_swap_mode;               /* 1 another stick at the same place, 2 pulled */
static const char *g_swap_stick;

static void swap_stick(const struct transfer_progress *pr, void *user)
{
	char old[1100], cmd[1200];

	(void)pr;
	(void)user;
	if (!g_swap_mode)
		return;
	if (g_swap_mode == 1) {
		pathf(old, sizeof(old), "%s.old", g_swap_stick);
		CHECK(rename(g_swap_stick, old) == 0);
		put_file(g_swap_stick, "roms/snes/Good.sfc", 1000, 44, 1600000000);  /* other bytes */
	} else {
		pathf(cmd, sizeof(cmd), "rm -rf '%s'", g_swap_stick);
		CHECK(system(cmd) == 0);
	}
	g_swap_mode = 0;
}

/*
 * Review F-M16: symlinks on the stick were followed out of it (stat() on
 * the roots), so system files could be imported. F-M17: sources were
 * opened by absolute path, so a stick plugged in at the same mount point
 * during an import was read against the old plan, and a pulled stick gave
 * "some files failed" instead of stopping.
 */
static void test_import_stick_safety(void)
{
	char stick[512], data[512], outside[512], a[1100], b[1100], cmd[1200];
	struct transfer_plan *p = NULL;
	struct transfer_progress res;
	int r;

	pathf(stick, sizeof(stick), "%s/stick-s", g_tmp);
	pathf(data, sizeof(data), "%s/data-s", g_tmp);
	pathf(outside, sizeof(outside), "%s/outside", g_tmp);
	mkdirs(data);
	put_file(outside, "secret.bin", 100, 1, 1600000000);
	put_file(outside, "roms/snes/Evil.sfc", 100, 2, 1600000000);
	put_file(stick, "roms/snes/Good.sfc", 1000, 3, 1600000000);
	pathf(a, sizeof(a), "%s/bios", stick);
	CHECK(symlink(outside, a) == 0);                       /* bios -> outside the stick */
	pathf(a, sizeof(a), "%s/themes", stick);
	CHECK(symlink(outside, a) == 0);
	pathf(a, sizeof(a), "%s/RetroPie", stick);
	mkdirs(a);
	pathf(a, sizeof(a), "%s/RetroPie/roms", stick);
	pathf(b, sizeof(b), "%s/roms", outside);
	CHECK(symlink(b, a) == 0);                             /* a nested layout root too */
	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && p);
	if (!p)
		return;
	/* only Good.sfc: nothing from outside (Evil.sfc, secret.bin) */
	CHECK(p->rom_files == 1 && p->bios_files == 0 && p->theme_files == 0 && p->total_bytes == 1000);

	/* another stick at the same place during the import: never read */
	g_swap_mode = 1;
	g_swap_stick = stick;
	r = transfer_import_run(p, NULL, swap_stick, NULL, NULL, &res);
	pathf(a, sizeof(a), "%s.old/roms/snes/Good.sfc", stick);
	pathf(b, sizeof(b), "%s/roms/snes/Good.sfc", data);
	CHECK(r == 0 && res.copied == 1 && same_files(a, b));
	transfer_plan_free(p);
	p = NULL;

	/* the stick pulled during the import: it stops, "removed" */
	remove(b);
	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && p && p->rom_files == 1);
	if (!p)
		return;
	g_swap_mode = 2;
	r = transfer_import_run(p, NULL, swap_stick, NULL, NULL, &res);
	CHECK(r == -ENODEV && res.state == TRANSFER_FAILED && strstr(res.errmsg, "removed"));
	CHECK(!exists(data, "roms/snes/Good.sfc"));
	transfer_plan_free(p);
	pathf(cmd, sizeof(cmd), "rm -rf '%s.old' '%s' '%s' '%s'", stick, stick, data, outside);
	CHECK(system(cmd) == 0);
}

static void test_import(void)
{
	char stick[512], data[512], a[1024], b[1024];
	struct transfer_plan *p = NULL;
	struct transfer_progress res;
	struct transfer_import_opts o;
	int r;

	pathf(stick, sizeof(stick), "%s/stick", g_tmp);
	pathf(data, sizeof(data), "%s/data", g_tmp);
	mkdirs(stick);
	mkdirs(data);

	put_file(stick, "roms/snes/Zelda.sfc", 3000, 1, 1600000000);
	put_file(stick, "roms/snes/Zelda.srm", 800, 2, 1600000000);          /* save, conflict */
	put_file(stick, "roms/snes/Mario.srm", 800, 3, 1600000000);          /* save, new */
	put_file(stick, "roms/snes/Mario.state1", 900, 4, 1600000000);
	put_file(stick, "roms/snes/sub dir/Kirby.sfc", 2000, 5, 1600000000);
	put_file(stick, "roms/snes/._Zelda.sfc", 10, 6, -1);                  /* junk */
	put_file(stick, "roms/.DS_Store", 10, 7, -1);
	put_file(stick, "roms/genesis/Sonic.md", 1500, 8, 1600000000);
	put_file(stick, "roms/megadrive/Sonic.md", 1500, 9, 1600000000);     /* dup dest */
	put_file(stick, "roms/dreamcast/Game.cdi", 100, 10, -1);             /* unknown */
	put_file(stick, "roms/snes/Same.sfc", 1234, 11, 1600000000);         /* identical */
	put_file(stick, "roms/snes/Check.sfc", 1000, 12, 1600000000);        /* CHECK, same */
	put_file(stick, "roms/snes/Check2.sfc", 1000, 13, 1600000000);       /* CHECK, differs */
	put_file(stick, "roms/snes/Bad:Name?.sfc", 100, 14, 1600000000);     /* sanitized */
	put_file(stick, "RetroPie/roms/gba/Metroid.gba", 4000, 15, 1600000000);
	put_file(stick, "RetroPie/BIOS/gba_bios.bin", 1024, 16, 1600000000);
	put_file(stick, "BIOS/scph5501.bin", 2048, 17, 1600000000);
	put_file(stick, "saves/gb/Tetris.srm", 512, 18, 1600000000);
	put_file(stick, "themes/mytheme/theme.xml", 300, 19, 1600000000);
	put_file(stick, "Game Boy Color (GBC)/Wario.gbc", 700, 20, 1600000000);  /* root-level MinUI */
	pathf(a, sizeof(a), "%s/roms/snes/link.sfc", stick);
	CHECK(symlink("/etc/passwd", a) == 0);                                        /* never followed */

	put_file(data, "roms/snes/Same.sfc", 1234, 11, 1600000000);
	put_file(data, "roms/snes/Check.sfc", 1000, 12, 1500000000);
	put_file(data, "roms/snes/Check2.sfc", 1000, 99, 1500000000);
	put_file(data, "saves/snes/Zelda.srm", 800, 77, 1500000000);         /* user's save */

	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && p);
	if (!p)
		return;
	printf("  plan: layout=%s nsys=%d rom %d/%d (%llu B) bios %d/%d save %d/%d conflicts %d "
	       "theme %d/%d identical %d replace %d unknown %d skipped_names %d fits %d\n",
	       p->layout, p->nsys, p->rom_copy, p->rom_files, (unsigned long long)p->rom_bytes,
	       p->bios_copy, p->bios_files, p->save_copy, p->save_files, p->save_conflicts,
	       p->theme_copy, p->theme_files, p->identical, p->replace, p->nunknown,
	       p->skipped_names, p->fits);
	CHECK(!strcmp(p->layout, "roms"));
	/* ROMs: Zelda, Kirby, Sonic (x1), Same, Check, Check2, Bad_Name_, Metroid, Wario = 9 */
	CHECK(p->rom_files == 9);
	CHECK(p->rom_copy == 8);                 /* all but Same */
	CHECK(p->identical == 1);
	CHECK(p->bios_files == 2 && p->bios_copy == 2);
	CHECK(p->save_files == 4);               /* Zelda.srm Mario.srm Mario.state1 Tetris.srm */
	CHECK(p->save_conflicts == 1);
	CHECK(p->save_copy == 3);
	CHECK(p->theme_files == 1);
	CHECK(p->nunknown == 1 && !strcmp(p->unknown[0], "dreamcast") && p->unknown_files[0] == 1);
	CHECK(p->fits);
	{
		bool snes = false, md = false, gba = false, gbc = false;

		for (int i = 0; i < p->nsys; i++) {
			if (!strcmp(p->sys[i].id, "snes"))
				snes = p->sys[i].files == 6 && p->sys[i].saves == 3;
			if (!strcmp(p->sys[i].id, "megadrive"))
				md = p->sys[i].files == 1;
			if (!strcmp(p->sys[i].id, "gba"))
				gba = p->sys[i].to_copy == 1;
			if (!strcmp(p->sys[i].id, "gbc"))
				gbc = p->sys[i].files == 1;
		}
		CHECK(snes && md && gba && gbc);
	}

	transfer_import_defaults(&o);
	r = transfer_import_run(p, &o, NULL, NULL, NULL, &res);
	CHECK(r == 0 && res.state == TRANSFER_DONE);
	printf("  run1: copied %d skipped %d failed %d conflicts_kept %d bytes %llu/%llu\n",
	       res.copied, res.skipped, res.failed, res.conflicts_kept,
	       (unsigned long long)res.bytes_done, (unsigned long long)res.bytes_total);
	CHECK(res.failed == 0);
	CHECK(res.conflicts_kept == 1);
	CHECK(res.copied == 7 + 2 + 3 + 1);      /* ROMs (Check skipped as same) + bios + saves + theme */
	CHECK(res.bytes_done == res.bytes_total);
	pathf(a, sizeof(a), "%s/roms/snes/Zelda.sfc", stick);
	pathf(b, sizeof(b), "%s/roms/snes/Zelda.sfc", data);
	CHECK(same_files(a, b));
	pathf(a, sizeof(a), "%s/roms/snes/sub dir/Kirby.sfc", stick);
	pathf(b, sizeof(b), "%s/roms/snes/sub dir/Kirby.sfc", data);
	CHECK(same_files(a, b));
	pathf(a, sizeof(a), "%s/roms/snes/Check2.sfc", stick);
	pathf(b, sizeof(b), "%s/roms/snes/Check2.sfc", data);
	CHECK(same_files(a, b));
	CHECK(exists(data, "roms/snes/Bad_Name_.sfc"));
	CHECK(exists(data, "roms/megadrive/Sonic.md"));
	CHECK(exists(data, "roms/gba/Metroid.gba"));
	CHECK(exists(data, "roms/gbc/Wario.gbc"));
	CHECK(exists(data, "bios/gba_bios.bin") && exists(data, "bios/scph5501.bin"));
	CHECK(exists(data, "saves/snes/Mario.srm") && exists(data, "states/snes/Mario.state1"));
	CHECK(exists(data, "saves/gb/Tetris.srm"));
	CHECK(exists(data, "themes/mytheme/theme.xml"));
	CHECK(!exists(data, "roms/snes/Zelda.srm") && !exists(data, "roms/snes/._Zelda.sfc"));
	CHECK(!exists(data, "roms/snes/link.sfc") && !exists(data, "roms/dreamcast"));
	/* the user's save was kept */
	pathf(a, sizeof(a), "%s/roms/snes/Zelda.srm", stick);
	pathf(b, sizeof(b), "%s/saves/snes/Zelda.srm", data);
	CHECK(!same_files(a, b));
	CHECK(count_parts(data) == 0);
	{
		struct stat st;

		pathf(b, sizeof(b), "%s/roms/snes/Zelda.sfc", data);
		CHECK(stat(b, &st) == 0 && st.st_mtim.tv_sec == 1600000000);   /* mtime kept */
		pathf(b, sizeof(b), "%s/roms/snes/Check.sfc", data);
		CHECK(stat(b, &st) == 0 && st.st_mtim.tv_sec == 1600000000);   /* re-stamped */
	}
	transfer_plan_free(p);

	/* play-from-USB folder discovery */
	{
		struct transfer_rom_dir rd[16];
		int nd = transfer_find_rom_dirs(stick, rd, 16), md = 0, gbc = 0;

		for (int i = 0; i < nd; i++) {
			md += !strcmp(rd[i].system, "megadrive");
			gbc += !strcmp(rd[i].system, "gbc") && strstr(rd[i].path, "Game Boy Color (GBC)");
		}
		CHECK(nd == 5 && md == 2 && gbc == 1);
	}

	/* second run: nothing to do except the kept conflict */
	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && p && p->rom_copy == 0 && p->save_copy == 0 && p->save_conflicts == 1 &&
	      p->replace == 0 && p->bytes_to_copy == 800);
	if (p) {
		CHECK(transfer_import_run(p, NULL, NULL, NULL, NULL, &res) == 0 && res.copied == 0);
		transfer_plan_free(p);
	}

	/* overwrite_saves: replaced, old one kept as .bak */
	r = transfer_plan_build(stick, data, &p);
	o.overwrite_saves = true;
	CHECK(r == 0 && transfer_import_run(p, &o, NULL, NULL, NULL, &res) == 0 && res.copied == 1);
	pathf(a, sizeof(a), "%s/roms/snes/Zelda.srm", stick);
	pathf(b, sizeof(b), "%s/saves/snes/Zelda.srm", data);
	CHECK(same_files(a, b));
	CHECK(exists(data, "saves/snes/Zelda.srm.bak"));
	transfer_plan_free(p);

	/* async API + changed systems */
	put_file(stick, "roms/nes/Mario Bros.nes", 40000, 21, 1600000000);
	p = NULL;
	CHECK(transfer_plan_start(stick, data) == 0);
	CHECK(transfer_plan_start(stick, data) == -EBUSY);
	{
		int spins = 0;

		while ((r = transfer_plan_poll(&p)) == 0 && spins++ < 1000)
			usleep(2000);
	}
	CHECK(r == 1 && p && p->rom_copy == 1);
	CHECK(transfer_plan_poll(&p) == -EINVAL);
	CHECK(transfer_import_start(p, NULL) == 0);
	CHECK(transfer_import_start(p, NULL) == -EBUSY);
	{
		struct transfer_progress pr;
		char ch[8][TRANSFER_SYSID_MAX];
		int n, spins = 0;

		while (transfer_import_status(&pr) == TRANSFER_RUNNING && spins++ < 1000)
			usleep(2000);
		CHECK(pr.state == TRANSFER_DONE && pr.copied == 1);
		n = transfer_import_finish(ch, 8);
		CHECK(n == 1 && !strcmp(ch[0], "nes"));
		CHECK(transfer_import_status(NULL) == TRANSFER_IDLE);
	}

	/* cancel in the middle of a big file: no partial file left */
	put_file(stick, "roms/psx/Big.bin", 12u << 20, 22, 1600000000);
	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && p->rom_copy == 1);
	g_cancel = 0;
	r = transfer_import_run(p, NULL, cancel_cb, NULL, &g_cancel, &res);
	CHECK(r == 0 && res.state == TRANSFER_CANCELLED && g_cb_calls > 0);
	CHECK(!exists(data, "roms/psx/Big.bin"));
	CHECK(count_parts(data) == 0);
	transfer_plan_free(p);

	/* stick with system folders at the root only */
	{
		char s2[512];

		pathf(s2, sizeof(s2), "%s/stick2", g_tmp);
		put_file(s2, "GBA/Advance Wars.gba", 100, 23, 1600000000);
		put_file(s2, "Genesis/Streets.md", 100, 24, 1600000000);
		put_file(s2, "Photos/cat.jpg", 100, 25, 1600000000);
		r = transfer_plan_build(s2, data, &p);
		CHECK(r == 0 && !strcmp(p->layout, "(root)") && p->rom_files == 2 && p->nunknown == 0);
		transfer_plan_free(p);
	}
	CHECK(transfer_plan_build("/nonexistent/stick", data, &p) == -ENOENT);
}

/* ------------------------------------------------------------ trees */

static void test_trees(void)
{
	char s[512], data[512], p[1024];
	struct transfer_tree t[TRANSFER_TREES_MAX];
	struct transfer_plan *pl = NULL;
	int n;

	pathf(s, sizeof(s), "%s/multi", g_tmp);
	pathf(data, sizeof(data), "%s/multi-data", g_tmp);
	mkdirs(data);
	put_file(s, "roms/snes/A.sfc", 1000, 1, 1600000000);
	put_file(s, "roms/nes/B.nes", 500, 2, 1600000000);
	put_file(s, "roms/dreamcast/x.cdi", 50, 3, 1600000000);                  /* unknown system */
	put_file(s, "RetroPie/roms/gba/C.gba", 2000, 4, 1600000000);
	put_file(s, "RetroPie/BIOS/gba_bios.bin", 100, 5, 1600000000);
	put_file(s, "Backups/RetroStone2-20260101-120000/roms/megadrive/D.md", 700, 6, 1600000000);
	put_file(s, "Backups/RetroStone2-20260101-120000/saves/megadrive/D.srm", 64, 7, 1600000000);
	put_file(s, "Backups/RetroStone2-20260101-120000/RetroStone2-backup.txt", 50, 8, 1600000000);
	put_file(s, "Old/Handheld/SD/Roms/GB/E.gb", 300, 9, 1600000000);         /* depth 3 */
	put_file(s, "Deep/a/b/c/d/roms/snes/F.sfc", 300, 10, 1600000000);       /* depth 5: too deep */
	put_file(s, "Music/song.mp3", 100, 11, 1600000000);
	put_file(s, ".hidden/roms/snes/G.sfc", 100, 12, 1600000000);
	put_file(s, "System Volume Information/roms/snes/H.sfc", 100, 13, 1600000000);
	pathf(p, sizeof(p), "%s/Empty/roms/snes", s);
	mkdirs(p);                                                                /* empty skeleton */

	n = transfer_find_trees(s, t, TRANSFER_TREES_MAX);
	CHECK(n == 4);
	if (n == 4) {
		CHECK(t[0].rel[0] == 0 && !t[0].backup && t[0].nsys == 2 && t[0].games == 2);
		CHECK(!strcmp(t[1].rel, "Backups/RetroStone2-20260101-120000") && t[1].backup &&
		      t[1].games == 1 && t[1].files == 2 && !strcmp(t[1].systems, "megadrive"));
		CHECK(!strcmp(t[2].rel, "Old/Handheld/SD") && t[2].games == 1 && !strcmp(t[2].systems, "gb"));
		CHECK(!strcmp(t[3].rel, "RetroPie") && t[3].games == 1 && t[3].files == 2 &&
		      t[3].bytes == 2100);
		printf("  trees: \"%s\" %s %d; \"%s\" %d; \"%s\" %d; \"%s\" %d\n", t[0].rel, t[0].systems,
		       t[0].games, t[1].rel, t[1].games, t[2].rel, t[2].games, t[3].rel, t[3].games);
	}
	/* every library ("All") and one of them */
	if (n == 4) {
		const char *roots[4] = { t[0].path, t[1].path, t[2].path, t[3].path };

		CHECK(transfer_plan_build_trees(roots, 4, data, &pl) == 0 && pl);
		CHECK(pl && pl->rom_files == 5 && pl->bios_files == 1 && pl->save_files == 1 &&
		      pl->nunknown == 1);
		transfer_plan_free(pl);
		CHECK(transfer_plan_build_trees(roots + 3, 1, data, &pl) == 0 && pl);
		CHECK(pl && pl->rom_files == 1 && pl->bios_files == 1 && pl->nsys == 1);
		transfer_plan_free(pl);
	}
	/* async */
	{
		struct transfer_tree t2[TRANSFER_TREES_MAX];
		int r, n2 = -1, spins = 0;

		CHECK(transfer_trees_start(s) == 0);
		CHECK(transfer_trees_start(s) == -EBUSY);
		while ((r = transfer_trees_poll(t2, TRANSFER_TREES_MAX, &n2)) == 0 && spins++ < 1000)
			usleep(2000);
		CHECK(r == 1 && n2 == 4);
	}
	/* an empty stick, a stick with only BIOS files */
	pathf(s, sizeof(s), "%s/empty-stick", g_tmp);
	mkdirs(s);
	CHECK(transfer_find_trees(s, t, TRANSFER_TREES_MAX) == 0);
	put_file(s, "bios/scph1001.bin", 100, 14, 1600000000);
	n = transfer_find_trees(s, t, TRANSFER_TREES_MAX);
	CHECK(n == 1 && t[0].games == 0 && t[0].files == 1);
	CHECK(transfer_find_trees("/nonexistent/stick", t, TRANSFER_TREES_MAX) == -ENOENT);
}

/* ------------------------------------------------------------ duplicates */

static int g_ask_n;
static enum transfer_answer g_answers[16];
static struct transfer_question g_qs[16];

static enum transfer_answer ask_script(const struct transfer_question *q, void *user)
{
	(void)user;
	if (g_ask_n >= 16)
		return TRANSFER_ANSWER_SKIP;
	g_qs[g_ask_n] = *q;
	return g_answers[g_ask_n++];
}

/* File content: seed pattern, with one byte changed at flip (if >= 0). */
static void put_file_flip(const char *root, const char *rel, size_t size, int seed, time_t mtime,
			  long flip)
{
	char p[1024];
	FILE *f;

	put_file(root, rel, size, seed, mtime);
	if (flip < 0)
		return;
	pathf(p, sizeof(p), "%s/%s", root, rel);
	f = fopen(p, "r+b");
	fseek(f, flip, SEEK_SET);
	fputc(0x5a ^ fgetc(f), f);
	fseek(f, flip, SEEK_SET);
	fputc(0xa5, f);
	fclose(f);
	if (mtime >= 0) {
		struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } };

		utimensat(AT_FDCWD, p, ts, 0);
	}
}

static bool same_rel(const char *ra, const char *rb, const char *rel)
{
	char a[1024], b[1024];

	pathf(a, sizeof(a), "%s/%s", ra, rel);
	pathf(b, sizeof(b), "%s/%s", rb, rel);
	return same_files(a, b);
}

static void test_duplicates(void)
{
	char stick[512], data[512];
	struct transfer_plan *p = NULL;
	struct transfer_import_opts o;
	struct transfer_progress res;
	const char *five[] = { "A", "B", "C", "D", "E" };
	int r;

	pathf(stick, sizeof(stick), "%s/dup-stick", g_tmp);
	pathf(data, sizeof(data), "%s/dup-data", g_tmp);
	for (int i = 0; i < 5; i++) {
		char rel[64];

		snprintf(rel, sizeof(rel), "roms/snes/%s.sfc", five[i]);
		put_file(stick, rel, 1000, 1 + i, 1600000000);
		put_file(data, rel, 1100, 11 + i, 1500000000);                   /* other size */
	}
	put_file(stick, "roms/snes/Same.sfc", 3000, 40, 1600000000);
	put_file(data, "roms/snes/Same.sfc", 3000, 40, 1500000000);               /* same content */
	/* 3 MiB, one byte changed between two samples: found only by the full
	 * compare while copying */
	put_file_flip(stick, "roms/snes/Big1.bin", 3u << 20, 41, 1600000000, -1);
	put_file_flip(data, "roms/snes/Big1.bin", 3u << 20, 41, 1500000000, 100000);
	/* 3 MiB changed in the first block: found by the samples at plan time */
	put_file_flip(stick, "roms/snes/Big2.bin", 3u << 20, 42, 1600000000, -1);
	put_file_flip(data, "roms/snes/Big2.bin", 3u << 20, 42, 1500000000, 10);
	put_file(stick, "saves/snes/s1.srm", 512, 21, 1600000000);
	put_file(stick, "saves/snes/s2.srm", 512, 22, 1600000000);
	put_file(data, "saves/snes/s1.srm", 512, 31, 1500000000);
	put_file(data, "saves/snes/s2.srm", 512, 32, 1500000000);

	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && p);
	if (!p)
		return;
	/* A-E and Big2 differ for sure; Same and Big1 look alike (checked while
	 * copying); both saves differ */
	CHECK(p->replace == 6 && p->save_conflicts == 2 && p->identical == 0);
	transfer_import_defaults(&o);
	o.ask = ask_script;
	/* order of the items: A, B, Big1, Big2, C, D, E, Same, s1, s2 */
	g_ask_n = 0;
	g_answers[0] = TRANSFER_ANSWER_SKIP;          /* A */
	g_answers[1] = TRANSFER_ANSWER_REPLACE;       /* B */
	g_answers[2] = TRANSFER_ANSWER_REPLACE;       /* Big1 (found while copying) */
	g_answers[3] = TRANSFER_ANSWER_SKIP_ALL;      /* Big2, then C, D, E without asking */
	g_answers[4] = TRANSFER_ANSWER_REPLACE_ALL;   /* s1, then s2 without asking */
	r = transfer_import_run(p, &o, NULL, NULL, NULL, &res);
	transfer_plan_free(p);
	CHECK(r == 0 && res.state == TRANSFER_DONE);
	printf("  dup run1: asked %d, copied %d replaced %d kept %d identical %d\n", g_ask_n, res.copied,
	       res.replaced, res.kept, res.identical);
	CHECK(g_ask_n == 5);
	CHECK(!strcmp(g_qs[0].path, "roms/snes/A.sfc") && !g_qs[0].save && g_qs[0].src_size == 1000 &&
	      g_qs[0].dst_size == 1100 && g_qs[0].src_mtime == 1600000000 && g_qs[0].dst_mtime == 1500000000);
	CHECK(!strcmp(g_qs[2].path, "roms/snes/Big1.bin") && !strcmp(g_qs[3].path, "roms/snes/Big2.bin"));
	CHECK(!strcmp(g_qs[4].path, "saves/snes/s1.srm") && g_qs[4].save && g_qs[4].seq == 5);
	CHECK(res.replaced == 4 && res.copied == 4 && res.kept == 5 && res.identical == 1 &&
	      res.conflicts_kept == 0 && res.skipped == 6);
	CHECK(!same_rel(stick, data, "roms/snes/A.sfc") && same_rel(stick, data, "roms/snes/B.sfc"));
	CHECK(same_rel(stick, data, "roms/snes/Big1.bin") && !same_rel(stick, data, "roms/snes/Big2.bin"));
	CHECK(!same_rel(stick, data, "roms/snes/C.sfc") && !same_rel(stick, data, "roms/snes/E.sfc"));
	CHECK(same_rel(stick, data, "saves/snes/s1.srm") && same_rel(stick, data, "saves/snes/s2.srm"));
	CHECK(exists(data, "saves/snes/s1.srm.bak") && exists(data, "saves/snes/s2.srm.bak"));
	CHECK(!exists(data, "roms/snes/B.sfc.bak"));                          /* only saves keep one */
	CHECK(count_parts(data) == 0);

	/* second import: Replace all at the first question */
	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && p && p->replace == 5 && p->save_conflicts == 0);
	g_ask_n = 0;
	g_answers[0] = TRANSFER_ANSWER_REPLACE_ALL;
	r = transfer_import_run(p, &o, NULL, NULL, NULL, &res);
	transfer_plan_free(p);
	CHECK(r == 0 && g_ask_n == 1 && res.replaced == 5 && res.kept == 0);
	CHECK(same_rel(stick, data, "roms/snes/A.sfc") && same_rel(stick, data, "roms/snes/E.sfc") &&
	      same_rel(stick, data, "roms/snes/Big2.bin"));

	/* a policy instead of a question: never asked */
	put_file(data, "roms/snes/C.sfc", 1100, 99, 1500000000);
	r = transfer_plan_build(stick, data, &p);
	g_ask_n = 0;
	o.dup_files = TRANSFER_DUP_SKIP;
	r |= transfer_import_run(p, &o, NULL, NULL, NULL, &res);
	transfer_plan_free(p);
	CHECK(r == 0 && g_ask_n == 0 && res.kept == 1 && !same_rel(stick, data, "roms/snes/C.sfc"));

	/* the async import waits for the UI's answer */
	r = transfer_plan_build(stick, data, &p);
	transfer_import_defaults(&o);
	CHECK(r == 0 && transfer_import_start(p, &o) == 0);
	{
		struct transfer_progress pr;
		char ch[8][TRANSFER_SYSID_MAX];
		int spins = 0, asked = 0;

		while (transfer_import_status(&pr) == TRANSFER_RUNNING && spins++ < 2000) {
			if (pr.asking && !asked) {
				asked = pr.question.seq;
				CHECK(!strcmp(pr.question.path, "roms/snes/C.sfc"));
				transfer_import_answer(TRANSFER_ANSWER_REPLACE);
			}
			usleep(2000);
		}
		CHECK(asked == 1 && pr.state == TRANSFER_DONE && pr.replaced == 1);
		CHECK(transfer_import_finish(ch, 8) == 1 && !strcmp(ch[0], "snes"));
		CHECK(same_rel(stick, data, "roms/snes/C.sfc"));
	}
	/* cancel while a question is open */
	put_file(data, "roms/snes/D.sfc", 1100, 98, 1500000000);
	r = transfer_plan_build(stick, data, &p);
	CHECK(r == 0 && transfer_import_start(p, &o) == 0);
	{
		struct transfer_progress pr;
		char ch[8][TRANSFER_SYSID_MAX];
		int spins = 0;

		while (transfer_import_status(&pr) == TRANSFER_RUNNING && !pr.asking && spins++ < 2000)
			usleep(2000);
		transfer_import_cancel();
		while (transfer_import_status(&pr) == TRANSFER_RUNNING && spins++ < 4000)
			usleep(2000);
		CHECK(pr.state == TRANSFER_CANCELLED);
		transfer_import_finish(ch, 8);
		CHECK(!same_rel(stick, data, "roms/snes/D.sfc") && count_parts(data) == 0);
	}
}

/* ------------------------------------------------------------ backup */

static bool trees_equal(const char *a, const char *b, const char *sub)
{
	char cmd[1400];

	/* dotfiles are never copied either way; .bak are safety copies */
	pathf(cmd, sizeof(cmd), "diff -r -x '.*' -x '*.bak' '%s/%s' '%s/%s' > /dev/null", a, sub, b, sub);
	return system(cmd) == 0;
}

static bool file_has(const char *root, const char *rel, const char *text)
{
	char cmd[1400];

	pathf(cmd, sizeof(cmd), "grep -q '%s' '%s/%s'", text, root, rel);
	return system(cmd) == 0;
}

/* Scan + run in one go. */
static int bk(const char *data, const char *stick, const char *fs, const struct transfer_backup_opts *o,
	      volatile int *cancel, transfer_progress_fn cb, struct transfer_progress *res,
	      struct transfer_backup_totals *tot)
{
	struct transfer_backup *b = NULL;
	int r = transfer_backup_scan(data, stick, fs, o->mode, &b);

	if (r < 0)
		return r;
	if (tot)
		transfer_backup_totals(b, o, tot);
	r = transfer_backup_run(b, o, cb, NULL, cancel, res);
	transfer_backup_free(b);
	return r;
}

static void test_backup(void)
{
	char data[512], stick[512], data2[512], bk_dir[600], p[1200];
	struct transfer_backup *b = NULL;
	const struct transfer_backup_info *in;
	struct transfer_backup_opts o;
	struct transfer_backup_totals t;
	struct transfer_progress res;
	struct transfer_tree tr[TRANSFER_TREES_MAX];
	struct transfer_plan *pl = NULL;
	int r, n;

	pathf(data, sizeof(data), "%s/bk-data", g_tmp);
	pathf(stick, sizeof(stick), "%s/bk-stick", g_tmp);
	pathf(data2, sizeof(data2), "%s/bk-data2", g_tmp);
	pathf(bk_dir, sizeof(bk_dir), "%s/" TRANSFER_BACKUP_DIR, stick);
	mkdirs(stick);
	mkdirs(data2);
	put_file(data, "roms/snes/A (USA).sfc", 5000, 1, 1600000000);
	put_file(data, "roms/snes/sub dir/B.sfc", 3000, 2, 1600000001);
	put_file(data, "roms/snes/gamelist.xml", 300, 3, 1600000002);
	put_file(data, "roms/nes/C.nes", 4000, 4, 1600000003);
	put_file(data, "roms/snes/.hidden", 10, 5, -1);                          /* never copied */
	put_file(data, "roms/snes/.A.sfc.rsos-part", 10, 6, -1);                 /* temp leftover */
	put_file(data, "bios/scph5501.bin", 2048, 9, 1600000006);
	put_file(data, "saves/snes/A (USA).srm", 800, 7, 1600000004);
	put_file(data, "states/snes/A (USA).state1", 900, 8, 1600000005);
	put_file(data, "states/snes/A (USA).state.auto", 900, 10, 1600000005);
	put_file(data, "states/snes/A (USA).state.auto.png", 300, 11, 1600000005);
	put_file(data, "saves/nes/C.srm", 800, 12, 1600000004);
	put_file(data, "rsos/coreopts/snes9x/A (USA).ini", 40, 13, 1600000004);
	put_file(data, "rsos/coreopts/snes9x.ini", 40, 14, 1600000004);
	put_file(data, "rsos/remaps/snes/A (USA).ini", 40, 15, 1600000004);
	put_file(data, "rsos/settings.ini", 40, 16, 1600000004);                  /* not a backup file */
	put_file(data, "screenshots/snes-A.png", 700, 17, 1600000007);           /* not either */

	/* --- export games: roms + bios into RetroStone2/ */
	r = transfer_backup_scan(data, stick, "exfat", TRANSFER_EXPORT_GAMES, &b);
	CHECK(r == 0 && b);
	if (!b)
		return;
	in = transfer_backup_get_info(b);
	CHECK(in->files[TRANSFER_BK_ROMS] == 4 && in->files[TRANSFER_BK_BIOS] == 1 &&
	      in->files[TRANSFER_BK_SAVES] == 0 && !in->exists && !in->fat32 && in->nbig == 0);
	CHECK(!strcmp(in->folder, TRANSFER_BACKUP_DIR));
	transfer_backup_defaults(&o, TRANSFER_EXPORT_GAMES);
	CHECK(o.bios && !o.settings);
	transfer_backup_totals(b, &o, &t);
	CHECK(t.files == 5 && t.bytes == 12300 + 2048 && t.unchanged == 0 && t.fits && t.need > t.bytes);
	r = transfer_backup_run(b, &o, NULL, NULL, NULL, &res);
	CHECK(r == 0 && res.state == TRANSFER_DONE && res.copied == 5 && res.identical == 0 &&
	      res.bytes_done == res.bytes_total && !strcmp(res.folder, TRANSFER_BACKUP_DIR));
	transfer_backup_free(b);
	CHECK(exists(bk_dir, TRANSFER_BACKUP_MARKER) && exists(bk_dir, "RetroStone2-backup.txt"));
	CHECK(file_has(bk_dir, TRANSFER_BACKUP_MARKER, "^games_updated=[1-9]") &&
	      file_has(bk_dir, TRANSFER_BACKUP_MARKER, "^saves_updated=0") &&
	      file_has(bk_dir, TRANSFER_BACKUP_MARKER, "^console=rs2-"));
	CHECK(exists(data, "rsos/console-id"));
	CHECK(exists(bk_dir, "roms/snes/sub dir/B.sfc") && exists(bk_dir, "bios/scph5501.bin"));
	CHECK(!exists(bk_dir, "roms/snes/.hidden") && !exists(bk_dir, "roms/snes/.A.sfc.rsos-part"));
	CHECK(!exists(bk_dir, "saves") && !exists(bk_dir, "screenshots") && count_parts(stick) == 0);
	{
		struct stat st;

		pathf(p, sizeof(p), "%s/roms/nes/C.nes", bk_dir);
		CHECK(stat(p, &st) == 0 && st.st_mtim.tv_sec == 1600000003);    /* mtime kept */
	}
	/* again: nothing to copy */
	r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
	CHECK(r == 0 && t.files == 0 && t.unchanged == 5 && res.copied == 0 && res.identical == 5);
	/* a changed game, a new one, a game removed from the console, a stick
	 * file with a 1 s older date (FAT's 2 s resolution): only the first two */
	put_file(data, "roms/nes/C.nes", 4100, 40, 1600000100);
	put_file(data, "roms/nes/D.nes", 1000, 41, 1600000101);
	pathf(p, sizeof(p), "%s/roms/snes/gamelist.xml", data);
	unlink(p);
	{
		struct timespec ts[2] = { { 1600000000, 0 }, { 1600000000, 0 } };

		pathf(p, sizeof(p), "%s/roms/snes/sub dir/B.sfc", bk_dir);
		utimensat(AT_FDCWD, p, ts, 0);
	}
	r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
	CHECK(r == 0 && t.files == 2 && t.replace == 1 && t.unchanged == 3 && res.copied == 2 &&
	      res.replaced == 1 && res.identical == 3);
	CHECK(same_rel(data, bk_dir, "roms/nes/C.nes") && same_rel(data, bk_dir, "roms/nes/D.nes"));
	CHECK(exists(bk_dir, "roms/snes/gamelist.xml"));                      /* never deleted */
	CHECK(!exists(bk_dir, "roms/nes/C.nes.bak"));                          /* games: no .bak */
	/* without BIOS */
	put_file(data, "bios/other.bin", 100, 42, 1600000000);
	o.bios = false;
	r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
	CHECK(r == 0 && res.copied == 0 && !exists(bk_dir, "bios/other.bin"));
	o.bios = true;

	/* --- back up saves: every game, no settings */
	transfer_backup_defaults(&o, TRANSFER_BACKUP_SAVES);
	r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
	CHECK(r == 0 && res.state == TRANSFER_DONE && res.copied == 5 && res.identical == 0);
	CHECK(exists(bk_dir, "saves/snes/A (USA).srm") && exists(bk_dir, "states/snes/A (USA).state.auto.png") &&
	      exists(bk_dir, "saves/nes/C.srm") && !exists(bk_dir, "rsos"));
	CHECK(file_has(bk_dir, TRANSFER_BACKUP_MARKER, "^saves_updated=[1-9]") &&
	      file_has(bk_dir, TRANSFER_BACKUP_MARKER, "^games_updated=[1-9]"));  /* kept */
	/* with the settings */
	o.settings = true;
	r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
	CHECK(r == 0 && res.copied == 3 && res.identical == 5);
	CHECK(exists(bk_dir, "rsos/coreopts/snes9x/A (USA).ini") && exists(bk_dir, "rsos/remaps/snes/A (USA).ini") &&
	      !exists(bk_dir, "rsos/settings.ini"));
	/* a save changes twice: the stick keeps one previous version (.bak);
	 * same size and date but other content: still copied (compared by
	 * content) */
	put_file(data, "saves/snes/A (USA).srm", 800, 50, 1600000004);
	r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
	CHECK(r == 0 && res.copied == 1 && res.replaced == 1 && same_rel(data, bk_dir, "saves/snes/A (USA).srm"));
	{
		char a[1300], c[1300];

		pathf(p, sizeof(p), "%s/saves/snes/A (USA).srm.bak", bk_dir);
		put_file(g_tmp, "bk-expect/v1.srm", 800, 7, 1600000004);
		pathf(a, sizeof(a), "%s/bk-expect/v1.srm", g_tmp);
		CHECK(same_files(a, p));                                  /* the first version */
		put_file(data, "saves/snes/A (USA).srm", 800, 51, 1600000009);
		r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
		put_file(g_tmp, "bk-expect/v2.srm", 800, 50, 1600000004);
		pathf(c, sizeof(c), "%s/bk-expect/v2.srm", g_tmp);
		CHECK(r == 0 && res.copied == 1 && same_files(c, p));      /* now the second */
		pathf(p, sizeof(p), "%s/saves/snes", bk_dir);
		CHECK(count_parts(p) == 0);
	}

	/* --- the last played game only: its saves, states, settings */
	put_file(data, "saves/snes/A (USA).srm", 800, 52, 1600000010);
	put_file(data, "states/snes/A (USA).state.auto", 900, 53, 1600000010);
	put_file(data, "saves/nes/C.srm", 800, 54, 1600000010);               /* another game */
	pathf(o.game_rom, sizeof(o.game_rom), "%s/roms/snes/A (USA).sfc", data);
	snprintf(o.game_system, sizeof(o.game_system), "snes");
	r = bk(data, stick, "exfat", &o, NULL, NULL, &res, &t);
	printf("  last played: copied %d (%s, %s), unchanged %d\n", res.copied,
	       res.ncopied_names > 0 ? res.copied_names[0] : "", res.ncopied_names > 1 ? res.copied_names[1] : "",
	       res.identical);
	/* A: .srm, .state1, .state.auto, .state.auto.png, 2 settings = 6, 2 changed */
	CHECK(r == 0 && res.copied == 2 && res.identical == 4 && res.ncopied_names == 2);
	CHECK(!same_rel(data, bk_dir, "saves/nes/C.srm"));                   /* not this game */
	CHECK(same_rel(data, bk_dir, "states/snes/A (USA).state.auto"));
	o.game_rom[0] = 0;

	/* --- round trip: RetroStone2/ imported into an empty console */
	n = transfer_find_trees(stick, tr, TRANSFER_TREES_MAX);
	CHECK(n == 1 && tr[0].backup && !strcmp(tr[0].rel, TRANSFER_BACKUP_DIR) && tr[0].backup_time > 0);
	if (n == 1) {
		const char *roots[1] = { tr[0].path };

		r = bk(data, stick, "exfat", &o, NULL, NULL, &res, NULL);   /* saves up to date */
		{
			struct transfer_backup_opts eo;

			transfer_backup_defaults(&eo, TRANSFER_EXPORT_GAMES);
			r |= bk(data, stick, "exfat", &eo, NULL, NULL, &res, NULL);   /* games too */
		}
		CHECK(r == 0 && transfer_plan_build_trees(roots, 1, data2, &pl) == 0 && pl);
		CHECK(pl && transfer_import_run(pl, NULL, NULL, NULL, NULL, &res) == 0 && res.failed == 0);
		transfer_plan_free(pl);
		CHECK(trees_equal(data, data2, "roms/nes") && trees_equal(data, data2, "bios") &&
		      trees_equal(data, data2, "saves") && trees_equal(data, data2, "states") &&
		      trees_equal(data, data2, "rsos/coreopts") && trees_equal(data, data2, "rsos/remaps"));
		CHECK(!exists(data2, "saves/snes/A (USA).srm.bak"));               /* safety copies stay */
		/* and back into the console it came from: all silently identical */
		g_ask_n = 0;
		CHECK(transfer_plan_build_trees(roots, 1, data, &pl) == 0 && pl && pl->replace == 0 &&
		      pl->save_conflicts == 0);
		if (pl) {
			struct transfer_import_opts io;

			/* only gamelist.xml, removed from the console, is new */
			CHECK(pl->identical == pl->nitems - 1);
			transfer_import_defaults(&io);
			io.ask = ask_script;
			CHECK(transfer_import_run(pl, &io, NULL, NULL, NULL, &res) == 0 && g_ask_n == 0 &&
			      res.copied == 1 && res.identical == pl->nitems - 1);
		}
		transfer_plan_free(pl);
	}

	/* --- FAT32: a file over 4 GiB is listed and skipped (a sparse file) */
	pathf(p, sizeof(p), "%s/roms/psx", data);
	mkdirs(p);
	pathf(p, sizeof(p), "%s/roms/psx/Huge.bin", data);
	{
		int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		CHECK(fd >= 0 && ftruncate(fd, (off_t)TRANSFER_FAT32_MAX + 2) == 0);
		if (fd >= 0)
			close(fd);
	}
	transfer_backup_defaults(&o, TRANSFER_EXPORT_GAMES);
	r = transfer_backup_scan(data, stick, "vfat", TRANSFER_EXPORT_GAMES, &b);
	CHECK(r == 0 && b);
	if (b) {
		in = transfer_backup_get_info(b);
		CHECK(in->fat32 && in->nbig == 1 && !strcmp(in->big[0], "roms/psx/Huge.bin"));
		transfer_backup_totals(b, &o, &t);
		CHECK(t.too_big == 1 && t.files == 0);
		r = transfer_backup_run(b, &o, NULL, NULL, NULL, &res);
		CHECK(r == 0 && res.state == TRANSFER_DONE && res.copied == 0 && res.skipped == 1);
		CHECK(!exists(bk_dir, "roms/psx/Huge.bin") && file_has(bk_dir, "RetroStone2-backup.txt", "Huge.bin"));
		transfer_backup_free(b);
	}
	r = transfer_backup_scan(data, stick, "exfat", TRANSFER_EXPORT_GAMES, &b);
	CHECK(r == 0 && b && transfer_backup_get_info(b)->nbig == 0);             /* exFAT: fine */
	transfer_backup_free(b);
	pathf(p, sizeof(p), "%s/roms/psx/Huge.bin", data);
	unlink(p);

	/* --- cancel in the middle of a 12 MiB file: no partial file */
	put_file(data, "roms/psx/Big.bin", 12u << 20, 60, 1600000000);
	g_cancel = 0;
	g_cb_calls = 0;
	r = bk(data, stick, "exfat", &o, &g_cancel, cancel_cb, &res, NULL);
	CHECK(r == 0 && res.state == TRANSFER_CANCELLED && g_cb_calls > 0);
	CHECK(!exists(bk_dir, "roms/psx/Big.bin") && count_parts(stick) == 0);
	CHECK(file_has(bk_dir, TRANSFER_BACKUP_MARKER, "^status=INCOMPLETE (cancelled)"));

	/* --- async, and the scan in a thread */
	CHECK(transfer_backup_scan_start(data, stick, "exfat", TRANSFER_EXPORT_GAMES) == 0);
	{
		int spins = 0;

		b = NULL;
		while ((r = transfer_backup_scan_poll(&b)) == 0 && spins++ < 1000)
			usleep(2000);
		CHECK(r == 1 && b);
	}
	if (b) {
		struct transfer_progress pr;
		int spins = 0;

		CHECK(transfer_backup_start(b, NULL) == 0);
		CHECK(transfer_backup_start(b, NULL) == -EBUSY);
		while (transfer_backup_status(&pr) == TRANSFER_RUNNING && spins++ < 3000)
			usleep(2000);
		CHECK(pr.state == TRANSFER_DONE && pr.copied == 1 && same_rel(data, bk_dir, "roms/psx/Big.bin"));
		transfer_backup_finish();
		CHECK(transfer_backup_status(NULL) == TRANSFER_IDLE);
	}
	/* a mode mismatch is refused */
	r = transfer_backup_scan(data, stick, "exfat", TRANSFER_EXPORT_GAMES, &b);
	transfer_backup_defaults(&o, TRANSFER_BACKUP_SAVES);
	CHECK(r == 0 && transfer_backup_run(b, &o, NULL, NULL, NULL, &res) == -EINVAL);
	transfer_backup_free(b);

	/* --- a full stick: stops, keeps the complete files, removes the partial one */
	if (geteuid() == 0) {
		char full[512], d4[512], cmd[1200], fb[600];

		pathf(full, sizeof(full), "%s/full-stick", g_tmp);
		pathf(d4, sizeof(d4), "%s/bk-data4", g_tmp);
		pathf(fb, sizeof(fb), "%s/" TRANSFER_BACKUP_DIR, full);
		mkdirs(full);
		for (int i = 0; i < 5; i++) {
			char rel[64];

			snprintf(rel, sizeof(rel), "roms/snes/%d.bin", i);
			put_file(d4, rel, 1u << 20, 20 + i, 1600000000);
		}
		pathf(cmd, sizeof(cmd), "mount -t tmpfs -o size=3m rsos-test '%s'", full);
		if (system(cmd) == 0) {
			transfer_backup_defaults(&o, TRANSFER_EXPORT_GAMES);
			r = bk(d4, full, "vfat", &o, NULL, NULL, &res, &t);
			printf("  full stick: %s, copied %d of 5\n", res.errmsg, res.copied);
			CHECK(!t.fits && t.files == 5);
			CHECK(r == -ENOSPC && res.state == TRANSFER_FAILED &&
			      !strcmp(res.errmsg, "The USB drive is full"));
			CHECK(res.copied >= 1 && res.copied < 5 && count_parts(full) == 0);
			n = 0;
			for (int i = 0; i < 5; i++) {
				char rel[64], a[1300], c[1300];

				snprintf(rel, sizeof(rel), "roms/snes/%d.bin", i);
				pathf(a, sizeof(a), "%s/%s", d4, rel);
				pathf(c, sizeof(c), "%s/%s", fb, rel);
				if (access(c, F_OK) == 0) {
					CHECK(same_files(a, c));             /* complete copies only */
					n++;
				}
			}
			CHECK(n == res.copied);
			pathf(cmd, sizeof(cmd), "umount '%s'", full);
			if (system(cmd) != 0)
				fprintf(stderr, "umount %s failed\n", full);
		} else {
			printf("  full stick: tmpfs mount failed, test skipped\n");
		}
	} else {
		printf("  full stick: needs root (tmpfs), test skipped\n");
	}
}

/* ------------------------------------------------------------ USB */

static int g_mounts, g_umounts;
static unsigned long g_last_flags;
static char g_last_fs[16], g_last_data[64], g_last_target[256];

/* a slow mount (a failing stick, ntfs3): it runs in usb.c's worker thread */
static volatile int g_mount_delay_ms, g_mount_started;

static int fake_mount(const char *src, const char *target, const char *fstype,
		      unsigned long flags, const char *data)
{
	(void)src;
	g_last_flags = flags;
	if (flags & MS_REMOUNT)
		return 0;
	g_mount_started = 1;
	if (g_mount_delay_ms)
		usleep((useconds_t)g_mount_delay_ms * 1000);
	g_mounts++;
	snprintf(g_last_fs, sizeof(g_last_fs), "%s", fstype);
	snprintf(g_last_data, sizeof(g_last_data), "%s", data ? data : "");
	snprintf(g_last_target, sizeof(g_last_target), "%s", target);
	return 0;
}

static int fake_umount(const char *target, int flags)
{
	(void)target;
	(void)flags;
	g_umounts++;
	return 0;
}

static void fake_disk(const char *sys, const char *dev, const char *disk, bool usb,
		      const char *part, uint64_t sectors, const unsigned char *bs)
{
	char dir[1024], p[1200];
	FILE *f;

	pathf(dir, sizeof(dir), "%s/devices/%s/%s", sys, usb ? "soc/1c14000.usb/usb1/1-1/host0" :
	      "soc/1c0f000.mmc", disk);
	mkdirs(dir);
	pathf(p, sizeof(p), "%s/block/%s", sys, disk);
	{
		char tgt[1200];

		pathf(tgt, sizeof(tgt), "../devices/%s/%s", usb ? "soc/1c14000.usb/usb1/1-1/host0" :
		      "soc/1c0f000.mmc", disk);
		CHECK(symlink(tgt, p) == 0);
	}
	pathf(p, sizeof(p), "%s/size", dir);
	f = fopen(p, "w");
	fprintf(f, "%llu\n", (unsigned long long)sectors);
	fclose(f);
	pathf(p, sizeof(p), "%s/device", dir);
	mkdir(p, 0755);
	pathf(p, sizeof(p), "%s/device/vendor", dir);
	f = fopen(p, "w");
	fprintf(f, "SanDisk \n");
	fclose(f);
	pathf(p, sizeof(p), "%s/device/model", dir);
	f = fopen(p, "w");
	fprintf(f, "Cruzer Blade    \n");
	fclose(f);
	if (part) {
		pathf(p, sizeof(p), "%s/%s", dir, part);
		mkdir(p, 0755);
		pathf(p, sizeof(p), "%s/%s/partition", dir, part);
		f = fopen(p, "w");
		fprintf(f, "1\n");
		fclose(f);
		pathf(p, sizeof(p), "%s/%s/size", dir, part);
		f = fopen(p, "w");
		fprintf(f, "%llu\n", (unsigned long long)sectors - 2048);
		fclose(f);
	}
	if (bs) {
		pathf(p, sizeof(p), "%s/%s", dev, part ? part : disk);
		write_img(p, bs, 512, 4096);
	}
}

static int drain(struct transfer_usb_event *evs, int max, int ms)
{
	int n = 0;

	for (int t = 0; t < ms; t += 5) {
		struct transfer_usb_event ev;

		while (transfer_usb_poll(&ev) > 0)
			if (n < max)
				evs[n++] = ev;
		usleep(5000);
	}
	return n;
}

static void test_usb(void)
{
	char sys[512], blk[600], dev[512], media[512], p[1200];
	unsigned char bs[512], junk[512];
	struct transfer_usb_config cfg = { 0 };
	struct transfer_usb_event ev[8];
	struct transfer_usb_drive drv[4];
	int n;

	pathf(sys, sizeof(sys), "%s/sys", g_tmp);
	pathf(blk, sizeof(blk), "%s/block", sys);
	pathf(dev, sizeof(dev), "%s/dev", g_tmp);
	pathf(media, sizeof(media), "%s/media", g_tmp);
	mkdirs(blk);
	mkdirs(dev);
	/* like the device: /media -> /run/media, the target missing at boot */
	pathf(p, sizeof(p), "%s/run/media", g_tmp);
	CHECK(symlink(p, media) == 0);

	make_fat32(bs, "MYROMS     ");
	fake_disk(sys, dev, "mmcblk0", false, "mmcblk0p1", 1u << 24, bs);  /* the SD: ignored */

	cfg.mount_base = media;
	cfg.sys_block = blk;
	cfg.dev_dir = dev;
	cfg.settle_ms = 30;
	cfg.rescan_ms = 10;
	cfg.no_netlink = true;
	cfg.mount_fn = fake_mount;
	cfg.umount_fn = fake_umount;
	CHECK(transfer_usb_init(&cfg) == 0);
	CHECK(transfer_usb_fd() == -1);
	n = drain(ev, 8, 80);
	CHECK(n == 0 && g_mounts == 0);

	/* plug a FAT32 stick */
	fake_disk(sys, dev, "sda", true, "sda1", 1u << 22, bs);
	n = drain(ev, 8, 10);
	CHECK(n == 0);                     /* not settled yet */
	CHECK(transfer_usb_timeout_ms() >= 0 && transfer_usb_timeout_ms() <= 200);   /* settling */
	n = drain(ev, 8, 120);
	CHECK(n == 1 && ev[0].type == TRANSFER_USB_MOUNTED);
	if (n == 1) {
		pathf(p, sizeof(p), "%s/usb0", media);
		CHECK(!strcmp(ev[0].drive.mountpoint, p) && !strcmp(ev[0].drive.dev, "sda1") &&
		      !strcmp(ev[0].drive.fstype, "vfat") && !strcmp(ev[0].drive.label, "MYROMS") &&
		      !strcmp(ev[0].drive.vendor, "SanDisk Cruzer Blade") &&
		      ev[0].drive.size_bytes == ((1ull << 22) - 2048) * 512);
		CHECK(!strcmp(g_last_fs, "vfat") && strstr(g_last_data, "utf8"));
		/* read-write for a backup, then read-only again */
		CHECK(transfer_usb_remount(ev[0].drive.mountpoint, true) == 0 &&
		      (g_last_flags & MS_REMOUNT) && !(g_last_flags & MS_RDONLY) && (g_last_flags & MS_NOEXEC));
		CHECK(transfer_usb_remount(ev[0].drive.mountpoint, false) == 0 &&
		      (g_last_flags & (MS_REMOUNT | MS_RDONLY)) == (MS_REMOUNT | MS_RDONLY));
		CHECK(transfer_usb_remount("/nowhere", true) == -ENOENT && g_mounts == 1);
		{
			struct stat st;

			pathf(p, sizeof(p), "%s/run/media/usb0", g_tmp);
			CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
		}
	}
	CHECK(transfer_usb_drives(drv, 4) == 1);

	/* a second disk with no known filesystem */
	memset(junk, 0x33, sizeof(junk));
	fake_disk(sys, dev, "sdb", true, NULL, 1u << 20, junk);
	n = drain(ev, 8, 150);
	CHECK(n == 1 && ev[0].type == TRANSFER_USB_UNSUPPORTED && !strcmp(ev[0].drive.disk, "sdb"));

	/* eject, then no remount while it stays plugged */
	pathf(p, sizeof(p), "%s/usb0", media);
	CHECK(transfer_usb_eject(p) == 0);
	n = drain(ev, 8, 100);
	CHECK(n == 1 && ev[0].type == TRANSFER_USB_EJECTED);
	CHECK(transfer_usb_drives(drv, 4) == 0 && g_mounts == 1);
	CHECK(transfer_usb_eject(p) == -ENOENT);

	/* unplug and plug again: mounted again as usb0 */
	pathf(p, sizeof(p), "%s/sda", blk);
	unlink(p);
	drain(ev, 8, 40);
	fake_disk(sys, dev, "sda", true, "sda1", 1u << 22, bs);
	n = drain(ev, 8, 150);
	CHECK(n == 1 && ev[0].type == TRANSFER_USB_MOUNTED && ev[0].drive.index == 0);

	/* pulled while mounted */
	unlink(p);
	n = drain(ev, 8, 60);
	CHECK(n == 1 && ev[0].type == TRANSFER_USB_REMOVED && g_umounts >= 2);
	CHECK(transfer_usb_drives(drv, 4) == 0);

	/* review: a mount that takes 400 ms (a slow ntfs3, a failing stick)
	 * never blocks transfer_usb_poll(): the main loop pets the watchdog */
	{
		int64_t t0, worst = 0, got_at = -1;
		int mounts = g_mounts, umounts;

		g_mount_delay_ms = 400;
		g_mount_started = 0;
		fake_disk(sys, dev, "sdc", true, "sdc1", 1u << 22, bs);
		t0 = tr_now_ms();
		n = 0;
		while (tr_now_ms() - t0 < 1500 && n == 0) {
			struct transfer_usb_event e;
			int64_t a = tr_now_ms();
			int k = transfer_usb_poll(&e);
			int64_t d = tr_now_ms() - a;

			if (d > worst)
				worst = d;
			if (k > 0) {
				ev[n++] = e;
				got_at = tr_now_ms() - t0;
			}
			usleep(5000);
		}
		CHECK(g_mount_started && worst < 100);
		if (worst >= 100)
			printf("  a poll took %lld ms during the mount\n", (long long)worst);
		CHECK(n == 1 && ev[0].type == TRANSFER_USB_MOUNTED && got_at >= 400 && g_mounts == mounts + 1);
		CHECK(transfer_usb_drives(drv, 4) == 1);
		pathf(p, sizeof(p), "%s/sdc", blk);
		unlink(p);
		n = drain(ev, 8, 60);
		CHECK(n == 1 && ev[0].type == TRANSFER_USB_REMOVED);

		/* pulled during the mount: no MOUNTED, the new mount undone */
		g_mount_started = 0;
		fake_disk(sys, dev, "sdd", true, "sdd1", 1u << 22, bs);
		t0 = tr_now_ms();
		while (!g_mount_started && tr_now_ms() - t0 < 1000)
			drain(ev, 8, 5);
		CHECK(g_mount_started);
		umounts = g_umounts;
		pathf(p, sizeof(p), "%s/sdd", blk);
		unlink(p);
		n = drain(ev, 8, 700);
		CHECK(n == 0 && transfer_usb_drives(drv, 4) == 0 && g_umounts == umounts + 1);
		g_mount_delay_ms = 0;
	}
	transfer_usb_shutdown();
}

/* ------------------------------------------------------------ names */

static size_t dns_query(uint8_t *q, uint16_t id, const char *name, uint16_t type, uint16_t cls)
{
	size_t o = 12;
	const char *p = name;

	memset(q, 0, 12);
	q[0] = (uint8_t)(id >> 8);
	q[1] = (uint8_t)id;
	q[5] = 1;
	while (*p) {
		size_t l = strcspn(p, ".");

		q[o++] = (uint8_t)l;
		memcpy(q + o, p, l);
		o += l;
		p += l;
		if (*p)
			p++;
	}
	q[o++] = 0;
	q[o++] = (uint8_t)(type >> 8);
	q[o++] = (uint8_t)type;
	q[o++] = (uint8_t)(cls >> 8);
	q[o++] = (uint8_t)cls;
	return o;
}

static void test_netnames(void)
{
	uint8_t q[512], out[512];
	uint32_t ip = htonl(0xc0a80117);      /* 192.168.1.23 */
	const uint8_t want_ip[4] = { 192, 168, 1, 23 };
	size_t ql;
	bool uc;
	int n;

	/* mDNS, normal multicast query */
	ql = dns_query(q, 0, "RetroStone.local", 1, 1);
	n = netnames_reply(NETNAMES_MDNS, q, ql, ip, "retrostone", true, out, sizeof(out), &uc);
	CHECK(n > 0 && !uc);
	if (n > 0) {
		CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0x84 && out[5] == 0 && out[7] == 1);
		CHECK(!memcmp(out + n - 4, want_ip, 4));
		CHECK(out[n - 12] == 0x80 && out[n - 11] == 0x01);  /* cache-flush IN */
	}
	/* QU bit: unicast reply */
	ql = dns_query(q, 0, "retrostone.local", 1, 0x8001);
	n = netnames_reply(NETNAMES_MDNS, q, ql, ip, "retrostone", true, out, sizeof(out), &uc);
	CHECK(n > 0 && uc);
	/* legacy unicast (source port != 5353): ID and question echoed */
	ql = dns_query(q, 0x1234, "retrostone.local", 1, 1);
	n = netnames_reply(NETNAMES_MDNS, q, ql, ip, "retrostone", false, out, sizeof(out), &uc);
	CHECK(n > 0 && uc && out[0] == 0x12 && out[1] == 0x34 && out[5] == 1 && out[7] == 1);
	/* ANY also answered; AAAA, other names and responses are not */
	ql = dns_query(q, 0, "retrostone.local", 255, 1);
	CHECK(netnames_reply(NETNAMES_MDNS, q, ql, ip, "retrostone", true, out, sizeof(out), &uc) > 0);
	ql = dns_query(q, 0, "retrostone.local", 28, 1);
	CHECK(netnames_reply(NETNAMES_MDNS, q, ql, ip, "retrostone", true, out, sizeof(out), &uc) == 0);
	ql = dns_query(q, 0, "other.local", 1, 1);
	CHECK(netnames_reply(NETNAMES_MDNS, q, ql, ip, "retrostone", true, out, sizeof(out), &uc) == 0);
	ql = dns_query(q, 0, "retrostone.local", 1, 1);
	q[2] = 0x84;
	CHECK(netnames_reply(NETNAMES_MDNS, q, ql, ip, "retrostone", true, out, sizeof(out), &uc) == 0);
	/* malformed: truncated, pointer loop */
	CHECK(netnames_reply(NETNAMES_MDNS, q, 5, ip, "retrostone", true, out, sizeof(out), &uc) < 0);
	memset(q, 0, 12);
	q[5] = 1;
	q[12] = 0xc0;
	q[13] = 12;                           /* points to itself */
	CHECK(netnames_reply(NETNAMES_MDNS, q, 18, ip, "retrostone", true, out, sizeof(out), &uc) < 0);
	/* a compressed second question pointing into the first */
	{
		size_t o = dns_query(q, 0, "x.local", 1, 1);

		q[5] = 2;
		q[o++] = 10;
		memcpy(q + o, "retrostone", 10);
		o += 10;
		q[o++] = 0xc0;
		q[o++] = 14;                  /* -> "local" of the first name */
		q[o++] = 0;
		q[o++] = 1;
		q[o++] = 0;
		q[o++] = 1;
		n = netnames_reply(NETNAMES_MDNS, q, o, ip, "retrostone", true, out, sizeof(out), &uc);
		CHECK(n > 0);
	}

	/* LLMNR */
	ql = dns_query(q, 0xbeef, "RETROSTONE", 1, 1);
	n = netnames_reply(NETNAMES_LLMNR, q, ql, ip, "retrostone", false, out, sizeof(out), &uc);
	CHECK(n > 0 && uc && out[0] == 0xbe && out[1] == 0xef && out[2] == 0x80 && out[5] == 1 &&
	      !memcmp(out + n - 4, want_ip, 4));
	ql = dns_query(q, 1, "retrostone.local", 1, 1);
	CHECK(netnames_reply(NETNAMES_LLMNR, q, ql, ip, "retrostone", false, out, sizeof(out), &uc) == 0);

	/* NBNS: "RETROSTONE<20>" broadcast name query */
	{
		uint8_t nq[50];
		const char *nm = "RETROSTONE";
		uint8_t raw[16];

		memset(nq, 0, sizeof(nq));
		nq[0] = 0x44;
		nq[1] = 0x55;
		nq[2] = 0x01;                     /* RD */
		nq[3] = 0x10;                     /* B */
		nq[5] = 1;
		nq[12] = 32;
		memset(raw, ' ', 15);
		memcpy(raw, nm, strlen(nm));
		raw[15] = 0x20;
		for (int i = 0; i < 16; i++) {
			nq[13 + 2 * i] = (uint8_t)('A' + (raw[i] >> 4));
			nq[14 + 2 * i] = (uint8_t)('A' + (raw[i] & 15));
		}
		nq[45] = 0;
		nq[47] = 0x20;
		nq[49] = 1;
		n = netnames_reply(NETNAMES_NBNS, nq, sizeof(nq), ip, "retrostone", false, out,
				   sizeof(out), &uc);
		CHECK(n == 62 && uc && out[0] == 0x44 && out[2] == 0x85 && out[7] == 1 &&
		      !memcmp(out + 58, want_ip, 4));
		/* another name */
		nq[13] = 'A';
		CHECK(netnames_reply(NETNAMES_NBNS, nq, sizeof(nq), ip, "retrostone", false, out,
				     sizeof(out), &uc) == 0);
	}
}

/* ------------------------------------------------------------ QR */

static int get_bits(const uint8_t *d, int *bit, int n)
{
	int v = 0;

	for (int i = 0; i < n; i++, (*bit)++)
		v = v << 1 | ((d[*bit >> 3] >> (7 - (*bit & 7))) & 1);
	return v;
}

/* Independent reader: rebuilds the function-module map from the spec
 * tables, reads the format word, unmasks, reads the codewords, checks the
 * Reed-Solomon blocks and decodes the byte-mode payload. */
static bool qr_read(const struct qr_code *q, char *text, size_t tn)
{
	static const int align[11][4] = {
		{ 0 }, { 0 }, { 6, 18 }, { 6, 22 }, { 6, 26 }, { 6, 30 }, { 6, 34 },
		{ 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 },
	};
	static const uint8_t ecl[11] = { 0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
	static const uint8_t nbk[11] = { 0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 };
	static uint8_t fn[QR_MAX_SIZE][QR_MAX_SIZE];
	uint8_t cw[400], dat[400];
	int s = q->size, v = q->version, na = v == 1 ? 0 : v < 7 ? 2 : 3, fmt = 0, mask = -1;
	int total, nd = 0, k = 0;

	memset(fn, 0, sizeof(fn));
	for (int y = 0; y < 9; y++)
		for (int x = 0; x < 9; x++) {
			fn[y][x] = 1;
			if (x < 8)
				fn[y][s - 8 + x] = 1;
			if (y < 8)
				fn[s - 8 + y][x] = 1;
		}
	for (int i = 0; i < s; i++)
		fn[6][i] = fn[i][6] = 1;
	fn[8][s - 8] = fn[s - 8][8] = 1;
	for (int i = 0; i < 8; i++)
		fn[8][s - 1 - i] = 1;              /* format copy 2 row part */
	for (int i = 0; i < 8; i++)
		fn[s - 1 - i][8] = 1;
	for (int i = 0; i < na; i++)
		for (int j = 0; j < na; j++) {
			int cx = align[v][i], cy = align[v][j];

			if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0))
				continue;
			for (int dy = -2; dy <= 2; dy++)
				for (int dx = -2; dx <= 2; dx++)
					fn[cy + dy][cx + dx] = 1;
		}
	if (v >= 7)
		for (int i = 0; i < 6; i++)
			for (int j = 0; j < 3; j++)
				fn[i][s - 11 + j] = fn[s - 11 + j][i] = 1;

	/* finder patterns: 7x7 dark ring / light / 3x3 dark center */
	for (int y = 0; y < 7; y++)
		for (int x = 0; x < 7; x++) {
			int d = abs(x - 3) > abs(y - 3) ? abs(x - 3) : abs(y - 3);
			int want = d != 2;

			if (q->m[y][x] != want || q->m[y][s - 7 + x] != want || q->m[s - 7 + y][x] != want)
				return false;
		}
	/* format word, copy 1: bits 0-5 at (8,0..5), 6 at (8,7), 7 at (8,8),
	 * 8 at (7,8), 9-14 at (5..0, 8) */
	for (int i = 0; i <= 5; i++)
		fmt |= q->m[i][8] << i;
	fmt |= q->m[7][8] << 6 | q->m[8][8] << 7 | q->m[8][7] << 8;
	for (int i = 9; i < 15; i++)
		fmt |= q->m[8][14 - i] << i;
	for (int m = 0; m < 8; m++)
		if (qr_format_bits(m) == fmt)
			mask = m;
	if (mask < 0 || mask != q->mask)
		return false;

	total = 0;
	for (int right = s - 1; right >= 1; right -= 2) {
		if (right == 6)
			right = 5;
		for (int vert = 0; vert < s; vert++)
			for (int j = 0; j < 2; j++) {
				int x = right - j, y = ((right + 1) & 2) == 0 ? s - 1 - vert : vert;
				int bit, inv;

				if (fn[y][x])
					continue;
				switch (mask) {
				case 0: inv = (x + y) % 2 == 0; break;
				case 1: inv = y % 2 == 0; break;
				case 2: inv = x % 3 == 0; break;
				case 3: inv = (x + y) % 3 == 0; break;
				case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
				case 5: inv = x * y % 2 + x * y % 3 == 0; break;
				case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
				default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
				}
				bit = q->m[y][x] ^ inv;
				if (total / 8 < (int)sizeof(cw)) {
					if (total % 8 == 0)
						cw[total / 8] = 0;
					cw[total / 8] |= (uint8_t)(bit << (7 - total % 8));
				}
				total++;
			}
	}
	/* de-interleave */
	{
		int raw = total / 8, nb = nbk[v], e = ecl[v], nshort = nb - raw % nb, sl = raw / nb;
		uint8_t blk[5][200];
		int dl[5], pos = 0;

		for (int b = 0; b < nb; b++)
			dl[b] = sl - e + (b < nshort ? 0 : 1);
		for (int i = 0; i < sl - e + 1; i++)
			for (int b = 0; b < nb; b++)
				if (i < dl[b])
					blk[b][i] = cw[pos++];
		for (int i = 0; i < e; i++)
			for (int b = 0; b < nb; b++)
				blk[b][dl[b] + i] = cw[pos++];
		for (int b = 0; b < nb; b++) {
			uint8_t rem[30];

			qr_rs_remainder(blk[b], dl[b], e, rem);
			if (memcmp(rem, blk[b] + dl[b], (size_t)e))
				return false;
			memcpy(dat + nd, blk[b], (size_t)dl[b]);
			nd += dl[b];
		}
	}
	/* byte mode */
	{
		int bit = 0, len, cc = v < 10 ? 8 : 16;

		if (get_bits(dat, &bit, 4) != 4)
			return false;
		len = get_bits(dat, &bit, cc);
		if ((size_t)len >= tn)
			return false;
		for (k = 0; k < len; k++)
			text[k] = (char)get_bits(dat, &bit, 8);
		text[k] = 0;
	}
	return true;
}

static void test_qr(void)
{
	/* ISO 18004 / thonky.com worked example: "HELLO WORLD", 1-M */
	const uint8_t data[16] = { 32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17 };
	const uint8_t want[10] = { 196, 35, 39, 119, 235, 215, 231, 226, 93, 23 };
	uint8_t ecc[10];
	struct qr_code q;
	char back[256];
	const char *texts[] = {
		"http://192.168.1.23/#pin=123456",
		"http://retrostone.local/",
		"http://192.168.100.200:8080/#pin=654321 and a bit more text to reach version five!",
		/* 153 bytes: version 9, blocks of two lengths + version info */
		"http://192.168.100.200/#pin=000000&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	};

	qr_rs_remainder(data, 16, 10, ecc);
	CHECK(!memcmp(ecc, want, 10));
	/* published format words for level M */
	CHECK(qr_format_bits(0) == 0x5412 /* 101010000010010 */);
	CHECK(qr_format_bits(1) == 0x5125 /* 101000100100101 */);
	CHECK(qr_format_bits(7) == 0x4aa0 /* 100101010100000 */);

	for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		int r = qr_encode(texts[i], &q);
		bool ok = r == 0 && qr_read(&q, back, sizeof(back)) && !strcmp(back, texts[i]);

		printf("  qr \"%.30s...\" (%zu B): version %d size %d mask %d -> %s\n", texts[i],
		       strlen(texts[i]), q.version, q.size, q.mask, ok ? "decoded OK" : "FAILED");
		CHECK(ok);
	}
	CHECK(qr_encode("http://192.168.1.23/#pin=123456", &q) == 0 && q.version == 3);
	{
		char big[300];

		memset(big, 'a', 250);
		big[250] = 0;
		CHECK(qr_encode(big, &q) == -EMSGSIZE);
	}
}

/* start -> idle stop -> start again -> stop (no leaked fds) */
/* A TCP connection to the share from source address src (127.0.0.x). */
static int ws_connect(const char *src, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET };
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct timeval tv = { .tv_sec = 5 };

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	inet_pton(AF_INET, src, &a.sin_addr);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	a.sin_port = htons((uint16_t)port);
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void ws_send(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t w = send(fd, s, n, MSG_NOSIGNAL);

		if (w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}

/* Reads the whole answer (the server closes); returns the status code. */
static int ws_answer(int fd, char *out, size_t n)
{
	size_t got = 0;
	ssize_t r;
	int code = 0;

	while (got + 1 < n && (r = recv(fd, out + got, n - got - 1, 0)) > 0)
		got += (size_t)r;
	out[got] = 0;
	close(fd);
	sscanf(out, "HTTP/1.1 %d", &code);
	return code;
}

static int ws_login(const char *src, int port, const char *pin, char *token)
{
	char req[256], ans[2048], *t;
	int fd = ws_connect(src, port), code;

	if (fd < 0)
		return -1;
	snprintf(req, sizeof(req), "POST /api/login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10\r\n\r\npin=%s",
		 pin);
	ws_send(fd, req, strlen(req));
	code = ws_answer(fd, ans, sizeof(ans));
	if (token && code == 200 && (t = strstr(ans, "rsos_token=")))
		snprintf(token, 33, "%.32s", t + 11);
	return code;
}

/* Starts an upload of `len` bytes to roms/snes/<path>, sends `first` bytes
 * of `fill`. Returns the connection. */
static int ws_upload_start(int port, const char *token, const char *path, int overwrite, size_t len,
			   char fill, size_t first)
{
	char req[512], body[4096];
	int fd = ws_connect("127.0.0.1", port);

	if (fd < 0)
		return -1;
	snprintf(req, sizeof(req),
		 "PUT /api/upload?target=roms&sys=snes&path=%s&overwrite=%d HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		 "X-Requested-With: rsos\r\nX-RSOS-Token: %s\r\nContent-Length: %zu\r\n\r\n",
		 path, overwrite, token, len);
	ws_send(fd, req, strlen(req));
	memset(body, fill, sizeof(body));
	while (first) {
		size_t n = first > sizeof(body) ? sizeof(body) : first;

		ws_send(fd, body, n);
		first -= n;
	}
	return fd;
}

/*
 * Review F-H1 (two uploads of one path mixed into one file, reported as
 * success) and F-L2 (one LAN device could lock everybody out), on the
 * running share.
 */
static void test_webshare_races(const char *root, int port)
{
	char token[40] = "", ans[2048], p[1024];
	int a, b, code_a, code_b, nonA = 0;
	FILE *f;

	CHECK(ws_login("127.0.0.1", port, "123456", token) == 200 && strlen(token) == 32);
	/* A is in the middle of its body when B asks for the same path */
	a = ws_upload_start(port, token, "Race.sfc", 1, 300000, 'A', 100000);
	CHECK(a >= 0);
	usleep(200000);
	b = ws_upload_start(port, token, "race.SFC", 1, 300000, 'B', 300000);   /* exFAT: same file */
	code_b = ws_answer(b, ans, sizeof(ans));
	CHECK(code_b == 409 && strstr(ans, "\"busy\""));
	{
		char body[4096];

		memset(body, 'A', sizeof(body));
		for (size_t left = 200000; left;) {
			size_t n = left > sizeof(body) ? sizeof(body) : left;

			ws_send(a, body, n);
			left -= n;
		}
	}
	code_a = ws_answer(a, ans, sizeof(ans));
	CHECK(code_a == 201);
	pathf(p, sizeof(p), "%s/roms/snes/Race.sfc", root);
	f = fopen(p, "rb");
	CHECK(f != NULL);
	if (f) {
		int ch;
		long n = 0;

		while ((ch = fgetc(f)) != EOF) {
			nonA += ch != 'A';
			n++;
		}
		fclose(f);
		CHECK(n == 300000 && nonA == 0);
	}
	/* the path is free again: a later upload of it works */
	b = ws_upload_start(port, token, "Race.sfc", 1, 10, 'B', 10);
	CHECK(ws_answer(b, ans, sizeof(ans)) == 201);
	/* overwrite=0 on an existing file: 409 exists, the file is kept */
	b = ws_upload_start(port, token, "Race.sfc", 0, 10, 'C', 10);
	CHECK(ws_answer(b, ans, sizeof(ans)) == 409 && strstr(ans, "\"exists\""));
	pathf(p, sizeof(p), "%s/roms/snes", root);
	CHECK(count_parts(p) == 0);

	/* lockout per device: 127.0.0.2 locks itself, 127.0.0.1 still logs in */
	for (int i = 0; i < 5; i++)
		CHECK(ws_login("127.0.0.2", port, "000000", NULL) == 403);
	CHECK(ws_login("127.0.0.2", port, "123456", NULL) == 429);
	CHECK(ws_login("127.0.0.1", port, "123456", NULL) == 200);
}

static void test_webshare_lifecycle(void)
{
	struct webshare_config c = { 0 };
	struct webshare_status st;
	char root[512], ch[4][TRANSFER_SYSID_MAX];
	int fd_before, fd_after, spins = 0;

	pathf(root, sizeof(root), "%s/wsroot", g_tmp);
	mkdirs(root);
	c.data_root = root;
	c.port = 18099;
	c.idle_timeout_s = 1;
	fd_before = dup(0);
	close(fd_before);
	CHECK(webshare_start(&c) == 0);
	CHECK(webshare_start(&c) == -EALREADY);
	webshare_get_status(&st);
	CHECK(st.running && strlen(st.pin) == 6 && st.port == 18099);
	while (webshare_running() && spins++ < 400)
		usleep(10000);
	webshare_get_status(&st);
	CHECK(!webshare_running() && !strcmp(st.stop_reason, "idle"));
	c.pin = "123456";
	c.idle_timeout_s = 0;
	CHECK(webshare_start(&c) == 0);
	webshare_get_status(&st);
	CHECK(st.running && !strcmp(st.pin, "123456") && st.stop_reason[0] == 0);
	test_webshare_races(root, c.port);
	webshare_stop();
	CHECK(!webshare_running());
	webshare_stop();                      /* harmless twice */
	webshare_take_changes(ch, 4);
	CHECK(webshare_take_changes(ch, 4) == 0);
	fd_after = dup(0);
	close(fd_after);
	CHECK(fd_after == fd_before);
}

/* ------------------------------------------------------------ copy engine */

static int g_ticks;
static volatile int g_cancel_copy;

static void cancel_after_3mb(void *arg)
{
	struct tr_sink *s = arg;

	g_ticks++;
	if (s->written >= (3u << 20))
		g_cancel_copy = 1;
}

/* One file through tr_copy_file(): 0 or -errno; *out_written. */
static int engine_copy(struct tr_copier *c, const char *src, const char *dstdir, const char *name,
		       uint64_t size_hint, bool cancel_mid, uint64_t *out_written)
{
	struct tr_sink s;
	uint64_t prog = 0;
	int sfd = open(src, O_RDONLY), dfd = open(dstdir, O_RDONLY | O_DIRECTORY), e;

	if (sfd < 0 || dfd < 0)
		return -errno;
	g_cancel_copy = 0;
	e = tr_sink_open(&s, dfd, name);
	if (e == 0)
		e = tr_copy_file(c, sfd, size_hint, &s, &prog, &g_cancel_copy, cancel_mid ? cancel_after_3mb : NULL,
				 &s);
	if (out_written)
		*out_written = s.written;
	if (e == 0 && prog != s.written)
		e = -EPROTO;
	if (e == 0)
		e = tr_copier_commit(c, &s, 1500000000, 0, false);
	else
		tr_sink_abort(&s);
	close(sfd);
	close(dfd);
	return e;
}

static void test_copy_engine(void)
{
	char src[1024], dst[1024], a[1024], b[1024];
	struct tr_copier c;
	struct tr_group g;
	struct stat st;
	uint64_t w = 0;
	size_t big = (13u << 20) + 7;          /* 13 blocks and a short one */

	pathf(src, sizeof(src), "%s/engine/src", g_tmp);
	pathf(dst, sizeof(dst), "%s/engine/dst", g_tmp);
	mkdirs(src);
	mkdirs(dst);
	put_file(src, "big.bin", big, 3, -1);
	put_file(src, "exact.bin", 4u << 20, 4, -1);          /* whole blocks: EOF on an empty read */
	put_file(src, "small.bin", 100000, 5, -1);
	put_file(src, "empty.bin", 0, 6, -1);
	CHECK(tr_copier_init(&c) == 0);

	/* pipelined (reader thread), with the size as hint and a larger one */
	pathf(a, sizeof(a), "%s/big.bin", src);
	CHECK(engine_copy(&c, a, dst, "big.bin", big, false, &w) == 0 && w == big);
	pathf(b, sizeof(b), "%s/big.bin", dst);
	CHECK(same_files(a, b) && stat(b, &st) == 0 && (size_t)st.st_size == big && st.st_mtime == 1500000000);
	CHECK(engine_copy(&c, a, dst, "big2.bin", big + (5u << 20), false, &w) == 0);
	pathf(b, sizeof(b), "%s/big2.bin", dst);
	CHECK(same_files(a, b) && stat(b, &st) == 0 && (size_t)st.st_size == big);   /* reserve trimmed */
	pathf(a, sizeof(a), "%s/exact.bin", src);
	CHECK(engine_copy(&c, a, dst, "exact.bin", 4u << 20, false, &w) == 0 && w == (4u << 20));
	pathf(b, sizeof(b), "%s/exact.bin", dst);
	CHECK(same_files(a, b));
	/* one block or less: no thread */
	pathf(a, sizeof(a), "%s/small.bin", src);
	CHECK(engine_copy(&c, a, dst, "small.bin", 100000, false, &w) == 0 && w == 100000);
	pathf(b, sizeof(b), "%s/small.bin", dst);
	CHECK(same_files(a, b));
	pathf(a, sizeof(a), "%s/empty.bin", src);
	CHECK(engine_copy(&c, a, dst, "empty.bin", 0, false, &w) == 0 && w == 0);
	CHECK(exists(dst, "empty.bin"));
	CHECK(c.files == 5 && c.bytes == 2 * big + (4u << 20) + 100000);
	/* the folder fsync waits for the last file of the folder */
	CHECK(c.dirfd >= 0 && tr_copier_sync_dir(&c) == 0 && c.dirfd == -1);
	/* a source that grew since the plan (hint too small): all of it */
	pathf(a, sizeof(a), "%s/big.bin", src);
	CHECK(engine_copy(&c, a, dst, "grew.bin", 2u << 20, false, &w) == 0 && w == big);
	/* cancel in the middle: -ECANCELED, nothing left */
	g_ticks = 0;
	CHECK(engine_copy(&c, a, dst, "cancel.bin", big, true, &w) == -ECANCELED && w >= (3u << 20) && w < big);
	CHECK(!exists(dst, "cancel.bin") && count_parts(dst) == 0 && g_ticks >= 3);
	/* a read error in the reader thread reaches the writer */
	CHECK(engine_copy(&c, src, dst, "dir.bin", 5u << 20, false, &w) == -EISDIR);
	CHECK(count_parts(dst) == 0);
	tr_copier_free(&c);

	/* the per-folder throughput groups */
	memset(&g, 0, sizeof(g));
	CHECK(tr_copier_init(&c) == 0);
	tr_group_step(&g, &c, "import", "roms/snes/A.sfc");
	CHECK(!strcmp(g.key, "roms/snes"));
	tr_group_step(&g, &c, "import", "roms/snes/sub/B.sfc");
	CHECK(!strcmp(g.key, "roms/snes"));
	tr_group_step(&g, &c, "import", "bios/scph5501.bin");
	CHECK(!strcmp(g.key, "bios"));
	tr_group_step(&g, &c, "import", "README.txt");
	CHECK(!strcmp(g.key, "README.txt"));
	tr_group_end(&g, &c);
	CHECK(!g.key[0]);
	tr_copier_free(&c);
}

int main(void)
{
	snprintf(g_tmp, sizeof(g_tmp), "/tmp/rsos-transfer-test-%d", (int)getpid());
	mkdirs(g_tmp);

	test_names();
	test_sysmap();
	test_dirs_and_sink();
	test_copy_engine();
	test_probe();
	test_import();
	test_import_stick_safety();
	test_trees();
	test_duplicates();
	test_backup();
	test_usb();
	test_netnames();
	test_qr();
	test_webshare_lifecycle();

	{
		char cmd[400];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp);
		if (system(cmd) != 0)
			fprintf(stderr, "cleanup failed\n");
	}
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
