# Feature parity and gap analysis: RetroStoneOS vs RetroPie (and handheld firmwares)

Review of 2026-09-27. Compares what a RetroStone2 owner got from RetrOrangePi/RetroPie (EmulationStation + RetroArch +
RetroPie-Setup), and what the handheld firmwares (Onion, muOS, KNULLI, ROCKNIX, Batocera) offer, with what
RetroStoneOS has today. Read-only review: nothing in the code was changed.

**How the "today" column was established**: the design docs (`requirements.md`, `cores.md`, `ui-design.md`,
`host-design.md`, `input-design.md`, `rom-transfer.md`, `power.md`, `build.md`, `homebrew.md`), the defconfig, and a
grep of `frontend/src/` where a doc was silent or older than the code (for example: there is no screenshot hotkey,
no `retro_cheat_set` call, no play-time field, no letter jump in `view_gamelist.c`). `requirements.md` is a 2026-09-25
snapshot: many of its "M" rows are implemented now (host, states, resume, pacing, DRC, power, time zone, USB import);
this review uses the current state.

**Legend**: ✅ have · 🟡 partial · ❌ missing · ➖ not relevant or dropped on purpose.
Value (for a handheld owner): **H** high, **M** medium, **L** low. Effort in our architecture (one C frontend, a
libretro host without RetroArch, a game child process per launch): **S** ≤ 2 days, **M** ~1 week, **L** several weeks.

RetroPie documentation is cited as `RP:<page>` = `https://retropie.org.uk/docs/<page>/` (page names from the
RetroPie-Docs repository, e.g. RP:Runcommand = https://retropie.org.uk/docs/Runcommand/).

---

## 0. Summary

Where RetroStoneOS is already **ahead** of what RetroPie gave on this device: crash/hang isolation (game child
process), durable SRAM (periodic flush + fsync, `.bak` states), power-off-always-saves with a boot "Resume <game>?"
offer, auto state + resume prompt, charge mode, critical-battery save, DRC audio and two pacing modes, live LCD ↔ HDMI
switching with automatic player 1, per-game core options that save themselves, a per-game core choice without
runcommand, USB-stick import/export/backup, a web upload page with QR code, the N64 benchmark, and a ~0.3 s menu at boot.

Main **gaps** a RetroPie/Onion user will notice: no jump-to-letter/search, no fast-forward or screenshot hotkey, no
play time, no game switcher, no in-UI remap/turbo editor, no Bluetooth pairing screen, no cheats, no updater UI, no
shaders/LCD grid, and no home computers (DOS, C64, Amiga, Spectrum, CPC), PICO-8, ports (Doom, Cave Story) or
ScummVM. The last group is the largest perceived loss versus RetroPie's system list.

---

## 1. Part 1: feature parity

### 1.1 EmulationStation (menu, game lists)

| Feature | RetroPie / others | RetroStoneOS today | St. |
|---|---|---|---|
| System carousel, basic/detailed views | ES | Own ES-compatible renderer; `system`, `basic`, `detailed` views; 8 ES themes render correctly at 640x480 (ui-design §5) | ✅ |
| Game lists from folders | ES | Scan with RetroPie folder aliases, `.cue`/`.m3u` members hidden, junk files skipped, scan cache, background loading | ✅ |
| gamelist.xml from Skraper | RP:Scraper recommends Skraper on a PC | Read from the ROM folder or `/data/rsos/gamelists/<system>/`; foreign absolute paths remapped; media sub-folders (`media/`, `images/`...) | ✅ |
| On-device scraper (ScreenScraper/TheGamesDB) | ES built-in scraper, Skyscraper in RetroPie-Setup | None (deliberate: needs WiFi, a valid clock, API keys; slow on the A20) | ➖ |
| Metadata editing | ES "Edit metadata" | None (game options: favorite, core, launch, file name) | ❌ |
| Favorites | ES | X toggles; "Favorites" collection; "Favorites first" option | ✅ |
| Last played | ES `auto-lastplayed` | "Last played" collection (50 most recent), last played + times played in the detailed view | ✅ |
| All games collection | ES `auto-allgames` | Not built (art exists in the theme) | ❌ |
| Custom collections | ES custom collections | None | ❌ |
| Kid / Kiosk UI modes | RP:Child-friendly-EmulationStation | `kidgame` parsed from gamelist.xml, never used | ❌ |
| Jump to letter / search | ES "Jump to..." letter; ES-DE search | Page up/down only (L1/R1, L2/R2); Y is unused in lists | ❌ |
| Sort options | ES: name, rating, play count, last played, players, developer, genre... | Natural name sort, favorites first | 🟡 |
| Video previews | ES video view | Rendered as a still (`showSnapshotNoVideo`); no playback (deliberate on the A20) | ➖ |
| Screensaver | ES dim/black/slideshow/random video | Idle dim at 2 min, screen off at 5 min (better for battery) | ➖ |
| Background music | ES-DE / RetroPie add-on scripts | None; no UI sounds either (deliberate) | ❌ |
| Per-system theming | ES themes | Full per-system lookup with alias folders; 2 own themes + gbz35/gbz35-dark bundled | ✅ |
| Theme download | RetroPie-Setup theme installer | Copy to `/data/themes/` (USB import has a Themes toggle; web share) | 🟡 |
| Hidden games | ES metadata `hidden` | `hidden=true` in gamelist.xml is honoured; no UI to hide or show them | 🟡 |
| Random game | ES "Random" in quick select (newer builds) | None | ❌ |
| Delete a game from the device | ES metadata editor "Delete" | None (PC, USB or web only) | ❌ |
| Grid view | ES grid | Falls back to detailed (ui-design §15) | ➖ |
| Localisation | ES has translations in forks (Batocera, ES-DE) | English only (requirements §14: FR/EN is P2) | ❌ |

### 1.2 RetroArch in-game features

| Feature | RetroArch / RetroPie | RetroStoneOS today | St. |
|---|---|---|---|
| Save states, slots, thumbnails | Slots, thumbnails in menu | Slots 0-9 + auto slot, PNG thumbnail and date in the Select+X menu, `.bak` of the previous state per slot, durable writes | ✅ |
| Undo save/load state | RetroArch menu | `.bak` kept, no menu item to restore it | 🟡 |
| Auto save / auto load state | `savestate_auto_save/load` | Auto state on exit (setting) and always on power-off; launch prompt Ask/Always/Never; boot resume offer | ✅ |
| Rewind | Off by default in RetroPie (RP:RetroArch-Configuration) | Dropped (requirements §1.2) | ➖ |
| Fast-forward | Hotkey (unmapped by default in RetroPie) | Select+X menu: off/x2/x3/x4; **no hotkey** | 🟡 |
| Slow motion | Hotkey | None | ❌ |
| Cheats | RP:Cheats (`.cht` files, cheat search) | None (`retro_cheat_set` never called) | ❌ |
| Netplay | RP:Netplay | None (deliberate) | ➖ |
| Shaders / smoothing | RP:Shaders-and-Smoothing (GLSL on the GPU) | Hardware plane scaling only: sharp ×2/×4 backend or soft DEFE filter depending on geometry; no scanlines, no LCD grid | 🟡 |
| Overlays / bezels | RetroArch overlays | None; the single alpha plane is used by the battery/FPS strip | ❌ |
| Aspect ratio, integer scaling | RetroArch video settings | Aspect / Integer / Stretch (setting + in-game menu); in-game change is not saved per game | 🟡 |
| Core options (per core / per game) | RP:RetroArch-Core-Options | v0/v1/v2 with categories and info text, per-game and per-core layers, auto-saved, source shown | ✅ |
| Per-core / per-game remaps | RetroArch remap files + menu | Remap files (`/data/rsos/remaps/<system>[/<game>].ini`, shipped per-system defaults incl. C/Z); **no editor in the UI** | 🟡 |
| Turbo buttons | RetroArch turbo | None | ❌ |
| Screenshots | Hotkey, `Take-and-Scrape-Your-Own-Screenshots` | None (`--menu-shot` is a test tool); PNG writer already in the host (`host_png.c`) | ❌ |
| RetroAchievements | RP:RetroAchievements | None (`SET_SUPPORT_ACHIEVEMENTS`/`SET_MEMORY_MAPS` accepted and ignored) | ❌ |
| Frame stats / FPS | "Show framerate" | FPS/speed/skip/CPU overlay on the overlay plane, perf log, N64 benchmark | ✅ |
| Run-ahead | RetroArch run-ahead | None | ❌ |
| Frame delay | RetroArch `video_frame_delay` | None (input polled right before `retro_run`, double buffering: 1 frame) | ❌ |
| Audio DSP | RetroArch DSP filters | None | ➖ |
| Volume | RetroArch volume / hotkeys | Analog wheel on the LCD/jack; **no software volume on HDMI** (requirements §4, P1) | 🟡 |
| Controller autoconfig | RetroArch autoconfig + ES input config | SDL GameControllerDB, Linux gamepad spec, user wizard; hotplug; kernel HID drivers (sony, playstation, nintendo, microsoft, steam, xpad) | ✅ |
| Player assignment | RetroArch ports | Launching pad = P1, stable hot-plug, docked policy | ✅ |
| Multitap / port device choice | RetroArch "Device type" per port | Automatic joypad/analog only; no SNES multitap or NES Four Score choice | ❌ |
| Analog → d-pad | RetroArch "Analog to Digital Type" | D-pad → analog exists (N64/PS1); stick → d-pad for digital games is missing | 🟡 |
| Rumble | RetroArch | Kernel FF on; host forwards to `hin_rumble()`, which is a no-op | 🟡 |
| Disk control (multi-disc) | RetroArch disk menu | Disc `< i / n >` in Select+X for `.m3u` (pcsx, picodrive) | ✅ |
| MSU-1 | snes9x (current), bsnes | Cores that support it are shipped (snes9x2010, supafaust experimental); untested; a zipped MSU pack cannot work (only one member is extracted) | 🟡 |
| Super Game Boy border | mGBA, bsnes | None: gambatte has no SGB mode, mGBA not packaged | ❌ |
| GB colorisation palettes | gambatte core options | gambatte options (internal palettes, GBC colorisation) through the core options menu | ✅ |
| Link cable | gambatte netlink (network), TGB Dual (two GBs in one core) | gambatte built with `HAVE_NETWORK=0`; TGB Dual not packaged | ➖ |
| RTC (Pokémon G/S/C, GBA carts) | `.rtc` files | `.rtc` saved with SRAM; clock fallback from `/data/rsos/clock` when the battery dies | ✅ |
| Vertical arcade games | RetroArch rotation | Cores rotate in software (`SET_ROTATION` = false); no "TATE" mode for holding the unit sideways | 🟡 |
| Zipped content | RetroArch | `.zip` (miniz, extract to tmpfs; arcade as is) | ✅ |
| 7z content | RetroArch | Refused with a message | ❌ |
| CHD | RetroArch cores | pcsx_rearmed, pce_fast | ✅ |
| Crash isolation | none (a core crash takes RetroArch down; runcommand returns to ES) | Child process, crash/hang messages, SRAM kept | ✅ |
| Accidental exit protection | RetroArch "confirm quit" | Auto state on exit (default on) | ✅ |

### 1.3 RetroPie system features

| Feature | RetroPie | RetroStoneOS today | St. |
|---|---|---|---|
| Configuration menu | RetroPie menu in ES + RetroPie-Setup | One Settings menu: Display, Controls, Network, Theme, Game lists, Games, Storage, System information, Date & time, Power | ✅ |
| WiFi setup | RP:Wifi (dialog in RetroPie-Setup) | Toggle, SSID and password through the on-screen keyboard; **no scan list**; `wpa_supplicant.conf` on the card still works | 🟡 |
| Bluetooth pairing UI | RP:Bluetooth-Controller (RetroPie-Setup dialog) | Toggle only; pairings persist (`bluetooth.img`); no scan/pair/trust/forget screen | ❌ |
| Time zone, clock | raspi-config | 61 zones, manual date/time; **no NTP** after WiFi comes up | 🟡 |
| Language / locale | raspi-config, ES forks | English only | ❌ |
| SSH | RP:SSH | None (dropbear not in the defconfig); UART root shell for debugging | ➖ |
| Samba shares | RetroPie default shares | ksmbd + ksmbd-tools in the image, UI toggle shows "Windows file share (later)" | 🟡 |
| USB ROM service | RP:Running-ROMs-from-a-USB-drive, usbromservice | USB Import / Export / Back up saves with plan, duplicates prompt, progress, eject (rom-transfer §2) | ✅ |
| Web upload | none in RetroPie (Batocera has a web manager) | Web page upload with QR code, PIN, `retrostone.local` | ✅ |
| Updates | RP:Updating-RetroPie | A/B slots and the boot counter are done (build.md); **the updater and its UI are not written** | 🟡 |
| Runcommand: core per game/system | RP:Runcommand | Game options: "Core for this game" / "for all games of the system"; experimental cores labelled | ✅ |
| Runcommand: video mode per game | RP:Runcommand | HDMI mode is global (Auto/720p/1080p); LCD 60/78 Hz global | 🟡 |
| Splash screen | RP:Splashscreen (custom images/videos) | Built-in raw splash (fast); no user splash | 🟡 |
| File manager | RetroPie-Setup File Manager (mc) | None on the device | ❌ |
| On-device scraper | RP:Scraper | See 1.1 | ➖ |
| Kodi / media player | RP:KODI | None | ➖ |
| Ports (Doom, Quake, Cave Story...) | RP:Ports, RP:Doom, RP:Quake, RP:Cave-Story | None (homebrew console ROMs are bundled instead) | ❌ |
| PICO-8, ScummVM, DOSBox, OpenBOR | RP:ScummVM, RP:PC, RP:OpenBOR | None (see Part 2) | ❌ |
| BIOS check | none (silent failure) | Checked before launch with a clear message; wrong md5 = warning; no BIOS status screen | 🟡 |
| Overclocking | RP:Overclocking | CPU policy 720-960 MHz; no OC (deliberate) | ➖ |
| Factory reset, log export, licences screen | not in RetroPie | Logs copied to `rsos/logs/` by the boot logger; no reset or licences screen (requirements §12-13) | 🟡 |

### 1.4 Handheld-specific (Onion, muOS, KNULLI, ROCKNIX)

| Feature | Handheld firmwares | RetroStoneOS today | St. |
|---|---|---|---|
| Sleep / suspend | Onion/muOS: suspend or fake sleep | Removed on purpose: the power key saves and powers off, the next boot offers "Resume <game>?" (A20 mainline has no suspend-to-RAM) | ➖ |
| Quick-save + quick-resume on power | Onion "Save and power off" | ✅ always on | ✅ |
| Battery %, warnings, critical save | all | Menu + in-game pill, 15 %/7 % warnings, critical auto-save and power-off | ✅ |
| Charging screen | muOS/KNULLI | Charge mode on charger boot | ✅ |
| Brightness overlay | all | Brightness keys + overlay; idle dim | ✅ |
| Volume overlay | all | Analog wheel (no software volume) | ➖ |
| Per-game CPU speed | muOS/KNULLI governor per game | Per-core governor/max clock from the core `.ini`; no per-game choice | 🟡 |
| Play time / activity log | Onion Activity Tracker, muOS | Play count and last played only; no time played | ❌ |
| Recents | all | "Last played" collection | ✅ |
| Game switcher | Onion GameSwitcher (recent games with state screenshots, one button) | None (auto states + thumbnails exist, so the parts are there) | ❌ |
| State thumbnail as game art | Onion, muOS | Not used in lists | ❌ |
| Thermal protection | some | Toast at 75 °C, save + power off at 95 °C | ✅ |
| HDMI docked mode | ROCKNIX/KNULLI on some devices | Live switch, external pad becomes P1 | ✅ |
| Button test | muOS/KNULLI tester | Button test (buttons, C/Z, stick) | ✅ |

### 1.5 Missing and partial features, ranked

Ranked by value first, then by lowest effort. "Where" names the module that would change.

| # | Feature | Value | Effort | Where / how in our architecture |
|---|---|---|---|---|
| 1 | Jump to letter + search | H | S | `ui/view_gamelist.c`: Y opens a letter strip (A-Z, 0-9) or L2/R2 jump to the next letter; search reuses the OSK and filters the current list |
| 2 | Fast-forward hotkey | H | S | `input.h` new `IN_HK_FAST_FORWARD` (Select+R2 is free); the host already runs FF (menu). Toggle, optional "hold" mode |
| 3 | Updater (file dropped in `/data/update/`) | H | M | The contract exists (build.md "A/B slots"): verify sha256, `xz -dc` to the inactive slot, `fw_setenv`, reboot; UI: detect, battery ≥ 30 %, progress. Without it every release means a reflash (the ROM partition survives only if the user does not rewrite the whole card) |
| 4 | Game switcher / quick resume list | H | M | New in-game menu item "Switch game" (auto state + a new exit code) and a UI screen of the last N games showing their `.state.auto.png`; A = launch with `resume` |
| 5 | Play time + activity | M/H | S | Launch time → return time in `ui_launch_game()` minus menu time, or a `playtime <s>` status line from the child; a column in `gamedb.tsv`; show in the detailed view and a "Most played" collection |
| 6 | Screenshot hotkey (+ use as game image) | M | S | `IN_HK_SCREENSHOT` (Select+L2), `host_png.c` on the core frame, `/data/screenshots/<system>/<game>-<date>.png`; a game option "Use as picture" stored in `gamedb.tsv` (never write into the user's gamelist.xml) |
| 7 | In-game remap editor + turbo | M/H | M | Select+X > Controls: per button → RetroPad id, saved to the existing remap files (game/system); turbo flag per button in the input layer (period in frames) |
| 8 | Bluetooth pairing screen | H if BT pads are sold with it, else M | M | Async helper around `bluetoothctl` (scan, pair, trust, connect, remove) like `rsos-net`; list screen; auto-connect on BT on |
| 9 | Cheats (`.cht`) | M | M | Parse RetroArch `.cht` (`cheatN_desc/code/enable`), `/data/cheats/<system>/<game>.cht`, a Select+X page, `retro_cheat_reset/set` after load and on change. fceumm, snes9x, gambatte, gpsp, pcsx_rearmed, picodrive implement `retro_cheat_set`. Cheat search: no |
| 10 | Per-game scaling and CPU profile, hide/delete game | M | S | Scaling: a host key (`rsos-scale`) in the per-game options file, as `rsos-glthread` already is. CPU: read the per-game file in the UI before `power_set_game_cpu()`. Hide/delete: game options + confirm |
| 11 | HDMI software volume | M | S | Gain in the audio path (`audio.c`), brightness keys act as volume when docked, persisted |
| 12 | BIOS status screen | M | S | `coreinfo.c` already has the table: per system found / wrong md5 / missing / optional |
| 13 | WiFi scan list, NTP | M | S | `wpa_cli scan_results` in the SSID picker; `ntpd -q` + `hwclock -w` after DHCP in `rsos-net` |
| 14 | LCD grid / scanline mask | M | M | A full-screen ARGB mask on the overlay plane (merged with the battery/FPS strip, one alpha plane only), meaningful with integer scale (GB 3× = 480x432). Zero CPU; 74 MB/s of DDR reads on the LCD. TODO(hw): plane alpha and bandwidth, and not on 1080p HDMI |
| 15 | Localisation (French first) | M | M | String table for the UI and the in-game menu; DejaVu covers Latin-1 |
| 16 | Rewind (8-bit/16-bit systems only) | M | M | Serialize every N frames into a delta-compressed ring (NES/GB/SMS states are 8-40 KB); menu toggle per system; off for PS1/N64/arcade |
| 17 | Samba toggle | L/M | S | ksmbd is in the image; the script is in rom-transfer §3.3 |
| 18 | Sort options, random game, "All games" collection | L/M | S | `games_sort()` comparators; Y on the carousel = random game |
| 19 | Rumble | L/M | S | `EVIOCSFF`/`FF_RUMBLE` in `input.c` behind `hin_rumble()`; the kernel side is enabled |
| 20 | Stick → d-pad, port device choice (multitap) | L/M | S | Input layer option; Controls page lists `SET_CONTROLLER_INFO` devices per port |
| 21 | Kid mode | L | S | Filter on `kidgame`, lock Settings behind a combo |
| 22 | RetroAchievements | M for some users | L | rcheevos (MIT), keep `SET_MEMORY_MAPS` instead of ignoring it, login token, WiFi, a valid clock, hardcore mode (disables states/FF) |
| 23 | Run-ahead / frame delay | L | M / S | Run-ahead doubles the core cost: only 8-bit cores. Frame delay: auto-tuned sleep before `retro_run` in vsync mode |
| 24 | Custom collections | L | M | Read ES `custom-*.cfg` lists from `/data/rsos/collections/`; "Add to collection" in game options |
| 25 | 7z | L | M | LZMA SDK (public domain) |
| 26 | Metadata editor, background music, theme downloader, video previews | L | M-L | Low value on a 3.5" handheld; video previews would need the Cedrus VPU or software decode |

---

## 2. Part 2: missing systems and cores

### 2.1 Performance yardstick

- The A20 core is the same Cortex-A7 as the Raspberry Pi 2 (4× A7 at 900 MHz), running at up to 960 MHz here, but with
  **2 cores instead of 4** and a slower DDR3 controller (cores.md). Single-thread speed is about a Pi 2; anything
  RetroPie says needs a Pi 3 (4× A53 at 1.2 GHz, ~1.5-2× faster per core) is at best borderline here.
- The closest handheld peer is the **Miyoo Mini (Onion)**: 2× A7 at 1.2 GHz. What Onion runs "well" needs ~20 % margin
  on the RetroStone2; what Onion runs "with frameskip" will be too slow.
- GPU: Mali-400 on lima = **GLES 2.0 only**. Any core that needs GLES 3 (Flycast's current renderer, PPSSPP's modern
  paths, Beetle PSX HW, Dolphin) is out, independently of CPU.
- All speeds below are estimates from those peers, not measurements. TODO(hw): measure each added core as cores.md
  asks for the existing ones.

### 2.2 Host prerequisites for the new classes of cores

| Need | Who needs it | Today | Effort |
|---|---|---|---|
| `RETRO_DEVICE_KEYBOARD` (+ `SET_KEYBOARD_CALLBACK`) from USB keyboards | DOS, C64, Amiga, Spectrum, CPC, Atari 8-bit/ST, TIC-80, ScummVM text input | Keyboards are UI-only; the host answers JOYPAD and ANALOG only | M |
| `RETRO_DEVICE_MOUSE` / `POINTER` | ScummVM (optional), Amiga/ST games, DOS (optional), TIC-80 | Not answered | S (with the above) |
| Cores' own virtual keyboards | vice, puae, cap32, fuse, atari800, dosbox-pure, hatari | Work already: they are drawn by the core and driven by the RetroPad. Check each core's vkbd button against our Select hotkey (Select alone does reach the core) | none |
| `retro_load_game_special` / subsystems | Sufami Turbo, SGB with separate BIOS in some cores, TGB Dual link | `SET_SUBSYSTEM_INFO` ignored | M |
| Data files in the system dir | ScummVM (`scummvm.zip` data), prboom (`prboom.wad`), fake-08/TIC-80 none | `system_files` / `system_tree` copy mechanism exists (bluemsx) | none |
| Portrait / rotated output | Vectrex, WonderSwan vertical, arcade vertical | Cores rotate in software | none |
| Standalone (non-libretro) programs with DRM master hand-off | OpenBOR, official PICO-8 (user-supplied Raspberry Pi build), DraStic (closed source) | Not supported; the process model (drop master, exec, wait) would allow it with SDL2 KMSDRM in the rootfs | M, and outside "libretro only" |

### 2.3 Consoles and handhelds not covered

| System | RetroPie doc | Best core for the A20 | Expected on A20 | Licence | Build | Verdict |
|---|---|---|---|---|---|---|
| PC Engine SuperGrafx | RP:PC-Engine | beetle-supergrafx | Full speed (same code base as the shipped pce_fast) | GPL-2.0 | S (same Makefile style as pce_fast) | **Add now** (cores.md already lists it as the gap) |
| Pokémon mini | RP:Pokemon-Mini | PokeMini | Full speed, large margin | GPL-3.0 | S | **Add now** |
| Game & Watch | RP:Game-&-Watch | gw-libretro | Full speed (Lua) | Zlib | S | Later: needs `.mgw` files built from MAME artwork (copyright), low value |
| Supervision | - | potator | Full speed | Unlicense | S | Later, low value |
| Virtual Boy | RP:Virtual-Boy | beetle-vb | Borderline: Pi 2 class is at the limit, Onion runs it with frameskip on some games | GPL-2.0 | S | Later, as experimental; 3D red/black shown flat (anaglyph option) |
| Intellivision | RP:Intellivision | FreeIntv | Full speed | GPL-3.0 | S | Later: needs `exec.bin` + `grom.bin` (not free); keypad through the core's mini-keypad |
| Vectrex | RP:Vectrex | vecx | Full speed (software renderer) | GPL-3.0 | S | Later: colour overlays matter for these games and we have no overlays |
| Odyssey 2 / Videopac | RP:VideoPac-Odyssey-2 | o2em | Full speed | Artistic-2.0 | S | Later, low value; needs `o2rom.bin` |
| Channel F | RP:Fairchild-ChannelF | FreeChaF | Full speed | GPL-3.0 | S | Later, very low value |
| Arduboy | - | arduous | Probably full speed (AVR at 16 MHz; Onion ships it) | GPL-3.0 | S | Later: free homebrew library, good handheld fit |
| Sufami Turbo, BS-X / Satellaview | - | snes9x2010 (BS-X as content with `BS-X.bin`); Sufami needs a subsystem | As SNES | Snes9x (non-commercial) | none / M (subsystem) | Later, low value |
| Super Game Boy border | - | mGBA (GB/GBC with SGB mode) | GB/GBC probably full speed, GBA mode not (Pi 3 class needed) | MPL-2.0 | S-M (CMake) | Later, as a per-game GB core for SGB titles only; gpsp stays for GBA |
| Mega Drive, accurate alternative | RP:Mega-Drive-Genesis (lr-genesis-plus-gx is RetroPie's default on Pi 2+) | Genesis Plus GX | MD/SMS full speed with less margin than PicoDrive; Sega CD heavier | Non-commercial (acceptable now) | S | Later, per-game alternative (SMS FM sound, accuracy) |
| Neo Geo CD | RP:Neo-Geo | neocd_libretro | Borderline (C 68000, no Cyclone) | LGPL-3.0 | M (C++) | Later; geolith (experimental) already covers `neocd` |
| PC-FX | RP:PC-FX | beetle-pcfx | Borderline; a handful of games | GPL-2.0 | S | Not now (value) |
| Game Boy link on one device | - | TGB Dual | Full speed (two GBs) | GPL-2.0 | S + host subsystem (M) | Not now |
| Atari Jaguar | RP:Atari-Jaguar | virtualjaguar | Too slow even on much faster ARM | GPL-3.0 | - | **Not viable** |
| Sega Saturn | RP:Saturn | yabasanshiro / yabause | Far too slow; wants GLES 3 and 4 fast cores | GPL-2.0 | - | **Not viable** |
| Dreamcast | RP:Dreamcast | flycast | Pi 3 + VideoCore was already marginal; 2× A7 and lima GLES 2 cannot run it | GPL-2.0 | - | **Not viable** |
| PSP | RP:PSP | ppsspp | A few 2D titles at partial speed at best; needs a stronger GLES stack | GPL-2.0 | L | **Not viable** |
| Nintendo DS | RP:Nintendo-DS | desmume2015 (melonDS has no ARM32 JIT) | Far below full speed | GPL-2.0 / GPL-3.0 | M | **Not viable** as libretro. DraStic (closed, ARM-optimised) is the only thing that might; outside the architecture |
| 3DO | RP:3do | opera | Pi 3 class runs it at partial speed | LGPL-2.1 + non-commercial parts | M | **Not viable** |
| GameCube, Wii, PS2 | RP:GameCube, RP:Wii, RP:Playstation-2 | - | - | - | - | **Not viable** |

### 2.4 Home computers

All of these want a keyboard. The listed cores ship their own on-screen keyboard driven by the RetroPad, so they work
without the host keyboard support of 2.2; a USB keyboard when docked needs it.

| System | RetroPie doc | Best core for the A20 | Expected on A20 | Licence | Build | Verdict |
|---|---|---|---|---|---|---|
| DOS | RP:PC | **dosbox-pure** (dosbox-svn as the older fallback) | Real-mode and 386-era games (Commander Keen, Monkey Island, Prince of Persia, Wolf3D) full speed with the ARM dynrec core; 486-class games (DOS Doom at full size, Duke 3D) slow | GPL-2.0 | M (large C++ unity build, no deps) | **Add now**: loads games straight from `.zip`, gamepad mapper and on-screen keyboard built in, no BIOS |
| ScummVM | RP:ScummVM | **scummvm** (libretro) | 2D point-and-click engines (SCUMM, AGI/SCI0-1, Kyrandia, Broken Sword 1-2) full speed; hi-res/video-heavy engines slower | GPL-3.0 | M-L (big C++ tree, several minutes; trim engines to cut the .so size; `scummvm.zip` data in the system dir) | **Add now**: the best handheld fit of all computer systems (the core turns the pad into a mouse cursor); freeware games (Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the Temptress) could be bundled |
| Commodore 64 | RP:Commodore-64 | vice **x64** (fast); x64sc per game | x64 full speed; x64sc (cycle-exact) borderline | GPL-2.0 | M (large, `EMUTYPE=x64`) | **Add now/next** |
| ZX Spectrum | RP:ZX-Spectrum | **fuse** | Full speed, large margin | GPL-3.0 | S | **Add now**; the Spectrum ROMs are built in (Amstrad allows their distribution with emulators) |
| Amstrad CPC | RP:Amstrad-CPC | **cap32** (crocods is lighter, MIT) | Full speed | GPL-2.0 | S | **Add now**; ROMs built in |
| Atari 800 / 5200 | RP:Atari-800-and-5200 | atari800 | Full speed | GPL-2.0 | S | Later: needs OS/BASIC/5200 ROMs (check whether the libretro fork has the Altirra replacement ROMs) |
| Amiga | RP:Amiga | uae4arm-libretro (ARM JIT, written for Pandora-class CPUs); PUAE / puae2021 as fallback | A500 (OCS/ECS) likely full speed with uae4arm; AGA borderline; PUAE heavier | GPL-2.0 | M-L (uae4arm port less maintained; PUAE big) | Later: high value in Europe but Kickstart ROMs are commercial (AROS replacement works for some games) and WHDLoad setup is fiddly |
| Atari ST | RP:Atari-ST-STE-TT-Falcon | hatari | ST probably full speed; STE borderline; Falcon no | GPL-2.0 | M (needs zlib, some patches) | Later: EmuTOS (GPL) can be shipped; many games need a mouse |
| Apple II | RP:Apple-II | AppleWin (libretro port) | Probably full speed (6502 at 1 MHz), but the port is young and untested on ARM32 | GPL-2.0 | M | Not now |
| Macintosh (68k) | RP:Macintosh | minivmac | Full speed (Mac Plus class) | GPL-2.0 | S | Not now (mouse-only, ROM needed) |
| ZX81 | RP:ZX81 | 81 | Full speed | GPL-3.0 | S | Not now (value) |
| Sharp X68000 | RP:Sharp-X68000 | px68k | Borderline | GPL-2.0 | M | Not now |
| Thomson, PC-88, PC-98, TI-99, Oric, CoCo/Dragon | RP:Thomson-MOTO etc. | theodore, quasi88, np2kai, MAME | varied; np2kai too heavy | varied | M | Not now (value) |

### 2.5 Fantasy consoles, ports and game engines

| System / game | RetroPie doc | Best core | Expected on A20 | Licence | Build | Verdict |
|---|---|---|---|---|---|---|
| PICO-8 | (PICO-8 is installed by hand in RetroPie) | **fake-08** (retro8 as the alternative) | Most carts full speed (Onion ships fake-08 on 1.2 GHz A7); CPU-heavy carts slow | MIT (retro8 GPL-3.0) | S-M | **Add now**: huge free cart library, perfect handheld fit. The official PICO-8 binary would need the standalone launcher of 2.2 |
| TIC-80 | - | tic80 | Most carts full speed; mouse-driven carts need the pointer device | MIT | M (CMake, several script VMs; build only Lua to keep it small) | Later |
| Doom | RP:Doom | **prboom** | Full speed (320x200 software) | GPL-2.0 | S | **Add now**: Freedoom 1/2 (BSD-3-Clause) can be bundled, so it plays out of the box; users add `doom.wad`/`doom2.wad` |
| Quake | RP:Quake | tyrquake | 320x240 software at roughly 30-60 fps (estimate) | GPL-2.0 | S | Later (measure); shareware `pak0.pak` bundling is an owner decision |
| Cave Story | RP:Cave-Story | nxengine | Full speed | GPL-3.0 | S (data files: the freeware game) | **Add now** |
| Wolfenstein 3D | RP:Wolfenstein-3D | ecwolf | Full speed | GPL-2.0 + id licence for parts | S-M | Later |
| Bomberman clone | - | mrboom | Full speed; up to 8 players with USB pads | MIT | S | Later (fun docked; tiny) |
| 2048, Dinothawr, xrick, REminiscence, Lutro games | RP:Dinothawr, RP:Xrick | 2048, dinothawr, xrick, REminiscence, lutro | Full speed | Unlicense / GPL-3.0 / various | S each | Later, low value each |
| RPG Maker 2000/2003 | - | EasyRPG | Mostly full speed | GPL-3.0 | M (liblcf and several libraries) | Later |
| OutRun | RP:Cannonball | cannonball | Probably full speed | Non-commercial (acceptable now) | S | Later (needs the arcade ROMs) |
| OpenBOR | RP:OpenBOR | none (no maintained libretro core) | - | BSD-3-Clause | - | **Not viable** without the standalone launcher (2.2) |
| Daphne / laserdisc | RP:Daphne | none (standalone, video decoding) | - | - | - | **Not viable** |

---

## 3. Top 10 features to add next

| # | Feature | Effort | Why now |
|---|---|---|---|
| 1 | Jump to letter + search in game lists | S | The most-used ES feature missing; big libraries on a d-pad are painful; Y is free |
| 2 | Fast-forward hotkey (Select+R2, toggle or hold) | S | The engine already exists; handheld owners use it for RPG grinding |
| 3 | Updater from `/data/update/` with UI | M | A/B slots are ready; without it every release is a reflash, which is where saves get lost |
| 4 | Game switcher (recent games with auto-state thumbnails, one hotkey) | M | Onion's most-liked feature; all parts exist (auto states, thumbnails, last played, resume) |
| 5 | Play time tracking and a "most played" view | S | Cheap, asked for on every handheld firmware |
| 6 | Screenshot hotkey (Select+L2), "use as picture" for unscraped games | S | PNG writer exists; also gives art to unscraped lists (RP:Take-and-Scrape-Your-Own-Screenshots) |
| 7 | In-game remap editor with turbo | M | Remap files exist but must be edited on a PC; turbo is a common shmup request |
| 8 | Bluetooth pairing screen | M | BT is a toggle with no way to pair from the device |
| 9 | Cheats (`.cht`, per game, Select+X page) | M | RetroPie had them; the 6 main cores support `retro_cheat_set` |
| 10 | Per-game scaling + CPU profile, hide and delete a game | S | Closes the "game options" set (runcommand video mode, muOS per-game governor, ES hide/delete) |

Next tier: HDMI volume (S), BIOS status screen (S), WiFi scan + NTP (S), LCD grid/scanline mask on the overlay plane
(M, TODO(hw)), French UI (M), rewind for 8/16-bit systems (M), host keyboard/mouse for computer cores (M).

## 4. Top 10 cores to add next

| # | Core (system) | Effort | Why |
|---|---|---|---|
| 1 | prboom (Doom) + Freedoom bundled | S | Full speed, GPL, playable out of the box with free data |
| 2 | fake-08 (PICO-8) | S-M | Large free library, ideal on a 640x480 handheld, MIT |
| 3 | dosbox-pure (DOS) | M | RetroPie's most used computer system; zip loading and on-screen keyboard make it pad-friendly |
| 4 | scummvm | M-L | Point-and-click plays perfectly with a pad cursor; freeware games can be bundled |
| 5 | beetle-supergrafx (SuperGrafx) | S | Closes the PC Engine family gap noted in cores.md |
| 6 | fuse (ZX Spectrum) | S | Full speed, ROMs built in, big European library |
| 7 | vice x64 (C64) | M | High demand; x64 is full speed |
| 8 | cap32 (Amstrad CPC) | S | Full speed, ROMs built in |
| 9 | nxengine (Cave Story) | S | A complete freeware game, full speed |
| 10 | PokeMini (Pokémon mini) | S | Full speed, tiny |

Next: atari800, Genesis Plus GX (per game), mGBA (SGB only), beetle-vb (experimental), mrboom, TIC-80, tyrquake,
uae4arm/PUAE, hatari. Not viable on the A20: Saturn, Dreamcast, PSP, NDS (libretro), 3DO, Jaguar, GameCube and up.

## 5. Notes for other owners

- `docs/requirements.md` §1.2, §2, §14 and `docs/integration-todo.md` "Remaining gaps" are stale (they list the resume
  prompt, states, fast-forward and the `cz_buttons`/`p1` settings as missing; all exist now). Worth a refresh so they
  do not mislead the next agent.
- Settings > Network shows "Windows file share (later)": either finish it (ksmbd is already in the image) or hide it.
- Hotkey budget: Select + L2/R2/Y/Up/Down are free today. Proposal: R2 fast-forward, L2 screenshot, Y game switcher.
- Every new core needs the same `cores/<id>.ini` metadata (systems, extensions, BIOS, `experimental`), a ROM-folder
  row in cores.md, a theme folder (Carbon art exists for most of the systems above) and a `data-partition` folder.
