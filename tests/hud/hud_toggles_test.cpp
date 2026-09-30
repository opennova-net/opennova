// The HUD's key-driven toggles (hud/hud_toggles.h), pinned where they used to
// live in the Godot shell's HUD presenter and panel lanes (ADR 0040 ladder
// E3): the gated down-edge latch, the huddetail/hudcolor shared-key
// shadowing (D-CTRL-4), the cycles and their wraps, the view actions' gun bit
// and camera preference, the three overlay window toggles with ShowScore's
// SP-only gate and sibling close, the respawn reset, the death-screen force,
// the friendly-tags and verbose rows, the help / map legend / briefing
// windows and the escape close chain.
#include <runtime/hud/hud_toggles.h>

#include <cstdio>
#include <cstring>

using namespace opennova::hud;
using namespace opennova::hud::hud_toggle_event;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

HudKeyPoll keys() {
	HudKeyPoll k;
	k.active = true;
	return k;
}

void test_edge_latch() {
	HudKeyEdge e;
	CHECK(e.step(true, true, false));   // the press edge fires
	CHECK(!e.step(true, true, false));  // a held key does not re-fire
	CHECK(!e.step(false, true, false));
	CHECK(!e.step(true, true, true));   // a chorded press never fires ...
	CHECK(!e.step(true, true, false));  // ... and the latch followed the ungated key
	CHECK(!e.step(false, true, false));
	CHECK(!e.step(true, false, false)); // inactive input: ignored, latch set
	CHECK(!e.step(true, true, false));  // so reopening the gate cannot re-fire it
	CHECK(!e.step(false, true, false));
	CHECK(e.step(true, true, false));
	e.reset();
	CHECK(e.step(true, true, false)); // reset forgets the held key
}

void test_huddetail_cycle_and_shared_key_shadowing() {
	HudToggleState s;
	s.hud_detail_level = 2;
	s.hud_color_index = 5;
	HudKeyPoll k = keys();
	k.huddetail = true;
	CHECK(hud_toggles_poll(s, k) == kHudDetailCycled);
	CHECK(s.hud_detail_level == 3);
	k.huddetail = false;
	hud_toggles_poll(s, k);
	k.huddetail = true;
	CHECK(hud_toggles_poll(s, k) == kHudDetailCycled);
	CHECK(s.hud_detail_level == 0); // 3 wraps to 0
	// Both rows down on a shared key: huddetail (row 50) consumes the edge and
	// hudcolor stays dormant; the hudcolor latch follows the shadowed state.
	k.huddetail = false;
	hud_toggles_poll(s, k);
	k.huddetail = true;
	k.hudcolor = true;
	k.rows_share_key = true;
	CHECK(hud_toggles_poll(s, k) == kHudDetailCycled);
	CHECK(s.hud_color_index == 5);
	k.huddetail = false;
	k.hudcolor = false;
	hud_toggles_poll(s, k);
	// hudcolor alone (Ctrl+F6) cycles and wraps 5 -> 0.
	k.hudcolor = true;
	CHECK(hud_toggles_poll(s, k) == kHudColorCycled);
	CHECK(s.hud_color_index == 0);
	// Distinct keys leave both rows live.
	k.huddetail = false;
	k.hudcolor = false;
	hud_toggles_poll(s, k);
	k.huddetail = true;
	k.hudcolor = true;
	k.rows_share_key = false;
	CHECK(hud_toggles_poll(s, k) == (kHudDetailCycled | kHudColorCycled));
	CHECK(s.hud_detail_level == 2 && s.hud_color_index == 1);
}

void test_showhud_goals_dotsize_and_view_actions() {
	HudToggleState s;
	CHECK(s.showhud_flags == 3);
	HudKeyPoll k = keys();
	k.showhud = true;
	CHECK(hud_toggles_poll(s, k) == kShowHudCycled);
	CHECK(s.showhud_flags == 0); // (3 + 1) & 3
	k.showhud = false;
	k.dotsize = true;
	k.goals = true;
	CHECK(hud_toggles_poll(s, k) ==
			(kDotsizeCycled | kObjectivesToggled | kOverlayWindowsCleared));
	CHECK(s.objectives_visible);
	k.dotsize = false;
	k.goals = false;
	hud_toggles_poll(s, k);
	// view1st clears the gun bit and selects first person; viewwithgun sets it
	// and reports its own row (its input-action bit differs).
	s.showhud_flags = 3;
	k.view1st = true;
	CHECK(hud_toggles_poll(s, k) == (kGunBitChanged | kFirstPersonSelected));
	CHECK(s.showhud_flags == 2);
	k.view1st = false;
	k.viewwithgun = true;
	CHECK(hud_toggles_poll(s, k) == (kGunBitChanged | kGunViewSelected));
	CHECK(s.showhud_flags == 3);
	k.viewwithgun = false;
	k.viewchase = true;
	CHECK(hud_toggles_poll(s, k) == kThirdPersonSelected);
	CHECK(s.showhud_flags == 3); // chase never touches the gun bit
}

// The overlay windows keep ONE up: every window action runs the respawn init
// through the keeping wrapper [orig: Game_InitRespawnState @0x499360;
// Game_InitRespawnStateKeepingToggle @0x4993c0], which closes the others and
// orders the sim's map overlay closed; the player list is closed by the
// others only for a session peer [orig: @0x4993a4..0x4993ae].
void test_overlay_windows() {
	HudToggleState s;
	HudKeyPoll k = keys();
	// Out of a session (SP): Tab and J on one frame — Tab's open edge runs the
	// init first, then J's flip closes nothing but the (SP-safe) player list
	// survives J's init.
	k.playerlist = true;
	k.old_messages = true;
	CHECK(hud_toggles_poll(s, k) ==
			(kScoreboardToggled | kScoreboardPageReset | kMessageLogToggled |
					kOverlayWindowsCleared));
	CHECK(s.scoreboard_open && s.message_log_open);
	k.playerlist = false;
	k.old_messages = false;
	hud_toggles_poll(s, k);
	// G (objectives) closes J's message log and keeps the SP player list.
	k.goals = true;
	CHECK(hud_toggles_poll(s, k) == (kObjectivesToggled | kOverlayWindowsCleared));
	CHECK(s.objectives_visible && !s.message_log_open && s.scoreboard_open);
	k.goals = false;
	hud_toggles_poll(s, k);
	// ShowScore is SP-only: in a session the press is swallowed (latch set).
	k.show_score = true;
	k.in_session = true;
	CHECK(hud_toggles_poll(s, k) == 0);
	CHECK(!s.end_round_stats_open && s.objectives_visible);
	k.show_score = false;
	hud_toggles_poll(s, k);
	// Out of a session it opens and the respawn-init wrapper closes the
	// objectives panel beside it.
	k.show_score = true;
	k.in_session = false;
	CHECK(hud_toggles_poll(s, k) == (kShowScoreToggled | kOverlayWindowsCleared));
	CHECK(s.end_round_stats_open && !s.objectives_visible && s.scoreboard_open);
	k.show_score = false;
	hud_toggles_poll(s, k);
	// In a session (a peer): J closes the Show Score panel AND the player
	// list; Tab's open edge then closes J's log; Tab's close edge clears only
	// the list.
	k.in_session = true;
	k.old_messages = true;
	CHECK(hud_toggles_poll(s, k) == (kMessageLogToggled | kOverlayWindowsCleared));
	CHECK(s.message_log_open && !s.end_round_stats_open && !s.scoreboard_open);
	k.old_messages = false;
	hud_toggles_poll(s, k);
	k.playerlist = true;
	// The open edge zeroes the page too [orig: @0x4244e4].
	CHECK(hud_toggles_poll(s, k) ==
			(kScoreboardToggled | kScoreboardPageReset | kOverlayWindowsCleared));
	CHECK(s.scoreboard_open && !s.message_log_open);
	k.playerlist = false;
	hud_toggles_poll(s, k);
	k.old_messages = true;
	hud_toggles_poll(s, k); // J closes the list (peer) and opens the log
	k.old_messages = false;
	hud_toggles_poll(s, k);
	k.playerlist = true;
	hud_toggles_poll(s, k); // Tab open edge closes the log
	k.playerlist = false;
	hud_toggles_poll(s, k);
	s.message_log_open = true; // a log opened by other means ...
	k.playerlist = true;
	CHECK(hud_toggles_poll(s, k) == kScoreboardToggled); // ... survives Tab's close edge
	CHECK(!s.scoreboard_open && s.message_log_open);
	k.playerlist = false;
	hud_toggles_poll(s, k);
	// The mission teardown clears the four windows and their latches; the
	// color, detail, showhud and friendly-tag globals and the HUD-row latches
	// survive.
	s.hud_color_index = 4;
	s.hud_detail_level = 2;
	k.playerlist = true;
	hud_toggles_poll(s, k);
	s.objectives_visible = true;
	s.huddetail.was_down = true;
	hud_toggles_reset_mission(s);
	CHECK(!s.scoreboard_open && !s.message_log_open && !s.end_round_stats_open &&
			!s.objectives_visible);
	CHECK(s.hud_color_index == 4 && s.hud_detail_level == 2);
	CHECK(!s.show_score.was_down && !s.playerlist.was_down && s.huddetail.was_down);
}

void test_death_screen_and_friendly_tags() {
	HudToggleState s;
	s.hud_detail_level = 1;
	hud_toggles_death_screen(s);
	CHECK(s.hud_detail_level == 3);
	CHECK(s.friendly_tag_mode == FriendlyTagMode::kFull);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_BRIEF") == 0);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_OFF") == 0);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_FARBRIEF") == 0);
	CHECK(std::strcmp(hud_toggles_cycle_friendly_tags(s), "STRMISC_FRIENDLYTAGS_FULL") == 0);
	CHECK(s.friendly_tag_mode == FriendlyTagMode::kFull);
}

// The row mask the embedder samples maps onto the poll's key bits, and each
// row names its catalog token.
void test_row_mask_and_tokens() {
	HudKeyPoll k;
	hud_key_poll_set_rows(k, (1u << kRowGoals) | (1u << kRowFriendlyTags) | (1u << kRowVerbose));
	CHECK(k.goals && k.friendly_tags && k.verbose);
	CHECK(!k.huddetail && !k.help && !k.briefing && !k.helpmap);
	CHECK(std::strcmp(hud_toggle_row_token(kRowFriendlyTags), "ShowFriendly") == 0);
	CHECK(std::strcmp(hud_toggle_row_token(kRowHelp), "help") == 0);
	CHECK(std::strcmp(hud_toggle_row_token(kRowHelpMap), "helpmap") == 0);
	CHECK(std::strcmp(hud_toggle_row_token(kRowBriefing), "Briefing") == 0);
	CHECK(std::strcmp(hud_toggle_row_token(kRowVerbose), "Verbose") == 0);
	CHECK(std::strcmp(hud_toggle_row_token(kHudToggleRowCount), "") == 0);
}

// ShowFriendly (row 100) cycles the mode on its edge; Verbose flips the MP
// verbose flag, each for the embedder's toast [orig: case 30 @0x49b573;
// case 37 @0x49b78f].
void test_friendly_tags_and_verbose_rows() {
	HudToggleState s;
	HudKeyPoll k = keys();
	k.friendly_tags = true;
	CHECK(hud_toggles_poll(s, k) == kFriendlyTagsCycled);
	CHECK(s.friendly_tag_mode == FriendlyTagMode::kBrief);
	CHECK(hud_toggles_poll(s, k) == 0); // held
	k.friendly_tags = false;
	hud_toggles_poll(s, k);
	CHECK(s.mp_verbose);
	k.verbose = true;
	CHECK(hud_toggles_poll(s, k) == kVerboseToggled);
	CHECK(!s.mp_verbose);
	CHECK(std::strcmp(verbose_toast_key(s.mp_verbose), "STRMISC_VERBOSE_OFF") == 0);
	k.verbose = false;
	hud_toggles_poll(s, k);
	k.verbose = true;
	hud_toggles_poll(s, k);
	CHECK(s.mp_verbose);
	CHECK(std::strcmp(verbose_toast_key(s.mp_verbose), "STRMISC_VERBOSE_ON") == 0);
}

// help (F1) and helpmap (F12) are keep-one windows like the others; the
// respawn init closes both [orig: case 8 @0x49af73; case 234 @0x49af8c;
// Game_InitRespawnState @0x49936d / @0x499372].
void test_help_and_map_legend_windows() {
	HudToggleState s;
	HudKeyPoll k = keys();
	k.help = true;
	CHECK(hud_toggles_poll(s, k) == (kHelpToggled | kOverlayWindowsCleared));
	CHECK(s.help_open);
	k.help = false;
	hud_toggles_poll(s, k);
	k.helpmap = true;
	CHECK(hud_toggles_poll(s, k) == (kMapLegendToggled | kOverlayWindowsCleared));
	CHECK(s.map_legend_open && !s.help_open);
	k.helpmap = false;
	hud_toggles_poll(s, k);
	k.goals = true;
	hud_toggles_poll(s, k);
	CHECK(s.objectives_visible && !s.map_legend_open);
	k.goals = false;
	hud_toggles_poll(s, k);
	k.help = true;
	hud_toggles_poll(s, k);
	CHECK(s.help_open && !s.objectives_visible);
	hud_toggles_reset_mission(s);
	CHECK(!s.help_open && !s.map_legend_open && s.briefing_mode == 0);
}

// Briefing (I): out of a session an open resets the pages and a second press
// closes; in a session a plain 0 <-> 2 flip. Goals re-dispatches Briefing in
// a non-objective session [orig: case 53 @0x49b5e4; case 31 @0x49b65f].
void test_briefing_and_goals_redispatch() {
	HudToggleState s;
	HudKeyPoll k = keys();
	k.briefing = true;
	CHECK(hud_toggles_poll(s, k) ==
			(kBriefingToggled | kBriefingPagesReset | kOverlayWindowsCleared));
	CHECK(s.briefing_mode == kBriefingModeOpen);
	k.briefing = false;
	hud_toggles_poll(s, k);
	k.briefing = true;
	CHECK(hud_toggles_poll(s, k) == (kBriefingToggled | kOverlayWindowsCleared));
	CHECK(s.briefing_mode == 0);
	k.briefing = false;
	hud_toggles_poll(s, k);
	// In a session: no page reset.
	k.in_session = true;
	k.briefing = true;
	CHECK(hud_toggles_poll(s, k) == (kBriefingToggled | kOverlayWindowsCleared));
	CHECK(s.briefing_mode == kBriefingModeOpen);
	k.briefing = false;
	hud_toggles_poll(s, k);
	// Goals in a non-objective session flips the briefing, not the objectives.
	k.goals = true;
	CHECK(hud_toggles_poll(s, k) == (kBriefingToggled | kOverlayWindowsCleared));
	CHECK(s.briefing_mode == 0 && !s.objectives_visible);
	k.goals = false;
	hud_toggles_poll(s, k);
	// An objective session toggles the objectives.
	k.objective_game = true;
	k.goals = true;
	CHECK(hud_toggles_poll(s, k) == (kObjectivesToggled | kOverlayWindowsCleared));
	CHECK(s.objectives_visible);
}

// Escape closes one HUD window per press in the witnessed order, the map
// legend through the keeping init; with none open it runs the init and asks
// for the in-game menu. Out of a session the spawn gate swallows it
// [orig: case 18 @0x49b234..0x49b3be].
void test_escape_chain() {
	HudToggleState s;
	HudEscapeInput in;
	s.message_log_open = true;
	s.help_open = true;
	s.briefing_mode = kBriefingModeOpen;
	s.objectives_visible = true;
	s.map_legend_open = true;
	s.scoreboard_open = true;
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(!s.message_log_open && s.help_open);
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(!s.help_open && s.briefing_mode == kBriefingModeOpen);
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(s.briefing_mode == 0 && s.objectives_visible);
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(!s.objectives_visible && s.map_legend_open);
	s.end_round_stats_open = true;
	CHECK(hud_toggles_escape(s, in) == (kEscapeClosedWindow | kOverlayWindowsCleared));
	CHECK(!s.map_legend_open && !s.end_round_stats_open);
	CHECK(s.scoreboard_open); // SP: the init keeps the player list
	CHECK(hud_toggles_escape(s, in) == (kEscapeOpenMenu | kOverlayWindowsCleared));
	in.spawn_gate = true;
	s.help_open = true;
	CHECK(hud_toggles_escape(s, in) == 0);
	CHECK(s.help_open);
	in.in_session = true; // in a session the gate does not apply
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(hud_toggles_escape(s, in) == (kEscapeOpenMenu | kOverlayWindowsCleared));
	CHECK(!s.scoreboard_open); // a session peer's init closes the list
}

// The commander map (row 53 commander_menu, action 221): the press edge opens
// CMAP after the full respawn init, only with gameplay input live and the
// local player alive (the row's binding flag 0x1) [orig: the row flag test
// @0x49ad8a..0x49ada0; case 221 @0x49b8fa..0x49b928].
void test_commander_menu_row() {
	CHECK(std::strcmp(hud_toggle_row_token(kRowCommanderMenu), "commander_menu") == 0);
	HudKeyPoll rows;
	hud_key_poll_set_rows(rows, 1u << kRowCommanderMenu);
	CHECK(rows.commander_menu && !rows.verbose);
	HudToggleState s;
	s.help_open = true;
	s.message_log_open = true;
	HudKeyPoll k = keys();
	k.commander_menu = true;
	CHECK(hud_toggles_poll(s, k) == (kCommandMapOpened | kOverlayWindowsCleared));
	CHECK(!s.help_open && !s.message_log_open); // the full init keeps nothing
	CHECK(hud_toggles_poll(s, k) == 0);         // held
	k.commander_menu = false;
	hud_toggles_poll(s, k);
	// A dead local player: the row drops the action.
	k.commander_menu = true;
	k.local_alive = false;
	CHECK(hud_toggles_poll(s, k) == 0);
	k.commander_menu = false;
	k.local_alive = true;
	hud_toggles_poll(s, k);
	// No gameplay input (a menu screen is up): nothing.
	k.commander_menu = true;
	k.active = false;
	CHECK(hud_toggles_poll(s, k) == 0);
}

} // namespace


// The single-player pause (row 70 pause, dispatch 25): out of a session the
// word flips on the edge; in a session the case returns. While it is set a
// binding-flag bit-0 row (dotsize, commander_menu, AudioEmote) is dropped and
// a bit-0-clear row (help) still runs [orig: case 25 @0x49b520..0x49b52d;
// the row gate @0x49addd..0x49ade8].
void test_pause_row() {
	CHECK(std::strcmp(hud_toggle_row_token(kRowPause), "pause") == 0);
	HudToggleState s;
	HudKeyPoll k = keys();
	k.pause = true;
	CHECK(hud_toggles_poll(s, k) == kPauseToggled);
	CHECK(s.paused);
	CHECK(hud_toggles_poll(s, k) == 0); // held
	k.pause = false;
	hud_toggles_poll(s, k);
	// Paused: the bit-0 rows drop, the others run.
	k.dotsize = true;
	k.commander_menu = true;
	k.audio_emote = true;
	CHECK(hud_toggles_poll(s, k) == 0);
	CHECK(!s.emotes_menu_open);
	k.dotsize = k.commander_menu = k.audio_emote = false;
	hud_toggles_poll(s, k);
	k.help = true;
	CHECK((hud_toggles_poll(s, k) & kHelpToggled) != 0);
	CHECK(s.help_open && s.paused); // the keeping init leaves the pause word
	k.help = false;
	hud_toggles_poll(s, k);
	k.pause = true;
	CHECK(hud_toggles_poll(s, k) == kPauseToggled);
	CHECK(!s.paused);
	k.pause = false;
	hud_toggles_poll(s, k);
	// In a session the pause row does nothing.
	k.in_session = true;
	k.pause = true;
	CHECK(hud_toggles_poll(s, k) == 0);
	CHECK(!s.paused);
}

// The F9 AudioEmote and F10 RadioMacro menus (rows 101 / 102, dispatch 33 /
// 54): each edge flips its word and runs the keeping init, so opening one
// closes the other and the other windows; a dead player drops both; the
// respawn init and the mission reset close them [orig: @0x49b6c9 /
// @0x49b6e2; Game_InitRespawnState @0x499377 / @0x49937c].
void test_voice_macro_menus() {
	CHECK(std::strcmp(hud_toggle_row_token(kRowAudioEmote), "AudioEmote") == 0);
	CHECK(std::strcmp(hud_toggle_row_token(kRowRadioMacro), "RadioMacro") == 0);
	HudKeyPoll rows;
	hud_key_poll_set_rows(rows, (1u << kRowAudioEmote) | (1u << kRowRadioMacro) | (1u << kRowPause));
	CHECK(rows.audio_emote && rows.radio_macro && rows.pause && !rows.commander_menu);
	HudToggleState s;
	s.help_open = true;
	HudKeyPoll k = keys();
	k.audio_emote = true;
	CHECK(hud_toggles_poll(s, k) == kOverlayWindowsCleared);
	CHECK(s.emotes_menu_open && !s.help_open);
	k.audio_emote = false;
	hud_toggles_poll(s, k);
	k.radio_macro = true;
	CHECK(hud_toggles_poll(s, k) == kOverlayWindowsCleared);
	CHECK(s.radio_menu_open && !s.emotes_menu_open);
	k.radio_macro = false;
	hud_toggles_poll(s, k);
	k.radio_macro = true;
	hud_toggles_poll(s, k); // the second press closes it
	CHECK(!s.radio_menu_open);
	k.radio_macro = false;
	hud_toggles_poll(s, k);
	k.local_alive = false;
	k.audio_emote = true;
	CHECK(hud_toggles_poll(s, k) == 0);
	CHECK(!s.emotes_menu_open);
	s.emotes_menu_open = true;
	s.radio_menu_open = true;
	s.paused = true;
	hud_toggles_reset_mission(s);
	CHECK(!s.emotes_menu_open && !s.radio_menu_open && !s.paused);
}

// The escape chain's first legs: the pause word clears first (the embedder
// resumes the session), then the emotes menu, then the radio menu, before the
// message log [orig: @0x49b24f -> @0x49b3cd..0x49b3d3; D4 @0x49b267; D8
// @0x49b27a; the message log @0x49b2a0].
void test_escape_pause_and_voice_menus() {
	HudToggleState s;
	HudEscapeInput in;
	s.paused = true;
	s.emotes_menu_open = true;
	s.radio_menu_open = true;
	s.message_log_open = true;
	CHECK(hud_toggles_escape(s, in) == (kEscapeClosedWindow | kPauseCleared));
	CHECK(!s.paused && s.emotes_menu_open);
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(!s.emotes_menu_open && s.radio_menu_open);
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(!s.radio_menu_open && s.message_log_open);
	CHECK(hud_toggles_escape(s, in) == kEscapeClosedWindow);
	CHECK(!s.message_log_open);
	// The SP spawn gate holds ahead of the pause word too.
	s.paused = true;
	in.spawn_gate = true;
	CHECK(hud_toggles_escape(s, in) == 0);
	CHECK(s.paused);
}

int main() {
	test_edge_latch();
	test_huddetail_cycle_and_shared_key_shadowing();
	test_showhud_goals_dotsize_and_view_actions();
	test_overlay_windows();
	test_death_screen_and_friendly_tags();
	test_row_mask_and_tokens();
	test_friendly_tags_and_verbose_rows();
	test_help_and_map_legend_windows();
	test_briefing_and_goals_redispatch();
	test_escape_chain();
	test_commander_menu_row();
	test_pause_row();
	test_voice_macro_menus();
	test_escape_pause_and_voice_menus();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_toggles_test OK\n");
	return 0;
}
