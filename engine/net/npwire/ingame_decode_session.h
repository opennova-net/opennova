#pragma once

// The session/HUD state channel records and their decoders, split out of
// ingame_decode.h by responsibility (the size ratchet): the S2C session
// configuration and status, the capture-zone timers, the chat and game-text
// lanes, the end-of-round board and the join-time loadout submission. The
// definitions stay in wire/ingame_decode.cpp; ingame_decode.h includes this
// header, so every decoder user keeps one include.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova {

// ===========================================================================
// Session/HUD state channel (§5.48–§5.55) — the 2026-07-01 coverage-sweep wave.
// ===========================================================================

// §5.48 S2C 0x58 — SESSION-STATUS block (server name, mission name, session
// up-time sync, and the end-game scoring-rule table). The client parses it into
// the single global g_SessionStatus (0x24E3E88): server name feeds the
// STROVER_SERVERNAME end-game line, uptime_ms is elapsed-at-send (the client
// stamps GetTickCount at parse so elapsed = wire − parseTick + now,
// CSessionTimer_GetElapsedMS), and the 39 i32s are the per-stat point values
// STROVER_STATVAR00..38 the end-game stats screen awards (positive) or
// penalizes (negative). Historic "texture loader (terrain assets)" note was
// wrong. [orig: NapiNPClientMsg_SessionStatus @ 0x4228C0 →
// SessionStatus_ParseFromBuffer @ 0x530ED0; readers Overlay_BuildEndGameStatsText
// @ 0x54A240, SessionStatus_GetStatPointValue @ 0x52D5D0]
struct SessionStatusKV {
	uint8_t  key = 0;    // key <= 9 kept (first 8 pairs stored)
	uint32_t value = 0;
};
struct SessionStatusBlock {
	std::string server_name;   // cstr; client keeps <= 31 chars
	std::string mission_name;  // cstr; client keeps <= 63 chars
	uint8_t  byte0 = 0, byte1 = 0, byte2 = 0; // → g_SessionStatus[25..27]
	uint32_t uptime_ms = 0;    // session elapsed ms at send time
	int32_t  stat_values[39] = {}; // STROVER_STATVAR00..38 point table
	uint8_t  kv_count = 0;     // wire count; MAY exceed the pairs present (reader
	                           // is bounds-tolerant, missing pairs read as zeros)
	std::vector<SessionStatusKV> kv; // the pairs actually on the wire
	// The writer's unadvertised extra pair: it loops while pair_index <= count,
	// so the zero-initialized report adds one five-byte {0, 0} pair after the
	// counted ones; the reader stops at kv_count and never reads it.
	// [orig: Server_BuildStatusReport @0x530A60; reader @0x531055]
	bool     has_writer_sentinel = false;
	SessionStatusKV writer_sentinel;
	size_t   trailing_bytes = 0; // bytes past the sentinel pair (none in retail)
};
bool decode_session_status(const uint8_t *body, size_t len, SessionStatusBlock &out);

// §5.49 S2C 0x6F — ZONE-TIMER VALUE update (15 B). Programs the per-zone-entity
// timer entry the capture/takeover HUD reads: value/limit are SECONDS on the
// wire, scaled ×62 into 62 Hz ticks by the client; rate is the per-tick
// increment (the client advances value += rate each frame, clamped at limit).
// Also tracks the nearest zone entity to the local player for the takeover
// widget. NOT a cinematic-camera message (historic label was wrong).
// [orig: NapiNPClientMsg_ZoneTimerValue @ 0x428D60 → ZoneTimerList_SetEntryValue
//  @ 0x537EC0; per-frame advance Client_ProcessNetworkFrame @ 0x42C2E6;
//  consumer HUD_DrawTakeoverStatus @ 0x59B630]
struct ZoneTimerValue {
	uint16_t zone_handle = 0;  // (pool<<12)|slot of the zone entity
	uint8_t  mode = 0;
	int32_t  value_s = 0;      // current value, 16.16 fixed seconds (client ×62 → tick-fixed)
	int32_t  limit_s = 0;      // clamp limit, 16.16 fixed seconds (golden: 1.0 for owned zones)
	int16_t  rate = 0;         // per-tick accumulator increment
	uint8_t  byte544 = 0;      // → zone entity+544
	uint8_t  byte545 = 0;      // → zone entity+545
};
bool decode_zone_timer_value(const uint8_t *body, size_t len,
                             ZoneTimerValue &out, size_t &consumed);

// §5.49 S2C 0x53 — ZONE-TIMER WINDOW update (9 B). The companion channel of the
// same zone-timer entry: a [start, end) window in SECONDS (client ×62 → ticks)
// plus a rate byte; mode_b lands at zone entity+547. The tracked-nearest-zone
// adoption additionally requires the zone within 20.0 world units (1310720 in
// 16.16). [orig: NapiNPClientMsg_ZoneTimerWindow @ 0x428AE0 →
//  ZoneTimerList_SetEntryWindow @ 0x537DE0]
struct ZoneTimerWindow {
	uint16_t zone_handle = 0;
	uint8_t  mode_a = 0;
	uint8_t  mode_b = 0;       // → zone entity+547
	uint16_t start_s = 0;      // window start, seconds
	uint16_t end_s = 0;        // window end, seconds
	uint8_t  rate = 0;
};
bool decode_zone_timer_window(const uint8_t *body, size_t len,
                              ZoneTimerWindow &out, size_t &consumed);

// §5.61 S2C 0x6C — the unique contact count for the currently active timed
// capture. Fixed 3 B: [u16 zone handle][u8 count]. The producer clamps the
// advertised byte to 1..32. [orig: NapiNPClientMsg_0x06C @0x428FC0;
// NetPacket_WriteZonePresenceCount @0x506DE0]
struct ZonePresenceCount {
	uint16_t zone_handle = 0;
	uint8_t count = 0;
};
bool decode_zone_presence_count(const uint8_t *body, size_t len,
                                ZonePresenceCount &out, size_t &consumed);

// §5.50 S2C 0x34 — PLAY-SOUND by sound-profile name. flag 0 → flat/ambient
// play; flag 1 → positioned 3D one-shot at full volume (the 3 i16 coords are
// shifted << 16 into 16.16 world space). No position block on the wire when
// flag != 1. Gated is_mp_session_peer. The IDB name "GotoTeleport" was a
// misnomer. [orig: NapiNPClientMsg_PlaySoundByName @ 0x4283A0 →
//  SoundBank_FindSetByNameAnyBank @ 0x5274F0 / Entity_PlaySound3D_FullVolume @ 0x528E20]
// S2C 0x6D: event, raw pool-0 index, signed location-name index.
// [orig: NapiNPClientMsg_HandleEntityDeath @0x430C50]
struct TrackedPlayerVoice {
    uint8_t event = 0;
    uint8_t player_index = 0;
    int16_t location = 0;
};
bool decode_tracked_player_voice(const uint8_t *body, size_t len, TrackedPlayerVoice &out);

struct PlaySoundCommand {
	uint8_t     flag = 0;      // 0 = flat play, 1 = positioned 3D
	std::string sound_name;    // sound-profile name (cstr)
	bool        has_pos = false; // true iff flag == 1 (position block present)
	int16_t     pos_x = 0, pos_y = 0, pos_z = 0; // world units (engine shifts << 16)
};
bool decode_play_sound(const uint8_t *body, size_t len, PlaySoundCommand &out);

// §5.51 S2C 0x2C — SESSION + MISSION-FILE NAME assign: [cstr sessionName]
// [cstr bmsFileName] → byte_A82378 / g_MapFileName; bumps g_LoadingProgress
// to >= 1. A join-burst member; the historic "chat entry" table note was wrong
// (chat-history is 0x2A). Golden values: "Untitled" (the host's session name,
// matching the 0x58 server name) + "TDH_I5A.BMS".
// [orig: NapiNPClientMsg_MissionMapNames @ 0x427E10]
struct MissionMapNames {
	std::string session_name;   // → byte_A82378 (host session/server name)
	std::string map_file_name;  // → g_MapFileName (0x24D1F3E), the .BMS file
};
bool decode_mission_map_names(const uint8_t *body, size_t len, MissionMapNames &out);

// §5.52 chat text channel. C2S 0x0D uplink: [u8 channel][cstr text] — the
// server strips <...> tags, rate-limits 1000 ms/player, prepends name(/squad),
// then fans the formatted line out as S2C 0x14 [u8][u8][cstr] per recipient
// (channel routing: 2=team, 4/5=per-side, 11/12=squad/commander, 13=proximity
// <= 100.0 world units, default=all). The historic C2S 0x0D "replication frame
// ACK" note was wrong. [orig: uplink NapiNPServer_HandleChatMessage @ 0x513760;
//  downlink NapiNPClientMsg_ChatMessage @ 0x42F240 → Chat_DispatchToChannel @ 0x42B910]
struct ChatUplink {
	uint8_t     channel = 0;
	std::string text;
};
bool decode_chat_uplink(const uint8_t *body, size_t len, ChatUplink &out);
struct ChatBroadcast {
	// HEADER ORDER RESOLVED 2026-08-20 (closes D-NET-215): the wire is
	// [channel][sender_slot][cstr text], NOT the reverse. The handler tail-jumps
	// into the dispatcher as Chat_DispatchToChannel(body[1], (char)body[0],
	// &body[2]) [orig: NapiNPClientMsg_ChatMessage @0x42F240], and the
	// dispatcher's own signature settles which is which
	// [orig: Chat_DispatchToChannel @0x42B910]: its FIRST parameter indexes the
	// roster (PlayerSlotTable_GetActiveSlot) and gates the line on the sender
	// being neither muted nor a spectator, while its SECOND is switched over
	// 0..0xE to pick the line colour and sink. So body[1] is the sender and
	// body[0] is the channel.
	//
	// The channel byte is read SIGNED at the call site (`*(char *)packetData`)
	// and widened to int for the switch, so it is kept signed here rather than
	// silently reinterpreted.
	int8_t      channel = 0;     // body[0], sign-extended into the switch
	uint8_t     sender_slot = 0; // body[1], the roster index
	std::string text;            // formatted "name(/squad): text" line
};
bool decode_chat_broadcast(const uint8_t *body, size_t len, ChatBroadcast &out);

// S2C 0x28 — one co-op dialog line the authority's dialog playback started:
// [cstr dialog name][i16 line]. The reader takes the name to its NUL (clamped
// at the body end) and a line that does not fit reads 0; a non-authority then
// plays that line of the named dialog. False (the fields still filled) when
// the body is not exactly that shape.
// [orig: Server_SendEntityStateToAll @0x50A0D0 -> sub_5038A0 @0x5038A0 (the
//  writer); NapiNPClientMsg_0x028 @0x425B40 — the name @0x425b67..0x425b73,
//  the line @0x425b7a..0x425b80]
struct DialogLine {
	std::string dialog_name;
	int16_t line = 0;
};
bool decode_dialog_line(const uint8_t *body, size_t len, DialogLine &out);

// S2C 0x32 — FORMATTED GAME TEXT, the SYSTEM-ring join/leave lines:
// `[i8 subtype][cstr text]`, subtypes 1/2 add `[i8 team]` after the NUL.
// Every other subtype posts nothing. The client resolves the Client gametext
// template per subtype and substitutes the text as `$A`:
//   1 a player joined (`{name, team}`) — team 0: STRCLI22 (as a spectator);
//     game type < 2 or == 8: STRCLI11; teams 1..4: STRCLI12..15; any other
//     team: nothing;
//   2 a player is leaving (STRCLI16);
//   3 a squadron entered (the literal `"$A" squadron has entered.`);
//   4 a squadron left (no line — the case exists but posts nothing);
//   5 a player became a spectator (STRCLI24).
// The subtype and team bytes are read SIGNED (movsx); a short body reads 0
// for each byte and the text is the bytes up to the NUL or the body end.
// [orig: NapiNPClientMsg_0x032 @0x428060 — the subtype read @0x42807b, the
//  movsx switch @0x428080..0x428099, the team byte past the NUL
//  @0x4280ac..0x4280d5, the key picks @0x4280df..0x428155; the senders
//  Server_PlayerAdd {1, slot+0x28, slot+0x1A0} @0x51d21e..0x51d291 and {3,
//  node name} @0x51d18e..0x51d20e, Server_HandlePlayerDisconnect {2, name,
//  team} via NetPacket_SerializeMinimapSlot_0 @0x51b6a8..0x51b6d3 and {4,
//  node name} @0x51b705..0x51b781, the spectator convert {5, name}
//  @0x519edc..0x519f43 — every one reliable, send mask 0x80]
inline constexpr int8_t kGameTextPlayerJoined = 1;
inline constexpr int8_t kGameTextPlayerLeaving = 2;
inline constexpr int8_t kGameTextSquadronEntered = 3;
inline constexpr int8_t kGameTextSquadronLeft = 4;
inline constexpr int8_t kGameTextPlayerSpectating = 5;
struct FormattedGameText {
	int8_t subtype = 0;
	std::string text;
	int8_t team = 0;   // subtypes 1/2 only
};
// True for subtypes 1..5 (the handled switch), false for every other; `clean`
// reports a well-formed body (terminated text, the team byte present for 1/2,
// nothing trailing).
bool decode_formatted_game_text(const uint8_t *body, size_t len, FormattedGameText &out,
		bool *clean = nullptr);

// §5.68 S2C 0x56 — END-OF-ROUND STAT BOARD. Everything the post-round
// stat.mnu screen shows arrives here, and it arrives CHUNKED and PULLED: each
// datagram is [u16 total_size][u16 chunk_offset][chunk], written into a
// reassembly stream at its offset, and the board is complete once
// chunk_offset + chunk_len >= total_size. A zero offset RESETS the stream, so
// a re-sent board never merges with the previous one
// [orig: NapiNPClientMsg_0x056 @0x431D10 — the envelope reads @0x431D4D /
//  @0x431D61, the reset @0x431D79, the completion test @0x431D9F]. The whole
// handler is gated on the spawn-success gate [orig: @0x431D33].
//
// The server never pushes a chunk: it cuts at most 200 bytes from its board
// stream at a CLIENT-REQUESTED offset and replies to that client alone
// [orig: NapiNPServerMsg 0x2B @0x514FE0 -> NetPacket_WriteReplayStreamChunk
//  @0x506F60, the 200 clamp @0x506FB9; the stream is filled by
//  Server_BuildEndOfRoundScoreboard @0x508F30 from Server_ProcessRoundEnd
//  @0x5164F0, the call @0x516590]. While a chunk leaves the board
// incomplete the client answers with C2S 0x2B [u16 chunk_offset + chunk_len]
// [orig: @0x431DB3..0x431DC4],
// and on completion raises g_ScoreboardDirty, the stat.mnu trigger
// [orig: @0x4321BE; polled by UI_ProcessEndRoundScreenTransition @0x5B8600].
//
// Reassembly AND the 0x2B request leg deliberately stay with the CALLER: the
// stream is per-session state with a lifetime the decoder does not own, and
// retail keeps it in one global for the same reason. A consumer that only
// decodes and never sends 0x2B receives the first 200 bytes and nothing else.
// S2C 0x1D. TWO forms, picked by session state on both ends — never by
// length [orig: `is_in_session && !(g_GameType & 0x10000)` @0x5052a6 serializer,
// @0x43086c..0x430883 client]. Team/offline form: exactly seven bytes,
// [i8 winner][i16 score0][i16 score1][u8 draw][i8 index]. Non-team in-session
// (DM/KOTH-family) form: the winner/team-score words are REPLACED by the top
// three rows of the frozen board — three C-string names then three i16
// primary scores — before the same draw/index tail. The receiving client
// keeps at most 31 chars + NUL of each name while its cursor advances by the
// sender's full string. The final signed byte is this recipient's row in the
// frozen board (-1 when absent). [orig: EndRoundScoreboard_SerializeHeader
// @0x505280 — names @0x5052bf.., scores @0x50535e..0x505381;
// NapiNPClientMsg_0x01D @0x430840 — named parse @0x430889..0x4309af, name
// commit Napi_CopyString(dst, 32) @0x430a70..0x430a92]
struct EndRoundHeader {
	int8_t winner_team = 0;
	int16_t team_score_0 = 0;
	int16_t team_score_1 = 0;
	uint8_t draw = 0;
	int8_t player_index = -1;
	// Non-team form only; an empty name marks an absent board row.
	std::string player_names[3];
	int16_t player_scores[3] = {0, 0, 0};
};
bool decode_end_round_header(const uint8_t *body, size_t len,
		bool non_team_form, EndRoundHeader &out);

// C2S 0x2B, the next byte offset requested by the client.
struct EndRoundStatsRequest {
	uint16_t offset = 0;
};
bool decode_end_round_stats_request(const uint8_t *body, size_t len,
		EndRoundStatsRequest &out);

struct EndRoundStatsChunk {
	uint16_t total_size = 0;   // bytes in the whole board
	uint16_t chunk_offset = 0; // where this chunk lands; 0 also means "restart"
	std::vector<uint8_t> chunk;
	// True when this chunk completes the board (offset + size >= total).
	bool complete() const {
		return size_t(chunk_offset) + chunk.size() >= size_t(total_size);
	}
};
bool decode_end_round_stats_chunk(const uint8_t *body, size_t len,
                                  EndRoundStatsChunk &out);

// One player row of the reassembled board. The seven stat words are in WIRE
// order, which is NOT the order retail stores them in (score is read fourth
// but written to slot 5, captures fifth to slot 4) — the storage shuffle is
// the screen's column layout, not the wire's
// [orig: the row loop @0x431F3C..@0x4320E1].
struct EndRoundPlayerRow {
	uint8_t slot = 0;
	std::string name;   // clipped to 31 chars + NUL by the copy loop
	std::string clan;
	std::string tag;    // the squad tag
	uint8_t team = 0;
	uint8_t player_class = 0; // entity+660, the selected soldier type
	int16_t kills = 0;
	int16_t deaths = 0;
	int16_t assists = 0;
	int16_t score = 0;
	int16_t captures = 0;
	int16_t flags = 0;
	int16_t special = 0;
	// One score per declared team field, positional against `team_fields`.
	std::vector<int16_t> per_team;
	// The board draws "<clan> <name>" when a clan is present, else the bare
	// name [orig: the sprintf @0x4320D7 vs the copy loop @0x4320E1..0x432100].
	// Retail then STORES that join through a 32-byte copy (31 chars) and the
	// tag through an 8-byte one [orig: Napi_CopyString @0x432110 / @0x432120]
	// — the fold's truncation, which the decoder leaves to the fold.
	std::string display_name() const {
		return clan.empty() ? name : clan + " " + name;
	}
};

// The reassembled board.
struct EndRoundStats {
	// Stored into the winner-team global despite the decompiler naming it
	// gameType [orig: the store @0x431E1E].
	int8_t winner_team = 0;
	int16_t team_score_0 = 0;
	int16_t team_score_1 = 0;
	// The declared team columns: one {field, enabled} pair each, and the count
	// also sizes every row's per_team array and the trailing matrix's columns
	// [orig: the pair loop @0x431E6E]. The count byte is SIGNED like the
	// team-row count (movsx @0x431E69): a byte >= 0x80 declares no fields.
	std::vector<std::pair<uint8_t, uint8_t>> team_fields;
	std::vector<EndRoundPlayerRow> players;
	// Trailing per-team score matrix: team_rows x team_fields.size()
	// [orig: the row/col loops @0x4321A2..@0x4321D6].
	std::vector<std::vector<int16_t>> team_rows;
};

// Parse a COMPLETE reassembled board. Retail tolerates truncation here — every
// read is bounds-checked and yields 0/empty past the end, leaving a partly
// zeroed board — but this decoder follows npwire's protocol-cursor contract
// instead and rejects a short stream outright, so a truncated board cannot
// half-decode into a plausible scoreboard.
bool decode_end_round_stats(const uint8_t *data, size_t len, EndRoundStats &out);

// §5.53 S2C 0x04 — SESSION SLOT CONFIG (24 B): four leading i32s the handler
// skips, then [u8 sessionConfig][u8 teamMode][u8 maxPlayers] (maxPlayers drives
// PlayerSlotTable_Reallocate), one more skipped i32, and a trailing byte.
// [orig: NapiNPClientMsg_SessionSlotConfig @ 0x425410]
struct SessionSlotConfig {
	uint32_t skipped[4] = {};   // on the wire, not read by the handler
	uint8_t  session_config = 0; // → dword_24D2110
	uint8_t  local_player_slot = 0; // → g_LocalPlayerSlotId = g_LocalPlayerSlotId, the recipient's
	                                // OWN roster slot [orig: the write side is slot+20,
	                                // NetPacket_WriteSlotAssignment @0x502b30; the old
	                                // `team_mode` reading was a misnomer, witness 2026-07-03]
	uint8_t  max_players = 0;    // → g_MaxPlayerSlots = g_MaxPlayerSlots + PlayerSlotTable_Reallocate;
	                             // the 0x46/0x22 roster walk terminates at this count
	uint32_t skipped4 = 0;       // on the wire, not read
	uint8_t  trailing = 0;       // → byte_A85B48
};
bool decode_session_slot_config(const uint8_t *body, size_t len, SessionSlotConfig &out);

// §5.54 S2C 0x08 — SESSION CONFIG (fixed 51 B; the historic "game-state
// snapshot ~2 KB" note was wrong): [10 × i32][7 × u8][u32 bitflags]. fields[3]
// = gameType (→ g_GameType); bitflags bits 13/15/16 are latched into
// byte_A821EE/EF/F0. Bumps g_LoadingProgress to >= 1.
// [orig: NapiNPClientMsg_HandleSessionConfig @ 0x4281D0]
struct SessionConfig {
	int32_t  fields[10] = {};  // → dword_A821BC..A821E0; fields[3] = gameType
	uint8_t  bytes[7] = {};    // → byte_A821E8..ED + dword_24D2110
	uint32_t bitflags = 0;     // → dword_A821E4 (bits 13/15/16 latched)
};
bool decode_session_config(const uint8_t *body, size_t len, SessionConfig &out);

// §5.55 S2C 0x02 — JOIN POSITION-ACK + PADDING PROBE. The handler reads only
// [i32 posX][i32 posY][i32 paddingLen]; the rest of the (typically ~512 B) body
// is ignored filler. The client replies C2S 0x02 = position + paddingLen random
// bytes (NetPacket_WritePositionWithPadding) and resets its send holdoff.
// The decoder consumes the filler explicitly (filler_bytes = len - 12).
// [orig: NapiNPClientMsg_HandleJoinResponse @ 0x42E0F0]
struct JoinPaddingProbe {
	int32_t  pos_x = 0;
	int32_t  pos_y = 0;
	uint32_t padding_len = 0;  // random-filler byte count the client must echo
	size_t   filler_bytes = 0; // trailing wire bytes after the 12-B header
};
bool decode_join_padding_probe(const uint8_t *body, size_t len, JoinPaddingProbe &out);

// §5.56 C2S 0x2F — LOADOUT SUBMIT (spawn-menu accept). The client uploads its
// server-assigned team, chosen character class, current weapon slot, and the
// ADM weapon-slot picks of its per-side profile kit; the server validates the
// envelope FIRST (team 1..4 — above 4 only in a team-less game type @0x5158a9;
// a NONZERO class must be 5..9 @0x5158b1, else the handler aborts @0x515fa5 and
// only re-sends the player's current slot list), writes the class to entity+660
// (playerClass — class 0 applies with an empty soldier-type mask @0x5159af),
// rebuilds the avatar display list + weapon slots, and replies with the S2C 0x5A
// weapon-slot list. The in-range class is remapped through the host class-allow
// mask g_HostClassAllowMask @0x24D59FC (@0x5158e6) before the [5,9]-else-8 tail
// @0x515913. Entries repeat until an 0xFF adm_index terminator — the same
// {typeId, ammoP, ammoS, variant} slot vocabulary as the §5.30 S2C 0x5A downlink.
// [orig: builder NetPacket_SendLoadoutSubmit @ 0x42cdc0 (team = byte_A85B48,
//  the S2C 0x04 tail byte; class = the profile's per-side class byte; slot =
//  195 pre-Player_InitPlayer, g_CurrentWeaponSlot after);
//  decoder NapiNPServerMsg_HandlePlayerLoadout @ 0x515790]
struct LoadoutSubmitEntry {
	uint8_t adm_index = 0;      // AdmDef index (0xFF = list terminator, not stored)
	uint8_t ammo_primary = 0;   // clamped to admEntry[83], scaled by admEntry[22]
	uint8_t ammo_secondary = 0; // sub-entry ammo
	uint8_t variant = 0;        // → player+89688+admEntry[1]
};
struct LoadoutSubmit {
	uint8_t  team = 0;              // wire byte 0; read SIGNED, accepted 1..4 [orig: byte_A85B48]
	uint8_t  player_class = 0;      // wire byte 1; 0 or 5..9 → entity+660 playerClass
	uint32_t weapon_slot_index = 0; // selected weapon slot (entity+280 binding)
	std::vector<LoadoutSubmitEntry> entries;
	bool     terminated = false;    // saw the 0xFF terminator
};
bool decode_loadout_submit(const uint8_t *body, size_t len, LoadoutSubmit &out);

} // namespace opennova
