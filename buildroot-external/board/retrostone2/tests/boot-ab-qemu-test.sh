#!/bin/sh
# Functional test of the A/B logic of board/retrostone2/boot.cmd on QEMU's
# "cubieboard" machine (Allwinner A10: same sunxi U-Boot family, AXP209,
# sunxi MMC and watchdog). Run as root in WSL (needs qemu-system-arm,
# arm-linux-gnueabihf-gcc, the U-Boot tarball in ~/rsos/dl and the host
# tools of ~/rsos/output):
#   sh buildroot-external/board/retrostone2/tests/boot-ab-qemu-test.sh
# U-Boot is built for Cubieboard with this tree's patches and uboot.fragment
# (work dir ~/rsos/qemu-ab). Each QEMU run is one boot. The kernels are fake:
# a "zImage" starting with "GOOD" is reported as started ("=== KERNEL slot
# <x> ...", then reset: a kernel that never confirms, i.e. a panic or a hang),
# anything else makes the real bootz fail. "Confirming" a boot is done
# between runs with fw_setenv on the card image, as rsos-boot-ok would.
# RSOS_QEMU_AB_DIR: another work dir (default ~/rsos/qemu-ab).
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
BR=$(cd "$(dirname "$0")/.." && pwd)
W=${RSOS_QEMU_AB_DIR:-$HOME/rsos/qemu-ab}
H=$HOME/rsos/output/host
mkdir -p "$W"
cd "$W" || exit 1

if [ ! -f u-boot/u-boot-sunxi-with-spl.bin ] || [ "$BR/uboot.fragment" -nt u-boot/u-boot-sunxi-with-spl.bin ]; then
	rm -rf u-boot u-boot-2026.07
	tar xf ~/rsos/dl/uboot/u-boot-2026.07.tar.bz2 && mv u-boot-2026.07 u-boot
	cd u-boot || exit 1
	for p in "$BR"/patches/uboot/*.patch; do patch -p1 -s < "$p"; done
	# The A8 has no generic timer: give sun4i a bootstage clock (test only)
	cat >> arch/arm/mach-sunxi/timer.c <<'EOT'
#if CONFIG_IS_ENABLED(BOOTSTAGE) && defined(CONFIG_MACH_SUN4I)
ulong timer_get_boot_us(void) { return timer_get_us(); }
#endif
EOT
	export CROSS_COMPILE=arm-linux-gnueabihf-
	PATH=$H/bin:$PATH make -s Cubieboard_defconfig
	./scripts/kconfig/merge_config.sh -m .config "$BR/uboot.fragment" > /dev/null
	PATH=$H/bin:$PATH make -s olddefconfig
	grep -E 'WATCHDOG_AUTOSTART|CMD_POWEROFF|CMD_FS_GENERIC' .config
	PATH=$H/bin:$PATH make -s -j16 > ../build.log 2>&1 || { tail -20 ../build.log; exit 1; }
	cd "$W" || exit 1
fi

cat > fw_env.config <<EOF
$W/sd.img 0x100000 0x10000
$W/sd.img 0x110000 0x10000
EOF
env_get() { "$H/bin/fw_printenv" -c fw_env.config -n "$1" 2> /dev/null; }
env_set() { "$H/bin/fw_setenv" -c fw_env.config "$@"; }
# confirm: what rsos-boot-ok writes once the running slot is stable
confirm() {
	# (read before the pipe: fw_printenv would wait for fw_setenv's lock)
	_s=$(env_get rsos_slot)
	_lv=$(env_get rsos_lowvolt)
	{
		printf 'rsos_ok 1\nrsos_tries 0\nrsos_fails 0\nrsos_fallback\nrsos_rounds\nrsos_good %s\n' "$_s"
		[ "$_lv" != trial ] || echo rsos_lowvolt
	} | "$H/bin/fw_setenv" -c fw_env.config -s -
}
# update <slot>: what the updater writes once the slot holds a new system
update() { printf 'rsos_slot %s\nrsos_ok 0\nrsos_tries 3\nrsos_fails 0\nrsos_fallback\nrsos_bad\nrsos_rounds\n' "$1" | "$H/bin/fw_setenv" -c fw_env.config -s -; }
state() { echo "slot=$(env_get rsos_slot) ok=$(env_get rsos_ok) tries=$(env_get rsos_tries) fails=$(env_get rsos_fails) fallback=$(env_get rsos_fallback)"; }
state2() { echo "bad=$(env_get rsos_bad) rounds=$(env_get rsos_rounds) good=$(env_get rsos_good)"; }

# boot.scr for the test: boot.cmd with bootz preceded by the fake-kernel check
sed 's|^\([[:space:]]*\)bootz \${kernel_addr_r} - \${fdt_addr_r}$|\1setexpr.b rsos_t *${kernel_addr_r}\
\1if test "${rsos_t}" = "47"; then echo "=== KERNEL slot ${rsos_slot}: ${bootargs}"; reset; fi\
&|' "$BR/boot.cmd" > boot.cmd.body
# (console on: bootcmd sets "silent" before it sources boot.scr)
{ echo "setenv silent"; cat boot.cmd.body; } > boot.cmd.test
grep -q '=== KERNEL' boot.cmd.test || { echo "could not patch boot.cmd"; exit 1; }
"$H/bin/mkimage" -A arm -O linux -T script -C none -n test -d boot.cmd.test boot.scr > /dev/null

# make_slot <dir> good|bad|none
make_slot() {
	rm -rf "$1"; mkdir -p "$1/boot"
	cp boot.scr "$1/boot/"
	cp "$HOME/rsos/output/images/sun7i-a20-retrostone2.dtb" "$1/boot/"
	mkdir -p "$1/boot/overlays"
	cp "$HOME/rsos/output/target/boot/overlays/emmc.dtbo" "$HOME/rsos/output/target/boot/overlays/cpu-lowvolt.dtbo" \
		"$1/boot/overlays/"
	case $2 in
	good) { echo GOOD; head -c 100000 /dev/zero; } > "$1/boot/zImage" ;;
	bad) head -c 100000 /dev/urandom | tr 'G' 'x' > "$1/boot/zImage" ;;
	noscr) rm -f "$1/boot/boot.scr" ;;
	esac
}
# make_card <slot a: good|bad|none|noscr> <slot b: good|bad|none|empty>: fresh card, zeroed environment
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
# boot: one power-on; the console is in $W/out.txt, the rsos lines are shown
boot() {

	timeout 40 qemu-system-arm -M cubieboard -nographic -no-reboot \
		-drive if=sd,format=raw,file=sd.img < /dev/null 2>&1 | tr -d '\r' > out.txt
	grep -E '^rsos:|^=== KERNEL|poweroff|resetting|Saving Environment' out.txt | sed 's/^/     | /'
	echo "     > $(state) $(state2)"
}
# boots <n>: n power-ons
boots() { _n=$1; while [ "$_n" -gt 0 ]; do boot; _n=$((_n - 1)); done; }
no_kernel() { ! grep -q '^=== KERNEL' out.txt; }
pass=0; fail=0
check() { if eval "$2"; then echo "  ok: $1"; pass=$((pass+1)); else echo "  FAIL: $1"; fail=$((fail+1)); fi; }
started() { grep -q "^=== KERNEL slot $1:.* rsos.boot=$2" out.txt; }
sawb=0
# boot, and remember whether slot b ever started
bootw() { boot; ! grep -q '^=== KERNEL slot b' out.txt || sawb=1; }

echo "== 1: fresh card, slot a; every boot counted until confirmed"
make_card good empty
boot
check "first boot: slot a started, pending" "started a pending"
check "defaults saved, fails=1" "[ \"\$(state)\" = 'slot=a ok=1 tries=0 fails=1 fallback=' ]"
confirm
boot
check "second boot: counted again (fails=1), started" "started a pending && [ \"\$(env_get rsos_fails)\" = 1 ]"

echo "== 2: confirmed slot a panics 3 times, slot b has a kernel: fallback to b"
make_card good good
boot; confirm
boot; boot; boot
check "3 unconfirmed boots: fails=3, still slot a" "started a pending && [ \"\$(env_get rsos_fails)\" = 3 ] && [ \"\$(env_get rsos_slot)\" = a ]"
boot
check "4th boot: falls back to slot b" "grep -q 'falling back to slot b' out.txt && started b pending"
check "state: slot=b ok=1 fails=1 fallback=a, round 1" "[ \"\$(state) \$(state2)\" = 'slot=b ok=1 tries=0 fails=1 fallback=a bad= rounds=1 good=a' ]"

echo "== 3: ... and slot b panics too: back to the last confirmed slot, then power off (failure budget)"
boot; boot
boot
check "b fails 3 times: back to a (rsos_good), round 2" "grep -q 'falling back to slot a' out.txt && started a pending && [ \"\$(env_get rsos_rounds)\" = 2 ] && [ \"\$(env_get rsos_fallback)\" = b ]"
boot; boot
boot
check "a fails again: both failed, stays on a (the last confirmed), round 3" "grep -q 'staying on a' out.txt && started a pending && [ \"\$(env_get rsos_rounds)\" = 3 ]"
boot; boot
boot
check "round 4: powers off (then resets: QEMU's AXP209 has no power-off), no kernel" \
	"grep -q 'no slot starts, powering off (next power-on: slot a)' out.txt && grep -q 'poweroff ...' out.txt && grep -q 'resetting' out.txt && no_kernel"
check "a fresh budget for the next power-on" "[ \"\$(state) \$(state2)\" = 'slot=a ok=1 tries=0 fails=0 fallback= bad= rounds=0 good=a' ]"
boot
check "next power-on: slot a starts (counted)" "started a pending"
confirm
check "rsos-boot-ok clears the fallback and the rounds" "[ \"\$(state) \$(state2)\" = 'slot=a ok=1 tries=0 fails=0 fallback= bad= rounds= good=a' ]"

echo "== 4: confirmed slot a panics, slot b empty (factory card): stay on a"
make_card good empty
boot; confirm; boot; boot; boot
boot
check "stays on a (b has no kernel)" "grep -q 'slot b has no kernel' out.txt && started a pending"

echo "== 5: confirmed slot a with a corrupt kernel, slot b good: switch at once"
make_card bad good
boot
check "bootz fails on a: switching to b, reset" "grep -q 'switching to slot b' out.txt && no_kernel"
check "state: slot=b fallback=a" "[ \"\$(env_get rsos_slot)\" = b ] && [ \"\$(env_get rsos_fallback)\" = a ]"
boot
check "next boot starts b" "started b pending"

echo "== 5b (review): unloadable slot a with rsos_fallback=b: still switches to the good slot b"
env_set rsos_slot a
env_set rsos_fallback b
boot
check "no power-off: switching to b" "grep -q 'switching to slot b' out.txt && ! grep -q 'powering off' out.txt"
boot
check "b starts" "started b pending"

echo "== 6: both kernels corrupt: a few switches (the budget), then power off"
make_card bad bad
boot
check "a fails, switch to b" "grep -q 'switching to slot b' out.txt"
boot
check "b fails, switch to a (round 2)" "grep -q 'switching to slot a' out.txt && [ \"\$(env_get rsos_rounds)\" = 2 ]"
boot; boot
# (QEMU's AXP209 has no power-off: U-Boot's poweroff returns there, and the
# script resets)
check "round 4: powering off, then reset" "grep -q 'no slot can be booted, powering off' out.txt && grep -q 'poweroff ...' out.txt && grep -q 'resetting' out.txt"
check "fresh budget for the next power-on" "[ \"\$(env_get rsos_rounds)\" = 0 ]"

echo "== 7: rsos_slot=b set by hand while slot b is empty: back to a"
make_card good empty
boot; confirm
env_set rsos_slot b
boot
check "load fails on b, switches to a" "grep -q 'switching to slot a' out.txt"
boot
check "slot a starts" "started a pending"

echo "== 8: updated slot on trial (ok=0 tries=3) that never confirms: marked bad"
make_card good good
boot; confirm
update b
boot; boot; boot
check "3 trial boots of b (tries 2, 1, 0)" "started b pending && [ \"\$(env_get rsos_tries)\" = 0 ]"
boot
check "4th boot: back to a, fallback=b, b marked bad" \
	"started a pending && grep -q 'marking it bad' out.txt && [ \"\$(state) \$(state2)\" = 'slot=a ok=1 tries=0 fails=1 fallback=b bad=b rounds=1 good=a' ]"

echo "== 8b (review): ... then the good slot a panics: never back to the bad slot b; power off after the budget"
sawb=0
bootw; bootw
bootw
check "a fails 3 times: b is marked bad, stays on a (round 2)" "grep -q 'slot b is marked bad: staying on a' out.txt && started a pending && [ \"\$(env_get rsos_rounds)\" = 2 ]"
bootw; bootw; bootw
bootw; bootw; bootw
check "round 4: powers off, next power-on on a" "grep -q 'no slot starts, powering off (next power-on: slot a)' out.txt && [ \"\$(env_get rsos_slot)\" = a ]"
check "slot b never started, and is still marked bad" "[ $sawb = 0 ] && [ \"\$(env_get rsos_bad)\" = b ]"
confirm
check "a confirmation does not clear the bad mark" "[ \"\$(env_get rsos_bad)\" = b ]"

echo "== 8c: the updater writes slot b again: the mark goes"
update b
boot
check "b on trial again" "grep -q 'trying unconfirmed slot b' out.txt && started b pending && [ -z \"\$(env_get rsos_bad)\" ]"

echo "== 9: rsos_maxfails=0: a confirmed slot is not counted (no environment write)"
make_card good empty
boot; confirm
env_set rsos_maxfails 0
boot
check "started with rsos.boot=ok, nothing saved" "started a ok && ! grep -q 'Saving Environment' out.txt && [ \"\$(env_get rsos_fails)\" = 0 ]"

echo "== 10: device tree overlays (rsos_overlays, the l_ov loop)"
env_set rsos_overlays emmc
boot
check "overlay emmc loaded and applied" "grep -q 'rsos: overlay emmc applied' out.txt && started a ok"

echo "== 11: the hardware watchdog runs from U-Boot on"
check "U-Boot started the watchdog" "grep -qi 'watchdog\|wdt' out.txt"

echo "== 12 (review): the cpu-lowvolt overlay is one-shot until a boot with it is confirmed"
env_set rsos_overlays "emmc cpu-lowvolt"
boot
check "applied, rsos_lowvolt=trial" "grep -q 'rsos: overlay cpu-lowvolt applied' out.txt && [ \"\$(env_get rsos_lowvolt)\" = trial ] && started a ok"
boot
check "the boot was not confirmed (a freeze, a watchdog reset): skipped, failed" \
	"grep -q 'cpu-lowvolt was not confirmed: skipping' out.txt && ! grep -q 'overlay cpu-lowvolt applied' out.txt && grep -q 'overlay emmc applied' out.txt && [ \"\$(env_get rsos_lowvolt)\" = failed ]"
boot
check "still skipped" "grep -q 'cpu-lowvolt skipped' out.txt && ! grep -q 'overlay cpu-lowvolt applied' out.txt && started a ok"
env_set rsos_lowvolt
boot
confirm
check "tried again by hand, then confirmed: the trial mark goes" "grep -q 'overlay cpu-lowvolt applied' out.txt && [ -z \"\$(env_get rsos_lowvolt)\" ]"
boot
check "a confirmed boot keeps it: applied again" "grep -q 'overlay cpu-lowvolt applied' out.txt && [ \"\$(env_get rsos_lowvolt)\" = trial ]"

echo "== 13 (review): no boot.scr at all: the bootcmd ends with a power-off, not the U-Boot prompt"
make_card noscr empty
boot
check "bootflow scan, then poweroff" "grep -q 'poweroff ...' out.txt && no_kernel"

echo "PASS=$pass FAIL=$fail"
[ $fail -eq 0 ]

