#include <algorithm>
#include <base/io/fixed.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <runtime/hud/hud_frame.h>

namespace opennova::hud {
int hud_silhouette_alpha(int elapsed, int ramp, int base, int maximum) {
	if (ramp <= 0)
		return 0;
	elapsed = std::min(elapsed, ramp);
	if (elapsed == 0)
		elapsed = 1;
	const int phase = int((uint16_t((int64_t(elapsed) * 65536) / ramp - 1) >> 8) & 255);
	return std::min((base + maximum) / 2 - phase + 255, maximum);
}

void HudFrameCompiler::element_targeting(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	const auto &l = layout_.combat;
	if (c.dead || s.binoculars_view_active || w <= 0 || h <= 0)
		return;
	// [orig: palette[5], palette[3], auxiliaryColors[2] initialized @0x51F240]
	constexpr uint32_t warning = uint32_t(-44976), aim_color = uint32_t(-8347393);
	// Every cue here draws with clipToScreen: a quad that would cross a design
	// edge is slid fully back on-screen and recoloured auxiliaryColors[1].
	// [orig: draw_textured_quad_centered @0x5909E0 -- gate @0x590A37, left
	//  @0x590A45..0x590A5F, right @0x590A70..0x590A87, top @0x590A8B..0x590AA9,
	//  bottom @0x590AB4..0x590ACB]
	constexpr uint32_t edge_color = uint32_t(-32736);
	auto centered = [&](const HudProjectedPoint &p, const HudSprite &sprite, int tex,
							uint32_t color) {
		if (!p.valid || !sprite.valid)
			return;
		const int cx = int(p.x * 1024 / w), cy = int(p.y * 768 / h);
		int left = cx - sprite.width / 2, right = sprite.width / 2 + cx;
		int top = cy - sprite.height / 2, bottom = cy + sprite.height / 2;
		if (left < 0) {
			right += std::abs(left) + 1;
			left = 1;
			color = edge_color;
		}
		if (right > 1024) {
			left = 1023 - right + left;
			right = 1023;
			color = edge_color;
		}
		if (top < 0) {
			bottom += std::abs(top) + 1;
			top = 1;
			color = edge_color;
		}
		if (bottom > 768) {
			top = 767 - bottom + top;
			bottom = 767;
			color = edge_color;
		}
		emit_rect(sx(float(left), w), sy(float(top), h), sx(float(right), w),
				sy(float(bottom), h), color, true, tex);
	};
	if (c.vehicle_fixed)
		centered(c.vehicle_fixed_point, l.vehicle_fixed, kHudTexVehicleFixed, active_color(s));
	if (c.vehicle_lag) {
		const HudProjectedPoint center{ w / 2, h / 2, 0, true };
		centered(c.vehicle_lag_center ? center : c.vehicle_lag_point,
				l.vehicle_lag.valid ? l.vehicle_lag : l.vehicle_fixed,
				l.vehicle_lag.valid ? kHudTexVehicleLag : kHudTexVehicleFixed, active_color(s));
	}
	if (s.declutter_visible[kDeclutterXhairs]) {
		if (c.target_cursor) {
			const bool friendly_art = c.target_friendly && l.target_friendly.valid;
			const auto color = c.target_friendly || c.target_point.clip ? warning
					: c.target_locked									? active_color(s)
																		: uint32_t(-6250336);
			centered(c.target_point, friendly_art ? l.target_friendly : l.target,
					friendly_art ? kHudTexTargetFriendly : kHudTexTarget, color);
		}
		if (c.custom_aim && !(c.aim_point.clip & 16))
			centered(c.aim_point, l.custom_aim, kHudTexCustomAim,
					c.aim_point.clip ? warning : aim_color);
		// Four diagonal corner strokes around the MAIN aim anchor, not the
		// tracked target point. [orig: @0x592D32..0x592DD7]
		if (c.target_brackets) {
			const float x = s.aim_valid ? s.aim_screen_x * 1024 / w : 512;
			const float y = s.aim_valid ? s.aim_screen_y * 768 / h : 384;
			for (int dx : { -1, 1 })
				for (int dy : { -1, 1 }) {
					float x0 = x + dx * 13, y0 = y + dy * 13;
					float x1 = x + dx * 3, y1 = y + dy * 3;
					// Liang-Barsky clipping to the original design viewport.
					float lo = 0, hi = 1;
					const float vx = x1 - x0, vy = y1 - y0;
					const float p[] = { -vx, vx, -vy, vy };
					const float q[] = { x0, 1024 - x0, y0, 768 - y0 };
					bool visible = true;
					for (int i = 0; i < 4; ++i) {
						if (p[i] == 0) {
							if (q[i] < 0)
								visible = false;
						} else if (p[i] < 0)
							lo = std::max(lo, q[i] / p[i]);
						else
							hi = std::min(hi, q[i] / p[i]);
					}
					if (visible && lo <= hi)
						draw_list_.lines.push_back({ sx(x0 + lo * vx, w), sy(y0 + lo * vy, h),
								sx(x0 + hi * vx, w), sy(y0 + hi * vy, h), 1, warning });
				}
		}
	}
	// The commander's reticle is a scope detail, independent of XHAIRS.
	// Clamp to the 334-design-pixel circle and connect it to the center.
	// [orig: HUD_DrawScopeOverlayDetails @0x59E74D..0x59E87F]
	if (s.scope.active && c.commander && c.commander_point.valid) {
		auto p = c.commander_point;
		float x = p.x * 1024 / w - 512, y = p.y * 768 / h - 384;
		const int distance = int(std::sqrt(x * x + y * y));
		uint32_t color = active_color(s);
		if (distance > 334) {
			x = int(334 * x / distance);
			y = int(334 * y / distance);
			color = uint32_t(-32736);
		}
		p.x = (512 + x) * w / 1024;
		p.y = (384 + y) * h / 768;
		centered(p, l.commander, kHudTexCommander, color);
		draw_list_.lines.push_back(
				{ sx(512 + x, w), sy(384 + y, h), sx(512, w), sy(384, h), 1, color });
	}
}

void HudFrameCompiler::element_instruments(const HudFrameState &s, float w, float h) {
	const auto &c = s.combat;
	const auto &l = layout_.combat;
	if (c.dead)
		return;
	auto sprite = [&](const HudSprite &r, int x, int y, int tex, uint32_t color) {
		if (!r.valid)
			return;
		emit_rect(sx(float(x), w), sy(float(y), h), sx(float(x + r.width), w),
				sy(float(y + r.height), h), color, true, tex);
		++draw_list_.elements_drawn;
	};
	// Weapon and control-seat silhouettes share the original flash stamp.
	// [orig: HUD_RenderOverlays @0x5A7CBE..0x5A7D64; sub_59A710 @0x59A710]
	const int ramp = int(layout_.alpha_fade_seconds * 62.0f);
	const int base = int(layout_.alpha_fade_base * 2.55f);
	const int maximum = int(layout_.alpha_fade_max * 2.55f);
	auto tint = [&]() {
		return layout_.stance_tint |
				(uint32_t(hud_silhouette_alpha(s.ticks - silhouette_stamp_, ramp, base, maximum))
						<< 24);
	};
	if (ramp > 0 && c.vehicle_controls) {
		if (silhouette_vehicle_ != c.vehicle_identity) {
			silhouette_vehicle_ = c.vehicle_identity;
			silhouette_stamp_ = s.ticks;
		}
		sprite(l.vehicle, l.icon_x, l.icon_y, kHudTexVehicleStatus, tint());
		emit_text(c.gear_text[std::clamp(c.gear, 0, 2)].c_str(), float(l.gear_x),
				float(l.gear_y - 50), w, h, half_bright_argb(active_color(s)), 0);
	}
	if (ramp > 0 && s.weapon.active && hud_weapon_group_visible(s)) {
		if (silhouette_weapon_ != c.weapon_identity) {
			silhouette_weapon_ = c.weapon_identity;
			silhouette_stamp_ = s.ticks;
		}
		sprite(l.weapon, l.icon_x, l.icon_y, kHudTexWeaponSilhouette, tint());
	}
	// [orig: HUD_DrawParachuteAndArmorIcons @0x5925C0; carried object @0x599C20]
	if (c.parachute)
		sprite(l.parachute, l.parachute_x, l.parachute_y, kHudTexParachute, uint32_t(-6250336));
	if (c.armor)
		sprite(l.armor, l.armor_x, l.armor_y, kHudTexArmor, uint32_t(-6250336));
	if (c.carrying)
		sprite(l.cargo, l.cargo_x, l.cargo_y, kHudTexCargo, active_color(s));
	// The flight instrument uses ground-relative altitude. The two other
	// fields participate in its nonzero gate only. [orig: HUD_RenderOverlays @0x5A7D81..0x5A7DA5]
	if (!s.declutter_visible[kDeclutterAltGrp] ||
			!(c.altitude_agl_q16 || c.altitude_q16 || c.vertical_velocity_q16))
		return;
	const int altitude = c.altitude_agl_q16 / 21501;
	const int limited = std::min(altitude, 500);
	const int bar_height = int(sy(float(l.agl_height), h));
	const int bar_y = int(sy(float(l.agl_y), h));
	const int right = int(sx(float(l.agl_right), w));
	const int tick_width = int(sx(float(l.agl_tick_width), w));
	constexpr int kAglRangeSquared = 500 * 500;
	const auto curve = [&](int a) {
		return int((((int64_t(a) * a << 16) / kAglRangeSquared) * bar_height + io::kFp16OneInt / 2) >> 16);
	};
	const int marker = bar_y + bar_height - curve(500 - limited);
	// This rectangle is a colour-target CLEAR, not the readout's wire outline:
	// a device clear ignores the authored alpha (stock AGLCOLOR carries 0x41) and
	// writes the colour opaque.
	// [orig: sub_5D48E0 @0x5D48E0 -> CGfxTextOverlay_Draw(rect, 1) @0x5D493D ->
	//  the device vtbl+0xAC clear @0x67719B]
	emit_rect(sx(float(l.agl_left), w), float(marker - bar_height), float(right), float(marker),
			l.agl_color | 0xFF000000u, true);
	for (int a = 500; a >= 0; a -= 100) {
		const float y = float(marker + curve(a) - bar_height);
		draw_list_.lines.push_back({ float(right), y, float(right + 4), y, 1, l.agl_color });
	}
	const int stem = right + 2, text_x = stem + 2 * tick_width;
	draw_list_.lines.push_back(
			{ float(stem), float(bar_y), float(text_x), float(bar_y), 1, l.agl_color });
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &font = have_bold ? label_font_bold_ : font_;
	const float scale = have_bold ? label_scale_ : 1.0f;
	int text_w = 0, text_h = 0;
	font.measure("0000", scale, scale, &text_w, &text_h);
	const auto label = [&](const char *text, float x, float y, uint32_t align) {
		const auto run =
				font.layout(text, x, y, scale, scale, align, half_bright_argb(l.agl_color));
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
	};
	const int top = bar_y - text_h / 2;
	char text[32];
	std::snprintf(text, sizeof(text), "%3d", altitude);
	label(text, float(text_x + text_w - 5), float(top + 1), kFontAlignRight);
	emit_wire_rect(float(text_x - 1), float(top - 2), float(text_x + text_w),
			float(top + text_h + 1), l.agl_color);
	label(c.altitude_text.c_str(), float(text_x - 3), float(top + text_h + 3), 0);
	for (int y : { top - 1, top + text_h })
		draw_list_.lines.push_back({ float(stem + tick_width), float(bar_y), float(text_x - 1),
				float(y), 1, l.agl_color });
	++draw_list_.elements_drawn;
}
} // namespace opennova::hud
