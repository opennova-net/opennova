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
// ctests): read `<terrain_name>.cpt/.trn` through `index`, apply the
// mission's BMS tile set (`tile_set`; empty keeps the .trn's own tilestrip),
// decode the .trn-named charmap PCX when present (absent or undecodable = no
// surface map, the sampler's "no charmap -> surface 1" leg, logged at kWarn),
// and build the store. False with `error` when the documents are missing or
// malformed. The shell keeps its parsed TerrainData and calls
// terrain_field_store_build directly.
bool terrain_field_store_load(TerrainFieldStore &store, const ResourceIndex &index,
		const std::string &terrain_name, const std::string &tile_set, std::string &error);

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
