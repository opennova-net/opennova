#include <editor/import/texture_import.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>

#include <base/io/strutil.h>
#include <editor/import/import_context.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/pcx/pcx_io.h>

namespace opennova::editor {

namespace {

// A file name of the format's extension: no folder, at most the archives' 16 characters.
bool name_form(const std::string &value) {
	return !value.empty() && value.size() <= 16 && value.find_first_of("/\\:") == std::string::npos &&
	       value.find('.') != std::string::npos;
}

} // namespace

const std::vector<ImportOptionRow> &image_import_option_rows() {
	static const std::vector<ImportOptionRow> rows = [] {
		std::vector<ImportOptionRow> out;
		ImportOptionRow format;
		format.key = "format";
		format.label = "Format";
		format.words = "The file the import writes, as the texture's loaders read it.";
		format.values = {
		        {"tga", "TGA, 32-bit: colour and alpha, the form every TGA loader reads"},
		        {"tga24", "TGA, 24-bit: colour alone (a terrain colour map's form)"},
		        {"pcx", "PCX, 8-bit: 256 colours, no alpha (a loading screen's, a sky cloud's form)"},
		        {"pcx24", "PCX, 24-bit: three planes of colour, no alpha (two of the game's loading screens)"},
		        {"dds", "DDS: DXT5 with its mip chain, the form of the game's model textures"},
		        {"mdt", "MDT: a 32-bit TGA under .mdt, a model's finished normal map"},
		        {"png", "PNG: the menus alone read one"},
		};
		format.fallback = "tga";
		out.push_back(format);
		ImportOptionRow name;
		name.key = "name";
		name.label = "File name";
		name.words = "The file the game sees, at most 16 characters, its extension the format's. Left out: the "
		             "source's name with the format's extension.";
		name.forms = {"a file name"};
		name.accepts = name_form;
		name.keeps_case = true;
		out.push_back(name);
		ImportOptionRow alpha;
		alpha.key = "alpha";
		alpha.label = "Alpha";
		alpha.words = "The alpha written: the source's, none, the texels' brightness, a cut-out at a level, or one "
		              "colour made clear.";
		alpha.values = {
		        {"source", "the source's alpha"},
		        {"opaque", "every texel opaque"},
		        {"luminance", "each texel's brightness, (85 x (r + g + b)) >> 8, as the game makes a sky PCX's"},
		};
		alpha.forms = {"threshold:<0..255>", "key:#RRGGBB"};
		alpha.accepts = renderer::image_alpha_form;
		alpha.fallback = "source";
		alpha.applies_to = "format";
		alpha.applies_values = {"tga", "dds", "mdt", "png"};
		out.push_back(alpha);
		ImportOptionRow size;
		size.key = "size";
		size.label = "Size";
		size.words = "The texture's sides.";
		size.values = {
		        {"source", "the source's sides"},
		        {"pow2_down", "each side down to a power of two"},
		        {"pow2_up", "each side up to a power of two"},
		};
		size.forms = {"<W>x<H>", "fit:<W>x<H>"};
		size.accepts = renderer::image_size_form;
		size.fallback = "source";
		out.push_back(size);
		ImportOptionRow palette;
		palette.key = "palette";
		palette.label = "Palette";
		palette.words = "How the PCX's 256 colours are chosen.";
		palette.values = {
		        {"median_cut", "the source's colours when 256 or fewer, else a median cut"},
		        {"exact", "the source's colours, refused past 256"},
		        {"indices", "an 8-bit PCX source's indices and palette as they are: a foliage or char map's data"},
		};
		palette.fallback = "median_cut";
		palette.applies_to = "format";
		palette.applies_values = {"pcx"};
		out.push_back(palette);
		ImportOptionRow dds;
		dds.key = "dds";
		dds.label = "Compression";
		dds.words = "The DDS's texels: DXT5 (colour and graded alpha), DXT1 (colour, alpha on or off) or "
		            "A8R8G8B8 (uncompressed, one level).";
		dds.values = {
		        {"dxt5", "DXT5: the retail model textures' form"},
		        {"dxt1", "DXT1: half DXT5's size, alpha on or off"},
		        {"argb", "A8R8G8B8: uncompressed, one level"},
		};
		dds.fallback = "dxt5";
		dds.applies_to = "format";
		dds.applies_values = {"dds"};
		out.push_back(dds);
		ImportOptionRow mips;
		mips.key = "mips";
		mips.label = "Mip levels";
		mips.words = "A DXT texture's levels: every one down to 1 x 1, as the game's own DDS files carry, or the "
		             "first alone.";
		mips.values = {
		        {"full", "every level, each the box filter of the one before"},
		        {"none", "the first level alone"},
		};
		mips.fallback = "full";
		mips.applies_to = "dds";
		mips.applies_values = {"dxt5", "dxt1"};
		out.push_back(mips);
		ImportOptionRow green;
		green.key = "green";
		green.label = "Green";
		green.words = "A normal map's green: the game's (its tangent frame runs down the texture) or flipped, for "
		              "one drawn with the other convention.";
		green.values = {
		        {"game", "as the source holds it"},
		        {"flip", "flipped"},
		};
		green.fallback = "game";
		out.push_back(green);
		ImportOptionRow normal;
		normal.key = "normal";
		normal.label = "Normal map";
		normal.words = "What a model's normal row naming a .tga is made from: a finished normal map, or a height map "
		               "the game turns into one.";
		normal.values = {
		        {"normal", "the source's colour as it is"},
		        {"height", "the source's brightness into the alpha, the height the game makes the normal map from, "
		                   "and its alpha into the blue, the map's alpha"},
		};
		normal.fallback = "normal";
		normal.applies_to = "format";
		normal.applies_values = {"tga"};
		out.push_back(normal);
		return out;
	}();
	return rows;
}

std::string image_import_output_name(const std::string &source_name, const renderer::ImageImportSettings &settings) {
	if (!settings.name.empty()) return settings.name;
	return utf8_of(path_of(source_name).stem()) + renderer::image_format_extension(settings.format);
}

bool run_image_import(ImportContext &context, ImportProduct &out) {
	const std::string &source_name = context.source_name();
	const renderer::ImageImportSettings settings = renderer::image_import_settings(context.options());
	const auto refuse = [&](CoreFinding code, const std::string &message, const std::string &field = std::string()) {
		out.diagnostics.push_back(make_finding(code, DiagnosticSeverity::Error, message, source_name, field));
		return false;
	};
	// Every option the record holds is one a row takes.
	for (const auto &[key, value] : context.options()) {
		const ImportOptionRow *row = import_option_row(image_import_option_rows(), key);
		if (!row) return refuse(CoreFinding::ImportOption, "The image importer has no option '" + key + "'.", key);
		// A file name keeps its case; every other value is a token, read in lower case.
		if (!value.empty() && !import_option_accepts(*row, row->keeps_case ? value : strutil::to_lower(value)))
			return refuse(CoreFinding::ImportOption,
			              "The image importer's " + key + " takes " + import_option_takes(*row) + "; '" + value + "' is none of them.",
			              key);
	}
	const std::string name = image_import_output_name(source_name, settings);
	if (strutil::to_lower(utf8_of(path_of(name).extension())) != renderer::image_format_extension(settings.format))
		return refuse(CoreFinding::ImportOption,
		              "The file name " + name + " does not end in " + renderer::image_format_extension(settings.format) +
		                      ", the extension of the format " + settings.format + ".",
		              "name");
	renderer::ImageSource source;
	std::string error;
	if (!renderer::decode_image_source(source_name, context.source(), source, error)) return refuse(CoreFinding::ImportDecode, error);
	RgbaImage &image = source.image;
	uint32_t width = 0, height = 0;
	if (!renderer::image_target_size(settings.size, uint32_t(image.width), uint32_t(image.height), width, height, error))
		return refuse(CoreFinding::ImportOption, "The image importer cannot use " + error + ".", "size");
	ImportOutput output;
	output.name = name;
	if (settings.format == "pcx" && settings.palette == "indices") {
		// The source's own indices and palette, written as they are.
		if (!source.indexed)
			return refuse(CoreFinding::ImportOption,
			              "The palette indices keeps an 8-bit PCX source's indices, and " + source_name + " holds colours.",
			              "palette");
		if (width != uint32_t(image.width) || height != uint32_t(image.height))
			return refuse(CoreFinding::ImportOption,
			              "The palette indices keeps every texel's index, so the size stays the source's.", "size");
		if (!encode_pcx_indexed(source.indices, output.bytes, error))
			return refuse(CoreFinding::ImportEncode, "Could not write " + name + ": " + error + ".");
		out.outputs.push_back(std::move(output));
		return true;
	}
	std::string field;
	if (!renderer::image_import_texels(image, settings, error, field))
		return refuse(CoreFinding::ImportOption, "The image importer cannot use " + error + ".", field);
	std::string note;
	if (!renderer::encode_image(image, settings, output.bytes, error, note))
		return refuse(CoreFinding::ImportEncode, "Could not write " + name + ": " + error + ".");
	if (!note.empty()) {
		note[0] = char(std::toupper(static_cast<unsigned char>(note[0])));
		out.diagnostics.push_back(make_finding(CoreFinding::ImportAlphaDropped, DiagnosticSeverity::Warning,
		                                       source_name + ": " + note, source_name));
	}
	out.outputs.push_back(std::move(output));
	return true;
}

} // namespace opennova::editor
