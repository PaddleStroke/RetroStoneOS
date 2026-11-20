# The Legend of Zelda: Majora's Mask (2Ship2Harkinian) on the RetroStone2

[2Ship2Harkinian](https://github.com/HarbourMasters/2ship2harkinian) is the PC port of the Majora's Mask
decompilation, built on HarbourMasters' [libultraship](https://github.com/Kenix3/libultraship), like Ocarina of
Time ([../soh](../soh/README.md)). This folder adds the same OpenGL ES 2.0 mode to 2Ship's libultraship, fixes
2Ship for 32-bit ARM and builds the game for the console as a native game (docs/native-games.md).

**Nothing from the game is in this repository, the OS images or the updates.** The game's data comes from **your
own ROM** on your PC and stays yours. Tested with the N64 US ROM (SHA-1 `d6133ace5afaa0882cf214cf88daba39e266c078`
in `.z64` order); 2Ship's extractor also takes the decompressed N64 US ROM and the GameCube US one. `.z64`, `.v64`
and `.n64` byte orders are all fine.

## Build

2Ship's extractor (ZAPD) runs on the PC and is built with libultraship, so the PC needs libultraship's libraries
too, once:

```sh
sudo apt install libsdl2-dev libsdl2-net-dev libpng-dev libzip-dev zipcmp zipmerge ziptool libtinyxml2-dev \
	nlohmann-json3-dev libspdlog-dev libgles-dev libegl-dev libogg-dev libvorbis-dev libopusfile-dev libbz2-dev
```

Then:

```sh
ports/2ship/build-2ship.sh ~/"Majora's Mask (U).z64" ~/my-ports
```

It fetches 2Ship2Harkinian at a fixed commit into `~/rsos/2ship-work`, applies `2ship.patch` and
`libultraship-gles2.patch`, builds ZAPD for the PC and makes `mm.o2r` from the ROM and `2ship.o2r` (which carries
the patched shaders), builds a static libpng and the game for the console (`USE_GLES2`, the RetroStoneOS cross
compiler), then writes:

```
~/my-ports/roms/native/Majora's Mask.sh
~/my-ports/roms/native/2ship/   2s2h.elf, mm.o2r, 2ship.o2r, gamecontrollerdb.txt, lib/, assets/
```

Copy the `roms` folder onto the SD card's **RETROSTONE** partition; in the menu: **Native games > Majora's
Mask**. Settings and saves stay in `roms/native/2ship/` (`SHIP_HOME`). Hold **Select + Start** for a second to go
back to the menu.

The console needs a RetroStoneOS with Native games and `rsos-gamepad`. Opus and OpusFile are in the RetroStone2
configs too; `build-2ship.sh` also copies them into `2ship/lib/` for a RetroStoneOS that doesn't have them.

## Controls

`rsos-gamepad` makes the built-in controls one SDL gamepad, by position. By the labels on the console:

| RetroStone2 | N64 |
|---|---|
| B (bottom) / A (right) | A / B |
| X (top) / Y (left) / R1 | C-down / C-left / C-right |
| L2 / R2 | Z / R |
| L1 | L |
| Start | Start |
| Analog stick (Pro) | the stick |
| D-pad | the D-pad |

As in Ocarina of Time, the C buttons go on X, Y and R1 (the default with `USE_GLES2`); C-up is free to set in
2Ship's own controller settings.

## What the patches change

* **`libultraship-gles2.patch`**: Ship of Harkinian's GLES2 patch ([../soh](../soh/README.md): the OpenGL ES 2.0
  mode, the 32-bit ARM and Mali-400 fixes, the speed work), which applies to 2Ship's libultraship (`7cb1022`) as
  it is, plus:
  * backported from newer libultraship (`766a6bad`): SIGTERM no longer calls `exit()` inside the signal handler,
    whose destructors deadlocked, so quitting from the menu is immediate (as for Mario Kart 64);
  * loading a texture that is already bound no longer ends the draw call (as for Mario Kart 64);
  * first start: linear texture filtering (one sample instead of three-point's three);
  * the crash handler also catches SIGBUS and, on 32-bit ARM, logs the registers and the program's load address.
* **`2ship.patch`**, 2Ship's own code:
  * **notes on 32-bit** (an upstream bug, the cause of the broken music): `AudioPlayback_ProcessNotes` skips a
    note whose layer pointer is below `0x7FFFFFFF`, a check from the N64, where RAM starts at `0x80000000`. On a
    64-bit PC no heap pointer is that low; on 32-bit ARM every one is, so no playing note got its volume, pitch or
    start, and instruments were cut off or missing. The check is now for 64-bit builds only (2Ship already left
    it out on the Wii U, its other 32-bit platform);
  * **the mixer's DMEM buffer** (an upstream bug, found with AddressSanitizer on the console): some audio commands
    clear or move more bytes than the 0xC80-byte buffer holds, which overwrote the memory after it (crashes and a
    freeze in the opening cutscene). The buffer has a guard area and those lengths are clamped to it;
  * **samples' `sampleEnd`** (an upstream bug): the sample importers never set it, so the synthesizer read
    leftover memory for where a sample ends. It is now the sample's length;
  * when the game runs below 20 fps, the audio thread makes up to three extra audio frames to refill the buffer,
    instead of gaps;
  * the GLES2 changes in 2Ship's own copy of the shaders (packed into `2ship.o2r`);
  * `mixer.c`'s NEON code uses AArch64-only intrinsics: their ARMv7 equivalents;
  * Release builds with `USE_GLES2` use `-O3 -funsafe-math-optimizations` instead of 2Ship's `-O2` on Linux:
    GCC uses NEON for float math on 32-bit ARM only with unsafe math.

The launcher starts `2s2h.elf` directly, never through `/lib/ld-linux-armhf.so.3` as the other ports can: 2Ship
looks for its `.o2r` files next to the program it was started as, which would be `/lib`.

## Status

**Works on a RetroStone2 Pro**: title screen, the opening, 3D, music and sound, at the game's own speed (20 fps
in most places). The game's messages are in `rsos/logs/boot<N>/game.log` on the SD card, and 2Ship's own log in
`roms/native/2ship/logs/`.

`mm.o2r` is not the same byte for byte from one build to the next: ZAPD writes uninitialized memory into fields the
game doesn't use (the vertex count of limbs without skin, the tuning of drums without a sample).
