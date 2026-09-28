/*
 * test_rumble.c - the rumble path of the input layer (input_rumble(): the
 * libretro rumble interface -> one evdev FF_RUMBLE effect per pad) against a
 * virtual gamepad made with uinput: the upload and its magnitudes, the play
 * and stop events, the same effect updated (not a second one), the repeated
 * state ignored, the "Controller vibration" switch, a port without a pad.
 * A thread answers the kernel's upload requests as a pad driver would.
 *
 * Without /dev/uinput or an evdev node (a machine without the modules, not
 * root): only the pure mapping runs, and the rest is reported as SKIP.
 *
 * usage: test_rumble WORKDIR
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/input/input.h"

static int failures;

#define CHECK(cond, ...)                                           \
	do {                                                       \
		int ok_ = (cond);                                  \
		printf(ok_ ? "  ok    " : "  FAIL  ");             \
		if (!ok_)                                          \
			failures++;                                \
		printf(__VA_ARGS__);                               \
		printf("\n");                                      \
	} while (0)

/* What the "driver" saw. */
static struct {
	int ufd;
	volatile int stop;
	pthread_mutex_t mu;
	int uploads, erases;
	int last_id;
	uint16_t strong, weak;
	uint16_t length;
	int plays, stops;
	int play_code;
} D = { .ufd = -1, .mu = PTHREAD_MUTEX_INITIALIZER };

static void *driver(void *arg)
{
	(void)arg;
	while (!D.stop) {
		struct pollfd p = { .fd = D.ufd, .events = POLLIN };
		struct input_event ev;

		if (poll(&p, 1, 50) <= 0)
			continue;
		while (read(D.ufd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
				struct uinput_ff_upload up;

				memset(&up, 0, sizeof(up));
				up.request_id = (uint32_t)ev.value;
				if (ioctl(D.ufd, UI_BEGIN_FF_UPLOAD, &up) < 0)
					continue;
				pthread_mutex_lock(&D.mu);
				D.uploads++;
				D.last_id = up.effect.id;
				D.strong = up.effect.u.rumble.strong_magnitude;
				D.weak = up.effect.u.rumble.weak_magnitude;
				D.length = up.effect.replay.length;
				pthread_mutex_unlock(&D.mu);
				up.retval = up.effect.type == FF_RUMBLE ? 0 : -EINVAL;
				ioctl(D.ufd, UI_END_FF_UPLOAD, &up);
			} else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
				struct uinput_ff_erase er;

				memset(&er, 0, sizeof(er));
				er.request_id = (uint32_t)ev.value;
				if (ioctl(D.ufd, UI_BEGIN_FF_ERASE, &er) < 0)
					continue;
				pthread_mutex_lock(&D.mu);
				D.erases++;
				pthread_mutex_unlock(&D.mu);
				er.retval = 0;
				ioctl(D.ufd, UI_END_FF_ERASE, &er);
			} else if (ev.type == EV_FF) {
				pthread_mutex_lock(&D.mu);
				if (ev.value)
					D.plays++;
				else
					D.stops++;
				D.play_code = ev.code;
				pthread_mutex_unlock(&D.mu);
			}
		}
	}
	return NULL;
}

static void wait_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* A gamepad with FF_RUMBLE (the Linux gamepad spec codes: no mapping file
 * needed). Returns the evdev node ("eventN") or "" on failure. */
static int make_pad(char *node, size_t n)
{
	struct uinput_setup us;
	char sys[64] = "", path[256];
	static const int keys[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
				    BTN_SELECT, BTN_START, BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT,
				    BTN_DPAD_RIGHT };

	node[0] = 0;
	D.ufd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (D.ufd < 0)
		return -errno;
	ioctl(D.ufd, UI_SET_EVBIT, EV_KEY);
	ioctl(D.ufd, UI_SET_EVBIT, EV_FF);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		ioctl(D.ufd, UI_SET_KEYBIT, keys[i]);
	ioctl(D.ufd, UI_SET_FFBIT, FF_RUMBLE);
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = 0x1234;
	us.id.product = 0x5678;
	us.id.version = 1;
	us.ff_effects_max = 4;
	snprintf(us.name, sizeof(us.name), "RSOS rumble test pad");
	if (ioctl(D.ufd, UI_DEV_SETUP, &us) < 0 || ioctl(D.ufd, UI_DEV_CREATE) < 0)
		return -errno;
	if (ioctl(D.ufd, UI_GET_SYSNAME(sizeof(sys)), sys) < 0)
		return -errno;
	/* /sys/devices/virtual/input/inputN/eventM, then /dev/input/eventM */
	for (int t = 0; t < 40 && !node[0]; t++) {
		for (int m = 0; m < 64 && !node[0]; m++) {
			snprintf(path, sizeof(path), "/sys/devices/virtual/input/%s/event%d", sys, m);
			if (access(path, F_OK) == 0)
				snprintf(node, n, "event%d", m);
		}
		if (!node[0])
			wait_ms(50);
	}
	for (int t = 0; t < 40 && node[0]; t++) {
		snprintf(path, sizeof(path), "/dev/input/%s", node);
		if (access(path, R_OK | W_OK) == 0)
			return 0;
		wait_ms(50);
	}
	node[0] = 0;
	return -ENOENT;
}

int main(int argc, char **argv)
{
	char node[32], dir[512], link[600], target[64];
	struct input_config cfg;
	struct input *in;
	pthread_t th;
	uint16_t s, w;
	int r;

	if (argc < 2) {
		fprintf(stderr, "usage: %s WORKDIR\n", argv[0]);
		return 2;
	}
	printf("rumble: the mapping\n");
	CHECK(input_rumble_mix(0xffff, 0, &s, &w) && s == 0xffff && w == 0, "strong motor alone: %04x/%04x", s, w);
	CHECK(input_rumble_mix(0x1000, 0x8000, &s, &w) && s == 0x1000 && w == 0x8000, "both motors: %04x/%04x", s, w);
	CHECK(!input_rumble_mix(0, 0, &s, &w), "both at 0: stop");

	printf("rumble: a virtual pad with FF_RUMBLE (uinput)\n");
	r = make_pad(node, sizeof(node));
	if (r < 0) {
		printf("  SKIP  no uinput pad with an evdev node here (%s): the kernel path is tested on the device\n",
		       strerror(-r));
		if (D.ufd >= 0)
			close(D.ufd);
		printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
		return failures ? 1 : 0;
	}
	/* the input layer reads a folder of its own with the node in it */
	snprintf(dir, sizeof(dir), "%s/dev-input", argv[1]);
	mkdir(argv[1], 0755);
	mkdir(dir, 0755);
	snprintf(link, sizeof(link), "%s/%s", dir, node);
	snprintf(target, sizeof(target), "/dev/input/%s", node);
	unlink(link);
	if (symlink(target, link) < 0) {
		printf("  FAIL  symlink %s: %s\n", link, strerror(errno));
		return 1;
	}
	pthread_create(&th, NULL, driver, NULL);
	input_config_defaults(&cfg);
	cfg.dev_dir = dir;
	cfg.user_map_dir = dir;
	cfg.gcdb_path = "/nonexistent";
	cfg.remap_sys_dir = dir;
	cfg.remap_user_dir = dir;
	cfg.backlight_dir = dir;
	in = input_open(&cfg);
	input_set_mode(in, INPUT_MODE_GAME);
	CHECK(input_port_has_rumble(in, 0), "player 1 is the pad, it can rumble");
	CHECK(!input_port_has_rumble(in, 1) && !input_rumble(in, 1, 0, 0x8000), "player 2: no pad, nothing sent");

	CHECK(input_rumble(in, 0, 0, 0x8000), "strong motor at 0x8000");
	wait_ms(100);
	pthread_mutex_lock(&D.mu);
	CHECK(D.uploads == 1 && D.strong == 0x8000 && D.weak == 0 && D.length == 0xffff && D.plays == 1,
	      "one effect uploaded (strong %04x, weak %04x, %u ms) and played (%d)", D.strong, D.weak, D.length,
	      D.plays);
	pthread_mutex_unlock(&D.mu);

	CHECK(input_rumble(in, 0, 1, 0x4000), "weak motor added");
	CHECK(input_rumble(in, 0, 1, 0x4000), "the same state again (cores repeat it every frame)");
	wait_ms(100);
	pthread_mutex_lock(&D.mu);
	CHECK(D.uploads == 2 && D.strong == 0x8000 && D.weak == 0x4000 && D.plays == 2,
	      "the same effect updated (%d uploads, strong %04x, weak %04x), played again (%d)", D.uploads,
	      D.strong, D.weak, D.plays);
	pthread_mutex_unlock(&D.mu);

	input_rumble(in, 0, 0, 0);
	input_rumble(in, 0, 1, 0);
	wait_ms(100);
	pthread_mutex_lock(&D.mu);
	CHECK(D.stops == 1 && D.uploads == 3,
	      "strong off: the weak motor alone (an update), then both at 0: stopped (%d uploads, %d stop)", D.uploads,
	      D.stops);
	pthread_mutex_unlock(&D.mu);

	input_rumble(in, 0, 0, 0xffff);
	input_set_rumble(in, false);             /* Settings > Controls > Controller vibration off */
	wait_ms(100);
	pthread_mutex_lock(&D.mu);
	CHECK(D.stops == 2, "vibration switched off during a rumble: stopped (%d stops)", D.stops);
	pthread_mutex_unlock(&D.mu);
	CHECK(!input_rumble(in, 0, 0, 0x8000), "vibration off: requests ignored");
	input_set_rumble(in, true);
	CHECK(input_rumble(in, 0, 0, 0x2000), "on again: it rumbles");
	input_close(in);                         /* a game that ends while rumbling */
	wait_ms(150);
	pthread_mutex_lock(&D.mu);
	CHECK(D.stops >= 3, "closed while rumbling: stopped (%d stops, %d erases)", D.stops, D.erases);
	pthread_mutex_unlock(&D.mu);

	D.stop = 1;
	pthread_join(th, NULL);
	ioctl(D.ufd, UI_DEV_DESTROY);
	close(D.ufd);
	unlink(link);
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
