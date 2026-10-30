/*
 * controller_rsos.c - the RetroStone2's built-in controls for sm64-port
 * (RetroStoneOS native games, ports/sm64).
 *
 * The console's controls are three evdev devices: "RetroStone2 Buttons"
 * (face buttons, D-pad, Start/Select), "RetroStone2 Shoulder Buttons" (L1,
 * R1, L2, R2) and the optional analog stick ("analog-stick"). They are read
 * here together, straight from /dev/input; controller_sdl leaves them
 * alone (it would see three separate pads) and keeps USB pads.
 *
 * The buttons follow the N64 emulator (parallel_n64), so Mario plays the
 * same natively and emulated, and the N64 button settings are global:
 *   1. physical button -> RetroPad, as the menu's input layer does
 *      (bottom = B, right = A, top = X, left = Y, C/Z = L3/R3);
 *   2. the N64 remap made in the emulator's in-game menu, in the same files
 *      and order (/data/rsos/remaps/n64[-cz].ini, then the shipped one in
 *      /usr/share/rsos/remaps; -cz first when "C/Z buttons fitted" is on);
 *   3. RetroPad -> N64 as parallel_n64 maps it, following its core option
 *      "Independent C-button Controls" (parallel-n64-alt-map, from
 *      /data/rsos/coreopts/parallel_n64.ini or the shipped defaults):
 *        off (default): B = A, Y = B, L2 = Z, L = L, R = R, Start;
 *                       R2 held: A = C-right, Y = C-left, B = C-down,
 *                       X = C-up (the core's "C buttons mode")
 *        on:            B = A, Y = B, L2 = Z, R2 = R, Select = L, Start,
 *                       R = C-right, L = C-left, A = C-down, X = C-up
 * The analog stick (Pro) moves Mario; the D-pad does too while the stick is
 * centred or not fitted (Super Mario 64 has no use for the N64 D-pad).
 * Select+Start stays the menu's: held, it quits the game.
 */
#if defined(__linux__) && defined(TARGET_LINUX_KMSDRM)

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ultra64.h>

#include "controller_api.h"
#include "controller_rsos.h"

#define MAX_DEVS 8
#define STICK_DEADZONE 0.15f    /* of the half range */
#define DATA "/data/rsos"
#define SHIP "/usr/share/rsos"

/* RetroPad ids, in the menu's order (input.h enum input_btn) */
enum { RP_B, RP_Y, RP_SELECT, RP_START, RP_UP, RP_DOWN, RP_LEFT, RP_RIGHT,
       RP_A, RP_X, RP_L, RP_R, RP_L2, RP_R2, RP_L3, RP_R3, RP_NUM };

static const char *const rp_names[RP_NUM] = {
    "b", "y", "select", "start", "up", "down", "left", "right",
    "a", "x", "l", "r", "l2", "r2", "l3", "r3",
};

/* the built-in buttons, as the menu's input layer maps them */
static const struct { int code, rp; } builtin_keys[] = {
    { BTN_SOUTH, RP_B }, { BTN_EAST, RP_A }, { BTN_NORTH, RP_X }, { BTN_WEST, RP_Y },
    { BTN_TL, RP_L }, { BTN_TR, RP_R }, { BTN_TL2, RP_L2 }, { BTN_TR2, RP_R2 },
    { BTN_SELECT, RP_SELECT }, { BTN_START, RP_START }, { BTN_C, RP_L3 }, { BTN_Z, RP_R3 },
    { BTN_DPAD_UP, RP_UP }, { BTN_DPAD_DOWN, RP_DOWN }, { BTN_DPAD_LEFT, RP_LEFT },
    { BTN_DPAD_RIGHT, RP_RIGHT },
};

struct rsos_dev {
    int fd;
    bool has_stick;
    struct input_absinfo ax, ay;
};

static struct rsos_dev devs[MAX_DEVS];
static int ndevs;
static bool keys[KEY_MAX + 1];
static int abs_x, abs_y;
static unsigned char remap[RP_NUM];   /* physical RetroPad id -> RetroPad id, 0xff = none */
static bool alt_map;

/* ------------------------------------------------------------- ini files */

static char *trim(char *s) {
    char *e;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    return s;
}

/* calls fn(section, key, value, user) for every key = value line; false if
 * the file cannot be read */
static bool ini_each(const char *path, void (*fn)(const char *, const char *, const char *, void *),
                     void *user) {
    FILE *f = fopen(path, "r");
    char line[512], section[64] = "";

    if (f == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *c, *k, *v, *eq;

        if ((c = strchr(line, ';')) != NULL) *c = 0;
        if ((c = strchr(line, '#')) != NULL) *c = 0;
        k = trim(line);
        if (k[0] == '[') {
            snprintf(section, sizeof(section), "%s", k);
            continue;
        }
        if ((eq = strchr(k, '=')) == NULL) {
            continue;
        }
        *eq = 0;
        v = trim(eq + 1);
        k = trim(k);
        if (v[0] == '"' && strlen(v) >= 2 && v[strlen(v) - 1] == '"') {
            v[strlen(v) - 1] = 0;
            v++;
        }
        fn(section, k, v, user);
    }
    fclose(f);
    return true;
}

static int rp_id(const char *name) {
    for (int i = 0; i < RP_NUM; i++) {
        if (!strcasecmp(name, rp_names[i])) {
            return i;
        }
    }
    if (!strcasecmp(name, "l1")) return RP_L;
    if (!strcasecmp(name, "r1")) return RP_R;
    return -1;
}

static void remap_line(const char *section, const char *k, const char *v, void *user) {
    int from = rp_id(k), to = rp_id(v);

    (void)user;
    if (strcmp(section, "[remap]") != 0 && section[0] != 0) {
        return;   /* [options]: dpad_to_analog is always on here */
    }
    if (from >= 0 && (to >= 0 || !strcasecmp(v, "none"))) {
        remap[from] = to >= 0 ? (unsigned char)to : 0xff;
    }
}

static void flag_line(const char *section, const char *k, const char *v, void *user) {
    const char **want = user;

    (void)section;
    if (!strcmp(k, want[0])) {
        want[1] = (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
                   !strcasecmp(v, "on") || !strcasecmp(v, "enabled")) ? "1" : "0";
    }
}

/* "1"/"0" for key in the first file that has it, NULL if none does */
static const char *ini_flag(const char *key, const char *const *paths, int n) {
    static const char *want[2];

    for (int i = 0; i < n; i++) {
        want[0] = key;
        want[1] = NULL;
        if (ini_each(paths[i], flag_line, want) && want[1] != NULL) {
            return want[1];
        }
    }
    return NULL;
}

/* the emulator's N64 button settings (see the top of this file) */
static void load_n64_settings(void) {
    static const char *const settings[] = { DATA "/settings.ini" };
    static const char *const coreopts[] = { DATA "/coreopts/parallel_n64.ini", SHIP "/coreopts/parallel_n64.ini" };
    const char *cz = ini_flag("cz_buttons", settings, 1);
    const char *am = ini_flag("parallel-n64-alt-map", coreopts, 2);
    char cand[4][128];
    int n = 0;

    for (int i = 0; i < RP_NUM; i++) {
        remap[i] = (unsigned char)i;
    }
    if (cz != NULL && cz[0] == '1') {
        snprintf(cand[n++], sizeof(cand[0]), DATA "/remaps/n64-cz.ini");
    }
    snprintf(cand[n++], sizeof(cand[0]), DATA "/remaps/n64.ini");
    if (cz != NULL && cz[0] == '1') {
        snprintf(cand[n++], sizeof(cand[0]), SHIP "/remaps/n64-cz.ini");
    }
    snprintf(cand[n++], sizeof(cand[0]), SHIP "/remaps/n64.ini");
    for (int i = 0; i < n; i++) {
        if (ini_each(cand[i], remap_line, NULL)) {
            fprintf(stderr, "controller_rsos: N64 remap %s\n", cand[i]);
            break;
        }
    }
    alt_map = am != NULL && am[0] == '1';
    fprintf(stderr, "controller_rsos: independent C buttons %s\n", alt_map ? "on" : "off");
}

/* ------------------------------------------------------------- devices */

static void controller_rsos_init(void) {
    DIR *d = opendir("/dev/input");
    struct dirent *de;

    load_n64_settings();
    if (d == NULL) {
        return;
    }
    while ((de = readdir(d)) != NULL && ndevs < MAX_DEVS) {
        char path[300], name[128] = "";
        unsigned long absbits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
        int fd;

        if (strncmp(de->d_name, "event", 5) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        /* the buttons are "RetroStone2 ...", the stick (adc-joystick) is
         * named after its device tree node, as in the menu's board.ini */
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
            (strcmp(name, "analog-stick") != 0 &&
             (strncmp(name, "RetroStone2", 11) != 0 || strstr(name, "Brightness") != NULL))) {
            close(fd);
            continue;
        }
        devs[ndevs].fd = fd;
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
        if ((absbits[ABS_X / (8 * sizeof(long))] >> (ABS_X % (8 * sizeof(long)))) & 1) {
            devs[ndevs].has_stick = ioctl(fd, EVIOCGABS(ABS_X), &devs[ndevs].ax) == 0 &&
                                    ioctl(fd, EVIOCGABS(ABS_Y), &devs[ndevs].ay) == 0 &&
                                    devs[ndevs].ax.maximum != devs[ndevs].ax.minimum &&
                                    devs[ndevs].ay.maximum != devs[ndevs].ay.minimum;
            if (devs[ndevs].has_stick) {
                abs_x = devs[ndevs].ax.value;
                abs_y = devs[ndevs].ay.value;
            }
        }
        fprintf(stderr, "controller_rsos: %s (%s)%s\n", path, name,
                devs[ndevs].has_stick ? ", analog stick" : "");
        ndevs++;
    }
    closedir(d);
}

static bool axis_in_range(int v, const struct input_absinfo *a, bool near_centre) {
    int lo = a->minimum < a->maximum ? a->minimum : a->maximum;
    int hi = a->minimum < a->maximum ? a->maximum : a->minimum;

    if (near_centre) {
        return abs(v - (lo + (hi - lo) / 2)) <= (hi - lo) / 4;
    }
    return v >= lo && v <= hi;
}

/*
 * Fitted or not, as the menu decides (input.c): a reading out of range means
 * no stick (the ADC inputs float above it); fitted again only once both axes
 * read near the centre (floating inputs can wander into the range near an
 * edge and would hold a direction).
 */
static bool stick_fitted(const struct rsos_dev *sd) {
    static bool fitted;

    if (!axis_in_range(abs_x, &sd->ax, false) || !axis_in_range(abs_y, &sd->ay, false)) {
        if (fitted) {
            fprintf(stderr, "controller_rsos: analog stick reads %d, %d: not fitted\n", abs_x, abs_y);
        }
        fitted = false;
    } else if (!fitted && axis_in_range(abs_x, &sd->ax, true) && axis_in_range(abs_y, &sd->ay, true)) {
        fprintf(stderr, "controller_rsos: analog stick fitted (%d, %d)\n", abs_x, abs_y);
        fitted = true;
    }
    return fitted;
}

/* -1..1 from an axis, 0 in the dead zone */
static float stick_axis(int v, const struct input_absinfo *a) {
    int lo = a->minimum < a->maximum ? a->minimum : a->maximum;
    int hi = a->minimum < a->maximum ? a->maximum : a->minimum;
    float half = (hi - lo) / 2.0f, f;

    if (half <= 0) {
        return 0;
    }
    f = (v - (lo + half)) / half;
    if (f > -STICK_DEADZONE && f < STICK_DEADZONE) {
        return 0;
    }
    return f;
}

static void controller_rsos_read(OSContPad *pad) {
    const struct rsos_dev *sd = NULL;
    struct input_event ev;
    bool rp[RP_NUM] = { false };
    int dx, dy;

    for (int i = 0; i < ndevs; i++) {
        while (read(devs[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY && ev.code <= KEY_MAX) {
                keys[ev.code] = ev.value != 0;
            } else if (ev.type == EV_ABS && ev.code == ABS_X) {
                abs_x = ev.value;
            } else if (ev.type == EV_ABS && ev.code == ABS_Y) {
                abs_y = ev.value;
            }
        }
        if (devs[i].has_stick) {
            sd = &devs[i];
        }
    }

    /* Select + Start quits at once, as in the console's emulators (the
     * physical buttons, whatever the remap). The save file is written as
     * soon as the game saves, so nothing is lost. */
    {
        static bool start_was;
        bool start = keys[BTN_START];

        if (keys[BTN_SELECT] && start && !start_was) {
            fprintf(stderr, "controller_rsos: Select+Start: quitting\n");
            _exit(0);
        }
        start_was = start;
    }

    /* physical -> RetroPad -> the N64 remap */
    for (size_t i = 0; i < sizeof(builtin_keys) / sizeof(builtin_keys[0]); i++) {
        unsigned char to = remap[builtin_keys[i].rp];

        if (keys[builtin_keys[i].code] && to != 0xff) {
            rp[to] = true;
        }
    }

    /* RetroPad -> N64, as parallel_n64 */
    if (rp[RP_START]) pad->button |= START_BUTTON;
    if (rp[RP_L2]) pad->button |= Z_TRIG;
    if (alt_map) {
        if (rp[RP_B]) pad->button |= A_BUTTON;
        if (rp[RP_Y]) pad->button |= B_BUTTON;
        if (rp[RP_R]) pad->button |= R_CBUTTONS;
        if (rp[RP_L]) pad->button |= L_CBUTTONS;
        if (rp[RP_A]) pad->button |= D_CBUTTONS;
        if (rp[RP_X]) pad->button |= U_CBUTTONS;
        if (rp[RP_R2]) pad->button |= R_TRIG;
        if (rp[RP_SELECT]) pad->button |= L_TRIG;
    } else {
        if (rp[RP_R]) pad->button |= R_TRIG;
        if (rp[RP_L]) pad->button |= L_TRIG;
        if (rp[RP_R2]) {
            if (rp[RP_A]) pad->button |= R_CBUTTONS;
            if (rp[RP_Y]) pad->button |= L_CBUTTONS;
            if (rp[RP_B]) pad->button |= D_CBUTTONS;
            if (rp[RP_X]) pad->button |= U_CBUTTONS;
        } else {
            if (rp[RP_B]) pad->button |= A_BUTTON;
            if (rp[RP_Y]) pad->button |= B_BUTTON;
        }
    }

    /* the analog stick, when fitted and moved */
    if (sd != NULL && stick_fitted(sd)) {
        float fx = stick_axis(abs_x, &sd->ax), fy = stick_axis(abs_y, &sd->ay);

        if (fx != 0 || fy != 0) {
            int sx = (int)(fx * 80), sy = (int)(-fy * 80);

            if (abs(sx) > abs(pad->stick_x) || abs(sy) > abs(pad->stick_y)) {
                pad->stick_x = sx;
                pad->stick_y = sy;
            }
            return;
        }
    }
    /* else the D-pad steers (the remapped one, like the emulator's "D-pad
     * to analog"): full tilt, 56 on a diagonal (80 / sqrt 2) */
    dx = (rp[RP_RIGHT] ? 1 : 0) - (rp[RP_LEFT] ? 1 : 0);
    dy = (rp[RP_UP] ? 1 : 0) - (rp[RP_DOWN] ? 1 : 0);
    if (dx != 0 || dy != 0) {
        int m = dx != 0 && dy != 0 ? 56 : 80;

        pad->stick_x = dx * m;
        pad->stick_y = dy * m;
    }
}

struct ControllerAPI controller_rsos = {
    controller_rsos_init,
    controller_rsos_read
};

#endif
