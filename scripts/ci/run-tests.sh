#!/bin/sh
# Run the shell test suites of buildroot-external/board/*/tests/ the way they
# expect to be run in WSL (docs/build.md), from a fake $HOME whose rsos/
# points at the CI directories:
#   $HOME/rsos/output -> a Buildroot output directory (or just host/bin with
#                        the U-Boot tools, for the tests that need nothing else)
#   $HOME/rsos/dl     -> the download cache
#
#   scripts/ci/run-tests.sh host
#       board/common/tests/*.sh, with U-Boot host tools built from source
#       (uboot-host-tools.sh): no image needed (the ci workflow)
#   scripts/ci/run-tests.sh image <board> <output directory>
#       board/common/tests/*.sh and board/<board dir>/tests/*.sh against a
#       finished build (the images workflow). A test whose header says it runs
#       "as root" is run with sudo (loop devices, mount namespaces).
# Every test runs even if an earlier one fails; the exit status is 1 if any
# failed. RSOS_CI_TESTS_SKIP: space-separated test file names to skip.
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
EXT=$REPO/buildroot-external
WORK=${RSOS_CI_WORK:-$HOME/rsos-ci}
FAKE=$WORK/test-home
MODE=${1:?usage: run-tests.sh host | image <board> <output>}
SUDO=
[ "$(id -u)" = 0 ] || SUDO="sudo -n"

if [ "${GITHUB_ACTIONS:-}" = true ]; then
	group() { echo "::group::$*"; }
	endgroup() { echo "::endgroup::"; }
else
	group() { echo "=== $*"; }
	endgroup() { :; }
fi

rm -rf "$FAKE"
mkdir -p "$FAKE/rsos"
ln -s "${BR2_DL_DIR:-$WORK/dl}" "$FAKE/rsos/dl"
case $MODE in
host)
	TESTS=$(ls "$EXT"/board/common/tests/*.sh)
	mkdir -p "$FAKE/rsos/output/host"
	sh "$HERE/uboot-host-tools.sh" "$FAKE/rsos/output/host/bin" || exit 1
	;;
image)
	BOARD=${2:?board}
	OUT=$(cd "${3:?output directory}" && pwd)
	eval "$(sh "$HERE/board-info.sh" "$BOARD")"
	ln -s "$OUT" "$FAKE/rsos/output"
	TESTS=$(ls "$EXT"/board/common/tests/*.sh "$EXT/board/$BOARD_DIR"/tests/*.sh 2>/dev/null)
	# the data partition test mounts exFAT on loop devices
	$SUDO modprobe exfat 2>/dev/null || true
	;;
*) echo "run-tests: unknown mode $MODE" >&2; exit 1 ;;
esac

pass=
fail=
skip=
for t in $TESTS; do
	n=${t##*/}
	case " ${RSOS_CI_TESTS_SKIP:-} " in *" $n "*) skip="$skip $n"; continue ;; esac
	# the tests that need the host U-Boot tools of a RetroStone2 build
	if grep -q 'rsos/output/host' "$t" && [ ! -x "$FAKE/rsos/output/host/bin/fw_printenv" ]; then
		skip="$skip $n(no-uboot-tools)"
		continue
	fi
	interp="sh"
	head -n 1 "$t" | grep -q bash && interp="bash"
	run=
	head -n 12 "$t" | grep -qi 'as root' && run=$SUDO
	group "$n${run:+ (root)}"
	t0=$(date +%s)
	# shellcheck disable=SC2086
	if $run env HOME="$FAKE" PATH="$PATH" "$interp" "$t"; then
		pass="$pass $n"
		r=ok
	else
		fail="$fail $n"
		r=FAILED
	fi
	endgroup
	echo "$n: $r ($(($(date +%s) - t0)) s)"
	[ "$r" = ok ] || { [ "${GITHUB_ACTIONS:-}" = true ] && echo "::error::test $n failed"; }
done
echo "passed:${pass:- none}"
echo "failed:${fail:- none}"
echo "skipped:${skip:- none}"
[ -z "$fail" ]
