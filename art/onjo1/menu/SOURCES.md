# The menu fonts' sources

The fonts OpenNova's own game writes its menus and its HUD's text in (the menus, the objectives, the
F1 help, the end screens, the loading screen, the cinematics): our own glyph sheets, rasterised from the
OFL Barlow family as the HUD fonts are (`../hud/SOURCES.md`). Each font the game loads by name keeps
that name and the original's line height and colour form (a dark rim, or a plain white mask); the
original game's `.fnt` files were measured for those only (in a scratch folder outside the repo), and
nothing of theirs is here.

## What is the source

- **`fonts/<name>.png`**: the font's glyph sheet, 16 x 14 cells holding the bytes 0x20..0xFF in
  Windows-1252, the cell's height the font's line. Each glyph is drawn with its pen origin one texel in
  from its cell's left (the rim's column; a mask's at the edge), so it keeps the typeface's left bearing;
  the few drawn left of their origin (`j`'s hook, `(` in the lighter faces) are moved right until their
  rim is at the edge and step on that much further. A glyph's rect runs from its cell's left to its
  advance (the face's hinted advance in whole texels, plus one in `Arial12b`, `Impac22b`, `Impac38b` and
  `onttl40`, whose letters otherwise touch) plus the font's pad, the room its widest overhang needs (an
  arm, a rim) past the next glyph's pen: its last column is marked, where the ink stops short of it, by one
  black texel of alpha 1, which the importer reads as ink and the page's blend draws as nothing. The
  pad is paid back by the font's negative spacing word, as the original's own fonts pay back theirs, so
  the game steps each glyph by the typeface's advance and an arm or a rim may overhang its neighbour.
  White glyphs with a dark one-texel rim (each texel darkened by its neighbours' coverage, a diagonal
  one at 0.6, to an alpha of at most 0.72, the glyph composited white over it: the HUD sheets' form), or
  for the two loading-screen fonts plain white coverage. The face is drawn with its own hinting (FreeType 2.14.3 through Pillow) at
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
  then the `.fnt` from the project's import cache copied into `assets/` under the same name. The
  proportional fonts import with `advance left` (a rect from the cell's left to its last texel of
  alpha above 0: the mark), `tracking 0`, the pad as `spacing` (1 minus the pad) and the space as its
  hinted advance plus the pad, give or take a texel or two so that the clear columns between two words
  come to the original font's of the same line or up to two texels more (measured, as the gaps between
  letters were, by drawing both fonts as the game steps them); every font keeps `design_width 800`.
  The original's spacing word is -3 (0 in `Arials18`, `Arial22` and `Impact50`) for the same reason:
  its glyph rects carry clear columns (`docs/fonts/fnt-re.md`, "Authoring policy"). `couri20b` imports
  with `advance cell` and `tracking 0`.

No generator script is kept.

## The files

| Font (in `assets/`) | Face | Line | Sheet cell | Drawn at | Pages | Options | What it is |
|---|---|---|---|---|---|---|---|
| `Arial12b.fnt` | Barlow Semi Condensed Bold | 10 | 12 x 10 | 8.15 px, 4x reduced | 1 | `advance left`, `tracking 0`, `space 5`, `spacing -1`, `color sheet` | The smallest bold label font of the HUD's label slots (the menus, the objectives, the F1 help, the end screens). |
| `Arial14n.fnt` | Barlow Semi Condensed Medium | 12 | 16 x 12 | 11.75 px | 1 | `advance left`, `tracking 0`, `space 6`, `spacing -2`, `color sheet` | A label font of the same slots. |
| `Arial14b.fnt` | Barlow Semi Condensed Bold | 15 | 18 x 15 | 13.65 px | 1 | `advance left`, `tracking 0`, `space 7`, `spacing -2`, `color sheet` | A label font of the same slots. |
| `Arial16n.fnt` | Barlow Semi Condensed Medium | 14 | 18 x 14 | 13.85 px | 1 | `advance left`, `tracking 0`, `space 7`, `spacing -2`, `color sheet` | A label font of the same slots. |
| `Arial16b.fnt` | Barlow Semi Condensed Bold | 16 | 21 x 16 | 15.95 px | 1 | `advance left`, `tracking 0`, `space 8`, `spacing -3`, `color sheet` | A label font of the same slots. |
| `Impac22b.fnt` | Barlow Condensed Black | 19 | 22 x 19 | 17.6 px | 1 | `advance left`, `tracking 0`, `space 7`, `spacing -2`, `color sheet` | The heading font of the same slots. |
| `Impac38b.fnt` | Barlow Condensed Black | 32 | 38 x 32 | 30.75 px | 3 | `advance left`, `tracking 0`, `space 11`, `spacing -4`, `color sheet` | The large heading font of the same slots. |
| `Arials18.fnt` | Barlow Regular | 13 | 17 x 13 | 13.9 px | 1 | `advance left`, `tracking 0`, `space 5`, `spacing 0`, `color white` | The loading screen's small font, a plain white mask. |
| `Arial22.fnt` | Barlow Regular | 16 | 22 x 16 | 18.3 px | 1 | `advance left`, `tracking 0`, `space 6`, `spacing 0`, `color white` | The loading screen's font, a plain white mask. |
| `couri20b.fnt` | Barlow Semi Condensed Bold | 16 | 9 x 16 | 15.95 px | 1 | `advance cell`, `tracking 0`, `color sheet` | The cinematics' monospaced font: each glyph centred in its cell, the wider ones (M, W, m, w, @, %, the ligatures) narrowed to it; 8 texels a character (the original's `Couri20b` step), neighbours' rims sharing a column. |
| `onmnu19.fnt` | Barlow Condensed Medium | 19 | 22 x 19 | 18.1 px | 1 | `advance left`, `tracking 0`, `space 9`, `spacing -3`, `color sheet` | A menu stylesheet font, at the line of the original's `Gunpl22b`. |
| `onmnub23.fnt` | Barlow Condensed SemiBold | 23 | 28 x 23 | 23.8 px | 2 | `advance left`, `tracking 0`, `space 11`, `spacing -3`, `color sheet` | A menu stylesheet font, at the line of the original's `Gunpl27b`. |
| `onttl40.fnt` | Barlow Condensed Black | 40 | 47 x 40 | 40.4 px | 4 | `advance left`, `tracking 0`, `space 12`, `spacing -4`, `color sheet` | The menus' title font, at the line of the original's `Impact50`. |

## Third-party material

| Material | Author | Licence | Used for |
|---|---|---|---|
| [Barlow](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlow) Regular, [Barlow Semi Condensed](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlowsemicondensed) Medium and Bold, [Barlow Condensed](https://github.com/google/fonts/tree/e9678a5385322c63730122ba464765baad83cfd8/ofl/barlowcondensed) Medium, SemiBold and Black, v1.408 (google/fonts at `e9678a5385322c63730122ba464765baad83cfd8`) | The Barlow Project Authors (Jeremy Tribby) | SIL Open Font License 1.1 (`fonts/OFL.txt`, the copy google/fonts ships beside each of the three families) | Rasterised into the thirteen glyph sheets |

## The background

| File | Size | Made from | What it is |
|---|---|---|---|
| `src/onmnbg.png` | 1600 x 1200 | the scene `onmnbg` of `art/onjo1/screens/screens.blend`, rendered (that folder's `SOURCES.md`) | The main menu's background master: the camp at dusk in the rain, our soldier with his carbine before the guard tower and the hut's lit door, palms over it all. The left third is kept dark and calm for the menu's buttons (a graduated filter before the lens darkens it), and the only text is a small `OPENNOVA` mark top right: the menu draws its own title. |
| `assets/onmnbg.tga` | 800 x 600 | `src/onmnbg.png` | The file the game reads: an uncompressed 24-bit TGA, bottom row first, no image ID. The editor's image importer makes it with `format tga24` and `size 800x600` (the master area-averaged down), in a scratch project as the HUD's textures are made (`../hud/SOURCES.md`): `opennova-project import <scratch> art/onjo1/menu/src/onmnbg.png`, those options set on its record (`set_import_options`), then the `.tga` from the project's import cache copied into `assets/`. It is a plain git blob, not LFS (`.gitattributes`), so the bundled-menu tests read it without an LFS pull. |
