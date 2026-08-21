#pragma once

// The impact-scar draw list: the portable compile of the world's scar rings
// into textured quads, per texture strip, for an embedding renderer to upload.
// Mirrors the particle frame seam (renderer/particle_frame.h): no Godot, no
// simulation dependencies beyond the ring cache.
// [orig: Scar_RenderAllCaches @0x5CDF70 -> Scar_RenderCache @0x5CD830, the
//  per-texture batch append Terrain_ParseSectorTypeCallback @0x5cf390]

#include <cstdint>
#include <vector>

#include "world/impact_scar.h"

namespace renderer {

// The witnessed vertex stride {x, y, z, argb, u, v} [orig: the six 24-byte
// vertices Scar_RenderCache writes].
struct ScarVertex {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	std::uint32_t argb = 0xFF000000u;
	float u = 0.0f;
	float v = 0.0f;
};

// One run of quads sharing an owner ring and a texture strip. Six vertices
// per quad in the witnessed order; vertices are mission-space world
// coordinates (the retail Y-negation is the world->D3D fold the presenter
// replaces).
struct ScarDrawBatch {
	std::uint16_t owner_packed = 0xFFFF; // EntityHandle::packed; 0xFFFF = terrain
	std::uint8_t texture = 0;            // strip index (world::scar_texture_strip_name)
	bool building = false;
	std::uint32_t first_vertex = 0;
	std::uint32_t vertex_count = 0;
};

struct ScarDrawList {
	std::vector<ScarVertex> vertices;
	std::vector<ScarDrawBatch> batches;
	// Slots seen / slots culled this compile (observability for tests + F3).
	std::uint32_t slots_live = 0;
	std::uint32_t slots_culled = 0;
};

// The per-frame inputs Scar_RenderCache reads: the camera ground position and
// the fog distance for the pre-transform box cull [orig: `|p.x - camX| <=
// fog + r && |p.y - camY| <= fog + r` before the view transform], the
// terrain light colour folded onto every vertex [orig:
// Env_TerrainLightCombined | 0xFF000000], and the owner visibility predicate
// [orig: a building owner draws when `g_BuildingSectionVisMask[idx] &
// 0xFFFFFFF` is nonzero; another owner when any of its four containing
// buildings (+464..+476) is visible, or outright when +464 == 0]. A null
// predicate treats every owner as visible.
struct ScarViewContext {
	float cam_x = 0.0f;
	float cam_y = 0.0f;
	float fog_distance = 0.0f;
	std::uint32_t terrain_light_argb = 0xFFFFFFFFu;
	bool (*owner_visible)(std::uint16_t owner_packed, void *user) = nullptr;
	void *user = nullptr;
};

// Compile every live slot of every ring into `out` (cleared first). The terrain
// ring is emitted first, then the entity rings in cache order; each ring's
// quads are grouped per texture strip into one batch [orig: the 32 per-texture
// batches at 0x2BDF8C8, flushed by Scar_RenderAllCaches].
void compile_scar_draws(const opennova::world::ScarCache &cache,
		const ScarViewContext &ctx, ScarDrawList &out);

} // namespace renderer
