// The owner's pushes end to end: an in-process opennova-serve (serve::Server on a loose game
// directory, the opennova_serve_server_command_push setup) hosting with an account (its
// --credentials) on an in-process NovaWorld service (nw_service_harness.h: the HTTP, NW UDP and
// gate listeners over one pooled SQLite file), administered by that account from the website's
// own routes. Built only with BUILD_NOVAWORLD_HTTP.
//
//   * the server logs in, fetches its HOSTKEY from /NWHost.dll, hosts, and the update carrying
//     the key lists it as the account's (active_hosts.host_user_id);
//   * the account logs in to the website (POST /api/login) and GET /api/me/hosts names the
//     server with its roster;
//   * another account's POST /api/me/hosts/<rid>/command is a 404 and the server runs nothing;
//   * the owner's SetServerName renames the live match and saves game.cfg, as retail's handler
//     does [orig: CNapiGameSession_HandleServerCommand — the block copy @0x4D2CFC,
//     Game_SaveConfig @0x4D2DDF];
//   * the owner's TextChatServer runs on the match (the executor's channel-10 broadcast
//     [orig: @0x4D25A5..0x4D25E6]);
//   * the owner's POST /api/me/hosts/<rid>/stop ends the listing: the server's lister stops on the
//     service's word and the row leaves the browser.

#include "nw_service_harness.h"

#include "serve/serve_test_support.h"
#include "server.h"
#include "web_session.h"

#include "lister.h"
#include "net_http.h"
#include "net_sockets.h"

#include <base/io/log.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/novaworld/host_repository.h>

#include <crow/json.h>

#include "common/temp_dir.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
using nw_service_test::kOwner;
using nw_service_test::kOwnerPassword;
using nw_service_test::Service;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

constexpr const char *kServerName = "Owner Admin";
constexpr const char *kStranger = "stranger";
constexpr const char *kStrangerPassword = "pw-stranger";

// Every engine diagnostic is printed (beside the listeners' own printf lines) and kept, so the
// test can see what the server ran: opennova-serve logs "[host] ServerCommand <verb>" for each
// command its match executed.
std::mutex g_log_mu;
std::vector<std::string> g_log;

void log_sink(opennova::io::LogLevel level, const char *message) {
	if (level == opennova::io::LogLevel::kDebug)
		return;
	std::printf("%s\n", message);
	std::lock_guard<std::mutex> lock(g_log_mu);
	g_log.emplace_back(message);
}

int count_logged(const std::string &line) {
	std::lock_guard<std::mutex> lock(g_log_mu);
	return static_cast<int>(std::count(g_log.begin(), g_log.end(), line));
}

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

// One request to the service's HTTP listener.
net::HttpReply send(const Service &service, const char *method, const std::string &path,
		const std::vector<std::string> &headers, const std::string &body = {}) {
	const std::string url = "http://127.0.0.1:" + std::to_string(service.http.bound_port()) + path;
	return net::http_exchange(
			method, url, headers, body, 3000, [](const std::string &host, net::Endpoint &out) {
				return net::resolve_ipv4(host, out, false);
			});
}

// POST /api/login (the site's own request, its CSRF header included): the session cookie's
// value, "" when the login failed.
std::string site_login(
		const Service &service, const std::string &username, const std::string &password) {
	const auto reply = send(service, "POST", "/api/login",
			{ "Content-Type: application/json", "X-OpenNova-Request: 1" },
			"{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}");
	if (!reply.transport_ok || reply.code != 200)
		return {};
	const std::string prefix = std::string(nws::kWebSessionCookie) + "=";
	for (const std::string &line : reply.headers) {
		const auto colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		std::string name = line.substr(0, colon);
		for (char &c : name)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (name != "set-cookie")
			continue;
		const auto start = line.find(prefix, colon);
		if (start == std::string::npos)
			continue;
		const auto value = start + prefix.size();
		return line.substr(value, line.find(';', value) - value);
	}
	return {};
}

// A push through the owner's route, as the site's page sends it.
net::HttpReply owner_push(const Service &service, const std::string &session, uint32_t rid,
		const char *what, const std::string &body = {}) {
	return send(service, "POST", "/api/me/hosts/" + std::to_string(rid) + "/" + what,
			{ "Content-Type: application/json", "X-OpenNova-Request: 1",
					"Cookie: " + std::string(nws::kWebSessionCookie) + "=" + session },
			body);
}

} // namespace

int main() {
	opennova::io::set_log_sink(&log_sink);
	if (net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	const fs::path source_dir{ OPENNOVA_SOURCE_DIR };
	{
		// Declared first, so the database's files go only once every connection is closed.
		test_temp::TempDir temp("nw_owner_admin");
		Service service(temp.path / "owner_admin.db");
		const bool up = service.start(source_dir, temp.path, /*require_host_key=*/false);
		expect(up, "the service starts");
		{
			nws::CreateUserParams p;
			p.username = kStranger;
			p.password = kStrangerPassword;
			p.pcid = "0e2e0002";
			p.nwh = "1";
			p.nwhandle = "StrangerHandle";
			expect(nws::create_user(*service.pool.acquire(), p).ok, "a second account");
		}

		// The server: a loose game directory with one deathmatch map, its working directory
		// its own (game.cfg lands there), listed on the service with the owner's account.
		const fs::path dir = serve_test::fresh_dir("owner_admin");
		const fs::path work = serve_test::fresh_dir("owner_admin_cwd");
		serve_test::ScopedCwd cwd(work);
		expect(serve_test::write_bytes(dir / "SERVETST.BMS", serve_test::deathmatch_mission()),
				"the map");
		expect(serve_test::write_bytes(dir / "gametext.bin",
					   serve_test::gametext({
							   { "NovaWorld",
									   { { "STRNOVA07", "Jungle" }, { "STRNOVA08", "Desert" },
											   { "STRNOVA09", "Snow" }, { "STRNOVA10", "None" },
											   { "STRNOVA11", "Yes" }, { "STRNOVA12", "No" } } },
							   { "GateTypeAbbrev", { { "DM", "DM" } } },
					   })),
				"the game text");
		{
			std::ofstream host(dir / "test.host", std::ios::binary);
			host << "GameName \"" << kServerName << "\"\r\n"
				 << "ServerMessage \"administered by its owner\"\r\n"
				 << "MaxPlayers 8\r\n"
				 << "Mission servetst.bms\r\n";
		}
		std::unique_ptr<opennova::serve::Server> holder;
		if (up) {
			std::string error;
			opennova::serve::ServeOptions options;
			expect(opennova::serve::parse_serve_options(
						   { "--resource-dir", dir.string(), "/HOST", (dir / "test.host").string(),
								   "--loose-root", "--lan-port", "0", "--master-host", "127.0.0.1",
								   "--master-gate-port",
								   std::to_string(service.gate.bound_port()) },
						   options, error) == 0,
					"the serve options parse");
			// What main() loads from --credentials.
			options.credentials.user = kOwner;
			options.credentials.pass = kOwnerPassword;
			holder = std::make_unique<opennova::serve::Server>(options);
			if (!holder->start(error)) {
				std::fprintf(stderr, "  start: %s\n", error.c_str());
				holder.reset();
			}
		}
		expect(holder != nullptr, "opennova-serve starts and lists with the account");

		if (holder) {
			opennova::serve::Server &server = *holder;
			constexpr double kFrame = 1.0 / 62.5;
			// Frames until `done` within a wall-clock bound, a short sleep every few frames so the
			// service's threads and the loopback sockets keep up; false on the bound or once the
			// server stops.
			auto frame_until = [&](auto done, std::chrono::seconds bound) {
				const auto deadline = std::chrono::steady_clock::now() + bound;
				for (unsigned f = 1; !done() && std::chrono::steady_clock::now() < deadline; ++f) {
					if (!server.frame(kFrame))
						break;
					if (f % 4 == 0)
						std::this_thread::sleep_for(1ms);
				}
				return done();
			};
			std::optional<opennova::hostdb::HostRow> row;
			const bool owned = frame_until(
					[&] {
						row = service.row(kServerName);
						return row && row->host_user_id == service.owner_id;
					},
					20s);
			expect(owned, "the server's key lists it as the account's");
			const uint32_t rid = row ? row->rid : 0;

			const std::string owner_session = site_login(service, kOwner, kOwnerPassword);
			const std::string stranger_session = site_login(service, kStranger, kStrangerPassword);
			expect(!owner_session.empty() && !stranger_session.empty(),
					"both accounts log in to the site");

			// The owner's list names the server, with its roster.
			auto reply = send(service, "GET", "/api/me/hosts",
					{ "Cookie: " + std::string(nws::kWebSessionCookie) + "=" + owner_session });
			const auto mine = crow::json::load(body_text(reply));
			expect(reply.code == 200 && mine && mine["hosts"].size() == 1 &&
							mine["hosts"][0]["rid"].i() == static_cast<int64_t>(rid) &&
							mine["hosts"][0]["roster"].t() == crow::json::type::List,
					"GET /api/me/hosts names the owner's server with its roster");

			// Another account: 404, and the server runs nothing.
			const std::string rename = "{\"verb\":\"SetServerName\",\"args\":[\"Owner Named\"]}";
			reply = owner_push(service, stranger_session, rid, "command", rename);
			expect(reply.transport_ok && reply.code == 404, "another account's push is a 404");
			reply = owner_push(service, stranger_session, rid, "stop");
			expect(reply.transport_ok && reply.code == 404, "another account's stop is a 404");
			frame_until([] { return false; }, 1s);
			expect(server.role().state.host_owner.ctx.config.server_name == kServerName,
					"the server ran nothing for another account");
			expect(count_logged("[host] ServerCommand SetServerName") == 0,
					"no command was logged");

			// SetServerName: the live match takes the name and game.cfg is saved.
			reply = owner_push(service, owner_session, rid, "command", rename);
			if (reply.code != 202)
				std::fprintf(stderr, "  rename: %d %s\n", reply.code, body_text(reply).c_str());
			expect(reply.transport_ok && reply.code == 202, "the owner's rename is queued");
			const opennova::inmatch::NapiNPServerCtx &ctx = server.role().state.host_owner.ctx;
			expect(frame_until([&] { return ctx.config.server_name == "Owner Named"; }, 10s),
					"the owner's SetServerName renames the live match");
			expect(server.game_cfg().game_name == "Owner Named",
					"the server's config block takes the name");
			const opennova::gamecfg::LoadResult saved =
					opennova::gamecfg::load_file(opennova::gamecfg::kFileName, {});
			expect(saved.file_read && saved.cfg.game_name == "Owner Named",
					"game.cfg is saved with the name");

			// TextChatServer: the match runs it (a channel-10 line to every player in the game).
			reply = owner_push(service, owner_session, rid, "command",
					"{\"verb\":\"TextChatServer\",\"args\":[\"hello from the site\"]}");
			expect(reply.transport_ok && reply.code == 202, "the owner's chat line is queued");
			expect(frame_until(
						   [] { return count_logged("[host] ServerCommand TextChatServer") == 1; },
						   10s),
					"the server's match executes the owner's TextChatServer");

			// The stop: the server's lister stops on the service's word; the row leaves the
			// browser.
			reply = owner_push(service, owner_session, rid, "stop");
			expect(reply.transport_ok && reply.code == 202, "the owner's stop is queued");
			int lister_exit = -1;
			// The lister can finish in the very frame whose mission exit stops the server, which
			// tears the lister down; the server keeps the code it finished with.
			frame_until(
					[&] {
						if (server.lister() != nullptr && server.lister()->finished())
							lister_exit = server.lister()->exit_code();
						else if (server.lister() == nullptr)
							lister_exit = server.last_lister_exit();
						return lister_exit != -1;
					},
					10s);
			expect(lister_exit == opennova::nw_lister::kExitStoppedByService,
					"the server's lister stops on the service's word");
			expect(nw_service_test::wait_for([&] {
				return !opennova::hostdb::find_host_by_rid(*service.pool.acquire(), rid);
			}),
					"the stopped server leaves the browser");
			reply = owner_push(service, owner_session, rid, "command", rename);
			expect(reply.transport_ok && reply.code == 404,
					"a stopped server's RID is no longer the owner's");
			std::printf("nw_owner_admin_e2e: rid %u renamed, chatted to and stopped by its owner\n",
					rid);
		}

		holder.reset();
		cwd.restore();
		std::error_code ec;
		fs::remove_all(dir, ec);
		fs::remove_all(work, ec);
		service.stop();
	}
	net::shutdown();
	opennova::io::set_log_sink(nullptr);
	if (g_failures == 0) {
		std::printf("OK: a listed server's owner administers it from the website\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
