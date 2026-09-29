# Integration checklist (lead)

What's left to assemble the modules into the `rsos-frontend` binary and the image. Items are ticked when done.

## Frontend assembly
- [x] `frontend/Makefile`: include `ui.mk`, `host.mk`, `transfer.mk` and `power.mk`, and build one `rsos-frontend` binary plus the tools
      (`rsos-kmstest`, `rsos-display-selftest`, `rsos-run`, `rsos-uipreview` (host only, `make uipreview`), `rsos-bootreason`, `rsos-clock`).
      Build dir outside the tree (`BUILDDIR`, default `~/rsos/frontend-build`); every object gets the caller's CPPFLAGS/CFLAGS.
      `make check` runs the display self-test, host unit + launch tests, power tests, transfer tests, a UI preview tour and
      `check-frontend` (the real main loop headless, see below). Zero warnings on host gcc 13 and the Buildroot ARM gcc 14 (2026-09-26).
- [x] `frontend/src/main.c`: display, input, ui, host (fork per game), power, transfer and the display's uevents in one `poll()`
      loop (plus a signal eventfd); timeout = min of the modules' timeouts. Boot order: display → input (the UI binds it at
      creation) → UI → first frame → power → USB. The web share is never started at boot.
- [x] Display: `display_set_active(bool)`, `display_suspend/resume`, `display_present_fb` (in display.c; main.c uses suspend/resume
      around each game and set_active for sleep/idle-off).
- [x] Display and host: the GBM/dma-buf present path for HW-render (N64) (host owner, `display_present_fb`; TODO(hw): lima test).
- [x] Input API used by both UI and host (the host uses `src/input`; rumble waits for an FF API).
- [x] Core options path: `/data/rsos/coreopts/` everywhere (requirements.md fixed).
- [x] main(): the first line dispatches `--run` to `host_main()` (host-design.md §2). Launch flow: the UI's core (else host_pick_core)
      → host_check_game (message shown on failure) → input game mode + remap → power_set_game_running → display_suspend →
      host_launch(idle: power_poll, SIGTERM/power-off forwarding, Select+Start 5 s kill) → display_resume (re-open on failure) →
      input drained, UI mode → the child's message/crash note shown by the UI. The child gets `--scale` (menu "Game scaling")
      and `--hdmi` (menu "HDMI resolution").
- [x] The frontend calls `/usr/bin/rsos-boot-ok` once the menu is up (A/B boot confirmation; double fork, non-blocking).
- [x] `package/rsos-frontend/rsos-frontend.mk`: install every binary, `/usr/share/rsos/{fonts,themes,remaps,coreopts,web}`,
      gamecontrollerdb.txt, data-README.txt, `/etc/rsos-version`; depends on libdrm, alsa-lib, and the EGL/GLES/GBM headers
      (mesa3d, dlopen()ed at run time: the binary links only libdrm, libasound, libm, libc; verified with readelf).
- [x] `post-build.sh` then adds the inittab respawn line automatically when `/usr/bin/rsos-frontend` exists (checked in rootfs.ext2).
- [x] Image: `images/retrostoneos-dev-20260926.img` (900 MiB then; the A/B image is now 1.16 GB), boots straight into
      the menu (first boot on a unit: done, bringup.md).

- [x] Boot logo (2026-09-26): `frontend/assets/splash/retrostone2-logo.png` → build-time `mksplash` (host compiler) →
      `/usr/share/rsos/splash.rle` (640x480, RLE, 11 114 bytes, background `SPLASH_BG` = `2a2a35` dark slate,
      Buildroot option `BR2_PACKAGE_RSOS_FRONTEND_SPLASH_BG`). The frontend draws it right after `display_init()`,
      before input/UI (not in charge mode; `boot_logo_min_ms`, default 0, adds no delay). `rsos-frontend --splash
      --message TEXT` is the first-boot "Preparing the SD card" screen, started by `data-partition` only around the
      grow/format and the backup conversion (data-partition test: 27/27; a variant with a fake splash shows start/stop
      exactly around those steps and never on a normal boot). Previews: `docs/ui-previews/splash-*.png`.
      Decode: 20 µs (640x480) / 35 µs (854x480) on the build host; A7 estimate 2-5 ms including the first touch of
      the buffer. TODO(hw): the `splash: ... shown N ms after start` log line on a unit.

### Test evidence (2026-09-26, WSL)
- `make check` (host): all module tests pass; `check-frontend` runs `rsos-frontend --headless --root <tree> --script ...` three
  times: (1) menu → game list → launch (real fork + exec of `rsos-frontend --run --headless` with the host's test core) → back,
  play count recorded in gamedb.tsv → fake HDMI hotplug (854x480 relayout) → LCD → sleep/wake (screen off/on) → power off
  through the power module; (2) the core segfaults: the UI shows the crash message, then SIGTERM → clean exit; (3) charge
  mode (bootreason `charger`, charger unplugged): charge screen, power-off after 2 s.
- The ARM binary runs under `qemu-arm -L ~/rsos/output/target` (`--help`, `--run --help`, `--version`, and the same headless
  menu script with the image's real core .ini files, themes and fonts).
- Not testable off-hardware: DRM/KMS, evdev, the power key, ALSA, a real game launch on ARM (qemu-user cannot re-exec
  `/proc/self/exe`).

### Gaps found at that point (all closed since)
- [x] Host: the game child applies `cz_buttons` (the `-cz` remap lookup) and the `p1` policy from settings.ini
      (2026-09-26 01:12, below).
- [x] UI: the "Resume where you left off?" prompt (`host_has_resume_state()`), and later the "Resume on boot"
      setting (below).
- [x] The UI has a public toast API, `ui_toast()` (below), for notes such as the host's `warn` status lines.
- [x] build.md no longer says rsos-frontend installs "for now the display tools".

## Build and board
- [x] 11 more GPL cores in the defconfig (22 cores in the image, verified 2026-09-26).
- [x] rcS: `rsos-bootreason`, `rsos-clock restore`. rcK: `rsos-clock save`. rsos-net: skips apply in charge mode. udhcpc NTP hook.
- [x] `CONFIG_CPU_FREQ_GOV_POWERSAVE=y`.
- [x] Kernel: HID drivers (sony, playstation, nintendo, microsoft, logitech, steam, dragonrise, greenasia, smartjoy) with FF, UHID,
      USB storage + UAS + ntfs3, all built in (no udev). Verified in .config on 2026-09-26.
- [x] ksmbd module + ksmbd-tools (not started at boot), loop, e2fsprogs.
- [x] Bluetooth pairings persist in the ext4 loop image /data/rsos/bluetooth.img.
- [x] Partition layout: data = MBR entry 1, a 128 MiB FAT32 seed prefilled; first boot grows it to exFAT power-cut-safely (27/27 simulated checks). Windows 11 (2026-09-26): after flashing, only RETROSTONE got a drive letter; the ext4 roots (type 0x83) got none, so no format prompt.
- [x] Experimental cores supafaust, clownmdemu, geolith in the dev defconfig (25 cores in the image, 2026-09-26).
- [x] `quiet` on the kernel cmdline (−1.4 s measured on hardware); i2c1/EEPROM disabled (a stuck bus: −4.2 s).
- [ ] Production: drop the bring-up tools from the release image.
- [ ] LCD 60 Hz: the hardware logs show the panel at 78.57 Hz. Test the lcd60 overlay (25.2 MHz), ideally as a live
      "Display > LCD refresh" trial with an automatic revert (a custom KMS mode on the DPI connector, no reboot).
- [ ] Boot time: measure U-Boot → kernel → menu on hardware after the i2c/quiet fixes (target: menu in ~3 s).

## 2026-09-27: boot speed (menu from a snapshot, exFAT folder warm-up)
Hardware 2026-09-26c (normal boot): rcS 0.95 s, then "5 systems, 11 games loaded in 3881566 us" behind the
"Preparing" screen, then a 551 ms first menu frame. Root cause of the 3.9 s: the kernel exFAT driver reads a cold
directory's first cluster as 64 single-sector requests (`exfat_dir_readahead()`, one `sb_breadahead()` per sector),
triggered even by a `stat()`; ~1.6 ms per request on the A20 SD host = ~110 ms per ROM folder. Reproduced on the build
host with the same kernel: the old normal boot = 2403 read requests (≈ 3.9 s at 1.6 ms), now ~100 (~20 before the
menu). Details: ui-design.md §4.1.
- [x] `ui/fswarm.c`: warms the exFAT folders the UI is about to use with a few large reads of the block device
      (34 ROM folders: 2240 requests → 10). Used in `ui_create()`, for the image cache folder, the carousel's ROM
      folders and by the loader.
- [x] `ui/loader.c`: carousel snapshot `/data/rsos/cache/systems.idx` → the real menu at the first frame, no
      "Preparing" screen; one worker thread validates every list after the first frame (selected system first),
      results installed by the main thread; carousel rebuilt keeping the selection when something changed; on-demand
      loading when a system is opened early; paused during games. First boot keeps the loading screen (now ~85
      requests instead of ~2100).
- [x] Backdrop cache files run-length coded (rsos-dark: 69 KB instead of 1.2 MB read by the first menu frame).
- [x] Instrumentation in frontend.log: per-system load split, warm-up and block-device read counts per phase, first
      menu frame breakdown (themes, image cache hits, reads), frame timings (buffer wait / render / present), slow
      input device opens, "menu up N ms after start", "game lists complete N ms after start".
- [x] Input: devices never used (axp20x-pek when the power module owns it, audio jacks) are skipped by their sysfs
      name instead of being opened (TODO(hw): the 138 ms between two input log lines in boot3; the new
      `input: opening eventN took` line names the device if it is still slow).
- [x] rcS: `/run/rsos/rcS.times` per step; boots at the "performance" governor (the power module sets schedutil at
      its init; main.c does it if the power module fails); hostname, lo, rsos-net apply-early, bootlog in the
      background; the mixer after the menu is up (`/run/rsos/menu-up`, written by the frontend). Only bootreason,
      data-partition and the clock restore remain before the frontend.
- [x] bootlog: waits for the menu before its first write; copies `game.log` and `rcS.times` too; every 2 s for 5 min,
      then every 30 s up to 60 min.
- [x] Game child log: `/run/rsos/game.log` on the device (appended, a `=== [uptime] launch:` header per game,
      restarted above 1 MiB); `host_launch_opts.log_append`.
- [x] Tests: `make check-loader` (34-folder library: first boot, snapshot boot with the menu before the lists and a
      final state equal to a full load, library changed while off, on-demand opening, RLE cache files; also clean
      under ThreadSanitizer and ASan/UBSan), `check-frontend` step 1a (snapshot boot headless: menu before the lists,
      same digest and same pixels as a full load), game.log header.
- [ ] **Kernel (recommended, other owner)**: the proper fix of the exFAT cost, a block plug around the readahead loop
      in `fs/exfat/dir.c` `exfat_dir_readahead()` so the 64 requests merge into one:
      `struct blk_plug plug; blk_start_plug(&plug); for (...) sb_breadahead(...); blk_finish_plug(&plug);`
      (upstream candidate). It speeds up every cold folder (user folders, media folders, the transfer module),
      not only the ones fswarm knows about. fswarm stays harmless after it.
- [ ] inittab (other owner): `::once:/bin/nice -n 10 /usr/bin/rsos-net apply` runs during the menu's boot (dmesg: the
      Ethernet driver is probed and removed at ~3.6 s). Deferring it until `/run/rsos/menu-up` exists (or having the
      frontend start it after the first frame) would keep module loading off the boot path.
- [ ] Frontend before /data: starting the frontend from rcS before `data-partition` (display, splash, input in
      parallel with the mount) would save at most the overlap (~0.2 s) but needs the inittab respawn line changed
      and a hand-off with the first-boot `--splash` (DRM master). Decide after `rcS.times` from a unit: worth it
      only if `data-partition` takes > ~0.2 s on a normal boot.
- [ ] host-design.md §2 still says the child's stderr goes to `/tmp/rsos-game.log` (now `/run/rsos/game.log` on
      the device).
- [ ] TODO(hw): the next normal boot's frontend.log/rcS.times (see bringup.md, "Normal boot").

## 2026-09-26 01:12: state at hand-off to hardware testing
- [x] UI resume prompt ("Resume where you left off?") + public `ui_toast()`; main.c wires `cb.has_resume` → `host_has_resume_state`, `o.resume = req->resume`.
- [x] Game child honours `p1` and `cz_buttons` from settings.ini.
- [x] Full verification: host build 0 warnings, `make check` passes, Buildroot `rsos-frontend-rebuild all` passes with 0 warnings.
      Image: `images/retrostoneos-dev-20260926.img` (900 MiB then; 1.16 GB since the A/B layout).
- [ ] Next: the hardware bring-up per docs/bringup.md. Then fix what the device reveals (display timing, 60 Hz, HDMI CRTC, audio levels, battery curve…).

## Batch 2 (after the updater): features decided with the owner
Done 2026-09-27 (host build 0 warnings; `make check` incl. the new `check-b2`, `check-asan`; details in
ui-design.md §7-§9/§12/§16, host-design.md §1/§10/§11.1/§14/§15, input-design.md §2/§4, rom-transfer.md §3.3;
hardware steps in bringup.md 7f; previews `docs/ui-previews/b2-fr-*.png`):
- [x] Hotkeys (approved): Select+R2 = fast-forward (toggle; `▶▶ x3` on the overlay, 2x-4x setting `ff_speed`, muted),
      Select+L2 = screenshot (to /data/screenshots/<system>/<game>-<date>.png, 2x for small pictures, GLES read back),
      Select+Y = game switcher (recent games with thumbnails; exit code 6 + `ui_switch_to()`).
- [x] **Resume on boot** setting (owner 2026-09-28): Settings > Games > "Resume on boot: Always / Ask / Never" (key `resume_boot`, default `ask`).
  Always = relaunch the game directly with `--load-state auto` at boot, no dialog. Ask = the boot dialog gets 4 choices:
  Resume / Start fresh / Always resume / Never ask (the last two apply now AND save the setting). Never = boot to the menu; the auto
  state is kept, so the per-game launch prompt still offers it. Separate from `resume_mode` (the per-game launch prompt).
- [x] Jump-to-letter (L1/R1) and search (game options, live; Settings > Search all games) in game lists; play time per
      game (gamedb column 7, detailed view, "Most played" sort); hide/delete a game (Show hidden games; the saves
      question); per-game scaling/CPU settings (game options and the in-game menu); USB pad rumble (evdev FF_RUMBLE,
      Settings > Controls > Controller vibration).
- [x] New systems (cores batch 3): proper names, makers and carousel order in ui/systems.c, host/coreinfo.c and transfer/sysmap.c; a dos.ini remap (r2 = l3, for DOSBox's on-screen keyboard) + dos-cz.ini.
- [x] Settings > Network: "Windows file share" (ksmbd via `/usr/bin/rsos-smb`, user retrostone + the transfer PIN,
      signing mandatory, share /data). Verified with stubs and the image's ksmbd-tools under qemu-arm; TODO(hw) with
      Windows (the WSL kernel has no ksmbd).
- [ ] TODO(hw): bringup.md 7f (fast-forward speed/sound on the A20, GLES screenshot orientation, the powersave cap
      720 MHz vs the A20's OPP table, rumble on real pads, the Windows share).
