/*
 * policy.h - which output (built-in panel or HDMI), which CRTC and which
 * mode a RetroStoneOS process uses. Pure logic: no DRM, no I/O. It mirrors
 * pick_mode() and choose_crtc() in frontend/src/display.c, so the libretro
 * host and the native ports (SDL2 patch, ports/) take the same decision.
 */
#ifndef RSOS_FOLLOW_POLICY_H
#define RSOS_FOLLOW_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define PF_MAX_MODES 64
#define PF_MAX_CONNS 8

enum pf_kind { PF_KIND_LCD = 0, PF_KIND_HDMI = 1 };

struct pf_mode {
	int w, h, hz;
	bool preferred;
};

struct pf_conn {
	enum pf_kind kind;
	bool connected;
	bool has_edid;
	uint32_t possible_crtcs;   /* bit i = CRTC i can drive this connector */
	int nmodes;
	struct pf_mode modes[PF_MAX_MODES];
};

struct pf_cfg {
	int hdmi_w, hdmi_h, hdmi_hz;   /* policy mode, default 1280x720@60 */
	bool hdmi_builtin_mode;        /* no EDID: use CEA 720p60 */
	int hdmi_crtc_index;           /* preferred CRTC for HDMI (default 1) */
	bool panel_keep_scanning;      /* the panel keeps its CRTC for good */
	int crtc_count;
};

struct pf_choice {
	int conn;        /* index into conns, -1 = none usable */
	int crtc;        /* -1 = none */
	int w, h, hz;
	bool builtin;    /* the CEA timing, not an EDID mode */
};

/* Decide the output. want_hdmi = the user's choice when HDMI is connected
 * (always true in the default policy: HDMI when plugged in). */
struct pf_choice pf_choose(const struct pf_cfg *cfg, const struct pf_conn *conns, int nconns);

/* The mode pf_choose() would give one connector (no CRTC check). */
bool pf_pick_mode(const struct pf_cfg *cfg, const struct pf_conn *c, struct pf_mode *out, bool *builtin);

#endif
