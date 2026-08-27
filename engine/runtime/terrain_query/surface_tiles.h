#pragma once

// The placed-tile surface-override resolvers (D-SND-15) — terrain_query-side
// so the mission/world consumer trees stay behind the ADR 0020 seam and the
// embedder (Simulation) carries wiring only. The .til/.tsd format access
// lives in the .cpp.

#include <runtime/terrain_query/surface_type_map.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::terrain {

// The embedder's mounted-resource reader (mirrors mission::BootFileSource's
// shape; a separate pair here keeps terrain_query free of mission includes).
struct SurfaceTileFileSource {
	std::function<bool(const std::string &name)> has_file;
	std::function<bool(const std::string &name, std::vector<uint8_t> &out)>
			read_file;
	bool valid() const { return has_file != nullptr && read_file != nullptr; }
};

// Resolve the tileset's companion surface-definition table: probe
// `<tilestrip base>.TSD` and parse its INDEX_ rows into the 256-entry
// tile-index -> surface table (fills out_table, zeroed first). Absent — every
// shipped JO install — the zeroed table is retail's memset default, so placed
// tiles read 0 = TSD_NULL. The name derivation is the authored tilestrip
// value with its extension replaced by .TSD, exactly the retail pairing; the
// retail BMS tile-set-name override of the tilestrip pair is not modeled (the
// reimpl BMS document carries no tileset field), so the .trn's authored
// tilestrip names both the atlas and the .TSD — the recorded D-SND-15 residue.
// [orig: PolyTrn_InitTextures — table memset @ 0x60c5c9, exists probe
// @ 0x60c5d3, File_ParseASCIIFile @ 0x60c5ef with the Terrain_ParseTsdRow row
// callback; the .TSD extension pairing off the tilestrip copy @ 0x610a1c
// (Terrain_LoadEnvironmentConfig); the unmodeled BMS override
// Bms_TileSetName @ 0xa762e8, applied @ 0x6109ce]
void resolve_tileset_surface_table(const SurfaceTileFileSource &files,
		const std::string &tilestrip, uint8_t out_table[256]);

// The mission .til placements -> the sim-side placed-tile surface array.
// Retail keeps ONE shared tile array serving the network stream, the render
// overlay, and the surface-type walk; the reimpl folds the same til0 bytes
// the S2C 0x45 stream carries. Unparseable/empty bytes yield an empty array
// (no override pass).
// [orig: PolyTrn_LoadTileData @ 0x6081d0 fills g_TerrainTileArray
// @ 0x319f7a4, walked by Terrain_GetSurfaceTypeAtPosition @ 0x6065cc]
std::vector<SurfaceTileEntry> surface_tiles_from_til_bytes(
		const std::vector<uint8_t> &til_bytes);

} // namespace opennova::terrain
