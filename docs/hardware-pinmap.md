# RetroStone2 hardware pin map

This page covers the Allwinner A20 pin assignments used by
`buildroot-external/board/retrostone2/dts/sun7i-a20-retrostone2.dts` (mainline Linux 6.18).

**Source of truth: schematic v1.15** (`hardware/retrostoneA20-1.15.sch`, Eagle 7.6, read with
`hardware/tools/eagle_nets.py`). Nets and part names below (K1, U$4, R110...) are the schematic's.
The older RetrOrangePi files in `reference/retrostone-rop/` were used only where the schematic says nothing
(timings, polarities, key codes), and every place where they disagree with the schematic is listed in "Conflicts".

Source abbreviations:

| Tag | Meaning |
|---|---|
| **SCH** | schematic v1.15 |
| **PY** | `gpio_retrostone2 - production - with brightness - joystick v7.py` (production button driver) |
| **ODTS** | old RetrOrangePi kernel 5.3 DTS `sun7i-a20-retrostone2.dts` |
| **OKP** | old kernel patch `kernel-sunxi-current.patch` (panel-simple timing hack) |
| **OUB** | old U-Boot patch `u-boot-sunxi-current.patch` (`A20-Retrostone2_defconfig`) |
| **RA** | production RetroArch autoconfig `RetroStone2 Controle.cfg` |
| **BTP** | `btpatch.tar.gz` (RetrOrangePi Bluetooth init, loaded the AP6210 firmware) |

Confidence: **High** = schematic confirms. **Medium** = schematic confirms the wiring, but a detail (polarity,
timing, fitted or not) comes from old software. **Low** = no schematic evidence.

## Buttons and keys

All buttons switch to GND and have an external 10k pull-up to 3.3 V (SCH), so they are active low and the
DTS does not enable internal pull-ups. PH0-PH21 have an external interrupt (EINT0-21) on the A20, so they
are interrupt driven. PH22-PH27 and port C have no EINT, so they are polled (checked in 6.18
`drivers/pinctrl/sunxi/pinctrl-sun4i-a10.c`).

Key codes follow `Documentation/input/gamepad.rst`, which names the face buttons **by position**
(SOUTH/EAST/NORTH/WEST), not by label. The RetroStone2 uses the Nintendo layout: A right, B bottom, X top, Y left.

| Pin | SCH part / net | Button (PY name) | Linux code | Input device | Old PY code | Confidence |
|---|---|---|---|---|---|---|
| PH0 | K2 / P_K2 | A (right) | `BTN_EAST` (0x131) | RetroStone2 Buttons (irq) | `BTN_A` (= BTN_SOUTH) | High (pin), Medium (position) |
| PH11 | K4 / P_K4 | B (bottom) | `BTN_SOUTH` (0x130) | RetroStone2 Buttons (irq) | `BTN_B` (= BTN_EAST) | High (pin), Medium (position) |
| PH12 | K1 / P_K1 | X (top) | `BTN_NORTH` (0x133) | RetroStone2 Buttons (irq) | `BTN_X` | High (pin), Medium (position) |
| PH20 | K3 / P_K3 | Y (left) | `BTN_WEST` (0x134) | RetroStone2 Buttons (irq) | `BTN_Y` | High (pin), Medium (position) |
| PH3 | K5 (pad SELECT3) / P_K5 | C (optional) | `BTN_C` (0x132) | RetroStone2 Buttons (irq) | `BTN_C` | High |
| PH13 | K6 (pad SELECT4) / P_K6 | Z (optional) | `BTN_Z` (0x135) | RetroStone2 Buttons (irq) | `BTN_Z` | High |
| PH14 | START / P_START | Start | `BTN_START` | RetroStone2 Buttons (irq) | same | High |
| PH15 | SELECT / P_SELECT | Select | `BTN_SELECT` | RetroStone2 Buttons (irq) | same | High |
| PH19 | UP / P_AU | D-pad up | `BTN_DPAD_UP` | RetroStone2 Buttons (irq) | same | High |
| PH7 | DOWN / P_AD | D-pad down | `BTN_DPAD_DOWN` | RetroStone2 Buttons (irq) | same | High |
| PH4 | LEFT / P_AL | D-pad left | `BTN_DPAD_LEFT` | RetroStone2 Buttons (irq) | same | High |
| PH16 | RIGHT / P_AR | D-pad right | `BTN_DPAD_RIGHT` | RetroStone2 Buttons (irq) | same | High |
| PH23 | JST U$4 pin 1 / P_L1 | L1 | `BTN_TL` | RetroStone2 Shoulder Buttons (polled, 5 ms) | same | High |
| PH22 | JST U$4 pin 2 / P_R1 | R1 | `BTN_TR` | RetroStone2 Shoulder Buttons (polled, 5 ms) | same | High |
| PH27 | JST U$4 pin 4 / P_L2 | L2 | `BTN_TL2` | RetroStone2 Shoulder Buttons (polled, 5 ms) | same | High |
| PH26 | JST U$4 pin 3 / P_R2 | R2 | `BTN_TR2` | RetroStone2 Shoulder Buttons (polled, 5 ms) | same | High |
| PC18 | tact switch U$32 (also header SV2.3) | Brightness + | `KEY_BRIGHTNESSUP` | RetroStone2 Brightness Keys (polled, 10 ms) | wrote sysfs brightness | High |
| PC22 | tact switch U$31 | Brightness - | `KEY_BRIGHTNESSDOWN` | RetroStone2 Brightness Keys (polled, 10 ms) | wrote sysfs brightness | High |

Why the A/B change: the Python driver emitted `BTN_A` for PH0 and `BTN_B` for PH11. In Linux `BTN_A` **is**
`BTN_SOUTH` (0x130), so the old driver reported the right-hand A button as "south". RetroArch compensated
through its autoconfig (RA maps button index 0 = PH0 to RetroPad A, which is the east/right button, and index 1
= PH11 to RetroPad B, south). Using positional codes makes SDL, libretro and our frontend agree without a
per-device remap: PH0 = `BTN_EAST` = RetroPad A, PH11 = `BTN_SOUTH` = RetroPad B.

Why brightness codes for PC18/PC22: the production firmware used them only for backlight brightness, and the
volume is an analog wheel (see Audio), so there is nothing else for them to control by default. The frontend
can still reuse them as hotkeys. They are a separate input device so the gamepad device reports only gamepad codes.

Three input devices exist because the A20 cannot put PH22-PH27 and port C on interrupts: the frontend should
merge every input device whose name starts with `RetroStone2` into one virtual pad.

### Analog stick (optional, "Pro" part)

| Signal | Connection | DTS | Confidence |
|---|---|---|---|
| X wiper | U$33 via FPC CN1.2 -> AXP209 GPIO0 | `adc-joystick` axis 0, `io-channels = <&axp_adc 5>` (gpio0_v), ABS_X, inverted | High |
| Y wiper | U$33 via FPC CN1.4 -> AXP209 GPIO1 | axis 1, `<&axp_adc 6>` (gpio1_v), ABS_Y | High |
| Pot supply | 1.5 V net (V+, the DRAM rail) | full scale ~1500 mV = raw 3000 (0.5 mV/LSB) | High |

The old kernel exposed these as IIO `in_voltage3`/`in_voltage4` (channel numbers); the 6.18 DT specifier is the
index in `axp20x_adc_channels[]`, which is 5 and 6. The AXP GPIO0/GPIO1 pins are put in ADC mode by a pinctrl
hog on `&axp_gpio`. The stick is polled every 10 ms (the AXP ADC samples at 100 Hz). With no stick fitted the
inputs float at ~2000 mV (raw ~4000, PY `JOYOFFVAL`), outside the 0-3000 range: userspace must treat
out-of-range values as "stick not connected". PY used VREF = 1350 mV and a 200 mV dead zone; the DTS uses the
electrical full scale (0-3000 raw) and `abs-flat = 400` (200 mV).

## Display

| Pin(s) | Function | Net / part | DTS | Source | Confidence |
|---|---|---|---|---|---|
| PD0-PD7 | LCD B0-B7 | LCD_Bx_Dx -> FPC U$7 | `lcd0-rgb888-pins`, tcon0 | SCH | High |
| PD8-PD15 | LCD G0-G7 | LCD_Gx_Dx -> U$7 | same | SCH | High |
| PD16-PD23 | LCD R0-R7 | LCD_Rx_Dx -> U$7 | same | SCH | High |
| PD24 | LCD CLK | via series R24 -> U$7 DCLK | same | SCH | High |
| PD25 | LCD DE | U$7 ENB | same | SCH | High |
| PD26 | LCD HSYNC | U$7 HSYNC | same | SCH | High |
| PD27 | LCD VSYNC | U$7 VSYNC | same | SCH | High |
| PB2 | PWM0 backlight | R22 -> CE of U14 (KA2707 boost), R23 10k pull-down | `pwm-backlight`, 50 us period | SCH, ODTS | High |
| - | Panel VCC | 3.3 V rail directly (no switch: see the note below) | `power-supply = <&reg_vcc3v3>` | SCH | High |
| - | Panel SPI / reset (P_CS, P_SCL, P_SDA, P_RST) | not connected to the SoC | none | SCH | High |
| - | HDMI | on-chip HDMI (HTX*, HHPD with 47k pull-down R47) | `&hdmi`, `hdmi-connector` type a | SCH | High |

**Panel power: worth a GPIO in a future board revision.** The panel's VCC is the always-on 3.3 V rail, so the panel
cannot be powered off while the unit runs, and a TFT must not stay powered with its RGB signals stopped (DC bias on
the liquid crystal and the source drivers; a panel was damaged by an hour of that, 2026-09-28). The software now keeps
TCON0 scanning whenever the system runs (board quirk `panel-keep-scanning`, display-design.md §8.5), but the boot
(power-on to the first modeset) and every panel modeset still drive it briefly without signals. A load switch on the
panel VCC (e.g. a P-MOSFET or a TPS22917-class switch) driven by a free GPIO, described as the panel's
`power-supply` (a `regulator-fixed` with `gpio`/`enable-active-high` and a start-up delay), would let drm_panel power
the panel with its signals (panel-simple `prepare`/`unprepare`), which is the sequence the datasheets ask for.

Panel timing (`panel-dpi`, OKP): 640x480, pclk 33 MHz, hfp 16 / hsync 30 / hbp 114, vfp 10 / vsync 3 / vbp 32,
hsync and vsync active low, DE active high, pixel data driven on the rising edge (`pixelclk-active = <1>`). This
reproduces exactly what the production kernel programmed. Note that 33 MHz / (800 x 525) = **78.6 Hz**, not
60 Hz.

Data format: all 24 data lines are wired, so RGB888. 6.18 `panel-dpi` cannot describe a bus format, so the TCON
gets no format and does not dither, which is right for a 24-bit bus. The kernel prints two harmless
`panel-simple` warnings ("Specify missing bus_format", "Expected bpc in {6,8}"). If they matter, a small
panel-simple entry with `MEDIA_BUS_FMT_RGB888_1X24`, bpc 8 would silence them.

## Power (AXP209, U13, on TWI0 PB0/PB1 at 0x34, IRQ -> NMI)

| AXP output | Rail / use | DTS | Confidence |
|---|---|---|---|
| DCDC2 | VDD_CPU | `vdd-cpu` 1.0-1.4 V, `cpu0` `cpu-supply` (cpufreq/DVFS over the 144-960 MHz OPP table) | High |
| DCDC3 | VDD_INT / DLL / SATA 1.2 V | `vdd-int-dll` 1.0-1.4 V, always on | High |
| LDO1 | VDD_RTC | `vdd-rtc` | High |
| LDO2 | AVCC 3.0 V, and the enable of buck U15 (the 1.5 V DRAM rail) | `avcc` 3.0 V, always on (must never turn off) | High |
| LDO3 | VCC-PE (header SV2/SV6 bank) | `vcc-pe` 2.8 V, not always-on | High |
| LDO4 | VCC-PG (test pads / SV6.13) | `vcc-pg` 2.8 V, not always-on | High |
| GPIO0 / GPIO1 | analog stick ADC | pinctrl hog, function `adc` | High |
| EXTEN | enables buck U17 (**the 3.3 V rail**: SoC I/O, panel VCC, SD card, eMMC, AP6210), the 5 V boost U16 and the speaker amp /SD | not modelled (the 6.18 driver does not touch it; it stays on) | High |
| ACIN | micro-USB U$17 VBUS (charging input) | `ac_power_supply` okay | High |
| VBUS | only a capacitor | `usb_power_supply` left disabled | High |
| BAT / TS | battery U$18 with NTC | `battery_power_supply` okay | High |
| CHGLED | LED1 (blue, charge LED) | hardware only | High |

Power tree (schematic 1.15, checked with `hardware/tools/eagle_nets.py`): the 3.3 V rail (net `VCC`) is buck **U17**
from IPSOUT, enabled by the AXP209 **EXTEN** output (REG12 bit 0); the EXTEN net also enables the 5 V boost U16 and
the PAM8302A /SD. Buck **U15**, enabled by LDO2, makes the **1.5 V DRAM** rail. Neither is software controlled.

**Never touch EXTEN** (no `regulator` node for it, no write to REG12 bit 0, no `i2cset` on it while testing): turning
it off cuts the 3.3 V rail, so the SoC I/O and the SD card (the root file system) lose power at once, together with
the panel, and the unit hangs or corrupts the card. It cannot be used to mute the speaker. The same goes for LDO2
(the DRAM rail).

## Storage

| Pin(s) | Function | Net / part | DTS | Confidence |
|---|---|---|---|---|
| PF0-PF5 | mmc0 (microSD) | SD/MMC socket | `&mmc0` 4-bit | High |
| PH1 | SD card detect | SD0-DET#, 47k pull-up R41 | `cd-gpios` active low | High |
| PC6, PC7, PC8-PC11 | mmc2 CMD, CLK, D0-D3 | eMMC U23 KLMAG2GEND ("Pro" part) | `&mmc2` 4-bit, **disabled** (overlay) | High (wiring), Low (fitted) |
| PC12-PC15 | eMMC D4-D7 | wired to U23 | not usable: the A20 has no SDC2 function on PC12-PC15 | High |
| PC16 | eMMC RST_N | 10k pull-up R121 ("Pro") | `mmc-pwrseq-emmc`, **disabled** (overlay) | High |
| SATA pins | M.2 NGFF M-key J1 ("Pro" part), 3.3 V powered | `&ahci` **disabled** (overlay) | High (wiring), Low (fitted) |

## WiFi / Bluetooth (U$2, AP6210)

The schematic symbol is AP6212 but the value field says AP6210, and RetrOrangePi loaded
`ap6210/bcm20710a1.hcd` successfully (BTP), so the DTS models an AP6210 (BCM43362 WiFi + BCM20710 BT) exactly
like mainline `sun7i-a20-cubietruck.dts`. WiFi, BT and Ethernet stay enabled in the DTS: RetroStoneOS turns
them on and off at runtime (driver modules, and unbinding the mmc3 host, which makes `mmc-pwrseq-simple` drive
WL_REG_ON low).

| Pin(s) | Function | Net | DTS | Confidence |
|---|---|---|---|---|
| PI4-PI9 | mmc3 SDIO CMD/CLK/D0-D3 | SD1-* | `&mmc3` 4-bit, non-removable | High |
| PH9 | WL_REG_ON | WIFI-SHDN (pull-up R96 not fitted) | `mmc-pwrseq-simple` reset, active low | High |
| PH10 | WL_HOST_WAKE (EINT10) | WIFI-HOST-WAKE | `brcmf` `host-wake` irq, level low | High |
| PI12 | CLK_OUT_A, 32.768 kHz LPO | CLK-32K (2k pull-up R34) | pinctrl hog on `&pio`; pwrseq sets osc32k parent and 32768 Hz | High |
| PI16 / PI17 | UART2 RTS / CTS | BT-RTS / BT-CTS | `uart2_cts_rts_pi_pins`, `uart-has-rtscts` | High |
| PI18 / PI19 | UART2 TX / RX | BT-TXD / BT-RXD | `uart2_pi_pins` | High |
| PH18 | BT_RST_N | BT-RESET | `shutdown-gpios` active high | High |
| PH24 | BT_WAKE | BT-WAKE | `device-wakeup-gpios` active low (as Cubietruck) | Medium |
| PH25 | BT_HOST_WAKE | BT-HOST-WAKE | `host-wakeup-gpios` active low; no EINT on PH25, so no wake irq | Medium |
| PB6, PB7, PB8, PB12 | BT PCM | BT-PCM-* (also SV6) | not used | High |

Firmware: `brcm/brcmfmac43362-sdio.bin` plus NVRAM (brcmfmac looks for
`brcmfmac43362-sdio.8bcraft,retrostone2.txt` first, then `brcmfmac43362-sdio.txt`). For BT, btbcm picks the
patch name from the chip ID (expected `brcm/BCM20710A1.hcd`, which is the AP6210 `bcm20710a1.hcd` from BTP).
Units with an AP6212 would need `brcmfmac43430-sdio` and the BT compatible `brcm,bcm43430a1-bt`.

## Ethernet (U10, LAN8710A)

| Pin(s) | Function | DTS | Confidence |
|---|---|---|---|
| PA0-PA17 | GMAC MII | `gmac_mii_pins`, `phy-mode = "mii"` | High |
| straps | PHYAD0 (RXER) pull-up R12; PHYAD1 (RXCLK) R15 and PHYAD2 (RXD3) R16 pull-down -> **address 1**; RMIISEL (RXD2) R17 pull-down -> MII; MODE0-2 pulled up (all capable) | `ethernet-phy@1` | High |
| PH6 | optional PHY reset through R54, **not fitted** (PHY NRST is on the system reset) | not used | High |

## USB

| Port | Controller | Connector | VBUS | DTS | Confidence |
|---|---|---|---|---|---|
| USB0 (DM0/DP0) | MUSB | stacked USB-A USB2, upper level | always on (+5V_USB from U4 SY6280, EN tied to +5V) | `&usb_otg` `dr_mode = "host"` | High |
| USB1 (DM1/DP1) | EHCI0/OHCI0 | stacked USB-A USB2, lower level | always on | `&ehci0`, `&ohci0` | High |
| USB2 (DM2/DP2) | EHCI1/OHCI1 | USB-A U$1 | always on | `&ehci1`, `&ohci1` | High |

No VBUS GPIOs, no ID or VBUS detect pins. PC17 (net PC17/USB0-DRV) goes to AXP N_VBUSEN through R65, which is
not fitted: the DTS never drives PC17. `sunxi-common-regulators.dtsi` is **not** included (see Conflicts).

## Audio

| Signal | Connection | DTS | Confidence |
|---|---|---|---|
| HPL / HPR | A20 codec -> analog volume wheel U$27 -> headphone jack U$24 | `&codec` okay | High |
| Speaker | jack switch contacts -> PAM8302A U1-A; /SD = AXP209 EXTEN | no `allwinner,pa-gpios` (no GPIO exists) | High |
| MIC | jack mic contact -> MICIN1 | not routed in software yet | Medium |

Volume and speaker/headphone switching are fully analog. Software should keep the codec headphone volume
fixed near maximum and leave volume to the wheel. The speaker amp cannot be muted or powered down by software.

## Misc

| Pin | Function | DTS | Confidence |
|---|---|---|---|
| PB22 / PB23 | UART0 TX / RX, header "UART0" (RX through diode D2) | `&uart0`, console 115200 8N1 | High |
| PB18 / PB19 | TWI1 -> AT24C16 EEPROM U5 at 0x50 | `&i2c1`, `eeprom@50` | High |
| PH2 | LED3 (blue) through R110 10k, cathode to GND: active high | `gpio-leds` `blue:status`, default off | High |
| - | LED2 (blue) power LED on 3.3 V | hardware only | High |
| - | Touch panel XP/XN/YP/YN -> FPC CN2 ("Pro" part) | not enabled (`&rtp` is only the thermal sensor) | High |
| LRADC0 / LRADC1 | pads with pull-ups to AVCC | not used | High |

Unconnected or header-only pins (free for hacking, not in the DTS): PE0-PE11 (SV2/SV6, VCC-PE = LDO3),
PI0-PI3, PB3-PB5, PB13-PB17, PG11 (SV6); test pads PH5, PH8, PH17, PH21, PC0-PC5, PC19-PC21, PC23, PC24,
PG0-PG10, PI10, PI11, PI13, PI20; no-connects PB9-PB11, PB20, PB21, PI14, PI15, PI21.

## Conflicts between the old files and the schematic

| Item | Old claim | Schematic v1.15 | Resolution in the DTS |
|---|---|---|---|
| PH7 | OUB: backlight enable | D-pad DOWN | button, never driven |
| PH8 | ODTS: backlight enable; OUB: LCD power | unconnected test pad | not used; backlight has no enable GPIO |
| PH12 | ODTS: panel power regulator (10.4 V!) | button K1 (X) | button, never driven |
| PH3 | `sunxi-common-regulators.dtsi` usb2-vbus (ODTS enabled it) | button K5 (C) | button; that dtsi is not included |
| PH6 | `sunxi-common-regulators.dtsi` usb1-vbus | PH6/EPHY-RST# via R54, not fitted | not used |
| PB8 | `sunxi-common-regulators.dtsi` ahci-5v | BT-PCM-OUT | not used |
| PB9 | `sunxi-common-regulators.dtsi` usb0-vbus | no connect | not used; VBUS is always on |
| PC3 | ODTS/OUB: SATA power enable | unconnected pad | `&ahci` has no target-supply |
| PH4 / PH5 | Olimex usb0 ID / VBUS detect (U-Boot DTS) | PH4 = D-pad LEFT, PH5 = test pad | not used as detect pins |
| PH17 | ODTS: WiFi host-wake ("PH17 / EINT10") | unconnected pad; host-wake is PH10 | PH10 (EINT10); the old OOB irq never worked |
| PH9, PH24 | `RS2GPIO.tar.gz` `tz_gpio_controller.py`: D-pad up, R | WL_REG_ON, BT_WAKE | generic RetrOrangePi GPIO driver, not the RetroStone2 map: ignored |
| PC18 / PC22 | `e.py` (factory test): buttons C / Z | brightness tact switches U$32 / U$31 | brightness keys (PY production map) |
| LCD depth | OUB: 18-bit (RGB666), dithering | all 24 lines wired | RGB888 (as ODTS) |
| LCD sync | OUB: hsync/vsync forced active high, `hs:1 vs:1`, `le:45 ri:209 up:22 lo:22` (70 Hz) | - | OKP timings, active low (production kernel) |
| LCD DE | stock `lemaker,bl035-rgb-002`: DE active low | - | active high (OKP removed DE_LOW) |
| LCD clock phase | OUB: `DCLK_PHASE=1` | - | not expressible in mainline; kernel resets the TCON; drive on rising edge as the production kernel |
| eMMC | ODTS: 4-bit, `broken-hpi`, enabled | U23 8-bit wiring, "Pro" part | 4-bit (A20 limit), disabled, overlay |
| SATA | ODTS: enabled with PC3 supply | J1 M.2 "Pro" part, no enable GPIO | disabled, overlay, no supply |
| LED | ODTS: Olimex "a20-olinuxino-lime:green:usr" on PH2 | blue LED3 on PH2, active high | `blue:status` |
| EEPROM | ODTS: AT24C16 on I2C1 (assumed Olimex leftover) | U5 AT24C16 on TWI1 exists | kept |
| WiFi/BT module | SCH symbol AP6212 | SCH value AP6210; BTP used AP6210 firmware | AP6210 (Cubietruck-style nodes) |
| uart2 | ODTS: declared twice | - | one node |
| cpu-supply | ODTS: missing (no DVFS) | DCDC2 = VDD_CPU | `cpu-supply = <&reg_dcdc2>` |

## Optional hardware: device tree overlays

The "Pro" parts are described in the base DTS with `status = "disabled"` and enabled by overlays in
`buildroot-external/board/retrostone2/dts/overlays/`:

| Overlay | Enables | Notes |
|---|---|---|
| `retrostone2-emmc.dtso` | `&mmc2_pwrseq` and `&mmc2` | eMMC U23 on mmc2, 4-bit, reset PC16. The A20 cannot do 8-bit on mmc2. |
| `retrostone2-sata.dtso` | `&ahci` | M.2 SATA slot J1, no supply GPIO |

They use `/dts-v1/; /plugin/;` with `&label` references, so the base DTB must be built with symbols
(`dtc -@`; in Buildroot `BR2_LINUX_KERNEL_DTB_OVERLAY_SUPPORT=y`). Compile with
`dtc -@ -I dts -O dtb -o retrostone2-emmc.dtbo retrostone2-emmc.dtso`. For the fastest boot, merge them at image
build time (`fdtoverlay -i sun7i-a20-retrostone2.dtb -o merged.dtb retrostone2-emmc.dtbo`) instead of applying
them from U-Boot at every boot. Both were test-applied with `fdtoverlay` on the 6.18 base DTB, alone and together.

## To verify against the hardware

- [ ] Physical A/B/X/Y positions on the case (Nintendo layout assumed: K2 = A right, K4 = B bottom, K1 = X top, K3 = Y left).
- [ ] Which units have the optional C/Z buttons (K5/K6 pads), the analog stick (CN1), eMMC (U23), M.2 SATA (J1) and touch (CN2) fitted.
- [ ] LCD panel model and datasheet: timings, and whether 25.2 MHz (60 Hz) works instead of 33 MHz (78.6 Hz). 60 Hz is much better for emulator vsync.
- [ ] LCD hsync/vsync/DE polarity and pixel clock edge on the real panel (the DTS copies the production kernel).
- [ ] Backlight PWM frequency (50 us / 20 kHz from production) against the KA2707 datasheet; check for audible whine or flicker.
- [ ] Analog stick: center, range and inversion with the 1.5 V pot supply; the "not connected" reading (~2000 mV).
- [ ] WiFi/BT module actually fitted (AP6210 vs AP6212), BT_WAKE/BT_HOST_WAKE polarity, and that the WiFi OOB interrupt on PH10 works.
- [ ] 32.768 kHz on PI12 (CLK_OUT_A) once the pwrseq is probed.
- [ ] eMMC overlay on a unit with U23 (4-bit mode, reset on PC16).
- [ ] SATA overlay with an M.2 SATA SSD in J1.
- [ ] Ethernet PHY found at MDIO address 1.
- [ ] The three USB-A ports all get VBUS at boot with no GPIO action, and USB0 works in host mode without ID/VBUS detect.
- [ ] Speaker amp and headphone switching with the codec headphone volume near maximum.
- [ ] PH2 LED is visible from outside the case (otherwise it is only a debug LED).
