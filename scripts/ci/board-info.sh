#!/bin/sh
# What CI knows about one board, from its defconfig alone.
#   scripts/ci/board-info.sh <board>        (board = defconfig name without _defconfig)
# prints shell assignments:
#   BOARD       retrostone2_release
#   IMAGE       retrostone2          (file names: retrostoneos-<version>-<IMAGE>.img.xz)
#   NAME        RetroStone2          (for the release notes: "# ci: name=", else
#                                    NAMES below, else board.ini "name")
#   STATUS      tested on hardware | community-tested
#   VARIANT     release | dev | default
#               (default: a board without a separate release defconfig; CI
#               builds it as a release on a version tag, as a development
#               build otherwise: build-board.sh)
#   RELEASE     yes | no             (built on a version tag)
#   HEAVY       yes | no             (its cold build may not fit the build
#               step's 300 minutes on a standard runner: images.yml runs it on
#               IMAGES_RUNNER_HEAVY when set and never lets it block a release;
#               docs/ci.md, "Build times")
#   BOARD_DIR   retrostone2          (buildroot-external/board/<BOARD_DIR>)
#
# Markers, as comment lines in the defconfig (docs/ci.md):
#   # ci: skip                   never built on a tag (still buildable by hand)
#   # ci: status=<text>          label in the release notes
#   # ci: name=<text>            board name in the release notes
#   # ci: heavy                  see HEAVY (or list the board in HEAVY_BOARDS)
# A defconfig <x>_defconfig for which <x>_release_defconfig exists is the
# development variant: only built by hand (workflow_dispatch).
# The status defaults to "community-tested", except for the boards in
# HW_TESTED (the RetroStoneOS developers run them on real hardware), or when
# the defconfig says "COMMUNITY-TESTED" anyway.
set -eu
HW_TESTED="retrostone2"
# Cold build over 300 min on a 4-vCPU runner: the Orange Pi 5 compiles LLVM
# and Clang for the host and the target (Mesa's panfrost needs them in
# Buildroot 2026.02; docs/boards.md).
HEAVY_BOARDS="orangepi5"
# Release-notes names of boards that share a board folder, hence a board.ini:
# <defconfig name>=<name>, separated by "|".
NAMES="orangepi_h3_pc=Orange Pi PC / PC Plus|orangepi_h3_one=Orange Pi One / Lite"

HERE=$(cd "$(dirname "$0")" && pwd)
CONFIGS=${RSOS_CI_CONFIGS:-$HERE/../../buildroot-external/configs}
EXT=$(cd "$CONFIGS/.." && pwd)
BOARD=${1:?usage: board-info.sh <board>}
BOARD=${BOARD%_defconfig}
DEF=$CONFIGS/${BOARD}_defconfig
if [ ! -f "$DEF" ]; then
	known=
	for f in "$CONFIGS"/*_defconfig; do
		[ -f "$f" ] || continue
		f=${f##*/}
		known="$known ${f%_defconfig}"
	done
	echo "board-info: unknown board '$BOARD' (no configs/${BOARD}_defconfig); known:$known" >&2
	exit 1
fi

has_marker() { grep -Eq "^#[[:space:]]*ci:[[:space:]]*$1([=[:space:]]|\$)" "$DEF"; }

case $BOARD in
*_release) VARIANT=release; base=${BOARD%_release} ;;
*)
	base=$BOARD
	if [ -f "$CONFIGS/${BOARD}_release_defconfig" ]; then VARIANT=dev; else VARIANT=default; fi
	;;
esac
IMAGE=$(echo "$base" | tr '_' '-')
[ "$VARIANT" = dev ] && IMAGE=$IMAGE-dev

# The board folder: the last board/<x>/rootfs-overlay that is not common.
BOARD_DIR=$(sed -n 's/^BR2_ROOTFS_OVERLAY="\(.*\)"/\1/p' "$DEF" | tr ' ' '\n' |
	sed -n 's#.*/board/\([^/]*\)/rootfs-overlay$#\1#p' | grep -v '^common$' | tail -n 1 || true)
NAME=
INI=$EXT/board/$BOARD_DIR/rootfs-overlay/etc/rsos/board.ini
if [ -n "$BOARD_DIR" ] && [ -f "$INI" ]; then
	NAME=$(sed -n 's/^[[:space:]]*name[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p' "$INI" | head -n 1)
fi
ci_name=$(sed -n 's/^#[[:space:]]*ci:[[:space:]]*name=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p' "$DEF" | head -n 1)
[ -n "$ci_name" ] || ci_name=$(printf '%s\n' "$NAMES" | tr '|' '\n' | sed -n "s/^$base=//p" | head -n 1)
[ -z "$ci_name" ] || NAME=$ci_name
[ -n "$NAME" ] || NAME=$base
[ "$VARIANT" = dev ] && NAME="$NAME (development build)"

STATUS=$(sed -n 's/^#[[:space:]]*ci:[[:space:]]*status=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p' "$DEF" | head -n 1)
if [ -n "$STATUS" ]; then
	:
elif grep -qi '^#.*community-tested' "$DEF"; then
	STATUS=community-tested
else
	STATUS=community-tested
	for b in $HW_TESTED; do [ "$b" = "$BOARD_DIR" ] && STATUS="tested on hardware"; done
fi

RELEASE=yes
{ [ "$VARIANT" = dev ] || has_marker skip; } && RELEASE=no
HEAVY=no
has_marker heavy && HEAVY=yes
for b in $HEAVY_BOARDS; do [ "$b" = "$base" ] && HEAVY=yes; done

q() { printf "%s='%s'\n" "$1" "$(printf '%s' "$2" | sed "s/'/'\\\\''/g")"; }
q BOARD "$BOARD"
q IMAGE "$IMAGE"
q NAME "$NAME"
q STATUS "$STATUS"
q VARIANT "$VARIANT"
q RELEASE "$RELEASE"
q HEAVY "$HEAVY"
q BOARD_DIR "$BOARD_DIR"
