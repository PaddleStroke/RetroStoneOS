# Native games

A native game is a program built for the console itself (ARM, Linux), started without an emulator: an
open-source game engine, a source port, a homebrew game. The menu lists them as the system **Native games**.

## On the SD card

One launch script per game in `roms/native/` (subfolders are fine), with the game's own files next to it:

```
roms/native/
  My Game.sh            <- the entry in the menu ("My Game")
  mygame/
    mygame              <- the program (ARM, linked against this system's libraries)
    data files...
```

The script runs with `/bin/sh` in **its own folder** (the working directory), so it can use relative paths. It
should end with `exec` so the game is the process the menu watches:

```sh
#!/bin/sh
cd mygame
exec ./mygame
```

A program that the exFAT partition does not mark executable can be started through the dynamic loader:
`exec /lib/ld-linux-armhf.so.3 ./mygame`.

## What the game gets

* The display, the sound and the pads, as a libretro game does: the menu drops DRM master before the launch and
  takes it back afterwards. **SDL2** is installed with the KMS/DRM video driver (GLES2 on the Mali-400 through
  Mesa's lima) and ALSA; the menu sets `SDL_VIDEODRIVER=kmsdrm` and `SDL_AUDIODRIVER=alsa`.
* `HOME=/data/rsos/native-home` (the root file system is read-only), with `XDG_CONFIG_HOME` and
  `XDG_DATA_HOME` under it. A game that saves next to itself (its working directory) keeps its saves on the card.
* `RSOS_GAME_DIR` (the script's folder) and `RSOS_LANG` (the menu's language, e.g. `en`).
* Its output goes to `/run/rsos/game.log`, copied to `rsos/logs/` on the SD card at the next start.

The built-in controls are three input devices (the buttons, the shoulder buttons, the optional analog stick):
a game that reads only one SDL game controller would see only one of them, and SDL does not take the stick for a
joystick at all. **`rsos-gamepad`** (RetroStone2, build 18 or newer) merges them into one standard virtual
gamepad, "RetroStoneOS Gamepad" (uinput): the buttons by position (bottom = SDL "a", left = "x"), L1/R1 the
shoulders, L2/R2 the triggers, the optional C/Z the stick clicks, the D-pad as hat 0, the stick as the left stick
(centred while it is not fitted, as in the menu). A launch script for an SDL game starts it and gives SDL its
mapping (ports/starship/Star Fox 64.sh):

```sh
SDL_GAMECONTROLLERCONFIG="$(/usr/bin/rsos-gamepad --print-mapping)"
export SDL_GAMECONTROLLERCONFIG
export SDL_GAMECONTROLLER_IGNORE_DEVICES=0x0001/0x0001   # the raw button devices
/usr/bin/rsos-gamepad &
```

It runs in the game's process group, so the menu stops it with the game. A game can also read the devices
itself from `/dev/input` (ports/sm64/controller_rsos.c does, to follow the N64 emulator's button settings).

## What the menu does around it

* **Select + Start** held for one second quits the game (SIGTERM to its process group, SIGKILL if it is still
  there 5 s later). A native game has no in-game menu of ours.
* **Sleep** (power key): the game is frozen (SIGSTOP) and the backlight goes off; waking continues it.
* **Power-off**: the game gets SIGTERM, then SIGKILL after 8 s. There is no save state: the game must save on
  its own.
* The **idle power-off** waits while a native game runs (it could not resume it).
* An exit code other than 0 (and other than a stop by Select+Start or the menu) shows "The game ended with an
  error".

`make check-frontend` (steps 2a-2c) covers the launch, the environment, the error message and the process group.
