#!/bin/sh
# RetroStoneOS post-image script (Orange Pi H3): assemble sdcard.img with
# genimage. Called by Buildroot from its top directory as:
# post-image.sh <BINARIES_DIR>
set -e

BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARIES_DIR="${1:-$BINARIES_DIR}"

# Data partition seed (data.vfat, 128 MiB FAT32 "RETROSTONE"), shared by the
# boards: board/common/mk-data-seed.sh.
sh "$BOARD_DIR/../common/mk-data-seed.sh" "$BINARIES_DIR"

exec support/scripts/genimage.sh -c "$BOARD_DIR/genimage.cfg"
