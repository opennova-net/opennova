// The spinmap's marker-bank walk and its blip drawer: the four-layer walk
// over the persistent, transient and special banks, the per-slot dispatch,
// and Minimap_DrawBlip over a marker's live-entity facts, cropped per pixel
// like retail's depth/viewport crop. See hud_minimap_view.h.
// [orig: MapOverlay_RenderAllLayers @0x5BE840 -> MapOverlay_RenderAllByLayer
//  @0x5BE590 -> Render_MinimapSlotBlip @0x5BE240 -> Minimap_DrawBlip
//  @0x597890]

#include <runtime/hud/hud_minimap_view.h>

#include <algorithm>
#include <cmath>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <runtime/hud/hud_medic_cross.h>

namespace opennova::hud::minimap_detail {

namespace {

constexpr double kBam16ToRadians = (2.0 * io::kPi) / io::kFp16OneD;
// Special-bank icon quads are a fixed 6px half-extent, resolution-independent
// [orig: Minimap_DrawBillboardDecal size_override = 6.0 @0x5be297; floor 6.0
// @0x597666]
constexpr float kSpecialIconHalfPx = 6.0f;
// The 32-gon's inradius factor: a point this close to the centre is inside
// every fan edge [orig: the 0x8000000-BAM ring step @0x5a6618].
const float kDiscInradius = static_cast<float>(std::cos(io::kPi / 32.0));

void push_fan_geom(MapCompile &c, const Polygon &poly, uint32_t color,
		HudMapSprite &sprite) {
	sprite.geom_first = static_cast<uint32_t>(c.out.geom.size());
	for (size_t i = 1; i + 1 < poly.size(); ++i) {
		for (const HudMapVertex *v : {&poly[0], &poly[i], &poly[i + 1]})
			c.out.geom.push_back({v->x, v->y, v->u, v->v, color});
	}
	sprite.geom_count = static_cast<uint32_t>(c.out.geom.size()) -
			sprite.geom_first;
}

// The retail colour table: team 1 palette[3], team 2 palette[5], else
// palette[1] [orig: Render_MinimapSlotBlip @0x5be4c5..0x5be4e1].
uint32_t slot_zone_palette(uint8_t team) {
	if (team == 1) return kHudPaletteLightBlue;
	return team == 2 ? kHudPaletteSalmon : kHudPaletteGreen;
}

// Render_MinimapSlotBlip: the special arms (the 253/254 pulse rings and the
// 6px billboard) and the regular arm (the zone ring + the blip).
void slot_blip(MapCompile &c, const HudMinimapMarker &marker, uint8_t layer) {
	const HudMinimapInput &input = c.input;
	if ((marker.flags & 0x40u) != 0) {
		if (marker.icon == 253 || marker.icon == 254) {
			// The pulse rings: the MAIN ring sized by the slot height with
			// the colour pulsed toward white, then the secondary rings AFTER
			// retail zeroes the slot z, so their radii are just the min args
			// at the marker CENTER — icon 253 one filled 1-px ring in the
			// pulsed colour, icon 254 three filled yellow rings (2/1/0 px;
			// the 49152/65536 folds multiply the already-zeroed z).
			// [orig: Render_MinimapSlotBlip 253 arm @0x5be3db..0x5be471
			//  (@0x5be46a main ring, z := 0 @0x5be47a, dot @0x5be482),
			//  254 arm @0x5be2a8..0x5be3b5 (main ring @0x5be337, z := 0
			//  @0x5be34f, rings @0x5be357 / @0x5be393 / @0x5be3ca)]
			const uint32_t pulse = pulse_color(marker.color, input.ticks);
			ring_blip(c, marker.x, marker.y, marker.z, pulse, 0, 4, layer);
			if (marker.icon == 253) {
				ring_blip(c, marker.x, marker.y, 0, pulse, pulse, 1, layer);
			} else {
				ring_blip(c, marker.x, marker.y, 0, 0xFFFFFF00u, 0xFFFFFF00u, 2,
						layer);
				ring_blip(c, marker.x, marker.y, 0, 0xFFFFFF00u, 0xFFFFFF00u, 1,
						layer);
				ring_blip(c, marker.x, marker.y, 0, 0xFFFFFF00u, 0xFFFFFF00u, 0,
						layer);
			}
			return;
		}
		// The special billboard: a fixed 6px half-extent at angle 0; drawMode
		// 0xFF is the alpha arg, so authored 0x7F table alphas still draw
		// opaque; culled against the rect, cropped per pixel.
		// [orig: Minimap_DrawBillboardDecal call @0x5be297, angle 0, drawMode
		//  0xFF pushed @0x5a6d20/@0x5a5a02; @0x597775..0x59778f]
		float mx = 0.0f, my = 0.0f;
		view_project(c.view, input, marker.x, marker.y, mx, my);
		if (mx + kSpecialIconHalfPx < c.view.px_x1 ||
				my + kSpecialIconHalfPx < c.view.px_y1 ||
				mx - kSpecialIconHalfPx > c.view.px_x2 ||
				my - kSpecialIconHalfPx > c.view.px_y2)
			return;
		HudMapSprite sprite;
		sprite.center_x = mx;
		sprite.center_y = my;
		sprite.half_w = kSpecialIconHalfPx;
		sprite.half_h = kSpecialIconHalfPx;
		sprite.color = marker.color | 0xFF000000u;
		sprite.layer = layer;
		marker_uv(input, marker.icon, sprite.u0, sprite.v0, sprite.u1,
				sprite.v1);
		emit_cropped_sprite(c, sprite, c.disc_crop);
		return;
	}
	// The regular arm reads the slot's pool entity; a zone-numbered entity
	// swaps the slot colour for the team palette — full brightness plus the
	// zone-radius ring when the slot carries an objective/defensive bit
	// (source & 0xC0), else quartered (each channel (c & 0xFC) >> 2).
	// [orig: Render_MinimapSlotBlip @0x5be490..0x5be551]
	uint32_t color = marker.color;
	if (marker.entity_known && marker.zone_number != 0) {
		color = slot_zone_palette(marker.team);
		if ((marker.source & 0xC0u) != 0) {
			ring_blip(c, marker.entity_x, marker.entity_y,
					static_cast<int32_t>(static_cast<uint32_t>(marker.zone_radius)
							<< 16),
					color, 0, 6, layer);
		} else {
			color = (((color >> 16) & 0xFCu) >> 2) << 16 |
					(((color >> 8) & 0xFCu) >> 2) << 8 |
					((color & 0xFCu) | 0x7C000000u) >> 2;
		}
	}
	if (marker.medic != 0) {
		// A local-team medic draws the red-cross plate IN PLACE of its blip:
		// the rect (px - 3.5, py - 3.5)..(px + 4.5, py + 4.5) in map pixels,
		// through the shared white-field + two-red-bars primitive at the
		// overlay pass's opaque alpha. This folds the bit5 loop-1 medic leg
		// (whose player-slot table has no producer here) onto the bank blip.
		// [orig: HUD_DrawEntityLabelsAndMarkers @0x5a49e0 — the
		//  CharAttr_ClassHasAttribute(playerClass, 8) test @0x5a4ab3, the rect
		//  @0x5a4cd6..0x5a4d24 (flt_7C44B8 = 4.0, flt_7C691C = 4.5,
		//  flt_7C3B94 = 0.5), HUD_DrawMedicCrossQuad @0x5a4d40]
		if (!marker.entity_known) return;
		float mx = 0.0f, my = 0.0f;
		view_project(c.view, input, marker.entity_x, marker.entity_y, mx, my);
		const float x1 = mx - 3.5f, y1 = my - 3.5f;
		const float x2 = mx + 4.5f, y2 = my + 4.5f;
		for (const MedicCrossQuad &q : medic_cross_quads(x1, y1, x2, y2, 0xFF)) {
			c.clip_a.resize(4);
			c.clip_a[0] = {q.x0, q.y0, 0.0f, 0.0f};
			c.clip_a[1] = {q.x1, q.y0, 0.0f, 0.0f};
			c.clip_a[2] = {q.x1, q.y1, 0.0f, 0.0f};
			c.clip_a[3] = {q.x0, q.y1, 0.0f, 0.0f};
			clip_rect(c.clip_a, c.clip_b, c.view.px_x1, c.view.px_y1,
					c.view.px_x2, c.view.px_y2);
			if (c.disc_crop) {
				clip_circle32(c.clip_a, c.clip_b, c.view.center_x,
						c.view.center_y, c.view.disc_radius, c.view.disc_radius);
			}
			emit_fan(c.clip_a, q.color, c.out.overlays);
		}
		return;
	}
	// Minimap_DrawBlip(entity, rect, alpha - entity heading, drawMode 0xFF,
	// colour, the slot icon) [orig: @0x5be553..0x5be57b].
	const int32_t angle = static_cast<int32_t>(
			c.view.base_angle_bam - static_cast<uint32_t>(marker.heading_bam));
	draw_blip(c, marker, color, marker.icon, angle, 0xFF, layer);
}

} // namespace

void emit_cropped_sprite(MapCompile &c, const HudMapSprite &in, bool disc) {
	const MapView &view = c.view;
	const float cs = std::cos(in.rotation_rad);
	const float sn = std::sin(in.rotation_rad);
	const auto corner = [&](float x, float y, float u, float v) {
		return HudMapVertex{in.center_x + x * cs - y * sn,
				in.center_y + x * sn + y * cs, u, v};
	};
	const HudMapVertex quad[4] = {
		corner(-in.half_w, -in.half_h, in.u0, in.v0),
		corner(in.half_w, -in.half_h, in.u1, in.v0),
		corner(in.half_w, in.half_h, in.u1, in.v1),
		corner(-in.half_w, in.half_h, in.u0, in.v1),
	};
	bool inside = true;
	for (const HudMapVertex &v : quad) {
		const bool in_rect = v.x >= view.px_x1 && v.x <= view.px_x2 &&
				v.y >= view.px_y1 && v.y <= view.px_y2;
		if (!in_rect) inside = false;
		if (disc) {
			const float dx = v.x - view.center_x;
			const float dy = v.y - view.center_y;
			const float r = view.disc_radius * kDiscInradius;
			if (dx * dx + dy * dy > r * r) inside = false;
		}
	}
	if (inside) {
		c.out.sprites.push_back(in);
		return;
	}
	c.clip_a.assign(quad, quad + 4);
	clip_rect(c.clip_a, c.clip_b, view.px_x1, view.px_y1, view.px_x2,
			view.px_y2);
	if (disc) {
		clip_circle32(c.clip_a, c.clip_b, view.center_x, view.center_y,
				view.disc_radius, view.disc_radius);
	}
	if (c.clip_a.size() < 3) return;
	HudMapSprite sprite = in;
	push_fan_geom(c, c.clip_a, in.color, sprite);
	c.out.sprites.push_back(sprite);
}

void draw_blip(MapCompile &c, const HudMinimapMarker &marker, uint32_t color,
		uint8_t cell, int32_t angle_bam, uint8_t alpha, uint8_t layer) {
	const HudMinimapInput &input = c.input;
	const MapView &view = c.view;
	// No def: nothing draws [orig: @0x5978a8].
	if (!marker.entity_known) return;
	// The blip centre is the placement matrix of entity+4 applied to the
	// collision-bbox centre entity+0x1FC, projected; the early cull pads the
	// rect by max(100 px, the projected entity+0 radius).
	// [orig: @0x5978B2..0x5978DC anchor; @0x5978EB..0x59797F cull]
	float px = 0.0f, py = 0.0f;
	view_project(view, input, marker.anchor_x, marker.anchor_y, px, py);
	const float margin = std::max(100.0f, view_length_px(view,
			static_cast<float>(marker.bound_radius_q16) / io::kFp16One));
	if (px + margin < view.px_x1 || py + margin < view.px_y1 ||
			px - margin > view.px_x2 || py - margin > view.px_y2)
		return;
	// The model halves (or the class table's), projected.
	float half_x_wu;
	float half_y_wu;
	if (marker.half_x_q16 > 0 || marker.half_y_q16 > 0) {
		half_x_wu = static_cast<float>(marker.half_x_q16) / io::kFp16One;
		half_y_wu = static_cast<float>(marker.half_y_q16) / io::kFp16One;
	} else {
		const bool person = marker.def_type == 3 || marker.icon == 3 ||
				marker.icon == 8;
		half_x_wu = half_y_wu = person ? 2.0f : 10.0f;
	}
	float hx = view_length_px(view, half_x_wu);
	float hy = view_length_px(view, half_y_wu);
	int32_t quad_angle = marker.rotate != 0 ? angle_bam : 0;
	uint8_t draw_cell = cell;
	if ((marker.entity_bits & kMarkerEntityFarp) != 0) {
		// The FARP class: hidden while its zone is not owned, the fixed
		// 0xFF808000 colour [orig: @0x597a1f..0x597a5d — (1 << e+538) &
		// dword_A85BBC, size_floor = -8355840].
		const uint32_t owned = input.overlays != nullptr
				? input.overlays->owned_zone_mask : 0u;
		if (marker.zone_number != 0 &&
				((1u << (marker.zone_number & 31u)) & owned) == 0)
			return;
		color = 0xFF808000u;
	}
	if (marker.footprint != 0) {
		// The Building leg: the extent-culled occlusion ground-slice fills
		// instead of an icon quad [orig: @0x597a84..0x597b2d ->
		// Render_CollisionWireframe @0x596800].
		const float ext = hx + hy;
		if (px + ext < view.px_x1 || py + ext < view.px_y1 ||
				px - ext > view.px_x2 || py - ext > view.px_y2)
			return;
		for (const HudMinimapFootprint *candidate : c.footprint_index) {
			if (candidate->handle == marker.handle) {
				emit_footprint(view, input, *candidate, c.clip_a, c.clip_b,
						c.out, c.disc_crop);
				break;
			}
		}
		return;
	}
	if (marker.def_type == 3 && (marker.entity_bits & kMarkerEntityDead) != 0) {
		// A dead person always draws the upright cell-8 body, whatever cell
		// the caller passed [orig: @0x597b58..0x597b72 — color = 8,
		// def_flags2 = 0].
		draw_cell = 8;
		quad_angle = 0;
	}
	const float floor_px = static_cast<float>(
			marker.floor_px != 0 ? marker.floor_px : 6);
	hx = std::max(floor_px, hx);
	hy = std::max(floor_px, hy);
	// The tight cull: the summed extents against the rect only — the disc
	// crop is per pixel [orig: @0x597DA1..0x597DF7].
	const float ext = hx + hy;
	if (px + ext < view.px_x1 || py + ext < view.px_y1 ||
			px - ext > view.px_x2 || py - ext > view.px_y2)
		return;
	// The HUD item flash gates: a lit phase (bit 0x10) hides the blip —
	// timer 12 the table-1..3 colour, 13 the table-4/5/9 colour, then 15
	// cell 5, 10 cell 3, 11 cell 0.
	// [orig: @0x597DFD..0x597E92 — dword_2723D28 / D2C / D34 / D20 / D24]
	bool hidden = false;
	if (color == 0xFF204080u)
		hidden = (input.item_flash[12] & 0x10) != 0;
	else if (color == 0xFF802020u)
		hidden = (input.item_flash[13] & 0x10) != 0;
	if (draw_cell == 5) {
		if ((input.item_flash[15] & 0x10) != 0) return;
	} else if (draw_cell == 3) {
		if ((input.item_flash[10] & 0x10) != 0) return;
	} else if (draw_cell == 0 && (input.item_flash[11] & 0x10) != 0) {
		return;
	}
	if (hidden) return;
	// The alpha arg replaces the colour's alpha when nonzero, else the colour
	// is forced opaque [orig: @0x597F3A..0x597F4C].
	const uint32_t argb = alpha != 0
			? (static_cast<uint32_t>(alpha) << 24) | (color & 0xFFFFFFu)
			: color | 0xFF000000u;
	HudMapSprite sprite;
	sprite.center_x = px;
	sprite.center_y = py;
	sprite.half_w = hx;
	sprite.half_h = hy;
	// HIWORD(angle) x 2pi/65536 [orig: @0x597E9D].
	sprite.rotation_rad = static_cast<float>(
			static_cast<double>(io::bam_sar(quad_angle, 16)) * kBam16ToRadians);
	// The raw diffuse: the strip's material 0x651 runs its MODULATE2X stage
	// on the device [orig: Minimap_DrawBlip @0x597f48..0x597f73 ->
	// Render_DrawIconStripCell_Debug @0x67bae0 via @0x597F6B].
	sprite.color = argb;
	sprite.layer = layer;
	marker_uv(input, draw_cell, sprite.u0, sprite.v0, sprite.u1, sprite.v1);
	emit_cropped_sprite(c, sprite, c.disc_crop);
}

// MapOverlay_RenderAllLayers: four layer passes, each walking the persistent
// bank twice (first the type-5 entities whose model carries the occlusion
// block — the footprints — then, after a flush, every model-bearing slot that
// is not such a footprint), the transient bank, and after a second flush the
// special bank's live slots: layer 0 draws the ones WITHOUT flag bit 7,
// layer 3 the ones WITH it, and layers 1 and 2 draw every live special slot
// — each special slot draws three times. A persistent slot whose entity has
// no model never draws.
// [orig: MapOverlay_RenderAllByLayer @0x5BE590 — pass 1 @0x5BE5C4..0x5BE64B,
//  flush sub_596780 @0x5BE64D, pass 2 @0x5BE652..0x5BE706 (model +0x30,
//  (def && type != 5) || !model+0xE0), transient @0x5BE70C..0x5BE779, flush
//  @0x5BE77B, special lifetime/bit7 @0x5BE780..0x5BE7D7]
void render_all_layers(MapCompile &c) {
	const std::vector<HudMinimapMarker> &markers = c.input.markers;
	const auto bank_of = [](const HudMinimapMarker &m) {
		return static_cast<HudMinimapBank>(m.bank);
	};
	for (uint8_t layer = 0; layer < 4; ++layer) {
		for (const HudMinimapMarker &m : markers) {
			if (bank_of(m) != HudMinimapBank::kPersistent ||
					hud_minimap_icon_layer(m.icon) != layer)
				continue;
			if (m.entity_known && m.def_type == 5 &&
					(m.entity_bits & kMarkerEntityHasModel) != 0 &&
					(m.entity_bits & kMarkerEntityOcclusion) != 0)
				slot_blip(c, m, layer);
		}
		for (const HudMinimapMarker &m : markers) {
			if (bank_of(m) != HudMinimapBank::kPersistent ||
					hud_minimap_icon_layer(m.icon) != layer)
				continue;
			if ((m.entity_bits & kMarkerEntityHasModel) == 0) continue;
			const bool def_not_building = m.entity_known && m.def_type != 5;
			if (def_not_building ||
					(m.entity_bits & kMarkerEntityOcclusion) == 0)
				slot_blip(c, m, layer);
		}
		for (const HudMinimapMarker &m : markers) {
			if (bank_of(m) == HudMinimapBank::kTransient &&
					hud_minimap_icon_layer(m.icon) == layer)
				slot_blip(c, m, layer);
		}
		for (const HudMinimapMarker &m : markers) {
			if (bank_of(m) != HudMinimapBank::kSpecial) continue;
			// Special slots keep their handle after expiry (the bank floors
			// the lifetime at zero) but the walk skips them.
			if (m.remaining_ticks == 0) continue;
			const bool bit7 = (m.flags & 0x80u) != 0;
			if ((layer == 0 && bit7) || (layer == 3 && !bit7)) continue;
			slot_blip(c, m, layer);
		}
	}
}

} // namespace opennova::hud::minimap_detail
