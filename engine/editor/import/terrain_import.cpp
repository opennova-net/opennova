// The terrain importer (ADR 0046 S20): a terrain set's images made into the files the game reads for a
// terrain (terrain_import.h). Tooling, not a port, but for the bake (editor/terrain: TrnGen.exe's) and
// the rules the files are made to (each cited where it binds).
#include <editor/import/terrain_import.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <optional>
#include <sstream>
#include <utility>

#include <base/io/strutil.h>
#include <editor/import/png_decode.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <editor/terrain/terrain_bake.h>
#include <formats/cpt/cpt.h>
#include <formats/tga/tga.h>
#include <formats/til/til_io.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

namespace {

constexpr int kSide = trngen::kDepthSide; // 1024: the colour map's and the heightmap's side
constexpr int kDetailSide = 512;          // a made detail's side (the shipped Det_*.tga's)
constexpr uint8_t kNeutralGrey = 128;     // the detail's modulate x2 leaves a colour as it is at 128
constexpr double kTrnGenTop = 127.5;      // an 8-bit map's white after TrnGen's smoothing: 32 x 4 x 255 / 256

const char *const kSetKeys[] = {"heightmap", "colormap", "detail", "tiles"};

bool number_in(const std::string &value, double low, double high, double &out) {
	const std::optional<double> parsed = strutil::parse_double(value);
	if (!parsed || !std::isfinite(*parsed) || *parsed < low || *parsed > high) return false;
	out = *parsed;
	return true;
}

bool top_form(const std::string &value) {
	double top = 0;
	return number_in(value, 0.25, 255.99, top);
}

bool water_form(const std::string &value) {
	double water = 0;
	return number_in(value, 0.0, 255.0, water);
}

bool power_of_two(int side) { return side > 0 && (side & (side - 1)) == 0; }

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A flat RGBA image of `side` x `side`.
RgbaImage flat_image(int side, uint8_t r, uint8_t g, uint8_t b) {
	RgbaImage image;
	image.width = side;
	image.height = side;
	image.pixels.resize(size_t(side) * side * 4);
	for (size_t i = 0; i < image.pixels.size(); i += 4) {
		image.pixels[i] = r;
		image.pixels[i + 1] = g;
		image.pixels[i + 2] = b;
		image.pixels[i + 3] = 255;
	}
	return image;
}

// The .trn the outputs make: the names, the sector grid of the layout, the water.
TrnConfig make_trn(const std::string &stem, const TerrainImportSettings &settings, bool tiles) {
	TrnConfig trn;
	trn.name = stem;
	trn.colormap = stem + "_c.tga";
	trn.detailmap = stem + "_dm.tga";
	trn.detailmap_c1 = trn.detailmap_c2 = trn.detailmap_c3 = stem + "_dt.tga";
	trn.detailblendmap = stem + "_d1.tga";
	trn.polydata = stem + ".cpt";
	trn.tileinfo = stem + ".til";
	if (tiles) trn.tilestrip = stem + "_t.tga";
	// The .trn's water_height is half world units [orig: TimeOfDay_ParseProperty @ 0x57CB48..0x57CB6A,
	// atol << 15 into 16.16]; 0 draws no water [orig: render_water_surface @ 0x5C32E0..0x5C32E7].
	trn.water_height = static_cast<int>(std::lround(settings.water * 2.0));
	// An 8 x 8 grid of 512-unit sectors from (-4, -4), as the shipped maps lay theirs out: `island` the
	// four quadrants of the 1024 atlas in the middle, the world's origin at the heightmap's centre and
	// every other sector the flat ground at height 0 (Dvxi5.trn's grid); `tiled` the four repeated over
	// the grid, wrapping both ways (flat.trn's). Quadrant ids: 1 top-left, 3 top-right, 2 bottom-left,
	// 4 bottom-right (runtime/terrain_query/coords.h).
	trn.sector_count = 8;
	trn.sector_rows = 8;
	trn.origin_x = -4;
	trn.origin_y = -4;
	// Dvxi5's block starts at row and column 3 (the heightmap's corner at world (-512, -512)); flat.trn's
	// pattern at row and column 0 (its corner at (-2048, -2048), and so at the world's origin).
	const bool tiled = settings.layout == "tiled";
	for (int row = 0; row < 8; ++row)
		for (int col = 0; col < 8; ++col) {
			const bool top = (row % 2 == 0) == tiled, left = (col % 2 == 0) == tiled;
			const int quadrant = top ? (left ? 1 : 3) : (left ? 2 : 4);
			const bool middle = (row == 3 || row == 4) && (col == 3 || col == 4);
			trn.sector_grid[row][col] = tiled || middle ? quadrant : 0;
		}
	trn.wrap_x = trn.wrap_y = tiled ? 1 : 0;
	return trn;
}

} // namespace

bool parse_terrain_set(const std::vector<uint8_t> &bytes, TerrainSet &out, std::string &why) {
	out = TerrainSet();
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
		std::string *slot = key == "heightmap" ? &out.heightmap
		                    : key == "colormap" ? &out.colormap
		                    : key == "detail" ? &out.detail
		                    : key == "tiles" ? &out.tiles
		                                     : nullptr;
		if (!slot) {
			why = "line " + std::to_string(number) + " names '" + key + "', which a terrain set does not take (it takes " +
			      "heightmap, colormap, detail and tiles)";
			return false;
		}
		if (value.empty()) {
			why = "line " + std::to_string(number) + "'s " + key + " names no file";
			return false;
		}
		*slot = value;
	}
	if (out.heightmap.empty() || out.colormap.empty()) {
		why = "a terrain set names its heightmap and its colormap";
		return false;
	}
	return true;
}

std::vector<uint8_t> write_terrain_set(const TerrainSet &set) {
	std::string text = "; A terrain set: the images the editor's terrain importer makes a terrain from (ADR 0046 S20).\r\n";
	const std::string *values[] = {&set.heightmap, &set.colormap, &set.detail, &set.tiles};
	for (size_t i = 0; i < 4; ++i)
		if (!values[i]->empty()) text += std::string(kSetKeys[i]) + " " + *values[i] + "\r\n";
	return text_bytes(text);
}

const std::vector<ImportOptionRow> &terrain_import_option_rows() {
	static const std::vector<ImportOptionRow> rows = [] {
		std::vector<ImportOptionRow> out;
		ImportOptionRow top;
		top.key = "top";
		top.label = "Height of white";
		top.words = "The height, in world units, of the heightmap's brightest value. An 8-bit map's grey levels are "
		            "TrnGen's half a unit each at 127.5, scaled from there; a 16-bit map spans 0 to this. A .raw of the "
		            "game's own 16-bit heights takes none.";
		top.forms = {"<0.25..255.99>"};
		top.accepts = top_form;
		top.fallback = "127.5";
		out.push_back(top);
		ImportOptionRow water;
		water.key = "water";
		water.label = "Water level";
		water.words = "The sea's height in world units; 0 is no water.";
		water.forms = {"<0..255>"};
		water.accepts = water_form;
		water.fallback = "0";
		out.push_back(water);
		ImportOptionRow layout;
		layout.key = "layout";
		layout.label = "Layout";
		layout.words = "Where the 1024 x 1024 heightmap lies in the world's grid of 512-unit sectors.";
		layout.values = {
		        {"island", "once, its centre at the world's origin, flat ground at height 0 around it"},
		        {"tiled", "repeated over 4096 x 4096 units, its edges meeting"},
		};
		layout.fallback = "island";
		out.push_back(layout);
		return out;
	}();
	return rows;
}

bool terrain_import_settings(const ImportOptions &options, TerrainImportSettings &out, std::string &why,
                             std::string &field) {
	out = TerrainImportSettings();
	for (const auto &[key, raw] : options) {
		const ImportOptionRow *row = import_option_row(terrain_import_option_rows(), key);
		field = key;
		if (!row) {
			why = "The terrain importer has no option '" + key + "'.";
			return false;
		}
		const std::string value = strutil::to_lower(raw);
		if (value.empty()) continue;
		if (!import_option_accepts(*row, value)) {
			why = "The terrain importer's " + key + " takes " + import_option_takes(*row) + "; '" + raw + "' is none of them.";
			return false;
		}
		if (key == "top") number_in(value, 0.25, 255.99, out.top);
		else if (key == "water") number_in(value, 0.0, 255.0, out.water);
		else if (key == "layout") out.layout = value;
	}
	field.clear();
	return true;
}

bool terrain_stem_fits(const std::string &stem, std::string &why) {
	if (stem.empty()) {
		why = "a terrain needs a name";
		return false;
	}
	if (stem.size() > kTerrainStemMax) {
		why = "a terrain's name is at most " + std::to_string(kTerrainStemMax) + " characters (" + stem + "_dt.tga, its longest file, " +
		      "must fit an archive's 16)";
		return false;
	}
	for (char c : stem)
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
			why = "a terrain's name is letters, digits and underscores (" + stem + ")";
			return false;
		}
	return true;
}

std::vector<std::string> terrain_output_names(const std::string &stem, bool tiles) {
	std::vector<std::string> names = {stem + ".cpt", stem + "_c.tga", stem + "_dt.tga", stem + "_dm.tga",
	                                  stem + "_d1.tga"};
	if (tiles) names.push_back(stem + "_t.tga");
	names.push_back(stem + ".til");
	names.push_back(stem + ".trn");
	return names;
}

bool decode_terrain_heightmap(const std::string &name, const std::vector<uint8_t> &bytes, double top,
                              TerrainHeights &out, std::string &why) {
	out = TerrainHeights();
	const size_t texels = size_t(kSide) * kSide;
	const auto scaled16 = [&](const std::vector<uint16_t> &raw16) {
		// A raw16 height scaled by top / 127.5, rounded, kept within the format's 16 bits.
		const double scale = top / kTrnGenTop;
		out.depth16.resize(raw16.size());
		for (size_t i = 0; i < raw16.size(); ++i)
			out.depth16[i] = static_cast<uint16_t>(std::min(65535.0, std::floor(raw16[i] * scale + 0.5)));
	};
	const auto from8 = [&](std::vector<uint8_t> depth8) {
		// TrnGen's input as it is at its own scale; else smoothed as TrnGen smooths it, then scaled.
		if (top == kTrnGenTop) out.depth8 = std::move(depth8);
		else scaled16(trngen::smooth_depthmap(depth8));
	};
	if (strutil::ends_with_icase(name, ".raw")) {
		if (bytes.size() == texels) {
			from8(bytes);
			return true;
		}
		if (bytes.size() == texels * 2) {
			out.depth16.resize(texels);
			for (size_t i = 0; i < texels; ++i) out.depth16[i] = static_cast<uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
			return true;
		}
		why = name + " is " + std::to_string(bytes.size()) + " bytes: a .raw heightmap is 1024 x 1024 texels, 1 MiB of 8-bit "
		      "heights or 2 MiB of 16-bit";
		return false;
	}
	GrayImage grey;
	if (!decode_png_gray(bytes, grey, why)) {
		why = name + ": " + why + " (a heightmap is a PNG or a .raw)";
		return false;
	}
	if (grey.width != kSide || grey.height != kSide) {
		why = name + " is " + std::to_string(grey.width) + " x " + std::to_string(grey.height) +
		      ": a heightmap is 1024 x 1024 texels, one a world unit";
		return false;
	}
	if (grey.max_value == 255) {
		std::vector<uint8_t> depth8(grey.samples.size());
		for (size_t i = 0; i < depth8.size(); ++i) depth8[i] = static_cast<uint8_t>(grey.samples[i]);
		from8(std::move(depth8));
		return true;
	}
	// 16 bits: 0 to 65535 over 0 to top world units, 256 raw a unit.
	out.depth16.resize(texels);
	const double scale = top * 256.0 / 65535.0;
	for (size_t i = 0; i < texels; ++i)
		out.depth16[i] = static_cast<uint16_t>(std::min(65535.0, std::floor(grey.samples[i] * scale + 0.5)));
	return true;
}

bool decode_terrain_image(const std::string &key, const std::string &name, const std::vector<uint8_t> &bytes,
                          RgbaImage &out, std::string &why) {
	ImageSource source;
	if (!decode_image_source(name, bytes, source, why)) {
		why = name + ": " + why;
		return false;
	}
	const int w = source.image.width, h = source.image.height;
	const std::string sides = name + " is " + std::to_string(w) + " x " + std::to_string(h);
	if (key == "colormap" && (w != kSide || h != kSide)) {
		why = sides + ": the game reads a terrain's colour map as exactly 1024 x 1024 texels";
		return false;
	}
	if (key == "detail" && (!power_of_two(w) || !power_of_two(h))) {
		why = sides + ": the game makes a detail's coefficients over sides that are powers of two";
		return false;
	}
	if (key == "tiles" && (w < 64 || h < 64 || w % 64 || h % 64)) {
		why = sides + ": the game cuts a tile set's atlas in 64-texel cells, so its sides are multiples of 64";
		return false;
	}
	out = std::move(source.image);
	return true;
}

int terrain_steep_blocks(const std::vector<uint16_t> &raw16) {
	int steep = 0;
	for (size_t start = 0; start + 256 <= raw16.size(); start += 256) {
		const auto [low, high] = std::minmax_element(raw16.begin() + static_cast<std::ptrdiff_t>(start),
		                                             raw16.begin() + static_cast<std::ptrdiff_t>(start + 256));
		if (int(*high) - int(*low) > 32766) ++steep; // a width of range + 1 holds 32,766 in 15 bits
	}
	return steep;
}

bool run_terrain_import(ImportContext &context, ImportProduct &out) {
	const std::string &source_name = context.source_name();
	const std::string stem = utf8_of(path_of(source_name).stem());
	const auto refuse = [&](const std::string &message, CoreFinding code = CoreFinding::ImportTerrain,
	                        const std::string &field = std::string()) {
		out.outputs.clear();
		out.diagnostics.push_back(make_finding(code, DiagnosticSeverity::Error, message, source_name, field));
		return false;
	};
	TerrainImportSettings settings;
	std::string why, field;
	if (!terrain_import_settings(context.options(), settings, why, field)) return refuse(why, CoreFinding::ImportOption, field);
	if (!terrain_stem_fits(stem, why)) return refuse(source_name + ": " + why + ".");
	TerrainSet set;
	if (!parse_terrain_set(context.source(), set, why)) return refuse(source_name + ": " + why + ".");

	// Every image read through the context: each an input, a change to it importing the terrain again.
	std::vector<uint8_t> bytes;
	if (!context.read(set.heightmap, bytes)) return false;
	TerrainHeights heights;
	if (!decode_terrain_heightmap(set.heightmap, bytes, settings.top, heights, why)) return refuse(why + ".");

	RgbaImage colour;
	if (!context.read(set.colormap, bytes)) return false;
	if (!decode_terrain_image("colormap", set.colormap, bytes, colour, why)) return refuse(why + ".");

	RgbaImage detail = flat_image(kDetailSide, kNeutralGrey, kNeutralGrey, kNeutralGrey);
	if (!set.detail.empty()) {
		if (!context.read(set.detail, bytes)) return false;
		if (!decode_terrain_image("detail", set.detail, bytes, detail, why)) return refuse(why + ".");
	}

	RgbaImage tiles;
	if (!set.tiles.empty()) {
		if (!context.read(set.tiles, bytes)) return false;
		if (!decode_terrain_image("tiles", set.tiles, bytes, tiles, why)) return refuse(why + ".");
	}

	// The bake: the heights and the ground mesh.
	trngen::TerrainBakeInput bake;
	bake.depth8 = std::move(heights.depth8);
	bake.depth16 = std::move(heights.depth16);
	bake.terrain_name = stem;
	bake.creator = "OpenNova";
	bake.version = "opennova";
	bake.depth_format = DepthFormat::CDEP;
	CptFile cpt;
	if (!trngen::bake_terrain(bake, cpt, why)) return refuse(source_name + ": " + why + ".");
	const int steep = terrain_steep_blocks(cpt.depth_buffer);
	ImportOutput cpt_out;
	cpt_out.name = stem + ".cpt";
	try {
		cpt_out.bytes = cpt.write_bytes();
	} catch (const std::exception &e) {
		return refuse(source_name + ": the .cpt cannot be written: " + e.what() + ".", CoreFinding::ImportEncode);
	}
	if (steep > 0)
		out.diagnostics.push_back(make_finding(
		        CoreFinding::ImportTerrain, DiagnosticSeverity::Warning,
		        set.heightmap + " rises more than 128 world units within 256 texels of a row in " + std::to_string(steep) +
		                " place(s): the game's compressed heights cannot hold that, so the .cpt clamps them there. Lower `top` "
		                "or soften the slope.",
		        source_name, "top"));

	const auto tga = [&](const std::string &name, const RgbaImage &image, bool alpha) {
		ImportOutput output;
		output.name = name;
		std::string error;
		const bool ok = alpha ? tga::tga_write_rgba32(image.pixels.data(), uint32_t(image.width), uint32_t(image.height), output.bytes, error)
		                      : tga::tga_write_rgb24(image.pixels.data(), uint32_t(image.width), uint32_t(image.height), output.bytes, error);
		if (!ok) {
			refuse(name + " cannot be written: " + error + ".", CoreFinding::ImportEncode);
			return false;
		}
		out.outputs.push_back(std::move(output));
		return true;
	};
	out.outputs.push_back(std::move(cpt_out));
	if (!tga(stem + "_c.tga", colour, false)) return false;
	if (!tga(stem + "_dt.tga", detail, false)) return false;
	if (!tga(stem + "_dm.tga", detail, false)) return false;
	if (!tga(stem + "_d1.tga", flat_image(kSide, 255, 0, 0), true)) return false;
	if (!tiles.empty() && !tga(stem + "_t.tga", tiles, true)) return false;

	ImportOutput til;
	til.name = stem + ".til";
	if (!save_til(TilFile(), til.bytes, why)) return refuse(til.name + " cannot be written: " + why + ".", CoreFinding::ImportEncode);
	out.outputs.push_back(std::move(til));

	ImportOutput trn_out;
	trn_out.name = stem + ".trn";
	std::ostringstream trn_text;
	if (!save_trn(trn_text, make_trn(stem, settings, !tiles.empty()), why))
		return refuse(trn_out.name + " cannot be written: " + why + ".", CoreFinding::ImportEncode);
	trn_out.bytes = text_bytes(trn_text.str());
	out.outputs.push_back(std::move(trn_out));
	return true;
}

} // namespace opennova::editor
