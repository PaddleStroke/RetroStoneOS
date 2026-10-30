# Super Mario 64 on the RetroStone2 (native, built from your own ROM)

This folder builds [sm64-port](https://github.com/sm64-port/sm64-port) (the PC port of the Super Mario 64
decompilation) for RetroStoneOS, where it runs as a **native game**: ARM code with the Mali-400 GPU (GLES2), no N64
emulator (docs/native-games.md).

**Nothing from the game is in this repository, in the OS images or in the System updates.** The build takes the
game's data from **your own ROM**, on your PC, and the result is yours alone: copy it to your own SD card, never
publish it (not in a release, an update, a fork or a download link).

## What you need

* A PC with Linux or WSL (Ubuntu), the usual build tools (`build-essential`, `git`, `python3`) and a built
  RetroStoneOS tree for the RetroStone2 with SDL2 (`retrostone2_release_defconfig`; `~/rsos/output` or
  `RSOS_OUTPUT=<dir>`).
* Your Super Mario 64 ROM, **US** version, `.z64` byte order (SHA-1 `9bef1128717f958171a4afac3ed78ee2bb4e86ce`).
* A RetroStoneOS with "Native games" on the console (it brings SDL2).

## Build

```sh
ports/sm64/build-sm64.sh ~/baserom.us.z64 ~/sm64-retrostone2
```

It clones sm64-port at a fixed commit into `~/rsos/sm64-work`, applies `retrostone2.patch` and adds
`controller_rsos.c`, builds the asset tools for the PC and the game for the console (`TARGET_LINUX_KMSDRM=1`,
the RetroStoneOS cross compiler), and writes:

```
~/sm64-retrostone2/roms/native/Super Mario 64.sh
~/sm64-retrostone2/roms/native/sm64/sm64.us
```

## Install

Put the console's SD card in the PC and copy the `roms` folder onto the **RETROSTONE** partition (it merges with
the `roms` folder already there), or use the menu's USB import. In the menu: **Native games > Super Mario 64**.

## Controls

The buttons are **the same as the N64 emulator** (parallel_n64), so Mario plays the same natively and emulated,
and the N64 button settings are global: Mario follows the remap you make for N64 in the emulator's in-game menu
(`rsos/remaps/n64.ini` on the card; `n64-cz.ini` when "C/Z buttons fitted" is on) and the N64 core option
**Independent C-button Controls**.

| RetroStone2 | N64 (default) | N64 (Independent C-button Controls on) |
|---|---|---|
| B (bottom) | A | A |
| Y (left) | B | B |
| L2 | Z | Z |
| L1 / R1 | L / R | C-left / C-right |
| A (right) / X (top) | (with R2: C-right / C-up) | C-down / C-up |
| R2 | hold: B, Y, A, X become C-down, C-left, C-right, C-up | R |
| Select | | L |
| Start | Start | Start |
| Analog stick (Pro), or the D-pad | the stick | the stick |

For Super Mario 64, **Independent C-button Controls** is the comfortable one (L1/R1 turn the camera, R2 is R):
in any N64 game, open the in-game menu, Core options, turn it on and choose **Save for all games**; both the
emulator and native Mario use it from then on. ("Save for this game" keeps it to that one ROM only.) Mario reads
these settings when it starts.

**Select + Start** goes back to the menu at once, as in the emulators (the save file is written whenever the game
saves). A USB pad works too (SDL: bottom = A, left = B, the
shoulders = Z/R, the right stick = C buttons).

Saves (`sm64_save_file.bin`) and settings (`sm64config.txt`) stay in `roms/native/sm64/`. The game saves on its
own (each star); quitting or the power key never loses a saved star.

## What the patch changes in sm64-port

* `TARGET_LINUX_KMSDRM=1`: SDL2 on KMS/DRM with a GLES2 context (`#version 100` shaders with
  `precision mediump float`), ALSA, no X11/PulseAudio/Wii U adapter, no `-march=native`; `CROSS_PC` (the
  compiler of the game only, the asset tools keep the PC's) and `SDL2_CONFIG`.
* `controller_rsos.c`: the console's three input devices (buttons, shoulder buttons, analog stick) read
  together from `/dev/input`; `controller_sdl` leaves them alone and keeps USB pads.
* `tools/armips.cpp`: `<cstdint>` for newer host compilers.

## Status

**Works on a RetroStone2 Pro**: full speed, it looks great. The analog stick moves Mario on the Pro; the D-pad
moves him whenever the stick is centred or not fitted. If something goes wrong, the game's messages are in
`rsos/logs/boot<N>/game.log` on the SD card.
