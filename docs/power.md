# Power management

Battery status and warnings, the critical-battery emergency save, the power key (any press = clean power-off),
idle dimming, the CPU governor policy, the thermal watch, charge mode and the wall clock for the RetroStone2 (A20,
AXP209, 4000 mAh Li-ion, Linux 6.18). It covers requirements §5 (Power) and §7 (Time) and risks #9, #10 and #11.
There is **no sleep mode** any more (§6): the owner found it confusing, and the boot is fast enough.

| Path | Contents |
|---|---|
| `frontend/src/power/power.{h,c}` | the module the frontend links: API, state machine, sysfs, power key |
| `frontend/src/power/battery.{h,c}` | pure battery logic: OCV curve, smoothing, levels, critical detector |
| `frontend/src/power/pclock.{h,c}` | saved clock, RTC, time zone table |
| `frontend/src/power/bootreason.{h,c}` | AXP209 power-on source (REG00 over i2c-dev) |
| `frontend/src/power/psys.{h,c}` | sysfs helpers, durable atomic writes |
| `frontend/src/power/rsos-bootreason.c`, `rsos-clock.c` | helpers for the init scripts |
| `frontend/src/power/tests/power_test.c` | unit tests (fake sysfs trees, simulated discharges) |
| `frontend/power.mk` | make fragment (`power-all`, `power-check`, `power-arm-check`, `power-install`) |

The module runs only in the frontend's **supervisor (menu) process**, never in the game child. It is single-threaded
and non-blocking, and it adds nothing to the boot path except one sysfs scan in `power_init()` (about 1 ms, plus
the first battery sample, a few I2C reads).

---

## 1. What 6.18 gives us (checked in the sources)

| Item | 6.18 fact | Where |
|---|---|---|
| Battery supply | `/sys/class/power_supply/axp20x-battery`: `present`, `status` (Charging / Discharging / Full / Not charging), `voltage_now` (µV, the **loaded** voltage), `current_now` (µA, **negative when discharging**), `capacity` (REG B9 fuel gauge, raw %), `health`, `constant_charge_current[_max]`, `voltage_min` / `voltage_max`. `voltage_min`, `voltage_max`, `constant_charge_current[_max]` and `status` are writable | `drivers/power/supply/axp20x_battery.c` (`axp209_batt_ps_desc`) |
| Charger supplies | `axp20x-ac` (ACIN = the micro-USB, `online`, `present`), `axp20x-usb` (AXP VBUS pin, **disabled in our DTS**: it only has a capacitor) | `axp20x_ac_power.c`, `axp20x_usb_power.c` |
| monitored-battery | AXP209 applies **only** `voltage-min-design-microvolt` (→ REG31 V_OFF, 2.6-3.3 V in 0.1 V steps) and `constant-charge-current-max-microamp` (→ REG33, 300-1800 mA in 100 mA steps). Without it the charge current stays at the register value and a bad value falls back to 300 mA. `voltage-max-design` is applied only on the AXP717 | `axp209_set_battery_info()` |
| APS low-voltage warnings | REG3A/3B (`AXP20X_APS_WARN_L1/L2`) exist in the header, but no driver programs them or consumes their IRQs: **not usable** from userspace. V_OFF is the only hardware threshold we can set | `include/linux/mfd/axp20x.h` |
| Power key | `axp20x-pek` input device, `KEY_POWER` (press/release from the PEK_DBF/DBR IRQs). sysfs `startup` (128/1000/2000/3000 ms, REG36[7:6], hold time to power on) and `shutdown` (4000/6000/8000/10000 ms, REG36[1:0], hardware force-off). The long-press IRQ time REG36[5:4] is **not** exposed | `drivers/input/misc/axp20x-pek.c` |
| Power-on source | REG00 bit 0 ("boot source is ACIN/VBUS") is **not** exposed by any driver. Read with an `I2C_RDWR` transfer on `/dev/i2c-N` (works while `axp20x-i2c` owns 0x34; the adapter lock serialises it; REG00 is read-only). `CONFIG_I2C_CHARDEV=y` in sunxi_defconfig and in the built `.config` | `rsos-bootreason` |
| Power-off | `axp20x` registers the pm_power_off handler, so `poweroff` cuts power (requirements §5) | `drivers/mfd/axp20x.c` |
| Suspend to RAM | **Not available.** Mainline U-Boot v2026.07 sunxi PSCI implements only `CPU_ON`/`CPU_OFF` (`arch/arm/cpu/armv7/sunxi/psci.c`: `psci_cpu_on`, `psci_cpu_off`, no `SYSTEM_SUSPEND`), and the kernel has no sun7i suspend code (`arch/arm/mach-sunxi/`). `CONFIG_SUSPEND=y` so `/sys/power/state` should list only `freeze` (s2idle): it would suspend DRM/MMC/USB/brcmfmac drivers whose A20 resume paths are untested, for little gain (DRAM and the CPU rails stay up). Hence **fake sleep** (§5) | U-Boot and kernel sources, [linux-sunxi](https://linux-sunxi.org/Suspend-to-RAM) |
| Thermal | `sun4i-ts` (the "rtp" block) registers the `cpu-thermal` zone: passive trip 75 °C (cpufreq cooling on cpu0/cpu1), critical 100 °C (orderly power-off, **no save**). `temp` returns `-EAGAIN` until the first conversion | `sun7i-a20.dtsi`, `drivers/input/touchscreen/sun4i-ts.c` |
| cpufreq | one policy for both cores (`policy0`), OPPs 144/312/528/720/864/912/960 MHz. The built kernel has schedutil and performance but **no powersave governor** (the module then clamps `scaling_max_freq` to the minimum instead) | `sun7i-a20.dtsi`, `~/rsos/output/build/linux-6.18.54/.config` |
| RTC | `rtc-sunxi` for sun7i: years 1970-2225, `RTC_HCTOSYS=y`, `RTC_INTF_DEV=y` (`/dev/rtc0`). VDD_RTC = AXP LDO1; the AXP BACKUP pin goes only to a test pad, so the time is lost when the battery is removed or fully drained | `drivers/rtc/rtc-sunxi.c`, schematic |

---

## 2. Battery status

`power_get_status()` returns a `struct power_status`: `percent` (what the icon shows), `charger_online`, `state`
(charging / discharging / full / not charging), `voltage_mv`, `current_ma` (averaged, + charging), `minutes_left`,
`level`, `temp_mc`, `mode`, `screen`, `boot_reason`, `clock_restored`. `on_status` fires when anything the icon
shows changes.

**Sampling**: every 10 s; every 2 s when the filtered voltage is under 3.65 V (or in charge mode); every 30 s while
asleep (still 2 s if low). A `power_supply` uevent (charger plugged or unplugged, status change) triggers an immediate
sample. One sample = 5-6 sysfs reads, each a few AXP register reads over I2C (under 2 ms).

**Percentage** (`battery_gauge` setting):
- `voltage`: the open-circuit voltage is estimated as `median(last 5 loaded samples) - I_avg × R_int`
  (R_int = 150 mΩ: cell, protection and wiring), then mapped through a typical Li-ion OCV curve
  (3.45 V = 3 %, 3.72 V = 25 %, 3.84 V = 55 %, 4.02 V = 82 %, 4.20 V = 100 %).
- `axp`: the AXP209 fuel gauge (REG B9) as is.
- `auto` (default): the AXP value while it agrees with the voltage estimate (distrusted at once if they differ by more
  than 30 points, trusted again after 10 samples within 15 points), otherwise the voltage estimate. A gauge stuck at
  100 % or 0 % (uncalibrated coulomb counter) therefore never shows.

**Smoothing**: an exponential average with a 60 s time constant. For 3 minutes after boot or a charger plug/unplug the
display follows the estimate freely (boot load, surface charge); after that it only moves **down** while discharging
and **up** while charging. `Full` shows 100 %.

TODO(hw): log a full discharge (voltage, current, AXP %) at a typical game load and fit the OCV table and R_int; check
whether the AXP gauge is usable (then `auto` will mostly show it).

## 3. Warnings

| Level | When | UI (on_warning) |
|---|---|---|
| `POWER_LEVEL_LOW` | shown ≤ 15 % | a toast, once |
| `POWER_LEVEL_VERY_LOW` | shown ≤ 7 % | a persistent warning (icon blinks / banner), the callback repeats every 5 min |
| `POWER_LEVEL_OK` | charger online, or back above threshold + 3 % | clear the warning |
| `POWER_LEVEL_CRITICAL` | see §4 | `on_critical`, then the shutdown path |

Levels get worse immediately and only get better past the threshold plus 3 % (hysteresis), so a value hovering around
15 % gives exactly one toast (unit-tested).

## 4. Critical battery: emergency save and clean power-off

The AXP209 cuts the power without warning when the system voltage falls below V_OFF. The frontend must save before that.

**Rule** (on the **loaded** voltage, because that is what the AXP compares, never on the percentage):
- median of the last 5 samples < **3.45 V** for **4 samples spanning at least 6 s** (the countdown resets only above
  3.50 V: 50 mV hysteresis), or
- emergency: median **and** the last 2 raw samples < **3.30 V** (a collapsing cell).
- Disabled while a charger is online (which also re-arms it).

A single noisy sample, or two in a row, never triggers it (the median needs 3 of 5); tested with ±30 mV noise, -250 mV
spikes and double deep spikes. In the simulated 500 mA discharge the trigger comes within 30 s of the true loaded
voltage crossing 3.45 V, and never above 3.51 V. At 3.45 V under load the cell has about 3-5 % left, i.e. minutes of
margin before the 3.0 V cut.

**Sequence**: `on_critical()` (show "Battery empty, saving...") → `on_shutdown_request(POWER_REASON_CRITICAL)` → the
frontend saves (§10) and calls `power_poweroff()` → lastclock, `sync()`, BusyBox init powers off (rcK unmounts
`/data`). If `power_poweroff()` has not been called 10 s after the request (15 s for the other reasons), the module
calls it itself; if init has not powered off 20 s later, it calls `reboot(RB_POWER_OFF)` directly.

**Last line of defence (hardware)**: `voltage-min-design-microvolt = <3000000>` in the DTS sets REG31 V_OFF to 3.0 V
(reset default 2.9 V) at probe; `power_init()` re-applies it through the writable `voltage_min` if it differs. The APS
warning thresholds (REG3A/3B) are not reachable (§1).

## 5. Power key

The module opens the `axp20x-pek` input device itself (and the built-in `RetroStone2*` button devices, EV_KEY only, no
grab), so the key works whichever process draws the screen. **The UI and the game child must ignore
`IN_HK_POWER_SHORT` / `IN_HK_POWER_OFF`** (see §10 for the child's long-press flush, which stays as a harmless extra).

| Press | Menu or game | Charge mode |
|---|---|---|
| short (released < 2 s) | clean shutdown with save flush (`POWER_REASON_USER`), same path as §4 | boot normally (leave charge mode) |
| long (held 2 s) | the same shutdown, fired at 2 s without waiting for the release | power off |
| 6 s | AXP hardware force-off (last resort) | |

- **In the menu**: "Powering off..." is drawn at the next frame (right away), settings and game data are saved, and
  init is signalled at once (`SHUTDOWN_MSG_MS` is 0 since 2026-09-27: the fixed 300 ms hold is gone; the frontend
  only waits for that frame's page flip, at most 100 ms).
- **In a game**: the frontend sends `RSOS_SIG_POWEROFF`; the child draws one "Powering off..." frame (an OSD toast
  over the last game frame), flushes SRAM, writes `.state.auto` (both fsync'ed), unloads the core (cores write their
  own files there), lets the save worker finish and `_exit`s 3 without the rest of its teardown (no audio drain, no
  GL/display release: `power-off: saves on the card, exiting without the teardown` in game.log); the frontend records
  the game in `/data/rsos/resume.ini` (durable write) and does **not** take the display back (no modeset and panel
  power-up just to exit: `game ended for a power-off: the display is not taken back`); then the unit powers off.
  The next boot offers "Resume <game>?" once the menu is up (host-design.md §7.1), and launching that game offers
  "Resume where you left off?".
- **The last steps** (`finish_shutdown()`, 2026-09-27, from the shutdown timing on hardware: 0.4-0.8 s from the
  request to rcK): the UI state and a running USB copy are flushed, `/run/rsos/shutdown-signal` gets the uptime,
  init is signalled (SIGUSR2 / SIGTERM) and the frontend `_exit`s right away, so rcK finds no frontend to stop. No
  `sync()` and no clock save there any more: rcK saves the clock (`rsos-clock save`), syncs and unmounts `/data`;
  `power_poweroff()` still saves the clock and syncs itself when it has to call `reboot(2)` because init could not
  be signalled, and when a `do_poweroff` hook replaces init (tests, headless). (The `poweroff_fallback_ms` watchdog
  only runs in a process that stays after signalling init, i.e. not the frontend any more.) `shutdown-start` → `shutdown-signal` → rcK's `rcK start` in `shutdown.txt` shows how long BusyBox init
  takes to start rcK.
- **Screen off** (menu idle, §7): the first press only turns the screen on (the user cannot see what state the unit
  is in). A **dimmed** menu is visible, so there the press powers off.
- **Debounce**: one request per shutdown (the module is then `SHUTTING_DOWN` and ignores the key; unit-tested with a
  second press). Presses in the first **2 s after `power_init()`** are ignored (`pek_start_ignore_ms`), and for
  **1.5 s after leaving charge mode** (`pek_quiet_ms`), so the press that started the unit or left the charge
  screen cannot also power it off. An ignored press also makes its release ignored.
- **How axp20x-pek reports the power-on press**: the AXP needs the key held for `startup` (1000 ms on the unit)
  before it powers the board, i.e. before U-Boot and Linux run. axp20x-pek reports only debounced edges (falling =
  press, rising = release, `axp20x_pek_irq()`), from its probe on, and the AXP MFD's regmap-irq chip acks the
  latched PEK edges at init (`init_ack_masked`), so the power-on press is never delivered. If the key is still held
  at the probe, only its release arrives, and a release without a recorded press is ignored. The 2 s guard covers
  a key held until the menu appears. Evdev events from before the frontend opened the device are not queued.

`power_init()` raises the AXP `shutdown` (force-off) time to 6000 ms if it is shorter, so the 2 s software long press
always gets to save first; `startup` (hold time to power on, REG36[7:6]) is only logged (the unit logs "PEK
force-off 6000 ms, power-on hold 1000 ms").

## 6. Fake sleep (removed from every user-facing path)

**Not reachable any more**: the power key powers off (§5) and Settings > Power no longer has "Sleep", "Power off
when asleep for" or "Wake up with". The owner's report ("it wakes and then nothing works") came from the wake-key
swallow bug described in §7, which also hit the idle screen-off; it is fixed, but a sleep mode is not wanted: the
boot is fast enough. The code below stays in the module (`power_sleep()`, the child's `RSOS_SIG_SLEEP/WAKE`
handling) for tools and tests; the keys `sleep_timeout_min` and `sleep_wake` are still accepted but nothing shows
them. What it did, for reference:

A20 mainline has no suspend-to-RAM (§1). On `power_sleep(true)`:

| Step | Done by |
|---|---|
| backlight off (`bl_power` = 4, brightness saved) | power module |
| CPU to the lowest OPP: `powersave` governor, or schedutil with `scaling_max_freq` = 144 MHz when powersave is not built | power module |
| blue status LED (PH2) on, as an "asleep, not off" indicator | power module (`sleep_led`) |
| cpu1 offline (U-Boot PSCI has `CPU_OFF`) | power module, **off by default** (`sleep_cpu1_offline`, TODO(hw)) |
| pause the game, mute audio, display off (black frame, CRTC still scanning on the RetroStone2: display-design.md §8.5; CRTC inactive on HDMI and other boards) | `on_sleep_request(true)`: menu: display layer; game: signal to the child (§10) |

**Wake**: any built-in button (setting `sleep_wake=any`, default) or the power key (`sleep_wake=power`: power key
only, better in a bag). Everything is restored, then `on_sleep_request(false)`. The waking key is swallowed: the
module drops UI input until every built-in key is released (+100 ms), and the child drains its input on wake.

**Timeout**: after `sleep_timeout_min` (default **15**, 0 = never) asleep: `on_shutdown_request(POWER_REASON_SLEEP_TIMEOUT)`
→ SRAM flush + auto save state → power off (MinUI/Onion behaviour). Critical battery detection keeps running while asleep.

WiFi/BT are not touched: `rsos-net` persists its state, so a sleep toggle would change the user's setting. Request to
the rsos-net owner: a non-persistent `rsos-net suspend|resume` (power down the chips, then restore the saved state).

**Current draw (estimates, battery side at 3.7 V; nothing measured yet)**:

| State | Estimate | 4000 mAh lasts |
|---|---|---|
| Menu, LCD at 50 % | 350-500 mA | 8-11 h |
| Game, PS1 at 960 MHz | 550-750 mA | 5-7 h |
| Fake sleep (backlight and CRTC off, 144 MHz) | 120-200 mA | 20-33 h (15 min asleep ≈ 40 mAh, 1 %) |
| Off (AXP off, RTC LDO only) | < 0.1 mA | months |

TODO(hw): measure each state with `current_now` (charger unplugged) and a USB meter; if fake sleep is above ~150 mA, try
cpu1 offline and the DRAM clock.

## 7. Idle dimming (menus only)

No input for `idle_dim_min` (default 2 min): the backlight goes to 30 % of the user's level. After `idle_off_min`
(default 5 min): backlight off (`bl_power` = 4) and `on_screen(POWER_SCREEN_OFF)` (the frontend turns the display off,
`display_set_active(false)`: on the RetroStone2 a black frame with the panel still scanning, never CRTC off: a powered
panel must keep its signals, display-design.md §8.5). Any key restores it and is swallowed. Never while a game runs
(`power_set_game_running(true)`); the timers restart when the game exits. Docked on HDMI (`power_set_docked(true)`)
the module never touches the LCD backlight; it still reports `on_screen`.

**Wake-key swallow.** The key that wakes the screen must not also act in the menu: its press, repeats and release are
dropped (`power_on_input()` returns true), and the next press passes. The module counts the built-in keys down from its
own evdev fds (`keys_down`). **Bug fixed (2026-09-26, seen on the device after a sleep/wake, same path for the idle
dim/off and charge-mode exit):** the latch was only cleared from `power_on_input()`, when no key was down and the last
release was 100 ms old. But the UI calls `power_on_input()` only for input events, and every built-in event reaches
the module first (its evdev read runs just before the UI's), so at that moment a key was always down (a press, a
repeat) or had just been released: the latch never cleared, and every built-in button was dropped until a reboot.
The display came back fine (the SD-card log shows `screen on ... in 16 ms`, no flip warnings, the power key still
worked). Now a new press clears the latch when it arrives, if every built-in key had been released for 100 ms; after
an evdev overflow (`SYN_DROPPED`) `keys_down` is recounted with `EVIOCGKEY`, so it can never stay stuck. Regression
tests: `power_test` (idle off, dim, fake sleep: press/release as the evdev read sees them, then the next presses must
pass) and `make check-frontend` step 1b (the real main loop: idle screen-off, wake, the next button opens Settings).

`idle_dim_s` / `idle_off_s` (seconds) are accepted by `power_set_setting()` for development and tests (the headless
check uses `idleoff:2`); the UI never writes them.

### 7.1 Idle power-off (2026-09-28)

Settings > Power > **Power off after**: Never / 5 / 10 / 15 / 30 / 60 min (`idle_poweroff_min`, default **5**, the
same as "Screen off after"). No input for that long and the unit powers off by itself, through the normal clean
shutdown (the same path as the power key: saves, then rcK and `poweroff -f`).

- **Order**: dim (`idle_dim_min`) -> screen off (`idle_off_min`: backlight off, the panel still scanning,
  display-design.md §8.5) -> power off. A stage at or after the power-off never happens: **equal timers** (the
  defaults, 5 and 5 min) power off without a screen-off stage, and a power-off **shorter** than the screen-off wins.
- **The notice**: `idle_warn_s` (10 s) before, `on_idle_poweroff(POWER_IDLE_WARN)`: the screen is lit again if it was
  dimmed or off, and "Powering off in 10 s — press any button to cancel" shows (a 10 s warning toast in the menu; in a
  game the menu sends `RSOS_SIG_IDLE_WARN` and the game process shows it on its OSD). **Any input cancels it** (a
  button on any pad, a built-in key, the power key: its release then does not power off) and restarts the countdown;
  the cancelling press is swallowed. `POWER_IDLE_CANCEL` takes the notice down (`RSOS_SIG_IDLE_CANCEL` in a game).
- **In a game**: the dim and screen-off stages still never happen in a game, but the power-off counts. The menu
  process (blocked in `host_launch()`, it reads the same pads, no grab) reports the player: a button held or changed on
  any pad, or a real stick move (`power_axis_activity()`: an axis past half deflection, or moved by more than 20 %
  of its range since the last move counted; the jitter of a drifting stick never counts, so it cannot keep the unit
  on), at most once a second (`pads_activity()` -> `power_notify_activity()`); the same stick check runs in the
  menu. N64/PS1 players steering with the stick keep the unit on. The built-in keys reach the module directly.
  The power-off sends `RSOS_SIG_POWEROFF`: the game writes SRAM and `.state.auto`, the menu records
  `/data/rsos/resume.ini` (reason `idle`), so the next boot offers "Resume" (or resumes, per `resume_mode`), then
  powers off. **Never without saving**: if the game did not write its resume state (no save-state support, a write
  error, no exit within 8 s), the idle power-off is cancelled (`power_cancel_shutdown()`): the menu comes back with
  "Automatic power-off cancelled: the game could not be saved.", the unit stays on and the countdown starts over. The
  module's own 15 s shutdown watchdog never forces an idle power-off either: it cancels it. A game whose core cannot
  save a state says so at its start (status line `nostate`, host-design.md): the menu then holds the idle power-off
  for that whole game (below), so it never quits it unsaved (review, batch B2).
- **A game that ends or starts during the notice** cancels it (`power_set_game_running()` -> `POWER_IDLE_CANCEL`,
  review B2): before, the notice state stuck, the menu's dim and screen-off stages were skipped and the unit powered
  off later without a notice.
- **Never while busy** (`power_set_busy()`, checked by the menu once a second and whenever a power deadline is due): a
  USB import, a USB export or saves backup while it **copies** (`TRANSFER_RUNNING`; not while it waits for the player:
  a duplicate question, or a finished copy whose summary is not read yet, review B2), the web share with a client
  connected or an upload running, the SMB share with a client (an established TCP connection on port 445 in
  `/proc/net/tcp{,6}`: ksmbd has no client count), an OS update download or install (`ui_update_busy()`), the storage
  being formatted (`ui_data_problem_busy()`), the game list still loading, and in a game: a core without save states
  (`nostate`) or a benchmark (`busy bench` ... `busy off`; the child's exit also ends both). A job that starts cancels
  a pending notice; when the last one ends, the countdown **starts over** from that moment. The dim and screen-off
  stages go on meanwhile (in the menu). During a benchmark, the `autostate` of a power-off (the start state copied
  to the game's `.state.auto`) records `resume.ini` as it arrives: the driver may not exit within the grace time; a
  game killed after the grace time that had sent `autostate` is recorded too.
- **Never in charge mode** (the charge screen has its own rules, §9), nor while asleep or already shutting down.
- `idle_poweroff_s` (seconds) for development and tests (`idlepoweroff:N` in the headless scripts, in the menu and
  as `game:idlepoweroff:N`).

## 8. CPU governor and thermal

| State | Governor |
|---|---|
| Menu | `schedutil` |
| Game | `performance`, or the core's profile: `power_set_game_cpu("schedutil", 720000)` for NES/GB/SMS (requirements §5) |
| Asleep, charge mode | `powersave` (fallback: `scaling_max_freq` = min) |
| Shutting down | `performance` (save as fast as possible) |

The module is the **only** writer of the governor (request to the host owner in §10). Thermal: `cpu-thermal` zone read
at every sample. `on_thermal(temp, true)` at ≥ 75 °C (the kernel is throttling: toast "Hot, slowing down"),
`false` below 70 °C. Three samples ≥ 95 °C → `POWER_REASON_THERMAL` shutdown with save (the kernel's own 100 °C trip
powers off without saving). TODO(hw): temperature after 30 min of PS1 in the closed case.

## 9. Charge mode

The AXP209 powers the board on when ACIN (the micro-USB) is plugged in. `rsos-bootreason` reads REG00: bit 0 = 1
means "boot source ACIN/VBUS". rcS writes the answer to `/run/rsos/bootreason`, and `power_init()` enters
`POWER_MODE_CHARGE` when it says `charger`:

- the UI shows **only a big battery / charging screen** (`power_get_status()`: percent, state, minutes to full): no
  menu, no ROM scan, no emulation, no network (`rsos-net apply` must skip it, §11);
- the LCD is dimmed to 20 % and turns off after 30 s; any button shows it again;
- CPU at the lowest OPP;
- **short power-key press → boot normally**: `on_charge_exit()`, the module rewrites `/run/rsos/bootreason` to `key`
  (a frontend restart does not come back to charge mode) and the UI starts as usual;
- **long press → power off**; **charger removed for 2 s → power off** (`POWER_REASON_CHARGER_REMOVED`); if the
  charger is already gone when the frontend starts, it powers off 2 s later.

While the unit is really off, the AXP keeps charging and the CHGLED (LED1, blue) shows it: charge mode is only what
happens when plugging the charger wakes the board. TODO(hw): confirm REG00 bit 0 (charger plugged while off → 1, power
key → 0, `rsos-bootreason -v` prints the raw registers), and that a power-off with the charger connected stays off.

## 10. API and integration

The board-specific names (the AXP209 supplies and power key, the "RetroStone2" built-in keys, the PEK power-on
time, V_OFF, the governors) are `power_config` fields whose defaults are the RetroStone2's; the menu fills them from
the board profile (`board_apply_power()`, board.ini keys `power_key_device`, `pek_startup_ms`, `battery_supply`,
`ac_supply`, `usb_supply`, `battery_voff_mv`, `backlight`, `thermal_zone`, `cpu_governor_*`; docs/porting.md). On a
board without a PMIC power key (`power_key_device = auto`), any input device that reports `KEY_POWER` with at most 4
keys is the power key, and no PEK timing is written.

```c
struct power_config pc;
power_config_defaults(&pc);                 // target paths and the defaults above
pc.cb.on_status = ...; pc.cb.on_warning = ...; pc.cb.on_critical = ...;
pc.cb.on_sleep_request = ...; pc.cb.on_shutdown_request = ...;
pc.cb.on_screen = ...; pc.cb.on_thermal = ...; pc.cb.on_charge_exit = ...;
power_init(&pc);
// apply settings.ini: for each power key, power_set_setting(key, value)
```

| Call | Who, when |
|---|---|
| `power_get_fd()` + `power_poll()` | main loop: add the fd to `poll()`; call `power_poll()` every iteration (cheap). `power_timeout_ms()` for loops that do not poll the fd |
| **during a game** | `host_launch()` blocks: call `power_poll()` from `host_launch_opts.idle` (every 100 ms) |
| `power_on_input()` | UI: for every nav event (press, repeat, release) from any pad: `if (power_on_input()) continue;` |
| `power_set_game_running(true/false)` | UI, just before `host_launch()` and after it returns |
| `power_set_game_cpu(gov, max_khz)` | UI, before launching, from the core ini (NULL, 0 = default) |
| `power_set_docked(bool)` | display output callback (HDMI in/out) |
| `power_request_shutdown(POWER_REASON_USER / _REBOOT)` | menu items "Power off", "Restart" (`ui_callbacks.power`) |
| `power_sleep(bool)` | nobody (fake sleep is not user-facing any more, §6) |
| `power_poweroff(reboot)` | **last step** of the shutdown handler, after saving |
| `power_set_setting(key, value)` | UI `setting_changed` for `idle_dim_min`, `idle_off_min`, `battery_gauge`, `timezone` (also `sleep_timeout_min`, `sleep_wake`, `idle_dim_s`, `idle_off_s`; -ENOENT for other keys) |
| `power_set_time(t)`, `power_set_timezone(name)`, `power_timezones(&n)` | the date/time screen (61 zones as POSIX TZ strings, no tzdata) |

**Callbacks the frontend implements**:

- `on_status`: redraw the battery icon in the menu, and publish the value for the game child: main.c writes
  `/run/rsos/battery` = `"<percent> <charging>\n"` (percent -1 = unknown, charging 0/1 = charger online), replaced
  atomically (tmp + rename, tmpfs) only when it changes. The child's in-game indicator reads that file every 10 s
  (host-design.md §8), so the game shows exactly the smoothed number of the menu and never computes its own.
- `on_warning(level)`: LOW → toast "Battery low (15 %)"; VERY_LOW → persistent banner; OK → clear. During a game the
  parent cannot draw: forward it to the child (host request below) or skip it (the child's OSD already warns).
- `on_critical`: show "Battery empty, saving..." (menu) — the shutdown request follows at once.
- `on_sleep_request(enter)`: (unreachable now, §6) menu: `display_set_active(false/true)` (display-design.md §8.3, §8.5). Game: `kill(child, RSOS_SIG_SLEEP)` /
  `kill(child, RSOS_SIG_WAKE)`.
- `on_shutdown_request(why)`: game running: `kill(child, RSOS_SIG_POWEROFF)`, wait for its exit (≤ 8 s, then SIGKILL:
  the periodic SRAM flush is on disk); menu: save settings; show "Powering off..." ("Restarting..." for REBOOT,
  "Battery empty, saving..." for CRITICAL) for about 1 s; then `power_poweroff(why == POWER_REASON_REBOOT)`. It must
  not block the process for more than the grace time.
- `on_screen(s)`: OFF → `display_set_active(false)`; ON → `display_set_active(true)` and redraw. If the screen does not
  come back (`display_set_active(true)` fails), main.c re-opens the display (full probe + modeset) and logs
  "screen on failed: re-opening the display".
- `on_charge_exit`: start the normal UI (scan, menu).

**Game child protocol (proposal for docs/host-design.md, matching today's host.c)**. The child already handles SIGTERM
(quit), SIGUSR1 (SRAM flush) and writes `.state.auto` when `H.poweroff` is set:

| Signal (parent → child) | Child action | Status |
|---|---|---|
| `SIGUSR1` | flush SRAM now, keep running | exists |
| `SIGTERM` | quit normally (auto state per setting) | exists |
| `RSOS_SIG_POWEROFF` = `SIGUSR2` | set `H.poweroff`, quit with `HOST_EXIT_POWEROFF`: SRAM flush + `.state.auto` regardless of the setting | **request** |
| `RSOS_SIG_SLEEP` = `SIGRTMIN+1` | pause the core, `audio_pause(true)`, display off, `sram_flush(false)`, wait for the next signal (keep the watchdog quiet with `busy_ok`) | **request** |
| `RSOS_SIG_WAKE` = `SIGRTMIN+2` | drain input (drop the waking key), display on, `audio_pause(false)`, reset pacing (`next_frame_us = 0`) | **request** |

Other host requests: (1) remove the child's own 3 % battery check (`host.c` `BATTERY_CRITICAL`) when a parent exists
(`status_fd >= 0`): it uses the AXP percentage, which may be wrong, and duplicates §4; keep it for standalone
`rsos-run`. (2) Launch with `host_config.governor = false` from the UI: the power module sets the governor (per-core
profiles, sleep, shutdown). (3) The child keeps its `IN_HK_POWER_OFF` flush (harmless: the parent's long press arrives
at the same time) but must ignore `IN_HK_POWER_SHORT`.

**Display request**: `display_set_active(bool)` (CRTC `ACTIVE` = 0/1 in one atomic commit; drm_panel then also turns the
panel and its backlight off). Until it exists, the module's `bl_power` = 4 already removes most of the power.

**Input request**: an `input_config` flag to skip the `axp20x-pek` device (or the UI simply ignores the two power
hotkeys). The brightness keys keep working; while the screen is dimmed their first press only wakes it.

## 11. Init-script changes (for the build owner)

`/etc/init.d/rcS`, early (before the frontend starts; costs ~1 ms, no dependency on `/data`):

```sh
mkdir -p /run/rsos
/usr/bin/rsos-bootreason -w /run/rsos/bootreason > /dev/null 2>&1
```

`/etc/init.d/rcS`, right after `/usr/libexec/rsos/data-partition` (needs `/data` mounted):

```sh
/usr/bin/rsos-clock restore        # moves the clock forward to /data/rsos/lastclock if the RTC reset
```

`/etc/init.d/rcK`, after the frontend has exited and before `umount /data`:

```sh
/usr/bin/rsos-clock save
```

`/usr/bin/rsos-net`: (1) `apply` does nothing when `/run/rsos/bootreason` says `charger` (charge mode); the frontend
then runs `rsos-net apply` in the background from `on_charge_exit` if the user boots normally; (2) after a
successful DHCP (WiFi or Ethernet), the udhcpc hook `/usr/share/udhcpc/default.script.d/rsos-ntp` runs, in the
background and once per boot: `timeout 30 ntpd -q -n -p <DHCP option 42 servers> -p pool.ntp.org && rsos-clock systohc
&& rsos-clock save` (BusyBox ntpd, client only, enabled in `board/common/busybox.fragment` since the system review:
before that the applet was missing and the hook failed silently; `systohc` replaces `hwclock -w`). TODO(hw): check
once on the device that the static BusyBox resolves `pool.ntp.org` (glibc loads `libnss_dns` at run time; it does
under qemu-arm with the target's libraries: `busybox ntpd -n -w -d -p pool.ntp.org` resolved and got replies). The frontend
applies the `timezone` setting at start with `power_set_timezone()`; children inherit `TZ`.

Data safety (requirements §6), done in rcS (two `echo`s into `/proc/sys/vm`, no fork) since the system review:
`vm.dirty_expire_centisecs=200` (dirty pages reach the card after 2 s instead of 30 s) and
`vm.dirty_writeback_centisecs=100` (the flusher looks every second instead of every 5 s). The flusher only runs
while something is dirty, so an idle system gets no extra wake-ups; the cost is more, smaller writes to the card
around saves. And `data-partition` checks the exFAT VolumeDirty flag at every boot: Linux sets it at the first write
and clears it only at unmount, so it is set after any power cut, forced power-off (4-6 s press), freeze or V_OFF
cut-off. Then `fsck.exfat -p` runs before the mount (the "Checking the SD card" splash; result in
`/run/rsos/data-fsck`, "`-p <status>`"). Measured on a 32 GiB volume with 6,040 files: 156 read requests, 1.5 MiB
(67 ms on the PC, 360 ms for the ARM binary under qemu-user), so well under a second on the A20 (TODO(hw): time it
on a full card). A clean boot costs one 1-byte read.

**Hardware watchdog** (system review S7): U-Boot starts the A20 watchdog (16 s, its maximum) and services it until
the kernel starts; the kernel finds it running and services it itself (`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED`, no open
timeout) until a process opens `/dev/watchdog`. So a frozen kernel (the idle freeze of the first hardware boot, a
driver deadlock) resets the board, and a long first boot never does. Soft and hard lockups panic, and a panic
reboots after 10 s; `boot.cmd` counts every such boot until `rsos-boot-ok` confirms it (docs/build.md, "A/B
slots"). **Frontend part (to do):** open `/dev/watchdog` in the menu process (`O_WRONLY|O_CLOEXEC`, never in
`--splash` or `--run`), pet it (`WDIOC_KEEPALIVE`) from the main loop at least every 4 s, also while a game runs, in
fake sleep and in charge mode, and stop it with the magic close (write `V`, then close) on every orderly exit; a
menu that hangs for 16 s then resets the board. rcK also writes `V` after the frontend has stopped. Nothing in the
menu loop may block that long (review B2): the probe and mount of a USB drive run in usb.c's worker thread, a
stopped update check is SIGKILLed and reaped with `WNOHANG` (its resolver gets `RES_OPTIONS=timeout:2 attempts:1`),
the storage format runs in a child polled with `WNOHANG`, and the exit paths (SIGTERM, power-off) pet it before each
step. A menu that has no display for 60 s on a board whose panel keeps scanning powers off cleanly. TODO(hw): on the
first boot with this U-Boot, check that the board does not reset after 16 s and that `dmesg | grep -i wdt` shows no
error (the node `watchdog@1c20c90` is enabled in the DTB and the driver is built in; QEMU shows U-Boot starting it).

`rsos-bootreason`: prints `key`, `charger` or `unknown`, exit status 0 / 10 / 1, `-w FILE` writes the word, `-v` prints
REG00/REG01. `rsos-clock save|restore|systohc|show [FILE]` (default `/data/rsos/lastclock`); `restore` also writes the
RTC and creates `/run/rsos/clock-restored` (the UI can say "time restored from the last shutdown, check it";
`power_get_status()->clock_restored`). The restore floor is the firmware build time.

Package (for `buildroot-external/package/rsos-frontend`): `make -f power.mk HOST_CC="$(TARGET_CC)" POWER_BUILD=...
power-all power-install DESTDIR=$(TARGET_DIR)` installs `/usr/bin/rsos-bootreason` and `/usr/bin/rsos-clock`; the
frontend links `$(POWER_OBJS)` (it already has `uevent.c`), compiles `src/power/pclock.c` with `$(POWER_DEFS)` (the
build epoch; `SOURCE_DATE_EPOCH` when set) and links `-lm`.

## 12. Kernel configuration needs (for the kernel fragment owner)

Already set (checked in `~/rsos/output/build/linux-6.18.54/.config`): `MFD_AXP20X_I2C`, `BATTERY_AXP20X`,
`CHARGER_AXP20X` (the AC supply), `AXP20X_POWER`, `INPUT_AXP20X_PEK`, `I2C_CHARDEV` (rsos-bootreason),
`CPU_FREQ_GOV_SCHEDUTIL`, `CPU_FREQ_GOV_PERFORMANCE`, `THERMAL`, `THERMAL_OF`, `CPU_THERMAL`, `TOUCHSCREEN_SUN4I` (the
thermal sensor), `RTC_DRV_SUNXI`, `RTC_HCTOSYS`, `RTC_INTF_DEV`, `LEDS_GPIO`, `HOTPLUG_CPU`, `BACKLIGHT_PWM`.

To add:
- `CONFIG_CPU_FREQ_GOV_POWERSAVE=y`: fake sleep and charge mode (the module falls back to clamping `scaling_max_freq`,
  so it is not strictly required, but it is 1 KB and makes the intent visible).

Keep: `CONFIG_I2C_CHARDEV=y` (never disable it in a fragment), `CONFIG_SUSPEND` may stay (unused). Not needed:
`CPU_IDLE` (no A20 cpuidle driver), `LEDS_TRIGGER_TIMER`.

## 13. Device tree change

`buildroot-external/board/retrostone2/dts/sun7i-a20-retrostone2.dts`: a `battery: battery` `simple-battery` node
(Li-ion, 4000 mAh, `voltage-min-design` 3.0 V, `voltage-max-design` 4.2 V, `constant-charge-current-max` 1.0 A,
`constant-charge-voltage-max` 4.2 V) and `monitored-battery = <&battery>` on `&battery_power_supply`. The AXP209
driver applies V_OFF = 3.0 V and the 1.0 A charge current; the rest describes the cell. Compiled with
`make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=~/rsos/src/build-dts allwinner/sun7i-a20-retrostone2.dtb`: no
dtc warning; `CHECK_DTBS=y` reports nothing for the new nodes (only the known board-compatible and panel messages).

## 14. Time

- No RTC backup: the RTC survives power-off while the battery is connected and not empty.
- `rsos-clock restore` at boot, `save` at shutdown (rcK; `power_poweroff()` only when it calls `reboot(2)` itself or a `do_poweroff` hook replaces init), and the module saves every 10 minutes
  (only a sane clock, i.e. not before the build, is ever saved). Durable writes (tmp + fsync + rename + fsync(dir)).
- Manual time: `power_set_time(t)` sets the system clock, the RTC and lastclock, and clears `clock_restored`.
- Time zone: `power_set_timezone("Europe/Paris")` or any POSIX TZ string; 61 zones in `power_timezones()` with their
  current rules (checked against glibc in the tests: offsets in January and July for 16 zones including half-hour,
  45-minute and southern-hemisphere DST zones).
- NTP: only when the network is on (WiFi or Ethernet), from the udhcpc hook (§11).

## 15. Settings (settings.ini keys the UI shows)

| Key | Values | Default |
|---|---|---|
| `idle_dim_min` | 0 (never), 1, 2, 5 | 2 |
| `idle_off_min` | 0 (never), 2, 5, 10 | 5 |
| `idle_poweroff_min` | 0 (never), 5, 10, 15, 30, 60 (§7.1) | 5 |
| `battery_gauge` | `auto`, `voltage`, `axp` | auto |
| `timezone` | a zone name | UTC |

`sleep_timeout_min` and `sleep_wake` (fake sleep, §6) are still parsed by the module but no longer shown or written.

## 16. Tests

`make -f power.mk POWER_BUILD=~/rsos/power-build power-check` (in WSL, output outside `/mnt/c`): 314 checks, 0
failures (2026-09-26), zero compiler warnings (`-Wall -Wextra`; also clean with `-Wshadow -Wformat=2` and ASan/UBSan).
`power-arm-check` compiles and links the module, both tools and the test binary with `arm-linux-gnueabihf-gcc
-mcpu=cortex-a7`, zero warnings.

Covered: the OCV curve; full simulated discharges (voltage, auto, auto with a bogus 100 % gauge, AXP) with noise and
spikes: monotonic display, exactly one LOW, one VERY_LOW and one critical, trigger timing; spike rejection (single and
double), hovering around the threshold, short dips, emergency collapse, charger re-arm; warning hysteresis; and on
fake sysfs trees with a fake clock: PEK force-off raised to 6 s, V_OFF, governors per state and per core, idle dim/off
(the idle power-off, §7.1: the order dim, screen off, power off; equal timers and a shorter power-off; the
notice, lit screen and cancel by a UI input, a built-in key, the power key and game activity; the busy hold and the
restart after it; the cancel of a power-off that cannot save and the watchdog that never forces it; charge mode;
review B2: a game ending or starting during the notice cancels it and the dim/off stages come back, a game without
save states or a benchmark held busy for hours),
and key swallowing (including the wake regression: the waking press and release as the evdev read sees them, then
the next presses, repeats and releases must pass, after idle-off, dim and fake sleep), the power key (start-up guard,
a lone release, short press in the menu and in a game = one shutdown request even if pressed again, screen off =
wake only, dimmed = power off, the charge-exit press not repeated by a quick second tap), fake sleep through
`power_sleep()` (wake by button and by the power key, `sleep_wake=power`, the 15-minute timeout with poweroff and
lastclock, timeout 0), the powersave fallback, long press and the shutdown watchdog, reboot,
the critical path (noise ignored, sustained trigger, none while charging), repeated VERY_LOW warnings, charge mode
(dim, screen timeout, exit to normal boot, charger removed, charger already gone, long press), thermal hysteresis and
shutdown, settings parsing, the clock file and restore rule, time zones, REG00 decoding.

## 17. TODO(hw)

- [ ] REG00 bit 0 on charger insertion vs power key (`rsos-bootreason -v`), and power-off with the charger connected stays off.
- [ ] PEK `startup`/`shutdown` values read back; 6 s force-off works; 2 s long press feels right.
- [ ] Cell capacity and datasheet: charge current (1.0 A in the DTS), cut-off, and that REG33 charge target reads 4.2 V (`voltage_max`).
- [ ] A logged full discharge (V, I, AXP %) to fit the OCV table and R_int, and to see if the AXP gauge is usable.
- [ ] Critical threshold: loaded voltage at the moment the AXP cuts at V_OFF 3.0 V under a PS1 load; minutes left at 3.45 V.
- [ ] Current draw: menu, NES, PS1, HDMI, WiFi on, fake sleep (with and without cpu1 offline), off.
- [ ] `/sys/power/state` content (expected `freeze` only).
- [ ] Thermal: `cpu-thermal` readings sane (sun4i-ts calibration), temperature after 30 min of PS1 in the closed case.
- [ ] Charging while playing from a 1 A and a 2 A charger (ACIN has no input current limit on the AXP209).
- [ ] A short power-key press in the menu and in a game powers off cleanly (log: `power key short press: powering
      off`); the game offers "Resume where you left off?" next time; the press that powers the unit on never turns it
      off again.
- [ ] Idle dim (2 min) and screen-off (5 min) in the menu: a button brings the screen back, and the **next** button
      works (the 2026-09-26 bug).
- [ ] RTC drift over 24 h (LOSC on the external 32 kHz crystal, requirements §7).
