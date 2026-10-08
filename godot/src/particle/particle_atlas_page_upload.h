#pragma once

#include <cstdint>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <runtime/renderer/particle_atlas.h>

namespace godot {

// The two uploads one particle atlas page makes of its device texture
// (renderer::particle_atlas_page_texture): the Godot image the render-list
// materials sample, and the compositor's RenderingDevice page.
struct ParticleAtlasPageUpload {
	// The page's retail levels, then a tail to 1 x 1 a mipmapped Godot image
	// needs (the same box filter; no draw reads it, the stage stopping at the
	// page's last level). A DXT5 page uploads its blocks for the GPU to decode,
	// as the retail device did.
	Ref<Image> image;
	// The retail levels alone, end to end from level 0: RGBA8 bytes, or a DXT5
	// page's blocks.
	PackedByteArray levels;
	std::uint32_t side = 0; // level 0's (after the page's halvings)
	std::uint32_t level_count = 0;
	bool dxt5 = false;
};

ParticleAtlasPageUpload particle_atlas_page_upload(
		const opennova::renderer::ParticleAtlasPageTexture &p_texture);

// The bytes one level of `side` takes in the compositor's page: four a texel,
// or a DXT5 page's 16 a 4 x 4 block (at least one block a side).
std::uint64_t particle_atlas_page_level_bytes(std::uint32_t p_side, bool p_dxt5);

} // namespace godot
