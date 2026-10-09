// The spinmap's ring primitive and the pool-3 walk: Render_DrawRingOverlay's
// anti-aliased band (every map ring — the 253/254 pulse rings, the zone-radius
// rings, the KOTH and capture-zone rings) and the mask&0x810 walk over the
// pool-3 zone/location entities. See hud_minimap_view.h.
// [orig: Render_DrawRingOverlay @0x5D4270; Minimap_DrawRingBlip @0x597320;
//  HUD_DrawMapOverlay pool-3 walk @0x5a7504..0x5a76f5]

#include <runtime/hud/hud_minimap_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <formats/def/reserved_items.h>

namespace opennova::hud::minimap_detail {

namespace {

using GeomPolygon = std::vector<HudMapGeomVertex>;

uint32_t lerp_argb(uint32_t a, uint32_t b, float t) {
	uint32_t out = 0;
	for (int shift = 0; shift <= 24; shift += 8) {
		const float ca = static_cast<float>((a >> shift) & 0xFFu);
		const float cb = static_cast<float>((b >> shift) & 0xFFu);
		const float v = std::clamp(ca + (cb - ca) * t, 0.0f, 255.0f);
		out |= static_cast<uint32_t>(v + 0.5f) << shift;
	}
	return out;
}

// One half-plane of the per-pixel crop over a shaded polygon: position, UV
// and the gouraud diffuse interpolate along the cut edge.
void clip_geom_edge(const GeomPolygon &in, GeomPolygon &out, float nx, float ny,
		float d) {
	out.clear();
	if (in.empty()) return;
	HudMapGeomVertex prev = in.back();
	float prev_dist = nx * prev.x + ny * prev.y - d;
	for (const HudMapGeomVertex &cur : in) {
		const float cur_dist = nx * cur.x + ny * cur.y - d;
		const bool prev_inside = prev_dist <= 0.0001f;
		const bool cur_inside = cur_dist <= 0.0001f;
		if (prev_inside != cur_inside) {
			const float t = prev_dist / (prev_dist - cur_dist);
			out.push_back({prev.x + (cur.x - prev.x) * t,
					prev.y + (cur.y - prev.y) * t,
					prev.u + (cur.u - prev.u) * t,
					prev.v + (cur.v - prev.v) * t,
					lerp_argb(prev.color, cur.color, t)});
		}
		if (cur_inside) out.push_back(cur);
		prev = cur;
		prev_dist = cur_dist;
	}
}

void clip_geom(MapCompile &c, GeomPolygon &poly, bool disc) {
	const MapView &view = c.view;
	GeomPolygon &scratch = c.geom_b;
	clip_geom_edge(poly, scratch, -1.0f, 0.0f, -view.px_x1);
	clip_geom_edge(scratch, poly, 1.0f, 0.0f, view.px_x2);
	clip_geom_edge(poly, scratch, 0.0f, -1.0f, -view.px_y1);
	clip_geom_edge(scratch, poly, 0.0f, 1.0f, view.px_y2);
	if (!disc) return;
	// The 32-gon the bit0 fan lays [orig: @0x5a65f7..0x5a6641].
	for (int i = 0; i < 32 && !poly.empty(); ++i) {
		const float a0 = static_cast<float>(2.0 * io::kPi * i / 32.0);
		const float a1 = static_cast<float>(2.0 * io::kPi * (i + 1) / 32.0);
		const float x0 = view.center_x + std::cos(a0) * view.disc_radius;
		const float y0 = view.center_y + std::sin(a0) * view.disc_radius;
		const float x1 = view.center_x + std::cos(a1) * view.disc_radius;
		const float y1 = view.center_y + std::sin(a1) * view.disc_radius;
		const float nx = y1 - y0;
		const float ny = -(x1 - x0);
		clip_geom_edge(poly, scratch, nx, ny, nx * x0 + ny * y0);
		poly.swap(scratch);
	}
}

} // namespace

// The band: four concentric vertex rings per step — R-1 in the fill colour
// (the ring RGB at alpha 0 when there is no fill), R and R in the ring
// colour, R+1 in the ring RGB at alpha 0 — tied by six triangles per
// segment, plus a centre fan in the fill colour when the fill is nonzero.
// Segments = clamp(trunc(R * 1/3 * 4pi), 4, 95); the angle starts at
// 0x200000 BAM and steps 16 * (0x10000000 / n) + 1 through the Q22 table,
// x taking the sine and y the cosine.
// [orig: Render_DrawRingOverlay @0x5D4270 — radii @0x5d42a9..0x5d4328
//  (width 1.0 -> a = max(w/2 - 1, 0) = 0), flt_7CA274 / flt_7DC650
//  @0x5d4321, clamp @0x5d4335..0x5d434a, step @0x5d4353, vertex loop
//  @0x5d43c9..0x5d4661, indices @0x5d43fb..0x5d4510; the stock shader #2
//  pass | 0x400000 @0x5d4677]
void emit_ring_band(MapCompile &c, float cx, float cy, float radius,
		uint32_t ring_argb, uint32_t fill_argb, uint8_t layer) {
	const float a = 0.0f;
	const float r0 = std::max(radius - a - 1.0f, 0.0f);
	const float r1 = std::max(radius - a, 0.0f);
	const float r2 = radius + a;
	const float r3 = radius + a + 1.0f;
	int segments = static_cast<int>(radius * 0.3333333432674408f *
			12.56637954711914f);
	segments = std::clamp(segments, 4, 95);
	const uint32_t step = 16u * static_cast<uint32_t>(0x10000000 / segments) + 1u;
	const uint32_t inner_color = fill_argb != 0 ? fill_argb
			: (ring_argb & 0xFFFFFFu);
	const uint32_t outer_color = ring_argb & 0xFFFFFFu;

	// The rings, then the triangle list over them.
	GeomPolygon &verts = c.geom_a;
	verts.clear();
	verts.push_back({cx, cy, 0.0f, 0.0f, inner_color});
	uint32_t angle = 0x200000u;
	for (int k = 0; k <= segments; ++k) {
		const int idx = io::bam_table_index(angle);
		const float s = static_cast<float>(io::bam_table_sin(idx));
		const float co = static_cast<float>(io::bam_table_cos(idx));
		verts.push_back({cx + r0 * s, cy + r0 * co, 0.0f, 0.0f, inner_color});
		verts.push_back({cx + r1 * s, cy + r1 * co, 0.0f, 0.0f, ring_argb});
		verts.push_back({cx + r2 * s, cy + r2 * co, 0.0f, 0.0f, ring_argb});
		verts.push_back({cx + r3 * s, cy + r3 * co, 0.0f, 0.0f, outer_color});
		angle += step;
	}
	HudMapSprite sprite;
	sprite.center_x = cx;
	sprite.center_y = cy;
	sprite.texture = kHudMapTextureNone;
	sprite.layer = layer;
	sprite.color = ring_argb;
	sprite.geom_first = static_cast<uint32_t>(c.out.geom.size());
	// Whole-band bypass: a band whose outer ring clears every crop edge
	// needs no per-triangle clip.
	const MapView &view = c.view;
	bool clear = cx - r3 >= view.px_x1 && cx + r3 <= view.px_x2 &&
			cy - r3 >= view.px_y1 && cy + r3 <= view.px_y2;
	if (clear && c.disc_crop) {
		const float dx = cx - view.center_x;
		const float dy = cy - view.center_y;
		const float reach = view.disc_radius *
				static_cast<float>(std::cos(io::kPi / 32.0)) - r3;
		clear = reach >= 0.0f && dx * dx + dy * dy <= reach * reach;
	}
	GeomPolygon tri;
	const auto emit_tri = [&](int i0, int i1, int i2) {
		const HudMapGeomVertex &v0 = verts[static_cast<size_t>(i0)];
		const HudMapGeomVertex &v1 = verts[static_cast<size_t>(i1)];
		const HudMapGeomVertex &v2 = verts[static_cast<size_t>(i2)];
		if (clear) {
			c.out.geom.push_back(v0);
			c.out.geom.push_back(v1);
			c.out.geom.push_back(v2);
			return;
		}
		tri.assign({v0, v1, v2});
		clip_geom(c, tri, c.disc_crop);
		for (size_t i = 1; i + 1 < tri.size(); ++i) {
			c.out.geom.push_back(tri[0]);
			c.out.geom.push_back(tri[i]);
			c.out.geom.push_back(tri[i + 1]);
		}
	};
	for (int k = 0; k < segments; ++k) {
		const int vc = 1 + 4 * k;
		if (fill_argb != 0) emit_tri(0, vc, vc + 4);
		emit_tri(vc, vc + 4, vc + 5);
		emit_tri(vc, vc + 5, vc + 1);
		emit_tri(vc + 1, vc + 5, vc + 6);
		emit_tri(vc + 1, vc + 6, vc + 2);
		emit_tri(vc + 2, vc + 6, vc + 7);
		emit_tri(vc + 2, vc + 7, vc + 3);
	}
	sprite.geom_count = static_cast<uint32_t>(c.out.geom.size()) -
			sprite.geom_first;
	if (sprite.geom_count != 0) c.out.sprites.push_back(sprite);
}

// Minimap_DrawRingBlip: the descriptor {x, y, 0.5, max(min_px, projected z),
// width 1.0, fill, ring | 0xFF000000, 1.0}.
// [orig: Minimap_DrawRingBlip @0x597320 — radius = max(arg, projected z)
//  @0x59734F..0x597366 (@0x597357), color | 0xFF000000 @0x597387..0x597392]
void ring_blip(MapCompile &c, int32_t x_q16, int32_t y_q16, int32_t z_q16,
		uint32_t color, uint32_t fill, int min_px, uint8_t layer) {
	float px = 0.0f, py = 0.0f;
	view_project(c.view, c.input, x_q16, y_q16, px, py);
	const float z_px = view_length_px(c.view,
			static_cast<float>(z_q16) / io::kFp16One);
	const float radius = std::max(static_cast<float>(min_px), z_px);
	emit_ring_band(c, px, py, radius, color | 0xFF000000u, fill, layer);
}

// The pool-3 walk (mask & 0x810): the 6006 KOTH ring under bit4, the 2044
// location labels under bit11, and the 6027/6028 capture-zone rings whenever
// the walk runs.
// [orig: HUD_DrawMapOverlay @0x5a7504..0x5a76f5]
void draw_pool3_walk(MapCompile &c) {
	const HudMinimapOverlays *ov = c.input.overlays;
	if (ov == nullptr) return;
	for (const HudMinimapPoolEntity &e : ov->pool3) {
		if (e.def_id == def::DEF_TYPE_KOTH_CENTRE) {
			if ((c.flags & 0x10u) == 0) continue;
			// The KOTH zone ring over the score delta: ahead blue with a
			// 0x60 fill, behind red with a 0x60 fill, level the active HUD
			// colour, no fill; the radius entity+0, min 6 px.
			// [orig: Minimap_DrawKothZoneRing @0x5974E0 — dword_A85AFC - dword_A85B0C,
			//  0x4060FF / 0xFF2020 / g_HUDColors.active, alpha 96 / 255]
			const int32_t delta = ov->zone_score_delta;
			if (delta > 0) {
				ring_blip(c, e.x, e.y, e.radius_q16, 0xFF4060FFu, 0x604060FFu, 6, 4);
			} else if (delta < 0) {
				ring_blip(c, e.x, e.y, e.radius_q16, 0xFFFF2020u, 0x60FF2020u, 6, 4);
			} else {
				ring_blip(c, e.x, e.y, e.radius_q16,
						c.input.overlay_color | 0xFF000000u, 0, 6, 4);
			}
			continue;
		}
		if (e.def_id == def::DEF_TYPE_NAMED_LOCATION) {
			if ((c.flags & 0x800u) == 0) continue;
			// The location label: the g_LocationNames entry at the entity's
			// int16 +0x280 index, bold, centred on the UNROTATED projection,
			// (ctx+64 alpha << 24) + 0x9F9F9F — the half-bright drawer forces
			// that alpha opaque. [orig: @0x5a75a8..0x5a7600; the index is
			// stamped by Entity_SpawnFromBMSRecord @0x40F180..0x40F20C]
			if (e.location_index < 0 ||
					static_cast<size_t>(e.location_index) >=
							ov->location_names.size())
				continue;
			float px = 0.0f, py = 0.0f;
			view_project_unrotated(c.view, c.input, e.x, e.y, px, py);
			HudMapLabel label;
			label.x = static_cast<float>(static_cast<int>(px));
			label.y = static_cast<float>(static_cast<int>(py));
			std::snprintf(label.text, sizeof(label.text), "%s",
					ov->location_names[static_cast<size_t>(e.location_index)]
							.c_str());
			label.color = 0xFF9F9F9Fu;
			label.font = 0;
			label.clip = 1;
			c.out.labels.push_back(label);
			continue;
		}
		if (e.def_id == 6027 || e.def_id == 6028) {
			// The capture-zone rings: 6027 red with a 0x40 fill, 6028 blue,
			// the radius max(entity+0, 64 wu), min 6 px.
			// [orig: @0x5a761b..0x5a7664 / @0x5a7690..0x5a76d9 —
			//  Minimap_DrawRingBlip(pos, rgb | 0x50000000, rgb | 0x40000000, 6)]
			const int32_t radius = std::max(e.radius_q16, 0x400000);
			const uint32_t rgb = e.def_id == 6027 ? 0xFF2020u : 0x4060FFu;
			ring_blip(c, e.x, e.y, radius, rgb | 0x50000000u, rgb | 0x40000000u,
					6, 4);
		}
	}
}

} // namespace opennova::hud::minimap_detail
