#include <editor/import/texture_import.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>

#include <base/io/strutil.h>
#include <editor/import/import_context.h>
#include <editor/import/png_decode.h>
#include <editor/import/png_encode.h>
#include <editor/import/quantize.h>
#include <editor/import/tga_source.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/texture_dxt.h>

namespace opennova::editor {

namespace {

bool digits(const std::string &text, size_t from, size_t to, uint32_t &out) {
	if (from >= to || to - from > 5) return false;
	uint32_t value = 0;
	for (size_t i = from; i < to; ++i) {
		if (text[i] < '0' || text[i] > '9') return false;
		value = value * 10 + uint32_t(text[i] - '0');
	}
	out = value;
	return true;
}

// "<W>x<H>", each side 1..16384.
bool sides_of(const std::string &text, uint32_t &width, uint32_t &height) {
	const size_t x = text.find('x');
	if (x == std::string::npos || !digits(text, 0, x, width) || !digits(text, x + 1, text.size(), height)) return false;
	return width >= 1 && height >= 1 && width <= 16384 && height <= 16384;
}

bool hex_colour(const std::string &text, uint8_t rgb[3]) {
	if (text.size() != 7 || text[0] != '#') return false;
	for (int c = 0; c < 3; ++c) {
		const std::string pair = text.substr(size_t(1 + c * 2), 2);
		char *end = nullptr;
		const long value = std::strtol(pair.c_str(), &end, 16);
		if (!end || *end || !std::isxdigit(uint8_t(pair[0])) || !std::isxdigit(uint8_t(pair[1]))) return false;
		rgb[c] = uint8_t(value);
	}
	return true;
}

bool alpha_form(const std::string &value) {
	uint32_t n = 0;
	uint8_t rgb[3];
	if (value.rfind("threshold:", 0) == 0) return digits(value, 10, value.size(), n) && n <= 255;
	if (value.rfind("key:", 0) == 0) return hex_colour(value.substr(4), rgb);
	return false;
}

bool size_form(const std::string &value) {
	uint32_t w = 0, h = 0;
	if (value.rfind("fit:", 0) == 0) return sides_of(value.substr(4), w, h);
	return sides_of(value, w, h);
}

// A file name of the format's extension: no folder, at most the archives' 16 characters.
bool name_form(const std::string &value) {
	return !value.empty() && value.size() <= 16 && value.find_first_of("/\\:") == std::string::npos &&
	       value.find('.') != std::string::npos;
}

uint32_t power_of_two_floor(uint32_t side) {
	uint32_t out = 1;
	while (out * 2 <= side && out < 0x8000u) out *= 2;
	return out;
}

uint32_t power_of_two_ceil(uint32_t side) {
	uint32_t out = 1;
	while (out < side && out < 0x8000u) out *= 2;
	return out;
}

bool translucent(const RgbaImage &image) {
	for (size_t i = 3; i < image.pixels.size(); i += 4)
		if (image.pixels[i] != 255) return true;
	return false;
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
		alpha.accepts = alpha_form;
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
		size.accepts = size_form;
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

ImageImportSettings image_import_settings(const ImportOptions &options) {
	const auto value = [&](const char *key, const char *fallback) {
		const auto found = options.find(key);
		return found == options.end() || found->second.empty() ? std::string(fallback) : strutil::to_lower(found->second);
	};
	ImageImportSettings out;
	out.format = value("format", "tga");
	const auto name = options.find("name");
	out.name = name == options.end() ? std::string() : name->second;
	out.alpha = value("alpha", "source");
	out.size = value("size", "source");
	out.palette = value("palette", "median_cut");
	out.dds = value("dds", "dxt5");
	out.mips = value("mips", "full");
	out.green = value("green", "game");
	out.normal = value("normal", "normal");
	return out;
}

bool decode_image_source(const std::string &name, const std::vector<uint8_t> &bytes, ImageSource &out,
                         std::string &error) {
	out = ImageSource();
	const std::string extension = strutil::to_lower(utf8_of(path_of(name).extension()));
	if (extension == ".png") return decode_png(bytes, out.image, error);
	if (extension == ".tga") {
		// The format's own decode (import/tga_source.h): its origin honoured, every depth and colour map.
		tga::TgaImage image;
		if (!decode_tga_source(bytes.data(), bytes.size(), image, error)) return false;
		out.image.width = image.width;
		out.image.height = image.height;
		out.image.pixels = std::move(image.rgba);
		return true;
	}
	if (extension == ".pcx") {
		// The format's own decode (formats/pcx: rows of their bytes a line, the first `width` of each kept),
		// an 8-bit image's indices and palette beside its colours.
		if (bytes.size() > 65 && bytes[65] == 1) {
			if (!decode_pcx_indexed(bytes.data(), bytes.size(), out.indices, error)) return false;
			out.indexed = true;
			const size_t texels = size_t(out.indices.width) * size_t(out.indices.height);
			out.image.width = out.indices.width;
			out.image.height = out.indices.height;
			out.image.pixels.resize(texels * 4);
			for (size_t i = 0; i < texels; ++i) {
				const uint8_t *entry = out.indices.palette[out.indices.indices[i]];
				out.image.pixels[i * 4] = entry[0];
				out.image.pixels[i * 4 + 1] = entry[1];
				out.image.pixels[i * 4 + 2] = entry[2];
				out.image.pixels[i * 4 + 3] = 255;
			}
			return true;
		}
		RgbImage rgb;
		if (!decode_pcx_rgb(bytes.data(), bytes.size(), rgb, error)) return false;
		const size_t texels = size_t(rgb.width) * size_t(rgb.height);
		out.image.width = rgb.width;
		out.image.height = rgb.height;
		out.image.pixels.resize(texels * 4);
		for (size_t i = 0; i < texels; ++i) {
			out.image.pixels[i * 4] = rgb.pixels[i * 3];
			out.image.pixels[i * 4 + 1] = rgb.pixels[i * 3 + 1];
			out.image.pixels[i * 4 + 2] = rgb.pixels[i * 3 + 2];
			out.image.pixels[i * 4 + 3] = 255;
		}
		return true;
	}
	error = name + " is no image the importer reads (a PNG, a TGA or a PCX)";
	return false;
}

void height_into_alpha(RgbaImage &image) {
	uint8_t *p = image.pixels.data();
	for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
		const uint8_t height = uint8_t((85u * (uint32_t(p[i]) + p[i + 1] + p[i + 2])) >> 8);
		p[i + 2] = p[i + 3];
		p[i] = p[i + 1] = p[i + 3] = height;
	}
}

std::string image_format_extension(const std::string &format) {
	if (format == "tga24") return ".tga";
	if (format == "pcx24") return ".pcx";
	return "." + format;
}

std::string image_import_output_name(const std::string &source_name, const ImageImportSettings &settings) {
	if (!settings.name.empty()) return settings.name;
	return utf8_of(path_of(source_name).stem()) + image_format_extension(settings.format);
}

bool image_target_size(const std::string &size, uint32_t width, uint32_t height, uint32_t &out_width,
                       uint32_t &out_height, std::string &why) {
	out_width = width;
	out_height = height;
	if (size == "source") return true;
	if (size == "pow2_down") {
		out_width = power_of_two_floor(width);
		out_height = power_of_two_floor(height);
		return true;
	}
	if (size == "pow2_up") {
		out_width = power_of_two_ceil(width);
		out_height = power_of_two_ceil(height);
		return true;
	}
	uint32_t w = 0, h = 0;
	if (size.rfind("fit:", 0) == 0 && sides_of(size.substr(4), w, h)) {
		// The largest size inside w x h of the image's shape (never larger than the image).
		const double scale = std::min({double(w) / double(width), double(h) / double(height), 1.0});
		out_width = std::max(1u, uint32_t(double(width) * scale + 0.5));
		out_height = std::max(1u, uint32_t(double(height) * scale + 0.5));
		return true;
	}
	if (sides_of(size, w, h)) {
		out_width = w;
		out_height = h;
		return true;
	}
	why = "the size '" + size + "' is none of source, pow2_down, pow2_up, <W>x<H> or fit:<W>x<H>";
	return false;
}

RgbaImage resize_image(const RgbaImage &image, uint32_t width, uint32_t height) {
	if (image.empty() || (uint32_t(image.width) == width && uint32_t(image.height) == height)) return image;
	const uint32_t sw = uint32_t(image.width), sh = uint32_t(image.height);
	RgbaImage out;
	out.width = int(width);
	out.height = int(height);
	out.pixels.assign(size_t(width) * height * 4, 0);
	if (width * 2 == sw && height * 2 == sh) {
		// Halved: each texel the 2 x 2 box's sum shifted down by two, as the game halves a texture.
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				for (int c = 0; c < 4; ++c) {
					const auto at = [&](uint32_t sx, uint32_t sy) {
						return uint32_t(image.pixels[(size_t(sy) * sw + sx) * 4 + size_t(c)]);
					};
					const uint32_t sum = at(x * 2, y * 2) + at(x * 2 + 1, y * 2) + at(x * 2, y * 2 + 1) + at(x * 2 + 1, y * 2 + 1);
					out.pixels[(size_t(y) * width + x) * 4 + size_t(c)] = uint8_t(sum >> 2);
				}
		return out;
	}
	// Any other size: each texel the average of the source texels its footprint covers, weighted by how
	// much of each it covers (a footprint smaller than a texel takes the texel under it).
	const double fx = double(sw) / double(width), fy = double(sh) / double(height);
	for (uint32_t y = 0; y < height; ++y) {
		const double y0 = double(y) * fy, y1 = std::min(double(sh), y0 + fy);
		for (uint32_t x = 0; x < width; ++x) {
			const double x0 = double(x) * fx, x1 = std::min(double(sw), x0 + fx);
			double sum[4] = {0, 0, 0, 0}, weight = 0;
			for (uint32_t sy = uint32_t(y0); sy < sh && double(sy) < y1; ++sy) {
				const double wy = std::min(y1, double(sy) + 1.0) - std::max(y0, double(sy));
				if (wy <= 0) continue;
				for (uint32_t sx = uint32_t(x0); sx < sw && double(sx) < x1; ++sx) {
					const double wx = std::min(x1, double(sx) + 1.0) - std::max(x0, double(sx));
					if (wx <= 0) continue;
					const uint8_t *p = &image.pixels[(size_t(sy) * sw + sx) * 4];
					for (int c = 0; c < 4; ++c) sum[c] += double(p[c]) * wx * wy;
					weight += wx * wy;
				}
			}
			for (int c = 0; c < 4; ++c)
				out.pixels[(size_t(y) * width + x) * 4 + size_t(c)] =
				        uint8_t(std::clamp(weight > 0 ? sum[c] / weight + 0.5 : 0.0, 0.0, 255.0));
		}
	}
	return out;
}

bool apply_image_alpha(RgbaImage &image, const std::string &alpha, std::string &why) {
	uint8_t *p = image.pixels.data();
	const size_t texels = image.pixels.size() / 4;
	if (alpha == "source") return true;
	if (alpha == "opaque") {
		for (size_t i = 0; i < texels; ++i) p[i * 4 + 3] = 255;
		return true;
	}
	if (alpha == "luminance") {
		// [orig: Texture_LoadFromArchive @ 0x58BC35..0x58BCA9: (85 x (r + g + b)) >> 8 of each palette entry]
		for (size_t i = 0; i < texels; ++i)
			p[i * 4 + 3] = uint8_t((85u * (uint32_t(p[i * 4]) + p[i * 4 + 1] + p[i * 4 + 2])) >> 8);
		return true;
	}
	uint32_t level = 0;
	if (alpha.rfind("threshold:", 0) == 0 && digits(alpha, 10, alpha.size(), level) && level <= 255) {
		// Opaque where a cut-out keeps the texel, a > ref [orig: CRenderBatchQueue_FlushBatches @ 0x5DA3A9].
		for (size_t i = 0; i < texels; ++i) p[i * 4 + 3] = p[i * 4 + 3] > level ? 255 : 0;
		return true;
	}
	uint8_t key[3];
	if (alpha.rfind("key:", 0) == 0 && hex_colour(alpha.substr(4), key)) {
		for (size_t i = 0; i < texels; ++i)
			p[i * 4 + 3] = p[i * 4] == key[0] && p[i * 4 + 1] == key[1] && p[i * 4 + 2] == key[2] ? 0 : 255;
		return true;
	}
	why = "the alpha '" + alpha + "' is none of source, opaque, luminance, threshold:<0..255> or key:#RRGGBB";
	return false;
}

void flip_image_green(RgbaImage &image) {
	for (size_t i = 1; i < image.pixels.size(); i += 4) image.pixels[i] = uint8_t(255 - image.pixels[i]);
}

bool encode_image(const RgbaImage &image, const ImageImportSettings &settings, std::vector<uint8_t> &out,
                  std::string &why, std::string &note) {
	const uint32_t w = uint32_t(image.width), h = uint32_t(image.height);
	const std::string &format = settings.format;
	if (format == "tga" || format == "mdt") return tga::tga_write_rgba32(image.pixels.data(), w, h, out, why);
	if (format == "tga24") {
		if (translucent(image)) note = "a 24-bit TGA carries no alpha: the transparency is dropped (format tga keeps it).";
		return tga::tga_write_rgb24(image.pixels.data(), w, h, out, why);
	}
	if (format == "pcx24") {
		if (translucent(image)) note = "a PCX carries no alpha: the transparency is dropped (format tga keeps it).";
		RgbImage rgb;
		rgb.width = image.width;
		rgb.height = image.height;
		rgb.pixels.reserve(size_t(w) * h * 3);
		for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) rgb.pixels.insert(rgb.pixels.end(), {image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]});
		return encode_pcx_rgb(rgb, out, why);
	}
	if (format == "png") {
		out = encode_png_rgba(image.pixels.data(), w, h);
		if (out.empty()) why = "the PNG writer made nothing of it";
		return !out.empty();
	}
	if (format == "pcx") {
		if (settings.palette != "median_cut" && settings.palette != "exact") {
			why = "the palette '" + settings.palette + "' is neither median_cut nor exact";
			return false;
		}
		if (settings.palette == "exact") {
			std::vector<uint32_t> colours;
			for (size_t i = 0; i + 3 < image.pixels.size(); i += 4)
				colours.push_back(uint32_t(image.pixels[i]) << 16 | uint32_t(image.pixels[i + 1]) << 8 | image.pixels[i + 2]);
			std::sort(colours.begin(), colours.end());
			const size_t distinct = size_t(std::unique(colours.begin(), colours.end()) - colours.begin());
			if (distinct > 256) {
				why = "it holds " + std::to_string(distinct) + " colours, more than a PCX's 256 (palette exact)";
				return false;
			}
		}
		if (translucent(image)) note = "a PCX carries no alpha: the transparency is dropped (format tga keeps it).";
		return encode_pcx_indexed(quantize_to_256(image), out, why);
	}
	if (format == "dds") {
		if (settings.dds == "argb") return dds::dds_write_a8r8g8b8(image.pixels.data(), w, h, out, why);
		if (settings.dds != "dxt5" && settings.dds != "dxt1") {
			why = "the compression '" + settings.dds + "' is none of dxt5, dxt1 or argb";
			return false;
		}
		if (settings.mips != "full" && settings.mips != "none") {
			why = "the mip levels '" + settings.mips + "' are neither full nor none";
			return false;
		}
		const bool dxt5 = settings.dds == "dxt5";
		uint32_t levels = 1;
		if (settings.mips == "full")
			for (uint32_t side = std::max(w, h); side > 1; side >>= 1) ++levels;
		const std::vector<renderer::DxtSurface> chain = renderer::build_dxt_texture_levels(
		        image.pixels.data(), w, h, dxt5 ? renderer::TextureDxtFormat::Dxt5 : renderer::TextureDxtFormat::Dxt1, levels);
		std::vector<std::vector<uint8_t>> blocks;
		for (const renderer::DxtSurface &level : chain) blocks.push_back(level.blocks);
		return dds::dds_write_dxt(dds::dds_fourcc('D', 'X', 'T', dxt5 ? '5' : '1'), w, h, blocks, out, why);
	}
	why = "the format '" + format + "' is none of tga, tga24, pcx, pcx24, dds, mdt or png";
	return false;
}

bool run_image_import(ImportContext &context, ImportProduct &out) {
	const std::string &source_name = context.source_name();
	const ImageImportSettings settings = image_import_settings(context.options());
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
	if (strutil::to_lower(utf8_of(path_of(name).extension())) != image_format_extension(settings.format))
		return refuse(CoreFinding::ImportOption,
		              "The file name " + name + " does not end in " + image_format_extension(settings.format) +
		                      ", the extension of the format " + settings.format + ".",
		              "name");
	ImageSource source;
	std::string error;
	if (!decode_image_source(source_name, context.source(), source, error)) return refuse(CoreFinding::ImportDecode, error);
	RgbaImage &image = source.image;
	uint32_t width = 0, height = 0;
	if (!image_target_size(settings.size, uint32_t(image.width), uint32_t(image.height), width, height, error))
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
	image = resize_image(image, width, height);
	if (settings.green == "flip") flip_image_green(image);
	if (settings.format == "tga" && settings.normal == "height") height_into_alpha(image);
	else if (settings.format != "tga24" && settings.format != "pcx" && settings.format != "pcx24" &&
	         !apply_image_alpha(image, settings.alpha, error))
		return refuse(CoreFinding::ImportOption, "The image importer cannot use " + error + ".", "alpha");
	std::string note;
	if (!encode_image(image, settings, output.bytes, error, note))
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
