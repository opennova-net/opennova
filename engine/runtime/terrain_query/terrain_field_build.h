#pragma once

// Format-typed build entries over the parsed .cpt/.trn documents for the
// terrain field store and the non-owning height field (ADR 0042 d4).
// Deliberately NOT on the ADR 0020 seam list: the seam consumers
// (engine/net, runtime/wac, runtime/mission, runtime/world) take the
// format-free terrain_field_store.h; this header is for the embedders that
// hold the parsed documents (TerrainData, Simulation, the retail-mission rig).

#include <cstdint>

#include <formats/cpt/cpt.h>
#include <formats/trn/trn.h>
#include <runtime/terrain_query/terrain_field_store.h>

namespace opennova::terrain {

// The .trn's four lock_* pairs as the portable neighbour-tap policy. One
// converter for every heightmap tap in the runtime: the height samplers (via
// the height field), the render mesh, and the collision heightfield — so no
// site can silently keep the old unconditional full-atlas wrap.
CoordsQuadrantLocks coords_locks_from(const TrnConfig &trn);

// Everything a TerrainHeightField takes from the TRN: the sector origins and
// the neighbour-tap locks. The heightmap and sector-grid POINTERS stay the
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
// apply the origins and the per-quadrant neighbour-tap locks, and point the
// SurfaceTypeMap at the (copied) charmap raster when one is supplied
// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510].
void terrain_field_store_build(TerrainFieldStore &store, const CptFile &cpt,
		const TrnConfig &trn, const uint8_t *charmap = nullptr,
		int32_t charmap_width = 0, int32_t charmap_height = 0);

} // namespace opennova::terrain
