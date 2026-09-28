/*
 * display.c - RetroStoneOS display layer: atomic KMS, live LCD/HDMI
 * switching, hardware-scaled game plane with a software fallback.
 *
 * See display.h for the API and docs/display-design.md for the design
 * (pipeline, switching sequence, scaler constraints from the sun4i driver,
 * buffer strategy, fallback).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "display.h"
#include "uevent.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_CONN  8
#define MAX_CRTC  4
#define MAX_PLANE 16

#define FLIP_DRAIN_MS 100   /* max wait for an outstanding flip before a modeset */
#define PRESENT_WAIT_MS 100 /* max wait for a free scanout buffer in present */
#define FLIP_STUCK_MS 1000  /* a flip event this late is lost: re-commit (flip_watchdog) */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

struct conn {
	uint32_t id;
	uint32_t type;
	enum display_output_type kind;
	uint32_t possible_crtcs;
	char name[32];
	uint32_t prop_crtc_id;
	uint32_t prop_edid;

	drmModeConnection status;
	drmModeModeInfo *modes;
	int count_modes;
	bool has_edid;
	char monitor[16];
	bool failed;          /* last modeset on it failed; retried on next hotplug */
};

struct crtc {
	uint32_t id;
	uint32_t prop_active;
	uint32_t prop_mode_id;
	int primary;          /* index in planes[] of the plane we use, -1 none */
};

struct plane {
	uint32_t id;
	uint32_t possible_crtcs;
	uint64_t type;
	bool xrgb8888, rgb565, argb8888;
	uint32_t fb_id, crtc_id, src_x, src_y, src_w, src_h;
	uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
};

enum bstate { B_FREE = 0, B_DRAW, B_QUEUED, B_PENDING, B_FRONT };

struct fbuf {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	uint8_t *map;
	enum bstate st;
	bool dirty;           /* needs clearing to black before next use */
	/* Scaled path: image size in this buffer and where it goes on screen. */
	int fw, fh;
	struct display_rect dst;
};

struct bufset {
	struct fbuf b[DISPLAY_MAX_BUFFERS];
	int n, w, h;
	uint32_t format;         /* DRM_FORMAT_XRGB8888 or DRM_FORMAT_RGB565 */
	struct display_rect img; /* image rect last blitted (unscaled path) */
};

/*
 * PATH_SCALED: game-sized buffers on the plane, scaled by the display
 * engine. PATH_SOFT: the fallback, screen-sized buffers filled by the CPU
 * (integer nearest-neighbour scale, or a plain unscaled blit).
 */
enum path { PATH_NONE = 0, PATH_SCALED, PATH_SOFT, PATH_BLANK };

static const char *const path_names[] = {
	"none", "hw-scaled", "sw-blit", "blank",
};

struct strategy {
	enum path path;
	uint32_t fmt;
};

#define MAX_TRIES 3

struct plan {
	enum path path;
	uint32_t fmt;              /* framebuffer format of this plan */
	struct bufset *set;
	int idx;
	bool direct;
	struct display_rect dst;   /* plane CRTC_* rect */
	int src_w, src_h;          /* plane SRC_* size */
	struct display_rect img;   /* where the image goes inside the buffer */
	int n;                     /* CPU scale factor (PATH_SOFT) */
	int sx0, sy0;              /* crop offset in the source image */
};

/* Strategy cache: which try worked for a given geometry. */
struct strat_cache {
	bool valid;
	uint32_t crtc_id;
	int W, H, sw, sh;
	uint32_t sfmt;
	unsigned int sflags;
	struct display_rect dst;
	int ok;                    /* index in the try list that succeeded */
};

#define STRAT_CACHE_SIZE 16

struct ext_fb {
	bool valid;
	struct display_fb fb;
	struct display_rect dst;
};

struct frame_src {
	const uint8_t *p;
	int stride;
	uint32_t fmt;
	bool valid;
};

/*
 * The overlay plane (display_set_overlay): a CPU copy of the content, two
 * ARGB8888 dumb buffers (the one on screen is never written), and what the
 * plane shows now. Updates ride on page flips; see ov_prepare().
 */
struct ov_key {
	int pi;                    /* plane index */
	uint32_t crtc_id;
	struct display_rect dst;
};

struct overlay {
	bool want;                 /* set and not hidden */
	uint32_t *pix;             /* w x h, the latest content */
	int w, h, margin;
	enum display_corner corner;
	uint64_t serial;           /* content version */
	int64_t dirty_since;       /* ms; 0 = nothing to commit */
	struct fbuf fb[2];
	int fbw[2], fbh[2];
	uint64_t fb_serial[2];     /* content version in each buffer */
	int front;                 /* buffer on screen, -1 = plane off */
	struct ov_key on;          /* where it is on screen (front >= 0) */
	bool pend_valid;           /* a non-blocking commit carries it */
	int pend;                  /* buffer in that commit, -1 = it hides the plane */
	struct ov_key pend_key;
	bool tested;               /* key that passed TEST_ONLY */
	struct ov_key tested_key;
	bool failed;               /* key that was refused: nothing shown there */
	struct ov_key failed_key;
};

/* What the next commit does with the overlay. */
struct ov_plan {
	bool show;                 /* true: plane on with fb[b] at key; false: plane off */
	int b;
	struct ov_key key;
};

static struct {
	bool inited;
	struct display_config cfg;
	int fd, ufd, tfd, efd;
	char devname[32];

	struct conn conns[MAX_CONN];
	int nconn;
	struct crtc crtcs[MAX_CRTC];
	int ncrtc;
	struct plane planes[MAX_PLANE];
	int nplane;

	/* Current pipeline. */
	int conn, crtc, plane;
	drmModeModeInfo mode;
	uint32_t mode_blob;
	struct display_output_info info;

	/* Game surface. */
	bool surf_set;
	bool fresh;           /* surface just (re)created: nothing to carry over */
	int cw, ch;           /* capacity: buffers are allocated at this size */
	int sw, sh, sbpp;     /* current frame size (<= capacity) */
	uint32_t sfmt;
	unsigned int sflags;
	uint8_t *shadow;
	int shadow_stride;
	bool shadow_valid;
	enum display_scale_mode scale;
	double aspect;
	struct display_surface view;

	enum path path;
	bool direct;
	struct bufset game, screen, blank;
	struct bufset *act;
	struct display_rect dst, img;
	int soft_n;
	int sx0, sy0;
	struct strat_cache cache[STRAT_CACHE_SIZE];
	int cache_next;
	/* Plane geometry of the last commit (scaled path). */
	int geo_fw, geo_fh;
	struct display_rect geo_dst;

	/* Frames. */
	uint64_t gen;
	bool flip_pending;
	int draw_idx;
	uint64_t draw_gen;
	bool draw_shadow;

	/* Hotplug. */
	int timer_reason;
	int edid_retries_left;
	int64_t t_uevent;
	bool reprobe_pending;   /* hotplug seen while suspended or inactive */

	/* Power / hand-off. */
	bool active;            /* CRTC ACTIVE (display_set_active) */
	bool suspended;         /* master dropped (display_suspend) */

	/* External framebuffers (display_present_fb). */
	struct ext_fb ext_front;   /* on screen (or hidden by a surface frame) */
	struct ext_fb ext_pending; /* committed, waiting for its flip event */
	bool ext_shown;            /* the plane shows ext_front */
	bool flip_ext;             /* the flip in flight is an external FB */
	struct ext_fb ext_tested;  /* last geometry that passed TEST_ONLY */
	uint32_t ext_tested_crtc;

	/* Overlay plane (display_set_overlay). */
	struct overlay ov;
	bool flip_ov;              /* the flip in flight only changes the overlay */
	int64_t flip_at;           /* when the flip in flight was queued (ms) */

	/* Refresh measured from the page-flip events after a modeset (§3.1). */
	struct {
		bool on, have0;
		uint32_t seq0, seql;   /* vblank sequence: first and last event */
		int64_t t0, tl;        /* their kernel timestamps (us) */
	} meas;

	struct display_stats stats;
} D = { .fd = -1, .ufd = -1, .tfd = -1, .efd = -1 };

enum { TIMER_NONE = 0, TIMER_DEBOUNCE, TIMER_EDID_RETRY };

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

int64_t display_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void dlog(enum display_log_level lvl, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void dlog(enum display_log_level lvl, const char *fmt, ...)
{
	char msg[512];
	va_list ap;

	if (lvl > D.cfg.log_level)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (D.cfg.log) {
		D.cfg.log(lvl, msg, D.cfg.user);
	} else {
		int64_t t = display_now_ms();
		fprintf(stderr, "[%6lld.%03lld] display: %s\n",
			(long long)(t / 1000), (long long)(t % 1000), msg);
	}
}

static int bpp_of(uint32_t fmt)
{
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return 4;
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_XRGB1555:
		return 2;
	default:
		return 0;
	}
}

static bool fmt_is_32(uint32_t fmt)
{
	return fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_ARGB8888;
}

/*
 * Framebuffer format that holds a surface format without conversion, 0 if
 * none. ARGB8888 is scanned out as XRGB8888 (the frontend has no alpha
 * formats, sun4i_frontend.c); XRGB1555 is not a plane format on sun4i.
 */
static uint32_t native_fb_fmt(uint32_t sfmt)
{
	if (fmt_is_32(sfmt))
		return DRM_FORMAT_XRGB8888;
	if (sfmt == DRM_FORMAT_RGB565)
		return DRM_FORMAT_RGB565;
	return 0;
}

static int refresh_mhz(const drmModeModeInfo *m)
{
	int64_t num, den;

	if (!m->htotal || !m->vtotal)
		return m->vrefresh * 1000;
	num = (int64_t)m->clock * 1000000;
	den = (int64_t)m->htotal * m->vtotal;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (m->vscan > 1)
		den *= m->vscan;
	return (int)((num + den / 2) / den);
}

static bool mode_equal(const drmModeModeInfo *a, const drmModeModeInfo *b)
{
	return a->clock == b->clock &&
	       a->hdisplay == b->hdisplay && a->hsync_start == b->hsync_start &&
	       a->hsync_end == b->hsync_end && a->htotal == b->htotal &&
	       a->vdisplay == b->vdisplay && a->vsync_start == b->vsync_start &&
	       a->vsync_end == b->vsync_end && a->vtotal == b->vtotal &&
	       (a->flags & ~DRM_MODE_FLAG_PIC_AR_MASK) ==
	       (b->flags & ~DRM_MODE_FLAG_PIC_AR_MASK);
}

/*
 * LCD refresh (display-design.md §3.1). The panel's DT mode is 640x480 with
 * htotal 800 x vtotal 525 at 33 MHz, i.e. 78.571 Hz. The same timings with
 * the pixel clock set for `hz` (25.2 MHz for 60 Hz) are committed as a user
 * mode: sun4i_rgb_mode_valid() skips its clock check for a panel, and
 * sun4i_tcon0_mode_set_rgb() calls clk_set_rate(dclk, mode clock) in the
 * modeset, so no reboot or DT overlay is needed.
 */
int display_parse_lcd_refresh(const char *v)
{
	return v && atoi(v) == 60 ? 60 : 0;
}

/* The mode m re-clocked for `hz`. False (out untouched) when m already runs
 * at that rate (within 1 Hz: 78 = the 78.571 Hz panel mode), hz is out of 30..120, or m has no totals. */
static bool mode_retime(const drmModeModeInfo *m, int hz, drmModeModeInfo *out)
{
	uint64_t clk;

	if (hz < 30 || hz > 120 || !m->htotal || !m->vtotal || (m->flags & DRM_MODE_FLAG_INTERLACE))
		return false;
	if (abs(refresh_mhz(m) - hz * 1000) < 1000)
		return false;
	clk = ((uint64_t)m->htotal * m->vtotal * (uint64_t)hz + 500) / 1000;   /* kHz */
	*out = *m;
	out->clock = (uint32_t)clk;
	out->vrefresh = (uint32_t)hz;
	out->type = DRM_MODE_TYPE_USERDEF;
	snprintf(out->name, sizeof(out->name), "%dx%d", m->hdisplay, m->vdisplay);
	return true;
}

/*
 * What the A20's TCON0 will make of a pixel clock request (for the log and
 * the tests; the kernel decides): sun4i_dclk_round_rate() tries the divider
 * 4..127 (sun7i_a20_tcon0_quirks.dclk_min_div = 4) and takes the first exact
 * one, else the closest; the parent is tcon0-ch0-sclk (CLK_SET_RATE_PARENT),
 * a mux over pll-video0/1 and their 2x, which offers the largest rate <= the
 * request that a pll-video can make: 3 MHz x (9..127) (24 MHz / pre-divider
 * 8, ccu-sun4i-a10.c), or 270 / 297 MHz in fractional mode.
 */
struct lcd_clock {
	unsigned long parent_hz;   /* pll-video (or its 2x) rate */
	unsigned div;
	unsigned long pixel_hz;
};

static unsigned long pll_video_round(unsigned long rate)
{
	unsigned long m;

	if (rate == 270000000UL || rate == 297000000UL)
		return rate;
	m = rate / 3000000UL;
	if (m < 9)
		m = 9;
	if (m > 127)
		m = 127;
	return m * 3000000UL;
}

static unsigned long disp_mux_round(unsigned long rate)
{
	unsigned long best = 0, c[2] = { pll_video_round(rate), 2 * pll_video_round(rate / 2) };

	for (int i = 0; i < 2; i++)
		if (c[i] <= rate && c[i] > best)
			best = c[i];
	return best ? best : c[0];
}

static void lcd_clock_model(unsigned long rate, struct lcd_clock *out)
{
	unsigned long best_parent = 0;
	unsigned best_div = 1;

	for (unsigned i = 4; i <= 127; i++) {
		uint64_t ideal = (uint64_t)rate * i;
		unsigned long r;

		if (ideal > 0xffffffffULL)
			break;
		r = disp_mux_round((unsigned long)ideal);
		if (r == ideal) {
			best_parent = r;
			best_div = i;
			break;
		}
		if (labs((long)rate - (long)(r / i)) < labs((long)rate - (long)(best_parent / best_div))) {
			best_parent = r;
			best_div = i;
		}
	}
	out->parent_hz = best_parent;
	out->div = best_div;
	out->pixel_hz = best_div ? best_parent / best_div : 0;
}

/* CEA-861 VIC 4: 1280x720p @ 60 Hz, 74.25 MHz. Used when there is no EDID. */
static const drmModeModeInfo mode_cea_720p60 = {
	.clock = 74250,
	.hdisplay = 1280, .hsync_start = 1390, .hsync_end = 1430, .htotal = 1650,
	.vdisplay = 720, .vsync_start = 725, .vsync_end = 730, .vtotal = 750,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	.type = DRM_MODE_TYPE_DRIVER,
	.name = "1280x720",
};

/* CEA-861 VIC 1: 640x480p @ 59.94 Hz, 25.175 MHz; every HDMI sink must take it. */
static const drmModeModeInfo mode_cea_480p60 = {
	.clock = 25175,
	.hdisplay = 640, .hsync_start = 656, .hsync_end = 752, .htotal = 800,
	.vdisplay = 480, .vsync_start = 490, .vsync_end = 492, .vtotal = 525,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.type = DRM_MODE_TYPE_DRIVER,
	.name = "640x480",
};

/* ------------------------------------------------------------------ */
/* Properties                                                          */
/* ------------------------------------------------------------------ */

struct prop_want {
	const char *name;
	uint32_t *id;
	uint64_t *value;
};

static void load_props(uint32_t obj, uint32_t type, struct prop_want *w, int n)
{
	drmModeObjectProperties *props;
	uint32_t i;
	int k;

	props = drmModeObjectGetProperties(D.fd, obj, type);
	if (!props)
		return;
	for (i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(D.fd, props->props[i]);

		if (!p)
			continue;
		for (k = 0; k < n; k++) {
			if (strcmp(p->name, w[k].name) == 0) {
				if (w[k].id)
					*w[k].id = p->prop_id;
				if (w[k].value)
					*w[k].value = props->prop_values[i];
			}
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
}

/* ------------------------------------------------------------------ */
/* Enumeration and probing                                             */
/* ------------------------------------------------------------------ */

static enum display_output_type classify(uint32_t type)
{
	enum display_output_type internal = DISPLAY_OUTPUT_LCD;

	if (D.cfg.internal_mode == DISPLAY_INTERNAL_NONE ||
	    (D.cfg.internal_mode == DISPLAY_INTERNAL_LIST &&
	     (type >= 32 || !(D.cfg.internal_types & (1u << type)))))
		internal = DISPLAY_OUTPUT_NONE;
	switch (type) {
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_HDMIB:
	case DRM_MODE_CONNECTOR_DVII:
	case DRM_MODE_CONNECTOR_DVID:
	case DRM_MODE_CONNECTOR_DVIA:
	case DRM_MODE_CONNECTOR_DisplayPort:
	case DRM_MODE_CONNECTOR_VGA:
	case DRM_MODE_CONNECTOR_TV:
	case DRM_MODE_CONNECTOR_Composite:
	case DRM_MODE_CONNECTOR_SVIDEO:
	case DRM_MODE_CONNECTOR_Component:
		return DISPLAY_OUTPUT_HDMI;
	default:
		/*
		 * DPI, LVDS, eDP, DSI, Virtual and Unknown. Note that sun4i_rgb.c
		 * registers the parallel RGB panel connector as
		 * DRM_MODE_CONNECTOR_Unknown, not DPI. The board profile can
		 * narrow this down (internal_mode): NONE = not used at all.
		 */
		return internal;
	}
}

static void edid_monitor_name(const uint8_t *e, size_t len, char *out, size_t outsz)
{
	int d;

	out[0] = '\0';
	if (len < 128 || outsz < 2)
		return;
	for (d = 54; d <= 108; d += 18) {
		const uint8_t *p = e + d;
		size_t i;

		if (p[0] || p[1] || p[2] || p[3] != 0xfc)
			continue;
		for (i = 0; i < 13 && i < outsz - 1 && p[5 + i] != 0x0a; i++)
			out[i] = isprint(p[5 + i]) ? (char)p[5 + i] : '?';
		while (i > 0 && out[i - 1] == ' ')
			i--;
		out[i] = '\0';
		return;
	}
}

static void probe_conn(struct conn *c, bool full)
{
	drmModeConnector *mc;
	uint64_t edid_blob = 0;

	mc = full ? drmModeGetConnector(D.fd, c->id)
		  : drmModeGetConnectorCurrent(D.fd, c->id);
	if (!mc) {
		c->status = DRM_MODE_UNKNOWNCONNECTION;
		return;
	}
	c->status = mc->connection;
	free(c->modes);
	c->modes = NULL;
	c->count_modes = 0;
	if (mc->count_modes > 0) {
		c->modes = calloc((size_t)mc->count_modes, sizeof(*c->modes));
		if (c->modes) {
			memcpy(c->modes, mc->modes,
			       (size_t)mc->count_modes * sizeof(*c->modes));
			c->count_modes = mc->count_modes;
		}
	}
	drmModeFreeConnector(mc);

	/* The EDID property blob is refreshed by the probe above. */
	c->has_edid = false;
	c->monitor[0] = '\0';
	{
		struct prop_want w[] = { { "EDID", &c->prop_edid, &edid_blob } };
		load_props(c->id, DRM_MODE_OBJECT_CONNECTOR, w, 1);
	}
	if (edid_blob) {
		drmModePropertyBlobRes *b = drmModeGetPropertyBlob(D.fd, (uint32_t)edid_blob);

		if (b) {
			c->has_edid = b->length >= 128;
			edid_monitor_name(b->data, b->length, c->monitor, sizeof(c->monitor));
			drmModeFreePropertyBlob(b);
		}
	}
}

static int enumerate(void)
{
	drmModeRes *res;
	drmModePlaneRes *pres;
	int i, j;

	res = drmModeGetResources(D.fd);
	if (!res)
		return -errno ? -errno : -ENODEV;

	D.ncrtc = 0;
	for (i = 0; i < res->count_crtcs && D.ncrtc < MAX_CRTC; i++) {
		struct crtc *c = &D.crtcs[D.ncrtc++];
		struct prop_want w[] = {
			{ "ACTIVE", &c->prop_active, NULL },
			{ "MODE_ID", &c->prop_mode_id, NULL },
		};

		memset(c, 0, sizeof(*c));
		c->id = res->crtcs[i];
		c->primary = -1;
		load_props(c->id, DRM_MODE_OBJECT_CRTC, w, 2);
	}

	D.nconn = 0;
	for (i = 0; i < res->count_connectors && D.nconn < MAX_CONN; i++) {
		drmModeConnector *mc = drmModeGetConnectorCurrent(D.fd, res->connectors[i]);
		struct conn *c;
		const char *tn;

		if (!mc)
			continue;
		c = &D.conns[D.nconn++];
		memset(c, 0, sizeof(*c));
		c->id = mc->connector_id;
		c->type = mc->connector_type;
		c->kind = classify(mc->connector_type);
		tn = drmModeGetConnectorTypeName(mc->connector_type);
		snprintf(c->name, sizeof(c->name), "%s-%u", tn ? tn : "Unknown",
			 mc->connector_type_id);
		for (j = 0; j < mc->count_encoders; j++) {
			drmModeEncoder *e = drmModeGetEncoder(D.fd, mc->encoders[j]);

			if (e) {
				c->possible_crtcs |= e->possible_crtcs;
				drmModeFreeEncoder(e);
			}
		}
		{
			struct prop_want w[] = {
				{ "CRTC_ID", &c->prop_crtc_id, NULL },
				{ "EDID", &c->prop_edid, NULL },
			};
			load_props(c->id, DRM_MODE_OBJECT_CONNECTOR, w, 2);
		}
		drmModeFreeConnector(mc);
	}
	drmModeFreeResources(res);

	pres = drmModeGetPlaneResources(D.fd);
	if (!pres)
		return -errno ? -errno : -ENODEV;
	D.nplane = 0;
	for (i = 0; i < (int)pres->count_planes && D.nplane < MAX_PLANE; i++) {
		drmModePlane *mp = drmModeGetPlane(D.fd, pres->planes[i]);
		struct plane *p;
		uint32_t k;

		if (!mp)
			continue;
		p = &D.planes[D.nplane++];
		memset(p, 0, sizeof(*p));
		p->id = mp->plane_id;
		p->possible_crtcs = mp->possible_crtcs;
		for (k = 0; k < mp->count_formats; k++) {
			if (mp->formats[k] == DRM_FORMAT_XRGB8888)
				p->xrgb8888 = true;
			if (mp->formats[k] == DRM_FORMAT_ARGB8888)
				p->argb8888 = true;
			if (mp->formats[k] == DRM_FORMAT_RGB565)
				p->rgb565 = true;
		}
		drmModeFreePlane(mp);
		{
			struct prop_want w[] = {
				{ "type", NULL, &p->type },
				{ "FB_ID", &p->fb_id, NULL },
				{ "CRTC_ID", &p->crtc_id, NULL },
				{ "SRC_X", &p->src_x, NULL },
				{ "SRC_Y", &p->src_y, NULL },
				{ "SRC_W", &p->src_w, NULL },
				{ "SRC_H", &p->src_h, NULL },
				{ "CRTC_X", &p->crtc_x, NULL },
				{ "CRTC_Y", &p->crtc_y, NULL },
				{ "CRTC_W", &p->crtc_w, NULL },
				{ "CRTC_H", &p->crtc_h, NULL },
			};
			load_props(p->id, DRM_MODE_OBJECT_PLANE, w, 11);
		}
	}
	drmModeFreePlaneResources(pres);

	/*
	 * One plane per CRTC carries the game: the primary plane (lowest zpos,
	 * so no alpha restriction from sun4i_backend_atomic_check()). Overlay
	 * planes stay free for the UI. On sun4i each backend owns its 4 layers
	 * (sun4i_crtc.c sets possible_crtcs to that backend's CRTC only).
	 */
	for (i = 0; i < D.ncrtc; i++) {
		for (j = 0; j < D.nplane; j++) {
			struct plane *p = &D.planes[j];

			if (!(p->possible_crtcs & (1u << i)) || !p->xrgb8888 || !p->fb_id)
				continue;
			if (p->type == DRM_PLANE_TYPE_PRIMARY) {
				D.crtcs[i].primary = j;
				break;
			}
			if (D.crtcs[i].primary < 0 && p->type == DRM_PLANE_TYPE_OVERLAY)
				D.crtcs[i].primary = j;
		}
	}

	for (i = 0; i < D.nconn; i++)
		dlog(DISPLAY_LOG_DEBUG, "connector %u %s (%s) possible_crtcs=0x%x",
		     D.conns[i].id, D.conns[i].name,
		     D.conns[i].kind == DISPLAY_OUTPUT_HDMI ? "external" :
		     D.conns[i].kind == DISPLAY_OUTPUT_LCD ? "internal" : "ignored",
		     D.conns[i].possible_crtcs);
	for (i = 0; i < D.ncrtc; i++)
		dlog(DISPLAY_LOG_DEBUG, "crtc %u index %d plane %u", D.crtcs[i].id, i,
		     D.crtcs[i].primary >= 0 ? D.planes[D.crtcs[i].primary].id : 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Policy: output, mode, CRTC                                          */
/* ------------------------------------------------------------------ */

static const drmModeModeInfo *find_mode(const struct conn *c, int w, int h, int hz)
{
	const drmModeModeInfo *best = NULL;
	int best_score = -1, i;

	for (i = 0; i < c->count_modes; i++) {
		const drmModeModeInfo *m = &c->modes[i];
		int score, diff;

		if (m->hdisplay != w || m->vdisplay != h)
			continue;
		diff = abs(refresh_mhz(m) - hz * 1000);
		/* Progressive first, then closest refresh, then preferred. */
		score = (m->flags & DRM_MODE_FLAG_INTERLACE) ? 0 : 1000000;
		score += 100000 - (diff > 99999 ? 99999 : diff);
		if (m->type & DRM_MODE_TYPE_PREFERRED)
			score += 1;
		if (score > best_score) {
			best_score = score;
			best = m;
		}
	}
	return best;
}

static bool pick_mode(const struct conn *c, drmModeModeInfo *out, bool *builtin)
{
	const drmModeModeInfo *m = NULL;
	int i;

	*builtin = false;
	if (D.cfg.force_width > 0 && D.cfg.force_height > 0)
		m = find_mode(c, D.cfg.force_width, D.cfg.force_height,
			      D.cfg.hdmi_refresh > 0 ? D.cfg.hdmi_refresh : 60);

	if (!m && c->kind == DISPLAY_OUTPUT_HDMI) {
		/*
		 * No EDID: the kernel only offers its no-EDID DMT list (up to
		 * 1024x768, drm_probe_helper.c:651-653), which many TVs refuse.
		 * Use the CEA timing of the policy mode: 720p60 (accepted by
		 * every HDMI TV) or 480p60 (mandatory for every sink).
		 */
		if ((!c->has_edid || c->count_modes == 0) && D.cfg.hdmi_builtin_mode) {
			*out = (D.cfg.hdmi_width == 640 && D.cfg.hdmi_height == 480)
				       ? mode_cea_480p60 : mode_cea_720p60;
			*builtin = true;
			return true;
		}
		if (D.cfg.hdmi_width > 0 && D.cfg.hdmi_height > 0)
			m = find_mode(c, D.cfg.hdmi_width, D.cfg.hdmi_height,
				      D.cfg.hdmi_refresh > 0 ? D.cfg.hdmi_refresh : 60);
	}
	for (i = 0; !m && i < c->count_modes; i++)
		if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
			m = &c->modes[i];
	if (!m && c->count_modes > 0)
		m = &c->modes[0];
	if (!m)
		return false;
	*out = *m;
	/* The internal panel at the configured refresh (§3.1). */
	if (c->kind == DISPLAY_OUTPUT_LCD && D.cfg.lcd_refresh_hz > 0 &&
	    !(D.cfg.force_width > 0 && D.cfg.force_height > 0))
		mode_retime(m, D.cfg.lcd_refresh_hz, out);
	return true;
}

static int choose_output(void)
{
	int i;

	for (i = 0; i < D.nconn; i++) {
		struct conn *c = &D.conns[i];

		if (c->kind == DISPLAY_OUTPUT_HDMI && c->status == DRM_MODE_CONNECTED &&
		    !c->failed && c->possible_crtcs &&
		    (c->count_modes > 0 || D.cfg.hdmi_builtin_mode))
			return i;
	}
	for (i = 0; i < D.nconn; i++) {
		struct conn *c = &D.conns[i];

		if (c->kind == DISPLAY_OUTPUT_LCD && c->status != DRM_MODE_DISCONNECTED &&
		    c->count_modes > 0 && c->possible_crtcs)
			return i;
	}
	/*
	 * No built-in screen at all (a board with HDMI only) and nothing
	 * connected: keep the external connector in use, else light the first
	 * one with the CEA mode, so the menu runs and a TV plugged in later is
	 * a normal hotplug. Never on a board with a panel connector.
	 */
	for (i = 0; i < D.nconn; i++)
		if (D.conns[i].kind == DISPLAY_OUTPUT_LCD)
			return -1;
	if (!D.cfg.hdmi_builtin_mode)
		return -1;
	if (D.conn >= 0 && D.conn < D.nconn && D.conns[D.conn].kind == DISPLAY_OUTPUT_HDMI &&
	    D.conns[D.conn].possible_crtcs)
		return D.conn;
	for (i = 0; i < D.nconn; i++) {
		struct conn *c = &D.conns[i];

		if (c->kind == DISPLAY_OUTPUT_HDMI && !c->failed && c->possible_crtcs)
			return i;
	}
	return -1;
}

static bool crtc_usable(const struct conn *c, int i)
{
	return i >= 0 && i < D.ncrtc && (c->possible_crtcs & (1u << i)) &&
	       D.crtcs[i].primary >= 0;
}

/*
 * CRTC choice. The HDMI encoder can be fed by either TCON on the A20
 * (sun4i_hdmi_enc.c uses drm_of_find_possible_crtcs(), and both TCONs have
 * an HDMI endpoint in sun7i-a20.dtsi), while the RGB panel is on TCON0
 * only (sun4i_rgb.c). We keep the CRTC already in use when possible: the
 * same backend and, crucially, the same frontend (DEFE) keep feeding the
 * scaled plane. sun4i_backend_find_frontend() hands the first frontend
 * endpoint (fe0) to *both* backends ("TODO: This needs to take multiple
 * pipelines into account"), so the scaled plane on the second backend is
 * the least trustworthy configuration.
 */
static int choose_crtc(int ci)
{
	const struct conn *c = &D.conns[ci];
	int i, k;

	if (c->kind == DISPLAY_OUTPUT_HDMI && crtc_usable(c, D.cfg.hdmi_crtc_index))
		return D.cfg.hdmi_crtc_index;
	if (crtc_usable(c, D.crtc))
		return D.crtc;
	for (k = 0; k < D.nconn; k++) {
		if (D.conns[k].kind != DISPLAY_OUTPUT_LCD)
			continue;
		for (i = 0; i < D.ncrtc; i++)
			if ((D.conns[k].possible_crtcs & (1u << i)) && crtc_usable(c, i))
				return i;
	}
	for (i = 0; i < D.ncrtc; i++)
		if (crtc_usable(c, i))
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Dumb buffers                                                        */
/* ------------------------------------------------------------------ */

static void fb_destroy(struct fbuf *b)
{
	if (b->map && b->map != MAP_FAILED)
		munmap(b->map, b->size);
	if (b->fb_id)
		drmModeRmFB(D.fd, b->fb_id);
	if (b->handle) {
		struct drm_mode_destroy_dumb d = { .handle = b->handle };
		drmIoctl(D.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	memset(b, 0, sizeof(*b));
}

static int fb_create(struct fbuf *b, int w, int h, uint32_t fmt)
{
	struct drm_mode_create_dumb cd = { 0 };
	struct drm_mode_map_dumb md = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	int ret;

	memset(b, 0, sizeof(*b));
	cd.width = (uint32_t)w;
	cd.height = (uint32_t)h;
	cd.bpp = (uint32_t)bpp_of(fmt) * 8;
	if (drmIoctl(D.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return -errno;
	b->handle = cd.handle;
	b->pitch = cd.pitch;
	b->size = cd.size;

	handles[0] = cd.handle;
	pitches[0] = cd.pitch;
	ret = drmModeAddFB2(D.fd, (uint32_t)w, (uint32_t)h, fmt,
			    handles, pitches, offsets, &b->fb_id, 0);
	if (ret) {
		ret = -errno ? -errno : -EINVAL;
		fb_destroy(b);
		return ret;
	}

	md.handle = cd.handle;
	if (drmIoctl(D.fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
		ret = -errno;
		fb_destroy(b);
		return ret;
	}
	/*
	 * sun4i dumb buffers are CMA, mapped write-combined: fast sequential
	 * writes, very slow reads. Never read back from them.
	 */
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd,
		      (off_t)md.offset);
	if (b->map == MAP_FAILED) {
		ret = -errno;
		b->map = NULL;
		fb_destroy(b);
		return ret;
	}
	memset(b->map, 0, b->size);
	return 0;
}

static void set_free(struct bufset *s)
{
	int i;

	for (i = 0; i < s->n; i++)
		fb_destroy(&s->b[i]);
	memset(s, 0, sizeof(*s));
}

static int set_alloc(struct bufset *s, int w, int h, int n, uint32_t fmt)
{
	int i, ret;

	memset(s, 0, sizeof(*s));
	for (i = 0; i < n; i++) {
		ret = fb_create(&s->b[i], w, h, fmt);
		if (ret) {
			set_free(s);
			return ret;
		}
		s->n = i + 1;
	}
	s->w = w;
	s->h = h;
	s->format = fmt;
	return 0;
}

static int set_find(const struct bufset *s, enum bstate st)
{
	int i;

	if (!s)
		return -1;
	for (i = 0; i < s->n; i++)
		if (s->b[i].st == st)
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Pixel conversion                                                    */
/* ------------------------------------------------------------------ */

/*
 * 16-bit to XRGB8888, expanding each channel to 8 bits by replicating its
 * top bits (x << 3 | x >> 2 for 5 bits), so white stays 0xffffff.
 *
 * NEON: 8 pixels per iteration. The channels are narrowed to bytes already
 * shifted to the top (vshrn/vmovn), the low bits are filled with a
 * shift-right-insert (vsri), and vst4 interleaves B,G,R,X, which is
 * XRGB8888 in memory on little-endian. The destination is write-combined
 * memory: vst4 writes 32 contiguous bytes, which suits it well.
 */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

static int rgb565_to_xrgb_neon(uint32_t *dst, const uint16_t *src, int w)
{
	const uint8x8_t ff = vdup_n_u8(0xff);
	int x = 0;

	for (; x + 8 <= w; x += 8) {
		uint16x8_t p = vld1q_u16(src + x);
		uint8x8x4_t o;
		uint8x8_t r = vshrn_n_u16(p, 8);                   /* RRRRRGGG */
		uint8x8_t g = vshrn_n_u16(p, 3);                   /* GGGGGGBB */
		uint8x8_t b = vmovn_u16(vshlq_n_u16(p, 3));        /* BBBBB000 */

		/* vsri keeps the top bits of the destination and fills the low
		 * bits with the top bits of the channel: x << 3 | x >> 2. */
		o.val[0] = vsri_n_u8(b, b, 5);
		o.val[1] = vsri_n_u8(g, g, 6);
		o.val[2] = vsri_n_u8(r, r, 5);
		o.val[3] = ff;
		vst4_u8((uint8_t *)(void *)(dst + x), o);
	}
	return x;
}

static int xrgb1555_to_xrgb_neon(uint32_t *dst, const uint16_t *src, int w)
{
	const uint8x8_t ff = vdup_n_u8(0xff);
	int x = 0;

	for (; x + 8 <= w; x += 8) {
		uint16x8_t p = vld1q_u16(src + x);
		uint8x8x4_t o;
		uint8x8_t r = vshrn_n_u16(p, 7);                   /* RRRRRGGG */
		uint8x8_t g = vshrn_n_u16(p, 2);                   /* GGGGGBBB */
		uint8x8_t b = vmovn_u16(vshlq_n_u16(p, 3));        /* BBBBB000 */

		o.val[0] = vsri_n_u8(b, b, 5);
		o.val[1] = vsri_n_u8(g, g, 5);
		o.val[2] = vsri_n_u8(r, r, 5);
		o.val[3] = ff;
		vst4_u8((uint8_t *)(void *)(dst + x), o);
	}
	return x;
}
#endif

/* Converts one line to XRGB8888. */
static void convert_line(uint32_t *dst, const uint8_t *src, uint32_t fmt, int w)
{
	const uint16_t *s = (const uint16_t *)(const void *)src;
	int x = 0;

	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		memcpy(dst, src, (size_t)w * 4);
		break;
	case DRM_FORMAT_RGB565:
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		x = rgb565_to_xrgb_neon(dst, s, w);
#endif
		for (; x < w; x++) {
			uint32_t p = s[x];
			uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;

			dst[x] = 0xff000000u | ((r << 3 | r >> 2) << 16) |
				 ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
		}
		break;
	case DRM_FORMAT_XRGB1555:
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		x = xrgb1555_to_xrgb_neon(dst, s, w);
#endif
		for (; x < w; x++) {
			uint32_t p = s[x];
			uint32_t r = (p >> 10) & 0x1f, g = (p >> 5) & 0x1f, b = p & 0x1f;

			dst[x] = 0xff000000u | ((r << 3 | r >> 2) << 16) |
				 ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
		}
		break;
	default:
		break;
	}
}

/* Converts one XRGB8888 line to RGB565 (only for carrying a frame over). */
static void convert_line_565(uint16_t *dst, const uint8_t *src, int w)
{
	const uint32_t *s = (const uint32_t *)(const void *)src;
	int x;

	for (x = 0; x < w; x++)
		dst[x] = (uint16_t)(((s[x] >> 8) & 0xf800) | ((s[x] >> 5) & 0x07e0) |
				    ((s[x] >> 3) & 0x001f));
}

#define LINEBUF_MAX 4096
static uint32_t linebuf[LINEBUF_MAX];

/*
 * Copies the visible part of a source image into a scanout buffer of
 * format dfmt (XRGB8888 or RGB565), at img (in the buffer), scaled by the
 * integer n (nearest neighbour, n > 1 only for XRGB8888 destinations).
 * Same layout: plain memcpy per line. Destination writes are sequential
 * whole lines: the buffers are write-combined and must never be read.
 */
static void blit(struct fbuf *b, uint32_t dfmt, const struct display_rect *img, int n,
		 int sx0, int sy0, const uint8_t *src, int sstride, uint32_t sfmt)
{
	int sbpp = bpp_of(sfmt), dbpp = bpp_of(dfmt), y, x, k;
	bool same = native_fb_fmt(sfmt) == dfmt;

	if (b->dirty) {
		memset(b->map, 0, b->size);
		b->dirty = false;
	}
	if (n <= 1 || dfmt != DRM_FORMAT_XRGB8888 || img->w > LINEBUF_MAX) {
		for (y = 0; y < img->h; y++) {
			uint8_t *d = b->map + (size_t)(img->y + y) * b->pitch + (size_t)img->x * dbpp;
			const uint8_t *s = src + (size_t)(sy0 + y) * sstride + (size_t)sx0 * sbpp;

			if (same)
				memcpy(d, s, (size_t)img->w * dbpp);
			else if (dfmt == DRM_FORMAT_XRGB8888)
				convert_line((uint32_t *)(void *)d, s, sfmt, img->w);
			else if (dfmt == DRM_FORMAT_RGB565 && fmt_is_32(sfmt))
				convert_line_565((uint16_t *)(void *)d, s, img->w);
		}
		return;
	}
	/* CPU integer scale: convert + widen one source line, write it n times. */
	for (y = 0; y < img->h / n; y++) {
		const uint8_t *s = src + (size_t)(sy0 + y) * sstride + (size_t)sx0 * sbpp;
		int w = img->w / n;

		convert_line(linebuf, s, sfmt, w);
		for (x = w - 1; x >= 0; x--) {  /* in place, from the end */
			uint32_t v = linebuf[x];

			for (k = n - 1; k >= 0; k--)
				linebuf[x * n + k] = v;
		}
		for (k = 0; k < n; k++)
			memcpy(b->map + (size_t)(img->y + y * n + k) * b->pitch + (size_t)img->x * 4,
			       linebuf, (size_t)img->w * 4);
	}
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static struct display_rect compute_scaled(int sw, int sh, int W, int H,
					  enum display_scale_mode mode, double aspect)
{
	struct display_rect r = { 0, 0, W, H };

	if (aspect <= 0.0)
		aspect = (double)sw / (double)sh;

	if (mode == DISPLAY_SCALE_INTEGER) {
		int n = W / sw < H / sh ? W / sw : H / sh;

		if (n >= 1) {
			r.w = sw * n;
			r.h = sh * n;
		} else {
			mode = DISPLAY_SCALE_ASPECT; /* image larger than screen */
		}
	}
	if (mode == DISPLAY_SCALE_ASPECT) {
		if ((double)W / (double)H > aspect) {
			r.h = H;
			r.w = (int)(H * aspect + 0.5);
		} else {
			r.w = W;
			r.h = (int)(W / aspect + 0.5);
		}
	}
	if (r.w > W)
		r.w = W;
	if (r.h > H)
		r.h = H;
	if (r.w < 1)
		r.w = 1;
	if (r.h < 1)
		r.h = 1;
	/* sun4i has no plane clipping: the rect must stay inside the CRTC. */
	r.x = (W - r.w) / 2;
	r.y = (H - r.h) / 2;
	return r;
}

/* ------------------------------------------------------------------ */
/* Atomic commits                                                      */
/* ------------------------------------------------------------------ */

static int add(drmModeAtomicReq *r, uint32_t obj, uint32_t prop, uint64_t v)
{
	if (!prop)
		return -ENOENT;
	return drmModeAtomicAddProperty(r, obj, prop, v) < 0 ? -EINVAL : 0;
}

/* Full plane state; a source rect at (sx, sy) inside the FB. */
static int add_plane_src(drmModeAtomicReq *r, const struct plane *p, uint32_t crtc_id,
			 uint32_t fb_id, int sx, int sy, int src_w, int src_h,
			 const struct display_rect *d)
{
	int ret = 0;

	if (!fb_id) {
		ret |= add(r, p->id, p->fb_id, 0);
		ret |= add(r, p->id, p->crtc_id, 0);
		return ret ? -EINVAL : 0;
	}
	ret |= add(r, p->id, p->fb_id, fb_id);
	ret |= add(r, p->id, p->crtc_id, crtc_id);
	ret |= add(r, p->id, p->src_x, (uint64_t)sx << 16);
	ret |= add(r, p->id, p->src_y, (uint64_t)sy << 16);
	ret |= add(r, p->id, p->src_w, (uint64_t)src_w << 16);
	ret |= add(r, p->id, p->src_h, (uint64_t)src_h << 16);
	ret |= add(r, p->id, p->crtc_x, (uint64_t)(int64_t)d->x);
	ret |= add(r, p->id, p->crtc_y, (uint64_t)(int64_t)d->y);
	ret |= add(r, p->id, p->crtc_w, (uint64_t)d->w);
	ret |= add(r, p->id, p->crtc_h, (uint64_t)d->h);
	return ret ? -EINVAL : 0;
}

static int add_plane(drmModeAtomicReq *r, const struct plane *p, uint32_t crtc_id,
		     uint32_t fb_id, int src_w, int src_h, const struct display_rect *d)
{
	return add_plane_src(r, p, crtc_id, fb_id, 0, 0, src_w, src_h, d);
}

/* ------------------------------------------------------------------ */
/* Overlay plane                                                       */
/* ------------------------------------------------------------------ */

bool display_overlay_rect(int W, int H, int w, int h, enum display_corner corner, int margin,
			  struct display_rect *out)
{
	bool right = corner == DISPLAY_CORNER_TOP_RIGHT || corner == DISPLAY_CORNER_BOTTOM_RIGHT;
	bool bottom = corner == DISPLAY_CORNER_BOTTOM_RIGHT || corner == DISPLAY_CORNER_BOTTOM_LEFT;
	struct display_rect r;

	if (w < 1 || h < 1 || W < w || H < h)
		return false;
	if (margin < 0)
		margin = 0;
	r.w = w;
	r.h = h;
	r.x = right ? W - w - margin : margin;
	r.y = bottom ? H - h - margin : margin;
	/* sun4i does not clip planes: the whole rect stays on screen */
	if (r.x < 0)
		r.x = 0;
	if (r.x > W - w)
		r.x = W - w;
	if (r.y < 0)
		r.y = 0;
	if (r.y > H - h)
		r.y = H - h;
	*out = r;
	return true;
}

static bool ov_key_eq(const struct ov_key *a, const struct ov_key *b)
{
	return a->pi == b->pi && a->crtc_id == b->crtc_id && a->dst.x == b->dst.x &&
	       a->dst.y == b->dst.y && a->dst.w == b->dst.w && a->dst.h == b->dst.h;
}

/* An overlay plane of CRTC ki that takes ARGB8888 (not the game plane). */
static int ov_plane(int ki)
{
	int i;

	if (ki < 0 || ki >= D.ncrtc)
		return -1;
	for (i = 0; i < D.nplane; i++)
		if (i != D.crtcs[ki].primary && (D.planes[i].possible_crtcs & (1u << ki)) &&
		    D.planes[i].type == DRM_PLANE_TYPE_OVERLAY && D.planes[i].argb8888 &&
		    D.planes[i].fb_id)
			return i;
	return -1;
}

/*
 * Decides what the next commit on CRTC ki (mode W x H) does with the
 * overlay. full: the commit writes every plane (modeset), so a visible
 * overlay must be included even if nothing changed. Returns false when the
 * overlay needs nothing in this commit (then, in a modeset, the plane is
 * switched off with the other unused planes).
 */
static bool ov_prepare(int ki, int W, int H, bool full, struct ov_plan *op)
{
	struct overlay *o = &D.ov;
	bool on_screen = o->front >= 0;
	int pi = ov_plane(ki);

	memset(op, 0, sizeof(*op));
	if (o->pend_valid)
		return false;   /* resolved by the flip event first */
	op->key.pi = pi;
	op->key.crtc_id = ki >= 0 ? D.crtcs[ki].id : 0;
	if (!o->want || !o->pix || pi < 0 ||
	    !display_overlay_rect(W, H, o->w, o->h, o->corner, o->margin, &op->key.dst) ||
	    (o->failed && ov_key_eq(&o->failed_key, &op->key))) {
		if (!on_screen || full)
			return false;
		op->show = false;   /* take it down */
		op->key = o->on;
		return true;
	}
	op->show = true;
	if (on_screen && o->fb_serial[o->front] == o->serial && o->fbw[o->front] == o->w &&
	    o->fbh[o->front] == o->h) {
		op->b = o->front;
		if (!full && ov_key_eq(&o->on, &op->key))
			return false;   /* nothing changed */
		return true;
	}
	/* New content: into the buffer that is not on screen. */
	op->b = o->front == 0 ? 1 : 0;
	if (o->fbw[op->b] != o->w || o->fbh[op->b] != o->h || !o->fb[op->b].map) {
		fb_destroy(&o->fb[op->b]);
		o->fbw[op->b] = o->fbh[op->b] = 0;
		if (fb_create(&o->fb[op->b], o->w, o->h, DRM_FORMAT_ARGB8888) < 0) {
			dlog(DISPLAY_LOG_WARN, "overlay: no %dx%d buffer", o->w, o->h);
			return false;
		}
		o->fbw[op->b] = o->w;
		o->fbh[op->b] = o->h;
		o->fb_serial[op->b] = 0;
	}
	if (o->fb_serial[op->b] != o->serial) {
		struct fbuf *b = &o->fb[op->b];

		for (int y = 0; y < o->h; y++)   /* whole lines: write-combined memory */
			memcpy(b->map + (size_t)y * b->pitch, o->pix + (size_t)y * o->w, (size_t)o->w * 4);
		o->fb_serial[op->b] = o->serial;
	}
	return true;
}

static int ov_add(drmModeAtomicReq *r, const struct ov_plan *op)
{
	const struct plane *p = &D.planes[op->key.pi];

	if (!op->show)
		return add_plane(r, p, 0, 0, 0, 0, NULL);
	return add_plane(r, p, op->key.crtc_id, D.ov.fb[op->b].fb_id, D.ov.w, D.ov.h, &op->key.dst);
}

/* The overlay state after a commit that has reached the screen. */
static void ov_shown(const struct ov_plan *op)
{
	struct overlay *o = &D.ov;

	if (op->show) {
		if (o->front < 0 || !ov_key_eq(&o->on, &op->key))
			dlog(DISPLAY_LOG_INFO, "overlay: plane %u, %dx%d at %d,%d", D.planes[op->key.pi].id,
			     op->key.dst.w, op->key.dst.h, op->key.dst.x, op->key.dst.y);
		o->front = op->b;
		o->on = op->key;
		o->tested = true;
		o->tested_key = op->key;
	} else {
		o->front = -1;
	}
	if ((op->show && o->want && o->fb_serial[op->b] == o->serial) || (!op->show && !o->want))
		o->dirty_since = 0;
}

/* A modeset commit without the overlay switched its plane off. */
static void ov_off(void)
{
	D.ov.front = -1;
	D.ov.pend_valid = false;
	if (D.ov.want)
		D.ov.dirty_since = display_now_ms();
}

/* The overlay could not be shown at this key: log once, show nothing. */
static void ov_refused(const struct ov_plan *op, int err)
{
	struct overlay *o = &D.ov;

	if (!o->failed || !ov_key_eq(&o->failed_key, &op->key))
		dlog(DISPLAY_LOG_WARN, "overlay refused on plane %u (%dx%d at %d,%d): %s; not shown",
		     D.planes[op->key.pi].id, op->key.dst.w, op->key.dst.h, op->key.dst.x,
		     op->key.dst.y, strerror(err < 0 ? -err : err));
	o->failed = true;
	o->failed_key = op->key;
	o->dirty_since = 0;
}

/* The flip in flight completed (or was given up): what it carried is shown. */
static void ov_flip_done(void)
{
	struct ov_plan op;

	if (!D.ov.pend_valid)
		return;
	D.ov.pend_valid = false;
	memset(&op, 0, sizeof(op));
	op.show = D.ov.pend >= 0;
	op.b = op.show ? D.ov.pend : 0;
	op.key = D.ov.pend_key;
	ov_shown(&op);
}

static void ov_pending(const struct ov_plan *op)
{
	D.ov.pend_valid = true;
	D.ov.pend = op->show ? op->b : -1;
	D.ov.pend_key = op->key;
}

/* TEST_ONLY of the overlay alone against the current state (once per key). */
static int ov_test(const struct ov_plan *op)
{
	drmModeAtomicReq *r;
	int ret;

	if (!op->show || (D.ov.tested && ov_key_eq(&D.ov.tested_key, &op->key)))
		return 0;
	r = drmModeAtomicAlloc();
	if (!r)
		return -ENOMEM;
	ret = ov_add(r, op);
	if (ret == 0)
		ret = drmModeAtomicCommit(D.fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
	drmModeAtomicFree(r);
	if (ret == 0) {
		D.ov.tested = true;
		D.ov.tested_key = op->key;
	}
	return ret;
}

/*
 * No page flip is coming to carry an overlay change (the game shows the
 * same frame, a menu is idle): commit it alone, non-blocking. The event
 * marks the flip done like any other, so frames presented meanwhile are
 * queued and go out right after (at most one frame late, only when the
 * overlay changes).
 */
static void ov_kick(void)
{
	struct ov_plan op;
	drmModeAtomicReq *r;
	int ret;

	if (!D.inited || D.suspended || !D.active || D.flip_pending || D.crtc < 0 ||
	    !D.ov.dirty_since)
		return;
	if (!ov_prepare(D.crtc, D.mode.hdisplay, D.mode.vdisplay, false, &op)) {
		if (!D.ov.pend_valid)
			D.ov.dirty_since = 0;
		return;
	}
	ret = ov_test(&op);
	if (ret) {
		ov_refused(&op, ret);
		return;
	}
	r = drmModeAtomicAlloc();
	if (!r)
		return;
	ret = ov_add(r, &op);
	if (ret == 0)
		ret = drmModeAtomicCommit(D.fd, r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
					  (void *)(uintptr_t)D.gen);
	drmModeAtomicFree(r);
	if (ret) {
		if (ret != -EBUSY)
			ov_refused(&op, ret);
		return;
	}
	ov_pending(&op);
	D.flip_pending = true;
	D.flip_ov = true;
	D.flip_at = display_now_ms();
}

/* From the event handler: an overlay change nobody carried for 100 ms. */
static void ov_kick_stale(void)
{
	if (D.ov.dirty_since && display_now_ms() - D.ov.dirty_since >= 100)
		ov_kick();
}

int display_set_overlay(const uint32_t *argb, int w, int h, enum display_corner corner, int margin)
{
	struct overlay *o = &D.ov;

	if (!argb || w < 1 || h < 1 || w > 512 || h > 512 || corner < DISPLAY_CORNER_TOP_RIGHT ||
	    corner > DISPLAY_CORNER_BOTTOM_LEFT)
		return -EINVAL;
	if (w != o->w || h != o->h || !o->pix) {
		uint32_t *p = malloc((size_t)w * h * 4);

		if (!p)
			return -ENOMEM;
		free(o->pix);
		o->pix = p;
		o->w = w;
		o->h = h;
	} else if (o->want && o->corner == corner && o->margin == margin &&
		   !memcmp(o->pix, argb, (size_t)w * h * 4)) {
		return 0;   /* same picture, same place */
	}
	memcpy(o->pix, argb, (size_t)w * h * 4);
	o->corner = corner;
	o->margin = margin;
	o->want = true;
	o->serial++;
	o->failed = false;   /* new content or place: may be tried again */
	o->dirty_since = display_now_ms();
	/* Idle display: nothing will carry it, commit it now. */
	if (D.inited && !D.flip_pending &&
	    display_now_ms() - D.stats.last_flip_us / 1000 >= 100)
		ov_kick();
	return 0;
}

void display_hide_overlay(void)
{
	if (!D.ov.want)
		return;
	D.ov.want = false;
	D.ov.dirty_since = display_now_ms();
	if (D.inited && !D.flip_pending)
		ov_kick();
}

bool display_overlay_visible(void)
{
	return D.ov.want && D.ov.front >= 0;
}

/*
 * Commits the whole desired state. With modeset, every connector, CRTC and
 * plane is written, so leftovers (fbcon, a previous output) are switched
 * off in the same commit: one atomic step, no intermediate state. op: the
 * overlay plane, or NULL.
 */
static int commit_state(int ci, int ki, int pi, uint32_t blob, bool modeset,
			const struct plan *pl, bool test, const struct ov_plan *op)
{
	drmModeAtomicReq *r = drmModeAtomicAlloc();
	uint32_t flags = test ? DRM_MODE_ATOMIC_TEST_ONLY : 0;
	uint32_t fb = 0;
	int i, ret = 0;

	if (!r)
		return -ENOMEM;
	if (modeset) {
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
		for (i = 0; i < D.nconn; i++)
			ret |= add(r, D.conns[i].id, D.conns[i].prop_crtc_id,
				   i == ci ? D.crtcs[ki].id : 0);
		for (i = 0; i < D.ncrtc; i++) {
			/* display_set_active(false) survives switches and resumes. */
			ret |= add(r, D.crtcs[i].id, D.crtcs[i].prop_active,
				   i == ki && D.active);
			ret |= add(r, D.crtcs[i].id, D.crtcs[i].prop_mode_id,
				   i == ki ? blob : 0);
		}
		for (i = 0; i < D.nplane; i++)
			if (i != pi && !(op && op->show && i == op->key.pi))
				ret |= add_plane(r, &D.planes[i], 0, 0, 0, 0, NULL);
	}
	if (pl->path != PATH_NONE)
		fb = pl->set->b[pl->idx].fb_id;
	ret |= add_plane(r, &D.planes[pi], D.crtcs[ki].id, fb, pl->src_w, pl->src_h,
			 &pl->dst);
	if (op && (op->show || !modeset))
		ret |= ov_add(r, op);
	if (ret) {
		drmModeAtomicFree(r);
		return -EINVAL;
	}
	ret = drmModeAtomicCommit(D.fd, r, flags, NULL);
	drmModeAtomicFree(r);
	return ret;
}

static void on_flip(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
		    unsigned int crtc_id, void *data);

/* Hands an external FB back to its owner. */
static void ext_release(struct ext_fb *e)
{
	struct display_fb fb;

	if (!e->valid)
		return;
	fb = e->fb;
	e->valid = false;
	if (fb.release)
		fb.release(fb.fb_id, fb.user);
}

/* The pending external FB is now on screen; release the one it replaced. */
static void ext_flip_done(void)
{
	if (D.ext_front.valid &&
	    (!D.ext_pending.valid || D.ext_front.fb.fb_id != D.ext_pending.fb.fb_id))
		ext_release(&D.ext_front);
	if (D.ext_pending.valid)
		D.ext_front = D.ext_pending;
	D.ext_pending.valid = false;
}

/* An overlay change to carry in the next non-blocking flip (tested once per key). */
static bool ov_for_flip(struct ov_plan *op)
{
	int ret;

	if (!D.ov.dirty_since || !ov_prepare(D.crtc, D.mode.hdisplay, D.mode.vdisplay, false, op))
		return false;
	ret = ov_test(op);
	if (ret) {
		ov_refused(op, ret);
		return false;
	}
	return true;
}

static int flip_once(int idx, const struct ov_plan *op);

static int flip(int idx)
{
	struct ov_plan op;
	bool with_ov = ov_for_flip(&op);
	int ret = flip_once(idx, with_ov ? &op : NULL);

	if (ret && with_ov) {
		/* Never lose a game frame to the overlay: without it. */
		ret = flip_once(idx, NULL);
		if (ret == 0)
			ov_refused(&op, -EINVAL);
	} else if (ret == 0 && with_ov) {
		ov_pending(&op);
	}
	return ret;
}

static int flip_once(int idx, const struct ov_plan *op)
{
	struct fbuf *b = &D.act->b[idx];
	const struct plane *p = &D.planes[D.plane];
	drmModeAtomicReq *r = drmModeAtomicAlloc();
	bool geo;
	int ret;

	if (!r)
		return -ENOMEM;
	/*
	 * Normally only FB_ID changes; everything else is carried over from
	 * the current state. When the frame size changed (PSX), the new
	 * SRC/CRTC rectangles ride along with the first frame of that size,
	 * in the same commit: no modeset. The backend latches its registers
	 * at vblank (sun4i_backend_commit() sets REGBUFFCTL.LOADCTL), so a
	 * flip never tears.
	 */
	if (D.path == PATH_SCALED) {
		geo = b->fw > 0 &&
		      (b->fw != D.geo_fw || b->fh != D.geo_fh ||
		       memcmp(&b->dst, &D.geo_dst, sizeof(b->dst)) != 0);
		if (geo)
			ret = add_plane(r, p, D.crtcs[D.crtc].id, b->fb_id, b->fw, b->fh,
					&b->dst);
		else
			ret = add(r, p->id, p->fb_id, b->fb_id);
	} else {
		/* Full-screen plane; rects only needed after an external FB. */
		struct display_rect full = { 0, 0, D.act->w, D.act->h };

		geo = D.geo_fw < 0;
		if (geo)
			ret = add_plane(r, p, D.crtcs[D.crtc].id, b->fb_id, full.w, full.h,
					&full);
		else
			ret = add(r, p->id, p->fb_id, b->fb_id);
		b->fw = full.w;
		b->fh = full.h;
		b->dst = full;
	}
	if (op && ret == 0)
		ret = ov_add(r, op);
	if (ret) {
		drmModeAtomicFree(r);
		return -EINVAL;
	}
	ret = drmModeAtomicCommit(D.fd, r,
				  DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
				  (void *)(uintptr_t)D.gen);
	drmModeAtomicFree(r);
	if (ret)
		return ret;
	if (geo) {
		D.geo_fw = b->fw;
		D.geo_fh = b->fh;
		D.geo_dst = b->dst;
	}
	b->st = B_PENDING;
	D.flip_pending = true;
	D.flip_at = display_now_ms();
	D.flip_ext = false;
	D.ext_shown = false;   /* a surface frame replaces the external FB */
	return 0;
}

static int apply(int ci, int ki, const drmModeModeInfo *mode, bool modeset);

static void submit(int idx)
{
	struct fbuf *b = &D.act->b[idx];
	int q = set_find(D.act, B_QUEUED);

	/* Geometry of this frame (scaled path), applied by flip(). */
	b->fw = D.sw;
	b->fh = D.sh;
	b->dst = D.dst;

	if (!D.active || D.suspended) {
		/* Screen off: keep the newest frame for display_set_active(true). */
		if (q >= 0 && q != idx) {
			D.act->b[q].st = B_FREE;
			D.stats.dropped++;
		}
		b->st = B_QUEUED;
		return;
	}
	if (!D.flip_pending) {
		bool geo = D.path == PATH_SCALED &&
			   (b->fw != D.geo_fw || b->fh != D.geo_fh ||
			    memcmp(&b->dst, &D.geo_dst, sizeof(b->dst)) != 0);
		int ret;

		if (q >= 0 && q != idx) {
			D.act->b[q].st = B_FREE;
			D.stats.dropped++;
		}
		ret = flip(idx);
		if (ret) {
			dlog(DISPLAY_LOG_WARN, "page flip failed: %s", strerror(-ret));
			b->st = B_FREE;
			D.stats.dropped++;
			/* A geometry the kernel refuses: re-pick the strategy. */
			if (geo && ret != -EBUSY)
				apply(D.conn, D.crtc, &D.mode, false);
		}
		return;
	}
	/* A flip is in flight: keep only the newest frame for the next vblank. */
	if (q >= 0 && q != idx) {
		D.act->b[q].st = B_FREE;
		D.stats.dropped++;
	}
	b->st = B_QUEUED;
}

/*
 * The real refresh of a new mode: the vblank sequence and timestamp that
 * every page-flip event carries, over >= 2 s of flips. A gap of more than
 * 3 s between events restarts it (sun4i has no hardware vblank counter:
 * while the vblank interrupt is off, the count does not advance). Logged
 * once per modeset; display_output()->measured_mhz.
 */
static void meas_start(void)
{
	D.meas.on = true;
	D.meas.have0 = false;
	D.info.measured_mhz = 0;
}

static void meas_flip(unsigned int seq, int64_t t)
{
	if (!D.meas.on)
		return;
	if (!D.meas.have0 || t - D.meas.tl > 3000000 || seq < D.meas.seql || t < D.meas.tl) {
		D.meas.have0 = true;
		D.meas.seq0 = seq;
		D.meas.t0 = t;
	} else if (t - D.meas.t0 >= 2000000 && seq - D.meas.seq0 >= 60) {
		uint32_t n = seq - D.meas.seq0;
		int64_t dt = t - D.meas.t0;
		int nominal = refresh_mhz(&D.mode);

		D.info.measured_mhz = (int)(((int64_t)n * 1000000000LL + dt / 2) / dt);
		D.meas.on = false;
		dlog(DISPLAY_LOG_INFO, "refresh measured on %s: %d.%03d Hz (%u vblanks in %lld.%03lld s; mode %d.%03d Hz)",
		     D.info.name, D.info.measured_mhz / 1000, D.info.measured_mhz % 1000, n,
		     (long long)(dt / 1000000), (long long)(dt / 1000 % 1000), nominal / 1000, nominal % 1000);
		if (abs(D.info.measured_mhz - nominal) * 1000 > nominal * 5)
			dlog(DISPLAY_LOG_WARN, "%s runs %.2f %% away from its mode's refresh: pacing uses the mode's",
			     D.info.name, 100.0 * (D.info.measured_mhz - nominal) / nominal);
	}
	D.meas.seql = seq;
	D.meas.tl = t;
}

static void on_flip(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
		    unsigned int crtc_id, void *data)
{
	int f, p, q;

	(void)fd;
	(void)crtc_id;
	if ((uintptr_t)data != (uintptr_t)D.gen)
		return; /* belongs to a pipeline we tore down */
	meas_flip(seq, (int64_t)sec * 1000000 + usec);
	D.flip_pending = false;
	ov_flip_done();
	if (D.flip_ov) {
		/* Only the overlay changed. Frames presented meanwhile go now. */
		D.flip_ov = false;
		q = D.act ? set_find(D.act, B_QUEUED) : -1;
		if (q >= 0 && flip(q)) {
			D.act->b[q].st = B_FREE;
			D.stats.dropped++;
		}
		return;
	}
	D.stats.flips++;
	D.stats.last_flip_us = (int64_t)sec * 1000000 + usec;

	if (D.flip_ext) {
		/* An external FB reached the screen: the previous one left it. */
		D.flip_ext = false;
		ext_flip_done();
		return;
	}
	/* A surface frame reached the screen: an external FB left it. */
	if (D.ext_front.valid && !D.ext_shown)
		ext_release(&D.ext_front);
	if (!D.act)
		return;

	p = set_find(D.act, B_PENDING);
	if (p >= 0) {
		f = set_find(D.act, B_FRONT);
		if (f >= 0)
			D.act->b[f].st = B_FREE;
		D.act->b[p].st = B_FRONT;
	}
	q = set_find(D.act, B_QUEUED);
	if (q >= 0) {
		int ret = flip(q);

		if (ret) {
			D.act->b[q].st = B_FREE;
			D.stats.dropped++;
		}
	}
}

static void handle_drm(void)
{
	drmEventContext ev = { 0 };

	ev.version = 3;
	ev.page_flip_handler2 = on_flip;
	drmHandleEvent(D.fd, &ev);
}

/* Waits for the outstanding flip (only DRM events, no hotplug handling). */
static void drain_flip(int timeout_ms)
{
	int64_t end = display_now_ms() + timeout_ms;

	while (D.flip_pending) {
		struct pollfd pfd = { D.fd, POLLIN, 0 };
		int64_t left = end - display_now_ms();
		int r;

		if (left <= 0)
			break;
		r = poll(&pfd, 1, (int)left);
		if (r > 0)
			handle_drm();
		else if (r < 0 && errno != EINTR)
			break;
	}
	if (D.flip_pending) {
		dlog(DISPLAY_LOG_WARN, "flip did not complete in %d ms", timeout_ms);
		D.flip_pending = false;
		D.flip_ov = false;
		ov_flip_done();   /* assume it was shown, like the frame */
		if (D.flip_ext) {
			D.flip_ext = false;
			ext_flip_done();   /* assume it was shown; keep ownership consistent */
		}
	}
}

/* ------------------------------------------------------------------ */
/* Pipeline (re)configuration                                          */
/* ------------------------------------------------------------------ */

static int use_set(struct bufset *cur, struct bufset *tmp, int w, int h, int n,
		   uint32_t fmt, struct bufset **out)
{
	int ret;

	if (cur->n == n && cur->w == w && cur->h == h && cur->format == fmt) {
		*out = cur;
		return 0;
	}
	ret = set_alloc(tmp, w, h, n, fmt);
	if (ret)
		return ret;
	*out = tmp;
	return 0;
}

static int newest_idx(const struct bufset *s)
{
	int i = set_find(s, B_QUEUED);

	if (i < 0)
		i = set_find(s, B_PENDING);
	if (i < 0)
		i = set_find(s, B_FRONT);
	return i;
}

static struct frame_src newest_frame(void)
{
	struct frame_src f = { 0 };

	if (D.fresh)
		return f;
	if (D.surf_set && D.path == PATH_SCALED && D.direct && D.act) {
		int i = newest_idx(D.act);

		if (i >= 0) {
			f.p = D.act->b[i].map;
			f.stride = (int)D.act->b[i].pitch;
			f.fmt = D.act->format;
			f.valid = true;
			return f;
		}
	}
	if (D.surf_set && D.shadow_valid) {
		f.p = D.shadow;
		f.stride = D.shadow_stride;
		f.fmt = D.sfmt;
		f.valid = true;
	}
	return f;
}

/*
 * Fallback geometry: largest integer factor that fits (capped by
 * soft_scale_max), image centered, source cropped if it is larger than
 * the screen.
 */
static void soft_geometry(int W, int H, struct display_rect *img, int *n, int *sx0,
			  int *sy0)
{
	int max = D.cfg.soft_scale_max > 0 ? D.cfg.soft_scale_max : 1;
	int f = W / D.sw < H / D.sh ? W / D.sw : H / D.sh;
	int vw, vh;

	if (f > max)
		f = max;
	if (f < 1 || W > LINEBUF_MAX)
		f = 1;
	vw = D.sw * f <= W ? D.sw : W / f;
	vh = D.sh * f <= H ? D.sh : H / f;
	img->w = vw * f;
	img->h = vh * f;
	img->x = (W - img->w) / 2;
	img->y = (H - img->h) / 2;
	*n = f;
	*sx0 = (D.sw - vw) / 2;
	*sy0 = (D.sh - vh) / 2;
}

static int make_plan(enum path path, uint32_t fmt, int W, int H, struct bufset *tmp,
		     struct plan *pl)
{
	int nbuf = D.cfg.buffers, ret = 0, i;

	memset(pl, 0, sizeof(*pl));
	pl->path = path;
	pl->fmt = fmt;
	pl->idx = -1;

	switch (path) {
	case PATH_NONE:
		return 0;
	case PATH_BLANK:
		ret = use_set(&D.blank, tmp, W, H, 1, fmt, &pl->set);
		pl->dst = (struct display_rect){ 0, 0, W, H };
		pl->src_w = W;
		pl->src_h = H;
		break;
	case PATH_SCALED:
		/* Allocated at capacity; the plane shows the frame-size corner. */
		ret = use_set(&D.game, tmp, D.cw, D.ch, nbuf, fmt, &pl->set);
		pl->dst = compute_scaled(D.sw, D.sh, W, H, D.scale, D.aspect);
		pl->src_w = D.sw;
		pl->src_h = D.sh;
		pl->img = (struct display_rect){ 0, 0, D.sw, D.sh };
		pl->n = 1;
		pl->direct = native_fb_fmt(D.sfmt) == fmt &&
			     !(D.sflags & DISPLAY_SURFACE_CACHED);
		break;
	case PATH_SOFT:
		ret = use_set(&D.screen, tmp, W, H, nbuf, fmt, &pl->set);
		pl->dst = (struct display_rect){ 0, 0, W, H };
		pl->src_w = W;
		pl->src_h = H;
		soft_geometry(W, H, &pl->img, &pl->n, &pl->sx0, &pl->sy0);
		break;
	}
	if (ret)
		return ret;

	if (path == PATH_SOFT && memcmp(&pl->set->img, &pl->img, sizeof(pl->img))) {
		/* Borders are cleared once per buffer, not per frame. */
		for (i = 0; pl->set != tmp && i < pl->set->n; i++)
			pl->set->b[i].dirty = true;
		pl->set->img = pl->img;
	}

	/* Scaled, zero-copy, same buffers: show the newest frame as is. */
	if (path == PATH_SCALED && pl->direct && pl->set == &D.game &&
	    D.path == PATH_SCALED && D.direct && !D.fresh) {
		pl->idx = newest_idx(pl->set);
		if (pl->idx >= 0)
			return 0;
	}
	/* Otherwise draw the newest frame into a buffer that is not on screen. */
	for (i = 0; i < pl->set->n; i++)
		if (pl->set->b[i].st != B_FRONT) {
			pl->idx = i;
			break;
		}
	if (pl->idx < 0)
		pl->idx = 0;
	return 0;
}

static void fill_plan(const struct plan *pl, const struct frame_src *src)
{
	struct fbuf *b;

	if (pl->path != PATH_SCALED && pl->path != PATH_SOFT)
		return;
	b = &pl->set->b[pl->idx];
	if (pl->path == PATH_SCALED && pl->direct && pl->set == &D.game &&
	    D.path == PATH_SCALED && D.direct && src->valid && src->p == b->map)
		return;
	if (src->valid)
		blit(b, pl->fmt, &pl->img, pl->n, pl->sx0, pl->sy0, src->p, src->stride,
		     src->fmt);
	else if (b->dirty) {
		memset(b->map, 0, b->size);
		b->dirty = false;
	}
}

static struct bufset *slot_for(enum path p)
{
	switch (p) {
	case PATH_SCALED: return &D.game;
	case PATH_SOFT: return &D.screen;
	case PATH_BLANK: return &D.blank;
	default: return NULL;
	}
}

static void update_view(void)
{
	struct display_surface *v = &D.view;
	int i;

	memset(v, 0, sizeof(*v));
	if (!D.surf_set)
		return;
	v->width = D.sw;
	v->height = D.sh;
	v->max_width = D.cw;
	v->max_height = D.ch;
	v->format = D.sfmt;
	v->hw_scaled = D.path == PATH_SCALED;
	v->direct = D.direct;
	v->plane_format = D.act ? D.act->format : 0;
	v->soft_scale = D.path == PATH_SOFT ? D.soft_n : 1;
	v->dst = D.path == PATH_SCALED ? D.dst : D.img;
	if (D.direct && D.act) {
		v->count = D.act->n;
		for (i = 0; i < D.act->n; i++) {
			v->buffers[i].pixels = D.act->b[i].map;
			v->buffers[i].stride = (int)D.act->b[i].pitch;
		}
	} else {
		v->count = 1;
		v->buffers[0].pixels = D.shadow;
		v->buffers[0].stride = D.shadow_stride;
	}
}

static void finalize(int ci, int ki, int pi, uint32_t blob, bool modeset,
		     const drmModeModeInfo *mode, struct plan *pl, struct bufset *tmp)
{
	struct bufset *slot = slot_for(pl->path);
	int i;

	/* If the old path was zero-copy, keep the shadow current. */
	if (!D.fresh && D.path == PATH_SCALED && D.direct && !pl->direct && D.shadow &&
	    D.act) {
		int n = newest_idx(D.act);

		/* Direct buffers hold the caller's own format (native_fb_fmt). */
		if (n >= 0) {
			for (i = 0; i < D.sh; i++)
				memcpy(D.shadow + (size_t)i * D.shadow_stride,
				       D.act->b[n].map + (size_t)i * D.act->b[n].pitch,
				       (size_t)D.sw * D.sbpp);
			D.shadow_valid = true;
		}
	}

	if (pl->set == tmp && slot) {
		set_free(slot);   /* not on screen any more: the commit replaced it */
		*slot = *tmp;
		memset(tmp, 0, sizeof(*tmp));
		pl->set = slot;
	}
	if (pl->path != PATH_SCALED)
		set_free(&D.game);
	if (pl->path != PATH_SOFT)
		set_free(&D.screen);
	if (pl->path != PATH_BLANK)
		set_free(&D.blank);

	D.act = pl->path == PATH_NONE ? NULL : pl->set;
	if (D.act) {
		for (i = 0; i < D.act->n; i++)
			D.act->b[i].st = B_FREE;
		D.act->b[pl->idx].st = B_FRONT;
		D.act->b[pl->idx].fw = pl->src_w;
		D.act->b[pl->idx].fh = pl->src_h;
		D.act->b[pl->idx].dst = pl->dst;
	}
	D.path = pl->path;
	D.direct = pl->direct;
	D.dst = pl->dst;
	D.img = pl->img;
	D.soft_n = pl->n;
	D.sx0 = pl->sx0;
	D.sy0 = pl->sy0;
	D.geo_fw = pl->src_w;
	D.geo_fh = pl->src_h;
	D.geo_dst = pl->dst;
	D.plane = pi;
	if (modeset) {
		if (D.mode_blob)
			drmModeDestroyPropertyBlob(D.fd, D.mode_blob);
		D.mode_blob = blob;
		D.mode = *mode;
		D.conn = ci;
		D.crtc = ki;
	}
	D.gen++;
	D.flip_pending = false;
	D.draw_idx = -1;
	D.fresh = false;
	D.ext_shown = false;   /* the commit put the surface plan on the plane */
	update_view();
}

/*
 * After a modeset or a re-plan while an external FB (GL game) was on
 * screen: put it back, scaled for the (possibly new) output. If the plane
 * refuses it there, hand it back to its owner; the next
 * display_present_fb() then fails its TEST_ONLY and the caller falls back.
 */
static void ext_recommit(void)
{
	struct ext_fb *e = &D.ext_front;
	struct display_rect dst;
	drmModeAtomicReq *r;
	int ret;

	dst = compute_scaled(e->fb.src.w, e->fb.src.h, D.mode.hdisplay, D.mode.vdisplay,
			     D.scale, D.aspect);
	r = drmModeAtomicAlloc();
	if (!r)
		return;
	ret = add_plane_src(r, &D.planes[D.plane], D.crtcs[D.crtc].id, e->fb.fb_id,
			    e->fb.src.x, e->fb.src.y, e->fb.src.w, e->fb.src.h, &dst);
	if (ret == 0)
		ret = drmModeAtomicCommit(D.fd, r, 0, NULL);
	drmModeAtomicFree(r);
	if (ret) {
		dlog(DISPLAY_LOG_WARN, "external FB %u refused on %s: %s", e->fb.fb_id,
		     D.conns[D.conn].name, strerror(-ret));
		ext_release(e);
		return;
	}
	e->dst = dst;
	D.ext_shown = true;
	D.geo_fw = -1;   /* the next surface flip must re-send its rects */
}

/*
 * Strategy list, best first (see display_set_game_surface()):
 *   1. native format on the plane, hardware-scaled (zero-copy): XRGB8888
 *      through the frontend at any ratio, or RGB565 through the backend
 *      integer scaler (x1/x2/x4, kernel patch 0003);
 *   2. XRGB8888 on the plane, frontend-scaled, converted in the copy;
 *   3. software blit (CPU integer scale) into a screen-sized buffer.
 * Without a surface: no plane (backend background), then a black buffer.
 */
static int build_tries(int pi, struct strategy *t)
{
	uint32_t nat = native_fb_fmt(D.sfmt);
	int n = 0;

	if (!D.surf_set) {
		t[n++] = (struct strategy){ PATH_NONE, 0 };
		t[n++] = (struct strategy){ PATH_BLANK, DRM_FORMAT_XRGB8888 };
		return n;
	}
	if (!D.cfg.no_hw_scale) {
		if (nat == DRM_FORMAT_RGB565 && D.planes[pi].rgb565)
			t[n++] = (struct strategy){ PATH_SCALED, DRM_FORMAT_RGB565 };
		t[n++] = (struct strategy){ PATH_SCALED, DRM_FORMAT_XRGB8888 };
	}
	t[n++] = (struct strategy){ PATH_SOFT, DRM_FORMAT_XRGB8888 };
	return n;
}

static struct strat_cache *cache_find(uint32_t crtc_id, int W, int H,
				      const struct display_rect *dst)
{
	int i;

	for (i = 0; i < STRAT_CACHE_SIZE; i++) {
		struct strat_cache *c = &D.cache[i];

		if (c->valid && c->crtc_id == crtc_id && c->W == W && c->H == H &&
		    c->sw == D.sw && c->sh == D.sh && c->sfmt == D.sfmt &&
		    c->sflags == D.sflags && !memcmp(&c->dst, dst, sizeof(*dst)))
			return c;
	}
	return NULL;
}

static void cache_store(uint32_t crtc_id, int W, int H, const struct display_rect *dst,
			int ok)
{
	struct strat_cache *c = cache_find(crtc_id, W, H, dst);

	if (!c) {
		c = &D.cache[D.cache_next];
		D.cache_next = (D.cache_next + 1) % STRAT_CACHE_SIZE;
	}
	c->valid = true;
	c->crtc_id = crtc_id;
	c->W = W;
	c->H = H;
	c->sw = D.sw;
	c->sh = D.sh;
	c->sfmt = D.sfmt;
	c->sflags = D.sflags;
	c->dst = *dst;
	c->ok = ok;
}

/*
 * Lights up connector ci on CRTC ki with mode, using the first strategy
 * the kernel accepts. Each candidate is validated with TEST_ONLY before the
 * real (blocking) commit; the winner is cached per (CRTC, mode size, frame
 * size, format, destination rect), so known failures are skipped next time.
 */
static int apply(int ci, int ki, const drmModeModeInfo *mode, bool modeset)
{
	struct strategy tries[MAX_TRIES];
	int ntries, t, t0 = 0, pi, ret = -EINVAL;
	int W = mode->hdisplay, H = mode->vdisplay;
	uint32_t blob = 0;
	struct frame_src src;
	struct display_rect key_dst = { 0, 0, 0, 0 };
	struct strat_cache *cached = NULL;
	struct ov_plan ovp;
	const struct ov_plan *op;
	bool ext_on;

	if (ki < 0 || D.crtcs[ki].primary < 0)
		return -ENODEV;
	pi = D.crtcs[ki].primary;

	drain_flip(FLIP_DRAIN_MS);
	ext_on = D.ext_shown && D.ext_front.valid;
	/* The overlay rides along, placed for this mode (a modeset writes
	 * every plane, so a visible overlay is always included then). */
	op = ov_prepare(ki, W, H, modeset, &ovp) ? &ovp : NULL;

	if (modeset) {
		ret = drmModeCreatePropertyBlob(D.fd, mode, sizeof(*mode), &blob);
		if (ret)
			return ret;
	}
	ntries = build_tries(pi, tries);
	if (D.surf_set) {
		key_dst = compute_scaled(D.sw, D.sh, W, H, D.scale, D.aspect);
		cached = cache_find(D.crtcs[ki].id, W, H, &key_dst);
		if (cached && cached->ok < ntries)
			t0 = cached->ok;
	}
	src = newest_frame();

	for (t = t0; t < ntries; t++) {
		struct bufset tmp;
		struct plan pl;

		memset(&tmp, 0, sizeof(tmp));
		ret = make_plan(tries[t].path, tries[t].fmt, W, H, &tmp, &pl);
		if (ret) {
			dlog(DISPLAY_LOG_WARN, "%s: buffer allocation failed: %s",
			     path_names[tries[t].path], strerror(-ret));
			set_free(&tmp);
			continue;
		}
		fill_plan(&pl, &src);
		ret = commit_state(ci, ki, pi, blob, modeset, &pl, true, op);
		if (ret && op && op->show &&
		    commit_state(ci, ki, pi, blob, modeset, &pl, true, NULL) == 0) {
			/* The game plane is fine, the overlay is not: drop it. */
			ov_refused(op, ret);
			op = NULL;
			ret = 0;
		}
		if (ret == 0)
			ret = commit_state(ci, ki, pi, blob, modeset, &pl, false, op);
		if (ret == 0) {
			char f[5];

			memcpy(f, &pl.fmt, 4);
			f[4] = '\0';
			if (pl.path == PATH_SCALED)
				dlog(DISPLAY_LOG_INFO, "plane %u: %s %dx%d scaled to %dx%d+%d+%d (%s)",
				     D.planes[pi].id, f, D.sw, D.sh, pl.dst.w, pl.dst.h,
				     pl.dst.x, pl.dst.y, pl.direct ? "zero-copy" : "converted");
			else if (pl.path == PATH_SOFT)
				dlog(DISPLAY_LOG_INFO, "plane %u: software blit %dx%d x%d at %d,%d",
				     D.planes[pi].id, D.sw, D.sh, pl.n, pl.img.x, pl.img.y);
			else
				dlog(DISPLAY_LOG_INFO, "plane %u: %s", D.planes[pi].id,
				     path_names[pl.path]);
			if (D.surf_set)
				cache_store(D.crtcs[ki].id, W, H, &key_dst, t);
			finalize(ci, ki, pi, blob, modeset, mode, &pl, &tmp);
			if (op)
				ov_shown(op);
			else if (modeset)
				ov_off();
			if (ext_on)
				ext_recommit();
			return 0;
		}
		dlog(DISPLAY_LOG_WARN, "%s path rejected on %s/crtc %d: %s",
		     path_names[tries[t].path], D.conns[ci].name, ki, strerror(-ret));
		set_free(&tmp);
		/* A cached winner that now fails: forget it, try everything. */
		if (cached && t == t0 && t0 > 0) {
			cached->valid = false;
			cached = NULL;
			t = -1;
			t0 = 0;
		}
	}
	if (blob)
		drmModeDestroyPropertyBlob(D.fd, blob);
	return ret;
}

static void fill_info(struct display_output_info *in, bool builtin)
{
	const struct conn *c = &D.conns[D.conn];
	int hz = refresh_mhz(&D.mode);

	memset(in, 0, sizeof(*in));
	in->type = c->kind;
	snprintf(in->name, sizeof(in->name), "%s", c->name);
	snprintf(in->monitor, sizeof(in->monitor), "%s", c->monitor);
	in->has_edid = c->has_edid;
	in->builtin_mode = builtin;
	in->width = D.mode.hdisplay;
	in->height = D.mode.vdisplay;
	in->refresh_mhz = hz;
	snprintf(in->mode_name, sizeof(in->mode_name), "%dx%d@%d.%02d",
		 D.mode.hdisplay, D.mode.vdisplay, hz / 1000, (hz % 1000) / 10);
	in->connector_id = c->id;
	in->crtc_id = D.crtcs[D.crtc].id;
	in->plane_id = D.planes[D.plane].id;
	in->crtc_index = D.crtc;
}

static void arm_timer(int ms, int reason)
{
	struct itimerspec its = { 0 };

	if (D.tfd < 0)
		return;
	if (D.timer_reason == TIMER_DEBOUNCE && reason == TIMER_EDID_RETRY)
		return; /* a pending re-probe covers it */
	its.it_value.tv_sec = ms / 1000;
	its.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
	if (!its.it_value.tv_sec && !its.it_value.tv_nsec)
		its.it_value.tv_nsec = 1;
	timerfd_settime(D.tfd, 0, &its, NULL);
	D.timer_reason = reason;
}

/* Switches to connector ci. Falls back to the LCD if an external output fails. */
static int do_switch(int ci, enum display_event_reason why, struct display_switch_timing *tm)
{
	struct display_output_info before = D.info;
	drmModeModeInfo mode;
	bool builtin;
	int ki, ret;

	if (!pick_mode(&D.conns[ci], &mode, &builtin))
		return -ENOENT;
	ki = choose_crtc(ci);
	if (ki < 0)
		return -ENODEV;

	dlog(DISPLAY_LOG_INFO, "switching to %s%s%s%s: %dx%d@%.2f on crtc %u (index %d)%s",
	     D.conns[ci].name, D.conns[ci].monitor[0] ? " \"" : "",
	     D.conns[ci].monitor, D.conns[ci].monitor[0] ? "\"" : "",
	     mode.hdisplay, mode.vdisplay, refresh_mhz(&mode) / 1000.0,
	     D.crtcs[ki].id, ki, builtin ? " [built-in mode, no EDID]" : "");

	/* Audio first: the HDMI PCM must be closed before the encoder goes off. */
	if (before.type != DISPLAY_OUTPUT_NONE && D.cfg.on_audio)
		D.cfg.on_audio(DISPLAY_AUDIO_RELEASE, &before, D.cfg.user);

	ret = apply(ci, ki, &mode, true);
	if (ret && D.conns[ci].kind == DISPLAY_OUTPUT_LCD && (mode.type & DRM_MODE_TYPE_USERDEF)) {
		/* The retimed LCD mode is refused: the panel's own mode, always. */
		dlog(DISPLAY_LOG_ERROR, "LCD at %d Hz refused (%s): back to the panel's mode", D.cfg.lcd_refresh_hz,
		     strerror(-ret));
		D.cfg.lcd_refresh_hz = 0;
		if (pick_mode(&D.conns[ci], &mode, &builtin))
			ret = apply(ci, ki, &mode, true);
	}
	if (ret) {
		int k = -1;

		dlog(DISPLAY_LOG_ERROR, "modeset on %s failed: %s", D.conns[ci].name,
		     strerror(-ret));
		if (D.conns[ci].kind == DISPLAY_OUTPUT_HDMI) {
			D.conns[ci].failed = true;
			k = choose_output();
		}
		if (k >= 0 && k != ci && pick_mode(&D.conns[k], &mode, &builtin))
			ret = apply(k, choose_crtc(k), &mode, true);
		if (ret || k < 0 || k == ci) {
			/* Nothing changed on screen: let the audio resume. */
			if (before.type != DISPLAY_OUTPUT_NONE && D.cfg.on_audio)
				D.cfg.on_audio(DISPLAY_AUDIO_ACQUIRE, &before, D.cfg.user);
			return ret ? ret : -EIO;
		}
		why = DISPLAY_EVENT_FALLBACK;
	}
	tm->t_commit = display_now_ms();
	fill_info(&D.info, builtin);
	meas_start();
	if (before.type != DISPLAY_OUTPUT_NONE)
		D.stats.switches++;
	if (D.info.type == DISPLAY_OUTPUT_LCD && (D.mode.type & DRM_MODE_TYPE_USERDEF) &&
	    D.cfg.a20_clock_log) {
		struct lcd_clock lc;

		lcd_clock_model((unsigned long)D.mode.clock * 1000UL, &lc);
		dlog(DISPLAY_LOG_INFO, "LCD retimed to %d Hz: pixel clock %u kHz (A20 TCON0: pll-video %lu MHz / %u = "
		     "%lu.%03lu MHz expected, %d.%03d Hz)", D.cfg.lcd_refresh_hz, D.mode.clock, lc.parent_hz / 1000000,
		     lc.div, lc.pixel_hz / 1000000, lc.pixel_hz / 1000 % 1000,
		     (int)((uint64_t)lc.pixel_hz * 1000 / ((uint64_t)D.mode.htotal * D.mode.vtotal)) / 1000,
		     (int)((uint64_t)lc.pixel_hz * 1000 / ((uint64_t)D.mode.htotal * D.mode.vtotal)) % 1000);
	}

	dlog(DISPLAY_LOG_INFO, "output now %s %s (%s) crtc %u plane %u, path %s",
	     D.info.name, D.info.mode_name,
	     D.info.type == DISPLAY_OUTPUT_HDMI ? "external" : "internal",
	     D.info.crtc_id, D.info.plane_id, path_names[D.path]);
	if (tm->t_uevent)
		dlog(DISPLAY_LOG_INFO, "switch latency: uevent->probe %lld ms, probe->commit %lld ms, total %lld ms",
		     (long long)(tm->t_probe - tm->t_uevent),
		     (long long)(tm->t_commit - tm->t_probe),
		     (long long)(tm->t_commit - tm->t_uevent));

	if (D.info.type == DISPLAY_OUTPUT_HDMI && !D.info.has_edid &&
	    D.edid_retries_left > 0) {
		D.edid_retries_left--;
		arm_timer(D.cfg.edid_retry_ms, TIMER_EDID_RETRY);
	}

	if (D.cfg.on_output)
		D.cfg.on_output(&D.info, &before, why, tm, D.cfg.user);
	if (D.cfg.on_audio)
		D.cfg.on_audio(DISPLAY_AUDIO_ACQUIRE, &D.info, D.cfg.user);
	return 0;
}

enum probe_kind { PROBE_INIT, PROBE_HOTPLUG, PROBE_RESUME };

static void probe_all(enum probe_kind kind)
{
	bool init = kind == PROBE_INIT;
	int i;

	for (i = 0; i < D.nconn; i++) {
		struct conn *c = &D.conns[i];

		if (c->kind == DISPLAY_OUTPUT_HDMI) {
			drmModeConnection old = c->status;

			if (kind == PROBE_RESUME) {
				/*
				 * The game child probed the connectors itself (and the
				 * HPD work keeps the status current): the cached state
				 * is fresh, no EDID read unless it has no modes.
				 */
				probe_conn(c, false);
				if (c->status == DRM_MODE_CONNECTED && c->count_modes == 0)
					probe_conn(c, true);
			} else {
				/* Full probe: detect + EDID read over DDC when connected. */
				probe_conn(c, true);
			}
			if (c->status != old)
				c->failed = false;
		} else if (init) {
			/* Panels have static modes: the cached state is enough. */
			probe_conn(c, false);
			if (c->count_modes == 0 || c->status == DRM_MODE_UNKNOWNCONNECTION)
				probe_conn(c, true);
		}
		dlog(DISPLAY_LOG_DEBUG, "probe %s: %s, %d modes, edid %s%s%s", c->name,
		     c->status == DRM_MODE_CONNECTED ? "connected" :
		     c->status == DRM_MODE_DISCONNECTED ? "disconnected" : "unknown",
		     c->count_modes, c->has_edid ? "yes" : "no",
		     c->monitor[0] ? " " : "", c->monitor);
	}
}

static void reprobe(enum display_event_reason why)
{
	struct display_switch_timing tm = { 0 };
	drmModeModeInfo mode;
	bool builtin;
	int want;

	tm.t_uevent = why == DISPLAY_EVENT_HOTPLUG ? D.t_uevent : 0;
	tm.t_probe = display_now_ms();
	D.t_uevent = 0;
	probe_all(PROBE_HOTPLUG);
	if (why == DISPLAY_EVENT_HOTPLUG)
		D.edid_retries_left = D.cfg.edid_retries;

	want = choose_output();
	if (want < 0) {
		dlog(DISPLAY_LOG_WARN, "no usable output after hotplug, keeping %s",
		     D.conn >= 0 ? D.conns[D.conn].name : "nothing");
		return;
	}
	if (want == D.conn && pick_mode(&D.conns[want], &mode, &builtin) &&
	    mode_equal(&mode, &D.mode)) {
		/* Spurious or duplicate uevent: the real state did not change. */
		dlog(DISPLAY_LOG_INFO, "hotplug: no change (%s still %s)",
		     D.conns[want].name, D.info.mode_name);
		if (D.info.has_edid != D.conns[want].has_edid) {
			D.info.has_edid = D.conns[want].has_edid;
			snprintf(D.info.monitor, sizeof(D.info.monitor), "%s",
				 D.conns[want].monitor);
		}
		if (D.info.type == DISPLAY_OUTPUT_HDMI && !D.info.has_edid &&
		    D.edid_retries_left > 0) {
			D.edid_retries_left--;
			arm_timer(D.cfg.edid_retry_ms, TIMER_EDID_RETRY);
		}
		return;
	}
	if (want == D.conn)
		why = DISPLAY_EVENT_REPROBE;
	do_switch(want, why, &tm);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void hotplug_kick(const char *what)
{
	if (!D.t_uevent)
		D.t_uevent = display_now_ms();
	dlog(DISPLAY_LOG_INFO, "hotplug uevent (%s), debounce %d ms", what,
	     D.cfg.debounce_ms);
	arm_timer(D.cfg.debounce_ms, TIMER_DEBOUNCE);
}

static bool is_our_card(const struct uevent *ev)
{
	const char *base;

	if (!ev->devname)
		return true;
	base = strrchr(ev->devname, '/');
	base = base ? base + 1 : ev->devname;
	return strcmp(base, D.devname) == 0;
}

static int handle_uevents(void)
{
	static struct uevent ev;
	int n = 0, r;

	if (D.ufd < 0)
		return 0;
	while ((r = uevent_read(D.ufd, &ev)) != 0) {
		if (r == -ENOBUFS) {
			hotplug_kick("socket overflow");
			n++;
			continue;
		}
		if (r < 0)
			break;
		if (uevent_is_drm_hotplug(&ev) && is_our_card(&ev)) {
			char what[48];

			if (ev.connector_id)
				snprintf(what, sizeof(what), "connector %d", ev.connector_id);
			else
				snprintf(what, sizeof(what), "seq %llu", ev.seqnum);
			hotplug_kick(what);
			n++;
		}
	}
	return n;
}

/*
 * A page-flip event that never comes (a CRTC whose vblank interrupt did not
 * come back, a lost event) would leave every buffer busy and the picture
 * frozen for good. After FLIP_STUCK_MS, log it and re-commit the whole
 * output (the same full commit as display_set_active(true)).
 */
static void flip_watchdog(void)
{
	int64_t late;
	int ret;

	if (!D.flip_pending || !D.active || D.suspended || D.conn < 0 || D.crtc < 0)
		return;
	late = display_now_ms() - D.flip_at;
	if (late < FLIP_STUCK_MS)
		return;
	dlog(DISPLAY_LOG_WARN, "no page-flip event for %lld ms on %s: re-committing the output",
	     (long long)late, D.info.name);
	ret = apply(D.conn, D.crtc, &D.mode, true);
	if (ret)
		dlog(DISPLAY_LOG_ERROR, "re-commit after a lost flip: %s", strerror(-ret));
}

int display_handle_events(void)
{
	struct pollfd pfd = { D.fd, POLLIN, 0 };
	uint64_t exp;
	int n = 0;

	if (!D.inited)
		return -EINVAL;
	while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		handle_drm();
		n++;
	}
	n += handle_uevents();
	if (D.tfd >= 0 && read(D.tfd, &exp, sizeof(exp)) == (ssize_t)sizeof(exp)) {
		int reason = D.timer_reason;

		D.timer_reason = TIMER_NONE;
		n++;
		if (D.suspended || !D.active)
			D.reprobe_pending = true;   /* handled by resume / set_active(true) */
		else
			reprobe(reason == TIMER_DEBOUNCE ? DISPLAY_EVENT_HOTPLUG
							 : DISPLAY_EVENT_REPROBE);
	}
	flip_watchdog();
	ov_kick_stale();
	return n;
}

int display_wait_events(int timeout_ms)
{
	struct pollfd pfd = { D.efd, POLLIN, 0 };
	int r;

	if (!D.inited)
		return -EINVAL;
	r = poll(&pfd, 1, timeout_ms);
	if (r < 0)
		return -errno; /* -EINTR lets callers check their quit flag */
	return display_handle_events();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void display_config_defaults(struct display_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->hdmi_width = 1280;
	cfg->hdmi_height = 720;
	cfg->hdmi_refresh = 60;
	cfg->hdmi_builtin_mode = true;
	cfg->hdmi_crtc_index = -1;
	cfg->scale = DISPLAY_SCALE_ASPECT;
	cfg->soft_scale_max = 2;
	cfg->buffers = 2;
	cfg->debounce_ms = 250;
	cfg->edid_retry_ms = 1000;
	cfg->edid_retries = 2;
	cfg->log_level = DISPLAY_LOG_INFO;
	cfg->internal_mode = DISPLAY_INTERNAL_AUTO;
	cfg->a20_clock_log = true;
}

static int open_card(const char *path)
{
	uint64_t cap = 0;
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) || !cap ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		close(fd);
		return -EOPNOTSUPP;
	}
	return fd;
}

int display_init(const struct display_config *cfg)
{
	struct display_switch_timing tm = { 0 };
	struct epoll_event ee;
	int i, ret, want;

	if (D.inited)
		return -EBUSY;
	memset(&D, 0, sizeof(D));
	D.fd = D.ufd = D.tfd = D.efd = -1;
	D.conn = D.crtc = D.plane = -1;
	D.draw_idx = -1;
	D.active = true;
	D.ov.front = -1;
	if (cfg)
		D.cfg = *cfg;
	else
		display_config_defaults(&D.cfg);
	if (D.cfg.buffers < 2)
		D.cfg.buffers = 2;
	if (D.cfg.buffers > DISPLAY_MAX_BUFFERS)
		D.cfg.buffers = DISPLAY_MAX_BUFFERS;
	D.scale = D.cfg.scale;

	if (D.cfg.device) {
		D.fd = open_card(D.cfg.device);
		snprintf(D.devname, sizeof(D.devname), "%s",
			 strrchr(D.cfg.device, '/') ? strrchr(D.cfg.device, '/') + 1
						    : D.cfg.device);
	} else {
		for (i = 0; i < 8 && D.fd < 0; i++) {
			char path[32];
			drmModeRes *res;
			int fd;

			snprintf(path, sizeof(path), "/dev/dri/card%d", i);
			fd = open_card(path);
			if (fd < 0)
				continue;
			res = drmModeGetResources(fd);
			if (res && res->count_connectors > 0 && res->count_crtcs > 0) {
				D.fd = fd;
				snprintf(D.devname, sizeof(D.devname), "card%d", i);
			} else {
				close(fd);
			}
			drmModeFreeResources(res);
		}
		if (D.fd < 0)
			D.fd = -ENODEV;
	}
	if (D.fd < 0) {
		ret = D.fd;
		D.fd = -1;
		dlog(DISPLAY_LOG_ERROR, "no usable atomic KMS device: %s", strerror(-ret));
		return ret;
	}
	/* The first opener is master already; this only helps after a drop. */
	(void)drmSetMaster(D.fd);

	ret = enumerate();
	if (ret) {
		dlog(DISPLAY_LOG_ERROR, "enumeration failed: %s", strerror(-ret));
		goto fail;
	}

	D.ufd = uevent_open();
	if (D.ufd < 0) {
		dlog(DISPLAY_LOG_WARN, "uevent socket: %s (no hotplug)", strerror(-D.ufd));
		D.ufd = -1;
	}
	D.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	D.efd = epoll_create1(EPOLL_CLOEXEC);
	if (D.efd < 0) {
		ret = -errno;
		goto fail;
	}
	memset(&ee, 0, sizeof(ee));
	ee.events = EPOLLIN;
	ee.data.fd = D.fd;
	epoll_ctl(D.efd, EPOLL_CTL_ADD, D.fd, &ee);
	if (D.ufd >= 0) {
		ee.data.fd = D.ufd;
		epoll_ctl(D.efd, EPOLL_CTL_ADD, D.ufd, &ee);
	}
	if (D.tfd >= 0) {
		ee.data.fd = D.tfd;
		epoll_ctl(D.efd, EPOLL_CTL_ADD, D.tfd, &ee);
	}
	D.inited = true;

	/* Startup with HDMI already plugged in is just a probe like any other. */
	probe_all(PROBE_INIT);
	D.edid_retries_left = D.cfg.edid_retries;
	want = choose_output();
	if (want < 0) {
		dlog(DISPLAY_LOG_ERROR, "no connected output");
		ret = -ENODEV;
		goto fail;
	}
	tm.t_probe = display_now_ms();
	ret = do_switch(want, DISPLAY_EVENT_INIT, &tm);
	if (ret)
		goto fail;
	return 0;

fail:
	D.inited = false;
	display_shutdown();
	return ret;
}

void display_shutdown(void)
{
	int i;

	if (D.fd >= 0)
		drain_flip(FLIP_DRAIN_MS);
	/* Hand external FBs back while the DRM fd is still open. */
	ext_release(&D.ext_pending);
	ext_release(&D.ext_front);
	/* Removing the framebuffers also disables the plane that shows them. */
	set_free(&D.game);
	set_free(&D.screen);
	set_free(&D.blank);
	if (D.fd >= 0) {
		fb_destroy(&D.ov.fb[0]);
		fb_destroy(&D.ov.fb[1]);
	}
	free(D.ov.pix);
	free(D.shadow);
	D.shadow = NULL;
	if (D.mode_blob && D.fd >= 0)
		drmModeDestroyPropertyBlob(D.fd, D.mode_blob);
	for (i = 0; i < D.nconn; i++)
		free(D.conns[i].modes);
	if (D.efd >= 0)
		close(D.efd);
	if (D.tfd >= 0)
		close(D.tfd);
	uevent_close(D.ufd);
	if (D.fd >= 0)
		close(D.fd);
	memset(&D, 0, sizeof(D));
	D.fd = D.ufd = D.tfd = D.efd = -1;
	D.conn = D.crtc = D.plane = -1;
}

int display_get_fd(void)
{
	return D.efd;
}

int display_get_drm_fd(void)
{
	return D.fd;
}

const struct display_output_info *display_output(void)
{
	return &D.info;
}

const struct display_surface *display_get_surface(void)
{
	return D.surf_set ? &D.view : NULL;
}

bool display_flip_pending(void)
{
	return D.flip_pending;
}

void display_get_stats(struct display_stats *st)
{
	*st = D.stats;
}

const struct display_surface *display_set_game_surface(int width, int height,
						       uint32_t format,
						       unsigned int flags)
{
	int bpp = bpp_of(format), ret, i;
	uint8_t *shadow;

	if (!D.inited || D.suspended || width <= 0 || height <= 0 || width > 4096 || height > 4096 || !bpp)
		return NULL;

	drain_flip(FLIP_DRAIN_MS);
	shadow = calloc((size_t)width * height, (size_t)bpp);
	if (!shadow)
		return NULL;
	free(D.shadow);
	D.shadow = shadow;
	D.shadow_stride = width * bpp;
	D.shadow_valid = false;

	/* The old buffers hold the previous surface's image: start black. */
	D.fresh = true;
	for (i = 0; i < D.game.n; i++)
		D.game.b[i].dirty = true;
	D.surf_set = true;
	D.cw = D.sw = width;
	D.ch = D.sh = height;
	D.sfmt = format;
	D.sbpp = bpp;
	D.sflags = flags;

	ret = apply(D.conn, D.crtc, &D.mode, false);
	if (ret) {
		/* Some drivers want a full modeset for a plane change. */
		ret = apply(D.conn, D.crtc, &D.mode, true);
	}
	if (ret) {
		dlog(DISPLAY_LOG_ERROR, "cannot show a %dx%d surface: %s", width, height,
		     strerror(-ret));
		D.surf_set = false;
		update_view();
		return NULL;
	}
	D.info.plane_id = D.planes[D.plane].id;
	return &D.view;
}

/* TEST_ONLY: can the current plane show a fw x fh frame at dst? */
static int test_geometry(int fw, int fh, const struct display_rect *dst)
{
	drmModeAtomicReq *r;
	int f, ret;

	if (!D.act || D.path != PATH_SCALED)
		return -EINVAL;
	f = newest_idx(D.act);
	if (f < 0)
		f = 0;
	r = drmModeAtomicAlloc();
	if (!r)
		return -ENOMEM;
	ret = add_plane(r, &D.planes[D.plane], D.crtcs[D.crtc].id, D.act->b[f].fb_id,
			fw, fh, dst);
	if (ret == 0)
		ret = drmModeAtomicCommit(D.fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
	drmModeAtomicFree(r);
	return ret;
}

int display_set_frame_size(int width, int height)
{
	struct strategy tries[MAX_TRIES];
	struct strat_cache *c;
	struct display_rect key;
	int W, H, n, k, cur = -1;

	if (!D.inited || !D.surf_set || !D.act || D.suspended)
		return -EINVAL;
	if (width < 1 || height < 1 || width > D.cw || height > D.ch)
		return -EINVAL;
	if (width == D.sw && height == D.sh)
		return 0;
	W = D.mode.hdisplay;
	H = D.mode.vdisplay;
	D.sw = width;
	D.sh = height;
	key = compute_scaled(D.sw, D.sh, W, H, D.scale, D.aspect);

	/* Index of the strategy in use, in the try list. */
	n = build_tries(D.plane, tries);
	for (k = 0; k < n; k++)
		if (tries[k].path == D.path &&
		    (D.path == PATH_SOFT || tries[k].fmt == D.act->format))
			cur = k;
	c = cache_find(D.crtcs[D.crtc].id, W, H, &key);

	if (D.path == PATH_SCALED) {
		/*
		 * Same strategy still valid: just record the new rectangles;
		 * flip() sends them with the first frame of the new size.
		 */
		if ((c && c->ok == cur) || (!c && test_geometry(D.sw, D.sh, &key) == 0)) {
			if (!c)
				cache_store(D.crtcs[D.crtc].id, W, H, &key, cur);
			D.dst = key;
			D.img = (struct display_rect){ 0, 0, D.sw, D.sh };
			update_view();
			dlog(DISPLAY_LOG_DEBUG, "frame size %dx%d -> %dx%d+%d+%d", D.sw, D.sh,
			     key.w, key.h, key.x, key.y);
			return 0;
		}
	} else if (D.path == PATH_SOFT && (D.cfg.no_hw_scale || (c && c->ok == cur))) {
		struct display_rect img;
		int f, sx0, sy0, i;

		soft_geometry(W, H, &img, &f, &sx0, &sy0);
		if (memcmp(&img, &D.img, sizeof(img))) {
			for (i = 0; i < D.act->n; i++)
				D.act->b[i].dirty = true;   /* re-clear the borders */
			D.act->img = img;
		}
		D.img = img;
		D.soft_n = f;
		D.sx0 = sx0;
		D.sy0 = sy0;
		update_view();
		return 0;
	}
	dlog(DISPLAY_LOG_INFO, "frame size %dx%d: re-picking the scaling strategy",
	     D.sw, D.sh);
	return apply(D.conn, D.crtc, &D.mode, false);
}

int display_set_scaling(enum display_scale_mode mode, double aspect)
{
	D.scale = mode;
	D.aspect = aspect;
	if (!D.inited || !D.surf_set || D.suspended)
		return 0;
	return apply(D.conn, D.crtc, &D.mode, false);
}

/*
 * A free buffer of the active set. With the screen off nothing is flipped,
 * so the queued (newest) frame is recycled instead of waiting forever.
 */
static int free_or_steal(void)
{
	int i;

	if (!D.act)
		return -1;
	i = set_find(D.act, B_FREE);
	if (i < 0 && (!D.active || D.suspended)) {
		i = set_find(D.act, B_QUEUED);
		if (i >= 0) {
			D.act->b[i].st = B_FREE;
			D.stats.dropped++;
		}
	}
	return i;
}

int display_begin_frame(int timeout_ms)
{
	int64_t end = timeout_ms >= 0 ? display_now_ms() + timeout_ms : 0;

	if (!D.inited || !D.surf_set)
		return -EINVAL;
	if (D.suspended)
		return -EAGAIN;
	for (;;) {
		if (D.act && D.direct) {
			int i = set_find(D.act, B_DRAW);

			if (i < 0)
				i = free_or_steal();
			if (i >= 0) {
				D.act->b[i].st = B_DRAW;
				D.draw_idx = i;
				D.draw_gen = D.gen;
				D.draw_shadow = false;
				return i;
			}
		} else if (D.act && free_or_steal() >= 0) {
			D.draw_idx = 0;
			D.draw_gen = D.gen;
			D.draw_shadow = true;
			return 0;
		}
		{
			int left = -1, r;

			if (timeout_ms >= 0) {
				int64_t l = end - display_now_ms();

				if (l <= 0)
					return -ETIMEDOUT;
				left = (int)l;
			}
			r = display_wait_events(left);
			if (r < 0)
				return r;
			if (!D.surf_set)
				return -EINVAL;
		}
	}
}

/* Waits for a free scanout buffer in the active set (fallback/convert paths). */
static int wait_free(int timeout_ms)
{
	int64_t end = display_now_ms() + timeout_ms;

	for (;;) {
		int i = free_or_steal();
		int64_t left;

		if (i >= 0)
			return i;
		left = end - display_now_ms();
		if (left <= 0 || display_wait_events((int)left) < 0)
			return -ETIMEDOUT;
	}
}

int display_present(void)
{
	int i;

	if (!D.inited || !D.surf_set || D.draw_idx < 0)
		return -EINVAL;
	if (D.suspended)
		return -EAGAIN;
	if (!D.draw_shadow) {
		i = D.draw_idx;
		D.draw_idx = -1;
		if (D.draw_gen != D.gen || !D.act || D.act->b[i].st != B_DRAW) {
			D.stats.dropped++;
			return -EAGAIN;
		}
		submit(i);
		return 0;
	}
	/* The frame is in the shadow buffer, which survives output switches. */
	D.draw_idx = -1;
	D.shadow_valid = true;
	i = wait_free(PRESENT_WAIT_MS);
	if (i < 0) {
		D.stats.dropped++;
		return -EAGAIN;
	}
	blit(&D.act->b[i], D.act->format, &D.img, D.soft_n, D.sx0, D.sy0, D.shadow, D.shadow_stride, D.sfmt);
	submit(i);
	return 0;
}

int display_present_copy(const void *src, int src_stride)
{
	int i = display_begin_frame(PRESENT_WAIT_MS);

	if (i < 0)
		return i;
	if (!D.draw_shadow) {
		blit(&D.act->b[i], D.act->format, &D.img, D.soft_n, D.sx0, D.sy0, src, src_stride, D.sfmt);
		return display_present();
	}
	/* Fallback path: convert straight into the scanout buffer, skip the shadow. */
	i = free_or_steal();
	D.draw_idx = -1;
	if (i < 0) {
		D.stats.dropped++;
		return -EAGAIN;
	}
	blit(&D.act->b[i], D.act->format, &D.img, D.soft_n, D.sx0, D.sy0, src, src_stride, D.sfmt);
	D.shadow_valid = false;
	submit(i);
	return 0;
}

int display_present_frame(const void *src, int width, int height, int src_stride)
{
	if (D.suspended)
		return -EAGAIN;
	if (!D.inited || !D.surf_set || !src)
		return -EINVAL;
	if (width != D.sw || height != D.sh) {
		int ret = display_set_frame_size(width, height);

		if (ret < 0)
			return ret;
	}
	return display_present_copy(src, src_stride);
}

/* ------------------------------------------------------------------ */
/* External framebuffers (GL / GBM zero-copy)                          */
/* ------------------------------------------------------------------ */

static bool rect_eq(const struct display_rect *a, const struct display_rect *b)
{
	return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

int display_present_fb(const struct display_fb *fb, int timeout_ms)
{
	const struct plane *p;
	struct display_rect dst;
	drmModeAtomicReq *r;
	int64_t end;
	bool tested;
	int ret;

	if (!D.inited || D.plane < 0 || D.crtc < 0 || !fb || !fb->fb_id)
		return -EINVAL;
	if (fb->src.w < 1 || fb->src.h < 1 || fb->src.x < 0 || fb->src.y < 0 ||
	    fb->src.x + fb->src.w > fb->width || fb->src.y + fb->src.h > fb->height)
		return -EINVAL;
	if (D.suspended || !D.active)
		return -EAGAIN;

	/* One flip in flight at a time: waiting here is the vsync pacing. */
	end = display_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
	while (D.flip_pending) {
		int left = -1;

		if (timeout_ms == 0)
			return -EBUSY;
		if (timeout_ms > 0) {
			int64_t l = end - display_now_ms();

			if (l <= 0)
				return -ETIMEDOUT;
			left = (int)l;
		}
		ret = display_wait_events(left);
		if (ret < 0)
			return ret;
		if (D.suspended || !D.active)
			return -EAGAIN;
	}

	/* Same scaling and letterboxing as the game surface. */
	dst = compute_scaled(fb->src.w, fb->src.h, D.mode.hdisplay, D.mode.vdisplay,
			     D.scale, D.aspect);
	p = &D.planes[D.plane];
	tested = D.ext_tested.valid && D.ext_tested_crtc == D.crtcs[D.crtc].id &&
		 D.ext_tested.fb.format == fb->format && D.ext_tested.fb.width == fb->width &&
		 D.ext_tested.fb.height == fb->height && rect_eq(&D.ext_tested.fb.src, &fb->src) &&
		 rect_eq(&D.ext_tested.dst, &dst);

	r = drmModeAtomicAlloc();
	if (!r)
		return -ENOMEM;
	/* Full plane state every time: BOs may differ in size or source rect. */
	ret = add_plane_src(r, p, D.crtcs[D.crtc].id, fb->fb_id, fb->src.x, fb->src.y,
			    fb->src.w, fb->src.h, &dst);
	if (ret == 0 && !tested) {
		ret = drmModeAtomicCommit(D.fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
		if (ret) {
			char f[5];

			memcpy(f, &fb->format, 4);
			f[4] = '\0';
			dlog(DISPLAY_LOG_WARN, "external FB %u (%s %dx%d, src %dx%d+%d+%d -> %dx%d) "
			     "refused: %s", fb->fb_id, f, fb->width, fb->height, fb->src.w,
			     fb->src.h, fb->src.x, fb->src.y, dst.w, dst.h, strerror(-ret));
			drmModeAtomicFree(r);
			return -EINVAL;
		}
		D.ext_tested.valid = true;
		D.ext_tested.fb = *fb;
		D.ext_tested.dst = dst;
		D.ext_tested_crtc = D.crtcs[D.crtc].id;
	}
	if (ret == 0) {
		/* An overlay change rides along (the N64 zero-copy path too). */
		struct ov_plan op;
		bool with_ov = ov_for_flip(&op);

		if (with_ov && ov_add(r, &op) == 0) {
			ret = drmModeAtomicCommit(D.fd, r,
						  DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
						  (void *)(uintptr_t)D.gen);
			if (ret == 0) {
				ov_pending(&op);
				goto committed;
			}
			/* retry the frame alone: the overlay never costs a frame */
			drmModeAtomicFree(r);
			r = drmModeAtomicAlloc();
			if (!r)
				return -ENOMEM;
			ret = add_plane_src(r, p, D.crtcs[D.crtc].id, fb->fb_id, fb->src.x, fb->src.y,
					    fb->src.w, fb->src.h, &dst);
			if (ret == 0) {
				ret = drmModeAtomicCommit(D.fd, r,
							  DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
							  (void *)(uintptr_t)D.gen);
				if (ret == 0)
					ov_refused(&op, -EINVAL);
			}
		} else {
			ret = drmModeAtomicCommit(D.fd, r,
						  DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
						  (void *)(uintptr_t)D.gen);
		}
	}
committed:
	drmModeAtomicFree(r);
	if (ret) {
		D.ext_tested.valid = false;
		return (ret == -EINVAL || ret == -ERANGE || ret == -ENOENT) ? -EINVAL : ret;
	}
	D.flip_at = display_now_ms();
	D.ext_pending.valid = true;
	D.ext_pending.fb = *fb;
	D.ext_pending.dst = dst;
	D.flip_pending = true;
	D.flip_ext = true;
	D.ext_shown = true;
	D.geo_fw = -1;   /* the plane rects are the external FB's now */
	return 0;
}

/* ------------------------------------------------------------------ */
/* DRM master hand-off                                                 */
/* ------------------------------------------------------------------ */

int display_suspend(void)
{
	int ret;

	if (!D.inited)
		return -EINVAL;
	if (D.suspended)
		return 0;
	drain_flip(FLIP_DRAIN_MS);
	if (drmDropMaster(D.fd) < 0) {
		ret = -errno;
		dlog(DISPLAY_LOG_ERROR, "drmDropMaster: %s", strerror(-ret));
		return ret;
	}
	D.suspended = true;
	dlog(DISPLAY_LOG_INFO, "suspended: DRM master dropped");
	return 0;
}

int display_resume(void)
{
	struct display_switch_timing tm = { 0 };
	struct itimerspec off;
	drmModeModeInfo mode;
	bool builtin, same;
	int64_t t0 = display_now_ms();
	int want, ret;

	if (!D.inited)
		return -EINVAL;
	if (!D.suspended)
		return 0;
	if (drmSetMaster(D.fd) < 0) {
		ret = -errno;
		dlog(DISPLAY_LOG_ERROR, "drmSetMaster: %s (is the game process still running?)",
		     strerror(-ret));
		return ret;
	}
	D.suspended = false;

	/* Uevents seen during the game are superseded by the probe below. */
	if (D.ufd >= 0) {
		static struct uevent ev;
		int r;

		while ((r = uevent_read(D.ufd, &ev)) > 0 || r == -ENOBUFS)
			;
	}
	memset(&off, 0, sizeof(off));
	if (D.tfd >= 0)
		timerfd_settime(D.tfd, 0, &off, NULL);
	D.timer_reason = TIMER_NONE;
	D.t_uevent = 0;
	D.reprobe_pending = false;

	/*
	 * The child's exit removed its framebuffers: the kernel disabled the
	 * plane, and the CRTC too when the FB was on the primary plane
	 * (atomic_remove_fb() in drm_framebuffer.c). Assume nothing about the
	 * hardware state: re-commit everything.
	 */
	D.geo_fw = -1;
	D.flip_pending = false;
	tm.t_probe = display_now_ms();
	probe_all(PROBE_RESUME);
	want = choose_output();
	if (want < 0)
		want = D.conn;
	same = want == D.conn &&
	       (!pick_mode(&D.conns[want], &mode, &builtin) || mode_equal(&mode, &D.mode));
	if (same) {
		ret = apply(D.conn, D.crtc, &D.mode, true);
	} else {
		dlog(DISPLAY_LOG_INFO, "output changed during the game");
		D.edid_retries_left = D.cfg.edid_retries;
		ret = do_switch(want, DISPLAY_EVENT_HOTPLUG, &tm);
	}
	dlog(DISPLAY_LOG_INFO, "resumed on %s in %lld ms%s", D.info.name,
	     (long long)(display_now_ms() - t0), ret ? " (commit failed)" : "");
	return ret;
}

/* ------------------------------------------------------------------ */
/* Screen off/on                                                       */
/* ------------------------------------------------------------------ */

int display_set_active(bool active)
{
	int64_t t0 = display_now_ms();
	int ret;

	if (!D.inited || D.conn < 0 || D.crtc < 0)
		return -EINVAL;
	if (D.suspended)
		return -EAGAIN;
	if (active == D.active)
		return 0;

	if (!active) {
		drmModeAtomicReq *r;

		drain_flip(FLIP_DRAIN_MS);
		r = drmModeAtomicAlloc();
		if (!r)
			return -ENOMEM;
		/*
		 * ACTIVE = 0 only: mode, connector routing and planes stay in
		 * the state. The encoder disable turns the panel and its
		 * backlight off through drm_panel (or stops the HDMI signal).
		 */
		ret = add(r, D.crtcs[D.crtc].id, D.crtcs[D.crtc].prop_active, 0);
		if (ret == 0)
			ret = drmModeAtomicCommit(D.fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		drmModeAtomicFree(r);
		if (ret) {
			dlog(DISPLAY_LOG_ERROR, "screen off: %s", strerror(-ret));
			return ret;
		}
		D.active = false;
		dlog(DISPLAY_LOG_INFO, "screen off (%s) in %lld ms", D.info.name,
		     (long long)(display_now_ms() - t0));
		return 0;
	}

	/* Full re-commit with the newest frame (queued while off). */
	D.active = true;
	ret = apply(D.conn, D.crtc, &D.mode, true);
	if (ret) {
		D.active = false;
		dlog(DISPLAY_LOG_ERROR, "screen on: %s", strerror(-ret));
		return ret;
	}
	dlog(DISPLAY_LOG_INFO, "screen on (%s) in %lld ms", D.info.name,
	     (long long)(display_now_ms() - t0));
	/* A hotplug while the screen was off is evaluated now. */
	if (D.reprobe_pending) {
		D.reprobe_pending = false;
		reprobe(DISPLAY_EVENT_HOTPLUG);
	}
	return 0;
}

bool display_is_active(void)
{
	return D.active;
}

/* ------------------------------------------------------------------ */
/* LCD refresh (§3.1)                                                  */
/* ------------------------------------------------------------------ */

int display_set_lcd_refresh(int hz)
{
	struct display_switch_timing tm = { 0 };
	drmModeModeInfo mode;
	bool builtin;
	int old = D.cfg.lcd_refresh_hz, ret;

	if (hz < 30 || hz > 120)
		hz = 0;
	if (hz == D.cfg.lcd_refresh_hz && D.inited)
		return 0;
	D.cfg.lcd_refresh_hz = hz;
	if (!D.inited)
		return 0;
	dlog(DISPLAY_LOG_INFO, "LCD refresh: %s", hz ? "retimed" : "the panel's own mode");
	if (D.suspended || !D.active) {
		D.reprobe_pending = true;       /* applied by resume / screen on */
		return 0;
	}
	if (D.conn < 0 || D.conns[D.conn].kind != DISPLAY_OUTPUT_LCD)
		return 0;                       /* next time the LCD is chosen */
	if (!pick_mode(&D.conns[D.conn], &mode, &builtin) || mode_equal(&mode, &D.mode))
		return 0;
	tm.t_probe = display_now_ms();
	ret = do_switch(D.conn, DISPLAY_EVENT_REPROBE, &tm);
	if (!ret && D.cfg.lcd_refresh_hz != hz)
		ret = -EINVAL;                  /* do_switch() fell back to the panel's mode */
	if (ret) {
		dlog(DISPLAY_LOG_WARN, "LCD refresh change refused (%s): keeping the current mode", strerror(-ret));
		D.cfg.lcd_refresh_hz = (D.mode.type & DRM_MODE_TYPE_USERDEF) ? old : 0;
	}
	return ret;
}

int display_get_lcd_refresh(void)
{
	return D.cfg.lcd_refresh_hz;
}
