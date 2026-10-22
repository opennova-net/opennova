// The NovaWorld service pushes ServerCommand and ServerStopHosting to opennova-serve over the
// server's own NovaWorld session: the real apps/novaworld_lobby NwUdpListener and an in-test
// gate, and an in-process serve::Server listed on them over loopback (the
// opennova_serve_novaworld setup: a loose game directory, two deathmatch maps in rotation, no
// retail data), over a database of the test's own. Each push is a command this thread inserts on
// its own connection (lobby_repository.h) once the server's RID resolves to its hosting session;
// the listener's receive thread takes it and frames it on the server's session as a reliable
// record; the server's lobby
// session reads it [orig: CNapiGameSession_DispatchServerStatement @0x4d18a0] and the match runs
// it [orig: CNapiGameSession_HandleServerCommand @0x4d22f0]. A pushed SetServerName renames the
// live match and saves game.cfg, a pushed Cycle ends the round and the map changes, the next map's
// republish carries the new name to the service's row, a pushed ServerStopHosting ends the
// listing (the row leaves the browser at once; the server's session drops to verified and stays
// up until the match's NovaWorld exit, whose reset sends the goodbye alone, no ClientStopHosting,
// its line in _connectlog.txt ahead of the socket's teardown line, and finishes the lister on the
// service's word), and a RID no connection holds is refused.
#include "server.h"
#include "serve_test_support.h"

#include "lister.h"
#include "listing_source.h"
#include "nw_udp_listener.h"
#include "program/server_config.h"

#include "net_sockets.h"

#include <formats/gamecfg/game_cfg.h>
#include <net/napi/session.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/lobby_repository.h>
#include <net/novaworld/lobby_session.h>
#include <runtime/inmatch/mission_exit.h>

#include "common/test_database.h"

#include <base/io/log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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
using lobbydb::PushResult;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

// The service's lobby session logs each hosting it ends with the statement that ended it
// (LobbySession::end_hosting): the ClientStopHosting receipts the server's stop must not cause.
std::atomic<int> client_stop_hosting_logs{ 0 };
void log_sink(io::LogLevel level, const char *message) {
	if (std::strstr(message, "reason=ClientStopHosting") != nullptr)
		++client_stop_hosting_logs;
	if (level != io::LogLevel::kDebug)
		std::printf("%s\n", message);
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	io::set_log_sink(&log_sink);
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);

	test_db::TestDatabase database;
	db::ConnectionPool pool(database.options());
	// The service's logouts (declared ahead of the listener, whose teardown may report one): the
	// server's goodbye burst ends its session with one.
	std::atomic<int> logouts{ 0 };
	ConnectionManager manager;
	novaworld_server::NwUdpListener listener(manager);
	listener.set_db_pool(&pool);
	// The pushes, as a route inserts them: on a connection of this thread's own.
	auto push_command = [&pool](uint32_t rid, const std::string &cmd) {
		return lobbydb::queue_server_command(*pool.acquire(), rid, cmd);
	};
	manager.on_lost([&listener, &logouts](const Connection &connection, DropReason reason) {
		if (reason == DropReason::Logout)
			++logouts;
		listener.erase_lobby_state(connection.addr, drop_reason_name(reason));
	});
	novaworld_server::ServerConfig service;
	service.nw_udp_port = 0;
	// The endpoint its instance announces (the in-test gate hands out the port
	// it bound instead).
	service.lobby_endpoint = "198.51.100.7:64206";
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
					std::to_string(gate_port), "/CONNECTLOG"},
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
	CHECK(push_command(rid + 1000, "Cycle") == PushResult::UnknownRid);
	CHECK(push_command(0, "Cycle") == PushResult::UnknownRid);

	// --- SetServerName: the live match takes the name and game.cfg is saved, as retail's
	// handler does [orig: CNapiGameSession_HandleServerCommand — the block copy @0x4D2CFC,
	// Game_SaveConfig @0x4D2DDF].
	const inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
	const std::string rename =
			server_command_text(ServerCommandVerb::SetServerName, ServerCommandTarget::None, {"Pushed Name"});
	CHECK(push_command(rid, rename) == PushResult::Queued);
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
	CHECK(push_command(rid, cycle) == PushResult::Queued);
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

	// --- ServerStopHosting: the server's session drops to verified and sends nothing; the row
	// leaves the browser as the statement goes out. The session stays up, and the service keeps
	// it, while the match runs: the match's next 62-frame block reads the dropped word and ends
	// it with the NovaWorld exit (12), whose router resets the session with the goodbye burst
	// alone (no ClientStopHosting); the lister finishes there, on the service's word.
	// [orig: CNapiGameSession_HandleServerMessage @0x4D1C50 - SetState(4) @0x4D1D29, the word 1
	//  @0x4D1D4F; Game_ProcessMainFrame @0x5263F0 - the check @0x52654F..0x52657C;
	//  PostMenu_RouteMissionExit @0x568460 - `cmp eax, 0Ch` @0x56854F / jz @0x568552 ->
	//  loc_5686CB, SetNetworkType(0) (the call @0x5686E1); CNapiNetwork_SetNetworkType @0x4C4A50 -
	//  CNapiGameSession_ResetToDisconnected @0x4D0890 (the call @0x4C4A7D)]
	CHECK(logouts.load() == 0);
	CHECK(lobbydb::queue_stop_hosting(*pool.acquire(), rid, SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT) ==
			PushResult::Queued);
	int lister_exit = -1;
	bool finished_while_running = false;
	bool goodbye_while_running = false;
	CHECK(frame_until(
			[&] {
				if (server.lister() != nullptr) {
					finished_while_running = finished_while_running || server.lister()->finished();
					goodbye_while_running = goodbye_while_running || logouts.load() != 0;
					return false;
				}
				lister_exit = server.last_lister_exit();
				return true;
			},
			10s));
	CHECK(!finished_while_running);
	CHECK(!goodbye_while_running);
	CHECK(server.role().state.host_owner.ctx.mission_exit_reason == inmatch::kMissionExitNovaWorld);
	CHECK(lister_exit == serve::kExitStoppedByService);
	for (int i = 0; i < 100 && logouts.load() == 0; ++i)
		std::this_thread::sleep_for(20ms);
	CHECK(logouts.load() == 1);
	// The service never read a ClientStopHosting: the router's reset sends the goodbye alone.
	CHECK(client_stop_hosting_logs.load() == 0);
	// The router's order in _connectlog.txt: the session's goodbye line (the reset's
	// OnDisconnect), then the socket's teardown line (D-NET-NEW-L9-23)
	// [orig: PostMenu_RouteMissionExit @0x568460 - SetNetworkType(0) called @0x5686E1 ahead of
	//  SetTransportMode(0) called @0x5686F9; CNapiGameSession_OnDisconnect @0x4CFAA0 - the line
	//  @0x4CFB5C; CNapiNetwork_TearDownSocket @0x4C4C90 - the line @0x4C4CBE..0x4C4CE7]
	const std::string connect_log = serve_test::read_text(work / "_connectlog.txt");
	const std::size_t goodbye_line = connect_log.rfind(" : NWU DISCONNECTING FROM NOVAWORLD ");
	const std::size_t teardown_line = connect_log.rfind(" : NET TEARING DOWN SOCKET\r\n");
	CHECK(goodbye_line != std::string::npos && teardown_line != std::string::npos &&
			goodbye_line < teardown_line);
	bool still_listed = false;
	for (const auto &h : listener.snapshot_hosted()) still_listed = still_listed || h.lobby.rid == rid;
	CHECK(!still_listed);
	CHECK(push_command(rid, "Cycle") != PushResult::Queued);
	std::printf("opennova_serve_server_command_push: rid %u, renamed, cycled to '%s', stopped\n", rid,
			cycled_to.c_str());

	holder.reset();
	stop_gate = true;
	gate_thread.join();
	listener.stop();
	net::shutdown();
	io::set_log_sink(nullptr);
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
