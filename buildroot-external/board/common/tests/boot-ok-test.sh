#!/bin/sh
# Test of /usr/bin/rsos-boot-ok (A/B confirmation, refund, stability wait)
# with the host fw_printenv/fw_setenv of ~/rsos/output on an environment
# image in a file. Run in WSL after a build (as any user):
#   sh buildroot-external/board/common/tests/boot-ok-test.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
T=$(cd "$(dirname "$0")/../rootfs-overlay/usr/bin" && pwd)/rsos-boot-ok
H=$HOME/rsos/output/host/bin
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/run"
printf '%s 0x0 0x10000\n%s 0x10000 0x10000\n' "$W/env.img" "$W/env.img" > "$W/fw_env.config"
# the "frontend": a sleeping process that pidof reports
sleep 1000 &
FE=$!
printf '#!/bin/sh\ncat %s/fe.pid 2>/dev/null\n' "$W" > "$W/bin/pidof"
chmod +x "$W/bin/pidof"
echo $FE > "$W/fe.pid"

pass=0; fail=0
check() { if eval "$2"; then echo "  ok: $1"; pass=$((pass+1)); else echo "  FAIL: $1"; fail=$((fail+1)); fi; }
# env_init "var=value ..."
env_init() {
	for kv in "$@"; do echo "$kv"; done > "$W/env.txt"
	"$H/mkenvimage" -r -s 0x10000 -o "$W/copy.img" "$W/env.txt"
	cat "$W/copy.img" "$W/copy.img" > "$W/env.img"
	rm -f "$W/run"/*
}
get() { "$H/fw_printenv" -c "$W/fw_env.config" -n "$1" 2> /dev/null; }
bo() { # <cmdline> [args]
	c=$1; shift
	echo "$c" > "$W/cmdline"
	RSOS_RUN=$W/run RSOS_CMDLINE=$W/cmdline RSOS_FW_CONFIG=$W/fw_env.config \
		RSOS_TEST_PATH=$W/bin RSOS_BOOT_OK_DELAY=${DELAY:-1} \
		sh "$T" "$@" 2> "$W/err.txt"
}
# The host fw_printenv/fw_setenv on the file (-c; fw_setenv is the same
# binary called by that name)
for t in fw_printenv fw_setenv; do
	printf '#!/bin/bash\nexec -a %s %s/fw_printenv -c %s/fw_env.config "$@"\n' "$t" "$H" "$W" > "$W/bin/$t"
	chmod +x "$W/bin/$t"
done

echo "== confirm at once (now) a counted boot of a confirmed slot"
env_init rsos_slot=a rsos_ok=1 rsos_tries=0 rsos_fails=1
bo "root=/dev/mmcblk0p2 rsos.slot=a rsos.boot=pending" now; rc=$?
check "exit 0, fails=0, state confirmed" "[ $rc -eq 0 ] && [ \"\$(get rsos_fails)\" = 0 ] && [ \"\$(cat $W/run/boot-state)\" = confirmed ]"

echo "== refund on an orderly shutdown before the confirmation"
env_init rsos_slot=a rsos_ok=1 rsos_tries=0 rsos_fails=2
echo pending > "$W/run/boot-state"
bo "rsos.slot=a rsos.boot=pending" refund; rc=$?
check "fails 2 -> 1, state refunded" "[ $rc -eq 0 ] && [ \"\$(get rsos_fails)\" = 1 ] && [ \"\$(cat $W/run/boot-state)\" = refunded ]"
bo "rsos.slot=a rsos.boot=pending" refund
check "a second refund does nothing" "[ \"\$(get rsos_fails)\" = 1 ]"
bo "rsos.slot=a rsos.boot=pending" now
check "no confirmation after a refund (shutting down)" "[ \"\$(get rsos_fails)\" = 1 ]"

echo "== refund of a slot on trial"
env_init rsos_slot=b rsos_ok=0 rsos_tries=2
bo "rsos.slot=b rsos.boot=pending" refund
check "tries 2 -> 3" "[ \"\$(get rsos_tries)\" = 3 ]"

echo "== the running slot is not the selected one"
env_init rsos_slot=a rsos_ok=0 rsos_tries=1
bo "rsos.slot=b rsos.boot=pending" now; rc=$?
check "refused, environment unchanged" "[ $rc -ne 0 ] && [ \"\$(get rsos_ok)\" = 0 ] && grep -q 'not the selected slot' $W/err.txt"

echo "== booted without rsos.slot= (bootflow scan fallback)"
env_init rsos_slot=a rsos_ok=0 rsos_tries=1
bo "root=/dev/sda1" now; rc=$?
check "refused" "[ $rc -ne 0 ] && [ \"\$(get rsos_ok)\" = 0 ]"

echo "== confirming after a fallback clears it and tells the UI"
env_init rsos_slot=a rsos_ok=1 rsos_tries=0 rsos_fails=1 rsos_fallback=b
bo "rsos.slot=a rsos.boot=pending" now
check "fallback cleared, /run/rsos/boot-fallback = b" "[ -z \"\$(get rsos_fallback)\" ] && [ \"\$(cat $W/run/boot-fallback)\" = b ]"

echo "== nothing to write when the slot is already confirmed"
env_init rsos_slot=a rsos_ok=1 rsos_tries=0 rsos_fails=0
before=$(md5sum < "$W/env.img")
bo "rsos.slot=a rsos.boot=ok" now
check "environment image unchanged" "[ \"\$(md5sum < $W/env.img)\" = \"$before\" ]"

echo "== wait mode: net-applied, then the delay, then confirm"
env_init rsos_slot=a rsos_ok=1 rsos_tries=0 rsos_fails=1
echo pending > "$W/run/boot-state"
bo "rsos.slot=a rsos.boot=pending" &
WP=$!
sleep 3
check "still waiting for net-applied" "kill -0 $WP 2> /dev/null && [ \"\$(get rsos_fails)\" = 1 ]"
bo "rsos.slot=a rsos.boot=pending"; rc=$?
check "a second waiter exits at once" "[ $rc -eq 0 ] && kill -0 $WP 2> /dev/null"
: > "$W/run/net-applied"
wait $WP
check "confirmed after net-applied + delay" "[ \"\$(get rsos_fails)\" = 0 ] && [ \"\$(cat $W/run/boot-state)\" = confirmed ]"

echo "== wait mode: the frontend restarted meanwhile"
env_init rsos_slot=a rsos_ok=1 rsos_tries=0 rsos_fails=1
echo pending > "$W/run/boot-state"
: > "$W/run/net-applied"
DELAY=2 bo "rsos.slot=a rsos.boot=pending" &
WP=$!
sleep 1; kill $FE; wait $FE 2> /dev/null
sleep 1000 & FE=$!; echo $FE > "$W/fe.pid"
wait $WP; rc=$?
check "not confirmed" "[ $rc -ne 0 ] && [ \"\$(get rsos_fails)\" = 1 ] && grep -q 'frontend restarted' $W/err.txt"

echo "== wait mode in charge mode: waits beyond 120 s for the user to leave it"
env_init rsos_slot=a rsos_ok=1 rsos_tries=0 rsos_fails=1
echo pending > "$W/run/boot-state"
echo charger > "$W/run/bootreason"
bo "rsos.slot=a rsos.boot=pending" &
WP=$!
sleep 3
check "waiting" "kill -0 $WP 2> /dev/null"
: > "$W/run/net-applied"
wait $WP
check "confirmed once rsos-net apply ran" "[ \"\$(get rsos_fails)\" = 0 ]"

echo "== a board without the U-Boot environment"
rm -f "$W/fw_env.config" "$W/run"/*
bo "rsos.slot=a rsos.boot=pending" now; rc=$?
check "exit 0, nothing done" "[ $rc -eq 0 ] && [ ! -e $W/run/boot-state ]"

kill $FE 2> /dev/null
echo "PASS=$pass FAIL=$fail"
[ $fail -eq 0 ]
