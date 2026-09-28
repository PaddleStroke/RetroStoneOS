/*
 * testcore.c - a minimal libretro core for the host's process-model tests.
 * RSOS_TESTCORE=segv crashes in retro_run() after 30 frames, =hang loops
 * forever there, =slow sleeps 20 ms per frame, =nostate refuses
 * retro_serialize(), anything else renders a
 * moving gradient and a 1 kHz tone.
 * It also declares v0 options and has 1 KB of SRAM (a frame counter):
 * testcore_speed (logged at load: the options regression test),
 * testcore_cost (busy milliseconds per frame: the benchmark test picks the
 * cheapest) and testcore_crash (SIGSEGV at frame 30: a crashing benchmark
 * run).
 */
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "../../../third_party/libretro/libretro.h"

static retro_environment_t env;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio;
static retro_input_poll_t poll_cb;
static uint16_t fb[160 * 120];
static uint8_t sram[1024];
static unsigned frame;
static double phase;
static int cost_ms;
static int crash;
static retro_log_printf_t log_cb;

static const char *var(const char *key)
{
	struct retro_variable v = { key, NULL };

	return env(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value ? v.value : "";
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
	static const struct retro_variable vars[] = {
		{ "testcore_speed", "Speed; normal|fast" },
		{ "testcore_cost", "Cost per frame (ms); 0|4|12" },
		{ "testcore_crash", "Crash; no|yes" },
		{ NULL, NULL },
	};
	bool no_game = true;

	env = cb;
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
RETRO_API void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *i)
{
	memset(i, 0, sizeof(*i));
	i->library_name = "testcore";
	i->library_version = "1";
	i->valid_extensions = "bin";
	i->need_fullpath = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *i)
{
	memset(i, 0, sizeof(*i));
	i->geometry.base_width = i->geometry.max_width = 160;
	i->geometry.base_height = i->geometry.max_height = 120;
	i->geometry.aspect_ratio = 4.0f / 3.0f;
	i->timing.fps = 60.0;
	i->timing.sample_rate = 44100.0;
}

RETRO_API bool retro_load_game(const struct retro_game_info *g)
{
	enum retro_pixel_format f = RETRO_PIXEL_FORMAT_RGB565;

	struct retro_log_callback lc;

	(void)g;
	if (env(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc))
		log_cb = lc.log;
	cost_ms = atoi(var("testcore_cost"));
	crash = !strcmp(var("testcore_crash"), "yes");
	if (log_cb)
		log_cb(RETRO_LOG_INFO, "testcore: testcore_speed=%s testcore_cost=%d testcore_crash=%d\n",
			   var("testcore_speed"), cost_ms, crash);
	return env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
}

/* Dies of SIGSEGV, also in the check-asan build (a null store would be
 * stopped by UBSan first, with exit(1): not the crash the tests expect). */
static void segv(void)
{
	signal(SIGSEGV, SIG_DFL);
	raise(SIGSEGV);
	abort();
}

RETRO_API void retro_run(void)
{
	const char *mode = getenv("RSOS_TESTCORE");
	int16_t snd[735 * 2];

	poll_cb();
	if (frame == 30 && ((mode && !strcmp(mode, "segv")) || crash))
		segv();
	if (cost_ms > 0) {
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		do
			clock_gettime(CLOCK_MONOTONIC, &b);
		while ((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000 < cost_ms);
	}
	if (frame == 30 && mode && !strcmp(mode, "hang"))
		for (;;)
			;
	if (mode && !strcmp(mode, "slow")) {
		/* ~20 ms per frame: a headless game that lasts long enough for a
		 * power-off during it (the frontend's resume test) */
		struct timespec ts = { 0, 20 * 1000000L };

		nanosleep(&ts, NULL);
	}
	for (int y = 0; y < 120; y++)
		for (int x = 0; x < 160; x++)
			fb[y * 160 + x] = (uint16_t)((((x + frame) & 31) << 11) | ((y & 63) << 5) | (frame & 31));
	video(fb, 160, 120, 160 * 2);
	for (int i = 0; i < 735; i++) {
		int16_t v = (int16_t)(8000 * sin(phase));

		phase += 2 * M_PI * 1000 / 44100.0;
		snd[2 * i] = snd[2 * i + 1] = v;
	}
	audio(snd, 735);
	frame++;
	memcpy(sram, &frame, sizeof(frame));
}

RETRO_API void retro_reset(void) { frame = 0; }
RETRO_API size_t retro_serialize_size(void) { return sizeof(frame); }
RETRO_API bool retro_serialize(void *d, size_t s)
{
	const char *mode = getenv("RSOS_TESTCORE");

	/* =nostate: the core refuses to save a state (the host must keep the
	 * previous .state.auto) */
	if (s < sizeof(frame) || (mode && !strcmp(mode, "nostate")))
		return false;
	memcpy(d, &frame, sizeof(frame));
	return true;
}
RETRO_API bool retro_unserialize(const void *d, size_t s) { if (s < sizeof(frame)) return false; memcpy(&frame, d, sizeof(frame)); return true; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info *g, size_t n) { (void)t; (void)g; (void)n; return false; }
RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof(sram) : 0; }
