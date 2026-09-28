/*
 * test_host.c - unit tests for the parts of the host that can be checked
 * without hardware: the resampler (frequency, level, noise), the pacing
 * policy and a long DRC simulation against a model audio device, the INI
 * parser, MD5, the core options layering and the BIOS check.
 *
 * Build and run: make -f host.mk host-check
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../audio/resampler.h"
#include "../batt_overlay.h"
#include "../bench.h"
#include "../coreinfo.h"
#include "../hutil.h"
#include "../ini.h"
#include "../md5.h"
#include "../options.h"
#include "../pacing.h"
#include "../perf.h"
#include "../playtime.h"

/* A literal's length is never typed by hand (review F-L25: 66 for 65 bytes). */
#define HWRITE_LIT(path, lit, bak) hwrite_atomic((path), (lit), sizeof(lit) - 1, (bak))

static int failures;

#define CHECK(cond, ...)                                         \
	do {                                                     \
		if (cond) {                                      \
			printf("  ok    ");                      \
		} else {                                         \
			printf("  FAIL  ");                      \
			failures++;                              \
		}                                                \
		printf(__VA_ARGS__);                             \
		printf("\n");                                    \
	} while (0)

/* ------------------------------------------------------------ resampler */

/* Feeds a sine of f Hz at in_rate in chunks, returns SNR (dB) of the
 * output against the ideal sine at 48 kHz, and the measured frequency. */
static double sine_test(enum resampler_quality q, double in_rate, double f, double *meas_f,
			double *out_ratio)
{
	const int total_in = (int)(in_rate * 2); /* 2 s */
	struct resampler *r = resampler_new(q, 4096);
	double ratio = 48000.0 / in_rate;
	int16_t *in = malloc((size_t)total_in * 4);
	int16_t *out = malloc((size_t)(total_in * ratio + 64) * 4);
	int produced = 0, pos = 0, crossings = 0, first = -1, last = -1;
	double sig = 0, err = 0, best_phase = 0;

	for (int i = 0; i < total_in; i++) {
		int16_t v = (int16_t)lrint(16000 * sin(2 * M_PI * f * i / in_rate));

		in[2 * i] = v;
		in[2 * i + 1] = v;
	}
	while (pos < total_in) {
		int n = total_in - pos < 700 ? total_in - pos : 700; /* ~a frame of audio */

		produced += resampler_run(r, in + 2 * pos, n, out + 2 * produced,
					  (int)(total_in * ratio + 64) - produced, ratio);
		pos += n;
	}
	*out_ratio = (double)produced / total_in;
	/* Frequency from zero crossings (skip the first 0.1 s). */
	for (int i = 4800; i < produced - 1; i++)
		if (out[2 * i] < 0 && out[2 * i + 2] >= 0) {
			if (first < 0)
				first = i;
			last = i;
			crossings++;
		}
	*meas_f = crossings > 1 ? (crossings - 1) * 48000.0 / (last - first) : 0;
	/* SNR: best-fit phase of an ideal sine (the filter delay is unknown). */
	{
		double best = 1e300;

		for (int k = 0; k < 400; k++) {
			double ph = k * 2 * M_PI / 400, e = 0;

			for (int i = 4800; i < 4800 + 2400; i++) {
				double ideal = 16000 * sin(2 * M_PI * f * i / 48000.0 + ph);
				double d = out[2 * i] - ideal;

				e += d * d;
			}
			if (e < best) {
				best = e;
				best_phase = ph;
			}
		}
	}
	for (int i = 4800; i < produced - 4800; i++) {
		double ideal = 16000 * sin(2 * M_PI * f * i / 48000.0 + best_phase);
		double d = out[2 * i] - ideal;

		sig += ideal * ideal;
		err += d * d;
	}
	resampler_free(r);
	free(in);
	free(out);
	return 10 * log10(sig / (err > 0 ? err : 1e-9));
}

static void test_resampler(void)
{
	double mf, rr, snr;

	printf("resampler\n");
	snr = sine_test(RESAMPLER_SINC, 32040.5, 1000, &mf, &rr);
	CHECK(fabs(mf - 1000) < 1 && fabs(rr - 48000 / 32040.5) < 0.001 && snr > 60,
	      "sinc 32040.5 -> 48000, 1 kHz: %.2f Hz, ratio %.5f, SNR %.1f dB", mf, rr, snr);
	snr = sine_test(RESAMPLER_SINC, 44100, 5000, &mf, &rr);
	CHECK(fabs(mf - 5000) < 2 && snr > 55, "sinc 44100 -> 48000, 5 kHz: %.2f Hz, SNR %.1f dB", mf, snr);
	snr = sine_test(RESAMPLER_SINC, 65536, 3000, &mf, &rr);
	CHECK(fabs(mf - 3000) < 2 && snr > 55, "sinc 65536 -> 48000 (downsampling), 3 kHz: %.2f Hz, SNR %.1f dB", mf, snr);
	snr = sine_test(RESAMPLER_LINEAR, 32040.5, 1000, &mf, &rr);
	CHECK(fabs(mf - 1000) < 1 && snr > 30, "linear 32040.5 -> 48000, 1 kHz: %.2f Hz, SNR %.1f dB", mf, snr);
	snr = sine_test(RESAMPLER_SINC, 48000, 1000, &mf, &rr);
	CHECK(fabs(rr - 1) < 0.001 && snr > 60, "sinc 48000 -> 48000 (pass-through): SNR %.1f dB", snr);

	/* Review F-M1: a position fraction just below 1 rounds to 1.0f, the
	 * phase becomes PHASES and the next coefficient row is read past the
	 * table (ASan: heap-buffer-overflow). A step of 1 - 2^-30 makes the
	 * first ~30 fractions round up. DC in, DC out. */
	{
		struct resampler *r = resampler_new(RESAMPLER_SINC, 4096);
		static int16_t in[2 * 2048], out[2 * 4096];
		double ratio = 1.0 / (1.0 - ldexp(1.0, -30));
		int n = 0, bad = 0;

		for (int i = 0; i < 2048; i++)
			in[2 * i] = in[2 * i + 1] = 16384;
		/* history full of DC at ratio 1 (integer positions), then the
		 * nearly-1 step from an integer position */
		resampler_run(r, in, 2048, out, 4096, 1.0);
		n = resampler_run(r, in, 2048, out, 4096, ratio);
		for (int i = 0; i < n; i++)
			if (abs(out[2 * i] - 16384) > 200 || abs(out[2 * i + 1] - 16384) > 200)
				bad++;
		CHECK(n > 0 && !bad, "fraction rounding to 1.0f: %d frames, %d off the DC level", n, bad);
		resampler_free(r);
	}
}

/* --------------------------------------------------------------- pacing */

static void test_policy(void)
{
	struct pacing p;

	printf("pacing policy\n");
	pacing_setup(&p, 60.0988, 32040.5, 60.0, 48000, true, true, 0.01);
	CHECK(p.mode == PACE_VSYNC, "NES/SNES 60.0988 fps on HDMI 60 Hz -> %s", pacing_mode_name(p.mode));
	pacing_setup(&p, 60.0988, 32040.5, 78.6, 48000, true, true, 0.01);
	CHECK(p.mode == PACE_AUDIO, "60.0988 fps on the 78.6 Hz LCD -> %s", pacing_mode_name(p.mode));
	pacing_setup(&p, 59.7275, 32768, 60.0, 48000, true, true, 0.01);
	CHECK(p.mode == PACE_VSYNC, "GBA 59.7275 fps on 60 Hz -> %s", pacing_mode_name(p.mode));
	pacing_setup(&p, 50.0, 44100, 60.0, 48000, true, true, 0.01);
	CHECK(p.mode == PACE_AUDIO, "PAL 50 fps on 60 Hz -> %s", pacing_mode_name(p.mode));
	pacing_setup(&p, 54.7, 44100, 60.0, 48000, true, false, 0.01);
	CHECK(p.mode == PACE_TIMER, "54.7 fps, no audio -> %s", pacing_mode_name(p.mode));
	pacing_setup(&p, 60, 44100, 0, 48000, false, false, 0.01);
	CHECK(p.mode == PACE_FREE, "headless -> %s", pacing_mode_name(p.mode));
	pacing_setup(&p, 60.0988, 32040.5, 60.0, 48000, true, true, 0.01);
	CHECK(fabs(pacing_ratio(&p, 0.0) / p.base_ratio - 1.005) < 1e-9 &&
	      fabs(pacing_ratio(&p, 1.0) / p.base_ratio - 0.995) < 1e-9 &&
	      fabs(pacing_ratio(&p, 0.5) / p.base_ratio - 1.0) < 1e-9,
	      "DRC clamps at +/-0.5%% (empty %.5f, half %.5f, full %.5f x base)",
	      pacing_ratio(&p, 0.0) / p.base_ratio, pacing_ratio(&p, 0.5) / p.base_ratio,
	      pacing_ratio(&p, 1.0) / p.base_ratio);
}

/*
 * DRC simulation: vsync-locked loop at disp_hz (one retro_run per vblank),
 * the core produces core_rate / core_fps frames per run, the real resampler
 * converts them with pacing_ratio(fill), and a model DAC drains the buffer
 * at 48 kHz * (1 + dac_ppm). The buffer must never under/overflow after the
 * prefill, and the fill must settle near half.
 */
static void drc_sim(const char *name, double core_fps, double core_rate, double disp_hz, double dac_ppm,
		    double seconds)
{
	struct pacing p;
	struct resampler *r = resampler_new(RESAMPLER_SINC, 4096);
	const int buffer = 3072; /* 64 ms */
	double fill = buffer / 2.0, acc_in = 0, dac_acc = 0;
	double min_fill = 1e9, max_fill = 0, rmin = 1e9, rmax = 0, fill_end = 0;
	int16_t in[4096 * 2], out[8192 * 2];
	int under = 0, over = 0;
	long vblanks = (long)(seconds * disp_hz);

	pacing_setup(&p, core_fps, core_rate, disp_hz, 48000, true, true, 0.01);
	memset(in, 0, sizeof(in));
	for (long v = 0; v < vblanks; v++) {
		int n_in, n_out;
		double ratio, drain;

		/* core: one frame of samples (fractional accumulation) */
		acc_in += core_rate / core_fps;
		n_in = (int)acc_in;
		acc_in -= n_in;
		ratio = pacing_ratio(&p, fill / buffer);
		if (ratio < rmin)
			rmin = ratio;
		if (ratio > rmax)
			rmax = ratio;
		n_out = resampler_run(r, in, n_in, out, 8192, ratio);
		fill += n_out;
		if (fill > buffer) {
			over++;
			fill = buffer;
		}
		/* DAC drains during one refresh period */
		dac_acc += 48000.0 * (1 + dac_ppm * 1e-6) / disp_hz;
		drain = (int)dac_acc;
		dac_acc -= drain;
		fill -= drain;
		if (fill < 0) {
			under++;
			fill = 0;
		}
		if (v > disp_hz * 10) { /* after 10 s of settling */
			if (fill < min_fill)
				min_fill = fill;
			if (fill > max_fill)
				max_fill = fill;
		}
		fill_end = fill;
	}
	resampler_free(r);
	CHECK(!under && !over && min_fill > buffer * 0.2 && max_fill < buffer * 0.8,
	      "%s: %.0f s, underruns %d, overflows %d, fill %.0f..%.0f (end %.0f of %d), ratio x%.5f..x%.5f of nominal",
	      name, seconds, under, over, min_fill, max_fill, fill_end, buffer, rmin * core_rate / 48000,
	      rmax * core_rate / 48000);
}

/*
 * Fast-forward (Select+R2) in the same model, as the host does it
 * (docs/host-design.md §10.1): from ff_on to ff_off seconds each vblank runs
 * the core `speed` times; the samples of the extra runs are dropped and
 * those of the shown run go through the resampler at the DRC ratio (then
 * muted: silence has the same length). The buffer must neither underrun nor
 * overflow while it runs, nor after (no resync step needed), and the fill
 * must stay near half.
 */
static void drc_sim_ff(const char *name, double core_fps, double core_rate, double disp_hz, int speed,
		       double ff_on, double ff_off, double seconds)
{
	struct pacing p;
	struct resampler *r = resampler_new(RESAMPLER_SINC, 4096);
	const int buffer = 3072;
	double fill = buffer / 2.0, acc_in = 0, dac_acc = 0, min_fill = 1e9, max_fill = 0;
	int16_t in[4096 * 2], out[8192 * 2];
	int under = 0, over = 0;
	long vblanks = (long)(seconds * disp_hz), runs = 0;

	pacing_setup(&p, core_fps, core_rate, disp_hz, 48000, true, true, 0.01);
	memset(in, 0, sizeof(in));
	for (long v = 0; v < vblanks; v++) {
		double t = v / disp_hz, drain;
		int n = t >= ff_on && t < ff_off ? speed : 1, n_in = 0, n_out;

		for (int i = 0; i < n; i++) {
			acc_in += core_rate / core_fps;
			n_in = (int)acc_in;
			acc_in -= n_in;
			runs++;
			/* the extra runs' samples are dropped (audio_process) */
		}
		n_out = resampler_run(r, in, n_in, out, 8192, pacing_ratio(&p, fill / buffer));
		fill += n_out;
		if (fill > buffer) {
			over++;
			fill = buffer;
		}
		dac_acc += 48000.0 / disp_hz;
		drain = (int)dac_acc;
		dac_acc -= drain;
		fill -= drain;
		if (fill < 0) {
			under++;
			fill = 0;
		}
		if (t > 5) {
			if (fill < min_fill)
				min_fill = fill;
			if (fill > max_fill)
				max_fill = fill;
		}
	}
	resampler_free(r);
	CHECK(!under && !over && min_fill > buffer * 0.2 && max_fill < buffer * 0.8 &&
	      runs > (long)(seconds * core_fps * (1 + (speed - 1) * (ff_off - ff_on) / seconds) * 0.97),
	      "%s: x%d from %.0f to %.0f s of %.0f: %ld runs, underruns %d, overflows %d, fill %.0f..%.0f",
	      name, speed, ff_on, ff_off, seconds, runs, under, over, min_fill, max_fill);
}

/* The play-time clock of the game process (playtime.c): the menu, the
 * switcher and pauses do not count, pauses nest. */
static void test_playtime(void)
{
	struct playtime pt;

	printf("play time\n");
	memset(&pt, 0, sizeof(pt));
	CHECK(pt_total_ms(&pt, 5000) == 0, "not started: 0");
	pt_start(&pt, 1000);
	CHECK(pt_total_ms(&pt, 61000) == 60000 && pt_running(&pt), "one minute of play");
	pt_pause(&pt, 61000);                   /* the in-game menu */
	CHECK(pt_total_ms(&pt, 121000) == 60000 && !pt_running(&pt), "a minute in the menu is not counted");
	pt_pause(&pt, 90000);                   /* the controller unplugged meanwhile */
	pt_resume(&pt, 100000);                 /* plugged again: still in the menu */
	CHECK(pt_total_ms(&pt, 110000) == 60000, "pauses nest: still paused");
	pt_resume(&pt, 121000);                 /* the menu closes */
	CHECK(pt_total_ms(&pt, 151000) == 90000, "running again: 90 s");
	pt_resume(&pt, 151000);                 /* one resume too many: harmless */
	CHECK(pt_total_ms(&pt, 151000) == 90000 && pt_running(&pt), "an extra resume changes nothing");
	pt_pause(&pt, 200000);
	CHECK(pt_total_ms(&pt, 999999) == 139000, "at exit: 139 s");
}

static void test_drc(void)
{
	printf("dynamic rate control (simulated)\n");
	drc_sim_ff("SNES on HDMI 60, fast-forward", 60.0988, 32040.5, 60.0, 3, 20, 80, 140);
	drc_sim_ff("GBA on HDMI 60, fast-forward", 59.7275, 32768, 60.0, 4, 10, 40, 90);
	drc_sim("SNES 60.0988 fps / 32040.5 Hz on HDMI 60.000", 60.0988, 32040.5, 60.0, 0, 600);
	drc_sim("NES  60.0988 fps / 48000 Hz on HDMI 59.940", 60.0988, 48000, 59.94, 0, 600);
	drc_sim("GBA  59.7275 fps / 32768 Hz on HDMI 60.000", 59.7275, 32768, 60.0, 0, 600);
	drc_sim("PS1  59.94 fps / 44100 Hz, DAC +300 ppm", 59.94, 44100, 60.0, 300, 600);
	drc_sim("MD   59.92 fps / 44100 Hz, DAC -300 ppm", 59.92, 44100, 60.0, -300, 600);
}

/* ------------------------------------------------------------ ini / md5 */

static void test_ini_md5(void)
{
	struct ini ini = { 0 };
	char hex[33];
	char path[256];
	int fd;

	printf("ini, md5\n");
	ini_parse(&ini, "; comment\n[core]\nid = fceumm   ; trailing\nneed_fullpath = true\n"
			"[bios:disksys.rom]\nmd5 = CA30B50F880EB660A320674ED365EF7A\n"
			"[options]\nfoo = \"a b ; c\"\n");
	CHECK(!strcmp(ini_get(&ini, "core", "id"), "fceumm"), "inline comment stripped");
	CHECK(ini_get_bool(&ini, "core", "need_fullpath", false), "bool");
	CHECK(!strcmp(ini_get(&ini, "options", "foo"), "a b ; c"), "quoted value kept verbatim");
	ini_free(&ini);

	snprintf(path, sizeof(path), "/tmp/rsos-host-test-%d.bin", (int)getpid());
	fd = HWRITE_LIT(path, "abc", false);
	CHECK(fd == 0 && md5_file(path, hex) == 0 && !strcmp(hex, "900150983cd24fb0d6963f7d28e17f72"),
	      "md5(\"abc\") = %s", hex);
	CHECK(HWRITE_LIT(path, "xyz", true) == 0 && hfile_size(path) == 3, "atomic rewrite with .bak");
	{
		char bak[300];

		snprintf(bak, sizeof(bak), "%s.bak", path);
		CHECK(md5_file(bak, hex) == 0 && !strcmp(hex, "900150983cd24fb0d6963f7d28e17f72"),
		      ".bak holds the previous version");
		unlink(bak);
	}
	unlink(path);
}

/* -------------------------------------------------------------- options */

static void test_options(void)
{
	char dir[128], p[256];
	struct ini core_ini = { 0 };
	struct opts_paths op;
	struct retro_variable var;
	static const struct retro_variable vars[] = {
		{ "t_mode", "Mode; fast|accurate|slow" },
		{ "t_pad", "Pad; 3button|6button" },
		{ "t_skip", "Frameskip; off|auto|1|2" },
		{ NULL, NULL },
	};
	bool upd;

	printf("core options\n");
	snprintf(dir, sizeof(dir), "/tmp/rsos-host-test-opts-%d", (int)getpid());
	snprintf(p, sizeof(p), "%s/ship", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/ship/tcore.ini", dir);
	{
		const char *s = "t_pad = \"6button\"\nt_skip = \"bogus\"\n";

		hwrite_atomic(p, s, strlen(s), false);
	}
	snprintf(p, sizeof(p), "%s/user", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/user/tcore.ini", dir);
	{
		const char *s = "t_mode = \"slow\"\n";

		hwrite_atomic(p, s, strlen(s), false);
	}
	ini_parse(&core_ini, "[options]\nt_mode = accurate\nt_skip = auto\n");

	snprintf(p, sizeof(p), "%s/ship", dir);
	op.ship_dir = strdup(p);
	snprintf(p, sizeof(p), "%s/user", dir);
	op.user_dir = strdup(p);
	opts_init("tcore", "Game (USA)", &core_ini, &op);
	opts_env_set_variables(vars);
	var.key = "t_mode";
	CHECK(opts_env_get_variable(&var) && !strcmp(var.value, "slow"), "user system file wins: t_mode=%s", var.value);
	var.key = "t_pad";
	CHECK(opts_env_get_variable(&var) && !strcmp(var.value, "6button"), "shipped default: t_pad=%s", var.value);
	var.key = "t_skip";
	CHECK(opts_env_get_variable(&var) && !strcmp(var.value, "auto"),
	      "invalid shipped value ignored, package default: t_skip=%s", var.value);
	opts_env_get_update(&upd);
	opts_step(0, +1); /* slow -> fast (wraps) */
	opts_env_get_update(&upd);
	var.key = "t_mode";
	opts_env_get_variable(&var);
	CHECK(upd && !strcmp(var.value, "fast"), "menu change -> GET_VARIABLE_UPDATE, t_mode=%s", var.value);
	CHECK(opts_save(true) == 0, "per-game save");
	snprintf(p, sizeof(p), "%s/user/tcore/Game (USA).ini", dir);
	{
		size_t sz;
		char *txt = hread_file(p, &sz);

		CHECK(txt && strstr(txt, "t_mode = \"fast\"") && !strstr(txt, "t_pad"),
		      "game file holds only the difference");
		free(txt);
	}
	opts_free();
	ini_free(&core_ini);
	free((void *)op.ship_dir);
	free((void *)op.user_dir);
	{
		char cmd[300];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		if (system(cmd) != 0)
			printf("  (cleanup failed)\n");
	}
}

/* ----------------------------------------------------------------- bios */

static void test_bios(void)
{
	char dir[128], p[256], err[256], warn[256];
	struct core_info ci;
	const char *ini =
		"[core]\nid = tpico\n"
		"[bios:bios_CD_U.bin]\nmd5 = 900150983cd24fb0d6963f7d28e17f72\nrequired = segacd\n"
		"[bios:bios_CD_E.bin]\nmd5 = 00000000000000000000000000000000\nrequired = segacd\n"
		"[bios:neogeo.zip]\nrequired = neogeo\n"
		"[bios:scph5501.bin]\nmd5 = 490f666e1afb15b7362b406ed1cea246\nrequired = no\n";

	printf("bios check\n");
	snprintf(dir, sizeof(dir), "/tmp/rsos-host-test-bios-%d", (int)getpid());
	snprintf(p, sizeof(p), "%s/bios", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/roms/neogeo", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/tpico.ini", dir);
	hwrite_atomic(p, ini, strlen(ini), false);
	coreinfo_load(&ci, dir, "tpico");
	snprintf(p, sizeof(p), "%s/bios", dir);
	CHECK(bios_check(&ci, "megadrive", "/x/sonic.md", p, err, sizeof(err), warn, sizeof(warn)),
	      "Mega Drive needs nothing");
	CHECK(!bios_check(&ci, "segacd", "/x/game.cue", p, err, sizeof(err), warn, sizeof(warn)),
	      "Sega CD without BIOS refused: \"%s\"", err);
	{
		char f[300];

		snprintf(f, sizeof(f), "%s/bios/bios_CD_U.bin", dir);
		HWRITE_LIT(f, "abc", false);
	}
	CHECK(bios_check(&ci, "segacd", "/x/game.cue", p, err, sizeof(err), warn, sizeof(warn)),
	      "Sega CD with one of the three region BIOSes accepted");
	{
		char rom[300], f[300];

		snprintf(rom, sizeof(rom), "%s/roms/neogeo/mslug.zip", dir);
		CHECK(!bios_check(&ci, "neogeo", rom, p, err, sizeof(err), warn, sizeof(warn)),
		      "Neo Geo without neogeo.zip refused: \"%s\"", err);
		snprintf(f, sizeof(f), "%s/roms/neogeo/neogeo.zip", dir);
		HWRITE_LIT(f, "PK", false);
		CHECK(bios_check(&ci, "neogeo", rom, p, err, sizeof(err), warn, sizeof(warn)),
		      "Neo Geo with neogeo.zip next to the game accepted");
	}
	coreinfo_free(&ci);
	{
		char cmd[300];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		if (system(cmd) != 0)
			printf("  (cleanup failed)\n");
	}
}

static void write_str(const char *path, const char *s)
{
	hwrite_atomic(path, s, strlen(s), false);
}

static void test_pick_and_tree(void)
{
	char dir[128], p[512], id[64];
	struct core_info ci;

	printf("core choice, system_tree\n");
	snprintf(dir, sizeof(dir), "/tmp/rsos-host-test-pick-%d", (int)getpid());
	snprintf(p, sizeof(p), "%s/cores", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/cores/gambatte.ini", dir);
	write_str(p, "[core]\nsystems = gb, gbc\n");
	snprintf(p, sizeof(p), "%s/cores/gearboy.ini", dir);
	write_str(p, "[core]\nsystems = gb, gbc\n[bios:7800 BIOS (U).rom]\nrequired = no\n"
		     "[bios:Machines/Shared Roms/MSX2P.rom]\nrequired = msx\n");
	snprintf(p, sizeof(p), "%s/cores/zz.ini", dir);
	write_str(p, "[core]\nsystems = foo\n");
	snprintf(p, sizeof(p), "%s/cores/fbneo.ini", dir);
	write_str(p, "[core]\nsystems = fbneo, neogeo\nextensions = zip, 7z\nblock_extract = true\n");
	snprintf(p, sizeof(p), "%s/cores/geolith.ini", dir);
	write_str(p, "[core]\nsystems = neogeo, neocd\nextensions = neo, cue, chd\nexperimental = true\n");
	snprintf(p, sizeof(p), "%s/cores", dir);
	CHECK(coreinfo_pick(p, NULL, "gb", "/x/Tetris.gb", id, sizeof(id)) == 0 && !strcmp(id, "gambatte"),
	      "gb default from the docs table: %s", id);
	CHECK(coreinfo_pick(p, NULL, "foo", NULL, id, sizeof(id)) == 0 && !strcmp(id, "zz"),
	      "unknown system: first core listing it: %s", id);
	{
		char ch[300];

		snprintf(ch, sizeof(ch), "%s/cores.ini", dir);
		write_str(ch, "[gb/Tetris (World)]\ncore = gearboy\n");
		CHECK(coreinfo_pick(p, ch, "gb", "/x/Tetris (World).gb", id, sizeof(id)) == 0 && !strcmp(id, "gearboy"),
		      "per-game choice: %s", id);
		CHECK(coreinfo_pick(p, ch, "gb", "/x/Other.gb", id, sizeof(id)) == 0 && !strcmp(id, "gambatte"),
		      "other game keeps the default: %s", id);
		write_str(ch, "[neogeo]\ncore = geolith\n");
		CHECK(coreinfo_pick(p, ch, "neogeo", "/x/mslug.zip", id, sizeof(id)) == 0 && !strcmp(id, "fbneo"),
		      "per-system choice skipped when the core cannot open .zip: %s", id);
		CHECK(coreinfo_pick(p, ch, "neogeo", "/x/mslug.neo", id, sizeof(id)) == 0 && !strcmp(id, "geolith"),
		      "user choice may be experimental (.neo): %s", id);
	}
	{
		struct core_candidate c[8];
		int nc = coreinfo_candidates(p, "neogeo", "/x/mslug.neo", c, 8);

		CHECK(coreinfo_pick(p, NULL, "neogeo", "/x/mslug.neo", id, sizeof(id)) == -ENOENT && nc == 1 &&
		      !strcmp(c[0].id, "geolith") && c[0].experimental && !c[0].is_default,
		      "experimental core never picked automatically; offered as a candidate (%d)", nc);
		CHECK(coreinfo_pick(p, NULL, "neocd", "/x/game.cue", id, sizeof(id)) == 0 && !strcmp(id, "geolith"),
		      "neocd: the documented experimental default: %s", id);
	}
	CHECK(coreinfo_load(&ci, p, "gearboy") == 0 && ci.nbios == 2 &&
	      !strcmp(ci.bios[0].file, "7800 BIOS (U).rom") && !strcmp(ci.bios[1].file, "Machines/Shared Roms/MSX2P.rom"),
	      "BIOS names with spaces, parentheses and slashes: \"%s\", \"%s\"", ci.bios[0].file, ci.bios[1].file);
	coreinfo_free(&ci);

	/* system_tree: copy missing files, never overwrite. */
	snprintf(p, sizeof(p), "%s/tree/Machines/MSX2+ - C-BIOS", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/tree/Machines/MSX2+ - C-BIOS/config.ini", dir);
	write_str(p, "shipped");
	snprintf(p, sizeof(p), "%s/tree/Databases", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/tree/Databases/msxromdb.xml", dir);
	write_str(p, "shipped");
	snprintf(p, sizeof(p), "%s/bios/Databases", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/bios/Databases/msxromdb.xml", dir);
	write_str(p, "user");
	memset(&ci, 0, sizeof(ci));
	snprintf(ci.system_tree, sizeof(ci.system_tree), "%s/tree", dir);
	snprintf(p, sizeof(p), "%s/bios", dir);
	coreinfo_install_system_tree(&ci, p);
	{
		char f[512];
		size_t sz;
		char *a, *b;

		snprintf(f, sizeof(f), "%s/bios/Machines/MSX2+ - C-BIOS/config.ini", dir);
		a = hread_file(f, &sz);
		snprintf(f, sizeof(f), "%s/bios/Databases/msxromdb.xml", dir);
		b = hread_file(f, &sz);
		CHECK(a && !strcmp(a, "shipped") && b && !strcmp(b, "user"),
		      "system_tree copies missing files and keeps the user's");
		free(a);
		free(b);
	}
	{
		char cmd[300];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		if (system(cmd) != 0)
			printf("  (cleanup failed)\n");
	}
}

/* ------------------------------------------------------ battery overlay */

static void test_batt_overlay(void)
{
	static uint32_t px[BOV_W * 2 * BOV_H * 2];
	char path[] = "/tmp/rsos-bov-XXXXXX";
	int pct, w, opaque = 0, clear = 0, bg = 0, other = 0, amber = 0, red = 0, bolt = 0, fd;
	bool chg;

	CHECK(bov_parse("70 0\n", &pct, &chg) == 0 && pct == 70 && !chg, "battery file: 70 0");
	CHECK(bov_parse("-1 1", &pct, &chg) == 0 && pct == -1 && chg, "battery file: unknown, charging");
	CHECK(bov_parse("101 0", &pct, &chg) < 0 && bov_parse("x", &pct, &chg) < 0 &&
	      bov_parse("50 2", &pct, &chg) < 0 && bov_parse(NULL, &pct, &chg) < 0, "battery file: garbage refused");
	fd = mkstemp(path);
	CHECK(fd >= 0 && write(fd, "42 1\n", 5) == 5, "battery file written");
	if (fd >= 0)
		close(fd);
	CHECK(bov_read(path, &pct, &chg) == 0 && pct == 42 && chg, "battery file read: %d %d", pct, chg);
	unlink(path);
	CHECK(bov_read(path, &pct, &chg) == -ENOENT, "missing battery file");

	/* 70 %, not charging, right corner: about 56 x 20, hugging the right edge */
	w = bov_render(px, BOV_W, 1, 70, false, true);
	CHECK(w >= 50 && w <= 60, "70%% pill width %d", w);
	for (int i = 0; i < BOV_W * BOV_H; i++) {
		uint32_t a = px[i] >> 24;

		opaque += a == 0xff;
		clear += px[i] == 0;
		bg += px[i] == 0xa8000000u;
		other += a != 0xff && px[i] != 0 && px[i] != 0xa8000000u;
	}
	CHECK(other == 0, "only clear, opaque or black-with-alpha pixels (%d others)", other);
	CHECK(opaque > 40 && bg > 400 && clear > 0, "content %d, pill %d, clear %d", opaque, bg, clear);
	CHECK(px[BOV_W - 1 + (BOV_H / 2) * BOV_W] == 0xa8000000u && px[BOV_H / 2 * BOV_W] == 0,
	      "right-aligned: pill at the right edge, clear on the left");
	w = bov_render(px, BOV_W, 1, 70, false, false);
	CHECK(px[BOV_H / 2 * BOV_W] == 0xa8000000u && px[BOV_W - 1 + BOV_H / 2 * BOV_W] == 0,
	      "left-aligned for the left corners");
	CHECK(px[0] == 0 && px[BOV_W * (BOV_H - 1)] == 0, "rounded corners");

	/* colours: amber <= 15 %, red <= 7 %, bolt when charging */
	bov_render(px, BOV_W, 1, 15, false, true);
	for (int i = 0; i < BOV_W * BOV_H; i++)
		amber += px[i] == 0xffffb020u;
	bov_render(px, BOV_W, 1, 7, false, true);
	for (int i = 0; i < BOV_W * BOV_H; i++)
		red += px[i] == 0xffff4038u;
	w = bov_render(px, BOV_W, 1, 100, true, true);
	for (int i = 0; i < BOV_W * BOV_H; i++)
		bolt += px[i] == 0xffffe040u;
	CHECK(amber > 30 && red > 30 && bolt > 10, "amber %d, red %d, bolt %d", amber, red, bolt);
	CHECK(w <= BOV_W, "100%% + bolt fits the buffer (%d <= %d)", w, BOV_W);

	/* 2x for large outputs: every pixel doubled */
	{
		static uint32_t one[BOV_W * BOV_H];
		bool same = true;

		bov_render(one, BOV_W, 1, 55, true, true);
		w = bov_render(px, BOV_W * 2, 2, 55, true, true);
		for (int y = 0; y < BOV_H * 2 && same; y++)
			for (int x = 0; x < BOV_W * 2; x++)
				if (px[y * BOV_W * 2 + x] != one[(y / 2) * BOV_W + x / 2]) {
					same = false;
					break;
				}
		CHECK(same && w > 100, "2x render = 1x doubled (width %d)", w);
	}
}

/* ------------------------------------------ options: sources, auto-save */

static char *slurp(const char *path)
{
	return hread_file(path, NULL);
}

static void test_options_menu(void)
{
	char dir[128], p[256], game[300], sys[300], *txt = NULL;
	struct opts_paths op;
	struct retro_variable var;
	static const struct retro_variable vars[] = {
		{ "t_mode", "Mode; fast|accurate|slow" },
		{ "t_pad", "Pad; 3button|6button" },
		{ NULL, NULL },
	};

	printf("core options: sources, auto-save, precedence\n");
	snprintf(dir, sizeof(dir), "/tmp/rsos-host-test-opts2-%d", (int)getpid());
	snprintf(p, sizeof(p), "%s/ship", dir);
	hmkdir_p(p, 0755);
	snprintf(p, sizeof(p), "%s/ship/tcore.ini", dir);
	HWRITE_LIT(p, "t_pad = \"6button\"\n", false);
	snprintf(p, sizeof(p), "%s/user/tcore", dir);
	hmkdir_p(p, 0755);
	snprintf(game, sizeof(game), "%s/user/tcore/G.ini", dir);
	snprintf(sys, sizeof(sys), "%s/user/tcore.ini", dir);
	HWRITE_LIT(game, "t_mode = \"slow\"\nrsos-glthread = \"true\"\n", false);
	snprintf(p, sizeof(p), "%s/ship", dir);
	op.ship_dir = strdup(p);
	snprintf(p, sizeof(p), "%s/user", dir);
	op.user_dir = strdup(p);

	opts_init("tcore", "G", NULL, &op);
	opts_env_set_variables(vars);
	CHECK(opts_source(0) == OPT_SRC_GAME && !strcmp(opts_source_tag(OPT_SRC_GAME), " (game)"),
	      "t_mode = slow comes from the game file");
	CHECK(opts_source(1) == OPT_SRC_DEFAULT, "t_pad = 6button is a RetroStone default");
	CHECK(opts_value("rsos-glthread") && !strcmp(opts_value("rsos-glthread"), "true"),
	      "a host key (not declared by the core) is read from the files");
	opts_step(0, -1); /* slow -> accurate */
	CHECK(opts_source(0) == OPT_SRC_UNSAVED && opts_dirty(), "a menu change is marked unsaved");
	CHECK(opts_autosave() == 1 && (txt = slurp(game)) && strstr(txt, "t_mode = \"accurate\"") &&
	      strstr(txt, "rsos-glthread"), "leaving the menu saves it for this game, other keys kept");
	free(txt);
	txt = NULL;
	CHECK(opts_source(0) == OPT_SRC_GAME && opts_autosave() == 0, "saved: tag (game), nothing more to save");
	/* "Save for all games" must not be beaten by the older per-game value */
	opts_step(0, +1); /* accurate -> slow */
	txt = NULL;
	CHECK(opts_save(false) == 0 && (txt = slurp(sys)) && strstr(txt, "t_mode = \"slow\""), "saved for all games");
	free(txt);
	txt = NULL;
	txt = slurp(game);
	CHECK(txt && !strstr(txt, "t_mode") && strstr(txt, "rsos-glthread"),
	      "the per-game t_mode is dropped (it would have won), the host key stays");
	free(txt);
	txt = NULL;
	CHECK(opts_source(0) == OPT_SRC_SYSTEM, "t_mode now comes from the all-games file");
	opts_free();

	/* next start: per game > per system > shipped */
	opts_init("tcore", "G", NULL, &op);
	opts_env_set_variables(vars);
	var.key = "t_mode";
	CHECK(opts_env_get_variable(&var) && !strcmp(var.value, "slow"), "restart: t_mode = %s", var.value);
	/* a file written behind the menu's back (the benchmark's "Use this") */
	HWRITE_LIT(game, "t_mode = \"fast\"\n", false);
	opts_reload_game();
	var.key = "t_mode";
	CHECK(opts_env_get_variable(&var) && !strcmp(var.value, "fast") && !opts_dirty(),
	      "reloaded per-game file: t_mode = %s", var.value);
	opts_free();

	/* the benchmark's session override beats every file, is never saved */
	opts_init("tcore", "G", NULL, &op);
	opts_set_override("t_mode", "accurate");
	opts_env_set_variables(vars);
	var.key = "t_mode";
	CHECK(opts_env_get_variable(&var) && !strcmp(var.value, "accurate") && opts_source(0) == OPT_SRC_OVERRIDE,
	      "override: t_mode = %s", var.value);
	opts_step(1, +1);
	opts_save(true);
	txt = slurp(game);
	CHECK(txt && !strstr(txt, "accurate"), "the override is not written to the game file");
	free(txt);
	txt = NULL;
	opts_free();
	free((void *)op.ship_dir);
	free((void *)op.user_dir);
	{
		char cmd[300];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		if (system(cmd) != 0)
			printf("  (cleanup failed)\n");
	}
}

/* ------------------------------------------------------------ benchmark */

static struct bench_result mkres(const char *id, const char *status, double speed, double eff)
{
	struct bench_result r;

	memset(&r, 0, sizeof(r));
	snprintf(r.id, sizeof(r.id), "%s", id);
	snprintf(r.label, sizeof(r.label), "label %s", id);
	snprintf(r.core, sizeof(r.core), "parallel_n64");
	snprintf(r.status, sizeof(r.status), "%s", status);
	r.speed = speed;
	r.eff_fps = eff;
	r.fps = eff;
	r.secs = 25;
	r.runs = 1500;
	return r;
}

static void test_bench(void)
{
	char dir[128], p[300], *txt = NULL, buf[16384];
	struct ini ini = { 0 };
	struct bench_plan plan, back;
	struct bench_result rs[6], r2;
	int order[6], best;

	printf("benchmark: plans, results, ranking, report, writing the choice\n");
	snprintf(dir, sizeof(dir), "/tmp/rsos-host-test-bench-%d", (int)getpid());
	hmkdir_p(dir, 0755);
	ini_parse(&ini, "[bench]\nseconds = 20\nwarmup = 4\n\n"
			"[rice-320]\nlabel = Rice 320x240\nparallel-n64-gfxplugin = rice\n"
			"parallel-n64-screensize = 320x240\n\n"
			"[m64p]\ncore = mupen64plus_next\nlabel = GLideN64\nmupen64plus-43screensize = 320x240\n");
	CHECK(bench_plan_parse(&ini, "parallel_n64", &plan) == 0 && plan.n == 2 && plan.seconds == 20 &&
	      plan.warmup == 4 && !strcmp(plan.c[0].core, "parallel_n64") && plan.c[0].nkv == 2 &&
	      !strcmp(plan.c[1].core, "mupen64plus_next") && !strcmp(plan.c[0].kv[0].value, "rice"),
	      "plan parsed: %d configurations, default core filled in", plan.n);
	ini_free(&ini);
	snprintf(plan.game, sizeof(plan.game), "Super Mario 64 (U) [!]");
	snprintf(plan.state, sizeof(plan.state), "/tmp/rsos/bench/start.state");
	snprintf(plan.core_id, sizeof(plan.core_id), "parallel_n64");
	snprintf(p, sizeof(p), "%s/plan.ini", dir);
	CHECK(bench_plan_write(p, &plan) == 0 && bench_plan_load(p, NULL, &back) == 0 && back.n == 2 &&
	      !strcmp(back.game, plan.game) && !strcmp(back.state, plan.state) &&
	      !strcmp(back.c[1].kv[0].key, "mupen64plus-43screensize"),
	      "plan written and read back (run fields, keys)");
	CHECK(bench_estimate_s(&plan) == 2 * (20 + 4 + 8), "time estimate %d s", bench_estimate_s(&plan));
	bench_result_path(p, 3, buf, sizeof(buf));
	CHECK(strstr(buf, "/result-3.ini") != NULL, "result path %s", buf);

	rs[0] = mkres("slow", "ok", 80, 24);        /* no skip, slow */
	rs[1] = mkres("skip", "ok", 110, 15);       /* frameskip: full speed, fewer frames */
	rs[2] = mkres("best", "ok", 105, 30);       /* full speed, all frames */
	rs[3] = mkres("crash", "crash", 0, 0);
	rs[4] = mkres("boot", "ok", 200, 60);
	rs[4].from_boot = true;                     /* another scene: not ranked */
	rs[5] = mkres("blank", "ok", 300, 60);
	rs[5].blank = true;                         /* a broken renderer */
	best = bench_rank(rs, 6, order);
	CHECK(best == 2 && order[0] == 2 && order[1] == 1 && order[2] == 0,
	      "ranking: full speed first by effective fps, then the fastest (%d %d %d)", order[0], order[1], order[2]);
	CHECK(!bench_eligible(&rs[3]) && !bench_eligible(&rs[4]) && !bench_eligible(&rs[5]) && order[3] >= 3,
	      "crash / from boot / blank picture are not ranked");
	{
		struct bench_result none[1] = { mkres("x", "hang", 0, 0) };

		CHECK(bench_rank(none, 1, order) == -1, "nothing ran: no best");
	}
	rs[2].ft_p99 = 33.5;
	rs[2].ncpu = 2;
	rs[2].cpu[1] = 41.5;
	snprintf(rs[2].shot, sizeof(rs[2].shot), "/data/rsos/logs/x-03-best.png");
	bench_result_format(&rs[2], buf, sizeof(buf));
	CHECK(bench_result_parse(buf, &r2) == 0 && !strcmp(r2.id, "best") && r2.speed == 105 && r2.ft_p99 == 33.5 &&
	      r2.ncpu == 2 && r2.cpu[1] == 41.5 && r2.runs == 1500 && !strcmp(r2.shot, rs[2].shot),
	      "result written and parsed back");
	CHECK(bench_result_parse("status = \"ok\"\n", &r2) < 0, "a result without an id is refused");
	{
		struct bench_plan rp = plan;

		rp.n = 2;
		snprintf(rp.c[0].id, sizeof(rp.c[0].id), "best");
		bench_format_report(&rp, rs, 6, best, true, buf, sizeof(buf));
		CHECK(strstr(buf, "Best: best") && strstr(buf, "parallel-n64-gfxplugin = \"rice\"") &&
		      strstr(buf, "not ranked") && strstr(buf, "Ranking"), "report: table, ranking, best settings");
		if (getenv("RSOS_TEST_VERBOSE"))
			fputs(buf, stdout);
		bench_format_report(&rp, rs, 2, -1, false, buf, sizeof(buf));
		CHECK(strstr(buf, "running: 2 of 2") != NULL, "partial report while running");
	}
	{
		uint8_t img[64 * 48 * 3];

		memset(img, 0x20, sizeof(img));
		CHECK(bench_flat_fraction(img, 64, 48) > 0.99, "flat picture detected");
		for (int i = 0; i < 64 * 48; i++) {
			img[3 * i] = (uint8_t)(i * 7);
			img[3 * i + 1] = (uint8_t)(i / 3);
			img[3 * i + 2] = (uint8_t)(i * 13);
		}
		CHECK(bench_flat_fraction(img, 64, 48) < 0.2, "a real picture is not flat (%.2f)",
		      bench_flat_fraction(img, 64, 48));
	}
	{
		char *const argv[] = { "--core", "x.so", "--rom", "r.z64", "--load-state", "auto", "--bench-start", "30",
				       "--status-fd", "3", "--bench-step", "plan", "1", "--stats", "--bench-auto-apply",
				       "--bench-report", "plan", NULL };
		const char *out[32];
		int n = bench_filter_args(17, argv, true, out, 32);

		CHECK(n == 5 && !strcmp(out[0], "--core") && !strcmp(out[3], "r.z64") && !strcmp(out[4], "--stats") &&
		      !out[5], "step arguments: resume, benchmark and status-fd dropped (%d)", n);
		n = bench_filter_args(17, argv, false, out, 32);
		CHECK(n == 8 && !strcmp(out[4], "--status-fd") && !strcmp(out[7], "--bench-auto-apply"),
		      "resume arguments keep the status fd (%d)", n);
	}
	bench_safe_name("Super Mario 64 (U) [!]", buf, 64);
	CHECK(!strcmp(buf, "Super_Mario_64_U"), "safe name: %s", buf);
	/* "Use this for this game" */
	{
		char user[200], gfile[300], choices[300];
		struct bench_config c;

		snprintf(user, sizeof(user), "%s/coreopts", dir);
		snprintf(gfile, sizeof(gfile), "%s/parallel_n64/Mario.ini", user);
		snprintf(choices, sizeof(choices), "%s/cores.ini", dir);
		hmkdir_p(user, 0755);
		snprintf(p, sizeof(p), "%s/parallel_n64", user);
		hmkdir_p(p, 0755);
		HWRITE_LIT(gfile, "parallel-n64-gfxplugin = \"glide64\"\nparallel-n64-pak1 = \"memory\"\n", false);
		c = plan.c[0];
		snprintf(c.kv[c.nkv].key, sizeof(c.kv[0].key), "rsos-glthread");
		snprintf(c.kv[c.nkv++].value, sizeof(c.kv[0].value), "true");
		CHECK(bench_apply(&c, user, "Mario", choices, "n64", "Mario", "parallel_n64") == 0 &&
		      (txt = slurp(gfile)) && strstr(txt, "parallel-n64-gfxplugin = \"rice\"") &&
		      !strstr(txt, "glide64") && strstr(txt, "parallel-n64-pak1 = \"memory\"") &&
		      strstr(txt, "rsos-glthread = \"true\"") && !hfile_exists(choices),
		      "per-game options merged (old value replaced, other keys kept), same core: no core choice");
		free(txt);
		txt = NULL;
		HWRITE_LIT(choices, "[n64]\ncore = parallel_n64\n", false);
		c = plan.c[1];
		CHECK(bench_apply(&c, user, "Mario", choices, "n64", "Mario", "parallel_n64") == 0 &&
		      (txt = slurp(choices)) && strstr(txt, "[n64]\ncore = \"parallel_n64\"") &&
		      strstr(txt, "[n64/Mario]\ncore = \"mupen64plus_next\""),
		      "other core: its options and the per-game core choice ([n64/Mario]) are written");
		free(txt);
		txt = NULL;
		{
			struct ini ch = { 0 };

			CHECK(ini_load(&ch, choices) == 0 && ini_get(&ch, "n64/Mario", "core") &&
			      !strcmp(ini_get(&ch, "n64/Mario", "core"), "mupen64plus_next") &&
			      !strcmp(ini_get(&ch, "n64", "core"), "parallel_n64"),
			      "the core choice file reads back as coreinfo_pick() expects");
			ini_free(&ch);
		}
		/* Review F-M9: "Use this for this game" kept only 64 sections
		 * (per-game core choices) and rewrote an unreadable file. */
		{
			char big[100 * 48] = "", line[48];
			struct ini ch = { 0 };
			int kept = 0;

			for (int i = 0; i < 100; i++) {
				snprintf(line, sizeof(line), "[snes/Game %d]\ncore = snes9x\n", i);
				strcat(big, line);
			}
			hwrite_atomic(choices, big, strlen(big), false);
			CHECK(bench_ini_set_file(choices, "n64/Mario", "core", "parallel_n64") == 0 &&
			      ini_load(&ch, choices) == 0, "cores.ini with 100 sections updated");
			for (int i = 0; i < 100; i++) {
				snprintf(line, sizeof(line), "snes/Game %d", i);
				kept += ini_get(&ch, line, "core") != NULL;
			}
			CHECK(kept == 100 && ini_get(&ch, "n64/Mario", "core"),
			      "every one of the 100 sections kept (%d), the new one added", kept);
			ini_free(&ch);
			remove(choices);
			hmkdir_p(choices, 0755);                     /* unreadable: a folder */
			CHECK(bench_ini_set_file(choices, "n64/Mario", "core", "x") < 0,
			      "an unreadable cores.ini is not rewritten");
			rmdir(choices);
		}
	}
	{
		char cmd[300];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		if (system(cmd) != 0)
			printf("  (cleanup failed)\n");
	}
}

/* ------------------------------------------------------ perf and overlay */

static void test_perf(void)
{
	static const char stat1[] =
		"cpu  100 0 100 800 0 0 0 0 0 0\n"
		"cpu0 50 0 50 400 0 0 0 0 0 0\n"
		"cpu1 50 0 50 400 0 0 0 0 0 0\n"
		"intr 12345\n";
	static const char stat2[] =
		"cpu  300 0 150 850 0 0 0 0 0 0\n"
		"cpu0 230 0 60 410 0 0 0 0 0 0\n"
		"cpu1 70 0 90 440 0 0 0 0 0 0\n";
	struct perf_cpu a, b;
	double pct[PERF_MAX_CPUS];
	float v[100];
	struct perf_counters c0, c1;
	struct perf_report r;
	char line[256];

	printf("perf counters\n");
	CHECK(perf_parse_proc_stat(stat1, &a) == 2 && perf_parse_proc_stat(stat2, &b) == 2 &&
	      perf_cpu_busy(&a, &b, pct, PERF_MAX_CPUS) == 2 && fabs(pct[0] - 190.0 / 200 * 100) < 0.01 &&
	      fabs(pct[1] - 60.0 / 100 * 100) < 0.01, "/proc/stat: cpu0 %.1f %%, cpu1 %.1f %%", pct[0], pct[1]);
	for (int i = 0; i < 100; i++)
		v[i] = (float)(100 - i);
	CHECK(fabs(perf_percentile(v, 100, 95) - 95.05) < 0.01 && perf_percentile(v, 100, 0) == 1 &&
	      perf_percentile(v, 0, 50) == 0, "percentiles (p95 of 1..100 = %.2f)", perf_percentile(v, 100, 95));
	memset(&c0, 0, sizeof(c0));
	memset(&c1, 0, sizeof(c1));
	c0.t_us = 1000000;
	c1.t_us = 11000000;              /* 10 s */
	c1.runs = 480;                   /* 48 VI/s of 60: 80 % */
	c1.delivered = 240;              /* a 30 fps game */
	c1.presented = 230;
	c1.core_ms = 480 * 16.0;
	c1.gl_swaps = 230;
	c1.gl_swap_us = 230 * 4000;
	c1.proc_cpu_us = 9000000;
	c0.cpu = a;
	c1.cpu = b;
	perf_diff(&c0, &c1, 60.0, &r);
	CHECK(fabs(r.speed_pct - 80) < 0.01 && fabs(r.fps - 24) < 0.01 && fabs(r.eff_fps - 24) < 0.01 &&
	      fabs(r.core_ms_avg - 16) < 0.01 && fabs(r.swap_ms - 4) < 0.01 && fabs(r.proc_cpu_pct - 90) < 0.01 &&
	      r.ncpu == 2, "report: speed %.0f %%, %.0f fps, effective %.0f, core %.1f ms, swap %.1f ms", r.speed_pct,
	      r.fps, r.eff_fps, r.core_ms_avg, r.swap_ms);
	CHECK(fabs(perf_eff_fps(30, 60, 60, 150) - 30) < 0.01, "effective fps capped at real speed");
	perf_format_overlay(&r, 7, line, sizeof(line));
	CHECK(!strcmp(line, "SPD 80% FPS 24.0 SKIP 7 CPU 95/60 GPU 4.0"), "overlay line \"%s\"", line);
	perf_format_log(&r, line, sizeof(line));
	CHECK(strstr(line, "speed 80.0 %") && strstr(line, "gl swap 4.00 ms"), "log line");

	/* the overlay strip: text left, battery right, same plane */
	{
		static uint32_t px[632 * 26 * 4];
		const char *lines[2] = { "SPD 80% FPS 24.0", "BENCH 1/8 rice" };
		int h = ovl_render_strip(px, 632, 1, lines, 2, 70, false, true), text = 0, batt = 0;

		for (int y = 0; y < h; y++)
			for (int x = 0; x < 632; x++) {
				uint32_t c = px[y * 632 + x];

				if (x < 200 && (c >> 24) == 0xff)
					text++;
				if (x > 632 - BOV_W && (c >> 24) == 0xff)
					batt++;
			}
		CHECK(h == ovl_strip_height(2) && text > 100 && batt > 30, "strip %dx%d: text %d px, battery %d px", 632,
		      h, text, batt);
		h = ovl_render_strip(px, 316, 2, lines, 1, -1, false, true);
		CHECK(h == BOV_H * 2 && px[5 * 632 + 0] == 0xa8000000u, "2x strip without battery (%d lines high)", h);
	}
}

#ifdef HOST_TEST_INPUT_PORTS
#include "../../input/input.h"

static void test_input_ports(void)
{
	struct input_port_dev ext[3] = {
		{ 3, 5, "/dev/input/event5", "030000005e0400008e02000014010000" },
		{ 4, 6, "/dev/input/event6", "03000000c82d00000031000011010000" },
	};
	int ports[INPUT_MAX_PORTS];

	printf("player assignment (the launching controller is player 1)\n");
	CHECK(input_ports_resolve_id("/dev/input/event6|03000000c82d00000031000011010000", ext, 2, true) == 4,
	      "exact id (node + guid)");
	CHECK(input_ports_resolve_id("/dev/input/event9|03000000c82d00000031000011010000", ext, 2, true) == 4,
	      "re-plugged pad (new node, same guid)");
	CHECK(input_ports_resolve_id("builtin", ext, 2, true) == -1 &&
	      input_ports_resolve_id("builtin", ext, 2, false) == -3, "built-in pad");
	CHECK(input_ports_resolve_id("/dev/input/event7|ffff", ext, 2, true) == -3 &&
	      input_ports_resolve_id("", ext, 2, true) == -3, "unknown or no id: none");
	input_ports_assign(ports, false, true, ext, 2, 4, false);
	CHECK(ports[0] == 4 && ports[1] == -1 && ports[2] == 3 && ports[3] == -2,
	      "launched by the second pad: P1 pad, P2 built-in, P3 first pad (%d %d %d %d)", ports[0], ports[1],
	      ports[2], ports[3]);
	input_ports_assign(ports, false, true, ext, 2, -1, true);
	CHECK(ports[0] == -1 && ports[1] == 3 && ports[2] == 4, "launched with the built-in pad on HDMI: built-in P1");
	input_ports_assign(ports, false, true, ext, 2, -3, false);
	CHECK(ports[0] == -1 && ports[1] == 3 && ports[2] == 4, "id missing, LCD: policy (built-in first)");
	input_ports_assign(ports, false, true, ext, 2, -3, true);
	CHECK(ports[0] == 3 && ports[1] == 4 && ports[2] == -1, "id missing, docked: policy (pads first)");
	/* hot-plug during the game */
	input_ports_assign(ports, false, true, ext, 2, 4, false);   /* 4, builtin, 3 */
	ext[2] = (struct input_port_dev){ 5, 7, "/dev/input/event7", "0300000054060000" };
	input_ports_assign(ports, true, true, ext, 3, -3, true);
	CHECK(ports[0] == 4 && ports[1] == -1 && ports[2] == 3 && ports[3] == 5,
	      "a new pad takes the next free port, P1 kept (%d %d %d %d)", ports[0], ports[1], ports[2], ports[3]);
	ext[1] = ext[2]; /* slot 4 (P1) unplugged */
	input_ports_assign(ports, true, true, ext, 2, -3, true);
	CHECK(ports[0] == -1 && ports[1] == 3 && ports[2] == 5 && ports[3] == -2,
	      "P1 unplugged: the others move up, P1 never empty (%d %d %d %d)", ports[0], ports[1], ports[2], ports[3]);
	ext[2] = (struct input_port_dev){ 4, 8, "/dev/input/event6", "03000000c82d00000031000011010000" };
	input_ports_assign(ports, true, true, ext, 3, -3, true);
	CHECK(ports[0] == -1 && ports[3] == 4, "plugged back: next free port, does not steal P1");
}
#endif

int main(void)
{
	hlog_set_level(HLOG_WARN);
	test_batt_overlay();
	test_perf();
	test_bench();
	test_options_menu();
#ifdef HOST_TEST_INPUT_PORTS
	test_input_ports();
#endif
	test_resampler();
	test_policy();
	test_drc();
	test_playtime();
	test_ini_md5();
	test_options();
	test_bios();
	test_pick_and_tree();
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
