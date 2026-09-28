#!/bin/sh
# Functional test of the A/B logic of board/sbc-uboot/boot.cmd.in (the boot
# script of the Orange Pi ports) on QEMU's "cubieboard" machine (Allwinner
# A10: the sunxi U-Boot family of the H3 boards). The same method as
# board/retrostone2/tests/boot-ab-qemu-test.sh: U-Boot is built for
# Cubieboard with board/sbc-uboot/uboot.fragment and the H3 fragment
# (environment at 1 MiB, watchdog), the kernels are fake (a zImage starting
# with "GOOD" is reported as started and resets: a kernel that never
# confirms; anything else makes bootz fail), and "confirming" a boot is a
# fw_setenv on the card image between runs, as rsos-boot-ok does.
# Run as root in WSL (needs qemu-system-arm, arm-linux-gnueabihf-gcc, the
# U-Boot tarball in ~/rsos/dl and the host tools of a build: mkimage and
# fw_printenv, from ~/rsos/output/host by default, RSOS_HOST_DIR otherwise):
#   sh buildroot-external/board/sbc-uboot/tests/boot-ab-qemu-test.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
T=$(cd "$(dirname "$0")/.." && pwd)
H3=$T/../orangepi-h3
W=$HOME/rsos/qemu-ab-sbc
H=${RSOS_HOST_DIR:-$HOME/rsos/output/host}
mkdir -p "$W"
cd "$W" || exit 1

if [ ! -f u-boot/u-boot-sunxi-with-spl.bin ] || [ "$T/uboot.fragment" -nt u-boot/u-boot-sunxi-with-spl.bin ] ||
	[ "$H3/uboot.fragment" -nt u-boot/u-boot-sunxi-with-spl.bin ]; then
	rm -rf u-boot u-boot-2026.07
	tar xf ~/rsos/dl/uboot/u-boot-2026.07.tar.bz2 && mv u-boot-2026.07 u-boot
	cd u-boot || exit 1
	export CROSS_COMPILE=arm-linux-gnueabihf-
	PATH=$H/bin:$PATH make -s Cubieboard_defconfig
	./scripts/kconfig/merge_config.sh -m .config "$T/uboot.fragment" "$H3/uboot.fragment" > /dev/null
	PATH=$H/bin:$PATH make -s olddefconfig
	grep -E 'WATCHDOG_AUTOSTART|CMD_SETEXPR|ENV_OFFSET' .config
	PATH=$H/bin:$PATH make -s -j16 > ../build.log 2>&1 || { tail -20 ../build.log; exit 1; }
	cd "$W" || exit 1
fi
# The device tree U-Boot's fdtfile names (the Cubieboard's own)
FDT=$(strings u-boot/u-boot-sunxi-with-spl.bin | sed -n 's/^fdtfile=//p' | head -n 1)
[ -n "$FDT" ] || { echo "no fdtfile in U-Boot"; exit 1; }

cat > fw_env.config <<EOF
$W/sd.img 0x100000 0x10000
$W/sd.img 0x110000 0x10000
EOF
env_get() { "$H/bin/fw_printenv" -c fw_env.config -n "$1" 2> /dev/null; }
env_set() { "$H/bin/fw_setenv" -c fw_env.config "$@"; }
confirm() { printf 'rsos_ok 1\nrsos_tries 0\nrsos_fails 0\nrsos_fallback\n' | "$H/bin/fw_setenv" -c fw_env.config -s -; }
state() { echo "slot=$(env_get rsos_slot) ok=$(env_get rsos_ok) tries=$(env_get rsos_tries) fails=$(env_get rsos_fails) fallback=$(env_get rsos_fallback)"; }

# boot.scr: the template filled in as mk-boot-scr.sh does for the H3 boards,
# with the fake-kernel check before bootz
mkdir -p scr
HOST_DIR=$H BUILD_DIR=$W RSOS_KERNEL=zImage RSOS_BOOTM=bootz RSOS_MMC=0 RSOS_DISK=mmcblk0 \
	RSOS_CONSOLE="console=ttyS0,115200" RSOS_FDT_DEFAULT="$FDT" sh "$T/mk-boot-scr.sh" scr > /dev/null || exit 1
sed 's|^\([[:space:]]*\)bootz \${kernel_addr_r} - \${fdt_addr_r}$|\1setexpr.b rsos_t *${kernel_addr_r}\
\1if test "${rsos_t}" = "47"; then echo "=== KERNEL slot ${rsos_slot}: ${bootargs} fdt=${l_fdt}"; reset; fi\
&|' scr/boot/boot.cmd > boot.cmd.test
grep -q '=== KERNEL' boot.cmd.test || { echo "could not patch boot.cmd"; exit 1; }
"$H/bin/mkimage" -A arm -O linux -T script -C none -n test -d boot.cmd.test boot.scr > /dev/null

# make_slot <dir> good|bad
make_slot() {
	rm -rf "$1"; mkdir -p "$1/boot"
	cp boot.scr "$1/boot/"
	mkdir -p "$(dirname "$1/boot/$FDT")"
	cp u-boot/u-boot.dtb "$1/boot/$FDT"
	cp u-boot/u-boot.dtb "$1/boot/other.dtb"
	case $2 in
	good) { echo GOOD; head -c 100000 /dev/zero; } > "$1/boot/zImage" ;;
	bad) head -c 100000 /dev/urandom | tr 'G' 'x' > "$1/boot/zImage" ;;
	esac
}
# make_card <slot a: good|bad> <slot b: good|bad|empty>: fresh card, zeroed
# environment; the data partition is MBR entry 1 (physically last)
make_card() {
	make_slot ra "$1"; make_slot rb "$2"
	rm -f sd.img; truncate -s 64M sd.img
	printf 'label: dos\nstart=90112, size=40960, type=c\nstart=8192, size=40960, type=83\nstart=49152, size=40960, type=83\n' | sfdisk -q sd.img
	truncate -s 20M pa.img pb.img
	mkfs.ext4 -q -F -O ^64bit,^has_journal,^orphan_file -b 4096 -d ra pa.img
	dd if=pa.img of=sd.img bs=512 seek=8192 conv=notrunc status=none
	if [ "$2" != empty ]; then
		mkfs.ext4 -q -F -O ^64bit,^has_journal,^orphan_file -b 4096 -d rb pb.img
		dd if=pb.img of=sd.img bs=512 seek=49152 conv=notrunc status=none
	fi
	dd if=u-boot/u-boot-sunxi-with-spl.bin of=sd.img bs=1024 seek=8 conv=notrunc status=none
	rm -f pa.img pb.img
}
boot() {
	timeout "${RSOS_QEMU_TIMEOUT:-120}" qemu-system-arm -M cubieboard -nographic -no-reboot \
		-drive if=sd,format=raw,file=sd.img < /dev/null 2>&1 | tr -d '\r' > out.txt
	grep -E '^rsos:|^=== KERNEL|poweroff|Saving Environment' out.txt | sed 's/^/     | /'
	echo "     > $(state)"
}
pass=0; fail=0
check() { if eval "$2"; then echo "  ok: $1"; pass=$((pass+1)); else echo "  FAIL: $1"; fail=$((fail+1)); fi; }
started() { grep -q "^=== KERNEL slot $1:.* rsos.boot=$2" out.txt; }

echo "== 1: fresh card, slot a; every boot counted until confirmed"
make_card good empty
boot
check "first boot: slot a started, pending" "started a pending"
check "defaults saved, fails=1" "[ \"\$(state)\" = 'slot=a ok=1 tries=0 fails=1 fallback=' ]"
check "root=/dev/mmcblk0p2 and U-Boot's device tree" "grep -q '^=== KERNEL slot a: console=ttyS0,115200 root=/dev/mmcblk0p2 .* fdt=$FDT' out.txt"
confirm
boot
check "second boot: counted again (fails=1), started" "started a pending && [ \"\$(env_get rsos_fails)\" = 1 ]"

echo "== 2: confirmed slot a panics 3 times, slot b has a kernel: fallback to b"
make_card good good
boot; confirm
boot; boot; boot
check "3 unconfirmed boots: fails=3, still slot a" "started a pending && [ \"\$(env_get rsos_fails)\" = 3 ] && [ \"\$(env_get rsos_slot)\" = a ]"
boot
check "4th boot: falls back to slot b (root=/dev/mmcblk0p3)" "grep -q 'falling back to slot b' out.txt && started b pending && grep -q 'root=/dev/mmcblk0p3' out.txt"
check "state: slot=b ok=1 fails=1 fallback=a" "[ \"\$(state)\" = 'slot=b ok=1 tries=0 fails=1 fallback=a' ]"

echo "== 3: ... and slot b panics too: no ping-pong, stay on b"
boot; boot
boot
check "b fails 3 times: stays on b" "grep -q 'staying on b' out.txt && started b pending && [ \"\$(env_get rsos_slot)\" = b ]"
confirm
check "rsos-boot-ok clears the fallback" "[ \"\$(state)\" = 'slot=b ok=1 tries=0 fails=0 fallback=' ]"

echo "== 4: confirmed slot a panics, slot b empty (factory card): stay on a"
make_card good empty
boot; confirm; boot; boot; boot
boot
check "stays on a (b has no kernel)" "grep -q 'slot b has no kernel' out.txt && started a pending"

echo "== 5: slot a with a corrupt kernel, slot b good: switch at once"
make_card bad good
boot
check "bootz fails on a: switching to b, reset" "grep -q 'switching to slot b' out.txt && ! grep -q '=== KERNEL' out.txt"
boot
check "next boot starts b" "started b pending"

echo "== 6: both kernels corrupt: power off"
make_card bad bad
boot
check "a fails, switch to b" "grep -q 'switching to slot b' out.txt"
boot
check "b fails too: powering off" "grep -q 'no slot can be booted, powering off' out.txt"

echo "== 7: updated slot on trial (ok=0 tries=3) that never confirms"
make_card good good
boot; confirm
printf 'rsos_slot b\nrsos_ok 0\nrsos_tries 3\nrsos_fails 0\nrsos_fallback\n' | "$H/bin/fw_setenv" -c fw_env.config -s -
boot; boot; boot
check "3 trial boots of b (tries 2, 1, 0)" "started b pending && [ \"\$(env_get rsos_tries)\" = 0 ]"
boot
check "4th boot: back to a, fallback=b" "started a pending && [ \"\$(state)\" = 'slot=a ok=1 tries=0 fails=1 fallback=b' ]"

echo "== 8: rsos_maxfails=0: a confirmed slot is not counted (no environment write)"
make_card good empty
boot; confirm
env_set rsos_maxfails 0
boot
check "started with rsos.boot=ok, nothing saved" "started a ok && ! grep -q 'Saving Environment' out.txt"

echo "== 9: rsos_fdtfile selects another device tree (a sister board)"
env_set rsos_fdtfile other.dtb
boot
check "fdt=other.dtb" "grep -q '^=== KERNEL slot a: .* fdt=other.dtb' out.txt"
env_set rsos_fdtfile missing.dtb
boot
check "a missing device tree falls back to the board's own" "grep -q 'rsos: no /boot/missing.dtb' out.txt && grep -q '^=== KERNEL slot a: .* fdt=$FDT' out.txt"

echo "== 10: the hardware watchdog runs from U-Boot on"
check "U-Boot started the watchdog" "grep -qi 'watchdog\|wdt' out.txt"

echo "PASS=$pass FAIL=$fail"
[ $fail -eq 0 ]
