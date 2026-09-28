#!/bin/sh
# RetroStoneOS post-build script, RetroStone2 part (after
# board/common/post-build.sh, which does everything the boards share).
# Called as: post-build.sh <TARGET_DIR>; HOST_DIR, BINARIES_DIR... are in the
# environment.
set -e

TARGET="$1"
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Boot script: boot.cmd -> /boot/boot.scr (loaded by U-Boot's bootcmd).
# Release build (BR2_RETROSTONE_RELEASE, docs/build.md "Release build"):
# without the development-only kernel arguments "initcall_debug
# log_buf_len=1M".
mkdir -p "$TARGET/boot"
BOOT_CMD=$BOARD_DIR/boot.cmd
if grep -q '^BR2_RETROSTONE_RELEASE=y$' "$BR2_CONFIG"; then
	BOOT_CMD=${BUILD_DIR:-${TMPDIR:-/tmp}}/rsos-boot-release.cmd
	sed 's/ initcall_debug log_buf_len=1M / /' "$BOARD_DIR/boot.cmd" > "$BOOT_CMD"
	if grep '^setenv bootargs ' "$BOOT_CMD" | grep -q 'initcall_debug\|log_buf_len'; then
		echo "post-build.sh: could not remove the debug arguments from boot.cmd" >&2
		exit 1
	fi
fi
"$HOST_DIR/bin/mkimage" -A arm -O linux -T script -C none \
	-n "RetroStoneOS boot" -d "$BOOT_CMD" "$TARGET/boot/boot.scr"

# --- /usr/libexec/rsos/cntvct: reads the ARM generic counter, so the boot
# logger can put U-Boot's bootstage marks and the kernel log on one time line
# (tools/rsos-cntvct.c). One small C file: built here with the target
# toolchain wrapper rather than as a package.
CC=
for c in "$HOST_DIR"/bin/arm-*linux*-gcc; do
	[ -x "$c" ] && CC=$c && break
done
if [ -z "$CC" ]; then
	echo "post-build.sh: no target C compiler in $HOST_DIR/bin" >&2
	exit 1
fi
mkdir -p "$TARGET/usr/libexec/rsos"
"$CC" -O2 -s -Wall -Wextra -Werror -o "$TARGET/usr/libexec/rsos/cntvct" \
	"$BOARD_DIR/tools/rsos-cntvct.c"
# /usr/libexec/rsos/axpstamp: rcK keeps the times of its last steps (after
# /data is unmounted) in the AXP209's data registers; the boot logger adds
# them to shutdown.txt on the next boot (tools/rsos-axpstamp.c).
"$CC" -O2 -s -Wall -Wextra -Werror -o "$TARGET/usr/libexec/rsos/axpstamp" \
	"$BOARD_DIR/tools/rsos-axpstamp.c"

# --- WiFi/BT firmware names.
# brcmfmac asks for brcm/brcmfmac<chip>-sdio.<board compatible>.{bin,txt},
# then brcm/brcmfmac<chip>-sdio.{bin,txt}; btbcm asks for brcm/<chip>.hcd, then
# brcm/BCM.hcd. Provide the generic names.
FW="$TARGET/lib/firmware/brcm"
link_fw() { # <link name> <target, relative to brcm/>
	if [ -e "$FW/$2" ] && [ ! -e "$FW/$1" ]; then
		ln -sf "$2" "$FW/$1"
	fi
}
if [ -d "$FW" ]; then
	# AP6210 WiFi (BCM43362). The Cubietruck nvram is for the same AP6210
	# module. TODO(hw): check it (or the Armbian nvram_ap6210.txt) on the
	# RetroStone2: MAC address, antenna/tx power settings.
	link_fw brcmfmac43362-sdio.bin ../cypress/cyfmac43362-sdio.bin
	link_fw brcmfmac43362-sdio.txt brcmfmac43362-sdio.cubietech,cubietruck.txt
	# AP6212 fallback (BCM43430)
	link_fw brcmfmac43430-sdio.bin ../cypress/cyfmac43430-sdio.bin
	link_fw brcmfmac43430-sdio.clm_blob ../cypress/cyfmac43430-sdio.clm_blob
	link_fw brcmfmac43430-sdio.txt brcmfmac43430-sdio.AP6212.txt
	# AP6210 Bluetooth (BCM20710). btbcm has no name for this chip in its
	# table (the old DTS called it bcm20702a1), so cover the likely names.
	# TODO(hw): check in dmesg which name btbcm requests.
	link_fw BCM20702A1.hcd BCM20710A1.hcd
	link_fw BCM.hcd BCM20710A1.hcd
fi
