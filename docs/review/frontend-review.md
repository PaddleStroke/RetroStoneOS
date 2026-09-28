# RetroStoneOS frontend: code review (defects)

Read-only review of `frontend/` (about 59k lines of C: menu UI, libretro host, display/KMS,
input, power, USB/web transfer), done on 2026-09-27. No source file was modified.

**Fix pass (2026-09-27, later the same day).** Every critical, high and medium finding is
fixed, and 20 of the 25 low ones (two of them partly); the other 5 are deferred with a reason.
The **status** column of the summary says which, and [Fixes](#fixes) has one short note per
finding (what changed, and the regression test). Also new: `make check-asan` (the whole
`make check` under ASan + UBSan + LeakSanitizer), `-Werror=format-security` everywhere, and
every link rule honours `$(LDFLAGS)`. `make check` and `make check-asan` pass with 0 warnings,
and so do the ARM cross checks.

**Line numbers.** Another agent was refactoring the tree (board.ini, renames) while this
review ran, so some line numbers had already moved by a few lines (display.c, main.c, sys.c,
screens.c). Every finding also names its function, and that is the reliable reference.

**Method.**
- Everything was read by hand, split by area: ui/gfx/theme, host, main/display/input/power/audio,
  transfer (USB), and transfer (web/netnames/QR) plus the build.
- Every finding below was re-checked in the code. Findings marked **[tool]** were also
  reproduced at run time.

**Tools used** (in WSL Ubuntu 24.04, gcc 13.3; build dirs `~/rsos/review-*`; sources were
snapshotted to ext4 first):

| Tool | Scope | Result |
|---|---|---|
| ASan + UBSan (`-fsanitize=address,undefined`) | full `make check`: display, host, power, transfer, ui, loader, splash, frontend | 3 real defects (F-M1 resampler, F-M2 backup menu use-after-free, one test bug). The resampler over-read also fires in the real game path (`check-frontend`: `rsos-run` → `audio_process`). |
| ThreadSanitizer | `test_loader` (game-list worker) | clean |
| cppcheck 2.13 (`--enable=warning,portability --inconclusive`) | src/ | 7 reports, all false positives (qr.c:212, batt_overlay.c:204, coreinfo.c:274, ui.c:392, and test files) |
| Custom mutation fuzzer, ASan/UBSan (scratchpad `review-fe/fuzz.c`) | `netnames_reply()` (mDNS/LLMNR/NBNS), 500k packets; webshare `read_head`/`get_param`/`host_ok`/`check_auth`/`transfer_relpath_check`, 100k requests | no memory errors; 1 UBSan report (F-L10) |
| `webshare_test.sh` against an ASan build of `rsos-webshare-host` | the stock 68 checks | 68/68 pass, no ASan reports |
| Extra dynamic tests (scratchpad `review-fe/web2.sh`) | concurrent uploads of one path; slowloris; lockout | the concurrent-upload corruption (F-H1) reproduced 6 times out of 6; slowloris answered 503 (F-L1) |

## Summary

| id | sev | area | file:line (function) | issue | status |
|---|---|---|---|---|---|
| F-C1 | critical | UI / memory | ui/menu.c:~847 `osk_ops` → `d_update` / `d_timeout` | The on-screen keyboard runs the dialog's update and timeout code on a 240-byte `struct osk`: heap out-of-bounds read and write, and a call through a garbage function pointer. | fixed |
| F-H1 | high | transfer / data | transfer/tr_util.c:410-433 `tr_sink_open`; webshare.c:720 `h_upload` | Fixed temp name opened with `O_TRUNC` and no `O_EXCL`: two uploads of the same path write into one temp file. The committed ROM is a mix of both and is reported `201 OK` **[tool]**. | fixed |
| F-H2 | high | UI / memory | ui/games.c:~947 `gamedb_set_core` | `free(e->core)` while list entries and Favorites/Last played copies still point at it: use-after-free, wrong core launched. | fixed |
| F-H3 | high | host / data | host/menu.c:54-59 `enum item_id` | `IT_DPAD_ANALOG` (1001) and `IT_INFO` (1002) equal the IDs of core options #1 and #2. Pressing A on info rows silently changes option #2, and the change is saved per game. | fixed |
| F-H4 | high | UI / data | ui/screens.c `game_item` (`m->ctx = g`) | The game options menu keeps a `struct game *`, but toggling Favorite re-sorts the list, so later actions (launch, core, favourite) hit another game and are saved to gamedb. | fixed |
| F-H5 | high | UI / memory | gfx/image.c:558-583 `img_put` / `img_trim` | The image cache is only trimmed on theme or resolution change. Browsing a scraped library grows memory without bound (about 250 KB per picture). | fixed |
| F-M1 | medium | audio / memory | audio/resampler.c:205 `resampler_run`, :129-160 `sinc_frame` | A fraction just below 1 rounds to `1.0f`, giving `p = 128`, and `c1` reads 64 bytes past the heap block **[tool]**. | fixed |
| F-M2 | medium | UI / memory | ui/transfer_ui.c:622-623 `backup_item` | `c->mode` is read after `pop_menus()` has freed `c` **[tool]**. | fixed |
| F-M3 | medium | UI / memory | ui/menu.c:~590 `dialog_open` + ui.c:~95 `ui_push` | When the stack is full the new dialog is destroyed but still returned and then used. Reachable by hot-plugging unconfigured pads. | fixed |
| F-M4 | medium | UI / memory | ui/widgets.c:~595 icon table eviction | Icons still in use are freed when the 96-slot table is full (`help_draw`, `layout_rating`). | fixed |
| F-M5 | medium | host / memory | host/hwrender.c:847 `hwr_readback`, host.c:~310, ~359 | The readback clamps w/h to the FBO size, but callers use the unclamped w/h/pitch: over-read of `G.xrgb`. | fixed (FBO not resized: see note) |
| F-M6 | medium | host / memory | host/saves.c:~481 `rzip_decode` | `pos + csize > n` wraps on 32-bit ARM: a crafted `.state` (uploadable via USB or web) causes an out-of-bounds read and a crash. | fixed |
| F-M7 | medium | UI / data | ui/settings.c:~182 `settings_save`, games.c:~874 `gamedb_open`, hw.c:~171 | A failed or short read is treated as an empty file, so the next save erases settings, favourites/history or `rsos.env`. | fixed |
| F-M8 | medium | transfer / data | transfer/tr_util.c:544-556 `tr_sink_commit`; host/hutil.c:~338 `hwrite_atomic_v` | `.bak` rotation: the first rename's result is ignored, nothing is rolled back if the second rename fails, and between the two renames the live file does not exist. | fixed |
| F-M9 | medium | host / data | host/bench.c:496-541 `bench_ini_set_file` | `secs[64]` cap and an ignored `ini_load` error: "Use this for this game" can wipe per-game core choices in `cores.ini`. | fixed |
| F-M10 | medium | host / data | host/saves.c:~523 `state_load`, :398 `state_exists` | Does not wait for the async save queue: load right after save loads the older state, and the menu says "slot empty". | fixed |
| F-M11 | medium | host / error | host/saves.c:116-128, 330-368 `finish_job` / `sram_tick` | On ENOSPC/EROFS the SRAM write is retried every second forever: log flood on tmpfs and a permanent error toast. | fixed |
| F-M12 | medium | host / process | host/bench_run.c:~126 `bench_driver_main` | The benchmark driver keeps the game pid but has no handler for `RSOS_SIG_SLEEP`/`WAKE` (SIGRTMIN+1/2). A sleep request kills it. | fixed |
| F-M13 | medium | libretro | host/host.c:~2158-2174 `host_run` teardown | HW `context_destroy` is called after `retro_unload_game` (RetroArch does the opposite). | fixed |
| F-M14 | medium | input | input/input.c:1316 `dev_resync`, :1711 | After SYN_DROPPED the newest queued event is counted twice (it is already in `EVIOCGKEY`), so buttons stick. The merged built-in pad is never resynced, and ABS is not re-read. | fixed (not tested on hardware) |
| F-M15 | medium | main / error | main.c `scr_draw` / main-loop timeout | If the UI surface cannot be (re)created, the loop spins at 0 ms and logs one line per iteration to tmpfs. | fixed |
| F-M16 | medium | transfer / path | transfer/usb.c:347 `do_mount`; import.c:173, 414 `resolve_dir_ci`, `plan_scan_root` | NTFS stick symlinks are followed (`stat`, no `MS_NOSYMFOLLOW`): system files get copied into `/data`, and `/proc/kmsg` makes the thread hang. | fixed |
| F-M17 | medium | transfer / error | transfer/import.c:~1033 `fatal_errno`, :~1107 `copy_item` | Pulling the stick between files does not stop the import. A new stick mounted at the same `/media/usbN` is then read against the old plan. | fixed |
| F-L1 | low | web / DoS | transfer/webshare.c:281-293 `read_head`, 905-911 `worker` | No whole-request deadline (30 s per `recv`): 4 slow LAN clients hold every slot indefinitely and block the idle auto-stop **[tool]**. | fixed |
| F-L2 | low | web / auth | transfer/webshare.c:482-526 `h_login` | Lock check happens before the body read and is not atomic with the verdict: up to `max_clients` guesses per lock window. The lockout is global, so any LAN peer can lock the owner out. | fixed |
| F-L3 | low | web / lifecycle | ui/transfer_ui.c:1291 `transfer_before_game`, :1300 `transfer_network_off` | After the share stops by itself (idle), netnames keeps running, during games too. | fixed |
| F-L4 | low | netnames / perf | transfer/netnames.c:457-462 `serve` | `getifaddrs()` (a netlink dump) runs for every received packet, before checking that the packet is for us. | fixed |
| F-L5 | low | netnames | transfer/netnames.c:430-479 `serve` | No source-subnet or IP TTL=255 check (RFC 6762 §11): answers off-link unicast queries. | fixed (subnet check) |
| F-L6 | low | netnames | transfer/netnames.c:410 `join_groups` | `joined[16]` is never pruned: after 16 interface (re)appearances, no more multicast joins or announcements. | fixed |
| F-L7 | low | fds | transfer/tr_util.c:303 `tr_open_dir_chain`; webshare.c:596 `list_dir` | `dup()` without CLOEXEC: short windows where a fork (hw jobs) inherits `/data` directory fds. | fixed |
| F-L8 | low | transfer | transfer/tr_util.c:447 / import.c:1168 / backup.c:424 | `bytes_done` underflows after a write error, so the summary shows an absurd size. | fixed |
| F-L9 | low | transfer / error | tr_util.c:649, backup.c:674-678, import.c:1271 | Final folder fsync / `syncfs` errors are ignored while success is shown; a previous folder's fsync error is blamed on the current file. | deferred |
| F-L10 | low | netnames / UB | transfer/netnames.c:236 `nbns_reply` | `(q[..] - 'A') << 4` left-shifts a negative value (UBSan) **[tool]**. | fixed |
| F-L11 | low | input / UB | input/input.c:~453-492 `apply_sdl_mapping`, ~1173 `handle_abs` | "guide" mapped to a hat or axis gives `BTN_MASK(35)` = `1u << 35`. | fixed |
| F-L12 | low | main / log | main.c:280-301 `log_thread` | A blocking console write stalls every `fprintf(stderr)` once the 64 KB pipe is full. `frontend.log` on tmpfs has no size cap. | deferred |
| F-L13 | low | UI / fd | ui/util.c:~398 `file_write_atomic` | fd leaks when `fsync` fails (`||` short-circuits `close`). | fixed |
| F-L14 | low | UI / process | ui/hw.c:314-324 `hw_job_start` | The child keeps `SIGPIPE=SIG_IGN` and the parent's signal mask. The `/dev/null` fd has no CLOEXEC. | fixed |
| F-L15 | low | host / data | host/options.c:71-78, 425-500; bench.c:469 | `ini_load` errors are ignored, so an unreadable options file is overwritten with only the new keys. | fixed |
| F-L16 | low | libretro | host/options.c:147-165, 491-495 | `GET_VARIABLE` value pointers are freed when a core re-declares its options. | deferred |
| F-L17 | low | libretro | host/core.c:~484 | `SET_SERIALIZATION_QUIRKS` does not clear the unsupported bits. | fixed |
| F-L18 | low | host | host/hutil.c:~194 `hmkdir_p("")` | Reads past the NUL (`--logs ""`). | fixed |
| F-L19 | low | audio | audio/audio.c `write_raw`, `audio_close` | `-EINTR` is treated as fatal. `snd_pcm_drain` has no timeout, so it can hang on a stalled HDMI device. | fixed |
| F-L20 | low | main / data | main.c:~1507 `resume_answered` | Unlinks `resume.ini` without an fsync of the directory. | fixed |
| F-L21 | low | UI | gfx/image.c:417, gfx.c:721, widgets.c:360-409 | No decode-size cap (`STBI_MAX_DIMENSIONS`), and `xmalloc`/`xcalloc` abort: a huge image or a bad theme number crashes (and may crash-loop) the menu. | partly fixed (decode cap; theme numbers deferred) |
| F-L22 | low | transfer | transfer/tr_util.c:420-429 | Orphan `.name.rsos-part` files (power cut, EROFS) are never swept. On exFAT they are pre-sized to the full length. | deferred |
| F-L23 | low | transfer | transfer/tr_util.c:212-240 `transfer_name_sanitize` | Long names are truncated silently, which drops the extension and can collide with another name. | deferred |
| F-L24 | low | build | Makefile:40, ui.mk, power.mk:67-73, transfer.mk:71-74 | No `-Wformat-security`; fragment link rules ignore `$(LDFLAGS)`; FORTIFY is only level 1, and only implicitly via Buildroot defaults. | fixed in `frontend/` (defconfig: suggested to the lead) |
| F-L25 | low | tests | host/tests/test_host.c:798 | Passes 66 for a 65-byte literal (ASan global overflow in the test) **[tool]**. | fixed |

Smaller items and notes are at the end.

---

## Critical

### F-C1: the on-screen keyboard uses the dialog's `update`/`timeout` on the wrong struct
- **Where:** `ui/menu.c` `osk_ops` (≈ line 847): `.update = d_update, .timeout = d_timeout`. `d_update` / `d_timeout` (≈ 523-564) cast the screen to `struct dialog`.
- **Why it breaks:**
  - `struct osk` is `{ base; title[64]; text[128]; bools; row, col; cb; user }`, about 240 bytes.
  - `struct dialog` has `countdown_s` and `deadline` at about offset 1200 and 1216, after `text[512]` and `tmpl[512]`.
- **Failure:** `ui_update` calls `d_update` on every tick while the WiFi SSID, WiFi password or console-name keyboard is open. It reads `countdown_s` out of bounds.
  - If that value is non-zero, it writes `deadline` (8 bytes) out of bounds.
  - `d_countdown_text` then `snprintf`s up to 512 bytes into `d->text` at offset 16 of the 240-byte block.
  - Once the stray deadline passes, it `ui_pop`s (frees) the keyboard and calls `d->cb`, read from about offset 672: an arbitrary function pointer.
  - Results depend on the neighbouring heap: a crash, heap corruption, or a call to a garbage address.
- **Evidence:** ASan (UI reviewer): heap-buffer-overflow at `menu.c:528`, "960 bytes after 240-byte region allocated in osk_open".
- **Fix:** give `osk_ops` `.update = NULL` and a `.timeout` that returns -1, or its own functions. Add a `_Static_assert`/type tag so ops can't be shared across screen types by mistake.

## High

### F-H1: concurrent uploads of the same file corrupt it, and the server reports success
- **Where:**
  - `transfer/tr_util.c:410-433` `tr_sink_open`: the temp name is deterministic (`.<name>.rsos-part`, or `.rsos-part-<fnv32>` for long names) and opened with `O_WRONLY|O_CREAT|O_TRUNC` (line 429), without `O_EXCL`.
  - `webshare.c:720-731` `h_upload`: the "exists / overwrite" check is a plain `fstatat` before the sink opens.
- **Failure:** two requests for the same `target/sys/path` share one temp inode. Up to 4 clients by default: a double drag-and-drop, two browser tabs, or a script retry while the first upload is still in flight.
  - The second `O_TRUNC` truncates the first writer's data, and both then write at their own offsets into the same file.
  - The first `tr_sink_commit` fsyncs and renames the mixed file into place and answers `201`.
  - The second `renameat` fails with ENOENT and answers `500 write`.
  - With `overwrite=0` on both requests, both pass the exists check (TOCTOU).
- **Reproduced:** two 60 MB uploads (`AAAA…` and `BBBB…`) to `roms/snes/x.sfc` at the same time. In 6 of 6 rounds the committed file contained about 25-34 MB of each, and one client got `{"ok":true}`.
- **Same pattern elsewhere:**
  - A USB import running while the web share writes the same destination hits the same temp name.
  - Two different long names with the same 32-bit FNV hash in one folder also share a temp file.
- **Fix:**
  - Create the temp file with `O_EXCL` and a unique random suffix (`.name.<8 hex>.rsos-part`); loop on EEXIST.
  - In webshare, keep an in-flight set of destination paths under `S.lock` and answer `409 busy` for a second upload of the same path.
  - For `overwrite=0`, commit with `renameat2(..., RENAME_NOREPLACE)` so the exists check is atomic.

### F-H2: `gamedb_set_core` frees a string that other game entries still point to
- **Where:**
  - `ui/games.c` `gamedb_apply` (≈934) sets `g->core = e->core` on every list entry.
  - `loader.c` `build_coll` (≈435) copies whole `struct game`s, pointer included, into the Favorites and Last played lists.
  - `gamedb_set_core` (≈947-955) `free(e->core)` and only updates the entry it was given.
- **Failure:**
  1. Change "Core for this game" from Last played or Favorites (or from the system list).
  2. The other copy's `core` now dangles.
  3. Launching it runs `ui_game_core`/`fill_launch` (`ui.c` ≈390/432), which `strcmp` freed memory. The chosen core is silently ignored or garbage is compared.
- **Fix:** never free core strings while lists live: intern them for the database's lifetime, or point `g->core` at the static core id from `systems`. Alternatively, update every copy (the real list and both collections) when the core changes.

### F-H3: in-game menu item IDs collide with core option IDs
- **Where:** `host/menu.c:54-59`: `IT_OPT_BASE = 1000, IT_DPAD_ANALOG, IT_INFO` (so 1001 and 1002). Option *i* is added as `IT_OPT_BASE + i` (line 140).
- **Failure:**
  - Core option #1 cannot be stepped: `change()` hits `case IT_DPAD_ANALOG` and toggles "D-pad as left stick" instead.
  - Core option #2 is drawn dimmed (`id == IT_INFO`, line 266).
  - Pressing A, Left or Right on any info row (Controls, Benchmark, results pages) goes to `change()`'s default branch, which calls `opts_step(2, ±1)`. That silently changes core option #2 and marks the options dirty.
  - `autosave()` then writes it to `/data/rsos/coreopts/<core>/<game>.ini`. The corruption is persistent, and invisible because it happens on another page.
- **Fix:** move `IT_DPAD_ANALOG, IT_INFO` before `IT_OPT_BASE` (the option range must be last), and handle `IT_INFO` first in `change()`/`activate()`.

### F-H4: game options menu acts on the wrong game after the list is re-sorted
- **Where:** `ui/screens.c` `game_item`: the menu keeps `struct game *g = m->ctx`, a pointer into `se->games->games[]`. The Favorite branch calls `glview_refresh`, which calls `games_sort(...)` (`view_gamelist.c` ≈1041). That sorts the same array in place.
- **Failure:** with "favorites first" (the default), toggle Favorite. `g` now points to whichever game moved into that slot. "Launch", "Core for this game" or a second Favorite toggle then applies to that other game, and `gamedb_save` persists it.
- **Fix:** store system + path in the menu context and look the game up for each action (`real_game()`), or defer the re-sort until the menu closes.

### F-H5: the image cache grows for the whole session
- **Where:**
  - `gfx/image.c:558-562` `img_put` only decrements `refs`.
  - `img_trim()` (564-583) is called only from `ui.c` ≈378/746/775 (theme change, resolution change, exit).
  - Lookups in `img_get` walk the list linearly (≈496).
- **Failure:**
  - Every game picture, marquee and thumbnail shown in the detailed list stays decoded in RAM, about 250 KB at 640x480. The first pass over a few thousand scraped games is heap-allocated.
  - Later passes load them from the `.rpx` cache via `mmap`, which still costs VMAs and 32-bit address space.
  - Lookups get slower as the list grows.
  - The menu runs for hours, and the A20 has little RAM, so this ends in out-of-memory.
- **Fix:** LRU eviction of `refs == 0` entries over a byte budget (`img_mem_used()` exists), or `img_trim()` on game-list screen exit and when idle.

## Medium

### F-M1: the resampler reads past its heap block [tool]
- **Where:** `audio/resampler.c` `resampler_run` line 205: `float f = (float)(r->pos - i)`. Then `sinc_frame` (129-133) does `p = (int)(f*PHASES)` and `c1 = c0 + TAPS`.
- **Cause:** a double fraction above `1 - 2^-25` rounds to `1.0f`, so `p = 128` and `c1` is row 129. `table[]` has rows 0..128 and is the struct's last member, so this reads 64 bytes past the `posix_memalign` block.
- **Frequency:** the fraction is quasi-uniform under rate control, so it happens roughly once every few minutes of play. Garbage coefficients mean a click; a NaN reaches `lrintf`, which is undefined behaviour.
- **Evidence:**
  - ASan in `rsos-host-test` (`test_resampler`).
  - The same error in every `rsos-run` game under `check-frontend` (via `host.c:438 audio_process`).
- **Fix:** after computing `p`: `if (p >= PHASES) { p = PHASES - 1; mu = 1.0f; }`. Or allocate `(PHASES + 2) * TAPS`.

### F-M2: `backup_item` reads freed memory [tool]
- **Where:** `ui/transfer_ui.c:622-623`. `pop_menus(ui, K_BACKUP)` destroys the menu, and `on_destroy` frees `c`. The next line then reads `c->mode`.
- **Evidence:** ASan in `check-ui` (the "export games" script).
- **Fix:** `int kind = c->mode == TRANSFER_EXPORT_GAMES ? 1 : 2;` before `pop_menus`.

### F-M3: `dialog_open` returns a freed dialog when the screen stack is full
- **Where:** `ui.c` ≈95 `ui_push`: at `MAX_SCREENS` (12) it calls `s->ops->destroy`. `menu.c` ≈590 `dialog_open` still returns `&d->base`.
- **Callers that use it:** `lcd_hz_changed` (`dialog_select`, `dialog_set_countdown`) and `usb_offer` (stores `ui->usb_dialog`).
- **How the stack fills:** each unconfigured-controller hotplug pushes a dialog (`ui.c` ≈834-840), so repeated plugging fills it.
- **Fix:** make `ui_push` return bool and have `dialog_open` return NULL on failure. Callers must accept NULL.

### F-M4: the icon table frees icons that are still in use
- **Where:** `ui/widgets.c` ≈595-599: when all 96 slots are used, the oldest icon is freed.
  - `help_draw` (≈670-697) looks up all its icons and draws afterwards.
  - `layout_rating` (≈441-447) keeps star icons with no reference.
- **When:** more than 96 distinct name/size/colour combinations, for example themes with per-system help colours.
- **Fix:** add reference counts, or evict only unreferenced least-recently-used entries, or defer frees to the end of the frame.

### F-M5: hardware-render readback can over-read `G.xrgb`
- **Where:** `host/hwrender.c:847-872` clamps `w/h` to the FBO (`G.w/G.h`) and packs rows at the clamped width. `host.c` ≈310 then uses `pitch = w*4` and `present(data, w, h, pitch)` with the unclamped values, and `host_capture_last` (≈359) does the same.
- **Failure:** a GL core that reports a frame larger than the FBO: after `SET_SYSTEM_AV_INFO` raises the maximum (the FBO is not resized), or a core that exceeds its declared maximum.
- **Fix:** return the effective w/h/pitch from `hwr_readback`, and recreate the FBO when the maximum geometry grows.

### F-M6: `rzip_decode` bounds check wraps on 32-bit
- **Where:** `host/saves.c` ≈481: `if (pos + csize > n ...)` with `size_t` pos/n and a `uint32_t` csize. On ARMv7, `size_t` is 32-bit, so a huge `csize` wraps and passes. `mz_uncompress` then reads out of bounds.
- **Input:** state files can be dropped in by the web share (`target=states`) or restored from USB, so they are untrusted.
- **Fix:** `if (csize > n - pos)`.

### F-M7: a failed read of a user file is treated as empty, so the next save erases data
- **Where:**
  - `ui/settings.c` ≈182: `settings_save` calls `load()` again. `file_read` (`util.c` ≈330-352) returns NULL on open/fstat failure, or a *short* buffer on a mid-file read error. `settings.ini` is then rewritten with only the pending keys.
  - The same pattern exists in `gamedb_open` (≈874) then `gamedb_save` (≈991), which loses all favourites and history.
  - Also in `hw_env_set_overlay` (`hw.c` ≈171/197-223), which rewrites `/boot/rsos.env` with only `overlays=`.
- **Trigger:** EIO on a failing SD card, EMFILE (see F-L13), ENOMEM.
- **Fix:** `file_read` must report errors and short reads. Treat only ENOENT as empty, and refuse to save after a read error.

### F-M8: `.bak` rotation is not failure-safe and briefly leaves no live file
- **Where:** `transfer/tr_util.c:544-556` `tr_sink_commit(keep_bak)` and `host/hutil.c` ≈338-341 `hwrite_atomic_v(backup=true)`.
- **Failure:**
  - `renameat(name → name.bak)` is not checked. If it fails (ENOSPC growing an exFAT directory for the longer name, EIO, `.bak` being a directory), or is skipped because `strlen(bak) >= 256`, the save is overwritten with no backup, although the UI promised one.
  - If `renameat(tmp → name)` fails, the tmp file is unlinked and `.bak` is not restored, so the save exists only as `name.bak`.
  - A power cut between the two renames has the same result. `/data` is exFAT (no journal, per the comment in `util.c`).
  - Only `state_load` falls back to `.bak`. `state_exists`, the resume lookup (`host.c` ≈2077 `load_slot`, `content.c` 247-272) and the emulator's SRAM load do not.
- **Fix:** check the first rename, and on failure of the second, rename `.bak` back. Better: `linkat(name, name.bak)` then a single `renameat(tmp, name)`, so `name` always exists (exFAT has no hard links, so keep the checked two-rename path there, plus recovery at load: if `name` is missing and `.bak`/`.tmp` exist, restore).

### F-M9: "Use this for this game" can wipe `cores.ini`
- **Where:** `host/bench.c:496-541` `bench_ini_set_file`: `const char *secs[64]`, so sections after the 64th are dropped on rewrite. `ini_load`'s return value is ignored (≈505): a read error rewrites the file with only the new key. Values are written unquoted and comments are lost.
- **Fix:** no fixed cap; abort on any `ini_load` error except ENOENT; quote values.

### F-M10: state load does not wait for a queued async save
- **Where:** `host/saves.c` ≈523 `state_load` and ≈398 `state_exists` do not call `wait_idle()`. Saves go through the async `submit`.
- **Failure:**
  - Save, then load within the write time (multi-MB N64/PS1 states on SD take a second or more): the previous state is loaded, or the `.bak` during the rename window.
  - "Load state" reports "slot empty" for a first save that is still queued.
- **Fix:** `wait_idle()` (with `busy_ok`) at the start of both.

### F-M11: SRAM write failures retry every second forever
- **Where:** `host/saves.c` 116-128 `finish_job` resets `written = 0`, and `sram_tick` (330-368) retries on every 1 s tick.
- **Failure:** full disk or read-only `/data`. Each second: a new `.tmp` attempt, two log lines (tmpfs grows) and a new 4 s error toast, which keeps the OSD copy path active on every frame.
- **Fix:** exponential backoff, and one toast per error kind.

### F-M12: a sleep request kills a running benchmark
- **Where:** `host/bench_run.c` ≈126-132 installs handlers for TERM/INT/HUP/USR1/poweroff only. `host.c` `bench_start` execs `/proc/self/exe` as the driver in the same pid. `main.c` ≈964 sends `RSOS_SIG_SLEEP`/`WAKE` (SIGRTMIN+1/2) to that pid, and the default action for real-time signals is to terminate.
- **Failure:** a power-key tap or auto-sleep during a benchmark kills the driver. `PR_SET_PDEATHSIG` then kills the step, and the UI shows "The emulator crashed".
- **Also:** in the exec windows, a signal that arrives before `sigaction` also terminates.
- **Fix:** handle or ignore SIGRTMIN+1/2 in the driver (or forward them). Block these signals across exec and unblock them after installing the handlers.

### F-M13: HW-render teardown order
- **Where:** `host/host.c` ≈2158-2174 calls `retro_unload_game`, then `context_destroy`.
- **Why it matters:** RetroArch's `core_unload_game()` calls `video_driver_free_hw_context()` (`context_destroy`) *before* `retro_unload_game()`, and cores are written for that order. A core that frees its GL wrappers or state in `unload_game` and uses them in `context_destroy` hits a use-after-free, or double-frees GL objects.
- **Fix:** `context_destroy` (context current), then `unload_game`, `deinit`, `hwr_deinit`.
- **Confidence:** medium; the effect depends on the core.

### F-M14: buttons stick after an evdev buffer overflow (SYN_DROPPED)
- **Where:** `input/input.c:1316-1334` `dev_resync`, called from the read loop at :1711.
- **Cause:** the kernel keeps SYN_DROPPED plus the *newest* event after an overflow. `dev_resync` rebuilds counts from `EVIOCGKEY`, which already includes that newest event, and the loop then applies it again.
  - A press ends at count 2, so the release leaves 1: the button stays held and the menu auto-repeats.
  - For the merged built-in pad (`d->pad != &d->own`), resync does nothing, so a lost release stays latched.
  - ABS is not re-read (`EVIOCGABS`), so a dropped stick recentre keeps `stick_nav` latched.
- **When:** the main thread stalls (modeset, SD write, the parent polls only every 100 ms during a game) while the user presses buttons. A stuck Select+Start in the parent even trips the 5 s hang-kill in `launch_idle`.
- **Fix:** like `power.c` `drain_input()`: discard events until the next SYN_REPORT, then resync. Derive counts from per-device key bitmaps, recompute the merged pad from all built-in devices, and re-read ABS.
- **Confidence:** medium-high; this is from the kernel's documented semantics and was not tested on hardware.

### F-M15: main loop hot-spins when the UI surface cannot be created
- **Where:** `main.c` `scr_draw` logs "frame: no buffer" on every call and returns false. `need_redraw` stays set, so the poll timeout is 0. `display.c` `display_set_game_surface` failure leaves `surf_set = false`, and nothing retries until the next hotplug.
- **Trigger:** e.g. CMA exhaustion when HDMI 1080p is plugged (new buffers are allocated before the old ones are freed).
- **Effect:** 100 % CPU and thousands of log lines per second into `/run/rsos/frontend.log` (tmpfs, never rotated).
- **Fix:** rate-limit the log, back off 100-500 ms after a failed draw, and periodically retry `scr_set_surface`.
- **Confidence:** medium; the loop behaviour is certain, the trigger is not.

### F-M16: symlinks on an NTFS stick are followed out of the mount
- **Where:**
  - `transfer/usb.c:347` `do_mount` flags `MS_RDONLY|NOSUID|NODEV|NOEXEC|NOATIME`, without `MS_NOSYMFOLLOW`. ntfs3 exposes reparse and LX symlinks.
  - `import.c:173` `resolve_dir_ci` and `:414` `plan_scan_root` use `stat()`, which follows links, on the resolved roots (`bios/`, `roms/`, `saves/`, `states/`, `themes/`, `rsos/coreopts`).
  - `is_backup_dir` opens the `.rsos-backup` marker without `O_NOFOLLOW`.
- **Failure:** a stick with `bios → ../../..` makes the import walk the console's root filesystem as root and copy regular files into `/data/bios/...`, where the web share can list them. `/proc/kmsg` (a regular file of size 0) blocks `read()` forever, and cancel/shutdown (`transfer_import_finish`) hang.
- **Fix:** mount with `MS_NOSYMFOLLOW` (kernel ≥ 5.10; this kernel is 6.18). Use `lstat`/`O_NOFOLLOW` for roots and the marker, and optionally require the stick's `st_dev` on every directory walked.

### F-M17: pulling the stick mid-import does not stop it, and a new stick can be read
- **Where:**
  - `import.c` ≈1107 `copy_item` opens `it->src` by absolute path, and ≈1033 `fatal_errno` excludes ENOENT/ENOTDIR.
  - `usb.c` ≈499-510 `drop_drive` lazy-unmounts, `rmdir`s the mount point and frees the `usbN` index.
  - `transfer_ui.c` (REMOVED handler, ≈1144) does not cancel the job.
- **Failure:** every remaining file fails with ENOENT and the import ends as "Some files failed" instead of "USB drive removed". If another stick is plugged within the run, it mounts at the same `/media/usb0`, and the rest of the old plan is read from it.
- **Fix:** keep an `O_DIRECTORY` fd of the stick root (or its `st_dev`) in the plan and `openat` relative to it. Treat ENOENT/ENOTDIR/a changed `st_dev` as fatal `-ENODEV`, and cancel the job on REMOVED for its mount point.

## Low

- **F-L1 web share slowloris / no request deadline** [tool]
  - **Where:** `webshare.c:281-293` `read_head` loops over `recv` with only `SO_RCVTIMEO=30 s` per call (set in `worker` 905-911), so a client sending 1 byte every 29 s keeps its slot for up to about 8192 × 29 s.
  - **Effect:** with the default `max_clients = 4`, four such LAN clients get everyone else a `503`. This was reproduced with 4 idle sockets. They also keep `nclients > 0`, so the idle auto-stop never fires (battery). It happens before authentication.
  - **Fix:** a whole-head deadline (e.g. 10 s, checked around each `recv` with `poll`), and a minimum body rate for uploads. Optionally reserve one slot for authenticated clients.
- **F-L2 PIN lockout race and global lockout**
  - **Where:** `webshare.c:482-491` checks `lock_until` before reading the body (497-504), and the verdict comes later (510-526).
  - **Effect:** clients that pass the check in the same unlocked window each get a guess. That is up to `max_clients` guesses per 30-600 s window instead of 1: still years for 10^6 PINs, but weaker than documented.
  - The lockout is global, and `/api/login` needs no `X-Requested-With`. Any LAN device, or a web page in a LAN browser (form POST, IP-literal `Host`), can keep the owner locked out.
  - **Fix:** re-check and record the attempt atomically after the body is read (count "in flight" attempts). Consider a per-peer counter plus a smaller global one.
- **F-L3 netnames outlives the share**
  - **Where:** `transfer_ui.c:1291` `transfer_before_game` and `:1300` `transfer_network_off` call `webshare_stop_all` only `if webshare_running()`.
  - **Effect:** after an idle auto-stop, the netnames thread and its 3 UDP sockets keep running, during games too. The next `webshare_open` then gets `-EALREADY` from `netnames_start` and keeps the old hostname.
  - **Fix:** always call `netnames_stop()` in those paths, and when `webshare_running()` turns false.
- **F-L4 `getifaddrs` on every packet**
  - **Where:** `netnames.c:457-462` calls `if_ip()` (`getifaddrs`, a netlink dump) for every mDNS/LLMNR/NBNS packet received, before `netnames_reply` checks that the packet asks for our name.
  - **Effect:** busy LANs carry a lot of mDNS traffic.
  - **Fix:** build the reply first (with `ipi_spec_dst`) and look up the address only on a match, or cache it and refresh on `join_groups`.
- **F-L5 off-link replies**
  - **Where:** `netnames.c` `serve` accepts queries from any source.
  - **Effect:** RFC 6762 §11 says to ignore queries whose source is not on the local link (or IP TTL ≠ 255). LLMNR, NBNS and legacy-unicast mDNS replies go unicast to the source, so the service is a small reflector if the device ever sits on a routed or public network.
  - **Fix:** check the source against the interface's subnet (and `IP_RECVTTL`).
- **F-L6 `joined[16]` never pruned**
  - **Where:** `netnames.c:410`.
  - **Effect:** after 16 distinct ifindexes (USB WiFi or Ethernet replugged), there are no more joins or announcements until restart. Also, an interface that reconnects to a new network is never re-announced.
- **F-L7 fds without CLOEXEC**
  - **Where:** `tr_util.c:303` `tr_open_dir_chain` and `webshare.c:596` `list_dir` use `dup()`.
  - **Effect:** while a worker thread holds these fds, a concurrent `fork` (for example `hw_job_start` for a network helper, if one runs while the share is active) inherits a `/data` directory fd.
  - **Fix:** `fcntl(fd, F_DUPFD_CLOEXEC, 0)`.
- **F-L8 progress underflow**
  - **Where:** `tr_util.c:447-449` counts `s->written` before a later failing write or `sync_file_range(WAIT)`. `import.c:1168` / `backup.c:424` then subtract `sink.written`, which is more than was ever added to `*progress`.
  - **Effect:** `bytes_done` wraps to about 1.8e19 in the "Stopped" summary.
- **F-L9 flush errors ignored**
  - **Where:** `tr_copier_free` (≈649) only logs the deferred folder fsync. `backup.c:674/678` ignores `syncfs`/`fsync(stickfd)`. `write_text` markers ignore errors. `import.c:1271` ignores `fsync(rootfd)`.
  - **Effect:** success is shown although the data may not be on disk. Conversely, `tr_copier_commit` (≈904) returns the *previous* folder's fsync error as the current file's error.
- **F-L10 UB in `nbns_reply`** [tool, UBSan]
  - **Where:** `netnames.c:236`: `(q[43] - 'A') << 4` with a byte below `'A'` left-shifts a negative value.
  - **Effect:** harmless with gcc, but undefined behaviour.
  - **Fix:** `(uint8_t)((unsigned)(q[43] - 'A') << 4 | (unsigned)(q[44] - 'A') & 0xf)`, or reject bytes outside `'A'..'P'`.
- **F-L11 `1u << 35`**
  - **Where:** `input.c` `apply_sdl_mapping` accepts `T_GUIDE` (35) for hat and axis targets, and `handle_abs` then does `BTN_MASK(35)`.
  - **Effect:** undefined behaviour; on x86 it aliases to bit 3 (Start).
  - **Fix:** skip targets `>= IN_NUM_BUTTONS`.
- **F-L12 log thread**
  - **Where:** `main.c:280-301` `log_thread` writes to the console synchronously.
  - **Effect:** a blocked console (serial flow control) stalls it, and then every `fprintf(stderr)` in the UI once the pipe is full. If the thread exits, `L.rd` stays open and writers block instead of getting EPIPE. `frontend.log` on tmpfs has no size cap (compare the 1 MB cap on `game.log`).
  - **Fix:** a non-blocking console fd that drops data on EAGAIN, close `L.rd` on exit, and cap or rotate the file.
- **F-L13 fd leak in `file_write_atomic`**
  - **Where:** `ui/util.c` ≈398: `if (fsync(fd) < 0 || close(fd) < 0 || rename(...))`.
  - **Effect:** skips `close` when `fsync` fails. Repeated EIO leads to EMFILE, which then triggers F-M7.
- **F-L14 `hw_job_start` child setup**
  - **Where:** `ui/hw.c:314-324`.
  - **Effect:** the child does not reset `SIGPIPE` (inherits SIG_IGN into `rsos-net` and the daemons it starts) or the signal mask. The `/dev/null` fd lacks `O_CLOEXEC`. `hw_job_poll` switches the pipe to blocking and drains it, so a descendant that keeps stdout would freeze the UI (fragile, not currently broken).
- **F-L15 options overwrite after a read error**
  - **Where:** `options.c:71-78` `opts_init`, `425-500` `write_layer`, `bench.c:469`.
  - **Effect:** `ini_load` errors are ignored, so an existing but unreadable file is replaced by only the new keys. This contradicts the design's "never overwrite what we could not read" rule.
- **F-L16 `GET_VARIABLE` lifetime**
  - **Where:** `options.c:147-165` / `491-495`.
  - **Effect:** re-declaring options, or `write_layer` re-parsing the ini, frees strings that a core may still hold from `GET_VARIABLE`.
  - **Fix:** keep value strings for the core's lifetime (arena).
- **F-L17 `SET_SERIALIZATION_QUIRKS`**
  - **Where:** `core.c` ≈484.
  - **Effect:** the spec says the frontend clears the quirk bits it does not support; they are left set.
- **F-L18 `hmkdir_p("")`**
  - **Where:** `hutil.c` ≈194.
  - **Effect:** starts at `buf + 1`, past the NUL, and reads uninitialized stack. Reachable with `--logs ""` / `--tmp ""`.
- **F-L19 audio error paths**
  - **Where:** `audio.c` `write_raw`, `audio_close`.
  - **Effect:** `-EINTR` is treated as a fatal write error. `snd_pcm_drain` in `audio_close` has no timeout, so a stalled HDMI sink can hang exit (saves are already written by then).
- **F-L20 `resume_answered` unlink**
  - **Where:** `main.c` ≈1507.
  - **Effect:** `resume.ini` is unlinked without an fsync of `/data/rsos`, so after a power cut the resume offer can come back once.
- **F-L21 oversized images and theme values**
  - **Where:** `image.c:417` (no `STBI_MAX_DIMENSIONS` in `third_party_impl.c`), `gfx.c:721` (`xmalloc` aborts on failure), `widgets.c:360-409` (theme sizes not checked for huge, NaN or infinity).
  - **Effect:** a huge image, or a bad user theme, aborts the menu, possibly in a crash loop.
  - **Fix:** `stbi_info` plus a cap; validate theme numbers.
- **F-L22 orphan temp files**
  - **Where:** `tr_util.c:420-429`.
  - **Effect:** `.name.rsos-part` / `.rsos-part-XXXXXXXX` left by a power cut or EROFS are never swept. On exFAT they are pre-sized to the full length, and they are hidden from the scanner and the listing.
  - **Fix:** sweep at import start or at boot.
- **F-L23 name sanitising**
  - **Where:** `tr_util.c:212-240` `transfer_name_sanitize` (`o + 5 < n` with `name[256]`).
  - **Effect:** 251-255-byte names are cut and lose their extension. `a?b` and `a_b` both become `a_b`, and `plan_finish` then drops one without counting it.
- **F-L24 build hardening** (see below).
- **F-L25 test bug**
  - **Where:** `host/tests/test_host.c:798` passes 66 for a 65-byte literal to `hwrite_atomic`.
  - **Effect:** ASan aborts `rsos-host-test` before the later tests run.

### Other small items (verified, low impact)
- `host/launch.c` ≈156-161: once `status[]` is full, `read(..., 0)` returns 0 while `poll` keeps reporting POLLIN, so `launch_idle` spins. Not reachable with today's short status lines.
- `host/launch.c` ≈173-179: the post-exit drain uses a blocking `read`. A descendant holding fd 3 would block the UI; make the pipe `O_NONBLOCK`.
- `host/content.c:219/233` `host_game_name` picks the name using `ci.extensions`, while `content_prepare` uses the core's `valid_extensions`. If they disagree on "zip", the UI looks for the wrong `.state.auto` (medium confidence).
- `host/host.c` ≈1398-1420 `bench_start` execs the driver without `retro_unload_game`. Cores that flush their own files at unload lose data from that session.
- `host/saves.c:313-323` `flush_region`: `malloc` failure leaves `pending` stuck, so that region is skipped for the session. `saves_init` returns `-errno` after `pthread_create` (which does not set errno).
- `host/core.c` ≈463 `SET_CONTENT_INFO_OVERRIDE` leaks on a second call. `options.c:213/231` has unchecked `strndup`.
- Long ROM names: stem + `.state.auto.png.tmp` exceeds NAME_MAX when the stem is over about 236 bytes, so states fail cleanly with ENAMETOOLONG.
- `ui/games.c` ≈929: `favorite = e->favorite || g->favorite`, so a gamelist.xml favourite can't be un-favourited permanently.
- `ui/loader.c:797-801`: `finish()` reads `L->warm`/`worker_us` without the lock (timing log only). `fswarm.c:522-541` holds its lock for the worker's whole warm-up, so the UI blocks on `cache_load`/`update_cache_dir` meanwhile.
- `ui/transfer_ui.c` ≈1089-1097, 1148-1153: `ui->usb_dialog` is compared by pointer after reloads, so it can match an unrelated dialog at the same address.
- `ui/games.c:209-243`: hiding `.m3u`/`.cue` members depends on `readdir` order.
- `power.c` ≈1484 `power_set_timezone`: `setenv("TZ")` after threads exist (hardening; no concurrent `getenv` found).
- `input.c` `scale_axis`: `hi - lo` / `lo + hi` overflow for absinfo ranges near INT_MIN/INT_MAX.
- `transfer/usb.c:199-222`: exFAT labels can carry C0 controls into UI and logs. `import.c:1125`: a read error in `tr_same_content` counts as "differs". `backup.c:440-466`: `console_id` is written non-atomically and the `tr_random` result is ignored. `import.c:1215-1220`: `errno` is read after other calls (`fatal = rootfd < 0 ? -errno : …`).
- `transfer/tr_util.c:821-822`: the copier's writer ignores `*cancel` while waiting on a stalled reader (hung USB). The fallback path adds `s->written` instead of the delta (statistics only).
- `transfer.mk:37` default `TR_CFLAGS` lacks `-D_FILE_OFFSET_BITS=64`. The Buildroot build gets it from `CPPFLAGS`, but the standalone `transfer-arm-check` gets a 32-bit `off_t`. Add the define and `_Static_assert(sizeof(off_t) == 8)`.
- `-Wformat-truncation` (gcc 13): `webshare.c:1045` `mdns_url[80]` can truncate for a 63-character hostname with a port (display only); `usb.c:408` vendor+model into 48 bytes (display only).
- `main.c:197` `P()` mallocs per call when `--root` is set (test mode only; LeakSanitizer reports 1.4-1.9 KB per run). No effect on the device, where `M.root` is empty.

---

## Build and compiler hardening (F-L24)

**What is actually applied today.** The Buildroot image *is* hardened, but only implicitly.
- `output/.config` has `BR2_SSP_STRONG`, `BR2_FORTIFY_SOURCE_1`, `BR2_RELRO_FULL` and `BR2_PIC_PIE`. These are Buildroot 2026.02 defaults and are **not** set in `configs/retrostone2_defconfig`.
- The toolchain wrapper adds `-fstack-protector-strong -D_FORTIFY_SOURCE=1 -Wl,-z,relro,-z,now -fPIE/-pie`.
- The frontend's own Makefiles add none of these. `RSOS_CFLAGS = -std=c11 -Wall -Wextra -D_GNU_SOURCE`.

**Proposed:**
1. In `retrostone2_defconfig`, set these explicitly so a Buildroot default change can't silently drop them:
   ```
   BR2_SSP_STRONG=y
   BR2_FORTIFY_SOURCE_2=y        # or _3 (GCC >= 12): catches more __builtin_object_size cases
   BR2_RELRO_FULL=y
   BR2_PIC_PIE=y
   ```
2. In `Makefile` `RSOS_CFLAGS`, `ui.mk` `UI_WARN`, `power.mk` `POWER_CFLAGS` and `transfer.mk` `TR_WARN` (which already has `-Wformat=2`), add:
   `-Wformat -Wformat-security -Werror=format-security -Wshadow -Wvla`. Consider `-Wformat-truncation=2` as a warning to audit path building.
3. Link rules that ignore `$(LDFLAGS)`:
   - `power.mk:67,70,73,87,90,93`: `rsos-bootreason` and `rsos-clock` are installed on the target.
   - `transfer.mk:71,74`: tests and the host tool.
   - `ui.mk:67`: `rsos-uipreview`.

   Effect: a sanitizer or hardened `LDFLAGS` build fails to link (`undefined reference to __asan_*`, seen during this review), and target tools miss the caller's link flags. Use `$(CC) $(LDFLAGS) -o $@ ...`.
4. Add a `check-asan` target that runs the whole `check` with `-fsanitize=address,undefined -fno-omit-frame-pointer`. Today it would have caught F-M1, F-M2 and F-L25, and it should run in CI. Add `test_loader` under `-fsanitize=thread` too.
5. Fix the `test_host.c:798` length so the ASan run can proceed past `test_bench`. Also fix `host.mk:147` `./$(HOST_TEST)`, which breaks with an absolute `BUILDDIR`.

---

## Areas checked and found sound

**Web share (`webshare.c`):**
- The head is bounded by `REQ_MAX` (8 KiB, answer 431).
- The request line needs `HTTP/1.` and a leading `/`.
- Content-Length is parsed strictly (`strtoll`, full-string, non-negative). Huge values get 507 from the free-space check with no overflow.
- `Transfer-Encoding` is refused with 411.
- Login bodies are capped at 256 bytes and bounded inside the buffer.
- `get_param` checks lengths, and `tr_url_decode` rejects `%00` and truncated escapes.
- Targets come from a fixed table.
- `transfer_relpath_check` rejects empty, absolute, `//`, trailing `/`, dot-leading components (`..`), `\ : * ? " < > |`, controls, invalid UTF-8 and DOS devices, with a depth limit of 8 and `TRANSFER_PATH_MAX`.
- Directories are opened component by component with `O_NOFOLLOW`. The final entry is checked with `AT_SYMLINK_NOFOLLOW`. Symlink and traversal escapes are covered by `webshare_test.sh`.
- Uploads go to a temp file, then fsync, rename and directory fsync (except F-H1).
- The session token is 128-bit from `getrandom`, compared in constant time, in an HttpOnly, SameSite=Strict cookie.
- CSRF: non-GET needs `X-Requested-With: rsos`.
- DNS rebinding: `Host` must be an IPv4 literal, localhost or our own name.
- Peers are limited to private, link-local or CGNAT addresses.
- The PIN is 6 digits without modulo bias, regenerated per start.
- The page uses `textContent` for file names (no DOM XSS), and there is a strict CSP.
- `SIGPIPE` is ignored and sends use `MSG_NOSIGNAL`.
- Stop shuts down client sockets and waits for workers.

**netnames:**
- `read_name` bounds every label, caps compression jumps at 16, and checks `outn` including the NUL.
- Question and answer building check `outmax`.
- NBNS needs at least 50 bytes before reading.
- Sockets are CLOEXEC and non-blocking; packets over 1500 bytes are truncated safely.
- No memory errors in 500k fuzzed packets.

**QR (`qr.c`):** buffers are sized for version 10 (346 codewords, blocks of 5×160, ECC ≤ 26). Input over 213 bytes returns `-EMSGSIZE`.

**Signals, fork and exec:**
- Handlers set `sig_atomic_t` flags and write to an eventfd, with errno saved.
- No SA_RESTART, and EINTR is handled.
- SIGCHLD keeps its default; `waitpid` uses specific pids, and the double fork reaps its intermediate child.
- Children (`launch.c`, `spawn_detached`, `bench_run.c`) call only async-signal-safe functions before `execv` and reset the mask and SIGPIPE.
- Every long-lived fd is CLOEXEC: DRM, epoll, timerfd, netlink, evdev, eventfd, the log pipe and file, sockets, and `fopen("re")`.

**Display:**
- `drmHandleEvent` is only called after `poll`.
- `drain_flip` has a timeout, and stale flip generations are discarded.
- The dumb buffer size is computed by the kernel, and `mmap` failures are handled.
- `drmDropMaster`/`SetMaster` results are handled.

**Input:** `ev.code` is bounds-checked, device slots are bounded, ENODEV closes the slot, and the axis math guards against division by zero.

**Loader thread:** all shared state is under `mu`, the worker is joined (never detached), lists are replaced only when no list screen is open, and TSan is clean.

**Durable writes:**
- `hwrite_atomic` (SRAM, states, options, cores.ini, bench), `file_write_atomic` (settings, gamedb, wpa_supplicant.conf, rsos.env), `write_durable` (resume.ini), `psys_write_atomic` and `tr_sink` all do tmp, fsync, rename, then directory fsync, with errors checked.
- No user file is truncated in place.
- ENOSPC/EROFS keep the old file.
- The exceptions are F-M7, F-M8, F-M9 and F-L15.

**Libretro environment:**
- Pixel formats (0RGB1555, RGB565, XRGB8888; others refused), dupe frames, `RETRO_HW_FRAME_BUFFER_VALID`, `get_proc_address`/`get_current_framebuffer`, and GEOMETRY/AV_INFO (except F-M5) are correct.
- Core options V1/V2/INTL parsing is bounded.
- `serialize_size` is re-queried for each save, and the audio batch buffer grows under the mutex.

**Shell injection:** none. No ROM or user string reaches `system`/`popen`. The only `sh -c` paths run fixed, configured helpers.

**Audio:** `snd_pcm_writei` handles `-EPIPE`/`-ESTRPIPE` through recover, and `-EAGAIN` and short writes are handled.

---

## Top 10 to fix first

1. **F-C1**: take `d_update`/`d_timeout` off `osk_ops` (a one-line fix for heap corruption in the WiFi and hostname keyboards).
2. **F-H1**: make `tr_sink` temp names unique with `O_EXCL`, add a per-path in-flight lock in the web share, and use `RENAME_NOREPLACE` when not overwriting. This is silent ROM or save corruption, reproduced.
3. **F-H3**: reorder `enum item_id` in `host/menu.c` (a one-line fix that stops silent, persistent core-option changes).
4. **F-H2**: stop freeing `gamedb` core strings that list entries share.
5. **F-H4**: look the game up by path in `game_item` instead of keeping a pointer across `games_sort`.
6. **F-M1**: clamp the resampler phase (`p >= PHASES`). It is hit in normal play; ASan shows it in every game run.
7. **F-H5**: bound the image cache with LRU eviction by byte budget.
8. **F-M8 + F-M7**: make the `.bak` rotation checked and reversible, and stop treating a failed read as an empty file before rewriting settings, gamedb or `rsos.env`. Also fix the fd leak (F-L13) that feeds it.
9. **F-M16 + F-M17**: mount sticks with `MS_NOSYMFOLLOW`, use `lstat`/`O_NOFOLLOW` on the import roots, and pin the import to the stick's root fd/`st_dev` (abort on removal).
10. **F-M2, F-M3, F-M4, F-M5, F-M6**: the remaining memory-safety bugs (freed backup menu, dialog on a full stack, icon eviction, HW readback pitch, 32-bit `rzip` bounds). Then add a `check-asan` target (F-L24) so this class is caught automatically.

---

## Fixes

Fix pass of 2026-09-27, `frontend/` only. **Regression tests** are named for every critical and
high finding (and most medium ones). Each critical/high test was also run against the
**original** sources (the review's snapshot) and fails there, as shown in the "before" column.

### Verification

| check | result |
|---|---|
| `make check` (host) | pass, 0 warnings |
| `make check-asan` (new: every check under ASan + UBSan + LeakSanitizer, no recovery) | pass, 0 reports, 0 warnings |
| ARM cross checks (`ui-arm-check`, `transfer-arm-check`, `power-arm-check`; display, host and `main.c` compiled with `arm-linux-gnueabihf-gcc`) | 0 warnings |
| the review's scenarios against `rsos-webshare-host` (6 concurrent same-path uploads, slowloris, lockout, idle stop with slow clients) | before: 6/6 files mixed, 503 at 13 s, owner locked out, share never stops. After: 0 mixed (the second upload gets `409 busy`), 200 at 13 s, the owner's device logs in, idle stop after 4 s |
| the review's mutation fuzzer (`netnames_reply`; `read_head`, `get_param`, `host_ok`, `check_auth`, `transfer_relpath_check`, plus whole requests through `handle()`), ASan + UBSan fatal | 100 M name packets and 20 M HTTP requests (a quarter of them through `handle()`) in 288 s: no memory error, no UB (the review's F-L10 report is gone) |

### Critical and high

| id | fix | regression test | before (original code) |
|---|---|---|---|
| F-C1 | The keyboard has its own `update`/`timeout`/`describe` ops. Every dialog entry point that can receive another screen type (`d_update`, `d_timeout`, `dialog_set_countdown`, `dialog_select`) checks the ops pointer first (`screen_is_dialog()`), so ops can no longer be shared across screen types by mistake. | `check-ui`: the WiFi name and password keyboards through 1.5 s of timer ticks, typing, cancel (under `check-asan`) | ASan heap-buffer-overflow in `d_update` (menu.c:528) |
| F-H1 | `tr_sink_open` creates `.name.<8 random hex>.rsos-part` with `O_EXCL` (loops on EEXIST); a writer can only commit its own temp file. The web share keeps a per-destination in-flight set (case-insensitive, `/data` is exFAT) and answers `409 {"error":"busy"}` to a second upload of the same path. `overwrite=0` commits with `renameat2(RENAME_NOREPLACE)` (then a hard link, then check + rename) and answers `409 exists` if the name appeared meanwhile (the USB import). The page shows "Already being uploaded". | `test_transfer`: two sinks of one name, interleaved writes (result whole), `no_replace` loses atomically; live share: B uploads the same path (other case) while A is mid-body, `409 busy`, A's file intact, no temp file left | web scenario: 6/6 committed files mixed |
| F-H2 | Core ids are interned in the gamedb for its lifetime: `gamedb_set_core` never frees a string another copy of the game (Favorites, Last played) still uses. The game options menu also updates every other copy of the game (`game_core_sync`). | `test_loader` 8: two copies of a game, the core changed on one, the other read (ASan) | ASan heap-use-after-free in `gamedb_set_core` |
| F-H3 | `IT_DPAD_ANALOG`/`IT_INFO` are before `IT_OPT_BASE` (the option range is last, `_Static_assert`), `change()`/`activate()` ignore info rows, options go through `is_option()`. | `rsos-launch-test` run 5: step core option #1, A on an info row; the per-game file has option #1 and not option #2 | option #1 not stepped, option #2 changed |
| F-H4 | The game options menu keeps the game's system and path (freed by `on_destroy`) and looks the game up in the list for each action; a game gone from the list closes the menu. | `check-ui`: 3 games, options of the last one, Favorite on then off: nothing is left a favourite | the 1st game moved into the slot got the second toggle, "C Game" stayed a favourite |
| F-H5 | The image cache is an LRU: unreferenced images stay cached up to 24 MB of pixels and 256 entries (mapped `.rpx` count too), least recently used evicted first; images in use are never evicted; lookups move hits to the front. `img_set_cache_budget()`, `img_cache_entries()`. | `test_loader` 7: 300 pictures browsed with a 20-picture budget: at most 21 cached, the one in use intact, LRU order | (new API; unbounded growth) |

### Medium

| id | fix | test |
|---|---|---|
| F-M1 | `sinc_frame` clamps `p >= PHASES` to the end point (`p = PHASES-1, mu = 1`). | `rsos-host-test`: DC through a step of 1 - 2^-30 (fractions round to 1.0f); before: ASan heap-buffer-overflow in `sinc_frame` |
| F-M2 | `backup_item` uses locals after `pop_menus()`. | `check-ui` export flow under `check-asan` |
| F-M3 | `ui_push` returns false when the stack is full; `dialog_open` then returns NULL; callers handle it (LCD trial reverts, USB dialog becomes a toast). A new-controller question is a toast while a dialog or keyboard is on top, so hot-plugging no longer piles dialogs up. | code review; `check-asan` |
| F-M4 | The icon table evicts the least recently used entry (a use counter), so the ≤ 12 icons `help_draw` fetched stay valid; rating stars are the element's own images, never borrowed from the table. | `check-asan` over the UI tours |
| F-M5 | `hwr_readback(&w, &h)` returns the clamped size and the callers use it for the pitch and the frame (a crop is logged once). Resizing the FBO when the maximum geometry grows is not done: a core exceeding its declared maximum gets a cropped frame, not an over-read. | compile (host and ARM); no GL test on the host |
| F-M6 | `if (csize > n - pos ...)`: no wrap on 32-bit. | code review |
| F-M7 | `file_read` treats a short read as an error; new `file_read_err`/`file_read_user`. settings.ini, gamedb.tsv and rsos.env: a read error (not ENOENT) loads `<file>.bak` if it can be read, and the file is **never rewritten** that session (`settings_save`/`gamedb_save`/`hw_env_set_overlay` refuse, the settings stay pending in memory). They are written with `file_write_atomic_bak()`: the previous version is kept as a durable `.bak` copy, then one atomic rename (the live file exists at every moment, also on FAT). cores.ini: `bench_ini_set_file` refuses on a read error and keeps `cores.ini.bak`; `coreinfo_pick` falls back to it. | `rsos-host-test` (unreadable cores.ini not rewritten); `check-frontend` |
| F-M8 | `tr_replace_file()` (transfer) and `hwrite_atomic_v` (host): the backup is made or nothing is replaced (-ENAMETOOLONG if `name.bak` cannot exist); a hard link + one rename where possible, else two checked renames with the first undone if the second fails. Loaders recover a missing file from `.bak` (`sram_load`, `state_exists`, `state_load` already did). UI files: see F-M7. | `test_transfer`: `.bak` that cannot be made (a folder) → the commit fails, the old file is kept |
| F-M9 | No section cap (dynamic array), abort on any read error but ENOENT, values quoted, `cores.ini.bak` kept. | `rsos-host-test`: 100 sections kept (before: 64), unreadable file not rewritten |
| F-M10 | `state_load` waits for the save queue; `state_exists` reports a state that is queued or being written (and one that only exists as `.bak`). | code review |
| F-M11 | A failed SRAM write backs off (2 s doubling to 5 min), logs and shows one message per error kind, "Game saved" when it works again. | code review |
| F-M12 | The driver forwards `RSOS_SIG_SLEEP`/`WAKE` to the running step (or ignores them between steps). The supervisor signals are blocked across every exec (game → driver, driver → step, driver → game) and unblocked once the handlers exist (`host_supervisor_signals()`). | `rsos-launch-test`: sleep + wake sent to the driver during the benchmark; before: the driver died (`crashed 1`, no report) |
| F-M13 | Teardown is `context_destroy` (context current), then `retro_unload_game`, `retro_deinit`, `hwr_deinit`, also in the power-off path. | code review |
| F-M14 | Each device keeps a bitmap of the mapped keys it holds: a press or release counts once on the pad (the event the kernel keeps after an overflow is not counted twice). After `SYN_DROPPED` everything up to the next `SYN_REPORT` is dropped, then the key state (only this device's share of the merged built-in pad) and every mapped ABS axis are re-read. `TODO`: confirm on the device. | build; not reproducible without uinput |
| F-M15 | A failed frame backs off (100 ms, 250 ms, then 500 ms), logs the first 3 then one in 50, and re-creates a missing UI surface every 4th try. | `check-frontend` (no "frame: no buffer" in the power-off step) |
| F-M16 | Sticks are mounted (and remounted) with `MS_NOSYMFOLLOW`; `resolve_dir_ci` checks every component with `lstat`; the backup marker is opened `O_NOFOLLOW` and must be a regular file. | `test_transfer`: a stick with `bios`, `themes` and `RetroPie/roms` symlinked outside: nothing from outside in the plan |
| F-M17 | The import opens the stick's root once and every source under it (`openat`, `O_NOFOLLOW`); a stick plugged in at the same mount point is never read. ENOENT/ENOTDIR/EIO when the stick's root is gone is fatal `-ENODEV`, "The USB drive was removed". The UI cancels a running copy when its drive's REMOVED event comes. | `test_transfer`: the stick replaced during the import (the old one's file is imported), then pulled (fails with `-ENODEV`, "removed", nothing written) |

### Low

| id | status | fix, or why deferred |
|---|---|---|
| F-L1 | fixed | The request head (and the login body) must arrive within 10 s (`poll` with a deadline). Uploads slower than 1 KB/s on average after the first minute are dropped. The idle stop only waits for authenticated requests in progress, and only logins and authenticated requests count as activity. |
| F-L2 | fixed | The lock check and the verdict are in one critical section after the body is read. Lockout per peer address (5 wrong PINs: 30 s doubling to 10 min) plus a global one (20 wrong PINs from any peers: 60 s, not doubling); the console shows the longest. `test_transfer`: 127.0.0.2 locks itself, 127.0.0.1 still logs in. |
| F-L3 | fixed | `transfer_before_game`/`transfer_network_off` always stop netnames; `transfer_poll` stops it once the share has stopped by itself (and picks up the share's last changes). |
| F-L4 | fixed | An interface table (address, netmask) refreshed with the 10 s group join (at most once a second for an unknown interface); no `getifaddrs` per packet. |
| F-L5 | fixed | Unicast answers (LLMNR, NBNS, legacy mDNS) only to sources on the interface's subnet. (IP TTL = 255 is not checked.) |
| F-L6 | fixed | Interfaces that disappear are dropped from the table; a new or re-addressed interface is joined and announced again. |
| F-L7 | fixed | `fcntl(F_DUPFD_CLOEXEC)`. |
| F-L8 | fixed | Import and backup restore `bytes_done` to its value before the file on any failure. |
| F-L9 | deferred | Propagating the copier's deferred folder fsync and `syncfs` errors to the right file needs a rework of `tr_copier_commit`'s error accounting and of the result screens; the data path itself (fsync of each file before its rename) is sound. |
| F-L10 | fixed | NBNS name bytes outside `'A'..'P'` are rejected before the shift. The fuzzer runs with UBSan fatal. |
| F-L11 | fixed | Axis/hat targets past the pad's buttons are ignored in `handle_abs`. |
| F-L12 | deferred | Needs a design for the console output (non-blocking fd that drops on EAGAIN, closing the pipe when the thread exits, rotating `frontend.log`) and a test on the serial console of the device. |
| F-L13 | fixed | The fd is closed on every path. |
| F-L14 | fixed | The child resets the signal mask and `SIGPIPE`; `/dev/null` is opened `O_CLOEXEC`. |
| F-L15 | fixed | A user options file that exists but cannot be read is marked and never rewritten that session. |
| F-L16 | deferred | Keeping every `GET_VARIABLE` value alive for the core's lifetime needs an arena in `options.c`; no core that keeps these pointers across a re-declaration is known. |
| F-L17 | fixed | Bits the frontend does not know are cleared (written back to the core). |
| F-L18 | fixed | `hmkdir_p("")` returns -ENOENT. |
| F-L19 | fixed | `-EINTR` retries; `audio_close` drains non-blocking with a 500 ms deadline, then drops. |
| F-L20 | fixed | `resume.ini` removal is followed by an fsync of `/data/rsos`. |
| F-L21 | partly | `STBI_MAX_DIMENSIONS 4096`: larger pictures are refused (a log line), never decoded. Validating theme sizes (NaN, huge) is deferred: it touches every theme property parser. |
| F-L22 | deferred | The temp names are now random, so a sweep can only go by age; the safe place is the boot script (system layer) or an age-based sweep at import start. Proposed for later. |
| F-L23 | deferred | Avoiding collisions needs a suffix policy ("name (2).sfc") and reporting it in the import summary. |
| F-L24 | fixed in `frontend/` | `-Wformat -Wformat-security -Werror=format-security` in every module's flags; every host link rule uses `$(LDFLAGS)` and the ARM ones `$(ARM_LDFLAGS)`; `make check-asan`; `host.mk` runs its tests without `./` (absolute `BUILDDIR`); 64-bit `off_t` in every module (`-D_FILE_OFFSET_BITS=64`, asserted in `tr_util.c`). `-Wshadow`/`-Wvla` are not added (hundreds of existing shadowing warnings in the UI and host). The defconfig hardening is suggested to the lead (below). |
| F-L25 | fixed | `HWRITE_LIT()`: literal lengths come from `sizeof`. |

Also fixed on the way: `main.c` `P()` leaked a string per call in test mode (interned now, which
LeakSanitizer needed); a `-Wformat-truncation` warning in `bench.c` at `-O1`; the USB mount base
buffer (48 bytes) was too short for the ASan build's test tree; the test core crashes with a real
SIGSEGV also under UBSan.

**Defconfig suggestion (system layer, not edited here):** set explicitly in
`configs/retrostone2_defconfig` (and the rpi4 one) so that a Buildroot default change cannot drop
them: `BR2_SSP_STRONG=y`, `BR2_FORTIFY_SOURCE_2=y` (or `_3`, GCC ≥ 12), `BR2_RELRO_FULL=y`,
`BR2_PIC_PIE=y`.

**Needs the system layer:** `/boot/rsos.env.bak` now appears next to `rsos.env` (harmless for
U-Boot); an age-based sweep of `.*.rsos-part` files at boot would close F-L22.

### Frontend side of the system fixes (S5, S7, boot notes, `rsos-net`)

Asked by the system fix pass, done in the same batch:

| item | change | test |
|---|---|---|
| S5 boot confirmation | `/run/rsos/menu-up` is still written at the first frame of a loaded UI (charge mode too). `rsos-boot-ok` is spawned (flag `boot_ok_spawned`) only when the normal menu runs: never in charge mode, and right after the charge-mode exit. | `check-frontend` 3a: charge mode, power key, then "would run rsos-boot-ok" after "charge mode: booting normally" |
| S7 watchdog | The menu process only (never `--splash` or `--run`: the fd is `O_CLOEXEC`) opens `/dev/watchdog` before its main loop and pets it (`WDIOC_KEEPALIVE`) every 2 s: from the loop (its poll timeout is capped for it, so the screen off and charge mode are covered) and from `launch_idle()` during games and the benchmark (every ~100 ms). Orderly exits (SIGTERM teardown, the power-off exit) write `'V'` then close. Headless: only with `--watchdog PATH` (a plain file: the keepalive is written as `k`). | `check-frontend` 1: ≥ 2 keepalives over the menu and a game, `V` last, nothing in the game log |
| boot notes | Once the normal menu is up: `/run/rsos/data-restore-failed`, `data-reformatted`, `data-error` and `boot-fallback` give one OK dialog (short wording, the fallback slot logged); `data-fsck` alone gives a calm toast ("The SD card was checked after an unexpected power loss"). New `ui_message()`. | `check-frontend` 3a (fallback dialog), 3b (fsck toast) |
| `rsos-net` up to 60 s | It already ran as an async job; the item shows "please wait..." and a toast says it is turning on/off. The job's pipe is no longer drained with a blocking read after the helper exits (a daemon it started could keep it open and freeze the menu, now also a watchdog reboot); same for the game's status pipe in `host_launch()` (and a `read(0)` spin when that buffer was full). | `check-frontend`, `rsos-launch-test` |

Watchdog audit of the menu's main thread (the A20 maximum is 16 s): USB scans and copies run in
threads; the game, its state saves and the benchmark run in the game process (the menu pets from
`launch_idle()`); the game lists load in the loader thread; network helpers are async jobs. What
remains in the main thread and could in theory approach 16 s: `transfer_stop_jobs()` at exit if a
copy is stuck on a hung USB read (the copier's writer does not see the cancel while waiting on its
reader, a known small item), opening a large system's list on demand before the loader got to it,
and the idle `syncfs` of the image cache after a first boot that decoded many pictures. The loop
pets right before the exit teardown. `TODO(hw)`: measure these on the device.
