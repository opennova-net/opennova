#pragma once

// Shared listen-host bring-up for the npruntime tests, so every test stands the host up through the
// real lifecycle (set_connection_mode -> set_transport_mode -> create_session). After this the host
// is_in_session + (when authority) host_running, so the gated handshake legs admit a join.

#include <net/npruntime/napi_np_protocol.h>
#include <net/npruntime/server_session.h>

#include <net/netsim/session_transport.h>

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
	// start_host_session installs the host's own per-side character selection on the
	// type-2 loopback right after create_session; the tests carry the stock fresh-profile
	// seed (side A 0x0200 / avatar 1 = the golden host record) unless a test overrides it.
	for (NapiNPConnection &connection : ctx.np_protocol.connection_list) {
		if (connection.type == 2) connection.char_vars = retail_fresh_profile_character_vars();
	}
}

} // namespace opennova::np::test
