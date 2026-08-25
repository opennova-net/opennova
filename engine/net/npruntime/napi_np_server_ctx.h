#pragma once

#include <array>
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
// is NO socket and NO Godot code anywhere under engine/net/npruntime (engine/CLAUDE.md).
namespace opennova::world {
class World;
}
// The loaded mission, read by the P3 initial-state burst for the S2C 0x0B BMS-header body
// (bms::encode_loaded_header_blob). Forward-declared (NOT included) so bms.h stays out of this light header.
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
	uint32_t host_run_duration_ms = 0;  // [orig +0x544] live elapsed ms; frozen at stop

	// [orig +0xECC: protocol[947]] Monotonic, non-zero connection-id source. NapiNPConnection_Create
	// @0x62acb0 stamps each new connection's dcb (connection_id, +0x18) from ++protocol[947] (wrapping
	// 0 -> 1). On a LAN listen host the host ASSIGNS this dcb, ships it in the 0x82 ServerAuth MI TLV
	// [orig: CNapiNPConnection_SendSessionInit @0x620ef0], and stamps it into the joiner's 0x0C
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
// The host-side rtxt "Server" strings (GameText section "Server"). Each is the
// sprintf format the retail handler fills; empty means the string is absent.
struct ServerTextTable {
	std::string medic_request_format; // STRSRV_MEDREQ: "%s" = the requester's name
};

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

	// [orig: g_scoreboard_broadcast_timer @0xC8D80C] One global mission
	// counter shared by the 0x16 scoreboard and 0x30/0x31 integrity broadcast.
	// Server_TickUpdate increments first; a value >0x136 fires and resets to 0,
	// so a fresh mission reaches its first boundary after 311 calls. Mission
	// start resets it through create_session; round init does not.
	uint32_t scoreboard_broadcast_timer = 0;
	// Round-end wire pass edge latch. Retail has no equivalent field because
	// Server_ProcessRoundEnd @0x5164f0 IS the one-shot: it runs the per-slot
	// wire block inline behind its own `if (!g_spawn_success_gate)` guard
	// [orig: @0x516502] and latches the gate at the end [orig: @0x5168e4]. Our
	// World::process_round_end owns that guard/latch on the sim side, so the
	// NET side observes `round_end.ended` as a LEVEL and needs its own edge
	// memory to fire the block exactly once. DIVERGENCE (placement only): the
	// wire block runs from the server tick on the ended-edge rather than inline
	// inside process_round_end, because the world layer holds no connection
	// list. The emitted bytes and their order are unchanged.
	bool round_end_wire_sent = false;
	// [orig: g_endround_linger_timer @0xc8d820] MP-only, set to 2790 (45 s at
	// the 62 Hz tick) BEFORE the per-slot loop and gated on is_in_session
	// [orig: @0x5166c4]. Drained by Server_TickUpdate (authority) / the client
	// frame; SP never drains it — the epilog owns the SP exit. Stored here, not
	// in World, because it is a session-lifetime net timer.
	uint32_t endround_linger_timer = 0;
	// [orig: dword_24C10C0] The process-global family toggle. Executable initial
	// storage is zero: false selects 0x31, true selects 0x30, then every boundary
	// XORs it even when no player is eligible. Neither session nor round init
	// resets it, so keep it as NapiNPServerCtx-lifetime state.
	bool integrity_entity_family_next = false;

	// The rtxt "Server" section strings the host formats into chat
	// (`GameText_GetString("Server", key)`), loaded by the embedder from its
	// gametext table through set_server_text(). An EMPTY string is the null
	// lookup: the consumer no-ops exactly as retail does when the text is
	// absent. [orig: Server_BroadcastMedicRequest @0x5153C9..0x5153D0]
	ServerTextTable server_text;

	// Host CNetQuality scalar sent as S2C 0x79. Retail derives this byte as
	// max(frame-rate pressure, mean ping, packet loss) over a five-sample window.
	// A local healthy LAN resolves to 1; the host adapter may replace it when
	// equivalent live telemetry is available.
	uint8_t host_network_quality = 1;
	// [orig: g_network_quality_broadcast_timer] One global explicit countdown,
	// reset to zero by Server_InitNewRoundState @0x51CA9E. Server_TickUpdate
	// decrements a positive value, emits when it reaches/is zero, then reloads
	// 0x136. This state must not be derived from World::logic_tick: round reset
	// intentionally makes the next server boundary due immediately.
	uint32_t network_quality_broadcast_countdown = 0;

	// The authoritative end-round transaction. The domain Match freezes the
	// result; these are only the once-only wire announcement and retail MP linger
	// clock. [orig: Server_ProcessRoundEnd @0x5164F0; 2790 store @0x5166C4]
	bool round_end_announced = false;
	uint32_t round_end_linger_ticks = 0;
	// The frozen end-of-round board stream (stru_C947D8): built once by the
	// round-end producer before the per-slot 0x61/0x1D push, then only READ by
	// the C2S 0x2B chunk service; empty until a round ends and cleared with the
	// other round-end fields at session creation.
	// [orig: Server_ProcessRoundEnd @0x5164F0 (the
	// Server_BuildEndOfRoundScoreboard(1, winTeam) call @0x516590);
	// NetPacket_WriteReplayStreamChunk @0x506F60]
	std::vector<uint8_t> round_end_board_stream;

	// Non-dedicated S2C 0x68 wraps its 50-row cursor against the live renderer
	// viewport height. Zero means no renderer seam was installed and suppresses
	// that request instead of fabricating a screen size. Simulation refreshes
	// this from its root viewport before every host pump (the parity matrix is
	// explicitly 1920x1080); focused tests set it directly.
	uint32_t loaded_model_viewport_height = 0;
	// --- reimpl-owned, NOT in the original singleton ---
	// The authoritative simulation. Non-owning. The in-match replication seam (the per-connection
	// C2S drain / S2C fan) is owned by Server_TickUpdate over connection_list — there is no separate
	// NetSystem (retired P8): the drain/emit primitives live in netsim/connection_fan.h.
	world::World *world = nullptr;

	// Last-sent S2C 0x6F body per zone handle — the golden shows 0x6F is NOT a steady
	// per-second stream (268 across a whole session): unchanged bodies are withheld and
	// pending/dead (deploy-screen) recipients get the full set at 1 Hz instead
	// (change-gated broadcast; D-NET-162 note). Keyed by the zone's packed handle.
	std::unordered_map<uint16_t, std::vector<uint8_t>> zone_6f_cache;
	// The loaded mission, read by the initial-state burst for the S2C 0x0B BMS-header body
	// (bms::encode_loaded_header_blob). Non-owning; null on the P2 unit-test path (0x0B skipped + logged).
	const bms::File *mission = nullptr;

	// The mission's raw terrain-tile (.til) file bytes: `[u32 'til0'][u32 count][u32 res0][u32 res1]`
	// then count × 12-B entries. Streamed to a joiner as the S2C 0x45 terrain-tile load (phase 5) so the
	// client's g_loading_progress climbs 5 -> 6 and its terrain finishes loading [orig: serialize_terrain_tiles
	// @0x6080F0 reads g_TerrainTileData; the 0x45 header magic/count/hdr2/hdr3 map 1:1 onto the .til
	// header]. Owning copy set by the host at mission load (Godot-free: the caller resolves the .til via
	// engine/formats/til). EMPTY => 0x45 is faithfully skipped (serialize_terrain_tiles returns 0 with no tile data).
	std::vector<uint8_t> terrain_til_data;

	// The current mission text table's raw cp1252 briefing strings. The Godot/resource
	// adapter resolves [info]/briefing3 and [info]/briefing2 (falling back to
	// [info]/briefing) before host bring-up. When loaded, phase 6 serializes these as
	// two consecutive C strings for S2C 0x7E. The explicit loaded bit distinguishes a
	// valid pair of empty strings from a missing/unparseable mission text resource.
	// [orig: NetPacket_WriteBriefingText @0x506620]
	bool mission_text_loaded = false;
	std::string mission_briefing3;
	std::string mission_briefing2;
	// MissionText [Locations]/LOCATION%03i labels for each BMS type-2044
	// marker, in marker spawn order. The S2C 0x0F writer copies these onto
	// the joining client's deploy map.
	std::vector<std::string> mission_location_names;

	// The 180-byte mission/session block streamed by S2C 0x64. Retail builds it
	// once at mission start, including two random 32-byte regions and a nonzero
	// session id, then serves that same block to every joiner/re-request.
	// [orig: CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0]
	std::array<uint8_t, 180> mission_metadata_blob{};

	// Retail's overloaded g_spawn_success_gate is deliberately not copied into
	// this host context. Per-connection InitialStateBurst owns load progress;
	// world::Match owns the round-over latch. The client retains the 0x1D header
	// that starts its end-round board transaction. [orig: §5.2/§5.68]

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

// Install the embedder's "Server" strings (the Godot shell reads its gametext
// table; nw_server reads a loose gametext.bin beside the mission).
inline void set_server_text(NapiNPServerCtx &ctx, ServerTextTable text) {
	ctx.server_text = std::move(text);
}

} // namespace opennova::np
