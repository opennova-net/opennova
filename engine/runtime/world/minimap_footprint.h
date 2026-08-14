#pragma once

// The minimap building/zone footprint: the top-down projection of the
// collision model's UP-FACING faces, plus the silhouette edges that survive
// the shared-edge parity toggle. The map draws the triangles filled in the
// team color and outlines the surviving edges in translucent black; entities
// on the footprint policy never draw an icon quad.
// [orig: render_collision_wireframe @0x596800 — the face filter reads the
//  plane normal's up component against 0.5 (@0x596acb region), every passing
//  face emits its three vertices as a top-down triangle in the team color
//  (@0x596b42..0x596bbb), and each face edge toggles in/out of a running
//  index list so only boundary edges emit line vertices in 0x80000000
//  (@0x596afe..0x596b30, the line pass @0x596be7..)]

#include <cstdint>
#include <vector>

namespace opennova::world {

struct CollisionModel;
struct Entity;

struct MinimapFootprintMesh {
	// Model-local mission ground-plane coordinates, 16.16.
	std::vector<int32_t> fill_xy_q16; // x0,y0,x1,y1,x2,y2 per triangle
	std::vector<int32_t> edge_xy_q16; // x0,y0,x1,y1 per boundary edge
	bool empty() const { return fill_xy_q16.empty() && edge_xy_q16.empty(); }
};

// Walks every ordinary hull section's face run (COBJ type byte <= 1, section
// offsets applied), keeping faces whose Q14 normal up component exceeds 0.5.
MinimapFootprintMesh minimap_footprint_from_collision(
		const CollisionModel &model);

// The witnessed footprint fill color for a placed entity: team blue/red, the
// ChangeTeam green, gray otherwise — all under the overlay ctx alpha 0xD0
// (the same alpha the 0xD0606060 terrain tint carries; the retail capture's
// fill-over-ground blend measures it). The byte-547 zone-state recolor under
// caps flag 0x20 is an unported residual.
// [orig: render_collision_wireframe @0x596848..0x596880 —
//  0x4050A0 / 0xA05040 / itemAttrib bit17 -> 0x609F60 / 0xA0A0A0,
//  each | ctx_alpha << 24]
uint32_t minimap_footprint_fill_argb(const Entity &entity);

// Place the model-local mesh at the entity's pose: the SAME yaw-only
// placement matrix the collision instance uses (mission yaw degrees ->
// BAM heading), so the footprint, the model, and the collision shell agree.
// Appends world-space Q16 pairs to the two out vectors.
// [orig: the wireframe transforms each vertex by the entity sin/cos + pos
//  @0x596b42..0x596bbb — the entity placement collision also reads]
void minimap_footprint_place(const MinimapFootprintMesh &mesh,
		const Entity &entity, std::vector<int32_t> &out_fill_xy_q16,
		std::vector<int32_t> &out_edge_xy_q16);

} // namespace opennova::world
