/*
 * host_input_rsos.c - host_input.h on top of the real input layer
 * (src/input/input.h). Built by host.mk when src/input/input.c exists.
 */
#include "host_input.h"

#include "../board_apply.h"

#include <stddef.h>
#include <string.h>

static struct input *IN;

int hin_open(bool enabled, void (*on_power_off)(void *user), void *user)
{
	struct input_config cfg;

	if (!enabled)
		return 0;
	input_config_defaults(&cfg);
	board_apply_input(board_get(), &cfg);
	cfg.on_power_off = on_power_off;
	cfg.user = user;
	IN = input_open(&cfg);
	if (!IN)
		return -1;
	input_set_mode(IN, INPUT_MODE_GAME);
	return 0;
}

void hin_close(void)
{
	if (IN)
		input_close(IN);
	IN = NULL;
}

const char *hin_backend(void)
{
	return IN ? "input layer" : "none";
}

int hin_fd(void)
{
	return IN ? input_fd(IN) : -1;
}

int hin_timeout_ms(void)
{
	return IN ? input_timeout_ms(IN) : -1;
}

void hin_poll(void)
{
	if (IN)
		input_poll(IN);
}

void hin_set_game_mode(bool game)
{
	if (IN)
		input_set_mode(IN, game ? INPUT_MODE_GAME : INPUT_MODE_UI);
}

void hin_set_docked(bool docked)
{
	if (IN)
		input_set_docked(IN, docked);
}

void hin_set_p1_policy(enum input_p1_policy p)
{
	if (IN)
		input_set_p1_policy(IN, p);
}

void hin_set_p1_device(const char *id)
{
	if (IN)
		input_set_p1_device(IN, id);
}

void hin_set_cz_buttons(bool fitted)
{
	if (IN)
		input_set_cz_buttons(IN, fitted);
}

bool hin_next_hotkey(enum input_hotkey *hk)
{
	return IN && input_next_hotkey(IN, hk);
}

bool hin_next_nav(struct input_nav *ev)
{
	return IN && input_next_nav(IN, ev);
}

const char *hin_last_pad(void)
{
	return IN ? input_last_pad(IN) : "";
}

uint16_t hin_buttons(int port)
{
	return IN ? input_port_buttons(IN, port) : 0;
}

int16_t hin_analog(int port, int stick, int axis)
{
	return IN ? input_port_analog(IN, port, stick, axis) : 0;
}

int16_t hin_analog_button(int port, int btn)
{
	return IN ? input_port_analog_button(IN, port, btn) : 0;
}

bool hin_has_analog(int port)
{
	return IN && input_port_has_analog(IN, port);
}

void hin_port_info(int port, struct input_port_info *out)
{
	if (IN) {
		input_port_info(IN, port, out);
		return;
	}
	memset(out, 0, sizeof(*out));
	out->device = -1;
}

void hin_set_dpad_to_analog(int port, bool on)
{
	if (IN)
		input_set_dpad_to_analog(IN, port, on);
}

const char *hin_load_remap(const char *system, const char *game)
{
	return IN ? input_load_remap(IN, system, game) : NULL;
}

bool hin_rumble(int port, int effect, uint16_t strength)
{
	/* the libretro port is the input layer's port; effect 0 strong, 1 weak */
	return IN && input_rumble(IN, port, effect, strength);
}

void hin_set_rumble(bool enabled)
{
	if (IN)
		input_set_rumble(IN, enabled);
}
