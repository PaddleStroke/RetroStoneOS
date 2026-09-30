{{PRERELEASE}}

RetroStoneOS {{VERSION}}: a fast-booting firmware for the **RetroStone2** handheld (8BCraft), plus community ports
to other boards. One SD card image per board.

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
  are already there), BIOS files in `bios/`. `README.txt` on the card explains the layout.
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
- The RetroStone system (Bomber Mole, Leady Squid, [RetroStone VC](https://github.com/PaddleStroke/RetroStoneVC)):
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
