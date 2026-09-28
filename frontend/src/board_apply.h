/*
 * board_apply.h - the board profile (board.h) turned into the module
 * configurations shared by the menu and the game process. The module
 * defaults (input_config_defaults(), display_config_defaults(), ...) are
 * the RetroStone2 values; these helpers replace them with the profile's,
 * which are the same on the RetroStone2 (tests/test_board.c).
 * board_apply_power() only in the menu (the power module is not in the
 * game process).
 */
#ifndef RSOS_BOARD_APPLY_H
#define RSOS_BOARD_APPLY_H

#include <string.h>
#include <strings.h>

#include "board.h"
#include "display.h"
#include "input/input.h"
#include "power/power.h"

static inline void board_apply_input(const struct board_profile *b, struct input_config *c)
{
	c->builtin_prefix = b->builtin_pad_prefix;      /* "" = no built-in pad */
	c->builtin_stick = b->builtin_stick;
	c->builtin_name = b->builtin_pad_name;
	/* auto: this layer never opens power-button-only devices unless it
	 * handles the power key itself (input.c), so no name is needed */
	c->power_key_name = b->power_key_device[0] && strcasecmp(b->power_key_device, "none")
				    ? b->power_key_device : "";
	c->backlight_name = board_name_or_auto(b->backlight);
}

static inline void board_apply_display(const struct board_profile *b, struct display_config *c)
{
	c->internal_mode = b->internal_display == BOARD_INTERNAL_NONE ? DISPLAY_INTERNAL_NONE :
			   b->internal_display == BOARD_INTERNAL_LIST ? DISPLAY_INTERNAL_LIST :
			   DISPLAY_INTERNAL_AUTO;
	c->internal_types = b->internal_types;
	c->a20_clock_log = (b->display_quirks & BOARD_QUIRK_SUN4I_TCON0_CLOCK) != 0;
}

static inline void board_apply_power(const struct board_profile *b, struct power_config *c)
{
	c->builtin_prefix = b->builtin_pad_prefix;
	c->pek_name = board_name_or_auto(b->power_key_device);   /* NULL = auto */
	c->pek_startup_ms = b->pek_startup_ms;
	c->axp_voff_mv = b->battery_voff_mv;
	c->battery_name = board_name_or_auto(b->battery_supply);
	c->ac_name = board_name_or_auto(b->ac_supply);
	c->usb_name = board_name_or_auto(b->usb_supply);
	c->backlight_name = board_name_or_auto(b->backlight);
	c->thermal_type = board_name_or_auto(b->thermal_zone);
	c->menu_governor = b->cpu_governor_menu;
	c->game_governor = b->cpu_governor_game;
}

#endif /* RSOS_BOARD_APPLY_H */

/* The UI part: only after ui/ui.h (the menu and the tests include this
 * header after it). */
#if defined(RSOS_UI_H) && !defined(RSOS_BOARD_APPLY_UI)
#define RSOS_BOARD_APPLY_UI
static inline void board_apply_ui(const struct board_profile *b, struct ui_config *c)
{
	c->has_internal_display = b->internal_display != BOARD_INTERNAL_NONE;
	c->lcd_refresh_choice = b->refresh_native > 0 && b->refresh_60;
	c->storage_overlays = b->storage_overlays;
}
#endif
