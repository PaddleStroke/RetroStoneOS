# Bundled homebrew games

RetroStoneOS ships a few free games so that a fresh console is fun out of the box, like RetrOrangePi was. Every
bundled game needs a licence that allows redistribution, including commercial redistribution, for **both code and
assets**, and its files come from a public, hash-checked download. Nothing with an unclear licence goes in.

8BCraft's own games (Bomber Mole, Leady Squid) are not homebrew ROMs: they are the RetroStone system, built into
the image from the public RetroStone VC repository and never copied to `roms/` ([vc-games.md](vc-games.md)).

| Game | Buildroot option | Default | Source of the files | Licence |
|---|---|---|---|---|
| µCity (`gbc`) | `BR2_PACKAGE_RSOS_UCITY` | on (every defconfig) | the v1.2 GitHub release ROM and the source of the tag, hash-checked | GPL-3.0+ / BSD-2-Clause / CC BY-SA 4.0, see [µCity](#µcity-gbc-folder) |
| Freedoom (`doom`) | `BR2_PACKAGE_RSOS_FREEDOOM` (Phase 1 and 2 sub-options) | on (every defconfig) | Freedoom 0.13.0 release zip, hash-checked | BSD-3-Clause, see [Freedoom](#freedoom-doom-folder) |
| Open-licence supplement | `BR2_PACKAGE_RSOS_HOMEBREW_OPEN` | **off** until the owner reviews the list | downloaded from the authors' GitHub releases | open licences (see the tables) |
| Cave Story (freeware) | `BR2_PACKAGE_LIBRETRO_NXENGINE_CAVESTORY` | **off**, owner decision | the nxengine-libretro source tree (`datafiles/`) | freeware without written terms, see [Cave Story](#cave-story-not-shipped-by-default) |

**History.** Until 2026-09-30 the images also carried ten games shipped only with their authors' permission (Run to
Databay, Espionage, Into the Blue, Retroid, Rope & Bombs, Nomolos, The Legends of Owlia, Liquid Space Dodger,
Virtua Worm 1 and 2), fetched by CI from a private repository. The owner dropped that permission-only set on
2026-09-30: only µCity was kept from it, now downloaded from its public release like Freedoom.

## How it works

- Each game is its own package, which installs to the root filesystem (read-only):
  - `/usr/share/rsos/homebrew/<system>/`: the game files and a `gamelist.xml` (from the package directory);
  - `/usr/share/rsos/licenses/<game>/`: the licence texts and credits.
- `buildroot-external/package/rsos-homebrew/` is the first-boot seeding: `/usr/bin/rsos-seed-homebrew` copies every
  `/usr/share/rsos/homebrew/<system>/` folder to the data partition. The game packages depend on it.
- The downloads are checked against the packages' `.hash` files, and `make legal-info` saves their sources (µCity's
  GPL source included) in the legal-info archive of every release.

### First-boot seeding

`rsos-seed-homebrew [--force] [DATA_DIR]` (POSIX sh, BusyBox only, tested with BusyBox 1.36 `sh` and `dash`):

1. If `DATA_DIR/rsos/homebrew-seeded` exists, it exits at once (unless `--force`).
2. For every folder `/usr/share/rsos/homebrew/<system>/`, it copies the files to `DATA_DIR/roms/<system>/`. A file
   that already exists there is never overwritten.
3. `gamelist.xml`: if the user has none, ours is copied. Otherwise our `<game>` entries whose `<path>` the user's
   file does not list yet are inserted before its last `</gameList>` (paths compared without `./`). The user's entries
   are never changed. A file without `</gameList>` (or a self-closing `<gameList/>`) is left alone.
4. It writes the marker `DATA_DIR/rsos/homebrew-seeded` and syncs. On an error it returns 1 without the marker, so
   the next boot tries again. Temporary files end in `.rsos-tmp` and are renamed atomically.

`--force` ("restore the bundled games", for a future UI entry) runs it again; it still never overwrites or deletes.

**Call** (in the first-boot code, `board/common/rootfs-overlay/usr/libexec/rsos/data-partition`), inside the
`if [ ! -d "$MNT/roms" ]` block, after the `mkdir` loop and before its `sync`:

```sh
	[ -x /usr/bin/rsos-seed-homebrew ] && /usr/bin/rsos-seed-homebrew "$MNT"
```

Doing it inside that block keeps it off the normal boot path (the script also has its own marker check, so an
unconditional call after the mount would be safe too, at the cost of one `stat`). Most of the time goes to Freedoom
(58 MB); µCity is 128 KB.

## µCity (`gbc` folder)

[µCity](https://github.com/AntonioND/ucity) 1.2 (Antonio Niño Díaz, 2018-01-31), an open-source city builder for the
Game Boy Color. Code GPL-3.0+ (the files with the GPL header), GBT Player (music engine) BSD-2-Clause, graphics and
music CC BY-SA 4.0 ([upstream readme, "License"](https://github.com/AntonioND/ucity/blob/v1.2/readme.rst)).

- Package `buildroot-external/package/rsos-ucity/`: the main download is the source of the tag v1.2
  (`rsos-ucity-1.2.tar.gz`, sha256 `9ffdd325294ebbd8a4adcc5a5bb40a2ea4b981cb8e4889a5392ef4796cdb2dc1`), so that
  `make legal-info` publishes the GPL source; the ROM is the
  [v1.2 release asset](https://github.com/AntonioND/ucity/releases/download/v1.2/ucity.gbc) (`_EXTRA_DOWNLOADS`,
  sha256 `39a254f95672c3dafe8ae0e12763366b990bc9993fab13bd0c8283b0459be102`), installed unchanged. Nothing is built.
- Installs `/usr/share/rsos/homebrew/gbc/ucity.gbc` with its `gamelist.xml`, and `LICENSES/homebrew/ucity/`
  (`NOTICE.txt`: credits and the written source offer; the GPL-3.0, BSD-2-Clause and CC BY-SA 4.0 texts) to
  `/usr/share/rsos/licenses/ucity/`.
- Attribution: "µCity by Antonio Niño Díaz. Graphics and music CC BY-SA 4.0." (NOTICE, gamelist developer field).
- TODO(owner): the contact address for source requests in `LICENSES/homebrew/ucity/NOTICE.txt`.

## Freedoom (`doom` folder)

[Freedoom](https://freedoom.github.io/) is a complete, free game for Doom engines: new levels, graphics, sounds and
music made by the Freedoom contributors, under the **BSD-3-Clause** licence (`COPYING.txt` of the release). It makes the
`doom` system (prboom, docs/cores.md "Third batch") playable out of the box, and it is also the IWAD that lets users
play many free PWADs without owning Doom.

- Package `buildroot-external/package/rsos-freedoom/`: downloads `freedoom-0.13.0.zip` from the GitHub release (sha256
  `3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59`, 24 MB) and installs
  - `/usr/share/rsos/homebrew/doom/freedoom1.wad` (Phase 1, 28.8 MB, option `BR2_PACKAGE_RSOS_FREEDOOM_PHASE1`) and
    `freedoom2.wad` (Phase 2, 28.8 MB, `BR2_PACKAGE_RSOS_FREEDOOM_PHASE2`), both on by default;
  - `/usr/share/rsos/homebrew/doom/gamelist.xml` (names, descriptions; `package/rsos-freedoom/doom.xml`);
  - `/usr/share/rsos/licenses/freedoom/`: `COPYING.txt`, `CREDITS.txt`, `CREDITS-MUSIC.txt` (the BSD licence requires
    the notice with binary redistribution).
- Seeding: it depends on `BR2_PACKAGE_RSOS_HOMEBREW` and uses the same first-boot tool: `rsos-seed-homebrew` copies
  every `/usr/share/rsos/homebrew/<system>/` folder, so `doom/` lands in `/data/roms/doom/` with its gamelist. A card
  that was already seeded by an older image does not get it automatically (the marker exists); "restore the bundled
  games" (`rsos-seed-homebrew --force`) adds it.
- Cost: 57.6 MB in the root filesystem, and the same again on the data partition after seeding. Drop one phase (set its
  option to `n`) if the root filesystem gets tight.
- The BSD licence allows redistribution in a commercial product with the notice; no source offer is needed (the WADs
  are the preferred form for levels; the build sources are on GitHub anyway).

## Cave Story (not shipped by default)

nxengine-libretro's source tree carries the freeware **Cave Story** 1.0.0.6 (Studio Pixel, 2004) with the Aeon Genesis
English translation in `datafiles/`. `BR2_PACKAGE_LIBRETRO_NXENGINE_CAVESTORY` installs `Doukutsu.exe`, `data/` and the
readme to `/usr/share/rsos/homebrew/cavestory/` (3.4 MB, with a gamelist from
`package/libretro-nxengine/cavestory.xml`); the Windows tools (`DoConfig.exe`, `OrgView.exe`) and the manual are left
out. Seeded to `/data/roms/cavestory/` like the other bundled games.

**Why it is off:** the readme only says "This program is freeware" and gives no redistribution terms. Pixel has let the
freeware version circulate freely for twenty years (the libretro project and several handheld firmwares ship it), but
it is not a written licence, and Nicalis sells the commercial versions. That does not meet the rule of this page
("a licence that allows commercial redistribution" or the author's permission). **TODO(owner):** decide, or ask Studio
Pixel / Nicalis; then set the option in the defconfigs. Users can always copy the freeware game themselves.

## Open-licence supplement (optional, default off)

Status: **not filled yet.** `BR2_PACKAGE_RSOS_HOMEBREW_OPEN` exists (default off) with an empty game list; the
research for SNES, GBA, Master System/Game Gear was still running when this was written. Same strict bar as before:
public domain/CC0, MIT/BSD/zlib/WTFPL, GPL (with source), CC-BY/CC-BY-SA, for **both code and assets**.

- **Arcade:** nothing qualifies. The "free ROMs" on mamedev.org (Gridlee, Robby Roto, Super Tank, ...) are licensed
  for non-commercial distribution only.
- **PS1:** nothing found yet.

### Verified open-licence candidates for NES and Game Boy (not shipped)

| System | Game | Author | Version | Licence (proof) | ROM URL | Notes |
|---|---|---|---|---|---|---|
| nes | Concentration Room | Damian Yerrick | v0.02a | GPL-3.0+ with an exception allowing exact ROM copies without source ([README](https://github.com/pinobatch/croom-nes)) | https://github.com/pinobatch/croom-nes/releases/download/v0.02a/croom.nes | best NES candidate |
| nes | Thwaite | Damian Yerrick | v0.04 | GPL-3.0+ ([README](https://github.com/pinobatch/thwaite-nes)) | https://github.com/pinobatch/thwaite-nes/releases/download/v0.04/thwaite.nes | source offer needed |
| nes | Super Tilt Bro | sgadrat | beta-6 | WTFPL ([LICENSE](https://github.com/sgadrat/super-tilt-bro/blob/master/LICENSE)) | itch.io only (no stable URL) | 2 players; check that the LICENSE also covers the art |
| gb | Libbet and the Magic Floor | Damian Yerrick | v0.08 | zlib ([README](https://github.com/pinobatch/libbet)) | https://github.com/pinobatch/libbet/releases/download/v0.08/libbet.gb | |
| gb | Tobu Tobu Girl | Tangram Games | 1.0 | MIT (code) + CC-BY-4.0 (assets) ([README](https://github.com/SimonLarsen/tobutobugirl)) | no GitHub release; the authors' site | attribution needed |

## Rejected / needs permission

| Game | System | Reason | Contact |
|---|---|---|---|
| Nova the Squirrel | NES | code GPL-3.0, but assets CC-BY-NC-SA 4.0 ("may not be sold without permission") | NovaSquirrel, GitHub `NovaSquirrel`, https://novasquirrel.com |
| Shock Lobster | GB | code zlib, but third-party assets (Lucky Bestiary, MinimalPixel and Electrox fonts, GB Studio Community Assets music) have licences not verified | Dave VanEe (`tbsp` on GitHub) |
| Loose files of the owner's local `homebrew/` folder (Classic Kong Complete, Crazybus, SHEEP, Super Connard, ...) | various | no licence or permission on record | per author |

## Research results: SNES and PS1 (open-licence supplement candidates)
Licences were checked at the authors' own repos or pages (2026-09-25). The owner should review before enabling `BR2_PACKAGE_RSOS_HOMEBREW_OPEN`.

| System | Game | Author | Licence | Verdict |
|---|---|---|---|---|
| SNES | **Super Sudoku v1.1** | raphnet | MIT, whole repo incl. assets | **Cleared.** A complete game. Ship the MIT notice. [ROM](https://raw.githubusercontent.com/raphnet/super_sudoku/master/releases/super_sudoku_v1.1.sfc) |
| SNES | unnamed-snes-game tech demo alpha v3 | undisbeliever | MIT code, CC BY-SA 4.0 art and music | Cleared, but only a tech demo. Needs attribution. |
| SNES | Memory Game v1.02, Horizontal Shooter v1.1, Castle Platformer v1.04 | undisbeliever | MIT code, own/CC0 art | Cleared. Small demos (filler). |
| PS1 | **VoXide 0.1.11** | Bonnie Studios | GPL-2.0+ code, CC0 assets | Cleared. **GPL source offer needed.** In development. Don't use "Minecraft" in marketing. The only stable URL is a third-party GitHub mirror. |
| PS1 | Zenmai (Zork I) 1.3.0 | msonrm | MIT, OFL font, Zork I MIT (Microsoft 2025) | Cleared but niche (a .psexe file). Don't use "Zork" as a brand. |

**Worth an email (best candidates):**
- Space Rescue Squad (undisbeliever@gmail.com, zlib code, but its resources are "only for this game")
- Sure Instinct (BenjaminSchulte; its music transcribes Bjørn Lynne's "snippers.mod")
- Dottie Dreads Nought (goldlocke.itch.io; no licence)
- Incognity (cand.itch.io)
- Tetrade (PS1; audio provenance, plus a Tetris trade-dress risk)

**Rejected:** Nova the Squirrel 1/2 (NC or all-rights-reserved assets); the retrobrews collection titles (freeware, with distribution approved for their own site only); games that reuse other companies' IP (CelesteSNES, Super Penalty Kick, rumbleminze ports, PS1 Cave Story/Quake/Another World ports); unfinished prototypes; SDK examples.

## Research results: GBA (open-licence supplement candidates)
Checked on the authors' repos, 2026-09-25. The owner should review before enabling.

| Game | Author | Licence | Verdict |
|---|---|---|---|
| **The Hat Chooses the Wizard** | Corwin & Gwilym Kuiper (agb) | MPL-2.0; music "Sylvan Waltz" by Otto Halmén, CC-BY 3.0 | **Cleared.** A platformer, ~12 levels. Credit Halmén and point to the source (github.com/agbrs/agb). ROM in [examples.zip v0.25.0](https://github.com/agbrs/agb/releases/download/v0.25.0/examples.zip) |
| **The Purple Night** | Kuiper brothers, music Sam Williams | MPL-2.0 (whole repo) | Cleared. An action platformer. A short confirmation from the agb team would be sensible. Same zip. |
| **CASCADE7 v1.0.0** | Mick Schroeder | MIT; Butano fonts CC0 | **Cleared.** A Drop7-like puzzle game. Ship it unmodified (name/logo trademark). [ROM](https://github.com/mick-schroeder/gba-cascade7/releases/download/v1.0.0/CASCADE7.gba) |
| **Noonlight Paradox v1.0.0** | zegalur | MIT code, CC-BY assets; OFL/M+ fonts | Cleared. A metroidvania jam demo. Credit zegalur. [zip](https://github.com/zegalur/noonlight-paradox-gba/releases/download/v1.0.0/noonlight_paradox_gba.zip) |
| Attack on Voxelburg (jam) | John Tsiombikas | GPL-3.0+ | Cleared, but a WIP with no sound. **GPL source offer needed.** |

**Worth an email:**
- Inheritors of the Oubliette (DrLancer-X; the best game, but its sound/music origins are unclear)
- Solar Guard (Deft Spade; GBA Jam splash logo, GM.DLS samples)
- Hyperspace Roll / Dungeon Puzzler's Lament (agb; music and font licences unclear)
- Rubido / Znax (MIT, but they must be built from source)
- HEXES (third-party music)
- 2048 Advance (CC0, but itch.io only)

**Rejected:**
- NC assets: Butano Fighter, Varooom 3D, µCity Advance, MeteoRain, Feline, BeatBeast, Bumper Neko Arena, Work-Life, Blind Jump.
- Paid or third-party assets: Skyland, Collie Defense.
- Trade dress or third-party IP: Tetris-likes, Minicraft, piugba, the Celeste/GBAStranger ports.
- Other: Goodboy Galaxy (commercial), Anguna (no licence), unfinished alphas.
- More leads: the gbadev.org games database lists a licence per entry.

## Research results: Mega Drive, Master System, Game Gear (open-licence supplement candidates)
Checked on the authors' repos, 2026-09-25. The owner should review before enabling.

| System | Game | Author | Licence | Verdict |
|---|---|---|---|---|
| MD | **Miniplanets (REMIX)** | Sik | zlib (the Sona driver too) | **Cleared.** A great game, but the ROM is on itch.io only: download it once by hand and host your own copy. |
| MD | **KłełeAtoms MD 1.2.1** | Nightwolf-47 | MIT | **Cleared.** Chain-reaction strategy, 1-4 players. [ROM](https://github.com/Nightwolf-47/KleleAtoms-MD/releases/download/v1.2.1/kleleatoms-md-121.bin) |
| MD | Minesweeper MD 1.1.1 | Nightwolf-47 | MIT | Cleared. [ROM](https://github.com/Nightwolf-47/Minesweeper-MD/releases/download/v1.1.1/minesweeper-md-111.bin) |
| MD | Alex vs Bus: The Race (pre3) | M374LX | GPL-3.0+ code, CC BY-SA 4.0 assets | Cleared. Plays to the end. **GPL source offer needed.** |
| MD | Dragon's Castle (2016) | Sik | zlib | Cleared. A prototype: play-test it first. [ROM @ pinned commit](https://raw.githubusercontent.com/sikthehedgehog/dragon/72ef51b648ee1e5bd1a7e9c6731b95f16cce7f28/witch.bin) |
| MD | Retail Clerk '89 1.0.1 | Hugues Johnson | MIT / CC BY-SA / PD music | Legally fine, but the author "would prefer" no physical reproductions, so send a courtesy email first. |
| SMS + GG | **Waternet 1.0** | joyrider3774 | MIT | **Cleared.** A pipe puzzle game. Ship the ROM only (not the cartridge-label art). |
| SMS | **Data Storm 1.00** | Haroldo O. Pinheiro | Apache-2.0 | **Cleared.** A Turmoil-style shooter. |
| SMS | Space Tonbow 1.0.0 | Bofner | GPL-3.0 (art and music by the author) | Cleared. itch.io only, or build it from source. **GPL source offer needed.** |

**Needs permission:** Digger Ball (SMS; GPL code, assets not explicitly licensed; contact Aypok on SMS Power), Jump 'n Bump MD (GPL, but the finished ROM is sold on itch).

**Rejected:**
- Non-commercial licences: all Mojon Twins games (CC BY-NC-SA), Griel's Quest/Chase MD, Petris (NC).
- No redistribution or no licence: Break An Egg, Old Towers.
- Commercial titles: Xeno Crisis, Tanglewood, Paprium, Cave Story MD, and others.
- Third-party IP ports.
