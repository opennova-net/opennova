#include <editor/documents/texture_compare.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <editor/import/texture_import.h>
#include <runtime/renderer/texture_dxt.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

double psnr(double mse) {
	if (mse <= 0.0) return kTexturePsnrExact;
	return std::min(kTexturePsnrExact, 10.0 * std::log10(255.0 * 255.0 / mse));
}

// What an image's alpha holds over its first level: none (every texel 255), on or off, or graded.
TextureAlpha alpha_of(const TextureLevel &level) {
	bool partial = false, graded = false;
	for (size_t i = 3; i < level.rgba.size(); i += 4) {
		const uint8_t a = level.rgba[i];
		if (a != 255) partial = true;
		if (a != 0 && a != 255) graded = true;
	}
	return graded ? TextureAlpha::Graded : partial ? TextureAlpha::Mask : TextureAlpha::None;
}

// The reference's chain: its first level, then each the D3DX box filter of the one before, to 1 x 1 (the
// chain the importer's full mips make).
std::shared_ptr<TextureImage> reference_chain(const TextureLevel &first) {
	auto out = std::make_shared<TextureImage>();
	out->loads = true;
	out->decoded = true;
	out->levels.push_back(first);
	uint32_t w = first.width, h = first.height;
	std::vector<renderer::DxtColor> colours = renderer::decode_rgba8(first.rgba.data(), w, h);
	while (w > 1 || h > 1) {
		colours = renderer::box_filter_half(colours, w, h);
		w = std::max(1u, w / 2);
		h = std::max(1u, h / 2);
		TextureLevel level;
		level.width = w;
		level.height = h;
		level.rgba = renderer::encode_rgba8(colours);
		out->levels.push_back(std::move(level));
	}
	out->alpha = alpha_of(first);
	return out;
}

// The reference written as a `.dds` of `dds` (dxt5 or dxt1) with its full chain, read back as the game reads it.
bool compress(const TextureLevel &first, const std::string &dds, TextureCompression &out) {
	RgbaImage image;
	image.width = int(first.width);
	image.height = int(first.height);
	image.pixels = first.rgba;
	renderer::ImageImportSettings settings;
	settings.format = "dds";
	settings.dds = dds;
	settings.mips = "full";
	std::vector<uint8_t> bytes;
	std::string why, note;
	if (!renderer::encode_image(image, settings, bytes, why, note)) {
		out.why = "It does not write as a " + dds + " .dds: " + why;
		return false;
	}
	out.file_bytes = bytes.size();
	out.compressed = decode_texture("compare.dds", bytes);
	if (!out.compressed || !out.compressed->loads || !out.compressed->decoded) {
		out.why = "The .dds written of it does not read back.";
		return false;
	}
	return true;
}

// Each level of the compressed texture against the reference's of the same sides.
void weigh(TextureCompression &out) {
	out.errors.clear();
	for (const TextureLevel &level : out.compressed->levels)
		for (const TextureLevel &reference : out.reference->levels)
			if (reference.width == level.width && reference.height == level.height) {
				out.errors.push_back(texture_level_error(reference, level));
				break;
			}
}

void copy_texel(const TextureLevel &from, TextureLevel &to, size_t at) {
	if (at * 4 + 3 < from.rgba.size() && at * 4 + 3 < to.rgba.size())
		std::copy(from.rgba.begin() + std::ptrdiff_t(at * 4), from.rgba.begin() + std::ptrdiff_t(at * 4 + 4),
		          to.rgba.begin() + std::ptrdiff_t(at * 4));
}

} // namespace

TextureLevelError texture_level_error(const TextureLevel &reference, const TextureLevel &test) {
	TextureLevelError out;
	if (reference.width != test.width || reference.height != test.height || reference.rgba.size() != test.rgba.size() ||
	    reference.rgba.size() != size_t(reference.width) * reference.height * 4)
		return out;
	out.width = reference.width;
	out.height = reference.height;
	const uint32_t w = reference.width, h = reference.height;
	double sum_rgb = 0.0, sum_alpha = 0.0;
	const uint32_t blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
	std::vector<double> block_sum(size_t(blocks_x) * blocks_y, 0.0);
	std::vector<uint32_t> block_count(block_sum.size(), 0);
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x) {
			const size_t at = (size_t(y) * w + x) * 4;
			double texel = 0.0;
			for (int c = 0; c < 3; ++c) {
				const int d = std::abs(int(reference.rgba[at + c]) - int(test.rgba[at + c]));
				out.max_rgb = std::max<uint8_t>(out.max_rgb, uint8_t(d));
				texel += double(d) * d;
			}
			const int da = std::abs(int(reference.rgba[at + 3]) - int(test.rgba[at + 3]));
			out.max_alpha = std::max<uint8_t>(out.max_alpha, uint8_t(da));
			sum_rgb += texel;
			sum_alpha += double(da) * da;
			const size_t block = size_t(y / 4) * blocks_x + x / 4;
			block_sum[block] += texel;
			++block_count[block];
		}
	const double texels = double(w) * h;
	out.rms_rgb = std::sqrt(sum_rgb / (texels * 3.0));
	out.rms_alpha = std::sqrt(sum_alpha / texels);
	out.psnr_rgb = psnr(sum_rgb / (texels * 3.0));
	out.psnr_alpha = psnr(sum_alpha / texels);
	for (size_t block = 0; block < block_sum.size(); ++block) {
		const double rms = block_count[block] ? std::sqrt(block_sum[block] / (double(block_count[block]) * 3.0)) : 0.0;
		if (rms > out.worst_rms) {
			out.worst_rms = rms;
			out.worst_x = uint32_t(block % blocks_x) * 4;
			out.worst_y = uint32_t(block / blocks_x) * 4;
		}
	}
	return out;
}

TextureCompression compress_texture(const TextureImage &image, const std::string &dds) {
	TextureCompression out;
	if (!image.loads || !image.decoded || image.levels.empty() || image.levels.front().rgba.empty()) {
		out.why = image.loads ? "The editor does not decode its texels." : "The game cannot load it.";
		return out;
	}
	out.against = "its own texels";
	out.reference = reference_chain(image.levels.front());
	const std::string format = !dds.empty() ? dds : out.reference->alpha != TextureAlpha::None ? "dxt5" : "dxt1";
	out.format = format == "dxt1" ? "DXT1" : "DXT5";
	if (!compress(image.levels.front(), format, out)) return out;
	weigh(out);
	out.made = true;
	return out;
}

TextureCompression compare_dds(const std::shared_ptr<const TextureImage> &image, const RgbaImage &source,
                               const std::string &against) {
	TextureCompression out;
	out.against = against;
	if (!image || !image->loads || !image->decoded || image->levels.empty()) {
		out.why = "The .dds does not read.";
		return out;
	}
	if (source.empty() || uint32_t(source.width) != image->width() || uint32_t(source.height) != image->height()) {
		out.why = "Its source, as its import prepares it, is " + std::to_string(source.width) + " x " +
		          std::to_string(source.height) + ", the .dds " + std::to_string(image->width()) + " x " +
		          std::to_string(image->height()) + ".";
		return out;
	}
	TextureLevel first;
	first.width = uint32_t(source.width);
	first.height = uint32_t(source.height);
	first.rgba = source.pixels;
	out.reference = reference_chain(first);
	out.compressed = image;
	// The DDS's format, as its texels' fact names it ("DXT5: 8 bits a texel, ...").
	const TextureFact *texels = image->fact("texels");
	out.format = texels ? texels->words.substr(0, texels->words.find(':')) : std::string("DDS");
	weigh(out);
	out.made = true;
	return out;
}

const char *texture_compare_view_token(TextureCompareView view) {
	switch (view) {
	case TextureCompareView::Off: return "off";
	case TextureCompareView::Split: return "split";
	case TextureCompareView::Compressed: return "dds";
	case TextureCompareView::Difference: return "difference";
	}
	return "off";
}

bool texture_compare_view_from_token(const std::string &token, TextureCompareView &out) {
	for (const TextureCompareView view : {TextureCompareView::Off, TextureCompareView::Split, TextureCompareView::Compressed,
	                                      TextureCompareView::Difference})
		if (token == texture_compare_view_token(view)) {
			out = view;
			return true;
		}
	return false;
}

std::shared_ptr<const TextureImage> texture_compare_picture(const TextureCompression &compression, TextureCompareView view,
                                                            float split) {
	if (view == TextureCompareView::Off || !compression.made) return nullptr;
	if (view == TextureCompareView::Compressed) return compression.compressed;
	auto out = std::make_shared<TextureImage>(*compression.compressed);
	out->palette.clear();
	out->indices.clear();
	out->luminance.clear();
	split = std::clamp(split, 0.0f, 1.0f);
	for (TextureLevel &level : out->levels) {
		const TextureLevel *reference = nullptr;
		for (const TextureLevel &each : compression.reference->levels)
			if (each.width == level.width && each.height == level.height) reference = &each;
		if (!reference) continue;
		if (view == TextureCompareView::Split) {
			// The reference left of the split's column, the compressed from it on.
			const uint32_t cut = uint32_t(std::lround(split * float(level.width)));
			for (uint32_t y = 0; y < level.height; ++y)
				for (uint32_t x = 0; x < std::min(cut, level.width); ++x) copy_texel(*reference, level, size_t(y) * level.width + x);
			continue;
		}
		for (size_t i = 0; i + 3 < level.rgba.size() && i + 3 < reference->rgba.size(); i += 4) {
			for (size_t c = 0; c < 3; ++c)
				level.rgba[i + c] = uint8_t(std::min(255, std::abs(int(level.rgba[i + c]) - int(reference->rgba[i + c])) *
				                                                 kTextureDifferenceGain));
			level.rgba[i + 3] =
			        uint8_t(255 - std::min(255, std::abs(int(level.rgba[i + 3]) - int(reference->rgba[i + 3])) * kTextureDifferenceGain));
		}
	}
	return out;
}

io::JsonValue texture_compression_json(const TextureCompression &compression) {
	JsonValue out = JsonValue::make_object();
	out.set("made", JsonValue::make_bool(compression.made));
	out.set("why", json_string(compression.why));
	out.set("against", json_string(compression.against));
	out.set("format", json_string(compression.format));
	out.set("file_bytes", json_number(double(compression.file_bytes)));
	JsonValue levels = JsonValue::make_array();
	for (size_t i = 0; i < compression.errors.size(); ++i) {
		const TextureLevelError &error = compression.errors[i];
		JsonValue level = JsonValue::make_object();
		level.set("level", json_number(double(i)));
		level.set("width", json_number(double(error.width)));
		level.set("height", json_number(double(error.height)));
		level.set("psnr_rgb", json_number(error.psnr_rgb));
		level.set("psnr_alpha", json_number(error.psnr_alpha));
		level.set("rms_rgb", json_number(error.rms_rgb));
		level.set("rms_alpha", json_number(error.rms_alpha));
		level.set("max_rgb", json_number(error.max_rgb));
		level.set("max_alpha", json_number(error.max_alpha));
		JsonValue worst = JsonValue::make_object();
		worst.set("x", json_number(double(error.worst_x)));
		worst.set("y", json_number(double(error.worst_y)));
		worst.set("rms", json_number(error.worst_rms));
		level.set("worst_block", std::move(worst));
		levels.push(std::move(level));
	}
	out.set("levels", std::move(levels));
	return out;
}

std::string texture_level_error_words(const TextureLevelError &error, bool alpha) {
	char text[200];
	if (alpha)
		std::snprintf(text, sizeof(text), "PSNR %.1f dB colour, %.1f dB alpha; largest error %u, alpha %u; worst block at %u, %u",
		              error.psnr_rgb, error.psnr_alpha, unsigned(error.max_rgb), unsigned(error.max_alpha), error.worst_x,
		              error.worst_y);
	else
		std::snprintf(text, sizeof(text), "PSNR %.1f dB colour; largest error %u; worst block at %u, %u", error.psnr_rgb,
		              unsigned(error.max_rgb), error.worst_x, error.worst_y);
	return text;
}

} // namespace opennova::editor
