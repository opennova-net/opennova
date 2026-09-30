#pragma once

// The HUD's key-driven toggles and cycles: the retail globals the action
// dispatch flips (the declutter level, the color scheme, the showhud flags,
// the friendly-tag mode, the objectives panel, the three overlay windows)
// and the per-frame edge polls that flip them. One state, one poll; the
// embedder samples the bound keys, hands them in, and applies the device
// side effects the returned events name (the overlay restamps, the config
// write, the toast, the FP gun bit, the camera preference).
// [orig: the first-match key scan @0x49d42f; Input_HandleActionBinding /
//  _0 dispatch cases @0x4E0601 (huddetail), @0x49afc7 (hudcolor), @0x4E0561
//  (showhud), @0x49b68b (goals), @0x49c073 / @0x49c0d9 / @0x49c0f6 (the view
//  actions), @0x49bb68 (playerlist), @0x49b55a (OldMessages), @0x49bd29
//  (ShowScore), @0x49b573 (friendly tags), @0x49af73 (help),
//  @0x49af8c (helpmap), @0x49b5e4 (Briefing), @0x49b78f (Verbose), the
//  escape close chain @0x49b234; the respawn init Game_InitRespawnState
//  @0x499360 the window actions run through the keeping wrapper @0x4993c0
//  clears @0x49936d / @0x499372 / @0x499381 / @0x499386 / @0x499395 /
//  @0x49939a / @0x49939f and, for a session peer, @0x4993ae]

#include <runtime/hud/feed_format.h>
#include <runtime/hud/hud_config_tokens.h>
#include <runtime/hud/hud_math.h>

#include <cstdint>

namespace opennova::hud {

// One action row's down-edge latch with the witnessed gate: the row fires
// on the press edge while gameplay input is active and no Shift/Alt chord is
// held (our debug picks ride Shift+F6; Ctrl is a binding modifier the sampler
// already resolved). The latch follows the UNGATED key state, so a press held
// across an armory or F3 window cannot re-fire when the gate reopens.
struct HudKeyEdge {
	bool was_down = false;
	// Advances the latch; true on the gated press edge.
	bool step(bool down, bool active, bool chorded);
	void reset() { was_down = false; }
};

// The toggles' shared state — the retail globals.
struct HudToggleState {
	int hud_color_index = kHudColorIndexDefault;      // [orig: g_HUDColors.active's index]
	int hud_detail_level = kHudDetailLevelDefault;    // the LIVE declutter level
	uint32_t showhud_flags = kShowHudFlagsDefault;    // [orig: g_FpWeaponViewFlags]
	FriendlyTagMode friendly_tag_mode = kFriendlyTagModeDefault; // [orig: g_FriendlyTagsMode]
	bool objectives_visible = false;  // [orig: dword_24C18CC, toggled 0 <-> 0xFF]
	bool scoreboard_open = false;     // [orig: g_ScoreboardPanelVisible]
	bool message_log_open = false;    // [orig: g_ShowMessageLog @0x24C18C0]
	bool end_round_stats_open = false; // [orig: dword_24C18AC]
	bool help_open = false;           // the F1 key-binding help [orig: dword_24C18B0]
	bool map_legend_open = false;     // the F12 map legend [orig: dword_24C18B4]
	// The briefing window: 0 closed, 2 open (the draw tests nonzero, the
	// escape chain and the page keys test bit 1) [orig: dword_24C18C8 —
	// HUD_DrawGameplayOverlays @0x5be133; @0x49b2c6; @0x49cafe].
	int briefing_mode = 0;
	// The MP verbose toggle [orig: g_MpVerbose2 @0x24D2154, seeded verbose-on
	// from the session settings @0x551D0F].
	bool mp_verbose = kMpVerboseDefault;

	HudKeyEdge huddetail, hudcolor, showhud, dotsize, goals;
	HudKeyEdge view1st, viewwithgun, viewchase;
	HudKeyEdge playerlist, old_messages, show_score;
	HudKeyEdge friendly_tags, help, helpmap, briefing, verbose;
	HudKeyEdge commander;
};

// The briefing window's open value [orig: `mov eax, 2` @0x49b615].
inline constexpr int kBriefingModeOpen = 2;

// One frame's sampled key states for the poll: each bound row's down state,
// whether the huddetail and hudcolor rows currently resolve to a common key
// (retail's scan is FIRST-MATCH-WINS by catalog row, huddetail 50 before
// hudcolor 76, so a shared key lets huddetail consume the edge — the D-CTRL-4
// adjudication), the chord and gate bits, and whether an MP session is live
// (the ShowScore toggle is SP-only [orig: the !is_in_session gate @0x49bd29];
// the respawn init closes the player list only for a session peer
// [orig: is_in_session && is_mp_session_peer @0x4993a4..0x4993ae] — every
// HUD-bearing process in a session is that peer), and whether the game type
// carries the objective bit (the Goals row toggles the objectives only out of a
// session or in an objective game; otherwise it re-dispatches Briefing
// [orig: case 31 @0x49b65f — `g_GameType & 0x20000`]).
struct HudKeyPoll {
	bool huddetail = false;
	bool hudcolor = false;
	bool rows_share_key = false;
	bool showhud = false;
	bool dotsize = false;
	bool goals = false;
	bool view1st = false;
	bool viewwithgun = false;
	bool viewchase = false;
	bool playerlist = false;
	bool old_messages = false;
	bool show_score = false;
	bool friendly_tags = false; // row 100 ShowFriendly, dispatch 30
	bool help = false;          // row 106 help, dispatch 8
	bool helpmap = false;       // row 73 helpmap, dispatch 234
	bool briefing = false;      // row 54 Briefing, dispatch 53
	bool verbose = false;       // row 75 Verbose, dispatch 37
	bool commander_menu = false; // row 53 commander_menu, dispatch 221
	bool chorded = false;
	bool active = false;
	bool in_session = false;
	bool objective_game = false; // g_GameType & kObjectiveBit (0x20000)
	// The local player is alive: the commander_menu row carries the binding
	// flag 0x1 the dispatcher tests against a dead player (Flags & 2).
	bool local_alive = true;
};

// What a poll flipped, for the embedder's device side effects.
namespace hud_toggle_event {
inline constexpr uint32_t kHudDetailCycled = 0x1;   // restamp the overlay's live level
inline constexpr uint32_t kHudColorCycled = 0x2;    // restamp the overlay; write the config token
inline constexpr uint32_t kShowHudCycled = 0x4;     // restamp the overlay flags; the FP gun bit
inline constexpr uint32_t kDotsizeCycled = 0x8;     // the overlay's sight-scale cycle
inline constexpr uint32_t kObjectivesToggled = 0x10;
inline constexpr uint32_t kGunBitChanged = 0x20;    // a view action rewrote showhud bit 0
inline constexpr uint32_t kFirstPersonSelected = 0x40; // view1st (action 400)
inline constexpr uint32_t kThirdPersonSelected = 0x80; // viewchase (action 402)
inline constexpr uint32_t kScoreboardToggled = 0x100;
inline constexpr uint32_t kMessageLogToggled = 0x200;
inline constexpr uint32_t kShowScoreToggled = 0x400;
// A window action ran the respawn init: the embedder closes the sim's map
// overlay mode beside the windows this state already cleared
// [orig: g_MapOverlayMode = 0 @0x499395].
inline constexpr uint32_t kOverlayWindowsCleared = 0x800;
// viewwithgun (action 401): first person too, but its own input-action bit.
inline constexpr uint32_t kGunViewSelected = 0x1000;
// The friendly-tags cycle ran: restamp the overlay mode and post the toast
// friendly_tag_toast_key names.
inline constexpr uint32_t kFriendlyTagsCycled = 0x2000;
inline constexpr uint32_t kHelpToggled = 0x4000;
inline constexpr uint32_t kMapLegendToggled = 0x8000;
inline constexpr uint32_t kBriefingToggled = 0x10000;
// An SP briefing open reset the briefing pages to the first
// [orig: sub_5B9150(0) @0x49b645].
inline constexpr uint32_t kBriefingPagesReset = 0x20000;
// The verbose toggle flipped: post the toast verbose_toast_key names.
inline constexpr uint32_t kVerboseToggled = 0x40000;
// The escape action closed one HUD window and consumed the key.
inline constexpr uint32_t kEscapeClosedWindow = 0x80000;
// The escape action found no HUD window open: open the in-game menu
// [orig: UI_OpenMenuScreen("game.mnu", "INGAME") @0x49b3b6].
inline constexpr uint32_t kEscapeOpenMenu = 0x100000;
// The commander_menu action ran (after the respawn init): open cmap.mnu's CMAP
// screen [orig: UI_OpenMenuScreen("cmap.mnu", "CMAP", 0) @0x49b920].
inline constexpr uint32_t kCommandMapOpened = 0x200000;
// The player list's open edge zeroed its page: the embedder resets the
// compiler's page cursor [orig: Scoreboard_TogglePlayerList @0x4244e4].
inline constexpr uint32_t kScoreboardPageReset = 0x400000;
} // namespace hud_toggle_event

// The catalog rows the poll samples, one bit each: the embedder reads each
// row's held state by its config token (controls catalog) and hands the
// mask in.
enum HudToggleRow : int {
	kRowHudDetail,    // 50 huddetail
	kRowHudColor,     // 76 hudcolor
	kRowShowHud,      // 27 showhud
	kRowDotsize,      // 38 dotsize
	kRowGoals,        // 55 Goals
	kRowView1st,      // 107 view1st
	kRowViewWithGun,  // 108 viewwithgun
	kRowViewChase,    // 109 viewchase
	kRowPlayerList,   // 63 playerlist_alt
	kRowOldMessages,  // 56 OldMessages
	kRowShowScore,    // 99 ShowScore
	kRowFriendlyTags, // 100 ShowFriendly
	kRowHelp,         // 106 help
	kRowHelpMap,      // 73 helpmap
	kRowBriefing,     // 54 Briefing
	kRowVerbose,      // 75 Verbose
	kRowCommanderMenu, // 53 commander_menu
	kHudToggleRowCount,
};
// The row's catalog config token.
const char *hud_toggle_row_token(int row);
// Sets the poll's key bits from a mask of (1 << HudToggleRow).
void hud_key_poll_set_rows(HudKeyPoll &keys, uint32_t rows_down);

// One frame's poll: advances every latch, applies the cycles and toggles to
// the state, returns the hud_toggle_event bits that fired.
uint32_t hud_toggles_poll(HudToggleState &state, const HudKeyPoll &keys);

// The mission teardown clears the four overlay windows and their latches,
// keeping the color, detail, showhud and friendly-tag globals.
void hud_toggles_reset_mission(HudToggleState &state);

// The death-screen edge forces the declutter level to the blank level through
// the same live seam the cycle uses [orig: NapiNPClientMsg_0x00F
// @0x42E410..0x42E41C — level = 3, then the visibility rebuild].
void hud_toggles_death_screen(HudToggleState &state);

// The escape action's HUD-window close chain: the first open window in the
// witnessed order closes and consumes the key; with none open the respawn
// init runs and the embedder opens the in-game menu. Closing the map legend
// runs the respawn init too (its close keeps nothing else). Out of a
// session the key is dead while the spawn gate holds (the special-key
// handler owns it there). The chain's other legs close state the port does
// not model (the epilog screens dword_A87050 / g_EpilogScreenActive, which
// clear first @0x49b24f, the MP team/number menus dword_24C18D4/D8, the save/disconnect
// dialog dword_24C1880, dword_B76494, the cine editor dword_24C18B8, the tip
// dismiss) and the never-set dword_24C18D0 briefing twin
// (docs/interface/hud-re.md D-HUD-31).
// [orig: Input_HandleActionBinding case 18 @0x49b234: the SP spawn-gate
//  return @0x49b243; the order D4 @0x49b267, D8 @0x49b27a, 1880 @0x49b28d,
//  message log @0x49b2a0, help @0x49b2b3, briefing & 2 @0x49b2c6, D0
//  @0x49b2da, objectives @0x49b2ed, B76494 @0x49b300, cine @0x49b313, map
//  legend + keeping init @0x49b32d, tip @0x49b34d; the init @0x49b36f and
//  game.mnu INGAME @0x49b3b6]
struct HudEscapeInput {
	bool in_session = false;
	bool spawn_gate = false; // [orig: g_SpawnSuccessGate]
};
uint32_t hud_toggles_escape(HudToggleState &state, const HudEscapeInput &input);

// The verbose toggle's toast key [orig: STRMISC_VERBOSE_ON / _OFF @0x49b78f].
const char *verbose_toast_key(bool verbose);

// The friendly-tags cycle 0->1->2->3->0 with its retail toast key (gametext
// Misc/STRMISC_FRIENDLYTAGS_*) [orig: Input_HandleActionBinding case 30
// @0x49b573 -> Chat_AddMessageChannel2 @0x49bc60; the keys @0x49b596 /
// @0x49b5c1 / @0x49b5d0 / @0x49b5da]. Returns the toast key for the new mode.
const char *hud_toggles_cycle_friendly_tags(HudToggleState &state);
const char *friendly_tag_toast_key(FriendlyTagMode mode);

} // namespace opennova::hud
