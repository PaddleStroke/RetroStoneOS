/*
 * rsos-gamepad - the RetroStone2's built-in controls as one standard gamepad
 * (uinput), for native games that read SDL game controllers (docs/native-games.md).
 *
 * The controls are three evdev devices: "RetroStone2 Buttons" (face, D-pad,
 * Start/Select, optional C/Z), "RetroStone2 Shoulder Buttons" and the optional
 * analog stick ("analog-stick", which SDL does not even take for a joystick).
 * This reads them (no grab: the menu reads them too, for Select+Start) and
 * writes "RetroStoneOS Gamepad": the Linux gamepad layout by position (bottom
 * = BTN_SOUTH), C/Z as the stick clicks, the D-pad as hat 0, the stick as
 * ABS_X/ABS_Y. The launch script starts it in the game's process group, so
 * the menu stops it with the game, and gives SDL its mapping:
 *
 *   SDL_GAMECONTROLLERCONFIG=  RSOS_GAMEPAD_SDL_MAPPING below
 *   SDL_GAMECONTROLLER_IGNORE_DEVICES=0x0001/0x0001   (the raw gpio-keys)
 *
 *   rsos-gamepad [--print-mapping]
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NAME "RetroStoneOS Gamepad"
#define VENDOR 0x1209   /* pid.codes test vendor: no real device to clash with */
#define PRODUCT 0x5253  /* "RS" */
#define VERSION 0x0001
/* SDL's GUID for it with the name CRC left 0 (matches any name): bus
 * BUS_VIRTUAL (0x06), vendor, product, version, little-endian 16-bit words;
 * buttons numbered in key code order (BTN_SOUTH 0x130 ... BTN_THUMBR 0x13e) */
#define RSOS_GAMEPAD_SDL_MAPPING                                                              \
	"06000000091200005352000001000000," NAME ",a:b0,b:b1,y:b2,x:b3,leftshoulder:b4,"     \
	"rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,leftstick:b10,"     \
	"rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,"    \
	"platform:Linux,"

#define MAX_SRC 4

static const int out_keys[] = {
	BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
	BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR,
};

static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static void emit(int fd, int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = (unsigned short)type;
	ev.code = (unsigned short)code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) < 0 && errno != EAGAIN)
		fprintf(stderr, "rsos-gamepad: write: %s\n", strerror(errno));
}

static int create_pad(void)
{
	struct uinput_setup us;
	struct uinput_abs_setup abs;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "rsos-gamepad: /dev/uinput: %s\n", strerror(errno));
		return -1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	for (size_t i = 0; i < sizeof(out_keys) / sizeof(out_keys[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, out_keys[i]);
	memset(&abs, 0, sizeof(abs));
	abs.absinfo.minimum = -32767;
	abs.absinfo.maximum = 32767;
	abs.code = ABS_X;
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_ABS_SETUP, &abs);
	abs.code = ABS_Y;
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(fd, UI_ABS_SETUP, &abs);
	abs.absinfo.minimum = -1;
	abs.absinfo.maximum = 1;
	abs.code = ABS_HAT0X;
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
	ioctl(fd, UI_ABS_SETUP, &abs);
	abs.code = ABS_HAT0Y;
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);
	ioctl(fd, UI_ABS_SETUP, &abs);

	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_VIRTUAL;
	us.id.vendor = VENDOR;
	us.id.product = PRODUCT;
	us.id.version = VERSION;
	snprintf(us.name, sizeof(us.name), "%s", NAME);
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "rsos-gamepad: cannot create the device: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

struct src {
	int fd;
	bool stick;
	struct input_absinfo ax, ay;
};

static int open_sources(struct src *s)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)) && n < MAX_SRC) {
		char path[300], name[128] = "";
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 ||
		    (strcmp(name, "RetroStone2 Buttons") && strcmp(name, "RetroStone2 Shoulder Buttons") &&
		     strcmp(name, "analog-stick"))) {
			close(fd);
			continue;
		}
		memset(&s[n], 0, sizeof(s[n]));
		s[n].fd = fd;
		s[n].stick = !strcmp(name, "analog-stick") && ioctl(fd, EVIOCGABS(ABS_X), &s[n].ax) == 0 &&
			     ioctl(fd, EVIOCGABS(ABS_Y), &s[n].ay) == 0 && s[n].ax.maximum != s[n].ax.minimum &&
			     s[n].ay.maximum != s[n].ay.minimum;
		fprintf(stderr, "rsos-gamepad: %s (%s)\n", path, name);
		n++;
	}
	closedir(d);
	return n;
}

static bool in_range(int v, const struct input_absinfo *a, bool near_centre)
{
	int lo = a->minimum < a->maximum ? a->minimum : a->maximum;
	int hi = a->minimum < a->maximum ? a->maximum : a->minimum;

	if (near_centre)
		return abs(v - (lo + (hi - lo) / 2)) <= (hi - lo) / 4;
	return v >= lo && v <= hi;
}

/* -32767..32767 from the ADC range */
static int scale(int v, const struct input_absinfo *a)
{
	int lo = a->minimum < a->maximum ? a->minimum : a->maximum;
	int hi = a->minimum < a->maximum ? a->maximum : a->minimum;
	long r = ((long)(v - lo) * 65534) / (hi - lo) - 32767;

	return r < -32767 ? -32767 : r > 32767 ? 32767 : (int)r;
}

int main(int argc, char **argv)
{
	struct src src[MAX_SRC];
	struct pollfd pfd[MAX_SRC];
	int out, n, sx = 0, sy = 0, hat_x = 0, hat_y = 0;
	bool fitted = false, dpad[4] = { false };   /* up, down, left, right */

	if (argc > 1 && !strcmp(argv[1], "--print-mapping")) {
		puts(RSOS_GAMEPAD_SDL_MAPPING);
		return 0;
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);
	n = open_sources(src);
	if (n == 0) {
		fprintf(stderr, "rsos-gamepad: no built-in controls found\n");
		return 1;
	}
	out = create_pad();
	if (out < 0)
		return 1;
	for (int i = 0; i < n; i++) {
		pfd[i].fd = src[i].fd;
		pfd[i].events = POLLIN;
		if (src[i].stick) {
			sx = src[i].ax.value;
			sy = src[i].ay.value;
		}
	}

	while (!quit) {
		if (poll(pfd, (nfds_t)n, 1000) <= 0)
			continue;
		for (int i = 0; i < n; i++) {
			struct input_event ev;
			bool changed = false;

			if (!(pfd[i].revents & POLLIN))
				continue;
			while (read(src[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type == EV_KEY) {
					int code = ev.code;

					changed = true;
					switch (code) {
					case BTN_C: code = BTN_THUMBL; break;
					case BTN_Z: code = BTN_THUMBR; break;
					case BTN_DPAD_UP: dpad[0] = ev.value; code = -1; break;
					case BTN_DPAD_DOWN: dpad[1] = ev.value; code = -1; break;
					case BTN_DPAD_LEFT: dpad[2] = ev.value; code = -1; break;
					case BTN_DPAD_RIGHT: dpad[3] = ev.value; code = -1; break;
					default: break;
					}
					if (code >= 0 && ev.value != 2)
						emit(out, EV_KEY, code, ev.value != 0);
				} else if (ev.type == EV_ABS && src[i].stick && (ev.code == ABS_X || ev.code == ABS_Y)) {
					if (ev.code == ABS_X)
						sx = ev.value;
					else
						sy = ev.value;
					changed = true;
				}
			}
			if (!changed)
				continue;
			{
				int hx = dpad[3] - dpad[2], hy = dpad[1] - dpad[0];

				if (hx != hat_x)
					emit(out, EV_ABS, ABS_HAT0X, hat_x = hx);
				if (hy != hat_y)
					emit(out, EV_ABS, ABS_HAT0Y, hat_y = hy);
			}
			if (src[i].stick) {
				/* the menu's rule (input.c): out of range = not fitted;
				 * fitted again once both axes read near the centre */
				if (!in_range(sx, &src[i].ax, false) || !in_range(sy, &src[i].ay, false))
					fitted = false;
				else if (!fitted && in_range(sx, &src[i].ax, true) && in_range(sy, &src[i].ay, true))
					fitted = true;
				emit(out, EV_ABS, ABS_X, fitted ? scale(sx, &src[i].ax) : 0);
				emit(out, EV_ABS, ABS_Y, fitted ? scale(sy, &src[i].ay) : 0);
			}
			emit(out, EV_SYN, SYN_REPORT, 0);
		}
	}
	ioctl(out, UI_DEV_DESTROY);
	close(out);
	return 0;
}
