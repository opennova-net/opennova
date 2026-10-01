#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <functional>

#include <runtime/inmatch/game_config.h>       // inmatch::GameConfig — the ONE consolidated server-state config
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_designations.h> // the designation table (S2C 0x6B)
#include <runtime/replication/net_quality.h>   // the CNetQuality window (the host send half)
#include <runtime/world/entity.h>              // world::EntityHandle (the deployable spawner seam)

// Forward declarations — the runtime holds non-owning pointers to the authoritative world and
// the in-match replication seam. No World/codec headers are pulled into this header, and there
// is NO socket and NO Godot code anywhere under engine/runtime/inmatch (engine/CLAUDE.md).
namespace opennova::world {
class World;
}
// The loaded mission, read by the P3 initial-state burst for the S2C 0x0B BMS-header body
// (bms::encode_loaded_header_blob). Forward-declared (NOT included) so bms.h stays out of this light header.
namespace opennova::bms {
struct File;
}

namespace opennova::inmatch {

class ClientRuntime;

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
// AppId/JoinTicket gates branch on this == NovaWorld. The session setter stores it from its
// argument, the menu's connect type on a host or a join, 0 on SP and every reset; a joiner
// carries it on its join record (the Godot JoinTarget.network_type), where the squad talk
// row reads it [orig: CNapiNetwork_SetNetworkType @0x4c4a50: +0x50 = state @0x4c4a82 (and +0x58
// is_in_session = state in 1..3 @0x4c4a85); the join leg passes
// g_GameConfigState.networkConnectType_480 @0x558314..0x55831f, SinglePlayer_StartMission 0
// @0x561bd4; the squad gate @0x49ba13].
enum class NetworkType : uint32_t {
	NovaWorld = 1,
	Lan = 2,
};

// [orig +0x54] socket_state — the value [orig: CNapiNetwork_SetTransportMode @0x4c8750] writes.
// 1 = socketless (the host's own local client is in-process); 2/3/4 reach
// [orig: CNapiNetwork_OpenTransportSocket @0x4c6a40] and open a UDP socket (§5.0 step 2). SP = 1.
//
// NOTE (refines the plan's "reuse replication::TransportMode at +0x50"): the witness puts the
// loopback-vs-socket selector in socket_state (+0x54), and the network-type enum in
// transport_mode (+0x50) — two separate fields (§6.3). replication::TransportMode stays the
// per-connection mode on NapiNPConnection.link, which is the right home for it.
enum class SocketMode : uint32_t {
	Socketless = 1,       // single player / in-process loopback (no socket)
	Lan = 2,              // LAN socket
	HostClientSocket = 3, // host + client over a socket
	NovaWorldSocket = 4,  // NovaWorld-routed socket
};

// [orig: g_NapiNPCtx.np_protocol @+0xE5C] NapiNPProtocol (§6.5) — the host state block reached
// from the singleton. Only the fields the in-match runtime needs now are modeled; offsets cited.
struct NapiNPProtocol {
	bool reject_new_connections = false; // [orig +0x1F7, JFC6 @0x62be8f]
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

	// [orig +0xE44 / +0xE80] the cs_dir1 / cs_dir0 connection-template blocks — the reap window
	// (CS field 0) and outbound pool bound (CS field 11) every server-side node is created with
	// and the host's 0x82 advertises. CNapiNetwork_Init seeds 120000 / 1200 and a loose
	// `_NSTMOUT.TXT` overrides both (session_timeout_config.h); start_host_session installs it.
	SessionTimeoutConfig connection_template{};

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

// [orig: g_NapiNPCtx @0xB5CBC8] NapiNPServerCtx (§6.3) — the game-level singleton, the full
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
	// Live admission controls. The IP ban compares the connection's UDP source
	// address (conn+0x30, the datagram source stored @0x62bf28), never the
	// client-reported SIP; entries use PeerAddr::ip's LE octet packing (the
	// same as BanList_ParseIPEntry @0x4fd5c9).
	// [orig: CNapiNetwork_ValidateJoinRequest @0x4c61b0, the compare @0x4c6210]
	bool join_locked = false;
	std::vector<uint32_t> banned_join_addresses;
	GameConfig config;             // [orig g_NapiNPCtx.game_settings @+0xE68 + the scattered g_* rule globals]
	NapiNPProtocol np_protocol;    // [orig +0xE5C] (pointer in the original; embedded here)

	// [§5.2a] The advertised weapon-restriction set (S2C 0x66). The original reads a 255-entry
	// restriction table (unused6 @0x24D5600); weapon_restrictions holds only the RESTRICTED
	// (index,value) entries (value 0 or 2); empty = no restrictions -> 0x66 emits a single count byte 0
	// (golden frame 160).
	std::vector<std::pair<uint8_t, uint8_t>> weapon_restrictions;

	// [orig +0x1198] the SendFiltered send mask (preserved name). Present but the
	// 2-peer MVP broadcasts the whole world (filter == 1); the per-connection cull
	// (retail's +0x119C target player / +0x11A0 target state) is not modelled.
	uint32_t send_mask = 0;

	// [orig: g_ScoreboardBroadcastTimer @0xC8D80C] One global mission
	// counter shared by the 0x16 scoreboard and 0x30/0x31 integrity broadcast.
	// Server_TickUpdate increments first; a value >0x136 fires and resets to 0,
	// so a fresh mission reaches its first boundary after 311 calls. Mission
	// start resets it through create_session; round init does not.
	uint32_t scoreboard_broadcast_timer = 0;
	// NOT MODELLED: retail's g_EndRoundLingerTimer [orig: @0xc8d820] -- MP-only,
	// set to 2790 (45 s at the 62 Hz tick) BEFORE the per-slot round-end loop and
	// gated on is_in_session [orig: @0x5166c4], drained by Server_TickUpdate
	// (authority) / the client frame; SP never drains it (the epilog owns the SP
	// exit). The round-end wire pass keys on World::match.outcome() instead.
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
	// A local healthy LAN resolves to 1; the shell binding may replace it when
	// equivalent live telemetry is available.
	uint8_t host_network_quality = 1;
	// The host CNetQuality SEND window that derives the byte above, sampled
	// every 62 frames while in session [orig: Game_ProcessMainFrame @0x52658B,
	//  the dword_24D1DDC 62-frame countdown; CNetQuality_UpdateMetrics @0x4C52C0].
	replication::NetQualityWindow host_quality_window;
	uint32_t net_quality_sample_countdown = 0;
	// The authority's bucketed 0..4 level off that window (the receive window
	// never runs here, so the combined scalar is the send window's), stored
	// with each sample; the host role hands it to its own client's connection
	// indicators [orig: CNetQuality_UpdateMetrics's tail @0x4c585a..0x4c58b0 ->
	// CNetQuality_SetLevel(&g_NetQuality, level) @0x52659b].
	int32_t net_quality_level = 0;
	// The NovaWorld UDP (NWU) session the authority's NovaWorld registration
	// runs, as the shell reports it each tick: in use (dword_B5FD2C) and its
	// hosting/playing word (session+0x128, dword_B60108). The 62-frame block's
	// NovaWorld exit reads them beside transport_mode, and stores
	// g_MissionExitReason 12 here; so does the session's own punt handler.
	// [orig: Game_ProcessMainFrame @0x52655d..0x52657c]
	bool nwu_in_use = false;
	int32_t nwu_session_role = 0;
	int32_t mission_exit_reason = 0;
	// The server protocol's link-error callbacks since the host role last
	// drained them (kNetQualityLinkError* bits): a joiner's 0x44 resend list
	// that named a sequence, our 0x84 missing-sequence request that named one.
	uint32_t net_quality_link_errors = 0;
	// The main loop's FR counter (world::TickAccumulator::average_fps), handed
	// over by the session once per banked frame (HostRole::observe_frame_rate):
	// the send window's frame-pressure input and, copied at the head of every
	// Server_TickUpdate, the 0x0A server-status fps byte. 0 = retail's
	// mode-init value, until the first 2 s window closes.
	// [orig: g_StatsAvgFps; Server_TickUpdate @0x51D7E0..0x51D7E5 -> g_ServerFps]
	int32_t stats_avg_fps = 0;
	// The main loop's frames drawn in the last 62 logic updates and the frame
	// window's CPU share (Session::frame_statistics), handed over after every
	// frame (HostRole::observe_frame_statistics): the status page's bottom row.
	// [orig: dword_24C193C; g_StatsCpuPercent — Server_DrawStatusScreen
	//  @0x50afaf / @0x50b024]
	int32_t stats_frames_last_second = 0;
	int32_t stats_cpu_percent = 0;
	// The persistent slot cursor of the 1 Hz S2C 0x46 quality resend walk.
	// [orig: g_WeaponBroadcastSlotCursor, Server_TickUpdate @0x51DE79]
	int32_t quality_broadcast_slot_cursor = 0;
	// [orig: g_NetworkQualityBroadcastTimer] One global explicit countdown,
	// reset to zero by Server_InitNewRoundState @0x51CA9E. Server_TickUpdate
	// decrements a positive value, emits when it reaches/is zero, then reloads
	// 0x136. This state must not be derived from World::logic_tick: round reset
	// intentionally makes the next server boundary due immediately.
	uint32_t network_quality_broadcast_countdown = 0;

	// The team-change list a late joiner's C2S 0x29 walk reads back as S2C
	// 0x51: every entity Server_ChangeEntityTeam retargeted (a player at an
	// admin ChangeTeam, a capture zone at its flip), each once, cleared at the
	// new-round init. [orig: g_TeamChangeEntityList — CBufferList_AddOrFind
	//  @0x518EEC; the clear in Server_InitNewRoundState @0x51C911; the read
	//  NapiNPServerMsg_0x029 @0x514F7C]
	std::vector<world::EntityHandle> team_change_entities;

	// The designation table the radio calls fill and the per-player S2C 0x6B
	// batch reads, cleared at the new-round init
	// (inmatch/server_designations.h). [orig: g_ServerDesignations @0xC84810;
	//  the memset in Server_InitNewRoundState @0x51cb9b]
	ServerDesignationTable designations{};

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
	// NetSystem (retired P8): the drain/emit primitives live in runtime/replication/connection_fan.h.
	world::World *world = nullptr;
	// The host process's own client half (the listen host's loopback
	// ClientRuntime; null on a dedicated host): the client-side state a host
	// handler reads through the process globals retail shares, here the
	// active-zone overlay the radio key builder tests (the A&S context).
	// Non-owning; the role re-points it whenever it rebuilds that runtime.
	const ClientRuntime *host_client = nullptr;

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
	// client's g_LoadingProgress climbs 5 -> 6 and its terrain finishes loading [orig: Terrain_SerializeTiles
	// @0x6080F0 reads g_TerrainTileData; the 0x45 header magic/count/hdr2/hdr3 map 1:1 onto the .til
	// header]. Owning copy set by the host at mission load (Godot-free: the caller resolves the .til via
	// engine/formats/til). EMPTY => 0x45 is faithfully skipped (Terrain_SerializeTiles returns 0 with no tile data).
	std::vector<uint8_t> terrain_til_data;

	// The current mission text table's raw cp1252 briefing strings. The Godot/resource
	// binding resolves [info]/briefing3 and [info]/briefing2 (falling back to
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
	// The live transfer identities the 0x60 / 0x64 chunk headers carry: retail's
	// two process-global mission counters, each incremented once per mission
	// start, so a fresh process's first mission serves id 1 on both. A C2S
	// 0x33 / 0x37 carrying another token restarts its transfer at offset 0.
	// [orig: g_ReplayBlockMagic @0xC86FC4 (`++` in Game_StartMission @0x5247F3);
	//  dword_C86FC8 (`++` in CNapiGameSession_InitRandomSeedOrRequest @0x51E9C1)]
	uint32_t server_info_transfer_id = 0;
	uint32_t mission_metadata_transfer_id = 0;

	// Retail's overloaded g_SpawnSuccessGate is deliberately not copied into
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
		// Bare host:port — the retail client prefixes "http://" itself
		// [orig: CNapiGameSession_OnNovaWorldConnected @0x4D1627
		//  sprintf(url, "http://%s", domain)].
		std::string novaworld_web_url = "127.0.0.1:8080";
	};
	ServerKeyMint server_key_mint;

	// The per-registration NovaWorld AppId (CNapiNetwork_RandomizeTimeout's
	// value, net/napi/session.h make_session_app_id), installed by the
	// shell's NovaWorld host binding beside the GSID; 0 on a LAN host. The
	// authority's status page shows it on its NovaWorld server line.
	// [orig: ctx+0x1194 — CNapiNetwork_RandomizeTimeout @0x4c4da3, read back
	//  by sub_4C4DB0 @0x4c4db0 (Server_DrawStatusScreen @0x50a7fa)]
	uint32_t novaworld_app_id = 0;
	// Every connection the host's NapiNP layer brought up since the server
	// started, the host's own local connection included: the status page's
	// total logins.
	// [orig: ctx+0x11A8 — `add` in NapiNPServer_HandleNewConnection
	//  @0x4c8203, the new-connection callback CNapiNPConnection_OnStateChange
	//  @0x6261c6 runs when a connection enters state 1; zeroed by the host
	//  start callback CNapiServer_OnHostStarted (ex
	//  CNapiServer_OnPlayerDisconnected) @0x4c94f0 that
	//  NapiNPProtocol_StartServer @0x62b640 runs; read @0x50b0e4]
	uint32_t total_logins = 0;
	// The round tallies the status page's team block shows: every round end
	// of Team Deathmatch, Team KOTH or CTF counts one round and one win for
	// the winning side 1..4. Only the Reset Game action clears them
	// (Server_ForceRoundEndAndClearState, catalog row 92 `resetgames`, code
	// 106, which the port does not dispatch).
	// [orig: g_TotalRoundsPlayed @0xC8FF1C, g_RoundWinsTeam1..4
	//  @0xC8FF0C..0xC8FF18 — Server_ProcessRoundEnd @0x516883..0x5168d8; the
	//  reset @0x5175aa..0x5175be]
	std::array<int32_t, 4> round_wins{};
	int32_t rounds_played = 0;

	// The GSID the NovaWorld service returned in ServerHostResult HostCommands,
	// installed by the shell's NovaWorld host binding once registration
	// succeeds; empty on a pure LAN host. Advertised as the 0x81 SUS1.
	// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4D59D0 — "GSID"
	//  @0x4D5BD6 -> byte_24D5A12 @0x4D5BEE -> np_protocol->server_user_string1
	//  @0x4D5C0F]
	std::string novaworld_gsid;

	// The NovaWorld join-ticket arm (byte_B60100 & 0x40): set by the shell on a
	// NovaWorld-registered host whose ServerHostResult carried
	// HostRequiresJoinTicket. Armed, every validated joiner is announced to the
	// service as a ClientPlayerEnterRequest and held in game state 4 until the
	// ServerPlayerEnterResult (Server_ApplyPlayerEnterResult) or the 120 s
	// NWJTICKTMOUT reap; a pure LAN host leaves it clear.
	// [orig: CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 — the arm reads
	//  @0x4C8B88 / @0x4C8CA9, SendPlayEnterRequest @0x4C8BC1, SetGameState(4)
	//  @0x4C8BCA]
	bool novaworld_join_tickets_armed = false;
	// The host's own address string (CNapiNetwork_GetLocalAddress @0x4C4F60):
	// it prefixes the JOINTICKET key the request looks up in the joiner's CD
	// identity blob ("<localaddr>JOINTICKET"). Empty = no lookup, an empty
	// ticket rides the request (retail: GetLocalAddress failed). On a NovaWorld
	// transport retail's string is the constant "PUB", so the key is the
	// PUBJOINTICKET cookie; the shell installs kNovaWorldLocalAddress when it arms.
	// [orig: CNapiGameSession_SendPlayEnterRequest @0x4D0312..0x4D0362;
	//  CNapiNetwork_GetLocalAddress @0x4C4F60 copies g_LocalNetAddressStr @0x7CA298]
	static constexpr const char *kNovaWorldLocalAddress = "PUB";
	std::string host_local_address;
	// One ClientPlayerEnterRequest: the joiner's connection id, its UDP source
	// and the JOINTICKET its JOIN carried. The shell binds the hook to its
	// NovaWorld host session (ClientSession::build_player_enter_request); an
	// unbound hook drops the request, and the joiner then reaps on the ticket
	// deadline exactly as a service that never answered.
	// [orig: CNapiGameSession_SendPlayEnterRequest @0x4D02A0 — ConnectionId
	//  @0x4D0396, IpAddress @0x4D03D0, PortNumber @0x4D040B, JoinTicket @0x4D046B]
	struct PlayerEnterRequest {
		uint32_t connection_id = 0;
		PeerAddr peer{};
		std::string join_ticket;
	};
	std::function<void(const PlayerEnterRequest &)> on_player_enter_request;

	// A ServerCommand Cycle / EndMission / GameOver ends the round and then
	// overrides the linger Server_ProcessRoundEnd stored (2790) with 620 ticks;
	// nonzero here is consumed by the announcing pass, then cleared.
	// [orig: the ServerCommand handler CNapiGameSession_HandleServerCommand — Server_ProcessRoundEnd
	//  @0x4D31C0, g_EndRoundLingerTimer = 0x26C @0x4D31CA]
	uint32_t round_end_linger_override_ticks = 0;

	// The host EntityLimit table behind the vehicle-spawn availability reply
	// (C2S 0x42 -> S2C 0x70) and the C2S 0x40 spawn gate: one row per spawnable
	// items.def id. Retail's 73-dword rows carry the id (row[0]), the type cap
	// (row[1], -1 = unlimited), the per-team flag (row[2], -1 = no per-team
	// cap) and the per-team slot counts (row[3 + team], -1 = unlimited).
	// Empty = no limit table loaded: the reply carries only its terminator and
	// every spawn is refused, exactly as an empty g_EntityLimitTable.
	// [orig: g_EntityLimitTable @0xC7B480 / g_EntityLimitCount @0xC84680;
	//  NetPacket_SerializeWeaponOverlaySlots_0 @0x5105A0; sub_5104C0 @0x5104C0]
	struct VehicleSpawnLimitRow {
		uint16_t type_id = 0;
		int32_t type_cap = -1;                // row[1]
		int32_t per_team_flag = -1;           // row[2]
		std::array<int32_t, 8> team_slots{};  // row[3 + team]
	};
	// The table is bypassed while the host config's unlimited_vehicles is set
	// (GameConfig::unlimited_vehicles, stock 1): 0xFF/0xFF rows on the wire and
	// no limit check. [orig: g_RulesUnlimitedVehicles @0x5105F5..0x5105FF, @0x51C5E2..0x51C5E9]
	std::vector<VehicleSpawnLimitRow> vehicle_spawn_limits;
	// The world-side spawner Entity_SpawnDeployable @0x51C2B0 delegates to: the
	// embedder that owns the items.def traits sweep installs it; the C2S 0x40
	// handler refuses the spawn when unset. Returns the new pool-1 handle.
	std::function<world::EntityHandle(world::World &, uint16_t item_id, uint8_t team,
			const int32_t position[3])> deployable_spawner;

	// All members are complete + movable now that the unique_ptr<GameServerRuntime> is gone (P8), so the
	// compiler-default special members suffice.
	NapiNPServerCtx() = default;
	NapiNPServerCtx(NapiNPServerCtx &&) noexcept = default;
	NapiNPServerCtx &operator=(NapiNPServerCtx &&) noexcept = default;
};

// Install the embedder's "Server" strings (the Godot shell reads its gametext
// table).
inline void set_server_text(NapiNPServerCtx &ctx, ServerTextTable text) {
	ctx.server_text = std::move(text);
}

} // namespace opennova::inmatch
