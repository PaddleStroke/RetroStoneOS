#!/bin/sh
# Buildroot's utils/check-package (coding style of .mk, Config.in, .hash and
# patch files) on the external tree's packages and defconfigs.
#   scripts/ci/check-package.sh [--strict]
# Gets the Buildroot tree with get-buildroot.sh (RSOS_CI_BR_DIR, default
# $RSOS_CI_WORK/buildroot-<version>). Without --strict it reports the
# findings and exits 0 (docs/ci.md).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# shellcheck disable=SC1091
. "$HERE/buildroot.env"
STRICT=0
[ "${1:-}" = --strict ] && STRICT=1
WORK=${RSOS_CI_WORK:-$HOME/rsos-ci}
BR_DIR=${RSOS_CI_BR_DIR:-$WORK/buildroot-$BR_VERSION}
sh "$HERE/get-buildroot.sh" "$BR_DIR" > /dev/null || exit 2
OUTF=$(mktemp)
trap 'rm -f "$OUTF"' EXIT

cd "$REPO" || exit 2
# -b: files of a BR2_EXTERNAL tree (no check of the Buildroot-only rules)
find buildroot-external/package buildroot-external/configs buildroot-external/Config.in \
	buildroot-external/external.mk buildroot-external/external.desc -type f |
	sort | tr '\n' '\0' |
	xargs -0 python3 "$BR_DIR/utils/check-package" -b > "$OUTF" 2>&1
total=$(grep -c '^buildroot-external/' "$OUTF" || true)
cat "$OUTF"
if grep -q '^Traceback' "$OUTF"; then
	echo "check-package did not run (python3-magic or flake8 missing? install-deps.sh lint)" >&2
	exit 2
fi
echo "check-package: $total findings"
if [ "${GITHUB_ACTIONS:-}" = true ]; then
	# file:line: message  ->  annotations
	sed -n 's/^\(buildroot-external\/[^:]*\):\([0-9]*\): \(.*\)$/\1 \2 \3/p' "$OUTF" |
		while read -r file line msg; do
			echo "::warning file=$file,line=$line::check-package: $msg"
		done
	echo "### check-package: $total findings" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
fi
[ "$STRICT" = 0 ] || [ "$total" = 0 ]
