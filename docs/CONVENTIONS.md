# RetroStoneOS: project conventions

RetroStoneOS is a minimal, fast-booting firmware for the **RetroStone2** handheld (8BCraft), replacing RetrOrangePi
(Armbian + RetroPie + RetroArch + EmulationStation).

## Goals
1. Boot very fast (target: 3-6 s from power-on to the menu).
2. Run emulators smoothly: no background daemons, KMS-direct video with hardware plane scaling, ALSA-direct audio.
3. Switch LCD and HDMI live (switch, never mirror). HDMI plugged in: LCD and backlight off, picture on the TV. Unplugged: back to the LCD.
4. One EmulationStation-like UI with one page per system and all settings in one place.

## Principle: nothing in the boot path that isn't needed to play
Boot speed is the top priority. Anything not needed to play a game stays out of the boot path and is
opt-in:
- **SATA (M.2) and eMMC**: disabled in the base DTS. Enabled by overlays listed in `/boot/rsos.env` (`overlays=emmc sata`), which needs a reboot.
- **WiFi, Ethernet, Bluetooth**: drivers are modules that aren't loaded at boot, and the chips are powered down. Toggled at runtime from the UI
  through `rsos-net {wifi|eth|bt} {on|off}`, persisted in `/data/rsos/settings.ini`. If enabled, they're applied in the background
  after the menu is up, never before.

## Architecture
- **Buildroot** (BR2_EXTERNAL tree in `buildroot-external/`) produces one SD card image.
- **Mainline U-Boot** with bootdelay=0 and no U-Boot video (kept minimal for speed).
- **Mainline Linux LTS** with a custom DTS and a few patches: drivers built in (`=y`), no initramfs, no modules where avoidable.
- **Init**: BusyBox init, or a tiny custom init that starts the frontend directly. No systemd, no udevd if avoidable
  (use devtmpfs and read hotplug uevents from netlink directly), no X11/Wayland, no login.
- **Frontend** (`frontend/`): a single C binary that owns DRM/KMS. It handles connector hotplug, hosts
  libretro cores via dlopen (no RetroArch) and draws the UI.
- **Emulation**: libretro cores, packaged in `buildroot-external/package/libretro-*`.

## Pinned versions
| Component | Version |
|---|---|
| Buildroot | 2026.02.3 (LTS) |
| Linux | 6.18.54 (longterm), DTS path `arch/arm/boot/dts/allwinner/` |
| U-Boot | v2026.07 |

## Hardware: RetroStone2
- SoC: Allwinner A20 (sun7i), 2x Cortex-A7 @ 1 GHz, Mali-400 MP2 (lima), 1 GB DDR3.
- PMIC: AXP209 on I2C0 @0x34 (battery, charger, ADC for the analog stick on AXP GPIO0/GPIO1).
- LCD: 3.5" 640x480 parallel RGB on TCON0. Old timings: pclk 33 MHz, hfp 16, hsync 30, hbp 114, vfp 10, vsync 3, vbp 32,
  negative hsync/vsync. The backlight is PWM0 (PB2).
- HDMI: on-chip sun4i HDMI encoder on TCON1. There is no HPD interrupt, so it is polled.
- Storage: microSD on mmc0 (card-detect PH1). The old DTS also declared eMMC on mmc2; verify it exists.
- WiFi: Broadcom SDIO on mmc3 (brcmfmac). Bluetooth: Broadcom BCM20702 on UART2 (PI pins).
- Buttons: A20 GPIOs, active-low with pull-ups (map in `docs/hardware-pinmap.md`).
- Debug UART: uart0 on PB22/PB23, 115200 8N1.
- Reference material: the old RetrOrangePi board files are in `reference/retrostone-rop/` (a local copy, not in the public repository). **Their pin assignments contradict
  each other (PH7, PH8, PH12); the schematic is the source of truth.** The Eagle files are in [`hardware/`](../hardware/).

## Build host
- Windows 11 + WSL2 `Ubuntu-24.04` (16 cores, passwordless sudo). Run commands with
  `wsl.exe -d Ubuntu-24.04 -- bash -c '...' 2>&1 | tr -d '\0'`.
- The repository is checked out on the Windows side (for example `/mnt/c/Users/<you>/RetroStoneOS` from WSL). Sources and configs live there.
- **Heavy build output lives in WSL ext4 at `~/rsos/`** (never under /mnt/c):
  - `~/rsos/buildroot-2026.02.3/`: Buildroot source
  - `~/rsos/output/`: Buildroot `O=` output directory
  - `~/rsos/dl/`: Buildroot download cache (`BR2_DL_DIR`)
  - `~/rsos/src/linux-6.18/`: a plain shallow clone of v6.18.54 for DTS and patch development
    (`~/rsos/src/linux-6.18.READY` exists when the clone is complete)
- Host cross compiler for quick compile checks: `arm-linux-gnueabihf-gcc` (Ubuntu package).
- A background WSL process only survives the `wsl.exe` call if it is started with `setsid -f`, for example
  `setsid -f bash -c 'make ... > ~/rsos/build.log 2>&1; echo $? > ~/rsos/build.exit' < /dev/null > /dev/null 2>&1`.

## Repo layout and ownership
| Path | Contents |
|---|---|
| `buildroot-external/` (`external.desc`, `external.mk`, `Config.in`, `configs/`) | Buildroot glue and defconfig |
| `buildroot-external/board/common/` | what every board shares: init scripts (rcS, rcK, data-partition, rsos-net, bootlog), post-build, data seed, BusyBox and generic kernel fragments (docs/porting.md) |
| `buildroot-external/board/retrostone2/` | genimage, boot script, post-build/post-image scripts, kernel/U-Boot config fragments, rootfs overlay with `etc/rsos/board.ini` (the board profile) |
| `buildroot-external/board/rpi4/`, `configs/rpi4_64_defconfig` | the community-tested Raspberry Pi 4 (64-bit) port |
| `buildroot-external/board/retrostone2/dts/` | the RetroStone2 device tree(s) |
| `buildroot-external/board/retrostone2/patches/linux/` | kernel patches (git format-patch style, numbered) |
| `buildroot-external/board/retrostone2/linux-patches.fragment` | kernel config options that the kernel patches need |
| `buildroot-external/package/rsos-frontend/` | Buildroot package for `frontend/` |
| `buildroot-external/package/libretro-*/` | libretro core packages |
| `frontend/` | the C frontend (display, input, audio, libretro host, UI) |
| `docs/` | design notes and hardware docs |

## Rules
- Write files with LF line endings. Shell scripts need `#!/bin/sh` and must be POSIX.
- Do **not** run `git commit`. The lead integrates and commits.
- Stay inside the paths your task owns. If you need a change elsewhere, say so in your final report instead of making it.
- Mark anything that needs checking on real hardware or against the schematic with a `TODO(hw):` comment.
