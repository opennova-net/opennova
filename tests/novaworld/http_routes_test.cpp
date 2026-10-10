// The NovaWorld service's HTTP routes, driven in-process: a SQLite file in the
// run's temp directory under backend/migrations (plus the games seed), the
// Crow HttpListener from opennova_novaworld_server_core on port 0 (the OS
// picks; the harness reads the port back from bound_port()), and requests sent
// through apps/common's http_exchange to 127.0.0.1. Built only
// with BUILD_NOVAWORLD_HTTP. The cases share one listener and run in order
// (the admin users case reads the account the register case made; the
// concurrent case runs last, since it adds accounts and a host). The listener
// trusts 127.0.0.1 as its proxy, so the website-session cases each name their
// own client address in X-Real-IP and keep their own rate-limit buckets; a
// second listener on the same database sets the Secure cookie and trusts no
// proxy.
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
#include "web_session.h"

#include <base/io/sha256.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>

#include <crow/json.h>

#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
using opennova::db::ConnectionPool;

namespace {

constexpr const char *kAdminToken = "harness-admin-token";
// The one page in the templates root: the menu URLs the listener fills in.
constexpr const char *kUrlsTemplate = "urls.htm";
constexpr const char *kBearer = "Authorization: Bearer harness-admin-token";
constexpr const char *kJson = "Content-Type: application/json";

struct Harness {
	ConnectionPool &pool;
	nws::SessionStore &sessions; // the listener's, for what a reply only names by its tag
	uint16_t port = 0;
	// A second listener on the same database with the Secure session cookie
	// (ONNET_COOKIE_SECURE) and no trusted proxy.
	uint16_t secure_port = 0;
	std::string registered_pcid; // what the register case's reply carried

	// Every connect, send and recv waits at most `timeout_ms`.
	net::HttpReply send(const char *method, const std::string &path,
	                    const std::vector<std::string> &headers = {},
	                    const std::string &body = {}, int timeout_ms = 3000) const {
		return send_to(port, method, path, headers, body, timeout_ms);
	}

	net::HttpReply send_to(uint16_t to_port, const char *method, const std::string &path,
	                       const std::vector<std::string> &headers = {},
	                       const std::string &body = {}, int timeout_ms = 3000) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(to_port) + path;
		return net::http_exchange(method, url, headers, body, timeout_ms,
		                          [](const std::string &host, net::Endpoint &out) {
			                          return net::resolve_ipv4(host, out, false);
		                          });
	}
};

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

// A reply for a failure line: its status and body, or why no reply came.
std::string describe(const net::HttpReply &reply) {
	if (!reply.transport_ok) return "no reply (" + reply.error + ")";
	return "HTTP " + std::to_string(reply.code) + " " + body_text(reply);
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

// The value a reply's Set-Cookie gives `name`, "" when no Set-Cookie names it.
std::string set_cookie_value(const net::HttpReply &reply, const std::string &name) {
	static const std::string kSetCookie = "set-cookie";
	for (const std::string &line : reply.headers) {
		const auto colon = line.find(':');
		if (colon != kSetCookie.size()) continue;
		std::string header = line.substr(0, colon);
		for (char &c : header) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (header != kSetCookie) continue;
		const auto start = line.find_first_not_of(' ', colon + 1);
		if (start == std::string::npos || line.compare(start, name.size() + 1, name + "=") != 0) {
			continue;
		}
		const auto value = start + name.size() + 1;
		return line.substr(value, line.find(';', value) - value);
	}
	return {};
}

// GET /api/health on the port bound_port() reported: start() returned once Crow
// served, so the first request answers.
int test_health(Harness &h) {
	const net::HttpReply reply = h.send("GET", "/api/health");
	if (!reply.transport_ok) {
		std::fprintf(stderr, "  /api/health did not answer on :%u: %s\n",
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
	TEST_EXPECT(body_text(reply) == "{\"error\":\"unauthorized\"}");
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

// ---- website sessions -------------------------------------------------------

constexpr const char *kCsrf = "X-OpenNova-Request: 1";
// The cookie text the session routes set and clear, on the harness listener
// (no Secure: its config leaves cookie_secure off).
constexpr const char *kCookieTail = "; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax";
constexpr const char *kCookieCleared =
		"opennova_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";

// "X-Real-IP: <ip>". The harness listener trusts 127.0.0.1 as its proxy, so
// each case names its own client address and draws on its own rate buckets.
std::string from_ip(const std::string &ip) { return "X-Real-IP: " + ip; }

std::string with_session(const std::string &token) {
	return std::string("Cookie: ") + nws::kWebSessionCookie + "=" + token;
}

// Every value a reply's `name` header carries, in order.
std::vector<std::string> header_values(const net::HttpReply &reply, const std::string &name) {
	std::vector<std::string> out;
	for (const std::string &line : reply.headers) {
		const auto colon = line.find(':');
		if (colon != name.size()) continue;
		if (!std::equal(name.begin(), name.end(), line.begin(), [](char a, char b) {
			    return std::tolower(static_cast<unsigned char>(a)) ==
			           std::tolower(static_cast<unsigned char>(b));
		    })) {
			continue;
		}
		const auto start = line.find_first_not_of(' ', colon + 1);
		out.push_back(start == std::string::npos ? std::string() : line.substr(start));
	}
	return out;
}

std::string header_value(const net::HttpReply &reply, const std::string &name) {
	const auto values = header_values(reply, name);
	return values.empty() ? std::string() : values.front();
}

std::string credentials(const std::string &username, const std::string &password) {
	return "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";
}

net::HttpReply login(const Harness &h, const std::string &ip, const std::string &username,
                     const std::string &password) {
	return h.send("POST", "/api/login", {kJson, kCsrf, from_ip(ip)}, credentials(username, password));
}

// A successful login's session token, "" when the login failed.
std::string login_token(const Harness &h, const std::string &ip, const std::string &username,
                        const std::string &password) {
	const auto reply = login(h, ip, username, password);
	if (!reply.transport_ok || reply.code != 200) {
		std::fprintf(stderr, "  login %s: %s\n", username.c_str(), describe(reply).c_str());
		return {};
	}
	return set_cookie_value(reply, nws::kWebSessionCookie);
}

// An account made straight in the database: /api/register's per-address
// bucket belongs to the register cases.
int64_t make_account(Harness &h, const std::string &username, const std::string &password) {
	static int next = 0;
	char pcid[16];
	std::snprintf(pcid, sizeof(pcid), "%08x", 0x5e550000u + static_cast<unsigned>(next++));
	nws::CreateUserParams p;
	p.username = username;
	p.password = password;
	p.pcid = pcid;
	p.nwh = "1";
	p.nwhandle = username;
	const auto result = nws::create_user(*h.pool.acquire(), p);
	if (!result.ok) std::fprintf(stderr, "  create %s: %s\n", username.c_str(), result.error_code.c_str());
	return result.ok ? result.id : 0;
}

// An integer the session row of `token` answers `select` with (-1: no row).
int64_t session_scalar(Harness &h, const std::string &token, const std::string &select) {
	const auto rows = h.pool.acquire()->query(
			"SELECT " + select + " FROM web_sessions WHERE token_hash = ?;",
			{opennova::db::BindValue(nws::web_session_token_hash(token))});
	return rows.empty() ? -1 : rows.front().as_int(0).value_or(-1);
}

// POST /api/login: 200 with {id, username, role} and the session cookie (64
// hex digits, Path=/, the 30-day Max-Age, HttpOnly, SameSite=Lax; no Secure on
// this http listener), not cached. The table keeps only the token's SHA-256,
// beside the client address the trusted proxy named.
int test_login_sets_session_cookie(Harness &h) {
	const int64_t id = make_account(h, "web_alice", "pw-alice");
	TEST_EXPECT(id > 0);
	const auto reply = login(h, "10.1.0.1", "web_alice", "pw-alice");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["id"].i() == id);
	TEST_EXPECT(str(json["username"]) == "web_alice");
	TEST_EXPECT(str(json["role"]) == "player");
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");

	const std::string token = set_cookie_value(reply, nws::kWebSessionCookie);
	TEST_EXPECT(nws::is_web_session_token(token));
	const auto cookies = header_values(reply, "Set-Cookie");
	TEST_EXPECT(cookies.size() == 1);
	TEST_EXPECT(cookies[0] == std::string(nws::kWebSessionCookie) + "=" + token + kCookieTail);

	const auto rows = h.pool.acquire()->query(
			"SELECT token_hash, ip FROM web_sessions WHERE user_id = ?;",
			{opennova::db::BindValue(id)});
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_text(0).value() == opennova::io::sha256_hex(opennova::io::sha256(token)));
	TEST_EXPECT(rows[0].as_text(0).value() != token);
	TEST_EXPECT(rows[0].as_text(1).value() == "10.1.0.1");
	return 0;
}

// The Secure flag (ONNET_COOKIE_SECURE) on the second listener's login and
// logout cookies.
int test_cookie_secure_flag(Harness &h) {
	TEST_EXPECT(make_account(h, "web_secure", "pw-secure") > 0);
	auto reply = h.send_to(h.secure_port, "POST", "/api/login", {kJson, kCsrf},
	                       credentials("web_secure", "pw-secure"));
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const std::string token = set_cookie_value(reply, nws::kWebSessionCookie);
	TEST_EXPECT(nws::is_web_session_token(token));
	TEST_EXPECT(header_value(reply, "Set-Cookie") ==
	            std::string(nws::kWebSessionCookie) + "=" + token + kCookieTail + "; Secure");

	reply = h.send_to(h.secure_port, "POST", "/api/logout", {kCsrf, with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	TEST_EXPECT(header_value(reply, "Set-Cookie") == std::string(kCookieCleared) + "; Secure");
	return 0;
}

// A wrong password and an unknown username get the same 401 body and no
// cookie; a login without the CSRF header is refused before it is tried.
int test_login_failures_are_generic(Harness &h) {
	const auto wrong = login(h, "10.2.0.1", "web_alice", "not-her-password");
	const auto unknown = login(h, "10.2.0.1", "web_nobody", "not-her-password");
	TEST_EXPECT(wrong.transport_ok && wrong.code == 401);
	TEST_EXPECT(unknown.transport_ok && unknown.code == 401);
	TEST_EXPECT(body_text(wrong) == "{\"error\":\"invalid_credentials\"}");
	TEST_EXPECT(body_text(unknown) == body_text(wrong));
	TEST_EXPECT(header_values(wrong, "Set-Cookie").empty());
	TEST_EXPECT(header_values(unknown, "Set-Cookie").empty());

	auto reply = h.send("POST", "/api/login", {kJson, from_ip("10.2.0.1")},
	                    credentials("web_alice", "pw-alice"));
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"csrf_header_required\"}");
	TEST_EXPECT(header_values(reply, "Set-Cookie").empty());
	reply = h.send("POST", "/api/login", {kJson, "X-OpenNova-Request: yes", from_ip("10.2.0.1")},
	               credentials("web_alice", "pw-alice"));
	TEST_EXPECT(reply.transport_ok && reply.code == 403);

	reply = h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.2.0.1")},
	               "{\"username\":\"web_alice\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	reply = h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.2.0.1")},
	               "{\"username\":\"web_alice\",\"password\":7}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	reply = h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.2.0.1")}, "not json");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);

	// No CORS: another origin's preflight for the CSRF header is granted
	// nothing, so its script can never send that header.
	reply = h.send("OPTIONS", "/api/login",
	               {"Origin: https://elsewhere.example", "Access-Control-Request-Method: POST",
	                "Access-Control-Request-Headers: content-type, x-opennova-request"});
	TEST_EXPECT(reply.transport_ok);
	for (const std::string &line : reply.headers) {
		std::string lower = line;
		for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		TEST_EXPECT(lower.rfind("access-control-", 0) != 0);
	}
	return 0;
}

// The account status gates the site as it does the game login: the right
// password on a banned account is 403 account_banned, on any other inactive
// status 403 account_restricted; a wrong password stays the generic 401. A ban
// also ends the account's live sessions.
int test_login_refuses_inactive_accounts(Harness &h) {
	const int64_t banned = make_account(h, "web_banned", "pw-banned");
	const int64_t disabled = make_account(h, "web_disabled", "pw-disabled");
	const int64_t later = make_account(h, "web_later", "pw-later");
	TEST_EXPECT(banned > 0 && disabled > 0 && later > 0);
	auto set_status = [&h](int64_t id, const char *status) {
		nws::UpdateUserParams p;
		p.account_status = status;
		return nws::update_user(*h.pool.acquire(), id, p).ok;
	};
	TEST_EXPECT(set_status(banned, "banned"));
	TEST_EXPECT(set_status(disabled, "disabled"));

	auto reply = login(h, "10.3.0.1", "web_banned", "pw-banned");
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"account_banned\"}");
	TEST_EXPECT(header_values(reply, "Set-Cookie").empty());
	reply = login(h, "10.3.0.1", "web_disabled", "pw-disabled");
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"account_restricted\"}");
	reply = login(h, "10.3.0.1", "web_banned", "wrong");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"invalid_credentials\"}");
	TEST_EXPECT(h.pool.acquire()->query(
			"SELECT 1 FROM web_sessions WHERE user_id IN (?, ?);",
			{opennova::db::BindValue(banned), opennova::db::BindValue(disabled)}).empty());

	const std::string token = login_token(h, "10.3.0.2", "web_later", "pw-later");
	TEST_EXPECT(!token.empty());
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(token)}).code == 200);
	TEST_EXPECT(set_status(later, "banned"));
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(token)}).code == 401);
	TEST_EXPECT(session_scalar(h, token, "1") == -1);
	return 0;
}

// GET /api/me: {id, username, role} for a live session; 401 with no cookie, a
// malformed or unknown one (which the reply deletes), and an expired session,
// which the sweep's prune then removes.
int test_me_requires_live_session(Harness &h) {
	auto reply = h.send("GET", "/api/me");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"unauthorized\"}");
	TEST_EXPECT(!has_header(reply, "WWW-Authenticate"));
	reply = h.send("GET", "/api/me", {with_session("not-a-token")});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(header_value(reply, "Set-Cookie") == kCookieCleared);
	reply = h.send("GET", "/api/me", {with_session(std::string(64, 'a'))});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);

	TEST_EXPECT(make_account(h, "web_me", "pw-me") > 0);
	const std::string token = login_token(h, "10.4.0.1", "web_me", "pw-me");
	TEST_EXPECT(!token.empty());
	// Beside other cookies, as a browser sends it.
	reply = h.send("GET", "/api/me",
	               {"Cookie: theme=dark; " + std::string(nws::kWebSessionCookie) + "=" + token});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["username"]) == "web_me");
	TEST_EXPECT(str(json["role"]) == "player");
	TEST_EXPECT(json["id"].i() > 0);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	TEST_EXPECT(header_values(reply, "Set-Cookie").empty()); // fresh: nothing to renew

	h.pool.acquire()->exec(
			"UPDATE web_sessions SET expires_at = datetime('now', '-1 seconds') "
			"WHERE token_hash = ?;",
			{opennova::db::BindValue(nws::web_session_token_hash(token))});
	reply = h.send("GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(nws::prune_expired_web_sessions(*h.pool.acquire()) >= 1);
	TEST_EXPECT(session_scalar(h, token, "1") == -1);
	return 0;
}

// The 30-day expiry slides: a use a minute or more after the last recorded
// one moves it 30 days ahead and renews the cookie's Max-Age on the reply (on
// any route, an admin one included); a use within that minute writes nothing
// and sends no cookie.
int test_session_slides(Harness &h) {
	TEST_EXPECT(make_account(h, "web_slide", "pw-slide") > 0);
	const std::string token = login_token(h, "10.5.0.1", "web_slide", "pw-slide");
	TEST_EXPECT(!token.empty());
	auto age = [&h, &token](const char *last_seen) {
		h.pool.acquire()->exec(
				std::string("UPDATE web_sessions SET last_seen_at = datetime('now', '") + last_seen +
						"'), expires_at = datetime('now', '+1 days') WHERE token_hash = ?;",
				{opennova::db::BindValue(nws::web_session_token_hash(token))});
	};

	age("-120 seconds");
	auto reply = h.send("GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(header_value(reply, "Set-Cookie") ==
	            std::string(nws::kWebSessionCookie) + "=" + token + kCookieTail);
	TEST_EXPECT(session_scalar(h, token, "expires_at > datetime('now', '+29 days')") == 1);
	TEST_EXPECT(session_scalar(h, token, "last_seen_at >= datetime('now', '-5 seconds')") == 1);

	age("-10 seconds");
	reply = h.send("GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(header_values(reply, "Set-Cookie").empty());
	TEST_EXPECT(session_scalar(h, token, "expires_at < datetime('now', '+2 days')") == 1);

	// A player's session refused by an admin route still slid.
	age("-120 seconds");
	reply = h.send("GET", "/api/admin/users", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(header_value(reply, "Set-Cookie") ==
	            std::string(nws::kWebSessionCookie) + "=" + token + kCookieTail);
	TEST_EXPECT(session_scalar(h, token, "expires_at > datetime('now', '+29 days')") == 1);
	return 0;
}

// POST /api/logout: refused without the CSRF header (the session lives on);
// with it, 204, the row deleted and the cookie cleared, so the old token is
// dead. Logging out again still answers 204.
int test_logout_invalidates(Harness &h) {
	TEST_EXPECT(make_account(h, "web_logout", "pw-logout") > 0);
	const std::string token = login_token(h, "10.6.0.1", "web_logout", "pw-logout");
	TEST_EXPECT(!token.empty());
	auto reply = h.send("POST", "/api/logout", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"csrf_header_required\"}");
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(token)}).code == 200);

	reply = h.send("POST", "/api/logout", {kCsrf, with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	TEST_EXPECT(header_value(reply, "Set-Cookie") == kCookieCleared);
	TEST_EXPECT(session_scalar(h, token, "1") == -1);
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(token)}).code == 401);
	reply = h.send("POST", "/api/logout", {kCsrf, with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 204);

	// A new login ends the session the browser held before.
	const std::string first = login_token(h, "10.6.0.1", "web_logout", "pw-logout");
	TEST_EXPECT(!first.empty());
	reply = h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.6.0.1"), with_session(first)},
	               credentials("web_logout", "pw-logout"));
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const std::string second = set_cookie_value(reply, nws::kWebSessionCookie);
	TEST_EXPECT(!second.empty() && second != first);
	TEST_EXPECT(session_scalar(h, first, "1") == -1);
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(second)}).code == 200);
	return 0;
}

// The admin routes under each credential: none (401 + the Bearer challenge), a
// player's session (403), an admin's session (200; a state change needs the
// CSRF header, else 403), and the Bearer token (200, no CSRF header). The role
// is read on every request, so demoting the admin closes the routes to its
// live session; the role update takes only player or admin.
int test_admin_routes_by_credential(Harness &h) {
	TEST_EXPECT(make_account(h, "web_player", "pw-player") > 0);
	const std::string player = login_token(h, "10.7.0.1", "web_player", "pw-player");
	const int64_t admin_id = make_account(h, "web_admin", "pw-admin");
	TEST_EXPECT(!player.empty() && admin_id > 0);
	TEST_EXPECT(nws::promote_to_admin(*h.pool.acquire(), "web_admin"));
	auto reply = login(h, "10.7.0.2", "web_admin", "pw-admin");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["role"]) == "admin");
	const std::string admin = set_cookie_value(reply, nws::kWebSessionCookie);
	TEST_EXPECT(!admin.empty());

	reply = h.send("GET", "/api/admin/users");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(has_header(reply, "WWW-Authenticate"));
	reply = h.send("GET", "/api/admin/users", {with_session(player)});
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"forbidden\"}");
	reply = h.send("GET", "/api/admin/users", {with_session(admin)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const auto users = crow::json::load(body_text(reply));
	TEST_EXPECT(users && users["users"].size() >= 2);
	reply = h.send("GET", "/api/admin/users", {kBearer});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);

	// A state change: PUT /api/admin/server-status (maintenance stays on, as
	// the server-status case left it).
	const auto status_body = [](const char *message) {
		return std::string("{\"maintenance_enabled\":true,\"message\":\"") + message + "\"}";
	};
	reply = h.send("PUT", "/api/admin/server-status", {kJson, with_session(admin)},
	               status_body("no csrf"));
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"csrf_header_required\"}");
	TEST_EXPECT(nws::get_server_status(*h.pool.acquire()).message == "back soon");
	reply = h.send("PUT", "/api/admin/server-status", {kJson, kCsrf, with_session(player)},
	               status_body("player"));
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"forbidden\"}");
	reply = h.send("PUT", "/api/admin/server-status", {kJson, kCsrf, with_session(admin)},
	               status_body("via session"));
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(nws::get_server_status(*h.pool.acquire()).message == "via session");
	reply = h.send("PUT", "/api/admin/server-status", {kJson, kBearer}, status_body("back soon"));
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(nws::get_server_status(*h.pool.acquire()).message == "back soon");

	const std::string user_path = "/api/admin/users/" + std::to_string(admin_id);
	reply = h.send("PUT", user_path, {kJson, kBearer}, "{\"role\":\"superuser\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "invalid_field");
	reply = h.send("PUT", user_path, {kJson, kBearer}, "{\"role\":\"player\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["user"]["role"]) == "player");
	reply = h.send("GET", "/api/admin/users", {with_session(admin)});
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	return 0;
}

// ONNET_BOOTSTRAP_ADMIN's promotion (main() runs promote_to_admin at boot):
// the named account's sessions act as an admin's, its own live one included;
// a name no account has promotes nobody.
int test_bootstrap_admin(Harness &h) {
	TEST_EXPECT(make_account(h, "web_boss", "pw-boss") > 0);
	const std::string token = login_token(h, "10.8.0.1", "web_boss", "pw-boss");
	TEST_EXPECT(!token.empty());
	TEST_EXPECT(h.send("GET", "/api/admin/connections", {with_session(token)}).code == 403);

	TEST_EXPECT(nws::promote_to_admin(*h.pool.acquire(), "web_boss"));
	auto reply = h.send("GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["role"]) == "admin");
	TEST_EXPECT(h.send("GET", "/api/admin/connections", {with_session(token)}).code == 200);

	const auto before = nws::list_users(*h.pool.acquire());
	TEST_EXPECT(!nws::promote_to_admin(*h.pool.acquire(), "web_nobody"));
	const auto after = nws::list_users(*h.pool.acquire());
	TEST_EXPECT(after.size() == before.size());
	for (std::size_t i = 0; i < after.size(); ++i) TEST_EXPECT(after[i].role == before[i].role);
	return 0;
}

// Sends `request` until a 429 answers (at most `limit` tries) and returns how
// many went through first, -1 when none was refused.
template <typename Request>
int accepted_before_429(Request request, int limit, net::HttpReply &refusal) {
	for (int i = 0; i < limit; ++i) {
		net::HttpReply reply = request(i);
		if (!reply.transport_ok) {
			std::fprintf(stderr, "  try %d: %s\n", i, describe(reply).c_str());
			return -1;
		}
		if (reply.code == 429) {
			refusal = std::move(reply);
			return i;
		}
	}
	return -1;
}

bool is_retry_after(const net::HttpReply &reply) {
	const std::string value = header_value(reply, "Retry-After");
	return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
		return c >= '0' && c <= '9';
	}) && std::stoi(value) >= 1;
}

// The brakes answer 429 {"error":"rate_limited"} with Retry-After: login per
// client address (20 at once, counted before the body is read), login per
// username (10, whatever the address), register per address (10).
int test_rate_limits(Harness &h) {
	net::HttpReply refusal;
	int accepted = accepted_before_429(
			[&h](int) { return h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.9.0.1")}, "{}"); },
			60, refusal);
	TEST_EXPECT(accepted >= 20);
	TEST_EXPECT(body_text(refusal) == "{\"error\":\"rate_limited\"}");
	TEST_EXPECT(is_retry_after(refusal));
	// Another address keeps its own bucket.
	TEST_EXPECT(h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.9.0.2")}, "{}").code == 400);

	// One username from many addresses: an unknown one is braked like a real one.
	accepted = accepted_before_429(
			[&h](int i) {
				return login(h, "10.9.1." + std::to_string(i), "web_guessed", "guess" + std::to_string(i));
			},
			30, refusal);
	TEST_EXPECT(accepted >= 10);
	TEST_EXPECT(is_retry_after(refusal));

	accepted = accepted_before_429(
			[&h](int) { return h.send("POST", "/api/register", {kJson, from_ip("10.9.2.1")}, "not json"); },
			40, refusal);
	TEST_EXPECT(accepted >= 10);
	TEST_EXPECT(is_retry_after(refusal));
	TEST_EXPECT(h.send("POST", "/api/register", {kJson, from_ip("10.9.2.2")}, "not json").code == 400);
	return 0;
}

// The second listener trusts no proxy: an X-Real-IP from its peer is the
// client's own claim, so attempts that each name a fresh address all draw on
// the peer's one bucket and are braked.
int test_untrusted_proxy_header_ignored(Harness &h) {
	net::HttpReply refusal;
	const int accepted = accepted_before_429(
			[&h](int i) {
				return h.send_to(h.secure_port, "POST", "/api/login",
				                 {kJson, kCsrf, from_ip("10.10.0." + std::to_string(i))}, "{}");
			},
			60, refusal);
	TEST_EXPECT(accepted >= 0); // refused: one bucket, not sixty
	TEST_EXPECT(is_retry_after(refusal));
	return 0;
}

// Concurrent requests, the shape that broke when every handler shared one
// connection (#981): /api/register calls race /api/hosts and /jop_2.gsb reads
// while the test rewrites one host's row and roster on its own lease, the way
// the lobby session does (one transaction). Every registration's reply must
// carry its own new account, and every host read must hold one write's row and
// roster: the name, the Players count and every roster name from the same
// generation.
constexpr uint32_t kConcurrentRid = 2000;
constexpr int kConcurrentRegistrations = 16;
constexpr int kHostGenerations = 40;
// The case checks that the replies are each request's own and consistent, not
// how fast they come: the bcrypt registrations queue behind Crow's few workers
// (three on a 4-vCPU runner), so a request may wait well past the default 3 s.
constexpr int kConcurrentTimeoutMs = 30000;
// The readers' pause between rounds, so they do not crowd the registrations
// out of the workers.
constexpr auto kReaderPause = std::chrono::milliseconds(5);

std::string generation_name(int generation, int slot) {
	return "c" + std::to_string(generation) + "-s" + std::to_string(slot);
}

// The generation a "S-<n>" server name or a "c<n>-s<slot>" roster name carries, or -1.
int generation_in(const std::string &text) {
	std::size_t at = 0;
	if (text.compare(0, 2, "S-") == 0) at = 2;
	else if (text.compare(0, 1, "c") == 0) at = 1;
	else return -1;
	int value = 0;
	bool digits = false;
	for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; ++at) {
		value = value * 10 + (text[at] - '0');
		digits = true;
	}
	return digits ? value : -1;
}

void write_generation(ConnectionPool &pool, int generation) {
	opennova::hostdb::HostRow row;
	row.rid = kConcurrentRid;
	row.game = "jop_2_consumer";
	row.server_name = "S-" + std::to_string(generation);
	row.host_ip = "10.0.0.8";
	row.host_port = 32768;
	row.player_count = 1 + generation % 4;
	row.max_players = 16;
	std::vector<opennova::HostRosterSlot> roster;
	for (int slot = 0; slot < row.player_count; ++slot) {
		opennova::HostRosterSlot s;
		s.slot = slot;
		s.player_name = generation_name(generation, slot);
		roster.push_back(std::move(s));
	}
	auto conn = pool.acquire();
	opennova::db::Transaction tx(*conn);
	opennova::hostdb::upsert_host(*conn, row);
	opennova::hostdb::replace_roster(*conn, kConcurrentRid, roster);
	tx.commit();
}

// One read's verdict: empty when the host it found agrees with itself.
std::string check_host(const std::string &name, int players,
                       const std::vector<std::string> &roster) {
	const int generation = generation_in(name);
	if (generation < 0) return "server name '" + name + "'";
	if (players != 1 + generation % 4 || static_cast<int>(roster.size()) != players) {
		return name + " with Players " + std::to_string(players) + " and " +
		       std::to_string(roster.size()) + " roster names";
	}
	for (int slot = 0; slot < players; ++slot) {
		if (roster[slot] != generation_name(generation, slot)) {
			return name + " beside roster name '" + roster[slot] + "'";
		}
	}
	return {};
}

int test_concurrent_requests(Harness &h) {
	write_generation(h.pool, 0);
	const std::size_t accounts_before = nws::list_users(*h.pool.acquire()).size();

	std::mutex mu;
	std::vector<std::string> failures;
	auto fail = [&mu, &failures](std::string line) {
		std::lock_guard<std::mutex> lock(mu);
		failures.push_back(std::move(line));
	};
	struct Registered {
		std::string username;
		int64_t id = 0;
	};
	std::vector<Registered> registered(kConcurrentRegistrations);
	std::atomic<bool> writing{true};
	std::atomic<int> host_reads{0};

	std::vector<std::thread> threads;
	for (int i = 0; i < kConcurrentRegistrations; ++i) {
		threads.emplace_back([&h, &registered, &fail, i] {
			const std::string username = "conc" + std::to_string(i);
			// Each from its own address, past /api/register's per-address brake.
			const auto reply = h.send("POST", "/api/register",
			                          {kJson, from_ip("10.20.0." + std::to_string(i))},
			                          "{\"username\":\"" + username + "\",\"password\":\"pw\"}",
			                          kConcurrentTimeoutMs);
			const auto json = crow::json::load(body_text(reply));
			if (!reply.transport_ok || reply.code != 201 || !json || !json.has("user")) {
				fail(username + ": " + describe(reply));
				return;
			}
			if (str(json["user"]["username"]) != username) {
				fail(username + ": the reply carried '" + str(json["user"]["username"]) + "'");
			}
			registered[i] = {username, json["user"]["id"].i()};
		});
	}
	std::vector<std::thread> readers;
	for (int r = 0; r < 3; ++r) {
		readers.emplace_back([&h, &writing, &host_reads, &fail] {
			while (writing.load()) {
				const auto hosts = h.send("GET", "/api/hosts", {}, {}, kConcurrentTimeoutMs);
				const auto json = crow::json::load(body_text(hosts));
				if (!hosts.transport_ok || hosts.code != 200 || !json) {
					fail("/api/hosts: " + describe(hosts));
					return;
				}
				for (const auto &e : json["hosts"]) {
					if (e["rid"].i() != kConcurrentRid) continue;
					std::vector<std::string> names;
					for (const auto &s : e["roster"]) names.push_back(str(s["player_name"]));
					const auto bad = check_host(str(e["server_name"]),
					                            static_cast<int>(e["players"].i()), names);
					if (!bad.empty()) fail("/api/hosts: " + bad);
				}
				const auto gsb = h.send("GET", "/jop_2.gsb", {}, {}, kConcurrentTimeoutMs);
				if (!gsb.transport_ok || gsb.code != 200) {
					fail("/jop_2.gsb: " + describe(gsb));
					return;
				}
				opennova::GsbResponse parsed;
				if (!opennova::gsb_parse_response(gsb.body.data(), gsb.body.size(), parsed)) {
					fail("/jop_2.gsb: " + std::to_string(gsb.body.size()) + " bytes that do not parse");
					return;
				}
				for (const auto &s : parsed.servers) {
					if (s.rid != kConcurrentRid) continue;
					const auto bad = check_host(s.server_name, s.players, s.player_names);
					if (!bad.empty()) fail("/jop_2.gsb: " + bad);
				}
				host_reads.fetch_add(1);
				std::this_thread::sleep_for(kReaderPause);
			}
		});
	}
	for (int generation = 1; generation <= kHostGenerations; ++generation) {
		try {
			write_generation(h.pool, generation);
		} catch (const std::exception &e) {
			fail("host write " + std::to_string(generation) + ": " + e.what());
		}
	}
	for (auto &t : threads) t.join();
	writing.store(false);
	for (auto &t : readers) t.join();

	for (const auto &line : failures) std::fprintf(stderr, "  %s\n", line.c_str());
	TEST_EXPECT(failures.empty());
	TEST_EXPECT(host_reads.load() > 0);

	// Every reply's id is that registration's own account, and no two share one.
	auto conn = h.pool.acquire();
	std::set<int64_t> ids;
	for (const auto &r : registered) {
		TEST_EXPECT(ids.insert(r.id).second);
		const auto user = nws::get_user_by_id(*conn, r.id);
		TEST_EXPECT(user && user->username == r.username);
	}
	TEST_EXPECT(nws::list_users(*conn).size() ==
	            accounts_before + static_cast<std::size_t>(kConcurrentRegistrations));
	return 0;
}

// GET /NWHost.dll, the first call: a fresh session tag
// (NWServer:NWHost.dll:SESSIONTAG:<0..99999>:<8 hex>) whose stored HOSTKEY is 48
// letters A-P (24 random bytes, a nibble each, from the OS CSPRNG); two first
// calls share neither.
int test_nwhost_first_call_mints_hostkey(Harness &h) {
	static const std::string kTagPrefix = "NWServer:NWHost.dll:SESSIONTAG:";
	std::string tags[2];
	std::string keys[2];
	for (int i = 0; i < 2; ++i) {
		const auto reply = h.send("GET", "/NWHost.dll?success=jop_2_host2.htm&pfid=28");
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		tags[i] = set_cookie_value(reply, "NWJOINSESSIONTAG");
		TEST_EXPECT(tags[i].compare(0, kTagPrefix.size(), kTagPrefix) == 0);
		const std::string tail = tags[i].substr(kTagPrefix.size());
		const auto colon = tail.find(':');
		TEST_EXPECT(colon != std::string::npos && colon >= 1 && colon <= 5);
		TEST_EXPECT(std::all_of(tail.begin(), tail.begin() + colon,
		                        [](char c) { return c >= '0' && c <= '9'; }));
		TEST_EXPECT(is_lower_hex8(tail.substr(colon + 1)));
		const auto session = h.sessions.get_host(tags[i]);
		TEST_EXPECT(session.has_value());
		keys[i] = session->host_key;
		TEST_EXPECT(keys[i].size() == 48);
		TEST_EXPECT(std::all_of(keys[i].begin(), keys[i].end(),
		                        [](char c) { return c >= 'A' && c <= 'P'; }));
	}
	TEST_EXPECT(tags[0] != tags[1]);
	TEST_EXPECT(keys[0] != keys[1]);
	return 0;
}

// GET /nwprepare.dll renders the page it names with HOST_URL and GSB_SERVER
// on the public host and the port Crow serves: the OS's pick here, where the
// config's port is 0.
int test_menu_urls_name_bound_port(Harness &h) {
	const auto reply = h.send("GET", std::string("/nwprepare.dll?url=") + kUrlsTemplate);
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const std::string base = "http://127.0.0.1:" + std::to_string(h.port);
	const std::string want = base + "/nwhost.dll|" + base + "/jop_2.gsb";
	const std::string body = body_text(reply);
	if (body != want) std::fprintf(stderr, "  the menu URLs: %s\n", body.c_str());
	TEST_EXPECT(body == want);
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
		{"login_sets_session_cookie", test_login_sets_session_cookie},
		{"cookie_secure_flag", test_cookie_secure_flag},
		{"login_failures_are_generic", test_login_failures_are_generic},
		{"login_refuses_inactive_accounts", test_login_refuses_inactive_accounts},
		{"me_requires_live_session", test_me_requires_live_session},
		{"session_slides", test_session_slides},
		{"logout_invalidates", test_logout_invalidates},
		{"admin_routes_by_credential", test_admin_routes_by_credential},
		{"bootstrap_admin", test_bootstrap_admin},
		{"rate_limits", test_rate_limits},
		{"untrusted_proxy_header_ignored", test_untrusted_proxy_header_ignored},
		{"nwhost_first_call_mints_hostkey", test_nwhost_first_call_mints_hostkey},
		{"menu_urls_name_bound_port", test_menu_urls_name_bound_port},
		{"unknown_path_404", test_unknown_path_404},
		{"concurrent_requests", test_concurrent_requests},
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

		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.admin_api_token = kAdminToken;
		// Port 0: Crow binds the port the OS picks, and start() returns once it
		// serves there, with that port in bound_port().
		config.http_port = 0;
		// Web roots that do not exist and a templates root holding only the URL
		// page, so the catch-all family 404s deterministically.
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates";
		config.static_dir = temp.path / "static";
		std::filesystem::create_directories(config.templates_dir);
		const std::string urls_page = "{{HOST_URL}}|{{GSB_SERVER}}";
		TEST_EXPECT(test_io::write_file((config.templates_dir / kUrlsTemplate).string(),
		                                std::vector<uint8_t>(urls_page.begin(), urls_page.end())));

		// The harness talks to the listener from 127.0.0.1 and names each
		// case's client address in X-Real-IP, the way nginx forwards it.
		config.trusted_proxies = {"127.0.0.1"};

		opennova::ConnectionManager manager;
		nws::SessionStore sessions;
		nws::HttpListener http(manager, pool, sessions);
		TEST_EXPECT(http.start(config));
		TEST_EXPECT(http.bound_port() != 0);

		// The second listener: the Secure cookie (an https site) and no
		// trusted proxy.
		nws::ServerConfig secure_config = config;
		secure_config.cookie_secure = true;
		secure_config.trusted_proxies.clear();
		nws::HttpListener secure_http(manager, pool, sessions);
		TEST_EXPECT(secure_http.start(secure_config));
		TEST_EXPECT(secure_http.bound_port() != 0);

		Harness harness{pool, sessions, http.bound_port(), secure_http.bound_port(), {}};
		rc = run(harness);
		// Join the Crow threads before the pool and the TempDir go.
		secure_http.stop();
		http.stop();
	}
	net::shutdown();
	if (rc == 0) std::printf("OK: http routes\n");
	return rc;
}
