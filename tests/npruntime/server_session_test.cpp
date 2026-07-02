// npruntime host bring-up — the §5.0 connection-mode -> {is_authority, is_mp_session_peer}
// table and the socketless transport mode. The IDA-faithful in-process listen-server bring-up
// state writes [orig: SinglePlayer_StartMission @0x561af0].

#include "npruntime/server_session.h"

#include "netsim/loopback_channel.h"

#include <cstdio>

namespace {

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

using opennova::np::ConnectionMode;
using opennova::np::NapiNPServerCtx;
using opennova::np::SocketMode;

// [orig: CGameSession_SetConnectionMode @0x4c49f0] decomposes the mode into the is_host /
// is_client booleans exactly per the §5.0 table.
bool check_connection_mode_table() {
	struct Row {
		ConnectionMode mode;
		uint32_t is_host;
		uint32_t is_client;
	};
	const Row rows[] = {
	    {ConnectionMode::None, 0, 0},
	    {ConnectionMode::HostOnly, 1, 0},
	    {ConnectionMode::ClientOnly, 0, 1},
	    {ConnectionMode::HostClient, 1, 1}, // single-player / co-op listen server
	};
	for (const Row &r : rows) {
		NapiNPServerCtx ctx;
		opennova::np::set_connection_mode(ctx, r.mode);
		if (!expect(ctx.connection_mode == r.mode, "connection_mode stored")) return false;
		if (!expect(ctx.is_authority == r.is_host, "is_authority = is_host bit")) return false;
		if (!expect(ctx.is_mp_session_peer == r.is_client, "is_mp_session_peer = is_client bit"))
			return false;
	}
	return true;
}

// SP host = mode 3 + socketless transport (§5.0 steps 1-2): is_authority && is_mp_session_peer,
// socket_state == Socketless (no UDP socket opened).
bool check_single_player_signature() {
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);
	if (!expect(ctx.is_authority == 1 && ctx.is_mp_session_peer == 1, "SP is host + client"))
		return false;
	if (!expect(ctx.socket_state == SocketMode::Socketless, "SP is socketless")) return false;
	// Defaults that must hold before StartServer (P1).
	if (!expect(ctx.np_protocol.host_running == 0, "host not running before StartServer"))
		return false;
	if (!expect(ctx.is_in_session == 0, "no session before CreateSession")) return false;
	if (!expect(ctx.np_protocol.connection_list.empty(), "no connections before bring-up"))
		return false;
	return true;
}

// The full SP bring-up [orig: SinglePlayer_StartMission @0x561af0]:
// SetConnectionMode(3) -> SetTransportMode(1) -> CreateSession -> StartServer, registering the
// host's own loopback client connection. After it: host_running == 1, in session, one connection.
bool check_create_session_brings_up_host() {
	opennova::netsim::LoopbackChannel local_client;
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);

	opennova::np::GameConfig settings;
	settings.server_name = "SINGLEPLAYERGAME"; // §5.0 default
	settings.max_players = 1;

	opennova::np::SessionStartup startup;
	startup.host_key = 0xABCD1234;     // seed-injected (NapiNP_GenerateSessionKey result)
	startup.host_start_tick = 100000;  // seed-injected (GetTickCount)
	startup.session_seed_id = 654321;  // seed-injected

	opennova::np::create_session(ctx, settings, startup, &local_client);

	if (!expect(ctx.is_in_session == 1, "in session after CreateSession")) return false;
	if (!expect(ctx.np_protocol.host_running == 1, "host_running == 1 after StartServer"))
		return false;
	if (!expect(ctx.np_protocol.host_key == 0xABCD1234, "host_key stamped")) return false;
	if (!expect(ctx.np_protocol.host_start_tick == 100000, "host_start_tick stamped"))
		return false;
	if (!expect(ctx.np_protocol.session_seed_id == 654321, "session_seed_id stamped"))
		return false;
	if (!expect(ctx.np_protocol.session_name == "SINGLEPLAYERGAME", "session_name = server_name"))
		return false;
	if (!expect(ctx.np_protocol.max_players == 1, "MP TLV mirrors settings")) return false;
	if (!expect(ctx.np_protocol.connection_list.size() == 1, "one (loopback) connection"))
		return false;
	const opennova::np::NapiNPConnection &c = ctx.np_protocol.connection_list[0];
	if (!expect(c.type == 2, "host's own client is a type-2 connection")) return false;
	if (!expect(c.link.mode == opennova::netsim::TransportMode::Loopback, "mode 1 loopback"))
		return false;
	if (!expect(c.link.transport == &local_client, "transport bound (non-owning)")) return false;
	return true;
}

// A dedicated host (mode 1) starts the server but registers no local client connection.
bool check_dedicated_host_has_no_local_client() {
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(ctx, SocketMode::Lan);
	opennova::np::GameConfig settings;
	settings.max_players = 16;
	opennova::np::create_session(ctx, settings, opennova::np::SessionStartup{}, nullptr);
	if (!expect(ctx.np_protocol.host_running == 1, "dedicated host running")) return false;
	if (!expect(ctx.np_protocol.connection_list.empty(), "no local client on a dedicated host"))
		return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_connection_mode_table() && ok;
	ok = check_single_player_signature() && ok;
	ok = check_create_session_brings_up_host() && ok;
	ok = check_dedicated_host_has_no_local_client() && ok;
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
