#!/bin/sh
# RetroStoneOS: /boot/boot.scr for a U-Boot single-board computer (the
# Orange Pi ports, docs/boards.md). Called by the board's post-build.sh with
# the board's values in the environment:
#   RSOS_KERNEL       kernel file in /boot (zImage, Image)
#   RSOS_BOOTM        U-Boot boot command for it (bootz, booti)
#   RSOS_MMC          U-Boot mmc device number of the SD card
#   RSOS_DISK         Linux block device of the SD card (mmcblk0)
#   RSOS_CONSOLE      console= argument(s) of the kernel command line
#   RSOS_FDT_DEFAULT  device tree in /boot when U-Boot has no fdtfile
#   RSOS_EXTRA_ARGS   more kernel arguments (optional)
# Usage: mk-boot-scr.sh <TARGET_DIR>; HOST_DIR comes from Buildroot.
set -e

TARGET="$1"
DIR="$(cd "$(dirname "$0")" && pwd)"
for v in RSOS_KERNEL RSOS_BOOTM RSOS_MMC RSOS_DISK RSOS_CONSOLE RSOS_FDT_DEFAULT; do
	eval "val=\${$v}"
	if [ -z "$val" ]; then
		echo "mk-boot-scr.sh: $v is not set" >&2
		exit 1
	fi
	case "$val" in
	*'|'* | *'&'* | *'\'*)
		echo "mk-boot-scr.sh: $v: '|', '&' and '\\' are not allowed" >&2
		exit 1
		;;
	esac
done
EXTRA=
[ -n "$RSOS_EXTRA_ARGS" ] && EXTRA="$RSOS_EXTRA_ARGS "

mkdir -p "$TARGET/boot"
CMD=${BUILD_DIR:-${TMPDIR:-/tmp}}/rsos-sbc-boot.cmd
sed -e "s|@KERNEL@|$RSOS_KERNEL|g" -e "s|@BOOTM@|$RSOS_BOOTM|g" \
	-e "s|@MMC@|$RSOS_MMC|g" -e "s|@DISK@|$RSOS_DISK|g" \
	-e "s|@CONSOLE@|$RSOS_CONSOLE|g" -e "s|@FDT_DEFAULT@|$RSOS_FDT_DEFAULT|g" \
	-e "s|@EXTRA_ARGS@|$EXTRA|g" "$DIR/boot.cmd.in" > "$CMD"
if grep -q '@[A-Z_]*@' "$CMD"; then
	echo "mk-boot-scr.sh: unreplaced placeholder in boot.cmd" >&2
	exit 1
fi
"$HOST_DIR/bin/mkimage" -A arm -O linux -T script -C none \
	-n "RetroStoneOS boot" -d "$CMD" "$TARGET/boot/boot.scr"
# The expanded script, for reference on the device
install -m 0644 "$CMD" "$TARGET/boot/boot.cmd"
