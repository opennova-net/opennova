// opennova-serve end to end (ADR 0051), with no retail data: a loose game
// directory holding two synthetic missions and a retail-format host file. The
// server mounts it, reads the host file, binds a real UDP socket, boots the
// starting mission headless as a DedicatedHost, answers a LAN browser's 0x41
// probe with its 0x81 ServerHello, and admits a LAN joiner (a ClientRuntime on
// a second real socket) through the handshake into the match. Then the round
// ends three times: the server runs the map change twice, each time inside
// the same session, and the joiner reloads in place on its kept connection
// (D-NET-331); at the third the rotation (Replay 0) runs out, the joiner
// receives the STOP goodbye and the server reports the rotation's end, having
// saved game.cfg at each map change. The run happens in a temp working directory, where
// the server writes its game.cfg and activesrvr.txt (ADR 0051 d2; the files
// themselves are opennova_serve_game_cfg's).
#include "server.h"
#include "serve_test_support.h"

#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <net/npwire/lan_discovery.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/server_admin_command.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;
using serve_test::deathmatch_mission;
using serve_test::write_bytes;

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
	const fs::path dir = serve_test::fresh_dir("lan_join");
	const fs::path work = serve_test::fresh_dir("lan_join_cwd");
	serve_test::ScopedCwd cwd(work);
	CHECK(write_bytes(dir / "SERVETST.BMS", deathmatch_mission()));
	CHECK(write_bytes(dir / "SERVETS2.BMS", deathmatch_mission("Serve Test Map Two")));
	// A loose score.ini with a deathmatch row that is not the default table:
	// it must reach world.match, which the bring-up configures.
	{
		std::ofstream score(dir / "score.ini", std::ios::binary);
		score << "VERSION 40\r\n"
		      << "GAMETYPE \"DM\"\r\n"
		      << "FIELD \"NUMENEMYKILLS\" 1\r\n"
		      << "VAR \"FIRE\" 7\r\n"
		      << "VAR \"ENEMYKILL\" 9\r\n";
	}
	// charattr.def: class 2 is the medic. The authority's class attribute table
	// must carry it (the game's own host never loads the file, D-NET-345).
	{
		std::ofstream charattr(dir / "charattr.def", std::ios::binary);
		charattr << "[CHARACTER1]\r\nATTRIBUTES\t= AutoScope\r\n"
		         << "[CHARACTER2]\r\nATTRIBUTES\t= Medic\r\n";
	}
	{
		std::ofstream host(dir / "test.host", std::ios::binary);
		host << "// opennova-serve test host file\r\n"
		     << "GameName \"Serve Test\"\r\n"
		     << "ServerMessage \"hello from the test\"\r\n"
		     << "MaxPlayers 8\r\n"
		     << "KillLimit 20\r\n"
		     << "TeamFF 1\r\n"
		     << "Replay 0\r\n"
		     // The rotation: the last line names the starting map, the first
		     // entry with its file; the round ends then take SERVETS2.BMS, the
		     // third entry, and the end of the list.
		     << "Mission servetst.bms\r\n"
		     << "Mission NOSUCH.BMS\r\n"
		     << "Mission servets2.bms\r\n"
		     << "Mission servetst.bms\r\n";
	}

	auto parse_options = [&](uint16_t port, serve::ServeOptions &out) {
		std::string error;
		return serve::parse_serve_options(
				{"--resource-dir", dir.string(), "/host", (dir / "test.host").string(), "--loose-root",
						"--lan-port", std::to_string(port)},
				out, error);
	};
	{
		serve::ServeOptions parsed;
		CHECK(parse_options(4321, parsed) == 0 && parsed.port == 4321);
		std::string error;
		serve::ServeOptions missing;
		CHECK(serve::parse_serve_options({"--resource-dir", dir.string()}, missing, error) == 1);
		CHECK(serve::parse_serve_options({"--help"}, missing, error) == -1);
		// `/mod` is `/exp`; the last one wins and keeps 32 bytes [orig:
		// Game_ParseCommandLineAndInit @ 0x4a76ac / 0x4a76cf].
		serve::ServeOptions mod;
		CHECK(serve::parse_serve_options({"--resource-dir", dir.string(), "/host", "x.host", "/exp",
						"a", "/MOD", "abcdefghijklmnopqrstuvwxyz0123456789"},
				mod, error) == 0);
		CHECK(mod.expansion == "abcdefghijklmnopqrstuvwxyz012345");
		// No --lan-port: the scan starts at the cfg range's head.
		CHECK(!mod.port.has_value());
	}

	// --lan-port 0: the socket binds the port the OS picks, and bound_port()
	// reports the port it holds from then on.
	serve::ServeOptions options;
	CHECK(parse_options(0, options) == 0 && options.port == 0);
	auto holder = std::make_unique<serve::Server>(options);
	std::string error;
	const bool started = holder->start(error);
	serve::Server &server = *holder;
	if (!started) {
		std::printf("start: %s (catalog rows: %zu)\n", error.c_str(), server.catalog().size());
		for (const mission_catalog::Row &row : server.catalog())
			std::printf("  row '%s'\n", row.file.c_str());
	}
	CHECK(started);
	if (!started) return 1;

	// The host file reached the session: the name, the message, the cap (8; the
	// slot limit adds the dedicated slot, 8 + 1), the 20-kill limit, the starting
	// map; the unknown mission was reported and skipped.
	const inmatch::GameConfig &config = server.role().state.host_owner.ctx.config;
	CHECK(config.server_name == "Serve Test");
	CHECK(config.custom_text == "hello from the test");
	CHECK(config.max_players == 8u && config.player_slot_limit() == 9u);
	CHECK(config.score_limit == 20u);
	CHECK(config.mission_file == "servetst.bms");
	CHECK(config.mission_name == "Serve Test Map");
	CHECK(config.game_type == game_type::kDeathmatch);
	CHECK(server.host_file_report().unknown_missions.size() == 1);
	CHECK(server.rotation().list.count == 3);
	CHECK(server.rotation().list.cursor == 0);
	CHECK(server.missions_played() == 1);
	// The score.ini row reached the match the bring-up configured: its values
	// (FIRE status 0, ENEMYKILL status 3) and its one FIELD row.
	const world::Match &match = server.kernel().world.match;
	CHECK(match.rules().score_values.has_value());
	CHECK(match.rules().score_values && (*match.rules().score_values)[0] == 7);
	CHECK(match.rules().score_values && (*match.rules().score_values)[3] == 9);
	CHECK(match.rules().score_fields.size() == 1);
	// The authority's class attributes came from charattr.def.
	const world::MissionTables &tables = server.kernel().world.tables;
	CHECK(tables.class_has_attribute(2, world::MissionTables::kCharAttrMedic));
	CHECK(!tables.class_has_attribute(1, world::MissionTables::kCharAttrMedic));
	CHECK(tables.class_attribute_flags[0] == 0x1u); // CHARACTER1: AutoScope
	// Serve Only: the dedicated role, no player of the host's own, no local client.
	CHECK(server.role().kind() == inmatch::RoleKind::DedicatedHost);
	CHECK(!server.role().state.host_owner.serve_and_play);
	CHECK(server.role().client_runtime() == nullptr);
	CHECK(!server.kernel().local.has_local_player());
	CHECK(server.kernel().world.rules.mp_session);
	// A joiner's body binds its items.def row at its spawn (D-NET-395).
	CHECK(static_cast<bool>(server.role().state.host_owner.ctx.bind_spawned_body));
	CHECK(server.bound_port() != 0);
	// No game.cfg: remote_admin_port is 0, so no admin listener; the launch still truncates
	// admin_log.txt, as retail's static construction opens it [orig: Game_InitSubsystems
	// @0x4A72C7..0x4A72D1; CAdminServer_Construct @0x402C84].
	CHECK(server.admin_port() == 0);
	CHECK(fs::exists(work / "admin_log.txt") && fs::file_size(work / "admin_log.txt") == 0);

	constexpr double kFrame = 1.0 / 62.5;
	const net::Endpoint server_ep{{127, 0, 0, 1}, server.bound_port()};

	// --- a LAN browser's probe: the server answers on its game socket.
	{
		uint16_t browse_port = 0;
		net::ScopedSocket browse(net::udp_bind(0, &browse_port));
		CHECK(browse.is_valid());
		const uint32_t ci = 0x1234;
		const std::vector<uint8_t> probe = build_lan_discovery_probe(ci);
		net::udp_send_to(browse.get(), server_ep, probe.data(), probe.size());
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
		CHECK(found.server_name == "Serve Test");
		// MP is the session's cap, never the dedicated slot [orig: protocol
		// +0x524 @0x6209FF = ctx+0xF28 @0x4C9A80].
		CHECK(found.max_players == 8u);
		CHECK(found.current_players == 0u);
	}

	// --- a LAN joiner: the handshake, the spawn-gate burst, into the match.
	uint16_t joiner_port = 0;
	net::ScopedSocket joiner_sock(net::udp_bind(0, &joiner_port));
	CHECK(joiner_sock.is_valid());
	inmatch::ClientRuntime client("ServeJoiner");
	auto ship = [&](const std::vector<uint8_t> &d) {
		if (!d.empty()) net::udp_send_to(joiner_sock.get(), server_ep, d.data(), d.size());
	};
	auto drain = [&]() {
		uint8_t rx[4096];
		net::Endpoint from{};
		for (;;) {
			const int n = net::udp_recv_from(joiner_sock.get(), rx, sizeof(rx), from, 0);
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
	// The joiner plays: it deploys at the deathmatch start marker, and its body
	// is the host's pool-0 entity its connection owns.
	for (int f = 0; f < 600 && in_match && !client.is_deployed(); ++f) {
		if (!server.frame(kFrame)) break;
		drain();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
		++tick;
	}
	CHECK(client.is_deployed());
	// The host holds the joiner as its one remote (type-1) connection.
	int remotes = 0;
	world::EntityHandle body{};
	for (const inmatch::NapiNPConnection &c : server.role().state.host_owner.ctx.np_protocol.connection_list) {
		if (c.type != inmatch::NapiNPConnection::kTypeServerSide) continue;
		++remotes;
		body = c.link.owned_entity;
	}
	CHECK(remotes == 1);
	CHECK(body.valid());
	CHECK(body.valid() && client.self_handle() == body.packed);
	std::printf("opennova_serve: joiner in match after %u client frames on UDP %u\n", tick,
			server.bound_port());

	// --- the map rotation: a round end, the map change, the joiner's reload.
	uint32_t kept_connection_id = 0;
	for (const inmatch::NapiNPConnection &c : server.role().state.host_owner.ctx.np_protocol.connection_list)
		if (c.type == inmatch::NapiNPConnection::kTypeServerSide) kept_connection_id = c.connection_id;
	// One round end, then frames until the joiner is back in a match on the
	// next map. The Cycle command ends the round with the 620-tick linger
	// [orig: CNapiGameSession_HandleServerCommand @0x4D31CA].
	auto play_round_end = [&](int expect_missions) {
		inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
		CHECK(inmatch::Server_ExecuteServerCommand(ctx, &server.kernel().world, "Cycle", "", {})
						.handled);
		bool reloaded = false;
		bool back_in_match = false;
		for (int f = 0; f < 4000 && !back_in_match; ++f) {
			if (!server.frame(kFrame)) break;
			drain();
			for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship(d);
			++tick;
			// The joiner's stored exit 4, in session and not the authority, is
			// the Game Loop: the reload in place (main_frame_exit).
			const int32_t reason = client.mission_exit_reason();
			if (reason != 0) {
				CHECK(reason == inmatch::kMissionExitRoundOver);
				CHECK(inmatch::main_frame_exit(reason, /*in_session=*/true, /*authority=*/false) ==
						inmatch::MainFrameExit::GameLoop);
				CHECK(client.begin_mission_reload());
				reloaded = true;
				continue;
			}
			back_in_match = reloaded && client.in_match() && client.is_deployed() &&
					server.missions_played() == expect_missions;
		}
		CHECK(reloaded);
		return back_in_match;
	};

	// The map change saves game.cfg, as the PreMenu's init does at every map
	// change [orig: PreMenu_Init @0x5693E0 -> Game_SaveConfig @0x5693F4].
	std::error_code removed;
	fs::remove(work / "game.cfg", removed);
	CHECK(!fs::exists(work / "game.cfg"));
	CHECK(play_round_end(2));
	CHECK(fs::exists(work / "game.cfg"));
	CHECK(server.missions_played() == 2);
	CHECK(server.rotation().list.cursor == 1);
	CHECK(server.role().state.host_owner.ctx.config.mission_file == "SERVETS2.BMS");
	CHECK(server.role().state.host_owner.ctx.config.mission_name == "Serve Test Map Two");
	// The next map's session still carries the block's settings, re-applied
	// at its mission start [orig: Game_StartMission @0x524662].
	CHECK(server.role().state.host_owner.ctx.config.server_name == "Serve Test");
	CHECK(server.role().state.host_owner.ctx.config.score_limit == 20u);
	// The joiner reloaded on its kept connection: the one remote is the same
	// connection, now owning a fresh body in the next map, and the 0x64 block
	// named the next map's file.
	{
		int kept = 0;
		world::EntityHandle body2{};
		for (const inmatch::NapiNPConnection &c :
				server.role().state.host_owner.ctx.np_protocol.connection_list) {
			if (c.type != inmatch::NapiNPConnection::kTypeServerSide) continue;
			++kept;
			CHECK(c.connection_id == kept_connection_id);
			body2 = c.link.owned_entity;
		}
		CHECK(kept == 1);
		CHECK(body2.valid() && client.self_handle() == body2.packed);
		CHECK(client.map_file() == "SERVETS2.BMS");
		CHECK(!client.session_lost());
	}
	// The transfer tokens moved with the mission start (1 -> 2), so the
	// joiner's stale requests were served from offset 0.
	CHECK(server.role().state.host_owner.ctx.server_info_transfer_id == 2);
	CHECK(server.role().state.host_owner.ctx.mission_metadata_transfer_id == 2);

	CHECK(play_round_end(3));
	CHECK(server.rotation().list.cursor == 2);
	CHECK(client.map_file() == "SERVETST.BMS");
	std::printf("opennova_serve: joiner reloaded through %d missions on UDP %u\n",
			server.missions_played(), server.bound_port());

	// The last round end finds no next entry with Replay 0: the session ends
	// (StopServer's goodbye) and the server reports the rotation's end.
	{
		inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
		CHECK(inmatch::Server_ExecuteServerCommand(ctx, &server.kernel().world, "Cycle", "", {})
						.handled);
		bool ended = false;
		for (int f = 0; f < 2000 && !ended; ++f) {
			ended = !server.frame(kFrame);
			drain();
			(void)client.Client_ProcessNetworkFrame(tick++);
		}
		CHECK(ended);
		CHECK(server.rotation_ended());
		CHECK(server.missions_played() == 3);
	}
	for (int f = 0; f < 120 && !client.session_lost(); ++f) {
		drain();
		(void)client.Client_ProcessNetworkFrame(tick++);
	}
	CHECK(client.session_lost());
	CHECK(client.has_disconnect_event() &&
			client.last_disconnect_event().ddstr == "NP.C:SH:STOP");
	server.stop();
	net::shutdown();
	cwd.restore();
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::remove_all(work, ec);
	if (failures != 0) {
		std::printf("opennova_serve: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve: ok\n");
	return 0;
}
