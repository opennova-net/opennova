// opennova-serve's files in its working directory (ADR 0051 d2), with no
// retail data: retail's dead /HOST path reads ./game.cfg, applies the host
// file over it, writes ./activesrvr.txt, saves ./game.cfg with dedicated = 1,
// and at a clean exit saves it again and deletes the lock
// [orig: Game_Run @0x4A7FBB, Game_InitSubsystems @0x4A70AB,
//  Game_HostMultiplayerSession @0x4A65C1..0x4A6604, Game_Run @0x4A7FFF..0x4A800E].
// Three runs, each in a temp working directory apart from --resource-dir:
//   1. a game.cfg with host keys off their defaults, some of which the host
//      file overrides: the session config, the cfg port range, the rewritten
//      game.cfg, the lock while serving and its delete at stop();
//   2. no game.cfg: the defaults, and the file is created;
//   3. a game.cfg with mpreset set: the start stops before anything is
//      written, as retail's load exits the process.
#include "server.h"
#include "serve_test_support.h"

#include <base/gameprofile/game_type.h>
#include <formats/gamecfg/game_cfg.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

constexpr double kFrame = 1.0 / 62.5;

// Starts a server, probing for a free port again when a parallel test took
// the probed one in between (the server binds before it boots, so a lost race
// fails fast). With `cfg`, the working directory's game.cfg is written first,
// its LAN server range the probed port alone, and no --lan-port is passed.
std::unique_ptr<serve::Server> start_server(const fs::path &resource_dir, const fs::path &host_file,
		const gamecfg::GameCfg *cfg, std::string &error) {
	std::unique_ptr<serve::Server> server;
	for (int attempt = 0; attempt < 5; ++attempt) {
		const uint16_t port = serve_test::free_udp_port();
		std::vector<std::string> args = {"--resource-dir", resource_dir.string(), "/HOST",
				host_file.string(), "--loose-root"};
		if (cfg != nullptr) {
			gamecfg::GameCfg written = *cfg;
			written.mp_lan_server_port_min = port;
			written.mp_lan_server_port_max = port;
			std::string save_error;
			CHECK(gamecfg::save_file(gamecfg::kFileName, written, {}, save_error));
		} else {
			args.push_back("--lan-port");
			args.push_back(std::to_string(port));
		}
		serve::ServeOptions options;
		CHECK(serve::parse_serve_options(args, options, error) == 0);
		server = std::make_unique<serve::Server>(options);
		if (server->start(error)) return server;
		if (error.find("bind scan") == std::string::npos) break;
	}
	return nullptr;
}

} // namespace

int main() {
	if (net::startup() != 0) return (std::printf("FAIL net::startup\n"), 1);
	const fs::path root = serve_test::fresh_dir("game_cfg_root");
	CHECK(serve_test::write_bytes(root / "SERVETST.BMS", serve_test::deathmatch_mission()));
	CHECK(serve_test::write_text(root / "override.host",
			"// the host file over game.cfg\r\n"
			"GameName \"From The Host File\"\r\n"
			"MaxPlayers 8\r\n"
			"KillLimit 20\r\n"
			"Mission SERVETST.BMS\r\n"));
	CHECK(serve_test::write_text(root / "mission_only.host", "Mission SERVETST.BMS\r\n"));
	const std::string marker_text = gamecfg::kActiveServerMarkerText;
	std::vector<fs::path> dirs = {root};

	// --- 1. a game.cfg off its defaults, the host file over it.
	{
		const fs::path work = serve_test::fresh_dir("game_cfg_work");
		dirs.push_back(work);
		serve_test::ScopedCwd cwd(work);
		gamecfg::DefaultTexts texts;
		texts.untitled = "Untitled";
		texts.user_message = "Put your message here.";
		gamecfg::GameCfg cfg = gamecfg::defaults(texts);
		cfg.game_name = "From game.cfg";        // the host file overrides it
		cfg.servermsg = "the cfg's own message";  // kept
		cfg.mp_max_players = 20;                 // the host file overrides it
		cfg.max_kills = 30;                      // the host file overrides it
		cfg.time_limit = 40;                     // kept: g_RespawnTime
		cfg.mp_num_spectators_max = 3;           // game.cfg alone
		cfg.enable_ai = 0;                       // game.cfg alone
		cfg.mp_tod_continuity = 1;               // game.cfg alone
		cfg.mp_use_lineup_queue = 0;
		cfg.mp_lineup_queue_size = 7;
		cfg.remote_admin_port = 4711;            // the admin server's (ADR 0051 PR5)
		cfg.music_volume = 77;                   // no host setting: round-trips
		cfg.dedicated = 0;
		std::string error;
		std::unique_ptr<serve::Server> server =
				start_server(root, root / "override.host", &cfg, error);
		if (!server) std::printf("start: %s\n", error.c_str());
		CHECK(server != nullptr);
		if (server) {
			// The session config: the host file's keys over game.cfg's.
			const inmatch::GameConfig &config = server->role().state.host_owner.ctx.config;
			CHECK(config.server_name == "From The Host File");
			CHECK(config.custom_text == "the cfg's own message");
			CHECK(config.max_players == 9u); // 8 plus the dedicated slot
			CHECK(config.score_limit == 20u);
			CHECK(config.respawn_time == 40u);
			CHECK(config.spectator_slots == 3);
			CHECK(!config.allow_ai);
			CHECK(config.time_of_day_continuity == 1);
			CHECK(config.game_type == game_type::kDeathmatch);
			CHECK(server->host_screen().use_lineup_queue == 0);
			CHECK(server->host_screen().lineup_queue_size == 7);
			CHECK(!server->role().state.host_owner.serve_and_play);
			CHECK(server->game_cfg().remote_admin_port == 4711);
			// The cfg's one-port range was the bind scan.
			const gamecfg::LoadResult on_disk = gamecfg::load_file(gamecfg::kFileName, {});
			CHECK(on_disk.file_read);
			// game.cfg rewritten as Serve Only, the host file's keys in it, the
			// rest as it was.
			CHECK(on_disk.cfg.dedicated == 1);
			CHECK(on_disk.cfg.game_name == "From The Host File");
			CHECK(on_disk.cfg.mp_max_players == 8);
			CHECK(on_disk.cfg.max_kills == 20);
			CHECK(on_disk.cfg.servermsg == "the cfg's own message");
			CHECK(on_disk.cfg.music_volume == 77);
			CHECK(on_disk.cfg.remote_admin_port == 4711);
			CHECK(server->bound_port() == static_cast<uint16_t>(on_disk.cfg.mp_lan_server_port_min));
			// The lock while serving: the one LF-framed line.
			CHECK(fs::exists(work / gamecfg::kActiveServerMarkerFileName));
			CHECK(serve_test::read_text(work / gamecfg::kActiveServerMarkerFileName) == marker_text);
			// Nothing written under --resource-dir.
			CHECK(!fs::exists(root / gamecfg::kFileName));
			CHECK(!fs::exists(root / gamecfg::kActiveServerMarkerFileName));
			for (int f = 0; f < 10; ++f) CHECK(server->frame(kFrame));
			// The map change's save (PR3 calls it).
			CHECK(server->save_config());
			server->stop();
			CHECK(!server->frame(kFrame));
			// The clean exit: game.cfg saved again, the lock gone.
			CHECK(!fs::exists(work / gamecfg::kActiveServerMarkerFileName));
			const gamecfg::LoadResult after = gamecfg::load_file(gamecfg::kFileName, {});
			CHECK(after.file_read && after.cfg.dedicated == 1);
			CHECK(after.cfg.game_name == "From The Host File");
			// stop() is idempotent and owes nothing more.
			fs::remove(work / gamecfg::kFileName);
			server->stop();
			CHECK(!fs::exists(work / gamecfg::kFileName));
		}
	}

	// --- 2. no game.cfg: the defaults, and the file is created.
	{
		const fs::path work = serve_test::fresh_dir("game_cfg_fresh");
		dirs.push_back(work);
		serve_test::ScopedCwd cwd(work);
		std::string error;
		std::unique_ptr<serve::Server> server =
				start_server(root, root / "mission_only.host", nullptr, error);
		if (!server) std::printf("start: %s\n", error.c_str());
		CHECK(server != nullptr);
		if (server) {
			const inmatch::GameConfig &config = server->role().state.host_owner.ctx.config;
			// No gametext.bin in the loose root: the `!` default name.
			CHECK(config.server_name == "!Untitled");
			CHECK(config.max_players == 65u); // the stock 64 plus the dedicated slot
			CHECK(config.score_limit == game_rules::kDefaultScoreLimit);
			CHECK(config.respawn_time == game_rules::kDefaultRespawnTime);
			CHECK(config.spectator_slots == 0 && config.allow_ai);
			CHECK(fs::exists(work / gamecfg::kFileName));
			CHECK(fs::exists(work / gamecfg::kActiveServerMarkerFileName));
			const gamecfg::LoadResult on_disk = gamecfg::load_file(gamecfg::kFileName, {});
			CHECK(on_disk.file_read && !on_disk.version_reset);
			CHECK(on_disk.cfg.version == gamecfg::kConfigVersion);
			CHECK(on_disk.cfg.dedicated == 1);
			CHECK(on_disk.cfg.mp_max_players == 64);
			CHECK(on_disk.cfg.game_name == "!Untitled");
			// Every other key as a load of no file leaves it (the defaults, then
			// the graphics clamp).
			gamecfg::GameCfg expected = gamecfg::load(nullptr, 0, {}).cfg;
			expected.dedicated = 1;
			CHECK(serve_test::read_text(work / gamecfg::kFileName) == gamecfg::write(expected, {}));
			server->stop();
			CHECK(!fs::exists(work / gamecfg::kActiveServerMarkerFileName));
			CHECK(fs::exists(work / gamecfg::kFileName));
		}
	}

	// --- 3. mpreset: the start stops at the first read; nothing is written.
	{
		const fs::path work = serve_test::fresh_dir("game_cfg_reset");
		dirs.push_back(work);
		serve_test::ScopedCwd cwd(work);
		gamecfg::GameCfg cfg = gamecfg::defaults();
		cfg.mp_reset = 1;
		cfg.game_name = "Reset";
		std::string save_error;
		CHECK(gamecfg::save_file(gamecfg::kFileName, cfg, {}, save_error));
		const std::string before = serve_test::read_text(work / gamecfg::kFileName);
		serve::ServeOptions options;
		std::string error;
		CHECK(serve::parse_serve_options({"--resource-dir", root.string(), "/HOST",
						  (root / "mission_only.host").string(), "--loose-root", "--lan-port",
						  std::to_string(serve_test::free_udp_port())},
					  options, error) == 0);
		serve::Server server(options);
		CHECK(!server.start(error));
		CHECK(server.reset_exit());
		CHECK(!fs::exists(work / gamecfg::kActiveServerMarkerFileName));
		CHECK(serve_test::read_text(work / gamecfg::kFileName) == before);
	}

	net::shutdown();
	std::error_code ec;
	for (const fs::path &dir : dirs) fs::remove_all(dir, ec);
	if (failures != 0) {
		std::printf("opennova_serve_game_cfg: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_game_cfg: ok\n");
	return 0;
}
