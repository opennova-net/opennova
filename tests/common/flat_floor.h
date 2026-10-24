// A flat heightfield floor for rigs whose bodies run the player-body (org2)
// update on the authority: its gravity, the movement resolver and the fall and
// landing edges sit behind the terrain retail always has, so a rig that wants a
// standing remote player's contacts gives it a floor at the body's own height.
// Header-only test infrastructure (no retail counterpart to cite).
#pragma once

#include <runtime/terrain_query/height_field.h>

#include <cstdint>
#include <vector>

namespace test_common {

struct FlatFloor {
	static constexpr int kDim = 512;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	opennova::terrain::TerrainHeightField field;

	// The height is whole 1/256 units: a heightmap texel is the height's 16.16
	// word shifted down 8.
	explicit FlatFloor(double height_units = 0.0)
	    : heightmap(kDim * kDim, static_cast<uint16_t>(height_units * 256.0)),
	      sector_grid(256, 1) {
		field.heightmap = heightmap.data();
		field.dim = kDim;
		field.layout.sector_grid = sector_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;
	}
};

} // namespace test_common
