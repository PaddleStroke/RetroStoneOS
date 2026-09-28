/*
 * pclock.h - wall clock helpers: the saved-time fallback ("fake hwclock")
 * for a board without an RTC backup cell, the RTC write, and the time zone
 * table for the settings screen.
 *
 * The RetroStone2 RTC (A20 RTC on AXP LDO1) keeps time while the main
 * battery is connected, and loses it when the battery is unplugged or
 * fully drained (the AXP209 BACKUP pin only goes to a test pad). So:
 *   - the time is saved to /data/rsos/lastclock at shutdown and every
 *     10 minutes (power module, rsos-clock save);
 *   - at boot, rsos-clock restore moves the clock forward to that value
 *     (and to the firmware build time) if the RTC is earlier.
 */
#ifndef RSOS_POWER_PCLOCK_H
#define RSOS_POWER_PCLOCK_H

#include <stdbool.h>
#include <time.h>

/* The build time: power.mk passes -DRSOS_BUILD_EPOCH=... when compiling
 * pclock.c only (so every object agrees). The clock can never legitimately
 * be earlier. Fallback: 2026-01-01 00:00 UTC. */
time_t pclock_build_epoch(void);

#define PCLOCK_FILE_DEFAULT "/data/rsos/lastclock"
#define PCLOCK_RTC_DEFAULT "/dev/rtc0"

/* True when t is not before the build time (an RTC reset reads 1970). */
bool pclock_sane(time_t t);

/* Atomic, fsync'ed "<seconds>\n". -EINVAL if now is not sane (never
 * overwrite a good value with 1970). */
int pclock_save(const char *path, time_t now);
/* 0 and *out, or -errno (-EINVAL for garbage). */
int pclock_load(const char *path, time_t *out);

/*
 * The restore rule, pure: returns the time to set, or 0 when the clock is
 * fine. target = max(saved, build epoch); set it if now < target.
 * (A saved value from the future is accepted: it only moves forward.)
 */
time_t pclock_restore_target(time_t now, time_t saved, bool have_saved);

/* settimeofday(). 0 or -errno. */
int pclock_set_system(time_t t);
/* RTC_SET_TIME on dev (UTC). 0 or -errno. */
int pclock_rtc_write(const char *dev, time_t t);
/* RTC_RD_TIME. 0 or -errno. */
int pclock_rtc_read(const char *dev, time_t *out);

/* Time zones offered by the settings screen: POSIX TZ strings, so no
 * tzdata is needed on the read-only root. */
struct power_tz {
	const char *name;      /* "Europe/Paris" */
	const char *posix;     /* "CET-1CEST,M3.5.0,M10.5.0/3" */
};
const struct power_tz *pclock_timezones(int *count);
/* The POSIX string for a zone name, or NULL. */
const char *pclock_tz_lookup(const char *name);
/* Basic sanity check of a POSIX TZ string (length, charset). */
bool pclock_tz_valid(const char *posix);

#endif
