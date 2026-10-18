// The service's admin link (apps/novaworld_fleet/hosted/admin_link.h) against a
// real opennova-serve child (ctest passes the binary) with its remote-admin port
// open, over the synthetic loose game directory the hosted e2e serves
// (serve_test_support.h's deathmatch map), its admin.cfg and host file rendered
// by the hosted renders (hosted_settings.h) and its game.cfg the defaults with
// the admin port, on the LAN network type so it runs with no NovaWorld. No
// retail data. The link:
//
//   * logs in (the challenge, the encrypted login) and says so in the console;
//   * runs PLAYER LIST (the table, one packet);
//   * sends CHAT SEND and reads the line back with CHAT GET;
//   * frames each command's replies by the dispatcher's count: SET with a key
//     the server does not know draws two packets, PLAYER BAN on no slot one, a
//     PLAYER sub-verb the server does not know the quiet gap, GOTO alone one,
//     each followed by GET GAMESTATE answering in step;
//   * polls the server's scene;
//   * reconnects, with the same login, after the child is killed and started
//     again on the same port;
//   * ends with QUIT (the server logs it);
//   * never puts the admin password in a log line or a console line, and masks
//     it as a log secret only while it lives.
//
// The supervisor's side (a link up from running, down at stopping) is
// hosted_supervisor's and hosted_local_e2e's.

#include "hosted/admin_link.h"
#include "hosted/hosted_repository.h"
#include "hosted/hosted_settings.h"
#include "loggable.h"
#include "serve/serve_test_support.h"

#include "net_sockets.h"

#include <base/proc/child_process.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/novaworld/db/database.h>

#include "common/temp_dir.h"
#include "common/test_database.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace fs = std::filesystem;
namespace net = opennova::net;
namespace hosted = opennova::novaworld_server::hosted;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

template <typename Done>
bool wait_for(Done done, int seconds) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (!done() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	return done();
}

bool starts_with(const std::string &text, const std::string &prefix) {
	return text.compare(0, prefix.size(), prefix) == 0;
}

std::string joined(const std::vector<std::string> &replies) {
	std::string out;
	for (const std::string &r : replies) out += "[" + r + "]";
	return out;
}

// The link's log lines, kept for the secret check.
struct LogTap {
	std::mutex mu;
	std::vector<std::string> lines;
	void add(const std::string &line) {
		std::printf("%s\n", line.c_str());
		std::lock_guard<std::mutex> lock(mu);
		lines.push_back(line);
	}
	bool contains(const std::string &needle) {
		std::lock_guard<std::mutex> lock(mu);
		for (const std::string &line : lines)
			if (line.find(needle) != std::string::npos) return true;
		return false;
	}
};

std::vector<hosted::ConsoleLine> console(opennova::db::ConnectionPool &pool, int64_t id) {
	return hosted::list_console(*pool.acquire(), id, 0, hosted::kConsoleKeep);
}

size_t count_status(opennova::db::ConnectionPool &pool, int64_t id, const std::string &prefix) {
	size_t n = 0;
	for (const auto &line : console(pool, id))
		if (line.kind == "status" && starts_with(line.text, prefix)) ++n;
	return n;
}

hosted::AdminCommandResult run(hosted::AdminLink &link, const std::string &command, bool record = false) {
	hosted::AdminCommandOptions options;
	options.actor = "owner";
	options.record = record;
	return link.run(command, options, std::chrono::milliseconds(3000));
}

// One command's replies, answered; else a line saying why.
std::vector<std::string> replies_of(hosted::AdminLink &link, const std::string &command, bool record = false) {
	const hosted::AdminCommandResult result = run(link, command, record);
	if (result.outcome != hosted::AdminCommandOutcome::Answered) {
		std::fprintf(stderr, "  %s -> %s\n", command.c_str(), hosted::admin_command_outcome_name(result.outcome));
		return { "<" + std::string(hosted::admin_command_outcome_name(result.outcome)) + ">" };
	}
	return result.replies;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (argc < 2) {
		std::fprintf(stderr, "usage: hosted_admin_link_test <opennova-serve binary>\n");
		return 2;
	}
	const std::string serve_binary = argv[1];
	if (net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	const std::string password = hosted::random_secret(hosted::kAdminPasswordChars);
	{
		// Declared first, so the files go last.
		test_temp::TempDir temp("hosted_admin_link");
		const fs::path data = temp.path / "data";
		const fs::path work = temp.path / "work";
		fs::create_directories(data);
		fs::create_directories(work);
		expect(serve_test::write_bytes(data / "SERVETST.BMS", serve_test::deathmatch_mission()), "the map");

		// The working directory: the hosted renders' host file and admin.cfg (user svc,
		// rights 7F, ip_restrict loopback), and game.cfg's defaults with the admin port and
		// the LAN server port the OS picks.
		hosted::HostedSettings settings;
		settings.game_name = "Admin Link Test";
		settings.max_players = 8;
		settings.missions = { "servetst.bms" };
		const uint16_t admin_port = serve_test::free_tcp_port();
		expect(admin_port != 0, "a free admin port");
		hosted::SlotPlan plan;
		plan.admin_port = admin_port;
		plan.admin_allowed_ip = "127.0.0.1";
		opennova::gamecfg::GameCfg cfg = opennova::gamecfg::defaults();
		cfg.mp_lan_server_port_min = 0;
		cfg.mp_lan_server_port_max = 0;
		cfg.remote_admin_port = admin_port;
		expect(serve_test::write_text(work / hosted::kHostFileName, hosted::render_host_file(settings, 1)) &&
						serve_test::write_text(work / hosted::kGameCfgFileName, opennova::gamecfg::write(cfg, {})) &&
						serve_test::write_text(work / hosted::kAdminCfgFileName,
								hosted::render_admin_cfg(plan, password)),
				"the working directory");

		opennova::proc::SpawnOptions spawn;
		spawn.argv = { serve_binary, "--resource-dir", data.u8string(), "--loose-root", "/HOST",
			hosted::kHostFileName };
		spawn.working_dir = work.u8string();
		spawn.output_path = (work / "console.log").u8string();
		opennova::proc::ChildProcess child;
		std::string error;
		expect(child.spawn(spawn, error), "opennova-serve starts");
		if (!error.empty()) std::fprintf(stderr, "  spawn: %s\n", error.c_str());

		// The console's database, with the server's row.
		test_db::TestDatabase scratch;
		opennova::db::ConnectionPool pool(scratch.options());
		int64_t server_id = 0;
		{
			auto db = pool.acquire();
			server_id = db->query("INSERT INTO hosted_servers (state, game_name, max_players, time_limit, "
								  "kill_limit, max_score, respawn, start_delay, missions) "
								  "VALUES ('running', 'Admin Link Test', 8, 30, 50, 5, 5, 0, 'servetst.bms') "
								  "RETURNING id;")
								.front()
								.as_int(0)
								.value();
		}

		LogTap tap;
		hosted::AdminLinkConfig config;
		config.status_interval_ms = 500;
		config.reconnect_min_ms = 100;
		config.reconnect_max_ms = 500;
		config.log = [&tap](const std::string &line) { tap.add(line); };
		hosted::AdminLinkTarget target;
		target.host = "127.0.0.1";
		target.port = admin_port;
		target.user = hosted::kAdminUser;
		target.password = password;
		{
			hosted::AdminLink link(server_id, target, &pool, config);
			expect(opennova::novaworld_server::mask_log_secrets("pw=" + password) == "pw=***",
					"the password is a log secret while the link lives");
			link.start();
			const auto up = [&link] { return link.status().state == hosted::AdminLinkState::Up; };

			// The login.
			const bool logged_in = wait_for(up, 60);
			if (!logged_in) {
				std::fprintf(stderr, "  link: %s (%s)\n", hosted::admin_link_state_name(link.status().state),
						link.status().error.c_str());
				std::fprintf(stderr, "---- console.log\n%s\n----\n", serve_test::read_text(work / "console.log").c_str());
			}
			expect(logged_in, "the link logs in");
			expect(wait_for([&] { return count_status(pool, server_id, "the admin session is up") == 1; }, 5),
					"the console says the session is up");

			if (logged_in) {
				// PLAYER LIST: the table in one packet.
				auto replies = replies_of(link, "PLAYER LIST");
				expect(replies.size() == 1 && starts_with(replies[0], "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n"),
						"PLAYER LIST answers the table");

				// CHAT SEND, recorded, then CHAT GET reads the line back.
				replies = replies_of(link, "CHAT SEND hello from the admin link", /*record=*/true);
				expect(replies == std::vector<std::string>{ "OK - Chat sent." }, "CHAT SEND is sent");
				std::vector<std::string> window;
				const bool read_back = wait_for([&] {
					window = replies_of(link, "CHAT GET");
					return window.size() == 1 && window[0].find("hello from the admin link") != std::string::npos;
				}, 5);
				if (!read_back) std::fprintf(stderr, "  CHAT GET: %s\n", joined(window).c_str());
				expect(read_back, "CHAT GET reads the line back");

				// The framing, each followed by a command answering in step.
				const std::vector<std::string> in_step = { "OK - Current State = Game" };
				replies = replies_of(link, "SET NoSuchSetting 1");
				expect(replies.size() == 2 && starts_with(replies[0], "USAGE -  SET [") &&
								replies[1] == "ERROR - Setting Not Found.",
						"SET with an unknown key draws the usage and the error");
				expect(replies_of(link, "GET GAMESTATE") == in_step, "in step after the two");
				replies = replies_of(link, "PLAYER BAN 77");
				expect(replies == std::vector<std::string>{ "ERROR - Player Not Found." },
						"PLAYER BAN on no slot draws one");
				expect(replies_of(link, "GET GAMESTATE") == in_step, "in step after the ban");
				replies = replies_of(link, "PLAYER NOSUCHVERB 0");
				expect(replies.size() <= 1, "a PLAYER sub-verb the server does not know: the quiet gap");
				expect(replies_of(link, "GET GAMESTATE") == in_step, "in step after the quiet gap");
				replies = replies_of(link, "GOTO");
				expect(replies == std::vector<std::string>{ "USAGE -  GOTO [GAMESTATE | MENUSTATE]" },
						"GOTO alone draws its usage");
				expect(replies_of(link, "GET GAMESTATE") == in_step, "in step after GOTO");
				// A line no packet may carry is refused without a send.
				expect(run(link, "CHAT SEND two\nlines").outcome == hosted::AdminCommandOutcome::Refused,
						"a control byte is refused");

				// The status poll.
				expect(wait_for([&link] {
					const auto s = link.status();
					return s.game_state == "Game" && !s.last_poll_at.empty();
				}, 5),
						"the status poll reads the scene");

				// The recorded command and its reply are in the console; the reads are not.
				bool command_seen = false;
				bool reply_seen = false;
				bool read_recorded = false;
				for (const auto &line : console(pool, server_id)) {
					if (line.kind == "command" && line.actor == "owner" &&
							line.text == "CHAT SEND hello from the admin link")
						command_seen = true;
					if (line.kind == "reply" && line.text == "OK - Chat sent.") reply_seen = true;
					if (line.kind == "command" && line.text == "PLAYER LIST") read_recorded = true;
				}
				expect(command_seen && reply_seen && !read_recorded, "the console holds the recorded command");

				// The child killed: the link notices, and logs in again, same login, once the
				// server is back on the same port.
				child.kill();
				expect(child.wait(10000).has_value(), "the child ends");
				expect(wait_for([&] { return !up(); }, 10), "the link notices the server is gone");
				opennova::proc::ChildProcess again;
				expect(again.spawn(spawn, error), "opennova-serve starts again");
				const bool relogged = wait_for(up, 60);
				if (!relogged)
					std::fprintf(stderr, "---- console.log\n%s\n----\n",
							serve_test::read_text(work / "console.log").c_str());
				expect(relogged, "the link logs in again");
				expect(replies_of(link, "GET GAMESTATE") == in_step, "the new session answers");
				expect(count_status(pool, server_id, "the admin session was lost") >= 1 &&
								wait_for([&] { return count_status(pool, server_id, "the admin session is up") == 2; }, 5),
						"the console tells the loss and the new session");

				// The end: QUIT, which the server logs.
				link.request_stop();
				link.join();
				expect(link.status().state == hosted::AdminLinkState::Stopped, "the link ends");
				expect(count_status(pool, server_id, "the admin session is closed") == 1, "the console says closed");
				const bool quit_logged = wait_for([&] {
					return serve_test::read_text(work / "admin_log.txt").find("User command (svc) - QUIT") !=
							std::string::npos;
				}, 5);
				expect(quit_logged, "the session ended with QUIT");
				again.kill();
				again.wait(10000);
			}
		}

		// The secret: in no log line, no console line, and no longer a log secret.
		expect(!tap.contains(password), "no log line carries the admin password");
		for (const auto &line : console(pool, server_id))
			expect(line.text.find(password) == std::string::npos, "no console line carries the admin password");
		expect(opennova::novaworld_server::mask_log_secrets("pw=" + password) == "pw=" + password,
				"the password stops being a log secret with its link");
		child.kill();
		child.wait(10000);
	}
	net::shutdown();
	if (g_failures == 0) {
		std::printf("OK: the admin link logs in, frames each reply, reconnects and keeps its secret\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
