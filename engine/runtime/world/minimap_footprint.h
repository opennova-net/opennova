#pragma once

// The minimap building footprint: the top-down projection of the model's
// type-0/1 OOBJ occlusion records, plus the silhouette edges that survive the
// authored OFAC edge-word parity toggle. The map draws the triangles filled in
// the team color. Retail builds boundary vertices too, but completed-pass
// captures show no observable stroke; entities on the footprint policy never
// draw an icon quad. OVRT source space is X/Y-up/Z, so the slice tests OPLN
// normal Y and projects vertex X/Z.
// [orig: render_collision_wireframe @0x596800 — the model's OOBJ count/pointer
//  at +0xDC/+0xE0, 60-byte records, and OVRT/OPLN/OFAC pointers at
//  +24/+32/+40 are walked @0x59690f..0x597185.]

#include <cstdint>
#include <vector>

namespace opennova::world {

struct Entity;
struct OcclusionModel;

struct MinimapFootprintMesh {
	// Model-local X/Z ground-plane coordinates, 16.16.
	std::vector<int32_t> fill_xy_q16; // x0,y0,x1,y1,x2,y2 per triangle
	std::vector<int32_t> edge_xy_q16; // x0,y0,x1,y1 per boundary edge
	bool empty() const { return fill_xy_q16.empty() && edge_xy_q16.empty(); }
};

// Walks every ordinary OOBJ record (type byte <= 1), keeping faces whose
// source-Y OPLN normal exceeds 0.5. Record positions are portal metadata;
// OVRT vertices are already model-local and are projected directly.
MinimapFootprintMesh minimap_footprint_from_occlusion(
		const OcclusionModel &model);

// The witnessed footprint fill color for a placed entity: team blue/red, the
// ChangeTeam green, gray otherwise — opaque because the map-overlay caller's
// zero alpha override is promoted to 0xFF. The byte-547 zone-state recolor
// under caps flag 0x20 is an unported residual.
// [orig: MapOverlay_DrawView @0x5a5abc; render_collision_wireframe
//  @0x596884..0x596891 and @0x596848..0x596880 —
//  0x4050A0 / 0xA05040 / itemAttrib bit17 -> 0x609F60 / 0xA0A0A0,
//  each | 0xFF << 24]
uint32_t minimap_footprint_fill_argb(const Entity &entity);

// Place model-local X/Z in mission X/Y with entityHeading-90 degrees. That
// compensates for the minimap projection's separate quarter-turn; its mission-Y
// reflection then completes retail's direct screen-space footprint matrix.
// Appends world-space Q16 pairs to the two out vectors.
// [orig: map heading @0x5a636e; relative-angle handoff @0x5be55b;
//  wireframe @0x596844..0x596bbb]
void minimap_footprint_place(const MinimapFootprintMesh &mesh,
		const Entity &entity, std::vector<int32_t> &out_fill_xy_q16,
		std::vector<int32_t> &out_edge_xy_q16);

} // namespace opennova::world
