#!/bin/sh
# RetroStoneOS post-build script, Orange Pi 5 part (after
# board/common/post-build.sh). Called as: post-build.sh <TARGET_DIR>;
# HOST_DIR, BR2_CONFIG... are in the environment.
set -e

TARGET="$1"
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
DTB=rockchip/rk3588s-orangepi-5.dtb
if [ ! -f "$TARGET/boot/$DTB" ]; then
	echo "post-build.sh: $TARGET/boot/$DTB is missing" >&2
	exit 1
fi

# Mesa panfrost pulls LLVM, Clang, libclc and the SPIR-V tools into the
# target (Buildroot 2026.02: BR2_PACKAGE_MESA3D_LLVM, the precompiled shader
# compiler). At run time only libgallium needs libLLVM; the rest (about
# 200 MiB) is build-time only: remove it, then check that every library
# libgallium needs is still there.
rm -rf "$TARGET"/usr/lib/libclang.so* "$TARGET"/usr/lib/libclang-cpp.so* \
	"$TARGET"/usr/lib/libLTO.so* "$TARGET"/usr/lib/libRemarks.so* \
	"$TARGET"/usr/lib/libLLVMSPIRVLib.so* "$TARGET/usr/lib/clang" "$TARGET/usr/share/clc" \
	"$TARGET"/usr/bin/llvm-spirv "$TARGET"/usr/bin/spirv-* "$TARGET/usr/bin/diagtool" \
	"$TARGET"/usr/bin/*-arch "$TARGET"/usr/bin/clang*
READELF=
for r in "$HOST_DIR"/bin/*-readelf; do
	[ -x "$r" ] && READELF=$r && break
done
for lib in "$TARGET"/usr/lib/libgallium-*.so; do
	[ -f "$lib" ] || continue
	for n in $("$READELF" -d "$lib" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		if [ ! -e "$TARGET/usr/lib/$n" ] && [ ! -e "$TARGET/lib/$n" ]; then
			echo "post-build.sh: ${lib##*/} needs $n, which was removed" >&2
			exit 1
		fi
	done
done

# /boot/boot.scr (the A/B boot script, board/sbc-uboot/boot.cmd.in). The
# kernel is the uncompressed arm64 Image (booti).
RSOS_KERNEL=Image RSOS_BOOTM=booti RSOS_MMC=0 RSOS_DISK=mmcblk0 \
	RSOS_CONSOLE="console=ttyS2,1500000" RSOS_FDT_DEFAULT="$DTB" \
	sh "$BOARD_DIR/../sbc-uboot/mk-boot-scr.sh" "$TARGET"
