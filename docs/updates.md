# System updates

RetroStoneOS updates itself from the console, without reflashing the SD card and without touching the games, saves
and settings on the data partition. The new system is written to the **inactive** root filesystem slot (A/B,
docs/build.md "SD card layout"), checked, and only then selected for the next boot; if it does not start, U-Boot goes
back to the previous one by itself.

| | |
|---|---|
| Boards | the RetroStone2 (and, untried, the other boards with the U-Boot A/B boot: the community-tested Orange Pi ports). The Raspberry Pi ports have no A/B boot yet: the console tells about new versions and says to flash the new image |
| Menu | Settings > **System update**; the USB drive dialog ("Install update"); a daily check in the background |
| Program | `/usr/bin/rsos-update` (the menu runs it as a helper process; it is also a UART command) |
| Package | `retrostoneos-<version>-<board>.rsu`, attached to every GitHub release |
| Code | `frontend/src/update/` (engine, CLI, tests), `frontend/src/ui/update_ui.c` (screens), `frontend/src/tools/rsos-mkupdate.c` (build machine), `scripts/ci/make-rsu.sh` |

## 1. For users

**Online**: turn WiFi on (Settings > Network), then Settings > System update > **Check for updates**. When a newer
version exists, its release notes show (scroll with up/down): **A** = Update, **B** = Later. The console downloads the
package (about 90 MB), checks it, installs it (about 2-4 minutes on the RetroStone2: the new system is 512 MB on the
card), checks the installation and asks to **restart** (it restarts by itself after 30 s). At the next start the menu
says "Updated to RetroStoneOS x.y" once.

**Offline**: copy the `.rsu` file of your board (from the release page on GitHub) to

- a USB drive, at its root or in a `RetroStoneOS` folder: plug it in and choose **Install update** in the dialog, or
- the SD card: the `update` folder of the RETROSTONE drive (create it), then Settings > System update > Check for
  updates (without a network connection, only the SD card and the USB drives are searched).

**Every day**, when WiFi is connected and "Check every day (WiFi)" is on (the default), the console checks in the
background (never during a game) and shows a short message once for each new version.

What is kept: everything on the data partition (games, saves, save states, settings, WiFi password, themes). What is
replaced: the system (kernel, drivers, emulators, menu). The update key, the bootloader and the partition layout are
not changed.

**Safety**:

- nothing is written before the package's signature and content are verified;
- the running system is never written: the new one goes to the other slot, and the boot switches to it in one step
  at the very end. A power cut (or a crash, or pulling the USB drive) at any moment before that leaves the console as
  it was; after that, it starts the new version;
- a new version that does not reach a stable menu three times in a row (it crashes, hangs, or loses power) is
  abandoned: U-Boot starts the previous version again, and the menu says "The last update didn't start; the previous
  version was restored";
- the console refuses to update with the battery under 30 % and no charger;
- only versions newer than the installed one are offered; an older package is refused (see "Downgrades").

**Recovery** when nothing starts any more (which the two slots should prevent): flash the image of the release to the
SD card. **Flashing erases the whole card**: copy `saves/`, `states/` and anything else you care about from the
RETROSTONE drive to a PC first (Settings > Storage > Back up saves also copies them to a USB drive).

## 2. The package format (`.rsu`)

A plain **ustar archive** (7-Zip or `tar tf` lists it) with exactly these members, in this order:

| Member | Content |
|---|---|
| `manifest` | UTF-8 text, `key = value` lines (below) |
| `manifest.sig` | the [signify](https://man.openbsd.org/signify) signature of `manifest` (absent in an unsigned package) |
| `rootfs.ext4.zst` | the payload: the new root filesystem image (`output/images/rootfs.ext4`, the whole slot content: `/boot` has the kernel, the device tree, `boot.scr` and the overlays), compressed with zstd |

The signature covers the manifest, and the manifest holds the size and SHA-256 of the payload *and* of the
uncompressed image, so one signature protects every byte. Anyone can check a package with OpenBSD's tool:

```sh
tar xf retrostoneos-0.2.0-retrostone2.rsu manifest manifest.sig
signify -V -p update.pub -x manifest.sig -m manifest      # Ubuntu: signify-openbsd
tar xf retrostoneos-0.2.0-retrostone2.rsu rootfs.ext4.zst && sha256sum rootfs.ext4.zst   # = payload_sha256
```

Manifest (`rsu_manifest_parse()`, strict: every required key once, safe values; unknown keys are ignored so that a
later version can add some without breaking older updaters):

| Key | Example | Meaning |
|---|---|---|
| `format` | `1` | manifest format; a newer one than the updater knows is refused |
| `board` | `retrostone2` | board id (below); must equal the console's |
| `version` | `0.2.0` | the OS version (`/etc/rsos/version.env` of the image) |
| `variant` | `release` / `dev` | the build variant |
| `build_date`, `build_time` | `2026-10-01`, `1790800000` | when it was built (`build_time` orders two builds of one version) |
| `min_updater` | `1` | the oldest `rsos-update` that can install it (`RSU_UPDATER_VERSION`) |
| `bootloader_min` | `1` | the bootloader level it needs (see "U-Boot") |
| `payload`, `compression` | `rootfs.ext4.zst`, `zstd` | the payload member (`none`: an uncompressed `rootfs.ext4`) |
| `payload_size`, `payload_sha256` | | the compressed payload |
| `image_size`, `image_sha256` | `536870912` | the image as written to the slot (a multiple of 4096) |
| `changelog` | `- faster menus\n- ...` | an excerpt shown on the console when the package comes from a USB drive or the SD card (`\n` `\t` `\\` escapes) |

**Board id**: the defconfig name without `_defconfig` and `_release`, `_` -> `-` (`retrostone2`, `orangepi-h3-pc`,
`rpi4-64`), the `<image>` part of the CI file names. It is written to `/etc/rsos/version.env` (`RSOS_BOARD_ID`) by
the rsos-frontend package from `BR2_DEFCONFIG`.

**Compression: zstd -19**, measured on the RetroStone2 root filesystem (512 MiB image, 214 MiB used), host
Ryzen 9800X3D:

| | size | decompression (host) | on the Cortex-A7 (est. x20-25) |
|---|---|---|---|
| xz -9e | 73.7 MB | 3.8 s | ~80-95 s |
| zstd -19 | 90.7 MB | 0.48 s | ~10-12 s |
| zstd --ultra -22 --long=27 | 84.3 MB | 0.60 s | ~12-15 s, but a 128 MiB decoder window |

The SD card writes the 512 MiB at 10-20 MB/s (30-50 s): zstd's decoding hides behind the writes, xz's would double
the install time. The 17 MB more to download cost 5-10 s on the AP6210 WiFi. The device limits the decoder window to
32 MiB (`ZSTD_d_windowLogMax` 25; `-19` uses 8 MiB). The device decodes with the Buildroot `zstd` library.

**Hashes**: SHA-256 (`src/update/sha256.c`, our own, checked against the NIST vectors and `sha256sum`), the fastest
of the usual ones on a 32-bit CPU. **Signatures**: Ed25519 from [Monocypher](https://monocypher.org) 4.0.2 (vendored in
`frontend/third_party/monocypher/`, CC0 or BSD-2-Clause, audited by Cure53 in 2020), with the signify key and
signature formats; no OpenSSL.

## 3. Keys and signing

| File | Where | |
|---|---|---|
| public key | `frontend/assets/update.pub` in the repository, installed as `/usr/share/rsos/update.pub` | public |
| **private key** | `update-signing.key` in a private folder **outside the repository** (for example `C:\path\to\RetroStoneOS-keys\`), and the GitHub secret `UPDATE_SIGNING_KEY` | **never committed** |

Both are signify keys ("untrusted comment: ..." + one base64 line). The private key has **no passphrase** (the CI
cannot type one); protect the file instead. It was made on 2026-09-27 with
`rsos-mkupdate keygen -s update-signing.key -p update-signing.pub`.

**For the owner** (once):

1. **Back up** `update-signing.key` somewhere safe and offline (a password manager, an encrypted USB drive). Losing
   it means the consoles in the field cannot be updated any more (they only accept this key): a new key can only
   reach them through a reflash, or through an update signed with the old key.
2. Add it to GitHub: `PaddleStroke/RetroStoneOS` > Settings > Secrets and variables > Actions > New repository
   secret: name **`UPDATE_SIGNING_KEY`**, value: the whole content of `update-signing.key` (both lines).
3. Anyone who has the file can publish an update every console installs: never mail it, never put it in the
   repository or in an issue. If it leaks: make a new key, put its public key in `frontend/assets/update.pub`, and
   publish one last update signed with the **old** key that brings the new public key (then revoke the secret).

**Policy** (the updater enforces it, `rsu_policy()`):

- **Release builds** (`BR2_RETROSTONE_RELEASE`, `RSOS_VARIANT=release`) install only packages signed with their
  key. Unsigned, or signed with another key, or modified: refused.
- **Development builds** install signed packages the same way. An **unsigned** package is accepted only from a local
  file (USB drive, SD card, UART) and only after an explicit confirmation ("This update is not signed. Install it only
  if you built it yourself." / `rsos-update --allow-unsigned apply FILE`). The online check never offers one. A
  package signed with *another* key is always refused.
- A fork uses its own key: `rsos-mkupdate keygen`, its public key in `frontend/assets/update.pub` (or
  `RSOS_UPDATE_PUBKEY=<file>` for the frontend build), its secret in its own repository.

Signing by hand (a package built locally, or an unsigned CI package):

```sh
make -C frontend mkupdate                      # build-machine tool: $(BUILDDIR)/build-tools/rsos-mkupdate
rsos-mkupdate sign -s update-signing.key -o signed.rsu retrostoneos-0.2.0-retrostone2-unsigned.rsu
rsos-mkupdate verify -p frontend/assets/update.pub signed.rsu
rsos-mkupdate info signed.rsu
```

## 4. On the console: `rsos-update`

```
rsos-update info                     version, board, in-place updates or why not, slot, key, HTTPS
rsos-update check [--local-only | --net-only] [--dir DIR]
rsos-update apply FILE               install a package (SD card, USB drive)
rsos-update apply auto               download and install the newest version found
rsos-update verify [--full] FILE     check a package (signature, payload; --full: decompress and check the image)
rsos-update boot                     after a restart: "updated", clean-up once confirmed
rsos-update status                   the boot environment (rsos_*) and the installed update
options: --allow-unsigned (development builds), --force (same or older version), --prerelease,
         --ignore-battery, --machine (lines for the menu), --log FILE
```

Every run is logged to `rsos/logs/update.log` on the RETROSTONE drive (`/data/rsos/logs/update.log`, kept across the
restart; `update.log.old` beyond 256 KiB), or `/run/rsos/update.log` when `/data/rsos/logs` does not exist.

### 4.1 Finding updates

- **Online** (`check`): `GET https://api.github.com/repos/PaddleStroke/RetroStoneOS/releases?per_page=10`
  (unauthenticated: 60 requests per hour per address, plenty for a daily check), the newest non-draft release
  (pre-releases only with `--prerelease`) that has an asset `retrostoneos-*-<board>.rsu` (or, on a board without A/B,
  `retrostoneos-*-<board>.img.xz`). If it is newer than the running version, the first 64 KiB of the package are
  fetched with a Range request: the manifest and its signature are checked (board, bootloader, format) *before*
  offering it, and the release notes (Markdown, cleaned) are shown. Unsigned packages are never offered online.
- **Local**: every `*.rsu` in `/data/update/`, `/data/rsos/update/` (an interrupted download), `/media/<drive>/` and
  `/media/<drive>/RetroStoneOS/`. The newest installable one wins; without one, the most useful refusal is reported
  ("needs a newer bootloader", "for another console", "already installed"...).
- **Versions** (`rsu_version_cmp()`): the numbers first (`0.2.10` > `0.2.9`, a leading `v` is ignored, missing parts
  are 0), then a release before its pre-releases and development builds (`0.2` > `0.2-rc2` > `0.2-rc1` >
  `0.2-dev`), then the build time. The running version is `RSOS_VERSION` of `/etc/rsos/version.env`
  (`0.1-dev` for a development build, the tag's version for a CI release).
- **Downgrades** and reinstalling the same version are refused, except `rsos-update --force apply FILE` on the UART.

### 4.2 HTTPS

mbedTLS 3.6 (Buildroot `mbedtls`) with the Mozilla CA bundle (Buildroot `ca-certificates`,
`/etc/ssl/certs/ca-certificates.crt`); the certificate chain and the host name are checked
(`MBEDTLS_SSL_VERIFY_REQUIRED`), plain `http://` and redirects from https to http are refused. Our own small HTTP/1.1
client (`src/update/http.c`): redirects (GitHub sends the download to `release-assets.githubusercontent.com`),
`Content-Length` or chunked bodies, Range requests, 30 s timeouts, cancellation. BusyBox `wget` was not used: its
TLS does not verify certificates. mbedTLS rather than BearSSL: BearSSL has had no release since 2018 and does
only TLS 1.2; mbedTLS 3.6 is an LTS branch with security fixes, and does TLS 1.3.

**Size** in the RetroStone2 root filesystem (measured): `rsos-update` 150 KB, mbedTLS 0.83 MB, libzstd 0.58 MB
(+ the `zstd` programs 0.2 MB, installed by the Buildroot package), the CA bundle 0.22 MB (+ the individual
certificates 0.22 MB): about 2.2 MB of the 512 MiB slot. `rsos-frontend` links none of them (the menu starts as
fast as before); only `rsos-update` does, and it only runs when an update is checked or installed.

**The clock**: certificates are only valid between two dates. The console's clock comes from `rsos-clock` (saved at
shutdown, never earlier than the build) and from NTP once WiFi connects (docs/power.md §11). A clock before the build
date, or a certificate refused as "expired" or "not yet valid", gives "The clock is not set. Connect to WiFi and wait a
minute, or set the date in Settings > Date & time."

### 4.3 Downloading

To `/data/rsos/update/<name>.part`, with `<name>.part.info` (name and size): a free-space check first (size + 16 MB),
then the download, retried 3 times on a network error, each time **resumed** with a Range request from what is on the
card (also after a power cut or a new attempt later). A server that ignores Range restarts from the beginning; a file
of another size on the server (a rebuilt release) is refused. Complete: fsync, renamed to `<name>`. The downloaded
package stays there until the new system is confirmed, then it is deleted (a package the user copied is never
deleted).

### 4.4 Installing (`upd_apply_file()`)

1. one `rsos-update` at a time (`flock /run/rsos/update.lock`);
2. the manifest's signature (policy above), board, bootloader, version; the battery (>= 30 % or a charger);
3. the slots: the running slot is `rsos.slot=` of the kernel command line, its partition `root=`
   (`/dev/mmcblk0p2` for slot a, `p3` for b; any `<disk>[p]N`); the target is the other one. The boot environment must
   select the running slot and it must be **confirmed** (`rsos_ok=1`): a system on trial (just updated, not yet
   confirmed by `rsos-boot-ok`) is the only one that boots, and its fallback must not be overwritten ("The system is
   still starting up. Try again in a minute."; an installed update not yet started: "restart first");
4. **pass 1**: the payload's SHA-256 against the signed manifest, before anything is written;
5. **pass 2**: the target opened with `O_EXCL` (fails if it is mounted); its first 64 KiB are zeroed and synced
   first, so an interrupted write never leaves a slot with a valid superblock (U-Boot's fallback checks
   `test -e mmc 0:<part> /boot/zImage`, which fails: it never falls back to a half-written slot); then the payload is
   decompressed and written from 64 KiB on, hashing both the payload (again: a USB drive swapped meanwhile is caught)
   and the image, 8 MiB at a time written back (`sync_file_range`) and dropped from the page cache (the menu's
   cached files stay); the first 64 KiB last, then `fdatasync`;
6. **pass 3**: the slot is read back from the card (`O_DIRECT`) and its SHA-256 compared with the manifest;
7. **the switch**: one `fw_setenv -s` with `rsos_slot=<new> rsos_ok=0 rsos_tries=3 rsos_fails=0 rsos_fallback=`
   (the "Updater contract" of docs/build.md; the environment is redundant: a power cut during that write leaves
   either the old or the new state), read back with `fw_printenv`;
8. `/data/rsos/update-state.ini` (version, slots, the downloaded file, `announced`) for after the restart.

The running slot and `/data` are never opened for writing. Cancelling (the menu's STOP, `SIGTERM`, `Ctrl-C`) is
possible until step 7.

### 4.5 After the restart

U-Boot boots the new slot on trial (3 tries, docs/build.md "A/B slots"). `rsos-boot-ok` confirms it once the menu has
been stable for 30 s after the network settings were applied. The menu, once its game lists are loaded, runs
`rsos-update boot` (only when `update-state.ini` exists; again every 30 s until the confirmation, at most 10 minutes):
`updated` the first time ("Updated to RetroStoneOS x.y"), `confirmed` once `rsos-boot-ok` confirmed the slot (the
downloaded package and the state file are deleted), `failed` when U-Boot fell back (the existing boot note says so;
the download is deleted: it would fail again).

### 4.6 U-Boot is not updated

U-Boot lives in the raw area of the card (8 KiB - 1 MiB) in a single copy: rewriting it is the one step a power cut
could turn into a console that needs a PC to reflash. So `.rsu` packages never contain it. A future release whose
`boot.cmd` needs a newer U-Boot (a new command, another environment layout) sets `bootloader_min` (the
`RSOS_RSU_BOOTLOADER_MIN` of `make-rsu.sh`) higher than the level of the consoles in the field, which is the
`rsos_bootloader` environment variable (absent = 1). Such a U-Boot puts `rsos_bootloader=2` in its default
environment (a `uboot.fragment`/patch change); consoles with the old one refuse the package: "This update needs a
newer bootloader: flash the new image to the SD card instead".

### 4.7 Boards

`board.ini` key `ab_update` (docs/porting.md): `auto` (default: in-place updates when `/etc/fw_env.config` exists and
the kernel was started by the A/B boot script, `rsos.slot=` on its command line), `1`, or `0`. The RetroStone2 says
`1`. On the Raspberry Pi ports (no U-Boot, the firmware's `cmdline.txt`) it is off: Settings > System update still
checks and shows new versions, with "Updates: flash the new image to the SD card".

### 4.8 The menu protocol (`--machine`)

One line per event, `type<TAB>key=value<TAB>...`, `\\` `\t` `\n` escaped in values:

| Line | Fields |
|---|---|
| `info` | `version board variant build_date ab reason slot key bootloader tls` |
| `local`, `net`, `best` | `status` (`found`/`refused`/`none`/`uptodate`/`error`), `source version size where name page signed installable verdict notes`; `net` errors: `code msg detail` |
| `state`, `progress` | `phase` (`download verify write readback switch`), `done`, `total` (bytes) |
| `done` | `version slot` |
| `error` | `code` (`rsu_err_code()`: `badsig`, `battery`, `clock`...), `msg`, `detail` |
| `boot` | `event` (`none updated updated-waiting pending confirmed failed`), `version` |
| `verified` | `version board signed compare` |

The menu (`ui/update_ui.c`) translates the codes; the helper's English text goes to the log only. The helper runs in
its own session: an install goes on if the menu restarts.

## 5. Releases (CI)

`scripts/ci/build-board.sh <board> package` runs `scripts/ci/make-rsu.sh` after compressing the image. For a board
whose image has `/etc/fw_env.config` it makes `retrostoneos-<version>-<image>.rsu` (+ `.sha256`):
`zstd -19` of `images/rootfs.ext4` (checked by decompressing it again), the manifest from the image's
`/etc/rsos/version.env`, the changelog excerpt (the commit subjects since the previous tag, at most 40), and the
signature with the `UPDATE_SIGNING_KEY` secret, whose public half must equal the image's `update.pub` (else the job
fails). Without the secret the package is **`<...>-unsigned.rsu`**: release images refuse it and the online check never
looks at it (it only looks for `-<board>.rsu`), but a developer can install it on a development image from a USB
drive. `images.yml` uploads the packages with the images; the release job attaches them to the release (and to
`SHA256SUMS`), and the release notes list them.

For a tag `v0.2.0` the images report `0.2.0` (`RSOS_CI_OS_VERSION` -> `BR2_RETROSTONE_VERSION`), and
`make-rsu.sh` checks that the package says the same. A manual run (workflow_dispatch) builds packages too (named after
the branch and commit; their version is the one of `rsos-frontend.mk`, `0.1` + `-dev` for development images).

Locally (WSL), after a build:

```sh
UPDATE_SIGNING_KEY="$(cat /mnt/c/path/to/RetroStoneOS-keys/update-signing.key)" \
  RSOS_CI_WORK=~/rsos/ci-test sh scripts/ci/make-rsu.sh ~/rsos/output ~/rsos/ci-test/artifacts retrostoneos-test-retrostone2
```

## 6. Tests

| Test | What |
|---|---|
| `make check-update` (in `make check`) | `test_update` (146 checks): SHA-256 vectors, base64, signify keys and signatures (good, modified, another key, a key with the same number), the manifest parser, version order, the tar container, the policy (bad signature, tampered manifest, wrong board even forced, older, same, forced, unsigned on release and development builds, bootloader, updater), JSON, URLs, slot selection from the command line (mmcblk, sda, nvme, mismatch, `PARTUUID=`, on trial, restart pending, no environment), installs into a file slot with fake `fw_printenv`/`fw_setenv` (content, environment, one `fw_setenv` call, state file, battery, tampered payload, bad signature, wrong board, downgrade, forced downgrade, unsigned on development builds, an interrupted write: environment untouched and no superblock, then the retry, a slot too small), the after-restart events, downloads from a local HTTP server (a dropped connection resumed with Range, a process killed halfway resumed by the next run, a server without Range, a changed file, no space, 404, plain http refused), the GitHub check (chunked JSON, redirect, only the package head fetched, pre-releases, up to date, a board without A/B, the clock, an unsigned package online, no server) |
| | `cli_test.sh` (35 checks): `rsos-mkupdate` keygen/pack/sign/verify/info, **interoperability with OpenBSD signify** (it verifies our signatures; its keys sign our packages; identical signatures), `rsos-update` `--machine` output for info/check/verify/apply/boot/status on a fake system |
| | `check-update-ui`: the screens with the preview tool and `tests/fake-rsos-update.sh`: check, notes, install, restart dialog, up to date, a failed install, a board without A/B, "Updated to" after a restart, the USB dialog with and without a package |
| `board/retrostone2/tests/update-ab-test.sh` (root, WSL, after a build; run by the images workflow) | the real image on a loop device (22 checks): a bad signature, a power cut in the middle of the write, the install (the environment read with the host `fw_printenv` on the card, slot b mounted and compared byte for byte, slot a and the data partition unchanged), the restart and the confirmation; then **U-Boot in QEMU** (the cubieboard build of `boot-ab-qemu-test.sh`) boots the updated slot on trial, and after an interrupted install still boots slot a |
| live | `rsos-update check` against `api.github.com` from WSL: TLS and the certificate chain; a wrong CA bundle and a name mismatch (by IP) are refused |

## 7. Limits and follow-ups

- U-Boot is never updated (4.6).
- The whole 512 MiB slot is written and read back even though ~220 MiB are used (2-4 minutes on the RetroStone2,
  TODO(hw): measure). Writing only the used ext4 blocks would halve it, at the cost of a more complex format.
- No delta updates: every package is the full system (about 90 MB).
- The Raspberry Pi ports need their own A/B mechanism (`autoboot.txt` + `tryboot`) before they can use packages.
- The GitHub API is unauthenticated (60 requests/hour per address): fine for consoles, not for a classroom behind
  one address checking every minute.
- TODO(hw): the whole flow on a RetroStone2 (download speed, install time, the restart, the confirmation).
