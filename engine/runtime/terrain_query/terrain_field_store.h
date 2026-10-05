#pragma once

// The ONE owning cpt/trn(+charmap) -> TerrainHeightField + SurfaceTypeMap
// builder (ADR 0042 d4, amending ADR 0020 d4). Before it, three sites derived
// the runtime terrain field by hand — the Godot Simulation binding, the
// static-shadow receiver snapshot, and the retail-mission test rig (which had
// drifted: it built no SurfaceTypeMap at all) — and a fourth copy sat in a
// conformance test. The store owns the buffer copies the views point into
// (heightmap, flattened sector grid, charmap raster), so the raw-pointer PODs
// survive the source document/resource being freed or reloaded.
//
// Format-free by design: this header is on the ADR 0020 seam list, so
// engine/net, runtime/wac, runtime/mission and runtime/world may include it.
// The format-typed build entry over the parsed cpt/trn documents lives in
// terrain_field_build.h, which is NOT on the seam list.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/surface_type_map.h>

namespace opennova::terrain {

class TerrainFieldStore {
public:
	TerrainFieldStore() = default;
	// The views hold raw pointers into this store's own buffers; a copy would
	// silently point the copy's views at the original's storage.
	TerrainFieldStore(const TerrainFieldStore &) = delete;
	TerrainFieldStore &operator=(const TerrainFieldStore &) = delete;

	// Rebuild from plain buffers (all copied; the caller's may be freed after).
	//   heightmap        row-major dim*dim raw16 (height_units * 256);
	//                    dim = floor(sqrt(heightmap_count)), the 1024 atlas
	//                    side for real terrain. nullptr/0 clears the store.
	//   sector_grid      the 16x16 sector grid, 256 row-major ints
	//                    (0 = empty, 1..4 = quadrant).
	//   origin_x/y       the .trn sector origins in 512-unit sector coords.
	//   sector_count/rows the authored grid width/height for bounds-checked
	//                    queries (runtime sampling still wraps the 16x16 grid).
	//   wrap_x/z         the .trn's global wrap flags for fixed-point gradients.
	//   locks            the .trn's four lock_* pairs folded to the portable
	//                    neighbour-tap policy (coords_locks_from).
	//   charmap          optional palette-indexed surface raster (row-major
	//                    charmap_width * charmap_height); nullptr or a
	//                    non-positive dimension leaves the surface map zeroed —
	//                    the sampler's "no charmap -> surface 1" leg.
	void build(const uint16_t *heightmap, size_t heightmap_count,
			const int *sector_grid, int32_t origin_x, int32_t origin_y,
			int32_t sector_count, int32_t sector_rows, bool wrap_x, bool wrap_z,
			const CoordsQuadrantLocks &locks,
			const uint8_t *charmap = nullptr, int32_t charmap_width = 0,
			int32_t charmap_height = 0);
	void clear();

	bool valid() const { return field_.valid(); }
	// The occupant water clamp's plane: the mission water height in the SAME
	// 16.16 world units the ground solve compares (the retail global the
	// clamp reads [orig: worldY @0x26C6454 -- g_EnvWaterHeightFixed]); 0 = no
	// authored water. The kernel feeds it from World::env.water_z whenever
	// the environment changes; build() resets it to none.
	void set_water_plane(int32_t water_y_q16) {
		field_.water_y = water_y_q16;
		field_.has_water = water_y_q16 != 0;
	}

	// Views into the owned buffers; stable until the next build()/clear().
	const TerrainHeightField &height_field() const { return field_; }
	// Zero-initialized (charmap members) when no charmap was supplied. It
	// carries the placed-tile overlay below, so every copy of the view (the
	// world's, MissionKernel::wire_terrain's) carries the tiles too.
	const SurfaceTypeMap &surface_map() const { return surface_map_; }

	// The .trn facts the mission boot reads beside the field, set by the
	// format-typed entries (terrain_field_build.h): the map's water height in
	// world units (the raw value * 0.5; 0 = the map has no water), the water
	// plane's terrain rung [orig: Terrain_Init @ 0x60fcb1..0x60fcba, the
	// nonzero value standing for the bit-31 "has water" flag], and the
	// tilestrip after the mission's BMS tile-set override, the name the .TSD
	// table pairs with (formats/trn trn_mission_tilestrip). build()/clear()
	// reset them.
	void set_trn_facts(float water_height, std::string tilestrip) {
		trn_water_height_ = water_height;
		tilestrip_ = std::move(tilestrip);
	}
	float terrain_water_height() const { return trn_water_height_; }
	const std::string &tilestrip() const { return tilestrip_; }

	// The mission's placed-tile overlay (D-SND-15): the .til placements and
	// the tileset's .TSD tile-index -> surface table, which the surface walk
	// reads after the charmap sample. Retail keeps both in the terrain
	// subsystem's globals [orig: g_TerrainTileArray @ 0x319f7a4, filled by
	// PolyTrn_LoadTileData @ 0x6081d0; the table byte_319F7D8 @ 0x319f7d8,
	// memset by PolyTrn_InitTextures @ 0x60c5c9]; here they ride the store so
	// the views every wiring copies carry them. The overlay is mission data
	// set once per load and outlives build()/clear() (a fresh kernel owns a
	// fresh store); the format-typed setters are in terrain_field_build.h.
	void set_placed_tiles(std::vector<SurfaceTileEntry> tiles);
	void set_tile_surface_table(const std::array<uint8_t, 256> &table);
	const std::vector<SurfaceTileEntry> &placed_tiles() const { return placed_tiles_; }
	const std::array<uint8_t, 256> &tile_surface_table() const { return tile_surface_; }

private:
	void attach_placed_tiles();

	std::vector<uint16_t> heightmap_;
	std::vector<int> sector_grid_;
	std::vector<uint8_t> surface_indices_;
	TerrainHeightField field_;
	SurfaceTypeMap surface_map_;
	float trn_water_height_ = 0.0f;
	std::string tilestrip_;
	std::vector<SurfaceTileEntry> placed_tiles_;
	// Zeroed: retail's memset default, every placed tile TSD_NULL
	// [orig: PolyTrn_InitTextures @ 0x60c5c9].
	std::array<uint8_t, 256> tile_surface_{};
};

} // namespace opennova::terrain
