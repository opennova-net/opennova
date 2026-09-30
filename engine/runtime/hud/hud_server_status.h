#pragma once

// THE SERVER-STATUS PAGE: the authority's full-screen console page that
// replaces the whole scene frame while the status view is up (a listen
// host's ToggleServer view, a dedicated host's standing display). It lists
// the host's player-slot roster, the server line, the team block, the round
// clock, the host statistics, a ticker, the console lines, and the windows
// that ride it (the message log, the chat input line, the quit dialog, the
// player score list, the end-round transition). The page redraws at most
// every 200 ms (every 10 s while the window is inactive) and draws nothing
// in between, so the previous page stays on screen.
// [orig: Server_DrawStatusScreen @0x50a2d0, drawn INSTEAD of
//  Render_ProcessMainSceneFrame by GameLoop_RenderFrame @0x521cd6..0x521cef
//  while g_ServerStatusViewActive && is_authority]
// The feed that fills the roster and the host statistics from the host's
// server context is inmatch/server_status_feed.h; the compile is
// HudFrameCompiler::compile_server_status_page (hud_frame_server_status.cpp);
// the witness record is docs/interface/hud-re.md "The quit dialog and the
// server-status view".

#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {

// One player slot of the host's table, as the page reads it [orig: the
// g_PlayerSlots row, stride 100584; the fields the page and the score list
// read, by offset].
struct ServerStatusSlot {
	bool active = false;      // +4, the slot holds a player
	bool local = false;       // +5, the host's own slot
	// +0x178E3 (96483): the bot byte. Its only writers store 0
	// [orig: @0x51a878; net-re "Absent roster features"], so it reads clear;
	// carried for the idle column's gate.
	bool bot = false;
	bool in_game = false;     // +0x20 == 6, the slot's in-game state
	bool loading = false;     // +0x188E3 (100579), not yet in game
	bool spectator = false;   // +0x188D7 (100567), the spectator latch
	bool entity_dead = false; // the slot's entity (+0) with health (+286) <= 0
	uint8_t team = 0;         // +0x1A0
	// +0x1C0: the status word the roster's rights colours test. Its only
	// writers store 0 [orig: CServerTick_SetPhase @0x5100ca, Server_OnPlayerJoin
	// @0x51a764], so the rights arm never colours a row.
	uint32_t status_word = 0;
	int32_t class_word = 0;   // +0x15EDC (89820), the soldier class
	// Seconds since the slot's connection last parsed a session packet
	// ((GetTickCount - conn+0x5E8) / 1000; 0 without a connection)
	// [orig: slot+0x1C -> +0xB8 -> +0x5E8 @0x50a525..0x50a561].
	int32_t idle_seconds = 0;
	int32_t points = 0;            // stats field 0x1C (raw[29])
	int32_t flag_captures = 0;     // stats field 0x0B (raw[12])
	int32_t objective_seconds = 0; // +0x170AC (94380), the hill seconds
	std::string name;              // +0x28
};

// The gametext strings the page draws (the embedder resolves them).
struct ServerStatusText {
	std::string empty_slot;       // Server/STRSRV01
	std::string server_novaworld; // Server/STRSRV02
	std::string server_lan;       // Server/STRSRV03
	std::string team_wins;        // Server/STRSRV04
	std::string team2;            // Server/STRSRV05
	std::string team1;            // Server/STRSRV06
	std::string ties;             // Server/STRSRV07
	std::string team_scores;      // Server/STRSRV08
	std::string frames;           // Server/STRSRV10
	std::string total_logins;     // Server/STRSRV11
	std::string cpu;              // Server/STRSRV17
	std::string score_list_title; // Server/STRSRV23
	std::string start_timer;      // Server/STRSRV24
	std::string current_logins;   // Server/STRSRV25
	// GameType_GetAbbreviation(g_GameType, 1): the GateTypeAbbrev text
	// [orig: @0x50a3d6, sprintf'd as a FORMAT with no argument @0x50a3e4].
	std::string game_type_abbreviation;
};

// One frame's page facts.
struct ServerStatusPageState {
	// [orig: g_NapiNPCtx.is_mp_session_peer]: a listen host (peer) lists
	// slot i from 0 and shows the SYSTEM ring; a dedicated host skips its own
	// slot 0 (slot i + 1) and shows the CHAT ring.
	bool mp_session_peer = true;
	int capacity = 0;   // [orig: g_PlayerSlotCapacity @0x24C0CA4]
	int slot_limit = 0; // [orig: dword_24D211C, the session's slot limit]
	std::vector<ServerStatusSlot> slots; // indexed by slot
	bool novaworld = false;   // [orig: transport_mode == NovaWorld @0x50a7db]
	uint32_t session_key = 0; // [orig: ctx+0x1194 via sub_4C4DB0 @0x4c4db0]
	std::string server_name;  // [orig: g_ServerNameStr @0x24D1FC4]
	uint32_t game_type = 0;   // [orig: g_GameType]
	// The round tallies [orig: g_RoundWinsTeam1 @0xC8FF0C, g_RoundWinsTeam2
	// @0xC8FF10, g_TotalRoundsPlayed @0xC8FF1C].
	int32_t round_wins_team1 = 0;
	int32_t round_wins_team2 = 0;
	int32_t rounds_played = 0;
	// The two team records' stats and hold timers, [0] team 1, [1] team 2
	// [orig: g_TeamRecords[1] / [2] — CPlayerStats_GetFieldPlusOne(+0x44, 0x1C
	//  / 0x0B), unknown_040[272]].
	int32_t team_points[2] = {0, 0};
	int32_t team_flag_captures[2] = {0, 0};
	int32_t team_hold_seconds[2] = {0, 0};
	int32_t round_time_remaining = -1; // [orig: g_RoundTimeRemaining], ticks
	int32_t frames = 0;                // [orig: dword_24C193C]
	int32_t cpu_percent = 0;           // [orig: g_StatsCpuPercent]
	uint32_t pre_round_delay = 0;      // [orig: g_PreRoundDelayTimer], seconds
	uint32_t total_logins = 0;         // [orig: ctx+0x11A8]
	// The page's own window, the player score list [orig:
	// g_ServerStatusScoreListOpen]. The message log, the chat input line, the
	// quit dialog and the end-round transition ride HudFrameState, which the
	// scene frame's HUD shares.
	bool score_list_open = false;
	ServerStatusText text;
};

// The page's gametext: the Server section strings, and the game type's
// GateTypeAbbrev text (the Co-op family's "COOP") [orig: the
// GameText_GetString("Server", ..) calls of Server_DrawStatusScreen and
// HUD_DrawPlayerScoreList; GameType_GetAbbreviation(g_GameType, 1) @0x520fd0,
// whose dword_24C1930 & 0x10000 arm has no writer].
ServerStatusText server_status_text(const GameTextLookup &gametext, uint32_t game_type);

// The quit dialog's Overlays key: STROVER_QUITSERVER on an authority in a
// session, STROVER_QUITCLIENT for a joiner, STROVER5 out of a session
// [orig: UI_DrawDisconnectReasonDialog @0x5b8edc..0x5b8eff].
const char *quit_dialog_text_key(bool in_session, bool authority);

// The page's throttle: a draw is due once 200 ms passed since the last one,
// or, while the window is inactive, once 10 s passed; the first draw seeds
// the last stamp one second back.
// [orig: Server_DrawStatusScreen @0x50a305..0x50a35d — byte_24C10A0+0x2C;
//  the inactive-window flag dword_3342E90, Game_WindowProc @0x762670 /
//  @0x762697]
inline constexpr uint32_t kServerStatusRedrawMs = 200;
inline constexpr uint32_t kServerStatusInactiveRedrawMs = 10000;
bool server_status_page_due(uint32_t *last_ms, uint32_t now_ms, bool window_active);

// The roster grid: ceil(capacity / 30) columns, doubled under three; rows
// enough for the capacity; columns 1010 / columns apart from x 7; rows 540 /
// rows apart from y 52 [orig: @0x50a3ec..0x50a424, the walk
// @0x50a44d..0x50a7d5].
struct ServerStatusRosterGrid {
	int columns = 0;
	int rows = 0;
	int column_step = 0;
	int row_step = 0;
};
ServerStatusRosterGrid server_status_roster_grid(int capacity);

// One roster cell's text and colour; `slot_index` is the slot the cell
// shows (i on a peer, i + 1 on a dedicated host). A slot past the limit
// prints nothing and keeps the colour of the cell before it
// (`previous_color`).
// [orig: @0x50a487..0x50a79b]
struct ServerStatusRosterCell {
	std::string text;
	uint32_t color = 0xFFFFFFFFu;
};
ServerStatusRosterCell server_status_roster_cell(const ServerStatusPageState &page,
		int slot_index, uint32_t previous_color);

// The class word's one-letter code [orig: GameType_GetShortCodeWChar
// @0x4fd9a0: 1..9 -> d r t U M S G R E, else ?].
char server_status_class_code(int32_t class_word);

// The player score list's row for slot `index` (the host's own slot and
// every index from 64 draw nothing: `drawn` false), with its position
// [orig: HUD_DrawPlayerScoreList @0x500300].
struct ServerStatusScoreRow {
	bool drawn = false;
	int x = 0;
	int y = 0;
	std::string text;
	uint32_t color = 0;
};
ServerStatusScoreRow server_status_score_row(const ServerStatusPageState &page, int index);

} // namespace opennova::hud
