// DFX2-36: dfx2.exe's EXPPOINTS element, its fade's start at the pass's head and its draw in
// the dispatcher's walk (hud_exp_points.h carries the witness map). Every input rides
// HudFrameState::exp_points, filled by the role feed (inmatch/role_feeds.h hud_role_facts).
// [orig: sub_59B730 @0x59B730; sub_59B7E0 @0x59B7E0; sub_5A6D20 @0x5A6D20 (dfx2.exe)]

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_game_text.h>

#include <cstdio>
#include <string>

namespace opennova::hud {

namespace {

// One design coordinate onto the surface, in integers [orig: Viewport_ScaleToVirtualCoords
// @0x5d2b20 - (v * dim + extent/2) / extent, the divide truncating toward zero].
int virtual_scale(int v, int surface, int extent) {
	return static_cast<int>((static_cast<int64_t>(v) * surface + extent / 2) / extent);
}

} // namespace

void HudFrameCompiler::start_exp_points_fade(const HudFrameState &state) {
	// Each 0x81 whose delta sounds the EXP_FANFARE ladder starts the fade with its tier, reading
	// g_HUDColors.active and the hudpos line as they stand; the replica counts them, and the pass
	// starts the last one's (each start overwrites the whole fade, so only the last of a frame's
	// stands).
	// [orig: @0x422350..0x4223C5 (dfx2.exe) - sub_59B730 called @0x4223C5 with the tier's
	//  duration and colour; hud_exp_points.h carries the tiers]
	const HudExpPointsState &xp = state.exp_points;
	if (!exp_fade_primed_) {
		exp_fade_arms_seen_ = xp.fade_arms;
		exp_fade_primed_ = true;
		return;
	}
	if (xp.fade_arms == exp_fade_arms_seen_) return;
	exp_fade_arms_seen_ = xp.fade_arms;
	if (!xp.element) return;
	const HudExpPointsTier tier = hud_exp_points_tier(xp.fade_tone);
	const uint32_t color = tier.palette < 0 ? hud_colors_active_ : hud_palette(tier.palette);
	hud_exp_points_fade_start(exp_fade_, layout_.exp_points.hidden != 0, tier.duration, color,
			hud_colors_active_, layout_.exp_points.pulse_percent);
}

void HudFrameCompiler::element_exp_points(const HudFrameState &state, float w, float h) {
	// THE EXPPOINTS ELEMENT, the title's (Title::hud_exp_points): under its declutter slot (9),
	// unless the hudpos line hides it. Each draw steps the fade first. The label in the bold slot
	// at the anchor, half-bright in g_HUDColors.active; the score right after the label's width,
	// its scale the fade's and its centre held on the label's middle row, half-bright in the fade's
	// colour; then, while the S2C 0x80 / 0x82 tracker is valid and neither value is negative, the
	// session and total lines, each one bold '0' height (rounded) lower. A tracker with no 0x82
	// draws no lines: their other arm, g_DebugCheatFlags & 0x8000000, is dead (nothing sets it).
	// [orig: sub_5A6D20 @0x5A6D20 (dfx2.exe) - `cmp dword_272501C, 0` @0x5A6EDE; sub_59B7E0
	//  @0x59B7E0 (dfx2.exe) - the hidden test @0x59B7F1, the step @0x59B805..0x59B91B, the anchor
	//  dword_2724A2C / dword_2724A30 through Viewport_ScaleToVirtualCoords @0x59B921..0x59B945,
	//  HUD_DrawTextLeft_HalfBright(g_HUDLabelFontBold, x, y, label, alpha, 0) @0x59B962,
	//  GameFont_MeasureCharHeight('0') @0x59B96E, GameFont_MeasureTextWidth(label) @0x59B98B, the
	//  score's x @0x59B994..0x59B9AD and y @0x59B9B1..0x59B9CF, sprintf("%d", dword_A7BA40)
	//  @0x59B998..0x59B9D3, CGameFont_DrawText at the slot's scales times the fade's
	//  @0x59B9E9..0x59BA16 in ((colour >> 1) & 0x7F7F7F) | 0xFF000000 @0x59B9F6..0x59BA06 (the
	//  half-bright drawer's arguments, flags 0) @0x59BA33, the tracker's valid word @0x59BA38, the
	//  two values' sign tests @0x59BA40..0x59BA56, the dead cheat arm @0x59BA6B, the pitch
	//  (int)(height + 0.5) @0x59BA79..0x59BA83, the lines @0x59BA90..0x59BAB7 /
	//  @0x59BABC..0x59BAE9]
	const HudExpPointsState &xp = state.exp_points;
	if (!xp.element || !state.declutter_visible[kDeclutterExpPoints]) return;
	const HudExpPointsRecord &pos = layout_.exp_points;
	if (pos.hidden != 0) return;
	const uint32_t active = hud_colors_active_;
	const HudExpPointsPulse pulse = hud_exp_points_fade_step(exp_fade_, active);
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bold = have_bold ? label_font_bold_ : font_;
	const float bold_scale = have_bold ? label_scale_ : hud_font_scale_;
	if (bold.font() == nullptr) return;
	const int x = virtual_scale(pos.x, static_cast<int>(w), 1024);
	int y = virtual_scale(pos.y, static_cast<int>(h), 768);
	emit_slot_text(label_font_bold_, label_scale_, xp.text.label.c_str(), static_cast<float>(x),
			static_cast<float>(y), half_bright_argb(active), 0u);
	const float char_h = static_cast<float>(static_cast<int>(bold.char_height('0', bold_scale)));
	// The label's width: the unscaled extent times the slot's scale, truncated [orig:
	// GameFont_MeasureTextWidth @0x580A50 - CGameFont_GetTextExtent at 1.0, the multiply and the
	// truncation].
	int label_extent = 0;
	bold.measure(xp.text.label.c_str(), 1.0f, 1.0f, &label_extent, nullptr);
	const int label_w = static_cast<int>(static_cast<double>(label_extent) * bold_scale);
	const float score_x = static_cast<float>(label_w) + static_cast<float>(x);
	const float score_y = (char_h * 0.5f + static_cast<float>(y)) - (char_h * pulse.scale) * 0.5f;
	char score[16];
	std::snprintf(score, sizeof(score), "%d", xp.score);
	const float score_scale = bold_scale * pulse.scale;
	const GameFontRun run = bold.layout(score, score_x, score_y, score_scale, score_scale, 0u,
			half_bright_argb(pulse.color));
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
	draw_list_.underlines.insert(draw_list_.underlines.end(), run.underlines.begin(),
			run.underlines.end());
	++draw_list_.elements_drawn;
	if (!xp.tracker_valid || xp.session_points < 0 || xp.total_points < 0) return;
	const int pitch = static_cast<int>(char_h + 0.5f);
	y += pitch;
	emit_slot_text(label_font_bold_, label_scale_,
			hud_sprintf(xp.text.session_format, xp.session_points).c_str(), static_cast<float>(x),
			static_cast<float>(y), half_bright_argb(active), 0u);
	y += pitch;
	emit_slot_text(label_font_bold_, label_scale_,
			hud_sprintf(xp.text.total_format, xp.total_points).c_str(), static_cast<float>(x),
			static_cast<float>(y), half_bright_argb(active), 0u);
}

} // namespace opennova::hud
