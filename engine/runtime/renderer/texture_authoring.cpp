// The image import's pieces (texture_authoring.h): a source read as an image program reads it, resized,
// its green and alpha made, then written as the file the game reads. Tooling, not a port, but for the
// lines cited.
#include <runtime/renderer/texture_authoring.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>

#include <base/io/strutil.h>
#include <base/resource_index/resource_kind.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/pcx/pcx_quantize.h>
#include <formats/png/png_decode.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>
#include <formats/tga/tga_source.h>
#include <runtime/renderer/dxt_encode.h>
#include <runtime/renderer/texture_load_rules.h>

namespace opennova::renderer {

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

bool image_alpha_form(const std::string &value) {
	uint32_t n = 0;
	uint8_t rgb[3];
	if (value.rfind("threshold:", 0) == 0) return digits(value, 10, value.size(), n) && n <= 255;
	if (value.rfind("key:", 0) == 0) return hex_colour(value.substr(4), rgb);
	return false;
}

bool image_size_form(const std::string &value) {
	uint32_t w = 0, h = 0;
	if (value.rfind("fit:", 0) == 0) return sides_of(value.substr(4), w, h);
	return sides_of(value, w, h);
}

ImageImportSettings image_import_settings(const std::map<std::string, std::string> &options) {
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
	const std::string extension = resource_extension_for_name(name);
	if (extension == ".png") return png::decode_png(bytes, out.image, error);
	if (extension == ".tga") {
		// The format's own decode (formats/tga tga_source.h): its origin honoured, every depth and colour map.
		tga::TgaImage image;
		if (!tga::decode_tga_source(bytes.data(), bytes.size(), image, error)) return false;
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

bool image_source_has_alpha(const std::string &name, const std::vector<uint8_t> &head) {
	const std::string extension = resource_extension_for_name(name);
	if (extension == ".png") {
		if (head.size() < 33 || !png::is_png(head)) return false;
		const uint8_t colour = head[25]; // IHDR's colour type
		if (colour == 4 || colour == 6) return true;
		// A transparency chunk before the image data.
		size_t at = 8;
		while (at + 8 <= head.size()) {
			const uint32_t length = (uint32_t(head[at]) << 24) | (uint32_t(head[at + 1]) << 16) | (uint32_t(head[at + 2]) << 8) | head[at + 3];
			const std::string type(head.begin() + std::ptrdiff_t(at + 4), head.begin() + std::ptrdiff_t(at + 8));
			if (type == "tRNS") return true;
			if (type == "IDAT" || type == "IEND") return false;
			at += 12 + size_t(length);
		}
		return false;
	}
	if (extension == ".tga") {
		if (head.size() < 18) return false;
		const uint8_t map_bits = head[7], bits = head[16], alpha_bits = head[17] & 0x0F;
		return alpha_bits > 0 || bits == 32 || (head[1] == 1 && map_bits == 32);
	}
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
	if (width * 2 == sw && height * 2 == sh) {
		// Halved: each texel the 2 x 2 box's sum shifted down by two, as the game halves a texture
		// (halve_rgba [orig: GTexture_Downsample2x2_RGBA8 @ 0x687000]; at an exact half its stride is the
		// source row's).
		out.pixels = image.pixels;
		uint32_t halved_width = sw, halved_height = sh;
		halve_rgba(out.pixels, halved_width, halved_height);
		return out;
	}
	out.pixels.assign(size_t(width) * height * 4, 0);
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
		out = png::encode_png_rgba(image.pixels.data(), w, h);
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
		// The authoring encoder (dxt_encode.h): rgbcx's blocks, each level the box filter of the source's.
		const std::vector<std::vector<uint8_t>> blocks = encode_dxt_levels(image.pixels.data(), w, h, dxt5, settings.mips == "full");
		return dds::dds_write_dxt(dds::dds_fourcc('D', 'X', 'T', dxt5 ? '5' : '1'), w, h, blocks, out, why);
	}
	why = "the format '" + format + "' is none of tga, tga24, pcx, pcx24, dds, mdt or png";
	return false;
}

bool image_import_texels(RgbaImage &image, const ImageImportSettings &settings, std::string &why, std::string &field) {
	uint32_t width = 0, height = 0;
	if (!image_target_size(settings.size, uint32_t(image.width), uint32_t(image.height), width, height, why)) {
		field = "size";
		return false;
	}
	image = resize_image(image, width, height);
	if (settings.green == "flip") flip_image_green(image);
	if (settings.format == "tga" && settings.normal == "height") height_into_alpha(image);
	else if (settings.format != "tga24" && settings.format != "pcx" && settings.format != "pcx24" &&
	         !apply_image_alpha(image, settings.alpha, why)) {
		field = "alpha";
		return false;
	}
	return true;
}

} // namespace opennova::renderer
