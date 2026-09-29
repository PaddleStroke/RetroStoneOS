#!/bin/sh
# Test of /usr/libexec/rsos/frontend-respawn (init's respawn entry for the
# menu: a crash-looping menu stops being restarted and the watchdog resets
# the board). Run anywhere (as any user):
#   sh buildroot-external/board/common/tests/frontend-respawn-test.sh
set -u
T=$(cd "$(dirname "$0")/../rootfs-overlay/usr/libexec/rsos" && pwd)/frontend-respawn
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/run"
# the "frontend": says it ran, with its arguments, and exits (a crash)
printf '#!/bin/sh\necho "ran $*" >> %s/fe.log\nexit 1\n' "$W" > "$W/fe"
chmod +x "$W/fe"

pass=0
fail=0
check() { if eval "$2"; then echo "  ok: $1"; pass=$((pass + 1)); else echo "  FAIL: $1"; fail=$((fail + 1)); fi; }
uptime() { echo "$1.42 100.00" > "$W/uptime"; }
# one start by init at uptime <s>
start() {
	uptime "$1"
	RSOS_RUN=$W/run RSOS_FRONTEND=$W/fe RSOS_UPTIME=$W/uptime RSOS_WATCHDOG=$W/wd RSOS_KMSG=$W/kmsg \
		RSOS_RESPAWN_HOLD_S=0 RSOS_REBOOT="touch $W/rebooted" RSOS_RESPAWN_TEST=1 \
		sh "$T" --arg 2> "$W/err.txt"
}
runs() { [ -f "$W/fe.log" ] && wc -l < "$W/fe.log" | tr -d ' ' || echo 0; }

echo "== a menu that crashes at once: 5 starts, then no more; the watchdog is opened"
for s in 10 11 12 13 14; do start $s; done
check "5 starts, with the arguments" "[ \$(runs) = 5 ] && grep -q '^ran --arg$' $W/fe.log"
check "no give-up yet" "[ ! -e $W/wd ] && [ ! -e $W/run/frontend-gave-up ]"
start 15; rc=$?
check "the 6th: not started, the watchdog opened (and left running), said" \
	"[ $rc = 3 ] && [ \$(runs) = 5 ] && [ -e $W/wd ] && [ ! -s $W/wd ] && grep -q 'started 5 times' $W/kmsg && [ -s $W/run/frontend-gave-up ]"
check "the fallback reboot after the hold time" "[ -e $W/rebooted ]"

echo "== slow crashes (one a minute): always restarted"
rm -f "$W/fe.log" "$W/wd" "$W/run"/* "$W/rebooted"
for s in 100 125 150 175 200 225 250 275; do start $s; done
check "8 starts over 3 minutes" "[ \$(runs) = 8 ] && [ ! -e $W/wd ]"

echo "== development: respawn-unlimited"
rm -f "$W/fe.log" "$W/wd" "$W/run"/*
: > "$W/run/respawn-unlimited"
for s in 300 300 300 300 300 300 300; do start $s; done
check "7 starts in the same second" "[ \$(runs) = 7 ] && [ ! -e $W/wd ]"

echo "== a damaged starts file is ignored"
rm -f "$W/fe.log" "$W/run"/*
echo "x 12a -5" > "$W/run/frontend-starts"
start 400
check "started" "[ \$(runs) = 1 ] && [ \"\$(cat $W/run/frontend-starts)\" = 400 ]"

echo "PASS=$pass FAIL=$fail"
[ $fail -eq 0 ]
