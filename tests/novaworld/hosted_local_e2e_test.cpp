// A hosted server end to end with LocalProcessProvider: an in-process NovaWorld
// service (nw_service_harness.h, hosted servers on) and its supervisor run a real
// opennova-serve child (ctest passes the binary) over synthetic loose game
// directories (serve_test_support.h's deathmatch map and game text, as the serve
// tests make them), one per data set: Joint Operations' and Delta Force Xtreme
// 2's. Built only with BUILD_NOVAWORLD_HTTP.
//
//   * the owner, granted a server (players.comp_servers), starts a Joint Operations server
//     with POST /api/me/server/start, which runs over that data set's folder
//     with /game jo;
//   * the supervisor mints its service account srv-<id>, renders its working
//     directory and launches opennova-serve there, which logs in as that
//     account through /NWLogin.dll and /NWHost.dll, hosts and lists; the server
//     reaches running;
//   * GET /api/me/hosts shows the listing as the owner's hosted server (the
//     service account's claim resolved to its owner);
//   * the owner's SetServerName (POST /api/me/hosts/<rid>/command) reaches the
//     server, which saves game.cfg in its working directory with the name;
//   * the supervisor's admin link comes up once the server runs: it logs in on
//     the server's remote-admin port with the rendered svc login, runs PLAYER
//     LIST and a recorded CHAT SEND, and the console ring holds
//     the session's line, the command and its reply, never the password;
//   * POST /api/me/server/stop: the supervisor pushes ServerStopHosting, the
//     listing leaves the browser, the process is gone, the row is stopped and
//     the service account deleted; the admin link went down at stopping (QUIT,
//     the console's closing line);
//   * a second owner starts a Delta Force Xtreme 2 server naming no missions:
//     it takes that data set's rotation, runs over its own folder with /game
//     dfx2, reaches running, and stops.

#include "nw_service_harness.h"

#include "hosted/admin_link.h"
#include "hosted/hosted_repository.h"
#include "hosted/local_process_provider.h"
#include "hosted/supervisor.h"
#include "serve/serve_test_support.h"
#include "web_session.h"

#include "net_http.h"
#include "net_sockets.h"

#include <base/io/file_io.h>
#include <base/io/log.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/napi/session.h>
#include <net/novaworld/host_repository.h>

#include <crow/json.h>

#include "common/temp_dir.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace fs = std::filesystem;
namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
using nw_service_test::kOwner;
using nw_service_test::kOwnerPassword;
using nw_service_test::Service;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

// The JSON reply's member at `path` (object keys), or null when the reply is no
// JSON or any step is missing or no object: Crow throws on a missing key, and a
// throw would end the test with no dump and no cleanup.
const crow::json::rvalue *member(const crow::json::rvalue &json, std::initializer_list<const char *> path) {
	const crow::json::rvalue *at = &json;
	for (const char *key : path) {
		if (!*at || at->t() != crow::json::type::Object || !at->has(key)) return nullptr;
		at = &(*at)[key];
	}
	return at;
}

std::optional<int64_t> integer_at(const crow::json::rvalue &json, std::initializer_list<const char *> path) {
	const auto *v = member(json, path);
	if (!v || v->t() != crow::json::type::Number) return std::nullopt;
	return v->i();
}

std::string text_at(const crow::json::rvalue &json, std::initializer_list<const char *> path) {
	const auto *v = member(json, path);
	return v && v->t() == crow::json::type::String ? std::string(v->s()) : std::string();
}

net::HttpReply send(const Service &service, const char *method, const std::string &path,
		const std::vector<std::string> &headers, const std::string &body = {}) {
	const std::string url = "http://127.0.0.1:" + std::to_string(service.http.bound_port()) + path;
	return net::http_exchange(
			method, url, headers, body, 5000, [](const std::string &host, net::Endpoint &out) {
				return net::resolve_ipv4(host, out, false);
			});
}

// POST /api/login, as the site sends it: the session cookie's value, "" on a failure.
std::string site_login(const Service &service, const std::string &username, const std::string &password) {
	const auto reply = send(service, "POST", "/api/login",
			{ "Content-Type: application/json", "X-OpenNova-Request: 1" },
			"{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}");
	if (!reply.transport_ok || reply.code != 200) return {};
	const std::string prefix = std::string(nws::kWebSessionCookie) + "=";
	for (const std::string &line : reply.headers) {
		const auto colon = line.find(':');
		if (colon == std::string::npos) continue;
		std::string name = line.substr(0, colon);
		for (char &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (name != "set-cookie") continue;
		const auto start = line.find(prefix, colon);
		if (start == std::string::npos) continue;
		const auto value = start + prefix.size();
		return line.substr(value, line.find(';', value) - value);
	}
	return {};
}

std::vector<std::string> as_owner(const std::string &session, bool post) {
	std::vector<std::string> headers = { "Cookie: " + std::string(nws::kWebSessionCookie) + "=" + session };
	if (post) {
		headers.push_back("Content-Type: application/json");
		headers.push_back("X-OpenNova-Request: 1");
	}
	return headers;
}

// Polls `done` every 100 ms for up to `seconds`; its last answer.
template <typename Done>
bool wait_for(Done done, int seconds) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (!done() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return done();
}

std::optional<hosted::HostedServer> server_row(Service &service, int64_t id) {
	return hosted::find_server(*service.pool.acquire(), id);
}

// The server's console and its events, for a failure's diagnosis.
void dump(Service &service, hosted::LocalProcessProvider &provider, int64_t id) {
	std::string console;
	std::string error;
	if (opennova::io::read_file_text(
				(provider.server_dir(id) / hosted::LocalProcessProvider::kConsoleLogName).u8string(), console,
				error)) {
		std::fprintf(stderr, "---- console.log (last 4000 bytes)\n%s\n----\n",
				console.substr(console.size() > 4000 ? console.size() - 4000 : 0).c_str());
	}
	for (const auto &e : hosted::list_events(*service.pool.acquire(), id, 50))
		std::fprintf(stderr, "  event %s -> %s: %s\n", e.from_state.c_str(), e.to_state.c_str(), e.detail.c_str());
}

// Whether `argv` holds `flag` followed by `value`.
bool has_pair(const std::vector<std::string> &argv, const std::string &flag, const std::string &value) {
	for (size_t i = 0; i + 1 < argv.size(); ++i) {
		if (argv[i] == flag && argv[i + 1] == value) return true;
	}
	return false;
}

void log_sink(opennova::io::LogLevel level, const char *message) {
	if (level != opennova::io::LogLevel::kDebug) std::printf("%s\n", message);
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (argc < 2) {
		std::fprintf(stderr, "usage: hosted_local_e2e_test <opennova-serve binary>\n");
		return 2;
	}
	const std::string serve_binary = argv[1];
	opennova::io::set_log_sink(&log_sink);
	if (net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	const fs::path source_dir{ OPENNOVA_SOURCE_DIR };
	{
		// Declared first, so the database's files and the working directories go last.
		test_temp::TempDir temp("hosted_local_e2e");
		// Two synthetic game directories, two data sets: each one deathmatch map
		// (named after its data set, so the listing tells which folder a server
		// mounted) and the game text.
		const auto write_game = [](const fs::path &dir, const char *map_file, const char *map_name) {
			fs::create_directories(dir);
			expect(serve_test::write_bytes(dir / map_file, serve_test::deathmatch_mission(map_name)), "a map");
			expect(serve_test::write_bytes(dir / "gametext.bin",
						   serve_test::gametext({
								   { "NovaWorld",
										   { { "STRNOVA07", "Jungle" }, { "STRNOVA08", "Desert" },
												   { "STRNOVA09", "Snow" }, { "STRNOVA10", "None" },
												   { "STRNOVA11", "Yes" }, { "STRNOVA12", "No" } } },
								   { "GateTypeAbbrev", { { "DM", "DM" } } },
						   })),
					"the game text");
		};
		const fs::path data = temp.path / "jo data";
		const fs::path dfx2_data = temp.path / "dfx2 data";
		write_game(data, "SERVETST.BMS", "Serve Test Map");
		write_game(dfx2_data, "DFXTEST.BMS", "DFX2 Test Map");
		// ONNET_HOSTED_DATA_SETS as a deployment writes it (the paths with
		// forward slashes, which JSON takes unescaped), read as main() reads it.
		const std::string data_sets_json = "{\"jop_2_consumer\":{\"resource_dir\":\"" + data.generic_u8string() +
				"\",\"loose_root\":true,\"missions\":[\"servetst.bms\"]},\"dfx2_consumer\":{\"resource_dir\":\"" +
				dfx2_data.generic_u8string() + "\",\"loose_root\":true,\"missions\":[\"dfxtest.bms\"]}}";
		std::vector<hosted::DataSetConfig> data_set_configs;
		std::string config_error;
		expect(hosted::parse_data_set_configs(data_sets_json, "", data_set_configs, config_error),
				"ONNET_HOSTED_DATA_SETS reads");
		const std::vector<hosted::DataSet> data_sets = hosted::offered_data_sets("local", data_set_configs);
		expect(data_sets.size() == 2, "the local provider offers both data sets");

		Service service;
		const bool up = service.start(source_dir, temp.path, /*require_host_key=*/false,
				[&data_sets_json](nws::ServerConfig &config) {
					config.hosted_provider = "local";
					config.hosted_cap = 5;
					config.hosted_data_sets = data_sets_json;
				});
		expect(up, "the service starts");
		expect(nws::set_comp_servers(*service.pool.acquire(), service.owner_id, 1).ok, "the owner is granted a server");

		hosted::LocalProcessConfig local;
		local.serve_binary = serve_binary;
		local.data_sets = data_sets;
		local.work_dir = (temp.path / "work").u8string();
		hosted::LocalProcessProvider provider(local);
		hosted::SupervisorConfig config;
		config.data_sets = data_sets;
		config.cap = 5;
		config.master_host = "127.0.0.1";
		config.master_gate_port = service.gate.bound_port();
		config.tick_interval_ms = 100;
		config.listing_timeout_ms = 60 * 1000;
		config.restart_limit = 0; // a crash fails it at once, with its reason
		hosted::Supervisor supervisor(service.pool, provider, config);
		// The admin links, their log lines kept for the secret check.
		std::mutex link_log_mu;
		std::vector<std::string> link_log;
		hosted::AdminLinkConfig link_config;
		link_config.status_interval_ms = 1000;
		link_config.log = [&](const std::string &line) {
			std::printf("%s\n", line.c_str());
			std::lock_guard<std::mutex> lock(link_log_mu);
			link_log.push_back(line);
		};
		hosted::AdminLinks links(&service.pool, link_config);
		supervisor.set_admin_links(&links);
		supervisor.set_stop_listing([&service](uint32_t rid, int64_t account) {
			return service.nwudp.push_stop_hosting(rid, opennova::SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT, 0,
						   0, account) == nws::HostPushResult::Queued;
		});
		if (up) supervisor.start();

		const std::string session = up ? site_login(service, kOwner, kOwnerPassword) : std::string();
		expect(!session.empty(), "the owner logs in to the site");

		// The start.
		int64_t id = 0;
		if (!session.empty()) {
			const auto reply = send(service, "POST", "/api/me/server/start", as_owner(session, true),
					"{\"game\":\"jop_2_consumer\",\"game_name\":\"Hosted E2E\",\"max_players\":8,"
					"\"missions\":[\"servetst.bms\"]}");
			const auto json = crow::json::load(body_text(reply));
			if (reply.code != 202) std::fprintf(stderr, "  start: %d %s\n", reply.code, body_text(reply).c_str());
			const auto started = reply.code == 202 ? integer_at(json, { "server", "id" }) : std::nullopt;
			expect(reply.transport_ok && started.has_value(), "POST /api/me/server/start is accepted");
			id = started.value_or(0);
		}

		bool running = false;
		if (id != 0) {
			running = wait_for([&] {
				const auto row = server_row(service, id);
				return row && (row->state == hosted::State::Running || !hosted::is_live(row->state));
			}, 90);
			const auto row = server_row(service, id);
			running = running && row && row->state == hosted::State::Running;
			if (!running) dump(service, provider, id);
			expect(running, "the hosted server is launched, lists with its service account and runs");
		}

		if (running) {
			const auto row = server_row(service, id);
			const auto account = row->service_user_id
					? nws::get_user_by_id(*service.pool.acquire(), *row->service_user_id)
					: std::nullopt;
			expect(account && account->kind == "service" && account->username == nws::service_username(id),
					"the server lists as its own service account");
			expect(fs::exists(provider.server_dir(id) / "server.host") &&
							fs::exists(provider.server_dir(id) / "credentials.txt"),
					"its working directory was rendered");
			// Joint Operations' data set: its folder and its title.
			expect(row->settings.game == "jop_2_consumer", "the row names its data set");
			const std::vector<std::string> command = provider.command_of(row->machine_id);
			expect(has_pair(command, "--resource-dir", fs::absolute(data).u8string()) &&
							has_pair(command, "/game", "jo"),
					"opennova-serve runs over Joint Operations' folder with /game jo");

			// The owner's view: the listing, as its hosted server's.
			auto reply = send(service, "GET", "/api/me/server", as_owner(session, false));
			auto json = crow::json::load(body_text(reply));
			const auto listed_rid = integer_at(json, { "server", "listing", "rid" });
			if (!listed_rid) std::fprintf(stderr, "  /api/me/server: %s\n", body_text(reply).c_str());
			expect(listed_rid.has_value(), "GET /api/me/server names the listing");
			const uint32_t rid = static_cast<uint32_t>(listed_rid.value_or(0));
			reply = send(service, "GET", "/api/me/hosts", as_owner(session, false));
			json = crow::json::load(body_text(reply));
			const auto *hosts = member(json, { "hosts" });
			bool shown = false;
			if (hosts && hosts->t() == crow::json::type::List && hosts->size() == 1) {
				const auto &first = (*hosts)[0];
				shown = integer_at(first, { "rid" }) == static_cast<int64_t>(rid) &&
						integer_at(first, { "hosted_server_id" }) == id;
			}
			if (!shown) std::fprintf(stderr, "  /api/me/hosts: %s\n", body_text(reply).c_str());
			expect(shown, "GET /api/me/hosts shows the listing as the owner's hosted server");

			// The owner's ServerCommand reaches the server, which saves game.cfg.
			reply = send(service, "POST", "/api/me/hosts/" + std::to_string(rid) + "/command",
					as_owner(session, true), "{\"verb\":\"SetServerName\",\"args\":[\"Renamed Hosted\"]}");
			if (reply.code != 202) std::fprintf(stderr, "  command: %d %s\n", reply.code, body_text(reply).c_str());
			expect(reply.transport_ok && reply.code == 202, "the owner's SetServerName is queued");
			const std::string cfg_path = (provider.server_dir(id) / "game.cfg").u8string();
			const bool renamed = wait_for([&] {
				const auto cfg = opennova::gamecfg::load_file(cfg_path, {});
				return cfg.file_read && cfg.cfg.game_name == "Renamed Hosted";
			}, 20);
			if (!renamed) dump(service, provider, id);
			expect(renamed, "the server takes the owner's name and saves game.cfg");

			// The admin link: up from running, logged in with the rendered login.
			const auto login = hosted::admin_login_of(*service.pool.acquire(), id);
			expect(login && login->user == "svc" && login->password.size() == hosted::kAdminPasswordChars,
					"the start rendered the admin login");
			std::shared_ptr<hosted::AdminLink> link;
			const bool link_up = wait_for([&] {
				link = links.find(id);
				return link && link->status().state == hosted::AdminLinkState::Up;
			}, 30);
			if (!link_up) dump(service, provider, id);
			expect(link_up, "the admin link logs in on the running server");
			if (link_up) {
				hosted::AdminCommandOptions read;
				read.actor = "owner";
				read.record = false;
				const auto players = link->run("PLAYER LIST", read, std::chrono::milliseconds(3000));
				expect(players.outcome == hosted::AdminCommandOutcome::Answered && players.replies.size() == 1 &&
								players.replies[0].rfind("NAME            \t #\tTEAM", 0) == 0,
						"PLAYER LIST answers on the link");
				hosted::AdminCommandOptions say;
				say.actor = "owner";
				const auto chat = link->run("CHAT SEND hello from the service", say, std::chrono::milliseconds(3000));
				expect(chat.outcome == hosted::AdminCommandOutcome::Answered &&
								chat.replies == std::vector<std::string>{ "OK - Chat sent." },
						"CHAT SEND answers on the link");
				const bool recorded = wait_for([&] {
					bool up_line = false;
					bool command = false;
					bool reply = false;
					for (const auto &line : hosted::list_console(*service.pool.acquire(), id, 0, 100)) {
						up_line = up_line || (line.kind == "status" && line.text == "the admin session is up");
						command = command ||
								(line.kind == "command" && line.actor == "owner" &&
										line.text == "CHAT SEND hello from the service");
						reply = reply || (line.kind == "reply" && line.text == "OK - Chat sent.");
					}
					return up_line && command && reply;
				}, 10);
				expect(recorded, "the console ring holds the session, the command and its reply");
			}
			link.reset();

			// The stop.
			reply = send(service, "POST", "/api/me/server/stop", as_owner(session, true));
			json = crow::json::load(body_text(reply));
			const std::string stop_state = text_at(json, { "server", "state" });
			if (reply.code != 202) std::fprintf(stderr, "  stop: %d %s\n", reply.code, body_text(reply).c_str());
			expect(reply.code == 202 && (stop_state == "stopping" || stop_state == "stopped"),
					"POST /api/me/server/stop is accepted");
			const bool stopped = wait_for([&] {
				const auto r = server_row(service, id);
				return r && !hosted::is_live(r->state);
			}, 60);
			const auto ended = server_row(service, id);
			if (!stopped || !ended || ended->state != hosted::State::Stopped) dump(service, provider, id);
			expect(stopped && ended && ended->state == hosted::State::Stopped, "the server ends stopped");
			expect(!opennova::hostdb::find_host_by_rid(*service.pool.acquire(), rid),
					"its listing has left the browser");
			expect(provider.list_tagged().empty(), "its process is gone");
			// Deleted with the transition that ended the row; polled all the same.
			expect(account && wait_for([&] {
				const auto r = server_row(service, id);
				return r && !r->service_user_id && !nws::get_user_by_id(*service.pool.acquire(), account->id);
			}, 10),
					"its service account is deleted");
			bool pushed_stop = false;
			for (const auto &e : hosted::list_events(*service.pool.acquire(), id, 50)) {
				if (e.from_state == "running" && e.to_state == "stopping") pushed_stop = true;
			}
			expect(pushed_stop, "the timeline runs through running -> stopping");
			expect(!links.find(id), "the admin link went down at stopping");
			// Its QUIT and the server's own exit on the ServerStopHosting race, so either
			// line ends the session (hosted_admin_link pins the QUIT).
			const bool session_ended = wait_for([&] {
				for (const auto &line : hosted::list_console(*service.pool.acquire(), id, 0, 100)) {
					if (line.kind == "status" &&
							(line.text == "the admin session is closed" ||
									line.text.rfind("the admin session was lost", 0) == 0))
						return true;
				}
				return false;
			}, 10);
			expect(!link_up || session_ended, "the admin link's session ended with the server's run");
			if (login) {
				for (const auto &line : hosted::list_console(*service.pool.acquire(), id, 0, hosted::kConsoleKeep))
					expect(line.text.find(login->password) == std::string::npos, "no console line has the password");
				std::lock_guard<std::mutex> lock(link_log_mu);
				for (const std::string &line : link_log)
					expect(line.find(login->password) == std::string::npos, "no link log line has the password");
			}
			std::printf("hosted_local_e2e: server %lld ran as RID %u, took its owner's name and stopped\n",
					static_cast<long long>(id), rid);
		}

		// A second owner's server on Delta Force Xtreme 2's data set: its own
		// folder (whose map alone it can start on), /game dfx2, and that data
		// set's default rotation (the start names none).
		int64_t second_owner = 0;
		if (up) {
			nws::CreateUserParams p;
			p.username = "owner2";
			p.password = "pw-owner2";
			p.pcid = "0e2e0002";
			p.nwh = "1";
			p.nwhandle = "Owner2Handle";
			const auto created = nws::create_user(*service.pool.acquire(), p);
			second_owner = created.ok ? created.id : 0;
			expect(second_owner > 0 && nws::set_comp_servers(*service.pool.acquire(), second_owner, 1).ok,
					"a second owner is granted a server");
		}
		const std::string second = second_owner > 0 ? site_login(service, "owner2", "pw-owner2") : std::string();
		expect(!up || !second.empty(), "the second owner logs in to the site");
		int64_t dfx2_id = 0;
		if (!second.empty()) {
			const auto reply = send(service, "POST", "/api/me/server/start", as_owner(second, true),
					"{\"game\":\"dfx2_consumer\",\"game_name\":\"Hosted E2E DFX2\",\"max_players\":8}");
			if (reply.code != 202) std::fprintf(stderr, "  dfx2 start: %d %s\n", reply.code, body_text(reply).c_str());
			dfx2_id = reply.code == 202 ? integer_at(crow::json::load(body_text(reply)), { "server", "id" }).value_or(0)
										: 0;
			expect(dfx2_id != 0, "POST /api/me/server/start is accepted for Delta Force Xtreme 2");
		}
		if (dfx2_id != 0) {
			const bool dfx2_running = wait_for([&] {
				const auto row = server_row(service, dfx2_id);
				return row && (row->state == hosted::State::Running || !hosted::is_live(row->state));
			}, 90) && server_row(service, dfx2_id)->state == hosted::State::Running;
			if (!dfx2_running) dump(service, provider, dfx2_id);
			expect(dfx2_running, "the Delta Force Xtreme 2 server is launched, lists and runs");
			const auto row = server_row(service, dfx2_id);
			if (dfx2_running && row) {
				expect(row->settings.game == "dfx2_consumer" &&
								row->settings.missions == std::vector<std::string>{ "dfxtest.bms" },
						"its row names its data set and that data set's rotation");
				const std::vector<std::string> command = provider.command_of(row->machine_id);
				expect(has_pair(command, "--resource-dir", fs::absolute(dfx2_data).u8string()) &&
								has_pair(command, "/game", "dfx2"),
						"opennova-serve runs over Delta Force Xtreme 2's folder with /game dfx2");
				const auto reply = send(service, "GET", "/api/me/server", as_owner(second, false));
				const auto json = crow::json::load(body_text(reply));
				expect(text_at(json, { "server", "settings", "game" }) == "dfx2_consumer" &&
								text_at(json, { "server", "game_display_name" }) == "Delta Force Xtreme 2",
						"GET /api/me/server names the game");
			}
			const auto reply = send(service, "POST", "/api/me/server/stop", as_owner(second, true));
			expect(reply.code == 202, "the second owner's stop is accepted");
			const bool stopped = wait_for([&] {
				const auto r = server_row(service, dfx2_id);
				return r && !hosted::is_live(r->state);
			}, 60);
			const auto ended = server_row(service, dfx2_id);
			if (!stopped || !ended || ended->state != hosted::State::Stopped) dump(service, provider, dfx2_id);
			expect(stopped && ended && ended->state == hosted::State::Stopped, "the Delta Force Xtreme 2 server stops");
			expect(provider.list_tagged().empty(), "every process is gone");
		}

		supervisor.stop();
		links.stop_all();
		service.stop();
	}
	net::shutdown();
	opennova::io::set_log_sink(nullptr);
	if (g_failures == 0) {
		std::printf("OK: a hosted server starts, lists, takes its owner's command and stops\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
