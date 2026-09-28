#!/bin/sh
# configs/retrostone2_release_defconfig must be configs/retrostone2_defconfig
# followed by its release block (docs/build.md, "Release build").
#   sh buildroot-external/board/common/tests/release-defconfig-test.sh
C=$(cd "$(dirname "$0")/../../../configs" && pwd)
n=$(wc -l < "$C/retrostone2_defconfig")
if head -n "$n" "$C/retrostone2_release_defconfig" | cmp -s - "$C/retrostone2_defconfig"; then
	echo "ok: the release defconfig starts with retrostone2_defconfig"
else
	echo "FAIL: retrostone2_release_defconfig no longer starts with retrostone2_defconfig:"
	head -n "$n" "$C/retrostone2_release_defconfig" | diff "$C/retrostone2_defconfig" -
	exit 1
fi
tail -n +"$((n + 1))" "$C/retrostone2_release_defconfig" | grep -q '^BR2_RETROSTONE_RELEASE=y$' ||
	{ echo "FAIL: no BR2_RETROSTONE_RELEASE=y in the release block"; exit 1; }
echo "ok: release block"
