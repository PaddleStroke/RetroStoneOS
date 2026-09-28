#!/bin/sh
# End-to-end test of the system updater (rsos-update, docs/updates.md) on
# the real RetroStone2 image, as root in WSL after a build (loop devices):
#   sh buildroot-external/board/retrostone2/tests/update-ab-test.sh
#
# 1. The image (~/rsos/output/images/sdcard.img) on a loop device, with a
#    U-Boot environment as its first boot leaves it (slot a, confirmed,
#    counted). Version B = its rootfs.ext4 with another version and a marker
#    file, packed as a signed .rsu (a test key: --pubkey).
#    - a package signed by another key: refused, nothing written;
#    - a "power cut" in the middle of the write (RSOS_UPDATE_TEST_ABORT): the
#      environment is untouched (slot a boots), slot b has no superblock;
#    - the install: the environment reads rsos_slot=b rsos_ok=0 rsos_tries=3
#      rsos_fails=0 (host fw_printenv on the card), slot b mounts and holds
#      version B, byte for byte;
#    - after the "restart" into b: "updated", then "confirmed" (clean-up).
#    The running slot (a) and the data partition are compared before/after.
# 2. With QEMU (the U-Boot of boot-ab-qemu-test.sh, ~/rsos/qemu-ab, if it
#    was built): a card with fake kernels; rsos-update installs slot b; U-Boot
#    then boots slot b on trial (rsos.boot=pending, tries 3 -> 2); after an
#    interrupted install U-Boot still boots slot a.
# Needs: the build (~/rsos/output: images, host fw_printenv/mkenvimage),
# gcc + libzstd-dev (+ libmbedtls-dev), zstd, e2fsprogs, losetup.
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
O=$HOME/rsos/output
H=$O/host/bin
W=$HOME/rsos/update-ab-test
[ "$(id -u)" = 0 ] || { echo "update-ab-test: run as root (loop devices)"; exit 1; }
for f in "$O/images/sdcard.img" "$O/images/rootfs.ext4" "$H/fw_printenv" "$H/mkenvimage"; do
	[ -e "$f" ] || { echo "update-ab-test: $f missing (build first)"; exit 1; }
done
command -v zstd > /dev/null || { echo "update-ab-test: zstd missing"; exit 1; }

pass=0
fail=0
check() { if eval "$2"; then echo "  ok: $1"; pass=$((pass + 1)); else echo "  FAIL: $1"; fail=$((fail + 1)); fi; }
LOOPS=
cleanup() {
	for m in "$W"/mnt*; do mountpoint -q "$m" 2> /dev/null && umount "$m"; done
	for l in $LOOPS; do losetup -d "$l" 2> /dev/null; done
}
trap cleanup EXIT
rm -rf "$W" && mkdir -p "$W"

echo "== build the host rsos-update and rsos-mkupdate"
make -s -C "$REPO/frontend" BUILDDIR="$W/fe" "$W/fe/rsos-update" "$W/fe/build-tools/rsos-mkupdate" \
	> "$W/build.log" 2>&1 || { tail -20 "$W/build.log"; exit 1; }
UPD=$W/fe/rsos-update
MK=$W/fe/build-tools/rsos-mkupdate
"$MK" keygen -s "$W/test.key" -p "$W/test.pub" > /dev/null
"$MK" keygen -s "$W/other.key" -p "$W/other.pub" > /dev/null

# version B: the same root file system, another version and a marker
make_version_b() { # <image> <version>
	cp --sparse=always "$O/images/rootfs.ext4" "$1"
	mkdir -p "$W/mntb"
	mount -o loop "$1" "$W/mntb" || return 1
	# (an image older than the updater has no version.env: the one of A)
	[ -f "$W/mntb/etc/rsos/version.env" ] || cp "$R/etc/rsos/version.env" "$W/mntb/etc/rsos/version.env"
	sed -i "s/^RSOS_VERSION=.*/RSOS_VERSION='$2'/; s/^RSOS_BUILD_TIME=.*/RSOS_BUILD_TIME='$(date +%s)'/" \
		"$W/mntb/etc/rsos/version.env"
	echo "update-ab-test $2" > "$W/mntb/etc/rsos-update-test"
	umount "$W/mntb"
	e2fsck -fn "$1" > /dev/null 2>&1
}
pack() { # <image> <out.rsu> <key>
	zstd -q -19 -T0 -f -c "$1" > "$W/payload.zst"
	(
		# shellcheck disable=SC1090,SC1091
		. "$W/vb.env"
		"$MK" pack -o "$2" --image "$1" --payload "$W/payload.zst" --board "$RSOS_BOARD_ID" \
			--version "$RSOS_VERSION" --variant "$RSOS_VARIANT" --build-time "$RSOS_BUILD_TIME" \
			--changelog-file "$W/changes.txt" -s "$3" > /dev/null
	)
}

echo "== 1. the RetroStone2 image on a loop device"
cp --sparse=always "$O/images/sdcard.img" "$W/card.img"
L=$(losetup -f --show -P "$W/card.img") || exit 1
LOOPS="$L"
sleep 1
[ -b "${L}p3" ] || { echo "no partitions on $L"; exit 1; }
# the environment of a first boot: slot a, confirmed, this boot counted
printf '%s 0x100000 0x10000\n%s 0x110000 0x10000\n' "$L" "$L" > "$W/fw_env.config"
printf 'rsos_slot=a\nrsos_ok=1\nrsos_tries=0\nrsos_fails=1\nbootdelay=0\n' > "$W/env.txt"
"$H/mkenvimage" -r -s 0x10000 -o "$W/env.bin" "$W/env.txt"
dd if="$W/env.bin" of="$L" bs=1024 seek=1024 conv=notrunc status=none
dd if="$W/env.bin" of="$L" bs=1024 seek=1088 conv=notrunc status=none
envget() { "$H/fw_printenv" -c "$W/fw_env.config" -n "$1" 2> /dev/null; }
check "the environment reads slot a" "[ \"\$(envget rsos_slot)\" = a ]"

# the system as rsos-update sees it: --root for /etc, /proc/cmdline, /data, /run
R=$W/root
mkdir -p "$R/etc/rsos" "$R/proc" "$R/data/update" "$R/run/rsos" "$R/usr/share/rsos"
mkdir -p "$W/mnta"
mount -o ro "${L}p2" "$W/mnta"
if [ -f "$W/mnta/etc/rsos/version.env" ]; then
	cp "$W/mnta/etc/rsos/version.env" "$R/etc/rsos/version.env"
else
	echo "  (an image built before the updater: a version.env is made up)"
	printf "RSOS_VERSION='0.1-dev'\nRSOS_VARIANT='dev'\nRSOS_BUILD_TIME='1790000000'\nRSOS_BOARD_ID='retrostone2'\n" \
		> "$R/etc/rsos/version.env"
fi
cp "$W/mnta/etc/rsos/board.ini" "$R/etc/rsos/board.ini"
umount "$W/mnta"
cp "$W/fw_env.config" "$R/etc/fw_env.config"
echo "console=ttyS0,115200 root=${L}p2 rootfstype=ext4 rootwait ro rsos.slot=a rsos.boot=pending" > "$R/proc/cmdline"
U() { "$UPD" --root "$R" --pubkey "$W/test.pub" --fw-printenv "$H/fw_printenv" --fw-setenv "$H/fw_setenv" \
	--log "$W/update.log" --ignore-battery "$@"; }
# shellcheck disable=SC1091
. "$R/etc/rsos/version.env"
echo "  version A: $RSOS_VERSION ($RSOS_BOARD_ID)"
VB=$(echo "$RSOS_VERSION" | sed 's/-dev$//').1-test
make_version_b "$W/rootfs-b.ext4" "$VB" || { echo "cannot make version B"; exit 1; }
mount -o ro,loop "$W/rootfs-b.ext4" "$W/mntb" && cp "$W/mntb/etc/rsos/version.env" "$W/vb.env" && umount "$W/mntb"
printf -- '- update-ab-test: version %s\n' "$VB" > "$W/changes.txt"
pack "$W/rootfs-b.ext4" "$R/data/update/retrostoneos-$VB-$RSOS_BOARD_ID.rsu" "$W/test.key"
pack "$W/rootfs-b.ext4" "$W/badsig.rsu" "$W/other.key"
P=$R/data/update/retrostoneos-$VB-$RSOS_BOARD_ID.rsu
stat -c '  %n: %s bytes' "$P"
sum_a0=$(md5sum < "${L}p2")
sum_d0=$(md5sum < "${L}p1")
sum_b0=$(head -c 1048576 "${L}p3" | md5sum)

U --machine check --local-only > "$W/out"
check "check: version B found, installable" "grep -q \"^best.*version=$VB.*installable=1\" $W/out"

echo "-- a package signed by another key"
U --machine apply "$W/badsig.rsu" > "$W/out"
check "refused (badsig)" "grep -q '^error.*code=badsig' $W/out"
check "slot b untouched, environment untouched" \
	"[ \"\$(head -c 1048576 ${L}p3 | md5sum)\" = \"$sum_b0\" ] && [ \"\$(envget rsos_slot)\" = a ]"

echo "-- a power cut in the middle of the write (100 MiB of 512)"
RSOS_UPDATE_TEST_ABORT=write:104857600 U --machine apply "$P" > "$W/out"; rc=$?
check "the install stopped in the middle (exit $rc)" "[ $rc = 99 ] && grep -q '^progress.*phase=write' $W/out"
check "environment untouched: slot a, confirmed" \
	"[ \"\$(envget rsos_slot)\" = a ] && [ \"\$(envget rsos_ok)\" = 1 ] && [ \"\$(envget rsos_fails)\" = 1 ]"
check "slot b has no file system (U-Boot never falls back to it)" \
	"[ -z \"\$(head -c 65536 ${L}p3 | tr -d '\\000')\" ] && ! dumpe2fs -h ${L}p3 > /dev/null 2>&1"

echo "-- the install"
t0=$(date +%s)
U --machine apply "$P" > "$W/out"; rc=$?
echo "  ($(($(date +%s) - t0)) s on the build host)"
check "done: version B in slot b" "[ $rc = 0 ] && grep -q \"^done.*version=$VB.*slot=b\" $W/out"
check "phases: verify, write, readback, switch" "grep -q 'phase=verify' $W/out && grep -q 'phase=write' $W/out && \
	grep -q 'phase=readback' $W/out && grep -q 'phase=switch' $W/out"
check "environment: rsos_slot=b rsos_ok=0 rsos_tries=3 rsos_fails=0, no fallback" \
	"[ \"\$(envget rsos_slot)\" = b ] && [ \"\$(envget rsos_ok)\" = 0 ] && [ \"\$(envget rsos_tries)\" = 3 ] && \
	[ \"\$(envget rsos_fails)\" = 0 ] && [ -z \"\$(envget rsos_fallback)\" ]"
check "slot b is version B, byte for byte" "cmp -s -n $(stat -c %s "$W/rootfs-b.ext4") $W/rootfs-b.ext4 ${L}p3"
mkdir -p "$W/mntb3"
mount -o ro "${L}p3" "$W/mntb3"
check "slot b mounts: marker, version, kernel, boot script" \
	"grep -q \"update-ab-test $VB\" $W/mntb3/etc/rsos-update-test && grep -q \"RSOS_VERSION='$VB'\" $W/mntb3/etc/rsos/version.env && \
	[ -s $W/mntb3/boot/zImage ] && [ -s $W/mntb3/boot/boot.scr ]"
umount "$W/mntb3"
check "slot a (running) and the data partition unchanged" \
	"[ \"\$(md5sum < ${L}p2)\" = \"$sum_a0\" ] && [ \"\$(md5sum < ${L}p1)\" = \"$sum_d0\" ]"
check "the state file" "grep -q \"^version=$VB\$\" $R/data/rsos/update-state.ini && grep -q '^slot=b$' $R/data/rsos/update-state.ini"
check "boot before the restart: pending" "U --machine boot | grep -q '^boot.*event=pending'"

echo "-- the restart into slot b"
echo "console=ttyS0,115200 root=${L}p3 rootfstype=ext4 rootwait ro rsos.slot=b rsos.boot=pending" > "$R/proc/cmdline"
cp "$W/vb.env" "$R/etc/rsos/version.env"
echo pending > "$R/run/rsos/boot-state"
check "first start of version B: updated" "U --machine boot | grep -q \"^boot.*event=updated.*version=$VB\""
# rsos-boot-ok confirms (its one fw_setenv)
printf 'rsos_ok 1\nrsos_tries 0\nrsos_fails 0\nrsos_fallback\n' | "$H/fw_setenv" -c "$W/fw_env.config" -s -
echo confirmed > "$R/run/rsos/boot-state"
check "confirmed: clean-up" "U --machine boot | grep -q '^boot.*event=confirmed' && [ ! -e $R/data/rsos/update-state.ini ]"
check "now up to date" "U --machine check --local-only | grep -q '^local.*verdict=same'"
losetup -d "$L"
LOOPS=

echo "== 2. U-Boot (QEMU cubieboard) boots the updated slot"
Q=$HOME/rsos/qemu-ab
if [ ! -f "$Q/u-boot/u-boot-sunxi-with-spl.bin" ] || [ ! -f "$Q/boot.scr" ] || ! command -v qemu-system-arm > /dev/null; then
	echo "  skipped: run boot-ab-qemu-test.sh first (it builds U-Boot for QEMU in $Q)"
else
	# a slot: the test boot script and a fake kernel ("GOOD": U-Boot reports it started)
	mkslot() { # <dir> <image> <marker>
		rm -rf "$1"; mkdir -p "$1/boot/overlays"
		cp "$Q/boot.scr" "$1/boot/"
		cp "$O/images/sun7i-a20-retrostone2.dtb" "$1/boot/"
		{ echo GOOD; echo "$3"; head -c 100000 /dev/zero; } > "$1/boot/zImage"
		mkdir -p "$1/etc/rsos"
		cp "$R/etc/rsos/board.ini" "$1/etc/rsos/"
		truncate -s 20M "$2"
		mkfs.ext4 -q -F -O ^64bit,^has_journal,^orphan_file -b 4096 -d "$1" "$2"
	}
	mkslot "$W/qa" "$W/qa.img" slot-a
	mkslot "$W/qb" "$W/qb.img" slot-b-updated
	rm -f "$W/sd.img"; truncate -s 64M "$W/sd.img"
	printf 'label: dos\nstart=90112, size=40960, type=c\nstart=8192, size=40960, type=83\nstart=49152, size=40960, type=83\n' |
		sfdisk -q "$W/sd.img"
	dd if="$W/qa.img" of="$W/sd.img" bs=512 seek=8192 conv=notrunc status=none
	dd if="$Q/u-boot/u-boot-sunxi-with-spl.bin" of="$W/sd.img" bs=1024 seek=8 conv=notrunc status=none
	qboot() {
		timeout 40 qemu-system-arm -M cubieboard -nographic -no-reboot -drive if=sd,format=raw,file="$W/sd.img" \
			< /dev/null 2>&1 | tr -d '\r' > "$W/qemu.txt"
		grep -E '^rsos:|^=== KERNEL' "$W/qemu.txt" | sed 's/^/     | /'
	}
	# first boot: U-Boot saves its environment (slot a)
	qboot
	L=$(losetup -f --show -P "$W/sd.img") || exit 1
	LOOPS="$L"
	sleep 1
	printf '%s 0x100000 0x10000\n%s 0x110000 0x10000\n' "$L" "$L" > "$R/etc/fw_env.config"
	cp "$R/etc/fw_env.config" "$W/fw_env.config"
	printf 'rsos_ok 1\nrsos_tries 0\nrsos_fails 0\nrsos_fallback\n' | "$H/fw_setenv" -c "$W/fw_env.config" -s -
	echo "console=ttyS0 root=${L}p2 rootwait ro rsos.slot=a rsos.boot=pending" > "$R/proc/cmdline"
	rm -rf "$R/data/update" "$R/data/rsos"; mkdir -p "$R/data/update"
	(
		# shellcheck disable=SC1091
		. "$W/vb.env"
		printf "RSOS_VERSION='%s'\nRSOS_VARIANT='%s'\nRSOS_BUILD_TIME='%s'\nRSOS_BOARD_ID='%s'\n" "0.0.1" \
			"$RSOS_VARIANT" 1000 "$RSOS_BOARD_ID" > "$R/etc/rsos/version.env"
	)
	pack "$W/qb.img" "$W/qb.rsu" "$W/test.key"
	echo "-- an interrupted install, then a boot"
	RSOS_UPDATE_TEST_ABORT=write:8388608 U --machine apply "$W/qb.rsu" > "$W/out"
	sync
	losetup -d "$L"; LOOPS=
	qboot
	check "U-Boot boots slot a (slot b has no file system)" "grep -q '^=== KERNEL slot a:.*rsos.boot=pending' $W/qemu.txt"
	L=$(losetup -f --show -P "$W/sd.img"); LOOPS="$L"; sleep 1
	printf '%s 0x100000 0x10000\n%s 0x110000 0x10000\n' "$L" "$L" > "$W/fw_env.config"
	cp "$W/fw_env.config" "$R/etc/fw_env.config"
	echo "console=ttyS0 root=${L}p2 rootwait ro rsos.slot=a rsos.boot=pending" > "$R/proc/cmdline"
	printf 'rsos_ok 1\nrsos_tries 0\nrsos_fails 0\nrsos_fallback\n' | "$H/fw_setenv" -c "$W/fw_env.config" -s -
	echo "-- the install, then a boot"
	U --machine apply "$W/qb.rsu" > "$W/out"
	check "installed" "grep -q '^done.*slot=b' $W/out"
	sync
	losetup -d "$L"; LOOPS=
	qboot
	check "U-Boot boots the new slot b on trial" "grep -q '^rsos: trying unconfirmed slot b, 2 tries left' $W/qemu.txt && \
		grep -q '^=== KERNEL slot b:.*root=/dev/mmcblk0p3.*rsos.slot=b rsos.boot=pending' $W/qemu.txt"
	printf '%s 0x100000 0x10000\n%s 0x110000 0x10000\n' "$W/sd.img" "$W/sd.img" > "$W/fw_file.config"
	check "the environment after that boot: slot b, tries 2" \
		"[ \"\$($H/fw_printenv -c $W/fw_file.config -n rsos_slot)\" = b ] && \
		[ \"\$($H/fw_printenv -c $W/fw_file.config -n rsos_tries)\" = 2 ]"
fi

echo "PASS=$pass FAIL=$fail"
[ "$fail" = 0 ]
