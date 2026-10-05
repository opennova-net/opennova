// opennova-serve listed on NovaWorld (ADR 0051 d6), with no retail data and
// nothing off loopback: the real apps/novaworld_server NwUdpListener and an
// in-test gate, and the loose game directory of the LAN test plus a
// gametext.bin. No game.cfg, and --master-host names the gate on this machine:
// the default network type (1) lists the server there. The server binds its one
// socket from the NovaWorld range, hosts on the gate's NovaWorld session BEFORE
// the mission boots, then boots and serves. The service lists it with the witnessed Serve Only columns (Dedicated = STRNOVA11, MaxPlayers
// without the dedicated slot, Players without the host, Port "-1", no slot 0)
// at the endpoint it observed, which is the game socket (D-NET-346); a LAN
// probe on that socket is answered; a joiner dialing the advertised endpoint
// plays and reaches the PlayerList as its UDP source; the stop deregisters.
#include "server.h"
#include "serve_test_support.h"

#include "lister.h"
#include "nw_udp_listener.h"
#include "server_config.h"

#include "net_sockets.h"

#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/gate_probe.h>
#include <net/novaworld/lobby_session.h>
#include <net/npwire/lan_discovery.h>
#include <runtime/inmatch/client_runtime.h>


#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

std::string var(const VarList &list, const char *name) {
	return var_has(list, name) ? var_value(list, name) : std::string("<absent>");
}

} // namespace

int main() {
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);

	ConnectionManager manager;
	novaworld_server::NwUdpListener listener(manager);
	manager.on_lost([&listener](const Connection &connection, DropReason reason) {
		listener.erase_lobby_state(connection.addr, drop_reason_name(reason));
	});
	novaworld_server::ServerConfig service;
	service.nw_udp_port = 0;
	if (!listener.start(service)) return (std::printf("FAIL listener.start\n"), 1);
	uint16_t gate_port = 0;
	net::ScopedSocket gate_server(net::udp_bind(0, &gate_port));
	std::atomic<bool> stop_gate{false};
	std::thread gate_thread(
			[&] { serve_test::serve_gate(gate_server.get(), listener.bound_port(), true, stop_gate); });

	const fs::path dir = serve_test::fresh_dir("novaworld");
	const fs::path work = serve_test::fresh_dir("novaworld_cwd");
	serve_test::ScopedCwd cwd(work);
	CHECK(serve_test::write_bytes(dir / "SERVETST.BMS", serve_test::deathmatch_mission()));
	// The Host list's tokens come from the mounted gametext, as the game's do.
	CHECK(serve_test::write_bytes(dir / "gametext.bin",
			serve_test::gametext({
					{"NovaWorld",
							{{"STRNOVA07", "Jungle"}, {"STRNOVA08", "Desert"}, {"STRNOVA09", "Snow"},
									{"STRNOVA10", "None"}, {"STRNOVA11", "Yes"}, {"STRNOVA12", "No"}}},
					{"GateTypeAbbrev", {{"DM", "DM"}}},
			})));
	{
		std::ofstream host(dir / "test.host", std::ios::binary);
		host << "GameName \"Serve NW\"\r\n"
		     << "ServerMessage \"listed by the test\"\r\n"
		     << "MaxPlayers 8\r\n"
		     << "Mission servetst.bms\r\n";
	}

	// Started with the listing: the socket binds, the session hosts, then the
	// mission boots. A port the probe released can be taken in between: retry.
	std::unique_ptr<serve::Server> holder;
	std::string error;
	bool started = false;
	for (int attempt = 0; attempt < 5 && !started; ++attempt) {
		serve::ServeOptions options;
		std::string parse_error;
		CHECK(serve::parse_serve_options(
				{"--resource-dir", dir.string(), "/HOST", (dir / "test.host").string(), "--loose-root",
						"--lan-port", std::to_string(serve_test::free_udp_port()), "--master-host", "127.0.0.1",
						"--master-gate-port",
						std::to_string(gate_port)},
				options, parse_error) == 0);
		holder = std::make_unique<serve::Server>(options);
		started = holder->start(error);
		if (!started && error.find("bind scan") == std::string::npos) break;
	}
	if (!started) std::printf("start: %s\n", error.c_str());
	CHECK(started);
	if (!started) return 1;
	serve::Server &server = *holder;
	CHECK(server.novaworld() && server.lister() != nullptr && server.lister()->hosting());
	// The match runs on the NovaWorld network type with the session's GSID.
	const inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
	CHECK(ctx.transport_mode == inmatch::NetworkType::NovaWorld);
	CHECK(!ctx.novaworld_gsid.empty());
	CHECK(ctx.nwu_session_role == ClientSession::kSessionRoleHosting);

	constexpr double kFrame = 1.0 / 62.5;
	auto row = [&]() -> std::optional<LobbyState> {
		for (const auto &h : listener.snapshot_hosted())
			if (h.lobby.hosting && h.lobby.server_name == "Serve NW") return h.lobby;
		return std::nullopt;
	};
	// The first full Host list lands right after the hosting.
	std::optional<LobbyState> lobby;
	for (int f = 0; f < 300 && !(lobby && !lobby->last_host_update["Host"].empty()); ++f) {
		CHECK(server.frame(kFrame));
		std::this_thread::sleep_for(2ms);
		lobby = row();
	}
	CHECK(lobby.has_value());
	if (!lobby) return 1;

	// The service's only endpoint is the source of the host's NWU datagrams:
	// the game socket.
	CHECK(lobby->host_ip == "127.0.0.1");
	CHECK(lobby->host_port == server.bound_port());
	CHECK(lobby->max_players == 8);
	CHECK(lobby->roster.empty());
	const VarList &host_list = lobby->last_host_update["Host"];
	CHECK(var(host_list, "Dedicated") == "Yes");
	CHECK(var(host_list, "MaxPlayers") == "8");
	CHECK(var(host_list, "Players") == "0");
	CHECK(var(host_list, "Port") == "-1");
	CHECK(var(host_list, "GameType") == "DM");
	CHECK(var(host_list, "Msg") == "listed by the test");
	CHECK(var(host_list, "Region") == "Jungle");
	CHECK(var(host_list, "TimeLeft") != "<absent>");
	CHECK(var(host_list, "CountryName") != "<absent>"); // METEXT 1
	CHECK(lobby->last_host_update["PlayerList"].empty());

	const net::Endpoint game_ep{{127, 0, 0, 1}, static_cast<uint16_t>(lobby->host_port)};
	// --- a LAN browser's probe on the same socket: the game's protocol answers.
	{
		uint16_t browse_port = 0;
		net::ScopedSocket browse(net::udp_bind(0, &browse_port));
		const uint32_t ci = 0x4321;
		const std::vector<uint8_t> probe = build_lan_discovery_probe(ci);
		net::udp_send_to(browse.get(), game_ep, probe.data(), probe.size());
		LanDiscoveryServer found;
		bool answered = false;
		for (int f = 0; f < 60 && !answered; ++f) {
			CHECK(server.frame(kFrame));
			uint8_t rx[2048];
			net::Endpoint from{};
			const int n = net::udp_recv_from(browse.get(), rx, sizeof(rx), from, 10);
			if (n > 0) answered = parse_lan_discovery_reply(rx, static_cast<size_t>(n), ci, found);
		}
		CHECK(answered);
		CHECK(found.server_name == "Serve NW");
	}

	// --- a joiner dials the advertised endpoint and plays.
	uint16_t joiner_port = 0;
	net::ScopedSocket joiner_sock(net::udp_bind(0, &joiner_port));
	inmatch::ClientRuntime client("ListedJoiner");
	auto ship = [&](const std::vector<uint8_t> &d) {
		if (!d.empty()) net::udp_send_to(joiner_sock.get(), game_ep, d.data(), d.size());
	};
	auto drain = [&]() {
		uint8_t rx[4096];
		net::Endpoint from{};
		for (;;) {
			const int n = net::udp_recv_from(joiner_sock.get(), rx, sizeof(rx), from, 5);
			if (n <= 0) break;
			client.receive(rx, static_cast<size_t>(n));
		}
	};
	uint32_t tick = 1;
	ship(client.start());
	bool in_match = false;
	for (int f = 0; f < 900 && !in_match; ++f) {
		if (!server.frame(kFrame)) {
			std::printf("the session ended: %s\n", server.end_message().c_str());
			break;
		}
		drain();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
		in_match = client.in_match();
	}
	CHECK(in_match);

	// The joiner reaches the service's PlayerList at once (ClientHostPlayerAdded),
	// as its observed UDP source, and never at slot 0, which a Serve Only host
	// keeps for itself without publishing it.
	std::optional<LobbyState> with_joiner;
	for (int f = 0; f < 300; ++f) {
		if (!server.frame(kFrame)) break;
		drain();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
		with_joiner = row();
		if (with_joiner && with_joiner->roster.size() == 1) break;
	}
	CHECK(with_joiner && with_joiner->roster.size() == 1);
	if (with_joiner && with_joiner->roster.size() == 1) {
		const HostRosterSlot &slot = with_joiner->roster[0];
		CHECK(slot.slot != 0);
		CHECK(slot.player_name == "ListedJoiner");
		CHECK(slot.ip_and_port == "127.0.0.1:" + std::to_string(joiner_port));
		CHECK(with_joiner->player_count == 1);
		// The endpoint stays the game socket.
		CHECK(with_joiner->host_port == server.bound_port());
	}
	std::printf("opennova_serve_novaworld: listed on UDP %u, joiner in after %u frames\n",
			server.bound_port(), tick);

	// --- the stop deregisters: the row leaves the service at once.
	server.stop();
	bool gone = false;
	for (int i = 0; i < 100 && !gone; ++i) {
		std::this_thread::sleep_for(20ms);
		gone = !row().has_value();
	}
	CHECK(gone);

	holder.reset();
	stop_gate = true;
	gate_thread.join();
	listener.stop();
	net::shutdown();
	cwd.restore();
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::remove_all(work, ec);
	if (failures != 0) {
		std::printf("opennova_serve_novaworld: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_novaworld: ok\n");
	return 0;
}
