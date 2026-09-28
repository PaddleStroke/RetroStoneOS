# Vendored third-party code and data (frontend UI)

Everything here is copied unmodified from upstream, except the CJK font
subsets (below). Only permissive licences (public domain, MIT, zlib,
Bitstream Vera/DejaVu, Apache-2.0 and the SIL Open Font License for fonts)
are allowed. The UI compiles the libraries in exactly one translation unit,
`src/gfx/third_party_impl.c` (stb_image_write only in `src/tools/uipreview.c`).

| Component | Version / source | Licence | Used for | Files |
|---|---|---|---|---|
| stb_image | v2.30, github.com/nothings/stb (master `2c980bb`, 2026-09) | Public domain (Unlicense) **or** MIT, at your choice (text at the end of the header) | PNG / JPEG / GIF / BMP / TGA decoding of theme and game images | `stb/stb_image.h` |
| stb_truetype | v1.26, same repository | Public domain or MIT | TrueType/OpenType (CFF) glyph rasterization | `stb/stb_truetype.h` |
| stb_image_write | v1.16, same repository | Public domain or MIT | PNG screenshots of the host preview tool only (not in the firmware UI path) | `stb/stb_image_write.h` |
| nanosvg + nanosvgrast | github.com/memononen/nanosvg (master `239e102`, 2026-09) | zlib | SVG parsing and rasterization (ES themes use SVG logos and art) | `nanosvg/nanosvg.h`, `nanosvg/nanosvgrast.h`, `nanosvg/LICENSE.txt` |
| DejaVu Sans, DejaVu Sans Condensed Bold | 2.37, github.com/dejavu-fonts/dejavu-fonts release `version_2_37` | Bitstream Vera + Arev fonts licence (permissive, redistribution allowed, renamed modified versions only) | Default UI font (wide Unicode coverage, glyph fallback for theme fonts), bold UI text | `fonts/DejaVuSans.ttf`, `fonts/DejaVuSansCondensed-Bold.ttf`, `fonts/LICENSE-DejaVu.txt` |
| Roboto Condensed Regular, Bold, Light | 2.138, github.com/googlefonts/roboto release `v2.138` (`roboto-unhinted.zip`) | Apache License 2.0 | Typography of the rsos-dark / rsos-light themes; substitute for unreadable theme fonts (gbz35 ships a PostScript Type 1 file named `RobotoCondensed-Regular.ttf`, which stb_truetype cannot read) | `fonts/RobotoCondensed-*.ttf`, `fonts/LICENSE-Roboto.txt` |
| RetroStone CJK JP / SC / KR (subsets of Noto Sans CJK Regular) | Noto Sans CJK 2.004, github.com/notofonts/noto-cjk (Ubuntu fonts-noto-cjk 1:20230817); subset and renamed by `po/mkcjkfont.py` (`make cjk-fonts`) | SIL Open Font License 1.1 | Japanese, Chinese and Korean text: the per-glyph fallback chain after DejaVu Sans (translations, game names); only the characters of the translations plus common ones: kana and the 2965 kanji of JIS X 0208 level 1 (JP, 680 KB), the 3755 hanzi of GB 2312 level 1 (SC, 810 KB), the 2350 Hangul syllables of KS X 1001 (KR, 273 KB); no hinting, no OpenType layout or variation-sequence tables (stb_truetype uses neither) | `fonts/RSOS-CJK-JP.otf`, `fonts/RSOS-CJK-SC.otf`, `fonts/RSOS-CJK-KR.otf`, `fonts/LICENSE-NotoSansCJK.txt` |
| SDL GameControllerDB | github.com/mdqinc/SDL_GameControllerDB (master `c6d6e7e`, 2026-09); only the `platform:Linux` lines are kept (746 of 2290 lines) | zlib | Automatic mapping of USB/Bluetooth gamepads by SDL GUID, parsed by `src/input/input.c` (no SDL) | `sdl-gamecontrollerdb/gamecontrollerdb.txt`, `sdl-gamecontrollerdb/LICENSE` |

Also in this directory, vendored by the libretro host (not used by the UI;
listed so the table is complete, see the host's documentation for versions):

| Component | Licence | Files |
|---|---|---|
| libretro.h (libretro API header) | MIT | `libretro/libretro.h` |
| miniz (zip/deflate) | MIT | `miniz/miniz.c`, `miniz/miniz.h`, `miniz/LICENSE` |

And by the system updater (`src/update`, `rsos-update` and the build machine tool `rsos-mkupdate`, docs/updates.md):

| Component | Version / source | Licence | Used for | Files |
|---|---|---|---|---|
| Monocypher (+ its optional Ed25519/SHA-512 part) | 4.0.2, monocypher.org/download/monocypher-4.0.2.tar.gz (the same sources as the GitHub tag `4.0.2`, except the version line); unmodified. Audited by Cure53 (2020) | CC0-1.0 **or** BSD-2-Clause, at your choice (`LICENCE.md`) | Ed25519 signatures of the update packages (signify format), verified on the device and made by `rsos-mkupdate` | `monocypher/monocypher.c`, `monocypher/monocypher.h`, `monocypher/monocypher-ed25519.c`, `monocypher/monocypher-ed25519.h`, `monocypher/LICENCE.md` |

## Install locations on the device

| Repository path | Target path |
|---|---|
| `third_party/fonts/*.ttf`, `*.otf` | `/usr/share/rsos/fonts/` (the `:/fonts/...` prefix in themes) |
| `third_party/sdl-gamecontrollerdb/gamecontrollerdb.txt` | `/usr/share/rsos/gamecontrollerdb.txt` |
| `themes/rsos-dark`, `themes/rsos-light`, `themes/gbz35`, `themes/gbz35-dark` | `/usr/share/rsos/themes/` (the whole `themes/` folder, with each theme's `LICENSE.txt`) |
| `src/input/remaps/*.ini` | `/usr/share/rsos/remaps/` |

The licence files must be installed next to the fonts and the database (or
under `/usr/share/licenses/`), as their licences ask.

## Themes (in `../themes/`, not in this directory)

The permissive-only rule above is for code and fonts. Theme art is different:
by the owner's decision (2026-09-27; RetroStoneOS is free and open source
with public builds, and the console is sold without firmware), Creative
Commons BY-NC-SA theme material may ship in the free firmware image, under
its terms (attribution, non-commercial, share-alike). Each theme folder
carries its own `LICENSE.txt`, installed with it.

| Theme folder | Origin | Licence | Changes |
|---|---|---|---|
| `rsos-dark`, `rsos-light` | RetroStoneOS | project licence, except `_art/carbon/` | |
| `rsos-*/_art/carbon/*.svg` | "carbon" theme by Rookervik (based on "simple" by Nils Bonenberger), github.com/RetroPie/es-theme-carbon commit `b09973e`, `<system>/art/controller.svg`; Carbon's `readme.txt` copied as `CARBON-README.txt` | CC BY-NC-SA (readme: "2.0" summary, 4.0 International legal text) | outline stroke added (thicker lines), editor metadata removed |
| `gbz35` | Gameboy Zero 3.5" theme by Brad Miller (rxbrad), github.com/rxbrad/es-theme-gbz35 commit `300c4b6` (repo-root zip); based on Carbon (Rookervik), Spare (Matt Kennedy), SimpleBigArt (Ewzzy) | CC BY-NC-SA 3.0 | none (`.gitattributes` dropped, `LICENSE.txt` added) |
| `gbz35-dark` | the dark version, github.com/rxbrad/es-theme-gbz35-dark commit `aefbbb2` (zip) | CC BY-NC-SA 3.0 | none (same) |

gbz35's system logos (`*/system.svg`) are trademarks of their owners.
Other ES themes tried for compatibility (simple, cosmos-ropi, gamehistoria,
hyperion...) are not in the repository: see docs/ui-design.md §13.

## Updating

Replace the files with newer upstream copies and record the commit here. The
gamecontrollerdb file is regenerated with:

    { grep '^#' gamecontrollerdb.txt | head -5; grep 'platform:Linux' gamecontrollerdb.txt; } > sdl-gamecontrollerdb/gamecontrollerdb.txt
