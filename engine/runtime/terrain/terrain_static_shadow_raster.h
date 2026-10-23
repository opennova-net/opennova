#pragma once

// Portable leaf raster for one retail static-shadow tile page. The Godot
// binding resolves selected-LOD ROBJ geometry into normalized page vertices;
// this module owns the witnessed temporary-RT semantics: the near-plane clip
// (depth < 0, i.e. geometry lower than 0.1 u above the caster's ground plane,
// never casts — buried skirts and foundations are cut away), 2x coverage,
// z<=0.5 receiver admission, material alpha testing/blending, and a linear
// resolve into the destination-resolution light-alpha carrier. It never
// changes RGB.
// [orig: Terrain_CollectAndRenderTileModels @0x60D5BF..0x60DA4F;
// the ortho depth row RenderSlot_SetupShadowCascadeMatrices_0 @0x58D5F8..0x58D60C;
// PROJSHAD submits @0x60D960..0x60D97D; temp-blue composite
// @0x60E0C6..0x60E19D]

#include <runtime/terrain/terrain_static_shadow_alpha.h>
#include <runtime/terrain/terrain_static_shadow.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::terrain {

// The temp target is twice the page at every tier: config+0x1740 selects page
// dword_31A00D4 = 0x100 with temp dword_31A00D0 = 0x200, otherwise 0x80 with 0x100
// [orig: PolyTrn_LoadTerrainConfig @0x60E41D..0x60E446].
inline constexpr uint32_t kTerrainStaticShadowTemporaryScale = 2;
// The collector's clamp on the light's vertical component (its fixed twin 0x4000)
// [orig: float clamp @ 0x60D33F..0x60D341, fixed twin @ 0x60d325..0x60d32c].
inline constexpr float kTerrainStaticShadowMinimumVerticalLight = 0.25f;
// Projected depth h*0.0005 - 0.00005 [orig: RenderSlot_SetupShadowCascadeMatrices_0
// @ 0x58D4D3..0x58D618].
inline constexpr float kTerrainStaticShadowDepthScale = 0.00050000002f;
inline constexpr float kTerrainStaticShadowDepthBias = -0.000050000002f;

struct TerrainStaticShadowRasterVertex {
	// Normalized destination-page coordinates. (0,0) and (1,1) are the outer
	// pixel edges. depth is post-projection D3D depth; texture UVs are wrapped.
	float page_u = 0.0f;
	float page_v = 0.0f;
	float depth = 0.0f;
	float texture_u = 0.0f;
	float texture_v = 0.0f;
};

struct TerrainStaticShadowWorldVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float texture_u = 0.0f;
	float texture_v = 0.0f;
};

struct TerrainStaticShadowProjectionInput {
	TerrainTilePageKey page{};
	// Godot/presentation-world surface-to-light. For the direct retail
	// Environment getter tuple g, use (g2,g1,g0), not the tuple verbatim.
	TerrainStaticShadowLightDirection surface_to_light{};
	// Retail subtracts Terrain_GetHeightAtPosition(entity x,y) from the entity
	// vertical translation before rendering the selected shadow LOD.
	float caster_ground_y = 0.0f;
};

// Environment_GetLightDirectionFloat preserves retail's direct g tuple; the
// terrain page projector operates in presentation-world XYZ.
TerrainStaticShadowLightDirection
terrain_static_shadow_world_light_from_environment_tuple(
		float g0, float g1, float g2) noexcept;

// Structural reduction of the retail world/view/ortho matrices into page UV
// and D3D depth. Returns false for invalid/non-finite inputs.
bool project_terrain_static_shadow_vertex(
		const TerrainStaticShadowProjectionInput &input,
		const TerrainStaticShadowWorldVertex &world,
		TerrainStaticShadowRasterVertex &projected) noexcept;

struct TerrainStaticShadowAlphaMipView {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t row_stride = 0;
	const uint8_t *alpha = nullptr;
};

enum class TerrainStaticShadowMipFilter : uint8_t {
	Point,
	Linear,
};

struct TerrainStaticShadowAlphaTextureView {
	const TerrainStaticShadowAlphaMipView *mips = nullptr;
	std::size_t mip_count = 0;
	TerrainStaticShadowMipFilter mip_filter =
			TerrainStaticShadowMipFilter::Point;
};

enum class TerrainStaticShadowBlend : uint8_t {
	// BLEND_NONE: the black PROJSHAD fragment replaces temp blue with zero.
	Opaque,
	// BLEND_ALPHA: black source leaves destination * (1-source alpha).
	Alpha,
	// BLEND_ADD: black + destination is an output no-op.
	Additive,
	// BLEND_MULT: DESTCOLOR/SRCCOLOR with a black source resolves to zero.
	Multiply,
};

struct TerrainStaticShadowRasterTriangle {
	std::array<TerrainStaticShadowRasterVertex, 3> vertices{};
	TerrainStaticShadowBlend blend = TerrainStaticShadowBlend::Opaque;
	// Retail uses CULLMODE CCW unless the authored material carries its
	// two-sided flag. The static tile pass is not a reflection pass, so its
	// mirror-winding override is never active.
	bool two_sided = false;
	// -1 means no texture-alpha sample. Alpha-blended or material-alpha-tested
	// triangles name a view in TerrainStaticShadowRasterInput::alpha_textures.
	int32_t alpha_texture_index = -1;
	bool alpha_test_enabled = false;
	// The material flag flips D3DCMP_GREATER into D3DCMP_LESSEQUAL while
	// retaining the authored reference byte.
	bool alpha_test_inverted = false;
	uint8_t alpha_ref = 0;
	float alpha_scale = 1.0f;
};

// The work one raster did (diagnostics; the bytes never depend on them).
// Every blend leaves a black temp pixel black (opaque and multiply write 0,
// additive keeps the destination, alpha scales it), and the resolve reads the
// temp alone, so the pixel loop skips black pixels: an 8x8 temp block whose
// pixels are all black, and a black pixel inside a live block, cost no edge
// test, depth test or texture fetch.
struct TerrainStaticShadowRasterStats {
	// Pixel centres a triangle admitted over a live (non-black) temp pixel:
	// each reached the depth test.
	uint64_t fragments = 0;
	// 8x8 temp blocks of a triangle's bounds skipped as all black.
	uint64_t absorbed_blocks = 0;
	// Black temp pixels skipped inside a live block.
	uint64_t absorbed_pixels = 0;
	// Wall time of the raster's stages, microseconds (an instrument): the temp's fill, the
	// triangles' setup on the lanes, the pixel loop on the lanes, and the resolve. The pixel
	// loop also sums its lanes' own times and keeps the slowest lane's: their ratio is the
	// parallelism the lane pool gave it.
	uint64_t setup_us = 0;
	uint64_t prepare_us = 0;
	uint64_t pixel_us = 0;
	uint64_t pixel_lane_sum_us = 0;
	uint64_t pixel_lane_max_us = 0;
	uint64_t resolve_us = 0;
};

struct TerrainStaticShadowRasterInput {
	// Input order remains retail collector/ROBJ/strip order. Every triangle is
	// already admitted for culling/material visibility by the binding.
	std::vector<TerrainStaticShadowRasterTriangle> triangles;
	std::vector<TerrainStaticShadowAlphaTextureView> alpha_textures;
	float receiver_depth = 0.5f;
	// Threads the pixel loop may split its rows across (1 = the calling
	// thread only). Every count produces the same bytes.
	std::size_t threads = 1;
	// Optional: receives this raster's work counts (added to).
	TerrainStaticShadowRasterStats *stats = nullptr;
};

// Atomically mutates only page.alpha. Invalid page storage, non-finite input,
// or a referenced invalid alpha texture returns false without changing page.
// Empty/degenerate/depth-rejected geometry is a valid no-op; a triangle
// crossing depth 0 is clipped to its part at or above the near plane.
bool rasterize_terrain_static_shadow_alpha(
		const TerrainStaticShadowRasterInput &input,
		TerrainStaticShadowAlphaPage &page) noexcept;

} // namespace opennova::terrain
