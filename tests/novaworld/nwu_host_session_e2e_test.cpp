// The engine NWU driver and host role (net/novaworld/nwu_lobby_session.h, nwu_host_role.h) over
// real loopback UDP against the real apps/novaworld_server NwUdpListener and an in-test gate
// responder: the gate probe, the handshake to Verified, a duplicate gate reply ignored, the host
// registration with no update after its result (D-NET-465), a roster change, the 1860-tick
// refresh carrying a changed column of the live session, the mission start's update with the
// next map's mission (D-NET-373), the NWU reconnect's re-host carrying the outage's refresh and
// team change in its request (D-NET-659) and republishing both lists at its result, and the stop.
// [orig: CNapiGateManager_ProbeThreadProc @0x6339e0; CNapiGameSession_ProcessPeriodicUpdate
//  @0x4d4400; CNapiGameSession_ConnectOrHost @0x4d4f10; Server_TickUpdate @0x51d7e0;
//  CNapiGameSession_HandleHostVerifyResponse @0x4d59d0; Game_StartMission @0x5248c6..0x524903]

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
#include <net/novaworld/lobby_session.h>
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
	// The value of `name` in the kept Host list of the row listed as `server` ("" when absent).
	// The fresh request carries BuildHostVarLists' first run alone, so a column only the
	// server-info update writes (MissionName) reaches the service with a ClientHostUpdate.
	auto host_var = [&](const std::string &server, const char *name) {
		for (const auto &h : hosted()) {
			if (!h.lobby.hosting || h.lobby.server_name != server)
				continue;
			const auto list = h.lobby.last_host_update.find("Host");
			if (list != h.lobby.last_host_update.end())
				return opennova::var_value(list->second, name);
		}
		return std::string();
	};

	// A fresh host's result publishes nothing: ReconnectCounter, which the request zeroed, gates
	// the result's republish, so no ClientHostUpdate follows it and the row keeps the request's
	// Host list until a mission start or the refresh (D-NET-465).
	// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4d5c52..0x4d5c58;
	//  CNapiGameSession_BuildHostVarLists @0x4d0b50 (the zero @0x4d0b7b)]
	for (int i = 0; i < 50; ++i)
		tick();
	expect(lobby.session() != nullptr && lobby.session()->host_reconnect_counter() == 0,
			"a fresh host's ReconnectCounter is 0");
	expect(row("Engine Driver Host") && host_var("Engine Driver Host", "MissionName").empty(),
			"a fresh host's result sends no ClientHostUpdate");

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

	// The live session the server-info update reads its columns from at each run, as an
	// embedder's bound match: from here a column changes with it, never with the registration.
	// [orig: Lobby_UpdateServerInfo @0x4fe8c0]
	std::string live_name = "Engine Driver Host";
	std::string live_mission = "ASH_G11A";
	role.set_column_source([&](opennova::HostRegistration &r) {
		r.server_name = live_name;
		r.mission_name = live_mission;
	});

	// The refresh: a changed column rides the next 1860-tick delta, not the edit.
	live_name = "Engine Driver Renamed";
	for (int i = 0; i < 20; ++i) tick();
	expect(!row("Engine Driver Renamed"), "a renamed column waits for the refresh");
	tick(30000); // past SESSION_HOST_INFO_REFRESH_TICKS (1860 ticks, ~29.8 s)
	expect(tick_until([&] { return row("Engine Driver Renamed"); }, 300),
	       "the 1860-tick refresh carries the changed column as its delta");
	expect(host_var("Engine Driver Renamed", "MissionName") == "ASH_G11A",
			"the first refresh carries every column (nothing went out before it)");

	// The mission start's update: both lists in full at once, outside the refresh timer; a map
	// change's next mission is the column the live session now reads (D-NET-373).
	// [orig: Game_StartMission @0x5248c6..0x524903; Lobby_UpdateServerInfo's MissionName
	//  @0x4fe989..0x4fe9d0]
	live_name = "Engine Driver Started";
	live_mission = "ASH_G12A";
	role.publish_mission_start();
	expect(tick_until([&] { return row("Engine Driver Started"); }, 100),
			"the mission start's update carries a changed column at once");
	expect(host_var("Engine Driver Started", "MissionName") == "ASH_G12A",
			"the mission start's update lists the next map's mission");

	// The NWU reconnect's re-host: the re-verify counts ReconnectCounter up and re-sends the
	// request with the session's kept lists, which the outage's refresh and a team change wrote,
	// and the result republishes both lists in full. A column changed after the request left
	// reaches the service with that republish alone: the role's own refresh is held off by driving
	// the lobby alone from here.
	// [orig: CNapiGameSession_HandleConnectVerifyResponse @0x4d5800 (the count @0x4d5968);
	//  CNapiGameSession_HandleHostVerifyResponse @0x4d59d0 (the gate @0x4d5c52..0x4d5c58, the
	//  republish @0x4d5c5a..0x4d5c9e)]
	auto tick_lobby = [&](uint32_t step_ms = 10) {
		now += step_ms;
		lobby.tick(now);
		if (opennova::ClientSession *session = lobby.session()) {
			for (const auto &notice : session->take_notices())
				role.handle_notice(notice);
		}
		std::this_thread::sleep_for(2ms);
	};
	// The session's own reap (240 s without a receive): its 0x46 burst drops the listener's
	// connection, which parks the lobby state and its RID. A lobby pass receives before it pumps,
	// so a service answer still in flight would re-arm the receive clock ahead of the reap check;
	// the jump's pass therefore pumps the session alone, with no receive between the jump and
	// the check (as nw_host_register_e2e's reconnect does), and the reap fires on that one pass.
	opennova::ClientSession *reaped = lobby.session();
	if (reaped != nullptr) {
		now += 240001;
		reaped->set_clock_ms(now);
		std::vector<std::vector<uint8_t>> burst;
		reaped->pump(burst);
		for (const auto &dg : burst)
			lobby.send(dg);
	}
	expect(reaped != nullptr && reaped->reconnecting(),
			"the reap tears the connection down for a reconnect");
	bool dropped = false;
	for (int i = 0; i < 100 && !dropped; ++i) {
		std::this_thread::sleep_for(20ms);
		dropped = !row("Engine Driver Started");
	}
	expect(dropped, "the listener drops the row on the burst");
	// The outage: the refresh still runs and its server-info update writes the Host list into the
	// session's (no state 6, so nothing is sent), and a team change writes its PlayerTeam into the
	// session's PlayerList at once; the re-host's request carries both (D-NET-659).
	// [orig: Lobby_UpdateServerInfo @0x4fe8c0 (its gates @0x4fe8d8 / @0x4fe8e5, no state test, the
	//  first SetOrCreate @0x4fe911); Server_TickUpdate's call @0x51d943; Server_ChangeEntityTeam
	//  @0x518e22; CNapiGameSession_SendHostRequest @0x4d3700 (the PlayerList @0x4d37ec..0x4d37f6)]
	live_name = "Engine Driver Outage";
	role.tick(now); // the refresh period passed since the role's last tick
	role.set_player_vars(1, "2", "0");
	// The re-probe past its first delay, the re-join, and the re-verify, which counts
	// ReconnectCounter up and sends the re-host's request (state 5). The request leaves in its
	// pass's pump, after that pass's receive, so the pass that first sees state 5 has not
	// received the result.
	bool requested = false;
	for (int i = 0; i < 3000 && !requested; ++i) {
		tick_lobby();
		const opennova::ClientSession *session = lobby.session();
		requested = session != nullptr && session->host_reconnect_counter() == 1 &&
				session->host_state() == opennova::ClientSession::HostState::Requested;
	}
	expect(requested, "the re-verify counts ReconnectCounter 1 and re-sends the request");
	// State 5: a mission start's update sends nothing until the result. The lobby runs on with
	// its notices held, so the role sees no result until they are handed over.
	// [orig: CPlayerManager_RebuildLists @0x4d4590 (the state-6 test @0x4d4593)]
	live_name = "Engine Driver Rehosted";
	role.publish_mission_start();
	std::vector<opennova::ClientSession::Notice> held;
	auto tick_holding = [&]() {
		now += 10;
		lobby.tick(now);
		if (opennova::ClientSession *session = lobby.session()) {
			for (auto &notice : session->take_notices())
				held.push_back(std::move(notice));
		}
		std::this_thread::sleep_for(2ms);
	};
	auto listed = [&]() {
		for (const auto &h : hosted()) {
			if (h.lobby.hosting)
				return true;
		}
		return false;
	};
	bool answered = false;
	for (int i = 0; i < 300 && !answered; ++i) {
		tick_holding();
		const opennova::ClientSession *session = lobby.session();
		answered = session != nullptr &&
				session->host_state() == opennova::ClientSession::HostState::Established &&
				listed();
	}
	expect(answered, "the service re-lists the row and answers the re-host");
	for (int i = 0; i < 20; ++i)
		tick_holding();
	expect(row("Engine Driver Outage"), "the re-host's request carries the outage's refresh");
	auto team_of = [&](int slot) {
		for (const auto &h : hosted()) {
			if (!h.lobby.hosting)
				continue;
			for (const auto &player : h.lobby.roster)
				if (player.slot == slot)
					return player.team;
		}
		return std::string();
	};
	expect(team_of(1) == "2", "the re-host's request carries the PlayerTeam changed in the outage");
	expect(!row("Engine Driver Rehosted"), "a mission start in state 5 sends no ClientHostUpdate");
	// The result reaches the role: ReconnectCounter 1, so it republishes.
	for (const auto &notice : held)
		role.handle_notice(notice);
	expect(role.is_hosting(), "the re-host's result keeps the hosting");
	bool republished = false;
	for (int i = 0; i < 300 && !republished; ++i) {
		tick_lobby();
		republished = row("Engine Driver Rehosted");
	}
	expect(republished, "the re-host's result republishes the column changed before the drop");
	expect(host_var("Engine Driver Rehosted", "MissionName") == "ASH_G12A",
			"the republish carries the whole Host list");

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
		gone = !row("Engine Driver Rehosted");
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
