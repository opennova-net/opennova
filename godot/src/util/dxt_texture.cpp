#include "util/dxt_texture.h"

#include <godot_cpp/classes/image_texture.hpp>

#include <algorithm>
#include <cstring>

namespace godot {

namespace {

// The retail levels, checked, then the box chain's tail to 1x1.
std::vector<opennova::renderer::DxtSurface> complete_chain(
		const std::vector<opennova::renderer::DxtSurface> &p_levels) {
	using opennova::renderer::DxtSurface;
	if (p_levels.empty() || !p_levels.front().is_valid()) {
		return {};
	}
	std::vector<DxtSurface> chain = p_levels;
	uint32_t expected_width = chain.front().width;
	uint32_t expected_height = chain.front().height;
	for (const DxtSurface &level : chain) {
		if (!level.is_valid() || level.format != chain.front().format ||
				level.width != expected_width || level.height != expected_height) {
			return {};
		}
		expected_width = std::max(1u, expected_width >> 1);
		expected_height = std::max(1u, expected_height >> 1);
	}
	while (chain.back().width > 1 || chain.back().height > 1) {
		const DxtSurface last = chain.back();
		chain.push_back(opennova::renderer::encode_dxt_surface(
				opennova::renderer::box_filter_half(
						opennova::renderer::decode_dxt_surface(last), last.width, last.height),
				std::max(1u, last.width >> 1), std::max(1u, last.height >> 1), last.format));
	}
	return chain;
}

} // namespace

PackedByteArray packed_dxt_levels(const std::vector<opennova::renderer::DxtSurface> &p_levels) {
	std::size_t total = 0;
	for (const opennova::renderer::DxtSurface &level : p_levels) {
		total += level.blocks.size();
	}
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(total));
	std::size_t at = 0;
	for (const opennova::renderer::DxtSurface &level : p_levels) {
		if (!level.blocks.empty()) {
			std::memcpy(bytes.ptrw() + at, level.blocks.data(), level.blocks.size());
		}
		at += level.blocks.size();
	}
	return bytes;
}

Ref<Image> image_from_dxt_levels(const std::vector<opennova::renderer::DxtSurface> &p_levels) {
	const std::vector<opennova::renderer::DxtSurface> chain = complete_chain(p_levels);
	if (chain.empty()) {
		return Ref<Image>();
	}
	const Image::Format format =
			chain.front().format == opennova::renderer::TextureDxtFormat::Dxt1
					? Image::FORMAT_DXT1
					: Image::FORMAT_DXT5;
	Ref<Image> image = Image::create_from_data(static_cast<int>(chain.front().width),
			static_cast<int>(chain.front().height), true, format, packed_dxt_levels(chain));
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}
	return image;
}

Ref<Texture2D> texture_from_dxt_levels(const std::vector<opennova::renderer::DxtSurface> &p_levels) {
	const Ref<Image> image = image_from_dxt_levels(p_levels);
	if (image.is_null()) {
		return Ref<Texture2D>();
	}
	return ImageTexture::create_from_image(image);
}

} // namespace godot
