# GoldenEye 007 (Project Midas) on the RetroStone2

[Project Midas](https://github.com/murkantor/Project-Midas) is a PC port of GoldenEye 007 built from the
[N64 decompilation](https://github.com/n64decomp/007). Its renderer is fast3d (the Perfect Dark port's, the same
family as Super Mario 64's) and its PC builds are deliberately **32-bit**: GoldenEye's code keeps N64 pointers in
32-bit fields, so Midas runs it on the ILP32 model. The RetroStone2's ARMv7 is that same 32-bit, little-endian
model, so the game itself needs almost no change; the work is the renderer, which needs OpenGL 3.0. This folder
adds an OpenGL ES 2.0 mode to it and builds the game for the console as a native game (docs/native-games.md).

**Nothing from the game is in this repository, the OS images or the updates.** The game's data comes from **your
own ROM** on your PC and stays yours. Project Midas takes the **GoldenEye 007 (US)** ROM, SHA-1
`abe01e4aeb033b6c0836819f549c791b26cfde83` in `.z64` order (`.v64` and `.n64` dumps are converted). The game
also reads the ROM itself at every start, so a copy goes into `goldeneye/Data/` on the card.

## Build

```sh
ports/goldeneye/build-goldeneye.sh ~/"GoldenEye 007 (U).z64" ~/my-ports
```

It fetches Project Midas at a fixed commit (1.0.2b) into `~/rsos/goldeneye-work`, applies `goldeneye.patch`,
extracts the assets from the ROM with Midas's own tools (built for the PC: `make`, a C compiler and `python3`
are enough), builds the game for the console (`GE_GLES2`, the RetroStoneOS cross compiler), then writes:

```
~/my-ports/roms/native/GoldenEye 007.sh
~/my-ports/roms/native/goldeneye/   ProjectMidas, gamecontrollerdb.txt, gamecontrollerdefault.txt,
                                    midas_ini_help.txt, Data/ (your ROM, shaders_base.txt)
```

Copy the `roms` folder onto the SD card's **RETROSTONE** partition; in the menu: **Native games > GoldenEye
007**. Settings (`midas.ini`) and saves (`Data/Saves/`) stay in `roms/native/goldeneye/`. Hold **Select + Start**
for a second to go back to the menu. The console needs a RetroStoneOS with Native games and `rsos-gamepad`.

On the first start the launcher writes a `midas.ini` for the console: 30 fps (Midas runs one game tick per
frame, so this is also the game's pace), bilinear filtering and the frame counter in the top-left corner
(`show_fps=0` hides it). `midas_ini_help.txt` explains every setting.

## Controls

The original N64 control styles (1.1 Honey by default, 1.1-2.4 in the game's options), through `rsos-gamepad`. By
the labels on the console:

| RetroStone2 | N64 |
|---|---|
| Analog stick (Pro) | the stick (move, turn) |
| D-pad | C buttons (look up/down, strafe) |
| R2 or Y (left) | Z (fire) |
| R1 or X (top) | R (aim) |
| L1 or L2 | L |
| B (bottom) / A (right) | A (change weapon) / B (use, reload) |
| Start | Start |

The layout is the "RetroStone2" line at the top of `goldeneye/gamecontrollerdefault.txt`.

## What the patch changes

**OpenGL ES 2.0** (`GE_GLES2`, for the Mali-400): an ES 2.0 context and glad's GLES2 loader, GLSL ES 1.00 shaders,
texture sizes as uniforms (GLSL ES 1.00 has no `textureSize()`), no vertex array objects, unsized texture
formats, edge clamping instead of border clamping, no mipmaps on non-power-of-two textures without
`OES_texture_npot`, linear instead of three-point filtering, no framebuffer effects (they need blits and
multisampling: no MSAA, N64 edge AA, Low Res or Classic Mode), a window that is visible from the start (SDL's
KMSDRM backend), and streamed vertices as client-side arrays (lima made a new buffer object for every upload).

**The Mali-400's 16-bit fragment shaders**, found on the console:
* GoldenEye's far clip discards fragments past the far plane by comparing clip z with w. In 16-bit floats those
  two large, nearly equal numbers rounded to the wrong side, and the whole intro, the menus and many effects
  (muzzle flashes, impacts, the Dam's gate switch) were discarded. The vertex shader (32-bit) now passes w - z.
* The noise (alpha dither) multiplied by 143758.5453, which is infinity in 16 bits: a hash that stays small.

**32-bit ARM**: the x86 flags give way to ARM's (single-precision VFP rounds like the N64 already;
`-ffp-contract=off` keeps multiply-adds unfused, as on the N64 and SSE), the program is linked at `0x10000000`
(the address the Windows build uses: the display-list code takes any value below it for an N64 segment address),
`-fpermissive` for GCC 14, and crash reports with the ARM registers and the return addresses on the stack.
Upstream bug fixed: `stanCheckLinkedSpecialTile` declared the float arguments of the tile-walk callback as `s32`;
on the hard-float ARM ABI the floats travel in other registers, so its last argument came from the stack and
walking onto some tiles crashed the game.

**Speed** (CPU profiles on the console): drawing on its own thread (the A20 has two cores), `-O3` and NEON math
for the renderer, `-O3` for the audio mixer. Midas's room cache (geometry kept on the GPU) stays off: its vertex
shader exceeds the Mali-400 vertex processor's 512 instructions. Its setting now applies at start-up (it only
did from an ini line).

**The controls**: the "RetroStone2" layout above, on the N64 control styles.

## Status

**Works on a RetroStone2 Pro** (played: the intro, the menus and the Dam): picture, effects and sound. 30 fps in light areas, around 10-15 in the
busiest views (the N64 itself often ran GoldenEye at 15-20); the time goes to GL draw calls in the Mesa lima
driver, a few hundred a frame. The game's messages are in `rsos/logs/boot<N>/game.log` on the SD card.
