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
#include <runtime/hud/tip_system.h>

#include <cstddef>
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
	// The F9 AudioEmote and F10 RadioMacro menus (catalog rows 101 / 102,
	// dispatch 33 / 54) [orig: dword_24C18D4 / dword_24C18D8 — `xor 1` then the
	// keeping init @0x49b6c9 / @0x49b6e2].
	bool emotes_menu_open = false;
	bool radio_menu_open = false;
	// The single-player pause word's bit 0: the pause row (catalog 70,
	// dispatch 25) flips it out of a session, the in-game menu sets it on
	// open and clears it on resume out of a session, the escape chain clears
	// it first, and a mission start zeroes it. Every row whose binding flags
	// carry bit 0 is dropped while it is set. The embedder applies it to the
	// session pause and draws Overlays/STROVER7 while it holds.
	// [orig: dword_A87050 — case 25 `xor 1` @0x49b52d; UI_OptionsScreenInit
	//  `or 1` @0x554dd8 / UI_IngameBackResumeCommand `and ~1` @0x55549e (both
	//  behind !is_in_session); the escape clear @0x49b3d3;
	//  Game_ResetSessionHudState @0x434bd7; the row gate @0x49addd..0x49ade8;
	//  Game_ProcessMainFrame's tick gate @0x5265a0; HUD_DrawOverlayPanels
	//  @0x5c0120]
	bool paused = false;
	// The tip ("MrClippy"), a process global like the rest of this state: the
	// escape chain's last leg fades it, the mission start clears the showing
	// tip and keeps the once-counters (tip_system.h carries the witness).
	TipSystem tips;
	// The authority's server-status view: while it is up the embedder draws
	// the status page INSTEAD of the scene frame (hud_server_status.h). The
	// ToggleServer row (catalog 83, dispatch 11) flips it on an authority that
	// is a session peer, in a session; the session create / destroy sets it
	// for a dedicated host and clears it otherwise.
	// [orig: g_ServerStatusViewActive (dword_24C1914) — case 11 `xor 1`
	//  @0x49b018 behind @0x49aff1..0x49b012; Server_InitNewRoundState
	//  @0x51cb43..0x51cb5d; the render gate GameLoop_RenderFrame
	//  @0x521cd6..0x521cef]
	bool server_status_view = false;
	// The status page's player score list, flipped by the playerlist action
	// in place of the Tab board and never cleared [orig:
	// g_ServerStatusScoreListOpen (dword_C94798) — Server_ToggleStatusScoreList
	// @0x500580, its only writer; read @0x50b278].
	bool server_status_score_list = false;
	// The quit dialog: opened only by the escape chain's tail on the view,
	// closed by its own escape leg, the respawn init, the SP pause row and
	// the mission start; while it is up the special-key handler takes the
	// yes / no keys (hud_toggles_quit_dialog_key).
	// [orig: g_QuitDialogOpen (dword_24C1880) — set @0x49b38f; cleared
	//  @0x49b295, Game_InitRespawnState @0x499368, case 25 @0x49b534,
	//  Game_StartMission @0x525b2b]
	bool quit_dialog_open = false;

	HudKeyEdge huddetail, hudcolor, showhud, dotsize, goals;
	HudKeyEdge view1st, viewwithgun, viewchase;
	HudKeyEdge playerlist, old_messages, show_score;
	HudKeyEdge friendly_tags, help, helpmap, briefing, verbose;
	HudKeyEdge commander;
	HudKeyEdge pause, audio_emote, radio_macro;
	HudKeyEdge toggle_server;
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
	bool pause = false;          // row 70 pause, dispatch 25
	bool audio_emote = false;    // row 101 AudioEmote, dispatch 33
	bool radio_macro = false;    // row 102 RadioMacro, dispatch 54
	bool toggle_server = false;  // row 83 ToggleServer, dispatch 11
	bool chorded = false;
	bool active = false;
	bool in_session = false;
	// The connection mode's two bits [orig: g_NapiNPCtx.is_authority /
	// is_mp_session_peer]: single player and a listen host carry both, a
	// joiner only the peer bit, a dedicated host only the authority bit.
	bool authority = false;
	bool mp_session_peer = false;
	bool objective_game = false; // g_GameType & kObjectiveBit (0x20000)
	// The local player is alive: the commander_menu, AudioEmote and
	// RadioMacro rows carry the binding flag 0x1 the dispatcher tests against
	// a dead player (Flags & 2).
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
// The ToggleServer row flipped the server-status view: the embedder swaps the
// scene frame for the status page (or back) [orig: case 11 @0x49b018].
inline constexpr uint32_t kServerStatusViewToggled = 0x2000000;
// The playerlist action flipped the status page's score list instead of the
// Tab board [orig: Server_ToggleStatusScoreList @0x500580 from @0x49bb4b /
// @0x49bb5e].
inline constexpr uint32_t kServerStatusScoreListToggled = 0x4000000;
// The escape chain's tail opened the quit dialog on the view (the key is
// consumed) [orig: @0x49b377..0x49b38f].
inline constexpr uint32_t kQuitDialogOpened = 0x8000000;
// The pause row flipped HudToggleState::paused: the embedder applies it to
// the session pause (and, pausing, the audio stop) [orig: case 25 @0x49b520
// — Audio_ShutdownChannelsAndDeviceTable @0x49b550 when the word is now set].
inline constexpr uint32_t kPauseToggled = 0x800000;
// The escape chain cleared the pause word: the embedder resumes the session
// [orig: @0x49b3cd..0x49b3d3].
inline constexpr uint32_t kPauseCleared = 0x1000000;
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
	kRowPause,        // 70 pause
	kRowAudioEmote,   // 101 AudioEmote
	kRowRadioMacro,   // 102 RadioMacro
	kRowToggleServer, // 83 ToggleServer
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
// keeping the color, detail, showhud and friendly-tag globals; the tip and
// its once-counters clear as the next start from the menu clears them.
void hud_toggles_reset_mission(HudToggleState &state);

// An S2C 0x0F folded with the death screen up forces the declutter level to
// the blank level through the same live seam the cycle uses (the replica's
// HudDetailBlankCommand; a join's 0x0F with the death screen down leaves it)
// [orig: NapiNPClientMsg_0x00F `cmp g_DeathScreenActive` @0x42E3F5 / @0x42E407
//  -> @0x42E410..0x42E41C — level = 3, then the visibility rebuild].
void hud_toggles_death_screen(HudToggleState &state);

// The escape action's HUD-window close chain: out of a session the key is
// dead while the spawn gate holds (the special-key handler owns it there);
// the pause word clears first; then the first open window in the witnessed
// order closes and consumes the key; a showing tip starts its fade; with none
// open the respawn init runs and the embedder opens the in-game menu. Closing
// the map legend runs the respawn init too (its close keeps nothing else).
// Legs with nothing to close in the port (docs/interface/hud-re.md D-HUD-31):
// g_EpilogScreenActive, cleared beside the pause word, is only ever raised
// while the SP spawn gate holds (the SP round end sets the gate before the
// epilog cine starts, and a session never runs the epilog), so this chain
// never sees it; dword_24C18D0 and dword_B76494 have no reachable setter;
// the cine editor dword_24C18B8 opens only from its own dialog. The quit
// dialog closes third, after the voice-macro menus; with none open the tail
// runs the init keeping the dialog word, then opens the dialog on the
// authority's server-status view in a session, else the in-game menu.
// [orig: Input_HandleActionBinding case 18 @0x49b234: the SP spawn-gate
//  return @0x49b243; the pause/epilog clear @0x49b24f..0x49b261 ->
//  @0x49b3cd..0x49b3d3; the order D4 @0x49b267, D8 @0x49b27a, 1880
//  @0x49b28d, message log @0x49b2a0, help @0x49b2b3, briefing & 2 @0x49b2c6,
//  D0 @0x49b2da, objectives @0x49b2ed, B76494 @0x49b300, cine @0x49b313, map
//  legend + keeping init @0x49b32d, tip @0x49b34d; the init @0x49b36f, the
//  server-view arm @0x49b377..0x49b38f and game.mnu INGAME @0x49b3b6]
struct HudEscapeInput {
	bool in_session = false;
	bool spawn_gate = false; // [orig: g_SpawnSuccessGate]
	bool authority = false;  // [orig: g_NapiNPCtx.is_authority]
};
uint32_t hud_toggles_escape(HudToggleState &state, const HudEscapeInput &input);

// The session create / destroy's view rule: in a session a dedicated host
// (an authority that is not a peer) comes up on the status view, every other
// role and every session end clears it.
// [orig: Server_InitNewRoundState @0x51cb43..0x51cb5d — callers
//  CNapiGameSession_BuildAndCreateSession @0x5695b3,
//  CNapiGameSession_FullDestroy @0x4c9780, SinglePlayer_StartMission
//  @0x561c23, Game_InitSubsystems @0x4a6d76]
void hud_toggles_session_init(HudToggleState &state, bool in_session, bool mp_session_peer);

// The special-key handler's quit-dialog leg, the first in its chain: while
// the dialog is up the YES key runs action 3 (Exit Mission: the embedder
// plays the PU_EXIT_CONFIRM interface sound, sets the exit reason and drops
// the connection), the NO key closes it, the RESTART key queues the restart
// only out of a session, and a digit 1..9 or ':' is taken (its numbered
// save loads only out of a session). The yes / no / restart keys are the
// first characters of gametext KeyPress/STRKEYPRESS_YES / _NO / _RESTART as
// written (defaults 'Y', 'N', 'R' when a string is empty). A key the leg
// takes is consumed: it never reaches the action rows.
// While the dialog is up the rest of the chain's exclusive legs (the menus'
// digits, the round-over keys, the Tab board's and the status page's page
// keys) are skipped (kChainTaken); its tail (the deploy keys, the help and
// briefing pages) still runs.
// [orig: Input_HandleSpecialKeys @0x49c5c0 — @0x49c5df..0x49c6d1; the keys
//  Input_InitBindingSystem @0x49a58e..0x49a5a2 (the defaults into
//  dword_B3B73C / 40 / 44), the first-character `movsx` @0x49a5d1;
//  action 3 = catalog row 77 `exit`, case 3 @0x49af1f..0x49af42; the save
//  loader sub_439680 @0x4396a0 refuses in a session; the caller
//  Input_ProcessKeyboardEvents @0x49d2fb skips the binding scan on a take]
struct HudQuitDialogKeyInput {
	int vk = 0;
	int yes_vk = 'Y';
	int no_vk = 'N';
	int restart_vk = 'R';
	bool in_session = false;
};
namespace hud_special_key {
inline constexpr uint32_t kConsumed = 0x1;      // the key never reaches the rows
inline constexpr uint32_t kChainTaken = 0x2;    // the dialog was up: skip the exclusive legs
inline constexpr uint32_t kQuitConfirmed = 0x4; // action 3 ran (Exit Mission)
inline constexpr uint32_t kRestartQueued = 0x8; // event 12, out of a session only
} // namespace hud_special_key
uint32_t hud_toggles_quit_dialog_key(HudToggleState &state, const HudQuitDialogKeyInput &input);

// The special-key handler's round-over leg, reached once the round-over gate
// holds (the round end raised it) and no earlier leg took the chain: out of a
// session, or in a co-op game type without bit 0x20000, EVERY key is taken
// there — the RESTART key (gametext KeyPress/STRKEYPRESS_RESTART's first
// character, 'R' by default) restarts the round (the embedder re-runs the SP
// splash over a custom loading background, then the world takes the
// restart: the end screen down and exit reason 4), ESC leaves the mission
// (exit reason 1), any other key does nothing. In a session the leg's page
// and ADVANCED keys (the end-of-round board's) are not this leg's; it
// returns 0 there.
// [orig: Input_HandleSpecialKeys @0x49c5c0 — the g_SpawnSuccessGate arm
//  @0x49c7cf; the session/co-op test @0x49c7d5..0x49c7fd; the RESTART key
//  (dword_B3B744) @0x49c86b / @0x49c871: g_EpilogScreenActive = 0 @0x49c879,
//  the splash re-run under g_LoadScreenHasCustomBg && !is_in_session
//  @0x49c883..0x49c899, Input_QueueEvent(12) @0x49c8a3, g_MissionExitReason
//  = 4 @0x49c8ad; every other key returns 1 @0x49c8d5, ESC (27) storing
//  reason 1 @0x49c8e2]
struct HudRoundOverKeyInput {
	int vk = 0;
	int restart_vk = 'R';
	bool in_session = false;
	uint32_t game_type = 0; // the session g_GameType word
};
namespace hud_round_over {
inline constexpr uint32_t kConsumed = 0x1; // the key never reaches the action rows
inline constexpr uint32_t kRestart = 0x2;  // the RESTART arm
inline constexpr uint32_t kExit = 0x4;     // ESC: exit reason 1
} // namespace hud_round_over
uint32_t hud_round_over_key(const HudRoundOverKeyInput &input);

// The SP end-of-round cine's start runs the respawn init over every HUD
// window (the quit dialog, the voice menus, the stats, the briefing, the map
// mode, the message log and the objectives) [orig: Cine_InitPlayback
// @0x57867C / Cine_StartPlayback @0x57792D -> Game_InitRespawnState
// @0x499360]. Returns the hud_toggle_event bits (the map-mode clear).
uint32_t hud_toggles_cine_start(HudToggleState &state, bool in_session);

// The special-key handler's status-page leg, after the Tab board's page
// keys: on a dedicated host, or on the authority's view, Enter, PgUp and PgDn
// step the page cursor and are consumed. The cursor both steppers clamp back
// to 0 (and the page zeroes before its roster) is not modelled: it never
// leaves 0, so only the consumption is observable.
// [orig: Input_HandleSpecialKeys @0x49c960..0x49c9c7; Server_StatusPageNext
//  @0x4fe840, Debug_DecrementPageIndex @0x4fe860 on g_ServerStatusPage
//  (dword_C8FC60); the page's zeroing @0x50a43b]
bool hud_toggles_server_status_page_key(const HudToggleState &state, int vk, bool in_session,
		bool authority, bool mp_session_peer);

// The special-key handler's deploy leg: in a session (or a mission with the
// SinglePlayerRespawn attribute 0x40), with the local player dead or the
// deploy-map overlay up, 'X' queues input event 12 with parameter 0 (the
// 0x0E's 0xFFFF), SPACE with 0xFFFE (the auto-team pick), and a letter A..Z
// the 1-based index of the spawn-zone list entry it names when that zone's
// team byte is the local player's; the overlay alone opens the leg out of a
// session. The key is consumed whenever a pick is queued. Returns the event
// parameter, or -1 when the key is not taken.
// [orig: Input_HandleSpecialKeys @0x49c9c9..0x49ca73 — `cmp is_in_session`
//  @0x49c9c9, `test g_BmsAttribFlags, 40h` @0x49c9d1, `cmp
//  g_DeployScreenActive` @0x49c9da / @0x49c9f1, `test [player+24h], 2`
//  @0x49c9eb; 'X' @0x49c9fd -> @0x49c658, SPACE @0x49ca06 -> 0xFFFE
//  @0x49ca0d, the letter walk SpawnZoneList_GetCount / _GetByIndex and the
//  +0x162 team compare @0x49ca37..0x49ca69; Input_QueueEvent(12, ...)]
struct HudDeployKeyInput {
	int vk = 0;
	bool in_session = false;
	bool single_player_respawn = false; // g_BmsAttribFlags & 0x40
	bool local_dead = false;            // the local player's Flags & 2
	bool deploy_overlay = false;        // g_DeployScreenActive
	uint8_t local_team = 0;             // the local player's team byte (+0x162)
	// The spawn-zone list in list order: each entry's team byte, -1 for an
	// entry that resolves no entity.
	const int16_t *zone_teams = nullptr;
	size_t zone_count = 0;
};
inline constexpr int kHudDeployKeyAutoTeam = 0xFFFE;
int hud_deploy_key_pick(const HudDeployKeyInput &input);

// The tip producers' events, in the order they were raised
// [orig: CTipSystem_HandleEvent @0x5b6ad0 per call site].
void hud_toggles_tip_events(HudToggleState &state, const uint8_t *events, size_t count);
// `frames` main frames of the tip countdown [orig: CTipSystem_TickCountdown
// @0x5b69f0, called once per Game_ProcessMainFrame @0x52675d whether or not
// the SP pause holds (the pause gate @0x526779 comes after it)].
void hud_toggles_tip_frames(HudToggleState &state, int frames);
// The SP restart's reset: the mission reset's windows and latches, but the
// tip's once-counters survive (a start from the menu clears them)
// [orig: Game_RestartRoundSP @0x5263db -> Game_StartMission(1) ->
// CTipSystem_Reset(0) @0x525df2; the first start's Game_StartMission(0)
// passes 1].
void hud_toggles_restart_round(HudToggleState &state);

// A menu pick closes its menu with a plain store, no respawn init (the
// special-key handler's digit arms; the pick itself is the controls poll's).
// [orig: Input_HandleSpecialKeys — `dword_24C18D4 = 0` @0x49c75c (emotes),
//  `dword_24C18D8 = 0` @0x49c7a8 (radio)]
void hud_toggles_close_voice_menu(HudToggleState &state, bool radio);

// The verbose toggle's toast key [orig: STRMISC_VERBOSE_ON / _OFF @0x49b78f].
const char *verbose_toast_key(bool verbose);

// The friendly-tags cycle 0->1->2->3->0 with its retail toast key (gametext
// Misc/STRMISC_FRIENDLYTAGS_*) [orig: Input_HandleActionBinding case 30
// @0x49b573 -> Chat_AddMessageChannel2 @0x49bc60; the keys @0x49b596 /
// @0x49b5c1 / @0x49b5d0 / @0x49b5da]. Returns the toast key for the new mode.
const char *hud_toggles_cycle_friendly_tags(HudToggleState &state);
const char *friendly_tag_toast_key(FriendlyTagMode mode);

} // namespace opennova::hud
