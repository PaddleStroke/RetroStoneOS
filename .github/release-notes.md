{{PRERELEASE}}

RetroStoneOS {{VERSION}}: a fast-booting firmware for the **RetroStone2** handheld (8BCraft), plus community ports
to other boards. One SD card image per board.

38 built-in RetroStone games: Bomber Mole, Beaver Rush, Biscuit Bandit, Blueberry Tumble, Caterpillar Crawl,
Cosmo Corgi, Crab Coconut, Duck Parade, Fishy Feast, Frog Reach, Gecko Golf, Goat Gorge, Gopher Dig, Hamster Havoc,
Hedgehog Hills, Jelly Jam, Kitty Kibble, Ladybug Laps, Leady Squid, Lemur Leap, Llama Drama, Mole Marbles,
Moth Around, Mouse Espresso, Nutty Branch, Otter Rapids, Pancake Tower, Penguin Slalom, Pogo Mamie, Prickle Patch,
Puffin Plunge, Raccoon Recycler, Radish Rascals, Seal Shove, Snail Switch, Snowball Siege, Spider Swing and
Unicorn Hop. Bomber Mole, Goat Gorge, Lemur Leap, Prickle Patch and Puffin Plunge are one-player games; the 33 others
support up to four players.

**New in RetroStoneOS 0.1.5**: **Bluetooth controllers**: Settings > Network > **Bluetooth devices** (with
Bluetooth on) pairs, connects and forgets pads; paired pads reconnect by themselves (issue #13). **Buttons stuck
held down on the RetroStone2** (issue #10): the face buttons and the D-pad are now read every 5 ms like the
shoulder buttons; with interrupts, a release could be lost in the kernel, leaving the button held until its next
press. **Adafruit PiGRRL 2** (issue #6): up/down, L/Select and R/Start were swapped, and the boot logo now fits the
320x240 screen. **Bluetooth extras** (contributed by KurdtNervanna): a connected pad shows its battery level
("connected, 85%"), and a pad that needs a PIN or a USB cable to pair (a PlayStation 3 pad, for example) now says
so. A Bluetooth or USB **keyboard** works in the menus and for the in-game hotkeys; a mouse or trackpad no longer
takes a player port as if it were a gamepad. **Bluetooth headphones (experimental, RetroStone2)**: headphones
can be paired in the same screen and the game sound goes to them; this is untested on real headphones, the sound
arrives a little late (A2DP), and the volume wheel does not change their volume yet. `audio_bluetooth = false` in
the settings keeps the sound on the speaker.

**In RetroStoneOS 0.1.4**: Import from USB and the network upload page recognise every system's folder: the
WonderSwan, Neo Geo Pocket, Atari 7800, Lynx, ColecoVision, MSX, PC Engine CD and Neo Geo CD folders were reported
as "not supported", and imported PC Engine CD games went to the HuCard list where they never showed. The Batocera
folder names `wswan` and `wswanc` work on the card too. **Settings > Game lists > Refresh game lists** now rescans
every folder whatever the caches say. **Raspberry Pi**: the HDMI I2C driver the display waits for is built in,
so HDMI should work with the normal `config.txt` (a Pi 3 needed `vc4-fkms-v3d`; the Pi 2, 4 and 5 had the same
bug), and the **Adafruit PiGRRL 2** is supported, untested, through a block at the end of `config.txt`. On a
**keyboard**, Esc in a game opens the in-game menu and Tab+Space leaves the game. **New systems: Atari 800 and
Atari 5200** (the Atari800 core; folders `atari800` and `atari5200`, no BIOS needed, untested on the device yet).
R2 opens the core's on-screen keyboard (the Atari keys, or the 5200 keypad). **Buttons that seemed held down**
(RetroStone2 and RetroStone1): the buttons' internal pull-ups are on again, closing the in-game menu with A, B or
Start no longer sends that button to the game, a RetroStone2 without the analog stick no longer takes floating
inputs for a held stick, and a lost button release is corrected (and logged) within a second.

**RetroStone2 Pro storage** (contributed by KurdtNervanna): **Settings > Storage** turns on the Pro's 16 GB internal
eMMC and an M.2 SATA SSD (both stay off by default, so every RetroStone2 starts as fast as before), copies
RetroStoneOS to the eMMC (the microSD then stays in the slot as a small "starter card": the A20 cannot start this
eMMC by itself), prepares a drive for games and moves games between drives; the games of every drive show in the
lists. **It needs a card freshly flashed with 0.1.4**: an update over the air keeps the old boot loader, which
cannot start the eMMC. Settings > Storage and System information show the model (RetroStone2 or RetroStone2 Pro).
Also new: **Settings > Game lists > Download game covers** (box art from the libretro thumbnail server, WiFi needed)
and **Group multi-disc games** (one entry per multi-disc game, for the systems whose emulator takes `.m3u`
playlists); **Rename this game** in a game's options; **Settings > Power > Memory speed** (RetroStone2,
**experimental**): a faster memory clock (+12 % or +25 %), tried at the next start and kept only after a memory
test passes; a start that fails goes back to the normal speed by itself.

Game source: [RetroStone VC c900b9839c15](https://github.com/PaddleStroke/RetroStoneVC/tree/c900b9839c15bfc7c75c05fe91ffb6e86c57b89e).

## Downloads

{{BOARDS_TABLE}}

- **tested on hardware**: the RetroStoneOS developers ran this image on the device.
- **community-tested**: the image builds, but the developers have not run it on that hardware. Reports and fixes are
  welcome in the issues ([porting guide]({{REPO_URL}}/blob/{{TAG}}/docs/porting.md)).

Check a download against `SHA256SUMS`:

- Linux/macOS: `sha256sum -c SHA256SUMS --ignore-missing`
- Windows (PowerShell): `Get-FileHash .\retrostoneos-{{VERSION}}-retrostone2.img.xz` and compare with the line in
  `SHA256SUMS`

## Flashing the SD card

Flashing erases the whole card. The card must be **2 GB or more (4 GB or more for the Orange Pi 5)**: the images
are about 1.2 GB (2.2 GB for the Orange Pi 5) once written. 8 GB or more leaves room for games.

- **balenaEtcher** (Windows, macOS, Linux): "Flash from file", pick the `.img.xz` as it is (no need to extract it),
  select the card, "Flash". It verifies the card afterwards.
- **Rufus** (Windows): select the card under "Device", then the `.img.xz` with "SELECT" (pick "All files" in the
  dialog if it is not listed; or extract the `.img` with 7-Zip first), keep "DD image" mode if asked, "START".
- **Raspberry Pi Imager**: "Choose OS" > "Use custom" > the `.img.xz`.
- Linux: `xz -dc retrostoneos-{{VERSION}}-<board>.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress`

## First boot

- The first boot prepares the card: the data partition is grown to fill the card and formatted as exFAT (the
  screen says "Preparing the SD card..."). It takes a few seconds; do not remove the power meanwhile (an
  interrupted first boot resumes at the next one).
- Afterwards the card shows up on a PC as the drive **RETROSTONE**: put your games in `roms/<system>/` (the folders
  are already there), BIOS files in `bios/`. `README.txt` on the card explains the layout. Games copied from a PC
  show up at the next start (or with **Settings > Game lists > Refresh game lists**).
{{HOMEBREW}}
- WiFi, Bluetooth and Ethernet are off until you turn them on in the settings.

## Updating without reflashing

The boards with A/B system slots (the RetroStone2, the RetroStone1 and the Orange Pi boards: those with a `.rsu`
file below) update themselves: **Settings > System update > Check for updates** (network on), or copy the `.rsu`
file of your board to a USB drive (at its root or in a `RetroStoneOS` folder) or to the `update` folder of the
RETROSTONE drive, then plug it in. Games, saves and settings are kept; if the new version does not start, the
console goes back to the previous one by itself. The packages are signed; the console checks the signature before
writing anything. Other boards (Raspberry Pi): flash the new image.

{{UPDATES}}

## Licences

**Free, non-commercial distribution only.** RetroStoneOS is a free download; it is not sold, and it does not come
with the hardware. The images contain emulator cores and theme art whose licences forbid commercial use: Snes9x
(snes9x2005, snes9x2010), PicoDrive, MAME 2003-Plus and FBNeo, and the gbz35 themes, the Carbon system art and the
RetroStone games' art, music and levels (CC BY-NC-SA). So **these images must not be sold, and must not be preloaded on hardware that is sold** (a console, an
SD card or a kit). Sharing them for free, unchanged, is fine.

- The RetroStoneOS code (frontend, build tree, scripts) is under the MIT licence
  ([LICENSE]({{REPO_URL}}/blob/{{TAG}}/LICENSE)).
- The Linux kernel, U-Boot, BusyBox and the other system packages keep their own licences (GPL-2.0 and others).
- The other emulator cores keep their own licences (GPL, zlib, BSD and others).
- The pre-installed games: µCity (GPL-3.0+ code, BSD-2-Clause GBT Player, CC BY-SA 4.0 graphics and music) and
  Freedoom (BSD-3-Clause); their source is in the legal-info archives.
- The RetroStone system (38 games, [RetroStone VC](https://github.com/PaddleStroke/RetroStoneVC)):
  code under MIT, art, music, sound and levels under CC BY-NC-SA 4.0. The names and logos are not licensed.
- Per-package licences: `legal-info/manifest.csv` in the archives below.

### Source code (GPL)

The complete corresponding source of the GPL and LGPL software in these images, with the licence texts, the
Buildroot configuration and the patches, is attached to this release (`make legal-info`, one archive per board;
the NXEngine source there leaves out the Cave Story game data of the upstream tarball, which is not GPL software
and not in the images):

{{LEGAL_FILES}}

The RetroStoneOS sources of this release: {{REPO_URL}}/tree/{{TAG}} (commit {{COMMIT}}).
Parts in several files: `cat <name>.part* > <name>` before extracting.

## Changes

{{CHANGELOG}}

---
Built by GitHub Actions from commit {{COMMIT}} with Buildroot {{BUILDROOT}} on {{DATE}}.
