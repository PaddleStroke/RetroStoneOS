# Kernel patches

Patches for Linux 6.18.54 in `buildroot-external/board/retrostone2/patches/linux/`, applied by Buildroot through
`BR2_GLOBAL_PATCH_DIR` (`patch -p1`, in file-name order). The config options they need are in
`buildroot-external/board/retrostone2/linux-patches.fragment`.

| # | Patch | Status |
|---|---|---|
| 0001 | `drm/sun4i: hdmi: poll the HPD line every 500 ms from a private work` | Compiles, small, high confidence |
| 0002 | `drm/sun4i: hdmi: add HDMI audio support (EXPERIMENTAL)` | Compiles, untested on hardware |
| 0003 | `drm/sun4i: backend: use the backend 2x/4x integer scaler (EXPERIMENTAL)` | Compiles, untested on hardware |

**Build checks.** Each patch applies with `patch -p1` on a clean v6.18.54 tree. The series builds with `sunxi_defconfig`
plus `linux.fragment` and `linux-patches.fragment` (zImage and modules), and `drivers/gpu/drm/sun4i/` builds with `W=1`,
with no warnings, in three configs: built in with audio, as modules with audio, and as modules without audio.
`checkpatch.pl --strict` only reports the missing `Signed-off-by:`.

**Authorship.** The patches have no `Signed-off-by:`, because adding one is a legal statement (DCO) that only a person can
make. Add yours before sending anything upstream. 0002 is derived from Stefan Mavrodiev's unmerged v3, so credit him
(for example with `Co-developed-by:`) if you post it.

**Development tree.** `~/rsos/src/linux-6.18-patches` in WSL is a git worktree of `~/rsos/src/linux-6.18` on the
`rsos-patches` branch, with one commit per patch. To regenerate the patch files:
`git format-patch --no-signature -o <repo>/buildroot-external/board/retrostone2/patches/linux 1b357ecb3..rsos-patches`.

**Other boards.** The RetroStone1 (`board/retrostone1/patches/linux/`) carries a copy of the RetroStone2's 0004
(exFAT directory read-ahead plug, generic) as its own 0001; keep the two copies identical. Its 0002 is the H3 TV
encoder (composite output), see "RetroStone1 0002" at the end of this page.

| RetroStone1 # | Patch | Status |
|---|---|---|
| 0001 | `exfat: plug the queue around the directory read-ahead` (= RetroStone2 0004) | as the RetroStone2's |
| 0002 | `drm/sun4i: add the H3 TV encoder (composite output)` | Compiles (`W=1`, no warnings; `checkpatch --strict` clean), from Armbian; untested on RetroStone1 hardware |

---

## 0001: fast HDMI hotplug detection

### Problem
The A10/A20 HDMI encoder has no HPD interrupt. Mainline flags the connector
`DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT` (`sun4i_hdmi_enc.c`, in `sun4i_hdmi_bind()`), so the
drm_kms_helper output poll worker detects hotplug. That worker runs every `DRM_OUTPUT_POLL_PERIOD` = 10 s
(`drivers/gpu/drm/drm_probe_helper.c:266`), so a plug or unplug can take up to 10 s to be noticed.

### Chosen approach: (b) a private delayed work in sun4i_hdmi
- `sun4i_hdmi_hpd_work()` reads `SUN4I_HDMI_HPD_REG` every `hpd_poll_ms` (default 500 ms). It compares the value with
  `connector->status` and does nothing else when they match, so the common case is one MMIO read. It takes no lock and
  reads no EDID.
- On a change it calls `drm_connector_helper_hpd_irq_event()`. That function takes `mode_config.mutex`, runs `detect()`
  (which is only the same HPD register read) and bumps the epoch counter. It then sends a **connector hotplug uevent**
  (`drm_sysfs_connector_hotplug_event()`, which carries `HOTPLUG=1 CONNECTOR=<id>` on the card0 device) and calls
  `drm_client_dev_hotplug()`.
- The connector is now flagged `DRM_CONNECTOR_POLL_HPD`, so the core poll worker skips it. The LCD (RGB panel)
  connector is not polled either, so the core poll worker is never scheduled at all.
- The work is not started while the DRM device is unregistered or the connector is forced (`video=HDMI-A-1:e`). It runs
  on `system_freezable_power_efficient_wq`, so it is frozen during suspend, and it is cancelled in unbind.
- Module parameter: `sun4i_drm_hdmi.hpd_poll_ms=<20..10000>` on the kernel command line. It can also be changed at
  runtime in `/sys/module/sun4i_drm_hdmi/parameters/hpd_poll_ms`.

### Why not (a), a poll-period parameter in drm_kms_helper
- It changes a core module that all DRM drivers share. Every tick of the core worker takes `mode_config.mutex` (a
  trylock), walks every connector, and runs `drm_helper_probe_detect()`, which takes the `connection_mutex` modeset lock
  through a full acquire context. At 2 Hz that contends with the frontend's atomic commits for nothing.
- It needs a new core parameter (an upstream API change) and a cmdline entry. Patch (b) is local to the one driver that
  lacks an interrupt, and it is the same pattern as an HPD interrupt handler (vc4 calls the same helper from its HPD IRQ
  thread).

### detect() cost and EDID
`sun4i_hdmi_connector_detect()` was already only an HPD register read, and 0002 adds only a jack notification on
disconnect. The EDID is read over DDC only in `sun4i_hdmi_get_modes()`, which runs when **userspace** probes the
connector (`DRM_IOCTL_MODE_GETCONNECTOR` → `fill_modes`). Reading two EDID blocks over DDC at about 100 kHz takes about
25-50 ms. `CONFIG_DRM_FBDEV_EMULATION` is off in `linux.fragment`, so no in-kernel client re-probes on hotplug.

### CPU cost
- Per tick: one timer expiry, one kworker wakeup and context switch, and one uncached AHB read. On a Cortex-A7 at
  1 GHz that is roughly 5-20 µs, so 2 ticks/s cost about 10-40 µs of CPU per second (**< 0.005 % of one core**), plus
  2 wakeups/s from idle. HZ is 100, so the period is rounded to 10 ms jiffies.
- On a change (rare): a detect cycle under the mutex plus a uevent, well under 1 ms.
- Latency: at most one period (500 ms, 250 ms on average), plus uevent delivery.

### What the emulator frontend must do
- Listen on a `NETLINK_KOBJECT_UEVENT` socket for `SUBSYSTEM=drm`, `HOTPLUG=1` (and optionally `CONNECTOR=<id>`).
- On such an event, call `drmModeGetConnectorCurrent()` for a cheap status check (no probe, no EDID). Call
  `drmModeGetConnector()` (full probe with EDID read and mode list) only when the status changed to connected.
- HPD can bounce while a plug is being inserted, and some TVs pulse HPD when they change input. A new status can
  therefore come back within one period. Let it settle (for example, act on a status that holds for 2 events or
  500 ms) before switching outputs.
- TODO(hw): measure the actual plug-to-uevent latency and check for HPD bounce on the RetroStone2 connector.

---

## 0002: A20 HDMI audio (EXPERIMENTAL)

### Sources
Stefan Mavrodiev (Olimex) posted "Add support for sun4i HDMI audio":
- v1, 2020-01-10, `20200110141140.28527-1-stefan@olimex.com`: 2 patches, with sun4i-dma cyclic DMA for dedicated
  channels, which was merged.
- v2, 2020-01-20, `20200120123326.30743-1-stefan@olimex.com`: a separate platform driver and an ELD kcontrol.
- v3, 2020-01-28, `20200128140642.8404-1-stefan@olimex.com` (cover) and `20200128140642.8404-2-stefan@olimex.com`:
  a single patch adding `sun4i_hdmi_audio.c` (450 lines). It registers and unregisters an ASoC card at every encoder
  enable/disable, and restores the drvdata by hand.

lore.kernel.org and lkml.org are behind a bot check (Anubis), so the patch text and the review were read from the
spinics.net arm-kernel archive (msg782913/782914/783066/783071/783108) and from lkml.iu.edu (2001.1/02884, 03082).

Review points that were not addressed before the series died:
- Maxime Ripard (v1, v3): don't create or destroy the sound card at enable/disable time. Creating it only on enable
  also left a duplicate card after a disable/enable cycle in v1. snd_soc_register_card() overwrites the device drvdata,
  which breaks unbind, so use the card pointer instead. ELD is the mechanism for telling userspace whether the sink is
  plugged in and can play audio.
- Chen-Yu Tsai (v3): ASoC is dead weight for "a FIFO fed by a DMA engine"; a plain ALSA dmaengine card would do. Also
  CC the ALSA/ASoC maintainers.

### Design for 6.18: generic `hdmi-codec`, like vc4_hdmi
6.18 has the DRM HDMI audio helper (`drm_connector_hdmi_audio_init()`, `drivers/gpu/drm/display/drm_hdmi_audio_helper.c`).
It creates an `hdmi-codec` device bound to the connector, and it handles the ELD control, the ELD-based hw constraints,
the IEC 60958 channel status, the audio infoframe contents and the jack/plug notifications. vc4_hdmi has exactly the
same hardware shape (an internal audio FIFO fed by DMA) and uses it. **This is the route chosen.** It needs about
330 lines against the original 450. It also fixes both of Maxime's points and replaces the custom ELD kcontrol and the
hand-built infoframe. Chen-Yu's "plain ALSA" idea would need all of that written by hand again.

Structure (`drivers/gpu/drm/sun4i/sun4i_hdmi_audio.c`):
- **CPU DAI** `sun4i-hdmi-cpu-dai`: the HDMI audio FIFO. Its dma_data is 4-byte wide with burst 8, and the DMA
  channel `audio-tx` feeds it through the generic dmaengine PCM. The buffer limits are those of the tested patch
  (128 KiB buffer, periods of 4-32 KiB, 2-8 periods).
- **Codec DAI** `i2s-hifi` of hdmi-codec. The driver implements `startup` (refuses DVI sinks), `prepare` (reset,
  layout/channel count, channel map, channel status sampling frequency, N/CTS, enable, audio infoframe) and
  `shutdown`.
- **Card** `sun4i-hdmi`: registered **once at bind** with `devm_snd_soc_register_deferrable_card()`. hdmi-codec is
  built in but registers after `drivers/gpu`, so the card binds later on its own. The device drvdata is restored after
  registration, and the card finds the HDMI struct through `card->drvdata`. The DAI link init creates an
  **"HDMI Jack"**.
- N/CTS: from `drm_hdmi_acr_get_n_cts()` (the HDMI 1.4b tables) using the TMDS character rate, and reprogrammed from
  `sun4i_hdmi_enable()` when the mode changes while audio plays. The original patch used N = 128·fs/1000 with a
  truncated CTS.
- Channel status sampling frequency: taken from the IEC 60958 status that hdmi-codec computes. The original patch
  had its own table, which wrote 0x9 for 96 kHz instead of 0xA.
- Audio infoframe: written to 0x0a0 by `write_infoframe()`. While audio runs, it is added to the packet scheduler as
  packet type 3 (`SUN4I_HDMI_PKT_CTRL_REG(0)`: AVI, AUDIO, END), and `clear_infoframe()` removes it. The original
  patch wrote the frame but never scheduled it (it left `PKT_CTRL` at AVI, END).
- Jack: plugged after each EDID read (`get_modes`), unplugged when `detect()` sees HPD low.
- Audio failures are never fatal for the display: they only log a warning.
- Unsupported infoframe types, such as the SPD infoframe that the HDMI state helper sends at every modeset, are now
  logged at debug level instead of `drm_err`.

Formats: **S16_LE, 2 channels, 32/44.1/48/88.2/96/176.4/192 kHz**. The BSP's S20_3LE/S24_LE did not work for Stefan,
and multichannel was never tested, so they are not offered.

### Device tree
**No DT change is needed.** `sun7i-a20.dtsi` already has, on the `hdmi` node,
`dmas = <&dma SUN4I_DMA_NORMAL 16>, <&dma SUN4I_DMA_NORMAL 16>, <&dma SUN4I_DMA_DEDICATED 24>;` and
`dma-names = "ddc-tx", "ddc-rx", "audio-tx";`. The driver uses `audio-tx`. The card is created by the driver, so no
`simple-audio-card` node or `#sound-dai-cells` is needed. The board DTS only has to enable `&hdmi` (and `&dma`, which
is enabled in the dtsi).

### What is uncertain (TODO(hw))
1. **DMA destination address 0.** As in the BSP and in the tested patch, the FIFO address is not given: the dedicated
   DMA HDMI audio port seems to be selected by DRQ type 24 alone. If there is no sound and no DMA progress, this is
   the first thing to check.
2. **Packet type 3 = audio infoframe** is inferred from the BSP packet setup (`0x2f0 = 0x0000f321`) and from the buffer
   layout (type 2 = AVI at 0x080, type 3 at 0x0a0). If a TV loses the picture or behaves oddly while audio plays, remove
   the audio slot in `sun4i_hdmi_write_pkt_ctrl()`. Many TVs play LPCM without an audio infoframe.
3. The call order moved from "reset+enable at startup, program at hw_params" (v3) to "reset, program, then enable at
   prepare" (BSP order).
4. Latency: with a period of at least 4 KiB, the minimum is 2 × 1024 frames ≈ 43 ms at 48 kHz. `period_bytes_min` can
   probably be lowered once it works.
5. Switching HDMI to LCD while an HDMI stream is open: the audio block keeps waiting for TMDS. Close the HDMI PCM
   before disabling the HDMI CRTC.

### Using it from the frontend
- ALSA card `sun4i-hdmi`, PCM device 0 (`hw:sun4i-hdmi,0` or by index), S16_LE stereo. It sits next to the analog
  `sun4i-codec` card.
- The `HDMI Jack` control (and the jack input device, because `SND_JACK_INPUT_DEV=y`) tells whether an HDMI sink with
  a valid EDID is plugged in. The `ELD` control (from hdmi-codec) tells whether it accepts audio. A DVI sink makes
  `snd_pcm_open`/`startup` fail with `-ENODEV`.

---

## 0003: backend 2x/4x integer scaling (EXPERIMENTAL)

This is the TODO from `sun4i_backend_plane_uses_frontend()`; see the next section for the background. With the patch,
a plane whose on-screen size is exactly 1×, 2× or 4× its source size on each axis (independently, for example 2× wide
and 4× tall), in any RGB format the backend supports, is scaled by the backend itself (`LAY_WSCAFCT`/`LAY_HSCAFCT` in
`ATTCTL_REG1`, value log2(factor)). The frontend is not used for it. Other ratios still use the frontend as before.
- Formats: RGB565, XRGB8888, ARGB8888, ARGB1555, RGBA5551, ARGB4444, RGBA4444, RGB888.
- Result: nearest neighbour (pixel repeat), sharp, with per-pixel alpha kept, and the frontend stays free.
- TODO(hw): the layer size register (`LAYSIZE`) is assumed to hold the **on-screen** size, as today, and not the source
  size when scaling is on. If the picture shows up at 2× the expected size (clipped), `LAYSIZE` must be the source size
  instead: a one-line change in `sun4i_backend_update_layer_coord()`.
- YUV layers are excluded, because it is unknown whether the backend's YUV channel honours the factors.

---

## Display scaling on A20

The line numbers below refer to **unpatched v6.18.54**, in `drivers/gpu/drm/sun4i/` unless noted.

### Pipeline
- The A20 has two DEFE (frontend: scaler and CSC, `fe0`/`fe1`), two DEBE (backend: 4 layers, blending, `be0`/`be1`) and
  two TCONs. Backend N always outputs to TCON N (`sun4i_backend.c:909-928`, `needs_output_muxing`), so CRTC0 is
  be0+TCON0 and CRTC1 is be1+TCON1. The LCD is on TCON0 channel 0. HDMI can be fed by TCON0 or TCON1 through channel 1
  (`sun4i_tcon.c:1343` `sun4i_a10_tcon_set_mux()`, and `hdmi_in_tcon0`/`hdmi_in_tcon1` in `sun7i-a20.dtsi:673-680`).
- Each backend exposes **4 planes**: layer 0 is `DRM_PLANE_TYPE_PRIMARY` and layers 1-3 are overlays
  (`sun4i_layer.c:247-248`), with `zpos` 0-3 and a plane `alpha` property (`sun4i_layer.c:227-229`).
- Both backend and frontend run at 300 MHz (`sun4i_backend.c:854`, `sun4i_frontend.c:647`).

### Which formats can be scaled
| Path | Formats | Scaling |
|---|---|---|
| Backend only (unpatched) | ARGB1555, ARGB4444, ARGB8888, RGB565, RGB888, RGBA4444, RGBA5551, UYVY, VYUY, XRGB8888, YUYV, YVYU (`sun4i_backend.c:142-155`) | **none**: a backend-only plane with src ≠ dst is rejected (`sun4i_backend.c:451-453`) |
| Backend + 0003 | the RGB formats above | ×1, ×2, ×4 per axis, nearest neighbour |
| Frontend | **XRGB8888, BGRX8888** and YUV (NV12/16/21/61, YUV/YVU 411/420/422/444, YUYV/YVYU/UYVY/VYUY; `sun4i_frontend.c:364-383`) | any ratio, 4-tap horizontal and 2-tap vertical filter (coefficients `sun4i_frontend.c:26-54`) |

- **RGB565 cannot go through the frontend.** The DEFE only reads 32 bpp packed RGB: the input format register has a
  single RGB mode (`sun4i_frontend.h:62`) with the XRGB/BGRX pixel sequences (`:69-70`). So without 0003, **RGB565
  cannot be scaled at all**.
- **XRGB8888 can be scaled** (any ratio) by the frontend. BGRX8888 can too, and YUV can.
- The frontend output is always XRGB8888 to the backend (`sun4i_layer.c:99-102`, `sun4i_frontend.c:348-362`). It
  **drops alpha**: ARGB8888 is not accepted as input, and the TODO at `sun4i_frontend.c:483-487` about ALPHA_EN applies
  to the A31/A80. The global plane `alpha` property still works on the backend layer.
- Choice logic (`sun4i_backend_plane_uses_frontend()`, `sun4i_backend.c:408-439`): the frontend is used when the format
  is frontend-only (YUV planar, BGRX), or when the format is supported by both and the plane is scaled. Unscaled
  XRGB8888 uses the backend directly.
- Tiled buffers (`DRM_FORMAT_MOD_ALLWINNER_TILED`) are frontend-only and YUV-only (`sun4i_frontend.c:136-153`).

### Which planes can be scaled, and how many
- **Any of the 4 planes, primary included.** There is no primary/overlay restriction. The primary plane need not
  cover the CRTC either: sun4i has no plane `atomic_check`, so `drm_atomic_helper_check_planes()` applies no
  restriction.
- **At most 1 plane per CRTC through the frontend** (`SUN4I_BACKEND_NUM_FRONTEND_LAYERS` = 1, `sun4i_backend.h:166`,
  enforced at `sun4i_backend.c:601-604`), and at most 1 YUV plane handled by the backend (`:595-599`).
- With 0003, backend integer scaling has no count limit: all 4 layers can use it.
- Alpha: on the A20 (no `supports_lowest_plane_alpha`, `sun4i_backend.c:989-991`), at most **1** plane with alpha
  (a format with alpha, or plane alpha < 1). **The lowest plane (zpos 0) must be opaque.** A plane with alpha goes to
  pipe 1 (`sun4i_backend.c:533-593`). An opaque game plane at zpos 0 plus one ARGB OSD overlay works.

### The backend's own 2x/4x integer scaling (the TODO)
`sun4i_backend.c:424-429`: "The backend alone allows 2x and 4x integer scaling, including support for an alpha component
(which the frontend doesn't support)". The DEBE layer attribute register 1 has per-layer `LAY_HSCAFCT` (bits 15:14) and
`LAY_WSCAFCT` (bits 13:12) (`sun4i_backend.h:78-79`), which repeat pixels 2× or 4× vertically and horizontally.
Mainline defines the fields but never sets them, so today every scaled plane has to go through the frontend.
320×240 → 640×480 is exactly 2× on both axes, so the backend alone can do it in RGB565 and with alpha, sharp and at no
cost. 0003 implements this.

### Known limitations and bugs
- **No clipping.** There is no plane atomic_check (see above), so planes partly outside the CRTC, or with negative
  `crtc_x`/`crtc_y`, are programmed as is (`LAYCOOR` is written with 16-bit masks, `sun4i_backend.h:47-49`). Keep every
  plane fully inside the mode.
- **Fractional source coordinates are ignored** (`src_x/src_y/src_w/src_h >> 16`). Use integer source rectangles.
- **Frontend scaling factor** = `(src << 16) / dst`, truncated (`sun4i_frontend.c:523-531`), with the phase fixed at 0
  on A10/A20 (`sun4i_frontend.c:686-688`). Expect slight softness and edge offsets rather than exact nearest neighbour.
- **Odd widths.** For RGB input there is no code-side restriction on odd widths. For subsampled YUV, the chroma size
  is `DIV_ROUND_UP(width, hsub)` (`sun4i_frontend.c:510-511`). Packed 4:2:2 at odd widths is not handled specially.
  Use even widths for YUV. TODO(hw): confirm odd RGB widths (for example 255, or crops) on the frontend.
- **Frontend and a second pipeline (HDMI on TCON1).** `sun4i_backend_find_frontend()` (`sun4i_backend.c:735-763`, marked
  "TODO: This needs to take multiple pipelines into account") returns the first enabled frontend on the backend's input
  port. In `sun7i-a20.dtsi` both `be0` and `be1` list `fe0` first (`sun7i-a20.dtsi:1633,1681`), so **both backends get
  fe0**, and the driver never programs any fe→be cross routing. So:
  - LCD on CRTC0 with the frontend is the tested mainline path.
  - **HDMI on CRTC1 plus frontend scaling is unverified** and may show nothing or garbage if fe0 cannot feed be1 by
    default. TODO(hw).
  - Using both CRTCs at once with the frontend would share fe0. We switch outputs and never mirror, so this does not
    arise.
  - Workarounds: use backend integer scaling (0003) on HDMI, or drive HDMI from CRTC0 (TCON0 channel 1, allowed by the
    `hdmi_in_tcon0` link and `sun4i_a10_tcon_set_mux()`; TODO(hw)).
- **Frontend runtime PM leak.** `sun4i_backend_layer_atomic_update()` calls `sun4i_frontend_init()`
  (`pm_runtime_get_sync()`) on every update of a frontend plane (`sun4i_layer.c:95-96`). `sun4i_frontend_exit()` runs
  only once on teardown (`sun4i_layer.c:74-80`, `sun4i_backend.c:635-639`). After a few page flips the frontend
  therefore never powers down. This is harmless for us, apart from a little power.
- **Frontend reprogrammed on every flip.** Coordinates, formats and `FRM_START` are rewritten on each page flip
  (`sun4i_layer.c:95-103`). This works, but a flip through the frontend costs more register writes than a backend flip.

### Recommendation for the emulator frontend
Cores give RGB565 (or XRGB8888) at 256×224, 320×240 and similar sizes. Present each core frame as a **dumb buffer on a
plane** and let the display engine scale it:

1. **LCD 640×480, 320×240 cores:** keep **RGB565**, source 320×240, plane 640×480 at (0,0). With 0003 this is backend
   2× scaling: no conversion, no filter, no frontend, and it stays sharp. Without 0003, RGB565 cannot be scaled, so you
   would have to convert to XRGB8888 and use the frontend.
2. **LCD, other sizes (256×224, 384×224...):** either
   - integer scaling with borders: 256×224 ×2 = 512×448 at (64,16), RGB565, backend only (sharp), or
   - full-screen or aspect-correct fill: convert to **XRGB8888** (NEON, about 60k pixels/frame, cheap) and use one
     frontend plane with any destination size (filtered).
3. **HDMI 1280×720:** 320×240 ×3 = 960×720 and 256×224 ×3 = 768×672 are not 2×/4×, so they need the frontend
   (XRGB8888, one plane). The 2× integer options (640×480 or 512×448 centered) are small on a TV. The simplest robust
   option is to set **640×480@60** (CEA mode 1, which every HDMI sink must accept) on HDMI and reuse the LCD path, and
   let the TV upscale (sharp 2× from the RetroStone2, then the TV's own scaler). Test HDMI frontend scaling on CRTC1
   before relying on it (see the fe0 note above).
4. Use the **primary plane (zpos 0, opaque)** for the game and at most **one ARGB8888 overlay** for OSD/menus. Only one
   plane may have alpha, and not the bottom one. Keep all planes inside the screen.
5. Only one plane per CRTC can use the frontend. If you need a scaled overlay too, it must be an integer 2×/4× RGB plane
   (0003).
6. Page flip with atomic commits (`DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT`) that change only `FB_ID`.
   Changing plane sizes or formats every frame forces a frontend/backend reconfiguration.

## Patch 0004: exfat: plug the queue around the directory read-ahead
Found on hardware (2026-09-26): the first lookup in a cold exFAT directory costs ~110 ms on the A20. `exfat_dir_readahead()`
(fs/exfat/dir.c) calls `sb_breadahead()` once per sector of the directory cluster, and without a block plug each becomes
its own request: 64 × 512-byte reads for a 32 KiB cluster, at ~1.6 ms each on the sunxi MMC host. The menu checks 34 ROM
folders at boot, which came to 3.9 s. The patch wraps the loop in `blk_start_plug()`/`blk_finish_plug()` so the block
layer merges the reads into one request (the same pattern other filesystems use around read-ahead). The frontend also warms
the ROM folders itself (frontend/src/ui/fswarm.c, see ui-design.md §4.1); the patch covers every other cold directory
(saves, states, the image cache, USB sticks). A candidate for upstream submission.

## Patch 0005: clk: sunxi-ng: a10: register the CCU driver at subsys_initcall
Found in hardware boot logs (2026-09-27): the A20 clock controller (sun4i-a10-ccu) registered at the default device initcall
level, after drivers that depend on it. The pin controller was deferred to late_initcall (it probed at 0.199 s), which pushed
SD card detection to 0.309 s and delayed the root mount. Registering the CCU at `subsys_initcall` lets pinctrl, mmc and the
rest probe in order on the first pass (estimated −100…−170 ms before /sbin/init). To be confirmed with the `initcall_debug`
log of the round-3 dev image (bootlog writes `initcalls.txt`).

## RetroStone1 0002: drm/sun4i: add the H3 TV encoder (composite output)

`board/retrostone1/patches/linux/0002-drm-sun4i-add-the-H3-TV-encoder-composite-output.patch`, for the RetroStone1's
built-in screen (H3 TVOUT -> AMT630A CVBS-to-RGB converter -> panel; docs/boards.md, "RetroStone1").

### Why a patch
Mainline 6.18 drives the TV encoder of the A10/A20 only (`sun4i_tv.c`, `allwinner,sun4i-a10-tv-encoder`, behind the
sun4i backend). The H3 has the same encoder behind its second DE2 pipeline (mixer 1 -> TCON1 -> TVE), which neither
the drivers nor `sun8i-h3.dtsi` describe.

### Source (searched 2026-09-28)
| Candidate | Base | Notes |
|---|---|---|
| **Armbian `sunxi-6.18` (chosen)** | 6.18 | `patch/kernel/archive/sunxi-6.18/patches.armbian/` in https://github.com/armbian/build (commit `faa60b8b7015`, 2026-09-28): `clk-sunxi-ng-h3-add-the-tve-clock.patch`, `drm-sun4i-support-the-h3-and-h5-tv-encoders.patch`, `drm-sun4i-fix-null-deref-when-unbinding-the-tv-encoder.patch`, `dt-bindings-display-allwinner-add-h3-h5-tv-encoder-and-mixer1.patch`, `arm-dts-sunxi-h3-describe-the-tv-encoder-pipeline.patch`, plus the overlay `overlay_32/sun8i-h3-tve.dtso`. Author enthropy7 (August 2026, Signed-off-by), GPL-2.0. Tested there on an Orange Pi Zero (H2+): 720x480i, TVE clock 13.5 MHz, picture checked on a capture card. The closest to 6.18: written for it. |
| LibreELEC `0055-wip-h3-h5-cvbs.patch` | 5.x (2021) | Jernej Škrabec's work-in-progress (`projects/Allwinner/patches/linux/` in LibreELEC.tv), the ancestor of the Armbian series. Written against the older sun4i_tv (before the TV mode property of 6.2). |
| Icenowy Zheng, "drm: sun4i: add support for the TV encoder in H3 SoC" (DE2 series v2, 2017) | 4.13 | Dropped before the DE2 merge. |
| linux-sunxi wiki / Megous (xff.cz) | – | Nothing newer than the above for H3 TV-out. |

### What the patch does (one commit)
- **clk: sunxi-ng: h3**: the TVE module clock (0x120) gets its undocumented **fixed post-divider of 16**
  (`CCU_FEATURE_FIXED_POSTDIV`). Without it TCON1 sets its channel-1 clock (which is the TVE clock) 16 times too
  high and the picture never locks. With it, 13.5 MHz = pll-de 432 MHz / 2 / 16 (pll-de runs at 432 MHz for the
  mixers).
- **drm/sun4i: tv**: a quirks table per compatible. `allwinner,sun8i-h3-tv-encoder`: DAC calibration `0x02000c00`
  written to 0x304 at bind (the H3 DAC drives nothing without it); `allwinner,sun50i-h5-tv-encoder`: `0x02850000`
  plus 0x30c = `0x00101110`. The regmap grows to 0x400. The A10 keeps its behaviour. Also the unbind fix (no second
  `drm_connector_cleanup()`/`drm_encoder_cleanup()`: `drm_mode_config_cleanup()` already did it, NULL dereference).
- **drm/sun4i: sun8i-mixer**: the encoder takes YUV, so the mixer's **DCSC** converts its RGB output (BT.601
  coefficients at 0xb0010, enable at 0xb0000). This goes through the engine's `apply_color_correction` /
  `disable_color_correction` callbacks, which `sun4i_tv_enable()` / `_disable()` already call. New
  `allwinner,sun8i-h3-de2-mixer-1`: one VI and one UI channel, both scaled (`scaler_mask 0x3`), CSC layout of a
  mixer 1, 432 MHz.
- **dt-bindings**: the three new compatibles, and (not in Armbian's series) the H3 display engine may list two
  pipelines, so that `make CHECK_DTBS=y` accepts `allwinner,pipelines = <&mixer0>, <&mixer1>`.

**Port to 6.18.54.** The clk, sun4i_tv and binding parts apply as they are. Armbian applies its mixer part on top of
Jernej Škrabec's sun4i-drm refactor series (`patches.drm/`, 43 patches: `lay_cfg`, `de2_fcc_alpha`...), which 6.18.54
does not have, so the mixer 1 configuration and the colour correction were rewritten for the 6.18 `sun8i_mixer_cfg`.
The colour-correction callbacks return early on DE3/DE3.3 mixers (their DCSC is elsewhere; only the DE2 H3/H5 have
this TV encoder).

**Device tree.** Not in the patch: `board/retrostone1/dts/sun8i-h3-tve-pipeline.dtsi` (included by the board DTS)
does what Armbian's `arm-dts-sunxi-h3-describe-the-tv-encoder-pipeline.patch` does to the SoC dtsi: `&de` gets
`allwinner,pipelines = <&mixer0>, <&mixer1>`, the single mixer0 -> TCON0 endpoints become `endpoint@0/@1` pairs (the
A83T layout: each mixer can reach each TCON), and it adds `mixer1` (`sun8i-h3-de2-mixer-1`, reset `RST_WB`, which the
H3 DE2 clock unit exports for mixer 1), `tcon1` (`sun8i-h3-tcon-tv`, clocks `CLK_BUS_TCON1` + `CLK_TVE` as
`tcon-ch1`, IRQ 87) and `tve` (`sun8i-h3-tv-encoder`, 0x01e00000, `CLK_BUS_TVE`, `RST_BUS_TVE`), all disabled; the
board DTS enables `mixer1`, `tcon1` and `tve`. The driver resolves the pairs by endpoint id: mixer 0 = TCON0 = HDMI,
mixer 1 = TCON1 = TVE (checked in the built DTB). TVOUT is a dedicated analog pin: no pinctrl.

**Development tree.** `~/rsos/src/linux-6.18-rs1` in WSL, a git worktree of `~/rsos/src/linux-6.18` on the `rs1-tve`
branch (one commit on v6.18.54). To regenerate: `git format-patch --no-signature --start-number 2 -o
<repo>/buildroot-external/board/retrostone1/patches/linux 1b357ecb3..rs1-tve`. The Armbian originals it started from
are in `~/rsos/src/tve/armbian-build` (sparse clone).

**Config.** `sun4i_tv.o` is built by `CONFIG_DRM_SUN4I` (=y in `sunxi_defconfig`); `board/retrostone1/linux.fragment`
spells out `DRM_SUN4I=y` and `DRM_SUN8I_MIXER=y`.

### Userspace view
Connector **"Composite-1"** (always "connected": no detect), modes 720x480i (NTSC, preferred by default) and 720x576i
(PAL), connector property **"TV mode"** (NTSC, PAL; `sun4i_tv` encodes this property, not the CRTC mode, so the
frontend sets both together). CRTC 1 with two planes: VI (zpos 0, no alpha formats) and the primary UI plane (zpos
1). The frontend side is in display-design.md §3.2.

### TODO(hw)
The patch is only compile-tested here: the probe (`dmesg | grep -i -e tv -e mixer -e tcon`), a picture on the
AMT630A in NTSC and PAL, the colours (DCSC coefficients, the H3 DAC calibration value), and the HDMI <-> composite
switch (two CRTCs sharing pll-de). The test list is in docs/boards.md, "RetroStone1".
