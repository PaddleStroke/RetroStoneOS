#!/bin/sh
# Star Fox 64 (Starship), built from your own ROM by ports/starship/build-starship.sh.
# The game data (sf64.o2r), settings and saves stay in starship/.
cd starship || exit 1
export SHIP_HOME="$PWD"
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
if [ -x ./Starship ]; then
	exec ./Starship
fi
exec /lib/ld-linux-armhf.so.3 ./Starship
