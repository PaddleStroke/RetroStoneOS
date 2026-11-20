/*
 * input.c - evdev input layer. See input.h and docs/input-design.md.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "input.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../ui/util.h"

#define BITS_PER_LONG_ (8 * sizeof(unsigned long))
#define NBITS(x) ((((x) - 1) / (8 * sizeof(unsigned long))) + 1)
#define TEST_BIT(bit, arr) \
	(((arr)[(bit) / (8 * sizeof(unsigned long))] >> ((bit) % (8 * sizeof(unsigned long)))) & 1UL)

#define BTN_MASK(b) ((uint16_t)(1u << (b)))
#define DPAD_MASK (BTN_MASK(IN_UP) | BTN_MASK(IN_DOWN) | BTN_MASK(IN_LEFT) | BTN_MASK(IN_RIGHT))

/* Key targets beyond the RetroPad ids. */
enum {
	T_NONE = -1,
	T_BRIGHT_UP = 32,
	T_BRIGHT_DOWN,
	T_POWER,
	T_GUIDE,
};

/* What an absolute axis drives. */
enum ax_kind { AX_NONE = 0, AX_STICK, AX_BUTTONS, AX_TRIGGER, AX_HAT };

struct axmap {
	uint8_t kind;
	int8_t a, b;       /* STICK: stick, axis; BUTTONS: neg btn, pos btn;
			    * TRIGGER: btn, -; HAT: 0 = x, 1 = y */
	bool invert;
	bool half;         /* TRIGGER on a full-range axis: only the + half */
};

/* A logical pad: the merged built-in pad or one external device. */
struct pad {
	uint8_t cnt[IN_NUM_BUTTONS];   /* keys held per target */
	uint16_t axis_btns;            /* buttons driven by axes/hats */
	uint16_t suppress;             /* consumed by a hotkey until released */
	int16_t analog[2][2];
	int16_t trig[2];               /* L2, R2 analog */
	bool has_analog;
	bool stick_nav[4];             /* up, down, left, right from the stick */
	int64_t combo_since;           /* N64 pad: Z + Start held since (hotkey) */
	bool combo_fired;
};

struct dev {
	int fd;
	int slot;
	char path[32];
	char name[80];
	struct input_id id;
	char guid[33];
	bool builtin, builtin_stick, keyboard, power_only;
	bool configured;
	enum input_mapping_source source;
	enum input_pad_type type;     /* the kind of controller (input.h) */
	bool guide;                   /* Home / Guide / PS held (quit combination) */
	char driver[24];              /* the kernel driver ("xpad", "nintendo", ...) */
	int seq;                      /* connection order */
	int16_t key_map[KEY_CNT];     /* evdev key code -> target */
	struct axmap ax[ABS_CNT];
	struct input_absinfo absinfo[ABS_CNT];
	int abs_rest[ABS_CNT];        /* value at open (capture reference) */
	int16_t btn_index[KEY_CNT];   /* SDL button index per key code, -1 */
	int16_t axis_index[ABS_CNT];  /* SDL axis index per abs code, -1 */
	int16_t hat_index[4];         /* SDL hat index per HAT0..3, -1 */
	uint8_t hat_state[4];         /* SDL hat mask */
	uint8_t axis_btn_state[ABS_CNT]; /* for capture: 0, 1 = -, 2 = + */
	struct pad own;               /* external devices */
	struct pad *pad;              /* where events go (own or builtin) */
	int port;
	/* mapped keys this device holds down: a press or a release counts once
	 * on the pad, and a resync (SYN_DROPPED) changes only this device's
	 * share of a merged pad (review F-M14) */
	unsigned long down[NBITS(KEY_CNT)];
	bool dropping;                /* after SYN_DROPPED, until SYN_REPORT */
	/* held-key check (check_held): while a key is down, the kernel's key
	 * state is compared once a second (issue #10) */
	int64_t held_check_at;
	int64_t press_at[IN_NUM_BUTTONS];
	uint16_t long_logged;         /* targets already logged as held long */
	/* rumble: one FF_RUMBLE effect (input_rumble) */
	bool ff_rumble;               /* EV_FF with FF_RUMBLE, and the fd is writable */
	int ff_id;                    /* uploaded effect id, -1 = none yet */
	uint16_t ff_strength[2];      /* strong, weak as the core asked */
	bool ff_playing;
};

#define NAV_QUEUE 64
#define HK_QUEUE 32
#define CAP_QUEUE 16

/* While a key is down, its device's kernel key state is re-read this often;
 * a hold this long is logged once (issue #10: buttons reported as held) */
#define HELD_CHECK_MS 1000
#define HELD_LOG_MS 30000

struct input {
	struct input_config cfg;
	int epfd, infd;
	struct dev *devs[INPUT_MAX_DEVICES];
	struct pad builtin;
	bool builtin_present;
	bool stick_fitted;
	int stick_raw[2];
	int seq;
	enum input_mode mode;
	bool docked;
	enum input_p1_policy p1;
	char p1_id[112];              /* the launching controller (auto policy), "" = none */
	int last_src;                 /* device of the last button press: slot, -1 built-in, -3 none */
	char last_src_id[112];
	bool cz;
	/* ports: -2 = none, -1 = built-in pad, else device slot */
	int port_dev[INPUT_MAX_PORTS];
	bool dpad_analog[INPUT_MAX_PORTS];
	uint8_t remap[IN_NUM_BUTTONS];
	/* nav */
	uint16_t nav_state;
	int64_t nav_next[IN_NUM_BUTTONS];
	struct input_nav navq[NAV_QUEUE];
	int nav_head, nav_tail;
	/* hotkeys */
	enum input_hotkey hkq[HK_QUEUE];
	int hk_head, hk_tail;
	char last_pad[80];
	/* power key */
	int64_t power_down_at;
	bool power_fired;
	/* capture */
	int cap_slot;
	char capq[CAP_QUEUE][16];
	int cap_head, cap_tail;
	/* pending device nodes (inotify create before the node is usable) */
	char pending[8][32];
	int64_t pending_at[8];
	/* backlight */
	char bl_path[600];
	int bl_max;
	/* rumble ("Controller vibration", default on) */
	bool rumble_off;
};

static const char *const g_btn_names[IN_NUM_BUTTONS] = {
	"B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT",
	"A", "X", "L1", "R1", "L2", "R2", "L3", "R3",
};

/* remap file keys */
static const char *const g_remap_keys[IN_NUM_BUTTONS] = {
	"b", "y", "select", "start", "up", "down", "left", "right",
	"a", "x", "l", "r", "l2", "r2", "l3", "r3",
};

const char *input_btn_name(enum input_btn b)
{
	return (unsigned)b < IN_NUM_BUTTONS ? g_btn_names[b] : "?";
}

void input_config_defaults(struct input_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->dev_dir = "/dev/input";
	cfg->user_map_dir = "/data/rsos/input";
	cfg->gcdb_path = "/usr/share/rsos/gamecontrollerdb.txt";
	cfg->remap_sys_dir = "/usr/share/rsos/remaps";
	cfg->remap_user_dir = "/data/rsos/remaps";
	cfg->backlight_dir = "/sys/class/backlight";
	cfg->repeat_delay_ms = 400;
	cfg->repeat_rate_ms = 70;
	cfg->power_long_ms = 2000;
	cfg->builtin_prefix = "RetroStone2";
	cfg->builtin_stick = "analog-stick";
	cfg->builtin_name = "RetroStone2 built-in";
	cfg->power_key_name = "axp20x-pek";
}

/* ---------------------------------------------------------------- queues */
static void push_nav(struct input *in, enum input_btn b, enum input_nav_type t, int dev)
{
	int next = (in->nav_head + 1) % NAV_QUEUE;

	if (in->mode != INPUT_MODE_UI || next == in->nav_tail)
		return;
	in->navq[in->nav_head] = (struct input_nav){ b, t, dev };
	in->nav_head = next;
}

static void push_hk(struct input *in, enum input_hotkey hk)
{
	int next = (in->hk_head + 1) % HK_QUEUE;

	if (next == in->hk_tail)
		return;
	in->hkq[in->hk_head] = hk;
	in->hk_head = next;
}

bool input_next_nav(struct input *in, struct input_nav *ev)
{
	if (in->nav_tail == in->nav_head)
		return false;
	*ev = in->navq[in->nav_tail];
	in->nav_tail = (in->nav_tail + 1) % NAV_QUEUE;
	return true;
}

bool input_next_hotkey(struct input *in, enum input_hotkey *hk)
{
	if (in->hk_tail == in->hk_head)
		return false;
	*hk = in->hkq[in->hk_tail];
	in->hk_tail = (in->hk_tail + 1) % HK_QUEUE;
	return true;
}

const char *input_last_pad(const struct input *in)
{
	return in->last_pad;
}

/* ------------------------------------------------------------- backlight */
static void backlight_find(struct input *in)
{
	DIR *d = opendir(in->cfg.backlight_dir);
	struct dirent *de;

	const char *want = in->cfg.backlight_name;   /* NULL: the first one */
	bool retry = false;

	in->bl_path[0] = 0;
	in->bl_max = 0;
	if (!d)
		return;
	if (want && !*want) {                         /* "": the board has none */
		closedir(d);
		return;
	}
again:
	while ((de = readdir(d))) {
		char p[512];
		char *s;

		if (de->d_name[0] == '.')
			continue;
		if (want && strcmp(de->d_name, want))
			continue;
		snprintf(p, sizeof(p), "%s/%s/max_brightness", in->cfg.backlight_dir, de->d_name);
		s = file_read(p, NULL);
		if (!s)
			continue;
		in->bl_max = atoi(s);
		free(s);
		snprintf(in->bl_path, sizeof(in->bl_path), "%s/%s/brightness",
			 in->cfg.backlight_dir, de->d_name);
		break;
	}
	/* a named backlight that is not there: the first one, as without a name */
	if (!in->bl_path[0] && want && !retry) {
		retry = true;
		want = NULL;
		rewinddir(d);
		goto again;
	}
	closedir(d);
}

int input_brightness_get(const struct input *in, int *max)
{
	char *s;
	int v;

	if (max)
		*max = in->bl_max;
	if (!in->bl_path[0])
		return -1;
	s = file_read(in->bl_path, NULL);
	if (!s)
		return -1;
	v = atoi(s);
	free(s);
	return v;
}

int input_brightness_set(struct input *in, int value)
{
	char buf[16];
	int fd, n;

	if (!in->bl_path[0])
		return -ENODEV;
	/* never fully dark on the LCD: minimum 1 */
	value = CLAMP(value, 1, in->bl_max);
	fd = open(in->bl_path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = snprintf(buf, sizeof(buf), "%d\n", value);
	if (write(fd, buf, (size_t)n) != n) {
		int e = errno;

		close(fd);
		return -e;
	}
	close(fd);
	return value;
}

static void brightness_step(struct input *in, int dir)
{
	int max, cur = input_brightness_get(in, &max);
	int step;

	if (cur < 0 || max <= 0)
		return;
	step = MAX(1, max / 10);
	if (input_brightness_set(in, cur + dir * step) >= 0)
		push_hk(in, IN_HK_BRIGHTNESS);
}

/* -------------------------------------------------------------- mapping */
static void map_reset(struct dev *d)
{
	for (int i = 0; i < KEY_CNT; i++)
		d->key_map[i] = T_NONE;
	memset(d->ax, 0, sizeof(d->ax));
	d->type = IN_PAD_MODERN;
}

static const char *const g_pad_type_ids[IN_PAD_TYPES] = { "modern", "n64", "gc" };

const char *input_pad_type_id(enum input_pad_type t)
{
	return t >= 0 && t < IN_PAD_TYPES ? g_pad_type_ids[t] : g_pad_type_ids[0];
}

enum input_pad_type input_pad_type_from_id(const char *id)
{
	for (int t = 0; id && t < IN_PAD_TYPES; t++)
		if (!strcmp(id, g_pad_type_ids[t]))
			return (enum input_pad_type)t;
	return IN_PAD_MODERN;
}

/* Linux gamepad spec codes, already positional. Also the built-in pad. */
static void map_gamepad_spec(struct dev *d, const unsigned long *absbits)
{
	static const struct { int code, t; } keys[] = {
		{ BTN_SOUTH, IN_B }, { BTN_EAST, IN_A }, { BTN_NORTH, IN_X },
		{ BTN_WEST, IN_Y }, { BTN_TL, IN_L }, { BTN_TR, IN_R },
		{ BTN_TL2, IN_L2 }, { BTN_TR2, IN_R2 }, { BTN_SELECT, IN_SELECT },
		{ BTN_START, IN_START }, { BTN_THUMBL, IN_L3 }, { BTN_THUMBR, IN_R3 },
		{ BTN_C, IN_L3 }, { BTN_Z, IN_R3 }, { BTN_MODE, T_GUIDE },
		{ BTN_DPAD_UP, IN_UP }, { BTN_DPAD_DOWN, IN_DOWN },
		{ BTN_DPAD_LEFT, IN_LEFT }, { BTN_DPAD_RIGHT, IN_RIGHT },
		{ KEY_BRIGHTNESSUP, T_BRIGHT_UP }, { KEY_BRIGHTNESSDOWN, T_BRIGHT_DOWN },
		{ KEY_POWER, T_POWER },
	};

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++)
		d->key_map[keys[i].code] = (int16_t)keys[i].t;
	d->ax[ABS_X] = (struct axmap){ AX_STICK, 0, 0, false, false };
	d->ax[ABS_Y] = (struct axmap){ AX_STICK, 0, 1, false, false };
	if (TEST_BIT(ABS_RX, absbits)) {
		d->ax[ABS_RX] = (struct axmap){ AX_STICK, 1, 0, false, false };
		d->ax[ABS_RY] = (struct axmap){ AX_STICK, 1, 1, false, false };
		/* xpad style: Z/RZ are the analog triggers */
		d->ax[ABS_Z] = (struct axmap){ AX_TRIGGER, IN_L2, 0, false, false };
		d->ax[ABS_RZ] = (struct axmap){ AX_TRIGGER, IN_R2, 0, false, false };
	} else {
		/* HID style: Z/RZ are the right stick */
		d->ax[ABS_Z] = (struct axmap){ AX_STICK, 1, 0, false, false };
		d->ax[ABS_RZ] = (struct axmap){ AX_STICK, 1, 1, false, false };
	}
	d->ax[ABS_BRAKE] = (struct axmap){ AX_TRIGGER, IN_L2, 0, false, false };
	d->ax[ABS_GAS] = (struct axmap){ AX_TRIGGER, IN_R2, 0, false, false };
	d->ax[ABS_HAT0X] = (struct axmap){ AX_HAT, 0, 0, false, false };
	d->ax[ABS_HAT0Y] = (struct axmap){ AX_HAT, 1, 0, false, false };
}

static void map_keyboard(struct dev *d)
{
	static const struct { int code, t; } keys[] = {
		{ KEY_UP, IN_UP }, { KEY_DOWN, IN_DOWN }, { KEY_LEFT, IN_LEFT },
		{ KEY_RIGHT, IN_RIGHT }, { KEY_ENTER, IN_A }, { KEY_KPENTER, IN_A },
		{ KEY_ESC, IN_B }, { KEY_BACKSPACE, IN_B }, { KEY_X, IN_X }, { KEY_Y, IN_Y },
		{ KEY_PAGEUP, IN_L }, { KEY_PAGEDOWN, IN_R }, { KEY_TAB, IN_SELECT },
		{ KEY_SPACE, IN_START }, { KEY_HOME, IN_L2 }, { KEY_END, IN_R2 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(keys); i++)
		d->key_map[keys[i].code] = (int16_t)keys[i].t;
}

/* SDL-style indices: buttons from BTN_JOYSTICK to KEY_MAX, then from 0 to
 * BTN_JOYSTICK; axes in code order skipping hats; hats by pairs. */
static void make_sdl_indices(struct dev *d, const unsigned long *keybits,
			     const unsigned long *absbits)
{
	int n = 0;

	for (int i = 0; i < KEY_CNT; i++)
		d->btn_index[i] = -1;
	for (int i = BTN_JOYSTICK; i < KEY_MAX; i++)
		if (TEST_BIT(i, keybits))
			d->btn_index[i] = (int16_t)n++;
	for (int i = 0; i < BTN_JOYSTICK; i++)
		if (TEST_BIT(i, keybits))
			d->btn_index[i] = (int16_t)n++;
	n = 0;
	for (int i = 0; i < ABS_CNT; i++) {
		d->axis_index[i] = -1;
		if (i >= ABS_HAT0X && i <= ABS_HAT3Y)
			continue;
		if (TEST_BIT(i, absbits))
			d->axis_index[i] = (int16_t)n++;
	}
	n = 0;
	for (int h = 0; h < 4; h++) {
		d->hat_index[h] = -1;
		if (TEST_BIT(ABS_HAT0X + 2 * h, absbits) || TEST_BIT(ABS_HAT0Y + 2 * h, absbits))
			d->hat_index[h] = (int16_t)n++;
	}
}

static int code_for_btn_index(const struct dev *d, int idx)
{
	for (int i = 0; i < KEY_CNT; i++)
		if (d->btn_index[i] == idx)
			return i;
	return -1;
}

static int code_for_axis_index(const struct dev *d, int idx)
{
	for (int i = 0; i < ABS_CNT; i++)
		if (d->axis_index[i] == idx)
			return i;
	return -1;
}

/*
 * The SDL names that mean another button for a kind of pad (input.h, enum
 * input_pad_type): the database's own conventions for N64 and GameCube pads.
 */
static const struct { enum input_pad_type type; const char *n; int b; } g_type_names[] = {
	/* N64: A and B where an N64 game wants them (A on RetroPad B, B on
	 * RetroPad Y); the C buttons on X / A / L3 / R3, which n64_c_stick()
	 * turns into the right stick */
	{ IN_PAD_N64, "b", IN_Y },
	{ IN_PAD_N64, "-righty", IN_X }, { IN_PAD_N64, "+righty", IN_A },
	{ IN_PAD_N64, "-rightx", IN_L3 }, { IN_PAD_N64, "+rightx", IN_R3 },
	/* GameCube, by label: B is the left one, X the right one */
	{ IN_PAD_GC, "b", IN_Y }, { IN_PAD_GC, "x", IN_A },
};

/* SDL target name -> RetroPad id / stick axis, for a kind of pad. */
static int sdl_target(const char *t, enum input_pad_type type, int *stick, int *axis)
{
	static const struct { const char *n; int b; } btns[] = {
		{ "a", IN_B }, { "b", IN_A }, { "x", IN_Y }, { "y", IN_X },
		{ "back", IN_SELECT }, { "start", IN_START }, { "guide", T_GUIDE },
		{ "leftshoulder", IN_L }, { "rightshoulder", IN_R },
		{ "lefttrigger", IN_L2 }, { "righttrigger", IN_R2 },
		{ "leftstick", IN_L3 }, { "rightstick", IN_R3 },
		{ "dpup", IN_UP }, { "dpdown", IN_DOWN }, { "dpleft", IN_LEFT },
		{ "dpright", IN_RIGHT },
	};

	*stick = *axis = -1;
	for (size_t i = 0; i < ARRAY_SIZE(g_type_names); i++)
		if (g_type_names[i].type == type && !strcmp(t, g_type_names[i].n))
			return g_type_names[i].b;
	if (!strcmp(t, "leftx")) { *stick = 0; *axis = 0; return -2; }
	if (!strcmp(t, "lefty")) { *stick = 0; *axis = 1; return -2; }
	if (!strcmp(t, "rightx")) { *stick = 1; *axis = 0; return -2; }
	if (!strcmp(t, "righty")) { *stick = 1; *axis = 1; return -2; }
	for (size_t i = 0; i < ARRAY_SIZE(btns); i++)
		if (!strcmp(t, btns[i].n))
			return btns[i].b;
	return T_NONE;
}

/* Applies "a:b0,b:b1,dpup:h0.1,leftx:a0,lefttrigger:+a2,..." */
static int apply_sdl_mapping(struct dev *d, const char *mapping)
{
	char buf[2048];
	char *save = NULL, *tok;
	int n = 0;

	/* the kind of pad first: it gives the names their meaning */
	strlcpy_(buf, mapping, sizeof(buf));
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(tok, ':');

		if (!colon)
			continue;
		*colon = 0;
		if (!strcmp(str_trim(tok), "type"))
			d->type = input_pad_type_from_id(str_trim(colon + 1));
	}
	strlcpy_(buf, mapping, sizeof(buf));
	save = NULL;
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(tok, ':');
		char *src;
		int stick, axis, t;
		char sign = 0;
		bool invert = false;

		if (!colon)
			continue;
		*colon = 0;
		src = colon + 1;
		if (!strcmp(str_trim(tok), "type"))
			continue;
		t = sdl_target(str_trim(tok), d->type, &stick, &axis);
		if (t == T_NONE)
			continue;
		if (*src == '+' || *src == '-')
			sign = *src++;
		if (src[0] && src[strlen(src) - 1] == '~') {
			invert = true;
			src[strlen(src) - 1] = 0;
		}
		if (src[0] == 'b') {
			int code = code_for_btn_index(d, atoi(src + 1));

			if (code >= 0 && t >= 0) {
				d->key_map[code] = (int16_t)t;
				n++;
			}
		} else if (src[0] == 'h') {
			int hat = atoi(src + 1), mask = 0, code;
			const char *dot = strchr(src, '.');
			int h;

			if (dot)
				mask = atoi(dot + 1);
			for (h = 0; h < 4; h++)
				if (d->hat_index[h] == hat)
					break;
			if (h == 4 || t < 0)
				continue;
			/* 1 up, 2 right, 4 down, 8 left */
			code = (mask & (1 | 4)) ? ABS_HAT0Y + 2 * h : ABS_HAT0X + 2 * h;
			if (d->ax[code].kind != AX_BUTTONS)
				d->ax[code] = (struct axmap){ AX_BUTTONS, -1, -1, false, false };
			if (mask & (1 | 8))
				d->ax[code].a = (int8_t)t;
			else
				d->ax[code].b = (int8_t)t;
			n++;
		} else if (src[0] == 'a') {
			int code = code_for_axis_index(d, atoi(src + 1));

			if (code < 0)
				continue;
			if (t == -2) {
				d->ax[code] = (struct axmap){ AX_STICK, (int8_t)stick, (int8_t)axis,
							      invert, false };
			} else if (t == IN_L2 || t == IN_R2) {
				d->ax[code] = (struct axmap){ AX_TRIGGER, (int8_t)t, 0, invert,
							      sign == '+' };
			} else if (t >= 0) {
				if (d->ax[code].kind != AX_BUTTONS)
					d->ax[code] = (struct axmap){ AX_BUTTONS, -1, -1, false, false };
				if (sign == '-')
					d->ax[code].a = (int8_t)t;
				else
					d->ax[code].b = (int8_t)t;
			}
			n++;
		}
	}
	return n;
}

static void make_guid(struct dev *d)
{
	uint16_t g[8] = { 0 };

	g[0] = d->id.bustype;
	g[1] = 0; /* name CRC: SDL >= 2.26; matched as 0 */
	if (d->id.vendor) {
		g[2] = d->id.vendor;
		g[4] = d->id.product;
		g[6] = d->id.version;
	} else {
		memcpy(&g[2], d->name, MIN(strlen(d->name), (size_t)12));
	}
	for (int i = 0; i < 8; i++)
		snprintf(d->guid + 4 * i, 5, "%02x%02x", g[i] & 0xff, g[i] >> 8);
}

/* Compares GUIDs ignoring the CRC field (and the version if !with_ver). */
static bool guid_match(const char *a, const char *b, bool with_ver)
{
	for (int i = 0; i < 32; i++) {
		if (i >= 4 && i < 8)
			continue;
		if (!with_ver && i >= 24 && i < 28)
			continue;
		if ((a[i] | 0x20) != (b[i] | 0x20))
			return false;
	}
	return true;
}

static bool find_gcdb(const struct input *in, const struct dev *d, char *out, size_t n,
		      const char *path, bool any_platform)
{
	FILE *f = fopen(path, "r");
	char line[4096];
	bool found = false;

	if (!f)
		return false;
	for (int pass = 0; pass < 2 && !found; pass++) {
		rewind(f);
		while (fgets(line, sizeof(line), f)) {
			if (line[0] == '#' || strlen(line) < 34 || line[32] != ',')
				continue;
			if (!any_platform && !strstr(line, "platform:Linux"))
				continue;
			if (guid_match(line, d->guid, pass == 0)) {
				strlcpy_(out, line, n);
				found = true;
				break;
			}
		}
	}
	fclose(f);
	(void)in;
	return found;
}

/*
 * Pads the kernel drives with a layout of its own that the database does
 * not describe right (or under versions it does not list), by evdev code
 * (linux 6.18 drivers/hid/hid-nintendo.c). They win over the database.
 * Every other known pad comes from the database or the Linux standard.
 */
struct profile_key { uint16_t code; int8_t t; };

/* Nintendo Switch Online N64: BTN_A / BTN_B are A / B, the C buttons are on
 * Select (up), X (down), Y (left) and C (right), Z on TL2, ZR on TR2 */
static const struct profile_key g_nso_n64_keys[] = {
	{ BTN_SOUTH, IN_B }, { BTN_EAST, IN_Y },
	{ BTN_SELECT, IN_X }, { BTN_NORTH, IN_A }, { BTN_WEST, IN_L3 }, { BTN_C, IN_R3 },
	{ BTN_TL2, IN_L2 }, { BTN_TL, IN_L }, { BTN_TR, IN_R }, { BTN_TR2, IN_R2 },
	{ BTN_START, IN_START }, { BTN_MODE, T_GUIDE },
	{ 0, 0 },
};

/* Nintendo Switch Online NES: the kernel puts A at the bottom and B on the
 * right; on the pad A is the right one */
static const struct profile_key g_nso_nes_keys[] = {
	{ BTN_SOUTH, IN_A }, { BTN_EAST, IN_B }, { BTN_TL, IN_L }, { BTN_TR, IN_R },
	{ BTN_SELECT, IN_SELECT }, { BTN_START, IN_START },
	{ 0, 0 },
};

struct pad_profile {
	uint16_t vendor, product;
	int has, lacks;                 /* key codes that tell it apart, 0 = none */
	enum input_pad_type type;
	const struct profile_key *keys; /* NULL: the Linux standard is right */
};

static const struct pad_profile g_profiles[] = {
	{ 0x057e, 0x2019, 0, 0, IN_PAD_N64, g_nso_n64_keys },
	/* NES pads say they are Joy-Cons: they have A and B but no X */
	{ 0x057e, 0x2006, BTN_SOUTH, BTN_NORTH, IN_PAD_MODERN, g_nso_nes_keys },
	{ 0x057e, 0x2007, BTN_SOUTH, BTN_NORTH, IN_PAD_MODERN, g_nso_nes_keys },
	/* NSO Mega Drive / Genesis: the database's line is for another layout
	 * (its C on rightshoulder); the kernel's is positional (A left, B
	 * bottom, C right; X Y Z above: L1, X, R1) like Genesis Plus GX wants */
	{ 0x057e, 0x201e, 0, 0, IN_PAD_MODERN, NULL },
};

/*
 * Kernel drivers that lay their pads out by the Linux gamepad standard
 * (Documentation/input/gamepad.rst): their pads need no database line, and
 * a line made for another driver or version could only be wrong.
 */
static bool driver_follows_spec(const char *drv)
{
	static const char *const drivers[] = { "xpad", "playstation", "sony", "nintendo", "hid-steam" };

	for (size_t i = 0; i < ARRAY_SIZE(drivers); i++)
		if (!strcmp(drv, drivers[i]))
			return true;
	return false;
}

/* The name of the driver of /dev/input/eventN, "" if unknown */
static void read_driver(const char *path, char *out, size_t n)
{
	const char *ev = strrchr(path, '/');
	char sys[128], link[256];
	ssize_t r;

	out[0] = 0;
	snprintf(sys, sizeof(sys), "/sys/class/input/%s/device/device/driver", ev ? ev + 1 : path);
	r = readlink(sys, link, sizeof(link) - 1);
	if (r <= 0)
		return;
	link[r] = 0;
	strlcpy_(out, strrchr(link, '/') ? strrchr(link, '/') + 1 : link, n);
}

static const struct pad_profile *find_profile(const struct dev *d, const unsigned long *keybits)
{
	for (size_t i = 0; i < ARRAY_SIZE(g_profiles); i++) {
		const struct pad_profile *pf = &g_profiles[i];

		/* (all of them hid-nintendo's: another driver lays them out
		 * another way) */
		if (pf->vendor == d->id.vendor && pf->product == d->id.product &&
		    (!d->driver[0] || !strcmp(d->driver, "nintendo")) &&
		    (!pf->has || TEST_BIT(pf->has, keybits)) &&
		    (!pf->lacks || !TEST_BIT(pf->lacks, keybits)))
			return pf;
	}
	return NULL;
}

static bool has_word(const char *hay, const char *w)
{
	size_t n = strlen(w);

	for (const char *p = hay; *p; p++)
		if (!strncasecmp(p, w, n) && (p == hay || !isalnum((unsigned char)p[-1])) &&
		    !isalnum((unsigned char)p[n]))
			return true;
	return false;
}

/* The kind of pad from its name in the database: N64 and GameCube pads and
 * adapters follow the database's conventions for them (input.h). An
 * adapter for both maps the GameCube way. */
static enum input_pad_type type_from_name(const char *name)
{
	if (strcasestr(name, "gamecube") || has_word(name, "GC"))
		return IN_PAD_GC;
	if (has_word(name, "N64") || strcasestr(name, "nintendo 64"))
		return IN_PAD_N64;
	return IN_PAD_MODERN;
}

/*
 * A game controller, from its capabilities. A device without gamepad or
 * joystick buttons counts when it has a stick and a few keys, but not a
 * touchpad: PlayStation pads add theirs as a device of its own, with ABS_X
 * and enough keys to look like a pad (their motion sensors have no keys).
 */
static bool caps_are_pad(const unsigned long *keybits, const unsigned long *absbits, int nkeys)
{
	if (TEST_BIT(BTN_GAMEPAD, keybits) || TEST_BIT(BTN_JOYSTICK, keybits) ||
	    TEST_BIT(BTN_TRIGGER_HAPPY1, keybits))
		return true;
	return TEST_BIT(ABS_X, absbits) && nkeys >= 4 && !TEST_BIT(KEY_A, keybits) &&
	       !TEST_BIT(BTN_TOUCH, keybits) && !TEST_BIT(ABS_MT_SLOT, absbits) &&
	       !TEST_BIT(ABS_MT_POSITION_X, absbits);
}

/* A mapping without the left stick (skipped in the wizard, or a profile
 * made before the stick worked there): ABS_X / ABS_Y, as for unknown pads */
static void default_left_stick(struct dev *d, const unsigned long *absbits)
{
	for (int a = 0; a < ABS_CNT; a++)
		if (d->ax[a].kind == AX_STICK && d->ax[a].a == 0)
			return;
	if (!TEST_BIT(ABS_X, absbits) || !TEST_BIT(ABS_Y, absbits) ||
	    d->ax[ABS_X].kind != AX_NONE || d->ax[ABS_Y].kind != AX_NONE)
		return;
	d->ax[ABS_X] = (struct axmap){ AX_STICK, 0, 0, false, false };
	d->ax[ABS_Y] = (struct axmap){ AX_STICK, 0, 1, false, false };
}

static void resolve_mapping(struct input *in, struct dev *d, const unsigned long *keybits,
			    const unsigned long *absbits)
{
	char line[4096], p[512];
	char *s;

	map_reset(d);
	make_sdl_indices(d, keybits, absbits);
	make_guid(d);
	d->configured = true;
	if (d->builtin || d->power_only) {
		map_gamepad_spec(d, absbits);
		d->source = IN_MAP_BUILTIN;
		return;
	}
	if (d->keyboard) {
		map_keyboard(d);
		d->key_map[KEY_POWER] = T_POWER;
		d->source = IN_MAP_KEYBOARD;
		return;
	}
	/* hats always drive the d-pad unless a mapping says otherwise */
	d->ax[ABS_HAT0X] = (struct axmap){ AX_HAT, 0, 0, false, false };
	d->ax[ABS_HAT0Y] = (struct axmap){ AX_HAT, 1, 0, false, false };
	/* 2. user mapping */
	snprintf(p, sizeof(p), "%s/%s.cfg", in->cfg.user_map_dir, d->guid);
	s = file_read(p, NULL);
	if (s) {
		const char *m = s;

		/* skip "guid,name," */
		for (int k = 0; k < 2 && m; k++) {
			m = strchr(m, ',');
			if (m)
				m++;
		}
		if (m && apply_sdl_mapping(d, m) > 0) {
			free(s);
			default_left_stick(d, absbits);
			d->source = IN_MAP_USER;
			return;
		}
		free(s);
	}
	/* 3. a pad the kernel lays out its own way */
	{
		const struct pad_profile *pf = find_profile(d, keybits);

		if (pf) {
			map_reset(d);
			map_gamepad_spec(d, absbits);    /* sticks and hats */
			if (pf->keys) {
				for (int c = 0; c < KEY_CNT; c++)
					if (d->key_map[c] >= 0 && d->key_map[c] < IN_NUM_BUTTONS)
						d->key_map[c] = T_NONE;
				d->key_map[BTN_MODE] = T_NONE;
				for (const struct profile_key *k = pf->keys; k->code; k++)
					d->key_map[k->code] = k->t;
			}
			d->type = pf->type;
			d->source = IN_MAP_PROFILE;
			return;
		}
	}
	/* 3b. a driver that follows the Linux standard (Xbox, PlayStation,
	 * Switch, Steam pads) */
	if (driver_follows_spec(d->driver) && TEST_BIT(BTN_GAMEPAD, keybits)) {
		map_reset(d);
		map_gamepad_spec(d, absbits);
		if (!strcmp(d->driver, "hid-steam")) {
			/* its left touch pad reports where it is touched: no d-pad
			 * (the pad's clicks are BTN_DPAD_*) */
			d->ax[ABS_HAT0X] = (struct axmap){ 0 };
			d->ax[ABS_HAT0Y] = (struct axmap){ 0 };
		}
		d->source = IN_MAP_PROFILE;
		return;
	}
	/* 4. SDL GameControllerDB */
	if (in->cfg.gcdb_path && find_gcdb(in, d, line, sizeof(line), in->cfg.gcdb_path, false)) {
		char *m = line, *name = strchr(line, ',');

		for (int k = 0; k < 2 && m; k++) {
			m = strchr(m, ',');
			if (m)
				m++;
		}
		map_reset(d);
		if (name && m) {
			m[-1] = 0;
			d->type = type_from_name(name + 1);
			m[-1] = ',';
		}
		if (m && apply_sdl_mapping(d, m) > 0) {
			default_left_stick(d, absbits);
			d->source = IN_MAP_GCDB;
			return;
		}
	}
	/* 5. Linux gamepad spec */
	map_reset(d);
	if (TEST_BIT(BTN_GAMEPAD, keybits)) {
		map_gamepad_spec(d, absbits);
		d->source = IN_MAP_GAMEPAD_SPEC;
		return;
	}
	/* 6. unknown: hats and sticks still navigate; buttons need the wizard */
	d->ax[ABS_X] = (struct axmap){ AX_STICK, 0, 0, false, false };
	d->ax[ABS_Y] = (struct axmap){ AX_STICK, 0, 1, false, false };
	d->ax[ABS_HAT0X] = (struct axmap){ AX_HAT, 0, 0, false, false };
	d->ax[ABS_HAT0Y] = (struct axmap){ AX_HAT, 1, 0, false, false };
	d->configured = false;
	d->source = IN_MAP_NONE;
}

/* ------------------------------------------------------------- ports */
static struct pad *port_pad(const struct input *in, int port)
{
	int pd;

	if (port < 0 || port >= INPUT_MAX_PORTS)
		return NULL;
	pd = in->port_dev[port];
	if (pd == -1)
		return in->builtin_present ? (struct pad *)&in->builtin : NULL;
	if (pd >= 0 && in->devs[pd])
		return &in->devs[pd]->own;
	return NULL;
}

/*
 * Device id for input_last_source_id() / input_set_p1_device():
 * "builtin", or "<evdev path>|<SDL guid>". The path tells two identical
 * pads apart; the guid alone still matches after a re-plug renumbered the
 * node.
 */
int input_ports_resolve_id(const char *id, const struct input_port_dev *devs, int n, bool builtin_present)
{
	const char *bar;
	int by_guid = -3, seq = 0;

	if (!id || !*id)
		return -3;
	if (!strcmp(id, "builtin"))
		return builtin_present ? -1 : -3;
	bar = strchr(id, '|');
	for (int i = 0; i < n; i++) {
		const struct input_port_dev *d = &devs[i];

		if (bar && !strncmp(id, d->path, (size_t)(bar - id)) && d->path[bar - id] == 0 &&
		    !strcmp(bar + 1, d->guid))
			return d->slot;
		if (bar && !strcmp(bar + 1, d->guid) && (by_guid == -3 || d->seq < seq)) {
			by_guid = d->slot;
			seq = d->seq;
		}
	}
	return by_guid;
}

/*
 * Player assignment (docs/input-design.md §3). ports[]: -2 none, -1 the
 * built-in pad, else a device slot. Full: `forced` (the launching
 * controller) first, then the built-in pad, then the external pads in
 * connection order; without a forced device the policy decides whether
 * external pads come first (ext_first). Incremental (hot-plug during a
 * game): devices keep their ports, a missing one is removed and the ports
 * above it move down (P1 is never left empty while a pad is connected),
 * and a new device takes the next free port, never P1's.
 */
void input_ports_assign(int *ports, bool incremental, bool builtin_present,
			const struct input_port_dev *ext, int n_ext, int forced, bool ext_first)
{
	int order[INPUT_MAX_DEVICES], n = 0, p = 0;
	int out[INPUT_MAX_PORTS];

	for (int i = 0; i < n_ext && i < INPUT_MAX_DEVICES; i++)
		order[n++] = i;
	for (int i = 1; i < n; i++)          /* connection order */
		for (int j = i; j > 0 && ext[order[j]].seq < ext[order[j - 1]].seq; j--) {
			int t = order[j];

			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	for (int i = 0; i < INPUT_MAX_PORTS; i++)
		out[i] = -2;
	if (incremental) {
		/* keep the devices still present, in their order */
		for (int i = 0; i < INPUT_MAX_PORTS; i++) {
			bool present = ports[i] == -1 ? builtin_present : false;

			for (int k = 0; k < n && ports[i] >= 0; k++)
				present |= ext[k].slot == ports[i];
			if (ports[i] != -2 && present)
				out[p++] = ports[i];
		}
		/* newcomers take the next free ports */
		if (builtin_present) {
			bool placed = false;

			for (int i = 0; i < p; i++)
				placed |= out[i] == -1;
			if (!placed && p < INPUT_MAX_PORTS)
				out[p++] = -1;
		}
		for (int k = 0; k < n && p < INPUT_MAX_PORTS; k++) {
			bool placed = false;

			for (int i = 0; i < p; i++)
				placed |= out[i] == ext[order[k]].slot;
			if (!placed)
				out[p++] = ext[order[k]].slot;
		}
	} else {
		if (forced != -3)
			out[p++] = forced;
		if (forced == -3 && ext_first && n > 0) {
			for (int k = 0; k < n && p < INPUT_MAX_PORTS; k++)
				out[p++] = ext[order[k]].slot;
			if (builtin_present && p < INPUT_MAX_PORTS)
				out[p++] = -1;
		} else {
			if (builtin_present && forced != -1 && p < INPUT_MAX_PORTS)
				out[p++] = -1;
			for (int k = 0; k < n && p < INPUT_MAX_PORTS; k++)
				if (ext[order[k]].slot != forced)
					out[p++] = ext[order[k]].slot;
		}
	}
	memcpy(ports, out, sizeof(out));
}

static int ext_devices(const struct input *in, struct input_port_dev *ext)
{
	int n = 0;

	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		struct dev *d = in->devs[i];

		if (d && !d->builtin && !d->keyboard && !d->power_only)
			ext[n++] = (struct input_port_dev){ i, d->seq, d->path, d->guid };
	}
	return n;
}

static void assign_ports_mode(struct input *in, bool incremental)
{
	struct input_port_dev ext[INPUT_MAX_DEVICES];
	int old[INPUT_MAX_PORTS];
	int n = ext_devices(in, ext), forced = -3;
	bool ext_first;

	memcpy(old, in->port_dev, sizeof(old));
	if (in->p1 == INPUT_P1_AUTO && in->p1_id[0])
		forced = input_ports_resolve_id(in->p1_id, ext, n, in->builtin_present);
	ext_first = n > 0 && (in->p1 == INPUT_P1_EXTERNAL || (in->p1 == INPUT_P1_AUTO && in->docked));
	input_ports_assign(in->port_dev, incremental, in->builtin_present, ext, n, forced, ext_first);
	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i])
			in->devs[i]->port = -1;
	for (int i = 0; i < INPUT_MAX_PORTS; i++)
		if (in->port_dev[i] >= 0)
			in->devs[in->port_dev[i]]->port = i;
	if (memcmp(old, in->port_dev, sizeof(old)))
		push_hk(in, IN_HK_PORTS_CHANGED);
}

/* Policy, docked or launching-controller changes: full assignment. */
static void assign_ports(struct input *in)
{
	assign_ports_mode(in, false);
}

/* A pad appeared or went away: during a game the ports stay stable. */
static void assign_ports_hotplug(struct input *in)
{
	assign_ports_mode(in, in->mode == INPUT_MODE_GAME);
}

void input_set_p1_device(struct input *in, const char *id)
{
	strlcpy_(in->p1_id, id ? id : "", sizeof(in->p1_id));
	assign_ports(in);
}

const char *input_last_source_id(const struct input *in)
{
	return in->last_src_id;
}

void input_set_docked(struct input *in, bool docked)
{
	in->docked = docked;
	assign_ports(in);
}

void input_set_p1_policy(struct input *in, enum input_p1_policy p)
{
	in->p1 = p;
	assign_ports(in);
}

static uint16_t pad_buttons(const struct pad *p)
{
	uint16_t b = p->axis_btns;

	for (int i = 0; i < IN_NUM_BUTTONS; i++)
		if (p->cnt[i])
			b |= BTN_MASK(i);
	return b;
}

void input_set_mode(struct input *in, enum input_mode mode)
{
	/*
	 * Back to the game from a menu (the in-game menu, the game switcher):
	 * the buttons still held there belong to the menu. The A that chose
	 * "Resume", the B or Start that closed it, reach the core only once
	 * released and pressed again, as a hotkey's buttons do (issue #10).
	 */
	if (in->mode == INPUT_MODE_UI && mode == INPUT_MODE_GAME) {
		in->builtin.suppress |= pad_buttons(&in->builtin);
		for (int i = 0; i < INPUT_MAX_DEVICES; i++)
			if (in->devs[i])
				in->devs[i]->own.suppress |= pad_buttons(&in->devs[i]->own);
	}
	in->mode = mode;
	in->nav_head = in->nav_tail = 0;
}

uint16_t input_port_buttons(const struct input *in, int port)
{
	const struct pad *p = port_pad(in, port);
	uint16_t raw, out = 0;

	if (!p)
		return 0;
	raw = pad_buttons(p) & (uint16_t)~p->suppress;
	if (in->dpad_analog[port])
		raw &= (uint16_t)~DPAD_MASK;
	for (int i = 0; i < IN_NUM_BUTTONS; i++)
		if ((raw & BTN_MASK(i)) && in->remap[i] < IN_NUM_BUTTONS)
			out |= BTN_MASK(in->remap[i]);
	return out;
}

/* The external device of a port, NULL for the built-in pad or none. */
static const struct dev *port_device(const struct input *in, int port)
{
	int pd = port >= 0 && port < INPUT_MAX_PORTS ? in->port_dev[port] : -2;

	return pd >= 0 && pd < INPUT_MAX_DEVICES ? in->devs[pd] : NULL;
}

/*
 * N64 pad: its C buttons (RetroPad X, A, L3, R3) also push the right
 * stick, full deflection, as the N64 core reads C buttons there.
 */
static int16_t n64_c_stick(uint16_t b, int axis)
{
	if (axis == 0)
		return (int16_t)((b & BTN_MASK(IN_L3)) ? -0x7fff : (b & BTN_MASK(IN_R3)) ? 0x7fff : 0);
	return (int16_t)((b & BTN_MASK(IN_X)) ? -0x7fff : (b & BTN_MASK(IN_A)) ? 0x7fff : 0);
}

/*
 * The menu's view of a pad's buttons. N64 and GameCube pads confirm with A
 * and go back with B (in the games they stay positional: A on RetroPad B,
 * B on RetroPad Y); the button on RetroPad A (C-down, GameCube X) is Y, and
 * an N64's Z is Select (the menu's options button).
 */
static uint16_t ui_buttons(const struct dev *d, uint16_t b)
{
	uint16_t o;

	if (!d || (d->type != IN_PAD_N64 && d->type != IN_PAD_GC))
		return b;
	o = b & (uint16_t)~(BTN_MASK(IN_A) | BTN_MASK(IN_B) | BTN_MASK(IN_Y));
	if (b & BTN_MASK(IN_B)) o |= BTN_MASK(IN_A);
	if (b & BTN_MASK(IN_Y)) o |= BTN_MASK(IN_B);
	if (b & BTN_MASK(IN_A)) o |= BTN_MASK(IN_Y);
	if (d->type == IN_PAD_N64 && (b & BTN_MASK(IN_L2))) {
		o &= (uint16_t)~BTN_MASK(IN_L2);
		o |= BTN_MASK(IN_SELECT);
	}
	return o;
}

int16_t input_port_analog(const struct input *in, int port, int stick, int axis)
{
	const struct pad *p = port_pad(in, port);
	const struct dev *d = port_device(in, port);

	if (!p || stick < 0 || stick > 1 || axis < 0 || axis > 1)
		return 0;
	if (stick == 1 && d && d->type == IN_PAD_N64) {
		int16_t c = n64_c_stick(pad_buttons(p) & (uint16_t)~p->suppress, axis);

		if (c)
			return c;
	}
	if (stick == 0 && in->dpad_analog[port]) {
		uint16_t b = pad_buttons(p);

		if (axis == 0)
			return (int16_t)((b & BTN_MASK(IN_LEFT)) ? -0x7fff :
					 (b & BTN_MASK(IN_RIGHT)) ? 0x7fff : 0);
		return (int16_t)((b & BTN_MASK(IN_UP)) ? -0x7fff :
				 (b & BTN_MASK(IN_DOWN)) ? 0x7fff : 0);
	}
	return p->analog[stick][axis];
}

int16_t input_port_analog_button(const struct input *in, int port, int btn)
{
	const struct pad *p = port_pad(in, port);
	uint16_t b;

	if (!p)
		return 0;
	if (btn == IN_L2 && p->trig[0])
		return p->trig[0];
	if (btn == IN_R2 && p->trig[1])
		return p->trig[1];
	b = input_port_buttons(in, port);
	return (int16_t)((b & BTN_MASK(btn)) ? 0x7fff : 0);
}

bool input_port_has_analog(const struct input *in, int port)
{
	const struct pad *p = port_pad(in, port);

	if (!p)
		return false;
	if (port >= 0 && in->port_dev[port] == -1)
		return in->stick_fitted;
	return p->has_analog;
}

void input_port_info(const struct input *in, int port, struct input_port_info *out)
{
	memset(out, 0, sizeof(*out));
	out->device = -2;
	if (port < 0 || port >= INPUT_MAX_PORTS || in->port_dev[port] == -2)
		return;
	out->connected = port_pad(in, port) != NULL;
	out->device = in->port_dev[port];
	out->has_analog = input_port_has_analog(in, port);
	if (out->device == -1) {
		strlcpy_(out->name, in->cfg.builtin_name, sizeof(out->name));
		out->source = IN_MAP_BUILTIN;
	} else {
		strlcpy_(out->name, in->devs[out->device]->name, sizeof(out->name));
		out->source = in->devs[out->device]->source;
	}
}

void input_set_dpad_to_analog(struct input *in, int port, bool on)
{
	if (port >= 0 && port < INPUT_MAX_PORTS)
		in->dpad_analog[port] = on;
}

/* ---------------------------------------------------------------- rumble */
bool input_rumble_mix(uint16_t strong, uint16_t weak, uint16_t *mag_strong, uint16_t *mag_weak)
{
	/* the libretro strengths are the FF_RUMBLE magnitudes (both 0..0xffff):
	 * the strong motor is the big, low-frequency one on every pad driver */
	*mag_strong = strong;
	*mag_weak = weak;
	return strong || weak;
}

static struct dev *port_ff_dev(const struct input *in, int port)
{
	int slot;

	if (port < 0 || port >= INPUT_MAX_PORTS)
		return NULL;
	slot = in->port_dev[port];
	if (slot < 0 || slot >= INPUT_MAX_DEVICES || !in->devs[slot] || !in->devs[slot]->ff_rumble)
		return NULL;
	return in->devs[slot];
}

bool input_port_has_rumble(const struct input *in, int port)
{
	return port_ff_dev(in, port) != NULL;
}

/* Uploads (or updates) the pad's effect and plays or stops it. */
static bool ff_apply(struct dev *d)
{
	struct ff_effect e;
	struct input_event ev;
	uint16_t ms, mw;
	bool play = input_rumble_mix(d->ff_strength[0], d->ff_strength[1], &ms, &mw);

	if (!play) {
		if (d->ff_id < 0 || !d->ff_playing)
			return true;
		memset(&ev, 0, sizeof(ev));
		ev.type = EV_FF;
		ev.code = (uint16_t)d->ff_id;
		ev.value = 0;
		d->ff_playing = false;
		return write(d->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev);
	}
	memset(&e, 0, sizeof(e));
	e.type = FF_RUMBLE;
	e.id = (int16_t)d->ff_id;          /* -1: a new effect */
	e.u.rumble.strong_magnitude = ms;
	e.u.rumble.weak_magnitude = mw;
	/* the core's state holds until it changes: the longest replay (65 s),
	 * started again at every change */
	e.replay.length = 0xffff;
	e.replay.delay = 0;
	if (ioctl(d->fd, EVIOCSFF, &e) < 0) {
		ui_log_once(d->path, "input: %s: rumble upload refused (%s)", d->name, strerror(errno));
		if (errno == ENOSPC || errno == EINVAL)
			d->ff_rumble = false;   /* the pad cannot take our effect: stop trying */
		return false;
	}
	if (d->ff_id < 0)
		LOGI("input: %s: rumble effect %d uploaded", d->name, e.id);
	d->ff_id = e.id;
	memset(&ev, 0, sizeof(ev));
	ev.type = EV_FF;
	ev.code = (uint16_t)d->ff_id;
	ev.value = 1;
	d->ff_playing = true;
	return write(d->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev);
}

bool input_rumble(struct input *in, int port, int effect, uint16_t strength)
{
	struct dev *d = port_ff_dev(in, port);

	if (!d || in->rumble_off || effect < 0 || effect > 1)
		return false;
	if (d->ff_strength[effect] == strength && (d->ff_playing || !strength))
		return true;             /* cores repeat the same state every frame */
	d->ff_strength[effect] = strength;
	return ff_apply(d);
}

void input_set_rumble(struct input *in, bool enabled)
{
	in->rumble_off = !enabled;
	if (enabled)
		return;
	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		struct dev *d = in->devs[i];

		if (d && d->ff_rumble && d->ff_playing) {
			d->ff_strength[0] = d->ff_strength[1] = 0;
			ff_apply(d);
		}
	}
}

/* ---------------------------------------------------------------- remap */
void input_clear_remap(struct input *in)
{
	for (int i = 0; i < IN_NUM_BUTTONS; i++)
		in->remap[i] = (uint8_t)i;
}

static int remap_key(const char *k)
{
	for (int i = 0; i < IN_NUM_BUTTONS; i++)
		if (str_ieq(k, g_remap_keys[i]))
			return i;
	if (str_ieq(k, "l1"))
		return IN_L;
	if (str_ieq(k, "r1"))
		return IN_R;
	return -1;
}

static bool load_remap_file(struct input *in, const char *path)
{
	char *s = file_read(path, NULL), *save = NULL, *line;
	bool in_remap = true;

	if (!s)
		return false;
	input_clear_remap(in);
	for (int p = 0; p < INPUT_MAX_PORTS; p++)
		in->dpad_analog[p] = false;
	for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *eq, *k, *v, *c = strchr(line, ';');

		if (c)
			*c = 0;
		c = strchr(line, '#');
		if (c)
			*c = 0;
		line = str_trim(line);
		if (line[0] == '[') {
			in_remap = !strncmp(line, "[remap]", 7);
			if (!strncmp(line, "[options]", 9))
				in_remap = false;
			continue;
		}
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		k = str_trim(line);
		v = str_trim(eq + 1);
		if (in_remap) {
			int from = remap_key(k), to = remap_key(v);

			if (from < 0) {
				LOGW("remap %s: unknown button \"%s\"", path, k);
				continue;
			}
			if (to < 0 && !str_ieq(v, "none")) {
				LOGW("remap %s: unknown target \"%s\"", path, v);
				continue;
			}
			in->remap[from] = to < 0 ? 0xff : (uint8_t)to;
		} else if (!strcmp(k, "dpad_to_analog")) {
			bool on = parse_bool(v, false);

			for (int p = 0; p < INPUT_MAX_PORTS; p++)
				if (!input_port_has_analog(in, p))
					in->dpad_analog[p] = on;
		}
	}
	free(s);
	return true;
}

const char *input_load_remap(struct input *in, const char *system, const char *game)
{
	static char used[1024];
	char cand[6][1024];
	int n = 0;
	const char *suffix[2] = { in->cz ? "-cz" : NULL, "" };

	input_clear_remap(in);
	if (!system || !*system)
		return NULL;
	/* most specific first; at each level the -cz variant wins when the
	 * C/Z buttons are fitted */
	if (game && *game)
		for (int s = 0; s < 2; s++)
			if (suffix[s])
				snprintf(cand[n++], sizeof(cand[0]), "%s/%s%s/%s.ini",
					 in->cfg.remap_user_dir, system, suffix[s], game);
	for (int s = 0; s < 2; s++)
		if (suffix[s])
			snprintf(cand[n++], sizeof(cand[0]), "%s/%s%s.ini",
				 in->cfg.remap_user_dir, system, suffix[s]);
	for (int s = 0; s < 2; s++)
		if (suffix[s])
			snprintf(cand[n++], sizeof(cand[0]), "%s/%s%s.ini",
				 in->cfg.remap_sys_dir, system, suffix[s]);
	for (int i = 0; i < n; i++) {
		if (load_remap_file(in, cand[i])) {
			strlcpy_(used, cand[i], sizeof(used));
			LOGI("input: remap %s", used);
			return used;
		}
	}
	return NULL;
}

void input_set_cz_buttons(struct input *in, bool fitted)
{
	in->cz = fitted;
}

/* --------------------------------------------------------------- devices */
static void pad_release_all(struct pad *p)
{
	memset(p->cnt, 0, sizeof(p->cnt));
	p->axis_btns = 0;
	p->suppress = 0;
	memset(p->analog, 0, sizeof(p->analog));
	memset(p->trig, 0, sizeof(p->trig));
	memset(p->stick_nav, 0, sizeof(p->stick_nav));
}

static void dev_close(struct input *in, int slot, bool notify)
{
	struct dev *d = in->devs[slot];

	if (!d)
		return;
	epoll_ctl(in->epfd, EPOLL_CTL_DEL, d->fd, NULL);
	close(d->fd);
	LOGI("input: %s removed (%s)", d->name, d->path);
	if (d->pad == &in->builtin) {
		pad_release_all(&in->builtin); /* drop keys held on it */
		for (int i = 0; i < INPUT_MAX_DEVICES; i++)
			if (in->devs[i] && in->devs[i]->pad == &in->builtin)
				memset(in->devs[i]->down, 0, sizeof(in->devs[i]->down));
	}
	if (notify && !d->builtin && !d->keyboard && !d->power_only) {
		strlcpy_(in->last_pad, d->name, sizeof(in->last_pad));
		push_hk(in, IN_HK_PAD_DISCONNECTED);
	}
	free(d);
	in->devs[slot] = NULL;
	if (in->cap_slot == slot)
		in->cap_slot = -1;
	/* builtin presence */
	in->builtin_present = false;
	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i] && in->devs[i]->builtin)
			in->builtin_present = true;
	if (in->last_src == slot)
		in->last_src = -3;
	assign_ports_hotplug(in);
}

static int16_t scale_axis(const struct input_absinfo *ai, int v, bool invert)
{
	int lo = ai->minimum, hi = ai->maximum;
	int center = (lo + hi) / 2, half = (hi - lo) / 2;
	int r;

	if (half <= 0)
		return 0;
	if (abs(v - center) <= ai->flat)
		return 0;
	r = (int)((long)(v - center) * 32767 / half);
	r = CLAMP(r, -32767, 32767);
	return (int16_t)(invert ? -r : r);
}

static int16_t scale_trigger(const struct input_absinfo *ai, int v, bool invert, bool half)
{
	int lo = ai->minimum, hi = ai->maximum, r;

	if (hi <= lo)
		return 0;
	if (half) {
		lo = (lo + hi) / 2;
		if (v < lo)
			v = lo;
	}
	r = (int)((long)(v - lo) * 32767 / (hi - lo));
	r = CLAMP(r, 0, 32767);
	return (int16_t)(invert ? 32767 - r : r);
}

static void update_stick_nav(struct pad *p)
{
	/* hysteresis: on at 50 %, off at 35 % */
	int x = p->analog[0][0], y = p->analog[0][1];
	const int on = 16384, off = 11468;

	p->stick_nav[0] = y < -(p->stick_nav[0] ? off : on);
	p->stick_nav[1] = y > (p->stick_nav[1] ? off : on);
	p->stick_nav[2] = x < -(p->stick_nav[2] ? off : on);
	p->stick_nav[3] = x > (p->stick_nav[3] ? off : on);
}

static void handle_abs(struct input *in, struct dev *d, int code, int v)
{
	struct axmap *m = &d->ax[code];
	struct pad *p = d->pad;
	const struct input_absinfo *ai = &d->absinfo[code];

	if (d->builtin_stick) {
		/* adc-joystick: out of 0..3000 (or of the absinfo range)
		 * means the add-on stick is not fitted. */
		int lo = ai->maximum > ai->minimum ? ai->minimum : 0;
		int hi = ai->maximum > ai->minimum ? ai->maximum : 3000;
		bool in_range = v >= lo && v <= hi;

		if (code == ABS_X || code == ABS_Y)
			in->stick_raw[code == ABS_Y] = v;
		if (!in_range) {
			if (in->stick_fitted)
				LOGI("input: analog stick reads %d, treated as not fitted", v);
			in->stick_fitted = false;
			memset(p->analog[0], 0, sizeof(p->analog[0]));
			update_stick_nav(p);
			return;
		}
		if (!in->stick_fitted) {
			/* Fitted only once both axes read near the centre: a stick
			 * at rest is centred, while the floating inputs of a unit
			 * without one can wander into the range near an edge,
			 * which would hold a direction (issue #10). */
			int ox = in->stick_raw[0], oy = in->stick_raw[1];
			int mid = lo + (hi - lo) / 2, near = (hi - lo) / 4;

			if (abs(ox - mid) <= near && abs(oy - mid) <= near) {
				in->stick_fitted = true;
				p->has_analog = true;
				LOGI("input: analog stick detected (x %d, y %d)", ox, oy);
			}
		}
		if (!in->stick_fitted)
			return;
	}
	switch (m->kind) {
	case AX_STICK:
		p->analog[m->a][m->b] = scale_axis(ai, v, m->invert);
		if (m->a == 0)
			update_stick_nav(p);
		break;
	case AX_TRIGGER: {
		int16_t t = scale_trigger(ai, v, m->invert, m->half);
		int idx = m->a == IN_R2 ? 1 : 0;

		p->trig[idx] = t;
		if (m->a < 0 || m->a >= IN_NUM_BUTTONS)
			break;
		if (t > 16384)
			p->axis_btns |= BTN_MASK(m->a);
		else if (t < 8192)
			p->axis_btns &= (uint16_t)~BTN_MASK(m->a);
		break;
	}
	case AX_BUTTONS: {
		int c = (ai->minimum + ai->maximum) / 2, half = (ai->maximum - ai->minimum) / 2;
		bool neg = half > 0 ? v < c - half / 2 : v < 0;
		bool pos = half > 0 ? v > c + half / 2 : v > 0;

		/* targets past the pad's buttons ("guide" on a hat or an axis):
		 * nothing (review F-L11: BTN_MASK(35) was 1u << 35) */
		if (m->a >= IN_NUM_BUTTONS)
			m->a = -1;
		if (m->b >= IN_NUM_BUTTONS)
			m->b = -1;
		if (m->a >= 0) {
			if (neg)
				p->axis_btns |= BTN_MASK(m->a);
			else
				p->axis_btns &= (uint16_t)~BTN_MASK(m->a);
		}
		if (m->b >= 0) {
			if (pos)
				p->axis_btns |= BTN_MASK(m->b);
			else
				p->axis_btns &= (uint16_t)~BTN_MASK(m->b);
		}
		break;
	}
	case AX_HAT: {
		int neg = m->a == 0 ? IN_LEFT : IN_UP, pos = m->a == 0 ? IN_RIGHT : IN_DOWN;

		p->axis_btns &= (uint16_t)~(BTN_MASK(neg) | BTN_MASK(pos));
		if (v < 0)
			p->axis_btns |= BTN_MASK(neg);
		else if (v > 0)
			p->axis_btns |= BTN_MASK(pos);
		break;
	}
	default:
		break;
	}
}

static void capture_push(struct input *in, const char *elem)
{
	int next = (in->cap_head + 1) % CAP_QUEUE;

	if (next == in->cap_tail)
		return;
	strlcpy_(in->capq[in->cap_head], elem, sizeof(in->capq[0]));
	in->cap_head = next;
}

static void capture_event(struct input *in, struct dev *d, const struct input_event *ev)
{
	char e[16];

	if (ev->type == EV_KEY && ev->value == 1 && ev->code < KEY_CNT &&
	    d->btn_index[ev->code] >= 0) {
		snprintf(e, sizeof(e), "b%d", d->btn_index[ev->code]);
		capture_push(in, e);
	} else if (ev->type == EV_ABS && ev->code >= ABS_HAT0X && ev->code <= ABS_HAT3Y) {
		int h = (ev->code - ABS_HAT0X) / 2;
		bool y = (ev->code - ABS_HAT0X) & 1;
		int mask = ev->value == 0 ? 0 : y ? (ev->value < 0 ? 1 : 4) : (ev->value < 0 ? 8 : 2);

		if (mask && d->hat_index[h] >= 0) {
			snprintf(e, sizeof(e), "h%d.%d", d->hat_index[h], mask);
			capture_push(in, e);
		}
	} else if (ev->type == EV_ABS && ev->code < ABS_CNT && d->axis_index[ev->code] >= 0) {
		const struct input_absinfo *ai = &d->absinfo[ev->code];
		int range = ai->maximum - ai->minimum;
		int rest = d->abs_rest[ev->code];
		int delta = ev->value - rest;
		uint8_t st = 0;

		if (range <= 0)
			return;
		/* a third of the range: a centred 8-bit stick (0..255, rest 128)
		 * goes only 127 from rest, never past half the range */
		if (abs(delta) > range / 3)
			st = delta < 0 ? 1 : 2;
		else if (abs(delta) < range / 6)
			st = 0;
		else
			return;
		if (st && st != d->axis_btn_state[ev->code]) {
			snprintf(e, sizeof(e), "%ca%d", st == 1 ? '-' : '+', d->axis_index[ev->code]);
			capture_push(in, e);
		}
		d->axis_btn_state[ev->code] = st;
	}
}

static bool any_down(const struct dev *d)
{
	for (size_t i = 0; i < ARRAY_SIZE(d->down); i++)
		if (d->down[i])
			return true;
	return false;
}

static void handle_key(struct input *in, struct dev *d, int code, int value)
{
	int t = code < KEY_CNT ? d->key_map[code] : T_NONE;
	struct pad *p = d->pad;

	if (value == 2)
		return; /* kernel autorepeat: we do our own */
	/*
	 * A keyboard is no player in a game (it has no port), so its Esc opens
	 * the in-game menu (Resume, ..., Exit) instead of being B (issue #6: a
	 * keyboard alone could not leave a game). In the menus (UI mode) Esc
	 * stays B. A release of an Esc pressed in a menu (counted down) goes
	 * the normal way, so B never stays held.
	 */
	if (d->keyboard && code == KEY_ESC && in->mode == INPUT_MODE_GAME && !TEST_BIT(code, d->down)) {
		if (value == 1)
			push_hk(in, IN_HK_MENU);
		return;
	}
	switch (t) {
	case T_NONE:
		return;
	case T_GUIDE:
		/* Home / Guide / PS: the in-game menu; held, the quit combination */
		d->guide = value != 0;
		if (value == 1 && in->mode == INPUT_MODE_GAME)
			push_hk(in, IN_HK_MENU);
		return;
	case T_BRIGHT_UP:
	case T_BRIGHT_DOWN:
		if (value == 1 && !in->docked)
			brightness_step(in, t == T_BRIGHT_UP ? 1 : -1);
		return;
	case T_POWER:
		if (!in->cfg.handle_power_key)
			return;
		if (value == 1) {
			in->power_down_at = ui_now_ms();
			in->power_fired = false;
		} else if (in->power_down_at) {
			if (!in->power_fired)
				push_hk(in, IN_HK_POWER_SHORT);
			in->power_down_at = 0;
		}
		return;
	default:
		break;
	}
	if (t < 0 || t >= IN_NUM_BUTTONS)
		return;
	/* per key of this device: a press it already counted (the event the
	 * kernel keeps after an overflow, also in EVIOCGKEY) counts once */
	if (value) {
		int64_t now;

		if (TEST_BIT(code, d->down))
			return;
		now = ui_now_ms();
		if (!any_down(d))
			d->held_check_at = now + HELD_CHECK_MS;
		d->down[code / BITS_PER_LONG_] |= 1ul << (code % BITS_PER_LONG_);
		d->press_at[t] = now;
		d->long_logged &= (uint16_t)~BTN_MASK(t);
		if (p->cnt[t] < 255)
			p->cnt[t]++;
	} else {
		if (!TEST_BIT(code, d->down))
			return;
		d->down[code / BITS_PER_LONG_] &= ~(1ul << (code % BITS_PER_LONG_));
		if (p->cnt[t])
			p->cnt[t]--;
	}
}

/* The device's mapped keys from the kernel's key state. Returns the number
 * of keys that changed, -1 if the state cannot be read. Only this device's
 * keys move, so the merged built-in pad stays right, and a press already
 * counted is not counted twice. */
static int dev_sync_keys(struct input *in, struct dev *d, bool log_changes)
{
	unsigned long keys[NBITS(KEY_CNT)];
	int changed = 0;

	memset(keys, 0, sizeof(keys));
	if (ioctl(d->fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
		return -1;
	for (int c = 0; c < KEY_CNT; c++) {
		int t = d->key_map[c];
		bool now = TEST_BIT(c, keys) != 0, was = TEST_BIT(c, d->down) != 0;

		if (t >= 0 && t < IN_NUM_BUTTONS && now != was) {
			if (log_changes)
				LOGW("input: %s: %s (key %d) %s without an event: state corrected", d->name,
				     g_btn_names[t], c, now ? "pressed" : "released");
			handle_key(in, d, c, now);
			changed++;
		}
	}
	return changed;
}

/*
 * Issue #10 (buttons acting as held): while this layer has a key of the
 * device down, the kernel's key state is compared once a second. A release
 * that never arrived is applied, and a key the kernel itself reports down
 * for HELD_LOG_MS is logged once (a stuck or shorted button shows in
 * frontend.log / game.log). Called after the read loop has drained every
 * device: EVIOCGKEY drops the key events still queued, and the state it
 * returns already includes them.
 */
static void check_held(struct input *in, int64_t now)
{
	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		struct dev *d = in->devs[i];

		if (!d || !any_down(d) || now < d->held_check_at)
			continue;
		d->held_check_at = now + HELD_CHECK_MS;   /* also when skipped: no busy timeout */
		if (in->cap_slot == i || d->dropping || dev_sync_keys(in, d, true) < 0)
			continue;
		for (int c = 0; c < KEY_CNT; c++) {
			int t;

			if (!TEST_BIT(c, d->down))
				continue;
			t = d->key_map[c];
			if (t < 0 || t >= IN_NUM_BUTTONS || (d->long_logged & BTN_MASK(t)) ||
			    now - d->press_at[t] < HELD_LOG_MS)
				continue;
			d->long_logged |= BTN_MASK(t);
			LOGI("input: %s: %s (key %d) held for %lld s (the kernel reports it down)", d->name,
			     g_btn_names[t], c, (long long)(now - d->press_at[t]) / 1000);
		}
	}
}

/*
 * After SYN_DROPPED (evdev buffer overflow) and the SYN_REPORT that ends
 * it: the device's real key and axis state (review F-M14). The kernel
 * keeps the newest event after an overflow, and EVIOCGKEY includes it.
 */
static void dev_resync(struct input *in, struct dev *d)
{
	if (dev_sync_keys(in, d, false) < 0)
		return;
	for (int a = 0; a < ABS_CNT; a++) {
		struct input_absinfo ai;

		if (d->ax[a].kind != AX_NONE && ioctl(d->fd, EVIOCGABS(a), &ai) == 0)
			handle_abs(in, d, a, ai.value);
	}
	LOGI("input: %s: events dropped by the kernel, state re-read", d->name);
}

/* The board's built-in buttons: evdev name starting with builtin_prefix
 * ("RetroStone2" on the RetroStone2; "" = the board has none). */
static bool name_is_builtin(const struct input *in, const char *name)
{
	const char *p = in->cfg.builtin_prefix;

	return p && *p && !strncmp(name, p, strlen(p));
}

static bool name_is_builtin_stick(const struct input *in, const char *name)
{
	const char *s = in->cfg.builtin_stick;

	return s && *s && !strcmp(name, s);
}

static int dev_open(struct input *in, const char *node)
{
	char path[64];
	int fd, slot = -1;
	struct dev *d;
	unsigned long evbits[NBITS(EV_CNT)] = { 0 };
	unsigned long keybits[NBITS(KEY_CNT)] = { 0 };
	unsigned long absbits[NBITS(ABS_CNT)] = { 0 };
	int nkeys = 0;

	snprintf(path, sizeof(path), "%s/%s", in->cfg.dev_dir, node);
	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		if (in->devs[i] && !strcmp(in->devs[i]->path, path))
			return i;
		if (!in->devs[i] && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return -ENOSPC;
	/* read-write: rumble effects are uploaded and played through the fd
	 * (EVIOCSFF, EV_FF writes); read-only when that is refused */
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS))
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	d = xcalloc(1, sizeof(*d));
	d->fd = fd;
	d->slot = slot;
	d->port = -1;
	d->ff_id = -1;
	strlcpy_(d->path, path, sizeof(d->path));
	ioctl(fd, EVIOCGNAME(sizeof(d->name) - 1), d->name);
	ioctl(fd, EVIOCGID, &d->id);
	read_driver(path, d->driver, sizeof(d->driver));
	ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
	if (TEST_BIT(EV_KEY, evbits))
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	if (TEST_BIT(EV_ABS, evbits))
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	if (TEST_BIT(EV_FF, evbits)) {
		unsigned long ffbits[NBITS(FF_CNT)] = { 0 };
		int flags = fcntl(fd, F_GETFL);

		ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits);
		d->ff_rumble = TEST_BIT(FF_RUMBLE, ffbits) && flags >= 0 && (flags & O_ACCMODE) == O_RDWR;
	}
	for (int c = 0; c < KEY_CNT; c++)
		if (TEST_BIT(c, keybits))
			nkeys++;

	d->builtin = name_is_builtin(in, d->name);
	d->builtin_stick = name_is_builtin_stick(in, d->name);
	if (d->builtin_stick)
		d->builtin = true;
	if (!d->builtin) {
		if (caps_are_pad(keybits, absbits, nkeys)) {
			/* external gamepad */
		} else if (TEST_BIT(KEY_ENTER, keybits) && TEST_BIT(KEY_A, keybits)) {
			d->keyboard = true;
		} else if (TEST_BIT(KEY_POWER, keybits) && nkeys <= 4 && in->cfg.handle_power_key) {
			d->power_only = true;
		} else {
			/* not an input we use (lid switch, IR, touch, ...) */
			close(fd);
			free(d);
			return -ENODEV;
		}
	}
	for (int a = 0; a < ABS_CNT; a++) {
		if (TEST_BIT(a, absbits)) {
			ioctl(fd, EVIOCGABS(a), &d->absinfo[a]);
			d->abs_rest[a] = d->absinfo[a].value;
		}
	}
	resolve_mapping(in, d, keybits, absbits);
	d->pad = d->builtin ? &in->builtin : &d->own;
	d->own.has_analog = !d->builtin && TEST_BIT(ABS_X, absbits) && TEST_BIT(ABS_Y, absbits) &&
			    d->ax[ABS_X].kind == AX_STICK;
	d->seq = ++in->seq;
	in->devs[slot] = d;
	{
		struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (uint32_t)slot };

		epoll_ctl(in->epfd, EPOLL_CTL_ADD, fd, &ev);
	}
	LOGI("input: %s \"%s\" %04x:%04x (%s) guid %s -> %s%s%s%s", path, d->name, d->id.vendor,
	     d->id.product, d->driver[0] ? d->driver : "?", d->guid,
	     d->builtin ? "built-in" : d->keyboard ? "keyboard" : d->power_only ? "power key" :
	     d->source == IN_MAP_USER ? "user mapping" : d->source == IN_MAP_GCDB ? "gamecontrollerdb" :
	     d->source == IN_MAP_PROFILE ? "built-in profile" :
	     d->source == IN_MAP_GAMEPAD_SPEC ? "gamepad spec" : "UNCONFIGURED",
	     d->type == IN_PAD_N64 ? " (N64)" : d->type == IN_PAD_GC ? " (GameCube)" : "",
	     d->builtin_stick ? " (analog stick)" : "", d->ff_rumble ? " (rumble)" : "");
	if (d->builtin) {
		in->builtin_present = true;
		/* initial stick state */
		if (d->builtin_stick) {
			in->stick_raw[0] = d->absinfo[ABS_X].value;
			in->stick_raw[1] = d->absinfo[ABS_Y].value;
			handle_abs(in, d, ABS_X, d->absinfo[ABS_X].value);
			handle_abs(in, d, ABS_Y, d->absinfo[ABS_Y].value);
		}
	} else if (!d->keyboard && !d->power_only) {
		strlcpy_(in->last_pad, d->name, sizeof(in->last_pad));
		push_hk(in, d->configured ? IN_HK_PAD_CONNECTED : IN_HK_PAD_UNCONFIGURED);
	}
	assign_ports_hotplug(in);
	return slot;
}

/*
 * Devices this layer never uses, known by name from sysfs without opening
 * them (an open() runs the driver's open callback): the PMIC power key when
 * the power module owns it, and audio jack switches (EV_SW only). Only for
 * the real /dev/input; anything unknown is opened and probed as before.
 */
static bool skip_by_name(const struct input *in, const char *node)
{
	char p[320], name[96] = "";
	FILE *f;
	size_t l;

	if (strcmp(in->cfg.dev_dir, "/dev/input"))
		return false;
	snprintf(p, sizeof(p), "/sys/class/input/%s/device/name", node);
	f = fopen(p, "re");
	if (!f)
		return false;
	if (!fgets(name, sizeof(name), f))
		name[0] = 0;
	fclose(f);
	name[strcspn(name, "\n")] = 0;
	l = strlen(name);
	if (in->cfg.power_key_name && *in->cfg.power_key_name &&
	    !strcmp(name, in->cfg.power_key_name) && !in->cfg.handle_power_key)
		return true;
	return l > 5 && !strcmp(name + l - 5, " Jack");
}

static void scan_dir(struct input *in)
{
	DIR *dir = opendir(in->cfg.dev_dir);
	struct dirent *de;

	if (!dir) {
		LOGW("input: cannot open %s", in->cfg.dev_dir);
		return;
	}
	while ((de = readdir(dir))) {
		int64_t t0;
		int r;

		if (strncmp(de->d_name, "event", 5) || skip_by_name(in, de->d_name))
			continue;
		t0 = ui_now_us();
		r = dev_open(in, de->d_name);
		t0 = ui_now_us() - t0;
		/* boot instrumentation: a driver whose open() is slow */
		if (t0 > 10000)
			LOGI("input: opening %s took %lld ms (%s)", de->d_name, (long long)t0 / 1000,
			     r >= 0 ? "used" : strerror(-r));
	}
	closedir(dir);
}

struct input *input_open(const struct input_config *cfg)
{
	struct input *in = xcalloc(1, sizeof(*in));
	struct input_config def;

	input_config_defaults(&def);
	in->cfg = cfg ? *cfg : def;
#define DEF(f) if (!in->cfg.f) in->cfg.f = def.f
	DEF(dev_dir);
	DEF(user_map_dir);
	DEF(remap_sys_dir);
	DEF(remap_user_dir);
	DEF(backlight_dir);
	DEF(repeat_delay_ms);
	DEF(repeat_rate_ms);
	DEF(power_long_ms);
	DEF(builtin_prefix);
	DEF(builtin_stick);
	DEF(builtin_name);
	DEF(power_key_name);
#undef DEF
	in->cap_slot = -1;
	in->last_src = -3;
	for (int i = 0; i < INPUT_MAX_PORTS; i++)
		in->port_dev[i] = -2;
	input_clear_remap(in);
	in->epfd = epoll_create1(EPOLL_CLOEXEC);
	in->infd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (in->infd >= 0) {
		struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 0xffffffffu };

		if (inotify_add_watch(in->infd, in->cfg.dev_dir, IN_CREATE | IN_ATTRIB | IN_DELETE) < 0)
			LOGW("input: inotify on %s: %s", in->cfg.dev_dir, strerror(errno));
		epoll_ctl(in->epfd, EPOLL_CTL_ADD, in->infd, &ev);
	}
	backlight_find(in);
	scan_dir(in);
	/* nothing queued for devices present at start */
	in->hk_head = in->hk_tail = 0;
	assign_ports(in);
	in->hk_head = in->hk_tail = 0;
	return in;
}

void input_close(struct input *in)
{
	if (!in)
		return;
	/* the kernel erases a file's effects when it is closed; stop the
	 * motors first all the same (a game that ends while rumbling) */
	input_set_rumble(in, false);
	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i]) {
			close(in->devs[i]->fd);
			free(in->devs[i]);
		}
	if (in->infd >= 0)
		close(in->infd);
	close(in->epfd);
	free(in);
}

int input_fd(const struct input *in)
{
	return in->epfd;
}

/* ----------------------------------------------------------------- polling */
static void handle_inotify(struct input *in)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;

	while ((len = read(in->infd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + len;) {
			struct inotify_event *ev = (struct inotify_event *)p;

			if (ev->len && !strncmp(ev->name, "event", 5)) {
				if (ev->mask & IN_DELETE) {
					char path[64];

					snprintf(path, sizeof(path), "%s/%s", in->cfg.dev_dir, ev->name);
					for (int i = 0; i < INPUT_MAX_DEVICES; i++)
						if (in->devs[i] && !strcmp(in->devs[i]->path, path))
							dev_close(in, i, true);
				} else {
					int r = dev_open(in, ev->name);

					if (r == -EACCES || r == -ENOENT || r == -EBUSY) {
						/* retry a bit later */
						for (int k = 0; k < 8; k++) {
							if (!in->pending[k][0]) {
								strlcpy_(in->pending[k], ev->name, sizeof(in->pending[k]));
								in->pending_at[k] = ui_now_ms() + 100;
								break;
							}
						}
					}
				}
			}
			p += sizeof(*ev) + ev->len;
		}
	}
}

/* Hotkeys: Select held on the built-in pad or on player 1. */
static void hotkeys(struct input *in, struct pad *p, uint16_t before, uint16_t now)
{
	static const struct { int btn; enum input_hotkey hk; } combos[] = {
		{ IN_START, IN_HK_EXIT }, { IN_R, IN_HK_SAVE_STATE },
		{ IN_L, IN_HK_LOAD_STATE }, { IN_RIGHT, IN_HK_SLOT_NEXT },
		{ IN_LEFT, IN_HK_SLOT_PREV }, { IN_X, IN_HK_MENU }, { IN_B, IN_HK_RESET },
		/* batch 2 (docs/input-design.md §4) */
		{ IN_R2, IN_HK_FAST_FORWARD }, { IN_L2, IN_HK_SCREENSHOT }, { IN_Y, IN_HK_SWITCHER },
	};
	uint16_t pressed = now & (uint16_t)~before;

	/* release suppression once the button is up */
	p->suppress &= now;
	if (in->mode != INPUT_MODE_GAME || !(now & BTN_MASK(IN_SELECT)))
		return;
	for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
		if (pressed & BTN_MASK(combos[i].btn)) {
			p->suppress |= BTN_MASK(combos[i].btn);
			push_hk(in, combos[i].hk);
		}
	}
}

/*
 * A pad without Select (an N64 pad) has no hotkey button: Z + Start held
 * for a second opens the in-game menu instead (Resume, save states, Quit).
 * A hold, not a press: holding Z (targeting, crouching) and pressing Start
 * to pause is ordinary play.
 */
#define COMBO_HOLD_MS 1000
/* Z + Start on the pads without Select (Z is L2 on an N64, R on a GameCube) */
static uint16_t pad_combo(enum input_pad_type t)
{
	return t == IN_PAD_N64 ? (uint16_t)(BTN_MASK(IN_L2) | BTN_MASK(IN_START)) :
	       t == IN_PAD_GC ? (uint16_t)(BTN_MASK(IN_R) | BTN_MASK(IN_START)) : 0;
}

static void combo_holds(struct input *in, int64_t now)
{
	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		struct dev *d = in->devs[i];
		struct pad *p;

		uint16_t combo;

		if (!d || !(combo = pad_combo(d->type)) || d->pad != &d->own)
			continue;
		p = &d->own;
		if (in->mode != INPUT_MODE_GAME || (pad_buttons(p) & combo) != combo) {
			p->combo_since = 0;
			p->combo_fired = false;
			continue;
		}
		if (!p->combo_since) {
			p->combo_since = now;
		} else if (!p->combo_fired && now - p->combo_since >= COMBO_HOLD_MS) {
			p->combo_fired = true;
			p->suppress |= combo;
			push_hk(in, IN_HK_MENU);
		}
	}
}

static uint16_t nav_bits(const struct pad *p)
{
	uint16_t b = pad_buttons(p);

	if (p->stick_nav[0]) b |= BTN_MASK(IN_UP);
	if (p->stick_nav[1]) b |= BTN_MASK(IN_DOWN);
	if (p->stick_nav[2]) b |= BTN_MASK(IN_LEFT);
	if (p->stick_nav[3]) b |= BTN_MASK(IN_RIGHT);
	return b;
}

static bool repeats(int b)
{
	return b == IN_UP || b == IN_DOWN || b == IN_LEFT || b == IN_RIGHT ||
	       b == IN_L || b == IN_R || b == IN_L2 || b == IN_R2;
}

static void update_nav(struct input *in, int64_t now)
{
	uint16_t all = 0;

	if (in->builtin_present)
		all |= nav_bits(&in->builtin);
	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i] && in->devs[i]->pad == &in->devs[i]->own && i != in->cap_slot)
			all |= ui_buttons(in->devs[i], nav_bits(&in->devs[i]->own));
	for (int b = 0; b < IN_NUM_BUTTONS; b++) {
		bool was = in->nav_state & BTN_MASK(b), is = all & BTN_MASK(b);

		if (is && !was) {
			push_nav(in, (enum input_btn)b, IN_NAV_PRESS, -1);
			in->nav_next[b] = now + in->cfg.repeat_delay_ms;
		} else if (!is && was) {
			push_nav(in, (enum input_btn)b, IN_NAV_RELEASE, -1);
		} else if (is && repeats(b) && now >= in->nav_next[b]) {
			push_nav(in, (enum input_btn)b, IN_NAV_REPEAT, -1);
			in->nav_next[b] += in->cfg.repeat_rate_ms;
			if (in->nav_next[b] < now)
				in->nav_next[b] = now + in->cfg.repeat_rate_ms;
		}
	}
	in->nav_state = all;
}

int input_poll(struct input *in)
{
	struct epoll_event evs[16];
	int n, handled = 0;
	int64_t now;
	uint16_t before[INPUT_MAX_DEVICES + 1];

	before[INPUT_MAX_DEVICES] = pad_buttons(&in->builtin);
	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		before[i] = in->devs[i] ? pad_buttons(&in->devs[i]->own) : 0;

	while ((n = epoll_wait(in->epfd, evs, 16, 0)) > 0) {
		for (int k = 0; k < n; k++) {
			uint32_t id = evs[k].data.u32;
			struct dev *d;
			struct input_event ie[64];
			ssize_t r;

			if (id == 0xffffffffu) {
				handle_inotify(in);
				continue;
			}
			d = id < INPUT_MAX_DEVICES ? in->devs[id] : NULL;
			if (!d)
				continue;
			while ((r = read(d->fd, ie, sizeof(ie))) > 0) {
				for (size_t e = 0; e < (size_t)r / sizeof(ie[0]); e++) {
					handled++;
					if (in->cap_slot == (int)id) {
						capture_event(in, d, &ie[e]);
						continue;
					}
					/* SYN_DROPPED: everything up to the next SYN_REPORT
					 * is incomplete; drop it, then read the state */
					if (ie[e].type == EV_SYN && ie[e].code == SYN_DROPPED) {
						d->dropping = true;
					} else if (d->dropping) {
						if (ie[e].type == EV_SYN && ie[e].code == SYN_REPORT) {
							d->dropping = false;
							dev_resync(in, d);
							/* The snapshot is newer than the rest of ie[].
							 * EVIOCGKEY also flushes queued kernel key events:
							 * replaying an old press here can stick a button
							 * whose release was just flushed. Discard the old
							 * in-memory events before reading again. */
							break;
						}
					} else if (ie[e].type == EV_KEY) {
						handle_key(in, d, ie[e].code, ie[e].value);
					} else if (ie[e].type == EV_ABS && ie[e].code < ABS_CNT) {
						handle_abs(in, d, ie[e].code, ie[e].value);
					}
				}
			}
			if (r < 0 && errno == ENODEV) {
				dev_close(in, (int)id, true);
				break; /* evs[] may reference the closed slot */
			}
		}
		if (n < 16)
			break;
	}
	now = ui_now_ms();
	check_held(in, now);
	/* pending hotplugged nodes */
	for (int k = 0; k < 8; k++) {
		if (in->pending[k][0] && now >= in->pending_at[k]) {
			int r = dev_open(in, in->pending[k]);

			if (r >= 0 || now > in->pending_at[k] + 2000 || r == -ENODEV)
				in->pending[k][0] = 0;
			else
				in->pending_at[k] = now + 100;
		}
	}
	/* power key long press */
	if (in->power_down_at && !in->power_fired &&
	    now - in->power_down_at >= in->cfg.power_long_ms) {
		in->power_fired = true;
		if (in->cfg.on_power_off)
			in->cfg.on_power_off(in->cfg.user);
		push_hk(in, IN_HK_POWER_OFF);
	}
	/* the device of the last new button press (the UI passes it to the
	 * game it launches: that controller becomes player 1) */
	{
		int src = -3;

		if (in->builtin_present && (pad_buttons(&in->builtin) & ~before[INPUT_MAX_DEVICES]))
			src = -1;
		for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
			struct dev *d = in->devs[i];

			if (d && d->pad == &d->own && !d->keyboard && !d->power_only &&
			    (pad_buttons(&d->own) & ~before[i]))
				src = i;
		}
		if (src != -3 && src != in->last_src) {
			in->last_src = src;
			if (src == -1)
				strlcpy_(in->last_src_id, "builtin", sizeof(in->last_src_id));
			else
				snprintf(in->last_src_id, sizeof(in->last_src_id), "%s|%s", in->devs[src]->path,
					 in->devs[src]->guid);
		}
	}
	/* a suppressed button (a hotkey's, or one held when a menu gave the
	 * pad back to the game) counts again once released, on every pad */
	in->builtin.suppress &= pad_buttons(&in->builtin);
	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i])
			in->devs[i]->own.suppress &= pad_buttons(&in->devs[i]->own);
	/* hotkeys: the built-in pad and player 1 */
	if (in->builtin_present)
		hotkeys(in, &in->builtin, before[INPUT_MAX_DEVICES], pad_buttons(&in->builtin));
	if (in->port_dev[0] >= 0 && in->devs[in->port_dev[0]]) {
		struct pad *p = &in->devs[in->port_dev[0]]->own;

		hotkeys(in, p, before[in->port_dev[0]], pad_buttons(p));
	}
	/* and any keyboard: Tab (Select) + Space (Start) exits, Tab + X opens
	 * the menu... (a keyboard has no port, so it never disturbs a player) */
	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i] && in->devs[i]->keyboard)
			hotkeys(in, &in->devs[i]->own, before[i], pad_buttons(&in->devs[i]->own));
	combo_holds(in, now);
	update_nav(in, now);
	return handled;
}

int input_timeout_ms(const struct input *in)
{
	int64_t now = ui_now_ms(), next = -1;

	for (int b = 0; b < IN_NUM_BUTTONS; b++)
		if ((in->nav_state & BTN_MASK(b)) && repeats(b))
			if (next < 0 || in->nav_next[b] < next)
				next = in->nav_next[b];
	if (in->power_down_at && !in->power_fired) {
		int64_t t = in->power_down_at + in->cfg.power_long_ms;

		if (next < 0 || t < next)
			next = t;
	}
	for (int k = 0; k < 8; k++)
		if (in->pending[k][0] && (next < 0 || in->pending_at[k] < next))
			next = in->pending_at[k];
	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		const struct dev *d = in->devs[i];

		if (d && any_down(d) && (next < 0 || d->held_check_at < next))
			next = d->held_check_at;   /* check_held() */
		if (d && d->own.combo_since && !d->own.combo_fired) {
			int64_t t = d->own.combo_since + COMBO_HOLD_MS;   /* combo_holds() */

			if (next < 0 || t < next)
				next = t;
		}
	}
	if (next < 0)
		return -1;
	return next <= now ? 0 : (int)(next - now);
}

/* -------------------------------------------------------------------- UI */
uint16_t input_any_buttons(const struct input *in)
{
	uint16_t b = in->builtin_present ? pad_buttons(&in->builtin) : 0;

	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i] && in->devs[i]->pad == &in->devs[i]->own)
			b |= pad_buttons(&in->devs[i]->own);
	return b;
}

bool input_builtin_stick(const struct input *in, int *x, int *y)
{
	*x = in->stick_raw[0];
	*y = in->stick_raw[1];
	return in->stick_fitted;
}

int input_device_count(const struct input *in)
{
	int n = 0;

	for (int i = 0; i < INPUT_MAX_DEVICES; i++)
		if (in->devs[i])
			n++;
	return n;
}

void input_device_info(const struct input *in, int slot, struct input_device_info *out)
{
	const struct dev *d;

	memset(out, 0, sizeof(*out));
	out->port = -1;
	if (slot < 0 || slot >= INPUT_MAX_DEVICES || !(d = in->devs[slot]))
		return;
	out->present = true;
	out->builtin = d->builtin;
	out->configured = d->configured;
	strlcpy_(out->name, d->name, sizeof(out->name));
	strlcpy_(out->guid, d->guid, sizeof(out->guid));
	strlcpy_(out->path, d->path, sizeof(out->path));
	out->source = d->source;
	out->type = d->type;
	out->port = d->port;
}

bool input_quit_combo_held(const struct input *in)
{
	const uint16_t sel = BTN_MASK(IN_SELECT) | BTN_MASK(IN_START);

	if (in->builtin_present && (pad_buttons(&in->builtin) & sel) == sel)
		return true;
	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		const struct dev *d = in->devs[i];
		uint16_t want = d && pad_combo(d->type) ? pad_combo(d->type) : sel;

		if (d && d->pad == &d->own && ((pad_buttons(&d->own) & want) == want || d->guide))
			return true;
	}
	return false;
}

/* The database name of a RetroPad id for a kind of pad (sdl_target() the
 * other way round); NULL for what SDL has no name for */
static const char *sdl_name(enum input_pad_type type, int t)
{
	static const struct { int b; const char *n; } names[] = {
		{ IN_B, "a" }, { IN_A, "b" }, { IN_Y, "x" }, { IN_X, "y" },
		{ IN_SELECT, "back" }, { IN_START, "start" }, { T_GUIDE, "guide" },
		{ IN_L, "leftshoulder" }, { IN_R, "rightshoulder" },
		{ IN_L2, "lefttrigger" }, { IN_R2, "righttrigger" },
		{ IN_L3, "leftstick" }, { IN_R3, "rightstick" },
		{ IN_UP, "dpup" }, { IN_DOWN, "dpdown" }, { IN_LEFT, "dpleft" }, { IN_RIGHT, "dpright" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(g_type_names); i++)
		if (g_type_names[i].type == type && g_type_names[i].b == t)
			return g_type_names[i].n;
	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (names[i].b == t)
			return names[i].n;
	return NULL;
}

static void put_elem(char *line, size_t n, size_t *l, const char *name, const char *src)
{
	if (name && *l < n)
		*l += (size_t)snprintf(line + *l, n - *l, "%s:%s,", name, src);
}

/*
 * One pad's mapping as an SDL database line, from what the menu resolved
 * (key_map, ax): whatever its source, a native game sees the pad as the
 * menu does. An N64 pad's R is also the right trigger (the libultraship
 * ports' default R) when it has no ZR.
 */
static size_t export_dev(const struct dev *d, char *line, size_t n)
{
	char src[24], rshoulder[24] = "", nbuf[160];
	bool rtrig = false;
	const char *name = d->name;
	size_t l;

	/* The menu knows an N64 pad when the pad does not say so (a USB
	 * adapter's "Generic USB Joystick"): games that choose their layout by
	 * the controller's name (GoldenEye) then find their N64 one. */
	if (d->type == IN_PAD_N64 && !strstr(d->name, "64")) {
		snprintf(nbuf, sizeof(nbuf), "N64 Controller (%s)", d->name);
		name = nbuf;
	}
	l = (size_t)snprintf(line, n, "%s,%s,", d->guid, name);

	for (int c = 0; c < KEY_CNT; c++) {
		int t = d->key_map[c];

		if (t == T_NONE || d->btn_index[c] < 0)
			continue;
		snprintf(src, sizeof(src), "b%d", d->btn_index[c]);
		put_elem(line, n, &l, sdl_name(d->type, t), src);
		if (t == IN_R2)
			rtrig = true;
		if (t == IN_R && !rshoulder[0])
			strlcpy_(rshoulder, src, sizeof(rshoulder));
	}
	for (int a = 0; a < ABS_CNT; a++) {
		const struct axmap *x = &d->ax[a];
		int idx = d->axis_index[a];

		if (idx < 0)
			continue;   /* (hats have no axis index) */
		switch (x->kind) {
		case AX_STICK:
			snprintf(src, sizeof(src), "a%d%s", idx, x->invert ? "~" : "");
			put_elem(line, n, &l, x->a == 0 ? (x->b == 0 ? "leftx" : "lefty") :
					      (x->b == 0 ? "rightx" : "righty"), src);
			break;
		case AX_TRIGGER:
			snprintf(src, sizeof(src), "%sa%d%s", x->half ? "+" : "", idx, x->invert ? "~" : "");
			put_elem(line, n, &l, sdl_name(d->type, x->a), src);
			if (x->a == IN_R2)
				rtrig = true;
			break;
		case AX_BUTTONS:
			snprintf(src, sizeof(src), "-a%d", idx);
			if (x->a >= 0)
				put_elem(line, n, &l, sdl_name(d->type, x->a), src);
			src[0] = '+';
			if (x->b >= 0)
				put_elem(line, n, &l, sdl_name(d->type, x->b), src);
			break;
		default:
			break;
		}
	}
	for (int h = 0; h < 4; h++) {
		/* 1 up, 2 right, 4 down, 8 left */
		static const struct { int axis, neg, pos; const char *nn, *pn; } dirs[2] = {
			{ 0, 8, 2, "dpleft", "dpright" }, { 1, 1, 4, "dpup", "dpdown" },
		};

		if (d->hat_index[h] < 0)
			continue;
		for (int k = 0; k < 2; k++) {
			const struct axmap *x = &d->ax[ABS_HAT0X + 2 * h + dirs[k].axis];

			if (x->kind == AX_HAT) {
				snprintf(src, sizeof(src), "h%d.%d", d->hat_index[h], dirs[k].neg);
				put_elem(line, n, &l, dirs[k].nn, src);
				snprintf(src, sizeof(src), "h%d.%d", d->hat_index[h], dirs[k].pos);
				put_elem(line, n, &l, dirs[k].pn, src);
			} else if (x->kind == AX_BUTTONS) {
				snprintf(src, sizeof(src), "h%d.%d", d->hat_index[h], dirs[k].neg);
				if (x->a >= 0)
					put_elem(line, n, &l, sdl_name(d->type, x->a), src);
				snprintf(src, sizeof(src), "h%d.%d", d->hat_index[h], dirs[k].pos);
				if (x->b >= 0)
					put_elem(line, n, &l, sdl_name(d->type, x->b), src);
			}
		}
	}
	if (d->type == IN_PAD_N64 && !rtrig && rshoulder[0])
		put_elem(line, n, &l, "righttrigger", rshoulder);
	if (l < n)
		l += (size_t)snprintf(line + l, n - l, "platform:Linux,\n");
	return l;
}

/*
 * A port's layout for an N64 pad: which SDL element the port reads for each
 * N64 function, so a real N64 pad does what the built-in buttons do in that
 * game, whatever the port's bindings were changed to. One "function = element"
 * per line (# comments), the functions being the ones the default N64 mapping
 * names below; "none" drops the element.
 */
static const struct { const char *fn, *def; } g_n64_fn[] = {
	{ "a", "a" }, { "b", "b" }, { "z", "lefttrigger" }, { "l", "leftshoulder" },
	{ "r", "rightshoulder" }, { "start", "start" },
	{ "cup", "-righty" }, { "cdown", "+righty" }, { "cleft", "-rightx" }, { "cright", "+rightx" },
	{ "dpup", "dpup" }, { "dpdown", "dpdown" }, { "dpleft", "dpleft" }, { "dpright", "dpright" },
};
#define N64_FN_COUNT ARRAY_SIZE(g_n64_fn)

struct n64_layout {
	bool has[N64_FN_COUNT];
	char to[N64_FN_COUNT][24];
};

static void layout_parse(struct n64_layout *lay, const char *text)
{
	memset(lay, 0, sizeof(*lay));
	while (text && *text) {
		const char *eol = strchr(text, '\n');
		size_t len = eol ? (size_t)(eol - text) : strlen(text);
		char ln[96], *eq, *k, *v;

		if (len >= sizeof(ln))
			len = sizeof(ln) - 1;
		memcpy(ln, text, len);
		ln[len] = 0;
		text = eol ? eol + 1 : text + strlen(text);
		if (ln[0] == '#' || !(eq = strchr(ln, '=')))
			continue;
		*eq = 0;
		k = ln;
		v = eq + 1;
		while (*k == ' ' || *k == '\t')
			k++;
		for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); *--e = 0)
			;
		while (*v == ' ' || *v == '\t')
			v++;
		for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); *--e = 0)
			;
		for (size_t i = 0; i < N64_FN_COUNT; i++)
			if (!strcmp(k, g_n64_fn[i].fn) && v[0] && strlen(v) < sizeof(lay->to[i])) {
				lay->has[i] = true;
				strlcpy_(lay->to[i], v, sizeof(lay->to[i]));
			}
	}
}

/* Renames the elements of one N64 mapping line (guid,name,elem:src,...,platform:Linux,)
 * by the layout; all at once, so two functions can swap. */
static size_t layout_apply(char *line, size_t cap, size_t len, const struct n64_layout *lay)
{
	char out[1536];
	size_t o = 0;
	const char *p = line, *end;
	char rsrc[24] = "";
	int commas = 0;
	size_t orig_len = len;

	if (len && line[len - 1] == '\n')
		len--;                 /* the newline goes back at the end */
	end = line + len;

	/* the source of the right shoulder: the right trigger on it is only the
	 * default's copy of R (an N64 pad has no ZR) */
	for (const char *t = line; t < end; ) {
		const char *c = memchr(t, ',', (size_t)(end - t));
		size_t tl = c ? (size_t)(c - t) : (size_t)(end - t);

		if (tl > 14 && !strncmp(t, "rightshoulder:", 14) && tl - 14 < sizeof(rsrc)) {
			memcpy(rsrc, t + 14, tl - 14);
			rsrc[tl - 14] = 0;
		}
		t = c ? c + 1 : end;
	}
	while (p < end) {
		const char *c = memchr(p, ',', (size_t)(end - p));
		size_t tl = c ? (size_t)(c - p) : (size_t)(end - p);
		char tok[64];
		const char *colon;
		bool keep = true;

		if (tl == 0 && !c)
			break;
		if (tl >= sizeof(tok))
			return orig_len;   /* odd line: leave it */
		memcpy(tok, p, tl);
		tok[tl] = 0;
		p = c ? c + 1 : end;
		if (commas++ < 2 || !(colon = strchr(tok, ':'))) {
			/* guid and name (the name may hold a comma-free text) and odd tokens */
			if (o + tl + 1 < sizeof(out)) {
				memcpy(out + o, tok, tl);
				o += tl;
				out[o++] = ',';
			}
			continue;
		}
		{
			char nm[32];
			size_t nl = (size_t)(colon - tok);
			const char *src = colon + 1;

			if (nl >= sizeof(nm))
				return orig_len;
			memcpy(nm, tok, nl);
			nm[nl] = 0;
			if (!strcmp(nm, "righttrigger") && rsrc[0] && !strcmp(src, rsrc)) {
				for (size_t i = 0; i < N64_FN_COUNT; i++)
					if (!strcmp(g_n64_fn[i].fn, "r") && lay->has[i])
						keep = false;   /* R is placed by the layout */
			}
			for (size_t i = 0; keep && i < N64_FN_COUNT; i++) {
				if (lay->has[i] && !strcmp(nm, g_n64_fn[i].def)) {
					if (!strcmp(lay->to[i], "none")) {
						keep = false;
					} else {
						strlcpy_(nm, lay->to[i], sizeof(nm));
					}
					break;
				}
			}
			if (keep) {
				int w = snprintf(out + o, sizeof(out) - o, "%s:%s,", nm, src);

				if (w < 0 || (size_t)w >= sizeof(out) - o)
					return orig_len;
				o += (size_t)w;
			}
		}
	}
	if (o + 2 >= cap || o + 2 >= sizeof(out))
		return orig_len;
	memcpy(line, out, o);
	line[o++] = '\n';
	line[o] = 0;
	return o;
}

size_t input_sdl_mappings_layout(const struct input *in, char *buf, size_t n, const char *layout)
{
	size_t len = 0;
	struct n64_layout lay;

	if (n)
		buf[0] = 0;
	if (layout)
		layout_parse(&lay, layout);
	for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
		const struct dev *d = in->devs[i];
		char line[1536];
		size_t l;

		if (!d || d->builtin || d->keyboard || d->power_only || !d->configured)
			continue;
		l = export_dev(d, line, sizeof(line));
		if (l >= sizeof(line) - 1)
			continue;   /* cut short: better none than a wrong one */
		if (layout && d->type == IN_PAD_N64)
			l = layout_apply(line, sizeof(line), l, &lay);
		if (len + l < n) {
			memcpy(buf + len, line, l + 1);
			len += l;
		}
	}
	return len;
}

size_t input_sdl_mappings(const struct input *in, char *buf, size_t n)
{
	return input_sdl_mappings_layout(in, buf, n, NULL);
}

/* --------------------------------------------------------------- capture */
void input_capture_begin(struct input *in, int slot)
{
	in->cap_slot = slot;
	in->cap_head = in->cap_tail = 0;
	if (slot >= 0 && slot < INPUT_MAX_DEVICES && in->devs[slot]) {
		struct dev *d = in->devs[slot];

		memset(d->axis_btn_state, 0, sizeof(d->axis_btn_state));
		for (int a = 0; a < ABS_CNT; a++)
			if (d->absinfo[a].maximum > d->absinfo[a].minimum) {
				struct input_absinfo ai;

				if (ioctl(d->fd, EVIOCGABS(a), &ai) == 0)
					d->abs_rest[a] = ai.value;
			}
		pad_release_all(&d->own);
		memset(d->down, 0, sizeof(d->down));
	}
}

bool input_capture_next(struct input *in, char *elem, int n)
{
	if (in->cap_tail == in->cap_head)
		return false;
	strlcpy_(elem, in->capq[in->cap_tail], (size_t)n);
	in->cap_tail = (in->cap_tail + 1) % CAP_QUEUE;
	return true;
}

void input_capture_end(struct input *in)
{
	in->cap_slot = -1;
}

/* The device's mapping again, after its user mapping changed */
static void reresolve(struct input *in, struct dev *d)
{
	unsigned long keybits[NBITS(KEY_CNT)] = { 0 };
	unsigned long absbits[NBITS(ABS_CNT)] = { 0 };

	ioctl(d->fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	ioctl(d->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	resolve_mapping(in, d, keybits, absbits);
	pad_release_all(&d->own);
	memset(d->down, 0, sizeof(d->down));   /* the key map changed */
	d->guide = false;
}

int input_save_user_mapping(struct input *in, int slot, const char *mapping)
{
	struct dev *d;
	char p[512], buf[4096];
	int r;

	if (slot < 0 || slot >= INPUT_MAX_DEVICES || !(d = in->devs[slot]))
		return -ENODEV;
	mkdir_p(in->cfg.user_map_dir);
	snprintf(p, sizeof(p), "%s/%s.cfg", in->cfg.user_map_dir, d->guid);
	snprintf(buf, sizeof(buf), "%s,%s,%s,platform:Linux,\n", d->guid, d->name, mapping);
	r = file_write_atomic(p, buf, strlen(buf));
	if (r < 0)
		return r;
	reresolve(in, d);
	LOGI("input: saved mapping for %s to %s", d->name, p);
	return 0;
}

int input_forget_user_mapping(struct input *in, int slot)
{
	struct dev *d;
	char p[512];

	if (slot < 0 || slot >= INPUT_MAX_DEVICES || !(d = in->devs[slot]))
		return -ENODEV;
	snprintf(p, sizeof(p), "%s/%s.cfg", in->cfg.user_map_dir, d->guid);
	if (unlink(p) < 0 && errno != ENOENT)
		return -errno;
	reresolve(in, d);
	LOGI("input: removed the mapping of %s (%s): %s", d->name, p,
	     d->source == IN_MAP_NONE ? "not configured" : "automatic again");
	return 0;
}
