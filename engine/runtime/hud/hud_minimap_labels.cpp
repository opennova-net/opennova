// The spinmap's zone, route and name legs: the bit15 route lines, the bit14
// zone waypoint labels, the bit3 zone letters, the bit17 player waypoints and
// HUD_DrawEntityLabelsAndMarkers (bit5: the player-slot markers, bit13 their
// names and the same-team persons' names). See hud_minimap_view.h.
// [orig: HUD_DrawMapOverlay @0x5a6dfc..0x5a74fa / @0x5a76fe..0x5a77cb;
//  HUD_DrawEntityLabelsAndMarkers @0x5A49E0]

#include <runtime/hud/hud_minimap_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_medic_cross.h>

namespace opennova::hud::minimap_detail {

namespace {

// g_SquadColors @0x83B450 — the fourteen authored squad tints.
constexpr uint32_t kSquadColors[14] = {
	0xFF000000u, 0xFFA000F0u, 0xFFFF9999u, 0xFFFFCC99u, 0xFFFFFF99u,
	0xFFCCFF99u, 0xFF99FF99u, 0xFF99FFCCu, 0xFF99FFFFu, 0xFF99CCFFu,
	0xFF9999FFu, 0xFFCC99FFu, 0xFFFF99FFu, 0xFFFF99CCu,
};

// The 1160-slot walks run the transient bank, then the persistent one, and
// skip the special slots (flag 0x40) [orig: word_28E5620 + 1160 * 32 spans
//  the three contiguous banks; `(slot+3) & 0x40` skips @0x5a6d50].
template <typename Fn>
void walk_regular_slots(const MapCompile &c, Fn &&fn) {
	for (const HudMinimapBank bank :
			{HudMinimapBank::kTransient, HudMinimapBank::kPersistent}) {
		for (const HudMinimapMarker &m : c.input.markers) {
			if (static_cast<HudMinimapBank>(m.bank) != bank) continue;
			if ((m.flags & 0x40u) != 0) continue;
			fn(m);
		}
	}
}

// The zone labels' anchor: the rotated projection of the placement-matrix
// bbox centre, less half of scaleX(8) and scaleY(16), truncated.
// [orig: @0x5a7284..0x5a730a / @0x5a743a..0x5a74b3]
void zone_label_anchor(const MapCompile &c, const HudMinimapMarker &m,
		float &x, float &y) {
	float px = 0.0f, py = 0.0f;
	view_project(c.view, c.input, m.anchor_x, m.anchor_y, px, py);
	const int half_x = static_cast<int>(scale_axis(8.0, c.input.surface_w,
			kDesignWidth)) >> 1;
	const int half_y = static_cast<int>(scale_axis(16.0, c.input.surface_h,
			kDesignHeight)) >> 1;
	x = static_cast<float>(static_cast<int>(px - static_cast<float>(half_x)));
	y = static_cast<float>(static_cast<int>(py - static_cast<float>(half_y)));
}

void push_label(MapCompile &c, float x, float y, const std::string &text,
		uint32_t color, uint8_t font, uint8_t align) {
	HudMapLabel label;
	label.x = x;
	label.y = y;
	std::snprintf(label.text, sizeof(label.text), "%s", text.c_str());
	label.color = color;
	label.font = font;
	label.align = align;
	label.clip = 1;
	c.out.labels.push_back(label);
}

std::string zone_letter(int16_t zone_index) {
	return std::string(1, static_cast<char>(zone_index + 'A'));
}

} // namespace

// bit15 (g_GameType 0x10010 only): every pair of non-special slots with
// nonzero source bytes, distinct live entities and adjacent route numbers
// ((source & 0x3F) differing by exactly 1) draws a line between the two
// entities, each end in its own slot colour made opaque — every pair twice
// (A->B and B->A).
// [orig: @0x5a6dfc..0x5a6fbc — outer/inner 1160-slot walks, the slot+20
//  colours | 0xFF000000 @0x5a6f3b/@0x5a6f5c, pass 0x200000 @0x5a6f47]
void draw_route_lines(MapCompile &c) {
	walk_regular_slots(c, [&](const HudMinimapMarker &a) {
		if (a.source == 0 || !a.entity_known) return;
		float ax = 0.0f, ay = 0.0f;
		view_project(c.view, c.input, a.entity_x, a.entity_y, ax, ay);
		walk_regular_slots(c, [&](const HudMinimapMarker &b) {
			if (b.source == 0 || !b.entity_known || b.handle == a.handle) return;
			const int step = static_cast<int>(a.source & 0x3Fu) -
					static_cast<int>(b.source & 0x3Fu);
			if (step != 1 && step != -1) return;
			float bx = 0.0f, by = 0.0f;
			view_project(c.view, c.input, b.entity_x, b.entity_y, bx, by);
			float x0 = ax, y0 = ay, x1 = bx, y1 = by;
			if (!clip_segment_rect(c.view, x0, y0, x1, y1)) return;
			HudMapLine line;
			line.x0 = x0;
			line.y0 = y0;
			line.x1 = x1;
			line.y1 = y1;
			line.color = a.color | 0xFF000000u;
			line.color_end = b.color | 0xFF000000u;
			c.out.lines.push_back(line);
		});
	});
}

// bit14: every zone-def slot's label. A numbered zone shows its WPNames name
// — wrapped in STROVER_OBJECTIVEPOINT_SHORT (source bit 7) or
// STROVER_DEFENSIVEPOSITION (bit 6) and pulsed toward white (the pulse
// frozen while dword_A85B68 runs), else at 0.75 brightness; an unnumbered
// zone its letter in the plain team colour (1 0xFF304080, 2 0xFF802020,
// else 0xFF208020).
// [orig: @0x5a6fc2..0x5a7329 — SpawnZoneList_IndexOf @0x5a7058, colours
//  @0x5a705e..0x5a708b, phase @0x5a708f..0x5a70b3, "STRWPNAME%03d" idx + 1
//  @0x5a70c4, the three text arms @0x5a70dd..0x5a7257, "%c" @0x5a726b]
void draw_zone_waypoint_labels(MapCompile &c) {
	const HudMinimapOverlays *ov = c.input.overlays;
	walk_regular_slots(c, [&](const HudMinimapMarker &m) {
		if (!m.entity_known || (m.entity_bits & kMarkerEntityZoneDef) == 0) return;
		uint32_t color = m.team == 1 ? kMapOverlayTeam1
				: (m.team == 2 ? kMapOverlayTeam2 : kMapOverlayNeutral);
		uint32_t phase = pulse_phase(c.input.ticks);
		if (ov != nullptr && ov->zone_timer != 0) phase = 0;
		std::string text;
		if (m.zone_number != 0) {
			const std::string name = ov != nullptr && m.zone_index >= 0 &&
							static_cast<size_t>(m.zone_index) < ov->zone_wp_names.size()
					? ov->zone_wp_names[static_cast<size_t>(m.zone_index)]
					: std::string();
			if ((m.source & 0x80u) != 0) {
				text = hud_sprintf(ov != nullptr ? ov->objective_point_format
						: std::string(), {HudTextArg{name, 0, false}});
				color = pulse_toward_white(color, phase);
			} else if ((m.source & 0x40u) != 0) {
				text = hud_sprintf(ov != nullptr ? ov->defensive_position_format
						: std::string(),
						{HudTextArg{name, 0, false}, HudTextArg{{}, 0, true}});
				color = pulse_toward_white(color, phase);
			} else {
				text = name;
				uint32_t dim = color & 0xFF000000u;
				for (int shift = 0; shift <= 16; shift += 8) {
					const uint32_t ch = (color >> shift) & 0xFFu;
					dim |= ((ch >> 1) + (ch >> 2)) << shift;
				}
				color = dim;
			}
		} else {
			text = zone_letter(m.zone_index);
		}
		float x = 0.0f, y = 0.0f;
		zone_label_anchor(c, m, x, y);
		push_label(c, x, y, text, color, 0, 0);
	});
}

// bit3: every zone-def slot whose def type is not 1 draws its letter, twice
// at the same spot (1 0xFF80A0FF, 2 0xFFFF9090, else 0xFF80FF80).
// [orig: @0x5a732f..0x5a74fa — the two HUD_DrawTextCentered_HalfBright
//  calls @0x5a74be / @0x5a74ea]
void draw_zone_letters(MapCompile &c) {
	walk_regular_slots(c, [&](const HudMinimapMarker &m) {
		if (!m.entity_known || (m.entity_bits & kMarkerEntityZoneDef) == 0) return;
		if (m.def_type == 1) return;
		const uint32_t color = m.team == 1 ? 0xFF80A0FFu
				: (m.team == 2 ? 0xFFFF9090u : 0xFF80FF80u);
		float x = 0.0f, y = 0.0f;
		zone_label_anchor(c, m, x, y);
		const std::string text = zone_letter(m.zone_index);
		push_label(c, x, y, text, color, 0, 0);
		push_label(c, x, y, text, color, 0, 0);
	});
}

// bit17: each pool-4 player waypoint's name, bold, LEFT-aligned at its
// unrotated projection, (ctx+64 alpha << 24) + 0x9F9F9F through the
// alpha-forcing drawer.
// [orig: @0x5a76fe..0x5a77ac — HUD_DrawTextLeft_HalfBright @0x5a779d]
void draw_player_waypoints(MapCompile &c) {
	const HudMinimapOverlays *ov = c.input.overlays;
	if (ov == nullptr) return;
	for (const HudMinimapPlayerWaypoint &w : ov->player_waypoints) {
		float px = 0.0f, py = 0.0f;
		view_project_unrotated(c.view, c.input, w.x, w.y, px, py);
		push_label(c, static_cast<float>(static_cast<int>(px)),
				static_cast<float>(static_cast<int>(py)), w.name, 0xFF9F9F9Fu, 0, 2);
	}
}

// bit5 — HUD_DrawEntityLabelsAndMarkers(rect, the map angle, 1, bit13, the
// ctx+60 alpha). Loop 1 walks the player-slot pointer table: the HUD team's
// slots (and every slot while enemy tags are granted, the medic bit then
// cleared) draw a marker in the team or squad colour — a dead player the
// cell-8 body plus, while revivable, the blue/pulsed cell-14 revive mark; a
// radio-requesting player off any vehicle cell 23; the tracked target at full
// alpha cell 28; a medic the red cross; else cell 3 — then the tracked
// target's fading cell 28, then (bit13) the slot name with its clan tag.
// Loop 2 (bit13) names the same-team transient-bank persons.
// [orig: HUD_DrawEntityLabelsAndMarkers @0x5A49E0 — loop 1 @0x5a4a54..
//  0x5a4eb4, loop 2 @0x5a4ede..0x5a4fa3]
void draw_entity_labels_and_markers(MapCompile &c) {
	const HudMinimapOverlays *ov = c.input.overlays;
	if (ov == nullptr || !ov->hud_present) return;
	const bool names = (c.flags & 0x2000u) != 0;
	// The ctx+60 alpha: 255 on the corner map, 96 on the modes-1..3 big map,
	// 255 on the mode-4 windows (CMAP ctx; the DEATH view passes 0xFF).
	// [orig: HUD_RenderAllOverlays rayStart[18] = 255 @0x5a81b6;
	//  HUD_BuildMapOverlayView color_red = 96 @0x5a7e3c, 255 @0x5a7f92;
	//  MapOverlay_DrawView @0x5a5a29]
	const uint8_t alpha = (c.input.map_mode == 2 || c.input.map_mode == 3) ? 96 : 255;
	// The view angle without the windowed walk's bias (the DEATH view walks
	// its banks at 0x3FFFFFC0 but labels at 0x40000000).
	int32_t map_angle = static_cast<int32_t>(c.view.base_angle_bam);
	if (c.input.map_mode == 4)
		map_angle = static_cast<int32_t>(static_cast<uint32_t>(map_angle) -
				static_cast<uint32_t>(c.input.window_marker_angle_bias_bam));
	const HudMinimapTracked &tracked = ov->tracked;
	const bool tracking = tracked.ticks != 0;
	for (const HudMinimapPlayerSlot &slot : ov->player_slots) {
		if (!slot.active) continue;
		const HudMinimapMarker &e = slot.blip;
		bool medic = slot.medic;
		if (e.team != ov->hud_team) {
			medic = false;
			if (!ov->enemy_tags_visible) continue;
		}
		uint32_t color = e.team == 1 ? kMapOverlayTeam1
				: (e.team == 2 ? kMapOverlayTeam2 : kMapOverlayNeutral);
		if (slot.squad != 0 && slot.squad < 14) {
			const uint32_t squad = kSquadColors[slot.squad];
			color = (squad & 0xFF000000u) | ((squad >> 1) & 0x7F7F7Fu);
		}
		const int32_t heading_angle = static_cast<int32_t>(
				static_cast<uint32_t>(map_angle) - static_cast<uint32_t>(e.heading_bam));
		const bool tracked_here = tracking && e.handle == tracked.blip.handle;
		if ((e.entity_bits & kMarkerEntityDead) != 0) {
			if (slot.revivable) {
				// [orig: @0x5a4b2e..0x5a4c32 — pulse_phase = 0xFF304080]
				uint32_t revive = 0xFF304080u;
				bool body = false;
				if (!slot.medic_request) {
					body = slot.own_slot && ov->own_revive_profile;
				} else {
					const uint32_t ph = pulse_phase(c.input.ticks);
					uint32_t v = 0xFF304080u;
					const uint32_t r = ((207u * ph) >> 5) + 48u;
					const uint32_t g = 0x40u + ((ph * (255u - 0x40u)) >> 5);
					const uint32_t b = 0x80u + ((ph * (255u - 0x80u)) >> 5);
					v = (v & 0xFF000000u) | ((r & 0xFFu) << 16) |
							((g & 0xFFu) << 8) | (b & 0xFFu);
					revive = v;
				}
				if (body) draw_blip(c, e, color, 8, map_angle, alpha, 3);
				draw_blip(c, e, revive, 14, map_angle, alpha, 3);
			} else {
				draw_blip(c, e, color, 8, map_angle, alpha, 3);
			}
		} else if (!slot.radio_request || slot.aboard_vehicle) {
			if (!(tracked_here && c.tracked_alpha == 0xFF)) {
				if (medic) {
					// The red cross over the rotated entity projection
					// [orig: @0x5a4ccd..0x5a4d48].
					float px = 0.0f, py = 0.0f;
					view_project(c.view, c.input, e.entity_x, e.entity_y, px, py);
					const float ix = static_cast<float>(static_cast<int>(px));
					const float iy = static_cast<float>(static_cast<int>(py));
					for (const MedicCrossQuad &q : medic_cross_quads(ix - 3.5f,
								 iy - 3.5f, ix + 4.5f, iy + 4.5f, alpha)) {
						c.clip_a.resize(4);
						c.clip_a[0] = {q.x0, q.y0, 0.0f, 0.0f};
						c.clip_a[1] = {q.x1, q.y0, 0.0f, 0.0f};
						c.clip_a[2] = {q.x1, q.y1, 0.0f, 0.0f};
						c.clip_a[3] = {q.x0, q.y1, 0.0f, 0.0f};
						clip_rect(c.clip_a, c.clip_b, c.view.px_x1, c.view.px_y1,
								c.view.px_x2, c.view.px_y2);
						if (c.disc_crop) {
							clip_circle32(c.clip_a, c.clip_b, c.view.center_x,
									c.view.center_y, c.view.disc_radius,
									c.view.disc_radius);
						}
						emit_fan(c.clip_a, q.color, c.out.overlays);
					}
				} else {
					draw_blip(c, e, color, 3, heading_angle, alpha, 3);
				}
			} else {
				draw_blip(c, e, c.tracked_color, 28, heading_angle, 255, 3);
			}
		} else {
			draw_blip(c, e, color, 23, heading_angle, alpha, 3);
		}
		// [orig: @0x5a4d7c..0x5a4db2]
		if (tracked_here && c.tracked_alpha != 0xFF)
			draw_blip(c, e, c.tracked_color, 28, heading_angle, c.tracked_alpha, 3);
		if (!names) continue;
		// The name: the slot name plus "<ch>" clan "<co>", or the entity name
		// without a slot; the regular label face, white, at the UNROTATED
		// projection [orig: @0x5a4dc3..0x5a4ea6].
		std::string text;
		if (slot.name_slot) {
			text = slot.name;
			if (!slot.clan.empty()) text += "<ch>" + slot.clan + "<co>";
		} else {
			text = slot.name;
		}
		if (text.size() > 255) text.resize(255);
		float px = 0.0f, py = 0.0f;
		view_project_unrotated(c.view, c.input, e.entity_x, e.entity_y, px, py);
		push_label(c, static_cast<float>(static_cast<int>(px)),
				static_cast<float>(static_cast<int>(py)), text,
				(static_cast<uint32_t>(alpha) << 24) + 0xFFFFFFu, 2, 0);
	}
	if (!names) return;
	// Loop 2: the transient bank's persons of the HUD team other than the HUD
	// entity, named by Entity_GetDisplayName when it returns one.
	// [orig: @0x5a4ef5..0x5a4f95; Entity_GetDisplayName @0x59BF70]
	for (const HudMinimapMarker &m : c.input.markers) {
		if (static_cast<HudMinimapBank>(m.bank) != HudMinimapBank::kTransient)
			continue;
		if (!m.entity_known || m.def_type != 3) continue;
		if (m.handle == ov->hud_handle || m.team != ov->hud_team) continue;
		const HudMinimapName *name = nullptr;
		for (const HudMinimapName &n : ov->names) {
			if (n.handle == m.handle) {
				name = &n;
				break;
			}
		}
		if (name == nullptr) continue;
		float px = 0.0f, py = 0.0f;
		view_project_unrotated(c.view, c.input, m.entity_x, m.entity_y, px, py);
		push_label(c, static_cast<float>(static_cast<int>(px)),
				static_cast<float>(static_cast<int>(py)), name->text,
				(static_cast<uint32_t>(alpha) << 24) + 0xFFFFFFu, 2, 0);
	}
}

} // namespace opennova::hud::minimap_detail
