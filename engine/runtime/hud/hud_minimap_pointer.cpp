// The spinmap's target pointers: HUD_DrawMapTargetPointer (the objective
// tethers, the nearest-FARP chevron, the waypoint pointer) with the 34-degree
// distance-label race they share, and Render_LaserSightEffect (the tracked
// callout) with its own race, plus the two tail labels. See
// hud_minimap_view.h.
// [orig: HUD_DrawMapTargetPointer @0x599220; Render_LaserSightEffect
//  @0x59D110; HUD_DrawMapOverlay @0x5a6d33..0x5a6df6 / @0x5a77f8..0x5a7893
//  / @0x5a7a53..0x5a7b8d]

#include <runtime/hud/hud_minimap_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/bam.h>
#include <base/io/fixed.h>

namespace opennova::hud::minimap_detail {

namespace {

// fpatan(dx, player_y - target_y) x dbl_7C19D8 (2^31 / pi), stored through
// a truncating fistp [orig: @0x59926F..0x59928D / @0x59D303..0x59D316].
int32_t bearing_bam(int32_t dx, int32_t neg_dy) {
	const double bam = std::atan2(static_cast<double>(dx),
			static_cast<double>(neg_dy)) * 683565275.5764316;
	return static_cast<int32_t>(static_cast<uint32_t>(
			static_cast<int64_t>(bam)));
}

// The race's angle from screen-up: (delta - 0x3FFFFFC0) / 0xB60B60 as an
// unsigned divide, folded to <= 180 [orig: @0x599542..0x599563].
int32_t race_degrees(int32_t delta) {
	int32_t deg = static_cast<int32_t>(
			(static_cast<uint32_t>(delta) - 0x3FFFFFC0u) / 0xB60B60u);
	if (deg > 180) deg = 360 - deg;
	return deg;
}

// sqrt(dx^2 + dy^2) over the Q16 deltas, clamped to flt_7C19E0 and truncated
// [orig: @0x59957C..0x59959F].
int32_t race_distance(int32_t dx, int32_t dy) {
	double dist = std::sqrt(static_cast<double>(dx) * dx +
			static_cast<double>(dy) * dy);
	dist = std::min(dist, 2147418112.0);
	return static_cast<int32_t>(dist);
}

// The label slot: floor(centre + dir * (span / 2 + span + R)), the sum
// stored as a float [orig: @0x5995C5..0x599616 — the sum @0x5995c7].
void race_anchor(const MapCompile &c, float cx, float cy, float dir_x,
		float dir_y, PointerRace &race) {
	const float reach = static_cast<float>(c.span / 2) +
			static_cast<float>(c.span) + c.view.disc_radius;
	race.label_x = static_cast<int32_t>(std::floor(
			static_cast<double>(reach * dir_x) + static_cast<double>(cx)));
	race.label_y = static_cast<int32_t>(std::floor(
			static_cast<double>(reach * dir_y) + static_cast<double>(cy)));
}

// ONE TSDicon cell centred on the tip, rotated along the bearing (cell 7's
// chevron points up, so +90 degrees lays it along (C, S)); the viewport rect
// crops it [orig: the quad @0x5994BD..0x599539 -> Render_DrawIconStripCell_Debug
// @0x5994BF].
void emit_tip_cell(MapCompile &c, float x, float y, float half, float dir_x,
		float dir_y, uint8_t cell, uint32_t argb) {
	HudMapSprite tip;
	tip.center_x = x;
	tip.center_y = y;
	tip.half_w = half;
	tip.half_h = half;
	tip.rotation_rad = std::atan2(dir_y, dir_x) + static_cast<float>(io::kPi * 0.5);
	// The raw diffuse: the strip's material 0x651 runs its MODULATE2X stage on
	// the device.
	tip.color = argb;
	marker_uv(c.input, cell, tip.u0, tip.v0, tip.u1, tip.v1);
	tip.texture = 0;
	tip.layer = 4;
	emit_cropped_sprite(c, tip, false);
}

// The line's colour, made on the CPU: each RGB byte below 0x80 doubled, any
// other 0xFF, the alpha 0xFF [orig: HUD_DrawMapTargetPointer @0x599220 —
// `cmp al, 80h; add al, al` / `mov cl, 0FFh` @0x5993b9..0x5993f8, the alpha
// @0x599400].
uint32_t line_color_doubled(uint32_t argb) {
	const auto doubled = [&](int shift) -> uint32_t {
		const uint32_t c = (argb >> shift) & 0xFFu;
		return (c < 0x80u ? c * 2u : 0xFFu) << shift;
	};
	return 0xFF000000u | doubled(16) | doubled(8) | doubled(0);
}

void emit_line(MapCompile &c, float x0, float y0, float x1, float y1,
		uint32_t argb) {
	if (!clip_segment_rect(c.view, x0, y0, x1, y1)) return;
	c.out.lines.push_back({x0, y0, x1, y1, argb});
}

// "%03dm" up to 1000 m, else "%01.2fk" of metres x flt_7C69E8 (0.001f).
void format_distance(char *buf, size_t size, int32_t metres, bool padded) {
	if (metres <= 1000) {
		std::snprintf(buf, size, padded ? "%03dm" : "%dm", metres);
	} else {
		std::snprintf(buf, size, padded ? "%01.2fk" : "%1.2fk",
				static_cast<double>(metres) * static_cast<double>(0.001f));
	}
}

} // namespace

// The pointer: the bearing from the player to the target minus ctx[11] (no
// mission yaw) through the Q22 table, the tip clamped to R - span from the
// centre while the projected target lies outside that circle (chevron cell 7,
// half-size span) and AT the target once inside (dot cell 1, half-size 0.75
// span, the colour halved on frames with counter bit 5). The line from the
// centre to the tip rides the 2c-saturated colour; the cell takes the colour
// through the strip's MODULATE2X. A drawn cell enters the label race: the
// first within 34 degrees of screen-up (closer wins) stores its distance less
// the caller's offset and its label slot.
// [orig: HUD_DrawMapTargetPointer @0x599220 — bearing @0x59925A..0x5992A2,
//  tip @0x5992C5 (R - span), inside d^2 < (R - span)^2 @0x599340..0x599351,
//  lodLevel = 1 @0x59935e, inside tip/0.75 span @0x599366..0x59937e
//  (flt_7C3DC8), blink halve
//  @0x599384..0x599397, line 2c saturate @0x5993b5..0x5993f8 pass 0x200000,
//  cell gates @0x599497..0x5994b7, race @0x599542..0x599616]
void draw_map_target_pointer(MapCompile &c, int32_t target_x, int32_t target_y,
		uint32_t color, bool line, bool dot_inside, bool chevron_outside,
		int32_t distance_offset) {
	const HudMinimapInput &input = c.input;
	const MapView &view = c.view;
	const int32_t dx = static_cast<int32_t>(
			static_cast<uint32_t>(target_x) - static_cast<uint32_t>(input.player_x));
	const int32_t dy = static_cast<int32_t>(
			static_cast<uint32_t>(target_y) - static_cast<uint32_t>(input.player_y));
	const int32_t neg_dy = static_cast<int32_t>(
			static_cast<uint32_t>(input.player_y) - static_cast<uint32_t>(target_y));
	const int32_t delta = static_cast<int32_t>(
			static_cast<uint32_t>(bearing_bam(dx, neg_dy)) -
			static_cast<uint32_t>(view.heading_bam));
	const int idx = io::bam_table_index(static_cast<uint32_t>(delta) + 0x200000u);
	const float dir_x = static_cast<float>(io::bam_table_cos(idx));
	const float dir_y = -static_cast<float>(io::bam_table_sin(idx));
	float cx = 0.0f, cy = 0.0f;
	view_project(view, input, input.player_x, input.player_y, cx, cy);
	float tx = 0.0f, ty = 0.0f;
	view_project(view, input, target_x, target_y, tx, ty);
	const float span = static_cast<float>(c.span);
	const float clamp_r = view.disc_radius - span;
	float tip_x = cx + clamp_r * dir_x;
	float tip_y = cy + clamp_r * dir_y;
	const float ddx = tx - cx;
	const float ddy = ty - cy;
	const double d2 = std::min(static_cast<double>(ddx) * ddx +
			static_cast<double>(ddy) * ddy, 2147483647.0);
	const bool inside = static_cast<double>(static_cast<int32_t>(d2)) <
			static_cast<double>(clamp_r) * clamp_r;
	uint8_t cell = 7;
	float half = span;
	uint32_t light = color;
	if (inside) {
		cell = 1;
		tip_x = tx;
		tip_y = ty;
		half = span * 0.75f;
		if ((static_cast<uint32_t>(input.ticks) & 0x20u) != 0)
			light = (color & 0xFF000000u) + ((color >> 1) & 0x7F7F7Fu);
	}
	if (line) emit_line(c, cx, cy, tip_x, tip_y, line_color_doubled(light));
	if (!(cell == 7 ? chevron_outside : dot_inside)) return;
	emit_tip_cell(c, tip_x, tip_y, half, dir_x, dir_y, cell,
			light | 0xFF000000u);
	const int32_t deg = race_degrees(delta);
	if (deg < c.race.best_deg) {
		c.race.best_deg = deg;
		const int32_t dist = race_distance(dx, dy);
		c.race.distance = dist <= distance_offset ? 0 : dist - distance_offset;
		race_anchor(c, cx, cy, dir_x, dir_y, c.race);
	}
}

// bit2: every non-special slot whose live entity is a zone def (attrib
// 0x40000) with a zone number and an objective/defensive slot bit gets a
// tethered pointer in its team colour (1 0xFF304080, 2 0xFF802020, else the
// hudpos text colour) — line on, dot off, chevron on, the distance less the
// entity+0 radius. Any drawn tether suppresses the waypoint pointer.
// [orig: HUD_DrawMapOverlay @0x5a6d33..0x5a6df6 — the 1160-slot walk from
//  word_28E5620 (transient, then persistent; the 0x40 special slots
//  skipped), HIBYTE(v142) = 1 @0x5a6deb]
void draw_objective_tethers(MapCompile &c) {
	for (const HudMinimapBank bank :
			{HudMinimapBank::kTransient, HudMinimapBank::kPersistent}) {
		for (const HudMinimapMarker &m : c.input.markers) {
			if (static_cast<HudMinimapBank>(m.bank) != bank) continue;
			if ((m.flags & 0x40u) != 0 || !m.entity_known) continue;
			if ((m.entity_bits & kMarkerEntityZoneDef) == 0 ||
					m.zone_number == 0 || (m.source & 0xC0u) == 0)
				continue;
			const uint32_t color = m.team == 1 ? kMapOverlayTeam1
					: (m.team == 2 ? kMapOverlayTeam2 : c.input.hudpos_text_color);
			draw_map_target_pointer(c, m.entity_x, m.entity_y, color, true,
					false, true, m.bound_radius_q16);
			c.tether_drawn = true;
		}
	}
}

// bit7: the nearest FARP as a chevron-only pointer in g_HUDColors.active.
// [orig: @0x5a7817..0x5a7834 — line 0, dot 0, chevron 1, offset 0,
//  target g_TrackedTargetPos]
void draw_farp_pointer(MapCompile &c) {
	draw_map_target_pointer(c, c.input.farp_x, c.input.farp_y,
			c.input.overlay_color, false, false, true, 0);
}

// bit8: the waypoint pointer in the altitude tricolour, every part on.
// [orig: the bit8 leg @0x5a7850..0x5a78a0 — @0x5a7874..0x5a7893
//  g_WaypointAltitudeColor, target g_WaypointPosXY]
void draw_waypoint_pointer(MapCompile &c, uint32_t color) {
	draw_map_target_pointer(c, c.input.waypoint_x, c.input.waypoint_y, color,
			true, true, true, 0);
}

// bit18: the race's distance when one was stored, unless an authored
// nonzero SPINMAPWPDISTOFF suppresses it — bold, "centred" half-bright in
// the frame overlay colour at the race's label slot.
// [orig: @0x5a7a51..0x5a7ae4 — ctx[20] != 0, g_SpinmapWpDistLabelOff
//  (dword_27237C0), (ctx[17], ctx[18])]
void draw_race_label(MapCompile &c) {
	if (c.race.distance == 0 || c.input.waypoint_distance_offset != 0) return;
	HudMapLabel label;
	label.x = static_cast<float>(c.race.label_x);
	label.y = static_cast<float>(c.race.label_y);
	label.color = c.input.overlay_color;
	format_distance(label.text, sizeof(label.text), c.race.distance / 0x10000,
			true);
	c.out.labels.push_back(label);
}

// bit19, the tracked callout: while the tracked timer runs (a radio-request
// target only for a viewer in a controller/driver seat) its alpha is
// min(255, 255 * timer / 186). Outside the R - span circle: a line and a
// cell-7 chevron at half the alpha (the stored alpha halves too) plus its own
// race; inside: the colour blinks at full alpha and an ENEMY target draws the
// cell-28 blip at its set-time snapshot (a friendly one draws in the bit5
// walk). A radio-request target draws only outside, and never aboard a
// vehicle. The position is the snapshot for an enemy or a hidden entity,
// the live one otherwise.
// [orig: Render_LaserSightEffect @0x59D110 — resets @0x59D121/@0x59D12B,
//  viewer gate @0x59D148..0x59D16D, alpha @0x59D173..0x59D1A3, ED0
//  @0x59D1A8..0x59D1BD, position @0x59D1D8..0x59D209, inside test
//  @0x59D23A..0x59D289, radio arm @0x59D291..0x59D2BB, outside
//  @0x59D2C6..0x59D41D, race @0x59D49A..0x59D639, inside @0x59D50A..0x59D5B0]
void draw_tracked_callout(MapCompile &c) {
	c.tracked_race = PointerRace{};
	const HudMinimapOverlays *ov = c.input.overlays;
	if (ov == nullptr) return;
	const HudMinimapTracked &t = ov->tracked;
	if (t.ticks == 0) return;
	if (t.radio_request && !t.viewer_mount_ok) return;
	const int32_t alpha = std::min(255, 255 * t.ticks / 186);
	c.tracked_alpha = static_cast<uint8_t>(alpha);
	const uint32_t base = t.radio_request ? kHudPaletteLightBlue : 0xFFFFFFFFu;
	c.tracked_color = base;
	uint32_t color_base = (static_cast<uint32_t>(alpha) << 24) + (base & 0xFFFFFFu);
	int32_t pos_x = t.snap_x;
	int32_t pos_y = t.snap_y;
	if (t.friendly && !t.hidden_bit && t.live_known) {
		pos_x = t.live_x;
		pos_y = t.live_y;
	}
	const HudMinimapInput &input = c.input;
	const MapView &view = c.view;
	float cx = 0.0f, cy = 0.0f;
	view_project(view, input, input.player_x, input.player_y, cx, cy);
	float tx = 0.0f, ty = 0.0f;
	view_project(view, input, pos_x, pos_y, tx, ty);
	const float span = static_cast<float>(c.span);
	const float clamp_r = view.disc_radius - span;
	const float ddx = tx - cx;
	const float ddy = ty - cy;
	const double d2 = std::min(static_cast<double>(ddx) * ddx +
			static_cast<double>(ddy) * ddy, 2147483647.0);
	const bool outside = static_cast<double>(static_cast<int32_t>(d2)) >=
			static_cast<double>(clamp_r) * clamp_r;
	if (t.radio_request) {
		if (!outside || t.aboard_vehicle) return;
	}
	if (outside) {
		const int32_t dx = static_cast<int32_t>(
				static_cast<uint32_t>(pos_x) - static_cast<uint32_t>(input.player_x));
		const int32_t dy = static_cast<int32_t>(
				static_cast<uint32_t>(pos_y) - static_cast<uint32_t>(input.player_y));
		const int32_t neg_dy = static_cast<int32_t>(
				static_cast<uint32_t>(input.player_y) - static_cast<uint32_t>(pos_y));
		const int32_t delta = static_cast<int32_t>(
				static_cast<uint32_t>(bearing_bam(dx, neg_dy)) -
				static_cast<uint32_t>(view.heading_bam));
		const int idx = io::bam_table_index(static_cast<uint32_t>(delta) + 0x200000u);
		const float dir_x = static_cast<float>(io::bam_table_cos(idx));
		const float dir_y = -static_cast<float>(io::bam_table_sin(idx));
		const uint32_t half_alpha = (color_base & 0xFFFFFFu) +
				(static_cast<uint32_t>(c.tracked_alpha >> 1) << 24);
		c.tracked_alpha = static_cast<uint8_t>(c.tracked_alpha >> 1);
		const float tip_x = cx + clamp_r * dir_x;
		const float tip_y = cy + clamp_r * dir_y;
		emit_line(c, cx, cy, tip_x, tip_y, half_alpha);
		emit_tip_cell(c, tip_x, tip_y, span, dir_x, dir_y, 7, half_alpha);
		const int32_t deg = race_degrees(delta);
		if (deg < c.tracked_race.best_deg) {
			c.tracked_race.best_deg = deg;
			c.tracked_race.distance = race_distance(dx, dy);
			race_anchor(c, cx, cy, dir_x, dir_y, c.tracked_race);
		}
		return;
	}
	if (alpha == 255 && (static_cast<uint32_t>(input.ticks) & 0x20u) != 0)
		color_base = ((color_base >> 1) & 0x7F7F7Fu) + (color_base & 0xFF000000u);
	c.tracked_color = color_base;
	if (t.friendly) return;
	// The enemy blip at the snapshot: the entity's position is swapped for
	// the stored one around the draw, so the anchor keeps its bbox offset.
	// [orig: @0x59D570..0x59D5C9 — Minimap_DrawBlip(e, rect, 0x40000000,
	//  alpha, colour, 28)]
	HudMinimapMarker blip = t.blip;
	blip.anchor_x = static_cast<int32_t>(static_cast<uint32_t>(t.snap_x) +
			(static_cast<uint32_t>(blip.anchor_x) - static_cast<uint32_t>(blip.entity_x)));
	blip.anchor_y = static_cast<int32_t>(static_cast<uint32_t>(t.snap_y) +
			(static_cast<uint32_t>(blip.anchor_y) - static_cast<uint32_t>(blip.entity_y)));
	blip.entity_x = t.snap_x;
	blip.entity_y = t.snap_y;
	draw_blip(c, blip, color_base, 28, 0x40000000, static_cast<uint8_t>(alpha), 4);
}

// bit19's label: the laser race's distance ("%dm" / "%1.2fk"), bold, in
// dword_2721ED0 carrying byte_2721ED4 as its alpha through the one map
// drawer that keeps the alpha.
// [orig: @0x5a7aec..0x5a7b8d — HUD_DrawTextHalfBrightF @0x580720]
void draw_tracked_label(MapCompile &c) {
	if (c.tracked_race.distance == 0) return;
	HudMapLabel label;
	label.x = static_cast<float>(c.tracked_race.label_x);
	label.y = static_cast<float>(c.tracked_race.label_y);
	label.color = (c.tracked_color & 0xFFFFFFu) |
			(static_cast<uint32_t>(c.tracked_alpha) << 24);
	label.keep_alpha = 1;
	format_distance(label.text, sizeof(label.text),
			c.tracked_race.distance / 0x10000, false);
	c.out.labels.push_back(label);
}

} // namespace opennova::hud::minimap_detail
