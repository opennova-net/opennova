// THE SERVER-STATUS PAGE compile (hud_server_status.h), the quit dialog it
// shares with the scene frame's gameplay overlays, the console lines and the
// player score list.
// [orig: Server_DrawStatusScreen @0x50a2d0; UI_DrawDisconnectReasonDialog
//  @0x5b8eb0; HUD_DrawServerConsoleLines (ex
//  Vehicle_ComputeSuspensionBoneTransforms) @0x5ba0a0;
//  HUD_DrawPlayerScoreList @0x500300]

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_game_text.h> // hud_sprintf

#include <cstdio>
#include <string>

namespace opennova::hud {

namespace {

// Every page line rides the bold label slot [orig: &g_HUDLabelFontBold at
// each draw of Server_DrawStatusScreen]; the colour -1 draws half-bright.
constexpr uint32_t kPageWhite = 0xFFFFFFFFu;

} // namespace

void HudFrameCompiler::element_quit_dialog(const HudFrameState &state, float w, float h) {
	if (!state.quit_dialog_open) return;
	// The untitled stdbox, then the text centred in the Impact38 slot in
	// g_HUDColors.active [orig: HUD_DrawLabelBox(ctx, 280, 340, 744, 428, 0,
	//  -1) @0x5b8ed4; the key pick @0x5b8edc..0x5b8eff; HUD_DrawTextCenteredScaled
	//  (&g_HUDLabelFontImpact38, 512, 364, text, active, 1) @0x5b8f21 — the
	//  Viewport_ScaleToVirtualCoords pair @0x580b80 then the centred
	//  half-bright draw].
	emit_label_box(280.0f, 340.0f, 744.0f, 428.0f, std::string(), 0xFFFFFFFFu, w, h);
	const bool have_impact = label_font_impact38_.font() != nullptr;
	const bool have_large = label_font_large_.font() != nullptr;
	const GameFont &lf = have_impact ? label_font_impact38_
			: have_large ? label_font_large_ : label_font_bold_;
	const float ls = (have_impact || have_large) ? label_large_scale_ : label_scale_;
	emit_text_at_virtual_pos(lf.font() != nullptr ? lf : font_, lf.font() != nullptr ? ls : 1.0f,
			state.quit_dialog_text.c_str(), 512, 364, w, h, active_color(state), 2);
	++draw_list_.elements_drawn;
}

void HudFrameCompiler::element_server_console_lines(bool mp_session_peer, float w, float h) {
	// The four newest raw lines, oldest at the top, from y 608 in steps of 24
	// at x 10: a peer's SYSTEM ring with the inline tags on, a dedicated
	// host's CHAT ring with them off. An empty slot leaves its row blank; a
	// zero colour is rewritten to -1 in the ring before the draw.
	// [orig: HUD_DrawServerConsoleLines @0x5ba0a0 — called (ctx, 10, 512, 600)
	//  @0x50b209; y = 600 + 8 @0x5ba0b1; is_mp_session_peer @0x5ba0bb; the
	//  SYSTEM raw slots 3..0 from unk_B41438 @0x5ba117 (flags 0), the CHAT
	//  raw slots 3..0 from unk_B3EC30 @0x5ba0bd (flags 256); the colour
	//  write-back @0x5ba0de / @0x5ba12e; `+= 24` @0x5ba107 / @0x5ba154]
	std::vector<HudMessageLine> &ring = mp_session_peer ? feed_lines_ : chat_raw_lines_;
	const uint32_t flags = mp_session_peer ? 0u : kFontTagsDisabled;
	const int count = static_cast<int>(ring.size());
	int y = 608;
	for (int slot = 3; slot >= 0; --slot, y += 24) {
		const int index = count - 1 - slot; // raw slot 0 is the newest line
		if (index < 0) continue;
		HudMessageLine &line = ring[static_cast<size_t>(index)];
		if (line.text.empty()) continue;
		if (line.color == 0u) line.color = 0xFFFFFFFFu;
		emit_text_aligned(label_font_bold_, label_scale_, line.text.c_str(), 10, y, w, h,
				line.color, 0, flags);
	}
}

void HudFrameCompiler::element_player_score_list(const ServerStatusPageState &page, float w,
		float h) {
	if (!page.score_list_open) return;
	// [orig: HUD_DrawPlayerScoreList @0x500300 — the STRSRV23 stdbox
	//  (100, 100)-(924, 668) in white @0x500347, then one row per slot below
	//  the capacity @0x500370..0x500553 through HUD_DrawTextAtVirtualPos
	//  mode 0 in the bold slot @0x50053c]
	emit_label_box(100.0f, 100.0f, 924.0f, 668.0f, page.text.score_list_title, 0xFFFFFFFFu, w,
			h);
	for (int i = 0; i < page.capacity; ++i) {
		const ServerStatusScoreRow row = server_status_score_row(page, i);
		if (!row.drawn) continue;
		emit_text_at_virtual_pos(label_font_bold_, label_scale_, row.text.c_str(), row.x, row.y,
				w, h, row.color, 0);
	}
	++draw_list_.elements_drawn;
}

bool HudFrameCompiler::compile_server_status_page(const HudFrameState &state,
		const ServerStatusPageState &page, uint32_t now_ms, bool window_active, float surface_w,
		float surface_h) {
	if (!server_status_page_due(&status_last_draw_ms_, now_ms, window_active)) return false;
	const float w = surface_w;
	const float h = surface_h;
	draw_list_ = HudDrawList{};
	draw_list_.top_begin = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
	// The page clears the whole frame to black first [orig:
	// CGfxDevice_SetClearColor(0) @0x50a376, CGfxDevice_Clear @0x50a393].
	emit_rect(0.0f, 0.0f, w, h, 0xFF000000u, true);

	const GameFont &bold = label_font_bold_;
	const float bs = label_scale_;
	char line[512];

	// THE ROSTER [orig: @0x50a3ec..0x50a7d5]
	const ServerStatusRosterGrid grid = server_status_roster_grid(page.capacity);
	uint32_t color = kPageWhite; // [orig: `v10 = -1` @0x50a3d3]
	int x = 7;
	int base = 0;
	for (int column = 0; column < grid.columns; ++column) {
		int y = 52;
		for (int row = 0; row < grid.rows; ++row) {
			const int slot_index = page.mp_session_peer ? base + row : base + row + 1;
			const ServerStatusRosterCell cell = server_status_roster_cell(page, slot_index, color);
			color = cell.color;
			// [orig: HUD_DrawTextAligned(ctx, x, y, 0, text, bold, colour, 0,
			//  256) @0x50a79b]
			emit_text_aligned(bold, bs, cell.text.c_str(), x, y, w, h, color, 0,
					kFontTagsDisabled);
			y += grid.row_step;
		}
		x += grid.column_step;
		base += grid.rows;
	}

	// THE SERVER LINE, centred at (512, 10): on NovaWorld STRSRV02 with the
	// session key, else STRSRV03 [orig: the abbreviation sprintf'd as a
	// format @0x50a3e4; "%s %s [%s] (%i)%s" @0x50a82a with an empty tail
	// @0x50a7f2; "%s %s [%s]" @0x50a85e; the draw mode 2 @0x50a882]
	const std::string abbreviation = hud_sprintf(page.text.game_type_abbreviation);
	if (page.novaworld) {
		std::snprintf(line, sizeof(line), "%s %s [%s] (%i)%s", page.text.server_novaworld.c_str(),
				page.server_name.c_str(), abbreviation.c_str(),
				static_cast<int>(page.session_key), "");
	} else {
		std::snprintf(line, sizeof(line), "%s %s [%s]", page.text.server_lan.c_str(),
				page.server_name.c_str(), abbreviation.c_str());
	}
	emit_text_at_virtual_pos(bold, bs, line, 512, 10, w, h, kPageWhite, 2);

	// THE TEAM BLOCK at x 800 for Team Deathmatch, CTF and Team KOTH: the
	// round wins by side (team 2 first) and the ties, then the current
	// scores — the team records' points, flag captures, or hold times
	// [orig: @0x50a88f..0x50af10; the team 2 / team 1 records g_TeamRecords[2]
	//  / [1]; STRSRV05 labels team 2, STRSRV06 team 1]
	const uint32_t gt = page.game_type;
	if (gt == 0x10000u || gt == 0x10004u || gt == 0x10001u) {
		const auto draw = [&](int y) { emit_text_at_virtual_pos(bold, bs, line, 800, y, w, h, kPageWhite, 0); };
		std::snprintf(line, sizeof(line), "%s", hud_sprintf(page.text.team_wins).c_str());
		draw(100);
		std::snprintf(line, sizeof(line), "  %2i %s", page.round_wins_team2, page.text.team2.c_str());
		draw(130);
		std::snprintf(line, sizeof(line), "  %2i %s", page.round_wins_team1, page.text.team1.c_str());
		draw(160);
		std::snprintf(line, sizeof(line), "  %2i %s",
				page.rounds_played - page.round_wins_team1 - page.round_wins_team2,
				page.text.ties.c_str());
		draw(190);
		std::snprintf(line, sizeof(line), "%s", hud_sprintf(page.text.team_scores).c_str());
		draw(230);
		if (gt == 0x10001u) {
			// [orig: "  %2i:%02i %s" over unknown_040[272] @0x50ae3c..0x50aeec]
			const int32_t t2 = page.team_hold_seconds[1];
			const int32_t t1 = page.team_hold_seconds[0];
			std::snprintf(line, sizeof(line), "  %2i:%02i %s", t2 / 60, t2 - 60 * (t2 / 60),
					page.text.team2.c_str());
			draw(260);
			std::snprintf(line, sizeof(line), "  %2i:%02i %s", t1 / 60, t1 - 60 * (t1 / 60),
					page.text.team1.c_str());
			draw(290);
		} else {
			// Points for Team Deathmatch (field 0x1C), flag captures for CTF
			// (field 0x0B) [orig: @0x50aa06 / @0x50ac16].
			const bool ctf = gt == 0x10004u;
			std::snprintf(line, sizeof(line), "  %2i %s",
					ctf ? page.team_flag_captures[1] : page.team_points[1], page.text.team2.c_str());
			draw(260);
			std::snprintf(line, sizeof(line), "  %2i %s",
					ctf ? page.team_flag_captures[0] : page.team_points[0], page.text.team1.c_str());
			draw(290);
		}
	}

	// THE ROUND CLOCK, right-aligned at (1000, 10) [orig: @0x50af20..0x50afa7,
	//  "%i:%02i:%02i" of ticks / 62]
	if (page.round_time_remaining >= 0) {
		const int32_t s = page.round_time_remaining / 62;
		std::snprintf(line, sizeof(line), "%i:%02i:%02i", s / 60 / 60, s / 60 % 60, s % 60);
		emit_text_at_virtual_pos(bold, bs, line, 1000, 10, w, h, kPageWhite, 1);
	}

	// THE BOTTOM ROW at y 704 [orig: @0x50afb7..0x50b19c]
	if (page.frames <= 60)
		std::snprintf(line, sizeof(line), "%s %i", page.text.frames.c_str(), page.frames);
	else
		std::snprintf(line, sizeof(line), "%s 63+", page.text.frames.c_str());
	emit_text_at_virtual_pos(bold, bs, line, 16, 704, w, h, kPageWhite, 0);
	std::snprintf(line, sizeof(line), "%s %i%%", page.text.cpu.c_str(), page.cpu_percent);
	emit_text_at_virtual_pos(bold, bs, line, 200, 704, w, h, kPageWhite, 0);
	std::snprintf(line, sizeof(line), "%s %i:%02i ", page.text.start_timer.c_str(),
			static_cast<int>(page.pre_round_delay / 60u), static_cast<int>(page.pre_round_delay % 60u));
	emit_text_at_virtual_pos(bold, bs, line, 320, 704, w, h, kPageWhite, 0);
	std::snprintf(line, sizeof(line), "%s %i", page.text.total_logins.c_str(),
			static_cast<int>(page.total_logins));
	emit_text_at_virtual_pos(bold, bs, line, 512, 704, w, h, kPageWhite, 0);
	// The current logins: slots in use that are not the host's own
	// [orig: the walk @0x50b12b..0x50b158 — `+4 && !+5`].
	int current = 0;
	for (int i = 0; i < page.capacity && static_cast<size_t>(i) < page.slots.size(); ++i) {
		const ServerStatusSlot &slot = page.slots[static_cast<size_t>(i)];
		if (slot.active && !slot.local) ++current;
	}
	std::snprintf(line, sizeof(line), "%s %i", page.text.current_logins.c_str(), current);
	emit_text_at_virtual_pos(bold, bs, line, 700, 704, w, h, kPageWhite, 0);

	// THE TICKER: a "." bouncing between x 700 and 780 at y 736, one step per
	// drawn page [orig: @0x50b1a1..0x50b203 — counter byte_24C10A0+0x24,
	//  `5c + 700` under 16, `5 * (172 - c)` from 16, `(c + 1) & 31`]
	const int tick_x = status_ticker_ < 16 ? 5 * status_ticker_ + 700 : 5 * (172 - status_ticker_);
	emit_text_at_virtual_pos(bold, bs, ".", tick_x, 736, w, h, kPageWhite, 0);
	status_ticker_ = (status_ticker_ + 1) & 0x1F;

	element_server_console_lines(page.mp_session_peer, w, h);
	// The windows draw in the page's order, each box over what came before
	// it (an order break per window: the renderer groups by kind only
	// within a run).
	mark_order_break();
	// The message log, else the class roster of the console capture
	// [orig: @0x50b218..0x50b23d]. The console capture (g_InputCaptureMode 1
	// with dword_B3E318) has no caller that opens it (hud_chat_entry.h
	// kChatDispatchConsole), so HUD_DrawClassRosterOverlay @0x5b9f30 never
	// draws here; not ported.
	element_message_log(state, w, h);
	mark_order_break();
	// The chat input line at (50, 480) while a capture is open
	// [orig: Input_GetCaptureMode @0x50b245, HUD_DrawChatInputLine @0x50b25a].
	element_chat_input(state, w, h, 480.0f);
	mark_order_break();
	element_quit_dialog(state, w, h); // [orig: @0x50b269..0x50b270]
	mark_order_break();
	element_player_score_list(page, w, h); // [orig: @0x50b27f..0x50b281]
	mark_order_break();
	// The end-round transition under the spawn gate, but not in Co-op
	// [orig: @0x50b28d..0x50b2a3 — `(g_GameType & 0xFFFDFFFF) != 0x10020`,
	//  UI_ProcessEndRoundScreenTransition @0x5b8600, whose overlay is
	//  HudFrameState::end_round].
	if (state.spawn_success_gate && (gt & 0xFFFDFFFFu) != 0x10020u)
		element_end_round_overlay(state, w, h);
	// CNetQuality_DrawIndicators(&g_NetQuality, 1) @0x50b2af draws the
	// net-quality icons at their fixed slot; the indicator drawer is not
	// ported for either of its callers (docs/interface/hud-re.md D-HUD-37).
	status_page_list_ = draw_list_;
	return true;
}

} // namespace opennova::hud
