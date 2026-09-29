# RetroStoneOS: master requirements checklist

What a shippable firmware must do, now that RetroPie, RetroArch, EmulationStation and Armbian no longer supply it for
free. Every item is either **provided by our own code** or **dropped on purpose**.

Snapshot of the repo on 2026-09-25. Docs that existed then: CONVENTIONS, build, cores, hardware-pinmap, input-design,
kernel-patches, display-design. `docs/ui-design.md` did not exist yet. Frontend code that existed: `display.c`,
`uevent.c`, `input/input.h`, `ui/util.c`, `ui/xml.c`, `gfx/`, `theme/`. There is no libretro host, audio, power or
main loop yet.

**Legend**
- Status: **C** = covered (ref), **P** = partial (ref, and what is missing), **M** = missing, **D** = dropped on purpose.
- Priority: **P0** = needed for the first usable release, **P1** = soon after, **P2** = nice to have.
- The architecture constraints behind every "how": a single C frontend, a libretro host through dlopen, no
  RetroArch/SDL/udev/systemd, Buildroot, a read-only root plus exFAT `/data`, boot speed first, and non-essential
  features off by default.

Sources for other firmwares: RetroPie/RetroArch docs and the production `reference/retrostone-rop/retroarch.cfg`, plus
the MinUI/minarch, Onion, muOS, KNULLI, ROCKNIX and Batocera feature lists and issue trackers, where power-loss save
corruption, crackle, sleep, clock and update complaints recur. A20 suspend status: [linux-sunxi Suspend-to-RAM](https://linux-sunxi.org/Suspend-to-RAM).

---

## 1. Libretro host

### 1.1 Environment callbacks the shipped cores actually call
I got this list by grepping the pinned sources in `~/rsos/cores-test/<core>` (without libretro-common/deps).
`libretro_core_options.h` adds `GET_CORE_OPTIONS_VERSION` and `SET_CORE_OPTIONS{,_V2,_INTL}`/`SET_VARIABLES` to every core.

| Callback | Used by | Pri | How |
|---|---|---|---|
| `SET_PIXEL_FORMAT` | all | P0 | Accept RGB565/XRGB8888/0RGB1555, then `display_set_game_surface()` (C: display-design §5.5) |
| `GET_SYSTEM_DIRECTORY`, `GET_SAVE_DIRECTORY` | all but snes9x2005 | P0 | `/data/bios`, `/data/saves/<system>`. Create them. Arcade cores write subdirs (`mame2003-plus/nvram,hi,cfg`, `fbneo/`) |
| `GET_VARIABLE`, `GET_VARIABLE_UPDATE`, `SET_VARIABLES`, `GET_CORE_OPTIONS_VERSION`, `SET_CORE_OPTIONS(_V2/_INTL)`, `SET_CORE_OPTIONS_DISPLAY`, `SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK` | all | P0 | Report version 2. Parse V2 (with categories and info text) and V1, keep `SET_VARIABLES` as the fallback. Hidden options are not shown. See 1.2 |
| `SET_INPUT_DESCRIPTORS`, `SET_CONTROLLER_INFO` | all | P1 | Labels for the remap screen. Port device choice (PS1 DualShock, MD 6-button, multitap) |
| `GET_INPUT_BITMASKS` | all | P0 | true. Return `input_port_buttons()` for `JOYPAD_MASK` (C: input.h) |
| `GET_LOG_INTERFACE` | all | P0 | Route to the frontend log ring (see 12) |
| `SET_GEOMETRY`, `SET_SYSTEM_AV_INFO` | pcsx, picodrive, fceumm, gambatte, gpsp, fbneo, mame | P0 | Geometry: `display_set_frame_size()` (C). AV info (PAL/NTSC switch, PS1 region): re-run the timing policy of 1.4 |
| `GET_CAN_DUPE` | pcsx, snes9x2010, gambatte | P0 | true. A NULL frame means repeat the last frame (no flip) |
| `SET_AUDIO_BUFFER_STATUS_CALLBACK`, `SET_MINIMUM_AUDIO_LATENCY` | pcsx, snes9x×2, picodrive, gpsp, mame, fbneo | **P0** | Call it before each `retro_run()` with the ALSA fill level (0-100) and an underrun-likely flag. The cores' **auto frameskip** depends on it, and it is what makes PS1/arcade playable on a 1 GHz A7. Honour the minimum latency by raising the ALSA buffer |
| `GET_PERF_INTERFACE` | snes9x2005, gpsp, mame | P0 | Minimal implementation (`get_time_usec`, `get_cpu_features` = NEON, no-op counters). Some cores call it without checking |
| `SET_DISK_CONTROL_INTERFACE`, `SET_DISK_CONTROL_EXT_INTERFACE`, `GET_DISK_CONTROL_INTERFACE_VERSION` | pcsx, picodrive | P1 | Disc swap in the in-game menu (eject, choose, insert) for `.m3u`. Save the disc index with the state |
| `GET_RUMBLE_INTERFACE` | gambatte, gpsp, pcsx | P2 | Forward to evdev FF_RUMBLE (needs kernel FF, see 2) |
| `SET_ROTATION` | mame2003+, fbneo | P1 | The A20 planes cannot rotate. Either return false (the cores then rotate in software; check that for each core) or rotate with NEON during the present copy. Check a vertical shooter |
| `SET_MESSAGE`, `SET_MESSAGE_EXT`, `GET_MESSAGE_INTERFACE_VERSION` | most | P1 | Toast on the OSD overlay plane. Version 1 |
| `GET_TARGET_REFRESH_RATE` | fbneo | P1 | Return the rate the host paces at (not the LCD's 78.6) |
| `GET_SAVESTATE_CONTEXT`, `SET_SERIALIZATION_QUIRKS` | fbneo, snes9x2010 | P1 | NORMAL. Honour INCOMPLETE (run 1 frame before serialize) and variable size |
| `GET_GAME_INFO_EXT`, `SET_CONTENT_INFO_OVERRIDE` | fceumm, picodrive, snes9x2010 | P1 | Return false at first (cores fall back). Implement later to pass zip contents from memory |
| `GET_CURRENT_SOFTWARE_FRAMEBUFFER` | fceumm, snes9x2010, pcsx | P2 | Return false. The scanout buffers are write-combined, so a core that reads back would be slow (display-design §6) |
| `SHUTDOWN` | fbneo | P0 | Treat it like the exit hotkey |
| `GET_LANGUAGE` | fceumm, gambatte, fbneo | P2 | ENGLISH, or the UI language |
| `GET_VFS_INTERFACE` | most | P2 | Return false (stdio). Fine on local files |
| `GET_AUDIO_VIDEO_ENABLE` | snes9x×2, fbneo | P1 | Bit 0 (video) off during fast-forward frames that are not shown |
| `SET_PERFORMANCE_LEVEL`, `SET_MEMORY_MAPS`, `SET_SUPPORT_ACHIEVEMENTS`, `GET_LED_INTERFACE`, `SET_SUBSYSTEM_INFO`, `SET_NETPACKET_INTERFACE`, `SET_FASTFORWARDING_OVERRIDE`, `SET_SAVE_STATE_DISABLE_UNDO`, `GET_HW_RENDER_INTERFACE` | various | P2 | Accept and ignore (return true or false as the spec says). Log unknown calls once |
| Unknown or experimental calls | all | P0 | Return false and log once (`ui_log_once`) |

### 1.2 Settings, content and saves

| Item | Status | Pri | How |
|---|---|---|---|
| Core options per system and per game | M | P0 | `/data/rsos/coreopts/<core>.ini` plus `<core>/<game>.ini` (only the keys that differ). Global → core → game. Written atomically (see 5) |
| Curated option defaults (e.g. pcsx `drc` on, `neon_enhancement` off; gpsp `bios=auto`; mame "skip disclaimer/warnings"; fbneo frameskip auto) | P (cores.md notes) | P0 | A `[defaults]` section in `/usr/share/rsos/cores/<core>.ini` applied before the user file |
| Per-system default core and per-game core override (snes9x2005 → 2010) | P (cores.md "ROM folders") | P0 | The ini `systems=` list; the per-game override stored with the game options. Replaces runcommand |
| BIOS check before launch with a clear message | P (md5 table in cores.md and the ini `[bios:*]`) | P0 | Before dlopen: check `required=` files. If missing: "PlayStation: scph5501.bin missing in /data/bios (optional, HLE BIOS used)" or a blocking message for FDS/Sega CD. A BIOS status screen (found/wrong md5/missing) |
| SRAM (`.srm`) load and flush timing | M | **P0** | Load after `retro_load_game`. **Flush periodically**: every 2 s compare `RETRO_MEMORY_SAVE_RAM` with the last write (memcmp/hash). If it changed and has been stable for 1 s, write it with temp + fsync + rename + fsync(dir). Also flush on menu open, exit, power key, low battery and SIGTERM. RetroArch only writes at unload (`autosave_interval=0` in the old config), which is why saves are lost when the battery dies |
| `.rtc` for GB/GBC MBC3 carts (Pokémon G/S/C) | M | P0 | `RETRO_MEMORY_RTC` saved next to the `.srm`, same timing |
| Save states: slots 0-9, auto slot, with thumbnails | P (hotkeys in input-design §4) | P0 | `/data/states/<system>/<game>.state{,1..9,.auto}` (RetroArch names) plus `.png` (stb_image_write of the last frame, core resolution). Written atomically. Keep the previous file as `.bak` (undo) |
| Auto-save state on exit and resume on launch (the MinUI/Onion feature) | M | P1 | Setting, default on for handheld use. Write `.state.auto` on exit, sleep-timeout, low battery. Offer "Resume?" on launch |
| Save compatibility with RetroArch naming | M | P0 | Basename of the content (for `.m3u` the m3u name, for zip the zip name) + `.srm`/`.state`. See 14 |
| Load RetroArch-compressed states (RZIP `#RZIPv`) | M | P2 | Only for migration. Decompress with zlib |
| Zipped ROMs | M | P0 | Cartridge systems: extract the first matching member to RAM (tmpfs) with miniz (MIT). Pass a buffer if `need_fullpath=false`, else a `/tmp` path. Arcade: pass the zip as is (`block_extract=true`, C: ini). Refuse archives > 64 MB. 7z: P2 (LZMA SDK, public domain) |
| Disc games: `.cue/.bin`, `.chd`, `.pbp`, `.m3u` multi-disc | P (pcsx ini extensions) | P0 | Hide `.bin` files referenced by a `.cue` and discs referenced by an `.m3u` in the list. Disc swap: see 1.1 |
| Per-core quirks | P (cores.md) | P0 | PicoDrive: region auto, Sega CD `bios_CD_U/E/J.bin` per disc region, 3/6-button pad option, PAL = 50 Hz (see 1.4). gpsp: `gba_bios.bin` optional, some games need it (message). pcsx_rearmed: HLE BIOS warning, one memory card per game in `.srm`, card 2 in the save dir, DualShock port type for analog games. mame2003+/fbneo: `neogeo.zip` in the ROM folder or `<system>/fbneo/`, samples/hiscore.dat optional, vertical games. fceumm: `disksys.rom` for `.fds` |
| Arcade display names (`sf2.zip` → "Street Fighter II") | M | P1 | Ship a name table generated from the mame2003-plus XML and the FBNeo DAT at build time. Hide BIOS sets (`neogeo.zip`, `pgm.zip`) and optionally clones |
| HW-render GLES2 (N64) | D (cores.md "N64 verdict") | - | No `SET_HW_RENDER`. Answer false. N64 folders from RetrOrangePi are not supported: tell users |
| Fast-forward | M | P1 | Hotkey toggle (proposal: Select+R2): run `retro_run` up to N times per vblank (setting, default 3x) with video disabled except the last frame and audio dropped. RetroPie had no FF hotkey by default, so users do not expect it, but it is cheap |
| Rewind | D | - | Too much RAM bandwidth and CPU on the A20. RetroPie had it off too |
| Cheats | D (P2) | P2 | Optional later: `.cht` → `retro_cheat_set` |
| RetroAchievements, netplay, shaders, overlays/bezels | D | P2 | No GPU path, WiFi off by default. Bezels on HDMI would be a P2 overlay plane |
| Core crash isolation | M | **P0** | **Run each game in a child process** (`rsos-frontend --run <core> <rom>`, fork+exec of the same binary). Reasons: (1) a core segfault must not kill the menu; (2) many cores keep static globals and do not support a second `retro_load_game` or dlclose/dlopen in the same process (RetroArch has per-core issues with this, and MinUI re-executes minarch per game); (3) fbneo (59 MB) and PS1 memory are freed on exit. The parent drops DRM master (`drmDropMaster`), the child opens the card and becomes master, and on exit the parent takes master back and redraws. The parent `waitpid()`s: on SIGSEGV/SIGBUS/SIGILL it shows "Emulator crashed", saves the log, and loses nothing except SRAM since the last periodic flush. Cost: one fork+exec+dlopen (tens of ms) |
| Hung core | M | P1 | The parent keeps reading evdev (without grabbing). Holding Select+Start for 5 s sends SIGKILL to the child |

### 1.3 Audio path (libretro side; the hardware side is in 4)

| Item | Status | Pri | How |
|---|---|---|---|
| ALSA output, fixed 48 kHz S16 stereo on both cards | M | P0 | One output rate for the codec and HDMI (0002 offers S16 only). Never switch between the 44.1/48 kHz families (PLL2 relock, pop) |
| Resampler core rate → 48 kHz | M | P0 | Cores output ~32 kHz (SNES), 32768/65536 Hz (GB/GBA), 44.1/48 kHz, etc. Use a NEON linear or 4-tap polyphase resampler (RetroArch sinc q3 is too heavy here; MinUI uses linear) |
| Dynamic rate control (DRC) | M | **P0** | Steer the ratio by ±0.5% (RetroArch `audio_rate_control_delta=0.005`, as in the production config) from the ALSA fill level, aiming at a half-full buffer |
| Latency and underruns | M | P0 | Default ~48-64 ms (production: 72 ms), setting. Non-blocking `snd_pcm_writei` from the main loop. On -EPIPE: `snd_pcm_recover` and pre-fill silence. Fade the last period on underrun to avoid clicks. Log the underrun count |
| Audio thread priority | M | P1 | If a separate audio thread is ever used: SCHED_FIFO low priority. pcsx already runs 2 threads on the 2 cores |

### 1.4 Frame timing and A/V sync

| Item | Status | Pri | How |
|---|---|---|---|
| Know the real display rate | C (`display_output_info.refresh_mhz`, display-design §3) | P0 | - |
| **Vsync-locked mode** when \|core_fps − display_Hz\| / display_Hz ≤ 1% (setting; RetroArch default 5%) | M | P0 | One `retro_run` per vblank, paced by `display_begin_frame()`. Resample audio at `core_rate × display_Hz / core_fps` plus DRC. Covers NES/SNES 60.10, GB/GBA 59.73, MD 59.92, PS1 59.94 on a 60 Hz output |
| **Audio-clocked mode** otherwise (PAL 50 Hz, arcade 54-59 Hz, **the LCD at 78.6 Hz**) | M | P0 | Run the core at its own fps, paced by audio/CLOCK_MONOTONIC. Each vblank shows the newest finished frame (duplicates/drops = judder). Without this, games run 31% fast on the current LCD |
| Retime the LCD to 60 Hz | P (TODO(hw) in pinmap and display-design) | **P0** | pclk 25.2 MHz with the same 800×525 totals. Check for flicker and panel limits |
| LCD refresh follows the core (50 Hz PAL, 54.7/57.5/59.2 Hz arcade) | M | P2 | We own TCON0. Set a custom mode with pclk = fps × htotal × vtotal (PLL granularity: tune vtotal). Low-judder PAL/arcade on the LCD, which RetroArch cannot do on most devices. HDMI stays at 60 (or 50 for PAL via CEA 720p50) |
| Input latency | P (input-design §6: poll right before `retro_run`) | P1 | Double buffering by default (C: display-design §6). Measure button-to-photon with a 240 fps phone camera |
| Frame-time stats | P (display drop counter) | P1 | Per-game FPS and drop and underrun counters in the log, and an optional on-screen FPS (the old ES/RetroArch had "show framerate") |

---

## 2. Input (gaps in `docs/input-design.md`)

| Item | Status | Pri | How |
|---|---|---|---|
| Kernel drivers for the pads the design promises | **M** (`linux.fragment` only has `JOYSTICK_XPAD`, `HID_GENERIC`) | **P0** | Add `HID_SONY`, `HID_PLAYSTATION`, `HID_NINTENDO`, `HID_MICROSOFT`, `HID_8BITDO`(if present), `JOYSTICK_XPAD_FF`, `INPUT_FF_MEMLESS`, `HID_*_FF`, `HID_STEAM`(opt). As modules loaded on the USB/BT uevent modalias (a tiny modalias → `modprobe` in the frontend), or built in (size vs boot cost) |
| Bluetooth pads survive a reboot | **M** (build.md: pairings on tmpfs, `TODO`) | **P0** if BT is advertised | exFAT cannot store `AA:BB:..` names. Loop-mount an ext4 image `/data/rsos/state.img` (16 MB, created on first use) on `/var/lib` for bluetooth, dropbear keys, random seed. Only when BT/network is turned on, never at boot |
| BT pairing UI | M | P1 | Scan/pair/trust/forget screen driving `bluetoothctl` or the D-Bus API. Auto-reconnect on BT on |
| Select pass-through semantics | P (input-design §4 "Select alone reaches the game") | P0 | Decide: send Select to the core at once (RetroPie behaviour; games see Select during combos) or on release when no combo fired (adds latency). Recommend: at once, and document it |
| Accidental exit | M | P0 | Select+Start is too easy to hit in-game (a common RetroPie complaint). Either write the auto state on exit (1.2) or require holding for 0.5 s. Never exit without flushing SRAM (C in the design) |
| Hotkeys not yet assigned | P | P1 | Fast-forward (Select+R2), screenshot (Select+L2), and on HDMI **brightness keys → volume** (the backlight is off and the wheel does not affect HDMI, see 4) |
| Analog stick calibration and deadzone | P (DTS `abs-flat`, "not fitted" rule) | P1 | Calibration screen (center/extent) saved to `/data/rsos/input/builtin-stick.cfg`. Per-pad deadzone |
| On-screen keyboard | M | P1 | Needed for the WiFi PSK, renaming, search |
| Per-game and per-core remaps | P (`input_load_remap()` in input.h) | P1 | `/data/rsos/remaps/<system>/<game>.cfg` or `<system>.cfg`. The hotkey button stays the physical Select whatever the remap |
| D-pad ↔ analog per system | P (`input_set_dpad_to_analog`) | P1 | Default on for PS1 analog-only games when no stick is fitted |
| Turbo buttons | M | P2 | Per-port toggle in the remap screen |
| USB keyboard in the UI | C (input-design §2) | - | - |
| Input self-test | M | P1 | See 12 |

---

## 3. Display

| Item | Status | Pri | How |
|---|---|---|---|
| LCD ↔ HDMI switch in the menu and mid-game | C (display-design §2-4, `display.c`) | P0 | Libretro host: **pause the core during the switch** (≤ 1 s), keep the AV info, re-apply scaling. Audio: 4 |
| HDMI on CRTC0 + fe0 and HDMI audio | P (TODO(hw) in display-design §10, 0002 EXPERIMENTAL) | P0 | Hardware checklist first. If HDMI audio fails: say "HDMI audio not supported" rather than silence |
| 60 Hz LCD timing | P (TODO(hw)) | **P0** | See 1.4 |
| Scaling modes aspect/integer/stretch | C (display.h, display-design §5.3) | P0 | Per-system defaults: GB/GBC integer (3× = 480×432), GBA aspect, others aspect with the core aspect. Setting per system and per game |
| Sharp vs smooth | P (backend ×2 sharp, frontend filtered) | P1 | Say so in the UI. The production RetroArch had `video_smooth=true`, so people are used to smooth |
| TV overscan | P (checklist item in display-design §10) | P1 | "HDMI safe area" setting 0-10% that shrinks the plane dest rect. P2: IT-content AVI bit so TVs stop cropping |
| Screenshots | M | P1 | Hotkey → PNG of the core frame (not the screen) in `/data/screenshots/<system>/<game>-<YYYYMMDD-HHMMSS>.png`. Needs a sane clock (see 7), otherwise use a counter |
| Brightness | P (keys + `input_brightness_set`) | P0 | Persist in settings. Never allow 0 (minimum ~5%). Restore at boot before the first frame |
| Idle dim / screen off | M | P1 | See 5 |
| Vertical arcade games | M | P1 | See `SET_ROTATION` in 1.1. On a 4:3 LCD, rotated games are pillarboxed |
| First frame and boot splash | P (no fbdev/U-Boot video on purpose) | P0 | See 11 |
| Backlight vs first modeset | P (panel `backlight = <&backlight>`, drm_panel enables it) | P1 | TODO(hw): check that there is no white or garbage flash between the pwm-backlight probe and the first commit. Otherwise boot with brightness 0 and let the frontend light it |
| HDMI-CEC (TV on, input switch) | D (P2) | P2 | `DRM_SUN4I_HDMI_CEC=y` exists in sunxi_defconfig. Off |

---

## 4. Audio (hardware side)

| Item | Status | Pri | How |
|---|---|---|---|
| Codec level at boot | P (`rcS` sets PA volume 56, TODO(hw)) | P0 | Tune it on hardware so the wheel covers its range without clipping at max |
| Routing when switching LCD ↔ HDMI | P (the display audio callback, display-design §4) | P0 | RELEASE: drain/close the PCM. ACQUIRE: open `sun4i-hdmi` or the codec at 48 kHz. Keep the resampler state. Mute ~50 ms around the switch |
| Pops and clicks | M | P0 | The PAM8302 amp cannot be muted by software (pinmap: `/SD` = EXTEN). So: (1) open the codec PCM once at frontend start and **keep it open, playing silence**, so DAPM never powers the DAC down and up (or raise `pmdown_time`); (2) ramp volume in software on pause, menu, exit; (3) before power-off, set `Power Amplifier Mute Switch` off, then `DAC Playback Switch` off (rcK) |
| Volume on HDMI | M | P1 | The analog wheel only acts on the jack and speaker. Use software gain in the mixer, with the brightness keys as volume while on HDMI. Persist the HDMI volume |
| Sample rates | P (0002: 32-192 kHz S16) | P0 | 48 kHz fixed, see 1.3 |
| UI sounds | D (P2) | P2 | Off by default (ES "navigation sounds") |
| Microphone | D | - | MICIN1 is wired but no use |
| USB audio or BT audio (A2DP) | D | P2 | No bluez-alsa/pipewire. `SND_USB_AUDIO` module only if asked for |

---

## 5. Power

| Item | Status | Pri | How |
|---|---|---|---|
| Battery % and charging state | M (driver on: `BATTERY_AXP20X`, DTS `battery_power_supply`) | P0 | `/sys/class/power_supply/axp20x-battery/{capacity,status,voltage_now,current_now}`. Read every 10 s and on `SUBSYSTEM=power_supply` uevents (the same netlink socket as DRM). Smooth the value. Icon in the UI and in the in-game menu |
| Battery description in the DTS | M | P1 | `monitored-battery` (simple-battery: `voltage-min-design-microvolt`, `constant-charge-current-max-microamp`, capacity). Without it the AXP keeps its reset-default charge current. TODO(hw): cell capacity and connector BC-75 (U$18) |
| Low-battery warning | M | P0 | A toast at 15% and 7%, and a battery icon on the OSD. Repeat every 5 min |
| **Automatic save + clean shutdown at critical battery** | M | **P0** | At ≤ 3% or V < 3.45 V under load (the AXP hard cut V_OFF, REG31, defaults to 2.9 V and cuts without warning): pause, flush SRAM, write `.state.auto`, sync, `poweroff`. Show "Battery empty, game saved" for 2 s |
| Clean power-off path | P (rcK: SIGTERM, 3 s, sync, umount; `IN_HK_POWER_OFF`) | P0 | The frontend handles SIGTERM (flush) and forwards it to the game child. `axp20x` registers the power-off handler unconditionally in 6.18 (checked `drivers/mfd/axp20x.c:1459`), so `poweroff` really cuts power |
| Charging indication while off | C in hardware (AXP CHGLED → LED1 blue) | - | Document it |
| **Plugging the charger boots the unit** | M | P1 | The AXP209 powers on when ACIN is inserted (TODO(hw) confirm with REG00 bit 0, "boot source ACIN/VBUS"). If that is the boot reason and PEK is not pressed: show a charge screen (battery %) for 5 s, then power off, or stay in a dark charge-only state. Every Allwinner handheld firmware needs this (muOS/KNULLI have a charge mode) |
| Sleep / suspend | D for real suspend: **A20 suspend-to-RAM is not in mainline** ([linux-sunxi](https://linux-sunxi.org/Suspend-to-RAM)) | P1 | **Fake sleep** on a PEK short press: pause the core, mute, CRTC off (backlight off), governor powersave (144 MHz), WiFi/BT off, wake on PEK. After N minutes asleep (default 10): auto state + power off (MinUI/Onion behaviour). TODO(hw): measure mA awake, asleep, off |
| Idle dimming / screen off | M | P1 | Menu: dim at 2 min, off at 5 min. In game: only after 10 min without input (cutscenes). Any key wakes it and is not passed on |
| CPU governor per core | P (rcS: schedutil; the fragment has performance) | P1 | Game child: `performance` (schedutil ramp-up causes frame drops) for pcsx/fbneo/mame/snes9x2010/picodrive-32X. `schedutil` with max 720 MHz for NES/GB/SMS to save battery (setting in the core ini). Menu: schedutil |
| Thermal | P (sun7i-a20.dtsi: 75 °C passive, 100 °C critical on `rtp`; `TOUCHSCREEN_SUN4I=y`) | P1 | Read `thermal_zone0/temp`. Log it, show it in diagnostics, toast when throttling starts. TODO(hw): temperature after 30 min of PS1 in the closed case |
| PEK timings | P (input-design §5: long press 2 s, AXP 6 s force-off) | P1 | Check the AXP209 REG36 values (long-press time, auto-shutdown enable and time) and set them from the frontend or the DTS if needed |
| Power budget | M | P1 | TODO(hw): mA in menu, NES, PS1, HDMI, WiFi on, asleep. Runtime estimate in the "About" screen |

---

## 6. Storage and data safety

| Item | Status | Pri | How |
|---|---|---|---|
| Atomic, durable writes | **P** (`file_write_atomic()` in `ui/util.c` does write + close + rename with **no fsync**) | **P0** | temp → `fsync(fd)` → `rename` → `fsync(dir fd)`. Use it for settings, SRAM, states, options, remaps. Without fsync, a rename can leave a 0-byte file after a power cut |
| Dirty-page writeback | M | P0 | rcS: `vm.dirty_expire_centisecs=200`, `vm.dirty_writeback_centisecs=100` (the default 30 s window loses whatever the user saved just before the battery died). Free, no boot cost |
| exFAT dirty flag and fsck | M | P1 | exFAT has no journal. The Linux driver sets VolumeDirty on mount and warns when mounting a dirty volume, and Windows then asks "Scan and fix". In `data-partition`: read the boot sector VolumeFlags (1 sector, ~0 ms). **Only if dirty**, run `fsck.exfat -p` (exfatprogs 1.2.9 repairs some errors) and show the reason on the next UI start. TODO(hw): fsck time on a full 256 GB card with 20k files; if > 10 s, run it only on user request |
| Mount options | P (`noatime`) | P0 | Add `errors=remount-ro`, `iocharset=utf8`, `fmask/dmask` irrelevant as root. No `sync` (too slow, wears the card) |
| Read-only root: paths that expect to be writable | P (BlueZ → /run) | P0 | `/etc/resolv.conf` → `/run/resolv.conf` (udhcpc), `/etc/localtime` → `/run` or the TZ env, `/var/log` → tmpfs, dropbear keys and bluetooth → `state.img` (see 2). Audit with `find / -xdev -newer` after a session |
| First-boot partition creation | C (`data-partition`, build.md) | P0 | Gaps: (1) a **black screen for the whole first boot** (sfdisk + mkfs + folder creation happen before the frontend): time it on 4/64/256 GB and show a "Preparing SD card" frame if > 2 s; (2) exFAT cluster size: default for > 32 GB is 128 KiB, fine; (3) a 4 GB card leaves 3.4 GB: fine; (4) MBR limit 2 TiB: fine |
| Folder list consistency | **P** (`data-partition` creates `atari2600`, `pcengine` without cores and misses `fds sega32x segacd sg1000 fbneo`; cores.md table differs) | P0 | Single source: generate `SYSTEMS` at build time from the core ini `systems=` lists (post-build.sh) |
| User reformatted p2 as FAT32, or other layouts | M (`data-partition` mounts only exFAT and never reformats) | P1 | Try `exfat` then `vfat`. If neither mounts: the UI shows "Data partition unreadable: repair / reformat (erases)" instead of a menu with no games |
| Windows asks to format the ext4 root partition | M | P1 | TODO: check on Windows 10/11 whether p1 (type 0x83) gets a drive letter and the "You need to format the disk" prompt (a common complaint with Raspberry Pi cards). Mitigations: a docs warning, or order the MBR so that the exFAT entry comes first |
| macOS/Windows junk files | M | P0 | The scanner ignores dotfiles, `._*`, `.DS_Store`, `System Volume Information`, `$RECYCLE.BIN`, `Thumbs.db`, `desktop.ini`, and `.srm/.state*/.png/.txt/.dat/.cfg` in ROM folders |
| Path length and charset | P (`util.c` uses 4096-byte buffers) | P0 | Never 256-byte path buffers. UTF-8 everywhere. Extensions compared case-insensitively (exFAT is case-insensitive, so `.CUE` references still resolve). Names with `& ' # ,` and Japanese work. The font: DejaVu has no CJK, so show tofu or a fallback (P2: a CJK subset) |
| Large collections and boot speed | M | P1 | Cache the scan per system (`/data/rsos/cache/<system>.idx`, keyed by directory mtime and entry count). Never scan all folders before the first frame |
| ROMs on USB sticks | M (no `USB_STORAGE` in the kernel config) | P2 | `USB_STORAGE` module + a block uevent in the frontend → mount read-only at `/media/usbN` (exfat/vfat) → scan `roms/`. Saves stay on the SD |
| SD removal while running | D | - | The card is the root filesystem. Nothing to do. Document it |
| Install to eMMC (Pro) | D (P2) | P2 | Out of scope for now. The overlay exists |

---

## 7. Time

**Schematic check** (`eagle_nets.py` on v1.15): the A20 has a **32.768 kHz crystal Q2 (MC-306)** on
CLK32K_IN/OUT. VDD_RTC = AXP209 **LDO1** (the always-on RTC LDO). The AXP209 **BACKUP pin goes only to a test pad
(net N$30 → `BACKUP.PP`): there is no coin cell or supercap.** So the RTC keeps time while the unit is "off" as long as the
main battery (U$18) is connected and not fully depleted. It **loses the time when the battery is unplugged or dies**.
The kernel has `RTC_DRV_SUNXI=y` (sunxi_defconfig), and `RTC_HCTOSYS` sets the clock at boot.

| Item | Status | Pri | How |
|---|---|---|---|
| RTC runs on the external crystal | M | P1 | TODO(hw): check that LOSC_CTRL (0x01c20c00) bit 0 = external 32k after boot. Measure drift over 24 h. The internal RC is ±30% |
| Invalid-clock fallback | M | P1 | At shutdown and every 10 min, save the time to `/data/rsos/clock`. At boot, if the RTC is earlier than that (reset to its epoch after a dead battery), set the clock from the file and flag it "unset". Onion does the same on RTC-less devices |
| Timezone | M | P1 | A POSIX `TZ=` string in settings (no tzdata package). The UI offers ~40 zones |
| Manual date/time screen | M | P1 | Set the clock, then `hwclock -w` (BusyBox) |
| NTP when WiFi is on | M | P1 | `rsos-net`: after DHCP, `ntpd -q -n -p pool.ntp.org`, then `hwclock -w` |
| Why it matters | - | - | Save and screenshot timestamps, file mtimes seen on the PC, "last played" and "recent" lists, log timestamps, **TLS certificate checks** (scraping and updates fail with a 2010 clock) |

---

## 8. Networking (off by default)

| Item | Status | Pri | How |
|---|---|---|---|
| WiFi, Ethernet and BT on/off at runtime, off at boot | C (`rsos-net`, build.md) | - | - |
| WiFi setup UX | M (only a hand-written `/data/rsos/wpa_supplicant.conf`) | P1 | UI: scan list (`wpa_cli scan_results`), PSK through the on-screen keyboard, status and IP. Keep "drop a wpa_supplicant.conf on the card" as the power-user path. The AP6210 is 2.4 GHz only with no WPA3: document it |
| Scraping | P (`ui/xml.c` parses gamelist.xml) | P1 | Recommend **Skraper on a PC** and support its ES layout (`roms/<sys>/gamelist.xml` + `media/...` relative paths). On-device ScreenScraper: P2 (API credentials, rate limits, slow on the A20, needs a valid clock) |
| File transfer | M | P2 | The card reader is the main path (exFAT). Optional SFTP (dropbear, ~200 KB) started only when "Remote access" is on and WiFi is up, **with a password the user must set** (not a default). Samba: D (heavy) |
| Netplay | D | - | - |
| OTA updates | D (P2) | P2 | See 9 |

---

## 9. Updates and versioning

| Item | Status | Pri | How |
|---|---|---|---|
| Update without losing `/data` | **M** (build.md: flashing overwrites the whole card, data included) | **P0 (decision) / P1 (tool)** | Decide the **partition layout before the first release**, because it cannot change later without a wipe. Recommendation: **A/B roots** (p1 root A 512 MB, p2 root B 512 MB, p3 exFAT). The updater writes the inactive slot from `/data/update/rsos-<ver>.img.xz` (+ `.sha256`), verifies it, then flips the MBR bootable flag. U-Boot `bootcmd` boots the partition flagged bootable, falling back to the other. The alternative (a single root plus a tmpfs pivot_root updater, like OpenWrt sysupgrade) saves 512 MB but can brick on power loss |
| Update UX | M | P1 | Copy one file to `/data/update/`. The UI offers "Update to X.Y". It requires ≥ 30% battery or the charger. Show progress |
| Bootloader updates | D | P2 | U-Boot at 8 KiB is not updated by the updater (risky). Needs a full reflash if it ever has to change |
| Version identification | M | P0 | `/etc/os-release` `VERSION_ID` + git hash + build date (post-build.sh). "About" screen: firmware version, kernel, core commits (from the ini), serial/MAC |
| Settings schema version | M | P1 | `version=` in `settings.ini`. Migrate forward, keep unknown keys |

---

## 10. Migration from RetrOrangePi

| Item | Status | Pri | How |
|---|---|---|---|
| Migration guide | M | P1 | `docs/migration.md` (user-facing). Warning: **the old card is ext4, which Windows cannot read**. Copy through the old firmware's Samba shares (network on) or with a Linux PC. The new image wipes the card: copy to the PC first |
| ROM folder names | P (cores.md table) | P1 | Accept RetroPie aliases in the scanner: `genesis`→`megadrive`, `sg-1000`→`sg1000`, `fba`→`fbneo`, `mame-libretro`/`mame2003`→`arcade`, `mastersystem`, `gamegear`, `segacd`, `sega32x`, `fds`... List the unsupported ones (`n64`, `pcengine`, `atari2600`, `dreamcast`, `psp`, `amiga`, `ports`, `kodi`) until cores exist (see 16) |
| Save files | M | P1 | RetroPie kept `.srm`/`.state*` **next to the ROMs** (production `savefile_directory = "default"`). On scan, if `/data/saves/<sys>/<game>.srm` is missing and `roms/<sys>/<game>.srm` exists, move it (or look in both places). `.srm` is raw SRAM and compatible for fceumm, snes9x, gambatte, gpsp, pcsx (memcard). TODO: check MD `.srm` byte order if the old core was Genesis Plus GX rather than PicoDrive, and mame2003 → mame2003-plus nvram |
| Save states | M | P1 | States are core-version specific and usually **won't load**. Say so. Only SRAM is guaranteed |
| gamelist.xml | P (the xml parser tolerates the ES quirks) | P1 | Read `roms/<sys>/gamelist.xml`. Rewrite `~/.emulationstation/downloaded_images/<sys>/x.png` to `roms/<sys>/media/`. Otherwise tell users to re-scrape with Skraper |
| BIOS | C (names match, cores.md) | P1 | `/home/pi/RetroPie/BIOS/*` → `/data/bios/`. Arcade BIOS zips stay in the ROM folder |
| Controller configs, RetroArch configs, themes | D | - | Not migrated |

---

## 11. Boot

| Item | Status | Pri | How |
|---|---|---|---|
| Boot-time budget per stage, measured | M (target 3-6 s in CONVENTIONS, "TODO: measured" in build.md) | **P0** | Measure with `grabserial -t` on the UART from power-on, printk times, and a frontend log of CLOCK_BOOTTIME at the first flip and at "menu interactive". Proposed budget: BROM+SPL 0.4 s · U-Boot + zImage/DTB load 0.4 s · kernel → init 1.2 s · rcS 0.3 s · frontend → first frame 0.3 s · → menu interactive 0.8 s = **3.4 s** |
| Quiet boot | P (TODO in boot.cmd) | P0 | `quiet loglevel=3`: ~30 KB of log at 115200 baud costs ~2.6 s. Keep the UART console for errors. U-Boot: `CONFIG_SILENT_CONSOLE` or fewer prints (P2) |
| Immediate "it's booting" feedback | M | P1 | Blue LED3 (PH2) `default-state = "on"` in the DTS (on at kernel start); the frontend turns it off. Power LED2 is hardwired |
| Splash / first frame | P (display lib ready; no fbdev on purpose) | P0 | The frontend's **first action** is a modeset with a raw pre-rendered 640×480 RGB565 splash (mmap, no decode), before the theme load or ROM scan. No kernel logo (it would need fbdev) |
| Recovery if the frontend crashes | P (`::respawn`, UART root shell) | P0 | Crash-loop guard: count starts in `/run`. After 3 crashes in 30 s, start `rsos-frontend --safe` (default settings, built-in theme, no scan) with "Reset settings / Check SD / Show log". The UART shell stays |
| Safe-mode key combo at boot | M | P1 | The frontend reads the key state with `EVIOCGKEY` at start. Holding **Select+Start** (or B) at power-on → safe mode |
| Frontend startup failures visible | M | P1 | If DRM init fails, log to the UART and blink LED3 |
| Extra boot options for the Pro variant | C (`/boot/rsos.env`) | P2 | Editing needs a remount rw. Later: an equivalent in the UI |
| Remove bring-up packages from the release | M | P2 | mesa/lima/kmscube (no core uses the GPU, cores.md), htop, evtest, speaker-test. Consider `DRM_LIMA=m`. Less rootfs size and probe time |

---

## 12. Manufacturing and support

| Item | Status | Pri | How |
|---|---|---|---|
| Hardware self-test screen (replaces `ropi-rs2-final.sh` + `e.py`) | M | P1 | One screen per test, each with pass/fail: all 18 buttons incl. C/Z, brightness keys, PEK; stick range; LCD colour bars and dead pixels; backlight sweep; speaker L/R and headphone tones; HDMI detect + picture + audio; battery V/I/charging; temperature; SD read speed; USB ports (plug a pad or stick); WiFi scan (SSID count); BT scan; Ethernet link; eMMC/SATA presence (Pro); EEPROM read; RTC tick; LED. Reached from a hidden combo or `--selftest` |
| Logs users can send | M | P1 | A log ring in RAM (`/run/rsos/log`). On a crash, and from "Export logs": write `/data/rsos/logs/report-<date>.txt` (versions, dmesg, frontend log, settings, core list, BIOS status, battery, temperature). Never log continuously to the SD |
| Factory reset | M | P1 | "Reset settings" (settings, options, remaps, input maps; keep ROMs, saves, states) and "Erase data partition" (double confirm, mkfs) |
| About / serial | M | P1 | Version, kernel, MAC (derived from the SID by U-Boot), EEPROM contents if 8BCraft put a serial there (TODO(hw)) |

---

## 13. Legal and licensing

Checked the licence files in the pinned sources (`~/rsos/cores-test/*`):

| Component | Licence | Commercial distribution with a sold device |
|---|---|---|
| fceumm | GPL-2.0+ | OK (source offer) |
| gambatte | **GPL-2.0-only** | OK (source offer). Constrains the frontend licence, see below |
| gpsp (+ open GBA BIOS, GPL-2) | GPL-2.0+ | OK |
| pcsx_rearmed | GPL-2.0+ (deps: libchdr BSD, lightrec LGPL, miniz MIT) | OK |
| **snes9x2005** | Snes9x licence (+ MIT glue): "non-commercial purposes... Commercial use includes... charging money for Snes9x or software derived from Snes9x" | **Forbidden without permission** |
| **snes9x2010** | Snes9x licence, same text | **Forbidden** |
| **picodrive** | MAME-style: "Redistributions may not be sold, nor may they be used in a commercial product or activity". Bundles **DrZ80: "free for non-commercial use"**. Cyclone: GPL-2 or MAME, your choice | **Forbidden** |
| **mame2003_plus** | MAME 0.78 licence: "Selling either is not allowed". Also no distribution on the same medium as illegal ROMs | **Forbidden** |
| **fbneo** | FBNeo licence: "You may not sell, lease, rent or otherwise seek to gain monetary profit"; also "**You may not ask for donations** to support your work on any project that uses the FB Neo source code" | **Forbidden**, and **no donation links** for RetroStoneOS while FBNeo ships |

**5 of 9 cores are non-commercial**: all of SNES, all Sega systems, all arcade. There are no GPL cores for SNES or arcade
that are fast enough on a 1 GHz A7 (bsnes/Mesen-S are too slow; current MAME is too heavy; Genesis Plus GX is also
non-commercial).

| Item | Status | Pri | How |
|---|---|---|---|
| **Policy for non-commercial cores** | C (owner decision, 2026-09-29) | **P0 (owner decision)** | **The OS is not sold with the hardware: it is a free download only.** So the non-commercial cores (snes9x2005/2010, PicoDrive, MAME 2003-Plus, FBNeo) and the CC BY-NC-SA theme art stay built into every image, with no separate core pack. The release notes, the README and LICENSE say it: free, non-commercial distribution; an image must not be sold or preloaded on hardware that is sold. (Had the device or the card been sold with the OS, the alternative was a GPL-only base image plus a free "core pack", or written permission from the copyright holders.) This is not legal advice |
| GPL/LGPL source offer | P (`make legal-info` in CI, one archive per board attached to each release: docs/ci.md; the NXEngine source without the Cave Story data. Missing: `SOURCE-OFFER.txt` in the image) | P0 | Kernel (with patches and DTS), U-Boot, BusyBox, glibc (LGPL), util-linux, exfatprogs, bluez, alsa-utils, GPL cores. Run Buildroot `make legal-info` in the release script and publish `legal-info/` (sources, patches, licence texts, manifest) next to each image (GPL-2 §3(a)). Put `/usr/share/rsos/SOURCE-OFFER.txt` in the image |
| On-device notices | M (the licence texts are only in the legal-info archives of the release) | P0 | `/usr/share/licenses/<pkg>/` (legal-info licence files) + a "Licences" screen. Include the DejaVu, nanosvg (zlib), stb (MIT/PD), SDL GameControllerDB (zlib) and linux-firmware Cypress/Broadcom notices, and the credits of the cores (e.g. the V30MZ core of the WonderSwan emulator). A first step: a post-build step (board/common/post-build.sh) writing `/usr/share/rsos/licenses.txt` from the packages' licences |
| Frontend's own licence | M | P0 | It is linked in-process with GPL-2.0-only (gambatte) and GPL-3 (future cores) code, so pick **MIT** (or GPL-2.0-or-later). Not GPL-3-only |
| Firmware blobs | P | P1 | linux-firmware brcm/cypress: redistributable, include LICENCE. **Armbian `BCM20710A1.hcd`: provenance and licence unknown**, check or replace |
| Themes and assets | M | P1 | Many EmulationStation themes and system logos are CC BY-NC(-SA) or trademarked (Nintendo/Sega/Sony logos). Use only assets with a licence compatible with the distribution model. Record them in `frontend/third_party/` |
| ROMs and BIOS | C (never shipped) | P0 | Never ship or preload ROMs or BIOS (the MAME/FBNeo clauses also forbid shipping with illegal ROMs). Only homebrew with explicit permission |
| Kernel patch authorship | C (kernel-patches.md: no SoB, credit Stefan Mavrodiev) | - | - |
| Names | M | P2 | Do not use "RetroPie" or "EmulationStation" branding. "RetroStone" is 8BCraft's |

---

## 14. Frontend parity with what EmulationStation and RetroArch gave for free

| Item | Status | Pri | How |
|---|---|---|---|
| System carousel, game lists, metadata view, theme | P (`theme/`, `gfx/`, ui-design.md pending) | P0 | - |
| Favourites, recently played, play time | M | P1 | `/data/rsos/history.ini` (last played, play count, seconds played) |
| Sort, jump to letter, search | M | P1 | Natural sort (C: `str_casecmp_natural`) |
| Hide game, per-game core, per-game options | M | P1 | See 1.2 |
| In-game menu (replaces RGUI) | P (input-design §4) | P0 | Resume, states with thumbnails, disc, options, display, controls, reset, exit |
| Quit menu: power off, reboot, back to the menu | M | P0 | - |
| Settings in one place (input-design, CONVENTIONS) | P | P0 | Display, audio, power, network, input, time, about, self-test |
| Kodi, ports, media player | D | - | - |
| Localisation (FR/EN at least) | M | P2 | String table |

---

## 15. Top 15 risks / most likely forgotten items (ranked)

1. **Licences:** 5 of 9 cores (both snes9x, picodrive with DrZ80, mame2003-plus, fbneo) forbid commercial use. FBNeo also forbids asking for donations. If 8BCraft sells units or cards with the image, this blocks shipping SNES, Sega and arcade. Needs an owner decision before any release (13).
2. **No update path that keeps `/data`:** reflashing wipes saves. The partition layout (A/B or not) must be fixed **before v1.0**, because changing it later means wiping the card (9).
3. **LCD at 78.6 Hz:** a vsync-locked host runs games 31% fast; an unlocked one judders and crackles. Retime to 60 Hz (hardware test) and implement both pacing modes with DRC (1.4).
4. **Save loss on power cut or battery death:** SRAM written only at exit (RetroArch default), `file_write_atomic()` without fsync, 30 s writeback, no critical-battery autosave, exFAT without a journal (1.2, 5, 6).
5. **Cores in the frontend process:** a crash kills the menu, and cores that keep global state break on the second game. Use a child process per game (1.2).
6. **No audio buffer status callback:** the cores' auto frameskip will not work, and PS1/arcade will stutter and crackle on the A20 (1.1, 1.3).
7. **Kernel lacks the pad drivers the input design promises** (sony/playstation/nintendo/microsoft HID, FF, USB storage), so many pads will not be recognised at all (2).
8. **HDMI docked mode unverified:** fe0 on CRTC0/TCON0-ch1, experimental HDMI audio, and no volume control on HDMI, since the wheel is analog-only (3, 4).
9. **Battery:** no % or warning, no critical auto-shutdown, AXP hard cut at 2.9 V, no `monitored-battery`, and the unit boots when the charger is plugged in (5).
10. **No suspend on A20 mainline:** users expect sleep/resume. It must be faked (screen off + pause + timed auto-save power-off) and battery drain measured (5).
11. **Clock:** no RTC backup cell, so the time is lost when the battery dies. A 2010 clock breaks TLS (scraping, updates) and "last played". Needs a saved-time fallback and NTP (7).
12. **Bluetooth pairings live on tmpfs:** BT pads must be re-paired at every boot (2).
13. **PC interaction:** Windows may offer to format the ext4 partition, macOS `._` files show up as games, users reformat p2 as FAT32, and the first boot is a long black screen (6).
14. **Migration friction:** the old card is ext4 (unreadable on Windows), RetroPie folder aliases, saves next to ROMs, incompatible states, gamelist image paths (10).
15. **Boot time and recovery unmeasured:** 115200-baud console output (~2.6 s), no per-stage budget, a respawn loop without a safe mode (11).

---

## 16. Proposed next agents / tasks

Each package is self-contained. The owned paths are exclusive; anything outside them goes in the agent's report.

| # | Work package | Scope | Owned paths |
|---|---|---|---|
| 1 | **Libretro host core** | dlopen, all callbacks of 1.1, core options V2/V1/variables with global/core/game layers, BIOS check, SRAM/.rtc periodic flush, states + PNG thumbnails, zip via miniz, m3u/cue list filtering, disk control, rotation, perf interface; `--run <core> <rom>` child mode | `frontend/src/core/`, `frontend/third_party/miniz/`, `docs/libretro-host.md` |
| 2 | **Audio and A/V sync** | ALSA 48 kHz, NEON resampler, DRC, audio-buffer-status feed, underrun recovery, keep-open codec, fades, HDMI/codec switch through `display_audio_cb`, HDMI software volume, vsync-locked vs audio-clocked pacing, FF | `frontend/src/audio/`, `frontend/src/core/pacing.*`, `docs/av-sync.md` |
| 3 | **LCD 60 Hz and refresh experiments (hardware)** | Try 25.2 MHz timings; check flicker, panel limits and the range of refresh rates the panel accepts (50-60 Hz); backlight/first-frame flash; LED3 early on | `buildroot-external/board/retrostone2/dts/sun7i-a20-retrostone2.dts` (panel, leds nodes), a section of `docs/display-design.md` |
| 4 | **Game runner process and recovery** | Parent/child split, DRM master handover, crash and hang handling, force-quit combo, crash-loop counter, `--safe` mode, boot key combo, frontend `main.c` | `frontend/src/main.c`, `frontend/src/runner/`, `docs/runner.md` (post-build inittab change reported to the lead) |
| 5 | **Power management** | Battery reading and smoothing, warnings, critical autosave + poweroff, fake sleep, idle dim, governor per core, thermal readout, charger-boot detection (AXP REG00 via i2c or sysfs), PEK REG36 check, `monitored-battery` proposal, power measurements plan | `frontend/src/power/`, `docs/power-design.md` |
| 6 | **Data safety and first boot** | fsync in `file_write_atomic`, vm.dirty tuning, exFAT dirty-flag + conditional fsck, vfat fallback, folder list generated from the core inis, first-boot timing and message, read-only-root audit, `state.img` loop mount helper | `rootfs-overlay/usr/libexec/rsos/data-partition`, `rootfs-overlay/etc/init.d/rcS`, `rcK`, `frontend/src/ui/util.c` (file_write_atomic only, coordinate with the UI owner), `docs/storage.md` |
| 7 | **Partition layout and updater** | Decide A/B vs pivot updater (recommend A/B); genimage, U-Boot bootcmd slot selection, `rsos-update` script, `/etc/os-release` versioning, release script with `make legal-info` | `genimage.cfg`, `boot.cmd`, `uboot.fragment`, `post-build.sh` (version), `rootfs-overlay/usr/libexec/rsos/update`, `docs/update.md` |
| 8 | **Kernel input/USB/BT completeness** | HID drivers, FF, USB storage, uinput if needed, modalias autoload helper, BlueZ persistence through `state.img`, BT pairing helper script | `linux.fragment`, `rootfs-overlay/usr/bin/rsos-net` (bt part), `docs/bluetooth.md` |
| 9 | **Time** | RTC LOSC check, saved-time fallback, TZ, manual set, NTP after DHCP, `hwclock -w` | `rootfs-overlay/usr/libexec/rsos/clock`, `rsos-net` (ntp hook, coordinate with WP8), `docs/time.md` |
| 10 | **Licensing and compliance** | Core licence policy memo for the owner (base GPL image + optional core pack), `make legal-info` integration, on-device licence notices, frontend licence file, Armbian BT firmware provenance, theme/asset audit | `docs/licensing.md`, `LICENSE` (frontend), `frontend/third_party/*/LICENSE*`, `buildroot-external/board/retrostone2/post-image.sh` (legal-info step) |
| 11 | **Migration** | User guide, RetroPie folder alias table, save import rules, gamelist image path rewrite; test on a real RetrOrangePi card image | `docs/migration.md`, `frontend/src/ui/systems.*` (alias table, coordinate with the UI owner) |
| 12 | **Self-test, logs, factory reset** | Self-test screens (list in 12), log ring and report export, factory reset, About screen | `frontend/src/tools/selftest.*`, `frontend/src/log.*`, `docs/support.md` |
| 13 | **Boot-time measurement and trimming** | grabserial measurement per stage on the device, quiet boot, raw splash first frame, U-Boot trimming, remove bring-up packages, `DRM_LIMA` decision | `boot.cmd` (bootargs), `uboot.fragment`, `retrostone2_defconfig` (package removal), `docs/boot-time.md` |
| 14 | **Extra GPL cores** (they widen what can be shipped commercially) | beetle-pce-fast (PC Engine), stella2014 (2600), smsplus-gx or Gearsystem (SMS/GG/SG-1000, GPL), handy (Lynx, zlib), beetle-ngp, beetle-wswan: package, build, time on the A20 | `buildroot-external/package/libretro-<new>/`, `docs/cores.md` (new rows) |
| 15 | **Network UX** | WiFi scan/connect screen, on-screen keyboard, optional SFTP with a user-set password, Skraper media layout support | `frontend/src/ui/net.*`, `frontend/src/ui/osk.*`, `docs/network.md` |
