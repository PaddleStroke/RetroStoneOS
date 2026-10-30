# The Legend of Zelda: Ocarina of Time (Ship of Harkinian) on the RetroStone2

[Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) is the PC port of the Ocarina of Time
decompilation, built on HarbourMasters' [libultraship](https://github.com/Kenix3/libultraship), like Star Fox 64
([../starship](../starship/README.md)). This folder adds the same OpenGL ES 2.0 mode to Ship of Harkinian 9.3.1's
libultraship and builds the game for the console as a native game (docs/native-games.md).

**Nothing from the game is in this repository, the OS images or the updates.** The game's data comes from **your
own ROM** on your PC and stays yours. Tested with the N64 NTSC 1.2 ROM; the other versions Ship of Harkinian 9.3.1
supports should work the same. `.z64`, `.v64` and `.n64` byte orders are all fine.

## Build

```sh
ports/soh/build-soh.sh ~/"Ocarina of Time (U) (V1.2).z64" ~/my-ports
```

It clones Ship of Harkinian at 9.3.1 into `~/rsos/soh-work`, applies `soh.patch` and `libultraship-gles2.patch`,
builds the asset tools (Torch) for the PC and makes `oot.o2r` from the ROM and `soh.o2r` (which carries the
patched shaders), builds the game for the console (`USE_GLES2`, the RetroStoneOS cross compiler), then writes:

```
~/my-ports/roms/native/Ocarina of Time.sh
~/my-ports/roms/native/soh/   soh.elf, oot.o2r, soh.o2r, gamecontrollerdb.txt, lib/, assets/
```

Copy the `roms` folder onto the SD card's **RETROSTONE** partition; in the menu: **Native games > Ocarina of
Time**. Settings and saves stay in `roms/native/soh/` (`SHIP_HOME`). Hold **Select + Start** for a second to go
back to the menu.

The console needs a RetroStoneOS with Native games and `rsos-gamepad`. Opus, OpusFile and SDL2_net are in the
RetroStone2 configs too; `build-soh.sh` also copies them into `soh/lib/` for a RetroStoneOS that doesn't have them.

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

The console has no right stick, so the C buttons go on X, Y and R1 (the default with `USE_GLES2`). C-up is free:
set it in Ship of Harkinian's own controller settings if you want it. Without an analog stick, map the stick's
directions to the D-pad there too (only tried on a Pro, which has the stick).

## What the patches change

**The GLES2 mode** (as for Starship): an OpenGL ES 2.0 context, GLSL ES 1.00 shaders, unsized texture formats,
packed depth+stencil or 16-bit depth, no multisampling or vertex array objects, framebuffer copies through
`glCopyTexSubImage2D`, depth reads reported as "far", and shader errors printed before aborting.

**32-bit ARM and the Mali-400**, found on the console:
* Textures whose data lives below `0x10000000` were taken for unresolved N64 segment addresses and dropped. That
  is always the heap on 32-bit ARM, so menus, titles and the HUD showed leftover pictures. Textures from the
  game's archives are always valid now, and on 32-bit any mapped address is.
* In 2-cycle mode the shader samples a texture slot the combiner leaves unsized: the slot gets the used one's size.
* Draws with a primitive depth (the pause screen, parts of the HUD) wrote `gl_FragDepth`, which GLSL ES 1.00
  doesn't have: the depth goes into the vertices instead.
* Mesa hands a deleted texture's name out again at once; the texture binding cache forgets it.
* `mixer.c`'s NEON code uses AArch64-only intrinsics: their ARMv7 equivalents.
* `audio_load.c` cleared 8 bytes of a 4-byte array (glibc's `_FORTIFY_SOURCE` stopped the game).
* Torch's command line reads the ROM into memory first (9.3.1's `Extract()` signature).

**Speed** (measured with a CPU profile on the console, where the game's main thread is the limit): vertices go to
the driver as client-side arrays instead of a new buffer object per draw (lima made one each time); assets named
by hash or by a path string are kept once resolved instead of going through the resource manager on every draw
(not when the archives hold alternate assets, which are reloaded as new objects); the texture address check uses
`mincore` instead of `dladdr`.

**Other**: a failed sound start no longer saves "null" as the audio backend for every later start; the crash
handler ends the game with `_exit()`, so a crash returns to the menu at once.

## Status

**Works on a RetroStone2 Pro**: title screen, menus, 3D, sound, saves and the pause screen, with a little slowdown
in busy places. The game's messages are in `rsos/logs/boot<N>/game.log` on the SD card, and Ship of Harkinian's
own log in `roms/native/soh/logs/`.
