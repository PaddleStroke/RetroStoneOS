#!/bin/sh
# Every defconfig must load without losing a line: a symbol whose
# dependencies are not met (a core not available on that architecture, a
# renamed option) is silently dropped by kconfig, and the image then lacks
# it. For each configs/*_defconfig: make <board>_defconfig in a scratch
# output directory, then check that every BR2_ line of the defconfig (the
# last assignment when a symbol is set twice, as the release defconfig does)
# is in the .config, and that the RetroStone VC games switch of
# build-board.sh works.
#   scripts/ci/check-defconfigs.sh [board ...]
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
EXT=$REPO/buildroot-external
# shellcheck disable=SC1091
. "$HERE/buildroot.env"
WORK=${RSOS_CI_WORK:-$HOME/rsos-ci}
BR_DIR=${RSOS_CI_BR_DIR:-$WORK/buildroot-$BR_VERSION}
sh "$HERE/get-buildroot.sh" "$BR_DIR" > /dev/null || exit 2
T=$WORK/defconfig-check
rm -rf "$T"
mkdir -p "$T"

if [ $# -eq 0 ]; then
	for f in "$EXT"/configs/*_defconfig; do
		b=${f##*/}
		set -- "$@" "${b%_defconfig}"
	done
fi
bad=0
for b in "$@"; do
	b=${b%_defconfig}
	O=$T/$b
	if ! make -C "$BR_DIR" O="$O" BR2_EXTERNAL="$EXT" "${b}_defconfig" > "$O.log" 2>&1; then
		echo "FAIL $b: make ${b}_defconfig failed"
		tail -n 20 "$O.log"
		bad=1
		continue
	fi
	# expected lines: last assignment of each symbol
	grep -E '^(BR2_[A-Za-z0-9_]+=|# BR2_[A-Za-z0-9_]+ is not set$)' "$EXT/configs/${b}_defconfig" |
		awk '{ s = $0; sub(/^# /, "", s); sub(/[= ].*/, "", s); last[s] = $0; order[s] = NR }
			END { for (s in last) print order[s] "\t" last[s] }' | sort -n | cut -f 2- > "$O.expected"
	missing=$(grep -vxF -f "$O/.config" "$O.expected" | grep -v '^# ' || true)
	# "is not set" lines: the symbol must not be set
	unset_bad=$(sed -n 's/^# \(BR2_[A-Za-z0-9_]*\) is not set$/\1/p' "$O.expected" | while read -r s; do
		grep -q "^$s=" "$O/.config" && echo "$s (set, but the defconfig unsets it)"
	done)
	# the CI switch of the RetroStone VC games (build-board.sh): y (with its
	# source path) and n must both survive olddefconfig
	hb=ok
	for v in y n; do
		sed -i '/^BR2_PACKAGE_RSOS_VC_GAMES=/d; /^# BR2_PACKAGE_RSOS_VC_GAMES is not set$/d' "$O/.config"
		sed -i '/^BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR=/d' "$O/.config"
		if [ "$v" = y ]; then
			echo 'BR2_PACKAGE_RSOS_VC_GAMES=y' >> "$O/.config"
			echo 'BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR="/ci/.vc-games-src"' >> "$O/.config"
		else
			echo '# BR2_PACKAGE_RSOS_VC_GAMES is not set' >> "$O/.config"
		fi
		make -C "$BR_DIR" O="$O" olddefconfig > /dev/null 2>&1
		if [ "$v" = y ]; then
			grep -q '^BR2_PACKAGE_RSOS_VC_GAMES=y$' "$O/.config" &&
				grep -q '^BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR="/ci/.vc-games-src"$' "$O/.config" ||
				hb="BR2_PACKAGE_RSOS_VC_GAMES=y (with its source path) does not stick"
		else
			grep -q '^BR2_PACKAGE_RSOS_VC_GAMES=' "$O/.config" && hb="BR2_PACKAGE_RSOS_VC_GAMES=n does not stick"
		fi
	done
	if [ -z "$missing" ] && [ -z "$unset_bad" ] && [ "$hb" = ok ]; then
		echo "ok   $b ($(wc -l < "$O.expected") lines)"
	else
		bad=1
		echo "FAIL $b: lines of ${b}_defconfig not in the resulting .config:"
		[ -n "$missing" ] && echo "$missing" | sed 's/^/       /'
		[ -n "$unset_bad" ] && echo "$unset_bad" | sed 's/^/       /'
		[ "$hb" = ok ] || echo "       $hb"
		if [ "${GITHUB_ACTIONS:-}" = true ]; then
			echo "::error file=buildroot-external/configs/${b}_defconfig::symbols dropped by kconfig: $(echo "$missing $unset_bad" | tr '\n' ' ')"
		fi
	fi
done
exit "$bad"
