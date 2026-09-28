/*
 * rsos-axpstamp - keep the times of the last shutdown steps across the power
 * cut, in the AXP209's 12 data buffer registers (REG04-REG0F).
 *
 * rcK writes /data/rsos/logs/shutdown.txt before it unmounts /data, so the
 * last steps (sync, unmount, poweroff -f) cannot be logged there. The AXP209
 * keeps REG04-0F as long as it has a power source (the battery), also while
 * the system is off, and neither U-Boot nor Linux use them. So rcK saves the
 * uptimes of those steps there just before "poweroff -f", and the boot logger
 * appends them to shutdown.txt on the next boot (and clears them).
 * TODO(hw): check that the registers survive a power-off on battery.
 *
 *   rsos-axpstamp save <t_log> <t_sync> <t_umount>
 *       uptimes in seconds ("12.34", as in /proc/uptime) of "log written",
 *       "sync done" and "/data unmounted"; the current uptime is added as
 *       the fourth time ("poweroff -f"). Stored as 4 x 24-bit centiseconds.
 *   rsos-axpstamp show
 *       prints "<t_log> <t_sync> <t_umount> <t_poweroff>" in seconds and
 *       clears the registers; exit status 1 (nothing printed) if they hold
 *       no plausible record.
 *   -b <N>  I2C bus number (default 0: the A20's TWI0, where the AXP209 is)
 *
 * The AXP209 (0x34) is bound to the axp20x driver, so plain I2C_SLAVE
 * would fail with EBUSY; I2C_RDWR transfers go through the adapter's lock
 * next to the driver's own (as rsos-bootreason does). One register per
 * transfer: the AXP209 does not auto-increment on writes.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define AXP_ADDR	0x34
#define AXP_DATA0	0x04
#define NVALS		4
#define MAX_CS		0xffffff	/* 24 bits of centiseconds: 46 hours */

static int reg_write(int fd, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = AXP_ADDR, .flags = 0, .len = 2, .buf = buf };
	struct i2c_rdwr_ioctl_data x = { .msgs = &msg, .nmsgs = 1 };

	return ioctl(fd, I2C_RDWR, &x) == 1 ? 0 : -1;
}

static int reg_read(int fd, uint8_t reg, uint8_t *val)
{
	struct i2c_msg msgs[2] = {
		{ .addr = AXP_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = AXP_ADDR, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };

	return ioctl(fd, I2C_RDWR, &x) == 2 ? 0 : -1;
}

/* "12.34" -> 1234 (centiseconds); -1 if not a number */
static long parse_cs(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || v < 0 || v * 100 > MAX_CS)
		return -1;
	return (long)(v * 100 + 0.5);
}

static long uptime_cs(void)
{
	char buf[64];
	FILE *f = fopen("/proc/uptime", "r");
	long cs = -1;

	if (f) {
		if (fgets(buf, sizeof(buf), f))
			cs = parse_cs(buf);
		fclose(f);
	}
	return cs;
}

static int usage(void)
{
	fprintf(stderr, "usage: rsos-axpstamp [-b N] save <t_log> <t_sync> <t_umount>\n"
			"       rsos-axpstamp [-b N] show\n");
	return 2;
}

int main(int argc, char **argv)
{
	char dev[32];
	int bus = 0, fd, i, j;
	long cs[NVALS];
	uint8_t raw[NVALS * 3];

	if (argc > 2 && strcmp(argv[1], "-b") == 0) {
		bus = atoi(argv[2]);
		argv += 2;
		argc -= 2;
	}
	if (argc < 2)
		return usage();
	snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);

	if (strcmp(argv[1], "save") == 0) {
		if (argc != 5)
			return usage();
		for (i = 0; i < 3; i++)
			cs[i] = parse_cs(argv[2 + i]);
		cs[3] = uptime_cs();
		for (i = 0; i < NVALS; i++) {
			if (cs[i] < 0)
				return usage();
			raw[i * 3] = cs[i] >> 16;
			raw[i * 3 + 1] = cs[i] >> 8;
			raw[i * 3 + 2] = cs[i];
		}
		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "rsos-axpstamp: %s: %s\n", dev, strerror(errno));
			return 1;
		}
		for (j = 0; j < (int)sizeof(raw); j++) {
			if (reg_write(fd, AXP_DATA0 + j, raw[j]) < 0) {
				fprintf(stderr, "rsos-axpstamp: write REG%02X: %s\n",
					AXP_DATA0 + j, strerror(errno));
				close(fd);
				return 1;
			}
		}
		close(fd);
		return 0;
	}

	if (strcmp(argv[1], "show") == 0 && argc == 2) {
		int ok = 1;

		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "rsos-axpstamp: %s: %s\n", dev, strerror(errno));
			return 1;
		}
		for (j = 0; j < (int)sizeof(raw); j++) {
			if (reg_read(fd, AXP_DATA0 + j, &raw[j]) < 0) {
				fprintf(stderr, "rsos-axpstamp: read REG%02X: %s\n",
					AXP_DATA0 + j, strerror(errno));
				close(fd);
				return 1;
			}
		}
		for (i = 0; i < NVALS; i++)
			cs[i] = (long)raw[i * 3] << 16 | raw[i * 3 + 1] << 8 | raw[i * 3 + 2];
		/* Plausible: non-zero, in order, all within a minute */
		for (i = 0; i < NVALS; i++) {
			if (cs[i] == 0 || (i && cs[i] < cs[i - 1]))
				ok = 0;
		}
		if (cs[NVALS - 1] - cs[0] > 6000)
			ok = 0;
		/* Report once: clear the registers */
		for (j = 0; j < (int)sizeof(raw); j++)
			if (raw[j])
				reg_write(fd, AXP_DATA0 + j, 0);
		close(fd);
		if (!ok)
			return 1;
		for (i = 0; i < NVALS; i++)
			printf("%s%ld.%02ld", i ? " " : "", cs[i] / 100, cs[i] % 100);
		printf("\n");
		return 0;
	}

	return usage();
}
