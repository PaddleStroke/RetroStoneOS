# Translating RetroStoneOS

RetroStoneOS shows its menus, dialogs, in-game menu and messages in 22 languages. **Every translation except
English was made by an AI (Claude) and has not been reviewed by native speakers yet: your review is very welcome.**
The header of each `.po` file says so ("Translated by AI, review welcome"); once a native speaker has reviewed a file,
replace that line with their name.

| Code | Language | Plural forms | Notes |
|---|---|---|---|
| `en` | English | 2 | the source text (no catalog) |
| `fr` | Français | 2 (0 and 1 singular) | the owner's language: made with the most care; "tu" |
| `es` | Español | 2 | |
| `de` | Deutsch | 2 | |
| `it` | Italiano | 2 | |
| `pt_BR` | Português (Brasil) | 2 (0 and 1 singular) | |
| `pt_PT` | Português (Portugal) | 2 | distinct from pt_BR (ecrã, ficheiro, guardar...) |
| `nl` | Nederlands | 2 | |
| `pl` | Polski | 3 | |
| `sv` | Svenska | 2 | |
| `da` | Dansk | 2 | |
| `nb` | Norsk bokmål | 2 | |
| `fi` | Suomi | 2 | |
| `cs` | Čeština | 3 | |
| `tr` | Türkçe | 2 | uppercase uses İ/I |
| `el` | Ελληνικά | 2 | uppercase drops the accents (automatic) |
| `ru` | Русский | 3 | |
| `uk` | Українська | 3 | |
| `ja` | 日本語 | 1 | CJK font subset |
| `zh_CN` | 简体中文 | 1 | CJK font subset |
| `ko` | 한국어 | 1 | CJK font subset |
| `id` | Bahasa Indonesia | 1 | |

**Not supported: right-to-left scripts (Arabic, Hebrew, Persian).** They need text shaping (joined letter forms)
and the bidirectional algorithm, which the text renderer (stb_truetype, one glyph after the other) does not do. Thai,
Devanagari and other scripts that need shaping are out too. Adding them means a shaping engine (HarfBuzz + FriBidi,
~1-2 MB): not planned.

## How it works (for developers)

- **Marking** (`frontend/src/i18n/i18n.h`): `_("English")` returns the translation, `N_("...")` marks a string in a
  static table (translated later with `_()` where it is shown), `C_("context", "...")` when one English text needs
  different translations, `_n("%d game", "%d games", n)` for plurals. A `/* TRANSLATORS: ... */` comment right
  before a marked string (at most two lines above) goes into the `.pot` for translators: say where the text appears
  and how much room it has. Log lines are never translated.
- **Catalogs**: `frontend/po/<code>.po` (standard gettext PO files), compiled at build time by `rsos-i18n`
  (`frontend/src/tools/rsos-i18n.c`, no dependency) into `<code>.cat`, a small hashed binary installed in
  `/usr/share/rsos/locale/` and mmap()ed. A missing translation shows the English. The compiler **refuses a
  translation whose printf conversions differ from the English** (wrong type, extra or missing argument): the
  English is shown instead and `make check-i18n` fails, so a bad translation can never crash the console.
- **The language**: `language = <code>` in `/data/rsos/settings.ini`. With none (first boot, or an update from a
  version without translations), a picker comes before the menu. Settings > Language changes it at once (no
  restart). The game process gets it with `--lang` and answers `RETRO_ENVIRONMENT_GET_LANGUAGE` with it (cores that
  have their own translations use it).
- **Fonts**: the theme's font, then DejaVu Sans (Latin, Greek, Cyrillic), then three CJK subsets (Japanese,
  Chinese, Korean glyph shapes, the one of the language first) for each character (`src/gfx/font.c`).
- **Locale conventions** are translations too: the decimal and thousands separators (contexts `decimal separator`,
  `thousands separator`), the size units (context `unit`: "Go" in French), and the date formats (context
  `strftime`, numbers only).

### Make targets (in `frontend/`, WSL)

| Target | What it does |
|---|---|
| `make pot` | extracts every marked string into `po/rsos.pot` (commit it with the code change) |
| `make po-update` | merges `po/rsos.pot` into every `po/<code>.po` (gettext's `msgmerge`) |
| `make po-stats` | translated / fuzzy / missing counts per language |
| `make catalogs` | compiles the `.po` files (also part of `make`) |
| `make cjk-fonts` | rebuilds the CJK font subsets from the `ja`, `zh_CN`, `ko` translations (python3-fonttools, fonts-noto-cjk) |
| `make check-i18n` | every marked string is in the `.pot`, every `.po` compiles without error, every translated character is in the fonts, unit tests, the language picker and the live switch (part of `make check`) |

## Improving a translation

1. Edit `frontend/po/<code>.po` with any text editor, or with [Poedit](https://poedit.net) (free; open the `.po`
   file, it shows the English, the comments and the length hints), or through a Weblate instance if the project
   sets one up (Weblate reads the same files).
2. Rules:
   - Keep `%d`, `%s`, `%02d`, `%llu`... exactly (same number, same kind). To change the word order, number them:
     `%2$s ... %1$d` (then number all of them in that text). A percent sign is written `%%`.
   - Keep `\n` line breaks.
   - Keep it **short and simple**: the screen is 640x480 and the players are often children. The `#.` comment above
     each entry says where the text appears and how much room it has (menu rows, dialog buttons, the help bar,
     theme labels of ~7 characters...). Long texts are cut with "...".
   - Dialog buttons are UPPERCASE.
   - Be consistent: a **save state** (a snapshot of the game at any moment, from the in-game menu, in numbered
     slots) is not a **save** (the game's own save). The **core** is the emulator.
   - Console names (context `system`): leave empty unless your country uses another name (e.g. Japanese
     スーパーファミコン for the Super Nintendo).
3. Check your file (WSL, in `frontend/`): `make po-stats` and `make check-i18n`. If you only have Windows,
   Poedit's validation (Catalog > Validate) catches most mistakes too.
4. Try it: `make uipreview catalogs` then
   `$BUILDDIR/ui/rsos-uipreview --root ~/rsos/ui-test --locale $BUILDDIR/locale --lang <code> --keys "start shot:/tmp/menu.png"`,
   or on the console: Settings > Language.
5. Japanese, Chinese or Korean: a character that is in no font shows as "?": run `make cjk-fonts` (it adds the
   characters of the translations to the font subsets), and commit the three `.otf` files.
6. Send the `.po` file (a pull request, or to the project owner). Replace "Translated by AI, review welcome" in its
   header with "Reviewed by <your name>".

## Adding a language

1. Add it to the table in `frontend/src/i18n/i18n.c` (code, name in the language itself, English name,
   `RETRO_LANGUAGE_*` value from `libretro.h`, or 0), in alphabetical order of the native name.
2. `msginit -i po/rsos.pot -l <code> -o po/<code>.po --no-translator` (or copy an existing file and empty the
   `msgstr`), set the `Plural-Forms` line from the
   [gettext table](https://www.gnu.org/software/gettext/manual/html_node/Plural-forms.html), translate.
3. `make check-i18n`: the unit test checks that every language of the table has a catalog.
4. Right-to-left and shaped scripts cannot be added (see above).

## What is not translated

- Theme texts: EmulationStation themes carry their own literal texts (labels, headers) in their XML: they stay as
  the theme's author wrote them. The built-in `rsos-dark` / `rsos-light` themes are ours: their few labels and
  taglines are translated (context `theme`).
- Game names, system names (except regional names, see above), core names, core options (labels and values come
  from the cores themselves), controller names, file names and paths.
- Messages from the operating system inside some errors (e.g. "No space left on device").
- The web page of "Transfer over network" (opened on a computer or phone), the benchmark report file, the logs.
- The two messages under the boot logo while the SD card is prepared or checked are translated only when the
  language is already known (`/data` mounted); the very first boot has no language yet.
