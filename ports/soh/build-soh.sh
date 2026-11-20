#!/bin/bash
# Builds The Legend of Zelda: Ocarina of Time (Ship of Harkinian) for RetroStoneOS
# from YOUR OWN ROM, on the PC (Linux/WSL), as a native game for the SD card
# (docs/native-games.md).
#
#   ports/soh/build-soh.sh <Ocarina of Time ROM (.z64, .v64 or .n64)> [output folder]
#
# Needs a built RetroStoneOS tree for the RetroStone2 with SDL2 and the port
# libraries (the RetroStone2 configs): ~/rsos/output, or RSOS_OUTPUT=<dir>;
# cmake, ninja, python3 and a C++ compiler for the PC (Torch, the asset
# extractor, runs here). Ship of Harkinian is cloned at a fixed release into
# ~/rsos/soh-work (RSOS_SOH_WORK) and patched (soh.patch,
# libultraship-gles2.patch).
#
# The result contains the game's data, taken from your ROM: keep it for
# yourself. Never put it in a release, an update or a repository.
#
# Output (default ./soh-retrostone2): roms/native/Ocarina of Time.sh and
# roms/native/soh/. Copy the "roms" folder onto the SD card's RETROSTONE
# partition (it merges with the roms folder there).
set -euo pipefail

REPO=https://github.com/HarbourMasters/Shipwright.git
COMMIT=f0599f96f8d442f26d0fa1c5cedafddacd56633f   # 9.3.1, libultraship da6daf1

here=$(cd "$(dirname "$0")" && pwd)
rom=${1:-}
out=${2:-$PWD/soh-retrostone2}
O=${RSOS_OUTPUT:-$HOME/rsos/output}
W=${RSOS_SOH_WORK:-$HOME/rsos/soh-work}

die() { echo "build-soh: $*" >&2; exit 1; }

[ -n "$rom" ] || die "usage: $0 <Ocarina of Time ROM (.z64, .v64 or .n64)> [output folder]"
[ -f "$rom" ] || die "$rom: no such file"
rom=$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")
case "$out" in /*) ;; *) out=$PWD/$out ;; esac
[ -x "$O/host/bin/arm-linux-gcc" ] || die "no cross compiler in $O/host/bin (build RetroStoneOS first)"
# libopus, libopusfile and libSDL2_net are also copied next to the game, for consoles
# whose RetroStoneOS does not have them yet
LIBS="libSDL2.so libzip.so libtinyxml2.so libspdlog.so libvorbisfile.so libopus.so.0 libopusfile.so.0 libSDL2_net-2.0.so.0"
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
git apply "$here/soh.patch"
git -C libultraship apply "$here/libultraship-gles2.patch"

# the ROM, in .z64 byte order (Torch reads that one only)
mkdir -p rsos-rom
rm -f rsos-rom/*
python3 - "$rom" rsos-rom/oot.z64 << 'EOF' || die "$rom is not an N64 ROM"
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

# Torch for this PC: oot.o2r (or oot-mq.o2r) from the ROM, and soh.o2r, which carries
# libultraship's (patched) shaders
cmake -S . -B build-tools -G Ninja -DSOH_TOOLS_ONLY=ON -DCMAKE_BUILD_TYPE=Release \
	-DSOH_ROM_PATH="$W/rsos-rom"
cmake --build build-tools --target ExtractAssets
cmake --build build-tools --target GenerateSohOtr
ls build-tools/soh/oot*.o2r > /dev/null 2>&1 ||
	die "no oot.o2r: $rom is not an Ocarina of Time ROM that Ship of Harkinian supports"

# the game for the console. CMAKE_SYSTEM_NAME Linux: Buildroot's toolchain file names
# it "Buildroot", which skips the Linux parts of SoH and libultraship. SDL2_net's CMake
# file (made by autotools) points at the PC's /usr: a copy pointing at the staging folder.
C=$W/build-gles2-cmake/SDL2_net
mkdir -p "$C"
sed -e "s#^set(exec_prefix \"/usr\")#set(exec_prefix \"$O/staging/usr\")#" \
	-e "s#^get_filename_component(prefix .*#set(prefix \"$O/staging/usr\")#" \
	"$O/staging/usr/lib/cmake/SDL2_net/sdl2_net-config.cmake" > "$C/sdl2_net-config.cmake"
cp "$O/staging/usr/lib/cmake/SDL2_net/sdl2_net-config-version.cmake" "$C/"
cmake -S . -B build-gles2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_OPENGLES=ON -DUSE_GLES2=ON \
	-DNON_PORTABLE=OFF -DSDL2_net_DIR="$C" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_SYSTEM_NAME=Linux
cmake --build build-gles2 --target soh

file build-gles2/soh/soh.elf | grep -q 'ARM' || die "build-gles2/soh/soh.elf is not an ARM program"
S=$out/roms/native/soh
mkdir -p "$S/lib" "$S/assets"
"$O/host/bin/arm-linux-strip" -o "$S/soh.elf" build-gles2/soh/soh.elf
cp build-tools/soh/oot*.o2r build-tools/soh/soh.o2r "$S/"
[ -f build-gles2/gamecontrollerdb.txt ] && cp build-gles2/gamecontrollerdb.txt "$S/"
for lib in libopus.so.0 libopusfile.so.0 libSDL2_net-2.0.so.0; do
	cp -L "$O/staging/usr/lib/$lib" "$S/lib/"
	"$O/host/bin/arm-linux-strip" "$S/lib/$lib"
done
# SoH quits at start without an assets/ folder next to the program; it reads it only to
# make oot.o2r on the console, which this script does on the PC instead
printf 'Ship of Harkinian checks that this folder exists. oot.o2r is made on the PC\n(ports/soh/build-soh.sh), so the extraction files are not needed here.\n' \
	> "$S/assets/README.txt"
install -m 0644 "$here/Ocarina of Time.sh" "$out/roms/native/Ocarina of Time.sh"
echo
echo "Done: $out/roms"
echo "Copy that \"roms\" folder onto the SD card's RETROSTONE partition, then in the menu:"
echo "Native games > Ocarina of Time. Hold Select + Start to quit."
