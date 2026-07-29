#pragma once

// Portable terrain height sampler.
//
// The "height at a world position" query was RE'd long ago but lived only inside
// the Godot binding (NovaTerrainData::get_height / get_height_world /
// get_height_world_bilinear), duplicated three times over the class's own
// cpt.depth_buffer + trn. That left the portable engine (libs/world AI grounding,
// libs/mission) with NO way to sample terrain height. This is the one shared home:
// NovaTerrainData becomes a thin wrapper over these functions, and the runtime AI
// samples the exact same implementation the renderer/editor do.
//
// [orig: jodemo.exe Terrain_SampleHeightBilinear @0x5C6770 +
// Terrain_GetFoliageMapValue @0x5C65E0 world->source mapping (docs §5.2/§4.4.4).
// Jointops entity grounding goes through Terrain_RaycastHeightmapHiRes_0 @0x60e710,
// a hi-res lo-res-then-bisect DOWN-raycast that writes the ground height back into
// the probe position's Z. Its near-vertical result equals this bilinear column
// height; the sub-cell refinement along the ray is a tracked deviation, faithful
// enough for terrain-following grounding.]

#include <cstdint>

#include "terrain/coords.h"

namespace opennova::terrain {

// Pure data: the heightmap atlas + its sector layout + the water plane. No Godot.
// `heightmap` is row-major dim*dim raw16 (raw value = height_units * 256). For real
// terrain dim is the 1024 atlas side; tests may use a smaller square atlas.
struct TerrainHeightField {
	const uint16_t *heightmap = nullptr;
	int dim = 0;             // heightmap side (atlas = 1024)
	// Per-quadrant neighbour-tap locks (.trn lock_*). Default all-zero = every tap
	// wraps across the full atlas, the behavior before the locks were honored.
	// Sits here, next to `dim`, so it lands in that member's tail padding: this is a
	// by-value POD copied into deep stack frames and it must not grow.
	CoordsQuadrantLocks locks{};
	SectorLayout layout;     // world->source sector remap (the *_world variants)
	int32_t water_y = 0;     // [orig: worldY @0x26C6454] water plane, 16.16 fixed
	bool has_water = false;

	bool valid() const { return heightmap != nullptr && dim > 0; }
};

namespace detail {
// The same layout minus the locks. They are free only if they fit its padding.
struct TerrainHeightFieldNoLocks {
	const uint16_t *heightmap;
	int dim;
	SectorLayout layout;
	int32_t water_y;
	bool has_water;
};
} // namespace detail

static_assert(sizeof(TerrainHeightField) == sizeof(detail::TerrainHeightFieldNoLocks),
              "TerrainHeightField grew: the packed locks no longer fit its padding. This is a "
              "by-value POD copied into deep stack frames — a 32-byte lock array here overflowed "
              "tests/world/infantry_test's stack.");

// Direct bilinear sample of the square buffer with (dim-1) wrap and NO sector remap.
// [orig: NovaTerrainData::get_height / gobj_trn_sample_height_bilinear 0x100314A1.]
// Returns height in WORLD UNITS (raw16 / 256).
float height_field_height_bilinear(const TerrainHeightField &f, float world_x, float world_z);

// world->source sector remap (coords.h runtime options), then NEAREST sample.
// [orig: NovaTerrainData::get_height_world.] World units.
float height_field_height_world(const TerrainHeightField &f, float world_x, float world_z);

// world->source sector remap, then BILINEAR sample. This is the renderer-accurate
// column height the AI grounds on. [orig: NovaTerrainData::get_height_world_bilinear /
// Terrain_SampleHeightBilinear @0x5C6770.] World units.
float height_field_height_world_bilinear(const TerrainHeightField &f, float world_x, float world_z);

} // namespace opennova::terrain
