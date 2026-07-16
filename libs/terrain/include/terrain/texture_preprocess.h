#pragma once

#include <trn/trn.h>

#include <cstdint>
#include <vector>

namespace opennova::terrain {

// Godot-neutral RGBA8 carrier for the terrain texture preprocessing seam.
// Pixels are tightly packed in row-major R, G, B, A order.
struct Rgba8Image {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels;

	bool is_valid() const noexcept;
};

// Build the detail coefficient/normal texture from the authored detailmap's
// blue channel. [orig: Texture_GenerateNormalMap @ 0x58c070]
Rgba8Image build_detail_coefficient_map(const Rgba8Image &detailmap);

// Build the four-quadrant terrain heightfield normal atlas from raw 16-bit
// depth. Each lock component independently selects quadrant-local wrap
// (non-zero) or full-atlas seam crossing (zero). Encoded RGB is texture-basis
// {slope-x, slope-y, up}; alpha is the retail constant 0x80.
// [orig: Terrain_GenerateNormalMap @ 0x603210]
Rgba8Image build_heightfield_normal_map(
		const std::vector<uint16_t> &depth,
		uint32_t width,
		uint32_t height,
		const TerrainQuadrantLocks &locks = TerrainQuadrantLocks{});

// Normalize DBlend RGB with the retail integer path, preserving alpha and
// mapping a zero RGB sum to pure red.
// [orig: PolyTrn_InitTextures @ 0x60b1f0]
Rgba8Image normalize_detail_blend_map(const Rgba8Image &blendmap);

// Build the exact retail level set for a base/far texture pair. Each input is
// downsampled independently before RGB blending; output alpha always comes
// from the base chain.
// [orig: GTexture_CreateFromPixelDataWithAlphaBlend @ 0x687270]
std::vector<Rgba8Image> build_paired_detail_mip_chain(
		const Rgba8Image &base,
		const Rgba8Image &far_detail);

} // namespace opennova::terrain
