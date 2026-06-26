#include "npruntime/server_session.h"

namespace opennova::np {

// [orig: CGameSession_SetConnectionMode @0x4c49f0] — stores the mode and decomposes it into the
// is_host / is_client booleans (§5.0):
//   mode 0 -> 0/0   mode 1 -> 1/0   mode 2 -> 0/1   mode 3 -> 1/1
void set_connection_mode(NapiNPServerCtx &ctx, ConnectionMode mode) {
	ctx.connection_mode = mode;
	const uint32_t m = static_cast<uint32_t>(mode);
	ctx.is_authority = (m & 0x1u) ? 1u : 0u;        // is_host bit
	ctx.is_mp_session_peer = (m & 0x2u) ? 1u : 0u;  // is_client bit
}

// [orig: CNapiNetwork_SetTransportMode @0x4c8750] — records the socket mode. The original opens a
// socket only for modes 2/3/4 (CNapiNetwork_OpenTransportSocket @0x4c6a40); mode 1 is in-process.
// The owner performs any real socket open — here we just hold the witnessed mode.
void set_transport_mode(NapiNPServerCtx &ctx, SocketMode socket) {
	ctx.socket_state = socket;
}

// [orig: NapiNPProtocol_StartServer @0x62b5e0] — see header. Host-only.
void start_server(NapiNPServerCtx &ctx, const SessionStartup &startup) {
	if (!ctx.is_authority) return;
	NapiNPProtocol &p = ctx.np_protocol;
	p.host_key = startup.host_key;          // HK TLV (validated against the client HK on join)
	p.host_start_tick = startup.host_start_tick; // uptime base
	if (p.gen_session_seed_flag) p.session_seed_id = startup.session_seed_id;
	p.max_players = ctx.game_settings.max_players; // MP TLV mirror
	p.host_stop_tick = 0;                   // cleared until StopServer
	p.host_run_duration_ms = 0;
	p.host_running = 1;                      // StartServer succeeded
}

// [orig: CNapiGameSession_CreateSession @0x4c97c0] — see header.
void create_session(NapiNPServerCtx &ctx, const NapiGameSettings &settings,
                    const SessionStartup &startup, netsim::ISessionTransport *local_client) {
	ctx.game_settings = settings;
	ctx.np_protocol.session_name = settings.server_name; // "HOST STARTED \"%s\"" log name
	ctx.np_protocol.max_players = settings.max_players;
	ctx.is_in_session = 1; // gates the whole replication loop (+0x58)

	if (ctx.is_authority) start_server(ctx, startup);

	// mode 3 (host + client): the listen server connects its own local client in-process.
	if (ctx.connection_mode == ConnectionMode::HostClient && local_client != nullptr) {
		NapiNPConnection self;
		self.connection_id = 0; // the host's own client is connection 0
		self.type = 2;          // client-side connection [orig: NapiNPConnection_Create type 2]
		self.phase = ConnectionPhase::New; // advanced by the host's own-player spawn flow (P3)
		self.link.transport = local_client;
		self.link.mode = netsim::TransportMode::Loopback; // socketless mode 1
		ctx.np_protocol.connection_list.push_back(self);
	}
}

} // namespace opennova::np
