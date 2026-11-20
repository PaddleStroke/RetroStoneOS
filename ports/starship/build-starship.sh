#!/bin/bash
# Builds Star Fox 64 (Starship) for RetroStoneOS from YOUR OWN ROM, on the PC
# (Linux/WSL), as a native game for the SD card (docs/native-games.md).
#
#   ports/starship/build-starship.sh <Star Fox 64 ROM (.z64)> [output folder]
#
# Needs a built RetroStoneOS tree for the RetroStone2 with SDL2 and the port
# libraries (the RetroStone2 configs): ~/rsos/output, or RSOS_OUTPUT=<dir>; cmake,
# ninja and a C++ compiler for the PC (Torch, the asset extractor, runs here).
# Starship is cloned at a fixed commit into ~/rsos/starship-work
# (RSOS_STARSHIP_WORK) and patched (starship.patch, libultraship-gles2.patch).
#
# The result contains the game's data, taken from your ROM: keep it for
# yourself. Never put it in a release, an update or a repository.
#
# Output (default ./starship-retrostone2): roms/native/Star Fox 64.sh and
# roms/native/starship/. Copy the "roms" folder onto the SD card's
# RETROSTONE partition (it merges with the roms folder there).
set -euo pipefail

REPO=https://github.com/HarbourMasters/Starship.git
COMMIT=3322643f0bbf80d2f8f7e1c0d2c4c578515f6c90   # libultraship eaaf9d0, Torch cd92cc0

here=$(cd "$(dirname "$0")" && pwd)
rom=${1:-}
out=${2:-$PWD/starship-retrostone2}
O=${RSOS_OUTPUT:-$HOME/rsos/output}
W=${RSOS_STARSHIP_WORK:-$HOME/rsos/starship-work}

die() { echo "build-starship: $*" >&2; exit 1; }

[ -n "$rom" ] || die "usage: $0 <Star Fox 64 ROM (.z64)> [output folder]"
[ -f "$rom" ] || die "$rom: no such file"
rom=$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")
case "$out" in /*) ;; *) out=$PWD/$out ;; esac
[ -x "$O/host/bin/arm-linux-gcc" ] || die "no cross compiler in $O/host/bin (build RetroStoneOS first)"
for lib in libSDL2.so libzip.so libtinyxml2.so libspdlog.so libvorbisfile.so; do
	[ -e "$O/staging/usr/lib/$lib" ] || die "$lib is not in $O/staging (build RetroStoneOS with the RetroStone2 configs)"
done
TOOLCHAIN=$O/host/share/buildroot/toolchainfile.cmake

if [ ! -d "$W/.git" ]; then
	git clone -q "$REPO" "$W"
fi
cd "$W"
git fetch -q origin 2> /dev/null || true
# a clean tree at the fixed commit, then our changes (the build folder, the
# ROM and the .o2r files are ignored by git and stay)
git checkout -q -f "$COMMIT"
git submodule update -q --init --recursive --force
git -C libultraship checkout -q -f .
git apply "$here/starship.patch"
git -C libultraship apply "$here/libultraship-gles2.patch"

# the ROM: a supported Star Fox 64 (its SHA-1 is a key of config.yml)
sum=$(sha1sum "$rom" | cut -d' ' -f1)
grep -q "^$sum:" config.yml || die "$rom is not a Star Fox 64 ROM that Starship supports, in .z64 order (SHA-1 $sum)"
cp "$rom" baserom.z64

# the game for the console, then Torch for this PC and the two archives
cmake -S . -B build-gles2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_OPENGLES=ON -DUSE_GLES2=ON \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
cmake --build build-gles2 --target Starship
cmake --build build-gles2 --target ExtractAssets
cmake --build build-gles2 --target GeneratePortO2R

file build-gles2/Starship | grep -q 'ARM' || die "build-gles2/Starship is not an ARM program"
mkdir -p "$out/roms/native/starship"
"$O/host/bin/arm-linux-strip" -o "$out/roms/native/starship/Starship" build-gles2/Starship
cp build-gles2/sf64.o2r build-gles2/starship.o2r "$out/roms/native/starship/"
[ -f build-gles2/gamecontrollerdb.txt ] && cp build-gles2/gamecontrollerdb.txt "$out/roms/native/starship/"
install -m 0644 "$here/Star Fox 64.sh" "$out/roms/native/Star Fox 64.sh"
echo
echo "Done: $out/roms"
echo "Copy that \"roms\" folder onto the SD card's RETROSTONE partition, then in the menu:"
echo "Native games > Star Fox 64. Select + Start quits."
