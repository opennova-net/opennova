#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <formats/fnt/fnt_sheet.h>

namespace opennova::editor {

// The font importer: a bitmap font the menus and the HUD write with (`.fnt`), made from a glyph sheet,
// an ordinary picture of the glyphs drawn on a grid. Its source is a font set (`<stem>.fntset`, a short
// text a modder writes beside the sheet) naming the sheet, which the import reads through its context,
// so a change to the sheet imports the font again; an import of the set from the disk brings the sheet
// beside it (Importer::names_inputs). Its record's options are the font's metrics. The output is one
// file, `<stem>.fnt`, written by the FNT writer (formats/fnt fnt_write), the glyphs packed onto its
// 256 x 256 pages in byte order by fnt_pack_shelf.
//
// The set file: a line a key and its value, `;` to the line's end a comment. `sheet` (required): the
// sheet's file, relative to the set's folder, a PNG (a TGA or a PCX reads too). The sheet's grid, each
// optional: `columns` and `rows` (1..256 each, 16 and 14 when left out) and `first`, the byte of its first
// cell (0..255, decimal or 0x hex, 0x20 when left out): a sheet of fewer rows from the space, a full
// 256-byte grid from 0, a grid from a later byte (fnt_sheet.h). A key the set does not know is refused.
//
// The sheet, the advance rules and what the font keeps of its colour are formats/fnt fnt_sheet.h's
// (make_font_from_sheet, which makes the .fnt). The record's options are its settings (each a row of
// font_import_option_rows):
// - `advance`: `ink` (the fallback), `left` or `cell` (FontSheetAdvance);
// - `tracking`, the clear columns after a glyph's ink (ink, left) or its cell (cell), 0..32, fallback 1;
// - `space` (ink, left), the width of byte 0x20 and of every other cell drawn empty, 1..254 and no
//   wider than a cell, fallback a quarter of the cell's height, rounded;
// - `spacing`, the FNT header's glyph_spacing word, -16..16, fallback 0;
// - `design_width`, the FNT header's +4 word, 1..4096, fallback 800;
// - `color`: `white` (the fallback: every texel white with its alpha kept) or `sheet` (each texel's own
//   colour).
//
// Tooling, not a port: the option ranges are authoring policy, as fnt_sheet.h's rules are.

inline constexpr int kFontImporterVersion = 1;
inline constexpr const char *kFontSetExtension = ".fntset";

struct FontSet {
	std::string sheet;
	int columns = fnt::kFontSheetColumns;
	int rows = fnt::kFontSheetRows;
	int first = fnt::kFontSheetFirst;
};

// The set's text read; false, with `why`, for a line of a key the set does not take, a key with no
// value, a grid value out of its range, or a set without its sheet.
bool parse_font_set(const std::vector<uint8_t> &bytes, FontSet &out, std::string &why);
// The files an import of the set reads besides it (its sheet), as the set names them: the importer
// row's names_inputs. None for a set that does not read.
void font_set_inputs(const std::vector<uint8_t> &bytes, std::vector<std::string> &out);

const std::vector<ImportOptionRow> &font_import_option_rows();

// The options as an import reads them (fnt_sheet.h's settings), each left out its fallback. False,
// with `why` and the option's key in `field`, for a key no row has or a value its row does not take.
bool font_import_settings(const ImportOptions &options, fnt::FontSheetSettings &out, std::string &why,
                          std::string &field);

bool run_font_import(ImportContext &context, ImportProduct &out);

} // namespace opennova::editor
