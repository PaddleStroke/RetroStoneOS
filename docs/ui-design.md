# UI design: renderer, ES themes, screens, input

The RetroStoneOS menu is part of the single `rsos-frontend` binary. It draws an EmulationStation-style UI (system
carousel, game lists, one unified settings menu) into a plain CPU buffer, with a small software renderer and an
EmulationStation theme engine. It never touches DRM: the display layer scales the buffer to the LCD or the TV in
hardware. Source: `frontend/src/{gfx,theme,ui,input}`, build fragment `frontend/ui.mk`, themes `frontend/themes/`,
vendored code `frontend/third_party/` (licences in its README), host preview tool `frontend/src/tools/uipreview.c`.

Previews: `docs/ui-previews/*.png` (list in §16).

## 1. Architecture

```
            settings.ini, gamedb.tsv            /usr/share/rsos/{themes,fonts,cores}      /data/roms/<system>
                     │                                     │                                   │
 input (evdev) ──► ui_button / ui_hotkey ──► ui.c: screen stack ◄── theme/ (theme.xml → properties)   games.c (scan,
 power module ───► ui_power_event                │  system view │ game list │ menus │ dialogs │ OSK    gamelist.xml,
 transfer ───────► ui_usb_event                  │                                                       scan cache)
                                                 ▼
                                   widgets.c: theme elements → pixels (pre-scaled images, cached texts)
                                                 ▼
                              gfx/: fills, blends, blits, glyphs, SVG/PNG/JPG, image cache (.rpx)
                                                 ▼
                         struct gfx_surface { uint32_t *pixels; int w, h, stride /* pixels */; }
                                                 ▼
                    display layer: XRGB8888 buffer, hardware-scaled to the LCD (640x480) or HDMI (720p/1080p)
```

| Module | Files | Role |
|---|---|---|
| Renderer | `gfx/gfx.{c,h}` | XRGB8888 surfaces, premultiplied ARGB images, fills, gradients, rounded rects, blends, blits, nine-patch, resampler |
| Text | `gfx/font.{c,h}` | stb_truetype faces (mmap), per-size glyph cache, ES line layout, wrapping (also between CJK characters), ellipsis, font substitution, per-glyph fallback chain (DejaVu Sans, then the CJK subsets) |
| Translations | `i18n/i18n.{c,h}`, `i18n/i18n_cat.h`, `tools/rsos-i18n.c`, `po/*.po` | `_()` / `N_()` / `C_()` / `_n()`, compiled catalogs (mmap), the language list, plural rules, locale formats (sizes, numbers, dates), language-aware uppercase; see docs/translating.md |
| Images | `gfx/image.{c,h}` | PNG/JPG/GIF/BMP/TGA (stb_image), SVG (nanosvg), pre-scaling, tint, memory + disk cache |
| Themes | `theme/theme.{c,h}` | theme.xml parsing, `<include>`, `${variables}`, `<feature>`, `<resolution>`, per-system lookup |
| UI core | `ui/ui.c`, `ui/ui.h`, `ui/ui_internal.h` | public API, loading, screen stack, modal snapshots, overlays, launching, themes, collections |
| Views | `ui/view_system.c`, `ui/view_gamelist.c`, `ui/widgets.c` | ES system / basic / detailed / video views, help bar, battery, icons, menu style |
| Menus | `ui/menu.c`, `ui/screens.c`, `ui/transfer_ui.c`, `ui/update_ui.c` | generic list menu, dialogs (up to 6 buttons), on-screen keyboard, every settings screen, button test, controller wizard, USB import, web share, system update (the `rsos-update` helper process, docs/updates.md) |
| Data | `ui/games.c`, `ui/systems.c`, `ui/settings.c`, `ui/hw.c`, `ui/xml.c`, `ui/util.c` | ROM scan, gamelist.xml, scan cache, per-game data, core .ini files, settings.ini, battery/storage/rsos.env/WiFi, async helpers |
| Loading | `ui/loader.c`, `ui/fswarm.c` | carousel snapshot (`systems.idx`), background game list loader, on-demand loading, carousel sync; exFAT folder warm-up (§4.1) |
| Input | `input/input.{c,h}`, `input/remaps/*.ini` | evdev devices, merged built-in pad, gamepad mappings, ports, hotkeys, remaps, key repeat (shared with the libretro host) |
| Preview | `tools/uipreview.c` | runs the whole UI on the host with scripted input and writes PNG screenshots |

The UI itself is single-threaded. One worker thread loads game lists in the background (§4.1): it only reads files
and parses them into lists it owns, and the main thread installs them; the transfer module's threads are behind its
status API. No allocation happens per frame except for text caches when a text changes.

## 2. Integration (for `main.c`)

### 2.1 Building
`include ui.mk` from the main Makefile: `UI_OBJS` (all UI objects, no libdrm), `UI_INCLUDES` (`-Isrc`), `UI_LDLIBS`
(`-lm`). `ui.h` includes `power/power.h` and `transfer/transfer.h` for the types only; the frontend links
`POWER_*`/`TR_OBJS` anyway. Standalone: `make -f ui.mk` (host `rsos-uipreview`), `make -f ui.mk ui-arm-check`
(compile + link every UI object for the A20), `make -f ui.mk ui-asan` (preview with ASan/UBSan). Output directory:
`UI_BUILD` (default `frontend/build-ui`; use `~/rsos/...` in WSL). Compiled with `-std=gnu11 -Wall -Wextra -O2
-ftree-vectorize`: zero warnings on host gcc 13 and arm-linux-gnueabihf-gcc 13; also warning-free with the main
Makefile's `-std=c11 -D_GNU_SOURCE`. Define `RSOS_DEFAULT_THEME` / `RSOS_VERSION` to change the defaults.

### 2.2 Wiring

```c
#include "ui/ui.h"          /* also brings input.h, power.h, transfer.h */

static struct ui *ui;
static struct input *in;

static void on_output(const struct display_output_info *now, const struct display_output_info *before,
                      enum display_event_reason why, const struct display_switch_timing *t, void *user)
{
        int w, h;
        bool hdmi = now->type == DISPLAY_OUTPUT_HDMI;

        ui_pick_logical_size(now->width, now->height, &w, &h);   /* 640x480 LCD, 854x480 on 16:9 */
        display_set_game_surface(w, h, DRM_FORMAT_XRGB8888, DISPLAY_SURFACE_CACHED);
        display_set_scaling(DISPLAY_SCALE_ASPECT, (double)w / h);
        power_set_docked(hdmi);
        if (ui) {
                ui_set_size(ui, w, h);          /* full relayout, image cache dir per size */
                ui_set_output(ui, hdmi);        /* also input_set_docked(): player 1, brightness keys */
        }
}

int main(void)
{
        struct ui_config uc;
        struct input_config ic;
        struct ui_power_api pa;
        struct ui_transfer_api ta;

        /* display_init(&dc) with dc.on_output = on_output; power_init(&pc) (callbacks below) */
        input_config_defaults(&ic);          /* handle_power_key = false: the power module owns KEY_POWER */
        in = input_open(&ic);
        ui_config_defaults(&uc);             /* /data/roms, /data/rsos, /usr/share/rsos/..., see ui.h */
        uc.input = in;
        ui_power_api_from_module(&pa);    uc.power = &pa;       /* both must outlive the UI */
        ui_transfer_api_from_module(&ta); uc.transfer = &ta;
        uc.cb.launch = launch_game;          /* see 2.3 */
        uc.cb.setting_changed = setting_changed;  /* hdmi_mode, scaling, p1, cz_buttons, theme, power keys */
        ui = ui_create(&uc);                 /* cheap: settings + core .ini files; lists load from ui_update() */
        if (power_get_mode() == POWER_MODE_CHARGE)
                ui_set_charge_mode(ui, true);  /* on_charge_exit: ui_set_charge_mode(ui, false) */
        /* the first on_output happened in display_init(): call ui_set_size/ui_set_output now */

        for (;;) {
                int t = min_positive(ui_timeout_ms(ui, now_ms()), input_timeout_ms(in), power_timeout_ms());
                /* t also includes transfer_usb_timeout_ms() (a USB disk settling) */
                poll({ display_get_fd(), input_fd(in), power_get_fd(), transfer_usb_fd() }, t);
                ui_set_now(ui, now_ms());          /* the events below happen now */
                display_handle_events();
                power_poll();
                input_poll(in);
                while (input_next_nav(in, &ev))  ui_button(ui, ev.btn, ev.type);  /* calls power_on_input() */
                while (input_next_hotkey(in, &hk)) ui_hotkey(ui, hk);
                /* after the first frame: transfer_usb_init() once, then every iteration: */
                while (transfer_usb_poll(&uev) > 0) ui_usb_event(ui, &uev);
                if (ui_update(ui, now_ms())) {                   /* true: redraw needed */
                        int i = display_begin_frame(-1);
                        const struct display_surface *s = display_get_surface();
                        struct gfx_surface fs;

                        gfx_surface_init(&fs, s->buffers[i].pixels, s->width, s->height,
                                         s->buffers[i].stride / 4);     /* stride in PIXELS */
                        ui_render(ui, &fs);                                /* always a full frame */
                        display_present();
                }
                /* once: if (ui_is_loaded(ui)) run /usr/bin/rsos-boot-ok */
        }
}
```

- `ui_is_loaded()` is true as soon as the carousel is up: at the first `ui_update()` on a normal boot (from the
  snapshot, §4.1), after every list on the first boot. `ui_lists_complete()` is true once every list has been
  loaded and validated. While lists are arriving, `ui_timeout_ms()` asks for a 20 ms poll.
- The UI pauses the background loader around its launch callback (`ui_pause_background()` is public for other
  long blocking work).

- The UI redraws only when `ui_update()` returns true; idle, `ui_timeout_ms()` is -1 and the loop sleeps in `poll()`.
  Animations request 16 ms ticks; the description scroll ~12 fps; toasts/menus with live values 0.5-2 s.
- `ui_render()` draws a complete frame every time, so it works with double/triple buffering.
- Power callbacks → UI: `on_status` → `ui_power_event(ui, UI_PWR_STATUS)`; `on_warning(LOW/VERY_LOW/OK)` →
  `UI_PWR_LOW/VERY_LOW/OK` (toast, persistent banner, clear); `on_critical` → `UI_PWR_CRITICAL`;
  `on_shutdown_request` → `UI_PWR_SHUTDOWN` ("Powering off...") or `UI_PWR_REBOOT` ("Restarting...") (message,
  saves settings/gamedb, flushes caches) then the lead saves the game and calls `power_poweroff()`;
  `on_thermal(hot)` → `UI_PWR_HOT/COOL`; `on_screen(ON)` → `UI_PWR_STATUS`.
  The menu items Restart / Power off call `power_request_shutdown(REBOOT/USER)` through `uc.power`; without it,
  `uc.cb.power` or `/sbin/reboot|poweroff`. There is no Sleep item (and `ui_power_api` has no `sleep` any more).
- `IN_HK_POWER_*` are ignored by the UI (the power module owns the key).

### 2.3 Launching a game
`uc.cb.launch(const struct ui_launch *req, void *user)` receives `rom_path`, `system` (ROM folder name), `core`
(id), `core_path` (.so from the core .ini), `game_name`, `bios_dir`. It may block for the whole game. Before
calling it the UI stops the web share and the name responder, refuses while a USB import runs, and calls
`power_set_game_cpu(cpu_governor, cpu_max_khz)` (optional keys of the core .ini) and `power_set_game_running(true)`
(then `false` after). The lead's callback, per host-design.md: `host_check_game` → `input_set_mode(in,
INPUT_MODE_GAME)`, `input_load_remap(in, system, game_basename)` → display suspend → `host_launch()` → display
resume, `display_set_game_surface()` back to the UI size → `input_clear_remap()`, `input_set_mode(in,
INPUT_MODE_UI)`. Return < 0 if the game could not start. The callback may write a message into `req->message` /
`message_size` (missing BIOS, crash...): the UI shows it (as the error when it returned < 0, as information
otherwise), else a generic error. The UI then records last played / play count and redraws everything.

**Resume.** If `uc.cb.has_resume(req, user)` is set and returns true (main.c: `host_auto_state_path()` under the
states root), the setting `resume_mode` (Settings > Games > On launch) decides: `ask` (default) asks "Resume where
you left off?" [RESUME] [START FRESH] (B cancels), `always` resumes and `never` starts fresh without asking. The
answer goes in **`req->resume`**; the launch callback forwards it as `host_launch_opts.resume`. `req->core_source`
says where the core choice came from (`game`, `system`, `default`, or `resume` for the boot offer).

**Boot resume offer** (host-design.md §7.1). After a power-off during a game, main.c finds `/data/rsos/resume.ini`
once the menu is up (after the first frame, never in charge mode) and calls `ui_offer_resume(ui, &offer)` with the
game name, ROM, system, the core it was saved with and an `answered(resume, user)` callback (main.c deletes the file
there). The UI shows **"Resume <game>?"** [RESUME] [START FRESH]; B = start fresh. RESUME runs the normal launch path
(`launch_req()`: transfer stop, CPU profile, the launch callback, last played; the list entry is looked up, its list
loaded on the spot through `ui_system_ready()` if the background loader has not reached it) with the recorded core,
`req->resume = true` and `req->boot_resume = true`. START FRESH just closes the dialog: the auto state stays, so the
game's own prompt still offers it. `ui_debug_screen()`: `dialog:Resume <game>?|RESUME`.

**Toasts.** `ui_toast(ui, text, UI_SEV_INFO | UI_SEV_WARNING | UI_SEV_ERROR)`: a pill at the bottom, dark / amber /
red, shown 3.5 / 5 / 6 s; the UI's own toasts (`ui_toastf`) 3.5 s, and 6 s for those that ask the user to do
something (`ui_toast_long`: "USB drive ejected: you can unplug it"). Every toast's timer starts at the next
`ui_update()`, so it works from inside the launch callback, before the menu is loaded, and after a long `poll()`
sleep. **The UI clock**: `ui->now` is the time of the last `ui_update()`, which the main loop runs *before* its
`poll()` sleep, while buttons, hotkeys, USB and power events are delivered *after* it; `ui_set_now()` (called by
main.c right after `poll()`) moves the clock to the events' time. Without it a deadline set by an event started in
the past: the missing "USB drive detected" toast of the 2026-09-27 hardware test (rom-transfer.md §4.1), and
shortened "USB drive ejected" toasts.

**Tests.** `ui_debug_screen(ui, buf, n)` describes the top screen: `carousel`, `list:<system>`,
`menu:<title>|<item under the cursor>`, `dialog:<text>|<selected button>`, `progress:import 3/21`, `screen`, plus
` toast=<text>` while a toast shows (screens give it through the optional `screen_ops.describe`). The preview
tool and the headless frontend check it with `expect:` script tokens.

Core choice: per game (game options menu, `gamedb.tsv`) > per system (`core.<system>=` in settings.ini) > the
system default. Only cores whose extensions match the file are offered (a `.neo` in `neogeo` → geolith, a `.zip` →
fbneo); `experimental = true` cores are labelled and never the automatic default (unless the table's default for
that system, e.g. `neocd`, or the only core that accepts the file).

### 2.4 Logical resolution
`ui_pick_logical_size()`: at most 480 lines, the output's aspect ratio, even width: LCD 640x480 → 640x480,
1280x720 / 1920x1080 → 854x480 (scaled ×1.5 / ×2.25 by the display engine). Every theme coordinate is normalized,
so a size change is a full relayout: all pixel caches (images, texts, backdrops, fonts) are dropped and rebuilt
lazily; themes and game lists stay. The disk image cache is per size (`cache/<theme>/<W>x<H>/`), so switching
LCD ↔ HDMI costs a decode only the first time.

## 3. Renderer (`gfx/`)

- **Formats.** Surfaces are XRGB8888 (`stride` in pixels). Images are premultiplied ARGB8888: blending is
  `d = s + d·(255-a)/255`, a tint or global opacity is the same multiply. API colors are straight `0xAARRGGBB`.
- **Primitives.** `gfx_fill` (opaque = store loop, translucent = blend), vertical/horizontal gradients (rows built
  once), anti-aliased rounded rects and strokes (only the corner bands are computed per pixel), circles, glyph
  masks, `gfx_mask_erase` (punched-out button icons), blits (opaque rows = `memcpy`, fully transparent spans
  skipped, translucent runs blended), global-alpha blits, sub-rect blits, tiled blits, nine-patch, nearest-neighbour
  scaled blits (only for transient animation frames), clip rectangles.
- **Speed on the A7.** Two channels per 32-bit multiply (R|B and A|G lanes) with exact /255 rounding. The hot loops
  are written branch-free over plain arrays so GCC vectorizes them: with `-O2 -ftree-vectorize -mfpu=neon-vfpv4`,
  `-fopt-info-vec` reports NEON (16-byte) vectorization for the translucent-run blend, the global-alpha blend, the
  constant-color blend and the fills (92 NEON multiply/add/shift instructions in `gfx.o`). No per-pixel function
  calls; scaling is never done per frame.
- **Resampler** (`gfx_image_scale`, load time only): separable tent filter, radius max(1, scale): area-like when
  shrinking (1920x1080 art to 640x480 without aliasing), bilinear when enlarging; 14-bit fixed-point weights.
- **Text.** stb_truetype at the ES size (EM = pixel size, like FreeType's `FT_Set_Pixel_Sizes`). Faces are
  mmap()ed once, glyphs rasterized on first use into a per-(font, size) hash of 8-bit masks. ES layout rules: line
  box = max glyph height × lineSpacing, capital height centred in it, `w 0` wraps, `w h` with h ≤ 1.2 lines gets an
  ellipsis. **Per-glyph fallback chain** (`font_setup_dir()`): a character the requested font lacks comes from
  DejaVu Sans (Latin, Greek, Cyrillic), then from the three CJK subsets `RSOS-CJK-{JP,SC,KR}.otf` (the one of the
  UI language first, so a Han character gets the Japanese or Chinese shape; §17), else `?`. Lines also break
  between CJK characters (no space needed; closing punctuation never starts a line). NBSP / narrow NBSP (French
  typography) are drawn as spaces and never break. Fonts stb_truetype cannot read (gbz35's
  `RobotoCondensed-Regular.ttf` is really a PostScript Type 1 file, which ES's FreeType reads) are replaced by the
  vendored Roboto Condensed / DejaVu Condensed Bold by name (log once).
- **Static layers.** Every view composites the layers under its first dynamic element (background, pattern,
  panels, extras) into one full-screen backdrop, so a frame is one `memcpy` plus the dynamic parts. Menus and
  dialogs draw over a cached, dimmed snapshot of the view below.

## 4. Caching (boot speed)

| Cache | Where | Key | Content |
|---|---|---|---|
| Images | `/data/rsos/cache/<theme>/<W>x<H>/<key>.rpx` | hash(path, mtime ns, size, target W×H, tint, pipeline version) | 32-byte header + premultiplied ARGB: raw and mmap()ed read-only, or run-length coded (header flag) when that saves ≥ 40 % |
| SVG sizes | same dir, `<key>.inf` | hash(path, mtime, size) | natural width/height (so no SVG parse just to lay out) |
| System backdrops | same dir, `bd-<key>.rpx` | hash(every property of the system's extras, mtimes of their files, size) | the composited extras under the carousel; run-length coded (rsos-dark: 69 KB instead of 1.2 MB) |
| Game lists | `/data/rsos/cache/scan/<system>.bin` | mtimes of every scanned folder, gamelist.xml mtime+size, the cores' extension lists | the parsed list (names, metadata, media paths) |
| Carousel snapshot | `/data/rsos/cache/systems.idx` | the core set (system names + extension lists) and the ROM root | the carousel of the last complete load (§4.1) |
| In memory | refcounted by key | | images shared between views; unreferenced ones dropped on theme/size changes |

Cache files are written without fsync (a torn file fails its header/size/key check and is rebuilt) and synced
once with `syncfs()` when the UI has been idle 1.5 s. User data (settings.ini, gamedb.tsv, mappings, rsos.env,
wpa_supplicant.conf) uses tmp + fsync + rename + fsync(dir). Loading is lazy: themes are parsed per system on first
use; a system's images load when it scrolls into view; game images load when the cursor rests (150 ms) or at once on
a single press; only ±2 systems keep their backdrop in memory.

### 4.1 Loading model (boot)

**Why the old loader took 3.9 s on the device.** Measured on hardware (2026-09-26c logs): 34 ROM folders, almost all
empty, took 3.88 s even from the scan caches, ~110 ms per system whatever it held. The cost is in the kernel's exFAT
driver: the first time a directory is touched in a boot, even by a plain `stat()` of it (`exfat_find()` counts its
subdirectories), `exfat_get_dentry()` → `exfat_dir_readahead()` reads the directory's first cluster with **one
`sb_breadahead()` per 512-byte sector**, i.e. 64 separate requests for the 32 KiB clusters of an 8 GB card. The
A20's SD host takes ~1.6 ms per request, so every cold folder costs ~100-110 ms. Reproduced on the build host with the
same kernel (6.18) on a loop device: `stat` of one cold folder = 64 read requests of 1 sector; the old boot path =
~2400 requests (≈ 3.9 s at 1.6 ms, which is what the device measured); the first-boot "scanned 0 games in 109 ms"
lines are the same effect. Nothing waited on vsync (the per-system time was inside `games_load()`).

**The fix has three parts:**
1. **fswarm** (`ui/fswarm.c`): before touching folders, the UI reads their clusters itself, through the block
   device of the mount, in a few large `pread()`s (it walks the exFAT directory entries: boot sector, entry sets,
   FAT chains; neighbouring folders are merged into one read). That fills the block device's page cache, which is
   the buffer cache the driver reads directories from, so the driver's own lookups then issue no request: the 34
   ROM folders go from 2240 requests to 10 (one 1.2 MB read). Read-only; on any other filesystem or error it does
   nothing. The real fix is a two-line kernel change (a `blk_plug` around the readahead loop, so the 64 requests
   merge; see integration-todo.md); fswarm stays harmless after it.
2. **The carousel snapshot** (`<cache>/systems.idx`, a small text file): at the end of each complete load the UI
   saves the carousel (systems with games, their game and favorite counts, their ROM folders, the collections)
   plus, for each system whose folder was empty with no subfolder, the folder's mtime. At boot the carousel is
   built from it and the menu is drawn at once, **without the "Preparing your console" screen and without any game
   list**: the carousel only needs names and counts. The selected system comes from `last_system` as before.
3. **Background completion**: after the first menu frame, one worker thread loads every list (scan cache when valid,
   else a scan; an empty folder whose mtime has not changed is settled with one `stat()`), the selected system
   first, then its neighbours, then the rest. The worker only does file I/O and parsing into lists it owns; results
   go through a mutex-protected queue and the main thread installs them in `ui_update()`. A list for an entry that
   has none yet is installed at once. Changes to what the carousel shows (a system gained or lost its games, a list
   that changed under a screen using it) wait until the carousel is the only screen, then the carousel is rebuilt
   **keeping the selected system**; collections are built once every list is in, and the snapshot is saved again if
   it changed. Why a thread rather than steps between frames: on the device the cost is blocking SD I/O that cannot
   be sliced (one cold folder was one 110 ms `stat()`), and the carousel must keep animating and taking input. The
   worker is paused while a game runs and stopped (joined) before a synchronous reload or exit.

**Opening a system before its list arrived** (`ui_system_ready()`): the list is loaded on the spot (a few ms from the
scan cache), or, if the worker is on that very system, the UI waits for it; opening a collection first completes
every list. If the list turns out empty (the games were removed while the console was off), a toast says so and the
carousel drops the entry.

**First boot / no snapshot / the core set changed**: the same worker with the "Preparing your console..." screen and
a progress bar until every list is in (then the carousel appears). With fswarm, 34 folders cost ~85 requests instead
of ~2100, so the screen shows for well under a second on the device (estimate) instead of ~2.5 s.

Other boot-path folder touches are warmed the same way: `/data/rsos`, the cache folder and `/data/themes` in
`ui_create()`, the image cache folder when the size is known, the ROM folders of the carousel entries (their
`theme.xml` lookup) before the first theme loads, and the subfolders listed in a scan cache before validating it.

Instrumentation (kept in, a few lines per boot in `frontend.log`): `ui: warm-up ...` and `ui: created in ... (io N
reads, K KiB)`; `ui: menu from the snapshot: ... (warm-up ..., io N reads)`; `ui: first frame in ... (theme parsing,
images decoded vs from the cache, io reads/KiB)`; one `games: <system>: ...` line per system with the time split
(folder lookup, cache read, validation stats, decode, or readdir/gamelist.xml/cache write for a scan); `ui: game lists
complete: ...` (worker time, warm-up reads, carousel unchanged/updated, lists replaced, block-device reads, a digest
of the final state) and `ui: game list time by step`; main.c's `frame N: ... (wait for a buffer, render, present)` for
the first frames and any slow one; `menu up N ms after start` and `game lists complete N ms after start`. The
`io` counters are the read requests of `/sys/dev/block/<dev>/stat` for the device holding `/data`.

Coming back to the carousel re-checks the folder mtimes (a few `stat()`) and rescans what changed; Settings > Game
lists > Refresh forces it (both synchronous, with the worker stopped). The check runs only if the carousel is still
on top at the next `ui_update()`: the USB import pops its menus and pushes its progress screen in one step, and the
copy that starts meanwhile changes the folders; the reload used to pop the progress screen (the import then waited
forever for an answer nobody could give). The import reloads the lists itself when it ends.

## 5. EmulationStation theme support

Theme sets are directories in `/usr/share/rsos/themes/` (built in) and `/data/themes/` (user; same name wins).
Lookup per system, as ES: `<rom folder>/theme.xml`, then `<set>/<theme folder>/theme.xml` for each alias of the
system (e.g. `megadrive`, `genesis`; `fbneo`, `fba`, `arcade`), then (ours, not ES) the folder of a related
console for systems many sets lack (`pcenginecd`→`pcengine`/`tg16`, `neocd`→`neogeo`,
`wonderswancolor`→`wonderswan`, `ngpc`→`ngp`, `gbc`→`gb`, `pico`/`segacd`/`sega32x`→`megadrive`/`genesis`,
`neogeo`→`fba`/`arcade`, `atari7800`→`atari2600`; table in `theme/theme.c`), then `<set>/theme.xml`. Collections use
`auto-favorites` / `auto-lastplayed`. Parse order per file, as ES: `<variables>` (first definition wins),
`<include>` (recursive, relative to the including file), `<resolution>`, `<view>`, `<feature supported="...">`
(all features accepted). View and element `name` lists (`"basic, detailed"`) are split on spaces/commas. Paths: `./`
and bare names relative to the file, `~/` = $HOME, `:/` = `/usr/share/rsos` (fonts). `${system.name}`,
`${system.fullName}`, `${system.theme}` and theme variables are expanded everywhere. The XML parser is our own
(`ui/xml.c`), tolerant of comments before `<?xml?>`, Latin-1 bytes and broken nesting (real themes have all three).
Unsupported element types and properties are ignored and logged once.

### 5.1 Views
| View | Status |
|---|---|
| `system` | Supported: carousel (horizontal/vertical), logos (SVG/PNG, tinted), logoText fallback (auto-shrunk to fit), `systemInfo` ("N games available, M favorites"), per-system extras sliding with the carousel, zIndex split around carousel/systemInfo, help. |
| `basic` | Supported (used when a system has no images/descriptions). |
| `detailed` | Supported (used when a system has scraped media). |
| `video` | Rendered like detailed: `md_video` shows the game's image (`showSnapshotNoVideo`), `md_marquee`, `md_thumbnail`. Chosen only if the theme has no `detailed` view. |
| `grid` | Not supported (falls back to detailed/basic). |
| `menu` | Subset of the Batocera-style menu view (below); otherwise the menu colors are derived from the theme's game list colors. |

Default positions follow ES's code (ISimpleGameListView, BasicGameListView, DetailedGameListView, SystemView,
TextListComponent): list at y 0.2, detailed list at x 0.51, md_image centred at (0.25, 0.4125) max 0.48×0.4,
labels in a 2×4 grid from (0.01, 0.625), values right of their labels, description under the lowest value,
`md_name` off screen, carousel band at y 0.38375 height 0.2325 color FFFFFFD8, logos 0.25×0.155 ×1.2, help at
(0.012, 0.9515).

### 5.2 Elements and properties
| Element | Supported | Partial / ignored |
|---|---|---|
| `image` | pos, size (one axis 0 = keep aspect), maxSize, origin, path, default, tile (native pixel size, like ES), color (baked into the cached image), visible, zIndex | colorEnd/gradientType (drawn with the start color), rotation, rotationOrigin, flipX/Y |
| `text` | pos, size (0 0 / w 0 / w h rules), origin, text, color, backgroundColor, fontPath, fontSize, alignment, forceUppercase, lineSpacing, visible, zIndex | rotation |
| `datetime` | as text, plus format (strftime), displayRelative ("3 hours ago", "never") | rotation |
| `textlist` | pos, size, origin, selectorColor(+End, gradient), selectorImagePath, selectorImageTile, selectorHeight, selectorOffsetY, selectedColor, primaryColor, secondaryColor, fontPath, fontSize, alignment, horizontalMargin, forceUppercase, lineSpacing, zIndex; ES scrolling (cursor centred) and text placement | scrollSound (no UI sounds), marquee scrolling of long names (clipped instead) |
| `rating` | pos, size (star height; ES rules), origin, filledPath, unfilledPath, color, visible, zIndex; fractional stars | rotation |
| `helpsystem` | pos, origin, textColor, iconColor, fontPath, fontSize; our own button icons (A/B/X/Y circles, L/R, START/SELECT pills, d-pad arrows); prompts dropped when too wide; battery shown at the right | entrySpacing, iconTextSpacing, textStyle |
| `carousel` | type (wheels drawn straight), pos, size, origin, color, colorEnd, gradientType, logoSize, logoScale, logoAlignment, maxLogoCount, zIndex; 220 ms eased scroll, 50 % opacity for other logos | logoRotation, logoRotationOrigin, defaultTransition |
| `video` (md_video) | pos, size, maxSize, origin, visible, zIndex, showSnapshotNoVideo | default, delay, showSnapshotDelay (no video playback on the A20) |
| `ninepatch` | pos, size, path, color, visible, zIndex (corners = 1/3 of the image) | |
| `sound` | ignored (no UI sounds) | |
| `imagegrid`, `gridtile` | ignored | |
| md_favorite (carbon, gbz35...) | shown when the game is a favorite | md_kidgame, md_hidden are never shown |
| `menuBackground` / `menuText` / `menuTextSmall` / `menuGroup` (view `menu`) | color; fontPath, fontSize, color, selectedColor, selectorColor, separatorColor; small text color/size; group (bold) font | fadePath, menuSwitch/menuSlider/menuButton/menuIcons images |

### 5.3 Compatibility results (8 themes, 640x480, previews in `docs/ui-previews/`)
| Theme | Result | Notes |
|---|---|---|
| carbon | Correct | carbon fibre tile, red bars, SVG logos, detailed view with image and metadata |
| simple | Correct | "SUPER NINTENDO / ENTERTAINMENT" header texts, stars, metadata grid; long labels wrap/clip as in ES at 640x480 |
| simple-dark | Correct | same layout, dark colors |
| cosmos-ropi | Correct | vertical carousel on the left, full-screen art; its `auto-favorites` has no logo image (text logo shown) |
| gamehistoria | Correct | vertical carousel on the right, pill logos, PNG art with baked text; `<!-- -->` before `<?xml?>` and Latin-1 comments parse fine |
| hyperion | Correct | based on simple; its SNES help color is black on black (theme choice) |
| gbz35 | Correct (owner's theme; **bundled**, §6.1) | carousel band, per-system background.png, selector per system, logo + md_image panel; the Type 1 "ttf" is replaced by the vendored Roboto Condensed (same family); no folder for pcenginecd, neocd, wonderswancolor, pico: the related console's folder is used (§5) |
| gbz35-dark | Correct (**bundled**) | as gbz35 with black panels |

## 6. The built-in themes: `rsos-dark`, `rsos-light`

Our own work (`themes/<name>/_art/*.svg`: gradient background, a diagonal light-band pattern, generic icons, stars,
a white rect for panels) plus **console pictures from Carbon** (`_art/carbon/*.svg`, v2, 2026-09-27): the
per-system controller/console line art of RetroPie's "carbon" theme by Rookervik (CC BY-NC-SA; the same vectors
gbz35 credits to Carbon), with a 4-unit outline stroke added so the lines stay ~2 px wide at ~250 px (collection
folder icons: 2 units, compensating nested transforms), editor metadata removed (each file says so in a
comment). Drawn at (0.5, 0.195), max 0.46×0.28, in the system's accent, plus the same art again in
`iconShade` (white 28 % on dark, black 19 % on light) for a lighter / deeper tint. No console logos or photos;
system names are typography. The layout deliberately follows gbz35, which owners know: system carousel band
across the middle with the big system name, a per-system accent color, the list panel on the right, the
image/metadata panel on the left, a help band at the bottom. Fonts: Roboto Condensed (`:/fonts/...`).
Licence: `themes/rsos-{dark,light}/LICENSE.txt` (the Carbon art CC BY-NC-SA, the rest the project licence).

- `theme.xml` holds all views; a palette in `<variables>` is the only difference between dark and light.
- Each system folder (`nes/theme.xml`, ...) only defines `accent`, `icon` (path under `_art` without `.svg`, e.g.
  `carbon/snes`; default `icon-console`) and `tagline` ("Nintendo · 1990") and includes `../theme.xml`; because the
  first definition of a variable wins, the system values override the defaults. Adding a system = one 10-line
  file. The accent tints the pattern, the icon, the lines, the list selector, the labels and the rating stars.
- A `menu` view styles the settings menu (panel, text, selector).

| Systems | Console art (`_art/carbon/`) |
|---|---|
| nes, fds, snes, n64, gb, gbc, gba, mastersystem, megadrive, gamegear, sega32x, segacd, psx, pcengine, ngp, wonderswan, atari2600, atari7800, atarilynx, msx, arcade, neogeo | Carbon's own art for that system |
| sg1000 → `sg-1000`, coleco → `colecovision`, fbneo → `fba` | Carbon's art (Carbon's folder name) |
| pcenginecd → `pcengine`, ngpc → `ngp`, wonderswancolor → `wonderswan` | same file as Carbon's own folder for them (identical art) |
| neocd → `neogeo`, pico → `megadrive` | alias (Carbon has no art for them) |
| supergrafx, zxspectrum (Sinclair wordmark removed), amstradcpc, c64, scummvm | Carbon's own art for that system (third batch of cores, docs/cores.md) |
| dos → `pc` | Carbon's art (Carbon's folder name) |
| doom, cavestory → `ports` | alias (Carbon's "ports" keyboard art; it has no art for them) |
| pico8 → `icon-pico8`, pokemini → `icon-pokemini` | our own icons (`_art/`, project licence; Carbon has no art for them) |
| auto-favorites, auto-lastplayed, auto-allgames | Carbon's collection folders (star, clock, grid) |
| anything else | our `icon-console.svg` |

### 6.1 Bundled third-party themes: `gbz35`, `gbz35-dark`

RetroStoneOS is free and open source with public builds and the console is sold without firmware, so the owner's
favourite theme ships in the image under its licence (CC BY-NC-SA 3.0: attribution, non-commercial, share-alike).
`themes/gbz35` and `themes/gbz35-dark` are the upstream zips (`es-theme-gbz35-master.zip` commit `300c4b6`,
`es-theme-gbz35-dark-master.zip` commit `aefbbb2`) unmodified, minus `.gitattributes`, plus a `LICENSE.txt`
(author rxbrad; Carbon by Rookervik, Spare by Matt Kennedy, SimpleBigArt by Ewzzy). Note: gbz35's `system.svg`
files are the **system logos** (trademarked wordmarks) and its console art is the cropped `background.png`
behind the carousel; the console vectors themselves come from Carbon (above).

**Switching the default theme**: set `ui_config.default_theme = "gbz35"` (or build with
`-DRSOS_DEFAULT_THEME=\"gbz35\"`). A user choice (`theme=` in settings.ini) always wins; a missing theme falls back
to the default, then to the first theme found. The default stays `rsos-dark` (previews `*-v2-*` and
`gbz35*-bundled-*`, §16).

## 7. Game lists

- **Systems** come from the core .ini files (`/usr/share/rsos/cores/*.ini`): a system is shown if a core lists it
  and its ROM folder has at least one game. A built-in table gives display names, theme folder aliases, the
  default core and ROM folder aliases for RetroPie/RetrOrangePi users (`genesis`→megadrive, `fba`→fbneo,
  `sg-1000`→sg1000, `mame-libretro`→arcade, `32x`, `megacd`, `ps1`, `tg16`, `lynx`, `colecovision`, `neogeocd`...);
  the first existing folder is used. Systems only named in a .ini still work (generic name). Without any .ini (dev
  host) a fallback table is used. 46 systems are described (batch 2 added Pokémon mini after the GBA, SuperGrafx
  after the PC Engine CD, the computers C64, ZX Spectrum, (MSX), Amstrad CPC, MS-DOS and ScummVM together before the
  arcade systems, then PICO-8 and the ports Doom and Cave Story; folders `pc`/`msdos` are read as `dos`, `sgfx` as
  `supergrafx`, `spectrum`/`zx`, `cpc`/`amstrad`, `prboom`, `nxengine`; `host/coreinfo.c` and `transfer/sysmap.c`
  know the same names); the transfer module's table also has `pico`. Preview `b2-fr-carousel-new-systems.png`.
- **Scan**: extensions from every core of the system, plus `.zip/.7z` for cores that do not use archives
  themselves (the host extracts); `.cue` tracks and `.m3u` discs are hidden behind their playlist; hidden files,
  macOS `._*`/`.DS_Store`/`.Trashes`/`.fseventsd`/`.Spotlight-V100` and Windows `System Volume Information`,
  `Thumbs.db`, `desktop.ini`, `$RECYCLE.BIN` are skipped; media folders (`images`, `videos`, `media`, `snap`...)
  are not scanned; arcade BIOS sets (`neogeo.zip`, `pgm.zip`, `aes.zip`...) are not games. Names are cleaned:
  no extension, no `(USA)` / `[!]` tags, `_` → space.
- **gamelist.xml** (Skraper, ES) from the ROM folder or `/data/rsos/gamelists/<system>/`: name, desc, image,
  thumbnail, marquee, video, rating, releasedate, developer, publisher, genre, players, playcount, lastplayed,
  favorite, hidden, kidgame. Entries without a file are dropped (like ES); absolute paths from another machine
  (`/home/pi/RetroPie/roms/snes/...`) are matched by the part after `/<system>/`.
- **Per-game data** (`/data/rsos/gamedb.tsv`, never written into the user's gamelist.xml): favorite, last played,
  play count, core override. Favorites sort first (setting). Collections "Favorites" and "Last played" (50 most
  recent) appear at the end of the carousel when not empty (setting).
  Columns (tab-separated, one game a line; older 6-column files are read as they are):
  `system  rel_path  favorite  last_played  play_count  core  play_time_s  hidden  scale  cpu`
  (batch 2: play time in seconds, hidden 0/1, `scale` = aspect/integer/stretch or empty (the setting), `cpu` =
  performance/powersave or empty (auto)).
- **Sort order** (batch 2, Settings > Game lists > "Sort games by", key `gamelist_sort`): Name (default), Most
  played (play time, then name), Recently played; favorites still first when that setting is on.
- **Play time** (batch 2): the game process counts the time the game really runs (not paused, not in the in-game
  menu, not asleep, not in the switcher) and reports it (`playtime <s>` status lines every 5 min and at exit);
  main.c adds each new part to gamedb.tsv, so a crash loses at most 5 min. Shown in the detailed view
  (`md_lbl_playtime` / `md_playtime`, placed by rsos-dark/-light under the rating; other themes: after the play
  count, "12 (3 h 12 min)") and in the game options ("Play time").
- **Jump to a letter** (batch 2): in a list sorted by name, L1/R1 go to the first game of the previous/next letter
  (accents folded, `#` for digits and signs, wraps around); the letter is shown big over the list for 0.7 s.
  Other sort orders: L1/R1 page as before (L2/R2 always page).
- **Search** (batch 2): game options (SELECT) > "Search this list" opens the keyboard; the list follows each letter
  (case and accents ignored, the count in the list's title: « Recherche : me (2) »); START keeps it, SELECT
  cancels (the list as it was). B in a searched list clears the search first. Settings > "Search all games" searches
  every system into one list ("Search: bo", system names shown). Previews `b2-fr-search.png`, `b2-fr-letter-jump.png`.
- **Hide / Delete** (batch 2, game options): "Hide this game" (gamedb `hidden`; also the gamelist.xml `hidden`
  flag) removes it from the lists; Settings > Game lists > "Show hidden games" (key `show_hidden`) shows them again,
  marked "(hidden)". "Delete this game" asks `Delete "<file>" from the SD card?` [DELETE] [CANCEL] (CANCEL
  selected; for .m3u/.cue: "The discs/tracks it lists are kept"), deletes the file only, then asks "Also delete its
  saves and save states?" [NO] [YES] (NO selected): `.srm`/`.rtc` in `/data/saves/<system>/`, `.state*` (and their
  `.png`) in `/data/states/<system>/`. The game is removed from every list and from gamedb.tsv.
- **Per-game scaling and CPU profile** (batch 2, game options and the in-game menu): "Scaling" (Default (the
  setting) / Aspect / Integer / Stretch) and "CPU profile" (Automatic / Performance / Battery saver), stored in
  gamedb.tsv and passed at launch (`--scale`, `--cpu-profile`); the menu process applies the profile with
  `power_set_game_cpu()` (performance: `performance` governor; battery saver: `schedutil` capped at 720 MHz,
  TODO(hw): check the cap on the A20; automatic: the core's .ini). A change in the in-game menu comes back as a
  `setting scale|cpu <value>` status line and is saved for that game.

## 8. Screens and navigation

System carousel → A: game list → A: launch; B: back; X: favorite; SELECT: game options (favorite, search this
list, core for this game, core for all games of the system, scaling, CPU profile, launch, hide, delete, play time,
file name); left/right: previous/next system; L1/R1: previous/next letter (sorted by name; else page), L2/R2: page;
START: settings (from anywhere). Menus: up/down, left/right change values, A select, B back, START closes all.

**Settings** (one menu for everything):
| Section | Items |
|---|---|
| Display | Brightness (1-10, LCD only; "TV" on HDMI), LCD refresh rate (78 Hz (legacy) / 60 Hz: 60 Hz applies at once, then "Keep this setting? Reverting in 15 s" [KEEP] [REVERT], REVERT selected, the countdown reverts by itself; only KEEP saves it; greyed on HDMI; display-design.md §3.1), HDMI resolution (Auto/720p/1080p), Game scaling (Aspect/Integer), Game list style (Automatic/Basic/Detailed), Battery in games (on/off), Battery position (Top right/Top left/Bottom right/Bottom left) |
| Controls | Button test (all buttons, C/Z, stick; exit: hold SELECT+START), Configure a controller (wizard), Player 1 (Auto (the controller that starts the game) / Built-in / External), Extra C/Z buttons fitted, Controller vibration (on), Players 1-4 (live assignment) |
| Network | WiFi, WiFi network (OSK), WiFi password (OSK, ≥ 8 chars), Ethernet, Bluetooth (each runs `rsos-net {wifi,eth,bt} {on,off}` asynchronously, shows "..." until it returns, toasts the result); Transfer over network, Console name, Stop when idle for, Keep receiving in the background, Windows file share (only when `/usr/bin/rsos-smb` is installed; rom-transfer.md §3.3) |
| Theme | every theme set found (built in + `/data/themes`), applied at once |
| Game lists | Favorites first, Favorites & last played pages, Sort games by (Name / Most played / Recently played), Show hidden games, Refresh game lists |
| Games | Resume games: Auto-save on exit (on), On launch (Ask / Always resume / Always start fresh), Resume on boot (Always / Ask / Never); "Powering off in a game always saves it" (host-design.md §7.1); In game: Fast-forward speed (2x / 3x / 4x) |
| Search all games | (main menu, before Language) the keyboard, then one list of every system's matching games |
| Storage | free space, USB drive: Import games, Export games, Back up saves, Eject USB drive, Ask when a USB drive is plugged in; Advanced: Internal eMMC, M.2 SATA drive (edit `overlays=` in `/boot/rsos.env`, remounting `/` rw for the write, then "Reboot required" [REBOOT NOW] [LATER]) |
| System information | version, battery (%, charging, time left), power source and voltage, storage, display and UI size, games/systems, input devices, CPU temperature |
| System update | (only when `/usr/bin/rsos-update` is there; right after System information, as the main menu has no "System" section) Installed version, Check for updates ("Checking...", "Up to date", "0.2.0 available"; without a network address only the SD card and USB drives are searched, then "Turn WiFi on to check online?" [TURN WIFI ON] [CANCEL], and the check runs once WiFi has an address), Install RetroStoneOS x.y, Check every day (WiFi); "Updates: flash the new image to the SD card" on boards without A/B. The update found: its notes (Markdown cleaned, scrollable, up/down, L/R page) with A = update / B = later; unsigned (development builds): a warning first; a battery under 30 % without charger: refused. Progress: Downloading (MB of MB), Checking the update, Installing (%), Checking the installation, Finishing; B = stop (not in the last step). Then "RetroStoneOS x.y is installed. Restart now to use it?" [RESTART NOW] [LATER], restarting by itself after 30 s. docs/updates.md |
| Date & time | now, time zone (61 zones from the power module), set year/month/day/hour/minute |
| Power | Restart, Power off (confirmation), Dim the screen after, Screen off after, Battery gauge (no sleep entries: the power key powers off, power.md §5-6) |
| Language | every language in its own name (Français, 日本語...), the one in use ticked; applied at once (§17). The item is last (one press of UP from the top: the menu wraps) and reads "Langue (Language)" in any language but English, so it can always be found |

Board-dependent items (`ui_config`, from the board profile, docs/porting.md; the defaults are the RetroStone2's):
Brightness and LCD refresh rate only on a board with a built-in screen (`internal_display` not `none`) whose
backlight the SoC drives (`backlight` not `none`: the RetroStone1's AMT630A sets its own), the LCD
refresh rate only with `internal_refresh_options = <native>,60`, and the Storage "Advanced" toggles only for the
overlays listed in `storage_overlays`.

Other screens: on-screen keyboard (qwerty/shift/symbols, two pages of accented letters (àé key or R: French,
German, Spanish, Portuguese, Italian, Nordic; Polish, Czech, Turkish, Romanian...; SHIFT gives the capitals; not
offered for passwords, which stay ASCII), A type, B delete, X space, Y shift, L show password,
START OK, SELECT cancel), the first-boot language picker (§17), message and confirmation dialogs, toasts, brightness overlay, very-low-battery banner,
"Battery empty, saving..." screen, charge-mode screen (big battery, %, "Full in 1 h 20 min"), the USB dialog
(Import games / Export games / Back up saves / Nothing, on every new stick, plus Install update when a `*.rsu`
is at the stick's root or in its `RetroStoneOS/` folder), Import games (library picker, plan per
system with sizes, unsupported folders, toggles Games/BIOS/Saves & save states/Themes/Screenshots, space check,
Copy/Eject; progress with rate/ETA and cancel; the Skip / Replace / Skip all / Replace all prompt; summary with
Eject), Export games and Back up saves (options, what is already on the drive, FAT32 warnings; progress; summary), Transfer over network (QR code, URL,
`retrostone.local`, PIN, live status, stop/keep in background). A new unknown pad pops "Configure it in Settings >
Controls?".

## 9. Settings keys (`/data/rsos/settings.ini`)

Unknown keys, comments and order are preserved; a save re-reads the file and only rewrites the keys the UI changed
(rsos-net writes `wifi/eth/bt` concurrently).

| Key | Values (default) | Applied by |
|---|---|---|
| `theme` | theme folder (rsos-dark) | UI |
| `language` | `en`, `fr`, `pt_BR`... (none: the first-boot picker) | UI (live), the game process (`--lang`), `--splash` messages |
| `brightness` | 1..10 | UI (sysfs via input) at start and when changed |
| `hdmi_mode` | auto, 720p, 1080p (auto) | lead (setting_changed) |
| `lcd_refresh` | 60, 78 (78) | lead: `display_set_lcd_refresh()` live (the UI sends `setting_changed` on the trial and on revert, saves on KEEP), every display init, and the game children (`--lcd-refresh`); display-design.md §3.1 |
| `scaling` | aspect, integer (aspect) | lead |
| `game_battery_overlay` | on, off (on) | the game process, at game start (host-design.md §8, battery overlay) |
| `game_battery_corner` | top-right, top-left, bottom-right, bottom-left (top-right) | the game process, at game start |
| `gamelist_view` | auto, basic, detailed | UI |
| `p1` | auto (the controller that starts the game is player 1: main.c passes `input_last_source_id()` to the game as `--p1-device`), builtin, external (auto) | input (`input_set_p1_policy`) + lead |
| `cz_buttons` | 0, 1 (0) | input (`input_set_cz_buttons`) |
| `favorites_first`, `collections` | 0/1 (1) | UI |
| `autosave_exit` | 0/1 (1): write `.state.auto` when the player leaves a game (a power-off always does) | the game process, at game start (host-design.md §7.1) |
| `resume_mode` | ask, always, never (ask): what launching a game with an auto state does | UI (launch) |
| `resume_boot` | ask, always, never (ask): the game running at the last power-off (`resume.ini`), at the next boot. always = relaunched at once with `--load-state auto` once the menu is up (not in charge mode, no dialog); ask = "Resume <game>?" [RESUME] [START FRESH] [ALWAYS RESUME] [NEVER ASK] (the last two act now and save the setting, toast "Setting saved: you can change it in Settings > Games"); never = the menu, the auto state kept (the launch prompt still offers it) | UI (`ui_offer_resume`) |
| `ff_speed` | 2, 3, 4 (3): fast-forward speed (Select+R2) | the game process, at game start |
| `rumble` | 0/1 (1): controller vibration | the game process, at game start (input-design.md §2) |
| `gamelist_sort` | name, playtime, lastplayed (name) | UI (lists re-sorted at once) |
| `show_hidden` | 0/1 (0): list hidden games (marked "(hidden)") | UI (lists reloaded) |
| `last_system` | system name | UI (carousel position at start) |
| `core.<system>` | core id ("" = default) | UI (launch) |
| `wifi`, `eth`, `bt` | 0/1 | written by rsos-net, read by the UI |
| `idle_dim_min`, `idle_off_min`, `battery_gauge`, `timezone` | see power.md §15 | power module (`power_set_setting`, at start and when changed) |
| `sleep_timeout_min`, `sleep_wake` | no longer shown (no sleep mode); still forwarded at start if present | power module |
| `usb_import_prompt` (1: the USB dialog; 0: a toast), `webshare_bg` (0), `webshare_idle_min` (30), `hostname` (retrostone), `smb` (0: the Windows file share started with the network transfer, rom-transfer.md §3.3) | see rom-transfer.md §4.2 | UI / transfer module |
| `update_auto` | 0/1 (1): the daily update check when WiFi is connected (3 min after start at the earliest, never during a game; a toast once per new version) | UI (update_ui.c) |
| `update_last_check`, `update_notified` | the time of the last daily check (epoch), the version the toast was shown for | UI (update_ui.c) |

Other files: `/data/rsos/gamedb.tsv`, `/data/rsos/wpa_supplicant.conf` (the UI writes one network; rsos-net starts
wpa_supplicant with it), `/data/rsos/input/<guid>.cfg`, `/data/rsos/remaps/`, `/data/rsos/cache/`,
`/data/rsos/resume.ini` (main.c: the game running at the last power-off, deleted once the boot offer is answered).

## 10. Input (`input/`)

One layer for the UI and the libretro host (docs/input-design.md). evdev only; `/dev/input` is watched with inotify
(nodes that are not ready yet are retried for 2 s).

| Source | Mapping |
|---|---|
| Built-in (`RetroStone2*` devices merged into one pad, `analog-stick`) | BTN_EAST=A, BTN_SOUTH=B, BTN_NORTH=X, BTN_WEST=Y, BTN_TL/TR=L/R, BTN_TL2/TR2=L2/R2, BTN_SELECT/START, BTN_DPAD_*, BTN_C=L3, BTN_Z=R3. Stick: ABS_X/Y; a value outside 0..3000 (or the absinfo range) means "not fitted". KEY_BRIGHTNESSUP/DOWN change `/sys/class/backlight/*/brightness` by 10 % (min 1; disabled when docked) and raise `IN_HK_BRIGHTNESS` (overlay). |
| 1. user mapping | `/data/rsos/input/<guid>.cfg`: one SDL mapping line, written by the Configure controller wizard |
| 2. SDL GameControllerDB | `gamecontrollerdb.txt` (Linux lines, vendored). GUID from `input_id` as SDL builds it; matched ignoring the name-CRC field, then also the version. SDL targets are positional: `a` (bottom) → RetroPad B, `b` (right) → A, `x` (left) → Y, `y` (top) → X. Buttons `bN` (SDL index order), hats `hH.M`, axes `aN`, `+aN`/`-aN`, `~` |
| 3. Linux gamepad spec | BTN_GAMEPAD devices: codes used directly (positional); ABS_Z/RZ are triggers when the pad has ABS_RX, else the right stick; ABS_GAS/BRAKE triggers; hats = d-pad |
| 4. unknown | sticks and hats still navigate; `IN_HK_PAD_UNCONFIGURED` → the UI offers the wizard |
| Keyboards (UI only) | arrows, Enter=A, Esc/Backspace=B, x/y, PgUp/PgDn=L/R, Tab=Select, Space=Start, Home/End=L2/R2 |

- **Ports** (1..4): handheld: built-in = P1, external pads P2.. in connection order; docked (HDMI) with policy
  auto, or policy external: the first external pad is P1 and the built-in pad last. `IN_HK_PORTS_CHANGED` on change.
- **Host API** (per frame): `input_poll()`; `input_port_buttons(port)` (RetroPad bitmask, = `JOYPAD_MASK`),
  `input_port_analog(port, stick, axis)`, `input_port_analog_button()` (analog L2/R2), `input_port_has_analog()`,
  `input_set_dpad_to_analog(port, on)` (N64 without a stick), `input_next_hotkey()`.
- **Hotkeys** (game mode, from the built-in pad and P1): Select+Start exit, +R save state, +L load state,
  +Right/Left slot, +X menu, +B reset; the combo button is suppressed until released, Select alone reaches the core.
- **UI navigation**: `input_next_nav()` gives press / release / repeat (400 ms, then every 70 ms, on the d-pad,
  L/R, L2/R2) merged over all pads; the left stick navigates with hysteresis (on at 50 %, off at 35 %).
- **Configure controller**: `input_capture_begin(slot)` reports raw elements (`b3`, `h0.1`, `+a2`) of that pad
  while the built-in pad keeps working; the wizard asks for 20 targets (optional ones skip after 6 s) and saves
  with `input_save_user_mapping()`.
- **Power key**: owned by the power module; `input_config.handle_power_key` (default false) keeps this layer away
  from `axp20x-pek` (set it for standalone use: then short press = `IN_HK_POWER_SHORT`, 2 s = `IN_HK_POWER_OFF` +
  `on_power_off()` callback).

### 10.1 Remap files
`input_load_remap(in, system, game)` loads the first existing file of
`/data/rsos/remaps/<system>/<game>.ini`, `/data/rsos/remaps/<system>.ini`, `/usr/share/rsos/remaps/<system>.ini`,
trying `<system>-cz` first at each level when `cz_buttons=1`. `input_clear_remap()` returns to identity.

```ini
; physical RetroPad position = RetroPad id sent to the core
[remap]
r3 = y      ; names: b y select start up down left right a x l r l2 r2 l3 r3 (l1/r1 accepted), or none
l3 = r
select = none
[options]
dpad_to_analog = 1   ; d-pad drives the left stick on ports without an analog stick
```
Samples: `frontend/src/input/remaps/megadrive.ini` (identity) and `megadrive-cz.ini` (6-button face per
input-design.md §1b).

## 11. Timings

Measured with `rsos-uipreview --timing / --bench` on the build host (AMD Ryzen 7 9800X3D, WSL2, ext4), with a
3000-game library (12 systems × 250 files, a 1000-entry gamelist.xml with 250 matches and images), 640x480.

| | rsos-dark | gbz35 | carbon | cosmos-ropi |
|---|---|---|---|---|
| First boot, process start → first carousel frame | 38 ms | 31 ms | 27 ms | 63 ms |
| of which: game list scan (3000 games) | 8 ms | 8 ms | 8 ms | 8 ms |
| of which: image decode + scale (SVG raster, PNG/JPEG) | 22 ms (5 SVGs) | 10 ms (13 files) | 7 ms (14) | 41 ms (17) |
| Second boot, process start → first carousel frame | 4 ms | 3 ms | 3 ms | 3 ms |
| of which: game lists from the scan cache | 1.4 ms | 1.3 ms | 1.3 ms | 1.4 ms |
| Theme XML, all systems used by the first frame | 0.6 ms | 0.4 ms | 0.5 ms | 0.6 ms |
| Full static frame (backdrop + carousel/list + help) | 0.11 ms | 0.11 ms | 0.11 ms | 0.08 ms |
| Carousel scroll frame (warm) | 0.13 ms | 0.13 ms | 0.13 ms | 0.08 ms |
| First scroll to a new system (its backdrop/logos from cache) | 1.1 ms | 0.6 ms | 0.5 ms | 0.1 ms |
| Game list scroll frame (key repeat, images deferred) | 0.11 ms | 0.09 ms | 0.10 ms | 0.06 ms |

`ui_create()` itself is 0.2 ms (settings, gamedb, 25 core .ini files). Note: themes on WSL's `/mnt/c` cost 2.8 ms
per file open; the numbers above are from ext4, like the device.

**Cortex-A7 @ 1 GHz estimate.** The host core is ~20-25× faster on this kind of code (CoreMark ~3000 on an A7 @
1 GHz vs ~65-70 k on a Zen 5 core) and its memory ~25× faster (A20 DDR3 `memcpy` ≈ 0.5 GB/s); the SD card adds
its own time. Estimates:

| | A7 estimate |
|---|---|
| Second boot, UI part of "power on → menu" | 0.1-0.25 s: ~80 ms CPU (cache reads, text logos, first frame) + reading ~1.5 MB (one 1.2 MB backdrop, logos, scan caches) from the SD card |
| First boot (empty caches) | 0.7-1.5 s for the images (SVG rasterization dominates) + the SD readdir of the ROM folders; the "Preparing your console" screen shows meanwhile |
| Full frame / carousel scroll frame | 3-5 ms (one 1.2 MB copy + blends): 60 fps with margin |
| First scroll to a system not seen yet this boot | 15-30 ms (one late frame) |
| Game list key-repeat frame | 3-5 ms; a game image decoded on rest: 30-80 ms for a 640x480 PNG the first time, then from cache |
| Theme switch | 50-150 ms warm, ~1 s the first time |

TODO(hw): measure on a unit (`rsos-frontend` logs "first frame in N us", and the preview tool's `--bench` code can
run on the device against a real /data). Backdrops are now run-length coded (§4).

**Boot on an exFAT card (2026-09-27).** The headless frontend (`--headless --root`) on the owner's layout (34 ROM
folders, 5 with 15 games, rsos-dark), /data on exFAT on an emulated SD card (loop → dm-delay 2 ms → loop with direct
I/O; the request counts are the reliable figure: the device measured ~1.6 ms per request, 2403 requests = its 3.9 s):

| | before | after |
|---|---|---|
| Normal boot: read requests to the SD card, whole boot | 2403 (all before the menu) | ~100 (~20 before the menu) |
| Normal boot: process start → menu visible (emulated card) | 292 ms (behind "Preparing") | 67 ms (the carousel, lists still loading) |
| Normal boot: → every list loaded/validated | 277 ms | 109 ms (in the background) |
| Normal boot: first menu frame reads | 1.2 MB backdrop | 69 KB backdrop (1 request) |
| First boot: read requests | 2118 | 85 |
| Same, on ext4 (CPU only): menu visible / every list | 4 ms / 2 ms | 5 ms / 25 ms (worker starts after the first frame) |

Device projection for a normal boot (to be checked on the next hardware log): ~20 SD requests and ~0.4 MB before the
menu instead of ~2400 requests and 1.2 MB, i.e. the menu within ~0.3 s of the logo instead of ~4.3 s.

## 12. Testing

- `rsos-uipreview` (host): `--root DIR` (fake `/data`, cores, sysfs, rsos.env), `--theme`, `--size WxH`,
  `--keys "right a down shot:x.png start ..."` (tokens: buttons, `hold:btn:ms`, `combo:b1+b2:ms`, `wait:ms`,
  `shot:file`, `size:WxH`, `theme:name`, `hdmi`, `lcd`, `usb`, `unplug`, `charge`, `toast:sev:text`, `idle:ms` (time
  passes *without* `ui_update()`, like the device's `poll()` sleep before an event), `expect:text` (the run fails
  unless `ui_debug_screen()` contains the text; `_` = space), `usbtrees:N`, `usbfs:NAME`), `--resume` (every game
  has an auto state: exercises the prompt), `--fake-transfer` (a canned USB drive, library search, plan, import with
  four duplicate questions, export and saves backup, web share; it prints `ANSWER`, `PLAN`, `BACKUP`, `REMOUNT` lines
  for the checks), `--timing`, `--bench`, `--png-bits`. Virtual time: animations complete between tokens.
- Test tree (WSL, `~/rsos/ui-test/`): fake ROMs for 13 systems (with junk files, a `.cue`+`.bin`, an `.m3u`, an `fba`
  alias folder, arcade BIOS zips), a SNES gamelist.xml with metadata, generated screenshots (PNG and JPEG), a
  foreign absolute path and a missing file, the real core .ini files, a fake battery; the 8 third-party themes.
- `make check-loader` (`frontend/tests/test_loader.c`): a 34-folder library; first boot (carousel only when complete,
  snapshot written); snapshot boot with the loader slowed down (`RSOS_LOADER_DELAY_MS`): the carousel before the
  lists, at the saved position, final state digest = full load; library changed while off (a system gains games,
  one loses them, a gamelist.xml changes): stale carousel first, then rebuilt keeping the selected system, final
  state = a full load without snapshot; opening a system before its list (loaded on the spot) and a collection
  (every list first); RLE cache files (round trip, raw fallback, corrupt = miss). Clean under ThreadSanitizer and
  ASan/UBSan. `check-frontend` step 1a does the same with the real main loop (menu up "game lists still loading",
  same digest and same pixels as a full load).
- `make check-ui` also runs the USB screens with the fake transfer module: the USB dialog after 9 s of `idle:` (the
  regression for the missing "USB drive" message), B, unplug; the toast with `usb_import_prompt=0`; the library
  picker, the four duplicate answers (checked from the printed `ANSWER` lines), no picker with one library; export and
  saves backup with exactly two read-write and two read-only remounts. `check-frontend` steps 4 and 4a run a fake
  USB stick through the real main loop (see rom-transfer.md §5).
- `make check-frontend` step 5 (resume, 2026-09-27): a power-off during a game (`game:poweroff` while the slow test
  core runs) writes `resume.ini`; the next boot shows "Resume Smoke Test?" and RESUME launches with
  `--load-state auto`; START FRESH and B keep the auto state and drop the offer; a missing ROM gives no dialog;
  the launch prompt under `resume_mode` ask / always / never; Settings > Games > Auto-save on exit off → no auto
  state at exit, on → written.
- `make check-ui` (2026-09-27): Settings > Display > LCD refresh rate: 60 Hz on trial (`SETTING lcd_refresh=60`),
  the dialog counting down, gone after 15 s with `lcd_refresh=78` sent back and nothing saved; B on the dialog also
  reverts; KEEP saves `lcd_refresh = 60` and nothing is sent back after 15 s; 78 Hz applies and saves at once.
  `check-frontend` step 6: the same in the real main loop (headless fake output `640x480@60.000`), the game child
  gets `--lcd-refresh 60`, the next boot starts at 60 Hz. Dialogs can count down (`dialog_set_countdown()`: `%d` in
  the text, a choice when it runs out) and preselect a button (`dialog_select()`).
- `make check-i18n` (2026-09-27, docs/translating.md): every marked string is in `po/rsos.pot`; every `.po` compiles
  with no format error; every translated character is in the fonts; `tests/test_i18n.c` (a test catalog: round trip,
  contexts, escapes, a positional format, the bad entries the compiler drops, fallback to English, a catalog reused
  after a switch; the plural forms of fr, pl, ru, uk, cs in the real catalogs; French sizes and numbers; the
  language-aware uppercase; CJK, Greek and Cyrillic glyphs drawn through Roboto Condensed + the fallback chain
  (non-empty boxes, not the `?` glyph); CJK wrapping); the first-boot picker (`--language-prompt`: English
  highlighted, B does nothing, the title previews the language under the cursor, A saves `language = fr` and the
  menu comes up in French; no picker once a language is set); the live switch (Settings > Langue (Language) >
  Deutsch: the menus reopen in German, then `lang:en`).
- `make check-update-ui` (in `check-update`, 2026-09-27): Settings > System update with the preview tool and a fake
  helper (`tests/fake-rsos-update.sh`, `RSOS_UPDATE_SYNC=1`: the preview runs on virtual time, the helper in real
  time): check -> the notes -> install (the helper gets `apply --url ... --name ... --size ...`) -> the restart dialog
  -> Later; up to date; a failed install (battery); a board without A/B; "Updated to RetroStoneOS 0.2.0" once after
  a restart; the USB dialog with a package (Install update -> `check --local-only --dir <stick>`) and without.
- `make check-b2` (in `make check`, batch 2, `frontend/batch2.mk`): `check-b2-unit` (`tests/test_b2.c`: the 10 new
  systems with the real core .ini files (names, makers, default cores, carousel order, `pc/` read as dos), letter
  groups, accent-folding search, gamedb.tsv 10 columns and the 6-column format, the 3 sort orders, hidden games,
  removal); `check-rumble` (`tests/test_rumble.c`, a uinput pad with FF_RUMBLE); `check-smb` (`tests/smb-test.sh`,
  the rsos-smb helper with stub commands); `check-b2-ui` (`tests/b2-ui.sh`, preview tool: jump to letter with
  wrap-around, live search (counts, accents, no match, cancel, B clears), Search all games, the new settings,
  most-played order, the Windows share toggle started/stopped with the network transfer, no helper → no item);
  `check-b2-frontend` (`tests/b2-frontend.sh`, the real main loop headless: play time saved, the switcher's
  relaunch (exit 6, the left game saved even with auto-save off, the chosen one resumed), Resume on boot
  always/never/ask with ALWAYS RESUME and NEVER ASK, per-game scaling/CPU passed at launch and saved from the
  in-game menu, Hide (and Show hidden games), Delete with the saves question YES/NO and CANCEL).
- A full tour of every screen under ASan/UBSan (`make -f ui.mk ui-asan`) runs clean; unit checks of the path,
  XML, settings-merge, name-cleaning, color, blend and resampling helpers, and of the input mapping code (GUIDs,
  SDL mapping lines, gamecontrollerdb lookup, remap files) pass.
- Not tested on hardware: evdev devices, backlight, rsos-net, rsos.env remount, the power and transfer modules
  (fakes only), real frame times.

## 13. Licences

Vendored code: see `frontend/third_party/README.md` (stb_image, stb_truetype, stb_image_write: public domain/MIT;
nanosvg: zlib; DejaVu fonts: Bitstream Vera licence; Roboto Condensed: Apache-2.0; SDL GameControllerDB: zlib).

Third-party themes were used for compatibility testing from clones in `~/rsos/ui-test/data/themes/`. Since
2026-09-27 gbz35 and gbz35-dark are in the repository (`frontend/themes/`, with their `LICENSE.txt`) and so is
Carbon's controller art, adapted, in `frontend/themes/rsos-*/_art/carbon/` (see below and §6); the others are not.

| Theme | Source (commit) | Licence found | Notes |
|---|---|---|---|
| carbon | github.com/RetroPie/es-theme-carbon (`b09973e`) | CC BY-NC-SA (readme; "2.0" in the summary, 4.0 legal text) | based on simple; contains console logos (trademarks); its `*/art/controller.svg` (no logos) are adapted in rsos-dark/-light |
| simple | github.com/RetroPie/es-theme-simple (`5a6c1da`) | CC BY-NC-SA (same readme text) | |
| simple-dark | github.com/RetroPie/es-theme-simple-dark (`058472c`) | CC BY-NC-SA (inherits simple) | |
| cosmos-ropi | github.com/retr0rangepi/es-theme-cosmos-ropi (`ed77a5a`) | none stated (no licence file, README is a title) | photos and logos of third parties |
| gamehistoria | github.com/retr0rangepi/es-theme-gamehistoria (`ba0c9f6`) | none stated ("based on the showcase theme by dmmarti") | |
| hyperion | github.com/retr0rangepi/es-theme-hyperion (`5af70dc`) | CC BY-NC-SA (README) | "logos and trademarks are copyright of their respective owners" |
| gbz35 | rxbrad/es-theme-gbz35 (zip in the repo root, commit `300c4b6`) | CC BY-NC-SA 3.0 (README) | derives from Carbon, Spare, SimpleBigArt; **bundled** (`themes/gbz35`, §6.1) |
| gbz35-dark | rxbrad/es-theme-gbz35-dark (zip, commit `aefbbb2`) | CC BY-NC-SA 3.0 | **bundled** (`themes/gbz35-dark`) |

All eight are non-commercial or unlicensed and contain trademarked logos. **Owner's decision (2026-09-27)**:
RetroStoneOS is free and open source with public builds and the console is sold without firmware, so CC BY-NC-SA
material may ship in the free image under its terms (attribution, non-commercial, share-alike). Bundled since then:
gbz35 and gbz35-dark (unmodified, `LICENSE.txt` added), and Carbon's controller art (adapted) in rsos-dark /
rsos-light (§6). The unlicensed ones (cosmos-ropi, gamehistoria) still cannot ship.

## 14. Requests to other owners

- Display: `display_set_active()` for sleep/idle-off (power.md), and a way to keep the UI surface across a game
  launch (today the lead re-creates it after `host_launch()`).
- Build: install `third_party/fonts/*.ttf` → `/usr/share/rsos/fonts/`, `third_party/sdl-gamecontrollerdb/gamecontrollerdb.txt`
  → `/usr/share/rsos/gamecontrollerdb.txt`, `themes/*` → `/usr/share/rsos/themes/`, `src/input/remaps/*.ini` →
  `/usr/share/rsos/remaps/`, and `/etc/rsos-version` (shown in System information).
- Cores: optional `cpu_governor` / `cpu_max_khz` keys in `[core]` are read and passed to `power_set_game_cpu()`.
- rsos-net: a non-persistent status for the WiFi SSID/IP would let System information show the address.
- `data-partition`'s folder list should match the systems in `ui/systems.c` (it creates `atari2600`,
  `pcengine`...; the UI hides folders without a core anyway).

## 15. Known limitations

No video playback (snapshots instead), no UI sounds, no grid view, no image rotation, no carousel wheels (drawn
straight), no marquee scrolling of long list names, image `colorEnd` gradients drawn flat. The description
auto-scroll and the help bar follow ES closely but not pixel-exactly. TODO(hw): everything in §12 "not tested".

## 16. Previews (`docs/ui-previews/`)

| File | Content |
|---|---|
| `rsos-dark-system.png`, `rsos-dark-gamelist.png`, `rsos-dark-settings.png`, `rsos-dark-system-854x480.png`, `rsos-dark-gamelist-854x480.png` | built-in dark theme |
| `rsos-light-*.png` (same set) | built-in light theme |
| `rsos-dark-v2-system-<s>.png`, `rsos-light-v2-system-<s>.png` for snes, nes, gb, megadrive, n64, psx; `rsos-{dark,light}-v2-system-snes-854x480.png` | v2 console art (Carbon, §6), 3-game library |
| `gbz35-bundled-system-<s>.png`, `gbz35-dark-bundled-system-<s>.png` (same six systems) | the bundled gbz35 themes, same library (5 bits per channel) |
| `rsos-dark-game-options.png`, `rsos-dark-web-transfer.png`, `rsos-dark-charging.png`, `rsos-light-button-test.png` | other screens |
| `rsos-dark-resume-boot.png`, `rsos-dark-resume-prompt.png`, `rsos-dark-settings-games.png`, `rsos-dark-resume-boot-854x480.png` | resume: the boot offer after a power-off in a game, the launch prompt, Settings > Games (rendered by the headless frontend) |
| `rsos-dark-update-menu.png`, `rsos-dark-update-notes.png`, `rsos-dark-update-restart.png`, `rsos-dark-update-usb-dialog.png` and the same `i18n-fr-update-*.png` | Settings > System update: the menu after a check, the notes of the update found, the restart dialog, the USB dialog with a package (docs/updates.md) |
| `rsos-dark-usb-dialog.png`, `rsos-dark-usb-picker.png`, `rsos-dark-usb-import.png`, `rsos-dark-usb-duplicate.png`, `rsos-dark-usb-duplicate-save.png`, `rsos-dark-usb-export.png` (FAT32 stick: files over 4 GB), `rsos-dark-usb-progress.png`, `rsos-dark-usb-saves.png` (last played game), `rsos-dark-usb-summary.png` | USB drive: the plug-in dialog, the library picker, the import plan, the duplicate prompt (game, save), export games, progress, back up saves, a summary |
| `<theme>-system.png`, `<theme>-gamelist.png` for carbon, simple, simple-dark, cosmos-ropi, gamehistoria, hyperion, gbz35, gbz35-dark | compatibility (posterized to 5 bits per channel to keep the files small) |
| `i18n-<lang>-system.png`, `i18n-<lang>-gamelist.png`, `i18n-<lang>-settings.png`, `i18n-<lang>-dialog.png` for fr, de, ru, ja, zh_CN | translations (§17), rsos-dark, 640x480, 6 bits per channel |
| `i18n-fr-*.png`: `game-options`, `display`, `controls`, `network`, `keyboard`, `keyboard-accents`, `settings-games`, `storage`, `info`, `datetime`, `power`, `language`, `toast`, `charging`, `button-test`, `resume-prompt`, `usb-dialog`, `usb-import`, `web-transfer`, `lcd-dialog`, `first-boot-language`, `ingame-menu`, `ingame-menu-options`; `i18n-en-first-boot-language.png` | every French screen touched, for the owner's review; the first-boot picker in English (as it opens) and on Français |
| `b2-fr-switcher.png` (rsos-run `--switcher-shot`), `b2-fr-resume-boot-4.png` (headless frontend), `b2-fr-search.png`, `b2-fr-letter-jump.png`, `b2-fr-game-options.png`, `b2-fr-playtime.png`, `b2-fr-settings-games.png`, `b2-fr-carousel-new-systems.png` | batch 2 in French: the game switcher (Select+Y), the 4-choice boot offer, a live search, jump to letter, the game options (scaling, CPU profile, hide, delete, play time), the play time in the detailed view, Settings > Games, a new system (MS-DOS) in the carousel |

## 17. Languages (2026-09-27)

22 languages (English + 21 translated by AI, review welcome; the list, how to improve or add one:
docs/translating.md): Bahasa Indonesia, Čeština, Dansk, Deutsch, English, Español, Français, Italiano, Nederlands,
Norsk bokmål, Polski, Português (Brasil), Português (Portugal), Suomi, Svenska, Türkçe, Ελληνικά, Русский,
Українська, 日本語, 简体中文, 한국어. **Right-to-left scripts (Arabic, Hebrew) are out of scope**: they need shaping and
the bidi algorithm, which the renderer does not have.

- **Mechanism** (`src/i18n/`): `_()`, `N_()`, `C_()`, `_n()` over English msgids; `po/<code>.po` compiled at build
  time by `rsos-i18n` (`src/tools/rsos-i18n.c`, build machine, no dependency) into `<code>.cat` (~50 KB each; a
  hashed table, mmap()ed, validated once at load; `i18n_cat.h`) in `/usr/share/rsos/locale`. The compiler drops a
  translation whose printf conversions differ from the English (the English is shown), so a catalog cannot crash
  the frontend; translators may reorder with `%2$s`. Plural forms come from each file's `Plural-Forms` (a small C
  expression evaluator: 1 form for ja/zh/ko/id, 2 for most, 3 for pl, cs, ru, uk). Catalogs are never unmapped:
  a returned string stays valid after a language switch. Lookups are lock-free (the transfer worker reads the
  catalog too). `make pot` extracts the marked strings (604) into `po/rsos.pot`; `make check-i18n` fails when a
  marked string is missing from it, a `.po` has an error, or a translated character is in no font.
- **What is translated**: every screen of the menu (settings, dialogs, toasts, help bar, the keyboard, the
  charge screen, the USB and network transfer screens, the resume dialogs, the boot notes, "Preparing your
  console"), the in-game menu and toasts of the game process (host-design.md §11) and the messages it sends back
  (BIOS, crashes), the two `--splash --message` texts of `data-partition` (when the language is known). Not: logs,
  game/core/controller names, core options, ES theme texts (as their authors wrote them; our `rsos-*` themes' few
  labels and taglines are translated, context `theme`), the network transfer's web page.
- **System names** keep their English (the table in `systems.c`, marked `NC_("system", ...)`); a language may give
  its regional name (Japanese ファミリーコンピュータ / スーパーファミコン / メガドライブ, Chinese 红白机 / 超级任天堂,
  Brazilian "Fliperama" for Arcade...). `se->fullname` holds the display name (`loader_relabel()` on a switch);
  the theme variable `${system.fullName}` follows it.
- **Formats**: sizes `i18n_format_size()` ("1,5 Go" in French, "1,5 GB" in German, "1.5 GB" in English), numbers
  with the language's separators, dates: the language's short date (context `strftime`) for Settings > Date & time,
  the in-game slot preview and theme datetimes that set no `format` (ES's `%m/%d/%Y` stays the English default);
  relative dates ("il y a 3 jours") with plurals. Uppercase (`utf8_upper()` → `i18n_upper()`): Latin-1/Extended-A,
  Greek without accents, Cyrillic, the Turkish İ/ı.
- **Settings > Language** (§8) and **the first-boot picker**: `language` absent from settings.ini (a first boot,
  or an update from a version without it) and `ui_config.language_prompt` set (main.c) → once the carousel is up, a
  full-screen list of the languages in their own names, English highlighted, big rows (D-pad, A or START; B does
  nothing: a choice is needed). The title "Choose your language" and the help switch to the language under the
  cursor as it moves, so a child who cannot read English recognises theirs. The boot notes and the resume offer wait
  for it (`ui_first_boot_busy()`). `ui_debug_screen()`: `language:<title>|<code>|<name>`.
- **Live switch** (`ui_set_language()`, no restart): the catalog, the CJK font order, the carousel names
  (`loader_relabel()`), themes (reloaded: `${system.fullName}` and our themes' labels), views, icons and fonts are
  rebuilt like for a theme switch; open menus are closed and Settings > Language reopened in the new language; the
  system view's backdrop cache key includes the language (its taglines and names are drawn into it). The game
  process gets `--lang` at every launch.
- **Fonts** (`gfx/font.c`, §3): per glyph, the theme font, then DejaVu Sans (Latin, Greek, Cyrillic: covers
  every European translation; Roboto Condensed, the rsos themes' font, has Greek and Cyrillic too), then the CJK
  subsets `RSOS-CJK-JP.otf` (kana + the 2965 kanji of JIS X 0208 level 1 + every character of ja.po: 680 KB),
  `RSOS-CJK-SC.otf` (the 3755 hanzi of GB 2312 level 1 + zh_CN.po: 810 KB), `RSOS-CJK-KR.otf` (the 2350 Hangul
  syllables of KS X 1001 + ko.po: 273 KB), the language's own first (a Han character gets its Japanese or Chinese
  shape). They are subsets of Noto Sans CJK (SIL OFL, renamed "RetroStone CJK") made by `po/mkcjkfont.py`
  (`make cjk-fonts`; no hinting, no OpenType layout tables: stb_truetype uses neither), and are only opened when a
  character needs them. Game names in these scripts render too when their characters are in the common sets.
- **Keyboard**: two pages of accented letters (§8); passwords stay ASCII.
- **Cores**: `RETRO_ENVIRONMENT_GET_LANGUAGE` answers the language (`RETRO_LANGUAGE_*`; Danish, which has none:
  English).
- **Cost**: the catalogs are ~1.1 MB in all (21 × ~50 KB), the CJK fonts 1.76 MB; `ui_create()` adds one open +
  mmap of the catalog; lookups hash the English text (a few hundred ns); nothing per frame beyond that.