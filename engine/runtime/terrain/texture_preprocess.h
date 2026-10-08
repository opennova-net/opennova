#pragma once

#include <formats/trn/trn.h>
#include <runtime/renderer/texture_dxt.h>

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

// One of the three splat detail layers as the device texture retail creates:
// the session texcompression_level's flags (0x400100 below 1, else 0x400200:
// renderer/texture_compression.h) with the terrain-detail flags (0 at full
// detail) and 8, which pick DXT1 either way on an adapter outside the NVIDIA
// DXT5 list. Without a far texture the layer's TGA goes through
// GTexture_CreateFromPixelData_0 (level 0 loaded, later levels
// D3DXFilterTexture's chain); with one, GTexture_CreateFromPixelDataWithAlphaBlend
// loads each paired level with D3DX_FILTER_NONE.
// [orig: PolyTrn_InitTextures @ 0x60AB95..0x60ABE5 (flags), loads
// @ 0x60ABEF..0x60AC13 and @ 0x60ACB1/0x60AD0B/0x60AD65;
// Texture_LoadByNameWithChannel @ 0x58B70C (TGA flag 0x100000),
// @ 0x58B73F; GTexture_CreateFromPixelDataWithAlphaBlend @ 0x6875C5]
std::vector<renderer::DxtSurface> build_detail_layer_levels(
		const Rgba8Image &base,
		const Rgba8Image *far_detail,
		int32_t session_texcompression_level);

// D3DXFilterTexture(D3DX_FILTER_BOX) level chain used by ordinary TGA
// textures. Retail allocates while min(width,height)>2, so the terminal
// level for the shipped power-of-two terrain scorch images is 4x4.
// [orig: GTexture_CreateFromPixelData_0 @0x6876C0]
std::vector<Rgba8Image> build_box_mip_chain_to_4x4(
		const Rgba8Image &base);

} // namespace opennova::terrain
