#!/bin/sh
# fake-rsos-store.sh: a stand-in for rsos-store (src/update/rsos-store.c) for the preview tool
# (--store-helper): three games, the install state in $FAKE_STORE_DIR/installed, every call logged
# to $FAKE_STORE_DIR/calls.log. FAKE_STORE_FAIL=space makes "get" fail like a full drive; a port needs a ROM:
# "find-rom" finds one unless $FAKE_STORE_DIR/norom exists (FAKE_STORE_FAIL=memory: install-port fails).
D=${FAKE_STORE_DIR:?FAKE_STORE_DIR is not set}
mkdir -p "$D"
echo "$*" >> "$D/calls.log"
OUT=/dev/stdout
ROMS=
ROMARG=
while [ $# -gt 0 ]; do
	case "$1" in
	--out) OUT=$2; shift 2 ;;
	--roms) ROMS=$2; shift 2 ;;
	--rom) ROMARG=$2; shift 2 ;;
	--scan) shift 2 ;;
	*) break ;;
	esac
done
CMD=$1
ID=${2:-}
INS=$D/installed
touch "$INS"
T=$(printf '\t')
entry() { # id name system version size desc kind
	v=$(grep "^$1$T" "$INS" | cut -f2)
	upd=0
	[ -n "$v" ] && [ "$v" != "$4" ] && upd=1
	extra=
	[ "$7" = port ] && extra=$(printf '	rom=Ocarina of Time (US 1.2)	mode=extract	minutes=4')
	printf 'entry	id=%s	name=%s	system=%s	version=%s	author=Test Author	license=CC0	desc=%s	size=%s	kind=%s	installed=%s	update=%s%s
' 		"$1" "$2" "$3" "$4" "$6" "$5" "$7" "$v" "$upd" "$extra"
}
case "$CMD" in
find-rom)
	if [ -e "$D/norom" ]; then
		printf 'error	code=norom	text=no ROM found
' > "$OUT"
		exit 1
	fi
	printf 'rom	path=/data/roms/n64/Ocarina of Time (U) (V1.2).z64	sha1=41b3bdc4	name=Ocarina of Time
done	count=1
' > "$OUT"
	;;
install-port)
	if [ "${FAKE_STORE_FAIL:-}" = memory ]; then
		printf 'error	code=memory	text=not enough memory
' > "$OUT"
		exit 1
	fi
	printf 'progress	phase=extract	done=30	total=100
' > "$OUT"
	sleep 1
	printf 'progress	phase=extract	done=90	total=100
ok	id=%s	version=x
' "$ID" >> "$OUT"
	grep -v "^$ID$T" "$INS" > "$INS.tmp"
	printf '%s	%s
' "$ID" 9.3.1 >> "$INS.tmp"
	mv "$INS.tmp" "$INS"
	;;
list)
	{
		entry micro-mage "Micro Mage" nes 1.0 41040 "A platformer for the NES." rom
		entry cool-game "Cool Game" native 2.1 5242880 "A native game." native
		entry zelda-port "Ocarina of Time" native 9.3.1 61000000 "A port made from your own ROM." port
		entry puzzle "Puzzle Pack" snes 1.1 800000 "Ten puzzles." tar
		printf 'done\tcount=3\tsource=net\n'
	} > "$OUT"
	;;
get)
	if [ "${FAKE_STORE_FAIL:-}" = space ]; then
		printf 'error\tcode=space\ttext=not enough free space\n' > "$OUT"
		exit 1
	fi
	printf 'progress\tphase=download\tdone=500\ttotal=1000\n' > "$OUT"
	sleep 1
	printf 'progress\tphase=download\tdone=1000\ttotal=1000\nok\tid=%s\tversion=x\n' "$ID" >> "$OUT"
	grep -v "^$ID$T" "$INS" > "$INS.tmp"
	case "$ID" in
	micro-mage) ver=1.0 ;; cool-game) ver=2.1 ;; *) ver=1.1 ;;
	esac
	printf '%s\t%s\n' "$ID" "$ver" >> "$INS.tmp"
	mv "$INS.tmp" "$INS"
	;;
remove)
	grep -v "^$ID$T" "$INS" > "$INS.tmp"
	mv "$INS.tmp" "$INS"
	printf 'ok\tid=%s\tremoved=1\n' "$ID" > "$OUT"
	;;
*) exit 2 ;;
esac
exit 0
