#!/bin/sh
# configs/<board>_release_defconfig must be configs/<board>_defconfig
# followed by its release block (docs/build.md, "Release build"), for every
# board that has one (retrostone2, retrostone1).
#   sh buildroot-external/board/common/tests/release-defconfig-test.sh
C=$(cd "$(dirname "$0")/../../../configs" && pwd)
found=0
for rel in "$C"/*_release_defconfig; do
	[ -f "$rel" ] || continue
	found=1
	b=${rel##*/}
	b=${b%_release_defconfig}
	n=$(wc -l < "$C/${b}_defconfig")
	if head -n "$n" "$rel" | cmp -s - "$C/${b}_defconfig"; then
		echo "ok: the release defconfig starts with ${b}_defconfig"
	else
		echo "FAIL: ${b}_release_defconfig no longer starts with ${b}_defconfig:"
		head -n "$n" "$rel" | diff "$C/${b}_defconfig" -
		exit 1
	fi
	tail -n +"$((n + 1))" "$rel" | grep -q '^BR2_RETROSTONE_RELEASE=y$' ||
		{ echo "FAIL: no BR2_RETROSTONE_RELEASE=y in the ${b} release block"; exit 1; }
	echo "ok: ${b} release block"
done
[ "$found" = 1 ] || { echo "FAIL: no *_release_defconfig in $C"; exit 1; }
