# Supported boards

RetroStoneOS is made for the **RetroStone2** handheld. The same system (the frontend, the libretro cores, the init
scripts) also builds for a few single-board computers, as an HDMI box with USB or Bluetooth pads. How a port is put
together is in [porting.md](porting.md); this page says which boards there are, what to flash and what to expect.

"Community-tested" means: **the image builds and its contents were checked (and the frontend was run under
qemu-user with the board's profile), but the RetroStoneOS developers have not booted it on the board.** Reports
(serial console log, `/data/rsos/logs/`) and fixes are welcome.

## Support matrix

| Board(s) | Defconfig | Image | CPU | GPU (Mesa driver) | Status |
|---|---|---|---|---|---|
| RetroStone2 | `retrostone2_defconfig` | `retrostoneos-<version>-retrostone2.img.xz` | Allwinner A20, 2x Cortex-A7 1 GHz, 32-bit | Mali-400 MP2 (lima) | tested on hardware |
| Raspberry Pi 2; Pi 3 / 3B+ / Zero 2 W in 32-bit mode | `rpi2_defconfig` | `retrostoneos-<version>-rpi2.img.xz` | BCM2836/2837, 4x Cortex-A7 900 MHz (Pi 3: 4x A53 1.2-1.4 GHz running 32-bit code) | VideoCore IV (vc4) | builds, community-tested |
| Raspberry Pi 3 / 3B+ / Zero 2 W (64-bit) | `rpi3_64_defconfig` | `retrostoneos-<version>-rpi3-64.img.xz` | BCM2837, 4x Cortex-A53 1.2-1.4 GHz (Zero 2 W: 1 GHz, 512 MB) | VideoCore IV (vc4) | builds, community-tested |
| Raspberry Pi 4 / 400 / CM4 | `rpi4_64_defconfig` | `retrostoneos-<version>-rpi4-64.img.xz` | BCM2711, 4x Cortex-A72 1.5-1.8 GHz | VideoCore VI (v3d + vc4) | builds, community-tested |
| Raspberry Pi 5 / 500 | `rpi5_64_defconfig` | `retrostoneos-<version>-rpi5-64.img.xz` | BCM2712, 4x Cortex-A76 2.4 GHz | VideoCore VII (v3d + vc4) | builds, community-tested |
| Orange Pi PC, PC Plus | `orangepi_h3_pc_defconfig` | `retrostoneos-<version>-orangepi-h3-pc.img.xz` | Allwinner H3, 4x Cortex-A7 1.2-1.3 GHz | Mali-400 MP2 (lima) | builds, community-tested |
| Orange Pi One, Lite | `orangepi_h3_one_defconfig` | `retrostoneos-<version>-orangepi-h3-one.img.xz` | Allwinner H3, 4x Cortex-A7 1.2 GHz | Mali-400 MP2 (lima) | builds, community-tested |
| Orange Pi 5 | `orangepi5_defconfig` | `retrostoneos-<version>-orangepi5.img.xz` | Rockchip RK3588S, 4x Cortex-A76 2.4 GHz + 4x A55 | Mali-G610 MP4 (panfrost, Panthor kernel driver) | builds, community-tested |

The single-board computer images are **only produced by the CI** (the release workflow builds every defconfig
without a `# ci: skip` line, `scripts/ci/list-boards.sh`); local builds of these boards are compile and
configuration checks. The images are xz-compressed (`xz -dk`, or let balenaEtcher decompress them). Flash them like
the RetroStone2 image (docs/build.md, "Flashing"): the first boot grows the `RETROSTONE` data partition to the whole
card.

Every board here is an **HDMI box**: `internal_display = none`, no battery, no built-in pad
(`builtin_pad_prefix` empty), a power key only if some input device reports `KEY_POWER` (the Pi 5 power button
does). The sound follows the display, so it goes to the HDMI card; the analog jacks (Orange Pi PC, Orange Pi 5)
are in the kernel but not used by the frontend yet.

## What to expect: emulation by board class

The cores and the frontend are the same everywhere; the CPU and the GPU decide what runs at full speed. Only
the two N64 cores use the GPU (GLES 2.0); every other core renders in software.

| Class | Boards | Expected |
|---|---|---|
| **A: Cortex-A7, 32-bit** | RetroStone2, Raspberry Pi 2, Orange Pi H3 (and a Pi 3 with the 32-bit image) | Like the RetroStone2: the 8/16-bit consoles and handhelds, GBA (gpsp dynarec), PlayStation (pcsx_rearmed dynarec + NEON GPU), Neo Geo and most CPS1/CPS2 arcade games, the computers (DOSBox for the lighter games). N64: only light games, with frame skipping. The H3 and the Pi 2 have four cores and a higher clock than the A20: a little more headroom. |
| **B: Cortex-A53, 64-bit** | Raspberry Pi 3, 3B+, Zero 2 W | Class A and a bit more (PlayStation with enhancements off, more arcade games); the vc4 GPU is weak for N64 (light games only). The Zero 2 W's 512 MB is tight for N64 and DOSBox. |
| **C: Cortex-A72** | Raspberry Pi 4 / 400 | Everything in the core list at full speed, including most N64 games (mupen64plus-next, GLES 2.0 on v3d). Later candidates: NDS (melonDS/DeSmuME), Dreamcast (flycast, needs GLES 3 in the host) with a part of the library. |
| **D: Cortex-A76** | Raspberry Pi 5, Orange Pi 5 | N64 well, with upscaling; the room for PSP (PPSSPP), NDS, Dreamcast and Saturn cores once they are packaged (PPSSPP and flycast need GLES 3 contexts, which the host does not offer yet: host-design.md §13). |

## Per-board notes

### Raspberry Pi 2 (32-bit image), also Pi 3 / Zero 2 W in 32-bit mode

- Buildroot's `raspberrypi2_defconfig` parts: the Raspberry Pi kernel (6.12, `bcm2709_defconfig`), the Pi 0-3
  firmware (`bootcode.bin`, `start.elf`). The device trees of the Pi 2 (both revisions), 3, 3B+, Zero 2 W and CM3
  are on the boot partition: the firmware picks the board's own.
- The CPU class is the RetroStone2's: the cores are the same `arm-a7` builds (`platform=rpi2` and friends), which
  also run on the Pi 3's Cortex-A53 in 32-bit mode.
- Display and GPU: vc4 (`dtoverlay=vc4-kms-v3d`); HDMI audio through the `vc4-hdmi` card and alsa-lib's `hdmi`
  PCM (`audio_hdmi_pcm = hdmi`). The 3.5 mm jack is off.
- Serial console on GPIO 14/15, `ttyAMA0`, 115200 (on a Pi 3 / Zero 2 W, `miniuart-bt` moves Bluetooth to the
  mini UART so that the console is the PL011 on every model).
- Ethernet (USB SMSC95xx / LAN78xx) built in; WiFi/Bluetooth (Pi 3 / Zero 2 W only) as modules for `rsos-net`.
- Watchdog: `bcm2835_wdt` built in. `gpio-keys` built in, so a GPIO power button overlay works as the power key.
- Root file system slots: 512 MiB (about 360 MiB used, 70 MiB of it kernel modules).

### Raspberry Pi 3 / 3B+ / Zero 2 W (64-bit image)

- **One image for the three boards** (and the CM3): the firmware picks the device tree. Buildroot's
  `raspberrypi3_64_defconfig` parts (the Raspberry Pi kernel with `bcm2711_defconfig`, the Pi 0-3 firmware).
- `BR2_cortex_a53`: the cores use their `rpi3_64` platforms (cores.md, "Architectures").
- WiFi firmware for the BCM43430 (3B), CYW43455 (3B+) and BCM43436/43430B0 (Zero 2 W) from
  `brcmfmac_sdio-firmware-rpi`. Console on `ttyAMA0` (GPIO 14/15, `miniuart-bt`).
- The Zero 2 W has 512 MB of RAM and a mini-HDMI port; the vc4 CMA area comes out of that RAM.
- Root file system slots: **768 MiB** (about 445 MiB used: the 64-bit Raspberry Pi kernel brings about 100 MiB of
  modules, and 512 MiB left too little room for more cores). The Raspberry Pi genimage template takes the slot
  size from `BR2_TARGET_ROOTFS_EXT2_SIZE`.

### Raspberry Pi 4 / 400 / CM4

The reference port, unchanged in its layout (porting.md, section 6). Review notes (this round):

- The kernel `.config` of the last Pi 4 build did not have the lockup detectors of `linux-rsos.fragment`
  (`SOFTLOCKUP_DETECTOR`, `HARDLOCKUP_DETECTOR`): that build predates the robustness block of the common
  fragment. A rebuild (`make O=~/rsos/output-rpi4 linux-dirclean all`) picks them up; check the `.config`.
- `gpio-keys` (`CONFIG_KEYBOARD_GPIO`) was a module: the GPIO power button overlay that porting.md and
  `board.ini` suggest would never have registered. Now built in (`board/rpi4/linux.fragment`), with
  `BCM2835_WDT` spelled out there too.
- `BR2_DOWNLOAD_FORCE_CHECK_HASHES=y` added, as in the other Raspberry Pi defconfigs (they build with it).
- Its post-image script and genimage template are now shared by all the Raspberry Pi ports; the slot size follows
  `BR2_TARGET_ROOTFS_EXT2_SIZE` (still 512 MiB for the Pi 4: the same layout as before). With about 100 MiB of
  modules the Pi 4 root file system is as full as the Pi 3's (about 445 MiB): consider 768 MiB there too when
  the next cores land.
- Everything else (vc4 + v3d built in, GENET, `bcm2835_wdt`, `audio_hdmi_pcm = hdmi`, `vc4-hdmi.conf` in the
  image) checked out.

### Raspberry Pi 5 / 500

- Buildroot's `raspberrypi5_defconfig` parts: the Raspberry Pi kernel with `bcm2712_defconfig` **and 4 KiB pages**
  (Buildroot's `linux-4k-page-size.fragment`; the cores' dynarecs assume 4 KiB pages), no GPU firmware on the card
  (the EEPROM boot loader loads the kernel).
- USB, Ethernet and the GPIO UARTs are behind the RP1 chip on PCIe: `PCIE_BRCMSTB`, `MFD_RP1`, DWC3/xHCI and
  `MACB` are built in so that pads work without module loading.
- Serial console: the 3-pin debug connector (`ttyAMA10`, 115200). The power button registers `KEY_POWER`
  (`pwr_button`, `gpio-keys` built in): the frontend uses it as its power key.
- `BR2_cortex_a76`: the cores use their generic aarch64 build.
- Root file system slots: 768 MiB (about 445 MiB used), as the Pi 3.

### Orange Pi PC / PC Plus and Orange Pi One / Lite (Allwinner H3)

- **Two images**, because the U-Boot SPL (DRAM clock, the PC's SY8106A CPU regulator) differs: `orangepi_h3_pc`
  (PC, PC Plus) and `orangepi_h3_one` (One, Lite). One board folder, `board/orangepi-h3/`.
- Mainline Linux 6.18.54 (`sunxi_defconfig`) and U-Boot 2026.07, the RetroStone2 versions. **A/B root file
  systems with the RetroStone2's boot logic** (`board/sbc-uboot/boot.cmd.in`): boot counting, `rsos-boot-ok`,
  fallback to the other slot, the U-Boot environment at 1 MiB (`fw_printenv`/`fw_setenv`). U-Boot starts the
  watchdog (16 s) and the kernel's `sunxi_wdt` takes it over. SD card layout as on the RetroStone2 (data seed
  = MBR entry 1, physically last).
- **The PC Plus and the Lite** boot with the PC / One image and the PC / One device tree (no eMMC / WiFi in the
  device tree). Their own device trees are in `/boot`: `fw_setenv rsos_fdtfile sun8i-h3-orangepi-pc-plus.dtb`
  (or `sun8i-h3-orangepi-lite.dtb`) from the serial console selects them. Their RTL8189FTV WiFi has no mainline
  driver: no WiFi (`wifi_module = none`); a USB Bluetooth dongle works (`bt_module = btusb`).
- Display: sun4i-drm (DE2 mixer + sun8i DesignWare HDMI), GPU lima (Mali-400 MP2), the RetroStone2's GPU.
- **HDMI audio**: mainline has the I2S2 -> HDMI path but no sound card for it in the H3 device tree;
  `board/orangepi-h3/patches/linux/0001-...` adds the `allwinner-hdmi` simple-audio-card (the LibreELEC/Armbian
  one) and `mmc` aliases (the SD card is always `mmcblk0`). TODO(hw): check that it plays.
- Console: UART0 (the 3-pin header), `ttyS0`, 115200.
- CPU class A (the cores are the RetroStone2's `arm-a7` builds). Root file system slots: 512 MiB (about 300 MiB
  used: `sunxi_defconfig` builds almost everything in, 3 MiB of modules).
- The A/B boot script is tested on QEMU (`board/sbc-uboot/tests/boot-ab-qemu-test.sh`: U-Boot 2026.07 for the
  Cubieboard with the shared and H3 fragments, fake kernels): counting, fallback, no ping-pong, empty slot B,
  corrupt kernels, trial slot, `rsos_maxfails=0`, `rsos_fdtfile` and its fallback, the watchdog (20 checks).

### Orange Pi 5 (RK3588S)

- Mainline Linux 6.18.54 (arm64 defconfig restricted to Rockchip) and U-Boot 2026.07 (`orangepi-5-rk3588s`, with
  TF-A LTS 2.12 and the Rockchip DDR blob, as Buildroot's `orangepi_5_plus_defconfig`). **A/B** as on the H3
  boards; the U-Boot environment sits at 14 MiB (after `u-boot-rockchip.bin`), the partitions start at 16 MiB.
- Display: VOP2 + DesignWare HDMI QP + Samsung HDPTX PHY (HDMI0, mainline since 6.13), HDMI audio through the
  `HDMI0` card (I2S5), the ES8388 codec on the 3.5 mm jack.
- GPU: Mali-G610 with **Panthor**, a module that the board hook loads once the menu is up (it loads its CSF
  firmware from `/lib/firmware` when it probes, which a built-in driver would do before the root file system is
  mounted); Mesa panfrost. Only the N64 cores need it.
- In Buildroot 2026.02, Mesa's panfrost driver depends on `BR2_PACKAGE_MESA3D_LLVM` (its shaders are precompiled
  with mesa-clc): the build compiles LLVM and Clang for the host and the target (about an hour more on the first
  build) and installs them; `board/orangepi5/post-build.sh` removes everything but `libLLVM` (which `libgallium`
  links), about 200 MiB. Root file system slots: **1 GiB** (about 490 MiB used), room for the heavier cores this
  class can run.
- **The SPI flash must be empty**: the RK3588 boot ROM tries it before the SD card, and a boot loader there
  (Orange Pi's NVMe images write one) would not run RetroStoneOS's boot command. TODO(hw).
- Console: UART2 (the 3-pin header), `ttyS2`, **1500000** baud. No on-board WiFi/Bluetooth.
- TODO(hw): the whole port (HDMI modes, HDMI audio, the USB-C port with FUSB302, cpufreq).

## Dropped boards

| Board | Why |
|---|---|
| Orange Pi Zero / Zero LTS (H2+) | **No video output**: the board has no HDMI connector, only composite TV-out on the expansion board, which mainline does not drive. Nothing for a frontend to show. |
| Orange Pi Zero 3 / Zero 2W (H618) | **No HDMI in mainline 6.18**: the kernel has the DE 3.3 mixer (`sun50i-h616-de33-mixer-0`) but not the H616 TCON-TOP / HDMI PHY support, and the H616 device tree has no display pipeline at all (only the Mali-G31 GPU node). Porting that series (and HDMI audio) is a kernel project of its own. The WiFi (UWE5622 / AW859A) has no mainline driver either. To revisit on a kernel with the H616 display series merged. |

## Checks done for every board (local builds)

Each defconfig was built from scratch in its own output directory (`~/rsos/output-<board>`) with Buildroot
2026.02.3, and:

- `scripts/ci/check-defconfigs.sh <board>`: every line of the defconfig survives kconfig;
- the frontend and all 35 cores are built for the board's architecture (`file`), their `.ini` files installed;
- the kernel `.config` has the display, GPU, HDMI audio, HID/gamepad, watchdog and lockup-detector options
  built in (`=y`) and the network drivers as modules;
- the SD card image has the expected MBR layout, the boot partition (Raspberry Pi) or `/boot` (U-Boot boards)
  the kernel, device trees, `boot.scr` and the firmware; `/etc/rsos/board.env` and the UART shell line of
  `/etc/inittab` match `board.ini`;
- the image's own `rsos-frontend` ran headless under qemu-user (`qemu-arm` / `qemu-aarch64` with the target
  root as the library prefix) with the board's `board.ini`: the menu came up, a NES ROM started with the image's
  fceumm core and ended cleanly.

Images are not published from these builds: the release CI produces them.

## Follow-ups

- **Hardware reports** for every board here (HDMI modes and hotplug, HDMI audio at 48 kHz, the N64 cores on the
  GPU, WiFi/Bluetooth, the power key, boot time).
- A generic `rsos_board_late_modules` hook in rcS (the Orange Pi 5 loads Panthor from `rsos_board_late_audio`).
- A/B updates on the Raspberry Pi (`autoboot.txt` + `tryboot`, with two boot partitions): rootfs B is reserved.
- An "audio output: HDMI / analog jack" setting for the HDMI boxes whose analog output is in the kernel (Orange Pi
  PC, Orange Pi 5); the frontend's sound follows the display today.
- USB WiFi dongles: no driver is loaded without udev; a `wifi_module` per dongle, or a hotplug helper.
- The Orange Pi Zero 3 / Zero 2W when mainline gains the H616 display pipeline.
- The Pi 4 slot size (512 MiB, about 445 MiB used) once more cores are added (see its notes).
