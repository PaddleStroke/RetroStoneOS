#!/bin/sh
# Cross-compile the whole frontend (make all: rsos-frontend and its tools,
# linked against the target libdrm and alsa-lib, and rsos-update, against
# libzstd and mbedTLS) for 32-bit ARM (the
# RetroStone2's Cortex-A7) or aarch64 (Cortex-A72, the Raspberry Pi 4), with
# the flags Buildroot uses. Nothing is run: this catches what only breaks on
# the target (32-bit time_t/off_t, alignment, missing prototypes).
#   scripts/ci/frontend-cross.sh arm-linux-gnueabihf | aarch64-linux-gnu
# CI: Ubuntu's cross compilers + the multiarch -dev packages
# (install-deps.sh cross). Elsewhere, CROSS_CC / CROSS_AR / CROSS_PKG_CONFIG
# can name another toolchain, e.g. Buildroot's host/bin/arm-linux-gcc and
# host/bin/pkg-config.
set -eu
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TRIPLE=${1:?usage: frontend-cross.sh arm-linux-gnueabihf|aarch64-linux-gnu}
case $TRIPLE in
arm-linux-gnueabihf)
	ARCH_FLAGS="-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
	# Buildroot's 32-bit glibc targets: 64-bit off_t and time_t
	LFS="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64"
	EXPECT="ARM, EABI5"
	;;
aarch64-linux-gnu)
	ARCH_FLAGS="-mcpu=cortex-a72"
	LFS="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
	EXPECT="ARM aarch64"
	;;
*) echo "frontend-cross: unknown target $TRIPLE" >&2; exit 1 ;;
esac
CC=${CROSS_CC:-$TRIPLE-gcc}
AR=${CROSS_AR:-$TRIPLE-ar}
if [ -n "${CROSS_PKG_CONFIG:-}" ]; then
	PKG_CONFIG=$CROSS_PKG_CONFIG
else
	PKG_CONFIG=pkg-config
	export PKG_CONFIG_LIBDIR="/usr/lib/$TRIPLE/pkgconfig:/usr/share/pkgconfig"
fi
# pkg-config must find the target libraries, not fall back to the host's
"$PKG_CONFIG" --exists libdrm alsa libzstd || {
	echo "frontend-cross: libdrm/alsa/libzstd for $TRIPLE not found by $PKG_CONFIG (install-deps.sh cross)" >&2
	exit 1
}
BUILDDIR=${RSOS_CI_WORK:-$HOME/rsos-ci}/frontend-$TRIPLE
rm -rf "$BUILDDIR"
LOG=$BUILDDIR.log
echo "cross build for $TRIPLE with $("$CC" --version | head -n 1), BUILDDIR=$BUILDDIR"
rc=0
make -C "$REPO/frontend" -j"$(nproc)" BUILDDIR="$BUILDDIR" \
	CC="$CC" AR="$AR" PKG_CONFIG="$PKG_CONFIG" CC_FOR_BUILD=cc \
	CPPFLAGS="$LFS" CFLAGS="-O2 -g0 $ARCH_FLAGS" all > "$LOG" 2>&1 || rc=$?
warnings=$(grep -c 'warning:' "$LOG" || true)
if [ "$rc" != 0 ]; then
	tail -n 60 "$LOG"
	echo "frontend-cross: $TRIPLE build failed"
	exit 1
fi
if [ "$warnings" != 0 ]; then
	echo "frontend-cross: $warnings compiler warnings for $TRIPLE:"
	grep 'warning:' "$LOG" | sort -u | head -n 50
	[ "${GITHUB_ACTIONS:-}" = true ] && echo "::warning::frontend: $warnings compiler warnings in the $TRIPLE build"
fi
for b in rsos-frontend rsos-run rsos-kmstest rsos-update; do
	file -b "$BUILDDIR/$b" | grep -q "$EXPECT" || {
		echo "frontend-cross: $BUILDDIR/$b is not a $EXPECT binary: $(file -b "$BUILDDIR/$b")"
		exit 1
	}
done
if [ "$TRIPLE" = arm-linux-gnueabihf ]; then
	# the modules' own ARM checks (link their test programs for the A20)
	make -C "$REPO/frontend" -j"$(nproc)" BUILDDIR="$BUILDDIR" ARM_CC="$CC" CC_FOR_BUILD=cc \
		PKG_CONFIG="$PKG_CONFIG" ui-arm-check transfer-arm-check power-arm-check > "$LOG.modules" 2>&1 || {
		tail -n 40 "$LOG.modules"
		echo "frontend-cross: the module ARM checks failed"
		exit 1
	}
fi
ls -l "$BUILDDIR/rsos-frontend"
echo "frontend-cross: $TRIPLE OK ($warnings warnings)"
