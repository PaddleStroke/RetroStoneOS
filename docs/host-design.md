# libretro host design

The libretro host is the part of the frontend that runs one game: it `dlopen()`s a libretro core, loads the content,
and drives the core with the display layer (`display.h`), ALSA and the input layer. It replaces RetroArch.

| Path | Contents |
|---|---|
| `frontend/src/host/host.h` | public API: `host_main()` (the `--run` child mode and `rsos-run`), `host_launch()` (UI side), `host_check_game()`, `host_pick_core()`, `host_has_resume_state()`, `host_auto_state_path()` |
| `src/host/host.c` | the game process: set-up, main loop, pacing, hotkeys, shutdown, signals, watchdog |
| `src/host/core.c` | `dlopen`, the environment callback, the video/audio/input callbacks |
| `src/host/options.c` | core options (v0/v1/v2), layering, per-game/per-system files |
| `src/host/saves.c` | SRAM/RTC, save states, thumbnails, the save worker thread |
| `src/host/content.c` | zip extraction, content in memory, `GET_GAME_INFO_EXT`, the resume helpers |
| `src/host/coreinfo.c` | core `.ini` metadata, BIOS check, `system_files`/`system_tree`, core choice |
| `src/host/pacing.c` | pacing policy and DRC (pure maths, unit-tested) |
| `src/host/menu.c`, `osd.c`, `draw.c` | in-game menu, toasts (and the FPS overlay when the plane is refused), the drawing primitives |
| `src/host/perf.{h,c}` | performance counters: speed, fps, frame times, per-CPU load, GLES timing (section 8) |
| `src/host/bench.{h,c}`, `bench_run.c` | the benchmark: plans, results, ranking, report, "Use this"; the driver process (section 18) |
| `src/host/batt_overlay.{h,c}` | the in-game battery indicator picture (section 8) |
| `src/host/hwrender.c` | GLES2 HW rendering (EGL on GBM, lima), loaded only for HW-render cores; Mesa's shader cache (13.4) |
| `src/host/glprobe.{h,c}`, `glprobe.syms` | the GL stall probe: exported timed wrappers of the compile/link/draw/upload entry points (13.4) |
| `src/host/launch.c`, `cli.c`, `sys.c` | fork+exec of the game process, the command line, governor/battery/power-off |
| `src/host/host_input*.{h,c}` | the input interface; `host_input_rsos.c` wraps `src/input/input.h` |
| `src/audio/audio.{h,c}`, `resampler.{h,c}` | ALSA output and the resampler |
| `src/tools/run.c` | `rsos-run` |
| `src/host/coreopts/*.ini` | RetroStone default core options (installed to `/usr/share/rsos/coreopts/`) |
| `src/host/tests/` | unit tests, the process-model test and its test core, the test ROM generator |
| `frontend/host.mk` | make fragment (included by `frontend/Makefile`, or standalone with `make -f host.mk`) |

## 1. Process model

**Decision: every game runs in its own process** (fork + exec of the frontend binary with `--run`, or of `rsos-run`).

Why:
- A core that crashes (SIGSEGV in a dynarec, a bad dump) or hangs cannot take the menu down. The UI shows a message
  and nothing is lost except SRAM newer than the last periodic flush (at most about 2 s, see section 7).
- Many cores keep static state and do not survive a second `retro_load_game()` or a `dlclose()`/`dlopen()` cycle in
  one process (a known RetroArch problem; MinUI re-executes its host for every game). A fresh process per game has
  none of that, and fbneo's 59 MB or the PS1 memory is returned to the system at exit.
- The dynarecs, the GL driver (lima) and the cores' threads never exist in the UI process, so the menu's memory and
  boot path stay small. EGL is never initialised in the UI.
- Cost: one fork + exec + `dlopen` (a few ms, plus the core's own relocations: TEXTREL cores take longer), and the
  display re-initialisation in the child (no modeset if the mode is unchanged: the atomic commit keeps the CRTC).

Exec rather than a bare fork: the child starts clean (no inherited UI heap, fonts, image cache, DRM buffers or fds),
and `/proc/self/exe --run` keeps a single binary. `rsos-run` is the same code as a stand-alone tool.

### Sequence

```
UI (supervisor)                                        game process
---------------                                        ------------
power_set_game_running(true)
display_suspend()       -> drmDropMaster; fd, buffers and state kept
(evdev fds stay open without EVIOCGRAB: drained after the game)
host_launch(core, rom, system, &opts, &res)
   pipe (status), fork, exec  ---------------------->  host_main(): parse args
   poll(status pipe, 100 ms) + opts.idle()             BIOS check (exit 2 + message if missing)
     idle: power_poll(), hang kill switch              dlopen, retro_get_system_info, content, options
   waitpid                                             display_init() (DRM master), input, ALSA
                                                       retro_init, retro_load_game, SRAM load
                                                       main loop
                                                       exit: SRAM flush, auto state, unload, deinit
   <---------------------- exit code + status lines   _exit
res.status / res.message / res.warning
display_resume()        -> drmSetMaster, hotplug re-evaluated, full re-commit
drain input, redraw
power_set_game_running(false)
```

**DRM master hand-off.** `display_suspend()` before `host_launch()` (waits for the in-flight flip, drops master,
keeps the fd, buffers and state) and `display_resume()` after `waitpid()` (takes master back, re-evaluates the outputs
from the kernel's cached connector state, so a hotplug during the game switches output here, and re-commits the full
state with the newest frame; display-design.md 8.2). The child's `display_init()` becomes master because the parent
no longer holds it. If the child dies in any way, the kernel closes its fd, so master is always free again for
`display_resume()`. `display_shutdown()` + `display_init()` around the launch still work (slower: device open and
connector probe with EDID reads).

### Status pipe and exit codes

The child writes lines on `--status-fd N` (fd 3 from `host_launch()`): `error <message>`, `warn <message>`,
`running <game>`, `autostate <bytes> <ms>` (the `.state.auto` was written at exit: `res.auto_state_saved`, section
7.1), `poweroff`; batch 2: `playtime <s>` (the total play time of this run, every `--playtime-report` s (300) and
at exit: `res.playtime_s`), `setting scale|cpu <value>` (changed in the in-game menu, to be kept for this game),
`switch <n>` (the game switcher's choice: `res.switch_to`). `host_launch_opts.on_status(kind, arg, user)` gets
each line as it comes (main.c saves the play time as it grows). Exit codes (`enum host_exit`):

| Code | Meaning | UI action |
|---|---|---|
| 0 `HOST_EXIT_OK` | user quit | back to the list |
| 2 `HOST_EXIT_ERROR` | could not start (BIOS, bad file, core refused) | show `res.message` ("PS1 needs scph5501.bin in /bios") |
| 3 `HOST_EXIT_POWEROFF` | saves flushed after a power-off request | power off |
| 4 `HOST_EXIT_HANG` | the child's watchdog saw no progress for 15 s | "The emulator stopped responding..." |
| 6 `HOST_EXIT_SWITCH` | the game switcher (Select+Y) chose another game; this one was saved (auto state, always) | main.c reads the switcher file, `ui_switch_to()`: the chosen game is launched, resumed from its auto state |
| signal | crash | `res.crashed`, "The emulator crashed (Segmentation fault). Game saves up to a few seconds ago are kept." |
| 1, 127 | unexpected failure / exec failed | message |

The child's stderr goes to `opts.log_path`: `/run/rsos/game.log` under the UI (appended, one `=== launch` header
per game; copied to the SD card's `rsos/logs/` by the boot logger), `/tmp/rsos-game.log` by default.

### Signals (supervisor → game process)

| Signal | Child action |
|---|---|
| `SIGTERM`, `SIGINT`, `SIGHUP` | quit normally: SRAM flush, `.state.auto` if `autosave_exit` is on (section 7.1) |
| `SIGUSR1` | flush SRAM now, keep running |
| `RSOS_SIG_POWEROFF` = `SIGUSR2` | one "Powering off..." frame (OSD toast over the last frame), SRAM flush + `.state.auto` (always), exit 3 |
| `RSOS_SIG_SLEEP` = `SIGRTMIN+1` | (not sent any more: no sleep mode, power.md §6) SRAM flush (async), ALSA closed, `display_set_active(false)`, no more frames; wait (200 ms naps, watchdog muted) |
| `RSOS_SIG_WAKE` = `SIGRTMIN+2` | (not sent any more) `display_set_active(true)`, drain input (the waking key never reaches the game), reopen ALSA, reset pacing |

The values come from `src/power/power.h` when it exists. Under a supervisor (`--status-fd` given), the child takes no
battery decision and ignores the power key except for an immediate SRAM flush on the long press (the power module owns
both, docs/power.md §10). Stand-alone `rsos-run` keeps its own critical-battery check (≤ 3 %, TODO(hw)) and runs
`/sbin/poweroff` after the flush.

**Hang handling.** Two layers: the child's watchdog thread (`_exit(4)` after 15 s without a main-loop iteration,
without writing any save since memory may be corrupt); and `host_launch_opts.idle`, called every 100 ms, where the UI
can kill the child (e.g. Select+Start held 5 s read from its own non-grabbed evdev fds).

**CPU governor.** Under the UI the power module owns it: `host_launch()` passes `--no-governor` unless
`opts.manage_governor` is set (then the child switches to `performance` and `host_launch()` restores `schedutil` even
after a crash). Stand-alone `rsos-run` sets `performance` itself.

## 2. Integration (for the lead)

1. **Makefile**: `include host.mk` in `frontend/Makefile`, add `$(HOST_OBJS)` and `$(HOST_LIBS)` to the frontend
   binary, and `$(RSOS_RUN)` to `all`/`install` (or call `host-all` / `host-install`, which also installs
   `src/host/coreopts/*.ini` to `/usr/share/rsos/coreopts/`). `$(HOST_OBJS)` does not contain the input layer (the
   frontend has it already); `HOST_INPUT_EXTRA_OBJS` (default: `src/input/*.c` + `src/ui/util.c`) is linked into
   `rsos-run` only: adjust it if the input layer gains dependencies. `host.mk` builds the display library itself only
   when used standalone.
2. **main()**: `if (argc > 1 && !strcmp(argv[1], "--run")) return host_main(argc - 1, argv + 1);` as the very first
   thing, before any UI initialisation.
3. **Launching a game** (UI launch callback):
   ```c
   char core[256], msg[256];
   if (host_pick_core(system, rom, core, sizeof core) < 0) { /* offer coreinfo_candidates() */ }
   if (!host_check_game(core, rom, system, msg, sizeof msg)) { show(msg); return; }  /* no fork needed */
   bool resume = host_has_resume_state(core, rom, system) && ask("Resume where you left off?");
   /* (the UI asks through cb.has_resume, honouring resume_mode; section 7.1) */
   struct host_launch_opts o = { .idle = ui_idle /* power_poll(); hang kill */, .resume = resume };
   power_set_game_running(true); display_suspend();
   host_launch(core, rom, system, &o, &res);
   display_resume(); /* then drain input */ power_set_game_running(false);
   if (res.status == HOST_EXIT_POWEROFF) ...; else if (res.message[0]) show(res.message);
   ```
   Forward `SIGTERM` and the power module's signals to `host_child_pid()`.
4. **Buildroot** (`rsos-frontend.mk`): add `alsa-lib` to `RSOS_FRONTEND_DEPENDENCIES`. For N64, add
   `$(if $(BR2_PACKAGE_HAS_LIBEGL),libegl) $(if $(BR2_PACKAGE_HAS_LIBGLES),libgles)`: only the headers are used (EGL,
   GLES2 and GBM are `dlopen()`ed at run time, the binary does not link Mesa). Without them `hwrender.c` builds a stub
   and N64 games fail with "This build cannot run GLES games". `host.mk` needs `pkg-config alsa` (Buildroot's).
5. **Settings** read from `/data/rsos/settings.ini` by the game process (flat `key = value`; command-line options
   win): `p1` (`auto`/`builtin`/`external`, or `--p1`) and `cz_buttons` (`0`/`1`, or `--cz`), applied to the input
   layer before the remap lookup (`<system>-cz.ini` variant) and before the docked state is known; `game_scale` or the
   UI's `scaling` (`aspect`/`integer`/`stretch`, or `--scale`), `game_show_fps`, `autosave_exit` (`.state.auto`
   at every exit, default on, or `--autosave`/`--no-autosave`; the older `game_autosave` is read when it is absent;
   section 7.1), `audio_latency_ms`
   (64), `vsync_tolerance_permille` (10), `game_battery_overlay` (`on`/`off`, default on) and `game_battery_corner`
   (`top-right` default, `top-left`, `bottom-right`, `bottom-left`) for the in-game battery indicator (section 8);
   `lcd_refresh` (`60` = the retimed 25.2 MHz LCD mode, anything else = the panel's own 78.6 Hz mode; the UI passes
   the value in use as `--lcd-refresh 60|0`, which wins, so a game started during the 15 s trial runs like the menu,
   display-design.md §3.1). The child's display then commits the same mode as the menu (no modeset at launch) and
   pacing sees `refresh_mhz` = 60000 on the LCD, so ~60 Hz cores get vsync+DRC instead of the audio clock.
   **Player 1** (docs/input-design.md §3): with `p1 = auto` (default) the controller that launched the game is P1: the
   UI passes `input_last_source_id()` as `--p1-device <id>`, the child calls `hin_set_p1_device()` right after the
   policy, before the remap lookup.
6. **rcS** (not host code, suggestions): the codec level stays set by rcS (the host never touches the mixer: the
   analog wheel is the volume); consider raising the codec's DAPM `pmdown_time` so short pauses never power the DAC
   down (pops through the PAM8302).

## 3. The game process, step by step

1. Settings; identity: core id from the `.so` name, system = `--system` or the ROM's parent folder, BIOS dir.
2. Core `.ini` (`/usr/share/rsos/cores/<id>.ini`), **BIOS check** (section 6) → exit 2 with the message if a
   mandatory file is missing. `system_files` and `system_tree` are copied into `/data/bios` when missing.
   `renderer = gles2`: `EGL_PLATFORM=gbm` is exported (GLideN64 opens the default EGL display itself).
3. `dlopen` (`RTLD_NOW | RTLD_LOCAL`), the 21 mandatory symbols, `retro_get_system_info()`.
4. Content preparation (zip extraction) → the game name used for saves and per-game options.
5. Core options layers, the save worker, the input layer, `display_init()` (the output's refresh rate is known before
   the core asks for it). ALSA is opened only after the game is loaded (an open stream would underrun during a long
   load).
6. `retro_set_environment` + callbacks, `retro_init()`, content load (memory or path), `retro_load_game()`.
7. `retro_get_system_av_info()`; HW rendering: context + FBO, then `context_reset()`.
8. Game surface, ALSA, pacing mode, SRAM/RTC load, controller port devices, remap, optional resume state.
9. Watchdog; main loop.
10. Exit: SRAM/RTC flush (synchronous), `.state.auto` (power-off or autosave), ALSA fade-out and drain,
    `retro_unload_game()`, `context_destroy()`, `retro_deinit()`, `display_shutdown()`, then the EGL/GBM teardown
    (after the display released its BOs), save worker join, extracted files removed. No `dlclose()` (the process exits).

## 4. Environment callback coverage

List made by grepping the pinned sources in `~/rsos/cores-test/<core>` (without libretro-common) for all ten cores
of the first batch, including parallel-n64. The second-batch cores use a subset of the same calls.

| Command | Cores | Host |
|---|---|---|
| `GET_SYSTEM_DIRECTORY`, `GET_CORE_ASSETS_DIRECTORY` | all | `/data/bios` |
| `GET_SAVE_DIRECTORY` | snes9x2010, pcsx, mame, fbneo, n64 | `/data/saves/<system>` (created before `retro_init`) |
| `GET_LIBRETRO_PATH`, `GET_USERNAME`, `GET_LANGUAGE` | various | core path, "Player", the menu's language as `RETRO_LANGUAGE_*` (`i18n_retro_language()`: French → 2, Japanese → 1, Danish → English; logged once) |
| `SET_PIXEL_FORMAT` | all | RGB565, XRGB8888, 0RGB1555 (default); re-creates the surface if it changes after load |
| `SET_GEOMETRY` | fceumm, snes9x2010, picodrive, pcsx, mame, fbneo, n64 | `display_set_scaling()` with the new aspect; frame size per frame via `display_present_frame()` |
| `SET_SYSTEM_AV_INFO` | fceumm, picodrive, gambatte, gpsp, pcsx, fbneo, n64 | new surface if max size changed, pacing mode and resample ratio re-evaluated |
| `GET_CAN_DUPE` | snes9x2010, gambatte, pcsx, n64 | true; a NULL frame keeps the screen, and in vsync mode waits one vblank (`drmWaitVBlank`) |
| `GET_OVERSCAN` | - | true (show it) |
| `SET_ROTATION` | mame, fbneo | **false**: the planes cannot rotate, the cores rotate in software. TODO(hw): check a vertical shooter |
| `GET_TARGET_REFRESH_RATE` | fbneo | the rate the host really runs the core at (display Hz in vsync mode, else the core fps) |
| `GET_THROTTLE_STATE` | - | vsync / none / fast-forward, with the rate |
| `GET_AUDIO_VIDEO_ENABLE` | snes9x×2, fbneo, n64 | video off on fast-forward frames that are not shown |
| `GET_FASTFORWARDING` | - | true while fast-forward is on |
| `GET_CURRENT_SOFTWARE_FRAMEBUFFER` | fceumm, snes9x2010, pcsx, n64 | **false**: scanout buffers are write-combined (reads would crawl) |
| `SET_HW_RENDER` | parallel-n64, mupen64plus-next | GLES2 (and `OPENGLES_VERSION` 2.x) only; see section 13 |
| `GET_PREFERRED_HW_RENDER` | n64 | `RETRO_HW_CONTEXT_OPENGLES2` |
| `SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE`, `GET_HW_RENDER_INTERFACE` | n64, fceumm, picodrive | false |
| `GET_VARIABLE`, `SET_VARIABLES`, `GET_VARIABLE_UPDATE`, `SET_VARIABLE` | all | section 5 |
| `GET_CORE_OPTIONS_VERSION` | all | 2 |
| `SET_CORE_OPTIONS`, `_INTL`, `_V2`, `_V2_INTL` | all | parsed (English `us` set; v2 categories kept for the menu) |
| `SET_CORE_OPTIONS_DISPLAY`, `SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK` | fceumm, gambatte, pcsx, fbneo, n64 | hidden options are not listed; the callback runs when the options page opens and after each change |
| `SET_INPUT_DESCRIPTORS` | all | stored, shown on the Controls page |
| `SET_CONTROLLER_INFO` | fceumm, snes9x2010, pcsx, mame, fbneo, n64 | stored; a port with an analog source gets the core's DualShock/analog device, else `RETRO_DEVICE_JOYPAD` |
| `GET_INPUT_BITMASKS` | all | true (`JOYPAD_MASK` answered from the input layer's bitmask) |
| `GET_INPUT_DEVICE_CAPABILITIES`, `GET_INPUT_MAX_USERS` | - | joypad + analog, 4 |
| `GET_RUMBLE_INTERFACE` | gambatte, gpsp, pcsx, n64 | given; forwarded to `hin_rumble()` (no-op until input.h exposes FF) |
| `GET_LOG_INTERFACE` | all | host log, prefixed with the core id |
| `GET_PERF_INTERFACE` | snes9x2005, gpsp, mame, n64 | time, NEON flag, counters |
| `SET_MESSAGE`, `SET_MESSAGE_EXT`, `GET_MESSAGE_INTERFACE_VERSION` (1) | most | OSD toasts (LOG target: log only) |
| `SET_PERFORMANCE_LEVEL` | fceumm, snes9x2010, picodrive, gambatte, pcsx, mame | logged |
| `SET_SUPPORT_NO_GAME` | n64 | honoured (`--rom` optional) |
| `SET_DISK_CONTROL_INTERFACE`, `_EXT_INTERFACE`, `GET_DISK_CONTROL_INTERFACE_VERSION` (1) | picodrive, pcsx | disc swap in the in-game menu (eject, index, insert) |
| `SET_AUDIO_BUFFER_STATUS_CALLBACK` | snes9x×2, picodrive, gpsp, pcsx, mame, fbneo, parallel-n64 (patch 0001) | called before every `retro_run()`: active, ALSA fill 0-100, underrun likely below 25 % (the cores' auto frameskip; parallel-n64 skips below 40 %) |
| `GET_CLEAR_ALL_THREAD_WAITS_CB`, `POLL_TYPE_OVERRIDE` (RetroArch block `0x800000`) | mupen64plus-next (threaded GLideN64) | a no-op callback (our audio never blocks the core's thread), accepted; the core's audio buffer is locked (`core_audio_lock()`) |
| `SET_MINIMUM_AUDIO_LATENCY` | snes9x×2, picodrive, gpsp, pcsx, fbneo, n64 | raises the ALSA buffer (reopen) |
| `GET_TARGET_SAMPLE_RATE` | fceumm | 48000 (fceumm then outputs 48 kHz directly) |
| `SET_FRAME_TIME_CALLBACK` | - | called before each run with the measured delta |
| `GET_GAME_INFO_EXT` | fceumm, snes9x2010, picodrive | implemented (path, archive, data) |
| `SET_CONTENT_INFO_OVERRIDE` | fceumm, picodrive | per-extension `need_fullpath` honoured |
| `GET_SAVESTATE_CONTEXT` | snes9x2010, fbneo | NORMAL |
| `SET_SERIALIZATION_QUIRKS` | fbneo, n64 | stored; no serialize before the first frame when INCOMPLETE; size read each time |
| `GET_JIT_CAPABLE` | - | true |
| `GET_DEVICE_POWER` | - | battery from `/sys/class/power_supply` |
| `SHUTDOWN` | fbneo | quit like the exit hotkey |
| `SET_SUBSYSTEM_INFO`, `SET_MEMORY_MAPS`, `SET_SUPPORT_ACHIEVEMENTS`, `SET_PROC_ADDRESS_CALLBACK` | various | accepted, ignored |
| `GET_VFS_INTERFACE`, `GET_LED_INTERFACE`, `SET_KEYBOARD_CALLBACK`, `SET_AUDIO_CALLBACK`, `SET_FASTFORWARDING_OVERRIDE`, `SET_NETPACKET_INTERFACE`, `GET_MIDI/MICROPHONE/SENSOR/CAMERA/LOCATION_INTERFACE`, `GET_PLAYLIST_DIRECTORY`, `GET_FILE_BROWSER_START_DIRECTORY`, `SET_HW_SHARED_CONTEXT` | various | false, silently (cores fall back: stdio for VFS) |
| anything else | - | false, logged once (`environment command N not supported`) |

## 5. Core options

Effective value of a key (first found wins):

| Layer | File | Written by |
|---|---|---|
| 5 session override | (memory) | the benchmark's configuration for one run (`opts_set_override()`), never saved |
| 4 per game | `/data/rsos/coreopts/<core>/<game>.ini` | in-game menu (automatically, see below), "Use this for this game" (benchmark) |
| 3 per system (core) | `/data/rsos/coreopts/<core>.ini` | in-game menu, "Save for all games" |
| 2 RetroStone defaults | `/usr/share/rsos/coreopts/<core>.ini` | shipped (`src/host/coreopts/`) |
| 1 package defaults | `[options]` of `/usr/share/rsos/cores/<core>.ini` | the core package |
| 0 | the core's own default | |

Format: `key = "value"` (the RetroArch `.opt` syntax, so RetroArch option files can be copied; unquoted values are
accepted). A value that is not one of the option's allowed values is ignored (logged once) and the next layer
applies. User files only store the keys that differ from the layers below them, so shipped default changes still reach
users; keys of a user file that the core did not declare this time are kept. A key the core queries without declaring
it is answered from the files. Writes are atomic (section 7).

**Menu changes are saved automatically** (owner report, 2026-09-27: "changed the GFX plugin to gln64 in Select+X >
Core options, restarted, it was back to glide64"). Root cause: a value changed in the menu only lived in memory; it
reached a file only through the explicit "Save for this game" / "Save for all games" items, and Resume or Exit
dropped it without a word. Now a change is written to the per-game file when the options page is left, when the menu
closes (Resume, Exit game) and at game exit (`opts_autosave()`); "Save for all games" also rewrites the per-game file
as the difference from the new per-system values, so an older per-game value no longer beats it. Each value shows
where it comes from: ` *` changed and not written yet, ` (game)` this game's file, ` (all)` all games of the core,
nothing for the RetroStone/core defaults (` (bench)` during a benchmark run). Regression test: `rsos-launch-test`
drives the menu (`--menu-script`), exits, restarts and checks the value the core receives (section 15).

Keys the core does not declare are answered from the files too; the host's own keys start with `rsos-`
(`rsos-glthread`: export `mesa_glthread=true` before the EGL context, read right after `retro_init()`).

Shipped defaults (`src/host/coreopts/`): picodrive 6-button pad + dynarec; pcsx_rearmed dynarec on, NEON enhanced
resolution off, frameskip off (per game: `auto`, which works with the audio buffer status callback), BIOS logo off;
gpsp dynarec; parallel_n64 rice, accuracy low, HLE RSP, dynarec, 320x240, frameskip auto, Count Per Op from the ROM
database (the same as the package `[options]`; docs/cores.md "N64 performance"); mame2003_plus skips the disclaimer
and warnings. `n64.bench.ini` is the benchmark plan (section 18). Requirement doc note: `docs/requirements.md` 1.2 names the user dir
`/data/rsos/options/`; this implementation uses `/data/rsos/coreopts/` as the task specified.

## 6. Content, BIOS, core choice

- **BIOS check** before anything is loaded, from the core `.ini` `[bios:<path>]` sections (the name after `bios:` is a
  path relative to the system directory, case-sensitive, spaces, parentheses and slashes allowed). `required = yes`,
  or a list of systems/extensions (`segacd`, `fds`, `pcenginecd`...). Several mandatory entries with the same
  `required` value are **alternatives** (Sega CD US/EU/JP: one is enough). Arcade zip sets without md5
  (`neogeo.zip`) are also looked for next to the ROM. Missing → no launch, message
  `"<System> needs <file>[, <file> or <file>] in /bios"` (paths shown as the user sees the card: `/data` stripped).
  Wrong md5 → the game starts and a `warn` status line is sent (other dumps/revisions often work).
- **`system_files`** (flat list) and **`system_tree`** (a directory, e.g. bluemsx `Databases/` + `Machines/`) are
  copied into `/data/bios` when missing; existing files are never overwritten.
- **Zipped ROMs**: arcade cores (`block_extract = true`, mame2003_plus, fbneo) and cores whose extensions include `zip`
  get the zip as is. Otherwise the first member whose extension the core accepts (or the only file) is extracted with
  miniz to `/tmp/rsos/<pid>/` (tmpfs), max 64 MB; `.7z` is refused with a message. The extracted file is removed at
  exit. **Saves follow RetroArch**: the game name is the content basename without extension, and for an extracted zip
  the name of the file inside it.
- `need_fullpath = false` (also per extension via `SET_CONTENT_INFO_OVERRIDE`): the file is read into memory; the path
  is passed as well.
- **Core choice** (`host_pick_core()` / `coreinfo_pick()`): per-game (`[<system>/<rom stem>] core = ...`) then
  per-system (`[<system>] core = ...`) choice in `/data/rsos/cores.ini`, if that core accepts the file's extension
  (experimental cores allowed there); otherwise the automatic default among the candidates whose `systems` list the
  system and whose `extensions` match the file (a `.zip` also matches a non-arcade core that accepts a file inside):
  a core `.ini` with `default_systems` listing the system, then the docs/cores.md "ROM folders" table, then the first
  candidate. **An `experimental = true` core is never the automatic default** (`.neo` files in `neogeo` only match
  geolith: `-ENOENT`, and the UI offers `coreinfo_candidates()`, which flags experimental cores for an
  "(experimental)" label). The one documented exception is `neocd`, whose table default is geolith (no other core).

## 7. Saves and states

### Layout (RetroArch / RetroPie names)

```
/data/saves/<system>/<game>.srm           RETRO_MEMORY_SAVE_RAM (battery save, PS1 memory card 1)
/data/saves/<system>/<game>.rtc           RETRO_MEMORY_RTC (GB/GBC MBC3 clock: Pokemon G/S/C)
/data/saves/<system>/...                  files the cores write themselves (mame nvram/hi, fbneo, pcsx card 2)
/data/states/<system>/<game>.state        slot 0
/data/states/<system>/<game>.state1..9    slots 1-9
/data/states/<system>/<game>.state.auto   power-off / autosave slot (resume)
<state>.png                               thumbnail (last frame, box-filtered to <= 160 px wide)
<state>.bak                               the previous state of that slot (undo, and power-cut fallback)
```

Migration from RetrOrangePi/RetroPie: `.srm` files (kept next to the ROMs there) go to `/data/saves/<system>/`
unchanged; they are raw SRAM for fceumm, snes9x, gambatte, gpsp and pcsx. States are core-version specific and usually
do not load (say so to users); RetroArch compressed states (`#RZIPv1#`) are decompressed with miniz and handed to the
core.

### Durable atomic write

Every file (SRAM, states, thumbnails, option files, system files): write `<file>.tmp`, `fsync()`, `rename()` over
`<file>` (states: the old file is first renamed to `.bak`), `fsync()` of the directory. `/data` is exFAT (no journal),
so this ordering is what makes a power cut leave either the old or the new file. Recovery at load: if `<game>.srm` is
missing and a complete `<game>.srm.tmp` (exact SRAM size) exists, it is renamed into place; a missing state falls back
to its `.bak`. **A save file that exists but cannot be read disables SRAM writing for the session** (a blank SRAM
never overwrites a good save).

### When SRAM is written

The host keeps a hash (64-bit, 8 bytes per step) of what is on disk. Once per second it hashes SRAM and RTC:
unchanged → nothing; changed → wait until it has been stable for one check (≈1-2 s after the game's last write), or
until it has been dirty for 30 s (games that write continuously), then hand a copy to the save worker thread. Also
flushed: at exit (synchronous), on the save-state hotkey, when the menu opens, on P1 disconnect, on the power key long
press (synchronous, from the input layer's callback), `SIGUSR1`, sleep, `RSOS_SIG_POWEROFF`, and (stand-alone only)
the low-battery warning. A write failure resets the hash so it is retried. States and periodic SRAM writes run on the
worker thread (FIFO, so ordering is kept), with completion toasts ("State saved, slot 2"); exit and power-off writes
are synchronous.

### 7.1 Resume: the auto state, the launch prompt, the boot offer (2026-09-27)

Owner requests: "when I power off while a game is running and power it back on, it does not offer to resume" and
"even when we exit with Select+Start, we could save state automatically and offer to resume".

**When `.state.auto` is written** (game process, at the end of the main loop, after the synchronous SRAM flush):

| Exit | `.state.auto` |
|---|---|
| power-off (`RSOS_SIG_POWEROFF`: power key short/long press, critical battery, thermal) | always |
| Select+Start, Select+X > Exit game, `SIGTERM`, `RETRO_ENVIRONMENT_SHUTDOWN`, `--frames` reached | if `autosave_exit` (Settings > Games > "Auto-save on exit", **default on**; `--autosave`/`--no-autosave` win) |
| crash, hang (watchdog), SIGKILL after the grace time, a benchmark run | never (memory may be corrupt; the last one stays) |

`save_auto_state()` (host.c): thumbnail from the last frame, then `state_save(STATE_SLOT_AUTO, ..., sync)`: the
durable write of section 7 (tmp, fsync, the previous file renamed to `.bak`, rename, fsync of the folder), so a power
cut leaves the old or the new state. A core that cannot serialize now (`retro_serialize()` false, or
`SERIALIZATION_QUIRK_INCOMPLETE` before the first frame) writes nothing and the previous `.state.auto` stays as it
was. Numbered slots are never touched. The time is logged (`auto state saved (exit): 1 KB in 10 ms`); states of
1 MB and more (PS1 ~4.5 MB, N64 several MB) first get a **"Saving..."** frame over the game (an OSD toast, the same
path as "Powering off..."), so a slow SD card never looks like a hang. TODO(hw): exit times for PS1 and N64 (target
~1 s). On success the child sends `autostate <bytes> <ms>` on the status pipe (`res.auto_state_saved`).

**At launch** (UI, `ui_launch_game()`): if `cb.has_resume()` finds the game's `.state.auto` (main.c:
`host_auto_state_path()` under the states root), `resume_mode` (Settings > Games > "On launch") decides: `ask`
(default) "Resume where you left off?" [RESUME] [START FRESH] (B cancels), `always` resumes without asking, `never`
starts fresh without asking. Resume = `host_launch_opts.resume` = `--load-state auto`. "Start fresh" deletes
nothing; with auto-save on, the next exit replaces the auto state.

**The boot offer** (main.c). When the game ends with `HOST_EXIT_POWEROFF` **and** `autostate` was received, the
supervisor records the session in `/data/rsos/resume.ini` before powering off (tmp + fsync + rename + fsync of
`/data/rsos`):

```ini
; RetroStoneOS: the game that was running at the last power-off.
name = Super Mario World
system = snes
rom = /data/roms/snes/Super Mario World.sfc
core = snes9x2010                ; the core the state was saved with
core_path = /usr/lib/libretro/snes9x2010_libretro.so
core_source = game | system | default   ; where the core choice came from
options = game | system | default       ; the core options layer in use (informational)
p1_device = <input source id>           ; the pad that had launched the game (informational)
reason = user | battery-critical | thermal
state = /data/states/snes/Super Mario World.state.auto
state_bytes = 574720
time = 1790499522
```

At the next boot, in the main loop, once the first menu frame is out and `ui_is_loaded()` (so it never delays the
menu), not in charge mode (after a charge-mode exit it comes when the menu starts): if the file exists and the ROM,
the core `.so` and the `.state.auto` are all still there, `ui_offer_resume()` shows **"Resume <name>?"** [RESUME]
[START FRESH] (B = start fresh). Otherwise the file is deleted and the reason logged (`resume: X (rom): the game file
is gone, offer dropped`). The file is deleted as soon as the player answers, before the game starts, so the question
never comes twice. RESUME goes through the normal launch flow (web share stopped, CPU profile, game running, remap,
last played) with the **recorded core** and `req->resume` (so `--load-state auto`); player 1 is the pad that pressed
A (`input_last_source_id()`), as for any launch; the options are the usual layers. START FRESH leaves the menu and
keeps the auto state, so starting the game later still offers "Resume where you left off?".

Robustness: a power cut (battery pulled) during a game writes neither file, so nothing is offered (the periodic SRAM
flush is on the card); a game killed after the 8 s grace time sends no `autostate`, so no `resume.ini`; a power cut
between the state and `resume.ini` only loses the boot question (the launch prompt still works).

## 8. Video and pacing

The game surface is created at the core's `max_width x max_height` in its pixel format (`display_set_game_surface`),
frames go through `display_present_frame()` (size changes, conversion and copy in one call), the aspect from the AV
info goes to `display_set_scaling()` with the scale mode (aspect / integer / stretch; setting, menu, `--scale`).

### Modes (`pacing.c`)

Let `F` be the core fps, `S` its sample rate, `R` the display refresh (`display_output()->refresh_mhz`), `O` = 48000.

- **Vsync + DRC** when `|R - F| / F <= 1 %` (setting): one `retro_run()` per vblank. With double buffering,
  `display_present_frame()` blocks until the previous flip completed, so the loop period is exactly one refresh and
  input latency is one frame. The core now runs at `R` instead of `F`, producing `S·R/F` samples per second, so the
  base resample ratio (output frames per input frame) is

  `base = (O / S) · (F / R)`

  and dynamic rate control corrects the remaining drift (DAC clock versus pixel clock) from the ALSA fill `f`
  (0..1, target 1/2):

  `ratio = base · (1 + d · clamp((0.5 − f) / 0.5, −1, 1))`, `d = 0.005`

  A pitch change of at most 0.5 % is inaudible; the fill converges to a point where the ratio exactly matches the real
  clocks, so neither audio nor video ever drops. Examples: NES/SNES 60.0988 on HDMI 60.000 → the game runs 0.16 %
  slower; GBA 59.7275 → 0.46 % faster.
- **Audio clock** otherwise (the LCD at 78.6 Hz unless Settings > Display > "LCD refresh rate" is 60 Hz,
  display-design.md §3.1; PAL 50 Hz and 54-58 Hz arcade on a 60 Hz output):
  the core runs at its own `F`. Before each run the loop sleeps (`ppoll` on the display fd, so page-flip events are
  serviced) until the ALSA queue is down to half the buffer: audio is the master clock, `ratio = O / S`. Video uses
  latest-frame semantics: `display_begin_frame(0)` tells whether a buffer is free; if not the frame is skipped (the
  screen keeps the previous one, counted), and when the screen is faster than the core it simply shows frames longer
  (judder at 78.6 Hz, which is why the LCD should be retimed to 60 Hz: TODO(hw) in the DTS).
- **Timer** when there is no audio device (or during an output switch): `CLOCK_MONOTONIC` at `F`.
- **Free-run** for `--headless` tests.

A `NULL` (dupe) frame in vsync mode waits one vblank with `drmWaitVBlank()` so the loop keeps its period. The mode is
re-evaluated on every output change (`on_output`) and `SET_SYSTEM_AV_INFO`.

### Overlay

Toasts (bottom-left) are drawn into a copy of the core frame, in its own pixel format and resolution, only while
one is visible (one extra copy of ≤ 150 KB), scaled with the game on every path. Since 2026-09-27 (translations)
the text is TrueType (`frame_text()`: DejaVu Sans Condensed with the per-glyph fallback chain, rendered into a small
XRGB8888 buffer, then packed into the frame's format), so accents, Greek, Cyrillic and CJK show; without fonts it
falls back to the 8x8 font (`?` for non-ASCII). The FPS line and the battery pill stay 8x8 (digits).

**FPS overlay** (Select+X > Show FPS, `--stats`, setting `game_show_fps`): one line,
`SPD 97% FPS 29.8 SKIP 12 CPU 95/31 GPU 4.1` = emulated speed (runs per second / the core's rate), new frames per
second, frames not shown since the start (host skips + display drops), busy % of each CPU core (/proc/stat), and on
GLES the milliseconds per frame in the quad blit + `eglSwapBuffers`. It is refreshed once a second and shown on the
**display's overlay plane**, in the same image as the battery indicator: a strip across the top (or the bottom, with
a bottom battery corner), text on the side away from the battery, so the one alpha plane the A20 has holds both and
nothing is drawn into the game frame: **the N64 zero-copy path stays zero-copy with the overlay on**. If the plane
never shows the strip (TEST_ONLY refusal: `display_overlay_visible()` still false 1.5 s after the set), it falls back
to drawing the line into the frame, which on GLES means the readback path while it is on (logged once). During a
benchmark run a second, yellow line shows the run and its countdown.

### Performance counters and game.log

`perf.c` gathers the counters the host keeps (`retro_run()` calls, new frames, presented, skipped, dupes, core time,
display drops, underruns, the GLES blit/swap, present and readback times from `hwr_get_timing()`, process CPU time
and /proc/stat per CPU) into snapshots; a window is the difference of two. Three windows: 1 s for the overlay, 10 s
for game.log, the whole session. For every N64 / GLES game (`H.perf_log`), game.log gets: the effective core
options after `retro_load_game()` (every non-default value, plus the renderer, resolution, frameskip, Count Per Op,
CPU/RSP, and `rsos-glthread`, each with its source), the GL renderer string, the pacing mode (as for every core), a
`perf:` line every 10 s and a `perf session:` line at exit:

```
perf: 10.0 s: speed 97.3 % (58.38 VI/s), 29.2 fps (shown 29.0, effective 29.2), skipped 3, dropped 0, dupes 290,
      core 14.21 ms (max 31.02), gl swap 3.10 ms, present 0.41 ms, readback 0.00 ms, cpu 96/34 % at 960 MHz,
      process 104 %, underruns 0, stalls 2 (410 ms), shaders 6 (380 ms), links 3 (15 ms), slow draws 4 (40 ms),
      tex 120 (30 ms)
```

Since 2026-09-27 (hardware log `2026-09-27f`: Super Mario 64 at 67 %, max core time 400-616 ms per frame while the
average was 16 ms) the line also has the **cpu0 clock** at both ends of the window (`at 960 MHz`, or `at 720-960
MHz` if it moved; `scaling_cur_freq`), the **stalls** (frames whose core time was over 50 ms, `PERF_STALL_MS`) and,
for GL cores, what the GL probe saw (section 13.4): shader compiles and links, draws slower than 2 ms, texture
uploads, with their total time. Each stall is also logged on its own line (the first 100 per game):

```
stall: frame 1234: core 412 ms; GL: 2 compiles 250 ms, 1 links 40 ms, 3 slow draws 110 ms, 0 tex 0 ms, other 12 ms; cpu0 960 MHz
```

and the exit adds `gl probe: N shader compiles (X ms), N links, N draws (N over 2 ms: X ms), N texture uploads`,
`stalls: N frames over 50 ms (X ms in all)` and `stalls with shader work (compile, link or slow draw): N of M`. At
start: `cpu: governor performance, 960 MHz (policy 720-960 MHz)` (a warning if the governor is not `performance`).

### Battery overlay (in-game battery indicator)

A small pill in a screen corner with a battery icon, "70%" in white (amber at ≤ 15 %, red at ≤ 7 %) and a yellow
bolt while the charger is plugged in. Unlike the toasts it is **not** drawn into the game frame: it lives on the
display's overlay plane (`display_set_overlay()`, display-design.md 8.4), at screen resolution, unscaled, so it costs
nothing per frame and works the same for every core, including the N64 zero-copy GL path (no readback forced).

- **Content** (`src/host/batt_overlay.c`, `bov_render()`): drawn with the 8x8 font (nothing to load in the game
  process) into a 72x20 ARGB8888 buffer (the pill is about 56x20 for "70%", 70x20 for a bolt and "100%"), hugging the
  corner side. Pixels are either clear, opaque, or black with alpha 0xA8 (the pill), so premultiplied and straight
  alpha blending give the same picture. 2x (144x40) on outputs of 900 lines or more.
- **Place**: `game_battery_corner`; margin 4 px on the LCD, 3 % of the height on HDMI (21 px at 720p, 32 px at
  1080p) to stay clear of TV overscan. The display re-places it on every output switch; `on_output` also forces a
  redraw at the new scale and margin.
- **Value**: the supervisor's smoothed percentage (the menu's number), published in `/run/rsos/battery`
  (`"<percent> <charging>"`, power.md §10), read every 10 s (`bov_read()`); `--battery-file` overrides the path. No
  file, or -1: not shown. Stand-alone `rsos-run` (no supervisor) falls back to the AXP gauge. The picture is only
  redrawn and re-committed when the percent, the charging state or the scale changes.
- **In-game menu**: hidden while the menu (Select+X) is open (the menu is a full-screen 640x480 canvas, the pill
  would cover its top corner and the menu has its own context), shown again when it closes.
- **Off**: `game_battery_overlay = off` (Settings > Display > Battery in games), read at game start.
- If the kernel refuses the plane (TEST_ONLY), the display logs it once and simply shows nothing: the game is never
  affected.

## 9. Audio

- ALSA (alsa-lib) directly, `plughw:<card>,0`, S16_LE stereo **48 kHz** on both outputs (the codec's native rate; the
  sun4i HDMI audio takes 48 kHz S16), non-blocking. Card choice by name: HDMI = the card whose name contains `hdmi`
  (`sun4i-hdmi`); LCD = the card whose name contains `codec` (the built-in `sun4i-codec`), else the first non-HDMI
  card. Overridable (`audio_config.device_lcd/hdmi`).
- Buffer 64 ms (setting `audio_latency_ms`), 4 periods (HDMI: its 4 KiB minimum period gives 3 × 21 ms);
  `start_threshold` = one period, prefill with silence to half the buffer.
- **Output switching**: the display's `on_audio(RELEASE)` fades out, drains and closes the PCM before the modeset
  (the HDMI PCM must be closed before the encoder goes down); `ACQUIRE` opens the new card with a silence prefill and a
  10 ms fade-in, and the pacing is re-evaluated for the new refresh. The resampler keeps its state.
- **Underruns**: `-EPIPE` → `snd_pcm_recover()` → silence to half the buffer → fade-in (no click loop: the next frames
  land at the target fill, and DRC pushes the ratio up). Four underruns within 3 s raise the latency by 50 % (up to
  160 ms) instead of crackling. `SET_MINIMUM_AUDIO_LATENCY` raises it too.
- **Pause / menu / disconnect**: 5 ms fade to zero, then silence is kept flowing (`audio_keepalive()`) so the stream
  never stops (no DAPM power-down/up pop through the PAM8302 amp, which software cannot mute); fade-in on resume.
  Sleep closes the PCM, then turns the screen off (`display_set_active(false)`).
- **Volume**: the analog wheel; the host never touches the mixer (rcS sets the codec level). HDMI volume (software
  gain) is a P1 item for later.
- **Resampler** (`resampler.c`, written for this project): 16-tap Kaiser-windowed sinc (β = 6), 128 phases with
  linear interpolation between phases, cutoff at 0.91 × min(1, ratio) of the input Nyquist (no aliasing when a core
  runs above 48 kHz), coefficient rows normalised to unity gain, float, explicit NEON path (4 × `vmlaq_f32` per channel
  per output frame, ≈ 1.5 M MAC/s at 48 kHz). The ratio can change on every call. A two-tap linear mode exists for
  comparison.

## 10. Input and hotkeys

Input is read right before each `retro_run()` (`hin_poll()`); `input_state` answers ports 1-4 from the input layer:
`JOYPAD` (and `JOYPAD_MASK` in one call), `ANALOG` (left/right stick, and analog buttons for L2/R2). Remaps
(`input_load_remap(system, game)`), player assignment and the "hotkey buttons are not passed to the core" rule belong to
the input layer; the host only consumes its hotkey queue:

| Hotkey | Action |
|---|---|
| Select+Start | exit (SRAM flush, optional auto state) |
| Select+R / Select+L | save / load the current slot (toast; thumbnail) |
| Select+Right / Left | slot + / − (toast shows "(empty)") |
| Select+X | in-game menu |
| Select+B | reset |
| Select+R2 | fast-forward on/off (batch 2) |
| Select+L2 | screenshot (batch 2) |
| Select+Y | game switcher (batch 2, section 11.1) |
| pad disconnected (P1) | pause + toast until P1 is back |
| ports changed | controller port devices re-chosen (analog device when the port has a stick) |

**N64 without a stick**: `input_set_dpad_to_analog(port, true)` for every port without an analog source (also a toggle
on the Controls page).

**Fast-forward** (Select+R2 toggles it; also the menu's Fast-forward item, `host_set_ff()`): N runs of the core per
shown frame, N = `ff_speed` (2..4, default 3; `--ff-speed`). Only the last run is shown; the audio of every run is
dropped and the ring gets silence, so the sound is muted while it lasts (no pitch-shifted chipmunk sound, no ring
overflow). The overlay strip shows `▶▶ x3` (glyph 0x10 of the 8x8 font twice), or the OSD text `>> x3` when the
overlay plane is off. Pacing: each pacing mode keeps its clock (vsync, audio or timer), so the speed is N x the
game's rate; the DRC's latency back-off is suspended during fast-forward and for 2 s after, and the pacing restarts
from zero at the switch back (no catch-up burst). Logged: `fast-forward on (x3) at frame F` / `fast-forward off at
frame F: R runs in T ms (x3.05)`.

**Screenshot** (Select+L2): the last frame (`frame_thumbnail()`, any pixel format, GLES read back), 2x nearest when
240 px wide or less, written atomically to `/data/screenshots/<system>/<game>-<YYYYMMDD-HHMMSS>.png` (`-2`, `-3`...
for a second one in the same second; `--screenshots DIR`). Toasts "Screenshot saved" / "Screenshot NOT saved" / "No
picture to save yet".

**Rumble**: `retro_set_rumble_state` → `input_rumble()` (evdev FF_RUMBLE, input-design.md §2); `--rumble 0|1`, else
the `rumble` setting (on).

**Play time**: `struct playtime` (`playtime.c`) counts the monotonic time the game runs; paused by the in-game
menu, the switcher, P1 disconnected and sleep. Reported as `playtime <s>` lines (see Status pipe).

**Per-game settings**: `--scale` (the game's own or the setting, from main.c) and `--cpu-profile auto|performance|
powersave` (logged `scaling: aspect (--scale), cpu profile: performance`); the menu's CPU profile item changes the
governor at once (`power_set_game_cpu`). Changed in the menu, both are reported (`setting ...` lines) when it closes.

**Test hooks**: `--hotkey-script "F:name,..."` fires hotkeys at frame F (`ff`, `shot`, `switcher`, `menu`, `save`,
`load`, `exit`, `mark` logs the frame and time), `--timer-pacing` (clock pacing when headless),
`--switcher-pick N` (picks entry N at frame 12), `--switcher-shot FILE.png` (draws the switcher and exits).

The temporary `host_input_evdev.c` backend (all devices on P1, gamepad codes plus a debug
keyboard map, Select hotkeys, power long press) is used only if `src/input/input.c` is absent.

## 11. In-game menu (Select+X)

The game pauses (no `retro_run`, audio faded to silence, SRAM flushed), the game plane switches to a 640x480 XRGB8888
canvas (1:1 on the LCD, scaled by the plane on HDMI) showing the dimmed last frame, and the input layer switches to
UI navigation (key repeat). Pages:

- **Main**: Resume · Save state `< Slot N >` · Load state `< Slot N >` (preview panel: the slot's PNG thumbnail and
  date, or "Empty") · Disc `< i / n >` (multi-disc `.m3u`) · Reset · Core options > · Scaling `< Aspect / Integer /
  Stretch >` · Show FPS · Fast-forward · Benchmark this game > (when a plan exists for the system: N64) · Controls > ·
  Exit game.
- **Core options**: the core's visible options (label, value and its source tag, Left/Right or A to change, applied
  at once through `GET_VARIABLE_UPDATE`), the option's info text, then "Save for this game", "Save for all games (this
  core)", "Reset to defaults". Changes are saved for this game when the page or the menu is left (section 5).
- **Benchmark**: how many settings, the estimated time, "Start benchmark" (section 18). **Benchmark results** (opened
  by itself after a benchmark): the runs in rank order with speed and effective fps, "Use this for this game",
  "Resume game"; the status line gives the report's path.
- **Controls**: players 1-4 (pad name, stick), "D-pad as left stick (P1)", the core's input descriptors for P1.

A: select, B/Start: back (resume from the main page), L/R: page up/down. On close the game surface is restored and
the pacing restarts from zero. `rsos-run --menu-shot FILE.png` renders the main and options pages without a display.

**Languages** (docs/translating.md): every text of the menu, the toasts and the messages sent to the menu (BIOS,
crash, zip errors) goes through `_()`; the game process takes the language from `--lang` (the menu always passes the
one in use), else the `language` key of settings.ini, loads `<locale>/<lang>.cat` (`--locale`, default
`/usr/share/rsos/locale`) before the BIOS check, and the fonts from `--fonts` (default `/usr/share/rsos/fonts`). Text
is TrueType (DejaVu Sans Condensed, 11 px at scale 1, 18 px at scale 2, the CJK fallbacks), cut to the pixel width
with "..." (`cv_text_fit()`): a row's value keeps at least half the row and the label is cut before it, so long
translations never overlap. `--menu-script sel=<label>` also matches the English label under any `--lang`.

**Hook for the UI agent**: the menu draws only through `draw.h` (`cv_fill`, `cv_text`, `cv_text_fit`,
`cv_text_ellipsize`, `cv_blit_frame`, `cv_blit_rgb`; `draw_set_fonts(dir)` loads the TrueType fonts) on an XRGB8888 `struct canvas`. To theme it, implement those four functions on top of `src/gfx`
(`gfx_fill_round`, `font_draw`, `gfx_blit`) in a `draw_gfx.c` and select it in `host.mk` instead of `draw.c`; layout
constants are at the top of `menu.c`.

### 11.1 Game switcher (Select+Y, batch 2)

main.c writes `/run/rsos/switcher.tsv` before each launch (`--switcher FILE`): a `#` header, then one line per game,
tab-separated `current name system rom core core_path thumb` (the running game first, then the last played games
from gamedb, 8 at most; `thumb` = the game's `.state.auto.png`). Select+Y pauses the game (audio silent, play time
paused) and shows a 640x480 canvas like the in-game menu: "Recent games", 4 x 2 cards (a 128x96 picture: the live
frame for the running game ("Now playing"), else the auto state's thumbnail or "No picture"; the name; the system),
the selected game's full name, "A: play  B: back to the game". Left/right/up/down move, B (or Select+Y) returns to
the game. A on another game: the running game is saved to its auto state (even with auto-save on exit off), the
child writes `switch <n>` and exits with `HOST_EXIT_SWITCH`; main.c reads the file back (`host_switcher_read`) and
calls `ui_switch_to()`, which launches entry n with resume (auto state) once the menu has processed the exit. No other
game played: the toast "No other game played recently". Preview: `docs/ui-previews/b2-fr-switcher.png`.

## 12. Power, sleep, governor

See section 1 (signals, governor ownership). The battery level is also answered to cores (`GET_DEVICE_POWER`).
Stand-alone, the child flushes at 7 % (toast) and saves + powers off at 3 % (TODO(hw): thresholds; under the UI the
power module decides, docs/power.md).

**Power key in a game**: any press (short or long) powers off (power.md §5): the supervisor sends
`RSOS_SIG_POWEROFF`; the child puts up one "Powering off..." frame (an OSD toast over the last frame, through the
surface even on the GL path), writes SRAM and `.state.auto`, and exits 3; the supervisor records the game in
`/data/rsos/resume.ini` and the next boot offers "Resume <game>?" (section 7.1). There is no sleep mode any more;
`RSOS_SIG_SLEEP`/`WAKE` are still handled but never sent.

**Board profile.** The game process reads the same `/etc/rsos/board.ini` as the menu (`board_get()`,
`src/board.h`; the menu passes its path in `RSOS_BOARD_INI`, docs/porting.md): the game/menu governors
(`cpu_governor_game`/`_menu`, `sys.c`), the built-in pad names for its input layer, the built-in screen's connector
types for its display, and the ALSA cards (`audio_internal`, `audio_hdmi`, `audio_hdmi_pcm`). On a board with two
HDMI ports the HDMI card of the port in use is opened (`HDMI-A-2` -> the second "hdmi" card). `game.log` starts with
the `board <name> (<path>)` line.

## 13. HW rendering (N64, GLES2)

Nothing GL-related runs unless a core calls `SET_HW_RENDER`. Then:

1. `SET_HW_RENDER` (during `retro_load_game`): accept `RETRO_HW_CONTEXT_OPENGLES2` (or `OPENGLES_VERSION` 2.x;
   lima is GLES 2.0 only), `dlopen("libEGL.so.1", "libGLESv2.so.2", "libgbm.so.1")` (the binary does not link Mesa),
   fill `get_current_framebuffer` and `get_proc_address` (`eglGetProcAddress`, then `dlsym`).
2. After `retro_load_game`: `hwr_init(max_w, max_h, display_get_drm_fd())` sets up the context (13.1 or 13.2), then
   **the core's FBO** in both cases: an RGBA texture at `max_width x max_height` plus a depth renderbuffer
   (`DEPTH24_STENCIL8_OES` when stencil is asked and `OES_packed_depth_stencil` exists, else `DEPTH_COMPONENT16` +
   `STENCIL_INDEX8`). Then `context_reset()`.
3. Exit: `retro_unload_game()`, `context_destroy()`, `retro_deinit()`, **`display_shutdown()` (it releases the BOs it
   still holds), then `hwr_deinit()`** (FBO, context, window surface, GBM surface: the BO destroy callbacks remove the
   DRM FBs, GBM device, fd). The GL libraries stay mapped until the process exits (the core references them).

### 13.1 Zero-copy path (default)

- **Set-up.** `gbm_create_device()` on a **dup** of `display_get_drm_fd()` (Mesa kmsro: lima renders, the scanout
  buffers come from sun4i-drm CMA; the dup keeps the DRM file, hence the GEM handles, alive after
  `display_shutdown()` closes the display's fd), `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR)`, an ES2 window
  config whose `EGL_NATIVE_VISUAL_ID` is `GBM_FORMAT_XRGB8888`, `gbm_surface_create(max_w, max_h, XRGB8888,
  SCANOUT | RENDERING)`, an EGL window surface on it, the context made current on it, `eglSwapInterval(0)` (the display
  paces), and a two-attribute textured-quad program.
- **Each frame** (`video_refresh(RETRO_HW_FRAME_BUFFER_VALID, w, h)`), `hwr_present()`:
  1. in audio-clock mode, if `display_flip_pending()` the frame is skipped before any GPU work;
  2. the `w x h` region of the core's FBO texture is drawn with one quad into the top-left `w x h` of the window
     (viewport `(0, H − h, w, h)`: GL rows are bottom-up and Mesa presents window surfaces upright). **The plane cannot
     flip vertically**, so `bottom_left_origin` is handled here: a bottom-left frame is copied as is, a top-left one
     is flipped by the texture coordinates. The GL state the quad touches (program, texture unit and binding, array
     buffer, viewport, depth/blend/scissor/cull/stencil, attribute arrays 0/1) is saved and restored, and the core's
     FBO is bound again;
  3. `eglSwapBuffers`, `gbm_surface_lock_front_buffer`, a DRM FB for the BO (`drmModeAddFB2`, cached in the BO's user
     data, removed in its destroy callback);
  4. `display_present_fb(&fb, timeout)` with `fb = { fb_id, DRM_FORMAT_XRGB8888, BO width/height, src = {0, 0, w, h},
     release = gbm_surface_release_buffer(surface, bo) }`, `timeout` = −1 in vsync mode (it waits for the previous
     flip: the pacing) and 0 in audio-clock mode.
- **Return codes.** 0: the display owns the BO until `release()`. `-EINVAL` (the plane or the frontend scaler refuses
  the BO): the BO is released and **zero-copy is switched off for the rest of the game**, the frame goes through the
  readback path. `-EBUSY`, `-EAGAIN` (screen off, suspended), `-ETIMEDOUT`, `-EINTR`: the frame is not shown, the BO
  is released at once, counted as skipped.
- **Overlay frames.** While a toast is visible, the frame goes through the readback path (13.2), where the OSD is
  drawn; the surface flip then releases the external FB (display-design 8.1, "mixing"). The FPS overlay does not:
  it is on the overlay plane (section 8), unless the plane was refused. The in-game menu uses the surface.
- **Timing.** `hwr_get_timing()`: cumulative microseconds in blit + `eglSwapBuffers` + lock (the GPU/driver wait),
  in `display_present_fb()` (the flip wait in vsync mode) and in readbacks, for the overlay, game.log and the
  benchmark.
- **Last frame on demand.** In zero-copy mode nothing is read back per frame. The menu background, save-state
  thumbnails and `--dump` call `host_capture_last()`, which reads the FBO back once (it still holds the last frame).
- **Output switch** during a GL game: the display re-commits the FB on the new output itself (or refuses it, and the
  next present returns `-EINVAL`: readback from then on).

### 13.2 Readback path (fallback)

Used when the zero-copy set-up fails (no EGL window config for XRGB8888, no kmsro, `gbm_surface_create` refused...),
after a `-EINVAL` from `display_present_fb()`, in `--headless` runs (then on the lima render node with a
surfaceless context, `EGL_KHR_surfaceless_context`), and for overlay frames. `glReadPixels` of the `w x h` region of the
FBO, flipped when `bottom_left_origin`, swizzled RGBA → XRGB8888 into a host buffer, then the normal
`display_present_frame()` path (plane scaling, OSD, pacing: everything as for software cores). Cost: a GPU sync and
~300 KB read + convert per 320x240 frame (TODO(hw): measure both paths on the A20: `hw render: N zero-copy frames`
is logged at exit).

### 13.3 Display API used

`display.h` now provides what the host proposed (display-design.md 8.1-8.3):

```c
struct display_fb { uint32_t fb_id; uint32_t format; int width, height; struct display_rect src;
                    void (*release)(uint32_t fb_id, void *user); void *user; };
int display_present_fb(const struct display_fb *fb, int timeout_ms);  /* 13.1 */
bool display_flip_pending(void);                                       /* audio-clock skip */
int display_suspend(void);  int display_resume(void);                  /* section 1, UI side */
int display_set_active(bool active);                                   /* RSOS_SIG_SLEEP / WAKE */
```

`display_set_active(false)` is called by the game process on `RSOS_SIG_SLEEP`, after ALSA is closed (the HDMI PCM
must be closed before the encoder stops), and `display_set_active(true)` on `RSOS_SIG_WAKE`.

### 13.4 Shader cache and the stall probe (2026-09-27)

**Finding.** Hardware log `2026-09-27f`, Super Mario 64 on parallel-n64 (Rice, auto frameskip): 67 % speed, 128
underruns in 25 s, gl swap 1.4 ms, both CPUs ~45 % busy, average core time 16 ms but **max 400-616 ms per frame**.
The same game.log has, right before the EGL set-up:

```
Failed to create //.cache for shader cache (Read-only file system)---disabling.
```

Mesa 26.0.1 chooses the cache directory from `MESA_SHADER_CACHE_DIR`, else `$XDG_CACHE_HOME`, else
`$HOME/.cache` (`disk_cache_generate_cache_dir()`, `src/util/disk_cache_os.c`); the game process has `HOME=/` on
the read-only root, so the on-disk cache was **off**. lima supports it (`lima_disk_cache.c`: compiled VS/FS are
stored and looked up by a key over the NIR and the variant key), and Mesa's GLSL front end skips a compile whose
key it has seen (`disk_cache_has_key()`) and loads linked programs from it (`shader_cache.cpp`). Without the cache,
Rice's combiner shaders were compiled at every session, on the frame that first needs them: `glCompileShader` /
`glLinkProgram` (GLSL to NIR) at combiner creation, and **lima's backend compile at the first draw** that uses the
program with a new state (`lima_update_fs_state()` in `lima_draw.c` -> `lima_get_compiled_fs()`, not at link time
unless `LIMA_DEBUG=precompile`). Hundreds of ms on the A7: the stalls, the skipped frames and the underruns. The ROM
is not read lazily (parallel-n64 reads the whole file with `filestream_read_file()` in `retro_load_game()`, even
with `need_fullpath`), and the governor was `performance` at 960 MHz during the game (`status.txt`).

**Fix.** `hwr_shader_cache_env()` (hwrender.c), called for a `renderer = gles2` core before `core_open()` and again
in `SET_HW_RENDER`, before any EGL display exists: `mkdir -p /data/rsos/cache/mesa` (`--shader-cache DIR`, `""` =
off; an existing `MESA_SHADER_CACHE_DIR` in the environment wins), then

| Variable | Value | Why |
|---|---|---|
| `MESA_SHADER_CACHE_DIR` | `/data/rsos/cache/mesa` | writable, survives reboots |
| `MESA_SHADER_CACHE_MAX_SIZE` | `64M` | Mesa's default is 1 GB |
| `MESA_DISK_CACHE_DATABASE` | `1` | a few files that pack the entries (`mesa_shader_cache_db/partN/`); the default multi-file cache makes one file per entry, a whole exFAT cluster each (32-128 KB) |
| `MESA_DISK_CACHE_DATABASE_NUM_PARTS` | `4` | a miss opens every part once (8 files instead of 100) |
| `MESA_SHADER_CACHE_SHOW_STATS` | `true` | Mesa prints the cache hits and misses on stderr (game.log) when the GL screen is destroyed |

Cache writes happen in Mesa's own thread; the cache key includes Mesa's build id, so a new image starts a new cache.
Expected: the **first** session of a game still compiles (and stalls) as before, the **second** loads the programs
and lima variants from the card (a few ms each). The legacy `MESA_GLSL_CACHE_*` names are only read when the new
ones are absent.

**The stall probe** (`src/host/glprobe.{h,c}`). The game process exports `glCompileShader`, `glLinkProgram`,
`glDrawArrays`, `glDrawElements`, `glTexImage2D` and `glTexSubImage2D` (`-Wl,--dynamic-list=src/host/glprobe.syms`,
frontend and rsos-run). parallel-n64 links `libGLESv2.so.2` directly (`NEEDED`, the GL calls are undefined
symbols of the core), and the dynamic linker searches the executable before the core's own dependencies, so the
core's calls reach the wrappers; a core that resolves them through the hw render `get_proc_address` gets the same
wrappers (`glprobe_wrap()`). Each wrapper times the call (vDSO clock, atomic counters: GLideN64's threaded renderer
is fine) and forwards it to the real entry point (`dlsym` on hwrender's libGLESv2 handle, or the loaded one). The
main loop snapshots the counters around each `retro_run()` and logs the stalls (section 8). A draw slower than 2 ms
is almost always a lima variant compile (or a wait for a busy buffer). The dynarec gives no hook for "cache flushed",
so that part of a stall shows as "other". Test: `rsos-glprobe-test` (a fake `libGLESv2.so.2` with known call times,
a "core" linked against it and loaded `RTLD_LOCAL`, a driver linked like the frontend: the direct calls and the
`get_proc_address` path are counted with their times, and every call still reaches the "driver").

**What the next hardware log should show** (N64, same game twice): session 1: `shader cache: /data/rsos/cache/mesa
(Mesa database, max 64 MB)`, stall lines with compiles/links/slow draws, Mesa's stats with misses; session 2: far
fewer stalls, stall lines without compiles (or with fast ones), Mesa's stats with hits, and a higher speed. If the
stalls stay and show `other` only, the shaders were not the cause (then look at the dynarec and the RSP).

## 14. rsos-run

```
rsos-run --core /usr/lib/libretro/fceumm_libretro.so --rom /data/roms/nes/x.nes [--system nes]
         [--frames N] [--headless] [--dump last.png] [--test-states] [--menu-shot menu.png]
         [--scale aspect|integer|stretch] [--stats] [--latency MS] [--vsync-tolerance PCT] [--hdmi WxH]
         [--lcd-refresh 60|0] [--shader-cache DIR] [--lang CODE] [--locale DIR] [--fonts DIR]
         [--p1 auto|builtin|external] [--p1-device ID] [--cz 0|1] [--load-state-file F]
         [--menu-script STEPS] [--bench-start N --bench-seconds S --bench-auto-apply --logs DIR]
         [--load-state 0-9|auto] [--autosave|--no-autosave] [--no-governor] [--status-fd N]
         [--poweroff-cmd CMD] [--bios DIR --saves DIR --states DIR --coreopts DIR --coreopts-ship DIR
          --cores-info DIR --tmp DIR --settings FILE --battery-file FILE] [-v|-q]
         batch 2: [--screenshots DIR] [--switcher FILE] [--switcher-shot FILE.png] [--switcher-pick N]
         [--hotkey-script "F:name,..."] [--timer-pacing] [--playtime-report S] [--ff-speed 2-4] [--rumble 0|1]
         [--cpu-profile auto|performance|powersave]
```

`--headless`: no DRM, no ALSA, no input, free-running; `--dump` writes the last frame as PNG; `--test-states` saves a
state at frame N/2, replays to N from it and compares the frame hashes; the path options point everything at a test
tree. The same options are accepted after `rsos-frontend --run`. `--menu-script` drives the in-game menu at frame 10
without a display (`sel=<label>,a,b,l,r,u,d`), `--bench-start N` starts a benchmark at frame N as the menu would,
`--bench-seconds` shortens the runs (1 s warm-up), `--bench-auto-apply` presses "Use this" on the results;
`--bench-driver`, `--bench-step` and `--bench-report` are the benchmark's internal re-executions (section 18).

## 15. Tests (WSL, x86_64, 2026-09-26)

All built with `-O2 -Wall -Wextra`, zero warnings (host gcc 13 and `arm-linux-gnueabihf-gcc` 13, Cortex-A7 NEON
flags, against the Buildroot staging headers for libdrm/EGL/GLES2; `make -f host.mk host-objs`). `host.mk` also
checked when included by `frontend/Makefile`.

- `make -f host.mk host-check`: **unit tests** (also run as ARM code under `qemu-arm -cpu cortex-a7`, NEON path):
  resampler 32040.5→48k 1 kHz SNR 65 dB, 44.1k→48k 5 kHz 75.7 dB, 65536→48k (downsampling) 59.5 dB, linear 49 dB;
  pacing policy cases; **DRC simulation** of 10 minutes each (SNES on HDMI 60.000, NES 48 kHz on 59.94, GBA, PS1 with
  a DAC +300 ppm fast, MD −300 ppm): 0 underruns, 0 overflows, fill settled at 47-53 % of 3072 frames, ratio within
  the ±0.5 % window; INI (inline comments, quoted values), MD5, atomic write + `.bak`; option layering, invalid values,
  `GET_VARIABLE_UPDATE`, per-game diff file; BIOS check (Sega CD alternatives, Neo Geo zip next to the game); core
  choice (docs table, per-game choice, extension filtering, experimental never automatic), `system_tree` copy.
- **Process model** (`rsos-launch-test` with `src/host/tests/testcore.c` through `host_launch()` + `rsos-run --headless`):
  normal exit (SRAM written), missing BIOS → status 2 + message, SIGSEGV in `retro_run` → crash reported with
  signal 11, infinite loop → watchdog exit 4 after 15 s.
- **Resume (2026-09-27, section 7.1)**: `rsos-launch-test`: `autosave_exit = 0` → no auto state and no `autostate`
  line; `= 1` and no key (the default) → `.state.auto` + thumbnail, reported; a core that refuses to serialize
  (`RSOS_TESTCORE=nostate`) leaves the previous `.state.auto` byte for byte; `RSOS_SIG_POWEROFF` during a slow game
  (`RSOS_TESTCORE=slow`) with the exit auto-save off → exit 3 and the state written anyway; `opts.resume` loads it.
  `make check-frontend` step 5 (the real main loop, headless, script tokens `game:wait:MS game:poweroff` run while
  the game runs): power-off during the game → `resume.ini` recorded; the next boot shows "Resume Smoke Test?",
  RESUME launches with `--load-state auto` (`load state ...state.auto: ok`) and the file is gone; START FRESH and B
  keep the auto state and drop the file; a missing ROM → no dialog, file dropped; the launch prompt with
  `resume_mode` ask / always / never; Settings > Games toggles `autosave_exit` and the next exit writes nothing.
- **Core options regression (2026-09-27)**: `--menu-script` changes an option in the menu and exits; the next start
  logs the value the test core received (`testcore: testcore_speed=fast`); "Save for all games" removes an older
  per-game value (the core then receives it). Unit tests: source tags, auto-save, reload after an outside write, the
  benchmark override never written.
- **Benchmark orchestration** (headless, test core with options `testcore_cost` = busy ms per frame and
  `testcore_crash`): three configurations (12 ms, 0 ms, crash at frame 30) each run in a fresh process from the start
  state, the crash is recorded and the benchmark goes on, the report and a PNG per completed run are written, the
  game restarts from the start state with the results, the best is the 0 ms run, and "Use this" writes it to the
  per-game file. Unit tests: plan parse/write, result format/parse, ranking (full speed first by effective fps; crash,
  from-boot and blank runs not ranked), report, flat-picture check, argument filtering, the per-game options merge,
  the core choice file, perf maths (/proc/stat, percentiles, effective fps, overlay/log lines), the overlay strip, and
  the player-1 assignment (the launching pad, fallbacks, hot-plug).
- **Real cores** built for x86_64 (`~/rsos/host-cores/`, pinned commits, `platform=unix`): fceumm, gambatte,
  snes9x2005, with freely redistributable ROMs: the 240p Test Suite ports by Damian Yerrick (`240pee.nes`,
  `gb240p.gb`, GPL-2.0+) and three tiny SRAM test ROMs generated by `src/host/tests/mk_test_roms.py` (original, public
  domain; GB MBC1+RAM+battery, NES NROM+battery, SNES LoROM+SRAM). Results in `~/rsos/host-test/`:
  frames render (PNG dumps show the test suite screens and the SRAM-dependent graphics); **SRAM saves and reloads**
  (each ROM booted twice: the counter byte in the `.srm` went 1→2, 0→1, 1→2, with the "RSOS" signature); **save
  states round-trip** (frame 600 identical when replayed from the state saved at frame 300: fceumm 22 KB, gambatte
  51 KB, snes9x2005 562 KB); zipped ROM extracted to tmpfs, save named after the inner file; `--autosave` wrote
  `.state.auto` + PNG and `--load-state auto` resumed it; missing core / missing ROM → exit 2 with a message; the menu
  rendered with `--menu-shot`.
- **Stalls and the GL probe (2026-09-27, section 13.4)**: `rsos-glprobe-test` (in `make check-host`): the
  interposition through the real dynamic linker (a core loaded `RTLD_LOCAL` with `NEEDED libGLESv2.so.2`), 2
  compiles of 60 ms, a link, 3 draws of which one slow, an upload, a compile through `get_proc_address`: all
  counted with their times, every call forwarded. The perf line with the new fields (`test_host`).
  `check-frontend` step 6: the child gets `--lcd-refresh 60` (game.log `lcd refresh: 60 Hz (25.2 MHz user mode)`).
- **Batch 2 (2026-09-27)**: `rsos-launch-test` `test_batch2`: the switcher file written and read back; entry 1
  picked (`--switcher-pick 1`) → exit 6, `switch_to` 1, the auto state saved with `--no-autosave`; the switcher
  drawn (`--switcher-shot`); play time with a slow core (3 s, `playtime` lines seen as they came through
  `on_status`); Scaling → Integer and CPU profile → Performance in the menu reported as `setting` lines; the game's
  own scaling/profile at start; fast-forward x3 with clock pacing (`--timer-pacing`, `--hotkey-script
  "60:ff,120:ff,180:mark"`): 180 runs in 984 ms (x3.05) then 60 normal frames in 983 ms (no burst, no stall);
  Select+L2 twice → 2 PNG files (the second `-2`), 320x240 (the core's 160x120 doubled). `rsos-host-test`: the
  fast-forward DRC model (SNES x3, GBA x4: the fill stays in the window, no overflow, no underrun after) and the
  play-time clock (pause nesting, resume). Also `make check-b2` (ui-design.md §12).
- Not testable in WSL: DRM, ALSA, evdev, lima (section 16).

Reproduce: `src/host/tests/mk_test_roms.py OUTDIR`, then `rsos-run --headless ...` as in section 14.

## 16. Hardware test checklist

**Basics**
- [ ] `rsos-run --core .../fceumm_libretro.so --rom <nes> --stats`: picture, sound, `pacing:` log line. On the 78.6 Hz
      LCD: `audio clock`; on HDMI 60: `vsync+DRC`. FPS 60.1 / 60.0, no underruns in the overlay after a minute.
- [ ] Same with `--scale integer` and `stretch`; a 256x224 core (SNES) and 160x144 (GB) look right.
- [ ] `-v`: the DRC ratio settles (debug line every second), the fill stays near 50 %.
- [ ] Audio: no pop at start, on Select+X (menu) and back, on exit. Headphones and speaker.
- [ ] `top`: CPU use of NES (should be low), PS1 (pcsx: two threads).

**Pacing**
- [ ] LCD at 78.6 Hz: judder is expected, audio clean. With Settings > Display > LCD refresh rate = 60 Hz
      (display-design.md §3.1): `pacing: vsync+DRC (... display 60.000 Hz ...)` for NES/SNES/GB, smooth scrolling.
- [ ] PAL game (50 Hz) on HDMI: `audio clock`, correct speed, clean audio.
- [ ] pcsx with `frameskip = auto` on a heavy 3D game: audio stays clean while frames are skipped.

**Output switching**
- [ ] Plug/unplug HDMI during a game, 10 times: audio moves to the TV and back, no ALSA error, the game keeps
      running, pacing log line changes mode.

**Saves**
- [ ] Pokémon Gold: save in game, wait 3 s, pull the battery: the save is there after reboot (and the `.rtc`).
- [ ] Select+R / Select+L on each core; slot +/-; thumbnails in the menu's slot picker.
- [ ] Power key short press (and long press) in a game: "Powering off..." shows at once, `.state.auto` written,
      power off; the next boot shows "Resume <game>?" once the menu is up (section 7.1).
- [ ] Select+Start: `auto state saved (exit): N KB in M ms` in game.log for NES/SNES/GBA/PS1/N64; "Saving..." on PS1
      and N64; the next launch offers "Resume where you left off?".
- [ ] Battery indicator: top-right by default, the menu's number, updates within ~10-20 s of a change, bolt when
      plugged in, hidden in the Select+X menu, gone with Settings > Display > Battery in games = off, other corners;
      N64 keeps zero-copy.
- [ ] Copy a RetroPie `.srm` in `/data/saves/<system>/`: the game sees it.

**Process model**
- [ ] A core crash (e.g. a bad dump in gpsp) returns to the menu with the message; the menu is intact.
- [ ] Hang: build `src/host/tests/testcore.c` for ARM and run it with `RSOS_TESTCORE=hang`: watchdog message after
      15 s, back to the menu.
- [ ] Time from "launch" to the first game frame for NES, PS1, fbneo (TEXTREL relocation), N64.

**N64**
- [ ] parallel_n64 + glide64: picture right side up (zero-copy quad orientation), depth correct (Mario 64 castle),
      `hw render: zero-copy` and `first zero-copy frame` in the log; exit logs `N zero-copy frames` and returns to the
      menu cleanly.
- [ ] N64 with `--stats` (overlay = readback path): same orientation as zero-copy; compare frame times of both paths.
- [ ] N64: HDMI plug/unplug mid-game (the display re-commits the external FB), Select+X menu and back, save state
      thumbnail correct, RSOS_SIG_SLEEP/WAKE (screen off/on).
- [ ] mupen64plus_next (GLideN64): starts with `EGL_PLATFORM=gbm`.
- [ ] Show FPS on an N64 game: the line is on the overlay plane next to the battery, `first zero-copy frame` in the
      log and no `overlay plane refused`; the frame time does not change when it is toggled.
- [ ] Rice default, frameskip auto: SPD near 100 % with SKIP counting up on heavy scenes; no freeze, no flicker.
- [ ] **Shader cache (13.4)**: play Super Mario 64 for ~30 s, exit, start it again and play the same part: game.log
      has `shader cache: /data/rsos/cache/mesa (Mesa database, max 64 MB)` and no `Failed to create //.cache`;
      session 1 has `stall:` lines with compiles / slow draws, session 2 far fewer (and Mesa's cache stats with
      hits), a smaller `max` core time and fewer underruns in the `perf:` lines. `/data/rsos/cache/mesa/` exists.
      `cpu ... at 960 MHz` in the perf lines and `cpu: governor performance` at start.
- [ ] Benchmark on 2-3 games (docs/bringup.md 7b): every run starts at the same scene, Select+Start stops it, the
      results page appears, "Use this" then a restart uses the winner (game.log `option ... (game)`).
- [ ] mupen64plus_next threaded renderer: sound clean, no crash at exit.

**Input**
- [ ] Hotkeys as RetroPie; Select alone reaches the game; P1 unplug pauses; N64 without a stick uses the d-pad.
- [ ] Batch 2 (docs/bringup.md 7f): Select+R2 fast-forward on the LCD and HDMI (speed, silence, no crackle after),
      Select+L2 screenshot of a GLES game (N64: right side up), Select+Y switcher, pad rumble.

## 17. Limits and open items

- Zero-copy HW rendering and the readback fallback are both implemented but untested on lima (no GPU in WSL).
- `SET_ROTATION` is refused: vertical arcade games rely on the cores' software rotation (to check on hardware).
- Rumble: waits for an input-layer FF API. Cheats, rewind, netplay, RetroAchievements: not implemented (dropped in
  docs/requirements.md).
- The menu's scaling choice is per session; persisting it per game is a UI/settings decision.
- `GET_CURRENT_SOFTWARE_FRAMEBUFFER` is refused (write-combined scanout buffers); a cached-surface variant could save
  one copy for some cores later.

## 18. Benchmark (N64)

**Why.** Which renderer / frameskip / Count Per Op / resolution is best differs per game and has to be measured on
the device; the owner has no UART, so the whole flow runs from the in-game menu and writes its results to the SD card.

**Plan.** `<coreopts ship dir>/<system>.bench.ini` (then `<core>.bench.ini`): `[bench] seconds, warmup` and one
section per configuration (`label`, `core` = core id, default the running one, and any core option or `rsos-` host
key). Configurations whose core is not installed (or does not list the system) are left out. The shipped
`n64.bench.ini` has 10 (docs/cores.md "N64 performance"); each lists every key the plan varies, so a user's per-game
value cannot leak into a run.

**Processes.** Every configuration needs a fresh core (renderer, resolution, dynarec and Count Per Op are read at
start, and cores do not survive a second load), so every run is a new game process:

```
game process (pid P)              menu "Start benchmark": state_save_to(<tmp>/bench/start.state) + .png,
                                  <tmp>/bench/plan.ini (the configurations + [run]: game, core, state, report
                                  path, .state.auto path), SRAM flush, display/GL/input released, then
  exec  -->  driver (pid P)       --bench-driver PLAN: no display, no core. For each configuration:
                                    fork + exec  --> step: <original args> --core <core> --bench-step PLAN I
                                    (PR_SET_PDEATHSIG: the UI's hang killer takes it down with the driver)
                                    waitpid; result-I.ini from the step, or crash / hang / abort / error
                                    rewrite /data/rsos/logs/bench-<game>-<time>.txt (partial results survive)
  exec  -->  game process (pid P) <original args> --load-state-file start.state --bench-report PLAN:
                                  the results page (menu), "Use this for this game"
```

The pid never changes, so the UI keeps waiting for the same child and its signals still arrive: SIGTERM stops the
run and exits; `RSOS_SIG_POWEROFF` stops the run, copies the start state (and its thumbnail) to the game's
`.state.auto` (the game resumes at the moment the benchmark started) and exits 3; the status pipe gets `poweroff`.
The steps get the original arguments minus `--status-fd` (no messages to the UI from them) and minus the resume,
dump and benchmark ones (`bench_filter_args()`).

**A step** (`--bench-step PLAN I`): the configuration's keys become session overrides (layer 5, never saved), SRAM
and states are read-only (`saves_set_readonly()`), no audio (the ALSA stream is never opened), pacing is free-run
(no vsync, no audio clock: the zero-copy present uses the no-wait path, so speed can exceed 100 %), the core gets
no input, the only hotkey is Select+Start (exit `BENCH_EXIT_ABORT` = 10: the driver stops), no battery decisions.
It loads the start state after one frame when it was written by the same core; otherwise (mupen64plus-next) it runs
from boot and its result is marked `from_boot` (listed, not ranked). After `warmup` seconds it snapshots the
counters, measures for `seconds`, then writes `result-I.ini`: speed %, fps, effective fps, VIs per second, frame
time average / p95 / p99 (every loop iteration of the window), core ms, GLES swap / present / readback ms, process
CPU %, per-CPU busy %, runs, frames delivered / presented / skipped, dupes; and the last picture as
`bench-<game>-<time>-NN-<id>.png` next to the report, flagged `blank` when 97 % of it is one colour (a broken
renderer can look very fast).

**Ranking** (`bench_rank()`): runs that completed, from the start state, with a picture; first those at full speed
(≥ 98 %), by effective fps (new frames per VI × the game's rate × min(1, speed): what is seen at real speed), then
speed; then the others by speed. "Use this for this game" (`bench_apply()`) merges the winner's keys into
`/data/rsos/coreopts/<core>/<game>.ini` (other keys kept) and, when the winner runs on another core, sets
`[<system>/<rom stem>] core = <core>` in `/data/rsos/cores.ini` (the per-game core choice `coreinfo_pick()` reads).
It applies at the next start (the running core is not told: renderers cannot change live).
