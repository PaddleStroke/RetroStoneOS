#!/bin/bash
# Builds GoldenEye 007 (Project Midas) for RetroStoneOS from YOUR OWN ROM, on the PC
# (Linux/WSL), as a native game for the SD card (docs/native-games.md).
#
#   ports/goldeneye/build-goldeneye.sh <GoldenEye 007 US ROM (.z64, .v64 or .n64)> [output folder]
#
# Needs a built RetroStoneOS tree for the RetroStone2 with SDL2 (the RetroStone2
# configs): ~/rsos/output, or RSOS_OUTPUT=<dir>; cmake, ninja, make, python3 and a C
# compiler for the PC (the asset extraction runs here). Project Midas is fetched at a
# fixed commit into ~/rsos/goldeneye-work (RSOS_GOLDENEYE_WORK) and patched
# (goldeneye.patch).
#
# The result contains the game's data, taken from your ROM (the game also reads the
# ROM itself at every start, from goldeneye/Data/): keep it for yourself. Never put
# it in a release, an update or a repository.
#
# Output (default ./goldeneye-retrostone2): roms/native/GoldenEye 007.sh and
# roms/native/goldeneye/. Copy the "roms" folder onto the SD card's RETROSTONE
# partition (it merges with the roms folder there).
set -euo pipefail

REPO=https://github.com/murkantor/Project-Midas.git
COMMIT=9062f9f120b7f64d39e5db59500ed50e8e7a6686   # Project Midas 1.0.2b
ROM_SHA1=abe01e4aeb033b6c0836819f549c791b26cfde83  # GoldenEye 007 (US), .z64 order

here=$(cd "$(dirname "$0")" && pwd)
rom=${1:-}
out=${2:-$PWD/goldeneye-retrostone2}
O=${RSOS_OUTPUT:-$HOME/rsos/output}
W=${RSOS_GOLDENEYE_WORK:-$HOME/rsos/goldeneye-work}

die() { echo "build-goldeneye: $*" >&2; exit 1; }

[ -n "$rom" ] || die "usage: $0 <GoldenEye 007 US ROM (.z64, .v64 or .n64)> [output folder]"
[ -f "$rom" ] || die "$rom: no such file"
rom=$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")
case "$out" in /*) ;; *) out=$PWD/$out ;; esac
[ -x "$O/host/bin/arm-linux-gcc" ] || die "no cross compiler in $O/host/bin (build RetroStoneOS first)"
[ -e "$O/staging/usr/lib/libSDL2.so" ] || die "libSDL2.so is not in $O/staging (build RetroStoneOS with the RetroStone2 configs)"
TOOLCHAIN=$O/host/share/buildroot/toolchainfile.cmake

# only the fixed commit (the whole history is large)
if [ ! -d "$W/.git" ]; then
	git init -q "$W"
	git -C "$W" remote add origin "$REPO"
fi
cd "$W"
git fetch -q --depth 1 origin "$COMMIT"
# a clean tree at the fixed commit, then our changes (the build folder, the ROM and
# the extracted assets are ignored by git and stay)
git checkout -q -f "$COMMIT"
git apply "$here/goldeneye.patch"

# the ROM, in .z64 byte order, as baserom.u.z64 (where the extraction reads it)
python3 - "$rom" baserom.u.z64 << 'EOF' || die "$rom is not an N64 ROM"
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
sum=$(sha1sum baserom.u.z64 | cut -d' ' -f1)
[ "$sum" = "$ROM_SHA1" ] ||
	die "$rom is not the GoldenEye 007 (US) ROM Project Midas takes (SHA-1 $sum in .z64 order, needs $ROM_SHA1)"

# the assets the build compiles in or includes (images, music, demos), with tools built for this PC
TERM=${TERM:-dumb} ./scripts/extract_baserom.u.sh
make convert_props convert_chrs convert_guns combine_images

# the game for the console: the fast3d renderer on OpenGL ES 2.0 (GE_GLES2). CMAKE_SYSTEM_NAME
# Linux: Buildroot's toolchain file names it "Buildroot". 32-bit ARM is the same ILP32
# little-endian model Midas is written for (its PC builds are 32-bit x86).
cmake -S . -B build-gles2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DGE_VIDEO=ON -DGE_GLES2=ON \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_SYSTEM_NAME=Linux
cmake --build build-gles2

file build-gles2/ProjectMidas | grep -q 'ARM' || die "build-gles2/ProjectMidas is not an ARM program"
G=$out/roms/native/goldeneye
mkdir -p "$G/Data"
"$O/host/bin/arm-linux-strip" -o "$G/ProjectMidas" build-gles2/ProjectMidas
cp tools/gamecontrollerdb.txt tools/gamecontrollerdefault.txt tools/midas_ini_help.txt "$G/"
cp tools/shaders_base.txt "$G/Data/"
cp tools/Data_README.txt "$G/Data/README.txt"
# the game reads its levels, models and sounds from the ROM at every start
cp baserom.u.z64 "$G/Data/ge007.u.z64"
install -m 0644 "$here/GoldenEye 007.sh" "$out/roms/native/GoldenEye 007.sh"
echo
echo "Done: $out/roms"
echo "Copy that \"roms\" folder onto the SD card's RETROSTONE partition, then in the menu:"
echo "Native games > GoldenEye 007. Hold Select + Start to quit."
