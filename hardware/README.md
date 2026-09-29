# RetroStone hardware files

Design files of the RetroStone2 main board (Allwinner A20) and of the RetroStone1 (Allwinner H3), by Pierre-Louis
Boyer / 8BCraft. The RetroStone2 files are the source of
truth for the pin map in [docs/hardware-pinmap.md](../docs/hardware-pinmap.md) and for the device tree
(`buildroot-external/board/retrostone2/dts/`). The folder also holds the RetroStone1 (Allwinner H3) schematic and
layout, the source of `buildroot-external/board/retrostone1/dts/`.

| File | Content |
|---|---|
| `retrostoneA20-1.15.sch` | schematic, board revision 1.15 (Eagle 7.6, XML) |
| `retrostoneA20-1.15.brd` | PCB layout, revision 1.15 (Eagle 7.6, XML) |
| `retrostoneA20-1.15.pdf` | the schematic as a PDF |
| `retrostone2_v1.15.zip` | Gerber and drill files, revision 1.15 |
| `retrostoneA20-1.08.xls` | bill of materials, revision 1.08 (older board) |
| `retrostoneH3-18.sch` | **RetroStone1** (Allwinner H3) schematic, revision 1.18 (Eagle 7.6, XML); source of `buildroot-external/board/retrostone1/dts/` (docs/boards.md, "RetroStone1") |
| `retrostoneH3-18.brd` | RetroStone1 PCB layout, revision 1.18 (Eagle 7.6, XML) |
| `RSN1-1.18.pdf` | the RetroStone1 schematic as a PDF (vector drawing, no text layer) |
| `tools/eagle_nets.py` | lists the parts and nets of a `.sch`: `python3 tools/eagle_nets.py <sch> parts\|part <name>\|net <name>` |
| `tools/eagle_parts_pos.py` | prints the board position of parts from a `.brd`: `python3 tools/eagle_parts_pos.py <brd> K1 K2 ...` |

## Known design notes (for future revisions)

- **RetroStone1: MCP3208 CH2 wired straight to VBAT.** The battery voltage goes to the ADC input without a divider:
  a charged cell (4.2 V) is above the MCP3208's absolute maximum input (VDD + 0.6 V at 3.3 V), and while the 3.3 V
  rail is off the input's protection diode back-feeds that rail from the battery. The owner does not plan to
  produce this board again; a new revision should use a divider (e.g. 2 x 100k, with a small capacitor) or a
  dedicated gauge.
- **RetroStone2: the panel VCC is the always-on 3.3 V rail**, and that rail (buck U17) is enabled by the AXP209
  EXTEN output together with the 5 V boost and the speaker amplifier: a load switch on the panel VCC driven by a
  free GPIO is worth adding (docs/hardware-pinmap.md, "Panel power" and "Power").

## Licence

The design files in this folder (not `tools/`) are open hardware, © 2026 Pierre-Louis Boyer / 8BCraft, licensed
under the **CERN Open Hardware Licence Version 2 – Permissive** (SPDX: `CERN-OHL-P-2.0`, full text in
[LICENSE.txt](LICENSE.txt)). You may study, modify, make and sell boards from them; keep the copyright and licence
notices. The scripts in `tools/` are under the MIT licence of the RetroStoneOS software ([LICENSE](../LICENSE)).
"RetroStone" and "8BCraft" are names of 8BCraft: a derived board should not be sold under them.
