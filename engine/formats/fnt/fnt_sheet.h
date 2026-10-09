#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/fnt/fnt.h>
#include <formats/pcx/pcx.h>

namespace opennova::fnt {

// A bitmap font (`.fnt`) made from a glyph sheet, an ordinary picture of the glyphs drawn on a grid
// (the editor's font import, ADR 0046), written by the FNT writer (fnt_write) with the glyphs packed
// onto its 256 x 256 pages in byte order by fnt_pack_shelf.
//
// The sheet: 16 columns by 14 rows of equal cells holding the bytes 0x20..0xFF in reading order, the
// cell of byte b the (b - 0x20)th, as the font's 224 glyph records run from the space [orig:
// GameFont_LoadFromBlob @ 0x674740] (fnt.h). Its sides divide by 16 and by 14, and a cell is at most
// 254 texels tall, a page's 256 less the packer's gutter at each edge. A texel is ink where its alpha is
// above 0, and its pixels are copied into the pages as they are, so the glyphs are drawn on a
// transparent ground; what of their colour the .fnt keeps is `sheet_color`'s.
//
// Every glyph is the cell's full height, one height for the whole font, which keeps a line's baselines
// together. Its width is its advance: the format has no advance table, the game stepping each glyph
// on by its rect's width plus the font's (spacing - 1) [orig: CGameFont_MeasureText @ 0x674e70;
// CGameFont_DrawText @ 0x6752c0] (fnt.h, glyph_spacing). The settings:
// - `advance`, how wide a glyph is: Ink (the cell's inked columns, from its first to its last, then
//   `tracking` clear columns), Left (from the cell's left edge to its last inked column, then
//   `tracking`: the left bearing kept as drawn) or Cell (the whole cell, inked or not: a monospaced
//   font, its space and its empty cells a cell wide too);
// - `tracking` (Ink, Left), the clear columns after a glyph's ink: with a spacing of 0 the game steps a
//   glyph on by its width less one, so 1 sets glyphs edge to edge and 2 leaves a clear texel between them;
// - `space` (Ink, Left), the width of byte 0x20 and of every other cell drawn empty, so an undrawn byte
//   draws as a blank rather than running words together (a 0 x 0 rect steps back a texel), no wider than a
//   cell, 0 for a quarter of the cell's height, rounded. The space's own cell is never read: it draws
//   blank in every mode;
// - `spacing`, the FNT header's glyph_spacing word;
// - `design_width`, the FNT header's +4 word: the game draws the font at 800 / design_width of its
//   texels [orig: GameFont_LoadFromBlob @ 0x674740] (fnt_design_scale);
// - `sheet_color`, what the pages keep of the sheet's colour: false writes every texel white with its
//   alpha kept, the FNT writer's mask, which the text's colour tints; true keeps each texel's own colour
//   (fnt.h keep_page_rgb). The game multiplies a page's colour by the text's (the page's mode 0x651,
//   MODULATE2X(TEXTURE, DIFFUSE) [orig: GameFont_LoadFromBlob @ 0x674740, 0x674830..0x67483B]), so a
//   glyph's dark rim drawn in the sheet stays dark under any text colour.
// The bytes 0x7F, 0x80 and 0x81 get the all-zero record whatever their cells hold: the game neither
// measures nor draws them [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]
// (fnt_byte_is_nonprinting).
//
// Tooling, not a port: the sheet's layout, the advance rules and the packing are authoring policy
// (docs/fonts/fnt-re.md, "Authoring policy: the glyph rect IS the advance"); only the reader's rules
// they are made to are the game's.

// The sheet's grid: the 224 bytes 0x20..0xFF, 16 to a row.
inline constexpr int kFontSheetColumns = 16;
inline constexpr int kFontSheetRows = 14;
// The widest and the tallest glyph a page holds: its side less the packer's gutter at each edge
// (fnt_pack_shelf clamps a larger one, which would cut its texels off).
inline constexpr int kFontSheetGlyphMax = int(FNT_TEXTURE_WIDTH - 2 * FNT_PACK_PAD);

enum class FontSheetAdvance { Ink, Left, Cell };

struct FontSheetSettings {
	FontSheetAdvance advance = FontSheetAdvance::Ink;
	int tracking = 1;
	int space = 0; // 0: a quarter of the cell's height, rounded
	int spacing = 0;
	uint32_t design_width = FNT_DEFAULT_DESIGN_WIDTH;
	bool sheet_color = false; // the pages keep the sheet's colour
};

// The font a sheet makes: the .fnt's bytes, or false with `why` (a sheet whose sides do not divide
// into the grid, a cell too tall for a page, a glyph too wide for one, a space wider than a cell, more
// pages than the format holds), and in `field` the setting to blame where one is ("space",
// "tracking"). `opaque` is set when no texel of the sheet is clear (a sheet drawn on an opaque ground,
// every glyph then a full box).
bool make_font_from_sheet(const RgbaImage &sheet, const FontSheetSettings &settings, std::vector<uint8_t> &out,
                          bool &opaque, std::string &why, std::string &field);

} // namespace opennova::fnt
