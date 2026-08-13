// The HUD frame compiler — the witnessed element walk over the hudpos layout,
// structural translation of the ported shell draws (game_hud.gd + hud_*.gd
// helpers, themselves cited ports) onto the typed draw list.
// [orig: HUD_RenderAllOverlays @ 0x5a8070 -> HUD_RenderOverlays @ 0x5a7bb0]

#include "hud/hud_frame.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace opennova::hud {

namespace {

constexpr float kDesignW = 1024.0f;
constexpr float kDesignH = 768.0f;
constexpr int kStanceFrames = 6;
constexpr int kMaxCarriedMessages = 40; // [orig: Chat_RebuildDisplayBuffers @ 0x498bd0]

uint32_t with_alpha(uint32_t argb, int alpha) {
	return (static_cast<uint32_t>(std::clamp(alpha, 0, 255)) << 24) |
			(argb & 0xFFFFFFu);
}

} // namespace

float HudFrameCompiler::sx(float design_x, float surface_w) const {
	return static_cast<float>(scale_axis(design_x, surface_w, kDesignW));
}

float HudFrameCompiler::sy(float design_y, float surface_h) const {
	return static_cast<float>(scale_axis(design_y, surface_h, kDesignH));
}

void HudFrameCompiler::configure(const HudLayout &layout,
		const fnt_font_t *font) {
	layout_ = layout;
	font_.set_font(font);
	reset_runtime_state();
}

void HudFrameCompiler::configure_label_fonts(const fnt_font_t *normal,
		const fnt_font_t *bold, float scale) {
	// [orig: HUD_InitAllFonts @ 0x51ee20 stores each pair through the
	// {font, scale_x, scale_y} slot writer @ 0x580453..0x580468]
	label_font_.set_font(normal);
	label_font_.set_page_base(
			static_cast<uint32_t>(kHudFontSlotLabel * FNT_MAX_PAGES));
	label_font_bold_.set_font(bold);
	label_font_bold_.set_page_base(
			static_cast<uint32_t>(kHudFontSlotLabelBold * FNT_MAX_PAGES));
	label_scale_ = scale > 0.0f ? scale : 1.0f;
}

void HudFrameCompiler::update_layout(const HudLayout &layout) {
	// Texture-table refresh only — the fade/flash/message state survives
	// [orig: HUD_LoadAllTextures @ 0x59e3d6 reloads art without a HUD reset].
	layout_ = layout;
}

void HudFrameCompiler::reset_runtime_state() {
	stance_ = StanceFade{};
	flash_prev_rounds_ = -1;
	flash_stamp_ = 0;
	messages_.clear();
	draw_list_ = HudDrawList{};
}

void HudFrameCompiler::push_message(const std::string &text, int now_ticks) {
	// [orig: HUD_DisplayTriggeredText @ 0x51f190 -> Chat_AddDebugMessage
	// @ 0x4987f0 — 930-tick life, >= 186-tick stagger vs the previous line]
	if (text.empty()) {
		return;
	}
	const bool has_prev = !messages_.empty();
	const int prev_expire = has_prev ? messages_.back().expire_tick : 0;
	HudMessageLine line;
	line.text = text.substr(0, static_cast<size_t>(kMessageTextMax));
	line.expire_tick = message_expire_tick(now_ticks, prev_expire, has_prev);
	messages_.push_back(line);
	while (messages_.size() > static_cast<size_t>(kMaxCarriedMessages)) {
		messages_.erase(messages_.begin());
	}
}

void HudFrameCompiler::emit_rect(float x0, float y0, float x1, float y1,
		uint32_t color, bool filled, int32_t texture, bool additive) {
	HudQuad quad;
	quad.x0 = x0;
	quad.y0 = y0;
	quad.x1 = x1;
	quad.y1 = y1;
	quad.color = color;
	quad.texture = texture;
	quad.filled = filled;
	quad.additive = additive;
	draw_list_.quads.push_back(quad);
}

void HudFrameCompiler::emit_wire_rect(float x0, float y0, float x1, float y1,
		uint32_t color) {
	emit_rect(x0, y0, x1, y1, color, false);
}

float HudFrameCompiler::measure_text_w(const char *text) const {
	int w = 0;
	int h = 0;
	font_.measure(text, 1.0f, 1.0f, &w, &h);
	return static_cast<float>(w);
}

float HudFrameCompiler::text_line_h() const {
	return font_.line_height(1.0f);
}

void HudFrameCompiler::emit_text(const char *text, float design_x,
		float design_y, float surface_w, float surface_h, uint32_t argb,
		uint32_t flags) {
	if (text == nullptr || text[0] == 0 || font_.font() == nullptr) {
		return;
	}
	const GameFontRun run = font_.layout(text, sx(design_x, surface_w),
			sy(design_y, surface_h), 1.0f, 1.0f, flags, argb);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
			run.quads.end());
	draw_list_.underlines.insert(draw_list_.underlines.end(),
			run.underlines.begin(), run.underlines.end());
}

const HudDrawList &HudFrameCompiler::compile(const HudFrameState &state,
		float surface_w, float surface_h) {
	draw_list_.quads.clear();
	draw_list_.tris.clear();
	draw_list_.lines.clear();
	draw_list_.glyphs.clear();
	draw_list_.underlines.clear();
	draw_list_.elements_drawn = 0;

	// The stance cross-fade restamp [orig: @ 0x599f8a].
	if (state.stance != stance_.cur) {
		stance_.prev = stance_.cur;
		stance_.cur = state.stance;
		stance_.stamp = state.ticks;
	}

	// The SIGHTS card draws first — the HUD overlays land on top of it
	// [orig: draw_weapon_sight_overlays @ 0x4dce00 runs at scene end;
	//  HUD_RenderAllOverlays later in the frame].
	element_sights_card(state, surface_w, surface_h);
	element_frame(state, surface_w, surface_h);
	element_health(state, surface_w, surface_h);
	element_stance(state, surface_w, surface_h);
	element_weapon_cluster(state, surface_w, surface_h);
	element_heat(state, surface_w, surface_h);
	element_power(state, surface_w, surface_h);
	element_waypoint(state, surface_w, surface_h);
	element_objectives(state, surface_w, surface_h);
	element_attach_labels(state, surface_w, surface_h);
	element_objective_line(state, surface_w, surface_h);
	// Friendly tags draw after the overlay cluster and before the console
	// messages, exactly the retail pass order [orig: HUD_DrawFriendlyTagsPass
	// @ 0x5a87cc, then HUD_DrawConsoleMessages @ 0x5a87d1].
	element_friendly_tags(state, surface_w, surface_h);
	element_messages(state, surface_w, surface_h);
	return draw_list_;
}

void HudFrameCompiler::element_sights_card(const HudFrameState &state,
		float w, float h) {
	if (!state.weapon.sights_card_up || state.binoculars_view_active) {
		return;
	}
	for (size_t row = 0; row < layout_.sights.size(); ++row) {
		const HudSightsRow &r = layout_.sights[row];
		if (!r.texture_valid) {
			continue;
		}
		// Row rects live in the 1024x768 design space and scale per draw
		// [orig: Viewport_ScaleToVirtualCoords @ 0x5d2b20].
		emit_rect(r.x0 * w / kDesignW, r.y0 * h / kDesignH,
				r.x1 * w / kDesignW, r.y1 * h / kDesignH, 0xFFFFFFFFu, true,
				kHudTexSightsBase + static_cast<int32_t>(row), r.additive);
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_frame(const HudFrameState &state, float w,
		float h) {
	(void)state;
	if (!layout_.frame_texture_valid) {
		return;
	}
	const float x = static_cast<float>(layout_.frame_pos.x);
	const float y = static_cast<float>(layout_.frame_pos.y);
	emit_rect(sx(x, w), sy(y, h),
			sx(x + static_cast<float>(layout_.frame_tex_w), w),
			sy(y + static_cast<float>(layout_.frame_tex_h), h), 0xFFFFFFFFu,
			true, kHudTexFrame);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_health(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawHealthBar @ 0x5a2e50 — fill (x1+1, y1+1)..(x1+fill, y2),
	// wireframe border on top, threshold colors]
	const HudRectRecord &r = layout_.health_rect;
	if (!r.present || (r.x == 0.0f && r.y == 0.0f && r.w == 0.0f && r.h == 0.0f)) {
		return;
	}
	const float x0 = sx(r.x, w);
	const float y0 = sy(r.y, h);
	const float x1 = sx(r.x + r.w, w);
	const float y1 = sy(r.y + r.h, h);
	const float fraction = std::clamp(state.health_fraction, 0.0f, 1.0f);
	const int32_t ratio_fp16 =
			static_cast<int32_t>(fraction * 65536.0f);
	const int band = health_color_band_fp16(ratio_fp16);
	const uint32_t fill = band == 0 ? layout_.tag_good
			: band == 1              ? layout_.tag_middle
									 : layout_.tag_bad;
	const float fill_w = (x1 - x0) * fraction;
	if (fill_w > 1.0f) {
		emit_rect(x0 + 1.0f, y0 + 1.0f, x0 + fill_w,
				std::max(y1, y0 + 1.0f), fill, true);
	}
	emit_wire_rect(x0, y0, x1, y1, layout_.health_border);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_stance(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawStanceIndicator @ 0x599f10 — ramp+texture gates, the
	// shared frame-0 scale, the cross-fade pair]
	if (layout_.stance_pos.x == 0 && layout_.stance_pos.y == 0) {
		return;
	}
	const int ramp = static_cast<int>(layout_.alpha_fade_seconds *
			static_cast<float>(kSecondsToTicks));
	if (ramp <= 0) {
		return;
	}
	for (int i = 0; i < kStanceFrames; ++i) {
		if (!layout_.stance_texture_valid[static_cast<size_t>(i)]) {
			return;
		}
	}
	const int32_t q16 =
			stance_scale_q16(layout_.stance_frame0_w, layout_.stance_frame0_h);
	if (q16 <= 0) {
		return;
	}
	const int base_alpha = static_cast<int>(layout_.alpha_fade_base *
			static_cast<float>(kPercentToAlpha));
	const int elapsed = state.ticks - stance_.stamp;
	const int cur_a = stance_current_alpha(elapsed, ramp, base_alpha);
	const int prev_a = stance_prev_alpha(elapsed, ramp);

	const int scaled_w = stance_scaled_dim(layout_.stance_frame0_w, q16);
	const int scaled_h = stance_scaled_dim(layout_.stance_frame0_h, q16);
	const int center_x = stance_center_axis(scaled_w);
	const int center_y = stance_center_axis(scaled_h);

	auto emit_frame = [&](int idx, int alpha) {
		const float dx = static_cast<float>(layout_.stance_pos.x +
				layout_.stance_offset_x[static_cast<size_t>(idx)] + center_x);
		const float dy = static_cast<float>(layout_.stance_pos.y +
				layout_.stance_offset_y[static_cast<size_t>(idx)] + center_y);
		emit_rect(sx(dx, w), sy(dy, h),
				sx(dx + static_cast<float>(scaled_w), w),
				sy(dy + static_cast<float>(scaled_h), h),
				with_alpha(layout_.stance_tint, alpha), true,
				kHudTexStance0 + idx);
	};
	if (stance_.cur >= 0 && stance_.cur < kStanceFrames) {
		emit_frame(stance_.cur, cur_a);
	}
	if (prev_a > 0 && stance_.prev != stance_.cur && stance_.prev >= 0 &&
			stance_.prev < kStanceFrames) {
		emit_frame(stance_.prev, prev_a);
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_weapon_cluster(const HudFrameState &state,
		float w, float h) {
	// Nothing draws without an installed weapon [orig: @ 0x5939f3 / @ 0x599a67].
	if (!state.weapon.active) {
		return;
	}
	const uint32_t wc = half_bright_argb(layout_.weapon_text);
	char ammo[64];
	const std::string ammo_text = format_ammo(state.weapon.clip,
			state.weapon.reserve, state.weapon.capacity);
	(void)ammo;
	if (!ammo_text.empty() && layout_.ammo_count.hidden == 0) {
		const uint32_t align_flags = layout_.ammo_count.align == 1
				? kFontAlignRight
				: (layout_.ammo_count.align == 2 ? kFontAlignCenter : 0u);
		emit_text(ammo_text.c_str(),
				static_cast<float>(layout_.ammo_count.x),
				static_cast<float>(layout_.ammo_count.y), w, h, wc,
				align_flags);
	}
	if (!state.weapon.display_name.empty() && layout_.weapon_name.hidden == 0) {
		// [orig: @ 0x593b36..0x593bf5 — the 640-wide x nudge]
		const int nudge =
				weapon_name_x_nudge(w <= 640.0f, layout_.weapon_name.align);
		const uint32_t align_flags = layout_.weapon_name.align == 1
				? kFontAlignRight
				: (layout_.weapon_name.align == 2 ? kFontAlignCenter : 0u);
		emit_text(state.weapon.display_name.c_str(),
				static_cast<float>(layout_.weapon_name.x + nudge),
				static_cast<float>(layout_.weapon_name.y), w, h, wc,
				align_flags);
	}
	element_clip_indicator(state, w, h);
	element_crosshair(state, w, h);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_clip_indicator(const HudFrameState &state,
		float w, float h) {
	// [orig: draw_hud_ammo_indicator @ 0x599a30 — anchor/ramp/-1 gates, the
	// flash restamp, HUDCLIPGFX background at base alpha, one HUDRNDGFX icon
	// per round stepped along the authored vector at flash alpha]
	const HudWeaponState &wep = state.weapon;
	if (layout_.clip_pos.x == 0 && layout_.clip_pos.y == 0) {
		return;
	}
	const int ramp = static_cast<int>(layout_.alpha_fade_seconds *
			static_cast<float>(kSecondsToTicks));
	if (ramp <= 0 || wep.reserve == -1 || wep.clip == -1) {
		return;
	}
	// The restamp key at the reimpl's single-pool altitude (D-HUD-5): the
	// (round_type, reserve) pair; the compiler keys on the folded count.
	const int folded = folded_reserve(wep.clip, wep.reserve, wep.capacity);
	if (folded != flash_prev_rounds_) {
		flash_prev_rounds_ = folded;
		flash_stamp_ = state.ticks;
	}
	const int base_alpha = static_cast<int>(layout_.alpha_fade_base *
			static_cast<float>(kPercentToAlpha));
	const int max_alpha = static_cast<int>(layout_.alpha_fade_max *
			static_cast<float>(kPercentToAlpha));
	const int flash = fade_flash_alpha(state.ticks - flash_stamp_, ramp,
			base_alpha, max_alpha);

	const float ax = static_cast<float>(layout_.clip_pos.x);
	const float ay = static_cast<float>(layout_.clip_pos.y);
	if (wep.clip_texture_valid) {
		const float bx = ax + static_cast<float>(wep.clipgfx_offset_x);
		const float by = ay + static_cast<float>(wep.clipgfx_offset_y);
		emit_rect(sx(bx, w), sy(by, h),
				sx(bx + static_cast<float>(wep.clip_tex_w), w),
				sy(by + static_cast<float>(wep.clip_tex_h), h),
				with_alpha(layout_.stance_tint, base_alpha), true,
				kHudTexClipGfx);
	}
	if (wep.round_texture_valid) {
		const int count = round_icon_count(wep.clip, wep.reserve, wep.capacity,
				wep.rounds_per_icon);
		float px = ax + static_cast<float>(wep.rndgfx_offset_x);
		float py = ay + static_cast<float>(wep.rndgfx_offset_y);
		for (int i = 0; i < count; ++i) {
			emit_rect(sx(px, w), sy(py, h),
					sx(px + static_cast<float>(wep.round_tex_w), w),
					sy(py + static_cast<float>(wep.round_tex_h), h),
					with_alpha(layout_.stance_tint, flash), true,
					kHudTexRoundGfx);
			px += static_cast<float>(wep.rndgfx_step_x);
			py += static_cast<float>(wep.rndgfx_step_y);
		}
	}
}

void HudFrameCompiler::element_crosshair(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawCrosshair @ 0x592640 — the !CanFire gate, the spread
	// projection, the five tapered regions via @ 0x590f50]
	if (state.binoculars_view_active) {
		return;
	}
	if (!crosshair_should_draw(state.aimed_shot_available,
				state.keep_crosshair_while_aimed)) {
		return;
	}
	if (!layout_.crosshair_texture_valid) {
		return;
	}
	// The projected aim point in design units; 1P pins the exact center
	// [orig: @ 0x5928a0 / the projected branch @ 0x592910].
	float cx = kDesignW * 0.5f;
	float cy = kDesignH * 0.5f;
	if (state.aim_valid && w > 0.0f && h > 0.0f) {
		cx = state.aim_screen_x * kDesignW / w;
		cy = state.aim_screen_y * kDesignH / h;
	}
	const float spread = static_cast<float>(crosshair_spread_px_fp16(
			state.hud_spread_fp16, state.fov_deg, w));

	const float half_w = static_cast<float>(layout_.crosshair_tex_w) * 0.5f;
	const float half_h = static_cast<float>(layout_.crosshair_tex_h) * 0.5f;
	const float offsets[5][2] = {
		{0.0f, -spread}, {0.0f, spread}, {-spread, 0.0f}, {spread, 0.0f},
		{0.0f, 0.0f},
	};
	for (int corner = 0; corner < 5; ++corner) {
		const float ox = cx + offsets[corner][0];
		const float oy = cy + offsets[corner][1];
		const float l = ox - half_w;
		const float t = oy - half_h;
		const float r = ox + half_w;
		const float b = oy + half_h;
		const float mx = (l + r) * 0.5f;
		const float my = (t + b) * 0.5f;
		const float tx = static_cast<float>(kCrosshairTaper) * half_w;
		const float ty = static_cast<float>(kCrosshairTaper) * half_h;
		float strip[5][2];
		int verts = 5;
		switch (corner) {
			case 0:
				strip[0][0] = l; strip[0][1] = t;
				strip[1][0] = mx - tx; strip[1][1] = my - ty;
				strip[2][0] = mx; strip[2][1] = t;
				strip[3][0] = mx + tx; strip[3][1] = my - ty;
				strip[4][0] = r; strip[4][1] = t;
				break;
			case 1:
				strip[0][0] = l; strip[0][1] = b;
				strip[1][0] = mx - tx; strip[1][1] = my + ty;
				strip[2][0] = mx; strip[2][1] = b;
				strip[3][0] = mx + tx; strip[3][1] = my + ty;
				strip[4][0] = r; strip[4][1] = b;
				break;
			case 2:
				strip[0][0] = l; strip[0][1] = t;
				strip[1][0] = mx - tx; strip[1][1] = my - ty;
				strip[2][0] = l; strip[2][1] = my;
				strip[3][0] = mx - tx; strip[3][1] = my + ty;
				strip[4][0] = l; strip[4][1] = b;
				break;
			case 3:
				strip[0][0] = r; strip[0][1] = t;
				strip[1][0] = mx + tx; strip[1][1] = my - ty;
				strip[2][0] = r; strip[2][1] = my;
				strip[3][0] = mx + tx; strip[3][1] = my + ty;
				strip[4][0] = r; strip[4][1] = b;
				break;
			default:
				strip[0][0] = mx + tx; strip[0][1] = my - ty;
				strip[1][0] = mx - tx; strip[1][1] = my - ty;
				strip[2][0] = mx + tx; strip[2][1] = my + ty;
				strip[3][0] = mx - tx; strip[3][1] = my + ty;
				verts = 4;
				break;
		}
		const float qw = r - l;
		const float qh = b - t;
		if (qw <= 0.0f || qh <= 0.0f) {
			continue;
		}
		for (int i = 0; i + 2 < verts; ++i) {
			HudTri tri;
			HudTriVertex *out[3] = {&tri.a, &tri.b, &tri.c};
			const int idx[3] = {i, i + 1, i + 2};
			for (int k = 0; k < 3; ++k) {
				out[k]->x = sx(strip[idx[k]][0], w);
				out[k]->y = sy(strip[idx[k]][1], h);
				out[k]->u = (strip[idx[k]][0] - l) / qw;
				out[k]->v = (strip[idx[k]][1] - t) / qh;
			}
			tri.color = 0xFFFFFFFFu;
			tri.texture = kHudTexCrosshair;
			draw_list_.tris.push_back(tri);
		}
	}
}

void HudFrameCompiler::element_heat(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawWeaponHeatBar @ 0x599700 — gate, border, proportional
	// fill, axis by rect shape]
	if (state.weapon.heat <= 0) {
		return;
	}
	const HudRectRecord &r = layout_.heat_rect;
	if (!r.present || (r.w <= 0.0f && r.h <= 0.0f)) {
		return;
	}
	const float x0 = sx(r.x, w);
	const float y0 = sy(r.y, h);
	const float x1 = sx(r.x + r.w, w);
	const float y1 = sy(r.y + r.h, h);
	emit_wire_rect(x0, y0, x1, y1, layout_.heat_border);
	if (heat_bar_is_horizontal(x1 - x0, y1 - y0)) {
		const int span =
				heat_fill_span(static_cast<int>(x1 - x0), state.weapon.heat);
		emit_rect(x0 + 1.0f, y0 + 1.0f,
				x0 + 1.0f + std::max(static_cast<float>(span) - 2.0f, 0.0f),
				y1 - 1.0f, layout_.stance_bad, true);
	} else {
		const int span =
				heat_fill_span(static_cast<int>(y1 - y0), state.weapon.heat);
		const float top = y1 - static_cast<float>(span) + 1.0f;
		emit_rect(x0 + 1.0f, top, x1 - 1.0f,
				std::max(y1 - 1.0f, top), layout_.stance_bad, true);
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_power(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawPowerThrowChargeBar @ 0x599830 — outline + inset fill +
	// "%d%" 15 output pixels above, all in the flat 0xFF800000 half-red]
	if (!state.windup_active) {
		return;
	}
	const HudRectRecord &r = layout_.power_rect;
	if (!r.present || r.w <= 0.0f || r.h <= 0.0f) {
		return;
	}
	const uint32_t color = 0xFF800000u; // [orig: the constant @ 0x840b1c]
	const int32_t progress = power_throw_progress_fp16(state.windup_held_ticks);
	const float x0 = sx(r.x, w);
	const float y0 = sy(r.y, h);
	const float x1 = sx(r.x + r.w, w);
	const float y1 = sy(r.y + r.h, h);
	emit_wire_rect(x0, y0, x1, y1, color);
	const int span = power_fill_span(progress, static_cast<int>(x1 - x0));
	if (span > 1) {
		emit_rect(x0 + 1.0f, y0 + 1.0f,
				x0 + 1.0f + std::min(static_cast<float>(span - 1),
									 x1 - x0 - 2.0f),
				y1 - 1.0f, color, true);
	}
	char label[16];
	std::snprintf(label, sizeof(label), "%d%%",
			static_cast<int>((static_cast<int64_t>(progress) * 100) >> 16));
	// 15 OUTPUT pixels above the bar: convert back to design so the offset
	// commutes with the rounding [orig: y-15 @ 0x5999ef].
	const float dy = static_cast<float>(
			pixel_delta_to_design(-15.0, h, kDesignH));
	emit_text(label, r.x, r.y + dy, w, h, half_bright_argb(color), 0u);
	++draw_list_.elements_drawn;
}

uint32_t HudFrameCompiler::active_color(const HudFrameState &state) const {
	// The hud_color_index scheme table + the derived master overlay color.
	// [orig: HUD_InitTeamColorTable @0x51f240 — the 16-dword g_hudColorTable
	// @0x24C1838 immediates (entries 0..5 are the cycled schemes); per frame
	// HUD_RenderAllOverlays @0x5a8100-0x5a8125 refreshes table[2] from the
	// hudpos hud_textcolor (g_hudposTextColor) and restamps the frame overlay
	// color (g_hudFrameOverlayColor @0x840B1C) = table[index]; the snapshot
	// twin g_hudActiveColor @0x24C1868 = table[index] | 0xFF000000 at init and
	// table[index] at the cycle @0x49afe0. Both twins carry the same value for
	// every authored scheme (all entries ship alpha FF); the compiler derives
	// ONE per-frame color and forces the init path's FF alpha.]
	static constexpr uint32_t kSchemeTable[6] = {
			0xFFFFFFFFu, // 0 white
			0xFF00FF00u, // 1 green
			0xFF010101u, // 2 init placeholder — sourced live from hud_textcolor
			0xFF80A0FFu, // 3 light blue
			0xFFF0F000u, // 4 yellow
			0xFFFF5050u, // 5 salmon
	};
	const int idx = state.hud_color_index >= 0 && state.hud_color_index <= 5
			? state.hud_color_index
			: 2;
	const uint32_t rgb = idx == 2 ? layout_.hud_text : kSchemeTable[idx];
	return rgb | 0xFF000000u;
}

void HudFrameCompiler::element_waypoint(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawWaypointNameAndDistance @ 0x5947a0 — align routing, the
	// wireframe distance box (field 3 hides only the box)]
	if (!state.waypoint.present || font_.font() == nullptr) {
		return;
	}
	const HudPosRecord &gp = layout_.wpd_info;
	if (!gp.present || (gp.x == 0 && gp.y == 0 && gp.hidden == 0 && gp.align == 0)) {
		return;
	}
	char dist[16];
	std::snprintf(dist, sizeof(dist), "%d", state.waypoint.distance_m);
	// The frame overlay color [orig: the g_hudFrameOverlayColor reads
	// @0x5949dd..0x594c9d in the waypoint pair].
	const uint32_t color = active_color(state);
	const float ax = static_cast<float>(gp.x);
	const float ay = static_cast<float>(gp.y);
	// Measures in font pixels, folded to design via the surface ratio — the
	// same conversion the ported shell used.
	const float dist_w = measure_text_w(dist) * kDesignW / std::max(w, 1.0f);
	const float text_h = text_line_h() * kDesignH / std::max(h, 1.0f);
	float dist_x = ax;
	float box_left = ax;
	float box_right = ax + dist_w + 4.0f;
	if (!state.waypoint.name.empty()) {
		switch (gp.align) {
			case 1: {
				emit_text(state.waypoint.name.c_str(), ax, ay, w, h, color,
						kFontAlignRight);
				const float name_w = measure_text_w(
						state.waypoint.name.c_str()) * kDesignW /
						std::max(w, 1.0f);
				dist_x = ax - 4.0f - name_w;
				box_left = dist_x - dist_w;
				box_right = dist_x + 4.0f;
				break;
			}
			case 2:
				emit_text(state.waypoint.name.c_str(), ax, ay, w, h, color, 0u);
				dist_x = ax - 4.0f;
				box_left = dist_x - dist_w;
				box_right = dist_x + 4.0f;
				break;
			default:
				emit_text(state.waypoint.name.c_str(), ax + dist_w + 4.0f, ay,
						w, h, color, 0u);
				break;
		}
	}
	if (gp.hidden == 0) {
		emit_wire_rect(sx(box_left, w), sy(ay - 2.0f, h), sx(box_right, w),
				sy(ay + text_h - 1.0f, h), color);
	}
	emit_text(dist, dist_x, ay, w, h, color, kFontAlignRight);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_objectives(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawWinConditions @ 0x5ba940 — full geometry witnessed
	// 2026-08-12: measure pass, HUD_DrawLabelBox rect, 16x16 checkbox outline
	// (4 lines, 0xFFE0E0E0), the done-mark red X (6 lines, 0xFFFF0000 — both
	// diagonals tripled for thickness), and the alpha/gray color folds. The
	// panel alpha byte dword_24C18CC rides every draw color's top byte.]
	if (state.objectives.empty() || font_.font() == nullptr) {
		return;
	}
	const uint32_t alpha = state.objectives_alpha;
	if (alpha == 0) {
		return; // retail draws with alpha 0 folded into every color — invisible
	}
	const uint32_t a24 = alpha << 24;
	// The header string is the gametext Overlays/STROVER_MISSIONOBJECTIVES
	// line, resolved by the embedder; the literal is the miss fallback
	// [orig: header @ 0x5ba986].
	const char *header = state.objectives_header.empty()
			? "MISSION OBJECTIVES"
			: state.objectives_header.c_str();
	// Anchor: the caller's arguments [orig: x = 15, y = dword_24C1900 + 0xF0
	// at the @ 0x5be163 site].
	const float x = 15.0f;
	const float y = 240.0f;
	// The measure pass [orig: @ 0x5ba9c9 — per-row text height scaled by the
	// overlay divisor ((h<<10)/overlayCtx) accumulates into the panel height;
	// the max width runs over the rows AND the header]. Rows here are
	// single-line, so the scaled line height stands for HUD_MeasureTextWH's
	// height. Retail measures with g_hudLabelFontLarge and draws the header
	// with g_hudLabelFontBold; the compiler's single HUD font stands in for
	// both (font-slot plumb = the remaining D-HUD-18 presentation residual).
	const float row_h = text_line_h() * kDesignH / std::max(h, 1.0f);
	float max_w = measure_text_w(header) * kDesignW / std::max(w, 1.0f);
	for (const HudObjectiveRow &row : state.objectives) {
		max_w = std::max(max_w, measure_text_w(row.text.c_str()) * kDesignW /
				std::max(w, 1.0f));
	}
	const float total_h = row_h * static_cast<float>(state.objectives.size());
	// The backing box [orig: HUD_DrawLabelBox(ctx, x, y-0x18, x+scaledW+0x48,
	// y+totalH+0x30, 0, (alpha<<24)+0xFFFFFF) @ 0x5baaba]. The box-shader
	// styling inside HUD_DrawLabelBox is unwitnessed — the fill+wire pair
	// stands in at the witnessed rect, with the panel alpha folded in.
	const float box_x1 = x + max_w + 72.0f;
	const float box_y0 = y - 24.0f;
	const float box_y1 = y + total_h + 48.0f;
	emit_rect(sx(x, w), sy(box_y0, h), sx(box_x1, w), sy(box_y1, h),
			(alpha / 2) << 24, true);
	emit_wire_rect(sx(x, w), sy(box_y0, h), sx(box_x1, w), sy(box_y1, h),
			a24 | 0x00FFFFFFu);
	// Header at x+24 [orig: Render_DrawTextScaled(ctx, x+0x18, y, ...,
	// g_hudLabelFontBold, (alpha<<24)+0xFFFFFF) @ 0x5baae4].
	emit_text(header, x + 24.0f, y, w, h, a24 | 0x00FFFFFFu, 0u);
	// Rows: y advances by the header's 0x18 first, then by each row's own
	// measured (scaled) text height [orig: esi += 0x18 @ 0x5baaf5; esi +=
	// heights[slot] @ 0x5bacc1].
	const float bx = x + 24.0f; // the checkbox x [orig: edi stays x + 0x18]
	float row_y = y + 24.0f;
	const auto line = [&](float x0, float y0, float x1, float y1,
			uint32_t color) {
		HudLine seg;
		seg.color = color;
		seg.width = 1.0f;
		seg.x0 = sx(x0, w);
		seg.y0 = sy(y0, h);
		seg.x1 = sx(x1, w);
		seg.y1 = sy(y1, h);
		draw_list_.lines.push_back(seg);
	};
	for (const HudObjectiveRow &row : state.objectives) {
		// The 16x16 checkbox outline, light gray [orig: the four
		// draw_clipped_2d_line calls @ 0x5bab47..0x5bab9a, color 0xFFE0E0E0
		// with the panel alpha as the separate modulate arg].
		const uint32_t box_c = a24 | 0x00E0E0E0u;
		line(bx, row_y, bx + 16.0f, row_y, box_c);
		line(bx + 16.0f, row_y, bx + 16.0f, row_y + 16.0f, box_c);
		line(bx + 16.0f, row_y + 16.0f, bx, row_y + 16.0f, box_c);
		line(bx, row_y + 16.0f, bx, row_y, box_c);
		if (row.done) {
			// The done mark is a RED X — both diagonals, each tripled with
			// one-pixel offsets for thickness [orig: the six calls
			// @ 0x5babc1..0x5bac5b, color 0xFFFF0000].
			const uint32_t x_c = a24 | 0x00FF0000u;
			line(bx, row_y, bx + 16.0f, row_y + 16.0f, x_c);
			line(bx + 1.0f, row_y, bx + 16.0f, row_y + 15.0f, x_c);
			line(bx, row_y + 1.0f, bx + 15.0f, row_y + 16.0f, x_c);
			line(bx + 16.0f, row_y, bx, row_y + 16.0f, x_c);
			line(bx + 15.0f, row_y, bx, row_y + 15.0f, x_c);
			line(bx + 16.0f, row_y + 1.0f, bx + 1.0f, row_y + 16.0f, x_c);
		}
		// The row color fold, exact arithmetic [orig: @ 0x5bac8c —
		// (done ? 0xFF808081 : 0) + 0xFFFFFF + (alpha<<24), wrapping to the
		// gray 0x808080 at any alpha].
		const uint32_t color = (row.done ? 0xFF808081u : 0u) + 0x00FFFFFFu + a24;
		// Text at x+0x30, one pixel above the checkbox top [orig: (edi+0x18,
		// esi-2) with g_hudLabelFontLarge @ 0x5bacb5].
		emit_text(row.text.c_str(), x + 48.0f, row_y - 2.0f, w, h, color, 0u);
		row_y += row_h;
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_attach_labels(const HudFrameState &state,
		float w, float h) {
	// [orig: draw_vehicle_seat_and_armory_labels @ 0x5a3290 — nearest at the
	// full color, others ((rgb & 0xFEFEFE) | 0xFE000001) >> 1 @ 0x5a364e]
	(void)w;
	(void)h;
	// Attach labels draw with the BOLD Arial label font at the slot scale
	// [orig: fontObj @ 0x5a3680/@ 0x5a38a1 via HUD_MeasureTextWH @ 0x580ab0 /
	// HUD_DrawTextCentered_HalfBright @ 0x580680 — both pass the slot scales].
	const bool have_label = label_font_bold_.font() != nullptr;
	const GameFont &lf = have_label ? label_font_bold_ : font_;
	const float ls = have_label ? label_scale_ : 1.0f;
	if (state.attach_labels.empty() || lf.font() == nullptr) {
		return;
	}
	for (const HudAttachLabel &label : state.attach_labels) {
		// The snapshot overlay color [orig: g_hudActiveColor reads
		// @0x5a362d/@0x5a3851; identical to hud_textcolor under the default
		// scheme 2 — D-HUD-13].
		uint32_t color = active_color(state);
		if (!label.nearest) {
			color = ((color & 0xFEFEFEu) | 0xFE000001u) >> 1;
		}
		// Screen-pixel anchors, already projected by the presenter — no
		// design scaling (the original projects then draws). The wireframe box
		// frames the measured label at the raw (dim-transformed) color:
		// (x - w/2, y - 2) .. (x + w/2 + 5, y + h + 1)
		// [orig: measure HUD_MeasureTextWH @ 0x5a3680, box
		//  Render_DrawWireframeRect @ 0x5a36ad].
		int text_w = 0;
		int text_h = 0;
		lf.measure(label.text.c_str(), ls, ls, &text_w, &text_h);
		const float half_w = static_cast<float>(text_w) * 0.5f;
		emit_wire_rect(label.screen_x - half_w, label.screen_y - 2.0f,
				label.screen_x + half_w + 5.0f,
				label.screen_y + static_cast<float>(text_h) + 1.0f, color);
		// The text rides the half-bright color mode like every HUD text draw
		// [orig: HUD_DrawTextCentered_HalfBright @ 0x5a36c1].
		const GameFontRun run = lf.layout(label.text.c_str(),
				label.screen_x, label.screen_y, ls, ls, kFontAlignCenter,
				half_bright_argb(color));
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
				run.quads.end());
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_friendly_tags(const HudFrameState &state,
		float w, float h) {
	// D-HUD-20 [orig: HUD_DrawEntityLabel @ 0x5a39b0]. Screen-pixel anchors
	// like the attach labels — the presenter projects, the compiler draws.
	(void)w;
	(void)h;
	// Friendly tags draw with the NORMAL Arial label font at the slot scale
	// [orig: g_hudLabelFont @ 0x5a3a0c; the spectated g_hudLabelFontLarge
	// Impac22b leg @ 0x5a3a29 rides the unported death screen].
	const bool have_label = label_font_.font() != nullptr;
	const GameFont &lf = have_label ? label_font_ : font_;
	const float ls = have_label ? label_scale_ : 1.0f;
	if (state.friendly_tag_mode == 0 || state.friendly_tags.empty() ||
			lf.font() == nullptr) {
		return;
	}
	// The line metric is the '0' glyph's height at the slot scale
	// [orig: GameFont_MeasureCharHeight ('0', font) @ 0x5a3a36 — the helper
	// multiplies by the slot's scale_y @ 0x580a80].
	const float font_h = lf.char_height('0', ls);
	for (const HudFriendlyTag &tag : state.friendly_tags) {
		// Too close to draw [orig: dist >= 0x8000 gate @ 0x5a3b0c] and the fog
		// cull [orig: dist <= Env_FogDistCurrent @ 0x5a3b28].
		if (tag.dist_q16 < kFriendlyTagMinDistQ16) {
			continue;
		}
		if (tag.dist_q16 > state.fog_dist_q16) {
			continue;
		}
		// Health tier -> the hudpos tag colors. The GOOD tier is scheme-
		// swapped: tagcolor_good only under hud_color_index == 2, else the
		// scheme table entry; middle/bad never swap [orig:
		// HUD_ClassifyHealthBand @ 0x5a3c2b; the index test + colors
		// @ 0x5a3c9e..0x5a3cf5; cfg default 2 @ 0x54d2a6].
		const int band = health_color_band_fp16(tag.health_ratio_fp16);
		uint32_t rgb = band == 0
				? (state.hud_color_index == 2 ? layout_.tag_good
											  : active_color(state))
				: band == 1 ? layout_.tag_middle
							: layout_.tag_bad;
		// The speaking pulse rides the voice output level [orig: @ 0x5a3e8f].
		if (tag.speaking) {
			rgb = friendly_tag_speaking_blend(rgb, state.speaking_level255);
		}
		// Distance alpha 255 -> 63 over 50..300 m [orig: @ 0x5a3eeb..0x5a3f18].
		const uint32_t argb =
				(static_cast<uint32_t>(friendly_tag_alpha(tag.dist_q16)) << 24) |
				(rgb & 0xFFFFFFu);
		const float top_y = tag.screen_y - font_h * 0.5f; // [orig: @ 0x5a4264]

		// A slot entry with an empty callsign draws the bar form
		// [orig: the empty-name leg @ 0x5a4398 — y unadjusted, height fontH].
		std::string resolved = tag.name;
		if (resolved.empty()) {
			if (tag.player) {
				HudLine bar;
				bar.color = argb;
				bar.x0 = tag.screen_x;
				bar.y0 = tag.screen_y;
				bar.x1 = tag.screen_x;
				bar.y1 = tag.screen_y + font_h;
				draw_list_.lines.push_back(bar);
				continue;
			}
			// '^' + the compiled-in name table [orig: @ 0x5a4047..0x5a40cd].
			resolved = friendly_tag_fallback_name(tag.entity_id);
		}

		if (friendly_tag_text_visible(state.friendly_tag_mode, tag.dist_q16)) {
			// Centered text at the projected point, half-bright with the
			// distance alpha kept [orig: HUD_DrawTextHalfBrightF @ 0x5a4268 ->
			// CGameFont_DrawText flags 1, the slot scales pushed @ 0x580720].
			const GameFontRun run = lf.layout(resolved.c_str(), tag.screen_x,
					top_y, ls, ls, kFontAlignCenter,
					half_bright_keep_alpha(argb));
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
			if (tag.medic) {
				// The red-cross-on-white medic plate, a fontH/2 square left of
				// the text at the tag alpha [orig: rect @ 0x5a4309..0x5a436c;
				// mesh = white quad + two red bars inset by an eighth,
				// HUD_DrawMedicCrossQuad @ 0x59bcb0].
				int text_w = 0;
				int text_h = 0;
				lf.measure(resolved.c_str(), ls, ls, &text_w, &text_h);
				const float x0 = tag.screen_x -
						(static_cast<float>(text_w) * 0.5f + font_h) - 0.5f;
				const float y0 = top_y - 0.5f;
				const float x1 = x0 + font_h * 0.5f;
				const float y1 = y0 + font_h * 0.5f;
				const uint32_t a = argb & 0xFF000000u;
				const float dx8 = (x1 - x0) * 0.125f;
				const float dy8 = (y1 - y0) * 0.125f;
				const float mid_x = (x0 + x1) * 0.5f;
				const float mid_y = (y0 + y1) * 0.5f;
				emit_rect(x0, y0, x1, y1, a | 0xFFFFFFu, true);
				emit_rect(mid_x - dx8, y0 + dy8, mid_x + dx8, y1 - dy8,
						a | 0xFF0000u, true);
				emit_rect(x0 + dx8, mid_y - dy8, x1 - dx8, mid_y + dy8,
						a | 0xFF0000u, true);
			}
		} else if (state.friendly_tag_mode == kFriendlyTagModeBrief) {
			// The BRIEF tick: three 1-px vertical lines at x-1/x/x+1 spanning
			// +-fontH/4 around the projected point [orig: @ 0x5a40eb..0x5a4160].
			const float half = font_h * 0.25f;
			for (int dx = -1; dx <= 1; ++dx) {
				HudLine seg;
				seg.color = argb;
				seg.x0 = tag.screen_x + static_cast<float>(dx);
				seg.y0 = tag.screen_y - half;
				seg.x1 = seg.x0;
				seg.y1 = tag.screen_y + half;
				draw_list_.lines.push_back(seg);
			}
		}
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_objective_line(const HudFrameState &state,
		float w, float h) {
	// [orig: draw_objective_status_text @ 0x59aa30 — the game_info anchor]
	if (state.objective_text.empty() || font_.font() == nullptr) {
		return;
	}
	const HudPosRecord &gp = layout_.game_info;
	const int gx = gp.present ? gp.x : 512;
	const int gy = gp.present ? gp.y : 40;
	if (gp.present && gp.hidden != 0) {
		return;
	}
	const uint32_t align_flags = gp.align == 1
			? kFontAlignRight
			: (gp.align == 2 ? kFontAlignCenter : 0u);
	// The objective drawer reads both overlay-color twins (frame @0x59aa4c,
	// snapshot @0x59ab0e) — one derived color here.
	emit_text(state.objective_text.c_str(), static_cast<float>(gx),
			static_cast<float>(gy), w, h, active_color(state), align_flags);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_messages(const HudFrameState &state, float w,
		float h) {
	// [orig: Chat_AddDebugMessage @ 0x4987f0 display — newest at the anchor,
	// scrolling upward, the HUDCHLINE cap]
	if (font_.font() == nullptr) {
		return;
	}
	const float ax = static_cast<float>(
			layout_.chat_text.present ? layout_.chat_text.x : 142);
	const float ay = static_cast<float>(
			layout_.chat_text.present ? layout_.chat_text.y : 711);
	std::vector<const HudMessageLine *> live;
	for (const HudMessageLine &line : messages_) {
		if (line.expire_tick > state.ticks) {
			live.push_back(&line);
		}
	}
	if (live.empty()) {
		return;
	}
	const int max_lines = std::max(layout_.chat_lines, 1);
	const int start = std::max(0,
			static_cast<int>(live.size()) - max_lines);
	const float row_h = text_line_h() * kDesignH / std::max(h, 1.0f);
	float row_y = ay;
	for (int i = static_cast<int>(live.size()) - 1; i >= start; --i) {
		// The message feed rides the snapshot overlay color like the chat
		// drawer [orig: the g_hudActiveColor read @0x5930e0].
		emit_text(live[static_cast<size_t>(i)]->text.c_str(), ax, row_y, w, h,
				active_color(state), 0u);
		row_y -= row_h;
	}
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
