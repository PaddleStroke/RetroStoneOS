#!/bin/sh
# Tests of board/common/rootfs-overlay/usr/libexec/rsos/board.sh (the system
# disk from board.env or root= on the kernel command line) with BusyBox sh
# when available. Run from anywhere: sh board-sh-test.sh
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD_SH=$HERE/../rootfs-overlay/usr/libexec/rsos/board.sh
SH=sh
command -v busybox > /dev/null 2>&1 && SH="busybox sh"
pass=0
fail=0

# t <expected disk> <expected sep> <data_disk or ""> <cmdline>
t() {
	got=$($SH -c '
		RSOS_BOARD_DATA_DISK=$1; CMDLINE=$2
		. "$0"
		echo "$RSOS_DISK $RSOS_DISK_NAME [$RSOS_PART_SEP]"' "$BOARD_SH" "$3" "$4")
	want="$1 ${1##*/} [$2]"
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1))
	else
		echo "FAIL: data_disk='$3' cmdline='$4': got '$got', want '$want'"
		fail=$((fail + 1))
	fi
}

t /dev/mmcblk0 p "" "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait ro"
t /dev/mmcblk0 p "" "root=/dev/mmcblk0p3 ro quiet"
t /dev/mmcblk1 p "" "root=/dev/mmcblk1p2"
t /dev/sda "" "" "root=/dev/sda2 rootwait"
t /dev/nvme0n1 p "" "root=/dev/nvme0n1p2"
t /dev/mmcblk0 p "" "root=PARTUUID=1234-02 rootwait"
t /dev/mmcblk0 p "" ""
t /dev/mmcblk0 p "/dev/mmcblk0" "root=/dev/sda2"
t /dev/sdb "" "/dev/sdb" "root=/dev/mmcblk0p2"
t /dev/mmcblk0 p "auto" "root=/dev/mmcblk0p2"

# the hooks are no-ops by default
if $SH -c '. "$0"; rsos_board_late_audio && rsos_board_late_storage' "$BOARD_SH"; then
	pass=$((pass + 1))
else
	echo "FAIL: default hooks"
	fail=$((fail + 1))
fi
echo "board.sh: $pass passed, $fail failed ($SH)"
[ "$fail" = 0 ]
