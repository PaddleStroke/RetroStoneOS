/*
 * bootreason.c - AXP209 power-on source (see bootreason.h).
 */
#include "bootreason.h"
#include "psys.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum power_boot_reason bootreason_decode(int reg00)
{
	if (reg00 < 0 || reg00 > 0xff)
		return POWER_BOOT_UNKNOWN;
	return (reg00 & AXP209_REG00_BOOT_ACIN) ? POWER_BOOT_CHARGER : POWER_BOOT_KEY;
}

const char *bootreason_name(enum power_boot_reason r)
{
	switch (r) {
	case POWER_BOOT_KEY: return "key";
	case POWER_BOOT_CHARGER: return "charger";
	default: return "unknown";
	}
}

enum power_boot_reason bootreason_parse(const char *s)
{
	if (s && !strcmp(s, "key"))
		return POWER_BOOT_KEY;
	if (s && !strcmp(s, "charger"))
		return POWER_BOOT_CHARGER;
	return POWER_BOOT_UNKNOWN;
}

/* <sysfs>/bus/i2c/devices/N-0034 -> N */
static int find_axp_bus(const char *sysfs)
{
	char dir[PSYS_PATH_MAX];
	struct dirent *de;
	DIR *d;
	int bus = -ENODEV;

	if (!psys_path(dir, sizeof(dir), "%s/bus/i2c/devices", sysfs))
		return -ENAMETOOLONG;
	d = opendir(dir);
	if (!d)
		return -errno;
	while ((de = readdir(d))) {
		char *end;
		long n = strtol(de->d_name, &end, 10);
		if (end != de->d_name && !strcmp(end, "-0034")) {
			bus = (int)n;
			break;
		}
	}
	closedir(d);
	return bus;
}

static int axp_read(int fd, uint8_t reg, uint8_t *val, int len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = AXP209_I2C_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = AXP209_I2C_ADDR, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = val },
	};
	struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };

	return ioctl(fd, I2C_RDWR, &x) < 0 ? -errno : 0;
}

int bootreason_read_axp(const char *sysfs, const char *dev_dir, int *reg00, int *reg01)
{
	char path[PSYS_PATH_MAX];
	uint8_t v[2];
	int bus, fd, ret;

	bus = find_axp_bus(sysfs);
	if (bus < 0)
		return bus;
	if (!psys_path(path, sizeof(path), "%s/i2c-%d", dev_dir, bus))
		return -ENAMETOOLONG;
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	/* One register per transfer: no reliance on address auto-increment. */
	ret = axp_read(fd, 0x00, &v[0], 1);
	if (!ret)
		ret = axp_read(fd, 0x01, &v[1], 1);
	close(fd);
	if (ret)
		return ret;
	*reg00 = v[0];
	*reg01 = v[1];
	return 0;
}
