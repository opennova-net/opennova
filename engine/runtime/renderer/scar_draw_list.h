#pragma once

// The impact-scar draw list: the portable compile of the world's scar rings
// into textured quads, per owner ring / section / texture strip, for an
// embedding renderer to upload. Mirrors the particle frame seam
// (renderer/particle_frame.h): no Godot, no simulation dependencies beyond the
// ring cache.
// [orig: Scar_RenderAllCaches @0x5CDF70 (from Terrain_CollectVisibleEntities
//  @0x5c91b7: the shared ring first, then every live entity ring) ->
//  Scar_RenderCache @0x5CD830; the 32 per-texture CDynList24 batches at
//  0x2BDF848; the drawer Scar_DrawBatches @0x5CCD10 (ex
//  Terrain_RenderFoliageBatches) after the lit sector entities]

#include <cstdint>
#include <vector>

#include "world/impact_scar.h"

namespace renderer {

// The witnessed vertex stride {x, y, z, argb, u, v} [orig: the six 24-byte
// vertices Scar_RenderCache writes; FVF 0x142].
struct ScarVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	std::uint32_t argb = 0xFF000000u;
	float u = 0.0f;
	float v = 0.0f;
};

// One run of quads sharing an owner ring, a section and a texture strip. Six
// vertices per quad in the witnessed order. A shared-ring batch is in
// mission-space world coordinates; an entity-ring batch is SECTION-LOCAL to
// `section` of the owner's model and must be drawn under that section's node
// [orig: Scar_RenderCache branches on the cache key — bone matrix
//  `bones + bone << 6` for an entity ring, the position as-is for the shared
//  ring]. (The retail Y-negation is the world->D3D fold the presenter replaces.)
struct ScarDrawBatch {
	std::uint16_t owner_packed = 0xFFFF; // EntityHandle::packed; 0xFFFF = the shared ring
	std::uint8_t texture = 0;            // strip index (world::scar_texture_strip_name)
	std::uint8_t section = 0;            // entity-local batches: the model section
	bool entity_local = false;
	bool building = false;
	std::uint32_t first_vertex = 0;
	std::uint32_t vertex_count = 0;
};

struct ScarDrawList {
	std::vector<ScarVertex> vertices;
	std::vector<ScarDrawBatch> batches;
	// Slots emitted / slots culled this compile (observability for tests + F3).
	std::uint32_t slots_live = 0;
	std::uint32_t slots_culled = 0;
};

// The per-frame inputs Scar_RenderCache reads: the camera ground position and
// the fog distance for the pre-transform box cull of world-space slots
// [orig: `|p.x - camX| <= fog + r && |p.y - camY| <= fog + r` before the view
// transform; entity-local slots are culled by the presenter with their
// owner's node], the terrain light colour folded onto every vertex [orig:
// Env_TerrainLightCombined | 0xFF000000], and the owner visibility predicate
// [orig: a building owner draws when `g_BuildingSectionVisMask[idx] &
// 0xFFFFFFF` is nonzero; another owner when any of its four containing
// buildings (+464..+476) is visible, or outright when +464 == 0]. A null
// predicate treats every owner as visible. The drawer's state is the
// presenter's: blend mode 0 (CD3DDevice_SetFogAndBlendMode), alpha test ref
// 128, the strip's one-texture mode effect with clamp wrap.
struct ScarViewContext {
	float cam_x = 0.0f;
	float cam_y = 0.0f;
	float fog_distance = 0.0f;
	std::uint32_t terrain_light_argb = 0xFFFFFFFFu;
	bool (*owner_visible)(std::uint16_t owner_packed, void *user) = nullptr;
	void *user = nullptr;
};

// Compile every live slot of every ring into `out` (cleared first): the shared
// ring first (each slot gated on its own owner's visibility), then the entity
// rings in cache order (gated once per ring), each grouped per section and
// texture strip into one batch.
void compile_scar_draws(const opennova::world::ScarCache &cache,
		const ScarViewContext &ctx, ScarDrawList &out);

} // namespace renderer
