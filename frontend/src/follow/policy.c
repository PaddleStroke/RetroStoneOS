/*
 * policy.c - output decision, see policy.h. Same rules as display.c:
 *   - HDMI when it is connected and has a usable mode, else the built-in panel;
 *   - HDMI mode: the configured size and refresh, else the preferred one,
 *     else the first; with no EDID the CEA 720p60 timing (builtin);
 *   - panel_keep_scanning: the panel keeps CRTC 0 (the first LCD's CRTC),
 *     HDMI takes hdmi_crtc_index (or the lowest free one), never the panel's.
 */
#include <stddef.h>

#include "policy.h"

static const struct pf_mode cea_720p60 = { 1280, 720, 60, false };

static const struct pf_mode *find_mode(const struct pf_conn *c, int w, int h, int hz)
{
	for (int i = 0; i < c->nmodes; i++)
		if (c->modes[i].w == w && c->modes[i].h == h && c->modes[i].hz == hz)
			return &c->modes[i];
	for (int i = 0; i < c->nmodes; i++)
		if (c->modes[i].w == w && c->modes[i].h == h)
			return &c->modes[i];
	return NULL;
}

bool pf_pick_mode(const struct pf_cfg *cfg, const struct pf_conn *c, struct pf_mode *out, bool *builtin)
{
	const struct pf_mode *m = NULL;

	*builtin = false;
	if (c->kind == PF_KIND_HDMI) {
		if ((!c->has_edid || c->nmodes == 0) && cfg->hdmi_builtin_mode) {
			*out = cea_720p60;
			*builtin = true;
			return true;
		}
		if (cfg->hdmi_w > 0 && cfg->hdmi_h > 0)
			m = find_mode(c, cfg->hdmi_w, cfg->hdmi_h, cfg->hdmi_hz > 0 ? cfg->hdmi_hz : 60);
	}
	for (int i = 0; !m && i < c->nmodes; i++)
		if (c->modes[i].preferred)
			m = &c->modes[i];
	if (!m && c->nmodes > 0)
		m = &c->modes[0];
	if (!m)
		return false;
	*out = *m;
	return true;
}

static bool crtc_ok(const struct pf_conn *c, const struct pf_cfg *cfg, int i)
{
	return i >= 0 && i < cfg->crtc_count && (c->possible_crtcs & (1u << i));
}

/* First CRTC that conn can use, lowest index first. */
static int first_crtc(const struct pf_conn *c, const struct pf_cfg *cfg, int avoid)
{
	for (int i = 0; i < cfg->crtc_count; i++)
		if (i != avoid && crtc_ok(c, cfg, i))
			return i;
	return -1;
}

struct pf_choice pf_choose(const struct pf_cfg *cfg, const struct pf_conn *conns, int nconns)
{
	struct pf_choice ch = { .conn = -1, .crtc = -1 };
	int lcd = -1, hdmi = -1, panel_crtc = -1;
	struct pf_mode m;
	bool builtin;

	for (int i = 0; i < nconns && i < PF_MAX_CONNS; i++) {
		if (!conns[i].connected)
			continue;
		if (conns[i].kind == PF_KIND_LCD && lcd < 0)
			lcd = i;
		if (conns[i].kind == PF_KIND_HDMI && hdmi < 0)
			hdmi = i;
	}
	if (lcd >= 0) {
		/* the panel's CRTC: the lowest one it can use */
		panel_crtc = first_crtc(&conns[lcd], cfg, -1);
	}

	/* HDMI wins when it can show a picture, else the panel */
	if (hdmi >= 0 && pf_pick_mode(cfg, &conns[hdmi], &m, &builtin)) {
		int crtc;

		if (cfg->panel_keep_scanning && lcd >= 0) {
			/* HDMI never on the panel's CRTC */
			crtc = crtc_ok(&conns[hdmi], cfg, cfg->hdmi_crtc_index) &&
			       cfg->hdmi_crtc_index != panel_crtc ? cfg->hdmi_crtc_index
			       : first_crtc(&conns[hdmi], cfg, panel_crtc);
		} else {
			crtc = crtc_ok(&conns[hdmi], cfg, cfg->hdmi_crtc_index) ? cfg->hdmi_crtc_index
			       : first_crtc(&conns[hdmi], cfg, -1);
		}
		if (crtc >= 0) {
			ch.conn = hdmi;
			ch.crtc = crtc;
			ch.w = m.w;
			ch.h = m.h;
			ch.hz = m.hz;
			ch.builtin = builtin;
			return ch;
		}
	}
	if (lcd >= 0 && pf_pick_mode(cfg, &conns[lcd], &m, &builtin) && panel_crtc >= 0) {
		ch.conn = lcd;
		ch.crtc = panel_crtc;
		ch.w = m.w;
		ch.h = m.h;
		ch.hz = m.hz;
		ch.builtin = builtin;
	}
	return ch;
}
