/* Exercise the real polling loop with a pipe as evdev and a state-query
 * mock. EVIOCGKEY flushes queued kernel key events, but cannot flush old
 * events already copied into input_poll's read buffer. No hardware needed. */
#include <stdarg.h>
#include <sys/ioctl.h>

static int state_ioctl(int fd, unsigned long request, ...);
#define ioctl state_ioctl
#include "../src/input/input.c"
#undef ioctl

static bool kernel_held;
static int queries, failures;

static int state_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	unsigned long *keys;

	(void)fd;
	if (request != EVIOCGKEY(sizeof(unsigned long) * NBITS(KEY_CNT))) {
		errno = ENOTTY;
		return -1;
	}
	va_start(ap, request);
	keys = va_arg(ap, unsigned long *);
	va_end(ap);
	memset(keys, 0, sizeof(unsigned long) * NBITS(KEY_CNT));
	if (kernel_held)
		keys[BTN_EAST / BITS_PER_LONG_] |= 1ul << (BTN_EAST % BITS_PER_LONG_);
	queries++;
	return 0;
}

#define CHECK(c, label) do { printf("  %s: %s\n", (c) ? "ok" : "FAIL", label); \
	if (!(c)) failures++; } while (0)

static void overflow(bool held)
{
	struct input in = { 0 };
	struct dev d = { 0 }, shoulder = { 0 };
	struct input_event events[] = {
		{ .type = EV_SYN, .code = SYN_DROPPED },
		{ .type = EV_KEY, .code = BTN_SOUTH, .value = 1 }, /* incomplete frame */
		{ .type = EV_SYN, .code = SYN_REPORT },
		{ .type = EV_KEY, .code = BTN_EAST, .value = !held }, /* older than ioctl */
		{ .type = EV_SYN, .code = SYN_REPORT },
	};
	struct epoll_event watch = { .events = EPOLLIN, .data.u32 = 0 };
	int fds[2];

	if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0 || (in.epfd = epoll_create1(EPOLL_CLOEXEC)) < 0)
		exit(2);
	input_config_defaults(&in.cfg);
	in.cap_slot = -1;
	in.last_src = -3;
	in.builtin_present = true;
	for (int p = 0; p < INPUT_MAX_PORTS; p++) in.port_dev[p] = -2;
	in.port_dev[0] = -1;
	for (int b = 0; b < IN_NUM_BUTTONS; b++) in.remap[b] = (uint8_t)b;
	map_reset(&d);
	d.fd = fds[0];
	d.pad = &in.builtin;
	d.builtin = true;
	d.key_map[BTN_EAST] = IN_A;
	d.key_map[BTN_SOUTH] = IN_B;
	in.devs[0] = &d;
	/* A separate built-in device (the shoulder buttons) shares the pad. */
	shoulder.pad = &in.builtin;
	shoulder.builtin = true;
	in.devs[1] = &shoulder;
	in.builtin.cnt[IN_L] = 1;
	epoll_ctl(in.epfd, EPOLL_CTL_ADD, fds[0], &watch);
	kernel_held = held;
	queries = 0;
	if (write(fds[1], events, sizeof(events)) != sizeof(events)) exit(2);
	input_poll(&in);
	CHECK(queries == 1, "overflow queries the kernel's state");
	CHECK(!!(input_port_buttons(&in, 0) & BTN_MASK(IN_A)) == held,
	      held ? "an old release cannot cancel a real hold" : "an old press cannot stick a released button");
	CHECK(!(input_port_buttons(&in, 0) & BTN_MASK(IN_B)), "incomplete events are discarded");
	CHECK(input_port_buttons(&in, 0) & BTN_MASK(IN_L), "the other built-in device keeps its held button");
	CHECK(!!(in.nav_state & BTN_MASK(IN_A)) == held, "menu and game agree after recovery");
	/* Events arriving after the state query must still be processed. */
	events[0] = (struct input_event){ .type = EV_KEY, .code = BTN_EAST, .value = !held };
	events[1] = (struct input_event){ .type = EV_SYN, .code = SYN_REPORT };
	if (write(fds[1], events, 2 * sizeof(events[0])) != 2 * sizeof(events[0])) exit(2);
	input_poll(&in);
	CHECK(!!(input_port_buttons(&in, 0) & BTN_MASK(IN_A)) != held, "fresh input still works after recovery");
	close(fds[0]); close(fds[1]); close(in.epfd);
}

/* ------------------------------------------------- issue #10 (held buttons) */
struct rig {
	struct input in;
	struct dev pad, ext;      /* the built-in buttons, a USB pad */
	int fds[2], efds[2];
};

static void rig_open(struct rig *r)
{
	struct epoll_event w0 = { .events = EPOLLIN, .data.u32 = 0 };
	struct epoll_event w1 = { .events = EPOLLIN, .data.u32 = 1 };

	memset(r, 0, sizeof(*r));
	if (pipe2(r->fds, O_NONBLOCK | O_CLOEXEC) < 0 || pipe2(r->efds, O_NONBLOCK | O_CLOEXEC) < 0 ||
	    (r->in.epfd = epoll_create1(EPOLL_CLOEXEC)) < 0)
		exit(2);
	input_config_defaults(&r->in.cfg);
	r->in.cap_slot = -1;
	r->in.last_src = -3;
	r->in.builtin_present = true;
	for (int p = 0; p < INPUT_MAX_PORTS; p++) r->in.port_dev[p] = -2;
	r->in.port_dev[0] = -1;   /* P1: the built-in pad */
	r->in.port_dev[1] = 1;    /* P2: the USB pad */
	for (int b = 0; b < IN_NUM_BUTTONS; b++) r->in.remap[b] = (uint8_t)b;
	map_reset(&r->pad);
	r->pad.fd = r->fds[0];
	r->pad.pad = &r->in.builtin;
	r->pad.builtin = true;
	r->pad.key_map[BTN_EAST] = IN_A;
	r->pad.key_map[BTN_SOUTH] = IN_B;
	r->in.devs[0] = &r->pad;
	map_reset(&r->ext);
	r->ext.fd = r->efds[0];
	r->ext.slot = 1;
	r->ext.pad = &r->ext.own;
	r->ext.key_map[BTN_SOUTH] = IN_B;
	r->in.devs[1] = &r->ext;
	epoll_ctl(r->in.epfd, EPOLL_CTL_ADD, r->fds[0], &w0);
	epoll_ctl(r->in.epfd, EPOLL_CTL_ADD, r->efds[0], &w1);
}

static void rig_close(struct rig *r)
{
	close(r->fds[0]); close(r->fds[1]); close(r->efds[0]); close(r->efds[1]); close(r->in.epfd);
}

/* One key event + SYN_REPORT on a device's pipe, then a poll. */
static void key(struct rig *r, bool ext, int code, int value)
{
	struct input_event ev[2] = {
		{ .type = EV_KEY, .code = (uint16_t)code, .value = value },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};

	if (write(ext ? r->efds[1] : r->fds[1], ev, sizeof(ev)) != sizeof(ev)) exit(2);
	input_poll(&r->in);
}

#define HELD(r, port, b) (!!(input_port_buttons(&(r)->in, port) & BTN_MASK(b)))

static void held_check(void)
{
	struct rig r;
	int t;

	puts("input: the kernel's key state is checked while a key is held");
	rig_open(&r);
	queries = 0;
	key(&r, false, BTN_EAST, 1);
	CHECK(HELD(&r, 0, IN_A) && queries == 0, "a press is not checked at once");
	t = input_timeout_ms(&r.in);
	CHECK(t >= 0 && t <= HELD_CHECK_MS, "the menu wakes up for the check while a key is held");
	/* the release never arrived: the next check applies it */
	kernel_held = false;
	r.pad.held_check_at = 0;
	input_poll(&r.in);
	CHECK(queries == 1 && !HELD(&r, 0, IN_A), "a lost release is corrected by the check");
	CHECK(!(r.in.nav_state & BTN_MASK(IN_A)), "the menu sees the release too");
	CHECK(input_timeout_ms(&r.in) < 0, "no wake-up once nothing is held");
	/* a real hold stays held, and a long one is logged once */
	key(&r, false, BTN_EAST, 1);
	kernel_held = true;
	r.pad.held_check_at = 0;
	input_poll(&r.in);
	CHECK(HELD(&r, 0, IN_A), "a hold the kernel confirms is kept");
	CHECK(!(r.pad.long_logged & BTN_MASK(IN_A)), "a short hold is not logged");
	r.pad.press_at[IN_A] -= HELD_LOG_MS;
	r.pad.held_check_at = 0;
	input_poll(&r.in);
	CHECK(HELD(&r, 0, IN_A) && (r.pad.long_logged & BTN_MASK(IN_A)), "a 30 s hold is logged");
	key(&r, false, BTN_EAST, 0);
	CHECK(!HELD(&r, 0, IN_A), "the release still works");
	rig_close(&r);
}

static void menu_to_game(void)
{
	struct rig r;

	puts("input: buttons held when a menu gives the pad back to the game");
	rig_open(&r);
	kernel_held = true;   /* (no check runs here: held_check_at is in the future) */
	r.in.mode = INPUT_MODE_UI;            /* the in-game menu is open */
	key(&r, false, BTN_EAST, 1);          /* A on "Resume" */
	key(&r, true, BTN_SOUTH, 1);          /* B on player 2's USB pad */
	input_set_mode(&r.in, INPUT_MODE_GAME);
	CHECK(!HELD(&r, 0, IN_A), "the A that closed the menu does not reach the core");
	CHECK(!HELD(&r, 1, IN_B), "nor a button held on another pad");
	key(&r, false, BTN_SOUTH, 1);
	CHECK(HELD(&r, 0, IN_B), "a button pressed in the game counts at once");
	key(&r, false, BTN_EAST, 0);
	key(&r, true, BTN_SOUTH, 0);
	key(&r, false, BTN_EAST, 1);
	key(&r, true, BTN_SOUTH, 1);
	CHECK(HELD(&r, 0, IN_A), "A counts again once released and pressed");
	CHECK(HELD(&r, 1, IN_B), "player 2's B too (not a hotkey pad)");
	input_set_mode(&r.in, INPUT_MODE_GAME);
	CHECK(HELD(&r, 0, IN_A), "game to game: nothing is suppressed");
	rig_close(&r);
}

static void stick_detect(void)
{
	struct rig r;
	struct dev s;

	puts("input: the built-in stick is fitted only when it reads centred");
	rig_open(&r);
	memset(&s, 0, sizeof(s));
	map_reset(&s);
	s.builtin = s.builtin_stick = true;
	s.pad = &r.in.builtin;
	s.ax[ABS_X] = (struct axmap){ AX_STICK, 0, 0, false, false };
	s.ax[ABS_Y] = (struct axmap){ AX_STICK, 0, 1, false, false };
	for (int a = ABS_X; a <= ABS_Y; a++)
		s.absinfo[a] = (struct input_absinfo){ .minimum = 0, .maximum = 3000, .flat = 400 };
	/* floating inputs near an edge of the range: not a stick */
	r.in.stick_raw[0] = 2950;
	r.in.stick_raw[1] = 60;
	handle_abs(&r.in, &s, ABS_X, 2950);
	handle_abs(&r.in, &s, ABS_Y, 60);
	CHECK(!r.in.stick_fitted, "in range but far from the centre: not fitted");
	CHECK(!r.in.builtin.stick_nav[0] && !r.in.builtin.stick_nav[3] && !r.in.builtin.analog[0][0],
	      "no direction from an unfitted stick");
	handle_abs(&r.in, &s, ABS_X, 4000);
	CHECK(!r.in.stick_fitted, "out of range: not fitted");
	/* a real stick at rest (the RetroStone2's is centred near 1350..1650) */
	handle_abs(&r.in, &s, ABS_X, 1650);
	handle_abs(&r.in, &s, ABS_Y, 1350);
	CHECK(r.in.stick_fitted, "centred readings: fitted");
	CHECK(!r.in.builtin.stick_nav[0] && !r.in.builtin.stick_nav[1] && !r.in.builtin.stick_nav[2] &&
	      !r.in.builtin.stick_nav[3], "at rest: no direction");
	handle_abs(&r.in, &s, ABS_X, 3000);
	CHECK(r.in.builtin.stick_nav[3], "once fitted, a full push steers");
	handle_abs(&r.in, &s, ABS_X, 4095);
	CHECK(!r.in.stick_fitted && !r.in.builtin.stick_nav[3], "unplugged: not fitted, no direction");
	rig_close(&r);
}

/* ------------------------------------------- issue #6 (keyboard in a game) */
static bool next_hk(struct input *in, enum input_hotkey want)
{
	enum input_hotkey hk;
	bool seen = false;

	while (input_next_hotkey(in, &hk))
		if (hk == want)
			seen = true;
	return seen;
}

static void keyboard_in_game(void)
{
	struct rig r;
	enum input_hotkey hk;
	bool seen;

	puts("input: a keyboard can open the in-game menu and use the hotkeys");
	rig_open(&r);
	kernel_held = false;
	/* the second device is a USB keyboard: no player port */
	map_reset(&r.ext);
	r.ext.keyboard = true;
	map_keyboard(&r.ext);
	r.in.port_dev[1] = -2;
	r.in.mode = INPUT_MODE_GAME;
	while (input_next_hotkey(&r.in, &hk))
		;
	key(&r, true, KEY_ESC, 1);
	seen = next_hk(&r.in, IN_HK_MENU);   /* (CHECK evaluates its condition twice) */
	CHECK(seen, "Esc in a game opens the in-game menu");
	key(&r, true, KEY_ESC, 0);
	CHECK(!(pad_buttons(&r.ext.own) & BTN_MASK(IN_B)), "and is not a held B");
	/* Esc pressed in the menu (B: back to the game), released in the game */
	r.in.mode = INPUT_MODE_UI;
	key(&r, true, KEY_ESC, 1);
	CHECK(pad_buttons(&r.ext.own) & BTN_MASK(IN_B), "Esc is B in a menu");
	input_set_mode(&r.in, INPUT_MODE_GAME);
	key(&r, true, KEY_ESC, 0);
	CHECK(!(pad_buttons(&r.ext.own) & BTN_MASK(IN_B)), "its release in the game is not lost");
	seen = next_hk(&r.in, IN_HK_MENU);
	CHECK(!seen, "and does not reopen the menu");
	/* Tab (Select) + Space (Start) */
	key(&r, true, KEY_TAB, 1);
	key(&r, true, KEY_SPACE, 1);
	seen = next_hk(&r.in, IN_HK_EXIT);
	CHECK(seen, "Tab + Space exits the game");
	key(&r, true, KEY_SPACE, 0);
	key(&r, true, KEY_TAB, 0);
	/* nothing of it reaches a player */
	CHECK(!HELD(&r, 0, IN_START) && !HELD(&r, 0, IN_SELECT), "player 1 does not see the keyboard");
	rig_close(&r);
}

/* ------------------------------ Bluetooth/USB keyboards and mice (classify) */
static void set_keys(unsigned long *bits, const int *codes, int n)
{
	for (int i = 0; i < n; i++)
		bits[codes[i] / BITS_PER_LONG_] |= 1ul << (codes[i] % BITS_PER_LONG_);
}

static enum dev_kind kind_of(const int *keys, int nk, const int *abs, int na)
{
	unsigned long kb[NBITS(KEY_CNT)] = { 0 }, ab[NBITS(ABS_CNT)] = { 0 };

	set_keys(kb, keys, nk);
	set_keys(ab, abs, na);
	return classify_dev(kb, ab, nk);
}

static void classify(void)
{
	static const int kbd[] = { KEY_ENTER, KEY_A, KEY_ESC, KEY_TAB, KEY_SPACE };
	static const int mouse[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
	static const int trackpad[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP };
	static const int media[] = { KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE, KEY_PLAYPAUSE };
	static const int pad[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST };
	static const int cheap[] = { BTN_TRIGGER, BTN_THUMB, BTN_TOP, BTN_PINKIE };
	static const int absxy[] = { ABS_X, ABS_Y };

	puts("input: what a Bluetooth/USB keyboard, mouse or trackpad is");
	CHECK(kind_of(kbd, 5, NULL, 0) == DK_KEYBOARD, "a keyboard is a keyboard");
	CHECK(kind_of(mouse, 5, NULL, 0) == DK_OTHER, "a relative mouse is ignored");
	CHECK(kind_of(trackpad, 4, absxy, 2) == DK_OTHER, "an absolute trackpad is no gamepad");
	CHECK(kind_of(media, 4, NULL, 0) == DK_OTHER, "a keyboard's media-key node is ignored");
	CHECK(kind_of(pad, 4, absxy, 2) == DK_PAD, "a gamepad is a pad");
	CHECK(kind_of(cheap, 4, absxy, 2) == DK_PAD, "a joystick-style pad is a pad");
	CHECK(kind_of(cheap, 4, NULL, 0) == DK_PAD, "joystick buttons alone are a pad");
	{
		static const int hmm[] = { 0x2c0 /* BTN_TRIGGER_HAPPY1 */ };

		CHECK(kind_of(hmm, 1, NULL, 0) == DK_PAD, "a pad with only BTN_TRIGGER_HAPPY keys");
	}
}

int main(void)
{
	puts("input: overflow recovery with stale events in the read buffer");
	overflow(false);
	overflow(true);
	held_check();
	menu_to_game();
	stick_detect();
	keyboard_in_game();
	classify();
	return failures ? 1 : 0;
}
