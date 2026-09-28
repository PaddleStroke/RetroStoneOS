#!/bin/sh
# Build the U-Boot host tools the A/B tests need (fw_printenv/fw_setenv,
# mkenvimage, mkimage) from the U-Boot release of retrostone2_defconfig,
# without a Buildroot build: the tarball is checked against
# board/retrostone2/patches/uboot/uboot.hash.
#   scripts/ci/uboot-host-tools.sh <bin directory>
# The tarball is taken from $BR2_DL_DIR/uboot/ when it is there, else
# downloaded from ftp.denx.de.
set -eu
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
EXT=$(cd "$HERE/../../buildroot-external" && pwd)
DEST=${1:?usage: uboot-host-tools.sh <bin directory>}
V=$(sed -n 's/^BR2_TARGET_UBOOT_CUSTOM_VERSION_VALUE="\(.*\)"/\1/p' "$EXT/configs/retrostone2_defconfig")
T=u-boot-$V.tar.bz2
SHA=$(sed -n "s/^sha256  *\([0-9a-f]\{64\}\)  *$T\$/\1/p" "$EXT/board/retrostone2/patches/uboot/uboot.hash")
if [ -z "$V" ] || [ -z "$SHA" ]; then
	echo "uboot-host-tools: no version or hash for $T" >&2
	exit 1
fi
W=${RSOS_CI_WORK:-$HOME/rsos-ci}/uboot-tools
mkdir -p "$W" "$DEST"
SRC=${BR2_DL_DIR:-$W}/uboot/$T
[ -f "$SRC" ] || SRC=$W/$T
if [ ! -f "$SRC" ]; then
	wget -q -O "$SRC.part" "https://ftp.denx.de/pub/u-boot/$T"
	mv "$SRC.part" "$SRC"
fi
echo "$SHA  $SRC" | sha256sum -c -
rm -rf "$W/u-boot-$V"
tar -xjf "$SRC" -C "$W"
cd "$W/u-boot-$V"
make -s tools-only_defconfig
# mkeficapsule needs gnutls: not wanted here
sed -i 's/^CONFIG_TOOLS_MKEFICAPSULE=y$/# CONFIG_TOOLS_MKEFICAPSULE is not set/' .config
make -s olddefconfig
make -s -j"$(nproc)" tools-only envtools > "$W/build.log" 2>&1 || { tail -n 30 "$W/build.log"; exit 1; }
install -m 0755 tools/env/fw_printenv tools/mkenvimage tools/mkimage "$DEST/"
ln -sf fw_printenv "$DEST/fw_setenv"
echo "U-Boot $V host tools in $DEST: fw_printenv fw_setenv mkenvimage mkimage"
