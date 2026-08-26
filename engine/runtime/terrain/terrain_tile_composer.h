#pragma once

// Portable pixel producer for one retail terrain-cache page. The cache
// compiler decides identity/lifetime; this module composes the page's bare
// colormap, mission .til RGB, and heightfield DOT3 alpha without any Godot
// resource or rendering-server dependency.
// [orig: PolyTrn_RenderTile @ 0x60DA70 — base colormap MODULATE2X pass, .til
// overlay quads, DOT3 light pass; the .cpp carries the per-pass witnesses.]

#include <terrain/terrain_tile_composition_cache.h>
#include <terrain/terrain_tile_light_epoch.h>
#include <terrain/terrain_scorch.h>
#include <terrain/texture_preprocess.h>
#include <til/til.h>

#include <array>

namespace opennova::terrain {

struct TerrainTilePageSourceView {
	const Rgba8Image *colormap = nullptr;
	const Rgba8Image *heightfield_normal = nullptr;
	const TilFile *tile_info = nullptr;
	const Rgba8Image *tilestrip = nullptr;
	const TerrainScorchPagePlan *scorch_plan = nullptr;
	const std::array<TerrainScorchTexture,
			kTerrainScorchTextureSlots> *scorch_textures = nullptr;
	std::array<float, 3> tile_overlay_tint{1.0f, 1.0f, 1.0f};
	// Texture-basis RGB bytes: retail getter tuple packed as (g2,g0,g1).
	TerrainTileLightEpoch light_bytes = kDefaultTerrainTileLightEpoch;
};

// Returns an invalid image when the job or mandatory source atlases are
// invalid. Output is one tightly packed RGBA8 cache layer. RGB includes the
// ordered .til composition; A includes the same overlays' fixed-function
// render-target alpha followed by the additive, byte-quantized heightfield
// DOT3 light term.
Rgba8Image compose_terrain_tile_page(
		const TerrainTileCompositionJob &job,
		const TerrainTilePageSourceView &sources);

} // namespace opennova::terrain
