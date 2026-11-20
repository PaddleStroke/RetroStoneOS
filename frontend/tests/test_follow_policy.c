/* test_follow_policy.c - host unit test for frontend/src/follow/policy.c */
#include <stdio.h>
#include <string.h>

#include "../src/follow/policy.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static struct pf_cfg cfg_rs2(void)
{
	struct pf_cfg c = { .hdmi_w = 1280, .hdmi_h = 720, .hdmi_hz = 60, .hdmi_builtin_mode = true,
			    .hdmi_crtc_index = 1, .panel_keep_scanning = true, .crtc_count = 2 };
	return c;
}

static struct pf_conn panel(void)
{
	struct pf_conn p = { .kind = PF_KIND_LCD, .connected = true, .has_edid = false, .possible_crtcs = 1u };
	p.nmodes = 1;
	p.modes[0] = (struct pf_mode){ 800, 480, 60, true };
	return p;
}

static struct pf_conn hdmi(bool edid)
{
	struct pf_conn h = { .kind = PF_KIND_HDMI, .connected = true, .has_edid = edid, .possible_crtcs = 3u };
	h.nmodes = 3;
	h.modes[0] = (struct pf_mode){ 1920, 1080, 60, true };
	h.modes[1] = (struct pf_mode){ 1280, 720, 60, false };
	h.modes[2] = (struct pf_mode){ 640, 480, 60, false };
	return h;
}

static void test_hdmi_plugged_wins_on_crtc1(void)
{
	struct pf_cfg c = cfg_rs2();
	struct pf_conn cs[2] = { panel(), hdmi(true) };
	struct pf_choice ch = pf_choose(&c, cs, 2);

	CHECK(ch.conn == 1, "HDMI chosen when plugged in");
	CHECK(ch.crtc == 1, "HDMI on CRTC 1 (panel keeps CRTC 0)");
	CHECK(ch.w == 1280 && ch.h == 720 && ch.hz == 60, "policy 720p60, not the preferred 1080p");
}

static void test_hdmi_never_on_panel_crtc(void)
{
	struct pf_cfg c = cfg_rs2();
	struct pf_conn cs[2] = { panel(), hdmi(true) };

	cs[1].possible_crtcs = 1u; /* HDMI can only use CRTC 0 */
	struct pf_choice ch = pf_choose(&c, cs, 2);
	CHECK(ch.conn != 1, "HDMI refused when it could only use the panel's CRTC");
	CHECK(ch.conn == 0 && ch.crtc == 0, "falls back to the panel");
}

static void test_no_edid_uses_cea720(void)
{
	struct pf_cfg c = cfg_rs2();
	struct pf_conn cs[2] = { panel(), hdmi(false) };
	struct pf_choice ch = pf_choose(&c, cs, 2);

	CHECK(ch.conn == 1 && ch.builtin, "no EDID: CEA 720p60 builtin");
	CHECK(ch.w == 1280 && ch.h == 720, "builtin 1280x720");
}

static void test_hdmi_unplugged_is_panel(void)
{
	struct pf_cfg c = cfg_rs2();
	struct pf_conn cs[2] = { panel(), hdmi(true) };
	struct pf_choice ch;

	cs[1].connected = false;
	ch = pf_choose(&c, cs, 2);
	CHECK(ch.conn == 0 && ch.crtc == 0, "unplugged: built-in panel on CRTC 0");
}

static void test_no_panel_board(void)
{
	/* a board with HDMI only (no built-in screen) and no panel */
	struct pf_cfg c = cfg_rs2();
	struct pf_conn cs[1] = { hdmi(true) };
	struct pf_choice ch = pf_choose(&c, cs, 1);

	CHECK(ch.conn == 0 && ch.crtc == 1, "HDMI-only board: HDMI on CRTC 1");
}

static void test_nothing_connected(void)
{
	struct pf_cfg c = cfg_rs2();
	struct pf_conn cs[1] = { panel() };
	struct pf_choice ch;

	cs[0].connected = false;
	ch = pf_choose(&c, cs, 1);
	CHECK(ch.conn == -1 && ch.crtc == -1, "no output: none");
}

int main(void)
{
	test_hdmi_plugged_wins_on_crtc1();
	test_hdmi_never_on_panel_crtc();
	test_no_edid_uses_cea720();
	test_hdmi_unplugged_is_panel();
	test_no_panel_board();
	test_nothing_connected();
	if (fails) {
		fprintf(stderr, "%d failure(s)\n", fails);
		return 1;
	}
	printf("follow policy: all tests pass\n");
	return 0;
}
