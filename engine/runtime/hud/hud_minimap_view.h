#pragma once

// INTERNAL to the spinmap compiler TUs (hud_minimap*.cpp): the per-pass view
// transform, the clip helpers, and the compile context the leg TUs share.
// One responsibility per TU: hud_minimap.cpp the view, terrain, grid and the
// pass order; hud_minimap_banks.cpp the marker-bank walk and the blip drawer;
// hud_minimap_rings.cpp the ring band and the pool-3 walk;
// hud_minimap_pointer.cpp the target pointer and the tracked callout;
// hud_minimap_labels.cpp the zone/route/name legs.
// [orig: HUD_DrawMapOverlay @0x5A5F40]

#include <runtime/hud/hud_minimap.h>

#include <cstdint>
#include <vector>

namespace opennova::hud::minimap_detail {

struct MapView {
	float center_x = 0.0f;
	float center_y = 0.0f;
	// The rect half-height (the compass base) and the disc radius var_354:
	// the half-height minus scaleX((mask >> 8) & 2) — the bit9-gated inset
	// [orig: @0x5a60d8 edi = 2; @0x5a64cd..0x5a651d].
	float base_radius = 0.0f;
	float disc_radius = 0.0f;
	float rect_w = 0.0f;
	float rect_h = 0.0f;
	// The viewport: the scaled rect, squared on its half-height by bit16
	// [orig: @0x5a63bf..0x5a642e].
	float px_x1 = 0.0f;
	float px_y1 = 0.0f;
	float px_x2 = 0.0f;
	float px_y2 = 0.0f;
	float scale = 1.0f; // world units per pixel
	float sin_a = 0.0f;
	float cos_a = 1.0f;
	// alpha = g_MapYaw180 + ctx[11]: the map rotation before the -90 fold,
	// the blip angle base [orig: @0x5a636e].
	uint32_t base_angle_bam = 0;
	// ctx[11] alone (no yaw): the pointer drawers' reference angle
	// [orig: HUD_DrawMapTargetPointer @0x59929F; Render_LaserSightEffect
	//  @0x59D328].
	int32_t heading_bam = 0;
};

// The pass's content mask: the corner input's own, or the big-map mask the
// view builder derives from its parameter block and mode.
uint32_t effective_flags(const HudMinimapInput &input);
MapView make_view(const HudMinimapInput &input, uint32_t flags);

// Mission Q16 -> map pixels through the map transform.
// [orig: Terrain_FixedPointToWorldFloat_Default @0x607060]
void view_project(const MapView &view, const HudMinimapInput &input,
		int32_t world_x_q16, int32_t world_y_q16, float &out_x, float &out_y);
// The same transform with the identity rotation the label legs pass
// (cos 1, sin 0) [orig: Terrain_FixedPointToWorldFloat @0x5a75cc /
//  @0x5a7773 / @0x5a4e73 / @0x5a4f64].
void view_project_unrotated(const MapView &view, const HudMinimapInput &input,
		int32_t world_x_q16, int32_t world_y_q16, float &out_x, float &out_y);
float view_length_px(const MapView &view, float world_units);

using Polygon = std::vector<HudMapVertex>;
void clip_rect(Polygon &poly, Polygon &scratch, float x1, float y1, float x2,
		float y2);
void clip_circle32(Polygon &poly, Polygon &scratch, float cx, float cy,
		float rx, float ry);
void emit_fan(const Polygon &poly, uint32_t color, std::vector<HudMapTri> &out);
bool clip_segment_rect(const MapView &view, float &x0, float &y0, float &x1,
		float &y1);
// Render_CollisionWireframe's fills for one footprint-class entity.
void emit_footprint(const MapView &view, const HudMinimapInput &input,
		const HudMinimapFootprint &footprint, Polygon &poly, Polygon &scratch,
		HudMapPass &pass, bool disc);

void marker_uv(const HudMinimapInput &input, uint8_t icon, float &u0,
		float &v0, float &u1, float &v1);
// The 64-frame colour pulse toward white.
uint32_t pulse_color(uint32_t argb, int ticks);
// ((frame - 8) & 0x3F) folded above 0x20 — the pulse phase every leg shares
// [orig: Render_MinimapSlotBlip @0x5be3f4; @0x5a708f; @0x5a4b97].
uint32_t pulse_phase(int ticks);
// The byte scale toward white c += phase * (255 - c) >> 5, per RGB channel.
uint32_t pulse_toward_white(uint32_t argb, uint32_t phase);

// ctx[17]/[18] (the label anchor), ctx[20] (the distance) and ctx[21] (the
// best angle, seeded 34) — the race every HUD_DrawMapTargetPointer call runs,
// and the laser's separate twin dword_2721ED8/EDC/EE4/EE8.
struct PointerRace {
	int32_t best_deg = 34;
	int32_t distance = 0;
	int32_t label_x = 0;
	int32_t label_y = 0;
};

struct MapCompile {
	const HudMinimapInput &input;
	const MapView &view;
	uint32_t flags;
	HudMapPass &out;
	Polygon &clip_a;
	Polygon &clip_b;
	std::vector<HudMapGeomVertex> &geom_a;
	std::vector<HudMapGeomVertex> &geom_b;
	std::vector<const HudMinimapFootprint *> &footprint_index;
	// The bit0 mask still crops z-tested draws after the terrain pass: laid
	// by bit0 and not overwritten by an unmasked (bit9-clear) terrain pass
	// [orig: @0x5a6527 mask; Render_TerrainDecal pass 0x500000 / 0x600000
	//  @0x6071F7].
	bool disc_crop = false;
	PointerRace race;
	bool tether_drawn = false; // HIBYTE(v142) [orig: @0x5a6deb]
	uint32_t &tracked_color;   // dword_2721ED0
	uint8_t &tracked_alpha;    // byte_2721ED4
	PointerRace tracked_race;  // dword_2721ED8/EDC/EE4/EE8
	int32_t span = 10;         // scaleY(10) [orig: renderFlags @0x5a64c5]
};

// A texture quad (corners in the TL, TR, BR, BL order of `sprite`'s UVs)
// cropped to the pass: whole when inside, dropped when outside, clipped
// geometry across the edge. The crop is the disc when the mask survives, the
// viewport rect otherwise — retail's per-pixel depth/viewport crop.
void emit_cropped_sprite(MapCompile &c, const HudMapSprite &sprite, bool disc);
// Render_DrawRingOverlay's four-radius anti-aliased band (and centre fan when
// the fill is nonzero), cropped like the markers.
void emit_ring_band(MapCompile &c, float cx, float cy, float radius,
		uint32_t ring_argb, uint32_t fill_argb, uint8_t layer);
// Minimap_DrawRingBlip: the centre projected, the radius max(min_px,
// projected z), the ring colour forced opaque.
void ring_blip(MapCompile &c, int32_t x_q16, int32_t y_q16, int32_t z_q16,
		uint32_t color, uint32_t fill, int min_px, uint8_t layer);

// Minimap_DrawBlip over a marker's entity facts.
void draw_blip(MapCompile &c, const HudMinimapMarker &marker, uint32_t color,
		uint8_t cell, int32_t angle_bam, uint8_t alpha, uint8_t layer);

// The legs, in pass order.
void render_all_layers(MapCompile &c);             // bit1
void draw_objective_tethers(MapCompile &c);        // bit2
void draw_route_lines(MapCompile &c);              // bit15
void draw_zone_waypoint_labels(MapCompile &c);     // bit14
void draw_zone_letters(MapCompile &c);             // bit3
void draw_pool3_walk(MapCompile &c);               // mask & 0x810
void draw_player_waypoints(MapCompile &c);         // bit17
void draw_entity_labels_and_markers(MapCompile &c); // bit5 (+bit13)
void draw_tracked_callout(MapCompile &c);          // bit19
void draw_farp_pointer(MapCompile &c);             // bit7
void draw_waypoint_pointer(MapCompile &c, uint32_t color); // bit8
// The tail labels after the viewport restore.
void draw_race_label(MapCompile &c);               // bit18
void draw_tracked_label(MapCompile &c);            // bit19

// HUD_DrawMapTargetPointer: a line + ONE strip cell toward a world target.
void draw_map_target_pointer(MapCompile &c, int32_t target_x, int32_t target_y,
		uint32_t color, bool line, bool dot_inside, bool chevron_outside,
		int32_t distance_offset);

} // namespace opennova::hud::minimap_detail
