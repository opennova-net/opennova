// The font importer: a font set's glyph sheet made into the .fnt the game reads (font_import.h).
// Tooling, not a port: the sheet's grid, the advance rules and the packing are authoring policy; the
// FNT facts it is made to are the reader's (formats/fnt/fnt.h), each cited where it binds.
#include <editor/import/font_import.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <sstream>

#include <base/io/strutil.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <formats/fnt/fnt.h>

namespace opennova::editor {

using namespace opennova::fnt;

namespace {

// The widest and the tallest glyph a page holds: its side less the packer's gutter at each edge
// (fnt_pack_shelf clamps a larger one, which would cut its texels off).
constexpr int kGlyphMax = int(FNT_TEXTURE_WIDTH - 2 * FNT_PACK_PAD);
constexpr int kTrackingMax = 32;
constexpr int kSpacingMax = 16;
constexpr int kDesignWidthMax = 4096;

bool integer_in(const std::string &value, int low, int high, int &out) {
	const std::string text = strutil::trim(value);
	const std::optional<int> parsed = strutil::parse_int(text);
	// The whole text a number: "12px" is no value of an integer option.
	if (!parsed || std::to_string(*parsed) != text || *parsed < low || *parsed > high) return false;
	out = *parsed;
	return true;
}

bool tracking_form(const std::string &value) {
	int n = 0;
	return integer_in(value, 0, kTrackingMax, n);
}

bool space_form(const std::string &value) {
	int n = 0;
	return integer_in(value, 1, kGlyphMax, n);
}

bool spacing_form(const std::string &value) {
	int n = 0;
	return integer_in(value, -kSpacingMax, kSpacingMax, n);
}

bool design_width_form(const std::string &value) {
	int n = 0;
	return integer_in(value, 1, kDesignWidthMax, n);
}

std::string hex_byte(int byte) {
	char text[8];
	std::snprintf(text, sizeof(text), "0x%02X", byte & 0xFF);
	return text;
}

} // namespace

bool parse_font_set(const std::vector<uint8_t> &bytes, FontSet &out, std::string &why) {
	out = FontSet();
	std::istringstream lines(std::string(bytes.begin(), bytes.end()));
	std::string line;
	int number = 0;
	while (std::getline(lines, line)) {
		++number;
		const size_t comment = line.find(';');
		if (comment != std::string::npos) line.resize(comment);
		const std::string text = strutil::trim(line);
		if (text.empty()) continue;
		const size_t gap = text.find_first_of(" \t");
		const std::string key = strutil::to_lower(text.substr(0, gap));
		const std::string value = gap == std::string::npos ? std::string() : strutil::trim(text.substr(gap));
		if (key != "sheet") {
			why = "line " + std::to_string(number) + " names '" + key + "', which a font set does not take (it takes sheet)";
			return false;
		}
		if (value.empty()) {
			why = "line " + std::to_string(number) + "'s sheet names no file";
			return false;
		}
		out.sheet = value;
	}
	if (out.sheet.empty()) {
		why = "a font set names its glyph sheet (a line `sheet <file>`)";
		return false;
	}
	return true;
}

void font_set_inputs(const std::vector<uint8_t> &bytes, std::vector<std::string> &out) {
	out.clear();
	FontSet set;
	std::string why;
	if (parse_font_set(bytes, set, why)) out.push_back(set.sheet);
}

const std::vector<ImportOptionRow> &font_import_option_rows() {
	static const std::vector<ImportOptionRow> rows = [] {
		std::vector<ImportOptionRow> out;
		ImportOptionRow advance;
		advance.key = "advance";
		advance.label = "Advance";
		advance.words = "How wide each glyph is. The font has no advance table: the game steps a glyph on by its width "
		                "and the font's spacing less one.";
		advance.values = {
		        {"ink", "the cell's inked columns, first to last, then the tracking"},
		        {"left", "from the cell's left edge to its last inked column, then the tracking (the left bearing as drawn)"},
		        {"cell", "the whole cell, inked or not (monospaced)"},
		};
		advance.fallback = "ink";
		out.push_back(advance);
		ImportOptionRow tracking;
		tracking.key = "tracking";
		tracking.label = "Tracking";
		tracking.words = "The clear columns after a glyph's ink. With a spacing of 0, 1 sets glyphs edge to edge and 2 "
		                 "leaves a clear texel between them.";
		tracking.forms = {"<0..32>"};
		tracking.accepts = tracking_form;
		tracking.fallback = "1";
		tracking.applies_to = "advance";
		tracking.applies_values = {"ink", "left"};
		out.push_back(tracking);
		ImportOptionRow space;
		space.key = "space";
		space.label = "Space width";
		space.words = "The width of the space (byte 0x20) and of every other cell drawn empty, so an undrawn byte "
		              "draws as a blank. No wider than a cell; left out, a quarter of the cell's height.";
		space.forms = {"<1..254>"};
		space.accepts = space_form;
		space.applies_to = "advance";
		space.applies_values = {"ink", "left"};
		out.push_back(space);
		ImportOptionRow spacing;
		spacing.key = "spacing";
		spacing.label = "Spacing";
		spacing.words = "The font's spacing (the .fnt header's glyph_spacing): the game steps each glyph on by its "
		                "width plus this less one.";
		spacing.forms = {"<-16..16>"};
		spacing.accepts = spacing_form;
		spacing.fallback = "0";
		out.push_back(spacing);
		ImportOptionRow design;
		design.key = "design_width";
		design.label = "Design width";
		design.words = "The screen width the font is drawn for (the .fnt header's design width): the game draws it at "
		               "800 / this of its texels, 800 at their own size.";
		design.forms = {"<1..4096>"};
		design.accepts = design_width_form;
		design.fallback = "800";
		out.push_back(design);
		ImportOptionRow color;
		color.key = "color";
		color.label = "Colour";
		color.words = "What the font's pages hold of the sheet's colour. The game multiplies a page's colour by the "
		              "text's, so a dark texel draws dark whatever the text's colour.";
		color.values = {
		        {"white", "every texel white with its alpha kept, a mask the text's colour tints"},
		        {"sheet", "each texel's own colour kept (a dark rim around a glyph stays dark)"},
		};
		color.fallback = "white";
		out.push_back(color);
		return out;
	}();
	return rows;
}

bool font_import_settings(const ImportOptions &options, FontImportSettings &out, std::string &why, std::string &field) {
	out = FontImportSettings();
	for (const auto &[key, raw] : options) {
		const ImportOptionRow *row = import_option_row(font_import_option_rows(), key);
		field = key;
		if (!row) {
			why = "The font importer has no option '" + key + "'.";
			return false;
		}
		const std::string value = strutil::to_lower(raw);
		if (value.empty()) continue;
		if (!import_option_accepts(*row, value)) {
			why = "The font importer's " + key + " takes " + import_option_takes(*row) + "; '" + raw + "' is none of them.";
			return false;
		}
		int number = 0;
		if (key == "advance")
			out.advance = value == "left" ? FontAdvance::Left : value == "cell" ? FontAdvance::Cell : FontAdvance::Ink;
		else if (key == "tracking" && integer_in(value, 0, kTrackingMax, number)) out.tracking = number;
		else if (key == "space" && integer_in(value, 1, kGlyphMax, number)) out.space = number;
		else if (key == "spacing" && integer_in(value, -kSpacingMax, kSpacingMax, number)) out.spacing = number;
		else if (key == "design_width" && integer_in(value, 1, kDesignWidthMax, number)) out.design_width = uint32_t(number);
		else if (key == "color") out.sheet_color = value == "sheet";
	}
	field.clear();
	return true;
}

bool make_font_from_sheet(const RgbaImage &sheet, const FontImportSettings &settings, std::vector<uint8_t> &out,
                          bool &opaque, std::string &why, std::string &field) {
	out.clear();
	opaque = false;
	field.clear();
	const std::string sides = std::to_string(sheet.width) + " x " + std::to_string(sheet.height);
	if (sheet.empty() || sheet.width % kFontSheetColumns != 0 || sheet.height % kFontSheetRows != 0) {
		why = "the sheet is " + sides + ": its width divides into 16 columns and its height into 14 rows of equal cells";
		return false;
	}
	const int cell_w = sheet.width / kFontSheetColumns;
	const int cell_h = sheet.height / kFontSheetRows;
	if (cell_h > kGlyphMax) {
		why = "a cell of the sheet is " + std::to_string(cell_h) + " texels tall: a glyph is at most " +
		      std::to_string(kGlyphMax) + " (a page's 256 less the packer's gutter at each edge)";
		return false;
	}
	const bool proportional = settings.advance != FontAdvance::Cell;
	const int space = settings.space > 0 ? settings.space : std::max(1, (cell_h + 2) / 4);
	if (proportional && space > cell_w) {
		why = "the space is " + std::to_string(space) + " texels wide and a cell " + std::to_string(cell_w) +
		      ": the space is no wider than a cell";
		field = "space";
		return false;
	}
	const auto alpha = [&](int x, int y) { return sheet.pixels[(size_t(y) * size_t(sheet.width) + size_t(x)) * 4 + 3]; };
	opaque = true;
	for (size_t i = 3; i < sheet.pixels.size() && opaque; i += 4) opaque = sheet.pixels[i] != 0;

	// Each glyph's width, and the columns of its cell copied into it (from `from`, `copy` of them: its
	// tracking, and a blank glyph whole, the page's clear texels).
	struct Glyph {
		int from = 0;
		int copy = 0;
	};
	std::vector<Glyph> glyphs(FNT_GLYPH_COUNT);
	std::vector<fnt_pack_size_t> sizes(FNT_GLYPH_COUNT);
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const int byte = int(FNT_FIRST_CHAR + i);
		sizes[i] = {0, 0};
		if (fnt_byte_is_nonprinting(uint8_t(byte))) continue; // the all-zero record
		const int cell_x = int(i % kFontSheetColumns) * cell_w, cell_y = int(i / kFontSheetColumns) * cell_h;
		int first = -1, last = -1;
		for (int x = 0; x < cell_w; ++x)
			for (int y = 0; y < cell_h; ++y)
				if (alpha(cell_x + x, cell_y + y) != 0) {
					if (first < 0) first = x;
					last = x;
					break;
				}
		Glyph &glyph = glyphs[i];
		int width = 0;
		if (byte == 0x20 || first < 0) {
			// The space (its cell never read: it draws blank), and a cell drawn empty: a blank of the
			// space's width, or the cell's in a monospaced font.
			width = proportional ? space : cell_w;
		} else if (!proportional) {
			glyph = {cell_x, cell_w};
			width = cell_w;
		} else if (settings.advance == FontAdvance::Ink) {
			glyph = {cell_x + first, last - first + 1};
			width = glyph.copy + settings.tracking;
		} else {
			glyph = {cell_x, last + 1};
			width = glyph.copy + settings.tracking;
		}
		if (width > kGlyphMax) {
			// Its tracking's fault where its own columns fit; the sheet's where its cells are wider than a page holds.
			const bool tracked = proportional && glyph.copy <= kGlyphMax;
			why = "byte " + hex_byte(byte) + "'s glyph is " + std::to_string(width) + " texels wide" +
			      (tracked ? " with its tracking" : "") + ": a glyph is at most " + std::to_string(kGlyphMax);
			if (tracked) field = "tracking";
			return false;
		}
		sizes[i] = {uint32_t(width), uint32_t(cell_h)};
	}

	std::vector<fnt_pack_rect_t> rects(FNT_GLYPH_COUNT);
	uint32_t pages = 0;
	const fnt_error_t packed = fnt_pack_shelf(sizes.data(), sizes.size(), rects.data(), &pages);
	if (packed == FNT_ERR_INVALID_PAGE_COUNT) {
		why = "the glyphs of " + std::to_string(cell_h) + "-texel cells take more than the " + std::to_string(FNT_MAX_PAGES) +
		      " pages of 256 x 256 a font holds";
		return false;
	}
	fnt_font_t font{};
	fnt_error_t error = packed;
	if (error == FNT_OK) error = fnt_init_blank(&font, pages, settings.spacing);
	if (error != FNT_OK) {
		why = std::string("the font cannot be made: ") + fnt_error_string(error);
		return false;
	}
	font.design_width = settings.design_width;
	font.keep_page_rgb = settings.sheet_color ? 1 : 0;
	const auto uv = [](uint32_t texels, uint32_t side) { return float(texels) / float(side); };
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		const fnt_pack_rect_t &rect = rects[i];
		if (rect.width == 0) continue;
		const Glyph &glyph = glyphs[i];
		const int cell_y = int(i / kFontSheetColumns) * cell_h;
		uint8_t *page = fnt_get_page_data(&font, rect.page);
		for (int y = 0; y < cell_h; ++y) {
			const uint8_t *from = &sheet.pixels[(size_t(cell_y + y) * size_t(sheet.width) + size_t(glyph.from)) * 4];
			uint8_t *to = page + (size_t(rect.y + uint32_t(y)) * FNT_TEXTURE_WIDTH + rect.x) * FNT_TEXTURE_CHANNELS;
			std::copy(from, from + size_t(glyph.copy) * 4, to);
		}
		fnt_glyph_t &record = font.glyphs[i];
		record.page = rect.page;
		record.uv.u0 = uv(rect.x, FNT_TEXTURE_WIDTH);
		record.uv.v0 = uv(rect.y, FNT_TEXTURE_HEIGHT);
		record.uv.u1 = uv(rect.x + rect.width, FNT_TEXTURE_WIDTH);
		record.uv.v1 = uv(rect.y + rect.height, FNT_TEXTURE_HEIGHT);
	}
	out.assign(fnt_calculate_file_size(font.num_pages), 0);
	size_t written = 0;
	error = fnt_write(&font, out.data(), out.size(), &written);
	fnt_free(&font);
	if (error != FNT_OK || written != out.size()) {
		out.clear();
		why = std::string("the font cannot be written: ") + fnt_error_string(error);
		return false;
	}
	return true;
}

bool run_font_import(ImportContext &context, ImportProduct &out) {
	const std::string &source_name = context.source_name();
	const auto refuse = [&](const std::string &message, CoreFinding code = CoreFinding::ImportFont,
	                        const std::string &field = std::string()) {
		out.outputs.clear();
		out.diagnostics.push_back(make_finding(code, DiagnosticSeverity::Error, message, source_name, field));
		return false;
	};
	FontImportSettings settings;
	std::string why, field;
	if (!font_import_settings(context.options(), settings, why, field)) return refuse(why, CoreFinding::ImportOption, field);
	// One output, `<stem>.fnt`: a name the game's archives hold, of the font's kind.
	const std::string name = utf8_of(path_of(source_name).stem()) + ".fnt";
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	if (!check_file_name(name, AssetKind::Font, problem, message)) return refuse(source_name + " makes " + name + ": " + message);
	FontSet set;
	if (!parse_font_set(context.source(), set, why)) return refuse(source_name + ": " + why + ".");

	// The sheet read through the context: an input, a change to it importing the font again.
	std::vector<uint8_t> bytes;
	if (!context.read(set.sheet, bytes)) return false;
	ImageSource sheet;
	if (!decode_image_source(set.sheet, bytes, sheet, why))
		return refuse(set.sheet + ": " + why + " (a glyph sheet is a PNG, a TGA or a PCX).", CoreFinding::ImportDecode);
	ImportOutput output;
	output.name = name;
	bool opaque = false;
	if (!make_font_from_sheet(sheet.image, settings, output.bytes, opaque, why, field))
		return refuse(set.sheet + ": " + why + ".", field.empty() ? CoreFinding::ImportFont : CoreFinding::ImportOption, field);
	if (opaque)
		out.diagnostics.push_back(make_finding(
		        CoreFinding::ImportFont, DiagnosticSeverity::Warning,
		        set.sheet + " has no clear texel: a glyph's ink is its texels of alpha above 0, so every glyph is a "
		                    "full box. Draw the glyphs on a transparent ground.",
		        source_name));
	out.outputs.push_back(std::move(output));
	return true;
}

} // namespace opennova::editor
