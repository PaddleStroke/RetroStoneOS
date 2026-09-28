/*
 * host_input.h - the small input interface the libretro host needs.
 *
 * Two backends, picked at build time by host.mk:
 *   host_input_rsos.c   pass-through to the real input layer
 *                       (src/input/input.h, the UI agent's), used as soon
 *                       as src/input/input.c exists;
 *   host_input_evdev.c  a temporary stand-alone evdev reader for bring-up
 *                       (all devices merged into player 1, Linux gamepad
 *                       codes + a keyboard map, Select hotkeys, power key
 *                       long press, no hotplug, no remaps).
 * The enums (IN_*, IN_HK_*) are input.h's, so switching is a relink.
 * Passing enabled = false (headless) gives a null backend: no devices.
 */
#ifndef RSOS_HOST_INPUT_H
#define RSOS_HOST_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "../input/input.h"

int hin_open(bool enabled, void (*on_power_off)(void *user), void *user);
void hin_close(void);
const char *hin_backend(void);

/* One pollable fd (-1 if none) and the ms until the next internal timer
 * (key repeat, power long press), -1 = none. */
int hin_fd(void);
int hin_timeout_ms(void);
/* Non-blocking: reads every pending event. Call right before retro_run(). */
void hin_poll(void);

/* false: UI navigation mode (in-game menu), true: game mode. */
void hin_set_game_mode(bool game);
void hin_set_docked(bool docked);
/* Settings: player-1 policy (settings `p1`) and the optional C/Z buttons
 * (settings `cz_buttons`: the remap lookup then tries <system>-cz.ini). */
void hin_set_p1_policy(enum input_p1_policy p);
/* The controller that launched the game (input_last_source_id() of the
 * UI, --p1-device): port 1 under the auto policy. */
void hin_set_p1_device(const char *id);
void hin_set_cz_buttons(bool fitted);

bool hin_next_hotkey(enum input_hotkey *hk);
bool hin_next_nav(struct input_nav *ev);
const char *hin_last_pad(void);

/* RetroPad state per port (0..3), remaps applied, hotkey buttons removed. */
uint16_t hin_buttons(int port);
int16_t hin_analog(int port, int stick, int axis);
int16_t hin_analog_button(int port, int btn);
bool hin_has_analog(int port);
void hin_port_info(int port, struct input_port_info *out);
void hin_set_dpad_to_analog(int port, bool on);

const char *hin_load_remap(const char *system, const char *game);

/* libretro rumble (strong/weak, 0..0xffff). false if unsupported. */
bool hin_rumble(int port, int effect, uint16_t strength);
/* Settings > Controls > Controller vibration (settings `rumble`, default on). */
void hin_set_rumble(bool enabled);

#endif
