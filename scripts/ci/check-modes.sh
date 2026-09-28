#!/bin/sh
# Scripts must be executable in git: Buildroot runs the post-build and
# post-image scripts directly, and the rootfs overlays copy their modes into
# the image. A commit made from Windows (core.fileMode=false) records new
# files as 100644, which breaks the build (or an init script) only on Linux.
#   scripts/ci/check-modes.sh          check (git index when available, else the files)
#   scripts/ci/check-modes.sh --list   print the paths that must be executable
# To fix the index before a commit (any OS):
#   git update-index --chmod=+x $(sh scripts/ci/check-modes.sh --list)
# Checked: every file with a "#!" first line under buildroot-external/
# (except patches) and scripts/, plus frontend/tests/*.sh.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO" || exit 2

list() {
	find buildroot-external scripts frontend/tests -type f ! -name '*.patch' ! -path '*/.git/*' 2>/dev/null |
		while IFS= read -r f; do
			[ "$(head -c 2 "$f")" = '#!' ] && echo "$f"
		done | sort
}
if [ "${1:-}" = --list ]; then
	list
	exit 0
fi

bad=0
n=0
tracked=0
git rev-parse --verify -q HEAD > /dev/null 2>&1 && tracked=1
L=$(mktemp)
trap 'rm -f "$L"' EXIT
list > "$L"
while IFS= read -r f; do
	n=$((n + 1))
	mode=
	if [ "$tracked" = 1 ]; then
		mode=$(git ls-files -s -- "$f" | cut -d' ' -f1)
	fi
	if [ -n "$mode" ]; then
		[ "$mode" = 100755 ] && continue
	elif [ -x "$f" ]; then
		continue
	fi
	echo "not executable${mode:+ in git ($mode)}: $f"
	[ "${GITHUB_ACTIONS:-}" = true ] && echo "::error file=$f::not executable: run git update-index --chmod=+x $f"
	bad=$((bad + 1))
done < "$L"
echo "check-modes: $n scripts, $bad not executable"
[ "$bad" = 0 ]
