#include <algorithm>
#include <base/io/bam.h>
#include <cmath>
#include <cstdio>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/inset_scope.h>
namespace opennova::hud {
namespace {
// Four radial stops: transparent fringe, two solid edges, transparent fringe.
// The original ring uses at most 95 segments and Q22 table samples.
// [orig: draw_ring_overlay @0x5D4270]
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
	if (c.service_prompt && (s.hud_detail_level < 3 || c.service_above_declutter))
		emit_text(c.service_text.c_str(), 512, 280, w, h, half_bright_argb(active_color(s)),
				kFontAlignCenter);
}
void HudFrameCompiler::element_inset_cues(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	if (c.dead || w <= 0 || h <= 0)
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
				emit_text(c.target_name.c_str(), x * 1024 / w, y * 768 / h, w, h, 0xFF7F0000u,
						kFontAlignCenter);
			}
		}
	}
}
void HudFrameCompiler::element_optical_cues(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	if (c.dead || w <= 0 || h <= 0)
		return;
	// [orig: HUD_RenderAllOverlays @0x5A87F9..0x5A89DA]
	if (c.designator && c.impact_point.valid && !c.impact_point.clip) {
		const float x = c.impact_point.x, y = c.impact_point.y;
		const int radius = int(20 * c.designator_scale);
		draw_list_.lines.push_back({ x, y, x, y - radius, 1, c.designator_color });
		ring(draw_list_, x, y - 2 * radius, float(radius), 1, 2, c.designator_color);
	}
	if (c.impact_distance) {
		// Formats are localized device inputs; the native safe formatter owns
		// the supported retail integer placeholder, including %u/%d and %%.
		std::string text;
		for (size_t i = 0; i < c.impact_format.size(); ++i) {
			const char ch = c.impact_format[i];
			if (ch == '%' && i + 1 < c.impact_format.size()) {
				const char next = c.impact_format[i + 1];
				if (next == 'd' || next == 'u' || next == 'i') {
					text += std::to_string(c.impact_distance_m);
					++i;
					continue;
				}
				if (next == '%') {
					text += '%';
					++i;
					continue;
				}
			}
			text += ch;
		}
		emit_text(text.c_str(), float(layout_.combat.impact_x), float(layout_.combat.impact_y), w,
				h, half_bright_argb(active_color(s)), kFontAlignCenter);
	}
}
} // namespace opennova::hud
