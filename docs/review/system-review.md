# RetroStoneOS system layer review

Scope: U-Boot config and patches, `boot.cmd` (A/B), kernel fragments and config, the
DTS and overlays, genimage/post-build/post-image, the rootfs overlay (`rcS`, `rcK`,
`inittab`, `data-partition`, `bootlog`, `rsos-net`, `rsos-boot-ok`, `uart-shell`, the
udhcpc hook), `tools/rsos-axpstamp.c`, `tools/rsos-cntvct.c`, and legal-info and
reproducibility. This was a read-only review: no source file was changed and no Buildroot
build was run.

Paths are the ones in the tree on 2026-09-27 (`buildroot-external/board/retrostone2/...`).
The board/common refactor may move them, so every finding also names the function or
block.

How the claims were checked:
- The final BusyBox, kernel and U-Boot `.config` files, the target tree and the images
  in `~/rsos/output` were inspected in WSL.
- `tests/data-partition-test.sh` was run as root. Result: **PASS=27 FAIL=0**.
- Two extra power-cut scenarios were simulated with the same harness (scratchpad
  `review-sys/dp-extra.sh`). Both failed; see S1 and S2.
- Claims that come from reading code rather than from running it are marked "(code)".

## Summary

| id | sev | area | file:line | issue |
|---|---|---|---|---|
| S1 | **High** | power-cut, first boot | `data-partition` `format_full` 135-142, `needs_growing` 93-98, empty-seed branch 217-225 | A power cut between `sfdisk` and the end of `mkfs.exfat` leaves the 128 MiB FAT32 seed inside a full-size type-07 entry for good. Verified: `df` shows 126 MiB on a 2 GiB card after every later boot. A cut during `mkfs.exfat` can leave nothing mountable, so there is never a `/data` (code). |
| S2 | **High** | power-cut, first boot | `data-partition` resume block 194-204 | Stale tail marker: if the user re-flashes after an interrupted conversion, the next boot formats the fresh seed and restores the **old** backup. Files copied after the re-flash are lost silently. Verified (scenario H). |
| S3 | Medium | data safety | `rcS` (no sysctl); `requirements.md:187`; `power.md:348` | The P0 requirement `vm.dirty_expire_centisecs=200`/`dirty_writeback_centisecs=100` is not implemented, although `power.md` says rcS does it. With the kernel defaults, writes can wait 30 s before reaching the card. |
| S4 | Medium | data safety | `data-partition` `mount_data` 71-76 | exFAT is mounted without checking the dirty flag and without `fsck.exfat`. `fsck.exfat` is in the image but nothing runs it. Linux sets VolumeDirty on the first write and clears it only at unmount or remount-ro, so every power loss leaves the volume dirty. (Requirement P1, `requirements.md:188`.) |
| S5 | Medium | A/B | `frontend/src/main.c:2271-2272` → `rsos-boot-ok` 32-36 | The slot is marked good at the first frame. That is before `rsos-net apply` loads WiFi/BT/Ethernet (menu-up + 1 s), before any game runs, and also in charge mode. A new kernel that crashes when those drivers load becomes permanent. |
| S6 | Medium | A/B | `boot.cmd` 126-135; `uboot.fragment:16` (bootcmd) | A confirmed slot that cannot boot has no fallback. If `bootz` fails, the loop ends at a U-Boot prompt (black screen, stays powered). A kernel panic gives a `panic=10` reboot loop that never tries the other slot. |
| S7 | Medium | robustness | kernel `.config`; `rcS` | The watchdog is built (`SUNXI_WATCHDOG=y`, `WATCHDOG_HANDLE_BOOT_ENABLED=y`) but nothing opens it. There is no softlockup or hung-task detection. A freeze (already seen on hardware, DTS:505-514) never resets, and it is never counted as a failed try. |
| S8 | Medium | clock | `default.script.d/rsos-ntp:190`; `busybox.fragment` | The NTP hook calls `ntpd`, but `CONFIG_NTPD` is off and there is no other ntpd in the target. The hook always fails, and silently (output goes to /dev/null). `power.md:338-341` documents it as working. |
| S9 | Medium | release hygiene | `boot.cmd:99`; `bootlog` 342-370; `inittab:19`; `uart-shell:183` | Development settings with no release switch: `initcall_debug log_buf_len=1M` on the command line; a logger that copies dmesg, renames files and syncs every 2 s on exFAT; a root shell on the UART with no password. |
| S10 | Medium | supply chain | `configs/retrostone2_defconfig:33-36,109-113`; `docs/build.md:31` | Linux 6.18.54 and U-Boot 2026.07 are downloaded with **no hash** (`WARNING: no hash file for linux-6.18.54.tar.xz`; Buildroot's `uboot.hash` only has 2026.01). `BR2_DOWNLOAD_FORCE_CHECK_HASHES` is off, and the Buildroot tarball is fetched with `wget` and never checked. |
| S11 | Low | races | `rsos-net` main 315-343, `wifi_on` 95-115 | No lock between inittab's `rsos-net apply` (runs 1-21 s after boot) and the frontend's `rsos-net wifi on/off` jobs. The result can be two `wpa_supplicant`/`udhcpc` instances, or mmc3 unbound while `wifi_on` waits. `flock` is available. |
| S12 | Low | power-cut | `rsos-net` `setting_set` 72-84 | settings.ini is written as tmp → `mv` → `sync` with no fsync of the tmp file. On exFAT a cut can leave settings.ini empty or missing. BusyBox `fsync` is not built. |
| S13 | Low | power-cut | `rsos-net` `bt_store_mount` 225-236; `rcK` 150-158 | The BT image is created without a sync before `mv`, so a cut can leave a bad `bluetooth.img` that is never recreated. rcK's retry path does not unmount `/var/lib/bluetooth`, so `/data` stays busy and power-off leaves it dirty. |
| S14 | Low | first boot | `data-partition` 227-236 | If the backup step fails (rc=1), the entry is not grown. Every later boot repeats the full backup (write + 2 reads, up to 128 MiB) behind the splash. |
| S15 | Low | reproducibility | `rsos-frontend.mk:120`; `post-image.sh:141`; defconfig:136 | Build date from `$(shell date)`; random FAT volume ID; no `BR2_REPRODUCIBLE`; the release defconfig needs the gitignored `homebrew/license ok/`. |
| S16 | Low | legal-info | `package/rsos-frontend/rsos-frontend.mk` | No `_LICENSE`/`_LICENSE_FILES`, so legal-info reports "unknown" and collects none of the bundled third-party licence texts (fonts, miniz, nanosvg, gamecontrollerdb, CC BY-NC-SA themes). Notes on the GPL homebrew and the non-commercial cores. |
| S17 | Low | security | frontend `netnames.c:529-536`, `webshare.c:1103`; kernel `NETFILTER` off; defconfig `ksmbd` | When the network is up, root listens on UDP 137/5353/5355 (and TCP 80 with the web share) on every interface, and no firewall is possible. ksmbd ships but nothing wires it (docs say the frontend starts it). Empty root password. `STRICT_DEVMEM` is off. |
| S18 | Low | WiFi | `post-build.sh:114-115`; brcmfmac `common.c:245-294` | The AP6210 nvram carries the Broadcom default MAC `00:90:4c:c5:12:38`, which brcmfmac replaces with a random MAC at every load. The result is DHCP churn and MAC filtering that cannot work. |
| S19 | Low | U-Boot | `boot.cmd` 39-44, 67 | `saveenv` stores transient variables (`silent=1`, `rsos_p`, `rsos_part_*`) and the whole default environment (bootcmd). U-Boot itself (8 KiB-1 MiB) is not A/B. |
| S20 | Info | misc | `post-build.sh:52`; `docs/build.md:209,468-469` | `sed '/rsos-frontend/d'` also removes the inittab comment lines that mention it. Some docs no longer match the code (bootargs, apply delay). |

## Status after the fix pass (2026-09-27)

Paths moved with the board/common refactor: the init scripts are in
`board/common/rootfs-overlay/`, `boot.cmd`/`uboot.fragment` in `board/retrostone2/`.
Tests run: `board/retrostone2/tests/data-partition-test.sh` **PASS=57 FAIL=0** (was 27
checks; now every interruption point, and the review's G and H, which also pass in the
review's own `dp-extra.sh` harness), `board/retrostone2/tests/boot-ab-qemu-test.sh`
(new, U-Boot on QEMU cubieboard) **21/21**, `board/common/tests/boot-ok-test.sh` (new)
**16/16**, `board-sh-test.sh` 11/11, `release-defconfig-test.sh` ok. Builds: RetroStone2
development image (`~/rsos/output`) and the release variant (`~/rsos/output-release`,
see docs/build.md "Release build").

| id | status | what was done |
|---|---|---|
| S1 | **Fixed** | Both paths write a marker `RSOSCV02 <id> <backup off> <backup MiB> <crc>` (MiB 0 = empty seed) **before** the MBR changes; the MBR is then rewritten in **one** write (sfdisk dump, edited: a new random disk identifier `<id>` + the data entry grown, type 07); `mkfs.exfat` only runs when the MBR carries `<id>` and the kernel sees the new size. At boot a marker whose `<id>` is the current MBR identifier (and the entry is grown) makes the script redo format + restore + layout from the start (idempotent), then clear it. A type 07 entry holding the FAT32 seed (scenario G, old images) is converted again, with a backup if it holds files. A partition that does not mount: `fsck.exfat -p`, `-y`; if it still fails and fsck does not call it clean (or there is no filesystem at all), it is formatted empty through the same crash-safe steps, with a message and `/run/rsos/data-reformatted` (FAT32 is never formatted). Tests: J (cut before the MBR write), K (after it), L (in the middle of mkfs), N (with files), C, C2 (after the restore), G, G2, P. |
| S2 | **Fixed** | The marker is bound to the MBR disk identifier chosen for this conversion: re-flashing rewrites the MBR (the image's identifier is 0), so an old marker never matches; a marker is also ignored while the entry still needs growing (the seed is then untouched and newer than any backup). Old `RSOSBK01` markers are ignored. Scenarios H and M pass (new files kept, old backup not restored). |
| S3 | **Fixed** | rcS: `echo 200 > /proc/sys/vm/dirty_expire_centisecs`, `echo 100 > .../dirty_writeback_centisecs` (builtins). These values are right for a battery device: the flusher only wakes while something is dirty. power.md corrected. |
| S4 | **Fixed** | `mount_data` reads VolumeFlags (1 byte, one `dd`; the shell drops a NUL, so any output = dirty or media failure) after the exFAT mount; if set: unmount, `fsck.exfat -p` with the "Checking the SD card" splash, mount again; result in `/run/rsos/data-fsck`. Before-mount only (no fsck on a mounted volume); measured: 32 GiB, 6,040 files = 156 reads, 1.5 MiB (67 ms on the PC, 360 ms ARM under qemu-user) so no background mode is needed. TODO(hw): time on a full card. A clean boot costs one 1-byte read (the marker read lost its `tr`, so the fork count is unchanged; normal boot ~30 ms in the test). Scenario Q. |
| S5 | **Fixed** (script) + **frontend change** | `rsos-boot-ok` (no argument) now waits for `/run/rsos/net-applied` (end of `rsos-net apply`; not written in charge mode, so in charge mode it waits until the user leaves it), then 30 s, checks that the menu process did not restart, then confirms; one waiter at a time; `rsos-boot-ok now` confirms at once. rcK gives the try back on an orderly shutdown before that (`refund`). Frontend: spawn it only when not in charge mode (see the lead's hand-off). |
| S6 | **Fixed** | `boot.cmd`: every boot is counted (trial: `rsos_tries-1`; confirmed: `rsos_fails+1` up to `rsos_maxfails`, default 3, `0` = off); fallback to the other slot only if it has a kernel (`test -e`) and is not the slot we fell back from (`rsos_fallback`), else stay and count again; kernel/DTB load failure or a returning `bootz`: switch to the other slot and `reset`, else `poweroff` (never a prompt). The kernel gets `rsos.boot=pending`. New finding, fixed: bootcmd's `for rsos_p in 2 3` fallback did not work on QEMU: with the environment variable `rsos_p` just set by bootcmd, the loop variable did not take the values 2 and 3 ("Can't set block device" twice), so a slot without `boot.scr` ended at the prompt; bootcmd now spells out 3/2/3. Tested on QEMU (fallback after 3 panics, no ping-pong, empty slot B, corrupt kernel(s), missing boot.scr, trial slot, `rsos_maxfails=0`, overlays). Cost: one `saveenv` per boot (TODO(hw): measure with the `env-saved` bootstage mark). |
| S7 | **Fixed** (system) + **frontend change** | U-Boot `WATCHDOG_AUTOSTART` (16 s, the A20 maximum; seen running on QEMU), the kernel services it (`HANDLE_BOOT_ENABLED`, no open timeout) until the frontend opens `/dev/watchdog`, so kernel freezes reset (also during the early boot) and a long first boot cannot trigger it. Kernel: `SOFTLOCKUP_DETECTOR` + panic, `HARDLOCKUP_DETECTOR` (buddy) + panic, `PANIC_TIMEOUT=10`, `DETECT_HUNG_TASK` (log only: a slow card flushing a large copy can block for minutes). rcK closes the watchdog with `V`. Frontend: open, pet, magic close (hand-off). |
| S8 | **Fixed** | `CONFIG_NTPD=y` (client only), `CONFIG_TIMEOUT=y`; the hook asks the DHCP servers (`$ntpsrv`) then `pool.ntp.org`, `timeout 30`, once per boot. Verified: the hook logic with a fake ntpd; the static ARM BusyBox `ntpd -w` under qemu-arm with the target's libraries resolved `pool.ntp.org` and got replies. TODO(hw): once on the device. |
| S9 | **Fixed** | `BR2_RETROSTONE_RELEASE` (Config.in) and `configs/retrostone2_release_defconfig`: boot.scr without `initcall_debug log_buf_len=1M`, light boot logger, UART shell open / **password** (release default; the build stops while the password is the `CHANGE-ME` placeholder) / disabled, U-Boot `bootdelay -2`, no "-dev" in the version. docs/build.md "Release build". |
| S10 | **Fixed** | `board/retrostone2/patches/linux/linux.hash` (tarball + licence files) and `patches/uboot/uboot.hash`, read through `BR2_GLOBAL_PATCH_DIR`; `BR2_DOWNLOAD_FORCE_CHECK_HASHES=y`; the Buildroot tarball sha256 in build.md (with how to check the `.sign` on the next bump). |
| S11 | **Fixed** | `flock` on `/run/rsos-net/lock` for on/off and `apply` (after its wait for the menu); daemons get `9>&-`; rcK waits at most 3 s. |
| S12 | **Fixed** | `CONFIG_FSYNC=y`; `setting_set` does tmp, `fsync tmp`, `mv`, `sync`. |
| S13 | **Fixed** | The BT image is made as tmp, `fsync`, `mv`, `sync`; an image that does not mount becomes `bluetooth.img.bad` and a new one is made; rcK's retry unmounts `/var/lib/bluetooth` before unmounting `/data` again. |
| S14 | **Fixed** | A failed backup keeps the FAT32 seed and grows its entry with type 0x0c (like "card too small"), so it is not retried at every boot (a test hook, `RSOS_TEST_FAIL=backup`, replaces the PATH trick that did not work; scenario I). |
| S15 | **Partly** | Build date from `SOURCE_DATE_EPOCH` when it is set (environment, command line, or `BR2_REPRODUCIBLE`); seed FAT volume ID fixed (`52534F53`, or from `SOURCE_DATE_EPOCH`) with `mkfs.vfat --invariant`. Not done: `BR2_REPRODUCIBLE` itself (full rebuild, ext4 UUIDs, mtools timestamps), and the release still needs the gitignored `homebrew/license ok/`. |
| S16 | **Partly** | `RSOS_FRONTEND_LICENSE` / `_LICENSE_FILES` (MIT + fonts, miniz, nanosvg, gamecontrollerdb, the four themes); the project's `LICENSE` is copied into the package source by a post-rsync hook (frontend/ has none). `make legal-info` not run (same reason as the review). Left to the owner: the `ucity.gbc` GPL written offer or a redistributable package, and writing the "sold without firmware" model down in docs/cores.md. |
| S17 | **Documented** | netnames/web share on all interfaces: acceptable, documented (build.md "Networking"); ksmbd: documented as **not wired** (docs, defconfig and fragment comments) with the shares to use when it is. Added `THERMAL_EMERGENCY_POWEROFF_DELAY_MS=5000`. Not done: `STRICT_DEVMEM` (bring-up still uses `devmem`), the empty root password of the development build (the release build has the UART password). |
| S18 | **Fixed** (TODO(hw)) | board.ini `wifi_mac = sid`: `rsos-net` sets `02:` + MD5(A20 SID + "wlan0") before `wlan0` goes up. |
| S19 | **Partly** | `boot.cmd` uses hush locals (`l_*`) instead of `rsos_part_a/b`/`rsos_part`, clears `rsos_p` before `saveenv`; `silent` stays on purpose (it keeps the console quiet from the environment load on). Documented: updates never touch U-Boot; an updater that does must rewrite the environment. `CONFIG_ENV_WRITEABLE_LIST` deferred (needs `CFG_ENV_FLAGS_LIST_STATIC` in a board header, i.e. a U-Boot patch). Minor fixed: `rsos-boot-ok` refuses without `rsos.slot=`. |
| S20 | **Fixed** | post-build removes only `^::respawn:/usr/bin/rsos-frontend$`; build.md bootargs/bootcmd/apply delay and power.md (dirty, NTP) corrected; the README path in post-build (`$COMMON_DIR/../../../frontend`) checked: correct. |

Checked and correct (details at the end): the `fw_env.config` offsets, the U-Boot image size,
the zeroed environment area, `fw_setenv -s -`, the try-counter arithmetic, BusyBox applet
coverage (only `ntpd` is missing), the DT GPIO map and MMC aliases, and the module loading
without udev.

---

## Details

### S1 (High): an interrupted grow/format leaves a 126 MiB data partition for good

**Where:** `rootfs-overlay/usr/libexec/rsos/data-partition`: `format_full()` (135-142),
`needs_growing()` (93-98) and the empty-seed branch (217-225). The same end state comes
from the fallback at 221-224, when `mkfs.exfat` or `partx` fails after `sfdisk` has
succeeded.

**Mechanism:**
1. `format_full` rewrites MBR entry 1 to type 07 and full size **before** it creates the
   filesystem.
2. For the empty seed there is no marker, so nothing records that a conversion was in
   progress.
3. On the next boot:
   - `mount_data` tries exFAT (fails), then vfat, which mounts the untouched 128 MiB
     FAT32 inside the grown entry.
   - `needs_growing` is now false (the entry reaches the end of the card), so nothing
     happens, on this boot and on every later one.

**Verified** (scenario G, `review-sys/dp-extra.sh`): after `echo ',+,7' | sfdisk -N1`,
every later run of `data-partition` exits 0 with `type=7 size=1276 MiB fs=vfat`, and
`df` reports 126 MiB.

**Worse variant (code):** exfatprogs writes the zeroed area and the boot record before
the FAT, the bitmap, the upcase table and the root directory (`mkfs/mkfs.c`
`make_exfat` 555-586). A cut during `mkfs.exfat` can therefore leave a volume that
neither exFAT nor vfat will mount. `data-partition` then prints "cannot mount", exits 1,
and by design ("never reformatted") the device never has a `/data` again.

**Scenario:** first boot on a 64 GB card. The "Preparing the SD card" splash is up. The
user thinks the device is stuck and holds the power key.

**Fix:** make the empty-seed path use the tail marker as well:
- Write a marker (`RSOSBK01 0 0 <cksum of nothing>`, or a distinct `RSOSFMT1`) **before**
  `sfdisk`, and clear it after `mkfs.exfat` and the mount have succeeded.
- In the resume block, a marker with an empty backup means: `format_full`, then
  `make_layout`.

Belt and braces: treat "type 07 but FSTYPE=vfat" as an unfinished conversion. Add
scenario G, plus "cut in the middle of mkfs" (write only the first 24 sectors of a fresh
exFAT), to `data-partition-test.sh`.

### S2 (High): a stale tail marker restores an old backup over fresh user files

**Where:** `data-partition`, block "1. An interrupted conversion comes first" (194-204).

**Mechanism:** the marker lives in the last MiB of the card. That area is outside the
900 MiB image, so re-flashing the card does not erase it. After a re-flash the resume
block sees `RSOSBK01` and `needs_growing` is true. It then runs `format_full` (which
destroys the freshly flashed seed and anything the user copied onto it) and restores the
old backup, whose checksum is still valid.

**Verified** (scenario H):
1. First boot aborted after the format (`RSOS_TEST_ABORT=after_format`).
2. The image is written again with `dd`, and `new.nes` is copied onto the seed.
3. On the next boot `/data` contains `old.nes` and `README.txt` only. `new.nes` is lost,
   with no message to the user.

**Fix:** only restore when the conversion got past `sfdisk`. In the resume block:
- If `needs_growing` is still true, the seed is untouched and newer than any backup. In
  that case clear the marker and fall through to the normal first-boot path, which takes
  a new backup from the seed.
- Restore only when the entry is already grown.

This is also correct for a genuine cut between "marker written" and `sfdisk`. Add
scenario H to the test.

### S3 (Medium): the vm.dirty tuning (P0) is missing

- `requirements.md:187` lists `vm.dirty_expire_centisecs=200` and
  `vm.dirty_writeback_centisecs=100` in rcS as a P0 item.
- `power.md:348` presents it as done.
- `rcS` has no `sysctl` call, and nothing in `buildroot-external/` or `frontend/src`
  writes these values (grep).
- With the kernel defaults (30 s expiry, 5 s writeback), a save or setting written just
  before the battery dies or the unit is forced off is lost.
- The bootlog's `sync` every 2 s only hides this during the first 5 minutes.

**Fix:** two `echo` redirections in rcS, next to the governor loop (builtins, no fork):
- `echo 200 > /proc/sys/vm/dirty_expire_centisecs`
- `echo 100 > /proc/sys/vm/dirty_writeback_centisecs`

Then correct `power.md`.

### S4 (Medium): no dirty-flag check and no fsck of the exFAT data partition

**Kernel behaviour** (checked in `linux-6.18.54/fs/exfat`):
- `exfat_set_volume_dirty()` runs on write paths (`inode.c:43`, `namei.c`, `file.c:159`).
- The flag is cleared only in `exfat_put_super` (super.c:49) and in reconfigure/remount-ro
  (super.c:763).
- Mounting a dirty volume only logs "Volume was not properly unmounted" (super.c:529).

**Consequence:** while the unit is on, the volume is dirty for nearly the whole time,
because the bootlog and the frontend write within seconds of boot. Any power loss
(battery pulled, a 4-6 s AXP forced power-off, the hang in S7, hitting the 3.0 V V_OFF
cut-off) leaves it dirty. From then on:
- `data-partition` mounts it as is, and new writes use an allocation bitmap that may be
  inconsistent (lost or cross-linked clusters).
- Windows asks "Scan and fix".
- `/usr/sbin/fsck.exfat` (exfatprogs 1.2.9, which has `-p`) is in the image but is never
  run.

**Fix** (as `requirements.md:188` specifies):
- In `mount_data`, read the VolumeFlags word of the boot sector (offset 106, bit 1): one
  64-byte `dd`, as `read_marker` already does.
- Only when the bit is set, run `fsck.exfat -p "$PART"` before the mount and leave a note
  in `/run/rsos/` for the UI.
- Measure the fsck time on a full large card (TODO(hw)).

### S5 (Medium): the slot is marked good too early

**Where:** `frontend/src/main.c:2271-2272`. `boot_ok()` (1717-1746) spawns
`/usr/bin/rsos-boot-ok` as soon as `M.first_frame && ui_is_loaded()`. It does not check
`M.charge`, so the charge screen also marks the slot.

**Timing:** `rsos-net apply` (inittab `::once`) waits for `menu-up`, then 1 s, then loads
`brcmfmac`, `hci_uart`/`btbcm` and `dwmac-sunxi`. `eth_park` loads Ethernet on **every**
boot, even when Ethernet is off.

**Scenario:** an update ships a kernel whose `brcmfmac` or `stmmac` oopses or panics at
load (Ethernet is always loaded by `eth_park`; WiFi when `wifi=1`).
1. Boot 1 of the new slot: first frame → `rsos_ok=1` → 1 s later the panic →
   `panic=10` → reboot.
2. On every later boot the slot is "good", so U-Boot never falls back. The result is an
   endless reboot loop.

The same applies to a regression that only appears when a game starts (libretro host,
GL).

**Fix:** mark good after a stability window, not after the first frame. Example:
- `rsos-boot-ok` waits for `/run/rsos/net-applied` (written at the end of
  `rsos-net apply`, also in charge mode), then for uptime ≥ 60 s.
- Or the frontend calls it after the first user input plus N seconds.
- `rsos-boot-ok` must stay idempotent. It already is.

### S6 (Medium): no fallback from a confirmed slot that cannot boot

**Where:** `boot.cmd` 126-135 (the `bootz` failure tail) and `uboot.fragment:16`
(`CONFIG_BOOTCOMMAND`).

**Case 1, `bootz` fails with `rsos_ok=1`** (missing or corrupt kernel on a confirmed
slot; bit rot; a mistaken `fw_setenv rsos_slot b` with `rsos_ok=1` while slot B is empty):
1. No `reset` happens.
2. bootcmd runs `for rsos_p in 2 3`. Each copy of `boot.scr` boots the same
   `rsos_slot` again, because the script uses `rsos_slot`, not the partition it was
   loaded from.
3. Then `bootflow scan -lb` finds the same `boot.scr` again.
4. Finally the U-Boot prompt: black screen, board powered until the battery is flat or
   the AXP long-press. (code)

**Case 2, a kernel panic on a confirmed slot:** `panic=10` gives an endless reboot loop,
because only `rsos_ok=0` counts tries. (code)

**Fix:**
- In the tail after `bootz`, when `rsos_ok=1`: if `rsos_fallback` is unset, set
  `rsos_fallback=1`, switch `rsos_slot`, `saveenv`, `reset`. Otherwise `poweroff`: the
  sunxi AXP209 driver in U-Boot implements it; enable `CONFIG_CMD_POWEROFF` if needed.
- `rsos-boot-ok` clears `rsos_fallback`.
- For panics, see S7 (a watchdog does not help with panics). Optionally, keep a boot
  counter in the AXP209 data registers, which survive resets and cost no flash wear.
  Reserve one register: `rsos-axpstamp` currently uses all twelve (REG04-0F).

### S7 (Medium): the watchdog is present but not used, and there is no lockup detection

**Final kernel `.config`:**
- `CONFIG_SUNXI_WATCHDOG=y`, `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`.
- `# CONFIG_SOFTLOCKUP_DETECTOR`, `# CONFIG_DETECT_HUNG_TASK`, `# CONFIG_PANIC_ON_OOPS`
  are all not set.

**U-Boot:** `CONFIG_WDT_SUNXI=y`, but `# CONFIG_WATCHDOG_AUTOSTART is not set`.

**Userspace:** nothing opens `/dev/watchdog` (no daemon; the BusyBox `watchdog` applet is
built but not started).

**Consequences:**
- The idle freeze documented in the DTS (505-514) leaves the device frozen until a
  forced power-off.
- An unconfirmed slot that hangs rather than panics is only counted when the user forces
  the power off.

**Fix:**
- Start `watchdog -T 16 -t 4 /dev/watchdog` early in `rcS`. The A20 WDT maximum is 16 s.
  It is a static BusyBox applet, so there is no library cost.
- Better: the frontend pets the watchdog from its main loop, so that a hung UI also
  resets. That needs a "magic close" path in rcK before power-off.
- For unconfirmed boots, `boot.cmd` can append `softlockup_panic=1 hung_task_panic=1` to
  `bootargs`, after enabling `SOFTLOCKUP_DETECTOR` and `DETECT_HUNG_TASK`. `panic=10`
  then turns a lockup into a counted failed try.

### S8 (Medium): the NTP hook never works

- `rootfs-overlay/usr/share/udhcpc/default.script.d/rsos-ntp:190` runs
  `ntpd -q -n -p pool.ntp.org`.
- In `busybox-1.37.0/.config`, `# CONFIG_NTPD is not set` (Buildroot's
  `busybox.config:960`), and `busybox.fragment` does not enable it. There is no ntpd,
  chronyd or sntp in `target/`.
- All output goes to `/dev/null`, so the failure is silent.
- The clock therefore relies only on `rsos-clock restore` (last saved time), even with
  WiFi on. `power.md:338-341` documents the NTP step as working.

**Fix:**
- Add `CONFIG_NTPD=y` to `busybox.fragment` (and `CONFIG_TIMEOUT=y` if you want to bound
  it).
- Prefer the DHCP-provided server (`$ntpsrv`, option 42) and fall back to the pool.
- Check once on hardware that name resolution works from the now **static** BusyBox:
  glibc static NSS loads `libnss_dns.so.2`, which is present in `target/lib`.
- The hook also runs on every `renew`, which is harmless once it works.

### S9 (Medium): development settings with no release switch

- **`boot.cmd:99`:** bootargs contain `initcall_debug log_buf_len=1M`. The comment at
  93-98 says "Remove both words for a release image", but no mechanism does it.
- **`bootlog`:**
  - It runs on every boot unless `rsos/logs/disabled` exists.
  - Every 2 s for 5 minutes, then every 30 s for an hour, it copies `dmesg` (up to 1 MiB
    with the settings above), the frontend and game logs, and a status file.
  - Each copy uses tmp + `mv` in `/data/rsos/logs/boot<N>/`, followed by a global `sync`
    (loop at 342-370).
  - Effects: constant exFAT metadata changes (renames) while the volume is dirty (S4),
    card wear, and extra I/O while a game runs.
- **Remote/physical access:**
  - `inittab:19` with `uart-shell:183` (`exec /bin/sh -l`) gives a root shell with no
    authentication (root password is empty: `BR2_TARGET_GENERIC_ROOT_PASSWD=""`).
  - U-Boot stops on any key on the UART (`CONFIG_BOOTDELAY=0`, no `AUTOBOOT_KEYED`).
  - The UART is an internal header (`hardware-pinmap.md:199`), so this needs physical
    access. For a consumer release, it is still a decision to make on purpose.

**Fix:** one release switch (a `Config.in` option, or a variable read by `post-build.sh`)
that:
- builds `boot.scr` without the debug words;
- makes the boot logger opt-in (create `rsos/logs/enabled` instead of `disabled`) or
  limits it to one copy after menu-up plus the shutdown log;
- replaces the UART shell with `getty` + login, or `sulogin`, and sets a stop string
  with `CONFIG_AUTOBOOT_KEYED`.

### S10 (Medium): kernel and U-Boot downloaded without hashes

**What was found:**
- `~/rsos/build.log.*` contains `WARNING: no hash file for linux-6.18.54.tar.xz`.
- `buildroot-2026.02.3/boot/uboot/uboot.hash` only lists `u-boot-2026.01.tar.bz2`, so
  `u-boot-2026.07.tar.bz2` is not checked either.
- The output `.config` has `# BR2_DOWNLOAD_FORCE_CHECK_HASHES is not set`, so a missing
  hash is only a warning.
- `docs/build.md:31` fetches the Buildroot tarball itself with `wget` and never checks
  its checksum or signature.

These are the bootloader and kernel of the product.

**Fix:**
- Add `board/retrostone2/patches/linux/linux.hash` and `patches/uboot/uboot.hash`.
  `BR2_GLOBAL_PATCH_DIR` is also searched for `<pkg>.hash`.
- Set `BR2_DOWNLOAD_FORCE_CHECK_HASHES=y`.
- Put the sha256 of `buildroot-2026.02.3.tar.xz` in `build.md` (or verify the `.sign`
  file).

The libretro packages (commit-pinned, `.hash` files present, including git+submodule
archives), `armbian-firmware` (Buildroot hash) and `rsos-homebrew` (own sha256 check) are
fine.

### S11 (Low): rsos-net has no mutual exclusion

`rsos-net` can run at the same time from:
- inittab `::once` `apply` (starts at boot and waits up to 21 s for the menu);
- the frontend's `net_job()` (`ui/screens.c:754-765`). This one is serialized per device
  by `net_busy`, but not against `apply`;
- the frontend's `"wifi off; wifi on"` restart (`screens.c:820-826`);
- `pw_charge_exit` → `apply` (`main.c:1067-1079`).

**Scenario:** `wifi=1` is saved, and the user switches WiFi off in the first seconds.
`wifi_off` unbinds mmc3 while `apply`'s `wifi_on` is waiting for `wlan0`. Or, both see no
`wpa_supplicant.pid` and start two supplicants; the second fails, which makes `rsos-net`
call `wifi_off`. The hardware state and `settings.ini` end up disagreeing. (code)

**Fix:** `exec 9> /run/rsos-net.lock && flock 9` at the top of the main dispatch
(`CONFIG_FLOCK=y` is built).

### S12 (Low): settings.ini is not written durably

`setting_set` (72-84) writes `settings.ini.tmp`, runs `mv -f`, then `sync`. There is no
fsync of the tmp data before the rename. exFAT has no journal and no ordering between data
and metadata, so a cut while that `sync` runs can publish the new directory entry before
its data: an empty or missing `settings.ini` (WiFi/BT settings lost). This is the same
class as the `file_write_atomic()` item in `requirements.md:186`.

**Fix:** `sync` the tmp file before `mv`. BusyBox `sync FILE` needs
`FEATURE_SYNC_FANCY`, and `fsync` needs `CONFIG_FSYNC`; both are off, so enable one in
`busybox.fragment`. A global `sync` before `mv` also works.

### S13 (Low): the Bluetooth store and the rcK retry

- **`bt_store_mount` (225-236):** it runs `dd` of zeros, `mkfs.ext4`, then `mv`, with no
  sync. After a cut, a `bluetooth.img` of garbage or zeros can exist. From then on,
  `e2fsck -p` fails, the mount fails, and every `bt on` falls back to tmpfs ("pairings
  will not be kept"), because the file is never recreated.
  **Fix:** `sync` before `mv`; if the mount fails, rename the file to `.bad` and create a
  new one.
- **`rcK`:** `bt off` waits at most 2 s for `bluetoothd` (`rsos-net` 250-253) and then
  unmounts. If that unmount fails, the loop device keeps `/data/rsos/bluetooth.img` open.
  The retry block (rcK 150-158) kills every process but never unmounts
  `/var/lib/bluetooth`, so `/data` stays busy, and `poweroff -f` leaves exFAT mounted
  read-write and dirty (S4).
  **Fix:** in the retry block, `umount /var/lib/bluetooth 2>/dev/null` (the loop device
  auto-clears) before retrying `/data`.

The ext4-in-a-loop design itself is fine for power cuts: the image is fully preallocated,
the journal is on, and loop passes flushes on as `fsync` of the backing file.

### S14 (Low): a failed backup is repeated on every boot

When `convert_with_backup` returns 1 (tar or dd error, file-count mismatch, marker write
error), the code only mounts the seed again (227-236); only `rc=2` grows the entry. The
next boot sees `needs_growing` again and repeats the whole backup, with the splash and up
to 128 MiB written plus two full reads. This happens on every boot, before the menu.
(code)

**Fix:** after an rc=1 failure, write a `RSOSBKFAIL` marker (or grow without
reformatting, as for rc=2) and leave the conversion to "Storage > Expand".

### S15 (Low): reproducibility

- `package/rsos-frontend/rsos-frontend.mk:120`: `RSOS_FRONTEND_BUILD_DATE := $(shell date
  +%Y-%m-%d)` ignores `SOURCE_DATE_EPOCH`.
- `post-image.sh:141`: `mkfs.vfat` without `-i` or `--invariant`. The seed volume ID
  (e.g. `7e c7 37 a8`) changes on every build.
- `BR2_REPRODUCIBLE` is off, so the ext4 UUID and hash seed and the mkimage timestamps
  vary.
- `BR2_PACKAGE_RSOS_HOMEBREW=y` in the release defconfig requires the gitignored
  `homebrew/license ok/`. A fresh clone cannot build that image, or run legal-info for
  it.

### S16 (Low): legal-info coverage

`make legal-info` was **not** run: it would rsync and extract local packages inside the
output tree another agent is using. This section is from reading `pkg-generic.mk`
(1112-1190).

- **`rsos-frontend`:** no `RSOS_FRONTEND_LICENSE` and no `_LICENSE_FILES`. legal-info
  prints "cannot save license" and the manifest says "unknown". The package bundles
  third-party material:
  - `third_party/fonts/LICENSE-{DejaVu,Roboto}.txt`, miniz, nanosvg,
    sdl-gamecontrollerdb;
  - themes `gbz35`/`gbz35-dark` under CC BY-NC-SA 3.0, plus the rsos themes.

  The on-device licence files are installed by the frontend's Makefile, but legal-info
  collects nothing. Also, the project's `LICENSE` (MIT) is at the repository root,
  outside the package source directory. **Fix:** add `frontend/LICENSE` and list all the
  above in `_LICENSE`/`_LICENSE_FILES`.
- **`rsos-homebrew`:**
  - `REDISTRIBUTE = NO` is right for the permission-only ROMs.
  - `ucity.gbc` is GPL-3.0+ and its source is then not in legal-info. That is acceptable
    only if `NOTICE.txt` holds a valid written offer (GPLv3 §6b). Cleaner: a separate,
    redistributable `rsos-homebrew-ucity` package carrying the source tarball.
  - The licence gathering needs a host `unzip` (listed in `build.md:25`).
- **Non-commercial licences:** FBNeo, MAME 2003-Plus, PicoDrive, Snes9x 2005/2010 and the
  gbz35 themes are acceptable only under the model stated in
  `frontend/themes/gbz35/LICENSE.txt`: the console is sold without firmware, and the
  image is free. Make sure no unit or card ships with the image preinstalled, and write
  that down in one place (`docs/cores.md`).
- **Firmware:** `armbian-firmware` is PROPRIETARY in Buildroot. The BT `.hcd` files are
  installed by an `ARMBIAN_FIRMWARE_POST_INSTALL_TARGET_HOOKS` hook in `external.mk`, so
  they are covered by that package's legal-info entry. No action needed.

### S17 (Low): network exposure and hardening

**Listening sockets** (all on `INADDR_ANY`, as root, from the frontend) when the network
is up:
- netnames: UDP 5353, 5355 and 137 (`netnames.c:529-536`);
- the web share when enabled: TCP 80 (`webshare.c:1067,1103`; PIN, private addresses
  only, plain HTTP).

`CONFIG_NETFILTER` is off, so there is no way to add a firewall later without a new
kernel. No other daemons: `sbin`/`usr/sbin` contain no sshd, dropbear or telnetd, and the
BusyBox `inetd` and `dnsd` applets exist but are not started.

**ksmbd:** `ksmbd.ko` and ksmbd-tools are in the image. `build.md:488-491` and the
defconfig say the frontend starts the share, but no code does (grep: no `ksmbd` in
`frontend/`). That is dead weight today. If it is wired later with `/data` as the share,
then `rsos/wpa_supplicant.conf`, `settings.ini` and `rsos/bluetooth.img` become writable
from the LAN. The last one is an ext4 image that root loop-mounts, so it exposes the
kernel's filesystem parser. Share `roms/`, `bios/` and `saves/` only.

**Hardening options for free:**
- `CONFIG_STRICT_DEVMEM=y` (currently `DEVMEM=y` with no restriction, plus the `devmem`
  applet).
- `CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS` is 0: if the orderly power-off at the
  critical trip fails, nothing forces it.

`wpa_supplicant.conf` with the PSK in plain text on exFAT is by design (readable on any
PC anyway). The file is written by the frontend and is not reachable through the web
share targets.

### S18 (Low): the WiFi MAC changes on every load

- `post-build.sh:114-115` links the Cubietruck nvram, which contains
  `macaddr=00:90:4c:c5:12:38` (same for AP6212).
- brcmfmac detects this Broadcom default (`common.c:245-294`) and calls
  `eth_random_addr()`.
- So `wlan0` gets a new random MAC every time the module loads: every boot and every WiFi
  toggle. Routers hand out new leases, MAC allow-lists cannot work, and the router's
  device list grows.

**Fix:** derive a stable locally administered MAC from the A20 SID (U-Boot already does
this for `ethernet0`), then either:
- apply it in `rsos-net wifi_on` (`ip link set wlan0 address ...` before `up`), or
- let U-Boot add `local-mac-address` to the `wifi@1` node.

### S19 (Low): U-Boot environment contents

`saveenv` (`boot.cmd` 43, 67) stores the whole in-memory environment. That includes
bootcmd's transient `silent=1`, `silent_linux=no`, `rsos_p`, and `rsos_part_a`/`_b`,
plus every default variable, including `bootcmd` (already noted in `build.md`). This is
harmless today, but:
- a future U-Boot update keeps the old `bootcmd`;
- U-Boot itself at 8 KiB to 1 MiB has no A/B copy, so an updater that rewrites it can
  brick the unit if the power is cut during the write (recoverable only by re-flashing
  on a PC).

Document that updates never touch U-Boot, or write the new copy and verify it before
switching.

Minor: `rsos-boot-ok` marks the slot even when `/proc/cmdline` has no `rsos.slot=` (a
boot from the `bootflow scan` fallback), because `[ -n "$booted" ]` skips the check.

### S20 (Info): minor issues

- `post-build.sh:52`: `sed -i '/rsos-frontend/d'` also deletes the two comment lines of
  the overlay inittab that mention it. The final `target/etc/inittab` has a truncated
  comment. Use `'/^::respawn:\/usr\/bin\/rsos-frontend$/d'`.
- Docs that no longer match the code:
  - `build.md:209` bootargs lack `axp20x-i2c initcall_debug log_buf_len=1M`.
  - `build.md:468-469` says `apply` "waits 2 s"; the code waits for menu-up, then 1 s.
  - `power.md:348` (S3) and `power.md:338-341` (S8).
- `post-build.sh:81` finds the README through `$BOARD_DIR/../../../frontend`. Re-check
  this path after the board/common move.

---

## Checked and correct

- **The environment configuration matches.** `fw_env.config` (`/dev/mmcblk0 0x100000
  0x10000` and `0x110000 0x10000`) matches the final `uboot-2026.07/.config`:
  `CONFIG_ENV_IS_IN_MMC=y`, `ENV_REDUNDANT=y`, `ENV_OFFSET=0x100000`,
  `ENV_OFFSET_REDUND=0x110000`, `ENV_SIZE=0x10000`, `ENV_MMC_DEVICE_INDEX=0`.
- **Layout does not overlap:**
  - `u-boot-sunxi-with-spl.bin` is 362,264 B and ends at 0x5A718, below 1 MiB.
  - The environment ends at 1.125 MiB, and rootfs A starts at 4 MiB (`sfdisk -d
    sdcard.img`: p2 start 8192, p3 794624, p1 1581056 type c).
- **The environment area of `sdcard.img` is all zeros** (verified), so a re-flash also
  resets the A/B state.
- **Power cuts during environment writes are safe.** U-Boot's `env_mmc` and uboot-tools
  2025.10 `fw_env` both use the incremental flag scheme on block devices and always
  write the inactive copy, so a cut during `saveenv` or `fw_setenv` leaves the previous
  copy valid. `fw_setenv -s -` reads the script from stdin (`fw_env.c:764`).
- **Try counter:**
  - With tries=3, the new slot is attempted 3 times, then the script falls back.
  - An empty or garbage `rsos_tries` falls back at once.
  - `setexpr` writes hex, which is fine for 1-9 as documented.
  - A failing `bootz` on an unconfirmed slot resets, and the reset counts as a try.
  - Loading the other slot's `boot.scr` when the selected slot's is unreadable still
    counts down correctly.
- **The MMC aliases in the DTS** (`mmc0=&mmc0; mmc1=&mmc2; mmc2=&mmc3`) keep the microSD
  as `mmcblk0`, so `root=`, `fw_env.config` and `data-partition` stay correct with the
  eMMC overlay.
- **DTS:**
  - No GPIO used twice (PH0-4,7,9-16,18-20,22-27; PC16,18,22; PI4-9,12,16-19; PB0-2,22-23;
    PA GMAC).
  - `dcdc2` 1.2-1.4 V with a ramp delay, which drops the OPPs below 720 MHz, as intended.
  - `dcdc3` always-on with a range and no consumer, so it stays at U-Boot's value.
  - `ldo3`/`ldo4` have no consumer and are switched off.
  - `vcc3v3` is a fixed always-on regulator.
  - `i2c1` is disabled and `at24` is only a module.
  - `WIFI_MMC=1c12000.mmc` matches `mmc3`.
  - Overlays target existing labels, and `mmc2` has its pinctrl in the dtsi.
- **BusyBox applets:** every applet the overlay scripts use exists in the static BusyBox
  1.37.0 or in a package (util-linux sfdisk/partx, exfatprogs, e2fsprogs, alsa-utils
  amixer, uboot-tools fw_*, dbus, bluez), **except `ntpd`** (S8). This includes
  `readahead`, `ionice`, `setsid`, `usleep`, `pidof`, `od`, `awk`, `cksum`, `top`,
  `find -iname/-ipath/-prune`, `dd conv=fsync`, `tar` create with GNU long names,
  `mount -o loop`, `flock`, `mktemp` (udhcpc script) and `nice`.
- **Modules without udev:**
  - rcS loads usb-storage, uas, sd_mod and ahci_sunxi.
  - rsos-net loads brcmfmac, hci_uart (btbcm by dependency) and dwmac-sunxi/sun4i-emac.
  - ntfs3, crypto and hidp come through the kernel's `request_module` (`/sbin/modprobe`
    exists; `modules.alias` and `modules.dep` are built).
  - Firmware is loaded directly from `/lib/firmware` (`FW_LOADER=y`, no user helper). The
    `brcm/` links resolve to the `cypress/` blobs and the Armbian `.hcd` files.
- **`data-partition` on the tested paths is sound:** empty seed; seed with files; cut
  after the format and resume; card too small; normal boot in 21 ms; damaged backup.
  Also tested: names with spaces, PC junk ignored, the marker cleared, and no retry after
  `RSOSBKBAD`.
- **rcK:** the fast-path ordering is correct (logger stopped, frontend TERM, clock saved,
  BT and SMB stopped, log written, `sync`, `umount` or remount-ro, AXP stamp,
  `poweroff -f` which syncs). The U-Boot environment is never written at shutdown.
- **Tools:** `rsos-axpstamp` uses `I2C_RDWR` one register at a time, stores a plausible
  record, and clears it after reading. REG04-0F are not in the axp20x regmap's use.
  `rsos-cntvct` computes the offset from its own kmsg line, which stays correct
  whichever clock drives printk.

---

## Top 10 to fix first

1. **S2:** in the resume block, restore only when the entry is already grown. Otherwise
   discard the marker and back up the seed again. Add scenario H to the test.
2. **S1:** write the marker before `sfdisk` on the empty-seed path too, and treat "type 07
   + vfat" as unfinished. Add scenario G and a cut in the middle of mkfs.
3. **S6:** handle a confirmed slot whose `bootz` fails with a one-shot switch to the
   other slot, then `poweroff`. Never stop at a U-Boot prompt.
4. **S5:** mark the slot good after a stability window (after `rsos-net apply` and ≥ 60 s,
   or after the first input), not at the first frame and not in charge mode.
5. **S3:** set the vm.dirty values in rcS (two `echo`s) and correct `power.md`.
6. **S4:** read the exFAT VolumeFlags and run `fsck.exfat -p` only when the volume is
   dirty.
7. **S7:** start the hardware watchdog in rcS (or pet it from the frontend). For
   unconfirmed boots, add `softlockup_panic`/`hung_task_panic`.
8. **S10:** add `linux.hash` and `uboot.hash` under `BR2_GLOBAL_PATCH_DIR`, set
   `BR2_DOWNLOAD_FORCE_CHECK_HASHES=y`, and pin the Buildroot tarball's sha256.
9. **S9:** add a release switch: no `initcall_debug log_buf_len=1M`, the boot logger
   opt-in, a UART login, and a U-Boot stop string.
10. **S8 + S11 + S12:** enable `CONFIG_NTPD` (and `FSYNC`) in `busybox.fragment`; add a
    `flock` to `rsos-net`; `sync` the tmp file before the `mv` in `setting_set`.

Reproduction scripts: session scratchpad `review-sys/` (`dp-extra.sh` holds scenarios G,
H and I; `s1.sh`-`s14.sh` are the read-only inspections). Scenario I, a simulated tar
failure, did not trigger, because the host BusyBox runs its own applet instead of the
PATH wrapper. S14 therefore rests on the code only.
