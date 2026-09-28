/*
 * input.c - evdev input layer. See input.h and docs/input-design.md.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	/* rumble: one FF_RUMBLE effect (input_rumble) */
	bool ff_rumble;               /* EV_FF with FF_RUMBLE, and the fd is writable */
	int ff_id;                    /* uploaded effect id, -1 = none yet */
	uint16_t ff_strength[2];      /* strong, weak as the core asked */
	bool ff_playing;
};

#define NAV_QUEUE 64
#define HK_QUEUE 32
#define CAP_QUEUE 16

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

/* SDL target name -> RetroPad id / stick axis. */
static int sdl_target(const char *t, int *stick, int *axis)
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

	strlcpy_(buf, mapping, sizeof(buf));
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
		t = sdl_target(str_trim(tok), &stick, &axis);
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
			d->source = IN_MAP_USER;
			return;
		}
		free(s);
	}
	/* 3. SDL GameControllerDB */
	if (in->cfg.gcdb_path && find_gcdb(in, d, line, sizeof(line), in->cfg.gcdb_path, false)) {
		char *m = line;

		for (int k = 0; k < 2 && m; k++) {
			m = strchr(m, ',');
			if (m)
				m++;
		}
		map_reset(d);
		if (m && apply_sdl_mapping(d, m) > 0) {
			d->source = IN_MAP_GCDB;
			return;
		}
	}
	/* 4. Linux gamepad spec */
	map_reset(d);
	if (TEST_BIT(BTN_GAMEPAD, keybits)) {
		map_gamepad_spec(d, absbits);
		d->source = IN_MAP_GAMEPAD_SPEC;
		return;
	}
	/* 5. unknown: hats and sticks still navigate; buttons need the wizard */
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

void input_set_mode(struct input *in, enum input_mode mode)
{
	in->mode = mode;
	in->nav_head = in->nav_tail = 0;
}

static uint16_t pad_buttons(const struct pad *p)
{
	uint16_t b = p->axis_btns;

	for (int i = 0; i < IN_NUM_BUTTONS; i++)
		if (p->cnt[i])
			b |= BTN_MASK(i);
	return b;
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

int16_t input_port_analog(const struct input *in, int port, int stick, int axis)
{
	const struct pad *p = port_pad(in, port);

	if (!p || stick < 0 || stick > 1 || axis < 0 || axis > 1)
		return 0;
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
			int ox = in->stick_raw[0], oy = in->stick_raw[1];

			if (ox >= lo && ox <= hi && oy >= lo && oy <= hi) {
				in->stick_fitted = true;
				p->has_analog = true;
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
		if (abs(delta) > range / 2 || (abs(delta) > range / 3 &&
		    (rest == ai->minimum || rest == ai->maximum)))
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

static void handle_key(struct input *in, struct dev *d, int code, int value)
{
	int t = code < KEY_CNT ? d->key_map[code] : T_NONE;
	struct pad *p = d->pad;

	if (value == 2)
		return; /* kernel autorepeat: we do our own */
	switch (t) {
	case T_NONE:
	case T_GUIDE:
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
		if (TEST_BIT(code, d->down))
			return;
		d->down[code / BITS_PER_LONG_] |= 1ul << (code % BITS_PER_LONG_);
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

/*
 * After SYN_DROPPED (evdev buffer overflow) and the SYN_REPORT that ends
 * it: the device's real key and axis state (review F-M14). Only this
 * device's keys move, so the merged built-in pad is resynced too, and a
 * press already counted is not counted twice (the kernel keeps the newest
 * event after an overflow, and EVIOCGKEY includes it).
 */
static void dev_resync(struct input *in, struct dev *d)
{
	unsigned long keys[NBITS(KEY_CNT)];

	memset(keys, 0, sizeof(keys));
	if (ioctl(d->fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
		return;
	for (int c = 0; c < KEY_CNT; c++) {
		int t = d->key_map[c];
		bool now = TEST_BIT(c, keys) != 0, was = TEST_BIT(c, d->down) != 0;

		if (t >= 0 && t < IN_NUM_BUTTONS && now != was)
			handle_key(in, d, c, now);
	}
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
		bool pad = TEST_BIT(BTN_GAMEPAD, keybits) || TEST_BIT(BTN_JOYSTICK, keybits) ||
			   TEST_BIT(BTN_TRIGGER_HAPPY1, keybits) ||
			   (TEST_BIT(ABS_X, absbits) && nkeys >= 4 && !TEST_BIT(KEY_A, keybits));

		if (pad) {
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
	LOGI("input: %s \"%s\" %04x:%04x guid %s -> %s%s%s", path, d->name, d->id.vendor,
	     d->id.product, d->guid,
	     d->builtin ? "built-in" : d->keyboard ? "keyboard" : d->power_only ? "power key" :
	     d->source == IN_MAP_USER ? "user mapping" : d->source == IN_MAP_GCDB ? "gamecontrollerdb" :
	     d->source == IN_MAP_GAMEPAD_SPEC ? "gamepad spec" : "UNCONFIGURED",
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
			all |= nav_bits(&in->devs[i]->own);
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
	/* hotkeys: the built-in pad and player 1 */
	if (in->builtin_present)
		hotkeys(in, &in->builtin, before[INPUT_MAX_DEVICES], pad_buttons(&in->builtin));
	if (in->port_dev[0] >= 0 && in->devs[in->port_dev[0]]) {
		struct pad *p = &in->devs[in->port_dev[0]]->own;

		hotkeys(in, p, before[in->port_dev[0]], pad_buttons(p));
	}
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
	out->port = d->port;
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

int input_save_user_mapping(struct input *in, int slot, const char *mapping)
{
	struct dev *d;
	char p[512], buf[4096];
	unsigned long keybits[NBITS(KEY_CNT)] = { 0 };
	unsigned long absbits[NBITS(ABS_CNT)] = { 0 };
	int r;

	if (slot < 0 || slot >= INPUT_MAX_DEVICES || !(d = in->devs[slot]))
		return -ENODEV;
	mkdir_p(in->cfg.user_map_dir);
	snprintf(p, sizeof(p), "%s/%s.cfg", in->cfg.user_map_dir, d->guid);
	snprintf(buf, sizeof(buf), "%s,%s,%s,platform:Linux,\n", d->guid, d->name, mapping);
	r = file_write_atomic(p, buf, strlen(buf));
	if (r < 0)
		return r;
	ioctl(d->fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	ioctl(d->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	resolve_mapping(in, d, keybits, absbits);
	pad_release_all(&d->own);
	memset(d->down, 0, sizeof(d->down));   /* the key map changed */
	LOGI("input: saved mapping for %s to %s", d->name, p);
	return 0;
}
