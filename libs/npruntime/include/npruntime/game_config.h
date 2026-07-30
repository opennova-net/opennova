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
	// mission-metadata blob's attrib dword. Observed bits (docs/net/novaworld-net-re.md §6.4;
	// [orig: CNapiServerConfig_BuildFlags @0x4c4dc0]):
	static constexpr uint32_t kMpAttribNoTracers = 0x001;
	static constexpr uint32_t kMpAttribTeamChoose = 0x004;
	static constexpr uint32_t kMpAttribFFWarningSuppress = 0x008;
	static constexpr uint32_t kMpAttribNoFriendlyFire = 0x200;
	static constexpr uint32_t kMpAttribNoFriendlyTag = 0x400;
	static constexpr uint32_t kMpAttribClaymorePref = 0x8000;
	uint32_t mp_attributes = 14854;             // [orig game_settings +0xD0]
	// Authoritative projectile game-option globals. These do not alter the
	// advertised mp_attributes word; the host simulation consumes them directly.
	bool fat_bullets = false;                   // [orig g_FatBullets @0x24D21A0]
	bool one_shot_kill = false;                 // [orig g_OneShotKill @0x24D219C]

	// --- §6.9 rule globals — the S2C 0x08 ServerConfig block [orig: ServerConfig_SerializeToPacket
	// @0x505bd0]. dword[3] is `game_type` above; the rest are the standalone g_* rule globals in wire
	// order. Default 0 for a dev host; a real host / the golden seeds them (retail frame 146:
	// [respawn 30, timelimit 10, _, gametype, _, score 50, _, startdelay, _, _]).
	// The five formerly-UNWITNESSED words were named 2026-07-01 by tracing each 0x08-block global to
	// its Config_ParseSettingsLine @0x54f740 setting-name compare (via apply_session_settings_to_globals
	// @0x551500, which copies the parsed cfg global into the live rule global).
	uint32_t respawn_time = 0;         // [orig g_respawn_time @0x24D2140]      dword[0]; SET `GameTime`
	uint32_t time_limit_minutes = 0;  // [orig g_time_limit_minutes @0x24D2144] dword[1]; SET `KOTHLimit`
	uint32_t replay_enabled = 0;      // [orig g_replay_enabled @0x24D2120 <- cfg `replay` @0x2550B24]      dword[2]
	uint32_t max_team_lives = 0;      // [orig g_max_team_lives @0x24D2130 <- cfg `max_team_lives` @0x2550ABC] dword[4]
	uint32_t score_limit = 0;         // [orig g_score_limit @0x24D2134]       dword[5]; SET `KillLimit` (name-swap)
	uint32_t respawn_timeout = 0;     // [orig g_respawn_timeout @0x24D214C <- cfg `timeout` @0x2550B34]    dword[6];
	                                  //   read by GameEvent_PlayerDeath @0x516dd0 / Server_UpdateBotMovement
	uint32_t start_delay = 0;         // [orig g_StartDelay @0x24D2160]        dword[7]; SET `StartDelay`
	uint32_t destroy_buildings = 0;   // [orig g_destroy_buildings @0x24D2164 <- cfg `destroybuild` @0x2550ACC] dword[8];
	                                  //   read by Entity_ApplyWeaponDamage @0x4e6820
	uint32_t death_messages = 0;      // [orig g_death_messages @0x24D2168 <- cfg `deathmes` @0x2550AD0]    dword[9];
	                                  //   read x3 by GameEvent_PlayerDeath @0x516dd0
	uint8_t config_bytes[7] = {0, 0, 0, 0, 0, 0, 0}; // [orig byte_24D234C..byte_24D2360 + dword_24D2110 low byte]

	// CNapiServerConfig_BuildFlags @0x4c4dc0 inputs beyond game_settings (the g_rules_flags bitfield
	// sources): the trailing flags dword of the 0x08 block. (`MaxScore`->`g_kill_limit @0x24D2138`
	// (§6.9 name-swap) is NOT in the 0x08 wire block — omitted until a cfg-persistence pass needs it.)
	bool squad_enforced = false;       // [orig g_squad_max_players @0x2550924 != 0] -> |0x2000
	std::string squad_required_tag;    // [orig g_squad_required_tag @0x2550928]      -> |0x4000
	bool permanent_death = false;      // [orig g_MpPermanentDeath @0x2550C9C]        -> |0x8000
	// Both flags named 2026-07-01: dword_2550A04 is the mpattrib BITFIELD store itself
	// (ServerConfig_ApplyHostSetting @0x4a6000: SET `TeamChoose`->bit 0x4 direct, `TeamFF`->0x200
	// inverted, `FriendlyTag`->0x400 inverted, `ClaymorePref` ...).
	bool team_choose = false;          // [orig dword_2550A04 & 4 = SET `TeamChoose` @0x4a63d9] -> |0x4
	bool allow_sniper_scope_zoom = false; // [orig g_mp_allowsniperscopezoom @0x2550CA4, cfg
	                                      //  `mp_allowsniperscopezoom` @0x550ac9; read by
	                                      //  WeaponSlot_InitFromDef @0x53ee70] -> |0x10000

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
