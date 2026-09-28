/*
 * bootreason.h - why did the AXP209 power the board on?
 *
 * AXP209 REG00 (power input status), bit 0: "boot source is ACIN/VBUS".
 * It is 1 when the PMIC started because a charger was plugged in, and 0
 * when it started from the power key (PEK). The 6.18 axp20x drivers do not
 * expose this bit, so it is read over /dev/i2c-N with an I2C_RDWR
 * transfer (which works while axp20x-i2c owns the address; the adapter
 * lock serialises it with the kernel's own accesses, and REG00 is a
 * read-only status register, so a read cannot disturb the driver).
 * REG01 (power mode / charge status) is read for the log only.
 * TODO(hw): confirm bit 0 on the unit: charger plugged while off -> 1,
 * power key while off (charger plugged or not) -> 0.
 */
#ifndef RSOS_POWER_BOOTREASON_H
#define RSOS_POWER_BOOTREASON_H

enum power_boot_reason {
	POWER_BOOT_UNKNOWN = 0,   /* read failed: treat as a normal boot */
	POWER_BOOT_KEY,           /* power key: normal boot */
	POWER_BOOT_CHARGER,       /* charger insertion: charge mode */
};

#define AXP209_I2C_ADDR 0x34
#define AXP209_REG00_ACIN_PRESENT 0x80
#define AXP209_REG00_ACIN_USABLE  0x40
#define AXP209_REG00_VBUS_PRESENT 0x20
#define AXP209_REG00_BOOT_ACIN    0x01

/* Pure decode of REG00. */
enum power_boot_reason bootreason_decode(int reg00);

/*
 * Finds the AXP209 bus (<sysfs>/bus/i2c/devices/N-0034) and reads REG00 and
 * REG01 through <dev_dir>/i2c-N. 0 or -errno.
 */
int bootreason_read_axp(const char *sysfs, const char *dev_dir,
			int *reg00, int *reg01);

const char *bootreason_name(enum power_boot_reason r);    /* "key", "charger", "unknown" */
enum power_boot_reason bootreason_parse(const char *s);   /* inverse, UNKNOWN otherwise */

#endif
