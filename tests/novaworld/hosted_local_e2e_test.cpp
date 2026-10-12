// A hosted server end to end with LocalProcessProvider: an in-process NovaWorld
// service (nw_service_harness.h, hosted servers on) and its supervisor run a real
// opennova-serve child (ctest passes the binary) over a synthetic loose game
// directory (serve_test_support.h's deathmatch map and game text, as the serve
// tests make them). Built only with BUILD_NOVAWORLD_HTTP.
//
//   * the owner, approved (players.can_host), starts it with POST
//     /api/me/server/start;
//   * the supervisor mints its service account srv-<id>, renders its working
//     directory and launches opennova-serve there, which logs in as that
//     account through /NWLogin.dll and /NWHost.dll, hosts and lists; the server
//     reaches running;
//   * GET /api/me/hosts shows the listing as the owner's hosted server (the
//     service account's claim resolved to its owner);
//   * the owner's SetServerName (POST /api/me/hosts/<rid>/command) reaches the
//     server, which saves game.cfg in its working directory with the name;
//   * POST /api/me/server/stop: the supervisor pushes ServerStopHosting, the
//     listing leaves the browser, the process is gone, the row is stopped and
//     the service account deleted.

#include "nw_service_harness.h"

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

void log_sink(opennova::io::LogLevel level, const char *message) {
	if (level != opennova::io::LogLevel::kDebug) std::printf("%s\n", message);
}

} // namespace

int main(int argc, char **argv) {
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
		// The synthetic game directory: one deathmatch map and the game text.
		const fs::path data = temp.path / "data";
		fs::create_directories(data);
		expect(serve_test::write_bytes(data / "SERVETST.BMS", serve_test::deathmatch_mission()), "the map");
		expect(serve_test::write_bytes(data / "gametext.bin",
					   serve_test::gametext({
							   { "NovaWorld",
									   { { "STRNOVA07", "Jungle" }, { "STRNOVA08", "Desert" },
											   { "STRNOVA09", "Snow" }, { "STRNOVA10", "None" },
											   { "STRNOVA11", "Yes" }, { "STRNOVA12", "No" } } },
							   { "GateTypeAbbrev", { { "DM", "DM" } } },
					   })),
				"the game text");

		Service service(temp.path / "hosted.db");
		const bool up = service.start(source_dir, temp.path, /*require_host_key=*/true,
				[](nws::ServerConfig &config) {
					config.hosted_provider = "local";
					config.hosted_cap = 5;
				});
		expect(up, "the service starts");
		expect(nws::set_can_host(*service.pool.acquire(), service.owner_id, true).ok, "the owner is approved");

		hosted::LocalProcessConfig local;
		local.serve_binary = serve_binary;
		local.resource_dir = data.u8string();
		local.loose_root = true;
		local.work_dir = (temp.path / "work").u8string();
		hosted::LocalProcessProvider provider(local);
		hosted::SupervisorConfig config;
		config.cap = 5;
		config.master_host = "127.0.0.1";
		config.master_gate_port = service.gate.bound_port();
		config.tick_interval_ms = 100;
		config.listing_timeout_ms = 60 * 1000;
		config.restart_limit = 0; // a crash fails it at once, with its reason
		hosted::Supervisor supervisor(service.pool, provider, config);
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
					"{\"game_name\":\"Hosted E2E\",\"max_players\":8,\"missions\":[\"servetst.bms\"]}");
			const auto json = crow::json::load(body_text(reply));
			if (reply.code != 202) std::fprintf(stderr, "  start: %d %s\n", reply.code, body_text(reply).c_str());
			expect(reply.transport_ok && reply.code == 202 && json, "POST /api/me/server/start is accepted");
			if (json) id = json["server"]["id"].i();
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

			// The owner's view: the listing, as its hosted server's.
			auto reply = send(service, "GET", "/api/me/server", as_owner(session, false));
			auto json = crow::json::load(body_text(reply));
			const bool listed = json && json["server"]["listing"].t() == crow::json::type::Object;
			expect(listed, "GET /api/me/server names the listing");
			const uint32_t rid = listed ? static_cast<uint32_t>(json["server"]["listing"]["rid"].i()) : 0;
			reply = send(service, "GET", "/api/me/hosts", as_owner(session, false));
			json = crow::json::load(body_text(reply));
			expect(json && json["hosts"].size() == 1 &&
							json["hosts"][0]["rid"].i() == static_cast<int64_t>(rid) &&
							json["hosts"][0]["hosted_server_id"].i() == id,
					"GET /api/me/hosts shows the listing as the owner's hosted server");

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

			// The stop.
			reply = send(service, "POST", "/api/me/server/stop", as_owner(session, true));
			json = crow::json::load(body_text(reply));
			expect(reply.code == 202 && json && std::string(json["server"]["state"].s()) == "stopping",
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
			expect(ended && !ended->service_user_id && account &&
							!nws::get_user_by_id(*service.pool.acquire(), account->id),
					"its service account is deleted");
			bool pushed_stop = false;
			for (const auto &e : hosted::list_events(*service.pool.acquire(), id, 50)) {
				if (e.from_state == "running" && e.to_state == "stopping") pushed_stop = true;
			}
			expect(pushed_stop, "the timeline runs through running -> stopping");
			std::printf("hosted_local_e2e: server %lld ran as RID %u, took its owner's name and stopped\n",
					static_cast<long long>(id), rid);
		}

		supervisor.stop();
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
