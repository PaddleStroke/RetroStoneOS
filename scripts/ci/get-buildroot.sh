#!/bin/sh
# Download the Buildroot release of buildroot.env, check its sha256 and
# extract it.
#   scripts/ci/get-buildroot.sh <directory>
# <directory> becomes the Buildroot tree (e.g. ~/rsos-ci/buildroot-2026.02.3);
# nothing is done if it already holds that release. RSOS_CI_BR_TARBALL may
# name an already downloaded tarball (it is checked all the same).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
. "$HERE/buildroot.env"
DEST=${1:?usage: get-buildroot.sh <directory>}

if [ -f "$DEST/Makefile" ] && grep -q "^export BR2_VERSION := $BR_VERSION\$" "$DEST/Makefile"; then
	echo "Buildroot $BR_VERSION already in $DEST"
	exit 0
fi

PARENT=$(dirname "$DEST")
mkdir -p "$PARENT"
TARBALL=${RSOS_CI_BR_TARBALL:-$PARENT/buildroot-$BR_VERSION.tar.xz}
if [ ! -f "$TARBALL" ]; then
	echo "Downloading $BR_URL"
	i=0
	until wget -q -O "$TARBALL.part" "$BR_URL"; do
		i=$((i + 1))
		[ "$i" -lt 3 ] || { echo "download failed" >&2; exit 1; }
		sleep 10
	done
	mv "$TARBALL.part" "$TARBALL"
fi
echo "$BR_SHA256  $TARBALL" | sha256sum -c - || {
	echo "Buildroot tarball $TARBALL does not match scripts/ci/buildroot.env" >&2
	exit 1
}
rm -rf "$DEST.tmp"
mkdir -p "$DEST.tmp"
tar -xJf "$TARBALL" -C "$DEST.tmp" --strip-components=1
rm -rf "$DEST"
mv "$DEST.tmp" "$DEST"
echo "Buildroot $BR_VERSION extracted to $DEST"
