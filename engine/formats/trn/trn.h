#pragma once

#include <formats/foliage/foliage.h>

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
	// The sector grid's WIDTH in columns — not the number of active sectors.
	// save_trn emits this many columns per `polytrn_sectors` row and load_trn
	// reads that many, edge-replicating (or wrapping, per wrap_x) out to 16, so
	// any sector placed at a column >= sector_count is silently dropped on write.
	// Retail maps carry 8 (Dvxi5.trn), as do the committed mnml and Tmap configs.
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

// The tile-set atlas a mission's .til tiles draw from. The environment config
// parse leaves the .trn's polytrn_tilestrip in place; a non-empty mission
// tile-set name (the BMS header's +0x118 slot) then replaces it, its extension
// from the first '.' replaced by, or else appended as, "TGA". The .TSD twin
// takes the same name with "TSD".
// [orig: Terrain_LoadEnvironmentConfig @ 0x610940 — g_BmsTileSetName test
//  @ 0x6109C8, copy into the atlas slot @ 0x6109D2..0x6109E2,
//  Path_ReplaceOrAppendExtension(slot, "TGA") @ 0x6109EE, the TSD slot
//  @ 0x610A00..0x610A1C; Path_ReplaceOrAppendExtension @ 0x53C780]
std::string trn_mission_tilestrip(const TrnConfig &trn,
		const std::string &mission_tile_set);

} // namespace opennova
