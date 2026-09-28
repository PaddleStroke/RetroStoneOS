/*
 * host_input_evdev.c - TEMPORARY stand-alone input backend for the host,
 * used until the real input layer (src/input/input.c) exists. Bring-up
 * quality only:
 *  - every /dev/input/event* device found at start is merged into player 1
 *    (no hotplug, no GameControllerDB, no remaps);
 *  - gamepads by the Linux gamepad codes (positional: BTN_SOUTH = RetroPad
 *    B, BTN_EAST = A, ...), BTN_C/BTN_Z = L3/R3 as on the RetroStone2;
 *  - keyboard: arrows, X=A Z=B S=X A=Y Q=L W=R, Enter=Start, RShift=Select,
 *    Esc = exit, F1 = menu (debugging over a USB keyboard);
 *  - hotkeys: Select + Start/R/L/Right/Left/X/B, as in input-design.md;
 *  - power key: tap / >= 2 s long press;
 *  - UI navigation events with key repeat for the in-game menu.
 */
#include "host_input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "hutil.h"

#define MAXDEV 16
#define NAVQ 32
#define HKQ 16
#define REPEAT_DELAY 400
#define REPEAT_RATE 70
#define POWER_LONG_MS 2000

struct dev {
	int fd;
	char name[80];
	bool has_abs_xy, has_abs_rxy;
	struct input_absinfo ax[ABS_CNT > 0x40 ? 0x40 : ABS_CNT];
};

static struct {
	bool open;
	int epfd;
	struct dev dev[MAXDEV];
	int ndev;
	uint16_t phys;          /* physical RetroPad state (merged) */
	uint16_t consumed;      /* buttons eaten by a hotkey until released */
	int16_t axis[2][2];     /* [stick][axis] */
	bool has_analog;
	bool dpad_to_analog;
	bool game_mode;
	enum input_hotkey hkq[HKQ];
	int hk_head, hk_n;
	struct input_nav navq[NAVQ];
	int nav_head, nav_n;
	int repeat_btn;
	int64_t repeat_at;
	int64_t power_down_at;  /* 0 = released */
	bool power_fired;
	void (*on_power_off)(void *);
	void *user;
} E;

static void hk_push(enum input_hotkey hk)
{
	if (E.hk_n < HKQ)
		E.hkq[(E.hk_head + E.hk_n++) % HKQ] = hk;
}

static void nav_push(int btn, enum input_nav_type t)
{
	if (E.nav_n < NAVQ) {
		struct input_nav *n = &E.navq[(E.nav_head + E.nav_n++) % NAVQ];

		n->btn = (enum input_btn)btn;
		n->type = t;
		n->device = -1;
	}
}

static int key_to_btn(int code)
{
	switch (code) {
	case BTN_SOUTH: return IN_B;
	case BTN_EAST: return IN_A;
	case BTN_NORTH: return IN_X;
	case BTN_WEST: return IN_Y;
	case BTN_TL: return IN_L;
	case BTN_TR: return IN_R;
	case BTN_TL2: return IN_L2;
	case BTN_TR2: return IN_R2;
	case BTN_SELECT: return IN_SELECT;
	case BTN_START: return IN_START;
	case BTN_THUMBL: case BTN_C: return IN_L3;
	case BTN_THUMBR: case BTN_Z: return IN_R3;
	case BTN_DPAD_UP: case KEY_UP: return IN_UP;
	case BTN_DPAD_DOWN: case KEY_DOWN: return IN_DOWN;
	case BTN_DPAD_LEFT: case KEY_LEFT: return IN_LEFT;
	case BTN_DPAD_RIGHT: case KEY_RIGHT: return IN_RIGHT;
	case KEY_X: return IN_A;
	case KEY_Z: return IN_B;
	case KEY_S: return IN_X;
	case KEY_A: return IN_Y;
	case KEY_Q: return IN_L;
	case KEY_W: return IN_R;
	case KEY_ENTER: return IN_START;
	case KEY_RIGHTSHIFT: return IN_SELECT;
	default: return -1;
	}
}

static enum input_hotkey combo(int btn)
{
	switch (btn) {
	case IN_START: return IN_HK_EXIT;
	case IN_R: return IN_HK_SAVE_STATE;
	case IN_L: return IN_HK_LOAD_STATE;
	case IN_RIGHT: return IN_HK_SLOT_NEXT;
	case IN_LEFT: return IN_HK_SLOT_PREV;
	case IN_X: return IN_HK_MENU;
	case IN_B: return IN_HK_RESET;
	default: return IN_HK_NONE;
	}
}

static bool nav_btn(int b)
{
	return b == IN_UP || b == IN_DOWN || b == IN_LEFT || b == IN_RIGHT || b == IN_L || b == IN_R;
}

static void button(int b, bool down)
{
	uint16_t bit = (uint16_t)(1u << b);

	if (down) {
		if (E.phys & bit)
			return;
		E.phys |= bit;
		if (E.game_mode && b != IN_SELECT && (E.phys & (1u << IN_SELECT))) {
			enum input_hotkey hk = combo(b);

			if (hk != IN_HK_NONE) {
				hk_push(hk);
				E.consumed |= bit;
			}
		}
		if (!E.game_mode) {
			nav_push(b, IN_NAV_PRESS);
			if (nav_btn(b)) {
				E.repeat_btn = b;
				E.repeat_at = hnow_ms() + REPEAT_DELAY;
			}
		}
	} else {
		E.phys &= (uint16_t)~bit;
		E.consumed &= (uint16_t)~bit;
		if (!E.game_mode)
			nav_push(b, IN_NAV_RELEASE);
		if (E.repeat_btn == b)
			E.repeat_btn = -1;
	}
}

static int16_t scale_abs(const struct input_absinfo *ai, int v)
{
	int range = ai->maximum - ai->minimum;
	long c;

	if (range <= 0)
		return 0;
	c = ((long)(v - ai->minimum) * 65534L) / range - 32767L;
	if (c > -3276 && c < 3276)
		c = 0; /* 10 % dead zone */
	if (c > 32767)
		c = 32767;
	if (c < -32767)
		c = -32767;
	return (int16_t)c;
}

static void handle(struct dev *d, const struct input_event *ev)
{
	if (ev->type == EV_KEY) {
		int b;

		if (ev->code == KEY_POWER) {
			if (ev->value == 1) {
				E.power_down_at = hnow_ms();
				E.power_fired = false;
			} else if (ev->value == 0) {
				if (E.power_down_at && !E.power_fired)
					hk_push(IN_HK_POWER_SHORT);
				E.power_down_at = 0;
			}
			return;
		}
		if (ev->value == 1 && ev->code == KEY_ESC) {
			hk_push(IN_HK_EXIT);
			return;
		}
		if (ev->value == 1 && ev->code == KEY_F1) {
			hk_push(IN_HK_MENU);
			return;
		}
		b = key_to_btn(ev->code);
		if (b >= 0 && ev->value != 2)
			button(b, ev->value != 0);
	} else if (ev->type == EV_ABS) {
		int code = ev->code;

		if (code == ABS_HAT0X) {
			button(IN_LEFT, ev->value < 0);
			button(IN_RIGHT, ev->value > 0);
		} else if (code == ABS_HAT0Y) {
			button(IN_UP, ev->value < 0);
			button(IN_DOWN, ev->value > 0);
		} else if (code < (int)(sizeof(d->ax) / sizeof(d->ax[0]))) {
			int16_t v = scale_abs(&d->ax[code], ev->value);

			if (code == ABS_X)
				E.axis[0][0] = v;
			else if (code == ABS_Y)
				E.axis[0][1] = v;
			else if (code == ABS_RX)
				E.axis[1][0] = v;
			else if (code == ABS_RY)
				E.axis[1][1] = v;
		}
	}
}

#define BITS_LONG (sizeof(long) * 8)
#define TEST_BIT(b, a) (((a)[(b) / BITS_LONG] >> ((b) % BITS_LONG)) & 1)

int hin_open(bool enabled, void (*on_power_off)(void *user), void *user)
{
	DIR *dir;
	struct dirent *de;

	memset(&E, 0, sizeof(E));
	E.epfd = -1;
	E.repeat_btn = -1;
	E.game_mode = true;
	E.on_power_off = on_power_off;
	E.user = user;
	if (!enabled)
		return 0;
	E.epfd = epoll_create1(EPOLL_CLOEXEC);
	dir = opendir("/dev/input");
	if (!dir) {
		hlog(HLOG_WARN, "input: /dev/input: %s", strerror(errno));
		E.open = true;
		return 0;
	}
	while ((de = readdir(dir)) && E.ndev < MAXDEV) {
		char path[300];
		unsigned long absbits[(ABS_CNT + BITS_LONG - 1) / BITS_LONG] = { 0 };
		struct dev *d = &E.dev[E.ndev];
		struct epoll_event ev = { .events = EPOLLIN };

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		d->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (d->fd < 0)
			continue;
		ioctl(d->fd, EVIOCGNAME(sizeof(d->name) - 1), d->name);
		ioctl(d->fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
		for (int a = 0; a < (int)(sizeof(d->ax) / sizeof(d->ax[0])); a++)
			if (TEST_BIT(a, absbits))
				ioctl(d->fd, EVIOCGABS(a), &d->ax[a]);
		d->has_abs_xy = TEST_BIT(ABS_X, absbits) && TEST_BIT(ABS_Y, absbits) &&
				d->ax[ABS_X].maximum > d->ax[ABS_X].minimum;
		/* TODO(hw): the built-in "analog-stick" reads outside 0..3000
		 * when the add-on is not fitted (input-design.md 2). */
		if (d->has_abs_xy)
			E.has_analog = true;
		ev.data.u32 = (uint32_t)E.ndev;
		epoll_ctl(E.epfd, EPOLL_CTL_ADD, d->fd, &ev);
		hlog(HLOG_INFO, "input (fallback): %s \"%s\"%s", path, d->name,
		     d->has_abs_xy ? " [analog]" : "");
		E.ndev++;
	}
	closedir(dir);
	E.open = true;
	return 0;
}

void hin_close(void)
{
	for (int i = 0; i < E.ndev; i++)
		close(E.dev[i].fd);
	if (E.epfd >= 0)
		close(E.epfd);
	memset(&E, 0, sizeof(E));
	E.epfd = -1;
}

const char *hin_backend(void)
{
	return E.ndev ? "evdev fallback" : "none";
}

int hin_fd(void)
{
	return E.epfd;
}

int hin_timeout_ms(void)
{
	int64_t now = hnow_ms();
	int64_t t = INT64_MAX;

	if (!E.game_mode && E.repeat_btn >= 0)
		t = E.repeat_at - now;
	if (E.power_down_at && !E.power_fired) {
		int64_t p = E.power_down_at + POWER_LONG_MS - now;

		if (p < t)
			t = p;
	}
	if (t == INT64_MAX)
		return -1;
	return t < 0 ? 0 : (int)t;
}

void hin_poll(void)
{
	int64_t now;

	for (int i = 0; i < E.ndev; i++) {
		struct input_event ev[64];
		ssize_t r;

		while ((r = read(E.dev[i].fd, ev, sizeof(ev))) > 0)
			for (int k = 0; k < (int)(r / (ssize_t)sizeof(ev[0])); k++)
				handle(&E.dev[i], &ev[k]);
	}
	now = hnow_ms();
	if (E.power_down_at && !E.power_fired && now - E.power_down_at >= POWER_LONG_MS) {
		E.power_fired = true;
		if (E.on_power_off)
			E.on_power_off(E.user);
		hk_push(IN_HK_POWER_OFF);
	}
	if (!E.game_mode && E.repeat_btn >= 0 && now >= E.repeat_at) {
		nav_push(E.repeat_btn, IN_NAV_REPEAT);
		E.repeat_at = now + REPEAT_RATE;
	}
}

void hin_set_game_mode(bool game)
{
	E.game_mode = game;
	E.repeat_btn = -1;
	E.nav_n = 0;
	/* Buttons held across the switch must be released before they count. */
	E.consumed = E.phys;
}

void hin_set_docked(bool docked)
{
	(void)docked;
}

void hin_set_p1_policy(enum input_p1_policy p)
{
	(void)p; /* one merged player 1 in the fallback */
}

void hin_set_p1_device(const char *id)
{
	(void)id; /* one merged player 1 in the fallback */
}

void hin_set_cz_buttons(bool fitted)
{
	(void)fitted; /* no remaps in the fallback */
}

bool hin_next_hotkey(enum input_hotkey *hk)
{
	if (!E.hk_n)
		return false;
	*hk = E.hkq[E.hk_head];
	E.hk_head = (E.hk_head + 1) % HKQ;
	E.hk_n--;
	return true;
}

bool hin_next_nav(struct input_nav *ev)
{
	if (!E.nav_n)
		return false;
	*ev = E.navq[E.nav_head];
	E.nav_head = (E.nav_head + 1) % NAVQ;
	E.nav_n--;
	return true;
}

const char *hin_last_pad(void)
{
	return "";
}

uint16_t hin_buttons(int port)
{
	uint16_t b;

	if (port != 0)
		return 0;
	b = E.phys & (uint16_t)~E.consumed;
	if (E.dpad_to_analog)
		b &= (uint16_t)~((1u << IN_UP) | (1u << IN_DOWN) | (1u << IN_LEFT) | (1u << IN_RIGHT));
	return b;
}

int16_t hin_analog(int port, int stick, int axis)
{
	if (port != 0 || stick < 0 || stick > 1 || axis < 0 || axis > 1)
		return 0;
	if (E.dpad_to_analog && stick == 0) {
		uint16_t p = E.phys & (uint16_t)~E.consumed;

		if (axis == 0)
			return (int16_t)(((p >> IN_RIGHT) & 1) * 32767 - ((p >> IN_LEFT) & 1) * 32767);
		return (int16_t)(((p >> IN_DOWN) & 1) * 32767 - ((p >> IN_UP) & 1) * 32767);
	}
	return E.axis[stick][axis];
}

int16_t hin_analog_button(int port, int btn)
{
	return (hin_buttons(port) >> btn) & 1 ? 0x7fff : 0;
}

bool hin_has_analog(int port)
{
	return port == 0 && E.has_analog;
}

void hin_port_info(int port, struct input_port_info *out)
{
	memset(out, 0, sizeof(*out));
	out->device = -1;
	out->source = IN_MAP_GAMEPAD_SPEC;
	if (port == 0 && E.open) {
		out->connected = true;
		out->has_analog = E.has_analog;
		hstrlcpy(out->name, E.ndev ? E.dev[0].name : "(no input device)", sizeof(out->name));
	}
}

void hin_set_dpad_to_analog(int port, bool on)
{
	if (port == 0)
		E.dpad_to_analog = on;
}

const char *hin_load_remap(const char *system, const char *game)
{
	(void)system;
	(void)game;
	return NULL;
}

bool hin_rumble(int port, int effect, uint16_t strength)
{
	(void)port;
	(void)effect;
	(void)strength;
	return false;
}

void hin_set_rumble(bool enabled)
{
	(void)enabled;
}
