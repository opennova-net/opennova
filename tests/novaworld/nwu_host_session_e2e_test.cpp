// The engine NWU driver and host role (net/novaworld/nwu_lobby_session.h, nwu_host_role.h) over
// real loopback UDP against the real apps/novaworld_server NwUdpListener and an in-test gate
// responder: the gate probe, the handshake to Verified, a duplicate gate reply ignored, the host
// registration, a roster change, the 1860-tick refresh carrying a changed column, and the stop.
// [orig: CNapiGateManager_ProbeThreadProc @0x6339e0; CNapiGameSession_ProcessPeriodicUpdate
//  @0x4d4400; CNapiGameSession_ConnectOrHost @0x4d4f10; Server_TickUpdate @0x51d7e0]

#include "nw_udp_listener.h"
#include "server_config.h"

#include "net_datagram_socket.h"
#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/gate_response.h>
#include <net/novaworld/nwu_host_role.h>
#include <net/novaworld/nwu_lobby_session.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
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

// A gate that answers every probe twice (a duplicate reply), naming the listener as UDPNOVAWORLD.
void serve_gate(opennova::net::Socket &gate, uint16_t nw_port, std::atomic<bool> &stop,
                std::atomic<int> &replies) {
	const std::string body = "VAR \"POSTIPADDRESS\" \"127.0.0.1\"\r\n"
	                         "VAR \"POSTIPPORT\" \"7597\"\r\n"
	                         "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n"
	                         "VAR \"UDPNOVAWORLD\" \"127.0.0.1:" + std::to_string(nw_port) + "\"\r\n"
	                         "VAR \"STARTUPURL\" \"http://127.0.0.1:8080\"\r\n";
	std::vector<uint8_t> inner(body.begin(), body.end());
	opennova::nwu_decrypt(inner.data(), inner.size(), opennova::GATE_NWU_KEY);
	std::vector<uint8_t> reply(inner.size() + 16);
	std::size_t reply_size = 0;
	opennova::napi_envelope_encode(inner.data(), inner.size(), reply.data(), reply.size(), &reply_size);
	reply.resize(reply_size);
	while (!stop) {
		uint8_t rx[1024];
		opennova::net::Endpoint from{};
		if (opennova::net::udp_recv_from(gate, rx, sizeof(rx), from, 50) <= 0) continue;
		for (int i = 0; i < 2; ++i) {
			opennova::net::udp_send_to(gate, from, reply.data(), reply.size());
			++replies;
		}
	}
}

} // namespace

int main() {
	using namespace std::chrono_literals;
	if (opennova::net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}

	opennova::ConnectionManager manager;
	opennova::novaworld_server::NwUdpListener listener(manager);
	manager.on_lost([&listener](const opennova::Connection &connection, opennova::DropReason reason) {
		listener.erase_lobby_state(connection.addr, opennova::drop_reason_name(reason));
	});
	opennova::novaworld_server::ServerConfig config;
	config.nw_udp_port = 0;
	if (!listener.start(config)) {
		std::fprintf(stderr, "FAIL: listener.start\n");
		return 1;
	}
	std::this_thread::sleep_for(50ms);

	uint16_t gate_port = 0;
	opennova::net::ScopedSocket gate_server(opennova::net::udp_bind(0, &gate_port));
	std::atomic<bool> stop_gate{false};
	std::atomic<int> gate_replies{0};
	std::thread gate_thread([&] {
		serve_gate(gate_server.get(), listener.bound_port(), stop_gate, gate_replies);
	});

	opennova::net::ScopedSocket gate_client(opennova::net::udp_bind(0));
	opennova::net::ScopedSocket session_client(opennova::net::udp_bind(0));
	opennova::net::NetDatagramSocket gate_socket(gate_client.get());
	opennova::net::NetDatagramSocket session_socket(session_client.get());

	std::vector<std::string> fatals;
	opennova::NwuLobbySession::Hooks lobby_hooks;
	lobby_hooks.cookie_vars = []() {
		return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
	};
	lobby_hooks.on_fatal = [&fatals](const std::string &message) { fatals.push_back(message); };
	opennova::NwuLobbySession lobby(lobby_hooks, opennova::NwuLobbySession::Environment{});
	bool hosting = false;
	std::vector<std::string> host_failures;
	opennova::NwuHostRole::Hooks role_hooks;
	role_hooks.on_hosting = [&hosting]() { hosting = true; };
	role_hooks.on_failed = [&host_failures](const std::string &tag) { host_failures.push_back(tag); };
	opennova::NwuHostRole role(lobby, role_hooks);

	// The owner's tick, on a synthetic clock that steps 10 ms per call.
	uint32_t now = 0;
	auto tick = [&](uint32_t step_ms = 10) {
		now += step_ms;
		lobby.tick(now);
		if (opennova::ClientSession *session = lobby.session()) {
			for (const auto &notice : session->take_notices()) role.handle_notice(notice);
		}
		role.tick(now);
		std::this_thread::sleep_for(2ms);
	};
	auto tick_until = [&](const std::function<bool()> &done, int rounds) {
		for (int i = 0; i < rounds && !done(); ++i) tick();
		return done();
	};
	auto hosted = [&]() { return listener.snapshot_hosted(); };

	lobby.open(gate_socket, session_socket);
	lobby.probe("127.0.0.1", gate_port);
	const bool verified = tick_until([&] { return lobby.session_verified(); }, 1500);
	expect(verified, "the driver probes the gate and verifies against the real listener");
	expect(fatals.empty(), "no fatal on the way up");
	const opennova::ClientSession *first_session = lobby.session();
	tick_until([&] { return gate_replies >= 2; }, 200);
	for (int i = 0; i < 20; ++i) tick();
	expect(lobby.session() == first_session && lobby.session_verified(),
	       "the gate's duplicate reply is never read: the live session stands");

	opennova::HostRegistration reg;
	reg.server_name = "Engine Driver Host";
	reg.mission_name = "ASH_G11A";
	reg.lobby_name = lobby.gate_response().lobby_name;
	expect(role.request(reg), "the host request is queued");
	expect(tick_until([&] { return hosting; }, 1500), "the role reaches Hosting");
	auto row = [&](const std::string &name) {
		for (const auto &h : hosted())
			if (h.lobby.server_name == name && h.lobby.hosting) return true;
		return false;
	};
	expect(tick_until([&] { return row("Engine Driver Host"); }, 300), "the listener lists the row");

	opennova::HostPlayerSlot joiner;
	joiner.slot = 1;
	joiner.player_name = "Joiner";
	joiner.ip_and_port = "10.0.0.9:32768";
	joiner.team = "1";
	joiner.type = "0";
	role.set_player_slot(joiner);
	auto players = [&]() {
		const auto snap = hosted();
		return snap.empty() ? -1 : snap.front().lobby.player_count;
	};
	const int before = players();
	expect(tick_until([&] { return players() == before + 1; }, 300),
	       "ClientHostPlayerAdded reaches the service while hosting");

	// The refresh: a changed column rides the next 1860-tick delta, not the edit.
	role.set_server_name("Engine Driver Renamed");
	for (int i = 0; i < 20; ++i) tick();
	expect(!row("Engine Driver Renamed"), "a renamed column waits for the refresh");
	tick(30000); // past SESSION_HOST_INFO_REFRESH_TICKS (1860 ticks, ~29.8 s)
	expect(tick_until([&] { return row("Engine Driver Renamed"); }, 300),
	       "the 1860-tick refresh carries the changed column as its delta");

	// Stop: ClientStopHosting on one pump, then the goodbye burst; the row goes.
	role.stop();
	lobby.flush();
	if (opennova::ClientSession *session = lobby.session()) {
		const std::vector<uint8_t> goodbye = session->build_goodbye();
		for (std::size_t i = 0; i < session->disconnect_burst_count(); ++i) lobby.send(goodbye);
	}
	bool gone = false;
	for (int i = 0; i < 100 && !gone; ++i) {
		std::this_thread::sleep_for(20ms);
		gone = !row("Engine Driver Renamed");
	}
	expect(gone, "the stop removes the row");
	lobby.close();

	stop_gate = true;
	gate_thread.join();
	listener.stop();
	opennova::net::shutdown();
	if (g_failures == 0) {
		std::printf("OK: the engine NWU driver hosts through the real listener\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
