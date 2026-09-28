#!/bin/sh
# RetroStoneOS post-build script, RetroStone1 part (after
# board/common/post-build.sh). Called as: post-build.sh <TARGET_DIR>;
# HOST_DIR, BR2_CONFIG... are in the environment.
set -e

TARGET="$1"
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"

# The board's device tree (board/retrostone1/dts, copied into the kernel tree
# by external.mk). U-Boot's own fdtfile is the Orange Pi One's, which /boot
# does not have: boot.scr falls back to this one.
DTS=$(sed -n 's/^BR2_LINUX_KERNEL_INTREE_DTS_NAME="\([^" ]*\).*/\1/p' "$BR2_CONFIG")
DTB=${DTS##*/}.dtb
if [ ! -f "$TARGET/boot/$DTB" ]; then
	echo "post-build.sh: $TARGET/boot/$DTB is missing" >&2
	exit 1
fi

# /boot/boot.scr (the A/B boot script, board/sbc-uboot/boot.cmd.in)
RSOS_KERNEL=zImage RSOS_BOOTM=bootz RSOS_MMC=0 RSOS_DISK=mmcblk0 \
	RSOS_CONSOLE="console=ttyS0,115200" RSOS_FDT_DEFAULT="$DTB" \
	sh "$BOARD_DIR/../sbc-uboot/mk-boot-scr.sh" "$TARGET"
