// THE MP SESSION TEXT LINES: the GAMEINFO/ZONEINFO overlay with its per-frame
// g_HUDColors.active restamp, the CLOCK slot's game timer + player count, and
// the TEAMID line. Every input rides HudFrameState::session, filled by the
// role feed (inmatch/role_feeds.h hud_role_facts); the gametext strings are
// hud_game_text.h hud_session_text's.
// [orig: HUD_DrawGameTimerOverlay @0x59cc80; HUD_DrawGameTimer @0x593d40;
//  HUD_DrawScoreOverlay @0x593e50; HUD_DrawTeamIdLine @0x59aa30]

#include <runtime/hud/hud_frame.h>

#include <cstdio>
#include <string>

namespace opennova::hud {

namespace {

// One design coordinate onto the surface, in integers [orig:
// Viewport_ScaleToVirtualCoords @0x5d2b20 — (v * dim + extent/2) / extent,
// the divide truncating toward zero; HUD_DrawTextAtVirtualPos @0x5d3ec0 inlines
// the same pair].
int virtual_scale(int v, int surface, int extent) {
	return static_cast<int>((static_cast<int64_t>(v) * surface + extent / 2) / extent);
}

// The int32 wrap of retail's `imul` / `sub` chains.
int32_t wrap32(int64_t v) {
	return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v)));
}

std::string format_team_timer(int32_t remaining, const std::string &label, int32_t hold) {
	// "%2i:%02i %s (%i)" with the hold count when nonzero, else "%2i:%02i %s";
	// the minutes/seconds pair is the truncating /60 and its remainder
	// [orig: @0x59CEAE..0x59CEF2 / @0x59CEFC..0x59CF25 (team 1),
	//  @0x59CF66..0x59CFAD / @0x59CFB7..0x59CFE0 (team 2)]
	char buf[160];
	const int32_t minutes = remaining / 60;
	const int32_t seconds = wrap32(static_cast<int64_t>(remaining) - 60LL * minutes);
	if (hold != 0)
		std::snprintf(buf, sizeof(buf), "%2i:%02i %s (%i)", minutes, seconds, label.c_str(), hold);
	else
		std::snprintf(buf, sizeof(buf), "%2i:%02i %s", minutes, seconds, label.c_str());
	return buf;
}

} // namespace

void HudFrameCompiler::element_squad_orders(const HudFrameState &state, float w, float h) {
	// THE SQUAD ORDER LINES (a misnamed sub_59AEE0): off while the server
	// status view is toggled and above detail level 1, and NO declutter slot
	// of its own (no HUDDECLUT row hides them: the CHAT slot gates only the
	// console rings); the HUDORDERS anchor and the 18-px pitch each through
	// Viewport_ScaleToVirtualCoords; both 128-byte lines right-aligned in the
	// bold slot in palette[4] less one alpha step, which the half-bright
	// drawer halves and forces opaque; an empty line still takes its rung.
	// They hold until overwritten, cancelled with an empty line or cleared at
	// the mission start. The status view's gate is the overlay's: its page
	// replaces the scene frame's HUD whole.
	// [orig: sub_59AEE0 @0x59aee0 — `test dword_24C1930, 0x2000000`
	//  @0x59aee3 and the level `jge` @0x59aefb, nothing else before the anchor
	//  @0x59af0a..0x59af2a; the loop @0x59af30..0x59af68 over byte_2721DB8 up
	//  to g_HUDTrackedTarget, HUD_DrawTextRightAligned_HalfBright
	//  (g_HUDLabelFont[1], x, y, line, palette[4] - 0x1000000, 0x101)]
	if (state.hud_detail_level > 1) return;
	const HudPosRecord &anchor = layout_.squad_orders;
	const int x = virtual_scale(anchor.x, static_cast<int>(w), 1024);
	int y = virtual_scale(anchor.y, static_cast<int>(h), 768);
	const int pitch = virtual_scale(18, static_cast<int>(h), 768);
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bold = have_bold ? label_font_bold_ : font_;
	const float bold_scale = have_bold ? label_scale_ : hud_font_scale_;
	const uint32_t color = hud_palette(4) - 0x01000000u;
	for (const std::string &line : state.squad_orders) {
		if (!line.empty())
			emit_half_bright_text(bold, bold_scale, line.c_str(), static_cast<float>(x),
					static_cast<float>(y), color, 1);
		y += pitch;
	}
}

void HudFrameCompiler::emit_half_bright_text(const GameFont &slot, float slot_scale,
		const char *text, float screen_x, float screen_y, uint32_t argb, int mode) {
	// [orig: sub_5D2EA0 @0x5d2ea0 — 0 HUD_DrawTextLeft_HalfBright, 1
	//  HUD_DrawTextRightAligned_HalfBright, 2 HUD_DrawTextCentered_HalfBright,
	//  any other mode falls through]
	if (mode < 0 || mode > 2) return;
	const uint32_t flags = mode == 1 ? kFontAlignRight : (mode == 2 ? kFontAlignCenter : 0u);
	emit_slot_text(slot, slot_scale, text, screen_x, screen_y, half_bright_argb(argb), flags);
}

void HudFrameCompiler::emit_text_at_virtual_pos(const GameFont &slot, float slot_scale,
		const char *text, int design_x, int design_y, float w, float h, uint32_t argb, int mode) {
	// [orig: HUD_DrawTextAtVirtualPos @0x5d3ec0 — ((x * W + 512) / 1024,
	//  (y * H + 384) / 768) then sub_5D2EA0]
	const int sxp = virtual_scale(design_x, static_cast<int>(w), 1024);
	const int syp = virtual_scale(design_y, static_cast<int>(h), 768);
	emit_half_bright_text(slot, slot_scale, text, static_cast<float>(sxp),
			static_cast<float>(syp), argb, mode);
}

void HudFrameCompiler::emit_text_aligned(const GameFont &slot, float slot_scale, const char *text,
		int design_x, int design_y, float w, float h, uint32_t argb, int mode, uint32_t flags) {
	// [orig: HUD_DrawTextAligned @0x5d3f30 — the same integer pair — then
	//  HUD_DrawTextAligned_HalfBright @0x5d2f20: modes 0 (left) and 1 (right)
	//  pass the caller's flag word, whose 0x100 turns the inline tags off
	//  (HUD_DrawTextLeft_HalfBright @0x5804e1..0x5804e9); mode 2 centres
	//  without it; any other mode draws nothing]
	if (mode < 0 || mode > 2) return;
	const int sxp = virtual_scale(design_x, static_cast<int>(w), 1024);
	const int syp = virtual_scale(design_y, static_cast<int>(h), 768);
	uint32_t f = mode == 1 ? kFontAlignRight : (mode == 2 ? kFontAlignCenter : 0u);
	if (mode != 2 && (flags & kFontTagsDisabled) != 0u) f |= kFontTagsDisabled;
	emit_slot_text(slot, slot_scale, text, static_cast<float>(sxp), static_cast<float>(syp),
			half_bright_argb(argb), f);
}

void HudFrameCompiler::element_game_info(const HudFrameState &state, float w, float h) {
	// [orig: HUD_DrawGameTimerOverlay @0x59cc80, called unconditionally from
	//  HUD_RenderAllOverlays @0x5A8499]. The side effect runs FIRST and on every
	// path: palette[2] takes the hudpos text colour and the snapshot twin is
	// restamped palette[index] with NO alpha OR; the value it held before is the
	// colour the TKOTH timers draw in [orig: the read @0x59CC9B kept @0x59CCB4;
	// the restamp @0x59CCB8..0x59CCD0].
	const uint32_t prior_active = hud_colors_active_;
	hud_colors_active_ = hud_palette(hud_colors_active_index_);
	// GAMEINFO's hidden dword ends the drawer [orig: @0x59CC94 / @0x59CCD5].
	const HudPosRecord &gi = layout_.game_info;
	if (gi.hidden != 0) return;
	const HudSessionState &s = state.session;
	const uint32_t game_type = s.game_type;
	// "In the Zone": the TRGTCNT slot, KOTH or team KOTH, and the local player
	// inside a neutral hill (any positive coverage) [orig: dword_2723CC8
	// (slot 18) @0x59CCF1; the type tests @0x59CDD0..0x59CDDB; the dead debug
	// arm dword_24C1930 & 0x8000000; CaptureZone_FindMaxProximityCoverage
	// @0x59CDE8].
	bool drew = false;
	if (state.declutter_visible[kDeclutterTrgtCnt] && (game_type == 1u || game_type == 0x10001u) &&
			s.zone_coverage != 0) {
		// Blue while team 1's score1 leads team 2's, red while it trails, the
		// restamped twin on a tie — the two literals carry NO alpha, which the
		// half-bright drawer forces to FF [orig: @0x59CDF4..0x59CE11,
		// 0x4060FF / unk_FF2020].
		const int32_t diff = wrap32(static_cast<int64_t>(s.team_score1[0]) - s.team_score1[1]);
		const uint32_t color = diff > 0 ? 0x004060FFu : (diff < 0 ? 0x00FF2020u : hud_colors_active_);
		// Overlays/STROVER53 in the LARGE slot at ZONEINFO [orig: @0x59CE16..0x59CE46].
		const HudPosRecord &zi = layout_.zone_info;
		emit_text_at_virtual_pos(label_font_large_, label_large_scale_, s.text.in_the_zone.c_str(),
				zi.x, zi.y, w, h, color, zi.align);
		drew = true;
	}
	// The line pitch: the bold '0' height, a SCREEN measure, back to design
	// through the X axis [orig: GameFont_MeasureCharHeight('0', bold) @0x59CE65
	// into both outX and outY; Viewport_ScreenToVirtual @0x59CE81; the y step
	// reads outX @0x59CF60].
	if (game_type == 0x10001u) {
		const bool have_bold = label_font_bold_.font() != nullptr;
		const GameFont &bold = have_bold ? label_font_bold_ : font_;
		const float bold_scale = have_bold ? label_scale_ : hud_font_scale_;
		const int char_h = static_cast<int>(bold.char_height('0', bold_scale));
		const int step = screen_to_design_x(char_h, static_cast<int32_t>(w));
		// T is the time limit in minutes; each team's remaining seconds is
		// 60*T less its score1 [orig: `edi = ebx*60` @0x59CE95..0x59CEA4;
		// dword_A85AFC / dword_A85B0C].
		const int32_t total = wrap32(60LL * s.time_limit_minutes);
		const int32_t rem1 = wrap32(static_cast<int64_t>(total) - s.team_score1[0]);
		const int32_t rem2 = wrap32(static_cast<int64_t>(total) - s.team_score1[1]);
		const std::string line1 = format_team_timer(rem1, s.text.team_names[1], s.team_koth[0]);
		const std::string line2 = format_team_timer(rem2, s.text.team_names[2], s.team_koth[1]);
		// Both lines bold at GAMEINFO in its alignment, the second one step
		// lower [orig: @0x59CF2D..0x59CF4F, @0x59CFE8..0x59D007].
		emit_text_at_virtual_pos(label_font_bold_, label_scale_, line1.c_str(), gi.x, gi.y, w, h,
				prior_active, gi.align);
		emit_text_at_virtual_pos(label_font_bold_, label_scale_, line2.c_str(), gi.x, gi.y + step,
				w, h, prior_active, gi.align);
		drew = true;
	}
	if (drew) ++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_clock(const HudFrameState &state, float w, float h) {
	// THE CLOCK SLOT: the game timer then the player count, drawn from BOTH
	// dispatcher arms — the normal walk and the death-screen arm
	// [orig: HUD_RenderOverlays `dword_2723CD4` @0x5A7D75 / @0x5A7C53 ->
	//  HUD_DrawGameTimer @0x593d40, HUD_DrawScoreOverlay @0x593e50].
	if (!state.declutter_visible[kDeclutterClock]) return;
	const HudSessionState &s = state.session;
	if (!s.in_session) return; // both drawers' own gate [orig: @0x593D4E / @0x593E71]
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bold = have_bold ? label_font_bold_ : font_;
	const float bold_scale = have_bold ? label_scale_ : hud_font_scale_;
	// g_HUDFrameOverlayColor [orig: @0x593E18 / @0x593F58..0x594036].
	const uint32_t frame = active_color(state);
	const int surface_w = static_cast<int>(w);
	const int surface_h = static_cast<int>(h);
	bool drew = false;
	// The timer: HUDTIMECLOCK's x, y, always right-aligned, while the round
	// clock is not the untimed -1 [orig: @0x593D76..0x593D99]. Hours and
	// minutes from the truncated minute count, seconds from the second count
	// [orig: @0x593DC4..0x593DF5; "%s %i:%02i:%02i" @0x593E13 with
	// Overlays/STROVER50].
	if (s.round_time_remaining >= 0) {
		const int32_t r = s.round_time_remaining;
		const int32_t total_minutes = r / 62 / 60;
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%s %i:%02i:%02i", s.text.timer.c_str(),
				total_minutes / 60, total_minutes % 60, r / 62 % 60);
		emit_half_bright_text(label_font_bold_, label_scale_, buf,
				static_cast<float>(virtual_scale(layout_.time_clock.x, surface_w, 1024)),
				static_cast<float>(virtual_scale(layout_.time_clock.y, surface_h, 768)), frame, 1);
		drew = true;
	}
	// The player count: HUDPLAYERCOUNT's hidden dword gates it; permanent death
	// counts the live players, else the rows less the spectators; the
	// spectator line follows one bold '0' height lower, only while any exist
	// [orig: HUD_DrawScoreOverlay — `cmp dword_272366C, 0` @0x593E64, the
	// lines @0x593EB7..0x593F3B; the pitch GameFont_MeasureCharHeight
	// @0x593E91; the three align arms @0x593F4D..0x594052].
	const HudPosRecord &pc = layout_.player_count;
	if (pc.hidden == 0) {
		char primary[128];
		if (s.permanent_death)
			std::snprintf(primary, sizeof(primary), "%s %i", s.text.players_remaining.c_str(),
					s.remaining_count);
		else
			std::snprintf(primary, sizeof(primary), "%s %i", s.text.players.c_str(),
					s.row_count - s.spectator_count);
		char secondary[128];
		std::snprintf(secondary, sizeof(secondary), "%s %i", s.text.spectators.c_str(),
				s.spectator_count);
		const int line_h = static_cast<int>(bold.char_height('0', bold_scale));
		const int px = virtual_scale(pc.x, surface_w, 1024);
		const int py = virtual_scale(pc.y, surface_h, 768);
		// Align 1 right, 2 centred, anything else left [orig: `cmp dword_2723670, 1`
		// @0x593F4D, `, 2` @0x593F56, the left fall-through @0x593F73].
		const int mode = pc.align == 1 ? 1 : (pc.align == 2 ? 2 : 0);
		emit_half_bright_text(label_font_bold_, label_scale_, primary, static_cast<float>(px),
				static_cast<float>(py), frame, mode);
		if (s.spectator_count != 0)
			emit_half_bright_text(label_font_bold_, label_scale_, secondary, static_cast<float>(px),
					static_cast<float>(py + line_h), frame, mode);
		drew = true;
	}
	if (drew) ++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_team_id_line(const HudFrameState &state, float w, float h) {
	// [orig: HUD_DrawTeamIdLine @0x59aa30 — the TEAMID slot (dword_2723CCC)
	//  gates both call sites, the dispatcher @0x5A7DE7 and the death screen's
	//  spectate arm @0x5A7C10; the non-spectate death arm draws the waypoint
	//  label instead @0x5A7C47].
	if (!state.declutter_visible[kDeclutterTeamId]) return;
	const HudSessionState &s = state.session;
	const bool death = state.combat.death_screen;
	if (death && !s.spectating) return;
	const HudPosRecord &t = layout_.team_xy;
	if (t.hidden != 0) return; // [orig: `cmp dword_2723834, 0` @0x59AA44]
	// Team modes in a session only [orig: `test g_GameType, 10000h` @0x59AAB9,
	// `is_in_session` @0x59AAC6].
	if ((s.game_type & 0x10000u) == 0u || !s.in_session) return;
	// The death screen hangs the line 20 above the health bar's top edge
	// [orig: `dword_27237CC - 20` @0x59AA7A, HUDHEALTH's y1].
	const int design_y = death ? static_cast<int>(layout_.health_rect.y) - 20 : t.y;
	const int x = virtual_scale(t.x, static_cast<int>(w), 1024);
	const int y = virtual_scale(design_y, static_cast<int>(h), 768);
	// The team byte picks the name and the colour: 0 the snapshot twin
	// [orig: `active = g_HUDColors.active` @0x59AB0E], 1 palette[3], 2
	// palette[5], 3 yellow, 4 violet, anything else keeps the frame overlay
	// colour [orig: the switch @0x59AADC..0x59ABFC — -256 and -64897 are
	// 0xFFFFFF00 / 0xFFFF027F].
	uint32_t color = active_color(state); // g_HUDFrameOverlayColor @0x59AA4C
	int name_index = 5;
	switch (s.team) {
		case 0: name_index = 0; color = hud_colors_active_; break;
		case 1: name_index = 1; color = hud_palette(3); break;
		case 2: name_index = 2; color = hud_palette(5); break;
		case 3: name_index = 3; color = 0xFFFFFF00u; break;
		case 4: name_index = 4; color = 0xFFFF027Fu; break;
		default: break;
	}
	std::string text = s.text.team_names[static_cast<size_t>(name_index)];
	// The bold measure's height lifts the spectated name above the line; an
	// empty name measures nothing and leaves 0 [orig: the strlen gate
	// @0x59ABFE, HUD_MeasureTextWH(.., &g_HUDLabelFont[1], ..) @0x59AC26].
	int text_h = 0;
	if (!text.empty()) {
		const bool have_bold = label_font_bold_.font() != nullptr;
		const GameFont &bold = have_bold ? label_font_bold_ : font_;
		const float bold_scale = have_bold ? label_scale_ : hud_font_scale_;
		int text_w = 0;
		bold.measure(text.c_str(), bold_scale, bold_scale, &text_w, &text_h);
	}
	// The spectated player's name, first [orig: sub_5D2EA0(.., dword_A860F4 + 244,
	// ..) @0x59AC64].
	if (death)
		emit_half_bright_text(label_font_bold_, label_scale_, s.spectated_name.c_str(),
				static_cast<float>(x), static_cast<float>(y - text_h), color, t.align);
	// Attack & Defend appends the side's role [orig: `cmp g_GameType, 10002h`
	// @0x59AC76; dword_B78FE8 & 2 -> strcli20 @0x59AC86, & 1 -> strcli21
	// @0x59AC96; the strcat @0x59ACA0..0x59ACC9].
	if (s.game_type == 0x10002u) {
		if ((s.attack_defend & 2u) != 0u) text += s.text.attacking;
		else if ((s.attack_defend & 1u) != 0u) text += s.text.defending;
	}
	emit_half_bright_text(label_font_bold_, label_scale_, text.c_str(), static_cast<float>(x),
			static_cast<float>(y), color, t.align);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
