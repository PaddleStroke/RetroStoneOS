/*
 * osd.c - toasts, drawn into a copy of the core frame (in its own pixel
 * format and resolution) just before it is presented, so they follow the
 * game through every scaling path. Costs one frame copy, only while
 * something is on screen. Toasts are TrueType text (any language, draw.h),
 * cut with "..." at the frame's edge. The FPS overlay normally lives on the display's
 * overlay plane (host.c, no copy); it is drawn here only when the plane is
 * refused (H.stats_on_plane false).
 */
#include <stdio.h>
#include <string.h>

#include "../audio/audio.h"
#include "draw.h"
#include "host_internal.h"
#include "hutil.h"

#define NTOAST 3

static struct {
	char msg[NTOAST][160];   /* translated: UTF-8, longer than English */
	int64_t until[NTOAST];
	char stats[2][48];
	int64_t stats_at;
} T;

void osd_toast(const char *msg, int ms)
{
	int slot = 0;
	int64_t now = hnow_ms();

	/* Replace the oldest (or an expired) line. */
	for (int i = 1; i < NTOAST; i++)
		if (T.until[i] < T.until[slot])
			slot = i;
	hstrlcpy(T.msg[slot], msg, sizeof(T.msg[slot]));
	T.until[slot] = now + ms;
	hlog(HLOG_INFO, "toast: %s", msg);
}

void osd_clear(void)
{
	memset(T.until, 0, sizeof(T.until));
}

void osd_untoast(const char *msg)
{
	for (int i = 0; i < NTOAST; i++)
		if (!strcmp(T.msg[i], msg))
			T.until[i] = 0;
}

bool osd_active(void)
{
	int64_t now = hnow_ms();

	if (H.show_stats && !H.stats_on_plane)
		return true;
	/* the fast-forward indicator, when the overlay plane is refused */
	if (H.ff_speed > 1 && !H.ff_on_plane)
		return true;
	for (int i = 0; i < NTOAST; i++)
		if (T.until[i] > now)
			return true;
	return false;
}

static void update_stats(int64_t now)
{
	struct display_stats ds = { 0 };
	struct audio_stats as;

	if (now - T.stats_at < 500)
		return;
	T.stats_at = now;
	if (H.display_ok)
		display_get_stats(&ds);
	audio_get_stats(&as);
	snprintf(T.stats[0], sizeof(T.stats[0]), "%.1f fps %.1f/%.1fms", H.fps, H.ft_avg_ms, H.ft_max_ms);
	snprintf(T.stats[1], sizeof(T.stats[1]), "drp %llu au %d%% un %u",
		 (unsigned long long)(ds.dropped + H.frames_skipped), (int)(audio_fill() * 100 + 0.5),
		 as.underruns);
}

void osd_draw(void *px, int pitch, int w, int h, uint32_t fmt)
{
	int64_t now = hnow_ms();
	int scale = w >= 512 ? 2 : 1;
	int line = frame_text_height(scale);
	int y;

	if (H.show_stats && !H.stats_on_plane) {
		/* ASCII figures: the 8x8 font */
		update_stats(now);
		frame_text_8x8(px, pitch, w, h, fmt, 0, 0, H.stats_line[0] ? H.stats_line : T.stats[0], scale,
			       0xffff40, 0x000000);
		frame_text_8x8(px, pitch, w, h, fmt, 0, 10 * scale, T.stats[1], scale, 0xffff40, 0x000000);
	}
	if (H.ff_speed > 1 && !H.ff_on_plane) {
		char ff[16];

		snprintf(ff, sizeof(ff), ">> x%d", H.ff_speed);
		frame_text_8x8(px, pitch, w, h, fmt, w - (int)strlen(ff) * 8 * scale - 4 * scale, 0, ff, scale,
			       0xffffff, 0x000000);
	}
	/* Toasts: bottom-left, newest at the bottom. */
	y = h - line;
	{
		int order[NTOAST], n = 0;

		for (int i = 0; i < NTOAST; i++)
			if (T.until[i] > now)
				order[n++] = i;
		/* newest last (at the bottom): sort by expiry */
		for (int i = 0; i < n; i++)
			for (int j = i + 1; j < n; j++)
				if (T.until[order[j]] < T.until[order[i]]) {
					int t = order[i];

					order[i] = order[j];
					order[j] = t;
				}
		for (int i = n - 1; i >= 0; i--, y -= line)
			frame_text(px, pitch, w, h, fmt, 0, y, T.msg[order[i]], scale, 0xffffff, 0x202040);
	}
}
