# Native games

A native game is a program built for the console itself (ARM, Linux), started without an emulator: an
open-source game engine, a source port, a homebrew game. The menu lists them in the one system **Ports**, with the
Doom and Cave Story games (folders `roms/native`, `roms/doom`, `roms/cavestory`, and `roms/ports` for any of them).
A game of the list starts as the system it belongs to: a `.sh` launch script as a native game (this page), a `.wad`
as Doom, an `.exe` as Cave Story (their libretro cores). Favorites and play statistics follow the games.

[ports/](../ports/README.md) has build scripts for three N64 PC ports (Super Mario 64, Star Fox 64, Ocarina of
Time), each built on a PC from the user's own ROM. No game or game data is in this repository.

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
* **HDMI**: like a libretro game, the game is on the TV or monitor while HDMI is plugged in (the built-in screen is off) and
  back on the built-in screen when it is unplugged, live. The SDL2 of the image does this (docs/display-design.md,
  9b): the game always renders 640x480 and the display engine scales it, so it must not assume another size. The sound
  follows: SDL's ALSA backend plays on the HDMI sound card while the game is on HDMI and on the speaker otherwise
  (a device the game chose itself, or `AUDIODEV`, is left alone).
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
  there 5 s later); on an N64 or GameCube pad (no Select) **Z + Start**, on any pad **Home / Guide / PS**. A native
  game has no in-game menu of ours.
* **External pads**: each one's mapping, as the menu resolved it (user, built-in profile, database or the Linux
  standard), is written as an SDL line to `/run/rsos/sdl-controllers.txt` and passed in
  `SDL_GAMECONTROLLERCONFIG_FILE`, so a game sees a pad as the menu does. N64 and GameCube pads get the database's
  names for them (an N64 pad: A/B on a/b, the C buttons on the right stick, Z on the left trigger, R also on the
  right trigger), which the libultraship ports and sm64-port read.
* **Sleep** (power key): the game is frozen (SIGSTOP) and the backlight goes off; waking continues it.
* **Power-off**: the game gets SIGTERM, then SIGKILL after 8 s. There is no save state: the game must save on
  its own.
* The **idle power-off** waits while a native game runs (it could not resume it).
* An exit code other than 0 (and other than a stop by Select+Start or the menu) shows "The game ended with an
  error".

`make check-frontend` (steps 2a-2c) covers the launch, the environment, the error message and the process group.

## Layouts: an N64 pad that plays like the built-in buttons

A port's bindings are usually changed for the built-in buttons (the C buttons on the shoulders, L on Select...),
so a real N64 pad, plugged in with the menu's own N64 mapping (C buttons on the right stick, Z on the left trigger,
L and R on the shoulders), would press the wrong things. A **layout** says which SDL element the game reads for each
N64 function; the menu applies it to an N64 pad's mapping when it starts the game, so the pad does what the
built-in buttons do in that game.

* The file is `<launch script name>.layout` next to the script (`Ocarina of Time.sh` → `Ocarina of Time.layout`),
  lines of `function = SDL element`, `#` for comments. A line left out keeps the default; `none` drops the element.
* Functions: `a b z l r start cup cdown cleft cright dpup dpdown dpleft dpright`. Elements are SDL names
  (`a b x y back start leftshoulder rightshoulder lefttrigger righttrigger dpup dpdown dpleft dpright`, and the
  right-stick halves `-righty +righty -rightx +rightx`).
* All lines apply at once, so two functions can swap. Naming `r` also removes the default's copy of R on the right
  trigger (an N64 pad has no ZR), which a port could read as another button.
* The menu also gives an N64 pad whose name does not say so (a USB adapter's "Generic USB Joystick") the name
  "N64 Controller (...)", for games that pick their layout by the pad's name (GoldenEye does).
* Edit the file on the card to change it; no new build is needed. Layouts shipped with the ports are in `ports/`.
