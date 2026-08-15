#pragma once

// Portable definition/entity half of the retail minimap classifier. Network
// producers consume this typed result; packet cadence and recipient policy stay
// outside the world module.
// [orig: Entity_ClassifyForMinimap @0x50FA70]

#include <cstdint>

namespace opennova::world {

struct Entity;

struct MinimapOverlayClassification {
	uint16_t handle = 0;
	uint8_t icon = 0;
	uint8_t color = 0;
	uint8_t flags = 0;
	uint8_t source = 0;
	bool visible = false;
};

uint8_t minimap_team_color(uint8_t team);
MinimapOverlayClassification classify_minimap_overlay(const Entity &entity);
bool minimap_overlay_entity_enabled(const Entity &entity);

// The client-side blip DRAW policy — the per-def-class size/floor/rotation
// table the retail drawer selects before building the quad. Sizes are half
// extents in 16.16 world units; the floor is the on-screen pixel minimum.
// Rotation: only the rotated classes take the live heading — the upright
// classes (armory, non-vehicle EWEAPs, cells 6/2, the attrib2-bit0 class,
// dead persons) draw axis-aligned badges. Buildings with a marker model
// leave the icon path entirely (footprint = the OOBJ occlusion ground-slice
// polygons, drawn by the footprint feed).
// [orig: draw_minimap_blip @0x597890 — branch heads @0x5979e4 (armory
//  0x40000 = 4 wu, upright; re-adjudicated 2026-08-14 — the branch pushes
//  40000h), @0x597a1b/@0x597a26 (attrib2 0x2000, 4 wu),
//  @0x597a84 (Building -> render_collision_wireframe @0x596800),
//  @0x597b43 (Person 2 wu; dead upright cell 8), @0x597b87 (attrib 0x20
//  non-vehicle, 4 wu floor 4, upright), @0x597bac (attrib bit1),
//  @0x597c06 (cells 6/2 upright, floors 12/6), @0x597c13 (spawn point
//  0x40000 non-vehicle: min(model, 4 wu), floor 16), @0x597c5a
//  (attrib2 bit0: 8 wu floor 8, upright), default model halves
//  (fallback 655360) floor 6; cell 9 scales x1.2 @0x597d5a]
struct MinimapBlipDrawPolicy {
	bool rotate = true;
	bool footprint = false;
	int32_t half_x_q16 = 655360; // 10 wu class fallback
	int32_t half_y_q16 = 655360;
	uint8_t floor_px = 6;
};
MinimapBlipDrawPolicy minimap_blip_draw_policy(const Entity &entity,
		uint8_t icon);

} // namespace opennova::world
