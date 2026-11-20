# Store: installing ports from the user's own ROM - design

Status: implemented in this fork (design written 2026-11-06; see "Status" at the end for what is built). Extends [store-design.md](store-design.md). Scope: the store carries
**only the ports built for this console** (curated, signed by the maintainer, each one tested on hardware). The store never
holds or downloads any game data: the data is made on the console from a ROM the player already has.

## What was measured (hardware, 2026-11-06)
Ship of Harkinian 9.3.1's own extractor, run on a RetroStone2 Pro with an Ocarina of Time 1.2 dump:

| | |
|---|---|
| Time | 207 s |
| Peak memory (process) | 810 MB of 1006 MB; lowest MemAvailable 129 MB |
| Result | `oot.o2r` 33,914,022 bytes, all 39,032 entries identical to the PC build |
| Extractor files | ~150 MB in thousands of small files = 2.6 GB on the data partition (128 KB per file) |
| What failed | an address-space cap of 768 MB (silent failure); the GUI "Generate one now?" popup (no gamepad navigation) |

So extraction on the console is possible but takes nearly all RAM: it must run alone, guarded, and be one-shot.

## Three kinds of port
| Kind | Examples | Package holds | Install step on the console |
|---|---|---|---|
| `port` / extract | Ocarina of Time, Majora's Mask, Star Fox 64, Mario Kart 64, Smash | program, libraries, launcher, port archive (`soh.o2r`), extractor description files (one archive) | find the ROM, run the extractor headless, keep the made archive (`oot.o2r`) |
| `port` / copy-rom | Perfect Dark, GoldenEye (if its data is not compiled in) | program, launcher | find the ROM, convert to `.z64`, copy it into the port's folder |
| not offered | Super Mario 64 (data compiled into the program) | - | store page says "build it on a PC" and links the recipe |

## Catalog entry (new kind `port`)
```json
{ "id": "oot", "name": "Ocarina of Time", "system": "native", "kind": "port", "arch": "armhf", "version": "9.3.1.5",
  "url": ".../oot-9.3.1.5.tar", "size": 61000000, "sha256": "...", "unpacked": 140000000,
  "launcher": "Ocarina of Time.sh", "cover": ".../oot.png",
  "rom": { "name": "Ocarina of Time (US 1.2)", "sha1": ["41b3bdc4...", "..."], "size": [33554432],
           "mode": "extract", "extract": "extract.sh", "needs_ram_mb": 850, "est_minutes": 4, "needs_disk_mb": 400 },
  "quality": { "tested_build": "0.1.5.26", "speed": "smooth", "fps": "20-30", "controls": "..." } }
```
- `rom.sha1` is every accepted dump (byte order does not matter: the helper converts); `rom.size` lets the console hash only
  files of the right size.
- `mode`: `extract` (run `extract.sh` from the package) or `copy` (copy the converted ROM to `rom.dest`).
- `quality` is shown on the page; only entries tested on hardware are signed into the catalog.

## ROM discovery
1. Candidates: `*.z64|*.v64|*.n64` in `<library>/roms/n64*/` of every game drive, and the root of a mounted USB drive.
2. Skip any file whose size is not in `rom.size` (most of a 40-game folder is skipped without reading it).
3. SHA-1 of the remaining files (a 32 MB file takes a few seconds from the SD card); hashes are cached by path, size and
   mtime so "Look again" is instant.
4. Page states: **ROM found** (file name shown), **not found** ("put your <name> ROM in your N64 folder or on a USB drive,
   then Look again"), **wrong version** (a ROM of this game with another hash: says which versions are accepted).

## Install sequence (`rsos-store install-port ID [--rom FILE]`)
1. Preconditions, checked before anything is written: free disk (`needs_disk_mb` + package), battery above a floor or a
   charger, no game running, and enough free RAM (`needs_ram_mb`; the menu's background workers are paused, other helpers
   refused). If RAM is short: "Restart the console and try again" (nothing else is running right after boot).
2. Download the package (existing path: size and SHA-256 against the signed catalog), unpack into
   `<drive>/.store-<id>/` (a hidden temp folder; one place to delete).
3. Extract (mode `extract`): run the package's `extract.sh` as a **guarded child**: `oom_score_adj` 1000 (it dies before
   the menu or the system), address-space limit of 1.8 GB, a watchdog that kills it if MemAvailable < 90 MB, a timeout
   (3x `est_minutes`), progress and the child's log written to the temp folder. Needs a headless entry in the port
   (see "Port changes").
4. Verify: the output archive exists and is non-empty, and the extractor's own "done" marker is present.
5. Commit atomically: move the program folder into `<drive>/roms/native/<port>/`, the launcher next to it, the cover into
   `roms/native/media/covers/`; write the install record (`installed.tsv`, with the ROM's SHA-1 and the archive format).
   The record is `/data/rsos/store/installed.tsv` for the console's own `/data/roms`; an install into another roms
   folder (`--roms`, a test into a scratch folder) keeps its record in that folder, `<roms>/.rsos-store/installed.tsv`,
   so it never shows as installed in the menu.
6. **Always clean up**, success or failure or cancel: delete `.store-<id>/` and the downloaded package. A start-up sweep
   removes any `.store-*` left by a power cut.

Failure never touches an installed copy: an update stages beside it and swaps at step 5.

## Remove / update
- **Remove** deletes the program, archives and launcher; **saves and settings are kept** by default (they live in the port's
  folder, so the helper moves them to `<drive>/roms/native/.removed-saves/<port>/` and a later install restores them).
- **Delete this game** in the Ports list, on a launcher the store installed, removes the whole port the same way
  (`rsos-store remove-path <launcher>`), with the files an earlier install moved aside into
  `roms/native/.store-replaced/<port>/`; any other native game loses its launcher only.
- **Update** replaces program and launcher. The made archive (`oot.o2r`) is kept unless the catalog's `rom.archive_format`
  changed; then the page asks for the ROM again ("This update needs your game data prepared again").

## Port changes needed (in RetroStone-Ports)
- A headless extraction entry for each Torch/ZAPD port (`soh.elf --rsos-extract <rom> <outdir>`: no window, no popups, exit
  code, one-line progress, run before SDL initialises). The measured run used the GUI path; headless should also save
  memory (no GL, no ImGui).
- Package script per port (`package-<port>.sh`): builds the asset-free tarball and the catalog fragment.
- Perfect Dark / GoldenEye: a launcher variant that takes the ROM from `Data/` (already how they work) and a check of
  whether GoldenEye's converted assets are inside the binary (if so it moves to "not offered").

## UI (Game store page, then the carousel tile)
Page for a port: cover, description, quality line (speed, tested build), the ROM line, then one button: INSTALL (disabled
until a ROM is found). Progress screen: phases "Downloading", "Preparing game data (about 4 min)", "Installing"; B cancels
(kills the child, cleans up). The screen stays awake and the idle power-off is held. The carousel tile later just opens
this list (flat while there are fewer than ~20 entries).

## Safety summary
- No game data is hosted, downloaded or kept: only the player's own ROM is read, and only the made archive is stored.
- The package runs code as root: it is covered by the signed catalog, like `native` entries.
- Memory guard, timeout, atomic commit, mandatory cleanup, saves preserved on remove.

## Milestones
1. `rsos-store find-rom`: size filter, SHA-1, byte-order check, hash cache (host tests).
2. Headless extract for Ship of Harkinian; re-measure time and RAM on the console (expect lower than 810 MB).
3. `rsos-store install-port` with the guard, cleanup and failure tests (kill mid-way, no space, wrong ROM, power-cut sweep).
4. Port page in the Store UI and preview tests with a fake helper.
5. Extraction test per port on the console: Majora's Mask, Star Fox 64, Mario Kart 64, Smash (memory is unknown for these).
6. Copy-ROM ports (Perfect Dark, GoldenEye).
7. Package build automation (self-hosted runner on the home server or a local script) and the signed catalog.
8. Carousel tile (online only).

## Open questions
- Headless extraction's real RAM peak (may drop well below 810 MB; if not, only Ocarina of Time-sized ports fit).
- Whether to reduce the 2.6 GB on-disk cost further (extractor files could be streamed from the package tar instead of
  unpacked).
- Legal review of distributing the asset-free binaries, per port (see the earlier notes), before anything is public.

## Status (2026-11-06)
Measured on the console (all five extractors, byte-identical archives except Majora's Mask, which also plays): Ocarina of Time
200 s / 757 MB peak, Majora's Mask 384 s / 342 MB, Star Fox 64 857 s / 267 MB, Mario Kart 64 199 s / 684 MB, Smash 25 s / 114 MB.
Headless extraction patches live in RetroStone-Ports (`*-extract.patch`, Smash uses `smash/build-torch-arm.sh`).

Built: `rsos-store find-rom` and `install-port` (guarded extractor, atomic commit, mandatory cleanup, per-file install record; 62 host
checks in `store_test.sh`) and the port page in Settings > Game store (`ui/store_ui.c`): searches the ROM, shows the game, the question
before a long preparation, the charger/battery check, progress ("preparing N%"), the translated errors, LOOK AGAIN when there is no ROM;
leaving the screen cancels an install; no idle power-off meanwhile. Preview tests with `tests/fake-rsos-store.sh` (`make check-ui`).

Since then (2026-11-13): the store is live. The signed catalog and the packages are in
[KurdtNervanna/RetroStoneOS-Store](https://github.com/KurdtNervanna/RetroStoneOS-Store) with eight ports (Ocarina of Time,
Majora's Mask, Star Fox 64, Mario Kart 64, Smash 64 and Super Mario 64: the game data is extracted from the player's ROM;
Perfect Dark and GoldenEye 007: the ROM is copied), and the fork's releases ship the Settings > Game store screen. A Star Fox 64 install from the
store was confirmed on a RetroStone2 with fork build 0.1.5.30.

Not built yet: the carousel tile (online only). A full install on the console has been confirmed for Star Fox 64 only; the other
ports' installs have not all been repeated through the store on hardware. The legal review in "Open questions" is still open.
