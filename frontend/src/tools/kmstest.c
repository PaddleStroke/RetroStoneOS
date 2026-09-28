/*
 * rsos-kmstest - display bring-up tool for RetroStoneOS.
 *
 *   rsos-kmstest --list       dump connectors/encoders/CRTCs/planes (mini modetest)
 *   rsos-kmstest --monitor    print raw kernel uevents (check HDMI HPD polling)
 *   rsos-kmstest [options]    animated 320x240 test pattern, hardware-scaled to
 *                             the active output, live LCD/HDMI switching
 *
 * Every event is logged with a CLOCK_MONOTONIC timestamp so plug-to-picture
 * latency can be read straight from the UART console.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../display.h"
#include "../font8x8.h"
#include "../uevent.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void tlog(const char *fmt, ...)
{
	int64_t t = display_now_ms();
	va_list ap;

	fprintf(stdout, "[%6lld.%03lld] ", (long long)(t / 1000), (long long)(t % 1000));
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
	fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* --list                                                              */
/* ------------------------------------------------------------------ */

static void fourcc_str(uint32_t f, char out[5])
{
	int i;

	for (i = 0; i < 4; i++) {
		char c = (char)((f >> (8 * i)) & 0xff);

		out[i] = (c >= 32 && c < 127) ? c : '?';
	}
	out[4] = '\0';
}

static const char *conn_status(drmModeConnection c)
{
	switch (c) {
	case DRM_MODE_CONNECTED: return "connected";
	case DRM_MODE_DISCONNECTED: return "disconnected";
	default: return "unknown";
	}
}

static const char *encoder_type(uint32_t t)
{
	switch (t) {
	case DRM_MODE_ENCODER_NONE: return "none";
	case DRM_MODE_ENCODER_DAC: return "DAC";
	case DRM_MODE_ENCODER_TMDS: return "TMDS";
	case DRM_MODE_ENCODER_LVDS: return "LVDS";
	case DRM_MODE_ENCODER_TVDAC: return "TVDAC";
	case DRM_MODE_ENCODER_VIRTUAL: return "virtual";
	case DRM_MODE_ENCODER_DSI: return "DSI";
	case DRM_MODE_ENCODER_DPMST: return "DPMST";
	case DRM_MODE_ENCODER_DPI: return "DPI";
	default: return "?";
	}
}

static void print_mode(const drmModeModeInfo *m, const char *indent)
{
	double hz = 0.0;

	if (m->htotal && m->vtotal)
		hz = m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		hz *= 2;
	printf("%s%-12s %7.3f Hz  clk %6u kHz  h %u %u %u %u  v %u %u %u %u  flags 0x%x%s%s\n",
	       indent, m->name, hz, m->clock, m->hdisplay, m->hsync_start, m->hsync_end,
	       m->htotal, m->vdisplay, m->vsync_start, m->vsync_end, m->vtotal, m->flags,
	       (m->type & DRM_MODE_TYPE_PREFERRED) ? "  preferred" : "",
	       (m->flags & DRM_MODE_FLAG_INTERLACE) ? "  interlaced" : "");
}

static void print_edid_summary(const uint8_t *e, uint32_t len)
{
	char name[16] = "";
	int d;

	if (len < 128) {
		printf(" (short EDID)");
		return;
	}
	printf(" (EDID %u bytes, mfg %c%c%c, product 0x%04x", len,
	       '@' + ((e[8] >> 2) & 0x1f), '@' + (((e[8] & 3) << 3) | (e[9] >> 5)),
	       '@' + (e[9] & 0x1f), e[10] | (e[11] << 8));
	for (d = 54; d <= 108; d += 18) {
		const uint8_t *p = e + d;
		int i;

		if (p[0] || p[1] || p[2] || p[3] != 0xfc)
			continue;
		for (i = 0; i < 13 && p[5 + i] != 0x0a; i++)
			name[i] = (p[5 + i] >= 32 && p[5 + i] < 127) ? (char)p[5 + i] : '?';
		name[i] = '\0';
	}
	if (name[0])
		printf(", \"%s\"", name);
	printf(", %u extension block(s))", e[126]);
}

static void print_props(int fd, uint32_t obj, uint32_t type, const char *indent)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t i;

	if (!props)
		return;
	printf("%sproperties:\n", indent);
	for (i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		uint64_t v = props->prop_values[i];
		int j;

		if (!p)
			continue;
		printf("%s  %-16s [%u]%s%s ", indent, p->name, p->prop_id,
		       (p->flags & DRM_MODE_PROP_IMMUTABLE) ? " immutable" : "",
		       (p->flags & DRM_MODE_PROP_ATOMIC) ? " atomic" : "");
		if (drm_property_type_is(p, DRM_MODE_PROP_RANGE)) {
			printf("range %llu..%llu = %llu",
			       (unsigned long long)(p->count_values > 0 ? p->values[0] : 0),
			       (unsigned long long)(p->count_values > 1 ? p->values[1] : 0),
			       (unsigned long long)v);
		} else if (drm_property_type_is(p, DRM_MODE_PROP_SIGNED_RANGE)) {
			printf("srange %lld..%lld = %lld",
			       (long long)(p->count_values > 0 ? (int64_t)p->values[0] : 0),
			       (long long)(p->count_values > 1 ? (int64_t)p->values[1] : 0),
			       (long long)(int64_t)v);
		} else if (drm_property_type_is(p, DRM_MODE_PROP_ENUM)) {
			printf("enum {");
			for (j = 0; j < p->count_enums; j++)
				printf("%s%s%s", j ? ", " : "", p->enums[j].name,
				       p->enums[j].value == v ? "*" : "");
			printf("}");
		} else if (drm_property_type_is(p, DRM_MODE_PROP_BITMASK)) {
			printf("bitmask {");
			for (j = 0; j < p->count_enums; j++)
				printf("%s%s%s", j ? ", " : "", p->enums[j].name,
				       (v & (1ULL << p->enums[j].value)) ? "*" : "");
			printf("} = 0x%llx", (unsigned long long)v);
		} else if (drm_property_type_is(p, DRM_MODE_PROP_BLOB)) {
			printf("blob %llu", (unsigned long long)v);
			if (v) {
				drmModePropertyBlobRes *b = drmModeGetPropertyBlob(fd, (uint32_t)v);

				if (b) {
					if (strcmp(p->name, "EDID") == 0)
						print_edid_summary(b->data, b->length);
					else
						printf(" (%u bytes)", b->length);
					drmModeFreePropertyBlob(b);
				}
			}
		} else if (drm_property_type_is(p, DRM_MODE_PROP_OBJECT)) {
			printf("object = %llu", (unsigned long long)v);
		} else {
			printf("= %llu", (unsigned long long)v);
		}
		printf("\n");
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
}

static void print_in_formats(int fd, uint32_t plane_id)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane_id,
								    DRM_MODE_OBJECT_PLANE);
	uint32_t i;

	if (!props)
		return;
	for (i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);

		if (p && strcmp(p->name, "IN_FORMATS") == 0 && props->prop_values[i]) {
			drmModePropertyBlobRes *b =
				drmModeGetPropertyBlob(fd, (uint32_t)props->prop_values[i]);

			if (b) {
				drmModeFormatModifierIterator it = { 0 };
				uint32_t last = 0;
				char f[5];

				printf("    IN_FORMATS:");
				while (drmModeFormatModifierBlobIterNext(b, &it)) {
					if (it.fmt != last)
						printf("\n      ");
					last = it.fmt;
					fourcc_str(it.fmt, f);
					printf("%s:0x%llx ", f, (unsigned long long)it.mod);
				}
				printf("\n");
				drmModeFreePropertyBlob(b);
			}
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
}

static int open_any(const char *dev, char *used, size_t usedsz)
{
	int i;

	if (dev) {
		snprintf(used, usedsz, "%s", dev);
		return open(dev, O_RDWR | O_CLOEXEC);
	}
	for (i = 0; i < 8; i++) {
		char path[32];
		int fd;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			drmModeRes *res = drmModeGetResources(fd);

			if (res && res->count_crtcs > 0) {
				drmModeFreeResources(res);
				snprintf(used, usedsz, "%s", path);
				return fd;
			}
			drmModeFreeResources(res);
			close(fd);
		}
	}
	errno = ENODEV;
	return -1;
}

static int do_list(const char *dev)
{
	char path[64];
	int fd = open_any(dev, path, sizeof(path)), i, j;
	drmVersionPtr ver;
	drmModeRes *res;
	drmModePlaneRes *pres;
	uint64_t cap;
	bool atomic;

	if (fd < 0) {
		fprintf(stderr, "rsos-kmstest: no DRM device (%s): %s\n",
			dev ? dev : "/dev/dri/card*", strerror(errno));
		return 1;
	}
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	atomic = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0;

	ver = drmGetVersion(fd);
	printf("device %s: driver %s %d.%d.%d (%s), atomic %s\n", path,
	       ver ? ver->name : "?", ver ? ver->version_major : 0,
	       ver ? ver->version_minor : 0, ver ? ver->version_patchlevel : 0,
	       ver ? ver->desc : "?", atomic ? "yes" : "NO");
	drmFreeVersion(ver);
	{
		static const struct { uint64_t cap; const char *name; } caps[] = {
			{ DRM_CAP_DUMB_BUFFER, "DUMB_BUFFER" },
			{ DRM_CAP_DUMB_PREFERRED_DEPTH, "DUMB_PREFERRED_DEPTH" },
			{ DRM_CAP_PRIME, "PRIME" },
			{ DRM_CAP_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC" },
			{ DRM_CAP_ASYNC_PAGE_FLIP, "ASYNC_PAGE_FLIP" },
			{ DRM_CAP_ADDFB2_MODIFIERS, "ADDFB2_MODIFIERS" },
			{ DRM_CAP_CRTC_IN_VBLANK_EVENT, "CRTC_IN_VBLANK_EVENT" },
		};
		printf("caps:");
		for (i = 0; i < (int)(sizeof(caps) / sizeof(caps[0])); i++) {
			cap = 0;
			if (drmGetCap(fd, caps[i].cap, &cap) == 0)
				printf(" %s=%llu", caps[i].name, (unsigned long long)cap);
		}
		printf("\n");
	}

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "drmModeGetResources: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("framebuffer size %ux%u .. %ux%u\n\n", res->min_width, res->min_height,
	       res->max_width, res->max_height);

	printf("=== %d connector(s)\n", res->count_connectors);
	for (i = 0; i < res->count_connectors; i++) {
		/* Full probe: runs detect and reads the EDID. */
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		const char *tn;

		if (!c)
			continue;
		tn = drmModeGetConnectorTypeName(c->connector_type);
		printf("connector %u: %s-%u (type %u) %s, %ux%u mm, encoder %u, encoders:",
		       c->connector_id, tn ? tn : "Unknown", c->connector_type_id,
		       c->connector_type, conn_status(c->connection), c->mmWidth,
		       c->mmHeight, c->encoder_id);
		for (j = 0; j < c->count_encoders; j++)
			printf(" %u", c->encoders[j]);
		printf("\n  %d mode(s):\n", c->count_modes);
		for (j = 0; j < c->count_modes; j++)
			print_mode(&c->modes[j], "    ");
		print_props(fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "  ");
		drmModeFreeConnector(c);
	}

	printf("\n=== %d encoder(s)\n", res->count_encoders);
	for (i = 0; i < res->count_encoders; i++) {
		drmModeEncoder *e = drmModeGetEncoder(fd, res->encoders[i]);

		if (!e)
			continue;
		printf("encoder %u: type %s, crtc %u, possible_crtcs 0x%x, possible_clones 0x%x\n",
		       e->encoder_id, encoder_type(e->encoder_type), e->crtc_id,
		       e->possible_crtcs, e->possible_clones);
		drmModeFreeEncoder(e);
	}

	printf("\n=== %d CRTC(s)\n", res->count_crtcs);
	for (i = 0; i < res->count_crtcs; i++) {
		drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);

		if (!c)
			continue;
		printf("crtc %u (index %d, mask 0x%x): fb %u, pos %u,%u, gamma %d, mode %s\n",
		       c->crtc_id, i, 1u << i, c->buffer_id, c->x, c->y, c->gamma_size,
		       c->mode_valid ? "" : "(none)");
		if (c->mode_valid)
			print_mode(&c->mode, "    ");
		print_props(fd, c->crtc_id, DRM_MODE_OBJECT_CRTC, "  ");
		drmModeFreeCrtc(c);
	}

	pres = drmModeGetPlaneResources(fd);
	printf("\n=== %u plane(s)\n", pres ? pres->count_planes : 0);
	for (i = 0; pres && i < (int)pres->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
		uint32_t k;
		char f[5];

		if (!p)
			continue;
		printf("plane %u: possible_crtcs 0x%x, crtc %u, fb %u, %u format(s):",
		       p->plane_id, p->possible_crtcs, p->crtc_id, p->fb_id, p->count_formats);
		for (k = 0; k < p->count_formats; k++) {
			fourcc_str(p->formats[k], f);
			printf(" %s", f);
		}
		printf("\n");
		print_in_formats(fd, p->plane_id);
		print_props(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "  ");
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pres);
	drmModeFreeResources(res);
	close(fd);
	return 0;
}

/* ------------------------------------------------------------------ */
/* --monitor                                                           */
/* ------------------------------------------------------------------ */

static int do_monitor(void)
{
	struct uevent *ev = malloc(sizeof(*ev));
	int fd = uevent_open();

	if (fd < 0 || !ev) {
		fprintf(stderr, "rsos-kmstest: uevent socket: %s\n", strerror(-fd));
		free(ev);
		return 1;
	}
	tlog("listening for kernel uevents (Ctrl-C to stop)");
	while (!quit) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		int r;

		if (poll(&pfd, 1, 500) <= 0)
			continue;
		while ((r = uevent_read(fd, ev)) != 0) {
			size_t off;

			if (r < 0) {
				tlog("uevent read error: %s", strerror(-r));
				break;
			}
			tlog("%s %s%s", ev->buf, ev->subsystem ? ev->subsystem : "-",
			     uevent_is_drm_hotplug(ev) ? "  <== DRM HOTPLUG" : "");
			for (off = strlen(ev->buf) + 1; off < ev->len; off += strlen(ev->buf + off) + 1)
				printf("           %s\n", ev->buf + off);
			fflush(stdout);
		}
	}
	uevent_close(fd);
	free(ev);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Test pattern                                                        */
/* ------------------------------------------------------------------ */

struct canvas {
	uint8_t *p;
	int stride, w, h;
	uint32_t fmt;  /* XRGB8888, RGB565 or XRGB1555 */
};

static inline uint32_t to_native(const struct canvas *c, uint32_t rgb)
{
	if (c->fmt == DRM_FORMAT_RGB565)
		return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
	if (c->fmt == DRM_FORMAT_XRGB1555)
		return ((rgb >> 9) & 0x7c00) | ((rgb >> 6) & 0x03e0) | ((rgb >> 3) & 0x001f);
	return rgb;
}

/* Write-only fill (the scanout buffers are write-combined: never read them). */
static void fill(const struct canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
	uint32_t v = to_native(c, rgb);
	int i, j;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > c->w) w = c->w - x;
	if (y + h > c->h) h = c->h - y;
	for (j = 0; j < h; j++) {
		uint8_t *row = c->p + (size_t)(y + j) * c->stride;

		if (c->fmt != DRM_FORMAT_XRGB8888) {
			uint16_t *d = (uint16_t *)(void *)row + x;

			for (i = 0; i < w; i++)
				d[i] = (uint16_t)v;
		} else {
			uint32_t *d = (uint32_t *)(void *)row + x;

			for (i = 0; i < w; i++)
				d[i] = v;
		}
	}
}

static void put(const struct canvas *c, int x, int y, uint32_t v)
{
	uint8_t *row = c->p + (size_t)y * c->stride;

	if (c->fmt != DRM_FORMAT_XRGB8888)
		((uint16_t *)(void *)row)[x] = (uint16_t)v;
	else
		((uint32_t *)(void *)row)[x] = v;
}

/* Text on an opaque background box so it is readable over the bars. */
static void text(const struct canvas *c, int x, int y, const char *s, uint32_t fg,
		 uint32_t bg)
{
	uint32_t f = to_native(c, fg), b = to_native(c, bg);

	for (; *s; s++, x += 8) {
		const uint8_t *g = font8x8_glyph((unsigned char)*s);
		int r, k;

		if (x + 8 > c->w || y + 8 > c->h)
			break;
		for (r = 0; r < 8; r++)
			for (k = 0; k < 8; k++)
				put(c, x + k, y + r, (g[r] & (0x80 >> k)) ? f : b);
	}
}

static void draw_pattern(const struct canvas *c, uint64_t frame, const char *lines[], int nlines)
{
	static const uint32_t bars[8] = {
		0xc0c0c0, 0xc0c000, 0x00c0c0, 0x00c000,
		0xc000c0, 0xc00000, 0x0000c0, 0x202020,
	};
	int i, bw = c->w / 8, x, y;

	/* Color bars. */
	for (i = 0; i < 8; i++)
		fill(c, i * bw, 0, i == 7 ? c->w - 7 * bw : bw, c->h, bars[i]);

	/* Grey ramp along the bottom (banding/filter check). */
	for (x = 0; x < c->w; x++) {
		uint32_t g = (uint32_t)(x * 255 / (c->w - 1));

		fill(c, x, c->h - 24, 1, 12, g << 16 | g << 8 | g);
	}

	/* 1-pixel checkerboard (shows the scaler's filtering). */
	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++)
			put(c, c->w - 40 + x, c->h - 60 + y,
			    to_native(c, ((x ^ y) & 1) ? 0xffffff : 0x000000));

	/* Moving vertical bar: any tearing shows as a horizontal break. */
	x = (int)((frame * 4) % (uint64_t)(c->w + 16)) - 16;
	fill(c, x, 0, 16, c->h, 0xffffff);
	fill(c, x + 6, 0, 4, c->h, 0xff0000);

	/* Frame border: every edge pixel must be visible (no crop, no overscan). */
	fill(c, 0, 0, c->w, 1, 0xffffff);
	fill(c, 0, c->h - 1, c->w, 1, 0xffffff);
	fill(c, 0, 0, 1, c->h, 0xffffff);
	fill(c, c->w - 1, 0, 1, c->h, 0xffffff);

	for (i = 0; i < nlines; i++)
		text(c, 4, 4 + i * 10, lines[i], 0xffffff, 0x000000);
}

/* ------------------------------------------------------------------ */
/* Demo                                                                */
/* ------------------------------------------------------------------ */

static const char *reason_name(enum display_event_reason r)
{
	switch (r) {
	case DISPLAY_EVENT_INIT: return "init";
	case DISPLAY_EVENT_HOTPLUG: return "hotplug";
	case DISPLAY_EVENT_REPROBE: return "reprobe";
	case DISPLAY_EVENT_FALLBACK: return "fallback";
	}
	return "?";
}

static void on_output(const struct display_output_info *now,
		      const struct display_output_info *before,
		      enum display_event_reason why,
		      const struct display_switch_timing *tm, void *user)
{
	(void)user;
	tlog("OUTPUT %s: %s -> %s %s %s (%s%s%s%s)", reason_name(why),
	     before->type == DISPLAY_OUTPUT_NONE ? "none" : before->name,
	     now->type == DISPLAY_OUTPUT_HDMI ? "HDMI" : "LCD", now->name, now->mode_name,
	     now->has_edid ? "EDID" : "no EDID", now->monitor[0] ? " \"" : "",
	     now->monitor, now->monitor[0] ? "\"" : "");
	if (tm->t_uevent)
		tlog("LATENCY first uevent -> modeset done: %lld ms (debounce+probe %lld, commit %lld)",
		     (long long)(tm->t_commit - tm->t_uevent),
		     (long long)(tm->t_probe - tm->t_uevent),
		     (long long)(tm->t_commit - tm->t_probe));
}

static void on_audio(enum display_audio_phase phase, const struct display_output_info *out,
		     void *user)
{
	(void)user;
	/* The audio layer closes/opens its ALSA PCM here (HDMI card vs codec). */
	if (phase == DISPLAY_AUDIO_RELEASE)
		tlog("AUDIO release: close the PCM on %s", out->type == DISPLAY_OUTPUT_HDMI ?
		     "HDMI (sun4i-hdmi card)" : "the internal codec");
	else
		tlog("AUDIO acquire: open the PCM on %s", out->type == DISPLAY_OUTPUT_HDMI ?
		     "HDMI (sun4i-hdmi card)" : "the internal codec");
}

static void usage(const char *argv0)
{
	printf("usage: %s [options]\n"
	       "  --list              dump connectors, encoders, CRTCs, planes and exit\n"
	       "  --monitor           print raw kernel uevents and exit on Ctrl-C\n"
	       "  --no-scale          software fallback path only (no plane scaling)\n"
	       "  --soft-scale N      fallback path: max CPU integer factor (default 2, 1 = unscaled)\n"
	       "  --mode WxH[@HZ]     force this mode on any output that offers it\n"
	       "  --hdmi WxH[@HZ]     preferred HDMI mode (default 1280x720@60; 640x480 = native 4:3)\n"
	       "  --size WxH          game surface size (default 320x240)\n"
	       "  --format F          xrgb8888 (default), rgb565 or xrgb1555 (libretro 0RGB1555)\n"
	       "  --copy              draw into a private buffer and present it with\n"
	       "                      display_present_frame(), like a libretro core\n"
	       "  --resize            cycle frame sizes (320x240, 256x224, 368x240, 640x480)\n"
	       "                      inside a 640x480 surface, like PSX resolution changes\n"
	       "  --fb-test           present two external XRGB8888 framebuffers with\n"
	       "                      display_present_fb() (the GL/GBM zero-copy path)\n"
	       "  --suspend-test      every 600 frames: display_suspend(), run a child\n"
	       "                      rsos-kmstest for 240 frames, display_resume()\n"
	       "  --active-test       every 600 frames: screen off for 2 s, then on\n"
	       "  --scale M           aspect (default), integer or stretch\n"
	       "  --buffers N         2 (default) or 3\n"
	       "  --hdmi-crtc N       force the CRTC index used for HDMI\n"
	       "  --lcd-refresh HZ    LCD at HZ (60: the 25.2 MHz user mode; 0: the panel's own mode)\n"
	       "  --tv NORM           the composite output is the built-in screen (RetroStone1):\n"
	       "                      ntsc (720x480i), pal (720x576i) or auto (the kernel's choice)\n"
	       "  --debounce MS       hotplug debounce (default 250)\n"
	       "  --frames N          exit after N frames\n"
	       "  --device PATH       DRM device (default: first usable /dev/dri/card*)\n"
	       "  --verbose           debug logs\n", argv0);
}

static bool parse_wxh(const char *s, int *w, int *h, int *hz)
{
	int n = 0;

	if (hz)
		*hz = 0;
	if (sscanf(s, "%dx%d%n", w, h, &n) != 2 || *w <= 0 || *h <= 0)
		return false;
	if (s[n] == '@' && hz)
		*hz = atoi(s + n + 1);
	return true;
}

/*
 * --fb-test: external framebuffers for display_present_fb(). Dumb buffers
 * stand in for the GBM BOs the GL host will use: same API, same plane path.
 */
struct test_fb {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	uint8_t *map;
	bool busy;          /* owned by the display until release() */
};

static void test_fb_release(uint32_t fb_id, void *user)
{
	struct test_fb *t = user;

	(void)fb_id;
	t->busy = false;
}

static int test_fb_create(int fd, struct test_fb *t, int w, int h)
{
	struct drm_mode_create_dumb cd = { 0 };
	struct drm_mode_map_dumb md = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	memset(t, 0, sizeof(*t));
	cd.width = (uint32_t)w;
	cd.height = (uint32_t)h;
	cd.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return -errno;
	t->handle = cd.handle;
	t->pitch = cd.pitch;
	t->size = cd.size;
	handles[0] = cd.handle;
	pitches[0] = cd.pitch;
	if (drmModeAddFB2(fd, (uint32_t)w, (uint32_t)h, DRM_FORMAT_XRGB8888, handles, pitches,
			  offsets, &t->fb_id, 0))
		return -errno;
	md.handle = cd.handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		return -errno;
	t->map = mmap(NULL, t->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
	if (t->map == MAP_FAILED) {
		t->map = NULL;
		return -errno;
	}
	return 0;
}

static void test_fb_destroy(int fd, struct test_fb *t)
{
	struct drm_mode_destroy_dumb d = { .handle = t->handle };

	if (t->map)
		munmap(t->map, t->size);
	if (t->fb_id)
		drmModeRmFB(fd, t->fb_id);
	if (t->handle)
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	memset(t, 0, sizeof(*t));
}

/* --suspend-test: hand the display to a child kmstest, like a game launch. */
static void suspend_cycle(const char *device)
{
	int64_t t0 = display_now_ms(), t1, t2;
	int status = 0, ret;
	pid_t pid;

	tlog("SUSPEND: dropping DRM master, starting a child kmstest");
	ret = display_suspend();
	if (ret < 0) {
		tlog("SUSPEND failed: %s", strerror(-ret));
		return;
	}
	t1 = display_now_ms();
	pid = fork();
	if (pid == 0) {
		if (device)
			execl("/proc/self/exe", "rsos-kmstest", "--frames", "240", "--device",
			      device, (char *)NULL);
		else
			execl("/proc/self/exe", "rsos-kmstest", "--frames", "240", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	t2 = display_now_ms();
	ret = display_resume();
	tlog("RESUME %s: suspend %lld ms, child %lld ms (exit %d), resume %lld ms",
	     ret ? strerror(-ret) : "ok", (long long)(t1 - t0), (long long)(t2 - t1),
	     WIFEXITED(status) ? WEXITSTATUS(status) : -1, (long long)(display_now_ms() - t2));
}

/* --active-test: screen off for 2 s (events still serviced), then on. */
static void active_cycle(void)
{
	int64_t end;
	int ret = display_set_active(false);

	tlog("SCREEN OFF: %s", ret ? strerror(-ret) : "ok");
	end = display_now_ms() + 2000;
	while (!quit && display_now_ms() < end)
		display_wait_events((int)(end - display_now_ms()));
	ret = display_set_active(true);
	tlog("SCREEN ON: %s", ret ? strerror(-ret) : "ok");
}

static const char *fmt_name(uint32_t f)
{
	switch (f) {
	case DRM_FORMAT_XRGB8888: return "XRGB8888";
	case DRM_FORMAT_ARGB8888: return "ARGB8888";
	case DRM_FORMAT_RGB565: return "RGB565";
	case DRM_FORMAT_XRGB1555: return "XRGB1555";
	default: return "?";
	}
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "list", no_argument, NULL, 'l' },
		{ "monitor", no_argument, NULL, 'u' },
		{ "no-scale", no_argument, NULL, 'n' },
		{ "soft-scale", required_argument, NULL, 'k' },
		{ "mode", required_argument, NULL, 'm' },
		{ "hdmi", required_argument, NULL, 'H' },
		{ "size", required_argument, NULL, 's' },
		{ "format", required_argument, NULL, 'f' },
		{ "copy", no_argument, NULL, 'C' },
		{ "resize", no_argument, NULL, 'R' },
		{ "fb-test", no_argument, NULL, 'B' },
		{ "suspend-test", no_argument, NULL, 'P' },
		{ "active-test", no_argument, NULL, 'A' },
		{ "scale", required_argument, NULL, 'S' },
		{ "buffers", required_argument, NULL, 'b' },
		{ "hdmi-crtc", required_argument, NULL, 'c' },
		{ "lcd-refresh", required_argument, NULL, 'L' },
		{ "tv", required_argument, NULL, 'T' },
		{ "debounce", required_argument, NULL, 'd' },
		{ "frames", required_argument, NULL, 'F' },
		{ "device", required_argument, NULL, 'D' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	static const int sizes[][2] = { { 320, 240 }, { 256, 224 }, { 368, 240 }, { 640, 480 } };
	struct display_config cfg;
	const struct display_surface *s;
	struct display_stats st, st0;
	int sw = 320, sh = 240, hz, opt, fw, fh;
	uint32_t fmt = DRM_FORMAT_XRGB8888;
	bool list = false, monitor = false, copy = false, resize = false;
	bool fbtest = false, suspend_test = false, active_test = false;
	struct test_fb tfb[2];
	long long max_frames = -1;
	uint64_t frame = 0;
	int64_t t_stat;
	double fps = 0.0;
	uint8_t *core = NULL;
	int core_stride = 0;
	struct sigaction sa;

	display_config_defaults(&cfg);
	while ((opt = getopt_long(argc, argv, "lunk:m:H:s:f:CRBPAS:b:c:L:T:d:F:D:vh", opts, NULL)) != -1) {
		switch (opt) {
		case 'l': list = true; break;
		case 'u': monitor = true; break;
		case 'n': cfg.no_hw_scale = true; break;
		case 'k': cfg.soft_scale_max = atoi(optarg); break;
		case 'm':
			if (!parse_wxh(optarg, &cfg.force_width, &cfg.force_height, &hz))
				goto bad;
			if (hz)
				cfg.hdmi_refresh = hz;
			break;
		case 'H':
			if (!parse_wxh(optarg, &cfg.hdmi_width, &cfg.hdmi_height, &hz))
				goto bad;
			if (hz)
				cfg.hdmi_refresh = hz;
			break;
		case 's':
			if (!parse_wxh(optarg, &sw, &sh, NULL))
				goto bad;
			break;
		case 'f':
			if (strcmp(optarg, "xrgb8888") == 0)
				fmt = DRM_FORMAT_XRGB8888;
			else if (strcmp(optarg, "rgb565") == 0)
				fmt = DRM_FORMAT_RGB565;
			else if (strcmp(optarg, "xrgb1555") == 0 || strcmp(optarg, "0rgb1555") == 0)
				fmt = DRM_FORMAT_XRGB1555;
			else
				goto bad;
			break;
		case 'C': copy = true; break;
		case 'R': resize = true; break;
		case 'B': fbtest = true; break;
		case 'P': suspend_test = true; break;
		case 'A': active_test = true; break;
		case 'S':
			if (strcmp(optarg, "aspect") == 0)
				cfg.scale = DISPLAY_SCALE_ASPECT;
			else if (strcmp(optarg, "integer") == 0)
				cfg.scale = DISPLAY_SCALE_INTEGER;
			else if (strcmp(optarg, "stretch") == 0)
				cfg.scale = DISPLAY_SCALE_STRETCH;
			else
				goto bad;
			break;
		case 'b': cfg.buffers = atoi(optarg); break;
		case 'c': cfg.hdmi_crtc_index = atoi(optarg); break;
		case 'L': cfg.lcd_refresh_hz = atoi(optarg); break;
		case 'T':
			/* The composite output is the built-in screen (RetroStone1),
			 * next to the usual panel types */
			if (strcmp(optarg, "ntsc") == 0)
				cfg.tv_norm = DISPLAY_TV_NTSC;
			else if (strcmp(optarg, "pal") == 0)
				cfg.tv_norm = DISPLAY_TV_PAL;
			else if (strcmp(optarg, "auto") == 0)
				cfg.tv_norm = DISPLAY_TV_AUTO;
			else
				goto bad;
			cfg.internal_mode = DISPLAY_INTERNAL_LIST;
			cfg.internal_types = (1u << DRM_MODE_CONNECTOR_Composite) | (1u << DRM_MODE_CONNECTOR_Unknown) |
					     (1u << DRM_MODE_CONNECTOR_DPI) | (1u << DRM_MODE_CONNECTOR_LVDS) |
					     (1u << DRM_MODE_CONNECTOR_DSI);
			break;
		case 'd': cfg.debounce_ms = atoi(optarg); break;
		case 'F': max_frames = atoll(optarg); break;
		case 'D': cfg.device = optarg; break;
		case 'v': cfg.log_level = DISPLAY_LOG_DEBUG; break;
		case 'h': usage(argv[0]); return 0;
		default:
			goto bad;
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal; /* no SA_RESTART: poll() returns EINTR */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (list)
		return do_list(cfg.device);
	if (monitor)
		return do_monitor();

	if (fbtest) {
		/* External FBs are XRGB8888 GBM-style buffers drawn directly. */
		fmt = DRM_FORMAT_XRGB8888;
		copy = false;
		resize = false;
	}
	if (resize) {
		if (sw < 640)
			sw = 640;
		if (sh < 480)
			sh = 480;
	}
	if (copy) {
		/* The "core" renders into its own cached buffer, as libretro cores do. */
		core_stride = sw * (fmt == DRM_FORMAT_XRGB8888 ? 4 : 2);
		core = calloc((size_t)sh, (size_t)core_stride);
		if (!core)
			return 1;
	}

	cfg.on_output = on_output;
	cfg.on_audio = on_audio;
	tlog("rsos-kmstest starting");
	if (display_init(&cfg) < 0) {
		fprintf(stderr, "rsos-kmstest: display_init failed (try --list)\n");
		free(core);
		return 1;
	}
	memset(tfb, 0, sizeof(tfb));
	if (fbtest) {
		int k, ret = 0;

		for (k = 0; k < 2 && ret == 0; k++)
			ret = test_fb_create(display_get_drm_fd(), &tfb[k], sw, sh);
		if (ret) {
			fprintf(stderr, "rsos-kmstest: framebuffer: %s\n", strerror(-ret));
			for (k = 0; k < 2; k++)
				test_fb_destroy(display_get_drm_fd(), &tfb[k]);
			display_shutdown();
			return 1;
		}
		s = NULL;
		tlog("fb-test: 2 external XRGB8888 framebuffers %dx%d (%u, %u), display_present_fb()",
		     sw, sh, tfb[0].fb_id, tfb[1].fb_id);
	} else {
		s = display_set_game_surface(sw, sh, fmt, 0);
		if (!s) {
			fprintf(stderr, "rsos-kmstest: cannot create a %dx%d surface\n", sw, sh);
			display_shutdown();
			free(core);
			return 1;
		}
		tlog("surface %dx%d %s, %s, plane %s, %d buffer(s)", s->max_width, s->max_height,
		     fmt_name(fmt), s->hw_scaled ? "hardware scaled" : "software blit",
		     fmt_name(s->plane_format), s->count);
	}

	display_get_stats(&st0);
	t_stat = display_now_ms();
	fw = resize ? sizes[0][0] : sw;
	fh = resize ? sizes[0][1] : sh;
	while (!quit && (max_frames < 0 || (long long)frame < max_frames)) {
		const struct display_output_info *o;
		char l0[96], l1[96], l2[96], l3[96], l4[96];
		const char *lines[5] = { l0, l1, l2, l3, l4 };
		struct canvas c;
		int i = 0, k = 0;

		if (suspend_test && frame % 600 == 300)
			suspend_cycle(cfg.device);
		if (active_test && frame % 600 == 150)
			active_cycle();
		if (resize && frame % 180 == 0) {
			fw = sizes[(frame / 180) % 4][0];
			fh = sizes[(frame / 180) % 4][1];
			tlog("frame size %dx%d", fw, fh);
		}
		if (fbtest) {
			/* Wait until the display hands one framebuffer back. */
			while (!quit && tfb[0].busy && tfb[1].busy)
				display_wait_events(100);
			if (quit)
				break;
			k = tfb[0].busy ? 1 : 0;
		} else if (!copy) {
			/* Zero-copy style: draw straight into the display's buffer. */
			if (resize && display_set_frame_size(fw, fh) < 0)
				tlog("display_set_frame_size(%d, %d) failed", fw, fh);
			i = display_begin_frame(200);
			if (i < 0) {
				if (i != -ETIMEDOUT && i != -EINTR && i != -EAGAIN) {
					fprintf(stderr, "begin_frame: %s\n", strerror(-i));
					break;
				}
				continue;
			}
		}
		s = display_get_surface();
		o = display_output();
		if (fbtest) {
			c.p = tfb[k].map;
			c.stride = (int)tfb[k].pitch;
			c.w = sw;
			c.h = sh;
		} else if (copy) {
			c.p = core;
			c.stride = core_stride;
			c.w = fw;
			c.h = fh;
		} else {
			c.p = s->buffers[i].pixels;
			c.stride = s->buffers[i].stride;
			c.w = s->width;
			c.h = s->height;
		}
		c.fmt = fmt == DRM_FORMAT_ARGB8888 ? DRM_FORMAT_XRGB8888 : fmt;

		display_get_stats(&st);
		snprintf(l0, sizeof(l0), "RetroStoneOS kmstest");
		snprintf(l1, sizeof(l1), "%s %s %s", o->type == DISPLAY_OUTPUT_HDMI ? "HDMI" : "LCD",
			 o->name, o->mode_name);
		if (fbtest)
			snprintf(l2, sizeof(l2), "EXT FB %u %dx%d ZERO-COPY", tfb[k].fb_id, sw, sh);
		else if (s->hw_scaled)
			snprintf(l2, sizeof(l2), "HW %s %dx%d>%dx%d", fmt_name(s->plane_format),
				 c.w, c.h, s->dst.w, s->dst.h);
		else
			snprintf(l2, sizeof(l2), "SW BLIT x%d %dx%d", s->soft_scale, c.w, c.h);
		snprintf(l3, sizeof(l3), "FPS %.1f DROP %llu SW %llu", fps,
			 (unsigned long long)st.dropped, (unsigned long long)st.switches);
		snprintf(l4, sizeof(l4), "%s %s %llu %s", fmt_name(fmt),
			 fbtest ? "FB" : copy ? "COPY" : "DIRECT", (unsigned long long)frame,
			 o->monitor);
		draw_pattern(&c, frame, lines, 5);
		if (fbtest) {
			struct display_fb dfb;

			memset(&dfb, 0, sizeof(dfb));
			dfb.fb_id = tfb[k].fb_id;
			dfb.format = DRM_FORMAT_XRGB8888;
			dfb.width = sw;
			dfb.height = sh;
			dfb.src = (struct display_rect){ 0, 0, sw, sh };
			dfb.release = test_fb_release;
			dfb.user = &tfb[k];
			tfb[k].busy = true;
			i = display_present_fb(&dfb, 200);
			if (i < 0) {
				tfb[k].busy = false;   /* not taken: still ours */
				if (i == -EINVAL) {
					fprintf(stderr, "present_fb: the plane refuses this framebuffer\n");
					break;
				}
			}
		} else if (copy) {
			i = display_present_frame(core, fw, fh, core_stride);
			if (i < 0 && i != -ETIMEDOUT && i != -EINTR && i != -EAGAIN) {
				fprintf(stderr, "present_frame: %s\n", strerror(-i));
				break;
			}
		} else {
			display_present();
		}
		frame++;

		/* Catch hotplug even when no wait was needed (triple buffering). */
		display_handle_events();

		if (display_now_ms() - t_stat >= 5000) {
			int64_t now = display_now_ms();

			display_get_stats(&st);
			fps = (double)(st.flips - st0.flips) * 1000.0 / (double)(now - t_stat);
			tlog("stats: %.2f flips/s, dropped %llu, switches %llu", fps,
			     (unsigned long long)(st.dropped - st0.dropped),
			     (unsigned long long)st.switches);
			st0 = st;
			t_stat = now;
		}
	}
	tlog("exiting after %llu frames", (unsigned long long)frame);
	/* Test FBs go before the display closes its fd (the plane is disabled). */
	test_fb_destroy(display_get_drm_fd(), &tfb[0]);
	test_fb_destroy(display_get_drm_fd(), &tfb[1]);
	display_shutdown();
	free(core);
	return 0;

bad:
	usage(argv[0]);
	return 2;
}
