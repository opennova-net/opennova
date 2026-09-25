#pragma once

// Portable pixel producer for one retail terrain-cache page. The cache
// compiler decides identity/lifetime; this module composes the page's bare
// colormap, mission .til RGB, and heightfield DOT3 alpha without any Godot
// resource or rendering-server dependency. Every pass is an XYZRHW quad drawn
// into the page render target, so the composer reproduces the D3D9 raster:
// page pixel (x, y) samples at the integer position (x, y), a quad covers the
// pixels whose position lies in [start, end) on each axis, and each source
// texture samples with the filter its creation flags select.
// [orig: PolyTrn_RenderTile @ 0x60DA70 — base colormap MODULATE2X pass, .til
// overlay quads, DOT3 light pass; fill_fullscreen_quad_vertices @ 0x678DB0
// copies the quad positions unbiased; the .cpp carries the per-pass witnesses.]

#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/terrain/terrain_tile_light_epoch.h>
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/terrain/texture_preprocess.h>
#include <formats/til/til.h>

#include <array>
#include <vector>

namespace opennova::terrain {

// One 1024-unit source atlas (colormap or heightfield normal) split into the
// four 512-unit quadrant textures retail samples, each carrying the box mip
// chain its texture creation builds. Index = 2 * x_quadrant + z_quadrant, the
// Z-order Terrain_SplitTileIntoQuadrants writes and the page key's quadrant
// bits select.
// [orig: Terrain_SplitTileIntoQuadrants @ 0x604E60; Colormap0..3 and
// TrnNMap0..3 created with flags 0x100001 @ 0x60B3FA..0x60B49A and
// @ 0x60B51C..0x60B591; quadrant select (key >> 24 & 2) + (key >> 9 & 1)
// @ 0x60DD1A..0x60DD28]
struct TerrainTileQuadrantSource {
	std::array<std::vector<Rgba8Image>, 4> quadrants;
	bool is_valid() const noexcept;
};

// Returns an invalid source for an atlas that cannot supply four quadrants
// (either dimension below 2).
TerrainTileQuadrantSource build_terrain_tile_quadrant_source(
		const Rgba8Image &atlas);

// The tile-set atlas texture's level set as its texels decode: created with
// flags 0x100203, which ask for DXT5, so level 0 is the atlas through D3DX's
// DXT5 encoder and every later level D3DXFilterTexture's box filter of the
// level before it, each read back as bytes for the composer to sample.
// [orig: Terrain_LoadTileSetAtlas @ 0x604A90, flags @ 0x604B24;
// GTexture_CreateFromPixelData_0 @ 0x687717..0x687727]
std::vector<Rgba8Image> build_terrain_tile_set_mips(const Rgba8Image &atlas);

struct TerrainTilePageSourceView {
	const TerrainTileQuadrantSource *colormap = nullptr;
	const TerrainTileQuadrantSource *heightfield_normal = nullptr;
	const TilFile *tile_info = nullptr;
	const std::vector<Rgba8Image> *tilestrip = nullptr;
	const TerrainScorchPagePlan *scorch_plan = nullptr;
	const std::array<TerrainScorchTexture,
			kTerrainScorchTextureSlots> *scorch_textures = nullptr;
	std::array<float, 3> tile_overlay_tint{1.0f, 1.0f, 1.0f};
	// Texture-basis RGB bytes: retail getter tuple packed as (g2,g0,g1).
	TerrainTileLightEpoch light_bytes = kDefaultTerrainTileLightEpoch;
};

// Returns an invalid image when the job or mandatory source atlases are
// invalid. Output is one tightly packed RGBA8 cache layer. RGB carries the
// base colormap and the ordered .til and scorch overlays; A is the additive,
// byte-quantized heightfield DOT3 light term alone (the overlay loops run with
// the alpha channel write-masked).
Rgba8Image compose_terrain_tile_page(
		const TerrainTileCompositionJob &job,
		const TerrainTilePageSourceView &sources);

} // namespace opennova::terrain
