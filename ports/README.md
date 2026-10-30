# Native ports for the RetroStone2

PC ports of N64 games (and other open-source engines) can run on the RetroStone2 **as native games**: ARM programs
that draw with the console's GPU, with no emulator in between. They are much faster and look much better than
emulating the N64: Super Mario 64 runs at full speed on the RetroStone2.

**RetroStoneOS never ships any game.** This folder holds only build scripts and code changes. Each port is built
**on your PC, from your own ROM**, and the result goes onto your own SD card. Keep it to yourself: never upload it,
share it or put it in a release.

## How to get a native port (step by step)

1. **Update the console** to a RetroStoneOS with **Native games** in the menu (Settings > System update). The
   libultraship ports (Star Fox 64, Ocarina of Time, Majora's Mask, Mario Kart 64) also need `rsos-gamepad` and the port libraries in the
   RetroStone2 configs (Ogg/Vorbis, Opus, libzip, tinyxml2, spdlog, SDL2_net).
2. **Set up the build on a PC** with Linux or Windows + WSL (Ubuntu):
   * the usual tools: `sudo apt install build-essential git python3 cmake ninja-build`;
   * RetroStoneOS, built once for the RetroStone2: it provides the compiler for the console and its libraries.
     Follow [docs/build.md](../docs/build.md) with `retrostone2_release_defconfig` (the output in
     `~/rsos/output`). A port built once usually keeps working after the console's System updates.
3. **Build the port** with its script, giving it your ROM (each port's README says which ROM version):
   ```sh
   ports/sm64/build-sm64.sh ~/baserom.us.z64 ~/my-ports
   ```
4. **Copy** the `roms` folder from the output (`~/my-ports/roms`) onto the SD card's **RETROSTONE** partition
   (it merges with the `roms` folder there). From WSL, Windows sees it at `\\wsl$\Ubuntu\home\<you>\my-ports`.
5. In the menu: **Native games** > the game. **Select + Start** goes back to the menu (Super Mario 64 and Star Fox
   64 quit at once, as the emulators do; any other native game when both are held for a second).

## Available ports

| Game | Folder | Port | Status on the RetroStone2 |
|---|---|---|---|
| Super Mario 64 | [sm64](sm64/README.md) | [sm64-port](https://github.com/sm64-port/sm64-port) | **Works**: full speed, built-in controls, analog stick (Pro) or D-pad |
| Star Fox 64 | [starship](starship/README.md) | [Starship](https://github.com/HarbourMasters/Starship) + a GLES2 libultraship | **Works**: built-in controls, cheaper first-start settings; some slowdown in busy scenes |
| Mario Kart 64 | [spaghettikart](spaghettikart/README.md) | [SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart) + a GLES2 libultraship | **Works**: races, sound, saves, built-in controls; mostly 30 fps, some slowdown in busy places |
| The Legend of Zelda: Ocarina of Time | [soh](soh/README.md) | [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) 9.3.1 + a GLES2 libultraship | **Works**: menus, 3D, sound, saves; a little slowdown in busy places |
| GoldenEye 007 | [goldeneye](goldeneye/README.md) | [Project Midas](https://github.com/murkantor/Project-Midas) + a GLES2 fast3d renderer | **Works**: intro, menus, levels, sound; 30 fps in light areas, 10-15 in busy views |
| The Legend of Zelda: Majora's Mask | [2ship](2ship/README.md) | [2Ship2Harkinian](https://github.com/HarbourMasters/2ship2harkinian) + a GLES2 libultraship | **Works**: 3D, music and sound, at the game's own 20 fps in most places |

## Other N64 PC ports: can they run here?

The RetroStone2's GPU is a **Mali-400 MP2: OpenGL ES 2.0 only** (Mesa's lima driver), with 1 GB of RAM and two
Cortex-A7 cores at 960-1056 MHz. That decides which ports can work:

| Port family | Games | GPU it needs | Here |
|---|---|---|---|
| sm64-port's renderer (Fast3D, GLSL 1.10 / ES 1.00) | Super Mario 64 (sm64-port, sm64ex) | OpenGL 2.1 / **ES 2.0** | **Yes** (Super Mario 64 works) |
| [libultraship](https://github.com/Kenix3/libultraship) (HarbourMasters) | Star Fox 64 ([Starship](https://github.com/HarbourMasters/Starship)), Ocarina of Time ([Ship of Harkinian](https://github.com/HarbourMasters/Shipwright)), Majora's Mask ([2Ship2Harkinian](https://github.com/HarbourMasters/2ship2harkinian)), Mario Kart 64 ([SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart)), [Ghostship](https://github.com/HarbourMasters/Ghostship), Banjo-Kazooie ([Lighthouse](https://github.com/HarbourMasters/Lighthouse)), Paper Mario ([PaperBoat](https://github.com/HarbourMasters/PaperBoat)) | OpenGL 3.0 (GLSL 1.30) or **ES 3.0** | **Yes with the GLES2 mode** in these patches (Star Fox 64, Ocarina of Time, Majora's Mask and Mario Kart 64 work); ports on the newest libultraship also need SDL3 |
| The [Perfect Dark port](https://github.com/fgsfdsfgs/perfect_dark)'s fast3d (also in [Project Midas](https://github.com/murkantor/Project-Midas)) | Perfect Dark, GoldenEye 007 | OpenGL 3.0 / ES 3.0 | **Yes with the GLES2 mode** in [goldeneye](goldeneye/README.md) (GoldenEye works); Perfect Dark would need the same changes |
| N64 Recompiled (RT64) | Zelda 64 Recomp, Banjo: Recompiled, DK64, Star Fox 64 Recomp, ... | Vulkan / Direct3D 12 | **No**: the Mali-400 has neither |

### libultraship on OpenGL ES 2.0

libultraship's OpenGL renderer has shader variants for desktop GL and **GLES 3.0**; the ports here add a **GLES
2.0** mode (`USE_GLES2`): GLSL ES 1.00 shaders, GLES2 framebuffers and depth buffers, no multisampling. Each port
pins its own libultraship, so each has its own copy of the change: [starship/libultraship-gles2.patch](starship/libultraship-gles2.patch)
(libultraship `eaaf9d0`), [soh/libultraship-gles2.patch](soh/libultraship-gles2.patch) (libultraship `da6daf1`,
with fixes for 32-bit ARM and speed work measured on the console) and
[spaghettikart/libultraship-gles2.patch](spaghettikart/libultraship-gles2.patch) (libultraship `60c9bb7`: Ship of
Harkinian's, plus fewer draw calls) and [2ship/libultraship-gles2.patch](2ship/libultraship-gles2.patch)
(libultraship `7cb1022`: the same). These libultraship versions still use SDL2.

## Making a new port

A native game is any ARM program with a launch script in `roms/native/` (docs/native-games.md). For an SDL2 port:
build it with the RetroStoneOS cross compiler (`~/rsos/output/host/bin/arm-linux-`) against the staging
libraries (`~/rsos/output/staging`), use GLES2 for 3D, and either start `rsos-gamepad` for the built-in controls
as one SDL gamepad (`starship/Star Fox 64.sh`) or read them from `/dev/input` yourself (three devices:
"RetroStone2 Buttons", "RetroStone2 Shoulder Buttons" and "analog-stick"; `sm64/controller_rsos.c` is an
example). Put the game's code changes in a patch next to a build script, never the game's data.
