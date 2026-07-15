#pragma once

#include <foliage/foliage.h>

#include <array>
#include <string>
#include <vector>

namespace opennova {

// Per-source-quadrant height lookup policy. A non-zero component wraps
// neighbour taps inside that 512x512 quadrant; zero crosses the internal seam
// and wraps across the full 1024x1024 atlas.
struct TerrainLockCoord {
	int x = 0;
	int y = 0;
};

// Retail order: top-left, top-right, bottom-left, bottom-right.
using TerrainQuadrantLocks = std::array<TerrainLockCoord, 4>;

struct TrnConfig {
	std::string name;
	std::string colormap;
	std::string detailmap_c1;
	std::string detailmap_c2;
	std::string detailmap_c3;
	std::string detailblendmap;
	std::string polydata;
	std::string detailmap;
	std::string detailmap2;
	std::string detailmapdist;
	std::string detailmapdist2;
	int detail_density = 128;
	int detail_density2 = 8;
	int sector_count = 0;
	int origin_x = 0;
	int origin_y = 0;
	int water_height = 0;
	int wrap_x = 0;
	int wrap_y = 0;
	TerrainLockCoord lock_topleft;
	TerrainLockCoord lock_topright;
	TerrainLockCoord lock_bottomleft;
	TerrainLockCoord lock_bottomright;
	double horizon = 0.0;
	int sector_grid[16][16] = {};
	int sector_rows = 0;
	std::string foliagemap;
	std::vector<FoliageDef> foliage_defs;
	std::string charmap;
	std::string tilestrip;
	std::string tileinfo;

	TerrainQuadrantLocks get_quadrant_locks() const noexcept {
		return {lock_topleft, lock_topright, lock_bottomleft, lock_bottomright};
	}
};

} // namespace opennova
