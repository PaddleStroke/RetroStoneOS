#!/bin/sh
# fake-usb-stick.sh ROOT DISK LABEL
#
# A USB disk for the headless frontend (make check-frontend), under the test
# tree ROOT: its sysfs device folder (behind a USB host, so the detection
# takes it for a USB disk), one FAT32 partition DISK1 (a boot sector the
# probe recognises, with LABEL) and the stick's contents folder
# ROOT/sticks/DISK1 (the headless mount points /media/usbN at it). The disk
# is not plugged yet: the script token plug:DISK adds ROOT/sys/block/DISK
# (unplug:DISK removes it).
set -e
root=$1
disk=$2
label=$3
dev="$root/sys/devices/platform/soc/1c14000.usb/usb1/1-1/host0/$disk"
mkdir -p "$dev/device" "$dev/${disk}1" "$root/dev" "$root/sticks/${disk}1" "$root/sys/block"
echo 62914560 > "$dev/size"                      # 30 GiB in 512-byte sectors
echo 1 > "$dev/${disk}1/partition"
echo 62912512 > "$dev/${disk}1/size"
printf 'FakeUSB \n' > "$dev/device/vendor"
printf 'Stick 2.0       \n' > "$dev/device/model"

# FAT32 boot sector: jump, OEM name, 512 bytes/sector, 8 sectors/cluster,
# 32 reserved, 2 FATs; the label at 71, "FAT32   " at 82, 55 AA at 510
f="$root/dev/${disk}1"
dd if=/dev/zero of="$f" bs=512 count=8 2>/dev/null
at() {
	dd of="$f" bs=1 seek="$1" conv=notrunc 2>/dev/null
}
printf '\353\130\220MSDOS5.0\000\002\010\040\000\002' | at 0
printf '%-11.11s' "$label" | at 71
printf 'FAT32   ' | at 82
printf '\125\252' | at 510
