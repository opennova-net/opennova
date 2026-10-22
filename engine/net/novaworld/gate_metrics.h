#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

// Six retail METPROTOCOL reporters. Keep the ordered fields: PLAYER emits
// GLANG/GTZB twice whenever the gate's METEXT is set -- the game locale pair,
// then the player's own (+556/+588) pair; the one gate is g_IsDedicatedServer,
// a misnomer whose one writer is the gate reply's METEXT (D-NET-347)
// [orig: Server_SendPlayerMetricsToGate @0x4fae5f; CNapiGateManager_ProcessResponse
//  @0x4cf2a1..0x4cf2c1]. PING nests one
// ENTRY/ENDENTRY row group per sorted ping entry (BIP1..BIP4, MS)
// [orig: Server_SendPingMetricsToGate @0x511BF0].
struct GateMetricsField {
	std::string name;
	std::string value;
	bool quoted = false;
};
struct GateMetricsReport {
	std::string block;
	std::vector<GateMetricsField> fields; // the block's own rows, in order
	// PING's `\tENTRY` .. `\tENDENTRY` groups, in order; each holds its
	// `\t\tKEY value` rows. Empty for the five flat reporters.
	std::vector<std::vector<GateMetricsField>> entries;
};

// Payload after NAPI envelope removal. Failure leaves report unchanged.
bool gate_metrics_decode(const uint8_t *data, size_t size, GateMetricsReport &report);

// One phase clock the SERVER, SERVERMISSION, PLAYER and PLAYERMISSION reporters print: retail's
// CServerTick, four of them (the process's, its mission's, and each player slot's two, at slot
// +97568 and +98964). The phase is 0 none, 1 loading the mission, 2 waiting to begin, 3 playing,
// 4 the game-over stats; a phase's time accrues into its counter when it is left. The per-entity
// kill tallies past +0x94 belong to no printed row and are not kept.
// [orig: CServerTick_Construct @0x5017D0; CServerTick_Reset @0x4F9F40; CServerTick_SetState
//  @0x4FA090; CServerTick_UpdateMaxPlayerCounts @0x4FA130]
struct GateServerTick {
	uint32_t state = 0;            // +0x00
	uint32_t start_ms = 0;         // +0x04: RUNNING's origin
	uint32_t phase_start_ms = 0;   // +0x08
	uint32_t loading_ms = 0;       // +0x0C: LOADINGMISSION
	uint32_t waiting_ms = 0;       // +0x10: WAITINGTOBEGIN
	uint32_t playing_ms = 0;       // +0x14: PLAYING
	uint32_t gameover_ms = 0;      // +0x18: GAMEOVERSTATS
	int32_t mission_completes = 0; // +0x1C: NUMMISSIONCOMPLETES, one per entry into phase 4
	int32_t cycles = 0;            // +0x20: NUMCYCLES, one per phase 4 -> 1
	std::string gcc;               // +0x24, char[32]: GCC
	std::string mission_file;      // +0x44, char[64]: MISSIONFILENAME
	int32_t max_players = 0;       // +0x84: MAXPLAYERS
	int32_t most_players = 0;      // +0x88: MOSTPLAYERS
	int32_t most_team1 = 0;        // +0x8C
	int32_t most_team2 = 0;        // +0x90

	// Everything zeroed, the clock stamped, the GCC the CC.BIN read (Napi_CopyString's 32-byte
	// cap) and MAXPLAYERS the config's player count; then the most-players high marks over the
	// live slots (update_max_player_counts, the caller's counts).
	// [orig: CServerTick_Construct @0x5017D0 - the stamp @0x5017F1, the mission file's NUL
	//  @0x501870, Game_ReadCCBinFile @0x501873, Napi_CopyString @0x501883, maxPlayers_3F4
	//  @0x501893, the high marks @0x501899]
	void construct(uint32_t now_ms, std::string_view cc_bin, int32_t config_max_players,
	               int32_t players, int32_t team1, int32_t team2);
	// The phase and its counters zeroed and the clock stamped; the GCC's first byte and the
	// MAXPLAYERS / MOSTPLAYERS words zeroed with them, the mission file kept.
	// [orig: CServerTick_Reset @0x4F9F40 - @0x4F9F47..0x4F9F64, the GCC byte @0x4F9F67, +33..+38
	//  @0x4F9F6A..0x4F9F88]
	void reset(uint32_t now_ms);
	// A change of phase: the old phase's time accrues (4 -> 1 counts a cycle), the phase and its
	// stamp set, and entering 4 counts a mission complete. The same phase is no change.
	// [orig: CServerTick_SetState @0x4FA090 - the test @0x4FA09A, the arms @0x4FA0B1..0x4FA0E2,
	//  @0x4FA0E5 / @0x4FA0EA, the count @0x4FA0EF]
	void set_state(uint32_t new_state, uint32_t now_ms);
	// The report's own close of the running phase: its time accrues (no cycle or completion
	// counted), the phase back to 0 and its stamp now; nothing while no phase runs.
	// [orig: Server_ProcessPeriodicTimerUpdate @0x4FBF50 - @0x4FBF72..0x4FBFD3 (the process's),
	//  @0x4FBFDF..0x4FC040 (the mission's); Server_TickUpdate @0x51D9D4..0x51DA3B (the mission's),
	//  the slots' @0x51DAD0..0x51DB1B]
	void close_phase(uint32_t now_ms);
	// The most-players high marks: each raised to the count when the count is above it.
	// [orig: CServerTick_UpdateMaxPlayerCounts @0x4FA130 - @0x4FA1BF..0x4FA1DF]
	void update_max_player_counts(int32_t players, int32_t team1, int32_t team2);
};

// The machine locale the dedicated-only rows print: GetLocaleInfoA(LOCALE_USER_DEFAULT,
// LOCALE_SENGCOUNTRY / LOCALE_SENGLANGUAGE) into 512 bytes, "" when the call fails, and
// GetTimeZoneInformation's Bias. The embedder reads it. [orig: Server_SendMetricsToGate
//  @0x4FB211..0x4FB2FD]
struct GateMetricsLocale {
	std::string country_name;
	std::string language;
	int32_t tz_bias = 0;
};

// The process-wide strings every reporter prints: the gate reply's METLABEL (g_ServerLabel) and
// LOBBYNAME (g_LobbyName), the version text (g_VersionText), and the gate reply's METEXT
// (g_IsDedicatedServer), which alone gates the locale rows.
struct GateMetricsHost {
	std::string label;
	std::string lobby;
	std::string version;
	bool met_ext = false;
	GateMetricsLocale locale;
};

// A net player's strings from its ClientAuth (NapiNetConfig at CNetPlayer +204): VERSIONSTRING
// (PV, +228), COUNTRYCODE (PCC, +260), CNT (PCOUNTRYNAME, +524), LNG (+556), TZB (+588), and the
// PID string at +592. [orig: NapiNetConfig_LoadFromConnTags @0x4C7260 - VERSIONSTRING
//  @0x4C73C8 (+24), COUNTRYCODE (+56), TZB @0x4C76C2 (+0x180)]
struct GateMetricsPlayer {
	std::string version;
	std::string country_code;
	std::string country_name;
	std::string language;
	int32_t tz_bias = 0;
	std::string pid;
};

// One PING entry, sorted by the caller: the peer's four address bytes and its round trip in ms.
struct GateMetricsPingEntry {
	uint8_t ip[4] = {0, 0, 0, 0};
	int32_t ms = 0;
};

// The six blocks' text, field for field as retail's sprintf chain builds it ("%ld" over the
// unsigned tick words, every string row quoted as is). `now_ms` is the reporter's GetTickCount.
// [orig: Server_SendServerMetricsToGate @0x4FA4B0 - @0x4FA4F7..0x4FA9B8]
std::string gate_metrics_server_text(const GateMetricsHost &host, std::string_view game_type,
                                     const GateServerTick &tick, uint32_t now_ms);
// [orig: Server_SendMetricsToGate @0x4FB0A0 - @0x4FB0E7..0x4FB328: LABEL, the CC.BIN read
//  (Game_ReadCCBinFile @0x4FB17C), GV]
std::string gate_metrics_id_text(const GateMetricsHost &host, std::string_view cc_bin);
// [orig: Server_SendMissionMetrics @0x4FB3A0 - @0x4FB3E7..0x4FB878]
std::string gate_metrics_server_mission_text(const GateMetricsHost &host,
                                             std::string_view game_type,
                                             const GateServerTick &mission, uint32_t now_ms);
// `server_gcc` is the process tick's GCC (byte_C87044).
// [orig: Server_SendPlayerMetricsToGate @0x4FAA30 - @0x4FAAA8..0x4FB02B]
std::string gate_metrics_player_text(const GateMetricsHost &host, std::string_view game_type,
                                     std::string_view server_gcc, const GateServerTick &slot,
                                     const GateMetricsPlayer &player, uint32_t now_ms);
// `mission` is the mission tick (its file byte_C875DC and GCC byte_C875BC), `slot` the slot's
// own mission tick (+98964).
// [orig: Server_SendPlayerMissionMetrics @0x4FB8F0 - @0x4FB982..0x4FBEDB]
std::string gate_metrics_player_mission_text(const GateMetricsHost &host,
                                             std::string_view game_type,
                                             const GateServerTick &mission,
                                             const GateServerTick &slot,
                                             const GateMetricsPlayer &player, uint32_t now_ms);
// Each entry's six rows go in only while the text is under 0x8000 bytes.
// [orig: Server_SendPingMetricsToGate @0x511BF0 - @0x511C5B..0x511E6D, the entries
//  @0x511ECB..0x512048 (the length test @0x511EF0), ENDBLOCK @0x512079]
std::string gate_metrics_ping_text(const GateMetricsHost &host, std::string_view cc_bin,
                                   const std::vector<GateMetricsPingEntry> &sorted_entries);

// The datagram a reporter hands the socket: the text's strlen bytes (no NUL) through
// Crypto_EncryptBuffer under "1010101" (the NapiNP ADD chain, our nwu_decrypt), then the NAPI CRC
// envelope CNapiNPManager_SendTo's encrypt flag applies. Empty when the envelope fails.
// [orig: Server_SendMetricsToGate - Crypto_EncryptBuffer @0x4FB35B, CNapiNetwork_SendUDPPacket
//  @0x4FB37E; CNapiNetwork_SendUDPPacket @0x4C4D30 -> CNapiNPManager_SendTo(.., 1) @0x4C4D56]
std::vector<uint8_t> gate_metrics_datagram(std::string_view text);

} // namespace opennova
