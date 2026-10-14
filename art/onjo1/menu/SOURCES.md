# The menu fonts' sources

The fonts OpenNova's own game writes its menus and its HUD's text in (the menus, the objectives, the
F1 help, the end screens, the loading screen, the cinematics): our own glyph sheets, rasterised from the
OFL Barlow family as the HUD fonts are (`../hud/SOURCES.md`). Each font the game loads by name keeps
that name and the original's line height and colour form (a dark rim, or a plain white mask); the
original game's `.fnt` files were measured for those only (in a scratch folder outside the repo), and
nothing of theirs is here.

## What is the source

- **`fonts/<name>.png`**: the font's glyph sheet, 16 x 14 cells holding the bytes 0x20..0xFF in
  Windows-1252, each glyph at its cell's left, the cell's height the font's line. White glyphs with a
  dark one-texel rim (each texel darkened by its neighbours' coverage, a diagonal one at 0.6, to an alpha
  of at most 0.72, the glyph composited white over it: the HUD sheets' form), or for the two loading-screen
  fonts plain white coverage. The face is drawn with its own hinting (FreeType 2.14.3 through Pillow) at
  the largest size, in quarter texels, at which every letter and digit, ascenders and descenders, fits the
  line inside the rim; the rows left over go above the capitals. A glyph that would leave its cell (the
  accented capitals, brackets, superscripts, the cedilla, the double dagger) is drawn at four times the
  size and scaled down about its baseline until it fits, as the original's own small fonts squash their
  accented capitals. `Arial12b` is drawn at four times its size and reduced throughout: the hinted sizes
  step from a 5-texel to a 7-texel capital, and neither leaves room for a descender in a 10-texel line.
  The bytes the game never draws (0x7F, 0x80, 0x81) and the ones Windows-1252 leaves undefined (0x8D,
  0x8F, 0x90, 0x9D) are empty cells, which the importer makes blanks of the space's width.
- **`fonts/<name>.fntset`**: the font set the editor's font importer makes the `.fnt` from (our own
  FNT writer, `engine/formats/fnt`), its import options in its comment: `opennova-project import
  <project> art/onjo1/menu/fonts/<name>.fntset`, the options set on its record (`set_import_options`),
  then the `.fnt` from the project's import cache copied into `assets/` under the same name. Every font
  keeps the fallbacks `spacing 0` and `design_width 800`: the original's spacing word is -3 (0 in
  `Arials18`, `Arial22` and `Impact50`) because its glyph rects carry clear columns, where ours are a
  glyph's ink and its tracking (`docs/fonts/fnt-re.md`, "Authoring policy").

No generator script is kept.

## The files

| Font (in `assets/`) | Face | Line | Sheet cell | Drawn at | Pages | Options | What it is |
|---|---|---|---|---|---|---|---|
| `Arial12b.fnt` | Barlow Semi Condensed Bold | 10 | 11 x 10 | 8.15 px, 4x reduced | 1 | `advance ink`, `tracking 0`, `space 3`, `color sheet` | The smallest bold label font of the HUD's label slots (the menus, the objectives, the F1 help, the end screens). |
| `Arial14n.fnt` | Barlow Semi Condensed Medium | 12 | 15 x 12 | 11.75 px | 1 | `advance ink`, `tracking 0`, `space 4`, `color sheet` | A label font of the same slots. |
| `Arial14b.fnt` | Barlow Semi Condensed Bold | 15 | 17 x 15 | 13.65 px | 1 | `advance ink`, `tracking 0`, `space 4`, `color sheet` | A label font of the same slots. |
| `Arial16n.fnt` | Barlow Semi Condensed Medium | 14 | 17 x 14 | 13.85 px | 1 | `advance ink`, `tracking 0`, `space 4`, `color sheet` | A label font of the same slots. |
| `Arial16b.fnt` | Barlow Semi Condensed Bold | 16 | 20 x 16 | 15.95 px | 1 | `advance ink`, `tracking 0`, `space 5`, `color sheet` | A label font of the same slots. |
| `Impac22b.fnt` | Barlow Condensed Black | 19 | 20 x 19 | 17.6 px | 1 | `advance ink`, `tracking 0`, `space 5`, `color sheet` | The heading font of the same slots. |
| `Impac38b.fnt` | Barlow Condensed Black | 32 | 34 x 32 | 30.75 px | 3 | `advance ink`, `tracking 1`, `space 8`, `color sheet` | The large heading font of the same slots. |
| `Arials18.fnt` | Barlow Regular | 13 | 16 x 13 | 13.9 px | 1 | `advance ink`, `tracking 2`, `space 4`, `color white` | The loading screen's small font, a plain white mask. |
| `Arial22.fnt` | Barlow Regular | 16 | 21 x 16 | 18.3 px | 1 | `advance ink`, `tracking 2`, `space 5`, `color white` | The loading screen's font, a plain white mask. |
| `couri20b.fnt` | Barlow Semi Condensed Bold | 16 | 10 x 16 | 15.95 px | 1 | `advance cell`, `tracking 1`, `color sheet` | The cinematics' monospaced font: each glyph centred in its cell (the digits' advance with the rim), the wider ones (M, W, m, w, @, %, the ligatures) narrowed to it; the cells set edge to edge, 10 texels a character. |
| `onmnu19.fnt` | Barlow Condensed Medium | 19 | 20 x 19 | 18.1 px | 1 | `advance ink`, `tracking 0`, `space 5`, `color sheet` | A menu stylesheet font, at the line of the original's `Gunpl22b`. |
| `onmnub23.fnt` | Barlow Condensed SemiBold | 23 | 26 x 23 | 23.8 px | 2 | `advance ink`, `tracking 1`, `space 6`, `color sheet` | A menu stylesheet font, at the line of the original's `Gunpl27b`. |
| `onttl40.fnt` | Barlow Condensed Black | 40 | 44 x 40 | 40.4 px | 4 | `advance ink`, `tracking 1`, `space 10`, `color sheet` | The menus' title font, at the line of the original's `Impact50`. |

## Third-party material

| Material | Author | Licence | Used for |
|---|---|---|---|
| [Barlow](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlow) Regular, [Barlow Semi Condensed](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlowsemicondensed) Medium and Bold, [Barlow Condensed](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlowcondensed) Medium, SemiBold and Black, v1.408 (google/fonts at `e9678a5385322c63730122ba464765baad83cfd8`) | The Barlow Project Authors (Jeremy Tribby) | SIL Open Font License 1.1 (`fonts/OFL.txt`, the copy google/fonts ships beside each of the three families) | Rasterised into the thirteen glyph sheets |
