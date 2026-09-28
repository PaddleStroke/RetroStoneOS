#!/bin/sh
# webshare_test.sh - curl tests for the web share (host).
#
#   sh webshare_test.sh path/to/rsos-webshare-host [port]
#
# Starts the server on a temp data root, then checks: the page, login and
# PIN, the session cookie and token header, CSRF header, Host check,
# multi-file and large uploads (content compared), overwrite rules and
# .bak for saves, listing, delete, path traversal and symlink escapes,
# chunked bodies, an aborted upload (no partial file), disk full (tmpfs,
# root only) and the wrong-PIN lockout.

BIN=${1:?usage: $0 rsos-webshare-host [port]}
PORT=${2:-18080}
PIN=246810
T=$(mktemp -d /tmp/rsos-webshare-test.XXXXXX)
ROOT=$T/data
JAR=$T/cookies
URL=http://127.0.0.1:$PORT
PASS=0
FAIL=0
mkdir -p "$ROOT" "$T/outside"

ok() { PASS=$((PASS + 1)); echo "  ok   $*"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL $*"; }
# expect <what> <wanted> <got>
expect() { if [ "$2" = "$3" ]; then ok "$1 ($3)"; else ko "$1: wanted $2, got $3"; fi; }

code() { curl -s -o "$T/body" -w '%{http_code}' "$@"; }
# authenticated request with the CSRF header
acode() { code -b "$JAR" -H 'X-Requested-With: rsos' "$@"; }
upload() { # <query> <file>
	acode -T "$2" "$URL/api/upload?$1"
}

"$BIN" --root "$ROOT" --port "$PORT" --pin "$PIN" --idle 0 > "$T/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$T"' EXIT
i=0
until curl -s -o /dev/null "$URL/" || [ $i -ge 50 ]; do
	sleep 0.1
	i=$((i + 1))
done

echo "== page and login"
expect "GET /" 200 "$(code "$URL/")"
grep -q 'RetroStone' "$T/body" && ok "page content" || ko "page content"
curl -s -D "$T/hdr" -o /dev/null "$URL/"
grep -qi '^Content-Security-Policy:' "$T/hdr" && ok "CSP header" || ko "CSP header"
expect "info without login" 401 "$(code "$URL/api/info")"
expect "wrong PIN" 403 "$(code -c "$JAR" -d pin=000000 "$URL/api/login")"
expect "right PIN" 200 "$(code -c "$JAR" -d pin=$PIN "$URL/api/login")"
grep -q 'rsos_token' "$JAR" && ok "session cookie set" || ko "session cookie"
curl -s -D "$T/hdr" -o /dev/null -d pin=$PIN "$URL/api/login"
grep -qi 'HttpOnly; SameSite=Strict' "$T/hdr" && ok "cookie is HttpOnly+SameSite" || ko "cookie flags"
expect "info with cookie" 200 "$(acode "$URL/api/info")"
grep -q '"free":' "$T/body" && grep -q '"id":"snes"' "$T/body" && ok "info JSON" || ko "info JSON"
TOKEN=$(awk '$6 == "rsos_token" { print $7 }' "$JAR")
expect "token header auth" 200 "$(code -H "X-RSOS-Token: $TOKEN" "$URL/api/info")"
expect "bad token" 401 "$(code -H "X-RSOS-Token: 0123456789abcdef0123456789abcdef" "$URL/api/info")"
expect "foreign Host header (DNS rebinding)" 421 "$(code -H 'Host: evil.example.com' "$URL/")"
expect "Host retrostone.local accepted" 200 "$(code -H 'Host: retrostone.local:8080' "$URL/")"

echo "== uploads"
printf 'snes rom one' > "$T/a.sfc"
printf 'second rom!!' > "$T/b.sfc"
head -c 70000 /dev/urandom > "$T/c.sfc"
expect "PUT without CSRF header" 403 \
	"$(code -b "$JAR" -T "$T/a.sfc" "$URL/api/upload?target=roms&sys=snes&path=a.sfc")"
expect "upload 1" 201 "$(upload 'target=roms&sys=snes&path=Super%20Mario%20World%20(USA).sfc' "$T/a.sfc")"
expect "upload 2 (UTF-8 name)" 201 "$(upload 'target=roms&sys=snes&path=Pok%C3%A9mon%20%E3%83%9D%E3%82%B1.sfc' "$T/b.sfc")"
expect "upload 3 (sub folder)" 201 "$(upload 'target=roms&sys=snes&path=Hacks/Kaizo%20%231.sfc&mtime=1600000000' "$T/c.sfc")"
cmp -s "$T/a.sfc" "$ROOT/roms/snes/Super Mario World (USA).sfc" && ok "file 1 content" || ko "file 1 content"
cmp -s "$T/b.sfc" "$ROOT/roms/snes/Pokémon ポケ.sfc" && ok "file 2 content" || ko "file 2 content"
cmp -s "$T/c.sfc" "$ROOT/roms/snes/Hacks/Kaizo #1.sfc" && ok "file 3 content" || ko "file 3 content"
[ "$(stat -c %Y "$ROOT/roms/snes/Hacks/Kaizo #1.sfc")" = 1600000000 ] && ok "mtime kept" || ko "mtime"
expect "same name again" 409 "$(upload 'target=roms&sys=snes&path=Super%20Mario%20World%20(USA).sfc' "$T/b.sfc")"
expect "overwrite=1" 201 "$(upload 'target=roms&sys=snes&path=Super%20Mario%20World%20(USA).sfc&overwrite=1' "$T/b.sfc")"
cmp -s "$T/b.sfc" "$ROOT/roms/snes/Super Mario World (USA).sfc" && ok "overwritten" || ko "overwritten"
expect "BIOS upload" 201 "$(upload 'target=bios&path=scph5501.bin' "$T/a.sfc")"
[ -f "$ROOT/bios/scph5501.bin" ] && ok "bios/scph5501.bin" || ko "bios file"
expect "save upload" 201 "$(upload 'target=saves&sys=snes&path=Zelda.srm' "$T/a.sfc")"
expect "save replace" 201 "$(upload 'target=saves&sys=snes&path=Zelda.srm&overwrite=1' "$T/b.sfc")"
cmp -s "$T/a.sfc" "$ROOT/saves/snes/Zelda.srm.bak" && ok "old save kept as .bak" || ko "save .bak"
: > "$T/empty.nes"
expect "empty file" 201 "$(upload 'target=roms&sys=nes&path=empty.nes' "$T/empty.nes")"
[ -f "$ROOT/roms/nes/empty.nes" ] && [ ! -s "$ROOT/roms/nes/empty.nes" ] && ok "empty file created" || ko "empty file"

echo "== large file"
dd if=/dev/urandom of="$T/big.bin" bs=1M count=256 2> /dev/null
S=$(date +%s.%N)
expect "256 MiB upload" 201 "$(upload 'target=roms&sys=psx&path=Big%20Game/disc.bin' "$T/big.bin")"
E=$(date +%s.%N)
cmp -s "$T/big.bin" "$ROOT/roms/psx/Big Game/disc.bin" && ok "256 MiB content identical" || ko "big content"
echo "       ($(echo "$S $E" | awk '{ printf "%.1f MB/s on the host", 256 / ($2 - $1) }'))"

echo "== path traversal and bad names"
for q in \
	'target=roms&sys=snes&path=../../../etc/evil' \
	'target=roms&sys=snes&path=%2e%2e%2f%2e%2e%2fevil' \
	'target=roms&sys=snes&path=/tmp/evil' \
	'target=roms&sys=snes&path=a/../../evil' \
	'target=roms&sys=snes&path=a%5c..%5cevil' \
	'target=roms&sys=snes&path=.hidden' \
	'target=roms&sys=snes&path=con.sfc' \
	'target=roms&sys=snes&path=a%00b' \
	'target=roms&sys=snes&path=a:b' \
	'target=roms&sys=snes&path=' \
	'target=roms&sys=../..&path=evil' \
	'target=../../tmp&path=evil' \
	'target=roms&sys=dreamcast&path=evil' \
	'target=roms&path=evil'; do
	expect "reject $q" 400 "$(upload "$q" "$T/a.sfc")"
done
ln -s "$T/outside" "$ROOT/roms/snes/link"
R=$(upload 'target=roms&sys=snes&path=link/evil.sfc' "$T/a.sfc")
[ "$R" != 201 ] && [ ! -e "$T/outside/evil.sfc" ] && ok "symlink not followed ($R)" || ko "symlink escape ($R)"
[ -z "$(find /tmp /etc "$T" -name 'evil*' 2> /dev/null)" ] && ok "no 'evil' file created anywhere" || ko "evil file created"
expect "GET /../../etc/passwd" 404 "$(code --path-as-is "$URL/../../etc/passwd")"
expect "chunked body refused" 411 \
	"$(printf 'xx' | acode -H 'Transfer-Encoding: chunked' -T - "$URL/api/upload?target=roms&sys=nes&path=c.nes")"

echo "== list and delete"
expect "list snes" 200 "$(acode "$URL/api/list?target=roms&sys=snes")"
grep -q 'Hacks/Kaizo #1.sfc' "$T/body" && grep -q 'Pok' "$T/body" && ok "list content" || ko "list content"
grep -q 'rsos-part' "$T/body" && ko "temp files listed" || ok "no temp files listed"
expect "delete" 200 "$(acode -X DELETE "$URL/api/file?target=roms&sys=snes&path=Hacks/Kaizo%20%231.sfc")"
[ ! -e "$ROOT/roms/snes/Hacks/Kaizo #1.sfc" ] && ok "deleted" || ko "not deleted"
expect "delete missing" 404 "$(acode -X DELETE "$URL/api/file?target=roms&sys=snes&path=nope.sfc")"
expect "delete traversal" 400 "$(acode -X DELETE "$URL/api/file?target=roms&sys=snes&path=../../x")"
expect "delete without CSRF header" 403 "$(code -b "$JAR" -X DELETE "$URL/api/file?target=roms&sys=snes&path=b.sfc")"
expect "delete empty folder" 200 "$(acode -X DELETE "$URL/api/file?target=roms&sys=snes&path=Hacks")"

echo "== aborted upload"
dd if=/dev/urandom of="$T/half.bin" bs=1M count=64 2> /dev/null
curl -s -o /dev/null -b "$JAR" -H 'X-Requested-With: rsos' --limit-rate 4M \
	-T "$T/half.bin" "$URL/api/upload?target=roms&sys=psx&path=half.bin" &
C=$!
sleep 1.5
kill $C 2> /dev/null
wait $C 2> /dev/null
sleep 0.5
[ ! -e "$ROOT/roms/psx/half.bin" ] && ok "no half file" || ko "half file visible"
[ -z "$(find "$ROOT" -name '*rsos-part*')" ] && ok "temp file removed" || ko "temp file left"

if [ "$(id -u)" = 0 ]; then
	echo "== disk full (second server on a 20 MiB tmpfs)"
	P2=$((PORT + 1))
	mkdir -p "$T/small"
	mount -t tmpfs -o size=20m tmpfs "$T/small"
	"$BIN" --root "$T/small" --port $P2 --pin 135790 --idle 0 > "$T/server2.log" 2>&1 &
	SRV2=$!
	sleep 0.3
	curl -s -o /dev/null -c "$T/jar2" -d pin=135790 "http://127.0.0.1:$P2/api/login"
	dd if=/dev/urandom of="$T/30m.bin" bs=1M count=30 2> /dev/null
	expect "30 MiB into 20 MiB" 507 "$(code -b "$T/jar2" -H 'X-Requested-With: rsos' \
		-T "$T/30m.bin" "http://127.0.0.1:$P2/api/upload?target=roms&sys=gba&path=big.gba")"
	[ -z "$(find "$T/small" -type f)" ] && ok "nothing written" || ko "files left on full disk"
	kill $SRV2
	wait $SRV2 2> /dev/null
	umount "$T/small"
fi

echo "== wrong PIN lockout"
for n in 1 2 3 4 5; do
	code -d pin=11111$n "$URL/api/login" > /dev/null
done
expect "6th try, even with the right PIN" 429 "$(code -d pin=$PIN "$URL/api/login")"
grep -q '"wait":30' "$T/body" && ok "lockout 30 s" || ko "lockout time: $(cat "$T/body")"

echo "== stop"
kill -INT $SRV
wait $SRV 2> /dev/null
tail -n 2 "$T/server.log" | sed 's/^/       /'
grep -q '^changed:.*snes' "$T/server.log" && ok "changed systems reported" || ko "changed systems"

echo "$PASS passed, $FAIL failed"
[ $FAIL = 0 ]
