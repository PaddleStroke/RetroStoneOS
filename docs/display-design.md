# Display layer design

The display layer lives in `frontend/src/display.{c,h}` (atomic KMS, libdrm and libc only) and
`frontend/src/uevent.{c,h}` (raw netlink hotplug listener). `rsos-kmstest` (`frontend/src/tools/kmstest.c`) is the
bring-up tool, and `rsos-display-selftest` (`frontend/tests/display_selftest.c`) checks the pixel paths.

Goals:
- One output at a time: HDMI when it is plugged in, else the LCD. **Switch, never mirror.**
- The game image stays at the core's native resolution in a small buffer. The display engine scales it to the screen,
  with no CPU or GPU cost.
- Page flips are vsync-locked, non-blocking, driven by page-flip events. No tearing.

Kernel source references are to **unpatched v6.18.54** (`~/rsos/src/linux-6.18`), in `drivers/gpu/drm/sun4i/` unless
noted. See also `docs/kernel-patches.md`, section "Display scaling on A20".

## 1. Pipeline

```
               CRTC 0 (index 0)                                  CRTC 1 (index 1)
  DRAM ---> [fe0 DEFE scaler] ---+                  DRAM ---> [fe1 DEFE] (never used: see 5.4)
     \                           v                     \
      +---> [be0 DEBE: 4 layers, 2x/4x int scaler*] --->  +---> [be1 DEBE: 4 layers] ---> TCON1
                      |                                                                    |
                      v                                                                    |
                    TCON0 --ch0--> panel-dpi 640x480 RGB888 (connector "Unknown-1")        |
                      |               + pwm backlight via drm_panel                        |
                      +--ch1--+                                                            |
                              v                                                            |
                   [HDMI encoder, TMDS] <---------------- ch1 ------------------------------+
                              |          (mux in TCON0, sun4i_a10_tcon_set_mux(), sun4i_tcon.c:1343)
                              v
                         HDMI-A-1 (HPD polled; patch 0001: every 500 ms)

  * backend integer scaler: kernel patch 0003 (experimental)
```

- The game goes on the **primary plane** (layer 0, zpos 0, opaque) of the active CRTC. The primary plane doesn't have
  to cover the CRTC: sun4i has no plane `atomic_check`, so nothing forces a full-screen primary. Areas outside it show
  the backend background colour (letterboxing at no cost; see the checklist, the driver never writes
  `SUN4I_BACKEND_BACKCOLOR_REG`, `sun4i_backend.h:36`). Overlay planes 1-3 stay free for the UI. At most one of them
  may have alpha, and the bottom plane must be opaque (`sun4i_backend.c:567-578`).
- **The CRTC is never hardcoded.** The display layer reads each connector's encoders and ORs their `possible_crtcs`.
  The RGB encoder is tied to TCON0 (`sun4i_rgb.c:218`). The HDMI encoder gets both CRTCs
  (`drm_of_find_possible_crtcs()`, `sun4i_hdmi_enc.c:612`, with `hdmi_in_tcon0`/`hdmi_in_tcon1` in
  `sun7i-a20.dtsi:673-680`).
- Connector classification: HDMI, DVI, DP, VGA and TV types count as external. Everything else (DPI, LVDS, eDP, DSI,
  Virtual, **Unknown**) counts as internal. Note that `sun4i_rgb.c:225` registers the panel connector as
  `DRM_MODE_CONNECTOR_Unknown`, not DPI.

### CRTC choice
**With `panel_keep_scanning` (the RetroStone2, §8.5)** the panel keeps CRTC 0 (TCON0) for good, so HDMI runs on
**CRTC 1 = be1 + TCON1 channel 1**, and `choose_crtc()` never gives another output the panel's CRTC
(`hdmi_crtc_index` pointing at it is ignored with a warning; an HDMI connector that could only use it is not used).
be1 gets its scaled planes from fe0 (`sun4i_backend_find_frontend()`, below), whose output goes to BE0 unless its
`OUT_PORT_SEL` field says otherwise: **kernel patch 0006** sets it to the backend that uses the frontend
(docs/kernel-patches.md). TODO(hw): the scaled game on HDMI via CRTC 1.

Without the quirk (other boards): `choose_crtc()`, in order: `hdmi_crtc_index` if forced (`rsos-kmstest --hdmi-crtc
N`), then **the CRTC already in use**, then the LCD's CRTC, then the lowest usable one. So HDMI runs on **CRTC 0 = be0
+ fe0 + TCON0 channel 1** there. The reason is `sun4i_backend_find_frontend()` (`sun4i_backend.c:735-763`, "TODO: This
needs to take multiple pipelines into account"): it returns the first frontend on the backend's input port, and both
`be0` and `be1` list `fe0` first in `sun7i-a20.dtsi`. So be1 gets fe0 too, and the unpatched driver never programs the
fe0 output port (BE0 at reset). Keeping HDMI on be0 keeps the one tested frontend pairing. TCON0 channel 1 to HDMI is
allowed by the DT graph and `sun4i_a10_tcon_set_mux()`, but it disables TCON0 channel 0, i.e. the panel's signals.

## 2. Hotplug detection

- **No udevd.** `uevent.c` opens a `NETLINK_KOBJECT_UEVENT` socket bound to multicast group 1 (raw kernel events),
  non-blocking and close-on-exec, with a 1 MB receive buffer (`SO_RCVBUFFORCE`, falling back to `SO_RCVBUF`).
  It only accepts messages from the kernel (sender port id 0, so udevd's `libudev` packets and spoofed senders are
  ignored). It parses `ACTION@DEVPATH\0KEY=VALUE\0...` and extracts ACTION, DEVPATH, SUBSYSTEM, DEVNAME, DEVTYPE,
  HOTPLUG, CONNECTOR, PROPERTY and SEQNUM. A socket overflow (`-ENOBUFS`) is treated as a hotplug, because the state
  is re-probed anyway. Tested in WSL with synthetic uevents (`echo change > /sys/class/mem/null/uevent`).
- A DRM hotplug is `SUBSYSTEM=drm`, `ACTION=change`, `HOTPLUG=1`, with `DEVNAME` matching our card. With patch 0001
  the HDMI driver samples HPD every 500 ms and sends `HOTPLUG=1 CONNECTOR=<id>` when the line changes. Without it, the
  core output poll runs every 10 s.
- **Debounce.** Each uevent (re)arms a one-shot `timerfd` (`debounce_ms`, default 250 ms). When it fires, the
  connectors are probed **once**. External connectors get a full `drmModeGetConnector()` (detect plus an EDID read
  over DDC, about 25-50 ms, only when connected). Internal connectors are probed only at init, since panel modes are
  static.
- **Spurious events.** The decision is made from the probed state, not from the event. If the chosen output and mode
  are unchanged, the event is logged as "no change" and nothing happens. A cable that bounces during insertion gives a
  burst of uevents that the debounce collapses into one probe.
- **Missing EDID.** With a connected HDMI and no EDID blob (or no modes), the kernel only offers its no-EDID DMT list
  up to 1024x768 (`drm_probe_helper.c:651-653`), which many TVs refuse. The display layer then uses the built-in CEA
  timing of the policy mode (VIC 4 720p60, or VIC 1 480p60), and re-probes up to `edid_retries` (2) times,
  `edid_retry_ms` (1 s) apart. If the EDID shows up and the chosen mode differs, it re-modesets (`REPROBE`).
- **Startup with HDMI connected** goes through the same probe, so it starts on HDMI.
- **Which connector is the built-in screen** comes from the board profile (`display_config.internal_mode` /
  `internal_types`, board.ini `internal_display`, docs/porting.md): by default every connector type that is not
  HDMI/DVI/DP/VGA/TV (the RetroStone2's panel is "Unknown-1"); a list of types; or none. An analog TV type in the
  list is the built-in screen: the RetroStone1's "Composite-1" (H3 TV encoder -> AMT630A), see §3.2. **A board without a built-in
  screen** (Raspberry Pi 4) that has nothing connected lights the external connector in use, else the first one, with
  the CEA timing of the policy mode, so the menu runs; a TV plugged in later is a normal hotplug (same connector: a
  `REPROBE` with its EDID mode; the other port: a switch). This never happens on a board with a panel connector.
- A failed external modeset marks the connector `failed` (it is skipped until its status changes), and the layer
  falls back to the LCD (`DISPLAY_EVENT_FALLBACK`).
- `display_get_fd()` returns an **epoll fd** that groups the DRM fd, the uevent socket and the timerfd, so the future
  UI and libretro host have one fd to poll. `display_handle_events()` services all three without blocking.

## 3. Mode policy

| Output | Policy |
|---|---|
| LCD | preferred mode, else the first one |
| Built-in composite screen (RetroStone1) | the `tv_norm` mode: 720x480i NTSC (default) or 720x576i PAL, each the fallback of the other (§3.2) |
| HDMI (default) | **1280x720**, progressive, refresh closest to 60 Hz (`hdmi_width/height/refresh`), else the preferred mode, else the first one |
| HDMI "native 4:3" option | `hdmi_width/height = 640x480` (CEA VIC 1, which every sink must accept). Same geometry as the LCD: 320x240 cores get the sharp backend 2x, and the TV upscales. Not the default. |
| HDMI without EDID | built-in CEA timing of the policy mode (720p60 or 480p60) |
| Any output | `force_width/height` (`rsos-kmstest --mode WxH[@HZ]`) wins when the output offers that size |

Why 720p rather than 1080p: 1080p60 is a 148.5 MHz pixel clock versus 74.25 MHz. Any full-screen framebuffer (UI
planes, the software fallback) costs 1920x1080x4x60 = 498 MB/s of DDR3 reads instead of 221 MB/s. The hardware-scaled
game plane itself only reads its small source buffer (about 18 MB/s for 320x240 XRGB8888 at 60 Hz, whatever the
output size), which is another reason to scale in hardware.

The panel timings in the DTS (33 MHz, 800x525 total) give **78.6 Hz**, not 60 Hz. Page flips follow the panel, so
cores that run at 60 Hz would need frame pacing on their own clock. `display_output()->refresh_mhz` reports the exact
rate, and the libretro host uses it. Section 3.1 runs the same panel at 60 Hz without a reboot.

### 3.1 LCD refresh: a live 60 Hz mode (2026-09-27)

At 78.571 Hz every ~60 Hz core runs on the audio clock (host-design.md §8), with judder. A true 60 Hz lets the
vsync-locked pacing (vsync + DRC) work. The `lcd60` DT overlay (25.2 MHz) needs `fw_setenv` and a reboot and was
never tried; the display layer can do the same **live**, as a user mode:

- **The mode** (`mode_retime()`): the panel's DT mode with only the pixel clock changed, `clock = htotal x vtotal x
  hz` = 800 x 525 x 60 = **25 200 kHz**, `vrefresh` 60, type `USERDEF`, same porches, syncs and flags, so
  `refresh_mhz()` = **60 000 mHz exactly**. It is committed in the usual full-state atomic commit (a new `MODE_ID`
  blob, `ALLOW_MODESET`).
- **What the kernel does with it** (unpatched 6.18.54): `sun4i_rgb_mode_valid()` (the RGB encoder's `mode_valid`,
  also called from the atomic check for a user mode) returns `MODE_OK` early for a panel ("RGB panel used,
  skipping clock rate checks"); `sun4i_crtc_atomic_check()` only checks the backend planes; the modeset runs
  `sun4i_tcon0_mode_set_rgb()`, which does `clk_set_rate(tcon->dclk, mode->crtc_clock * 1000)`. The TCON0 dot clock
  (`sun4i_dclk_round_rate()`) tries dividers 4..127 (`sun7i_a20_tcon0_quirks.dclk_min_div = 4`) and takes the first
  exact one; its parent `tcon0-ch0-sclk` (`CLK_SET_RATE_PARENT`) is a mux over pll-video0/1 and their 2x, which
  run at 3 MHz x 9..127 (24 MHz / 8, `ccu-sun4i-a10.c`) or 270/297 MHz. So **25.2 MHz = pll-video 126 MHz / 5,
  exactly** (the 33 MHz of today is 132 MHz / 4). Expected refresh: **60.000 Hz**. `lcd_clock_model()` reproduces
  this (unit-tested) and the switch logs it: `LCD retimed to 60 Hz: pixel clock 25200 kHz (A20 TCON0: pll-video 126
  MHz / 5 = 25.200 MHz expected, 60.000 Hz)`. The pll-video is not shared with the GPU (lima runs at 384 MHz from
  another PLL; pll-video tops out at 381 MHz) and HDMI re-programs it when it takes over.
- **Measured refresh**: after every modeset the display measures the real rate from the page-flip events (the
  vblank sequence and kernel timestamp each event carries) over >= 2 s and 60 vblanks, restarting after a gap of more
  than 3 s between events (sun4i has no hardware vblank counter: while the vblank interrupt is off the count stands
  still), and logs it once: `refresh measured on Unknown-1: 60.001 Hz (121 vblanks in 2.016 s; mode 60.000 Hz)`,
  with a warning when it is more than 0.5 % off the mode (then DRC could not hold vsync). Also in
  `display_output()->measured_mhz`. Every boot logs it for 78.571 Hz too.
- **API**: `display_config.lcd_refresh_hz` (0 = the panel's own mode, 30..120 = retimed), applied in `pick_mode()`
  for the internal connector only (never with `--mode`); `display_set_lcd_refresh(hz)` changes it live: on the LCD a
  `do_switch()` of the same connector (reason `REPROBE`: `on_audio` RELEASE/ACQUIRE and `on_output`, so the host
  re-evaluates its pacing); on HDMI it applies the next time the LCD is chosen; suspended or off, at resume / screen
  on. A refused commit keeps the previous mode and returns the error; at init, a refused retimed mode falls back to
  the panel's mode (`LCD at 60 Hz refused (...): back to the panel's mode`), so a bad setting can never leave the
  unit without a picture. `display_parse_lcd_refresh()` maps the settings value (`"60"` -> 60, else 0).
- **Setting** `lcd_refresh = 60 | 78` in `/data/rsos/settings.ini`, **default 78** until the owner confirms 60
  works. Settings > Display > "LCD refresh rate: 78 Hz (legacy) / 60 Hz" (ui-design.md): choosing 60 Hz applies it at
  once and shows "The screen now refreshes at 60 Hz. Keep this setting? Reverting in 15 s" [KEEP] [REVERT]
  (REVERT selected, B = revert, the countdown reverts by itself: a panel that shows nothing comes back after 15 s);
  only KEEP saves it. Going back to 78 Hz applies and saves at once. The frontend reads the key at every display init;
  the game children get the value in use as `--lcd-refresh 60|0` (the host reads the key itself when started
  alone), so they commit the same mode: no modeset at launch, and pacing sees 60.000 Hz.
- `rsos-kmstest --lcd-refresh 60` shows the pattern at 60 Hz (FPS 60.0) for a quick test from the UART.
- TODO(hw): the panel at 25.2 MHz (stable picture, no flicker, colours); the measured refresh line; NES/SNES in
  `vsync+DRC` pacing; then make 60 the default (one line in `lcd_hz_now()`/`display_parse_lcd_refresh()` callers)
  or change the DT.

### 3.2 A composite (TV) output as the built-in screen: the RetroStone1 (2026-09-28)

The RetroStone1's screen is fed by the H3 composite output (mixer 1 -> TCON1 -> TV encoder, the RetroStone1 kernel
patch 0002, docs/kernel-patches.md) through an AMT630A CVBS-to-RGB converter. The kernel connector is
**"Composite-1"**, with two modes (`drm_connector_helper_tv_get_modes()`): **720x480i** at 59.94 Hz (NTSC, 13.5 MHz,
858 x 525) and **720x576i** at 50 Hz (PAL, 864 x 625), plus the connector property **"TV mode"** (NTSC, PAL).
Nothing here is used on the RetroStone2.

- **Internal.** An analog TV connector type named in `internal_types` (board.ini `internal_display = composite`) is
  classified internal (`DISPLAY_OUTPUT_LCD`); without the list, or on another board, analog TV stays external.
  HDMI hotplug switches between it and HDMI exactly as between the RetroStone2 LCD and HDMI (same `choose_output()`,
  `do_switch()`, audio hooks: the codec on the built-in screen, HDMI on the TV). sun4i_tv has no `detect()`, so the
  connector always reads connected; it is probed once at init like a panel.
- **Mode** (`pick_mode()`): `display_config.tv_norm` (board.ini `tv_norm`): NTSC (the default: 59.94 Hz fields,
  the lower game latency and the rate of most cores), PAL, or AUTO (the kernel's preferred mode: NTSC, or PAL with
  `video=Composite-1:PAL` in `rsos_extraargs`). A missing mode falls back to the other standard; a refused NTSC
  modeset is retried in PAL once (`NTSC refused on Composite-1 (...): trying PAL`). Interlaced modes are never
  retimed (`lcd_refresh` does not apply); the no-EDID CEA fallback of HDMI is never used on an analog connector.
- **TV mode property.** sun4i_tv encodes the connector's "TV mode", not the CRTC mode, so every modeset on the
  connector also sets "TV mode" to NTSC for a 480-line mode and PAL for a 576-line one (`commit_state()`).
- **Interlace.** The planes stay in frame coordinates (720x480); the DE2 mixer outputs the fields itself
  (`SUN8I_MIXER_BLEND_OUTCTL_INTERLACED`). A 320x240 core scaled x2 vertically puts each source line on both fields,
  so it does not flicker. `display_output()->interlaced` is set; the host draws its FPS strip at 2x there (1-pixel
  strokes would only be drawn every other field). `refresh_mhz` is the field rate (59 940 / 50 000 mHz), which is
  also the page-flip rate the pacing sees.
- **Aspect ratio.** The picture is 4:3 whatever the line length, so a screen pixel is (4/3) / (W/H) wide: 0.889 in
  NTSC, 1.067 in PAL (`geo_for()`, `display_output()->pixel_aspect`). `compute_scaled()` works on the physical width
  `W x pixel_aspect`: ASPECT keeps the image ratio on the panel, INTEGER keeps square source pixels (the factor is
  taken on the physical width: 320x240 is x2 = 720x480, 256x224 is 576x448 at 72,16). The menu lays its UI out at
  `width x pixel_aspect` (640x480) and the plane scales it to 720x480. Panels and HDMI keep `pixel_aspect` 1.0 and
  the exact old geometry (unit-tested).
- **Overscan.** `display_config.tv_overscan` (board.ini `tv_overscan`, percent per edge, 0-20, default 0) insets the
  area the scaled planes and the overlay use (`display_output()->overscan_x/y`); the AMT630A's own OSD also has
  position and size settings. The software fallback (`PATH_SOFT`) ignores the pixel shape and the inset.
- **Planes of mixer 1.** The H3 DE2 mixer 1 has only two planes: a VI plane (XRGB8888/RGB565, no alpha formats,
  zpos 0, scaled) and the primary UI plane (ARGB8888, zpos 1, scaled). With the game on the primary plane there is
  no ARGB8888 overlay left for the FPS counter, so `crtc_pick_planes()` puts the game on the VI plane below and the
  overlay on the primary plane. It only does that for a CRTC with no ARGB8888 overlay plane and a lower XRGB8888
  overlay plane (by `zpos`): the A20 backends and the H3 mixer 0 (HDMI) keep the game on their primary plane. The
  RetroStone1 has no battery gauge, so its overlay only ever shows the FPS counter.
- `rsos-kmstest --tv ntsc|pal|auto` treats the composite output as the built-in screen (for tests from the UART).
- TODO(hw): everything above on a RetroStone1 (docs/boards.md, "RetroStone1").

## 4. Switching sequence

On a debounced hotplug, `reprobe()` → `choose_output()` → `pick_mode()` → `do_switch()`:

1. **Audio release.** `on_audio(DISPLAY_AUDIO_RELEASE, old_output)` runs *before* anything changes, so the audio layer
   closes the HDMI PCM (the `sun4i-hdmi` card, patch 0002) while the encoder is still up.
2. **Drain** the in-flight page flip (at most one frame, 100 ms cap), so no flip event is pending for the old pipeline.
   The frame generation counter is bumped after the switch. A late event carries the old generation in its
   `user_data` and is ignored.
3. **One atomic commit** with `DRM_MODE_ATOMIC_ALLOW_MODESET`, validated with `TEST_ONLY` first. It writes the
   *complete* state:
   - every connector: `CRTC_ID` = the chosen CRTC for the target, 0 for all others (without `panel_keep_scanning` the
     LCD connector goes off, so drm_panel disables the panel and the PWM backlight; **with it, the panel connector
     stays on CRTC 0**, §8.5);
   - every CRTC: `ACTIVE` = 1 and `MODE_ID` = the new mode blob for the chosen one, `ACTIVE` = 0 and `MODE_ID` = 0 for
     the others (**except the kept panel's CRTC: `ACTIVE` = 1, its own mode**);
   - every other plane: `FB_ID` = 0, `CRTC_ID` = 0 (the kept panel's primary plane: the black frame);
   - the game plane: `FB_ID`, `CRTC_ID`, `SRC_*` (game size, 16.16), `CRTC_*` (the scaled rect for the new screen).
   So the new output shows the current game frame, already scaled for its size, on its first frame. There is no black
   intermediate commit.
4. The **scaling strategy is re-picked** for the new screen size inside that commit (section 5). Game-sized buffers are
   reused as is. Only the fallback's screen-sized buffers are reallocated, before the commit, and the old ones are
   freed after it.
5. If the external modeset fails, the layer falls back to the LCD (and tells the audio layer to resume if nothing
   changed).
6. **Callbacks:** `on_output(now, before, reason, timing)` and `on_audio(DISPLAY_AUDIO_ACQUIRE, now)`.

Every step is logged with CLOCK_MONOTONIC timestamps. `display_switch_timing` carries the first uevent, probe and
commit times, and kmstest prints `LATENCY first uevent -> modeset done`. Expected plug-to-picture time: HPD sampling
(≤ 500 ms, 250 ms average) + debounce 250 ms + EDID ~40 ms + modeset 1-3 frames (+ TV resync, which is outside our
control). That is about 0.35-0.8 s until our commit returns.

## 5. Scaling

### 5.1 What the hardware and driver accept

| Path | Formats | Scaling | Source |
|---|---|---|---|
| Backend only, unpatched | ARGB1555, ARGB4444, ARGB8888, RGB565, RGB888, RGBA4444, RGBA5551, UYVY, VYUY, XRGB8888, YUYV, YVYU | **none**: a backend plane with src ≠ dst is rejected | `sun4i_backend.c:142-155`, `:451-453` |
| Backend + patch 0003 | the RGB formats above | x1, x2, x4 per axis, nearest neighbour, sharp; no count limit | `board/retrostone2/patches/linux/0003-*` |
| Frontend (DEFE) | **XRGB8888, BGRX8888**, YUV | any ratio, 4-tap H / 2-tap V filter (soft); **1 plane per CRTC** | `sun4i_frontend.c:364-383`, `sun4i_backend.h:166`, `sun4i_backend.c:601-604` |

- The choice between backend and frontend is the driver's (`sun4i_backend_plane_uses_frontend()`,
  `sun4i_backend.c:408-439`): a format supported by both, scaled, goes through the frontend (with 0003: unless the
  ratio is 1/2/4). **RGB565 can never be frontend-scaled.** The frontend always outputs XRGB8888 to the backend
  (`sun4i_layer.c:99-102`) and drops alpha, so ARGB8888 surfaces are scanned out as XRGB8888.
- There is no plane clipping, so the dest rect is always kept inside the mode. Source rectangles are whole pixels
  (fractional 16.16 parts are ignored by the driver).
- Every flip of a frontend plane reprograms the scaler (`sun4i_layer.c:95-103`). This is fine, and changing only
  `FB_ID` keeps flips cheap.

### 5.2 Strategy selection

`display_set_game_surface()` and every output switch run the try list, **best first**, each attempt validated with a
`TEST_ONLY` commit before the real one (`build_tries()`, `apply()`):

1. **Native format, hardware-scaled, zero-copy.** XRGB8888/ARGB8888 surfaces: any ratio (backend for 1/2/4 with 0003,
   frontend otherwise). RGB565 surfaces: an RGB565 plane, accepted only when the ratio is 1/2/4 on both axes (backend
   integer scaler, patch 0003). The core or UI draws straight into the scanout buffers.
2. **XRGB8888 plane, frontend-scaled, converted during the copy** (RGB565 or 0RGB1555 surfaces). The conversion runs in
   the copy the host does anyway (section 5.5).
3. **Software fallback** (section 7): a screen-sized XRGB8888 buffer, with the image CPU-scaled by an integer factor
   (≤ `soft_scale_max`, default 2) or plain unscaled, centered.

The winner is **cached** per (CRTC, mode size, frame size, surface format, flags, dest rect). Known failures are skipped
on the next switch or frame-size change. If a cached winner ever fails, the entry is dropped and the whole list is
tried again.

### 5.3 Scale modes and geometry (`compute_scaled()`)

- `DISPLAY_SCALE_ASPECT` (default): the largest rect with the image aspect (the core's `aspect_ratio`, or w/h), centered.
- `DISPLAY_SCALE_INTEGER`: the largest integer factor, square pixels, centered (falls back to ASPECT if the image is
  larger than the screen).
- `DISPLAY_SCALE_STRETCH`: full screen.

What that gives, with the strategy that should win (patch 0003 applied):

| Frame | Output | Mode | Plane rect | Strategy |
|---|---|---|---|---|
| 320x240 RGB565 | LCD 640x480 | aspect | 640x480+0+0 | 1: RGB565 backend x2, zero-copy, sharp |
| 320x240 XRGB8888 | LCD | aspect | 640x480 | 1: backend x2 (0003) or frontend |
| 256x224 RGB565, 4:3 | LCD | aspect | 640x480 | 2: XRGB8888 frontend (2.5x2.14), converted |
| 256x224 RGB565 | LCD | integer | 512x448+64+16 | 1: RGB565 backend x2 |
| 320x240 RGB565 | HDMI 720p | aspect | 960x720+160+0 | 2: frontend x3, converted |
| 160x144 RGB565 | HDMI 720p | integer | 800x720+240+0 | 2: frontend x5, converted |
| 320x240 RGB565 | HDMI 640x480 | aspect | 640x480 | 1: backend x2, zero-copy |

Without 0003, every RGB565 row becomes strategy 2, and XRGB8888 always uses the frontend. With the frontend,
"integer" scaling is still filtered (soft). Only the backend x2/x4 is pixel-exact.

### 5.4 Frame-size changes (PSX)

`display_set_game_surface(max_w, max_h, fmt)` allocates buffers for the core's `max_width` x `max_height` (PSX:
640x480). The current frame size can then change at any frame, with `display_set_frame_size(w, h)` or directly through
`display_present_frame(data, w, h, pitch)`:

- **No reallocation, no modeset.** Each buffer remembers the geometry it was drawn for (`fbuf.fw/fh/dst`). The first
  flip of a new size carries `SRC_W/SRC_H` and `CRTC_X/Y/W/H` in the same non-blocking commit as its `FB_ID`, so the
  new size and its first frame reach the screen on the same vblank. Later flips go back to `FB_ID` only.
- The new geometry is checked against the strategy cache, or with one `TEST_ONLY` commit the first time. Only if the
  current strategy cannot do it (for example RGB565 at 368x240 on the LCD is no longer an exact x2) is the strategy
  re-picked with a blocking commit (one frame).
- On the software path, the centered rect and factor are recomputed, and the borders are re-cleared once per buffer.

### 5.5 Pixel formats and conversion

Surface formats: `DRM_FORMAT_RGB565`, `DRM_FORMAT_XRGB1555` (libretro 0RGB1555, the libretro default),
`DRM_FORMAT_XRGB8888` and `DRM_FORMAT_ARGB8888`. The conversion to XRGB8888 is done line by line in `convert_line()`:
**NEON**, 8 pixels per iteration (`vld1q_u16`, `vshrn`/`vmovn` to put each channel at the top of a byte, `vsri` to
replicate the top bits into the low bits (`x << 3 | x >> 2`), `vst4_u8` to interleave B,G,R,X). The scalar loop
handles the tail and non-NEON builds. `rsos-display-selftest` checks all 65536 RGB565 and 32768 0RGB1555 values
against the reference formula for several widths, which exercises the NEON body and the scalar tail. It also times a
320x240 frame. The host (scalar, x86) needs 0.02 ms. On the A7 the NEON loop is bound by the write-combined stores
(300 KB per frame), expected well under 1 ms (to measure on the device, see the checklist).

The copy the host needs anyway is where the conversion happens. The core renders into its own (cached) buffer,
`display_present_frame()` converts straight into the scanout buffer, and nothing else touches the pixels.

## 6. Buffer strategy

- **Dumb buffers** (CMA), mapped **write-combined**. Writes are sequential whole lines, and nothing ever reads a
  scanout buffer. Cores that read their framebuffer back must render into cached memory, either their own buffer (the
  libretro norm) or the surface with `DISPLAY_SURFACE_CACHED` (a shadow buffer copied at present time).
- **Double buffering by default** (`buffers = 2`), **triple as an option** (`buffers = 3`). The reasons:
  - With double buffering, `display_begin_frame()` returns immediately if the previous flip is done, and waits (at
    most one refresh) for its page-flip event otherwise. That wait is the natural vsync pacing of the emulator loop,
    with the lowest latency (the frame drawn is shown at the next vblank).
  - Triple buffering lets a core that finishes a frame early start the next one without waiting, which absorbs
    frame-time jitter (SuperFX, PSX) at the cost of up to one more frame of latency. Frame states are FREE, DRAW,
    QUEUED, PENDING and FRONT. A frame presented while a flip is in flight is QUEUED and flipped from the flip event.
    A newer frame replaces a QUEUED one (counted as `dropped`).
  - Memory is not the issue: 320x240x4 = 300 KB per buffer (640x480 PSX capacity: 1.2 MB). The fallback's screen-sized
    buffers are 1.2 MB (LCD) or 3.7 MB (720p) each.
- **No tearing.** Each flip is an atomic commit (`NONBLOCK | PAGE_FLIP_EVENT`) that changes `FB_ID` (and, when the
  frame size changes, the plane rects). The plane state pulls in its CRTC (`drm_atomic.c:558-564`), so the event is
  delivered per CRTC. The backend latches its registers at vblank (`REGBUFFCTL.LOADCTL`, `sun4i_backend_commit()`,
  and `atomic_begin` waits for the previous load). Modesets are blocking commits. Buffers are only destroyed after the
  commit that replaced them has completed: `drmModeRmFB` on a displayed framebuffer would disable the plane.

## 7. Fallback path

Used when `TEST_ONLY` rejects both hardware strategies (for example on a kernel without the frontend bound), or when
forced (`no_hw_scale`, `rsos-kmstest --no-scale`):

- Screen-sized XRGB8888 buffers, full-screen primary plane, `SRC` = `CRTC` = screen (no scaler at all, always accepted
  by the backend).
- The image is scaled by the CPU by the largest integer factor that fits, capped by `soft_scale_max` (default 2: the
  320x240 LCD case fills the screen; 1 = the plain unscaled blit), nearest neighbour, centered. Each source line is
  converted once into a line buffer, widened in place, then written n times. Larger images are cropped around the
  center.
- The borders are cleared once per buffer (when the geometry changes), not per frame.
- The caller's view is the same API. `surface->direct` is false, and `buffers[0]` is a cached shadow in the caller's
  format. Switching outputs keeps the shadow, so nothing is lost.
- Without a game surface (the UI hasn't drawn yet), the CRTC runs with no plane (backend background). If the driver
  refuses that, a black full-screen buffer is used.

## 8. API summary (`display.h`)

| Function | Purpose |
|---|---|
| `display_config_defaults(&cfg)` / `display_init(&cfg)` | open the first atomic KMS card (or `cfg.device`), set `UNIVERSAL_PLANES` + `ATOMIC`, open the uevent socket, probe, light up HDMI if connected, else the LCD |
| `display_get_fd()` | epoll fd (DRM events, uevents, debounce timer) for poll loops; `display_get_drm_fd()` gives the raw DRM fd |
| `display_handle_events()` / `display_wait_events(ms)` | page flips, uevents, debounce expiry, output switches |
| `display_set_game_surface(max_w, max_h, fmt, flags)` | allocate the surface and pick the strategy; returns `display_surface` (buffers with stride, frame and max size, `hw_scaled`, `direct`, `plane_format`, dst rect) |
| `display_set_frame_size(w, h)` | change the frame size inside the capacity, no modeset |
| `display_set_scaling(mode, aspect)` | aspect / integer / stretch, image aspect ratio |
| `display_begin_frame(timeout)` | index of the buffer to draw into; waits for vsync when both buffers are busy |
| `display_present()` | queue the drawn frame for the next vblank |
| `display_present_copy(src, pitch)` / `display_present_frame(src, w, h, pitch)` | libretro-style copy + convert + present |
| `display_output()` | output type (LCD/HDMI), connector name, EDID monitor name, mode, exact refresh (mHz), measured refresh (mHz, §3.1), CRTC and plane |
| `display_set_lcd_refresh(hz)`, `display_get_lcd_refresh()`, `display_parse_lcd_refresh(v)` | the LCD at 60 Hz (25.2 MHz user mode) or its own 78.6 Hz mode, live (§3.1) |
| `cfg.on_output` | output changed: new/old info, reason (init, hotplug, reprobe, fallback), timestamps |
| `cfg.on_audio` | `RELEASE` before the modeset (close the HDMI PCM), `ACQUIRE` after it (open the new output's PCM) |
| `display_get_stats()`, `display_now_ms()` | flips, dropped frames, switches, last flip time |
| `display_present_fb(&fb, timeout)` | zero-copy: an external framebuffer (GBM BO) on the game plane, scaled and letterboxed like the surface (section 8.1) |
| `display_suspend()` / `display_resume()` | DRM master hand-off to the game process without closing the fd (section 8.2) |
| `display_set_active(bool)`, `display_is_active()` | screen off/on for the menu's idle screen-off, keeping the configuration: CRTC `ACTIVE`, or backlight off + black frame on a panel kept scanning (sections 8.3, 8.5) |
| `display_set_panel_picture(xrgb, w, h, bg)` | the picture a panel kept scanning shows behind HDMI, backlight off: the boot logo (section 8.5) |
| `display_set_overlay(argb, w, h, corner, margin)`, `display_hide_overlay()`, `display_overlay_visible()`, `display_overlay_rect()` | a small ARGB8888 picture on its own overlay plane in a screen corner, unscaled (the in-game battery indicator, section 8.4) |

The layer is single-threaded: call everything from the thread that owns the display.

### 8.1 External framebuffers (`display_present_fb`, GLES2 zero-copy)

For HW-render cores (N64 on lima), the host renders into a GBM surface created on **`display_get_drm_fd()`**
(`gbm_create_device(display_get_drm_fd())`, `gbm_surface_create(w, h, GBM_FORMAT_XRGB8888,
GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)`), draws the core's FBO texture into it with one quad, then per frame:

```c
eglSwapBuffers(dpy, surf);
struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
uint32_t fb_id = fb_for_bo(bo);   /* drmModeAddFB2 once per BO, cached in gbm_bo_set_user_data */
struct display_fb f = {
	.fb_id = fb_id, .format = DRM_FORMAT_XRGB8888,
	.width = gbm_bo_get_width(bo), .height = gbm_bo_get_height(bo),
	.src = { 0, 0, frame_w, frame_h },          /* the core's current frame size */
	.release = release_bo, .user = bo,         /* release_bo: gbm_surface_release_buffer(gs, bo) */
};
int r = display_present_fb(&f, -1);           /* waits for the previous flip: vsync pacing */
if (r == -EINVAL) { gbm_surface_release_buffer(gs, bo); /* fall back to glReadPixels + display_present_frame() */ }
else if (r < 0) gbm_surface_release_buffer(gs, bo);  /* -EAGAIN (screen off), -ETIMEDOUT, -EINTR: frame not shown */
```

- **Plane and geometry.** The FB goes on the same plane as the game surface. The `CRTC_*` rect is
  `compute_scaled(src.w, src.h)` with the current scale mode and aspect (`display_set_scaling()`), so letterboxing and
  the aspect/integer/stretch settings are identical to software cores. `SRC_X/Y/W/H` come from `fb.src` (whole
  pixels; the driver ignores fractions). Every present sends the full plane state, because BOs may differ.
- **Checks.** The first present of a geometry (CRTC, format, FB size, src, dst) runs a `TEST_ONLY` commit. A refusal
  returns `-EINVAL` and the host keeps its readback path. On sun4i, **use XRGB8888**: it is the only RGB format the
  frontend scaler reads (section 5.1). Other formats only pass at x1/x2/x4 with patch 0003. There is no plane rotation
  or Y flip, so a `bottom_left_origin` image must be flipped in the GL quad.
- **Pacing.** Only one flip is ever in flight. `timeout_ms` = -1 waits for it (the vsync-locked loop, like
  `display_present_frame()`), 0 returns `-EBUSY` at once (the audio-clock mode: skip the frame, or check
  `display_flip_pending()` before rendering).
- **Ownership.** After a successful present, the display owns the FB until `release(fb_id, user)`. That happens when
  the flip that replaced it has completed (the next external FB, or a surface frame such as the in-game menu), or at
  `display_shutdown()`. Presenting the fb_id that is already on screen gives no extra release. A failed present never
  calls `release()`. With a 2-BO GBM surface this is exactly the "lock front / release after flip" cycle.
- **Mixing with the surface.** The host can switch to the surface at any time (menu, OSD frames), by presenting a
  surface frame. That flip re-sends the surface's plane rects, and its completion releases the external FB.
- **Output switch during a GL game.** The hotplug modeset commits the surface plan first (one frame of the surface's
  content, or nothing), then `ext_recommit()` immediately puts the external FB back, scaled for the new output (a
  blocking commit, before the switch returns). If the new output refuses it, the FB is released, and the next
  `display_present_fb()` fails its `TEST_ONLY`, so the host falls back to readback. The same happens on
  `display_set_scaling()`, `display_resume()` and `display_set_active(true)`.
- Shutdown order: call `display_shutdown()` (or present a surface frame) **before** destroying the GBM surface.
  `display_shutdown()` releases the FBs it still holds while the DRM fd is still open.

### 8.2 DRM master hand-off (`display_suspend` / `display_resume`)

```
UI (parent)                          game (child)
display_suspend()   drains the in-flight flip, drmDropMaster(); fd, buffers, state kept
host_launch() ----fork/exec-------> display_init(): the first open with no master becomes master
   ...                               ... game, hotplugs handled by the child ...
   waitpid <------------------------ exit: its fd closes, its FBs are removed
display_resume()    drmSetMaster(); drain the uevents queued meanwhile; probe connectors from the
                    kernel's cached state (drmModeGetConnectorCurrent: the child's probes and the HPD work
                    keep it current, so no EDID read unless a connected HDMI has no modes); if the output
                    or mode changed, do_switch() with the usual callbacks (reason HOTPLUG, audio
                    RELEASE/ACQUIRE); else one full-state ALLOW_MODESET commit with the newest frame.
```

- While suspended, the drawing calls return `-EAGAIN` (`display_set_game_surface()` returns NULL), and
  `display_handle_events()` only records that a hotplug happened. Nothing touches the hardware.
- `display_resume()` returns `-EBUSY`/`-EINVAL` from `drmSetMaster()` if the child still holds master: call it after
  `waitpid()`.
- When the child exits with its FB on the primary plane, the kernel disables that plane (`atomic_remove_fb()` in
  `drm_framebuffer.c`), and the CRTC too only if the driver refuses the plane-only commit (sun4i accepts a CRTC
  without planes, so the panel keeps scanning the backend background, §8.5). The resume commit re-commits everything
  anyway. It still skips the device open, enumeration and EDID reads of a full `display_init()`. TODO(hw): measure
  both (`rsos-kmstest --suspend-test` logs the times).
- `display_init()`/`display_shutdown()` around the child keep working exactly as before. The hand-off is only the
  faster variant. With `panel_keep_scanning`, `display_resume()` lights the panel again (alone first when the output
  is HDMI) and writes the backlight again (§8.5).

### 8.3 Screen off/on (`display_set_active`)

- Off **on the panel with `panel_keep_scanning`** (the RetroStone2, §8.5): the backlight goes off (`bl_power` = 4),
  then a full commit puts the black frame on the game plane and takes the overlay down; the CRTC stays `ACTIVE`, the
  panel keeps its signals. `ACTIVE` = 0 is never committed on its CRTC.
- Off on HDMI, or on a panel without the quirk: waits for the in-flight flip, then one blocking commit with `ACTIVE`
  = 0 on the current CRTC (`ALLOW_MODESET`). The mode blob, the connector routing and the planes stay in the atomic
  state. On a panel, the encoder disable goes through drm_panel (`sun4i_rgb.c`), which unprepares the panel and turns
  off its PWM backlight. On HDMI the signal stops: **close the HDMI PCM before** (the host's sleep path already closes
  ALSA first). The audio hook is not called here. A panel kept scanning behind HDMI has its own CRTC: untouched.
- While off: presents are not flipped. The newest frame is kept (QUEUED, newest wins), and `display_begin_frame()`
  recycles it instead of blocking. `display_present_fb()` returns `-EAGAIN`. A hotplug is only recorded.
  Switches, `ACTIVE` in every modeset commit and resumes all respect the off state.
- On: a full re-commit with `ACTIVE` = 1 (the same path as a switch, so the newest frame shows at once, and an
  external FB is put back). Then a hotplug that happened while off is evaluated (it can switch output right away).
- The owner's "after wake, nothing works" (2026-09-26) was **not** a display problem: the device log shows
  `screen on (Unknown-1) in 16 ms` and no flip warning; the buttons were dropped by the power module's wake-key
  swallow (power.md §7, fixed). Two defensive measures were added anyway, for failures only real DRM can show:
  - **Lost flip event** (`flip_watchdog()`, run from `display_handle_events()`): if a page flip has been in flight for
    more than 1 s (`FLIP_STUCK_MS`; a real one takes one refresh), it logs `no page-flip event for N ms on <output>:
    re-committing the output` and does the same full re-commit as screen-on. Without it a missing vblank event would
    keep every buffer busy and freeze the picture for good.
  - **Screen-on failure** (main.c `scr_set_active()`): if `display_set_active(true)` fails, the display is re-opened
    (`display_shutdown()` + `display_init()`: full probe and modeset), logged as `screen on failed: re-opening the
    display`.

### 8.4 Overlay plane (`display_set_overlay`, the in-game battery indicator)

```c
int  display_set_overlay(const uint32_t *argb, int w, int h, enum display_corner corner, int margin);
void display_hide_overlay(void);
bool display_overlay_visible(void);                 /* the plane shows it now */
bool display_overlay_rect(int W, int H, int w, int h, enum display_corner c, int margin,
                          struct display_rect *out); /* placement maths, pure */
```

- **Plane**: the first `DRM_PLANE_TYPE_OVERLAY` plane of the current CRTC that lists ARGB8888 and is not the game
  plane (`ov_plane()`: on sun4i, one of the 3 overlay layers of the backend; default zpos = layer id, above the
  game's primary at 0). ARGB8888, `SRC` = `CRTC` size (never scaled: it goes through the backend, the frontend
  scaler stays the game's). Within the A20 rules (docs/kernel-patches.md, "Display scaling on A20",
  `sun4i_backend_atomic_check()`): it is the only plane with alpha, the bottom plane (the game, XRGB) is opaque, and
  it is not a frontend plane.
- **Placement**: `display_overlay_rect()`: `margin` pixels from the chosen corner (top-right, top-left,
  bottom-right, bottom-left), then clamped so the whole rect is on screen (planes are not clipped); refused (not
  shown) if it is larger than the mode. Recomputed from the mode in every commit that places it, so an LCD ↔ HDMI
  switch re-places it in the switch's own modeset commit. Unit-tested in `rsos-display-selftest` for 640x480, 720p,
  1080p and 576p, all four corners, clamping, and the plane choice.
- **Buffers**: the content is copied into a CPU buffer; two ARGB8888 dumb buffers alternate, and the one on screen is
  never written (no tearing). Only changed content is copied (whole lines: write-combined memory).
- **When it is committed** (`ov_prepare()`): nothing per frame. A change (new content, place, hide) rides on the
  next non-blocking page flip (`flip()`, and `display_present_fb()` for the GL zero-copy path, so N64 needs no
  readback). If no flip carries it within 100 ms (a paused game, dupe frames, the menu idle), `display_handle_events()`
  commits it alone, non-blocking with a flip event (frames presented meanwhile are queued and flipped right after,
  at most one frame late, only when the overlay changes). `display_set_overlay()` on an idle display commits at
  once. Every modeset (switch, `display_resume()`, `display_set_active(true)`, a strategy re-pick) re-sends it in the
  same commit; a modeset without it switches its plane off with the other unused planes.
- **Refusal**: the first commit of a new (plane, CRTC, rect) runs a `TEST_ONLY` of the overlay against the current
  state. In `apply()` the full state is tested with the overlay, and again without it if that fails. A refusal is
  logged once (`overlay refused on plane N ...; not shown`), the overlay is left out and never tried again at that
  place (new content or a new place retries). A flip that fails with the overlay is re-issued without it: the game
  display never pays for the overlay.
- Log: `overlay: plane N, WxH at X,Y` whenever it is placed somewhere new.

### 8.5 Panel safety: never stop the signals of a powered panel (2026-09-28)

**Why.** A TFT panel must not stay powered with its input signals stopped: without the pixel clock, syncs and data,
the source drivers and the liquid crystal sit at a DC bias, which damages them (panel datasheets: remove VCC within a
short time after the signals stop). The RetroStone2 panel's VCC is the **always-on 3.3 V rail** (no power GPIO,
`power-supply = <&reg_vcc3v3>` in the DTS), so drm_panel's disable only turns the PWM backlight off: the old screen
off (`ACTIVE` = 0: TCON0 stops the pixel clock, syncs and data) left the panel powered and undriven. On the owner's
unit the idle screen off (power.c `idle_off_s`, 300 s) did that for over an hour; afterwards the panel showed
permanent vertical lines and flicker, even under another OS. HDMI on TCON0 channel 1 had the same effect (channel 0
off for the whole HDMI session), and so could a game child's exit or the frontend's exit before `poweroff -f`.

**Rule.** On a board whose built-in panel cannot be powered off (board.ini `display_quirks = panel-keep-scanning`,
`display_config.panel_keep_scanning`; docs/porting.md), the panel's CRTC is never turned off while the system runs:

| Path | What happens now |
|---|---|
| Idle screen off, sleep, charge-mode screen off (menu: power module -> `scr_set_active(false)`; game: `RSOS_SIG_SLEEP` -> host `display_set_active(false)`) | backlight off (`bl_power` = 4, as drm_panel did), a black frame on the game plane, overlay off, **CRTC still ACTIVE**. Screen on: the newest frame, overlay, `bl_power` = 0. Presents are not flipped while off (no rendering cost) |
| HDMI plugged (switch, not mirror) | HDMI on **CRTC 1 (TCON1)**; the panel keeps CRTC 0 with its mode, scanning the RetroStone boot logo (`display_set_panel_picture()`: the menu, the splash process and the game process hand it the decoded `splash.rle`; black until then), backlight off. The owner's choice: the logo shows if the backlight is ever lit. Unplug: the game plane moves back to CRTC 0 (no panel modeset unless its mode changed), backlight on |
| Boot or resume with HDMI | the panel is lit alone first (black), then HDMI in its own commit: the two CRTCs are never modeset together (the panel's pll-video is only rate-protected once TCON0 channel 0 runs: `clk_rate_exclusive_get()` in `sun4i_tcon_channel_set_status()`, and the HDMI clocks could otherwise re-rate it) |
| Game hand-off (`display_suspend`/`resume`) | the menu's framebuffers stay on screen until the child's first commit; the child (same board.ini) keeps the panel scanning too; its exit removes only its planes (`atomic_remove_fb()` tries a plane-only commit first, which sun4i accepts: the CRTC stays on the backend background); `display_resume()` lights the panel again (alone first on HDMI) and resets the backlight |
| Display closed (frontend exit, re-open, `rcK` before `poweroff -f`) | `display_shutdown()` takes every plane down in a non-modeset commit, every CRTC stays ACTIVE (backend background): the panel scans until the rails are cut. Logged `display closed: planes off, the panel keeps scanning` |
| A modeset of the panel itself (60 Hz trial, first light) | TCON0 is re-programmed: a few ms without signals, like any modeset |

If the panel cannot be lit for HDMI (the black frame or its commit refused), the HDMI switch is refused and the
panel stays the output. A board whose HDMI could only use the panel's CRTC stays on the panel (logged). Without the
quirk (RetroStone1: its TFT is driven by the AMT630A's own controller; Pi, Orange Pi) nothing changes.

Remaining windows, all short: power-on to the splash's first modeset (U-Boot does not drive the LCD, the kernel
brings the display up), and each modeset of the panel. Tested against a KMS mock (`tests/test_display_kms.c`, `make
check`): screen off never commits `ACTIVE` = 0 on the panel CRTC, shows black and sets `bl_power` 4; HDMI runs on CRTC
50 with CRTC 49 scanning; boot on HDMI, hand-off and close keep it scanning; no joint modeset.

## 9. Kernel-side notes (for the kernel work)

- **fe0 is shared by both backends** (`sun4i_backend_find_frontend()`). Its output port (`FRM_CTRL.OUT_PORT_SEL`,
  BE0 at reset) is set to the backend that uses it by kernel patch 0006, which HDMI on TCON1 (§8.5) needs.
- **Frontend runtime PM**: `sun4i_frontend_init()` (`pm_runtime_get_sync`) runs on every frontend plane update, and
  `exit` only runs on teardown, so the usage count only grows (`sun4i_layer.c:95-96`, `:74-80`). This is harmless for
  us (the frontend stays powered).
- **Background colour** of the letterbox area: `SUN4I_BACKEND_BACKCOLOR_REG` is never written. Black is expected from
  reset (TODO(hw)).
- **LCD refresh** 78.6 Hz with the current DTS timings; 60 Hz as a user mode from userspace (section 3.1). If 60 Hz
  is confirmed, the DT `clock-frequency` can become 25200000 (a board change) and the setting stays as an option.
- `CONFIG_DRM_FBDEV_EMULATION` is off (`linux.fragment`), so nothing lights the panel before `display_init()`: the
  frontend's first modeset is the first picture. There is no fbdev client to fight over master either.

## 10. Hardware test checklist (UART console)

Build with `BR2_PACKAGE_RSOS_FRONTEND=y`. `rsos-kmstest` and `rsos-display-selftest` are in `/usr/bin`. Save every
log.

**Discovery**
- [ ] `rsos-kmstest --list`: driver `sun4i-drm`, atomic yes, 2 CRTCs, 8 planes (4 per CRTC: 1 primary + 3 overlays,
      zpos and alpha properties), connectors `Unknown-1` (panel) and `HDMI-A-1`. Encoder `possible_crtcs`: RGB 0x1,
      TMDS 0x3. Plane formats include RGB565 and XRGB8888 (and the YUV formats when the frontend is bound).
- [ ] `rsos-display-selftest`: `OK`, `NEON path`, and the 320x240 conversion time (expect well under 1 ms).

**LCD**
- [ ] `rsos-kmstest`: full-screen pattern. Line 3 reads `HW XRGB8888 320x240>640x480`. The 1-pixel white border is
      visible on all four edges. The moving white/red bar shows **no tear line**. FPS equals the panel refresh (78.6
      with the current DTS, 60 with a 25.2 MHz pixel clock). DROP stays 0. The backlight is on.
- [ ] `--format rgb565`: with patch 0003, `HW RGB565` (backend x2: the checkerboard stays crisp 2x2 blocks). Without
      it, `HW XRGB8888` (frontend, converted).
- [ ] `--format xrgb1555 --copy`: correct colours (bars, grey ramp without banding steps other than 5-bit).
- [ ] `--scale integer --size 256x224 --format rgb565`: 512x448 centered, black borders (checks BACKCOLOR).
- [ ] `--no-scale`: `SW BLIT x2`, full-screen; `--no-scale --soft-scale 1`: 320x240 centered.
- [ ] `--buffers 3`: FPS unchanged, DROP 0.
- [ ] `--resize` and `--resize --format rgb565 --copy`: the size cycles every 180 frames without black frames or
      modesets. With RGB565, 368x240 logs `re-picking the scaling strategy` once, and later cycles are silent (cache).
- [ ] `top`: kmstest CPU use (should be a few %).
- [ ] **60 Hz (§3.1)**: `rsos-kmstest --lcd-refresh 60`: stable picture, FPS 60.0, the log has `LCD retimed to 60
      Hz ... 126 MHz / 5` and `refresh measured on Unknown-1: 60.0xx Hz`. Then from the menu: Settings > Display >
      LCD refresh rate > 60 Hz, KEEP (and once: let the 15 s run out, the screen goes back to 78 Hz by itself).
      frontend.log: `setting lcd_refresh = 60`, `output reprobe: Unknown-1 640x480@60.000 Hz`, the measured line.

**Hotplug**
- [ ] `rsos-kmstest --monitor`, then plug/unplug HDMI: one `change@.../drm/card0 ... HOTPLUG=1 CONNECTOR=<id>` per
      edge, within ~500 ms, no storms.
- [ ] `rsos-kmstest`, then plug HDMI: the log shows `hotplug uevent`, `AUDIO release`, `switching to HDMI-A-1 "<TV>"
      ... on crtc 50 (index 1)`, `panel backlight off`, `OUTPUT hotplug`, `LATENCY ... ms`, `AUDIO acquire`. The
      backlight goes off (`cat /sys/class/backlight/*/bl_power` = 4) while the panel keeps scanning black (§8.5:
      `/sys/kernel/debug/dri/0/state` shows crtc-0 `active=1` with Unknown-1). The TV shows 1280x720, image 960x720
      centered, FPS 60, **the scaled image visible** (fe0 -> be1, kernel patch 0006).
- [ ] Unplug: back to the LCD, backlight on, pattern full-screen, FPS back to the panel rate.
- [ ] Repeat 20 plug/unplug cycles, including fast half-insertions: exactly one switch per final state, no stuck output,
      and no `WARN` in `dmesg` (e.g. `sun4i_backend_atomic_begin` LOADCTL timeout).
- [ ] Boot with HDMI already plugged in: starts on HDMI.
- [ ] Record the latency numbers (min/avg/max over 10 plugs).

**HDMI variants**
- [ ] Default on the RetroStone2 (HDMI on TCON1/be1 with fe0, panel kept scanning on TCON0): record whether the scaled
      plane shows correctly, is black, or shows garbage (then patch 0006's output port is wrong: report it; the panel
      is safe either way). Try `--format rgb565 --hdmi 640x480` too (backend x2, no frontend).
- [ ] Panel kept scanning while on HDMI for 10 min: no lines or flicker on the panel afterwards; its colours right
      when HDMI is unplugged (pll-video0 untouched by HDMI: `cat /sys/kernel/debug/clk/clk_summary | grep -i video`).
- [ ] `--hdmi 640x480`: the TV goes to 480p, `HW RGB565 ... 640x480` with `--format rgb565`.
- [ ] `--mode 1920x1080`: 1080p works (expect frontend x4.5 for 320x240); check `dropped` stays 0.
- [ ] No-EDID case (a DVI adapter without DDC, or DDC lines lifted): `[built-in mode, no EDID]` 720p60, then two
      re-probes one second apart.
- [ ] TV overscan: the 1-pixel border is visible (or note how much the TV crops).

**Zero-copy, hand-off, screen off**
- [ ] `--fb-test`: line 3 reads `EXT FB <id> 320x240 ZERO-COPY`, same picture and scaling as the surface mode, FPS =
      refresh, no tearing. Plug/unplug HDMI during it: the external FB comes back scaled on the new output (log shows no
      `external FB ... refused`). Also `--fb-test --hdmi-crtc 1`.
- [ ] `--suspend-test` (and `--suspend-test --fb-test`): every 600 frames the child kmstest takes the display for 240
      frames, then the parent comes back. Log `RESUME ok: suspend .. ms, child .. ms (exit 0), resume .. ms`. Record
      the resume time (compare with a `display_shutdown()`+`display_init()` cycle). Plug or unplug HDMI while the child
      runs: after resume the parent is on the right output (`output changed during the game`).
- [ ] `--active-test`: every 600 frames the screen goes off for 2 s. The backlight goes dark (`bl_power` 4), the
      panel keeps scanning black (§8.5: log `screen off (Unknown-1: backlight off, black frame, still scanning)`),
      the picture comes back with the newest frame, and `dmesg` is clean. Also on HDMI (the TV loses signal, then
      resyncs; the panel stays black and dark), and plug HDMI during the 2 s: the switch happens when the screen comes
      back.
- [ ] Current draw with the screen off versus `bl_power` = 4 only (power.md): record it.

**Overlay plane (in-game battery)**
- [ ] A game on the LCD: the indicator shows in the top-right corner, 4 px from the edges, 1:1 pixels, the game
      unchanged under it; the log has `overlay: plane N, 72x20 at 564,4` and no `overlay refused`.
- [ ] The same on HDMI 720p (margin 21 px) and 1080p (2x, 144x40, margin 32 px); plug/unplug during the game: it
      follows the output in the switch commit.
- [ ] An RGB565 core (backend x2), an XRGB8888 or 0RGB1555 core (frontend) and N64 zero-copy: shown on all, and
      for N64 `hw render: N zero-copy frames` at exit is about the frame count (no readback).
- [ ] `dmesg` clean (no `sun4i_backend_atomic_check` rejection); if refused, record the log line.

**Audio hook (once the ALSA layer exists)**
- [ ] The HDMI PCM is closed on `AUDIO release` before the HDMI modeset/disable, and reopened on `AUDIO acquire`.
      There is no ALSA error on unplug.
