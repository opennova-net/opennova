// The HUD frame compiler — the witnessed element walk over the hudpos layout,
// compiled onto the typed draw list.
// [orig: HUD_RenderAllOverlays @ 0x5a8070 -> HUD_RenderOverlays @ 0x5a7bb0]

#include <runtime/hud/hud_frame.h>

#include <runtime/world/entity.h> // SeatType
#include <runtime/hud/hud_game_text.h>

#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace opennova::fnt;

namespace opennova::hud {

std::string attach_label_text(const GameTextLookup &gametext, world::SeatType seat_type,
		const std::string &attach_text_key, int32_t armory_delay_seconds) {
	using world::SeatType;
	switch (seat_type) {
		case SeatType::Passenger:
			return game_text(gametext, kGameTextOverlays, "STROVER_SIT", "!sit");
		case SeatType::Controller:
		case SeatType::Driver:
			return game_text(gametext, kGameTextOverlays, "STROVER_CONTROL", "!Control");
		case SeatType::Gunner:
			if (attach_text_key.empty())
				return game_text(gametext, kGameTextOverlays, "STROVER_USEGUN", "!UseGun");
			return game_text(gametext, kGameTextOverlays, attach_text_key.c_str(), "");
		case SeatType::ArmoryPoint:
			if (armory_delay_seconds != 0) {
				const std::string format = game_text(gametext, kGameTextOverlays,
						"STROVER_USEARMORYD", "!ArmoryDelay %d");
				return hud_sprintf(format, armory_delay_seconds);
			}
			return game_text(gametext, kGameTextOverlays, "STROVER_USEARMORY", "!UseArmory");
		default:
			return std::string();
	}
}

namespace {

// The design space in the compiler's float arithmetic (hud_math.h).
constexpr float kDesignW = static_cast<float>(kDesignWidth);
constexpr float kDesignH = static_cast<float>(kDesignHeight);
constexpr int kStanceFrames = 6;
constexpr int kMaxCarriedMessages = 40; // [orig: Chat_RebuildDisplayBuffers @ 0x498bd0]

uint32_t with_alpha(uint32_t argb, int alpha) {
	return (static_cast<uint32_t>(std::clamp(alpha, 0, 255)) << 24) |
			(argb & 0xFFFFFFu);
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
	// The compiler's four words of the reset, in its order past the radar's tick (world::
	// radar_reset) [orig: HUD_ResetAllOverlayBuffers @0x59DD40 - `mov g_HUDItemFlashLastTick,
	//  esi` @0x59DD7D, g_HUDAmmoFadeStamp @0x59DD89, g_HUDStanceFadeStamp @0x59DD8F,
	//  g_HUDSilhouetteFadeStamp @0x59DD95].
	item_flash_.reset_last_tick();
	flash_stamp_ = 0;
	stance_.stamp = 0;
	silhouette_stamp_ = 0;
	draw_list_ = HudDrawList{};
}

void HudFrameCompiler::reset_runtime_state() {
	// The mission start's HUD words: the fade stamps (the HUD init's overlay-buffer reset), the
	// snapshot twin's seed and the rings (the start's chat clear). The process words
	// (HudProcessWords) are no HUD init's: a carried set stands [orig: HUD_InitOverlaySystem
	//  @0x5A4620 - the HUD_ResetAllOverlayBuffers call @0x5A4949, the g_HUDColors.active seed
	//  @0x5A4977; Game_StartMission @0x524360 - the Chat_ClearAllChannels call @0x524A10].
	stance_.stamp = 0;
	flash_stamp_ = 0;
	hud_colors_active_ = 0;
	hud_colors_active_index_ = -1;
	silhouette_stamp_ = 0;
	clear_message_rings();
	draw_list_ = HudDrawList{};
}

HudProcessWords HudFrameCompiler::process_words() const {
	HudProcessWords words;
	words.item_flash_timers = item_flash_.timers();
	words.stance_prev = stance_.prev;
	words.stance_cur = stance_.cur;
	words.ammo_key_bucket = flash_key_bucket_;
	words.ammo_key_reserve = flash_key_reserve_;
	words.ammo_key_class = flash_key_class_;
	words.waypoint_altitude_color = waypoint_altitude_color_;
	words.waypoint_altitude_band = waypoint_altitude_band_;
	words.silhouette_vehicle = silhouette_vehicle_;
	words.silhouette_weapon = silhouette_weapon_;
	return words;
}

void HudFrameCompiler::carry_process_words(const HudProcessWords &words) {
	item_flash_.carry_timers(words.item_flash_timers);
	stance_.prev = words.stance_prev;
	stance_.cur = words.stance_cur;
	flash_key_bucket_ = words.ammo_key_bucket;
	flash_key_reserve_ = words.ammo_key_reserve;
	flash_key_class_ = words.ammo_key_class;
	waypoint_altitude_color_ = words.waypoint_altitude_color;
	waypoint_altitude_band_ = words.waypoint_altitude_band;
	silhouette_vehicle_ = words.silhouette_vehicle;
	silhouette_weapon_ = words.silhouette_weapon;
}

void HudFrameCompiler::push_chat_line(const std::string &text, uint32_t argb,
		int now_ticks) {
	// The CHAT display-buffer sink [orig: Chat_AddMessageChannel1 @0x4985d0].
	if (text.empty()) {
		return;
	}
	// The raw ring first: the whole line cut at 119 characters, with the
	// caller's colour, into the newest raw slot [orig: the raw shift and the
	// copy @0x498621, the colour / stamp stores into slot 0 (byte_B3EA38
	// +0x78 / +0x7C)].
	{
		HudMessageLine raw;
		raw.text = text.substr(0, static_cast<size_t>(kMessageTextMax));
		raw.color = argb;
		raw.expire_tick = now_ticks;
		chat_raw_lines_.push_back(std::move(raw));
		while (chat_raw_lines_.size() > static_cast<size_t>(kMessageSlotCount))
			chat_raw_lines_.erase(chat_raw_lines_.begin());
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
	// The wrap runs on every line whatever the box holds: with no hud.def its
	// rows are 0 and the width is 4 (D-HUD-58).
	std::string buf = text;
	int count = 0;
	if (label_font_bold_.font() != nullptr) {
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

void HudFrameCompiler::clear_message_rings() {
	feed_lines_.clear();
	chat_lines_.clear();
	chat_raw_lines_.clear();
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

HudDrawCursor HudFrameCompiler::draw_cursor_() const {
	HudDrawCursor at;
	at.quads = draw_list_.quads.size();
	at.tris = draw_list_.tris.size();
	at.lines = draw_list_.lines.size();
	at.glyphs = draw_list_.glyphs.size();
	at.underlines = draw_list_.underlines.size();
	return at;
}

void HudFrameCompiler::note_element_(HudElement element, const HudDrawCursor &from, bool map, bool big_map) {
	HudElementSpan span;
	span.element = element;
	span.begin = from;
	span.end = draw_cursor_();
	span.map = map;
	span.big_map = big_map;
	const bool drew = span.end.quads != from.quads || span.end.tris != from.tris || span.end.lines != from.lines ||
			span.end.glyphs != from.glyphs || span.end.underlines != from.underlines || map || big_map;
	if (drew) draw_list_.element_spans.push_back(span);
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
	draw_list_.veil_begin = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
	draw_list_.order_breaks.clear();
	draw_list_.elements_drawn = 0;
	draw_list_.element_spans.clear();

	// The SIGHTS card draws first — the HUD overlays land on top of it
	// [orig: HUD_DrawWeaponSightOverlays @ 0x4dce00 runs at scene end;
	//  HUD_RenderAllOverlays later in the frame]. It rides the scene pass,
	//  not the overlay pass, so the level-3 early-out below never covers it.
	element_(HudElement::SightsCard, [&] { element_sights_card(state, surface_w, surface_h); });

	// The snapshot twin's two non-frame writers (sync_hud_colors_active); the
	// per-frame restamp is the GAMEINFO drawer's (element_game_info).
	sync_hud_colors_active(state);

	// hud_detail level 3 blanks the ENTIRE gameplay overlay pass — the walk
	// below never runs [orig: the early-out @ 0x5A80C4..0x5A80DB; the
	// death-screen "exception" arm calls a spectate-label fn whose own
	// `level < 2` guard makes it a structural no-op, so the early-out carries
	// with no exception]. Only the M-cycle big map still compiles: it rides
	// Render_ProcessMainSceneFrame, not this pass [orig: @ 0x5cac50 ->
	// HUD_BuildMapOverlayView @ 0x5a7e10] — element_spinmap's own gates
	// suppress the corner map at this level. HUD_DrawGameplayOverlays' legs
	// draw after the big map, over the view veil (compile_gameplay_overlay_windows).
	element_(HudElement::InsetCues, [&] { element_inset_cues(state, surface_w, surface_h); });
	// The pass's head builds the entity-info record past its round-over and white-flash returns
	// and ahead of the level-3 one: its map fields are what the big map and the map windows read
	// (record_map_) [orig: HUD_RenderAllOverlays @0x5A8070 - the HUD_BuildEntityInfo call
	//  @0x5A80BC, ahead of g_HUDDetailLevel @0x5A80C4].
	if (overlay_pass_runs(state)) {
		record_map_.waypoint_present = state.waypoint.present;
		record_map_.waypoint_x = state.waypoint.world_x;
		record_map_.waypoint_y = state.waypoint.world_y;
		record_map_.waypoint_z = state.waypoint.world_z;
		record_map_.farp_present = state.combat.farp_present;
		record_map_.farp_x = state.combat.farp_x_q16;
		record_map_.farp_y = state.combat.farp_y_q16;
	}
	if (state.hud_detail_level >= 3) {
		// The blanked pass draws nothing under the view veil.
		mark_veil_layer();
		element_(HudElement::Spinmap, [&] { element_spinmap(state, surface_w, surface_h); });
		// The tip rides the scene frame, not the overlay pass: the blank level
		// keeps it (the alternate slot draws in the top layer, after the map).
		if (state.minimap.map_mode == 0)
			element_(HudElement::Tip, [&] { element_tip(state, false, surface_w, surface_h); });
		// The frame drawer's panels run outside the level-3 skip: the chat
		// input line precedes the kill banner [orig: HUD_DrawOverlayPanels
		// @0x5c014e then HUD_DrawKillAnnounceBanner @0x5c0184]. The voice-macro
		// menus and the pause text are the same pass's, ahead of the input line.
		compile_overlay_panel_menus(state, surface_w, surface_h);
		element_(HudElement::ChatInput, [&] { element_chat_input(state, surface_w, surface_h); });
		element_(HudElement::KillAnnouncement, [&] { element_kill_announcement(state, surface_w, surface_h); });
		compile_gameplay_overlay_windows(state, surface_w, surface_h);
		return draw_list_;
	}
	// The gameplay overlay pass also returns at its head on the round-over latch and while the
	// white hit flash burns, before any draw or any of its state work (the palette entry 2
	// refresh, the GAMEINFO restamp, the stance cross-fade): only the big map of the scene
	// frame's own legs takes its place, and the legs below draw as on any frame
	// [orig: HUD_RenderAllOverlays @0x5A8084 (g_RoundOver), @0x5A8098 (g_ScreenFlashWhiteHit),
	//  the return @0x5A80DB].
	if (overlay_pass_runs(state))
		compile_overlay_pass(state, surface_w, surface_h);
	else
		element_(HudElement::Spinmap, [&] { element_spinmap(state, surface_w, surface_h); });
	// The main HUD ends here: the damage quads and the sun veil draw over it, and everything
	// below over them (HudDrawList::veil_begin carries the witness, D-NET-1384).
	mark_veil_layer();
	// The tip at MRCLIPPYNORMAL follows the HUD pass while the M-cycle map is
	// closed, ahead of the gameplay overlays and the overlay panels
	// [orig: Render_ProcessMainSceneFrame @0x5cac50 (map mode 0) -> @0x5cad21
	//  CTipSystem_Draw(0)]; the alternate slot rides the top layer
	// (compile_gameplay_overlay_windows).
	if (state.minimap.map_mode == 0)
		element_(HudElement::Tip, [&] { element_tip(state, false, surface_w, surface_h); });
	// The Recent Messages window draws from the frame drawer, not the overlay
	// pass: after every HUD element and BEFORE the Tab board
	// [orig: Server_DrawStatusScreen @0x50a2d0 — HUD_DrawMessageLog @0x50b21f,
	//  then HUD_DrawConsoleMessagesOverlay @0x50b23d and HUD_DrawPlayerScoreList
	//  @0x50b281].
	// The stats panel precedes the message log in the frame drawer
	// [orig: HUD_DrawOverlayPanels @0x5c0083 (stats) then @0x5c009a (message log)].
	element_(HudElement::EndRoundStatistics,
			[&] { element_end_round_statistics(state, surface_w, surface_h); });
	element_(HudElement::MessageLog, [&] { element_message_log(state, surface_w, surface_h); });
	element_(HudElement::Scoreboard, [&] { element_scoreboard(state, surface_w, surface_h); });
	element_(HudElement::EndRoundOverlay, [&] { element_end_round_overlay(state, surface_w, surface_h); });
	// [orig: HUD_DrawOverlayPanels — the voice-macro menus @0x5c00d8 /
	// @0x5c00ea and the pause text @0x5c0120 (compile_overlay_panel_menus),
	// the input line @0x5c014e, then the kill banner @0x5c0184]
	compile_overlay_panel_menus(state, surface_w, surface_h);
	element_(HudElement::ChatInput, [&] { element_chat_input(state, surface_w, surface_h); });
	element_(HudElement::KillAnnouncement, [&] { element_kill_announcement(state, surface_w, surface_h); });
	compile_gameplay_overlay_windows(state, surface_w, surface_h);
	// The connection indicators close the scene frame, after the overlay
	// panels, at level 0 only (so never on the level-3 path above)
	// [orig: Render_ProcessMainSceneFrame — HUD_DrawOverlayPanels @0x5cae3b,
	//  then CNetQuality_DrawIndicators(0) @0x5cae5d].
	element_(HudElement::NetQuality,
			[&] { element_net_quality_indicators(state, surface_w, surface_h); });
	return draw_list_;
}

// THE GAMEPLAY OVERLAY PASS below the level-3 early-out [orig:
// HUD_RenderAllOverlays @0x5a8070]. /NOHUD returns after the palette and radar
// work (bit 1 of the master word), so only the M-cycle big map — a pass of its
// own — still compiles [orig: `test dword_840B18, 2` @0x5A81CE; the big map
// Render_ProcessMainSceneFrame @0x5cac50 -> HUD_BuildMapOverlayView @0x5a7e10].
void HudFrameCompiler::compile_overlay_pass(const HudFrameState &state, float surface_w,
		float surface_h) {
	// Past its early-outs (the caller's overlay_pass_runs and level test) the pass first ticks
	// the item flash timers on the logic tick, so a gated stretch's ticks land on the next
	// pass whole (HudItemFlash) [orig: HUD_RenderAllOverlays @0x5A8070 - g_CurrentTick
	// @0x5A80F3, the HUD_TickItemFlashTimers call @0x5A80FB]. It then refreshes palette
	// entry 2 from the hudpos text colour, ahead of the /NOHUD gate (hud_palette2_refreshed_
	// carries the witness).
	item_flash_.tick(state.ticks);
	hud_palette2_refreshed_ = true;
	// The waypoint's altitude colour and band, past the palette store and the projection: set
	// only while the record's waypoint is active, kept otherwise. The x86 `sub` wraps; the
	// compares are signed [orig: HUD_RenderAllOverlays - the HUD_UpdateWaypointAltitudeColor call
	//  @0x5A815F; HUD_UpdateWaypointAltitudeColor @0x590970 - g_WaypointActiveByte @0x590970, the
	//  delta @0x590984, `jge` @0x59098C, `jle` @0x5909A8, the stores @0x59098E..0x5909C9].
	if (record_map_.waypoint_present) {
		const int32_t dz = static_cast<int32_t>(static_cast<uint32_t>(record_map_.waypoint_z) -
				static_cast<uint32_t>(state.minimap.player_z));
		if (dz < -0x20000) {
			waypoint_altitude_color_ = 0xFF20407Fu; // below: blue
			waypoint_altitude_band_ = 1;
		} else if (dz > 0x20000) {
			waypoint_altitude_color_ = 0xFF7F5000u; // above: orange
			waypoint_altitude_band_ = 0;
		} else {
			waypoint_altitude_color_ = 0xFF007000u; // level: green
			waypoint_altitude_band_ = 2;
		}
	}
	if ((state.overlay_master & 2u) == 0u) {
		element_(HudElement::Spinmap, [&] { element_spinmap(state, surface_w, surface_h); });
		return;
	}
	// The GAMEINFO/ZONEINFO overlay (and the g_HUDColors.active restamp) is
	// the first draw after the gate, before the crosshair and the element
	// dispatcher [orig: HUD_DrawGameTimerOverlay @0x5A8499].
	element_(HudElement::GameInfo, [&] { element_game_info(state, surface_w, surface_h); });

	// On the death screen the element dispatcher runs its death arm alone: the
	// spectate arm's health bar and TEAMID line, or else the waypoint, then the
	// timer and the score, and it returns before the static frame, so the
	// frame, the stance, the weapon cluster, the heat and power bars, the slot
	// bar, the vehicle panel and the seat labels never draw there (each drawer
	// below keeps its own death-screen rule) [orig: HUD_RenderOverlays
	// @0x5A7BC5..0x5A7C68, the returns @0x5A7C68 and (no CLOCK) @0x5A7C53 ->
	// @0x5A7E01, both before the frame's gate @0x5A7C70].
	const bool death_arm = state.combat.death_screen;

	// The stance cross-fade restamp [orig: @ 0x599f8a; it lives inside the
	// stance drawer, so a blanked level-3 pass or the death arm never restamps —
	// matched by placing it under the early-out and the death arm].
	if (!death_arm && hud_stance_group_visible(state) && state.stance != stance_.cur) {
		stance_.prev = stance_.cur;
		stance_.cur = state.stance;
		stance_.stamp = state.ticks;
	}

	if (!death_arm) element_(HudElement::Frame, [&] { element_frame(surface_w, surface_h); });
	element_(HudElement::Health, [&] { element_health(state, surface_w, surface_h); });
	element_(HudElement::Instruments, [&] { element_instruments(state, surface_w, surface_h); });
	if (death_arm) {
		// The death arm's tail: the TEAMID line (spectating) or the waypoint, then
		// the CLOCK pair [orig: @0x5A7C12 / @0x5A7C47, then @0x5A7C59 / @0x5A7C5E].
		element_(HudElement::TeamIdLine, [&] { element_team_id_line(state, surface_w, surface_h); });
		element_(HudElement::Waypoint, [&] { element_waypoint(state, surface_w, surface_h); });
		element_(HudElement::Clock, [&] { element_clock(state, surface_w, surface_h); });
	} else {
		element_(HudElement::Stance, [&] { element_stance(state, surface_w, surface_h); });
		element_weapon_cluster(state, surface_w, surface_h);
		element_(HudElement::Heat, [&] { element_heat(state, surface_w, surface_h); });
		// The CLOCK pair follows the heat bar and the icons in the dispatcher
		// [orig: HUD_RenderOverlays @0x5A7D75..0x5A7D7C].
		element_(HudElement::Clock, [&] { element_clock(state, surface_w, surface_h); });
		element_(HudElement::Power, [&] { element_power(state, surface_w, surface_h); });
		element_(HudElement::Waypoint, [&] { element_waypoint(state, surface_w, surface_h); });
		// The TEAMID line then the HUDLS bar close the dispatcher's walk
		// [orig: @0x5A7DE9, @0x5A7DF7].
		element_(HudElement::TeamIdLine, [&] { element_team_id_line(state, surface_w, surface_h); });
		element_(HudElement::WeaponSlotBar, [&] { element_weapon_slot_bar(state, surface_w, surface_h); });
	}
	// The AAS zone status panel draws BEFORE the map overlay in the retail
	// walk [orig: HUD_RenderAllOverlays @0x5a8070 — HUD_DrawZoneStatusPanel
	//  @0x5a8530, then HUD_DrawVehicleBayLogos (ex Radar_DrawBlips) @0x5a8535,
	//  the 3-D icon pass, and HUD_DrawMapOverlay @0x5a87bb]. The panel has NO
	//  declutter-mask bit of its own: the call @0x5a8530 is unconditional (the
	//  capture-point labels / bay-logo pair around it likewise) and the
	//  function's head tests only g_GameType [orig: @0x5a248d..0x5a24c5], so it
	//  draws on the shown flag alone here.
	element_(HudElement::ScopeDetails, [&] { element_scope_details(state, surface_w, surface_h); });
	// The capture-point labels join the walk after the death-screen fork's
	// scope details and before the zone panel, with no gate of their own
	// [orig: Render_CapturePointLabels @0x5a852b].
	element_(HudElement::CapturePointLabels,
			[&] { element_capture_point_labels(state, surface_w, surface_h); });
	element_(HudElement::LfpPanel, [&] { element_lfp_panel(state, surface_w, surface_h); });
	// The vehicle-bay logos: no mask bit, no death-screen test (the only
	// death-screen fork in this stretch covers the scope details)
	// [orig: HUD_DrawVehicleBayLogos @0x5a8535; the fork @0x5a850d..0x5a852b].
	element_(HudElement::VehicleBayLogos, [&] { element_vehicle_bay_logos(state, surface_w, surface_h); });
	element_(HudElement::Spinmap, [&] { element_spinmap(state, surface_w, surface_h); });
	// The seat and armory labels are the dispatcher's [orig: HUD_RenderOverlays
	// @0x5A7DAA], so the death arm skips them.
	if (!death_arm) element_(HudElement::AttachLabels, [&] { element_attach_labels(state); });
	// Friendly tags draw after the overlay cluster and before the console
	// messages, exactly the retail pass order [orig: HUD_DrawFriendlyTagsPass
	// @ 0x5a87cc, then HUD_DrawConsoleMessages @ 0x5a87d1].
	element_(HudElement::FriendlyTags, [&] { element_friendly_tags(state, surface_w); });
	// The mounted-vehicle panel sits with the overlay cluster, BEFORE the feed
	// and the Tab board -- both of those are held-open surfaces that should
	// cover it, not the other way round. The panel rides the seat dispatch's
	// stance arms (slots 1 / 2 / 5, and 3 under WPNGRP); seat slot 0 has no
	// panel arm of its own, so a hidden WPNGRP hides it there too
	// [orig: HUD_RenderOverlays -- no root @0x5A7CAE; the panel calls
	//  @0x5A7CE4 / @0x5A7CFF / @0x5A7D23; `cmp eax,1; jnz` @0x5A7CF5..0x5A7CF8].
    if (!death_arm && hud_stance_group_visible(state))
        element_(HudElement::VehiclePanel,
                [&] { element_vehicle_panel(state, surface_w, surface_h); });
	// The console messages close the overlay pass [orig: HUD_DrawConsoleMessages
	//  @0x5a87d1, after HUD_DrawFriendlyTagsPass @0x5a87cc].
	element_(HudElement::Feed, [&] { element_feed(state, surface_w, surface_h); });
	// The squad order lines right after the feed [orig: HUD_DrawSquadOrderLines @0x5a87d6;
	//  the earlier call @0x5a8508 off the death screen draws the same two
	//  lines at the same place first].
	element_(HudElement::SquadOrders, [&] { element_squad_orders(state, surface_w, surface_h); });
	// The takeover dialog off the death screen [orig: `cmp g_DeathScreenActive`
	//  @0x5A87E2, HUD_DrawTakeoverStatus @0x5A87E4].
	if (!death_arm)
		element_(HudElement::TakeoverStatus, [&] { element_takeover_status(state, surface_w, surface_h); });
	// The optical cues (the LollyPop marker, STROVER_DIST) close the pass, after
	// the console messages, the squad orders and the takeover status, with no
	// death-screen test [orig: HUD_RenderAllOverlays @0x5A87EF..0x5A89DA, after
	// HUD_DrawTakeoverStatus @0x5A87E4].
	element_(HudElement::OpticalCues, [&] { element_optical_cues(state, surface_w, surface_h); });
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
		element_(HudElement::VoiceMenus, [&] {
			element_voice_macro_menu(state, state.emotes_menu, kHudEmotesMenuContextRow, w, h);
		});
		element_(HudElement::VoiceMenus, [&] {
			element_voice_macro_menu(state, state.radio_menu, kHudRadioMenuContextRow, w, h);
		});
	}
	element_(HudElement::PausedText, [&] { element_paused_text(state, w, h); });
}

// HUD_DrawGameplayOverlays, which the scene frame calls after the big map, so
// its draws layer above it (HudDrawList::top_begin) and above the view veil:
// the frame-rate overlay, the breath bar, the connection status, the start
// timer and the service prompts, then past its level-3 skip, so they survive
// the blank level, the briefing, the objectives panel, then the help screen /
// map legend [orig: HUD_DrawGameplayOverlays @0x5BDE60 — the level-3 jump to
// LABEL_24 @0x5bdeaf skips only the framerate / breath / ping / timer / armory
// legs; the briefing @0x5be133..0x5be145 (a session draws HUD_DrawEndGameScreen
// instead), HUD_DrawWinConditions @0x5be163, HelpScreen_Draw @0x5be179; the
// caller runs after the big map, Render_ProcessMainSceneFrame @0x5cad15 then
// @0x5cae0b]. The whole function returns on a zero /NOHUD master word
// [orig: @0x5BDE9B].
void HudFrameCompiler::compile_gameplay_overlay_windows(const HudFrameState &state, float w,
		float h) {
	mark_top_layer();
	// With the M-cycle map up the tip draws at MRCLIPPYALTERNATE right after
	// the big map; a dead local player's map mode is cleared instead and that
	// frame draws no tip [orig: Render_ProcessMainSceneFrame @0x5cac5d..0x5cac77
	// (Flags & 2 -> g_MapOverlayMode = 0), @0x5cad15 HUD_BuildMapOverlayView
	// then @0x5cad1d CTipSystem_Draw(1)]. /NOHUD does not reach it (the
	// gameplay-overlay gate below is HUD_DrawGameplayOverlays').
	if (state.minimap.map_mode != 0 && !state.local_dead)
		element_(HudElement::TipAlternate, [&] { element_tip(state, true, w, h); });
	if (state.overlay_master == 0u) return;
	// The head legs, in their order. The level-3 skip jumps past the overlay and the bar to the
	// FARP leg (the service prompt's own level test keeps that) (D-NET-NEW-LI-32,
	// D-NET-NEW-LI-33) [orig: `cmp g_HUDDetailLevel, 3; jz` @0x5BDEA8 to @0x5BDFF6, the overlay
	// @0x5BDEB5..0x5BDEC2, the bar @0x5BDED3, the prompts @0x5BDF1B..0x5BE10E]. The connection
	// status line and the start timer follow the bar, past its round-over gate
	// (D-NET-NEW-LI-42) [orig: HUD_DrawPingDisplay called @0x5BDED8, HUD_DrawTimerOverlay
	// called @0x5BDEDD].
	if (state.hud_detail_level != 3) {
		element_(HudElement::FrameRateOverlay, [&] { element_frame_rate_overlay(state, w, h); });
		element_(HudElement::BreathBar, [&] { element_breath_bar(state, w, h); });
		element_(HudElement::ConnectStatus, [&] { element_connect_status(state, w, h); });
		element_(HudElement::StartTimer, [&] { element_start_timer(state, w, h); });
	}
	element_(HudElement::ServicePrompt, [&] { element_service_prompt(state, w, h); });
	element_(HudElement::Briefing, [&] { element_briefing(state, w, h); });
	element_(HudElement::Objectives, [&] { element_objectives(state, w, h); });
	element_(HudElement::HelpScreen, [&] { element_help_screen(state, w, h); });
	// The quit dialog below the blank level [orig: `cmp g_HUDDetailLevel, 3`
	// @0x5be185; UI_DrawQuitConfirmDialog @0x5be1ce]. Out of a session
	// it would also draw the save-game list (sub_5B72E0 / sub_5B74B0
	// @0x5be1dd..0x5be1eb); the dialog only opens in a session.
	if (state.hud_detail_level != 3) element_(HudElement::QuitDialog, [&] { element_quit_dialog(state, w, h); });
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

// The corner pass draws when the HUDSPINMAP rect is authored and its gates
// below pass. [orig: /NOHUD mask @0x4a7a09/@0x840B18]
// The corner map's declutter gates: the whole spinmap block sits inside the
// showhud bit-1 test, with the HUDDECLUT slot-17 cmp nested inside it (and
// the level-3 early-out blanks the pass wholesale — compile()'s arm re-enters
// element_spinmap for the big map only). The big-map pass rides
// Render_ProcessMainSceneFrame and none of these gates.
// [orig: showhud test 2 @0x5A8635; slot-17 cmp @0x5A86E8; level early-out
//  @0x5A80C4; big map @0x5cac50]
bool HudFrameCompiler::corner_spinmap_visible(const HudFrameState &state) const {
	// The corner map draws inside the pass: its call site and its head first
	// (overlay_pass_runs), then its own gates.
	return overlay_pass_runs(state) && corner_spinmap_gates_(state);
}

bool HudFrameCompiler::corner_spinmap_gates_(const HudFrameState &state) const {
	// The corner map's gates: the authored HUDSPINMAP rect, the /NOHUD
	// overlay master's bit 1, the level-3 early-out, the HUDDECLUT slot-17 cmp
	// nested in the showhud bit-1 test [orig: /NOHUD mask @0x4a7a09 / @0x5a81ce;
	// level early-out @0x5A80C4; showhud test 2 @0x5A8635; slot-17 cmp @0x5A86E8].
	return layout_.spinmap_rect.present && (state.overlay_master & 2u) != 0u &&
			state.hud_detail_level < 3 &&
			state.declutter_visible[kDeclutterSpinmap] && (state.showhud_flags & 2u) != 0u;
}

// The overlay pass runs where the scene frame calls it, past its head: the round-over latch
// and the white hit flash return before it draws [orig: Render_ProcessMainSceneFrame - the
// calls @0x5CACE3 / @0x5CAD04; HUD_RenderAllOverlays @0x5A8084..0x5A809F].
bool HudFrameCompiler::overlay_pass_runs(const HudFrameState &state) {
	return state.overlay_pass_called && overlay_pass_head_runs(state);
}

bool HudFrameCompiler::overlay_pass_head_runs(const HudFrameState &state) {
	return !state.round_over && !state.combat.white_flash;
}

uint32_t HudFrameCompiler::radar_frame_gates(const HudFrameState &state) const {
	// [orig: g_HUDDetailLevel == 3 @0x5a80c4; the round-over early-out
	//  @0x5a8084 is the session's own read (inmatch/session.h)]
	if (state.hud_detail_level >= 3) return 0u;
	// The corner map's own update site needs mask bits 9, 6 and 10; its head and gates are
	// the HUD's, its call site the session's own (inmatch::Session::hud_pass_called), so the
	// gates handed back leave overlay_pass_called out [orig: @0x5a78ff / @0x5a7906 /
	// @0x5a790a; HUD_RenderAllOverlays @0x5A8084..0x5A809F; Render_ProcessMainSceneFrame -
	// the calls @0x5CACE3 / @0x5CAD04].
	const bool map_site = overlay_pass_head_runs(state) && corner_spinmap_gates_(state) &&
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
	input.hud_frame = state.hud_frame;
	input.session_frame = state.session_frame;
	input.item_flash = item_flash_.timers();
	// The record's waypoint and nearest FARP as the pass's head last built them (record_map_),
	// the altitude colour and band as the pass last set them, the tether's neutral colour
	// (g_HudposTextColor = the hudpos hud_textcolor), and the non-bank legs' feed (hud_minimap.h
	// HudMinimapOverlays).
	input.waypoint_present = record_map_.waypoint_present;
	input.waypoint_x = record_map_.waypoint_x;
	input.waypoint_y = record_map_.waypoint_y;
	input.waypoint_z = record_map_.waypoint_z;
	input.waypoint_altitude_color = waypoint_altitude_color_;
	input.waypoint_altitude_band = waypoint_altitude_band_;
	input.waypoint_distance_offset = layout_.spinmap_wp_dist_off;
	input.farp_present = record_map_.farp_present;
	input.farp_x = record_map_.farp_x;
	input.farp_y = record_map_.farp_y;
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
	// LARGE slot — every one through the CPU half-bright drawer, its halved
	// colour the raw diffuse the font page's MODULATE2X doubles on the device
	// (kFontPageMaterialWord; the map pass's top item runs it per page).
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
		// The drawer's halved colour, the raw diffuse [orig:
		// HUD_DrawTextCentered_HalfBright @0x580680 and its siblings, alpha
		// forced opaque; HUD_DrawTextHalfBrightF @0x580720 keeps it (the bit19
		// label)].
		const GameFontRun run = lf.layout(label.text, label.x,
				label.y, ls, ls, flags,
				label.keep_alpha != 0
						? half_bright_keep_alpha(label.color)
						: half_bright_argb(label.color));
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
void map_window_input(const HudFrameState &state, const HudItemFlash &item_flash,
		float surface_w, float surface_h, HudMinimapInput &input) {
	input = state.minimap;
	input.footprints = &state.map_footprints;
	input.surface_w = surface_w;
	input.surface_h = surface_h;
	input.ticks = state.ticks;
	input.hud_frame = state.hud_frame;
	input.session_frame = state.session_frame;
	// The windowed passes run the same legs as the big map (the CMAP mask is
	// the big-map mask), so they read the same non-bank feed, flash timers,
	// FARP target (the record's, set by the caller) and colours.
	input.item_flash = item_flash.timers();
	input.overlays = &state.map_overlays;
}

} // namespace

const HudFrameCompiler::MapWindowDraw &HudFrameCompiler::compile_death_map(
		const HudFrameState &state, const DeathMapFrame &frame, const DeathMapFacts &facts,
		float surface_w, float surface_h) {
	// The zone letters and score lines lay out in the bold label slot like the
	// other map labels [orig: MapOverlay_DrawView — HUD_DrawTextCentered_HalfBright(
	// g_HUDLabelFontBold) @0x5a5c59/@0x5a5cbf/@0x5a5d14].
	map_window_input(state, item_flash_, surface_w, surface_h, map_window_input_);
	map_window_input_.farp_present = record_map_.farp_present;
	map_window_input_.farp_x = record_map_.farp_x;
	map_window_input_.farp_y = record_map_.farp_y;
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

// The console messages alone, into their own list: CMAP's CHAT_MSGS handler
// is a bare jump into HUD_DrawConsoleMessages from inside the menu walk — the
// payload rect unread, the same anchors, gates and rings as the HUD's own
// call, the squad order lines not drawn.
// [orig: CMap_OnChatMsgsCustomDraw @0x5482d0 (`if (event == 1) jmp
//  HUD_DrawConsoleMessages`); HUD_DrawConsoleMessages @0x59ad30]
const HudDrawList &HudFrameCompiler::compile_console_messages(const HudFrameState &state,
		float surface_w, float surface_h) {
	std::swap(draw_list_, console_draw_);
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
	draw_list_.veil_begin = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
	draw_list_.order_breaks.clear();
	draw_list_.elements_drawn = 0;
	element_feed(state, surface_w, surface_h);
	std::swap(draw_list_, console_draw_);
	return console_draw_;
}

const HudFrameCompiler::MapWindowDraw &HudFrameCompiler::compile_command_map(
		const HudFrameState &state, CommandMapView &cmap, const MapViewRect &rect,
		int32_t scaled_800, const DeathMapFacts &facts, float surface_w, float surface_h) {
	map_window_input(state, item_flash_, surface_w, surface_h, map_window_input_);
	map_window_input_.farp_present = record_map_.farp_present;
	map_window_input_.farp_x = record_map_.farp_x;
	map_window_input_.farp_y = record_map_.farp_y;
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

void HudFrameCompiler::emit_bordered_quad(float x, float y, int tex_w, int tex_h, uint32_t argb,
		int32_t texture, float surface_w, float surface_h) {
	const float x0 = sx(x, surface_w);
	const float y0 = sy(y, surface_h);
	const float x1 = sx(x + static_cast<float>(tex_w), surface_w);
	const float y1 = sy(y + static_cast<float>(tex_h), surface_h);
	const BorderedQuadUv uv = bordered_quad_uv(tex_w, tex_h, x0, y0, x1, y1);
	emit_rect_uv(x0, y0, x1, y1, uv.u0, uv.v0, uv.u1, uv.v1, argb, texture);
}

void HudFrameCompiler::element_frame(float w, float h) {
	if (!layout_.frame_texture_valid) {
		return;
	}
	// A bordered quad of the frame's image size [orig: HUD_RenderOverlays
	// @0x5A7C91 -> HUD_DrawTexturedQuadWithBorder @0x590C40, the record
	// 0x2723970's size as both the texture and the quad extent].
	const float x = static_cast<float>(layout_.frame_pos.x);
	const float y = static_cast<float>(layout_.frame_pos.y);
	emit_bordered_quad(x, y, layout_.frame_tex_w, layout_.frame_tex_h, 0xFFFFFFFFu, kHudTexFrame, w, h);
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
	// The ratio is HUD_BuildEntityInfo's (health << 16) / max capped UNSIGNED at
	// 1.0, so a negative health word reads full, as an over-max one does
	// [orig: HUD_BuildEntityInfo @0x4B87C9..0x4B87D3 (`cmp eax,10000h; jbe`)].
	const float raw_fraction = state.combat.death_screen
			? state.session.spectated_health_fraction
			: state.health_fraction;
	const float fraction =
			(raw_fraction >= 0.0f && raw_fraction <= 1.0f) ? raw_fraction : 1.0f;
	const int32_t ratio_fp16 =
			static_cast<int32_t>(fraction * 65536.0f);
	const int band = health_color_band_fp16(ratio_fp16);
	const uint32_t fill = band == 0 ? layout_.tag_good
			: band == 1              ? layout_.tag_middle
									 : layout_.tag_bad;
	// The fill's width is a whole pixel count, the scaled bar width times the
	// 16.16 ratio rounded, and its clear runs from x0 + 1 to x0 + width
	// [orig: HUD_DrawHealthBar @0x5A2EC3..0x5A2EE3 the width, Render_FillViewportRect
	// @0x5A2F7A the clear].
	const int bar_w = static_cast<int>(x1 - x0);
	const int fill_px = static_cast<int>(
			(static_cast<int64_t>(bar_w) * ratio_fp16 + 0x8000) >> 16);
	if (fill_px > 1) {
		emit_rect(x0 + 1.0f, y0 + 1.0f, x0 + static_cast<float>(fill_px),
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
	const int ramp = layout_.alpha_fade_ramp_ticks;
	if (ramp <= 0) {
		return;
	}
	for (int i = 0; i < kStanceFrames; ++i) {
		if (!layout_.stance_texture_valid[static_cast<size_t>(i)]) {
			return;
		}
	}
	const int32_t q16 = stance_scale_q16(layout_.stance_frame_w[0], layout_.stance_frame_h[0]);
	if (q16 <= 0) {
		return;
	}
	const int base_alpha = layout_.alpha_fade_base_alpha;
	const int elapsed = state.ticks - stance_.stamp;
	const int cur_a = stance_current_alpha(elapsed, ramp, base_alpha);
	const int prev_a = stance_prev_alpha(elapsed, ramp);

	// The centring off frame 0's scaled size; a frame past 128 there draws nothing
	// [orig: @0x59a023..0x59a07e].
	const int scaled_w = stance_scaled_dim(layout_.stance_frame_w[0], q16);
	const int scaled_h = stance_scaled_dim(layout_.stance_frame_h[0], q16);
	if (scaled_w > 128 || scaled_h > 128) {
		return;
	}
	const int center_x = stance_center_axis(scaled_w);
	const int center_y = stance_center_axis(scaled_h);

	// Each record is a bordered quad of its own image scaled by the shared
	// factor, its texture window off its image size [orig:
	// HUD_DrawTexturedQuadWithBorder @0x590C40 (x, y, tex_w, tex_h, w, h, shader,
	// argb) at @0x59a173 / @0x59a21c / @0x59a2e3, tex the record's +8 / +0xC and
	// the quad (q16 * size + 0x8000) >> 16].
	auto emit_frame = [&](int idx, uint32_t argb) {
		const size_t r = static_cast<size_t>(idx);
		const float dx = static_cast<float>(layout_.stance_pos.x + layout_.stance_offset_x[r] + center_x);
		const float dy = static_cast<float>(layout_.stance_pos.y + layout_.stance_offset_y[r] + center_y);
		const float x0 = sx(dx, w);
		const float y0 = sy(dy, h);
		const float x1 = sx(dx + static_cast<float>(stance_scaled_dim(layout_.stance_frame_w[r], q16)), w);
		const float y1 = sy(dy + static_cast<float>(stance_scaled_dim(layout_.stance_frame_h[r], q16)), h);
		const BorderedQuadUv uv =
				bordered_quad_uv(layout_.stance_frame_w[r], layout_.stance_frame_h[r], x0, y0, x1, y1);
		emit_rect_uv(x0, y0, x1, y1, uv.u0, uv.v0, uv.u1, uv.v1, argb, kHudTexStance0 + idx);
	};
	// Under the frames, in a team session (a game type with 0x10000 and
	// without 0x20000) with both team tiles loaded, the HUD entity's team tile
	// in white: team 1 record 6, team 2 record 7 [orig: @0x59a084..0x59a0eb,
	// the call @0x59a173].
	if ((state.session.game_type & 0x10000u) != 0u && (state.session.game_type & 0x20000u) == 0u &&
			state.session.in_session && layout_.stance_texture_valid[6] &&
			layout_.stance_texture_valid[7] && (state.session.team == 1 || state.session.team == 2)) {
		emit_frame(state.session.team == 1 ? 6 : 7, 0xFFFFFFFFu);
	}
	if (stance_.cur >= 0 && stance_.cur < kStanceFrames) {
		emit_frame(stance_.cur, with_alpha(layout_.stance_tint, cur_a));
	}
	if (prev_a > 0 && stance_.prev != stance_.cur && stance_.prev >= 0 &&
			stance_.prev < kStanceFrames) {
		emit_frame(stance_.prev, with_alpha(layout_.stance_tint, prev_a));
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
		element_(HudElement::AmmoCount, [&] {
			emit_text(ammo_text.c_str(),
					static_cast<float>(layout_.ammo_count.x),
					static_cast<float>(layout_.ammo_count.y), w, h, wc,
					align_flags);
		});
	}
	if (wpngrp_visible && !state.weapon.display_name.empty() &&
			layout_.weapon_name.hidden == 0) {
		// [orig: @ 0x593b36..0x593bf5 — the 640-wide x nudge]
		const int nudge =
				weapon_name_x_nudge(w <= 640.0f, layout_.weapon_name.align);
		const uint32_t align_flags = layout_.weapon_name.align == 1
				? kFontAlignRight
				: (layout_.weapon_name.align == 2 ? kFontAlignCenter : 0u);
		element_(HudElement::WeaponName, [&] {
			emit_text(state.weapon.display_name.c_str(),
					static_cast<float>(layout_.weapon_name.x + nudge),
					static_cast<float>(layout_.weapon_name.y), w, h, wc,
					align_flags);
		});
	}
	if (wpngrp_visible) {
		element_(HudElement::ClipIndicator, [&] { element_clip_indicator(state, w, h); });
	}
	element_(HudElement::Targeting, [&] { element_targeting(state, w, h); });
	element_(HudElement::Crosshair, [&] { element_crosshair(state, w, h); });
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
	const int ramp = layout_.alpha_fade_ramp_ticks;
	if (ramp <= 0 || wep.reserve == -1 || wep.clip == -1) {
		return;
	}
	// The restamp key (D-HUD-5): any change of the def's ammo bucket, the
	// HUD reserve or the LOW BYTE of the def's ammo-class id restamps; two
	// defs agreeing on all three do not [orig: @0x599A90 `mov edi,[ecx+0DCh]`,
	// @0x599A96 vs g_HUDAmmoKeyBucket, @0x599A9E the reserve vs g_HUDAmmoKeyReserve,
	// @0x599AAC `cmp dl,[ecx+0D8h]` vs g_HUDAmmoKeyClass; the stores
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
	const int base_alpha = layout_.alpha_fade_base_alpha;
	const int max_alpha = layout_.alpha_fade_max_alpha;
	const int flash = fade_flash_alpha(state.ticks - flash_stamp_, ramp,
			base_alpha, max_alpha);

	const float ax = static_cast<float>(layout_.clip_pos.x);
	const float ay = static_cast<float>(layout_.clip_pos.y);
	if (wep.clip_texture_valid) {
		const float bx = ax + static_cast<float>(wep.clipgfx_offset_x);
		const float by = ay + static_cast<float>(wep.clipgfx_offset_y);
		// The background's alpha is ORed onto the stance colour, its own alpha
		// byte kept [orig: HUD_DrawAmmoIndicator @0x599B71, `(base << 24) |
		// dword_2723AE8`].
		emit_bordered_quad(bx, by, wep.clip_tex_w, wep.clip_tex_h,
				(static_cast<uint32_t>(base_alpha) << 24) | layout_.stance_tint, kHudTexClipGfx, w, h);
	}
	if (wep.round_texture_valid) {
		const int count = round_icon_count(wep.clip, wep.reserve, wep.capacity,
				wep.rounds_per_icon);
		float px = ax + static_cast<float>(wep.rndgfx_offset_x);
		float py = ay + static_cast<float>(wep.rndgfx_offset_y);
		for (int i = 0; i < count; ++i) {
			// [orig: @0x599BF7, the round icon a bordered quad of its image size]
			emit_bordered_quad(px, py, wep.round_tex_w, wep.round_tex_h,
					with_alpha(layout_.stance_tint, flash), kHudTexRoundGfx, w, h);
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
	// The anchor is an integer surface pixel, 1P the surface's centre pixel
	// (w / 2, h / 2) and 3P / spectate the projected aim point (the
	// projection's `>> 16` floors it), mapped into design units through the
	// rounding screen-to-design divide, as the target brackets map the same
	// anchor (hud_combat.cpp) [orig: HUD_DrawCrosshair -- the 1P centre
	// @0x592896..0x5928AE, the projected branch @0x592910..0x59293C
	// (HUD_ClipPointToFrustumAndProject, its sars @0x592566 / @0x59256B),
	// Viewport_ScreenToVirtual @0x592AD4 ahead of the five corner quads
	// @0x592C50..0x592CD2; Viewport_ScreenToVirtual @0x5D2C70].
	float cx = kDesignW * 0.5f;
	float cy = kDesignH * 0.5f;
	if (w > 0.0f && h > 0.0f) {
		const int32_t surface_w = static_cast<int32_t>(w), surface_h = static_cast<int32_t>(h);
		const int32_t px = state.aim_valid ? static_cast<int32_t>(std::floor(state.aim_screen_x))
										   : surface_w / 2;
		const int32_t py = state.aim_valid ? static_cast<int32_t>(std::floor(state.aim_screen_y))
										   : surface_h / 2;
		cx = static_cast<float>(screen_to_design_x(px, surface_w));
		cy = static_cast<float>(screen_to_design_y(py, surface_h));
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
	// "%d%" 15 output pixels above, all in the per-frame HUD colour
	// g_HUDFrameOverlayColor @0x840B1C (the outline's push @0x599955, the fill's
	// @0x599982, the text's @0x5999f4), which HUD_RenderAllOverlays stamps
	// = g_HUDColors[index] every frame @0x5A8112..0x5A8125 before it calls
	// HUD_RenderOverlays @0x5A84EB; 0xFF800F00 is only its static seed]
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
	const uint32_t color = active_color(state);
	const int32_t progress = power_throw_progress_fp16(state.windup_held_ticks);
	// The corner and the extent scale apart and add on the surface
	// [orig: Viewport_ScaleToVirtualCoords of (x, y) @0x599925, of (w, h)
	//  @0x599939; the corner sums @0x59996C..0x599973].
	const float x0 = sx(r.x, w);
	const float y0 = sy(r.y, h);
	const float x1 = x0 + sx(r.w, w);
	const float y1 = y0 + sy(r.h, h);
	emit_wire_rect(x0, y0, x1, y1, color);
	// The fill clears from x + 1 to x + span, no clamp: at full charge it abuts
	// the outline's right edge at x + w [orig: Render_DrawWireframeRect
	// @0x59997D; Render_FillViewportRect @0x5999A4, (x + 1, y + 1, x + span, y + h - 1)].
	const int span = power_fill_span(progress, static_cast<int>(x1 - x0));
	if (span > 1) {
		emit_rect(x0 + 1.0f, y0 + 1.0f, x0 + static_cast<float>(span), y1 - 1.0f, color, true);
	}
	char label[16];
	std::snprintf(label, sizeof(label), "%d%%",
			static_cast<int>((static_cast<int64_t>(progress) * 100) >> 16));
	// The "%d%%" label [orig: sprintf @0x5999ef], 15 OUTPUT pixels above the
	// bar's scaled corner [orig: sub 0Fh @0x5999da],
	// in the BOLD label slot at its own scale, not the hudpos HUD slot
	// [orig: HUD_DrawTextLeft_HalfBright(&g_HUDLabelFontBold, ..) @0x599a0f..0x599a14];
	// layout-only embedders fall back to the HUD slot.
	const bool have_bold = label_font_bold_.font() != nullptr;
	emit_slot_text(have_bold ? label_font_bold_ : font_, have_bold ? label_scale_ : hud_font_scale_,
			label, x0, sy(r.y, h) - 15.0f, half_bright_argb(color), 0u);
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

std::string frame_rate_overlay_text(const HudFrameRateOverlay &overlay) {
	char line[80];
	std::snprintf(line, sizeof(line), "FR %2i  MN %2i MX %2i CPU %d", overlay.average_fps,
			overlay.min_fps, overlay.max_fps, overlay.cpu_percent);
	return line;
}

void HudFrameCompiler::element_frame_rate_overlay(const HudFrameState &state, float w, float h) {
	// Under the cheat word's 0x400 bit, the line at design (10, 80) in the bold label slot, the
	// snapshot twin's colour half-bright (flags 1 selects nothing there).
	// [orig: HUD_DrawGameplayOverlays @0x5BDE60 - `test g_DebugCheatFlags, 400h` @0x5BDEB5,
	//  HUD_DrawFrameRateOverlay called @0x5BDEC2; HUD_DrawFrameRateOverlay @0x5BAE20 -
	//  g_HUDColors.active @0x5BAE57, flags 1 @0x5BAE5D, y 80 @0x5BAE65, x 10 @0x5BAE67,
	//  g_HUDLabelFontBold @0x5BAE69, HUD_DrawTextLeftScaled called @0x5BAE6E;
	//  HUD_DrawTextLeftScaled @0x580B40 - Viewport_ScaleToVirtualCoords called @0x580B4F,
	//  HUD_DrawTextLeft_HalfBright called @0x580B72; HUD_DrawTextLeft_HalfBright @0x5804C0 - the
	//  colour `sar eax, 1; and eax, 7F7F7Fh` @0x5804D8..0x5804DC, `or eax, 0FF000000h`
	//  @0x5804F0, the flags tested 100h @0x5804E1 and 2 @0x5804F5 alone]
	if (!state.frame_rate.shown) return;
	const std::string line = frame_rate_overlay_text(state.frame_rate);
	emit_slot_text(label_font_bold_, label_scale_, line.c_str(), sx(10.0f, w), sy(80.0f, h),
			half_bright_argb(hud_colors_active_), 0u);
	++draw_list_.elements_drawn;
}

std::string connect_status_text(const HudConnectStatus &status) {
	char line[160];
	std::snprintf(line, sizeof(line), "%s (%i)", status.label.c_str(), status.seconds);
	return line;
}

std::string start_timer_text(const HudStartTimer &timer) {
	char line[160];
	std::snprintf(line, sizeof(line), "%s %i:%02i", timer.label.c_str(), timer.seconds / 60,
			timer.seconds % 60);
	return line;
}

void HudFrameCompiler::element_connect_status(const HudFrameState &state, float w, float h) {
	// The witness is at hud_frame.h HudConnectStatus.
	if (!state.connect_status.shown) return;
	const std::string line = connect_status_text(state.connect_status);
	const HudPosRecord &pos = layout_.connect_status;
	const uint32_t align =
			pos.align == 1 ? kFontAlignRight : (pos.align == 2 ? kFontAlignCenter : 0u);
	emit_slot_text(label_font_bold_, label_scale_, line.c_str(), sx(static_cast<float>(pos.x), w),
			sy(static_cast<float>(pos.y), h), half_bright_argb(0xFFFFFFFFu), align);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_start_timer(const HudFrameState &state, float w, float h) {
	// The witness is at hud_frame.h HudStartTimer.
	if (state.start_timer.seconds == 0) return;
	const std::array<int, 4> &box = layout_.start_timer;
	emit_label_box(static_cast<float>(box[0]), static_cast<float>(box[1]),
			static_cast<float>(box[0] + box[2]), static_cast<float>(box[1] + box[3]), std::string(),
			0xFFFFFFFFu, w, h);
	// The box's corner and its size each through Viewport_ScaleToVirtualCoords.
	const int x = static_cast<int>(sx(static_cast<float>(box[0]), w));
	int y = static_cast<int>(sy(static_cast<float>(box[1]), h));
	const int box_w = static_cast<int>(sx(static_cast<float>(box[2]), w));
	const int box_h = static_cast<int>(sy(static_cast<float>(box[3]), h));
	const bool have_large = label_font_large_.font() != nullptr;
	const GameFont &large = have_large ? label_font_large_ : font_;
	// The fallback the slot's draws take (emit_slot_text): the HUD slot at its own scale.
	const float scale = have_large ? label_large_scale_ : hud_font_scale_;
	int zero_w = 0;
	int zero_h = 0;
	large.measure("0", scale, scale, &zero_w, &zero_h);
	const uint32_t color = half_bright_argb(hud_colors_active_);
	const float column = static_cast<float>(x + box_w / 2);
	y += box_h / 2 - zero_h;
	const std::string line = start_timer_text(state.start_timer);
	emit_slot_text(label_font_large_, label_large_scale_, line.c_str(), column,
			static_cast<float>(y), color, kFontAlignCenter);
	y += zero_h;
	emit_slot_text(label_font_large_, label_large_scale_, state.start_timer.footer.c_str(), column,
			static_cast<float>(y), color, kFontAlignCenter);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_breath_bar(const HudFrameState &state, float w, float h) {
	// [orig: HUD_DrawBreathBar @0x59D6F0..0x59D9C9, whose only caller is
	//  HUD_DrawGameplayOverlays @0x5BDED3, behind g_RoundOver == 0
	//  @0x5BDECA..0x5BDED1]
	if (state.round_over) {
		return;
	}
	// The BREATHTIME declutter slot, then a positive breathtime
	// [orig: @0x59D6F3, @0x59D70F]. The count forced to 1 under
	// g_DebugCheatFlags & 0x8000000 is a dead arm: nothing sets that bit.
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

namespace {

// The g_HUDColors.palette entries 0..5 as HUD_InitTeamColorTable writes them
// [orig: HUD_InitTeamColorTable @0x51F245..0x51F277].
constexpr uint32_t kHudSchemeTableAtInit[6] = {
		0xFFFFFFFFu, // 0 white
		0xFF00FF00u, // 1 green
		0xFF010101u, // 2 the placeholder the frame refresh overwrites with hud_textcolor
		0xFF80A0FFu, // 3 light blue
		0xFFF0F000u, // 4 yellow
		0xFFFF5050u, // 5 salmon
};

} // namespace

uint32_t HudFrameCompiler::hud_palette(int index) const {
	// [orig: HUD_InitTeamColorTable @0x51f240 — the scheme immediates; entry 2
	// is refreshed from g_HudposTextColor every frame @0x5a810c / @0x59ccb8]
	if (index < 0 || index > 5) index = 2;
	return index == 2 ? layout_.hud_text : kHudSchemeTableAtInit[index];
}

void HudFrameCompiler::sync_hud_colors_active(const HudFrameState &state) {
	// The snapshot twin's two non-frame writers: the HUD-init seed with the
	// forced alpha, and the hudcolor cycle's table[index] store, seen here as
	// the scheme index changing between frames [orig: HUD_InitTeamColorTable
	// @0x51F240 (the store @0x51F2DE); the cycle @0x49AFC7].
	// The seed reads the table as the init leaves it, entry 2 the 0xFF010101
	// placeholder: every MP mission load runs GameLoop_Init's
	// HUD_InitTeamColorTable, then Game_StartMission(0), whose HUD init copies
	// the hudpos text colour into entry 2 and the twin, and whose first-load
	// arm then runs HUD_InitTeamColorTable again, restoring both. Only the
	// gameplay overlay pass refreshes entry 2 and restamps the twin, so a host
	// that never draws it (the dedicated host's status page) keeps the seed,
	// and with the default index 2 its twin readers draw half-bright black.
	// [orig: GameLoop_Init @0x526370 (HUD_InitTeamColorTable, then
	//  Game_StartMission(0) @0x526377); Game_StartMission @0x524360
	//  (is_first_load = !is_restart @0x5243A6, HUD_InitOverlaySystem @0x525733,
	//  the first-load HUD_InitTeamColorTable @0x5258C1); HUD_InitOverlaySystem
	//  @0x5A4620 (entry 2 @0x5A4965, the twin @0x5A4977); the refresh
	//  HUD_RenderAllOverlays @0x5A810C and the restamp HUD_DrawGameTimerOverlay
	//  @0x59CCB8..0x59CCD0, both past the level-3 return @0x5A80C4]
	const int idx = state.hud_color_index >= 0 && state.hud_color_index <= 5
			? state.hud_color_index
			: 2;
	// The cycle stores the palette entry as it stands, entry 2 the placeholder until a
	// gameplay overlay pass has refreshed it (hud_palette2_refreshed_)
	// [orig: Input_HandleActionBinding @0x49AD40 - case 10 @0x49AFC7, `mov eax,
	//  g_HUDColors.palette[eax*4]` @0x49AFE0, the store @0x49AFE7].
	if (hud_colors_active_index_ < 0)
		hud_colors_active_ = kHudSchemeTableAtInit[idx] | 0xFF000000u;
	else if (idx != hud_colors_active_index_)
		hud_colors_active_ = idx == 2 && !hud_palette2_refreshed_ ? kHudSchemeTableAtInit[2]
		                                                          : hud_palette(idx);
	hud_colors_active_index_ = idx;
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
	// Only the HUDWPDINFO anchor scales; the label is laid out from it in
	// SCREEN pixels, every measure and every gap (4 px, the box's -2 / -1) a
	// pixel count [orig: HUD_DrawWaypointNameAndDistance -- the anchor
	// Viewport_ScaleToVirtualCoords @0x59488D, the distance's width
	// GameFont_MeasureTextWidth @0x5948AC, the name's width and height
	// HUD_MeasureTextWH @0x594A15 / @0x594A82 / @0x594ACB, the name at
	// dist_w + x + 4 @0x5949FC, the align-1 shift -4 - name_w + x @0x594AE0, the
	// align-2 shift x - 4 @0x594A8E, the box right dist_w + x + 4 @0x594AEB and
	// its rows y - 2 / y + text_h - 1 @0x594B0C].
	const float ax = sx(static_cast<float>(gp.x), w);
	const float ay = sy(static_cast<float>(gp.y), h);
	// The name's box measure at the slot's scale: its width and its height,
	// each truncated [orig: HUD_MeasureTextWH @0x580AB0].
	int name_w = 0;
	int name_h = 0;
	wf.measure(state.waypoint.name.c_str(), ws, ws, &name_w, &name_h);
	// The text through the half-bright drawers, alpha forced opaque; the box keeps
	// the raw colour [orig: HUD_DrawTextLeft_HalfBright @0x5949fc / @0x594a69,
	//  HUD_DrawTextRightAligned_HalfBright @0x594ab2 / @0x594b31; the wireframe
	//  box Render_DrawWireframeRect @0x594b0c].
	const uint32_t text_color = half_bright_argb(color);
	auto draw = [&](const char *text, float x, uint32_t flags) {
		emit_slot_text(label_font_bold_, label_scale_, text, x, ay, text_color, flags);
	};
	// The distance's width: the unscaled extent times the slot's scale, truncated
	// [orig: GameFont_MeasureTextWidth @0x580A50 -- CGameFont_GetTextExtent at
	// 1.0 @0x580A66, the multiply @0x580A6F, the truncation @0x580A76].
	int dist_extent = 0;
	wf.measure(dist, 1.0f, 1.0f, &dist_extent, nullptr);
	const float dist_w = static_cast<float>(static_cast<int>(static_cast<double>(dist_extent) * ws));
	const float text_h = static_cast<float>(name_h);
	float dist_x = ax;
	if (!state.waypoint.name.empty()) {
		switch (gp.align) {
			case 1:
				draw(state.waypoint.name.c_str(), ax, kFontAlignRight);
				dist_x = ax - 4.0f - static_cast<float>(name_w);
				break;
			case 2:
				draw(state.waypoint.name.c_str(), ax, 0u);
				dist_x = ax - 4.0f;
				break;
			default:
				draw(state.waypoint.name.c_str(), ax + dist_w + 4.0f, 0u);
				break;
		}
	}
	// Align 1 or 2 boxes the distance left of its anchor, any other to the
	// right [orig: @0x594A3E..0x594A47, @0x594AEB]. (Retail's no-name arm
	// @0x594A1D is dead: HUD_GetWaypointName @0x594630 always yields a string,
	// as the shipping presenter's label, waypoint_label_text, always is.)
	const bool left_box = gp.align == 1 || gp.align == 2;
	const float box_left = left_box ? dist_x - dist_w : dist_x;
	const float box_right = left_box ? dist_x + 4.0f : dist_x + dist_w + 4.0f;
	if (gp.hidden == 0) {
		emit_wire_rect(box_left, ay - 2.0f, box_right, ay + text_h - 1.0f, color);
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

// The panel pass draws this after its other windows, independently of gameplay declutter.
// [orig: HUD_DrawOverlayPanels @ 0x5C0060; HUD_DrawKillAnnounceBanner @ 0x59DC90]
void HudFrameCompiler::element_kill_announcement(const HudFrameState &state, float w, float h) {
	if (!state.kill_announcement.visible(static_cast<uint32_t>(state.ticks))) return;
	const bool large = label_font_large_.font() != nullptr;
	const GameFont &font = large ? label_font_large_ : font_;
	if (font.font() == nullptr) return;
	const float scale = large ? label_large_scale_ : hud_font_scale_;
	// White through the mode-2 drawer, which halves it keeping the alpha
	// [orig: Render_DrawTextScaled(.., -1, 2) @0x59dd33 -> sub_5D2FC0 mode 2 ->
	//  HUD_DrawTextHalfBrightF @0x580720].
	const auto run = font.layout(state.kill_announcement.text.c_str(), sx(512.0f, w),
			sy(30.0f, h), scale, scale, kFontAlignCenter, half_bright_keep_alpha(0xFFFFFFFFu));
	draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(), run.quads.end());
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_end_round_overlay(const HudFrameState &state,
		float w, float h) {
	// [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0] The stdbox over the
	// overlay safe area, then each resolved line centred on design x 512 in
	// the Impact38 slot, half-bright like every HUD text
	// [orig: HUD_DrawLabelBox(ctx, 8, top+8, 1015, bottom-8) @0x5b7d3e;
	//  HUD_DrawTextCenteredScaled -> Viewport_ScaleToVirtualCoords + HUD_DrawTextCentered_HalfBright].
	const HudEndRoundOverlayState &er = state.end_round;
	if (!er.shown) return;
	emit_stdbox(sx(8.0f, w), sy(static_cast<float>(er.top + 8), h),
			sx(1015.0f, w), sy(static_cast<float>(er.bottom - 8), h), w, 0xFFu, 0.0f);
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
	// The snapshot twin, drawn half-bright with the alpha forced: a dedicated
	// host's seed (0xFF010101 at the default index) draws black on its black
	// status page [orig: `mov ebp, g_HUDColors.active` @0x5B7CE6, pushed at each
	// HUD_DrawTextCenteredScaled (the first @0x5B7E86);
	// HUD_DrawTextCentered_HalfBright @0x580680 (`and eax, 7F7F7Fh` @0x58068F,
	// `or eax, 0FF000000h` @0x5806AB)].
	const uint32_t color = half_bright_argb(hud_colors_active_);
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
	// Both loops draw in the BOLD label slot at its own scale, not the hudpos
	// HUD slot [orig: push offset g_HUDLabelFontBold @0x59ae09 / @0x59aea6 into
	// HUD_DrawTextLeft_HalfBrightKeepAlpha @0x580560, which hands the slot's scale pair to
	// CGameFont_DrawText @0x5805b3..0x5805c0]; layout-only embedders fall back
	// to the HUD slot.
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : hud_font_scale_;
	if (bf.font() == nullptr) {
		return;
	}
	// The anchors and the 18-unit step scale to the surface ONCE, in integers,
	// and the rows add up in surface pixels [orig: Viewport_ScaleToVirtualCoords
	// of the anchor @0x59ad9f / @0x59ae52 and of the step alone, x null,
	// @0x59adb0; the add @0x59ae1a / @0x59aeb7].
	const int surface_w = static_cast<int>(w);
	const int surface_h = static_cast<int>(h);
	const int row_h = design_to_screen_y(kFeedLineStepDesign, surface_h);
	bool drew = false;

	// THE CHAT RING — the FIRST loop [orig: HUD_DrawConsoleMessages
	// @0x59ad30, the chat walk @0x59adbc..0x59ae2e]: anchored at HUDCHATTEXT
	// (dword_27237A8/AC), slots 3..1 walked top-down, each line's alpha the
	// fold `255 * timer / 186` clamped to 255 and SKIPPED (no rung consumed)
	// at <= 0 [orig: @0x59add9..0x59adf4], folded over the stored RGB
	// `(alpha << 24) + (color & 0xFFFFFF)` @0x59ae04. The timer is the slot's
	// remaining life, so the alpha ramps down through the line's last 186 ticks.
	{
		const int cx = design_to_screen_x(layout_.chat_text.present ? layout_.chat_text.x : 5,
				surface_w);
		int row_y = design_to_screen_y(layout_.chat_text.present ? layout_.chat_text.y : 5,
				surface_h);
		const int n = static_cast<int>(chat_lines_.size());
		const int first = std::max(0, n - kFeedVisibleRows);
		for (int i = first; i < n; ++i) {
			const HudMessageLine &line = chat_lines_[static_cast<size_t>(i)];
			const int remaining = line.expire_tick - static_cast<int>(state.main_frame);
			int alpha = 255 * remaining / kMessageExpiryStagger;
			if (alpha <= 0) continue;
			if (alpha > 255) alpha = 255;
			// The drawer halves the RGB and keeps the alpha [orig: HUD_DrawTextLeft_HalfBrightKeepAlpha
			// @0x59ae0e — (c & 0xFF000000) + ((c >> 1) & 0x7F7F7F)].
			emit_slot_text(bf, bscale, line.text.c_str(), static_cast<float>(cx),
					static_cast<float>(row_y),
					half_bright_keep_alpha((static_cast<uint32_t>(alpha) << 24) |
							(line.color & 0xFFFFFFu)),
					0u);
			row_y += row_h;
			drew = true;
		}
	}

	// THE SYSTEM RING — the second loop. Fallback = the JO-authored HUDSYSTEXT
	// anchor (hudpos.def "5 , 22").
	const int ax = design_to_screen_x(layout_.sys_text.present ? layout_.sys_text.x : 5, surface_w);
	const int ay = design_to_screen_y(layout_.sys_text.present ? layout_.sys_text.y : 22,
			surface_h);
	std::vector<const HudMessageLine *> live;
	for (const HudMessageLine &line : feed_lines_) {
		if (line.expire_tick > static_cast<int>(state.main_frame)) {
			live.push_back(&line);
		}
	}
	if (!live.empty()) {
		// The three most recent lines, oldest first so the oldest lands on the
		// anchor and newer lines step downward.
		const int visible = std::min(static_cast<int>(live.size()), kFeedVisibleRows);
		const int first = static_cast<int>(live.size()) - visible;
		int row_y = ay;
		for (int i = first; i < static_cast<int>(live.size()); ++i) {
			const HudMessageLine *line = live[static_cast<size_t>(i)];
			// The same halving drawer [orig: HUD_DrawTextLeft_HalfBrightKeepAlpha @0x59aeab].
			emit_slot_text(bf, bscale, line->text.c_str(), static_cast<float>(ax),
					static_cast<float>(row_y), half_bright_keep_alpha(line->color), 0u);
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
//    [orig: GfxShader_CreateTwoTexture(BoxTexA, BoxTexB, 0x651, 2) -> style+0x30 @0x56af3c,
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
//    HUD_StdboxDrawFillWrapTiled @0x56b5d0]. The compiler emits that one quad
//    on the lifted cell (kHudTexBoxFill, HudQuad::wrap); the device samples it
//    wrap-addressed.
//
// Every retail quad's diffuse is alpha<<24 | 0x7F7F7F — half-bright under the
// device's MODULATE2X stage, so 0.5 x 2 = 1 and the material lands at full
// texture brightness [orig: the shl/lea prologue @0x56b70e-0x56b713]. The
// quads carry that raw diffuse; the box material 0x651 runs the stage on the
// device (hud_texture_materials.h kBoxMaterialWord).
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
// rect. The cell's UV region is the one the style's load stored, the half
// texel baked in: u = (col * (W >> 2) + 0.5) / W .. ((col + 1) * (W >> 2) +
// 0.5) / W, v alike over H, each worked in double and stored as a float
// [orig: BoxTexture_LoadAndSetupUVRegions @0x56acd0 -- the regions rec+0x48
// .. rec+0x150 @0x56afe3..0x56b2f7], which HUD_DrawTexturedQuad_0 maps onto
// the piece as given [orig: @0x56b3fc..0x56b424]; so a piece drawn at its
// cell's size samples texel centres, never the neighbouring cell's edge.
// Cell (1,1), the MrClippy fill's region rec+0x160, steps its rows by W >> 2
// where every other region steps by H >> 2 [orig: @0x56b31d, @0x56b338] (the
// same on every shipped atlas, all square). The bottom row keeps only its top
// 90% of BOTH the destination and the source cell — cropped, not squashed
// [orig: the crop arm @0x56b454-0x56b470, flt_7C459C = 0.9; the bottom trio
// passes the flag @0x56bbc0/@0x56bc22/@0x56bc80].
void HudFrameCompiler::emit_stdbox_piece(float x0, float y0, float x1, float y1,
		int col, int row, bool crop_bottom, uint32_t color, int32_t texture, int tex_w,
		int tex_h) {
	if (x1 <= x0 || y1 <= y0 || tex_w <= 0 || tex_h <= 0) return;
	const int qw = tex_w >> 2;
	const int qh = tex_h >> 2;
	const int row_step = (col == 1 && row == 1) ? qw : qh;
	const auto region = [](int texels, int extent) {
		return static_cast<float>((static_cast<double>(texels) + 0.5) / static_cast<double>(extent));
	};
	const float u0 = region(col * qw, tex_w);
	const float u1 = region((col + 1) * qw, tex_w);
	const float v0 = region(row * row_step, tex_h);
	const float v1 = region(row * row_step + qh, tex_h);
	const float v1_drawn = crop_bottom ? (v1 - v0) * kBoxBottomCrop + v0 : v1;
	const float dh = crop_bottom ? (y1 - y0) * kBoxBottomCrop : (y1 - y0);
	emit_rect_uv(x0, y0, x1, y0 + dh, u0, v0, u1, v1_drawn, color, texture);
	// The combined material's second stage: the boxtile camo at its own
	// dims, screen-anchored [orig: HUD_DrawTexturedQuad_0 @0x56b3e0 with the
	// style's boxtile w/h @0x56b97a..0x56bcbb]. Only the stdbox style was
	// registered with a secondary texture; a style without one binds the
	// border alone [orig: BoxTexture_LoadAndSetupUVRegions @0x56af4e..0x56af51].
	if (texture == kHudTexBoxBorder && layout_.box_tile_w > 0 && layout_.box_tile_h > 0) {
		HudQuad &piece = draw_list_.quads.back();
		piece.texture2 = kHudTexBoxTile;
		piece.stage2_w = static_cast<float>(layout_.box_tile_w);
		piece.stage2_h = static_cast<float>(layout_.box_tile_h);
	}
}

void HudFrameCompiler::emit_stdbox(float x0, float y0, float x1, float y1,
		float surface_w, uint32_t alpha, float title_gap_w) {
	if (!layout_.box_texture_valid) return;
	if (x1 - x0 < 2.0f || y1 - y0 < 2.0f) return;
	// [orig: Render_HUDBoxOverlay @0x56b700 — alpha<<24 | 0x7F7F7F
	//  @0x56b70e..0x56b713]
	const uint32_t color = ((alpha & 0xFFu) << 24) | 0x7F7F7Fu;
	// One source cell is a quarter of the atlas (32 px for the shipped 128 px
	// border.tga) [orig: quarterW/H = dims >> 2 @0x56adb6/@0x56adbd].
	const float src_cell = static_cast<float>(layout_.box_tex_w) * 0.25f;
	if (src_cell <= 0.0f) return;
	// Piece size and the fill inset scale with the surface; the fill's tile
	// PERIOD does not [orig: s = surface_w * 0.000625 @0x51f02e].
	const float s = surface_w / kBoxScaleRef;
	const float cw = src_cell * s;
	const float ch = src_cell * s;

	// 1. The interior fill: ONE quad over the 16*s / 24*s inset rect, bound to
	// the lifted brush cell (kHudTexBoxFill), its UV the screen position over the
	// cell's own unscaled size with retail's half texel, (dest + 0.5) / cell; the
	// texture's wrap addressing tiles it on the absolute screen grid, so pixel i
	// samples the cell's texel i mod cell [orig: Render_HUDBoxOverlay @0x56B700 -
	// the fill material rec+0x34 bound @0x56B7B4..0x56B7B8, the insets
	// rec+0x180..+0x18C @0x56B7BD..0x56B80D, the cell w / h rec+0x148 / +0x14C
	// @0x56B811..0x56B821, the one call @0x56B824; HUD_StdboxDrawFillWrapTiled
	// @0x56B5D0 - 1 / cell @0x56B5E8 / @0x56B5FD, u = (x + 0.5) * (1 / cell_w),
	// v = (y + 0.5) * (1 / cell_h) at each vertex @0x56B690..0x56B6E9, one
	// 4-vertex strip GDynamicVB_DrawPrimitive(5, v, 4) @0x56B6F5].
	const float fx1 = x0 + kBoxFillInsetX * s;
	const float fy1 = y0 + kBoxFillInsetY * s;
	const float fx2 = x1 - kBoxFillInsetX * s;
	const float fy2 = y1 - kBoxFillInsetY * s;
	const int fill_cell_w = layout_.box_tex_w >> 2;
	const int fill_cell_h = layout_.box_tex_h >> 2;
	if (fx2 > fx1 && fy2 > fy1 && fill_cell_w > 0 && fill_cell_h > 0) {
		const double inv_w = 1.0 / static_cast<double>(fill_cell_w);
		const double inv_h = 1.0 / static_cast<double>(fill_cell_h);
		const auto wrap_uv = [](float pos, double inv) {
			return static_cast<float>((static_cast<double>(pos) + 0.5) * inv);
		};
		emit_rect_uv(fx1, fy1, fx2, fy2, wrap_uv(fx1, inv_w), wrap_uv(fy1, inv_h),
				wrap_uv(fx2, inv_w), wrap_uv(fy2, inv_h), color, kHudTexBoxFill);
		draw_list_.quads.back().wrap = true;
	}

	// 2. The border pieces, over the fill.
	emit_box_pieces(x0, y0, x1, y1, cw, ch, color, title_gap_w, kHudTexBoxBorder,
			layout_.box_tex_w, layout_.box_tex_h);
}

// The eight border pieces (and the titled top row) of any registered box
// style over its own atlas: the same piece pass for every style
// [orig: Render_HUDBoxOverlay @0x56b700, the piece calls @0x56b937..0x56bcbb].
void HudFrameCompiler::emit_box_pieces(float x0, float y0, float x1, float y1, float cw,
		float ch, uint32_t color, float title_gap_w, int32_t texture, int tex_w, int tex_h) {
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
		emit_stdbox_piece(x0, y0, x0 + cw, y0 + ch, 0, 3, false, color, texture, tex_w, tex_h);
		emit_stdbox_piece(x0 + cw, y0, bar_x2, y0 + ch, 1, 3, false, color, texture, tex_w, tex_h);
		emit_stdbox_piece(bar_x2, y0, cap_x2, y0 + ch, 2, 3, false, color, texture, tex_w, tex_h);
		if (ix2 > cap_x2)
			emit_stdbox_piece(cap_x2, y0, ix2, y0 + ch, 1, 0, false, color, texture, tex_w, tex_h);
	} else {
		emit_stdbox_piece(x0, y0, x0 + cw, y0 + ch, 0, 0, false, color, texture, tex_w, tex_h);   // TL
		if (ix2 > ix1)
			emit_stdbox_piece(ix1, y0, ix2, y0 + ch, 1, 0, false, color, texture, tex_w, tex_h);  // top
	}
	emit_stdbox_piece(ix2, y0, x1, y0 + ch, 2, 0, false, color, texture, tex_w, tex_h);       // TR
	emit_stdbox_piece(x0, iy2, x0 + cw, y1, 0, 2, true, color, texture, tex_w, tex_h);        // BL
	emit_stdbox_piece(ix2, iy2, x1, y1, 2, 2, true, color, texture, tex_w, tex_h);            // BR
	if (ix2 > ix1)
		emit_stdbox_piece(ix1, iy2, ix2, y1, 1, 2, true, color, texture, tex_w, tex_h);       // bottom
	if (iy2 > iy1) {
		emit_stdbox_piece(x0, iy1, x0 + cw, iy2, 0, 1, false, color, texture, tex_w, tex_h);  // left
		emit_stdbox_piece(ix2, iy1, x1, iy2, 2, 1, false, color, texture, tex_w, tex_h);      // right
	}
}

} // namespace opennova::hud
