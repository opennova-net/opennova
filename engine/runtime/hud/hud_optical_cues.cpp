#include <algorithm>
#include <base/io/bam.h>
#include <cmath>
#include <cstdio>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/inset_scope.h>
namespace opennova::hud {
namespace {
// Four radial stops: transparent fringe, two solid edges, transparent fringe.
// The original ring uses at most 95 segments and Q22 table samples.
// [orig: Render_DrawRingOverlay @0x5D4270]
void ring(HudDrawList &draw, float x, float y, float radius, float aspect, float thickness,
		uint32_t color) {
	const int n = std::clamp(int(radius * 0.3333333432674408f * 12.56637954711914f), 4, 95);
	const float inner = std::max(thickness * 0.5f - 1, 0.0f);
	const float radii[] = { std::max(radius - inner - 1, 0.0f), std::max(radius - inner, 0.0f),
		radius + inner, radius + inner + 1 };
	const uint32_t colors[] = { color & 0xFFFFFFu, color, color, color & 0xFFFFFFu };
	const uint32_t step = 16 * (0x10000000u / uint32_t(n)) + 1;
	auto vertex = [&](int i, int band) {
		const uint32_t index = (0x200000u + uint32_t(i) * step) >> 22;
		const double a = (index % 1024) * (2 * io::kPi / 1024);
		const float cs = float(int32_t(std::cos(a) * 4194304)) / 4194304;
		const float sn = float(int32_t(std::sin(a) * 4194304)) / 4194304;
		return HudTriVertex{ x + cs * radii[band] * aspect, y + sn * radii[band], 0, 0,
			colors[band] };
	};
	for (int i = 0; i < n; ++i)
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
} // namespace opennova::hud
