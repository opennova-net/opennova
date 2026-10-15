// The font importer: a font set's glyph sheet made into the .fnt the game reads (font_import.h), by
// formats/fnt fnt_sheet.h's make_font_from_sheet. Tooling, not a port: the option ranges are authoring
// policy.
#include <editor/import/font_import.h>

#include <optional>
#include <sstream>

#include <base/io/strutil.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <formats/fnt/fnt_sheet.h>

namespace opennova::editor {

using namespace opennova::fnt;

namespace {

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
	return integer_in(value, 1, kFontSheetGlyphMax, n);
}

bool spacing_form(const std::string &value) {
	int n = 0;
	return integer_in(value, -kSpacingMax, kSpacingMax, n);
}

bool design_width_form(const std::string &value) {
	int n = 0;
	return integer_in(value, 1, kDesignWidthMax, n);
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
		if (key == "columns" || key == "rows" || key == "first") {
			// The sheet's grid: a count of cells, or the byte of the first, decimal or 0x hex.
			const bool hex = key == "first" && value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
			int parsed = 0;
			bool read = false;
			if (hex) {
				const std::optional<unsigned long> number_read = strutil::parse_ulong(value.substr(2), 16);
				read = number_read && *number_read <= 0xFF;
				parsed = read ? int(*number_read) : 0;
			} else {
				read = integer_in(value, key == "first" ? 0 : 1, key == "first" ? 0xFF : kFontSheetGridMost, parsed);
			}
			if (!read) {
				why = "line " + std::to_string(number) + "'s " + key + " takes " +
				      (key == "first" ? std::string("a byte, 0 to 255 (0x00 to 0xFF)")
				                      : "a number of cells, 1 to " + std::to_string(kFontSheetGridMost));
				return false;
			}
			(key == "columns" ? out.columns : key == "rows" ? out.rows : out.first) = parsed;
			continue;
		}
		if (key != "sheet") {
			why = "line " + std::to_string(number) + " names '" + key +
			      "', which a font set does not take (it takes sheet, columns, rows and first)";
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

bool font_import_settings(const ImportOptions &options, FontSheetSettings &out, std::string &why, std::string &field) {
	out = FontSheetSettings();
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
			out.advance = value == "left" ? FontSheetAdvance::Left : value == "cell" ? FontSheetAdvance::Cell : FontSheetAdvance::Ink;
		else if (key == "tracking" && integer_in(value, 0, kTrackingMax, number)) out.tracking = number;
		else if (key == "space" && integer_in(value, 1, kFontSheetGlyphMax, number)) out.space = number;
		else if (key == "spacing" && integer_in(value, -kSpacingMax, kSpacingMax, number)) out.spacing = number;
		else if (key == "design_width" && integer_in(value, 1, kDesignWidthMax, number)) out.design_width = uint32_t(number);
		else if (key == "color") out.sheet_color = value == "sheet";
	}
	field.clear();
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
	FontSheetSettings settings;
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
	renderer::ImageSource sheet;
	if (!renderer::decode_image_source(set.sheet, bytes, sheet, why))
		return refuse(set.sheet + ": " + why + " (a glyph sheet is a PNG, a TGA or a PCX).", CoreFinding::ImportDecode);
	ImportOutput output;
	output.name = name;
	bool opaque = false;
	// The sheet's grid, as the set says it.
	settings.columns = set.columns;
	settings.rows = set.rows;
	settings.first = set.first;
	if (!make_font_from_sheet(sheet.image, settings, output.bytes, opaque, why, field)) {
		// The grid's are the set's, not the record's options.
		if (field == "columns" || field == "rows" || field == "first") field.clear();
		return refuse(set.sheet + ": " + why + ".", field.empty() ? CoreFinding::ImportFont : CoreFinding::ImportOption, field);
	}
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
