// The NovaWorld service's HTTP routes, driven in-process: a database of the
// run's own (common/test_database.h: backend/migrations plus the games seed), the
// Crow HttpListener from opennova_novaworld_server_core on port 0 (the OS
// picks; the harness reads the port back from bound_port()), and requests sent
// through apps/common's http_exchange to 127.0.0.1. Built only
// with BUILD_NOVAWORLD_HTTP. The cases share one listener and run in order
// (the admin users case reads the account the register case made; the
// concurrent case runs last, since it adds accounts and a host). The listener
// trusts 127.0.0.1 as its proxy and answers /api/ to it alone
// (ONNET_API_PROXY_ONLY), so the website-session cases each name their own
// client address in X-Real-IP and keep their own rate-limit buckets; a second
// listener on the same database sets the Secure (__Host-) cookie, trusts no
// proxy and serves copies of the retail templates and static files, with a
// planted secret file beside them that no reply may carry; a third serves the
// same retail roots with ONNET_API_PROXY_ONLY and a proxy the harness is not,
// so its /api/ refuses the harness while its retail routes answer. The retail
// POST /NWLogin.dll cases post an EPASK-encrypted form the way a stock client
// does. The first listener's mail goes to a FakeMailer through a real
// MailOutbox (novaworld/fake_mailer.h); the other two run with mail off.
//
// The listener and the test share one db::ConnectionPool, the way main() wires
// the server: each Crow handler leases a connection for its request, and the
// test thread leases its own for every direct read or write.

#include "admin_tcp_server.h"
#include <net/title/title_identity.h>
#include <base/gameprofile/gameprofile.h>
#include "auth.h"
#include "billing/billing.h"
#include "billing/billing_repository.h"
#include "billing/billing_routes.h"
#include "billing/entitlement.h"
#include "novaworld/billing_fake_stripe.h"
#include "health.h"
#include "hosted/admin_link.h"
#include "hosted/console_commands.h"
#include "hosted/hosted_repository.h"
#include "hosted/limits_hold.h"
#include "hosted/stats_repository.h"
#include "hosted/supervisor.h"
#include "hosted_fake_driver.h"
#include "http_cookies.h"
#include "http_listener.h"
#include "mailer.h"
#include "novaworld/fake_mailer.h"
#include "net_http.h"
#include "net_sockets.h"
#include "nw_udp_listener.h"
#include "server_config.h"
#include "session_store.h"
#include "template_engine.h"
#include "web_session.h"

#include <base/io/base64.h>
#include <base/io/cp1252.h>
#include <base/io/crt_rand.h>
#include <base/io/sha256.h>
#include <formats/admincfg/admin_cfg.h>
#include <net/admin/admin_server.h>
#include <net/novacrypto/cdkey.h>
#include <net/novacrypto/epask.h>
#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
#include <net/novaworld/connection/manager.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/http_login.h>

#include <crow/json.h>

#include "common/file_io.h"
#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
using opennova::db::ConnectionPool;

namespace {

// The harness watchdog's hand-stepped clock and its watched loop's counter.
std::atomic<uint64_t> g_watch_clock_ms{ 0 };
std::atomic<uint64_t> g_watch_turns{ 0 };

constexpr const char *kAdminToken = "harness-admin-token";
// The one page in the templates root: the menu URLs the listener fills in.
constexpr const char *kUrlsTemplate = "urls.htm";
constexpr const char *kBearer = "Authorization: Bearer harness-admin-token";
constexpr const char *kJson = "Content-Type: application/json";
// The CSRF header the site's own pages send (web/src/api/client.ts).
constexpr const char *kCsrf = "X-OpenNova-Request: 1";
// The first listener's ONNET_SITE_URL: the links its mail carries.
constexpr const char *kMailSiteUrl = "https://site.example";

struct Harness {
	ConnectionPool &pool;
	nws::SessionStore &sessions; // the listener's, for what a reply only names by its tag
	nws::NwUdpListener &nw_udp;  // the push routes' channel, a real listener on loopback
	uint16_t port = 0;
	// A second listener on the same database with the Secure session cookie
	// (ONNET_COOKIE_SECURE) and no trusted proxy.
	uint16_t secure_port = 0;
	std::string registered_pcid; // what the register case's reply carried
	// The second listener also serves copies of the retail templates and static
	// files (under retail_root); a planted secret file sits beside each root
	// of both listeners, and symlinks under the retail roots lead to it.
	std::filesystem::path temp_root;
	std::filesystem::path retail_root;
	// A third listener on the same database over the retail roots, with
	// ONNET_API_PROXY_ONLY and a trusted proxy the harness never is, so its
	// /api/ refuses every request here; its retail login brake starts unspent
	// for the cases that log in through it.
	uint16_t fresh_port = 0;
	// The first listener's admin links (the console routes run their commands on
	// them); the console case syncs them by hand.
	opennova::novaworld_server::hosted::AdminLinks *links = nullptr;
	// The first listener's stall watchdog (GET /healthz): its clock and its one
	// watched loop's counter are the test's (g_watch_clock_ms, g_watch_turns).
	nws::Watchdog *watchdog = nullptr;
	// The first listener's billing over an in-memory Stripe (the Secure
	// listener runs with none: billing off).
	billing_fake::FakeStripeApi *stripe = nullptr;
	nws::billing::Billing *billing = nullptr;
	// The first listener's hosted servers' limits hold (no supervisor sets it
	// here: the billing admin case does by hand).
	nws::hosted::LimitsHold *limits_hold = nullptr;
	// The first listener's mail: its outbox, over a mailer that records.
	fake_mail::FakeMailer *mailer = nullptr;
	nws::MailOutbox *outbox = nullptr;

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

// "X-Real-IP: <ip>". The harness listener trusts 127.0.0.1 as its proxy, so
// each case names its own client address and draws on its own rate buckets.
std::string from_ip(const std::string &ip) { return "X-Real-IP: " + ip; }

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
// straight into active_hosts shows up under its game in camelCase, and one
// registered under the OpenNova LobbyName under the OpenNova game alone.
int test_lobbies_lists_seeded_games(Harness &h) {
	auto reply = h.send("GET", "/api/lobbies");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json.has("games"));
	TEST_EXPECT(json["games"].size() == 3);
	TEST_EXPECT(str(json["games"][0]["slug"]) == "dfx2_consumer");
	TEST_EXPECT(!str(json["games"][0]["displayName"]).empty());
	TEST_EXPECT(str(json["games"][1]["slug"]) == "jop_2_consumer");
	TEST_EXPECT(str(json["games"][2]["slug"]) == "opennova_0_consumer");
	TEST_EXPECT(str(json["games"][2]["displayName"]) == "OpenNova");
	TEST_EXPECT(json["games"][0]["hosts"].size() == 0);
	TEST_EXPECT(json["games"][1]["hosts"].size() == 0);
	TEST_EXPECT(json["games"][2]["hosts"].size() == 0);

	{
		opennova::hostdb::HostRow on;
		on.rid = 1001;
		on.gsid = "GSID-HARNESS-ON";
		on.game = "opennova_0_consumer";
		on.server_name = "Harness ON Srv";
		on.host_ip = "10.0.0.8";
		on.host_port = 32768;
		opennova::hostdb::upsert_host(*h.pool.acquire(), on);
		reply = h.send("GET", "/api/lobbies");
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		json = crow::json::load(body_text(reply));
		TEST_EXPECT(json && json["games"].size() == 3);
		TEST_EXPECT(json["games"][1]["hosts"].size() == 0);
		TEST_EXPECT(json["games"][2]["hosts"].size() == 1);
		TEST_EXPECT(str(json["games"][2]["hosts"][0]["serverName"]) == "Harness ON Srv");
		opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), 1001);
	}

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
	TEST_EXPECT(json && json["games"].size() == 3);
	TEST_EXPECT(json["games"][0]["hosts"].size() == 0);
	TEST_EXPECT(json["games"][2]["hosts"].size() == 0);
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

// Every message the first listener's outbox has sent so far, once it has
// sent all it was handed.
std::vector<nws::MailMessage> sent_mail(Harness &h) {
	h.outbox->drain();
	return h.mailer->messages();
}

// POST /api/register: 201 with the public record (a fresh 8-hex PCID, the
// email as stored, lowercased; never the hash), the account authenticates
// against its bcrypt hash, and its welcome mail went to its address.
int test_register_ok(Harness &h) {
	const auto reply = h.send("POST", "/api/register", {kJson, kCsrf},
	                          "{\"username\":\"harness\",\"password\":\"pw-harness\","
	                          "\"nwhandle\":\"HarnessPlayer\",\"email\":\"Harness@Example.COM\"}");
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
	TEST_EXPECT(str(user["email"]) == "harness@example.com");
	TEST_EXPECT(is_lower_hex8(str(user["pcid"])));
	TEST_EXPECT(user["id"].i() > 0);
	h.registered_pcid = str(user["pcid"]);

	const auto mail = sent_mail(h);
	TEST_EXPECT(mail.size() == 1);
	TEST_EXPECT(mail[0].kind == "welcome" && mail[0].to == "harness@example.com");
	TEST_EXPECT(mail[0].text.find("harness") != std::string::npos);
	TEST_EXPECT(mail[0].text.find(std::string(kMailSiteUrl) + "/") != std::string::npos);
	h.mailer->clear();

	auto conn = h.pool.acquire();
	const auto stored = nws::get_user_by_username(*conn, "harness");
	TEST_EXPECT(stored.has_value());
	TEST_EXPECT(stored->id == user["id"].i());
	TEST_EXPECT(stored->pcid == h.registered_pcid);
	TEST_EXPECT(stored->email == "harness@example.com");
	TEST_EXPECT(nws::authenticate_user(*conn, "harness", "pw-harness").has_value());
	TEST_EXPECT(!nws::authenticate_user(*conn, "harness", "wrong").has_value());
	return 0;
}

// POST /api/register refusals: a taken username, a missing password or email,
// an email that is no address or is another account's (in any case), a body
// that is not a JSON object, a field of the wrong type, a username over 64
// bytes or holding a control byte, and a request without the CSRF header.
// None of them adds an account or sends a mail.
int test_register_duplicate(Harness &h) {
	auto reply = h.send("POST", "/api/register", {kJson, kCsrf},
	                    "{\"username\":\"harness\",\"password\":\"pw-harness\","
	                    "\"nwhandle\":\"HarnessPlayer\",\"email\":\"other@example.com\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["error"]) == "username_exists");

	reply = h.send("POST", "/api/register", {kJson, kCsrf},
	               "{\"username\":\"nopw\",\"email\":\"nopw@example.com\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["error"]) == "missing_field");

	// The email: required, an address, one account's. From an address of
	// their own: the register brake's bucket for the harness's own is nearly spent.
	reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.2.0.1")}, "{\"username\":\"nomail\",\"password\":\"pw\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "missing_field");
	for (const char *bad : { "nomail", "nomail@localhost", "two@at@example.com", "sp ace@example.com" }) {
		reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.2.0.1")},
		               std::string("{\"username\":\"badmail\",\"password\":\"pw\",\"email\":\"") + bad + "\"}");
		TEST_EXPECT(reply.transport_ok && reply.code == 400);
		TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "invalid_field");
	}
	reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.2.0.1")},
	               "{\"username\":\"samemail\",\"password\":\"pw\",\"email\":\"HARNESS@example.com\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "email_exists");
	reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.2.0.1")},
	               "{\"username\":\"typedmail\",\"password\":\"pw\",\"email\":7}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"invalid_field\"}");

	reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.2.0.1")}, "not json");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["error"]) == "invalid_json");

	reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.2.0.1")}, "[\"username\"]");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"invalid_json\"}");
	reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.2.0.1")},
	               "{\"username\":\"typed\",\"password\":12345}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"invalid_field\"}");

	// The username rule: at most 64 bytes, no control byte.
	reply = h.send("POST", "/api/register", {kJson, kCsrf},
	               "{\"username\":\"" + std::string(65, 'u') + "\",\"password\":\"pw\",\"email\":\"u@example.com\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "invalid_field");
	reply = h.send("POST", "/api/register", {kJson, kCsrf},
	               "{\"username\":\"two\\nlines\",\"password\":\"pw\",\"email\":\"two@example.com\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "invalid_field");

	// Without the CSRF header: refused before anything is read.
	reply = h.send("POST", "/api/register", {kJson},
	               "{\"username\":\"forged\",\"password\":\"pw\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"csrf_header_required\"}");

	TEST_EXPECT(nws::list_users(*h.pool.acquire()).size() == 1);
	TEST_EXPECT(sent_mail(h).empty());
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
	TEST_EXPECT(str(json["users"][0]["email"]) == "harness@example.com");
	return 0;
}

// ---- website sessions -------------------------------------------------------

// The cookie text the session routes set and clear, on the harness listener
// (no Secure: its config leaves cookie_secure off); the Secure listener names
// the cookie __Host-opennova_session.
constexpr const char *kCookieTail = "; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax";
constexpr const char *kCookieCleared =
		"opennova_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";

std::string with_cookie(const char *name, const std::string &token) {
	return std::string("Cookie: ") + name + "=" + token;
}

std::string with_session(const std::string &token) {
	return with_cookie(nws::kWebSessionCookie, token);
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

// Whether a reply's Set-Cookie names `name` at all (set_cookie_value reads an
// empty value and none alike).
bool sets_cookie(const net::HttpReply &reply, const std::string &name) {
	for (const std::string &line : header_values(reply, "Set-Cookie")) {
		if (line.compare(0, name.size() + 1, name + "=") == 0)
			return true;
	}
	return false;
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

// A HOSTKEY recorded for `user` the way /NWHost.dll records one for its retail
// login: at the account's credentials generation now. True when recorded.
bool record_key_for(opennova::db::Database &db, const std::string &key, int64_t user) {
	const auto account = nws::get_user_by_id(db, user);
	return account && opennova::hostdb::record_host_key(db, key, user, account->credentials_gen);
}

bool record_key_for(Harness &h, const std::string &key, int64_t user) {
	return record_key_for(*h.pool.acquire(), key, user);
}

// The site role an admin gives an account (PUT /api/admin/users/<id> with the
// Bearer token): true on 200.
bool set_role(Harness &h, int64_t id, const char *role) {
	const auto reply = h.send("PUT", "/api/admin/users/" + std::to_string(id), {kJson, kBearer},
	                          std::string("{\"role\":\"") + role + "\"}");
	return reply.transport_ok && reply.code == 200;
}

int64_t admin_count(Harness &h) {
	return h.pool.acquire()
			->query("SELECT COUNT(*) FROM players WHERE role = 'admin';")
			.front()
			.as_int(0)
			.value_or(-1);
}

// An integer the session row of `token` answers `select` with (-1: no row).
int64_t session_scalar(Harness &h, const std::string &token, const std::string &select) {
	const auto rows = h.pool.acquire()->query(
			"SELECT " + select + " FROM web_sessions WHERE token_hash = $1;",
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
			"SELECT token_hash, ip FROM web_sessions WHERE user_id = $1;",
			{opennova::db::BindValue(id)});
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_text(0).value() == opennova::io::sha256_hex(opennova::io::sha256(token)));
	TEST_EXPECT(rows[0].as_text(0).value() != token);
	TEST_EXPECT(rows[0].as_text(1).value() == "10.1.0.1");
	return 0;
}

// ONNET_COOKIE_SECURE (the second listener): the cookie is
// __Host-opennova_session, Secure, and the server reads only that name, so a
// plain opennova_session cookie (one a sibling subdomain could plant) is no
// session there. Logout clears the __Host- cookie.
int test_cookie_secure_flag(Harness &h) {
	TEST_EXPECT(make_account(h, "web_secure", "pw-secure") > 0);
	auto reply = h.send_to(h.secure_port, "POST", "/api/login", {kJson, kCsrf},
	                       credentials("web_secure", "pw-secure"));
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(set_cookie_value(reply, nws::kWebSessionCookie).empty());
	const std::string token = set_cookie_value(reply, nws::kWebSessionHostCookie);
	TEST_EXPECT(nws::is_web_session_token(token));
	TEST_EXPECT(header_value(reply, "Set-Cookie") ==
	            std::string(nws::kWebSessionHostCookie) + "=" + token + kCookieTail + "; Secure");

	reply = h.send_to(h.secure_port, "GET", "/api/me", {with_cookie(nws::kWebSessionHostCookie, token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	reply = h.send_to(h.secure_port, "GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	// And the http listener reads only the plain name.
	reply = h.send("GET", "/api/me", {with_cookie(nws::kWebSessionHostCookie, token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);

	reply = h.send_to(h.secure_port, "POST", "/api/logout",
	                  {kCsrf, with_cookie(nws::kWebSessionHostCookie, token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 204);
	TEST_EXPECT(header_value(reply, "Set-Cookie") ==
	            std::string(nws::kWebSessionHostCookie) +
	                    "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax; Secure");
	TEST_EXPECT(session_scalar(h, token, "1") == -1);
	return 0;
}

// A wrong password and an unknown username get the same 401 body and no
// cookie; a login without the CSRF header is refused before it is tried;
// malformed bodies are 400s, never a 500; no CORS grant is ever made.
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

	struct Malformed { const char *body; const char *error; };
	const Malformed malformed[] = {
		{"{\"username\":\"web_alice\"}", "missing_field"},
		{"{\"username\":\"web_alice\",\"password\":7}", "invalid_field"},
		{"{\"username\":[\"web_alice\"],\"password\":\"pw-alice\"}", "invalid_field"},
		{"not json", "invalid_json"},
		{"[1,2]", "invalid_json"},
		{"5", "invalid_json"},
		{"\"web_alice\"", "invalid_json"},
	};
	for (const Malformed &m : malformed) {
		reply = h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.2.0.2")}, m.body);
		if (reply.code != 400) std::fprintf(stderr, "  body %s: %s\n", m.body, describe(reply).c_str());
		TEST_EXPECT(reply.transport_ok && reply.code == 400);
		TEST_EXPECT(body_text(reply) == std::string("{\"error\":\"") + m.error + "\"}");
	}
	// A username past 64 bytes is refused before any brake or lookup.
	reply = h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.2.0.2")},
	               credentials(std::string(65, 'u'), "pw"));
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"invalid_field\"}");

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
// through the admin route ends the account's sessions at once, before they
// are used again.
int test_login_refuses_inactive_accounts(Harness &h) {
	const int64_t banned = make_account(h, "web_banned", "pw-banned");
	const int64_t disabled = make_account(h, "web_disabled", "pw-disabled");
	const int64_t later = make_account(h, "web_later", "pw-later");
	TEST_EXPECT(banned > 0 && disabled > 0 && later > 0);
	auto set_status = [&h](int64_t id, const char *status) {
		const auto reply = h.send("PUT", "/api/admin/users/" + std::to_string(id), {kJson, kBearer},
		                          std::string("{\"account_status\":\"") + status + "\"}");
		return reply.transport_ok && reply.code == 200;
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
			"SELECT 1 FROM web_sessions WHERE user_id IN ($1, $2);",
			{opennova::db::BindValue(banned), opennova::db::BindValue(disabled)}).empty());

	const std::string token = login_token(h, "10.3.0.2", "web_later", "pw-later");
	TEST_EXPECT(!token.empty());
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(token)}).code == 200);
	TEST_EXPECT(set_status(later, "banned"));
	TEST_EXPECT(session_scalar(h, token, "1") == -1); // gone with the ban itself
	TEST_EXPECT(set_status(later, "active"));
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(token)}).code == 401);

	// A password reset ends the sessions too.
	const std::string again = login_token(h, "10.3.0.2", "web_later", "pw-later");
	TEST_EXPECT(!again.empty());
	reply = h.send("PUT", "/api/admin/users/" + std::to_string(later), {kJson, kBearer},
	               "{\"password\":\"pw-later-2\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(session_scalar(h, again, "1") == -1);
	TEST_EXPECT(h.send("GET", "/api/me", {with_session(again)}).code == 401);
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
			"UPDATE web_sessions SET expires_at = (now() + interval '-1 seconds') "
			"WHERE token_hash = $1;",
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
				std::string("UPDATE web_sessions SET last_seen_at = (now() + interval '") + last_seen +
						"'), expires_at = (now() + interval '+1 days') WHERE token_hash = $1;",
				{opennova::db::BindValue(nws::web_session_token_hash(token))});
	};

	age("-120 seconds");
	auto reply = h.send("GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(header_value(reply, "Set-Cookie") ==
	            std::string(nws::kWebSessionCookie) + "=" + token + kCookieTail);
	TEST_EXPECT(session_scalar(h, token, "expires_at > (now() + interval '+29 days')") == 1);
	TEST_EXPECT(session_scalar(h, token, "last_seen_at >= (now() + interval '-5 seconds')") == 1);

	age("-10 seconds");
	reply = h.send("GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(header_values(reply, "Set-Cookie").empty());
	TEST_EXPECT(session_scalar(h, token, "expires_at < (now() + interval '+2 days')") == 1);

	// A player's session refused by an admin route still slid.
	age("-120 seconds");
	reply = h.send("GET", "/api/admin/users", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(header_value(reply, "Set-Cookie") ==
	            std::string(nws::kWebSessionCookie) + "=" + token + kCookieTail);
	TEST_EXPECT(session_scalar(h, token, "expires_at > (now() + interval '+29 days')") == 1);
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

// The admin routes under each credential, on one route: none (401 + the
// Bearer challenge), a player's session (403), an admin's session (200; a
// state change needs the CSRF header, else 403), and the Bearer token (200,
// no CSRF header). The role is read on every request, so demoting the admin
// closes the routes to its live session; the role update takes only player or
// admin, as a string.
int test_admin_routes_by_credential(Harness &h) {
	TEST_EXPECT(make_account(h, "web_player", "pw-player") > 0);
	const std::string player = login_token(h, "10.7.0.1", "web_player", "pw-player");
	const int64_t admin_id = make_account(h, "web_admin", "pw-admin");
	TEST_EXPECT(!player.empty() && admin_id > 0);
	TEST_EXPECT(set_role(h, admin_id, "admin"));
	auto reply = login(h, "10.7.0.2", "web_admin", "pw-admin");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["role"]) == "admin");
	const std::string admin = set_cookie_value(reply, nws::kWebSessionCookie);
	TEST_EXPECT(!admin.empty());

	reply = h.send("GET", "/api/admin/users");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"unauthorized\"}");
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
	reply = h.send("PUT", user_path, {kJson, kBearer}, "{\"role\":7}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"invalid_field\"}");
	reply = h.send("PUT", user_path, {kJson, kBearer}, "[\"role\"]");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(body_text(reply) == "{\"error\":\"invalid_json\"}");
	reply = h.send("PUT", "/api/admin/server-status", {kJson, kBearer},
	               "{\"maintenance_enabled\":\"yes\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(set_role(h, admin_id, "player"));
	reply = h.send("GET", "/api/admin/users", {with_session(admin)});
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	return 0;
}

// Every /api/admin/* route, the ServerCommand / ServerStopHosting pushes
// included, and the admin-only GET /api/hosts and /api/unknowns: no
// credential is 401, a player's session 403 (the CSRF header
// sent, so the role is what refuses), and an admin's session without the CSRF
// header 403 on a state change and 200 on a read. Nothing a refused request
// names is touched (the user and host routes name no account or RID).
int test_admin_routes_table(Harness &h) {
	const int64_t admin_id = make_account(h, "web_table_admin", "pw-table-admin");
	TEST_EXPECT(admin_id > 0 && make_account(h, "web_table_player", "pw-table-player") > 0);
	TEST_EXPECT(set_role(h, admin_id, "admin"));
	const std::string admin = login_token(h, "10.8.1.1", "web_table_admin", "pw-table-admin");
	const std::string player = login_token(h, "10.8.1.2", "web_table_player", "pw-table-player");
	TEST_EXPECT(!admin.empty() && !player.empty());

	struct Route { const char *method; const char *path; const char *body; };
	const Route routes[] = {
		{"GET", "/api/admin/server-status", ""},
		{"PUT", "/api/admin/server-status", "{\"maintenance_enabled\":false,\"message\":\"x\"}"},
		{"POST", "/api/admin/hosts", "{\"rid\":4242}"},
		{"GET", "/api/admin/hosts", ""},
		{"GET", "/api/admin/connections", ""},
		{"GET", "/api/hosts", ""},
		{"GET", "/api/unknowns", ""},
		{"GET", "/api/admin/users", ""},
		{"POST", "/api/admin/users", "{\"username\":\"never\",\"password\":\"p\",\"pcid\":\"0badbad0\","
		                             "\"nwhandle\":\"never\"}"},
		{"DELETE", "/api/admin/users/999999", ""},
		{"PUT", "/api/admin/users/999999", "{\"nwh\":\"1\"}"},
		{"PUT", "/api/admin/users/999999/game-access",
		 "{\"game_slug\":\"jop_2_consumer\",\"status\":\"active\",\"exp_bits\":\"3\"}"},
		{"POST", "/api/admin/hosts/999999/command", "{\"verb\":\"Cycle\"}"},
		{"POST", "/api/admin/hosts/999999/stop", ""},
		{"GET", "/api/admin/cd-keys", ""},
		{"POST", "/api/admin/cd-keys", "{\"title\":\"dfx2\",\"count\":1}"},
		{"POST", "/api/admin/cd-keys/999999/assign", "{\"username\":\"web_table_player\"}"},
		{"POST", "/api/admin/cd-keys/999999/revoke", ""},
		{"POST", "/api/admin/cd-keys/999999/activations", "{\"nwpssk\":\"ABCDEFGHIJKLMNOPQRSTUVW\"}"},
		{"GET", "/api/admin/billing", ""},
		{"POST", "/api/admin/billing/reconcile", ""},
		{"POST", "/api/admin/billing/link", "{\"user_id\":1,\"stripe_customer_id\":\"cus_never\"}"},
	};
	const auto users_before = nws::list_users(*h.pool.acquire()).size();
	const auto status_before = nws::get_server_status(*h.pool.acquire());
	for (const Route &r : routes) {
		const bool changes = std::string(r.method) != "GET";
		auto check = [&r](const net::HttpReply &reply, int code, const char *what) {
			if (!reply.transport_ok || reply.code != code) {
				std::fprintf(stderr, "  %s %s (%s): %s, want %d\n", r.method, r.path, what,
				             describe(reply).c_str(), code);
				return false;
			}
			return true;
		};
		TEST_EXPECT(check(h.send(r.method, r.path, {kJson}, r.body), 401, "no credential"));
		TEST_EXPECT(check(h.send(r.method, r.path, {kJson, kCsrf, with_session(player)}, r.body), 403,
		                  "player session"));
		TEST_EXPECT(check(h.send(r.method, r.path, {kJson, with_session(admin)}, r.body),
		                  changes ? 403 : 200, "admin session, no CSRF header"));
	}
	TEST_EXPECT(nws::list_users(*h.pool.acquire()).size() == users_before);
	TEST_EXPECT(nws::get_server_status(*h.pool.acquire()).message == status_before.message);
	TEST_EXPECT(h.pool.acquire()->query("SELECT 1 FROM active_hosts WHERE rid = 4242;").empty());
	TEST_EXPECT(h.pool.acquire()->query("SELECT 1 FROM cd_keys;").empty());
	TEST_EXPECT(set_role(h, admin_id, "player"));
	return 0;
}

// ONNET_BOOTSTRAP_ADMIN (main() runs bootstrap_admin at boot): with no admin,
// the named account becomes one, its live session included; once any admin
// exists, a later boot promotes nobody, whatever name it carries.
int test_bootstrap_admin(Harness &h) {
	using Outcome = nws::BootstrapAdminResult::Outcome;
	TEST_EXPECT(admin_count(h) == 0); // the cases above demoted theirs
	TEST_EXPECT(make_account(h, "web_boss", "pw-boss") > 0);
	const std::string token = login_token(h, "10.8.0.1", "web_boss", "pw-boss");
	TEST_EXPECT(!token.empty());
	TEST_EXPECT(h.send("GET", "/api/admin/connections", {with_session(token)}).code == 403);

	TEST_EXPECT(nws::bootstrap_admin(*h.pool.acquire(), "web_nobody").outcome ==
	            Outcome::NoSuchAccount);
	TEST_EXPECT(nws::bootstrap_admin(*h.pool.acquire(), "web_boss").outcome == Outcome::Promoted);
	auto reply = h.send("GET", "/api/me", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["role"]) == "admin");
	TEST_EXPECT(h.send("GET", "/api/admin/connections", {with_session(token)}).code == 200);

	const auto again = nws::bootstrap_admin(*h.pool.acquire(), "web_player");
	TEST_EXPECT(again.outcome == Outcome::AdminExists);
	TEST_EXPECT(again.existing_admin == "web_boss");
	TEST_EXPECT(admin_count(h) == 1);
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

std::string address(const char *prefix, int i) { return prefix + std::to_string(i); }

// The CSRF header is checked before any bucket: forged requests without it,
// however many, leave the address's and the username's buckets whole.
int test_csrf_before_buckets(Harness &h) {
	TEST_EXPECT(make_account(h, "web_victim", "pw-victim") > 0);
	for (int i = 0; i < 30; ++i) {
		const auto reply = h.send("POST", "/api/login", {kJson, from_ip("10.11.0.1")},
		                          credentials("web_victim", "wrong"));
		TEST_EXPECT(reply.transport_ok && reply.code == 403);
	}
	for (int i = 0; i < 15; ++i) {
		const auto reply = h.send("POST", "/api/register", {kJson, from_ip("10.11.0.1")}, "not json");
		TEST_EXPECT(reply.transport_ok && reply.code == 403);
		TEST_EXPECT(body_text(reply) == "{\"error\":\"csrf_header_required\"}");
	}
	auto reply = login(h, "10.11.0.1", "web_victim", "pw-victim");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	reply = h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.11.0.1")}, "not json");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	return 0;
}

// The login brake. Every well-formed attempt draws on its address's bucket
// (20 at once; a malformed body is a 400 before any bucket), an IPv6 address
// counting as its /64. Each password check draws one token from the
// username's buckets and hands it back when the password is right: 10 per
// (username, address), an IPv6 address counting as its /56, so a guesser's
// address is shut out while the owner, elsewhere, still logs in; and 30 per
// username from any address the account has not logged in from, which only
// failures spread over many addresses empty. A run of right passwords drains
// nothing. Refusals are 429 with Retry-After.
int test_login_brake(Harness &h) {
	// The address bucket: a different unknown name each time, so only the
	// address is shared.
	net::HttpReply refusal;
	int accepted = accepted_before_429(
			[&h](int i) { return login(h, "10.9.0.1", address("ghost", i), "x"); }, 60, refusal);
	TEST_EXPECT(accepted >= 20);
	TEST_EXPECT(body_text(refusal) == "{\"error\":\"rate_limited\"}");
	TEST_EXPECT(is_retry_after(refusal));
	TEST_EXPECT(login(h, "10.9.0.2", "ghost-elsewhere", "x").code == 401);
	// A malformed body draws nothing, even from the spent address.
	TEST_EXPECT(h.send("POST", "/api/login", {kJson, kCsrf, from_ip("10.9.0.1")}, "{}").code == 400);
	// IPv6: every address in one /64 shares the bucket; the next /64 has its own.
	accepted = accepted_before_429(
			[&h](int i) {
				return login(h, "2001:db8:9:1::" + std::to_string(i + 1), address("ghost6-", i), "x");
			},
			60, refusal);
	TEST_EXPECT(accepted >= 20);
	TEST_EXPECT(login(h, "2001:db8:9:2::1", "ghost6-elsewhere", "x").code == 401);

	// One address guessing one account: 10 failures, then refused even with
	// the right password; the owner at another address is not.
	TEST_EXPECT(make_account(h, "web_target", "pw-target") > 0);
	accepted = accepted_before_429(
			[&h](int i) { return login(h, "10.12.0.1", "web_target", address("guess", i)); }, 30,
			refusal);
	TEST_EXPECT(accepted == 10);
	TEST_EXPECT(is_retry_after(refusal));
	TEST_EXPECT(login(h, "10.12.0.1", "web_target", "pw-target").code == 429);
	TEST_EXPECT(login(h, "10.12.0.2", "web_target", "pw-target").code == 200);

	// The same over IPv6: ten /64s inside one /56 are one guesser; another /56
	// is someone else.
	TEST_EXPECT(make_account(h, "web_target6", "pw-target6") > 0);
	accepted = accepted_before_429(
			[&h](int i) {
				return login(h, "2001:db8:56:" + std::to_string(i) + "::1", "web_target6",
				             address("guess", i));
			},
			30, refusal);
	TEST_EXPECT(accepted == 10);
	TEST_EXPECT(login(h, "2001:db8:57::1", "web_target6", "pw-target6").code == 200);

	// Right passwords drain nothing: twelve logins in a row from one address.
	TEST_EXPECT(make_account(h, "web_regular", "pw-regular") > 0);
	for (int i = 0; i < 12; ++i) {
		TEST_EXPECT(login(h, "10.12.1.1", "web_regular", "pw-regular").code == 200);
	}

	// Failures spread over addresses, five each (half the per-address limit):
	// once about 30 have landed, the per-username cap refuses an address that
	// is nowhere near its own limit.
	TEST_EXPECT(make_account(h, "web_spread", "pw-spread") > 0);
	int failures = 0;
	bool capped = false;
	for (int a = 0; a < 16 && !capped; ++a) {
		for (int i = 0; i < 5 && !capped; ++i) {
			const auto reply = login(h, address("10.12.2.", a), "web_spread", address("g", i));
			TEST_EXPECT(reply.code == 401 || reply.code == 429);
			if (reply.code == 401) ++failures;
			capped = reply.code == 429;
		}
	}
	TEST_EXPECT(capped);
	TEST_EXPECT(failures >= 30);
	return 0;
}

// POST /api/register's per-address brake: 10, then 429 with Retry-After.
int test_register_brake(Harness &h) {
	net::HttpReply refusal;
	const int accepted = accepted_before_429(
			[&h](int) {
				return h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.9.2.1")}, "not json");
			},
			40, refusal);
	TEST_EXPECT(accepted == 10);
	TEST_EXPECT(is_retry_after(refusal));
	TEST_EXPECT(h.send("POST", "/api/register", {kJson, kCsrf, from_ip("10.9.2.2")}, "not json").code ==
	            400);
	return 0;
}

// The retail POST /NWLogin.dll the way a stock client posts it: the EPASK form
// with NAME / PASSWORD encrypted, plus `headers` (a Cookie). With no name, the
// form carries no credentials (the PERSISTENTEXPRESSLOGINDATA pin path). The
// harness reaches the listener from 127.0.0.1, the TCP peer the retail login
// is braked by (no X-Real-IP counts there). nwlogin_to posts to the listener
// on `port`.
net::HttpReply nwlogin_to(Harness &h, uint16_t port, const std::string &name,
		const std::string &password, const std::vector<std::string> &headers = {}) {
	std::vector<std::string> request_headers = {"Content-Type: application/x-www-form-urlencoded"};
	request_headers.insert(request_headers.end(), headers.begin(), headers.end());
	if (name.empty())
		return h.send_to(port, "POST", "/NWLogin.dll", request_headers, "");
	const auto prepare = h.send_to(port, "GET", "/nwprepare.dll?url=jop_2_start.htm");
	opennova::EpaskParams epask;
	if (!opennova::epask_from_string(set_cookie_value(prepare, "EPASK"), epask)) return {};
	std::string body;
	if (!opennova::build_login_post_body(epask,
	                                     {{"EPASK", opennova::epask_to_string(epask), false},
	                                      {"NAME", name, true},
	                                      {"PASSWORD", password, true},
	                                      {"msgbase", "jop_2_msg.htm", true},
	                                      {"failure", "jop_2_main.htm", true},
	                                      {"pfid", "28", true}},
	                                     body)) {
		return {};
	}
	return h.send_to(port, "POST", "/NWLogin.dll", request_headers, body);
}

net::HttpReply nwlogin(Harness &h, const std::string &name, const std::string &password,
		const std::vector<std::string> &headers = {}) {
	return nwlogin_to(h, h.port, name, password, headers);
}

bool nwlogin_succeeded(const net::HttpReply &reply) {
	return reply.transport_ok && reply.code == 200 &&
	       !set_cookie_value(reply, "LOGINSESSIONTAG").empty();
}

// The retail login runs the brake on its own buckets, keyed by its TCP peer
// (a stock client reaches the HTTP port directly): an address the site's
// login is braked by (an X-Real-IP, which a Cloudflare Worker can forge
// through nginx) never brakes it. Its refusal is a retail login failure: the
// msgbase page (jop_2_msg.htm, the template a stock client already shows for a
// bad password) with its message, HTTP 200, and no login session tag. Its
// failures leave the site login alone: the two (username, address) buckets
// are apart at the same address.
int test_nwlogin_brake(Harness &h) {
	// Maintenance would answer before any password is checked.
	auto reply = h.send("PUT", "/api/admin/server-status", {kJson, kBearer},
	                    "{\"maintenance_enabled\":false}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(make_account(h, "web_retail", "pw-retail") > 0);
	TEST_EXPECT(make_account(h, "web_retail_flood", "pw-flood") > 0);

	const auto bad = nwlogin(h, "web_retail", "guess0");
	TEST_EXPECT(bad.transport_ok && bad.code == 200);
	const std::string bad_page = body_text(bad);
	TEST_EXPECT(bad_page.find("NovaWorld Message") != std::string::npos);
	TEST_EXPECT(bad_page.find("Invalid username or password") != std::string::npos);
	TEST_EXPECT(set_cookie_value(bad, "LOGINSESSIONTAG").empty());
	for (int i = 1; i < 10; ++i) {
		const auto r = nwlogin(h, "web_retail", address("guess", i));
		TEST_EXPECT(r.transport_ok && body_text(r) == bad_page);
	}

	const auto refused = nwlogin(h, "web_retail", "pw-retail");
	TEST_EXPECT(refused.transport_ok && refused.code == 200);
	TEST_EXPECT(header_value(refused, "Content-Type") == header_value(bad, "Content-Type"));
	TEST_EXPECT(set_cookie_value(refused, "LOGINSESSIONTAG").empty());
	std::string expected = bad_page;
	const std::string bad_message = "Invalid username or password";
	expected.replace(expected.find(bad_message), bad_message.size(),
	                 "Too many failed logins. Please try again later.");
	TEST_EXPECT(body_text(refused) == expected);

	// The site login keeps its own (username, address) bucket: the same account
	// from the same address, 127.0.0.1 named in X-Real-IP, is let in.
	TEST_EXPECT(login(h, "127.0.0.1", "web_retail", "pw-retail").code == 200);

	// A flood on the site's login naming the retail client's address (its
	// bucket of 20, less the login above)...
	net::HttpReply refusal;
	const int accepted = accepted_before_429(
			[&h](int i) { return login(h, "127.0.0.1", address("forged", i), "x"); }, 60, refusal);
	TEST_EXPECT(accepted >= 19);
	// ...brakes nothing on the retail login from it.
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "web_retail_flood", "pw-flood")));
	return 0;
}

// An account's known addresses (a right password in the last 30 days, on
// either login) are spared the per-username cap: once strangers' failures have
// emptied it, the owner still gets in from them, on the site and in the game,
// while the strangers' fresh addresses are refused.
int test_known_addresses(Harness &h) {
	TEST_EXPECT(make_account(h, "web_known", "pw-known") > 0);
	TEST_EXPECT(login(h, "10.15.0.1", "web_known", "pw-known").code == 200);
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "web_known", "pw-known"))); // from 127.0.0.1

	int failures = 0;
	bool capped = false;
	for (int a = 0; a < 16 && !capped; ++a) {
		for (int i = 0; i < 5 && !capped; ++i) {
			const auto reply = login(h, address("10.15.1.", a), "web_known", address("g", i));
			TEST_EXPECT(reply.code == 401 || reply.code == 429);
			if (reply.code == 401) ++failures;
			capped = reply.code == 429;
		}
	}
	TEST_EXPECT(capped && failures >= 30);
	TEST_EXPECT(login(h, "10.15.0.1", "web_known", "pw-known").code == 200);
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "web_known", "pw-known")));
	return 0;
}

// A retail login carrying PERSISTENTEXPRESSLOGINDATA pins that cookie to the
// account, and a later POST with the cookie alone resumes it with no
// password. An admin password reset, a status other than 'active' (even one
// lifted again) and a deleted account drop the pin.
int test_retail_pin_revocation(Harness &h) {
	const int64_t id = make_account(h, "web_pinned", "pw-pinned");
	TEST_EXPECT(id > 0);
	const std::string user_path = "/api/admin/users/" + std::to_string(id);
	const std::vector<std::string> cookie = {"Cookie: PERSISTENTEXPRESSLOGINDATA=pin-web-pinned"};
	auto refused_without_password = [&h, &cookie] {
		const auto reply = nwlogin(h, "", "", cookie);
		return reply.transport_ok && reply.code == 200 &&
		       set_cookie_value(reply, "LOGINSESSIONTAG").empty() &&
		       body_text(reply).find("Invalid username or password") != std::string::npos;
	};
	auto admin_put = [&h, &user_path](const std::string &body) {
		const auto reply = h.send("PUT", user_path, {kJson, kBearer}, body);
		return reply.transport_ok && reply.code == 200;
	};

	TEST_EXPECT(refused_without_password()); // no pin yet
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "web_pinned", "pw-pinned", cookie)));
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "", "", cookie))); // the pin resumes it
	TEST_EXPECT(admin_put("{\"password\":\"pw-pinned-2\"}"));
	TEST_EXPECT(refused_without_password());

	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "web_pinned", "pw-pinned-2", cookie)));
	TEST_EXPECT(admin_put("{\"account_status\":\"banned\"}"));
	TEST_EXPECT(admin_put("{\"account_status\":\"active\"}"));
	TEST_EXPECT(refused_without_password());

	// A rename (the username) keeps the pin, and so does a new handle.
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "web_pinned", "pw-pinned-2", cookie)));
	TEST_EXPECT(admin_put("{\"username\":\"web_pinned_renamed\"}"));
	TEST_EXPECT(nws::get_user_by_id(*h.pool.acquire(), id)->username == "web_pinned_renamed");
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "", "", cookie)));
	TEST_EXPECT(admin_put("{\"nwhandle\":\"Pinned\"}"));
	TEST_EXPECT(nwlogin_succeeded(nwlogin(h, "", "", cookie)));
	const auto deleted = h.send("DELETE", user_path, {kBearer});
	TEST_EXPECT(deleted.transport_ok && deleted.code == 204);
	TEST_EXPECT(refused_without_password());
	return 0;
}

// The second listener trusts no proxy: an X-Real-IP from its peer is the
// client's own claim, so registrations that each name a fresh address all
// draw on the peer's one bucket: exactly 10, then 429.
int test_untrusted_proxy_header_ignored(Harness &h) {
	net::HttpReply refusal;
	const int accepted = accepted_before_429(
			[&h](int i) {
				return h.send_to(h.secure_port, "POST", "/api/register",
				                 {kJson, kCsrf, from_ip(address("10.10.0.", i))}, "not json");
			},
			40, refusal);
	TEST_EXPECT(accepted == 10);
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
// The host writes run back to back, each taking the writer lock moments after
// the last COMMIT; a registration's Transaction queues on that lock between
// them, within the server's own lock timeout, which the pool keeps.
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
			                          {kJson, kCsrf, from_ip("10.20.0." + std::to_string(i))},
			                          "{\"username\":\"" + username + "\",\"password\":\"pw\",\"email\":\"" +
			                                  username + "@example.com\"}",
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
				const auto hosts = h.send("GET", "/api/hosts", {kBearer}, {}, kConcurrentTimeoutMs);
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

// A stock client's two /NWHost.dll calls: the page link's first call (its jar
// in `cookies`), then the relay page's bare refresh with the NWJOINSESSIONTAG
// it set. `key` is the HOSTKEY the session store minted for the tag.
struct HostFetch {
	net::HttpReply first;
	net::HttpReply second;
	std::string key;
};

HostFetch fetch_nwhost(Harness &h, const std::string &cookies) {
	HostFetch f;
	std::vector<std::string> headers;
	if (!cookies.empty()) headers.push_back("Cookie: " + cookies);
	f.first = h.send("GET", "/NWHost.dll?success=jop_2_host2.htm&pfid=28", headers);
	const std::string tag = set_cookie_value(f.first, "NWJOINSESSIONTAG");
	if (const auto session = h.sessions.get_host(tag)) f.key = session->host_key;
	const std::string jar = (cookies.empty() ? "" : cookies + "; ") + "NWJOINSESSIONTAG=" + tag;
	f.second = h.send("GET", "/NWHost.dll", {"Cookie: " + jar});
	return f;
}

// The account host_keys records for `key` (by its hash), nullopt when none.
std::optional<int64_t> key_owner(Harness &h, const std::string &key) {
	const auto rows = h.pool.acquire()->query("SELECT user_id FROM host_keys WHERE key_hash = $1;",
	                                          {opennova::db::BindValue(opennova::hostdb::host_key_hash(key))});
	if (rows.empty()) return std::nullopt;
	return rows.front().as_int(0);
}

// The cookie names a reply sets, in order.
std::vector<std::string> set_cookie_names(const net::HttpReply &reply) {
	std::vector<std::string> names;
	for (const std::string &value : header_values(reply, "Set-Cookie")) {
		names.push_back(value.substr(0, value.find('=')));
	}
	return names;
}

// /NWHost.dll records the HOSTKEY it mints, as its SHA-256, for the account the
// request's own retail login names: the LOGINSESSIONTAG of a live retail login,
// or the PERSISTENTEXPRESSLOGINDATA pin. No login (or only an NWHANDLE, which any
// client can set to any account's, or a dead tag) records nothing. Either way the
// relay page and the HOSTKEY page are the same bytes a stock client parses. A
// password reset ends the retail login's row and the account's keys.
int test_nwhost_records_owner(Harness &h) {
	const int64_t id = make_account(h, "host_owner", "pw-owner");
	TEST_EXPECT(id > 0);
	const auto logged_in = nwlogin(h, "host_owner", "pw-owner",
	                               {"Cookie: PERSISTENTEXPRESSLOGINDATA=pin-host-owner"});
	TEST_EXPECT(nwlogin_succeeded(logged_in));
	const std::string login_tag = set_cookie_value(logged_in, "LOGINSESSIONTAG");

	// The jar a stock client carries after its login.
	const auto owned = fetch_nwhost(h, "LOGINSESSIONTAG=" + login_tag + "; NWHANDLE=host_owner");
	TEST_EXPECT(opennova::hostdb::is_host_key(owned.key));
	TEST_EXPECT(key_owner(h, owned.key) == id);
	TEST_EXPECT(h.pool.acquire()
	                    ->query("SELECT 1 FROM host_keys WHERE key_hash = $1;",
	                            {opennova::db::BindValue(owned.key)})
	                    .empty());
	const auto pinned = fetch_nwhost(h, "PERSISTENTEXPRESSLOGINDATA=pin-host-owner");
	TEST_EXPECT(key_owner(h, pinned.key) == id);

	const auto anonymous = fetch_nwhost(h, "");
	TEST_EXPECT(opennova::hostdb::is_host_key(anonymous.key) && !key_owner(h, anonymous.key));
	const auto forged = fetch_nwhost(h, "NWHANDLE=host_owner; CHAR=host_owner");
	TEST_EXPECT(!forged.key.empty() && !key_owner(h, forged.key));
	const auto dead = fetch_nwhost(h, "LOGINSESSIONTAG=NWServer:NWLogin.dll:SESSIONTAG:1:0badf00d; "
	                                  "PERSISTENTEXPRESSLOGINDATA=pin-nobody");
	TEST_EXPECT(!dead.key.empty() && !key_owner(h, dead.key));

	// The same bytes whatever the login: the relay page whole, the HOSTKEY page
	// as the template with the key, the same cookies in the same order.
	const std::string host2 = test_io::read_file_text(
			(std::filesystem::path(OPENNOVA_SOURCE_DIR) / "apps" / "novaworld_server" / "templates" /
			 "jop_2_host2.htm")
					.string());
	auto with_key = [&host2](const std::string &key) {
		std::string page = host2;
		page.replace(page.find("{{HOSTKEY}}"), std::string("{{HOSTKEY}}").size(), key);
		return page;
	};
	for (const HostFetch *f : {&owned, &pinned, &anonymous, &forged}) {
		TEST_EXPECT(f->first.transport_ok && f->first.code == 200);
		TEST_EXPECT(f->second.transport_ok && f->second.code == 200);
		TEST_EXPECT(body_text(f->first) == body_text(anonymous.first));
		TEST_EXPECT(body_text(f->second) == with_key(f->key));
		TEST_EXPECT(set_cookie_names(f->first) == set_cookie_names(anonymous.first));
		TEST_EXPECT(set_cookie_names(f->second) == set_cookie_names(anonymous.second));
	}
	TEST_EXPECT(body_text(anonymous.first).find("NWHost.dll") != std::string::npos);

	// A password reset ends the retail login's row, the pin and the keys.
	const auto reset = h.send("PUT", "/api/admin/users/" + std::to_string(id), {kJson, kBearer},
	                          "{\"password\":\"pw-owner-2\"}");
	TEST_EXPECT(reset.transport_ok && reset.code == 200);
	TEST_EXPECT(!key_owner(h, owned.key) && !key_owner(h, pinned.key));
	const auto after = fetch_nwhost(h, "LOGINSESSIONTAG=" + login_tag +
	                                       "; PERSISTENTEXPRESSLOGINDATA=pin-host-owner");
	TEST_EXPECT(!after.key.empty() && !key_owner(h, after.key));
	return 0;
}

// GET /api/me/hosts lists the logged-in account's listed servers, and no
// other's; with no live session it is 401. GET /api/admin/hosts lists every
// listed server with its owner (null for an anonymous one), for an admin. The
// public /api/lobbies names no owner.
int test_listed_server_owners(Harness &h) {
	const int64_t me = make_account(h, "hosts_me", "pw-hosts-me");
	const int64_t other = make_account(h, "hosts_other", "pw-hosts-other");
	TEST_EXPECT(me > 0 && other > 0);

	auto reply = h.send("GET", "/api/me/hosts");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("GET", "/api/me/hosts", {with_session(std::string(64, 'a'))});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	const std::string token = login_token(h, "10.22.0.1", "hosts_me", "pw-hosts-me");
	TEST_EXPECT(!token.empty());
	reply = h.send("GET", "/api/me/hosts", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["hosts"].size() == 0);

	struct Listed {
		uint32_t rid;
		const char *name;
		std::optional<int64_t> owner;
	};
	const Listed listed[] = {{2001, "Mine", me}, {2002, "Theirs", other}, {2003, "Nobody's", {}}};
	for (const Listed &l : listed) {
		opennova::hostdb::HostRow row;
		row.rid = l.rid;
		row.gsid = "GSID-" + std::to_string(l.rid);
		row.game = "jop_2_consumer";
		row.server_name = l.name;
		row.mission_name = "ASH_G11A";
		row.game_type = "TDM";
		row.host_ip = "10.0.0.9";
		row.host_port = 32768;
		row.player_count = 3;
		row.max_players = 24;
		row.peer_ip = "10.0.0.9";
		// The owner as the lobby stores one: a key minted for it, claimed for
		// the row's RID.
		auto conn = h.pool.acquire();
		if (l.owner) {
			const std::string key(opennova::hostdb::kHostKeyChars,
			                      static_cast<char>('A' + (l.rid - 2001)));
			TEST_EXPECT(opennova::hostdb::record_host_key(
					*conn, key, *l.owner, nws::get_user_by_id(*conn, *l.owner)->credentials_gen));
			row.host_user_id = opennova::hostdb::claim_host_key(*conn, key, l.rid);
			TEST_EXPECT(row.host_user_id == l.owner);
		}
		opennova::hostdb::upsert_host(*conn, row);
	}

	reply = h.send("GET", "/api/me/hosts", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["hosts"].size() == 1);
	const auto &mine = json["hosts"][0];
	TEST_EXPECT(mine["rid"].i() == 2001);
	TEST_EXPECT(str(mine["server_name"]) == "Mine");
	TEST_EXPECT(str(mine["mission_name"]) == "ASH_G11A");
	TEST_EXPECT(str(mine["game"]) == "jop_2_consumer");
	TEST_EXPECT(mine["players"].i() == 3 && mine["max_players"].i() == 24);
	TEST_EXPECT(str(mine["host_ip"]) == "10.0.0.9" && mine["host_port"].i() == 32768);
	TEST_EXPECT(str(mine["peer_ip"]) == "10.0.0.9");
	TEST_EXPECT(str(mine["created_at"]).size() == 19 && str(mine["updated_at"]).size() == 19);
	TEST_EXPECT(!mine.has("owner"));
	TEST_EXPECT(mine["roster"].t() == crow::json::type::List && mine["roster"].size() == 0);

	// The admin view: the Bearer token or an admin session; a player is refused.
	reply = h.send("GET", "/api/admin/hosts");
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("GET", "/api/admin/hosts", {with_session(token)});
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	reply = h.send("GET", "/api/admin/hosts", {kBearer});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["count"].i() == static_cast<int64_t>(json["hosts"].size()));
	int seen = 0;
	for (const auto &e : json["hosts"]) {
		const int64_t rid = e["rid"].i();
		if (rid == 2001 || rid == 2002) {
			++seen;
			TEST_EXPECT(e["owner"].t() == crow::json::type::Object);
			TEST_EXPECT(e["owner"]["id"].i() == (rid == 2001 ? me : other));
			TEST_EXPECT(str(e["owner"]["username"]) == (rid == 2001 ? "hosts_me" : "hosts_other"));
		} else {
			TEST_EXPECT(e["owner"].t() == crow::json::type::Null);
			if (rid == 2003) ++seen;
		}
	}
	TEST_EXPECT(seen == 3);

	// The public list names no owner.
	{
		reply = h.send("GET", "/api/lobbies");
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		const std::string text = body_text(reply);
		TEST_EXPECT(text.find("Mine") != std::string::npos);
		TEST_EXPECT(text.find("hosts_me") == std::string::npos);
		TEST_EXPECT(text.find("hosts_other") == std::string::npos);
		TEST_EXPECT(text.find("owner") == std::string::npos);
		TEST_EXPECT(text.find("host_user_id") == std::string::npos);
	}

	// A deleted owner's listing stays listed, with no owner.
	const auto deleted = h.send("DELETE", "/api/admin/users/" + std::to_string(other), {kBearer});
	TEST_EXPECT(deleted.transport_ok && deleted.code == 204);
	const auto orphan = opennova::hostdb::find_host_by_rid(*h.pool.acquire(), 2002);
	TEST_EXPECT(orphan && !orphan->host_user_id);

	for (const Listed &l : listed) opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), l.rid);
	return 0;
}

// GET /api/hosts names each joiner /NWJoin.dll recorded (its handle, account id
// and address) and GET /api/unknowns each sample's sender, so both are admin
// routes: 401 with the Bearer challenge and no credential, 403 for a player's
// session, 200 for the admin, whose /api/hosts carries the joiner. The public
// /api/lobbies lists the host and names no joiner.
int test_joiner_routes_admin_only(Harness &h) {
	const int64_t joiner = make_account(h, "joiner_seen", "pw-joiner-seen");
	TEST_EXPECT(joiner > 0);
	const std::string player = login_token(h, "10.24.0.1", "joiner_seen", "pw-joiner-seen");
	TEST_EXPECT(!player.empty());

	constexpr uint32_t kRid = 2101;
	const std::string joiner_ip = "198.51.100.77";
	{
		auto conn = h.pool.acquire();
		opennova::hostdb::HostRow row;
		row.rid = kRid;
		row.gsid = "GSID-2101";
		row.game = "jop_2_consumer";
		row.server_name = "Joined Host";
		row.host_ip = "10.0.0.11";
		row.host_port = 32768;
		row.player_count = 1;
		row.max_players = 16;
		row.peer_ip = "10.0.0.11";
		opennova::hostdb::upsert_host(*conn, row);
		// The joiner as /NWJoin.dll records one.
		opennova::hostdb::PlayerRow p;
		p.host_rid = kRid;
		p.user_id = joiner;
		p.nwhandle = "joiner_seen";
		p.peer_ip = joiner_ip;
		p.peer_port = 4242;
		opennova::hostdb::add_player(*conn, p);
	}

	for (const char *path : {"/api/hosts", "/api/unknowns"}) {
		auto reply = h.send("GET", path);
		TEST_EXPECT(reply.transport_ok && reply.code == 401);
		TEST_EXPECT(has_header(reply, "WWW-Authenticate"));
		TEST_EXPECT(body_text(reply).find(joiner_ip) == std::string::npos);
		reply = h.send("GET", path, {with_session(player)});
		TEST_EXPECT(reply.transport_ok && reply.code == 403);
		TEST_EXPECT(body_text(reply).find(joiner_ip) == std::string::npos);
		reply = h.send("GET", path, {kBearer});
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		TEST_EXPECT(header_value(reply, "Content-Type") == "application/json");
	}

	const auto json = crow::json::load(body_text(h.send("GET", "/api/hosts", {kBearer})));
	TEST_EXPECT(json);
	bool seen = false;
	for (const auto &e : json["hosts"]) {
		if (e["rid"].i() != kRid) continue;
		seen = true;
		TEST_EXPECT(e["player_list"].size() == 1);
		const auto &p = e["player_list"][0];
		TEST_EXPECT(str(p["nwhandle"]) == "joiner_seen");
		TEST_EXPECT(p["user_id"].i() == joiner);
		TEST_EXPECT(str(p["peer_ip"]) == joiner_ip);
	}
	TEST_EXPECT(seen);

	const auto lobbies = h.send("GET", "/api/lobbies");
	TEST_EXPECT(lobbies.transport_ok && lobbies.code == 200);
	const std::string text = body_text(lobbies);
	TEST_EXPECT(text.find("Joined Host") != std::string::npos);
	TEST_EXPECT(text.find(joiner_ip) == std::string::npos);
	TEST_EXPECT(text.find("joiner_seen") == std::string::npos);
	TEST_EXPECT(text.find("user_id") == std::string::npos);

	opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), kRid);
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

// One lobby per title, over HTTP. /api/games lists every games row; each
// row's .gsb route lists the hosts registered under its LobbyName alone, a
// column they reported nothing for showing the row's VER1 / Expbits; and a
// retail page's GSB_SERVER is the requesting client's game's: the gate tag
// /nwprepare.dll carries (and echoes as the GT cookie), else the GT cookie,
// else the NWPF cookie's product family, else the page's prefix, else JO's.
int test_lobby_per_title(Harness &h) {
	auto reply = h.send("GET", "/api/games");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const auto games = crow::json::load(body_text(reply));
	TEST_EXPECT(games && games["games"].size() == 3);
	TEST_EXPECT(str(games["games"][2]["slug"]) == "opennova_0_consumer");
	TEST_EXPECT(str(games["games"][2]["gateTag"]) == "opennova:0");
	TEST_EXPECT(str(games["games"][2]["lobbyName"]) == "opennova_0_consumer");
	TEST_EXPECT(str(games["games"][2]["title"]) == "jo");
	TEST_EXPECT(str(games["games"][0]["title"]) == "dfx2");

	// One host per lobby, none reporting VER1 or Expbits.
	struct Listed {
		uint32_t rid;
		const char *lobby;
		const char *name;
	};
	const Listed listed[] = {
		{ 3101, "jop_2_consumer", "Title JO" },
		{ 3102, "dfx2_0_consumer", "Title DFX2" },
		{ 3103, "opennova_0_consumer", "Title ON" },
	};
	for (const Listed &l : listed) {
		opennova::hostdb::HostRow row;
		row.rid = l.rid;
		row.gsid = std::string("GSID-") + l.name;
		row.game = l.lobby;
		row.server_name = l.name;
		row.host_ip = "10.0.31.1";
		row.host_port = 32768;
		opennova::hostdb::upsert_host(*h.pool.acquire(), row);
	}
	struct Route {
		const char *path;
		const char *name;
		const char *ver1;
		const char *exp_bits;
	};
	const Route routes[] = {
		{ "/jop_2.gsb", "Title JO", "3", "3" },
		{ "/jop_2.gsb?a=1", "Title JO", "3", "3" },
		{ "/dfx2_0.gsb", "Title DFX2", "1", "1" },
		{ "/opennova_0.gsb?a=1", "Title ON", "3", "1" },
	};
	for (const Route &r : routes) {
		const auto gsb = h.send("GET", r.path);
		if (!gsb.transport_ok || gsb.code != 200)
			std::fprintf(stderr, "  %s: %s\n", r.path, describe(gsb).c_str());
		TEST_EXPECT(gsb.transport_ok && gsb.code == 200);
		opennova::GsbResponse parsed;
		TEST_EXPECT(opennova::gsb_parse_response(gsb.body.data(), gsb.body.size(), parsed));
		int titled = 0;
		for (const auto &s : parsed.servers) {
			if (s.server_name.rfind("Title ", 0) != 0)
				continue;
			++titled;
			TEST_EXPECT(s.server_name == r.name);
			TEST_EXPECT(s.ver1 == r.ver1);
			TEST_EXPECT(s.exp_bits == r.exp_bits);
		}
		if (titled != 1)
			std::fprintf(stderr, "  %s listed %d titled hosts\n", r.path, titled);
		TEST_EXPECT(titled == 1);
	}
	for (const Listed &l : listed)
		opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), l.rid);

	// The menu URL page as each client's game fills it.
	const std::string base = "http://127.0.0.1:" + std::to_string(h.port);
	const auto urls = [&base](const char *gsb) { return base + "/nwhost.dll|" + base + gsb; };
	struct Prepared {
		const char *gt;
		const char *gsb;
	};
	const Prepared prepared[] = {
		{ "jop:cus2", "/jop_2.gsb" },
		{ "jopd:cus4", "/jop_2.gsb" },
		{ "dfx2:0:cus:buffy", "/dfx2_0.gsb" },
		{ "opennova:0", "/opennova_0.gsb" },
		{ "nobody:1", "/jop_2.gsb" },
	};
	for (const Prepared &p : prepared) {
		reply = h.send("GET", std::string("/nwprepare.dll?url=") + kUrlsTemplate + "&gt=" + p.gt);
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		if (body_text(reply) != urls(p.gsb))
			std::fprintf(stderr, "  gt=%s: %s\n", p.gt, body_text(reply).c_str());
		TEST_EXPECT(body_text(reply) == urls(p.gsb));
		TEST_EXPECT(set_cookie_value(reply, "GT") == p.gt);
	}

	// Later pages carry no gt: the GT cookie /nwprepare.dll set names the
	// game, then the product family, then the page. A DFX2-prefixed copy of
	// the URL page stands in for a DFX2 page.
	const std::string urls_page = "{{HOST_URL}}|{{GSB_SERVER}}";
	TEST_EXPECT(test_io::write_file((h.temp_root / "templates" / "dfx2_0_urls.htm").string(),
			std::vector<uint8_t>(urls_page.begin(), urls_page.end())));
	struct Later {
		const char *cookie;
		const char *page;
		const char *gsb;
	};
	const Later later[] = {
		{ "GT=opennova:0", "urls.htm", "/opennova_0.gsb" },
		{ "GT=opennova:0", "dfx2_0_urls.htm", "/opennova_0.gsb" },
		{ "GT=jopd:cus4", "urls.htm", "/jop_2.gsb" },
		{ "NWPF=38", "urls.htm", "/dfx2_0.gsb" },
		{ "NWPF=28", "urls.htm", "/jop_2.gsb" },
		{ nullptr, "dfx2_0_urls.htm", "/dfx2_0.gsb" },
		{ nullptr, "urls.htm", "/jop_2.gsb" },
	};
	for (const Later &l : later) {
		std::vector<std::string> headers;
		if (l.cookie)
			headers.push_back(std::string("Cookie: ") + l.cookie);
		reply = h.send("GET", std::string("/NWAccount.dll?success=") + l.page, headers);
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		if (body_text(reply) != urls(l.gsb))
			std::fprintf(stderr, "  %s %s: %s\n", l.cookie ? l.cookie : "(no cookie)", l.page,
					body_text(reply).c_str());
		TEST_EXPECT(body_text(reply) == urls(l.gsb));
	}

	// A page the request does not name is its game's: /nwprepare.dll with no
	// url renders the gate tag's start page.
	const auto start_of = [&h](const std::string &query) {
		return body_text(h.send_to(h.secure_port, "GET", "/nwprepare.dll?" + query));
	};
	const std::string dfx2_start = start_of("url=dfx2_0_start.htm");
	const std::string jo_start = start_of("url=jop_2_start.htm");
	TEST_EXPECT(!dfx2_start.empty() && dfx2_start != jo_start);
	TEST_EXPECT(start_of("gt=dfx2:0:cus:buffy") == dfx2_start);
	TEST_EXPECT(start_of("gt=opennova:0") == jo_start);
	TEST_EXPECT(start_of("gt=jop:cus2") == jo_start);
	return 0;
}

// A client's cookie jar: every cookie a reply sets, sent back on each later
// request, as a stock client does.
struct CookieJar {
	std::map<std::string, std::string> cookies;

	void keep(const net::HttpReply &reply) {
		for (const std::string &line : header_values(reply, "Set-Cookie")) {
			const auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			const auto semi = line.find(';', eq);
			cookies[line.substr(0, eq)] =
					line.substr(eq + 1, (semi == std::string::npos ? line.size() : semi) - eq - 1);
		}
	}
	std::vector<std::string> header(const char *without = nullptr) const {
		std::string text;
		for (const auto &[name, value] : cookies) {
			if (without && name == without)
				continue;
			text += (text.empty() ? "" : "; ") + name + "=" + value;
		}
		return { "Cookie: " + text };
	}
};

// A retail dfx2.exe's whole path through the service, the one NovaWorld a
// DFX2 client has: the gate's startupurl page (dfx2_0_start.htm, the DFX2
// gate tag in its gt), NWStart's DFX2 login page, the login form that page
// posts (pfid 38, every field EPASK-encrypted) for an account with no DFX2
// grant of its own (a missing access row is the game's default access, its
// expansion bits DFX2's 1), its failure on the DFX2 message page, the main
// page naming the DFX2 browser list, that list holding the DFX2 listing
// alone, the join of it (the DFX2 .joi, NWPF 38), and a DFX2 host's key on
// the DFX2 host page, by its page link and by the bare HOST_URL.
int test_dfx2_client_path(Harness &h) {
	const uint16_t port = h.secure_port;
	const std::string base = "http://127.0.0.1:" + std::to_string(port);
	const auto page = [&h](const char *name) {
		return test_io::read_file_text((h.retail_root / "templates" / name).string());
	};
	CookieJar jar;

	auto reply = h.send_to(port, "GET",
			"/nwprepare.dll?ver1=1&ver2=9013&cc=us&pfid=38&gt=dfx2:0:cus:buffy&url=dfx2_0_start."
			"htm");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(body_text(reply) == page("dfx2_0_start.htm"));
	TEST_EXPECT(set_cookie_value(reply, "GT") == "dfx2:0:cus:buffy");
	jar.keep(reply);
	// An activated copy (dfx2_activation covers one that is not): the wire key its boot read
	// decoded and its machine's fingerprint, the browser variables its NovaWorld connect set.
	jar.cookies["NWCDKIID"] = opennova::cdkey::make_wire_key("DFX2-PATH-0001");
	jar.cookies["NWCDKIIDEXP1"] = "";
	jar.cookies["NWPSSK"] = "ABCDEFGHIJKLMNOPQRSTUVW";

	reply = h.send_to(port, "GET",
			"/NWStart.dll?MSGBASE=dfx2_0_msg.htm&IN=dfx2_0_main.htm&OUT=dfx2_0_login.htm"
			"&ACT=dfx2_0_actlogin.htm&verfile=dfx2_0.ver"
			"&newupdateavailable=dfx2_0_newupdateavailable.htm"
			"&newupdateavailablewithbypass=dfx2_0_newupdateavailable2.htm"
			"&junction=dfx2_0_junction.htm",
			jar.header());
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(body_text(reply) == page("dfx2_0_login.htm"));
	jar.keep(reply);

	// The DFX2 login form, as dfx2_0_login.htm has the client post it.
	TEST_EXPECT(make_account(h, "dfx2_player", "pw-dfx2") > 0);
	opennova::EpaskParams epask;
	TEST_EXPECT(opennova::epask_from_string(jar.cookies["EPASK"], epask));
	const auto login = [&](const std::string &password) {
		std::string body;
		const bool built = opennova::build_login_post_body(epask,
				{ { "EPASK", opennova::epask_to_string(epask), false },
						{ "NAME", "dfx2_player", true }, { "PASSWORD", password, true },
						{ "success", "dfx2_0_main.htm", true },
						{ "failure", "dfx2_0_login.htm", true },
						{ "enterkey", "dfx2_0_term.htm", true },
						{ "msgbase", "dfx2_0_msg.htm", true },
						{ "relay", "dfx2_0_relay.htm", true }, { "nodb", "dfx2_0_nodb.htm", true },
						{ "needtoagree", "dfx2_0_needtoagree.htm", true }, { "pfid", "38", true },
						{ "rememberlogin", "1", true },
						{ "charglsvssrequest", "[dfx]kills", true } },
				body);
		auto headers = jar.header();
		headers.push_back("Content-Type: application/x-www-form-urlencoded");
		return built ? h.send_to(port, "POST", "/NWLogin.dll", headers, body) : net::HttpReply{};
	};
	reply = login("wrong-pw");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(set_cookie_value(reply, "LOGINSESSIONTAG").empty());
	TEST_EXPECT(body_text(reply) ==
			nws::render_template(
					page("dfx2_0_msg.htm"), { { "MESSAGE", "Invalid username or password" } }));
	reply = login("pw-dfx2");
	TEST_EXPECT(nwlogin_succeeded(reply));
	TEST_EXPECT(set_cookie_value(reply, "EXPBITS") == "1");
	TEST_EXPECT(body_text(reply) ==
			nws::render_template(page("dfx2_0_relay.htm"),
					{ { "MESSAGE", "Contacting login databases..." },
							{ "REFRESH_ENDPOINT", "NWLogin.dll" } }));
	jar.keep(reply);

	// The relay's refresh: the DFX2 main page, its browser list DFX2's.
	reply = h.send_to(port, "GET", "/NWLogin.dll", jar.header());
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(body_text(reply) ==
			nws::render_template(page("dfx2_0_main.htm"),
					{ { "HOST_URL", base + "/nwhost.dll" }, { "GSB_SERVER", base + "/dfx2_0.gsb" },
							{ "JOINLAN_URL", "" } }));
	TEST_EXPECT(body_text(reply).find(base + "/dfx2_0.gsb</IB3_SUBST>") != std::string::npos);
	TEST_EXPECT(set_cookie_value(reply, "EXPBITS") == "1");
	// Activated already: no NWACT, and no key minted for the account.
	TEST_EXPECT(!sets_cookie(reply, "NWACT"));
	TEST_EXPECT(h.pool.acquire()
					->query("SELECT 1 FROM cd_keys k JOIN players p ON p.id = k.user_id "
							"WHERE p.username = 'dfx2_player';")
					.empty());
	jar.keep(reply);

	// The browser list: a DFX2 listing, and a JO one it never shows.
	for (const auto &[rid, lobby] : { std::pair<uint32_t, const char *>{ 3201, "dfx2_0_consumer" },
				 { 3202, "jop_2_consumer" } }) {
		opennova::hostdb::HostRow row;
		row.rid = rid;
		row.gsid = "GSID-" + std::to_string(rid);
		row.game = lobby;
		row.app_id = "20";
		row.server_name = std::string("Path ") + lobby;
		row.host_ip = "10.0.32.1";
		row.host_port = 32768;
		row.pcid_key = "pcid-key-dfx2-path";
		opennova::hostdb::upsert_host(*h.pool.acquire(), row);
	}
	reply = h.send_to(port, "GET", "/dfx2_0.gsb?a=1", jar.header());
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	opennova::GsbResponse listed;
	TEST_EXPECT(opennova::gsb_parse_response(reply.body.data(), reply.body.size(), listed));
	bool dfx2_listed = false;
	for (const auto &s : listed.servers) {
		TEST_EXPECT(s.rid != 3202);
		dfx2_listed = dfx2_listed || s.rid == 3201;
	}
	TEST_EXPECT(dfx2_listed);

	// The join, by the main menu's GLB_JOIN link.
	reply = h.send_to(port, "GET",
			"/NWJoin.dll?lan=0&needexpkey=dfx2_0_key2err.htm&success=dfx2_0_join.joi"
			"&failure=dfx2_0_main.htm&relay=dfx2_0_relay.htm&msgbase=dfx2_0_msg.htm"
			"&nodb=dfx2_0_nodb.htm&pfid=38&mode=Login&rid=3201",
			jar.header());
	TEST_EXPECT(reply.transport_ok && reply.code == 200 &&
			!set_cookie_value(reply, "NWJOINSESSIONTAG").empty());
	TEST_EXPECT(body_text(reply) ==
			nws::render_template(page("dfx2_0_relay.htm"),
					{ { "MESSAGE", "Contacting game server...." },
							{ "REFRESH_ENDPOINT", "NWJoin.dll" } }));
	jar.keep(reply);
	reply = h.send_to(port, "GET", "/NWJoin.dll", jar.header());
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const std::string joi = body_text(reply);
	if (joi.find("&NI=10.0.32.1&NP=32768&BK=986119&]") == std::string::npos)
		std::fprintf(stderr, "  the DFX2 join: %s\n", describe(reply).c_str());
	TEST_EXPECT(joi.find("&NI=10.0.32.1&NP=32768&BK=986119&]") != std::string::npos);
	TEST_EXPECT(joi.find("URL=dfx2_0_jend.htm") != std::string::npos);
	TEST_EXPECT(!set_cookie_value(reply, "PUBPCID").empty());
	TEST_EXPECT(set_cookie_value(reply, "NWPF") == "38");
	jar.keep(reply);

	// Hosting: dfx2_0_host1.htm's link, then the bare HOST_URL (a jar with
	// no relay tag in it), each DFX2's host page with its key and NWPF 38.
	const auto hosted_key = [&](const std::string &first_leg) {
		auto first = h.send_to(port, "GET", first_leg, jar.header("NWJOINSESSIONTAG"));
		TEST_EXPECT(first.transport_ok && first.code == 200 &&
				!set_cookie_value(first, "NWJOINSESSIONTAG").empty());
		TEST_EXPECT(body_text(first) ==
				nws::render_template(page("dfx2_0_relay.htm"),
						{ { "MESSAGE", "Contacting NovaWorld...." },
								{ "REFRESH_ENDPOINT", "NWHost.dll" } }));
		CookieJar host_jar = jar;
		host_jar.keep(first);
		const auto second = h.send_to(port, "GET", "/NWHost.dll", host_jar.header());
		TEST_EXPECT(second.transport_ok && second.code == 200);
		TEST_EXPECT(set_cookie_value(second, "NWPF") == "38");
		const std::string text = body_text(second);
		const auto at = text.find("[HOSTKEY=");
		const std::string key = at == std::string::npos ? std::string() : text.substr(at + 9, 48);
		TEST_EXPECT(opennova::hostdb::is_host_key(key));
		TEST_EXPECT(text == nws::render_template(page("dfx2_0_host2.htm"), { { "HOSTKEY", key } }));
		return 0;
	};
	if (hosted_key("/NWHost.dll?needexpkey=dfx2_0_key2err.htm&success=dfx2_0_host2.htm"
				   "&failure=dfx2_0_main.htm&relay=dfx2_0_relay.htm&msgbase=dfx2_0_msg.htm"
				   "&nodb=dfx2_0_nodb.htm&start=dfx2_0_login.htm&pfid=38") != 0)
		return 1;
	if (hosted_key("/nwhost.dll") != 0)
		return 1;

	for (const uint32_t rid : { 3201u, 3202u })
		opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), rid);
	return 0;
}

// The CD keys' admin routes (cd_key_routes.h), with the Bearer token: generate (the title
// and count rules; DFX2-XXXX-XXXX-XXXX-XXXX serials whose wire key nests them), the list and
// its filters, assign (by username and by id, each refusal), a manual activation (its NWACT
// read back to the key's wire key by the client's own leg, the same NWACT again for the same
// machine), the key's and the account's activations, and revoke (after which the key neither
// activates nor assigns, and the account may take another).
int test_cd_key_routes(Harness &h) {
	namespace cdkey = opennova::cdkey;
	const auto json = [](const net::HttpReply &reply) {
		return crow::json::load(body_text(reply));
	};
	const auto post = [&h](const std::string &path, const std::string &body) {
		return h.send("POST", path, { kJson, kBearer }, body);
	};
	const auto code_of = [&json](const net::HttpReply &reply) {
		const auto body = json(reply);
		return body && body.has("error") ? str(body["error"]) : std::string();
	};
	constexpr const char *kMachine = "ABCDEFGHIJKLMNOPQRSTUVW";

	for (const char *body :
			{ "{}", "{\"title\":\"dfx2\"}", "{\"count\":2}", "{\"title\":\"jo\",\"count\":1}",
					"{\"title\":\"dfx2\",\"count\":0}", "{\"title\":\"dfx2\",\"count\":101}",
					"{\"title\":\"dfx2\",\"count\":\"2\"}", "[]" }) {
		const auto refused = post("/api/admin/cd-keys", body);
		if (refused.code != 400)
			std::fprintf(stderr, "  generate %s: %s\n", body, describe(refused).c_str());
		TEST_EXPECT(refused.code == 400);
	}
	auto reply = post("/api/admin/cd-keys", "{\"title\":\"dfx2\",\"count\":3}");
	TEST_EXPECT(reply.code == 201);
	auto made = json(reply);
	TEST_EXPECT(made && made["keys"].size() == 3);
	std::vector<int64_t> ids;
	std::vector<std::string> serials;
	for (const auto &k : made["keys"]) {
		const std::string serial = str(k["serial"]);
		TEST_EXPECT(serial.size() == 24 && serial.compare(0, 5, "DFX2-") == 0);
		for (size_t i = 4; i < serial.size(); i += 5)
			TEST_EXPECT(serial[i] == '-');
		std::string inner;
		TEST_EXPECT(str(k["wire_key"]) == cdkey::make_wire_key(serial));
		TEST_EXPECT(cdkey::decode_wire_key(str(k["wire_key"]), inner) && inner == serial);
		TEST_EXPECT(str(k["title"]) == "dfx2" && str(k["state"]) == "unassigned");
		TEST_EXPECT(k["account"].t() == crow::json::type::Null &&
				k["assigned_at"].t() == crow::json::type::Null);
		TEST_EXPECT(str(k["created_by"]) == "token" && k["activations"].i() == 0);
		ids.push_back(k["id"].i());
		serials.push_back(serial);
	}
	TEST_EXPECT(serials[0] != serials[1] && serials[1] != serials[2]);

	// The list: newest first, the filters, the paging and its refusals.
	reply = h.send("GET", "/api/admin/cd-keys?title=dfx2&state=unassigned&limit=2", { kBearer });
	auto listed = json(reply);
	TEST_EXPECT(
			reply.code == 200 && listed && listed["keys"].size() == 2 && listed["total"].i() >= 3);
	TEST_EXPECT(listed["keys"][0]["id"].i() > listed["keys"][1]["id"].i());
	TEST_EXPECT(listed["auto_mint"].b() && listed["titles"].size() == 1 &&
			str(listed["titles"][0]) == "dfx2");
	reply = h.send("GET", "/api/admin/cd-keys?q=" + serials[1].substr(10), { kBearer });
	listed = json(reply);
	TEST_EXPECT(reply.code == 200 && listed && listed["total"].i() == 1 &&
			listed["keys"][0]["id"].i() == ids[1]);
	for (const char *query :
			{ "?state=bogus", "?title=jo", "?limit=0", "?limit=501", "?offset=x", "?limit=-1" }) {
		const auto refused = h.send("GET", std::string("/api/admin/cd-keys") + query, { kBearer });
		TEST_EXPECT(refused.code == 400 && code_of(refused) == "invalid_field");
	}

	// Assign: by username, then the refusals.
	const int64_t owner = make_account(h, "cdkey_owner", "pw-cdkey-owner");
	const int64_t other = make_account(h, "cdkey_other", "pw-cdkey-other");
	TEST_EXPECT(owner > 0 && other > 0);
	const std::string key0 = "/api/admin/cd-keys/" + std::to_string(ids[0]);
	const std::string key1 = "/api/admin/cd-keys/" + std::to_string(ids[1]);
	reply = post(key0 + "/assign", "{\"username\":\"cdkey_owner\"}");
	auto assigned = json(reply);
	TEST_EXPECT(reply.code == 200 && assigned && str(assigned["key"]["state"]) == "assigned");
	TEST_EXPECT(assigned["key"]["account"]["id"].i() == owner &&
			str(assigned["key"]["account"]["username"]) == "cdkey_owner");
	TEST_EXPECT(assigned["key"]["assigned_at"].t() == crow::json::type::String);
	reply = post(key0 + "/assign", "{\"user_id\":" + std::to_string(other) + "}");
	TEST_EXPECT(reply.code == 409 && code_of(reply) == "already_assigned");
	reply = post(key1 + "/assign", "{\"username\":\"cdkey_owner\"}");
	TEST_EXPECT(reply.code == 409 && code_of(reply) == "account_has_key");
	reply = post(key1 + "/assign", "{\"username\":\"nobody_here\"}");
	TEST_EXPECT(reply.code == 404 && code_of(reply) == "no_such_account");
	reply = post("/api/admin/cd-keys/999999/assign", "{\"username\":\"cdkey_other\"}");
	TEST_EXPECT(reply.code == 404 && code_of(reply) == "not_found");
	TEST_EXPECT(post(key1 + "/assign", "{}").code == 400);
	TEST_EXPECT(post(key1 + "/assign", "{\"username\":7}").code == 400);
	reply = post(key1 + "/assign", "{\"user_id\":" + std::to_string(other) + "}");
	TEST_EXPECT(reply.code == 200 && json(reply)["key"]["account"]["id"].i() == other);
	reply = h.send("GET", "/api/admin/cd-keys?state=assigned&q=cdkey_", { kBearer });
	TEST_EXPECT(reply.code == 200 && json(reply)["total"].i() == 2);

	// A manual activation: the NWACT a machine's registry CDKIID takes, recorded once per machine.
	TEST_EXPECT(post(key0 + "/activations", "{\"nwpssk\":\"abc\"}").code == 400);
	TEST_EXPECT(post(key0 + "/activations", "{}").code == 400);
	TEST_EXPECT(post("/api/admin/cd-keys/999999/activations",
						std::string("{\"nwpssk\":\"") + kMachine + "\"}")
						.code == 404);
	reply = post(key0 + "/activations", std::string("{\"nwpssk\":\"") + kMachine + "\"}");
	auto activated = json(reply);
	TEST_EXPECT(reply.code == 201 && activated && !activated["reactivated"].b());
	const std::string nwact = str(activated["nwact"]);
	std::string wire;
	TEST_EXPECT(cdkey::read_activation(nwact, kMachine, wire) &&
			wire == str(made["keys"][0]["wire_key"]));
	TEST_EXPECT(activated["key"]["activations"].i() == 1);
	reply = post(key0 + "/activations", std::string("{\"nwpssk\":\"") + kMachine + "\"}");
	TEST_EXPECT(reply.code == 200 && json(reply)["reactivated"].b() &&
			str(json(reply)["nwact"]) == nwact);
	reply = h.send("GET", key0 + "/activations", { kBearer });
	auto history = json(reply);
	TEST_EXPECT(reply.code == 200 && history && history["activations"].size() == 1);
	TEST_EXPECT(str(history["activations"][0]["nwpssk"]) == kMachine &&
			str(history["activations"][0]["nwact"]) == nwact);
	TEST_EXPECT(h.send("GET", "/api/admin/cd-keys/999999/activations", { kBearer }).code == 404);
	reply = h.send("GET", "/api/admin/users/" + std::to_string(owner) + "/cd-keys", { kBearer });
	auto account = json(reply);
	TEST_EXPECT(
			reply.code == 200 && account && str(account["account"]["username"]) == "cdkey_owner");
	TEST_EXPECT(account["keys"].size() == 1 && account["activations"].size() == 1);
	TEST_EXPECT(str(account["activations"][0]["serial"]) == serials[0]);
	TEST_EXPECT(h.send("GET", "/api/admin/users/999999/cd-keys", { kBearer }).code == 404);

	// Revoke: once; after it the key neither activates nor assigns, and the account takes another.
	reply = post(key0 + "/revoke", "");
	auto revoked = json(reply);
	TEST_EXPECT(reply.code == 200 && revoked && str(revoked["key"]["state"]) == "revoked");
	TEST_EXPECT(revoked["key"]["revoked_at"].t() == crow::json::type::String);
	reply = post(key0 + "/revoke", "");
	TEST_EXPECT(reply.code == 409 && code_of(reply) == "already_revoked");
	TEST_EXPECT(post("/api/admin/cd-keys/999999/revoke", "").code == 404);
	reply = post(key0 + "/activations", std::string("{\"nwpssk\":\"") + kMachine + "\"}");
	TEST_EXPECT(reply.code == 409 && code_of(reply) == "revoked");
	reply = post("/api/admin/cd-keys/" + std::to_string(ids[2]) + "/revoke", "");
	TEST_EXPECT(reply.code == 200);
	reply = post("/api/admin/cd-keys/" + std::to_string(ids[2]) + "/assign",
			"{\"username\":\"cdkey_owner\"}");
	TEST_EXPECT(reply.code == 409 && code_of(reply) == "revoked");
	reply = post("/api/admin/cd-keys", "{\"title\":\"dfx2\",\"count\":1}");
	const int64_t fresh = json(reply)["keys"][0]["id"].i();
	reply = post("/api/admin/cd-keys/" + std::to_string(fresh) + "/assign",
			"{\"username\":\"cdkey_owner\"}");
	TEST_EXPECT(reply.code == 200);
	reply = h.send("GET", "/api/admin/users/" + std::to_string(owner) + "/cd-keys", { kBearer });
	TEST_EXPECT(reply.code == 200 && json(reply)["keys"].size() == 2);
	reply = h.send("GET", "/api/admin/cd-keys?state=revoked", { kBearer });
	TEST_EXPECT(reply.code == 200 && json(reply)["total"].i() == 2);
	return 0;
}

// A retail DFX2 client's activation through the login (cd_key_routes.h login_activation), on
// listeners of its own (their own retail login brakes), one minting an account's first key and
// one not (ONNET_CD_KEY_AUTO_MINT=0). An unactivated client (its NWCDKIID empty) gets the
// activation login at NWStart, and its completed login carries NWACT for the NWPSSK it posts:
// the client's own reading of it (the mix under that fingerprint, the registry chain) gives the
// wire key of the account's key, minted now. The same machine gets the same NWACT again, another
// machine its own; an activated client (a wire key in NWCDKIID), a JO client and a client posting
// no NWPSSK get none; an account whose key was revoked gets none and no new key; with minting
// off, an account with no key gets none while one holding a key is activated.
int test_dfx2_activation(Harness &h) {
	namespace cdkey = opennova::cdkey;
	constexpr const char *kMachineA = "ABCDEFGHIJKLMNOPQRSTUVW";
	constexpr const char *kMachineB = "TTTTTTTTTTTTTTTTTTTTTTT";
	opennova::ConnectionManager manager;
	nws::SessionStore sessions;
	nws::ServerConfig config;
	config.public_host = "127.0.0.1";
	config.http_port = 0;
	config.admin_api_token = kAdminToken;
	config.templates_dir = h.retail_root / "templates";
	config.static_dir = h.retail_root / "static";
	config.web_dist_dir = h.retail_root / "web";
	nws::HttpListener minting(manager, h.pool, sessions);
	TEST_EXPECT(minting.start(config));
	config.cd_key_auto_mint = false;
	nws::HttpListener assigned_only(manager, h.pool, sessions);
	TEST_EXPECT(assigned_only.start(config));
	const auto page = [&h](const char *name) {
		return test_io::read_file_text((h.retail_root / "templates" / name).string());
	};

	struct Login {
		std::string start_page; // what NWStart.dll rendered
		net::HttpReply completion; // GET /NWLogin.dll
	};
	// A DFX2 client's login from the gate's startupurl on: `jar` holds the browser variables its
	// NovaWorld connect set (NWCDKIID, NWPSSK); JO's pages when `dfx2` is false.
	const auto retail_login = [&](uint16_t port, const std::string &user,
									  const std::string &password,
									  std::map<std::string, std::string> variables, bool dfx2) {
		Login out;
		CookieJar jar;
		jar.cookies = std::move(variables);
		auto reply = h.send_to(port, "GET",
				dfx2 ? "/nwprepare.dll?ver1=1&ver2=9013&cc=us&pfid=38&gt=dfx2:0:cus:buffy&url=dfx2_"
					   "0_start.htm"
					 : "/nwprepare.dll?ver1=3&ver2=2345&cc=us&gt=jop:cus2&url=jop_2_start.htm",
				jar.header());
		jar.keep(reply);
		reply = h.send_to(port, "GET",
				dfx2 ? "/NWStart.dll?MSGBASE=dfx2_0_msg.htm&IN=dfx2_0_main.htm&OUT=dfx2_0_login.htm"
					   "&ACT=dfx2_0_actlogin.htm&verfile=dfx2_0.ver"
					 : "/NWStart.dll?MSGBASE=jop_2_msg.htm&IN=jop_2_main.htm&OUT=jop_2_login.htm&"
					   "verfile=jop_2.ver",
				jar.header());
		out.start_page = body_text(reply);
		jar.keep(reply);
		opennova::EpaskParams epask;
		std::string body;
		if (!opennova::epask_from_string(jar.cookies["EPASK"], epask) ||
				!opennova::build_login_post_body(epask,
						{ { "EPASK", opennova::epask_to_string(epask), false },
								{ "NAME", user, true }, { "PASSWORD", password, true },
								{ "success", dfx2 ? "dfx2_0_main.htm" : "jop_2_main.htm", true },
								{ "failure", dfx2 ? "dfx2_0_actlogin.htm" : "jop_2_login.htm",
										true },
								{ "msgbase", dfx2 ? "dfx2_0_msg.htm" : "jop_2_msg.htm", true },
								{ "relay", dfx2 ? "dfx2_0_relay.htm" : "jop_2_relay.htm", true },
								{ "pfid", dfx2 ? "38" : "28", true } },
						body))
			return out;
		auto headers = jar.header();
		headers.push_back("Content-Type: application/x-www-form-urlencoded");
		reply = h.send_to(port, "POST", "/NWLogin.dll", headers, body);
		if (!nwlogin_succeeded(reply)) {
			std::fprintf(stderr, "  retail login %s: %s\n", user.c_str(), describe(reply).c_str());
			return out;
		}
		jar.keep(reply);
		out.completion = h.send_to(port, "GET", "/NWLogin.dll", jar.header());
		return out;
	};
	const auto unactivated = [](const char *machine) {
		return std::map<std::string, std::string>{ { "NWCDKIID", "" }, { "NWCDKIIDEXP1", "" },
			{ "NWPSSK", machine } };
	};
	const auto live_wire_key = [&h](int64_t user) {
		const auto rows = h.pool.acquire()->query("SELECT wire_key, created_by FROM cd_keys WHERE "
												  "user_id = $1 AND revoked_at IS NULL;",
				{ opennova::db::BindValue(user) });
		return rows.size() == 1
				? rows[0].as_text(0).value_or("") + "|" + rows[0].as_text(1).value_or("")
				: std::string();
	};

	// The first activation: the activation login, then the NWACT of a key minted for the account.
	const int64_t player = make_account(h, "act_player", "pw-act-player");
	TEST_EXPECT(player > 0);
	auto login = retail_login(
			minting.bound_port(), "act_player", "pw-act-player", unactivated(kMachineA), true);
	TEST_EXPECT(login.start_page == page("dfx2_0_actlogin.htm"));
	TEST_EXPECT(login.completion.transport_ok && login.completion.code == 200);
	TEST_EXPECT(body_text(login.completion).find("NovaWorld Main Menu") != std::string::npos);
	const std::string nwact_a = set_cookie_value(login.completion, "NWACT");
	const std::string minted = live_wire_key(player);
	TEST_EXPECT(
			!nwact_a.empty() && minted.size() > 5 && minted.substr(minted.size() - 5) == "|auto");
	const std::string wire_key = minted.substr(0, minted.size() - 5);
	std::string recovered;
	TEST_EXPECT(cdkey::read_activation(nwact_a, kMachineA, recovered) && recovered == wire_key);
	std::string serial;
	TEST_EXPECT(cdkey::decode_wire_key(recovered, serial) && serial.compare(0, 5, "DFX2-") == 0);
	// Only that machine reads it.
	TEST_EXPECT(!cdkey::read_activation(nwact_a, kMachineB, recovered));

	// The same machine again: the same NWACT, no second key; another machine its own.
	login = retail_login(
			minting.bound_port(), "act_player", "pw-act-player", unactivated(kMachineA), true);
	TEST_EXPECT(set_cookie_value(login.completion, "NWACT") == nwact_a);
	TEST_EXPECT(live_wire_key(player) == minted);
	login = retail_login(
			minting.bound_port(), "act_player", "pw-act-player", unactivated(kMachineB), true);
	const std::string nwact_b = set_cookie_value(login.completion, "NWACT");
	TEST_EXPECT(!nwact_b.empty() && nwact_b != nwact_a);
	TEST_EXPECT(cdkey::read_activation(nwact_b, kMachineB, recovered) && recovered == wire_key);
	TEST_EXPECT(h.pool.acquire()
						->query("SELECT COUNT(*) FROM cd_key_activations a JOIN cd_keys k ON k.id "
								"= a.key_id "
								"WHERE k.user_id = $1;",
								{ opennova::db::BindValue(player) })
						.front()
						.as_int(0)
						.value() == 2);

	// Activated (its NWCDKIID the wire key it now keeps): the login page, and no NWACT.
	login = retail_login(minting.bound_port(), "act_player", "pw-act-player",
			{ { "NWCDKIID", wire_key }, { "NWCDKIIDEXP1", "" }, { "NWPSSK", kMachineA } }, true);
	TEST_EXPECT(login.start_page == page("dfx2_0_login.htm"));
	TEST_EXPECT(login.completion.code == 200 && !sets_cookie(login.completion, "NWACT"));
	// No NWPSSK: nothing to bind an NWACT to, and nothing minted.
	const int64_t bare = make_account(h, "act_bare", "pw-act-bare");
	login = retail_login(
			minting.bound_port(), "act_bare", "pw-act-bare", { { "NWCDKIID", "" } }, true);
	TEST_EXPECT(login.completion.code == 200 && !sets_cookie(login.completion, "NWACT"));
	TEST_EXPECT(live_wire_key(bare).empty());
	// A JO client: JO's pages, no NWACT, no key, its empty NWCDKIID taken as ever.
	const int64_t jo = make_account(h, "act_jo", "pw-act-jo");
	login = retail_login(
			minting.bound_port(), "act_jo", "pw-act-jo", unactivated(kMachineA), false);
	// JO's start page has no ACT: its login page, whatever the NWCDKIID.
	TEST_EXPECT(login.start_page.find("<TITLE>NovaWorld Login</TITLE>") != std::string::npos &&
			login.start_page.find("VALUE=\"jop_2_main.htm\"") != std::string::npos);
	TEST_EXPECT(login.completion.code == 200 && !sets_cookie(login.completion, "NWACT"));
	TEST_EXPECT(live_wire_key(jo).empty());

	// Revoked: no NWACT, and no new key minted in its place.
	TEST_EXPECT(
			h.send("POST",
					 "/api/admin/cd-keys/" +
							 std::to_string(h.pool.acquire()
											 ->query("SELECT id FROM cd_keys WHERE user_id = $1;",
													 { opennova::db::BindValue(player) })
											 .front()
											 .as_int(0)
											 .value()) +
							 "/revoke",
					 { kJson, kBearer })
					.code == 200);
	login = retail_login(
			minting.bound_port(), "act_player", "pw-act-player", unactivated(kMachineA), true);
	TEST_EXPECT(login.completion.code == 200 && !sets_cookie(login.completion, "NWACT"));
	TEST_EXPECT(live_wire_key(player).empty());

	// Minting off: no key, no NWACT; an admin's key activates.
	const int64_t waiting = make_account(h, "act_waiting", "pw-act-waiting");
	login = retail_login(assigned_only.bound_port(), "act_waiting", "pw-act-waiting",
			unactivated(kMachineB), true);
	TEST_EXPECT(login.start_page == page("dfx2_0_actlogin.htm"));
	TEST_EXPECT(login.completion.code == 200 && !sets_cookie(login.completion, "NWACT"));
	TEST_EXPECT(live_wire_key(waiting).empty());
	auto made = h.send(
			"POST", "/api/admin/cd-keys", { kJson, kBearer }, "{\"title\":\"dfx2\",\"count\":1}");
	const auto key = crow::json::load(body_text(made));
	TEST_EXPECT(made.code == 201 && key);
	TEST_EXPECT(
			h.send("POST",
					 "/api/admin/cd-keys/" + std::to_string(key["keys"][0]["id"].i()) + "/assign",
					 { kJson, kBearer }, "{\"username\":\"act_waiting\"}")
					.code == 200);
	login = retail_login(assigned_only.bound_port(), "act_waiting", "pw-act-waiting",
			unactivated(kMachineB), true);
	TEST_EXPECT(cdkey::read_activation(
			set_cookie_value(login.completion, "NWACT"), kMachineB, recovered));
	TEST_EXPECT(recovered == str(key["keys"][0]["wire_key"]));

	assigned_only.stop();
	minting.stop();
	return 0;
}

// A host's NovaWorld session on loopback against the harness's NwUdpListener: a ClientSession
// verified and hosting, pumped by the test while it waits for what the service pushes.
class PushHost {
public:
	// `client_index` tells several hosts' sessions apart in the service's log (each has its own
	// socket, which is what the service keys a connection by).
	explicit PushHost(uint16_t service_port, uint32_t client_index = 0x48545450u)
		: session_(config(client_index)) {
		socket_ = net::ScopedSocket(net::udp_bind(0));
		service_.ip = {127, 0, 0, 1};
		service_.port = service_port;
	}

	// Verified, then a ClientHostRequest answered with success; false on a timeout.
	bool host(const std::string &server_name) {
		if (!socket_.is_valid()) return false;
		send(session_.start());
		pump_until(8000, [this] { return session_.is_verified(); });
		if (!session_.is_verified()) return false;
		session_.queue_statement(opennova::make_client_host_request(
				0, {{0, "NWUID", session_.server_nwuid()}},
				{{0, "LobbyName", "jop_2_consumer"}, {0, "MaxPlayers", "8"}},
				{{0, "ServerName", server_name}, {0, "Players", "1"}}, {{0, "PlayerName", "Host"}}));
		return wait_notice(Notice::Kind::HostResult, 3000) != nullptr &&
		       session_.host_state() == opennova::ClientSession::HostState::Established;
	}

	using Notice = opennova::ClientSession::Notice;
	// The first notice of `kind` the session raised, pumping up to `ms` for it; null on a timeout.
	const Notice *wait_notice(Notice::Kind kind, int ms) {
		pump_until(ms, [this, kind] { return find(kind) != nullptr; });
		return find(kind);
	}
	void clear_notices() { notices_.clear(); }
	void goodbye() { send(session_.build_goodbye()); }
	// The ClientHostUpdate that first carries the HostKey, as a stock host's at its mission's
	// start (the key names the listing's owner), pumped while it goes out and is ACKed.
	void present_key(const std::string &key) {
		session_.set_host_list_vars({ { 0, "HostKey", key } });
		session_.send_host_update();
		pump_until(300, [] { return false; });
	}

private:
	static opennova::ClientSession::Config config(uint32_t client_index) {
		opennova::ClientSession::Config cfg = opennova::ClientSession::Config::lobby(kJo, kJo.gate_tag);
		cfg.client_index = client_index;
		cfg.client_key = 0x50555348u;
		cfg.na = "http:push";
		cfg.cookie_vars = []() {
			return std::vector<std::pair<std::string, std::string>>{{"NWUID", ""}};
		};
		return cfg;
	}
	void send(const std::vector<uint8_t> &dg) {
		if (!dg.empty()) net::udp_send_to(socket_.get(), service_, dg.data(), dg.size());
	}
	const Notice *find(Notice::Kind kind) const {
		for (const Notice &n : notices_) {
			if (n.kind == kind) return &n;
		}
		return nullptr;
	}
	template <class Done> void pump_until(int ms, Done done) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
		uint8_t rx[4096];
		while (std::chrono::steady_clock::now() < deadline && !done()) {
			net::Endpoint from;
			const int n = net::udp_recv_from(socket_.get(), rx, sizeof rx, from, 20);
			std::vector<std::vector<uint8_t>> out;
			if (n > 0) session_.handle_datagram(rx, static_cast<size_t>(n), out);
			session_.finish_receive_batch(out);
			session_.pump(out);
			session_.process_periodic_update();
			for (const auto &dg : out) send(dg);
			for (Notice &notice : session_.take_notices()) notices_.push_back(std::move(notice));
		}
	}

	opennova::ClientSession session_;
	net::ScopedSocket socket_;
	net::Endpoint service_{};
	std::vector<Notice> notices_;
};

// POST /api/admin/hosts/<rid>/command and /stop: admin-gated (the Bearer token, or an admin
// session with the CSRF header; the admin table case pins the refusals); a body the composer
// refuses is a 400 with its reason; a RID no connection holds is a 404; a hosting connection's
// RID is a 202, and the statement reaches that host's session; a stopped host's RID is a 409.
int test_admin_host_push(Harness &h) {
	const std::string valid = "{\"verb\":\"SetServerName\",\"args\":[\"Route Name\"]}";
	auto command = [&h](const std::string &rid, const std::string &body, bool bearer = true) {
		std::vector<std::string> headers = {kJson};
		if (bearer) headers.push_back(kBearer);
		return h.send("POST", "/api/admin/hosts/" + rid + "/command", headers, body);
	};
	auto stop = [&h](const std::string &rid, bool bearer = true) {
		std::vector<std::string> headers;
		if (bearer) headers.push_back(kBearer);
		return h.send("POST", "/api/admin/hosts/" + rid + "/stop", headers);
	};
	auto error_of = [](const net::HttpReply &reply) {
		const auto json = crow::json::load(body_text(reply));
		return json && json.has("error") ? str(json["error"]) : std::string("<none>");
	};
	auto message_of = [](const net::HttpReply &reply) {
		const auto json = crow::json::load(body_text(reply));
		return json && json.has("message") ? str(json["message"]) : std::string();
	};

	// The admin check, before anything else is read.
	auto reply = command("1", valid, /*bearer=*/false);
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	TEST_EXPECT(has_header(reply, "WWW-Authenticate"));
	reply = h.send("POST", "/api/admin/hosts/1/command", {kJson, "Authorization: Bearer wrong-token"}, valid);
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = stop("1", /*bearer=*/false);
	TEST_EXPECT(reply.transport_ok && reply.code == 401);

	// The composer's refusals, each with its reason.
	struct Refused {
		const char *body;
		const char *reason; // a substring of the message
	};
	const Refused refused[] = {
		{"not json", ""},
		{"{\"args\":[\"x\"]}", "verb"},
		{"{\"verb\":\"Nope\"}", "no ServerCommand verb"},
		{"{\"verb\":\"PuntPlayer\",\"args\":[\"3\"]}", "needs a target suffix"},
		{"{\"verb\":\"Cycle\",\"target\":\"ByName\"}", "takes no target suffix"},
		{"{\"verb\":\"Cycle\",\"target\":\"ByNumber\"}", "\"target\""},
		{"{\"verb\":\"SetServerName\"}", "token-count gate"},
		{"{\"verb\":\"TextChatPlayer\",\"target\":\"ByName\",\"args\":[\"Some Guy\"]}", "token-count gate"},
		{"{\"verb\":\"TextChatServer\",\"args\":[\"say \\\"hi\\\"\"]}", "double quote"},
		{"{\"verb\":\"Cycle\",\"args\":[3]}", "list of strings"},
		// Control bytes: the stock reader would carry them inside quotes, and a host would save
		// a LF into game.cfg as a line of its own (the service's input rule, not the composer's).
		{"{\"verb\":\"SetServerName\",\"args\":[\"Evil\\nmpreset = \\\"1\\\"\"]}", "control byte"},
		{"{\"verb\":\"SetServerMsg\",\"args\":[\"a\\rb\"]}", "control byte"},
		{"{\"verb\":\"TextChatServer\",\"args\":[\"tab\\there\"]}", "control byte"},
		{"{\"verb\":\"TextChatServer\",\"args\":[\"del\\u007f\"]}", "control byte"},
		// A C1 control (U+0085, NEL) as UTF-8.
		{"{\"verb\":\"TextChatServer\",\"args\":[\"nel\xC2\x85here\"]}", "control byte"},
		// The host reads cp1252 (D-NET-382): a character it has no byte for is refused by name.
		{"{\"verb\":\"TextChatServer\",\"args\":[\"hi \xF0\x9F\x98\x80\"]}", "U+1F600"},
		{"{\"verb\":\"SetServerMsg\",\"args\":[\"\xE4\xB8\xAD\"]}", "U+4E2D"},
		// An escaped surrogate pair (Crow decodes each half on its own) is joined and named; an
		// unpaired half is no UTF-8.
		{"{\"verb\":\"TextChatServer\",\"args\":[\"gg \\ud83d\\ude00\"]}", "U+1F600"},
		{"{\"verb\":\"TextChatServer\",\"args\":[\"gg \\ud83d\"]}", "not UTF-8"},
		// What the host keeps of a name and a message, counted in its bytes.
		{"{\"verb\":\"SetServerName\",\"args\":[\"abcdefghijklmnopqrstuvwxyz012345\"]}", "keeps 31"},
	};
	for (const Refused &r : refused) {
		reply = command("1", r.body);
		if (!reply.transport_ok || reply.code != 400 ||
		    message_of(reply).find(r.reason) == std::string::npos) {
			std::fprintf(stderr, "  %s -> %s\n", r.body, describe(reply).c_str());
		}
		TEST_EXPECT(reply.transport_ok && reply.code == 400);
		TEST_EXPECT(message_of(reply).find(r.reason) != std::string::npos);
	}
	reply = command("1", "{\"verb\":\"Cycle\",\"args\":[\"" + std::string(506, 'a') + "\"]}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(message_of(reply).find("511") != std::string::npos);
	reply = command("1", "{\"verb\":\"SetServerMsg\",\"args\":[\"" + std::string(128, 'm') + "\"]}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(message_of(reply).find("keeps 127") != std::string::npos);
	// Bytes that are not UTF-8 at all.
	reply = command("1", "{\"verb\":\"TextChatServer\",\"args\":[\"bad\xFF\xFE\"]}");
	if (reply.code != 400 || message_of(reply).find("UTF-8") == std::string::npos) {
		std::fprintf(stderr, "  not UTF-8 -> %s\n", describe(reply).c_str());
	}
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(message_of(reply).find("UTF-8") != std::string::npos);

	// A RID no connection holds.
	for (const char *rid : {"0", "999999", "99999999999"}) {
		reply = command(rid, valid);
		TEST_EXPECT(reply.transport_ok && reply.code == 404);
		TEST_EXPECT(error_of(reply) == "unknown_rid");
		reply = stop(rid);
		TEST_EXPECT(reply.transport_ok && reply.code == 404);
	}

	// A hosting connection: the command is queued and reaches its session.
	PushHost host(h.nw_udp.bound_port());
	TEST_EXPECT(host.host("Route Push Host"));
	uint32_t rid = 0;
	for (const auto &entry : h.nw_udp.snapshot_hosted()) {
		if (entry.lobby.server_name == "Route Push Host") rid = entry.lobby.rid;
	}
	TEST_EXPECT(rid != 0);
	const std::string rid_text = std::to_string(rid);
	reply = command(rid_text, valid);
	if (reply.code != 202) std::fprintf(stderr, "  command: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["status"]) == "queued");
	TEST_EXPECT(json && json["rid"].i() == static_cast<int64_t>(rid));
	TEST_EXPECT(json && str(json["statement"]) == "ServerCommand");
	TEST_EXPECT(json && str(json["cmd"]) == "SetServerName \"Route Name\"");
	const PushHost::Notice *cmd = host.wait_notice(PushHost::Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr);
	TEST_EXPECT(cmd->command.verb == opennova::ServerCommandVerb::SetServerName);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{"Route Name"});

	// A name of 31 "é": 62 bytes of UTF-8, 31 in the host's cp1252, so it fits what the host
	// keeps, and it reaches the host as 31 0xE9 bytes; the reply shows the line as UTF-8.
	std::string e_acute_31;
	for (int i = 0; i < 31; ++i) e_acute_31 += "\xC3\xA9";
	host.clear_notices();
	reply = command(rid_text, "{\"verb\":\"SetServerName\",\"args\":[\"" + e_acute_31 + "\"]}");
	if (reply.code != 202) std::fprintf(stderr, "  31 e-acute: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["cmd"]) == "SetServerName \"" + e_acute_31 + "\"");
	cmd = host.wait_notice(PushHost::Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{std::string(31, '\xE9')});

	// cp1252's printable 0x80..0x9F characters are no C1 controls: "€ ™" reaches the host as
	// 0x80, a space and 0x99.
	host.clear_notices();
	reply = command(rid_text, "{\"verb\":\"TextChatServer\",\"args\":[\"\xE2\x82\xAC \xE2\x84\xA2\"]}");
	if (reply.code != 202) std::fprintf(stderr, "  euro, tm: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	cmd = host.wait_notice(PushHost::Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{"\x80 \x99"});

	// A message of 127 host bytes, the most a stock host keeps (126 letters and an "é", 128
	// bytes of UTF-8), is accepted whole.
	host.clear_notices();
	reply = command(rid_text, "{\"verb\":\"SetServerMsg\",\"args\":[\"" + std::string(126, 'm') +
	                                  "\xC3\xA9\"]}");
	if (reply.code != 202) std::fprintf(stderr, "  127-byte message: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	cmd = host.wait_notice(PushHost::Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{std::string(126, 'm') + "\xE9"});

	// An admin's website session pushes too, with the CSRF header (the
	// bootstrap case made web_boss the admin).
	const std::string admin = login_token(h, "10.14.0.1", "web_boss", "pw-boss");
	TEST_EXPECT(!admin.empty());
	host.clear_notices();
	reply = h.send("POST", "/api/admin/hosts/" + rid_text + "/command",
	               {kJson, kCsrf, with_session(admin)},
	               "{\"verb\":\"SetServerName\",\"args\":[\"Session Name\"]}");
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	cmd = host.wait_notice(PushHost::Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{"Session Name"});

	// The stop: queued, the host's session reads the sysop-punt code, the RID is then not hosting.
	reply = stop(rid_text);
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["statement"]) == "ServerStopHosting");
	TEST_EXPECT(json && json["msg_code"].i() == 7);
	const PushHost::Notice *stopped = host.wait_notice(PushHost::Notice::Kind::StopHosting, 3000);
	TEST_EXPECT(stopped != nullptr);
	TEST_EXPECT(stopped->msg_key == "NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT");
	reply = stop(rid_text);
	TEST_EXPECT(reply.transport_ok && reply.code == 409);
	TEST_EXPECT(error_of(reply) == "not_hosting");
	reply = command(rid_text, valid);
	TEST_EXPECT(reply.transport_ok && reply.code == 409);
	host.goodbye();
	return 0;
}

// Polls `done` every 20 ms for up to `ms`; its last answer.
template <typename Done>
bool wait_until(Done done, int ms = 3000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!done() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return done();
}

// The browser's row (active_hosts) of the server named `name`.
std::optional<opennova::hostdb::HostRow> row_named(Harness &h, const std::string &name) {
	for (auto &row : opennova::hostdb::list_hosts(*h.pool.acquire())) {
		if (row.server_name == name) return row;
	}
	return std::nullopt;
}

// POST /api/me/hosts/<rid>/command and /stop: the account that listed a server (its HostKey,
// active_hosts.host_user_id) pushes to that server alone. GET /api/me/hosts carries the roster the
// player verbs target. Logged out is 401 and a missing CSRF header 403, before any bucket. Another
// account's RID and a free one get the same 404 and push nothing; a body the composer refuses is a
// 400 (read before the listing), SetMPReset a 403 even for the owner; the owner's pushes are 202
// and reach the host's session, a "café" as the host's cp1252 (an emoji is a 400). The listener
// re-checks the owner at push time: a claim of the caller's on a connection that does not name
// the caller is a 404; and again at the drain: a push queued before a ban (which takes the
// listing's in-memory owner) never goes out. Every push draws one token from the account's
// bucket (8 at once, 429 with Retry-After past them); the stop ends the listing, and a listed
// row over a connection no longer hosting is the 409. Each account below keeps to its
// bucket's eight (Alice waits for a ninth).
int test_owner_host_push(Harness &h) {
	using opennova::db::BindValue;
	using Notice = PushHost::Notice;
	const int64_t alice = make_account(h, "owner_alice", "pw-owner-alice");
	TEST_EXPECT(alice > 0);
	TEST_EXPECT(make_account(h, "owner_bob", "pw-owner-bob") > 0);
	TEST_EXPECT(make_account(h, "owner_carol", "pw-owner-carol") > 0);
	TEST_EXPECT(make_account(h, "owner_dave", "pw-owner-dave") > 0);
	const std::string alice_s = login_token(h, "10.31.0.1", "owner_alice", "pw-owner-alice");
	const std::string bob_s = login_token(h, "10.31.0.2", "owner_bob", "pw-owner-bob");
	const std::string carol_s = login_token(h, "10.31.0.3", "owner_carol", "pw-owner-carol");
	const std::string dave_s = login_token(h, "10.31.0.4", "owner_dave", "pw-owner-dave");
	TEST_EXPECT(!alice_s.empty() && !bob_s.empty() && !carol_s.empty() && !dave_s.empty());

	auto command = [&h](const std::string &session, const std::string &rid, const std::string &body,
	                    bool csrf = true) {
		std::vector<std::string> headers = {kJson, with_session(session)};
		if (csrf) headers.push_back(kCsrf);
		return h.send("POST", "/api/me/hosts/" + rid + "/command", headers, body);
	};
	auto stop = [&h](const std::string &session, const std::string &rid, bool csrf = true) {
		std::vector<std::string> headers = {with_session(session)};
		if (csrf) headers.push_back(kCsrf);
		return h.send("POST", "/api/me/hosts/" + rid + "/stop", headers);
	};
	auto error_of = [](const net::HttpReply &reply) {
		const auto json = crow::json::load(body_text(reply));
		return json && json.has("error") ? str(json["error"]) : std::string("<none>");
	};
	auto message_of = [](const net::HttpReply &reply) {
		const auto json = crow::json::load(body_text(reply));
		return json && json.has("message") ? str(json["message"]) : std::string();
	};
	const std::string rename = "{\"verb\":\"SetServerName\",\"args\":[\"Owner Renamed\"]}";

	// Alice's server: listed with no owner by its request, then hers once its update presents a
	// key minted for her (as /NWHost.dll records one for her retail login).
	const std::string key = "PONMLKJIHGFEDCBAAAAABBBBCCCCDDDDEEEEFFFFGGGGHHHH";
	TEST_EXPECT(opennova::hostdb::is_host_key(key));
	TEST_EXPECT(record_key_for(h, key, alice));
	PushHost mine(h.nw_udp.bound_port(), 0x4f574e31u);
	TEST_EXPECT(mine.host("Owner Push Host"));
	mine.present_key(key);
	std::optional<opennova::hostdb::HostRow> row;
	TEST_EXPECT(wait_until([&] {
		row = row_named(h, "Owner Push Host");
		return row && row->host_user_id == alice;
	}));
	const std::string rid = std::to_string(row->rid);
	// Another server, listed with no owner (its update never presents a key).
	PushHost theirs(h.nw_udp.bound_port(), 0x414e4f4eu);
	TEST_EXPECT(theirs.host("Anonymous Push Host"));
	std::optional<opennova::hostdb::HostRow> anon;
	TEST_EXPECT(wait_until([&] {
		anon = row_named(h, "Anonymous Push Host");
		return anon.has_value();
	}));
	TEST_EXPECT(!anon->host_user_id);
	const std::string anon_rid = std::to_string(anon->rid);

	// The owner's list: her server with its roster (the host's PlayerList); Bob's has none of it.
	auto reply = h.send("GET", "/api/me/hosts", {with_session(alice_s)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["hosts"].size() == 1);
	TEST_EXPECT(json["hosts"][0]["rid"].i() == static_cast<int64_t>(row->rid));
	TEST_EXPECT(str(json["hosts"][0]["server_name"]) == "Owner Push Host");
	const auto &roster = json["hosts"][0]["roster"];
	TEST_EXPECT(roster.t() == crow::json::type::List && roster.size() == 1);
	TEST_EXPECT(roster[0]["slot"].i() == 0 && str(roster[0]["player_name"]) == "Host");
	TEST_EXPECT(json["hosts"][0].has("msg"));
	reply = h.send("GET", "/api/me/hosts", {with_session(bob_s)});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(crow::json::load(body_text(reply))["hosts"].size() == 0);

	// Logged out, a dead session, and no CSRF header: refused before anything else.
	reply = h.send("POST", "/api/me/hosts/" + rid + "/command", {kJson, kCsrf}, rename);
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = h.send("POST", "/api/me/hosts/" + rid + "/stop", {kCsrf});
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = command(std::string(64, 'a'), rid, rename);
	TEST_EXPECT(reply.transport_ok && reply.code == 401);
	reply = command(alice_s, rid, rename, /*csrf=*/false);
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(error_of(reply) == "csrf_header_required");
	reply = stop(alice_s, rid, /*csrf=*/false);
	TEST_EXPECT(reply.transport_ok && reply.code == 403);

	// Another account's server and a free RID: the same 404, and nothing reaches either host.
	const auto others_command = command(bob_s, rid, rename);
	const auto free_command = command(bob_s, "999999", rename);
	TEST_EXPECT(others_command.transport_ok && others_command.code == 404);
	TEST_EXPECT(error_of(others_command) == "unknown_rid");
	TEST_EXPECT(body_text(others_command) == body_text(free_command) && free_command.code == 404);
	const auto others_stop = stop(bob_s, rid);
	const auto free_stop = stop(bob_s, "999999");
	TEST_EXPECT(others_stop.transport_ok && others_stop.code == 404);
	TEST_EXPECT(body_text(others_stop) == body_text(free_stop) && free_stop.code == 404);
	TEST_EXPECT(body_text(others_stop) == body_text(others_command));
	reply = command(bob_s, anon_rid, rename);
	TEST_EXPECT(reply.transport_ok && reply.code == 404 && body_text(reply) == body_text(free_command));
	TEST_EXPECT(mine.wait_notice(Notice::Kind::Command, 300) == nullptr);
	TEST_EXPECT(mine.wait_notice(Notice::Kind::StopHosting, 100) == nullptr);
	TEST_EXPECT(theirs.wait_notice(Notice::Kind::Command, 100) == nullptr);
	TEST_EXPECT(row_named(h, "Owner Push Host").has_value());

	// The composer's refusals, the admin route's rule (a non-owner's too: the body is read first).
	struct Refused {
		const char *body;
		const char *error;
		const char *reason; // a substring of the message
	};
	const Refused refused[] = {
		{"not json", "invalid_json", ""},
		{"{\"args\":[\"x\"]}", "invalid_command", "verb"},
		{"{\"verb\":\"Nope\"}", "invalid_command", "no ServerCommand verb"},
		{"{\"verb\":\"KillPlayer\",\"args\":[\"1\"]}", "invalid_command", "needs a target suffix"},
		{"{\"verb\":\"TextChatServer\"}", "invalid_command", "token-count gate"},
		{"{\"verb\":\"SetServerMsg\",\"args\":[\"a\\nb\"]}", "invalid_command", "control byte"},
		{"{\"verb\":\"TextChatServer\",\"args\":[\"\xF0\x9F\x98\x80\"]}", "invalid_command", "U+1F600"},
	};
	for (const Refused &r : refused) {
		reply = command(carol_s, rid, r.body);
		if (!reply.transport_ok || reply.code != 400 || error_of(reply) != r.error ||
		    message_of(reply).find(r.reason) == std::string::npos) {
			std::fprintf(stderr, "  %s -> %s\n", r.body, describe(reply).c_str());
		}
		TEST_EXPECT(reply.transport_ok && reply.code == 400);
		TEST_EXPECT(error_of(reply) == r.error);
		TEST_EXPECT(message_of(reply).find(r.reason) != std::string::npos);
	}
	// SetMPReset is an admin's verb, the owner's own server or not.
	reply = command(alice_s, rid, "{\"verb\":\"SetMPReset\",\"args\":[\"1\"]}");
	TEST_EXPECT(reply.transport_ok && reply.code == 403);
	TEST_EXPECT(error_of(reply) == "verb_not_allowed");
	TEST_EXPECT(mine.wait_notice(Notice::Kind::Command, 300) == nullptr);

	// The owner: each push is queued and reaches the host's session.
	reply = command(alice_s, rid, rename);
	if (reply.code != 202) std::fprintf(stderr, "  owner command: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["status"]) == "queued" && str(json["statement"]) == "ServerCommand");
	TEST_EXPECT(json && json["rid"].i() == static_cast<int64_t>(row->rid));
	TEST_EXPECT(json && str(json["cmd"]) == "SetServerName \"Owner Renamed\"");
	const Notice *cmd = mine.wait_notice(Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr);
	TEST_EXPECT(cmd->command.verb == opennova::ServerCommandVerb::SetServerName);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{"Owner Renamed"});
	mine.clear_notices();
	// "café" in UTF-8 reaches the host in its cp1252: 0xE9 for the é.
	reply = command(alice_s, rid, "{\"verb\":\"TextChatServer\",\"args\":[\"caf\xC3\xA9\"]}");
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["cmd"]) == "TextChatServer \"caf\xC3\xA9\"");
	cmd = mine.wait_notice(Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr && cmd->command.verb == opennova::ServerCommandVerb::TextChatServer);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{"caf\xE9"});
	mine.clear_notices();
	// A player verb, aimed at a roster slot.
	reply = command(alice_s, rid, "{\"verb\":\"KillPlayer\",\"target\":\"ByIndex\",\"args\":[\"0\"]}");
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["cmd"]) == "KillPlayerByIndex 0");
	cmd = mine.wait_notice(Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr && cmd->command.verb == opennova::ServerCommandVerb::KillPlayer);
	TEST_EXPECT(cmd->command.target == opennova::ServerCommandTarget::ByIndex);
	mine.clear_notices();

	// The route reads the live claim, not the row: a row that names Alice with no key of hers
	// claimed for it is a 404 at the route.
	h.pool.acquire()->exec("UPDATE active_hosts SET host_user_id = $1 WHERE rid = $2;",
	                       {BindValue(alice), BindValue(static_cast<int64_t>(anon->rid))});
	TEST_EXPECT(!opennova::hostdb::listing_owned_by(*h.pool.acquire(), anon->rid, alice));
	reply = command(alice_s, anon_rid, rename);
	TEST_EXPECT(reply.transport_ok && reply.code == 404);
	TEST_EXPECT(body_text(reply) == body_text(free_command));
	h.pool.acquire()->exec("UPDATE active_hosts SET host_user_id = NULL WHERE rid = $1;",
	                       {BindValue(static_cast<int64_t>(anon->rid))});
	// The push-time check: a key of Alice's claimed for the anonymous host's RID passes the
	// route's look-up, but that connection never presented it (its lobby names no owner), so the
	// listener refuses the push with the same 404 and nothing reaches the host.
	const std::string stray = "PPPPOOOONNNNMMMMLLLLKKKKJJJJIIIIHHHHGGGGFFFFEEEE";
	TEST_EXPECT(record_key_for(h, stray, alice));
	TEST_EXPECT(opennova::hostdb::claim_host_key(*h.pool.acquire(), stray, anon->rid) == alice);
	TEST_EXPECT(opennova::hostdb::listing_owned_by(*h.pool.acquire(), anon->rid, alice));
	bool lobby_unowned = false;
	for (const auto &entry : h.nw_udp.snapshot_hosted()) {
		if (entry.lobby.rid == anon->rid) lobby_unowned = !entry.lobby.host_user_id.has_value();
	}
	TEST_EXPECT(lobby_unowned);
	reply = command(alice_s, anon_rid, rename);
	TEST_EXPECT(reply.transport_ok && reply.code == 404);
	TEST_EXPECT(body_text(reply) == body_text(free_command));
	TEST_EXPECT(theirs.wait_notice(Notice::Kind::Command, 500) == nullptr);
	h.pool.acquire()->exec("DELETE FROM host_keys WHERE key_hash = $1;",
	                       {BindValue(opennova::hostdb::host_key_hash(stray))});

	// The bucket, per account: Dave's runs dry (the stop draws on it too); Alice's is her own.
	net::HttpReply refusal;
	const int accepted = accepted_before_429(
			[&](int) { return command(dave_s, "999999", rename); }, 40, refusal);
	if (accepted < 8) std::fprintf(stderr, "  dave's bucket: %d before the 429\n", accepted);
	TEST_EXPECT(accepted >= 8);
	TEST_EXPECT(error_of(refusal) == "rate_limited" && is_retry_after(refusal));
	reply = stop(dave_s, "999999");
	TEST_EXPECT(reply.transport_ok && reply.code == 429);

	// The stop: the sysop punt reaches the host and the listing leaves the browser; the RID is
	// then no server of Alice's.
	reply = stop(alice_s, rid);
	TEST_EXPECT(reply.transport_ok && reply.code == 202);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && str(json["statement"]) == "ServerStopHosting" && json["msg_code"].i() == 7);
	const Notice *stopped = mine.wait_notice(Notice::Kind::StopHosting, 3000);
	TEST_EXPECT(stopped != nullptr);
	TEST_EXPECT(stopped->msg_key == "NWUSERVERMSGCODE_NOVAWORLDSYSOPPUNT");
	TEST_EXPECT(wait_until([&] { return !row_named(h, "Owner Push Host"); }));
	reply = command(alice_s, rid, rename);
	TEST_EXPECT(reply.transport_ok && reply.code == 404);

	// The 409: the row back under the RID (written by hand) with Alice's key still claimed for
	// it, so the route's check passes, while her connection no longer hosts. Her bucket first
	// gets a token back (one every 2 s).
	opennova::hostdb::upsert_host(*h.pool.acquire(), *row);
	TEST_EXPECT(opennova::hostdb::listing_owned_by(*h.pool.acquire(), row->rid, alice));
	std::this_thread::sleep_for(std::chrono::milliseconds(2100));
	reply = command(alice_s, rid, rename);
	if (reply.code != 409) std::fprintf(stderr, "  owner 409: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.transport_ok && reply.code == 409);
	TEST_EXPECT(error_of(reply) == "not_hosting");
	TEST_EXPECT(mine.wait_notice(Notice::Kind::Command, 300) == nullptr);
	opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), row->rid);

	// The drain's owner re-check, and a revocation taking the in-memory owner with the keys: with
	// the drain held, Erin's push is queued (202); an admin ban then takes her keys, her session
	// and her listing's in-memory owner (NwUdpListener::forget_owner), so once the drain runs it
	// drops the queued push and her server hears nothing.
	const int64_t erin = make_account(h, "owner_erin", "pw-owner-erin");
	TEST_EXPECT(erin > 0);
	const std::string erin_s = login_token(h, "10.31.0.5", "owner_erin", "pw-owner-erin");
	TEST_EXPECT(!erin_s.empty());
	const std::string erin_key = "ABABABABCDCDCDCDEFEFEFEFGHGHGHGHIJIJIJIJKLKLKLKL";
	TEST_EXPECT(record_key_for(h, erin_key, erin));
	PushHost erins(h.nw_udp.bound_port(), 0x4552494eu);
	TEST_EXPECT(erins.host("Erin Push Host"));
	erins.present_key(erin_key);
	std::optional<opennova::hostdb::HostRow> erin_row;
	TEST_EXPECT(wait_until([&] {
		erin_row = row_named(h, "Erin Push Host");
		return erin_row && erin_row->host_user_id == erin;
	}));
	const std::string erin_rid = std::to_string(erin_row->rid);
	{
		struct DrainHold {
			nws::NwUdpListener &nw_udp;
			~DrainHold() { nw_udp.pause_push_drain(false); }
		} hold{h.nw_udp};
		h.nw_udp.pause_push_drain(true);
		reply = command(erin_s, erin_rid, "{\"verb\":\"TextChatServer\",\"args\":[\"held back\"]}");
		TEST_EXPECT(reply.transport_ok && reply.code == 202);
		reply = h.send("PUT", "/api/admin/users/" + std::to_string(erin), {kJson, kBearer},
		               "{\"account_status\":\"banned\"}");
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		bool erin_unowned = false;
		for (const auto &entry : h.nw_udp.snapshot_hosted()) {
			if (entry.lobby.rid == erin_row->rid) erin_unowned = !entry.lobby.host_user_id.has_value();
		}
		TEST_EXPECT(erin_unowned);
	}
	TEST_EXPECT(erins.wait_notice(Notice::Kind::Command, 1000) == nullptr);
	reply = command(erin_s, erin_rid, rename);
	TEST_EXPECT(reply.transport_ok && reply.code == 401);

	mine.goodbye();
	theirs.goodbye();
	erins.goodbye();
	TEST_EXPECT(wait_until([&] {
		return !row_named(h, "Anonymous Push Host") && !row_named(h, "Erin Push Host");
	}));
	return 0;
}

// A host's own text comes in its code page (a stock host's cp1252) and the public JSON sends it
// as UTF-8: a server whose session names it "Café" with the byte 0xE9 (as a stock host reports
// a name its owner set to "café") is "Café" in /api/lobbies and /api/hosts, and both bodies are
// valid UTF-8. The GSB, which stock clients read, keeps the host's byte.
int test_host_text_utf8(Harness &h) {
	const std::string cp1252_name = "Caf\xE9 Lobby";
	const std::string utf8_name = "Caf\xC3\xA9 Lobby";
	PushHost host(h.nw_udp.bound_port(), 0x55544638u);
	TEST_EXPECT(host.host(cp1252_name));
	TEST_EXPECT(wait_until([&] { return row_named(h, cp1252_name).has_value(); }));

	for (const char *path : {"/api/lobbies", "/api/hosts"}) {
		const auto reply = h.send("GET", path, {kBearer});
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		const std::string text = body_text(reply);
		if (!opennova::is_valid_utf8(text)) std::fprintf(stderr, "  %s is not UTF-8\n", path);
		TEST_EXPECT(opennova::is_valid_utf8(text));
	}
	bool in_lobbies = false;
	const auto lobbies = crow::json::load(body_text(h.send("GET", "/api/lobbies")));
	TEST_EXPECT(lobbies);
	for (const auto &game : lobbies["games"]) {
		for (const auto &e : game["hosts"]) in_lobbies = in_lobbies || str(e["serverName"]) == utf8_name;
	}
	TEST_EXPECT(in_lobbies);
	bool in_hosts = false;
	const auto hosts = crow::json::load(body_text(h.send("GET", "/api/hosts", {kBearer})));
	TEST_EXPECT(hosts);
	for (const auto &e : hosts["hosts"]) in_hosts = in_hosts || str(e["server_name"]) == utf8_name;
	TEST_EXPECT(in_hosts);

	const auto gsb = h.send("GET", "/jop_2.gsb");
	TEST_EXPECT(gsb.transport_ok && gsb.code == 200);
	opennova::GsbResponse parsed;
	TEST_EXPECT(opennova::gsb_parse_response(gsb.body.data(), gsb.body.size(), parsed));
	bool raw_in_gsb = false;
	for (const auto &s : parsed.servers) raw_in_gsb = raw_in_gsb || s.server_name == cp1252_name;
	TEST_EXPECT(raw_in_gsb);

	host.goodbye();
	TEST_EXPECT(wait_until([&] { return !row_named(h, cp1252_name); }));
	return 0;
}

// The static/catch-all family answers a path no root holds with 404.
int test_unknown_path_404(Harness &h) {
	const auto reply = h.send("GET", "/definitely/missing.txt");
	TEST_EXPECT(reply.transport_ok && reply.code == 404);
	return 0;
}

// GET /api/server-info, the OpenNova Switcher's route: the configured public_host
// (the address the gate advertises) with no credential, never cached; the Secure
// listener, which trusts no proxy, answers it as well. A listener whose
// public_host is a hostname answers 503 no_public_ipv4 (a hosts line names an
// address), and so does one whose public_host no client can be sent to.
int test_server_info(Harness &h) {
	for (const uint16_t port : { h.port, h.secure_port }) {
		const auto reply = h.send_to(port, "GET", "/api/server-info", { from_ip("198.51.100.91") });
		if (reply.code != 200)
			std::fprintf(
					stderr, "  :%u %s\n", static_cast<unsigned>(port), describe(reply).c_str());
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
		TEST_EXPECT(body_text(reply) == "{\"novaworld_ip\":\"127.0.0.1\"}");
		TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
		TEST_EXPECT(header_value(reply, "Content-Type") == "application/json");
	}

	for (const char *public_host : { "nw.example.test", "0.0.0.0", "0.1.2.3", "224.0.0.1",
				 "240.0.0.1", "255.255.255.255" }) {
		opennova::ConnectionManager manager;
		nws::ServerConfig config;
		config.public_host = public_host;
		config.http_port = 0;
		config.web_dist_dir = h.temp_root / "web_dist";
		config.templates_dir = h.temp_root / "templates";
		config.static_dir = h.temp_root / "static";
		nws::HttpListener listener(manager, h.pool, h.sessions);
		TEST_EXPECT(listener.start(config));
		const auto refused = h.send_to(listener.bound_port(), "GET", "/api/server-info");
		listener.stop();
		if (refused.code != 503)
			std::fprintf(stderr, "  %s: %s\n", public_host, describe(refused).c_str());
		TEST_EXPECT(refused.transport_ok && refused.code == 503);
		TEST_EXPECT(body_text(refused) == "{\"error\":\"no_public_ipv4\"}");
	}
	return 0;
}

// ---- Request-named files and response header values ----

// What the planted secret files hold; no reply may carry it.
constexpr const char *kSecret = "PLANTED-SECRET-c41f";
// The RID of the host row the join cases reach.
constexpr uint32_t kSplitRid = 4242;

// `text` with every byte but [A-Za-z0-9] percent-encoded: a query value that
// reaches the route as exactly these bytes.
std::string pct(const std::string &text) {
	static const char *kHex = "0123456789ABCDEF";
	std::string out;
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (std::isalnum(byte)) {
			out.push_back(c);
		} else {
			out += '%';
			out += kHex[byte >> 4];
			out += kHex[byte & 0x0F];
		}
	}
	return out;
}

// `text` as a query value with only the bytes a request line cannot carry
// escaped, so "../" and "/" travel as they are.
std::string raw_query(const std::string &text) {
	std::string out;
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte <= 0x20 || byte >= 0x7F || c == '%' || c == '&' || c == '#' || c == '+' ||
				c == '\\') {
			out += pct(std::string(1, c));
		} else {
			out.push_back(c);
		}
	}
	return out;
}

// Template names a request may hand a route that the rule refuses: absolute
// paths, dot segments, a backslash, a drive, a hidden file and a symlink out
// of the root (both planted under the retail templates root).
std::vector<std::string> refused_names(const Harness &h) {
	return {
		"/proc/self/environ",
		"../../../../etc/hostname",
		"../secret.txt",
		"..\\secret.txt",
		(h.retail_root / "secret.txt").generic_string(),
		"C:\\Windows\\win.ini",
		"C:/Windows/win.ini",
		"jop_2_msg.htm/../../secret.txt",
		"./jop_2_msg.htm",
		".hidden.htm",
		"leak.htm",
	};
}

bool carries_secret(const net::HttpReply &reply) {
	return body_text(reply).find(kSecret) != std::string::npos;
}

// The tag a first relay leg set (NWJOINSESSIONTAG), "" when none.
std::string join_tag(const net::HttpReply &reply) {
	return set_cookie_value(reply, "NWJOINSESSIONTAG");
}

// The retail POST /NWLogin.dll to the second listener with `fields` beside the
// credentials, every field EPASK-encrypted as a stock client sends it.
net::HttpReply retail_login(Harness &h, const std::string &name, const std::string &password,
		const std::vector<std::pair<std::string, std::string>> &fields) {
	const auto prepare = h.send_to(h.secure_port, "GET", "/nwprepare.dll?url=jop_2_start.htm");
	opennova::EpaskParams epask;
	if (!opennova::epask_from_string(set_cookie_value(prepare, "EPASK"), epask))
		return {};
	std::vector<opennova::LoginFormField> form = { { "EPASK", opennova::epask_to_string(epask),
														   false },
		{ "NAME", name, true }, { "PASSWORD", password, true }, { "pfid", "28", true } };
	for (const auto &[field, value] : fields)
		form.push_back({ field, value, true });
	std::string body;
	if (!opennova::build_login_post_body(epask, form, body))
		return {};
	return h.send_to(h.secure_port, "POST", "/NWLogin.dll",
			{ "Content-Type: application/x-www-form-urlencoded" }, body);
}

// The rules themselves: every retail template name is a bare file name; a
// path out of its root (a dot segment, an empty segment, a symlink) resolves
// to nothing; a cookie value with a byte that could split the response or
// forge a cookie goes out empty, while every value the service sends a stock
// client, and a handle with a space, goes out as it is.
int test_name_and_value_rules(Harness &h) {
	const std::filesystem::path source_templates =
			std::filesystem::path(OPENNOVA_SOURCE_DIR) / "apps" / "novaworld_server" / "templates";
	int templates = 0;
	for (const auto &entry : std::filesystem::directory_iterator(source_templates)) {
		const std::string name = entry.path().filename().string();
		if (!nws::is_bare_file_name(name))
			std::fprintf(stderr, "  not a bare name: %s\n", name.c_str());
		TEST_EXPECT(nws::is_bare_file_name(name));
		TEST_EXPECT(nws::template_file((h.retail_root / "templates").string(), name).has_value());
		++templates;
	}
	TEST_EXPECT(templates >= 30);
	for (const std::string &name : refused_names(h)) {
		if (name == "leak.htm")
			continue; // a bare name; its target is what is refused
		TEST_EXPECT(!nws::is_bare_file_name(name));
	}
	const auto templates_root = (h.retail_root / "templates").string();
	TEST_EXPECT(!nws::template_file(templates_root, "leak.htm").has_value());
	TEST_EXPECT(!nws::template_file(templates_root, "jop_2/../jop_2_msg.htm").has_value());
	const auto static_root = h.retail_root / "static";
	TEST_EXPECT(nws::file_under_root(static_root, "jop_2/nonbhdx2.tga").has_value());
	TEST_EXPECT(!nws::file_under_root(static_root, "jop_2//nonbhdx2.tga").has_value());
	TEST_EXPECT(!nws::file_under_root(static_root, "/etc/hostname").has_value());
	TEST_EXPECT(!nws::file_under_root(static_root, "jop_2/../../secret.txt").has_value());
	TEST_EXPECT(!nws::file_under_root(static_root, "leak.tga").has_value());
	TEST_EXPECT(!nws::file_under_root(static_root, "jop_2").has_value()); // a directory

	for (const char *value : { "3", "2345", "jop:cus2", "us", "127.0.0.1", "::ffff:10.0.0.7",
				 "65537:3233:ABCDEFGHIJKLMNOP", "NWServer:NWLogin.dll:SESSIONTAG:123:0a1b2c3d",
				 "A-A02-085D18", "5e550001", "ljim", "Space Handle", "Jim's \"quoted\"",
				 "JT:0a000013143f1efe6c8000767fd748e1127304c5b6d728", "" }) {
		TEST_EXPECT(nws::cookie_value_ok(value));
		TEST_EXPECT(nws::set_cookie_text("NWHANDLE", value, "Path=/") ==
				std::string("NWHANDLE=") + value + "; Path=/");
	}
	for (const std::string &value :
			{ std::string("a\rb"), std::string("a\nb"), std::string("a\0b", 3), std::string("a;b"),
					std::string("a,b"), std::string("a\tb"), std::string("a\x7f") }) {
		TEST_EXPECT(!nws::cookie_value_ok(value));
		TEST_EXPECT(nws::set_cookie_text("NWHANDLE", value, "Path=/") == "NWHANDLE=; Path=/");
	}
	TEST_EXPECT(nws::set_cookie_text("BAD NAME", "x", "Path=/").empty());
	TEST_EXPECT(nws::set_cookie_text("A\r\nB", "x", "Path=/").empty());
	TEST_EXPECT(nws::header_text_ok("text/html; charset=a\tb"));
	TEST_EXPECT(!nws::header_text_ok("a\r\nX-Injected: 1"));
	return 0;
}

// Every template route answers a name the rule refuses as it answers a
// missing template, with no file's bytes: the absolute and dot-segment names,
// raw and percent-encoded, the backslash and drive names, a hidden file and a
// symlink planted in the templates root, on /NWAccount.dll and
// /NWCharacter.dll (404), /NWLogout.dll ("Logged out."), /nwprepare.dll,
// /NWStart.dll, both /NWJoin.dll and /NWHost.dll legs, and the retail
// POST /NWLogin.dll's msgbase / failure page (a bad password) and its relay and
// success pages (a good one): an empty page.
int test_template_names_refused(Harness &h) {
	auto get = [&h](const std::string &path, const std::vector<std::string> &headers = {}) {
		return h.send_to(h.secure_port, "GET", path, headers);
	};
	auto expect = [](const net::HttpReply &reply, int code, const std::string &body,
						  const std::string &what) {
		const bool ok = reply.transport_ok && reply.code == code && body_text(reply) == body &&
				!carries_secret(reply);
		if (!ok)
			std::fprintf(stderr, "  %s: %s\n", what.c_str(), describe(reply).c_str());
		return ok;
	};
	{
		opennova::hostdb::HostRow row;
		row.rid = kSplitRid;
		row.gsid = "GSID-SPLIT";
		row.game = "jop_2_consumer";
		row.app_id = "20";
		row.server_name = "Split Srv";
		row.host_ip = "10.0.0.42";
		row.host_port = 32770;
		auto conn = h.pool.acquire();
		opennova::hostdb::upsert_host(*conn, row);
	}

	for (const std::string &name : refused_names(h)) {
		// The 404 names only a bare file name (the symlink's).
		const std::string not_found =
				"template not found: " + (nws::is_bare_file_name(name) ? name : std::string());
		for (const std::string &q : { pct(name), raw_query(name) }) {
			const std::string what = "'" + nws::loggable(name) + "' as " + q;
			TEST_EXPECT(expect(
					get("/NWAccount.dll?success=" + q), 404, not_found, "NWAccount " + what));
			TEST_EXPECT(expect(
					get("/nwcharacter.dll?success=" + q), 404, not_found, "nwcharacter " + what));
			TEST_EXPECT(expect(
					get("/NWLogout.dll?success=" + q), 200, "Logged out.", "NWLogout " + what));
			TEST_EXPECT(expect(get("/nwprepare.dll?url=" + q), 200, "", "nwprepare " + what));
			TEST_EXPECT(expect(get("/NWStart.dll?OUT=" + q), 200, "", "NWStart " + what));
			TEST_EXPECT(
					expect(get("/NWJoin.dll?rid=1&relay=" + q), 200, "", "NWJoin relay " + what));
			TEST_EXPECT(
					expect(get("/NWHost.dll?pfid=28&relay=" + q), 200, "", "NWHost relay " + what));

			const auto join_first =
					get("/NWJoin.dll?rid=" + std::to_string(kSplitRid) + "&success=" + q);
			TEST_EXPECT(join_first.transport_ok && !join_tag(join_first).empty());
			TEST_EXPECT(expect(
					get("/NWJoin.dll", { "Cookie: NWJOINSESSIONTAG=" + join_tag(join_first) }), 200,
					"", "NWJoin success " + what));
			const auto host_first = get("/NWHost.dll?pfid=28&success=" + q);
			TEST_EXPECT(host_first.transport_ok && !join_tag(host_first).empty());
			TEST_EXPECT(expect(
					get("/NWHost.dll", { "Cookie: NWJOINSESSIONTAG=" + join_tag(host_first) }), 200,
					"", "NWHost success " + what));
		}
	}

	// The retail login: three names, a bad password then a good one each.
	TEST_EXPECT(make_account(h, "tpl_user", "pw-tpl") > 0);
	for (const std::string &name : { std::string("/proc/self/environ"),
				 std::string("../secret.txt"), (h.retail_root / "secret.txt").generic_string() }) {
		const std::string what = "'" + name + "'";
		const auto bad = retail_login(
				h, "tpl_user", "wrong-pw", { { "msgbase", name }, { "failure", name } });
		TEST_EXPECT(expect(bad, 200, "", "NWLogin msgbase " + what));
		TEST_EXPECT(set_cookie_value(bad, "LOGINSESSIONTAG").empty());

		const auto good =
				retail_login(h, "tpl_user", "pw-tpl", { { "relay", name }, { "success", name } });
		TEST_EXPECT(expect(good, 200, "", "NWLogin relay " + what));
		const std::string tag = set_cookie_value(good, "LOGINSESSIONTAG");
		TEST_EXPECT(!tag.empty());
		TEST_EXPECT(expect(get("/NWLogin.dll", { "Cookie: LOGINSESSIONTAG=" + tag }), 200, "",
				"NWLogin success " + what));
	}
	return 0;
}

// Every retail template still renders, by the names a stock client sends:
// /nwprepare.dll?url= fills each in (with no gate tag, a DFX2 page's game is
// DFX2's by its prefix, its browser list /dfx2_0.gsb; every other page's JO's),
// the catch-all serves each as it is, the retail static file serves, and the
// default pages render.
int test_retail_templates_render(Harness &h) {
	const auto templates_root = h.retail_root / "templates";
	const std::string base = "http://127.0.0.1:" + std::to_string(h.secure_port);
	for (const auto &entry : std::filesystem::directory_iterator(templates_root)) {
		const std::string name = entry.path().filename().string();
		if (name == "leak.htm" || name == ".hidden.htm")
			continue; // the planted ones
		const nws::TemplateVars vars{
			{ "HOST_URL", base + "/nwhost.dll" },
			{ "GSB_SERVER", base + (name.rfind("dfx2_0_", 0) == 0 ? "/dfx2_0.gsb" : "/jop_2.gsb") },
			{ "JOINLAN_URL", "" },
		};
		const std::string text = test_io::read_file_text(entry.path().string());
		TEST_EXPECT(!text.empty());
		const auto rendered = h.send_to(h.secure_port, "GET", "/nwprepare.dll?url=" + name);
		if (body_text(rendered) != nws::render_template(text, vars)) {
			std::fprintf(
					stderr, "  %s did not render: %s\n", name.c_str(), describe(rendered).c_str());
		}
		TEST_EXPECT(rendered.transport_ok && rendered.code == 200);
		TEST_EXPECT(body_text(rendered) == nws::render_template(text, vars));
		const auto served = h.send_to(h.secure_port, "GET", "/" + name);
		TEST_EXPECT(served.transport_ok && served.code == 200 && body_text(served) == text);
	}
	const auto tga = h.send_to(h.secure_port, "GET", "/static/jop_2/nonbhdx2.tga");
	TEST_EXPECT(tga.transport_ok && tga.code == 200);
	TEST_EXPECT(tga.body ==
			test_io::read_file((h.retail_root / "static" / "jop_2" / "nonbhdx2.tga").string()));
	const auto asset = h.send_to(h.secure_port, "GET", "/assets/app.js");
	TEST_EXPECT(asset.transport_ok && asset.code == 200 && body_text(asset) == "console.log(1);");

	const auto account = h.send_to(h.secure_port, "GET", "/NWAccount.dll?success=jop_2_main.htm");
	TEST_EXPECT(account.transport_ok && account.code == 200 && !body_text(account).empty());
	const auto logout = h.send_to(h.secure_port, "GET", "/NWLogout.dll");
	TEST_EXPECT(logout.transport_ok && logout.code == 200);
	const nws::TemplateVars jo_vars{
		{ "HOST_URL", base + "/nwhost.dll" },
		{ "GSB_SERVER", base + "/jop_2.gsb" },
		{ "JOINLAN_URL", "" },
	};
	TEST_EXPECT(body_text(logout) ==
			nws::render_template(
					test_io::read_file_text((templates_root / "jop_2_login.htm").string()),
					jo_vars));
	return 0;
}

// The catch-all static route serves nothing outside its roots, on either
// listener: an absolute path after the route's slash ("//etc/..."), dot
// segments raw or percent-encoded, a backslash, a hidden file, and a symlink
// in each retail root that leads to the secret file.
int test_catch_all_stays_in_root(Harness &h) {
	for (const auto &[port, root] :
			{ std::pair<uint16_t, std::filesystem::path>{ h.port, h.temp_root },
					{ h.secure_port, h.retail_root } }) {
		const std::string secret = (root / "secret.txt").generic_string();
		for (const std::string &path : { "/" + secret, "/static/" + secret, "/" + secret.substr(1),
					 std::string("/static/../secret.txt"), std::string("/%2e%2e/secret.txt"),
					 std::string("/%2e%2e%2fsecret.txt"), std::string("/static/..%2fsecret.txt"),
					 std::string("/static/%2e%2e%2f%2e%2e%2fsecret.txt"),
					 std::string("/..%5csecret.txt"),
					 std::string("/static/jop_2/..%5c..%5c..%5csecret.txt"),
					 std::string("/.hidden.htm"), std::string("/leak.htm"),
					 std::string("/static/leak.tga"), std::string("/webleak.txt"),
					 std::string("/static/jop_2/../../secret.txt") }) {
			const auto reply = h.send_to(port, "GET", path);
			const bool ok = reply.transport_ok && reply.code != 200 && !carries_secret(reply);
			if (!ok) {
				std::fprintf(stderr, "  GET %s on :%u: %s\n", path.c_str(),
						static_cast<unsigned>(port), describe(reply).c_str());
			}
			TEST_EXPECT(ok);
		}
	}
	return 0;
}

// Whether a reply's head is the one the route built: no header line holds a
// CR, LF or NUL, none is an injected one, and nothing injected spilled into
// the body.
bool split_free(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return false;
	for (const std::string &line : reply.headers) {
		if (line.find_first_of(std::string("\r\n\0", 3)) != std::string::npos)
			return false;
		std::string lower = line.substr(0, 10);
		for (char &c : lower)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (lower == "x-injected")
			return false;
	}
	return body_text(reply).find("X-Injected") == std::string::npos;
}

bool has_set_cookie(const net::HttpReply &reply, const std::string &text) {
	const auto cookies = header_values(reply, "Set-Cookie");
	return std::find(cookies.begin(), cookies.end(), text) != cookies.end();
}

// A CR, LF, NUL, ';' or ',' in any value a Set-Cookie carries never splits the
// response or forges a cookie: the cookie goes out empty. The sources: the
// /nwprepare.dll query (VER1, VER2, GT, CC), a stored nwhandle (NWHANDLE, CHAR
// at /NWLogin.dll and /NWAccount.dll: /api/register and the admin create and
// update refuse a handle with a control byte, so these are written to the
// table directly), an admin-set PCID and expansion bits (PCID, EXPBITS), and
// the pfid a first relay leg stored (NWPF on the second /NWHost.dll and
// /NWJoin.dll leg). The values a stock client sends, and a handle with a
// space, go out byte for byte.
int test_header_values_cannot_split(Harness &h) {
	// The /nwprepare.dll query: retail's own values, then injected ones.
	auto reply = h.send("GET", "/nwprepare.dll?ver1=3&ver2=2345&cc=us&gt=jop:cus2&url=urls.htm");
	TEST_EXPECT(split_free(reply));
	for (const char *cookie : { "VER1=3; Path=/", "VER2=2345; Path=/", "GT=jop:cus2; Path=/",
				 "CC=us; Path=/", "YOURIP=127.0.0.1; Path=/" }) {
		TEST_EXPECT(has_set_cookie(reply, cookie));
	}
	const std::string epask = set_cookie_value(reply, "EPASK");
	TEST_EXPECT(std::count(epask.begin(), epask.end(), ':') == 2);
	TEST_EXPECT(has_set_cookie(reply, "EPASK=" + epask + "; Path=/"));
	reply = h.send("GET",
			"/nwprepare.dll?url=urls.htm&ver1=" + pct("3\r\nX-Injected: v1") + "&ver2=" +
					pct("2345\nX-Injected: v2") + "&gt=" + pct("jop:cus2\rX-Injected: gt") +
					"&cc=" + pct("us; Domain=evil.example"));
	TEST_EXPECT(split_free(reply));
	for (const char *cookie : { "VER1=; Path=/", "VER2=; Path=/", "GT=; Path=/", "CC=; Path=/" }) {
		if (!has_set_cookie(reply, cookie))
			std::fprintf(stderr, "  no Set-Cookie %s\n", cookie);
		TEST_EXPECT(has_set_cookie(reply, cookie));
	}
	// A query's %00 ends the value Crow hands the route (a C string), so a NUL
	// reaches add_cookie only from a stored value; the rules case covers it.
	reply = h.send("GET",
			"/nwprepare.dll?url=urls.htm&ver1=" + pct("3,X-Injected=1") +
					"&ver2=" + pct("2345\r\n\r\nX-Injected: body"));
	TEST_EXPECT(split_free(reply));
	TEST_EXPECT(has_set_cookie(reply, "VER1=; Path=/") && has_set_cookie(reply, "VER2=; Path=/"));
	TEST_EXPECT(body_text(reply) ==
			"http://127.0.0.1:" + std::to_string(h.port) + "/nwhost.dll|" +
					"http://127.0.0.1:" + std::to_string(h.port) + "/jop_2.gsb");

	// A stored nwhandle, and expansion bits an admin set: the retail login's
	// relay and completion. /api/register refuses the handle itself.
	auto set_handle = [&h](const std::string &username, const std::string &handle) {
		h.pool.acquire()->exec("UPDATE players SET nwhandle = $1 WHERE username = $2;",
				{ opennova::db::BindValue(handle), opennova::db::BindValue(username) });
	};
	reply = h.send("POST", "/api/register", { kJson, kCsrf, from_ip("10.41.0.1") },
			"{\"username\":\"split_reg\",\"password\":\"pw-split\",\"email\":\"split@example.com\","
			"\"nwhandle\":\"Reg\\r\\nX-Injected: reg\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "invalid_field");
	reply = h.send("POST", "/api/register", { kJson, kCsrf, from_ip("10.41.0.1") },
			"{\"username\":\"split_reg\",\"password\":\"pw-split\",\"email\":\"split@example.com\","
			"\"nwhandle\":\"Reg\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 201);
	set_handle("split_reg", "Reg\r\nX-Injected: reg");
	const auto reg = nws::get_user_by_username(*h.pool.acquire(), "split_reg");
	TEST_EXPECT(reg.has_value());
	reply = h.send("PUT", "/api/admin/users/" + std::to_string(reg->id) + "/game-access",
			{ kJson, kBearer },
			"{\"game_slug\":\"jop_2_consumer\",\"status\":\"active\","
			"\"exp_bits\":\"3\\r\\nX-Injected: exp\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	const auto relay = retail_login(h, "split_reg", "pw-split", {});
	TEST_EXPECT(split_free(relay));
	TEST_EXPECT(has_set_cookie(relay, "EXPBITS=; Path=/"));
	const std::string tag = set_cookie_value(relay, "LOGINSESSIONTAG");
	TEST_EXPECT(!tag.empty());
	reply = h.send_to(h.secure_port, "GET", "/NWLogin.dll", { "Cookie: LOGINSESSIONTAG=" + tag });
	TEST_EXPECT(split_free(reply));
	for (const char *cookie : { "NWHANDLE=; Path=/", "CHAR=; Path=/", "EXPBITS=; Path=/" }) {
		TEST_EXPECT(has_set_cookie(reply, cookie));
	}
	TEST_EXPECT(has_set_cookie(reply, "PCID=" + reg->pcid + "; Path=/"));

	// A stored nwhandle (the admin create and update refuse it), then a PCID
	// from the admin update: /NWAccount.dll refreshes them for the request's own
	// retail login.
	reply = h.send("POST", "/api/admin/users", { kJson, kBearer },
			"{\"username\":\"split_admin\",\"password\":\"pw-split\",\"pcid\":\"5p11a001\","
			"\"nwhandle\":\"Adm\\nX-Injected: adm\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	reply = h.send("POST", "/api/admin/users", { kJson, kBearer },
			"{\"username\":\"split_admin\",\"password\":\"pw-split\",\"pcid\":\"5p11a001\","
			"\"nwhandle\":\"Adm\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 201);
	set_handle("split_admin", "Adm\nX-Injected: adm");
	const std::string admin_tag =
			set_cookie_value(retail_login(h, "split_admin", "pw-split", {}), "LOGINSESSIONTAG");
	TEST_EXPECT(!admin_tag.empty());
	const std::vector<std::string> as_admin_user = { "Cookie: LOGINSESSIONTAG=" + admin_tag };
	reply = h.send_to(h.secure_port, "GET", "/NWAccount.dll", as_admin_user);
	TEST_EXPECT(split_free(reply) && reply.code == 200);
	TEST_EXPECT(
			has_set_cookie(reply, "NWHANDLE=; Path=/") && has_set_cookie(reply, "CHAR=; Path=/"));
	TEST_EXPECT(has_set_cookie(reply, "PCID=5p11a001; Path=/"));
	const auto adm = nws::get_user_by_username(*h.pool.acquire(), "split_admin");
	TEST_EXPECT(adm.has_value());
	reply = h.send("PUT", "/api/admin/users/" + std::to_string(adm->id), { kJson, kBearer },
			"{\"nwhandle\":\"Upd\\r\\n\\r\\nX-Injected: upd\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	reply = h.send("PUT", "/api/admin/users/" + std::to_string(adm->id), { kJson, kBearer },
			"{\"pcid\":\"p,c;id\\r\\nX-Injected: pcid\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	set_handle("split_admin", "Upd\r\n\r\nX-Injected: upd");
	reply = h.send_to(h.secure_port, "GET", "/NWCharacter.dll", as_admin_user);
	TEST_EXPECT(split_free(reply) && reply.code == 200);
	TEST_EXPECT(
			has_set_cookie(reply, "NWHANDLE=; Path=/") && has_set_cookie(reply, "PCID=; Path=/"));

	// A handle with a space is a real one: byte for byte.
	{
		nws::CreateUserParams p;
		p.username = "split_space";
		p.password = "pw-split";
		p.pcid = "5p11a002";
		p.nwh = "1";
		p.nwhandle = "Space Handle";
		TEST_EXPECT(nws::create_user(*h.pool.acquire(), p).ok);
	}
	const std::string space_tag =
			set_cookie_value(retail_login(h, "split_space", "pw-split", {}), "LOGINSESSIONTAG");
	TEST_EXPECT(!space_tag.empty());
	reply = h.send_to(
			h.secure_port, "GET", "/NWAccount.dll", { "Cookie: LOGINSESSIONTAG=" + space_tag });
	TEST_EXPECT(split_free(reply) && reply.code == 200);
	TEST_EXPECT(has_set_cookie(reply, "NWHANDLE=Space Handle; Path=/"));
	TEST_EXPECT(has_set_cookie(reply, "CHAR=Space Handle; Path=/"));
	TEST_EXPECT(has_set_cookie(reply, "PCID=5p11a002; Path=/"));

	// The pfid a first relay leg stored, sent back as NWPF on the second.
	const std::string pfid = pct("28\r\nX-Injected: pf");
	const auto host_first =
			h.send_to(h.secure_port, "GET", "/NWHost.dll?success=jop_2_host2.htm&pfid=" + pfid);
	TEST_EXPECT(split_free(host_first) && !join_tag(host_first).empty());
	reply = h.send_to(h.secure_port, "GET", "/NWHost.dll",
			{ "Cookie: NWJOINSESSIONTAG=" + join_tag(host_first) });
	TEST_EXPECT(split_free(reply) && reply.code == 200);
	TEST_EXPECT(has_set_cookie(reply, "NWPF=; Path=/"));
	const auto join_first = h.send_to(
			h.secure_port, "GET", "/NWJoin.dll?rid=" + std::to_string(kSplitRid) + "&pfid=" + pfid);
	TEST_EXPECT(split_free(join_first) && !join_tag(join_first).empty());
	reply = h.send_to(h.secure_port, "GET", "/NWJoin.dll",
			{ "Cookie: NWJOINSESSIONTAG=" + join_tag(join_first) });
	TEST_EXPECT(split_free(reply) && reply.code == 200);
	TEST_EXPECT(has_set_cookie(reply, "NWPF=; Path=/"));
	// A stock client's pfid goes out as it came.
	const auto host_ok =
			h.send_to(h.secure_port, "GET", "/NWHost.dll?success=jop_2_host2.htm&pfid=28");
	reply = h.send_to(h.secure_port, "GET", "/NWHost.dll",
			{ "Cookie: NWJOINSESSIONTAG=" + join_tag(host_ok) });
	TEST_EXPECT(split_free(reply) && has_set_cookie(reply, "NWPF=28; Path=/"));

	auto conn = h.pool.acquire();
	opennova::hostdb::remove_host_by_rid(*conn, kSplitRid);
	return 0;
}

// ---- The credentials generation, retail identity, the API's proxy, page values

// Whether /NWAccount.dll on the third listener, with `cookies`, names
// `handle`: its identity cookies set to the account's handle.
bool account_page_names(
		Harness &h, const std::vector<std::string> &cookies, const std::string &handle) {
	const auto reply =
			h.send_to(h.fresh_port, "GET", "/NWAccount.dll?success=jop_2_main.htm", cookies);
	return reply.transport_ok && reply.code == 200 &&
			has_set_cookie(reply, "NWHANDLE=" + handle + "; Path=/") &&
			has_set_cookie(reply, "CHAR=" + handle + "; Path=/");
}

// /NWAccount.dll on the third listener with `cookies` names no account: no
// identity cookie at all.
bool account_page_anonymous(Harness &h, const std::vector<std::string> &cookies) {
	const auto reply =
			h.send_to(h.fresh_port, "GET", "/NWAccount.dll?success=jop_2_main.htm", cookies);
	return reply.transport_ok && reply.code == 200 &&
			header_values(reply, "Set-Cookie").size() == 1 &&
			!set_cookie_value(reply, "YOURIP").empty();
}

// The race a password reset can run against a login, made deterministic: the
// seam commits an admin reset of the account inside each password check,
// after the password matched and before the route grants anything (to the
// same password, so the credentials generation alone moves). The site login
// answers the generic 401 with no cookie and mints no session; the retail
// login (on the third listener) answers a bad password's page with no session
// tag, writes no session row and stores no pin; neither remembers its
// address. Without the seam the same logins succeed. Then a reset made behind
// the HTTP layer's back (a direct update_user, which drops no pin) ends the
// next login's session tag and pin all the same: the generation refuses them.
int test_reset_during_login(Harness &h) {
	const int64_t id = make_account(h, "race_user", "pw-race");
	TEST_EXPECT(id > 0);
	const std::vector<std::string> pin = { "Cookie: PERSISTENTEXPRESSLOGINDATA=pin-race-user" };
	// Another connection's write: on a thread of its own, since the seam runs
	// on the login's thread, which holds the login's lease (one per thread).
	auto reset = [&h](int64_t user_id) {
		bool ok = false;
		std::thread([&h, user_id, &ok] {
			nws::UpdateUserParams p;
			p.password_plaintext = "pw-race";
			ok = nws::update_user(*h.pool.acquire(), user_id, p).ok;
		}).join();
		return ok;
	};
	auto generation = [&h, id] {
		return nws::get_user_by_id(*h.pool.acquire(), id)->credentials_gen;
	};
	auto known = [&h](const std::string &address) {
		return nws::is_known_login_address(*h.pool.acquire(), "race_user", address);
	};
	auto rows = [&h, id](const std::string &table) {
		return h.pool.acquire()
				->query("SELECT COUNT(*) FROM " + table + " WHERE user_id = $1;",
						{ opennova::db::BindValue(id) })
				.front()
				.as_int(0)
				.value_or(-1);
	};

	std::atomic<int> resets{ 0 };
	nws::set_password_accepted_hook([&reset, &resets](int64_t user_id) {
		if (reset(user_id))
			resets.fetch_add(1);
	});
	const auto web = login(h, "10.31.0.1", "race_user", "pw-race");
	const auto retail = nwlogin_to(h, h.fresh_port, "race_user", "pw-race", pin);
	nws::set_password_accepted_hook(nullptr);
	TEST_EXPECT(resets.load() == 2);
	TEST_EXPECT(generation() == 2);
	TEST_EXPECT(web.transport_ok && web.code == 401);
	TEST_EXPECT(body_text(web) == "{\"error\":\"invalid_credentials\"}");
	TEST_EXPECT(header_values(web, "Set-Cookie").empty());
	TEST_EXPECT(rows("web_sessions") == 0);
	TEST_EXPECT(retail.transport_ok && retail.code == 200);
	TEST_EXPECT(set_cookie_value(retail, "LOGINSESSIONTAG").empty());
	TEST_EXPECT(body_text(retail).find("Invalid username or password") != std::string::npos);
	TEST_EXPECT(rows("active_user_sessions") == 0);
	TEST_EXPECT(!known("10.31.0.1") && !known("127.0.0.1"));
	TEST_EXPECT(
			set_cookie_value(nwlogin_to(h, h.fresh_port, "", "", pin), "LOGINSESSIONTAG").empty());

	// Without the seam: the same logins succeed and grant.
	TEST_EXPECT(login(h, "10.31.0.1", "race_user", "pw-race").code == 200);
	TEST_EXPECT(rows("web_sessions") == 1);
	const auto good = nwlogin_to(h, h.fresh_port, "race_user", "pw-race", pin);
	TEST_EXPECT(nwlogin_succeeded(good));
	TEST_EXPECT(known("10.31.0.1") && known("127.0.0.1"));
	const std::vector<std::string> tag = { "Cookie: LOGINSESSIONTAG=" +
		set_cookie_value(good, "LOGINSESSIONTAG") };
	TEST_EXPECT(account_page_names(h, tag, "race_user"));
	TEST_EXPECT(account_page_names(h, pin, "race_user"));

	// A reset behind the HTTP layer's back: the tag and the pin stop counting.
	TEST_EXPECT(reset(id));
	TEST_EXPECT(account_page_anonymous(h, tag));
	TEST_EXPECT(account_page_anonymous(h, pin));
	TEST_EXPECT(
			set_cookie_value(nwlogin_to(h, h.fresh_port, "", "", pin), "LOGINSESSIONTAG").empty());
	return 0;
}

constexpr uint32_t kJoinRid = 4343;

// The /NWJoin.dll second leg on the third listener, as a stock client follows
// the first: the NWJOINSESSIONTAG the first set, beside `cookies`
// ("NAME=value" each).
net::HttpReply join(Harness &h, const std::vector<std::string> &cookies) {
	const auto first =
			h.send_to(h.fresh_port, "GET", "/NWJoin.dll?rid=" + std::to_string(kJoinRid));
	std::string jar = "Cookie: NWJOINSESSIONTAG=" + join_tag(first);
	for (const std::string &cookie : cookies)
		jar += "; " + cookie;
	return h.send_to(h.fresh_port, "GET", "/NWJoin.dll", { jar });
}

// The account the join just recorded in host_players for the test host (-1:
// no row). Each join from the harness replaces the one row its address keeps.
int64_t joined_user(Harness &h) {
	const auto rows =
			h.pool.acquire()->query("SELECT user_id FROM host_players WHERE host_rid = $1;",
					{ opennova::db::BindValue(static_cast<int64_t>(kJoinRid)) });
	return rows.empty() ? -1 : rows.front().as_int(0).value_or(0);
}

void forget_joins(Harness &h) {
	h.pool.acquire()->exec("DELETE FROM host_players WHERE host_rid = $1;",
			{ opennova::db::BindValue(static_cast<int64_t>(kJoinRid)) });
}

// `text` with each of `fills`' markers replaced by its text, verbatim: the
// expected page built without the renderer under test.
std::string fill_markers(
		std::string text, const std::vector<std::pair<std::string, std::string>> &fills) {
	for (const auto &[marker, value] : fills) {
		for (auto at = text.find(marker); at != std::string::npos;
				at = text.find(marker, at + value.size())) {
			text.replace(at, marker.size(), value);
		}
	}
	return text;
}

// The .joi /NWJoin.dll's second leg renders for the join cases' host, its
// server name given as the page must carry it. NK and CK are written out: the
// host's "10.0.0.43:32771" and app id "20", each char plus its key char
// ("diheijefhgcdjcgcjcfbd", "cfhdcegjigecjehcgjdhe") less '0'.
std::string expected_joi(Harness &h, const std::string &server_name_as_sent) {
	return fill_markers(
			test_io::read_file_text((h.retail_root / "templates" / "jop_2_join.joi").string()),
			{ { "{{NK}}", "eifegjcjkqffqjh" }, { "{{CK}}", "ef" }, { "{{NI}}", "10.0.0.43" },
					{ "{{NP}}", "32771" }, { "{{BK}}", "986119" },
					{ "{{SERVER_NAME}}", server_name_as_sent } });
}

// /NWJoin.dll takes its joiner from server-side state only: the request's own
// retail login (its LOGINSESSIONTAG, else its PERSISTENTEXPRESSLOGINDATA pin),
// never the NWHANDLE cookie, which names an account but proves nothing. An
// identified joiner gets the PUB* identity and a host_players row, the .joi
// page byte for byte; an NWHANDLE alone gets neither, and beside a login it
// changes nothing. A ban (an admin update) ends both cookies' standing, so the
// banned joiner is anonymous; an account made inactive with no such update
// (its login still standing) is refused with the msgbase failure page and the
// game login's NWEC11 / NWEC12 message. /NWAccount.dll resolves the same way.
int test_join_identity(Harness &h) {
	{
		opennova::hostdb::HostRow row;
		row.rid = kJoinRid;
		row.gsid = "GSID-JOIN";
		row.game = "jop_2_consumer";
		row.app_id = "20";
		row.server_name = "Plain Server 7";
		row.host_ip = "10.0.0.43";
		row.host_port = 32771;
		row.pcid_key = "pcid-key-for-the-join-case";
		opennova::hostdb::upsert_host(*h.pool.acquire(), row);
	}
	const int64_t joiner = make_account(h, "joiner_ok", "pw-joiner");
	const int64_t victim = make_account(h, "join_victim", "pw-victim");
	const int64_t other = make_account(h, "joiner_b", "pw-joiner-b");
	TEST_EXPECT(joiner > 0 && victim > 0 && other > 0);
	const std::string pin = "PERSISTENTEXPRESSLOGINDATA=pin-joiner";
	const auto logged_in =
			nwlogin_to(h, h.fresh_port, "joiner_ok", "pw-joiner", { "Cookie: " + pin });
	TEST_EXPECT(nwlogin_succeeded(logged_in));
	const std::string tag = "LOGINSESSIONTAG=" + set_cookie_value(logged_in, "LOGINSESSIONTAG");
	auto identified = [](const net::HttpReply &reply) {
		return reply.transport_ok && reply.code == 200 &&
				!set_cookie_value(reply, "PUBPCID").empty() &&
				!set_cookie_value(reply, "PUBNAMEINFO").empty();
	};
	auto anonymous = [](const net::HttpReply &reply) {
		return reply.transport_ok && reply.code == 200 &&
				has_set_cookie(reply, "PUBPCID=; Path=/") &&
				has_set_cookie(reply, "PUBNAMEINFO=; Path=/");
	};

	forget_joins(h);
	auto reply = join(h, { tag });
	TEST_EXPECT(identified(reply));
	TEST_EXPECT(body_text(reply) == expected_joi(h, "Plain Server 7"));
	TEST_EXPECT(
			body_text(reply).find("[NK=eifegjcjkqffqjh&CK=ef&NI=10.0.0.43&NP=32771&BK=986119&]") !=
			std::string::npos);
	TEST_EXPECT(joined_user(h) == joiner);

	forget_joins(h);
	TEST_EXPECT(identified(join(h, { pin })));
	TEST_EXPECT(joined_user(h) == joiner);

	// The handle cookie alone: nobody; beside the login: the login.
	forget_joins(h);
	reply = join(h, { "NWHANDLE=join_victim" });
	TEST_EXPECT(anonymous(reply));
	TEST_EXPECT(body_text(reply) == expected_joi(h, "Plain Server 7"));
	TEST_EXPECT(joined_user(h) == -1);
	TEST_EXPECT(identified(join(h, { "NWHANDLE=join_victim", tag })));
	TEST_EXPECT(joined_user(h) == joiner);
	TEST_EXPECT(account_page_anonymous(h, { "Cookie: NWHANDLE=join_victim" }));
	TEST_EXPECT(account_page_names(h, { "Cookie: NWHANDLE=join_victim; " + tag }, "joiner_ok"));

	// A ban through the admin route: the login and its pin stand for nobody.
	reply = h.send("PUT", "/api/admin/users/" + std::to_string(joiner), { kJson, kBearer },
			"{\"account_status\":\"banned\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	forget_joins(h);
	TEST_EXPECT(anonymous(join(h, { tag, pin })));
	TEST_EXPECT(joined_user(h) == -1);
	TEST_EXPECT(account_page_anonymous(h, { "Cookie: " + tag }));

	// Inactive with its login still standing: refused, the failure page.
	const auto other_login = nwlogin_to(h, h.fresh_port, "joiner_b", "pw-joiner-b");
	TEST_EXPECT(nwlogin_succeeded(other_login));
	const std::string other_tag =
			"LOGINSESSIONTAG=" + set_cookie_value(other_login, "LOGINSESSIONTAG");
	const std::string msg_page =
			test_io::read_file_text((h.retail_root / "templates" / "jop_2_msg.htm").string());
	for (const auto &[status, message] :
			{ std::pair<const char *, const char *>{
					  "banned", "This NovaWorld account has been banned. NWEC11" },
					{ "disabled", "This NovaWorld account is restricted. NWEC12" } }) {
		h.pool.acquire()->exec("UPDATE players SET account_status = $1 WHERE id = $2;",
				{ opennova::db::BindValue(std::string(status)), opennova::db::BindValue(other) });
		forget_joins(h);
		reply = join(h, { other_tag });
		// The message page (its one marker is MESSAGE), filled by hand.
		const std::string expected = fill_markers(msg_page, { { "{{MESSAGE}}", message } });
		TEST_EXPECT(expected.find("{{") == std::string::npos);
		TEST_EXPECT(reply.transport_ok && reply.code == 200 && body_text(reply) == expected);
		TEST_EXPECT(header_values(reply, "Set-Cookie").size() == 1); // YOURIP alone
		TEST_EXPECT(joined_user(h) == -1);
		TEST_EXPECT(account_page_anonymous(h, { "Cookie: " + other_tag }));
	}
	h.pool.acquire()->exec("UPDATE players SET account_status = 'active' WHERE id = $1;",
			{ opennova::db::BindValue(other) });
	forget_joins(h);
	TEST_EXPECT(identified(join(h, { other_tag })));
	TEST_EXPECT(joined_user(h) == other);
	TEST_EXPECT(account_page_names(h, { "Cookie: " + other_tag }, "joiner_b"));

	forget_joins(h);
	opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), kJoinRid);
	return 0;
}

// ONNET_API_PROXY_ONLY: the third listener answers /api (and everything under
// it, a route or not, any method) with 403 {"error":"proxy_required"} to a
// peer that is not its trusted proxy, while its retail routes, the .gsb feed,
// its templates and its static files answer that peer as ever. The harness
// listener, whose trusted proxy the harness is, answers /api/ (every website
// case here runs through it); the Secure listener, with the setting off,
// answers its /api/ to anyone.
int test_api_proxy_only(Harness &h) {
	for (const auto &[method, path] :
			{ std::pair<const char *, const char *>{ "GET", "/api/health" }, { "GET", "/api" },
					{ "GET", "/api/lobbies" }, { "GET", "/api/server-info" },
					{ "GET", "/api/no-such-route" }, { "POST", "/api/login" },
					{ "POST", "/api/register" }, { "GET", "/api/admin/users" },
					{ "PUT", "/api/admin/server-status" }, { "GET", "/api/me/hosts" },
					{ "POST", "/api/me/hosts/1/command" }, { "POST", "/api/me/hosts/1/stop" },
					{ "GET", "/api/admin/hosts" }, { "POST", "/api/admin/hosts/1/command" },
					{ "POST", "/api/admin/hosts/1/stop" } }) {
		const auto reply = h.send_to(h.fresh_port, method, path, { kJson, kCsrf, kBearer }, "{}");
		if (reply.code != 403)
			std::fprintf(stderr, "  %s %s: %s\n", method, path, describe(reply).c_str());
		TEST_EXPECT(reply.transport_ok && reply.code == 403);
		TEST_EXPECT(body_text(reply) == "{\"error\":\"proxy_required\"}");
	}
	for (const char *path : { "/nwprepare.dll?url=jop_2_start.htm", "/NWStart.dll", "/NWLogout.dll",
				 "/NWAccount.dll", "/jop_2.gsb", "/jop_2_main.htm", "/static/jop_2/nonbhdx2.tga",
				 "/assets/app.js" }) {
		const auto reply = h.send_to(h.fresh_port, "GET", path);
		if (reply.code != 200)
			std::fprintf(stderr, "  GET %s: %s\n", path, describe(reply).c_str());
		TEST_EXPECT(reply.transport_ok && reply.code == 200);
	}
	TEST_EXPECT(h.send("GET", "/api/health").code == 200);
	TEST_EXPECT(h.send_to(h.secure_port, "GET", "/api/health").code == 200);
	return 0;
}

// A value a page is filled with is text: & < > " ' go out as entities, which
// a stock client's page parser turns back into the characters, so a request's
// IN / OUT / MSGBASE on /NWStart.dll, a listed server's name in the .joi page
// and an admin's maintenance message on the login failure page add no markup.
// Every value the service and a stock client actually use (A-P keys, session
// tags, page names, numbers, addresses, URLs, names with spaces) goes out byte
// for byte, so the pages a stock client gets are the template with the value
// in it, unchanged.
int test_template_values_escaped(Harness &h) {
	for (const std::string &plain : { std::string("ABCDEFGHIJKLMNOPABCDEFGHIJKLMNOP"),
				 std::string("NWServer:NWLogin.dll:SESSIONTAG:123:0a1b2c3d"),
				 std::string("jop_2_main.htm"), std::string("32771"), std::string("10.0.0.43"),
				 std::string("http://127.0.0.1:8080/nwhost.dll"), std::string("Space Handle"),
				 std::string("Contacting login databases..."), std::string("") }) {
		TEST_EXPECT(nws::html_escape(plain) == plain);
		TEST_EXPECT(nws::render_template("[{{V}}]", { { "V", plain } }) == "[" + plain + "]");
	}
	TEST_EXPECT(nws::html_escape("Tom & Jerry's <b>\"Base\"</b>") ==
			"Tom &amp; Jerry&#39;s &lt;b&gt;&quot;Base&quot;&lt;/b&gt;");
	TEST_EXPECT(nws::render_template("{{ V }}|{{MISSING}}", { { "V", "<i>" } }) ==
			"&lt;i&gt;|{{MISSING}}");

	// /NWStart.dll: the login page with IN / OUT / MSGBASE in its form.
	const std::string login_page =
			test_io::read_file_text((h.retail_root / "templates" / "jop_2_login.htm").string());
	auto start = [&h](const std::string &in, const std::string &msgbase) {
		return h.send_to(h.secure_port, "GET",
				"/NWStart.dll?OUT=jop_2_login.htm&IN=" + pct(in) + "&MSGBASE=" + pct(msgbase));
	};
	auto filled = [&login_page](const std::string &in, const std::string &msgbase) {
		std::string page = login_page;
		for (const auto &[key, value] : { std::pair<std::string, std::string>{ "{{IN}}", in },
					 { "{{OUT}}", "jop_2_login.htm" }, { "{{MSGBASE}}", msgbase } }) {
			const auto at = page.find(key);
			if (at != std::string::npos)
				page.replace(at, key.size(), value);
		}
		return page;
	};
	auto reply = start("jop_2_main.htm", "jop_2_msg.htm");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(body_text(reply) == filled("jop_2_main.htm", "jop_2_msg.htm"));
	reply = start("x\"><b>in</b>", "m'><i>");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(
			body_text(reply) == filled("x&quot;&gt;&lt;b&gt;in&lt;/b&gt;", "m&#39;&gt;&lt;i&gt;"));
	TEST_EXPECT(body_text(reply).find("<b>in</b>") == std::string::npos);

	// A listed server's own name in the .joi page.
	{
		opennova::hostdb::HostRow row;
		row.rid = kJoinRid;
		row.gsid = "GSID-JOIN";
		row.game = "jop_2_consumer";
		row.app_id = "20";
		row.server_name = "Tom & Jerry's <b>Base</b>";
		row.host_ip = "10.0.0.43";
		row.host_port = 32771;
		opennova::hostdb::upsert_host(*h.pool.acquire(), row);
	}
	reply = join(h, {});
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(
			body_text(reply) == expected_joi(h, "Tom &amp; Jerry&#39;s &lt;b&gt;Base&lt;/b&gt;"));
	TEST_EXPECT(body_text(reply).find("Joining Tom &amp; Jerry&#39;s &lt;b&gt;Base&lt;/b&gt;...") !=
			std::string::npos);
	opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), kJoinRid);

	// An admin's maintenance message on the retail login's failure page.
	reply = h.send("PUT", "/api/admin/server-status", { kJson, kBearer },
			"{\"maintenance_enabled\":true,\"message\":\"Back <b>soon</b> & then some\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	reply = nwlogin_to(h, h.fresh_port, "nobody", "x");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(body_text(reply).find("Back &lt;b&gt;soon&lt;/b&gt; &amp; then some") !=
			std::string::npos);
	TEST_EXPECT(body_text(reply).find("<b>soon</b>") == std::string::npos);
	reply = h.send("PUT", "/api/admin/server-status", { kJson, kBearer },
			"{\"maintenance_enabled\":false}");
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	return 0;
}

// POST /api/register's handle: the nwhandle given, else the username, takes
// the handle rule (at most 30 bytes, the longest a stock host reads whole; no
// control byte): a 31-byte one, given or defaulted, is 400 invalid_field and
// adds no account; a 30-byte one registers.
int test_register_handle_rule(Harness &h) {
	auto reg = [&h](const std::string &ip, const std::string &body) {
		return h.send("POST", "/api/register", { kJson, kCsrf, from_ip(ip) }, body);
	};
	const std::size_t before = nws::list_users(*h.pool.acquire()).size();
	for (const std::string &body :
			{ "{\"username\":\"handle_long\",\"password\":\"pw\",\"email\":\"hl@example.com\",\"nwhandle\":\"" +
							std::string(31, 'h') + "\"}",
					"{\"username\":\"" + std::string(31, 'u') + "\",\"password\":\"pw\",\"email\":\"h31@example.com\"}",
					std::string("{\"username\":\"handle_tab\",\"password\":\"pw\",\"email\":\"ht@example.com\","
								"\"nwhandle\":\"a\\tb\"}") }) {
		const auto reply = reg("10.42.0.1", body);
		TEST_EXPECT(reply.transport_ok && reply.code == 400);
		TEST_EXPECT(str(crow::json::load(body_text(reply))["error"]) == "invalid_field");
	}
	TEST_EXPECT(nws::list_users(*h.pool.acquire()).size() == before);
	auto reply = reg("10.42.0.2", "{\"username\":\"" + std::string(30, 'u') +
					"\",\"password\":\"pw\",\"email\":\"h30@example.com\"}");
	TEST_EXPECT(reply.transport_ok && reply.code == 201);
	TEST_EXPECT(
			str(crow::json::load(body_text(reply))["user"]["nwhandle"]) == std::string(30, 'u'));
	return 0;
}

// A retail login's session row stays alive while the login is used: each
// request it names its account by (here /NWJoin.dll and /NWAccount.dll)
// restarts the row's idle clock, so the main tick's 2 h sweep keeps a login
// used 1 h 50 min after its GET /NWLogin.dll, and the join after it still
// names the joiner. A row left idle past the 2 h is swept, and its tag then
// names no one.
int test_login_row_kept_alive(Harness &h) {
	{
		opennova::hostdb::HostRow row;
		row.rid = kJoinRid;
		row.gsid = "GSID-JOIN";
		row.game = "jop_2_consumer";
		row.app_id = "20";
		row.server_name = "Plain Server 7";
		row.host_ip = "10.0.0.43";
		row.host_port = 32771;
		row.pcid_key = "pcid-key-for-the-join-case";
		opennova::hostdb::upsert_host(*h.pool.acquire(), row);
	}
	const int64_t id = make_account(h, "lobby_sitter", "pw-sitter");
	TEST_EXPECT(id > 0);
	const auto logged_in = nwlogin_to(h, h.fresh_port, "lobby_sitter", "pw-sitter");
	TEST_EXPECT(nwlogin_succeeded(logged_in));
	const std::string tag = "LOGINSESSIONTAG=" + set_cookie_value(logged_in, "LOGINSESSIONTAG");
	auto age = [&h, id](const char *ago) {
		h.pool.acquire()->exec(
				std::string("UPDATE active_user_sessions SET last_seen_at = (now() + interval '") +
						ago + "') WHERE user_id = $1;",
				{ opennova::db::BindValue(id) });
	};
	auto fresh_row = [&h, id] {
		return !h.pool.acquire()
						->query("SELECT 1 FROM active_user_sessions WHERE user_id = $1 "
								"AND last_seen_at >= (now() + interval '-5 seconds');",
								{ opennova::db::BindValue(id) })
						.empty();
	};
	auto sweep = [&h] {
		return nws::evict_active_user_sessions_older_than(*h.pool.acquire(), 2 * 60 * 60);
	};
	auto joined_by_login = [&h, &tag] {
		const auto reply = join(h, { tag });
		return reply.transport_ok && reply.code == 200 &&
				!set_cookie_value(reply, "PUBPCID").empty();
	};

	age("-110 minutes");
	TEST_EXPECT(joined_by_login());
	TEST_EXPECT(fresh_row());
	sweep();
	TEST_EXPECT(joined_by_login());

	age("-110 minutes");
	TEST_EXPECT(account_page_names(h, { "Cookie: " + tag }, "lobby_sitter"));
	TEST_EXPECT(fresh_row());

	age("-121 minutes");
	TEST_EXPECT(sweep() >= 1);
	const auto reply = join(h, { tag });
	TEST_EXPECT(
			reply.transport_ok && reply.code == 200 && has_set_cookie(reply, "PUBPCID=; Path=/"));
	forget_joins(h);
	opennova::hostdb::remove_host_by_rid(*h.pool.acquire(), kJoinRid);
	return 0;
}

// /NWHost.dll's second leg serves a tag's HOSTKEY once: the same tag again is
// the unknown-tag 400, with no key.
int test_host_tag_serves_once(Harness &h) {
	const auto first =
			h.send_to(h.fresh_port, "GET", "/NWHost.dll?success=jop_2_host2.htm&pfid=28");
	const std::string tag = join_tag(first);
	TEST_EXPECT(!tag.empty());
	const auto session = h.sessions.get_host(tag);
	TEST_EXPECT(session.has_value());
	const std::vector<std::string> jar = { "Cookie: NWJOINSESSIONTAG=" + tag };
	auto reply = h.send_to(h.fresh_port, "GET", "/NWHost.dll", jar);
	TEST_EXPECT(reply.transport_ok && reply.code == 200);
	TEST_EXPECT(body_text(reply).find("[HOSTKEY=" + session->host_key + "&]") != std::string::npos);
	TEST_EXPECT(!h.sessions.get_host(tag).has_value());
	reply = h.send_to(h.fresh_port, "GET", "/NWHost.dll", jar);
	TEST_EXPECT(reply.transport_ok && reply.code == 400);
	TEST_EXPECT(body_text(reply).find(session->host_key) == std::string::npos);
	return 0;
}

// A database failure's reply is its error code and nothing else: the
// db::Error's text names its SQLSTATE, the server's message and the failing
// statement ("— INSERT INTO players ..."), and goes to the server log alone. A
// trigger fails each route's write the way any db::Error does: POST
// /api/register (the public route, create_user's db_error), PUT
// /api/admin/server-status and POST /api/admin/hosts (its upsert_failed).
// Nothing lands.
int test_db_failure_reply_carries_code_alone(Harness &h) {
	const char *const triggers[][2] = {
		{ "harness_fail_players", "players" },
		{ "harness_fail_server_status", "server_status" },
		{ "harness_fail_active_hosts", "active_hosts" },
	};
	h.pool.acquire()->exec("CREATE FUNCTION harness_fail() RETURNS trigger LANGUAGE plpgsql AS "
						   "$$BEGIN RAISE EXCEPTION 'harness forced failure'; END$$;");
	for (const auto &t : triggers) {
		h.pool.acquire()->exec(std::string("CREATE TRIGGER ") + t[0] + " BEFORE INSERT ON " + t[1] +
				" FOR EACH ROW EXECUTE FUNCTION harness_fail();");
	}
	auto refused = [](const net::HttpReply &reply, const char *want) {
		if (reply.transport_ok && reply.code == 500 && body_text(reply) == want)
			return true;
		std::fprintf(stderr, "  %s, want HTTP 500 %s\n", describe(reply).c_str(), want);
		return false;
	};
	const auto users_before = nws::list_users(*h.pool.acquire()).size();
	const auto status_before = nws::get_server_status(*h.pool.acquire());

	TEST_EXPECT(refused(h.send("POST", "/api/register", { kJson, kCsrf, from_ip("10.43.0.1") },
								"{\"username\":\"db_failure\",\"password\":\"pw-db-failure\","
								"\"email\":\"db_failure@example.com\"}"),
			"{\"error\":\"db_error\"}"));
	TEST_EXPECT(refused(h.send("PUT", "/api/admin/server-status", { kBearer, kJson },
								"{\"maintenance_enabled\":false,\"message\":\"never\"}"),
			"{\"error\":\"db_error\"}"));
	TEST_EXPECT(refused(h.send("POST", "/api/admin/hosts", { kBearer, kJson }, "{\"rid\":4343}"),
			"{\"error\":\"upsert_failed\"}"));

	for (const auto &t : triggers)
		h.pool.acquire()->exec(std::string("DROP TRIGGER ") + t[0] + " ON " + t[1] + ";");
	h.pool.acquire()->exec("DROP FUNCTION harness_fail();");
	TEST_EXPECT(nws::list_users(*h.pool.acquire()).size() == users_before);
	TEST_EXPECT(nws::get_server_status(*h.pool.acquire()).message == status_before.message);
	TEST_EXPECT(h.pool.acquire()->query("SELECT 1 FROM active_hosts WHERE rid = 4343;").empty());
	return 0;
}

// GET /healthz (health.h): outside /api, so a listener whose /api/ refuses the
// harness (a proxy it is not) still answers it; "ok" while the stall watchdog
// finds its loops moving and main()'s checks coming, 503 "stalled" (no detail)
// once its watched loop has not moved for the stall time, or no check came for
// that long, and "ok" again as soon as the loop moves. A listener with no
// watchdog answers "ok".
int test_healthz(Harness &h) {
	const auto expect_reply = [&h](int code, const char *body) {
		const net::HttpReply reply = h.send("GET", "/healthz");
		if (reply.code != code || body_text(reply) != body)
			std::fprintf(stderr, "  /healthz: %s, want %d %s\n", describe(reply).c_str(), code, body);
		return reply.code == code && body_text(reply) == body &&
				header_value(reply, "Cache-Control") == "no-store";
	};
	const uint64_t stall = nws::Watchdog::kStallMs;
	TEST_EXPECT(h.watchdog->check().empty());
	TEST_EXPECT(expect_reply(200, "ok"));
	auto reply = h.send_to(h.fresh_port, "GET", "/healthz");
	TEST_EXPECT(reply.code == 200 && body_text(reply) == "ok");
	TEST_EXPECT(h.send_to(h.fresh_port, "GET", "/api/health").code == 403);
	reply = h.send_to(h.secure_port, "GET", "/healthz");
	TEST_EXPECT(reply.code == 200 && body_text(reply) == "ok");

	// The loop stands still: healthy until the stall time, then stalled, by name.
	g_watch_clock_ms += stall - 1;
	TEST_EXPECT(h.watchdog->check().empty());
	TEST_EXPECT(expect_reply(200, "ok"));
	g_watch_clock_ms += 1;
	TEST_EXPECT(h.watchdog->check() == "harness loop");
	TEST_EXPECT(expect_reply(503, "stalled"));
	// It moves: healthy at once, before main()'s next check.
	++g_watch_turns;
	TEST_EXPECT(expect_reply(200, "ok"));
	TEST_EXPECT(h.watchdog->check().empty());
	// main()'s own loop stands still: no check for the stall time is stalled too,
	// whatever the loops do.
	g_watch_clock_ms += stall;
	++g_watch_turns;
	TEST_EXPECT(expect_reply(503, "stalled"));
	TEST_EXPECT(h.watchdog->check().empty());
	TEST_EXPECT(expect_reply(200, "ok"));
	// The heartbeat line's shape.
	nws::Heartbeat beat;
	beat.nw_sessions = 3;
	beat.hosted_failed = 1;
	TEST_EXPECT(nws::heartbeat_line(beat) ==
			"{\"heartbeat\":{\"nw_sessions\":3,\"listed_hosts\":0,\"hosted_live\":0,\"hosted_failed\":1,"
			"\"machines_lost\":0,\"launch_failures\":0,\"billing_sync_age_s\":-1,\"webhook_rejects\":0,"
			"\"limits_held\":0}}");
	beat.limits_held = 1;
	TEST_EXPECT(nws::heartbeat_line(beat).find(",\"limits_held\":1}}") != std::string::npos);
	return 0;
}

// ---- hosted servers ----------------------------------------------------------

std::string error_of_reply(const net::HttpReply &reply) {
	const auto json = crow::json::load(body_text(reply));
	return json && json.has("error") ? str(json["error"]) : std::string("<none>");
}

// Every hosted row still stopping, ended as the supervisor would end it (the
// harness runs none), so the cases after see no live server.
void end_stopping_servers(opennova::db::Database &db) {
	db.exec("UPDATE hosted_servers SET state = 'stopped' WHERE state = 'stopping';");
}

void end_stopping_servers(Harness &h) {
	end_stopping_servers(*h.pool.acquire());
}

// The hosted servers' routes (hosted/hosted_routes.h; the harness listener runs
// them with ONNET_HOSTED_PROVIDER set, a cap of 4 and two offered data sets,
// Joint Operations' with the default rotation "default.bms" and Delta Force
// Xtreme 2's with its own, and no supervisor, so rows stay where the routes
// put them): the session and CSRF refusals, the data sets the owner's view
// lists, the comp grant (PUT /api/admin/users/<id>/comp-servers, admin only),
// the settings' refusals (the game among them), the starts (on the first data
// set when it names none, and on Delta Force Xtreme 2's with its rotation) up
// to the account's hosting entitlement (server_limit with its counts past it,
// a stopping server counting), the cap and `full`, another account's id
// answering 404, the owner's stop by id (409 not_live once ended), the
// own-machine add-on (no_own_machine without one), the list's order (the live
// servers, then the newest 10 ended), the fleet view and the admin's stops
// (admin only, never the admin password), and a listener with hosting off.
int test_hosted_server_routes(Harness &h) {
	namespace hosted = opennova::novaworld_server::hosted;
	namespace billing = opennova::novaworld_server::billing;
	struct Owner {
		int64_t id = 0;
		std::string session;
	};
	const auto owner = [&h](const char *name, const char *ip) {
		Owner o;
		o.id = make_account(h, name, std::string("pw-") + name);
		o.session = login_token(h, ip, name, std::string("pw-") + name);
		return o;
	};
	const Owner ana = owner("hosted_ana", "10.51.0.1");
	const Owner ben = owner("hosted_ben", "10.51.0.2");
	const Owner cy = owner("hosted_cy", "10.51.0.3");
	const Owner dee = owner("hosted_dee", "10.51.0.4");
	const Owner eve = owner("hosted_eve", "10.51.0.5");
	const Owner fay = owner("hosted_fay", "10.51.0.6");
	const Owner gus = owner("hosted_gus", "10.51.0.7");
	for (const Owner *o : { &ana, &ben, &cy, &dee, &eve, &fay, &gus }) TEST_EXPECT(o->id > 0 && !o->session.empty());
	// The start / stop bucket (HostedRoutes::kStartStopPerUser) is 6 per account,
	// then one every 20 s, and a request refused before it (logged out, no CSRF
	// header) draws nothing. Ana draws exactly six below (three starts, three
	// stops), Dee six (the refused settings, then not_entitled), the others fewer:
	// add a draw to one of them and it must move to a new account.

	auto start = [&h](const Owner &o, const std::string &body, bool csrf = true) {
		std::vector<std::string> headers = { kJson, with_session(o.session) };
		if (csrf) headers.push_back(kCsrf);
		return h.send("POST", "/api/me/servers", headers, body);
	};
	auto stop = [&h](const Owner &o, int64_t id, bool csrf = true) {
		std::vector<std::string> headers = { with_session(o.session) };
		if (csrf) headers.push_back(kCsrf);
		return h.send("POST", "/api/me/servers/" + std::to_string(id) + "/stop", headers);
	};
	auto list = [&h](const Owner &o) { return h.send("GET", "/api/me/servers", { with_session(o.session) }); };
	auto one = [&h](const Owner &o, int64_t id) {
		return h.send("GET", "/api/me/servers/" + std::to_string(id), { with_session(o.session) });
	};
	auto grant = [&h](int64_t id, const std::string &body, std::vector<std::string> headers) {
		headers.push_back(kJson);
		return h.send("PUT", "/api/admin/users/" + std::to_string(id) + "/comp-servers", headers, body);
	};
	const auto entitlement_is = [](const crow::json::rvalue &json, int64_t max, int64_t used, int64_t own,
										int64_t own_used, bool can_start, const char *source) {
		if (!json || !json.has("entitlement")) return false;
		const auto &e = json["entitlement"];
		return e["max_servers"].i() == max && e["used"].i() == used && e["own_machines"].i() == own &&
				e["own_used"].i() == own_used && e["can_start"].b() == can_start && str(e["source"]) == source;
	};
	const std::string settings =
			"{\"game_name\":\"Ana's Server\",\"max_players\":16,\"missions\":[\"first.bms\",\"second.bms\"]}";

	// Logged out: 401; no CSRF header on a state change: 403, before any bucket.
	TEST_EXPECT(h.send("GET", "/api/me/servers").code == 401);
	TEST_EXPECT(h.send("GET", "/api/me/servers/1").code == 401);
	TEST_EXPECT(h.send("POST", "/api/me/servers", { kJson, kCsrf }, settings).code == 401);
	TEST_EXPECT(h.send("POST", "/api/me/servers/1/stop", { kCsrf }).code == 401);
	auto reply = start(ana, settings, /*csrf=*/false);
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "csrf_header_required");
	reply = stop(ana, 1, /*csrf=*/false);
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "csrf_header_required");

	// No hosting entitlement yet, no server.
	reply = list(ana);
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["enabled"].b() && !json["full"].b());
	TEST_EXPECT(entitlement_is(json, 0, 0, 0, 0, false, "none"));
	TEST_EXPECT(json["servers"].t() == crow::json::type::List && json["servers"].size() == 0);
	// The data sets the provider offers, in the service's order (Joint
	// Operations first; OpenNova's has no folder here, so it is not offered):
	// each one's game, name, expansions, rotation (the missions the site's
	// picker offers) and limits.
	TEST_EXPECT(json.has("data_sets") && json["data_sets"].size() == 2);
	if (json.has("data_sets") && json["data_sets"].size() == 2) {
		const auto &jo = json["data_sets"][0];
		const auto &dfx2 = json["data_sets"][1];
		TEST_EXPECT(str(jo["game"]) == "jop_2_consumer" && str(jo["display_name"]) == "Joint Operations");
		TEST_EXPECT(jo["expansions"].size() == 2 && str(jo["expansions"][0]["code"]).empty() &&
				str(jo["expansions"][1]["code"]) == "jox01" &&
				str(jo["expansions"][1]["name"]) == "Escalation (jox01)");
		TEST_EXPECT(jo["default_missions"].size() == 1 && str(jo["default_missions"][0]) == "default.bms");
		TEST_EXPECT(jo["limits"]["max_players"]["min"].i() == 2 && jo["limits"]["max_players"]["max"].i() == 64);
		TEST_EXPECT(jo["limits"]["kill_limit"]["max"].i() == 500 && jo["limits"]["start_delay"]["min"].i() == 0);
		TEST_EXPECT(str(dfx2["game"]) == "dfx2_consumer" && str(dfx2["display_name"]) == "Delta Force Xtreme 2");
		TEST_EXPECT(dfx2["expansions"].size() == 1 && str(dfx2["expansions"][0]["code"]).empty());
		TEST_EXPECT(dfx2["default_missions"].size() == 2 && str(dfx2["default_missions"][1]) == "dfx2_b.bms");
		TEST_EXPECT(dfx2["limits"].has("respawn") && dfx2["limits"]["time_limit"]["max"].i() == 300);
		TEST_EXPECT(dfx2["limits"]["max_players"]["max"].i() == 100);
	}
	TEST_EXPECT(!json.has("default_missions"));
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");

	// Settings the backend refuses (hosted_supervisor has every rule): a field and why,
	// a mistyped field, no JSON. Checked before the entitlement, so dee needs none.
	struct Refused {
		const char *body;
		const char *error;
		const char *field;
	};
	const Refused refused[] = {
		{ "{\"game_name\":\"\",\"missions\":[\"a.bms\"]}", "invalid_settings", "game_name" },
		{ "{\"game_name\":\"x\",\"max_players\":99,\"missions\":[\"a.bms\"]}", "invalid_settings",
				"max_players" },
		{ "{\"game_name\":\"x\",\"missions\":[\"../a.bms\"]}", "invalid_settings", "missions" },
		{ "{\"game_name\":\"x\",\"max_players\":\"16\"}", "invalid_field", "" },
		{ "not json", "invalid_json", "" },
	};
	for (const Refused &r : refused) {
		reply = start(dee, r.body);
		json = crow::json::load(body_text(reply));
		if (reply.code != 400 || error_of_reply(reply) != r.error)
			std::fprintf(stderr, "  %s -> %s\n", r.body, describe(reply).c_str());
		TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == r.error);
		if (*r.field) TEST_EXPECT(json && str(json["field"]) == r.field && json.has("message"));
	}
	// A valid start with no entitlement.
	reply = start(dee, settings);
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "not_entitled");
	// The game: one the service offers (OpenNova's is known, but not offered
	// here), whose expansions the expansion names; own_machine a flag.
	const Refused game_refused[] = {
		{ "{\"game\":\"jop_3_consumer\",\"game_name\":\"x\",\"missions\":[\"a.bms\"]}", "invalid_settings", "game" },
		{ "{\"game\":\"opennova_0_consumer\",\"game_name\":\"x\",\"missions\":[\"a.bms\"]}", "invalid_settings",
				"game" },
		{ "{\"game\":\"dfx2_consumer\",\"game_name\":\"x\",\"expansion\":\"jox01\"}", "invalid_settings",
				"expansion" },
		{ "{\"game\":7,\"game_name\":\"x\"}", "invalid_field", "" },
		{ "{\"game_name\":\"x\",\"own_machine\":1}", "invalid_field", "" },
	};
	for (const Refused &r : game_refused) {
		reply = start(eve, r.body);
		json = crow::json::load(body_text(reply));
		if (reply.code != 400 || error_of_reply(reply) != r.error)
			std::fprintf(stderr, "  %s -> %s\n", r.body, describe(reply).c_str());
		TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == r.error);
		if (*r.field) TEST_EXPECT(json && str(json["field"]) == r.field && json.has("message"));
	}

	// The comp grant: admin only (the token, or an admin session with the CSRF
	// header), a whole number 0..16.
	TEST_EXPECT(grant(ana.id, "{\"comp_servers\":1}", {}).code == 401);
	TEST_EXPECT(grant(ana.id, "{\"comp_servers\":1}", { with_session(ana.session), kCsrf }).code == 403);
	reply = grant(ana.id, "{}", { kBearer });
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "missing_field");
	for (const char *bad : { "{\"comp_servers\":true}", "{\"comp_servers\":1.5}", "{\"comp_servers\":17}",
				 "{\"comp_servers\":-1}", "{\"comp_servers\":\"2\"}" }) {
		reply = grant(ana.id, bad, { kBearer });
		TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_field");
	}
	TEST_EXPECT(grant(999999, "{\"comp_servers\":1}", { kBearer }).code == 404);
	reply = grant(ana.id, "{\"comp_servers\":2}", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["user"]["comp_servers"].i() == 2 &&
			json["user"]["max_servers"].i() == 2);
	for (const Owner *o : { &ben, &cy, &fay, &gus })
		TEST_EXPECT(grant(o->id, "{\"comp_servers\":1}", { kBearer }).code == 200);
	TEST_EXPECT(entitlement_is(crow::json::load(body_text(list(ana))), 2, 0, 0, 0, true, "comp"));

	// Two starts: 202 with each server, requested, on a shared machine.
	reply = start(ana, settings);
	json = crow::json::load(body_text(reply));
	if (reply.code != 202) std::fprintf(stderr, "  start: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 202 && json && str(json["server"]["state"]) == "requested");
	const int64_t ana_first = json["server"]["id"].i();
	TEST_EXPECT(json["server"]["live"].b() && !json["server"]["own_machine"].b());
	TEST_EXPECT(str(json["server"]["settings"]["game_name"]) == "Ana's Server");
	// No game named: the first the service offers, with its names.
	TEST_EXPECT(str(json["server"]["settings"]["game"]) == "jop_2_consumer");
	TEST_EXPECT(str(json["server"]["game_display_name"]) == "Joint Operations");
	TEST_EXPECT(str(json["server"]["expansion_display_name"]) == "Joint Operations (base game)");
	TEST_EXPECT(json["server"]["settings"]["max_players"].i() == 16);
	TEST_EXPECT(json["server"]["settings"]["missions"].size() == 2);
	TEST_EXPECT(str(json["server"]["settings"]["missions"][0]) == "first.bms");
	TEST_EXPECT(json["server"]["listing"].t() == crow::json::type::Null);
	reply = one(ana, ana_first);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["server"]["id"].i() == ana_first);
	TEST_EXPECT(json["server"]["events"].size() == 1);
	TEST_EXPECT(str(json["server"]["events"][0]["from"]).empty() &&
			str(json["server"]["events"][0]["to"]) == "requested");
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	// The second takes the default rotation when it names none.
	reply = start(ana, "{\"game_name\":\"Ana's Second\"}");
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json["server"]["settings"]["missions"].size() == 1);
	TEST_EXPECT(str(json["server"]["settings"]["missions"][0]) == "default.bms");
	const int64_t ana_second = json["server"]["id"].i();
	// The third: every server the grant allows is live.
	reply = start(ana, settings);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "server_limit");
	TEST_EXPECT(json && json["max_servers"].i() == 2 && json["used"].i() == 2 && json.has("message"));
	reply = list(ana);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(entitlement_is(json, 2, 2, 0, 0, true, "comp") && json["servers"].size() == 2);
	TEST_EXPECT(json["servers"][0]["id"].i() == ana_first && json["servers"][1]["id"].i() == ana_second);
	TEST_EXPECT(!json["servers"][0].has("events"));

	// The cap (4): two more fill it, and the next start finds it full.
	reply = start(ben, "{\"game_name\":\"Ben's\"}");
	TEST_EXPECT(reply.code == 202);
	const int64_t ben_server = crow::json::load(body_text(reply))["server"]["id"].i();
	reply = start(cy, "{\"game\":\"dfx2_consumer\",\"game_name\":\"Cy's\"}");
	json = crow::json::load(body_text(reply));
	if (reply.code != 202) std::fprintf(stderr, "  dfx2 start: %s\n", describe(reply).c_str());
	// Delta Force Xtreme 2's server takes that game's default rotation.
	TEST_EXPECT(reply.code == 202 && str(json["server"]["settings"]["game"]) == "dfx2_consumer");
	TEST_EXPECT(str(json["server"]["game_display_name"]) == "Delta Force Xtreme 2");
	TEST_EXPECT(json["server"]["settings"]["missions"].size() == 2 &&
			str(json["server"]["settings"]["missions"][0]) == "dfx2_a.bms");
	const int64_t cy_server = json["server"]["id"].i();
	TEST_EXPECT(crow::json::load(body_text(list(ana)))["full"].b());
	reply = start(fay, "{\"game_name\":\"Fay's\"}");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "at_capacity");

	// Another account's server, by id: 404, as no server's is.
	for (const int64_t id : { ana_first, int64_t{ 999999 } }) {
		reply = one(ben, id);
		TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	}
	reply = stop(ben, ana_first);
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	TEST_EXPECT(hosted::find_server(*h.pool.acquire(), ana_first)->state == hosted::State::Requested);

	// A stopping server still counts: Ben's stop, then his start is server_limit.
	reply = stop(ben, ben_server);
	TEST_EXPECT(reply.code == 202 && str(crow::json::load(body_text(reply))["server"]["state"]) == "stopping");
	reply = start(ben, "{\"game_name\":\"Ben's again\"}");
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "server_limit" && json["used"].i() == 1 &&
			json["max_servers"].i() == 1);

	// The fleet view: admin only; it names the owner and never the admin password.
	h.pool.acquire()->exec("UPDATE hosted_servers SET admin_password = 'planted-admin-secret' WHERE id = $1;",
			{ opennova::db::BindValue(ana_first) });
	TEST_EXPECT(h.send("GET", "/api/admin/hosted").code == 401);
	TEST_EXPECT(h.send("GET", "/api/admin/hosted", { with_session(ana.session) }).code == 403);
	reply = h.send("GET", "/api/admin/hosted", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["cap"].i() == 4 && json["live"].i() == 4);
	TEST_EXPECT(json["enabled"].b() && str(json["provider"]) == "local");
	TEST_EXPECT(body_text(reply).find("planted-admin-secret") == std::string::npos);
	bool seen = false;
	for (const auto &s : json["servers"]) {
		if (s["id"].i() != ana_first) continue;
		seen = true;
		TEST_EXPECT(s["owner"]["id"].i() == ana.id && str(s["owner"]["username"]) == "hosted_ana");
		TEST_EXPECT(s["service_account"].t() == crow::json::type::Null);
		TEST_EXPECT(str(s["state"]) == "requested" && !s["own_machine"].b());
	}
	TEST_EXPECT(seen);
	TEST_EXPECT(body_text(list(ana)).find("planted-admin-secret") == std::string::npos);
	TEST_EXPECT(body_text(one(ana, ana_first)).find("planted-admin-secret") == std::string::npos);

	// The owner's stop by id: stopping, again stopping, then (the row ended) 409
	// not_live; the list shows the live one first, then the ended one.
	reply = stop(ana, ana_first);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && str(json["server"]["state"]) == "stopping");
	reply = stop(ana, ana_first);
	TEST_EXPECT(reply.code == 202 && str(crow::json::load(body_text(reply))["server"]["state"]) == "stopping");
	TEST_EXPECT(hosted::transition(*h.pool.acquire(), ana_first, hosted::State::Stopping, hosted::State::Stopped,
			"ended by the test"));
	reply = stop(ana, ana_first);
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_live");
	json = crow::json::load(body_text(one(ana, ana_first)));
	TEST_EXPECT(str(json["server"]["state"]) == "stopped" && !json["server"]["live"].b());
	TEST_EXPECT(json["server"]["events"].size() == 3);
	TEST_EXPECT(json["server"]["ended_at"].t() == crow::json::type::String);
	json = crow::json::load(body_text(list(ana)));
	TEST_EXPECT(json["servers"].size() == 2 && json["servers"][0]["id"].i() == ana_second &&
			json["servers"][1]["id"].i() == ana_first && entitlement_is(json, 2, 1, 0, 0, true, "comp"));
	end_stopping_servers(h); // Ben's

	// The own-machine add-on: none without it (a comp grant buys no machine),
	// then a tier with one machine paid for, and the start on it.
	reply = start(fay, "{\"game_name\":\"Fay's own\",\"own_machine\":true}");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "no_own_machine");
	{
		auto db = h.pool.acquire();
		const int64_t period_end = billing::unix_now() + 30 * 24 * 60 * 60;
		billing::SubscriptionRow tier;
		tier.stripe_subscription_id = "sub_routes_fay_tier";
		tier.stripe_customer_id = "cus_routes_fay";
		tier.user_id = fay.id;
		tier.status = "active";
		tier.item = billing::ItemKind::Tier;
		tier.price_id = "price_routes_tier1";
		tier.tier = 1;
		tier.current_period_end = period_end;
		billing::upsert_subscription(*db, tier);
		billing::SubscriptionRow machine = tier;
		machine.stripe_subscription_id = "sub_routes_fay_machine";
		machine.item = billing::ItemKind::OwnMachine;
		machine.price_id = "price_routes_own";
		machine.tier = 0;
		billing::upsert_subscription(*db, machine);
	}
	TEST_EXPECT(entitlement_is(crow::json::load(body_text(list(fay))), 1, 0, 1, 0, true, "tier"));
	reply = start(fay, "{\"game_name\":\"Fay's own\",\"own_machine\":true}");
	json = crow::json::load(body_text(reply));
	if (reply.code != 202) std::fprintf(stderr, "  own-machine start: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 202 && json["server"]["own_machine"].b());
	const int64_t fay_server = json["server"]["id"].i();
	TEST_EXPECT(hosted::find_server(*h.pool.acquire(), fay_server)->own_machine);
	TEST_EXPECT(entitlement_is(crow::json::load(body_text(list(fay))), 1, 1, 1, 1, true, "tier"));

	// The list's ended servers: the newest 10, after the live ones.
	{
		auto db = h.pool.acquire();
		for (int i = 0; i < 12; ++i) {
			db->exec("INSERT INTO hosted_servers (owner_user_id, state, game_name, max_players, time_limit, "
					 "kill_limit, max_score, respawn, start_delay, missions, ended_at) "
					 "VALUES ($1, 'stopped', $2, 8, 30, 50, 5, 5, 0, 'a.bms', now());",
					{ opennova::db::BindValue(gus.id), opennova::db::BindValue("Gus ended " + std::to_string(i)) });
		}
		db->exec("INSERT INTO hosted_servers (owner_user_id, state, game_name, max_players, time_limit, "
				 "kill_limit, max_score, respawn, start_delay, missions) "
				 "VALUES ($1, 'running', 'Gus live', 8, 30, 50, 5, 5, 0, 'a.bms');",
				{ opennova::db::BindValue(gus.id) });
	}
	json = crow::json::load(body_text(list(gus)));
	TEST_EXPECT(json && json["servers"].size() == 11);
	if (json && json["servers"].size() == 11) {
		TEST_EXPECT(str(json["servers"][0]["settings"]["game_name"]) == "Gus live" && json["servers"][0]["live"].b());
		for (size_t i = 1; i < 11; ++i) {
			TEST_EXPECT(str(json["servers"][i]["settings"]["game_name"]) == "Gus ended " + std::to_string(12 - i));
			TEST_EXPECT(!json["servers"][i]["live"].b());
		}
	}

	// The admin's stops: one server (404 for none, 409 for an ended one), and the kill switch.
	auto admin_stop = [&h](const std::string &id, std::vector<std::string> headers) {
		return h.send("POST", "/api/admin/hosted/" + id + "/stop", headers);
	};
	TEST_EXPECT(admin_stop(std::to_string(cy_server), { with_session(cy.session), kCsrf }).code == 403);
	TEST_EXPECT(admin_stop("999999", { kBearer }).code == 404);
	reply = admin_stop(std::to_string(ana_first), { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_live");
	reply = admin_stop(std::to_string(cy_server), { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && str(json["server"]["state"]) == "stopping");
	TEST_EXPECT(str(json["server"]["owner"]["username"]) == "hosted_cy");
	TEST_EXPECT(h.send("POST", "/api/admin/hosted/stop-all", { with_session(ben.session), kCsrf }).code == 403);
	reply = h.send("POST", "/api/admin/hosted/stop-all", { kBearer });
	TEST_EXPECT(reply.code == 200 && crow::json::load(body_text(reply))["stopping"].i() == 3);
	reply = h.send("POST", "/api/admin/hosted/stop-all", { kBearer });
	TEST_EXPECT(reply.code == 200 && crow::json::load(body_text(reply))["stopping"].i() == 0);
	const auto second_row = hosted::find_server(*h.pool.acquire(), ana_second);
	TEST_EXPECT(second_row && second_row->state == hosted::State::Stopping);
	end_stopping_servers(h);
	{
		auto db = h.pool.acquire();
		db->exec("DELETE FROM billing_subscriptions WHERE stripe_subscription_id LIKE 'sub_routes_fay_%';");
	}

	// Hosting off (the Secure listener runs with ONNET_HOSTED_PROVIDER none): 503.
	std::string token;
	{
		auto conn = h.pool.acquire();
		const auto user = nws::get_user_by_id(*conn, dee.id);
		const auto minted = nws::create_web_session(*conn, dee.id, user->credentials_gen, "127.0.0.1", "harness");
		TEST_EXPECT(minted.has_value());
		token = *minted;
	}
	reply = h.send_to(h.secure_port, "POST", "/api/me/servers",
			{ kJson, kCsrf, with_cookie(nws::kWebSessionHostCookie, token) }, "{\"game_name\":\"x\"}");
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "hosting_disabled");
	reply = h.send_to(h.secure_port, "GET", "/api/me/servers", { with_cookie(nws::kWebSessionHostCookie, token) });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && !json["enabled"].b() && !json["full"].b() &&
			json["data_sets"].size() == 0);
	return 0;
}

// A hosted server's listing is its owner's (the one rule,
// hosted::owned_listing_account): the hosted server's service account lists
// it (a key /NWHost.dll recorded for that account, presented by a hand-driven
// host), and its owner's GET /api/me/hosts carries it with its hosted server,
// the owner's ServerCommand reaches it while another account's is a 404, and
// the admin view names the owner. An admin withdraws the owner's comp grant
// through the route and one supervisor pass (a fake machine driver) moves the server
// to stopping; a stopping server takes no command (409 server_stopping), and
// the owner's and the admin's stop on the listing are the hosted server's stop
// (no ServerStopHosting reaches the host, the supervisor's to send). The
// service account itself cannot use the site: its right password gets a wrong
// password's 401, the admin users routes do not know it, and registration
// refuses the srv- prefix. Last, a stale row (ended, its account still holding
// the listing) is no longer the owner's, and the admin's stop on its listing
// takes the plain push, which reaches the host.
int test_hosted_listing_owner(Harness &h) {
	namespace hosted = opennova::novaworld_server::hosted;
	using Notice = PushHost::Notice;
	const int64_t owner = make_account(h, "listing_owner", "pw-listing-owner");
	TEST_EXPECT(owner > 0 && make_account(h, "listing_other", "pw-listing-other") > 0);
	TEST_EXPECT(nws::set_comp_servers(*h.pool.acquire(), owner, 1).ok);
	const std::string owner_s = login_token(h, "10.52.0.1", "listing_owner", "pw-listing-owner");
	const std::string other_s = login_token(h, "10.52.0.2", "listing_other", "pw-listing-other");
	TEST_EXPECT(!owner_s.empty() && !other_s.empty());

	// The hosted server and its service account, as the supervisor makes them.
	hosted::HostedSettings settings;
	settings.game = hosted::kDataSetJointOperations;
	settings.game_name = "Hosted Owner Test";
	settings.missions = { "a.bms" };
	const auto request = hosted::request_server(*h.pool.acquire(), owner, settings, hosted::known_data_sets(), 10,
			opennova::novaworld_server::billing::unix_now());
	TEST_EXPECT(request.outcome == hosted::RequestOutcome::Created);
	const int64_t server_id = request.id;
	const std::string service_name = nws::service_username(server_id);
	const auto service = nws::upsert_service_account(*h.pool.acquire(), service_name, "pw-service-account");
	TEST_EXPECT(service.ok);
	hosted::set_service_account(*h.pool.acquire(), server_id, service.id);

	// Its lister hosts with a key minted for the service account.
	const std::string key = "PPPPOOOONNNNMMMMLLLLKKKKJJJJIIIIHHHHGGGGFFFFEEEE";
	TEST_EXPECT(opennova::hostdb::is_host_key(key) && record_key_for(h, key, service.id));
	PushHost host(h.nw_udp.bound_port(), 0x484f5354u);
	TEST_EXPECT(host.host("Hosted Listing Host"));
	host.present_key(key);
	std::optional<opennova::hostdb::HostRow> row;
	TEST_EXPECT(wait_until([&] {
		row = row_named(h, "Hosted Listing Host");
		return row && row->host_user_id == service.id;
	}));
	const uint32_t rid = row->rid;
	const std::string rid_text = std::to_string(rid);

	// The owner's list carries it, as its hosted server's; another account's does not.
	auto reply = h.send("GET", "/api/me/hosts", { with_session(owner_s) });
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["hosts"].size() == 1);
	TEST_EXPECT(json["hosts"][0]["rid"].i() == static_cast<int64_t>(rid));
	TEST_EXPECT(json["hosts"][0]["hosted_server_id"].i() == server_id);
	reply = h.send("GET", "/api/me/hosts", { with_session(other_s) });
	TEST_EXPECT(crow::json::load(body_text(reply))["hosts"].size() == 0);
	reply = h.send("GET", "/api/me/servers/" + std::to_string(server_id), { with_session(owner_s) });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(json && json["server"]["listing"]["rid"].i() == static_cast<int64_t>(rid));

	// The owner's ServerCommand reaches the host; another account's is a 404.
	auto command = [&h, &rid_text](const std::string &session, const std::string &body) {
		return h.send("POST", "/api/me/hosts/" + rid_text + "/command",
				{ kJson, kCsrf, with_session(session) }, body);
	};
	const std::string chat = "{\"verb\":\"TextChatServer\",\"args\":[\"hello hosted\"]}";
	reply = command(other_s, chat);
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "unknown_rid");
	TEST_EXPECT(host.wait_notice(Notice::Kind::Command, 300) == nullptr);
	reply = command(owner_s, chat);
	if (reply.code != 202) std::fprintf(stderr, "  owner command: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 202);
	const Notice *cmd = host.wait_notice(Notice::Kind::Command, 3000);
	TEST_EXPECT(cmd != nullptr && cmd->command.verb == opennova::ServerCommandVerb::TextChatServer);
	TEST_EXPECT(cmd->command.args == std::vector<std::string>{ "hello hosted" });

	// The admin view names the hosted server's owner, not its service account.
	reply = h.send("GET", "/api/admin/hosts", { kBearer });
	json = crow::json::load(body_text(reply));
	bool seen = false;
	for (const auto &e : json["hosts"]) {
		if (e["rid"].i() != static_cast<int64_t>(rid)) continue;
		seen = true;
		TEST_EXPECT(e["owner"]["id"].i() == owner && str(e["owner"]["username"]) == "listing_owner");
		TEST_EXPECT(e["hosted_server_id"].i() == server_id);
	}
	TEST_EXPECT(seen);

	// An admin withdraws the owner's comp grant (the route), and the supervisor's
	// next pass (here one hand-driven pass over a fake machine driver) stops the server.
	reply = h.send("PUT", "/api/admin/users/" + std::to_string(owner) + "/comp-servers", { kBearer, kJson },
			"{\"comp_servers\":0}");
	TEST_EXPECT(reply.code == 200 && crow::json::load(body_text(reply))["user"]["max_servers"].i() == 0);
	{
		hosted_test::FakeDriver fake;
		hosted::Supervisor supervisor(h.pool, fake, hosted::SupervisorConfig{});
		supervisor.tick(1000);
		TEST_EXPECT(fake.launches == 0);
	}
	TEST_EXPECT(hosted::find_server(*h.pool.acquire(), server_id)->state == hosted::State::Stopping);
	json = crow::json::load(body_text(h.send("GET", "/api/me/servers/" + std::to_string(server_id), { with_session(owner_s) })));
	TEST_EXPECT(json && str(json["server"]["state"]) == "stopping");
	const auto &events = json["server"]["events"];
	TEST_EXPECT(events.size() >= 2 &&
			str(events[events.size() - 1]["detail"]).find("hosting entitlement allows 0 hosted servers") != std::string::npos);

	// A stopping server takes no more commands; the stops on its listing are the
	// hosted server's: no ServerStopHosting goes out (the supervisor's to send).
	host.clear_notices();
	reply = command(owner_s, chat);
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "server_stopping");
	TEST_EXPECT(host.wait_notice(Notice::Kind::Command, 300) == nullptr);
	reply = h.send("POST", "/api/me/hosts/" + rid_text + "/stop", { kCsrf, with_session(other_s) });
	TEST_EXPECT(reply.code == 404);
	reply = h.send("POST", "/api/me/hosts/" + rid_text + "/stop", { kCsrf, with_session(owner_s) });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json && json["hosted_server_id"].i() == server_id);
	TEST_EXPECT(str(json["state"]) == "stopping" && str(json["status"]) == "hosted_stop");
	reply = h.send("POST", "/api/admin/hosts/" + rid_text + "/stop", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json && json["hosted_server_id"].i() == server_id);
	TEST_EXPECT(host.wait_notice(Notice::Kind::StopHosting, 500) == nullptr);
	TEST_EXPECT(row_named(h, "Hosted Listing Host").has_value());

	// The service account and the site.
	const auto service_login = login(h, "10.52.0.9", service_name, "pw-service-account");
	const auto wrong_login = login(h, "10.52.0.10", "listing_other", "not-the-password");
	TEST_EXPECT(service_login.code == 401 && wrong_login.code == 401);
	TEST_EXPECT(body_text(service_login) == body_text(wrong_login));
	TEST_EXPECT(set_cookie_value(service_login, nws::kWebSessionCookie).empty());
	reply = h.send("GET", "/api/admin/users", { kBearer });
	TEST_EXPECT(reply.code == 200 && body_text(reply).find(service_name) == std::string::npos);
	const std::string service_path = "/api/admin/users/" + std::to_string(service.id);
	TEST_EXPECT(h.send("PUT", service_path + "/comp-servers", { kBearer, kJson }, "{\"comp_servers\":1}").code == 404);
	TEST_EXPECT(h.send("PUT", service_path, { kBearer, kJson }, "{\"nwhandle\":\"Taken\"}").code == 404);
	TEST_EXPECT(h.send("DELETE", service_path, { kBearer }).code == 404);
	reply = h.send("POST", "/api/register", { kJson, kCsrf, from_ip("10.52.0.11") },
			"{\"username\":\"srv-777\",\"password\":\"pw-x\",\"email\":\"srv777@example.com\"}");
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_field");

	// A stale row: the hosted server ended while its service account still holds
	// the listing (the supervisor ends both in one transaction, so only a
	// database written by hand gets here). It is no longer the owner's, and the
	// admin's stop takes the plain push, which reaches the host.
	TEST_EXPECT(hosted::transition(*h.pool.acquire(), server_id, hosted::State::Stopping,
			hosted::State::Stopped, "ended by the test"));
	reply = h.send("GET", "/api/me/hosts", { with_session(owner_s) });
	TEST_EXPECT(crow::json::load(body_text(reply))["hosts"].size() == 0);
	reply = command(owner_s, chat);
	TEST_EXPECT(reply.code == 404);
	reply = h.send("POST", "/api/admin/hosts/" + rid_text + "/stop", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json && str(json["statement"]) == "ServerStopHosting");
	TEST_EXPECT(host.wait_notice(Notice::Kind::StopHosting, 3000) != nullptr);
	TEST_EXPECT(wait_until([&] { return !row_named(h, "Hosted Listing Host").has_value(); }));

	host.goodbye();
	TEST_EXPECT(nws::delete_service_account(*h.pool.acquire(), service.id).ok);
	return 0;
}

// A remote-admin server on loopback for the console routes: the engine's
// connection machine (net/admin/admin_server.h, its challenge and login) behind
// the apps' pump (admin_tcp_server.h) on a thread of its own, its verbs
// answered by a scripted console in retail's reply strings
// (docs/net/novaworld-net-re.md §6.9: two players, a two-map catalog, a
// rotation, a chat window), every line it is sent kept. The test turns its
// knobs: a delay before one verb's reply (a request in flight), the scene (a
// server between matches answers as retail's does there), hostile replies, and
// the rotation.
class FakeAdminConsole final : public opennova::AdminCommandHandler {
public:
	explicit FakeAdminConsole(const std::string &password) :
			server_(config(password), *this, rand_), tcp_(server_) {}
	~FakeAdminConsole() override { stop(); }

	bool start() {
		if (!tcp_.listen(0, /*loopback_only=*/true)) return false;
		thread_ = std::thread([this] {
			while (!stop_.load()) {
				tcp_.pump();
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		});
		return true;
	}
	void stop() {
		stop_.store(true);
		if (thread_.joinable()) thread_.join();
		tcp_.close();
	}
	uint16_t port() const { return tcp_.port(); }
	std::vector<std::string> lines() {
		std::lock_guard<std::mutex> lock(mu_);
		return lines_;
	}
	// The next replies to a line starting with `prefix` wait `ms` first (0: none).
	void delay(const std::string &prefix, int ms) {
		std::lock_guard<std::mutex> lock(mu_);
		delay_prefix_ = prefix;
		delay_ms_ = ms;
	}
	void set_scene(const std::string &scene) {
		std::lock_guard<std::mutex> lock(mu_);
		scene_ = scene;
	}
	void set_hostile(bool hostile) {
		std::lock_guard<std::mutex> lock(mu_);
		hostile_ = hostile;
	}
	void set_rotation(std::vector<std::string> rotation) {
		std::lock_guard<std::mutex> lock(mu_);
		rotation_ = std::move(rotation);
		next_ = -1;
	}
	std::vector<std::string> rotation() {
		std::lock_guard<std::mutex> lock(mu_);
		return rotation_;
	}

	bool dispatch(const opennova::AdminSession &, std::string_view line, std::vector<std::string> &replies) override {
		int delay_ms = 0;
		{
			std::lock_guard<std::mutex> lock(mu_);
			lines_.emplace_back(line);
			if (!delay_prefix_.empty() && line.substr(0, delay_prefix_.size()) == delay_prefix_) delay_ms = delay_ms_;
		}
		if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
		std::lock_guard<std::mutex> lock(mu_);
		std::vector<std::string> t;
		for (size_t i = 0; i < line.size();) {
			const size_t end = std::min(line.find(' ', i), line.size());
			if (end > i) t.emplace_back(line.substr(i, end - i));
			i = end + 1;
		}
		const auto is = [&t](size_t i, const char *word) { return t.size() > i && t[i] == word; };
		const auto number = [&t](size_t i) { return t.size() > i ? std::atoi(t[i].c_str()) : -1; };
		const bool in_game = scene_ == "Game";
		if (is(0, "QUIT")) return false;
		if (is(0, "GET") && is(1, "GAMESTATE")) {
			replies.push_back("OK - Current State = " + scene_);
		} else if (is(0, "GET") && is(1, "GAMESETTINGS")) {
			replies.emplace_back("AutoBalanceOnRecycle = 1\nServerName           = Console Test\n"
								 "ServerPassword       = s3cret\n");
		} else if (is(0, "PLAYER") && is(1, "LIST")) {
			if (!in_game) {
				replies.emplace_back("ERROR - Not in Game State.");
			} else if (hostile_) {
				replies.emplace_back("NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n"
									 "abc\n"
									 "garbage\tline\n"
									 "x\ty\tz\tw\n"
									 "Ev\x01l           \t 1\t 1\t0\t0\t0\t0\n"
									 "Caf\xE9            \t 2\t 2\t0\t0\t0\t0\n");
			} else {
				replies.emplace_back("NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n"
									 "Host            \t 0\t 0\t0\t0\t0\t0\n"
									 "Ann             \t 1\t 1\t2\t5\t1\t40\n"
									 "Bob             \t 2\t 2\t1\t0\t3\t55\n");
			}
		} else if (is(0, "PLAYER") && t.size() == 3) {
			const int slot = number(2);
			if (slot != 1 && slot != 2) {
				replies.emplace_back("ERROR - Player Not Found.");
			} else if (is(1, "PUNT")) {
				replies.emplace_back("OK - Player punted.");
			} else if (is(1, "SWAPTEAM")) {
				replies.emplace_back("OK - Player Swapped.");
			} else if (is(1, "KILL")) {
				replies.emplace_back("OK - Player Killed.");
			} else if (is(1, "BAN") && slot == 1) {
				replies.emplace_back("OK - Banned Player #1 \"Ann\".");
				replies.emplace_back("OK - Player Banned.");
			} else if (is(1, "BAN")) {
				replies.emplace_back("ERROR - Player #2 \"Bob\" doesn't have a PCID.");
			}
		} else if (is(0, "MISSION") && is(1, "AVAILABLE")) {
			replies.emplace_back(hostile_ ? "abc\n. (\n7. ODD.BMS (Odd (title))\n"
										  : "0. ALPHA.BMS (Alpha Base)\n1. BRAVO.BMS (Bravo Ridge)\n");
		} else if (is(0, "MISSION") && is(1, "LIST")) {
			// Outside a match retail lists nothing. [orig: @0x40634F]
			std::string out = hostile_ ? "abc\n: - (\n-1: x - ()\n" : "";
			for (size_t i = 0; in_game && i < rotation_.size(); ++i) {
				out += std::to_string(i) + ": " + rotation_[i] + " - () () () " + (i == 0 ? "<CURRENT MISSION>" : "<>") +
						" " + (static_cast<int>(i) == next_ ? "<NEXT MISSION>" : "<>") + "\n";
			}
			replies.push_back(!in_game || rotation_.empty() ? std::string("No missions in queue.") : out);
		} else if (is(0, "MISSION") && is(1, "ADD") && t.size() >= 3) {
			std::string file = t[2];
			for (char &c : file) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			if (file == "ALPHA.BMS" || file == "BRAVO.BMS") {
				// The reply in any state, the append in a match alone. [orig: @0x403E9C..0x403ECF]
				if (in_game) rotation_.push_back(file);
				replies.emplace_back("OK - Entry Added\n");
			} else {
				replies.emplace_back("ERROR - File Not Found.");
			}
		} else if (is(0, "MISSION") && is(1, "REMOVE")) {
			const int index = number(2);
			if (in_game && index >= 0 && index < static_cast<int>(rotation_.size()))
				rotation_.erase(rotation_.begin() + index);
			replies.emplace_back("OK - Mission Removed.");
		} else if (is(0, "MISSION") && is(1, "SETNEXT")) {
			const int index = number(2);
			if (index >= 0 && index < static_cast<int>(rotation_.size())) next_ = index;
			replies.emplace_back("OK - Next Mission Set.");
		} else if (is(0, "MISSION") && is(1, "CYCLE")) {
			replies.emplace_back(in_game ? "OK - Server is cycling..." : "ERROR - Must be in Game State to cycle server.");
		} else if (is(0, "CHAT") && is(1, "GET")) {
			std::string out;
			for (const std::string &l : chat_) out += l + "\r\n";
			replies.push_back(hostile_ ? std::string("\x01" "ctl\r\n\xE9t\xE9\r\n") : out);
		} else if (is(0, "CHAT") && is(1, "SEND")) {
			std::string text;
			for (size_t i = 2; i < t.size(); ++i) text += t[i] + " ";
			chat_.push_back("Server: " + text);
			replies.emplace_back("OK - Chat sent.");
		} else {
			replies.emplace_back("USAGE - [QUIT | GET | SET | MISSION | PLAYER | WEAPON | CMD | GOTO]");
		}
		return true;
	}
	std::string status_report() override { return {}; }

private:
	static opennova::admincfg::AdminConfig config(const std::string &password) {
		const std::string text = "svc " + password + " 7F\r\nip_restrict = 127.0.0.1\r\n";
		return opennova::admincfg::parse(text.data(), text.size());
	}

	opennova::io::CrtRand rand_;
	opennova::AdminServer server_;
	opennova::net::AdminTcpServer tcp_;
	std::atomic<bool> stop_{ false };
	std::thread thread_;
	std::mutex mu_;
	std::vector<std::string> lines_;
	std::string delay_prefix_;
	int delay_ms_ = 0;
	std::string scene_ = "Game";
	bool hostile_ = false;
	std::vector<std::string> rotation_{ "ALPHA.BMS" };
	int next_ = -1;
	std::vector<std::string> chat_;
};

// A line the server was sent, checked as the dispatcher splits it (' ' and
// '\t'..'\r'): the curated commands' exact shapes (console_commands.h), every
// argument inside its rule.
bool curated_line(const std::string &line) {
	std::vector<std::string> t;
	std::string token;
	for (const char c : line) {
		if (c == ' ' || (c >= '\t' && c <= '\r')) {
			if (!token.empty()) t.push_back(token);
			token.clear();
		} else {
			token.push_back(c);
		}
	}
	if (!token.empty()) t.push_back(token);
	const auto digits = [](const std::string &s, size_t max_len) {
		return !s.empty() && s.size() <= max_len && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
	};
	if (t.size() >= 2 && t[0] == "PLAYER" && (t[1] == "PUNT" || t[1] == "BAN" || t[1] == "SWAPTEAM" || t[1] == "KILL"))
		return t.size() == 3 && digits(t[2], 3) && std::atoi(t[2].c_str()) >= 1 && std::atoi(t[2].c_str()) <= 255;
	if (t.size() >= 2 && t[0] == "MISSION" && t[1] == "ADD") {
		if (t.size() != 4 || (t[3] != "0" && t[3] != "1")) return false;
		const std::string &f = t[2];
		const bool bms = f.size() > 4 && (f.compare(f.size() - 4, 4, ".bms") == 0 || f.compare(f.size() - 4, 4, ".BMS") == 0);
		return bms && std::all_of(f.begin(), f.end() - 4, [](char c) {
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		});
	}
	// An index below the maps a rotation holds: the composition's cap.
	if (t.size() >= 2 && t[0] == "MISSION" && (t[1] == "REMOVE" || t[1] == "SETNEXT"))
		return t.size() == 3 && digits(t[2], 2) &&
				static_cast<uint64_t>(std::atoi(t[2].c_str())) <=
				opennova::novaworld_server::hosted::kConsoleIndexMax;
	if (line.compare(0, 10, "CHAT SEND ") == 0) {
		return line.size() <= 69 && std::none_of(line.begin(), line.end(), [](char c) {
			const auto byte = static_cast<unsigned char>(c);
			return byte < 0x20 || byte == 0x7F;
		});
	}
	static const std::set<std::string> exact = { "PLAYER LIST", "MISSION AVAILABLE", "MISSION LIST", "MISSION CYCLE",
		"CHAT GET", "GET GAMESETTINGS", "GET GAMESTATE" };
	return exact.count(line) != 0;
}

// Whether `want` appears in `lines` in order (other lines between).
bool in_order(const std::vector<std::string> &lines, const std::vector<std::string> &want) {
	size_t next = 0;
	for (const std::string &line : lines) {
		if (next < want.size() && line == want[next]) ++next;
	}
	return next == want.size();
}

// The machine agents' routes (hosted/agent_route.h) are the internal listener's
// alone: the public listener answers neither the bootstrap nor the sync (nor the
// old per-server bootstrap path), under /api/ or not, with anything a machine
// could read as an answer.
int test_no_agent_routes_on_public_listener(Harness &h) {
	const std::string auth = "Authorization: Bearer " + std::string(48, 'A');
	const std::string hello = "{\"protocol\":1,\"agent_version\":\"test\"}";
	for (const char *path : { "/hosted/machines/1/bootstrap", "/hosted/machines/1/sync", "/hosted/bootstrap/1",
				 "/api/hosted/machines/1/sync" }) {
		const auto reply = h.send("POST", path, { auth, kJson }, hello);
		if (reply.code == 200) std::fprintf(stderr, "  %s: %s\n", path, describe(reply).c_str());
		TEST_EXPECT(reply.transport_ok && reply.code != 200);
		const std::string body = body_text(reply);
		TEST_EXPECT(body.find("agent_secret") == std::string::npos && body.find("desired") == std::string::npos);
	}
	return 0;
}

// The admin's fleet routes (hosted/fleet_routes.h) over fleet rows made by hand
// (no supervisor): the fleet view's machines (kind, state, type, slots and the
// slots used, the public address, the month's egress; no credential of the
// agent's) and servers (the owner with its support tier, the machine and slot,
// the game port, the listed players, null where there are none), its `owner`
// and `state` filters, one server with its events and its machine, and the
// machines' moves: a drain and an end (each again answered the same, an end
// past a drain, a drain past an end refused not_live), a fleet roll draining
// every machine that takes servers; admin only.
int test_admin_fleet_routes(Harness &h) {
	namespace hosted = opennova::novaworld_server::hosted;
	namespace billing = opennova::novaworld_server::billing;
	const int64_t ana = make_account(h, "fleet_ana", "pw-fleet-ana");
	const int64_t ben = make_account(h, "fleet_ben", "pw-fleet-ben");
	TEST_EXPECT(ana > 0 && ben > 0 && nws::set_comp_servers(*h.pool.acquire(), ana, 2).ok);
	{
		billing::SubscriptionRow tier;
		tier.stripe_subscription_id = "sub_fleet_ben";
		tier.stripe_customer_id = "cus_fleet_ben";
		tier.user_id = ben;
		tier.status = "active";
		tier.item = billing::ItemKind::Tier;
		tier.price_id = "price_htier2";
		tier.tier = 2;
		tier.current_period_end = billing::unix_now() + 30 * 24 * 60 * 60;
		billing::upsert_subscription(*h.pool.acquire(), tier);
	}
	const std::string ana_s = login_token(h, "10.53.0.1", "fleet_ana", "pw-fleet-ana");
	TEST_EXPECT(!ana_s.empty());
	auto db = h.pool.acquire();

	// A ready shared machine (its agent reported once) and the servers: Ana's on
	// its slot 0, running and listed with 5 players; Ana's second, waiting; Ben's
	// ended, where it last ran on slot 1.
	const auto machine = hosted::create_machine(*db, { "local", hosted::MachineKind::Shared, 4, "opennova-serve:local", 0 });
	hosted::LaunchedMachine launched;
	launched.cloud_id = "local-fleet-1";
	launched.private_address = "10.0.1.7";
	launched.public_address = "198.51.100.7";
	launched.instance_type = "t3a.large";
	launched.template_version = "lt-0fleet:3";
	TEST_EXPECT(hosted::mark_launched(*db, machine.id, launched));
	TEST_EXPECT(hosted::record_sync(*db, machine.id, "test-agent", {}).has_value());
	hosted::HostedSettings settings;
	settings.game = hosted::kDataSetJointOperations;
	settings.game_name = "Fleet Routes";
	settings.missions = { "a.bms" };
	const auto start = [&](int64_t owner) {
		const auto r = hosted::request_server(*db, owner, settings, hosted::known_data_sets(), 50, billing::unix_now());
		return r.outcome == hosted::RequestOutcome::Created ? r.id : 0;
	};
	const int64_t running = start(ana);
	const int64_t waiting = start(ana);
	const int64_t ended = start(ben);
	TEST_EXPECT(running > 0 && waiting > 0 && ended > 0);
	TEST_EXPECT(hosted::assign_slot(*db, running, machine.id, 0, 32768) == 1);
	TEST_EXPECT(hosted::assign_slot(*db, ended, machine.id, 1, 32769) == 1);
	for (const auto &[from, to] : { std::pair<hosted::State, hosted::State>{ hosted::State::Requested, hosted::State::Provisioning },
				 { hosted::State::Provisioning, hosted::State::Booting }, { hosted::State::Booting, hosted::State::Listing },
				 { hosted::State::Listing, hosted::State::Running } })
		TEST_EXPECT(hosted::transition(*db, running, from, to, "by the test"));
	TEST_EXPECT(hosted::transition(*db, ended, hosted::State::Requested, hosted::State::Stopped, "by the test"));
	const auto service = nws::upsert_service_account(*db, nws::service_username(running), "pw-fleet-service");
	TEST_EXPECT(service.ok);
	hosted::set_service_account(*db, running, service.id);
	const std::string key = "AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIIIJJJJKKKKLLLL";
	TEST_EXPECT(record_key_for(*db, key, service.id));
	const uint32_t rid = 0x0F1EE7u;
	{
		opennova::hostdb::HostRow row;
		row.rid = rid;
		row.gsid = "GSID-fleet";
		row.game = "jop_2_consumer";
		row.server_name = "Fleet Routes";
		row.host_ip = row.peer_ip = "198.51.100.7";
		row.host_port = row.peer_port = 32768;
		row.player_count = 5;
		row.max_players = 32;
		row.host_user_id = opennova::hostdb::claim_host_key(*db, key, rid);
		opennova::hostdb::upsert_host(*db, row);
	}

	// Admin only.
	TEST_EXPECT(h.send("GET", "/api/admin/hosted").code == 401);
	TEST_EXPECT(h.send("GET", "/api/admin/hosted", { with_session(ana_s) }).code == 403);
	TEST_EXPECT(h.send("GET", "/api/admin/hosted/" + std::to_string(running), { with_session(ana_s) }).code == 403);
	TEST_EXPECT(h.send("POST", "/api/admin/fleet/roll", { with_session(ana_s), kCsrf }).code == 403);
	TEST_EXPECT(h.send("POST", "/api/admin/fleet/machines/" + std::to_string(machine.id) + "/drain",
						{ with_session(ana_s), kCsrf })
						.code == 403);

	auto reply = h.send("GET", "/api/admin/hosted", { kBearer });
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["enabled"].b() && json["cap"].i() == 4);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	TEST_EXPECT(body_text(reply).find(machine.bootstrap_token) == std::string::npos);
	TEST_EXPECT(body_text(reply).find("agent_secret") == std::string::npos && body_text(reply).find("bootstrap_token") == std::string::npos);
	bool machine_seen = false;
	for (const auto &m : json["machines"]) {
		if (m["id"].i() != machine.id) continue;
		machine_seen = true;
		TEST_EXPECT(str(m["kind"]) == "shared" && str(m["state"]) == "ready" && str(m["instance_type"]) == "t3a.large");
		TEST_EXPECT(m["slots"].i() == 4 && m["slots_used"].i() == 1 && str(m["public_address"]) == "198.51.100.7");
		TEST_EXPECT(m["egress_bytes_month"].t() == crow::json::type::Number && m["egress_bytes_month"].i() == 0);
		TEST_EXPECT(str(m["template_version"]) == "lt-0fleet:3" && m["reserved_for"].t() == crow::json::type::Null);
		TEST_EXPECT(m["bootstrapped"].t() == crow::json::type::False);
	}
	TEST_EXPECT(machine_seen);
	const auto server_in = [](const crow::json::rvalue &list, int64_t id) -> const crow::json::rvalue * {
		for (const auto &s : list) {
			if (s["id"].i() == id) return &s;
		}
		return nullptr;
	};
	const crow::json::rvalue *r = server_in(json["servers"], running);
	TEST_EXPECT(r != nullptr);
	if (r) {
		TEST_EXPECT((*r)["owner"]["id"].i() == ana && str((*r)["owner"]["username"]) == "fleet_ana");
		TEST_EXPECT((*r)["owner"]["tier"].t() == crow::json::type::Null); // a comp grant, no tier
		TEST_EXPECT((*r)["machine_ref"].i() == machine.id && (*r)["slot"].i() == 0 && (*r)["game_port"].i() == 32768);
		TEST_EXPECT((*r)["players"].i() == 5 && (*r)["listing"]["rid"].i() == static_cast<int64_t>(rid));
		TEST_EXPECT(str((*r)["state"]) == "running" && (*r)["generation"].i() == 1);
	}
	const crow::json::rvalue *w = server_in(json["servers"], waiting);
	TEST_EXPECT(w != nullptr);
	if (w) {
		TEST_EXPECT((*w)["machine_ref"].t() == crow::json::type::Null && (*w)["slot"].t() == crow::json::type::Null);
		TEST_EXPECT((*w)["game_port"].t() == crow::json::type::Null && (*w)["players"].t() == crow::json::type::Null);
	}
	const crow::json::rvalue *e = server_in(json["servers"], ended);
	TEST_EXPECT(e != nullptr);
	if (e) {
		TEST_EXPECT((*e)["owner"]["tier"].i() == 2 && (*e)["machine_ref"].i() == machine.id && (*e)["slot"].i() == 1);
		TEST_EXPECT(!(*e)["live"].b());
	}

	// The filters: an owner by username, the live or the ended ones.
	const auto ids = [&h](const std::string &query) {
		std::vector<int64_t> out;
		const auto got = h.send("GET", "/api/admin/hosted" + query, { kBearer });
		const auto list = crow::json::load(body_text(got));
		if (got.code != 200 || !list) return std::vector<int64_t>{ -1 };
		for (const auto &s : list["servers"]) out.push_back(s["id"].i());
		return out;
	};
	auto has = [](const std::vector<int64_t> &list, int64_t id) {
		return std::find(list.begin(), list.end(), id) != list.end();
	};
	auto of_ana = ids("?owner=fleet_ana");
	TEST_EXPECT(of_ana.size() == 2 && has(of_ana, running) && has(of_ana, waiting));
	TEST_EXPECT(ids("?owner=fleet_nobody").empty());
	auto ana_live = ids("?owner=fleet_ana&state=live");
	TEST_EXPECT(ana_live.size() == 2);
	auto ben_ended = ids("?owner=fleet_ben&state=ended");
	TEST_EXPECT(ben_ended == std::vector<int64_t>{ ended });
	TEST_EXPECT(ids("?owner=fleet_ben&state=live").empty());
	TEST_EXPECT(!has(ids("?state=live"), ended) && !has(ids("?state=ended"), running));
	reply = h.send("GET", "/api/admin/hosted?state=sleeping", { kBearer });
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_field");

	// One server, with its events and its machine.
	reply = h.send("GET", "/api/admin/hosted/" + std::to_string(running), { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["server"]["id"].i() == running);
	TEST_EXPECT(json["server"]["events"].size() == 5 && json["machine"]["id"].i() == machine.id);
	TEST_EXPECT(str(json["server"]["owner"]["username"]) == "fleet_ana" && json["machine"]["slots_used"].i() == 1);
	reply = h.send("GET", "/api/admin/hosted/" + std::to_string(waiting), { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json["machine"].t() == crow::json::type::Null);
	reply = h.send("GET", "/api/admin/hosted/999999", { kBearer });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");

	// The machines' moves.
	const std::string machine_path = "/api/admin/fleet/machines/" + std::to_string(machine.id);
	reply = h.send("POST", machine_path + "/drain", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && str(json["machine"]["state"]) == "draining" &&
			str(json["machine"]["end_reason"]) == "drained by an admin");
	reply = h.send("POST", machine_path + "/drain", { kBearer });
	TEST_EXPECT(reply.code == 202 && str(crow::json::load(body_text(reply))["machine"]["state"]) == "draining");
	reply = h.send("POST", machine_path + "/end", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && str(json["machine"]["state"]) == "ending");
	TEST_EXPECT(h.send("POST", machine_path + "/end", { kBearer }).code == 202);
	reply = h.send("POST", machine_path + "/drain", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_live");
	TEST_EXPECT(h.send("POST", "/api/admin/fleet/machines/999999/end", { kBearer }).code == 404);
	TEST_EXPECT(h.send("POST", "/api/admin/fleet/machines/999999/drain", { kBearer }).code == 404);
	TEST_EXPECT(hosted::transition_machine(*db, machine.id, hosted::FleetMachineState::Ending,
			hosted::FleetMachineState::Ended, "by the test"));
	reply = h.send("POST", machine_path + "/end", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_live");

	// A roll drains every machine that takes servers, none other.
	const auto booting = hosted::create_machine(*db, { "local", hosted::MachineKind::Shared, 4, "opennova-serve:local", 0 });
	launched.cloud_id = "local-fleet-2";
	TEST_EXPECT(hosted::mark_launched(*db, booting.id, launched));
	const auto failed = hosted::create_machine(*db, { "local", hosted::MachineKind::Own, 1, "opennova-serve:local", 0 });
	TEST_EXPECT(hosted::transition_machine(*db, failed.id, hosted::FleetMachineState::Launching,
			hosted::FleetMachineState::Failed, "by the test"));
	reply = h.send("POST", "/api/admin/fleet/roll", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json && json["draining"].i() == 1 && json["machines"].size() == 1 &&
			json["machines"][0].i() == booting.id);
	TEST_EXPECT(hosted::find_machine(*db, booting.id)->state == hosted::FleetMachineState::Draining);
	TEST_EXPECT(hosted::find_machine(*db, booting.id)->end_reason == "rolled by an admin");
	TEST_EXPECT(hosted::find_machine(*db, failed.id)->state == hosted::FleetMachineState::Failed);
	reply = h.send("POST", "/api/admin/fleet/roll", { kBearer });
	TEST_EXPECT(reply.code == 202 && crow::json::load(body_text(reply))["draining"].i() == 0);

	// Leave no live server or machine for the cases after.
	TEST_EXPECT(hosted::transition_machine(*db, booting.id, hosted::FleetMachineState::Draining,
			hosted::FleetMachineState::Ended, "by the test"));
	opennova::hostdb::remove_host_by_rid(*db, rid);
	for (const int64_t id : { running, waiting }) hosted::request_stop(*db, id, "by the test");
	end_stopping_servers(*db);
	TEST_EXPECT(nws::delete_service_account(*db, service.id).ok);
	db->exec("DELETE FROM billing_subscriptions WHERE stripe_subscription_id = 'sub_fleet_ben';");
	return 0;
}

// The stats routes (hosted/stats_routes.h) over samples written by hand at this
// minute (no sampler runs here): the owner's one server (every key of the
// reply, the live view with its listing's players and its newest sample's CPU,
// the summary, a point a minute for 1h with ISO timestamps), another account's
// server a 404, the account's servers together and the fleet's (no live view,
// no server_id), the admin's routes (a player's session refused), the ranges
// (24h by default, 400 for another), no-store; and /api/me/servers/stats and
// /api/admin/hosted/stats never read as a server id.
int test_hosted_stats_routes(Harness &h) {
	namespace hosted = opennova::novaworld_server::hosted;
	namespace billing = opennova::novaworld_server::billing;
	const int64_t owner = make_account(h, "stats_owner", "pw-stats-owner");
	const int64_t other = make_account(h, "stats_other", "pw-stats-other");
	TEST_EXPECT(owner > 0 && other > 0);
	TEST_EXPECT(nws::set_comp_servers(*h.pool.acquire(), owner, 2).ok);
	TEST_EXPECT(nws::set_comp_servers(*h.pool.acquire(), other, 1).ok);
	const std::string owner_s = login_token(h, "10.54.0.1", "stats_owner", "pw-stats-owner");
	const std::string other_s = login_token(h, "10.54.0.2", "stats_other", "pw-stats-other");
	TEST_EXPECT(!owner_s.empty() && !other_s.empty());
	auto db = h.pool.acquire();
	hosted::HostedSettings settings;
	settings.game = hosted::kDataSetJointOperations;
	settings.game_name = "Stats Routes";
	settings.missions = { "a.bms" };
	const auto start = [&](int64_t who) {
		const auto r = hosted::request_server(*db, who, settings, hosted::known_data_sets(), 50, billing::unix_now());
		return r.outcome == hosted::RequestOutcome::Created ? r.id : 0;
	};
	const int64_t running = start(owner);
	const int64_t ended = start(owner);
	const int64_t others = start(other);
	TEST_EXPECT(running > 0 && ended > 0 && others > 0);
	for (const auto &[from, to] : { std::pair<hosted::State, hosted::State>{ hosted::State::Requested, hosted::State::Provisioning },
				 { hosted::State::Provisioning, hosted::State::Booting }, { hosted::State::Booting, hosted::State::Listing },
				 { hosted::State::Listing, hosted::State::Running } })
		TEST_EXPECT(hosted::transition(*db, running, from, to, "by the test"));
	TEST_EXPECT(hosted::transition(*db, ended, hosted::State::Requested, hosted::State::Stopped, "by the test"));
	// Its listing: 4 players, a mission, the round's clock (TimeLeft, whole minutes).
	const auto service = nws::upsert_service_account(*db, nws::service_username(running), "pw-stats-service");
	TEST_EXPECT(service.ok);
	hosted::set_service_account(*db, running, service.id);
	const std::string key = "LLLLKKKKJJJJIIIIHHHHGGGGFFFFEEEEDDDDCCCCBBBBAAAA";
	TEST_EXPECT(record_key_for(*db, key, service.id));
	const uint32_t rid = 0x05A75u;
	{
		opennova::hostdb::HostRow row;
		row.rid = rid;
		row.gsid = "GSID-stats";
		row.game = "jop_2_consumer";
		row.server_name = "Stats Routes";
		row.mission_name = "Cliff Hanger";
		row.game_type = "DM";
		row.time_left = "12";
		row.host_ip = row.peer_ip = "198.51.100.9";
		row.host_port = row.peer_port = 32768;
		row.player_count = 4;
		row.max_players = 32;
		row.host_user_id = opennova::hostdb::claim_host_key(*db, key, rid);
		opennova::hostdb::upsert_host(*db, row);
	}
	// The minutes: the running server's last two, the ended one's earlier, the
	// other account's, on an empty slate (an earlier case's hand-driven
	// supervisor pass sampled the servers live then).
	db->exec("DELETE FROM hosted_server_samples;");
	db->exec("DELETE FROM hosted_server_hourly;");
	const int64_t now = billing::unix_now();
	const int64_t minute = now - now % 60;
	// (The unique players written once per server, so an hour's edge between the
	// two minutes counts them once either way.)
	const auto put = [&](int64_t id, int64_t at, int64_t players, int64_t egress, int64_t unique) {
		hosted::ServerSample s;
		s.server_id = id;
		s.at = at;
		s.state = "running";
		s.players = players;
		s.max_players = 32;
		s.cpu_percent = 30.0;
		s.memory_bytes = 64 << 20;
		s.egress_bytes = egress;
		hosted::insert_sample(*db, s);
		hosted::rollup_hour(*db, id, at, unique);
	};
	put(running, minute, 4, 1000, 2);
	put(running, minute - 60, 4, 1000, 0);
	put(ended, minute - 120, 1, 10, 1);
	put(others, minute, 9, 5, 1);
	const auto stats = [&h](const std::string &path, const std::vector<std::string> &headers) {
		const auto reply = h.send("GET", path, headers);
		return std::make_pair(reply, crow::json::load(body_text(reply)));
	};
	const std::string one = "/api/me/servers/" + std::to_string(running) + "/stats";

	TEST_EXPECT(h.send("GET", one + "?range=1h").code == 401);
	auto [reply, json] = stats(one + "?range=1h", { with_session(owner_s) });
	if (reply.code != 200) std::fprintf(stderr, "  %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 200 && json && header_value(reply, "Cache-Control") == "no-store");
	TEST_EXPECT(json["server_id"].i() == running && str(json["range"]) == "1h");
	const auto &live = json["live"];
	TEST_EXPECT(str(live["state"]) == "running" && live["uptime_seconds"].t() == crow::json::type::Number);
	TEST_EXPECT(live["players"].i() == 4 && live["max_players"].i() == 32 && str(live["mission"]) == "Cliff Hanger");
	TEST_EXPECT(str(live["game_type"]) == "DM" && live["time_left"].i() == 12 * 60);
	TEST_EXPECT(live["scene"].t() == crow::json::type::Null && live["link_state"].t() == crow::json::type::Null);
	TEST_EXPECT(std::fabs(live["cpu_percent"].d() - 30.0) < 1e-9 && live["memory_bytes"].i() == (64 << 20));
	const auto &summary = json["summary"];
	TEST_EXPECT(summary["live_seconds"].i() == 120 && summary["running_seconds"].i() == 120);
	TEST_EXPECT(std::fabs(summary["availability"].d() - 1.0) < 1e-9 && summary["restarts"].i() == 0);
	TEST_EXPECT(summary["peak_players"].i() == 4 && std::fabs(summary["avg_players"].d() - 4.0) < 1e-9);
	TEST_EXPECT(std::fabs(summary["player_hours"].d() - 8.0 / 60) < 1e-9 && summary["unique_players"].i() == 2);
	// The month's egress: both minutes', unless a month began between them.
	TEST_EXPECT(summary["missions"].i() == 0 && summary["egress_bytes"].i() == 2000 &&
			summary["egress_bytes_month"].i() >= 1000 && summary["egress_bytes_month"].i() <= 2000);
	TEST_EXPECT(summary["approximate"].size() == 2 && str(summary["approximate"][0]) == "unique_players" &&
			str(summary["approximate"][1]) == "missions");
	const auto &series = json["series"];
	TEST_EXPECT(series["step_seconds"].i() == 60 && series["points"].size() == 60);
	if (series["points"].size() == 60) {
		const auto &last = series["points"][59];
		TEST_EXPECT(str(last["t"]) == hosted::iso_utc(minute) && str(last["t"]).size() == 20 && str(last["t"]).back() == 'Z');
		TEST_EXPECT(std::fabs(last["players"].d() - 4.0) < 1e-9 && last["running"].b() && last["egress_bytes"].i() == 1000);
		TEST_EXPECT(last["max_players"].i() == 32 && std::fabs(last["cpu_percent"].d() - 30.0) < 1e-9);
		const auto &empty = series["points"][0];
		TEST_EXPECT(empty["players"].t() == crow::json::type::Null && !empty["running"].b() &&
				empty["egress_bytes"].t() == crow::json::type::Null && empty["cpu_percent"].t() == crow::json::type::Null);
	}
	// 24h by default; another range refused.
	std::tie(reply, json) = stats(one, { with_session(owner_s) });
	TEST_EXPECT(reply.code == 200 && str(json["range"]) == "24h" && json["series"]["step_seconds"].i() == 300 &&
			json["series"]["points"].size() == 288);
	std::tie(reply, json) = stats(one + "?range=2h", { with_session(owner_s) });
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_field");
	// Another account's server is no server of the caller's; an ended one has no live view.
	std::tie(reply, json) = stats(one, { with_session(other_s) });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	std::tie(reply, json) = stats("/api/me/servers/" + std::to_string(ended) + "/stats?range=1h", { with_session(owner_s) });
	TEST_EXPECT(reply.code == 200 && json["live"].t() == crow::json::type::Null && json["summary"]["live_seconds"].i() == 60);

	// The account's servers together, and the 7d range from the rollups.
	std::tie(reply, json) = stats("/api/me/servers/stats?range=1h", { with_session(owner_s) });
	TEST_EXPECT(reply.code == 200 && json && !json.has("server_id") && json["live"].t() == crow::json::type::Null);
	TEST_EXPECT(json["summary"]["live_seconds"].i() == 180 && json["summary"]["peak_players"].i() == 4 &&
			json["summary"]["egress_bytes"].i() == 2010);
	std::tie(reply, json) = stats("/api/me/servers/stats?range=7d", { with_session(owner_s) });
	TEST_EXPECT(reply.code == 200 && json["series"]["step_seconds"].i() == 3600 && json["series"]["points"].size() == 168);
	// Totals over the rollups: the hours' peaks added up, approximately.
	TEST_EXPECT(json["summary"]["egress_bytes"].i() == 2010 && json["summary"]["approximate"].size() == 4);
	TEST_EXPECT(h.send("GET", "/api/me/servers/stats").code == 401);

	// The admin's: one server and the fleet (a player's session refused).
	TEST_EXPECT(h.send("GET", "/api/admin/hosted/stats", { with_session(owner_s) }).code == 403);
	TEST_EXPECT(h.send("GET", "/api/admin/hosted/" + std::to_string(running) + "/stats", { with_session(owner_s) }).code == 403);
	std::tie(reply, json) = stats("/api/admin/hosted/stats?range=1h", { kBearer });
	TEST_EXPECT(reply.code == 200 && json && !json.has("server_id") && json["live"].t() == crow::json::type::Null);
	TEST_EXPECT(json["summary"]["live_seconds"].i() == 240 && json["summary"]["peak_players"].i() == 13);
	std::tie(reply, json) = stats("/api/admin/hosted/stats?range=30d", { kBearer });
	TEST_EXPECT(reply.code == 200 && json["series"]["points"].size() == 180 && json["summary"]["approximate"].size() == 4);
	std::tie(reply, json) = stats("/api/admin/hosted/" + std::to_string(others) + "/stats?range=1h", { kBearer });
	TEST_EXPECT(reply.code == 200 && json["server_id"].i() == others && json["summary"]["peak_players"].i() == 9);
	TEST_EXPECT(h.send("GET", "/api/admin/hosted/999999/stats", { kBearer }).code == 404);
	// The fleet view still answers at /api/admin/hosted/<id>.
	TEST_EXPECT(h.send("GET", "/api/admin/hosted/" + std::to_string(running), { kBearer }).code == 200);

	// Leave no live server for the cases after.
	opennova::hostdb::remove_host_by_rid(*db, rid);
	for (const int64_t id : { running, others }) hosted::request_stop(*db, id, "by the test");
	end_stopping_servers(*db);
	TEST_EXPECT(nws::delete_service_account(*db, service.id).ok);
	return 0;
}

// The admin's usage view (GET /api/admin/hosted/usage, hosted/stats_routes.h)
// over samples written by hand at this minute (no sampler runs here) and the
// harness's slot limits (256 MiB and 2 CPUs a container): admin only (401
// with no credential, 403 for a player's session), no-store; every live
// server and no ended one, by machine then slot with the ones placed nowhere
// last; each server's owner, place, newest sample (one older than 3 minutes
// is none) and last hour (a sample past it left out); each machine's slots and
// the usage of the servers on it; and the fleet's totals.
int test_hosted_usage_route(Harness &h) {
	namespace hosted = opennova::novaworld_server::hosted;
	namespace billing = opennova::novaworld_server::billing;
	using crow::json::type;
	const int64_t ana = make_account(h, "usage_ana", "pw-usage-ana");
	const int64_t ben = make_account(h, "usage_ben", "pw-usage-ben");
	TEST_EXPECT(ana > 0 && ben > 0);
	TEST_EXPECT(nws::set_comp_servers(*h.pool.acquire(), ana, 3).ok);
	TEST_EXPECT(nws::set_comp_servers(*h.pool.acquire(), ben, 2).ok);
	const std::string ana_s = login_token(h, "10.55.0.1", "usage_ana", "pw-usage-ana");
	TEST_EXPECT(!ana_s.empty());
	auto db = h.pool.acquire();

	// A ready shared machine of 4 slots and a ready own machine of 1 (0 when
	// one did not get there).
	const auto ready_machine = [&](hosted::MachineKind kind, int slots,
									   const char *cloud_id) -> int64_t {
		const auto m =
				hosted::create_machine(*db, { "local", kind, slots, "opennova-serve:local", 0 });
		hosted::LaunchedMachine launched;
		launched.cloud_id = cloud_id;
		launched.private_address = "10.0.1.8";
		launched.instance_type = kind == hosted::MachineKind::Shared ? "t3a.large" : "c7a.medium";
		const bool ready = hosted::mark_launched(*db, m.id, launched) &&
				hosted::record_sync(*db, m.id, "test-agent", {}).has_value();
		return ready ? m.id : 0;
	};
	const int64_t shared = ready_machine(hosted::MachineKind::Shared, 4, "local-usage-1");
	const int64_t own = ready_machine(hosted::MachineKind::Own, 1, "local-usage-2");
	TEST_EXPECT(shared > 0 && own > shared);

	// The servers: Ana's running on the shared machine's slot 1, Ben's booting
	// on its slot 0, Ana's running on the own machine, Ana's waiting for a
	// place, and Ben's ended.
	hosted::HostedSettings settings;
	settings.game = hosted::kDataSetJointOperations;
	settings.game_name = "Usage Routes";
	settings.missions = { "a.bms" };
	const auto start = [&](int64_t owner, const char *name) {
		settings.game_name = name;
		const auto r = hosted::request_server(
				*db, owner, settings, hosted::known_data_sets(), 50, billing::unix_now());
		return r.outcome == hosted::RequestOutcome::Created ? r.id : 0;
	};
	const auto to_running = [&](int64_t id) {
		bool moved = true;
		for (const auto &[from, to] :
				{ std::pair<hosted::State, hosted::State>{
						  hosted::State::Requested, hosted::State::Provisioning },
						{ hosted::State::Provisioning, hosted::State::Booting },
						{ hosted::State::Booting, hosted::State::Listing },
						{ hosted::State::Listing, hosted::State::Running } })
			moved = moved && hosted::transition(*db, id, from, to, "by the test");
		return moved;
	};
	const int64_t a = start(ana, "Usage A");
	const int64_t b = start(ben, "Usage B");
	const int64_t c = start(ana, "Usage C");
	const int64_t d = start(ana, "Usage D");
	const int64_t e = start(ben, "Usage E");
	TEST_EXPECT(a > 0 && b > 0 && c > 0 && d > 0 && e > 0);
	TEST_EXPECT(hosted::assign_slot(*db, a, shared, 1, 32769) == 1);
	TEST_EXPECT(hosted::assign_slot(*db, b, shared, 0, 32768) == 1);
	TEST_EXPECT(hosted::assign_slot(*db, c, own, 0, 32768) == 1);
	TEST_EXPECT(to_running(a) && to_running(c));
	TEST_EXPECT(hosted::transition(
			*db, b, hosted::State::Requested, hosted::State::Provisioning, "by the test"));
	TEST_EXPECT(hosted::transition(
			*db, b, hosted::State::Provisioning, hosted::State::Booting, "by the test"));
	TEST_EXPECT(hosted::transition(
			*db, e, hosted::State::Requested, hosted::State::Stopped, "by the test"));

	// The minutes, on an empty slate (an earlier case's hand-driven supervisor
	// pass sampled the servers live then).
	db->exec("DELETE FROM hosted_server_samples;");
	const int64_t now = billing::unix_now();
	const int64_t minute = now - now % 60;
	const auto put = [&](int64_t id, int64_t at, const char *state, std::optional<int64_t> players,
							 std::optional<double> cpu, std::optional<int64_t> memory,
							 int64_t egress) {
		hosted::ServerSample s;
		s.server_id = id;
		s.at = at;
		s.state = state;
		s.players = players;
		s.max_players = players ? std::optional<int64_t>(32) : std::nullopt;
		s.cpu_percent = cpu;
		s.memory_bytes = memory;
		s.egress_bytes = egress;
		hosted::insert_sample(*db, s);
	};
	constexpr int64_t kMiB = 1024 * 1024;
	put(a, minute, "running", 6, 25.0, 200 * kMiB, 6000);
	put(a, minute - 60, "running", 4, 75.0, 300 * kMiB, 3000);
	put(a, minute - 7200, "running", 9, 99.0, 500 * kMiB, 999999); // past the hour
	put(b, minute, "booting", std::nullopt, std::nullopt, std::nullopt, 0);
	put(c, minute - 600, "running", 2, 10.0, 100 * kMiB, 1200); // too old to be its newest
	put(e, minute, "running", 3, 50.0, 50 * kMiB, 700); // ended: not listed

	const std::string path = "/api/admin/hosted/usage";
	TEST_EXPECT(h.send("GET", path).code == 401);
	TEST_EXPECT(h.send("GET", path, { with_session(ana_s) }).code == 403);
	const auto reply = h.send("GET", path, { kBearer });
	if (reply.code != 200)
		std::fprintf(stderr, "  %s\n", describe(reply).c_str());
	const auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && header_value(reply, "Cache-Control") == "no-store");
	TEST_EXPECT(json["window_seconds"].i() == 3600 && json["slot_memory_bytes"].i() == 256 * kMiB);
	TEST_EXPECT(std::fabs(json["slot_cpus"].d() - 2.0) < 1e-9 && str(json["at"]).size() == 20 &&
			str(json["at"]).back() == 'Z');

	// The fleet's totals: A alone reports (B's sample carries no numbers, C's
	// is too old); the hour counts A's two minutes and C's one.
	const auto &t = json["totals"];
	TEST_EXPECT(t["servers_live"].i() == 4 && t["machines_live"].i() == 2);
	TEST_EXPECT(t["slots"].i() == 5 && t["slots_used"].i() == 3 && t["reporting"].i() == 1 &&
			t["players"].i() == 6);
	TEST_EXPECT(t["memory_bytes"].i() == 200 * kMiB && t["memory_limit_bytes"].i() == 256 * kMiB);
	TEST_EXPECT(std::fabs(t["cpus_used"].d() - 0.5) < 1e-9 &&
			std::fabs(t["cpus_limit"].d() - 2.0) < 1e-9);
	TEST_EXPECT(std::fabs(t["egress_bytes_per_second"].d() - 100.0) < 1e-9 &&
			t["egress_bytes_window"].i() == 10200);

	// The machines, by id, each with the usage of the servers on it.
	TEST_EXPECT(json["machines"].size() == 2);
	if (json["machines"].size() == 2) {
		const auto &m = json["machines"][0];
		TEST_EXPECT(
				m["id"].i() == shared && str(m["kind"]) == "shared" && str(m["state"]) == "ready");
		TEST_EXPECT(str(m["instance_type"]) == "t3a.large" && m["slots"].i() == 4 &&
				m["slots_used"].i() == 2);
		TEST_EXPECT(m["reporting"].i() == 1 && m["players"].i() == 6 &&
				m["memory_bytes"].i() == 200 * kMiB);
		TEST_EXPECT(m["memory_limit_bytes"].i() == 256 * kMiB &&
				std::fabs(m["cpus_used"].d() - 0.5) < 1e-9);
		TEST_EXPECT(std::fabs(m["egress_bytes_per_second"].d() - 100.0) < 1e-9 &&
				m["egress_bytes_window"].i() == 9000);
		const auto &o = json["machines"][1];
		TEST_EXPECT(o["id"].i() == own && str(o["kind"]) == "own" && o["slots"].i() == 1 &&
				o["slots_used"].i() == 1);
		TEST_EXPECT(o["reporting"].i() == 0 && o["memory_bytes"].i() == 0 &&
				o["egress_bytes_window"].i() == 1200);
		TEST_EXPECT(std::fabs(o["egress_bytes_per_second"].d()) < 1e-9 &&
				std::fabs(o["cpus_limit"].d()) < 1e-9);
	}

	// The servers: by machine, then slot; the one placed nowhere last; no ended one.
	const auto &servers = json["servers"];
	TEST_EXPECT(servers.size() == 4);
	if (servers.size() == 4) {
		TEST_EXPECT(servers[0]["id"].i() == b && servers[1]["id"].i() == a &&
				servers[2]["id"].i() == c && servers[3]["id"].i() == d);
		const auto &sa = servers[1];
		TEST_EXPECT(str(sa["name"]) == "Usage A" && str(sa["state"]) == "running" &&
				!sa["own_machine"].b());
		TEST_EXPECT(sa["owner"]["id"].i() == ana && str(sa["owner"]["username"]) == "usage_ana");
		TEST_EXPECT(sa["machine_ref"].i() == shared && sa["slot"].i() == 1 &&
				sa["uptime_seconds"].t() == type::Number);
		TEST_EXPECT(str(sa["sample_at"]) == hosted::iso_utc(minute) && sa["players"].i() == 6 &&
				sa["max_players"].i() == 32);
		TEST_EXPECT(std::fabs(sa["cpu_percent"].d() - 25.0) < 1e-9 &&
				sa["memory_bytes"].i() == 200 * kMiB);
		TEST_EXPECT(std::fabs(sa["egress_bytes_per_second"].d() - 100.0) < 1e-9);
		const auto &wa = sa["window"];
		TEST_EXPECT(wa["minutes"].i() == 2 && std::fabs(wa["cpu_percent_mean"].d() - 50.0) < 1e-9);
		TEST_EXPECT(std::fabs(wa["cpu_percent_peak"].d() - 75.0) < 1e-9 &&
				wa["memory_bytes_peak"].i() == 300 * kMiB);
		TEST_EXPECT(wa["egress_bytes"].i() == 9000);
		// Booting: its sample, without its agent's numbers, and no uptime.
		const auto &sb = servers[0];
		TEST_EXPECT(str(sb["owner"]["username"]) == "usage_ben" && sb["slot"].i() == 0 &&
				str(sb["state"]) == "booting");
		TEST_EXPECT(sb["uptime_seconds"].t() == type::Null && sb["sample_at"].t() == type::String);
		TEST_EXPECT(sb["players"].t() == type::Null && sb["cpu_percent"].t() == type::Null &&
				sb["memory_bytes"].t() == type::Null);
		TEST_EXPECT(std::fabs(sb["egress_bytes_per_second"].d()) < 1e-9 &&
				sb["window"]["minutes"].i() == 1);
		TEST_EXPECT(sb["window"]["cpu_percent_mean"].t() == type::Null &&
				sb["window"]["memory_bytes_peak"].t() == type::Null);
		// Its newest sample too old: none, its hour still counted.
		const auto &sc = servers[2];
		TEST_EXPECT(sc["machine_ref"].i() == own && sc["slot"].i() == 0 &&
				sc["sample_at"].t() == type::Null);
		TEST_EXPECT(sc["memory_bytes"].t() == type::Null &&
				sc["egress_bytes_per_second"].t() == type::Null);
		TEST_EXPECT(sc["window"]["memory_bytes_peak"].i() == 100 * kMiB &&
				sc["window"]["egress_bytes"].i() == 1200);
		// Placed nowhere.
		const auto &sd = servers[3];
		TEST_EXPECT(sd["machine_ref"].t() == type::Null && sd["slot"].t() == type::Null &&
				str(sd["state"]) == "requested");
		TEST_EXPECT(sd["window"]["minutes"].i() == 0 && sd["window"]["egress_bytes"].i() == 0);
	}
	// No credential reaches the reply.
	TEST_EXPECT(body_text(reply).find("password") == std::string::npos &&
			body_text(reply).find("secret") == std::string::npos);

	// Leave no live server or machine for the cases after.
	for (const int64_t id : { a, b, c, d })
		hosted::request_stop(*db, id, "by the test");
	end_stopping_servers(*db);
	for (const int64_t m : { shared, own })
		TEST_EXPECT(hosted::transition_machine(*db, m, hosted::FleetMachineState::Ready,
				hosted::FleetMachineState::Ended, "by the test"));
	return 0;
}

// The console routes (hosted/console_routes.h) over a real admin link
// (hosted/admin_link.h) to a scripted remote-admin server on loopback
// (FakeAdminConsole), the hosted row put in running by hand (no supervisor):
// the session and CSRF refusals (an admin session's too) and the proxy-only
// listener's; the owner's server resolved by its session alone, so another
// account reaches none and neither reaches the admin's routes; each curated
// command and its reply, in the exact line the server receives; the arguments
// refused before anything is sent (JSON-escaped control characters among
// them); hostile replies parsed into valid JSON; the rotation's checks (no such
// entry, the last map, a full rotation, between matches); the admission (one
// request in flight per account, one rotation edit per server); the owner's
// write bucket; the console ring (the writes recorded with their sender, the
// reads not, the admin password nowhere, `limit` and `after`); the admin's
// routes and their one wider read; the refusals before running, with no link
// and while stopping; and every line the server was sent checked against the
// curated shapes.
int test_hosted_console_routes(Harness &h) {
	namespace hosted = opennova::novaworld_server::hosted;
	const std::string password = "ConsoleRoutesAdminPw0123456789";
	FakeAdminConsole fake(password);
	TEST_EXPECT(fake.start());
	const int64_t owner = make_account(h, "console_owner", "pw-console-owner");
	const int64_t admin = make_account(h, "console_admin", "pw-console-admin");
	TEST_EXPECT(owner > 0 && admin > 0 && make_account(h, "console_other", "pw-console-other") > 0);
	TEST_EXPECT(nws::set_comp_servers(*h.pool.acquire(), owner, 1).ok);
	TEST_EXPECT(set_role(h, admin, "admin"));
	const std::string owner_s = login_token(h, "10.53.0.1", "console_owner", "pw-console-owner");
	const std::string other_s = login_token(h, "10.53.0.2", "console_other", "pw-console-other");
	const std::string admin_s = login_token(h, "10.53.0.3", "console_admin", "pw-console-admin");
	TEST_EXPECT(!owner_s.empty() && !other_s.empty() && !admin_s.empty());

	auto get = [&h](const std::string &path, const std::vector<std::string> &headers) {
		return h.send("GET", path, headers, {}, 5000);
	};
	auto post = [&h](const std::string &path, std::vector<std::string> headers, const std::string &body = "{}") {
		headers.push_back(kJson);
		return h.send("POST", path, headers, body, 5000);
	};
	const std::vector<std::string> as_owner = { with_session(owner_s) };
	const std::vector<std::string> as_owner_post = { with_session(owner_s), kCsrf };
	auto result_of = [](const net::HttpReply &r) {
		const auto json = crow::json::load(body_text(r));
		return json && json.has("result") ? str(json["result"]) : std::string("<none>");
	};

	// No such server; then one, not running yet; then running with no link.
	auto reply = get("/api/me/servers/999999/players", as_owner);
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	hosted::HostedSettings settings;
	settings.game = hosted::kDataSetJointOperations;
	settings.game_name = "Console Test";
	settings.missions = { "alpha.bms" };
	const auto request = hosted::request_server(*h.pool.acquire(), owner, settings, hosted::known_data_sets(), 10,
			opennova::novaworld_server::billing::unix_now());
	TEST_EXPECT(request.outcome == hosted::RequestOutcome::Created);
	const int64_t id = request.id;
	const std::string base = "/api/admin/hosted/" + std::to_string(id);
	// The owner's routes name the server by its id.
	const std::string me = "/api/me/servers/" + std::to_string(id);
	reply = get(me + "/players", as_owner);
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "server_not_running");
	{
		auto db = h.pool.acquire();
		db->exec("UPDATE hosted_servers SET state = 'running', machine_address = '127.0.0.1' WHERE id = $1;",
				{ opennova::db::BindValue(id) });
		hosted::set_admin_login(*db, id, fake.port(), "svc", password);
	}
	reply = get(me + "/players", as_owner);
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "admin_unavailable");
	hosted::AdminLinkTarget target;
	target.host = "127.0.0.1";
	target.port = fake.port();
	target.user = "svc";
	target.password = password;
	h.links->sync({ { id, target } });
	TEST_EXPECT(wait_until([&] {
		const auto link = h.links->find(id);
		return link && link->status().state == hosted::AdminLinkState::Up;
	}, 10000));

	// Logged out: 401; a POST without the CSRF header: 403, an admin session's too, before
	// anything is read; the proxy-only listener answers no one but its proxy.
	TEST_EXPECT(get(me + "/players", {}).code == 401);
	TEST_EXPECT(post(me + "/players/1/punt", {}).code == 401);
	reply = post(me + "/players/1/punt", { with_session(owner_s) });
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "csrf_header_required");
	reply = post(base + "/players/1/punt", { with_session(admin_s) });
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "csrf_header_required");
	reply = h.send_to(h.fresh_port, "GET", me + "/players", as_owner);
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "proxy_required");
	reply = h.send_to(h.fresh_port, "POST", me + "/chat", { with_session(owner_s), kCsrf, kJson },
			"{\"text\":\"hi\"}");
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "proxy_required");
	// Another account reaches no server of the owner's: its id answers not_found,
	// as no server's does (never a 403 that would tell it exists).
	reply = get(me + "/players", { with_session(other_s) });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	reply = post(me + "/players/1/punt", { with_session(other_s), kCsrf });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	reply = get(me + "/console", { with_session(other_s) });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	// Nor do the owners reach the admin's routes.
	TEST_EXPECT(get(base + "/players", { with_session(other_s) }).code == 403);
	TEST_EXPECT(get(base + "/players", { with_session(owner_s) }).code == 403);
	TEST_EXPECT(get(base + "/settings", { with_session(owner_s) }).code == 403);
	TEST_EXPECT(post(base + "/players/1/punt", { with_session(owner_s), kCsrf }).code == 403);

	// The reads.
	reply = get(me + "/players", as_owner);
	auto json = crow::json::load(body_text(reply));
	if (reply.code != 200) std::fprintf(stderr, "  players: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 200 && json && json["players"].size() == 2);
	TEST_EXPECT(json["server_id"].i() == id && json["players"][0]["slot"].i() == 1);
	TEST_EXPECT(str(json["players"][0]["name"]) == "Ann" && json["players"][0]["team"].i() == 1);
	TEST_EXPECT(json["players"][0]["kills"].i() == 5 && json["players"][1]["ping"].i() == 55);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	reply = get(me + "/missions", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["missions"].size() == 2);
	TEST_EXPECT(str(json["missions"][1]["file"]) == "BRAVO.BMS" && str(json["missions"][1]["title"]) == "Bravo Ridge");
	reply = get(me + "/rotation", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["rotation"].size() == 1 && json["rotation"][0]["current"].b());

	// Hostile replies: names holding a control byte and a cp1252 letter, lines that are
	// no row; the JSON stays valid and the parsers keep what parses.
	fake.set_hostile(true);
	reply = get(me + "/players", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["players"].size() == 2);
	TEST_EXPECT(json && str(json["players"][0]["name"]) == "Ev\x01l" && str(json["players"][1]["name"]) == "Caf\xC3\xA9");
	reply = get(me + "/rotation", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["rotation"].size() == 1 &&
			str(json["rotation"][0]["file"]) == "ALPHA.BMS");
	reply = get(me + "/missions", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["missions"].size() == 1 && json["missions"][0]["index"].i() == 7 &&
			str(json["missions"][0]["title"]) == "Odd (title)");
	reply = get(me + "/chat", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["lines"].size() == 2 && str(json["lines"][0]) == "\x01" "ctl" &&
			str(json["lines"][1]) == "\xC3\xA9t\xC3\xA9");
	fake.set_hostile(false);

	// Arguments outside their rules: refused before anything is sent or any token drawn.
	const size_t lines_before = fake.lines().size();
	struct Refused {
		std::string path;
		const char *body;
		const char *error; // "" for any 400
	};
	const std::string long_mission = "{\"mission\":\"" + std::string(33, 'm') + ".bms\"}";
	const std::string long_text = "{\"text\":\"" + std::string(60, 'x') + "\"}";
	const Refused refusals[] = {
		{ me + "/players/0/punt", "{}", "invalid_slot" },
		{ me + "/players/256/ban", "{}", "invalid_slot" },
		{ me + "/rotation/32/remove", "{}", "invalid_index" },
		{ me + "/rotation/256/next", "{}", "invalid_index" },
		{ me + "/rotation", "{\"mission\":\"../x.bms\"}", "invalid_mission" },
		{ me + "/rotation", "{\"mission\":\"a.bms 1 0 ONESHOT\"}", "invalid_mission" },
		{ me + "/rotation", "{\"mission\":\".bms\"}", "invalid_mission" },
		{ me + "/rotation", long_mission.c_str(), "invalid_mission" },
		{ me + "/rotation", "{\"mission\":\"a\\u0000.bms\"}", "" },
		{ me + "/rotation", "{\"mission\":\"a\\u0085.bms\"}", "invalid_mission" },
		{ me + "/rotation", "{\"mission\":\"a\\t.bms\"}", "invalid_mission" },
		{ me + "/rotation", "{\"mission\":\"a\\u007f.bms\"}", "invalid_mission" },
		{ me + "/rotation", "{}", "missing_field" },
		{ me + "/rotation", "{\"mission\":1}", "invalid_field" },
		{ me + "/rotation", "{\"mission\":\"a.bms\",\"swap_sides\":1}", "invalid_field" },
		{ me + "/chat", "{\"text\":\"two\\nlines\"}", "invalid_text" },
		{ me + "/chat", "{\"text\":\"a\\u0000b\"}", "" },
		{ me + "/chat", "{\"text\":\"a\\u0085b\"}", "invalid_text" },
		{ me + "/chat", "{\"text\":\"a\\tb\"}", "invalid_text" },
		{ me + "/chat", "{\"text\":\"a\\u007fb\"}", "invalid_text" },
		{ me + "/chat", "{\"text\":\"\\ud83d\\ude00\"}", "invalid_text" },
		{ me + "/chat", "{\"text\":\"   \"}", "invalid_text" },
		{ me + "/chat", long_text.c_str(), "invalid_text" },
		{ me + "/chat", "{}", "missing_field" },
		{ me + "/chat", "not json", "invalid_json" },
	};
	for (const Refused &r : refusals) {
		reply = post(r.path, as_owner_post, r.body);
		const bool ok = reply.code == 400 && (*r.error == '\0' || error_of_reply(reply) == r.error);
		if (!ok) std::fprintf(stderr, "  %s %s -> %s\n", r.path.c_str(), r.body, describe(reply).c_str());
		TEST_EXPECT(ok);
	}
	TEST_EXPECT(fake.lines().size() == lines_before);

	// The writes, eight of them: the owner's bucket.
	reply = post(me + "/players/1/punt", as_owner_post);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && str(json["result"]) == "punted" && json["slot"].i() == 1);
	reply = post(me + "/players/9/kill", as_owner_post);
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "player_not_found");
	reply = post(me + "/players/1/ban", as_owner_post);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && str(json["result"]) == "banned" && json["listed"].b());
	reply = post(me + "/players/2/ban", as_owner_post);
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "no_pcid");
	TEST_EXPECT(body_text(reply).find("Bob") == std::string::npos);
	reply = post(me + "/players/2/swap", as_owner_post);
	TEST_EXPECT(reply.code == 200 && result_of(reply) == "swapped");
	reply = post(me + "/chat", as_owner_post, "{\"text\":\"hello from the site\"}");
	TEST_EXPECT(reply.code == 200 && result_of(reply) == "sent");
	reply = post(me + "/rotation", as_owner_post, "{\"mission\":\"bravo.bms\",\"swap_sides\":true}");
	TEST_EXPECT(reply.code == 200 && result_of(reply) == "added");
	reply = post(me + "/rotation", as_owner_post, "{\"mission\":\"nope.bms\"}");
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "mission_not_found");
	// The exact lines the server received for them.
	TEST_EXPECT(in_order(fake.lines(), { "PLAYER PUNT 1", "PLAYER KILL 9", "PLAYER BAN 1", "PLAYER BAN 2",
			"PLAYER SWAPTEAM 2", "CHAT SEND hello from the site", "MISSION LIST", "MISSION ADD bravo.bms 1",
			"MISSION LIST", "MISSION ADD nope.bms 0" }));
	// The bucket runs dry (one token every 2 s after eight).
	bool limited = false;
	for (int i = 0; i < 12 && !limited; ++i) {
		reply = post(me + "/rotation/cycle", as_owner_post);
		limited = reply.code == 429;
	}
	TEST_EXPECT(limited && error_of_reply(reply) == "rate_limited" && has_header(reply, "Retry-After"));
	// The chat window has the line (a read: another bucket).
	reply = get(me + "/chat", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["lines"].size() == 1 &&
			str(json["lines"][0]).find("hello from the site") != std::string::npos);

	// The admin's routes (the token, no bucket): the rotation's checks, the wider read.
	reply = get(base + "/rotation", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["rotation"].size() == 2 &&
			str(json["rotation"][1]["file"]) == "BRAVO.BMS");
	reply = post(base + "/rotation/5/remove", { kBearer });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "no_such_entry");
	reply = post(base + "/rotation/5/next", { kBearer });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "no_such_entry");
	reply = post(base + "/rotation/1/next", { kBearer });
	TEST_EXPECT(reply.code == 200 && result_of(reply) == "next_set");
	reply = post(base + "/rotation/0/remove", { kBearer });
	TEST_EXPECT(reply.code == 200 && result_of(reply) == "removed");
	reply = post(base + "/rotation/0/remove", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "last_mission");
	reply = post(base + "/rotation", { kBearer }, "{\"mission\":\"ALPHA.BMS\"}");
	TEST_EXPECT(reply.code == 200);
	// An admin session with the CSRF header works the same.
	reply = post(base + "/rotation", { with_session(admin_s), kCsrf }, "{\"mission\":\"A.BMS\"}");
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "mission_not_found"); // composed and sent: no such map
	reply = post(base + "/rotation/cycle", { kBearer });
	TEST_EXPECT(reply.code == 200 && result_of(reply) == "cycling");
	reply = post(base + "/players/2/kill", { kBearer });
	TEST_EXPECT(reply.code == 200 && result_of(reply) == "killed");
	reply = post(base + "/chat", { kBearer }, "{\"text\":\"the admin here\"}");
	TEST_EXPECT(reply.code == 200);
	reply = get(base + "/players", { kBearer });
	TEST_EXPECT(reply.code == 200 && crow::json::load(body_text(reply))["players"].size() == 2);
	reply = get(base + "/settings", { kBearer });
	json = crow::json::load(body_text(reply));
	bool named = false;
	if (reply.code == 200 && json) {
		for (const auto &row : json["settings"])
			named = named || (str(row["key"]) == "ServerName" && str(row["value"]) == "Console Test");
	}
	TEST_EXPECT(named);
	TEST_EXPECT(get("/api/admin/hosted/999999/players", { kBearer }).code == 404);

	// A full rotation takes no more (kConsoleRotationMax, policy).
	const std::vector<std::string> saved = fake.rotation();
	fake.set_rotation(std::vector<std::string>(32, "ALPHA.BMS"));
	reply = post(base + "/rotation", { kBearer }, "{\"mission\":\"BRAVO.BMS\"}");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "rotation_full");
	fake.set_rotation(saved);

	// Between matches: retail lists no rotation and answers an add OK while it appends
	// nothing, so the routes read the scene and refuse.
	fake.set_scene("Menus");
	reply = get(base + "/rotation", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_in_game");
	reply = post(base + "/rotation", { kBearer }, "{\"mission\":\"BRAVO.BMS\"}");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_in_game");
	reply = post(base + "/rotation/0/remove", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_in_game");
	reply = get(base + "/players", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_in_game");
	reply = post(base + "/rotation/cycle", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_in_game");
	TEST_EXPECT(fake.rotation() == saved);
	fake.set_scene("Game");

	// The admission: one console request in flight per account (a second answers at
	// once, with no token), and one rotation edit per server, its read and its edit a pair.
	const auto sent_since = [&fake](size_t before, const std::string &want) {
		const auto l = fake.lines();
		return std::find(l.begin() + static_cast<std::ptrdiff_t>(std::min(before, l.size())), l.end(), want) != l.end();
	};
	fake.delay("PLAYER LIST", 800);
	net::HttpReply slow;
	const size_t sent_before = fake.lines().size();
	std::thread first([&] { slow = get(me + "/players", as_owner); });
	const bool in_flight = wait_until([&] { return sent_since(sent_before, "PLAYER LIST"); }, 3000);
	const auto busy_started = std::chrono::steady_clock::now();
	const net::HttpReply second = get(me + "/missions", as_owner);
	const auto busy_took = std::chrono::steady_clock::now() - busy_started;
	first.join();
	TEST_EXPECT(in_flight && slow.code == 200);
	TEST_EXPECT(second.code == 429 && error_of_reply(second) == "console_busy" && has_header(second, "Retry-After"));
	TEST_EXPECT(busy_took < std::chrono::milliseconds(500));
	fake.delay("MISSION LIST", 800);
	TEST_EXPECT(fake.rotation().size() == 2);
	net::HttpReply edit;
	const size_t edit_before = fake.lines().size();
	std::thread editing([&] { edit = post(base + "/rotation/0/remove", { kBearer }); });
	const bool editing_now = wait_until([&] { return sent_since(edit_before, "MISSION LIST"); }, 3000);
	const net::HttpReply racing = post(me + "/rotation/1/remove", as_owner_post);
	editing.join();
	fake.delay("", 0);
	TEST_EXPECT(editing_now && racing.code == 409 && error_of_reply(racing) == "rotation_busy");
	TEST_EXPECT(edit.code == 200 && fake.rotation().size() == 1);

	// The console: the writes recorded with their sender, the reads not, no password.
	reply = get(me + "/console", as_owner);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && str(json["link"]["state"]) == "up");
	bool owner_punt = false;
	bool punt_reply = false;
	bool admin_kill = false;
	bool read_recorded = false;
	int64_t first_id = 0;
	int64_t last_id = 0;
	size_t total = 0;
	if (json) {
		total = json["lines"].size();
		if (total != 0) first_id = json["lines"][0]["id"].i();
		for (const auto &line : json["lines"]) {
			const std::string kind = str(line["kind"]);
			const std::string text = str(line["text"]);
			owner_punt = owner_punt || (kind == "command" && str(line["actor"]) == "owner" && text == "PLAYER PUNT 1");
			punt_reply = punt_reply || (kind == "reply" && text == "OK - Player punted.");
			admin_kill = admin_kill || (kind == "command" && str(line["actor"]) == "admin" && text == "PLAYER KILL 2");
			read_recorded = read_recorded || (kind == "command" && (text == "PLAYER LIST" || text == "MISSION LIST" ||
																		   text == "CHAT GET" || text == "GET GAMESTATE"));
			last_id = line["id"].i();
		}
	}
	TEST_EXPECT(owner_punt && punt_reply && admin_kill && !read_recorded);
	TEST_EXPECT(body_text(reply).find(password) == std::string::npos);
	// Past the last id: nothing but a status line the link's poll may have added since.
	reply = get(me + "/console?after=" + std::to_string(last_id), as_owner);
	json = crow::json::load(body_text(reply));
	bool past_last = reply.code == 200 && json;
	if (past_last) {
		for (const auto &line : json["lines"])
			past_last = past_last && line["id"].i() > last_id && str(line["kind"]) == "status";
	}
	TEST_EXPECT(past_last);
	// limit=0 reads one line; limit=9999 every line from the oldest (fewer than 200 here, so
	// the default read's too); after=abc is no after.
	reply = get(me + "/console?limit=0", as_owner);
	TEST_EXPECT(reply.code == 200 && crow::json::load(body_text(reply))["lines"].size() == 1);
	TEST_EXPECT(total > 0 && total < 200); // ConsoleRoutes::kConsoleLinesShown
	for (const char *query : { "?limit=9999", "?after=abc" }) {
		reply = get((me + "/console") + query, as_owner);
		json = crow::json::load(body_text(reply));
		TEST_EXPECT(reply.code == 200 && json && json["lines"].size() >= total &&
				json["lines"][0]["id"].i() == first_id);
	}
	TEST_EXPECT(get(base + "/console", { kBearer }).code == 200);

	// Every line the server was sent has a curated command's exact shape.
	for (const std::string &line : fake.lines()) {
		const bool curated = curated_line(line);
		if (!curated) std::fprintf(stderr, "  the server was sent '%s'\n", line.c_str());
		TEST_EXPECT(curated);
	}

	// Stopping: refused before the link, the console still read.
	TEST_EXPECT(hosted::request_stop(*h.pool.acquire(), id, "stopped by the test"));
	reply = get(me + "/players", as_owner);
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "server_stopping");
	reply = post(base + "/players/1/punt", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "server_stopping");
	TEST_EXPECT(get(me + "/console", as_owner).code == 200);

	// The end: the link down, the row ended.
	h.links->sync({});
	TEST_EXPECT(hosted::transition(*h.pool.acquire(), id, hosted::State::Stopping, hosted::State::Stopped,
			"ended by the test"));
	reply = get(base + "/players", { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_live");
	reply = get(me + "/players", as_owner);
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "not_live");
	// Its console log still reads.
	TEST_EXPECT(get(me + "/console", as_owner).code == 200);
	TEST_EXPECT(set_role(h, admin, "player")); // the cases after count no admin of this one's
	fake.stop();
	return 0;
}

// ---- support tiers (billing) --------------------------------------------------

// The harness's billing (billing/billing.h) runs over the in-memory Stripe
// (novaworld/billing_fake_stripe.h) with the settings below, its worker driven
// by hand (billing_pass); the Secure listener runs with none (billing off).
constexpr const char *kWebhookSecret = "whsec_harnesswebhooksecret";
constexpr const char *kSiteUrl = "https://site.example";

billing_fake::FakeStripeApi::Price harness_price(const char *id, int64_t amount) {
	billing_fake::FakeStripeApi::Price p;
	p.id = id;
	p.unit_amount = amount;
	return p;
}

void billing_pass(Harness &h) { h.billing->sync().pass(nws::billing::unix_now()); }

// A Stripe-Signature header for `body` at `t` under `secret`, as Stripe signs:
// the hex HMAC-SHA256 of "<t>.<body>".
std::string stripe_signature(const std::string &body, int64_t t, const std::string &secret = kWebhookSecret) {
	const std::string signed_text = std::to_string(t) + "." + body;
	return "Stripe-Signature: t=" + std::to_string(t) +
			",v1=" + opennova::io::sha256_hex(opennova::io::hmac_sha256(secret, signed_text));
}

std::string event_body(const std::string &id, const std::string &customer, bool livemode = false) {
	return "{\"id\":\"" + id + "\",\"object\":\"event\",\"type\":\"customer.subscription.updated\",\"livemode\":" +
			(livemode ? "true" : "false") + ",\"created\":" + std::to_string(nws::billing::unix_now()) +
			",\"data\":{\"object\":{\"id\":\"sub_x\",\"object\":\"subscription\",\"customer\":\"" + customer + "\"}}}";
}

// The tiers (public), the account's billing, checkout and the portal: the
// session and CSRF refusals, the item's rules (an own machine needs a tier, a
// tier while one grants is already_subscribed), the Checkout session's settings
// (the customer made once with its idempotency key and the account's metadata,
// the previous open session expired, the return URLs, the terms, the life), the
// sync after a checkout, the portal, the buckets, prices that do not check out
// (503 billing_misconfigured), Stripe unreachable or refusing the session (503
// stripe_unavailable, never a 502 Cloudflare would replace), and billing off.
int test_billing_routes(Harness &h) {
	namespace billing = opennova::novaworld_server::billing;
	using billing::StripeRequest;
	const int64_t ana = make_account(h, "bill_ana", "pw-bill-ana");
	const int64_t bea = make_account(h, "bill_bea", "pw-bill-bea");
	const int64_t cy = make_account(h, "bill_cy", "pw-bill-cy");
	TEST_EXPECT(ana > 0 && bea > 0 && cy > 0);
	const std::string ana_s = login_token(h, "10.61.0.1", "bill_ana", "pw-bill-ana");
	const std::string bea_s = login_token(h, "10.61.0.2", "bill_bea", "pw-bill-bea");
	const std::string cy_s = login_token(h, "10.61.0.3", "bill_cy", "pw-bill-cy");
	TEST_EXPECT(!ana_s.empty() && !bea_s.empty() && !cy_s.empty());
	auto post = [&h](const std::string &path, const std::string &session, const std::string &body = {},
						 bool csrf = true) {
		std::vector<std::string> headers = { kJson, with_session(session) };
		if (csrf) headers.push_back(kCsrf);
		return h.send("POST", path, headers, body);
	};
	auto me = [&h](const std::string &session) { return h.send("GET", "/api/me/billing", { with_session(session) }); };

	// The tiers: public, with Stripe's amounts once the prices check out.
	auto reply = h.send("GET", "/api/billing/tiers");
	auto json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["enabled"].b() && str(json["currency"]) == "usd");
	TEST_EXPECT(json["tiers"].size() == 3);
	if (json && json["tiers"].size() == 3) {
		for (int i = 0; i < 3; ++i) {
			TEST_EXPECT(json["tiers"][i]["tier"].i() == i + 1 && json["tiers"][i]["max_servers"].i() == i + 1);
			TEST_EXPECT(json["tiers"][i]["amount"].i() == 500 * (i + 1) && str(json["tiers"][i]["interval"]) == "month");
		}
	}
	TEST_EXPECT(json["own_machine"]["amount"].i() == 4500 && str(json["own_machine"]["interval"]) == "month");
	// Billing off: the tiers with no amount, no add-on; the rest 503.
	reply = h.send_to(h.secure_port, "GET", "/api/billing/tiers");
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && !json["enabled"].b() && json["tiers"].size() == 3 &&
			json["tiers"][0]["amount"].t() == crow::json::type::Null &&
			json["own_machine"].t() == crow::json::type::Null);

	// The account's billing: a session; nothing yet.
	TEST_EXPECT(h.send("GET", "/api/me/billing").code == 401);
	reply = me(ana_s);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && json["enabled"].b() && json["max_servers"].i() == 0 &&
			json["comp_servers"].i() == 0 && json["own_machines"].i() == 0 && json["used"].i() == 0 &&
			json["stopping"].i() == 0);
	TEST_EXPECT(!json["can_start"].b() && !json["can_manage"].b() && json["can_subscribe"].b());
	TEST_EXPECT(json["subscription"].t() == crow::json::type::Null &&
			json["own_machine_subscription"].t() == crow::json::type::Null);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");

	// Checkout's refusals: logged out, no CSRF header, the body (Bea's bucket).
	TEST_EXPECT(h.send("POST", "/api/billing/checkout", { kJson, kCsrf }, "{\"item\":\"tier1\"}").code == 401);
	reply = post("/api/billing/checkout", ana_s, "{\"item\":\"tier1\"}", /*csrf=*/false);
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "csrf_header_required");
	reply = post("/api/billing/checkout", bea_s, "not json");
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_json");
	for (const char *bad : { "{}", "{\"item\":\"tier4\"}", "{\"item\":2}", "{\"item\":\"premium\"}" }) {
		reply = post("/api/billing/checkout", bea_s, bad);
		TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_item");
	}
	TEST_EXPECT(h.stripe->count(StripeRequest::Method::Post, "/v1/customers") == 0);

	// Ana's checkouts (her bucket: 6). The add-on first needs a tier.
	reply = post("/api/billing/checkout", ana_s, "{\"item\":\"own_machine\"}");
	TEST_EXPECT(reply.code == 403 && error_of_reply(reply) == "not_eligible");
	reply = post("/api/billing/checkout", ana_s, "{\"item\":\"tier1\"}");
	json = crow::json::load(body_text(reply));
	if (reply.code != 200) std::fprintf(stderr, "  checkout: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 200 && json && str(json["url"]).rfind("https://checkout.stripe.com/", 0) == 0);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	const auto customer = billing::customer_of(*h.pool.acquire(), ana);
	TEST_EXPECT(customer && !customer->open_checkout_session.empty());
	const std::string first_session = customer ? customer->open_checkout_session : std::string();
	{
		std::lock_guard<std::mutex> lock(h.stripe->mu);
		const auto &made = h.stripe->customers[customer->stripe_customer_id];
		TEST_EXPECT(made.metadata.at(billing::kMetaInstance) == h.billing->instance_id() &&
				made.metadata.at(billing::kMetaUserId) == std::to_string(ana) &&
				made.metadata.at(billing::kMetaUsername) == "bill_ana");
		bool keyed = false;
		for (const StripeRequest &r : h.stripe->requests) {
			if (r.method == StripeRequest::Method::Post && r.path == "/v1/customers")
				keyed = r.idempotency_key == "opennova-" + h.billing->instance_id() + "-customer-" + std::to_string(ana);
		}
		TEST_EXPECT(keyed);
		const auto &session = h.stripe->checkout_sessions[first_session];
		TEST_EXPECT(session.customer == customer->stripe_customer_id && session.price_id == "price_htier1");
		TEST_EXPECT(session.success_url ==
				std::string(kSiteUrl) + "/me/support?checkout=success&session_id={CHECKOUT_SESSION_ID}");
		TEST_EXPECT(session.cancel_url == std::string(kSiteUrl) + "/me/support?checkout=cancel");
		TEST_EXPECT(session.client_reference_id == std::to_string(ana));
		TEST_EXPECT(session.subscription_metadata.at(billing::kMetaInstance) == h.billing->instance_id());
		const int64_t life = session.expires_at - billing::unix_now();
		TEST_EXPECT(life > 3500 && life <= 3600);
		bool terms = false;
		for (const auto &[k, v] : session.params) terms = terms || (k == "consent_collection[terms_of_service]" && v == "required");
		TEST_EXPECT(terms);
	}
	// Again: the same customer, the previous session expired.
	reply = post("/api/billing/checkout", ana_s, "{\"item\":\"tier1\"}");
	TEST_EXPECT(reply.code == 200);
	const std::string second_session = billing::customer_of(*h.pool.acquire(), ana)->open_checkout_session;
	TEST_EXPECT(second_session != first_session);
	TEST_EXPECT(h.stripe->count(StripeRequest::Method::Post, "/v1/customers") == 1);
	{
		std::lock_guard<std::mutex> lock(h.stripe->mu);
		TEST_EXPECT(h.stripe->checkout_sessions[first_session].status == "expired");
	}

	// She pays (Stripe makes the subscription); the success page's sync shows it.
	TEST_EXPECT(!h.stripe->complete_checkout(second_session).empty());
	reply = post("/api/me/billing/sync", ana_s);
	json = crow::json::load(body_text(reply));
	if (reply.code != 200) std::fprintf(stderr, "  sync: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 200 && json && json["max_servers"].i() == 1 && json["can_start"].b());
	TEST_EXPECT(json["subscription"]["tier"].i() == 1 && str(json["subscription"]["status"]) == "active" &&
			json["subscription"]["entitled"].b() && !json["subscription"]["cancel_at_period_end"].b());
	const std::string period_end = str(json["subscription"]["current_period_end"]);
	TEST_EXPECT(period_end.size() == 20 && period_end[10] == 'T' && period_end.back() == 'Z');
	TEST_EXPECT(json["can_manage"].b() && !json["can_subscribe"].b());
	// A tier while one grants: the portal changes it. The add-on now may start.
	reply = post("/api/billing/checkout", ana_s, "{\"item\":\"tier2\"}");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "already_subscribed");
	reply = post("/api/billing/checkout", ana_s, "{\"item\":\"own_machine\"}");
	TEST_EXPECT(reply.code == 200);
	// The portal, returning to the support page.
	reply = post("/api/billing/portal", ana_s);
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && str(json["url"]).rfind("https://billing.stripe.com/", 0) == 0);
	{
		std::lock_guard<std::mutex> lock(h.stripe->mu);
		TEST_EXPECT(!h.stripe->portal_sessions.empty() &&
				h.stripe->portal_sessions.back().return_url == std::string(kSiteUrl) + "/me/support");
	}
	reply = post("/api/billing/portal", bea_s);
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "no_billing_account");
	reply = post("/api/me/billing/sync", bea_s);
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "no_billing_account");
	// The sync bucket: 3 at once (Ana spent one).
	TEST_EXPECT(post("/api/me/billing/sync", ana_s).code == 200);
	TEST_EXPECT(post("/api/me/billing/sync", ana_s).code == 200);
	reply = post("/api/me/billing/sync", ana_s);
	TEST_EXPECT(reply.code == 429 && has_header(reply, "Retry-After"));
	// Ana's checkout bucket is spent too (six draws).
	TEST_EXPECT(post("/api/billing/checkout", ana_s, "{\"item\":\"tier1\"}").code == 429);

	// Prices that do not check out: checkout pauses (Cy's bucket).
	{
		std::lock_guard<std::mutex> lock(h.stripe->mu);
		h.stripe->prices["price_htier2"].interval = "year";
	}
	TEST_EXPECT(h.billing->sync().validate_prices() && !h.billing->sync().status().prices_ok);
	reply = post("/api/billing/checkout", cy_s, "{\"item\":\"tier2\"}");
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "billing_misconfigured");
	TEST_EXPECT(!crow::json::load(body_text(h.send("GET", "/api/billing/tiers")))["enabled"].b());
	TEST_EXPECT(!crow::json::load(body_text(me(cy_s)))["can_subscribe"].b());
	{
		std::lock_guard<std::mutex> lock(h.stripe->mu);
		h.stripe->prices["price_htier2"].interval = "month";
	}
	TEST_EXPECT(h.billing->sync().validate_prices() && h.billing->sync().status().prices_ok);
	// Stripe unreachable: 503 (a 502 is Cloudflare's to replace), nothing kept.
	h.stripe->fail_next(billing::StripeReply::Outcome::Unreachable);
	reply = post("/api/billing/checkout", cy_s, "{\"item\":\"tier2\"}");
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "stripe_unavailable");
	TEST_EXPECT(!billing::customer_of(*h.pool.acquire(), cy));
	// Stripe refuses the session, as it did prod's first checkouts (2026-10-16,
	// behind a 502 Cloudflare replaced): 503 stripe_unavailable; the customer
	// made, no session kept. Once the refusal is gone, the next one goes.
	h.stripe->refuse_next("/v1/checkout/sessions", 400, "parameter_invalid",
			"fake: a terms of service URL must be set in the Dashboard");
	const size_t sessions_before =
			h.stripe->count(StripeRequest::Method::Post, "/v1/checkout/sessions");
	reply = post("/api/billing/checkout", cy_s, "{\"item\":\"tier2\"}");
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "stripe_unavailable");
	TEST_EXPECT(h.stripe->count(StripeRequest::Method::Post, "/v1/checkout/sessions") ==
			sessions_before + 1);
	const auto cy_customer = billing::customer_of(*h.pool.acquire(), cy);
	TEST_EXPECT(cy_customer && cy_customer->open_checkout_session.empty());
	reply = post("/api/billing/checkout", cy_s, "{\"item\":\"tier2\"}");
	TEST_EXPECT(reply.code == 200 &&
			!billing::customer_of(*h.pool.acquire(), cy)->open_checkout_session.empty());

	// Billing off (the Secure listener): every account route 503 billing_disabled.
	std::string token;
	{
		auto conn = h.pool.acquire();
		const auto user = nws::get_user_by_id(*conn, cy);
		const auto minted = nws::create_web_session(*conn, cy, user->credentials_gen, "127.0.0.1", "harness");
		TEST_EXPECT(minted.has_value());
		token = *minted;
	}
	const std::string cookie = with_cookie(nws::kWebSessionHostCookie, token);
	reply = h.send_to(h.secure_port, "GET", "/api/me/billing", { cookie });
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "billing_disabled");
	for (const char *path : { "/api/billing/checkout", "/api/billing/portal", "/api/me/billing/sync" }) {
		reply = h.send_to(h.secure_port, "POST", path, { kJson, kCsrf, cookie }, "{\"item\":\"tier1\"}");
		TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "billing_disabled");
	}
	return 0;
}

// Checkout never sells twice what a paid session the webhook has not brought
// yet already sold: the previous session read from Stripe first, and once
// complete, the customer synced and the checkout judged again (a tier:
// already_subscribed; the add-on: own_machine_limit once the account has an own
// machine for every server it may run, several fine below that). A previous
// session Stripe no longer knows is no obstacle; one Stripe cannot be asked
// about refuses (503) with nothing made.
int test_billing_checkout_guard(Harness &h) {
	namespace billing = opennova::novaworld_server::billing;
	using billing::StripeRequest;
	const int64_t dot = make_account(h, "chk_dot", "pw-chk-dot");
	const int64_t fay = make_account(h, "chk_fay", "pw-chk-fay");
	TEST_EXPECT(dot > 0 && fay > 0);
	const std::string dot_s = login_token(h, "10.62.0.1", "chk_dot", "pw-chk-dot");
	const std::string fay_s = login_token(h, "10.62.0.2", "chk_fay", "pw-chk-fay");
	TEST_EXPECT(!dot_s.empty() && !fay_s.empty());
	auto checkout = [&h](const std::string &session, const char *item) {
		return h.send("POST", "/api/billing/checkout", { kJson, kCsrf, with_session(session) },
				std::string("{\"item\":\"") + item + "\"}");
	};
	auto open_session = [&h](int64_t user) {
		const auto customer = billing::customer_of(*h.pool.acquire(), user);
		return customer ? customer->open_checkout_session : std::string();
	};
	auto session_reads = [&h](const std::string &id) {
		return h.stripe->count(StripeRequest::Method::Get, "/v1/checkout/sessions/" + id);
	};

	// Dot pays for tier 1; before any sync, a checkout of tier 2 reads the
	// paid session, syncs her customer and refuses: she has a tier.
	auto reply = checkout(dot_s, "tier1");
	TEST_EXPECT(reply.code == 200);
	const std::string paid_tier = open_session(dot);
	TEST_EXPECT(!h.stripe->complete_checkout(paid_tier).empty());
	TEST_EXPECT(billing::entitlement_of(*h.pool.acquire(), dot, billing::unix_now()).max_servers == 0);
	reply = checkout(dot_s, "tier2");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "already_subscribed");
	TEST_EXPECT(session_reads(paid_tier) == 1);
	TEST_EXPECT(billing::entitlement_of(*h.pool.acquire(), dot, billing::unix_now()).max_servers == 1);
	// Her own machine: one, which tier 1's one server fills; paid, a second is
	// refused through the same sync.
	reply = checkout(dot_s, "own_machine");
	TEST_EXPECT(reply.code == 200);
	const std::string paid_machine = open_session(dot);
	TEST_EXPECT(paid_machine != paid_tier && !h.stripe->complete_checkout(paid_machine).empty());
	reply = checkout(dot_s, "own_machine");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "own_machine_limit");
	TEST_EXPECT(billing::entitlement_of(*h.pool.acquire(), dot, billing::unix_now()).own_machines == 1);
	// From the cache now, with no Stripe call.
	const size_t reads = session_reads(paid_machine);
	reply = checkout(dot_s, "own_machine");
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "own_machine_limit");
	TEST_EXPECT(session_reads(paid_machine) == reads);

	// Fay on tier 3: own machines one at a time below its three servers. A
	// previous session Stripe could not be asked about refuses; one it no
	// longer knows does not.
	const std::string fay_cus = h.stripe->add_customer({ { billing::kMetaInstance, h.billing->instance_id() },
			{ billing::kMetaUserId, std::to_string(fay) }, { billing::kMetaUsername, "chk_fay" } });
	h.stripe->add_subscription(fay_cus, "price_htier3", 1, "active", billing::unix_now() + 30 * 86400);
	TEST_EXPECT(h.billing->sync().sync_customer(*h.pool.acquire(), fay_cus, billing::unix_now()) ==
			billing::SyncOutcome::Synced);
	TEST_EXPECT(checkout(fay_s, "own_machine").code == 200);
	TEST_EXPECT(!h.stripe->complete_checkout(open_session(fay)).empty());
	reply = checkout(fay_s, "own_machine");
	TEST_EXPECT(reply.code == 200);
	TEST_EXPECT(billing::entitlement_of(*h.pool.acquire(), fay, billing::unix_now()).own_machines == 1);
	const std::string pending = open_session(fay);
	const size_t sessions_before = h.stripe->count(StripeRequest::Method::Post, "/v1/checkout/sessions");
	h.stripe->fail_next(billing::StripeReply::Outcome::Unreachable);
	reply = checkout(fay_s, "own_machine");
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "stripe_unavailable");
	TEST_EXPECT(open_session(fay) == pending &&
			h.stripe->count(StripeRequest::Method::Post, "/v1/checkout/sessions") == sessions_before);
	billing::set_open_checkout_session(*h.pool.acquire(), fay, "cs_test_gone");
	reply = checkout(fay_s, "own_machine");
	TEST_EXPECT(reply.code == 200 && open_session(fay) != "cs_test_gone");

	// A customer row of the other mode (the key went from live to test): no
	// customer this key can use; a new one replaces it, and the checkout goes on.
	const int64_t gil = make_account(h, "chk_gil", "pw-chk-gil");
	const std::string gil_s = login_token(h, "10.62.0.3", "chk_gil", "pw-chk-gil");
	TEST_EXPECT(gil > 0 && !gil_s.empty());
	TEST_EXPECT(billing::insert_customer(*h.pool.acquire(), gil, "cus_livemodegil", /*livemode=*/true));
	reply = checkout(gil_s, "tier1");
	TEST_EXPECT(reply.code == 200);
	const auto gil_customer = billing::customer_of(*h.pool.acquire(), gil);
	TEST_EXPECT(gil_customer && gil_customer->stripe_customer_id != "cus_livemodegil" && !gil_customer->livemode &&
			!gil_customer->open_checkout_session.empty());
	TEST_EXPECT(!billing::customer_by_id(*h.pool.acquire(), "cus_livemodegil"));

	// Two checkouts of one account at once (two tabs): while the first is
	// inside Stripe the second answers 409 checkout_in_progress; once it is
	// done, the next one runs.
	const int64_t hal = make_account(h, "chk_hal", "pw-chk-hal");
	const std::string hal_s = login_token(h, "10.62.0.4", "chk_hal", "pw-chk-hal");
	TEST_EXPECT(hal > 0 && !hal_s.empty());
	h.stripe->close_gate();
	net::HttpReply first;
	std::thread tab([&] {
		first = h.send("POST", "/api/billing/checkout", { kJson, kCsrf, with_session(hal_s) },
				"{\"item\":\"tier1\"}", 15000);
	});
	const bool inside = h.stripe->wait_at_gate(1, std::chrono::seconds(10));
	net::HttpReply second;
	if (inside) second = checkout(hal_s, "tier2");
	h.stripe->open_gate();
	tab.join();
	TEST_EXPECT(inside && second.code == 409 && error_of_reply(second) == "checkout_in_progress");
	TEST_EXPECT(first.code == 200);
	TEST_EXPECT(checkout(hal_s, "tier1").code == 200);
	return 0;
}

// POST /api/billing/webhook: no session and no CSRF header, the signature its
// credential. Missing, wrong, stale and several-v1 signatures; a body past
// 1 MiB (413); a signed body that is no event (400 invalid_json); an event of the
// other mode dropped (200); an event recorded once however often delivered,
// its customer synced by the worker's next pass into the account's billing;
// the refusals counted for the heartbeat; billing off (503).
int test_billing_webhook(Harness &h) {
	namespace billing = opennova::novaworld_server::billing;
	const int64_t dee = make_account(h, "bill_dee", "pw-bill-dee");
	const std::string dee_s = login_token(h, "10.62.0.1", "bill_dee", "pw-bill-dee");
	TEST_EXPECT(dee > 0 && !dee_s.empty());
	// Dee's customer and her tier 3, as a checkout left them, not synced yet.
	const std::string cus = h.stripe->add_customer({ { billing::kMetaInstance, h.billing->instance_id() },
			{ billing::kMetaUserId, std::to_string(dee) }, { billing::kMetaUsername, "bill_dee" } });
	TEST_EXPECT(billing::insert_customer(*h.pool.acquire(), dee, cus, false));
	h.stripe->add_subscription(cus, "price_htier3", 1, "active", billing::unix_now() + 30 * 86400);
	TEST_EXPECT(crow::json::load(body_text(h.send("GET", "/api/me/billing", { with_session(dee_s) })))["max_servers"]
						.i() == 0);

	auto deliver = [&h](const std::string &body, const std::vector<std::string> &extra) {
		std::vector<std::string> headers = { kJson };
		headers.insert(headers.end(), extra.begin(), extra.end());
		return h.send("POST", "/api/billing/webhook", headers, body);
	};
	const uint64_t rejects_before = h.billing->webhook_rejects();
	const int64_t now = billing::unix_now();
	const std::string body = event_body("evt_harness_1", cus);
	auto reply = deliver(body, {});
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "bad_signature");
	reply = deliver(body, { stripe_signature(body, now, "whsec_notthesecret") });
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "bad_signature");
	reply = deliver(body, { stripe_signature(body, now - 400) });
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "bad_signature");
	reply = deliver(body, { "Stripe-Signature: t=" + std::to_string(now) + ",v1=nothex" });
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "bad_signature");
	const std::string not_event = "{\"id\":\"evt_x\"}";
	reply = deliver(not_event, { stripe_signature(not_event, now) });
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_json");
	TEST_EXPECT(h.billing->webhook_rejects() == rejects_before + 5);
	const std::string huge(billing::BillingRoutes::kWebhookMaxBytes + 1, 'x');
	reply = deliver(huge, { stripe_signature(huge, now) });
	TEST_EXPECT(reply.code == 413);
	TEST_EXPECT(h.pool.acquire()->query("SELECT 1 FROM billing_events WHERE event_id = 'evt_harness_1';").empty());

	// Signed: received, recorded once however often it comes, with no session and no CSRF header.
	for (int i = 0; i < 2; ++i) {
		reply = deliver(body, { stripe_signature(body, now) });
		TEST_EXPECT(reply.code == 200 && body_text(reply) == "{\"received\":true}");
	}
	// Several v1 signatures (a secret's roll): any one matching passes.
	const std::string second = event_body("evt_harness_2", cus);
	const std::string both = stripe_signature(second, now) + ",v1=" + std::string(64, '0');
	TEST_EXPECT(deliver(second, { both }).code == 200);
	TEST_EXPECT(h.pool.acquire()->query("SELECT 1 FROM billing_events WHERE customer_id = $1;",
					{ opennova::db::BindValue(cus) }).size() == 2);
	// An event of the other mode: answered, dropped.
	const std::string live = event_body("evt_harness_live", cus, /*livemode=*/true);
	TEST_EXPECT(deliver(live, { stripe_signature(live, now) }).code == 200);
	TEST_EXPECT(h.pool.acquire()->query("SELECT 1 FROM billing_events WHERE event_id = 'evt_harness_live';").empty());

	// The worker's next pass reads Dee's customer back: her tier 3.
	billing_pass(h);
	auto json = crow::json::load(body_text(h.send("GET", "/api/me/billing", { with_session(dee_s) })));
	TEST_EXPECT(json && json["max_servers"].i() == 3 && json["subscription"]["tier"].i() == 3);
	TEST_EXPECT(h.pool.acquire()->query("SELECT 1 FROM billing_events WHERE customer_id = $1 AND outcome = 'synced';",
					{ opennova::db::BindValue(cus) }).size() == 2);

	// Billing off: 503.
	reply = h.send_to(h.secure_port, "POST", "/api/billing/webhook", { kJson, stripe_signature(body, now) }, body);
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "billing_disabled");
	return 0;
}

// The admin's billing view, its reconcile and relink: admin only; the mode,
// the price check, the last reconcile, the pending count, the subscriptions
// with their accounts, the webhook log, the orphans a reconcile found (another
// instance's subscription) and their relink to an account (its metadata
// stamped, the account's entitlement from it), and the account delete refused
// while a subscription grants.
int test_billing_admin(Harness &h) {
	namespace billing = opennova::novaworld_server::billing;
	const int64_t eli = make_account(h, "bill_eli", "pw-bill-eli");
	const std::string eli_s = login_token(h, "10.63.0.1", "bill_eli", "pw-bill-eli");
	TEST_EXPECT(eli > 0 && !eli_s.empty());
	TEST_EXPECT(h.send("GET", "/api/admin/billing").code == 401);
	TEST_EXPECT(h.send("GET", "/api/admin/billing", { with_session(eli_s) }).code == 403);
	TEST_EXPECT(h.send("POST", "/api/admin/billing/reconcile", { with_session(eli_s), kCsrf }).code == 403);

	// Another instance's customer with a granting tier: an orphan once reconciled.
	const std::string foreign = h.stripe->add_customer({ { billing::kMetaInstance, "another-instance" },
			{ billing::kMetaUserId, "424242" }, { billing::kMetaUsername, "bill_eli" } });
	const std::string orphan_sub =
			h.stripe->add_subscription(foreign, "price_htier2", 1, "active", billing::unix_now() + 30 * 86400);
	auto reply = h.send("POST", "/api/admin/billing/reconcile", { kBearer });
	TEST_EXPECT(reply.code == 202 && str(crow::json::load(body_text(reply))["reconcile"]) == "requested");
	billing_pass(h);
	reply = h.send("GET", "/api/admin/billing", { kBearer });
	auto json = crow::json::load(body_text(reply));
	if (reply.code != 200) std::fprintf(stderr, "  admin billing: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 200 && json && json["enabled"].b() && str(json["mode"]) == "test" &&
			json["prices_ok"].b());
	TEST_EXPECT(json["prices_error"].t() == crow::json::type::Null &&
			json["last_reconcile_error"].t() == crow::json::type::Null);
	TEST_EXPECT(str(json["last_reconcile_at"]).size() == 20 && json["pending_events"].i() == 0);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	bool ana_row = false;
	for (const auto &row : json["subscriptions"]) {
		if (str(row["username"]) != "bill_ana" || str(row["item"]) != "tier") continue;
		ana_row = row["tier"].i() == 1 && row["entitled"].b() && str(row["status"]) == "active" &&
				str(row["stripe_customer_id"]).rfind("cus_", 0) == 0 && row["user_id"].t() == crow::json::type::Number &&
				str(row["current_period_end"]).size() == 20;
	}
	TEST_EXPECT(ana_row);
	bool logged = false;
	for (const auto &e : json["recent_events"]) {
		logged = logged || (str(e["event_id"]) == "evt_harness_1" && str(e["outcome"]) == "synced" &&
								   str(e["received_at"]).size() == 20);
	}
	TEST_EXPECT(logged);
	bool listed = false;
	for (const auto &o : json["orphans"]) {
		listed = listed || (str(o["stripe_subscription_id"]) == orphan_sub && str(o["stripe_customer_id"]) == foreign &&
								   str(o["username_hint"]) == "bill_eli" && str(o["status"]) == "active");
	}
	TEST_EXPECT(listed);
	TEST_EXPECT(json["duplicates"].t() == crow::json::type::List && json["duplicates"].size() == 0);
	TEST_EXPECT(json["unknown_prices"].t() == crow::json::type::List && json["unknown_prices"].size() == 0);

	// A subscription of ours selling a price no list names: listed once a
	// reconcile reads it, gone once it is canceled.
	{
		const auto ana = nws::get_user_by_username(*h.pool.acquire(), "bill_ana");
		const auto ana_customer = ana ? billing::customer_of(*h.pool.acquire(), ana->id) : std::nullopt;
		TEST_EXPECT(ana_customer.has_value());
		const std::string unlisted = h.stripe->add_subscription(ana_customer->stripe_customer_id, "price_hunlisted", 1,
				"active", billing::unix_now() + 30 * 86400);
		TEST_EXPECT(h.send("POST", "/api/admin/billing/reconcile", { kBearer }).code == 202);
		billing_pass(h);
		json = crow::json::load(body_text(h.send("GET", "/api/admin/billing", { kBearer })));
		TEST_EXPECT(json && json["unknown_prices"].size() == 1 && str(json["unknown_prices"][0]) == "price_hunlisted");
		{
			std::lock_guard<std::mutex> lock(h.stripe->mu);
			h.stripe->subscriptions[unlisted].status = "canceled";
		}
		TEST_EXPECT(h.send("POST", "/api/admin/billing/reconcile", { kBearer }).code == 202);
		billing_pass(h);
		json = crow::json::load(body_text(h.send("GET", "/api/admin/billing", { kBearer })));
		TEST_EXPECT(json && json["unknown_prices"].size() == 0);
	}
	TEST_EXPECT(!json["limits_hold"].b() && json["limits_hold_reason"].t() == crow::json::type::Null);
	TEST_EXPECT(json["limits_hold_owners"].i() == 0 && json["limits_hold_servers"].i() == 0);

	// The hosted servers' limits hold: shown with the stops it keeps back. A
	// reconcile alone leaves it to the supervisor (it releases after a clean
	// one); the override arms the supervisor's next pass.
	TEST_EXPECT(h.limits_hold->hold("a pass would stop 9 server(s) of 3 of 4 owners", { 11, 12, 13 }, 9, 0) ==
			nws::hosted::LimitsHold::Held::Now);
	json = crow::json::load(body_text(h.send("GET", "/api/admin/billing", { kBearer })));
	TEST_EXPECT(json && json["limits_hold"].b() &&
			str(json["limits_hold_reason"]) == "a pass would stop 9 server(s) of 3 of 4 owners");
	TEST_EXPECT(json["limits_hold_owners"].i() == 3 && json["limits_hold_servers"].i() == 9);
	auto reconcile = [&h](const std::string &body) {
		return h.send("POST", "/api/admin/billing/reconcile", { kBearer, kJson }, body);
	};
	reply = reconcile("");
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json && str(json["reconcile"]) == "requested" && !json["force_limits"].b());
	TEST_EXPECT(h.limits_hold->held() && !h.limits_hold->take_force());
	reply = reconcile("{\"force_limits\":\"yes\"}");
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_field");
	reply = reconcile("not json");
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_json");
	TEST_EXPECT(!h.limits_hold->take_force());
	reply = reconcile("{\"force_limits\":true}");
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json && str(json["reconcile"]) == "requested" && json["force_limits"].b());
	TEST_EXPECT(h.limits_hold->take_force() && h.limits_hold->held());
	TEST_EXPECT(h.limits_hold->clear());
	billing_pass(h);

	// The relink: its checks, then the orphan made Eli's.
	auto link = [&h](const std::string &body) {
		return h.send("POST", "/api/admin/billing/link", { kBearer, kJson }, body);
	};
	TEST_EXPECT(link("{}").code == 400 && error_of_reply(link("{}")) == "missing_field");
	reply = link("{\"user_id\":\"1\",\"stripe_customer_id\":\"cus_x\"}");
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_field");
	reply = link("{\"user_id\":1,\"stripe_customer_id\":\"sub_x\"}");
	TEST_EXPECT(reply.code == 400 && error_of_reply(reply) == "invalid_field");
	reply = link("{\"user_id\":999999,\"stripe_customer_id\":\"" + foreign + "\"}");
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	reply = link("{\"user_id\":" + std::to_string(eli) + ",\"stripe_customer_id\":\"cus_nosuch\"}");
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	reply = link("{\"user_id\":" + std::to_string(eli) + ",\"stripe_customer_id\":\"" + foreign + "\"}");
	json = crow::json::load(body_text(reply));
	if (reply.code != 200) std::fprintf(stderr, "  link: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 200 && json && str(json["outcome"]) == "synced" && json["user_id"].i() == eli);
	{
		std::lock_guard<std::mutex> lock(h.stripe->mu);
		const auto &meta = h.stripe->customers[foreign].metadata;
		TEST_EXPECT(meta.at(billing::kMetaInstance) == h.billing->instance_id() &&
				meta.at(billing::kMetaUserId) == std::to_string(eli));
	}
	json = crow::json::load(body_text(h.send("GET", "/api/me/billing", { with_session(eli_s) })));
	TEST_EXPECT(json && json["max_servers"].i() == 2 && json["subscription"]["tier"].i() == 2);

	// The account delete waits for the subscription's end.
	reply = h.send("DELETE", "/api/admin/users/" + std::to_string(eli), { kBearer });
	TEST_EXPECT(reply.code == 409 && error_of_reply(reply) == "active_subscription");
	TEST_EXPECT(nws::get_user_by_id(*h.pool.acquire(), eli).has_value());
	{
		std::lock_guard<std::mutex> lock(h.stripe->mu);
		h.stripe->subscriptions[orphan_sub].status = "canceled";
	}
	TEST_EXPECT(h.billing->sync().sync_customer(*h.pool.acquire(), foreign, billing::unix_now()) ==
			billing::SyncOutcome::Synced);
	TEST_EXPECT(h.send("DELETE", "/api/admin/users/" + std::to_string(eli), { kBearer }).code == 204);
	// Billing off: the view says so, with the limits hold; a reconcile is 503,
	// an override still goes through.
	reply = h.send_to(h.secure_port, "GET", "/api/admin/billing", { kBearer });
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 200 && json && !json["enabled"].b() && !json["limits_hold"].b() &&
			!json.has("subscriptions"));
	reply = h.send_to(h.secure_port, "POST", "/api/admin/billing/reconcile", { kBearer });
	TEST_EXPECT(reply.code == 503 && error_of_reply(reply) == "billing_disabled");
	reply = h.send_to(h.secure_port, "POST", "/api/admin/billing/reconcile", { kBearer, kJson }, "{\"force_limits\":true}");
	json = crow::json::load(body_text(reply));
	TEST_EXPECT(reply.code == 202 && json && str(json["reconcile"]) == "off" && json["force_limits"].b());
	return 0;
}

// GET /api/admin/users/<id> and the users list's entitlement: admin only; the
// profile, the entitlement in full, the servers and the billing ids (the
// customer and each subscription); 404 for no such player and for a hosted
// server's service account.
int test_admin_user_detail(Harness &h) {
	namespace billing = opennova::novaworld_server::billing;
	const auto ana = nws::get_user_by_username(*h.pool.acquire(), "bill_ana");
	TEST_EXPECT(ana.has_value());
	const std::string path = "/api/admin/users/" + std::to_string(ana->id);
	TEST_EXPECT(h.send("GET", path).code == 401);
	const std::string ana_s = login_token(h, "10.64.0.1", "bill_ana", "pw-bill-ana");
	TEST_EXPECT(h.send("GET", path, { with_session(ana_s) }).code == 403);
	auto reply = h.send("GET", path, { kBearer });
	auto json = crow::json::load(body_text(reply));
	if (reply.code != 200) std::fprintf(stderr, "  user detail: %s\n", describe(reply).c_str());
	TEST_EXPECT(reply.code == 200 && json && str(json["user"]["username"]) == "bill_ana");
	TEST_EXPECT(json["user"]["comp_servers"].i() == 0 && json["user"]["max_servers"].i() == 1);
	TEST_EXPECT(body_text(reply).find("password_hash") == std::string::npos);
	const auto &e = json["entitlement"];
	TEST_EXPECT(e["max_servers"].i() == 1 && str(e["source"]) == "tier" && e["tier"].i() == 1 &&
			e["comp_servers"].i() == 0 && e["tier_servers"].i() == 1 && e["own_machines"].i() == 0 &&
			e["can_start"].b());
	TEST_EXPECT(json["servers"].t() == crow::json::type::List && json["servers"].size() == 0);
	const auto customer = billing::customer_of(*h.pool.acquire(), ana->id);
	TEST_EXPECT(customer && !json["billing"]["livemode"].b() &&
			str(json["billing"]["stripe_customer_id"]) == customer->stripe_customer_id);
	TEST_EXPECT(json["billing"]["subscriptions"].size() >= 1);
	bool tier = false;
	for (const auto &s : json["billing"]["subscriptions"])
		tier = tier || (str(s["item"]) == "tier" && s["tier"].i() == 1 && s["entitled"].b() &&
							   s["quantity"].i() == 1 && str(s["stripe_subscription_id"]).rfind("sub_", 0) == 0);
	TEST_EXPECT(tier);
	TEST_EXPECT(header_value(reply, "Cache-Control") == "no-store");
	// No such player, and a service account: 404.
	TEST_EXPECT(h.send("GET", "/api/admin/users/999999", { kBearer }).code == 404);
	const auto service = nws::upsert_service_account(*h.pool.acquire(), "srv-31337", "pw-detail-service");
	TEST_EXPECT(service.ok);
	reply = h.send("GET", "/api/admin/users/" + std::to_string(service.id), { kBearer });
	TEST_EXPECT(reply.code == 404 && error_of_reply(reply) == "not_found");
	TEST_EXPECT(nws::delete_service_account(*h.pool.acquire(), service.id).ok);
	// A comped account with no billing: its ids null.
	const auto bea = nws::get_user_by_username(*h.pool.acquire(), "bill_bea");
	TEST_EXPECT(bea && nws::set_comp_servers(*h.pool.acquire(), bea->id, 3).ok);
	json = crow::json::load(body_text(h.send("GET", "/api/admin/users/" + std::to_string(bea->id), { kBearer })));
	TEST_EXPECT(json && json["entitlement"]["max_servers"].i() == 3 && str(json["entitlement"]["source"]) == "comp" &&
			json["entitlement"]["tier"].t() == crow::json::type::Null && json["entitlement"]["tier_servers"].i() == 0 &&
			json["billing"]["stripe_customer_id"].t() == crow::json::type::Null &&
			json["billing"]["subscriptions"].size() == 0);

	// The list carries each one's entitlement, live servers and tier status.
	json = crow::json::load(body_text(h.send("GET", "/api/admin/users", { kBearer })));
	bool ana_seen = false;
	bool bea_seen = false;
	for (const auto &u : json["users"]) {
		if (str(u["username"]) == "bill_ana") {
			ana_seen = u["entitlement"]["max_servers"].i() == 1 && str(u["entitlement"]["source"]) == "tier" &&
					u["entitlement"]["tier"].i() == 1 && u["live_servers"].i() == 0 &&
					str(u["subscription_status"]) == "active" && u["max_servers"].i() == 1;
		}
		if (str(u["username"]) == "bill_bea") {
			bea_seen = u["entitlement"]["max_servers"].i() == 3 && str(u["entitlement"]["source"]) == "comp" &&
					u["subscription_status"].t() == crow::json::type::Null && u["comp_servers"].i() == 3;
		}
	}
	TEST_EXPECT(ana_seen && bea_seen);
	TEST_EXPECT(nws::set_comp_servers(*h.pool.acquire(), bea->id, 0).ok);
	// An id past 32 bits names no account (it is never cut down to another's).
	const std::string wide = std::to_string((int64_t{ 1 } << 32) + ana->id);
	reply = h.send("PUT", "/api/admin/users/" + wide, { kBearer, kJson }, "{\"nwh\":\"7\"}");
	TEST_EXPECT(reply.code == 404);
	TEST_EXPECT(h.send("PUT", "/api/admin/users/" + wide + "/game-access", { kBearer, kJson },
						"{\"game_slug\":\"jop_2_consumer\",\"status\":\"active\",\"exp_bits\":\"3\"}")
						.code != 200);
	TEST_EXPECT(h.send("DELETE", "/api/admin/users/" + wide, { kBearer }).code == 404);
	TEST_EXPECT(nws::get_user_by_id(*h.pool.acquire(), ana->id)->nwh == ana->nwh);
	return 0;
}

int run(Harness &h) {
	struct Case { const char *name; int (*fn)(Harness &); };
	const Case cases[] = {
		{ "health", test_health },
		{ "lobbies_lists_seeded_games", test_lobbies_lists_seeded_games },
		{ "register_ok", test_register_ok },
		{ "register_duplicate", test_register_duplicate },
		{ "admin_server_status_requires_token", test_admin_server_status_requires_token },
		{ "admin_users_list", test_admin_users_list },
		{ "login_sets_session_cookie", test_login_sets_session_cookie },
		{ "cookie_secure_flag", test_cookie_secure_flag },
		{ "login_failures_are_generic", test_login_failures_are_generic },
		{ "login_refuses_inactive_accounts", test_login_refuses_inactive_accounts },
		{ "me_requires_live_session", test_me_requires_live_session },
		{ "session_slides", test_session_slides },
		{ "logout_invalidates", test_logout_invalidates },
		{ "admin_routes_by_credential", test_admin_routes_by_credential },
		{ "admin_routes_table", test_admin_routes_table },
		{ "bootstrap_admin", test_bootstrap_admin },
		{ "csrf_before_buckets", test_csrf_before_buckets },
		{ "login_brake", test_login_brake },
		{ "register_brake", test_register_brake },
		{ "nwlogin_brake", test_nwlogin_brake },
		{ "known_addresses", test_known_addresses },
		{ "retail_pin_revocation", test_retail_pin_revocation },
		{ "untrusted_proxy_header_ignored", test_untrusted_proxy_header_ignored },
		{ "nwhost_first_call_mints_hostkey", test_nwhost_first_call_mints_hostkey },
		{ "nwhost_records_owner", test_nwhost_records_owner },
		{ "listed_server_owners", test_listed_server_owners },
		{ "joiner_routes_admin_only", test_joiner_routes_admin_only },
		{ "menu_urls_name_bound_port", test_menu_urls_name_bound_port },
		{ "lobby_per_title", test_lobby_per_title },
		{ "dfx2_client_path", test_dfx2_client_path },
		{ "cd_key_routes", test_cd_key_routes },
		{ "dfx2_activation", test_dfx2_activation },
		{ "unknown_path_404", test_unknown_path_404 },
		{ "server_info", test_server_info },
		{ "healthz", test_healthz },
		{ "admin_host_push", test_admin_host_push },
		{ "owner_host_push", test_owner_host_push },
		{ "hosted_server_routes", test_hosted_server_routes },
		{ "hosted_listing_owner", test_hosted_listing_owner },
		{ "hosted_console_routes", test_hosted_console_routes },
		{ "admin_fleet_routes", test_admin_fleet_routes },
		{ "hosted_stats_routes", test_hosted_stats_routes },
		{ "hosted_usage_route", test_hosted_usage_route },
		{ "no_agent_routes_on_public_listener", test_no_agent_routes_on_public_listener },
		{ "billing_routes", test_billing_routes },
		{ "billing_checkout_guard", test_billing_checkout_guard },
		{ "billing_webhook", test_billing_webhook },
		{ "billing_admin", test_billing_admin },
		{ "admin_user_detail", test_admin_user_detail },
		{ "host_text_utf8", test_host_text_utf8 },
		{ "name_and_value_rules", test_name_and_value_rules },
		{ "template_names_refused", test_template_names_refused },
		{ "retail_templates_render", test_retail_templates_render },
		{ "catch_all_stays_in_root", test_catch_all_stays_in_root },
		{ "header_values_cannot_split", test_header_values_cannot_split },
		{ "reset_during_login", test_reset_during_login },
		{ "join_identity", test_join_identity },
		{ "api_proxy_only", test_api_proxy_only },
		{ "template_values_escaped", test_template_values_escaped },
		{ "register_handle_rule", test_register_handle_rule },
		{ "login_row_kept_alive", test_login_row_kept_alive },
		{ "host_tag_serves_once", test_host_tag_serves_once },
		{ "db_failure_reply_carries_code_alone", test_db_failure_reply_carries_code_alone },
		{ "concurrent_requests", test_concurrent_requests },
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

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
		// Declared first, so they outlive the pool and the listener.
		test_temp::TempDir temp("http_routes");
		// Migrated, then the games seed alone: no dev users, so the players
		// table starts empty.
		test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
		// The server's own lock timeout (kDefaultLockTimeoutMs), unlike
		// db_concurrency_test's stress margin: a write that waits on a lock past
		// it fails here as it would in production.
		ConnectionPool pool(database.options());

		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.admin_api_token = kAdminToken;
		// Port 0: Crow binds the port the OS picks, and start() returns once it
		// serves there, with that port in bound_port().
		config.http_port = 0;
		// Web roots that do not exist and a templates root holding only the URL
		// page and the retail message page (copied below), so the catch-all
		// family 404s deterministically.
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates";
		config.static_dir = temp.path / "static";
		std::filesystem::create_directories(config.templates_dir);
		const std::string urls_page = "{{HOST_URL}}|{{GSB_SERVER}}";
		TEST_EXPECT(test_io::write_file((config.templates_dir / kUrlsTemplate).string(),
		                                std::vector<uint8_t>(urls_page.begin(), urls_page.end())));
		// The retail message page POST /NWLogin.dll's failures render, and the
		// relay and HOSTKEY pages /NWHost.dll renders.
		for (const char *page : {"jop_2_msg.htm", "jop_2_relay.htm", "jop_2_host2.htm"}) {
			std::filesystem::copy_file(source_dir / "apps" / "novaworld_server" / "templates" / page,
			                           config.templates_dir / page);
		}

		// The harness talks to the listener from 127.0.0.1 and names each
		// case's client address in X-Real-IP, the way nginx forwards it; its
		// /api/ answers that proxy alone, as prod's does.
		config.trusted_proxies = {"127.0.0.1"};
		config.api_proxy_only = true;
		// The hosted servers' routes run (no supervisor: rows stay where the routes
		// put them), with a cap of 4 and two data sets, each with a default
		// rotation: Joint Operations and Delta Force Xtreme 2 (the local
		// provider's folders, never read here). OpenNova's names no folder, so
		// the local provider does not offer it.
		config.hosted_provider = "local";
		config.hosted_cap = 4;
		config.hosted_data_sets =
				"{\"dfx2_consumer\":{\"missions\":[\"dfx2_a.bms\",\"dfx2_b.bms\"],\"resource_dir\":\"unused-dfx2\"},"
				"\"jop_2_consumer\":{\"missions\":[\"default.bms\"],\"resource_dir\":\"unused-jo\"},"
				"\"opennova_0_consumer\":{\"missions\":[\"on.bms\"]}}";

		opennova::ConnectionManager manager;
		// The push routes' channel, as main() wires it: a real NW UDP listener on a port the
		// OS picks, over the same pool, so a hosting connection's row (and its owner, which an
		// owner's push is checked against) lands in active_hosts. The host cases end their
		// listings, so they leave active_hosts as the other cases expect it.
		nws::NwUdpListener nw_udp(manager);
		manager.on_lost([&nw_udp](const opennova::Connection &c, opennova::DropReason r) {
			nw_udp.erase_lobby_state(c.addr, opennova::drop_reason_name(r));
		});
		nw_udp.set_db_pool(&pool);
		nws::ServerConfig udp_config;
		udp_config.nw_udp_port = 0;
		TEST_EXPECT(nw_udp.start(udp_config));
		nws::SessionStore sessions;
		// The console routes' admin links (the console case syncs them by hand),
		// declared before the listener so they outlive it.
		opennova::novaworld_server::hosted::AdminLinks links(&pool);
		// GET /healthz's watchdog on a hand-stepped clock, watching one loop whose
		// counter the healthz case moves; declared before the listener so it
		// outlives it.
		nws::Watchdog watchdog(nws::Watchdog::kStallMs, [] { return g_watch_clock_ms.load(); });
		watchdog.watch("harness loop", [] { return g_watch_turns.load(); });
		// Billing over the in-memory Stripe, its worker's passes the cases' own (no
		// thread): the first one here checks the prices and reconciles (nothing yet).
		billing_fake::FakeStripeApi stripe;
		stripe.now = nws::billing::unix_now(); // the periods Checkout starts are real ones
		stripe.add_price(harness_price("price_htier1", 500));
		stripe.add_price(harness_price("price_htier2", 1000));
		stripe.add_price(harness_price("price_htier3", 1500));
		stripe.add_price(harness_price("price_hown", 4500));
		nws::billing::BillingConfig billing_config;
		billing_config.secret_key = "sk_test_harnesskey";
		billing_config.webhook_secret = kWebhookSecret;
		billing_config.site_url = kSiteUrl;
		billing_config.tier_prices[1] = { "price_htier1" };
		billing_config.tier_prices[2] = { "price_htier2" };
		billing_config.tier_prices[3] = { "price_htier3" };
		billing_config.own_machine_prices = { "price_hown" };
		TEST_EXPECT(nws::billing::check_billing_config(billing_config).empty());
		nws::billing::Billing billing_service(billing_config, stripe, pool, nws::billing::instance_id(*pool.acquire()));
		billing_service.sync().pass(nws::billing::unix_now());
		TEST_EXPECT(billing_service.sync().status().prices_ok);
		nws::hosted::LimitsHold limits_hold;
		// The first listener's mail: a real outbox over a mailer that records,
		// declared before the listener so it outlives it, and the site URL its
		// links name.
		fake_mail::FakeMailer mailer;
		nws::MailOutbox outbox(mailer);
		config.site_url = kMailSiteUrl;
		nws::HttpListener http(manager, pool, sessions);
		http.set_mail_outbox(&outbox);
		http.set_nw_udp_listener(&nw_udp);
		http.set_admin_links(&links);
		http.set_watchdog(&watchdog);
		http.set_billing(&billing_service);
		http.set_limits_hold(&limits_hold);
		// The usage view's slot limits: 256 MiB and 2 CPUs a container.
		http.set_fleet_slots(256, 2.0);
		TEST_EXPECT(http.start(config));
		TEST_EXPECT(http.bound_port() != 0);

		// A secret file beside every root of both listeners, which no reply may
		// carry. The second listener's roots (under retail/) are copies of the
		// retail templates and static files and a small web root, each holding
		// a symlink to the secret; its templates also hold a hidden file with it.
		const std::string secret = kSecret;
		const std::vector<uint8_t> secret_bytes(secret.begin(), secret.end());
		const std::filesystem::path retail_root = temp.path / "retail";
		const std::filesystem::path apps_dir = source_dir / "apps" / "novaworld_server";
		std::filesystem::create_directories(retail_root / "web" / "assets");
		std::filesystem::copy(apps_dir / "templates", retail_root / "templates",
				std::filesystem::copy_options::recursive);
		std::filesystem::copy(apps_dir / "static", retail_root / "static",
				std::filesystem::copy_options::recursive);
		const std::string app_js = "console.log(1);";
		TEST_EXPECT(test_io::write_file((retail_root / "web" / "assets" / "app.js").string(),
				std::vector<uint8_t>(app_js.begin(), app_js.end())));
		for (const auto &file : { temp.path / "secret.txt", retail_root / "secret.txt",
					 retail_root / "templates" / ".hidden.htm" }) {
			TEST_EXPECT(test_io::write_file(file.string(), secret_bytes));
		}
		for (const auto &link : { retail_root / "templates" / "leak.htm",
					 retail_root / "static" / "leak.tga", retail_root / "web" / "webleak.txt" }) {
			std::error_code ec;
			std::filesystem::create_symlink(retail_root / "secret.txt", link, ec);
			if (ec)
				std::fprintf(
						stderr, "  symlink %s: %s\n", link.string().c_str(), ec.message().c_str());
			TEST_EXPECT(!ec);
		}

		// The second listener: the Secure cookie (an https site) and no
		// trusted proxy, over the retail roots.
		nws::ServerConfig secure_config = config;
		secure_config.cookie_secure = true;
		secure_config.trusted_proxies.clear();
		secure_config.api_proxy_only = false;
		secure_config.hosted_provider = "none"; // hosting off here (the hosted routes case)
		secure_config.templates_dir = retail_root / "templates";
		secure_config.static_dir = retail_root / "static";
		secure_config.web_dist_dir = retail_root / "web";
		nws::HttpListener secure_http(manager, pool, sessions);
		TEST_EXPECT(secure_http.start(secure_config));
		TEST_EXPECT(secure_http.bound_port() != 0);

		// The third: the retail roots, /api/ for a proxy the harness is not.
		nws::ServerConfig fresh_config = secure_config;
		fresh_config.cookie_secure = false;
		fresh_config.trusted_proxies = { "10.255.0.1" };
		fresh_config.api_proxy_only = true;
		nws::HttpListener fresh_http(manager, pool, sessions);
		TEST_EXPECT(fresh_http.start(fresh_config));
		TEST_EXPECT(fresh_http.bound_port() != 0);

		Harness harness{pool, sessions, nw_udp, http.bound_port(), secure_http.bound_port(), {}};
		harness.temp_root = temp.path;
		harness.retail_root = retail_root;
		harness.fresh_port = fresh_http.bound_port();
		harness.links = &links;
		harness.watchdog = &watchdog;
		harness.stripe = &stripe;
		harness.billing = &billing_service;
		harness.limits_hold = &limits_hold;
		harness.mailer = &mailer;
		harness.outbox = &outbox;
		rc = run(harness);
		// Join the Crow threads before the pool and the TempDir go, then the
		// NW UDP listener the push routes held.
		fresh_http.stop();
		secure_http.stop();
		http.stop();
		links.stop_all();
		nw_udp.stop();
	}
	net::shutdown();
	if (rc == 0) std::printf("OK: http routes\n");
	return rc;
}
