#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <net/npwire/game_type.h> // game_rules::kDefault* (the Config_SetDefaults baseline)
#include <net/npwire/protocol_message.h>

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

// The bidirectional game-session packet ceiling installed by the witnessed
// settings update. A large 0x0A uses a LEN16 message envelope (flags, tag,
// u16 length), so its body must leave room for both that envelope and the
// thirteen-byte sequenced connection header.
inline constexpr std::size_t kGameSessionMaxPacketBytes = 1300;
inline constexpr std::size_t kProtocolMessageLen16Bytes = 4;
inline constexpr std::size_t kMaxFrameUpdateBodyBytes =
		kGameSessionMaxPacketBytes - PROTOCOL_PACKET_HEADER_SIZE -
		kProtocolMessageLen16Bytes;
static_assert(kMaxFrameUpdateBodyBytes == 1283);

// The lobby player-cap ceiling: retail clamps the advertised max_players to
// 1..65 (the 64-player roster + the host) before storing game_settings +0xC0.
// [orig: the 1..65 clamp on game_settings +0xC0 — see GameConfig::max_players]
inline constexpr uint32_t kMaxPlayersCap = 65;

// The session family that selects retail's default send divider. `Automatic`
// is resolved from the installed transport by create_session: socketless is
// SinglePlayer and a socketed session is LAN unless its caller explicitly
// identifies the NovaWorld channel.
enum class GameSessionChannel : uint8_t {
	Automatic = 0,
	SinglePlayer,
	Lan,
	NovaWorld,
};

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
	// Signed spectator limit: 0 disables spectating, -1 shares max_players,
	// and a positive value adds that many spectator-only capacity slots.
	// [orig: HostDialog_ReadSettings @0x555940; CNapiNetwork_ValidateJoinRequest
	// @0x4c61b0]
	int32_t spectator_slots = 0;
	std::string spectator_password;             // [orig SPECTATOR_PW; BuildFlags |0x4000]
	// (retail game_settings +0x80 internet_address, +0xC4 use_lineup_queue and
	//  +0xC8 lineup_queue_size have no reader here and are not modelled.)
	uint32_t max_players = 1;                   // [orig game_settings +0xC0] clamped 1..kMaxPlayersCap

	// g_GameType @0x24D2128 — the ONE gametype global. Read by the 0x08 block dword[3], the 0x7B/0x60
	// reply bodies, the BuildFlags team-gate (game_settings.game_type copy, equal in a live session),
	// and Server_AssignPlayerTeam. Default 0 (a fresh/dev host); a real host seeds the mission gametype.
	uint32_t game_type = 0;                     // [orig g_GameType @0x24D2128 == game_settings +0xCC]
	// Process-global side count. Only TDM/TKOTH/FlagBall honor four; every
	// other team mode remains two-sided in the retail scoreboards.
	// [orig: g_MpNumTeams @0x2550B40 -> g_num_teams_config @0x24D2150;
	// Server_BuildAndBroadcastScoreboard @0x50D960]
	uint8_t num_teams = 2;
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
	// The host-global class availability word sent to every joining client as
	// S2C 0x76. Retail derives its ten low bits from the per-class host settings
	// and the selected mission-list entry; all classes enabled is the stock
	// default. [orig: g_hostClassAllowMask @0x24D59FC;
	// NetPacket_WriteClassAllowMask @0x510350]
	uint16_t class_allow_mask = 0x03FFu;
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
	// Retail carries this setting through cfg/global/S2C 0x08 but never reads
	// the live global in gameplay (whole-image xrefs: serializer + settings
	// apply only). Keep its wire value; do not invent a team-lives system.
	uint32_t max_team_lives = 0;      // [orig g_max_team_lives @0x24D2130 <- cfg `max_team_lives` @0x2550ABC] dword[4]
	uint32_t score_limit = 0;         // [orig g_score_limit @0x24D2134]       dword[5]; SET `KillLimit` (name-swap)
	// Gameplay-only rule globals omitted from S2C 0x08 but consumed by the
	// witnessed KOTH/flag win and return paths.
	uint32_t max_score = 0;           // [orig g_kill_limit @0x24D2138] SET `MaxScore`
	uint32_t koth_delta = 5;          // [orig dword_24D2148] cfg `koth_delta`
	uint32_t flag_return_ticks = 210; // [orig g_FlagReturnTime_2 @0x24D2174]
	int32_t capture_duration_seconds = 15; // [orig g_capture_duration @0x24D2248] `TakeoverTime`
	int32_t capture_speed_setting = 1;     // [orig g_capture_speed_setting @0x24D2254]
	int32_t spawn_wave_time_base = 0;      // [orig g_spawn_wave_time_base @0x24D224C]
	int32_t spawn_wave_time_zone = 10;     // [orig g_spawn_wave_time_zone @0x24D2250]
	// True limits target-less deployment to the absence of an eligible same-team
	// spawn zone. The clean name reflects the actual predicate; retail's global
	// g_respawn_requires_team_dead and cfg key `nodefaultspawnpoints` are
	// historical misnomers and are retained only as provenance.
	// [orig: apply_session_settings_to_globals @0x551D96;
	// Server_ProcessClientRequestRespawn @0x519C8E;
	// Entity_HasAliveEntityOfTeam @0x4FC7B0]
	uint32_t default_spawn_requires_no_team_zone = 0;
	uint32_t respawn_timeout = 0;     // [orig g_respawn_timeout @0x24D214C <- cfg `timeout` @0x2550B34]    dword[6];
	                                  //   read by GameEvent_PlayerDeath @0x516dd0 / Server_UpdateBotMovement
	uint32_t start_delay = 0;         // [orig g_StartDelay @0x24D2160] dword[7]; SET `StartDelay`;
	                                  //   reset_round_counters @0x516C50 copies it to g_preround_delay_timer @0xC8D824 (store @0x516C8D)
	uint32_t destroy_buildings = 0;   // [orig g_destroy_buildings @0x24D2164 <- cfg `destroybuild` @0x2550ACC] dword[8];
	                                  //   read by Entity_ApplyWeaponDamage @0x4e6820
	uint32_t death_messages = 0;      // [orig g_death_messages @0x24D2168 <- cfg `deathmes` @0x2550AD0]    dword[9];
	                                  //   read x3 by GameEvent_PlayerDeath @0x516dd0
	uint8_t config_bytes[7] = {0, 0, 0, 0, 0, 0, 0}; // [orig byte_24D234C..byte_24D2360 + dword_24D2110 low byte]

	// S2C 0x58's 39 signed STROVER_STATVAR point values. Retail initializes the
	// per-game-type table, then overlays score.ini VERSION 40 before answering
	// the client's C2S 0x2D world-load request. The Godot resource binding feeds
	// the mounted score.ini through load_session_score_config; a headless caller
	// may seed the structural values directly. [orig: GameType_CreateDefaultSettings
	// @0x52DD00 -> ScoreConfig_LoadFile @0x52D8A0; Server_BuildStatusReport
	// @0x530A60 copies row+300..+452]
	// Absent selects GameType_CreateDefaultSettings for game_type. A parsed or
	// explicitly supplied row is present even when every value is zero.
	std::optional<std::array<int32_t, 39>> session_status_stat_values;
	// score.ini FIELD rows for the selected game type, in file order. Empty
	// means the match should use GameType_CreateDefaultSettings' retail schema.
	// The second byte is preserved rather than normalized because it is emitted
	// verbatim in the S2C 0x56 end-round board.
	std::vector<std::pair<uint8_t, uint8_t>> scoreboard_fields;

	// CNapiServerConfig_BuildFlags @0x4c4dc0 inputs beyond game_settings (the g_rules_flags bitfield
	// sources): the trailing flags dword of the 0x08 block. `MaxScore` is retained
	// above for gameplay/session-status, but retail does not put it in this 0x08 block.
	bool permanent_death = false;      // [orig g_MpPermanentDeath @0x2550C9C]        -> |0x8000
	// dword_2550A04 is already represented once by mp_attributes above. In
	// particular SET `TeamChoose` writes bit 0x4 directly; there is no parallel
	// boolean setting in retail. [orig: ServerConfig_ApplyHostSetting @0x4A6000,
	// TeamChoose arm @0x4A63D9]
	bool allow_sniper_scope_zoom = false; // [orig g_mp_allowsniperscopezoom @0x2550CA4, cfg
	                                      //  `mp_allowsniperscopezoom` @0x550ac9; read by
	                                      //  WeaponSlot_InitFromDef @0x53ee70] -> |0x10000

	// --- §5.1 reactive-reply mission / player identity + advertised spawn ---------------------------
	// The server / mission / player fields the reactive NapiNPServerMsg_0x0NN reply handlers read
	// (server_message_dispatch.h). The replicated-entity / spawn-point inputs are NOT here — the
	// faithful world-stream burst sources those from World + bms::File.
	std::string mission_name = "AS - Dormant Volcano Isle"; // [orig title: MissionText "info"/"title" / dword_24D1FA4]
	std::string mission_file = "ASH_I5A.BMS";               // [orig g_map_file_name @0x24D1F3E]
	std::string custom_text = "Put your message here.";     // [orig g_sessionvar_custom_text @0x522123]
	std::string expansion = "jox01";                        // [orig g_ExpansionName @0xB4C584] (0x7B)
	// The active expansion's version checksum — retail's g_expansion_checksum
	// @0xB4C5A4: 0 unless a loose expansion/<name>/version.txt exists, else its
	// CRC (vfs_expansion_version_checksum ports the producer
	// [orig: Expansion_LoadAssets @0x4a4781/@0x4a4885]). The join gate compares
	// it against the client's VERSIONCRCSTRING only while `expansion` is
	// non-empty [orig: @0x51231e..0x512349, reject DPC=48] (D-NET-166).
	int32_t expansion_version_checksum = 0;
	// Optional exact retail resource corpus used to validate the host's inbound
	// C2S 0x20/0x21 integrity replies. Empty (or an unknown id at the binding
	// boundary) disables validation: a host must never compare against guessed
	// table bytes. This is deliberately independent of `expansion`, because two
	// installs with the same expansion name can carry different patched data.
	std::string integrity_profile;
	std::string player_name = "DevUser";                    // host identity/roster name; remote 0x7B uses recipient ClientAuth.NA
	std::string pcid;                                       // [orig entity+592] PCID (0x7A body / 0x7B field 2);
	                                                        // empty on a dev host -> the 0x7A body is a single NUL
	uint32_t spawn_x = 0xfe56f854u;
	uint32_t spawn_y = 0x0049f5f0u;
	uint32_t spawn_z = 0x003a5e6au;
	std::vector<std::string> spawn_names;
	std::vector<uint8_t> mission_header_blob;
	// The per-frame 0x0A byte cap [orig: g_entity_send_budget @0xC8FC50, the
	// BANDWIDTH server command — atol/5 clamped 100-1600 @0x50b884/@0x50b890;
	// the round-start initializer resets retail to 600 @0x51ca7c].
	// start_host_session applies it to the netsim global at bring-up.
	// The retail round-start default is 600 bytes. The configure_host_session
	// "bandwidth" lever remains an explicit per-session override.
	uint32_t entity_send_budget = 600;
	// The send-holdoff period this host dictates to each joiner (H:0x00 mask 8 /
	// CS field 3) AND runs itself — the per-session-type "tick rate" divider
	// [orig: NapiNPServer_GetSendHoldoffTicks @0x4c4ab0 — 1 only for
	// transport mode 0 (SP/none), 12 for modes 1/3 (NovaWorld), LAN authority
	// g_LanMode 1..4 -> 12/6/4/3; the loopback's per-tick cadence is the
	// NapiNPServer_UpdateHoldoffTicks @0x4c5f40 else-branch @0x4c5f63/@0x4c5f69,
	// not a GetSendHoldoffTicks case].
	// `send_holdoff_ticks` is an explicit override; absent means select the
	// witnessed value from session_channel and lan_mode. Retail's invalid
	// lanmode fallback is mode 2 (period 6). Loopback connections themselves
	// remain per-tick and do not consume this remote-peer setting.
	GameSessionChannel session_channel = GameSessionChannel::Automatic;
	uint32_t lan_mode = 1;
	std::optional<uint32_t> send_holdoff_ticks;

	// The fixed player-slot table grows only for a positive spectator limit.
	// -1 enables spectators inside max_players; 0 disables them. Retail's
	// network validation performs the same signed add before its 251-row slot
	// table clamp. [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0]
	uint32_t total_player_slot_capacity() const {
		const uint64_t total = static_cast<uint64_t>(max_players) +
				(spectator_slots > 0 ? static_cast<uint32_t>(spectator_slots) : 0u);
		return static_cast<uint32_t>(total < 251u ? total : 251u);
	}

	uint32_t effective_send_holdoff_ticks(
			GameSessionChannel automatic_fallback = GameSessionChannel::SinglePlayer) const {
		if (send_holdoff_ticks.has_value()) return *send_holdoff_ticks;
		GameSessionChannel channel = session_channel;
		if (channel == GameSessionChannel::Automatic) channel = automatic_fallback;
		switch (channel) {
		case GameSessionChannel::NovaWorld:
			return 12;
		case GameSessionChannel::Lan:
			switch (lan_mode) {
			case 1: return 12;
			case 2: return 6;
			case 3: return 4;
			case 4: return 3;
			default: return 6;
			}
		case GameSessionChannel::Automatic:
		case GameSessionChannel::SinglePlayer:
		default:
			return 1;
		}
	}
};

// Seed a config with the fresh-host rule defaults the retail config path
// applies before a mission starts -- the one place the game_rules baseline
// lands on the live wire fields (a dev host that skips this stays inert at
// zero). [orig: Config_SetDefaults @0x54D030 -> apply_session_settings_to_globals
// @0x551500; GameType_CreateDefaultSettings @0x52dd00]
inline void apply_fresh_host_rule_defaults(GameConfig &config) {
	config.respawn_time = game_rules::kDefaultRespawnTime;
	config.time_limit_minutes = game_rules::kDefaultTimeLimitMinutes;
	config.replay_enabled = game_rules::kDefaultReplayEnabled;
	config.max_team_lives = game_rules::kDefaultMaxTeamLives;
	config.score_limit = game_rules::kDefaultScoreLimit;
	config.max_score = game_rules::kDefaultMaxScore;
	config.koth_delta = game_rules::kDefaultKothDelta;
	config.flag_return_ticks = game_rules::kDefaultFlagReturnTicks;
	config.capture_duration_seconds = game_rules::kDefaultCaptureDurationSeconds;
	config.capture_speed_setting = game_rules::kDefaultCaptureSpeedSetting;
	config.spawn_wave_time_base = game_rules::kDefaultSpawnWaveTimeBase;
	config.spawn_wave_time_zone = game_rules::kDefaultSpawnWaveTimeZone;
	config.default_spawn_requires_no_team_zone = game_rules::kDefaultSpawnRequiresNoTeamZone;
	config.num_teams = static_cast<uint8_t>(game_rules::kDefaultNumTeams);
	config.respawn_timeout = game_rules::kDefaultRespawnTimeout;
	config.start_delay = game_rules::kDefaultStartDelay;
	config.destroy_buildings = game_rules::kDefaultDestroyBuildings;
	config.death_messages = game_rules::kDefaultDeathMessages;
}

} // namespace opennova::np
