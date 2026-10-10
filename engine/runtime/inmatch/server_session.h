#pragma once

#include <runtime/inmatch/napi_np_server_ctx.h>

// Host bring-up sequence — the in-process listen-server creation path (§5.0), mirroring the
// witnessed order [orig: SinglePlayer_StartMission @0x561af0]:
//
//   set_connection_mode(3)  [CGameSession_SetConnectionMode @0x4c49f0]
//   set_transport_mode(1)   [CNapiNetwork_SetTransportMode  @0x4c8750]   (socketless for SP)
//   create_session(settings)[CNapiGameSession_CreateSession @0x4c97c0]   (installs cbs, P1)
//     -> start_server()      [NapiNPProtocol_StartServer     @0x62b5e0]   (host_running=1, P1)
//
// All functions are pure state writes on the ctx — no sockets, no Godot. The owner (the Godot
// binding) opens any real socket; here we only record the witnessed mode.
namespace opennova::inmatch {

// Step 1 — map the connection mode to the three witnessed fields on the ctx (§5.0 table):
// connection_mode (+0x5C), is_authority = is_host bit (+0x60), is_mp_session_peer = is_client
// bit (+0x64). [orig: CGameSession_SetConnectionMode @0x4c49f0]
void set_connection_mode(NapiNPServerCtx &ctx, ConnectionMode mode);

// Step 2 — record the socket mode in socket_state (+0x54). 1 = socketless (SP, no socket); 2/3/4
// would open a UDP socket in the original (the owner does that here). [orig:
// CNapiNetwork_SetTransportMode @0x4c8750]
void set_transport_mode(NapiNPServerCtx &ctx, SocketMode socket);

// The volatile host-start values the original derives from GetTickCount / rand at StartServer
// (§6.5: host_key, host_start_tick, session_seed_id). Passed in rather than sampled so the
// headless runtime stays deterministic and a golden-pcap test can seed-inject the observed
// values (the plan's determinism approach). [orig: NapiNP_GenerateSessionKey @0x61ea70 ->
// host_key; GetTickCount -> host_start_tick; (GetTickCount + rand) % 900000 + 100000 ->
// session_seed_id]
struct SessionStartup {
	uint32_t host_key = 0;
	uint32_t host_start_tick = 0;
	uint32_t session_seed_id = 0;
};

// What the session create hands its caller. Retail's returns 0, or -1 / -2 on
// a null argument or a transport failure, which the references and the
// owner's socket rule out here; ProcessExit is the create that never returns:
// the cfg block's `mpreset` word is nonzero, and retail ends the process with
// code 0 before the session exists. The engine never exits: the context is
// left as it was, and the embedder ends its process with exit code 0 (the
// host boot reports it, HostBoot::session_create).
// [orig: CNapiGameSession_CreateSession @0x4C97E7..0x4C97F0 -> crt_exit(0)]
enum class CreateSessionResult : uint8_t {
	Created = 0,
	ProcessExit,
};

// Step 3 — the shared SP/MP session creator [orig: CNapiGameSession_CreateSession @0x4c97c0].
// A nonzero `config.multiplayer_reset` (the cfg block's `mpreset`, which the
// ServerCommand SetMPReset writes) creates nothing and returns ProcessExit.
// Otherwise it copies the whole GameConfig onto the ctx (identity + rules + the §5.1 reply
// slice), copies max_players + the lobby-visible session_name onto np_protocol, marks
// is_in_session, and (when is_authority) calls start_server. When the mode is HostClient and
// `local_client` is supplied, registers the host's own local client connection (type 2,
// TransportMode::Loopback) on the connection_list — the in-process listen-server's own client
// (§5.0: "creates a local client connection with NapiNPConnection_Create"). The transport is
// NON-OWNING (the binding/test owns the LoopbackChannel).
CreateSessionResult create_session(NapiNPServerCtx &ctx, const GameConfig &config,
                                   const SessionStartup &startup,
                                   replication::ISessionTransport *local_client = nullptr);

// The next mission inside the same session (net-re §5.70.6): the session
// config takes `config` (the next map's identity on the host screen's or the
// host file's rules), every mission-start reset of the context runs (the
// scoreboard counter, the round-end state, both transfer tokens, the
// mission-data block, the advertised flag words, the stored exit reason),
// and the connection list, the protocol state and the session identity stay.
// No session is created, so a set `mpreset` word does not end the process
// here: the PreMenu re-enters at its state 7, which pushes the Game Loop
// without CreateSession [orig: MultiPlayer_JoinSessionStateMachine
// @0x56A90B..0x56A925; PostMenu_RouteMissionExit's state 7 @0x568637].
// [orig: Game_StartMission @0x524360 -- Nbstat_StartupInit @0x526108, the
//  stream token `++g_ReplayBlockMagic` @0x5247F3,
//  CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0 (its `++dword_C86FC8`
//  @0x51E9C1), the end-round block @0x5249EA]
void continue_session(NapiNPServerCtx &ctx, const GameConfig &config);

// [orig: NapiNPProtocol_StartServer @0x62b5e0] — host-only. Stamps host_key / host_start_tick /
// session_seed_id (the latter only when gen_session_seed_flag is set), copies the MP TLV
// max_players, and sets host_running = 1 (HandleClientHello rejects while it is 0). No-op unless
// is_authority.
void start_server(NapiNPServerCtx &ctx, const SessionStartup &startup);

} // namespace opennova::inmatch
