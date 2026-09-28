#!/bin/sh
# RetroStoneOS post-image script (Orange Pi 5): assemble sdcard.img with
# genimage. Called by Buildroot from its top directory as:
# post-image.sh <BINARIES_DIR>
set -e

BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARIES_DIR="${1:-$BINARIES_DIR}"

# u-boot-rockchip.bin must end before the environment at 14 MiB
SIZE=$(stat -c %s "$BINARIES_DIR/u-boot-rockchip.bin")
if [ $((32 * 1024 + SIZE)) -gt $((14 * 1024 * 1024)) ]; then
	echo "post-image.sh: u-boot-rockchip.bin ($SIZE bytes) overlaps the U-Boot environment" >&2
	exit 1
fi

# Data partition seed (data.vfat, 128 MiB FAT32 "RETROSTONE"), shared by the
# boards: board/common/mk-data-seed.sh.
sh "$BOARD_DIR/../common/mk-data-seed.sh" "$BINARIES_DIR"

exec support/scripts/genimage.sh -c "$BOARD_DIR/genimage.cfg"
