# RetroStoneOS board helper, sourced (". /usr/libexec/rsos/board.sh") by the
# init scripts: rcS, rcK, data-partition, bootlog, rsos-net. Shell builtins
# only (no fork: rcS runs it on the way to the menu).
#
# /etc/rsos/board.env is generated at build time from the board's
# /etc/rsos/board.ini (board/common/post-build.sh): every "key = value"
# becomes RSOS_BOARD_<KEY>="value". The keys the scripts use (all optional;
# docs/porting.md):
#   RSOS_BOARD_DATA_DISK       the system disk (SD card) holding the root
#                              filesystems and the data partition; "auto" or
#                              unset: the disk of root= on the kernel command
#                              line, else /dev/mmcblk0
#   RSOS_BOARD_DATA_PARTITION  MBR entry number of the data partition (1)
#   RSOS_BOARD_BOOT_REASON     axp209: run rsos-bootreason (charge mode)
#   RSOS_BOARD_BATTERY_SUPPLY  for the boot logger's status (auto: the first
#                              Battery-type supply)
#   RSOS_BOARD_POWER_KEY_DEVICE  PEK driver for the boot logger (axp20x-pek)
#   RSOS_BOARD_WIFI_*, RSOS_BOARD_ETH_MODULES, RSOS_BOARD_BT_*  (rsos-net)
#
# Sets:
#   RSOS_DISK       /dev/<disk>    RSOS_DISK_NAME  <disk> (e.g. mmcblk0)
#   RSOS_PART_SEP   "p" when partitions are <disk>p<N> (mmcblk0p1, nvme0n1p1,
#                   loop0p1), "" otherwise (sda1)
# and the board hooks (no-ops unless /etc/rsos/board-hooks.sh, from the
# board's rootfs overlay, redefines them):
#   rsos_board_late_audio      rcS, background, menu up: mixer defaults
#   rsos_board_late_storage    rcS, background, after the USB storage modules

[ -r /etc/rsos/board.env ] && . /etc/rsos/board.env

rsos_board_late_audio() { :; }
rsos_board_late_storage() { :; }
[ -r /etc/rsos/board-hooks.sh ] && . /etc/rsos/board-hooks.sh

# The system disk.
case "$RSOS_BOARD_DATA_DISK" in
/dev/*)
	RSOS_DISK=$RSOS_BOARD_DATA_DISK
	;;
*)
	# root=/dev/<partition> on the kernel command line
	[ -n "$CMDLINE" ] || { [ -r /proc/cmdline ] && read -r CMDLINE < /proc/cmdline; }
	_rsos_r=
	case " $CMDLINE " in
	*" root=/dev/"*)
		_rsos_r=${CMDLINE##*root=/dev/}
		_rsos_r=${_rsos_r%% *}
		;;
	esac
	case "$_rsos_r" in
	"") RSOS_DISK=/dev/mmcblk0 ;;
	*[0-9]p[0-9]*) RSOS_DISK=/dev/${_rsos_r%p*} ;;
	*) RSOS_DISK=/dev/${_rsos_r%%[0-9]*} ;;
	esac
	unset _rsos_r
	;;
esac
RSOS_DISK_NAME=${RSOS_DISK##*/}
case "$RSOS_DISK_NAME" in
*[0-9]) RSOS_PART_SEP=p ;;
*) RSOS_PART_SEP= ;;
esac
