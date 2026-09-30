# libretro cores

RetroStoneOS has no RetroArch: the frontend `dlopen()`s the cores below through the libretro API. Every core is a
Buildroot package in `buildroot-external/package/libretro-<name>/` that installs:

- `/usr/lib/libretro/<core>_libretro.so`
- `/usr/share/rsos/cores/<core>.ini`: our own metadata (see [Core metadata files](#core-metadata-files))

All the cores render in software except the two N64 cores. For the software cores, the frontend shows their
framebuffers on a DRM plane with hardware scaling. The N64 cores render with GLES2 on the Mali-400 (Mesa lima), so the
frontend must implement libretro HW rendering for them only; see [Nintendo 64](#nintendo-64).

## Summary

| System | Core (package) | Upstream commit | Build flags (make) | Expected on A20 (2x A7 @ ~1 GHz) | BIOS | Known issues |
|---|---|---|---|---|---|---|
| NES, Famicom, FDS | fceumm (`libretro-fceumm`) | [`236ccdf`](https://github.com/libretro/libretro-fceumm/commit/236ccdfc911e84c60fea6b9d0699c2d440a8de14) 2026-08-22 | `-f Makefile.libretro platform=armv-neon-hardfloat` (-marm -mfloat-abi=hard, -O2, RGB565) | Full speed with a large margin (NES cores ran full speed on ~200 MHz ARM9 handhelds). | `disksys.rom` for FDS only | None known. The `unix` platform would force XRGB8888, which is why we use `armv`. |
| SNES / Super Famicom (**default**) | snes9x2005 (`libretro-snes9x2005`) | [`deb49d8`](https://github.com/libretro/snes9x2005/commit/deb49d80d1836e3e737480a326e31a54c46c04ae) 2026-07-22 | `-f Makefile platform=armv-hardfloat` (-marm -mfloat-abi=hard, -O2) | Full speed for the whole library, SuperFX and SA-1 included (Star Fox 2 holds 60 fps on a Pi 2 class A7). | none | Less accurate: old APU with timing hacks, no MSU-1 or BS-X. |
| SNES / Super Famicom (per game) | snes9x2010 (`libretro-snes9x2010`) | [`fe690dd`](https://github.com/libretro/snes9x2010/commit/fe690dd321fa5a46b5234a2bde089d2518c62b0e) 2026-09-21 | `-f Makefile.libretro platform=rpi2 LIBM=-lm` (-mcpu=cortex-a7 -mfpu=neon-vfpv4, -ffast-math, NEON tile renderer) | Full speed for most non-chip games; SuperFX/SA-1 titles drop to ~45-55 fps (Star Fox 2 intro: 47 fps on a Pi 2). | none | `rpi2` branch forgets `-lm` (link fails on `ceilf`), hence `LIBM=-lm`. |
| Mega Drive/Genesis, Master System, Game Gear, SG-1000, 32X, Mega-CD/Sega CD, Pico | picodrive (`libretro-picodrive`) | [`ab02114`](https://github.com/libretro/picodrive/commit/ab021146b70eef7ec0ac2afe06a94e9b4c16ef74) 2026-09-04 (git + submodules) | `-f Makefile.libretro platform=rpi2 ARCH=arm`, host `CYCLONE_CC/CXX`, `-Wl,-z,noexecstack` | MD/SMS/GG/Sega CD: full speed with a large margin (PicoDrive was written for 200 MHz ARM9). 32X: full speed for most games thanks to the SH-2 dynarec; a few heavy titles (Virtua Racing Deluxe, Virtua Fighter) may drop frames. | Sega CD: `bios_CD_U/E/J.bin` | TEXTREL in the .so (ARM asm, harmless with glibc). Needs `-z noexecstack` (see below). |
| Game Boy, Game Boy Color | gambatte (`libretro-gambatte`) | [`d9d6cd0`](https://github.com/libretro/gambatte-libretro/commit/d9d6cd06382d1ced30de34d56d3609452323dab1) 2026-08-21 | `-f Makefile.libretro platform=rpi2 HAVE_NETWORK=0` (-mcpu=cortex-a7 -mfpu=neon-vfpv4, -O2) | Full speed with a large margin. | optional `gb_bios.bin`, `gbc_bios.bin` | None known. |
| Game Boy Advance | gpsp (`libretro-gpsp`) | [`5819380`](https://github.com/libretro/gpsp/commit/5819380c2ffb0900219d700a382ee68c464ebb99) 2026-09-19 | `-f Makefile platform=rpi2` (ARM dynarec, `MMAP_JIT_CACHE=1`, -O3, -ffast-math) | Full speed for nearly all games (gpSP's dynarec was full speed on 400-600 MHz ARM handhelds). | optional `gba_bios.bin` (built-in open BIOS otherwise) | A few games need the official BIOS. |
| PlayStation | pcsx_rearmed (`libretro-pcsx-rearmed`) | [`ff81ed1`](https://github.com/libretro/pcsx_rearmed/commit/ff81ed17a15241d2f3730cdd7585b38e172532ca) 2026-09-24 | `-f Makefile.libretro platform=rpi2` (`DYNAREC=ari64`, `BUILTIN_GPU=neon`, `HAVE_NEON_ASM=1`, `NDRC_THREAD=1`, async GPU/SPU/CD, -O3) | Full speed for most 2D and many 3D games at native resolution, which is what the Pandora (1 GHz A8) and Pi 2 run. Heavy 3D titles may need frameskip. Use the second core: keep dynarec thread and GPU thread on. Keep NEON "enhanced resolution" off. | optional `scph5500/5501/5502.bin` (HLE BIOS otherwise) | HLE BIOS: no memory card manager, some games fail. |
| Arcade (MAME 0.78 romset) | mame2003_plus (`libretro-mame2003-plus`) | [`546424f`](https://github.com/libretro/mame2003-plus-libretro/commit/546424f3cd46674e996867ac26bd1060011739df) 2026-09-24 | `-f Makefile platform=rpi2 USE_CYCLONE=1 USE_DRZ80=1`, `-Wl,-z,noexecstack` (-O2, -ffast-math) | 80s boards and most 16-bit boards at full speed. Cyclone/DrZ80 are used automatically for the drivers on the core's compatibility list. 90s 3D/DSP-heavy boards (Midway T/X/Wolf units, Seta 2, ...) are too slow. | zip BIOS sets next to the games (`neogeo.zip`, ...) | TEXTREL (Cyclone/DrZ80 asm). 28 MB stripped. |
| Arcade (current FBNeo romset), Neo Geo | fbneo (`libretro-fbneo`) | [`aceeebe`](https://github.com/libretro/FBNeo/commit/aceeebed9e7edc8a28652365a064baee9a16e274) 2026-09-23 | `-C src/burner/libretro platform=rpi2` (`USE_CYCLONE=1`, `HAVE_NEON=1`, -O3), `-Wl,-z,noexecstack` | Neo Geo, CPS-1, most CPS-2, Toaplan, Cave, Sega System 16/18: full speed on Pi 2/3 class hardware with Cyclone. Expect the same here, with some CPS-2/Cave titles at the limit. CPS-3, PGM2 and other 32-bit boards are too slow. | `neogeo.zip` (FBNeo set) next to the games | 59 MB stripped. TEXTREL. Builds in about 2 minutes on the 16-core host (-j12). |
| Nintendo 64 (**default**) | parallel_n64 (`libretro-parallel-n64`), GLES2 | [`6e4c44c`](https://github.com/libretro/parallel-n64/commit/6e4c44c51885c8dc16e46d68464c517e6fca6712) 2026-09-19 | `platform=unix ARCH=arm WITH_DYNAREC=arm HAVE_NEON=1 GLES=1 GL_LIB=-lGLESv2 CPUFLAGS="-DNO_ASM -DARM -D__arm__ -DARM_ASM -D__NEON_OPT -DNOSSE -DARM_FIX"`, `-Wl,-z,noexecstack` (-Ofast) | **A subset of games only.** Light titles run close to full speed with frameskip; many 3D games are slow. See [N64 performance](#n64-performance). | none | Needs GLES2 HW rendering in the frontend. Upstream default renderer (gliden64) is not built for GLES2; our defaults: rice, auto frameskip (RetroStoneOS patch 0001). |
| Nintendo 64 (per game, experimental) | mupen64plus_next (`libretro-mupen64plus-next`), GLES2 | [`6752836`](https://github.com/libretro/mupen64plus-libretro-nx/commit/6752836de8b224febfd5708444755b77712ac939) 2026-09-12 | `platform=rpi2-mesa ARCH=arm` (GLES2 + EGL, ari64, NEON, -mcpu=cortex-a7 -mfpu=neon-vfpv4), `-U_LARGEFILE64_SOURCE`, `-Wl,-z,noexecstack` | Slower than parallel_n64 (GLideN64 is heavier); for games the light renderers draw wrong. Unproven on lima. | none | GLideN64 GLES2 path runs at fp16 on Mali-400. Complex combiner shaders may hit lima compiler limits. Experimental. |
| PC Engine / TurboGrafx-16, PC Engine CD | mednafen_pce_fast (`libretro-beetle-pce-fast`) | [`1c693c6`](https://github.com/libretro/beetle-pce-fast-libretro/commit/1c693c630121366f0b819d785762f1ce5b3a68d0) 2026-09-25 | `-f Makefile platform=rpi2` (-DARM -mcpu=cortex-a7 -mfpu=neon-vfpv4, -O2, -ffast-math, CHD) | Full speed, HuCard and CD (Pi 2 class hardware runs it full speed; the "fast" core was written for that). CD-DA from .ogg tracks costs a little extra (Tremor decode). | `syscard3.pce` for CD games | No SuperGrafx (.sgx): that is mednafen_supergrafx ([third batch](#third-batch-computers-fantasy-consoles-and-game-engines)). |
| Atari 2600 | stella2014 (`libretro-stella2014`) | [`7d1361e`](https://github.com/libretro/stella2014-libretro/commit/7d1361e407e63f29e52892655069e5fb4096e691) 2026-09-04 | `-f Makefile platform=rpi2` (-mcpu=cortex-a7 -mfpu=neon-vfpv4, -O2) | Full speed with a large margin (Stella 3.9 ran on 400 MHz-class ARM). | none | Stella 3.9.3 (2014): a few late homebrew bankswitch schemes are missing. See [Atari 2600: why stella2014](#atari-2600-why-stella2014). |
| Atari 7800 | prosystem (`libretro-prosystem`) | [`8a88014`](https://github.com/libretro/prosystem-libretro/commit/8a88014287c7a01cd568067e5a557d0a2b2a051f) 2026-08-22 | `-f Makefile platform=rpi2` (-DARM -mcpu=cortex-a7, -O2, -ffast-math, -fsigned-char) | Full speed with a large margin. | optional `7800 BIOS (U).rom` / `(E).rom` | None known. |
| Atari Lynx | handy (`libretro-handy`) | [`bc55d46`](https://github.com/libretro/libretro-handy/commit/bc55d462f0b2d6b073ea93dc552ebd73cec60fd1) 2026-04-20 | `-f Makefile platform=unix` (no ARM platform in the Makefile; CPU flags from the toolchain wrapper, -O2) | Full speed with a large margin (Handy ran on 200 MHz-class handhelds). | optional `lynxboot.img` (HLE boot otherwise) | None known. |
| Neo Geo Pocket / Color | mednafen_ngp (`libretro-beetle-ngp`) | [`a50d5ac`](https://github.com/libretro/beetle-ngp-libretro/commit/a50d5ac288a81f2104ddf43195a4efdd15c72227) 2026-06-14 | `-f Makefile platform=rpi2` (-mcpu=cortex-a7, -O2, -ffast-math) | Full speed with a large margin. | none (HLE BIOS) | None known. |
| WonderSwan / Color | mednafen_wswan (`libretro-beetle-wswan`) | [`4b01295`](https://github.com/libretro/beetle-wswan-libretro/commit/4b01295838ea89e3f1355bbe4cb5cf98aa6108cd) 2026-07-31 | `-f Makefile platform=rpi2` (-mcpu=cortex-a7, -O2, -ffast-math) | Full speed with a large margin. | none | Vertical games (`wswan_rotate_display`): the core asks for `RETRO_ENVIRONMENT_SET_ROTATION` and rotates in software if the frontend refuses it. handy does the same for rotated Lynx games. |
| Master System, Game Gear, SG-1000 (GPL) | gearsystem (`libretro-gearsystem`) | [`0e35ec6`](https://github.com/drhelius/Gearsystem/commit/0e35ec681bac7850d1518057aac38e4522066e16) 2026-09-16 | `-C platforms/libretro platform=rpi2` (-O3, C++) | Full speed; heavier than PicoDrive and SMS Plus GX (C++, more exact timing) but a 3.58 MHz Z80 leaves a wide margin on a 1 GHz A7. | optional `bios.sms`, `bios.gg` | None known. |
| Master System, Game Gear, SG-1000, ColecoVision (GPL, lightest) | smsplus (`libretro-smsplus-gx`) | [`3844b46`](https://github.com/libretro/smsplus-gx/commit/3844b46caa926b6494987b97da63092818c4ddef) 2026-09-04 | `-f Makefile.libretro platform=rpi2` (-DARM -mcpu=cortex-a7, -O2, -ffast-math) | Full speed with a large margin (SMS Plus GX targets 200-400 MHz MIPS/ARM handhelds; its tree even has standalone RetroStone and RetroStone2 ports). | optional `bios.sms`; `BIOS.col` for ColecoVision | Less exact than Gearsystem (a few timing-sensitive games). |
| Game Boy, Game Boy Color (GPL-3, per game) | gearboy (`libretro-gearboy`) | [`1f2ef68`](https://github.com/drhelius/Gearboy/commit/1f2ef68a5a43f2b6cdaa4fdf1b462c772d5d31d3) 2026-09-25 | `-C platforms/libretro platform=rpi2` (-O3, C++) | Full speed. | optional `dmg_boot.bin`, `cgb_boot.bin` | Alternative only: gambatte stays the default. |
| ColecoVision (+ ADAM) | gearcoleco (`libretro-gearcoleco`) | [`8d32d24`](https://github.com/drhelius/Gearcoleco/commit/8d32d242c6ba2104d75c17bb79773275387d4c37) 2026-09-16 | `-C platforms/libretro platform=rpi2` (-O3, C++) | Full speed with a large margin. | `colecovision.rom` (required) | None known. |
| MSX, MSX2, MSX2+, turbo R | bluemsx (`libretro-bluemsx`) | [`e3086eb`](https://github.com/libretro/blueMSX-libretro/commit/e3086eb5d36d77fa11704cf53dc176686e70127d) 2026-08-23 | `-f Makefile.libretro platform=rpi2` (-DARM -mcpu=cortex-a7, -O2, -ffast-math, `-std=gnu89`) | MSX1/MSX2 cartridges: full speed expected (lr-bluemsx is full speed on a Pi 2). turbo R and heavy sound expansions (MoonSound, FM-PAC + SCC) are the demanding cases. TODO(hw): measure. | none with the shipped C-BIOS; original MSX system ROMs for disk/tape games | C-BIOS runs cartridges only. See [MSX: blueMSX system files](#msx-bluemsx-system-files). |

"Expected on A20" is extrapolated from Raspberry Pi 2 (4x Cortex-A7 @ 900 MHz), H3 boards (4x A7 @ 1.2 GHz) and
older ARM handhelds, because single-thread speed is what matters. The A20 has the same core at a similar clock, but
its DDR3 controller is slow (linux-sunxi measurements), so memory-bound cores (PS1, arcade) may be a bit slower than on
a Pi 2. TODO(hw): measure each core on the RetroStone2 (frame time over a few minutes of a demanding title).

## SNES: snes9x2005 or snes9x2010

| | snes9x2005 | snes9x2010 |
|---|---|---|
| Base | Snes9x 1.43 (CATSFC) | Snes9x 1.52 (snes9x-next) |
| Audio | old Snes9x APU, timing hacks | blargg SPC700/DSP, accurate |
| Special chips | SuperFX, SA-1, DSP-1 (with DSP-2/4 code), C4, S-DD1, SPC7110, S-RTC, OBC1, Seta 010/011/018 | same set on a newer, more accurate code base, plus BS-X (Satellaview) and MSU-1 |
| Speed | fastest Snes9x; SuperFX games at 60 fps on a Pi 2 | 20-40 % slower; SuperFX/SA-1 games drop below 60 fps on a Pi 2 |
| .so size (stripped, our build) | 0.6 MB | 2.3 MB |

**Recommendation for the 1 GHz A7: snes9x2005 as the default SNES core**, and snes9x2010 as a per-game choice for
the few titles 2005 cannot run or gets wrong (MSU-1 hacks, Satellaview dumps, games with glitches or with music that
sounds wrong on the old APU). The deciding factor is frame pacing. A handheld drops or stutters audio as soon as a
frame takes longer than 16.7 ms. The A20 is no faster than a Pi 2 core for core, and snes9x2010 already misses 60 fps
there in the SuperFX titles that people play (Star Fox, Yoshi's Island). Both packages are small, so ship both.
TODO(hw): time snes9x2010 on a few non-chip games on the RetroStone2. If it holds 60 fps with margin, it could become
the default, with snes9x2005 kept for SuperFX/SA-1.

## ROM folders

Recommended layout: `/data/roms/<folder>`. The frontend maps a folder to a system and to a default core.

| Folder | System | Default core | Alternative |
|---|---|---|---|
| `nes` | NES / Famicom | fceumm | |
| `fds` | Famicom Disk System | fceumm | |
| `snes` | SNES / Super Famicom | snes9x2005 | snes9x2010, mednafen_supafaust (experimental) |
| `megadrive` | Mega Drive / Genesis | picodrive | clownmdemu (experimental) |
| `mastersystem` | Master System | picodrive | gearsystem, smsplus |
| `gamegear` | Game Gear | picodrive | gearsystem, smsplus |
| `sg1000` | SG-1000 | picodrive | gearsystem, smsplus |
| `sega32x` | 32X | picodrive | |
| `segacd` | Mega-CD / Sega CD | picodrive | clownmdemu (experimental) |
| `gb` | Game Boy | gambatte | gearboy |
| `gbc` | Game Boy Color | gambatte | gearboy |
| `gba` | Game Boy Advance | gpsp | |
| `psx` | PlayStation | pcsx_rearmed | |
| `arcade` | Arcade, MAME 0.78 (2003-Plus) romset | mame2003_plus | |
| `fbneo` | Arcade, current FBNeo romset | fbneo | |
| `neogeo` | Neo Geo (FBNeo romset, `neogeo.zip` in the folder) | fbneo | geolith (experimental, `.neo` files only) |
| `n64` | Nintendo 64 | parallel_n64 | mupen64plus_next (experimental) |
| `pcengine` | PC Engine / TurboGrafx-16 (HuCard) | mednafen_pce_fast | |
| `pcenginecd` | PC Engine CD / TurboGrafx-CD (`.cue`/`.chd`/`.m3u`) | mednafen_pce_fast | |
| `atari2600` | Atari 2600 | stella2014 | |
| `atari7800` | Atari 7800 | prosystem | |
| `atarilynx` | Atari Lynx | handy | |
| `ngp` | Neo Geo Pocket | mednafen_ngp | |
| `ngpc` | Neo Geo Pocket Color | mednafen_ngp | |
| `wonderswan` | WonderSwan | mednafen_wswan | |
| `wonderswancolor` | WonderSwan Color | mednafen_wswan | |
| `coleco` | ColecoVision | gearcoleco | smsplus, bluemsx |
| `msx` | MSX / MSX2 / MSX2+ / turbo R | bluemsx | |
| `neocd` | Neo Geo CD (`.cue`/`.chd`) | geolith (experimental) | |
| `supergrafx` | PC Engine SuperGrafx (`.sgx`) | mednafen_supergrafx | |
| `zxspectrum` | ZX Spectrum | fuse | |
| `amstradcpc` | Amstrad CPC / 6128+ | cap32 | |
| `c64` | Commodore 64 | vice_x64 | |
| `dos` | MS-DOS (one `.zip` per game) | dosbox_pure | |
| `scummvm` | ScummVM games (one folder per game with a `.scummvm` file) | scummvm | |
| `pico8` | PICO-8 carts (`.p8`, `.p8.png`) | fake08 | |
| `pokemini` | Pokémon mini | pokemini | |
| `doom` | Doom engine IWADs/PWADs (Freedoom pre-installed) | prboom | |
| `cavestory` | Cave Story (`Doukutsu.exe` + `data/`) | nxengine | |
| (none: `/usr/share/rsos/games/retrostone/`) | RetroStone: 8BCraft's own games, built in (no ROM files; [RetroStone](#retrostone-the-retrostone-vc-games)) | bombermole, leadysquid (one core per game) | |

`pcengine` and `pcenginecd` also offer mednafen_supergrafx as a per-game alternative. The third-batch folders and
how to fill them: [Third batch](#third-batch-computers-fantasy-consoles-and-game-engines).

For a licence-clean image (see [Licence-clean alternatives](#licence-clean-alternatives)), `mastersystem`, `gamegear`
and `sg1000` default to gearsystem instead of picodrive.

Arcade romsets are version-specific: a MAME 2003-Plus set usually does not load in FBNeo and the reverse. That is why
`arcade` and `fbneo` are separate folders instead of one `arcade` folder with a core switch.

BIOS files go to the libretro system directory that the frontend returns for `RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY`.
We suggest `/data/bios/`. Arcade BIOS zips stay in the ROM folder.

## GPU or software

| Core | Rendering | GLES needed |
|---|---|---|
| fceumm, snes9x2005, snes9x2010, picodrive, gambatte, gpsp | software | no |
| pcsx_rearmed | software (NEON GPU renderer, `gpu_neon`) | no |
| mame2003_plus, fbneo | software | no |
| mednafen_pce_fast, stella2014, prosystem, handy, mednafen_ngp, mednafen_wswan, gearsystem, smsplus, gearboy, gearcoleco, bluemsx | software | no |
| mednafen_supafaust (PPU on a second thread), clownmdemu, geolith (experimental) | software | no |
| prboom, fake08, dosbox_pure, scummvm, nxengine, mednafen_supergrafx, fuse, cap32, pokemini, vice_x64 | software (scummvm built without OpenGL) | no |
| parallel_n64, mupen64plus_next | GLES2 through `RETRO_ENVIRONMENT_SET_HW_RENDER` (`RETRO_HW_CONTEXT_OPENGLES2`) | **yes** |

Only the two N64 cores use HW rendering. For everything else the frontend never creates an EGL/GLES context and scans
out the core's framebuffer on a DRM plane.

Pixel formats the frontend must accept (from `RETRO_ENVIRONMENT_SET_PIXEL_FORMAT`): RGB565 (all cores, the default
we build for), XRGB8888 (pcsx_rearmed for 24-bit video, mame2003_plus and fbneo for 32-bit drivers) and 0RGB1555
(mame2003_plus for some drivers, and the libretro default if a core sets nothing).

## Nintendo 64

N64 did run on this exact device under RetrOrangePi. Many games were slow, but some were playable. We ship two GLES2
libretro cores built along the same lines, adapted to mainline Mesa **lima** (no Mali blob).

### What RetrOrangePi used

From its RetroPie-Setup fork (`scriptmodules/emulators/mupen64plus.sh`, `libretrocores/lr-parallel-n64.sh`,
`lr-mupen64plus.sh`):

- **Standalone mupen64plus** (not libretro) with GLES2 video plugins written for weak GPUs: ricrpi
  `video-gles2n64`, ricrpi `video-gles2rice` (`pandora-backport` branch) and `video-glide64mk2`. It was built with
  `VFP=1 USE_GLES=1 VFP_HARD=1 HOST_CPU=armv7 NEON=1`, which gives the ARMv7 dynarec and NEON.
- **lr-parallel-n64** with `GLES=1 GL_LIB:=-lGLESv2`, `WITH_DYNAREC=arm`, `HAVE_NEON=1` and
  `CPUFLAGS="-DNO_ASM -DARM -D__arm__ -DARM_ASM -D__NEON_OPT -DNOSSE -DARM_FIX"`. Core options: gfx plugin "auto",
  accuracy "low", 640x480. A `gles2n64rom.conf` with per-game gln64 settings (mostly `target FPS` frameskip targets
  of 17-27 fps, plus framebuffer and Zelda/Banjo hacks) went into the BIOS directory.
- **lr-mupen64plus**, the 2017-2019 libretro "mupen64plus + GLideN64" core (`libretro/mupen64plus-libretro`,
  archived; RetroPie kept a fork until 2020), built with `FORCE_GLES=1`.

The ricrpi GLES2 plugins come from the same code as parallel-n64's `gles2n64` and `gles2rice` renderers, and glide64mk2
is its `glide64`. So parallel-n64 bundles, in one libretro core, the renderers RetrOrangePi relied on.

### What we ship

**`libretro-parallel-n64` (default N64 core).** It is the RetrOrangePi recipe on the generic `unix` platform, so
nothing Raspberry Pi specific is pulled in. The flags are listed in the summary.

- Rendering: `GLES=1` gives `-DHAVE_OPENGLES2`, and the core asks for a `RETRO_HW_CONTEXT_OPENGLES2` context (depth
  buffer, no stencil, bottom-left origin). The Makefile itself drops GLideN64 on GLES2 builds, because in this core
  GLideN64 needs GL 3.3 or GLES 3. The build keeps **glide64**, **gln64** (gles2n64) and **rice**. Angrylion
  (software RDP) is also built but is far too slow. paraLLEl-RDP needs Vulkan and is not built.
- CPU: ari64 `new_dynarec` with the ARM backend (`NEW_DYNAREC=3`), NEON paths, HLE RSP.
- **RetroStoneOS patch** (`0001-libretro-count-per-op-and-frameskip-core-options.patch`, applied by Buildroot): two
  core options this fork does not have, `parallel-n64-CountPerOp` and `parallel-n64-frameskip`, and a startup fix (the
  `"auto"` GFX-plugin test had no braces and swallowed the gln64 test after it). See [N64 performance](#n64-performance).
- The package `[options]` and `frontend/src/host/coreopts/parallel_n64.ini` set the defaults (the table in
  [N64 performance](#n64-performance)), because the upstream defaults (`gliden64`, accuracy `veryhigh`) do not fit a
  GLES2 build on Mali-400: `gliden64` is not built here, so the core would silently fall back to glide64.
- `gles2n64rom.conf` is installed to `/usr/share/rsos/cores/parallel_n64/`. The `.ini` lists it under `system_files`.
  The frontend copies it into the libretro system directory before the first N64 game, if it is not there yet. gln64
  reads it from `<system dir>/gles2n64rom.conf`. **It was not doing what we thought**: this libretro port only reads
  ten keys (`video width/height`, `enable noise`, `enable lod`, `texture max anisotropy`, `texture use IA`,
  `texture fast CRC`, `bilinear mode`, `hack alpha`, `hack z`; `gles2n64/src/glN64Config.c`) and ignores the rest
  without a message. Everything the RetrOrangePi file tuned (`target FPS` frameskip targets, `framebuffer enable`,
  `update mode`, `hack zelda`, `hack banjo tooie`, ...) belongs to the standalone ricrpi plugin and was a no-op. The
  shipped file now documents that and keeps only valid keys; frameskip is the core option, for every renderer.

**`libretro-mupen64plus-next` (experimental, per game).** Built with `platform=rpi2-mesa`, the upstream recipe for a
Pi 2 (Cortex-A7) with a Mesa GLES2 driver: GLideN64 through its GLES2 path, EGL, ari64 and NEON. The mupen64plus-next
Makefile no longer offers the old light renderers (gln64/rice/glide64): only GLideN64, angrylion and paraLLEl.

Is GLideN64's GLES2 path still usable? The pinned source was checked.

- **Hard requirement: GLES 2.0 only.** `GLInfo::init()` forces `isGLES2` when built with `HAVE_OPENGLES2`. It then
  turns off what GLES2 cannot do instead of failing: no MSAA, no image textures, no fragment depth write, no hybrid
  filter, no async RDRAM colour copies, no N64 depth compare, no EGL-image framebuffers. The GLSL generators emit
  `#version 100` with `#ifndef GL_FRAGMENT_PRECISION_HIGH` → `precision mediump float`. That matters because the
  Mali-400 fragment processor only has fp16.
- **Optional GLES2 extensions it probes:**
  - `GL_OES_depth_texture` for depth buffer emulation
  - `GL_EXT_shader_texture_lod` + `GL_OES_standard_derivatives` for LOD emulation
  - `GL_OES_get_program_binary` for the shader cache
  - `GL_EXT_draw_elements_base_vertex`, `GL_EXT_texture_filter_anisotropic`

  In the Mesa 26.0.1 source we build, all of the first four are exposed on lima: `OES_depth_texture`,
  `OES_get_program_binary`, `OES_element_index_uint` and `OES_packed_depth_stencil` are always on for ES2, and lima
  sets `fragment_shader_derivatives` and `fragment_shader_texture_lod`. So the extension set is enough.
- **Risks** (why it stays experimental):
  - fp16 precision artifacts in the combiner, such as texture coordinate wobble on large textures
  - lima's `ppir` compiler can fail register allocation on very long fragment shaders
  - GLideN64 does far more per-pixel work than glide64/gln64
  - GLideN64 calls `eglGetDisplay(EGL_DEFAULT_DISPLAY)` / `eglQueryString` itself to probe EGL extensions, so the
    frontend should export `EGL_PLATFORM=gbm` (or `surfaceless`) before loading the core
  - the Raspberry Pi VideoCore IV builds, the nearest comparison, were already marginal

  Our `mupen64plus_next.ini` uses the core's own VideoCore defaults and the cheapest settings: native 320x240 (no
  resolution factor), no framebuffer emulation (GLideN64's own per-game ini still turns it on for the games that
  need it: `GLideN64IniBehaviour = late`), no RDRAM colour/depth/aux copies, no N64 depth compare (not available on
  GLES2 anyway), no LOD, no hybrid filter or dithering, legacy blending, shader storage on, frame duping on. The
  threaded renderer (GLideN64 on the second CPU core) now works with our frontend (it needed RetroArch's
  `GET_CLEAR_ALL_THREAD_WAITS_CB`, which the host answers with a no-op, and a lock around the audio buffer): off by
  default until measured, the benchmark tries it. TODO(hw): check that GLideN64's shaders compile on lima and measure
  it against parallel_n64 (the benchmark does both).

**Old libretro `mupen64plus` (RetroPie's lr-mupen64plus): not a better fit.** It is an older snapshot of the same
thing: GLideN64 and angrylion, with no light renderers. It has been unmaintained since 2019-2020 and needs patches for
GCC 10+ and newer Mesa headers. Its advantages on Mali-400 are exactly the GLideN64 GLES2 path that mupen64plus-next
still has, maintained. The light renderers RetrOrangePi relied on live in parallel-n64, so we do not package it.

### Expected performance (honest)

The CPU side (ari64 dynarec + HLE RSP + audio) needs roughly the power of a Pi 2 at 900 MHz to 1.2 GHz for light
games. The A20 has two A7 cores at ~1 GHz, slower RAM and a weaker GPU on an open driver.

- **Likely playable** (near full speed, or steady with frameskip): light and 2D-heavy titles. These are
  candidates from Pi 2 and RetrOrangePi experience and from the games the gles2n64rom.conf tunes, not measurements on
  this device: Super Mario 64, Mario Kart 64, Star Fox 64, Wave Race 64, Diddy Kong Racing, Mario Party, Paper Mario,
  Kirby 64, Bomberman 64, Dr. Mario 64.
- **Slow**: the demanding 3D titles: Zelda OoT/MM, GoldenEye/Perfect Dark, Banjo-Tooie, Conker, Rogue Squadron,
  World Driver Championship, and any game that needs framebuffer effects.

This matches what the hardware owner saw under RetrOrangePi ("many games slow, some playable"). It is a feature for
the handful of good cases, not full N64 support. TODO(hw): build a per-game list (playable / core / renderer)
on the RetroStone2 and turn it into per-game option files (the benchmark writes them).

### N64 performance

The owner's report: N64 works but is "very slow" (glide64, 320x240, no frameskip). What was checked, in the pinned
sources and in the known setups for weak ARM + GLES2 devices:

- **Renderer.** Lakka's parallel-n64 notes rank the three GLES2 renderers: glide64 "medium accuracy and speed",
  rice "fast (use this on RPi)", gln64 "fast (broken on GLES2)" ([lakka.tv/doc/Nintendo-64](https://www.lakka.tv/doc/Nintendo-64/)).
  RetroPie's Pi experience with the standalone plugins is the same order: gles2n64 fastest (Mario Kart 64 full speed
  on a Pi), glide64 slower with audio stutter, GLideN64 most accurate but only playable at low resolution
  ([RetroPie-Setup #1511](https://github.com/RetroPie/RetroPie-Setup/issues/1511),
  [petrockblock: mupen64 video plug-ins](https://www.petrockblock.com/forums/topic/mupen64-video-plug-ins/)). The
  N64 renderers do their vertex work on the CPU (HLE), and on the A20 the CPU is the bottleneck, so the lighter
  renderer matters more than the GPU. **Default: rice**; glide64 and gln64 are per-game choices.
- **Frameskip.** This parallel-n64 build had none: no core option, the frontend's "video disabled" flag only hides
  the frame after it was drawn, and the gles2n64rom.conf `target FPS` keys are dead (above). **Patch 0001** adds
  `parallel-n64-frameskip` (`0`, `1`, `2`, `3`, `auto`) in front of glide64, gln64 and rice, the way the standalone
  gles2n64 did it: the decision is taken at each game frame boundary (the VI origin changes); a skipped frame's
  display lists are not handed to the renderer at all (no CPU vertex work, no GPU work), the DP interrupt the
  renderer would raise is raised instead, and its UpdateScreen is not forwarded, so the core presents a dupe. A game
  that stops swapping buffers stops skipping after 8 VIs. `auto` skips (at most 3 in a row) while the frontend's
  audio buffer is below 40 % (`RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK`, which the host calls before every
  frame): no effect while the game keeps up. **Default: auto.** TODO(hw): watch for games that freeze or flicker with
  frameskip on (turn it off per game: Select+X > Core options > Frameskip > Off).
- **Count Per Op.** Also missing: the core only used the ROM database (`mupen64plus.ini.h`: 2 for most games, 1 or
  3 for some). Patch 0001 adds `parallel-n64-CountPerOp` (`0` = database, `1`-`4`), read at startup. 3 gives the
  game fewer emulated cycles per frame, so the dynarec does less work: faster, but a game that was already busy slows
  down in-game and a few break (mupen64plus-next has the same option with the same warning). **Default: 0**
  (database); the benchmark tries 3 per game.
- **Resolution.** `320x240` stays: the plane scales to the LCD; `640x480` costs 4x the fill rate and the GLES2 FBO
  copy. The benchmark measures it.
- **The second CPU core.** Everything (dynarec, HLE audio/graphics, the GL driver) runs on one thread; the A20 has
  two cores. Two ways to use the second one, both measured by the benchmark: Mesa's GL worker thread (`glthread`,
  host key `rsos-glthread = true`: the host exports `mesa_glthread=true` before the EGL context exists; Mesa 26
  disables glthread by default on machines with fewer than four CPUs, `src/gallium/frontends/dri/dri_context.c`,
  but honours the variable for any GL API, GLES2 included), and mupen64plus-next's threaded GLideN64 (above).
- Unchanged: accuracy `low` (only glide64 reads it), HLE RSP (LLE `cxd4` is far too slow), ari64 dynarec,
  `virefresh = auto` (1500/2200 give the game *more* cycles per VI: slower here).

| Option | Default | Per-game alternatives |
|---|---|---|
| `parallel-n64-gfxplugin` | `rice` | `glide64` (more compatible, heavier), `gln64` (lightest, broken in places) |
| `parallel-n64-frameskip` | `auto` | `0` (off), `1`-`3` (fixed: N of N+1 frames skipped) |
| `parallel-n64-CountPerOp` | `0` (ROM database) | `3` (faster, may slow the game down in-game) |
| `parallel-n64-screensize` | `320x240` | `640x480` |
| `parallel-n64-gfxplugin-accuracy` | `low` | `medium` |
| `parallel-n64-rspplugin`, `-cpucore`, `-virefresh` | `hle`, `dynamic_recompiler`, `auto` | |
| `rsos-glthread` (host key) | off | `true` (Mesa GL thread on the second core) |

**Benchmark mode** (docs/host-design.md §18, how-to in docs/bringup.md 7b). In a game, Select+X > "Benchmark this
game": the game restarts from that exact moment (a save state) once per configuration of
`/usr/share/rsos/coreopts/n64.bench.ini` (rice, glide64, gln64, rice + Count Per Op 3, rice and glide64 with
frameskip 1, rice + GL thread, rice 640x480, mupen64plus-next GLideN64 and threaded, when installed), each for 5 s
warm-up + 25 s measured, no input, unthrottled. The report goes to `/data/rsos/logs/bench-<game>-<time>.txt` (the SD
card's `rsos/logs/`) with a screenshot per run; the results page ranks the runs (full speed first, then the most
frames shown) and "Use this for this game" writes the winner's options to the per-game file. A mupen64plus-next run
cannot use parallel-n64's state: it starts from boot and is listed but not ranked.

**What to expect** (reasoning, to be confirmed with the benchmark on the device). The A20's A7 at 960 MHz is about a
Pi 2 core, with a slower 32-bit DDR3 at 384 MHz that the display also reads, and lima costs CPU time per draw call.

- Likely full speed with rice (auto frameskip absorbing the peaks): Mario Kart 64 (1 player), Super Mario 64, Dr.
  Mario 64, Bomberman 64, Puzzle League, Mario Party 1-3 (boards; a few mini-games use framebuffer effects), Kirby 64
  and Diddy Kong Racing with some skipping, Star Fox 64 and Wave Race 64 at the edge.
- Probably not: Zelda OoT/MM (heavy CPU, F3DZEX2, the pause screen and Majora's blur need framebuffer emulation that
  the light renderers lack), GoldenEye / Perfect Dark (CPU, TLB, Perfect Dark's framebuffer effects), Banjo-Tooie,
  Conker, Donkey Kong 64 (Expansion Pak, heavy), F-Zero X (60 fps game logic), and the games with custom microcodes
  the HLE renderers do not know (Rogue Squadron, Battle for Naboo, Indiana Jones, World Driver Championship): they
  need the LLE RSP, which is far too slow here.
- Next levers if a game is close: the DRAM clock (384 MHz; A20 boards commonly run 432-480 MHz, a U-Boot setting,
  board owner), a CPU boost (1008-1200 MHz OPPs with the matching voltage and a thermal check), the Mali clock, and
  profiling (`perf top` on the device: dynarec vs renderer vs lima vs kernel; the benchmark's core-ms vs swap-ms split
  is the first hint).

### Frontend requirement: GLES2 HW rendering

The two N64 cores call `RETRO_ENVIRONMENT_SET_HW_RENDER` with `context_type = RETRO_HW_CONTEXT_OPENGLES2`. The
libretro host must:

1. **Initialise EGL only when an N64 game starts**, not at boot. Open the DRM device already used for KMS, create a
   `gbm_device` on it, get an `EGLDisplay` for the GBM platform (`eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL)`),
   `eglInitialize`, `eglBindAPI(EGL_OPENGL_ES_API)`, and create a GLES 2.0 context
   (`EGL_CONTEXT_CLIENT_VERSION = 2`).
2. **Render target.** Create a `gbm_surface` (XRGB8888/RGB565, `GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING`) and an
   EGL window surface on it. Alternatively, use a surfaceless context with an FBO whose colour attachment is a
   GBM BO imported through `EGL_KHR_image_base`. Either way, provide the core's FBO: `get_current_framebuffer()`
   returns the FBO the core draws into, with a depth attachment (`hw_render.depth = true`, no stencil).
   `get_proc_address()` is `eglGetProcAddress`. Honour `bottom_left_origin = true` (the image is flipped: flip on
   the final blit, or scan out with a reflect-y plane property if the plane supports it).
3. **Callbacks.** Call `context_reset()` once the context is current, before the first `retro_run()`. Call
   `context_destroy()` before tearing the context down (game exit, or HDMI/LCD switch if the GBM surface has to be
   recreated).
4. **Present.** When the core calls `video_refresh(RETRO_HW_FRAME_BUFFER_VALID, w, h, pitch)`, blit or scale the FBO
   into the GBM surface's back buffer if needed, `eglSwapBuffers`, then `gbm_surface_lock_front_buffer()`, wrap the BO
   in a DRM framebuffer (`drmModeAddFB2`, cached per BO) and put it on the KMS plane with a page flip or atomic commit.
   Release the previous BO after the flip completes. The plane still does the final scaling to 640x480 or HDMI, as
   for the software cores.
5. **Teardown** after the game: destroy the EGL surface, context and display, and the GBM surface and device. Return
   to the software/dumb-buffer path so the menu and the other cores never touch the GPU.

Lima needs the `lima` and `sun4i-drm` kernel drivers and Mesa's `lima` Gallium driver with GBM and EGL. The main
defconfig already has these (`BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_LIMA`, `_OPENGL_EGL`, `_OPENGL_ES`, GBM is selected).

## Core metadata files

`/usr/share/rsos/cores/<core>.ini` is a plain INI file. `;` starts a comment, keys are lowercase, and lists are
comma-separated (spaces around items are ignored).

```ini
[core]
id = fceumm                       ; <id>_libretro.so, also the file name of this .ini
display_name = FCEUmm
library = /usr/lib/libretro/fceumm_libretro.so
commit = <40-char upstream commit>
license = GPL-2.0+
systems = nes, fds                ; ROM folder names (see "ROM folders") this core can run
default_systems = nes, fds        ; optional: systems for which this core is the automatic default (the host
                                  ; tries this key first, then its built-in "ROM folders" table, then the first
                                  ; non-experimental core; docs/host-design.md). The third-batch cores set it.
extensions = nes, fds, unf, unif  ; lowercase, no dot (same list as retro_system_info.valid_extensions)
need_fullpath = true              ; retro_system_info.need_fullpath: pass a path, not a memory buffer
block_extract = false             ; true: hand .zip/.7z to the core as-is (arcade)
savestates = true                 ; retro_serialize() is implemented
renderer = software               ; software | gles2 (gles2: needs SET_HW_RENDER, see "Nintendo 64")
pixel_format = rgb565             ; formats the core may request, first = usual one
system_files = <abs path>, ...    ; optional: copy into the system directory if missing (parallel_n64)
no_content = false                ; true: the game is built into the core (RetroStone VC). The menu entry is
                                  ; a stub file in /usr/share/rsos/games/<system>/ that is never read:
                                  ; retro_load_game(NULL), the saves named after it (docs/vc-games.md)

[bios:disksys.rom]                ; one section per file, path relative to the system directory
md5 = ca30b50f880eb660a320674ed365ef7a   ; absent for arcade zip sets
required = fds                    ; no | yes | <systems> for which the file is mandatory
description = Famicom Disk System BIOS (only needed for .fds images)

[options]                         ; optional: core option defaults (RETRO_ENVIRONMENT_GET_VARIABLE)
parallel-n64-gfxplugin = rice     ; applied unless the user or a per-game file overrides them
```

BIOS checksums listed:

| File | MD5 | Core | Needed |
|---|---|---|---|
| `disksys.rom` | `ca30b50f880eb660a320674ed365ef7a` | fceumm | FDS only |
| `gb_bios.bin` | `32fbbd84168d3482956eb3c5051637f5` | gambatte | optional |
| `gbc_bios.bin` | `dbfce9db9deaa2567f6a84fde55f9680` | gambatte | optional |
| `gba_bios.bin` | `a860e8c0b6d573d191e4ec7db1b1e4f6` | gpsp | optional (built-in open BIOS) |
| `bios_CD_U.bin` | `2efd74e3232ff260e371b99f84024f7f` | picodrive | Sega CD (US discs) |
| `bios_CD_E.bin` | `e66fa1dc5820d254611fdcdba0662372` | picodrive | Mega-CD (EU discs) |
| `bios_CD_J.bin` | `278a9397d192149e84e820ac621a8edd` | picodrive | Mega-CD (JP discs) |
| `scph5501.bin` | `490f666e1afb15b7362b406ed1cea246` | pcsx_rearmed | optional (HLE BIOS) |
| `scph5500.bin` | `8dd7d5296a650fac7319bce665a6a53c` | pcsx_rearmed | optional |
| `scph5502.bin` | `32736f17079d0b2b7024407c39bd3050` | pcsx_rearmed | optional |
| `scph1001.bin` | `924e392ed05558ffdb115408c263dccf` | pcsx_rearmed | optional |
| `syscard3.pce` | `38179df8f4ac870017db21ebcbf53114` | mednafen_pce_fast, mednafen_supergrafx | PC Engine CD |
| `syscard2.pce` | `3cdd6614a918616bfc41c862e889dd79` | mednafen_pce_fast, mednafen_supergrafx | optional (core option) |
| `syscard1.pce` | `2b7ccb3d86baa18f6402c176f3065082` | mednafen_pce_fast, mednafen_supergrafx | optional (core option) |
| `gexpress.pce` | `6d2cb14fc3e1f65ceb135633d1694122` | mednafen_pce_fast, mednafen_supergrafx | optional (core option) |
| `bios.min` | `1e4fb124a3a886865acb574f388c803d` | pokemini | optional (built-in FreeBIOS) |
| `7800 BIOS (U).rom` | `0763f1ffb006ddbe32e52d497ee848ae` | prosystem | optional |
| `7800 BIOS (E).rom` | `397bb566584be7b9764e7a68974c4263` | prosystem | optional |
| `lynxboot.img` | `fcd403db69f54290b51035d82f835e7b` | handy | optional (HLE boot) |
| `bios.sms` | `840481177270d5642a14ca71ee72844c` | gearsystem, smsplus | optional |
| `bios.gg` | `672e104c3be3a238301aceffc3b23fd6` | gearsystem | optional |
| `dmg_boot.bin` | `32fbbd84168d3482956eb3c5051637f5` | gearboy | optional (same file as `gb_bios.bin`) |
| `cgb_boot.bin` | `dbfce9db9deaa2567f6a84fde55f9680` | gearboy | optional (same file as `gbc_bios.bin`) |
| `colecovision.rom` | `2c66f5911e5b42b8ebe113403548eee7` | gearcoleco | required (also `coleco.rom`, `os7.u2`) |
| `BIOS.col` | `2c66f5911e5b42b8ebe113403548eee7` | smsplus | ColecoVision only (same file, this exact case) |
| `aes.zip` | - (MAME zip set) | geolith | Neo Geo cartridges, default AES mode |
| `neogeo.zip` | - (MAME zip set) | geolith | MVS / Universe BIOS mode only (fbneo reads its own copy from the ROM folder) |
| `neocdz.zip` | - (MAME zip set) | geolith | Neo Geo CD (all models) |
| `neocd.zip` | - (MAME zip set) | geolith | Neo Geo CD front/top-loader models only |

## Architectures (porting)

The core packages build for three targets (docs/porting.md): the RetroStone2 (ARMv7 Cortex-A7, hard-float, NEON),
64-bit ARM (Raspberry Pi 3/4/5) and x86_64 (a PC or VM test build). `package/libretro-common.mk` (included first by
`external.mk`) defines `$(call rsos-libretro-select,KEY=VALUE ...)`, which picks the value of the first matching key
among `<arch>-<cpu>` (`arm-a7`, `aarch64-a72`, `aarch64-a53`, ...), `<arch>` (`arm`, `aarch64`, `x86_64`) and
`default`. Each `.mk` sets its `platform=` (and dynarec options) with it. In Config.in, the hidden symbol
`BR2_PACKAGE_RSOS_LIBRETRO_ARCH_SUPPORTS` (`package/libretro-cores.Config.in`) replaces the old `BR2_cortex_a7`
dependency: Cortex-A7 on 32-bit ARM, any aarch64, x86_64.

Two facts shape the choices:

- Buildroot's toolchain wrapper adds `-mcpu`/`-march` for `BR2_cortex_*` only when the command line has none. The
  `rpi2`, `rpi3_64` and `rpi4_64` platforms set `-mcpu`/`-mtune` themselves, so they are only used on their own CPU
  (Cortex-A7, A53, A72); `platform=unix` leaves the CPU flags to the wrapper.
- Buildroot does `unexport ARCH`, and several makefiles guess the architecture from `uname -m` of the build host
  (x86_64). The dynarec cores therefore get `ARCH` (or their arm64 platform) explicitly.

| Core | RetroStone2 (arm-a7, unchanged) | aarch64 Cortex-A72 (Pi 4) | aarch64 Cortex-A53 (Pi 3) | other aarch64 | x86_64 | Dynarec / asm |
|---|---|---|---|---|---|---|
| beetle-ngp, beetle-pce-fast, beetle-wswan, bluemsx, gambatte, gearboy, gearcoleco, gearsystem, snes9x2010 | `rpi2` | `rpi4_64` | `rpi3_64` | `unix` | `unix` | none (snes9x2010: NEON or SSE2 tile renderer, automatic) |
| fbneo | `rpi2` (Cyclone, NEON) | `rpi4_64` | `rpi3_64` | `unix` | `unix` | Cyclone and the NEON flags are ARM32 only; no x86_64 DRC |
| pcsx_rearmed | `rpi2` (ari64, NEON asm GPU) | `rpi4_64` (ari64 arm64, SIMD C GPU) | `rpi3_64` | `unix` (ari64 arm64) | `unix` (lightrec) | never `HAVE_NEON_ASM=1` off ARM32 |
| prosystem | `rpi2` | `rpi4` (its 64-bit A72 branch) | `rpi3_64` | `unix` | `unix` | none |
| smsplus-gx, stella2014 | `rpi2` | `unix` | `unix` | `unix` | `unix` | their rpi branches are 32-bit only |
| fceumm | `armv-neon-hardfloat` (any ARMv7) | `rpi4_64 WANT_32BPP=0` | `rpi3_64 WANT_32BPP=0` | `unix WANT_32BPP=0` | `unix WANT_32BPP=0` | none; `WANT_32BPP=0` keeps RGB565 |
| snes9x2005 | `armv-hardfloat` (any ARMv7) | `unix` | `unix` | `unix` | `unix` | none |
| gpsp | `rpi2` (ARM dynarec) | `arm64` (arm64 dynarec) | `arm64` | `arm64` | `unix HAVE_DYNAREC=1 CPU_ARCH=x86_32 MMAP_JIT_CACHE=1` | never `unix` on aarch64 (host `uname -a`) |
| mame2003-plus | `rpi2 USE_CYCLONE=1 USE_DRZ80=1` | `rpi4_64` | `rpi3_64` | `unix` | `unix` | Cyclone/DrZ80 are ARM32 asm; C cores elsewhere |
| picodrive | `rpi2 ARCH=arm` (Cyclone, DrZ80, SH2/SVP DRC, asm) | `aarch64 ARCH=aarch64` | same | same | `unix ARCH=x86_64` | FAME + CZ80 + the SH2 DRC (arm64 / x86 emitter) off ARM32 |
| parallel-n64 | `unix ARCH=arm WITH_DYNAREC=arm HAVE_NEON=1`, ARM `CPUFLAGS` | `unix ARCH=aarch64 WITH_DYNAREC=aarch64 HAVE_NEON=0 CPUFLAGS=` | same | same | `unix ARCH=x86_64 WITH_DYNAREC=x86_64 CPUFLAGS=`, nasm | GLES2 (`GLES=1 GL_LIB=-lGLESv2`) everywhere; never `rpi4_64` (Vulkan only) |
| mupen64plus-next | `rpi2-mesa ARCH=arm` | `unix ARCH=aarch64 FORCE_GLES=1` | same | same | `unix ARCH=x86_64 FORCE_GLES=1`, nasm | GLES2 everywhere (the host's HW render is GLES 2.0; `rpi4_64` would be GLES3) |
| beetle-supafaust, clownmdemu, geolith, handy | `unix` | `unix` | `unix` | `unix` | `unix` | none |
| beetle-supergrafx, fuse | `rpi2` | `rpi4_64` | `rpi3_64` | `unix` | `unix` | none |
| pokemini | `rpi2` | `rpi4` (its 64-bit A72 branch) | `rpi3_64` | `unix` | `unix` | none |
| cap32 | `rpi2` | `unix` | `unix` | `unix` | `unix` | its rpi3 branch is 32-bit only |
| prboom, fake08, nxengine, vice (`EMUTYPE=x64`) | `unix` | `unix` | `unix` | `unix` | `unix` | none (prboom: NEON drawers from `__ARM_NEON`) |
| dosbox-pure | no platform (`MAKE_CPUFLAGS` empty) | `MAKE_CPUFLAGS=-DPAGESIZE=4096` | same | same | empty | DOSBox dynrec: ARMV7LE / ARMV8LE backends, `dynamic_x86` on x86_64, chosen by `include/config.h` |
| scummvm | `unix HAVE_NEON=1 BUILD_64BIT=0` | `unix HAVE_NEON=1 BUILD_64BIT=1` | same | same | `unix HAVE_NEON=0 BUILD_64BIT=1` | NEON blitters (`SCUMMVM_NEON`); software rendering everywhere |

The x86_64 builds of the two N64 cores need `host-nasm` (added to their dependencies on x86_64). The x86_64 column
is untested (no x86_64 image yet); the aarch64 column is what `rpi4_64_defconfig` builds.

**RetroStone2 unchanged.** The expanded build commands of all 25 core packages (`make printvars` of every
`LIBRETRO_*_BUILD_CMDS`, `*_INSTALL_TARGET_CMDS` and `*_FINAL_DEPENDENCIES`) are byte-identical before and after this
change, and the 25 cores rebuilt from scratch (`dirclean`) give byte-identical `.so` files, except pcsx_rearmed,
whose only difference is its embedded "build time:" string (`__DATE__`/`__TIME__`, 5 bytes).

## Build notes

- **Flags.** Buildroot's toolchain wrapper already adds `-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`
  (from `BR2_cortex_a7`, `BR2_ARM_FPU_NEON_VFPV4`, `BR2_ARM_EABIHF`). Each core also gets its own ARM platform from its
  Makefile, and those platforms enable the asm/dynarec/NEON paths. `CFLAGS`/`CXXFLAGS`/`LDFLAGS` are passed **in the
  environment** (`$(TARGET_CONFIGURE_OPTS)` before `$(MAKE)`), so each Makefile's own `CFLAGS += ...` still applies. On
  the make command line they would replace them. `CC`/`CXX`/`AR` go on the command line because several Makefile
  branches hard-code `CC = gcc`. PicoDrive is the exception, see its .mk.
- **Most `platform=rpi2` branches hard-code Cortex-A7 flags**, so on 32-bit ARM those packages need `BR2_cortex_a7`
  (`BR2_PACKAGE_RSOS_LIBRETRO_ARCH_SUPPORTS`, which also allows aarch64 and x86_64: see
  [Architectures](#architectures-porting)). fceumm and snes9x2005 only need `BR2_arm` + EABIhf.
- **Executable stack.** Cyclone and DrZ80 asm (picodrive, mame2003_plus, fbneo) have no `.note.GNU-stack` section, so
  by default the linker marks the whole .so `GNU_STACK RWE`. Since glibc 2.41, `dlopen()` refuses such a library
  instead of making the stack executable. Those three packages, and the two N64 cores (dynarec asm) as a precaution,
  link with `-Wl,-z,noexecstack`. The Buildroot build
  was checked: all 11 installed .so files are `GNU_STACK RW` (glibc 2.41 in the Bootlin toolchain).
- **TEXTREL.** picodrive, mame2003_plus and fbneo have text relocations from their asm (non-PIC literal pools). glibc
  handles this at load time by mprotecting and patching the text pages. It costs a little load time and memory. No
  action needed unless we enable a hardened loader.
- **PicoDrive sources** come from git with submodules (`_SITE_METHOD = git`, `_GIT_SUBMODULES = YES`). The GitHub
  tarball lacks cpu/cyclone, emu2413, libchdr, dr_libs. Cyclone.s is generated at build time by a host C++ tool
  (`CYCLONE_CC/CXX = $(HOSTCC)/$(HOSTCXX)`). For that reason the target `CC` must not be on PicoDrive's make command
  line: a command-line CC would be inherited by the Cyclone sub-make. PicoDrive also builds with
  `-U_LARGEFILE64_SOURCE`. With Buildroot's LFS flags, `dr_mp3.h` calls `fopen64()`, which bypasses the libretro
  VFS and assigns a `FILE *` to an `RFILE *`, and GCC 14 rejects that.
- **N64 packages** depend on `BR2_PACKAGE_HAS_LIBGLES` (plus `BR2_PACKAGE_HAS_LIBEGL` for mupen64plus-next) and
  build-depend on `libgles` (and `libegl`), which Buildroot's mesa3d provides. mupen64plus-next builds its C code with
  `-U_LARGEFILE64_SOURCE`, because its bundled zlib `#undef`s `_FILE_OFFSET_BITS` under `_LARGEFILE64_SOURCE`. That is
  a hard `#error` as soon as `_TIME_BITS=64` is on (`BR2_TIME_BITS_64`, or Ubuntu's armhf toolchain, which defaults to
  64-bit time_t). All the other cores were checked with `-D_TIME_BITS=64` too, so enabling `BR2_TIME_BITS_64` later is
  safe for them.
- **Hashes.** Every package has a `.hash` file with the sha256 of the source archive and of its license file. For
  PicoDrive, the archive is the one Buildroot's git helper generates
  (`libretro-picodrive-<sha>-git4.tar.gz`, reproducible).
- **Integration.** Buildroot's kconfig cannot glob, so `buildroot-external/Config.in` needs one line:
  `source "$BR2_EXTERNAL_RETROSTONE_PATH/package/libretro-cores.Config.in"` (a menu that sources the 11 packages).
  `external.mk` already includes the `.mk` files. Suggested defconfig lines: `BR2_PACKAGE_LIBRETRO_FCEUMM=y`,
  `..._SNES9X2005=y`, `..._SNES9X2010=y`, `..._PICODRIVE=y`, `..._GAMBATTE=y`, `..._GPSP=y`, `..._PCSX_REARMED=y`,
  `..._MAME2003_PLUS=y`, `..._FBNEO=y`, `..._PARALLEL_N64=y`, and optionally `..._MUPEN64PLUS_NEXT=y`
  (experimental).
- **Root filesystem size.** The eleven cores take about 108 MB stripped: fbneo 59 MB, mame2003_plus 29 MB,
  mupen64plus_next 4.6 MB, parallel_n64 4.0 MB, the rest ~12 MB together. Mesa/lima adds its own libraries.
  `BR2_TARGET_ROOTFS_EXT2_SIZE="512M"` is enough, but keep it in mind.
- **Optional arcade data.** MAME 2003-Plus reads `hiscore.dat`, `cheat.dat`, `history.dat` and samples from
  `<system dir>/mame2003-plus/`. FBNeo reads `hiscore.dat` and samples from `<system dir>/fbneo/`. They are not
  installed. Both upstream trees carry them in `metadata/` if we want to ship them on `/data` later.

## Verification

Two independent checks, both in WSL under `~/rsos/cores-test/` (never under /mnt/c).

**1. Standalone cross-compile with the host toolchain.** Each pinned commit was cloned into
`~/rsos/cores-test/<core>` and built with Ubuntu's `arm-linux-gnueabihf-gcc`/`g++` 13.3, using the make arguments of
the .mk. The environment emulated Buildroot: wrapper flags `-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`
plus `-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 -O2 -g0` in `CFLAGS`/`CXXFLAGS`.
All 9 cores built. This first pass found three problems: the missing `-lm` in snes9x2010, the Cyclone host tool
being built with the cross compiler when `CC` was on the command line (picodrive), and `GNU_STACK RWE` on picodrive and
fbneo. The .mk files handle all three.

**2. Real Buildroot build** with Buildroot 2026.02.3 and the project's toolchain (Bootlin armv7-eabihf glibc stable
2025.08-1: GCC 14.3, glibc 2.41). This used a scratch `O=~/rsos/cores-test/br-out` and a scratch BR2_EXTERNAL
(`~/rsos/cores-test/br-ext`, named RETROSTONE, symlinking these package directories and sourcing
`libretro-cores.Config.in`). The defconfig had the same arch and toolchain lines as `retrostone2_defconfig` plus the 9
`BR2_PACKAGE_LIBRETRO_*`. Downloads and `.hash` checks passed, and all 9 packages built and installed. One extra fix
came from this pass: GCC 14 turned the dr_mp3 `fopen64()` issue in picodrive into an error, fixed with
`-U_LARGEFILE64_SOURCE` (see its .mk).

Checks on the installed (stripped) files in `target/usr/lib/libretro/`:

- `file` reports `ELF 32-bit LSB shared object, ARM, EABI5`.
- `readelf -A` reports `Tag_CPU_name: 7-A`, `Tag_FP_arch: VFPv4`, `NEONv1 with Fused-MAC` and
  `Tag_ABI_VFP_args: VFP registers`.
- `readelf -l` reports `GNU_STACK RW` for all 9.
- NEEDED is only libc, libm and ld-linux-armhf, plus libstdc++ and libgcc_s for gambatte and fbneo.
- `nm -D` shows all 23 mandatory `retro_*` entry points exported (`retro_init` ... `retro_run` ... `retro_get_memory_size`).
- No `-Wincompatible-pointer-types`, `-Wimplicit-function-declaration` or `-Wint-conversion` warnings left in the log.

The build logs confirm that the ARM paths were compiled:

- picodrive: `Cyclone.o`, `drz80.o`, `sh2/compiler.o`, `svp/compiler.o` and the `*_arm.S` files
- gpsp: `arm/arm_stub.o` with `HAVE_DYNAREC` and `MMAP_JIT_CACHE`
- pcsx_rearmed: `new_dynarec/linkage_arm.o`, `gte_neon.o` and `gpu_neon/psx_gpu/psx_gpu_arm_neon.o` with `NDRC_THREAD`
- mame2003_plus: `cyclone.S` and `drz80.s`
- fbneo: `Cyclone.o`

| Core | Standalone build time (16-core host, -j) | .so installed by Buildroot (stripped) | TEXTREL |
|---|---|---|---|
| fceumm | 12 s (-j3) | 2.66 MB | no |
| snes9x2005 | 6 s (-j4) | 0.60 MB | no |
| snes9x2010 | 37 s (-j6) | 2.36 MB | no |
| picodrive | 21 s (-j6) | 2.30 MB | yes |
| gambatte | 4 s (-j3) | 1.61 MB | no |
| gpsp | 15 s (-j3) | 0.77 MB | no |
| pcsx_rearmed | 13 s (-j4) | 1.33 MB | no |
| mame2003_plus | 27 s (-j14) | 28.9 MB | yes |
| fbneo | 111 s (-j12) | 59.1 MB | yes |
| parallel_n64 (GLES2) | 28 s (-j8) | 3.97 MB | no |
| mupen64plus_next (GLES2) | 21 s (-j12) | 4.59 MB | no |
| **total** | | **~108 MB** | |

Second batch (Buildroot build time, including extraction; the table above has standalone times):

| Core | Buildroot build time (16-core host) | .so installed by Buildroot (stripped) | TEXTREL |
|---|---|---|---|
| mednafen_pce_fast | 10 s | 2.60 MB | no |
| stella2014 | 12 s | 1.71 MB | no |
| prosystem | 9 s | 0.21 MB | no |
| handy | 9 s | 0.55 MB | no |
| mednafen_ngp | 11 s | 0.31 MB | no |
| mednafen_wswan | 10 s | 0.69 MB | no |
| gearsystem | 14 s | 0.41 MB | no |
| smsplus | 10 s | 0.19 MB | no |
| gearboy | 14 s | 0.39 MB | no |
| gearcoleco | 14 s | 0.48 MB | no |
| bluemsx | 11 s | 1.59 MB (+ 2.7 MB `/usr/share/rsos/cores/bluemsx/`) | no |
| **total** | | **~9.1 MB + 2.7 MB** | |

**N64 cores.** For the N64 cores, the scratch Buildroot config also had `BR2_PACKAGE_LIBDRM`, `BR2_PACKAGE_MESA3D`,
`_GALLIUM_DRIVER_LIMA`, `_OPENGL_EGL` and `_OPENGL_ES`, the same Mesa lines as `retrostone2_defconfig`. Mesa 26.0.1
was built, and both cores were built through Buildroot against it.

- **Standalone check:** the same make arguments with Ubuntu gcc 13, using only the GLES2/EGL/KHR headers and
  `libGLESv2.so`/`libEGL.so` from the Buildroot staging dir.
- **Installed .so checks:** ARM EABI5, VFPv4 + NEON, `GNU_STACK RW`, no TEXTREL, all 23 mandatory `retro_*`
  exported. NEEDED adds `libGLESv2.so.2` (both) and `libEGL.so.1` (mupen64plus_next) to libc/libm/libstdc++/libgcc_s.
- **Build logs:** `-DNEW_DYNAREC=3`, `new_dynarec/arm/linkage_arm.o` and `-DHAVE_OPENGLES2`/`-DGLES2` in both;
  `glsym_es2.o` and the `glide2gl`, `gles2n64` and `gles2rice` objects in parallel_n64; `GLideN64.o` and
  `Neon/gSPNeon.o` in mupen64plus_next.
- **Hashes and licenses:** `legal-info` passed for all 11 packages, which checks their license file hashes.

Not verified yet: loading and running the cores (needs the frontend and the device). TODO(hw): dlopen each core
from the frontend on the RetroStone2, run a game for each system, and check save/load state and frame times. For N64,
also check that lima compiles the glide64/gln64/rice and GLideN64 shaders (look for `ppir`/`gpir` compile errors in the Mesa log with `MESA_DEBUG=1`).

## Second batch: licence-clean cores

Eleven more packages, chosen because their licences allow redistribution with a sold device (GPL, LGPL, zlib, BSD).
Each licence was checked in the pinned source tree, not taken from the README.

| Package | .so / .ini id | Licence (checked in the tree) | Notes |
|---|---|---|---|
| `libretro-beetle-pce-fast` | `mednafen_pce_fast` | GPL-2.0+ (Mednafen headers); vendored libchdr BSD-3-Clause, zlib, zstd BSD-3-Clause, LZMA SDK public domain | |
| `libretro-stella2014` | `stella2014` | GPL-2.0 (`stella/license.txt`) | |
| `libretro-prosystem` | `prosystem` | GPL-2.0+; CoreTone sound (`bupboop/`) zlib | |
| `libretro-handy` | `handy` | zlib (Handy, `lynx/license.txt`); Blip_Buffer LGPL-2.1+, Stereo_Buffer GPL-2.0+ | no top-level licence file |
| `libretro-beetle-ngp` | `mednafen_ngp` | GPL-2.0+ (Mednafen, NeoPop) | |
| `libretro-beetle-wswan` | `mednafen_wswan` | GPL-2.0+; NEC V30MZ core (`v30mz.c`, Bryan McPhail): commercial and non-commercial use allowed with a credit in the documentation | **put the V30MZ credit on the licence screen** |
| `libretro-gearsystem` | `gearsystem` | GPL-3.0 | |
| `libretro-smsplus-gx` | `smsplus` | GPL-2.0+ (descends from the GPL SMS Plus <= 1.3, not the later non-commercial releases, see its `docs/contributors.txt`); MAME Z80 core BSD-3-Clause; YM2413 GPL-2.0+; sms_ntsc LGPL | |
| `libretro-gearboy` | `gearboy` | GPL-3.0 | |
| `libretro-gearcoleco` | `gearcoleco` | GPL-3.0 | |
| `libretro-bluemsx` | `bluemsx` | zlib-style blueMSX licence; openMSX / fMSX-SDL sound chips GPL-2.0+; TinyXML, minizip zlib; V9938 engine free for any MSX emulator; C-BIOS BSD-2-Clause | upstream `system/` also carries copyrighted MSX ROMs: **not installed** |

Candidates rejected:

- **fMSX** (`fmsx-libretro`): its licence forbids commercial use without the author's agreement. blueMSX instead.
- **beetle-lynx**: GPL-2.0 and fine, but it is Mednafen's heavier Lynx core. handy is lighter, and its licence (zlib)
  is even more permissive.
- **Current Stella** (7.x, GPL-2.0, libretro core in `src/os/libretro`): far better compatibility, but its TIA core
  emulates per pixel and it is much heavier than Stella 3.9. libretro itself points weak ARM devices at stella2014. Not
  packaged. TODO(hw): try it on the RetroStone2 if a homebrew title needs it.
- **Virtual Boy** (heavy, two framebuffers, 3D): skipped for now. **DOS** (dosbox-pure) and **beetle-supergrafx**
  were added later, in the [third batch](#third-batch-computers-fantasy-consoles-and-game-engines).

### Build notes (second batch)

- Same package layout as the first batch: GitHub tarball pinned by commit, `.hash` with the tarball and licence file
  sha256, `CFLAGS`/`LDFLAGS` in the environment through `$(TARGET_CONFIGURE_OPTS)`, `CC`/`CXX`/`AR` on the command line,
  the `.ini` installed to `/usr/share/rsos/cores/`. None needs submodules. Gearsystem, Gearboy and Gearcoleco are built
  in `platforms/libretro/` of drhelius' own repositories.
- Platforms: `rpi2` wherever the Makefile has it (all but handy), so those packages `depend on BR2_cortex_a7`. handy's
  Makefile has no ARM platform, so it uses `unix` and only needs `BR2_arm` + EABIhf, like fceumm. The Gear* Makefiles
  add their `-mcpu`/`-mfpu` to `CFLAGS` only while the code is C++; the toolchain wrapper supplies them anyway.
- **No `-z noexecstack` needed**: all eleven are plain C/C++ with no asm, and all are `GNU_STACK RW` as built.
- **No `-U_LARGEFILE64_SOURCE` needed**: all eleven were rebuilt with `-D_TIME_BITS=64` (plus Buildroot's LFS flags)
  and compile cleanly. pce_fast vendors zlib 1.2.11 but only its inflate files, not the `gz*` ones that break
  mupen64plus-next.
- blueMSX's `Makefile.libretro` starts with `CFLAGS := -std=gnu89`, which throws away Buildroot's `CFLAGS` from the
  environment (LFS defines, `-O2 -g0`). Harmless: the wrapper still adds the CPU/ABI flags and the Makefile its own
  `-O2`. The .mk says so.
- C++ runtime: all but prosystem, smsplus and mednafen_wswan link `libstdc++` (pce_fast is C but links with `g++`), so
  their Config.in has `depends on BR2_INSTALL_LIBSTDCPP`.
- Integration: `libretro-cores.Config.in` sources the eleven new packages. Suggested defconfig lines:
  `BR2_PACKAGE_LIBRETRO_BEETLE_PCE_FAST=y`, `..._STELLA2014=y`, `..._PROSYSTEM=y`, `..._HANDY=y`, `..._BEETLE_NGP=y`,
  `..._BEETLE_WSWAN=y`, `..._GEARSYSTEM=y`, `..._SMSPLUS_GX=y`, `..._GEARCOLECO=y`, `..._BLUEMSX=y`, and optionally
  `..._GEARBOY=y` (alternative to gambatte only).

### Atari 2600: why stella2014

Stella 3.9.3 (2014) is the version libretro keeps for weak devices: small, fast, and it runs the commercial library.
The Atari 2600 has no BIOS. XRGB8888 is only used if `stella2014_color_depth` is set to `24bit`; the default is RGB565.

### MSX: blueMSX system files

blueMSX does not read a single BIOS file. It reads machine definitions from
`<system dir>/Machines/<machine>/config.ini` (the ROM paths inside are relative to the system directory) and game
databases from `<system dir>/Databases/*.xml`. With the default `bluemsx_msxtype = Auto`, cartridges, disks and tapes
start on the machine called `MSX2+`, `.col` files on `COL - ColecoVision`, and `.sg`/`.sc` files on the Sega machines.

The upstream `system/bluemsx/Machines/Shared Roms/` contains the original MSX system ROMs (MSX.rom, MSX2P.rom, KANJI,
disk ROMs, ...). They are copyrighted, so the package does not install them. It installs these to
`/usr/share/rsos/cores/bluemsx/`:

- `Databases/*.xml` (blueMSX data, 2.4 MB)
- `Machines/MSX - C-BIOS/`, `MSX2 - C-BIOS/`, `MSX2+ - C-BIOS/`: [C-BIOS](http://cbios.sourceforge.net/) 0.23, a free
  MSX BIOS (BSD-2-Clause; its `cbios.txt` is installed next to it)
- `Machines/MSX/config.ini`, `Machines/MSX2/config.ini`, `Machines/MSX2+/config.ini`: copies of the C-BIOS configs. Their
  ROM paths point at the C-BIOS folders, so the `Auto` machine (`MSX2+`) runs cartridges out of the box.

C-BIOS has no BASIC and no Disk BASIC: it runs **cartridge images only**. For disk (`.dsk`) and tape (`.cas`) games,
the user copies the complete blueMSX `Machines` folder (with the real ROMs) into the system directory. This replaces the
three alias `config.ini` files, which then point at the real ROMs. The user guide should document this.

### New metadata key: `system_tree`

`bluemsx.ini` needs a whole directory tree in the system directory, which `system_files` (a flat list of files) cannot
express. New key, in `[core]`:

```ini
system_tree = /usr/share/rsos/cores/bluemsx   ; copy every file below this directory into the system directory,
                                              ; keeping relative paths, creating directories, never overwriting
```

The frontend applies it like `system_files`: before the first game with that core, and skipping files that already
exist, so the user's own files win.

Other points for the frontend's `.ini` parser and BIOS check:

- BIOS section names can contain spaces, parentheses and a slash: `[bios:7800 BIOS (U).rom]`,
  `[bios:Machines/Shared Roms/MSX2P.rom]`. The name after `bios:` is the path relative to the system directory, up to
  the closing `]`.
- BIOS names are case-sensitive: smsplus opens `BIOS.col`, gearcoleco `colecovision.rom` (the same file).
- `required = <systems>` is used where systems share a core: `syscard3.pce` is required for `pcenginecd` only, and
  `BIOS.col` for `coleco` only.
- Pixel formats: nothing new. RGB565 is the default for all eleven. stella2014, prosystem, handy and mednafen_wswan
  switch to XRGB8888 only if their 24-bit colour option is set.

### Verification (second batch)

Built through the scratch Buildroot tree (`~/rsos/cores-test/br-out`, Buildroot 2026.02.3, Bootlin armv7-eabihf glibc
stable 2025.08-1, GCC 14.3, glibc 2.41), with the eleven `BR2_PACKAGE_LIBRETRO_*` added to its defconfig. After the
`.hash` files were written, every package was rebuilt from `-dirclean` (download hash check), and `<pkg>-legal-info`
passed for all eleven (licence file hashes). Checks on the installed, stripped `.so` files:

- `ELF 32-bit LSB shared object, ARM, EABI5`; `readelf -A`: `Tag_CPU_name: 7-A`, `Tag_FP_arch: VFPv4`,
  `NEONv1 with Fused-MAC`, `Tag_ABI_VFP_args: VFP registers`.
- `GNU_STACK RW` for all eleven, and no TEXTREL.
- NEEDED: libc and ld-linux-armhf, plus libm for all but wswan, plus libstdc++/libgcc_s for the C++-linked ones. No
  other library.
- `nm -D`: each exports all 25 mandatory `retro_*` entry points.
- Build logs: no `-Wincompatible-pointer-types`, `-Wimplicit-function-declaration`, `-Wint-conversion` or errors; only
  a few `-Wunused-result`/`-Wmissing-braces` warnings.
- A separate pass with `-D_TIME_BITS=64`: all eleven build.

Not verified yet: running them. TODO(hw): on the RetroStone2, load a game per system, check save states, and measure
frame time on a demanding title (pce_fast CD with CD-DA, bluemsx on an MSX2+ SCC/FM game, Gearsystem on a Game Gear
game).

## Licence-clean alternatives

What a commercially clean image (GPL/LGPL/permissive cores only, see `requirements.md` §13) can contain, per system.
"Packaged" means a Buildroot package exists in this tree.

| System | Current default | Licence-clean core | Licence | Status |
|---|---|---|---|---|
| NES / FDS | fceumm | fceumm | GPL-2.0+ | packaged (already clean) |
| SNES | snes9x2005 / snes9x2010 (non-commercial) | **supafaust** (beetle-supafaust) | GPL-2.0+ | packaged as **experimental** (`libretro-beetle-supafaust`, default off). The only viable GPL SNES core on this CPU, see below |
| Mega Drive / Genesis | picodrive (non-commercial) | **ClownMDEmu** | AGPL-3.0+ | packaged as **experimental** (`libretro-clownmdemu`, default off). Speed on a 1 GHz A7 unknown, see below |
| Mega-CD / Sega CD | picodrive | ClownMDEmu (MD + CD) | AGPL-3.0+ | as above |
| 32X, Pico | picodrive | none | - | no GPL 32X core light enough |
| Master System, Game Gear, SG-1000 | picodrive | gearsystem (default), smsplus (lighter) | GPL-3.0 / GPL-2.0+ | packaged |
| Game Boy / Color | gambatte | gambatte; gearboy per game | GPL-2.0 / GPL-3.0 | packaged |
| Game Boy Advance | gpsp | gpsp | GPL-2.0+ | packaged |
| PlayStation | pcsx_rearmed | pcsx_rearmed | GPL-2.0+ | packaged |
| Nintendo 64 | parallel_n64 | parallel_n64, mupen64plus_next | GPL-2.0 | packaged |
| Arcade | mame2003_plus, fbneo (non-commercial) | none light enough | - | current MAME is GPL-2.0+/BSD but far too heavy for a 1 GHz A7 |
| Neo Geo | fbneo | Geolith | BSD-3-Clause (Musashi MIT, ymfm BSD-3-Clause) | packaged as **experimental** (`libretro-geolith`, default off). Needs `.neo` ROMs and MAME BIOS zips, see [Neo Geo: Geolith](#neo-geo-geolith) |
| PC Engine / CD | - | mednafen_pce_fast | GPL-2.0+ | packaged |
| Atari 2600 / 7800 / Lynx | - | stella2014 / prosystem / handy | GPL-2.0 / GPL-2.0+ / zlib | packaged |
| Neo Geo Pocket, WonderSwan | - | mednafen_ngp, mednafen_wswan | GPL-2.0+ | packaged |
| ColecoVision | - | gearcoleco (smsplus and bluemsx also run it) | GPL-3.0 | packaged |
| MSX | - | bluemsx + C-BIOS | zlib-style + GPL-2.0+ + BSD-2-Clause | packaged |
| SuperGrafx | - | mednafen_supergrafx | GPL-2.0+ | packaged |
| ZX Spectrum, Amstrad CPC | - | fuse, cap32 | GPL-2.0+/GPL-3.0 + Amstrad's ROM permission | packaged |
| Commodore 64 | - | vice_x64 | GPL-2.0+; **the embedded C64 ROMs are Commodore's** (see [third batch](#licences-third-batch)) | packaged, owner decision |
| DOS, ScummVM, Doom, PICO-8, Pokémon mini | - | dosbox_pure, scummvm, prboom, fake08, pokemini | GPL-2.0+ / GPL-3.0+ / GPL-2.0+ / MIT / GPL-3.0+ | packaged |

So a clean image loses 32X and arcade entirely, keeps SNES and Mega Drive only if the two cores below hold up on the
hardware, and gains PC Engine, Atari, NGP, WonderSwan, ColecoVision and MSX.

GPL-3.0 (Gear*) and AGPL-3.0 (ClownMDEmu) are fine for a sold device if the frontend stays MIT or GPL-2.0-or-later
(`requirements.md` §13) and the usual source offer is made. GPL-3 §6 adds one obligation for consumer devices: users
must be able to install modified versions. A plain SD-card image with no signature check meets it. Keep it that way
(no locked or signed rootfs) if Gear* cores ship.

### Mega Drive: is there a GPL core light enough?

Checked in the sources (`~/rsos/cores-test/research/`):

- **Genesis Plus GX** (`c2838c7`): its `LICENSE.txt` says redistributions "may not be sold, nor may they be used in a
  commercial product". Non-commercial, like PicoDrive.
- **BlastEm** (`libretro/blastem` `542164d`, GPL-3.0): its 68000 and Z80 are x86/x86-64 JIT recompilers
  (`m68k_core_x86.c`, `z80_to_x86.c`). On any other CPU the Makefile builds `NEW_CORE`, the generated interpreters
  (`m68k.cpu`, `z80.cpu`), with a cycle-exact VDP. That is a desktop-class workload. No ARM32 benchmark is published,
  and it is not a candidate for a 1 GHz A7.
- **ClownMDEmu** (`Clownacy/clownmdemu-libretro` `0f23a16`, AGPL-3.0-or-later; clowncd ISC-style): C89, no
  dependencies, one translation unit. The author describes it as balancing accuracy with speed, more high-level than
  accuracy-focused emulators, and reports optimisations (YM2612, VDP and mixer, each ~10 % of frame time). It runs Mega
  Drive and Mega-CD. It is an interpreter (no dynarec, unlike PicoDrive's Cyclone/DrZ80), so it will be much heavier
  than PicoDrive. Published testing is on a Pi 3B+ (AArch64); **no Cortex-A7 or ARM32 numbers exist**. It
  cross-compiles cleanly with our toolchain (`arm-linux-gcc -O2`, 0.74 MB unstripped). It also has one more frame of
  input latency than Genesis Plus GX.
- Mednafen's own `md` module is GPL, but it has no maintained libretro port.

**Verdict: no GPL Mega Drive core is known to run at full speed on a 1 GHz Cortex-A7.** ClownMDEmu is the only
realistic candidate. It is now packaged as experimental (`libretro-clownmdemu`, see
[Experimental licence-clean cores](#experimental-licence-clean-cores)). Next step: measure frame time on the
RetroStone2 with Sonic 2 and a heavy title (Sonic 3D, Vectorman). Until then, a clean image has no proven Mega Drive
core.

### SNES: is there a GPL core light enough?

- **snes9x** (all variants, including snes9x2002/2005/2010): Snes9x licence, non-commercial.
- **bsnes / higan / bsnes-mercury / bsnes2014** (GPL-3.0): even the "performance" profiles are cycle-based
  interpreters that users report cannot hold 60 fps on Pi 3/4-class ARM. Not viable on a 1 GHz A7.
- **Mesen-S** (GPL-3.0), **beetle-bsnes** (bsnes 0.59, GPL-2.0): accuracy-oriented, too slow here.
- **beetle-supafaust** (`libretro/supafaust` `642d1d1`, GPL-2.0+ in the file headers): the libretro port of Mednafen's
  `snes_faust`, written for this class of hardware. Its README targets multicore ARM Cortex-A7, A9, A15 and A53 Linux
  devices, and gives a minimum of **dual-core 900 MHz for most games, 1.2 GHz for SuperFX, SA-1 and CX4 games**. The
  PPU renders on a second thread (`supafaust_renderer = mt`, with `supafaust_thread_affinity_emu` / `_ppu` options).
  Special chips: DSP-1/2, CX4, SuperFX, SA-1, S-DD1, MSU1. It cross-compiles cleanly with our toolchain (`-pthread`,
  1.26 MB). No formal benchmarks are published. User reports on dual-A7 1.2 GHz handhelds (Miyoo Mini class) describe
  most games at full speed.

**Verdict: supafaust is viable for a licence-clean image on the RetroStone2, with little margin.** The A20 meets the
900 MHz dual-core minimum, but only just. Both cores would be busy (emulation + PPU), the frontend's audio and video
work competes with them, and the A20's DDR3 is slower than that of the devices supafaust was tuned on. Expect most
non-chip games at 60 fps, and SuperFX/SA-1/CX4 games below full speed (they need 1.2 GHz). snes9x2005 stays the better
core wherever the licence allows it. supafaust is now packaged as experimental (`libretro-beetle-supafaust`, PPU
thread pinned to the other core, see [Experimental licence-clean cores](#experimental-licence-clean-cores)). Next step:
measure on the RetroStone2 (Super Mario World, Donkey Kong Country, Yoshi's Island).

## Experimental licence-clean cores

Three packages that would give a licence-clean image SNES, Mega Drive/Mega-CD and Neo Geo. They are **not proven fast
enough** on the A20, so they are experimental: `bool "... (experimental)"` in Config.in, off unless a defconfig
enables them, and `experimental = true` in their `.ini`. The default cores do not change (snes9x2005, picodrive,
fbneo).

| System | Core (package) | Upstream commit | Build flags (make) | Expected on A20 | BIOS | Known issues |
|---|---|---|---|---|---|---|
| SNES (experimental, GPL) | mednafen_supafaust (`libretro-beetle-supafaust`) | [`642d1d1`](https://github.com/libretro/supafaust/commit/642d1d1b6684aa7e306a02a89885f3f5456a5157) 2026-08-23 | `-f Makefile platform=unix` (-O2, -pthread, -fwrapv; CPU flags from the wrapper), `CXXFLAGS += -DHAVE_SEM_CLOCKWAIT` | Needs both cores. Most non-chip games around 60 fps with little margin; SuperFX/SA-1/CX4 below full speed. TODO(hw). | none (DSP-1 data ROM is synthesised) | Builds with upstream's "sketchy SPC700 optimization" warning (intentional speed-up). |
| Mega Drive, Mega-CD (experimental, AGPL) | clownmdemu (`libretro-clownmdemu`) | [`0f23a16`](https://github.com/Clownacy/clownmdemu-libretro/commit/0f23a1696581f6969cfc78cc763af492c4bb3fd6) 2026-09-25 (git + submodules) | `platform=unix` (-O2, one C89 unity file), `-Wl,--version-script=link.T` | Unknown: interpreter cores with no dynarec, single-threaded. Likely well below PicoDrive's margin; measure. TODO(hw). | none, also for Mega-CD | One frame more input latency than Genesis Plus GX (upstream design). |
| Neo Geo AES/MVS, Neo Geo CD (experimental, BSD) | geolith (`libretro-geolith`) | [`1940249`](https://github.com/libretro/geolith-libretro/commit/194024931935eff2092e36fc4f8e53e62ed11097) 2026-09-14 | `-C libretro platform=unix` (-O2 -flto, -fsigned-char), links zlib | Borderline, see [Neo Geo: Geolith](#neo-geo-geolith). TODO(hw). | `aes.zip` (default mode), `neogeo.zip` (MVS/UniBIOS), `neocdz.zip` / `neocd.zip` (CD), all MAME sets | Cartridges only in the `.neo` format; XRGB8888 output only. |

### New metadata key: `experimental`

```ini
experimental = true   ; optional, in [core]. The frontend never picks this core as a folder's default, even if it is
                      ; the only core installed for a system in `systems`; it is offered as a per-game choice and
                      ; labelled as experimental. Absent = false.
```

Exception: `neocd` has no other core, so geolith is listed as that folder's default in [ROM folders](#rom-folders).
The frontend may show such a folder only when the core is installed.

### Beetle Supafaust: threads and flags

- **PPU thread.** Always on. `retro_load_game()` forces Mednafen's `snes_faust.renderer` to `mt` regardless of the
  `supafaust_renderer` option, so no build flag is involved. The `.ini` still sets `supafaust_renderer = mt` so the
  options menu shows the real state.
- **Affinity.** `supafaust_thread_affinity_emu = 0x2` pins the **calling thread** (the frontend thread that runs
  `retro_run()`) to CPU1 from `retro_load_game()` until `retro_unload_game()`. `supafaust_thread_affinity_ppu = 0x1`
  pins the PPU render thread to CPU0. These are the upstream defaults, and they fit the dual-core A20. Frontend
  consequences: the runner thread's affinity is changed behind its back (it is restored on unload), and the frontend's
  audio/input threads share CPU0 with the PPU. If the game runner already pins its threads, set both options to `0x0`
  (no pinning). TODO(hw): compare pinned and unpinned.
- **Monotonic waits.** The two threads hand work over with 1 ms timed semaphore waits. The libretro Makefile does not
  set Mednafen's `HAVE_SEM_CLOCKWAIT`, so upstream builds fall back to `sem_timedwait()` on `CLOCK_REALTIME` (the build
  prints "Using realtime-clock-based sem_timedwait()"). The RetroStone2 has no RTC backup, so the wall clock jumps when
  the saved time or NTP sets it, which can stall or spin those waits mid-game. The package adds
  `-DHAVE_SEM_CLOCKWAIT` (glibc >= 2.30), and the built .so imports `sem_clockwait` instead.
- **Flags.** `platform=unix` plus the toolchain wrapper's Cortex-A7 flags. The upstream `classic_armv7_a7` platform
  (SNES Classic, quad A7 at 1.2 GHz) is not used, because it links libstdc++/libgcc statically and builds with
  `-Ofast` and whole-program LTO. TODO(hw): if the core falls just short on the A20, try `-O3`/LTO before anything
  else.
- Config.in: `depends on BR2_TOOLCHAIN_HAS_THREADS_NPTL` (PPU thread, `pthread_setaffinity_np()`) and C++.

### ClownMDEmu: packaging notes

- Git with submodules (`_SITE_METHOD = git`, `_GIT_SUBMODULES = YES`, like PicoDrive). The core, clown68000, clownz80,
  clowncd and libchdr are nested submodules. The `.hash` covers the archive that Buildroot's git helper generates
  (`libretro-clownmdemu-<sha>-git4.tar.gz`).
- The Makefile's `unix` branch links without the repository's `link.T`, so the unity build exported ~650 internal
  symbols. The package passes `-Wl,--version-script=link.T` through `LDFLAGS`, and the .so now exports only the 25
  `retro_*`.
- No BIOS: the core does not open anything in the system directory. Mega-CD backup RAM goes to the save directory.
- RGB565 first, XRGB8888 as a fallback.

### Neo Geo: Geolith

Evaluation (source `194024931935`, BSD-3-Clause):

- **What it is.** An accuracy-oriented Neo Geo AES/MVS/CD emulator. Its README: the CPUs are "emulated at instruction
  level granularity and are very tightly synced" with the LSPC (video) and the YM2610. 100 % of the commercial AES/MVS
  and CD libraries are listed as compatible.
- **CPU cost on the A20.** A C 68000 interpreter (Musashi) at 12 MHz, a C Z80 at 4 MHz, the ymfm YM2610 (MAME's
  accurate FM core, ported to C) at its native rate plus a speex resampler, and a line-based LSPC. It is
  single-threaded, so the second core stays idle, and it has no asm or dynarec. For comparison, fbneo on ARM32 runs the
  Neo Geo 68000 through Cyclone (asm) precisely to save CPU. On AArch64, where Cyclone does not exist, fbneo's C 68000
  is reported full speed on 1.2-1.5 GHz Cortex-A53/A35 handhelds, which are somewhat faster per clock than a 1 GHz A7.
  Geolith adds tighter synchronisation and a heavier FM core on top.
- **Verdict: plausible but borderline.** Some games may run at full speed and others may not. It is worth measuring
  because it is the only licence-clean Neo Geo option, and it is small (0.97 MB). Packaged as experimental.
  TODO(hw): measure frame time on a light title (Metal Slug) and a heavy one (Garou, KOF 2000).
- **Content requirements (user guide).**
  - Cartridges must be in TerraOnion's `.neo` format. MAME/FBNeo `.zip` romsets do not load; they are converted with
    lithogen (`https://github.com/carmiker/lithogen`) or NeoBuilder.
  - BIOS zips from a recent MAME set go into the system directory: `aes.zip` for the default AES mode, `neogeo.zip` for
    MVS or Universe BIOS mode (`geolith_system_type`), `neocdz.zip` for any Neo Geo CD model, and also `neocd.zip` for
    the front/top loaders (`geolith_cd_system_type`). The frontend cannot check them by md5 (zip sets); it checks that
    the file exists.
  - `.neo` files can live in the existing `neogeo` folder next to fbneo's `.zip` sets, because the extensions differ.
    The frontend must offer only the cores whose `extensions` match the file. CD images go to a new `neocd` folder.
- XRGB8888 is the only pixel format (320x224, negligible bandwidth). Links the system zlib (`select BR2_PACKAGE_ZLIB`).

### AGPL obligations (ClownMDEmu)

ClownMDEmu is AGPL-3.0-or-later. Shipping it on a sold device or card is allowed, with these obligations:

1. **Everything GPL-3.0 requires.** Ship the licence text, and offer the Corresponding Source of the exact version
   shipped, including the submodules. Buildroot's `make legal-info` collects the source archive, and it must be
   published with each image, as for the other GPL cores. The source must be reproducible from the pinned commit.
2. **Installation Information (GPL-3 §6).** The device is a consumer product, so users must be able to install a
   modified build. An SD-card image with no signature check meets this; do not lock or sign the rootfs while AGPL/GPL-3
   cores ship.
3. **The AGPL-specific clause (§13).** It applies only if we **modify** the program and users **interact with it
   remotely over a network** (for example netplay, game streaming, or a web UI driving the core). Then those remote
   users must be offered the source of the modified version. Today we ship it unmodified and run it locally, so §13
   adds nothing. If a patch is ever carried, or a network feature touches the core, publish the patched source
   prominently (a link in the About/Licences screen).
4. **No additional restrictions.** The image's own terms (EULA, warranty text) must not restrict what users may do
   with the AGPL core.
5. **Combination.** AGPL-3.0 is compatible with GPL-3.0, but not with GPL-2.0-only code. The core is a separate `.so`
   that the MIT frontend loads alone (one core per game), so there is no conflict with gambatte (GPL-2.0-only). Never
   link clownmdemu statically into a binary together with GPL-2.0-only code. Keep the frontend MIT (or
   GPL-2.0-or-later), as `requirements.md` §13 says.

The on-device Licences screen lists clownmdemu with the AGPL text and the source offer.

### Verification (experimental cores)

Built through the same scratch Buildroot tree, with the three `BR2_PACKAGE_LIBRETRO_*` added to its defconfig. After
the `.hash` files were written, each package was rebuilt from `-dirclean`, and `<pkg>-legal-info` passed.

| Core | Buildroot build time | .so (stripped) | NEEDED | Exports |
|---|---|---|---|---|
| mednafen_supafaust | 36 s | 1.26 MB | libstdc++, libgcc_s, libm, libc | 25 `retro_*` |
| clownmdemu | 13 s (+ ~15 s git clone with submodules, once) | 0.69 MB | libm, libc | 25 `retro_*` (after the version script) |
| geolith | 16 s | 0.97 MB | libz, libm, libc | 46 (all 25 mandatory `retro_*` present) |

For all three: ARM EABI5, `Tag_CPU_name: 7-A`, VFPv4, NEON with FMA, VFP-register arguments, `GNU_STACK RW` (no asm,
so no `-z noexecstack` needed), no TEXTREL, and no incompatible-pointer, implicit-declaration or int-conversion warnings.
A separate pass with `-D_TIME_BITS=64` builds all three. supafaust imports `sem_clockwait` and
`pthread_setaffinity_np`.

Not verified: running them. TODO(hw): time each one on the RetroStone2 as listed above. Keep them experimental until a
per-game list shows which titles hold 60 fps.

## Third batch: computers, fantasy consoles and game engines

The ten cores of the feature-parity report's "top 10 cores to add next" (docs/review/feature-parity.md §4), plus the
free game data for two of them. All are in the three defconfigs; none is experimental.

| System (folder) | Core (package) | Upstream commit | Build flags on the RetroStone2 | Expected on A20 (2x A7 @ ~1 GHz) | BIOS | Known issues |
|---|---|---|---|---|---|---|
| Doom engine (`doom`) | prboom (`libretro-prboom`) | [`d20300d`](https://github.com/libretro/libretro-prboom/commit/d20300de2d32e5b8e8b0a0f15b1e1a889583d248) 2026-09-15 | `platform=unix` (-O2, C99; NEON column/span drawers from the wrapper's `-mfpu=neon-vfpv4`) | Full speed at 320x200 (Doom ran on 100-200 MHz ARM handhelds). | none (`prboom.wad` compiled in) | A PWAD needs its IWAD in the same folder (or the system directory). |
| PICO-8 (`pico8`) | fake08 (`libretro-fake08`) | [`814991a`](https://github.com/jtothebell/fake-08/commit/814991a2571ad3970e386cef48f3b148aa1c27b9) 2026-06-13 (git + submodule `z8lua`) | `platform=unix` (-O2, C++17) | Most carts full speed (the Miyoo Mini, 2x A7 @ 1.2 GHz, runs it); CPU-heavy carts (3D demos, big particle effects) slow down. | none | Not the official PICO-8: a few carts that use undocumented behaviour differ. |
| MS-DOS (`dos`) | dosbox_pure (`libretro-dosbox-pure`) | [`73e03aa`](https://github.com/schellingb/dosbox-pure/commit/73e03aa145e0549ed4d5a20f8e65532714da33f5) 2026-09-15 | Makefile's generic Linux build (-O2, its own `CFLAGS`), DOSBox **dynrec ARMV7LE**, `-pthread` | Real-mode and 286/386 games full speed (Commander Keen, Prince of Persia, Monkey Island, Wolf3D); 486-class games (DOS Doom full screen, Duke 3D) slow. See [DOS](#dos-dosbox-pure). | none | Keyboard-heavy games need the core's on-screen keyboard (a USB keyboard needs host support, not there yet). |
| ScummVM (`scummvm`) | scummvm (`libretro-scummvm`) | [`fcbce3a`](https://github.com/libretro/scummvm/commit/fcbce3ae815269dacdc309092bc92ccc6d3e13bb) 2026-09-19 + libretro-deps `bab7d25`, libretro-common `879c8d5` | `platform=unix HAVE_NEON=1 LITE=1 FORCE_OPENGLNONE=1 USE_MT32EMU=0 USE_FLUIDSYNTH=0 USE_IMGUI=0` (-O3) | 320x200 engines (SCUMM, AGI, SCI0-1.1, Kyrandia, Simon, Sky, Queen, Lure) full speed; 640x480/video-heavy games (The 7th Guest, SCI32, Broken Sword cutscenes) slower. | none | No libretro save states (ScummVM's own saves work). 31 MB. See [ScummVM](#scummvm). |
| Cave Story (`cavestory`) | nxengine (`libretro-nxengine`) | [`fd1c068`](https://github.com/libretro/nxengine-libretro/commit/fd1c0686f8b4c0aea9b5addbc077e3ad7da23bb7) 2026-08-22 | `platform=unix` (-O2) | Full speed with a large margin. | none | No save states (the game's own save points work). Game data pre-install is off by default (below). |
| SuperGrafx (`supergrafx`) | mednafen_supergrafx (`libretro-beetle-supergrafx`) | [`3c6fcd3`](https://github.com/libretro/beetle-supergrafx-libretro/commit/3c6fcd3deded54ebecd69408f108407ac03d11b5) 2026-04-20 | `platform=rpi2` (-DARM, Cortex-A7/NEON, -O2, -ffast-math) | Full speed: pce_fast's code with the second VDC; a little heavier. | `syscard3.pce` for CD games | Also offered per game in `pcengine`/`pcenginecd`; pce_fast stays their default. |
| ZX Spectrum (`zxspectrum`) | fuse (`libretro-fuse`) | [`e997e2b`](https://github.com/libretro/fuse-libretro/commit/e997e2bc32c888348f862f69f2c53babfedf7791) 2026-09-26 | `platform=rpi2` (Cortex-A7/NEON, -O3, -ffast-math) | Full speed with a large margin (3.5 MHz Z80). | none (ROMs built in) | Select opens the keyboard overlay. |
| Amstrad CPC (`amstradcpc`) | cap32 (`libretro-cap32`) | [`1abeaac`](https://github.com/libretro/libretro-cap32/commit/1abeaac1589156bc1c968015337595cce4d853d6) 2026-09-27 | `platform=rpi2` (Cortex-A7/NEON, -O3, -ffast-math, -mword-relocations) | Full speed with a large margin. | none (ROMs built in) | |
| Pokémon mini (`pokemini`) | pokemini (`libretro-pokemini`) | [`132111b`](https://github.com/libretro/PokeMini/commit/132111b76343559860532a1ccc094f93f1ed5650) 2026-07-31 | `platform=rpi2` (-DARM, Cortex-A7/NEON, -O2, -ffast-math) | Full speed with a large margin. | optional `bios.min` (FreeBIOS built in) | |
| Commodore 64 (`c64`) | vice_x64 (`libretro-vice`, `EMUTYPE=x64`) | [`9d79838`](https://github.com/libretro/vice-libretro/commit/9d7983826ea792f6cce7fdfe6c09488129c6f886) 2026-09-22 | `platform=unix` (-O3) | x64 (the fast emulator) full speed with reSID "fast" sampling (Pi 2 class hardware runs it); true drive emulation during loading costs extra (`vice_autoloadwarp` can warp through loading). x64sc (cycle-exact) is not built. | none (ROMs built in, see licences) | FastSID is the fallback if a game stutters. |

"Expected" is extrapolated the same way as for the other cores (Pi 2, Miyoo Mini). TODO(hw): measure each one.

Why these cores and not the alternatives:

- **cap32, not crocods.** crocods is lighter and MIT, but both are far below the CPU budget (a 4 MHz Z80), and cap32 is
  more accurate, runs the 6128+/GX4000 cartridges and has the better on-screen keyboard.
- **vice x64, not x64sc.** x64sc is cycle-exact and borderline on a 1 GHz A7; x64 runs almost the whole library.
- **fake08, not retro8.** fake08 is MIT (retro8 GPL-3.0) and is what the other A7 handheld firmwares ship.
- **dosbox-pure, not dosbox-svn/core.** Zip loading, gamepad-to-keyboard mapper, on-screen keyboard, save states,
  no config files.

### ROM folders and content (third batch)

- `doom`: `.wad` files. IWADs (`doom.wad`, `doom2.wad`, `plutonia.wad`, `tnt.wad`, `freedoom1.wad`, `freedoom2.wad`)
  and PWADs next to them; prboom picks the IWAD for a PWAD by its maps. Freedoom 1 and 2 are pre-installed
  (`rsos-freedoom`, docs/homebrew.md). The folder is `doom` rather than RetroPie's `ports/doom` because the frontend
  maps one folder to one system.
- `pico8`: `.p8` and `.p8.png` carts (the BBS downloads). Box art goes in `media/` or `images/` (not scanned).
- `dos`: **one `.zip` (or `.dosz`) per game**. The core mounts it as C:, shows its start menu the first time and
  remembers the executable. `.iso`/`.cue`/`.chd` CD images and `.img`/`.ima`/`.vhd` disk images also work. Unzipped game
  folders are not listed (the scan would show every `.exe`).
- `scummvm`: one folder per game with the game's data files and a text file `<Game name>.scummvm` holding the
  ScummVM game id (for example `roms/scummvm/Beneath a Steel Sky/Beneath a Steel Sky.scummvm` containing `sky`;
  ids on https://www.scummvm.org/compatibility/). The core starts the game from the folder of that file with the
  default settings. (It can also autodetect from any file of the game folder, but the frontend only lists the
  `scummvm` extension.) The freeware games (Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the
  Temptress, Drascula, Soltys, Dreamweb, Teen Agent) are redistributable and could be bundled later like Freedoom.
- `cavestory`: `Doukutsu.exe` with its `data/` folder (the freeware 1.0.0.6 English translation). Pre-installed only
  with `BR2_PACKAGE_LIBRETRO_NXENGINE_CAVESTORY=y`.
- `supergrafx`: `.sgx` (and `.pce` SuperGrafx dumps).
- `zxspectrum`: `.tzx .tap .z80 .sna .szx .rzx .dsk .trd .scl .dck`, zipped or not (the core reads zips).
- `amstradcpc`: `.dsk .sna .cdt .tap .voc .cpr`; the core autostarts disks.
- `pokemini`: `.min`.
- `c64`: `.d64 .g64 .t64 .tap .prg .p00 .crt` (and the other VICE formats), zipped or not; `.m3u` for multi-disk games.

`post-build.sh` creates all these folders on a fresh data partition (it reads the `systems` of every installed
`.ini`).

### Computer cores and input

DOS, C64, Spectrum and CPC games often need keys. The four cores draw their own on-screen keyboard, driven by the
RetroPad, so they are playable on the handheld without host changes: fuse, cap32 and vice open it with **Select**
(vice: "VKBD comes up with RetroPad Select by default"). The frontend passes a plain Select press to the core
(docs/review/feature-parity.md §2.2), so there is no conflict with the Select hotkeys.

**dosbox-pure uses RetroPad L3** for its menu and on-screen keyboard (`dosbox_pure_on_screen_keyboard`, L3 only),
and L3 is the optional C button that almost no RetroStone2 has (docs/input-design.md §1b). Without a remap, DOS
games that need keys other than the ones its gamepad presets map cannot open the keyboard. **Done (batch 2):**
`frontend/src/input/remaps/dos.ini` maps `r2 = l3` (the joystick presets do not use R2, the generic keyboard
preset maps it to the `4` key, which the on-screen keyboard also has); `dos-cz.ini` (units with C/Z fitted) keeps
the physical C button as L3 and R2 as R2. Its start menu (shown when a game starts)
works with the d-pad and A. A USB keyboard or mouse needs `RETRO_DEVICE_KEYBOARD`/
`RETRO_DEVICE_MOUSE` in the host (not implemented; frontend owner). ScummVM and dosbox-pure move a mouse cursor with
the stick/d-pad themselves.

### DOS (dosbox-pure)

- **CPU core.** `include/config.h` turns on DOSBox's dynamic recompiler from the compiler's target macros: the
  ARMV7LE backend on the RetroStone2 (`C_DYNREC`, `C_UNALIGNED_MEMORY`), ARMV8LE on aarch64, `dynamic_x86` on x86_64.
  The build log shows `core_dynrec.cpp` compiled with its ARM code generator (`gen_*`, `CPU_Core_Dynrec_Cache_Init`).
  The Makefile only guesses CPU flags from `uname -m` of the build host, so the package passes `MAKE_CPUFLAGS`
  itself (empty on ARM32: the wrapper's Cortex-A7 flags; `-DPAGESIZE=4096` on aarch64, as upstream does on an
  aarch64 host). No `-ffast-math` (upstream uses it only for the Pi 4).
- **Default options** (`dosbox_pure.ini`): `dosbox_pure_cpu_core = auto` (interpreter for real-mode games, dynrec
  for protected mode), `dosbox_pure_cycles = auto`, `dosbox_pure_audiorate = 32000` (cheaper OPL/Sound Blaster
  mixing than 44.1/48 kHz), `dosbox_pure_sblaster_adlib_emu = default` (not the heavier Nuked OPL3). The 2x A7 is a
  386DX-40/486SX class machine at best: for a game that runs too fast or stutters, set `dosbox_pure_cycles` to a fixed
  value per game (`4720` = 386/20, `7800` = 386DX/33, `13400` = 486/33). TODO(hw): measure a few titles.

### ScummVM

- **Source.** The libretro backend lives in ScummVM itself (`backends/platform/libretro`); libretro/scummvm is the
  synced tree. Its Makefile clones libretro-deps and libretro-common at build time
  (`scripts/configure_submodules.sh`). The package downloads the same pinned commits as `_EXTRA_DOWNLOADS` (hashed),
  unpacks them into `deps/`, and passes `DEPS_SUBMODULES=` so nothing is fetched during the build. All libraries
  (zlib, libpng, libjpeg, FLAC, Vorbis, MAD, FAAD, FreeType, FriBidi, libmpeg2, giflib, Theora) are compiled in; the
  `.so` only needs libc/libm/libstdc++.
- **Engines.** `LITE=1`, upstream's list for small devices (`lite_engines.list`, 31 engines): AGI, AGOS (+AGOS2),
  Cine, Cruise, Draci, Drascula, EOB, Gob, Groovie, HE, IHNM, Kyra, LOL, Lure, MADE, Mortevielle, Parallaction,
  Queen, SAGA, SCI, SCI32, SCUMM (+7/8), Sherlock, Sky, Sword1, Sword2, Teen Agent, Tinsel, Touché, Tucker. The full
  build has ~120 engines, most of them for games this device cannot run (3D, 640x480 video), and is several times
  bigger. The size is dominated by the shared code (graphics, GUI, audio, codecs), not the engines: the lite build is
  31 MB stripped.
- **Features off.** No OpenGL (`FORCE_OPENGLNONE=1`: the host gives GLES2 to the N64 cores only), no MT-32 emulation
  and no FluidSynth (both need extra data and much CPU: MIDI music uses the AdLib/OPL emulation), no ImGui debugger,
  no cloud.
- **Data files.** The core looks for `<system dir>/scummvm/extra` and `<system dir>/scummvm/theme`. Upstream ships them
  as `scummvm.zip`; the package installs the relevant part to `/usr/share/rsos/cores/scummvm/scummvm/` (6.5 MB) and
  `scummvm.ini` has `system_tree = /usr/share/rsos/cores/scummvm`: the engine data of the built engines
  (`drascula.dat`, `kyra.dat`, `lure.dat`, `mort.dat`, `queen.tbl`, `sky.cpt`, `teenagent.dat`), the core data
  (`achievements.dat`, `classicmacfonts.dat`, `encoding.dat`, `helpdialog.zip`, `macgui.dat`), the AGI predictive
  dictionary `pred.dic`, the virtual keyboard `vkeybd_default.zip`, and the `scummremastered`/`scummclassic` themes.
  Left out: `fonts.dat` (6 MB, TrueType GUI fonts for translated menus), `translations.dat`, the launcher icons.
- **Saves.** ScummVM saves to the libretro save directory; the core has no libretro save states
  (`savestates = false`).

### Licences (third batch)

Checked in the pinned trees.

| Package | Licence (checked in the tree) | Notes |
|---|---|---|
| `libretro-prboom` | GPL-2.0+ (`COPYING`) | `prboom.wad` compiled in (GPL) |
| `rsos-freedoom` | BSD-3-Clause (`COPYING.txt`) | Freedoom 0.13.0; `COPYING.txt`, `CREDITS.txt`, `CREDITS-MUSIC.txt` installed to `/usr/share/rsos/licenses/freedoom/` |
| `rsos-ucity` | GPL-3.0+ (`gpl-3.0.txt`), BSD-2-Clause (GBT Player, `source/engine/gbt_player.asm`), CC-BY-SA-4.0 (graphics, music; `readme.rst`) | µCity 1.2: the release ROM, the tag's source for legal-info; `LICENSES/homebrew/ucity/` installed to `/usr/share/rsos/licenses/ucity/` |
| `libretro-fake08` | MIT (`LICENSE.MD`, which also lists zepto8 WTFPL and tac08 MIT); z8lua MIT (Lua notice in `lua.h`); miniz MIT; lodepng zlib | not related to Lexaloffle; no PICO-8 code or data |
| `libretro-dosbox-pure` | GPL-2.0+ (`LICENSE`) | |
| `libretro-scummvm` | GPL-3.0+ (`COPYING`, `COPYRIGHT`); bundled code under BSD, LGPL, MIT, ISC, Lua, OFL (`LICENSES/`); libretro-deps libraries under their own licences | source offer as for the other GPL cores |
| `libretro-nxengine` | GPL-3.0 (`nxengine/LICENSE`) | Cave Story data: see below |
| `libretro-beetle-supergrafx` | GPL-2.0+; libchdr BSD-3-Clause; Tremor BSD-3-Clause; LZMA SDK public domain | |
| `libretro-fuse` | GPL-3.0 (libretro port, `LICENSE`), GPL-2.0+ (Fuse, libspectrum), bzip2 licence | Spectrum ROMs: Amstrad's statement in `fuse/roms/README.copyright` (distribution allowed, the ROMs may not be sold on their own; a product containing them may be) |
| `libretro-cap32` | GPL-2.0+ (`cap32/COPYING.txt`) | CPC ROMs built in: the same Amstrad statement (Cliff Lawson, 1999) covers the CPC firmware |
| `libretro-pokemini` | GPL-3.0+ (`LICENSE`) | FreeBIOS built in (GPL) |
| `libretro-vice` | GPL-2.0+ (`COPYING`) | **The embedded C64/1541 ROMs** (`vice/include/embedded`): `vice/README` says they are "Copyright (C) by Commodore Business Machines", with no redistribution grant. Every VICE release and the libretro buildbot ship them, but the rights holder never licensed them openly. Owner decision for the sold device; the package lists `vice/README` as a licence file so `legal-info` carries the notice. |

**Cave Story data.** The nxengine-libretro tree carries the freeware Cave Story 1.0.0.6 with the Aeon Genesis
translation (`datafiles/`). Its readme only says "This program is freeware"; Studio Pixel never published
redistribution terms, and Nicalis sells the commercial versions. The core package can install it
(`BR2_PACKAGE_LIBRETRO_NXENGINE_CAVESTORY`, to `/usr/share/rsos/homebrew/cavestory/`, 3.4 MB, seeded like the other
bundled games), but the option is **off** in the defconfigs: docs/homebrew.md requires a licence or a permission that
covers a commercial product. Owner decision.

### Verification (third batch)

Built through the scratch Buildroot tree (`~/rsos/cores-test/br-out3`, BR2_EXTERNAL `br-ext`, which now includes
`package/libretro-common.mk` like the real `external.mk`), Buildroot 2026.02.3, Bootlin armv7-eabihf glibc stable
2025.08-1 (GCC 14.3, glibc 2.41), `BR2_DOWNLOAD_FORCE_CHECK_HASHES=y`. After the `.hash` files were written, every
package was rebuilt from `-dirclean` (download and licence hashes checked) and `<pkg>-legal-info` passed.

| Core | Buildroot build time (16-core host, cold ccache) | .so (stripped) | NEEDED |
|---|---|---|---|
| prboom | 18 s | 2.51 MB | libm, libc |
| fake08 | 22 s (incl. git clone with submodule) | 0.72 MB | libstdc++, libgcc_s, libm, libc |
| dosbox_pure | 17 s | 2.51 MB | libstdc++, libgcc_s, libm, libc |
| scummvm | 177 s including the first download of its 3 archives (~140 MB) | 31.2 MB | libstdc++, libgcc_s, libm, libc |
| nxengine | 10 s | 0.72 MB | libstdc++, libgcc_s, libm, libc |
| mednafen_supergrafx | 11 s | 2.03 MB | libstdc++, libgcc_s, libm, libc |
| fuse | 12 s | 1.22 MB | libm, libc |
| cap32 | 9 s | 1.45 MB | libm, libc |
| pokemini | 7 s | 0.27 MB | libm, libc |
| vice_x64 | 20 s | 2.90 MB | libstdc++, libgcc_s, libm, libc |
| **total** | | **45.5 MB** (+ 6.5 MB ScummVM data, + 57.6 MB Freedoom) | |

For all ten: `ELF 32-bit LSB shared object, ARM, EABI5`, `Tag_CPU_name: 7-A`, VFPv4, NEON with FMA, `GNU_STACK RW`
(no `-z noexecstack` needed: no hand-written asm; the dynrecs emit code at run time into mmap'd buffers), no
TEXTREL, all 25 mandatory `retro_*` exported, every NEEDED library present in the target, and no incompatible-pointer,
implicit-declaration or int-conversion warnings in the logs. The effective flags were checked with `make -n -B`:
`-mcpu=cortex-a7 -mfpu=neon-vfpv4` and the optimisation levels listed in the table above.

**Root filesystem.** Full `retrostone2_defconfig` build with the third batch (and Freedoom, without Cave Story),
2026-09-27: `target/` grew from 195.4 MB to 307.6 MB (+112 MB: cores ~47 MB after Buildroot's strip, scummvm data
6.6 MB, Freedoom 57.6 MB). The 384 MiB rootfs of that time (`BR2_TARGET_ROOTFS_EXT2_SIZE`; 512 MiB since 2026-09-27) then had **16368 free 4 KiB blocks
(64 MiB, 17 %)**. That is enough for now but tight for an A/B slot: the next large additions need either a bigger
slot, dropping Freedoom Phase 1 or 2 (28 MB each), or moving the bundled games out of the rootfs.

**aarch64 (Raspberry Pi 4).** All ten were also built in `~/rsos/output-rpi4` (`rpi4_64_defconfig` toolchain, Cortex-A72):
`ELF 64-bit ARM aarch64`, `GNU_STACK RW`, no TEXTREL, the 25 `retro_*`, NEEDED libraries present (fuse also links
`libmvec`, from `-ffast-math` vectorised maths). `make -n -B` shows `-march=armv8-a+crc+simd -mtune=cortex-a72`
for supergrafx and fuse (`rpi4_64`) and `-mcpu=cortex-a72` for pokemini (`rpi4`); dosbox-pure's `core_dynrec.o`
has the ARMV8LE code generator; scummvm reports "Platform is unix 64bit" (`BUILD_64BIT=1`). Unstripped sizes are
10-30 % larger than on ARM32 (scummvm 40 MB before stripping). The x86_64 column is untested, like for the other
cores.

Not verified: running them. TODO(hw): load a game per system on the RetroStone2, check save states where supported,
the on-screen keyboards, and frame times (a protected-mode DOS game, a SCI32 game, a C64 game with disk loading).

## RetroStone (the RetroStone VC games)

The **RetroStone** system, first in the carousel, holds 8BCraft's own games for RetroStone VC, a virtual console
with the feel of a Super Nintendo. Each game is its own libretro core with the game built in, so there is no ROM
folder: full description in [vc-games.md](vc-games.md).

| Game | Core (package) | Source | Build flags | Expected on A20 | Save RAM | Save states |
|---|---|---|---|---|---|---|
| Bomber Mole (1 player) | bombermole (`rsos-vc-games`) | RetroStone VC `games/bombermole` (code MIT, assets CC BY-NC-SA 4.0) + SDK (MIT) | the RetroStone VC `Makefile`'s cross target with Buildroot's `CC`/`CFLAGS` (-O2; the wrapper's Cortex-A7 NEON flags) | designed for 60 fps on one 1 GHz A7: RetroStone VC's spec estimates 6-8 ms per frame on average, 13-17 ms for the worst frame of the heaviest scenes (host `make bench` x15-x20); TODO(hw): measure | `/data/saves/retrostone/Bomber Mole.srm` (32 KiB) | yes: the SDK saves the whole console (about 350 KB), `/data/states/retrostone/Bomber Mole.state*` |
| Leady Squid (1-2 players) | leadysquid (`rsos-vc-games`) | RetroStone VC `games/leadysquid` (code MIT, assets CC BY-NC-SA 4.0) + SDK (MIT) | the same | the same renderer and 60 fps target (4 layers, raster effects, colour math); TODO(hw): measure | `/data/saves/retrostone/Leady Squid.srm` | yes (about 300 KB) |

- **Metadata**: `no_content = true` (see [Core metadata files](#core-metadata-files)), `systems = retrostone`,
  `extensions = <core id>` (each menu entry's extension picks its core), `savestates = true`: the auto state, the
  resume, the in-game slots and the game switcher work as for any core. A state never holds the `.srm`, and a state
  of another build of the game is refused ([vc-games.md](vc-games.md), "Save states").
- **Architectures**: one build line for every board (no platform switch in the Makefile): Cortex-A7 armhf on the
  RetroStone2/RetroStone1 and the 32-bit boards, aarch64 elsewhere. NEEDED: libc, libm only.
- **Licences**: the game code, the SDK and the tools are MIT; the games' art, music, sound and levels are CC BY-NC-SA
  4.0. `legal-info` saves the source. The SDK's third-party code is MIT / public domain (libxmp-lite 4.7.3, stb,
  libretro.h).
- Source: the public [RetroStone VC](https://github.com/PaddleStroke/RetroStoneVC) repository, checked out by CI
  ([ci.md](ci.md) section 4).
