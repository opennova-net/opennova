#pragma once

// Shared P0->P1->P2 listen-host bring-up for the npruntime tests, so every test stands the host up
// through the REAL lifecycle (set_connection_mode -> set_transport_mode -> create_session ->
// configure_session_runtime) rather than a bare configure_session_runtime(). After this the host is
// is_in_session + (when authority) host_running, so the gated handshake legs admit a join.

#include <npruntime/napi_np_protocol.h>
#include <npruntime/server_session.h>

#include <netsim/session_transport.h>

namespace opennova::np::test {

// Bring a listen/dedicated host fully up. `host_key` is seeded onto the host (and advertised in
// ServerHello.hk / checked against ClientAuth.hk); `local_client`, when non-null with HostClient,
// registers the host's own type-2 loopback connection.
inline void bring_up_host(NapiNPServerCtx &ctx, ConnectionMode mode, SocketMode socket,
                          uint32_t host_key = 0,
                          netsim::ISessionTransport *local_client = nullptr,
                          const GameConfig &config = GameConfig{}) {
	set_connection_mode(ctx, mode);
	set_transport_mode(ctx, socket);
	SessionStartup startup;
	startup.host_key = host_key;
	create_session(ctx, config, startup, local_client); // P1: is_in_session=1, host_running=1
	configure_session_runtime(ctx);                     // P2: drops type-1 joiners, keeps the loopback
}

} // namespace opennova::np::test
