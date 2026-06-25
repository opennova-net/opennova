// End-to-end host registration through the REAL gate listener.
//
// The in-process tests cover the two halves separately: client_session_loopback
// drives ClientSession against a mock server, and host_register_list drives a
// LobbySession directly. Neither exercises the actual apps/novaworld_server
// NwUdpListener — the UDP socket, the NWU/CRC envelope decode, the HELLO->AUTH->
// SESSION opcode dispatch, and the PN routing into the lobby path. This closes
// that gap: it stands up a real NwUdpListener on a loopback UDP port and drives
// a real ClientSession (the same one NovaWorldHost runs) through the full
// handshake to Verified, then sends a ClientHostRequest and asserts the host
// shows up in the listener's hosted snapshot — the source /api/hosts + the GSB
// browser read from. This is the F1 "browsable host" claim, verified against the
// process that serves it. [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]

#include "nw_udp_listener.h"
#include "server_config.h"

#include <napi/session.h>            // make_client_host_request, ClientVar
#include <novaworld/client_session.h>
#include <novaworld/connection/manager.h>

#include "net_sockets.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

// A loopback port distinct from the retail 64206 so a real local server (or a
// concurrent test) doesn't collide. Loopback-only; a bind failure fails loudly.
constexpr uint16_t kTestNwPort = 46206;

opennova::net::Endpoint loopback_ep(uint16_t port) {
	opennova::net::Endpoint ep;
	ep.ip = {127, 0, 0, 1};
	ep.port = port;
	return ep;
}

} // namespace

int main() {
	using namespace std::chrono_literals;

	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}

	// Stand up the real listener (no DB: snapshot_hosted reads the in-memory
	// lobby state, so host registration is observable without SQLite).
	opennova::ConnectionManager manager;
	opennova::server::NwUdpListener listener(manager);
	opennova::server::ServerConfig config;
	config.nw_udp_port = kTestNwPort;
	if (!listener.start(config)) {
		std::fprintf(stderr, "FAIL: listener.start (port %u in use?)\n", kTestNwPort);
		return 1;
	}
	// Let the worker thread re-bind its socket before the first datagram.
	std::this_thread::sleep_for(50ms);

	// The client: a real ClientSession over a real UDP socket — exactly what
	// NovaWorldHost::begin_session sets up (minus the gate-probe leg, which only
	// resolves this endpoint; we know it directly).
	opennova::ClientSession::Config cfg;
	cfg.verify_cookie_vars = {{"NWUID", ""}};  // echoed from the SessionInit
	opennova::ClientSession session(cfg);

	uint16_t client_port = 0;
	auto client = opennova::net::udp_bind(0, &client_port);
	expect(client.is_valid(), "client UDP socket bound");
	const auto server_ep = loopback_ep(kTestNwPort);

	auto send = [&](const std::vector<uint8_t> &dg) {
		if (!dg.empty())
			opennova::net::udp_send_to(client, server_ep, dg.data(), dg.size());
	};

	// Drive the handshake to Verified: send ClientHello, then pump replies back
	// through the session (each handle_datagram emits the next leg) until the
	// ServerVerifyResult lands or we time out.
	send(session.start());
	uint8_t rx[4096];
	bool verified = false;
	for (int i = 0; i < 40 && !verified; ++i) {
		opennova::net::Endpoint from;
		const int n = opennova::net::udp_recv_from(client, rx, sizeof rx, from, 200);
		if (n > 0) {
			std::vector<std::vector<uint8_t>> out;
			if (!session.handle_datagram(rx, static_cast<size_t>(n), out)) {
				std::fprintf(stderr, "FAIL: session error: %s\n",
				             session.last_error().c_str());
				++g_failures;
				break;
			}
			for (const auto &dg : out) send(dg);
		}
		verified = session.is_verified();
	}
	expect(verified, "ClientSession reached Verified against the real listener");

	if (verified) {
		using opennova::ClientVar;
		// Register a host the same way NovaWorldHost does on the wire.
		auto host_req = opennova::make_client_host_request(
		    /*CurrentlyHosting*/ 1,
		    /*Cookie*/    {{0, "NWUID", session.server_nwuid()}},
		    /*HostSetup*/ {{0, "AppId", "28"}, {0, "LobbyName", "jop_2_consumer"},
		                   {0, "MaxPlayers", "24"}},
		    /*Host*/      {{0, "ServerName", "E2E Listen Host"}, {0, "ServerIP", "127.0.0.1"},
		                   {0, "ServerPortNumber", "32768"}, {0, "Players", "1"},
		                   {0, "Region", "us"}},
		    /*PlayerList*/{{0, "Slot0", "Host"}});
		auto dg = session.build_lobby_message(host_req);
		expect(!dg.empty(), "host-request datagram built (session Verified)");
		send(dg);

		// The listener processes the request on its worker thread; poll the
		// hosted snapshot until the row appears (or time out).
		std::vector<opennova::server::NwUdpListener::HostedSnapshot> hosted;
		for (int i = 0; i < 100 && hosted.empty(); ++i) {
			std::this_thread::sleep_for(20ms);
			hosted = listener.snapshot_hosted();
		}
		expect(hosted.size() == 1, "exactly one host registered in the snapshot");
		if (hosted.size() == 1) {
			const auto &lobby = hosted[0].lobby;
			expect(lobby.hosting, "snapshot host is hosting");
			expect(lobby.server_name == "E2E Listen Host", "snapshot ServerName matches");
			expect(lobby.host_port == 32768, "snapshot host_port matches ServerPortNumber");
			expect(lobby.max_players == 24, "snapshot MaxPlayers matches");
			expect(lobby.game == "jop_2_consumer", "snapshot LobbyName matches");
		}

		// A ClientHostUpdate refreshes the live occupancy on the same session.
		auto upd = opennova::make_client_host_update(
		    /*Host*/      {{0, "Players", "2"}},
		    /*PlayerList*/{{0, "Slot0", "Host"}, {0, "Slot1", "Joiner"}});
		send(session.build_lobby_message(upd));
		int players = 0;
		for (int i = 0; i < 100; ++i) {
			std::this_thread::sleep_for(20ms);
			auto snap = listener.snapshot_hosted();
			if (!snap.empty()) players = snap[0].lobby.player_count;
			if (players == 2) break;
		}
		expect(players == 2, "ClientHostUpdate refreshed the player count to 2");
	}

	send(session.build_goodbye());
	opennova::net::close_socket(client);
	listener.stop();
	opennova::net::shutdown();

	if (g_failures == 0) {
		std::printf("OK: host registered + updated through the real NwUdpListener\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
