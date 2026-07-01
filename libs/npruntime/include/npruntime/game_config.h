#pragma once

#include <cstdint>
#include <string>
#include <vector>

// GameConfig — the ONE consolidated in-match server-state config (ADR 0013, D-NET-132; §6.9). It
// merges the three formerly-separate reimpl structs (NapiGameSettings §6.4, ServerRules, and the
// §5.1 SessionReplyConfig) into a single source of truth mirroring the CAdminServer `SET` field set
// witnessed in §6.9 [orig: CAdminServer_HandleSetCommand @0x405a60]. The wire serializers read
// through it; merging changed no byte on the retail-captured goldens.
//
// The one genuine value-collapse the merge performs is `game_type`: the original has ONE
// `g_GameType @0x24D2128` global that BOTH `ServerConfig_SerializeToPacket @0x505bd0` (the S2C 0x08
// block, dword[3]) AND `NapiNPMsg_0x7B_BuildPayload @0x507740` (the S2C 0x7B/0x60 bodies) read — the
// prior reimpl split it into an unseeded rules copy (0x08) and a separate reply `gametype` (0x7B),
// which is the artifact this merge removes. `CNapiServerConfig_BuildFlags @0x4c4dc0` reads a
// `game_settings.game_type` copy (ctx+0xCC) that is a snapshot of `g_GameType` in a live session;
// modeled here as the same `game_type` field (equal by construction on every seeded path).
namespace opennova::np {

struct GameConfig {
	// --- §6.4 identity (lobby name + wire server name + join-gate passwords) -----------------------
	// server_name feeds the lobby/session name [orig create_session -> np_protocol.session_name /
	// nstmout_path @proto+0x288] AND the wire bodies [orig g_server_name_str @0x24D1FC4: the S2C 0x2C
	// NetPacket_WriteServerNameAndMapFile @0x505780 + the 0x7B/0x60 serverName]; the original keeps
	// several synced copies, unified here.
	std::string server_name = "OpenNova Dev";  // [orig g_server_name_str @0x24D1FC4 / game_settings +0x00]
	std::string server_password;               // [orig game_settings +0x20] BuildFlags |0x8
	std::string side_a_password;               // [orig game_settings +0x40] BuildFlags |0x20; join-reject 19
	std::string side_b_password;               // [orig game_settings +0x60] BuildFlags |0x10; join-reject 20
	std::string internet_address;              // [orig game_settings +0x80] connect-target; default "0.0.0.0"
	uint32_t max_players = 1;                   // [orig game_settings +0xC0] clamped 1..65
	uint32_t use_lineup_queue = 0;             // [orig game_settings +0xC4]
	uint32_t lineup_queue_size = 0;            // [orig game_settings +0xC8]

	// g_GameType @0x24D2128 — the ONE gametype global. Read by the 0x08 block dword[3], the 0x7B/0x60
	// reply bodies, the BuildFlags team-gate (game_settings.game_type copy, equal in a live session),
	// and Server_AssignPlayerTeam. Default 0 (a fresh/dev host); a real host seeds the mission gametype.
	uint32_t game_type = 0;                     // [orig g_GameType @0x24D2128 == game_settings +0xCC]
	// mpattrib bitmask — the BuildFlags team-branch input [orig game_settings +0xD0] AND the 0x64
	// mission-metadata blob's attrib dword. Observed bits: 0x001 NoTracers, 0x004 TeamChoose,
	// 0x008 FFWarning-suppress, 0x200 NoFriendlyFire, 0x400 NoFriendlyTag, 0x8000 ClaymorePref.
	uint32_t mp_attributes = 14854;             // [orig game_settings +0xD0]

	// --- §6.9 rule globals — the S2C 0x08 ServerConfig block [orig: ServerConfig_SerializeToPacket
	// @0x505bd0]. dword[3] is `game_type` above; the rest are the standalone g_* rule globals in wire
	// order. Default 0 for a dev host; a real host / the golden seeds them (retail frame 146:
	// [respawn 30, timelimit 10, _, gametype, _, score 50, _, startdelay, _, _]).
	uint32_t respawn_time = 0;         // [orig g_respawn_time @0x24D2140]      dword[0]; SET `GameTime`
	uint32_t time_limit_minutes = 0;  // [orig g_time_limit_minutes @0x24D2144] dword[1]; SET `KOTHLimit`
	uint32_t config_word_2 = 0;       // [orig dword_24D2120]                  dword[2] (semantic UNWITNESSED)
	uint32_t config_word_4 = 0;       // [orig dword_24D2130]                  dword[4] (semantic UNWITNESSED)
	uint32_t score_limit = 0;         // [orig g_score_limit @0x24D2134]       dword[5]; SET `KillLimit` (name-swap)
	uint32_t config_word_6 = 0;       // [orig dword_24D214C]                  dword[6] (semantic UNWITNESSED)
	uint32_t start_delay = 0;         // [orig g_StartDelay @0x24D2160]        dword[7]; SET `StartDelay`
	uint32_t config_word_8 = 0;       // [orig dword_24D2164]                  dword[8] (semantic UNWITNESSED)
	uint32_t config_word_9 = 0;       // [orig dword_24D2168]                  dword[9] (semantic UNWITNESSED)
	uint8_t config_bytes[7] = {0, 0, 0, 0, 0, 0, 0}; // [orig byte_24D234C..byte_24D2360 + dword_24D2110 low byte]

	// CNapiServerConfig_BuildFlags @0x4c4dc0 inputs beyond game_settings (the g_rules_flags bitfield
	// sources): the trailing flags dword of the 0x08 block. (`MaxScore`->`g_kill_limit @0x24D2138`
	// (§6.9 name-swap) is NOT in the 0x08 wire block — omitted until a cfg-persistence pass needs it.)
	bool squad_enforced = false;       // [orig g_squad_max_players @0x2550924 != 0] -> |0x2000
	std::string squad_required_tag;    // [orig g_squad_required_tag @0x2550928]      -> |0x4000
	bool permanent_death = false;      // [orig g_MpPermanentDeath @0x2550C9C]        -> |0x8000
	bool config_flag_2550A04 = false;  // [orig dword_2550A04 & 4]   (semantic UNWITNESSED) -> |0x4
	bool config_flag_2550CA4 = false;  // [orig dword_2550CA4]       (semantic UNWITNESSED) -> |0x10000

	// --- §5.1 reactive-reply mission / player identity + advertised spawn ---------------------------
	// The server / mission / player fields the reactive NapiNPServerMsg_0x0NN reply handlers read
	// (server_message_dispatch.h). The replicated-entity / spawn-point inputs are NOT here — the
	// faithful world-stream burst sources those from World + bms::File.
	std::string mission_name = "AS - Dormant Volcano Isle"; // [orig title: MissionText "info"/"title" / dword_24D1FA4]
	std::string mission_file = "ASH_I5A.BMS";               // [orig g_map_file_name @0x24D1F3E]
	std::string expansion = "jox01";                        // [orig g_ExpansionName @0xB4C584] (0x7B)
	std::string player_name = "DevUser";                    // [orig player_data+128] (0x7B name / roster)
	std::string pcid;                                       // [orig entity+592] PCID (0x7A body / 0x7B field 2);
	                                                        // empty on a dev host -> the 0x7A body is a single NUL
	uint32_t spawn_x = 0xfe56f854u;
	uint32_t spawn_y = 0x0049f5f0u;
	uint32_t spawn_z = 0x003a5e6au;
	std::vector<std::string> spawn_names;
	std::vector<uint8_t> mission_header_blob;
};

} // namespace opennova::np
