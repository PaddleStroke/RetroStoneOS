#!/bin/bash
# Builds Mario Kart 64 (SpaghettiKart) for RetroStoneOS from YOUR OWN ROM, on the PC
# (Linux/WSL), as a native game for the SD card (docs/native-games.md).
#
#   ports/spaghettikart/build-spaghettikart.sh <Mario Kart 64 US ROM (.z64, .v64 or .n64)> [output folder]
#
# Needs a built RetroStoneOS tree for the RetroStone2 with SDL2 and the port
# libraries (the RetroStone2 configs): ~/rsos/output, or RSOS_OUTPUT=<dir>; cmake,
# ninja, python3 and a C++ compiler for the PC (Torch, the asset extractor, runs here).
# SpaghettiKart is fetched at a fixed commit into ~/rsos/spaghettikart-work
# (RSOS_SPAGHETTIKART_WORK) and patched (spaghettikart.patch, libultraship-gles2.patch).
#
# The result contains the game's data, taken from your ROM: keep it for
# yourself. Never put it in a release, an update or a repository.
#
# Output (default ./spaghettikart-retrostone2): roms/native/Mario Kart 64.sh and
# roms/native/spaghettikart/. Copy the "roms" folder onto the SD card's RETROSTONE
# partition (it merges with the roms folder there).
set -euo pipefail

REPO=https://github.com/HarbourMasters/SpaghettiKart.git
COMMIT=ce0f9358f0505bbdeb76b1aff7a25556cda3fdef   # libultraship 60c9bb7, Torch 2d474dd
ROM_SHA1=579c48e211ae952530ffc8738709f078d5dd215e  # Mario Kart 64 (US), .z64 order

here=$(cd "$(dirname "$0")" && pwd)
rom=${1:-}
out=${2:-$PWD/spaghettikart-retrostone2}
O=${RSOS_OUTPUT:-$HOME/rsos/output}
W=${RSOS_SPAGHETTIKART_WORK:-$HOME/rsos/spaghettikart-work}

die() { echo "build-spaghettikart: $*" >&2; exit 1; }

[ -n "$rom" ] || die "usage: $0 <Mario Kart 64 US ROM (.z64, .v64 or .n64)> [output folder]"
[ -f "$rom" ] || die "$rom: no such file"
rom=$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")
case "$out" in /*) ;; *) out=$PWD/$out ;; esac
[ -x "$O/host/bin/arm-linux-gcc" ] || die "no cross compiler in $O/host/bin (build RetroStoneOS first)"
for lib in libSDL2.so libzip.so libtinyxml2.so libspdlog.so libvorbisfile.so; do
	[ -e "$O/staging/usr/lib/$lib" ] || die "$lib is not in $O/staging (build RetroStoneOS with the RetroStone2 configs)"
done
TOOLCHAIN=$O/host/share/buildroot/toolchainfile.cmake

# only the fixed commit (the whole history is large)
if [ ! -d "$W/.git" ]; then
	git init -q "$W"
	git -C "$W" remote add origin "$REPO"
fi
cd "$W"
git fetch -q --depth 1 origin "$COMMIT"
# a clean tree at the fixed commit, then our changes (the build folder, the
# ROM and the .o2r files are ignored by git and stay)
git checkout -q -f "$COMMIT"
git submodule update -q --init --force --depth 1 libultraship torch
git -C libultraship checkout -q -f .
git apply "$here/spaghettikart.patch"
git -C libultraship apply "$here/libultraship-gles2.patch"

# the ROM, in .z64 byte order, as baserom.us.z64 (the only one SpaghettiKart takes)
python3 - "$rom" baserom.us.z64 << 'EOF' || die "$rom is not an N64 ROM"
import sys
data = bytearray(open(sys.argv[1], 'rb').read())
head = bytes(data[:4])
if head == b'\x37\x80\x40\x12':    # .v64: 16-bit words swapped
    data[0::2], data[1::2] = data[1::2], data[0::2]
elif head == b'\x40\x12\x37\x80':  # .n64: 32-bit words reversed
    for i in range(0, len(data) - 3, 4):
        data[i:i + 4] = data[i:i + 4][::-1]
elif head != b'\x80\x37\x12\x40':
    sys.exit(1)
open(sys.argv[2], 'wb').write(data)
EOF
sum=$(sha1sum baserom.us.z64 | cut -d' ' -f1)
[ "$sum" = "$ROM_SHA1" ] ||
	die "$rom is not the Mario Kart 64 (US) ROM SpaghettiKart takes (SHA-1 $sum in .z64 order, needs $ROM_SHA1; a modified dump such as [h] or [t] does not work)"

# the game for the console. CMAKE_SYSTEM_NAME Linux: Buildroot's toolchain file names it
# "Buildroot", which skips libultraship's Linux parts. Torch is built for this PC (its own
# sub-build) and makes mk64.o2r from the ROM and spaghetti.o2r (with the patched shaders).
# Release flags: Buildroot's toolchain file builds with -O2, and GCC uses NEON for float math on
# 32-bit ARM only with unsafe math; libultraship's vertex and triangle work is the busiest code.
fast="-O3 -DNDEBUG -fno-math-errno -funsafe-math-optimizations"
cmake -S . -B build-gles2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_OPENGLES=ON -DUSE_GLES2=ON \
	-DCMAKE_C_FLAGS_RELEASE="$fast" -DCMAKE_CXX_FLAGS_RELEASE="$fast" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_SYSTEM_NAME=Linux
cmake --build build-gles2 --target Spaghettify
cmake --build build-gles2 --target ExtractAssets

file build-gles2/Spaghettify | grep -q 'ARM' || die "build-gles2/Spaghettify is not an ARM program"
S=$out/roms/native/spaghettikart
mkdir -p "$S"
"$O/host/bin/arm-linux-strip" -o "$S/Spaghettify" build-gles2/Spaghettify
cp build-gles2/mk64.o2r build-gles2/spaghetti.o2r "$S/"
cp -r build-gles2/config.yml build-gles2/yamls build-gles2/meta "$S/"
[ -f build-gles2/gamecontrollerdb.txt ] && cp build-gles2/gamecontrollerdb.txt "$S/"
install -m 0644 "$here/Mario Kart 64.sh" "$out/roms/native/Mario Kart 64.sh"
echo
echo "Done: $out/roms"
echo "Copy that \"roms\" folder onto the SD card's RETROSTONE partition, then in the menu:"
echo "Native games > Mario Kart 64. Hold Select + Start to quit."
