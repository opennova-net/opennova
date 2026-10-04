#include <algorithm>
#include <base/io/bam.h>
#include <cmath>
#include <cstdio>
#include <runtime/hud/hud_bay_logos.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/inset_scope.h>
namespace opennova::hud {
namespace {
// Render_DrawRingOverlay's record {x, y, 0.5, radius, stroke, fill, color,
// x-scale}: four radial stops -- R0 in the fill colour (the ring RGB at
// alpha 0 without a fill), R1 and R2 in the ring colour, R3 the ring RGB at
// alpha 0 -- with R0 = max(r - i - 1, 0), R1 = max(r - i, 0), R2 = r + i,
// R3 = r + i + 1 and i = max(stroke/2 - 1, 0); six triangles a segment plus
// a centre fan in the fill colour when the fill is nonzero. The segments are
// clamp(trunc(r * 1/3 * 4pi), 4, 95); the angle starts at 0x200000 BAM and
// steps 16 * (0x10000000 / n) + 1 through the Q22 table, x taking the SINE
// times the x-scale and y the cosine.
// [orig: Render_DrawRingOverlay @0x5D4270 -- the fill arm @0x5d4292..0x5d42a4,
//  radii @0x5d42c9..0x5d431d, flt_7CA274 / flt_7DC650 @0x5d4322, clamp
//  @0x5d4335..0x5d434a, step @0x5d4353, g_BamSinTableQ22 x @0x5d43d2 /
//  off_849934 y @0x5d43db, the x-scale @0x5d4513, the fan @0x5d43fb..0x5d441e,
//  the band indices @0x5d4426..0x5d4510]
void ring(HudDrawList &draw, float x, float y, float radius, float aspect, float thickness,
		uint32_t color, uint32_t fill = 0) {
	const int n = std::clamp(int(radius * 0.3333333432674408f * 12.56637954711914f), 4, 95);
	const float inner = std::max(thickness * 0.5f - 1, 0.0f);
	const float radii[] = { std::max(radius - inner - 1, 0.0f), std::max(radius - inner, 0.0f),
		radius + inner, radius + inner + 1 };
	const uint32_t inner_color = fill != 0 ? fill : (color & 0xFFFFFFu);
	const uint32_t colors[] = { inner_color, color, color, color & 0xFFFFFFu };
	const uint32_t step = 16 * (0x10000000u / uint32_t(n)) + 1;
	auto vertex = [&](int i, int band) {
		const int index = io::bam_table_index(0x200000u + uint32_t(i) * step);
		const float sn = float(io::bam_table_sin(index));
		const float cs = float(io::bam_table_cos(index));
		return HudTriVertex{ x + sn * aspect * radii[band], y + cs * radii[band], 0, 0,
			colors[band] };
	};
	const HudTriVertex center{ x, y, 0, 0, inner_color };
	for (int i = 0; i < n; ++i) {
		if (fill != 0) {
			HudTri fan;
			fan.a = center;
			fan.b = vertex(i, 0);
			fan.c = vertex(i + 1, 0);
			draw.tris.push_back(fan);
		}
		for (int b = 0; b < 3; ++b) {
			HudTri a, btri;
			a.a = vertex(i, b);
			a.b = vertex(i + 1, b);
			a.c = vertex(i + 1, b + 1);
			btri.a = vertex(i, b);
			btri.b = vertex(i + 1, b + 1);
			btri.c = vertex(i, b + 1);
			draw.tris.push_back(a);
			draw.tris.push_back(btri);
		}
	}
}
} // namespace
void HudFrameCompiler::element_service_prompt(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	// Every service line centres on design (512, 280) in the LARGE slot
	// (Impac22b), draw mode 2 = the centred half-bright drawer.
	// [orig: HUD_DrawGameplayOverlays -- slot 0xB4C3A0 pushed @0x5BDFD7 (armory /
	//  vehicle bay), @0x5BE0AA (FARP wait), @0x5BE0F7 (FARP reloading);
	//  HUD_DrawTextAtVirtualPos @0x5D3EC0 -> sub_5D2EA0 mode 2 @0x5D2ECE]
	if (c.service_prompt && (s.hud_detail_level < 3 || c.service_above_declutter))
		emit_slot_text(label_font_large_, label_large_scale_, c.service_text.c_str(),
				sx(512, w), sy(280, h), half_bright_argb(active_color(s)), kFontAlignCenter);
}
void HudFrameCompiler::element_inset_cues(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	// The Inset scene pass sits inside the frame's death-screen fork.
	// [orig: Render_ProcessMainSceneFrame @0x5CA26A]
	if (c.death_screen || w <= 0 || h <= 0)
		return;
	if (c.inset && !s.binoculars_view_active) {
		const auto g = inset_scope_geometry(w, h, s.aspect_mode, c.inset_fov_over_zoom);
		if (g.valid) {
			ring(draw_list_, g.center_x, g.center_y, g.radius_y, g.radius_x / g.radius_y, g.stroke,
					0xFF00FF00u);
			const uint32_t color = c.hit_feedback ? 0xFFFF5050u : 0xFF00FF00u;
			const float x = g.center_x, y = g.center_y, dx = float(g.cross_dx),
						dy = float(g.cross_dy);
			for (int sign : { -1, 1 }) {
				draw_list_.lines.push_back({ x + sign * dx, y, x + sign * 2 * dx, y, 1, color });
				draw_list_.lines.push_back({ x, y + sign * dy, x, y + sign * 2 * dy, 1, color });
			}
			if (c.inset_friendly) {
				for (int sx : { -1, 1 })
					for (int sy : { -1, 1 })
						draw_list_.lines.push_back({ x + sx * 2 * dx, y + sy * 2 * dy, x + sx * dx,
								y + sy * dy, 1, 0xFFFF0000u });
				// The friendly name centres on the aperture in the BOLD slot
				// (Arial bold), half-bright red.
				// [orig: Render_WeaponInsetScene -- slot 0xB4C394 @0x5CA0C0,
				//  HUD_DrawTextAligned_HalfBright mode 2 @0x5CA0CF]
				emit_slot_text(label_font_bold_, label_scale_, c.target_name.c_str(), x, y,
						0xFF7F0000u, kFontAlignCenter);
			}
		}
	}
}
void HudFrameCompiler::element_optical_cues(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	// The tail of the overlay walk carries no death-screen test: the weapon
	// flag admission alone decides both cues.
	// [orig: HUD_RenderAllOverlays @0x5A87EF..0x5A89DA]
	if (w <= 0 || h <= 0)
		return;
	// [orig: the LollyPop marker, HUD_RenderAllOverlays @0x5A87F9..0x5A8929]
	if (c.designator && c.impact_point.valid && !c.impact_point.clip) {
		const float x = c.impact_point.x, y = c.impact_point.y;
		const int radius = int(20 * c.designator_scale);
		draw_list_.lines.push_back({ x, y, x, y - radius, 1, c.designator_color });
		// Marker type 0 hands the ring a width scale of 2.0 beside its 2.0
		// stroke: the LollyPop head is an ellipse twice as wide as it is tall.
		// [orig: HUD_DrawEntityMarker @0x593140 -- ring record +0x1C @0x59327B,
		//  +0x10 @0x59327F; Render_DrawRingOverlay applies +0x1C to x @0x5D4513]
		ring(draw_list_, x, y - 2 * radius, float(radius), 2, 2, c.designator_color);
	}
	if (c.impact_distance) {
		// The authored STROVER_DIST template's sprintf with its one int, the
		// scope range's formatter (hud_game_text.h hud_sprintf).
		const std::string text = hud_sprintf(c.impact_format, c.impact_distance_m);
		// The line draws LEFT-aligned in the hudpos font, its design anchor pulled
		// back by half the text's width as the BOLD slot measures it: the
		// unscaled extent times that slot's scale, truncated, then halved.
		// [orig: GameFont_MeasureTextWidth(text, slot 0xB4C394) @0x5A897E..0x5A8984
		//  (@0x580A50), sar 1 @0x5A8995, Viewport_ScaleToVirtualCoords @0x5A89B0,
		//  HUD_DrawTextLeft_HalfBright(slot 0x2723C74) @0x5A89D0..0x5A89D5]
		const bool have_bold = label_font_bold_.font() != nullptr;
		int extent = 0;
		(have_bold ? label_font_bold_ : font_).measure(text.c_str(), 1.0f, 1.0f, &extent, nullptr);
		const int width = int(double(extent) * (have_bold ? label_scale_ : 1.0f));
		emit_text(text.c_str(), float(layout_.combat.impact_x - (width >> 1)),
				float(layout_.combat.impact_y), w, h, half_bright_argb(active_color(s)), 0);
	}
}
// HUD_DrawTexturedQuadCentered: the caller's diffuse on every corner, raw. The
// texture's material decides the colour (a colour-mode texture's 0x651 doubles it
// on the device, renderer::hud_color_material_argb; D-HUD-49), so nothing is folded
// here. The corners are the design centre +- the extent / 2 (C division) scaled
// onto the surface by the integer Viewport_ScaleToVirtualCoords; the UVs inset half
// a texel of the DESIGN extent and widen by one output pixel. Two textured
// triangles, so the quad keeps its place inside a kind-grouped run.
// [orig: HUD_DrawTexturedQuadCentered @0x5909E0 -- the corners
//  @0x590a14..0x590a33, Viewport_ScaleToVirtualCoords @0x590b04 / @0x590b18,
//  the UVs @0x590ba6..0x590be4]
void HudFrameCompiler::emit_textured_quad_centered(int32_t cx, int32_t cy, int32_t qw,
		int32_t qh, int32_t texture, uint32_t diffuse, float w, float h) {
	if (qw <= 0 || qh <= 0)
		return;
	const int32_t surface_w = int32_t(w), surface_h = int32_t(h);
	const float x0 = float(design_to_screen_x(cx - qw / 2, surface_w));
	const float x1 = float(design_to_screen_x(qw / 2 + cx, surface_w));
	const float y0 = float(design_to_screen_y(cy - qh / 2, surface_h));
	const float y1 = float(design_to_screen_y(cy + qh / 2, surface_h));
	// The far edges sum the UNROUNDED insets (the x87 keeps them on the stack).
	const double du = 0.5 / double(qw), dv = 0.5 / double(qh);
	const float u0 = float(du), v0 = float(dv);
	const float u1 = x1 > x0 ? float(1.0 - du + 1.0 / (double(x1) - double(x0))) : 1.0f;
	const float v1 = y1 > y0 ? float(1.0 - dv + 1.0 / (double(y1) - double(y0))) : 1.0f;
	HudTri first, second;
	first.texture = second.texture = texture;
	first.a = { x0, y0, u0, v0, diffuse };
	first.b = { x1, y0, u1, v0, diffuse };
	first.c = { x1, y1, u1, v1, diffuse };
	second.a = { x0, y0, u0, v0, diffuse };
	second.b = { x1, y1, u1, v1, diffuse };
	second.c = { x0, y1, u0, v1, diffuse };
	draw_list_.tris.push_back(first);
	draw_list_.tris.push_back(second);
}
// The walk's logos through HUD_DrawEntityMarker types 5/6/7 with the walk's
// arguments: scale 2.0, color alpha<<24 | palette rgb, the fill
// (alpha & ~3) << 22 in the texture_id slot, and three empty labels.
// [orig: HUD_DrawVehicleBayLogos @0x5a2d50..0x5a2e0d -> HUD_DrawEntityMarker
//  @0x593140]
void HudFrameCompiler::element_vehicle_bay_logos(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	if (w <= 0 || h <= 0 || c.bay_logos.empty())
		return;
	const auto &l = layout_.combat;
	const int32_t surface_w = int32_t(w), surface_h = int32_t(h);
	// Retail submits each logo in the walk's order between the zone panel and
	// the map: its own kind-grouping run keeps the logo over its backing and
	// under everything the walk draws later.
	mark_order_break();
	for (const HudBayLogo &logo : c.bay_logos) {
		// Behind the near plane, or clipped by any frustum plane, the marker
		// draws nothing [orig: `jz loc_593810` @0x593189 on the near test;
		//  HUD_ClipPointToFrustumAndProject @0x593194 nonzero -> return 0].
		if (!logo.point.valid || logo.point.clip != 0 || logo.point.depth_q16 <= 0)
			continue;
		const uint32_t color =
				(uint32_t(logo.alpha) << 24) + (hud_palette(logo.palette) & 0xFFFFFFu);
		const uint32_t fill = (uint32_t(logo.alpha) & 0xFFFFFFFCu) << 22;
		// The projected pixel is integral (the projection's `>> 16`).
		// [orig: HUD_ClipPointToFrustumAndProject @0x592566..0x592570]
		const int32_t x = int32_t(std::floor(logo.point.x));
		const int32_t y = int32_t(std::floor(logo.point.y));
		// With an empty label_above the stem is scale * 20.0 = 40 screen
		// pixels, straight up [orig: flt_7D8E60 @0x5931e4; the stem
		//  Render_ClipAndDrawLine2DToRect @0x59321d].
		const int32_t half = int32_t(2.0f * 20.0f);
		draw_list_.lines.push_back(
				{ float(x), float(y), float(x), float(y - half), 1, color });
		// The stem's top in design space, the depth-scaled quad, and the
		// backing's radius t = trunc(h * 0.66) lifting the centre by t.
		// [orig: Viewport_ScreenToVirtual @0x59357e; 0x10000000 / depth
		//  clamped 32..256 @0x593586..0x5935af; h = w/2 @0x5935b4..0x5935bb;
		//  dbl_7D8E58 = 0.66 @0x5935c5; y -= t @0x5935d4]
		const int32_t vx = screen_to_design_x(x, surface_w);
		int32_t vy = screen_to_design_y(y - half, surface_h);
		const int32_t qw = std::clamp(int32_t(0x10000000 / logo.point.depth_q16), 32, 256);
		const int32_t qh = qw / 2;
		const int32_t t = int32_t(double(qh) * 0.66);
		vy -= t;
		// The backing: the ring record {vx, vy, 0.5, t, 2.0, fill, color, 2.0}
		// scaled out of design space, x and the radius by W/1024, y and the
		// stroke by H/768.
		// [orig: the record @0x5935e0..0x59361f; Render_DrawRingOverlayVirtual
		//  (ex sub_5D50A0) @0x5d50a0 -- flt_7D68F4 = 1/1024, flt_7D68F8 =
		//  0x3AAAAAAB, the float 1/768]
		const double k1 = double(1.0f / 1024.0f), k2 = double(1.0f / 768.0f);
		const double sw = double(surface_w), sh = double(surface_h);
		ring(draw_list_, float(double(vx) * sw * k1), float(double(vy) * sh * k2),
				float(k1 * (sw * double(t))), 2.0f, float(k2 * (sh * 2.0)), color, fill);
		// The logo: a centred design-space quad over the backing, the
		// half-bright colour at full alpha under the texture's MODULATE2X.
		// A logo that failed to load draws no quad.
		// [orig: HUD_DrawTexturedQuadCentered @0x5909E0 -- `if (textureId)`
		//  @0x590adf; the colour `sar 1; and 7F7F7Fh; or FF000000h`
		//  @0x59362c..0x593638]
		const HudSprite &sprite = logo.type == kMarkerTypeLogoHelo ? l.logo_helo
				: logo.type == kMarkerTypeLogoHumm                 ? l.logo_humm
																	: l.logo_boat;
		if (!sprite.valid)
			continue;
		const int32_t texture = logo.type == kMarkerTypeLogoHelo ? kHudTexLogoHelo
				: logo.type == kMarkerTypeLogoHumm               ? kHudTexLogoHumm
																  : kHudTexLogoBoat;
		emit_textured_quad_centered(vx, vy, qw, qh, texture,
				((color >> 1) & 0x7F7F7Fu) | 0xFF000000u, w, h);
	}
	mark_order_break();
}
} // namespace opennova::hud
