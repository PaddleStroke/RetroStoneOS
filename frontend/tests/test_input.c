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

/* A Retrolink N64 pad (0079:0006, buttons as recorded on a console):
 * A b6, B b8, Z b7, L b4, R b5, Start b9, C-up b0, C-down b2, C-left b3,
 * C-right b1, in the database's names for an N64 pad (input.h) */
#define RETROLINK_N64 "a:b6,b:b8,-righty:b0,+righty:b2,-rightx:b3,+rightx:b1,lefttrigger:b7," \
	"leftshoulder:b4,rightshoulder:b5,start:b9,dpup:h0.1,type:n64"
#define N64B(i) (BTN_TRIGGER + (i))

static bool next_nav(struct input *in, enum input_btn want)
{
	struct input_nav ev;
	bool seen = false;

	while (input_next_nav(in, &ev))
		if (ev.btn == want && ev.type == IN_NAV_PRESS)
			seen = true;
	return seen;
}

static void n64_pad(void)
{
	struct rig r;
	enum input_hotkey hk;
	char sdl[2048];
	bool seen;

	puts("input: an N64 pad profile (type n64)");
	rig_open(&r);
	kernel_held = false;
	map_reset(&r.ext);
	for (int c = 0; c < KEY_CNT; c++)
		r.ext.btn_index[c] = -1;
	for (int i = 0; i < 12; i++)
		r.ext.btn_index[N64B(i)] = (int16_t)i;
	r.ext.hat_index[0] = 0;
	CHECK(apply_sdl_mapping(&r.ext, RETROLINK_N64) >= 10, "the profile applies");
	CHECK(r.ext.type == IN_PAD_N64, "its type is n64");
	strlcpy_(r.ext.guid, "03000000790000000600000010010000", sizeof(r.ext.guid));
	strlcpy_(r.ext.name, "Generic USB Joystick", sizeof(r.ext.name));
	r.ext.source = IN_MAP_USER;
	r.ext.configured = true;

	/* its stick: not in the profile, so the usual X / Y axes */
	{
		unsigned long absbits[ABS_CNT / (8 * sizeof(unsigned long)) + 1] = { 0 };
		struct input_event ev = { .type = EV_ABS, .code = ABS_X, .value = 255 };
		char elem[16];

		absbits[0] = (1UL << ABS_X) | (1UL << ABS_Y);
		default_left_stick(&r.ext, absbits);
		CHECK(r.ext.ax[ABS_X].kind == AX_STICK && r.ext.ax[ABS_Y].kind == AX_STICK,
		      "a profile without the stick gets X / Y");
		/* the wizard: an 8-bit stick (0..255, rest 128) pushed all the way */
		r.ext.axis_index[ABS_X] = 0;
		r.ext.axis_index[ABS_Y] = 1;
		for (int a = ABS_X; a <= ABS_Y; a++) {
			r.ext.absinfo[a] = (struct input_absinfo){ .minimum = 0, .maximum = 255 };
			r.ext.abs_rest[a] = 128;
		}
		r.in.cap_slot = 1;
		capture_event(&r.in, &r.ext, &ev);
		seen = input_capture_next(&r.in, elem, sizeof(elem)) && !strcmp(elem, "+a0");
		CHECK(seen, "the wizard sees an 8-bit stick pushed right");
		r.in.cap_slot = -1;
	}

	/* in a game: positional for the N64 core, C buttons also on the right stick */
	r.in.mode = INPUT_MODE_GAME;
	key(&r, true, N64B(6), 1);
	CHECK(HELD(&r, 1, IN_B), "A is RetroPad B (the N64 core's A)");
	key(&r, true, N64B(6), 0);
	key(&r, true, N64B(0), 1);
	CHECK(input_port_analog(&r.in, 1, 1, 1) == -0x7fff, "C-up pushes the right stick up");
	key(&r, true, N64B(0), 0);
	CHECK(input_port_analog(&r.in, 1, 1, 1) == 0, "and lets it go");
	key(&r, true, N64B(1), 1);
	CHECK(input_port_analog(&r.in, 1, 1, 0) == 0x7fff, "C-right pushes it right");
	key(&r, true, N64B(1), 0);

	/* Z + Start: a short press is ordinary play, a held second the menu */
	while (input_next_hotkey(&r.in, &hk))
		;
	key(&r, true, N64B(7), 1);
	key(&r, true, N64B(9), 1);
	CHECK(input_quit_combo_held(&r.in), "Z + Start counts as the quit combination");
	seen = next_hk(&r.in, IN_HK_MENU);
	CHECK(!seen, "Z + Start pressed: no menu yet");
	CHECK(input_timeout_ms(&r.in) > 0 && input_timeout_ms(&r.in) <= 1000, "the hold is timed");
	r.ext.held_check_at = ui_now_ms() + 60000;   /* the mock's kernel state says released */
	usleep(1100 * 1000);
	input_poll(&r.in);
	seen = next_hk(&r.in, IN_HK_MENU);
	CHECK(seen, "Z + Start held a second opens the in-game menu");
	key(&r, true, N64B(9), 0);
	key(&r, true, N64B(7), 0);

	/* in the menus: A confirms, B goes back */
	input_set_mode(&r.in, INPUT_MODE_UI);
	while (next_nav(&r.in, IN_A))
		;
	key(&r, true, N64B(6), 1);
	seen = next_nav(&r.in, IN_A);
	CHECK(seen, "in the menu, A confirms");
	key(&r, true, N64B(6), 0);
	key(&r, true, N64B(8), 1);
	seen = next_nav(&r.in, IN_B);
	CHECK(seen, "and B goes back");
	key(&r, true, N64B(8), 0);

	/* for the native games: SDL's layout, without our own fields */
	input_sdl_mappings(&r.in, sdl, sizeof(sdl));
	CHECK(strstr(sdl, "03000000790000000600000010010000,N64 Controller (Generic USB Joystick),") != NULL,
	      "an SDL line per pad, named N64 for the games that choose a layout by name");
	CHECK(strstr(sdl, "b:b8,") && strstr(sdl, "-righty:b0,") && strstr(sdl, "+rightx:b1,"),
	      "B on b, the C buttons on the right stick");
	CHECK(strstr(sdl, "rightshoulder:b5,") && strstr(sdl, "righttrigger:b5,"), "R on the shoulder and the trigger");
	CHECK(strstr(sdl, "a:b6,") && strstr(sdl, "lefttrigger:b7,") && strstr(sdl, "dpup:h0.1,"),
	      "A, Z and the d-pad as set up");
	CHECK(!strstr(sdl, "type:"), "no type field for SDL");
	CHECK(strstr(sdl, "leftx:a0,lefty:a1,") != NULL, "and the default stick for SDL");
	rig_close(&r);
}

/* ------------------------------------------ known pads (no hardware here) */
#define END (-1)

static void set_bits(unsigned long *bits, const int *codes)
{
	for (; *codes != END; codes++)
		bits[*codes / BITS_PER_LONG_] |= 1ul << (*codes % BITS_PER_LONG_);
}

/* The pad as the kernel would show it (bus, ids, driver, codes), resolved
 * as input_open() does, into the rig's USB slot */
static void plug(struct rig *r, uint16_t vendor, uint16_t product, uint16_t version,
		 const char *driver, const char *name, const int *keys, const int *abs)
{
	unsigned long kb[NBITS(KEY_CNT)] = { 0 }, ab[NBITS(ABS_CNT)] = { 0 };
	struct input_nav ev;
	enum input_hotkey hk;

	set_bits(kb, keys);
	set_bits(ab, abs);
	r->ext.id = (struct input_id){ .bustype = BUS_USB, .vendor = vendor, .product = product,
				       .version = version };
	strlcpy_(r->ext.driver, driver, sizeof(r->ext.driver));
	strlcpy_(r->ext.name, name, sizeof(r->ext.name));
	resolve_mapping(&r->in, &r->ext, kb, ab);
	pad_release_all(&r->ext.own);
	memset(r->ext.down, 0, sizeof(r->ext.down));
	r->ext.guide = false;
	r->ext.own.has_analog = true;
	while (input_next_nav(&r->in, &ev))
		;
	while (input_next_hotkey(&r->in, &hk))
		;
}

/* A press and release in a game: the RetroPad buttons it held */
static uint16_t game_press(struct rig *r, int code)
{
	uint16_t b;

	input_set_mode(&r->in, INPUT_MODE_GAME);
	key(r, true, code, 1);
	b = input_port_buttons(&r->in, 1);
	key(r, true, code, 0);
	return b;
}

/* A press and release in the menus: did the menu see `want`? */
static bool ui_press(struct rig *r, int code, enum input_btn want)
{
	bool seen;

	input_set_mode(&r->in, INPUT_MODE_UI);
	while (next_nav(&r->in, want))
		;
	key(r, true, code, 1);
	seen = next_nav(&r->in, want);
	key(r, true, code, 0);
	return seen;
}

static void known_pads(void)
{
	/* linux 6.18 hid-nintendo, n64con_button_mappings */
	static const int nso_n64_keys[] = { BTN_A, BTN_B, BTN_C, BTN_X, BTN_Y, BTN_Z, BTN_TL, BTN_TR,
					    BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE, END };
	static const int stick_hat[] = { ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y, END };
	/* a 16-button HID joystick (hid-generic) with six axes */
	static const int hid16[] = { 0x120, 0x121, 0x122, 0x123, 0x124, 0x125, 0x126, 0x127, 0x128,
				     0x129, 0x12a, 0x12b, 0x12c, 0x12d, 0x12e, 0x12f, END };
	static const int six_axes[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, END };
	/* a 15-button HID gamepad (BTN_SOUTH ...) */
	static const int gp15[] = { 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138,
				    0x139, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e, END };
	static const int four_axes_hat[] = { ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y, END };
	/* xpad */
	static const int x360_keys[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT,
					 BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, END };
	static const int x360_abs[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X,
					ABS_HAT0Y, END };
	/* hid-nintendo gencon / nescon / a right Joy-Con */
	static const int gen_keys[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_Z, BTN_TL,
					BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, END };
	static const int nes_keys[] = { BTN_SOUTH, BTN_EAST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, END };
	static const int jcr_keys[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TR, BTN_TR2,
					BTN_START, BTN_THUMBR, BTN_MODE, END };
	/* a DualSense's touch pad, a device of its own */
	static const int touch_keys[] = { BTN_LEFT, BTN_TOOL_FINGER, BTN_TOUCH, BTN_TOOL_DOUBLETAP, END };
	static const int touch_abs[] = { ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_POSITION_X,
					 ABS_MT_POSITION_Y, END };
	struct rig r;
	char dir[] = "/tmp/rsos-input-XXXXXX", sdl[4096], path[128];
	unsigned long kb[NBITS(KEY_CNT)] = { 0 }, ab[NBITS(ABS_CNT)] = { 0 };
	enum input_hotkey hk;
	bool seen;
	uint16_t b;
	FILE *f;

	puts("input: known pads, as their kernel drivers and the database lay them out");
	rig_open(&r);
	kernel_held = false;
	if (!mkdtemp(dir))
		exit(2);
	r.in.cfg.user_map_dir = dir;
	r.in.cfg.gcdb_path = "third_party/sdl-gamecontrollerdb/gamecontrollerdb.txt";

	/* Nintendo Switch Online N64 (hid-nintendo) */
	plug(&r, 0x057e, 0x2019, 0x8111, "nintendo", "N64 Controller", nso_n64_keys, stick_hat);
	CHECK(r.ext.source == IN_MAP_PROFILE && r.ext.type == IN_PAD_N64, "NSO N64: a built-in N64 profile");
	input_set_mode(&r.in, INPUT_MODE_GAME);
	key(&r, true, BTN_SELECT, 1);
	CHECK(input_port_analog(&r.in, 1, 1, 1) == -0x7fff, "NSO N64: C-up (the kernel's Select) is C-up");
	key(&r, true, BTN_SELECT, 0);
	CHECK(game_press(&r, BTN_EAST) == BTN_MASK(IN_Y), "NSO N64: B (the kernel's EAST) is the N64 core's B");
	CHECK(game_press(&r, BTN_TL2) == BTN_MASK(IN_L2), "NSO N64: Z is L2");
	CHECK(ui_press(&r, BTN_SOUTH, IN_A), "NSO N64: A confirms in the menu");
	input_sdl_mappings(&r.in, sdl, sizeof(sdl));
	CHECK(strstr(sdl, "a:b0,") && strstr(sdl, "b:b1,") && strstr(sdl, "-righty:b10,") &&
	      strstr(sdl, "+rightx:b2,") && strstr(sdl, "lefttrigger:b8,") && strstr(sdl, "guide:b12,"),
	      "NSO N64: native games get the database's layout for it");
	{
		/* a port's layout renames the elements (all at once: two can swap) */
		static char s2[8192];

		input_sdl_mappings_layout(&r.in, s2, sizeof(s2),
					  "# comment\nz = righttrigger\ncup = leftshoulder\nl=back\nb = x\nr = none\n");
		CHECK(strstr(s2, "righttrigger:b8,") && !strstr(s2, "lefttrigger:"), "layout: Z moved to the right trigger");
		CHECK(strstr(s2, "leftshoulder:b10,") && !strstr(s2, "-righty:"), "layout: C-up moved to the left shoulder");
		CHECK(strstr(s2, "x:b1,") && !strstr(s2, "b:b1,"), "layout: B moved to x");
		CHECK(strstr(s2, "back:") && !strstr(s2, "rightshoulder:"), "layout: L is back, R dropped");
		CHECK(strstr(s2, "+rightx:b2,") && strstr(s2, "a:b0,"), "layout: what it does not name stays");
		CHECK(strstr(s2, "platform:Linux,\n"), "layout: the line still ends properly");
		input_sdl_mappings_layout(&r.in, s2, sizeof(s2), NULL);
		CHECK(strstr(s2, "lefttrigger:b8,") && strstr(s2, "b:b1,"), "no layout: unchanged");
	}

	/* Mayflash GameCube adapter, PC mode (hid-generic; the database knows it) */
	plug(&r, 0x0079, 0x1844, 0x0110, "hid-generic", "Mayflash GC", hid16, six_axes);
	CHECK(r.ext.source == IN_MAP_GCDB && r.ext.type == IN_PAD_GC, "Mayflash: a GameCube pad from the database");
	CHECK(game_press(&r, 0x121) == BTN_MASK(IN_B), "GameCube: A is RetroPad B");
	CHECK(game_press(&r, 0x120) == BTN_MASK(IN_Y), "GameCube: B (left of A) is RetroPad Y");
	CHECK(game_press(&r, 0x122) == BTN_MASK(IN_A), "GameCube: X (right of A) is RetroPad A");
	CHECK(game_press(&r, 0x127) == BTN_MASK(IN_R), "GameCube: Z is R1");
	CHECK(ui_press(&r, 0x121, IN_A), "GameCube: A confirms in the menu");
	CHECK(ui_press(&r, 0x120, IN_B), "GameCube: B goes back");
	input_set_mode(&r.in, INPUT_MODE_GAME);
	key(&r, true, 0x127, 1);
	key(&r, true, 0x129, 1);
	CHECK(input_quit_combo_held(&r.in), "GameCube: Z + Start is the quit combination");
	key(&r, true, 0x129, 0);
	key(&r, true, 0x127, 0);
	input_sdl_mappings(&r.in, sdl, sizeof(sdl));
	CHECK(strstr(sdl, "a:b1,") && strstr(sdl, "b:b0,") && strstr(sdl, "x:b2,") &&
	      strstr(sdl, "rightshoulder:b7,"), "GameCube: native games get the database's labels");

	/* 8BitDo N64 (hid-generic; the database knows it): C on a real stick */
	plug(&r, 0x2dc8, 0x2869, 0x0111, "hid-generic", "8BitDo N64", gp15, four_axes_hat);
	CHECK(r.ext.source == IN_MAP_GCDB && r.ext.type == IN_PAD_N64, "8BitDo N64: an N64 pad from the database");
	CHECK(game_press(&r, 0x131) == BTN_MASK(IN_Y), "8BitDo N64: B is the N64 core's B");
	CHECK(game_press(&r, 0x138) == BTN_MASK(IN_L2), "8BitDo N64: Z is L2");
	CHECK(r.ext.ax[ABS_Z].kind == AX_STICK && r.ext.ax[ABS_Z].a == 1, "8BitDo N64: its C stick is the right stick");

	/* Xbox 360 (xpad): the Linux standard */
	plug(&r, 0x045e, 0x028e, 0x0114, "xpad", "Microsoft X-Box 360 pad", x360_keys, x360_abs);
	CHECK(r.ext.source == IN_MAP_PROFILE && r.ext.type == IN_PAD_MODERN, "Xbox 360: the Linux standard");
	CHECK(game_press(&r, BTN_SOUTH) == BTN_MASK(IN_B), "Xbox 360: A (bottom) is RetroPad B");
	CHECK(game_press(&r, BTN_EAST) == BTN_MASK(IN_A), "Xbox 360: B (right) is RetroPad A");
	CHECK(r.ext.ax[ABS_Z].kind == AX_TRIGGER && r.ext.ax[ABS_RX].kind == AX_STICK,
	      "Xbox 360: analog triggers, the right stick on RX / RY");
	input_set_mode(&r.in, INPUT_MODE_GAME);
	while (input_next_hotkey(&r.in, &hk))
		;
	key(&r, true, BTN_MODE, 1);
	seen = next_hk(&r.in, IN_HK_MENU);
	CHECK(seen, "Guide in a game opens the in-game menu");
	CHECK(input_quit_combo_held(&r.in), "Guide held is the quit combination (native games)");
	key(&r, true, BTN_MODE, 0);
	CHECK(!input_quit_combo_held(&r.in), "and released, it is not");

	/* NSO Genesis: the kernel, not the database line for another layout */
	plug(&r, 0x057e, 0x201e, 0x8111, "", "Sega Genesis Control Pad", gen_keys, stick_hat);
	CHECK(r.ext.source == IN_MAP_PROFILE, "NSO Genesis: not the database's line");
	CHECK(game_press(&r, BTN_WEST) == BTN_MASK(IN_Y), "NSO Genesis: A is RetroPad Y");
	CHECK(game_press(&r, BTN_SOUTH) == BTN_MASK(IN_B), "NSO Genesis: B is RetroPad B");
	CHECK(game_press(&r, BTN_EAST) == BTN_MASK(IN_A), "NSO Genesis: C is RetroPad A");

	/* NSO NES (it says it is a Joy-Con) and a real right Joy-Con */
	plug(&r, 0x057e, 0x2007, 0x8111, "nintendo", "Joy-Con (R)", nes_keys, stick_hat);
	CHECK(game_press(&r, BTN_SOUTH) == BTN_MASK(IN_A), "NSO NES: A (the kernel's SOUTH) is RetroPad A");
	CHECK(game_press(&r, BTN_EAST) == BTN_MASK(IN_B), "NSO NES: B is RetroPad B");
	plug(&r, 0x057e, 0x2007, 0x8111, "nintendo", "Joy-Con (R)", jcr_keys, stick_hat);
	CHECK(game_press(&r, BTN_SOUTH) == BTN_MASK(IN_B), "a Joy-Con keeps the standard layout");

	/* names in the database */
	CHECK(type_from_name("Raphnet GC and N64 Adapter") == IN_PAD_GC &&
	      type_from_name("NSO GameCube Controller") == IN_PAD_GC &&
	      type_from_name("PS SS N64 Adapter") == IN_PAD_N64 &&
	      type_from_name("Hyperkin Admiral N64 Controller") == IN_PAD_N64 &&
	      type_from_name("MSI GC20 V2") == IN_PAD_MODERN &&
	      type_from_name("Be1 GC101 Xbox 360 Controller") == IN_PAD_MODERN,
	      "N64 and GameCube pads by their names in the database");

	/* not a pad: a PlayStation pad's touch pad */
	set_bits(kb, touch_keys);
	set_bits(ab, touch_abs);
	CHECK(!caps_are_pad(kb, ab, 4), "a DualSense touch pad is not a player");
	memset(kb, 0, sizeof(kb));
	set_bits(kb, hid16);
	CHECK(caps_are_pad(kb, ab, 16), "a HID joystick is");

	/* a user mapping wins; forgetting it goes back to the automatic one */
	snprintf(path, sizeof(path), "%s/03000000790000004418000010010000.cfg", dir);
	f = fopen(path, "w");
	if (!f)
		exit(2);
	fputs("03000000790000004418000010010000,Mayflash GC,a:b0,b:b1,start:b9,type:n64,platform:Linux,\n", f);
	fclose(f);
	plug(&r, 0x0079, 0x1844, 0x0110, "hid-generic", "Mayflash GC", hid16, six_axes);
	CHECK(r.ext.source == IN_MAP_USER && r.ext.type == IN_PAD_N64, "a user mapping wins over the database");
	b = game_press(&r, 0x121);
	CHECK(b == BTN_MASK(IN_Y), "with its own kind of pad");
	CHECK(input_forget_user_mapping(&r.in, 1) == 0 && access(path, F_OK) != 0, "forgetting it removes the file");
	CHECK(r.ext.source != IN_MAP_USER, "and the mapping is automatic again");
	rmdir(dir);
	rig_close(&r);
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
	n64_pad();
	known_pads();
	return failures ? 1 : 0;
}
