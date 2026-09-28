/*
 * pclock.c - saved-time fallback, RTC access and time zones (see pclock.h).
 */
#include "pclock.h"
#include "psys.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef RSOS_BUILD_EPOCH
#define RSOS_BUILD_EPOCH 1767225600L    /* 2026-01-01 00:00 UTC */
#endif

time_t pclock_build_epoch(void)
{
	return (time_t)RSOS_BUILD_EPOCH;
}

bool pclock_sane(time_t t)
{
	return t >= pclock_build_epoch();
}

int pclock_save(const char *path, time_t now)
{
	char buf[32];
	int n;

	if (!pclock_sane(now))
		return -EINVAL;
	n = snprintf(buf, sizeof(buf), "%lld\n", (long long)now);
	return psys_write_atomic(path, buf, (size_t)n);
}

int pclock_load(const char *path, time_t *out)
{
	char buf[64], *end;
	long long v;
	int ret = psys_read_str(path, buf, sizeof(buf));

	if (ret)
		return ret;
	errno = 0;
	v = strtoll(buf, &end, 10);
	if (end == buf || errno || v <= 0 || (*end && *end != '\n'))
		return -EINVAL;
	*out = (time_t)v;
	return 0;
}

time_t pclock_restore_target(time_t now, time_t saved, bool have_saved)
{
	time_t target = pclock_build_epoch();

	if (have_saved && saved > target)
		target = saved;
	return now < target ? target : 0;
}

int pclock_set_system(time_t t)
{
	struct timeval tv = { .tv_sec = t, .tv_usec = 0 };

	return settimeofday(&tv, NULL) ? -errno : 0;
}

int pclock_rtc_write(const char *dev, time_t t)
{
	struct rtc_time rt;
	struct tm tm;
	int fd, ret = 0;

	if (!gmtime_r(&t, &tm))
		return -EINVAL;
	memset(&rt, 0, sizeof(rt));
	rt.tm_sec = tm.tm_sec;
	rt.tm_min = tm.tm_min;
	rt.tm_hour = tm.tm_hour;
	rt.tm_mday = tm.tm_mday;
	rt.tm_mon = tm.tm_mon;
	rt.tm_year = tm.tm_year;
	rt.tm_wday = tm.tm_wday;
	rt.tm_yday = tm.tm_yday;
	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, RTC_SET_TIME, &rt) < 0)
		ret = -errno;
	close(fd);
	return ret;
}

int pclock_rtc_read(const char *dev, time_t *out)
{
	struct rtc_time rt;
	struct tm tm;
	int fd, ret = 0;

	fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	memset(&rt, 0, sizeof(rt));
	if (ioctl(fd, RTC_RD_TIME, &rt) < 0)
		ret = -errno;
	close(fd);
	if (ret)
		return ret;
	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = rt.tm_sec;
	tm.tm_min = rt.tm_min;
	tm.tm_hour = rt.tm_hour;
	tm.tm_mday = rt.tm_mday;
	tm.tm_mon = rt.tm_mon;
	tm.tm_year = rt.tm_year;
	*out = timegm(&tm);
	return 0;
}

/*
 * POSIX TZ strings, taken from the tzdata 2025 footers (the rule that
 * applies to current dates). Zones with irregular rules (Morocco, Iran
 * before 2022, ...) use their current fixed offset.
 */
static const struct power_tz tz_table[] = {
	{ "UTC",                            "UTC0" },
	{ "Europe/London",                  "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Europe/Dublin",                  "GMT0IST,M3.5.0/1,M10.5.0" },
	{ "Europe/Lisbon",                  "WET0WEST,M3.5.0/1,M10.5.0" },
	{ "Europe/Paris",                   "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Brussels",                "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Amsterdam",               "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Berlin",                  "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zurich",                  "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Madrid",                  "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Rome",                    "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Stockholm",               "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Warsaw",                  "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Athens",                  "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Helsinki",                "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Kyiv",                    "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Istanbul",                "<+03>-3" },
	{ "Europe/Moscow",                  "MSK-3" },
	{ "Africa/Lagos",                   "WAT-1" },
	{ "Africa/Cairo",                   "EET-2EEST,M4.5.5/0,M10.5.4/24" },
	{ "Africa/Johannesburg",            "SAST-2" },
	{ "Africa/Nairobi",                 "EAT-3" },
	{ "Asia/Jerusalem",                 "IST-2IDT,M3.4.4/26,M10.5.0" },
	{ "Asia/Riyadh",                    "<+03>-3" },
	{ "Asia/Tehran",                    "<+0330>-3:30" },
	{ "Asia/Dubai",                     "<+04>-4" },
	{ "Asia/Karachi",                   "PKT-5" },
	{ "Asia/Kolkata",                   "IST-5:30" },
	{ "Asia/Kathmandu",                 "<+0545>-5:45" },
	{ "Asia/Dhaka",                     "<+06>-6" },
	{ "Asia/Bangkok",                   "<+07>-7" },
	{ "Asia/Ho_Chi_Minh",               "<+07>-7" },
	{ "Asia/Jakarta",                   "WIB-7" },
	{ "Asia/Shanghai",                  "CST-8" },
	{ "Asia/Hong_Kong",                 "HKT-8" },
	{ "Asia/Taipei",                    "CST-8" },
	{ "Asia/Singapore",                 "<+08>-8" },
	{ "Asia/Manila",                    "PST-8" },
	{ "Asia/Seoul",                     "KST-9" },
	{ "Asia/Tokyo",                     "JST-9" },
	{ "Australia/Perth",                "AWST-8" },
	{ "Australia/Adelaide",             "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Brisbane",             "AEST-10" },
	{ "Australia/Sydney",               "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Pacific/Auckland",               "NZST-12NZDT,M9.5.0,M4.1.0/3" },
	{ "Pacific/Honolulu",               "HST10" },
	{ "America/Anchorage",              "AKST9AKDT,M3.2.0,M11.1.0" },
	{ "America/Los_Angeles",            "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Phoenix",                "MST7" },
	{ "America/Denver",                 "MST7MDT,M3.2.0,M11.1.0" },
	{ "America/Chicago",                "CST6CDT,M3.2.0,M11.1.0" },
	{ "America/Mexico_City",            "CST6" },
	{ "America/New_York",               "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Bogota",                 "<-05>5" },
	{ "America/Lima",                   "<-05>5" },
	{ "America/Caracas",                "<-04>4" },
	{ "America/Halifax",                "AST4ADT,M3.2.0,M11.1.0" },
	{ "America/Santiago",               "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
	{ "America/Sao_Paulo",              "<-03>3" },
	{ "America/Argentina/Buenos_Aires", "<-03>3" },
	{ "America/St_Johns",               "NST3:30NDT,M3.2.0,M11.1.0" },
};

const struct power_tz *pclock_timezones(int *count)
{
	if (count)
		*count = (int)(sizeof(tz_table) / sizeof(tz_table[0]));
	return tz_table;
}

const char *pclock_tz_lookup(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(tz_table) / sizeof(tz_table[0]); i++)
		if (!strcmp(tz_table[i].name, name))
			return tz_table[i].posix;
	return NULL;
}

bool pclock_tz_valid(const char *s)
{
	size_t n = s ? strlen(s) : 0;
	size_t i;

	if (n < 3 || n > 63)
		return false;
	for (i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || strchr("+-:,./<>", c)))
			return false;
	}
	return true;
}
