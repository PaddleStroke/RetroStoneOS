# Input design: buttons, gamepads, hotkeys

Everything from a physical button press to the emulator core goes through ONE path in the frontend.
There is no RetroArch, SDL or udev. The kernel delivers evdev events, and the frontend maps them to the libretro
**RetroPad** that every core understands.

```
 built-in buttons (gpio-keys, gpio-keys-polled)  ┐
 analog stick (adc-joystick "analog-stick")       ├─> evdev /dev/input/eventN ─> frontend input layer
 USB / Bluetooth gamepads (hid, xpad, sony, ...)  ┘        (inotify for hotplug)     │
                                                                                      ├─ hotkey filter (Select+...)
                                                                                      ├─ UI navigation
                                                                                      └─ RetroPad ports 1..4 ─> core
```

## 1. Physical layout and the RetroPad
The RetroStone2 has a Nintendo/SNES layout, and so does the libretro RetroPad, so the mapping is **positional**:

| Position | RetroStone2 pin | evdev code | RetroPad |
|---|---|---|---|
| right | PH0 (K2) | BTN_EAST | A |
| bottom | PH11 (K4) | BTN_SOUTH | B |
| top | PH12 (K1) | BTN_NORTH | X |
| left | PH20 (K3) | BTN_WEST | Y |
| L1 / R1 | PH23 / PH22 | BTN_TL / BTN_TR | L / R |
| L2 / R2 | PH27 / PH26 | BTN_TL2 / BTN_TR2 | L2 / R2 |
| Select / Start | PH15 / PH14 | BTN_SELECT / BTN_START | Select / Start |
| D-pad | PH19/PH7/PH4/PH16 | BTN_DPAD_* | Up/Down/Left/Right |
| optional C / Z | PH3 / PH13 | BTN_C / BTN_Z | L3 / R3 (the RetroPad's spare buttons); see §1b for per-system layouts |
| analog stick (add-on, not on every unit) | AXP GPIO0/1 | ABS_X / ABS_Y | Left analog |

The unit has L1, R1, L2 and R2, and **C and Z are two optional front buttons** that turn the face into a 6-button pad
(Mega Drive style: A B C on the bottom row, X Y Z on the top row). Some units don't have them.

### 1b. Per-system default layouts (shipped remaps)
The RetroPad has no C or Z, so the built-in C/Z report as L3/R3, and each system ships a default remap in
`/usr/share/rsos/remaps/<system>.ini`. The user can override it per system or per game in the in-game menu
(`/data/rsos/remaps/`). A remap works in RetroPad terms (physical position to RetroPad id), before the core sees input.
**Physical face layout** (front view, taken from the pad positions in `retrostoneA20-1.15.brd`; the pads are on the bottom layer, so the
board coordinates are mirrored; confirmed by a PCB photo). The six pads form two tilted rows:
```
            X (K1, PH12)     [opt BTN_C, PH3]      <- top row:    Y   X   PH3
  Y (K3, PH20)         A (K2, PH0)
  [opt BTN_Z, PH13]  B (K4, PH11)                  <- bottom row: PH13  B   A
```
The C/Z buttons are an owner mod (drill the case and add silicone), so **almost no units have them**. Defaults must be
perfect without them. The GPIOs just read "released" when a button isn't fitted, so the frontend can't detect the mod;
a Settings switch "Extra C/Z buttons fitted" (default off) enables the 6-button remaps below.

- **Mega Drive / 32X / Sega CD (picodrive)**: picodrive reads MD buttons as RetroPad Y=A, B=B, A=C, L=X, X=Y, R=Z, Select=Mode.
  - Default (no C/Z): identity. Left/bottom/right = MD A/B/C, and L1/top/R1 = MD X/Y/Z. The 6-button pad option is on,
    so 6-button games still work.
  - With "C/Z fitted": the bottom row PH13/B/A becomes MD A/B/C and the top row Y/X/PH3 becomes MD X/Y/Z. As RetroPad remaps:
    BTN_Z(PH13)→Y, K4→B, K2→A, K3(left)→L, K1(top)→X, BTN_C(PH3)→R.
- **Arcade 6-button games** (Street Fighter II and similar, in FBNeo/MAME 2003-Plus): the same 6-button face gives LP/MP/HP on the top row and
  LK/MK/HK on the bottom row. It's shipped as the default for the `arcade`/`fbneo` systems, and set per game where needed.
- **Neo Geo** (4 buttons A/B/C/D): the default follows the physical layout of the MVS/AES arcade panel.
- **Atari 800 / 5200** (atari800 core): A fires (on the computers B fires too; on the 5200 B is the second button),
  R2 opens the core's on-screen keyboard (its own button is L3/R3, the C/Z buttons); details in docs/cores.md
  "Atari 800 and 5200".
- **SNES/GBA/GB/NES/PS1**: identity (positional). C/Z are free, and could become turbo buttons later (P2).
- **N64**: see §1c.

### 1c. Analog stick absent (it is an add-on)
The stick is optional, so no system may *require* it:
- The input layer reports whether an analog source exists for each port: the built-in stick when fitted, or the external pad's stick.
- **N64 without any stick**: the host enables "D-pad → left analog" for that port (full deflection, and optionally Z/C
  as walk modifiers). The N64 C-buttons go to X/Y/L2/R2 or C/Z per the shipped `n64.ini` remap. It works, but badly for
  some games, so the N64 system page shows a hint: "an analog stick or USB pad is recommended".
- **PS1**: DualShock analog mode only when a stick is present, otherwise a digital pad.
- The UI never depends on the stick. It's only an extra navigation input.

External pads map **by position** too, like RetroArch: the bottom face button is RetroPad B on every pad,
even when it's labelled "A" on an Xbox pad. This keeps muscle memory consistent across controllers.

## 2. Recognising gamepads automatically (no user setup for known pads)
Each input device is resolved in this order:
1. **Built-in devices**, by name: every device whose name starts with `RetroStone2` is merged into ONE logical pad, plus
   `analog-stick`. That stick is optional: a reading outside 0..3000 means it isn't fitted, so ignore it. The names
   come from the board profile (board.ini `builtin_pad_prefix`, `builtin_stick`, docs/porting.md); a board without
   them (a Raspberry Pi with USB pads) has no built-in pad.
2. **User mapping** saved earlier: `/data/rsos/input/<guid>.cfg` (made with the "Configure controller" screen).
   "Configure controller" first asks what kind of pad it is, then walks through that pad's buttons. Only A, B and
   Start are needed; any other step is skipped by pressing a button already set, or by waiting 6 s.
   - **SNES, Xbox, PlayStation, Switch or similar** (`modern`): the usual 20 steps, by position.
   - **Nintendo 64** (`n64`): A, B, Z, L, R, Start, the four C buttons, the stick.
   - **GameCube** (`gc`): A, B, X, Y, Z, L, R, Start, the stick, the C stick.
   - **Forget my set-up (automatic)** (shown once a pad has a user mapping) deletes it: the pad is mapped by (3)-(6)
     again, or is unconfigured.
   - A mapping without the left stick (skipped, or made before the stick could be set) still gets `ABS_X`/`ABS_Y`.
   - The wizard reads a stick push past a third of the axis range: an 8-bit stick resting at 128 never goes
     past half.
3. **Built-in profiles** (`g_profiles` in `src/input/input.c`) for pads whose kernel layout no database line describes
   right, by vendor:product (and, for pads that pass as another, by the keys they have). All are hid-nintendo's
   (linux 6.18): the Switch Online **N64** pad (C-up on Select, B on the right button), the Switch Online **NES**
   pads (they say they are Joy-Cons; A at the bottom), the Switch Online **Genesis** pad (the database's line is for
   another layout).
   Then pads of **drivers that follow the Linux gamepad spec**: `xpad` (Xbox 360/One/Series over USB, many clones),
   `playstation` (DualSense), `sony` (DualShock 3/4), `nintendo` (Switch Pro, Joy-Cons, Switch Online SNES),
   `hid-steam` (Steam Controller; its left touch pad is no d-pad, its clicks are). The driver is read from
   `/sys/class/input/eventN/device/device/driver` and logged (`input: ... (xpad) ...`). The database is not asked
   for them: a line made under another driver or version could only be wrong.
4. **SDL GameControllerDB** (`gamecontrollerdb.txt`, zlib licence, shipped in the rootfs). We compute the SDL-style GUID from
   the evdev `input_id` (bus, vendor, product, version) and parse the mapping line ourselves. SDL isn't needed. This covers
   thousands of USB/BT pads, including cheap clones, adapters and 8BitDo pads. The kind of pad comes from its name
   there: "GameCube" or the word "GC" is a GameCube pad (an adapter for both GameCube and N64 maps the GameCube way),
   "N64" or "Nintendo 64" an N64 pad.
5. **Linux gamepad spec**: any other device with BTN_GAMEPAD, by its codes (already positional).
6. **Unknown device**: the UI shows a "New controller detected: hold any button to configure" prompt, which opens a guided
   mapping screen (the EmulationStation "configure input" flow) and saves to (2).

### Kinds of pad
The SDL names in a mapping mean what the database means by them for that kind of pad (`input.h`,
`enum input_pad_type`):

| Kind | Names | In a game (RetroPad) | In the menus |
|---|---|---|---|
| `modern` | by position: `a` bottom, `b` right, `x` left, `y` top | `a` = B, `b` = A, `x` = Y, `y` = X | A (right) confirms, B (bottom) goes back |
| `n64` | `a` = A, `b` = B, `-righty`/`+righty`/`-rightx`/`+rightx` = C-up/down/left/right (buttons or a stick), `lefttrigger` = Z, `leftshoulder` = L, `rightshoulder` = R | A = B, B = Y, C-up/down/left/right = X/A/L3/R3 **and** the right stick (what the N64 core reads as C), Z = L2 | A confirms, B goes back, Z is Select |
| `gc` | by label: `a` = A, `b` = B, `x` = X, `y` = Y, `rightshoulder` = Z, `lefttrigger`/`righttrigger` = L/R, `rightx`/`righty` = C stick | A = B, B = Y, X = A, Y = X (by position), Z = R1, L/R = L2/R2 | A confirms, B goes back |

- N64 and GameCube pads have no Select: **Z + Start held 1 s** opens the in-game menu, and quits a native game.
- **Home / Guide / PS** (any pad that has one): opens the in-game menu; held, it quits a native game.
- A **PlayStation pad's touch pad** is a device of its own with ABS_X and a few keys: it is not taken for a pad
  (no BTN_TOUCH / multitouch axes on a pad). The motion sensors (PlayStation, Switch) have no keys: ignored too.
- Not supported: Nintendo's GameCube adapter (WUP-028, 057e:0337) and a Mayflash in its "Wii U" mode need a driver
  Linux does not have; a Mayflash adapter in **PC mode** works (the database knows it). Two Joy-Cons are two pads,
  not one (that needs joycond).
- Test: `make check-input` (`n64_pad`, `known_pads`: each known pad laid out from its kernel driver's codes or its
  database line, with no hardware).

Additional handling:
- **Hats** (ABS_HAT0X/Y) become the D-pad.
- **Analog triggers** (ABS_Z/ABS_RZ, ABS_GAS/BRAKE) become L2/R2 over a threshold, and are also passed as analog.
- **Sticks**: sticks go to the RetroPad analog axes. In menus the left stick also navigates.
- **Hotplug**: inotify on `/dev/input` (devtmpfs creates the nodes, so no udev is needed). Pads can be plugged in or unplugged
  at any time, including mid-game. On unplug, the port gets a "disconnected" state, the game pauses if it was P1, and a toast is shown.
- **Keyboards** drive the menus and are not a player in games (they get no port). Their fixed mapping
  (`map_keyboard()` in `src/input/input.c`): arrows = D-pad, Enter (and keypad Enter) = A, Esc and Backspace = B,
  X = X, Y = Y, Page Up / Page Down = L / R, Home / End = L2 / R2, Tab = Select, Space = Start. In a game (issue #6)
  a keyboard can still leave it: **Esc opens the in-game menu** (in the menus Esc stays B), and **Tab works as the
  hotkey button** of section 4 (Tab+Space exits, Tab+X menu, Tab+Page Down / Page Up save / load a state...),
  from any keyboard whatever the player order. Test: `make check-input` (`keyboard_in_game`).
- **Rumble** (done, batch 2): the libretro rumble interface (`retro_set_rumble_state`) is forwarded to an evdev
  FF_RUMBLE effect on pads that support it (`input_rumble()` in `src/input/input.c`). The pad's node is opened
  read-write (read-only fallback: no rumble); `EV_FF` + `FF_RUMBLE` in its capabilities marks it. One effect per pad,
  uploaded with `EVIOCSFF` (strong = the libretro strong motor, weak = the weak one, replay 65 s) and updated in
  place when the core's strength changes; both at 0 stops it. Cores repeat the same state every frame: an
  unchanged state writes nothing. Everything stops on menu/exit (`input_set_rumble(false)`, `input_close`).
  Setting: Settings > Controls > "Controller vibration" (key `rumble`, default on; the game reads it at launch).
  The built-in pad (gpio-keys) has no motor. Test: `make check-rumble` (`tests/test_rumble.c`: a uinput pad with
  FF_RUMBLE, its upload/play/stop requests checked; SKIP without /dev/uinput).
- **Held buttons** (issue #10, buttons acting as if held):
  - While a key is down, its device's kernel key state (`EVIOCGKEY`) is re-read once a second. A release that
    never arrived is applied and logged (`... released without an event: state corrected`). A key the kernel
    itself reports down for 30 s is logged once (`... held for 30 s (the kernel reports it down)`), which points
    to the button or its wiring rather than to software. The lines are in `frontend.log` (menu) and `game.log`.
  - Buttons still held when the in-game menu or the game switcher hands the pad back to the game (the A on
    "Resume", the B or Start that closed it) reach the core only after a release, like a hotkey's buttons.
  - The built-in stick counts as fitted only once both axes read near the centre (the middle half of the range),
    logged as `input: analog stick detected (x .., y ..)`. Floating inputs on a unit without the stick that
    wander into the range near an edge no longer hold a direction.
  - The button GPIOs also get the SoC's internal pull-ups in the device trees (RetroStone1 and 2), as the
    production RetrOrangePi script did.
  - Test: `make check-input` (`tests/test_input.c`).

## 3. Player assignment
- **Default (`p1 = auto`): the controller that launched the game is player 1.** The UI remembers which device made the
  last button press (`input_last_source_id()`: `"builtin"` for the merged built-in pad, or `"<evdev node>|<SDL GUID>"`)
  and passes it to the game process as `--p1-device <id>`; the game process calls `input_set_p1_device()` before the
  remap lookup. Then the rest in a stable order: the built-in pad next (if it is not P1), then the other pads in
  connection order. Why: the owner started SNES Donkey Kong with a USB pad on the LCD and the built-in buttons still
  drove the game; whoever presses A on the game is the one who wants to play it, docked or not.
- If the id matches no device any more (the pad was unplugged between the menu and the game), or no id was given
  (stand-alone `rsos-run`), the previous rule applies: **handheld (LCD)** built-in pad P1, external pads P2..P4 in
  connection order; **docked (HDMI)** the first external pad P1 and the built-in pad last. The id is matched on node +
  GUID first, then on the GUID alone (a re-plugged pad gets a new node).
- `p1 = builtin` / `p1 = external` (Settings > Player 1) force the old fixed orders and ignore the launching controller.
- **Hot-plug during a game keeps the ports stable**: a newly connected pad takes the next free port and never takes P1;
  a pad that goes away frees its port and the ports above it move down, so P1 is never left empty while any pad is
  connected (unplugging the P1 pad hands P1 to the next player, e.g. the built-in pad, instead of pausing the game).
  A pad plugged back later gets the next free port. In the menus (UI mode) the order is recomputed on every change.
- Up to 4 ports: SNES multitap, N64 and arcade are 4-player; the core's port count is respected.
- Port reassignment happens live, with a toast ("Player 1: 8BitDo SN30").
- Tests: `src/host/tests/test_host.c` (`test_input_ports`): the launching pad becomes port 0, a missing id falls back
  to the policy (LCD and docked), hot-plug and unplug during a game.

## 4. Hotkeys (identical to RetroPie)
Hotkey button = **Select**. Hold Select, then press:

| Combo | Action |
|---|---|
| Select + Start | **Exit** the game (flush SRAM, back to the game list) |
| Select + R (R1) | **Save** state to the current slot |
| Select + L (L1) | **Load** state from the current slot |
| Select + Right | State slot + |
| Select + Left | State slot − |
| Select + X | **In-game menu** (our own, replacing RetroArch's RGUI) |
| Select + B | **Reset** the game |
| Select + R2 | **Fast-forward** on/off (toggle; `▶▶ x3` on the overlay; speed 2x–4x in Settings > Games, key `ff_speed`, default 3; sound muted) |
| Select + L2 | **Screenshot**: PNG of the game picture to `/data/screenshots/<system>/<game>-<YYYYMMDD-HHMMSS>.png` (2x for pictures ≤ 240 px wide) |
| Select + Y | **Game switcher**: the last 8 games with their auto-state pictures; A = save this game (auto state), then start the chosen one where it was left; B = back to the game |

Rules, matching RetroArch/RetroPie behaviour:
- Select pressed and released alone reaches the game normally. The combo button (Start, R, ...) is **not** passed to the core
  when it fires a hotkey.
- As in RetroPie, hotkeys come from the **P1** controller. The built-in pad can also always trigger them (it's the
  console itself).
- Each action shows a short on-screen toast ("State saved, slot 2").
- The in-game menu (Select+X) offers resume, save/load state (with thumbnail and slot picker), reset, core options (per game
  and per system), display (scaling: integer / aspect / stretch, and on the LCD the 60 Hz timing choice), controls (port
  assignment, remap), then exit. The emulation is paused while the menu is open.

## 5. Keys outside the emulator (always active, never passed to cores)
- **Brightness + / −** (PC18/PC22, KEY_BRIGHTNESSUP/DOWN): adjust the backlight and show an overlay. Only active on the LCD.
- **Power button** (AXP209 PEK, `axp20x-pek`, KEY_POWER):
  - short press: suspend/sleep, if we get suspend working (TODO: A20 suspend support in mainline), otherwise nothing
  - long press (~2 s): a clean power-off, which saves SRAM and optionally an auto save state first
  - the AXP hardware 6 s force-off always works as a last resort
- **Volume**: analog wheel, nothing for software to do.

## 6. What the libretro host must do per frame
1. Read all pending evdev events (non-blocking) **right before** `retro_run()`, which gives the lowest latency.
2. Update the hotkey state machine, and drop consumed buttons.
3. Answer `retro_input_state(port, device, index, id)` from the per-port RetroPad state (JOYPAD, including the
   JOYPAD_MASK bitmask, and ANALOG).
4. Support `RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS`, which gives button labels for the remap screen, and
   `SET_CONTROLLER_INFO` (e.g. multitap, N64 controller pak).

## 7. Hardware answers
- C/Z are optional front buttons for 6-button games (top-right = PH3, bottom-left = PH13). They are rarely fitted.
- The analog stick is an add-on, not fitted on every unit.
- To query pad positions: `hardware/tools/eagle_parts_pos.py <brd> K1 K2 ...`.
