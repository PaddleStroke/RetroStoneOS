#!/bin/sh
# A stand-in for rsos-update --machine (src/update/rsos-update.c) for the
# menu tests (make check-update-ui): canned lines, chosen by FAKE_UPDATE:
#   found      a signed 0.2.0 online, installable; apply succeeds
#   uptodate   the newest release is the installed version
#   noab       0.2.0 online, but this board has no A/B updates
#   usb        a package on the USB drive given with --dir
#   fail       as found, but apply fails (battery too low)
# FAKE_UPDATE_BOOT: the event of "boot" (default none).
# Every call is appended to $FAKE_UPDATE_LOG (one line of arguments).
[ -n "${FAKE_UPDATE_LOG:-}" ] && echo "$*" >> "$FAKE_UPDATE_LOG"
T=$(printf '\t')
info() {
	echo "info${T}version=0.1.0${T}board=retrostone2${T}variant=release${T}build_date=2026-09-27${T}ab=${1:-1}${T}reason=${T}slot=a${T}key=1${T}bootloader=1${T}tls=1"
}
found() { # <type> <status> <source> <installable> <verdict> <where>
	# (printf: dash's echo would turn the \n escapes into line breaks)
	printf '%s\n' "$1${T}status=$2${T}source=$3${T}version=0.2.0${T}size=88080384${T}where=$6${T}name=retrostoneos-0.2.0-retrostone2.rsu${T}page=https://github.com/PaddleStroke/RetroStoneOS/releases/tag/v0.2.0${T}signed=1${T}installable=$4${T}verdict=$5${T}notes=## What's new\\n- **Faster** menus\\n- A new \`updater\`\\n\\nLine 5\\nLine 6\\nLine 7\\nLine 8\\nLine 9\\nLine 10\\nLine 11\\nLine 12\\nLine 13\\nLine 14\\nLine 15\\nLine 16\\nLine 17\\nLine 18\\nLine 19\\nLine 20"
}
URL=https://github.com/PaddleStroke/RetroStoneOS/releases/download/v0.2.0/retrostoneos-0.2.0-retrostone2.rsu
cmd=
dir=
while [ $# -gt 0 ]; do
	case $1 in
	--machine | --allow-unsigned | --local-only | --net-only) ;;
	--dir) dir=$2; shift ;;
	--url | --name | --size) shift ;;
	-*) ;;
	*) [ -z "$cmd" ] && cmd=$1 ;;
	esac
	shift
done
case ${FAKE_UPDATE:-found}:$cmd in
*:info) info ;;
noab:check)
	info 0
	echo "local${T}status=none"
	found net found net 0 noab "https://github.com/x/retrostoneos-0.2.0-retrostone2.img.xz"
	echo "best${T}status=none" ;;
uptodate:check)
	info
	echo "local${T}status=none"
	echo "net${T}status=uptodate${T}source=net${T}version=0.1.0${T}size=0${T}where=${T}name=${T}page=${T}signed=0${T}installable=0${T}verdict=ok${T}notes="
	echo "best${T}status=none" ;;
usb:check)
	info
	found local found file 1 ok "$dir/RetroStoneOS/retrostoneos-0.2.0-retrostone2.rsu"
	found best found file 1 ok "$dir/RetroStoneOS/retrostoneos-0.2.0-retrostone2.rsu" ;;
*:check)
	info
	echo "local${T}status=none"
	found net found net 1 ok "$URL"
	found best found net 1 ok "$URL" ;;
fail:apply)
	echo "state${T}phase=download"
	echo "error${T}code=battery${T}msg=Battery too low${T}detail=battery at 12% without a charger"
	exit 1 ;;
*:apply)
	echo "state${T}phase=download"
	echo "progress${T}phase=download${T}done=44040192${T}total=88080384"
	echo "progress${T}phase=verify${T}done=88080384${T}total=88080384"
	echo "progress${T}phase=write${T}done=268435456${T}total=536870912"
	echo "progress${T}phase=readback${T}done=536870912${T}total=536870912"
	echo "progress${T}phase=switch${T}done=1${T}total=1"
	echo "done${T}version=0.2.0${T}slot=b" ;;
*:boot) echo "boot${T}event=${FAKE_UPDATE_BOOT:-none}${T}version=0.2.0" ;;
*) echo "error${T}code=internal${T}msg=unknown${T}detail=fake: $cmd"; exit 2 ;;
esac
exit 0
