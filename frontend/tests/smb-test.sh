#!/bin/sh
# smb-test.sh WORKDIR: the Windows file share helper (src/transfer/rsos-smb)
# with stub commands in place of modprobe and ksmbd-tools: the configuration
# it writes (user, signing, no guest, the share of /data), the user database
# made with the PIN, the start and stop order, a missing kernel module, a bad
# PIN, and "status". The real ksmbd-tools reading this configuration are
# checked separately (docs/rom-transfer.md §3.3: qemu-arm on the image's
# binaries); the kernel server needs the device.
set -u
W=${1:?usage: smb-test.sh WORKDIR}
HERE=$(cd "$(dirname "$0")" && pwd)
SMB="$HERE/../src/transfer/rsos-smb"
rm -rf "$W"
mkdir -p "$W/bin" "$W/data"
LOG=$W/calls.log
fails=0

ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fails=$((fails + 1)); }
check() { # check "description" command...
	d=$1
	shift
	if "$@"; then ok "$d"; else bad "$d"; fi
}

# --- stubs: they log their arguments; the "module" is a folder
stub() {
	printf '#!/bin/sh\n%s\n' "$2" > "$W/bin/$1"
	chmod +x "$W/bin/$1"
}
stub modprobe "echo \"modprobe \$*\" >> $LOG
[ -n \"\${STUB_NO_MODULE:-}\" ] && exit 1
if [ \"\$1\" = -r ]; then rm -rf $W/sysmod; else mkdir -p $W/sysmod; fi"
stub ksmbd.adduser "echo \"adduser \$*\" >> $LOG
while [ \$# -gt 0 ]; do [ \"\$1\" = -P ] && : > \"\$2\"; shift; done"
stub ksmbd.mountd "echo \"mountd \$*\" >> $LOG
[ -n \"\${STUB_MOUNTD_DIES:-}\" ] || : > $W/mountd.pid"
stub ksmbd.control "echo \"control \$*\" >> $LOG
rm -f $W/mountd.pid"
stub pidof "[ -e $W/mountd.pid ]"
stub usleep "exit 0"

export RSOS_SMB_RUN=$W/run RSOS_SMB_SHARE=$W/data RSOS_SMB_SYSMOD=$W/sysmod
export RSOS_SMB_PATH="$W/bin:/usr/bin:/bin"
CONF=$W/run/ksmbd.conf

echo "rsos-smb: start (the PIN, the console name)"
out=$(sh "$SMB" start 482913 retrostone-2 2> "$W/err")
check "start prints on" [ "$out" = on ]
check "the configuration in the run folder" [ -s "$CONF" ]
check "netbios name from the console name (uppercase)" grep -q 'netbios name = RETROSTONE-2' "$CONF"
check "signing mandatory, no guest, SMB2 or newer" \
	sh -c "grep -q 'server signing = mandatory' $CONF && grep -q 'map to guest = never' $CONF && \
	grep -q 'server min protocol = SMB2_10' $CONF && grep -q 'guest ok = no' $CONF"
check "the share is the data folder, written as root" \
	sh -c "grep -q 'path = $W/data' $CONF && grep -q 'force user = root' $CONF && grep -q 'read only = no' $CONF"
check "only user retrostone" grep -q 'valid users = retrostone' "$CONF"
check "\$RECYCLE.BIN vetoed literally" grep -q 'System Volume Information/\$RECYCLE.BIN/' "$CONF"
check "user retrostone added with the PIN as password" \
	grep -q "adduser -C $CONF -P $W/run/ksmbdpwd.db -a -p 482913 retrostone" "$LOG"
check "then the module, then ksmbd.mountd with the configuration" \
	sh -c "grep -n . $LOG | grep -q '^2:modprobe ksmbd' && grep -n . $LOG | grep -q '^3:mountd -C $CONF -P $W/run/ksmbdpwd.db'"
check "status: on" [ "$(sh "$SMB" status)" = on ]

echo "rsos-smb: start again while it runs (a new PIN): stopped first"
: > "$LOG"
sh "$SMB" start 111111 > /dev/null 2>&1
check "the old server is shut down before the new one" \
	sh -c "grep -n . $LOG | grep -q '^1:control -s' && grep -q 'adduser .* -p 111111 retrostone' $LOG"
check "default name RETROSTONE" grep -q 'netbios name = RETROSTONE$' "$CONF"

echo "rsos-smb: stop"
: > "$LOG"
out=$(sh "$SMB" stop)
check "stop prints off" [ "$out" = off ]
check "ksmbd.control -s, then the module removed" \
	sh -c "grep -n . $LOG | grep -q '^1:control -s' && grep -q 'modprobe -r ksmbd' $LOG"
check "nothing left in the run folder" [ ! -e "$W/run" ]
check "status: off" [ "$(sh "$SMB" status)" = off ]

echo "rsos-smb: failures"
STUB_NO_MODULE=1 sh "$SMB" start 482913 > /dev/null 2> "$W/err"
rc=$?
check "no kernel module: exit 1, a message, nothing left" \
	sh -c "[ $rc = 1 ] && grep -q 'ksmbd kernel module is missing' $W/err && [ ! -e $W/run ]"
STUB_MOUNTD_DIES=1 sh "$SMB" start 482913 > /dev/null 2> "$W/err"
rc=$?
check "ksmbd.mountd gone at once (no kernel server): exit 1, the module removed, nothing left" \
	sh -c "[ $rc = 1 ] && grep -q 'stopped at once' $W/err && [ ! -e $W/run ] && [ ! -e $W/sysmod ]"
sh "$SMB" start "12;rm" > /dev/null 2> "$W/err"
check "a PIN with other characters is refused" [ $? = 1 ]
sh "$SMB" start 12 > /dev/null 2> "$W/err"
check "a short PIN is refused" [ $? = 1 ]
RSOS_SMB_SHARE=$W/nowhere sh "$SMB" start 482913 > /dev/null 2> "$W/err"
check "no /data: refused" [ $? = 1 ]
sh "$SMB" bogus > /dev/null 2>&1
check "unknown command: usage, exit 2" [ $? = 2 ]

if [ $fails = 0 ]; then
	echo "smb-test: ALL OK"
else
	echo "smb-test: $fails FAILED"
	exit 1
fi
