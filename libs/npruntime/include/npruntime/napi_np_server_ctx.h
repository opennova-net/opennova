#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "npruntime/game_config.h"       // np::GameConfig — the ONE consolidated server-state config
#include "npruntime/napi_np_connection.h"

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

	// Reimpl-owned roster version: bumped when any player spawns or disconnects; each
	// connection's roster_seen_gen drives its 0x16 player-list (re)push so every client's
	// HUD count tracks the LIVE roster (D-NET-155) [orig: the retail host re-broadcasts
	// the list via Server_BuildAndBroadcastScoreboard @0x50de00 — its exact trigger set
	// is a tracked follow-up; the golden single-joiner 31→39 grow is preserved].
	uint32_t roster_generation = 1;
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

	// The ONE consolidated server-state config (ADR 0013 / §6.9): §6.4 identity + the §6.9 rule globals
	// (S2C 0x08) + the §5.1 reactive-reply mission/player/spawn. Merged from the former game_settings +
	// rules + session_config. Seeded by create_session (see server_session.h).
	GameConfig config;             // [orig g_napi_np_ctx.game_settings @+0xE68 + the scattered g_* rule globals]
	NapiNPProtocol np_protocol;    // [orig +0xE5C] (pointer in the original; embedded here)

	// [§5.2a] The advertised weapon-restriction set (S2C 0x66). The original reads a 255-entry
	// restriction table (unused6 @0x24D5600); weapon_restrictions holds only the RESTRICTED
	// (index,value) entries (value 0 or 2); empty = no restrictions -> 0x66 emits a single count byte 0
	// (golden frame 160).
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

	// Dead host-side players awaiting their respawn release [orig: the death queue +
	// respawn timers Server_ProcessPlayerDeath / GameEvent_PlayerDeath set (slot +360/+364,
	// the 620-tick recent-spawn rule); a joiner's respawn instead rides its own deploy
	// request]. Drained by Server_TickUpdate; §5.60.
	struct PendingRespawn {
		world::EntityHandle victim;
		uint32_t due_tick = 0;
	};
	std::vector<PendingRespawn> respawn_queue;
	// Last-sent S2C 0x6F body per zone handle — the golden shows 0x6F is NOT a steady
	// per-second stream (268 across a whole session): unchanged bodies are withheld and
	// pending/dead (deploy-screen) recipients get the full set at 1 Hz instead
	// (change-gated broadcast; D-NET-162 note). Keyed by the zone's packed handle.
	std::unordered_map<uint16_t, std::vector<uint8_t>> zone_6f_cache;
	// The loaded mission, read by the initial-state burst for the S2C 0x0B BMS-header body
	// (bms::encode_header_blob). Non-owning; null on the P2 unit-test path (0x0B skipped + logged).
	const bms::File *mission = nullptr;

	// The mission's raw terrain-tile (.til) file bytes: `[u32 'til0'][u32 count][u32 res0][u32 res1]`
	// then count × 12-B entries. Streamed to a joiner as the S2C 0x45 terrain-tile load (phase 5) so the
	// client's g_loading_progress climbs 5 -> 6 and its terrain finishes loading [orig: serialize_terrain_tiles
	// @0x6080F0 reads g_TerrainTileData; the 0x45 header magic/count/hdr2/hdr3 map 1:1 onto the .til
	// header]. Owning copy set by the host at mission load (Godot-free: the caller resolves the .til via
	// libs/til). EMPTY => 0x45 is faithfully skipped (serialize_terrain_tiles returns 0 with no tile data).
	std::vector<uint8_t> terrain_til_data;

	// Spawn gate (§5.2). spawn_success_gate <- dword_24C1928 (drop the loading screen; cleared later by
	// the per-frame 0x0A flags1 & 0x01, §5.2a step 4). The load-progress counter dword_A82370
	// (g_loading_progress, walks 3 -> 5 -> 6 as 0x0D/0x20/0x45 land) is CLIENT state, not host
	// bookkeeping — the host's spawn/load clock is the per-connection InitialStateBurst cursor
	// (conn.burst). It was write-only here and is removed (D-NET-132).
	uint32_t spawn_success_gate = 0;

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
