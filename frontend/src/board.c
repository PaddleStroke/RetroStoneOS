/*
 * board.c - see board.h.
 */
#include "board.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* DRM_MODE_CONNECTOR_* (uapi drm_mode.h; stable numbers). */
enum {
	CONN_UNKNOWN = 0, CONN_VGA = 1, CONN_DVII = 2, CONN_DVID = 3, CONN_DVIA = 4,
	CONN_COMPOSITE = 5, CONN_SVIDEO = 6, CONN_LVDS = 7, CONN_COMPONENT = 8,
	CONN_9PINDIN = 9, CONN_DISPLAYPORT = 10, CONN_HDMIA = 11, CONN_HDMIB = 12,
	CONN_TV = 13, CONN_EDP = 14, CONN_VIRTUAL = 15, CONN_DSI = 16, CONN_DPI = 17,
	CONN_WRITEBACK = 18, CONN_SPI = 19, CONN_USB = 20,
};

static const struct {
	const char *name;
	int type;
} g_conn_names[] = {
	{ "unknown", CONN_UNKNOWN }, { "dpi", CONN_DPI }, { "lvds", CONN_LVDS },
	{ "dsi", CONN_DSI }, { "edp", CONN_EDP }, { "virtual", CONN_VIRTUAL },
	{ "spi", CONN_SPI }, { "usb", CONN_USB }, { "composite", CONN_COMPOSITE },
	{ "svideo", CONN_SVIDEO }, { "tv", CONN_TV }, { "vga", CONN_VGA },
	{ "hdmi", CONN_HDMIA }, { "displayport", CONN_DISPLAYPORT },
};

static void copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src ? src : "");
}

void board_defaults(struct board_profile *b)
{
	memset(b, 0, sizeof(*b));
	copy(b->name, sizeof(b->name), "Generic");
	b->internal_display = BOARD_INTERNAL_AUTO;
	copy(b->cpu_governor_menu, sizeof(b->cpu_governor_menu), "schedutil");
	copy(b->cpu_governor_game, sizeof(b->cpu_governor_game), "performance");
	snprintf(b->builtin_pad_name, sizeof(b->builtin_pad_name), "%s built-in", b->name);
}

static char *trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

/* "auto" (any case) and "" both mean auto-detect: stored as "". */
static void set_name(char *dst, size_t n, const char *v)
{
	if (!strcasecmp(v, "auto"))
		v = "";
	copy(dst, n, v);
}

static void parse_internal(struct board_profile *b, const char *v)
{
	char buf[256], *tok, *save = NULL;

	b->internal_types = 0;
	if (!*v || !strcasecmp(v, "auto")) {
		b->internal_display = BOARD_INTERNAL_AUTO;
		return;
	}
	if (!strcasecmp(v, "none")) {
		b->internal_display = BOARD_INTERNAL_NONE;
		return;
	}
	copy(buf, sizeof(buf), v);
	for (tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
		for (size_t i = 0; i < sizeof(g_conn_names) / sizeof(g_conn_names[0]); i++)
			if (!strcasecmp(tok, g_conn_names[i].name))
				b->internal_types |= 1u << g_conn_names[i].type;
	}
	b->internal_display = b->internal_types ? BOARD_INTERNAL_LIST : BOARD_INTERNAL_NONE;
}

/* tv_norm: ntsc (also the default for an unknown value), pal, auto */
static void parse_tv_norm(struct board_profile *b, const char *v)
{
	if (!strcasecmp(v, "pal"))
		b->tv_norm = BOARD_TV_PAL;
	else if (!strcasecmp(v, "auto"))
		b->tv_norm = BOARD_TV_AUTO;
	else
		b->tv_norm = BOARD_TV_NTSC;
}

static void parse_refresh(struct board_profile *b, const char *v)
{
	char buf[128], *tok, *save = NULL;
	int n = 0;

	b->refresh_native = 0;
	b->refresh_60 = false;
	copy(buf, sizeof(buf), v);
	for (tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
		int hz = atoi(tok);

		if (hz <= 0)
			continue;
		if (n++ == 0)
			b->refresh_native = hz;
		else if (hz == 60)
			b->refresh_60 = true;
	}
}

static void parse_quirks(struct board_profile *b, const char *v)
{
	char buf[256], *tok, *save = NULL;

	b->display_quirks = 0;
	copy(buf, sizeof(buf), v);
	for (tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save))
		if (!strcasecmp(tok, "sun4i-tcon0-clock"))
			b->display_quirks |= BOARD_QUIRK_SUN4I_TCON0_CLOCK;
		else if (!strcasecmp(tok, "panel-keep-scanning"))
			b->display_quirks |= BOARD_QUIRK_PANEL_KEEP_SCANNING;
}

static void set_key(struct board_profile *b, const char *k, const char *v)
{
#define STR(key, field) \
	else if (!strcmp(k, key)) copy(b->field, sizeof(b->field), v)
#define NAME(key, field) \
	else if (!strcmp(k, key)) set_name(b->field, sizeof(b->field), v)
	if (!strcmp(k, "name"))
		copy(b->name, sizeof(b->name), *v ? v : "Generic");
	STR("builtin_pad_prefix", builtin_pad_prefix);
	STR("builtin_stick", builtin_stick);
	else if (!strcmp(k, "internal_display"))
		parse_internal(b, v);
	else if (!strcmp(k, "internal_refresh_options"))
		parse_refresh(b, v);
	else if (!strcmp(k, "tv_norm"))
		parse_tv_norm(b, v);
	else if (!strcmp(k, "tv_overscan"))
		b->tv_overscan = atoi(v) < 0 ? 0 : atoi(v) > 20 ? 20 : atoi(v);
	NAME("backlight", backlight);
	NAME("battery_supply", battery_supply);
	NAME("ac_supply", ac_supply);
	NAME("usb_supply", usb_supply);
	NAME("thermal_zone", thermal_zone);
	NAME("power_key_device", power_key_device);
	else if (!strcmp(k, "pek_startup_ms"))
		b->pek_startup_ms = atoi(v) > 0 ? atoi(v) : 0;
	else if (!strcmp(k, "battery_voff_mv"))
		b->battery_voff_mv = atoi(v) > 0 ? atoi(v) : 0;
	NAME("audio_internal", audio_internal);
	NAME("audio_hdmi", audio_hdmi);
	STR("audio_hdmi_pcm", audio_hdmi_pcm);
	else if (!strcmp(k, "cpu_governor_menu") && *v)
		copy(b->cpu_governor_menu, sizeof(b->cpu_governor_menu), v);
	else if (!strcmp(k, "cpu_governor_game") && *v)
		copy(b->cpu_governor_game, sizeof(b->cpu_governor_game), v);
	STR("storage_overlays", storage_overlays);
	else if (!strcmp(k, "display_quirks"))
		parse_quirks(b, v);
#undef STR
#undef NAME
}

void board_parse(struct board_profile *b, const char *text)
{
	const char *p = text;

	while (p && *p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char line[512], *s, *eq, *c;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = 0;
		p = nl ? nl + 1 : NULL;

		s = trim(line);
		if (!*s || *s == '#' || *s == ';' || *s == '[')
			continue;
		eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = 0;
		/* an inline comment after the value: " # ..." */
		for (c = eq + 1; *c; c++)
			if ((*c == '#' || *c == ';') && c > eq + 1 && isspace((unsigned char)c[-1])) {
				*c = 0;
				break;
			}
		{
			char *v = trim(eq + 1);
			size_t vl = strlen(v);

			if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
				v[vl - 1] = 0;
				v++;
			}
			set_key(b, trim(s), v);
		}
	}
	snprintf(b->builtin_pad_name, sizeof(b->builtin_pad_name), "%s built-in", b->name);
	/* ALSA card ids -> PCM names (plughw: the host converts nothing itself) */
	b->audio_internal_dev[0] = b->audio_hdmi_dev[0] = 0;
	if (b->audio_internal[0])
		snprintf(b->audio_internal_dev, sizeof(b->audio_internal_dev), "plughw:CARD=%s,0",
			 b->audio_internal);
	if (b->audio_hdmi[0] && b->audio_hdmi_pcm[0] && strcmp(b->audio_hdmi_pcm, "plughw"))
		snprintf(b->audio_hdmi_dev, sizeof(b->audio_hdmi_dev), "%s:CARD=%s,DEV=0", b->audio_hdmi_pcm,
			 b->audio_hdmi);
	else if (b->audio_hdmi[0])
		snprintf(b->audio_hdmi_dev, sizeof(b->audio_hdmi_dev), "plughw:CARD=%s,0", b->audio_hdmi);
}

int board_load(struct board_profile *b, const char *path)
{
	char buf[8192];
	size_t n;
	FILE *f;

	board_defaults(b);
	if (!path)
		path = getenv("RSOS_BOARD_INI");
	if (!path || !*path)
		path = BOARD_INI_DEFAULT;
	copy(b->path, sizeof(b->path), path);
	f = fopen(path, "re");
	if (!f)
		return -errno;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	board_parse(b, buf);
	b->loaded = true;
	return 0;
}

static struct board_profile g_board;
static bool g_board_ready;

const struct board_profile *board_get(void)
{
	if (!g_board_ready) {
		board_load(&g_board, NULL);
		g_board_ready = true;
	}
	return &g_board;
}

const struct board_profile *board_init(const char *path)
{
	board_load(&g_board, path);
	g_board_ready = true;
	if (path)
		setenv("RSOS_BOARD_INI", path, 1);
	return &g_board;
}

const char *board_name_or_auto(const char *v)
{
	if (!v || !*v)
		return NULL;
	if (!strcasecmp(v, "none"))
		return "";
	return v;
}

static bool type_is_external(uint32_t t)
{
	switch (t) {
	case CONN_HDMIA: case CONN_HDMIB: case CONN_DVII: case CONN_DVID: case CONN_DVIA:
	case CONN_DISPLAYPORT: case CONN_VGA: case CONN_TV: case CONN_COMPOSITE:
	case CONN_SVIDEO: case CONN_COMPONENT:
		return true;
	default:
		return false;
	}
}

bool board_connector_is_internal(const struct board_profile *b, uint32_t t)
{
	switch (b->internal_display) {
	case BOARD_INTERNAL_NONE:
		return false;
	case BOARD_INTERNAL_LIST:
		return t < 32 && (b->internal_types & (1u << t));
	case BOARD_INTERNAL_AUTO:
	default:
		return !type_is_external(t);
	}
}

bool board_has_storage_overlay(const struct board_profile *b, const char *name)
{
	char buf[BOARD_STR], *tok, *save = NULL;

	copy(buf, sizeof(buf), b->storage_overlays);
	for (tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save))
		if (!strcmp(tok, name))
			return true;
	return false;
}

void board_describe(const struct board_profile *b, char *out, int n)
{
	snprintf(out, (size_t)n, "board %s (%s%s)", b->name, b->path,
		 b->loaded ? "" : ": not found, generic defaults");
}
