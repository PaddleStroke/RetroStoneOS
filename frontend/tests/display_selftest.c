/*
 * display_selftest.c - checks the display layer's pure helpers: pixel
 * conversion (NEON on ARM, scalar elsewhere), the CPU integer scaler and
 * the scaling geometry. No DRM device needed.
 *
 * Built for the host by "make check", and for the target as
 * rsos-display-selftest, which is how the NEON paths get verified (and
 * timed) on the A20.
 */
#include "../src/display.c"

#include <stdio.h>

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

static uint32_t ref565(uint16_t p)
{
	uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;

	return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

static uint32_t ref1555(uint16_t p)
{
	uint32_t r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;

	return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static void test_convert(void)
{
	static uint16_t src[65536 + 7];
	static uint32_t dst[65536 + 7];
	int i, widths[] = { 65536, 13, 8, 7, 1 }, k;

	for (i = 0; i < 65536; i++)
		src[i] = (uint16_t)i;
	for (k = 0; k < 5; k++) {
		int w = widths[k];

		memset(dst, 0, sizeof(dst));
		convert_line(dst, (const uint8_t *)src, DRM_FORMAT_RGB565, w);
		for (i = 0; i < w; i++)
			if ((dst[i] & 0xffffff) != ref565(src[i])) {
				CHECK(0, "RGB565 w=%d px %d: 0x%04x -> 0x%08x, want 0x%06x", w, i,
				      src[i], dst[i], ref565(src[i]));
				break;
			}
		CHECK(dst[w] == 0, "RGB565 w=%d wrote past the end", w);

		memset(dst, 0, sizeof(dst));
		convert_line(dst, (const uint8_t *)src, DRM_FORMAT_XRGB1555, w);
		for (i = 0; i < w; i++)
			if ((dst[i] & 0xffffff) != ref1555(src[i])) {
				CHECK(0, "XRGB1555 w=%d px %d: 0x%04x -> 0x%08x, want 0x%06x", w, i,
				      src[i], dst[i], ref1555(src[i]));
				break;
			}
		CHECK(dst[w] == 0, "XRGB1555 w=%d wrote past the end", w);
	}
	printf("convert: RGB565 and XRGB1555 -> XRGB8888 (%s path)\n",
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	       "NEON"
#else
	       "scalar"
#endif
	);
}

static void test_blit_scaled(void)
{
	uint16_t src[3 * 2] = { 0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0x8410 };
	uint32_t buf[16 * 8];
	struct fbuf b;
	struct display_rect img = { 2, 1, 9, 6 };  /* 3x2 source, x3 */
	int x, y;

	memset(&b, 0, sizeof(b));
	memset(buf, 0xab, sizeof(buf));
	b.map = (uint8_t *)buf;
	b.pitch = 16 * 4;
	b.size = sizeof(buf);
	b.dirty = true;
	blit(&b, DRM_FORMAT_XRGB8888, &img, 3, 0, 0, (const uint8_t *)src, 3 * 2,
	     DRM_FORMAT_RGB565);
	for (y = 0; y < 8; y++)
		for (x = 0; x < 16; x++) {
			uint32_t v = buf[y * 16 + x] & 0xffffff;
			uint32_t want = 0;

			if (x >= 2 && x < 11 && y >= 1 && y < 7)
				want = ref565(src[((y - 1) / 3) * 3 + (x - 2) / 3]);
			CHECK(v == want, "x3 blit at %d,%d: 0x%06x want 0x%06x", x, y, v, want);
		}
	printf("blit: CPU integer scale x3 with borders\n");
}

static void test_geometry_math(void)
{
	struct display_rect r;
	int n, sx0, sy0;

	r = compute_scaled(320, 240, 640, 480, DISPLAY_SCALE_ASPECT, 0);
	CHECK(r.x == 0 && r.y == 0 && r.w == 640 && r.h == 480, "320x240 on LCD");
	r = compute_scaled(320, 240, 1280, 720, DISPLAY_SCALE_ASPECT, 0);
	CHECK(r.x == 160 && r.y == 0 && r.w == 960 && r.h == 720, "320x240 on 720p: %dx%d+%d+%d",
	      r.w, r.h, r.x, r.y);
	r = compute_scaled(256, 224, 640, 480, DISPLAY_SCALE_ASPECT, 4.0 / 3.0);
	CHECK(r.w == 640 && r.h == 480, "SNES 4:3 on LCD");
	r = compute_scaled(256, 224, 640, 480, DISPLAY_SCALE_INTEGER, 4.0 / 3.0);
	CHECK(r.x == 64 && r.y == 16 && r.w == 512 && r.h == 448, "SNES integer on LCD");
	r = compute_scaled(160, 144, 1280, 720, DISPLAY_SCALE_INTEGER, 0);
	CHECK(r.w == 800 && r.h == 720 && r.x == 240, "GB integer on 720p: %dx%d", r.w, r.h);
	r = compute_scaled(640, 480, 320, 240, DISPLAY_SCALE_INTEGER, 0);
	CHECK(r.w == 320 && r.h == 240, "downscale falls back to aspect");
	r = compute_scaled(256, 224, 1280, 720, DISPLAY_SCALE_STRETCH, 0);
	CHECK(r.w == 1280 && r.h == 720 && r.x == 0, "stretch");

	D.cfg.soft_scale_max = 2;
	D.sw = 320;
	D.sh = 240;
	soft_geometry(640, 480, &r, &n, &sx0, &sy0);
	CHECK(n == 2 && r.w == 640 && r.h == 480 && r.x == 0, "soft 320x240 on LCD");
	soft_geometry(1280, 720, &r, &n, &sx0, &sy0);
	CHECK(n == 2 && r.w == 640 && r.h == 480 && r.x == 320 && r.y == 120, "soft on 720p");
	D.sw = 640;
	D.sh = 480;
	soft_geometry(640, 480, &r, &n, &sx0, &sy0);
	CHECK(n == 1 && r.w == 640 && sx0 == 0, "soft 1:1");
	D.sw = 800;
	D.sh = 600;
	soft_geometry(640, 480, &r, &n, &sx0, &sy0);
	CHECK(n == 1 && r.w == 640 && r.h == 480 && sx0 == 80 && sy0 == 60, "soft crop");
	printf("geometry: aspect, integer, stretch, software fallback\n");
}

static void bench(void)
{
	static uint16_t src[320 * 240];
	static uint32_t dst[320 * 240];
	struct timespec a, b;
	int i, y;
	double ms;

	for (i = 0; i < 320 * 240; i++)
		src[i] = (uint16_t)(i * 2654435761u >> 16);
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (i = 0; i < 100; i++)
		for (y = 0; y < 240; y++)
			convert_line(dst + y * 320, (const uint8_t *)(src + y * 320),
				     DRM_FORMAT_RGB565, 320);
	clock_gettime(CLOCK_MONOTONIC, &b);
	ms = ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 100;
	printf("bench: RGB565 -> XRGB8888 320x240 in %.3f ms (cached memory)\n", ms);
}

/* The overlay plane: placement per output size and corner, plane choice. */
static void test_overlay(void)
{
	static const struct { int W, H; } outs[] = { { 640, 480 }, { 1280, 720 }, { 1920, 1080 }, { 720, 576 } };
	struct display_rect r;
	int i, c;

	for (i = 0; i < 4; i++) {
		int W = outs[i].W, H = outs[i].H, s = H >= 900 ? 2 : 1;
		int w = 72 * s, h = 20 * s, m = W == 640 ? 4 : H * 3 / 100;

		for (c = DISPLAY_CORNER_TOP_RIGHT; c <= DISPLAY_CORNER_BOTTOM_LEFT; c++) {
			bool right = c == DISPLAY_CORNER_TOP_RIGHT || c == DISPLAY_CORNER_BOTTOM_RIGHT;
			bool bottom = c == DISPLAY_CORNER_BOTTOM_RIGHT || c == DISPLAY_CORNER_BOTTOM_LEFT;

			CHECK(display_overlay_rect(W, H, w, h, (enum display_corner)c, m, &r), "%dx%d corner %d", W, H, c);
			CHECK(r.w == w && r.h == h, "size %dx%d", r.w, r.h);
			CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H,
			      "%dx%d corner %d: %d,%d %dx%d off screen", W, H, c, r.x, r.y, r.w, r.h);
			CHECK(r.x == (right ? W - w - m : m) && r.y == (bottom ? H - h - m : m),
			      "%dx%d corner %d: at %d,%d", W, H, c, r.x, r.y);
		}
	}
	/* 640x480 top-right with the LCD margin: the documented spot */
	display_overlay_rect(640, 480, 72, 20, DISPLAY_CORNER_TOP_RIGHT, 4, &r);
	CHECK(r.x == 564 && r.y == 4, "LCD top-right %d,%d", r.x, r.y);
	/* clamped inside a tiny screen, refused when it cannot fit */
	CHECK(display_overlay_rect(80, 30, 72, 20, DISPLAY_CORNER_BOTTOM_RIGHT, 20, &r) &&
	      r.x == 0 && r.y == 0, "clamped %d,%d", r.x, r.y);
	CHECK(!display_overlay_rect(60, 480, 72, 20, DISPLAY_CORNER_TOP_LEFT, 0, &r), "too wide accepted");
	CHECK(display_overlay_rect(640, 480, 72, 20, DISPLAY_CORNER_TOP_LEFT, -5, &r) && r.x == 0 && r.y == 0,
	      "negative margin");

	/* plane choice: an ARGB8888 overlay of the CRTC, never its game plane */
	memset(&D, 0, sizeof(D));
	D.ncrtc = 2;
	D.crtcs[0].primary = 0;
	D.crtcs[1].primary = 4;
	D.nplane = 8;
	for (i = 0; i < 8; i++) {
		D.planes[i].possible_crtcs = i < 4 ? 1 : 2;
		D.planes[i].type = i % 4 ? DRM_PLANE_TYPE_OVERLAY : DRM_PLANE_TYPE_PRIMARY;
		D.planes[i].argb8888 = true;
		D.planes[i].fb_id = 1;
	}
	D.planes[1].argb8888 = false;
	CHECK(ov_plane(0) == 2, "crtc 0 overlay plane %d", ov_plane(0));
	CHECK(ov_plane(1) == 5, "crtc 1 overlay plane %d", ov_plane(1));
	CHECK(ov_plane(2) == -1 && ov_plane(-1) == -1, "bad crtc");
	/* nothing wanted: nothing to commit; refused key: never retried */
	{
		struct ov_plan op;

		D.ov.front = -1;
		CHECK(!ov_prepare(0, 640, 480, true, &op), "overlay committed while not wanted");
		D.ov.want = true;
		D.ov.pix = calloc(72 * 20, 4);
		D.ov.w = 72;
		D.ov.h = 20;
		D.ov.corner = DISPLAY_CORNER_TOP_RIGHT;
		D.ov.margin = 4;
		D.ov.serial = 1;
		D.ov.failed = true;
		D.ov.failed_key.pi = 2;
		D.ov.failed_key.crtc_id = D.crtcs[0].id;
		display_overlay_rect(640, 480, 72, 20, DISPLAY_CORNER_TOP_RIGHT, 4, &D.ov.failed_key.dst);
		CHECK(!ov_prepare(0, 640, 480, false, &op), "refused overlay tried again");
		/* on screen with this content: a modeset re-sends it as is, a flip does not */
		D.ov.failed = false;
		D.ov.front = 1;
		D.ov.fb_serial[1] = 1;
		D.ov.fbw[1] = 72;
		D.ov.fbh[1] = 20;
		D.ov.on = D.ov.failed_key;
		CHECK(ov_prepare(0, 640, 480, true, &op) && op.show && op.b == 1, "modeset keeps it");
		CHECK(!ov_prepare(0, 640, 480, false, &op), "unchanged overlay re-committed");
		/* output switch to 720p: new place, same buffer */
		CHECK(ov_prepare(0, 1280, 720, true, &op) && op.show && op.key.dst.x == 1280 - 72 - 4,
		      "re-placed for 720p: x %d", op.key.dst.x);
		/* hidden: taken down by the next flip; a modeset just leaves it out */
		D.ov.want = false;
		CHECK(ov_prepare(0, 640, 480, false, &op) && !op.show && op.key.pi == 2, "hide");
		CHECK(!ov_prepare(0, 640, 480, true, &op), "modeset hides it by omission");
		free(D.ov.pix);
		memset(&D, 0, sizeof(D));
	}
}

/* LCD refresh (display-design.md §3.1): the retimed mode, the TCON clock
 * model, the policy in pick_mode(), the refresh measured from flip events. */
static void test_lcd_refresh(void)
{
	/* the RetroStone2 panel mode from the DT: 33 MHz, 800 x 525 */
	static const drmModeModeInfo panel = {
		.clock = 33000, .hdisplay = 640, .hsync_start = 656, .hsync_end = 686, .htotal = 800,
		.vdisplay = 480, .vsync_start = 490, .vsync_end = 493, .vtotal = 525, .vrefresh = 79,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED, .name = "640x480",
	};
	drmModeModeInfo m, hdmi = mode_cea_720p60;
	struct lcd_clock lc;
	struct conn c;
	bool builtin;

	CHECK(refresh_mhz(&panel) == 78571, "panel mode %d mHz", refresh_mhz(&panel));
	CHECK(display_parse_lcd_refresh("60") == 60 && display_parse_lcd_refresh("78") == 0 &&
	      display_parse_lcd_refresh(NULL) == 0 && display_parse_lcd_refresh("") == 0 &&
	      display_parse_lcd_refresh("legacy") == 0, "settings value lcd_refresh");
	CHECK(mode_retime(&panel, 60, &m) && m.clock == 25200 && refresh_mhz(&m) == 60000 && m.htotal == 800 &&
	      m.vtotal == 525 && m.hsync_start == 656 && m.vsync_end == 493 && m.flags == panel.flags &&
	      (m.type & DRM_MODE_TYPE_USERDEF) && !(m.type & DRM_MODE_TYPE_PREFERRED) && !strcmp(m.name, "640x480"),
	      "60 Hz: %u kHz, %d mHz, same porches", m.clock, refresh_mhz(&m));
	CHECK(!mode_retime(&panel, 78, &m) && !mode_retime(&panel, 0, &m) && !mode_retime(&panel, 200, &m),
	      "no retime for 78 Hz (already), 0 or out of range");
	CHECK(mode_retime(&panel, 50, &m) && m.clock == 21000 && refresh_mhz(&m) == 50000, "50 Hz: %u kHz", m.clock);

	/* what the A20's TCON0 makes of it: exact 25.2 MHz = pll-video 126 MHz / 5 */
	lcd_clock_model(25200000, &lc);
	CHECK(lc.parent_hz == 126000000 && lc.div == 5 && lc.pixel_hz == 25200000,
	      "25.2 MHz: pll %lu / %u = %lu Hz", lc.parent_hz, lc.div, lc.pixel_hz);
	lcd_clock_model(33000000, &lc);
	CHECK(lc.parent_hz == 132000000 && lc.div == 4 && lc.pixel_hz == 33000000,
	      "33 MHz (today): pll %lu / %u", lc.parent_hz, lc.div);
	lcd_clock_model(25175000, &lc);
	CHECK(lc.pixel_hz > 25100000 && lc.pixel_hz <= 25175000,
	      "25.175 MHz (VGA): nearest %lu Hz (pll %lu / %u)", lc.pixel_hz, lc.parent_hz, lc.div);
	CHECK(pll_video_round(297000000) == 297000000 && pll_video_round(1000000) == 27000000 &&
	      pll_video_round(500000000) == 381000000, "pll-video range 27..381 MHz, 297 fractional");

	/* pick_mode(): the LCD retimed only when configured, HDMI never */
	memset(&D, 0, sizeof(D));
	memset(&c, 0, sizeof(c));
	c.kind = DISPLAY_OUTPUT_LCD;
	c.modes = (drmModeModeInfo *)&panel;
	c.count_modes = 1;
	CHECK(pick_mode(&c, &m, &builtin) && m.clock == 33000, "LCD default: the panel's 33 MHz mode");
	D.cfg.lcd_refresh_hz = 60;
	CHECK(pick_mode(&c, &m, &builtin) && m.clock == 25200 && refresh_mhz(&m) == 60000,
	      "lcd_refresh 60: %u kHz", m.clock);
	c.kind = DISPLAY_OUTPUT_HDMI;
	c.modes = &hdmi;
	c.has_edid = true;
	D.cfg.hdmi_width = 1280;
	D.cfg.hdmi_height = 720;
	CHECK(pick_mode(&c, &m, &builtin) && m.clock == 74250, "HDMI untouched");
	/* a mode change is seen by the re-probe comparison */
	c.kind = DISPLAY_OUTPUT_LCD;
	c.modes = (drmModeModeInfo *)&panel;
	pick_mode(&c, &m, &builtin);
	CHECK(!mode_equal(&m, &panel), "retimed mode differs from the panel's (re-modeset)");
	CHECK(display_set_lcd_refresh(60) == 0 && display_get_lcd_refresh() == 60 &&
	      display_set_lcd_refresh(5) == 0 && display_get_lcd_refresh() == 0,
	      "setter before init: stored, out of range = the panel's mode");

	/* refresh measured from flip events: 60 Hz, then a gap restarts it */
	memset(&D, 0, sizeof(D));
	D.mode = panel;
	mode_retime(&panel, 60, &D.mode);
	meas_start();
	for (int i = 0; i <= 130; i++)
		meas_flip(1000 + (unsigned)i, 5000000 + (int64_t)i * 1000000 / 60);
	CHECK(!D.meas.on && D.info.measured_mhz == 60000, "measured %d mHz over 130 vblanks", D.info.measured_mhz);
	meas_start();
	meas_flip(10, 1000000);
	meas_flip(11, 1000000 + 16667);
	meas_flip(400, 9000000);                  /* 8 s later: the counter may have stopped */
	CHECK(D.meas.on && D.info.measured_mhz == 0, "a gap restarts the measurement");
	for (int i = 1; i <= 160; i++)             /* 78.571 Hz: 12727 us */
		meas_flip(400 + (unsigned)i, 9000000 + (int64_t)i * 12727);
	CHECK(!D.meas.on && abs(D.info.measured_mhz - 78573) <= 2, "measured %d mHz at 78.57 Hz",
	      D.info.measured_mhz);
	memset(&D, 0, sizeof(D));
}

int main(void)
{
	test_convert();
	test_blit_scaled();
	test_geometry_math();
	test_overlay();
	test_lcd_refresh();
	bench();
	printf("%s (%d failure(s))\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
