#!/bin/sh
# b2-ui.sh UIPREVIEW WORKDIR
#
# Batch 2 in the preview tool (run from frontend/): jump to letter (L1/R1),
# the search of a list (live, cancel, B clears it), "Search all games", the
# new settings (Resume on boot, fast-forward speed, vibration, sort order),
# the Windows file share next to the network transfer (a fake helper), and
# the game switcher's list order (ui_recent_games through the preview's
# launches).
set -u
UIP=$1
W=$2
fails=0

ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fails=$((fails + 1)); }
t() {
	d=$1
	shift
	if "$@"; then ok "$d"; else bad "$d"; fi
}
pv() { # pv "KEYS" [args]: fails on an unmet expect:
	k=$1
	shift
	"$UIP" --root "$W" --quiet --keys "$k" "$@" > "$W/pv.out" 2>&1
	r=$?
	if grep -q 'EXPECT FAILED' "$W/pv.out"; then
		grep 'EXPECT FAILED' "$W/pv.out" | head -3
		return 1
	fi
	return $r
}

rm -rf "$W"
mkdir -p "$W/data/roms/nes" "$W/data/roms/snes" "$W/data/rsos" "$W/usr/share/rsos/cores"
printf '[core]\nid = c_nes\nsystems = nes\nextensions = nes\n' > "$W/usr/share/rsos/cores/c_nes.ini"
printf '[core]\nid = c_snes\nsystems = snes\nextensions = sfc\n' > "$W/usr/share/rsos/cores/c_snes.ini"
for g in "Adventure Island" "Air Fortress" "Balloon Fight" "Batman" "Bomberman" "Castlevania" "Contra" \
	"Double Dragon" "Duck Tales" "Excitebike" "1942" "Pokémon Puzzle"; do
	printf 'NES' > "$W/data/roms/nes/$g.nes"
done
printf 'SNES' > "$W/data/roms/snes/Super Bomberman.sfc"
SET=$W/data/rsos/settings.ini

echo "b2-ui: jump to letter (L1/R1 in a list sorted by name)"
pv "a wait:500 expect:list:nes expect:game=1942 r expect:letter=A expect:game=Adventure_Island r expect:letter=B \
	expect:game=Balloon_Fight r r expect:game=Double_Dragon l expect:game=Castlevania l l expect:game=Adventure \
	l expect:game=1942 l expect:letter=P expect:game=Pokémon_Puzzle r expect:game=1942 expect:letter=#"
t "#, A, B, C, D and back, wrapping around" [ $? = 0 ]
pv "a wait:500 down down expect:game=Air_Fortress l expect:game=Adventure_Island down r expect:game=Balloon_Fight"
t "from the middle of a letter: L1 = the start of this one, R1 = the next letter" [ $? = 0 ]

echo "b2-ui: search this list (game options > Search this list, the keyboard, live)"
# the keyboard starts on "q" (row 1); b = row 3 col 4, o = row 1 col 8
pv "a wait:500 select wait:300 down expect:|Search_this_list a wait:300 expect:osk:Search_this_list \
	down down right right right right a expect:osk:Search:_5_games|b \
	up up right right right right a expect:osk:Search:_1_game|bo start wait:300 \
	expect:list:nes_search=bo(1)_game=Bomberman b expect:game=Bomberman b expect:carousel"
t "b: 5 games, bo: 1 (live, the keyboard counts them); OK keeps it; B clears it, then leaves" [ $? = 0 ]
pv "a wait:500 select wait:300 down a wait:300 down down right right right right a expect:osk:Search:_5_games|b \
	select wait:300 expect:list:nes_game="
t "SELECT (cancel) puts back the list as it was" [ $? = 0 ]
# p o k e m: left (wraps to p), left (o), down left (k), up then 5 x left (e), down down then 4 x right (m)
pv "a wait:500 select wait:300 down a wait:300 left a left a down left a up left left left left left a \
	down down right right right right a expect:osk:Search:_1_game start wait:300 expect:search=pokem(1) \
	expect:game=Pokémon_Puzzle"
t "accents ignored: pokem finds Pokémon Puzzle" [ $? = 0 ]
pv "a wait:500 select wait:300 down a wait:300 down down a up up a expect:osk:Search:_0_games start wait:300 \
	expect:search=zq(0)"
t "no match: an empty list, the count 0" [ $? = 0 ]

echo "b2-ui: Search all games (Settings)"
pv "start up up expect:|Search_all_games a wait:300 expect:osk:Search_all_games down down right right right right a \
	up up right right right right a start wait:500 expect:list:search_game=Bomberman down expect:game=Super_Bomberman \
	b expect:carousel"
t "bo: Bomberman (NES) and Super Bomberman (SNES) in one list, B back to the carousel" [ $? = 0 ]

echo "b2-ui: settings"
rm -f "$SET"
pv "start down down down down down a wait:300 expect:menu:Games|Auto-save down down expect:|Resume_on_boot left \
	down expect:|Fast-forward_speed right b b"
t "Settings > Games: Resume on boot, Fast-forward speed" sh -c "[ $? = 0 ] && grep -q '^resume_boot *= *always\$' '$SET' && \
	grep -q '^ff_speed *= *4\$' '$SET'"
pv "start down a wait:300 expect:menu:Controls down down down down expect:|Controller_vibration a b b"
t "Settings > Controls > Controller vibration off" sh -c "[ $? = 0 ] && grep -q '^rumble *= *0\$' '$SET'"
pv "start down down down down a wait:300 expect:menu:Game_lists down down expect:|Sort_games_by right wait:500 \
	expect:carousel"
t "Settings > Game lists > Sort games by: Most played" sh -c "[ $? = 0 ] && grep -q '^gamelist_sort *= *playtime\$' '$SET'"
printf 'nes\tDuck Tales.nes\t0\t0\t0\t\t5000\t0\t\t\nnes\tContra.nes\t0\t0\t0\t\t90\t0\t\t\n' > "$W/data/rsos/gamedb.tsv"
pv "a wait:500 expect:game=Duck_Tales down expect:game=Contra down expect:game=1942 down expect:game=Adventure"
t "most played first, then by name" [ $? = 0 ]
rm -f "$SET" "$W/data/rsos/gamedb.tsv"

echo "b2-ui: the Windows file share next to the network transfer (fake helper)"
# where the menu looks for a share left by a crashed menu (not the host's)
export RSOS_SMB_RUN="$W/ksmbd-run" RSOS_SMB_SYSMOD="$W/ksmbd-module"
cat > "$W/fake-smb.sh" <<EOF
#!/bin/sh
echo "\$*" >> "$W/smb.log"
echo on
EOF
chmod +x "$W/fake-smb.sh"
pv "start down down expect:menu:Settings|Network a wait:300 expect:menu:Network|WiFi down down down down down down down \
	down down expect:|Windows_file_share a wait:300 up up up up expect:|Transfer_over_network a wait:1500 \
	shot:$W/web.png b wait:300 b wait:300 b b wait:800" --fake-transfer --smb-helper "$W/fake-smb.sh"
t "the toggle saved, the share started with a password XXXX-XXXX (not the PIN) and the console name" \
	sh -c "[ $? = 0 ] && grep -q '^smb *= *1\$' '$SET' && \
	grep -Eq '^start [A-HJKMNP-Z2-9]{4}-[A-HJKMNP-Z2-9]{4} retrostone\$' '$W/smb.log'"
t "and stopped with it" grep -q '^stop$' "$W/smb.log"
: > "$W/smb.log"
"$UIP" --root "$W" --keys "start down down a wait:300 expect:menu:Network|WiFi down down down down down \
	expect:|Transfer_over_network a wait:1500 a wait:500" --fake-transfer --smb-helper "$W/fake-smb.sh" \
	> "$W/log.out" 2>&1
pass=$(awk '/^start/ { print $2 }' "$W/smb.log")
t "the password is not in the log (review: hw.c logged the job's arguments)" \
	sh -c "[ -n '$pass' ] && grep -q 'job .* started: .*fake-smb.sh start \.\.\.' '$W/log.out' && \
	! grep -q '$pass' '$W/log.out'"
: > "$W/smb.log"
mkdir -p "$RSOS_SMB_RUN"
pv "wait:2500" --fake-transfer --smb-helper "$W/fake-smb.sh"
t "a share left by a crashed menu is stopped when the menu starts" sh -c "[ \"\$(cat '$W/smb.log')\" = stop ]"
rm -rf "$RSOS_SMB_RUN"
: > "$W/smb.log"
pv "wait:2500" --fake-transfer --smb-helper "$W/fake-smb.sh"
t "none left: the helper is not run" [ ! -s "$W/smb.log" ]
# a start that lasts until the screenshot after STOP (the preview's time is
# virtual; a file it writes is the real-time signal), then the screenshots
# give the menu real time to see its end and run the stop
cat > "$W/slow-smb.sh" <<EOF
#!/bin/sh
echo "\$1 begin" >> "$W/smb.log"
i=0
while [ "\$1" = start ] && [ ! -e "$W/go.png" ] && [ \$i -lt 500 ]; do
	sleep 0.01
	i=\$((i + 1))
done
echo "\$1 end" >> "$W/smb.log"
echo on
EOF
chmod +x "$W/slow-smb.sh"
: > "$W/smb.log"
rm -f "$W/go.png"
shots=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	shots="$shots wait:100 shot:$W/later.png"
done
pv "start down down a wait:300 expect:menu:Network|WiFi down down down down down expect:|Transfer_over_network \
	a wait:200 a wait:100 shot:$W/go.png $shots wait:500" --fake-transfer --smb-helper "$W/slow-smb.sh"
r=$?
sleep 0.5
t "STOP while the share starts: the stop runs after the start, not during it" \
	sh -c "[ $r = 0 ] && [ \"\$(tr '\n' , < '$W/smb.log')\" = 'start begin,start end,stop begin,stop end,' ]"
unset RSOS_SMB_RUN RSOS_SMB_SYSMOD
pv "start down down a wait:300 expect:menu:Network|WiFi down down down down down down down down down expect:|WiFi" \
	--fake-transfer
t "no helper installed: the item is not there (the menu wraps to WiFi)" [ $? = 0 ]

echo "b2-ui: review: an export that ends under its \"Stop?\" dialog is finished at once (stick read-only again)"
pv "usbfs:vfat usb wait:300 right expect:|EXPORT_GAMES a wait:1000 expect:menu:Export_games_to_GAMES|Export tap:a \
	tap:b expect:dialog:Stop? wait:8000 expect:dialog:Copied right a wait:300 expect:dialog:Stop? right \
	expect:|CONTINUE a wait:300 expect:carousel" --fake-transfer
t "the summary over the dialog, then the dialog answers nothing" [ $? = 0 ]
t "read-write once, read-only again once, nothing cancelled" sh -c "[ \$(grep -c '^REMOUNT /media/usb0 rw\$' '$W/pv.out') = 1 ] && \
	[ \$(grep -c '^REMOUNT /media/usb0 ro\$' '$W/pv.out') = 1 ]"

echo "b2-ui: review: the update helper's resolver: 2 s per server, one try (RES_OPTIONS)"
cat > "$W/fake-update.sh" <<EOF
#!/bin/sh
echo "\${RES_OPTIONS:-none}|\$*" >> "$W/update-env.log"
printf 'boot\tevent=none\n'
EOF
chmod +x "$W/fake-update.sh"
printf '[update]\nstate = confirm\n' > "$W/data/rsos/update-state.ini"
rm -f "$W/update-env.log"
export RSOS_UPDATE_SYNC=1
pv "wait:1500" --update-helper "$W/fake-update.sh"
unset RSOS_UPDATE_SYNC
t "the helper ran with RES_OPTIONS=timeout:2 attempts:1" grep -q '^timeout:2 attempts:1|--machine boot' "$W/update-env.log"
rm -f "$W/data/rsos/update-state.ini"

if [ $fails = 0 ]; then
	echo "b2-ui: ALL OK"
else
	echo "b2-ui: $fails FAILED"
	exit 1
fi
