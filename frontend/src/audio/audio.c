/*
 * audio.c - see audio.h.
 */
#include "audio.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../host/hutil.h"

#define FADE_IN_FRAMES  480   /* 10 ms */
#define FADE_OUT_FRAMES 240   /* 5 ms */

static struct {
	snd_pcm_t *pcm;
	struct audio_config cfg;
	enum audio_output out;
	int latency_ms;
	snd_pcm_uframes_t buffer, period;
	int fade_in;            /* frames left in the fade-in ramp */
	int16_t last_l, last_r; /* last frame written (fade-out start) */
	bool paused;
	struct audio_stats st;
	char dev[64];
} A = { .latency_ms = 64 };

void audio_config_defaults(struct audio_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->latency_ms = 64;
	cfg->periods = 4;
}

static bool name_has(const char *s, const char *sub)
{
	size_t n = strlen(sub);

	for (; s && *s; s++)
		if (!strncasecmp(s, sub, n))
			return true;
	return false;
}

const char *audio_pick_device(const struct audio_config *cfg, enum audio_output out)
{
	static char dev[64];
	int card = -1, pick = -1, first_other = -1, first_hdmi = -1;
	int hdmi_skip = cfg && cfg->hdmi_port > 0 ? cfg->hdmi_port : 0;

	if (out == AUDIO_OUT_HDMI && cfg && cfg->device_hdmi)
		return cfg->device_hdmi;
	if (out == AUDIO_OUT_LCD && cfg && cfg->device_lcd)
		return cfg->device_lcd;
	while (snd_card_next(&card) == 0 && card >= 0) {
		char *name = NULL, *lname = NULL;
		bool hdmi;

		snd_card_get_name(card, &name);
		snd_card_get_longname(card, &lname);
		hdmi = name_has(name, "hdmi") || name_has(lname, "hdmi");
		hlog(HLOG_DEBUG, "ALSA card %d: \"%s\" (%s)%s", card, name ? name : "?",
		     lname ? lname : "?", hdmi ? " [hdmi]" : "");
		if (out == AUDIO_OUT_HDMI && hdmi) {
			if (first_hdmi < 0)
				first_hdmi = card;
			if (pick < 0 && hdmi_skip-- == 0)
				pick = card;
		}
		if (out == AUDIO_OUT_LCD && !hdmi) {
			if (pick < 0 && (name_has(name, "codec") || name_has(lname, "codec")))
				pick = card;
			if (first_other < 0)
				first_other = card;
		}
		free(name);
		free(lname);
	}
	if (pick < 0 && out == AUDIO_OUT_HDMI)
		pick = first_hdmi;   /* fewer HDMI cards than ports: the first */
	if (pick < 0)
		pick = first_other;
	if (pick < 0)
		return NULL;
	if (out == AUDIO_OUT_HDMI && cfg && cfg->hdmi_pcm && *cfg->hdmi_pcm && strcmp(cfg->hdmi_pcm, "plughw"))
		snprintf(dev, sizeof(dev), "%s:CARD=%d,DEV=0", cfg->hdmi_pcm, pick);
	else
		snprintf(dev, sizeof(dev), "plughw:%d,0", pick);
	return dev;
}

static int setup(snd_pcm_t *pcm, int latency_ms, int periods)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	unsigned rate = AUDIO_RATE, buf_us, per_us;
	int dir = 0, err;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0)
		return err;
	if (rate != AUDIO_RATE)
		hlog(HLOG_WARN, "ALSA: %u Hz instead of %u", rate, AUDIO_RATE);
	buf_us = (unsigned)latency_ms * 1000;
	per_us = buf_us / (unsigned)(periods > 1 ? periods : 4);
	dir = 0;
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, hw, &per_us, &dir)) < 0)
		return err;
	dir = 0;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buf_us, &dir)) < 0)
		return err;
	if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
		return err;
	snd_pcm_hw_params_get_buffer_size(hw, &A.buffer);
	snd_pcm_hw_params_get_period_size(hw, &A.period, &dir);

	if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
	    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw, A.period)) < 0 ||
	    (err = snd_pcm_sw_params_set_avail_min(pcm, sw, A.period)) < 0 ||
	    (err = snd_pcm_sw_params(pcm, sw)) < 0)
		return err;
	return 0;
}

int audio_open(const struct audio_config *cfg, enum audio_output out, int prefill_ms)
{
	const char *dev;
	snd_pcm_t *pcm;
	int err;

	if (A.pcm)
		audio_close();
	if (cfg)
		A.cfg = *cfg;
	else
		audio_config_defaults(&A.cfg);
	if (A.cfg.latency_ms > 0 && A.latency_ms < A.cfg.latency_ms)
		A.latency_ms = A.cfg.latency_ms;
	A.out = out;
	dev = audio_pick_device(&A.cfg, out);
	if (!dev) {
		hlog(HLOG_WARN, "audio: no ALSA card for the %s", out == AUDIO_OUT_HDMI ? "HDMI output" : "LCD");
		return -ENODEV;
	}
	hstrlcpy(A.dev, dev, sizeof(A.dev));
	err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (err < 0) {
		hlog(HLOG_WARN, "audio: cannot open %s: %s", dev, snd_strerror(err));
		return err;
	}
	err = setup(pcm, A.latency_ms, A.cfg.periods);
	if (err < 0) {
		hlog(HLOG_WARN, "audio: cannot configure %s: %s", dev, snd_strerror(err));
		snd_pcm_close(pcm);
		return err;
	}
	A.pcm = pcm;
	A.paused = false;
	A.st.opens++;
	A.last_l = A.last_r = 0;
	hlog(HLOG_INFO, "audio: %s open, %d Hz, buffer %lu frames (%.1f ms), period %lu (%.1f ms)",
	     dev, AUDIO_RATE, (unsigned long)A.buffer, A.buffer * 1000.0 / AUDIO_RATE,
	     (unsigned long)A.period, A.period * 1000.0 / AUDIO_RATE);
	audio_write_silence(prefill_ms * AUDIO_RATE / 1000);
	A.fade_in = FADE_IN_FRAMES;
	return 0;
}

static void fade_out_tail(void)
{
	int16_t buf[FADE_OUT_FRAMES * 2];

	if (!A.pcm)
		return;
	for (int i = 0; i < FADE_OUT_FRAMES; i++) {
		float g = 1.0f - (float)(i + 1) / FADE_OUT_FRAMES;

		buf[2 * i] = (int16_t)(A.last_l * g);
		buf[2 * i + 1] = (int16_t)(A.last_r * g);
	}
	snd_pcm_writei(A.pcm, buf, FADE_OUT_FRAMES);
	A.last_l = A.last_r = 0;
}

void audio_close(void)
{
	if (!A.pcm)
		return;
	fade_out_tail();
	audio_write_silence(AUDIO_RATE / 100);
	/* Drain with a deadline (review F-L19: a blocking drain on a stalled
	 * HDMI sink could hang the exit): 500 ms at most, then drop. */
	snd_pcm_nonblock(A.pcm, 1);
	if (snd_pcm_drain(A.pcm) == -EAGAIN)
		for (int i = 0; i < 50 && snd_pcm_state(A.pcm) == SND_PCM_STATE_DRAINING; i++)
			usleep(10000);
	snd_pcm_drop(A.pcm);
	snd_pcm_close(A.pcm);
	A.pcm = NULL;
	hlog(HLOG_INFO, "audio: %s closed (underruns %u, dropped %llu frames)", A.dev,
	     A.st.underruns, (unsigned long long)A.st.frames_dropped);
}

bool audio_is_open(void)
{
	return A.pcm != NULL;
}

int audio_set_latency(int latency_ms)
{
	struct audio_config cfg = A.cfg;
	bool was = A.pcm != NULL;

	if (latency_ms < 16)
		latency_ms = 16;
	if (latency_ms > 500)
		latency_ms = 500;
	A.latency_ms = latency_ms;
	cfg.latency_ms = latency_ms;
	if (!was)
		return 0;
	audio_close();
	return audio_open(&cfg, A.out, latency_ms / 2);
}

int audio_latency_ms(void)
{
	return A.latency_ms;
}

static void recover(int err)
{
	if (err == -EPIPE)
		A.st.underruns++;
	err = snd_pcm_recover(A.pcm, err, 1);
	if (err < 0) {
		snd_pcm_prepare(A.pcm);
	}
	/* Refill to half the buffer so we do not sit at the edge. */
	audio_write_silence((int)A.buffer / 2);
	A.fade_in = FADE_IN_FRAMES;
}

static int write_raw(const int16_t *frames, int n)
{
	int done = 0;

	while (done < n) {
		snd_pcm_sframes_t w = snd_pcm_writei(A.pcm, frames + 2 * done, (snd_pcm_uframes_t)(n - done));

		if (w == -EAGAIN)
			break;
		if (w == -EINTR)
			continue;                /* a signal, not an error (F-L19) */
		if (w == -EPIPE || w == -ESTRPIPE) {
			recover((int)w);
			continue;
		}
		if (w < 0) {
			hlog_once("alsa-write", "audio: write error %s", snd_strerror((int)w));
			break;
		}
		done += (int)w;
	}
	return done;
}

void audio_write_silence(int frames)
{
	static const int16_t zero[512 * 2];

	if (!A.pcm)
		return;
	while (frames > 0) {
		int n = frames > 512 ? 512 : frames;
		snd_pcm_sframes_t w = snd_pcm_writei(A.pcm, zero, (snd_pcm_uframes_t)n);

		if (w == -EINTR)
			continue;
		if (w == -EPIPE || w == -ESTRPIPE) {
			snd_pcm_recover(A.pcm, (int)w, 1);
			continue;
		}
		if (w <= 0)
			break;
		frames -= (int)w;
	}
}

int audio_write(const int16_t *frames, int n)
{
	int16_t tmp[1024 * 2];
	int done = 0;

	if (!A.pcm || A.paused || n <= 0)
		return 0;
	while (done < n) {
		int chunk = n - done, w;
		const int16_t *src = frames + 2 * done;

		if (A.fade_in > 0) {
			if (chunk > 1024)
				chunk = 1024;
			for (int i = 0; i < chunk; i++) {
				float g = A.fade_in > 0 ? 1.0f - (float)A.fade_in / FADE_IN_FRAMES : 1.0f;

				tmp[2 * i] = (int16_t)(src[2 * i] * g);
				tmp[2 * i + 1] = (int16_t)(src[2 * i + 1] * g);
				if (A.fade_in > 0)
					A.fade_in--;
			}
			src = tmp;
		}
		w = write_raw(src, chunk);
		done += w;
		if (w < chunk)
			break;
	}
	if (done > 0) {
		A.last_l = frames[2 * (done - 1)];
		A.last_r = frames[2 * (done - 1) + 1];
	}
	A.st.frames_written += (uint64_t)done;
	A.st.frames_dropped += (uint64_t)(n - done);
	return done;
}

int audio_buffer_frames(void)
{
	return A.pcm ? (int)A.buffer : 0;
}

int audio_period_frames(void)
{
	return A.pcm ? (int)A.period : 0;
}

int audio_queued(void)
{
	snd_pcm_sframes_t avail;

	if (!A.pcm)
		return 0;
	avail = snd_pcm_avail(A.pcm);
	if (avail < 0) {
		if (A.paused)
			return 0;
		recover((int)avail);
		avail = snd_pcm_avail(A.pcm);
		if (avail < 0)
			return 0;
	}
	if ((snd_pcm_uframes_t)avail > A.buffer)
		avail = (snd_pcm_sframes_t)A.buffer;
	return (int)(A.buffer - (snd_pcm_uframes_t)avail);
}

double audio_fill(void)
{
	return A.pcm && A.buffer ? (double)audio_queued() / (double)A.buffer : 0.0;
}

void audio_pause(bool pause)
{
	if (!A.pcm || pause == A.paused)
		return;
	if (pause) {
		/* Ramp to zero, then keep the stream alive with silence from
		 * audio_queued() callers (no XRUN, so DAPM never powers the DAC
		 * down and up: that would pop through the amp). */
		fade_out_tail();
		A.paused = true;
	} else {
		snd_pcm_state_t s = snd_pcm_state(A.pcm);

		if (s == SND_PCM_STATE_XRUN || s == SND_PCM_STATE_SETUP)
			snd_pcm_prepare(A.pcm);
		A.paused = false;
		A.fade_in = FADE_IN_FRAMES;
	}
}

bool audio_paused(void)
{
	return A.paused;
}

void audio_get_stats(struct audio_stats *st)
{
	*st = A.st;
}

int audio_poll_fds(void *pfds, int max)
{
	if (!A.pcm || max <= 0)
		return 0;
	return snd_pcm_poll_descriptors(A.pcm, (struct pollfd *)pfds, (unsigned)max);
}

void audio_keepalive(void)
{
	snd_pcm_sframes_t avail;
	snd_pcm_state_t s;

	if (!A.pcm || !A.paused)
		return;
	s = snd_pcm_state(A.pcm);
	if (s == SND_PCM_STATE_XRUN || s == SND_PCM_STATE_SETUP)
		snd_pcm_prepare(A.pcm);
	avail = snd_pcm_avail(A.pcm);
	if (avail < 0)
		return;
	if ((snd_pcm_uframes_t)avail > A.buffer / 2)
		audio_write_silence((int)((snd_pcm_uframes_t)avail - A.buffer / 2));
}
