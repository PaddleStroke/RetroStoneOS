#!/bin/bash
# Loop-device simulation of board/common/rootfs-overlay/usr/libexec/rsos/data-partition
# (first boot of the data partition, power cuts at every step, dirty volumes).
# Run as root in WSL after a build:
#   bash buildroot-external/board/retrostone2/tests/data-partition-test.sh
# It uses ~/rsos/output/images/sdcard.img, BusyBox applets (apt install
# busybox-static) plus host sfdisk/partx/cksum and the Buildroot host
# mkfs.exfat/fsck.exfat, inside a private mount namespace. Work dir: ~/rsos/dptest.
# A power cut is simulated with RSOS_TEST_ABORT=<step> (the script exits 99 at
# that point, leaving the card as a cut would); the namespace then goes away,
# which unmounts /data like the kernel would not, so a "dirty" volume is made
# by setting its flag by hand (scenario Q).
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
REPO=$(cd "$(dirname "$0")/../../../.." && pwd)
SCRIPT=$REPO/buildroot-external/board/common/rootfs-overlay/usr/libexec/rsos/data-partition
OUT=$HOME/rsos/output
W=$HOME/rsos/dptest
rm -rf "$W"; mkdir -p "$W/bin" "$W/data" "$W/run"
# BusyBox applets first in PATH; host tools for what BusyBox lacks.
for a in sh find tar dd head du cut wc grep sed tr mount umount sync cat mkdir rm mv sleep printf echo ls md5sum sort; do
	ln -sf /bin/busybox "$W/bin/$a"
done
for t in mkfs.exfat fsck.exfat; do
	T=$OUT/host/sbin/$t
	[ -x "$T" ] || T=$(command -v $t)   # apt install exfatprogs
	ln -sf "$T" "$W/bin/$t"
done
ln -sf /usr/sbin/sfdisk "$W/bin/sfdisk"
ln -sf /usr/bin/partx "$W/bin/partx"
ln -sf /usr/bin/unshare "$W/bin/unshare"
# Ubuntu's busybox has no cksum applet (the target's has): use coreutils (same CRC)
ln -sf /usr/bin/cksum "$W/bin/cksum"
TPATH=$W/bin

pass=0; fail=0
check() { if eval "$2"; then echo "  ok: $1"; pass=$((pass+1)); else echo "  FAIL: $1"; fail=$((fail+1)); fi; }

# make_card <size MiB> -> sets LOOP
make_card() {
	rm -f "$W/card.img" "$W/run"/*
	truncate -s "${1}M" "$W/card.img"
	dd if="$OUT/images/sdcard.img" of="$W/card.img" conv=notrunc,sparse bs=1M status=none
	LOOP=$(losetup -P -f --show "$W/card.img")
	sleep 1
}
# the user writes the image again (dd over the same card, tail untouched)
reflash() {
	dd if="$OUT/images/sdcard.img" of="$LOOP" conv=notrunc bs=1M status=none
	sync; partx -u "$LOOP"
}
run_dp() { # [env...]; output in $W/out.txt (and on stdout)
	rm -f "$W/run"/*
	env -i PATH="$TPATH" RSOS_DATA_DISK="$LOOP" RSOS_DATA_MNT="$W/data" RSOS_RUN="$W/run" \
		RSOS_SHARE="$OUT/target/usr/share/rsos" "$@" \
		unshare -m --propagation private /bin/busybox sh -c "/bin/busybox sh $SCRIPT; rc=\$?; ls -a $W/data > $W/ls.txt; grep ' $W/data ' /proc/mounts > $W/mnt.txt; (cd $W/data && find . -type f | sort | while read f; do md5sum \"\$f\"; done) > $W/sums.txt; exit \$rc" \
		> "$W/out.txt" 2>&1
	local rc=$?
	sed 's/^/     | /' "$W/out.txt"
	partx -u "$LOOP" 2> /dev/null
	return $rc
}
ptype() { sfdisk --part-type "$LOOP" 1 2>/dev/null | tr -d " "; }
psize_mib() { echo $(( $(cat /sys/class/block/${LOOP##*/}p1/size) / 2048 )); }
fstype() { blkid -p -o value -s TYPE "${LOOP}p1"; }
mounted_fs() { cut -d' ' -f3 "$W/mnt.txt"; }
marker() { dd if=$LOOP bs=1M skip=$(( $(cat /sys/class/block/${LOOP##*/}/size) / 2048 - 1 )) count=1 status=none | head -c 64 | tr -d '\0'; }
has() { grep -q "$(md5sum < "$1" | cut -d' ' -f1)  $2" "$W/sums.txt"; }
said() { grep -q "$1" "$W/out.txt"; }
cleanup() { losetup -d "$LOOP" 2>/dev/null; }
full_exfat() { [ "$(ptype)" = 7 ] && [ "$(fstype)" = exfat ] && [ "$(mounted_fs)" = exfat ] && [ "$(psize_mib)" -gt "$GROWN" ]; }
layout() { grep -q roms "$W/ls.txt" && grep -q README.txt "$W/ls.txt"; }

# Layout taken from the image itself (root slots were 384 MiB before 2026-09-27,
# 512 MiB since): the seed offset, and card sizes relative to the image size.
IMG_MIB=$(( ($(stat -c %s "$OUT/images/sdcard.img") + 1048575) / 1048576 ))
SEED_START=$(sfdisk -d "$OUT/images/sdcard.img" | sed -n 's/.*img1 : start= *\([0-9]*\).*/\1/p')
SEEDOFF=$(( SEED_START * 512 ))
SEEDOFF_MIB=$(( SEEDOFF / 1048576 ))
BIG=$(( (IMG_MIB / 1024 + 2) * 1024 ))     # e.g. 3072 MiB for a 1.15 GiB image
SMALL=$(( IMG_MIB + 64 ))                  # tail too small for a 100 MiB backup
GROWN=$(( BIG - SEEDOFF_MIB - 64 ))        # "grown to the end of the card"
ROOT_ENTRIES=$(sfdisk -d "$OUT/images/sdcard.img" | grep -E 'img[23] :' | sed 's#.*: ##')
command -v mkfs.exfat > /dev/null || { echo "mkfs.exfat missing on the host: apt install exfatprogs"; exit 2; }
echo "layout: image ${IMG_MIB} MiB, seed at ${SEEDOFF_MIB} MiB, cards ${BIG}/${SMALL} MiB"
mt() { # mtools on the seed inside the card image
	local cmd=$1; shift
	MTOOLS_SKIP_CHECK=1 $cmd -i "$W/card.img@@$SEEDOFF" "$@"
}

head -c 20M /dev/urandom > "$W/big.bin"; head -c 3000 /dev/urandom > "$W/small.gb"
head -c 5M /dev/urandom > "$W/old.bin"; head -c 5M /dev/urandom > "$W/new.bin"
echo x > "$W/j1"
seed_files() { # a few user files on the seed
	mt mcopy "$W/big.bin" ::roms/psx/big.bin; mt mcopy "$W/small.gb" "::roms/gb/My Game.gb"
	mt mcopy "$W/small.gb" ::saves/x.srm
	partx -u "$LOOP"
}
seed_ok() { has "$W/big.bin" ./roms/psx/big.bin && has "$W/small.gb" "./roms/gb/My Game.gb" && has "$W/small.gb" ./saves/x.srm; }

echo "== seed contents (mdir) =="
MTOOLS_SKIP_CHECK=1 mdir -i "$OUT/images/data.vfat" ::
MTOOLS_SKIP_CHECK=1 mdir -/ -b -i "$OUT/images/data.vfat" ::roms | sed 's|.*::/||' | tr '\n' ' '; echo

echo "== A: seed with only PC junk, big card =="
make_card $BIG
id0=$(sfdisk --disk-id "$LOOP")
mt mmd "::System Volume Information"
mt mcopy "$W/j1" "::System Volume Information/IndexerVolumeGuid"
mt mcopy "$W/j1" ::.DS_Store; mt mcopy "$W/j1" ::roms/nes/._foo.nes; mt mcopy "$W/j1" ::Thumbs.db
partx -u "$LOOP"
run_dp; rc=$?
check "exit 0" "[ $rc -eq 0 ]"
check "type 07, exFAT, grown to >$GROWN MiB ($(psize_mib))" full_exfat
check "layout (roms, README)" layout
check "marker cleared" "[ -z \"\$(marker)\" ]"
check "new MBR disk id ($id0 -> $(sfdisk --disk-id "$LOOP"))" "[ \"\$(sfdisk --disk-id $LOOP)\" != $id0 ]"
check "rootfs entries unchanged" "[ \"\$(sfdisk -d $LOOP | grep -E 'p[23] :' | sed 's#.*: ##')\" = \"\$ROOT_ENTRIES\" ]"
cleanup

echo "== B: seed with user files, big card =="
make_card $BIG
seed_files
run_dp; rc=$?
check "exit 0" "[ $rc -eq 0 ]"
check "type 07, exFAT, grown ($(psize_mib) MiB)" full_exfat
check "files restored intact (incl. a name with a space)" seed_ok
check "README kept" "grep -q README.txt $W/ls.txt"
check "marker cleared" "[ -z \"\$(marker)\" ]"
cleanup

echo "== C: power cut after the format, then reboot =="
make_card $BIG
seed_files
run_dp RSOS_TEST_ABORT=after_format; rc=$?
check "aborted (99)" "[ $rc -eq 99 ]"
check "marker present (RSOSCV02 + the MBR id)" "marker | grep -q \"^RSOSCV02 \$(sfdisk --disk-id $LOOP) \""
run_dp; rc=$?
check "resume exit 0" "[ $rc -eq 0 ] && said 'finishing an interrupted'"
check "files restored after resume" seed_ok
check "marker cleared" "[ -z \"\$(marker)\" ]"
run_dp; rc=$?
check "third boot is a no-op, files still there" "[ $rc -eq 0 ] && ! said formatting && seed_ok"
cleanup

echo "== C2: power cut after the restore, before the marker is cleared =="
make_card $BIG
seed_files
run_dp RSOS_TEST_ABORT=after_restore; rc=$?
check "aborted (99), files already there" "[ $rc -eq 99 ] && seed_ok"
run_dp; rc=$?
check "resume: formatted and restored again, exit 0" "[ $rc -eq 0 ] && said formatting && seed_ok && full_exfat"
check "marker cleared" "[ -z \"\$(marker)\" ]"
cleanup

echo "== D: small card with 100 MiB of files: too small, keep FAT32 =="
make_card $SMALL
head -c 100M /dev/urandom > "$W/huge.bin"
mt mcopy "$W/huge.bin" ::roms/psx/huge.bin
partx -u "$LOOP"
run_dp; rc=$?
check "exit 0" "[ $rc -eq 0 ]"
check "still vfat, type 0c" "[ \"\$(fstype)\" = vfat ] && [ \"\$(ptype)\" = c ]"
check "file intact" "has $W/huge.bin ./roms/psx/huge.bin"
run_dp; rc=$?
check "second boot mounts, no retry" "[ $rc -eq 0 ] && ! said 'too small'"
cleanup

echo "== E: normal boot after A (already exFAT, full size) =="
make_card $BIG
run_dp >/dev/null
start=$(date +%s%N); run_dp; rc=$?; end=$(date +%s%N)
check "exit 0 in $(( (end - start) / 1000000 )) ms, no fsck" "[ $rc -eq 0 ] && ! said checking"
cleanup

echo "== F: power cut after the format and a damaged backup =="
make_card $BIG
mt mcopy "$W/big.bin" ::roms/psx/big.bin
partx -u "$LOOP"
run_dp RSOS_TEST_ABORT=after_format > /dev/null
set -- $(marker)
printf 'XXXX' | dd of=$LOOP bs=1M seek=$(( $3 + 1 )) conv=notrunc status=none
run_dp; rc=$?
check "exit 0, data mounted (exFAT)" "[ $rc -eq 0 ] && full_exfat"
check "marker renamed RSOSBKBAD" "marker | grep -q '^RSOSBKBAD'"
check "layout recreated" layout
run_dp; rc=$?
check "next boot does not retry" "[ $rc -eq 0 ] && ! said damaged"
cleanup

echo "== G (review): empty seed, power cut after sfdisk and before mkfs.exfat, under the old image (no marker) =="
make_card $BIG
echo ',+,7' | sfdisk --quiet --no-reread -N 1 "$LOOP"; partx -u "$LOOP"
echo "   after the cut: type=$(ptype) size=$(psize_mib) MiB fs=$(fstype)"
run_dp; rc=$?
check "exit 0, full-size exFAT" "[ $rc -eq 0 ] && full_exfat"
check "layout" layout
run_dp; rc=$?
check "next boot: no-op" "[ $rc -eq 0 ] && ! said formatting && full_exfat"
cleanup

echo "== G2: the same, and the user copied files onto the 126 MiB FAT32 meanwhile =="
make_card $BIG
echo ',+,7' | sfdisk --quiet --no-reread -N 1 "$LOOP"; partx -u "$LOOP"
seed_files
run_dp; rc=$?
check "exit 0, full-size exFAT, files kept" "[ $rc -eq 0 ] && full_exfat && seed_ok"
cleanup

echo "== H (review): stale marker: power cut after the format, the card is re-flashed, new ROMs copied =="
make_card $BIG
mt mcopy "$W/old.bin" ::roms/nes/old.nes; partx -u "$LOOP"
run_dp RSOS_TEST_ABORT=after_format > /dev/null; echo "   first boot aborted rc=$?"
reflash
mt mcopy "$W/new.bin" ::roms/nes/new.nes; partx -u "$LOOP"
run_dp; rc=$?
check "exit 0, stale marker discarded" "[ $rc -eq 0 ] && said 'stale conversion marker'"
check "new.nes kept" "has $W/new.bin ./roms/nes/new.nes"
check "old.nes not brought back" "! grep -q old.nes $W/sums.txt"
check "converted to full-size exFAT" full_exfat
cleanup

echo "== J: empty seed, power cut after the marker, before the MBR write =="
make_card $BIG
run_dp RSOS_TEST_ABORT=before_sfdisk; rc=$?
check "aborted (99), seed untouched" "[ $rc -eq 99 ] && [ \"\$(ptype)\" = c ] && [ \"\$(fstype)\" = vfat ]"
run_dp; rc=$?
check "stale marker discarded, conversion done" "[ $rc -eq 0 ] && said 'stale conversion marker' && full_exfat && layout"
check "marker cleared" "[ -z \"\$(marker)\" ]"
cleanup

echo "== K: empty seed, power cut right after the MBR write =="
make_card $BIG
run_dp RSOS_TEST_ABORT=after_sfdisk; rc=$?
check "aborted (99), entry grown, FAT32 inside" "[ $rc -eq 99 ] && [ \"\$(ptype)\" = 7 ] && [ \"\$(fstype)\" = vfat ]"
run_dp; rc=$?
check "resumed: full-size exFAT with the layout" "[ $rc -eq 0 ] && said 'finishing an interrupted' && full_exfat && layout"
check "marker cleared" "[ -z \"\$(marker)\" ]"
cleanup

echo "== L: empty seed, power cut in the middle of mkfs.exfat (boot region written, the rest not) =="
make_card $BIG
run_dp RSOS_TEST_ABORT=after_sfdisk > /dev/null
"$W/bin/mkfs.exfat" -L RETROSTONE "${LOOP}p1" > /dev/null
dd if=/dev/zero of="${LOOP}p1" bs=512 seek=24 count=$((64 * 2048)) conv=notrunc status=none
echo "   half-made volume: blkid says '$(fstype)'"
run_dp; rc=$?
check "resumed: full-size exFAT with the layout" "[ $rc -eq 0 ] && full_exfat && layout"
cleanup

echo "== M: seed with files, power cut after the backup and the marker, before the MBR write =="
make_card $BIG
seed_files
run_dp RSOS_TEST_ABORT=before_sfdisk; rc=$?
check "aborted (99), seed untouched" "[ $rc -eq 99 ] && [ \"\$(ptype)\" = c ]"
mt mcopy "$W/new.bin" ::roms/nes/new.nes; partx -u "$LOOP"
run_dp; rc=$?
check "stale marker discarded, fresh backup: all files incl. one added after the cut" "[ $rc -eq 0 ] && full_exfat && seed_ok && has $W/new.bin ./roms/nes/new.nes"
cleanup

echo "== N: seed with files, power cut right after the MBR write =="
make_card $BIG
seed_files
run_dp RSOS_TEST_ABORT=after_sfdisk; rc=$?
check "aborted (99)" "[ $rc -eq 99 ]"
run_dp; rc=$?
check "resumed: files restored" "[ $rc -eq 0 ] && full_exfat && seed_ok"
check "marker cleared" "[ -z \"\$(marker)\" ]"
cleanup

echo "== P: full-size type 07 entry holding a broken exFAT and no marker =="
make_card $BIG
echo ',+,7' | sfdisk --quiet --no-reread -N 1 "$LOOP"; partx -u "$LOOP"
"$W/bin/mkfs.exfat" -L RETROSTONE "${LOOP}p1" > /dev/null
dd if=/dev/zero of="${LOOP}p1" bs=512 seek=24 count=$((64 * 2048)) conv=notrunc status=none
run_dp; rc=$?
check "checked, then formatted: /data available" "[ $rc -eq 0 ] && said 'checking' && full_exfat && layout"
check "note for the UI (data-reformatted)" "said 'formatting it as an empty exFAT'"
cleanup

echo "== Q: exFAT not unmounted cleanly (VolumeDirty set) =="
make_card $BIG
seed_files
run_dp > /dev/null
printf '\002' | dd of="${LOOP}p1" bs=1 seek=106 conv=notrunc status=none
run_dp; rc=$?
check "dirty volume checked with fsck.exfat -p, files intact" "[ $rc -eq 0 ] && said 'not unmounted cleanly' && seed_ok && full_exfat"
check "flag cleared on the card" "[ \"\$(dd if=${LOOP}p1 bs=1 skip=106 count=1 status=none | od -An -tu1 | tr -d ' ')\" = 0 ]"
run_dp; rc=$?
check "next (clean) boot: no check" "[ $rc -eq 0 ] && ! said 'not unmounted cleanly'"
cleanup

echo "== I (S14): the backup step fails: keep FAT32 and do not retry at every boot =="
make_card $BIG
seed_files
run_dp RSOS_TEST_FAIL=backup; rc=$?
check "exit 0, FAT32 kept, files intact" "[ $rc -eq 0 ] && [ \"\$(mounted_fs)\" = vfat ] && seed_ok"
check "entry grown, type 0c" "[ \"\$(ptype)\" = c ] && [ $(psize_mib) -gt $GROWN ]"
run_dp RSOS_TEST_FAIL=backup; rc=$?
check "second boot: no new backup attempt" "[ $rc -eq 0 ] && ! said 'backing up' && seed_ok"
cleanup

echo "PASS=$pass FAIL=$fail"
[ $fail -eq 0 ]
