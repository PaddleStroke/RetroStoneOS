# Getting ROMs, BIOS files, saves and themes onto the RetroStone2

How users put files on the console, and what the build and UI owners must do
to finish it. Code: `frontend/src/transfer/` (C API in `transfer.h`),
`frontend/src/transfer/web/index.html`, `frontend/transfer.mk`.

| Method | Needs | Speed (expected) | Status |
|---|---|---|---|
| 1. SD card in a PC card reader | a card reader | card/reader speed (10-80 MB/s) | partition layout done by the build owner; change list in [1.4](#14-change-list-for-the-build-owner) |
| 2. USB stick in the console: "Import games", "Export games", "Back up saves" | nothing (works offline) | bound by the SD card's write speed since the 2026-09-27 copy engine (3.9 MB/s measured before it; on emulated devices 5.7 -> 8.9 MB/s, §2.3). TODO(hw) | implemented, tested on host (import tested on hardware 2026-09-27) |
| 3a. Web page upload (phone or PC browser) | WiFi or Ethernet on | WiFi 2.4 GHz 1x1 ~2-4 MB/s, Ethernet 10/100 ~8-11 MB/s. TODO(hw) | implemented, tested on host |
| 3b. SMB share `\\RETROSTONE` | WiFi/Ethernet + kernel ksmbd | similar to 3a | implemented, opt-in (batch 2; [3.3](#33-smb-share-retrostone)), TODO(hw) |
| 3c. SFTP (dropbear) | WiFi/Ethernet | ~1-2 MB/s (cipher-bound on the A7) | recommendation only ([3.4](#34-sftp-advanced)) |

## 0. Hardware constraints

- The **micro-USB port is power-only** (no data lines on the schematic) and the
  three **USB-A ports are hosts with 5 V always on**. There is no USB gadget
  mode: the console can never appear as a USB drive on a PC.
- **Never connect the console to a PC with an A-to-A cable**: both sides drive
  5 V onto VBUS (back-feeding), which can damage the PC port or the console.
  This warning is in the data partition `README.txt`, and must be in the user
  manual and on the "Transfer files" help screen.
- WiFi, Ethernet and BT are off by default (`rsos-net`). Nothing in this
  document runs while networking is off, and nothing runs in the boot path.

---

## 1. SD card in a PC (primary method)

### 1.1 Windows behaviour (verified from sources)

- **Before Windows 10 version 1703**, Windows only mounted **the first
  partition of removable media** (USB flash drives, SD cards in USB readers);
  all other partitions were ignored. 1703 (Creators Update, April 2017) added
  multi-partition support for removable drives
  ([Born's Tech](https://borncity.com/win/2017/04/22/windows-10-version-1703-usb-stick-multi-partition-support/),
  [Windows OS Hub](https://woshub.com/creating-multiple-partitions-on-a-usb-drive-in-windows-10/)).
- "First" means **the first entry of the MBR partition table**, not the lowest
  disk offset: the standard workaround for older Windows is BOOTICE "Set
  Accessible" / RMPrepUSB, which only reorders the MBR entries so the wanted
  partition is entry 1
  ([RMPrepUSB tutorial 077](https://rmprepusb.com/tutorials/077-create-a-multiple-partition-multi-boot-usb-flash-drive-under-windows/),
  [Tom's Hardware forum](https://forums.tomshardware.com/threads/set-accessible-partition-on-flash-drive.2781784/)).
  An MBR does not require its entries to be in disk order.
- **Windows 10 1703+ and 11 give a drive letter to the Linux (0x83) partitions
  of an SD card too, and Explorer then says "You need to format the disk in
  drive X: before you can use it"**. This is the classic Raspberry Pi card
  complaint
  ([Raspberry Pi forum 1](https://forums.raspberrypi.com/viewtopic.php?t=201012),
  [2](https://forums.raspberrypi.com/viewtopic.php?t=208298)). With the A/B
  layout the card has **two** ext4 partitions, so users may get two such prompts.
- On MBR disks, Microsoft uses type **0x27** for recovery/utility partitions so
  that they get **no drive letter** (`diskpart set id=27`,
  [Microsoft Learn](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/configure-biosmbr-based-hard-drive-partitions?view=windows-11)).
  Linux, U-Boot (`load mmc 0:N`) and `root=/dev/mmcblk0pN` ignore the MBR type,
  so the root slots could use it. Whether Windows also honours it on
  *removable* media is **not documented**: TODO(hw) test on Windows 10 22H2
  and 11 24H2 with the slots typed 0x83, then 0x27 (see change C3).

Conclusion: data partition = **MBR entry 1**, physically last (old Windows see
only it; new Windows show it first); roots = entries 2 and 3.

I checked that genimage 19 (the version Buildroot builds) writes the MBR entries
in the order the partitions are listed, whatever their offsets, and only checks
for overlaps: a test image with `data` listed first at 65 MiB and `rootfs` second
at 1 MiB gives `sdcard.img1: start=133120 type=c` / `sdcard.img2: start=2048
type=83, bootable` (`sfdisk -d`).

### 1.2 Current state (build owner, 2026-09-25)

Already done in `genimage.cfg` / `post-image.sh` / `data-partition`:
- MBR entry 1 = data (type 0x07, at 1028 MiB, physically last), entries 2/3 =
  rootfs A/B (0x83). **This matches the recommendation.**
- The image contains a **64 MiB empty exFAT** data filesystem, so the
  RETROSTONE drive exists right after flashing.
- First boot: if the filesystem holds no regular file, grow entry 1 to the end
  of the card and `mkfs.exfat`; otherwise only grow the entry and **keep the
  64 MiB filesystem** (TODO in the script).
- FAT32 fallback: `data-partition` mounts `vfat` when exFAT fails.

Problems with it:
1. **Bug: the "empty" test sees Windows/macOS junk as user files.** Windows
   writes `System Volume Information\IndexerVolumeGuid` / `WPSettings.dat` on
   removable drives it mounts, and macOS writes `.Spotlight-V100`,
   `.fseventsd`, `.Trashes`, `._*`. So a user who only *looks* at the fresh card
   on a PC (or whose flasher auto-mounts it) never gets the full-size
   filesystem: the card stays a 64 MiB drive forever.
2. When the user really copied files before the first boot, the filesystem
   stays 64 MiB while the partition grows: Windows shows a 64 MiB drive, and
   the rest of the card is unusable until a manual reformat.
3. The seed is empty: no folder skeleton, no `README.txt` (an exFAT image
   cannot be populated at build time without mounting it, and Buildroot runs
   unprivileged; there is no mtools equivalent for exFAT).
4. Two ext4 partitions may each trigger a "format this disk?" prompt.

### 1.3 Design choices

**Seed filesystem: FAT32, 128 MiB, populated at build time.** FAT32 images can
be filled without mounting (`mkfs.vfat` + `mmd`/`mcopy` from mtools, which is
what genimage's `vfat` type uses). FAT32 is also readable by every PC and Mac
out of the box (exFAT needs an update on XP/Vista). 128 MiB keeps the image
(772 + 128 = 900 MiB) within a 1 GB card; use 256 MiB if 1 GB cards are
dropped. It holds the folder skeleton (`roms/<system>/` from the same
`rom-folders` list, `bios/`, `saves/`, `states/`) and `README.txt`.

**First boot: convert to full-size exFAT without losing files. Chosen method:
a verified tar backup in the raw free space at the end of the card, with an
on-disk marker, then reformat and restore.** Reasons:

| Option | Verdict |
|---|---|
| Copy the files to tmpfs, mkfs, copy back | Fits (seed <= 128 MiB, 1 GB RAM) but **a power cut between mkfs and the end of the copy loses the files**. The first boot is exactly when users pull the power ("black screen, it hung"). Rejected. |
| Create the partition with its final size at build time (sparse image) | The card size is unknown at build time. Rejected. |
| Leave the seed and add another partition for the rest | Two data drives; pre-1703 Windows only sees the small one; the UI must merge two ROM roots forever. Rejected. |
| Keep the small filesystem (current) | Wastes the card. Rejected. |
| **Raw tail backup + marker (chosen)** | The free space after the seed is not in any partition yet, so writing there is harmless. At every instant either the FAT32 seed is intact (before mkfs) or a verified backup and its marker are (until the marker is cleared), so a power cut at any point is recoverable on the next boot. Cost: only when the user copied files before the first boot, at most 128 MiB written 2x + read 1x (~30-60 s on a slow card), with a message. |

`mkfs.exfat` (exfatprogs 1.2.9, quick format, the default) only writes the
metadata at the start of the volume (`mkfs.c`: it zeroes up to the root
directory cluster; no `--full-format`, no BLKDISCARD), and the Linux exFAT
allocator fills clusters from the start, so a backup at the far end of the
card survives the mkfs and the restore as long as the volume is large enough:
require `restore_size + backup_size + margin < new_volume_size` (below). If it
does not fit (a 1 GB card with a full seed) the FAT32 seed is kept as it is and
the UI offers "Storage > Expand data partition" later (move files first).

### 1.4 Change list for the build owner

Files: `genimage.cfg`, `post-image.sh`, `post-build.sh`, `retrostone2_defconfig`,
`linux.fragment`, `rootfs-overlay/usr/libexec/rsos/data-partition`. None edited
by me.

**C1. Seed partition = FAT32 128 MiB with skeleton + README**
- defconfig: `BR2_PACKAGE_HOST_DOSFSTOOLS=y`, `BR2_PACKAGE_HOST_MTOOLS=y`.
- `post-image.sh`, replacing the exFAT seed:
  ```sh
  SEED="$BINARIES_DIR/data.vfat"
  rm -f "$SEED"
  "$HOST_DIR/sbin/mkfs.vfat" -F 32 -n RETROSTONE -C "$SEED" $((128 * 1024)) > /dev/null
  MT="$HOST_DIR/bin"
  "$MT/mmd" -i "$SEED" ::roms ::bios ::saves ::states ::screenshots ::themes
  for s in $(cat "$TARGET_DIR/usr/share/rsos/rom-folders"); do
  	"$MT/mmd" -i "$SEED" "::roms/$s"
  done
  # README with CRLF line ends for old Notepad
  sed 's/$/\r/' "$TARGET_DIR/usr/share/rsos/data-README.txt" > "$BINARIES_DIR/README.txt"
  "$MT/mcopy" -i "$SEED" "$BINARIES_DIR/README.txt" ::README.txt
  ```
  (`mkfs.vfat -C` takes the size in KiB. FAT32 on 128 MiB gives 512-byte
  clusters; fine for a seed.)
- `genimage.cfg`: `partition data { partition-type = 0x0c  image = "data.vfat"
  offset = 772M }`, still listed first. Update the header comment and
  build.md ("64 MiB exFAT" -> "128 MiB FAT32").
- `README.txt` source: `frontend/src/transfer/data-README.txt`, installed by
  the frontend package as `/usr/share/rsos/data-README.txt`
  (`make -f transfer.mk transfer-install DESTDIR=$(TARGET_DIR)`, or the
  equivalent line in the main Makefile's install rule).

**C2. `data-partition`: junk-aware empty test + safe conversion + README**
- "Empty" = no regular file outside this ignore list (case-insensitive, any
  depth): `README.txt` at the root, `System Volume Information/`,
  `$RECYCLE.BIN/`, `RECYCLER/`, `.Spotlight-V100/`, `.fseventsd/`,
  `.Trashes/`, `.TemporaryItems/`, `._*`, `.DS_Store`, `Thumbs.db`,
  `desktop.ini`, `IndexerVolumeGuid`, `WPSettings.dat`. Directories never
  count. (The scanner and the web share already ignore the same names:
  `tr_is_junk()` in `tr_util.c`.)
- Empty seed (the common case): as today, grow entry 1 (`echo ',+,7' | sfdisk
  --no-reread -N 1 /dev/mmcblk0`, note the type change to 0x07), `partx
  --update --nr 1`, `mkfs.exfat -L RETROSTONE`, recreate the layout, copy
  `/usr/share/rsos/data-README.txt` to `/data/README.txt`.
- Seed with user files: the tail backup. POSIX sh sketch (BusyBox `tar`,
  `dd conv=fsync`, `cksum` are all enabled in the current BusyBox config):
  ```sh
  DISK=/dev/mmcblk0; PART=${DISK}p1; MNT=/data; MAGIC=RSOSBK01
  total=$(cat /sys/class/block/mmcblk0/size)                  # 512-byte sectors
  start=$(cat /sys/class/block/mmcblk0p1/start)
  hdr_mib=$((total / 2048 - 1))                               # last whole MiB: marker
  read_marker() { dd if=$DISK bs=1M skip=$hdr_mib count=1 2>/dev/null | head -c 64 | tr -d '\0'; }

  convert_with_backup() {                                     # FAT32 seed mounted on $MNT
  	used_mib=$(( $(du -sk "$MNT" | cut -f1) / 1024 + 1 ))
  	nfiles=$(find "$MNT" -type f | wc -l)
  	bk_mib=$(( used_mib + nfiles / 1024 + 8 ))              # tar: 512 B header + padding per file
  	new_mib=$(( (total - start) / 2048 ))
  	restore_mib=$(( used_mib + nfiles / 8 + 64 ))           # <= 128 KiB exFAT cluster slack per file
  	[ $((restore_mib + bk_mib + 2)) -lt "$new_mib" ] || return 1   # too small: keep the seed
  	bk_off=$(( hdr_mib - bk_mib ))
  	tar -C "$MNT" -cf - . | dd of=$DISK bs=1M seek=$bk_off conv=fsync 2>/dev/null || return 1
  	n=$(dd if=$DISK bs=1M skip=$bk_off count=$bk_mib 2>/dev/null | tar -tf - | grep -vc '/$')
  	[ "$n" -eq "$nfiles" ] || return 1                      # verify before touching the seed
  	sum=$(dd if=$DISK bs=1M skip=$bk_off count=$bk_mib 2>/dev/null | cksum | cut -d' ' -f1)
  	printf '%s %s %s %s\n' $MAGIC $bk_off $bk_mib $sum | dd of=$DISK bs=1M seek=$hdr_mib conv=sync,fsync 2>/dev/null
  	umount "$MNT" && format_full && restore_backup
  }
  format_full() {                                             # entry 1 -> end of card, exFAT
  	echo ',+,7' | sfdisk --quiet --no-reread -N 1 $DISK && partx --update --nr 1 $DISK &&
  	mkfs.exfat -L RETROSTONE $PART > /dev/null && mount -t exfat -o noatime $PART $MNT
  }
  restore_backup() {                                          # marker present, exFAT mounted
  	set -- $(read_marker)
  	[ "$1" = $MAGIC ] || return 1
  	s=$(dd if=$DISK bs=1M skip=$2 count=$3 2>/dev/null | cksum | cut -d' ' -f1)
  	[ "$s" = "$4" ] || { echo "rsos-data: backup damaged, kept at the end of the card"; return 1; }
  	dd if=$DISK bs=1M skip=$2 count=$3 2>/dev/null | tar -C $MNT -xf - && sync &&
  	dd if=/dev/zero of=$DISK bs=1M seek=$hdr_mib count=1 conv=fsync 2>/dev/null
  }
  ```
  Boot order: **first** check the marker. If it is present, the previous boot
  was interrupted after the backup was verified: if entry 1 does not reach the
  end of the card or does not mount as exFAT, run `format_full`; then
  `restore_backup` (tar overwrites, so repeating it is harmless). Only then run
  the normal "needs growing" logic. The marker is cleared only after the
  restore and a `sync`. TODO(hw): time it with a full 128 MiB seed on a slow
  card and show a "Preparing SD card" message (requirements §6 "first boot").
- Kernel/userspace check: nothing new (`sfdisk`, `partx`, `mkfs.exfat`, `tar`,
  `dd`, `cksum` are already in the image).

**C3. Root slots and the "format this disk?" prompt**
- Keep 0x83 for now and test on Windows 10 22H2 and Windows 11 24H2 (fresh card,
  USB reader and a laptop's built-in SD slot): does each ext4 slot get a drive
  letter and a format prompt? TODO(hw).
- If it does, try `partition-type = 0x27` for rootfs-a and rootfs-b (and in the
  updater's slot writes). Nothing on the Linux side reads the type
  (`boot.cmd` uses `load mmc 0:${rsos_part}`, the kernel uses
  `root=/dev/mmcblk0pN`). If 0x27 hides them, keep it; Disk Management then
  also refuses to format them casually. Else document "click Cancel" (already
  in `README.txt`).

**C4. `/media` for the USB mounts** (read-only root): in `post-build.sh`,
`rm -rf "$TARGET/media"; ln -s /run/media "$TARGET/media"`. The frontend
creates `/media/usbN` under the `/run` tmpfs that rcS already mounts.

**C5. Kernel options** (`linux.fragment`):
```
CONFIG_USB_STORAGE=y        # USB sticks and card readers (built in: no udev to load it)
CONFIG_USB_UAS=y            # USB 3 SSD enclosures (optional, small)
CONFIG_NTFS3_FS=m           # NTFS sticks, read-only use; the kernel loads it itself
                            # on the first ntfs3 mount (request_module -> /sbin/modprobe)
CONFIG_NTFS3_LZX_XPRESS=y   # files compressed with "compact" on Windows
# already there: CONFIG_SCSI, CONFIG_BLK_DEV_SD, CONFIG_VFAT_FS, CONFIG_EXFAT_FS,
# CONFIG_NLS_UTF8, CONFIG_NLS_CODEPAGE_437, CONFIG_MSDOS_PARTITION, CONFIG_EFI_PARTITION
```
`USB_STORAGE=y` costs no boot time without a device (USB storage probing is
asynchronous and `delay_use` only applies when a stick is present). Also: the
micro-USB port has no data lines, so `CONFIG_USB_MUSB_DUAL_ROLE`/gadget support
is dead weight (`# CONFIG_USB_MUSB_HDRC is not set` if nothing else needs it;
TODO(hw) check the schematic once more).

**C6. FAT32 data partition (user reformatted it)**: already mounted as vfat.
Nothing else to do; the web share answers 413 "Too big for FAT32" for files
over 4 GiB and the import reports "File too large" for them.

---

## 2. USB stick in the console

Code: `usb.c` (detection, mount, remount), `import.c` (library search, plan
and copy), `backup.c` (export games, back up saves), `sysmap.c` (system names
and aliases). UI: `ui/transfer_ui.c`.

When a stick is plugged in (or found at boot), a dialog asks **"USB drive
"GAMES" connected (exFAT, 32 GB). What do you want to do?"** with **Import
games** / **Export games** / **Back up saves** / **Nothing** (B = Nothing),
for any stick, empty or not (§4.1). Settings > Storage has the same three
actions.

### 2.1 Detection and mount (no udev)

- A `NETLINK_KOBJECT_UEVENT` socket (kernel messages only) marks the state
  dirty when a `SUBSYSTEM=block` uevent arrives; the truth is always re-read
  from `/sys/block` (a readdir of ~10 entries). If the socket cannot be opened,
  `/sys/block` is polled every second instead.
- A disk is USB when its `/sys/block/<disk>` link goes through `/usb` (so the SD
  card `mmcblk0` and the Pro's SATA disk are never touched).
- A new disk is left alone for 700 ms to settle. Then each partition (or the
  whole disk if it has no partition table) is probed by reading its boot
  sector: FAT12/16/32 (with a BPB sanity check, so a bare MBR is not taken for
  FAT), exFAT (label read from the root directory entry), NTFS. ext4 sticks
  are not mounted (untrusted ext4 images are a kernel attack surface and
  Windows users do not have them).
- Mount **read-only**, `nosuid,nodev,noexec,noatime` at `/media/usbN`:
  vfat `utf8=1,shortname=mixed`, exfat `iocharset=utf8`, ntfs3
  `iocharset=utf8` (legacy `ntfs` name tried if ntfs3 is missing). Read-only
  means pulling the stick out is always safe for the stick.
- A pulled stick is lazily unmounted (`MNT_DETACH`) and reported as
  `TRANSFER_USB_REMOVED`. An empty USB card reader is ignored until a card
  arrives (its `change` uevent). `transfer_usb_eject()` unmounts (fails with
  `-EBUSY` while an import reads from it).
- `transfer_usb_remount(mp, true/false)`: `MS_REMOUNT` read-write for an
  export or a saves backup, then read-only again right after (after a
  `sync()`), so the stick is writable only while we write to it.
- `transfer_usb_timeout_ms()`: when the main loop must call
  `transfer_usb_poll()` without a uevent (a disk in its settle delay: 200 ms).
  A stick already plugged at boot produces no uevent after the socket is
  opened; without this the mount waited for the next unrelated wake-up of the
  loop (the power module's 10 s battery poll).
- Events: `MOUNTED`, `REMOVED`, `EJECTED`, `UNSUPPORTED` (USB disk with no
  FAT/exFAT/NTFS partition: "This drive's format is not supported, use FAT32 or
  exFAT"), `MOUNT_FAILED` (with errno).
- Log lines (frontend.log, one per event): `usb: disk sda: "SanDisk Cruzer",
  29.8 GB, 1 partition`, `usb: sda1 (vfat, "GAMES", 29.8 GB) mounted
  read-only on /media/usb0` (or the mount error, or "no FAT, exFAT or NTFS
  filesystem"), `usb: sda1 removed`, `usb: eject /media/usb0 (sda1): done`,
  `usb: /media/usb0 remounted read-write: ok`; the UI adds `usb: GAMES
  (/media/usb0): dialog shown`, or `dialog suppressed: usb_import_prompt=0
  (toast only)` / `a copy is running` / `the dialog for ... is open`, or
  `dialog deferred: a game is running` / `the menu is not up yet`, then the
  answer, the plan summary, the start and end of every copy (counts, bytes,
  duration, MB/s) and every duplicate answer.

### 2.2 What the import finds

**Game libraries anywhere on the stick** (`transfer_find_trees()`, in a
thread). The stick root and every folder up to 4 levels below it (hidden
folders, `System Volume Information`, `$RECYCLE.BIN`, `LOST.DIR`,
`Android`, `DCIM`... skipped) is a library when it holds:
- `roms/<known system>/` with at least one file, or
- known system folders or aliases with files (`snes/`, `Genesis/`, `Game Boy
  (GB)/`), or
- a RetroStone2 backup: a `.rsos-backup` or `RetroStone2-backup.txt` marker,
  or the name `RetroStone2` / `RetroStone2-*` with `roms/`, `saves/`,
  `states/` or `bios/` (§2.4), or
- (the root only) `bios/`, `saves/`, `states/` or `themes/` with files.

The folders a library consumes (`roms`, system folders, `bios`, `saves`,
`states`, `themes`, `screenshots`, `rsos`) are not searched again; its other
subfolders are (a stick with `roms/` and `Backups/RetroStone2/` has two
libraries). Nested layouts are found as libraries of their own:
`RetroPie/roms/snes` makes `RetroPie` a library, `home/pi/RetroPie` too.
Each gets a summary (systems, games, size; `gamelist.xml` and scraped media
are not counted as games). None found: the stick's top is planned (BIOS files
or nothing). One: straight to its plan. **Several: a picker** lists each
(path, "N systems, M games, size", "(backup)") plus **All**, and the plan
covers the chosen ones (`transfer_plan_build_trees()`: each library's own
`roms/`, system folders, `bios/`, `saves/`, `states/`, `themes/`,
`screenshots/`, `rsos/coreopts/`, `rsos/remaps/`; the same destination from
two libraries: the first wins).

Layouts read by `transfer_plan_build()` from one root (the API kept for tools
and tests; all names case-insensitive, several at once are merged):
`roms/<sys>/`, `RetroPie/roms/<sys>/`, `retropie-mount/roms/<sys>/` (RetroPie's
USB ROM service), `share/roms/`, `home/pi/RetroPie/roms/`, `userdata/roms/`,
and system folders at the root of the stick. BIOS: `bios/`, `RetroPie/BIOS/`,
`retropie-mount/BIOS/`, `share/bios/`, `home/pi/RetroPie/BIOS/`,
`userdata/bios/`. Also `saves/<sys>/`, `states/<sys>/` (a backup made with our
layout) and `themes/`.

System folder aliases (`transfer_system_canon()`): the canonical id, then
aliases compared on `[a-z0-9]` only, then a MinUI/Onion "(TAG)" suffix:
`genesis`/`md` -> megadrive, `sfc`/`superfamicom` -> snes, `fc`/`famicom` ->
nes, `sg-1000` -> sg1000, `sms`/`ms` -> mastersystem, `gg` -> gamegear,
`32x`/`THIRTYTWOX` -> sega32x, `megacd` -> segacd, `ps`/`ps1` -> psx,
`pce`/`tg16` -> pcengine, `mame`/`mame-libretro`/`mame2003`/`mame2003-plus` ->
arcade, `fba`/`fbalpha` -> fbneo, `Game Boy (GB)` -> gb... Folders with no
match (e.g. `dreamcast`) are listed as "not supported" with their file count.
The canonical list must match the UI's system table and
`/usr/share/rsos/rom-folders` (it includes `pico`).

Classification of files inside a system folder:
- `.srm .sav .rtc .eep .sra .fla .mpk .mcr` -> `/data/saves/<sys>/<name>`
  (RetroPie kept saves next to the ROMs).
- `.state`, `.stateN`, `.state.auto` (+ `.png`) -> `/data/states/<sys>/<name>`.
- Everything else (ROMs, `gamelist.xml`, Skraper `media/`) ->
  `/data/roms/<sys>/<same relative path>`.
- Never copied: dotfiles and `._*`, `.DS_Store`, `Thumbs.db`, `desktop.ini`,
  `System Volume Information`, `$RECYCLE.BIN`, `__MACOSX`, symlinks, devices.
  Names that exFAT/Windows cannot store get `_` for `\ : * ? " < > |` and
  control characters (Latin-1 names from old sticks too).
- Two source folders for one system (`genesis` and `megadrive`) with the same
  file: the first one found wins.

### 2.3 Copy rules (import: stick -> console)

| On /data | Game, BIOS, theme, screenshot | Save, state, settings file |
|---|---|---|
| missing | copied | copied |
| same size and mtime (+-2 s, FAT resolution) | **already there**: skipped silently | skipped silently |
| same size, other mtime | 16 samples of 64 KiB compared at plan time (files up to 1 MiB in full); samples differ: **asked**; samples equal: compared in full while copying, identical = already there (and re-stamped), else **asked** | compared in full at plan time: identical = already there, else **asked** |
| other size | **asked** | **asked** |

**Asked**: the import thread stops on the file and the UI shows "This game
is already on the console, but different: <name> / USB drive: size, date /
Console: size, date" with **Skip** / **Replace** / **Skip all** / **Replace
all** (B = Skip). For a save, state or settings file the text adds "Saves are
precious: if you replace it, the console's copy is kept as <name>.bak."
"All" is remembered for the rest of that import, separately for saves and
for everything else (Replace all on games never replaces a save). A replaced
save keeps the old one as `<name>.bak`; games are overwritten. The `.bak`
files of a backup on the stick are never imported. The summary: "Imported N,
already there M, skipped K, replaced R." The plan screen shows "Already on
the console: M files, skipped" and "Different on the console: N (saves too):
you will be asked"; its Saves toggle ("Saves & save states", default on)
restores the saves and states found in the chosen libraries.

API: `transfer_import_opts.dup_files` / `dup_saves` (`TRANSFER_DUP_ASK`
default, `SKIP`, `REPLACE`; the legacy `overwrite_saves` = REPLACE for
saves). Synchronous runs ask `opts.ask` (NULL: games replaced, saves kept,
the old behaviour); the async import sets `progress.asking` +
`progress.question` (path, kind, sizes, dates, `seq`) and waits for
`transfer_import_answer()`; cancel wakes it. Progress counters: `copied`
(written) of which `replaced`, `identical`, `kept`, `conflicts_kept` (saves
kept), `failed`, `elapsed_ms` (time spent asking excluded).

Every file is written as `.<name>.rsos-part`, `fsync`ed, its mtime set to the
source's, renamed, and the directory `fsync`ed (once per folder, see below).
Cancel removes the partial file.

#### Copy engine (2026-09-27)

Hardware log `2026-09-27f`: 66 games, 1.1 GB from a USB 2.0 stick (vfat) to
the exFAT `/data` in 298 s, **3.9 MB/s**. The old engine read 256 KiB,
wrote it, and every 8 MiB stopped for an `fdatasync` (on exFAT each one also
flushes the whole device: `exfat_file_fsync()` calls `sync_blockdev()`);
reading and writing never overlapped, and after **every** block it dropped
the whole source file from the page cache (`POSIX_FADV_DONTNEED` over
`0, 0`), throwing away the readahead the kernel had just done beyond it. Now
(`tr_util.c`, used by the import, the export and the saves backup):

| | Before | After |
|---|---|---|
| Blocks | 256 KiB, read then write | 1 MiB, **4 buffers**: a reader thread fills them while the caller writes (`tr_copy_file()`); files up to 1 MiB without the thread |
| Source | `DONTNEED` of the whole file per block | `POSIX_FADV_SEQUENTIAL` (double readahead), `DONTNEED` of the **consumed** range every 8 MiB |
| Next file | read when its turn comes | its first 16 MiB are read into the page cache by a short thread as soon as the current file is read (`tr_copier_hint_next()`), while the current one is written and flushed (`POSIX_FADV_WILLNEED` is clamped to the 128-256 KiB readahead window, so it is a real read) |
| Destination writeback | `fdatasync` every 8 MiB (the copy stops) | write-behind every 4 MiB: `sync_file_range(WRITE)` starts the new chunk, `WAIT` on the previous one, `DONTNEED` it; at most ~8 MiB dirty, the card always busy; `fdatasync` fallback where unsupported |
| Allocation | cluster by cluster | reserved at open: `fallocate(KEEP_SIZE)` (FAT, ext4...), on exFAT (no fallocate) `ftruncate` to the final size, which allocates without zero-filling (`valid_size` stays 0) and fails at once with `ENOSPC`; trimmed at commit if fewer bytes came |
| Flushes per file | `fsync` + folder `fsync` | `fsync` once at the end; the folder `fsync` once per folder (when the next file goes elsewhere, and at the end); a replaced save (`.bak`) still gets it at once. A power cut leaves the old file or the new one, at worst the hidden temp file of the last renames (copied again next time) |

Each folder group gets a throughput line, and the summary the breakdown:

```
transfer: import roms/n64: 4 files, 256 MB in 28.1 s = 9.1 MB/s (waiting for the source 1.3 s, writing 0.2 s, flushing 26.6 s)
transfer: import done: imported 92, ...; 555 MB in 62.5 s (8.9 MB/s; waiting for the source 3.0 s, writing 0.5 s, flushing 59.0 s)
```

"waiting for the source" = the stick is the limit, "flushing" = the card
(write-behind waits and fsyncs), "writing" = copying into the page cache.

**Measured on the host** (WSL, `scratchpad/fix3/emu.sh` + `nbdemu.py`: two
NBD devices served by a Python model that runs the requests one at a time,
each costing latency + bytes / bandwidth: the "stick" 20 MB/s + 0.5 ms per
120 KB command (usb-storage's `max_sectors` 240), the "card" 20 MB/s reads, 10
MB/s writes + 2 ms per request; `dd` gives 17.7 MB/s from the stick and 9.5 MB/s
to the card). A FAT32 stick image with 92 files, 555 MB (4 x 64 MB, 2 x 100 MB,
16 x 4 MB, 30 x 1 MB, 40 x 128 KB) imported into an empty exFAT card with the
real import code (`trbench`), caches dropped:

| Engine | Time | Throughput | 64-100 MB files | 4 MB | 1 MB | 128 KB |
|---|---|---|---|---|---|---|
| before | 98.0 s | **5.7 MB/s** | | | | |
| after | 62.5 s | **8.9 MB/s** (93 % of the card's 9.5) | 9.0-9.1 MB/s | 8.6 MB/s | 8.3 MB/s | 5.4 MB/s |

(Intermediate steps: pipeline + write-behind 7.7 MB/s, + folder fsync per
folder 8.1, + reading the next file ahead 8.9.) The copy is now bound by the
card's write speed; the device's own figure will depend on that card.
TODO(hw): the MB/s lines of an import on the unit.

**Verify pass** (identical-file detection) needed no change: same size and
mtime is "already there" without reading; same size and another mtime reads
16 samples of 64 KiB at plan time and compares in full only when they all
match (games) or for saves (small). `ENOSPC`, `EIO`, `ENODEV` stop the
import ("The SD card is full", "Read error: was the USB drive removed?");
other per-file errors are counted and the import goes on. The plan keeps 64 MiB
free on /data for saves (`plan->fits`; differing files count in
`bytes_to_copy` since they may be replaced).

### 2.4 Export games, back up saves (console -> stick)

Both write into **`RetroStone2/`** at the stick root (created the first
time), in the layout the import reads, so any RetroStone2 can import it back
(it is found as a library, marked "(backup)"):

| Action | Copies | Options |
|---|---|---|
| **Export games** | `roms/<system>/...` (the console's folders as they are, `gamelist.xml` and media included) | **BIOS files too** (`bios/`, default on) |
| **Back up saves** | `saves/<system>/...`, `states/<system>/...` (the host's `.srm .rtc .state .stateN .state.auto` and `.png` thumbnails) | **Games**: All games (default) / Last played: <name>; **Settings too** (`rsos/coreopts/` core options per core and per game, `rsos/remaps/`; default off) |

- **Incremental**: a file is copied only when it is missing or different on
  the stick. Games and BIOS: same size and mtime within 2 s (the copy keeps
  the console's mtime) = identical. Saves, states and settings are small and
  precious: compared by content whatever the dates. The screens show "Already
  on the drive: M files" and "To copy: N files, size (free on the drive)",
  the progress "... - M already there", the summary "Copied N files (X MB),
  already there M." A second export after adding one game copies one file.
- **Never deletes** anything on the stick (a game removed from the console
  stays in the backup). A save or settings file replaced on the stick keeps
  the previous stick version as `<name>.bak` (one; the next replacement
  rotates it).
- **Last played game**: the most recent entry of gamedb; its files are
  `saves/<system>/<stem>.*`, `states/<system>/<stem>.*` and, with settings,
  `rsos/coreopts/<core>/<stem>.ini` and `rsos/remaps/<system>[-cz]/<stem>.ini`
  (`<stem>` = the ROM file name without its extension, the host's naming;
  TODO: a zip whose inner file has another name uses that name). The summary
  lists what was copied.
- **FAT32**: files over 4 GiB - 1 cannot be written; the export screen says
  "N files over 4 GB: FAT32 can't hold them" with their names, skips them,
  and `RetroStone2-backup.txt` lists them. exFAT and NTFS sticks have no limit.
- **Space**: needed = every file to copy rounded up to the stick's cluster
  (+ a cluster/8 per file, folders, 1 MiB margin); "Not enough space" disables
  the button. A full stick anyway (other writers, FAT slack): the copy stops,
  the partial file is removed, the complete ones stay, "Stopped: The USB
  drive is full. The partial file was removed. Copied N files...".
- **Safety**: remount read-write only around the copy (`transfer_usb_remount`),
  read-only again right after (a `sync()` first), then EJECT / OK. Temp name +
  `fsync` + rename + `fsync(dir)` per file (the sink), `syncfs` at the end: a
  cancel, a full or a pulled stick leaves only complete files.
- **Markers** (rewritten at the end of every copy): `.rsos-backup`
  (`format=1`, `console=rs2-xxxxxxxx` from `/data/rsos/console-id`, made
  once, `created=`, `updated=`, `games_updated=`, `saves_updated=`, `last=`,
  `status=complete|INCOMPLETE (...)`), and `RetroStone2-backup.txt` (the same
  for humans, CRLF, "To restore: ... choose Import games").
- API: `transfer_backup_scan(data, stick, fstype, TRANSFER_EXPORT_GAMES |
  TRANSFER_BACKUP_SAVES)` (+ `_scan_start/_poll` in a thread) compares the
  console with the stick (a `stat` per file, the content of same-size saves);
  `transfer_backup_totals(b, opts)` gives files/bytes to copy, identical,
  too big, space needed, fits (cheap: the options screen recomputes it on
  every toggle); `transfer_backup_run` (sync) or `_start/_status/_cancel/
  _finish` (async, exclusive with an import).

### 2.5 "Play from USB without copying": worth it?

**Yes as a P2, it is cheap, but not for v1.** The stick is already mounted
read-only and `transfer_find_rom_dirs()` returns its system folders (same
layouts and aliases). The UI would add them as extra ROM roots while the stick
is present (saves still go to `/data/saves/<sys>/`, keyed by the ROM base
name, so they survive). Costs: the game list must merge two roots and drop
entries on `REMOVED`; pulling the stick during a disc game (PSX, Mega-CD,
which stream from the file) crashes the core child process; USB 2.0 random
reads are fine. Cartridge games are loaded into RAM, so they survive a pull.

### 2.6 API use (UI thread)

```c
/* after the first frame */
struct transfer_usb_config uc = { 0 };           /* defaults: /media, /sys/block, /dev */
transfer_usb_init(&uc);
/* add transfer_usb_fd() to the main loop's poll set (POLLIN) and
 * transfer_usb_timeout_ms() to its timeout; after poll() returns: */
ui_set_now(ui, now_ms());                        /* the events happen now */
struct transfer_usb_event ev;
while (transfer_usb_poll(&ev) > 0)
	ui_usb_event(ui, &ev);                       /* the dialog, toasts */

/* Import: libraries, then a plan over the chosen ones */
transfer_trees_start(mp);  /* each frame: */ transfer_trees_poll(trees, TRANSFER_TREES_MAX, &n);
transfer_plan_start_trees(roots, nroots, "/data");
/* each frame: */ if (transfer_plan_poll(&plan) == 1) show_plan(plan);
struct transfer_import_opts o;
transfer_import_defaults(&o);                    /* everything, ask about differing files */
transfer_import_start(plan, &o);                 /* takes ownership of plan */
/* each frame: */
struct transfer_progress pr;
enum transfer_state st = transfer_import_status(&pr);
if (pr.asking && pr.question.seq != last_seq)    /* Skip / Replace / Skip all / Replace all */
	ask_user(&pr.question);                      /* -> transfer_import_answer(a) */
if (st >= TRANSFER_DONE) {
	char changed[TRANSFER_SYS_MAX][TRANSFER_SYSID_MAX];
	int n = transfer_import_finish(changed, TRANSFER_SYS_MAX);
	for (int i = 0; i < n; i++)                  /* "bios" and "themes" can appear too */
		games_invalidate(cache_dir, changed[i]);
}
/* B on the progress screen: transfer_import_cancel(); */

/* Export games / Back up saves */
transfer_backup_scan_start("/data", mp, drive.fstype, TRANSFER_EXPORT_GAMES);
/* each frame: */ if (transfer_backup_scan_poll(&b) == 1) show_options(b);   /* transfer_backup_totals() */
transfer_usb_remount(mp, true);
transfer_backup_start(b, &opts);                 /* takes ownership of b */
/* each frame: transfer_backup_status(&pr) until >= TRANSFER_DONE, then: */
transfer_backup_finish();
transfer_usb_remount(mp, false);
transfer_usb_eject(ev.drive.mountpoint);         /* "Eject" button */
```

---

## 3. Over the network (only when WiFi or Ethernet is on)

### 3.1 Web upload page

`webshare.c`: a small HTTP/1.1 server written for this project (no third-party
code, no library beyond libc and pthreads), a thread per connection (max 4),
`Connection: close`, 30 s socket timeouts. The page
(`web/index.html`, 18 KB, vanilla HTML/JS, no external resources) is compiled
into the binary by `transfer.mk` (`od` + `sed`, no xxd needed).

Page features: PIN login (auto-login from the QR code's `#pin=`), destination
(Games / BIOS / Saves / States / Themes) and system picker with game counts,
"Auto" system from the file extension (ambiguous `.bin .cue .iso .chd .zip`
ask for a system; BIOS names like `scph5501.bin` go to BIOS; `.srm` and
`.stateN` dropped into a game folder go to saves/states), drag-and-drop of
files **and folders** (multi-disc games, Skraper media), file and folder
pickers (mobile: the file picker), sequential upload queue with per-file and
total progress, cancel, "already exists: replace / skip (and the next ones)"
(saves and states keep three backups: `.bak`, `.bak2`, `.bak3`; the same
file sent again replaces nothing), a filterable file list per folder with
delete (a deleted save or state takes its `.bak` files along, else the game
would load the `.bak` back), free
space bar. Dark/light follows the browser. Tested in Chromium at phone width.

HTTP API (all JSON; mutating calls need the cookie and `X-Requested-With: rsos`):

| Request | Result |
|---|---|
| `GET /` | the page (CSP: no external anything, no framing) |
| `POST /api/login` body `pin=NNNNNN` | 200 + `Set-Cookie: rsos_token=<128-bit>; HttpOnly; SameSite=Strict`; 403 wrong PIN; 429 `{"wait":N}` locked |
| `GET /api/info` | `{host, free, total, systems:[{id,name,n}], bios, themes}` |
| `GET /api/list?target=&sys=` | `{files:[{p,s,t}], truncated, free}` (recursive, max 5000) |
| `PUT /api/upload?target=&sys=&path=&overwrite=0/1&mtime=` | raw body streamed to disk; 201; 409 exists; 507 no space; 413 > 4 GiB on FAT32; 411 no Content-Length |
| `DELETE /api/file?target=&sys=&path=` | 200 (saves/states: its `.bak`, `.bak2`, `.bak3` too); 404; 409 folder not empty |

`target` is `roms|saves|states` (+ `sys` = a canonical system id) or
`bios|themes`; `path` is relative, `/`-separated, max 8 levels.

#### Web share security

The threat is another device on the same LAN (a guest's phone, a compromised
IoT box) and a malicious web page open in the user's own browser. The model:

- **Off unless the user turns it on**, and only while a network is up. The
  socket is bound in `webshare_start()` and closed in `webshare_stop()`, after
  30 min without requests (`idle_timeout_s`), and must be stopped by the UI when
  networking goes off and before a game starts.
- **LAN only**: connections from non-private IPv4 addresses are closed
  (allowed: 10/8, 172.16/12, 192.168/16, 169.254/16, 100.64/10 phone hotspots,
  127/8). IPv4 only.
- **PIN**: 6 random digits from `getrandom()`, new at every start, shown only
  on the console screen. 5 wrong PINs lock logins for 30 s, doubling to 10 min
  (brute force of 10^6 PINs would take months, and the share times out long
  before); 20 wrong PINs from any devices lock everybody out for 60 s. The
  lockouts are kept per address in a 16-entry table; a locked entry is never
  taken over by a new address (with all 16 locked, a new one waits too), so
  cycling through addresses does not reset a lock. Constant-time comparison.
- **Session**: a random 128-bit token in an `HttpOnly; SameSite=Strict` cookie
  (or `X-RSOS-Token` for scripts), valid until the share stops.
- **CSRF**: the login and every state-changing request need
  `X-Requested-With: rsos` (403 `csrf` otherwise, and a login without it is
  not counted as a guess); a foreign page cannot add it without a CORS
  preflight, which the server never answers, so it can neither change files
  nor post PIN guesses from a LAN browser to lock the owner out. **DNS rebinding**: the `Host` header must be an IPv4 literal,
  `localhost`, `retrostone` or `retrostone.local` (421 otherwise).
- **Paths**: the target folder comes from a fixed table; every name is checked
  by `transfer_name_check()` (no `.`/`..`, no leading dot, no `\ : * ? " < > |`,
  valid UTF-8, <= 255 UTF-16 units, no DOS device names; any other character,
  e.g. `żaba.gb` or `一二.gb`, is fine) after URL decoding
  (`%00` rejected); directories are opened one component at a time with
  `O_NOFOLLOW` from a `/data` directory fd, so neither `..` nor a symlink can
  escape.
- **Writes**: to a hidden temp file, `fsync`, rename, `fsync(dir)`; an aborted
  upload leaves nothing. Free space is checked against `Content-Length` + 16 MiB
  before writing. A replaced save or state keeps three generations (`.bak`,
  `.bak2`, `.bak3`), and the same bytes again replace nothing: a retried
  upload, or two saves of one name in a dropped folder, cannot push the
  console's own save out (the USB import and backup keep one `.bak`).
- **Not protected**: plain HTTP, so the PIN and the files cross the LAN in
  clear (same as the SMB guest shares of most consoles and NASes). TLS would
  need a certificate the browser does not trust. The page and the doc say "do
  not enable it on public WiFi".

### 3.2 Name resolution: tiny responder vs Avahi

`netnames.c`, one thread asleep in `poll()`:
- **mDNS** (UDP 5353, 224.0.0.251): answers A queries for `retrostone.local`
  (normal, QU and legacy-unicast queries), announces on each interface when it
  appears.
- **LLMNR** (UDP 5355, 224.0.0.252): `retrostone` -> A, what Windows uses for
  single-label names like `\\RETROSTONE`.
- **NetBIOS name service** (UDP 137): `RETROSTONE<00>` / `<20>` -> A, for
  Windows with LLMNR disabled and older clients.

Avahi would bring `avahi-daemon` + libdaemon (dbus and expat are already in the
image for BlueZ), ~0.5 MB and a resident daemon with probing and DNS-SD. The
only thing we lose is DNS-SD service advertisement (`_http._tcp`,
`_smb._tcp`), i.e. automatic listing in macOS Finder's "Network" sidebar; the
responder can grow PTR/SRV/TXT records later if wanted (P2). No conflict
detection: two consoles on one LAN both answer `retrostone`. TODO: a hostname
setting (`hostname=`), or a default suffix from the MAC. The **IP URL and the QR
code stay the primary way** in (Android's `.local` support varies).

### 3.3 SMB share `\\RETROSTONE`

| | in-kernel ksmbd + ksmbd-tools | Samba 4 |
|---|---|---|
| Size | `ksmbd.ko` (~0.5-0.7 MB, module) + ksmbd-tools ~0.3 MB; its deps libglib2 and libnl are **already in the image** (BlueZ, wpa_supplicant) | ~40 MB+ installed, Python needed at build time, many libraries |
| RAM / CPU | kernel threads + one small daemon; zero-copy file I/O, the best throughput on a 1 GHz A7 | several daemons, tens of MB RSS |
| Start/stop | `modprobe ksmbd` + `ksmbd.mountd` in ~100 ms; `ksmbd.control --shutdown` | seconds |
| Maturity | in mainline since 5.15; several remote CVEs in 2022-2023 (fixed in 6.x LTS). Acceptable only because it runs just while enabled, on the LAN, with authentication | reference implementation |
| Buildroot | `BR2_PACKAGE_KSMBD_TOOLS` (3.5.2) exists | `BR2_PACKAGE_SAMBA4` exists |

**Recommendation: ksmbd, as an opt-in "Windows file share" toggle next to the
web share, in a second step.** Not packaged by me: it needs kernel-fragment
and defconfig changes (not my paths) and hardware testing. Exactly:

- `linux.fragment`: `CONFIG_SMB_SERVER=m` (it selects NLS_UTF8, CRYPTO_CMAC,
  CRYPTO_CCM, CRYPTO_GCM, CRYPTO_ECB, the MD5/SHA256/SHA512/ARC4 libs, ASN1,
  OID_REGISTRY; it needs INET, MULTIUSER, FILE_LOCKING, all already on).
  `# CONFIG_SMB_SERVER_SMBDIRECT is not set`.
- defconfig: `BR2_PACKAGE_KSMBD_TOOLS=y`.
- **No guest access**: Windows blocks insecure guest logons on its
  Enterprise/Education editions since Windows 10 1709 and on Pro since
  Windows 11 24H2, and 24H2 requires SMB signing by default, which guest
  sessions cannot do ([Microsoft](https://techcommunity.microsoft.com/blog/filecab/accessing-a-third-party-nas-with-smb-in-windows-11-24h2-may-fail/4154300)).
  Use user `retrostone` with its own on-screen password (new at every start),
  signing on. Not the web PIN: 6 digits are fine behind the web share's
  lockout, but ksmbd only waits 5 s per failure after 10 of them.
- Config in `/run/ksmbd/` (read-only root), started by the frontend (or a
  `rsos-smb` helper script) only while enabled:
  ```sh
  mkdir -p /run/ksmbd
  cat > /run/ksmbd/ksmbd.conf <<EOF
  [global]
  	netbios name = RETROSTONE
  	server min protocol = SMB2_10
  	server signing = mandatory
  	map to guest = never
  	restrict anonymous = 2
  	max connections = 8
  [roms]                           ; and saves, states, bios, themes: never /data
  	path = /data/roms
  	read only = no
  	force user = root
  	force group = root
  	store dos attributes = no      ; exFAT has no xattrs
  	veto files = /.*/System Volume Information/$RECYCLE.BIN/
  EOF
  ksmbd.adduser -C /run/ksmbd/ksmbd.conf -P /run/ksmbd/ksmbdpwd.db -a retrostone -p "$PASSWORD"
  modprobe ksmbd && ksmbd.mountd -C /run/ksmbd/ksmbd.conf -P /run/ksmbd/ksmbdpwd.db
  # stop: ksmbd.control --shutdown; modprobe -r ksmbd; rm -rf /run/ksmbd
  ```
  TODO(hw): check these option names against ksmbd-tools 3.5.2's
  `ksmbd.conf(5)` (the CLI flags `-C/-P/-a/-p` are from its sources), mapping
  to `root` via `force user`, and throughput. `\\RETROSTONE` is resolved by the
  LLMNR/NBNS responder; the Explorer "Network" view would also need
  WS-Discovery (wsdd, Python) which we skip: users type `\\RETROSTONE` or
  `\\192.168.x.y` as shown on screen.

**Implemented (batch 2, 2026-09-27).** The kernel option and ksmbd-tools 3.5.2
are in the image (`CONFIG_SMB_SERVER=m`, `BR2_PACKAGE_KSMBD_TOOLS=y`; rcK
already shuts ksmbd down at power-off). The helper `/usr/bin/rsos-smb`
(`frontend/src/transfer/rsos-smb`, POSIX sh, installed by the frontend's
`make install`):

- `rsos-smb start PASSWORD [NAME]`: writes `/run/ksmbd/ksmbd.conf` (the
  options above plus `max connections = 8`; one share per folder, `roms`,
  `saves`, `states`, `bios`, `themes` = `/data/<folder>` (made if missing),
  each with `valid users = retrostone`, `guest ok = no`; netbios name = the
  console name in capitals), makes the user database with `ksmbd.adduser -C
  conf -P /run/ksmbd/ksmbdpwd.db -a -p PASSWORD retrostone`, `modprobe ksmbd`,
  `ksmbd.mountd -C conf -P db`, then checks after 0.5 s that ksmbd.mountd is
  still running (it leaves at once when the kernel server does not answer).
  Prints `on`, or a message on stderr and exit 1 (no module, no /data, a
  password that is not 8+ letters/digits/`-`); nothing is left behind on a
  failure. Starting again stops the old server first (new password).
  - **Only those folders** (security review): `/data` itself is not shared,
    so `rsos/` (settings, `wpa_supplicant.conf` with the WiFi password, logs,
    `update/`, `bluetooth.img`) cannot be read or changed over SMB. Not one
    share of `/data` with `veto files = /rsos/`: ksmbd matches veto patterns
    case-sensitively against the whole path and exFAT names are not
    case-sensitive (`RSOS\settings.ini` would pass). In Explorer,
    `\\RETROSTONE` shows the five folders.
  - **No `hosts allow`**: ksmbd-tools 3.5.2 compares it with the client's
    address as a plain string (`share.c`: "FIXME Do a real hosts lookup. IP
    masks"), so `192.168.0.0/16` would refuse every client. The share is
    off by default, runs only with the web transfer, and the password (below)
    is long enough without a lockout.
- `rsos-smb stop`: `ksmbd.control -s`, `modprobe -r ksmbd`, the run folder
  removed. `rsos-smb status`: `on`/`off`.
- UI: Settings > Network > "Windows file share" (key `smb`, off; the item only
  exists when the helper is installed). When on, the share starts and stops
  with "Transfer over network"; the transfer screen shows
  `Windows: \\RETROSTONE` and `User: retrostone, Password: XXXX-XXXX`
  ("starting..." until the helper answers). The password is made by the menu
  at every start: 8 characters from `ABCDEFGHJKMNPQRSTUVWXYZ23456789` (no
  0/O, 1/I/L; about 2^39) with a `-` in the middle, typed as shown. It goes
  to the helper as an argument, and the log shows only `rsos-smb start ...`
  (hw.c logs a job's third argument only when it is `on`/`off`). A failure is
  a toast ("Windows file share: <message>"); the web transfer keeps working.
- One helper job at a time: a stop asked while the start runs (STOP right
  after opening the screen) runs when the start ends, and a start asked
  during a stop after it. A menu that crashed and restarted has `smb_state`
  0 but a share may still run: the first time the menu sees the network
  transfer off, it runs `rsos-smb stop` if `/run/ksmbd` or
  `/sys/module/ksmbd` exists (`RSOS_SMB_RUN`/`RSOS_SMB_SYSMOD` in tests).
- Verified (WSL, no ksmbd in its kernel, so partly): `make check-smb`
  (`tests/smb-test.sh`, stub commands: the configuration, the order, stop,
  failures); `make check-b2-ui` (the toggle, started with the transfer's PIN
  and console name, stopped with it, no item without the helper); and the
  image's own ARM ksmbd-tools under qemu-arm on the generated configuration:
  `ksmbd.adduser` accepts it and writes the database (add and update; note
  "Could not open converter from UTF-8 to UTF-16LE, will try UCS-2LE": the
  image has no gconv modules, the UCS-2LE fallback gives the same NT hash for
  an ASCII PIN), `ksmbd.mountd` parses it and then leaves (no kernel server),
  which `rsos-smb` reports as "ksmbd.mountd stopped at once".
  Security review (2026-09-29): `tests/smb-test.sh` checks the five shares
  (never `/data` or `rsos`), `max connections = 8`, no `hosts allow`, the
  8+ character password; `tests/b2-ui.sh` the `XXXX-XXXX` password, its
  absence from the log, the stop after a crash, and a stop during the start
  run after it. The image's ARM `ksmbd.adduser` (qemu-arm) reads the new
  configuration: it lists the five shares with user `retrostone`.
  TODO(hw): Windows 11 24H2 and 10 connecting with user retrostone + the
  on-screen password (signing), the five folders shown, copy speed, `force
  user = root` on exFAT (bringup.md 7f).

### 3.4 SFTP (advanced)

Dropbear (`BR2_PACKAGE_DROPBEAR`, ~0.2 MB) has no SFTP server of its own: it
runs an external `sftp-server` (OpenSSH's), so SFTP means "dropbear + OpenSSH's
`sftp-server` binary" (scp works with dropbear alone). Recommend
it only as "Remote access (advanced)", off by default, started only while
enabled, password set by the user (never a default), host keys in
`/data/rsos/dropbear/` (the root is read-only), and `scp`/WinSCP/FileZilla as
clients. The web page and SMB cover normal users; SFTP is for power users and
debugging. Throughput will be CPU-bound (~1-2 MB/s with AES/ChaCha on the A7).

---

## 4. UI integration (for the UI owner)

### 4.1 Screens

1. **The USB dialog** on `TRANSFER_USB_MOUNTED` (preview
   `rsos-dark-usb-dialog.png`): "USB drive "<label or vendor>" connected
   (<fs>, <size>). What do you want to do?" [IMPORT GAMES] [EXPORT GAMES]
   [BACK UP SAVES] [NOTHING] (a 2x2 grid; B = Nothing), for any stick; a stick
   with a system update package (`*.rsu` at its root or in `RetroStoneOS/`)
   also gets [INSTALL UPDATE] before [NOTHING] (docs/updates.md). While a
   game runs, or before the menu is up, it waits (`ui->usb_pending`) and shows
   when the menu is back (the main loop does not poll USB during a game: the
   uevents queue and arrive after it). Toast only (6 s: "USB drive GAMES
   connected: Settings > Storage to use it") when `usb_import_prompt=0`, or
   while a copy runs. Pulling the stick closes its dialog. `UNSUPPORTED`:
   "USB drive: format not supported (use FAT32 or exFAT)". `REMOVED` while
   importing: the import fails with a read error; the summary says so.
   **The bug this replaces** (hardware test 2026-09-27: never any "USB drive
   detected" message): the old toast and its A-to-import offer got their
   deadline (`ui->now + 8000`) from the UI clock of the last `ui_update()`,
   which runs *before* the main loop's `poll()` sleep, while USB events and
   buttons are handled *after* it. Idle, that sleep lasts until the next
   wake-up (the power module's battery poll: 10 s), so an 8 s deadline was
   already over when set and the next `ui_update()` removed the toast before
   any frame drew it. A stick present at boot hit it every time (no uevent,
   so the mount itself waited for such a wake-up); the same stale clock made
   the "USB drive ejected" toast vanish early (the clock of before the wait
   on the summary dialog). Fixes: `ui_set_now()` after `poll()`, every toast
   starts at the next `ui_update()`, `transfer_usb_timeout_ms()` in the loop,
   and a dialog that does not expire.
2. **Import games** (from the dialog and Settings > Storage):
   "Looking for games on the drive..." (library search), the **picker** when
   there are several libraries (`rsos-dark-usb-picker.png`: path, systems,
   games, size, "(backup)"; All), "Comparing with the console...", then the
   **plan** (`rsos-dark-usb-import.png`): one row per system ("12 new of 15,
   38 MB, saves"), "Not supported: dreamcast (132 files)", toggles Games /
   BIOS files / Saves & save states / Themes / Screenshots, "Already on the
   console: M files, skipped", "Different on the console: N (saves too): you
   will be asked", "To copy: up to X, Y free" (red "Not enough space" and Copy
   disabled when `!fits`), Copy / Eject. "Nothing new on this drive" when
   there is nothing to copy and nothing differs.
3. **Progress** (import, export, saves; `rsos-dark-usb-progress.png`): bar,
   "x MB of y MB - n of m files", the current file, "MB/s, about N min left
   - M already there"; B = Stop (confirm; files already copied stay). An
   import question opens the **duplicate prompt** (`rsos-dark-usb-duplicate.png`,
   `rsos-dark-usb-duplicate-save.png`). End: a summary with EJECT / OK
   ("Imported N, already there M, skipped K, replaced R." / "Copied N files (X
   MB), already there M." + skipped 4 GB files, failures, the copied names of
   a last-played backup: `rsos-dark-usb-summary.png`), after
   `transfer_import_finish()` -> `games_invalidate()` + reload, or
   `transfer_backup_finish()` + remount read-only. A copy whose progress screen
   was closed (a reload) gets it back when it asks or ends.
   **Export games** (`rsos-dark-usb-export.png`) and **Back up saves**
   (`rsos-dark-usb-saves.png`): "Comparing with the drive...", then Folder
   on the drive (RetroStone2, "(new)"), games on the console, the options
   (BIOS files too; Games: All games / Last played: <name>, Settings too),
   "Already on the drive: M files", "To copy: N files, X (Y free)" or "Nothing
   to copy: the drive is up to date", the FAT32 rows, Export / Back up, Eject.
4. **Transfer over network** (Settings > Network, enabled only when WiFi or
   Ethernet is on and has an address): starts `webshare_start()` +
   `netnames_start()`; shows `urls[0]` large, `mdns_url`, the PIN, the QR code
   of `qr_text` (`qr_encode()`, draw `m[y][x]` at 4-6 px per module with a
   4-module light border, e.g. version 3 = 29 modules -> 148 px at 4 px),
   the live status (`current` + `cur_done/cur_total`, `files_received`,
   `clients`, `lockout_s`), the A-to-A warning in small print, and Stop.
   Leaving the screen: "Keep receiving in the background?" (default: stop).
   Every ~2 s while running: `webshare_take_changes()` -> `games_invalidate()`.
5. **Settings > Storage**: free space on /data; USB drive: Import games,
   Export games, Back up saves (a drive list first when several are
   plugged), Eject USB drive, "Ask when a USB drive is plugged in"
   (`usb_import_prompt`); later "Expand data partition" (change C2's
   fallback).

### 4.2 Settings keys (`/data/rsos/settings.ini`)

| Key | Default | Meaning |
|---|---|---|
| `usb_import_prompt` | 1 | the "what do you want to do" dialog on a new stick (0: a toast only) |
| `webshare_bg` | 0 | keep the web share running after leaving its screen (never across reboots) |
| `webshare_idle_min` | 30 | idle auto-stop (0 = never) |
| `hostname` | retrostone | mDNS/LLMNR/NetBIOS name, Host check |
| `smb` | 0 | future ksmbd share (3.3) |

The web share is never started at boot, even if it was on before.

### 4.3 Rules for the main loop

- Call `transfer_usb_init()` after the first frame; add `transfer_usb_fd()` to
  `poll()` and `transfer_usb_timeout_ms()` to its timeout; after `poll()`
  returns call `ui_set_now()`, then `transfer_usb_poll()` (cheap: one
  non-blocking `recvmsg` when idle).
- Before launching a game: `webshare_stop()` and `netnames_stop()` (an upload
  would compete with the core for the CPU and the SD card), and do not start
  an import. A running import, export or backup blocks game launch ("Files
  are being copied...").
- When `rsos-net` turns the network off: `webshare_stop()`,
  `netnames_stop()`. When the IP changes (DHCP): `webshare_refresh_urls()`.
- On exit/power-off: cancel and join a running import or export/backup
  (bounded: the thread stops after its current 1 MiB block; the partial
  file is removed), then `transfer_usb_shutdown()`.
- Status structs are snapshots copied under a mutex: call the getters once
  per frame, never hold pointers into them.

### 4.4 Build

`frontend/transfer.mk` (include it from the main Makefile):
`TR_OBJS` (objects + the generated page, compiled with `$(TR_CC)`, default
`$(CC)`), `TR_INCLUDES`, `TR_LDLIBS = -lpthread`, `transfer-install`
(README for the data partition). Standalone: `make -f transfer.mk
TR_BUILD=~/rsos/transfer-build transfer-check | transfer-web-test |
transfer-arm-check`. Object sizes on ARM: ~78 KB of code and data, including the 18 KB page.

---

## 5. Tests (host, WSL, build output in `~/rsos/transfer-build`)

- **Zero warnings** with `-Wall -Wextra -Wshadow -Wformat=2
  -Wstrict-prototypes -Wmissing-prototypes`, gcc 13 host and
  arm-linux-gnueabihf-gcc 13 (`-mcpu=cortex-a7 -mfpu=neon-vfpv4`, objects and
  linked binaries), and `-std=c11 -Wpedantic`.
- Copy engine (2026-09-27, in `transfer-check`): a 13 MiB + 7 B file through
  the pipeline (content, size, mtime), the same with a larger reserve (trimmed),
  a file of exactly 4 blocks (EOF on an empty read), 100 KB and empty files
  (no thread), a source that grew since the plan, cancel after 3 MiB
  (`-ECANCELED`, no temp file), a read error in the reader thread reaching the
  writer (`-EISDIR`), the deferred folder fsync, the folder groups of the
  throughput log. Throughput on emulated devices: see "Copy engine" above.
- `transfer-check` (`tests/test_transfer.c`, 387 checks; the disk-full case needs
  root for a tmpfs, else it is skipped): name
  and path checks (traversal, DOS names, invalid/overlong UTF-8, exFAT length),
  URL decoding, aliases, `O_NOFOLLOW` directory walk against a planted
  symlink, the sink (temp file, mtime, `.bak`, abort, long names), filesystem
  probes on synthetic FAT16/FAT32/exFAT/NTFS/MBR images, a full import on temp
  dirs (layouts, aliases, duplicates, junk, symlinks, saves/states, conflicts,
  CHECK re-stamping, second run = nothing to copy, overwrite with `.bak`,
  async plan and import, cancel in the middle of a 12 MiB file without
  leftovers), USB detection on a fake `/sys/block` (settle, mount options,
  label/vendor, unsupported disk, eject, re-plug, removal, remount read-write
  then read-only, the settle timeout), **library search** (a stick with
  `roms/`, `RetroPie/roms` + `RetroPie/BIOS`, a `RetroStone2-*` backup under
  `Backups/`, a library 3 levels deep, one 5 levels deep (not found), hidden
  and `System Volume Information` copies (skipped), an empty `roms/snes`
  skeleton (not a library), an unknown system: 4 libraries with the right
  summaries; a plan over all of them and over one; async; an empty stick;
  a BIOS-only stick), the **duplicate prompt** (5 games of another size, a
  same-content file with another date (already there, no question), a 3 MiB
  file differing between two samples (asked while copying) and one differing
  in its first block (asked, found at plan time), 2 differing saves: answers
  Skip, Replace, Replace, Skip all (the next 3 games kept without asking),
  Replace all on the saves (the second replaced without asking, `.bak` kept
  for both); a second import with Replace all at the first question; the Skip
  policy without a question; the async import waiting for
  `transfer_import_answer()`; cancel while a question is open, nothing
  written), **export games and back up saves** (5 games + BIOS into
  `RetroStone2/`, markers, console id, mtimes kept, dotfiles and temp files
  left out; a second export copies nothing; a changed game, a new one, a game
  removed from the console (kept on the stick), a stick file 1 s older (FAT's
  resolution: unchanged): 2 copied, 3 unchanged, no `.bak` for games; BIOS
  off; saves without then with settings; a save changed twice: the stick's
  `.bak` holds the previous version each time; the last played game only (its
  2 changed files of 6, another game's save left alone); **round trip**: the
  stick's `RetroStone2/` found as a backup library and imported into an empty
  console gives the same `roms`, `bios`, `saves`, `states`, `rsos/coreopts`,
  `rsos/remaps` trees (`diff -r`), and imported back into the console it came
  from everything is "already there" without a question; **FAT32**: a sparse
  4 GiB + 1 file listed, skipped, named in the marker, not on the stick
  (exFAT: no limit); cancel in the middle of a 12 MiB file: no partial file,
  marker INCOMPLETE; async scan and copy; a mode mismatch refused; **full
  stick**: 5 x 1 MiB onto a 3 MiB tmpfs: "The USB drive is full", the
  complete copies identical, no partial file), mDNS/LLMNR/NBNS
  packets (incl. compression pointers and a pointer loop), and the QR encoder:
  Reed-Solomon against the ISO/thonky "HELLO WORLD 1-M" codewords, format
  words, and a round trip through an independent reader for versions 2, 3, 5
  and 9 (9 has blocks of two lengths and version information), and the web
  share lifecycle (idle auto-stop, restart, no leaked descriptors).
- `transfer-web-test` (`tests/webshare_test.sh`, 86 curl checks): page and CSP,
  login (refused without the CSRF header), wrong PIN, cookie flags, token
  header, Host check, CSRF header, 3 uploads incl. UTF-8 and sub-folder names
  (content compared), `żaba/一二.gb`, mtime kept, 409/overwrite, BIOS, save
  `.bak` generations (the same save again keeps them; a third one: `.bak2`),
  a deleted save or state takes its `.bak` files (a game's do not go), empty
  file, a **256 MiB upload** (content
  identical; ~700-870 MB/s on the host, the A20 will be network-bound), 14
  traversal/bad-name attempts (all 400), a symlink planted in `/data` (not
  followed), chunked body (411), list/delete, an **aborted upload** (no file,
  no temp left), **disk full** on a 20 MiB tmpfs (507, nothing written), the
  wrong-PIN lockout (429 with the right PIN, 30 s), and the change list.
- UI (`make check-ui`, the preview tool with `--fake-transfer`): the USB
  dialog after 9 s of idle *without* a UI update (`idle:` = the device's
  poll() sleep; the old code lost the toast there), B = Nothing, unplugged
  while open; `usb_import_prompt=0`: a toast; the picker with 3 libraries and
  All, the four duplicate answers (printed by the fake and checked), one
  library: no picker; export and saves backup: exactly 2 remounts read-write
  and 2 read-only.
- The real main loop (`make check-frontend` steps 4 and 4a; a fake stick from
  `tests/fake-usb-stick.sh`: a sysfs device behind a USB host, a FAT32 boot
  sector, the contents folder; the headless mount points `/media/usb0` at it;
  an inotify watch on the fake `/sys/block` stands in for the uevents, so the
  loop sleeps like on the device): a stick present at boot gets its dialog
  within 2.5 s of the menu (was never); hot-plugged: import with a duplicate
  (Replace), export games, back up saves (files compared on disk, the marker,
  2+2 remounts in the log), unplug with the dialog open, then power-off with
  the rcK markers.
- Real mounts (root in WSL, loop devices behind a fake `/sys/block`): a FAT32
  stick made with mtools and an exFAT stick made with Buildroot's
  `mkfs.exfat` were detected, mounted read-only with the options above,
  imported (UTF-8 names kept, `RetroPie/roms/sfc` -> `roms/snes`) and ejected.
- Name responder live on loopback (test ports): mDNS legacy unicast, LLMNR and
  NBNS all answered with the right address; other names ignored.
- The page in Chromium against the host server: login (wrong then right PIN),
  auto-routing by extension, saves routed to `saves/snes`, replace prompt,
  delete, free space.

## 6. Licences

All code in `frontend/src/transfer/` and the page were written for RetroStoneOS
(no third-party code; the QR encoder follows the public ISO 18004 algorithm
description). No new build or runtime dependency. ksmbd-tools would be
GPL-2.0+ (a separate program, fine), ksmbd is in the kernel (GPL-2).

## 7. TODO(hw)

- Windows 10 22H2 / 11 24H2 / macOS with a freshly flashed card: which
  partitions get a letter, format prompts, with root types 0x83 and 0x27 (C3).
- First-boot conversion time with a full seed on a slow card (C2).
- USB stick read speed and import throughput; NTFS stick (ntfs3 autoload
  through modprobe); a USB card reader with and without a card; a stick pulled
  during an import.
- The USB dialog on hardware: plugged at boot, hot-plugged on the carousel,
  during a game (after it), with the screen dimmed or off (TODO: should a new
  stick wake the screen?).
- Export and saves backup: remount read-write/read-only of vfat, exfat and
  ntfs3 sticks; write speed to a slow FAT32 stick; a stick pulled while
  writing (the log says "read-write: pulled while writing?"); the FAT
  timestamps after a remount (mtime compare within 2 s).
- The empty-library case: the menus and dialogs used an all-zero style until
  a theme was loaded (fixed in `ui_create()`); check a first boot without ROMs.
- Web upload throughput over WiFi (AP6210) and Ethernet; phone browsers
  (Android Chrome, iOS Safari: file picker, QR auto-login).
- mDNS `retrostone.local` from Windows 11, macOS, iOS, Android; `\\RETROSTONE`
  via LLMNR/NBNS.
- Scan the QR code on the real screen with a phone (the encoder is verified by
  a round-trip test, not yet by a phone).
- ksmbd (3.3) if adopted.
