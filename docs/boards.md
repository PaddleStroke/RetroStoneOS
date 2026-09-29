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
| RetroStone1 | `retrostone1_defconfig` (release: `retrostone1_release_defconfig`) | `retrostoneos-<version>-retrostone1.img.xz` | Allwinner H3, 4x Cortex-A7 1.0-1.2 GHz, 32-bit | Mali-400 MP2 (lima) | **untested, built by CI; the built-in screen (composite, kernel patch from Armbian) is compile-tested only** |
| Raspberry Pi 2; Pi 3 / 3B+ / Zero 2 W in 32-bit mode | `rpi2_defconfig` | `retrostoneos-<version>-rpi2.img.xz` | BCM2836/2837, 4x Cortex-A7 900 MHz (Pi 3: 4x A53 1.2-1.4 GHz running 32-bit code) | VideoCore IV (vc4) | builds, community-tested |
| Raspberry Pi 3 / 3B+ / Zero 2 W (64-bit) | `rpi3_64_defconfig` | `retrostoneos-<version>-rpi3-64.img.xz` | BCM2837, 4x Cortex-A53 1.2-1.4 GHz (Zero 2 W: 1 GHz, 512 MB) | VideoCore IV (vc4) | builds, community-tested |
| Raspberry Pi 4 / 400 / CM4 | `rpi4_64_defconfig` | `retrostoneos-<version>-rpi4-64.img.xz` | BCM2711, 4x Cortex-A72 1.5-1.8 GHz | VideoCore VI (v3d + vc4) | builds, community-tested |
| Raspberry Pi 5 / 500 | `rpi5_64_defconfig` | `retrostoneos-<version>-rpi5-64.img.xz` | BCM2712, 4x Cortex-A76 2.4 GHz | VideoCore VII (v3d + vc4) | builds, community-tested |
| Orange Pi PC, PC Plus | `orangepi_h3_pc_defconfig` | `retrostoneos-<version>-orangepi-h3-pc.img.xz` | Allwinner H3, 4x Cortex-A7 1.2-1.3 GHz | Mali-400 MP2 (lima) | builds, community-tested |
| Orange Pi One, Lite | `orangepi_h3_one_defconfig` | `retrostoneos-<version>-orangepi-h3-one.img.xz` | Allwinner H3, 4x Cortex-A7 1.2 GHz | Mali-400 MP2 (lima) | builds, community-tested |
| Orange Pi 5 | `orangepi5_defconfig` | `retrostoneos-<version>-orangepi5.img.xz` | Rockchip RK3588S, 4x Cortex-A76 2.4 GHz + 4x A55 | Mali-G610 MP4 (panfrost, Panthor kernel driver) | builds, community-tested |

The single-board computer images are **only produced by the CI** (the release workflow builds every defconfig
without a `# ci: skip` line, `scripts/ci/list-boards.sh`; on a tag they are release builds, with the tag's version);
local builds of these boards are compile and configuration checks. The images are xz-compressed (`xz -dk`, or let
balenaEtcher decompress them). Flash them like the RetroStone2 image (docs/build.md, "Flashing") on a card of
**2 GB or more (4 GB for the Orange Pi 5**, whose image is about 2.2 GB): the first boot grows the `RETROSTONE` data
partition to the whole card.

Apart from the RetroStone handhelds (the RetroStone1 has its built-in buttons and its composite-fed screen, see its
notes), every board here is an **HDMI box**: `internal_display = none`, no battery, no built-in pad
(`builtin_pad_prefix` empty), a power key only if some input device reports `KEY_POWER` (the Pi 5 power button
does). The sound follows the display, so it goes to the HDMI card; the analog jacks (Orange Pi PC, Orange Pi 5)
are in the kernel but not used by the frontend yet.

## What to expect: emulation by board class

The cores and the frontend are the same everywhere; the CPU and the GPU decide what runs at full speed. Only
the two N64 cores use the GPU (GLES 2.0); every other core renders in software.

| Class | Boards | Expected |
|---|---|---|
| **A: Cortex-A7, 32-bit** | RetroStone2, RetroStone1, Raspberry Pi 2, Orange Pi H3 (and a Pi 3 with the 32-bit image) | Like the RetroStone2: the 8/16-bit consoles and handhelds, GBA (gpsp dynarec), PlayStation (pcsx_rearmed dynarec + NEON GPU), Neo Geo and most CPS1/CPS2 arcade games, the computers (DOSBox for the lighter games). N64: only light games, with frame skipping. The H3 and the Pi 2 have four cores and a higher clock than the A20: a little more headroom. |
| **B: Cortex-A53, 64-bit** | Raspberry Pi 3, 3B+, Zero 2 W | Class A and a bit more (PlayStation with enhancements off, more arcade games); the vc4 GPU is weak for N64 (light games only). The Zero 2 W's 512 MB is tight for N64 and DOSBox. |
| **C: Cortex-A72** | Raspberry Pi 4 / 400 | Everything in the core list at full speed, including most N64 games (mupen64plus-next, GLES 2.0 on v3d). Later candidates: NDS (melonDS/DeSmuME), Dreamcast (flycast, needs GLES 3 in the host) with a part of the library. |
| **D: Cortex-A76** | Raspberry Pi 5, Orange Pi 5 | N64 well, with upscaling; the room for PSP (PPSSPP), NDS, Dreamcast and Saturn cores once they are packaged (PPSSPP and flycast need GLES 3 contexts, which the host does not offer yet: host-design.md §13). |

## Per-board notes

### RetroStone1 (8BCraft, Allwinner H3)

8BCraft's earlier handheld. Board folder `board/retrostone1/`, from the schematic revision 1.18
(`hardware/retrostoneH3-18.sch`, `RSN1-1.18.pdf`). The RetrOrangePi reference files (`reference/retrostone-rop/`)
only cover the RetroStone2, so everything below comes from the schematic. **Nobody has booted this image on a
RetroStone1 yet**; the CI builds it (`retrostone1_release_defconfig`; `retrostone1_defconfig` is the development
variant, as for the RetroStone2).

What the schematic shows, and how the port uses it:

| Part | Hardware | In RetroStoneOS |
|---|---|---|
| SoC, RAM | H3, 2x Samsung K4B4G1646D DDR3 = 1 GiB (32-bit); Orange Pi One/PC reference design | U-Boot `orangepi_one` + `board/retrostone1/uboot.fragment` (DRAM 624 MHz), mainline Linux 6.18 |
| CPU supply | quad buck U57, CPU voltage 1.1 / 1.3 V switched by **PL6** (the Orange Pi One scheme) | `regulator-gpio` on PL6, cpufreq-dt |
| Screen | **H3 composite TV-out (TVOUT) -> AMT630A (U$23) CVBS-to-RGB converter -> 54-pin FPC panel** (TVOUT, load R77, -> L7/C13 filter -> R28/R29 -> C87 -> AMT630A input CVBS1; JP1/TP7 on TVOUT). The AMT630A has its own firmware (SPI flash U$18): it sets the panel up over SPI, scales the CVBS picture to it, drives the backlight enable (KA2707 boost U$14) and reads the three side keys U$36-U$38 (its OSD / brightness keys) | sun4i-drm DE2 **mixer 1 -> TCON1 -> TV encoder**: kernel patch 0002 (the H3 TVE, ported from Armbian, docs/kernel-patches.md) and `dts/sun8i-h3-tve-pipeline.dtsi`. DRM connector "Composite-1", **NTSC 720x480i at 59.94 Hz** by default, PAL 720x576i as the fallback (board.ini `tv_norm`), 4:3 picture, optional overscan margin (`tv_overscan`). `internal_display = composite`: it is the built-in screen, switched with HDMI on hotplug like the RetroStone2 LCD. `backlight = none` (no Brightness slider: the AMT630A keys set it). **Compile-tested only** |
| HDMI | H3 DesignWare HDMI, HPD, DDC, CEC | sun4i-drm (DE2 mixer 0 + TCON0 + dw-hdmi), HDMI audio through I2S2 (card `allwinner-hdmi`, in the board DTS). With HDMI plugged in the menu moves to the TV and the composite output is switched off, as on the RetroStone2 |
| Buttons | D-pad PD0/5/11/12, A/B/X/Y = K2 PD14 / K4 PD15 / K1 PD13 / K3 PD9, Start PD1, Select PD4, extra pads SELECT3 PE3 / SELECT4 PC9, L1 PD8, R1 PD3, L2 PD6, R2 PD7 (JST U$4); all active low with 10k pull-ups | one `gpio-keys-polled` device "RetroStone1 Buttons" (5 ms; ports C/D/E have no interrupts on the H3) |
| Analog stick | optional, on JOYSTICK_CON, read by an **MCP3208** SPI ADC (U$35) on SPI1 (PA13-PA16): CH0 = Y, CH1 = X | `mcp320x` IIO + `adc-joystick` "analog-stick" (`builtin_stick`) |
| Power key | side tact switch **U$15 on PA1** | `gpio-keys` "Power Key" (`KEY_POWER`): a short press powers off cleanly |
| Power | **no PMIC**: MCP73871 linear charger with power path (1 A, micro-USB or the Energysquare pads), charge LEDs only; **slide switch U$6** cuts the system rail in hardware; LDO VR1 (3.3 V always on), buck U57 enabled by PL8 (PWR-STB: CPU, 3.3 V) and PL9 (PWR-DRAM), VDD-SYS by PL5 (low = on) | `gpio-poweroff` on PL8: after shutdown the CPU, the 3.3 V rail and the screen go off; the user then turns the slide switch off |
| Battery level | **no gauge**: MCP3208 CH2 is wired to VBAT without a divider, VREF = 3.3 V, so it reads full scale for any charged cell | no battery supply, no battery icon or overlay |
| Audio | H3 codec LINEOUT -> analog volume wheel U$27 -> headphone jack U$24, whose switch feeds the PAM8302A mono amplifier (U1-A, /SD tied to VBAT); no amplifier enable or headphone-detect GPIO | codec card "H3 Audio Codec", `allwinner,audio-routing = "Line Out", "LINEOUT"`; `board-hooks.sh` sets the line out level |
| Storage | microSD on mmc0, card detect PF6; no eMMC | `/dev/mmcblk0`, A/B layout of the Orange Pi H3 ports |
| USB | four USB-A ports (two stacked pairs) on USB0-USB3, VBUS always on (SY6280, EN tied to 5 V) | host mode on all four (USB0 = OTG controller in host mode) |
| Ethernet | RJ45 with magnetics (CON1) on the H3 internal PHY | `dwmac-sun8i` as a **module**, loaded by `rsos-net` only when Ethernet is turned on |
| WiFi / Bluetooth | none: PG0-PG5 (SDIO), PL0, PL7, PA11 only go to test pads for an optional module | `wifi_module = none`, `bt_module = btusb` (USB dongle) |
| LEDs | LED1 power (VBAT), LED2/LED4 charger status, LED3 USB power: none driven by the SoC | none in the DTS |
| Debug UART | header UART0-DBG, PA4 TX / PA5 RX, 115200 | `ttyS0`, root shell |

Kernel: `sunxi_defconfig` + the common fragment + `board/retrostone1/linux.fragment`; the device tree
`board/retrostone1/dts/sun8i-h3-retrostone1.dts` (with `sun8i-h3-tve-pipeline.dtsi`) is copied into the kernel tree
by `external.mk` (as the RetroStone2's). Patches (`board/retrostone1/patches/`): the Linux/U-Boot hashes, 0001 the
RetroStone2's exFAT directory read-ahead patch (docs/kernel-patches.md, 0004), which shortens the first menu scan,
and 0002 the H3 TV encoder (composite output) from Armbian's `sunxi-6.18` patches (docs/kernel-patches.md,
"RetroStone1 0002"). Root file system slots: 512 MiB.

**The built-in screen, how it works** (2026-09-28, compile-tested). Mainline 6.18 only drives the A10/A20 TV
encoder. Patch 0002 adds the H3 one: the TVE clock's hidden /16 post-divider, the H3 DAC calibration, the DE2 mixer 1
and the RGB -> YUV conversion (DCSC) the encoder needs. The dtsi describes mixer 1 -> TCON1 -> TVE next to mixer 0 ->
TCON0 -> HDMI; the frontend sees two connectors, "HDMI-A-1" (CRTC 0) and "Composite-1" (CRTC 1). Composite is the
internal display (board.ini `internal_display = composite`), so the menu starts on it, moves to HDMI when a TV is
plugged in and comes back when it is unplugged, and the sound follows (codec / HDMI). The frontend sets the
connector's "TV mode" with every modeset (NTSC for 720x480i, PAL for 720x576i), lays the menu out at 640x480 (the
picture is 4:3, so a 720-pixel line has 0.889-wide pixels) and scales it to 720x480, and scales games with the right
aspect ratio. Mixer 1 has only two planes, so the game goes on its VI plane and the FPS counter on the primary plane
above it (display-design.md §3.2). NTSC is the default for the lower latency (59.94 Hz fields, the rate of most
cores); `tv_norm = pal` in `/etc/rsos/board.ini` (or `auto` with `fw_setenv rsos_extraargs video=Composite-1:PAL`)
selects PAL, and a refused NTSC mode falls back to PAL by itself. `rsos-kmstest --tv ntsc|pal --list` / `--tv ntsc`
tests the output from the UART without the menu.

**TODO(hw), for the owner, on a real RetroStone1** (serial console on UART0-DBG):

1. **The built-in screen** (kernel patch 0002 + the composite support of the frontend; compile-tested only). There
   is no boot logo on the panel before the kernel (U-Boot has no H3 composite output): the first picture is the
   menu. Tests, in order:
   1. **Probe.** `dmesg | grep -i -e tv -e mixer -e tcon -e drm`: sun4i-drm binds with no error or deferral loop;
      `rsos-kmstest --list` shows "Composite-1" (connected, 720x480i and 720x576i, "interlaced") next to
      "HDMI-A-1", and two CRTCs. If Composite-1 is missing, look for the TV encoder or mixer 1 in the errors.
   2. **Menu at boot on the panel** (no HDMI cable): the menu shows on the built-in screen, 4:3, stable, right
      colours; `/data/rsos/logs/frontend.log` has `output init: Composite-1 720x480@59.940 Hz, UI 640x480`.
      Nothing on the panel: try `rsos-kmstest --tv ntsc` then `--tv pal` from the UART (a test pattern), and the
      AMT630A side keys (its OSD, if its firmware shows one, also tells whether it sees a signal).
   3. **NTSC vs PAL.** With `tv_norm = pal` in `/etc/rsos/board.ini` (`mount -o remount,rw /` first) and a
      reboot: PAL 720x576i. Which one does the AMT630A show best (auto-detection, colour, stability)? Keep NTSC
      if both work (lower latency, the cores' 60 Hz); if only PAL works, make `tv_norm = pal` the default here.
      A game in NTSC should run at full speed with `vsync` pacing in game.log (59.94 Hz).
   4. **Switching.** Plug an HDMI TV with the menu up: the menu moves to the TV (and the sound to HDMI), the panel
      goes dark or shows "no signal"; unplug it: back on the panel, sound on the speaker. The same during a game.
      Check the latency lines (`switch latency: ...`) in the log.
   5. **Picture position and overscan.** Does the whole 720x480 picture reach the panel edges, or is part of it
      cropped (a menu border or the FPS counter at a corner cut off)? Then use the AMT630A OSD position/size
      settings, or set `tv_overscan = 3` (percent per edge) in board.ini. Also check the aspect ratio: a circle
      in the UI must be round (else the panel is not 4:3, and `geo_for()` in display.c needs another ratio).
   6. **Game planes.** A 320x240 game (NES/SNES/GBA) fills the height, sharp and 4:3, no interlace flicker; Show
      FPS (Select+X) displays the counter at 2x on the panel (the overlay on mixer 1's primary plane: the log
      must not say `overlay plane refused`).

   If the TV encoder does not probe or shows nothing: the likely suspects are the TVE clock (the /16 post-divider:
   `cat /sys/kernel/debug/clk/tve/clk_rate` should say 13500000), the DAC calibration value at 0x304 (H3:
   0x02000c00, from Armbian; some BSPs read it from the SID) and the mixer 1 reset (RST_WB). The next step would
   then be a register dump of the TVE (0x01e00000) against the Allwinner BSP's. **Panel timings are not needed**:
   the AMT630A drives the panel from its own firmware.
2. **Buttons**: that A/B/X/Y sit where the DTS says (K2 right, K4 bottom, K1 top, K3 left, as on the
   RetroStone2); whether SELECT3 / SELECT4 are fitted, and which one should be C and which Z; `evtest` on
   "RetroStone1 Buttons".
3. **U$15**: that the side switch on PA1 is meant as the power key (and not a menu/hotkey button, in which case
   make it `BTN_MODE` in the pad). `evtest` on "Power Key".
4. **Power-off**: that driving PL8 low after shutdown turns the screen and the CPU off, and the current drawn
   afterwards with the slide switch still on (the DRAM rail on PL9, the LDO and the speaker amplifier stay
   powered). Also that nothing ever drives PL5 high (it would cut VDD-SYS).
5. **DRAM clock**: 624 MHz; run `memtester`, then try the Orange Pi One's 672 MHz.
6. **Analog stick** (units with the stick): axis directions (`abs-range` swap for an inversion), centre and
   dead zone; with no stick fitted, check that the floating inputs do not produce phantom moves (if they do, drop
   the `analog-stick` node for those units).
7. **Audio**: the line out level in `board-hooks.sh` (0-31) so that the wheel covers a useful range, speaker and
   headphones; HDMI audio at 48 kHz.
8. **Charging**: the MCP73871 charges at 1 A (R18-C = 1k); nothing to do in software, but a low-battery warning
   would need a divider on the MCP3208 CH2 input (a hardware change) or a gauge.
9. **Boot time** (never measured on this board) and HDMI hotplug.

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
  class can run. Panfrost cannot be built without LLVM in this Buildroot (`depends on BR2_PACKAGE_MESA3D_LLVM`,
  and its precompiled shaders select OpenCL, hence Clang and libclc); llvmpipe is not enabled.
- **CI**: a cold build took 86 min on 16 cores (39 of them for LLVM and Clang) with 26 GB of output, which does not
  fit the 300-minute build step of a 4-vCPU runner. `scripts/ci/board-info.sh` marks it **heavy**: it runs on the
  `IMAGES_RUNNER_HEAVY` runner when that variable is set, keeps its ccache, and a failure never blocks a release
  (the release then goes out without it; docs/ci.md, "Build times").
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

The RetroStone1 (2026-09-28) got the same checks except the qemu-user run: `check-defconfigs.sh` for both
defconfigs, a full build of `retrostone1_defconfig` (35 cores, 297 MiB target), the kernel and U-Boot `.config`
against the fragments, the device tree compiled by the kernel build without warnings, the image layout, `/boot`,
`board.env` and the inittab UART line. The composite screen (same day, second round): the kernel rebuilt with patch
0002 (both patches apply with `patch -p1` on a clean 6.18.54; `drivers/gpu/drm/sun4i/` and `drivers/clk/sunxi-ng/`
build with `W=1` without warnings; `checkpatch --strict` clean), `DRM_SUN4I`, `DRM_SUN8I_MIXER`, `SUN8I_H3_CCU` and
`SUN8I_DE2_CCU` built in, `sun4i_tv_bind` and `sun8i_h3_mixer1_cfg` in `System.map`, the DTB with `tv-encoder@1e00000`,
`mixer@1200000` and `lcd-controller@1c0d000` enabled and the graph mixer 0 -> TCON0 -> HDMI, mixer 1 -> TCON1 -> TVE
(decompiled and followed), `make CHECK_DTBS=y` with only the errors every board of this tree has (the board's root
compatible, the HDMI `#sound-dai-cells` of the HDMI sound card); the frontend rebuilt, `board.ini` / `board.env`
with `internal_display = composite` and `tv_norm = ntsc`.

Images are not published from these builds: the release CI produces them.

## Follow-ups

- **The RetroStone1 built-in screen**: kernel patch 0002 and the frontend support are in; the hardware test list is
  TODO 1 of its notes. Once it works there, the same dtsi could give the Orange Pi H3 boards their AV output.
- **Hardware reports** for every board here (HDMI modes and hotplug, HDMI audio at 48 kHz, the N64 cores on the
  GPU, WiFi/Bluetooth, the power key, boot time).
- A generic `rsos_board_late_modules` hook in rcS (the Orange Pi 5 loads Panthor from `rsos_board_late_audio`).
- A/B updates on the Raspberry Pi (`autoboot.txt` + `tryboot`, with two boot partitions): rootfs B is reserved.
- An "audio output: HDMI / analog jack" setting for the HDMI boxes whose analog output is in the kernel (Orange Pi
  PC, Orange Pi 5); the frontend's sound follows the display today.
- USB WiFi dongles: no driver is loaded without udev; a `wifi_module` per dongle, or a hotplug helper.
- The Orange Pi Zero 3 / Zero 2W when mainline gains the H616 display pipeline.
- The Pi 4 slot size (512 MiB, about 445 MiB used) once more cores are added (see its notes).
