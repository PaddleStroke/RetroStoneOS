#!/bin/sh
# RetroStoneOS data partition seed, for the boards' post-image scripts:
#   board/common/mk-data-seed.sh <BINARIES_DIR>   -> <BINARIES_DIR>/data.vfat
# (TARGET_DIR and HOST_DIR come from Buildroot's environment.)
#
# A 128 MiB FAT32 with the folder layout and README.txt, filled without
# mounting (dosfstools + mtools), so that the card shows a ready RETROSTONE
# drive on a PC right after flashing. The first boot turns it into a
# full-size exFAT (usr/libexec/rsos/data-partition, docs/rom-transfer.md 1.3).
# Needs BR2_PACKAGE_HOST_DOSFSTOOLS and BR2_PACKAGE_HOST_MTOOLS.
set -e

BINARIES_DIR="${1:-$BINARIES_DIR}"

SEED="$BINARIES_DIR/data.vfat"
rm -f "$SEED"
# A fixed volume ID instead of a random one: 52534F53 ("RSOS"), or the low
# 32 bits of SOURCE_DATE_EPOCH when it is set (from the environment, or by
# Buildroot with BR2_REPRODUCIBLE), and fixed internal timestamps
# (--invariant): the same build inputs give the same seed (review S15;
# mtools still stamps the folders with the build time). Nothing relies on the
# ID being unique per card (the first boot binds its conversion marker to the
# MBR disk identifier instead).
VOLID=$(printf '%08x' $((${SOURCE_DATE_EPOCH:-1381191507} & 0xffffffff)))
"$HOST_DIR/sbin/mkfs.vfat" --invariant -i "$VOLID" -F 32 -n RETROSTONE -C "$SEED" $((128 * 1024)) > /dev/null
MT="$HOST_DIR/bin"
export MTOOLS_SKIP_CHECK=1
"$MT/mmd" -i "$SEED" ::roms ::bios ::saves ::states ::screenshots ::themes
for s in $(cat "$TARGET_DIR/usr/share/rsos/rom-folders"); do
	"$MT/mmd" -i "$SEED" "::roms/$s"
done
# README with CRLF line ends for old Notepad
sed 's/$/\r/' "$TARGET_DIR/usr/share/rsos/data-README.txt" > "$BINARIES_DIR/README.txt"
"$MT/mcopy" -i "$SEED" "$BINARIES_DIR/README.txt" ::README.txt
