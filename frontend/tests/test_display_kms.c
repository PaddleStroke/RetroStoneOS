/*
 * test_display_kms.c - the display layer against a small KMS mock: the
 * atomic commits it makes on a RetroStone2-like device (panel "Unknown-1"
 * on CRTC 49 = index 0 / TCON0, HDMI-A-1 able to use CRTC 49 or 50, four
 * backend planes per CRTC) are applied to a model of the kernel state, and
 * the tests check that state after each step.
 *
 * Panel safety (display-design.md §8.5): with panel_keep_scanning, once the
 * panel is lit, no commit ever leaves its CRTC inactive or its connector
 * detached: screen off shows a black frame with the backlight off (bl_power
 * 4), HDMI runs on the other CRTC with the panel scanning black, a game
 * hand-off and the display closing keep it scanning, and the two CRTCs are
 * never modeset in one commit. Without the quirk (other boards), the old
 * behaviour is kept (ACTIVE = 0 for screen off, HDMI on the CRTC in use).
 *
 * Host only ("make check"): libdrm is replaced by the mocks below, the
 * dumb buffers live in a memfd.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/mman.h>

#define drmModeAtomicAlloc mock_drmModeAtomicAlloc
#define drmModeAtomicFree mock_drmModeAtomicFree
#define drmModeAtomicAddProperty mock_drmModeAtomicAddProperty
#define drmModeAtomicCommit mock_drmModeAtomicCommit
#define drmModeCreatePropertyBlob mock_drmModeCreatePropertyBlob
#define drmModeDestroyPropertyBlob mock_drmModeDestroyPropertyBlob
#define drmModeAddFB2 mock_drmModeAddFB2
#define drmModeRmFB mock_drmModeRmFB
#define drmIoctl mock_drmIoctl
#define drmSetMaster mock_drmSetMaster
#define drmDropMaster mock_drmDropMaster
#define drmHandleEvent mock_drmHandleEvent
#define drmModeGetConnector mock_drmModeGetConnector
#define drmModeGetConnectorCurrent mock_drmModeGetConnectorCurrent
#define drmModeFreeConnector mock_drmModeFreeConnector
#define drmModeObjectGetProperties mock_drmModeObjectGetProperties

#include "../src/display.c"

#include <stdio.h>
#include <sys/stat.h>

static int failures;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			failures++;                                        \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
			printf(__VA_ARGS__);                               \
			printf("\n");                                      \
		}                                                          \
	} while (0)

/* ------------------------------------------------------------ the model */

enum {
	P_CONN_CRTC = 100, P_ACTIVE, P_MODE_ID, P_TV_MODE,
	P_FB = 110, P_PCRTC, P_SX, P_SY, P_SW, P_SH, P_CX, P_CY, P_CW, P_CH,
};
#define LCD_CONN 10
#define HDMI_CONN 11
#define CRTC0 49
#define CRTC1 50
#define PLANE0 30   /* 30..33 on CRTC 49 (30 primary), 34..37 on CRTC 50 (34 primary) */

struct _drmModeAtomicReq {
	int n;
	struct { uint32_t obj, prop; uint64_t v; } e[512];
};

static struct {
	struct { uint32_t obj, prop; uint64_t v; } k[256];
	int nk;
	drmModeModeInfo blobs[64];
	bool blob_used[64];
	struct { uint32_t fb, handle; uint64_t off, size; } fbs[64];
	int nfb;
	uint32_t next_fb, next_handle;
	uint64_t next_off;
	struct { uint64_t off, size; } dumb[256];
	bool flip_pending;
	void *flip_ud;
	unsigned int seq;
	drmModeConnection hdmi_status;
	/* what the commits did */
	int commits, tests, active0_on_panel, panel_broken, joint_modesets;
	bool panel_was_lit, keep;
	int last_touch_mask;      /* CRTCs (bit 0 = 49, 1 = 50) touched by the last commit */
	int modeset_mask_seq[16]; /* CRTCs modeset by each real commit (first 16) */
	char bldir[256];
} K;

static uint64_t kget(uint32_t obj, uint32_t prop)
{
	for (int i = 0; i < K.nk; i++)
		if (K.k[i].obj == obj && K.k[i].prop == prop)
			return K.k[i].v;
	return 0;
}

static void kset(uint32_t obj, uint32_t prop, uint64_t v)
{
	for (int i = 0; i < K.nk; i++)
		if (K.k[i].obj == obj && K.k[i].prop == prop) {
			K.k[i].v = v;
			return;
		}
	if (K.nk < 256) {
		K.k[K.nk].obj = obj;
		K.k[K.nk].prop = prop;
		K.k[K.nk].v = v;
		K.nk++;
	}
}

drmModeAtomicReqPtr mock_drmModeAtomicAlloc(void)
{
	return calloc(1, sizeof(struct _drmModeAtomicReq));
}

void mock_drmModeAtomicFree(drmModeAtomicReqPtr r)
{
	free(r);
}

int mock_drmModeAtomicAddProperty(drmModeAtomicReqPtr r, uint32_t obj, uint32_t prop, uint64_t v)
{
	if (!r || r->n >= 512)
		return -ENOMEM;
	r->e[r->n].obj = obj;
	r->e[r->n].prop = prop;
	r->e[r->n].v = v;
	return ++r->n;
}

static const drmModeModeInfo *blob_mode(uint64_t id)
{
	return id >= 5000 && id < 5064 && K.blob_used[id - 5000] ? &K.blobs[id - 5000] : NULL;
}

int mock_drmModeCreatePropertyBlob(int fd, const void *data, size_t size, uint32_t *id)
{
	(void)fd;
	if (size != sizeof(drmModeModeInfo))
		return -EINVAL;
	for (int i = 0; i < 64; i++)
		if (!K.blob_used[i]) {
			K.blob_used[i] = true;
			memcpy(&K.blobs[i], data, size);
			*id = 5000 + (uint32_t)i;
			return 0;
		}
	return -ENOSPC;
}

int mock_drmModeDestroyPropertyBlob(int fd, uint32_t id)
{
	(void)fd;
	/* The kernel keeps a blob referenced by a CRTC state: keep its content. */
	(void)id;
	return 0;
}

/* A CRTC is modeset by a commit when it turns on/off or its mode changes. */
static bool crtc_modeset(uint32_t crtc, uint64_t old_active, uint64_t old_mode)
{
	const drmModeModeInfo *a = blob_mode(old_mode), *b = blob_mode(kget(crtc, P_MODE_ID));

	if (old_active != kget(crtc, P_ACTIVE))
		return true;
	if (!a || !b)
		return a != b;
	return !mode_equal(a, b);
}

static bool panel_scanning(void)
{
	return kget(CRTC0, P_ACTIVE) == 1 && kget(LCD_CONN, P_CONN_CRTC) == CRTC0 && blob_mode(kget(CRTC0, P_MODE_ID));
}

int mock_drmModeAtomicCommit(int fd, drmModeAtomicReqPtr r, uint32_t flags, void *ud)
{
	uint64_t a0 = kget(CRTC0, P_ACTIVE), m0 = kget(CRTC0, P_MODE_ID);
	uint64_t a1 = kget(CRTC1, P_ACTIVE), m1 = kget(CRTC1, P_MODE_ID);
	bool ms0, ms1;
	int i, mask = 0;

	(void)fd;
	if (flags & DRM_MODE_ATOMIC_TEST_ONLY) {
		K.tests++;
		return 0;
	}
	if ((flags & DRM_MODE_ATOMIC_NONBLOCK) && K.flip_pending)
		return -EBUSY;
	for (i = 0; i < r->n; i++) {
		if (r->e[i].obj == CRTC0 && r->e[i].prop == P_ACTIVE && r->e[i].v == 0)
			K.active0_on_panel++;
		kset(r->e[i].obj, r->e[i].prop, r->e[i].v);
		if (r->e[i].obj == CRTC0)
			mask |= 1;
		if (r->e[i].obj == CRTC1)
			mask |= 2;
	}
	ms0 = crtc_modeset(CRTC0, a0, m0);
	ms1 = crtc_modeset(CRTC1, a1, m1);
	if (ms0 && ms1 && kget(CRTC0, P_ACTIVE) && kget(CRTC1, P_ACTIVE))
		K.joint_modesets++;
	if (K.commits < 16)
		K.modeset_mask_seq[K.commits] = (ms0 ? 1 : 0) | (ms1 ? 2 : 0);
	K.commits++;
	K.last_touch_mask = mask;
	if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
		K.flip_pending = true;
		K.flip_ud = ud;
	}
	/* The invariant: once the panel scans, it scans after every commit. */
	if (panel_scanning())
		K.panel_was_lit = true;
	else if (K.keep && K.panel_was_lit)
		K.panel_broken++;
	return 0;
}

int mock_drmIoctl(int fd, unsigned long req, void *arg)
{
	(void)fd;
	if (req == DRM_IOCTL_MODE_CREATE_DUMB) {
		struct drm_mode_create_dumb *c = arg;
		uint32_t h = ++K.next_handle;

		c->pitch = c->width * (c->bpp / 8);
		c->size = (uint64_t)c->pitch * c->height;
		c->handle = h;
		if (h < 256) {
			K.dumb[h].off = K.next_off;
			K.dumb[h].size = c->size;
		}
		K.next_off += (c->size + 4095) & ~4095ull;
		return 0;
	}
	if (req == DRM_IOCTL_MODE_MAP_DUMB) {
		struct drm_mode_map_dumb *m = arg;

		m->offset = m->handle < 256 ? K.dumb[m->handle].off : 0;
		return 0;
	}
	if (req == DRM_IOCTL_MODE_DESTROY_DUMB)
		return 0;
	errno = EINVAL;
	return -1;
}

int mock_drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t fmt, const uint32_t handles[4],
		       const uint32_t pitches[4], const uint32_t offsets[4], uint32_t *id, uint32_t flags)
{
	(void)fd; (void)w; (void)h; (void)fmt; (void)pitches; (void)offsets; (void)flags;
	*id = ++K.next_fb;
	if (K.nfb < 64 && handles[0] < 256) {
		K.fbs[K.nfb].fb = *id;
		K.fbs[K.nfb].handle = handles[0];
		K.fbs[K.nfb].off = K.dumb[handles[0]].off;
		K.fbs[K.nfb].size = K.dumb[handles[0]].size;
		K.nfb++;
	}
	return 0;
}

/* atomic_remove_fb(): the planes that show it go off, their CRTC stays on
 * (sun4i accepts a CRTC without planes, so the plane-only commit wins). */
int mock_drmModeRmFB(int fd, uint32_t id)
{
	(void)fd;
	for (int p = PLANE0; p < PLANE0 + 8; p++)
		if (kget((uint32_t)p, P_FB) == id) {
			kset((uint32_t)p, P_FB, 0);
			kset((uint32_t)p, P_PCRTC, 0);
		}
	return 0;
}

int mock_drmSetMaster(int fd)
{
	(void)fd;
	return 0;
}

int mock_drmDropMaster(int fd)
{
	(void)fd;
	return 0;
}

int mock_drmHandleEvent(int fd, drmEventContextPtr ev)
{
	if (!K.flip_pending)
		return 0;
	K.flip_pending = false;
	K.seq++;
	ev->page_flip_handler2(fd, K.seq, 1, K.seq * 16667u, CRTC0, K.flip_ud);
	return 0;
}

static const drmModeModeInfo panel_mode = {
	.clock = 33000, .hdisplay = 640, .hsync_start = 656, .hsync_end = 686, .htotal = 800,
	.vdisplay = 480, .vsync_start = 490, .vsync_end = 493, .vtotal = 525, .vrefresh = 79,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED, .name = "640x480",
};

static drmModeConnectorPtr mock_conn(uint32_t id)
{
	drmModeConnectorPtr c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	c->connector_id = id;
	c->connection = id == HDMI_CONN ? K.hdmi_status : DRM_MODE_CONNECTED;
	c->count_modes = id == HDMI_CONN && K.hdmi_status != DRM_MODE_CONNECTED ? 0 : 1;
	c->modes = calloc(1, sizeof(drmModeModeInfo));
	if (c->modes)
		c->modes[0] = id == HDMI_CONN ? mode_cea_720p60 : panel_mode;
	return c;
}

drmModeConnectorPtr mock_drmModeGetConnector(int fd, uint32_t id)
{
	(void)fd;
	return mock_conn(id);
}

drmModeConnectorPtr mock_drmModeGetConnectorCurrent(int fd, uint32_t id)
{
	(void)fd;
	return mock_conn(id);
}

void mock_drmModeFreeConnector(drmModeConnectorPtr c)
{
	if (c)
		free(c->modes);
	free(c);
}

drmModeObjectPropertiesPtr mock_drmModeObjectGetProperties(int fd, uint32_t id, uint32_t type)
{
	(void)fd; (void)id; (void)type;
	return NULL;
}

/* ------------------------------------------------------------- helpers */

static void quiet(enum display_log_level lvl, const char *msg, void *user)
{
	(void)user;
	if (getenv("KMS_TEST_VERBOSE") || lvl == DISPLAY_LOG_ERROR)
		printf("    [display] %s\n", msg);
}

static const char *bl_power(void)
{
	static char v[16];
	char p[400];
	FILE *f;

	snprintf(p, sizeof(p), "%s/panel-bl/bl_power", K.bldir);
	v[0] = '\0';
	f = fopen(p, "r");
	if (f) {
		if (!fgets(v, sizeof(v), f))
			v[0] = '\0';
		fclose(f);
	}
	v[strcspn(v, "\n")] = '\0';
	return v;
}

static void bl_reset(void)
{
	char p[400];
	FILE *f;

	snprintf(p, sizeof(p), "%s/panel-bl", K.bldir);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/panel-bl/bl_power", K.bldir);
	f = fopen(p, "w");
	if (f) {
		fputs("0\n", f);
		fclose(f);
	}
}

/* True if framebuffer fb (as the kernel sees it) is all black. */
static bool fb_black(uint32_t fb)
{
	for (int i = 0; i < K.nfb; i++)
		if (K.fbs[i].fb == fb) {
			uint8_t *p = malloc(K.fbs[i].size);
			bool black = p && pread(D.fd, p, K.fbs[i].size, (off_t)K.fbs[i].off) == (ssize_t)K.fbs[i].size;

			for (uint64_t j = 0; black && j < K.fbs[i].size; j++)
				black = p[j] == 0;
			free(p);
			return black;
		}
	return false;
}

static void complete_flip(void)
{
	drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };

	mock_drmHandleEvent(D.fd, &ev);
}

/* display_init() on the mock: the objects enumerate() would have found,
 * the probe, the first output. */
static int mock_open(bool keep, bool hdmi)
{
	struct display_switch_timing tm = { 0 };
	static const uint32_t pprops[] = { P_FB, P_PCRTC, P_SX, P_SY, P_SW, P_SH, P_CX, P_CY, P_CW, P_CH };
	int i, want, fd;
	char bldir[256];

	snprintf(bldir, sizeof(bldir), "%s", K.bldir);
	memset(&K, 0, sizeof(K));
	snprintf(K.bldir, sizeof(K.bldir), "%s", bldir);
	K.keep = keep;
	K.next_fb = 700;
	K.hdmi_status = hdmi ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	bl_reset();

	fd = memfd_create("kms-mock", 0);
	if (fd < 0 || ftruncate(fd, 256 << 20) < 0)
		return -errno;

	memset(&D, 0, sizeof(D));
	D.fd = fd;
	D.ufd = D.tfd = D.efd = -1;
	D.conn = D.crtc = D.plane = -1;
	D.draw_idx = -1;
	D.active = true;
	D.ov.front = -1;
	display_config_defaults(&D.cfg);
	D.cfg.panel_keep_scanning = keep;
	D.cfg.backlight_dir = K.bldir;
	D.cfg.log = quiet;
	D.cfg.log_level = DISPLAY_LOG_DEBUG;
	D.cfg.internal_mode = DISPLAY_INTERNAL_LIST;
	D.cfg.internal_types = (1u << DRM_MODE_CONNECTOR_Unknown) | (1u << DRM_MODE_CONNECTOR_DPI);
	D.scale = D.cfg.scale;
	snprintf(D.devname, sizeof(D.devname), "card0");

	D.nconn = 2;
	D.conns[0] = (struct conn){ .id = LCD_CONN, .type = DRM_MODE_CONNECTOR_Unknown, .kind = DISPLAY_OUTPUT_LCD,
				    .possible_crtcs = 1, .prop_crtc_id = P_CONN_CRTC };
	snprintf(D.conns[0].name, sizeof(D.conns[0].name), "Unknown-1");
	D.conns[1] = (struct conn){ .id = HDMI_CONN, .type = DRM_MODE_CONNECTOR_HDMIA, .kind = DISPLAY_OUTPUT_HDMI,
				    .possible_crtcs = 3, .prop_crtc_id = P_CONN_CRTC };
	snprintf(D.conns[1].name, sizeof(D.conns[1].name), "HDMI-A-1");
	D.ncrtc = 2;
	for (i = 0; i < 2; i++) {
		D.crtcs[i] = (struct crtc){ .id = (uint32_t)(CRTC0 + i), .prop_active = P_ACTIVE,
					    .prop_mode_id = P_MODE_ID, .primary = i * 4, .ov_idx = -1 };
	}
	D.nplane = 8;
	for (i = 0; i < 8; i++) {
		struct plane *p = &D.planes[i];

		*p = (struct plane){ .id = (uint32_t)(PLANE0 + i), .possible_crtcs = i < 4 ? 1u : 2u,
				     .type = i % 4 ? DRM_PLANE_TYPE_OVERLAY : DRM_PLANE_TYPE_PRIMARY,
				     .xrgb8888 = true, .rgb565 = true, .argb8888 = true };
		p->fb_id = pprops[0]; p->crtc_id = pprops[1]; p->src_x = pprops[2]; p->src_y = pprops[3];
		p->src_w = pprops[4]; p->src_h = pprops[5]; p->crtc_x = pprops[6]; p->crtc_y = pprops[7];
		p->crtc_w = pprops[8]; p->crtc_h = pprops[9];
	}
	D.inited = true;
	probe_all(PROBE_INIT);
	/* EDID: the TV's modes are real (no built-in CEA fallback needed) */
	D.conns[1].has_edid = true;
	lcd_setup();
	want = choose_output();
	if (want < 0)
		return -ENODEV;
	tm.t_probe = display_now_ms();
	return do_switch(want, DISPLAY_EVENT_INIT, &tm);
}

/* A hotplug of HDMI, as reprobe() does it after the debounce. */
static void hotplug(bool connected)
{
	K.hdmi_status = connected ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	reprobe(DISPLAY_EVENT_HOTPLUG);
	D.conns[1].has_edid = connected;
}

static void present_red(void)
{
	static uint32_t px[320 * 240];

	for (int i = 0; i < 320 * 240; i++)
		px[i] = 0x00ff0000u;
	display_present_frame(px, 320, 240, 320 * 4);
	complete_flip();
}

static uint32_t game_plane_fb(int crtc_index)
{
	return (uint32_t)kget((uint32_t)(PLANE0 + crtc_index * 4), P_FB);
}

/* ----------------------------------------------------------------- tests */

/* 1. Screen off on the panel: backlight 0, black frame, CRTC ACTIVE. */
static void test_screen_off(void)
{
	static const uint32_t red[4 * 4] = { 0xffff0000u };
	uint32_t shown;

	printf("1. panel-keep-scanning: screen off on the panel\n");
	CHECK(mock_open(true, false) == 0, "open");
	CHECK(D.lcd.on && D.lcd.ci == 0 && D.lcd.ki == 0, "panel found: ci %d ki %d", D.lcd.ci, D.lcd.ki);
	CHECK(panel_scanning() && !strcmp(bl_power(), "0"), "lit, backlight on (%s)", bl_power());
	CHECK(display_set_game_surface(320, 240, DRM_FORMAT_XRGB8888, 0) != NULL, "surface");
	present_red();
	display_set_overlay(red, 4, 4, DISPLAY_CORNER_TOP_RIGHT, 4);
	complete_flip();
	shown = game_plane_fb(0);
	CHECK(shown && !fb_black(shown), "the game frame is on the panel");
	CHECK(kget(PLANE0 + 1, P_FB) != 0, "overlay on screen");

	CHECK(display_set_active(false) == 0, "screen off");
	CHECK(!display_is_active(), "reported off");
	CHECK(K.active0_on_panel == 0, "ACTIVE = 0 committed on the panel CRTC %d time(s)", K.active0_on_panel);
	CHECK(panel_scanning(), "panel CRTC still ACTIVE and routed");
	CHECK(game_plane_fb(0) == D.black.fb_id && fb_black(game_plane_fb(0)), "a black frame on the panel");
	CHECK(kget(PLANE0 + 1, P_FB) == 0, "overlay off");
	CHECK(!strcmp(bl_power(), "4"), "backlight off: bl_power %s", bl_power());
	/* presents while off: nothing reaches the screen */
	present_red();
	CHECK(game_plane_fb(0) == D.black.fb_id, "no flip while off");
	/* surface/scaling changes while off keep the black frame */
	display_set_scaling(DISPLAY_SCALE_INTEGER, 0);
	CHECK(game_plane_fb(0) == D.black.fb_id && panel_scanning(), "re-plan while off: still black, scanning");

	CHECK(display_set_active(true) == 0, "screen on");
	CHECK(panel_scanning() && game_plane_fb(0) != D.black.fb_id && !fb_black(game_plane_fb(0)),
	      "the newest frame is back");
	CHECK(kget(PLANE0 + 1, P_FB) != 0, "overlay back");
	CHECK(!strcmp(bl_power(), "0"), "backlight on: bl_power %s", bl_power());
	CHECK(K.active0_on_panel == 0 && K.panel_broken == 0, "never stopped (%d, %d)", K.active0_on_panel,
	      K.panel_broken);
	display_shutdown();
}

/* 2. HDMI: switch, not mirror; the panel keeps scanning black on CRTC 49. */
static void test_hdmi(void)
{
	uint32_t fb;

	printf("2. panel-keep-scanning: HDMI plugged and unplugged\n");
	CHECK(mock_open(true, false) == 0, "open");
	CHECK(display_set_game_surface(320, 240, DRM_FORMAT_XRGB8888, 0) != NULL, "surface");
	present_red();

	hotplug(true);
	CHECK(display_output()->type == DISPLAY_OUTPUT_HDMI, "on HDMI");
	CHECK(display_output()->crtc_index == 1 && display_output()->crtc_id == CRTC1,
	      "HDMI on the other CRTC: index %d id %u", display_output()->crtc_index, display_output()->crtc_id);
	CHECK(kget(HDMI_CONN, P_CONN_CRTC) == CRTC1 && kget(CRTC1, P_ACTIVE) == 1, "HDMI routed to CRTC 50, active");
	CHECK(panel_scanning(), "panel CRTC 49 still ACTIVE with its connector");
	CHECK(game_plane_fb(0) == D.black.fb_id && fb_black(game_plane_fb(0)), "the panel scans a black frame");
	fb = game_plane_fb(1);
	CHECK(fb && fb != D.black.fb_id, "the game on HDMI's plane");
	CHECK(!strcmp(bl_power(), "4"), "panel backlight off on HDMI: %s", bl_power());
	CHECK(K.joint_modesets == 0, "no commit modesets both CRTCs");

	/* frames on HDMI: flips on CRTC 50's plane only */
	present_red();
	present_red();
	CHECK(panel_scanning() && game_plane_fb(0) == D.black.fb_id, "flips leave the panel alone");

	/* screen off on HDMI: the TV loses its signal, the panel does not */
	CHECK(display_set_active(false) == 0, "screen off on HDMI");
	CHECK(kget(CRTC1, P_ACTIVE) == 0 && panel_scanning(), "HDMI CRTC off, panel CRTC on");
	CHECK(display_set_active(true) == 0 && kget(CRTC1, P_ACTIVE) == 1, "screen on HDMI");

	hotplug(false);
	CHECK(display_output()->type == DISPLAY_OUTPUT_LCD && display_output()->crtc_id == CRTC0, "back on the panel");
	CHECK(kget(CRTC1, P_ACTIVE) == 0 && kget(HDMI_CONN, P_CONN_CRTC) == 0, "HDMI off");
	CHECK(panel_scanning() && game_plane_fb(0) != D.black.fb_id && !fb_black(game_plane_fb(0)),
	      "the game back on the panel");
	CHECK(!strcmp(bl_power(), "0"), "panel backlight on: %s", bl_power());
	CHECK(K.active0_on_panel == 0 && K.panel_broken == 0 && K.joint_modesets == 0,
	      "panel never stopped (%d, %d), no joint modeset (%d)", K.active0_on_panel, K.panel_broken,
	      K.joint_modesets);
	display_shutdown();
}

/* 3. Start with HDMI plugged in: the panel is lit alone first. */
static void test_boot_hdmi(void)
{
	printf("3. panel-keep-scanning: boot with HDMI connected\n");
	CHECK(mock_open(true, true) == 0, "open");
	CHECK(display_output()->type == DISPLAY_OUTPUT_HDMI && display_output()->crtc_index == 1, "HDMI on CRTC 50");
	CHECK(K.commits >= 2 && K.modeset_mask_seq[0] == 1, "first commit: the panel alone (mask %d)",
	      K.modeset_mask_seq[0]);
	CHECK(K.modeset_mask_seq[1] == 2, "then HDMI alone (mask %d)", K.modeset_mask_seq[1]);
	CHECK(panel_scanning() && fb_black(game_plane_fb(0)) && !strcmp(bl_power(), "4"),
	      "panel scanning black, backlight off (%s)", bl_power());
	CHECK(K.joint_modesets == 0 && K.panel_broken == 0, "joint %d broken %d", K.joint_modesets, K.panel_broken);
	display_shutdown();
}

/* 4. Game hand-off: the child's exit takes its framebuffers away. */
static void test_handoff(void)
{
	printf("4. panel-keep-scanning: game hand-off (suspend, child exit, resume)\n");
	for (int hdmi = 0; hdmi < 2; hdmi++) {
		int before;

		CHECK(mock_open(true, hdmi) == 0, "open");
		CHECK(display_set_game_surface(320, 240, DRM_FORMAT_XRGB8888, 0) != NULL, "surface");
		present_red();
		CHECK(display_suspend() == 0, "suspend");
		CHECK(panel_scanning(), "suspended: the menu's frame keeps the panel scanning");
		/* the child's display_init + exit: planes emptied by atomic_remove_fb */
		for (int p = PLANE0; p < PLANE0 + 8; p++) {
			kset((uint32_t)p, P_FB, 0);
			kset((uint32_t)p, P_PCRTC, 0);
		}
		CHECK(panel_scanning(), "child gone: CRTC 49 still ACTIVE (plane-only removal)");
		before = K.commits;
		CHECK(display_resume() == 0, "resume");
		CHECK(panel_scanning() && K.panel_broken == 0 && K.joint_modesets == 0,
		      "%s: panel scanning after resume (broken %d, joint %d)", hdmi ? "HDMI" : "LCD",
		      K.panel_broken, K.joint_modesets);
		CHECK(K.commits > before, "re-committed");
		if (hdmi)
			CHECK(fb_black(game_plane_fb(0)) && game_plane_fb(0) && game_plane_fb(1) &&
			      !strcmp(bl_power(), "4"), "HDMI: panel black and dark, game on HDMI");
		else
			CHECK(game_plane_fb(0) && !fb_black(game_plane_fb(0)) && !strcmp(bl_power(), "0"),
			      "LCD: the menu back, backlight on");
		display_shutdown();
	}
}

/* 5. Closing the display (frontend exit before rcK's poweroff -f). */
static void test_close(void)
{
	int fb0;

	printf("5. panel-keep-scanning: the display closes\n");
	CHECK(mock_open(true, false) == 0, "open");
	CHECK(display_set_game_surface(320, 240, DRM_FORMAT_XRGB8888, 0) != NULL, "surface");
	present_red();
	display_shutdown();
	fb0 = (int)kget(PLANE0, P_FB);
	CHECK(fb0 == 0, "planes off");
	CHECK(kget(CRTC0, P_ACTIVE) == 1 && kget(LCD_CONN, P_CONN_CRTC) == CRTC0,
	      "the panel keeps scanning until the power is cut");
	CHECK(K.active0_on_panel == 0, "no ACTIVE = 0");
	/* with HDMI */
	CHECK(mock_open(true, true) == 0, "open on HDMI");
	display_shutdown();
	CHECK(kget(CRTC0, P_ACTIVE) == 1 && K.active0_on_panel == 0, "HDMI: the panel keeps scanning");
}

/* 6. Other boards (no quirk): unchanged behaviour. */
static void test_no_quirk(void)
{
	printf("6. without panel-keep-scanning: the previous behaviour\n");
	CHECK(mock_open(false, false) == 0, "open");
	CHECK(!D.lcd.on, "feature off");
	CHECK(display_set_game_surface(320, 240, DRM_FORMAT_XRGB8888, 0) != NULL, "surface");
	present_red();
	CHECK(display_set_active(false) == 0 && K.active0_on_panel == 1 && kget(CRTC0, P_ACTIVE) == 0,
	      "screen off = ACTIVE 0");
	CHECK(!strcmp(bl_power(), "0"), "backlight left to drm_panel/power");
	CHECK(display_set_active(true) == 0 && kget(CRTC0, P_ACTIVE) == 1, "screen on");
	hotplug(true);
	CHECK(display_output()->type == DISPLAY_OUTPUT_HDMI && display_output()->crtc_index == 0,
	      "HDMI keeps CRTC 0 (index %d)", display_output()->crtc_index);
	CHECK(kget(LCD_CONN, P_CONN_CRTC) == 0, "the panel is switched off (another board's panel)");
	display_shutdown();
}

/* 7. A board where HDMI could only use the panel's CRTC: HDMI is not used. */
static void test_single_crtc(void)
{
	struct display_switch_timing tm = { 0 };

	printf("7. panel-keep-scanning, HDMI only on the panel's CRTC: stays on the panel\n");
	CHECK(mock_open(true, false) == 0, "open");
	D.conns[1].possible_crtcs = 1;
	K.hdmi_status = DRM_MODE_CONNECTED;
	probe_all(PROBE_HOTPLUG);
	D.conns[1].has_edid = true;
	CHECK(choose_output() == 0, "the panel is chosen");
	CHECK(do_switch(1, DISPLAY_EVENT_HOTPLUG, &tm) != 0, "a forced switch is refused (no CRTC)");
	CHECK(panel_scanning() && K.panel_broken == 0, "panel scanning");
	display_shutdown();
}

/* True if the kernel's framebuffer fb has pixel (x, y) = v (pitch = w * 4). */
static bool fb_pixel(uint32_t fb, int w, int x, int y, uint32_t v)
{
	for (int i = 0; i < K.nfb; i++)
		if (K.fbs[i].fb == fb) {
			uint32_t px = 0;

			if (pread(D.fd, &px, 4, (off_t)(K.fbs[i].off + ((uint64_t)y * w + x) * 4)) != 4)
				return false;
			return px == v;
		}
	return false;
}

/* 8. The boot logo as the panel's picture behind HDMI (screen off: black). */
static void test_panel_picture(void)
{
	static uint32_t logo[100 * 50];
	uint32_t fb;

	printf("8. panel-keep-scanning: the logo on the panel behind HDMI\n");
	for (int i = 0; i < 100 * 50; i++)
		logo[i] = 0x00e0453au;
	CHECK(mock_open(true, false) == 0, "open");
	CHECK(display_set_panel_picture(logo, 100, 50, 0x002a2a35u) == 0, "picture set");
	CHECK(display_set_game_surface(320, 240, DRM_FORMAT_XRGB8888, 0) != NULL, "surface");
	present_red();
	CHECK(display_set_active(false) == 0 && fb_black(game_plane_fb(0)), "screen off stays black");
	CHECK(display_set_active(true) == 0, "screen on");
	hotplug(true);
	fb = game_plane_fb(0);
	CHECK(panel_scanning() && fb && fb == D.picfb.fb_id, "HDMI: the panel scans the logo (fb %u)", fb);
	CHECK(fb_pixel(fb, 640, 320, 240, 0x00e0453au) && fb_pixel(fb, 640, 0, 0, 0x002a2a35u) &&
	      fb_pixel(fb, 640, 269, 214, 0x002a2a35u) && fb_pixel(fb, 640, 270, 215, 0x00e0453au),
	      "logo centred on its background");
	CHECK(!strcmp(bl_power(), "4"), "backlight off: %s", bl_power());
	/* a new picture while on HDMI: shown at once, HDMI untouched */
	logo[0] = 0x00ffffffu;
	CHECK(display_set_panel_picture(logo, 100, 50, 0) == 0, "picture changed");
	CHECK(game_plane_fb(0) == D.picfb.fb_id && game_plane_fb(0) != fb && K.last_touch_mask == 0 &&
	      fb_pixel(game_plane_fb(0), 640, 270, 215, 0x00ffffffu) && fb_pixel(game_plane_fb(0), 640, 0, 0, 0),
	      "new logo on the panel plane only");
	CHECK(display_set_panel_picture(NULL, 0, 0, 0) == 0 && fb_black(game_plane_fb(0)), "no picture: black");
	CHECK(kget(CRTC1, P_ACTIVE) == 1 && K.panel_broken == 0 && K.joint_modesets == 0, "HDMI on, panel scanning");
	display_shutdown();
	/* boot on HDMI: black first, the logo once it is given */
	CHECK(mock_open(true, true) == 0, "open on HDMI");
	CHECK(fb_black(game_plane_fb(0)), "black before the logo is known");
	CHECK(display_set_panel_picture(logo, 100, 50, 0) == 0 && game_plane_fb(0) == D.picfb.fb_id, "then the logo");
	display_shutdown();
	/* other boards: ignored */
	CHECK(mock_open(false, false) == 0 && display_set_panel_picture(logo, 100, 50, 0) == 0 && !D.pic,
	      "no quirk: nothing kept");
	display_shutdown();
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");

	snprintf(K.bldir, sizeof(K.bldir), "%s/rsos-kms-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(K.bldir)) {
		perror("mkdtemp");
		return 1;
	}
	test_screen_off();
	test_hdmi();
	test_boot_hdmi();
	test_handoff();
	test_close();
	test_no_quirk();
	test_single_crtc();
	test_panel_picture();
	{
		char cmd[400];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", K.bldir);
		if (system(cmd) != 0)
			printf("warning: %s not removed\n", K.bldir);
	}
	printf("%s (%d failure(s))\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
