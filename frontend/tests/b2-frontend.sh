#!/bin/sh
# b2-frontend.sh FRONTEND TESTCORE WORKDIR LOCALE_DIR BOARD_INI
#
# Batch 2 through the real rsos-frontend main loop, headless (as
# check-frontend): the play time saved in gamedb, the game switcher's
# relaunch (the child asks for another game, the menu launches it resumed),
# Resume on boot (always / never / ask with its 4 choices), per-game scaling
# and CPU profile (game options -> the game's arguments; the in-game menu ->
# gamedb), Hide and Delete (with the saves question, NO by default).
set -u
FE=$1
CORE=$2
W=$3
LOC=$4
BOARD=$5
HERE=$(cd "$(dirname "$0")/.." && pwd)
fails=0

ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fails=$((fails + 1)); }
t() {
	d=$1
	shift
	if "$@"; then ok "$d"; else bad "$d"; fi
}

rm -rf "$W"
mkdir -p "$W/data/roms/nes" "$W/data/rsos" "$W/data/bios" "$W/data/saves/nes" "$W/data/states/nes" \
	"$W/usr/share/rsos/cores" "$W/dev/input" "$W/run/rsos" "$W/tmp" "$W/media" "$W/sys/block" \
	"$W/sys/class/power_supply/axp20x-battery" "$W/sys/class/power_supply/axp20x-ac" "$W/etc/rsos"
cp "$BOARD" "$W/etc/rsos/board.ini"
printf 'language = en\n' > "$W/data/rsos/settings.ini"
for g in "Alpha Quest" "Bravo Run" "Charlie" "Delta"; do
	printf 'NES\032%s' "$g" > "$W/data/roms/nes/$g.nes"
done
printf '[core]\nid = fceumm\ndisplay_name = Test core\nlibrary = %s\nsystems = nes\nextensions = nes\n' \
	"$CORE" > "$W/usr/share/rsos/cores/fceumm.ini"
(cd "$W/sys/class/power_supply/axp20x-battery" && printf '1\n' > present && printf 'Discharging\n' > status &&
	printf '80\n' > capacity && printf '3900000\n' > voltage_now && printf '300000\n' > current_now &&
	printf 'Battery\n' > type)
(cd "$W/sys/class/power_supply/axp20x-ac" && printf '0\n' > online && printf 'Mains\n' > type)
printf 'key\n' > "$W/run/rsos/bootreason"

LOG=$W/run/rsos/frontend.log
GLOG=$W/run/rsos/game.log
DB=$W/data/rsos/gamedb.tsv
SET=$W/data/rsos/settings.ini
RESUME=$W/data/rsos/resume.ini
run() {
	rm -f "$LOG" "$GLOG"
	"$FE" --headless --root "$W" --res "$HERE/third_party" --themes "$HERE/themes" --locale "$LOC" \
		--run-ms 25000 --script "$1" > /dev/null 2>&1
	! grep -q 'EXPECT FAILED' "$LOG" || { grep 'EXPECT FAILED' "$LOG" | head -3; return 1; }
}
setting() { # setting key value
	sed -i "/^$1 *=/d" "$SET"
	printf '%s = %s\n' "$1" "$2" >> "$SET"
}
dbcol() { # dbcol rel column -> the value
	awk -F '\t' -v r="$1" -v c="$2" '$2 == r { print $c }' "$DB"
}

echo "b2-frontend: play time (a slow game of ~2.5 s), saved in gamedb.tsv"
RSOS_TESTCORE=slow run "wait:300 a wait:500 expect:game=Alpha_Quest a wait:3500 expect:list term"
t "the run" [ $? = 0 ]
pt=$(dbcol "Alpha Quest.nes" 7)
t "Alpha Quest: ${pt:-none} s of play in column 7" sh -c "[ ${pt:-0} -ge 2 ] && [ ${pt:-0} -le 5 ]"
t "added as the game reported it" grep -q 'ui: play time Alpha Quest.nes: +[0-9]* s' "$LOG"

echo "b2-frontend: the game switcher: the child picks entry 1, the menu launches it resumed"
setting autosave_exit 0      # the switch saves the game it leaves anyway
RSOS_TEST_GAME_ARGS_ONCE="--switcher-pick 1" run "wait:300 a wait:500 down expect:game=Bravo_Run a wait:2500 \
	expect:list term"
t "the run" [ $? = 0 ]
t "the switcher file listed the recent games" grep -q 'Alpha Quest' "$W/run/rsos/switcher.tsv"
t "Bravo Run ended with the switch (exit 6)" grep -q 'game ended: status 6, exit 6' "$LOG"
t "Bravo Run saved when left (auto-save on exit is off)" [ -s "$W/data/states/nes/Bravo Run.state.auto" ]
t "the menu launched Alpha Quest next" grep -q 'switcher: Bravo Run -> Alpha Quest' "$LOG"
t "resumed from its auto state" sh -c "grep -q 'game switcher: launching \"Alpha Quest\", resuming' '$LOG' && \
	grep -q 'load state .*Alpha Quest.state.auto (.*): ok' '$GLOG'"
t "both recorded as played" sh -c "[ \$(awk -F '\t' '\$2 == \"Bravo Run.nes\" { print \$5 }' '$DB') -ge 1 ]"

write_resume() {
	cat > "$RESUME" <<EOF
name = Alpha Quest
system = nes
rom = $W/data/roms/nes/Alpha Quest.nes
core = fceumm
core_path = $CORE
core_source = default
reason = user
state = $W/data/states/nes/Alpha Quest.state.auto
EOF
}

echo "b2-frontend: Resume on boot = ask: the 4 choices; ALWAYS RESUME resumes and saves the setting"
[ -s "$W/data/states/nes/Alpha Quest.state.auto" ] || printf 'x' > "$W/data/states/nes/Alpha Quest.state.auto"
write_resume
run "wait:500 expect:dialog:Resume_Alpha_Quest?|RESUME down expect:|ALWAYS_RESUME a wait:2000 \
	expect:toast=Setting_saved term"
t "the run" [ $? = 0 ]
t "resume_boot = always saved" grep -q '^resume_boot *= *always$' "$SET"
t "the game resumed" grep -q 'load state .*Alpha Quest.state.auto (.*): ok' "$GLOG"
t "the offer is gone" [ ! -e "$RESUME" ]

echo "b2-frontend: Resume on boot = always: no dialog, the game at once"
write_resume
run "wait:2000 expect:carousel term"
t "the run" [ $? = 0 ]
t "resumed without a dialog" sh -c "grep -q 'resume_boot always, resuming' '$LOG' && \
	grep -q 'launch Alpha Quest .*resuming (boot offer)' '$LOG'"

echo "b2-frontend: Resume on boot = never: the menu, the auto state kept"
setting resume_boot never
write_resume
run "wait:800 expect:carousel term"
t "the run" [ $? = 0 ]
t "no game, the offer dropped, the state kept" sh -c "grep -q 'resume_boot never' '$LOG' && \
	! grep -q 'launch Alpha Quest' '$LOG' && [ ! -e '$RESUME' ] && [ -s '$W/data/states/nes/Alpha Quest.state.auto' ]"

echo "b2-frontend: ask again, NEVER ASK: fresh start (the menu), saved"
setting resume_boot ask
write_resume
run "wait:500 expect:dialog:Resume_Alpha_Quest? down right expect:|NEVER_ASK a wait:300 expect:carousel term"
t "the run" [ $? = 0 ]
t "resume_boot = never saved, no launch" sh -c "grep -q '^resume_boot *= *never$' '$SET' && \
	! grep -q 'launch Alpha Quest' '$LOG'"
setting resume_boot ask

echo "b2-frontend: per-game scaling and CPU profile (game options), applied at launch"
run "wait:300 a wait:500 expect:game=Alpha_Quest select wait:300 down down down expect:|Scaling right \
	down expect:|CPU_profile right b wait:300 a wait:500 expect:dialog:Resume a wait:1500 expect:list term"
t "the run" [ $? = 0 ]
t "gamedb: aspect, performance" sh -c "[ \"\$(awk -F '\t' '\$2 == \"Alpha Quest.nes\" { print \$9 \",\" \$10 }' '$DB')\" = aspect,performance ]"
t "the game got --scale aspect and its CPU profile" grep -q 'scaling: aspect (--scale), cpu profile: performance' "$GLOG"
t "the menu applied the profile" grep -q 'ui: CPU profile performance' "$LOG"

echo "b2-frontend: scaling changed in the in-game menu: saved for the game"
RSOS_TEST_GAME_ARGS_ONCE="--menu-script sel=Scaling,r" run "wait:300 a wait:500 a wait:500 a wait:1500 \
	expect:list term"
t "the run" [ $? = 0 ]
t "gamedb: integer" [ "$(dbcol "Alpha Quest.nes" 9)" = integer ]
t "reported by the game" grep -q 'game setting scale = integer (saved for Alpha Quest)' "$LOG"

echo "b2-frontend: Hide this game: gone from the list, and at the next start"
run "wait:300 a wait:500 down expect:game=Bravo_Run select wait:300 down down down down down down \
	expect:|Hide_this_game a wait:300 expect:toast=Game_hidden expect:game=Charlie term"
t "the run" [ $? = 0 ]
t "gamedb: hidden" [ "$(dbcol "Bravo Run.nes" 8)" = 1 ]
run "wait:300 a wait:500 down expect:game=Charlie term"
t "not listed at the next start" [ $? = 0 ]
setting show_hidden 1
run "wait:300 a wait:500 down expect:game=Bravo_Run term"
t "listed again with Show hidden games" [ $? = 0 ]
setting show_hidden 0

echo "b2-frontend: Delete this game: the file, then the saves question (NO by default)"
printf 'SRAM' > "$W/data/saves/nes/Charlie.srm"
printf 'ST' > "$W/data/states/nes/Charlie.state1"
printf 'P' > "$W/data/states/nes/Charlie.state1.png"
printf 'SRAM' > "$W/data/saves/nes/Delta.srm"
run "wait:300 a wait:500 down expect:game=Charlie select wait:300 down down down down down down down \
	expect:|Delete_this_game a wait:300 expect:dialog:Delete_\"Charlie.nes\"_from_the_SD_card? expect:|CANCEL left \
	expect:|DELETE a wait:300 expect:dialog:Also_delete_its_saves expect:|NO right expect:|YES a wait:300 \
	expect:toast=Game_and_saves_deleted term"
t "the run" [ $? = 0 ]
t "Charlie: the ROM, its SRAM, its state and thumbnail deleted" sh -c "[ ! -e '$W/data/roms/nes/Charlie.nes' ] && \
	[ ! -e '$W/data/saves/nes/Charlie.srm' ] && [ ! -e '$W/data/states/nes/Charlie.state1' ] && \
	[ ! -e '$W/data/states/nes/Charlie.state1.png' ]"
run "wait:300 a wait:500 down expect:game=Delta select wait:300 down down down down down down down a wait:300 \
	left a wait:300 expect:dialog:Also_delete expect:|NO a wait:300 expect:toast=Game_deleted term"
t "the run" [ $? = 0 ]
t "Delta: the ROM deleted, NO kept its SRAM" sh -c "[ ! -e '$W/data/roms/nes/Delta.nes' ] && \
	[ -s '$W/data/saves/nes/Delta.srm' ]"
run "wait:300 a wait:500 select wait:300 down down down down down down down a wait:300 \
	expect:dialog:Delete expect:|CANCEL a wait:300 expect:menu:Alpha_Quest a wait:300 b wait:300 expect:menu:Alpha_Quest \
	b wait:300 expect:list term"
rc=$?
t "CANCEL (selected) and B delete nothing" sh -c "[ $rc = 0 ] && [ -e '$W/data/roms/nes/Alpha Quest.nes' ]"

if [ $fails = 0 ]; then
	echo "b2-frontend: ALL OK"
else
	echo "b2-frontend: $fails FAILED (logs in $W/run/rsos)"
	exit 1
fi
