// The NovaWorld service pushes ServerCommand and ServerStopHosting to opennova-serve over the
// server's own NovaWorld session: the real apps/novaworld_server NwUdpListener and an in-test
// gate, and an in-process serve::Server listed on them over loopback (the
// opennova_serve_novaworld setup: a loose game directory, two deathmatch maps in rotation, no
// retail data). The listener resolves the server's RID and queues each statement for its receive
// thread, which frames it on the server's session as a reliable record; the server's lobby
// session reads it [orig: CNapiGameSession_DispatchServerStatement @0x4d18a0] and the match runs
// it [orig: CNapiGameSession_HandleServerCommand @0x4d22f0]. A pushed SetServerName renames the
// live match and saves game.cfg, a pushed Cycle ends the round and the map changes, the next map's
// republish carries the new name to the service's row, a pushed ServerStopHosting ends the
// listing (the server's lister stops on the service's word, and the row leaves the browser at
// once), and a RID no connection holds is refused.
#include "server.h"
#include "serve_test_support.h"

#include "lister.h"
#include "listing_source.h"
#include "nw_udp_listener.h"
#include "server_config.h"

#include "net_sockets.h"

#include <formats/gamecfg/game_cfg.h>
#include <net/napi/session.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/lobby_session.h>

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
using novaworld_server::HostPushResult;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

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

	const fs::path dir = serve_test::fresh_dir("push");
	const fs::path work = serve_test::fresh_dir("push_cwd");
	serve_test::ScopedCwd cwd(work);
	CHECK(serve_test::write_bytes(dir / "SERVETST.BMS", serve_test::deathmatch_mission()));
	CHECK(serve_test::write_bytes(dir / "SERVETS2.BMS", serve_test::deathmatch_mission("Serve Test Map Two")));
	CHECK(serve_test::write_bytes(dir / "gametext.bin",
			serve_test::gametext({
					{"NovaWorld",
							{{"STRNOVA07", "Jungle"}, {"STRNOVA08", "Desert"}, {"STRNOVA09", "Snow"},
									{"STRNOVA10", "None"}, {"STRNOVA11", "Yes"}, {"STRNOVA12", "No"}}},
					{"GateTypeAbbrev", {{"DM", "DM"}}},
			})));
	{
		std::ofstream host(dir / "test.host", std::ios::binary);
		host << "GameName \"Serve Push\"\r\n"
		     << "ServerMessage \"pushed to by the test\"\r\n"
		     << "MaxPlayers 8\r\n"
		     // The last line names the starting map; the round end takes SERVETS2.BMS.
		     << "Mission servetst.bms\r\n"
		     << "Mission servets2.bms\r\n"
		     << "Mission servetst.bms\r\n";
	}

	std::string error;
	serve::ServeOptions options;
	CHECK(serve::parse_serve_options(
			{"--resource-dir", dir.string(), "/HOST", (dir / "test.host").string(), "--loose-root",
					"--lan-port", "0", "--master-host", "127.0.0.1", "--master-gate-port",
					std::to_string(gate_port)},
			options, error) == 0);
	auto holder = std::make_unique<serve::Server>(options);
	const bool started = holder->start(error);
	if (!started) std::printf("start: %s\n", error.c_str());
	CHECK(started);
	if (!started) return 1;
	serve::Server &server = *holder;
	CHECK(server.novaworld() && server.lister() != nullptr && server.lister()->hosting());

	constexpr double kFrame = 1.0 / 62.5;
	// Frames until `done` within a wall-clock bound, a short sleep every few frames so the
	// service's receive thread and the loopback socket keep up with the match (a push lands on the
	// session only after its datagram does); false on the bound or once the server stops.
	auto frame_until = [&](auto done, std::chrono::seconds bound) {
		const auto deadline = std::chrono::steady_clock::now() + bound;
		for (unsigned f = 1; !done() && std::chrono::steady_clock::now() < deadline; ++f) {
			if (!server.frame(kFrame)) break;
			if (f % 4 == 0) std::this_thread::sleep_for(1ms);
		}
		return done();
	};
	auto row = [&](const std::string &name) -> std::optional<LobbyState> {
		for (const auto &h : listener.snapshot_hosted())
			if (h.lobby.hosting && h.lobby.server_name == name) return h.lobby;
		return std::nullopt;
	};
	std::optional<LobbyState> listed;
	CHECK(frame_until(
			[&] {
				listed = row("Serve Push");
				return listed.has_value();
			},
			10s));
	if (!listed) return 1;
	const uint32_t rid = listed->rid;
	CHECK(rid != 0);

	// --- a RID no connection holds is refused, and nothing reaches the server.
	CHECK(listener.push_server_command(rid + 1000, "Cycle") == HostPushResult::UnknownRid);
	CHECK(listener.push_server_command(0, "Cycle") == HostPushResult::UnknownRid);

	// --- SetServerName: the live match takes the name and game.cfg is saved, as retail's
	// handler does [orig: CNapiGameSession_HandleServerCommand — the block copy @0x4D2CFC,
	// Game_SaveConfig @0x4D2DDF].
	const inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
	const std::string rename =
			server_command_text(ServerCommandVerb::SetServerName, ServerCommandTarget::None, {"Pushed Name"});
	CHECK(listener.push_server_command(rid, rename) == HostPushResult::Queued);
	CHECK(frame_until([&] { return ctx.config.server_name == "Pushed Name"; }, 10s));
	CHECK(server.game_cfg().game_name == "Pushed Name");
	const gamecfg::LoadResult renamed = gamecfg::load_file(gamecfg::kFileName, {});
	CHECK(renamed.file_read && renamed.cfg.game_name == "Pushed Name");

	// --- Cycle: the round ends with the 620-tick linger and the rotation's next map boots; its
	// mission start republishes the Host list, so the service's row carries the next map and the
	// pushed name [orig: Game_StartMission @0x5248f5 -> Lobby_UpdateServerInfo].
	CHECK(server.missions_played() == 1);
	const std::string cycle = server_command_text(ServerCommandVerb::Cycle, ServerCommandTarget::None, {});
	CHECK(cycle == "Cycle");
	CHECK(listener.push_server_command(rid, cycle) == HostPushResult::Queued);
	CHECK(frame_until([&] { return server.missions_played() >= 2; }, 30s));
	CHECK(server.missions_played() == 2);
	CHECK(server.role().state.host_owner.ctx.config.mission_name == "Serve Test Map Two");
	CHECK(server.role().state.host_owner.ctx.config.server_name == "Pushed Name");
	std::optional<LobbyState> after;
	CHECK(frame_until(
			[&] {
				after = row("Pushed Name");
				return after && after->mission_name == "Serve Test Map Two";
			},
			10s));
	CHECK(after.has_value() && after->rid == rid);
	const std::string cycled_to = server.role().state.host_owner.ctx.config.mission_name;

	// --- ServerStopHosting: the server's session drops to verified and its lister stops on the
	// service's word; the row leaves the browser as the statement goes out.
	CHECK(listener.push_stop_hosting(rid, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT) == HostPushResult::Queued);
	int lister_exit = -1;
	CHECK(frame_until(
			[&] {
				if (server.lister() != nullptr && server.lister()->finished())
					lister_exit = server.lister()->exit_code();
				return lister_exit != -1;
			},
			10s));
	CHECK(lister_exit == nw_lister::kExitStoppedByService);
	bool still_listed = false;
	for (const auto &h : listener.snapshot_hosted()) still_listed = still_listed || h.lobby.rid == rid;
	CHECK(!still_listed);
	CHECK(listener.push_server_command(rid, "Cycle") != HostPushResult::Queued);
	std::printf("opennova_serve_server_command_push: rid %u, renamed, cycled to '%s', stopped\n", rid,
			cycled_to.c_str());

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
		std::printf("opennova_serve_server_command_push: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_server_command_push: ok\n");
	return 0;
}
