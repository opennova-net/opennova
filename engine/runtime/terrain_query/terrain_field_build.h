#pragma once

// Format-typed build entries over the parsed .cpt/.trn documents for the
// terrain field store and the non-owning height field (ADR 0042 d4).
// Deliberately NOT on the ADR 0020 seam list: the seam consumers
// (engine/net, runtime/wac, runtime/mission, runtime/world) take the
// format-free terrain_field_store.h; this header is for the embedders that
// hold the parsed documents (TerrainData, Simulation, the retail-mission rig).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <base/vfs/file_source.h>
#include <formats/cpt/cpt.h>
#include <formats/trn/trn.h>
#include <runtime/terrain_query/terrain_field_store.h>

namespace opennova::terrain {

// The .trn's four lock_* pairs as the portable neighbour-tap policy. One
// converter for every heightmap tap in the runtime: the height samplers (via
// the height field), the render mesh, and the collision heightfield — so no
// site can silently keep the old unconditional full-atlas wrap.
CoordsQuadrantLocks coords_locks_from(const TrnConfig &trn);

// Everything a TerrainHeightField takes from the TRN: sector origins, extent and
// the global wrap flags and neighbour-tap locks. The heightmap and sector-grid POINTERS stay the
// caller's, but the derived members live here so a second field builder cannot
// silently miss one — which is exactly how the sim's grounding field kept the
// pre-lock full-atlas wrap, and the player kept falling through ground the
// mesh drew as solid.
void height_field_apply_trn(TerrainHeightField &field, const TrnConfig &trn);

// A NON-owning TerrainHeightField over the loaded CPT depth buffer + TRN
// sector layout — the per-query field TerrainData's get_height* methods and
// the editor raycast substrate build over the documents' own storage. The
// height samplers don't read water, so it's left default; the AI-grounding
// path (the owning terrain_field_store_build below) supplies its own policy.
TerrainHeightField height_field_from(const CptFile &cpt, const TrnConfig &trn);

// The ONE owning build: copy the depth buffer, flatten the 16x16 sector grid,
// apply the origins, extent and the per-quadrant neighbour-tap locks, and point the
// SurfaceTypeMap at the (copied) charmap raster when one is supplied
// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510]. The .trn's water
// height and tilestrip land as the store's trn facts; the caller's `trn`
// carries the mission's tilestrip already (TerrainData applies the BMS
// tile-set override at its load, terrain_field_store_load below at its own).
void terrain_field_store_build(TerrainFieldStore &store, const CptFile &cpt,
		const TrnConfig &trn, const uint8_t *charmap = nullptr,
		int32_t charmap_width = 0, int32_t charmap_height = 0);

// The whole embedder-side load for an embedder that holds no parsed terrain
// documents of its own (the mission kernel's file entry: opennova-serve, the
// ctests): read `<terrain_name>.trn` through `index`, then overcast.def and the
// mission's `<environment>.env` (empty: it names none) through the terrain's
// parser after it (formats/trn load_mission_trn, D-TERRAIN-18), the height data
// the result names, apply the mission's BMS tile set (`tile_set`; empty keeps
// the configuration's own tilestrip), decode the named charmap PCX when present
// (absent or undecodable = no surface map, the sampler's "no charmap -> surface
// 1" leg, logged at kWarn), and build the store. False with `error` when the
// documents are missing or malformed. The shell keeps its parsed TerrainData
// and calls terrain_field_store_build directly.
bool terrain_field_store_load(TerrainFieldStore &store, const ResourceIndex &index,
		const std::string &terrain_name, const std::string &tile_set, const std::string &environment,
		std::string &error);
// The same load over any flat-name file source (the editor's project files, its open documents
// standing in for theirs): the configuration it read handed back in `trn` where asked (its water
// height, the mission's tile strip and its char map name).
bool terrain_field_store_load(TerrainFieldStore &store, const FileSource &files,
		const std::string &terrain_name, const std::string &tile_set, const std::string &environment,
		std::string &error, TrnConfig *trn = nullptr);

// The terrain's own tile info file: a terrain configuration's polytrn_tileinfo
// with its extension forced to TIL from its FIRST '.' (the whole value
// scanned, a directory's dot included); "" for a configuration that names none
// (retail forces ".TIL" there, a name no file has). Lookups fold case, so the
// name carries the lower-case ".til" every other tile info name does.
// [orig: Terrain_Init @ 0x60fcfd, Path_ReplaceOrAppendExtension(+0xF00, "TIL")
//  over the value Terrain_ParseConfigCallback stores @ 0x60f910..0x60f930; the
//  first-dot scan @ 0x53c7c4]
std::string terrain_tileinfo_name(const std::string &tileinfo);

// The placed-tile bytes the authority loads at mission start: `mission_til`
// (the mission's own .til, mission_sidecars' tiles row), else the terrain's
// own (terrain_tileinfo_name of the polytrn_tileinfo the mission's terrain
// configuration holds: `terrain_name`.trn, overcast.def and the mission's
// `environment`.env through the terrain's parser, as terrain_field_store_load
// reads them). Each reads through `read_loose_first` and is taken only where
// the game's load takes it (formats/til til_load_accepts): a missing, short or
// bad-magic file sends the load on to the next. Returns the name it took, ""
// (with `out` empty) when neither loaded. The authority alone calls this; a
// joiner's tiles are the host's stream.
// [orig: PolyTrn_LoadTerrainConfig @ 0x60e3d0, the authority test @ 0x60e6c9:
//  Terrain_LoadTileInfoFile(<map>.TIL) @ 0x60e6d2, and on -1 @ 0x60e6dc
//  Terrain_LoadTileInfoFile(polytrn_tileinfo) @ 0x60e6e5; the loose-first
//  force @ 0x60a74e]
using TerrainFileReader = std::function<bool(const std::string &name, std::vector<uint8_t> &out)>;
std::string read_placed_tile_bytes(const TerrainFileReader &read_loose_first,
		const TerrainFileReader &read_file, const std::string &mission_til,
		const std::string &terrain_name, const std::string &environment,
		std::vector<uint8_t> &out);

// The mission .til placements onto the store's placed-tile overlay: the S2C
// 0x45 stream's til0 bytes folded to the surface walk's rows (unparseable or
// empty bytes = no tiles) [orig: Terrain_LoadTileInfoFile @ 0x60a740 ->
// PolyTrn_LoadTileData @ 0x6081d0 fills g_TerrainTileArray @ 0x319f7a4].
void terrain_field_store_set_placed_tiles(TerrainFieldStore &store,
		const std::vector<uint8_t> &til_bytes);

// The tileset's .TSD table for the store's tilestrip onto the overlay
// (`<tilestrip base>.TSD` through the mounted reader; absent, as on every
// shipped JO install, leaves the zeroed table, so every placed tile reads
// TSD_NULL) [orig: PolyTrn_InitTextures -- the memset @ 0x60c5c9, the exists
// probe @ 0x60c5d3, File_ParseASCIIFile @ 0x60c5ef]. No field (no terrain)
// leaves the zeroed table.
void terrain_field_store_resolve_tile_surfaces(TerrainFieldStore &store,
		const std::function<bool(const std::string &name)> &has_file,
		const std::function<bool(const std::string &name, std::vector<uint8_t> &out)> &read_file);

} // namespace opennova::terrain
