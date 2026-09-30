#!/bin/sh
# Build one board's image with Buildroot, as the images workflow does.
#   scripts/ci/build-board.sh <board> [step ...]
# <board>: a defconfig name without "_defconfig" (retrostone2_release, rpi4_64).
# Steps (default: all = configure source build package legal-info):
#   configure   get Buildroot (sha256 checked), make <board>_defconfig, set the
#               homebrew and RetroStone VC games options (and the UART
#               password, if the defconfig asks for one), make olddefconfig
#   source      download every source (make source), 3 tries
#   build       make (full log in <out>/build.log, ">>>" lines on stdout)
#   package     <artifacts>/retrostoneos-<version>-<image>.img.xz (xz -T0 -9),
#               its .sha256 and a .info file for the release notes; for a
#               board with A/B slots also the system update package .rsu
#               (make-rsu.sh: signed with UPDATE_SIGNING_KEY, else
#               *-unsigned.rsu)
#   legal-info  make legal-info -> <artifacts>/legal-info-<image>-<version>.tar.xz
#               (split in 1900 MiB parts if it would exceed GitHub's 2 GiB)
#
# Environment (all optional):
#   RSOS_CI_WORK        work directory (default ~/rsos-ci); never the repo
#   RSOS_CI_BR_DIR      Buildroot tree (default $RSOS_CI_WORK/buildroot-<version>)
#   RSOS_CI_OUT         output directory, O= (default $RSOS_CI_WORK/output-<board>)
#   RSOS_CI_ARTIFACTS   where the files to publish go (default $RSOS_CI_WORK/artifacts)
#   BR2_DL_DIR          download cache (default $RSOS_CI_WORK/dl; overrides the defconfig)
#   BR2_CCACHE_DIR      ccache directory (default $RSOS_CI_WORK/ccache)
#   RSOS_CI_HOMEBREW    auto (default): the approved homebrew games are built in
#                       when homebrew/license ok/ holds them; yes: they must be
#                       there; no: BR2_PACKAGE_RSOS_HOMEBREW=n
#   RSOS_CI_VC_GAMES    auto (default): the RetroStone VC games (rsos-vc-games) are
#                       built in when RSOS_CI_VC_GAMES_DIR holds a RetroStone VC
#                       checkout; yes: it must be there; no: BR2_PACKAGE_RSOS_VC_GAMES=n
#   RSOS_CI_VC_GAMES_DIR  the RetroStone VC checkout (default ../RetroStoneVC next
#                       to the repository, where the defconfigs expect it)
#   RSOS_CI_VERSION     version in the file names (default: git describe)
#   RSOS_CI_OS_VERSION  a release tag's version (0.2.0): the version the
#                       image reports (BR2_RETROSTONE_VERSION); it also makes a
#                       board without a release defconfig a release build
#                       (BR2_RETROSTONE_RELEASE=y, no "-dev")
#   UPDATE_SIGNING_KEY  the update signing key (signify secret key text)
#   RSOS_RSU_REQUIRE_SIGNED  1: no unsigned .rsu, the package step fails
#                       without the key (images.yml sets it on tags)
#   RSOS_UART_PASSWORD  root password, only for a defconfig that selects the
#                       password login on the UART (BR2_RETROSTONE_UART_SHELL_PASSWORD)
#   RSOS_CI_PER_PACKAGE 1: BR2_PER_PACKAGE_DIRECTORIES=y and a top-level
#                       parallel make (faster on many cores; see docs/ci.md)
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
EXT=$REPO/buildroot-external
# shellcheck disable=SC1091
. "$HERE/buildroot.env"

BOARD=${1:?usage: build-board.sh <board> [configure|source|build|package|legal-info|all ...]}
BOARD=${BOARD%_defconfig}
shift
[ $# -gt 0 ] || set -- all
# (board-info.sh names the known boards when this one does not exist)
info=$(sh "$HERE/board-info.sh" "$BOARD") || exit 1
eval "$info"

WORK=${RSOS_CI_WORK:-$HOME/rsos-ci}
BR_DIR=${RSOS_CI_BR_DIR:-$WORK/buildroot-$BR_VERSION}
OUT=${RSOS_CI_OUT:-$WORK/output-$BOARD}
ART=${RSOS_CI_ARTIFACTS:-$WORK/artifacts}
export BR2_DL_DIR="${BR2_DL_DIR:-$WORK/dl}"
export BR2_CCACHE_DIR="${BR2_CCACHE_DIR:-$WORK/ccache}"
HOMEBREW_DIR="$REPO/homebrew/license ok"
VC_DIR=${RSOS_CI_VC_GAMES_DIR:-$(dirname "$REPO")/RetroStoneVC}
VERSION=${RSOS_CI_VERSION:-$(git -C "$REPO" describe --tags --always --dirty 2>/dev/null || date -u +%Y%m%d)}
VERSION=$(printf '%s' "$VERSION" | tr -c 'A-Za-z0-9._+-' '-')
FILE=retrostoneos-$VERSION-$IMAGE

case $OUT in "$REPO"/*) echo "build-board: the output must not be inside the repository" >&2; exit 1 ;; esac
mkdir -p "$WORK" "$BR2_DL_DIR" "$BR2_CCACHE_DIR" "$ART"

if [ "${GITHUB_ACTIONS:-}" = true ]; then
	group() { echo "::group::$*"; }
	endgroup() { echo "::endgroup::"; }
	error() { echo "::error::$*"; }
else
	group() { echo "=== $*"; }
	endgroup() { :; }
	error() { echo "ERROR: $*" >&2; }
fi
brmake() { make -C "$BR_DIR" O="$OUT" "$@"; }
now() { date +%s; }

# set_kconfig <symbol> y|n|"string": rewrite one line of .config
set_kconfig() {
	sed -i "/^$1=/d; /^# $1 is not set\$/d" "$OUT/.config"
	if [ "$2" = n ]; then echo "# $1 is not set"; else echo "$1=$2"; fi >> "$OUT/.config"
}
kconfig_is() { # <symbol> y|n
	if [ "$2" = n ]; then
		if grep -q "^$1=" "$OUT/.config"; then return 1; fi
		return 0
	fi
	grep -q "^$1=$2\$" "$OUT/.config"
}
homebrew_present() {
	[ -d "$HOMEBREW_DIR" ] && [ -n "$(find "$HOMEBREW_DIR" -mindepth 1 -maxdepth 1 ! -name '.git*' | head -n 1)" ]
}
vc_present() {
	[ -f "$VC_DIR/Makefile" ] && [ -d "$VC_DIR/sdk" ] && [ -d "$VC_DIR/games" ]
}

step_configure() {
	group "configure $BOARD (Buildroot $BR_VERSION)"
	sh "$HERE/get-buildroot.sh" "$BR_DIR"
	mkdir -p "$OUT"
	brmake BR2_EXTERNAL="$EXT" "${BOARD}_defconfig"

	case ${RSOS_CI_HOMEBREW:-auto} in
	yes)
		homebrew_present || { error "RSOS_CI_HOMEBREW=yes but $HOMEBREW_DIR is empty or missing"; exit 1; }
		hb=y ;;
	no) hb=n ;;
	auto) if homebrew_present; then hb=y; else hb=n; fi ;;
	*) error "RSOS_CI_HOMEBREW must be auto, yes or no"; exit 1 ;;
	esac
	set_kconfig BR2_PACKAGE_RSOS_HOMEBREW "$hb"

	# the RetroStone VC games (proprietary: a private repository, docs/ci.md)
	case ${RSOS_CI_VC_GAMES:-auto} in
	yes)
		vc_present || { error "RSOS_CI_VC_GAMES=yes but $VC_DIR is not a RetroStone VC checkout"; exit 1; }
		vc=y ;;
	no) vc=n ;;
	auto) if vc_present; then vc=y; else vc=n; fi ;;
	*) error "RSOS_CI_VC_GAMES must be auto, yes or no"; exit 1 ;;
	esac
	set_kconfig BR2_PACKAGE_RSOS_VC_GAMES "$vc"
	if [ "$vc" = y ]; then
		case $VC_DIR in
		*" "* | *\"* | *\\*) error "RSOS_CI_VC_GAMES_DIR must not contain spaces, quotes or backslashes"; exit 1 ;;
		esac
		set_kconfig BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR "\"$VC_DIR\""
	fi

	if grep -q '^BR2_RETROSTONE_UART_SHELL_PASSWORD=y$' "$OUT/.config"; then
		if [ -n "${RSOS_UART_PASSWORD:-}" ]; then
			case $RSOS_UART_PASSWORD in
			*'"'* | *\\* | *'$'* | *'
'*) error "RSOS_UART_PASSWORD must not contain \", \\, \$ or a newline"; exit 1 ;;
			esac
			set_kconfig BR2_RETROSTONE_UART_PASSWORD "\"$RSOS_UART_PASSWORD\""
		else
			error "$BOARD selects a UART password login: set the UART_PASSWORD secret (docs/ci.md)"
			exit 1
		fi
	fi
	if [ "${RSOS_CI_PER_PACKAGE:-0}" = 1 ]; then
		set_kconfig BR2_PER_PACKAGE_DIRECTORIES y
	fi
	# a release tag: the image reports the tag's version (/etc/rsos-version,
	# /etc/rsos/version.env: what the system updater compares). A board
	# without a separate release defconfig (VARIANT=default: the Raspberry Pi
	# and Orange Pi boards) is a release build on a tag, like
	# <x>_release_defconfig, and a development build ("-dev") otherwise; a
	# development variant (<x>_defconfig next to <x>_release_defconfig) stays
	# one.
	if [ -n "${RSOS_CI_OS_VERSION:-}" ]; then
		case $RSOS_CI_OS_VERSION in
		*[!A-Za-z0-9._+-]* | [!0-9]*) error "RSOS_CI_OS_VERSION '$RSOS_CI_OS_VERSION' is not a version"; exit 1 ;;
		esac
		set_kconfig BR2_RETROSTONE_VERSION "\"$RSOS_CI_OS_VERSION\""
		[ "$VARIANT" = default ] && set_kconfig BR2_RETROSTONE_RELEASE y
	fi
	brmake olddefconfig > /dev/null
	kconfig_is BR2_PACKAGE_RSOS_HOMEBREW "$hb" || {
		error "BR2_PACKAGE_RSOS_HOMEBREW=$hb did not survive olddefconfig"
		exit 1
	}
	kconfig_is BR2_PACKAGE_RSOS_VC_GAMES "$vc" || {
		error "BR2_PACKAGE_RSOS_VC_GAMES=$vc did not survive olddefconfig"
		exit 1
	}
	if [ -n "${RSOS_CI_OS_VERSION:-}" ] && [ "$VARIANT" != dev ] && ! kconfig_is BR2_RETROSTONE_RELEASE y; then
		error "$BOARD on a tag: BR2_RETROSTONE_RELEASE=y did not survive olddefconfig"
		exit 1
	fi
	if kconfig_is BR2_RETROSTONE_RELEASE y; then build=release; else build=development; fi
	{
		echo "HOMEBREW=$hb"
		echo "VC_GAMES=$vc"
		echo "CONFIGURED=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "$OUT/rsos-ci.env"
	echo "board $BOARD -> $FILE, $build build, version ${RSOS_CI_OS_VERSION:-(package default)}, homebrew games: $hb, output $OUT"
	if [ "$vc" = y ]; then
		echo "RetroStone VC games: yes, from $VC_DIR ($(git -C "$VC_DIR" describe --always --dirty --abbrev=12 2>/dev/null || echo "no git"))"
	else
		echo "RetroStone VC games: no (no RetroStone VC checkout at $VC_DIR: the VC_GAMES_TOKEN secret is not set, or RSOS_CI_VC_GAMES=no); the image has no RetroStone system"
	fi
	echo "downloads: $BR2_DL_DIR, ccache: $BR2_CCACHE_DIR"
	endgroup
}

step_source() {
	group "download the sources of $BOARD"
	t0=$(now)
	i=1
	until brmake source > "$OUT/source.log" 2>&1; do
		tail -n 30 "$OUT/source.log"
		[ "$i" -lt 3 ] || { error "make source failed 3 times (log: $OUT/source.log)"; exit 1; }
		i=$((i + 1))
		echo "retrying the downloads ($i/3) in 30 s"
		sleep 30
	done
	grep '^>>>' "$OUT/source.log" | tail -n 5 || true
	echo "sources ready in $(($(now) - t0)) s; $(du -sh "$BR2_DL_DIR" | cut -f1) in $BR2_DL_DIR"
	endgroup
}

step_build() {
	echo "building $BOARD (log: $OUT/build.log)"
	t0=$(now)
	jobs=
	[ "${RSOS_CI_PER_PACKAGE:-0}" = 1 ] && jobs=-j$(nproc)
	rm -f "$OUT/build.rc"
	{
		rc=0
		brmake ${jobs:+"$jobs"} || rc=$?
		echo "$rc" > "$OUT/build.rc"
	} 2>&1 | tee "$OUT/build.log" |
		grep --line-buffered '^>>>' | while IFS= read -r l; do
			printf '[%5d s] %s\n' "$(($(now) - t0))" "$l"
		done
	rc=$(cat "$OUT/build.rc" 2>/dev/null || echo 1)
	if [ "$rc" != 0 ]; then
		group "last 150 lines of the build log"
		tail -n 150 "$OUT/build.log"
		endgroup
		error "the $BOARD build failed after $(($(now) - t0)) s (full log in the build-log artifact)"
		exit 1
	fi
	echo "built $BOARD in $(($(now) - t0)) s"
	if [ -x "$OUT/host/bin/ccache" ]; then
		CCACHE_DIR=$BR2_CCACHE_DIR "$OUT/host/bin/ccache" -s || true
	fi
	df -h "$WORK" | tail -n 1
}

step_package() {
	group "package $FILE"
	IMG=$OUT/images/sdcard.img
	[ -f "$IMG" ] || { error "no $IMG"; exit 1; }
	rm -f "$ART/$FILE".*
	xz -T0 -9 -c "$IMG" > "$ART/$FILE.img.xz.part"
	mv "$ART/$FILE.img.xz.part" "$ART/$FILE.img.xz"
	(cd "$ART" && sha256sum "$FILE.img.xz" > "$FILE.img.xz.sha256")
	# the system update package (boards with A/B slots; docs/updates.md). On
	# a tag, make-rsu checks the version the image reports: the tag's, plus
	# "-dev" for a development variant.
	osv=${RSOS_CI_OS_VERSION:-}
	if [ -n "$osv" ] && ! kconfig_is BR2_RETROSTONE_RELEASE y; then osv=$osv-dev; fi
	RSOS_CI_WORK=$WORK RSOS_CI_OS_VERSION=$osv sh "$HERE/make-rsu.sh" "$OUT" "$ART" "$FILE"
	rsu=
	signed=no
	if [ -f "$ART/$FILE.rsu" ]; then
		rsu=$FILE.rsu
		signed=yes
	elif [ -f "$ART/$FILE-unsigned.rsu" ]; then
		rsu=$FILE-unsigned.rsu
	fi
	hb=$(sed -n 's/^HOMEBREW=//p' "$OUT/rsos-ci.env" 2>/dev/null || echo n)
	vc=$(sed -n 's/^VC_GAMES=//p' "$OUT/rsos-ci.env" 2>/dev/null || echo n)
	{
		printf "BOARD='%s'\nIMAGE='%s'\nSTATUS='%s'\nFILE='%s'\nHOMEBREW='%s'\nVERSION='%s'\n" \
			"$BOARD" "$IMAGE" "$STATUS" "$FILE.img.xz" "$hb" "$VERSION"
		printf "NAME='%s'\n" "$(printf '%s' "$NAME" | sed "s/'/'\\\\''/g")"
		printf "SIZE=%s\nRAW_SIZE=%s\n" "$(stat -c %s "$ART/$FILE.img.xz")" "$(stat -c %s "$IMG")"
		printf "COMMIT='%s'\n" "$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
		printf "VC_GAMES='%s'\n" "${vc:-n}"
		printf "RSU='%s'\nRSU_SIGNED='%s'\n" "$rsu" "$signed"
		[ -n "$rsu" ] && printf "RSU_SIZE=%s\n" "$(stat -c %s "$ART/$rsu")"
		true
	} > "$ART/$FILE.info"
	ls -l "$ART"
	cat "$ART/$FILE.img.xz.sha256"
	endgroup
}

step_legal_info() {
	group "legal-info for $BOARD"
	t0=$(now)
	brmake legal-info > "$OUT/legal-info.log" 2>&1 || {
		tail -n 50 "$OUT/legal-info.log"
		error "make legal-info failed"
		exit 1
	}
	grep -i 'warning' "$OUT/legal-info/legal-info.warnings" 2>/dev/null | head -n 40 || true
	L=legal-info-$IMAGE-$VERSION.tar.xz
	rm -f "$ART/$L" "$ART/$L".part*
	tar -C "$OUT" -cf - legal-info | xz -T0 -3 > "$ART/$L"
	max=$((1900 * 1024 * 1024))
	if [ "$(stat -c %s "$ART/$L")" -gt "$max" ]; then
		echo "$L is over 1900 MiB: splitting it (cat $L.part* > $L)"
		split -b "$max" -d -a 2 "$ART/$L" "$ART/$L.part"
		rm -f "$ART/$L"
	fi
	(
		cd "$ART"
		for f in "$L" "$L".part*; do
			if [ -f "$f" ]; then sha256sum "$f"; fi
		done > "$L.sha256"
	)
	ls -l "$ART"/legal-info-*
	echo "legal-info done in $(($(now) - t0)) s"
	endgroup
}

for s in "$@"; do
	case $s in
	configure) step_configure ;;
	source) step_source ;;
	build) step_build ;;
	package) step_package ;;
	legal-info) step_legal_info ;;
	all) step_configure; step_source; step_build; step_package; step_legal_info ;;
	*) echo "build-board: unknown step $s" >&2; exit 1 ;;
	esac
done
