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

// The sector grid's sides: 16 sectors at most each way [orig: Terrain_LoadEnvironmentConfig @ 0x610940, the
// gate's `> 16`; the 16 x 16 grid Terrain_ShiftHeightmapRows @ 0x60F190 extends to].
inline constexpr int kTerrainGridSide = 16;

struct TrnConfig {
	// `terrain_name` and `terrain_creator`: read by no arm of either reader (neither word is in the
	// binary but inside "TexHorizon"), kept for whoever edits the file (every shipped .trn opens on them).
	// There is no `horizon`: no arm of either reader compares one (the image's only "horizon" is inside
	// "TexHorizon" [orig: Terrain_ParseConfigCallback @ 0x60f330; TimeOfDay_ParseProperty @ 0x57c590]) and
	// no shipped .trn writes one, so a `horizon` line is skipped as any unknown key is.
	std::string name;
	std::string creator;
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
	// The environment's keywords a terrain carries, which its reader reads in the terrain's pass (the
	// .trn is parsed by the time-of-day parser too, before the mission's .env [orig:
	// Environment_LoadTimeOfDayConfig @ 0x57db30, the .trn pass @ 0x57dbcc..0x57dbde]): the water's colour
	// (three bytes [orig: TimeOfDay_ParseProperty @ 0x57caf6]) and its murk (atof, held at 0.99 from above
	// as the parser stores it [orig: @ 0x57cb7c..0x57cba9]); a .env's line comes after and wins. Every
	// shipped .trn writes both; no shipped .env writes a murk, so the terrain's is the mission's. As
	// written, each with whether the file writes it.
	bool water_rgb_set = false;
	std::array<int, 3> water_rgb = {0, 0, 0};
	bool water_murk_set = false;
	float water_murk = 0.0f;
	int wrap_x = 0;
	int wrap_y = 0;
	TerrainLockCoord lock_topleft;
	TerrainLockCoord lock_topright;
	TerrainLockCoord lock_bottomleft;
	TerrainLockCoord lock_bottomright;
	int sector_grid[kTerrainGridSide][kTerrainGridSide] = {};
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
