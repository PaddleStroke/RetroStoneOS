/*
 * audio.h - ALSA output for the libretro host (alsa-lib, no daemon).
 *
 * One PCM at a time, S16_LE stereo at a fixed 48 kHz (the codec's native
 * rate; also what the sun4i HDMI audio accepts), opened non-blocking on the
 * card of the active output:
 *   - LCD:  the built-in codec (sun4i-codec: headphone jack / speaker amp)
 *   - HDMI: the "sun4i-hdmi" card (kernel patch 0002)
 * The display layer's on_audio hook drives audio_close() (RELEASE, before
 * the modeset) and audio_open() (ACQUIRE, after it).
 *
 * Writes never block: the caller (the host's main loop) measures the fill
 * level with audio_queued() for pacing and dynamic rate control.
 * Pops: every open starts with silence and a 10 ms fade-in; pause and close
 * fade the signal to zero first (the PAM8302 amp cannot be muted).
 */
#ifndef RSOS_AUDIO_AUDIO_H
#define RSOS_AUDIO_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_RATE 48000

enum audio_output { AUDIO_OUT_LCD = 0, AUDIO_OUT_HDMI };

struct audio_config {
	int latency_ms;          /* total ALSA buffer, default 64 */
	int periods;             /* default 4 (HDMI: rounded to its 4 KiB minimum period) */
	const char *device_lcd;  /* NULL = auto (built-in codec card) */
	const char *device_hdmi; /* NULL = auto ("sun4i-hdmi" card) */
	/* auto HDMI card: the n-th card named "hdmi" (0 = the first; boards
	 * with two HDMI ports, e.g. the Raspberry Pi 4: HDMI-A-2 -> 1) */
	int hdmi_port;
	/* ALSA PCM type for the auto HDMI card: NULL = "plughw" (plughw:N,0);
	 * e.g. "hdmi" gives hdmi:CARD=N,DEV=0, the card's own IEC958 set-up
	 * (vc4-hdmi on the Raspberry Pi takes IEC958 subframes only) */
	const char *hdmi_pcm;
};

struct audio_stats {
	uint64_t frames_written;
	uint64_t frames_dropped;   /* no room in the buffer */
	unsigned underruns;
	unsigned opens;
};

void audio_config_defaults(struct audio_config *cfg);

/* Picks the ALSA device name for an output ("plughw:1,0"), NULL if none.
 * Static buffer. */
const char *audio_pick_device(const struct audio_config *cfg, enum audio_output out);

/* Opens the PCM for `out` and pre-fills `prefill_ms` of silence.
 * Returns 0 or -errno (audio is then simply off). */
int audio_open(const struct audio_config *cfg, enum audio_output out, int prefill_ms);
/* Fades out, drains what is queued (<= latency) and closes. */
void audio_close(void);
bool audio_is_open(void);

/* Changes the latency (SET_MINIMUM_AUDIO_LATENCY, underrun back-off):
 * reopens the PCM if it is open. */
int audio_set_latency(int latency_ms);
int audio_latency_ms(void);

/* Non-blocking write of interleaved stereo frames. Recovers underruns
 * (then refills to half the buffer with silence). Returns frames accepted. */
int audio_write(const int16_t *frames, int n);

/* Buffer geometry and fill (frames). queued = buffer - avail. */
int audio_buffer_frames(void);
int audio_period_frames(void);
int audio_queued(void);
/* fill level 0..1, 0 if closed. */
double audio_fill(void);

/* Pause: fade to zero, then only silence (call audio_keepalive() at least
 * every 20 ms while paused so the stream never stops: no DAPM pop);
 * resume: fade-in. */
void audio_pause(bool pause);
bool audio_paused(void);
void audio_keepalive(void);

/* Adds silence (e.g. to reach a target fill). */
void audio_write_silence(int frames);

void audio_get_stats(struct audio_stats *st);

/* Fd set for poll(): fills up to max pollfds, returns the count (0 if
 * closed). Not used by the host loop today (it paces on its own clock). */
int audio_poll_fds(void *pfds, int max);

#endif
