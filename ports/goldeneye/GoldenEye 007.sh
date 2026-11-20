#!/bin/sh
# GoldenEye 007 (Project Midas), built from your own ROM by ports/goldeneye/build-goldeneye.sh.
# The program, your ROM (Data/), settings (midas.ini) and saves (Data/Saves/) stay in goldeneye/.
cd goldeneye || exit 1
# first start: settings for the console (the game fills in the rest and keeps them): one game
# tick per frame, so maxfps is also the game's speed
if [ ! -f midas.ini ]; then
	printf '[display]\nfullscreen=fullscreen\nmaxfps=30\nfiltering=bilinear\nshow_fps=1\n' > midas.ini
fi
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
exec ./ProjectMidas
