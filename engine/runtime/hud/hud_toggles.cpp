// The HUD's key-driven toggles (hud_toggles.h). Pushed down from the Godot
// shell's HUD presenter and its panel lanes (ADR 0040 ladder E3), where each
// row carried the same edge machine.
#include <runtime/hud/hud_toggles.h>

namespace opennova::hud {

bool HudKeyEdge::step(bool down, bool active, bool chorded) {
	// [orig: the first-match key scan @0x49d42f fires once per press]
	const bool edge = down && !was_down && active && !chorded;
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
// The init itself: every HUD window the port models [orig: help @0x49936d,
// map legend @0x499372, the emotes menu @0x499377, the radio menu @0x49937c,
// end-round stats @0x499381, briefing @0x499386, map mode @0x499395 (the
// event bit), message log @0x49939a, objectives @0x49939f, the player list
// @0x4993ae].
uint32_t respawn_init(HudToggleState &s, bool in_session) {
	s.help_open = false;
	s.map_legend_open = false;
	s.emotes_menu_open = false; // [orig: dword_24C18D4 @0x499377]
	s.radio_menu_open = false;  // [orig: dword_24C18D8 @0x49937c]
	s.end_round_stats_open = false;
	s.briefing_mode = 0;
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
		events |= kBriefingPagesReset;        // [orig: sub_5B9150(0) @0x49b645]
	}
	return events | respawn_init_keeping(s, &s.briefing_mode, in_session);
}

constexpr const char *kRowTokens[kHudToggleRowCount] = {
	"huddetail", "hudcolor", "showhud", "dotsize", "Goals", "view1st", "viewwithgun",
	"viewchase", "playerlist_alt", "OldMessages", "ShowScore", "ShowFriendly", "help",
	"helpmap", "Briefing", "Verbose", "commander_menu", "pause", "AudioEmote", "RadioMacro",
};

} // namespace

const char *hud_toggle_row_token(int row) {
	return row >= 0 && row < kHudToggleRowCount ? kRowTokens[row] : "";
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
}

uint32_t hud_toggles_poll(HudToggleState &s, const HudKeyPoll &k) {
	using namespace hud_toggle_event;
	uint32_t events = 0;
	// A row whose binding flags carry bit 0 is dropped while the pause word
	// is set [orig: Input_HandleActionBinding @0x49addd..0x49ade8 — `test
	// ecx, ecx` (flags & 1), `cmp dword_A87050, 0`]: dotsize, ShowFriendly,
	// view1st, viewwithgun (0x0C0008xx / 0x0C000Cxx), commander_menu
	// (0x04000801), AudioEmote and RadioMacro (0x0C000C01).
	const bool bit0_active = k.active && !s.paused;
	// huddetail (row 50) precedes hudcolor (row 76): when both rows resolve to
	// the same physical key the huddetail row consumes the edge and hudcolor
	// ships dormant on it; distinct keys leave both rows live (D-CTRL-4).
	// [orig: the first-match key scan @0x49d42f; rows 50 < 76]
	if (s.huddetail.step(k.huddetail, k.active, k.chorded)) {
		// The huddetail cycle: level + 1, wrapping past 3 to 0, on the LIVE
		// level only (game.cfg keeps the config value) [orig:
		// Input_HandleActionBinding_0 @0x4E0601..0x4E0624 ->
		// CRenderState_SetLayerVisibility @0x59B0F0]
		s.hud_detail_level = next_hud_detail_level(s.hud_detail_level);
		events |= kHudDetailCycled;
	}
	const bool hudcolor_down = k.hudcolor && !(k.huddetail && k.rows_share_key);
	if (s.hudcolor.step(hudcolor_down, k.active, k.chorded)) {
		// The color-scheme cycle 0..5 with wrap; deliberately NO toast — the
		// retail case only cycles and restamps the color [orig: the `hudcolor`
		// action, dispatch code 10 @0x49afc7 — idx+1, >5 wraps to 0]
		s.hud_color_index = next_hud_color_index(s.hud_color_index);
		events |= kHudColorCycled;
	}
	if (s.showhud.step(k.showhud, k.active, k.chorded)) {
		// flags = (flags + 1) & 3: bit 1 the corner spinmap block, bit 0 the FP
		// gun [orig: g_FpWeaponViewFlags cycle @0x4E0561; bit0 @0x4DEDEA; bit1
		// @0x5A8635]
		s.showhud_flags = next_showhud_flags(s.showhud_flags);
		events |= kShowHudCycled;
	}
	if (s.dotsize.step(k.dotsize, bit0_active, k.chorded)) events |= kDotsizeCycled;
	if (s.goals.step(k.goals, k.active, k.chorded)) {
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
	if (s.briefing.step(k.briefing, k.active, k.chorded)) events |= toggle_briefing(s, k.in_session);
	// The view-action rows (catalog 107/108/109 = view1st F2, viewwithgun F3,
	// viewchase F4): first person clears the FP-gun bit, gun view sets it, and
	// both select first person; chase selects the chase preference. Each row
	// reports its own event: the three actions write different BMS
	// input-action bits (world/player_view.h player_view_apply_view_action).
	// None of them moves the camera by itself — the sim's arbiter resolves the
	// mode from the preference and the seat (stock JO has no on-foot third
	// person).
	// The 412 cycle and the 405-410 orbit actions have no catalog row and are
	// unreachable from a key. [orig: Input_HandleActionBinding cases 400
	// @0x49c073, 401 @0x49c0d9, 402 @0x49c0f6..0x49c107; the records @0x8186CC /
	// @0x818738 / @0x8187A4 (keys F2/F3/F4) — their row flag gates (0x1 /
	// 0x40 / 0x400 / 0x8000000) ride the unported binding layer, D-CTRL-3;
	// g_FpWeaponViewFlags bit 0 cleared @0x49c073, set @0x49c0d9]
	if (s.view1st.step(k.view1st, bit0_active, k.chorded)) {
		s.showhud_flags &= ~kShowHudFlagGun;
		events |= kGunBitChanged | kFirstPersonSelected;
	}
	if (s.viewwithgun.step(k.viewwithgun, bit0_active, k.chorded)) {
		s.showhud_flags |= kShowHudFlagGun;
		events |= kGunBitChanged | kGunViewSelected;
	}
	if (s.viewchase.step(k.viewchase, k.active, k.chorded)) events |= kThirdPersonSelected;
	// The Tab player list TOGGLES the panel-visible flag — retail keeps the
	// board up until the next press; the OPEN edge runs the respawn init
	// first (the close edge clears only the panel) [orig:
	// Scoreboard_TogglePlayerList @0x4244c0 from the dispatch case @0x49bb68;
	// Game_InitRespawnState @0x4244df on the 0 -> 1 edge, then page = 0
	// @0x4244e4]
	if (s.playerlist.step(k.playerlist, k.active, k.chorded)) {
		s.scoreboard_open = !s.scoreboard_open;
		events |= kScoreboardToggled;
		if (s.scoreboard_open)
			events |= respawn_init_keeping(s, &s.scoreboard_open, k.in_session) |
					kScoreboardPageReset;
	}
	// The Recent Messages window, then the respawn init keeping it [orig:
	// `xor g_ShowMessageLog, 1` @0x49b55a; the wrapper call @0x49b566]
	if (s.old_messages.step(k.old_messages, k.active, k.chorded)) {
		s.message_log_open = !s.message_log_open;
		events |= kMessageLogToggled | respawn_init_keeping(s, &s.message_log_open, k.in_session);
	}
	// The SP Show Score panel: settable only OUTSIDE a session; each flip runs
	// the respawn-init wrapper, which clears the other overlay windows and
	// keeps this one [orig: case 422 @0x49bd29 (the !is_in_session gate) ->
	// Game_InitRespawnStateKeepingToggle @0x4993c0 @0x49bd4b]
	if (s.show_score.step(k.show_score, k.active && !k.in_session, k.chorded)) {
		s.end_round_stats_open = !s.end_round_stats_open;
		events |= kShowScoreToggled | respawn_init_keeping(s, &s.end_round_stats_open, false);
	}
	// The friendly-tags cycle [orig: case 30 @0x49b573; catalog row 100
	// ShowFriendly, default K]; the embedder posts the toast.
	if (s.friendly_tags.step(k.friendly_tags, bit0_active, k.chorded)) {
		s.friendly_tag_mode = next_friendly_tag_mode(s.friendly_tag_mode);
		events |= kFriendlyTagsCycled;
	}
	// The F1 key-binding help, then the keeping init [orig: case 8
	// `xor dword_24C18B0, 1` @0x49af73; the wrapper @0x49af7f]
	if (s.help.step(k.help, k.active, k.chorded)) {
		s.help_open = !s.help_open;
		events |= kHelpToggled | respawn_init_keeping(s, &s.help_open, k.in_session);
	}
	// The F12 map legend. Its gate refuses only a dedicated host's status
	// screen, and the HUD never runs on a dedicated host [orig: case 234
	// @0x49af8c -- is_authority && dword_24C1914 skips; `xor dword_24C18B4, 1`
	// @0x49afa2; the wrapper @0x49afae]
	if (s.helpmap.step(k.helpmap, k.active, k.chorded)) {
		s.map_legend_open = !s.map_legend_open;
		events |= kMapLegendToggled | respawn_init_keeping(s, &s.map_legend_open, k.in_session);
	}
	// The verbose flip; the embedder posts the toast [orig: case 37
	// `g_MpVerbose2 ^= 1` @0x49b78f -> Chat_AddMessageChannel2(text, -1, 930)]
	if (s.verbose.step(k.verbose, k.active, k.chorded)) {
		s.mp_verbose = !s.mp_verbose;
		events |= kVerboseToggled;
	}
	// The commander map: catalog row 53 dispatches action 221 under the
	// binding flags 0x04000801, whose bit 0 drops the action for a dead local
	// player; the case then needs the local entity and no open menu screen
	// (the gameplay gate here), runs the full respawn init and opens CMAP.
	// [orig: the row flag test @0x49ad8a..0x49ada0 (Flags & 2); case 221
	//  @0x49b8fa — sub_54B970 @0x49b902 (dword_255110C), Game_InitRespawnState
	//  @0x49b90f, UI_OpenMenuScreen("cmap.mnu", "CMAP", 0) @0x49b920,
	//  g_CmapScreenOpen = 1 @0x49b928]
	if (s.commander.step(k.commander_menu, bit0_active && k.local_alive, k.chorded))
		events |= kCommandMapOpened | respawn_init(s, k.in_session);
	// The single-player pause: out of a session the word flips; the embedder
	// applies it to the session and stops the audio when it is now set. The
	// row's flags (0x05000800) carry no bit 0, so it works while paused.
	// [orig: case 25 @0x49b520 — the in-session return @0x49b527, `xor
	//  dword_A87050, 1` @0x49b52d, dword_24C1880 = 0 @0x49b534 (the quit
	//  dialog the port does not have), Input_ResetKeyQueue @0x49b53e,
	//  Audio_ShutdownChannelsAndDeviceTable @0x49b550]
	if (s.pause.step(k.pause, k.active, k.chorded) && !k.in_session) {
		s.paused = !s.paused;
		events |= kPauseToggled;
	}
	// The F9 emotes and F10 radio menus: flip, then the keeping init (so
	// opening one closes the other). Both rows are live-player rows.
	// [orig: case 33 `xor dword_24C18D4, 1` @0x49b6c9 + the wrapper @0x49b6d5;
	//  case 54 `xor dword_24C18D8, 1` @0x49b6e2 + the wrapper @0x49b6ee; the
	//  row flag test @0x49ad8a..0x49ada0]
	if (s.audio_emote.step(k.audio_emote, bit0_active && k.local_alive, k.chorded)) {
		s.emotes_menu_open = !s.emotes_menu_open;
		events |= respawn_init_keeping(s, &s.emotes_menu_open, k.in_session);
	}
	if (s.radio_macro.step(k.radio_macro, bit0_active && k.local_alive, k.chorded)) {
		s.radio_menu_open = !s.radio_menu_open;
		events |= respawn_init_keeping(s, &s.radio_menu_open, k.in_session);
	}
	return events;
}

uint32_t hud_toggles_escape(HudToggleState &s, const HudEscapeInput &in) {
	using namespace hud_toggle_event;
	// [orig: @0x49b23b..0x49b249 -- out of a session the spawn gate returns]
	if (!in.in_session && in.spawn_gate) return 0;
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
	if (s.objectives_visible) { // [orig: @0x49b2ed]
		s.objectives_visible = false;
		return kEscapeClosedWindow;
	}
	if (s.map_legend_open) { // [orig: @0x49b32d..0x49b340 -- cleared, then the keeping init]
		s.map_legend_open = false;
		return kEscapeClosedWindow | respawn_init_keeping(s, &s.map_legend_open, in.in_session);
	}
	// None open: the init keeping the quit-dialog word (never set in the
	// port), then the menu [orig: @0x49b36a; @0x49b3ab..0x49b3be]
	return kEscapeOpenMenu | respawn_init(s, in.in_session);
}

const char *verbose_toast_key(bool verbose) {
	return verbose ? "STRMISC_VERBOSE_ON" : "STRMISC_VERBOSE_OFF";
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
	s.emotes_menu_open = false;
	s.radio_menu_open = false;
	// A mission start zeroes the pause word [orig: Game_StartMission @0x525baa
	// -> Game_ResetSessionHudState @0x434bd7].
	s.paused = false;
	s.playerlist.reset();
	s.old_messages.reset();
	s.show_score.reset();
	s.help.reset();
	s.helpmap.reset();
	s.briefing.reset();
	s.audio_emote.reset();
	s.radio_macro.reset();
}

void hud_toggles_death_screen(HudToggleState &s) {
	s.hud_detail_level = kHudDetailLevelBlank;
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

const char *hud_toggles_cycle_friendly_tags(HudToggleState &s) {
	s.friendly_tag_mode = next_friendly_tag_mode(s.friendly_tag_mode);
	return friendly_tag_toast_key(s.friendly_tag_mode);
}

} // namespace opennova::hud
