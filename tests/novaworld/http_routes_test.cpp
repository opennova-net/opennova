// The NovaWorld service's HTTP routes, driven in-process: a SQLite file in the
// run's temp directory under backend/migrations (plus the games seed), the
// Crow HttpListener from opennova_novaworld_server_core on a free loopback
// port, and requests sent through apps/common's http_exchange. Built only
// with BUILD_NOVAWORLD_HTTP. The cases share one listener and run in order
// (the admin users case reads the account the register case made).
//
// The listener and the test share one db::ConnectionPool, the way main() wires
// the server: each Crow handler leases a connection for its request, and the
// test thread leases its own for every direct read or write. A file rather
// than a shared-cache in-memory URI: Crow runs handlers on several threads at
// once, and shared cache's SQLITE_LOCKED skips the busy timeout a WAL file's
// writers wait on.

#include "auth.h"
#include "http_listener.h"
#include "net_http.h"
#include "net_sockets.h"
#include "server_config.h"
#include "session_store.h"

#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/host_repository.h>

#include <crow/json.h>

#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
using opennova::db::ConnectionPool;

namespace {

constexpr const char *kAdminToken = "harness-admin-token";
constexpr const char *kBearer = "Authorization: Bearer harness-admin-token";
constexpr const char *kJson = "Content-Type: application/json";

struct Harness {
	ConnectionPool &pool;
	uint16_t port = 0;
	std::string registered_pcid; // what the register case's reply carried

	net::HttpReply send(const char *method, const std::string &path,
	                    const std::vector<std::string> &headers = {},
	                    const std::string &body = {}) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
		return net::http_exchange(method, url, headers, body, 3000,
		                          [](const std::string &host, net::Endpoint &out) {
			                          return net::resolve_ipv4(host, out, false);
		                          });
	}
};

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

std::string str(const crow::json::rvalue &v) { return std::string(v.s()); }

bool has_header(const net::HttpReply &reply, const std::string &name) {
	for (const std::string &line : reply.headers) {
		if (line.size() <= name.size() || line[name.size()] != ':') continue;
		if (std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) {
			    return std::tolower(static_cast<unsigned char>(a)) ==
			           std::tolower(static_cast<unsigned char>(b));
		    })) {
			return true;
		}
	}
	return false;
}

bool is_lower_hex8(const std::string &s) {
	return s.size() == 8 && std::all_of(s.begin(), s.end(), [](char c) {
		       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	       });
}

// GET /api/health, the readiness probe: the listener binds on its own thread
// after start() returns, so this polls until a reply comes back.
int test_health(Harness &h) {
	net::HttpReply reply;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;) {
		reply = h.send("GET", "/api/health");
		if (reply.transport_ok || std::chrono::steady_clock::now() >= deadline) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (!reply.transport_ok) {
		std::fprintf(stderr, "  /api/health never answered on :%u: %s\n",
		             static_cast<unsigned>(h.port), reply.error.c_str());
	}
	TEST_EXPECT(reply.transport_ok);
	TEST_EXPECT(reply.code == 200);
	const auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json.has("status"));
	TEST_EXPECT(str(json["status"]) == "ok");
	return 0;
}

// GET /api/lobbies: the seeded games in slug order, then a host row written
// straight into active_hosts shows up under its game in camelCase.
int test_lobbies_lists_seeded_games(Harness &h) {
	auto reply = h.send("GET", "/api/lobbies");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json.has("games"));
	TEST_EXPECT(json["games"].size() == 2);
	TEST_EXPECT(str(json["games"][0]["slug"]) == "dfx2_consumer");
	TEST_EXPECT(!str(json["games"][0]["displayName"]).empty());
	TEST_EXPECT(str(json["games"][1]["slug"]) == "jop_2_consumer");
	TEST_EXPECT(json["games"][0]["hosts"].size() == 0);
	TEST_EXPECT(json["games"][1]["hosts"].size() == 0);

	opennova::hostdb::HostRow row;
	row.rid = 1000;
	row.gsid = "GSID-HARNESS";
	row.game = "jop_2_consumer";
	row.server_name = "Harness Srv";
	row.host_ip = "10.0.0.7";
	row.host_port = 32768;
	row.player_count = 2;
	row.max_players = 32;
	row.region = "us";
	{
		auto conn = h.pool.acquire();
		opennova::hostdb::upsert_host(*conn, row);
	}

	reply = h.send("GET", "/api/lobbies");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["games"].size() == 2);
	TEST_EXPECT(json["games"][0]["hosts"].size() == 0);
	const auto &hosts = json["games"][1]["hosts"];
	TEST_EXPECT(hosts.size() == 1);
	TEST_EXPECT(hosts[0]["id"].i() == 1000);
	TEST_EXPECT(str(hosts[0]["serverName"]) == "Harness Srv");
	TEST_EXPECT(str(hosts[0]["hostIp"]) == "10.0.0.7");
	TEST_EXPECT(hosts[0]["hostPort"].i() == 32768);
	TEST_EXPECT(hosts[0]["players"].i() == 2);
	TEST_EXPECT(hosts[0]["maxPlayers"].i() == 32);
	TEST_EXPECT(str(hosts[0]["region"]) == "us");
	return 0;
}

// POST /api/register: 201 with the public record (a fresh 8-hex PCID, never
// the hash), and the account authenticates against its bcrypt hash.
int test_register_ok(Harness &h) {
	const auto reply = h.send("POST", "/api/register", {kJson},
	                          "{\"username\":\"harness\",\"password\":\"pw-harness\","
	                          "\"nwhandle\":\"HarnessPlayer\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 201);
	const std::string text = body_text(reply);
	TEST_EXPECT(text.find("password_hash") == std::string::npos);
	const auto json = crow::json::load(text);
	TEST_EXPECT(json && json.has("user"));
	const auto &user = json["user"];
	TEST_EXPECT(str(user["username"]) == "harness");
	TEST_EXPECT(str(user["nwhandle"]) == "HarnessPlayer");
	TEST_EXPECT(str(user["nwh"]) == "1");
	TEST_EXPECT(str(user["account_status"]) == "active");
	TEST_EXPECT(is_lower_hex8(str(user["pcid"])));
	TEST_EXPECT(user["id"].i() > 0);
	h.registered_pcid = str(user["pcid"]);

	auto conn = h.pool.acquire();
	const auto stored = nws::get_user_by_username(*conn, "harness");
	TEST_EXPECT(stored.has_value());
	TEST_EXPECT(stored->id == user["id"].i());
	TEST_EXPECT(stored->pcid == h.registered_pcid);
	TEST_EXPECT(nws::authenticate_user(*conn, "harness", "pw-harness").has_value());
	TEST_EXPECT(!nws::authenticate_user(*conn, "harness", "wrong").has_value());
	return 0;
}

// POST /api/register refusals: a taken username, a missing password, a body
// that is not JSON. None of them adds an account.
int test_register_duplicate(Harness &h) {
	auto reply = h.send("POST", "/api/register", {kJson},
	                    "{\"username\":\"harness\",\"password\":\"pw-harness\","
	                    "\"nwhandle\":\"HarnessPlayer\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["error"]) == "username_exists");

	reply = h.send("POST", "/api/register", {kJson}, "{\"username\":\"nopw\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["error"]) == "missing_field");

	reply = h.send("POST", "/api/register", {kJson}, "not json");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["error"]) == "invalid_json");

	TEST_EXPECT(nws::list_users(*h.pool.acquire()).size() == 1);
	return 0;
}

// /api/admin/server-status: 401 (with the Bearer challenge) without the token
// or with a wrong one; with it, the migration's row, then a PUT that sticks.
int test_admin_server_status_requires_token(Harness &h) {
	auto reply = h.send("GET", "/api/admin/server-status");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(body_text(reply) == "unauthorized");
	TEST_EXPECT(has_header(reply, "WWW-Authenticate"));

	reply = h.send("GET", "/api/admin/server-status", {"Authorization: Bearer wrong-token"});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);

	reply = h.send("GET", "/api/admin/server-status", {kBearer});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json.has("maintenance_enabled"));
	TEST_EXPECT(!json["maintenance_enabled"].b());
	// 0003_novaworld_status_and_gsb.sql seeds the row with this message.
	TEST_EXPECT(str(json["message"]) == "NovaWorld is temporarily unavailable.");

	reply = h.send("PUT", "/api/admin/server-status", {kBearer, kJson},
	               "{\"maintenance_enabled\":true,\"message\":\"back soon\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["maintenance_enabled"].b());
	TEST_EXPECT(str(json["message"]) == "back soon");

	reply = h.send("GET", "/api/admin/server-status", {kBearer});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["maintenance_enabled"].b());
	TEST_EXPECT(str(json["message"]) == "back soon");
	return 0;
}

// GET /api/admin/users: token-gated; lists the registered account without
// its password hash.
int test_admin_users_list(Harness &h) {
	auto reply = h.send("GET", "/api/admin/users");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);

	reply = h.send("GET", "/api/admin/users", {kBearer});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const std::string text = body_text(reply);
	TEST_EXPECT(text.find("password_hash") == std::string::npos);
	const auto json = crow::json::load(text);
	TEST_EXPECT(json && json.has("users"));
	TEST_EXPECT(json["users"].size() == 1);
	TEST_EXPECT(str(json["users"][0]["username"]) == "harness");
	TEST_EXPECT(str(json["users"][0]["nwhandle"]) == "HarnessPlayer");
	TEST_EXPECT(str(json["users"][0]["pcid"]) == h.registered_pcid);
	return 0;
}

// The static/catch-all family answers a path no root holds with 404.
int test_unknown_path_404(Harness &h) {
	const auto reply = h.send("GET", "/definitely/missing.txt");
	TEST_EXPECT(reply.transport_ok && reply.code == 404);
	return 0;
}

int run(Harness &h) {
	struct Case { const char *name; int (*fn)(Harness &); };
	const Case cases[] = {
		{"health", test_health},
		{"lobbies_lists_seeded_games", test_lobbies_lists_seeded_games},
		{"register_ok", test_register_ok},
		{"register_duplicate", test_register_duplicate},
		{"admin_server_status_requires_token", test_admin_server_status_requires_token},
		{"admin_users_list", test_admin_users_list},
		{"unknown_path_404", test_unknown_path_404},
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn(h) != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
		// Declared first, so it outlives the pool and the listener: the database
		// file and its WAL siblings go only once every connection is closed.
		test_temp::TempDir temp("http_routes");
		ConnectionPool pool(temp.path / "novaworld.db");
		{
			auto boot = pool.acquire();
			const auto migrated =
					opennova::db::run_migrations(*boot, source_dir / "backend" / "migrations");
			TEST_EXPECT(!migrated.applied.empty());
			// The games seed alone: no dev users, so the players table starts empty.
			boot->exec_script(test_io::read_file_text(
					(source_dir / "backend" / "seed" / "0001_games.sql").string()));
		}

		// A free loopback port: bind an ephemeral one, release it, hand it to Crow.
		uint16_t port = 0;
		{
			net::ScopedSocket probe(net::tcp_listen(0, 1, &port, true));
			TEST_EXPECT(probe.is_valid() && port != 0);
		}

		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.admin_api_token = kAdminToken;
		config.http_port = port;
		// Roots that do not exist, so the catch-all family 404s deterministically.
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates";
		config.static_dir = temp.path / "static";

		opennova::ConnectionManager manager;
		nws::SessionStore sessions;
		nws::HttpListener http(manager, pool, sessions);
		TEST_EXPECT(http.start(config));

		Harness harness{pool, port, {}};
		rc = run(harness);
		http.stop(); // joins the Crow thread before the pool and the TempDir go
	}
	net::shutdown();
	if (rc == 0) std::printf("OK: http routes\n");
	return rc;
}
