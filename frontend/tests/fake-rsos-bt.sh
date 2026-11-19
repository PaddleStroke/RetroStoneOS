#!/bin/sh
# fake-rsos-bt.sh: a stand-in for rsos-bt (buildroot-external/board/common/
# rootfs-overlay/usr/bin/rsos-bt) for the preview tool (--bt-helper): the
# devices live in $FAKE_BT_DIR/devices ("MAC<TAB>state<TAB>name[<TAB>key=value,...]", the
# format of "rsos-bt list"), created with one paired and two found devices; every
# call is logged to $FAKE_BT_DIR/calls.log. FAKE_BT_FAIL=pair makes pairing
# fail, FAKE_BT_FAIL=pin makes it fail as a pad that needs a PIN or a cable
# does. Headphones carry audio=headphones in the extras ("sink" says whether
# one is connected). A device that gets connected reports battery=70 and a key the frontend
# does not know (it must ignore it); the Wireless Controller has an unknown key too.
D=${FAKE_BT_DIR:?FAKE_BT_DIR is not set}
mkdir -p "$D"
DEV=$D/devices
T=$(printf '\t')
if [ ! -f "$DEV" ]; then
	printf '%s\t%s\t%s\n' \
		"E4:17:D8:00:00:01" paired "8BitDo SN30 Pro" \
		"F0:13:C3:00:00:02" found "Stadia5RHM-1a2b" \
		"A0:5A:5D:00:00:03" found "Wireless Controller" > "$DEV"
	sed "/^A0:5A:5D:00:00:03/s/\$/${T}future=1/" "$DEV" > "$DEV.tmp" && mv "$DEV.tmp" "$DEV"
	# headphones: the extras carry audio=headphones
	printf '%s\t%s\t%s\t%s\n' "00:1B:66:00:00:07" found "WH-1000XM3" audio=headphones >> "$DEV"
fi
echo "$*" >> "$D/calls.log"

# set MAC STATE: change the state of a device (connected: with a battery)
set_state() {
	sed "/^${1}${T}/s/${T}[a-z]\{1,\}=[^${T}]*\$//; s/^${1}${T}[a-z]*${T}/${1}${T}${2}${T}/" "$DEV" > "$DEV.tmp" &&
		mv "$DEV.tmp" "$DEV"
	[ "$2" = connected ] && sed "/^${1}${T}/s/\$/${T}battery=70,future=x/" "$DEV" > "$DEV.tmp" && mv "$DEV.tmp" "$DEV"
	# the kind of a device does not change: audio=headphones stays (last, after battery)
	if [ "$1" = 00:1B:66:00:00:07 ]; then
		if [ "$2" = connected ]; then
			sed "/^${1}${T}/s/\$/,audio=headphones/" "$DEV" > "$DEV.tmp" && mv "$DEV.tmp" "$DEV"
		else
			sed "/^${1}${T}/s/\$/${T}audio=headphones/" "$DEV" > "$DEV.tmp" && mv "$DEV.tmp" "$DEV"
		fi
	fi
	return 0
}

case "$1" in
list)
	cp "$DEV" "$2.tmp" && mv "$2.tmp" "$2"
	;;
scan) ;;
sink) grep -q "${T}connected${T}.*audio=headphones" "$DEV" || exit 1 ;;
pair)
	sleep 1
	if [ "${FAKE_BT_FAIL:-}" = pair ]; then
		echo "rsos-bt: pairing failed: org.bluez.Error.Failed"
		exit 1
	fi
	if [ "${FAKE_BT_FAIL:-}" = pin ]; then
		echo "rsos-bt: pairing failed: Failed to pair: org.bluez.Error.AuthenticationFailed"
		exit 1
	fi
	set_state "$2" connected
	;;
connect) set_state "$2" connected ;;
disconnect) set_state "$2" paired ;;
forget)
	grep -v "^$2$T" "$DEV" > "$DEV.tmp"
	mv "$DEV.tmp" "$DEV"
	;;
*) exit 2 ;;
esac
exit 0
