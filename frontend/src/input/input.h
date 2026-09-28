/*
 * input.h - RetroStoneOS input layer (evdev, no udev/SDL). The single
 * input path for the UI and for the libretro host. See
 * docs/input-design.md (behaviour) and docs/ui-design.md (API, formats).
 *
 *  - Devices: every evdev node under /dev/input, hotplugged with inotify.
 *    Built-in devices (name "RetroStone2*" and "analog-stick" on the
 *    RetroStone2: input_config.builtin_prefix/builtin_stick) are merged
 *    into one logical pad. External pads are resolved, in order, from a
 *    user mapping (/data/rsos/input/<guid>.cfg), the SDL GameControllerDB
 *    (gamecontrollerdb.txt), the Linux gamepad spec codes; anything else
 *    is "unconfigured" (the UI offers the configuration wizard).
 *  - Output: RetroPad state per player port (1..4, index 0..3), with the
 *    remap layer applied, hotkey events (Select+...), UI navigation events
 *    with key repeat, and global keys (brightness, power).
 *
 * Single-threaded. Typical libretro host frame:
 *     input_poll(in);                       // non-blocking, right before retro_run()
 *     while (input_next_hotkey(in, &hk)) ...;
 *     retro_run();  // input_state_cb -> input_port_buttons()/input_port_analog()
 *
 * UI loop: poll(input_fd(in), timeout = input_timeout_ms(in)), then
 *     input_poll(in); while (input_next_nav(in, &ev)) ui_input(ui, ...);
 */
#ifndef RSOS_INPUT_H
#define RSOS_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* RetroPad button ids: identical to RETRO_DEVICE_ID_JOYPAD_*, so the
 * bitmask can be returned as-is for RETRO_DEVICE_ID_JOYPAD_MASK. */
enum input_btn {
	IN_B = 0, IN_Y, IN_SELECT, IN_START, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
	IN_A, IN_X, IN_L, IN_R, IN_L2, IN_R2, IN_L3, IN_R3,
	IN_NUM_BUTTONS,
};

#define INPUT_MAX_PORTS 4
#define INPUT_MAX_DEVICES 16

enum input_analog_axis { IN_AXIS_X = 0, IN_AXIS_Y = 1 };
enum input_analog_stick { IN_STICK_LEFT = 0, IN_STICK_RIGHT = 1 };

/* UI navigation events (from any pad, with key repeat on d-pad/L/R). */
enum input_nav_type { IN_NAV_PRESS = 0, IN_NAV_RELEASE, IN_NAV_REPEAT };
struct input_nav {
	enum input_btn btn;
	enum input_nav_type type;
	int device;              /* device slot, -1 = synthetic */
};

/* Hotkeys (game mode only) and global keys (always). */
enum input_hotkey {
	IN_HK_NONE = 0,
	IN_HK_EXIT,              /* Select + Start */
	IN_HK_SAVE_STATE,        /* Select + R */
	IN_HK_LOAD_STATE,        /* Select + L */
	IN_HK_SLOT_NEXT,         /* Select + Right */
	IN_HK_SLOT_PREV,         /* Select + Left */
	IN_HK_MENU,              /* Select + X */
	IN_HK_RESET,             /* Select + B */
	IN_HK_POWER_SHORT,       /* power key tap */
	IN_HK_POWER_OFF,         /* power key held >= 2 s: clean power-off */
	IN_HK_BRIGHTNESS,        /* brightness changed (overlay) */
	IN_HK_PAD_CONNECTED,     /* a pad appeared (toast); see input_last_pad() */
	IN_HK_PAD_DISCONNECTED,
	IN_HK_PAD_UNCONFIGURED,  /* an unknown pad appeared: offer the wizard */
	IN_HK_PORTS_CHANGED,     /* player assignment changed */
	IN_HK_FAST_FORWARD,      /* Select + R2: fast-forward on/off */
	IN_HK_SCREENSHOT,        /* Select + L2: PNG of the game picture */
	IN_HK_SWITCHER,          /* Select + Y: the game switcher (recent games) */
};

enum input_mode { INPUT_MODE_UI = 0, INPUT_MODE_GAME };
enum input_p1_policy { INPUT_P1_AUTO = 0, INPUT_P1_BUILTIN, INPUT_P1_EXTERNAL };

enum input_mapping_source {
	IN_MAP_BUILTIN = 0, IN_MAP_USER, IN_MAP_GCDB, IN_MAP_GAMEPAD_SPEC,
	IN_MAP_KEYBOARD, IN_MAP_NONE,
};

struct input_config {
	const char *dev_dir;         /* "/dev/input" */
	const char *user_map_dir;    /* "/data/rsos/input" */
	const char *gcdb_path;       /* "/usr/share/rsos/gamecontrollerdb.txt" */
	const char *remap_sys_dir;   /* "/usr/share/rsos/remaps" */
	const char *remap_user_dir;  /* "/data/rsos/remaps" */
	const char *backlight_dir;   /* "/sys/class/backlight" */
	int repeat_delay_ms;         /* 400 */
	int repeat_rate_ms;          /* 70 */
	int power_long_ms;           /* 2000 */
	/* false (default): the power module (src/power) owns axp20x-pek and
	 * KEY_POWER; this layer then never opens that device nor reports
	 * IN_HK_POWER_*. true: standalone use (tests, rsos-run). */
	bool handle_power_key;
	/*
	 * The board (src/board.h; NULL = the RetroStone2 names below, "" =
	 * none): builtin_prefix "RetroStone2" (evdev name prefix of the
	 * built-in buttons), builtin_stick "analog-stick", builtin_name
	 * "RetroStone2 built-in" (shown for the merged pad), power_key_name
	 * "axp20x-pek" (never opened unless handle_power_key), backlight_name
	 * (NULL: the first entry of backlight_dir).
	 */
	const char *builtin_prefix;
	const char *builtin_stick;
	const char *builtin_name;
	const char *power_key_name;
	const char *backlight_name;
	/* Called when the power key is held long enough, before the
	 * IN_HK_POWER_OFF event is queued (flush saves, then power off). */
	void (*on_power_off)(void *user);
	void *user;
};

struct input_port_info {
	bool connected;
	int device;                  /* device slot, -1 = built-in logical pad */
	char name[64];
	bool has_analog;
	enum input_mapping_source source;
};

struct input_device_info {
	bool present;
	bool builtin;
	bool configured;
	char name[80];
	char guid[33];
	char path[32];
	enum input_mapping_source source;
	int port;                    /* -1 = none */
};

struct input;

void input_config_defaults(struct input_config *cfg);
struct input *input_open(const struct input_config *cfg);
void input_close(struct input *in);

/* One fd for poll()/epoll: all devices + inotify. */
int input_fd(const struct input *in);
/* Reads everything pending (non-blocking), runs timers. Returns the number
 * of evdev events handled. */
int input_poll(struct input *in);
/* ms until the next key repeat / long-press timer, -1 = none. */
int input_timeout_ms(const struct input *in);

void input_set_mode(struct input *in, enum input_mode mode);
/* Docked (HDMI) changes player 1 (auto policy without a launching
 * controller) and disables the brightness keys (no backlight on a TV). */
void input_set_docked(struct input *in, bool docked);
void input_set_p1_policy(struct input *in, enum input_p1_policy p);
/*
 * Player 1 = the controller that launched the game (docs/input-design.md
 * §3). input_last_source_id(): the device of the last new button press,
 * "builtin" (the merged built-in pad) or "<evdev path>|<SDL guid>", "" if
 * none yet; the UI passes it to the game process (--p1-device).
 * input_set_p1_device(): with the auto policy, that device gets port 1,
 * then the built-in pad (if it is not P1), then the other pads in
 * connection order; an id that matches no device (unplugged meanwhile)
 * falls back to the policy (docked: external first). "" clears it.
 * During a game, hot-plugged pads take the next free port and never take
 * P1 from a pad that is still connected.
 */
const char *input_last_source_id(const struct input *in);
void input_set_p1_device(struct input *in, const char *id);

/* Pure player-assignment helpers (unit-tested in the host tests). */
struct input_port_dev {
	int slot;
	int seq;                     /* connection order */
	const char *path;
	const char *guid;
};
/* The slot `id` designates: slot, -1 built-in, -3 none. */
int input_ports_resolve_id(const char *id, const struct input_port_dev *devs, int n, bool builtin_present);
/* ports[INPUT_MAX_PORTS]: -2 none, -1 built-in, else slot. forced: -3 none. */
void input_ports_assign(int *ports, bool incremental, bool builtin_present,
			const struct input_port_dev *ext, int n_ext, int forced, bool ext_first);

/* -------------------------------------------------------- libretro host */
uint16_t input_port_buttons(const struct input *in, int port);   /* bitmask of 1 << IN_* */
int16_t input_port_analog(const struct input *in, int port, int stick, int axis);
/* Analog value of a button (L2/R2 triggers), 0..0x7fff. */
int16_t input_port_analog_button(const struct input *in, int port, int btn);
bool input_port_has_analog(const struct input *in, int port);
void input_port_info(const struct input *in, int port, struct input_port_info *out);
/* N64 without a stick: the d-pad drives the left analog at full deflection
 * (the d-pad bits are then cleared). */
void input_set_dpad_to_analog(struct input *in, int port, bool on);

/*
 * Remap layer (RetroPad position -> RetroPad id delivered to the core),
 * applied to every port. Loads, in order, the first that exists:
 *   <remap_user_dir>/<system>/<game>.ini, <remap_user_dir>/<system>.ini,
 *   <remap_sys_dir>/<system>.ini. NULL system = identity.
 * Returns the file used (static buffer) or NULL.
 */
const char *input_load_remap(struct input *in, const char *system, const char *game);
void input_clear_remap(struct input *in);
/* "Extra C/Z buttons fitted" (settings cz_buttons=1): the remap lookup then
 * tries <system>-cz.ini first (e.g. megadrive-cz.ini). */
void input_set_cz_buttons(struct input *in, bool fitted);

/*
 * Rumble (the libretro rumble interface, docs/input-design.md §2): one
 * FF_RUMBLE effect per pad, uploaded on first use; effect 0 = strong
 * motor, 1 = weak, strength 0..0xffff (0 on both motors stops it). The
 * effect is updated (EVIOCSFF on the same id) and played again when a
 * strength changes. Returns false if the port's pad has no FF_RUMBLE (the
 * built-in pad has none), rumble is off, or the kernel refused.
 * input_set_rumble(false) (setting "Controller vibration" off) stops every
 * motor now and ignores later requests.
 */
bool input_rumble(struct input *in, int port, int effect, uint16_t strength);
void input_set_rumble(struct input *in, bool enabled);
/* True if the port's pad can rumble (tests, menus). */
bool input_port_has_rumble(const struct input *in, int port);
/* Pure helper (unit-tested): the magnitudes of the pad's one effect from the
 * two motor strengths; returns true when the effect must play (either > 0). */
bool input_rumble_mix(uint16_t strong, uint16_t weak, uint16_t *mag_strong, uint16_t *mag_weak);

bool input_next_hotkey(struct input *in, enum input_hotkey *hk);
/* Name of the pad concerned by the last PAD_* event. */
const char *input_last_pad(const struct input *in);

/* -------------------------------------------------------------------- UI */
bool input_next_nav(struct input *in, struct input_nav *ev);
/* Merged state of all pads (UI button test screen). */
uint16_t input_any_buttons(const struct input *in);
/* Built-in stick: raw values and whether it is fitted. */
bool input_builtin_stick(const struct input *in, int *x, int *y);
int input_device_count(const struct input *in);
void input_device_info(const struct input *in, int slot, struct input_device_info *out);

/* Backlight: 0..max, -1 if no backlight. */
int input_brightness_get(const struct input *in, int *max);
int input_brightness_set(struct input *in, int value);

/* ----------------------------------------------- configuration wizard */
/*
 * Raw capture for the "Configure controller" screen: while active, the
 * events of `slot` are not mapped; each new button press, hat direction or
 * axis push past half range is reported by input_capture_next() as an SDL
 * mapping element ("b3", "h0.1", "+a2", "-a1").
 */
void input_capture_begin(struct input *in, int slot);
bool input_capture_next(struct input *in, char *elem, int n);
void input_capture_end(struct input *in);
/* Saves "<guid>,<name>,a:b0,..." (SDL format, targets in SDL names) to
 * <user_map_dir>/<guid>.cfg and re-resolves the device. */
int input_save_user_mapping(struct input *in, int slot, const char *mapping);

const char *input_btn_name(enum input_btn b);   /* "A", "B", "L1", ... */

#endif
