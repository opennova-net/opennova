#include "particle/particle_atlas_page_upload.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <runtime/renderer/texture_dxt.h>

#include "util/dxt_texture.h"

namespace godot {

namespace {

PackedByteArray packed_rgba_levels(const std::vector<opennova::renderer::ParticleRgbaImage> &p_levels) {
	std::size_t total = 0;
	for (const opennova::renderer::ParticleRgbaImage &level : p_levels)
		total += level.rgba.size();
	PackedByteArray result;
	result.resize(static_cast<int64_t>(total));
	std::size_t at = 0;
	for (const opennova::renderer::ParticleRgbaImage &level : p_levels) {
		if (!level.rgba.empty())
			std::memcpy(result.ptrw() + at, level.rgba.data(), level.rgba.size());
		at += level.rgba.size();
	}
	return result;
}

// An A8R8G8B8 page as a mipmapped Godot image: its retail levels, then the
// same box filter on to 1 x 1.
Ref<Image> rgba_page_image(std::vector<opennova::renderer::ParticleRgbaImage> p_levels) {
	if (p_levels.empty() || !p_levels.front().valid())
		return Ref<Image>();
	const int width = p_levels.front().width;
	const int height = p_levels.front().height;
	opennova::renderer::extend_box_chain(p_levels, 0);
	return Image::create_from_data(width, height, true, Image::FORMAT_RGBA8,
			packed_rgba_levels(p_levels));
}

} // namespace

ParticleAtlasPageUpload particle_atlas_page_upload(
		const opennova::renderer::ParticleAtlasPageTexture &p_texture) {
	ParticleAtlasPageUpload upload;
	upload.side = static_cast<std::uint32_t>(std::max(0, p_texture.side()));
	upload.level_count = static_cast<std::uint32_t>(p_texture.level_count());
	if (p_texture.format == opennova::renderer::TextureDxtFormat::Dxt5) {
		upload.dxt5 = true;
		upload.image = image_from_dxt_levels(p_texture.dxt_levels);
		upload.levels = packed_dxt_levels(p_texture.dxt_levels);
		return upload;
	}
	if (p_texture.format != opennova::renderer::TextureDxtFormat::None) {
		// The page flags never ask for DXT1 (no 0x100, no 0x400000).
		upload.level_count = 0;
		upload.side = 0;
		return upload;
	}
	upload.image = rgba_page_image(p_texture.rgba_levels);
	upload.levels = packed_rgba_levels(p_texture.rgba_levels);
	return upload;
}

std::uint64_t particle_atlas_page_level_bytes(std::uint32_t p_side, bool p_dxt5) {
	const std::uint64_t side = std::max<std::uint64_t>(1u, p_side);
	if (!p_dxt5)
		return side * side * 4u;
	const std::uint64_t blocks = std::max<std::uint64_t>(1u, (side + 3u) / 4u);
	return blocks * blocks * 16u;
}

} // namespace godot
