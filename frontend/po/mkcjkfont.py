#!/usr/bin/env python3
"""
mkcjkfont.py - makes the CJK fallback fonts of the text renderer
(third_party/fonts/RSOS-CJK-{JP,SC,KR}.otf) from Noto Sans CJK, keeping only:

  - every character the translations use (ja.po for JP, zh_CN.po for SC,
    ko.po for KR: `rsos-i18n chars`),
  - common characters for game names in other scripts:
      JP: all kana and the 2965 kanji of JIS X 0208 level 1 (the common ones),
      SC: the 3755 hanzi of GB 2312 level 1,
      KR: the 2350 Hangul syllables of KS X 1001,
  - CJK punctuation and fullwidth forms.

The renderer tries them after DejaVu Sans, the one of the UI language first
(src/gfx/font.c, font_setup_dir), so a Han character gets the Japanese or
Chinese glyph shape as the language wants. The fonts are renamed "RetroStone
CJK" (the SIL OFL asks modified versions to change their name); licence:
third_party/fonts/LICENSE-NotoSansCJK.txt.

Run by `make cjk-fonts` (fontTools, and the Noto Sans CJK collection:
Ubuntu's fonts-noto-cjk, or NOTO_CJK_TTC=...), after the translations
changed; `make check-i18n` fails when a translated character is missing.

usage: mkcjkfont.py NOTO_TTC OUT_DIR JA_CHARS ZH_CHARS KO_CHARS
       (the *_CHARS files: `rsos-i18n chars po/<lang>.po` output)
"""
import io
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

# Face indices in NotoSansCJK-Regular.ttc (checked by name below).
FACES = {"JP": (0, "Noto Sans CJK JP"), "KR": (1, "Noto Sans CJK KR"), "SC": (2, "Noto Sans CJK SC")}


def codec_set(codec, rows):
    out = set()
    for r in rows:
        for c in range(0xA1, 0xFF):
            try:
                out.add(bytes([r, c]).decode(codec))
            except UnicodeDecodeError:
                pass
    return out


def read_chars(path):
    with open(path, encoding="utf-8") as f:
        return set(f.read()) - {"\n", "\r"}


def is_cjk(ch):
    c = ord(ch)
    return c >= 0x2E80 or 0x1100 <= c <= 0x11FF or 0x3000 <= c <= 0x303F


def make(ttc, face, chars, out):
    idx, want = FACES[face]
    font = TTFont(ttc, fontNumber=idx)
    got = font["name"].getDebugName(4)
    if got != want:
        sys.exit("mkcjkfont: face %d of %s is %r, expected %r" % (idx, ttc, got, want))
    opts = subset.Options()
    # stb_truetype reads cmap, glyf/CFF and hmtx only: no OpenType layout,
    # no vertical metrics (each feature would keep alternate glyphs)
    opts.layout_features = []
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE", "JSTF", "vhea", "vmtx", "VORG", "DSIG"]
    opts.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    opts.name_languages = [0x409]
    opts.notdef_outline = True
    opts.hinting = False
    opts.desubroutinize = False
    # no Unicode variation sequences (cmap format 14): stb_truetype does not
    # use them, and they would keep every variant glyph of each character
    font["cmap"].tables = [t for t in font["cmap"].tables if t.format != 14]
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(ord(c) for c in chars))
    sub.subset(font)
    family = "RetroStone CJK " + face
    ps = "RetroStoneCJK" + face + "-Regular"
    name = font["name"]
    for rec in list(name.names):
        if rec.nameID in (1, 16):
            rec.string = family
        elif rec.nameID == 3:
            rec.string = "RetroStoneOS subset of Noto Sans CJK " + face + ": " + ps
        elif rec.nameID == 4:
            rec.string = family + " Regular"
        elif rec.nameID == 6:
            rec.string = ps
    if "CFF " in font:
        font["CFF "].cff.fontNames = [ps]
    buf = io.BytesIO()
    font.save(buf)
    data = buf.getvalue()
    with open(out, "wb") as f:
        f.write(data)
    cmap = font.getBestCmap()
    print("mkcjkfont: %s: %d characters, %d glyphs, %d bytes" % (out, len(cmap), len(font.getGlyphOrder()),
                                                                  len(data)))


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    ttc, outdir, ja, zh, ko = sys.argv[1:]
    kana = {chr(c) for c in range(0x3040, 0x3100)} | {chr(c) for c in range(0x31F0, 0x3200)}
    halfwidth = {chr(c) for c in range(0xFF61, 0xFFA0)}
    punct = {chr(c) for c in range(0x3000, 0x3040)} | {chr(c) for c in range(0xFF01, 0xFF61)} | \
        {chr(c) for c in range(0xFFE0, 0xFFEF)}
    jis1 = codec_set("euc_jp", range(0xB0, 0xD0))
    gb1 = codec_set("gb2312", range(0xB0, 0xD8))
    hangul = codec_set("euc_kr", range(0xB0, 0xC9)) | {chr(c) for c in range(0x3131, 0x318F)}
    ja_used = {c for c in read_chars(ja) if is_cjk(c)}
    zh_used = {c for c in read_chars(zh) if is_cjk(c)}
    ko_used = {c for c in read_chars(ko) if is_cjk(c)}
    make(ttc, "JP", kana | halfwidth | punct | jis1 | ja_used, outdir + "/RSOS-CJK-JP.otf")
    make(ttc, "SC", punct | gb1 | zh_used, outdir + "/RSOS-CJK-SC.otf")
    make(ttc, "KR", punct | hangul | ko_used, outdir + "/RSOS-CJK-KR.otf")


if __name__ == "__main__":
    main()
