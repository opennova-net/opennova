// The NovaWorld service's HTTP routes, driven in-process: a SQLite file in the
// run's temp directory under backend/migrations (plus the games seed), the
// Crow HttpListener from opennova_novaworld_server_core on port 0 (the OS
// picks; the harness reads the port back from bound_port()), and requests sent
// through apps/common's http_exchange to 127.0.0.1. Built only
// with BUILD_NOVAWORLD_HTTP. The cases share one listener and run in order
// (the admin users case reads the account the register case made; the
// concurrent case runs last, since it adds accounts and a host).
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
#include "nw_udp_listener.h"
#include "server_config.h"
#include "session_store.h"

#include <net/napi/session.h>
#include <net/novaworld/client_session.h>
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
	nws::NwUdpListener &nw_udp;  // the push routes' channel, a real listener on loopback
	uint16_t port = 0;
	std::string registered_pcid; // what the register case's reply carried

	// Every connect, send and recv waits at most `timeout_ms`.
	net::HttpReply send(const char *method, const std::string &path,
	                    const std::vector<std::string> &headers = {},
	                    const std::string &body = {}, int timeout_ms = 3000) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
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
			const auto reply = h.send("POST", "/api/register", {kJson},
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
	            static_cast<std::size_t>(1 + kConcurrentRegistrations));
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

// A host's NovaWorld session on loopback against the harness's NwUdpListener: a ClientSession
// verified and hosting, pumped by the test while it waits for what the service pushes.
class PushHost {
public:
	explicit PushHost(uint16_t service_port) : session_(config()) {
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

private:
	static opennova::ClientSession::Config config() {
		opennova::ClientSession::Config cfg;
		cfg.client_index = 0x48545450u;
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

// POST /api/admin/hosts/<rid>/command and /stop: Bearer-gated; a body the composer refuses is a
// 400 with its reason; a RID no connection holds is a 404; a hosting connection's RID is a 202,
// and the statement reaches that host's session; a stopped host's RID is a 409.
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
		{"nwhost_first_call_mints_hostkey", test_nwhost_first_call_mints_hostkey},
		{"menu_urls_name_bound_port", test_menu_urls_name_bound_port},
		{"unknown_path_404", test_unknown_path_404},
		{"admin_host_push", test_admin_host_push},
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

		opennova::ConnectionManager manager;
		// The push routes' channel, as main() wires it: a real NW UDP listener on a port the
		// OS picks, with no DB pool (the hosted row lives in its memory only, so the host
		// cases leave active_hosts as the other cases expect it).
		nws::NwUdpListener nw_udp(manager);
		manager.on_lost([&nw_udp](const opennova::Connection &c, opennova::DropReason r) {
			nw_udp.erase_lobby_state(c.addr, opennova::drop_reason_name(r));
		});
		nws::ServerConfig udp_config;
		udp_config.nw_udp_port = 0;
		TEST_EXPECT(nw_udp.start(udp_config));
		nws::SessionStore sessions;
		nws::HttpListener http(manager, pool, sessions);
		http.set_nw_udp_listener(&nw_udp);
		TEST_EXPECT(http.start(config));
		TEST_EXPECT(http.bound_port() != 0);

		Harness harness{pool, sessions, nw_udp, http.bound_port(), {}};
		rc = run(harness);
		http.stop(); // joins the Crow thread before the pool and the TempDir go
		nw_udp.stop();
	}
	net::shutdown();
	if (rc == 0) std::printf("OK: http routes\n");
	return rc;
}
