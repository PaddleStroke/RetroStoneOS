# The RetroStone VC games (the "RetroStone" system)

RetroStoneOS ships 8BCraft's own games, made for **RetroStone VC**, a virtual console with the feel of a Super
Nintendo (320x240, 60 fps, 4 SNES pads, 32 KiB of battery RAM). They form the **RetroStone** system, the first entry
of the menu's carousel:

| Game | Players | Core | Menu entry (read-only) |
|---|---|---|---|
| **Bomber Mole**: a mole drops bombs to open the way, on three depths at once, through four seasons | 1 | `bombermole_libretro.so` | `/usr/share/rsos/games/retrostone/Bomber Mole.bombermole` |
| **Leady Squid**: a one-button "flap" game in a sunken world, a race for two | 1-2 | `leadysquid_libretro.so` | `/usr/share/rsos/games/retrostone/Leady Squid.leadysquid` |

Each game is a libretro core with the game built in: there is no ROM file to copy, the games are part of the system
(and of its updates).

## Source and licences

- The source is the **RetroStone VC** repository, a separate public git repository,
  [PaddleStroke/RetroStoneVC](https://github.com/PaddleStroke/RetroStoneVC): `sdk/` (the runtime, the libretro
  frontend; **MIT**, (c) 2026 Pierre-Louis Boyer (8BCraft)), `tools/` (MIT), the vendored third-party code of
  `sdk/third_party/` (libxmp-lite MIT, stb MIT/public domain, libretro.h MIT; `THIRD_PARTY.md`), and the games in
  `games/bombermole/` and `games/leadysquid/`: the game **code under MIT**, the game **art, music, sound, levels and
  design documents under CC BY-NC-SA 4.0** (`games/<game>/LICENSE` lists the paths; `LICENSE-CC-BY-NC-SA-4.0.txt`).
  The names and logos "Bomber Mole", "Leady Squid", "RetroStone" and "8BCraft" are not licensed: a fork must rename.
- Local builds use a checkout next to this one; CI checks out the public repository (below, and [ci.md](ci.md)
  section 4).
- `RSOS_VC_GAMES_LICENSE` lists MIT (the SDK, tools and game code) and CC-BY-NC-SA-4.0 (the game assets), and the
  package is redistributed (no `_REDISTRIBUTE = NO`): `make legal-info` saves the licence texts (`LICENSE-MIT`,
  `LICENSE-CC-BY-NC-SA-4.0.txt`, `THIRD_PARTY.md`, the two game `LICENSE` files) and the source (the synced
  checkout, without the art inbox). The CC BY-NC-SA art is one more reason the images must not be sold.
- On the device: `/usr/share/rsos/licenses/retrostone-vc/` (the same texts, and `COMMIT`: the RetroStone VC commit
  the cores were built from, also in the `commit =` line of each core's `.ini`).

## The package: `buildroot-external/package/rsos-vc-games/`

| Option | Default | |
|---|---|---|
| `BR2_PACKAGE_RSOS_VC_GAMES` | `y` in every defconfig | `scripts/ci/build-board.sh` turns it off when there is no checkout (`RSOS_CI_VC_GAMES=auto`) |
| `BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR` | `$(BR2_EXTERNAL_RETROSTONE_PATH)/../../RetroStoneVC` | the checkout (Buildroot's `local` site method) |
| `BR2_PACKAGE_RSOS_VC_GAMES_BOMBERMOLE`, `..._LEADYSQUID` | `y` | one core per game |

- **Build.** The RetroStone VC `Makefile`'s cross target, once per game:
  `make GAME=<game> ARM_CC=$(TARGET_CC) ARM_AR=$(TARGET_AR) ARM_FLAGS="$(TARGET_CFLAGS)" build/armhf/<game>_libretro.so`.
  `ARM_FLAGS` replaces its Cortex-A7 default with Buildroot's flags, and the toolchain wrapper adds the CPU flags, so
  the same line builds the armhf core of the RetroStone2 and RetroStone1 (Cortex-A7, NEON) and the aarch64 cores of
  the Raspberry Pi / Orange Pi boards (the output folder is called `build/armhf/` whatever the architecture). No
  change to RetroStone VC was needed.
- **Assets.** The games' assets are generated C (`games/<game>/tools/build_assets.py`), which needs **Pillow**.
  Buildroot has no host package for it, so the package runs the build host's `/usr/bin/python3`
  (`RSOS_VC_GAMES_PYTHON`, apt package `python3-pil`; `scripts/ci/install-deps.sh buildroot` installs it) and stops
  with "rsos-vc-games: /usr/bin/python3 with Pillow is needed" without it.
- **Art.** The package passes no `ART` or `CHAR_SIZE`, so the games build with RetroStone VC's default art: for
  Bomber Mole the committed set `games/bombermole/art-ai/` (the AI-generated art, placeholders only for the strips
  still to draw, 24-px characters; `make art-ai` there regenerates it from the inbox). The console shows the same
  art as the owner's Windows build. The validated-only look would be `ART=art` in `RSOS_VC_GAMES_MAKE_OPTS`.
- **Copy.** The checkout is copied without `build/` and `dist/` (the developer's own builds: an x86 object must never
  reach the target build), the image agent's art inbox `games/*/art/incoming/` (65 MB, not used by the build),
  `docs/art-preview/` and `.git`.
- **Installed:**
  - `/usr/lib/libretro/<game>_libretro.so` (stripped on the RetroStone2: Bomber Mole 403 KB, Leady Squid 231 KB, assets included);
  - `/usr/share/rsos/cores/<game>.ini`: `systems = retrostone`, `extensions = <game>`, **`no_content = true`**,
    `savestates = true`;
  - `/usr/share/rsos/games/retrostone/`: the two menu entries (small text files, never read), `gamelist.xml`
    (names, descriptions, genre, players, developer; `package/rsos-vc-games/gamelist.xml`) and
    `media/images/<name>.png` (the title screens of the games' `docs/screenshots/`, 640x480);
  - `/usr/share/rsos/licenses/retrostone-vc/`.
- **Which version.** A local build takes the checkout **as it is**, uncommitted changes included (the recorded commit
  then ends in `-dirty`). For an image made from a given commit while the checkout is being worked on, clone it
  (`git clone ~/path/to/RetroStoneVC ~/rsos/vc-head`, `git -C ~/rsos/vc-head checkout <commit>`) and point
  `BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR` at the clone. CI builds `RETROSTONE_VC_REF` (`main`) of the public repository.
- **The recorded commit** comes from `package/rsos-vc-games/rsos-vc-commit`: `git describe` of the checkout; when
  git cannot read it (a git worktree made on Windows, whose `.git` file points at a `C:/` path that WSL git cannot
  open), the HEAD ref read from the files (`C:/` mapped to `/mnt/c/`, no `-dirty` then); or the value of
  `RSOS_VC_COMMIT` when it is set (`make RSOS_VC_COMMIT=<sha> rsos-vc-games-rebuild`).
- **Rebuild** after a change in RetroStone VC: `make O=~/rsos/output rsos-vc-games-rebuild all` (the rebuild copies
  the checkout again). To build from another tree without touching the defconfig: set
  `BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR` with `make menuconfig`, or put
  `RSOS_VC_GAMES_OVERRIDE_SRCDIR = /path/to/RetroStoneVC` in `~/rsos/output/local.mk`.
- Without a checkout (a fresh clone of RetroStoneOS): set `BR2_PACKAGE_RSOS_VC_GAMES=n`; with the option on, the
  build stops ("ERROR: .../RetroStoneVC does not exist").

## In the menu

- **Built-in game folders.** `/usr/share/rsos/games/<system>/` (the UI's `builtin_games_dir`) is read **before**
  `/data/roms/<system>/`: the RetroStone system is always there, whatever the card holds, and cannot be deleted
  from the card by mistake. `post-build.sh` creates no `roms/retrostone/` folder on the data partition (it skips the
  systems of `no_content` cores). The game options have no "Delete this game" for these games (the root filesystem
  is read-only); "Hide this game", favorites, play time, the core choice and the per-game settings work as for any
  game (they are in `/data/rsos/gamedb.tsv`).
- **Carousel.** RetroStone is the first system (`frontend/src/ui/systems.c`), before the Nintendo systems: they are
  our own games and the console's maker comes first. The name "RetroStone" is the same in every language (context
  `system`, empty translation in the 21 catalogs); the tagline of the built-in themes, "Original games · 2026", is
  translated (context `theme`).
- **Art.** `rsos-dark`/`rsos-light`: `retrostone/theme.xml` (accent `9b59b6`) and `_art/icon-retrostone.svg`, the
  three stones of the RetroStone2 logo as one-colour pixel art. `gbz35`/`gbz35-dark`: an added `retrostone/` folder
  (not part of the upstream theme): `system.png`, the RetroStone2 logo without the "2", and `background.png`, its
  stones in two flat purples in gbz35's style. All of it is derived from `frontend/assets/splash/retrostone2-logo.png`
  (8BCraft).
- **Which core.** Each game has its own entry extension (`.bombermole`, `.leadysquid`), which is also its core's only
  extension: the UI (`ui_game_core()`) and the host (`coreinfo_pick()`) pick the core by extension, as for any
  system, so no code knows the game list.

## Launching without content

- The host (`rsos-frontend --run`, `src/host/content.c`) reads the core's `.ini`: with `no_content = true` it does not
  read the entry file and calls `retro_load_game(NULL)` (the cores declare `RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME`;
  a core that does not is refused with "This core needs a game file"). The log says
  `content: none (Bomber Mole.bombermole is a menu entry: the game is built into the core)`.
- Everything else keeps the ROM path model, with the entry file as the "ROM": the game's name is the entry's name,
  the system is `retrostone`:
  - **Save RAM** (the games' progress, 32 KiB): `/data/saves/retrostone/Bomber Mole.srm`, loaded before the first
    frame (the SDK starts its runtime at the first `retro_run()`, after the host has filled the SRAM) and written
    like every core's SRAM (periodically when it changes, at exit, at a power-off).
  - **Save states, auto state, resume** (the in-game menu's slots with their thumbnails, "Resume where you left
    off?", the boot offer of `resume.ini`, the game switcher): as for any core, in
    `/data/states/retrostone/<name>.state*` (`.state.auto`, its `.png` thumbnail and its `.sram` saves reference),
    found by `host_auto_state_path()` from the entry path. The game does not send the `nostate` status line, so an
    idle power-off during it works as during any other game: the auto state is written first, and the next boot
    offers to resume it (see "Save states" below).
  - Play time, last played, favorites: `gamedb.tsv`, key `retrostone` + `Bomber Mole.bombermole`.
  - The game switcher (Select+Y), screenshots (`/data/screenshots/retrostone/`), the CPU profile, scaling: as for
    any game. Rumble: the games do not use it.

## Save states

The RetroStone VC SDK implements `retro_serialize()` (RetroStone VC `docs/spec.md`, "Save states"): a state is the
whole console at a frame boundary (VRAM, maps, palettes, sprites, the PPU registers and split-screen viewports, the
voices, the echo, the music track and row, the pads' edges, the frame counter and RNG) plus the objects each game
registers, so a game resumes exactly: the same level, positions, enemies, timers, bombs, score, battle round or
2-player race. The format: a 64-byte header (`RSVC`, format version, game id, game version, the game's state
version, a **build hash** over the saved objects' names and sizes and the game's asset pack, the payload size and a
checksum), then tagged sections (`CORE`, `PPU `, `APU `, `MUS `, `TEXT`, `GAME`, `PTRS`, `END `), about 350 KB
(Bomber Mole) or 300 KB (Leady Squid), a fixed size per build. **Versioning:** a state of another game, of another
version or build of the game (any change of its saved objects or assets), truncated or corrupted is refused
before anything changes: the load says "Cannot load the state", the game goes on, and the resume starts the game
fresh. A system update that changes a game therefore drops its states (not its progress). The battery save is
**not** in the state: progress (cleared levels, best times, medals) stays in the `.srm`, and loading an older state
never takes it back (the host's B1 guard has nothing to restore for these cores). The music restarts at the saved
row (the notes held at that moment start again on the next row).

## Controls

The games read a SNES-layout RetroPad on ports 1-4 (`RETRO_DEVICE_JOYPAD`). The RetroStone2's buttons map to the
RetroPad by position (docs/input-design.md §1: right = A, bottom = B, top = X, left = Y, L1/R1 = L/R,
Start/Select), which is the SNES layout the games expect, so there is no remap for `retrostone`:

| RetroStone2 | RetroPad | Bomber Mole | Leady Squid |
|---|---|---|---|
| bottom button (K4, PH11) | B | drop a bomb (facing a hole: toss it down) | swim (any face button) |
| right button (K2, PH0) | A | detonate the oldest bomb (remote power-up) | swim |
| top / left buttons | X / Y | (unused) | swim |
| D-pad / stick | D-pad | move, dig | Up: swim |
| Start | Start | pause menu, confirm | swim (starts a run) |
| Select | Select | back; in the pause menu: resume | pause during a run |
| L1 / R1 | L / R | level select: L+R+Select unlocks every level | |

Leady Squid: player 2 joins with a swim button on pad 2. Players 2-4 are external pads (docs/input-design.md §2),
mapped by position as well. The frontend's hotkeys use Select + a button (Select+Start quits, Select+X the menu,
Select+L/R the save-state slots...). A plain Select reaches the game, and the hotkeys only fire for a button pressed
**while** Select is held: for Bomber Mole's L+R+Select, hold L and R first, then press Select.

## CI

`.github/workflows/images.yml` checks out the public `PaddleStroke/RetroStoneVC` (no token) at `RETROSTONE_VC_REF`
(`main` by default; a tag or commit pins a release) into `.vc-games-src` and passes it to
`scripts/ci/build-board.sh` (`RSOS_CI_VC_GAMES=yes`, `RSOS_CI_VC_GAMES_DIR`), which sets
`BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR`. The configure step prints the commit it builds. See [ci.md](ci.md), section 4.

## Tests

- `make check` (frontend): `check-host` (`rsos-launch-test`, "a game built into its core"): the test core with
  `no_content = true` gets `retro_load_game(NULL)`, its SRAM is `saves/retrostone/Test Game.srm`, its auto state
  `states/retrostone/Test Game.state.auto`, the resume starts from it, and without the key the same file is ordinary
  content; with save states, as any game: no `nostate`, the auto state's thumbnail and `.sram` saves reference, the
  in-game menu's Save state / Load state (slot 1, `Test Game.state1` and its thumbnail), the game switcher leaving
  it (its auto state written). `check-frontend` step 8: the menu lists the system from the read-only folder (no
  `roms/retrostone/`), opens it, launches the entry with the right core and no content, the saves, the auto state
  (thumbnail, saves reference) and the play land in the right places, no idle power-off hold; 8a: the menu finds
  the auto state and resumes it; 8b: an idle power-off during the game writes the auto state first and records the
  entry in `resume.ini`; 8c: the next boot offers "Resume Test VC Game?" and resumes it without content.
- RetroStone VC's own tests (`make check`, `make leadysquid-check`, `make SAN=1 check`): each game saved at many
  points, the same frames replayed after a load, in the same process and in a fresh one, bad states refused.
- `make check-asan`: the same under ASan/UBSan.
- `scripts/ci/check-defconfigs.sh`: `BR2_PACKAGE_RSOS_VC_GAMES=y` (with its source path) and `=n` both survive
  `olddefconfig` for every board.

## Verification (2026-09-30, image `retrostoneos-dev-20260930-vcgames.img`)

- RetroStone VC `97aded521fde` (main HEAD, a clean clone), `retrostone2_defconfig`, Bootlin armv7-eabihf GCC 14.
- Both cores: `ELF 32-bit ARM, EABI5`, `Tag_CPU_name: 7-A`, VFPv4, NEON with FMA, `GNU_STACK RW`, no TEXTREL, the
  25 `retro_*` exported, NEEDED `libc`, `libm`, `ld-linux-armhf` only.
- Loaded by the image's own armhf `rsos-frontend --run` under `qemu-arm` (headless, 300 frames each): "content: none
  (... is a menu entry)", 320x240 at 60 fps, 32 kHz audio, exit 0; the Bomber Mole frame dump shows its title screen.
- `make rsos-vc-games-legal-info`: the four licence texts, no source; `rom-folders` has no `retrostone`.
- Not verified: the games on the device. TODO(hw): play both on the RetroStone2 (frame times, the SRAM after a
  cleared level, Leady Squid for two with an external pad). Save states (since RetroStone VC `savestates`): the
  time of a save and a load of about 350 KB on the SD card, a Bomber Mole battle resumed mid-round after an idle
  power-off, a co-op level and a boss resumed, a Leady Squid race for two resumed, the slots' thumbnails, the music
  after a load.
