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
W=${RSOS_DPTEST_DIR:-$HOME/rsos/dptest}
rm -rf "$W"; mkdir -p "$W/bin" "$W/data" "$W/run"
# BusyBox applets first in PATH; host tools for what BusyBox lacks.
for a in sh find tar dd head tail du cut wc grep sed tr mount umount sync cat mkdir rm mv sleep usleep printf echo ls \
	md5sum sort od; do
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
# Every run: the first-boot trace in the image's raw gap at 3 MiB (as the
# RetroStone2 board.ini), the kernel log and the watchdog as plain files
# (never the host's /dev/watchdog or /dev/kmsg).
DPARGS=      # arguments of data-partition (--format-confirmed)
KEEPRUN=     # 1: keep $W/run (the menu's call after a boot: data-problem)
PRE=true     # run first, in the namespace (the menu's tmpfs on /data)
run_dp() { # [env...]; output in $W/out.txt (and on stdout)
	[ -n "$KEEPRUN" ] || rm -f "$W/run"/*
	rm -f "$W/wd" "$W/kmsg"
	: > "$W/wd"
	env -i PATH="$TPATH" RSOS_DATA_DISK="$LOOP" RSOS_DATA_MNT="$W/data" RSOS_RUN="$W/run" \
		RSOS_SHARE="$OUT/target/usr/share/rsos" RSOS_TRACE_KIB=3072 RSOS_KMSG="$W/kmsg" \
		RSOS_WATCHDOG="$W/wd" "$@" \
		unshare -m --propagation private /bin/busybox sh -c "$PRE; /bin/busybox sh $SCRIPT $DPARGS; rc=\$?; ls -a $W/data > $W/ls.txt; grep ' $W/data ' /proc/mounts > $W/mnt.txt; (cd $W/data && find . -type f | sort | while read f; do md5sum \"\$f\"; done) > $W/sums.txt; exit \$rc" \
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
mounted_ro() { cut -d' ' -f4 "$W/mnt.txt" | grep -q '^ro,\|^ro$'; }
problem() { [ "$(cat "$W/run/data-problem" 2> /dev/null)" = "$1" ]; }
# md5 of the partition's first 4 MiB (untouched?)
p1sum() { dd if="${LOOP}p1" bs=1M count=4 status=none | md5sum | cut -d' ' -f1; }
# after a write to the partition: the host's udev may re-read the table (the node goes and comes back)
settle() { sync; udevadm settle 2> /dev/null; sleep 1; }
marker() { dd if=$LOOP bs=1M skip=$(( $(cat /sys/class/block/${LOOP##*/}/size) / 2048 - 1 )) count=1 status=none | head -c 64 | tr -d '\0'; }
# the first-boot trace in the raw gap (64 KiB at 3 MiB)
rawtrace() { dd if="$LOOP" bs=64k skip=48 count=1 status=none | tr -d '\0'; }
traced() { rawtrace | grep -q "$1"; }
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
check "first-boot trace on the card (3 MiB): every step, the exit" \
	"rawtrace | head -n 1 | grep -q '^RSOSFB01 ' && traced 'start: first boot' && traced 'partition table written' && traced 'exFAT created' && traced 'done (exit status 0)'"
check "trace in the kernel log too" "grep -q '^<5>rsos-data: partition table written' $W/kmsg"
check "watchdog serviced, then closed with the magic V" "grep -q '^\.*V\$' $W/wd"
check "rootfs A untouched by the trace (starts at 4 MiB)" "cmp -s -n 1048576 -i 4194304 $OUT/images/sdcard.img $LOOP"
check "U-Boot and its environment untouched (8 KiB .. 3 MiB)" "cmp -s -n $((3 * 1048576 - 8192)) -i 8192 $OUT/images/sdcard.img $LOOP"
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
check "(review) resume: the stamp says it finished: not formatted again, exit 0" \
	"[ $rc -eq 0 ] && said 'conversion had finished' && ! said formatting && seed_ok && full_exfat"
check "marker cleared" "[ -z \"\$(marker)\" ]"
cleanup

echo "== C3 (review): the marker cannot be cleared; the user writes to /data; the next boot keeps it all =="
make_card $BIG
seed_files
run_dp RSOS_TEST_FAIL=clear_marker; rc=$?
check "converted, exit 0, the marker left" "[ $rc -eq 0 ] && full_exfat && seed_ok && marker | grep -q '^RSOSCV02 '"
mkdir -p "$W/mnt"; mount -t exfat "${LOOP}p1" "$W/mnt" && cp "$W/new.bin" "$W/mnt/saves/new.srm" && umount "$W/mnt"
settle
run_dp; rc=$?
check "no format, no restore: the save made since is kept" \
	"[ $rc -eq 0 ] && said 'conversion had finished' && ! said formatting && ! said restoring && has $W/new.bin ./saves/new.srm && seed_ok"
check "marker cleared (read back)" "[ -z \"\$(marker)\" ] && said 'conversion marker cleared'"
cleanup

echo "== R1 (review): partx fails after the MBR write: the seed read-only, the next boot finishes =="
make_card $BIG
seed_files
run_dp RSOS_TEST_FAIL=partx; rc=$?
check "the seed mounted read-only (the marker is valid), files there" \
	"[ \"\$(mounted_fs)\" = vfat ] && mounted_ro && seed_ok && [ -e $W/run/data-readonly ] && marker | grep -q '^RSOSCV02 '"
run_dp; rc=$?
check "next boot: finished from the backup, files restored" "[ $rc -eq 0 ] && said 'finishing an interrupted' && full_exfat && seed_ok"
cleanup

echo "== R2 (review): mkfs.exfat fails, at the first boot and at the resume: read-only both times =="
make_card $BIG
seed_files
run_dp RSOS_TEST_FAIL=mkfs; rc=$?
check "first boot: read-only" "mounted_ro && seed_ok && marker | grep -q '^RSOSCV02 '"
run_dp RSOS_TEST_FAIL=mkfs; rc=$?
check "resume fails too: read-only again, the marker kept" "said 'could not finish it' && mounted_ro && marker | grep -q '^RSOSCV02 '"
run_dp; rc=$?
check "then finished: files restored" "[ $rc -eq 0 ] && full_exfat && seed_ok && [ -z \"\$(marker)\" ]"
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
rawtrace > "$W/trace-before"
start=$(date +%s%N); run_dp; rc=$?; end=$(date +%s%N)
check "exit 0 in $(( (end - start) / 1000000 )) ms, no fsck" "[ $rc -eq 0 ] && ! said checking"
check "no trace, no watchdog on a normal boot" "[ ! -e $W/run/firstboot.trace ] && [ ! -s $W/wd ] && rawtrace | cmp -s - $W/trace-before"
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
settle
run_dp; rc=$?
check "(review) checked (fsck -p, -y), NOT formatted: a tmpfs on /data, data-problem" \
	"[ $rc -ne 0 ] && said 'fsck.exfat -y' && ! said formatting && [ \"\$(mounted_fs)\" = tmpfs ] && layout && problem 'unmountable exfat'"
check "no marker written (nothing started)" "[ -z \"\$(marker)\" ]"
# the menu's "storage could not be read" screen, after the user confirmed:
# it runs while the menu keeps its files on the tmpfs /data
MENU_TMPFS="mount -t tmpfs menu $W/data && echo settings > $W/data/menu.cfg"
PRE=$MENU_TMPFS DPARGS=--format-confirmed KEEPRUN=1 run_dp; rc=$?
check "(review) --format-confirmed: exit 0, a full-size empty exFAT, data-problem gone, marker cleared" \
	"[ $rc -eq 0 ] && [ \"\$(ptype)\" = 7 ] && [ \"\$(fstype)\" = exfat ] && [ \$(psize_mib) -gt $GROWN ] && [ ! -e $W/run/data-problem ] && [ -z \"\$(marker)\" ]"
check "the menu's tmpfs /data untouched (still mounted, its file there, nothing else written)" \
	"[ \"\$(mounted_fs)\" = tmpfs ] && grep -q menu.cfg $W/ls.txt && ! grep -q roms $W/ls.txt"
check "messages on stderr and in the kernel log" "said 'confirmed by the user' && grep -q 'rsos-data: formatting' $W/kmsg"
run_dp; rc=$?
check "next boot: normal, the layout" "[ $rc -eq 0 ] && full_exfat && layout && ! said formatting"
PRE=$MENU_TMPFS DPARGS=--format-confirmed run_dp; rc=$?
check "(review) --format-confirmed without a data problem: refused, nothing formatted" \
	"[ $rc -ne 0 ] && said 'refused' && ! said formatting && [ -z \"\$(marker)\" ]"
run_dp; rc=$?
check "the volume still there" "[ $rc -eq 0 ] && full_exfat && layout && ! said formatting"
cleanup

echo "== P2 (review): another filesystem (NTFS from a PC), no marker: never formatted =="
make_card $BIG
echo ',+,7' | sfdisk --quiet --no-reread -N 1 "$LOOP"; partx -u "$LOOP"
dd if=/dev/urandom of="${LOOP}p1" bs=1M count=4 status=none
printf '\353R\220NTFS    ' | dd of="${LOOP}p1" conv=notrunc status=none
printf '\125\252' | dd of="${LOOP}p1" bs=1 seek=510 conv=notrunc status=none
settle; before=$(p1sum)
run_dp; rc=$?
check "foreign ntfs, tmpfs, untouched" "problem 'foreign ntfs' && [ \"\$(mounted_fs)\" = tmpfs ] && [ \"\$(p1sum)\" = $before ] && ! said formatting"
cleanup

echo "== P3 (review): FAT16 that does not mount, and a blank partition: never formatted =="
make_card $BIG
echo ',+,7' | sfdisk --quiet --no-reread -N 1 "$LOOP"; partx -u "$LOOP"
dd if=/dev/zero of="${LOOP}p1" bs=1M count=4 status=none
printf 'FAT16   ' | dd of="${LOOP}p1" bs=1 seek=54 conv=notrunc status=none
settle; before=$(p1sum)
run_dp; rc=$?
check "foreign fat16, untouched" "problem 'foreign fat16' && [ \"\$(p1sum)\" = $before ] && ! said formatting"
dd if=/dev/zero of="${LOOP}p1" bs=1M count=4 status=none
settle
run_dp; rc=$?
check "blank: unmountable unknown, still blank" "problem 'unmountable unknown' && cmp -s -n 4194304 ${LOOP}p1 /dev/zero && ! said formatting"
cleanup

echo "== P4 (review): a FAT32 that does not mount (seed-sized, damaged): kept for a PC =="
make_card $BIG
dd if=/dev/zero of="${LOOP}p1" bs=1 seek=11 count=2 conv=notrunc status=none   # bytes per sector = 0
settle; before=$(p1sum)
run_dp; rc=$?
check "unmountable vfat, untouched" "problem 'unmountable vfat' && [ \"\$(p1sum)\" = $before ] && [ \"\$(mounted_fs)\" = tmpfs ]"
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

# --- The splash can never hold the boot (a stub stands in for
# rsos-frontend --splash; RSOS_SPLASH_STOP_S=1 keeps the test short).
cat > "$W/splash-ok" << 'EOF'
#!/bin/busybox sh
echo "stub splash: pid $$ $*"
s=
trap 'kill $s; echo "stub splash: SIGTERM, exiting"; exit 0' TERM
while :; do sleep 1 & s=$!; wait $s; done
EOF
cat > "$W/splash-deaf" << 'EOF'
#!/bin/busybox sh
# ignores SIGTERM, as a splash stuck mid-commit would (no child left behind
# by the SIGKILL: the ignored SIGTERM goes through the exec)
echo "stub splash (deaf): pid $$"
trap '' TERM
exec sleep 1000
EOF
chmod +x "$W/splash-ok" "$W/splash-deaf"

echo "== S1: a splash that stops on SIGTERM: its log goes to the trace =="
make_card $BIG
seed_files
run_dp RSOS_SPLASH="$W/splash-ok" RSOS_SPLASH_STOP_S=1; rc=$?
check "exit 0, converted, files kept" "[ $rc -eq 0 ] && full_exfat && seed_ok"
check "splash started once and stopped" "traced 'splash started' && traced 'splash stopped' && [ \$(rawtrace | grep -c 'splash started') -eq 1 ]"
check "its log in the trace" "traced 'splash| stub splash: SIGTERM, exiting'"
check "no stub left" "! pgrep -f '$W/splash-ok' > /dev/null"
cleanup

echo "== S2: a splash that ignores SIGTERM: SIGKILL after 1 s, the boot goes on =="
make_card $BIG
seed_files
start=$(date +%s)
run_dp RSOS_SPLASH="$W/splash-deaf" RSOS_SPLASH_STOP_S=1; rc=$?
secs=$(( $(date +%s) - start ))
check "exit 0 in $secs s, converted, files kept" "[ $rc -eq 0 ] && [ $secs -lt 60 ] && full_exfat && seed_ok"
check "SIGKILL said and traced" "said 'SIGTERM (.*): SIGKILL' && traced 'still there 1 s after SIGTERM'"
check "no stub left" "! pgrep -f '$W/splash-deaf' > /dev/null"
run_dp RSOS_SPLASH="$W/splash-deaf" RSOS_SPLASH_STOP_S=1; rc=$?
check "next boot: normal, no splash" "[ $rc -eq 0 ] && ! said 'splash'"
cleanup

echo "== S3: a step that stalls (no I/O, no step): the watchdog is no longer serviced =="
make_card $BIG
run_dp RSOS_SPLASH="$W/splash-ok" RSOS_SPLASH_STOP_S=1 RSOS_TEST_HANG=before_sfdisk:8 \
	RSOS_WD_STALL_S=3 RSOS_WD_PERIOD_S=1; rc=$?
check "stall said and traced, where it stood" "said 'no progress for' && traced 'no progress for' && traced 'test: stopping 8 s at before_sfdisk'"
check "the watchdog got no service during the stall (<= 7 of ~10 periods)" "[ \$(tr -cd . < $W/wd | wc -c) -le 7 ]"
check "then finished (the test's step returns): exit 0, converted" "[ $rc -eq 0 ] && full_exfat && layout"
cleanup

echo "== S4: a first boot that never reached the menu: its trace is kept by the next one =="
make_card $BIG
run_dp RSOS_TEST_ABORT=before_sfdisk > /dev/null
check "aborted boot traced up to its end" "traced 'conversion marker written' && traced 'done (exit status 99)'"
run_dp; rc=$?
check "next boot: exit 0, converted" "[ $rc -eq 0 ] && full_exfat"
check "both boots in the record, the old one first" \
	"traced '(the previous record' && [ \$(rawtrace | grep -c '^=== boot') -eq 2 ] && rawtrace | grep -n 'exit status' | head -n 1 | grep -q 'status 99'"
cleanup

echo "== S5 (review): a request stuck in the driver (in-flight and io_ticks move, no I/O completes): no service =="
# a fake disk stat: the completed-I/O fields (1, 3, 5, 7) frozen, the
# in-flight count and io_ticks changing all the time
( i=0; while :; do echo "100 0 800 5 50 0 400 7 1 $((1000 + i)) $((2000 + i))" > "$W/stat.tmp"; mv "$W/stat.tmp" "$W/stat"; i=$((i + 1)); sleep 0.3; done ) &
STATW=$!
make_card $BIG
run_dp RSOS_TEST_HANG=before_sfdisk:8 RSOS_WD_STALL_S=3 RSOS_WD_PERIOD_S=1 RSOS_WD_STAT="$W/stat"; rc=$?
kill $STATW 2> /dev/null
check "no progress said despite the moving io_ticks; the watchdog starved" \
	"said 'no progress for' && [ \$(tr -cd . < $W/wd | wc -c) -le 7 ] && [ $rc -eq 0 ] && full_exfat"
cleanup

echo "== S6 (review): a chatty splash: its last 8 KiB only in the trace; the record ends with the last step =="
cat > "$W/splash-chatty" << 'EOF'
#!/bin/busybox sh
i=0
while [ $i -lt 3000 ]; do echo "stub splash: line $i of a very chatty splash log, padding padding padding"; i=$((i + 1)); done
s=
trap 'kill $s; exit 0' TERM
while :; do sleep 1 & s=$!; wait $s; done
EOF
chmod +x "$W/splash-chatty"
make_card $BIG
seed_files
run_dp RSOS_SPLASH="$W/splash-chatty" RSOS_SPLASH_STOP_S=1; rc=$?
check "exit 0, converted" "[ $rc -eq 0 ] && full_exfat && seed_ok"
check "the splash's last lines only (<= 8 KiB of them), the record ends with the exit" \
	"[ \$(rawtrace | grep 'splash|' | wc -c) -le 8400 ] && traced 'line 2999 of' && ! traced 'line 100 of' && rawtrace | tail -n 1 | grep -q 'done (exit status 0)'"
check "the record starts with the magic" "rawtrace | head -n 1 | grep -q '^RSOSFB01 '"
cleanup

echo "PASS=$pass FAIL=$fail"
[ $fail -eq 0 ]
