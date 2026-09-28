/*
 * board.h - the board profile: what the frontend needs to know about the
 * hardware it runs on, so that a port to another board (docs/porting.md)
 * is a board.ini file rather than code changes.
 *
 * /etc/rsos/board.ini (installed by the board's rootfs overlay; the path
 * can be overridden with the RSOS_BOARD_INI environment variable) is read
 * once at start-up by the menu and by the game process. Every key has an
 * auto-detection default, so a board without a profile (a Raspberry Pi
 * with HDMI, a USB pad and no battery) just works: no built-in pad, no
 * battery UI, no LCD switching, a power key only if some input device is
 * a power button, HDMI-only output.
 *
 * Format: "key = value" lines; '#' and ';' start comments; [sections] and
 * unknown keys are ignored (the shell scripts read their own keys from the
 * same file: data_disk, data_partition, serial_console, wifi_*, ...).
 * Keys, with the default when the key (or the file) is missing:
 *
 *   name                      board name for logs and the built-in pad   "Generic"
 *   builtin_pad_prefix        evdev name prefix of the built-in buttons  "" (none)
 *   builtin_stick             evdev name of the built-in analog stick    "" (none)
 *   internal_display          connector types of the built-in screen:    auto
 *                             auto (any non-external type) | none |
 *                             a list of dpi, unknown, lvds, dsi, edp,
 *                             virtual, spi, composite, svideo, tv
 *   internal_refresh_options  refresh rates offered for the built-in     "" (no choice)
 *                             screen: "<native>,60" (e.g. "78,60")
 *   backlight                 /sys/class/backlight entry: auto | none    auto
 *                             | name
 *   battery_supply            /sys/class/power_supply entries: auto      auto
 *   ac_supply                 (first of its type) | none | name          auto
 *   usb_supply                                                            auto
 *   thermal_zone              thermal zone type: auto (a "cpu" zone)     auto
 *   power_key_device          evdev name of the power key: auto (any     auto
 *                             device with KEY_POWER and at most 4 keys)
 *                             | none | name (also the platform driver
 *                             whose startup/shutdown attributes are set)
 *   pek_startup_ms            power-on hold time to program (AXP PEK),   0 (leave)
 *                             0 = leave it
 *   audio_internal            ALSA card id for the built-in speaker/     auto
 *   audio_hdmi                jack and for HDMI: auto (name match:
 *                             "codec" / "hdmi") | card id
 *   battery_voff_mv           battery cut-off voltage to program (the    0 (leave)
 *                             battery voltage_min attribute: AXP V_OFF)
 *   audio_hdmi_pcm            ALSA PCM type for the HDMI card: plughw    plughw
 *                             or hdmi (the card's IEC958 set-up, needed
 *                             by vc4-hdmi on the Raspberry Pi)
 *   cpu_governor_menu         cpufreq governors                          schedutil
 *   cpu_governor_game                                                     performance
 *   storage_overlays          opt-in device tree overlays the Storage    "" (none)
 *                             menu offers (known: emmc sata)
 *   display_quirks            driver quirks: sun4i-tcon0-clock (log the  "" (none)
 *                             A20 TCON0 pixel clock model when the panel
 *                             is retimed). Scaling and plane limits need
 *                             no quirk: they are probed with TEST_ONLY
 *                             commits.
 */
#ifndef RSOS_BOARD_H
#define RSOS_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_INI_DEFAULT "/etc/rsos/board.ini"
#define BOARD_STR 64

enum board_internal_display {
	BOARD_INTERNAL_AUTO = 0,   /* every connector type that is not external */
	BOARD_INTERNAL_LIST,       /* the types in internal_types */
	BOARD_INTERNAL_NONE,       /* no built-in screen: external outputs only */
};

#define BOARD_QUIRK_SUN4I_TCON0_CLOCK 0x1u

struct board_profile {
	bool loaded;                          /* false: no file, all defaults */
	char path[256];                       /* the file read (or tried) */
	char name[BOARD_STR];
	char builtin_pad_prefix[BOARD_STR];
	char builtin_stick[BOARD_STR];
	char builtin_pad_name[BOARD_STR + 16];/* "<name> built-in" (derived) */
	enum board_internal_display internal_display;
	uint32_t internal_types;              /* DRM_MODE_CONNECTOR_* bits (LIST) */
	int refresh_native;                   /* internal_refresh_options: first value, 0 = none */
	bool refresh_60;                      /* 60 is offered as well */
	char backlight[BOARD_STR];            /* "" = auto, "none", or a name */
	char battery_supply[BOARD_STR];       /* "" = auto, "none", or a name */
	char ac_supply[BOARD_STR];
	char usb_supply[BOARD_STR];
	char thermal_zone[BOARD_STR];         /* "" = auto */
	char power_key_device[BOARD_STR];     /* "" = auto, "none", or a name */
	int pek_startup_ms;
	char audio_internal[BOARD_STR];       /* "" = auto, or an ALSA card id */
	char audio_hdmi[BOARD_STR];
	char audio_internal_dev[128];  /* "plughw:CARD=<id>,0", "" = auto */
	char audio_hdmi_dev[128];
	char audio_hdmi_pcm[32];               /* "" = plughw, e.g. "hdmi" */
	int battery_voff_mv;                  /* PMIC cut-off to program, 0 = leave */
	char cpu_governor_menu[32];
	char cpu_governor_game[32];
	char storage_overlays[BOARD_STR];     /* space separated */
	unsigned display_quirks;              /* BOARD_QUIRK_* */
};

/* Fills b with the defaults (a generic board, see above). */
void board_defaults(struct board_profile *b);

/*
 * Defaults, then the file at path (NULL: $RSOS_BOARD_INI, else
 * BOARD_INI_DEFAULT). Returns 0 when the file was read, -errno when it
 * could not be opened (b then holds the defaults, which is not an error).
 */
int board_load(struct board_profile *b, const char *path);

/* Parses one ini text (tests). */
void board_parse(struct board_profile *b, const char *text);

/* The process-wide profile, loaded on the first call (board_load(NULL)). */
const struct board_profile *board_get(void);
/* Loads the process-wide profile from path now (the menu's --root mode);
 * also exports RSOS_BOARD_INI so that the game process reads the same file. */
const struct board_profile *board_init(const char *path);

/* "auto"/"" -> NULL, "none" -> "", else the name: the convention of the
 * module configs (NULL = auto-detect, "" = none). */
const char *board_name_or_auto(const char *v);

/* True if a connector of this DRM type is the built-in screen (for the
 * display layer's internal mask): see display_config.internal_*. */
bool board_connector_is_internal(const struct board_profile *b, uint32_t drm_connector_type);

/* True if the storage overlay `name` (emmc, sata) is offered. */
bool board_has_storage_overlay(const struct board_profile *b, const char *name);

/* One line for the logs: "board RetroStone2 (/etc/rsos/board.ini)". */
void board_describe(const struct board_profile *b, char *out, int n);

#endif
