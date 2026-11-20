#!/bin/sh
# Tests of rsos-store as the menu uses it (make check-update), against a local HTTP server:
#   signed catalog (good, tampered, wrong key), download + SHA-256 + size checks, rom / tar / native
#   installs, update + remove, files the store does not own, hostile archives, offline cache.
#   store_test.sh <rsos-store> <rsos-mkupdate> <work dir> <make-catalog.py>
set -u
ST=$1
MK=$2
W=$3
MC=$4
rm -rf "$W" && mkdir -p "$W/e" "$W/out"
pass=0
fail=0
check() { if eval "$2"; then pass=$((pass + 1)); else echo "  FAIL: $1"; fail=$((fail + 1)); fi; }

PORT=$((20000 + $$ % 20000))
export PORT W
BASE=http://127.0.0.1:$PORT
"$MK" keygen -s "$W/store.key" -p "$W/store.pub" -c "test store" > /dev/null
"$MK" keygen -s "$W/other.key" -p "$W/other.pub" > /dev/null

# --- content (payloads sit next to the entry files)
printf 'NES\032 pretend ROM v1' > "$W/e/mage.nes"
mkdir -p "$W/g/data"
printf '#!/bin/sh\necho hi\n' > "$W/g/Cool Game.sh"
printf 'level data' > "$W/g/data/level1.bin"
(cd "$W/g" && tar --format=ustar -cf ../e/cool.tar "Cool Game.sh" data)
(cd "$W/g" && tar --format=ustar -cf ../e/pack.tar data)
python3 - <<'EOF'
import json, os
W = os.environ["W"]
def entry(name, **kw):
    d = dict(id=name, name=name, system="nes", version="1.0", author="A", license="CC0", source="https://x/",
             desc="d", kind="rom")
    d.update(kw)
    json.dump(d, open(f"{W}/e/{name}.json", "w"))
entry("mage", file="Micro Mage.nes", payload="mage.nes")
entry("coolgame", system="native", kind="native", launcher="Cool Game.sh", arch="armhf", payload="cool.tar")
entry("wrongarch", system="native", kind="native", launcher="x.sh", arch="x86_64", payload="cool.tar")
entry("pack", system="snes", kind="tar", payload="pack.tar")
EOF

# --- a fake N64 dump (z64 order) in the three formats, and port packages
python3 - <<'EOF'
import hashlib, json, os, random
W = os.environ["W"]
random.seed(7)
z = bytes([0x80, 0x37, 0x12, 0x40]) + bytes(random.getrandbits(8) for _ in range(1048572))
open(f"{W}/game.z64", "wb").write(z)
v = bytearray(z)
v[0::2], v[1::2] = z[1::2], z[0::2]
open(f"{W}/game.v64", "wb").write(bytes(v))
n = bytearray(z)
for i in range(0, len(n), 4):
    n[i:i + 4] = z[i:i + 4][::-1]
open(f"{W}/game.n64", "wb").write(bytes(n))
open(f"{W}/rom.sha1", "w").write(hashlib.sha1(z).hexdigest())
EOF
SHA1=$(cat "$W/rom.sha1")
mkdir -p "$W/pk/ok/install/portgame" "$W/pk/ok/extract" "$W/pk/fail/install" "$W/pk/fail/extract" "$W/pk/slow/install" "$W/pk/slow/extract" \
	"$W/pk/copy/install"
printf '#!/bin/sh\necho port\n' > "$W/pk/ok/install/Port Game.sh"
printf 'program' > "$W/pk/ok/install/portgame/prog.bin"
cat > "$W/pk/ok/extract/extract.sh" <<'EOT'
#!/bin/sh
# the ROM comes in z64 order; TMPDIR is a folder of ours
[ -f "$RSOS_ROM" ] && [ -d "$TMPDIR" ] && [ -d "$RSOS_OUT" ] || exit 9
echo "progress	done=1	total=2"
cp "$RSOS_ROM" "$RSOS_OUT/game.o2r"
echo "progress	done=2	total=2"
EOT
printf '#!/bin/sh\n' > "$W/pk/fail/install/Fail Game.sh"
printf '#!/bin/sh\nexit 3\n' > "$W/pk/fail/extract/extract.sh"
printf '#!/bin/sh\n' > "$W/pk/slow/install/Slow Game.sh"
printf '#!/bin/sh\nsleep 30\n' > "$W/pk/slow/extract/extract.sh"
printf '#!/bin/sh\n' > "$W/pk/copy/install/Copy Game.sh"
for k in ok fail slow copy; do (cd "$W/pk/$k" && tar --format=ustar -cf "$W/e/$k-port.tar" *); done
python3 - <<'EOF'
import json, os
W = os.environ["W"]
sha = open(f"{W}/rom.sha1").read()
def port(name, tar, rom):
    d = dict(id=name, name=name, system="native", version="1.0", author="A", license="CC0", source="https://x/",
             desc="a port", kind="port", arch="armhf", payload=tar, rom=rom)
    json.dump(d, open(f"{W}/e/{name}.json", "w"))
base = dict(name="Test Game (US)", sha1=[sha], size=[1048576], mode="extract", dest="portgame", output=["game.o2r"],
            needs_ram_mb=1, est_minutes=1, needs_disk_mb=1)
port("goport", "ok-port.tar", base)
port("failport", "fail-port.tar", dict(base, dest="failgame"))
port("slowport", "slow-port.tar", dict(base, dest="slowgame"))
port("copyport", "copy-port.tar", dict(base, mode="copy", dest="copygame", copy_as="data/rom.z64", output=[]))
import tarfile
with tarfile.open(f"{W}/e/ok-port.tar.zst", "w:zst", format=tarfile.USTAR_FORMAT) as t:
    for n in ("install", "extract"):
        t.add(f"{W}/pk/ok/{n}", arcname=n)
port("zstdport", "ok-port.tar.zst", dict(base, dest="zstdgame"))
EOF
python3 "$MC" --entries "$W/e" --base-url "$BASE" -o "$W/out" --key "$W/store.key" --mkupdate "$MK" > /dev/null
check "make-catalog signs" "$MK verifyfile -p $W/store.pub -x $W/out/catalog.json.sig $W/out/catalog.json > /dev/null"
check "verifyfile: another key refused" "! $MK verifyfile -p $W/other.pub -x $W/out/catalog.json.sig $W/out/catalog.json 2> /dev/null"

# an entry without a licence is refused by the catalog tool
mkdir -p "$W/bad"
cp "$W/e/mage.nes" "$W/bad/"
python3 - <<'EOF'
import json, os
W = os.environ["W"]
d = json.load(open(f"{W}/e/mage.json")); d["id"] = "nolicense"; d["license"] = ""
json.dump(d, open(f"{W}/bad/x.json", "w"))
EOF
check "catalog tool: no licence refused" "! python3 $MC --entries $W/bad --base-url $BASE -o $W/badout > /dev/null 2>&1"

# --- server
(cd "$W/out" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 > "$W/server.log" 2>&1) &
SRV=$!
trap 'kill $SRV 2> /dev/null' EXIT
sleep 1

S="$ST --allow-http --pubkey $W/store.pub --url $BASE/catalog.json --roms $W/roms --state $W/state --cover-dir $W/covers"
OUT=$($S list)
check "list: entries" "echo \"\$OUT\" | grep -q '^entry.id=mage'"
check "list: native armhf shown" "echo \"\$OUT\" | grep -q 'id=coolgame'"
check "list: other CPU hidden" "! echo \"\$OUT\" | grep -q 'id=wrongarch'"
check "list: source net" "echo \"\$OUT\" | grep -q '^done.count=8.source=net'"
check "list by system" "[ \"\$($S list snes | grep -c '^entry')\" = 1 ]"

# rom
OUT=$($S get mage)
check "get rom ok" "echo \"\$OUT\" | grep -q '^ok.id=mage'"
check "rom placed" "cmp -s '$W/roms/nes/Micro Mage.nes' $W/e/mage.nes"
check "installed" "$S installed | grep -q 'id=mage.version=1.0'"
check "list marks installed" "$S list | grep -q 'id=mage.*installed=1.0.update=0'"
check "no staging left" "[ -z \"\$(ls -A $W/roms | grep '^\\.store')\" ]"

# tar + native
$S get pack > /dev/null
check "tar unpacked" "[ -f $W/roms/snes/data/level1.bin ]"
$S get coolgame > /dev/null
check "native placed" "[ -f '$W/roms/native/Cool Game.sh' ] && [ -f $W/roms/native/data/level1.bin ]"
check "native: record lists top-level paths" "[ \"\$(grep -c '^coolgame' $W/roms/.rsos-store/installed.tsv)\" = 2 ]"
check "a roms folder that is not the console's: its record is in it, not in the state" "[ ! -e $W/state/installed.tsv ]"

# remove
OUT=$($S remove coolgame)
check "remove ok" "echo \"\$OUT\" | grep -q '^ok.id=coolgame.removed=2'"
check "remove deletes files" "[ ! -e '$W/roms/native/Cool Game.sh' ] && [ ! -e $W/roms/native/data ]"
check "remove unknown" "! $S remove coolgame > /dev/null"
check "remove keeps the others" "[ -f '$W/roms/nes/Micro Mage.nes' ]"

# not ours: refuse to overwrite
mkdir -p "$W/roms/native" && printf 'mine' > "$W/roms/native/Cool Game.sh"
OUT=$($S get coolgame)
check "foreign file: refused" "echo \"\$OUT\" | grep -q '^error.code=exists'"
check "foreign file untouched" "[ \"\$(cat '$W/roms/native/Cool Game.sh')\" = mine ]"
rm -f "$W/roms/native/Cool Game.sh"

# wrong bytes on the server: hash / size
cp "$W/out/files/mage.nes" "$W/mage.good"
printf 'NES\032 pretend ROM v9' > "$W/out/files/mage.nes"
OUT=$($S get mage)
check "wrong bytes refused" "echo \"\$OUT\" | grep -q '^error.code=\\(hash\\|download\\)'"
check "old file kept" "cmp -s '$W/roms/nes/Micro Mage.nes' $W/e/mage.nes"
head -c 100 /dev/zero > "$W/out/files/mage.nes"
OUT=$($S get mage)
check "wrong size refused" "echo \"\$OUT\" | grep -q '^error.code=download'"
cp "$W/mage.good" "$W/out/files/mage.nes"
check "nothing left in the games folder" "[ -z \"\$(ls -A $W/roms | grep '^\\.store')\" ]"

# update: a new version replaces the old
python3 - <<'EOF'
import json, os, hashlib
W = os.environ["W"]
c = json.load(open(f"{W}/out/catalog.json"))
data = b"NES\x1a pretend ROM v2 longer"
open(f"{W}/out/files/mage.nes", "wb").write(data)
for e in c["entries"]:
    if e["id"] == "mage":
        e.update(version="1.1", size=len(data), unpacked=len(data), sha256=hashlib.sha256(data).hexdigest())
json.dump(c, open(f"{W}/out/catalog.json", "w"), sort_keys=True, indent=1)
EOF
"$MK" signfile -s "$W/store.key" -o "$W/out/catalog.json.sig" "$W/out/catalog.json" > /dev/null
check "update flagged" "$S list | grep -q 'id=mage.*installed=1.0.update=1'"
$S get mage > /dev/null
check "update installed" "$S installed | grep -q 'id=mage.version=1.1' && grep -q 'longer' '$W/roms/nes/Micro Mage.nes'"


# --- ports made from the player's ROM
mkdir -p "$W/roms/n64" "$W/proms/n64"
cp "$W/game.v64" "$W/proms/n64/Test Game.v64"
head -c 1048576 /dev/zero > "$W/proms/n64/Other.z64"
printf 'NOTAROM' > "$W/proms/n64/Short.z64"
cp "$W/game.n64" "$W/proms/n64/readme.txt"
P="$ST --allow-http --pubkey $W/store.pub --url $BASE/catalog.json --roms $W/proms --state $W/pstate"
OUT=$($P find-rom goport)
check "find-rom: the v64 dump is found by its z64 SHA-1" "echo \"\$OUT\" | grep -q '^rom.path=.*Test Game.v64.sha1='\"$SHA1\""
check "find-rom: the hash is remembered" "[ -s $W/pstate/romhash.tsv ]"
OUT=$($ST --allow-http --pubkey $W/store.pub --url $BASE/catalog.json --roms $W/roms --state $W/pstate find-rom goport)
check "find-rom: no ROM" "echo \"\$OUT\" | grep -q '^error.code=norom'"
mkdir -p "$W/usb/n64" && cp "$W/game.n64" "$W/usb/n64/Dump.n64"
OUT=$($ST --allow-http --pubkey $W/store.pub --url $BASE/catalog.json --roms $W/roms --scan $W/usb --state $W/pstate find-rom goport)
check "find-rom: a scanned drive, n64 byte order" "echo \"\$OUT\" | grep -q '^rom.path=.*Dump.n64'"

OUT=$($P install-port goport)
check "install-port ok" "echo \"\$OUT\" | grep -q '^ok.id=goport'"
check "port: program files placed" "[ -f '$W/proms/native/Port Game.sh' ] && [ -f $W/proms/native/portgame/prog.bin ]"
check "port: the made archive is in the port's folder (ROM given in z64 order)" "cmp -s $W/proms/native/portgame/game.o2r $W/game.z64"
check "port: progress from the extractor" "echo \"\$OUT\" | grep -q '^progress.phase=extract.done=2.total=2'"
check "port: no work folder left" "[ -z \"\$(ls -A $W/proms | grep '^\\.store')\" ]"
check "port: listed as installed" "$P list | grep -q 'id=goport.*kind=port.*installed=1.0'"
printf 'savegame' > $W/proms/native/portgame/save.dat
OUT=$($P remove goport)
check "port remove ok" "echo \"\$OUT\" | grep -q '^ok.id=goport'"
check "port remove: store files gone, the game's own save stays" "[ ! -e '$W/proms/native/Port Game.sh' ] && [ ! -e $W/proms/native/portgame/game.o2r ] && [ -f $W/proms/native/portgame/save.dat ]"
rm -rf $W/proms/native

OUT=$($P install-port failport)
check "failing extractor: error" "echo \"\$OUT\" | grep -q '^error.code=extract'"
check "failing extractor: nothing installed, no work folder" "[ ! -e '$W/proms/native/Fail Game.sh' ] && [ -z \"\$(ls -A $W/proms | grep '^\\.store')\" ]"
printf 'MemTotal: 1000000 kB\nMemAvailable: 500 kB\n' > "$W/lowmem.txt"
OUT=$(RSOS_MEMINFO=$W/lowmem.txt $P install-port goport)
check "not enough free memory: asked to restart" "echo \"\$OUT\" | grep -q '^error.code=memory'"
OUT=$($P --rom "$W/proms/n64/Other.z64" install-port goport)
check "a wrong ROM given: refused" "echo \"\$OUT\" | grep -q '^error.code=norom'"

T0=$(date +%s)
OUT=$(RSOS_MEMFLOOR_KB=999999999 $P install-port slowport)
T1=$(date +%s)
check "memory floor: the extractor is killed" "echo \"\$OUT\" | grep -q '^error.code=memory'"
check "memory floor: quickly, nothing left" "[ $((T1 - T0)) -lt 10 ] && [ ! -e '$W/proms/native/Slow Game.sh' ] && [ -z \"\$(ls -A $W/proms | grep '^\\.store')\" ]"
check "the killed extractor is gone" "! pgrep -f 'sleep 30' > /dev/null"
OUT=$(RSOS_TIMEOUT_S=2 $P install-port slowport)
check "timeout: the extractor is killed" "echo \"\$OUT\" | grep -q '^error.code=timeout'"

OUT=$($P install-port copyport)
check "copy mode: the ROM goes into the port's folder in z64 order" "echo \"\$OUT\" | grep -q '^ok.id=copyport' && cmp -s $W/proms/native/copygame/data/rom.z64 $W/game.z64"
$P remove copyport > /dev/null
mkdir -p $W/proms/native && printf 'mine' > "$W/proms/native/Copy Game.sh"
OUT=$($P install-port copyport)
check "port: a file that is not the store's is moved aside, then the port installs" "echo \"\$OUT\" | grep -q '^ok.id=copyport' && [ \"\$(cat '$W/proms/native/.store-replaced/copyport/Copy Game.sh')\" = mine ] && [ \"\$(cat '$W/proms/native/Copy Game.sh')\" != mine ]"
# removed by the store (the game's save stays in its folder), then installed again
$P remove copyport > /dev/null
mkdir -p $W/proms/native/copygame && printf 'old' > $W/proms/native/copygame/save.dat
OUT=$($P install-port copyport)
check "port: installs again over what a delete left, its save kept" "echo \"\$OUT\" | grep -q '^ok.id=copyport' && [ -f '$W/proms/native/Copy Game.sh' ] && [ \"\$(cat $W/proms/native/copygame/save.dat)\" = old ]"
# the menu's "Delete this game" on the launcher: the whole port goes, the save and other games stay
mkdir -p $W/proms/native/.store-replaced/copyport && printf 'mine' > "$W/proms/native/.store-replaced/copyport/Copy Game.sh"
printf 'by hand' > "$W/proms/native/Hand Game.sh"
OUT=$($P remove-path "$W/proms/native/Copy Game.sh")
check "remove-path: the launcher's port is removed" "echo \"\$OUT\" | grep -q '^ok.id=copyport' && [ ! -e '$W/proms/native/Copy Game.sh' ] && [ ! -e $W/proms/native/copygame/data/rom.z64 ]"
check "remove-path: the save stays, the moved-aside copy goes" "[ \"\$(cat $W/proms/native/copygame/save.dat)\" = old ] && [ ! -e $W/proms/native/.store-replaced ]"
check "remove-path: no longer installed" "! $P installed | grep -q 'id=copyport'"
OUT=$($P remove-path "native/Hand Game.sh")
check "remove-path: a game not from the store is left alone" "echo \"\$OUT\" | grep -q '^error.code=notstore' && [ -f '$W/proms/native/Hand Game.sh' ]"
OUT=$($P remove-path "../outside.sh")
check "remove-path: a path outside the roms folder" "echo \"\$OUT\" | grep -q '^error.code=notstore'"
rm -rf $W/proms/native


OUT=$($P install-port zstdport)
check "a zstd-compressed port package installs" "echo \"\$OUT\" | grep -q '^ok.id=zstdport' && cmp -s $W/proms/native/zstdgame/game.o2r $W/game.z64"
check "zstd: no temporary tar left" "[ -z \"\$(ls -A $W/proms | grep '^\.store')\" ]"
$P remove zstdport > /dev/null
rm -rf $W/proms/native


# the player's own catalog: <state>/store.ini
mkdir -p "$W/inistate" && printf 'url=%s/catalog.json
allow_http=1
' "$BASE" > "$W/inistate/store.ini"
OUT=$($ST --pubkey $W/store.pub --state $W/inistate --roms $W/roms list)
check "store.ini: the catalog URL and allow_http are used" "echo \"\$OUT\" | grep -q '^done.count=8.source=net'"
OUT=$($ST --pubkey $W/store.pub --state $W/inistate --roms $W/roms --url http://127.0.0.1:1/x.json list)
check "store.ini: an option wins over it (the dead URL falls back to the saved catalog)" "echo \"\$OUT\" | grep -q 'source=cache'"

# tampered / wrongly signed catalog
cp "$W/out/catalog.json" "$W/cat.good"
sed 's/"version": "1.1"/"version": "9.9"/' "$W/cat.good" > "$W/out/catalog.json"
OUT=$($ST --allow-http --pubkey $W/store.pub --url $BASE/catalog.json --state $W/state2 list)
check "tampered catalog refused" "echo \"\$OUT\" | grep -q '^error.code=catalog'"
cp "$W/cat.good" "$W/out/catalog.json"
OUT=$($ST --allow-http --pubkey $W/other.pub --url $BASE/catalog.json --state $W/state3 list)
check "catalog from another key refused" "echo \"\$OUT\" | grep -q '^error.code=catalog'"
check "refused catalog not cached" "[ ! -e $W/state3/catalog.json ]"

# hostile archives (hand-made signed catalog)
python3 - <<'EOF'
import json, os, tarfile, io, hashlib
W, PORT = os.environ["W"], os.environ["PORT"]
def tarbytes(members):
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, data, typ in members:
            ti = tarfile.TarInfo(name)
            ti.type = typ
            if typ == tarfile.REGTYPE:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
            else:
                ti.linkname = "/etc/passwd"
                t.addfile(ti)
    return b.getvalue()
evil = {"dots": [("../evil.txt", b"x", tarfile.REGTYPE)],
        "abs": [("/tmp/evil.txt", b"x", tarfile.REGTYPE)],
        "link": [("ln", b"", tarfile.SYMTYPE)],
        "bomb": [("big.bin", b"\0" * 100000, tarfile.REGTYPE)]}
entries = []
for k, m in evil.items():
    data = tarbytes(m)
    open(f"{W}/out/files/{k}.tar", "wb").write(data)
    entries.append(dict(id=k, name=k, system="snes", version="1", author="a", license="x", source="x", desc="d",
                        kind="tar", size=len(data), unpacked=2000 if k == "bomb" else len(data) + 10,
                        sha256=hashlib.sha256(data).hexdigest(), url=f"http://127.0.0.1:{PORT}/files/{k}.tar"))
json.dump({"format": 1, "entries": entries}, open(f"{W}/out/catalog.json", "w"))
EOF
"$MK" signfile -s "$W/store.key" -o "$W/out/catalog.json.sig" "$W/out/catalog.json" > /dev/null
for k in dots abs link bomb; do
	OUT=$($S get $k)
	check "hostile archive $k refused" "echo \"\$OUT\" | grep -q '^error.code=archive'"
done
check "nothing escaped" "[ ! -e $W/evil.txt ] && [ ! -e /tmp/evil.txt ] && [ ! -e $W/roms/snes/big.bin ] && [ ! -e $W/roms/snes/ln ]"
check "staging cleaned" "[ -z \"\$(ls -A $W/roms | grep '^\\.store')\" ]"

# offline: the last verified catalog (state) is used
kill $SRV 2> /dev/null
sleep 1
OUT=$($S list)
check "offline: cached catalog" "echo \"\$OUT\" | grep -q '^done.count=4.source=cache'"
OUT=$($ST --allow-http --pubkey $W/store.pub --url $BASE/catalog.json --state $W/state9 list)
check "offline without a cache: error" "echo \"\$OUT\" | grep -q '^error.code=catalog'"

# local catalog file mode
OUT=$($ST --pubkey $W/store.pub --catalog $W/out/catalog.json --state $W/state8 list)
check "local catalog file" "echo \"\$OUT\" | grep -q '^done.count=4'"

echo "store_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
