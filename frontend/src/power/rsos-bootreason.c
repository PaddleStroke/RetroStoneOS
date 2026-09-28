/*
 * rsos-bootreason - print why the AXP209 powered the board on.
 *
 *   rsos-bootreason [-v] [-w FILE]
 *
 * Prints "key", "charger" or "unknown" on stdout. With -w, also writes it
 * to FILE (atomically; rcS uses /run/rsos/bootreason, which the frontend's
 * power module reads). -v adds the raw registers on stderr.
 * Exit status: 0 key, 10 charger, 1 unknown (read error): an init script
 * can branch on it without parsing.
 *
 * Cost: one sysfs directory scan and two I2C register reads (~1 ms).
 * See bootreason.h for the register details.
 */
#include "bootreason.h"
#include "psys.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *out = NULL;
	enum power_boot_reason r;
	int reg00 = -1, reg01 = -1, ret, opt;
	bool verbose = false;
	char line[16];

	while ((opt = getopt(argc, argv, "vw:h")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		case 'w':
			out = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-v] [-w FILE]\n", argv[0]);
			return 2;
		}
	}

	ret = bootreason_read_axp("/sys", "/dev", &reg00, &reg01);
	r = ret ? POWER_BOOT_UNKNOWN : bootreason_decode(reg00);
	if (verbose) {
		if (ret)
			fprintf(stderr, "rsos-bootreason: AXP209 read failed: %s\n", strerror(-ret));
		else
			fprintf(stderr, "rsos-bootreason: REG00=0x%02x REG01=0x%02x "
				"(ACIN %s, boot source %s)\n", reg00, reg01,
				(reg00 & AXP209_REG00_ACIN_PRESENT) ? "present" : "absent",
				(reg00 & AXP209_REG00_BOOT_ACIN) ? "ACIN/VBUS" : "power key");
	}
	snprintf(line, sizeof(line), "%s\n", bootreason_name(r));
	fputs(line, stdout);
	if (out) {
		ret = psys_write_atomic(out, line, strlen(line));
		if (ret)
			fprintf(stderr, "rsos-bootreason: %s: %s\n", out, strerror(-ret));
	}
	return r == POWER_BOOT_CHARGER ? 10 : r == POWER_BOOT_KEY ? 0 : 1;
}
