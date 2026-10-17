#pragma once

#include <formats/trn/trn.h>
#include <runtime/renderer/texture_dxt.h>
#include <runtime/terrain_query/coords.h>

#include <array>
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

// The texels of `normals` (build_heightfield_normal_map's over the heights
// before) whose kernel reads a texel of the atlas rect `rect` made again from
// `depth`, every other texel kept: equal to a fresh build over `depth` when only
// that rect changed (an editor's sculpt stroke). The kernel reads a texel's
// four neighbours, never the texel itself, so the texels made again are those
// of the rect's rows whose left or right tap meets its columns and those of
// its columns whose upper or lower tap meets its rows. `written`, when given,
// receives boxes holding every texel made again, each inside one of the map's
// four quadrants and as tight as the texels allow: a tap that wraps (at a
// locked quadrant's edge, at the atlas's) writes texels across the quadrant
// from the rect, in a box of their own (the device textures cut from the
// quadrants are made again over those boxes: update_terrain_tile_quadrants).
// The rect is the terrain's region contract (quadtree.h refresh_mipchain's):
// inside the map, a change across its wrap arriving as
// several rects; one of no size writes nothing. False, nothing written, where
// `normals` is not a map of the depth's shape or the rect is not inside it.
bool refresh_heightfield_normal_map(
		Rgba8Image &normals,
		const std::vector<uint16_t> &depth,
		uint32_t width,
		uint32_t height,
		const TerrainQuadrantLocks &locks,
		const terrain::CoordsRect &rect,
		std::vector<terrain::CoordsRect> *written = nullptr);

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
// renderer/texture_compression.h) with the terrain texture detail's halving
// word (renderer::terrain_texdetail_flags, 0 at full detail) and 8, which pick
// DXT1 either way on an adapter outside the NVIDIA DXT5 list. The pixels (the
// base and the far alike) are halved first as the word asks. Without a far
// texture the layer's TGA goes through
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
		int32_t session_texcompression_level,
		int32_t terrain_texdetail);

// The minimap's `depthspin` shore texture, 256 x 256, from the 1024 x 1024 height atlas: each texel
// sums the four taps at (4x, 4z), (4x + 2, 4z), (4x, 4z + 2) and (4x + 2, 4z + 2) and keeps the sum
// >> 10, the integer height byte (retail stores (sum << 14) | 0xFFFFFF, the byte its alpha)
// [orig: PolyTrn_InitTextures @ 0x60BA20..0x60BB11, the texture made @ 0x60BB3B]. Packed RG8 for
// the device: R that byte, G the water plane the shore pass's alpha test compares it with (the
// plane floored, clamped 0..254; hud_minimap.h, the alpha-tested redraw), so the device samples
// both and tests after filtering. Empty (no texture) for an atlas of another size, a non-finite
// plane, or a plane below 1, where the pass draws nothing.
std::vector<uint8_t> build_depthspin_rg8(const std::vector<uint16_t> &depth, float water_height_wu);

// D3DXFilterTexture(D3DX_FILTER_BOX) level chain used by ordinary TGA
// textures. Retail allocates while min(width,height)>2, so the terminal
// level for the shipped power-of-two terrain scorch images is 4x4.
// [orig: GTexture_CreateFromPixelData_0 @0x6876C0]
std::vector<Rgba8Image> build_box_mip_chain_to_4x4(
		const Rgba8Image &base);

} // namespace opennova::terrain
