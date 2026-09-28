#!/usr/bin/env python3
"""
mk_test_roms.py - writes three tiny battery-save test ROMs for the host tests
(original work for RetroStoneOS, public domain; no Nintendo data inside):

  sramtest.gb   Game Boy, MBC1+RAM+BATTERY (gambatte)
  sramtest.nes  NES, NROM with battery-backed PRG-RAM (fceumm)
  sramtest.sfc  SNES, LoROM + 2 KB battery SRAM (snes9x2005/2010)

Each one increments the byte at the start of its save RAM once per boot,
writes "RSOS" after it, and shows something that depends on the counter
(GB: a bar of N tiles, NES/SNES: the backdrop colour). Running a ROM twice
through rsos-run must therefore give a .srm whose first byte grew by one.

usage: mk_test_roms.py OUTDIR
"""
import os
import sys


def gb_rom():
    rom = bytearray(32 * 1024)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])        # nop; jp $0150
    rom[0x134:0x13C] = b"RSOSSRAM"
    rom[0x143] = 0x00                                          # DMG
    rom[0x147] = 0x03                                          # MBC1+RAM+BATTERY
    rom[0x148] = 0x00                                          # 32 KB
    rom[0x149] = 0x02                                          # 8 KB RAM
    code = bytes([
        0xF3,                    # di
        0x31, 0xFE, 0xFF,        # ld sp,$FFFE
        0x3E, 0x0A,              # ld a,$0A
        0xEA, 0x00, 0x00,        # ld ($0000),a   ; RAM enable
        0xFA, 0x00, 0xA0,        # ld a,($A000)
        0x3C,                    # inc a
        0xEA, 0x00, 0xA0,        # ld ($A000),a
        0x5F,                    # ld e,a
        0x3E, 0x52, 0xEA, 0x01, 0xA0,   # 'R' -> $A001
        0x3E, 0x53, 0xEA, 0x02, 0xA0,   # 'S'
        0x3E, 0x4F, 0xEA, 0x03, 0xA0,   # 'O'
        0x3E, 0x53, 0xEA, 0x04, 0xA0,   # 'S'
        0xF0, 0x44,              # wait: ldh a,($44)
        0xFE, 0x90,              # cp 144
        0x38, 0xFA,              # jr c,wait
        0xAF,                    # xor a
        0xE0, 0x40,              # ldh ($40),a    ; LCD off
        0x21, 0x10, 0x80,        # ld hl,$8010    ; tile 1 = solid
        0x0E, 0x10,              # ld c,16
        0x3E, 0xFF,              # ld a,$FF
        0x22, 0x0D, 0x20, 0xFC,  # ld (hl+),a / dec c / jr nz
        0x21, 0x00, 0x80,        # ld hl,$8000    ; tile 0 = blank
        0x0E, 0x10,
        0xAF,
        0x22, 0x0D, 0x20, 0xFC,
        0x21, 0x00, 0x98,        # ld hl,$9800    ; clear the BG map
        0x01, 0x00, 0x04,        # ld bc,$0400
        0xAF, 0x22, 0x0B, 0x78, 0xB1, 0x20, 0xF9,
        0x21, 0x82, 0x98,        # ld hl,$9882    ; row 4, col 2
        0x7B,                    # ld a,e
        0xE6, 0x0F,              # and $0F
        0x28, 0x07,              # jr z,skip
        0x47,                    # ld b,a
        0x3E, 0x01,              # ld a,1
        0x22, 0x05, 0x20, 0xFC,  # ld (hl+),a / dec b / jr nz
        0x3E, 0xE4, 0xE0, 0x47,  # skip: BGP = $E4
        0xAF, 0xE0, 0x42, 0xE0, 0x43,  # SCY = SCX = 0
        0x3E, 0x91, 0xE0, 0x40,  # LCD on, BG on, tiles at $8000
        0x18, 0xFE,              # jr $
    ])
    rom[0x150:0x150 + len(code)] = code
    x = 0
    for i in range(0x134, 0x14D):
        x = (x - rom[i] - 1) & 0xFF
    rom[0x14D] = x
    s = sum(rom) - rom[0x14E] - rom[0x14F]
    rom[0x14E] = (s >> 8) & 0xFF
    rom[0x14F] = s & 0xFF
    return rom


def nes_rom():
    prg = bytearray([0xEA] * 16384)          # NOPs, mapped at $C000 (and $8000)
    code = bytes([
        0x78, 0xD8,                          # sei / cld
        0xA2, 0xFF, 0x9A,                    # ldx #$FF / txs
        0xA9, 0x00, 0x8D, 0x00, 0x20,        # lda #0 / sta $2000
        0x8D, 0x01, 0x20,                    # sta $2001
        0x2C, 0x02, 0x20,                    # bit $2002
        0x2C, 0x02, 0x20, 0x10, 0xFB,        # vw1: bit $2002 / bpl vw1
        0x2C, 0x02, 0x20, 0x10, 0xFB,        # vw2
        0xEE, 0x00, 0x60,                    # inc $6000
        0xA9, 0x52, 0x8D, 0x01, 0x60,        # "RSOS" -> $6001
        0xA9, 0x53, 0x8D, 0x02, 0x60,
        0xA9, 0x4F, 0x8D, 0x03, 0x60,
        0xA9, 0x53, 0x8D, 0x04, 0x60,
        0xA9, 0x3F, 0x8D, 0x06, 0x20,        # PPUADDR = $3F00
        0xA9, 0x00, 0x8D, 0x06, 0x20,
        0xAD, 0x00, 0x60,                    # lda $6000
        0x29, 0x0F, 0x09, 0x10,              # and #$0F / ora #$10
        0x8D, 0x07, 0x20,                    # sta $2007 (backdrop colour)
        0xA9, 0x00, 0x8D, 0x06, 0x20,        # PPUADDR = $0000
        0x8D, 0x06, 0x20,
        0xA9, 0x0A, 0x8D, 0x01, 0x20,        # show background
        0x4C, 0x00, 0x00,                    # jmp self (patched below)
    ])
    prg[0:len(code)] = code
    loop = 0xC000 + len(code) - 3
    prg[len(code) - 2] = loop & 0xFF
    prg[len(code) - 1] = loop >> 8
    rti = 0xC000 + 0x3F00
    prg[0x3F00] = 0x40                       # rti
    prg[0x3FFA:0x4000] = bytes([rti & 0xFF, rti >> 8, 0x00, 0xC0, rti & 0xFF, rti >> 8])
    header = b"NES\x1a" + bytes([1, 1, 0x02, 0x00]) + bytes(8)  # battery, mapper 0
    return header + prg + bytes(8192)


def snes_rom():
    rom = bytearray([0xFF] * 32768)
    code = bytes([
        0x78, 0x18, 0xFB,                    # sei / clc / xce (native, 8-bit A/X)
        0xA9, 0x8F, 0x8D, 0x00, 0x21,        # INIDISP = force blank
        0xAF, 0x00, 0x00, 0x70,              # lda $700000
        0x1A,                                # inc a
        0x8F, 0x00, 0x00, 0x70,              # sta $700000
        0xA9, 0x52, 0x8F, 0x01, 0x00, 0x70,  # "RSOS" -> $700001
        0xA9, 0x53, 0x8F, 0x02, 0x00, 0x70,
        0xA9, 0x4F, 0x8F, 0x03, 0x00, 0x70,
        0xA9, 0x53, 0x8F, 0x04, 0x00, 0x70,
        0x9C, 0x21, 0x21,                    # stz CGADD
        0xAF, 0x00, 0x00, 0x70,              # lda $700000
        0x29, 0x03, 0x0A, 0x0A, 0x0A, 0x09, 0x07,  # red = 7 + 8 * (n & 3)
        0x8D, 0x22, 0x21,                    # CGDATA low
        0xA9, 0x00, 0x8D, 0x22, 0x21,        # CGDATA high
        0xA9, 0x0F, 0x8D, 0x00, 0x21,        # INIDISP = on, full brightness
        0x80, 0xFE,                          # bra $
    ])
    rom[0:len(code)] = code
    rom[0x7FC0:0x7FD5] = b"RSOS SRAM TEST".ljust(21)
    rom[0x7FD5] = 0x20        # LoROM
    rom[0x7FD6] = 0x02        # ROM + RAM + battery
    rom[0x7FD7] = 0x05        # 32 KB
    rom[0x7FD8] = 0x01        # 2 KB SRAM
    rom[0x7FD9] = 0x01        # NTSC
    rom[0x7FDA] = 0x00
    rom[0x7FDB] = 0x00
    rti = 0x8000 + 0x7F00
    rom[0x7F00] = 0x40        # rti
    for v in range(0x7FE4, 0x7FF0, 2):   # native vectors
        rom[v] = rti & 0xFF
        rom[v + 1] = rti >> 8
    for v in range(0x7FF4, 0x7FFC, 2):   # emulation vectors
        rom[v] = rti & 0xFF
        rom[v + 1] = rti >> 8
    rom[0x7FFC] = 0x00        # RESET -> $8000
    rom[0x7FFD] = 0x80
    rom[0x7FDC:0x7FE0] = bytes([0xFF, 0xFF, 0x00, 0x00])
    s = sum(rom) & 0xFFFF
    rom[0x7FDE] = s & 0xFF
    rom[0x7FDF] = s >> 8
    c = s ^ 0xFFFF
    rom[0x7FDC] = c & 0xFF
    rom[0x7FDD] = c >> 8
    return rom


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    for sub, name, data in (("gb", "sramtest.gb", gb_rom()), ("nes", "sramtest.nes", nes_rom()),
                            ("snes", "sramtest.sfc", snes_rom())):
        d = os.path.join(out, sub)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
        print(os.path.join(d, name), len(data), "bytes")


if __name__ == "__main__":
    main()
