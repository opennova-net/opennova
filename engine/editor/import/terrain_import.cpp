// The terrain importer (ADR 0046 S20): a terrain set's images made into the files the game reads for a
// terrain (terrain_import.h). Tooling, not a port, but for the bake (formats/cpt/trngen: TrnGen.exe's)
// and the rules the files are made to (each cited where it binds).
#include <editor/import/terrain_import.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <utility>

#include <base/io/strutil.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <formats/cpt/cpt.h>
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_decode.h>
#include <formats/tga/tga.h>
#include <formats/til/til_io.h>
#include <formats/trn/charmap_legend.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

namespace {

constexpr int kSide = trngen::kDepthSide; // 1024: the colour map's and the heightmap's side
constexpr int kDetailSide = 512;          // a made detail's side (the shipped Det_*.tga's)
constexpr int kSurfaceSideMin = 256;      // a surface map's least side (JO ships 512)
constexpr uint8_t kNeutralGrey = 128;     // the detail's modulate x2 leaves a colour as it is at 128
constexpr double kTrnGenTop = 127.5;      // an 8-bit map's white after TrnGen's smoothing: 32 x 4 x 255 / 256

const char *const kSetKeys[] = {"heightmap", "colormap", "detail", "tiles", "surface", "foliagemap"};
constexpr size_t kSetKeyCount = sizeof(kSetKeys) / sizeof(kSetKeys[0]);
// A foliage block's keys, the .trn's own (formats/trn trn_parser_key's block arms, `end` aside) [orig:
// Terrain_ParseConfigCallback @ 0x60F330]. A definition holds graphic, match, color_lower, color_upper and
// attrib (set_foliage_key); the stampdown_* arms no port reads are keys it refuses.
bool foliage_key(const std::string &key) { return key != "end" && trn_parser_key(key, true) && !trn_parser_key(key); }

// One key of a foliage definition and its values set on `def`; false, with `why`, for values the game would
// not read as given.
bool set_foliage_key(const std::string &key, const std::vector<std::string> &values, FoliageDef &def, std::string &why) {
	if (key == "graphic") {
		if (values.size() != 1) {
			why = "graphic names one model";
			return false;
		}
		def.graphic = values.front();
		return true;
	}
	if (key == "match") {
		// The remap compares four codes [orig: Foliage_RemapPixelToDefMask @ 0x5FF4E0, +0x108..+0x10B]; code 0
		// matches nothing (pixel 0 returns before the walk) and the parser narrows a code to its byte.
		if (values.empty() || values.size() > size_t(FOLIAGE_MATCH_CODES)) {
			why = "match takes one to four codes (the game compares four)";
			return false;
		}
		def.match.fill(FOLIAGE_MATCH_UNSET);
		for (size_t i = 0; i < values.size(); ++i) {
			const std::optional<int> code = strutil::parse_int(values[i]);
			if (!code || *code < 1 || *code > 255) {
				why = "match's code '" + values[i] + "' is no code from 1 to 255 (0 grows nothing)";
				return false;
			}
			def.match[i] = *code;
		}
		return true;
	}
	if (key == "color_lower" || key == "color_upper") {
		const std::optional<int> mode = values.size() == 1 ? strutil::parse_int(values.front()) : std::nullopt;
		if (!mode || *mode < 0 || *mode > 2) {
			why = key + " is 0, 1 or 2";
			return false;
		}
		(key == "color_lower" ? def.color_lower : def.color_upper) = *mode;
		return true;
	}
	if (key == "attrib") {
		// Each word ORs its flag [orig: Terrain_ParseConfigCallback @ 0x60F58B, forceon 1, shadow 2].
		if (values.empty()) {
			why = "attrib takes forceon, shadow or both";
			return false;
		}
		for (const std::string &value : values) {
			if (strutil::iequals(value, "forceon")) def.attrib_flags = uint8_t(def.attrib_flags | FOLIAGE_ATTRIB_FORCE_ON);
			else if (strutil::iequals(value, "shadow")) def.attrib_flags = uint8_t(def.attrib_flags | FOLIAGE_ATTRIB_SHADOW);
			else {
				why = "attrib's '" + value + "' is neither forceon nor shadow";
				return false;
			}
		}
		return true;
	}
	why = "'" + key + "' is no key of a foliage definition (it takes graphic, match, color_lower, color_upper and attrib)";
	return false;
}

// A definition the game would place something by: a graphic (a slot without one is skipped [orig:
// Foliage_RemapPixelToDefMask @ 0x5FF4F2]) and a code.
bool foliage_whole(const FoliageDef &def, std::string &why) {
	if (def.graphic.empty()) {
		why = "names no graphic (the game skips a definition whose graphic is empty)";
		return false;
	}
	if (foliage_def_match_count(def) == 0) {
		why = "matches no code (it would grow nowhere)";
		return false;
	}
	return true;
}

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
TrnConfig make_trn(const std::string &stem, const TerrainImportSettings &settings, bool tiles, bool surface,
                   bool foliage) {
	TrnConfig trn;
	trn.name = stem;
	trn.colormap = stem + "_c.tga";
	trn.detailmap = stem + "_dm.tga";
	trn.detailmap_c1 = trn.detailmap_c2 = trn.detailmap_c3 = stem + "_dt.tga";
	trn.detailblendmap = stem + "_d1.tga";
	trn.polydata = stem + ".cpt";
	trn.tileinfo = stem + ".til";
	if (tiles) trn.tilestrip = stem + "_t.tga";
	if (surface) trn.charmap = stem + "_m.pcx";
	if (foliage) trn.foliagemap = stem + "_f.pcx";
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
	// The foliage block open, and the line that opened it (0: none).
	FoliageDef def;
	int block = 0;
	while (std::getline(lines, line)) {
		++number;
		const size_t comment = line.find(';');
		if (comment != std::string::npos) line.resize(comment);
		const std::string text = strutil::trim(line);
		if (text.empty()) continue;
		const size_t gap = text.find_first_of(" \t");
		const std::string key = strutil::to_lower(text.substr(0, gap));
		const std::string value = gap == std::string::npos ? std::string() : strutil::trim(text.substr(gap));
		const std::string at = "line " + std::to_string(number);
		if (block) {
			if (key == "end") {
				if (!foliage_whole(def, why)) {
					why = "the foliage block of line " + std::to_string(block) + " " + why;
					return false;
				}
				out.foliage.push_back(def);
				block = 0;
			} else if (key == "foliage") {
				why = at + " opens a foliage block inside the one of line " + std::to_string(block) + ", which has no end";
				return false;
			} else {
				// The values as the .trn's walk cuts a line (formats/trn trn_values_of_text).
				std::vector<std::string> values;
				if (!trn_values_of_text(value, values, why) || !set_foliage_key(key, values, def, why)) {
					why = at + ": " + why;
					return false;
				}
			}
			continue;
		}
		if (key == "foliage") {
			// The game reads four blocks; a fifth reads the rest of its file into nothing [orig:
			// Terrain_ParseConfigCallback @ 0x60F330, `dword_31BC900 < 4`].
			if (out.foliage.size() >= size_t(FOLIAGE_MAX_DEFS)) {
				why = at + " opens a fifth foliage block: a terrain holds four foliage definitions";
				return false;
			}
			if (!value.empty()) {
				why = at + "'s foliage opens a definition's block, its keys on the lines below it to its end, and takes no "
				           "file (the foliage map is foliagemap)";
				return false;
			}
			def = FoliageDef();
			block = number;
			continue;
		}
		if (key == "end" || foliage_key(key)) {
			why = at + "'s " + key + " stands outside a foliage block";
			return false;
		}
		std::string *slot = key == "heightmap"    ? &out.heightmap
		                    : key == "colormap"   ? &out.colormap
		                    : key == "detail"     ? &out.detail
		                    : key == "tiles"      ? &out.tiles
		                    : key == "surface"    ? &out.surface
		                    : key == "foliagemap" ? &out.foliagemap
		                                          : nullptr;
		if (!slot) {
			why = at + " names '" + key + "', which a terrain set does not take (it takes heightmap, colormap, detail, " +
			      "tiles, surface, foliagemap and foliage blocks)";
			return false;
		}
		if (value.empty()) {
			why = at + "'s " + key + " names no file";
			return false;
		}
		*slot = value;
	}
	if (block) {
		why = "the foliage block of line " + std::to_string(block) + " has no end";
		return false;
	}
	if (out.heightmap.empty() || out.colormap.empty()) {
		why = "a terrain set names its heightmap and its colormap";
		return false;
	}
	return true;
}

std::vector<uint8_t> write_terrain_set(const TerrainSet &set) {
	std::string text = "; A terrain set: the images the editor's terrain importer makes a terrain from (ADR 0046 S20).\r\n";
	const std::string *values[] = {&set.heightmap, &set.colormap, &set.detail, &set.tiles, &set.surface, &set.foliagemap};
	static_assert(sizeof(values) / sizeof(values[0]) == kSetKeyCount, "a value per set key");
	for (size_t i = 0; i < kSetKeyCount; ++i)
		if (!values[i]->empty()) text += std::string(kSetKeys[i]) + " " + *values[i] + "\r\n";
	// Each definition as the .trn writes its block.
	for (const FoliageDef &def : set.foliage) {
		text += "foliage\r\n  graphic " + def.graphic + "\r\n  match";
		for (const int code : def.match)
			if (code >= 0) text += " " + std::to_string(code);
		text += "\r\n  color_lower " + std::to_string(def.color_lower) + "\r\n  color_upper " +
		        std::to_string(def.color_upper) + "\r\n";
		if (def.attrib_flags & FOLIAGE_ATTRIB_KNOWN_MASK) {
			text += "  attrib";
			if (def.attrib_flags & FOLIAGE_ATTRIB_FORCE_ON) text += " forceon";
			if (def.attrib_flags & FOLIAGE_ATTRIB_SHADOW) text += " shadow";
			text += "\r\n";
		}
		text += "end\r\n";
	}
	return text_bytes(text);
}

bool parse_foliage_definitions(const std::string &text, std::vector<FoliageDef> &out, std::string &why) {
	out.clear();
	size_t start = 0;
	while (start <= text.size()) {
		const size_t bar = text.find('|', start);
		const std::string one = strutil::trim(text.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
		start = bar == std::string::npos ? text.size() + 1 : bar + 1;
		if (one.empty()) continue;
		if (out.size() >= size_t(FOLIAGE_MAX_DEFS)) {
			why = "a terrain holds four foliage definitions";
			return false;
		}
		const std::string number = "definition " + std::to_string(out.size() + 1);
		// A key, then its values up to the next key.
		std::vector<std::string> words;
		if (!trn_values_of_text(one, words, why)) {
			why = number + ": " + why;
			return false;
		}
		FoliageDef def;
		for (size_t i = 0; i < words.size();) {
			const std::string key = strutil::to_lower(words[i]);
			if (!foliage_key(key)) {
				why = number + ": '" + words[i] + "' is no key of a foliage definition (it takes graphic, match, " +
				      "color_lower, color_upper and attrib)";
				return false;
			}
			std::vector<std::string> values;
			for (++i; i < words.size() && !foliage_key(strutil::to_lower(words[i])); ++i) values.push_back(words[i]);
			if (!set_foliage_key(key, values, def, why)) {
				why = number + ": " + why;
				return false;
			}
		}
		if (!foliage_whole(def, why)) {
			why = number + " " + why;
			return false;
		}
		out.push_back(def);
	}
	return true;
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

std::vector<std::string> terrain_output_names(const std::string &stem, bool tiles, bool surface, bool foliage) {
	std::vector<std::string> names = {stem + ".cpt", stem + "_c.tga", stem + "_dt.tga", stem + "_dm.tga",
	                                  stem + "_d1.tga"};
	if (tiles) names.push_back(stem + "_t.tga");
	names.push_back(stem + ".til");
	if (surface) names.push_back(stem + "_m.pcx");
	if (foliage) names.push_back(stem + "_f.pcx");
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
	png::GrayImage grey;
	if (!png::decode_png_gray(bytes, grey, why)) {
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
	renderer::ImageSource source;
	if (!renderer::decode_image_source(name, bytes, source, why)) {
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

bool decode_terrain_surface(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                            std::string &why) {
	out = IndexedImage8();
	// The texels as an image program shows them, an indexed image's indices beside its colours.
	RgbaImage colours;
	IndexedImage8 indices;
	if (strutil::ends_with_icase(name, ".png")) {
		if (!png::decode_png(bytes, colours, why, &indices)) {
			why = name + ": " + why;
			return false;
		}
	} else {
		renderer::ImageSource source;
		if (!renderer::decode_image_source(name, bytes, source, why)) {
			why = name + ": " + why;
			return false;
		}
		colours = std::move(source.image);
		if (source.indexed) indices = std::move(source.indices);
	}
	// The game keeps the side alone, the rows' length and the power of two it samples the 1024-unit
	// heightmap by, so the map is square and a power of two no wider than the heightmap [orig: sub_605A10 @
	// 0x605A82..0x605AA0; Terrain_GetSurfaceTypeAtPosition @ 0x6065C6].
	const int w = colours.width, h = colours.height;
	if (w != h || !power_of_two(w) || w < kSurfaceSideMin || w > kSide) {
		why = name + " is " + std::to_string(w) + " x " + std::to_string(h) +
		      ": a surface map is square, 256, 512 or 1024 texels a side, laid over the heightmap";
		return false;
	}
	const bool indexed = !indices.empty();
	const size_t texels = size_t(w) * size_t(h);
	out.width = w;
	out.height = h;
	out.indices.assign(texels, 0);
	// The legend its palette, every entry past it white as the shipped legend leaves its own.
	for (int i = 0; i < 256; ++i) {
		const CharmapLegendColour &c = i < kCharmapLegendCount ? kCharmapLegend[i] : kCharmapLegendRest;
		out.palette[i][0] = c.r;
		out.palette[i][1] = c.g;
		out.palette[i][2] = c.b;
	}
	size_t strays = 0, first = 0;
	for (size_t i = 0; i < texels; ++i) {
		const uint8_t *p = &colours.pixels[i * 4];
		const int surface = indexed ? int(indices.indices[i]) : charmap_legend_class(p[0], p[1], p[2]);
		if (surface < 0 || surface >= kCharmapLegendCount) {
			if (strays++ == 0) first = i;
			continue;
		}
		out.indices[i] = uint8_t(surface);
	}
	if (strays == 0) return true;
	const std::string at = "(" + std::to_string(first % size_t(w)) + ", " + std::to_string(first / size_t(w)) + ")";
	const std::string more = strays > 1 ? " (" + std::to_string(strays) + " texels in all)" : std::string();
	if (indexed) {
		why = name + "'s texel " + at + " holds index " + std::to_string(indices.indices[first]) + more +
		      ": an indexed surface map's indices are the surface classes, 0 to 19";
	} else {
		const uint8_t *p = &colours.pixels[first * 4];
		char colour[8];
		std::snprintf(colour, sizeof(colour), "#%02X%02X%02X", p[0], p[1], p[2]);
		why = name + "'s texel " + at + " is " + colour + more +
		      ", the colour of no surface class: a colour surface map paints each class in its legend colour exactly";
	}
	out = IndexedImage8();
	return false;
}

bool decode_terrain_foliage(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                            std::string &why) {
	out = IndexedImage8();
	RgbaImage colours;
	IndexedImage8 indices;
	if (strutil::ends_with_icase(name, ".png")) {
		if (!png::decode_png(bytes, colours, why, &indices)) {
			why = name + ": " + why;
			return false;
		}
	} else {
		renderer::ImageSource source;
		if (!renderer::decode_image_source(name, bytes, source, why)) {
			why = name + ": " + why;
			return false;
		}
		colours = std::move(source.image);
		if (source.indexed) indices = std::move(source.indices);
	}
	// The game keeps the width alone, the rows' length and the power of two it shifts the atlas position by
	// [orig: Foliage_LoadFoliageMapPCX @ 0x605B44..0x605B60; Foliage_SampleFoliageMapMask @ 0x60662D]: a
	// map shorter than that is read past its last row, one wider than 1024 at its first texel alone.
	const int w = colours.width, h = colours.height;
	if (w != h || !power_of_two(w) || w > kSide) {
		why = name + " is " + std::to_string(w) + " x " + std::to_string(h) +
		      ": a foliage map is square, a power of two at most 1024 texels a side, laid over the heightmap";
		return false;
	}
	const size_t texels = size_t(w) * size_t(h);
	out.width = w;
	out.height = h;
	if (!indices.empty()) {
		out.indices = std::move(indices.indices);
		std::memcpy(out.palette, indices.palette, sizeof(out.palette));
		return true;
	}
	// A grey image: its levels the codes, a grey ramp the palette.
	out.indices.assign(texels, 0);
	for (size_t i = 0; i < texels; ++i) {
		const uint8_t *p = &colours.pixels[i * 4];
		if (p[0] != p[1] || p[1] != p[2]) {
			char colour[8];
			std::snprintf(colour, sizeof(colour), "#%02X%02X%02X", p[0], p[1], p[2]);
			why = name + "'s texel (" + std::to_string(i % size_t(w)) + ", " + std::to_string(i / size_t(w)) + ") is " +
			      colour + ": a foliage map's texels are codes, an indexed image's indices or a grey image's levels";
			out = IndexedImage8();
			return false;
		}
		out.indices[i] = p[0];
	}
	for (int i = 0; i < 256; ++i) out.palette[i][0] = out.palette[i][1] = out.palette[i][2] = uint8_t(i);
	return true;
}

std::vector<std::string> terrain_foliage_notes(const IndexedImage8 &map, const std::vector<FoliageDef> &defs) {
	std::vector<std::string> notes;
	size_t counts[256] = {};
	for (const uint8_t code : map.indices) ++counts[code];
	std::string loose;
	size_t loose_texels = 0;
	for (int code = 1; code < 256; ++code) {
		if (!counts[code] || foliage_remap_pixel_to_def_mask(defs, code) != 0) continue;
		loose += (loose.empty() ? "" : ", ") + std::to_string(code);
		loose_texels += counts[code];
	}
	if (!loose.empty())
		notes.push_back("codes " + loose + " (" + std::to_string(loose_texels) + " texels) match no foliage definition: " +
		                "nothing grows there");
	for (size_t slot = 0; slot < defs.size() && slot < size_t(FOLIAGE_MAX_DEFS); ++slot) {
		bool held = false;
		for (const int code : defs[slot].match)
			if (code > 0 && code < 256 && counts[code]) held = true;
		if (!held)
			notes.push_back("foliage " + std::to_string(slot + 1) + " (" + defs[slot].graphic +
			                ") matches no code the foliage map holds: it grows nowhere");
	}
	return notes;
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

	IndexedImage8 surface;
	if (!set.surface.empty()) {
		if (!context.read(set.surface, bytes)) return false;
		if (!decode_terrain_surface(set.surface, bytes, surface, why)) return refuse(why + ".");
	}

	IndexedImage8 foliage;
	if (!set.foliagemap.empty()) {
		if (!context.read(set.foliagemap, bytes)) return false;
		if (!decode_terrain_foliage(set.foliagemap, bytes, foliage, why)) return refuse(why + ".");
	}
	// What the foliage map grows by the definitions, as the game remaps it at load [orig:
	// Foliage_LoadFoliageMapPCX @ 0x605B73..0x605B8A; Foliage_RemapPixelToDefMask @ 0x5FF4E0].
	std::vector<std::string> foliage_notes;
	if (foliage.empty() && !set.foliage.empty())
		foliage_notes.push_back("the set's foliage definitions grow nothing: it names no foliagemap");
	else if (!foliage.empty() && set.foliage.empty())
		foliage_notes.push_back(set.foliagemap + " grows nothing: the set holds no foliage definition");
	else if (!foliage.empty())
		for (std::string &note : terrain_foliage_notes(foliage, set.foliage)) foliage_notes.push_back(set.foliagemap + ": " + note);
	for (const std::string &note : foliage_notes)
		out.diagnostics.push_back(
		        make_finding(CoreFinding::ImportTerrain, DiagnosticSeverity::Warning, note + ".", source_name, "foliagemap"));

	// The bake: the heights and the ground mesh.
	trngen::TerrainBakeInput bake;
	bake.depth8 = std::move(heights.depth8);
	bake.depth16 = std::move(heights.depth16);
	bake.terrain_name = stem;
	bake.creator = "OpenNova";
	bake.version = "opennova";
	bake.depth_format = DepthFormat::CDEP;
	bake.threads = trngen::TerrainBakeInput::for_hardware(); // the editor's count: the desktop sizing
	CptFile cpt;
	if (!trngen::bake_terrain(bake, cpt, why)) return refuse(source_name + ": " + why + ".");
	// The blocks the CPT writer clamps (formats/cpt cpt_steep_blocks).
	const int steep = cpt_steep_blocks(cpt.depth_buffer);
	ImportOutput cpt_out;
	cpt_out.name = stem + ".cpt";
	if (!save_cpt(cpt, cpt_out.bytes, why))
		return refuse(source_name + ": the .cpt cannot be written: " + why + ".", CoreFinding::ImportEncode);
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

	if (!surface.empty()) {
		ImportOutput charmap;
		charmap.name = stem + "_m.pcx";
		if (!encode_pcx_indexed(surface, charmap.bytes, why))
			return refuse(charmap.name + " cannot be written: " + why + ".", CoreFinding::ImportEncode);
		out.outputs.push_back(std::move(charmap));
	}

	if (!foliage.empty()) {
		// The game's 8-bit PCX reader takes the indices alone [orig: Foliage_LoadFoliageMapPCX @ 0x605AF1 ->
		// Texture_LoadPCXFromPFF8Bit @ 0x56E0A0]; retail names it `_f` (Dvxi5_f.pcx).
		ImportOutput foliage_map;
		foliage_map.name = stem + "_f.pcx";
		if (!encode_pcx_indexed(foliage, foliage_map.bytes, why))
			return refuse(foliage_map.name + " cannot be written: " + why + ".", CoreFinding::ImportEncode);
		out.outputs.push_back(std::move(foliage_map));
	}

	ImportOutput trn_out;
	trn_out.name = stem + ".trn";
	std::ostringstream trn_text;
	TrnConfig trn = make_trn(stem, settings, !tiles.empty(), !surface.empty(), !foliage.empty());
	// The definitions as the .trn's foliage blocks, in their slots' order.
	trn.foliage_defs = set.foliage;
	if (!save_trn(trn_text, trn, why))
		return refuse(trn_out.name + " cannot be written: " + why + ".", CoreFinding::ImportEncode);
	trn_out.bytes = text_bytes(trn_text.str());
	out.outputs.push_back(std::move(trn_out));
	return true;
}

} // namespace opennova::editor
