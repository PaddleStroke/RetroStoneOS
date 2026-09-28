#!/bin/sh
# RetroStoneOS post-image script (Raspberry Pi 4, and every other Raspberry Pi
# port: rpi2, rpi3_64 and rpi5_64 use it too, docs/boards.md): the boot FAT
# and sdcard.img with genimage. Called by Buildroot from its top directory as:
# post-image.sh <BINARIES_DIR>
set -e

BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARIES_DIR="${1:-$BINARIES_DIR}"

# Data partition seed (data.vfat, 128 MiB FAT32 "RETROSTONE"), shared by the
# boards: board/common/mk-data-seed.sh.
sh "$BOARD_DIR/../common/mk-data-seed.sh" "$BINARIES_DIR"

# The boot partition: everything rpi-firmware installed (bootcode.bin,
# start*.elf, fixup*.dat for the model, none on a Pi 5; config.txt,
# cmdline.txt, overlays/), the device trees and the kernel named in
# config.txt (Image, or zImage on the 32-bit Pi 2 port).
KERNEL=$(sed -n 's/^kernel=//p' "$BINARIES_DIR/rpi-firmware/config.txt")
{
	for f in "$BINARIES_DIR"/*.dtb "$BINARIES_DIR"/rpi-firmware/*; do
		[ -e "$f" ] && printf '\t\t\t"%s",\n' "${f#"$BINARIES_DIR"/}"
	done
	printf '\t\t\t"%s"\n' "$KERNEL"
} > "$BINARIES_DIR/boot-files.txt"
# The root file system slots are the size of rootfs.ext4
# (BR2_TARGET_ROOTFS_EXT2_SIZE: "512M", "768M", "1G"; 512 MiB by default).
SLOT=$(sed -n 's/^BR2_TARGET_ROOTFS_EXT2_SIZE="\(.*\)"$/\1/p' "${BR2_CONFIG:-/dev/null}")
case "$SLOT" in
*M) SLOT=${SLOT%M} ;;
*G) SLOT=$((${SLOT%G} * 1024)) ;;
*) SLOT=512 ;;
esac
awk -v list="$BINARIES_DIR/boot-files.txt" -v slot="$SLOT" '
	/^#BOOT_FILES#$/ { while ((getline l < list) > 0) print l; next }
	{
		gsub(/#SLOT#/, slot)
		gsub(/#B_OFFSET#/, 132 + slot)
		gsub(/#DATA_OFFSET#/, 132 + 2 * slot)
		print
	}' "$BOARD_DIR/genimage.cfg.in" > "$BINARIES_DIR/genimage.cfg"

exec support/scripts/genimage.sh -c "$BINARIES_DIR/genimage.cfg"
