# First hardware bring-up: a step-by-step test script

The first real boot of RetroStoneOS on a RetroStone2. Work top to bottom and stop at the first thing that
fails. **Copy the whole UART log** into a file and share it: that's the most useful single artefact.

Use the newest **`images/retrostoneos-dev-*.img`**. It boots straight into the menu on the LCD, **and** keeps a root
shell on the UART with all the test tools (`rsos-kmstest`, `rsos-display-selftest`, `rsos-run`, `evtest`,
`speaker-test`, `modetest`, `i2c-tools`, all 25 cores).

The menu (`rsos-frontend`) owns the display. To run the low-level display tests below, stop it first; init
respawns it, so stop it cleanly:
```sh
kill -TSTP 1                      # BusyBox init stops respawning until it gets SIGCONT
killall rsos-frontend
# ... run the tests ...
kill -CONT 1                      # the menu comes back
```
Logs from the menu go to `/run/rsos/frontend.log` (also shown on the UART).

### Logs without a UART
Once the menu is on screen, the console copies its logs to the SD card every 2 seconds for 5 minutes, then every
30 seconds up to 60 minutes (a play session): **RETROSTONE drive → `rsos/logs/`**. `last.txt` gives the newest boot
number, and `boot<N>/` holds:
- `dmesg.txt`: the kernel log
- `frontend.log`: the menu's log
- `game.log`: the games' own logs (core messages, pacing, overlay), one `=== [uptime] launch: core rom` header per game
- `rcS.times`: the boot script's steps with their uptime (`1.021 data-partition`, ...)
- `bootstage.txt`: the whole boot on one time line, from power-on to the menu (section 10)
- `initcalls.txt`: only on a boot with `initcall_debug` (section 10)
- `bootlog.txt`: the logger's own events (start, signals, exit). `heartbeat.txt` names the step it was in. (Up to
  image `20260927b` the logger died after its first pass: a variable clash in its status code; fixed in `20260927c`.)
- `../shutdown.txt` (in `rsos/logs/`): the step times of the last 5 shutdowns
- `status.txt`: CPU frequency and voltage, temperature, battery, SD card counters and `top`
- `heartbeat.txt`: a counter; if it stopped early, the whole system froze at that moment
- `info.txt` (also: when the menu came up, the /data mount options, the SD card's I/O scheduler)

After a freeze: hold power 6 s, put the card in the PC, and zip the `rsos/logs` folder for analysis.
(Create an empty file `rsos/logs/disabled` to turn the logger off.)

**The first boot** (the data partition conversion) runs before the logger can write anything. Since image
`20260928-firstboot` it keeps its own trace: each step goes to the UART (`rsos-data: ...`), to the kernel log and to
a raw spot on the card (3 MiB, outside every partition), and the next boot that reaches the menu appends it to
`rsos/logs/firstboot.txt`; the splash's own log is `boot<N>/splash.log`. The splash can no longer hold the boot, and
a first boot with no progress at all for 3 minutes lets the hardware watchdog reset the board (docs/build.md,
"First boot").

### A first boot that stays black: UART capture
1. Wire the UART (section 0) and log the whole session to a file (PuTTY: Session > Logging > "All session output").
2. Flash the image. Note whether you copy files to the RETROSTONE drive before the first boot, and how many GB:
   those are backed up and restored during the first boot, which then takes minutes, with the splash on screen.
3. Full kernel log (optional but best): power on while pressing Enter repeatedly in the terminal until U-Boot's `=>`
   prompt appears, then type `setenv rsos_extraargs loglevel=7`, `saveenv`, `boot`. Without it the `rsos-data:`
   lines and the kernel's warnings still print.
4. Power on and wait **5 minutes** without touching anything (the kernel reports a task blocked for 120 s, and a
   first boot stuck for 3 minutes resets the board by itself). Then save the log.
5. What to look for, in order: the last `rsos-data:` line (the step it stopped at); `the splash (pid N) still there
   ... SIGKILL` / `does not go away`, followed by the splash's kernel stack; `no progress for ... the board resets`;
   `INFO: task ... blocked for more than 120 seconds` with its stack; `flip_done timed out`, `hw_done or flip_done
   timed out`, `Unable to handle kernel`, `Internal error`, `Kernel panic`; and whether the SPL lines and
   `Starting kernel` appear at all. Nothing after the SPL lines means a hang before Linux.
6. After the next boot that reaches the menu: `rsos/logs/firstboot.txt` and `rsos/logs/boot<N>/splash.log`.
   Remove the extra kernel output afterwards: `fw_setenv rsos_extraargs` on the UART shell.

**If the screen stays black but the UART works**, it's the display or panel timing: go straight to section 2.
**If nothing appears on the UART**, check the wiring (TX/RX swapped?) and the card flash.

## 0. Prepare
1. Flash `images/retrostoneos-*.img` with balenaEtcher or Rufus. The full procedure is in [build.md](build.md#flashing-the-sd-card-windows).
2. **UART**: connect a 3.3 V USB-UART adapter to the `UART0` header, which has three pads: TX = PB22, RX = PB23, GND.
   Adapter RX goes to board TX, and adapter TX to board RX. Use 115200 8N1 (PuTTY, or the Arduino serial monitor).
   **Never connect the adapter's 5 V/3.3 V power pin.**
3. Don't plug in HDMI yet.

## 1. Boot
- [ ] The SPL prints its few lines (U-Boot proper is silent since image `20260927c`, see section 10; with
  `rsos_verbose=1` it prints its banner and loads `boot.scr`).
- [ ] The kernel boots to a `#` prompt on the UART. **Save the full log.**
- [ ] First boot: the data partition grows (a message from `data-partition`). Check with `df -h /data` (it should show the card size), then
  `ls /data/roms/*`: the homebrew games are there (gb, gbc, nes, megadrive).

What the screen should show (there is no U-Boot or kernel picture: the LCD stays dark until the frontend's first
modeset):
- [ ] **First boot only**: the RetroStone2 logo on a dark slate background (`#2a2a35`) with *"Preparing the SD card,
  please wait..."* underneath, while `data-partition` grows and formats the data partition (a few seconds on an empty
  card, longer when files copied from a PC must be converted). It is `rsos-frontend --splash`, started only around those
  steps. Later boots never show this message.
- [ ] **Every boot**: the same logo (no message) as the very first picture, right after the frontend opens the display,
  then the menu replaces it. `grep splash /run/rsos/frontend.log` shows
  `splash: ... shown N ms after start (read .. us, decode + present .. us)`. To keep it up longer, for a photo or to judge
  the colour: `echo boot_logo_min_ms=2000 >> /data/rsos/settings.ini` and reboot (0, the default, adds no delay).
- [ ] **Normal boot** (image `retrostoneos-dev-20260927a` and later): **the menu (the system carousel, on the system
  you last played) within about 0.3 s of the logo**, with no "Preparing your console" screen. The carousel comes from
  a small snapshot of the last boot (`/data/rsos/cache/systems.idx`); the game lists are checked in the background
  just after, and if something changed while the console was off (games copied or removed on a PC), the carousel
  updates by itself a moment later without moving the selection. Opening a system right away works (its list loads
  on the spot). Before this image, the menu took ~4 s after the logo (the "Preparing" screen with 34 folders).
- [ ] **First boot** after flashing (and the first boot of a new image with a different set of emulators): the
  "Preparing your console..." screen with a progress bar, now brief (well under a second is expected), then the menu.
- What to send back for boot speed: `rsos/logs/boot<N>/frontend.log`, `rcS.times` and `dmesg.txt` of a **normal**
  boot (the second one after flashing). The lines that matter:
  `ui: warm-up`, `ui: created in`, `ui: menu from the snapshot`, `ui: first frame in` (theme parsing, images, `io N
  reads`), `frame 0: ... (wait for a buffer ..., render ...)`, `menu up N ms after start`, the `games: <system>:` lines,
  `ui: game lists complete ... (io N reads ...)`, `input: opening eventN took` (only if slow), and in `rcS.times` the
  time of each step (`data-partition` in particular).
- [ ] **Charge mode** (the unit was switched on by plugging the charger): no logo, the battery screen comes first.
- [ ] On HDMI (plugged in at boot) the logo is centred on the TV with the same background colour on the sides.
- The colour and size are build options (`SPLASH_BG` in `frontend/Makefile`, `BR2_PACKAGE_RSOS_FRONTEND_SPLASH_BG` in
  Buildroot); previews of other colours are in `docs/ui-previews/splash-*.png`.
- [ ] Put the card in a PC: the **RETROSTONE** drive appears, no "format this disk?" prompt shows, and the games are visible.

Quick health check:
```sh
dmesg | grep -i -E "error|fail|warn" | head -50
dmesg | grep -i -E "drm|panel|lima|axp|hdmi|mmc|brcm|gpio-keys|adc-joystick"
cat /proc/cpuinfo | grep -i -E "processor|hardware"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies
```

## 2. Display (LCD)
Full checklist in [display-design.md §10](display-design.md). The short version:
```sh
rsos-kmstest --list          # 2 CRTCs, planes, connectors "Unknown-1"/DPI (panel) + HDMI-A-1
rsos-display-selftest        # must print OK and "NEON"
rsos-kmstest                 # animated test pattern on the LCD, a moving bar: look for tearing
rsos-kmstest --format rgb565 # should say "HW RGB565" (kernel patch 0003, backend 2x scaler)
```
- [ ] The picture is stable, the colours are correct (red/green/blue not swapped), there's no shift, and the border is visible on all four edges.
- [ ] Backlight: `echo 20 > /sys/class/backlight/*/brightness` then `echo 80 > ...`. The brightness changes.

### 60 Hz LCD test (important for smooth emulation)
The LCD runs at **78.6 Hz** (33 MHz pixel clock). Since image `retrostoneos-dev-20260927x` a true 60 Hz (25.2 MHz,
display-design.md §3.1) can be tried **live, without a reboot or `fw_setenv`**:
1. Settings > Display > **LCD refresh rate** > **60 Hz**. The screen switches at once and asks "Keep this setting?
   Reverting in 15 s". If the picture is fine, press Left (KEEP) then A. If the screen is black, garbled or
   flickers, do nothing: it goes back to 78 Hz by itself after 15 s (B or A on REVERT also go back).
2. Play NES/SNES/Game Boy games for a few minutes: scrolling should be smooth (no judder), sound clean.
- [ ] The picture at 60 Hz: stable, no flicker, colours right, no shift. Kept after a reboot.
- [ ] `frontend.log`: `LCD retimed to 60 Hz: pixel clock 25200 kHz (... 126 MHz / 5 = 25.200 MHz expected, 60.000
      Hz)` and **`refresh measured on Unknown-1: 60.0xx Hz`** (send the line; at 78 Hz every boot logs `78.57x Hz`).
- [ ] `game.log` of a NES/SNES game: `lcd refresh: 60 Hz (25.2 MHz user mode)` and `pacing: vsync+DRC (... display
      60.000 Hz ...)` (at 78 Hz it says `audio clock`).
- [ ] If 60 Hz fails: Settings > Display > LCD refresh rate > 78 Hz (legacy). From a PC: delete the `lcd_refresh`
      line in `rsos/settings.ini` on the RETROSTONE drive. On the UART: `rsos-kmstest --lcd-refresh 60` tries it
      without touching the setting.

## 3. Buttons and stick
```sh
evtest                      # lists the devices; pick "RetroStone2 Buttons", then each of the others
```
- [ ] **RetroStone2 Buttons**: A=BTN_EAST (right), B=BTN_SOUTH (bottom), X=BTN_NORTH (top), Y=BTN_WEST (left),
  Start, Select, D-pad (4 directions). Press each one: exactly one event per press and release, with no bouncing.
- [ ] **RetroStone2 Shoulder Buttons**: L1, R1, L2, R2.
- [ ] **RetroStone2 Brightness Keys**: + and −.
- [ ] **analog-stick** (only if the add-on is fitted): ABS_X/ABS_Y move smoothly and centre at rest. Without a stick,
  the values sit outside 0..3000.
- [ ] Power button: `evtest` on "axp20x-pek" gives KEY_POWER.

## 4. Audio
```sh
aplay -l                                   # cards: the sun4i codec (and sun4i-hdmi when HDMI is connected)
speaker-test -D hw:0 -c 2 -t sine -l 1     # speaker: a sine tone, left then right
```
- [ ] The speaker plays, and the **volume wheel** changes the level.
- [ ] Headphones plugged in: sound in the headphones, and the speaker goes silent (the analog jack switch).
- [ ] No loud pop at boot or when audio starts.

## 5. HDMI hot-switching (the key feature)
```sh
rsos-kmstest --monitor      # then plug/unplug HDMI several times: one hotplug event each, within ~0.5 s
rsos-kmstest                # with the pattern running, plug HDMI in: the picture moves to the TV and the LCD goes dark
```
- [ ] Plug in: the picture is on the TV within ~1 s (the scaled pattern visible, not black: HDMI now runs on TCON1,
  kernel patch 0006), and the LCD backlight is off. The LCD keeps scanning black behind it (panel safety, below).
- [ ] Unplug: back on the LCD.
- [ ] 20 plug/unplug cycles with no hang. Booting with HDMI already plugged in also works.
- [ ] HDMI audio: `aplay -l` shows `sun4i-hdmi`, and `speaker-test -D hw:sun4i-hdmi -c 2 -t sine -l 1` plays on the TV
  (experimental patch 0002).
- [ ] If the picture fails on HDMI (black game image, the TV has a signal): send `frontend.log` and try
  `rsos-kmstest --hdmi 640x480 --format rgb565` (backend x2, no frontend). Never work around it with
  `--hdmi-crtc 0`: that stops the LCD's signals (it is ignored anyway).

## 6. Power
```sh
cat /sys/class/power_supply/*/uevent       # battery %, voltage, status; AC online when charging
cat /sys/class/thermal/thermal_zone*/temp
```
- [ ] The battery % looks plausible, and plugging in the charger switches the status to Charging.

Power key and screen-off (image `retrostoneos-dev-20260926c` and later; there is no sleep mode any more):
- [ ] **Menu, short press** of the power key: "Powering off..." appears at once, the unit is off about 1 s later
      (`frontend.log`: `power key short press: powering off`, `powering off (user)`).
- [ ] **Game, short press**: "Powering off..." shows over the game, then it powers off. Power on again: as soon
      as the menu is up, **"Resume <game>?"** [RESUME] [START FRESH] is shown (image `retrostoneos-dev-20260927r` and
      later). RESUME starts the game where you were (same core; the pad that pressed A is player 1); START FRESH (or
      B) leaves the menu, and starting that game then still offers "Resume where you left off?". The question comes
      once: after either answer the next boot goes straight to the menu. `frontend.log`: `resume: recorded ...` at
      the power-off, `resume: offering ...` and `launch ..., resuming (boot offer)` at the next boot; `game.log`:
      `auto state saved (power-off): N KB in M ms`.
- [ ] **Resume after Select+Start** (Settings > Games > "Auto-save on exit", on by default): play a game, exit with
      Select+Start (or Select+X > Exit game), start it again: "Resume where you left off?" continues from the exit
      point. Note the exit time for NES/SNES/GBA (`game.log`: `auto state saved (exit): N KB in M ms`, expected
      under ~100 ms) and for PS1 and N64 (several MB: "Saving..." shows over the game; send back the times, the
      target is about 1 s). With "Auto-save on exit" off the exit writes nothing (`auto state not written`), but a
      power-off still saves. Settings > Games > "On launch": Always resume / Always start fresh skip the question.
- [ ] Power on with a normal press, and also holding the key 2-3 s: the unit boots to the menu and does **not**
      power off by itself (the power-on press is ignored). A double tap on the key does nothing more than one press.
- [ ] **Idle screen-off** (the 2026-09-26 bug): leave the menu alone for 2 min (dims) and 5 min (screen off). Press a
      button: the screen comes back and that press does nothing. Then press **another button: the menu must react**
      (it did not before). Same after the dim only. A power-key press while the screen is off only turns it on; a
      press while it is dimmed powers off. To test quickly: Settings > Power > "Screen off after" 2 min.
- [ ] **Idle power-off** (power.md §7.1; Settings > Power > "Power off after", default 5 min, the same as "Screen off
      after"). In the menu, touch nothing: with the defaults the screen dims at 2 min, then at 4:50 **"Powering off in
      10 s — press any button to cancel"** shows and at 5:00 the unit powers off (frontend.log: `idle power-off: notice`,
      `powering off (idle)`). Again, and press a button during the notice: it goes away, that press does nothing else,
      and the next power-off comes 5 min later. Set "Screen off after" 2 min and "Power off after" 10 min: dim, screen
      off, then the screen lights up with the notice at 9:50. In a game (with a USB pad on HDMI too), steer with the analog stick only for 10 min (N64 or PS1): no
      notice. Then stop playing for 5 min: the notice on the game's screen, then "Powering off..."; at the next boot "Resume <game>?" (resume.ini
      `reason = idle`). While a USB import or export runs, or a PC copies over the SMB share, or an update downloads:
      no notice at all (`idle power-off held: usb-import` / `smb-client` / `os-update`), and the countdown starts again
      when it ends. Never in charge mode.
- [ ] **Panel safety: the LCD keeps scanning while "off"** (image `retrostoneos-dev-20260928-lcdsafe` and later;
      display-design.md §8.5). A powered TFT must never be left without its signals: the old screen off stopped them
      for over an hour and damaged a panel. With "Screen off after" 2 min, let the screen go off, then on the UART:
      `grep -A3 "crtc-0" /sys/kernel/debug/dri/0/state` shows `active=1` (mount debugfs first:
      `mount -t debugfs none /sys/kernel/debug`), `cat /sys/class/backlight/*/bl_power` gives `4`, and frontend.log has
      `screen off (Unknown-1: backlight off, black frame, still scanning)` (never a plain `screen off (Unknown-1) in N
      ms`, the old ACTIVE = 0 path). The same with HDMI plugged in (`crtc-0` stays `active=1`, HDMI on `crtc-1`),
      during sleep (short power-key press in the menu and in a game), and just before power-off
      (`display closed: planes off, the panel keeps scanning` at the end of frontend.log). Leave the unit in screen
      off for 30 min: no lines, flicker or ghost image when it comes back.
- [ ] **In-game battery** (Settings > Display > "Battery in games" on, the default): a small pill with a battery icon
      and the same % as the menu, top-right, over any game (NES, SNES, PS1, N64). Plug the charger in: a yellow bolt
      appears within ~10-20 s. Try "Battery position" bottom-left, and "off". It hides while the Select+X menu is
      open. On HDMI it moves with the picture (a bit further from the edges for TV overscan).
- [ ] After each test, `grep -E "overlay|flip event|screen on failed" /run/rsos/frontend.log /run/rsos/game.log`:
      `overlay: plane N, 72x20 at 564,4` is expected; `overlay refused`, `no page-flip event` or `screen on failed`
      are not (send them back if they appear).

## 7. Emulator core smoke test
First from the menu: open NES → Nomolos, Game Boy → Run to Databay, Mega Drive → Virtua Worm.
- [ ] The game starts, runs at full speed, the sound is in sync, and the buttons work.
- [ ] Hotkeys: Select+Start exits, Select+R/L saves/loads a state, Select+X opens the in-game menu.
- [ ] Plug HDMI in **during** a game: the picture moves to the TV. Unplug it: the picture comes back to the LCD.

If a game fails from the menu, run it directly on the UART for the error messages (menu stopped as above):
```sh
rsos-run --core /usr/lib/libretro/fceumm_libretro.so --rom "/data/roms/nes/nomolos.nes"
rsos-run --core /usr/lib/libretro/gambatte_libretro.so --rom "/data/roms/gb/databay.gb"
rsos-run --core /usr/lib/libretro/picodrive_libretro.so --rom "/data/roms/megadrive/Virtua Worm.bin"
```
- [ ] Full speed, sound in sync, the buttons work, and Select+Start exits.

### 7b. N64: speed, the FPS overlay and the benchmark (no UART needed)
N64 now defaults to the **Rice** renderer at 320x240 with **automatic frameskip** (docs/cores.md, "N64
performance"). To see how fast a game really runs, and to find its best settings:

1. Start the game and play to a typical, busy scene (not a menu or the title screen). Stand still.
2. **Select+X → Show FPS → On.** A line appears at the top of the screen, next to the battery:
   `SPD 97% FPS 29.8 SKIP 12 CPU 95/31 GPU 4.1`. SPD is the emulation speed (100 % = real speed), FPS the new
   pictures per second (many N64 games draw 20 or 30), SKIP the frames not shown, CPU the load of each of the two CPU
   cores, GPU the milliseconds per frame spent drawing on the GPU and swapping. It costs nothing (it is on the display's
   overlay plane; the N64 picture stays zero-copy).
3. **Select+X → Benchmark this game → Start benchmark.** The game restarts from this exact moment once for each of
   about 10 settings (Rice, Glide64, gln64, Count Per Op 3, frameskip 1, the Mesa GL thread, 640x480,
   Mupen64Plus-Next and its threaded renderer if installed), 5 s warm-up + 25 s measured each, **no input needed, don't
   touch the pad** (about 6-7 minutes in total). A yellow line says which run is going on. **Select+Start stops it**
   (what was measured is kept).
4. At the end the game comes back at the same moment with the **Benchmark results** page: the settings ranked
   (full speed first, then the most pictures shown). **Use this for this game** saves the winner for this game only
   (`/rsos/coreopts/<core>/<game>.ini`, and `/rsos/cores.ini` if the winner is Mupen64Plus-Next); it applies the next
   time the game starts. The benchmark measures speed only: look at the screenshots before keeping a setting.
5. On the PC, the RETROSTONE drive → `rsos/logs/`: `bench-<game>-<date>.txt` (the table: speed %, fps, effective fps,
   p95/p99 frame time, core and GPU swap milliseconds, CPU load per core, skipped frames, notes) and one
   `bench-<game>-<date>-NN-<setting>.png` per run (the last picture: check that the renderer draws correctly).
   `game.log` (`rsos/logs/boot<N>/`) also has, for every N64 session, the effective core options, the renderer, the
   pacing mode, a `perf:` line every 10 s and a `perf session:` summary at exit.

- [ ] Send back the `bench-*.txt` files and the PNGs of 2-3 games (a light one, e.g. Mario Kart 64 or Super Mario 64,
      and a heavy one, e.g. Zelda OoT or GoldenEye), plus `game.log`.
- [ ] **Stalls and the shader cache** (image `retrostoneos-dev-20260927x`, host-design.md §13.4): the 2026-09-27
      log showed Super Mario 64 at 67 % with frames stuck for 0.4-0.6 s, because Mesa could not keep its compiled
      shaders (`Failed to create //.cache for shader cache`). Now they go to `/data/rsos/cache/mesa`. Test: start
      Super Mario 64, play the same ~30 s twice (exit with Select+Start in between). Send `game.log`: the first
      session has `stall: frame N: core X ms; GL: N compiles ..., N slow draws ...` lines, the second should have
      far fewer, a lower `max` in the `perf:` lines, fewer `underruns`, and a higher `speed`. Also check
      `shader cache: /data/rsos/cache/mesa (Mesa database, max 64 MB)` at the start, Mesa's cache stats at the
      end, `cpu: governor performance, 960 MHz` and `at 960 MHz` in the `perf:` lines.
- [ ] If a setting shows a black or broken picture in its PNG, say which game and setting.
- [ ] Core options (Select+X → Core options): a changed value is marked `*`, and saved for this game when you leave
      the page (then shown `(game)`); `(all)` is saved for all games. Change the GFX plugin, exit, restart: it stays.

### 7c. USB import speed (image `retrostoneos-dev-20260927x`)
The 2026-09-27 import copied 1.1 GB in 298 s (**3.9 MB/s**). The copy engine now reads the stick and writes the
card at the same time (rom-transfer.md §2.3, "Copy engine"; on emulated devices 5.7 -> 8.9 MB/s).
- [ ] Import the same stick into a card without those games (or delete them first): send `frontend.log`. It has one
      line per folder, `transfer: import roms/n64: 4 files, 256 MB in 28.1 s = 9.1 MB/s (waiting for the source
      1.3 s, writing 0.2 s, flushing 26.6 s)`, and the total `import done: ... (X MB/s; waiting for the source ...,
      flushing ...)`. "flushing" large = the SD card is the limit; "waiting for the source" large = the stick.
- [ ] Also an export of a few games to the stick (Export games): `export roms/...` lines, same format.

### 7d. Power-off time (image `retrostoneos-dev-20260927x`)
The frontend no longer holds "Powering off..." for 300 ms, does not sync (rcK does) and exits right after
signalling init; a game process exits right after its saves (power.md §5).
- [ ] Power off from the menu and from a game (NES, then N64) a few times; stopwatch key -> screen off, and send
      `rsos/logs/shutdown.txt` and `boot<N>/frontend.log`: `frontend: shutdown requested` -> `rcK start` should be
      well under the 0.4-0.8 s measured before; `/run/rsos/shutdown-signal` (the uptime when init was signalled) is
      copied by the boot logger if it collects `/run/rsos`. After each, the next boot offers "Resume <game>?" for
      the game case (the saves are complete).

### 7e. Languages (image `retrostoneos-dev-<date>-i18n`, docs/translating.md)
The UI, the in-game menu (Select+X) and the toasts are translated into 21 languages. An existing card has no
`language` in `settings.ini` yet, so the first boot of this image shows the **language picker** before the menu.
- [ ] First boot: the picker is up (big list, "Choose your language" at the top). Move with the D-pad: the title
      changes language as the cursor moves ("Choisis ta langue" on Français). A on Français: the menu comes up in
      French; `/data/rsos/settings.ini` has `language = fr`; the next boot goes straight to the menu.
- [ ] Settings > Langue (Language) (last item; UP from the top): pick Deutsch, then 日本語: the menu, the carousel
      names and the help bar change at once, no restart; back to Français. `frontend.log`: `ui: language ja (N
      translations) in X ms`: send X (the whole relayout; expected well under 0.5 s on the A20).
- [ ] 日本語 / 简体中文 / 한국어: the text is readable (no `?` boxes), lines wrap nicely in dialogs.
- [ ] In a game (French): Select+X: the menu is in French, rows are not cut in a way that hides the meaning, the
      slot date is `27/09/2026 14:03`; Select+R: the toast "Instantané enregistré (emplacement 0)" shows its accents.
- [ ] Sizes and dates: Settings > Informations système > Stockage shows "12,3 Go libres sur 29,1 Go".
- [ ] A core that reads the language: `game.log` has `core asked for the language: 2 (fr)` (in French); its own
      texts are in French where the core supports it.
- [ ] The on-screen keyboard (Settings > Réseau > Nom de la console, or a WiFi name): the "àé" key (or R) shows two
      pages of accented letters; SHIFT gives capitals; the WiFi password keyboard has no accent page.
- [ ] Boot time: `ui: created in ... us` and `ui: first frame in ...` in `frontend.log` stay within a few ms of the
      previous image (the catalog is one mmap; the CJK fonts are only opened when a CJK character is drawn).

### 7f. Batch 2 features (image `retrostoneos-dev-<date>-b2.img`)
- [ ] **Fast-forward**: in a NES/SNES game, Select+R2: `▶▶ x3` next to the battery, the game runs about 3x, no
      sound; Select+R2 again: normal speed and sound at once, no crackle or stutter in the next seconds. Do it on
      the LCD and on HDMI; Settings > Jeux > Vitesse de l'avance rapide 4x, then again. Send `game.log`
      (`fast-forward off at frame ...: ... (x3.0)` gives the real speed; with an N64 game it may stay below x3).
- [ ] **Screenshot**: Select+L2 in a NES game, a PS1 game and an N64 game (GLES): toast "Capture d'écran
      enregistrée"; the files in `/data/screenshots/<system>/` (web transfer or USB): the picture is right side up,
      NES/GB doubled (512x480 / 320x288), N64 at its size.
- [ ] **Game switcher**: play 3 games (quit each); in a 4th, Select+Y: "Jeux récents", the current game first with
      its live picture, the others with their last picture. B: back to the game (sound and speed normal). Select+Y,
      pick another: it starts where it was left; the first one resumes where the switch left it when launched again
      (or via the switcher). Send `frontend.log` (`switcher: ... -> ...`).
- [ ] **Resume on boot**: power off in a game; boot: 4 buttons (Reprendre / Recommencer / Toujours reprendre / Ne
      plus demander). Toujours reprendre: the game resumes, the toast about Settings > Jeux; next power-off in a game
      and boot: straight into the game (no dialog, after the menu's first frame). Set Never: boot to the menu,
      the launch prompt still offers to resume.
- [ ] **Lists**: in a big list (NES), L1/R1 jump by letter with the big letter shown; SELECT > Rechercher dans
      cette liste: the list follows each letter; START keeps it, B clears it. Settings > Rechercher dans tous les
      jeux. Play time: after 10 min of play the detailed view shows "Temps 10 min"; Settings > Listes de jeux >
      Trier les jeux par: Les plus joués.
- [ ] **Hide / delete**: hide a game (it goes, "Afficher les jeux masqués" shows it again); delete a test ROM with
      its saves (the .srm and states are gone, the list updates).
- [ ] **Per-game settings**: game options > Profil du processeur: Performance → `game.log` `cpu profile:
      performance` and `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` = performance during the game;
      Économie de batterie → schedutil, `scaling_max_freq` 720000 (TODO(hw): check the A20's OPP table has it).
      Taille de l'image: Entier → the game is integer-scaled; changed in Select+X it is kept at the next launch.
- [ ] **Rumble** (a pad with motors: Xbox / PS / 8BitDo): a game with rumble (N64 with a Rumble Pak option, PS1
      DualShock games): it vibrates; Settings > Manettes > Vibrations des manettes off: it does not. `game.log`/`frontend.log`:
      `input: <pad> (rumble)` when plugged.
- [ ] **New systems**: ROM folders `pc`/`dos`, `c64`, `zxspectrum`, `amstradcpc`, `scummvm`, `pico8`, `doom`,
      `cavestory`, `pokemini`, `supergrafx`: each in the carousel with its name and maker (e.g. "MS-DOS"), the default
      core starts. DOSBox: R2 opens the on-screen keyboard (dos.ini remap).
- [ ] **Windows file share**: Settings > Réseau > Partage de fichiers Windows on, then Transfert par le réseau: the screen shows
      `Windows : \\RETROSTONE (utilisateur retrostone)`. On Windows 11: `\\RETROSTONE` (or `\\<IP>`), user
      `retrostone`, password = the PIN: the `RetroStone` share lists `/data`; copy a ROM in. Stop the transfer: the
      share goes. Send `dmesg | grep -i ksmbd` and `frontend.log` (`Windows file share: ...`).

## 8. Networking (off by default)
```sh
rsos-net wifi on; rsos-net wifi status; iw dev wlan0 scan | grep SSID
rsos-net eth on;  ip addr
rsos-net bt on;   dmesg | grep -i -E "bluetooth|hci|bcm"   # which .hcd did btbcm ask for?
```

## 9. A/B boot state
```sh
fw_printenv | grep rsos_       # rsos_slot=a rsos_ok=1 ...
```

### 9b. System update (image `retrostoneos-dev-<date>-update.img`, docs/updates.md)

The test package `retrostoneos-0.1.1-dev-retrostone2.rsu` (next to the image in `images/`, signed with the update
key) is version **0.1.1-dev**, newer than the image's `0.1-dev`. On a freshly flashed card:

- [ ] Wait about a minute after the first menu (the new slot must be confirmed before any update:
  `rsos-boot-ok status` says `ok=1 fails=0`). Settings > **System update**: "Installed version 0.1-dev".
- [ ] **From a USB stick**: copy the `.rsu` into a folder `RetroStoneOS` (or the root) of a FAT32/exFAT stick, plug
  it in: the dialog has **INSTALL UPDATE**; choose it: the notes of 0.1.1-dev show. **A** (update): progress
  "Checking the update" (reads the 90 MB from the stick), "Installing" (%), "Checking the installation"; time it
  (TODO(hw): expected 2-4 min). Then "RetroStoneOS 0.1.1-dev is installed. Restart now?": **RESTART NOW**.
- [ ] After the restart: "Updated to RetroStoneOS 0.1.1-dev." once; Settings > System information and System update
  say 0.1.1-dev. On the UART: `rsos-update status` (slot b, `rsos_ok=0` until confirmed, then 1 about a minute after
  the menu), `cat /proc/cmdline` (`root=/dev/mmcblk0p3 rsos.slot=b`). A second restart: still 0.1.1-dev, no message.
- [ ] **Power cut during an install**: flash again, start the install from the stick, and pull the power (or hold the
  power key) in the middle of "Installing". The console starts **0.1-dev** as before (`rsos-update status`: slot a);
  the install can be started again and completes.
- [ ] **Stop**: B during "Installing" -> STOP: "Update stopped: nothing was changed", still 0.1-dev.
- [ ] **Battery**: below 30 % without the charger, the install is refused with a message.
- [ ] **From the SD card**: copy the `.rsu` to `RETROSTONE/update/` on a PC; Settings > System update > Check for
  updates (WiFi off): the notes show.
- [ ] **Online** (WiFi on): Check for updates contacts GitHub; with no release published yet it says "No update found
  for this console" (or "Up to date"). `rsos-update check` on the UART prints the details; a wrong date gives the
  "clock is not set" message (`date -s 2020-01-01` to try, then `rsos-clock` / NTP fixes it).
- [ ] Send `RETROSTONE/rsos/logs/update.log` (every run of the updater), `rsos-update status`, and the times.

## 10. Boot time (black screen before the logo)
Image `retrostoneos-dev-20260927c` and later time the whole boot, from power-on (the SoC's reset, after the power key
has been held for the AXP209 power-on time) to the menu. U-Boot is **silent** on the UART now (it saves ~0.1 s);
the SPL's few lines still print, then nothing until the kernel's warnings. For a full U-Boot log:
`fw_setenv rsos_verbose 1; fw_setenv silent` and reboot (`fw_setenv rsos_verbose` to go back).

- [ ] Boot normally (the second boot after flashing or later), wait until the menu has been up for 3 s, then send back
  from **RETROSTONE → `rsos/logs/boot<N>/`** (`last.txt` gives N):
  - **`bootstage.txt`**: the time line in ms since power-on and the summary. The lines that confirm the gains:
    `zImage read (... bytes, ... MB/s)` (the LZ4 zImage is 5.48 MB, 3% more than the old gzip one),
    `zImage decompression + kernel head` (the big one: expected well under 0.2 s with LZ4, against an estimated
    0.4-0.7 s with gzip), `power-on -> kernel time 0 (all pre-kernel)`, `kernel: time 0 -> /sbin/init` (was 0.55 s),
    `/sbin/init + rcS -> frontend start` (was ~0.50 s: 0.55 -> 1.05) and **`BLACK SCREEN: power-on -> logo`**.
    If it says "No rsos-cntvct line", the U-Boot and kernel parts are not on one time line: send it anyway.
  - `rcS.times` (`sysfs + governor` is new: it splits the old `mounts` step), `frontend.log`, `dmesg.txt`, `info.txt`.
- [ ] Also send a stopwatch figure: power key pressed -> logo, and power key released -> logo.
- [ ] Once, a boot with the kernel's per-driver timing: `fw_setenv rsos_extraargs "initcall_debug log_buf_len=1M"`,
  reboot, wait for the menu + 3 s, send `boot<N>/initcalls.txt` (the slowest initcalls and probes, and which drivers
  had to be retried) with its `dmesg.txt`, then `fw_setenv rsos_extraargs` (empty) to go back: initcall_debug itself
  slows the boot a little.
- [ ] **Shutdown** (goal: under 1 s from the power key to the power cut, docs/build.md "Shutdown"): power off from
  the menu a few times, stopwatch key press -> screen/LED off, then send `rsos/logs/shutdown.txt` (the step times of
  the last 5 shutdowns). The next boot must not show a dirty-volume message for `/data` in `dmesg.txt`.

## 11. Review fixes (2026-09-29, image `retrostoneos-dev-20260929-review.img`)
The fixes of the broad review (boot recovery, watchdog, data partition, updater, saves/resume, menu/power, SMB and web
share, USB), tested together. Send `rsos/logs/boot<N>/` (`frontend.log`, `dmesg.txt`, `rcS.times`), `game.log` and the
UART log for each failure.

Boot and recovery:
- [ ] **U-Boot AXP209 power-off really cuts power** (boot.cmd's failure budget): on the UART, `fw_setenv rsos_maxfails
  1; fw_setenv rsos_maxrounds 1; fw_setenv rsos_fails 1`, then `reboot`: the slot counts as used up, the budget is
  spent and U-Boot powers off. The LED and the screen go off and the unit **stays off** (no reboot loop; still off an
  hour later). The next power-on boots normally (a fresh budget): `fw_setenv rsos_maxfails; fw_setenv
  rsos_maxrounds` to go back. Also **after the bootcmd**: on a test card without `/boot/boot.scr` in both root
  partitions (and nothing for `bootflow scan`), the last command of `bootcmd`, `poweroff`, cuts the power the same way.
- [ ] **A slot that failed its update trial is never booted again**: install an update (9b), make its trial fail
  (pull the power during its first 3 boots, before the menu has been up 30 s): the old slot boots and
  `fw_printenv rsos_bad` names the updated slot. Then make the good slot fail 3 times the same way: it is retried
  (or the unit powers off when the budget is spent), but the `rsos_bad` slot is **never** booted (UART log,
  `fw_printenv` before and after).
- [ ] **Crash loop**: make the menu crash repeatedly (`killall -SEGV rsos-frontend`, 5 times
  within a minute): frontend-respawn stops restarting it and the watchdog resets the unit about **20 s** later
  (`dmesg` of the next boot / `/run/rsos` reason); `touch /run/rsos/respawn-unlimited` disables it for development.
- [ ] **rcS hang**: on a test card, add `sleep 600` near the top of `/etc/init.d/rcS` (remount the root read-write
  first): the unit resets after about **120 s** (`CONFIG_WATCHDOG_OPEN_TIMEOUT=120`), and that boot is counted as
  failed.
- [ ] **First-boot conversion on a slow card** (a large, slow or old card, 64 GB or more): the first boot converts
  the data partition to exFAT without a watchdog reset (it may take longer than 120 s: the conversion feeds the
  watchdog); the menu comes up, `rsos/logs/boot1/` has the conversion times.
- [ ] **rcK**: power off (power key or menu) during a game: the next start offers Resume and the in-game save is
  there; if a shutdown step hangs, the deadline fires and the unit still powers off (`rsos/logs/shutdown.txt`).
- [ ] **No usable data partition** (delete or corrupt partition 1 on a PC): the console shows the menu area on tmpfs
  with the "The storage could not be read" screen; **Format** works and the console restarts into a normal menu.
- [ ] **cpu-lowvolt is skipped after a freeze**: `fw_setenv rsos_overlays cpu-lowvolt`, reboot; if it freezes (or
  simulate it: pull the power before the menu has been up 30 s, while `fw_printenv rsos_lowvolt` says `trial`), the
  next boot skips the overlay and `fw_printenv rsos_lowvolt` says `failed` (docs/build.md, overlays);
  `fw_setenv rsos_lowvolt; fw_setenv rsos_overlays` to go back.
- [ ] **RetroStone1** (if available): after a power-off it stays off (PL8 held low); no restart after a few seconds.
- [ ] **Screen-off with board.ini missing** (rename `/etc/rsos/board.ini` on a test copy): idle screen-off then a
  button: the panel keeps scanning (no white/grey screen, no stuck image), the menu comes back.
- [ ] **No-display power-off**: on a panel board with the panel disconnected, the unit powers itself off after
  **60 s** (the log says why).

Saves, resume, benchmark:
- [ ] **Battery-save game, Resume keeps a newer in-game save**: play a game with a battery save (e.g. a Pokemon or
  Zelda cartridge), exit; choose **Start fresh**, save in-game, exit; then **Resume**: the in-game save survives.
  `game.log` says the save "is newer than the auto state", and a `.srm.bak` exists next to the `.srm`.
- [ ] **N64 benchmark with the shortest idle power-off** (Settings > Power, 5 min): the benchmark finishes (it holds
  the idle power-off); pressing the power key mid-benchmark records `resume.ini` (`rsos/resume.ini`).
- [ ] **A core without save states** (`savestates = false` in its `/usr/share/rsos/cores/<id>.ini`) holds the idle
  power-off for the whole game (`frontend.log`: "the game's core cannot save a state").

Network and USB:
- [ ] **SMB**: Windows 10 and 11 connect to `\\RETROSTONE` with user `retrostone` and the `XXXX-XXXX` password shown on
  the screen, and see **5 folders**; a 9th client is refused; after `killall rsos-frontend`, `ksmbd` is gone once
  the menu has respawned; **STOP** pressed while the share starts leaves it stopped; the password never appears in
  `frontend.log`, `dmesg.txt` or the other logs (`grep -r` the logs folder for it).
- [ ] **Web share**: upload files with Polish (`Zażółć gęślą jaźń.nes`) and CJK (`ゼルダの伝説.nes`, `塞尔达.sfc`) names:
  they arrive with the right names.
- [ ] **USB**: an **ntfs3** stick and a failing stick (bad sectors, or pulled during the scan) mount or are refused
  without a watchdog reset.
- [ ] **Update check with DNS down** (WiFi on, router without internet or a wrong DNS): Check for updates fails with
  a message and the unit is never reset.

## What to send back
The UART log from power-on to the prompt, the output of `rsos-kmstest --list`, `dmesg`, and a ✓/✗ for each checkbox,
plus a photo if the display looks wrong. For boot time: section 10 (`bootstage.txt`, `rcS.times`, `frontend.log`,
and once `initcalls.txt`).
