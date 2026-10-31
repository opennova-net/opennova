// The HUD's key-driven toggles (hud_toggles.h). Pushed down from the Godot
// shell's HUD presenter and its panel lanes (ADR 0040 ladder E3), where each
// row carried the same edge machine.
#include <runtime/hud/hud_toggles.h>
#include <runtime/world/title_rules.h> // friendly_tag_toast (the title's literal lines)

namespace opennova::hud {

bool HudKeyEdge::step(bool down, bool active) {
	// Ours fires once per press edge. Retail's key scan fires a keyboard row on
	// every key-down event it dequeues, Windows auto-repeat included: the queue
	// marks a repeat 0x80 and the action dispatch never tests it, so a held
	// key re-fires (D-CTRL-6).
	// [orig: the scan's fires @0x49d42f / @0x49d488; WM_KEYDOWN ->
	//  Input_HandleKeyPress @0x760c90 (called @0x76276a) -> Input_QueueKeyEvent
	//  @0x760c10, the repeat bit @0x760c7b; the window procedure's pre-hook
	//  g_WindowProcHook (read @0x7624c0), whose one setter
	//  Game_SetWindowProcHook @0x761ec0 has no caller]
	const bool edge = down && !was_down && active;
	was_down = down;
	return edge;
}

namespace {

// The respawn init every overlay-window action runs: clears the end-round
// stats, the map overlay mode, the message log and the objectives panel, and
// the player list only for a session peer (a joiner or the listen host; the
// HUD never runs on a dedicated server), then restores the one window the
// action just flipped — keep-one across the windows [orig:
// Game_InitRespawnState @0x499360 (@0x499381 / @0x499395 / @0x49939a /
// @0x49939f; @0x4993ae behind is_in_session && is_mp_session_peer);
// Game_InitRespawnStateKeepingToggle @0x4993c0 saves and restores *arg].
// The map overlay lives in the sim's HudMapControl; the event bit orders
// that clear from the embedder.
// The init itself: every HUD window the port models [orig: the quit dialog
// @0x499368, help @0x49936d, map legend @0x499372, the emotes menu @0x499377,
// the radio menu @0x49937c, end-round stats @0x499381, briefing @0x499386,
// map mode @0x499395 (the event bit), message log @0x49939a, objectives
// @0x49939f, the player list @0x4993ae].
uint32_t respawn_init(HudToggleState &s, bool in_session) {
	s.quit_dialog_open = false; // [orig: g_QuitDialogOpen @0x499368]
	s.help_open = false;
	s.map_legend_open = false;
	s.emotes_menu_open = false; // [orig: dword_24C18D4 @0x499377]
	s.radio_menu_open = false;  // [orig: dword_24C18D8 @0x49937c]
	s.end_round_stats_open = false;
	s.briefing_mode = 0;
	s.briefing_panel_open = false; // [orig: @0x49938B] [orig: @0x49939B (dfx2.exe)]
	s.message_log_open = false;
	s.objectives_visible = false;
	if (in_session) s.scoreboard_open = false;
	return hud_toggle_event::kOverlayWindowsCleared;
}

template <typename T>
uint32_t respawn_init_keeping(HudToggleState &s, T *keep, bool in_session) {
	const T kept = *keep;
	const uint32_t events = respawn_init(s, in_session);
	*keep = kept;
	return events;
}

// The Briefing action [orig: case 53 @0x49b5e4]: in a session a plain
// 0 <-> 2 flip; out of one, an open resets the pages first. Both run the
// keeping init.
uint32_t toggle_briefing(HudToggleState &s, bool in_session) {
	using namespace hud_toggle_event;
	uint32_t events = kBriefingToggled;
	if (in_session) {
		// [orig: neg/sbb/and/add @0x49b5ed..0x49b5fa -- nonzero -> 0, 0 -> 2]
		s.briefing_mode = s.briefing_mode != 0 ? 0 : kBriefingModeOpen;
	} else if (s.briefing_mode == kBriefingModeOpen) {
		s.briefing_mode = 0; // [orig: @0x49b61a..0x49b627]
	} else {
		s.briefing_mode = kBriefingModeOpen; // [orig: @0x49b640]
		events |= kBriefingPagesReset;        // [orig: HUD_CycleBriefingPage(0) @0x49b645]
	}
	return events | respawn_init_keeping(s, &s.briefing_mode, in_session);
}

// The rows' dispatch codes and config tokens, in HudToggleRow order: Jointops.exe's catalog
// rows 50, 76, 27, 38, 55, 107..109, 63, 56, 99, 100, 106, 73, 54, 75, 53, 70, 101, 102, 83,
// 79, 82, 84 (+0x00 / +0x4B), then dfx2.exe's rows 53 and 99. A title's catalog that carries
// no row of the code (dfx2.exe's has no dotsize, no commander_menu; Jointops.exe's no
// Objectives, no EmoteToggle) binds nothing to it.
// [orig: word_8159A8 + 108 * row, the token @+0x4B] [orig: rows 53 @0x815EFC, 99 @0x817264
//  (dfx2.exe)]
struct RowId {
	int code;
	const char *token;
};
constexpr RowId kRowIds[kHudToggleRowCount] = {
	{19, "huddetail"}, {10, "hudcolor"}, {14, "showhud"}, {216, "dotsize"}, {31, "Goals"},
	{400, "view1st"}, {401, "viewwithgun"}, {402, "viewchase"}, {102, "playerlist_alt"},
	{29, "OldMessages"}, {422, "ShowScore"}, {30, "ShowFriendly"}, {8, "help"},
	{234, "helpmap"}, {53, "Briefing"}, {37, "Verbose"}, {221, "commander_menu"},
	{25, "pause"}, {33, "AudioEmote"}, {54, "RadioMacro"}, {11, "ToggleServer"},
	{9, "flipmouse"}, {17, "mousescale"}, {119, "lockgame"}, {58, "Objectives"},
	{59, "EmoteToggle"},
};

} // namespace

const char *hud_toggle_row_token(int row) {
	return row >= 0 && row < kHudToggleRowCount ? kRowIds[row].token : "";
}

int hud_toggle_row_code(int row) {
	return row >= 0 && row < kHudToggleRowCount ? kRowIds[row].code : -1;
}

void hud_key_poll_set_rows(HudKeyPoll &k, uint32_t rows) {
	const auto down = [rows](HudToggleRow row) { return (rows & (1u << row)) != 0; };
	k.huddetail = down(kRowHudDetail);
	k.hudcolor = down(kRowHudColor);
	k.showhud = down(kRowShowHud);
	k.dotsize = down(kRowDotsize);
	k.goals = down(kRowGoals);
	k.view1st = down(kRowView1st);
	k.viewwithgun = down(kRowViewWithGun);
	k.viewchase = down(kRowViewChase);
	k.playerlist = down(kRowPlayerList);
	k.old_messages = down(kRowOldMessages);
	k.show_score = down(kRowShowScore);
	k.friendly_tags = down(kRowFriendlyTags);
	k.help = down(kRowHelp);
	k.helpmap = down(kRowHelpMap);
	k.briefing = down(kRowBriefing);
	k.verbose = down(kRowVerbose);
	k.commander_menu = down(kRowCommanderMenu);
	k.pause = down(kRowPause);
	k.audio_emote = down(kRowAudioEmote);
	k.radio_macro = down(kRowRadioMacro);
	k.toggle_server = down(kRowToggleServer);
	k.flipmouse = down(kRowFlipMouse);
	k.mousescale = down(kRowMouseScale);
	k.lockgame = down(kRowLockGame);
	k.objectives = down(kRowObjectives);
	k.emote_toggle = down(kRowEmoteToggle);
}

namespace {

// Every HUD row's held-state latch (the poll's).
constexpr HudKeyEdge HudToggleState::*kKeyEdges[] = {
	&HudToggleState::huddetail, &HudToggleState::hudcolor, &HudToggleState::showhud,
	&HudToggleState::dotsize, &HudToggleState::goals, &HudToggleState::view1st,
	&HudToggleState::viewwithgun, &HudToggleState::viewchase, &HudToggleState::playerlist,
	&HudToggleState::old_messages, &HudToggleState::show_score,
	&HudToggleState::friendly_tags, &HudToggleState::help, &HudToggleState::helpmap,
	&HudToggleState::briefing, &HudToggleState::verbose, &HudToggleState::commander,
	&HudToggleState::pause, &HudToggleState::audio_emote, &HudToggleState::radio_macro,
	&HudToggleState::toggle_server, &HudToggleState::flipmouse, &HudToggleState::mousescale,
	&HudToggleState::lockgame, &HudToggleState::objectives, &HudToggleState::emote_toggle,
};

void reset_key_edges(HudToggleState &s) {
	for (HudKeyEdge HudToggleState::*edge : kKeyEdges) (s.*edge).reset();
}

} // namespace

uint32_t hud_toggles_dispatch(HudToggleState &s, const HudKeyPoll &fired) {
	// A cleared latch passes its row's fire: each set row is one event.
	reset_key_edges(s);
	const uint32_t events = hud_toggles_poll(s, fired);
	reset_key_edges(s);
	return events;
}

uint32_t hud_toggles_poll(HudToggleState &s, const HudKeyPoll &k) {
	using namespace hud_toggle_event;
	uint32_t events = 0;
	// A row whose binding flags carry bit 0 is dropped while the pause word
	// is set [orig: Input_HandleActionBinding @0x49addd..0x49ade8 — `test
	// ecx, ecx` (flags & 1), `cmp dword_A87050, 0`]: dotsize, ShowFriendly,
	// AudioEmote and RadioMacro (0x0C000C01), view1st and viewwithgun
	// (0x0C000801), commander_menu (0x04000801).
	const bool bit0_active = k.active && !s.paused;
	// Those rows read the dead bit through the head's whole operand: the constant 2
	// on a dedicated host in a session (an authority that is no session peer), else the
	// local player's dead bit (D-NET-433) [orig: Input_HandleActionBinding - is_in_session
	//  @0x49AD52, is_authority @0x49AD5D, is_mp_session_peer @0x49AD66, `mov eax, 2`
	//  @0x49AD6F, Flags & 2 @0x49AD76..0x49AD7E].
	const bool bit0_operand_clear =
			!(k.in_session && k.authority && !k.mp_session_peer) && k.local_alive;
	// Each row's queued event passes the dispatcher's head by its row's flags
	// before its case runs; the embedder hands in the rows the head drops
	// (HudKeyPoll::head_drops). The edge latch steps either way, as the key scan
	// queues the event whatever the head then does with it.
	const auto admitted = [&k](HudToggleRow row) { return (k.head_drops & (1u << row)) == 0u; };
	// A key both rows carry reaches one of them: the key scan's first match
	// (controls::BindingSet::pressed_key, the sampler) gives it to hudcolor
	// (code 10) before huddetail (19) [orig: the first-match key scan
	// @0x49d42f / @0x49d488].
	if (s.huddetail.step(k.huddetail, k.active) && admitted(kRowHudDetail)) {
		// The huddetail cycle: level + 1, wrapping past 3 to 0, on the LIVE
		// level only (game.cfg keeps the config value) [orig:
		// Input_HandleActionBinding_0 @0x4E0601..0x4E0624 ->
		// CRenderState_SetLayerVisibility @0x59B0F0]
		s.hud_detail_level = next_hud_detail_level(s.hud_detail_level);
		events |= kHudDetailCycled;
	}
	if (s.hudcolor.step(k.hudcolor, k.active) && admitted(kRowHudColor)) {
		// The color-scheme cycle 0..5 with wrap; deliberately NO toast — the
		// retail case only cycles and restamps the color [orig: the `hudcolor`
		// action, dispatch code 10 @0x49afc7 — idx+1, >5 wraps to 0]
		s.hud_color_index = next_hud_color_index(s.hud_color_index);
		events |= kHudColorCycled;
	}
	if (s.showhud.step(k.showhud, k.active) && admitted(kRowShowHud)) {
		// flags = (flags + 1) & 3: bit 1 the corner spinmap block, bit 0 the FP
		// gun [orig: g_FpWeaponViewFlags cycle @0x4E0561; bit0 @0x4DEDEA; bit1
		// @0x5A8635]
		s.showhud_flags = next_showhud_flags(s.showhud_flags);
		events |= kShowHudCycled;
	}
	if (s.dotsize.step(k.dotsize, bit0_active && bit0_operand_clear) && admitted(kRowDotsize))
		events |= kDotsizeCycled;
	if (s.goals.step(k.goals, k.active) && admitted(kRowGoals)) {
		if (!k.in_session || k.objective_game) {
			// The objectives panel toggle, then the respawn init keeping it
			// [orig: case 31 @0x49b65f; dword_24C18CC ^= 0xFF @0x49b68b; the
			// wrapper call @0x49b69a]
			s.objectives_visible = !s.objectives_visible;
			events |= kObjectivesToggled |
					respawn_init_keeping(s, &s.objectives_visible, k.in_session);
		} else {
			// A non-objective session re-dispatches the Briefing action
			// [orig: Input_HandleActionBinding(53, 0, 0, 0) in case 31]
			events |= toggle_briefing(s, k.in_session);
		}
	}
	if (s.briefing.step(k.briefing, k.active) && admitted(kRowBriefing))
		events |= toggle_briefing(s, k.in_session);
	// DFX2-96: the Objectives row (flags 0x05000800, no bit 0): out of a session it flips the
	// briefing panel's window, then the keeping init; in one it re-dispatches Briefing.
	// [orig: Input_HandleActionBinding @0x49AD50 case 58 - the session test @0x49B6DB, the
	//  re-dispatch Input_HandleActionBinding(53) @0x49B6E4..0x49B6EE, `xor dword_24BAE80, 1`
	//  @0x49B6FB, the keeping init @0x49B702..0x49B707 (dfx2.exe)]
	if (s.objectives.step(k.objectives, k.active) && admitted(kRowObjectives)) {
		if (k.in_session) {
			events |= toggle_briefing(s, k.in_session);
		} else {
			s.briefing_panel_open = !s.briefing_panel_open;
			events |= kBriefingToggled |
					respawn_init_keeping(s, &s.briefing_panel_open, k.in_session);
		}
	}
	// The view-action rows (catalog 107/108/109 = view1st F2, viewwithgun F3,
	// viewchase F4): first person clears the FP-gun bit, gun view sets it, and
	// both select first person; chase selects the chase preference. Each row
	// reports its own event: the three actions write different BMS
	// input-action bits (world/player_view.h player_view_apply_view_action).
	// None of them moves the camera by itself — the sim's arbiter resolves the
	// mode from the preference and the seat (stock JO has no on-foot third
	// person).
	// The 412 cycle has no catalog row and is unreachable from a key; the
	// 405-410 orbit actions have none either, but the death screen's chase
	// sub-mode dispatches them (world player_view_chase_action, [orig:
	// DeathScreen_FilterInputAction @0x52ad50]). [orig: Input_HandleActionBinding cases 400
	// @0x49c073, 401 @0x49c0d9, 402 @0x49c0f6..0x49c107; the records @0x8186CC /
	// @0x818738 / @0x8187A4 (keys F2/F3/F4) — their row flag gates (0x1 with the
	// operand, the pause word and the round-over gate, 0x8000000) pass through
	// bit0_operand_clear, bit0_active and HudKeyPoll::head_drops;
	// g_FpWeaponViewFlags bit 0 cleared @0x49c073, set @0x49c0d9]
	if (s.view1st.step(k.view1st, bit0_active && bit0_operand_clear) && admitted(kRowView1st)) {
		s.showhud_flags &= ~kShowHudFlagGun;
		events |= kGunBitChanged | kFirstPersonSelected;
	}
	if (s.viewwithgun.step(k.viewwithgun, bit0_active && bit0_operand_clear) &&
			admitted(kRowViewWithGun)) {
		s.showhud_flags |= kShowHudFlagGun;
		events |= kGunBitChanged | kGunViewSelected;
	}
	if (s.viewchase.step(k.viewchase, k.active) && admitted(kRowViewChase))
		events |= kThirdPersonSelected;
	// The playerlist action (case 102) does nothing out of a session. In one,
	// the authority's status page takes it — a dedicated host always, a
	// listen host while its view is up — and flips the page's score list;
	// everyone else toggles the Tab board: retail keeps the board up until the
	// next press, and the OPEN edge runs the respawn init first (the close
	// edge clears only the panel).
	// [orig: case 102 @0x49bb23 — the session return @0x49bb2a, the peer arm
	//  @0x49bb30..0x49bb4b, the non-peer arm @0x49bb55..0x49bb5e,
	//  Server_ToggleStatusScoreList @0x500580; Scoreboard_TogglePlayerList
	//  @0x4244c0 from @0x49bb68; Game_InitRespawnState @0x4244df on the 0 -> 1
	//  edge, then page = 0 @0x4244e4]
	if (s.playerlist.step(k.playerlist, k.active) && admitted(kRowPlayerList) && k.in_session) {
		const bool status_page = k.mp_session_peer ? (k.authority && s.server_status_view)
												   : k.authority;
		if (status_page) {
			s.server_status_score_list = !s.server_status_score_list;
			events |= kServerStatusScoreListToggled;
		} else {
			s.scoreboard_open = !s.scoreboard_open;
			events |= kScoreboardToggled;
			if (s.scoreboard_open)
				events |= respawn_init_keeping(s, &s.scoreboard_open, k.in_session) |
						kScoreboardPageReset;
		}
	}
	// The Recent Messages window, then the respawn init keeping it [orig:
	// `xor g_ShowMessageLog, 1` @0x49b55a; the wrapper call @0x49b566]
	if (s.old_messages.step(k.old_messages, k.active) && admitted(kRowOldMessages)) {
		s.message_log_open = !s.message_log_open;
		events |= kMessageLogToggled | respawn_init_keeping(s, &s.message_log_open, k.in_session);
	}
	// The SP Show Score panel: settable only OUTSIDE a session; each flip runs
	// the respawn-init wrapper, which clears the other overlay windows and
	// keeps this one [orig: case 422 @0x49bd29 (the !is_in_session gate) ->
	// Game_InitRespawnStateKeepingToggle @0x4993c0 @0x49bd4b]
	if (s.show_score.step(k.show_score, k.active && !k.in_session) && admitted(kRowShowScore)) {
		s.end_round_stats_open = !s.end_round_stats_open;
		events |= kShowScoreToggled | respawn_init_keeping(s, &s.end_round_stats_open, false);
	}
	// The friendly-tags cycle [orig: case 30 @0x49b573; catalog row 100
	// ShowFriendly, default K]; the embedder posts the toast. A title whose
	// cycle steps past mode 1 under the death screen does so here
	// (world::TitleRules::friendly_tags_skip_mode_1_on_death_screen carries the witness).
	if (s.friendly_tags.step(k.friendly_tags, bit0_active && bit0_operand_clear) &&
			admitted(kRowFriendlyTags)) {
		s.friendly_tag_mode = next_friendly_tag_mode(s.friendly_tag_mode);
		if (k.death_screen && k.friendly_tags_skip_mode_1_on_death_screen &&
				s.friendly_tag_mode == FriendlyTagMode::kFarBrief)
			s.friendly_tag_mode = FriendlyTagMode::kFull;
		events |= kFriendlyTagsCycled;
	}
	// The F1 key-binding help, then the keeping init [orig: case 8
	// `xor g_HelpScreenOpen, 1` @0x49af73; the wrapper @0x49af7f]
	if (s.help.step(k.help, k.active) && admitted(kRowHelp)) {
		s.help_open = !s.help_open;
		events |= kHelpToggled | respawn_init_keeping(s, &s.help_open, k.in_session);
	}
	// The F12 map legend. Its gate refuses it while the authority's
	// server-status view is up [orig: case 234 @0x49af8c -- is_authority &&
	// dword_24C1914 skips @0x49af95; `xor dword_24C18B4, 1` @0x49afa2; the
	// wrapper @0x49afae]
	if (s.helpmap.step(k.helpmap, k.active) && admitted(kRowHelpMap) &&
			!(k.authority && s.server_status_view)) {
		s.map_legend_open = !s.map_legend_open;
		events |= kMapLegendToggled | respawn_init_keeping(s, &s.map_legend_open, k.in_session);
	}
	// The verbose flip; the embedder posts the toast [orig: case 37
	// `g_MpVerbose2 ^= 1` @0x49b78f -> Chat_AddMessageChannel2(text, -1, 930)]
	if (s.verbose.step(k.verbose, k.active) && admitted(kRowVerbose)) {
		s.mp_verbose = !s.mp_verbose;
		events |= kVerboseToggled;
	}
	// The commander map: catalog row 53 dispatches action 221 under the
	// binding flags 0x04000801, whose bit 0 drops the action for a dead local
	// player; the case then needs the local entity and no open menu screen
	// (the gameplay gate here), runs the full respawn init and opens CMAP.
	// [orig: the row flag test @0x49ad8a..0x49ada0 (Flags & 2); case 221
	//  @0x49b8fa — UI_IsMenuScreenOpen @0x49b902 (dword_255110C), Game_InitRespawnState
	//  @0x49b90f, UI_OpenMenuScreen("cmap.mnu", "CMAP", 0) @0x49b920,
	//  g_CmapScreenOpen = 1 @0x49b928]
	if (s.commander.step(k.commander_menu, bit0_active && bit0_operand_clear) &&
			admitted(kRowCommanderMenu))
		events |= kCommandMapOpened | respawn_init(s, k.in_session);
	// The single-player pause: out of a session the word flips and the quit
	// dialog closes; the embedder applies the word to the session and stops
	// the audio when it is now set. The row's flags (0x05000800) carry no bit
	// 0, so it works while paused.
	// [orig: case 25 @0x49b520 — the in-session return @0x49b527, `xor
	//  dword_A87050, 1` @0x49b52d, g_QuitDialogOpen = 0 @0x49b534,
	//  Input_ResetKeyQueue @0x49b53e, Audio_ShutdownChannelsAndDeviceTable
	//  @0x49b550]
	if (s.pause.step(k.pause, k.active) && admitted(kRowPause) && !k.in_session) {
		s.paused = !s.paused;
		s.quit_dialog_open = false;
		events |= kPauseToggled;
	}
	// The F9 emotes and F10 radio menus: flip, then the keeping init (so
	// opening one closes the other). Both rows are live-player rows.
	// [orig: case 33 `xor dword_24C18D4, 1` @0x49b6c9 + the wrapper @0x49b6d5;
	//  case 54 `xor dword_24C18D8, 1` @0x49b6e2 + the wrapper @0x49b6ee; the
	//  row flag test @0x49ad8a..0x49ada0]
	if (s.audio_emote.step(k.audio_emote, bit0_active && bit0_operand_clear) &&
			admitted(kRowAudioEmote)) {
		s.emotes_menu_open = !s.emotes_menu_open;
		events |= respawn_init_keeping(s, &s.emotes_menu_open, k.in_session);
	}
	if (s.radio_macro.step(k.radio_macro, bit0_active && bit0_operand_clear) &&
			admitted(kRowRadioMacro)) {
		s.radio_menu_open = !s.radio_menu_open;
		events |= respawn_init_keeping(s, &s.radio_menu_open, k.in_session);
	}
	// dfx2.exe's EmoteToggle row (flags 0x0C000C01, a live-player row): the cycle closed ->
	// emotes -> radio -> closed, each step through the keeping init. Jointops.exe carries the
	// case too, on no row (D-HUD-36).
	// [orig: Input_HandleActionBinding case 59 @0x49B703..0x49B74F]
	// [orig: Input_HandleActionBinding @0x49AD50 case 59 - the emotes test @0x49B76A, emotes 0 /
	//  radio 1 kept @0x49B772..0x49B786; the radio test @0x49B793, both 0, radio kept
	//  @0x49B799..0x49B7AA; else emotes 1 kept @0x49B7B7..0x49B7C6 (dfx2.exe)]
	if (s.emote_toggle.step(k.emote_toggle, bit0_active && bit0_operand_clear) &&
			admitted(kRowEmoteToggle)) {
		if (s.emotes_menu_open) {
			s.emotes_menu_open = false;
			s.radio_menu_open = true;
			events |= respawn_init_keeping(s, &s.radio_menu_open, k.in_session);
		} else if (s.radio_menu_open) {
			s.radio_menu_open = false;
			events |= respawn_init_keeping(s, &s.radio_menu_open, k.in_session);
		} else {
			s.emotes_menu_open = true;
			events |= respawn_init_keeping(s, &s.emotes_menu_open, k.in_session);
		}
	}
	// ToggleServer: the authority that is also a peer flips its status view,
	// in a session only. The row's flags (0x04000000) carry no bit 0.
	// [orig: case 11 @0x49aff1 — is_in_session @0x49aff1, is_authority
	//  @0x49affe, is_mp_session_peer @0x49b00b, `xor dword_24C1914, 1`
	//  @0x49b018; catalog row 83, default VK 0xDC]
	if (s.toggle_server.step(k.toggle_server, k.active) && admitted(kRowToggleServer) &&
			k.in_session &&
			k.authority && k.mp_session_peer) {
		s.server_status_view = !s.server_status_view;
		events |= kServerStatusViewToggled;
	}
	// The look's two live words: flipmouse flips the Y invert, mousescale
	// steps the sensitivity with the fire's sign; the embedder applies both
	// to the session's look and posts the scale toast. Both rows ship unbound
	// (flags 0x04000000: no gate bit, mode 3, alive or dead). Shift is the
	// step's sign.
	// [orig: case 9 `xor g_MouseInvertY, 1` @0x49afbb; case 17
	//  @0x49b185..0x49b1fe; catalog rows 79 @0x817AFC / 82 @0x817C40]
	if (s.flipmouse.step(k.flipmouse, k.active) && admitted(kRowFlipMouse))
		events |= kMouseFlipped;
	if (s.mousescale.step(k.mousescale, k.active) && admitted(kRowMouseScale))
		events |= kMouseScaleStepped | (k.mouse_scale_negative ? kMouseScaleNegative : 0u);
	// lockgame: an authority in a session, a listen host or a dedicated one, toggles its join
	// lock. The row ships unbound (flags 0x04000000: no gate bit, mode 3, alive or dead), and
	// the binding head's three filters take no code 119.
	// [orig: case 119 — is_in_session @0x49BC1C..0x49BC23, is_authority @0x49BC29..0x49BC30,
	//  Server_ToggleJoinLock @0x49BC36; catalog row 84; the filters @0x52AD50 (codes
	//  151..167 @0x52AD91 / @0x52AE9F, 405..410 @0x52AEBF), @0x4A3E50 (none), @0x56FC30 (the
	//  cine editor's table, 119 its pass-through entry @0x56FCF5)]
	if (s.lockgame.step(k.lockgame, k.active) && admitted(kRowLockGame) && k.in_session &&
			k.authority)
		events |= kJoinLockToggled;
	return events;
}

uint32_t hud_toggles_escape(HudToggleState &s, const HudEscapeInput &in) {
	using namespace hud_toggle_event;
	// [orig: @0x49b23b..0x49b249 -- out of a session the round-over latch returns]
	if (!in.in_session && in.round_over) return 0;
	// The pause word (with g_EpilogScreenActive, which never reaches here —
	// hud_toggles.h) clears first [orig: @0x49b24f..0x49b261 ->
	// @0x49b3cd..0x49b3d3].
	if (s.paused) {
		s.paused = false;
		return kEscapeClosedWindow | kPauseCleared;
	}
	if (s.emotes_menu_open) { // [orig: dword_24C18D4 @0x49b267]
		s.emotes_menu_open = false;
		return kEscapeClosedWindow;
	}
	if (s.radio_menu_open) { // [orig: dword_24C18D8 @0x49b27a]
		s.radio_menu_open = false;
		return kEscapeClosedWindow;
	}
	if (s.quit_dialog_open) { // [orig: g_QuitDialogOpen @0x49b28d, the clear @0x49b295]
		s.quit_dialog_open = false;
		return kEscapeClosedWindow;
	}
	if (s.message_log_open) { // [orig: @0x49b2a0]
		s.message_log_open = false;
		return kEscapeClosedWindow;
	}
	if (s.help_open) { // [orig: @0x49b2b3]
		s.help_open = false;
		return kEscapeClosedWindow;
	}
	if ((s.briefing_mode & 2) != 0) { // [orig: `test byte ptr dword_24C18C8, 2` @0x49b2c6]
		s.briefing_mode = 0;
		return kEscapeClosedWindow;
	}
	// The briefing panel's window (DFX2-96) [orig: @0x49b2da..0x49b2e2] [orig: @0x49B2EA..0x49B2F2
	//  (dfx2.exe)].
	if (s.briefing_panel_open) {
		s.briefing_panel_open = false;
		return kEscapeClosedWindow;
	}
	if (s.objectives_visible) { // [orig: @0x49b2ed]
		s.objectives_visible = false;
		return kEscapeClosedWindow;
	}
	if (s.map_legend_open) { // [orig: @0x49b32d..0x49b340 -- cleared, then the keeping init]
		s.map_legend_open = false;
		return kEscapeClosedWindow | respawn_init_keeping(s, &s.map_legend_open, in.in_session);
	}
	// A showing tip starts its fade [orig: @0x49b34d -- CTipSystem_IsShowing
	// @0x49b352, CTipSystem_BeginFade @0x49b360].
	if (tip_is_showing(s.tips)) {
		tip_begin_fade(s.tips);
		return kEscapeClosedWindow;
	}
	// None open: the init keeping the quit-dialog word, then on the
	// authority's status view in a session the dialog opens, else the menu
	// [orig: Game_InitRespawnStateKeepingToggle(&g_QuitDialogOpen) @0x49b36a;
	//  @0x49b377..0x49b38f; @0x49b39e..0x49b3be]
	const uint32_t init = respawn_init_keeping(s, &s.quit_dialog_open, in.in_session);
	if (in.in_session && in.authority && s.server_status_view) {
		s.quit_dialog_open = true;
		return kEscapeClosedWindow | kQuitDialogOpened | init;
	}
	return kEscapeOpenMenu | init;
}

void hud_toggles_session_init(HudToggleState &s, bool in_session, bool mp_session_peer) {
	// [orig: Server_InitNewRoundState @0x51cb43 (is_in_session) .. @0x51cb4b
	//  (is_mp_session_peer): 1 stored @0x51cb51, a peer or no session clears
	//  it @0x51cb5d]
	s.server_status_view = in_session && !mp_session_peer;
}

uint32_t hud_toggles_quit_dialog_key(HudToggleState &s, const HudQuitDialogKeyInput &in) {
	using namespace hud_special_key;
	if (!s.quit_dialog_open) return 0; // [orig: @0x49c5df]
	if (in.vk == in.yes_vk) {           // [orig: @0x49c5ec -> action 3 @0x49c5fa]
		return kChainTaken | kConsumed | kQuitConfirmed;
	}
	if (in.vk == in.no_vk) { // [orig: @0x49c61d, the clear @0x49c627]
		s.quit_dialog_open = false;
		return kChainTaken | kConsumed;
	}
	// [orig: @0x49c650..0x49c656 — the restart key queues event 12 only out
	//  of a session (LABEL_8 @0x49c658)]
	if (in.vk == in.restart_vk && !in.in_session) return kChainTaken | kConsumed | kRestartQueued;
	// A digit 1..9 or ':' is taken; the numbered save it names loads only out
	// of a session [orig: `keyCode - 49 <= 9` @0x49c688; Game_RequestLoadSaveFile @0x4396a0].
	if (static_cast<unsigned>(in.vk - '1') <= 9u) return kChainTaken | kConsumed;
	return kChainTaken;
}

uint32_t hud_round_over_key(const HudRoundOverKeyInput &in) {
	using namespace hud_round_over;
	// [orig: @0x49c7d5..0x49c7fd — out of a session, or the co-op game type
	//  0x10020 (bit 0x20000 masked) without bit 0x20000]
	const bool coop = (in.game_type & 0xFFFDFFFFu) == 0x10020u && (in.game_type & 0x20000u) == 0;
	if (in.in_session && !coop) {
		// The end-of-round board's arm: PgUp / PgDn scroll it and are taken;
		// ADVANCED flips its word and goes on, as every other key does
		// [orig: @0x49c802 / @0x49c82c; @0x49c859 -> the tail @0x49c9c9].
		if (in.vk == 0x21 || in.vk == 0x22) return kArm | kConsumed;
		return kArm;
	}
	if (in.vk == in.restart_vk) return kArm | kConsumed | kRestart; // [orig: @0x49c871]
	if (in.vk == 27) return kArm | kConsumed | kExit;               // [orig: @0x49c8da / @0x49c8e2]
	return kArm | kConsumed;                                        // [orig: @0x49c8d5]
}

uint32_t hud_toggles_cine_start(HudToggleState &s, bool in_session) {
	return respawn_init(s, in_session);
}

bool hud_toggles_server_status_page_key(const HudToggleState &s, int vk, bool in_session,
		bool authority, bool mp_session_peer) {
	// [orig: the in-session arm @0x49c903; @0x49c960 (!is_mp_session_peer) ||
	//  @0x49c968 (dword_24C1914) && @0x49c970 (is_authority); VK_RETURN
	//  @0x49c978, VK_PRIOR @0x49c99f, VK_NEXT @0x49c9c4]
	if (!in_session) return false;
	if (mp_session_peer && !(s.server_status_view && authority)) return false;
	return vk == 0x0D || vk == 0x21 || vk == 0x22;
}

void hud_toggles_close_voice_menu(HudToggleState &s, bool radio) {
	if (radio) {
		s.radio_menu_open = false; // [orig: @0x49c7a8]
	} else {
		s.emotes_menu_open = false; // [orig: @0x49c75c]
	}
}

const char *verbose_toast_key(bool verbose) {
	return verbose ? "STRMISC_VERBOSE_ON" : "STRMISC_VERBOSE_OFF";
}

std::string mouse_scale_toast(const GameTextLookup &gametext, int32_t scale) {
	// [orig: sprintf(buffer, "%i", scale) @0x49b1ca; the format and its $A
	//  @0x49b1ec / @0x49b1f5]
	const std::string word = std::to_string(scale);
	return chat_format_player_tokens(game_text(gametext, "Misc", "STRMISC_MOUSESCALE", ""),
			word.c_str());
}

void hud_toggles_reset_mission(HudToggleState &s) {
	// The mission teardown: the windows and their latches. The HUD-row
	// latches follow the ungated key state and survive, as the key scan's
	// per-key state does in retail.
	s.scoreboard_open = false;
	s.message_log_open = false;
	s.end_round_stats_open = false;
	s.objectives_visible = false;
	s.help_open = false;
	s.map_legend_open = false;
	s.briefing_mode = 0;
	s.briefing_panel_open = false;
	s.emotes_menu_open = false;
	s.radio_menu_open = false;
	// A mission start closes the quit dialog [orig: Game_StartMission
	// @0x525b2b]; the status view and its score list are the session's
	// (hud_toggles_session_init), not the mission's.
	s.quit_dialog_open = false;
	// A mission start zeroes the pause word [orig: Game_StartMission @0x525baa
	// -> Game_ResetSessionHudState @0x434bd7].
	s.paused = false;
	// ... and the showing tip with the once-counters: the argument is the
	// first-start flag, set on a start from the menu and clear on the SP
	// restart [orig: Game_StartMission @0x525dec..0x525df2 --
	// CTipSystem_Reset(ebp), ebp loaded @0x52540a from the `param == 0` slot
	// stored @0x5243a6 (`setz al`); the restart passes 1 @0x5263d9].
	tip_reset(s.tips, true);
	s.playerlist.reset();
	s.old_messages.reset();
	s.show_score.reset();
	s.help.reset();
	s.helpmap.reset();
	s.briefing.reset();
	s.audio_emote.reset();
	s.radio_macro.reset();
	s.objectives.reset();
	s.emote_toggle.reset();
}

HudBriefingWindow hud_briefing_window(const HudToggleState &s, bool in_session, bool sp_how_to) {
	if (s.briefing_panel_open) return HudBriefingWindow::Briefing;
	if (s.briefing_mode == 0 || in_session) return HudBriefingWindow::None;
	return sp_how_to ? HudBriefingWindow::HowTo : HudBriefingWindow::Briefing;
}

void hud_toggles_tip_events(HudToggleState &s, const uint8_t *events, size_t count) {
	for (size_t i = 0; i < count; ++i) tip_handle_event(s.tips, events[i]);
}

void hud_toggles_tip_frames(HudToggleState &s, int frames) {
	// Past the countdown every further frame is the floor's no-op.
	for (int i = 0; i < frames && s.tips.countdown > 0; ++i) tip_tick_countdown(s.tips);
}

void hud_toggles_restart_round(HudToggleState &s) {
	// The same windows and latches; the SP restart's tip reset keeps the
	// once-counters (its CTipSystem_Reset argument is 0).
	const TipSystem tips = s.tips;
	hud_toggles_reset_mission(s);
	s.tips = tips;
	tip_reset(s.tips, false);
}

void hud_toggles_death_screen(HudToggleState &s) {
	s.hud_detail_level = kHudDetailLevelBlank;
}

int hud_deploy_key_pick(const HudDeployKeyInput &in) {
	// [orig: @0x49c9c9..0x49c9f7 — out of a session (and without the
	//  SinglePlayerRespawn attribute) only the overlay opens the leg; then the
	//  player must be dead or the overlay up]
	if (!in.in_session && !in.single_player_respawn && !in.deploy_overlay) return -1;
	if (!in.local_dead && !in.deploy_overlay) return -1;
	if (in.vk == 'X') return 0;                       // [orig: @0x49c9fd -> @0x49c658]
	if (in.vk == ' ') return kHudDeployKeyAutoTeam;   // [orig: @0x49ca06..0x49ca0d]
	// [orig: @0x49ca37..0x49ca71 — `lea ecx, [esi-41h]; cmp ecx, 19h`; the
	//  index against SpawnZoneList_GetCount, a null entry, the team compare]
	const int index = in.vk - 'A';
	if (index < 0 || index > 25) return -1;
	if (static_cast<size_t>(index) >= in.zone_count || in.zone_teams == nullptr) return -1;
	const int16_t team = in.zone_teams[index];
	if (team < 0 || team != static_cast<int16_t>(in.local_team)) return -1;
	return index + 1;
}

const char *friendly_tag_toast_key(FriendlyTagMode mode) {
	switch (mode) {
		case FriendlyTagMode::kOff: return "STRMISC_FRIENDLYTAGS_OFF";
		case FriendlyTagMode::kFarBrief: return "STRMISC_FRIENDLYTAGS_FARBRIEF";
		case FriendlyTagMode::kFull: return "STRMISC_FRIENDLYTAGS_FULL";
		case FriendlyTagMode::kBrief: return "STRMISC_FRIENDLYTAGS_BRIEF";
	}
	return "";
}

std::string friendly_tag_toast(const GameTextLookup &gametext, FriendlyTagMode mode,
		const world::TitleRules &rules) {
	const int index = static_cast<int>(mode);
	if (index >= 0 && index < static_cast<int>(rules.friendly_tag_toast_literals.size()) &&
			rules.friendly_tag_toast_literals[static_cast<size_t>(index)] != nullptr)
		return rules.friendly_tag_toast_literals[static_cast<size_t>(index)];
	return game_text(gametext, kGameTextMisc, friendly_tag_toast_key(mode), "");
}

const char *hud_toggles_cycle_friendly_tags(HudToggleState &s) {
	s.friendly_tag_mode = next_friendly_tag_mode(s.friendly_tag_mode);
	return friendly_tag_toast_key(s.friendly_tag_mode);
}

} // namespace opennova::hud
