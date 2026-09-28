#!/bin/sh
# Ubuntu 24.04 packages for the CI jobs (one list per job, in one place).
#   scripts/ci/install-deps.sh <profile> ...
# Profiles:
#   frontend   make check / check-asan of frontend/ (host build)
#   cross      the frontend cross-compiled for armhf and arm64: the Ubuntu
#              cross compilers and the armhf/arm64 libdrm and alsa-lib from
#              ports.ubuntu.com (multiarch)
#   buildroot  the Buildroot host dependencies (docs/build.md) + xz/zstd
#   tests      board/common tests and the image tests: BusyBox, exFAT,
#              mtools, QEMU and the ARM cross compiler (A/B U-Boot test)
#   lint       shellcheck, and python3-magic for Buildroot's check-package
set -eu
export DEBIAN_FRONTEND=noninteractive
SUDO=
[ "$(id -u)" = 0 ] || SUDO=sudo
apt_install() {
	i=0
	until $SUDO apt-get install -y -q --no-install-recommends "$@"; do
		i=$((i + 1))
		[ "$i" -lt 3 ] || exit 1
		sleep 15
		$SUDO apt-get update -q || true
	done
}

setup_multiarch() {
	# The runner's sources (azure.archive.ubuntu.com) only serve amd64/i386:
	# pin them to amd64 and take armhf/arm64 from ports.ubuntu.com.
	. /etc/os-release
	f=/etc/apt/sources.list.d/ubuntu.sources
	if [ -f "$f" ] && ! grep -q '^Architectures:' "$f"; then
		$SUDO sed -i '/^Types:/a Architectures: amd64 i386' "$f"
	fi
	[ -f /etc/apt/sources.list ] && $SUDO sed -i 's/^deb \(http\)/deb [arch=amd64,i386] \1/' /etc/apt/sources.list
	$SUDO tee /etc/apt/sources.list.d/ubuntu-ports.sources > /dev/null <<EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: $VERSION_CODENAME $VERSION_CODENAME-updates $VERSION_CODENAME-security
Components: main universe
Architectures: armhf arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
	$SUDO dpkg --add-architecture armhf
	$SUDO dpkg --add-architecture arm64
}

PKGS=
MULTIARCH=0
for p in "$@"; do
	case $p in
	# (the system updater, rsos-update: libzstd and mbedTLS; its tests use
	# the zstd program and, when present, OpenBSD signify)
	frontend) PKGS="$PKGS build-essential pkg-config libdrm-dev libasound2-dev libzstd-dev libmbedtls-dev zstd
			signify-openbsd" ;;
	cross)
		MULTIARCH=1
		# (the target C library headers are only Recommends of the cross
		# compilers, which --no-install-recommends leaves out)
		PKGS="$PKGS build-essential pkg-config gcc-arm-linux-gnueabihf gcc-aarch64-linux-gnu
			binutils-arm-linux-gnueabihf binutils-aarch64-linux-gnu
			libc6-dev-armhf-cross libc6-dev-arm64-cross
			libdrm-dev:armhf libasound2-dev:armhf libdrm-dev:arm64 libasound2-dev:arm64
			libzstd-dev:armhf libmbedtls-dev:armhf libzstd-dev:arm64 libmbedtls-dev:arm64" ;;
	buildroot)
		PKGS="$PKGS build-essential bc cpio file git libncurses-dev libssl-dev python3 rsync unzip
			wget patch perl bzip2 gzip xz-utils zstd debianutils" ;;
	tests) PKGS="$PKGS busybox-static exfatprogs mtools dosfstools util-linux fdisk
			qemu-system-arm gcc-arm-linux-gnueabihf bison flex libssl-dev uuid-dev pkg-config bc wget
			e2fsprogs zstd libzstd-dev libmbedtls-dev" ;;
	lint) PKGS="$PKGS shellcheck python3 python3-magic python3-flake8 xz-utils wget" ;;
	*) echo "install-deps: unknown profile $p" >&2; exit 1 ;;
	esac
done
[ "$MULTIARCH" = 1 ] && setup_multiarch
$SUDO apt-get update -q
# shellcheck disable=SC2086
apt_install $PKGS
