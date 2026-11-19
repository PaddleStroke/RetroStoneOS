#!/bin/sh
# bt-test.sh [WORKDIR]: rsos-bt (rootfs-overlay/usr/bin/rsos-bt) with a stub
# bluetoothctl that keeps its devices in a file: the list format (connected,
# paired, found; nameless devices left out; colour codes stripped), pair =
# pair + trust + connect with the Just Works agent, a pad that does not pair,
# the battery of a connected pad (the battery= extra of the optional 4th field), connect / disconnect / forget, the scan started in the background and
# stopped, a bad address, Bluetooth off. The real bluetoothctl and pads need
# the device.
set -u
W=${1:-${TMPDIR:-/tmp}/rsos-bt-test.$$}
HERE=$(cd "$(dirname "$0")" && pwd)
BT="$HERE/../rootfs-overlay/usr/bin/rsos-bt"
SH="sh"
command -v busybox > /dev/null 2>&1 && SH="busybox sh"
rm -rf "$W"
mkdir -p "$W/bin"
LOG=$W/calls.log
fails=0

ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fails=$((fails + 1)); }
check() { # check "description" command...
	d=$1
	shift
	if "$@"; then ok "$d"; else bad "$d"; fi
}

# The stub's devices: "MAC paired trusted connected name" (0/1 flags).
cat > "$W/devices" << 'EOF'
E4:17:D8:00:00:01 1 1 1 8BitDo SN30 Pro
F0:13:C3:00:00:02 0 0 0 Stadia5RHM-1a2b
A0:5A:5D:00:00:03 0 0 0 Wireless Controller
11:22:33:44:55:66 0 0 0 11-22-33-44-55-66
00:1B:66:00:00:07 0 0 0 WH-Headphones
EOF
cat > "$W/bin/bluetoothctl" << EOF
#!/bin/sh
D=$W/devices
echo "bluetoothctl \$*" >> $LOG
while [ \$# -gt 0 ]; do
	case \$1 in --agent | --timeout) shift 2 ;; *) break ;; esac
done
esc=\$(printf '\033')
setf() { # setf MAC column value
	awk -v m="\$1" -v c="\$2" -v v="\$3" '\$1 == m { \$c = v } { print }' \$D > \$D.t && mv \$D.t \$D
}
case \$1 in
devices)
	col=0
	case \${2:-} in Paired) col=2 ;; Trusted) col=3 ;; Connected) col=4 ;; esac
	printf '%s[0;94m[bluetooth]%s[0m# \r\n' "\$esc" "\$esc"
	awk -v c=\$col '!c || \$c == 1 { n = \$1; \$1 = \$2 = \$3 = \$4 = ""; sub(/^ +/, ""); print "Device " n " " \$0 }' \$D
	;;
info)
	awk -v m="\$2" '\$1 == m {
		print "Device " m " (public)"; print "\tPaired: " (\$2 ? "yes" : "no")
		print "\tTrusted: " (\$3 ? "yes" : "no"); print "\tConnected: " (\$4 ? "yes" : "no")
		if (\$4 && m == "E4:17:D8:00:00:01") print "\tBattery Percentage: 0x55 (85)"
		if (\$4 && \$5 ~ /Headphones/) print "\tBattery Percentage: 0x37 (55)"
		if (\$5 ~ /Headphones/) { print "\tIcon: audio-headphones"
			print "\tUUID: Audio Sink                (0000110b-0000-1000-8000-00805f9b34fb)" }
		else if (\$5 !~ /^[0-9]/) print "\tIcon: input-gaming" }' \$D
	;;
pair)
	[ "\$2" = A0:5A:5D:00:00:03 ] && { echo "Failed to pair: org.bluez.Error.AuthenticationFailed"; exit 1; }
	setf "\$2" 2 1; echo "Pairing successful" ;;
trust) setf "\$2" 3 1 ;;
connect) setf "\$2" 4 1; echo "Connection successful" ;;
disconnect) setf "\$2" 4 0 ;;
remove) awk -v m="\$2" '\$1 != m' \$D > \$D.t && mv \$D.t \$D ;;
scan) exec sleep 60 ;;
esac
EOF
chmod +x "$W/bin/"*
echo $$ > "$W/bluetoothd.pid"           # "bluetoothd runs": a live pid
export RSOS_BT_RUN="$W/run" RSOS_BT_PATH="$W/bin:/usr/bin:/bin" RSOS_BT_DAEMON_PID="$W/bluetoothd.pid"
T=$(printf '\t')

bt() { $SH "$BT" "$@"; }

echo "rsos-bt ($SH)"
check "list" bt list "$W/list"
check "list: connected (with its battery), found, found; the nameless one and colour codes left out" \
	[ "$(cat "$W/list")" = "E4:17:D8:00:00:01${T}connected${T}8BitDo SN30 Pro${T}battery=85
F0:13:C3:00:00:02${T}found${T}Stadia5RHM-1a2b
A0:5A:5D:00:00:03${T}found${T}Wireless Controller
00:1B:66:00:00:07${T}found${T}WH-Headphones${T}audio=headphones" ]
check "sink: none connected" sh -c "! $SH '$BT' sink"
check "scan on starts a background bluetoothctl" bt scan on
pid=$(cat "$W/run/scan.pid" 2> /dev/null)
check "  ... which runs" kill -0 "$pid"
bt scan on
check "scan on again: the same one" [ "$(cat "$W/run/scan.pid")" = "$pid" ]
check "pair: ok" bt pair F0:13:C3:00:00:02
check "  ... it stopped the scan" sh -c "! kill -0 $pid 2> /dev/null && [ ! -f '$W/run/scan.pid' ]"
check "  ... Just Works agent, then trust and connect" \
	sh -c "grep -q 'bluetoothctl --agent NoInputNoOutput --timeout 30 pair F0:13:C3:00:00:02' $LOG && \
	grep -q 'bluetoothctl trust F0:13:C3:00:00:02' $LOG && grep -q 'connect F0:13:C3:00:00:02' $LOG"
bt list "$W/list"
check "  ... now connected" grep -q "^F0:13:C3:00:00:02${T}connected${T}" "$W/list"
check "  ... no extras field for a pad that does not report one" \
	grep -qx "F0:13:C3:00:00:02${T}connected${T}Stadia5RHM-1a2b" "$W/list"
out=$(bt pair A0:5A:5D:00:00:03)
rc=$?
check "a pad that does not pair: exit 1, the reason" \
	[ "$rc:$out" = "1:rsos-bt: pairing failed: Failed to pair: org.bluez.Error.AuthenticationFailed" ]
check "pair headphones: ok" bt pair 00:1B:66:00:00:07
bt list "$W/list"
check "  ... connected, marked audio=headphones, with its battery first" grep -q "^00:1B:66:00:00:07${T}connected${T}WH-Headphones${T}battery=55,audio=headphones$" "$W/list"
check "  ... a pad stays a plain three-field line" grep -q "^F0:13:C3:00:00:02${T}connected${T}Stadia5RHM-1a2b$" "$W/list"
check "sink: the headphones are connected" bt sink
bt disconnect 00:1B:66:00:00:07
check "sink: not after a disconnect" sh -c "! $SH '$BT' sink"
bt connect 00:1B:66:00:00:07
bt forget 00:1B:66:00:00:07
check "  ... forgotten: its kind is not kept" sh -c "! grep -q 00:1B:66:00:00:07 '$W/run/kinds'"
check "disconnect" bt disconnect E4:17:D8:00:00:01
bt list "$W/list"
check "  ... paired, not connected, no battery" grep -q "^E4:17:D8:00:00:01${T}paired${T}8BitDo SN30 Pro$" "$W/list"
check "connect" bt connect E4:17:D8:00:00:01
check "forget" bt forget E4:17:D8:00:00:01
bt list "$W/list"
check "  ... gone" sh -c "! grep -q E4:17:D8:00:00:01 '$W/list'"
check "a bad address is refused" sh -c "! $SH '$BT' pair 'AA:BB; reboot' > /dev/null"
rm -f "$W/bluetoothd.pid"
check "Bluetooth off: list fails, the file is empty" sh -c "! $SH '$BT' list '$W/list' > /dev/null && [ ! -s '$W/list' ]"
check "Bluetooth off: scan on fails" sh -c "! $SH '$BT' scan on > /dev/null"
echo "rsos-bt: $fails failed"
[ -n "${1:-}" ] || rm -rf "$W"
[ "$fails" = 0 ]
