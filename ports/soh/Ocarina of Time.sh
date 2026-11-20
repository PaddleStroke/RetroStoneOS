#!/bin/sh
# The Legend of Zelda: Ocarina of Time (Ship of Harkinian), built from your own ROM by
# ports/soh/build-soh.sh. The game data (oot.o2r, soh.o2r), settings and saves stay in soh/.
cd soh || exit 1
export SHIP_HOME="$PWD"
# Opus, OpusFile and SDL2_net, copied next to the game for a RetroStoneOS without them
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
if [ -x ./soh.elf ]; then
	exec ./soh.elf
fi
exec /lib/ld-linux-armhf.so.3 ./soh.elf
