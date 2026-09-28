#!/bin/sh
# The system update package (.rsu, docs/updates.md) of one board, from its
# Buildroot output: the root filesystem image compressed with zstd -19, a
# manifest (version, board, sizes, SHA-256, changelog excerpt) and, when the
# signing key is given, its signature.
#   scripts/ci/make-rsu.sh <output dir> <artifacts dir> <file base name>
# writes <artifacts>/<file base name>.rsu and .rsu.sha256 (the base name is
# build-board.sh's, retrostoneos-<version>-<image>), <base>-unsigned.rsu
# without the key. Boards without A/B updates (no /etc/fw_env.config in the
# image) get no package.
#
# Environment (all optional):
#   UPDATE_SIGNING_KEY   the signify secret key (the text of the file, the
#                        GitHub secret of the same name). Empty: an UNSIGNED
#                        package, which release builds refuse (docs/updates.md)
#   RSOS_RSU_REQUIRE_SIGNED  1: fail instead of making an unsigned package
#   RSOS_CI_OS_VERSION   the tag's version: the image must report exactly it
#   RSOS_RSU_BOOTLOADER_MIN  the bootloader level the new system needs (1)
#   RSOS_RSU_CHANGELOG   a file with the changelog excerpt (default: the
#                        commit subjects since the previous v* tag, if git has
#                        the history)
#   RSOS_CI_WORK         work directory (default ~/rsos-ci)
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: make-rsu.sh <output dir> <artifacts dir> <file base name>}
ART=${2:?artifacts dir}
FILE=${3:?file base name}
WORK=${RSOS_CI_WORK:-$HOME/rsos-ci}
T=$WORK/rsu-tmp
TARGET=$OUT/target

if [ "${GITHUB_ACTIONS:-}" = true ]; then
	error() { echo "::error::$*"; }
	warn() { echo "::warning::$*"; }
else
	error() { echo "ERROR: $*" >&2; }
	warn() { echo "WARNING: $*" >&2; }
fi

if [ ! -f "$TARGET/etc/fw_env.config" ]; then
	echo "make-rsu: $FILE: no A/B boot environment in this image (updates by reflashing): no .rsu"
	exit 0
fi
ENV=$TARGET/etc/rsos/version.env
IMG=$(readlink -f "$OUT/images/rootfs.ext4")
PUB=$TARGET/usr/share/rsos/update.pub
for f in "$ENV" "$IMG" "$PUB"; do
	[ -f "$f" ] || { error "make-rsu: $f missing"; exit 1; }
done
# shellcheck disable=SC1090
. "$ENV"
: "${RSOS_VERSION:?} ${RSOS_VARIANT:?} ${RSOS_BUILD_TIME:?} ${RSOS_BOARD_ID:?}"
if [ -n "${RSOS_CI_OS_VERSION:-}" ] && [ "$RSOS_VERSION" != "$RSOS_CI_OS_VERSION" ]; then
	error "make-rsu: the image reports version $RSOS_VERSION, the tag says $RSOS_CI_OS_VERSION"
	exit 1
fi
command -v zstd > /dev/null || { error "make-rsu: zstd is needed (install-deps.sh buildroot)"; exit 1; }
rm -rf "$T" && mkdir -p "$T"

# the tool, from this tree (build machine compiler, no dependency)
make -s -C "$REPO/frontend" BUILDDIR="$T/build" CC_FOR_BUILD="${CC_FOR_BUILD:-cc}" \
	"$T/build/build-tools/rsos-mkupdate" > "$T/build.log" 2>&1 || {
	tail -n 20 "$T/build.log"
	error "make-rsu: cannot build rsos-mkupdate"
	exit 1
}
MK=$T/build/build-tools/rsos-mkupdate

# the changelog excerpt shown by the console (the release notes on GitHub
# have the full list)
if [ -n "${RSOS_RSU_CHANGELOG:-}" ]; then
	cp "$RSOS_RSU_CHANGELOG" "$T/changes.txt"
else
	prev=$(git -C "$REPO" describe --tags --abbrev=0 --match 'v*' HEAD^ 2>/dev/null || true)
	{
		if [ -n "$prev" ] && git -C "$REPO" log --no-merges --pretty='- %s' -n 40 "$prev..HEAD" 2>/dev/null |
			grep -q .; then
			echo "Changes since $prev:"
			git -C "$REPO" log --no-merges --pretty='- %s' -n 40 "$prev..HEAD"
		else
			echo "RetroStoneOS $RSOS_VERSION. The release notes on GitHub list the changes."
		fi
	} > "$T/changes.txt"
fi

echo "make-rsu: compressing $IMG ($(($(stat -c %s "$IMG") >> 20)) MiB, zstd -19)"
# -19: 8 MiB window; the device decodes with at most 32 MiB (update.c)
zstd -q -19 -T0 -c "$IMG" > "$T/payload.zst"
zstd -q -dc "$T/payload.zst" | cmp -s - "$IMG" || { error "make-rsu: the compressed payload does not decompress to the image"; exit 1; }

sign=
if [ -n "${UPDATE_SIGNING_KEY:-}" ]; then
	# the key must be the one whose public half is in the image
	"$MK" pubkey --key-env UPDATE_SIGNING_KEY > "$T/key.pub" || { error "make-rsu: UPDATE_SIGNING_KEY is not an unencrypted signify secret key"; exit 1; }
	if [ "$(tail -n 1 "$T/key.pub")" != "$(tail -n 1 "$PUB")" ]; then
		error "make-rsu: UPDATE_SIGNING_KEY does not match the public key in the image (frontend/assets/update.pub)"
		exit 1
	fi
	sign="--key-env UPDATE_SIGNING_KEY"
elif [ "${RSOS_RSU_REQUIRE_SIGNED:-0}" = 1 ]; then
	error "make-rsu: UPDATE_SIGNING_KEY is not set: no unsigned package for a release"
	exit 1
else
	warn "make-rsu: UPDATE_SIGNING_KEY is not set: $FILE-unsigned.rsu is NOT SIGNED (release images refuse it; docs/updates.md)"
fi
# An unsigned package is named *-unsigned.rsu: the consoles' online check
# only looks for retrostoneos-*-<board>.rsu, so it never offers one.
NAME=$FILE.rsu
[ -n "$sign" ] || NAME=$FILE-unsigned.rsu
rm -f "$ART/$FILE.rsu" "$ART/$FILE-unsigned.rsu" "$ART/$FILE.rsu.sha256" "$ART/$FILE-unsigned.rsu.sha256"
# shellcheck disable=SC2086
"$MK" pack -o "$ART/$NAME" --image "$IMG" --payload "$T/payload.zst" --board "$RSOS_BOARD_ID" \
	--version "$RSOS_VERSION" --variant "$RSOS_VARIANT" --build-time "$RSOS_BUILD_TIME" \
	${RSOS_BUILD_DATE:+--build-date "$RSOS_BUILD_DATE"} --bootloader-min "${RSOS_RSU_BOOTLOADER_MIN:-1}" \
	--changelog-file "$T/changes.txt" $sign
if [ -n "$sign" ]; then
	"$MK" verify -p "$PUB" "$ART/$NAME"
fi
(cd "$ART" && sha256sum "$NAME" > "$NAME.sha256")
"$MK" info "$ART/$NAME" | sed -n '/^board\|^version\|^payload_size\|^image_size\|^# /p'
rm -rf "$T"
