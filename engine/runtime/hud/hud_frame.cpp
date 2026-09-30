// The HUD frame compiler — the witnessed element walk over the hudpos layout,
// compiled onto the typed draw list.
// [orig: HUD_RenderAllOverlays @ 0x5a8070 -> HUD_RenderOverlays @ 0x5a7bb0]

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_game_text.h>
#include <runtime/hud/hud_medic_cross.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace opennova::fnt;

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

uint32_t map_label_double_rgb(uint32_t half) {
	const auto doubled = [](uint32_t channel) {
		return std::min(channel * 2u, 255u);
	};
	return (half & 0xFF000000u) |
			(doubled((half >> 16) & 0xFFu) << 16) |
			(doubled((half >> 8) & 0xFFu) << 8) |
			doubled(half & 0xFFu);
}

uint32_t map_label_output_argb(uint32_t argb) {
	// Retail's map-label wrapper halves the diffuse RGB (and forces the alpha
	// opaque) before the active fixed-function map/font stage doubles it.
	// Fold both operations into the Canvas glyph color, retaining the one-bit
	// loss on odd input channels. [orig: HUD_DrawTextCentered_HalfBright
	// @0x580688; HUD_DrawTextLeft_HalfBright @0x5804c6]
	return map_label_double_rgb(half_bright_argb(argb));
}

// The one map drawer that keeps the caller's alpha (the bit19 label).
// [orig: HUD_DrawTextHalfBrightF @0x580726]
uint32_t map_label_output_keep_alpha(uint32_t argb) {
	return map_label_double_rgb(half_bright_keep_alpha(argb));
}

// Trims an axis-aligned glyph quad (and its UVs) to a rect; false when
// nothing is left. Italic shear is carried by the top corners unchanged.
bool clip_glyph_to_rect(GameFontQuad &q, float x1, float y1, float x2,
		float y2) {
	const float left = std::min(q.x_top_left, q.x_bottom_left);
	const float right = std::max(q.x_top_right, q.x_bottom_right);
	if (right <= x1 || left >= x2 || q.y_bottom <= y1 || q.y_top >= y2)
		return false;
	const float width = right - left;
	const float height = q.y_bottom - q.y_top;
	if (width > 0.0f) {
		const float cut_l = std::max(0.0f, x1 - left);
		const float cut_r = std::max(0.0f, right - x2);
		const float du = q.u1 - q.u0;
		q.u0 += du * (cut_l / width);
		q.u1 -= du * (cut_r / width);
		q.x_top_left += cut_l;
		q.x_bottom_left += cut_l;
		q.x_top_right -= cut_r;
		q.x_bottom_right -= cut_r;
	}
	if (height > 0.0f) {
		const float cut_t = std::max(0.0f, y1 - q.y_top);
		const float cut_b = std::max(0.0f, q.y_bottom - y2);
		const float dv = q.v1 - q.v0;
		q.v0 += dv * (cut_t / height);
		q.v1 -= dv * (cut_b / height);
		q.y_top += cut_t;
		q.y_bottom -= cut_b;
	}
	return true;
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
	hudpos_font_ = font;
	resolve_hud_font_();
	reset_runtime_state();
}

void HudFrameCompiler::set_hudpos_font(const fnt_font_t *font) {
	hudpos_font_ = font;
	resolve_hud_font_();
}

void HudFrameCompiler::resolve_hud_font_() {
	// [orig: HUD_SelectHudposFont @0x591890: the named font at 0x10000 (1.0)
	// @0x5918B4, else the whole bold slot, font and scale @0x5918C3..0x5918D6]
	if (hudpos_font_ != nullptr) {
		font_.set_font(hudpos_font_);
		font_.set_page_base(static_cast<uint32_t>(kHudFontSlotHud * FNT_MAX_PAGES));
		hud_font_scale_ = 1.0f;
	} else if (label_font_bold_.font() != nullptr) {
		font_.set_font(label_font_bold_.font());
		font_.set_page_base(static_cast<uint32_t>(kHudFontSlotLabelBold * FNT_MAX_PAGES));
		hud_font_scale_ = label_scale_;
	} else {
		font_.set_font(nullptr);
		font_.set_page_base(static_cast<uint32_t>(kHudFontSlotHud * FNT_MAX_PAGES));
		hud_font_scale_ = 1.0f;
	}
}

void HudFrameCompiler::configure_label_fonts(const fnt_font_t *normal,
		const fnt_font_t *bold, const fnt_font_t *large, float scale,
		float large_scale, const fnt_font_t *impact38) {
	// [orig: HUD_InitAllFonts @ 0x51ee20 stores each slot through the
	// {font, scale_x, scale_y} slot writer @ 0x580453..0x580468]
	label_font_.set_font(normal);
	label_font_.set_page_base(
			static_cast<uint32_t>(kHudFontSlotLabel * FNT_MAX_PAGES));
	label_font_bold_.set_font(bold);
	label_font_bold_.set_page_base(
			static_cast<uint32_t>(kHudFontSlotLabelBold * FNT_MAX_PAGES));
	label_font_large_.set_font(large);
	label_font_large_.set_page_base(
			static_cast<uint32_t>(kHudFontSlotLabelLarge * FNT_MAX_PAGES));
	label_font_impact38_.set_font(impact38);
	label_font_impact38_.set_page_base(
			static_cast<uint32_t>(kHudFontSlotImpact38 * FNT_MAX_PAGES));
	label_scale_ = scale > 0.0f ? scale : 1.0f;
	label_large_scale_ = large_scale > 0.0f ? large_scale : 1.0f;
	resolve_hud_font_();
}

void HudFrameCompiler::update_layout(const HudLayout &layout) {
	// Texture-table refresh only — the fade/flash/message state survives
	// [orig: HUD_LoadAllTextures @ 0x59e3d6 reloads art without a HUD reset].
	layout_ = layout;
}

void HudFrameCompiler::reset_overlay_buffers() {
    stance_.stamp = 0;
    flash_stamp_ = 0;
	silhouette_stamp_ = 0;
    draw_list_ = HudDrawList{};
}

void HudFrameCompiler::reset_runtime_state() {
	stance_ = StanceFade{};
	flash_key_bucket_ = 0;
	flash_key_reserve_ = 0;
	flash_key_class_ = 0;
	flash_stamp_ = 0;
	hud_colors_active_ = 0;
	hud_colors_active_index_ = -1;
	silhouette_stamp_ = 0;
	silhouette_vehicle_ = 0;
	silhouette_weapon_.clear();
	feed_lines_.clear();
	chat_lines_.clear();
	draw_list_ = HudDrawList{};
}

void HudFrameCompiler::push_chat_line(const std::string &text, uint32_t argb,
		int now_ticks) {
	// The CHAT display-buffer sink [orig: Chat_AddMessageChannel1 @0x4985d0].
	if (text.empty()) {
		return;
	}
	// The wrap: width `x2 - (x1 - 4)` of the chat box, the bold label font AT
	// ITS NATIVE (design) SIZE, current_x 0; a 0 count (no font) is 1
	// [orig: @0x498673..0x4986c5]. The wrapper measures the FONT OBJECT, not
	// the HUD slot with its scale pair: HUD_WordWrapText dereferences *fontPtr
	// @0x58098a and measures through CGameFont_GetTextExtent @0x5809ad /
	// CGameFont_GetCharExtent (ex sub_674DC0) @0x5809db with no slot scale, against a width authored in
	// design units — so the measure here is scale 1, never label_scale_. The
	// wrapper walks the FULL message (the 119-char cap is per copied segment
	// @0x498781, and on the raw ring @0x498621).
	std::string buf = text;
	int count = 0;
	if (layout_.chat_box_present && label_font_bold_.font() != nullptr) {
		const int width = layout_.chat_box_x2 - (layout_.chat_box_x1 - 4);
		count = chat_wrap_text(label_font_bold_, 1.0f, buf, width, 0);
	}
	if (count <= 0) {
		count = 1;
	}
	// Slot 1's timer reads slot 2 AFTER the shift zeroed the new slots
	// [orig: memset @0x498718, then dword_B3FF38 + 186 @0x498722..0x498734]:
	// a one-segment post staggers against the previous newest line, a longer
	// one against its own zeroed continuation slot.
	const bool has_prev = !chat_lines_.empty();
	const int prev_expire = has_prev ? chat_lines_.back().expire_tick : 0;
	const int newest_expire = count == 1
			? message_expire_tick(now_ticks, prev_expire, has_prev)
			: message_expire_tick(now_ticks, 0, false);
	// The segments: first highest, last in slot 1, continuation prefixed "  "
	// [orig: the fill loop @0x498750..0x4987da].
	size_t pos = 0;
	for (int k = 0; k < count; ++k) {
		const char *seg = buf.c_str() + pos;
		const size_t seg_len = std::strlen(seg);
		HudMessageLine line;
		line.text = k == 0 ? std::string(seg, seg_len)
						   : "  " + std::string(seg, seg_len);
		if (line.text.size() > static_cast<size_t>(kMessageTextMax)) {
			line.text.resize(static_cast<size_t>(kMessageTextMax));
		}
		line.color = argb;
		// Timer 0 on every slot but slot 1: remaining life 0 from this tick.
		line.expire_tick = k == count - 1 ? newest_expire : now_ticks;
		chat_lines_.push_back(line);
		pos += seg_len + 1;
		if (pos > buf.size()) {
			pos = buf.size();
		}
	}
	while (chat_lines_.size() > static_cast<size_t>(kMessageSlotCount)) {
		chat_lines_.erase(chat_lines_.begin());
	}
}

int chat_wrap_text(const GameFont &font, float scale, std::string &text,
		int max_width, int current_x) {
	// [orig: HUD_WordWrapText @0x580980 — the recursion unrolled into a loop over
	//  the remaining text; every test and step is the witnessed one]
	if (font.font() == nullptr) {
		return 0;
	}
	int lines = 0;
	size_t pos = 0;
	while (true) {
		const char *seg = text.c_str() + pos;
		int extent = 0;
		int unused_h = 0;
		font.measure(seg, scale, scale, &extent, &unused_h);
		// The fit test adds current_x to the threshold while the walk below
		// starts its cursor AT current_x — both as written [orig: @0x5809c1
		//  vs @0x5809b6..0x5809ea].
		if (extent < current_x + max_width) {
			return lines + 1;
		}
		if (text[pos] == 0) {
			// An empty remainder that fails the fit test (a non-positive
			// width) would recurse forever in retail; it is one line here.
			return lines + 1;
		}
		size_t cursor = pos;
		size_t last_space = std::string::npos;
		int x = current_x;
		bool broke = false;
		while (true) {
			const unsigned char c = static_cast<unsigned char>(text[cursor]);
			if (c == ' ') {
				last_space = cursor;
			}
			x += font.char_width(c, scale) + 1;
			if (x > max_width) {
				broke = true;
				break;
			}
			++cursor;
			if (text[cursor] == 0) {
				break;
			}
		}
		if (broke) {
			if (last_space == std::string::npos) {
				return lines + 1;
			}
			text[last_space] = 0;
			++lines;
			cursor = last_space + 1;
		}
		pos = cursor;
		current_x = 2 * font.char_width(static_cast<uint8_t>(' '), scale);
	}
}

void HudFrameCompiler::push_message(const std::string &text, int now_ticks) {
	// Mission triggered text posts into the ONE system ring with the default
	// white — retail routes it through the same sink as every 0x1E line
	// [orig: HUD_DisplayTriggeredText @ 0x51f190 ->
	// Chat_AddMessageChannel2(text, -1, 930) @ 0x51f216].
	push_feed_line(text, 0xFFFFFFFFu, now_ticks);
}

void HudFrameCompiler::push_feed_line(const std::string &text, uint32_t argb,
		int now_ticks) {
	// The SYSTEM ring sink [orig: Chat_AddMessageChannel2 @ 0x4987f0 — 930-tick
	// life, >= 186-tick stagger, 119-char slots]. The caller's packed color is
	// stored RAW and drawn as stored [orig: the second HUD_DrawConsoleMessages
	// loop passes the stored dword @0x59ae97].
	if (text.empty()) {
		return;
	}
	const bool has_prev = !feed_lines_.empty();
	const int prev_expire = has_prev ? feed_lines_.back().expire_tick : 0;
	HudMessageLine line;
	line.text = text.substr(0, static_cast<size_t>(kMessageTextMax));
	line.expire_tick = message_expire_tick(now_ticks, prev_expire, has_prev);
	line.color = argb;
	feed_lines_.push_back(line);
	while (feed_lines_.size() > static_cast<size_t>(kMaxCarriedMessages)) {
		feed_lines_.erase(feed_lines_.begin());
	}
}

// One atlas cell. emit_rect covers the whole-texture case; the stdbox border
// and the connection icon both need a sub-rect, so they push the quad directly.
void HudFrameCompiler::emit_rect_uv(float x0, float y0, float x1, float y1,
		float u0, float v0, float u1, float v1, uint32_t color,
		int32_t texture) {
	HudQuad q;
	q.x0 = x0;
	q.y0 = y0;
	q.x1 = x1;
	q.y1 = y1;
	q.u0 = u0;
	q.v0 = v0;
	q.u1 = u1;
	q.v1 = v1;
	q.color = color;
	q.texture = texture;
	q.filled = true;
	draw_list_.quads.push_back(q);
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
	font_.measure(text, hud_font_scale_, hud_font_scale_, &w, &h);
	return static_cast<float>(w);
}

float HudFrameCompiler::text_line_h() const {
	return font_.line_height(hud_font_scale_);
}

void HudFrameCompiler::emit_text(const char *text, float design_x,
		float design_y, float surface_w, float surface_h, uint32_t argb,
		uint32_t flags) {
	if (text == nullptr || text[0] == 0 || font_.font() == nullptr) {
		return;
	}
	const GameFontRun run = font_.layout(text, sx(design_x, surface_w),
			sy(design_y, surface_h), hud_font_scale_, hud_font_scale_, flags, argb);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
			run.quads.end());
	draw_list_.underlines.insert(draw_list_.underlines.end(),
			run.underlines.begin(), run.underlines.end());
}

void HudFrameCompiler::emit_slot_text(const GameFont &slot, float slot_scale, const char *text,
		float surface_x, float surface_y, uint32_t argb, uint32_t flags) {
	const bool have_slot = slot.font() != nullptr;
	const GameFont &font = have_slot ? slot : font_;
	if (text == nullptr || text[0] == 0 || font.font() == nullptr) {
		return;
	}
	const float scale = have_slot ? slot_scale : hud_font_scale_;
	const GameFontRun run = font.layout(text, surface_x, surface_y, scale, scale, flags, argb);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
	draw_list_.underlines.insert(draw_list_.underlines.end(), run.underlines.begin(),
			run.underlines.end());
}

// Seat-specific WPNGRP dispatch, independent of the crosshair's XHAIRS gate.
// [orig: HUD_RenderOverlays @0x5A7CBE..0x5A7D55]
bool hud_weapon_group_visible(const HudFrameState &state) {
    if (state.mount_slot == 2 || state.mount_slot == 5)
        return state.weapon_category > 9;
    return (state.mount_slot == 0 || state.mount_slot == 1 || state.mount_slot == 3) &&
        state.declutter_visible[kDeclutterWpnGrp];
}
bool hud_stance_group_visible(const HudFrameState &state) {
    if (state.mount_slot == 1 || state.mount_slot == 2 || state.mount_slot == 5) return true;
    return (state.mount_slot == 0 || state.mount_slot == 3) &&
        state.declutter_visible[kDeclutterWpnGrp];
}

void HudFrameCompiler::mark_order_break() {
	draw_list_.order_breaks.push_back({ draw_list_.quads.size(), draw_list_.tris.size(),
			draw_list_.lines.size(), draw_list_.glyphs.size(), draw_list_.underlines.size() });
}

const HudDrawList &HudFrameCompiler::compile(const HudFrameState &state,
		float surface_w, float surface_h) {
	draw_list_.quads.clear();
	draw_list_.tris.clear();
	draw_list_.lines.clear();
	draw_list_.glyphs.clear();
	draw_list_.underlines.clear();
	draw_list_.map.visible = false;
	draw_list_.big_map.visible = false;
	draw_list_.map_glyphs.clear();
	draw_list_.big_map_glyphs.clear();
	draw_list_.top_begin = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
	draw_list_.order_breaks.clear();
	draw_list_.elements_drawn = 0;

	// The SIGHTS card draws first — the HUD overlays land on top of it
	// [orig: HUD_DrawWeaponSightOverlays @ 0x4dce00 runs at scene end;
	//  HUD_RenderAllOverlays later in the frame]. It rides the scene pass,
	//  not the overlay pass, so the level-3 early-out below never covers it.
	element_sights_card(state, surface_w, surface_h);

	// The snapshot twin's two non-frame writers: the HUD-init seed with the
	// forced alpha, and the hudcolor cycle's table[index] store — seen here as
	// the scheme index changing between frames [orig: HUD_InitTeamColorTable
	// @0x51F240; the cycle @0x49AFC7]. The per-frame restamp is the GAMEINFO
	// drawer's (element_game_info).
	{
		const int idx = state.hud_color_index >= 0 && state.hud_color_index <= 5
				? state.hud_color_index
				: 2;
		if (hud_colors_active_index_ < 0)
			hud_colors_active_ = hud_palette(idx) | 0xFF000000u;
		else if (idx != hud_colors_active_index_)
			hud_colors_active_ = hud_palette(idx);
		hud_colors_active_index_ = idx;
	}

	// /NOHUD clears the overlay master word: HUD_DrawGameplayOverlays returns
	// whole on a zero word (the breath bar, the service prompts and the
	// windows are its legs) [orig: `cmp dword_840B18, 0; jz` @0x5BDE9B].
	const bool gameplay_overlays = state.overlay_master != 0u;

	// hud_detail level 3 blanks the ENTIRE gameplay overlay pass — the walk
	// below never runs [orig: the early-out @ 0x5A80C4..0x5A80DB; the
	// death-screen "exception" arm calls a spectate-label fn whose own
	// `level < 2` guard makes it a structural no-op, so the early-out carries
	// with no exception]. Only the M-cycle big map still compiles: it rides
	// Render_ProcessMainSceneFrame, not this pass [orig: @ 0x5cac50 ->
	// HUD_BuildMapOverlayView @ 0x5a7e10] — element_spinmap's own gates
	// suppress the corner map at this level.
	// The breath bar and the service prompts are HUD_DrawGameplayOverlays'
	// legs, outside that early-out; the bar is called first
	// [orig: HUD_DrawGameplayOverlays @0x5BDE60 — the bar @0x5BDED3, the
	//  prompts @0x5BDF1B..0x5BE10E].
	if (gameplay_overlays) {
		element_breath_bar(state, surface_w, surface_h);
		element_service_prompt(state, surface_w, surface_h);
	}
	element_inset_cues(state, surface_w, surface_h);
	if (state.hud_detail_level >= 3) {
		element_spinmap(state, surface_w, surface_h);
		// The frame drawer's panels run outside the level-3 skip: the chat
		// input line precedes the kill banner [orig: HUD_DrawOverlayPanels
		// @0x5c014e then HUD_DrawKillAnnounceBanner @0x5c0184]. The voice-macro
		// menus and the pause text are the same pass's, ahead of the input line.
		compile_overlay_panel_menus(state, surface_w, surface_h);
		element_chat_input(state, surface_w, surface_h);
		element_kill_announcement(state, surface_w, surface_h);
		compile_gameplay_overlay_windows(state, surface_w, surface_h);
		return draw_list_;
	}
	compile_overlay_pass(state, surface_w, surface_h);
	// The Recent Messages window draws from the frame drawer, not the overlay
	// pass: after every HUD element and BEFORE the Tab board
	// [orig: Server_DrawStatusScreen @0x50a2d0 — HUD_DrawMessageLog @0x50b21f,
	//  then HUD_DrawClassRosterOverlay @0x50b23d and HUD_DrawPlayerScoreList
	//  @0x50b281].
	// The stats panel precedes the message log in the frame drawer
	// [orig: HUD_DrawOverlayPanels @0x5c0083 (stats) then @0x5c009a (message log)].
	element_end_round_statistics(state, surface_w, surface_h);
	element_message_log(state, surface_w, surface_h);
	element_scoreboard(state, surface_w, surface_h);
	element_end_round_overlay(state, surface_w, surface_h);
	// [orig: HUD_DrawOverlayPanels — the voice-macro menus @0x5c00d8 /
	// @0x5c00ea and the pause text @0x5c0120 (compile_overlay_panel_menus),
	// the input line @0x5c014e, then the kill banner @0x5c0184]
	compile_overlay_panel_menus(state, surface_w, surface_h);
	element_chat_input(state, surface_w, surface_h);
	element_kill_announcement(state, surface_w, surface_h);
	compile_gameplay_overlay_windows(state, surface_w, surface_h);
	return draw_list_;
}

// THE GAMEPLAY OVERLAY PASS below the level-3 early-out [orig:
// HUD_RenderAllOverlays @0x5a8070]. /NOHUD returns after the palette and radar
// work (bit 1 of the master word), so only the M-cycle big map — a pass of its
// own — still compiles [orig: `test dword_840B18, 2` @0x5A81CE; the big map
// Render_ProcessMainSceneFrame @0x5cac50 -> HUD_BuildMapOverlayView @0x5a7e10].
void HudFrameCompiler::compile_overlay_pass(const HudFrameState &state, float surface_w,
		float surface_h) {
	if ((state.overlay_master & 2u) == 0u) {
		element_spinmap(state, surface_w, surface_h);
		return;
	}
	// The GAMEINFO/ZONEINFO overlay (and the g_HUDColors.active restamp) is
	// the first draw after the gate, before the crosshair and the element
	// dispatcher [orig: HUD_DrawGameTimerOverlay @0x5A8499].
	element_game_info(state, surface_w, surface_h);

	// The stance cross-fade restamp [orig: @ 0x599f8a; it lives inside the
	// stance drawer, so a blanked level-3 pass never restamps — matched by
	// placing it under the early-out].
	if (hud_stance_group_visible(state) && state.stance != stance_.cur) {
		stance_.prev = stance_.cur;
		stance_.cur = state.stance;
		stance_.stamp = state.ticks;
	}

	element_frame(surface_w, surface_h);
	element_health(state, surface_w, surface_h);
	element_instruments(state, surface_w, surface_h);
	element_optical_cues(state, surface_w, surface_h);
	element_stance(state, surface_w, surface_h);
	element_weapon_cluster(state, surface_w, surface_h);
	element_heat(state, surface_w, surface_h);
	// The CLOCK pair follows the heat bar and the icons in the dispatcher
	// [orig: HUD_RenderOverlays @0x5A7D75..0x5A7D7C].
	element_clock(state, surface_w, surface_h);
	element_power(state, surface_w, surface_h);
	element_waypoint(state, surface_w, surface_h);
	// The TEAMID line then the HUDLS bar close the dispatcher's walk
	// [orig: @0x5A7DE9, @0x5A7DF7].
	element_team_id_line(state, surface_w, surface_h);
	element_weapon_slot_bar(state, surface_w, surface_h);
	// The AAS zone status panel draws BEFORE the map overlay in the retail
	// walk [orig: HUD_RenderAllOverlays @0x5a8070 — HUD_DrawZoneStatusPanel
	//  @0x5a8530, then HUD_DrawVehicleBayLogos (ex Radar_DrawBlips) @0x5a8535,
	//  the 3-D icon pass, and HUD_DrawMapOverlay @0x5a87bb]. The panel has NO
	//  declutter-mask bit of its own: the call @0x5a8530 is unconditional (the
	//  render_capture_point_labels / bay-logo pair around it likewise) and the
	//  function's head tests only g_GameType [orig: @0x5a248d..0x5a24c5], so it
	//  draws on the shown flag alone here.
	element_scope_details(state, surface_w, surface_h);
	element_lfp_panel(state, surface_w, surface_h);
	// The vehicle-bay logos: no mask bit, no death-screen test (the only
	// death-screen fork in this stretch covers the scope details)
	// [orig: HUD_DrawVehicleBayLogos @0x5a8535; the fork @0x5a850d..0x5a852b].
	element_vehicle_bay_logos(state, surface_w, surface_h);
	element_spinmap(state, surface_w, surface_h);
	element_attach_labels(state);
	// Friendly tags draw after the overlay cluster and before the console
	// messages, exactly the retail pass order [orig: HUD_DrawFriendlyTagsPass
	// @ 0x5a87cc, then HUD_DrawConsoleMessages @ 0x5a87d1].
	element_friendly_tags(state);
	// The mounted-vehicle panel sits with the overlay cluster, BEFORE the feed
	// and the Tab board -- both of those are held-open surfaces that should
	// cover it, not the other way round. The panel rides the seat dispatch's
	// stance arms (slots 1 / 2 / 5, and 3 under WPNGRP); seat slot 0 has no
	// panel arm of its own, so a hidden WPNGRP hides it there too
	// [orig: HUD_RenderOverlays -- no root @0x5A7CAE; the panel calls
	//  @0x5A7CE4 / @0x5A7CFF / @0x5A7D23; `cmp eax,1; jnz` @0x5A7CF5..0x5A7CF8].
    if (hud_stance_group_visible(state))
        element_vehicle_panel(state, surface_w, surface_h);
	// The console messages close the overlay pass [orig: HUD_DrawConsoleMessages
	//  @0x5a87d1, after HUD_DrawFriendlyTagsPass @0x5a87cc].
	element_feed(state, surface_w, surface_h);
	// The squad order lines right after the feed [orig: sub_59AEE0 @0x5a87d6;
	//  the earlier call @0x5a8508 off the death screen draws the same two
	//  lines at the same place first].
	element_squad_orders(state, surface_w, surface_h);
}

// The overlay-panel pass's middle legs, after the message log: the F9 emotes
// menu then the F10 radio menu, both only while the death screen is down,
// then the SP pause text [orig: HUD_DrawOverlayPanels @0x5c00cf..0x5c00f9
// (`cmp g_DeathScreenActive` @0x5c00cf, dword_24C18D4 @0x5c00d8,
// dword_24C18D8 @0x5c00ea), dword_A87050 @0x5c0120]. The pass runs outside
// the level-3 early-out (its caller is Render_ProcessMainSceneFrame
// @0x5cae3b).
void HudFrameCompiler::compile_overlay_panel_menus(const HudFrameState &state, float w, float h) {
	if (!state.combat.death_screen) {
		element_voice_macro_menu(state, state.emotes_menu, kHudEmotesMenuContextRow, w, h);
		element_voice_macro_menu(state, state.radio_menu, kHudRadioMenuContextRow, w, h);
	}
	element_paused_text(state, w, h);
}

// The windows HUD_DrawGameplayOverlays draws after its level-3 skip, so they
// survive the blank level, and after the big map, so they layer above it
// (HudDrawList::top_begin): the briefing, the objectives panel, then the
// help screen / map legend [orig: HUD_DrawGameplayOverlays @0x5BDE60 — the
// level-3 jump to LABEL_24 @0x5bdeaf skips only the framerate / breath /
// ping / timer / armory legs; the briefing @0x5be133..0x5be145 (a session
// draws HUD_DrawEndGameScreen instead), HUD_DrawWinConditions @0x5be163,
// HelpScreen_Draw @0x5be179; the caller runs after the big map,
// Render_ProcessMainSceneFrame @0x5cad15 then @0x5cae0b]. The whole function
// returns on a zero /NOHUD master word [orig: @0x5BDE9B].
void HudFrameCompiler::compile_gameplay_overlay_windows(const HudFrameState &state, float w,
		float h) {
	mark_top_layer();
	if (state.overlay_master == 0u) return;
	element_briefing(state, w, h);
	element_objectives(state, w, h);
	element_help_screen(state, w, h);
}

// docs/interface/hud-re.md (D-HUD-27, D-HUD-30).
// [orig: HUD_DrawScopeOverlayDetails @ 0x59e420].
void HudFrameCompiler::element_scope_details(const HudFrameState &state, float w, float h) {
    const auto &scope = state.scope;
    if (!scope.active || state.binoculars_view_active) return;
    const uint32_t color = active_color(state);
    const auto draw = [&](const std::string &text, const HudPosRecord &pos, uint32_t tint) {
        emit_text(text.c_str(), pos.x, pos.y, w, h, half_bright_argb(tint), 0u);
    };
    // Every readout is the CRT sprintf of its template: the over-1km, auto and
    // none labels with NO argument (%% collapses, a conversion stays literal),
    // the rest with their one int (hud_game_text.h hud_sprintf).
    if (scope.rangefinder) { // Flags & 0x400 @0x59e4a9
        const auto text = scope.range_q16 > 1000 * 65536
            ? hud_sprintf(scope.range_over_1km) // @0x59e4d5
            : hud_sprintf(scope.range_format, std::max(scope.range_q16 / 65536, 1));
        const bool beyond = scope.max_range_q16 != 0 && scope.range_q16 > scope.max_range_q16;
        draw(text, layout_.scope_range, beyond ? 0xFFFF5050u : color);
    }
    if (scope.zeroable) { // Flags & 0x800 @0x59e8a2
        const auto text = scope.zero_word < 0
            ? hud_sprintf(scope.zero_word == -1 ? scope.zero_auto : scope.zero_none) // @0x59e938
            : hud_sprintf(scope.zero_format, scope.zero_step_metres * scope.zero_word);
        draw(text, layout_.scope_zero, color);
    }
    if (scope.scoped)
        draw(hud_sprintf(scope.magnification_format, scope.magnification), layout_.scope_mag, color);
    ++draw_list_.elements_drawn;
}

// The gameplay pass draws whenever the HUDSPINMAP rect is authored — the
// retail master switch is a compiled-in constant true and only /NOHUD
// suppresses the overlay set. [orig: HUD_RenderAllOverlays @0x5a86e8 gate
//  dword_2723CC4 (static -1); /NOHUD mask @0x4a7a09/@0x840B18]
// The corner map's declutter gates: the whole spinmap block sits inside the
// showhud bit-1 test, with the HUDDECLUT slot-17 cmp nested inside it (and
// the level-3 early-out blanks the pass wholesale — compile()'s arm re-enters
// element_spinmap for the big map only). The big-map pass rides
// Render_ProcessMainSceneFrame and none of these gates.
// [orig: showhud test 2 @0x5A8635; slot-17 cmp @0x5A86E8; level early-out
//  @0x5A80C4; big map @0x5cac50]
bool HudFrameCompiler::corner_spinmap_visible(const HudFrameState &state) const {
	// The corner map's gates: the authored HUDSPINMAP rect, the /NOHUD
	// overlay master's bit 1, the level-3 early-out, the HUDDECLUT slot-17
	// cmp nested in the showhud bit-1 test [orig: /NOHUD mask @0x4a7a09 /
	// @0x5a81ce; level early-out @0x5A80C4; showhud test 2 @0x5A8635;
	// slot-17 cmp @0x5A86E8].
	return layout_.spinmap_rect.present && (state.overlay_master & 2u) != 0u &&
			state.hud_detail_level < 3 && state.declutter_visible[kDeclutterSpinmap] &&
			(state.showhud_flags & 2u) != 0u;
}

uint32_t HudFrameCompiler::radar_frame_gates(const HudFrameState &state) const {
	// [orig: g_SpawnSuccessGate @0x5a8084; g_HUDDetailLevel == 3 @0x5a80c4]
	if (state.spawn_success_gate || state.hud_detail_level >= 3) return 0u;
	// The corner map's own update site needs mask bits 9, 6 and 10
	// [orig: @0x5a78ff / @0x5a7906 / @0x5a790a].
	const bool map_site = corner_spinmap_visible(state) &&
			(state.minimap.flags & 0x640u) == 0x640u;
	return kRadarGatePass | (map_site ? kRadarGateMapSite : 0u);
}

void HudFrameCompiler::element_spinmap(const HudFrameState &state, float w,
		float h) {
	// The big-map pass runs regardless of the authored corner rect
	// (corner_spinmap_visible above carries the corner gates).
	const bool corner_visible = corner_spinmap_visible(state);
	if (!corner_visible && state.minimap.map_mode == 0) return;
	// Copy-assign into the persistent input so the markers vector reuses its
	// capacity — a fresh local re-allocated it every frame.
	HudMinimapInput &input = minimap_input_;
	input = state.minimap;
	if (state.combat.impact_map) {
		HudMinimapMarker marker;
		marker.bank = uint8_t(HudMinimapBank::kSpecial);
		marker.icon = 254;
		marker.x = state.combat.impact_x_q16;
		marker.y = state.combat.impact_y_q16;
		marker.z = state.combat.impact_radius_q16;
		marker.color = 0xFF208020u;
		marker.flags = 208;
		// Each live preview refreshes the original special-slot lifetime.
		// [orig: MapOverlay_InitSlot @0x5BEA19]
		marker.remaining_ticks = 1984;
		input.markers.push_back(marker);
	}
	input.footprints = &state.map_footprints;
	input.rect_x1 = layout_.spinmap_rect.x;
	input.rect_y1 = layout_.spinmap_rect.y;
	input.rect_x2 = layout_.spinmap_rect.x + layout_.spinmap_rect.w;
	input.rect_y2 = layout_.spinmap_rect.y + layout_.spinmap_rect.h;
	input.surface_w = w;
	input.surface_h = h;
	input.ticks = state.ticks;
	input.item_flash = state.item_flash;
	input.waypoint_present = state.waypoint.present;
	input.waypoint_x = state.waypoint.world_x;
	input.waypoint_y = state.waypoint.world_y;
	input.waypoint_z = state.waypoint.world_z;
	input.waypoint_distance_offset = layout_.spinmap_wp_dist_off;
	// The nearest FARP the HUD info build resolved, the tether's neutral
	// colour (g_HudposTextColor = the hudpos hud_textcolor), and the non-bank
	// legs' feed (hud_minimap.h HudMinimapOverlays).
	input.farp_present = state.combat.farp_present;
	input.farp_x = state.combat.farp_x_q16;
	input.farp_y = state.combat.farp_y_q16;
	input.hudpos_text_color = layout_.hud_text;
	input.overlays = &state.map_overlays;
	input.map_coords_x = layout_.map_coords_x;
	input.map_coords_y = layout_.map_coords_y;
	input.map_coords_off = layout_.map_coords_off;
	input.overlay_color = active_color(state);
	// The corner spinmap compiles as mode 0 (when its gates above pass); an
	// active M-cycle mode compiles the big map as a second pass over it
	// (retail draws both). [orig: HUD_RenderAllOverlays spinmap ctx + the
	//  mode-gated HUD_BuildMapOverlayView pass @0x5cac50]
	const int map_mode = input.map_mode;
	if (corner_visible) {
		input.map_mode = 0;
		minimap_compiler_.compile(input, draw_list_.map);
	}
	if (map_mode != 0) {
		input.map_mode = map_mode;
		minimap_compiler_.compile(input, draw_list_.big_map);
	}
	// Map text: the corner-map distance/MAPCOORDS labels ride the bold label
	// font, the grid letters/numbers and the big map's player readout the
	// LARGE slot — every one through the CPU half-bright drawer. The active
	// fixed-function map/font stage then applies MODULATE2X, so the Canvas
	// compiler folds that second operation into the final glyph diffuse.
	// [orig: HUD_DrawTextCentered_HalfBright((int)&g_HUDLabelFontBold, ...)
	//  @0x5a7ab5; HUD_DrawTextRightAligned_HalfBright @0x59cc47;
	//  HUD_DrawTextCentered_HalfBright(g_HUDLabelFontLarge, ...) in the
	//  @0x5a5f40 grid branch]. Each pass keeps its own glyph list so the
	//  device leg can layer them inside that pass's sandwich.
	// An invisible pass keeps its last-compiled label rows (only compile()
	// resets a pass) — the device discards it whole, so skip the glyph
	// layout instead of rebuilding quads for a closed map every frame.
	if (draw_list_.map.visible)
		layout_map_labels(draw_list_.map, draw_list_.map_glyphs);
	if (draw_list_.big_map.visible)
		layout_map_labels(draw_list_.big_map, draw_list_.big_map_glyphs);
	if (draw_list_.map.visible) ++draw_list_.elements_drawn;
	if (draw_list_.big_map.visible) ++draw_list_.elements_drawn;
}

void HudFrameCompiler::layout_map_labels(const HudMapPass &pass,
		std::vector<GameFontQuad> &out) const {
	layout_map_labels(pass, 0, pass.labels.size(), out);
}

void HudFrameCompiler::layout_map_labels(const HudMapPass &pass, size_t begin, size_t end,
		std::vector<GameFontQuad> &out) const {
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bold_font = have_bold ? label_font_bold_ : font_;
	const float bold_scale = have_bold ? label_scale_ : 1.0f;
	const bool have_large = label_font_large_.font() != nullptr;
	const GameFont &large_font = have_large ? label_font_large_ : bold_font;
	const float large_scale = have_large ? label_large_scale_ : bold_scale;
	// The regular g_HUDLabelFont[0] face the bit5 names draw with.
	// [orig: HUD_DrawEntityLabelsAndMarkers @0x5a4ea6 / @0x5a4f95]
	const bool have_regular = label_font_.font() != nullptr;
	const GameFont &regular_font = have_regular ? label_font_ : bold_font;
	const float regular_scale = have_regular ? label_scale_ : bold_scale;
	end = std::min(end, pass.labels.size());
	for (size_t li = begin; li < end; ++li) {
		const HudMapLabel &label = pass.labels[li];
		const GameFont &lf = label.font == 1 ? large_font
				: (label.font == 2 ? regular_font : bold_font);
		const float ls = label.font == 1 ? large_scale
				: (label.font == 2 ? regular_scale : bold_scale);
		if (lf.font() == nullptr) continue;
		const uint32_t flags = label.align == 1 ? kFontAlignRight
				: (label.align == 2 ? 0u : kFontAlignCenter);
		const GameFontRun run = lf.layout(label.text, label.x,
				label.y, ls, ls, flags,
				label.keep_alpha != 0
						? map_label_output_keep_alpha(label.color)
						: map_label_output_argb(label.color));
		for (const GameFontQuad &quad : run.quads) {
			if (label.clip == 0) {
				out.push_back(quad);
				continue;
			}
			// An in-pass label crops to the rect viewport per pixel.
			// [orig: SetViewport(rect) @0x5a64a8 .. restore @0x5a78e3]
			GameFontQuad clipped = quad;
			if (clip_glyph_to_rect(clipped, pass.clip_x1, pass.clip_y1,
					pass.clip_x2, pass.clip_y2))
				out.push_back(clipped);
		}
	}
}

namespace {

// The same frame-state feed the spinmap element reads, copy-assigned into the
// persistent input (the markers vector keeps its capacity).
void map_window_input(const HudFrameState &state, float surface_w, float surface_h,
		HudMinimapInput &input) {
	input = state.minimap;
	input.footprints = &state.map_footprints;
	input.surface_w = surface_w;
	input.surface_h = surface_h;
	input.ticks = state.ticks;
	// The windowed passes run the same legs as the big map (the CMAP mask is
	// the big-map mask), so they read the same non-bank feed, flash timers,
	// FARP target and colours.
	input.item_flash = state.item_flash;
	input.farp_present = state.combat.farp_present;
	input.farp_x = state.combat.farp_x_q16;
	input.farp_y = state.combat.farp_y_q16;
	input.overlays = &state.map_overlays;
}

} // namespace

const HudFrameCompiler::MapWindowDraw &HudFrameCompiler::compile_death_map(
		const HudFrameState &state, const DeathMapFrame &frame, const DeathMapFacts &facts,
		float surface_w, float surface_h) {
	// The zone letters and score lines lay out in the bold label slot like the
	// other map labels [orig: MapOverlay_DrawView — HUD_DrawTextCentered_HalfBright(
	// g_HUDLabelFontBold) @0x5a5c59/@0x5a5cbf/@0x5a5d14].
	map_window_input(state, surface_w, surface_h, map_window_input_);
	map_window_input_.hudpos_text_color = layout_.hud_text;
	map_window_input_.overlay_color = active_color(state);
	death_map_draw_.glyphs.clear();
	death_map_draw_.zone_glyphs.clear();
	death_map_draw_.zone_glyph_ends.clear();
	death_map_compiler_.compile(map_window_input_, frame, facts, death_map_draw_.pass);
	if (death_map_draw_.pass.map.visible) {
		layout_map_labels(death_map_draw_.pass.map, death_map_draw_.glyphs);
		// Each zone's letters lay out on their own so the device can draw them
		// right after that zone's blip [orig: MapOverlay_DrawView — the blip
		// @0x5a5adc, then the letters @0x5a5c59..0x5a5d14, per zone].
		size_t label_begin = 0;
		for (const HudMapWindowSegment &segment : death_map_draw_.pass.segments) {
			layout_map_labels(death_map_draw_.pass.zones, label_begin, segment.label_end,
					death_map_draw_.zone_glyphs);
			death_map_draw_.zone_glyph_ends.push_back(death_map_draw_.zone_glyphs.size());
			label_begin = segment.label_end;
		}
	}
	return death_map_draw_;
}

const HudFrameCompiler::MapWindowDraw &HudFrameCompiler::compile_command_map(
		const HudFrameState &state, CommandMapView &cmap, const MapViewRect &rect,
		int32_t scaled_800, const DeathMapFacts &facts, float surface_w, float surface_h) {
	map_window_input(state, surface_w, surface_h, map_window_input_);
	map_window_input_.hudpos_text_color = layout_.hud_text;
	map_window_input_.overlay_color = active_color(state);
	command_map_draw_.glyphs.clear();
	command_map_compiler_.compile(map_window_input_, cmap, rect, scaled_800, facts,
			command_map_draw_.pass, command_map_draw_.waypoint_anchors.data());
	if (command_map_draw_.pass.map.visible)
		layout_map_labels(command_map_draw_.pass.map, command_map_draw_.glyphs);
	// The hover box's extent: the name measured in the bold label slot
	// [orig: CMapWindow_HandleEvent @0x549e15 — HUD_MeasureTextWH(entity +244,
	//  &g_HUDLabelFont[1], ..)].
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bold = have_bold ? label_font_bold_ : font_;
	const float bold_scale = have_bold ? label_scale_ : hud_font_scale_;
	for (size_t i = 0; i < command_map_draw_.waypoint_anchors.size(); ++i) {
		CommandMapWaypointAnchor &anchor = command_map_draw_.waypoint_anchors[i];
		if (!anchor.live) continue;
		int text_w = 0, text_h = 0;
		bold.measure(facts.user_waypoints[i].name.c_str(), bold_scale, bold_scale, &text_w,
				&text_h);
		anchor.text_w = text_w;
		anchor.text_h = text_h;
	}
	return command_map_draw_;
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
		// The row's mode resolves in the 1024x768 design space first — plain,
		// scaled about its centre by the sight-scale index, or slid by the
		// scope-zero multiplier (sight_overlay.h sight_row_rect carries the
		// witness) — then the corners scale per draw
		// [orig: Viewport_ScaleToVirtualCoords @ 0x5d2b20].
		SightRowSpec spec;
		spec.x1 = static_cast<int32_t>(r.x0);
		spec.y1 = static_cast<int32_t>(r.y0);
		spec.x2 = static_cast<int32_t>(r.x1);
		spec.y2 = static_cast<int32_t>(r.y1);
		spec.scale = r.scale;
		spec.slide = r.slide;
		spec.slide_frames = r.slide_frames;
		const SightRect rect = sight_row_rect(spec, state.sight_scale_index,
				state.sight_slide_multiplier);
		const SightViewportRect screen = sight_rect_to_viewport(rect, w, h, state.aspect_mode);
		emit_rect(screen.x1, screen.y1, screen.x2, screen.y2, 0xFFFFFFFFu, true,
				kHudTexSightsBase + static_cast<int32_t>(row), r.additive);
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_frame(float w, float h) {
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
	// The DMGBAR declutter gate [orig: the slot-7 cmp @ 0x5A7C99].
	if (!state.declutter_visible[kDeclutterDmgBar]) {
		return;
	}
	const HudRectRecord &r = layout_.health_rect;
	if (!r.present || (r.x == 0.0f && r.y == 0.0f && r.w == 0.0f && r.h == 0.0f)) {
		return;
	}
	const float x0 = sx(r.x, w);
	const float y0 = sy(r.y, h);
	const float x1 = sx(r.x + r.w, w);
	const float y1 = sy(r.y + r.h, h);
	// On the death screen the bar draws only inside the spectate arm, off the
	// info rebuilt for the target [orig: HUD_RenderOverlays @0x5a7bc5..0x5a7c04;
	// the living arm @0x5a7ca0].
	if (state.combat.death_screen && !state.session.spectating) return;
	const float fraction = std::clamp(state.combat.death_screen
					? state.session.spectated_health_fraction
					: state.health_fraction,
			0.0f, 1.0f);
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
    if (!hud_stance_group_visible(state)) return;
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
	// The WPNGRP declutter gate covers the ammo count, the weapon name, and
	// the clip indicator [orig: the slot-8 cmps @ 0x5A7CC8 / @ 0x5A7D04 /
	// @ 0x5A7D42]; the crosshair rides its OWN XHAIRS slot inside
	// element_crosshair.
	const bool wpngrp_visible = hud_weapon_group_visible(state);
	const uint32_t wc = half_bright_argb(layout_.weapon_text);
	char ammo[64];
	const std::string ammo_text = format_ammo(state.weapon.clip,
			state.weapon.reserve, state.weapon.capacity);
	(void)ammo;
	if (wpngrp_visible && !ammo_text.empty() && layout_.ammo_count.hidden == 0) {
		const uint32_t align_flags = layout_.ammo_count.align == 1
				? kFontAlignRight
				: (layout_.ammo_count.align == 2 ? kFontAlignCenter : 0u);
		emit_text(ammo_text.c_str(),
				static_cast<float>(layout_.ammo_count.x),
				static_cast<float>(layout_.ammo_count.y), w, h, wc,
				align_flags);
	}
	if (wpngrp_visible && !state.weapon.display_name.empty() &&
			layout_.weapon_name.hidden == 0) {
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
	if (wpngrp_visible) {
		element_clip_indicator(state, w, h);
	}
	element_targeting(state, w, h);
	element_crosshair(state, w, h);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_clip_indicator(const HudFrameState &state,
		float w, float h) {
	// [orig: HUD_DrawAmmoIndicator @ 0x599a30 — anchor/ramp/-1 gates, the
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
	// The restamp key (D-HUD-5): any change of the def's ammo bucket, the
	// HUD reserve or the LOW BYTE of the def's ammo-class id restamps; two
	// defs agreeing on all three do not [orig: @0x599A90 `mov edi,[ecx+0DCh]`,
	// @0x599A96 vs dword_2723D40, @0x599A9E the reserve vs dword_2723D44,
	// @0x599AAC `cmp dl,[ecx+0D8h]` vs byte_2723D4C; the stores
	// @0x599AB4..0x599ACA]. HUD_BuildEntityInfo has already folded
	// capacity-one ammo into the reserve, so a reload that keeps the displayed
	// total never restamps [orig: @0x4B85EF].
	if (wep.ammo_bucket != flash_key_bucket_ || wep.reserve != flash_key_reserve_ ||
			wep.ammo_class_id != flash_key_class_) {
		flash_key_bucket_ = wep.ammo_bucket;
		flash_key_reserve_ = wep.reserve;
		flash_key_class_ = wep.ammo_class_id;
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
	// The XHAIRS declutter gate covers the whole crosshair complex, AND'd
	// with the binocular suppression [orig: the slot-13 cmp @ 0x592757].
	if (!state.declutter_visible[kDeclutterXhairs]) {
		return;
	}
	if (state.binoculars_view_active || state.combat.death_screen || state.combat.custom_aim) {
		return;
	}
	if (!crosshair_should_draw(state.aimed_shot_available,
				state.keep_crosshair_while_aimed)) {
		return;
	}
	if (!(state.combat.driver_crosshair ? layout_.combat.driver_crosshair.valid
										: layout_.crosshair_texture_valid)) {
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
	// Spread off zeroes the offset and still draws all five arms
	// [orig: the g_cfgCrossHairSpread arm @ 0x592b82; the disabled fldz
	// @ 0x592bcc].
	const float spread = layout_.crosshair_spread_enabled && !state.combat.driver_crosshair
			? static_cast<float>(crosshair_spread_px_fp16(
					state.hud_spread_fp16, state.fov_deg, w))
			: 0.0f;

    const int half_w = (state.combat.driver_crosshair ? layout_.combat.driver_crosshair.width
													  : layout_.crosshair_tex_w) /
			2;
	const int half_h = (state.combat.driver_crosshair ? layout_.combat.driver_crosshair.height
													  : layout_.crosshair_tex_h) /
			2;
	constexpr float uv[5][5][2] = { { { 0, 0 }, { .45f, .45f }, { .5f, 0 }, { .55f, .45f },
											{ 1, 0 } },
		{ { 0, 1 }, { .45f, .55f }, { .5f, 1 }, { .55f, .55f }, { 1, 1 } },
		{ { 0, 0 }, { .45f, .45f }, { 0, .5f }, { .45f, .55f }, { 0, 1 } },
		{ { 1, 0 }, { .55f, .45f }, { 1, .5f }, { .55f, .55f }, { 1, 1 } },
		{ { .55f, .45f }, { .45f, .45f }, { .55f, .55f }, { .45f, .55f }, { 0, 0 } } };
	const float offsets[5][2] = {
		{0.0f, -spread}, {0.0f, spread}, {-spread, 0.0f}, {spread, 0.0f},
		{0.0f, 0.0f},
	};
	for (int corner = 0; corner < 5; ++corner) {
		const float ox = cx + offsets[corner][0];
		const float oy = cy + offsets[corner][1];
		// Snap the outer corners first; the original integer midpoints and
		// half-extents then produce fractional tapered vertices.
		// [orig: @0x590FCC..0x591067]
		const float l = sx(float(int(ox) - half_w), w), r = sx(float(int(ox) + half_w), w);
		const float t = sy(float(int(oy) - half_h), h), b = sy(float(int(oy) + half_h), h);
		const float mx = float((int(l) + int(r)) >> 1), my = float((int(t) + int(b)) >> 1);
		const float tx = kCrosshairTaper * float((int(r) - int(l)) >> 1);
		const float ty = kCrosshairTaper * float((int(b) - int(t)) >> 1);
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
				out[k]->x = strip[idx[k]][0];
				out[k]->y = strip[idx[k]][1];
				out[k]->u = uv[corner][idx[k]][0];
				out[k]->v = uv[corner][idx[k]][1];
			}
			// FVF 0x2C4: XYZRHW, diffuse at +16, specular at +20, two UVs.
			// The decompiler mislabeled RHW as diffuse and diffuse as specular.
			// Color is diffuse; specular is zero. [orig: @0x5914CC..0x591500;
			// GDynamicVB_DrawPrimitive SetFVF @0x678962 / @0x678A3E]
			tri.color = state.combat.driver_crosshair ? 0xFF007F00u
					: state.combat.hit_feedback		  ? 0xFFFF5050u
													  : layout_.crosshair_color;
			tri.texture = state.combat.driver_crosshair ? kHudTexDriverCrosshair : kHudTexCrosshair;
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
	// The PWRBAR declutter gate [orig: the slot-20 cmp @ 0x5A7DD2].
	if (!state.declutter_visible[kDeclutterPwrBar]) {
		return;
	}
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

void HudFrameCompiler::emit_progress_bar(int xl, int yt, int xr, int yb, uint32_t fill,
		uint32_t border, float fraction, bool centered) {
	// [orig: HUD_DrawProgressBar @0x59B340 — three untextured quads in one
	//  12-vertex draw (CEffect_GetPassDesc_Validated(..., 0x700000) @0x59B5EB):
	//  the border, the inner rect in opaque black (the float -1.7014118e38 =
	//  0xFF000000 store @0x59B4DB), then the fill; centred, the fill spans
	//  mid +- (xr - xl - 4) * fraction * 0.5 about mid = (xl + xr) / 2
	//  @0x59B520..0x59B56B, else xl + 2 .. xl + 2 + (xr - xl - 4) * fraction]
	emit_rect(static_cast<float>(xl), static_cast<float>(yt), static_cast<float>(xr),
			static_cast<float>(yb), border, true);
	emit_rect(static_cast<float>(xl + 1), static_cast<float>(yt + 1), static_cast<float>(xr - 1),
			static_cast<float>(yb - 1), 0xFF000000u, true);
	const float span = static_cast<float>(xr - xl - 4) * fraction;
	float x0 = static_cast<float>(xl + 2);
	float x1 = x0 + span;
	if (centered) {
		const float mid = static_cast<float>((xl + xr) / 2);
		x0 = mid - span * 0.5f;
		x1 = mid + span * 0.5f;
	}
	emit_rect(x0, static_cast<float>(yt + 2), x1, static_cast<float>(yb - 2), fill, true);
}

void HudFrameCompiler::element_breath_bar(const HudFrameState &state, float w, float h) {
	// [orig: HUD_DrawBreathBar @0x59D6F0..0x59D9C9, whose only caller is
	//  HUD_DrawGameplayOverlays @0x5BDED3, behind g_SpawnSuccessGate == 0
	//  @0x5BDECA..0x5BDED1]
	if (state.spawn_success_gate) {
		return;
	}
	// The BREATHTIME declutter slot, then a positive breathtime
	// [orig: @0x59D6F3, @0x59D70F]. The count forced to 1 under
	// dword_24C1930 & 0x8000000 is a dead arm: nothing sets that bit.
	if (!state.declutter_visible[kDeclutterBreathTime] || state.breath_time <= 0) {
		return;
	}
	const int count = state.breath_samples;
	if (count == 0) {
		return; // [orig: @0x59D742]
	}
	// Four samples a second of breathtime; red over the last 40 (10 s).
	const int limit = 4 * state.breath_time;
	const uint32_t color = count > limit - 40 ? 0xFFFF0000u : 0xFF00FF00u;
	const HudPosRecord &pos = layout_.breath_time;
	// The bar is skipped once the integer `100 - 100 * count / limit` is not
	// positive; the label still draws [orig: @0x59D763]. It is 200 x 10
	// design px from the anchor, left (align 0), right (1) or centred (2),
	// each corner scaled on its own [orig: @0x59D794..0x59D817,
	// @0x59D90E..0x59D991, @0x59D851..0x59D8D4 through
	// Viewport_ScaleToVirtualCoords], filled by 1 - count / limit in double
	// [orig: @0x59D80A / @0x59D8C7 / @0x59D984].
	if (100 - 100 * count / limit > 0) {
		const int left = pos.align == 1 ? pos.x - 200 : (pos.align == 2 ? pos.x - 100 : pos.x);
		const double fraction = 1.0 - static_cast<double>(count) / static_cast<double>(limit);
		emit_progress_bar(static_cast<int>(sx(static_cast<float>(left), w)),
				static_cast<int>(sy(static_cast<float>(pos.y), h)),
				static_cast<int>(sx(static_cast<float>(left + 200), w)),
				static_cast<int>(sy(static_cast<float>(pos.y + 10), h)), color, color,
				static_cast<float>(fraction), true);
	}
	// The label (Overlays/STROVER91) 15 design px below the anchor in the BOLD
	// slot, aligned like the bar, half-bright [orig: the slot push @0x59D8F7;
	// HUD_DrawTextLeftScaled @0x59D83F / RightAlignedScaled @0x59D9B9 /
	// CenteredScaled @0x59D8FC, all drawing through HUD_DrawTextLeft_HalfBright
	// @0x5804C0's (color >> 1) & 0x7F7F7F | 0xFF000000].
	const uint32_t flags =
			pos.align == 1 ? kFontAlignRight : (pos.align == 2 ? kFontAlignCenter : 0u);
	emit_slot_text(label_font_bold_, label_scale_, state.breath_label.c_str(),
			sx(static_cast<float>(pos.x), w), sy(static_cast<float>(pos.y + 15), h),
			half_bright_argb(color), flags);
	++draw_list_.elements_drawn;
}

uint32_t HudFrameCompiler::active_color(const HudFrameState &state) const {
	// The hud_color_index scheme table + the derived master overlay color.
	// [orig: HUD_InitTeamColorTable @0x51f240 — the 16-dword g_HUDColors
	// @0x24C1838 immediates (entries 0..5 are the cycled schemes); per frame
	// HUD_RenderAllOverlays @0x5a8100-0x5a8125 refreshes table[2] from the
	// hudpos hud_textcolor (g_HudposTextColor) and restamps the frame overlay
	// color (g_HUDFrameOverlayColor @0x840B1C) = table[index]; the snapshot
	// twin g_HUDColors.active @0x24C1868 = table[index] | 0xFF000000 at init and
	// table[index] at the cycle @0x49afc7. Both twins carry the same value for
	// every authored scheme (all entries ship alpha FF); the compiler derives
	// ONE per-frame color and forces the init path's FF alpha. The snapshot
	// twin's own writers are modelled for the readers that name it
	// (hud_colors_active_).]
	const int idx = state.hud_color_index >= 0 && state.hud_color_index <= 5
			? state.hud_color_index
			: 2;
	return hud_palette(idx) | 0xFF000000u;
}

uint32_t HudFrameCompiler::hud_palette(int index) const {
	// [orig: HUD_InitTeamColorTable @0x51f240 — the scheme immediates; entry 2
	// is refreshed from g_HudposTextColor every frame @0x5a810c / @0x59ccb8]
	static constexpr uint32_t kSchemeTable[6] = {
			0xFFFFFFFFu, // 0 white
			0xFF00FF00u, // 1 green
			0xFF010101u, // 2 init placeholder — sourced live from hud_textcolor
			0xFF80A0FFu, // 3 light blue
			0xFFF0F000u, // 4 yellow
			0xFFFF5050u, // 5 salmon
	};
	if (index < 0 || index > 5) index = 2;
	return index == 2 ? layout_.hud_text : kSchemeTable[index];
}

void HudFrameCompiler::element_waypoint(const HudFrameState &state, float w,
		float h) {
	// [orig: HUD_DrawWaypointNameAndDistance @ 0x5947a0 — align routing, the
	// wireframe distance box (field 3 hides only the box)]
	// The WAYPOINT declutter gate [orig: the slot-3 cmp @ 0x5A7DB8].
	if (!state.declutter_visible[kDeclutterWaypoint]) {
		return;
	}
	// The dispatcher's remaining gates (g_ShowWaypoints rides `present`):
	// never under game type 0x10010, and on the death screen only in the
	// non-spectate arm [orig: `g_GameType != 65552` @0x5A7DCB / @0x5A7C45; the
	// spectate arm @0x5A7BDB..0x5A7C25 draws the health bar and TEAMID line].
	if (state.session.game_type == 0x10010u ||
			(state.combat.death_screen && state.session.spectating)) {
		return;
	}
	// Every measure and draw here goes through the BOLD label slot
	// [orig: HUD_DrawWaypointNameAndDistance @0x5947a0 passes &g_HUDLabelFont[1]
	// (0xB4C394) to GameFont_MeasureTextWidth @0x580A50, HUD_MeasureTextWH
	// @0x580AB0 and every HUD_DrawText* call]; an absent bold file falls back to
	// the HUD slot, as emit_slot_text does.
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &wf = have_bold ? label_font_bold_ : font_;
	const float ws = have_bold ? label_scale_ : hud_font_scale_;
	if (!state.waypoint.present || wf.font() == nullptr) {
		return;
	}
	const HudPosRecord &gp = layout_.wpd_info;
	if (!gp.present || (gp.x == 0 && gp.y == 0 && gp.hidden == 0 && gp.align == 0)) {
		return;
	}
	char dist[16];
	std::snprintf(dist, sizeof(dist), "%d", state.waypoint.distance_m);
	// The frame overlay color [orig: the g_HUDFrameOverlayColor reads
	// @0x5949dd..0x594c9d in the waypoint pair].
	const uint32_t color = active_color(state);
	const float ax = static_cast<float>(gp.x);
	const float ay = static_cast<float>(gp.y);
	// Measures in font pixels, folded to design via the surface ratio.
	auto measure_design_w = [&](const char *text) {
		int mw = 0;
		int mh = 0;
		wf.measure(text, ws, ws, &mw, &mh);
		return static_cast<float>(mw) * kDesignW / std::max(w, 1.0f);
	};
	auto draw = [&](const char *text, float design_x, uint32_t flags) {
		emit_slot_text(label_font_bold_, label_scale_, text, sx(design_x, w), sy(ay, h), color,
				flags);
	};
	const float dist_w = measure_design_w(dist);
	const float text_h = wf.line_height(ws) * kDesignH / std::max(h, 1.0f);
	float dist_x = ax;
	float box_left = ax;
	float box_right = ax + dist_w + 4.0f;
	if (!state.waypoint.name.empty()) {
		switch (gp.align) {
			case 1: {
				draw(state.waypoint.name.c_str(), ax, kFontAlignRight);
				const float name_w = measure_design_w(state.waypoint.name.c_str());
				dist_x = ax - 4.0f - name_w;
				box_left = dist_x - dist_w;
				box_right = dist_x + 4.0f;
				break;
			}
			case 2:
				draw(state.waypoint.name.c_str(), ax, 0u);
				dist_x = ax - 4.0f;
				box_left = dist_x - dist_w;
				box_right = dist_x + 4.0f;
				break;
			default:
				draw(state.waypoint.name.c_str(), ax + dist_w + 4.0f, 0u);
				break;
		}
	}
	if (gp.hidden == 0) {
		emit_wire_rect(sx(box_left, w), sy(ay - 2.0f, h), sx(box_right, w),
				sy(ay + text_h - 1.0f, h), color);
	}
	draw(dist, dist_x, kFontAlignRight);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_attach_labels(const HudFrameState &state) {
	// [orig: HUD_DrawVehicleSeatAndArmoryLabels @ 0x5a3290 — nearest at the
	// full color, others ((rgb & 0xFEFEFE) | 0xFE000001) >> 1 @ 0x5a364e]
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
		// The snapshot overlay color [orig: g_HUDColors.active reads
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

uint32_t friendly_tag_squad_color(int band, bool has_slot, uint8_t squad_index,
		uint32_t tier_rgb) {
	// [orig: HUD_DrawEntityLabel — the slot's +0x33 byte, `test al,al; jbe`
	//  @0x5A3CCA / @0x5A3D04; the table read g_SquadColors[idx] @0x5A3CD5 /
	//  @0x5A3D0D]. Band 0 is retail's classifier value 2 (good), band 1 its 1.
	if (!has_slot || squad_index == 0 || band > 1 || squad_index >= kHudSquadColors.size())
		return tier_rgb;
	const uint32_t squad = kHudSquadColors[squad_index];
	if (band == 0) return squad;
	// Each channel times 0.7 (dbl_7D9DE8) through fistp under the chop control
	// word, the byte stored back; the alpha byte stays the table's
	// [orig: @0x5A3D18..0x5A3DBC].
	const auto chop = [](uint32_t channel) {
		return static_cast<uint32_t>(static_cast<int32_t>(static_cast<double>(channel) * 0.7)) &
				0xFFu;
	};
	return (squad & 0xFF000000u) | (chop((squad >> 16) & 0xFFu) << 16) |
			(chop((squad >> 8) & 0xFFu) << 8) | chop(squad & 0xFFu);
}

void HudFrameCompiler::element_friendly_tags(const HudFrameState &state) {
	// D-HUD-20 [orig: HUD_DrawEntityLabel @ 0x5a39b0]. Screen-pixel anchors
	// like the attach labels — the presenter projects, the compiler draws.
	// Friendly tags draw with the NORMAL Arial label font at the slot scale
	// [orig: @0x5a3a0c loads g_HUDLabelFont @0xB4C388 (slot +0); the spectated
	// leg @0x5a3a29 loads g_HUDLabelFontLarge @0xB4C3A0 (slot +0x18, Impac22b)
	// and rides the unported death screen].
	const bool have_label = label_font_.font() != nullptr;
	const GameFont &lf = have_label ? label_font_ : font_;
	const float ls = have_label ? label_scale_ : hud_font_scale_;
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
		// cull [orig: dist <= g_EnvFogDistCurrent @ 0x5a3b28].
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
		// The squad colour override of the good and middle tiers.
		rgb = friendly_tag_squad_color(band, tag.has_slot, tag.squad_color_index, rgb);
		// The bad tier's DOWNED legs: a dead entity with a slot still inside
		// its revive window is light blue (table[3]), pulsing toward white
		// while a medic request stands; dead without that is gray (table[8]);
		// alive-but-bad keeps tagcolor_bad [orig: @0x5a3dc9..0x5a3e85 —
		// `Flags & 2` -> slot && slot+0x10 ? (slot+0x2C ? pulse : light blue)
		// : gray, else tagcolor_bad].
		if (band == 2 && tag.dead) {
			if (tag.has_slot && tag.revive_seconds != 0) {
				rgb = tag.medic_request
						? friendly_tag_revive_pulse(kFriendlyTagDownedLightBlue,
								  state.ticks)
						: kFriendlyTagDownedLightBlue;
			} else {
				rgb = kFriendlyTagDownedGray;
			}
		}
		// The speaking pulse rides the voice output level [orig: @ 0x5a3e8f].
		if (tag.speaking) {
			rgb = friendly_tag_speaking_blend(rgb, state.speaking_level255);
		}
		// Distance alpha 255 -> 63 over 50..300 m [orig: @ 0x5a3eeb..0x5a3f18].
		const uint32_t argb =
				(static_cast<uint32_t>(friendly_tag_alpha(tag.dist_q16)) << 24) |
				(rgb & 0xFFFFFFu);
		const float top_y = tag.screen_y - font_h * 0.5f; // [orig: @ 0x5a4264]
		// The revive count rides the label while the entity is dead with a
		// slot inside its window [orig: the gate `dead && slot && slot+0x10`
		// @0x5a3fdc..0x5a3ff8 (text), @0x5a41c0..0x5a41df (ticks),
		// @0x5a4407..0x5a441b (bar)].
		const bool show_count = tag.dead && tag.has_slot && tag.revive_seconds != 0;
		// The tick and bar forms draw the bare count centered one fontH ABOVE
		// the projected point in the tag color, at the x the icon arm below
		// may have shifted [orig: sprintf("%ld") @0x5a41f0 / @0x5a4428 ->
		// HUD_DrawTextHalfBrightF(x, y - fontH) @0x5a4453].
		auto emit_bare_count = [&](float x) {
			if (!show_count) return;
			const std::string count = std::to_string(tag.revive_seconds);
			const GameFontRun run = lf.layout(count.c_str(), x,
					tag.screen_y - font_h, ls, ls, kFontAlignCenter,
					half_bright_keep_alpha(argb));
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
		};
		// The radio-request icon arm, gated on the tag's +885 fold AND the
		// viewer gate [orig: HUD_DrawEntityLabel `test ebp, ebp; jz` +
		// `cmp var_DC, 0; jz` @0x5a415a..0x5a4163 (tick), @0x5a4270..0x5a427d
		// (text), @0x5a43a0..0x5a43a9 (bar)]: the TSDicon cell 0x17 in table[3]
		// light blue at forced full alpha, half-size fontH*0.5 centered on
		// (cx, cy) [orig: HUD_DrawRotatedIconQuad @0x599630]. The strip
		// renderer's MODULATE2X stage folds into the diffuse as for the map
		// blips [orig: Render_DrawIconStripCell_Debug @0x67bae0].
		const bool request_icon = tag.radio_request && state.radio_request_icon_viewer;
		const float half_h = font_h * 0.5f;
		auto emit_request_icon = [&](float cx, float cy) {
			HudQuad icon;
			icon.x0 = cx - half_h;
			icon.y0 = cy - half_h;
			icon.x1 = cx + half_h;
			icon.y1 = cy + half_h;
			hud_icon_strip_cell_uv(state.minimap, kFriendlyTagRadioRequestIcon,
					icon.u0, icon.v0, icon.u1, icon.v1);
			icon.color = hud_icon_strip_modulate2x_color(
					0xFF000000u | (kFriendlyTagDownedLightBlue & 0xFFFFFFu));
			icon.texture = kHudTexMapIcons;
			draw_list_.quads.push_back(icon);
		};
		// The tick/bar forms: a medic tag pre-shifts x by -fontH/2, the icon
		// centers at (x - fontH/2, y - fontH/2), then a medic tag shifts x by
		// +fontH for the count [orig: @0x5a4165..0x5a41bc (tick) /
		// @0x5a43ab..0x5a4403 (bar): `cdq; sub; sar 1; neg; add axis`,
		// `fisubr axis` / `fild cosVal; fsub` at the icon, `add axis, ebx`].
		auto tick_form_icon_x = [&](float x) {
			if (!request_icon) return x;
			if (tag.medic) x -= half_h;
			emit_request_icon(x - half_h, tag.screen_y - half_h);
			if (tag.medic) x += font_h;
			return x;
		};

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
				emit_bare_count(tick_form_icon_x(tag.screen_x));
				continue;
			}
			// '^' + the compiled-in name table [orig: @ 0x5a4047..0x5a40cd].
			resolved = friendly_tag_fallback_name(tag.entity_id);
		}

		if (friendly_tag_text_visible(state.friendly_tag_mode, tag.dist_q16)) {
			// The text form appends the count to the name
			// [orig: sprintf("%s: %ld", name, slot+0x10) @0x5a400e, else
			//  sprintf("%s", name) @0x5a422e].
			const std::string label = show_count
					? resolved + ": " + std::to_string(tag.revive_seconds)
					: resolved;
			// Centered text at the projected point, half-bright with the
			// distance alpha kept [orig: HUD_DrawTextHalfBrightF @ 0x5a4268 ->
			// CGameFont_DrawText flags 1, the slot scales pushed @ 0x580720].
			const GameFontRun run = lf.layout(label.c_str(), tag.screen_x,
					top_y, ls, ls, kFontAlignCenter,
					half_bright_keep_alpha(argb));
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
			int text_w = 0;
			int text_h = 0;
			if (request_icon || tag.medic)
				lf.measure(label.c_str(), ls, ls, &text_w, &text_h);
			// The text form's icon: x shifts by -(fontH/2 + textW/2) and STAYS
			// shifted for the plate below; the icon centers at (x - fontH/2,
			// top_y) [orig: @0x5a4288..0x5a42ee -- `sar 1; neg; sar ecx, 1;
			//  sub; add axis` @0x5a429b..0x5a42aa, `fild cosVal; fsub; fiadd
			//  fontH/2` @0x5a42cb..0x5a42da, `fisubr axis` @0x5a42e2].
			float plate_x = tag.screen_x;
			if (request_icon) {
				plate_x += -half_h - static_cast<float>(text_w) * 0.5f;
				emit_request_icon(plate_x - half_h, top_y);
			}
			if (tag.medic) {
				// The red-cross-on-white medic plate, a fontH/2 square left of
				// the text at the tag alpha, measured from the (possibly
				// icon-shifted) x [orig: rect @ 0x5a4309..0x5a436c reading
				// `axis` @0x5a430e -> HUD_DrawMedicCrossQuad @ 0x59bcb0]. The
				// quad itself is the shared primitive in hud/hud_medic_cross.h
				// (the map medic marker and the help icons draw the same
				// routine): the white field, then the two red bars, in the
				// witnessed order.
				const float x0 = plate_x -
						(static_cast<float>(text_w) * 0.5f + font_h) - 0.5f;
				const float y0 = top_y - 0.5f;
				const float x1 = x0 + font_h * 0.5f;
				const float y1 = y0 + font_h * 0.5f;
				for (const MedicCrossQuad &q : medic_cross_quads(x0, y0, x1, y1,
							 static_cast<int>(argb >> 24)))
					emit_rect(q.x0, q.y0, q.x1, q.y1, q.color, true);
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
			emit_bare_count(tick_form_icon_x(tag.screen_x));
		}
	}
	++draw_list_.elements_drawn;
}

// The panel pass draws this after its other windows, independently of gameplay declutter.
// [orig: HUD_DrawOverlayPanels @ 0x5C0060; HUD_DrawKillAnnounceBanner @ 0x59DC90]
void HudFrameCompiler::element_kill_announcement(const HudFrameState &state, float w, float h) {
	if (!state.kill_announcement.visible(static_cast<uint32_t>(state.ticks))) return;
	const bool large = label_font_large_.font() != nullptr;
	const GameFont &font = large ? label_font_large_ : font_;
	if (font.font() == nullptr) return;
	const float scale = large ? label_large_scale_ : hud_font_scale_;
	const auto run = font.layout(state.kill_announcement.text.c_str(), sx(512.0f, w),
			sy(30.0f, h), scale, scale, kFontAlignCenter, 0xFFFFFFFFu);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_end_round_overlay(const HudFrameState &state,
		float w, float h) {
	// [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0] The stdbox over the
	// overlay safe area, then each resolved line centred on design x 512 in
	// the Impact38 slot, half-bright like every HUD text
	// [orig: HUD_DrawLabelBox(ctx, 8, top+8, 1015, bottom-8) @0x5b7d3e;
	//  HUD_DrawTextCenteredScaled (ex sub_580B80) -> Viewport_ScaleToVirtualCoords + HUD_DrawTextCentered_HalfBright].
	const HudEndRoundOverlayState &er = state.end_round;
	if (!er.shown) return;
	emit_stdbox(sx(8.0f, w), sy(static_cast<float>(er.top + 8), h),
			sx(1015.0f, w), sy(static_cast<float>(er.bottom - 8), h), w,
			0xFFFFFFFFu, 0.0f);
	// The Impact38 slot falls back to the large slot, then the bold label
	// slot, then the hudpos font at scale 1 when the files are absent
	// (layout-only embedders keep drawing, like the other label elements).
	const bool have_impact = label_font_impact38_.font() != nullptr;
	const bool have_large = label_font_large_.font() != nullptr;
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &lf = have_impact ? label_font_impact38_
			: have_large ? label_font_large_
			: have_bold ? label_font_bold_ : font_;
	const float ls = (have_impact || have_large) ? label_large_scale_
			: have_bold ? label_scale_ : 1.0f;
	if (lf.font() == nullptr) return;
	const uint32_t color = half_bright_keep_alpha(active_color(state));
	for (const HudEndRoundLine &line : er.lines) {
		if (line.text.empty()) continue;
		const GameFontRun run = lf.layout(line.text.c_str(), sx(512.0f, w),
				sy(static_cast<float>(line.y), h), ls, ls, kFontAlignCenter, color);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
	}
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_feed(const HudFrameState &state, float w,
		float h) {
	// THE SYSTEM MESSAGE FEED — the one ring every Chat_AddMessageChannel2 line
	// lands in: the 0x1E kill/objective/medic lines AND mission triggered
	// text [orig: HUD_DisplayTriggeredText @0x51f190 posts @0x51f216 into the
	// same sink]. Drawn by the second HUD_DrawConsoleMessages loop
	// [orig: @0x59ad30; the system walk @0x59ae5e..0x59aebf]. (The FIRST loop
	// is the player-chat ring at HUDCHATTEXT — fed by the S2C 0x14 channel,
	// ported with the chat ring in #555; D-HUD-6/D-NET-215 are FIXED.)
	// Witnessed properties:
	//   * only THREE ring rows are walked per channel, and the walk DESCENDS
	//     from the top slot to slot 0 (msgEntry -= 0x80 down to the base
	//     [orig: @0x59aebf..0x59aecb]); with the sink putting the NEWEST line
	//     in row 0, the OLDEST of the three lands AT the anchor and each newer
	//     line is 18 design px BELOW it [orig: the 0x12 step @0x59ad97, scaled
	//     through Viewport_ScaleToVirtualCoords] — the feed grows downward.
	//     Only drawn rows consume a rung (the skip path bypasses the y step
	//     [orig: @0x59aeb7 runs only on the >0-alpha arm]). Expiry staggering
	//     keeps recency and expiry in the same order, so "the newest three
	//     slots, live only" equals the last three live lines here.
	//   * the stored per-line color is drawn AS STORED [orig: the color read
	//     @0x59ae97]; the computed `timer * 255 / 186` alpha fold belongs to
	//     the CHAT loop alone [orig: @0x59adef], so this ring does not fade.
	//   * the anchor is HUDSYSTEXT, not HUDCHATTEXT.
	// The declutter mask and the hard level cull gate BOTH loops at one site
	// [orig: the slot bit @0x59AD33, level >= 2 @0x59AD43]. The ring itself
	// keeps aging — only the draw is skipped.
	if (!state.declutter_visible[kDeclutterChat] ||
			state.hud_detail_level >= 2) {
		return;
	}
	if (font_.font() == nullptr) {
		return;
	}
	const float row_h = static_cast<float>(kFeedLineStepDesign);
	bool drew = false;

	// THE CHAT RING — the FIRST loop [orig: HUD_DrawConsoleMessages
	// @0x59ad30, the chat walk @0x59adbc..0x59ae2e]: anchored at HUDCHATTEXT
	// (dword_27237A8/AC), slots 3..1 walked top-down, each line's alpha the
	// fold `255 * timer / 186` clamped to 255 and SKIPPED (no rung consumed)
	// at <= 0 [orig: @0x59add9..0x59adf4], folded over the stored RGB
	// `(alpha << 24) + (color & 0xFFFFFF)` @0x59ae04, drawn with the bold
	// label font. The timer is the slot's remaining life, so the alpha ramps
	// down through the line's last 186 ticks.
	{
		const float cx = static_cast<float>(
				layout_.chat_text.present ? layout_.chat_text.x : 5);
		const float cy = static_cast<float>(
				layout_.chat_text.present ? layout_.chat_text.y : 5);
		const int n = static_cast<int>(chat_lines_.size());
		const int first = std::max(0, n - kFeedVisibleRows);
		float row_y = cy;
		for (int i = first; i < n; ++i) {
			const HudMessageLine &line = chat_lines_[static_cast<size_t>(i)];
			const int remaining = line.expire_tick - state.ticks;
			int alpha = 255 * remaining / kMessageExpiryStagger;
			if (alpha <= 0) continue;
			if (alpha > 255) alpha = 255;
			emit_text(line.text.c_str(), cx, row_y, w, h,
					(static_cast<uint32_t>(alpha) << 24) | (line.color & 0xFFFFFFu),
					0u);
			row_y += row_h;
			drew = true;
		}
	}

	// THE SYSTEM RING — the second loop. Fallback = the JO-authored HUDSYSTEXT
	// anchor (hudpos.def "5 , 22").
	const float ax = static_cast<float>(
			layout_.sys_text.present ? layout_.sys_text.x : 5);
	const float ay = static_cast<float>(
			layout_.sys_text.present ? layout_.sys_text.y : 22);
	std::vector<const HudMessageLine *> live;
	for (const HudMessageLine &line : feed_lines_) {
		if (line.expire_tick > state.ticks) {
			live.push_back(&line);
		}
	}
	if (!live.empty()) {
		// The three most recent lines, oldest first so the oldest lands on the
		// anchor and newer lines step downward.
		const int visible = std::min(static_cast<int>(live.size()), kFeedVisibleRows);
		const int first = static_cast<int>(live.size()) - visible;
		float row_y = ay;
		for (int i = first; i < static_cast<int>(live.size()); ++i) {
			const HudMessageLine *line = live[static_cast<size_t>(i)];
			emit_text(line->text.c_str(), ax, row_y, w, h, line->color, 0u);
			row_y += row_h;
		}
		drew = true;
	}
	if (drew) ++draw_list_.elements_drawn;
}


// THE RETAIL "stdbox" PANEL [orig: HUD_DrawLabelBox @0x51efd0 -> the "stdbox"
// style slot @0x51f00a -> Render_HUDBoxOverlay @0x56b700; registered at
// mission load @0x525aa2 as stdbox(border.tga, boxtile.tga, monogram.tga)
// with a ZERO fourth arg, which selects the plain-fill path below].
//
// border.tga is a 4x4 cell grid (one cell = texW/4): row 0 holds the
// top-left/top/top-right pieces, row 1 columns 0/2 the sides, row 2 the
// bottom trio, row 3 columns 0..2 the TITLED top row (stub / title bar /
// end cap), and cell (3,0) is the interior brush. The style ctor EXTRACTS
// cell (3,0) into its own texture and zeroes it out of the atlas
// [orig: BoxTexture_LoadAndSetupUVRegions @0x56acd0 — the copy+zero loop
// @0x56adbd-0x56ae44; the per-cell UV table at rec+0x40..0x174].
//
// This is its OWN geometry, NOT the menu frame's model
// (CUIElement_DrawFrame @0x64a210), where the pieces OVERHANG the rect.
//
// What binds where:
//  - The BORDER PIECES bind border x boxtile — one combined material
//    [orig: CGfxTexture_Create (ex sub_676EA0)(BoxTexA, BoxTexB, 0x651, 2) -> style+0x30 @0x56af3c,
//    applied for the piece pass @0x56b902]. Its preferred permutation runs
//    stage 0 MODULATE2X(TEXTURE, DIFFUSE) and stage 1 MODULATE2X(CURRENT,
//    TEXTURE1), alpha MODULATE(TEXTURE, DIFFUSE) then MODULATE(CURRENT,
//    TEXTURE1) — the device's tex op is MODULATE2X because
//    GfxDevice_Modulate2XEnabled is always set [orig:
//    RenderState_FindBestTextureFormatPermutation @0x6820c0, the two-stage
//    search @0x682a24..0x682c9f; RenderState_DecodeModeColorStage @0x681080
//    (0x600 / 0xF00, tex_blend_op @0x6810ff)] — so a piece reads
//    saturate(2 x border x camo) at the stencil's alpha, the camo sampled
//    SCREEN-ANCHORED and wrap-addressed: UV1 = (screen_px + 0.5)/boxtile_dim
//    [orig: HUD_DrawTexturedQuad_0 @0x56b3e0 — the dest-derived second UV pair
//    @0x56b560-0x56b592; the divisors are the boxtile TGA's own w/h, stored
//    into the style @0x56b357/@0x56b361]. Each piece quad carries that
//    second stage (HudQuad::texture2) and the device leg combines it.
//  - The FILL does NOT ride that combine: with the registration's zero
//    fourth arg the drawer takes the plain path — the extracted cell's own
//    single-texture material, one wrap-addressed quad whose UV is
//    (screen_px + 0.5)/cell, i.e. a screen-anchored tiling at the cell's own
//    UNSCALED size [orig: the rec+0x3C == 0 arm @0x56b739 ->
//    HUD_StdboxDrawFillWrapTiled @0x56b5d0]. Sampling cell (3,0) of the
//    atlas slot here is the same pixels, so the fill matches retail; the
//    tile loop stands in for hardware wrap (an atlas sub-rect cannot wrap),
//    anchored to the same absolute screen grid.
//
// Every retail quad's diffuse is alpha<<24 | 0x7F7F7F — half-bright under the
// device's MODULATE2X stage, so 0.5 x 2 = 1 and the material lands at full
// texture brightness [orig: the shl/lea prologue @0x56b70e-0x56b713]. A host
// without that stage reproduces it with a neutral white diffuse, which is
// what the caller passes.
//
// The monogram watermark pass is deliberately NOT drawn: its material carries
// flag word 0x622, whose LOW NIBBLE selects ONE/ONE — pure additive
// [orig: RenderState_DecodeBlendModeToD3DStates @0x680f00 -> D3DRS 0x13/0x14/0x1B
// via GfxBlend_ApplyToDevice @0x6817d0], its diffuse is the darker alpha<<24 | 0x282828 [orig: the lea
// @0x56b8db], and the shipped monogram.tga is measured 100% pure black, so
// the pass adds nothing. Drawing it as an opaque quad (the reading that
// decodes only the colour op and never the blend nibble) paints a black slab
// across the middle of every box.

// One border piece: cell (col,row) of the 4x4 atlas stretched into the dest
// rect. The bottom row keeps only its top 90% of BOTH the destination and the
// source cell — cropped, not squashed [orig: the crop arm @0x56b454-0x56b470,
// flt_7C459C = 0.9; the bottom trio passes the flag @0x56bbc0/@0x56bc22/
// @0x56bc80].
void HudFrameCompiler::emit_stdbox_piece(float x0, float y0, float x1, float y1,
		int col, int row, bool crop_bottom, uint32_t color) {
	if (x1 <= x0 || y1 <= y0) return;
	constexpr float kCell = 1.0f / 4.0f;
	const float u0 = static_cast<float>(col) * kCell;
	const float v0 = static_cast<float>(row) * kCell;
	const float vh = crop_bottom ? kCell * kBoxBottomCrop : kCell;
	const float dh = crop_bottom ? (y1 - y0) * kBoxBottomCrop : (y1 - y0);
	emit_rect_uv(x0, y0, x1, y0 + dh, u0, v0, u0 + kCell, v0 + vh, color,
			kHudTexBoxBorder);
	// The combined material's second stage: the boxtile camo at its own
	// dims, screen-anchored [orig: HUD_DrawTexturedQuad_0 @0x56b3e0 with the
	// style's boxtile w/h @0x56b97a..0x56bcbb].
	if (layout_.box_tile_w > 0 && layout_.box_tile_h > 0) {
		HudQuad &piece = draw_list_.quads.back();
		piece.texture2 = kHudTexBoxTile;
		piece.stage2_w = static_cast<float>(layout_.box_tile_w);
		piece.stage2_h = static_cast<float>(layout_.box_tile_h);
	}
}

void HudFrameCompiler::emit_stdbox(float x0, float y0, float x1, float y1,
		float surface_w, uint32_t color, float title_gap_w) {
	if (!layout_.box_texture_valid) return;
	if (x1 - x0 < 2.0f || y1 - y0 < 2.0f) return;
	constexpr float kCell = 1.0f / 4.0f;   // the 4x4 atlas step in UV
	// One source cell is a quarter of the atlas (32 px for the shipped 128 px
	// border.tga) [orig: quarterW/H = dims >> 2 @0x56adb6/@0x56adbd].
	const float src_cell = static_cast<float>(layout_.box_tex_w) * 0.25f;
	if (src_cell <= 0.0f) return;
	// Piece size and the fill inset scale with the surface; the fill's tile
	// PERIOD does not [orig: s = surface_w * 0.000625 @0x51f02e].
	const float s = surface_w / kBoxScaleRef;
	const float cw = src_cell * s;
	const float ch = src_cell * s;

	// 1. The interior fill: the brush cell (3,0), wrap-tiled on the ABSOLUTE
	// screen grid at the cell's own unscaled period, inside the 16*s / 24*s
	// inset rect [orig: the insets @0x56b7bd-0x56b80d (rec+0x180/0x184);
	// HUD_StdboxDrawFillWrapTiled @0x56b5d0 — UV = (dest + 0.5)/cell, wrap].
	const float fx1 = x0 + kBoxFillInsetX * s;
	const float fy1 = y0 + kBoxFillInsetY * s;
	const float fx2 = x1 - kBoxFillInsetX * s;
	const float fy2 = y1 - kBoxFillInsetY * s;
	if (fx2 > fx1 && fy2 > fy1) {
		const float fu0 = 3.0f * kCell;
		const float period = src_cell;
		const float x_start = std::floor(fx1 / period) * period;
		const float y_start = std::floor(fy1 / period) * period;
		for (float ty = y_start; ty < fy2; ty += period) {
			const float cy1 = std::max(ty, fy1);
			const float cy2 = std::min(ty + period, fy2);
			if (cy2 <= cy1) continue;
			for (float tx = x_start; tx < fx2; tx += period) {
				const float cx1 = std::max(tx, fx1);
				const float cx2 = std::min(tx + period, fx2);
				if (cx2 <= cx1) continue;
				// A clipped tile samples the matching slice of the cell, so
				// the texel phase stays locked to the screen grid.
				emit_rect_uv(cx1, cy1, cx2, cy2,
						fu0 + kCell * ((cx1 - tx) / period),
						kCell * ((cy1 - ty) / period),
						fu0 + kCell * ((cx2 - tx) / period),
						kCell * ((cy2 - ty) / period), color,
						kHudTexBoxBorder);
			}
		}
	}

	// 2. The border pieces, over the fill.
	const float ix1 = x0 + cw;   // inner x after the left column
	const float ix2 = x1 - cw;   // inner x before the right column
	const float iy1 = y0 + ch;
	const float iy2 = y1 - ch;
	if (title_gap_w > 0.0f) {
		// The TITLED top row rides the row-3 cells: the stub, the title bar
		// stretched to the measured gap, the end cap — then the plain top
		// edge resumes to the right corner [orig: the outTechnique arm
		// @0x56b93d-0x56ba52 — stub (0,3) at x0 width cw, bar (1,3) to
		// x0+cw+gap, cap (2,3) width cw; the shared edge pick-up @0x56ba57].
		const float bar_x2 = x0 + cw + title_gap_w;
		const float cap_x2 = bar_x2 + cw;
		emit_stdbox_piece(x0, y0, x0 + cw, y0 + ch, 0, 3, false, color);
		emit_stdbox_piece(x0 + cw, y0, bar_x2, y0 + ch, 1, 3, false, color);
		emit_stdbox_piece(bar_x2, y0, cap_x2, y0 + ch, 2, 3, false, color);
		if (ix2 > cap_x2)
			emit_stdbox_piece(cap_x2, y0, ix2, y0 + ch, 1, 0, false, color);
	} else {
		emit_stdbox_piece(x0, y0, x0 + cw, y0 + ch, 0, 0, false, color);   // TL
		if (ix2 > ix1)
			emit_stdbox_piece(ix1, y0, ix2, y0 + ch, 1, 0, false, color);  // top
	}
	emit_stdbox_piece(ix2, y0, x1, y0 + ch, 2, 0, false, color);       // TR
	emit_stdbox_piece(x0, iy2, x0 + cw, y1, 0, 2, true, color);        // BL
	emit_stdbox_piece(ix2, iy2, x1, y1, 2, 2, true, color);            // BR
	if (ix2 > ix1)
		emit_stdbox_piece(ix1, iy2, ix2, y1, 1, 2, true, color);       // bottom
	if (iy2 > iy1) {
		emit_stdbox_piece(x0, iy1, x0 + cw, iy2, 0, 1, false, color);  // left
		emit_stdbox_piece(ix2, iy1, x1, iy2, 2, 1, false, color);      // right
	}
}

} // namespace opennova::hud
