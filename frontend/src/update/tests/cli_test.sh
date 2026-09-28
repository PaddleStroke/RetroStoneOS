#!/bin/sh
# Tests of the two programs as they are used (make check-update):
#   rsos-mkupdate: keygen, pack (signed and unsigned), sign, verify, info;
#     the keys and signatures against OpenBSD signify when it is installed
#     (Ubuntu: signify-openbsd);
#   rsos-update: info / check / verify / apply / boot / status on a fake
#     system tree (--root, a file as the inactive slot, fake fw_printenv /
#     fw_setenv on a text file), in --machine mode as the menu reads it.
#   cli_test.sh <rsos-update> <rsos-mkupdate> <work dir>
set -u
UPD=$1
MK=$2
W=$3
rm -rf "$W" && mkdir -p "$W"
pass=0
fail=0
T=$(printf '\t')
check() { if eval "$2"; then pass=$((pass + 1)); else echo "  FAIL: $1"; fail=$((fail + 1)); fi; }

echo "== rsos-mkupdate"
"$MK" keygen -s "$W/test.key" -p "$W/test.pub" -c "test" > /dev/null
check "keygen" "[ -s $W/test.key ] && [ -s $W/test.pub ] && [ \"\$(stat -c %a $W/test.key)\" = 600 ]"
check "keygen never overwrites" "! $MK keygen -s $W/test.key -p $W/x.pub 2> /dev/null"
"$MK" keygen -s "$W/other.key" -p "$W/other.pub" > /dev/null
check "pubkey from the secret key" "[ \"\$($MK pubkey -s $W/test.key | tail -n 1)\" = \"\$(tail -n 1 $W/test.pub)\" ]"
check "the key from an environment variable" \
	"[ \"\$(K=\"\$(cat $W/test.key)\" $MK pubkey --key-env K | tail -n 1)\" = \"\$(tail -n 1 $W/test.pub)\" ]"

# a 4 MiB "root file system": data and empty blocks
{ head -c 1048576 /dev/urandom; head -c 2097152 /dev/zero; head -c 1048576 /dev/urandom; } > "$W/rootfs.ext4"
if command -v zstd > /dev/null; then
	zstd -q -19 -c "$W/rootfs.ext4" > "$W/payload"
	COMP=zstd
else
	echo "  (no zstd program: uncompressed payload)"
	cp "$W/rootfs.ext4" "$W/payload"
	COMP=none
fi
printf -- '- first change\n- second: "quoted" \\ back\n' > "$W/changes.txt"
PACK="--image $W/rootfs.ext4 --payload $W/payload --compression $COMP --board testboard --build-time 1790000000"
# ($PACK: several words, on purpose)
# shellcheck disable=SC2086
"$MK" pack -o "$W/signed.rsu" $PACK --version 0.2.0 --changelog-file "$W/changes.txt" -s "$W/test.key" > /dev/null
# shellcheck disable=SC2086
"$MK" pack -o "$W/unsigned.rsu" $PACK --version 0.2.1 > /dev/null
check "pack signed" "$MK verify -p $W/test.pub $W/signed.rsu > /dev/null"
check "another key: refused" "! $MK verify -p $W/other.pub $W/signed.rsu 2> /dev/null"
check "unsigned: refused by verify" "! $MK verify -p $W/test.pub $W/unsigned.rsu 2> /dev/null"
check "a plain tar" "[ \"\$(tar tf $W/signed.rsu | tr '\n' ' ')\" = \"manifest manifest.sig rootfs.ext4$([ $COMP = zstd ] && echo .zst) \" ]"
check "info" "$MK info $W/signed.rsu | grep -q '^changelog = - first change\\\\n- second: \"quoted\" \\\\\\\\ back$'"
"$MK" sign -s "$W/test.key" -o "$W/resigned.rsu" "$W/unsigned.rsu" > /dev/null
check "sign an unsigned package" "$MK verify -p $W/test.pub $W/resigned.rsu > /dev/null"
cp "$W/signed.rsu" "$W/tampered.rsu"
# (byte 100000: inside the payload, after the manifest and the signature)
printf 'X' | dd of="$W/tampered.rsu" bs=1 seek=100000 conv=notrunc status=none
check "tampered payload: refused" "! $MK verify -p $W/test.pub $W/tampered.rsu 2> /dev/null"

if command -v signify-openbsd > /dev/null || command -v signify > /dev/null; then
	S=$(command -v signify-openbsd || command -v signify)
	mkdir -p "$W/sfy" && (cd "$W/sfy" && tar xf ../signed.rsu manifest manifest.sig)
	check "signify verifies our signature" "$S -V -q -p $W/test.pub -x $W/sfy/manifest.sig -m $W/sfy/manifest"
	"$S" -G -n -p "$W/sfy/s.pub" -s "$W/sfy/s.sec" > /dev/null 2>&1
	# shellcheck disable=SC2086
	"$MK" pack -o "$W/sfy.rsu" $PACK --version 0.2.0 -s "$W/sfy/s.sec" > /dev/null
	check "a signify key signs packages" "$MK verify -p $W/sfy/s.pub $W/sfy.rsu > /dev/null"
	(cd "$W/sfy" && tar xf ../sfy.rsu manifest manifest.sig && "$S" -S -s s.sec -m manifest -x ref.sig)
	check "same signature as signify (Ed25519 is deterministic)" "cmp -s $W/sfy/ref.sig $W/sfy/manifest.sig || \
		[ \"\$(tail -n 1 $W/sfy/ref.sig)\" = \"\$(tail -n 1 $W/sfy/manifest.sig)\" ]"
else
	echo "  (signify not installed: interoperability not checked)"
fi

echo "== rsos-update on a fake system"
R=$W/root
mk_system() { # version variant env
	rm -rf "$R"
	mkdir -p "$R/etc/rsos" "$R/proc" "$R/data/update" "$R/run/rsos" "$R/usr/share/rsos" "$R/bin" \
		"$R/sys/class/power_supply/bat" "$R/media/usb0/RetroStoneOS"
	printf "RSOS_VERSION='%s'\nRSOS_VARIANT='%s'\nRSOS_BUILD_TIME='1780000000'\nRSOS_BOARD_ID='testboard'\n" "$1" "$2" \
		> "$R/etc/rsos/version.env"
	echo "ab_update = auto" > "$R/etc/rsos/board.ini"
	echo "/dev/null 0x0 0x10000" > "$R/etc/fw_env.config"
	echo "root=/dev/mmcblk0p2 ro rsos.slot=a rsos.boot=pending" > "$R/proc/cmdline"
	printf '%b' "$3" > "$R/env.txt"
	cp "$W/test.pub" "$R/usr/share/rsos/update.pub"
	echo Battery > "$R/sys/class/power_supply/bat/type"
	echo 75 > "$R/sys/class/power_supply/bat/capacity"
	echo Discharging > "$R/sys/class/power_supply/bat/status"
	cat > "$R/bin/fw_printenv" <<EOF
#!/bin/sh
v=\$(grep "^\$4=" "$R/env.txt" | tail -n 1)
[ -n "\$v" ] && echo "\${v#*=}"
EOF
	cat > "$R/bin/fw_setenv" <<EOF
#!/bin/sh
cp "$R/env.txt" "$R/env.new"
while read -r n v; do
	grep -v "^\$n=" "$R/env.new" > "$R/env.tmp"; mv "$R/env.tmp" "$R/env.new"
	[ -n "\$v" ] && echo "\$n=\$v" >> "$R/env.new"
done < "\$4"
mv "$R/env.new" "$R/env.txt"
EOF
	chmod +x "$R/bin/fw_printenv" "$R/bin/fw_setenv"
	head -c 6291456 /dev/zero | tr '\0' 'U' > "$R/slot-b.img"
}
U() { "$UPD" --root "$R" --fw-printenv "$R/bin/fw_printenv" --fw-setenv "$R/bin/fw_setenv" \
	--target "$R/slot-b.img" --log "$W/update.log" "$@"; }
ENV_A='rsos_slot=a\nrsos_ok=1\nrsos_tries=0\nrsos_fails=1\n'

mk_system 0.1.0 release "$ENV_A"
cp "$W/signed.rsu" "$R/media/usb0/RetroStoneOS/retrostoneos-0.2.0-testboard.rsu"
cp "$W/unsigned.rsu" "$R/data/update/"
U --machine info > "$W/out"
check "info" "grep -q '^info${T}version=0.1.0${T}board=testboard${T}variant=release${T}build_date=${T}ab=1${T}reason=${T}slot=a${T}key=1' $W/out"
U --machine check --local-only > "$W/out"
check "check: the signed package on the stick, installable" \
	"grep -q '^local${T}status=found${T}source=file${T}version=0.2.0${T}.*installable=1${T}verdict=ok${T}notes=- first change\\\\n' $W/out"
U --machine check --local-only --dir "$R/data/update" > "$W/out"
check "check --dir: the unsigned one only, refused on a release build" \
	"grep -q '^local${T}status=refused${T}.*version=0.2.1${T}.*signed=0${T}installable=0${T}verdict=unsigned' $W/out"
check "verify --full" "U verify --full $R/media/usb0/RetroStoneOS/retrostoneos-0.2.0-testboard.rsu 2> /dev/null | grep -q 'OK.*newer.*image checked'"
echo 15 > "$R/sys/class/power_supply/bat/capacity"
U --machine apply "$R/media/usb0/RetroStoneOS/retrostoneos-0.2.0-testboard.rsu" > "$W/out"
check "battery 15 %: refused" "grep -q '^error${T}code=battery' $W/out && grep -q '^rsos_slot=a$' $R/env.txt"
echo 75 > "$R/sys/class/power_supply/bat/capacity"
U --machine apply "$R/data/update/unsigned.rsu" > "$W/out"
check "unsigned on a release build: refused even with --allow-unsigned" \
	"U --machine --allow-unsigned apply $R/data/update/unsigned.rsu | grep -q '^error${T}code=unsigned' && grep -q '^error${T}code=unsigned' $W/out"
U --machine apply "$R/media/usb0/RetroStoneOS/retrostoneos-0.2.0-testboard.rsu" > "$W/out"; rc=$?
check "apply: progress lines, then done" "[ $rc = 0 ] && grep -q '^progress${T}phase=verify' $W/out && \
	grep -q '^progress${T}phase=write${T}done=' $W/out && grep -q '^progress${T}phase=readback' $W/out && \
	grep -q '^done${T}version=0.2.0${T}slot=b$' $W/out"
check "the slot holds the image" "cmp -s -n 4194304 $W/rootfs.ext4 $R/slot-b.img"
check "the environment: slot b on trial" "grep -q '^rsos_slot=b$' $R/env.txt && grep -q '^rsos_ok=0$' $R/env.txt && \
	grep -q '^rsos_tries=3$' $R/env.txt && grep -q '^rsos_fails=0$' $R/env.txt"
check "boot before the restart: pending" "U --machine boot | grep -q '^boot${T}event=pending${T}version=0.2.0$'"
check "a second apply: restart first" "U --machine apply $R/media/usb0/RetroStoneOS/retrostoneos-0.2.0-testboard.rsu | grep -q '^error${T}code=restart'"
check "status" "U status 2> /dev/null | grep -q '^  version=0.2.0$'"
# the restart into slot b
echo "root=/dev/mmcblk0p3 ro rsos.slot=b rsos.boot=pending" > "$R/proc/cmdline"
sed -i "s/RSOS_VERSION='0.1.0'/RSOS_VERSION='0.2.0'/; s/RSOS_BUILD_TIME='1780000000'/RSOS_BUILD_TIME='1790000000'/" \
	"$R/etc/rsos/version.env"
check "first boot of 0.2.0: updated" "U --machine boot | grep -q '^boot${T}event=updated${T}version=0.2.0$'"
echo confirmed > "$R/run/rsos/boot-state"
printf 'rsos_slot=b\nrsos_ok=1\nrsos_tries=0\nrsos_fails=0\n' > "$R/env.txt"
check "confirmed: clean-up" "U --machine boot | grep -q '^boot${T}event=confirmed' && [ ! -e $R/data/rsos/update-state.ini ]"
check "the user's package on the stick is kept" "[ -e $R/media/usb0/RetroStoneOS/retrostoneos-0.2.0-testboard.rsu ]"
check "the stick's package is now installed (same)" \
	"U --machine check --local-only --dir $R/media/usb0 | grep -q '^local${T}status=refused${T}.*verdict=same'"
check "the newer unsigned one is the reason given for the whole card" \
	"U --machine check --local-only | grep -q '^local${T}status=refused${T}.*version=0.2.1${T}.*verdict=unsigned'"

echo "== development build: unsigned packages with --allow-unsigned"
mk_system 0.1-dev dev "$ENV_A"
cp "$W/unsigned.rsu" "$R/data/update/"
U --machine check --local-only > "$W/out"
check "listed on a development build (signed=0, installable)" \
	"grep -q '^local${T}status=found${T}.*version=0.2.1${T}.*signed=0${T}installable=1' $W/out"
check "not installed without --allow-unsigned" "U --machine apply $R/data/update/unsigned.rsu | grep -q '^error${T}code=unsigned'"
check "installed with --allow-unsigned" "U --machine --allow-unsigned apply $R/data/update/unsigned.rsu | grep -q '^done${T}version=0.2.1'"

echo "== the log"
check "every run logged" "grep -q 'installed 0.2.0 in slot b' $W/update.log"

echo "cli_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
