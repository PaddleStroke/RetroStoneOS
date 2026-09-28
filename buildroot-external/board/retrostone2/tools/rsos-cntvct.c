/*
 * rsos-cntvct - read the ARM generic timer counter from user space.
 *
 * The counter (24 MHz on the A20) runs from power-on. U-Boot's bootstage
 * marks are taken from it (patches/uboot/0001), and Linux uses it as its
 * clocksource, but the kernel's printk time 0 is some unknown count after
 * reset: the time spent in the boot ROM, SPL, U-Boot and the zImage
 * decompressor. With -k, this writes the current count to the kernel log;
 * the printk timestamp of that line and the count in it give the count at
 * kernel time 0, which puts everything on one time line (the boot logger does
 * the arithmetic for bootstage.txt).
 *
 * Output (stdout, and with -k also /dev/kmsg):
 *   rsos-cntvct: <count> ticks at <frequency> Hz
 *
 * Linux enables user access to the virtual counter (CNTKCTL.PL0VCTEN) and
 * keeps CNTVOFF at 0, so CNTVCT reads the same value as CNTPCT.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static inline uint64_t read_cntvct(void)
{
	uint64_t v;

	__asm__ volatile("isb\n\tmrrc p15, 1, %Q0, %R0, c14" : "=r"(v));
	return v;
}

static inline uint32_t read_cntfrq(void)
{
	uint32_t f;

	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f));
	return f;
}

int main(int argc, char **argv)
{
	char line[96];
	int n, fd = -1;
	uint32_t freq = read_cntfrq();

	if (argc > 1 && strcmp(argv[1], "-k") == 0) {
		fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			perror("rsos-cntvct: /dev/kmsg");
	} else if (argc > 1) {
		fprintf(stderr, "usage: rsos-cntvct [-k]\n");
		return 2;
	}
	/* Read the counter right before the write: printk stamps the line
	 * within a few microseconds. */
	n = snprintf(line, sizeof(line), "rsos-cntvct: %llu ticks at %u Hz\n",
		     (unsigned long long)read_cntvct(), freq);
	if (fd >= 0) {
		if (write(fd, line, n) != n)
			perror("rsos-cntvct: write /dev/kmsg");
		close(fd);
	}
	fputs(line, stdout);
	return fd >= 0 || argc == 1 ? 0 : 1;
}
