#!/bin/sh
# The boards to build, as a GitHub Actions matrix (JSON on stdout).
#   scripts/ci/list-boards.sh [selection]
# selection:
#   release (default)  every defconfig that board-info.sh marks RELEASE=yes
#                      (no "# ci: skip", not a development variant)
#   all                every defconfig
#   <names>            a list separated by spaces or commas, with or without
#                      "_defconfig" (e.g. "retrostone2 rpi4_64")
# Output: {"include":[{"board":..,"image":..,"name":..,"status":..,"heavy":true|false}, ...]}
# (heavy: board-info.sh HEAVY, the runner choice and continue-on-error of images.yml)
# With RSOS_CI_FORMAT=names: one board per line instead.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CONFIGS=${RSOS_CI_CONFIGS:-$HERE/../../buildroot-external/configs}
SEL=$(printf '%s' "${*:-release}" | tr ',' ' ')

all_boards() {
	for f in "$CONFIGS"/*_defconfig; do
		[ -f "$f" ] || continue
		b=${f##*/}
		echo "${b%_defconfig}"
	done
}

case $SEL in
release | all) CANDIDATES=$(all_boards) ;;
*)
	CANDIDATES=
	for b in $SEL; do
		b=${b%_defconfig}
		[ -f "$CONFIGS/${b}_defconfig" ] || {
			echo "list-boards: no such board: $b (known: $(all_boards | tr '\n' ' '))" >&2
			exit 1
		}
		case " $CANDIDATES " in *" $b "*) ;; *) CANDIDATES="$CANDIDATES $b" ;; esac
	done
	;;
esac

json() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
out=
names=
for b in $CANDIDATES; do
	info=$(RSOS_CI_CONFIGS=$CONFIGS sh "$HERE/board-info.sh" "$b")
	eval "$info"
	[ "$SEL" = release ] && [ "$RELEASE" != yes ] && continue
	names="$names$BOARD
"
	case $HEAVY in yes) h=true ;; *) h=false ;; esac
	entry=$(printf '{"board":"%s","image":"%s","name":"%s","status":"%s","heavy":%s}' \
		"$(json "$BOARD")" "$(json "$IMAGE")" "$(json "$NAME")" "$(json "$STATUS")" "$h")
	out=${out:+$out,}$entry
done
[ -n "$out" ] || { echo "list-boards: nothing to build for '$SEL'" >&2; exit 1; }
if [ "${RSOS_CI_FORMAT:-json}" = names ]; then
	printf '%s' "$names"
else
	printf '{"include":[%s]}\n' "$out"
fi
