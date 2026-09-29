/*
 * saves.h - battery saves (SRAM, RTC) and save states, written so that a
 * power cut at any moment never loses more than the last few seconds.
 *
 * Layout (RetroArch/RetroPie compatible names, <game> = content basename
 * without extension; for a zip or .m3u, the zip/m3u name):
 *   /data/saves/<system>/<game>.srm        RETRO_MEMORY_SAVE_RAM
 *   /data/saves/<system>/<game>.rtc        RETRO_MEMORY_RTC (GB/GBC MBC3)
 *   /data/states/<system>/<game>.state     slot 0
 *   /data/states/<system>/<game>.state<N>  slots 1..9
 *   /data/states/<system>/<game>.state.auto  power-off / low-battery slot
 *   <state file>.png                       thumbnail (last frame)
 *   <state file>.bak                       previous state (undo)
 *   <game>.state.auto.sram                 the .srm/.rtc when the auto state was written
 *   <game>.srm.bak, <game>.rtc.bak         the save before the first write after a
 *                                          state load (and before a resume from an
 *                                          auto state older than the save)
 *
 * Every write: <file>.tmp, fsync, rename over <file>, fsync(directory).
 * Big writes (states, periodic SRAM) run on a worker thread so a save never
 * stutters the game; exit / power-off flushes are synchronous.
 *
 * Resume and battery saves (docs/host-design.md): loading the auto state
 * never turns a newer .srm/.rtc on the card back into the state's older
 * copy; the file is kept as .bak and given back to the core after the state.
 */
#ifndef RSOS_HOST_SAVES_H
#define RSOS_HOST_SAVES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define STATE_SLOT_AUTO (-1)
/* The auto state's saves reference: <state><STATE_SAVES_REF_EXT>. */
#define STATE_SAVES_REF_EXT ".sram"

int saves_init(const char *save_dir, const char *state_dir, const char *game);
/* Waits for the worker, stops it. */
void saves_shutdown(void);

/* SRAM/RTC: load after retro_load_game(). A memory the core does not give
 * yet is loaded by sram_tick() when it first does, and never written
 * before. */
void sram_load(void);
/* Call every frame (cheap: works once per second). */
void sram_tick(int64_t now_ms);
/* Writes SRAM/RTC if they changed since the last write. sync: in this
 * thread, after the worker queue is empty. Returns 0 or -errno. */
int sram_flush(bool sync);

/* States. thumb: RGB24 (may be NULL). Returns 0 or -errno (queued when
 * !sync: the result arrives as a message). */
int state_save(int slot, const uint8_t *thumb, int tw, int th, bool sync);
/* Any successful load: the next write of .srm/.rtc keeps the file as .bak.
 * STATE_SLOT_AUTO (resume): a .srm/.rtc changed since the state was written
 * is kept as .bak and given back to the core after the state. */
int state_load(int slot);
void state_path(int slot, char *out, size_t n);
/* Writes <state>.sram for a state written outside state_save() (the
 * benchmark's start state, copied as the auto state at a power-off). The
 * .srm/.rtc must be flushed. Returns 0 or -errno. */
int state_write_saves_ref(const char *state);
bool state_exists(int slot, time_t *mtime);

/* Completion messages from the worker ("State saved to slot 2"). */
bool saves_next_message(char *buf, size_t n, bool *is_error);

/* Test hooks (rsos-run --test-states). */
int state_save_to(const char *path);
int state_load_from(const char *path);

/* Benchmark runs: SRAM and states are never written (the game's saves stay
 * as they were when the benchmark started). */
void saves_set_readonly(bool ro);

#endif
