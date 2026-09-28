/*
 * sys.c - CPU governor, battery level, power-off for the game process.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_internal.h"
#include "hutil.h"
#include "../board.h"

#define CPUFREQ "/sys/devices/system/cpu/cpufreq"
/* The board profile (src/board.h): schedutil / performance by default. */
#define DEFAULT_GOVERNOR (board_get()->cpu_governor_menu)
#define GAME_GOVERNOR (board_get()->cpu_governor_game)

static int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t w;

	if (fd < 0)
		return -errno;
	w = write(fd, s, strlen(s));
	close(fd);
	return w < 0 ? -errno : 0;
}

static int read_str(const char *path, char *buf, size_t n)
{
	long r = hread_file_into(path, buf, n - 1);

	if (r < 0)
		return (int)r;
	buf[r] = 0;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		buf[--r] = 0;
	return 0;
}

/*
 * performance while a game runs (schedutil's ramp-up drops frames), back
 * to schedutil after. The A20 has one policy for both cores (policy0);
 * every policy* found is set. The parent (host_launch) restores the
 * default again after the child exits, so a crash cannot leave the CPU
 * at full clock in the menu.
 */
void sys_governor_performance(bool on)
{
	DIR *d = opendir(CPUFREQ);
	struct dirent *de;
	int n = 0;

	if (!d) {
		hlog(HLOG_DEBUG, "no cpufreq: governor unchanged");
		return;
	}
	while ((de = readdir(d))) {
		char path[512], cur[64] = "";

		if (strncmp(de->d_name, "policy", 6) != 0)
			continue;
		snprintf(path, sizeof(path), CPUFREQ "/%s/scaling_governor", de->d_name);
		read_str(path, cur, sizeof(cur));
		if (write_str(path, on ? GAME_GOVERNOR : DEFAULT_GOVERNOR) == 0)
			n++;
		hlog(HLOG_INFO, "cpufreq %s: %s -> %s", de->d_name, cur, on ? GAME_GOVERNOR : DEFAULT_GOVERNOR);
	}
	closedir(d);
	if (!n)
		hlog(HLOG_DEBUG, "governor: nothing changed");
}

/* axp20x-battery (or any Battery-type supply). */
int sys_battery(int *percent, bool *charging)
{
	static char dir[300];
	char path[600], buf[64];

	if (!dir[0]) {
		DIR *d = opendir("/sys/class/power_supply");
		struct dirent *de;

		if (!d)
			return -ENODEV;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
			if (read_str(path, buf, sizeof(buf)) == 0 && !strcmp(buf, "Battery")) {
				snprintf(dir, sizeof(dir), "/sys/class/power_supply/%s", de->d_name);
				break;
			}
		}
		closedir(d);
		if (!dir[0]) {
			strcpy(dir, "-");
			return -ENODEV;
		}
	}
	if (dir[0] == '-')
		return -ENODEV;
	snprintf(path, sizeof(path), "%s/capacity", dir);
	if (read_str(path, buf, sizeof(buf)) < 0)
		return -EIO;
	*percent = atoi(buf);
	snprintf(path, sizeof(path), "%s/status", dir);
	*charging = read_str(path, buf, sizeof(buf)) == 0 &&
		    (!strcmp(buf, "Charging") || !strcmp(buf, "Full"));
	return 0;
}

void sys_power_off(const char *cmd)
{
	sync();
	if (!cmd || !*cmd)
		return;
	hlog(HLOG_INFO, "power off: %s", cmd);
	execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
	hlog(HLOG_ERROR, "cannot run %s: %s", cmd, strerror(errno));
}
