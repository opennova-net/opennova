// The HUD frame compiler — the witnessed element walk over the hudpos layout,
// structural translation of the ported shell draws (game_hud.gd + hud_*.gd
// helpers, themselves cited ports) onto the typed draw list.
// [orig: HUD_RenderAllOverlays @ 0x5a8070 -> HUD_RenderOverlays @ 0x5a7bb0]

#include <runtime/hud/hud_frame.h>
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

uint32_t map_label_output_argb(uint32_t argb) {
	// Retail's map-label wrapper halves the diffuse RGB before the active
	// fixed-function map/font stage doubles it. Fold both operations into the
	// Canvas glyph color, retaining the one-bit loss on odd input channels.
	const uint32_t half = half_bright_argb(argb);
	const auto doubled = [](uint32_t channel) {
		return std::min(channel * 2u, 255u);
	};
	return (half & 0xFF000000u) |
			(doubled((half >> 16) & 0xFFu) << 16) |
			(doubled((half >> 8) & 0xFFu) << 8) |
			doubled(half & 0xFFu);
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
}

void HudFrameCompiler::update_layout(const HudLayout &layout) {
	// Texture-table refresh only — the fade/flash/message state survives
	// [orig: HUD_LoadAllTextures @ 0x59e3d6 reloads art without a HUD reset].
	layout_ = layout;
}

void HudFrameCompiler::reset_overlay_buffers() {
    stance_.stamp = 0;
    flash_stamp_ = 0;
    draw_list_ = HudDrawList{};
}

void HudFrameCompiler::reset_runtime_state() {
	stance_ = StanceFade{};
	flash_prev_rounds_ = -1;
	flash_stamp_ = 0;
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
	// Chat_AddDebugMessage(text, -1, 930) @ 0x51f216].
	push_feed_line(text, 0xFFFFFFFFu, now_ticks);
}

void HudFrameCompiler::push_feed_line(const std::string &text, uint32_t argb,
		int now_ticks) {
	// The SYSTEM ring sink [orig: Chat_AddDebugMessage @ 0x4987f0 — 930-tick
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
	draw_list_.map.visible = false;
	draw_list_.big_map.visible = false;
	draw_list_.map_glyphs.clear();
	draw_list_.big_map_glyphs.clear();
	draw_list_.elements_drawn = 0;

	// The SIGHTS card draws first — the HUD overlays land on top of it
	// [orig: draw_weapon_sight_overlays @ 0x4dce00 runs at scene end;
	//  HUD_RenderAllOverlays later in the frame]. It rides the scene pass,
	//  not the overlay pass, so the level-3 early-out below never covers it.
	element_sights_card(state, surface_w, surface_h);

	// hud_detail level 3 blanks the ENTIRE gameplay overlay pass — the walk
	// below never runs [orig: the early-out @ 0x5A80C4..0x5A80DB; the
	// death-screen "exception" arm calls a spectate-label fn whose own
	// `level < 2` guard makes it a structural no-op, so the early-out carries
	// with no exception]. Only the M-cycle big map still compiles: it rides
	// Render_ProcessMainSceneFrame, not this pass [orig: @ 0x5cac50 ->
	// HUD_BuildMapOverlayView @ 0x5a7e10] — element_spinmap's own gates
	// suppress the corner map at this level.
	if (state.hud_detail_level >= 3) {
		element_spinmap(state, surface_w, surface_h);
		element_kill_announcement(state, surface_w, surface_h);
		return draw_list_;
	}

	// The stance cross-fade restamp [orig: @ 0x599f8a; it lives inside the
	// stance drawer, so a blanked level-3 pass never restamps — matched by
	// placing it under the early-out].
	if (state.stance != stance_.cur) {
		stance_.prev = stance_.cur;
		stance_.cur = state.stance;
		stance_.stamp = state.ticks;
	}

	element_frame(state, surface_w, surface_h);
	element_health(state, surface_w, surface_h);
	element_stance(state, surface_w, surface_h);
	element_weapon_cluster(state, surface_w, surface_h);
	element_heat(state, surface_w, surface_h);
	element_power(state, surface_w, surface_h);
	element_waypoint(state, surface_w, surface_h);
	// The AAS zone status panel draws BEFORE the map overlay in the retail
	// walk [orig: HUD_RenderAllOverlays @0x5a8070 — HUD_DrawZoneStatusPanel
	//  @0x5a8530, then draw_radar_blips @0x5a8535, the 3-D icon pass, and
	//  HUD_DrawMapOverlay @0x5a87bb]. The panel has NO declutter-mask bit of
	//  its own: the call @0x5a8530 is unconditional (the render_capture_point_
	//  labels / draw_radar_blips pair around it likewise) and the function's
	//  head tests only g_GameType [orig: @0x5a248d..0x5a24c5], so it draws on
	//  the shown flag alone here.
	element_lfp_panel(state, surface_w, surface_h);
	element_spinmap(state, surface_w, surface_h);
	element_objectives(state, surface_w, surface_h);
	element_attach_labels(state, surface_w, surface_h);
	element_objective_line(state, surface_w, surface_h);
	// Friendly tags draw after the overlay cluster and before the console
	// messages, exactly the retail pass order [orig: HUD_DrawFriendlyTagsPass
	// @ 0x5a87cc, then HUD_DrawConsoleMessages @ 0x5a87d1].
	element_friendly_tags(state, surface_w, surface_h);
	// The mounted-vehicle panel sits with the overlay cluster, BEFORE the feed
	// and the Tab board -- both of those are held-open surfaces that should
	// cover it, not the other way round.
	element_vehicle_panel(state, surface_w, surface_h);
	// The console messages close the overlay pass [orig: HUD_DrawConsoleMessages
	//  @0x5a87d1, after HUD_DrawFriendlyTagsPass @0x5a87cc].
	element_feed(state, surface_w, surface_h);
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
	element_kill_announcement(state, surface_w, surface_h);
	return draw_list_;
}

void HudFrameCompiler::element_spinmap(const HudFrameState &state, float w,
		float h) {
	// The gameplay pass draws whenever the HUDSPINMAP rect is authored — the
	// retail master switch is a compiled-in constant true and only /NOHUD
	// suppresses the overlay set. [orig: HUD_RenderAllOverlays @0x5a86e8 gate
	//  dword_2723CC4 (static -1); /NOHUD mask @0x4a7a09/@0x840B18]
	// The big-map pass runs regardless of the authored corner rect.
	//
	// The corner map's declutter gates: the whole spinmap block sits inside
	// the showhud bit-1 test, with the HUDDECLUT slot-17 cmp nested inside it
	// (and the level-3 early-out blanks the pass wholesale — compile()'s arm
	// re-enters here for the big map only). The big-map pass rides
	// Render_ProcessMainSceneFrame and none of these gates.
	// [orig: showhud test 2 @0x5A8635; slot-17 cmp @0x5A86E8; level early-out
	//  @0x5A80C4; big map @0x5cac50]
	const bool corner_visible = layout_.spinmap_rect.present &&
			state.hud_detail_level < 3 &&
			state.declutter_visible[kDeclutterSpinmap] &&
			(state.showhud_flags & 2u) != 0u;
	if (!corner_visible && state.minimap.map_mode == 0) return;
	// Copy-assign into the persistent input so the markers vector reuses its
	// capacity — a fresh local re-allocated it every frame.
	HudMinimapInput &input = minimap_input_;
	input = state.minimap;
	input.footprints = &state.map_footprints;
	input.rect_x1 = layout_.spinmap_rect.x;
	input.rect_y1 = layout_.spinmap_rect.y;
	input.rect_x2 = layout_.spinmap_rect.x + layout_.spinmap_rect.w;
	input.rect_y2 = layout_.spinmap_rect.y + layout_.spinmap_rect.h;
	input.surface_w = w;
	input.surface_h = h;
	input.ticks = state.ticks;
	input.waypoint_present = state.waypoint.present;
	input.waypoint_x = state.waypoint.world_x;
	input.waypoint_y = state.waypoint.world_y;
	input.waypoint_z = state.waypoint.world_z;
	input.waypoint_distance_m = state.waypoint.distance_m;
	input.waypoint_distance_offset = layout_.spinmap_wp_dist_off;
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
	// [orig: HUD_DrawTextCentered_HalfBright((int)&g_hudLabelFontBold, ...)
	//  @0x5a7ab5; HUD_DrawTextRightAligned_HalfBright @0x59cc47;
	//  HUD_DrawTextCentered_HalfBright(g_hudLabelFontLarge, ...) in the
	//  @0x5a5f40 grid branch]. Each pass keeps its own glyph list so the
	//  device leg can layer them inside that pass's sandwich.
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bold_font = have_bold ? label_font_bold_ : font_;
	const float bold_scale = have_bold ? label_scale_ : 1.0f;
	const bool have_large = label_font_large_.font() != nullptr;
	const GameFont &large_font = have_large ? label_font_large_ : bold_font;
	const float large_scale = have_large ? label_large_scale_ : bold_scale;
	const auto layout_pass = [&](const HudMapPass &pass,
			std::vector<GameFontQuad> &out) {
		for (const HudMapLabel &label : pass.labels) {
			const GameFont &lf = label.font == 1 ? large_font : bold_font;
			const float ls = label.font == 1 ? large_scale : bold_scale;
			if (lf.font() == nullptr) continue;
			const GameFontRun run = lf.layout(label.text, label.x,
					label.y, ls, ls,
					label.align == 1 ? kFontAlignRight : kFontAlignCenter,
					map_label_output_argb(label.color));
			out.insert(out.end(), run.quads.begin(), run.quads.end());
		}
	};
	// An invisible pass keeps its last-compiled label rows (only compile()
	// resets a pass) — the device discards it whole, so skip the glyph
	// layout instead of rebuilding quads for a closed map every frame.
	if (draw_list_.map.visible)
		layout_pass(draw_list_.map, draw_list_.map_glyphs);
	if (draw_list_.big_map.visible)
		layout_pass(draw_list_.big_map, draw_list_.big_map_glyphs);
	if (draw_list_.map.visible) ++draw_list_.elements_drawn;
	if (draw_list_.big_map.visible) ++draw_list_.elements_drawn;
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
	// The WPNGRP declutter gate covers the ammo count, the weapon name, and
	// the clip indicator [orig: the slot-8 cmps @ 0x5A7CC8 / @ 0x5A7D04 /
	// @ 0x5A7D42]; the crosshair rides its OWN XHAIRS slot inside
	// element_crosshair.
	const bool wpngrp_visible = state.declutter_visible[kDeclutterWpnGrp];
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
	// The XHAIRS declutter gate covers the whole crosshair complex, AND'd
	// with the binocular suppression [orig: the slot-13 cmp @ 0x592757].
	if (!state.declutter_visible[kDeclutterXhairs]) {
		return;
	}
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
	// Spread off zeroes the offset and still draws all five arms
	// [orig: the g_cfgCrossHairSpread arm @ 0x592b82; the disabled fldz
	// @ 0x592bcc].
	const float spread = layout_.crosshair_spread_enabled
			? static_cast<float>(crosshair_spread_px_fp16(
					state.hud_spread_fp16, state.fov_deg, w))
			: 0.0f;

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
			// The user colour [orig: dword_25510E0 into the corner-quad
			// params]; retail routes it via the specular channel — the
			// blend-stage witness stays open as D-HUD-8, vertex modulation
			// is the port's stand-in.
			tri.color = layout_.crosshair_color;
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

uint32_t HudFrameCompiler::active_color(const HudFrameState &state) const {
	// The hud_color_index scheme table + the derived master overlay color.
	// [orig: HUD_InitTeamColorTable @0x51f240 — the 16-dword g_hudColorTable
	// @0x24C1838 immediates (entries 0..5 are the cycled schemes); per frame
	// HUD_RenderAllOverlays @0x5a8100-0x5a8125 refreshes table[2] from the
	// hudpos hud_textcolor (g_hudposTextColor) and restamps the frame overlay
	// color (g_hudFrameOverlayColor @0x840B1C) = table[index]; the snapshot
	// twin g_hudActiveColor @0x24C1868 = table[index] | 0xFF000000 at init and
	// table[index] at the cycle @0x49afc7. Both twins carry the same value for
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
	// The WAYPOINT declutter gate [orig: the slot-3 cmp @ 0x5A7DB8].
	if (!state.declutter_visible[kDeclutterWaypoint]) {
		return;
	}
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
		// the projected point in the tag color [orig: sprintf("%ld") @0x5a41f0 /
		// @0x5a4428 -> HUD_DrawTextHalfBrightF(x, y - fontH) @0x5a4453].
		auto emit_bare_count = [&]() {
			if (!show_count) return;
			const std::string count = std::to_string(tag.revive_seconds);
			const GameFontRun run = lf.layout(count.c_str(), tag.screen_x,
					tag.screen_y - font_h, ls, ls, kFontAlignCenter,
					half_bright_keep_alpha(argb));
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
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
				emit_bare_count();
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
			if (tag.medic) {
				// The red-cross-on-white medic plate, a fontH/2 square left of
				// the text at the tag alpha [orig: rect @ 0x5a4309..0x5a436c ->
				// HUD_DrawMedicCrossQuad @ 0x59bcb0]. The quad itself is the
				// shared primitive in hud/hud_medic_cross.h (the map medic marker
				// and the help icons draw the same routine): the white field,
				// then the two red bars, in the witnessed order.
				int text_w = 0;
				int text_h = 0;
				lf.measure(label.c_str(), ls, ls, &text_w, &text_h);
				const float x0 = tag.screen_x -
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
			emit_bare_count();
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
	const float scale = large ? label_large_scale_ : 1.0f;
	const auto run = font.layout(state.kill_announcement.text.c_str(), sx(512.0f, w),
			sy(30.0f, h), scale, scale, kFontAlignCenter, 0xFFFFFFFFu);
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_end_round_overlay(const HudFrameState &state,
		float w, float h) {
	// [orig: draw_endround_stats_overlay @0x5b7cd0] The stdbox over the
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

void HudFrameCompiler::element_feed(const HudFrameState &state, float w,
		float h) {
	// THE SYSTEM MESSAGE FEED — the one ring every Chat_AddDebugMessage line
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
// style slot @0x51f00a -> render_hud_box_overlay @0x56b700; registered at
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
// What binds where, and the one recorded divergence (D-HUD-24):
//  - The BORDER PIECES bind border x boxtile — one combined material
//    [orig: CGfxTexture_Create (ex sub_676EA0)(BoxTexA, BoxTexB, 0x651, 2) -> style+0x30 @0x56af3c,
//    applied for the piece pass @0x56b902]. Stage 1 is MODULATE(CURRENT,
//    TEXTURE1) with a SCREEN-ANCHORED UV1 = (screen_px + 0.5)/boxtile_dim
//    [orig: draw_textured_quad_0 @0x56b3e0 — the dest-derived second UV pair
//    @0x56b560-0x56b592; the divisors are the boxtile TGA's own w/h, stored
//    into the style @0x56b357/@0x56b361]. We bind the raw stencil instead,
//    so the pieces read plain where retail reads camo — recorded, not
//    unknown; it needs a second texture stage this quad stream does not
//    carry yet.
//  - The FILL does NOT ride that combine: with the registration's zero
//    fourth arg the drawer takes the plain path — the extracted cell's own
//    single-texture material, one wrap-addressed quad whose UV is
//    (screen_px + 0.5)/cell, i.e. a screen-anchored tiling at the cell's own
//    UNSCALED size [orig: the rec+0x3C == 0 arm @0x56b739 ->
//    stdbox_draw_fill_wrap_tiled @0x56b5d0]. Sampling cell (3,0) of the
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
// [orig: decode_blend_mode_to_d3d_states @0x680f00 -> D3DRS 0x13/0x14/0x1B
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
	// stdbox_draw_fill_wrap_tiled @0x56b5d0 — UV = (dest + 0.5)/cell, wrap].
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

// The per-row connection icon: one band of the neticon2.tga 4-row vertical
// atlas, band = quality - 1. Any quality outside 1..3 draws NOTHING —
// retail's own gate (the parser clamps the byte at 4, and 4 still selects no
// band) [orig: NetIcon_DrawConnectionQualityBand @0x4c2ee0 — the 1..3
// switch, default returns; the atlas load @0x4c2cf0 sets band = tgaH/4].
void HudFrameCompiler::emit_net_icon(float x0, float y0, float x1, float y1,
		int quality) {
	if (!layout_.net_icon_texture_valid) return;
	if (quality < 1 || quality > 3) return;
	const float band = 1.0f / 4.0f;
	const float v0 = static_cast<float>(quality - 1) * band;
	emit_rect_uv(x0, y0, x1, y1, 0.0f, v0, 1.0f, v0 + band, 0xFFFFFFFFu,
			kHudTexNetIcon);
}

void HudFrameCompiler::element_scoreboard(const HudFrameState &state, float w,
		float h) {
	// THE TAB PLAYER LIST [orig: HUD_DrawKillListIfVisible @0x424300 gates on
	// g_scoreboardPanelVisible — TOGGLED by the playerlist input action
	// (Scoreboard_TogglePlayerList @0x4244c0), cleared on respawn init
	// @0x4993ae; rows HUD_DrawKillList @0x423a30; the centred header block
	// HUD_DrawGameScoreOverlay @0x423060]. Drawn last — above every other
	// overlay.
	if (!state.scoreboard.shown) return;

	// Every string on the board rides the BOLD label font at the slot scale
	// [orig: g_hudLabelFontBold at every draw site — the title @0x51f13a, the
	// header rungs, the rank/rows/footer HUD_DrawTextAligned (ex sub_5D3F30) calls]; layout-only
	// embedders fall back to the hudpos font.
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	if (bf.font() == nullptr) return;
	const auto text = [&](const char *t, float design_x, float design_y,
			uint32_t argb, uint32_t flags) {
		if (t == nullptr || t[0] == 0) return;
		const GameFontRun run = bf.layout(t, sx(design_x, w), sy(design_y, h),
				bscale, bscale, flags, argb);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
				run.quads.end());
		draw_list_.underlines.insert(draw_list_.underlines.end(),
				run.underlines.begin(), run.underlines.end());
	};

	const uint32_t hud = active_color(state);
	// The panel frame behind everything, with the title's bar notched into
	// the top border: the gap is the measured bold title + 2, less 12*s once
	// it exceeds that [orig: HUD_DrawLabelBox @0x51f0ea-0x51f114].
	const float s = w / kBoxScaleRef;
	float title_gap = 0.0f;
	if (!state.scoreboard.title.empty()) {
		int tw = 0;
		int th = 0;
		bf.measure(state.scoreboard.title.c_str(), bscale, bscale, &tw, &th);
		title_gap = static_cast<float>(tw) + kBoxTitlePad;
		if (title_gap > kBoxTitleTrim * s) title_gap -= kBoxTitleTrim * s;
	}
	emit_stdbox(sx(static_cast<float>(kBoardX1), w),
			sy(static_cast<float>(kBoardY1), h),
			sx(static_cast<float>(kBoardX2), w),
			sy(static_cast<float>(kBoardY2), h), w, 0xFFFFFFFFu, title_gap);
	// The title just inside the panel's top-left corner, white, left-aligned
	// [orig: (x+15, y+2) @0x51f002/@0x51f006; the caller's -1 @0x423a90].
	text(state.scoreboard.title.c_str(),
			static_cast<float>(kBoardX1 + kTitleDx),
			static_cast<float>(kBoardY1 + kTitleDy), 0xFFFFFFFFu, 0u);

	// The centred header ladder on its FIXED rungs — a missing string leaves
	// its rung blank rather than compacting the ladder [orig: the
	// unconditional +0x14 steps @0x42315c/@0x423184/@0x4231da/@0x423225].
	// Server name and mission title render white (retail embeds an explicit
	// <cFFFFFF> run [orig: @0x51f42d/@0x51f497]); the rest take the HUD color.
	float hy = static_cast<float>(kHeaderY);
	text(state.scoreboard.server_name.c_str(), kHeaderX, hy, 0xFFFFFFFFu,
			kFontAlignCenter);
	hy += kHeaderStep;
	text(state.scoreboard.mission_title.c_str(), kHeaderX, hy, 0xFFFFFFFFu,
			kFontAlignCenter);
	hy += kHeaderStep;
	text(state.scoreboard.game_type_label.c_str(), kHeaderX, hy, hud,
			kFontAlignCenter);
	hy += kHeaderStep;
	text(state.scoreboard.players_line.c_str(), kHeaderX, hy, hud,
			kFontAlignCenter);
	hy += kHeaderStep;
	// Only the spectator rung is conditional [orig: the nonzero gate
	// @0x42322a].
	if (!state.scoreboard.spectators_line.empty()) {
		text(state.scoreboard.spectators_line.c_str(), kHeaderX, hy, hud,
				kFontAlignCenter);
		hy += kHeaderStep;
	}
	// The per-mode team-score block and the flag-carrier line would advance
	// hy further here — recorded residuals [orig: @0x4232bf-0x423a12].

	// The list base sits one step below the header's return, and every row
	// cursor PRE-increments before its row draws [orig: base = return + 0x14
	// @0x423aad; row_y = cursor + 18 @0x423d30]. Paging is a recorded
	// residual, so the base never scrolls [orig: the page fold @0x423c1c].
	const float list_base = hy + static_cast<float>(kListGap);
	const bool non_team = scoreboard_is_non_team(state.scoreboard.game_type);
	// The two teams (and colors) a team-mode board columns this frame: 1/2,
	// or the 3/4 page on the frame counter's bit 7 once more than two sides
	// are configured [orig: @0x423cd0-0x423cf1].
	const ScoreboardTeamPage page = scoreboard_team_page(
			state.scoreboard.team_count, state.scoreboard.frame_counter);

	// The pre-pass mirrors the draw rules to seed the spectator cursor below
	// the LONGER player column, plus two spacer rows when both players and
	// spectators exist [orig: the count loop @0x423af1-0x423b93; the seed
	// @0x423c3a; header_rows @0x423ba1-0x423baa].
	int count_a = 0;
	int count_b = 0;
	int spectators = 0;
	{
		int toggle = 0;
		for (const ScoreboardEntry &e : state.scoreboard.rows) {
			if (e.spectator) {
				++spectators;
			} else if (non_team) {
				toggle ^= 1;
				if (toggle) ++count_a;
				else ++count_b;
			} else if (e.has_entity && e.team == page.team_a) {
				++count_a;
			} else if (e.has_entity && e.team == page.team_b) {
				++count_b;
			}
		}
	}
	const int header_rows = (spectators > 0 && (count_a > 0 || count_b > 0))
			? kSpectatorGapRows
			: 0;
	float y_a = list_base;
	float y_b = list_base;
	float y_spec = list_base + static_cast<float>(kRowPitch) *
			static_cast<float>(std::max(count_a, count_b) + header_rows);

	// One GLOBAL rank counter in wire order — both columns share it, and it
	// advances for every non-spectator row whether or not the row draws
	// [orig: the ++ @0x42424f sits outside the visibility test].
	int rank = 1;
	int ordinal = 0;
	for (const ScoreboardEntry &e : state.scoreboard.rows) {
		// Team modes draw only rows whose slot still binds a live entity on
		// one of the page's two teams — a leaver's row vanishes, and the
		// other page's teams wait their 128 frames [orig: the entity-null
		// fallthrough @0x423d1b; the team tests @0x423d28/@0x423d45].
		if (!non_team && !e.spectator &&
				!(e.has_entity && (e.team == page.team_a || e.team == page.team_b))) {
			continue;
		}
		const int col = scoreboard_column_x(e, non_team, ordinal, page);
		if (!e.spectator) ++ordinal;
		const int row_rank = rank;
		if (!e.spectator) ++rank;
		float *cursor = e.spectator ? &y_spec
				: (col == kColumnAX ? &y_a : &y_b);
		*cursor += static_cast<float>(kRowPitch);
		const float row_y = *cursor;
		// Rows draw only inside the panel [orig: the < 490 arm @0x424168;
		// the >= base arm only matters once paging lands].
		if (row_y >= static_cast<float>(kListBottom)) continue;
		const uint32_t color = scoreboard_row_color(e, non_team, hud, page);
		// Spectators carry no rank [orig: the rank sprintf sits inside the
		// non-spectator arm @0x42416e].
		if (!e.spectator) {
			char rankbuf[16];
			std::snprintf(rankbuf, sizeof(rankbuf), "%2d.", row_rank);
			// [orig: "%2ld." @0x424186 at column - 40, yellow -256 @0x4241b0]
			text(rankbuf, static_cast<float>(col + kRankDx), row_y, kRankColor,
					0u);
		}
		// The connection icon draws for EVERY row with a live slot —
		// spectators included; a wiped slot's quality 0 is the no-draw gate
		// [orig: the slot test @0x4241e2, the 16x16 quad @0x424203-0x424244].
		emit_net_icon(sx(static_cast<float>(col + kIconDx), w), sy(row_y, h),
				sx(static_cast<float>(col + kIconDx + kIconSize), w),
				sy(row_y + static_cast<float>(kIconSize), h), e.quality);
		const std::string line = scoreboard_row_text(e, non_team);
		text(line.c_str(), static_cast<float>(col), row_y, color, 0u);
	}

	// The paging hint, centred yellow [orig: the draw @0x4242d4]; PgUp/PgDn
	// paging itself is a recorded residual [orig: @0x49c912/@0x49c93e].
	text(state.scoreboard.footer.c_str(), static_cast<float>(kFooterX),
			static_cast<float>(kFooterY), kRankColor, kFontAlignCenter);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
