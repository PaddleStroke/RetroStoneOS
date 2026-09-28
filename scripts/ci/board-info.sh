#!/bin/sh
# What CI knows about one board, from its defconfig alone.
#   scripts/ci/board-info.sh <board>        (board = defconfig name without _defconfig)
# prints shell assignments:
#   BOARD       retrostone2_release
#   IMAGE       retrostone2          (file names: retrostoneos-<version>-<IMAGE>.img.xz)
#   NAME        RetroStone2          (board.ini "name", for the release notes)
#   STATUS      tested on hardware | community-tested
#   VARIANT     release | dev | default
#   RELEASE     yes | no             (built on a version tag)
#   BOARD_DIR   retrostone2          (buildroot-external/board/<BOARD_DIR>)
#
# Markers, as comment lines in the defconfig (docs/ci.md):
#   # ci: skip                   never built on a tag (still buildable by hand)
#   # ci: status=<text>          label in the release notes
# A defconfig <x>_defconfig for which <x>_release_defconfig exists is the
# development variant: only built by hand (workflow_dispatch).
# The status defaults to "community-tested", except for the boards in
# HW_TESTED (the RetroStoneOS developers run them on real hardware), or when
# the defconfig says "COMMUNITY-TESTED" anyway.
set -eu
HW_TESTED="retrostone2"

HERE=$(cd "$(dirname "$0")" && pwd)
CONFIGS=${RSOS_CI_CONFIGS:-$HERE/../../buildroot-external/configs}
EXT=$(cd "$CONFIGS/.." && pwd)
BOARD=${1:?usage: board-info.sh <board>}
BOARD=${BOARD%_defconfig}
DEF=$CONFIGS/${BOARD}_defconfig
[ -f "$DEF" ] || { echo "board-info: no $DEF" >&2; exit 1; }

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

q() { printf "%s='%s'\n" "$1" "$(printf '%s' "$2" | sed "s/'/'\\\\''/g")"; }
q BOARD "$BOARD"
q IMAGE "$IMAGE"
q NAME "$NAME"
q STATUS "$STATUS"
q VARIANT "$VARIANT"
q RELEASE "$RELEASE"
q BOARD_DIR "$BOARD_DIR"
