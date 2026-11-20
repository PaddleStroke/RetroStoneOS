#!/bin/sh
# The Legend of Zelda: Majora's Mask (2Ship2Harkinian), built from your own ROM by
# ports/2ship/build-2ship.sh. The game data (mm.o2r, 2ship.o2r), settings and saves stay in 2ship/.
cd 2ship || exit 1
export SHIP_HOME="$PWD"
# Opus and OpusFile, copied next to the game for a RetroStoneOS without them
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# The built-in controls as one standard gamepad (rsos-gamepad); SDL then
# ignores the raw button devices. It runs in the
# game's process group: the menu stops it with the game.
if [ -x /usr/bin/rsos-gamepad ]; then
	SDL_GAMECONTROLLERCONFIG="$(/usr/bin/rsos-gamepad --print-mapping)"
	export SDL_GAMECONTROLLERCONFIG
	export SDL_GAMECONTROLLER_IGNORE_DEVICES=0x0001/0x0001
	/usr/bin/rsos-gamepad &
	usleep 300000
fi
# Not through /lib/ld-linux-armhf.so.3 as the other ports can: 2Ship looks for its .o2r files
# next to the program it was started as, which would then be /lib
exec ./2s2h.elf
