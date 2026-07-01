#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "npruntime/napi_np_connection.h"
#include "npruntime/server_message_dispatch.h" // np::SessionReplyConfig — the §5.1 reactive-reply config (P8)

// Forward declarations — the runtime holds non-owning pointers to the authoritative world and
// the in-match replication seam. No World/codec headers are pulled into this header, and there
// is NO socket and NO Godot code anywhere under libs/npruntime (libs/CLAUDE.md).
namespace opennova::world {
class World;
}
// The loaded mission, read by the P3 initial-state burst for the S2C 0x0B BMS-header body
// (bms::encode_header_blob). Forward-declared (NOT included) so bms.h stays out of this light header.
namespace opennova::bms {
struct File;
}
namespace opennova::np {

// [orig +0x5C] The host/client connection mode written by [orig: CGameSession_SetConnectionMode
// @0x4c49f0] (§5.0 / §6.3). It decomposes into the two booleans is_authority (is_host) and
// is_mp_session_peer (is_client). Single player / co-op listen server = HostClient.
enum class ConnectionMode : uint32_t {
	None = 0,       // is_host 0, is_client 0
	HostOnly = 1,   // is_host 1, is_client 0 — dedicated server
	ClientOnly = 2, // is_host 0, is_client 1 — join a remote host
	HostClient = 3, // is_host 1, is_client 1 — single-player / co-op listen server
};

// [orig +0x50] transport_mode — the NETWORK TYPE (NovaWorld vs LAN) (§6.3). Distinct from
// socket_state below: the UI maps NovaWorld -> SetTransportMode(4) and LAN -> SetTransportMode(2),
// while a single-player host calls SetTransportMode(1) directly (§5.0 step 2). NovaWorld-only
// AppId/JoinTicket gates branch on this == NovaWorld.
enum class NetworkType : uint32_t {
	NovaWorld = 1,
	Lan = 2,
};

// [orig +0x54] socket_state — the value [orig: CNapiNetwork_SetTransportMode @0x4c8750] writes.
// 1 = socketless (the host's own local client is in-process); 2/3/4 reach
// [orig: CNapiNetwork_OpenTransportSocket @0x4c6a40] and open a UDP socket (§5.0 step 2). SP = 1.
//
// NOTE (refines the plan's "reuse netsim::TransportMode at +0x50"): the witness puts the
// loopback-vs-socket selector in socket_state (+0x54), and the network-type enum in
// transport_mode (+0x50) — two separate fields (§6.3). netsim::TransportMode stays the
// per-connection mode on NapiNPConnection.link, which is the right home for it.
enum class SocketMode : uint32_t {
	Socketless = 1,       // single player / in-process loopback (no socket)
	Lan = 2,              // LAN socket
	HostClientSocket = 3, // host + client over a socket
	NovaWorldSocket = 4,  // NovaWorld-routed socket
};

// [orig +0xE68] NapiGameSettings (216 B inline; §6.4). The same struct SP and MP fill — SP uses
// server_name = "SINGLEPLAYERGAME", max_players = 1.
struct NapiGameSettings {
	std::string server_name;      // [orig +0x00] char[32] lobby-visible name
	std::string server_password;  // [orig +0x20] char[32]
	std::string side_a_password;  // [orig +0x40] char[32] mismatch -> join-reject 19
	std::string side_b_password;  // [orig +0x60] char[32] mismatch -> join-reject 20
	std::string internet_address; // [orig +0x80] char[64] default "0.0.0.0"
	uint32_t max_players = 1;      // [orig +0xC0] clamped 1..65
	uint32_t use_lineup_queue = 0; // [orig +0xC4]
	uint32_t lineup_queue_size = 0;// [orig +0xC8]
	uint32_t game_type = 0;        // [orig +0xCC] mp_gametype enum
	uint32_t mp_attributes = 0;    // [orig +0xD0] mpattrib bitmask
};

// [orig: ServerConfig_SerializeToPacket @0x505bd0] The host game-rules block the §5.2a initial-state
// burst serializes as S2C 0x08 (10 dwords + 7 bytes + a flags dword = 51 B). These mirror the
// original's g_* rule globals (g_respawn_time @0x24D2140, g_time_limit_minutes @0x24D2144,
// g_GameType @0x24D2128, g_score_limit @0x24D2134, g_StartDelay @0x24D2160, ...). Default 0 for a
// headless dev host; a real host / the golden test seeds them (verified vs retail-lan-host-join
// frame 146: [respawn 30, timelimit 10, 1, gametype 0x10000, 100, score 50, 5, 0, 0, 1]). The four
// config_word_* fields are witnessed in the wire layout but their gameplay semantics are not yet
// pinned (kept named by position). The bool/string flag inputs feed CNapiServerConfig_BuildFlags
// @0x4c4dc0 (the trailing flags dword); the squad/perm-death/misc globals it reads default off.
struct ServerRules {
	uint32_t respawn_time = 0;        // [orig g_respawn_time @0x24D2140]   dword[0]
	uint32_t time_limit_minutes = 0; // [orig g_time_limit_minutes @0x24D2144] dword[1]
	uint32_t config_word_2 = 0;      // [orig dword_24D2120]               dword[2] (semantic UNWITNESSED)
	uint32_t game_type = 0;          // [orig g_GameType @0x24D2128]       dword[3]
	uint32_t config_word_4 = 0;      // [orig dword_24D2130]               dword[4] (semantic UNWITNESSED)
	uint32_t score_limit = 0;        // [orig g_score_limit @0x24D2134]    dword[5]
	uint32_t config_word_6 = 0;      // [orig dword_24D214C]               dword[6] (semantic UNWITNESSED)
	uint32_t start_delay = 0;        // [orig g_StartDelay @0x24D2160]     dword[7]
	uint32_t config_word_8 = 0;      // [orig dword_24D2164]               dword[8] (semantic UNWITNESSED)
	uint32_t config_word_9 = 0;      // [orig dword_24D2168]               dword[9] (semantic UNWITNESSED)
	uint8_t config_bytes[7] = {0, 0, 0, 0, 0, 0, 0}; // [orig byte_24D234C..byte_24D2360 + dword_24D2110 low byte]

	// CNapiServerConfig_BuildFlags inputs the game_settings don't already carry.
	bool squad_enforced = false;       // [orig g_squad_max_players @0x2550924 != 0] -> |0x2000
	std::string squad_required_tag;    // [orig g_squad_required_tag @0x2550928]      -> |0x4000
	bool permanent_death = false;      // [orig g_MpPermanentDeath @0x2550C9C]        -> |0x8000
	bool config_flag_2550A04 = false;  // [orig dword_2550A04 & 4]   (semantic UNWITNESSED) -> |0x4
	bool config_flag_2550CA4 = false;  // [orig dword_2550CA4]       (semantic UNWITNESSED) -> |0x10000
};

// [orig: g_napi_np_ctx.np_protocol @+0xE5C] NapiNPProtocol (§6.5) — the host state block reached
// from the singleton. Only the fields the in-match runtime needs now are modeled; offsets cited.
struct NapiNPProtocol {
	uint32_t server_flags = 0;          // [orig +0x500] P1 TLV (server config flag word)
	uint32_t build_flags = 0;           // [orig +0x504] P2 TLV (CNapiServerConfig_BuildFlags)
	uint32_t max_players = 1;           // [orig +0x524] MP TLV; clamped 1..251
	uint32_t gen_session_seed_flag = 1; // [orig +0x52C] 1 -> regenerate session_seed_id at start
	uint32_t session_seed_id = 0;       // [orig +0x530] (GetTickCount + rand) % 900000 + 100000
	uint32_t host_key = 0;              // [orig +0x534] HK TLV; NapiNP_GenerateSessionKey @0x61ea70
	uint32_t host_running = 0;          // [orig +0x538] 1 once StartServer succeeds; Hello rejects 0
	uint32_t host_start_tick = 0;       // [orig +0x53C] GetTickCount at StartServer (uptime base)
	uint32_t host_stop_tick = 0;        // [orig +0x540] GetTickCount at StopServer
	uint32_t host_run_duration_ms = 0;  // [orig +0x544] stop - start, frozen post-stop

	// [orig +0xECC: protocol[947]] Monotonic, non-zero connection-id source. NapiNPConnection_Create
	// @0x62acb0 stamps each new connection's dcb (connection_id, +0x18) from ++protocol[947] (wrapping
	// 0 -> 1). On a LAN listen host the host ASSIGNS this dcb, ships it in the 0x82 ServerAuth MI TLV
	// [orig: NapiNPConnection_SendSessionInit @0x620ef0], and stamps it into the joiner's 0x0C
	// ownerConnectionId so the client's Player_FindLocalPlayerEntity @0x4e0090 numeric self-match
	// (entity+0x78 == NapiNP_GetLocalConnectionId @0x4c6d40) succeeds.
	uint32_t next_connection_id = 1;

	// [orig +0x288] the session/server name ("HOST STARTED \"%s\"" log; lobby-visible).
	std::string session_name;

	// [orig +0xEBC] connection_list — the NapiListHead SendFiltered / timeouts walk. Modeled as a
	// vector of nodes (faithful structural translation of the intrusive list).
	std::vector<NapiNPConnection> connection_list;
};

// [orig: g_napi_np_ctx @0xB5CBC8] NapiNPServerCtx (§6.3) — the game-level singleton, the full
// in-match game-server state. CNapiNetwork-shaped header + game fields + np_protocol. Named
// fields with cited offsets; idiomatic C++ types (no byte-exact padding — see fidelity decision).
struct NapiNPServerCtx {
	NetworkType transport_mode = NetworkType::Lan; // [orig +0x50]
	SocketMode socket_state = SocketMode::Socketless; // [orig +0x54]
	uint32_t is_in_session = 0;        // [orig +0x58] gates the entire replication loop
	ConnectionMode connection_mode = ConnectionMode::None; // [orig +0x5C]
	uint32_t is_authority = 0;          // [orig +0x60] is_host bit of connection_mode
	uint32_t is_mp_session_peer = 0;    // [orig +0x64] is_client bit of connection_mode

	NapiGameSettings game_settings; // [orig +0xE68]
	NapiNPProtocol np_protocol;     // [orig +0xE5C] (pointer in the original; embedded here)

	// [§5.2a] The host game-rules block (S2C 0x08) + the advertised weapon-restriction set (S2C 0x66).
	// The original reads scattered g_* rule globals + a 255-entry restriction table (unused6 @0x24D5600);
	// modeled here as the host's own config. weapon_restrictions holds only the RESTRICTED (index,value)
	// entries (value 0 or 2); empty = no restrictions -> 0x66 emits a single count byte 0 (golden frame 160).
	ServerRules rules;
	std::vector<std::pair<uint8_t, uint8_t>> weapon_restrictions;

	// [orig +0x1198..0x11A0] the SendFiltered send descriptor (preserved names). Present but the
	// 2-peer MVP broadcasts the whole world (filter == 1); the per-connection cull is deferred.
	uint32_t send_mask = 0;          // [orig +0x1198]
	uint32_t send_target_player = 0; // [orig +0x119C]
	uint32_t send_target_state = 0;  // [orig +0x11A0]

	// --- reimpl-owned, NOT in the original singleton ---
	// The authoritative simulation. Non-owning. The in-match replication seam (the per-connection
	// C2S drain / S2C fan) is owned by Server_TickUpdate over connection_list — there is no separate
	// NetSystem (retired P8): the drain/emit primitives live in netsim/connection_fan.h.
	world::World *world = nullptr;
	// The loaded mission, read by the initial-state burst for the S2C 0x0B BMS-header body
	// (bms::encode_header_blob). Non-owning; null on the P2 unit-test path (0x0B skipped + logged).
	const bms::File *mission = nullptr;

	// Spawn gate (§5.2). spawn_success_gate <- dword_24C1928 (drop the loading screen; cleared later by
	// the per-frame 0x0A flags1 & 0x01, §5.2a step 4). The load-progress counter dword_A82370
	// (g_loading_progress, walks 3 -> 5 -> 6 as 0x0D/0x20/0x45 land) is CLIENT state, not host
	// bookkeeping — the host's spawn/load clock is the per-connection InitialStateBurst cursor
	// (conn.burst). It was write-only here and is removed (D-NET-132).
	uint32_t spawn_success_gate = 0;

	// The §5.1 reactive-reply config (server / mission / player identity + advertised spawn), read by
	// the gameplay-message dispatcher (server_message_dispatch.h, dispatch_session_replies). Seeded by
	// configure_session_runtime(). Replaces the retired GameServerRuntime (P8): the per-connection reply
	// state now lives on the node (NapiNPConnection.reply), the world-stream burst on
	// NapiNPConnection.burst (Server_SendInitialGameStateToPlayer).
	SessionReplyConfig session_config;

	// Deterministic server-key source (reimpl-only). The original mints the per-connection server
	// SCRK / SK / nwuid randomly at the 0x42 join (make_dev_scrk / make_random_session_u32 /
	// make_dev_nwuid). For golden 0x82 byte-parity these must be reproducible, so the default
	// leaves `forced` false (random, exactly as retail) and a golden test seed-injects the captured
	// values — mirroring the P1 SessionStartup "pass in, don't sample" determinism approach.
	struct ServerKeyMint {
		bool forced = false;       // false => mint randomly (retail behavior)
		std::string server_scrk;   // forced ServerAuth.scrk (61 chars)
		uint32_t server_sk = 0;    // forced ServerAuth.sk
		std::string nwuid;         // forced ServerAuth nwuid (60-char hex)
		std::string novaworld_name = "NWServer";
		std::string novaworld_web_url = "http://127.0.0.1:8080";
	};
	ServerKeyMint server_key_mint;

	// All members are complete + movable now that the unique_ptr<GameServerRuntime> is gone (P8), so the
	// compiler-default special members suffice.
	NapiNPServerCtx() = default;
	NapiNPServerCtx(NapiNPServerCtx &&) noexcept = default;
	NapiNPServerCtx &operator=(NapiNPServerCtx &&) noexcept = default;
};

} // namespace opennova::np
