#include <editor/documents/texture_operations.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include <base/io/strutil.h>
#include <editor/documents/texture_image.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_files.h>
#include <formats/pcx/pcx_io.h>

namespace opennova::editor {

namespace {

using K = TextureOperationKind;

struct OperationRow {
	K kind;
	const char *token;
};

constexpr OperationRow kRows[] = {
	{K::Resize, "resize"},
	{K::Alpha, "alpha"},
	{K::Format, "format"},
	{K::ReorderRows, "reorder_rows"},
	{K::RemapPalette, "remap_palette"},
};
static_assert(std::size(kRows) == size_t(K::kCount), "a row per texture operation");

std::string extension_of(const std::string &name) { return strutil::to_lower(utf8_of(path_of(name).extension())); }

// The params as an operation reads them: each key once, in lower case.
bool param(const TextureOperation &operation, const char *key, std::string &out) {
	for (const auto &[name, value] : operation.params)
		if (strutil::to_lower(name) == key) {
			out = strutil::to_lower(value);
			return true;
		}
	return false;
}

// The form the file is stored in, as the importer's settings write it: a .tga's depth, a .mdt's 32-bit TGA,
// a .pcx's quantized colours, a .dds's format and whether it carries its chain, a .png.
bool stored_settings(const std::string &name, const std::vector<uint8_t> &bytes, renderer::ImageImportSettings &out, std::string &why) {
	out = renderer::image_import_settings({});
	const TextureHeader header = texture_header(name, bytes);
	const std::string extension = extension_of(name);
	if (extension == ".tga") {
		out.format = header.tga_bits == 24 || header.tga_bits == 8 ? "tga24" : "tga";
		if (header.tga_type == 1 || header.tga_type == 3) out.format = "tga24";
		return true;
	}
	if (extension == ".mdt") {
		out.format = "mdt";
		return true;
	}
	if (extension == ".pcx") {
		// Its colours in three planes stay so; its indices are kept by the operations that read them
		// (indexed_pcx), never quantized again.
		out.format = header.pcx_rgb() ? "pcx24" : "pcx";
		return true;
	}
	if (extension == ".png") {
		out.format = "png";
		return true;
	}
	if (extension == ".dds") {
		out.format = "dds";
		if (header.dds_format == "DXT5") out.dds = "dxt5";
		else if (header.dds_format == "DXT1") out.dds = "dxt1";
		else if (header.dds_format == "A8R8G8B8") out.dds = "argb";
		else {
			why = "the editor writes a DDS as DXT5, DXT1 or A8R8G8B8, and this one is " +
			      (header.dds_format.empty() ? std::string("of a form it does not read") : header.dds_format);
			return false;
		}
		out.mips = header.dds_levels > 1 ? "full" : "none";
		return true;
	}
	why = basename_of(name) + " is no texture the editor writes";
	return false;
}

bool holds_alpha(const renderer::ImageImportSettings &settings) {
	return settings.format == "tga" || settings.format == "mdt" || settings.format == "png" || settings.format == "dds";
}

std::string format_words(const renderer::ImageImportSettings &settings) {
	if (settings.format == "tga") return "a 32-bit TGA";
	if (settings.format == "tga24") return "a 24-bit TGA";
	if (settings.format == "mdt") return "a 32-bit TGA under .mdt";
	if (settings.format == "png") return "a PNG";
	if (settings.format == "pcx24") return "a 24-bit PCX";
	if (settings.format == "pcx") return settings.palette == "exact" ? "an 8-bit PCX of its own colours" : "an 8-bit PCX";
	const std::string dds = settings.dds == "dxt5" ? "DXT5" : settings.dds == "dxt1" ? "DXT1" : "A8R8G8B8";
	return "a " + dds + " DDS" + (settings.dds != "argb" ? (settings.mips == "full" ? " with its mip chain" : " of one level") : "");
}

// The texels as the game reads them (the first level), or false with why.
bool game_texels(const std::string &name, const std::vector<uint8_t> &bytes, RgbaImage &out, std::string &why) {
	const std::shared_ptr<const TextureImage> image = decode_texture(name, bytes);
	if (!image || !image->loads) {
		why = "the game cannot read it" + (image && !image->refusal.empty() ? ": " + image->refusal : std::string());
		return false;
	}
	if (!image->decoded || image->levels.empty()) {
		why = "the editor does not decode its form" + (image->undecoded.empty() ? std::string() : " (" + image->undecoded + ")");
		return false;
	}
	out.width = int(image->levels[0].width);
	out.height = int(image->levels[0].height);
	out.pixels = image->levels[0].rgba;
	return true;
}

bool encode(const RgbaImage &image, const renderer::ImageImportSettings &settings, std::vector<uint8_t> &out, std::string &why) {
	std::string note;
	return renderer::encode_image(image, settings, out, why, note);
}

// An 8-bit PCX's indices and palette as the game reads them (decode_pcx_menu_rgba): false, with why, for
// one of colours or one that does not read.
bool indexed_pcx(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out, std::string &why) {
	if (extension_of(name) != ".pcx") return false;
	RgbaImage colours;
	PcxIndexed indexed;
	if (!decode_pcx_menu_rgba(bytes.data(), bytes.size(), colours, why, &indexed) || !indexed.indexed) return false;
	out.width = colours.width;
	out.height = colours.height;
	out.indices = std::move(indexed.indices);
	std::copy(&indexed.palette[0][0], &indexed.palette[0][0] + 256 * 3, &out.palette[0][0]);
	return true;
}

// A cube map's faces or a volume's slices, which every operation would flatten to the first: refused.
bool layered_refused(const std::string &name, const std::vector<uint8_t> &bytes, std::string &why) {
	const TextureHeader header = texture_header(name, bytes);
	if (!header.layered()) return false;
	why = header.dds_faces > 1 ? "it is a cube map of six faces, and the editor writes a flat texture: the edit would keep the first "
	                             "face alone, which the cube map's loader cannot take"
	                           : "it is a volume of " + std::to_string(header.dds_depth) +
	                                     " slices, and the editor writes a flat texture: the edit would keep the first slice alone";
	return true;
}

bool resize(const std::string &name, const std::vector<uint8_t> &bytes, const TextureOperation &operation,
            std::vector<uint8_t> &out, std::string &words, std::string &why) {
	std::string size;
	if (!param(operation, "size", size)) {
		why = "a resize takes a size (pow2_down, pow2_up, <W>x<H> or fit:<W>x<H>)";
		return false;
	}
	// An 8-bit PCX's indices are data (a foliage map's codes): resized as indices, each texel the index of the
	// source texel under its middle, written with the file's own palette; never averaged and quantized again.
	IndexedImage8 indices;
	std::string ignored;
	if (indexed_pcx(name, bytes, indices, ignored)) {
		uint32_t width = 0, height = 0;
		if (!renderer::image_target_size(size, uint32_t(indices.width), uint32_t(indices.height), width, height, why)) return false;
		if (width == uint32_t(indices.width) && height == uint32_t(indices.height)) {
			why = "it is " + std::to_string(width) + " x " + std::to_string(height) + " already";
			return false;
		}
		IndexedImage8 sized;
		sized.width = int(width);
		sized.height = int(height);
		std::copy(&indices.palette[0][0], &indices.palette[0][0] + 256 * 3, &sized.palette[0][0]);
		sized.indices.resize(size_t(width) * height);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const uint32_t sx = std::min(uint32_t(indices.width) - 1, uint32_t((uint64_t(x) * 2 + 1) * uint32_t(indices.width) / (uint64_t(width) * 2)));
				const uint32_t sy = std::min(uint32_t(indices.height) - 1, uint32_t((uint64_t(y) * 2 + 1) * uint32_t(indices.height) / (uint64_t(height) * 2)));
				sized.indices[size_t(y) * width + x] = indices.indices[size_t(sy) * uint32_t(indices.width) + sx];
			}
		words = "Resized to " + std::to_string(width) + " x " + std::to_string(height) + ", its palette indices kept";
		return encode_pcx_indexed(sized, out, why);
	}
	renderer::ImageImportSettings settings;
	RgbaImage image;
	uint32_t width = 0, height = 0;
	if (!stored_settings(name, bytes, settings, why) || !game_texels(name, bytes, image, why) ||
	    !renderer::image_target_size(size, uint32_t(image.width), uint32_t(image.height), width, height, why))
		return false;
	if (width == uint32_t(image.width) && height == uint32_t(image.height)) {
		why = "it is " + std::to_string(width) + " x " + std::to_string(height) + " already";
		return false;
	}
	words = "Resized to " + std::to_string(width) + " x " + std::to_string(height);
	return encode(renderer::resize_image(image, width, height), settings, out, why);
}

bool alpha(const std::string &name, const std::vector<uint8_t> &bytes, const TextureOperation &operation,
           std::vector<uint8_t> &out, std::string &words, std::string &why) {
	std::string value;
	if (!param(operation, "alpha", value)) {
		why = "an alpha edit takes an alpha (opaque, luminance, invert, threshold:<n> or key:#RRGGBB)";
		return false;
	}
	renderer::ImageImportSettings settings;
	RgbaImage image;
	if (!stored_settings(name, bytes, settings, why) || !game_texels(name, bytes, image, why)) return false;
	if (!holds_alpha(settings)) {
		why = "it is stored as " + format_words(settings) + ", which holds no alpha" +
		      (settings.format == "tga24" ? ": store it as a 32-bit TGA first" : std::string());
		return false;
	}
	if (value == "invert") {
		for (size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = uint8_t(255 - image.pixels[i]);
	} else if (!renderer::apply_image_alpha(image, value, why)) {
		return false;
	}
	words = value == "invert" ? "Alpha inverted" : "Alpha: " + value;
	return encode(image, settings, out, why);
}

bool format(const std::string &name, const std::vector<uint8_t> &bytes, const TextureOperation &operation,
            std::vector<uint8_t> &out, std::string &words, std::string &why) {
	renderer::ImageImportSettings settings;
	RgbaImage image;
	if (!stored_settings(name, bytes, settings, why) || !game_texels(name, bytes, image, why)) return false;
	const renderer::ImageImportSettings was = settings;
	const std::string extension = extension_of(name);
	std::string value;
	// An 8-bit PCX's indices are data: storing it again from its colours would renumber them.
	IndexedImage8 indices;
	std::string ignored;
	const bool indexed = indexed_pcx(name, bytes, indices, ignored);
	if (indexed && param(operation, "palette", value)) {
		why = "it holds palette indices, which a palette chosen again would renumber (a foliage or char map's codes "
		      "are its indices): remap them instead";
		return false;
	}
	if (param(operation, "format", value)) {
		// The stored form within the name's extension: another extension is another file.
		const bool fits = (extension == ".tga" && (value == "tga" || value == "tga24")) ||
		                  (extension == ".pcx" && (value == "pcx" || value == "pcx24")) || ("." + value) == extension;
		if (!fits) {
			why = "a " + extension + " file holds " + extension.substr(1) +
			      ": for the file a " + value + " would be, rename it or set its import's format";
			return false;
		}
		settings.format = value;
	}
	if (param(operation, "dds", value)) settings.dds = value;
	if (param(operation, "mips", value)) settings.mips = value;
	if (param(operation, "palette", value)) settings.palette = value;
	if (settings.format != "dds" && (param(operation, "dds", value) || param(operation, "mips", value))) {
		why = "only a DDS takes a compression and mip levels";
		return false;
	}
	if (settings.format != "pcx" && param(operation, "palette", value)) {
		why = "only an 8-bit PCX takes a palette";
		return false;
	}
	if (settings.palette == "indices") {
		why = "palette indices keeps a source's indices, and the file's own are kept by any edit that leaves its texels";
		return false;
	}
	if (settings.format == was.format && settings.dds == was.dds && settings.mips == was.mips && settings.palette == was.palette) {
		why = "it is stored as " + format_words(settings) + " already";
		return false;
	}
	words = "Stored as " + format_words(settings) +
	        (indexed && settings.format == "pcx24" ? ": its palette indices are colours from now on" : std::string()) +
	        (was.format == "pcx24" && settings.format == "pcx" ? ": its colours are chosen down to 256" : std::string());
	return encode(image, settings, out, why);
}

bool reorder_rows(const std::string &name, const std::vector<uint8_t> &bytes, std::vector<uint8_t> &out, std::string &words,
                  std::string &why) {
	const TextureHeader header = texture_header(name, bytes);
	if (header.reader != TextureReader::Tga || !header.read) {
		why = "only a TGA the game reads stores its rows either way";
		return false;
	}
	if (!(header.tga_descriptor & 0x20)) {
		why = "its rows are stored bottom first already, as the game reads them";
		return false;
	}
	RgbaImage image;
	if (!game_texels(name, bytes, image, why)) return false;
	// The game took the top row as the bottom: the image its header meant is the game's turned over.
	const size_t row = size_t(image.width) * 4;
	for (int y = 0; y < image.height / 2; ++y)
		std::swap_ranges(image.pixels.begin() + std::ptrdiff_t(size_t(y) * row), image.pixels.begin() + std::ptrdiff_t(size_t(y + 1) * row),
		                 image.pixels.begin() + std::ptrdiff_t(size_t(image.height - 1 - y) * row));
	renderer::ImageImportSettings settings = renderer::image_import_settings({});
	settings.format = header.alpha ? "tga" : "tga24";
	if (extension_of(name) == ".mdt") settings.format = "mdt";
	words = "Rows saved bottom first";
	return encode(image, settings, out, why);
}

bool remap_palette(const std::string &name, const std::vector<uint8_t> &bytes, const TextureOperation &operation,
                   std::vector<uint8_t> &out, std::string &words, std::string &why) {
	if (extension_of(name) != ".pcx") {
		why = "only an 8-bit PCX holds palette indices";
		return false;
	}
	RgbaImage colours;
	PcxIndexed game;
	if (!decode_pcx_menu_rgba(bytes.data(), bytes.size(), colours, why, &game)) return false;
	if (!game.indexed) {
		why = "it is a 24-bit PCX, which holds colours, not indices";
		return false;
	}
	uint8_t map[256];
	for (int i = 0; i < 256; ++i) map[i] = uint8_t(i);
	std::string pairs;
	for (const auto &[from, to] : operation.params) {
		char *end = nullptr;
		const long a = std::strtol(from.c_str(), &end, 10);
		const bool from_ok = !from.empty() && end && !*end && a >= 0 && a <= 255;
		const long b = std::strtol(to.c_str(), &end, 10);
		if (!from_ok || to.empty() || !end || *end || b < 0 || b > 255) {
			why = "each remap is an index to an index, 0 to 255 ('" + from + "' to '" + to + "' is not)";
			return false;
		}
		map[a] = uint8_t(b);
		pairs += (pairs.empty() ? "" : ", ") + std::to_string(a) + " to " + std::to_string(b);
	}
	if (pairs.empty()) {
		why = "a remap takes the indices to move, each <from> = <to>";
		return false;
	}
	IndexedImage8 image;
	image.width = colours.width;
	image.height = colours.height;
	const size_t texels = size_t(colours.width) * size_t(colours.height);
	image.indices.assign(game.indices.begin(), game.indices.begin() + std::ptrdiff_t(std::min(texels, game.indices.size())));
	for (uint8_t &index : image.indices) index = map[index];
	std::copy(&game.palette[0][0], &game.palette[0][0] + 256 * 3, &image.palette[0][0]);
	words = "Palette indices " + pairs;
	return encode_pcx_indexed(image, out, why);
}

} // namespace

const char *texture_operation_token(TextureOperationKind kind) {
	return size_t(kind) < std::size(kRows) ? kRows[size_t(kind)].token : "";
}

bool texture_operation_kind(const std::string &token, TextureOperationKind &out) {
	for (const OperationRow &row : kRows)
		if (token == row.token) {
			out = row.kind;
			return true;
		}
	return false;
}

bool apply_texture_operation(const std::string &name, const std::vector<uint8_t> &bytes, const TextureOperation &operation,
                             std::vector<uint8_t> &out, std::string &words, std::string &why) {
	out.clear();
	if (layered_refused(name, bytes, why)) return false;
	switch (operation.kind) {
	case K::Resize: return resize(name, bytes, operation, out, words, why);
	case K::Alpha: return alpha(name, bytes, operation, out, words, why);
	case K::Format: return format(name, bytes, operation, out, words, why);
	case K::ReorderRows: return reorder_rows(name, bytes, out, words, why);
	case K::RemapPalette: return remap_palette(name, bytes, operation, out, words, why);
	case K::kCount: break;
	}
	why = "no such operation";
	return false;
}

} // namespace opennova::editor
