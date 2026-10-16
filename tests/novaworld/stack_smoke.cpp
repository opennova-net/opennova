// opennova-novaworld-stack-smoke: the local stack's smoke test (deploy/compose,
// scripts/dev_stack.sh smoke), run against a running stack by its addresses, from a
// container on the stack's network (its image is apps/novaworld_common/Dockerfile's
// `smoke` target). Not a ctest: it needs the stack. Each check prints `ok` or
// `FAIL` and why; the exit code is the number of failures (0: all passed).
//
//   run    every program's liveness (GET /livez on each --livez address, web's on its
//          HTTP port) and web's readiness (GET /healthz); an account registered and
//          logged in through the site's API (the portal's nginx, --site, as a
//          browser reaches it: ONNET_API_PROXY_ONLY admits /api from it alone) and
//          GET /api/me; the gate's answer to a retail client's probe naming a live
//          lobby (the engine's own probe and reply codecs), and a NovaWorld session
//          verified on that lobby (the engine's ClientSession); the jobs admin API
//          (GET /api/admin/jobs/overview, the admin token). With --hosting <data
//          set>: the admin grants the account a server, the account starts one
//          (POST /api/me/servers) on a machine the worker launches, which reaches
//          running and is listed in the lobby (its listing's RID), takes console
//          commands through the controller's admin link (PLAYER LIST, CHAT SEND),
//          and stops.
//   data-set <dir>
//          writes the synthetic Joint Operations data set the stack's machines can
//          serve without licensed data (the serve tests' deathmatch map
//          SERVETST.BMS and game text, a loose folder): what `dev_stack.sh up
//          --synthetic-data` and CI host on.
//
// Infrastructure only, POSIX only.

#include "lobby_client.h"
#include "serve/serve_test_support.h"

#include "net_http.h"
#include "net_sockets.h"
#include "web_session.h"

#include <base/io/json.h>
#include <base/os_random/os_random.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace net = opennova::net;
namespace io = opennova::io;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void ok(const std::string &what) {
	std::printf("ok    %s\n", what.c_str());
	std::fflush(stdout);
}

void note(const std::string &what) {
	std::printf("      %s\n", what.c_str());
	std::fflush(stdout);
}

bool check(bool passed, const std::string &what, const std::string &why = {}) {
	if (passed) {
		ok(what);
		return true;
	}
	++g_failures;
	std::printf("FAIL  %s%s%s\n", what.c_str(), why.empty() ? "" : ": ", why.c_str());
	std::fflush(stdout);
	return false;
}

std::string body_text(const net::HttpReply &reply) {
	std::string text(reply.body.begin(), reply.body.end());
	return text.size() > 300 ? text.substr(0, 300) + "..." : text;
}

std::string status_text(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "no answer (" + reply.error + ")";
	return "HTTP " + std::to_string(reply.code) + " " + body_text(reply);
}

// A "host:port" endpoint resolved now (a compose service's name, or an address).
std::optional<net::Endpoint> endpoint_of(const std::string &text) {
	const size_t colon = text.rfind(':');
	if (colon == std::string::npos)
		return std::nullopt;
	net::Endpoint out;
	if (!net::resolve_ipv4(text.substr(0, colon), out))
		return std::nullopt;
	const long port = std::strtol(text.c_str() + colon + 1, nullptr, 10);
	if (port <= 0 || port > 65535)
		return std::nullopt;
	out.port = static_cast<uint16_t>(port);
	return out;
}

net::HttpReply http(const std::string &method, const std::string &base, const std::string &path,
		const std::vector<std::string> &headers = {}, const std::string &body = {}) {
	return net::http_exchange(method, "http://" + base + path, headers, body, 10000,
			[](const std::string &host, net::Endpoint &out) {
				return net::resolve_ipv4(host, out);
			});
}

std::optional<io::JsonValue> json_of(const net::HttpReply &reply) {
	io::JsonValue v;
	std::string error;
	if (!io::json_parse(std::string(reply.body.begin(), reply.body.end()), v, error))
		return std::nullopt;
	return v;
}

// The member at `path` (object keys) of `v`, or null.
const io::JsonValue *at(const io::JsonValue &v, std::initializer_list<const char *> path) {
	const io::JsonValue *cur = &v;
	for (const char *key : path) {
		if (!cur->is_object())
			return nullptr;
		cur = cur->get(key);
		if (!cur)
			return nullptr;
	}
	return cur;
}

std::string text_at(const io::JsonValue &v, std::initializer_list<const char *> path) {
	const io::JsonValue *m = at(v, path);
	return m && m->is_string() ? m->string : std::string();
}

int64_t number_at(const io::JsonValue &v, std::initializer_list<const char *> path) {
	const io::JsonValue *m = at(v, path);
	return m && m->is_number() ? static_cast<int64_t>(m->number) : 0;
}

template <typename Done> bool wait_for(Done done, int seconds) {
	const auto deadline = Clock::now() + std::chrono::seconds(seconds);
	while (!done()) {
		if (Clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	return true;
}

std::string random_hex(int bytes) {
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out;
	for (int i = 0; i < bytes; ++i) {
		const uint32_t r = opennova::os_random_u32();
		out += kHex[(r >> 4) & 0xFu];
		out += kHex[r & 0xFu];
	}
	return out;
}

struct Options {
	std::string web = "novaworld-web:8080"; // web's own port (/livez, /healthz)
	std::string site = "novaworld-web:80"; // the portal (nginx), /api
	std::string gate = "novaworld-gate:7597";
	std::vector<std::string> livez; // host:port of each other program's health listener
	std::string admin_token;
	std::string hosting; // a data set to host on, "" for none
	std::string mission; // the start's rotation; "" for the data set's default
	int timeout_s = 300;
};

// The site's API through the portal, as a browser calls it.
struct Site {
	std::string base;
	std::string session; // the session cookie's value
	std::string admin_token;

	net::HttpReply user(const std::string &method, const std::string &path,
			const std::string &body = {}) const {
		std::vector<std::string> headers = {
			"Cookie: " + std::string(opennova::novaworld_server::kWebSessionCookie) + "=" + session
		};
		if (method != "GET") {
			headers.push_back("Content-Type: application/json");
			headers.push_back("X-OpenNova-Request: 1");
		}
		return http(method, base, path, headers, body);
	}

	net::HttpReply admin(const std::string &method, const std::string &path,
			const std::string &body = {}) const {
		std::vector<std::string> headers = { "Authorization: Bearer " + admin_token };
		if (method != "GET")
			headers.push_back("Content-Type: application/json");
		return http(method, base, path, headers, body);
	}
};

// The session cookie a login's reply sets, "" for none.
std::string session_cookie(const net::HttpReply &reply) {
	const std::string prefix = std::string(opennova::novaworld_server::kWebSessionCookie) + "=";
	for (const std::string &line : reply.headers) {
		if (line.size() < 11)
			continue;
		std::string name = line.substr(0, 11);
		for (char &c : name)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (name != "set-cookie:")
			continue;
		const size_t start = line.find(prefix);
		if (start == std::string::npos)
			continue;
		const size_t value = start + prefix.size();
		return line.substr(value, line.find(';', value) - value);
	}
	return {};
}

void check_hosting(const Site &site, int64_t user_id, const Options &o) {
	auto reply = site.admin("PUT", "/api/admin/users/" + std::to_string(user_id) + "/comp-servers",
			"{\"comp_servers\":1}");
	if (!check(reply.code == 200, "the admin grants the account a hosted server (comp grant)",
				status_text(reply)))
		return;
	reply = site.user("GET", "/api/me/servers");
	const auto list = json_of(reply);
	const io::JsonValue *enabled = list ? at(*list, { "enabled" }) : nullptr;
	if (!check(reply.code == 200 && enabled && enabled->is_bool() && enabled->boolean,
				"hosting is on (GET /api/me/servers: enabled)", status_text(reply)))
		return;
	std::string missions;
	if (!o.mission.empty())
		missions = ",\"missions\":[\"" + o.mission + "\"]";
	const std::string name = "Smoke " + random_hex(3);
	reply = site.user("POST", "/api/me/servers",
			"{\"game\":\"" + o.hosting + "\",\"game_name\":\"" + name + "\",\"max_players\":8" +
					missions + "}");
	const auto started = json_of(reply);
	const int64_t id = reply.code == 202 && started ? number_at(*started, { "server", "id" }) : 0;
	if (!check(id > 0, "a hosted server starts (POST /api/me/servers: 202)", status_text(reply)))
		return;
	const std::string path = "/api/me/servers/" + std::to_string(id);
	std::string state;
	int64_t rid = 0;
	std::string listed_at;
	std::string why;
	const auto read = [&] {
		const auto r = site.user("GET", path);
		const auto v = json_of(r);
		if (r.code != 200 || !v) {
			why = status_text(r);
			return false;
		}
		const std::string now = text_at(*v, { "server", "state" });
		if (now != state)
			note("server " + std::to_string(id) + ": " + now);
		state = now;
		rid = number_at(*v, { "server", "listing", "rid" });
		listed_at = text_at(*v, { "server", "listing", "host_ip" }) + ":" +
				std::to_string(number_at(*v, { "server", "listing", "host_port" }));
		why = "state " + state + ", " + text_at(*v, { "server", "failure_reason" });
		return state == "running" || state == "failed" || state == "stopped";
	};
	wait_for(read, o.timeout_s);
	if (!check(state == "running", "it reaches running on a machine the worker launched", why))
		return;
	const bool listed = rid > 0 ||
			wait_for(
					[&] {
						read();
						return rid > 0;
					},
					30);
	check(listed, "it is listed in the lobby (its listing's RID " + std::to_string(rid) + ")", why);
	if (listed)
		note("its listing names " + listed_at);
	// The console: commands through the controller's admin link to the server.
	std::string console;
	const bool players = wait_for(
			[&] {
				const auto r = site.user("GET", path + "/players");
				console = status_text(r);
				return r.code == 200;
			},
			60);
	check(players, "a console read reaches it (PLAYER LIST)", console);
	reply = site.user("POST", path + "/chat", "{\"text\":\"hello from the smoke test\"}");
	const auto chat = json_of(reply);
	check(reply.code == 200 && chat && text_at(*chat, { "result" }) == "sent",
			"a console command reaches it (CHAT SEND)", status_text(reply));
	reply = site.user("POST", path + "/stop");
	check(reply.code == 202, "it stops (POST /api/me/servers/<id>/stop)", status_text(reply));
	check(wait_for(
				  [&] {
					  read();
					  return state == "stopped";
				  },
				  120),
			"it ends stopped", why);
}

int run(const Options &o) {
	// Each program's liveness and web's readiness.
	for (const std::string &target : o.livez) {
		const auto r = http("GET", target, "/livez");
		check(r.code == 200 && body_text(r) == "ok", "GET /livez on " + target, status_text(r));
	}
	auto reply = http("GET", o.web, "/livez");
	check(reply.code == 200, "GET /livez on web (" + o.web + ")", status_text(reply));
	reply = http("GET", o.web, "/healthz");
	check(reply.code == 200, "GET /healthz on web (its database and Redis)", status_text(reply));

	// An account through the site's API: registered, logged in, its session read.
	Site site{ o.site, {}, o.admin_token };
	const std::string username = "smoke_" + random_hex(4);
	const std::string password = "pw-" + random_hex(8);
	reply = http("POST", o.site, "/api/register",
			{ "Content-Type: application/json", "X-OpenNova-Request: 1" },
			"{\"username\":\"" + username + "\",\"password\":\"" + password + "\",\"email\":\"" +
					username + "@example.com\"}");
	check(reply.code == 201, "an account registers (POST /api/register through the portal)",
			status_text(reply));
	reply = http("POST", o.site, "/api/login",
			{ "Content-Type: application/json", "X-OpenNova-Request: 1" },
			"{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}");
	site.session = session_cookie(reply);
	check(reply.code == 200 && !site.session.empty(), "it logs in (POST /api/login: a session)",
			status_text(reply));
	reply = site.user("GET", "/api/me");
	const auto me = json_of(reply);
	const int64_t user_id = me ? number_at(*me, { "id" }) : 0;
	check(reply.code == 200 && me && text_at(*me, { "username" }) == username && user_id > 0,
			"GET /api/me names it", status_text(reply));

	// The gate answers a retail client's probe with a live lobby, and a session
	// verifies on that lobby.
	const auto gate = endpoint_of(o.gate);
	const std::string lobby = gate ? lobby_client::gate_hands_out(*gate) : std::string();
	check(!lobby.empty(), "the gate (" + o.gate + ") answers a retail probe with a live lobby",
			gate ? "no reply naming a lobby" : "its address does not resolve");
	if (!lobby.empty()) {
		note("the gate hands out " + lobby);
		const auto endpoint = endpoint_of(lobby);
		bool verified = false;
		if (endpoint) {
			lobby_client::LobbyClient client("smoke:lobby", 0x534d4f4bu, 0x4c4f4259u, *endpoint);
			client.start();
			verified = wait_for([&] { return client.verified(); }, 15);
			client.stop();
		}
		check(verified, "a NovaWorld session verifies on that lobby",
				endpoint ? "no verified session in 15 s" : "the lobby's address does not resolve");
	}

	// The jobs admin API.
	reply = site.admin("GET", "/api/admin/jobs/overview");
	check(reply.code == 200 && json_of(reply).has_value(),
			"the jobs admin API answers (GET /api/admin/jobs/overview)", status_text(reply));

	if (!o.hosting.empty() && user_id > 0)
		check_hosting(site, user_id, o);
	else if (o.hosting.empty())
		note("hosting not checked: no data set (dev_stack.sh up --data <dir> or --synthetic-data)");
	std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
	return g_failures;
}

// The synthetic data set: a loose Joint Operations folder of one deathmatch map
// (servetst.bms) and the game text a server's listing reads.
int write_data_set(const fs::path &dir) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	const bool written = serve_test::write_bytes(dir / "SERVETST.BMS",
								 serve_test::deathmatch_mission("Serve Test Map")) &&
			serve_test::write_bytes(dir / "gametext.bin",
					serve_test::gametext({
							{ "NovaWorld",
									{ { "STRNOVA07", "Jungle" }, { "STRNOVA08", "Desert" },
											{ "STRNOVA09", "Snow" }, { "STRNOVA10", "None" },
											{ "STRNOVA11", "Yes" }, { "STRNOVA12", "No" } } },
							{ "GateTypeAbbrev", { { "DM", "DM" } } },
					}));
	if (!written) {
		std::fprintf(stderr, "cannot write the data set to %s\n", dir.string().c_str());
		return 1;
	}
	std::printf(
			"the synthetic data set (SERVETST.BMS, gametext.bin) is in %s\n", dir.string().c_str());
	return 0;
}

int usage() {
	std::fprintf(stderr,
			"usage: opennova-novaworld-stack-smoke run [--web <host:port>] [--site <host:port>]\n"
			"           [--gate <host:port>] [--livez <host:port>]... --admin-token <token>\n"
			"           [--hosting <data set> [--mission <name.bms>]] [--timeout <seconds>]\n"
			"       opennova-novaworld-stack-smoke data-set <dir>\n");
	return 2;
}

} // namespace

int main(int argc, char **argv) {
	if (argc >= 3 && std::string(argv[1]) == "data-set")
		return write_data_set(argv[2]);
	if (argc < 2 || std::string(argv[1]) != "run")
		return usage();
	Options o;
	for (int i = 2; i < argc; ++i) {
		const std::string arg = argv[i];
		if (i + 1 >= argc)
			return usage();
		const std::string value = argv[++i];
		if (arg == "--web")
			o.web = value;
		else if (arg == "--site")
			o.site = value;
		else if (arg == "--gate")
			o.gate = value;
		else if (arg == "--livez")
			o.livez.push_back(value);
		else if (arg == "--admin-token")
			o.admin_token = value;
		else if (arg == "--hosting")
			o.hosting = value;
		else if (arg == "--mission")
			o.mission = value;
		else if (arg == "--timeout")
			o.timeout_s = std::atoi(value.c_str());
		else
			return usage();
	}
	if (o.admin_token.empty() || o.timeout_s <= 0)
		return usage();
	if (net::startup() != 0) {
		std::fprintf(stderr, "the socket layer did not start\n");
		return 1;
	}
	const int failures = run(o);
	net::shutdown();
	return failures;
}
