# Star Fox 64 (Starship) on the RetroStone2

[Starship](https://github.com/HarbourMasters/Starship) is the PC port of the Star Fox 64 decompilation, built on
HarbourMasters' [libultraship](https://github.com/Kenix3/libultraship). libultraship draws with OpenGL 3.0 or
OpenGL ES 3.0; the RetroStone2's Mali-400 only has **OpenGL ES 2.0**. This folder adds an OpenGL ES 2.0 mode to
libultraship (`USE_GLES2`) and builds Starship for the console as a native game (docs/native-games.md).

**Nothing from the game is in this repository, the OS images or the updates.** As for Super Mario 64, the game's
data comes from **your own ROM** on your PC (Star Fox 64 US 1.1, `.z64`, SHA-1
`09f0d105f476b00efa5303a3ebc42e60a7753b7a`) and stays yours.

## Status

| Step | |
|---|---|
| GLES2 renderer (`libultraship-gles2.patch`) | written |
| Cross-build for the RetroStone2 (32-bit ARM) | **builds** (Starship `3322643f`, libultraship `eaaf9d0`, Torch `cd92cc0`) |
| Game data from the ROM (`sf64.o2r`) and the port archive (`starship.o2r`) | **built** on a PC |
| Libraries in the OS (Ogg/Vorbis, libzip, tinyxml2, spdlog, fmt) | in the RetroStone2 configs |
| The built-in controls | `rsos-gamepad`: the three devices as one standard SDL gamepad, started by the launch script |
| Build script for the SD card folder | `build-starship.sh` |
| Run on a RetroStone2 | **runs** (RetroStone2 Pro); some slowdown in busy scenes |

Starship's libultraship still uses **SDL2** (only libultraship's newest code moved to SDL3), so no SDL3 is needed.

## What the GLES2 mode changes

* **Context**: asks SDL for an OpenGL ES 2.0 context (Mesa's lima would otherwise give desktop OpenGL 2.1).
* **Shaders**: GLSL ES 1.00 (`attribute`/`varying`, `texture2D`, `gl_FragColor`, `precision mediump float`);
  `textureSize()` replaced by the texture's size for masks; `mix(..., bvec3)` written with `vec3()`; the noise
  constant kept below 65504 (Mali-400 fragment shaders have 16-bit floats only). Both copies of the shader
  templates: libultraship's and Starship's own `port/shaders` (packed into `starship.o2r`).
* **Framebuffers**: unsized texture formats, packed depth+stencil when the driver has
  `GL_OES_packed_depth_stencil` (else 16-bit depth), clamp-to-edge on the screen-sized colour textures (GLES2 has
  no repeat on non-power-of-two textures), no multisampling, no vertex array object.
* **What GLES2 cannot do**: framebuffer blits (copies go through `glCopyTexSubImage2D`, unscaled only) and depth
  reads (reported as "far"). Starship uses neither; only libultraship's debug window does.
* A shader that fails to compile now prints the compiler's message and the shader before aborting.

## Other fixes in the patches

* **First start** (GLES2 builds): the cheap settings once, still changeable in the menu: the game's own frame
  rate (no interpolated frames), 320x240 rendering, bilinear filtering, no MSAA, no "press F1" hint.
* **Crash with frame interpolation off** ("Original", 20 FPS): the game turned recording back on in the middle
  of a frame and crashed in `FrameInterpolation_RecordMatrixPop`; recording now only resumes in a frame that
  started it.
* **Handheld controls**: Select + Start quits at once; Start skips any level intro (`gSkipAnyIntro`).
* **Crash handler**: `_exit()` instead of `exit()` in the signal handlers, so a crash or a quit ends the game at
  once (exit status 128 + signal) instead of hanging on a frozen picture.

## Build

```sh
ports/starship/build-starship.sh ~/"Star Fox 64 (U) (V1.1).z64" ~/my-ports
```

It clones Starship at the fixed commit into `~/rsos/starship-work`, applies `starship.patch` and
`libultraship-gles2.patch`, checks that the ROM is one Starship supports, builds the game for the console
(`USE_GLES2`, the RetroStoneOS cross compiler), builds Torch for the PC and makes `sf64.o2r` from the ROM and
`starship.o2r`, then writes:

```
~/my-ports/roms/native/Star Fox 64.sh
~/my-ports/roms/native/starship/   Starship, sf64.o2r, starship.o2r, gamecontrollerdb.txt
```

Copy the `roms` folder onto the SD card's **RETROSTONE** partition; in the menu: **Native games > Star Fox 64**.
Settings and saves stay in `roms/native/starship/` (`SHIP_HOME`). **Select + Start** quits at once, as in the
emulators. **Start** skips a level's opening scene, also on a planet not cleared yet (the N64 game allows it only
after a clear; the `gSkipAnyIntro` setting at 0 in `starship.cfg.json` brings that back).
The console needs a RetroStoneOS with Native games, the port libraries and `rsos-gamepad`.
