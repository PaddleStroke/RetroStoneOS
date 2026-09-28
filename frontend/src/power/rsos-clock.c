/*
 * rsos-clock - saved-time fallback for a board without an RTC backup cell.
 *
 *   rsos-clock save    [FILE]   write the current time to FILE (atomic, fsync)
 *   rsos-clock restore [FILE]   if the clock is earlier than FILE (or the
 *                               firmware build time), set it forward, write
 *                               the RTC and create /run/rsos/clock-restored
 *   rsos-clock systohc          write the RTC from the system clock (after NTP
 *                               or a manual set; BusyBox hwclock -w equivalent)
 *   rsos-clock show    [FILE]   print system time, RTC and FILE
 *
 * FILE defaults to /data/rsos/lastclock. Exit status 0 on success (restore
 * with nothing to do is a success), 1 on error.
 */
#include "pclock.h"
#include "psys.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RESTORED_FLAG "/run/rsos/clock-restored"

static void fmt(time_t t, char *buf, size_t n)
{
	struct tm tm;

	if (gmtime_r(&t, &tm))
		strftime(buf, n, "%Y-%m-%d %H:%M:%S UTC", &tm);
	else
		snprintf(buf, n, "?");
}

static int cmd_save(const char *file)
{
	int e = pclock_save(file, time(NULL));

	if (e == -EINVAL) {
		fprintf(stderr, "rsos-clock: clock not set (before the build date), not saving\n");
		return 1;
	}
	if (e) {
		fprintf(stderr, "rsos-clock: %s: %s\n", file, strerror(-e));
		return 1;
	}
	return 0;
}

static int cmd_restore(const char *file)
{
	time_t now = time(NULL), saved = 0, target;
	bool have = pclock_load(file, &saved) == 0;
	char a[40], b[40];
	int e;

	target = pclock_restore_target(now, saved, have);
	if (!target) {
		printf("ok\n");
		return 0;
	}
	fmt(now, a, sizeof(a));
	fmt(target, b, sizeof(b));
	e = pclock_set_system(target);
	if (e) {
		fprintf(stderr, "rsos-clock: settimeofday: %s\n", strerror(-e));
		return 1;
	}
	/* so the next boot does not start from 1970 again */
	if (pclock_rtc_write(PCLOCK_RTC_DEFAULT, target))
		fprintf(stderr, "rsos-clock: RTC write failed\n");
	mkdir("/run/rsos", 0755);
	psys_write_atomic(RESTORED_FLAG, a, strlen(a));
	printf("restored %s (clock was %s, source %s)\n", b, a, have ? file : "build date");
	return 0;
}

static int cmd_systohc(void)
{
	time_t now = time(NULL);
	int e;

	if (!pclock_sane(now)) {
		fprintf(stderr, "rsos-clock: clock not set, not writing the RTC\n");
		return 1;
	}
	e = pclock_rtc_write(PCLOCK_RTC_DEFAULT, now);
	if (e) {
		fprintf(stderr, "rsos-clock: %s: %s\n", PCLOCK_RTC_DEFAULT, strerror(-e));
		return 1;
	}
	unlink(RESTORED_FLAG);
	return 0;
}

static int cmd_show(const char *file)
{
	time_t now = time(NULL), t;
	char buf[40];

	fmt(now, buf, sizeof(buf));
	printf("system   %s%s\n", buf, pclock_sane(now) ? "" : " (not set)");
	if (!pclock_rtc_read(PCLOCK_RTC_DEFAULT, &t)) {
		fmt(t, buf, sizeof(buf));
		printf("rtc      %s\n", buf);
	} else {
		printf("rtc      unreadable\n");
	}
	if (!pclock_load(file, &t)) {
		fmt(t, buf, sizeof(buf));
		printf("saved    %s (%s)\n", buf, file);
	} else {
		printf("saved    none (%s)\n", file);
	}
	fmt(pclock_build_epoch(), buf, sizeof(buf));
	printf("build    %s\n", buf);
	return 0;
}

int main(int argc, char **argv)
{
	const char *file = argc > 2 ? argv[2] : PCLOCK_FILE_DEFAULT;

	if (argc < 2)
		goto usage;
	if (!strcmp(argv[1], "save"))
		return cmd_save(file);
	if (!strcmp(argv[1], "restore"))
		return cmd_restore(file);
	if (!strcmp(argv[1], "systohc"))
		return cmd_systohc();
	if (!strcmp(argv[1], "show"))
		return cmd_show(file);
usage:
	fprintf(stderr, "usage: %s save|restore|systohc|show [FILE]\n", argv[0]);
	return 1;
}
