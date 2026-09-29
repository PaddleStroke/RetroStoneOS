# Porting RetroStoneOS to another board

RetroStoneOS was written for the **RetroStone2** (Allwinner A20, AXP209, 640x480 LCD + HDMI), which stays the
reference hardware. Everything that depends on the board is now in one board folder and one profile file, so a port
to other hardware does not touch the frontend or the shared scripts. The first port is the **Raspberry Pi 4
(64-bit)**:

| Board | Defconfig | Status |
|---|---|---|
| RetroStone2 | `retrostone2_defconfig` | tested on hardware |
| RetroStone1 (Allwinner H3) | `retrostone1_defconfig` | untested, built by CI; board folder `board/retrostone1/` with its own device tree (like the RetroStone2) and the A/B boot of `board/sbc-uboot/`; the built-in screen is the H3 composite output (kernel patch 0002, `internal_display = composite`), not yet seen on hardware: [boards.md](boards.md) |
| Raspberry Pi 4 (64-bit) | `rpi4_64_defconfig` | **community-tested**: it builds; the RetroStoneOS developers have not run it on hardware. Reports and fixes are welcome. |
| Raspberry Pi 2 (and 3 / Zero 2 W in 32-bit mode), Raspberry Pi 3 / 3B+ / Zero 2 W (64-bit), Raspberry Pi 5, Orange Pi PC / PC Plus, Orange Pi One / Lite, Orange Pi 5 | `rpi2_defconfig`, `rpi3_64_defconfig`, `rpi5_64_defconfig`, `orangepi_h3_pc_defconfig`, `orangepi_h3_one_defconfig`, `orangepi5_defconfig` | **community-tested**; the support matrix, the per-board notes and the boards that were dropped are in [boards.md](boards.md) |

This page covers what a port needs, the board profile keys, a step-by-step guide for a new board, the Pi 4 as a
worked example, and (section 9) how the other boards are put together.

## 1. The layout: shared and board-specific parts

```
buildroot-external/
  configs/<board>_defconfig          one defconfig per board
  board/common/                      shared by every board
    rootfs-overlay/                  rcS, rcK, inittab, fstab, rsos-net, rsos-boot-ok,
                                     usr/libexec/rsos/{board.sh, data-partition, bootlog, uart-shell},
                                     the udhcpc NTP hook
    post-build.sh                    data mount point, file modes, /etc/rsos/board.env, inittab
                                     (frontend, serial console), rom-folders, /media, BlueZ state
    mk-data-seed.sh                  the 128 MiB FAT32 "RETROSTONE" data seed (for post-image)
    busybox.fragment                 static BusyBox + readahead/ionice, ntpd, timeout, fsync
    linux-rsos.fragment              what RetroStoneOS needs from any kernel (section 3)
    tests/board-sh-test.sh           tests of board.sh (the system disk)
    tests/boot-ok-test.sh            tests of rsos-boot-ok (A/B confirmation)
    tests/release-defconfig-test.sh  the release defconfig follows the development one
  board/<board>/                     only what is specific to the board
    rootfs-overlay/etc/rsos/board.ini       the board profile (section 4)
    rootfs-overlay/etc/rsos/board-hooks.sh  optional shell hooks (mixer, extra modules)
    linux.fragment, patches/, dts/   kernel configuration, patches, device tree
    genimage*, post-image.sh         the SD card image
    bootloader / firmware files      U-Boot fragment + boot.cmd (RetroStone2), config.txt +
                                     cmdline.txt (Raspberry Pi)
  board/sbc-uboot/                   the A/B boot.cmd and U-Boot options shared by the U-Boot
                                     single-board computers (Orange Pi ports, section 9)
  package/libretro-common.mk         per-architecture platform selection of the cores
```

The defconfig lists the shared parts first and the board's second:

```
BR2_ROOTFS_OVERLAY="$(BR2_EXTERNAL_RETROSTONE_PATH)/board/common/rootfs-overlay $(BR2_EXTERNAL_RETROSTONE_PATH)/board/<board>/rootfs-overlay"
BR2_ROOTFS_POST_BUILD_SCRIPT="$(BR2_EXTERNAL_RETROSTONE_PATH)/board/common/post-build.sh [$(BR2_EXTERNAL_RETROSTONE_PATH)/board/<board>/post-build.sh]"
BR2_ROOTFS_POST_IMAGE_SCRIPT="$(BR2_EXTERNAL_RETROSTONE_PATH)/board/<board>/post-image.sh"
BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_RETROSTONE_PATH)/board/common/busybox.fragment"
```

A file in the board's overlay replaces the shared one (the RetroStone2 replaces `/etc/fstab` to document its data
partition). The frontend, the libretro host and the cores are the same packages on every board.

## 2. What the software expects from the hardware

| Area | Expectation | Notes |
|---|---|---|
| Display | One DRM/KMS device with **atomic modesetting**, universal planes and dumb buffers; connector hotplug through uevents (or the kernel's own polling) | The frontend opens the first `/dev/dri/cardN` that has connectors and CRTCs. Scaling is probed with TEST_ONLY commits: a hardware-scaled plane when the driver accepts one, a CPU scaler into a screen-sized buffer otherwise. No quirk flags are needed for plane limits. A built-in panel is any non-external connector (board key `internal_display`). |
| GPU | EGL + **GLES 2.0 on GBM** from the KMS device (Mesa: lima, v3d/vc4 through kmsro, panfrost, freedreno, ...) | Only the two N64 cores use it (host-design.md §13). Every other core renders in software. GLES 3 contexts are not supported by the host yet. |
| Audio | ALSA: 48 kHz, S16_LE, stereo | The HDMI card is found by name ("hdmi"), the built-in one by name ("codec") or as the first other card; board keys can name them. |
| Input | evdev | Built-in buttons: one or more input devices whose name starts with `builtin_pad_prefix` (gpio-keys, adc-joystick). USB and Bluetooth pads through HID. **There is no udev**: every input and gamepad driver must be built into the kernel. |
| Power | Optional | A battery and charger in `/sys/class/power_supply`, a power key device reporting `KEY_POWER`, cpufreq with the `schedutil` and `performance` governors, a thermal zone. Missing parts simply disable the matching features (no battery icon, no power key, ...). |
| Storage | One system disk (SD card) with an MBR: root filesystem A and B (ext4, read-only) and the data partition | The data partition is a FAT32 seed at the end of the card that the first boot turns into a full-size exFAT (`data-partition`). Its MBR entry number is `data_partition`. |
| Boot | Kernel + device tree loaded by the board's bootloader; `root=/dev/<disk><part>` on the command line | A/B slot switching is board-specific: U-Boot + `boot.cmd` + `fw_setenv` on the RetroStone2 (every boot counted, `rsos.boot=pending` on the command line, confirmed by `rsos-boot-ok` once the menu has been stable for 30 s after `rsos-net apply`; docs/build.md "A/B slots"). `rsos-boot-ok` does nothing on a board without `/etc/fw_env.config`. |
| Watchdog | Optional: a `/dev/watchdog` driver built in | The frontend opens and pets it (docs/power.md §11). Best: the bootloader starts it and the kernel services it until then (`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED`), as U-Boot does on the RetroStone2; otherwise it only runs once the frontend has opened it. |

## 3. Kernel requirements

`board/common/linux-rsos.fragment` lists what every board needs; a new board merges it before its own fragment:

- devtmpfs mounted by the kernel, no initramfs, ext4, vfat, exFAT, NLS UTF-8/437/8859-1, loop devices;
- **built in (=y)**: DRM, the board's display and GPU drivers, ALSA and the board's sound drivers, evdev, HID, USB
  HID, uhid and the gamepad drivers (xpad, Sony, PlayStation, Nintendo, Microsoft, Logitech, DragonRise, GreenAsia,
  SmartJoyPlus, Steam, EMS). Nothing would load them as modules;
- as modules, loaded on demand with BusyBox `modprobe`: WiFi, Bluetooth, Ethernet (unless built in), `usb-storage`,
  `uas`, `sd_mod` (rcS loads them once the menu is up), NTFS3, ksmbd;
- **uncompressed modules** (BusyBox `modprobe` does not decompress);
- cpufreq with the schedutil, performance and powersave governors;
- firmware loaded directly by the kernel (no user helper);
- robustness: the watchdog core (the board fragment builds its watchdog driver in), soft and hard lockup detectors
  that panic, `PANIC_TIMEOUT=10` (a panic reboots, which the A/B logic counts), hung-task detection that only logs,
  and a 5 s emergency power-off after a critical temperature.

The RetroStone2's `linux.fragment` already contains all of this next to the A20 options and its boot-time trimming, so
it does not include the common file.

## 4. The board profile: `/etc/rsos/board.ini`

The board's rootfs overlay installs `/etc/rsos/board.ini`. The frontend and the game process read it once at start-up
(`frontend/src/board.h`; the `RSOS_BOARD_INI` environment variable overrides the path). At build time
`board/common/post-build.sh` turns it into `/etc/rsos/board.env` (`key = value` becomes `RSOS_BOARD_<KEY>='value'`),
which the init scripts source through `/usr/libexec/rsos/board.sh` without parsing anything at boot. Unknown keys are
ignored; `#` and `;` start comments.

**Every key is optional.** A missing key, or a missing file, means auto-detection. A board without a profile (for
example a Raspberry Pi with only HDMI, a USB pad and no battery) gets no built-in pad, no battery UI, no LCD
switching, a power key only if an input device is a power button, and HDMI output only.

### Frontend keys

| Key | Meaning | Default (no key) | RetroStone2 | Raspberry Pi 4 |
|---|---|---|---|---|
| `name` | board name, in the logs and in "<name> built-in" | `Generic` | `RetroStone2` | `Raspberry Pi 4` |
| `builtin_pad_prefix` | evdev name prefix of the built-in buttons; they are merged into one pad | none | `RetroStone2` | none |
| `builtin_stick` | evdev name of the built-in analog stick (merged into that pad) | none | `analog-stick` | none |
| `internal_display` | connector types of the built-in screen: `auto` (every type that is not HDMI/DVI/DP/VGA/TV), `none`, or a list of `dpi`, `unknown`, `lvds`, `dsi`, `edp`, `virtual`, `spi`, `composite`, `svideo`, `tv`. An analog TV type in the list is the built-in screen (the RetroStone1: `composite`) and is switched with HDMI like a panel | `auto` | `unknown, dpi` (sun4i_rgb registers the panel as "Unknown-1") | `none` |
| `tv_norm` | built-in composite screen only: `ntsc` (720x480 interlaced, 59.94 Hz), `pal` (720x576 interlaced, 50 Hz) or `auto` (the kernel's preferred mode, which `video=Composite-1:PAL` on the command line changes); `ntsc` and `pal` fall back to each other when the mode is missing or refused. The picture is taken as 4:3 | `ntsc` | not used | not used |
| `tv_overscan` | built-in composite screen only: margin kept clear on each edge, in percent (0-20); the scaled game/UI planes and the overlay stay inside it | `0` | not used | not used |
| `internal_refresh_options` | refresh rates offered for the built-in screen: `<native>,60` gives Settings > Display > LCD refresh rate | none | `78, 60` | none |
| `backlight` | `/sys/class/backlight` entry: `auto` (the first), `none`, or a name | `auto` | `auto` | `none` |
| `battery_supply`, `ac_supply`, `usb_supply` | `/sys/class/power_supply` entries: `auto` (the first of each type), `none`, or a name (a missing name falls back to auto) | `auto` | `axp20x-battery`, `axp20x-ac`, `axp20x-usb` | `none` |
| `battery_voff_mv` | battery cut-off voltage to program (`voltage_min`, the AXP V_OFF) | not programmed | `3000` | none |
| `thermal_zone` | thermal zone type: `auto` (a zone whose type contains "cpu"), or a type | `auto` | `auto` | `auto` |
| `power_key_device` | evdev name of the power key: `auto` (any device that reports `KEY_POWER` and has at most 4 keys), `none`, or a name. A name is also the platform driver whose `startup`/`shutdown` attributes are set | `auto` | `axp20x-pek` | `auto` |
| `pek_startup_ms` | power-on hold time to program into the PMIC power key | not programmed | `128` | none |
| `audio_internal`, `audio_hdmi` | ALSA card ids of the built-in output and of HDMI: `auto` (by name) or an id | `auto` | `auto` (sun4i-codec, sun4i-hdmi) | `auto` (vc4hdmi0/1: the card of the HDMI port in use) |
| `audio_hdmi_pcm` | ALSA PCM type for the HDMI card: `plughw`, or `hdmi` (the card's IEC958 set-up in alsa-lib) | `plughw` | `plughw` | `hdmi` (vc4-hdmi takes IEC958 subframes only) |
| `cpu_governor_menu`, `cpu_governor_game` | cpufreq governors in the menu and while a game runs | `schedutil`, `performance` | the same | the same |
| `storage_overlays` | opt-in device tree overlays offered in Settings > Storage (known: `emmc`, `sata`) | none | `emmc sata` | none |
| `display_quirks` | `sun4i-tcon0-clock`: log the A20 TCON0 pixel clock model when the panel is retimed. `panel-keep-scanning` is **the default** (also without a board.ini: panel safety fails safe) for a built-in DPI, LVDS, DSI or Unknown panel, as when its VCC is on an always-on rail with no power GPIO: a TFT must never stay powered with its signals stopped, so the panel's CRTC is never turned off while the system runs (screen off = backlight `bl_power` off + a black frame, HDMI on another CRTC while the panel scans black; display-design.md §8.5). Not applied to a composite screen behind its own controller (the RetroStone1's AMT630A) nor without a built-in panel; `panel-power-switched` opts out (a panel whose supply is switched with it) | none (keep-scanning) | `sun4i-tcon0-clock panel-keep-scanning` | none |

`frontend/tests/test_board.c` (in `make check`) loads the RetroStone2's `board.ini` and checks that it gives exactly
the constants the code used before the profile existed, and that the power module does the same things with and
without it on a fake sysfs tree. It also parses every other `board/*/rootfs-overlay/etc/rsos/board.ini` of the tree.

### Init script keys

| Key | Used by | Meaning | Default | RetroStone2 | Raspberry Pi 4 |
|---|---|---|---|---|---|
| `data_disk` | rcS (I/O timings, read-ahead), data-partition, bootlog | the system disk: `/dev/...`, or `auto` = the disk of `root=/dev/...` on the kernel command line (`/dev/mmcblk0` if there is none, e.g. `root=PARTUUID=`) | `auto` | `/dev/mmcblk0` | `auto` |
| `data_partition` | data-partition | MBR entry number of the data partition (physically last on the disk) | `1` | `1` | `4` |
| `firstboot_trace_kib` | data-partition, bootlog | offset in KiB (a multiple of 64) of a 64 KiB raw area outside every partition where the first boot's steps are kept across a hang or a power cut (docs/build.md "First boot"); used only if no partition overlaps it | none | `3072` | none |
| `boot_reason` | rcS | `axp209`: run `rsos-bootreason` (charge mode detection) | none | `axp209` | none |
| `wifi_module` | rsos-net | WiFi driver module | `brcmfmac` | `brcmfmac` | `brcmfmac` |
| `wifi_sdio_host`, `wifi_sdio_driver` | rsos-net | SDIO host device (and its platform driver) that is unbound to power the WiFi chip down | none (only the module is loaded/unloaded) | `1c12000.mmc`, `sunxi-mmc` | none |
| `eth_modules` | rsos-net | Ethernet modules to load | none (built-in driver) | `dwmac-sunxi sun4i-emac` | none (GENET built in) |
| `bt_module` | rsos-net | Bluetooth UART driver module | `hci_uart` | `hci_uart` | `hci_uart` |
| `wifi_mac` | rsos-net | `sid`: set a stable, locally administered WiFi MAC derived from the SoC ID (`/sys/bus/nvmem/devices/*sid*/nvmem`) before the interface goes up, for a chip whose nvram has no real MAC | none (the driver's MAC) | `sid` | none (the Pi firmware provides the MAC) |

`board-sh-test.sh` in `board/common/tests/` checks how `board.sh` finds the system disk (`mmcblk0p2`, `sda2`,
`nvme0n1p2`, `PARTUUID=`, an explicit `data_disk`).

### Build keys

| Key | Used by | Meaning | Default |
|---|---|---|---|
| `serial_console` | common post-build.sh | tty of the shell on the debug UART (`/etc/inittab`), `none` for no shell | `ttyS0` |

### System updater key

| Key | Used by | Meaning | Default | RetroStone2 | Raspberry Pi 4 |
|---|---|---|---|---|---|
| `ab_update` | `rsos-update` (docs/updates.md) | in-place A/B updates: `auto` = when `/etc/fw_env.config` exists and the kernel was started by the A/B boot script (`rsos.slot=` on its command line); `1`; `0` = "flash the new image" (the check still tells about new versions) | `auto` | `1` | (auto: off, no U-Boot environment) |

The updater's board id is not a key: it is the defconfig name (`retrostone2`, `orangepi-h3-pc`, `rpi4-64`), written
to `/etc/rsos/version.env` by the rsos-frontend package; update packages are made per board id.

The build variant is not a board key: `BR2_RETROSTONE_RELEASE` and the UART shell choice (open / password /
disabled) are Buildroot options of this tree (docs/build.md, "Release build"). `board/common/post-build.sh` writes
them to `/etc/rsos/build.env` (`RSOS_RELEASE`, `RSOS_UART_SHELL`) for the boot logger and the UART shell wrapper.

### Board hooks: `/etc/rsos/board-hooks.sh`

Optional shell functions, sourced by `board.sh` (only definitions; nothing runs when it is read). rcS calls them in
its background job once the menu is up:

| Function | When | RetroStone2 |
|---|---|---|
| `rsos_board_late_audio` | before the "bg: mixer" step | sets the sun4i-codec power amplifier level and routing with amixer |
| `rsos_board_late_storage` | after the USB storage modules | loads `ahci_sunxi` when the SATA overlay enabled the controller |

## 5. Step by step: a new board

1. **Start from Buildroot's defconfig for the board** (`configs/` in the Buildroot tree): architecture, toolchain,
   kernel source, bootloader or firmware. Pick a glibc toolchain with C++ (the frontend and many cores are C++ or link
   libstdc++); the Bootlin "stable" toolchains are what the two existing boards use.
2. **Write `configs/<board>_defconfig`** in this tree: copy the system, overlay, post-build, audio, graphics,
   data-partition, networking, host-tool and package sections of `rpi4_64_defconfig`, and keep the board's own
   architecture, kernel and bootloader lines. `BR2_DL_DIR="$(HOME)/rsos/dl"` shares the download cache.
3. **Kernel**: the board's defconfig + `board/common/linux-rsos.fragment` + `board/<board>/linux.fragment` with the
   display, GPU and sound drivers built in (section 3). Check the resulting `.config`
   (`output/build/linux-*/.config`): an `=m` that the menu needs will never be loaded.
4. **Graphics**: Mesa with the board's Gallium driver, EGL, GLES and GBM (`BR2_PACKAGE_MESA3D_OPENGL_EGL` selects GBM).
   The KMS device and the GPU can be two drivers: Mesa's kmsro pairs them (vc4 + v3d, sun4i-drm + lima).
5. **Board profile**: `board/<board>/rootfs-overlay/etc/rsos/board.ini` (section 4). Start from the Pi 4 one for a
   box with HDMI only, from the RetroStone2 one for a handheld.
6. **Image**: a `genimage` configuration with the bootloader or firmware, rootfs A (the ext4 image), an empty rootfs
   B of the same size, and the data seed **last on the disk** (`data.vfat`, made by `board/common/mk-data-seed.sh`
   from the board's `post-image.sh`). Set `data_partition` to its MBR entry number and make `root=` point at rootfs A.
7. **Cores**: nothing to do for an ARMv7 Cortex-A7, aarch64 or x86_64 target (`package/libretro-common.mk`,
   cores.md "Architectures"). For another 32-bit ARM CPU, add its key there and to
   `BR2_PACKAGE_RSOS_LIBRETRO_ARCH_SUPPORTS` in `package/libretro-cores.Config.in`.
8. **Build** in a separate output directory:
   ```sh
   export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
   cd ~/rsos/buildroot-2026.02.3
   make O=~/rsos/output-<board> BR2_EXTERNAL=/mnt/c/path/to/RetroStoneOS/buildroot-external <board>_defconfig
   make O=~/rsos/output-<board>
   ```
   and run `make check` in `frontend/` (it parses the new `board.ini`).
9. **Bring-up on the hardware** (serial console): `cat /etc/rsos/board.env`; `rsos-kmstest --list` (connectors,
   planes; the built-in screen must be "internal"); `evtest` (built-in buttons: names must match
   `builtin_pad_prefix`); `aplay -l` then `speaker-test -D plughw:N,0` (or `hdmi:CARD=N,DEV=0`);
   `/run/rsos/frontend.log` starts with `board <name> (/etc/rsos/board.ini)`; `/data/rsos/logs/boot<N>/` has the boot
   logs on the data partition.

## 6. Worked example: Raspberry Pi 4 (64-bit), community-tested

Files:

| File | Contents |
|---|---|
| `configs/rpi4_64_defconfig` | Cortex-A72 aarch64, Bootlin aarch64 glibc toolchain, the Raspberry Pi kernel and firmware of Buildroot's `raspberrypi4_64_defconfig`, Mesa v3d + vc4 (kmsro) with EGL/GLES/GBM, the RetroStone2's packages and all 25 cores |
| `board/rpi4/config.txt` | 64-bit, `dtoverlay=vc4-kms-v3d`, no splash, no boot delay, UART on GPIO 14/15, the 3.5 mm jack off |
| `board/rpi4/cmdline.txt` | `root=/dev/mmcblk0p2 rootfstype=ext4 rootwait ro quiet console=serial0,115200 consoleblank=0 rsos.slot=a` |
| `board/rpi4/linux.fragment` | vc4, v3d and ASoC built in, the firmware cpufreq driver, GENET built in, WiFi/BT as modules |
| `board/rpi4/genimage.cfg.in`, `post-image.sh` | boot FAT (128 MiB: firmware, `config.txt`, `cmdline.txt`, `Image`, device trees, overlays) as MBR entry 1, rootfs A and B (512 MiB each), the data seed as entry 4 |
| `board/rpi4/rootfs-overlay/etc/rsos/board.ini` | HDMI only (`internal_display = none`), no battery, no built-in pad, `audio_hdmi_pcm = hdmi`, `data_partition = 4` |

Build: `make O=~/rsos/output-rpi4 BR2_EXTERNAL=... rpi4_64_defconfig && make O=~/rsos/output-rpi4`; the image is
`~/rsos/output-rpi4/images/sdcard.img` (published as `images/retrostoneos-rpi4-<date>.img`). Flash it like the
RetroStone2 image.

What to expect, and what nobody has checked on a Pi yet:

- The menu on the TV at 720p (the HDMI policy), on either HDMI port. With no TV connected at boot, the first port is
  lit anyway and the menu moves to the TV when it is plugged in (display-design.md §3).
- USB pads and keyboards (and Bluetooth pads once Bluetooth is on). No power button: power off from the menu (a
  GPIO power button overlay that registers `KEY_POWER` works as the power key).
- HDMI audio through alsa-lib's `hdmi:` PCM of the vc4-hdmi card of the port in use. TODO(hw): check that
  `/usr/share/alsa/cards/vc4-hdmi.conf` is in the image and that the stream opens at 48 kHz.
- The N64 cores need Mesa v3d GLES 2.0 through kmsro from the vc4 KMS device. TODO(hw).
- WiFi/Bluetooth: `brcmfmac` and `hci_uart` from rsos-net, with the CYW43455 firmware of
  `brcmfmac_sdio-firmware-rpi`. TODO(hw).
- No A/B updates yet: rootfs B is reserved. The Pi firmware can do it with `autoboot.txt` and `tryboot` (two boot
  partitions); that is a follow-up. Settings > System update still tells about new versions and says to flash the
  new image (docs/updates.md 4.7).
- Boot time was tuned for the RetroStone2 only.

## 7. What is RetroStone2-specific (and optional for other boards)

| Part | Where | For another board |
|---|---|---|
| Kernel patches 0001-0006: sun4i HDMI hotplug polling, sun4i HDMI audio, the sun4i backend 2x/4x integer scaler, the exFAT read-ahead plug, the A20 CCU initcall, the frontend output port (HDMI on TCON1 while the panel keeps TCON0) | `board/retrostone2/patches/linux/` (docs/kernel-patches.md) | Not needed: the frontend probes what the display accepts and falls back to its CPU scaler. The exFAT patch is generic but optional. |
| LCD retiming to 60 Hz (the 78.571 Hz panel), the A20 TCON0 clock model in the logs | frontend display layer, `internal_refresh_options`, `display_quirks` | Only if the board's panel has a similar choice. |
| Panel safety: the panel keeps scanning (black frame, backlight off) whenever it is not the picture | frontend display layer, `display_quirks` (keep-scanning by default; `panel-power-switched` opts out) | The default: opt out only for a board whose built-in panel is powered off with it (display-design.md §8.5). |
| AXP209: charge-mode boot reason, power key timings, V_OFF, shutdown time stamps (`axpstamp`) | `rsos-bootreason` (`boot_reason = axp209`), `power_key_device`/`pek_startup_ms`, `battery_voff_mv`, `board/retrostone2/tools/` | Leave the keys out. |
| U-Boot A/B boot (`boot.cmd`, the redundant environment, `fw_env.config`, the watchdog started by U-Boot), bootstage time stamps and the ARM counter tool `cntvct` for the boot time line; tests `tests/boot-ab-qemu-test.sh` (QEMU cubieboard) and `tests/data-partition-test.sh` | `board/retrostone2/` | Only with U-Boot. (The data partition test is generic: it only uses the RetroStone2 image.) |
| The eMMC/SATA overlays of the "Pro" variant | `storage_overlays`, `board/retrostone2/dts/overlays/` | Leave out. |
| AP6210/AP6212 WiFi/BT firmware names, WiFi power-down by SDIO host unbinding | `board/retrostone2/post-build.sh`, `wifi_sdio_host` | Board firmware and keys of its own. |
| The codec mixer level, the SATA module | `board/retrostone2/rootfs-overlay/etc/rsos/board-hooks.sh` | Hooks of its own, or none. |

The name of the backup folder on USB drives (`RetroStone2/`, with `RetroStone2-backup.txt`) is a data format, not a
board property: it stays the same on every board so that any RetroStoneOS device imports it.

## 8. Cores on other architectures

`package/libretro-common.mk` gives each core its `platform=` and dynarec options per architecture (ARMv7 Cortex-A7,
aarch64 Cortex-A72/A53/other, x86_64); see cores.md, "Architectures". The RetroStone2's command lines are unchanged,
byte for byte.

## 9. The other boards (Raspberry Pi 2/3/5, Orange Pi H3, Orange Pi 5)

The support matrix, the images and what to expect of each board are in [boards.md](boards.md). How they are built:

| Part | Raspberry Pi 2 / 3 / 5 | Orange Pi H3 (PC, PC Plus, One, Lite), Orange Pi 5 |
|---|---|---|
| Board folder | `board/rpi2/`, `board/rpi3/`, `board/rpi5/`: `config.txt`, `cmdline.txt`, `linux.fragment`, `board.ini` | `board/orangepi-h3/` (shared by the two H3 defconfigs), `board/orangepi5/`: `linux.fragment`, `uboot.fragment`, `patches/` (the Linux/U-Boot hashes; the H3 device tree patch), `post-build.sh`, `genimage.cfg`, `post-image.sh`, `board.ini`, `fw_env.config` |
| Shared with | `board/rpi4/post-image.sh` and `genimage.cfg.in` (every Raspberry Pi port uses them) | `board/sbc-uboot/`: the RetroStone2's A/B `boot.cmd` made generic (`boot.cmd.in`, filled in by `mk-boot-scr.sh` from the board's `post-build.sh`: kernel file and boot command, U-Boot and Linux names of the SD card, console, default device tree) and the U-Boot options every such board needs (`uboot.fragment`: bootcmd, redundant environment in raw MMC, `setexpr`) |
| Kernel | the Raspberry Pi kernel of Buildroot's defconfig (`bcm2709`, `bcm2711`, `bcm2712` + Buildroot's 4 KiB-page fragment) | mainline 6.18.54 (the RetroStone2's), `sunxi_defconfig` / the arm64 defconfig restricted to Rockchip |
| Boot | the Pi firmware; `root=/dev/mmcblk0p2`, one root, **rootfs B reserved** (as on the Pi 4) | mainline U-Boot 2026.07, **A/B** as on the RetroStone2 (`rsos_slot`, counting, `rsos-boot-ok`, fallback) |
| SD card layout | boot FAT = MBR entry 1 (the Pi firmware boots from it), rootfs A/B = 2/3, data seed = **entry 4** (`data_partition = 4`) | boot loader in the raw area, U-Boot environment (2 x 64 KiB), rootfs A/B = 2/3, data seed = **entry 1** (physically last), as on the RetroStone2 |
| GPU (Mesa) | vc4 (Pi 2/3), v3d + vc4 (Pi 4/5) | lima (H3), panfrost (Orange Pi 5, through Panthor) |

Things a port can reuse from these:

- **The data partition entry.** Entry 1 for the data seed only works when nothing needs entry 1 itself; the Pi
  firmware boots from the first FAT partition of the MBR, so the Pi ports keep their boot FAT there and the data
  seed is entry 4. `data-partition` handles both (`data_partition`).
- **A/B on another U-Boot board**: set the environment offsets in the board's `uboot.fragment` and the same
  values in `/etc/fw_env.config` (outside the boot loader and the partitions), call
  `board/sbc-uboot/mk-boot-scr.sh` from the board's `post-build.sh`, enable `BR2_PACKAGE_UBOOT_TOOLS_FWPRINTENV`
  and `BR2_PACKAGE_HOST_UBOOT_TOOLS`, and install the kernel and device trees in `/boot`
  (`BR2_LINUX_KERNEL_INSTALL_TARGET`). `boot.cmd` loads `/boot/<rsos_fdtfile or U-Boot's fdtfile>`: a sister board
  that boots with the same U-Boot (the Orange Pi PC Plus with the PC image, the Lite with the One image) selects
  its own device tree with `fw_setenv rsos_fdtfile <name>.dtb` (a missing file falls back to the board's own).
  `board/sbc-uboot/tests/boot-ab-qemu-test.sh` tests the script on QEMU's Cubieboard (20 checks).
- **A driver that loads firmware when it probes** (Panthor on the Orange Pi 5) cannot be built in without an
  initramfs: build it as a module and load it from `rsos_board_late_audio` in the board's `board-hooks.sh` (the
  first hook after the menu is up; a dedicated "late modules" hook would be cleaner, see boards.md, follow-ups).
- **The kernel configuration check**: after the first build, compare the fragment lines with
  `output-<board>/build/linux-*/.config` (an `=m` that the menu needs, or an option whose dependency is missing,
  e.g. the lockup detectors need `DEBUG_KERNEL`, which `sunxi_defconfig` does not set).
