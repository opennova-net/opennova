#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <formats/pcx/pcx.h>

namespace opennova::editor {

// The font importer: a bitmap font the menus and the HUD write with (`.fnt`), made from a glyph sheet,
// an ordinary picture of the glyphs drawn on a grid. Its source is a font set (`<stem>.fntset`, a short
// text a modder writes beside the sheet) naming the sheet, which the import reads through its context,
// so a change to the sheet imports the font again; an import of the set from the disk brings the sheet
// beside it (Importer::names_inputs). Its record's options are the font's metrics. The output is one
// file, `<stem>.fnt`, written by the FNT writer (formats/fnt fnt_write), the glyphs packed onto its
// 256 x 256 pages in byte order by fnt_pack_shelf.
//
// The set file: a line a key and its value, `;` to the line's end a comment. One key, `sheet`
// (required): the sheet's file, relative to the set's folder, a PNG (a TGA or a PCX reads too). A key
// the set does not know is refused.
//
// The sheet: 16 columns by 14 rows of equal cells holding the bytes 0x20..0xFF in reading order, the
// cell of byte b the (b - 0x20)th, as the font's 224 glyph records run from the space [orig:
// GameFont_LoadFromBlob @ 0x674740] (fnt.h). Its sides divide by 16 and by 14, and a cell is at most
// 254 texels tall, a page's 256 less the packer's gutter at each edge. A texel is ink where its alpha is
// above 0, and its pixels are copied into the pages as they are, so the glyphs are drawn on a
// transparent ground; what of their colour the .fnt keeps is the `color` option's.
//
// Every glyph is the cell's full height, one height for the whole font, which keeps a line's baselines
// together. Its width is its advance: the format has no advance table, the game stepping each glyph
// on by its rect's width plus the font's (spacing - 1) [orig: CGameFont_MeasureText @ 0x674e70;
// CGameFont_DrawText @ 0x6752c0] (fnt.h, glyph_spacing). The record's options (each a row of
// font_import_option_rows):
// - `advance`, how wide a glyph is: `ink` (the fallback: the cell's inked columns, from its first to
//   its last, then `tracking` clear columns), `left` (from the cell's left edge to its last inked
//   column, then `tracking`: the left bearing kept as drawn) or `cell` (the whole cell, inked or not:
//   a monospaced font, its space and its empty cells a cell wide too);
// - `tracking` (ink, left), the clear columns after a glyph's ink, 0..32, fallback 1: with a spacing
//   of 0 the game steps a glyph on by its width less one, so 1 sets glyphs edge to edge and 2 leaves a
//   clear texel between them;
// - `space` (ink, left), the width of byte 0x20 and of every other cell drawn empty, so an undrawn
//   byte draws as a blank rather than running words together (a 0 x 0 rect steps back a texel), 1..254
//   and no wider than a cell, fallback a quarter of the cell's height, rounded. The space's own cell is
//   never read: it draws blank in every mode;
// - `spacing`, the FNT header's glyph_spacing word, -16..16, fallback 0;
// - `design_width`, the FNT header's +4 word, 1..4096, fallback 800: the game draws the font at
//   800 / design_width of its texels [orig: GameFont_LoadFromBlob @ 0x674740] (fnt_design_scale);
// - `color`, what the pages keep of the sheet's colour: `white` (the fallback: every texel white with
//   its alpha kept, the FNT writer's mask, which the text's colour tints) or `sheet` (each texel's own
//   colour, fnt.h keep_page_rgb). The game multiplies a page's colour by the text's (the page's mode
//   0x651, MODULATE2X(TEXTURE, DIFFUSE) [orig: GameFont_LoadFromBlob @ 0x674740, 0x674830..0x67483B]),
//   so a glyph's dark rim drawn in the sheet stays dark under any text colour.
// The bytes 0x7F, 0x80 and 0x81 get the all-zero record whatever their cells hold: the game neither
// measures nor draws them [orig: CGameFont_MeasureText @ 0x674e70; CGameFont_DrawText @ 0x6752c0]
// (fnt_byte_is_nonprinting).
//
// Tooling, not a port: the sheet's layout, the advance rules and the packing are authoring policy
// (docs/fonts/fnt-re.md, "Authoring policy: the glyph rect IS the advance"); only the reader's rules
// they are made to are the game's.

inline constexpr int kFontImporterVersion = 1;
inline constexpr const char *kFontSetExtension = ".fntset";
// The sheet's grid: the 224 bytes 0x20..0xFF, 16 to a row.
inline constexpr int kFontSheetColumns = 16;
inline constexpr int kFontSheetRows = 14;

struct FontSet {
	std::string sheet;
};

// The set's text read; false, with `why`, for a line of a key the set does not take, a key with no
// value, or a set without its sheet.
bool parse_font_set(const std::vector<uint8_t> &bytes, FontSet &out, std::string &why);
// The files an import of the set reads besides it (its sheet), as the set names them: the importer
// row's names_inputs. None for a set that does not read.
void font_set_inputs(const std::vector<uint8_t> &bytes, std::vector<std::string> &out);

const std::vector<ImportOptionRow> &font_import_option_rows();

// The options as an import reads them, each left out its fallback.
enum class FontAdvance { Ink, Left, Cell };
struct FontImportSettings {
	FontAdvance advance = FontAdvance::Ink;
	int tracking = 1;
	int space = 0; // 0: a quarter of the cell's height, rounded
	int spacing = 0;
	uint32_t design_width = 800;
	bool sheet_color = false; // `color sheet`: the pages keep the sheet's colour
};
// False, with `why` and the option's key in `field`, for a key no row has or a value its row does not
// take.
bool font_import_settings(const ImportOptions &options, FontImportSettings &out, std::string &why, std::string &field);

// The font a sheet makes: the .fnt's bytes, or false with `why` (a sheet whose sides do not divide
// into the grid, a cell too tall for a page, a glyph too wide for one, a space wider than a cell, more
// pages than the format holds), and in `field` the option to blame where one is (space, tracking).
// `opaque` is set when no texel of the sheet is clear (a sheet drawn on an opaque ground, every glyph
// then a full box).
bool make_font_from_sheet(const RgbaImage &sheet, const FontImportSettings &settings, std::vector<uint8_t> &out,
                          bool &opaque, std::string &why, std::string &field);

bool run_font_import(ImportContext &context, ImportProduct &out);

} // namespace opennova::editor
