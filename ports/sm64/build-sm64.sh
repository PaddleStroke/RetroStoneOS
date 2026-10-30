#!/bin/bash
# Builds Super Mario 64 (sm64-port) for RetroStoneOS from YOUR OWN ROM, on the
# PC (Linux/WSL), as a native game for the SD card (docs/native-games.md).
#
#   ports/sm64/build-sm64.sh <baserom.us.z64> [output folder]
#
# Needs a built RetroStoneOS tree with SDL2 (the cross compiler and the
# staging libraries): ~/rsos/output, or RSOS_OUTPUT=<dir>. The sm64-port
# source is cloned at a fixed commit into ~/rsos/sm64-work (RSOS_SM64_WORK)
# and patched (retrostone2.patch, controller_rsos.c).
#
# The result contains the game's data, taken from your ROM: keep it for
# yourself. Never put it in a release, an update or a repository.
#
# Output (default ./sm64-retrostone2): roms/native/Super Mario 64.sh and
# roms/native/sm64/sm64.us. Copy the "roms" folder onto the SD card's
# RETROSTONE partition (it merges with the roms folder there).
set -euo pipefail

SM64_REPO=https://github.com/sm64-port/sm64-port.git
SM64_COMMIT=2b17d08   # "Fix GCC 14 build on MinGW"
US_SHA1=9bef1128717f958171a4afac3ed78ee2bb4e86ce

here=$(cd "$(dirname "$0")" && pwd)
rom=${1:-}
out=${2:-$PWD/sm64-retrostone2}
O=${RSOS_OUTPUT:-$HOME/rsos/output}
W=${RSOS_SM64_WORK:-$HOME/rsos/sm64-work}

die() { echo "build-sm64: $*" >&2; exit 1; }

[ -n "$rom" ] || die "usage: $0 <baserom.us.z64> [output folder]"
[ -f "$rom" ] || die "$rom: no such file"
rom=$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")
case "$out" in /*) ;; *) out=$PWD/$out ;; esac
sum=$(sha1sum "$rom" | cut -d' ' -f1)
[ "$sum" = "$US_SHA1" ] || die "$rom is not the US Super Mario 64 ROM in .z64 (big-endian) order (SHA-1 $sum)"
[ -x "$O/host/bin/arm-linux-gcc" ] || die "no cross compiler in $O/host/bin (build RetroStoneOS first)"
SDL2_CONFIG=$O/staging/usr/bin/sdl2-config
[ -x "$SDL2_CONFIG" ] || die "no SDL2 in $O/staging (build RetroStoneOS with BR2_PACKAGE_SDL2)"

if [ ! -d "$W/.git" ]; then
	git clone -q "$SM64_REPO" "$W"
fi
cd "$W"
git fetch -q origin 2> /dev/null || true
# a clean tree at the fixed commit, then our changes (the extracted assets
# and the build folder stay: they are ignored by git)
git checkout -q -f "$SM64_COMMIT"
git clean -q -f -- src tools Makefile
git apply "$here/retrostone2.patch"
cp "$here/controller_rsos.c" "$here/controller_rsos.h" src/pc/controller/
cp "$rom" baserom.us.z64

# the asset tools for this PC (with its own compiler), then the game for the
# console (CROSS_PC: the game's compiler only)
make -C tools -j"$(nproc)" all-except-recomp
make -j"$(nproc)" VERSION=us TARGET_LINUX_KMSDRM=1 CROSS_PC="$O/host/bin/arm-linux-" SDL2_CONFIG="$SDL2_CONFIG"

file build/us_pc/sm64.us | grep -q 'ARM' || die "build/us_pc/sm64.us is not an ARM program"
mkdir -p "$out/roms/native/sm64"
"$O/host/bin/arm-linux-strip" -o "$out/roms/native/sm64/sm64.us" build/us_pc/sm64.us
install -m 0644 "$here/Super Mario 64.sh" "$out/roms/native/Super Mario 64.sh"
echo
echo "Done: $out/roms"
echo "Copy that \"roms\" folder onto the SD card's RETROSTONE partition, then in the menu:"
echo "Native games > Super Mario 64. Select + Start quits."
