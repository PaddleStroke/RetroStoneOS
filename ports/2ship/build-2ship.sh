#!/bin/bash
# Builds The Legend of Zelda: Majora's Mask (2Ship2Harkinian) for RetroStoneOS from
# YOUR OWN ROM, on the PC (Linux/WSL), as a native game for the SD card
# (docs/native-games.md).
#
#   ports/2ship/build-2ship.sh <Majora's Mask US ROM (.z64, .v64 or .n64)> [output folder]
#
# Needs a built RetroStoneOS tree for the RetroStone2 with SDL2 and the port
# libraries (the RetroStone2 configs): ~/rsos/output, or RSOS_OUTPUT=<dir>; cmake,
# ninja, python3, a C++ compiler for the PC and the PC libraries of 2Ship's extractor
# (ZAPD, which runs here and is built with libultraship): see README.md.
# 2Ship2Harkinian is fetched at a fixed commit into ~/rsos/2ship-work
# (RSOS_2SHIP_WORK) and patched (2ship.patch, libultraship-gles2.patch).
# RSOS_HOST_CMAKE_ARGS: extra cmake arguments for the PC build (libraries outside /usr).
#
# The result contains the game's data, taken from your ROM: keep it for
# yourself. Never put it in a release, an update or a repository.
#
# Output (default ./2ship-retrostone2): roms/native/Majora's Mask.sh and
# roms/native/2ship/. Copy the "roms" folder onto the SD card's RETROSTONE
# partition (it merges with the roms folder there).
set -euo pipefail

REPO=https://github.com/HarbourMasters/2ship2harkinian.git
COMMIT=8a24047fbce8915993804e7819f4df4fa591551f   # 5.0.1 + fixes, libultraship 7cb1022
LIBPNG_REPO=https://github.com/pnggroup/libpng.git
LIBPNG_COMMIT=95ab3fdca83ea294efd3b092e9a53c5a39886444   # v1.6.57

here=$(cd "$(dirname "$0")" && pwd)
rom=${1:-}
out=${2:-$PWD/2ship-retrostone2}
O=${RSOS_OUTPUT:-$HOME/rsos/output}
W=${RSOS_2SHIP_WORK:-$HOME/rsos/2ship-work}

die() { echo "build-2ship: $*" >&2; exit 1; }

[ -n "$rom" ] || die "usage: $0 <Majora's Mask US ROM (.z64, .v64 or .n64)> [output folder]"
[ -f "$rom" ] || die "$rom: no such file"
rom=$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")
case "$out" in /*) ;; *) out=$PWD/$out ;; esac
[ -x "$O/host/bin/arm-linux-gcc" ] || die "no cross compiler in $O/host/bin (build RetroStoneOS first)"
# libopus and libopusfile are also copied next to the game, for consoles whose
# RetroStoneOS does not have them yet
LIBS="libSDL2.so libzip.so libtinyxml2.so libspdlog.so libvorbisfile.so libopus.so.0 libopusfile.so.0"
for lib in $LIBS; do
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
# a clean tree at the fixed commit, then our changes (the build folders and the
# ROM are ignored by git and stay)
git checkout -q -f "$COMMIT"
git submodule update -q --init --recursive --force --depth 1
git -C libultraship checkout -q -f .
git apply "$here/2ship.patch"
git -C libultraship apply "$here/libultraship-gles2.patch"

# the ROM, in .z64 byte order, in OTRExporter/ (where the extractor looks for it)
rm -f OTRExporter/*.z64
python3 - "$rom" OTRExporter/baserom.z64 << 'EOF' || die "$rom is not an N64 ROM"
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

# ZAPD for this PC: mm.o2r from the ROM, and 2ship.o2r, which carries the (patched) shaders.
# 2Ship has no tools-only build: this configures the whole game for the PC, and builds ZAPD only.
rm -f mm/mm.o2r mm/2ship.o2r mm.o2r 2ship.o2r
# shellcheck disable=SC2086
cmake -S . -B build-tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_OPENGLES=ON -DUSE_GLES2=ON \
	-DNON_PORTABLE=OFF ${RSOS_HOST_CMAKE_ARGS:-}
cmake --build build-tools --target ExtractAssets ||
	die "the extraction failed: is $rom Majora's Mask (US), N64 or GameCube?"
[ -f build-tools/mm/mm.o2r ] && [ -f build-tools/mm/2ship.o2r ] ||
	die "no mm.o2r: $rom is not a Majora's Mask ROM that 2Ship2Harkinian supports"

# The game links ZAPD's library (its own extractor), which needs libpng. RetroStoneOS has
# none: a static one, built with the console's compiler from libpng's repository.
P=$W-libpng
if [ ! -f "$P/arm/lib/libpng16.a" ]; then
	rm -rf "$P"
	git init -q "$P/src"
	git -C "$P/src" fetch -q --depth 1 "$LIBPNG_REPO" "$LIBPNG_COMMIT"
	git -C "$P/src" checkout -q FETCH_HEAD
	cmake -S "$P/src" -B "$P/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
		-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF \
		-DCMAKE_INSTALL_PREFIX="$P/arm"
	cmake --build "$P/build" --target install
fi

# the game for the console. CMAKE_SYSTEM_NAME Linux: Buildroot's toolchain file names
# it "Buildroot", which skips the Linux parts of 2Ship and libultraship. The release
# flags (-O3, unsafe math for NEON) are in 2ship.patch: 2Ship sets its own on Linux.
cmake -S . -B build-gles2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_OPENGLES=ON -DUSE_GLES2=ON \
	-DNON_PORTABLE=OFF -DPNG_LIBRARY="$P/arm/lib/libpng16.a" -DPNG_PNG_INCLUDE_DIR="$P/arm/include" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_SYSTEM_NAME=Linux
cmake --build build-gles2 --target 2ship

file build-gles2/mm/2s2h.elf | grep -q 'ARM' || die "build-gles2/mm/2s2h.elf is not an ARM program"
S=$out/roms/native/2ship
mkdir -p "$S/lib" "$S/assets"
"$O/host/bin/arm-linux-strip" -o "$S/2s2h.elf" build-gles2/mm/2s2h.elf
cp build-tools/mm/mm.o2r build-tools/mm/2ship.o2r "$S/"
[ -f build-gles2/gamecontrollerdb.txt ] && cp build-gles2/gamecontrollerdb.txt "$S/"
for lib in libopus.so.0 libopusfile.so.0; do
	cp -L "$O/staging/usr/lib/$lib" "$S/lib/"
	"$O/host/bin/arm-linux-strip" "$S/lib/$lib"
done
# 2Ship quits at start without an assets/ folder next to the program; it reads it only to
# make mm.o2r on the console, which this script does on the PC instead
printf '2Ship2Harkinian checks that this folder exists. mm.o2r is made on the PC\n(ports/2ship/build-2ship.sh), so the extraction files are not needed here.\n' \
	> "$S/assets/README.txt"
install -m 0644 "$here/Majora's Mask.sh" "$out/roms/native/Majora's Mask.sh"
echo
echo "Done: $out/roms"
echo "Copy that \"roms\" folder onto the SD card's RETROSTONE partition, then in the menu:"
echo "Native games > Majora's Mask. Hold Select + Start to quit."
