// THE SERVER-STATUS PAGE compile (hud_server_status.h), the quit dialog it
// shares with the scene frame's gameplay overlays, the console lines and the
// player score list.
// [orig: Server_DrawStatusScreen @0x50a2d0; UI_DrawDisconnectReasonDialog
//  @0x5b8eb0; HUD_DrawServerConsoleLines (ex
//  Vehicle_ComputeSuspensionBoneTransforms) @0x5ba0a0;
//  HUD_DrawPlayerScoreList @0x500300]

#include <runtime/hud/hud_frame.h>
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

void HudFrameCompiler::element_server_console_lines(const ServerStatusPageState &page, float w,
		float h) {
	// The four newest raw lines, oldest at the top, from y 608 in steps of 24
	// at x 10: a peer's SYSTEM ring with the inline tags on, a dedicated
	// host's CHAT ring with them off. An empty slot leaves its row blank; a
	// zero colour is rewritten to -1 in the ring before the draw.
	// [orig: HUD_DrawServerConsoleLines @0x5ba0a0 — called (ctx, 10, 512, 600)
	//  @0x50b209; y = 600 + 8 @0x5ba0b1; is_mp_session_peer @0x5ba0bb; the
	//  SYSTEM raw slots 3..0 from unk_B41438 @0x5ba117 (flags 0), the CHAT
	//  raw slots 3..0 from unk_B3EC30 @0x5ba0bd (flags 256); the colour
	//  write-back @0x5ba0de / @0x5ba12e; `+= 24` @0x5ba107 / @0x5ba154]
	const bool mp_session_peer = page.mp_session_peer;
	const uint32_t flags = mp_session_peer ? 0u : kFontTagsDisabled;
	if (!mp_session_peer && !page.console_rows.empty()) {
		// A host with no client of its own: the feed carries its CHAT ring's
		// rows (inmatch/server_console.h), oldest first.
		int y = 608;
		for (const ServerStatusConsoleRow &row : page.console_rows) {
			if (!row.text.empty())
				emit_text_aligned(label_font_bold_, label_scale_, row.text.c_str(), 10, y, w, h,
						row.color, 0, flags);
			y += 24;
		}
		return;
	}
	std::vector<HudMessageLine> &ring = mp_session_peer ? feed_lines_ : chat_raw_lines_;
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

	// THE SERVER LINE, THE TEAM BLOCK, THE ROUND CLOCK and THE BOTTOM ROW, in
	// the page's draw order (hud_server_status.h carries each line's witness).
	const auto draw_line = [&](const ServerStatusTextLine &l) {
		emit_text_at_virtual_pos(bold, bs, l.text.c_str(), l.x, l.y, w, h, kPageWhite, l.align);
	};
	draw_line(server_status_server_line(page));
	for (const ServerStatusTextLine &l : server_status_team_block(page)) draw_line(l);
	ServerStatusTextLine clock;
	if (server_status_round_clock(page, clock)) draw_line(clock);
	for (const ServerStatusTextLine &l : server_status_bottom_row(page)) draw_line(l);

	// THE TICKER: a "." bouncing between x 700 and 780 at y 736, one step per
	// drawn page [orig: @0x50b1a1..0x50b203 — counter byte_24C10A0+0x24,
	//  `5c + 700` under 16, `5 * (172 - c)` from 16, `(c + 1) & 31`]
	const int tick_x = status_ticker_ < 16 ? 5 * status_ticker_ + 700 : 5 * (172 - status_ticker_);
	emit_text_at_virtual_pos(bold, bs, ".", tick_x, 736, w, h, kPageWhite, 0);
	status_ticker_ = (status_ticker_ + 1) & 0x1F;

	element_server_console_lines(page, w, h);
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
	if (state.spawn_success_gate && (page.game_type & 0xFFFDFFFFu) != 0x10020u)
		element_end_round_overlay(state, w, h);
	// The connection indicators at their reset corners, whatever hudpos
	// authors [orig: CNetQuality_DrawIndicators(&g_NetQuality, 1) @0x50b2af].
	mark_order_break();
	emit_net_quality_indicators(state.net_quality, state.overlay_master,
			/*force_default_pos=*/true, w, h);
	status_page_list_ = draw_list_;
	return true;
}

} // namespace opennova::hud
