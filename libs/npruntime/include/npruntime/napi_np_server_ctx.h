#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "npruntime/napi_np_connection.h"

// Forward declarations — the runtime holds non-owning pointers to the authoritative world and
// the in-match replication seam. No World/codec headers are pulled into this header, and there
// is NO socket and NO Godot code anywhere under libs/npruntime (libs/CLAUDE.md).
namespace opennova::world {
class World;
}
namespace opennova::netsim {
class NetSystem;
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

	// [orig +0x1198..0x11A0] the SendFiltered send descriptor (preserved names). Present but the
	// 2-peer MVP broadcasts the whole world (filter == 1); the per-connection cull is deferred.
	uint32_t send_mask = 0;          // [orig +0x1198]
	uint32_t send_target_player = 0; // [orig +0x119C]
	uint32_t send_target_state = 0;  // [orig +0x11A0]

	// --- reimpl-owned, NOT in the original singleton ---
	// The authoritative simulation and the in-match replication seam. Non-owning.
	world::World *world = nullptr;
	netsim::NetSystem *net = nullptr;

	// Load / spawn gates (§5.1 / §5.2). spawn_success_gate <- dword_24C1928 (drop loading screen);
	// loading_progress <- dword_A82370 (walks 3 -> 5 -> 6 as 0x0D/0x20/0x45 land).
	uint32_t spawn_success_gate = 0;
	uint32_t loading_progress = 0;
};

} // namespace opennova::np
