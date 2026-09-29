#!/bin/sh
# webshare_test.sh - curl tests for the web share (host).
#
#   sh webshare_test.sh path/to/rsos-webshare-host [port]
#
# Starts the server on a temp data root, then checks: the page, login and
# PIN, the session cookie and token header, CSRF header, Host check,
# multi-file and large uploads (content compared), overwrite rules, the
# .bak generations of saves (the same save again changes nothing), listing,
# delete (a save with its .bak files), non-ASCII names, path traversal and
# symlink escapes, chunked bodies, an aborted upload (no partial file), disk
# full (tmpfs, root only) and the wrong-PIN lockout.

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
# a login: the page sends the CSRF header there too
lcode() { code -H 'X-Requested-With: rsos' "$@"; }
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
expect "login without the CSRF header" 403 "$(code -c "$JAR" -d pin=$PIN "$URL/api/login")"
grep -q csrf "$T/body" && ok "refused as csrf" || ko "login csrf: $(cat "$T/body")"
expect "wrong PIN" 403 "$(lcode -c "$JAR" -d pin=000000 "$URL/api/login")"
expect "right PIN" 200 "$(lcode -c "$JAR" -d pin=$PIN "$URL/api/login")"
grep -q 'rsos_token' "$JAR" && ok "session cookie set" || ko "session cookie"
curl -s -D "$T/hdr" -o /dev/null -H "X-Requested-With: rsos" -d pin=$PIN "$URL/api/login"
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
# the same save again (a retried upload): nothing replaced, the .bak kept
expect "save sent again" 201 "$(upload 'target=saves&sys=snes&path=Zelda.srm&overwrite=1' "$T/b.sfc")"
cmp -s "$T/a.sfc" "$ROOT/saves/snes/Zelda.srm.bak" && [ ! -e "$ROOT/saves/snes/Zelda.srm.bak2" ] &&
	ok "same save again: .bak unchanged" || ko "same save again pushed the .bak out"
# another one: three generations, the first save is still there
printf 'third save!!' > "$T/s3.srm"
expect "save replaced again" 201 "$(upload 'target=saves&sys=snes&path=Zelda.srm&overwrite=1' "$T/s3.srm")"
cmp -s "$T/s3.srm" "$ROOT/saves/snes/Zelda.srm" && cmp -s "$T/b.sfc" "$ROOT/saves/snes/Zelda.srm.bak" &&
	cmp -s "$T/a.sfc" "$ROOT/saves/snes/Zelda.srm.bak2" && ok "older save kept as .bak2" || ko "save .bak2"
expect "state upload" 201 "$(upload 'target=states&sys=snes&path=Zelda.state1' "$T/a.sfc")"
expect "state replace" 201 "$(upload 'target=states&sys=snes&path=Zelda.state1&overwrite=1' "$T/b.sfc")"
cmp -s "$T/a.sfc" "$ROOT/states/snes/Zelda.state1.bak" && ok "old state kept as .bak" || ko "state .bak"
# a deleted save takes its backups along (the game would load the .bak back)
expect "delete a save" 200 "$(acode -X DELETE "$URL/api/file?target=saves&sys=snes&path=Zelda.srm")"
[ -z "$(ls "$ROOT/saves/snes/" | grep Zelda)" ] && ok "save and its .bak files deleted" ||
	ko "left: $(ls "$ROOT/saves/snes/")"
expect "delete a state" 200 "$(acode -X DELETE "$URL/api/file?target=states&sys=snes&path=Zelda.state1")"
[ -z "$(ls "$ROOT/states/snes/")" ] && ok "state and its .bak deleted" || ko "left: $(ls "$ROOT/states/snes/")"
printf 'rom' > "$T/keep.sfc"
expect "rom upload" 201 "$(upload 'target=roms&sys=snes&path=Keep.sfc' "$T/keep.sfc")"
cp "$T/keep.sfc" "$ROOT/roms/snes/Keep.sfc.bak"
expect "delete a rom" 200 "$(acode -X DELETE "$URL/api/file?target=roms&sys=snes&path=Keep.sfc")"
[ -e "$ROOT/roms/snes/Keep.sfc.bak" ] && ok "a game's other files untouched" || ko "rom .bak deleted"
rm -f "$ROOT/roms/snes/Keep.sfc.bak"
printf 'polish' > "$T/pl.gb"
expect "Polish and CJK names (review: refused)" 201 \
	"$(upload 'target=roms&sys=gb&path=%C5%BCaba/%E4%B8%80%E4%BA%8C.gb' "$T/pl.gb")"
cmp -s "$T/pl.gb" "$ROOT/roms/gb/żaba/一二.gb" && ok "żaba/一二.gb written" || ko "żaba/一二.gb"
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
	curl -s -o /dev/null -c "$T/jar2" -H "X-Requested-With: rsos" -d pin=135790 "http://127.0.0.1:$P2/api/login"
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
	lcode -d pin=11111$n "$URL/api/login" > /dev/null
done
expect "6th try, even with the right PIN" 429 "$(lcode -d pin=$PIN "$URL/api/login")"
grep -q '"wait":30' "$T/body" && ok "lockout 30 s" || ko "lockout time: $(cat "$T/body")"

echo "== stop"
kill -INT $SRV
wait $SRV 2> /dev/null
tail -n 2 "$T/server.log" | sed 's/^/       /'
grep -q '^changed:.*snes' "$T/server.log" && ok "changed systems reported" || ko "changed systems"

echo "$PASS passed, $FAIL failed"
[ $FAIL = 0 ]
