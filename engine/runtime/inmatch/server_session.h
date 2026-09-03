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
// All functions are pure state writes on the ctx — no sockets, no Godot. The owner (apps/nw_server
// or the Godot binding) opens any real socket; here we only record the witnessed mode.
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

// Step 3 — the shared SP/MP session creator [orig: CNapiGameSession_CreateSession @0x4c97c0].
// Copies the whole GameConfig onto the ctx (identity + rules + the §5.1 reply slice), copies
// max_players + the lobby-visible session_name onto np_protocol, marks is_in_session, and (when
// is_authority) calls start_server. When the mode is HostClient and `local_client` is supplied,
// registers the host's own local client connection (type 2, TransportMode::Loopback) on the
// connection_list — the in-process listen-server's own client (§5.0: "creates a local client
// connection with NapiNPConnection_Create"). The transport is NON-OWNING (the binding/test owns
// the LoopbackChannel).
void create_session(NapiNPServerCtx &ctx, const GameConfig &config,
                    const SessionStartup &startup,
                    replication::ISessionTransport *local_client = nullptr);

// [orig: NapiNPProtocol_StartServer @0x62b5e0] — host-only. Stamps host_key / host_start_tick /
// session_seed_id (the latter only when gen_session_seed_flag is set), copies the MP TLV
// max_players, and sets host_running = 1 (HandleClientHello rejects while it is 0). No-op unless
// is_authority.
void start_server(NapiNPServerCtx &ctx, const SessionStartup &startup);

} // namespace opennova::inmatch
