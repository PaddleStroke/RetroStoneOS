# Building and flashing RetroStoneOS

RetroStoneOS is built with Buildroot 2026.02.3 and the BR2_EXTERNAL tree in
`buildroot-external/` (name `RETROSTONE`, defconfig `retrostone2_defconfig`).
The result is one SD card image, `sdcard.img`.

## Build host

Windows 11 + WSL2 `Ubuntu-24.04`. Sources stay in this repository
(`/mnt/c/path/to/RetroStoneOS` from WSL); all build output lives in
the WSL ext4 filesystem under `~/rsos/` (never under `/mnt/c`, it is far too slow):

| Path | Contents |
|---|---|
| `~/rsos/buildroot-2026.02.3/` | Buildroot source |
| `~/rsos/output/` | Buildroot output (`O=`) |
| `~/rsos/dl/` | download cache (`BR2_DL_DIR`, set in the defconfig as `$(HOME)/rsos/dl`) |
| `~/rsos/build.log` | log of the last full build |

The ccache cache is in `~/.buildroot-ccache`. The toolchain is the external
Bootlin armv7-eabihf glibc "stable" 2025.08-1 (GCC 14, C++, kernel headers 5.4),
so no compiler is built.

Ubuntu packages needed (already installed): `build-essential bc cpio file git
libncurses-dev libssl-dev python3 rsync unzip wget`, and `python3-pil` for the
RetroStone VC games (the rsos-vc-games asset step, docs/vc-games.md). The
games also need a RetroStone VC checkout next to this repository
(`../RetroStoneVC`); without one, set `BR2_PACKAGE_RSOS_VC_GAMES=n`.

### First-time setup (inside WSL)

```sh
mkdir -p ~/rsos/dl && cd ~/rsos
wget https://buildroot.org/downloads/buildroot-2026.02.3.tar.xz
echo "5a59e7501b0b4ec52c41f4bfa79412320e0b37eae5f719605a258e8d0c6fc7fb  buildroot-2026.02.3.tar.xz" | sha256sum -c
tar xf buildroot-2026.02.3.tar.xz
```

The sha256 is the one of the tarball these images were built with. When moving to another Buildroot release, check
it against the PGP-signed `buildroot-<version>.tar.xz.sign` next to the tarball (it lists the sha256; `gpg --verify`
with the Buildroot release key) before writing it here.

Downloads of the packages are checked too: every package has a `.hash` file, the custom Linux (6.18.54) and U-Boot
(2026.07) versions in `board/retrostone2/patches/{linux/linux.hash,uboot/uboot.hash}` (Buildroot also reads hash
files from `BR2_GLOBAL_PATCH_DIR`), and `BR2_DOWNLOAD_FORCE_CHECK_HASHES=y` makes a download that a hash file does
not list an error instead of a warning. When bumping the kernel or U-Boot version, update the hash file (from
kernel.org's signed `sha256sums.asc`, U-Boot's `.sig`).

## Building

**Windows' PATH must not leak into the build**: WSL appends the Windows PATH,
whose entries contain spaces, and Buildroot refuses to run with it. Always
reset PATH first, as below.

### From inside WSL

```sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
cd ~/rsos/buildroot-2026.02.3
make O=~/rsos/output BR2_EXTERNAL=/mnt/c/path/to/RetroStoneOS/buildroot-external retrostone2_defconfig
nice -n 10 make O=~/rsos/output -j16 2>&1 | tee ~/rsos/build.log
```

The `retrostone2_defconfig` step is only needed once (or after editing the
defconfig); Buildroot remembers the BR2_EXTERNAL path in `~/rsos/output`.
The first full build of the base system (no frontend, no cores) took about
30 minutes on the 16-core host, downloads included. The result is
`~/rsos/output/images/sdcard.img`.

### From Windows (PowerShell or cmd)

A process started by `wsl.exe` dies with the `wsl.exe` call unless it is
detached with `setsid -f`. Start the build detached and follow the log:

```
wsl.exe -d Ubuntu-24.04 -- bash -c "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; cd ~/rsos/buildroot-2026.02.3 && make O=~/rsos/output BR2_EXTERNAL=/mnt/c/path/to/RetroStoneOS/buildroot-external retrostone2_defconfig && setsid -f bash -c 'nice -n 10 make O=~/rsos/output -j16 > ~/rsos/build.log 2>&1; echo \$? > ~/rsos/build.exit' < /dev/null > /dev/null 2>&1"

wsl.exe -d Ubuntu-24.04 -- tail -f /root/rsos/build.log
```

`~/rsos/build.exit` holds make's exit status (0 = success) once it is done.

### Rebuilding one part

Buildroot does not notice edits in the external tree by itself. From
`~/rsos/buildroot-2026.02.3`, with PATH reset as above:

| After editing | Run |
|---|---|
| the DTS or an overlay (`board/retrostone2/dts/`) | `make O=~/rsos/output linux-rebuild all` |
| `linux.fragment`, `linux-patches.fragment`, a kernel patch | `make O=~/rsos/output linux-dirclean all` |
| `uboot.fragment`, a U-Boot patch (`board/retrostone2/patches/uboot/`) | `make O=~/rsos/output uboot-dirclean all` |
| `board/retrostone2/tools/rsos-cntvct.c` | `make O=~/rsos/output all` (built by `post-build.sh`) |
| `boot.cmd`, `rootfs-overlay/`, `post-build.sh`, `genimage.cfg`, anything in `board/common/` | `make O=~/rsos/output all` |
| `board/common/busybox.fragment` | `make O=~/rsos/output busybox-dirclean all` |
| a libretro core's `.mk` or `package/libretro-common.mk` | `make O=~/rsos/output libretro-<name>-dirclean all` |
| the defconfig | re-run the `retrostone2_defconfig` step, then `make ... all` |
| `frontend/` (the menu, `rsos-update`: docs/updates.md) | `make O=~/rsos/output rsos-frontend-rebuild all` |
| the RetroStone VC checkout (the games, docs/vc-games.md) | `make O=~/rsos/output rsos-vc-games-rebuild all` |
| a package's `Config.in` (e.g. the `select`s of rsos-frontend) | `make O=~/rsos/output olddefconfig`, then `make ... all` |

The tree is split between `board/common/` (init scripts, data partition,
networking, boot logger, the shared post-build and data seed, the BusyBox and
generic kernel fragments) and `board/retrostone2/` (DTS, U-Boot, kernel
fragments and patches, genimage, `board.ini`, the RetroStone2 tools): see
docs/porting.md. The same tree builds the community-tested Raspberry Pi 4 port
(`rpi4_64_defconfig`) in its own output directory:

```sh
make O=~/rsos/output-rpi4 BR2_EXTERNAL=/mnt/c/path/to/RetroStoneOS/buildroot-external rpi4_64_defconfig
nice -n 10 make O=~/rsos/output-rpi4 2>&1 | tee ~/rsos/build-rpi4.log
```

`make O=~/rsos/output linux-menuconfig` / `uboot-menuconfig` are handy to explore
options; copy what you keep into the fragments.

### Getting the image to Windows

The image is reachable from Windows Explorer at
`\\wsl.localhost\Ubuntu-24.04\root\rsos\output\images\sdcard.img`, or copy it:

```
wsl.exe -d Ubuntu-24.04 -- cp /root/rsos/output/images/sdcard.img /mnt/c/path/to/
```

## Flashing the SD card (Windows)

`dd` from WSL is not practical (WSL2 cannot see USB card readers without
usbipd). Use a Windows tool:

- **balenaEtcher**: "Flash from file" -> `sdcard.img` -> select the card -> Flash.
  It verifies the write.
- **Rufus**: pick the card under "Device", select `sdcard.img` (choose "All files"
  in the dialog), keep "DD image" mode if asked, Start.
- Win32 Disk Imager works too.

Any card size >= 2 GB works (the image is 1.16 GB, mostly empty space that
compresses well); the first boot grows the data partition to fill the card.
Flashing overwrites the whole card, including the data partition.

## SD card layout (A/B)

| Disk offset | Size | MBR entry | Content |
|---|---|---|---|
| 0 | | | MBR partition table |
| 8 KiB | < 1016 KiB | - | `u-boot-sunxi-with-spl.bin` (SPL + U-Boot; the A20 boot ROM loads it from 8 KiB) |
| 1 MiB | 2 x 64 KiB | - | U-Boot environment, two redundant copies (A/B boot state). 1-4 MiB reserved |
| 4 MiB | 512 MiB | **2** | rootfs **A**: ext4, label `rsos-root`, mounted **read-only** at `/`. Holds `/boot/zImage`, `/boot/sun7i-a20-retrostone2.dtb`, `/boot/boot.scr`, `/boot/overlays/*.dtbo` |
| 516 MiB | 512 MiB | **3** | rootfs **B**: same layout; empty in the factory image, written by the system updater (docs/updates.md) |
| 1028 MiB | 128 MiB in the image, then the rest of the card | **1** | **data**, label `RETROSTONE`, mounted at `/data`: a FAT32 seed (type 0x0c) in the image, exFAT (type 0x07) after the first boot |

The data partition is the first MBR entry, although it is physically last, so
that a PC shows it as soon as the card is plugged in (Windows before 10 1703
only mounts the first entry of removable media). In the image it is a 128 MiB
**FAT32 seed** built by `post-image.sh` without mounting anything (`mkfs.vfat`
+ `mmd`/`mcopy` from host dosfstools and mtools): the folder layout and
`README.txt` (CRLF, from `frontend/src/transfer/data-README.txt`), so the card
is a ready RETROSTONE drive right after flashing. List it with
`mdir -i ~/rsos/output/images/data.vfat ::` (and `::roms`).

Each slot is 512 MiB (384 MiB until 2026-09-27, enlarged before the first release). The root filesystem with the 25 libretro cores, the
frontend tools and the homebrew games uses about 214 MiB (44% free).

`/data` (readable from any PC, the card shows up as `RETROSTONE`):

```
README.txt
roms/<system>/   one folder per system (see below)
bios/  saves/  states/  screenshots/  themes/
rsos/settings.ini          system settings (key=value)
rsos/wpa_supplicant.conf   WiFi credentials (optional)
rsos/bluetooth.img         Bluetooth pairings (ext4 image, see Networking)
```

### First boot

`/usr/libexec/rsos/data-partition` (run by `/etc/init.d/rcS` on every boot)
mounts `/dev/mmcblk0p1` on `/data` (exFAT, else vfat, `noatime`). On the first
boot, when entry 1 does not reach the end of the card yet:

- **Seed without user files** (the usual case). PC junk does not count: a card
  that was only looked at on Windows or a Mac still qualifies (`README.txt` at
  the root, `System Volume Information/`, `$RECYCLE.BIN/`, `RECYCLER/`,
  `.Spotlight-V100/`, `.fseventsd/`, `.Trashes/`, `.TemporaryItems/`, `._*`,
  `.DS_Store`, `Thumbs.db`, `desktop.ini`, `IndexerVolumeGuid`,
  `WPSettings.dat`, case-insensitive, any depth; the same list as
  `tr_is_junk()` in the frontend). The entry is grown to the end of the card,
  set to type 0x07 and formatted with `mkfs.exfat -L RETROSTONE`, and the
  layout and `README.txt` are recreated.
- **Seed with user files** (copied from a PC before the first boot): the files
  are converted without risk of loss, as described in docs/rom-transfer.md 1.3.
  They are tarred to the raw free space at the far end of the card (not in any
  partition yet) and the backup is read back and verified (file count, CRC);
  then the same steps as above, and the files are restored.
- **Card too small for the backup** (e.g. 1 GB with a full seed), or the backup
  step fails: the FAT32 seed is kept as it is, and only its entry is grown
  (type 0x0c) so that this runs once. TODO: the frontend's "Storage > Expand
  data partition".

**Power cuts** (system review S1, S2). Every step can be interrupted:

1. (seed with files) the backup at the end of the card, verified;
2. a marker in the last MiB of the card: `RSOSCV02 <id> <backup offset MiB>
   <backup size MiB> <crc>` (size 0: nothing to restore), where `<id>` is a new
   random MBR disk identifier;
3. **one** write of the MBR (sfdisk dump, edited, written back): the new disk
   identifier and the data entry grown to the end of the card, type 0x07; then
   `partx --update` (the table cannot be re-read while a root filesystem is
   mounted);
4. `mkfs.exfat`, mount, restore, layout, `sync`;
5. the stamp `rsos/.conversion-done` (the marker's `<id>`) on the new volume,
   `sync`, then the marker is cleared and read back.

Every boot reads the marker first. It is acted on only when its `<id>` is the
card's current MBR disk identifier and the entry already reaches the end of the
card, i.e. when step 3 happened on this card since it was last flashed: steps 4
and 5 are then redone from the start (format again, restore the verified backup
again), which is idempotent, unless the volume already carries the stamp of
that `<id>`: the conversion had finished and only the marker was left (a cut,
or a marker that could not be cleared), so it is just cleared and nothing
written since is lost. While a valid marker stays (`partx` or `mkfs.exfat`
failed: the next boot finishes the conversion from the backup), `/data` is
mounted **read-only** (`/run/rsos/data-readonly`; a tmpfs and
`/run/rsos/data-error` if even that fails): what the menu would write there
would be lost. Any other marker is stale and is discarded: a cut
before step 3 leaves the seed untouched, and re-flashing the card rewrites the
MBR (the image's disk identifier is 0), so a backup from before a re-flash is
never restored over the new seed. `mkfs.exfat` never runs unless the MBR
carries the marker's identifier and the kernel sees the new size. A backup that
fails its CRC (or whose restore fails) is left in place and its marker becomes
`RSOSBKBAD`, so it is not retried at every boot. A type 0x07 entry that still
holds the FAT32 seed (a cut between the MBR write and `mkfs.exfat` under an
image older than this scheme) is converted again, with a backup if it holds
files. Markers of older images (`RSOSBK01`) are ignored: a new image only
arrives by re-flashing, which makes them stale.

**Dirty volume** (S4): an exFAT volume whose VolumeDirty flag is set (the last
session did not unmount it) is checked with `fsck.exfat -p` before it is
mounted, with the "Checking the SD card" splash; the result goes to
`/run/rsos/data-fsck` (docs/power.md §11 has the timing). **A volume that does
not mount** is checked if it is exFAT (`fsck.exfat -p`, then `-y`). **Nothing
is ever formatted without the user's consent** (outside the first boot of the
pristine seed and the resume of its conversion): a partition that still does
not mount, holds another filesystem or cannot be read is left alone; a small
tmpfs goes on `/data` so that the menu starts, and `/run/rsos/data-problem`
says why: `unmountable <exfat|vfat|unknown>`, `foreign <ntfs|fat16|fat12|ext4>`
or `readerror`. The menu then offers to back the card up on a PC first, or to
format it: after the user confirmed, it runs
`/usr/libexec/rsos/data-partition --format-confirmed` (refused without
`data-problem`), which makes an empty full-size exFAT through the same
crash-safe steps on a mount point of its own (the menu keeps its tmpfs
`/data`; nothing is written there), logs to stderr and the kernel log, and
exits 0 only when the format and the layout succeeded; the menu then
restarts the console.

Then, if `roms/` is missing, it creates the layout, and it runs
`/usr/bin/rsos-seed-homebrew /data`, which copies the bundled homebrew games
once (it keeps its own marker and never overwrites a file). On a normal boot
the script costs two small reads (the marker and the exFAT flags) and the mount.

**A first boot never hangs on its splash, and always leaves a trace** (image
`20260928-firstboot`; the 20260928 images could stay black for good on their
first boot). Only on the long paths (conversion, resume, check, repair):

- the splash (`rsos-frontend --splash`, log `/run/rsos/splash.log`) is stopped
  with SIGTERM; after 3 s SIGKILL; after 2 s more the script goes on without it
  (a process stuck in the kernel), with its state and kernel stack in the
  trace. The splash itself exits within 2 s of SIGTERM (`alarm()`), its
  closing commit is non-blocking, and it never writes the backlight;
- each step is printed on the console (UART) as `rsos-data: ...`, written to
  the kernel log (`<5>rsos-data: ...`) and to `/run/rsos/firstboot.trace`,
  and a synced copy of that goes to a 64 KiB raw area outside every partition
  (board.ini `firstboot_trace_kib`: 3072 on the RetroStone2 and RetroStone1,
  i.e. 3 MiB, in the reserved gap between the U-Boot environment and rootfs
  A; checked against the partition table before use, so the marker and the
  backup at the end of the card are never touched). The area holds the
  record's magic line and the trace's **last** 64 KiB (a chatty splash log
  goes in as its last 8 KiB, a kernel stack as its first 4 KiB). A record
  that no boot collected (a boot that hung or lost power) is kept above the
  next one. The next boot that reaches the menu appends it to
  `RETROSTONE/rsos/logs/firstboot.txt` (bootlog; the file keeps its last
  256 KiB) and clears the area;
- the hardware watchdog (started by U-Boot, serviced by the kernel until a
  program opens it, for at most 120 s: `CONFIG_WATCHDOG_OPEN_TIMEOUT`) is
  serviced by a background keeper while the boot makes progress (the SD
  card's counters of **completed** I/O, fields 1, 3, 5 and 7 of
  `/sys/class/block/<disk>/stat`: the in-flight and `io_ticks` fields keep
  moving while a request is stuck; or a new step); after 180 s without either
  it stops servicing it and the board resets 16 s later (the conversion is
  power-cut safe). The keeper closes it with the magic `V` at the end, and
  the menu opens it again as before.

The `roms/<system>` folder list is generated at build time by `post-build.sh`
into `/usr/share/rsos/rom-folders`: every system in the `systems =` line of the
`[core]` section of `/usr/share/rsos/cores/*.ini`, plus `atari2600` and
`pcengine`. The same list is used for the seed.

**Test**: `board/retrostone2/tests/data-partition-test.sh` (root, in WSL, after
a build) writes `sdcard.img` to fake 2 GiB and 1 GiB cards on loop devices and
runs the script with BusyBox applets in a private mount namespace: seed with
only PC junk, seed with files, a power cut (`RSOS_TEST_ABORT`) before the MBR
write, right after it, in the middle of `mkfs.exfat`, after the format and
after the restore, each followed by a reboot, the review's scenarios G (cut
after sfdisk under an old image, also with files added afterwards) and H
(stale marker after a re-flash), a damaged backup, a failed backup, a card too
small, an unmountable exFAT without a marker (checked, not formatted; then
`--format-confirmed` beside a tmpfs `/data`, and its refusal without a data
problem), another filesystem (NTFS, FAT16), a blank partition and a damaged
FAT32 (never formatted, data-problem), a marker that cannot be cleared (the
stamp keeps a save made since), `partx` and `mkfs.exfat` failures (read-only
until the next boot finishes), a dirty volume, and a normal boot (about 30
ms); the trace (every step on the card at 3 MiB, the kernel log, the
watchdog closed with `V`, rootfs A, U-Boot and its environment untouched,
nothing on a normal boot), a stub splash that stops on SIGTERM (its log in the
trace), one that ignores it (SIGKILL, the boot goes on), a chatty one (its
last 8 KiB), a stalled step (the watchdog is no longer serviced), a request
stuck in the driver (in-flight and `io_ticks` moving: still no service) and a
first boot that never finished (its trace kept by the next one).

## Boot flow

1. Boot ROM -> SPL (DRAM init, 384 MHz) -> U-Boot v2026.07 (A20-OLinuXino-Lime
   defconfig + `uboot.fragment` + `patches/uboot/`: bootdelay 0, no video,
   USB, network, SATA or EFI; environment in raw MMC; bootstage; silent
   console; the hardware watchdog started with 16 s). Pressing a key on the
   UART right at power-on still stops autoboot (and turns the console back
   on); not in the release build (bootdelay -2).
2. `bootcmd` sets `silent=1` (unless `rsos_verbose=1`), then loads and runs
   `/boot/boot.scr`: slot b's copy first when `rsos_slot=b`, then slot a's
   (partition 2), then (console back on) slot b's (partition 3), then a bootstd
   scan as the last resort, then `poweroff`: U-Boot never waits at its prompt
   (it services the watchdog there, and the RetroStone2 panel, whose supply is
   always on, would stay powered without a picture). A development build still
   gives the prompt to a key pressed on the UART at power-on (the bootcmd does
   not run then). The partitions are spelled out: the former
   `for rsos_p in 2 3` loop did not work (found on QEMU: with the environment
   variable `rsos_p` just set by bootcmd, the loop variable did not take the
   values 2 and 3, "Can't set block device"), so a missing `boot.scr` on the
   selected slot ended at the prompt.
3. `boot.scr` (`board/retrostone2/boot.cmd`) runs the A/B logic below, loads
   `zImage` and the DTB from the chosen slot, applies the optional overlays and
   runs `bootz`, with `bootstage mark` timestamps around each load. Kernel
   command line:
   `console=ttyS0,115200 root=/dev/mmcblk0p<2|3> rootfstype=ext4 rootwait ro quiet panic=10 consoleblank=0 driver_async_probe=dw-apb-uart,musb-sunxi,sun4i-ts,axp20x-i2c initcall_debug log_buf_len=1M rsos.slot=<a|b> rsos.boot=<pending|ok>`
   plus `rsos_extraargs` (`initcall_debug log_buf_len=1M` only in the
   development build, see "Release build"). It never returns: if the kernel
   or the DTB cannot be loaded or `bootz` fails, it switches to the other slot
   (reset) or powers off (then resets, if the power-off returns).
4. Linux 6.18.54 (sunxi_defconfig + `linux.fragment` + `linux-patches.fragment`,
   LZ4-compressed zImage, everything needed at boot built in, no initramfs)
   mounts the root read-only and devtmpfs on `/dev`.
5. BusyBox init: `/etc/init.d/rcS` mounts proc/sys/devpts/tmpfs (`/run`, `/tmp`,
   `/dev/shm`), sets the `performance` governor for the boot, reads the boot
   reason, mounts `/data`, restores the clock and puts everything else in the
   background; then init starts `/usr/bin/rsos-frontend` first (respawned,
   through `/usr/libexec/rsos/frontend-respawn`; the line is only added to
   `/etc/inittab` when that binary is in the image, otherwise the system just
   boots to the UART shell), the UART shell on ttyS0 (see "Debug UART"), and
   `rsos-net apply` in the background. `frontend-respawn` execs the frontend
   (same pid) after counting its starts: after 5 starts within 60 s (a menu
   that crash-loops) it stops respawning and opens `/dev/watchdog` without
   servicing it, so the board resets (U-Boot counts the boot as failed and its
   A/B logic acts) instead of staying on with the panel powered and no
   picture; `reboot -f` 30 s later if no reset came. `touch
   /run/rsos/respawn-unlimited` turns it off (development). The kernel
   services U-Boot's watchdog only until the first open, for at most 120 s
   (`CONFIG_WATCHDOG_OPEN_TIMEOUT`): a hang in rcS or a menu that crashes
   before it opens the device resets the board too (`watchdog.open_timeout=0`
   in `rsos_extraargs` turns that off for development).

No udev and no mdev: devices come from devtmpfs, firmware is loaded by the
kernel directly from `/lib/firmware`.

## Boot time

The target is the black screen between power-on and the frontend's logo
(docs/CONVENTIONS.md: nothing in the boot path that isn't needed to play).

### Measured timeline (image `20260927b`, normal boot, before this work)

| Stage | Time | Source |
|---|---|---|
| Power key held until the AXP209 powers the SoC on | 1.0 s (REG36 `startup`) | `[power] ... power-on hold 1000 ms` |
| Boot ROM, SPL, U-Boot, zImage read + gzip decompression | not measured; ~0.9 s by difference with the owner's stopwatch | |
| Kernel time 0 -> `/sbin/init` | 0.55 s | dmesg `Run /sbin/init` |
| init -> rcS start (cold page cache: busybox, libc) | 0.13 s | rcS.times |
| rcS (mounts 0.10, data-partition 0.12, others 0.03) | 0.25 s | rcS.times |
| rcS done -> frontend `main()` (exec + loading its libraries cold) | 0.12 s | frontend.log |
| frontend start -> logo | 0.07 s | frontend.log `splash:` |

Kernel time 0 -> logo was 1.13 s; power-on -> logo about 2 s.

### What was changed (image `20260927c`)

- **LZ4 kernel** (`BR2_LINUX_KERNEL_LZ4`): the zImage is 5.48 MB instead of
  5.31 MB (gzip; the trimmed kernel would be 4.67 MB with gzip), i.e. ~10 ms
  (~40 ms against a trimmed gzip one) more to read at the U-Boot SD speed, but LZ4
  decompresses the 13.2 MB image in roughly 0.1 s where the kernel's inflate
  needs an estimated 0.4-0.7 s on the Cortex-A7 at 912 MHz. An uncompressed
  image would need ~0.7 s just to be read (U-Boot's sunxi MMC driver is PIO
  only, 4 bits at 50 MHz, ~15-20 MB/s), so LZ4 is the fastest choice.
- **Smaller kernel** (`linux.fragment`, "Boot time" section): only the A20
  platform (other Allwinner SoCs' clock, pin, RTC, DMA, PMIC-bus, display
  and PHY drivers removed), no SPI, CEC, I2S, PS/2, I2C-HID, USB gadget (the
  OTG port is host-only), PATA/SFF, initramfs support (and its six
  decompressors), cgroups, namespaces, io_uring, perf events, swap, kyber/BFQ
  schedulers, legacy ptys, regulatory-database signature checking (and the
  whole X.509 key infrastructure it pulled in; the crypto API is now modules,
  loaded with the WiFi/BT/SMB modules). `CONFIG_EXPERT` is on only to make
  some of these selectable; the defaults it changes are pinned back in the
  fragment. The uncompressed image went from 14.45 MB to 13.16 MB. `HZ=1000` so that the boot path's sleeps and
  polls (rootwait polls every 5 ms: 10-20 ms at `HZ=100`) have 1 ms
  granularity.
- **Asynchronous probes**: `driver_async_probe=dw-apb-uart,musb-sunxi,sun4i-ts`
  on the kernel command line (the UARTs took 55 ms in the probe cascade that
  leads to the SD card). EHCI/OHCI and sunxi-mmc probe asynchronously already.
  The audio codec stays synchronous: it must remain ALSA card 0. `lpj=` is
  not needed: the kernel already skips the delay calibration ("Calibrating
  delay loop (skipped), value calculated using timer frequency").
- **Silent U-Boot** (`CONFIG_SILENT_CONSOLE`, see `uboot.fragment`): every
  character costs 87 us at 115200 baud, and U-Boot printed ~1.2 KB (~0.1 s).
  `fw_setenv rsos_verbose 1; fw_setenv silent` brings the output back.
- **rcS / data-partition**: the normal-boot path of `data-partition` uses shell
  builtins instead of `cat`/`grep`/`head` and checks the homebrew marker
  before starting `rsos-seed-homebrew` (7 fewer fork+exec, each a few ms on the
  A20; the first-boot and recovery logic is unchanged). init starts the
  frontend before the UART shell. `rcS.times` has a new `sysfs + governor`
  step.
- **Boot logger fix**: up to `20260927b` it stopped after its first pass on
  every boot (its status function reused the loop's `t` variable, so the next
  `$((t + 30))` was a syntax error that ended the shell). rcS now also starts
  it in its own session (`setsid`); it logs its start, signals and exit in
  `bootlog.txt` and the current step in `heartbeat.txt`, and exits at once on
  rcK's SIGTERM.

### How the boot is measured

- U-Boot records **bootstage** marks (`CONFIG_BOOTSTAGE`,
  `patches/uboot/0001-...`): `reset`, `board_init_f`, `board_init_r`,
  `main_loop`, then boot.scr's own `bootstage mark` points (`boot.scr`,
  `load-zImage`, `zImage-loaded`, `dtb-loaded`, `overlays-applied`), then
  `bootm_start` ... `start_kernel`. The patch takes the timestamps from the ARM
  generic counter (CNTPCT, 24 MHz), which runs from reset: they are
  microseconds since power-on. `CONFIG_BOOTSTAGE_FDT` copies them into the DTB
  given to Linux: `/proc/device-tree/bootstage/<n>/{name,mark}`.
- The decompression happens after U-Boot's last mark and before the kernel's
  time 0. `/usr/libexec/rsos/cntvct -k` (built from
  `board/retrostone2/tools/rsos-cntvct.c` by `post-build.sh`) writes the
  current counter value to the kernel log; its printk timestamp gives the
  counter value at kernel time 0, so the kernel log, `rcS.times` and
  `frontend.log` join U-Boot's marks on one time line.
- The boot logger writes **`rsos/logs/boot<N>/bootstage.txt`** (RETROSTONE
  drive) 2 s after the menu is up: every step in ms since power-on with the
  delta to the previous one, then a summary: boot ROM + SPL, U-Boot init, the
  zImage read (with its MB/s), `zImage decompression + kernel head`
  (`start_kernel` -> kernel time 0), kernel -> `/sbin/init`, rcS, frontend
  start -> logo, and `BLACK SCREEN: power-on -> logo`. The time the power key
  is held before the SoC starts (AXP209 `startup`, 1000 ms) comes on top and
  is printed in the header.
- Per-driver kernel timing: `fw_setenv rsos_extraargs "initcall_debug
  log_buf_len=1M"`; the logger then also writes `initcalls.txt` (slowest
  initcalls, slowest probes, deferred probes). Remove it afterwards
  (`fw_setenv rsos_extraargs`).
- `/usr/libexec/rsos/bootlog --report` prints both reports on the UART at any
  time.

### Not done (evaluated)

- **Power-on hold time**: the AXP209 only powers the SoC on after the key has
  been held for 1 s (REG36[7:6]; the power module logs it). 128 ms would save
  ~0.9 s of the perceived black screen, at the cost of easier accidental
  power-ons in a bag. It is one sysfs write
  (`/sys/bus/platform/drivers/axp20x-pek/axp20x-pek/startup`, kept by the AXP
  while the battery is connected), a frontend power-module decision.
- **Falcon mode** (SPL loads the kernel directly, no U-Boot proper): saves
  U-Boot proper's init (~0.1-0.2 s, to be read from bootstage.txt), but the SPL
  has no ext4 or script support: the kernel, the DTB with the arguments
  already applied (`spl export`) and the overlays would have to live in raw
  areas per slot, the A/B logic (tries, fallback) would move into C in the
  SPL, and recovery would need a key to fall back to full U-Boot. Too much
  for the gain while bootstage has not shown U-Boot's share.
- **U-Boot boot logo** (`VIDEO_SUNXI` + a BMP splash): needs the LCD timings,
  the backlight PWM and the TCON0 parallel pins described to U-Boot, adds
  ~60-100 KB and ~50 ms (video init + reading and drawing the BMP) to U-Boot,
  and sun4i-drm resets the TCON and the display backend when it binds (in the
  late-initcall probe cascade, ~0.41 s kernel time), so the logo would be cut
  to black from there until the frontend's first modeset (~0.8-0.9 s kernel
  time after this work): a logo, then ~0.5 s of black, then the logo again.
  Keeping it would need simplefb + a hand-over in sun4i-drm (not in mainline).
  Not recommended; a sooner frontend logo is (see the frontend notes in the
  report of this change).
- **Uncompressed kernel, higher U-Boot CPU clock (960 MHz), DRAM clock,
  smaller U-Boot environment**: small or risky gains, left as they are.

## Shutdown

Goal: under 1 s from the power key to the power cut, without risking data.

1. The frontend saves (game state, settings), shows "Powering off..." briefly,
   writes `/run/rsos/shutdown-mode` (`poweroff` or `reboot`) and
   `/run/rsos/shutdown-start` (its `/proc/uptime` when the key was pressed),
   signals init (SIGUSR2 = power off, SIGTERM = reboot) and exits.
2. BusyBox init runs `/etc/init.d/rcK` (`::shutdown`), which:
   starts a deadline process (below), stops the boot logger (SIGTERM; it exits
   at once), stops the frontend if it is still running (SIGTERM, polled every
   20 ms, up to 3 s, or 12 s when a game runs: a second `rsos-frontend`
   process or `/run/rsos/game-running`; the menu gives a game 8 s to save its
   state), syncs at once, stops the
   hardware watchdog (magic close), gives back the A/B boot try if this boot
   was not confirmed yet (`rsos-boot-ok refund`, one `fw_setenv`, only then:
   see "A/B slots"), saves the clock (`rsos-clock save`), turns Bluetooth off
   only if its pairing store is mounted (waiting at most 3 s for a running
   `rsos-net`), stops the SMB share only if ksmbd is loaded, appends its step
   times to `/data/rsos/logs/shutdown.txt` (the last 5 shutdowns), syncs once
   and unmounts `/data` (read-only remount if busy; if it is still read-write,
   SIGTERM/SIGKILL to every process, as init would, unmounts the Bluetooth
   store's loop filesystem, and tries again). Each of these steps has a time
   limit (`timeout`), and the deadline process forces `poweroff -f -n` (or
   `reboot -f -n`) when the whole shutdown takes more than 35 s (a step stuck
   in the kernel, a dying card): with the watchdog disarmed, nothing else
   would stop a hang there from keeping the unit on until the battery dies.
3. **Fast path**: when the frontend wrote one of the two files, rcK then runs
   `poweroff -f` (or `reboot -f`) itself. The kernel's axp20x power-off
   handler switches the AXP209 off, exactly as on init's path, but init's own
   end of shutdown is skipped: it sleeps 1 s after its SIGTERM to every
   process and 1 s more before `reboot(2)`. Any other shutdown (`reboot` or
   `poweroff` typed on the UART) does the same clean-up and returns to init's
   normal path.

The exFAT volume is cleanly unmounted (its dirty flag is cleared), the root is
read-only, and the U-Boot environment is only written at shutdown when the
boot was not confirmed yet (a power-off within about 40 s of the power-on).
`shutdown.txt` looks like:

```
=== shutdown: poweroff
41.52 frontend: shutdown requested
41.85 rcK start
41.87 bootlog stopped
41.89 clock saved
41.90 log written; next: sync, unmount /data
```

## A/B slots and the boot state

The boot state lives in the U-Boot environment, stored raw on the card at
1 MiB and 1 MiB + 64 KiB (`CONFIG_ENV_REDUNDANT`: U-Boot and `fw_setenv` write
the two copies alternately with a sequence flag, so a power cut during a write
always leaves one valid copy). Linux reads and writes it with `fw_printenv` /
`fw_setenv` (`/etc/fw_env.config`). This was preferred over a small FAT/ext4
"bootstate" partition: there is no filesystem to corrupt or repair, and U-Boot
needs no extra filesystem code.

| Variable | Values | Meaning |
|---|---|---|
| `rsos_slot` | `a` / `b` | slot to boot (default `a`) |
| `rsos_ok` | `1` / `0` | `1`: the slot is confirmed (it reached a stable menu once). `0`: freshly installed, on trial |
| `rsos_tries` | `0`-`9` | trial slot: boots left before falling back |
| `rsos_fails` | `0`-`9` | confirmed slot: boots in a row that were not confirmed |
| `rsos_fallback` | `a` / `b` / empty | the slot this one fell back from (no ping-pong); cleared by the confirmation |
| `rsos_bad` | `a` / `b` / empty | an updated slot whose trial ran out: U-Boot never falls back to it; only the updater's next write clears it (never a confirmation) |
| `rsos_good` | `a` / `b` / empty | the last slot `rsos-boot-ok` confirmed |
| `rsos_rounds` | `0`-`9` | slots that ran out of boots (or could not be loaded) since the last confirmed boot: the failure budget; cleared by the confirmation |
| `rsos_maxfails` | `0`-`9` (default 3) | fallback threshold for a confirmed slot; `0`: do not count, no environment write on a normal boot |
| `rsos_maxrounds` | `1`-`9` (default 4) | failure budget: power off when `rsos_rounds` reaches it (about two full rounds of both slots) |
| `rsos_overlays` | e.g. `emmc sata` | device tree overlays to apply (see below) |
| `rsos_lowvolt` | `trial` / `failed` / empty | the one-shot `cpu-lowvolt` test overlay (see below) |
| `rsos_extraargs` | e.g. `quiet` | appended to the kernel command line |

**Every boot is counted, and Linux confirms it** (system review S5, S6). A
confirmed slot used to be booted forever: a kernel that panicked (`panic=10`)
or hung after an update had been confirmed, or a slot whose kernel became
unreadable, gave an endless reboot loop or a U-Boot prompt. Rules (in
`boot.cmd`):

- Very first boot (no saved environment): store `rsos_slot=a rsos_ok=1
  rsos_tries=0`, so that `fw_setenv` always finds a valid environment written
  by this U-Boot.
- Trial slot (`rsos_ok=0`): `rsos_tries>0`: decrement, save, boot; `0`: fall
  back.
- Confirmed slot (`rsos_ok=1`): `rsos_fails<rsos_maxfails`: increment, save,
  boot; else fall back.
- A slot that runs out of boots is exhausted: `rsos_rounds + 1`, and a trial
  slot is marked `rsos_bad` (an update that never got confirmed must never
  come back through a later fallback: before this, three unconfirmed boots of
  the good slot fell back to it, and when it failed again, "staying on" it
  lasted for ever). Then:
  - `rsos_rounds` reached `rsos_maxrounds` (default 4, about two full rounds
    of both slots): **power off** (then `reset`, in case the power-off
    returns), rather than loop for ever with the panel powered and no
    picture. The next power-on gets a fresh budget on the last confirmed slot
    (`rsos_good`, unless it is marked bad).
  - else fall back: if the other slot has a kernel (`test -e mmc 0:<part>
    /boot/zImage`), is not `rsos_bad`, and either did not fail before in
    this episode (`rsos_fallback`) or is the last confirmed slot (both
    failed: `rsos_good` is preferred), switch to it: `rsos_slot=<other>
    rsos_ok=1 rsos_fails=1 rsos_fallback=<failed slot>`.
  - otherwise stay and count again from 1 (a factory card has an empty slot
    B). One `saveenv` per boot at most.
- The kernel gets `rsos.boot=pending` when the boot was counted.
- The kernel or the DTB cannot be loaded, or `bootz` returns: a trial slot is
  marked bad, one round is counted, and U-Boot switches to the other slot at
  once and resets when it has a kernel, is not `rsos_bad` and the budget
  allows (also when it is `rsos_fallback`: before, an unloadable slot whose
  other slot had failed once powered off at every power-on), else powers off
  (fresh budget at the next power-on). Never a U-Boot prompt: the bootcmd
  itself ends with `poweroff`, and `boot.scr` resets if the power-off returns
  (TODO(hw): check that U-Boot's AXP209 power-off really cuts the power).
- A slot running an older `boot.scr` does not know `rsos_bad` (U-Boot runs the
  selected slot's script): the protection is complete once both slots hold
  this version.

In Linux, rcS copies `rsos.boot=pending` to `/run/rsos/boot-state`, and:

- **Confirm**: the frontend runs `/usr/bin/rsos-boot-ok` (detached) once its
  menu is up, outside charge mode. It waits for `/run/rsos/net-applied` (written
  at the end of `rsos-net apply`, i.e. once the WiFi/Bluetooth/Ethernet drivers
  that the settings ask for are loaded and the PHY is parked; in charge mode it
  keeps waiting, because `apply` runs when the user leaves charge mode; after
  120 s outside charge mode it goes on anyway), then 30 s more, and checks that
  the menu process is still the same (no crash and respawn). Then one
  `fw_setenv -s`: `rsos_ok=1 rsos_tries=0 rsos_fails=0 rsos_good=<slot>`,
  `rsos_fallback` and `rsos_rounds` cleared (the old `rsos_fallback` goes to
  `/run/rsos/boot-fallback` for the UI), and a pending `rsos_lowvolt=trial`
  cleared; never `rsos_bad`. It refuses when the running slot (`rsos.slot=`)
  is not `rsos_slot`, or when the kernel was not booted by `boot.scr` (no
  `rsos.slot=`). One waiter at a time. `rsos-boot-ok now` confirms at once;
  `rsos-boot-ok status` prints the state. Every access takes
  `/run/rsos/bootenv.lock`, the lock the updater takes around its slot
  switch, and re-reads `rsos_slot` under it: a confirmation can never land on
  a slot just selected for its trial.
- **Refund**: an orderly shutdown or reboot before the confirmation (rcK,
  `rsos-boot-ok refund`) gives the try back (`rsos_fails - 1`, or
  `rsos_tries + 1` on trial, and clears a `cpu-lowvolt` trial): turning the
  unit off right after the menu appears, or a charge-mode session, is not a
  failed boot.

So a slot that panics, hangs (hardware watchdog, lockup detectors: docs/power.md
§11), crashes the menu in a loop or loses power before the stability window,
three boots in a row, is replaced by the other slot, even if it was confirmed
before. A flat battery that cuts three boots in a row does the same (harmless:
the other slot is the previous version, and the next confirmation resets
everything). **Cost**: one `saveenv` in U-Boot per boot (bootstage marks
`boot.scr` -> `env-saved`; TODO(hw): measure it) and one `fw_setenv` in Linux
about 40 s later in the background. `fw_setenv rsos_maxfails 0` turns the
counting of confirmed slots off.

**Tests**: `board/retrostone2/tests/boot-ab-qemu-test.sh` builds U-Boot for
QEMU's cubieboard (A10) with this tree's patches and fragment, and boots fake
kernels (a "kernel" that starts and never confirms, one that `bootz` rejects):
first boot, counting, fallback after 3 failures, both slots failing (back to
the last confirmed one, then the power-off after the budget and a fresh
budget), empty slot B, corrupt kernel (also with `rsos_fallback` naming the
good slot), both corrupt (switches within the budget, then the power-off and
the reset), a slot on trial that never confirms (marked bad; the good slot
failing afterwards never falls back to it; the updater's write clears the
mark), `rsos_maxfails=0`, overlays, the one-shot `cpu-lowvolt`, the watchdog
start, and no `boot.scr` at all (the bootcmd's power-off) (40 checks);
`board/sbc-uboot/tests/boot-ab-qemu-test.sh` does the same for the Orange Pi
script (24 checks). `board/common/tests/boot-ok-test.sh` tests `rsos-boot-ok`
with the host `fw_setenv` on an environment image (20 checks, including the
lock shared with the updater).

**Updater contract** (implemented by `rsos-update`, docs/updates.md): write
the new root filesystem to the inactive slot's partition (its first 64 KiB
zeroed first and written last, so a half-written slot never has a superblock
and is never a fallback target), read it back, then, under
`/run/rsos/bootenv.lock` and after checking again that the running slot is
the selected, confirmed one, in one step `fw_setenv -s` with
`rsos_slot=<new> rsos_ok=0 rsos_tries=3 rsos_fails=0` and an empty
`rsos_fallback`, `rsos_bad` and `rsos_rounds`, and reboot. It refuses while
the running slot is on trial (`rsos_ok=0`): that slot's fallback must not be
overwritten.
`board/retrostone2/tests/update-ab-test.sh` (root, after a build) checks the
whole chain on the real image (22 checks, including U-Boot in QEMU booting the
updated slot and ignoring an interrupted one).

Useful from the UART shell: `fw_printenv | grep rsos_`, `fw_setenv rsos_slot b`
(switch slots by hand), `rsos-boot-ok status`.

Note: the saved environment is a full copy of U-Boot's default environment
(including `bootcmd` and the transient `silent`, which keeps the console quiet
from the environment load on). Updates never touch U-Boot itself (8 KiB-1 MiB
on the card, not A/B: a power cut while rewriting it would need a PC to
re-flash the card; an update that needs a newer U-Boot says so with the
`bootloader_min` of its manifest and is refused: docs/updates.md 4.6). An update that did replace U-Boot would also have to
rewrite the environment, keeping the `rsos_*` variables, or the old `bootcmd`
would stay. (`CONFIG_ENV_WRITEABLE_LIST` would import only the `rsos_*`
variables, but it needs the list in a board header, i.e. a U-Boot patch; not
done.)

## Optional hardware ("Pro" units): device tree overlays

The eMMC (16 GB) and the M.2 SATA slot only exist on the "Pro" variant, so
their nodes are disabled in the base DTS. To enable them, from the UART shell
(or later from the frontend):

```
fw_setenv rsos_overlays "emmc sata"
```

Each name loads `/boot/overlays/<name>.dtbo` from the booted slot (built from
`board/retrostone2/dts/overlays/retrostone2-<name>.dtso`; the base DTB is built
with `-@` so it has the symbols overlays need). If an overlay fails to apply,
the base DT is reloaded and the boot continues. With `rsos_overlays` unset the
boot script does no extra work. The setting lives in the environment, not in a
slot, so it survives updates. `fw_setenv rsos_overlays` (no value) removes it.
The test overlay `cpu-lowvolt` (the 1.0 V CPU floor, which may freeze the
board) is one-shot: `boot.scr` applies it with `rsos_lowvolt=trial`, which
`rsos-boot-ok` clears once that boot is confirmed (or given back at an orderly
shutdown); a boot that finds `trial` still set (the last one froze, the
watchdog reset it) skips it and sets `rsos_lowvolt=failed`, and it stays
skipped until `fw_setenv rsos_lowvolt`.

The AHCI driver is built into the kernel; with the SATA node disabled it never
probes, so it costs no boot time on units without the slot.

## Networking (WiFi, Ethernet, Bluetooth)

All three are **off by default**, never started at boot, and switched at
runtime without a reboot by `/usr/bin/rsos-net`:

```
rsos-net [-n] {wifi|eth|bt} {on|off|status}
```

`on`/`off` change the hardware state and save it in `/data/rsos/settings.ini`
as `wifi=0|1`, `eth=0|1`, `bt=0|1` (`-n`: don't save). `status` prints `on` or
`off`. The frontend will own `settings.ini` later and call this helper.

At boot:
- `rcS` runs `rsos-net apply-early` in the background: if `wifi` is not `1`, it
  unbinds the mmc3 SDIO host so the chip is left unpowered.
- init runs `nice rsos-net apply` (`::once`), which waits for the menu
  (`/run/rsos/menu-up`, at most 20 s) and 1 s more so the frontend comes up
  first, then turns on whatever is set to `1`, parks the Ethernet PHY, and
  writes `/run/rsos/net-applied` (for `rsos-boot-ok`). Nothing in charge mode.

All the drivers are modules that nothing loads automatically. Only one
`rsos-net` changes the hardware at a time (`flock` on `/run/rsos-net/lock`;
`apply` takes it after its wait for the menu), so a WiFi switch from the menu
during `apply` waits instead of racing it; the daemons it starts do not inherit
the lock. `settings.ini` and the Bluetooth store are written as tmp file +
`fsync` + rename + `sync` (exFAT has no journal). A Bluetooth store that does
not mount is set aside as `bluetooth.img.bad` and a new one is made.

**WiFi MAC address**: the AP6210 nvram carries the Broadcom default MAC, for
which brcmfmac picks a random address at every load (a new DHCP lease at every
boot, MAC filtering impossible). With `wifi_mac = sid` in `board.ini` (the
RetroStone2), `rsos-net` sets a stable, locally administered address before
`wlan0` goes up: `02:` + 5 bytes of the MD5 of the A20 SID
(`/sys/bus/nvmem/devices/sunxi-sid0/nvmem`) and the interface name. TODO(hw):
check it on the device.

**Clock**: once a DHCP lease is bound, the udhcpc hook sets the clock with
BusyBox `ntpd` (docs/power.md §11).

| | on | off |
|---|---|---|
| WiFi (AP6210, brcmfmac on SDIO mmc3) | bind the mmc3 host (pwrseq raises WL_REG_ON), `modprobe brcmfmac`, `wlan0` up; if `/data/rsos/wpa_supplicant.conf` exists: `wpa_supplicant` + `udhcpc` | stop them, `modprobe -r brcmfmac`, **unbind** mmc3 from `sunxi-mmc` (pwrseq drops WL_REG_ON: chip unpowered) |
| Ethernet (GMAC `dwmac-sunxi` + LAN8710A) | `modprobe dwmac-sunxi` (and `sun4i-emac`), `eth0` up, `udhcpc` | stop udhcpc, `eth0` down, unload the MAC drivers |
| Bluetooth (BCM20710 on UART2, `hci_uart`) | `modprobe hci_uart`, mount the pairing store, start `dbus-daemon` and `bluetoothd` | stop `bluetoothd`, unmount the store, `modprobe -r hci_uart btbcm` (closing the serdev powers the chip down) |

**Bluetooth pairings** (`/var/lib/bluetooth`): BlueZ names its directories
after MAC addresses (`AA:BB:CC:DD:EE:FF`), which exFAT cannot store. They live
in an 8 MiB ext4 image, `/data/rsos/bluetooth.img`, created by the first
`rsos-net bt on` (`mkfs.ext4` from e2fsprogs), checked with `e2fsck -p` and
loop-mounted on `/var/lib/bluetooth` while Bluetooth is on (`CONFIG_BLK_DEV_LOOP`).
It is unmounted by `bt off` and by `rcK` at shutdown. The ext4 journal means a
power cut loses at most the last pairing. Without `/data` a tmpfs is used for
the session.

**SMB share `\\RETROSTONE`**: `ksmbd.ko` (module, `CONFIG_SMB_SERVER=m`) and
ksmbd-tools are in the image but **never started at boot** and **not wired
yet**: nothing starts them (neither `rsos-net` nor the frontend, which will
start and stop the share on demand, docs/rom-transfer.md 3.3). rcK already
stops ksmbd if it is loaded. When it is wired, share `roms/`, `bios/` and
`saves/` only: `rsos/` holds `wpa_supplicant.conf`, `settings.ini` and
`bluetooth.img` (an ext4 image that root loop-mounts). Size: `ksmbd.ko`
283 KiB + 4 small crypto/NLS modules + ksmbd-tools 110 KiB (its libraries,
glib and libnl, were already in the image).

**Listening sockets** when the network is up: the frontend's name responders
(`netnames`: UDP 5353 mDNS, 5355 LLMNR and 137 NetBIOS) and, when it is on, the
web share (TCP 80, PIN, private addresses only). They bind to every interface
(`INADDR_ANY`), which is acceptable for a handheld on a home network; the
kernel has no netfilter, so no firewall can be added without a new kernel.

`/media` is a symlink to `/run/media` (tmpfs): the frontend mounts USB sticks
there (the root filesystem is read-only).

A minimal `/data/rsos/wpa_supplicant.conf`:

```
ctrl_interface=/run/wpa_supplicant
network={
    ssid="MyNetwork"
    psk="password"
}
```

### Ethernet PHY power while Ethernet is off

In Linux 6.18, closing the interface powers the PHY down: `stmmac_release()`
-> `phylink_disconnect_phy()` -> `phy_detach()` -> `phy_suspend()` ->
`genphy_suspend()` (the SMSC/LAN8710 driver uses it), which sets BMCR
power-down. Unloading `dwmac-sunxi` also disables its optional `phy-supply`
regulator.

But the LAN8710A comes out of reset **powered up**, and while the MAC driver
is not loaded nothing ever touches it. Options:

1. Do nothing: zero boot cost, the PHY draws its active current (tens of mA)
   while the unit is on.
2. **Park it (what `rsos-net apply` does when `eth` is not `1`)**: in the
   background, after the frontend has started, load the MAC driver, bring
   `eth0` up and down once (PHY -> BMCR power-down) and unload the driver.
   A few hundred ms of niced background work, nothing blocks the boot.
3. Hardware: if the PHY supply (or its reset line) is switchable, describe it
   as the GMAC `phy-supply` regulator (or a reset GPIO) in the DTS; the kernel
   then turns it off when unused, which is better than 1 and 2.

TODO(hw): measure the current with and without parking, and check the
schematic for a switchable PHY supply.

### Firmware

- WiFi AP6210 (BCM43362): `brcm/brcmfmac43362-sdio.bin` (linux-firmware,
  Cypress blob) and `brcm/brcmfmac43362-sdio.txt` -> the Cubietruck nvram (same
  AP6210 module). TODO(hw): check the nvram on the RetroStone2.
- BT BCM20710: `brcm/BCM20710A1.hcd` from the Armbian firmware repository, with
  aliases `BCM20702A1.hcd` and `BCM.hcd` (btbcm has no name for this chip).
  TODO(hw): check in dmesg which name btbcm requests.
- AP6212 fallback: `brcmfmac43430-sdio.bin/.txt` (AP6212 nvram) and
  `BCM43430A1.hcd`.

## Debug UART

The debug console is **uart0 on PB22 (TX) / PB23 (RX), 115200 8N1, 3.3 V**.
Connect a 3.3 V USB-serial adapter: adapter RX -> PB22 (TX), adapter TX ->
PB23 (RX), GND -> GND. Do not connect the adapter's VCC and never use a 5 V
(or RS-232) adapter. TODO(hw): mark the pads on the board photo/pinmap.

On Windows, find the COM port in Device Manager and open it with PuTTY
(Connection type "Serial", speed 115200) or `plink -serial COM5 -sercfg
115200,8,n,1,N`. The SPL, U-Boot and kernel messages all go there, and
`/usr/libexec/rsos/uart-shell` starts once the menu is up: a root shell with no
login in the development build, a root login (password set at build time) in
the release build, or nothing (see "Release build").

## Release build

The development image (`retrostone2_defconfig`) keeps bring-up aids that a
consumer image should not have (system review S9). One switch,
`BR2_RETROSTONE_RELEASE` (menuconfig: External options > RetroStoneOS build),
turns them off; `retrostone2_release_defconfig` is `retrostone2_defconfig` plus
that block (`board/common/tests/release-defconfig-test.sh` checks that the two
stay in step):

| | Development | Release |
|---|---|---|
| Kernel command line | `initcall_debug log_buf_len=1M` (the boot logger writes `initcalls.txt`) | without them: `board/retrostone2/post-build.sh` removes them from `boot.cmd` before `mkimage` and fails if they are still in the `bootargs` line |
| Boot logger | copies the logs every 2 s for 5 min, then every 30 s for an hour | once when the menu is up, 10 s later (the boot time line), then every 10 minutes for an hour; `rsos/logs/verbose` on the card restores the development cadence, `rsos/logs/disabled` turns it off in both |
| UART | root shell, no login | `BR2_RETROSTONE_UART_SHELL_*`: **password** (default: `login` as root), open, or disabled (no inittab line) |
| U-Boot | a key on the UART stops autoboot | `bootdelay -2`: never stops (`board/retrostone2/uboot-release.fragment`); a unit that cannot boot is recovered by flashing the card again |
| Version string | `RetroStoneOS 0.1-dev (<date>)` | `RetroStoneOS 0.1 (<date>)` (the version: `BR2_RETROSTONE_VERSION`, set by CI from the tag, else the rsos-frontend package's) |
| System updates (docs/updates.md) | signed packages; unsigned ones only from a local file, after a warning | signed packages only |

The UART choice is open (the owner has not decided): the release default is a
root login with the password in `BR2_RETROSTONE_UART_PASSWORD`, whose default
is the placeholder `CHANGE-ME`: **the build stops until it is set**
(`board/common/post-build.sh` writes its SHA-512 hash, made with the host
`mkpasswd`, into `/etc/shadow`). Build it in its own output directory:

```sh
cd ~/rsos/buildroot-2026.02.3
make O=~/rsos/output-release BR2_EXTERNAL=/mnt/c/path/to/RetroStoneOS/buildroot-external retrostone2_release_defconfig
sed -i 's/^BR2_RETROSTONE_UART_PASSWORD=.*/BR2_RETROSTONE_UART_PASSWORD="your password"/' ~/rsos/output-release/.config
make O=~/rsos/output-release olddefconfig
nice -n 10 make O=~/rsos/output-release -j16
```

`/etc/rsos/build.env` on the target says which variant it is (`RSOS_RELEASE`,
`RSOS_UART_SHELL`). The same switch works for other boards (the boot logger,
the UART and the version string are common; the Raspberry Pi's command line
has no debug arguments). Not part of the switch yet: `BR2_REPRODUCIBLE`.

## Adding packages

Buildroot 2026.02 still uses the Linux 4.x kconfig, which has neither
`osource` nor wildcards in `source`. `external.mk` includes every
`package/*/*.mk` automatically, but each package's `Config.in` must be sourced
from `buildroot-external/Config.in` by hand (the libretro cores are grouped in
`package/libretro-cores.Config.in`, which lists each core).

Because every `.mk` is included even when its package is disabled, a package
must parse cleanly while disabled (for example, a `local` site method needs a
non-empty `_SITE` even when the package is off).

Enabled in `retrostone2_defconfig`: `rsos-frontend` (the `rsos-frontend` menu,
started by init, plus the tools `rsos-run`, `rsos-kmstest`, `rsos-display-selftest`,
`rsos-bootreason`, `rsos-clock` and the system updater `rsos-update`, with
`/usr/share/rsos/update.pub` and `/etc/rsos/version.env`; it selects `zstd`,
`mbedtls` and `ca-certificates` for the updater, about 2.2 MB in the root
filesystem: docs/updates.md), the 25 libretro cores (installed
to `/usr/lib/libretro/`, metadata in `/usr/share/rsos/cores/`), the bundled
games (`rsos-homebrew`, the first-boot seeding; `rsos-ucity` and
`rsos-freedoom`, downloaded like any source: docs/homebrew.md) and
`rsos-vc-games`. **rsos-vc-games builds from a RetroStone VC checkout next to
this one (`../RetroStoneVC`, https://github.com/PaddleStroke/RetroStoneVC):
without it, set `BR2_PACKAGE_RSOS_VC_GAMES` to n** (`make O=~/rsos/output
menuconfig`, or edit the defconfig), or the build stops.

Device tree overlays: every `board/retrostone2/dts/overlays/*.dtso` is built
by the kernel and installed as `/boot/overlays/<name>.dtbo` (glob in
`external.mk`); currently `emmc`, `sata` and `lcd60`.

`/etc/inittab` starts `/usr/bin/rsos-frontend` (respawned) only if that binary
is in the image: `post-build.sh` adds the line when it exists. Otherwise the
system just boots to the root shell on the UART.

### Licences

Every image carries `/usr/share/rsos/licenses.txt`: each package of the image
with its version and licence (the package's `_LICENSE`, what `legal-info`
reports; Buildroot's own skeleton/script packages, which declare none, are
listed as Buildroot's GPL-2.0+), the toolchain's C and GCC runtime libraries, and where the licence texts
and the complete source code are (the legal-info archive of each release). It
is written by a `TARGET_FINALIZE_HOOKS` entry in `external.mk` from what make
already knows (no `legal-info` run, a few KB, never read at boot), so a new
package appears in it without any change there. Read it on the UART (`cat
/usr/share/rsos/licenses.txt`) or on the SD card image; the menu has no text
viewer for it yet (TODO: a Settings > System information > Licences entry once
there is a generic scrollable text screen; `ui/update_ui.c`'s notes screen is
the model).

### Round 3 (2026-09-27, image `…b3`, estimates to confirm on hardware)
Baseline measured on hardware (image f): power-on → logo 2090 ms (SPL 468, U-Boot 240, zImage read 231, LZ4 188, kernel 427,
init → frontend 435). Changes:

| Stage | Change | Expected |
|---|---|---|
| SPL | bootstage marks carried to U-Boot proper (U-Boot patch 0002); DRAM tries the final 1 GiB geometry first; no banner | −10…15 ms, plus a split of the rest in bootstage.txt |
| U-Boot | `SKIP_EARLY_DM` (dm_f ran with the data cache off: 100 ms → ~1 ms), unused commands dropped | −95…100 ms |
| zImage | 5.48 → 4.94 MB: SCSI/sd/usb-storage/uas/SATA/NTFS3/phylib/at24 as modules; VT, KEYS, XZ, bsg removed | −35…45 ms |
| Kernel | patch 0005 (CCU at subsys_initcall); root ext4 without journal/orphan file; mmc0 power-on delay 1 ms; i2c0 400 kHz + AXP async; 4 UARTs | −170…290 ms |
| Userspace | static BusyBox; 2 mount runs instead of 7; idle-priority prefetch of the frontend and its libraries; UART shell after menu-up | −150…200 ms |
| Shutdown | `rsos-axpstamp` stores the post-unmount timestamps in AXP209 REG04-0F; bootlog appends them to shutdown.txt | measurement |

Module loading without udev: rcS loads `usb-storage uas sd_mod` in the background after menu-up; `ntfs3` loads on mount;
`ahci_sunxi` only with the sata overlay. This dev image has `initcall_debug log_buf_len=1M` in bootargs (+30…60 ms) to
produce `initcalls.txt`; the release build (see "Release build") leaves them out. Switches: `fw_setenv rsos_extraargs "rsos.ra_kb=N rsos.prefetch=0"`.
