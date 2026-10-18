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
//          set id> (as GET /api/me/servers offers it, once its tree is ready): the
//          admin grants the account a server, the account starts one on that data
//          set (POST /api/me/servers, with --mission's rotation or the data's
//          whole one) on a machine the worker launches (its machine.launch job
//          done by a worker process, never the fleet or the data program), which
//          reaches running and is listed in the lobby (its listing's RID), takes
//          console commands
//          through the fleet's admin link (PLAYER LIST, CHAT SEND), shows on
//          the admin's pages (its machine's page with an agent and a ready
//          tree, its log holding "serving on UDP", usage samples of it, its
//          machine and every program), and stops. Then the static addresses
//          (check_static_address), with --zone-file the zone the worker
//          writes. Last, the attention list holds no danger (and reads no
//          alarm: the local stack has none).
//   data-set <dir>
//          writes the synthetic Joint Operations data set the stack's machines can
//          serve without licensed data: the serve tests' deathmatch map
//          SERVETST.BMS and game text packed into the three base archives a
//          data set's tree holds (resource.pff, localres.pff, language.pff:
//          apps/novaworld_common/data_sets/data_set_files.h), with a version.txt,
//          as an install's root. What `dev_stack.sh up --synthetic-data` uploads
//          and CI hosts on.
//
// Infrastructure only, POSIX only.

#include "lobby_client.h"
#include "serve/serve_test_support.h"

#include "accounts/web_session.h"
#include "net_http.h"
#include "net_sockets.h"

#include <base/io/json.h>
#include <base/os_random/os_random.h>
#include <formats/pff/pff.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
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
	std::string hosting; // the id of a data set to host on, "" for none
	std::string mission; // the start's rotation; "" for the data's whole one
	std::string zone_file; // the static addresses' zone file the worker writes, "" for none
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

// A running server as the admin's pages read it: its machine's page (an agent,
// a ready tree), its log (the line opennova-serve writes once it serves, read
// through the log store), and the usage samples of it, its machine and every
// program (each written once a minute).
void check_admin_views(const Site &site, int64_t server_id) {
	auto reply = site.admin("GET", "/api/admin/hosted/" + std::to_string(server_id));
	const auto view = json_of(reply);
	const int64_t machine = reply.code == 200 && view ? number_at(*view, { "machine", "id" }) : 0;
	if (!check(machine > 0, "the admin's server view names its machine", status_text(reply)))
		return;
	std::string why;
	const std::string machine_path = "/api/admin/fleet/machines/" + std::to_string(machine);
	const bool page = wait_for(
			[&] {
				const auto r = site.admin("GET", machine_path);
				const auto m = json_of(r);
				why = status_text(r);
				if (r.code != 200 || !m)
					return false;
				bool ready = false;
				if (const io::JsonValue *trees = at(*m, { "trees" }); trees && trees->is_array())
					for (const io::JsonValue &t : trees->array)
						ready = ready || text_at(t, { "state" }) == "ready";
				return ready && !text_at(*m, { "machine", "agent_version" }).empty();
			},
			60);
	check(page, "its machine's page (GET " + machine_path + ") has an agent and a ready tree", why);
	const std::string logs = "/api/admin/logs?subject=server&id=" + std::to_string(server_id) +
			"&q=serving%20on%20UDP";
	const bool serving = wait_for(
			[&] {
				const auto r = site.admin("GET", logs);
				const auto l = json_of(r);
				why = status_text(r);
				const io::JsonValue *lines = r.code == 200 && l ? at(*l, { "lines" }) : nullptr;
				if (!lines || !lines->is_array())
					return false;
				for (const io::JsonValue &line : lines->array)
					if (text_at(line, { "text" }).find("serving on UDP") != std::string::npos)
						return true;
				return false;
			},
			60);
	check(serving, "its log (GET /api/admin/logs) holds \"serving on UDP\"", why);
	const auto sampled = [&](const std::string &subject, const std::string &id) {
		const std::string path =
				"/api/admin/usage?subject=" + subject + "&id=" + id + "&range=48h";
		const bool any = wait_for(
				[&] {
					const auto r = site.admin("GET", path);
					const auto u = json_of(r);
					why = status_text(r);
					const io::JsonValue *series =
							r.code == 200 && u ? at(*u, { "series" }) : nullptr;
					return series && series->is_array() && !series->array.empty();
				},
				180);
		check(any, "usage samples exist for " + subject + " " + id + " (GET /api/admin/usage)",
				why);
	};
	sampled("server", std::to_string(server_id));
	sampled("machine", std::to_string(machine));
	for (const char *program : { "gate", "lobby", "web", "fleet", "worker", "data" })
		sampled("program", program);
}

// What needs an admin, once the rest ran: no danger finding, and no alarm read
// (the local stack has none). Each finding is noted.
void check_attention(const Site &site) {
	const auto reply = site.admin("GET", "/api/admin/attention");
	const auto json = json_of(reply);
	const io::JsonValue *findings = reply.code == 200 && json ? at(*json, { "findings" }) : nullptr;
	const io::JsonValue *alarms_read = json ? at(*json, { "alarms_read" }) : nullptr;
	if (!check(findings && findings->is_array() && alarms_read && alarms_read->is_bool() &&
					   !alarms_read->boolean,
				"the attention list answers, reading no alarm here (GET /api/admin/attention)",
				status_text(reply)))
		return;
	std::string dangers;
	for (const io::JsonValue &f : findings->array) {
		const std::string line = text_at(f, { "level" }) + " " + text_at(f, { "subject" }) + ": " +
				text_at(f, { "text" });
		note("attention: " + line);
		if (text_at(f, { "level" }) == "danger")
			dangers += (dangers.empty() ? "" : "; ") + line;
	}
	check(dangers.empty(), "the attention list holds no danger", dangers);
}

void check_hosting(const Site &site, int64_t user_id, const Options &o) {
	auto reply = site.admin("PUT", "/api/admin/users/" + std::to_string(user_id) + "/comp",
			"{\"servers\":1,\"own_machines\":0}");
	if (!check(reply.code == 200, "the admin grants the account a hosted server (comp grant)",
				status_text(reply)))
		return;
	reply = site.user("GET", "/api/me/servers");
	const auto list = json_of(reply);
	const io::JsonValue *enabled = list ? at(*list, { "enabled" }) : nullptr;
	if (!check(reply.code == 200 && enabled && enabled->is_bool() && enabled->boolean,
				"hosting is on (GET /api/me/servers: enabled)", status_text(reply)))
		return;
	// The data set --hosting names, among the ones the start may pick (a base
	// with a ready tree): its id, and the missions its data holds.
	const int64_t wanted = std::strtoll(o.hosting.c_str(), nullptr, 10);
	const io::JsonValue *data_sets = at(*list, { "data_sets" });
	const io::JsonValue *offered = nullptr;
	std::string offered_text;
	if (data_sets && data_sets->is_array()) {
		for (const io::JsonValue &d : data_sets->array) {
			const int64_t id = number_at(d, { "id" });
			offered_text += (offered_text.empty() ? "" : ", ") + std::to_string(id) + " " +
					text_at(d, { "display_name" });
			if (id == wanted)
				offered = &d;
		}
	}
	if (!check(offered != nullptr, "data set " + o.hosting + " is offered (its tree is ready)",
				"offered: " + (offered_text.empty() ? std::string("none") : offered_text)))
		return;
	const io::JsonValue *mission_list = at(*offered, { "missions" });
	std::string mission_text;
	if (mission_list && mission_list->is_array())
		for (const io::JsonValue &m : mission_list->array)
			mission_text += (mission_text.empty() ? "" : ", ") + text_at(m, { "file" });
	note("its missions: " + (mission_text.empty() ? std::string("none") : mission_text));
	std::string missions;
	if (!o.mission.empty())
		missions = ",\"missions\":[\"" + o.mission + "\"]";
	const std::string name = "Smoke " + random_hex(3);
	reply = site.user("POST", "/api/me/servers",
			"{\"data_set_id\":" + std::to_string(wanted) + ",\"game_name\":\"" + name +
					"\",\"max_players\":8" + missions + "}");
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
	// The launch was a worker's job: each program runs its own kinds alone.
	reply = site.admin("GET", "/api/admin/jobs?kind=machine.launch&state=done");
	const auto launches = json_of(reply);
	const io::JsonValue *done = launches ? launches->get("jobs") : nullptr;
	std::string launched_by;
	if (done != nullptr && done->is_array() && !done->array.empty())
		launched_by = text_at(done->array.front(), { "worker_id" });
	check(launched_by.rfind("worker-", 0) == 0,
			"its machine.launch was done by a worker process (" +
					(launched_by.empty() ? std::string("none") : launched_by) + ")",
			status_text(reply));
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
	// The console: commands through the fleet's admin link to the server.
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
	check_admin_views(site, id);
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

// The static addresses (with --zone-file, the zone the worker writes): the admin
// grants the account an own machine and a comp address, which the worker allocates
// from the stack's fixed range and names in the zone file; an own-machine server
// started at it runs on a machine at that address and lists there; stopped and
// started again, it keeps it. With billing on, a checkout past the fleet's shared
// places (an admin's grant fills them) is refused sold_out.
void check_static_address(const Site &site, int64_t user_id, const Options &o) {
	auto reply = site.admin("GET", "/api/admin/addresses");
	const auto view = json_of(reply);
	const io::JsonValue *enabled = view ? at(*view, { "enabled" }) : nullptr;
	if (!enabled || !enabled->is_bool() || !enabled->boolean) {
		note("static addresses not checked: off (" + status_text(reply) + ")");
		return;
	}
	reply = site.admin("PUT", "/api/admin/users/" + std::to_string(user_id) + "/comp",
			"{\"servers\":1,\"own_machines\":1}");
	if (!check(reply.code == 200, "the admin grants the account an own machine (comp grant)", status_text(reply)))
		return;
	const std::string name = "smoke-" + random_hex(3);
	reply = site.admin("POST", "/api/admin/users/" + std::to_string(user_id) + "/addresses",
			"{\"name\":\"" + name + "\"}");
	const auto made = json_of(reply);
	const int64_t address = reply.code == 201 && made ? number_at(*made, { "address", "id" }) : 0;
	if (!check(address > 0, "the admin gives it a comp static address " + name, status_text(reply)))
		return;
	std::string ip;
	std::string why;
	const bool ready = wait_for(
			[&] {
				const auto r = site.user("GET", "/api/me/addresses");
				const auto v = json_of(r);
				const io::JsonValue *list = v ? at(*v, { "addresses" }) : nullptr;
				why = status_text(r);
				if (!list || !list->is_array()) return false;
				for (const io::JsonValue &a : list->array) {
					if (number_at(a, { "id" }) != address) continue;
					ip = text_at(a, { "public_ip" });
					why = "state " + text_at(a, { "state" });
					return text_at(a, { "state" }) == "ready" && !ip.empty();
				}
				return false;
			},
			60);
	if (!check(ready, "the worker allocates it (ready at " + ip + ")", why))
		return;
	if (!o.zone_file.empty()) {
		std::ifstream in(o.zone_file);
		std::stringstream text;
		text << in.rdbuf();
		check(text.str().find(name + "\t300\tIN\tA\t" + ip + "\n") != std::string::npos,
				"the zone file names it (" + o.zone_file + ")", "no A record " + name + " " + ip);
	}
	// Twice: a start at the address runs and lists there, then stops.
	for (int round = 1; round <= 2; ++round) {
		const std::string label = round == 1 ? "an own-machine server" : "started again, it";
		reply = site.user("POST", "/api/me/servers",
				"{\"data_set_id\":" + o.hosting + ",\"game_name\":\"Smoke at " + name +
						"\",\"own_machine\":true,\"address_id\":" + std::to_string(address) + "}");
		const auto started = json_of(reply);
		const int64_t id = reply.code == 202 && started ? number_at(*started, { "server", "id" }) : 0;
		if (!check(id > 0, label + " starts at the address (POST /api/me/servers: 202)", status_text(reply)))
			return;
		const std::string path = "/api/me/servers/" + std::to_string(id);
		std::string state;
		std::string listed;
		const auto read = [&] {
			const auto r = site.user("GET", path);
			const auto v = json_of(r);
			if (r.code != 200 || !v) return false;
			state = text_at(*v, { "server", "state" });
			listed = text_at(*v, { "server", "listing", "host_ip" });
			why = "state " + state + ", " + text_at(*v, { "server", "failure_reason" });
			return (state == "running" && !listed.empty()) || state == "failed" || state == "stopped";
		};
		wait_for(read, o.timeout_s);
		// Its machine is at the address (the listing's is the machine's as the lobby
		// sees it through Docker).
		const auto admin_view = json_of(site.admin("GET", "/api/admin/hosted/" + std::to_string(id)));
		const std::string machine_at = admin_view ? text_at(*admin_view, { "machine", "public_address" }) : "";
		check(state == "running" && machine_at == ip, label + " runs on a machine at its static address (" + ip + ")",
				why + ", its machine at " + machine_at);
		note("its listing names " + listed);
		reply = site.user("POST", path + "/stop");
		check(reply.code == 202 &&
						wait_for(
								[&] {
									read();
									return state == "stopped";
								},
								120),
				"it stops", why);
	}

	// The capacity gate, with billing on: the fleet's shared places granted away,
	// a new account's tier is sold out.
	reply = http("GET", o.site, "/api/billing/tiers");
	const auto tiers = json_of(reply);
	const io::JsonValue *billing_on = tiers ? at(*tiers, { "enabled" }) : nullptr;
	if (!billing_on || !billing_on->is_bool() || !billing_on->boolean) {
		note("the capacity gate not checked at checkout: billing is off");
		return;
	}
	reply = site.admin("PUT", "/api/admin/users/" + std::to_string(user_id) + "/comp",
			"{\"servers\":16,\"own_machines\":0}");
	check(reply.code == 200, "the admin's grant fills the fleet's shared places", status_text(reply));
	const std::string other = "smoke_" + random_hex(4);
	const std::string password = "pw-" + random_hex(8);
	http("POST", o.site, "/api/register", { "Content-Type: application/json", "X-OpenNova-Request: 1" },
			"{\"username\":\"" + other + "\",\"password\":\"" + password + "\",\"email\":\"" + other +
					"@example.com\"}");
	reply = http("POST", o.site, "/api/login", { "Content-Type: application/json", "X-OpenNova-Request: 1" },
			"{\"username\":\"" + other + "\",\"password\":\"" + password + "\"}");
	Site second{ o.site, session_cookie(reply), o.admin_token };
	reply = second.user("POST", "/api/billing/checkout", "{\"item\":\"tier1\"}");
	const auto refused = json_of(reply);
	check(reply.code == 409 && refused && text_at(*refused, { "error" }) == "sold_out",
			"a checkout past the fleet is refused (409 sold_out)", status_text(reply));
	site.admin("PUT", "/api/admin/users/" + std::to_string(user_id) + "/comp", "{\"servers\":1,\"own_machines\":1}");
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

	if (!o.hosting.empty() && user_id > 0) {
		check_hosting(site, user_id, o);
		check_static_address(site, user_id, o);
	}
	else if (o.hosting.empty())
		note("hosting not checked: no data set (dev_stack.sh up --data <dir> or --synthetic-data)");
	check_attention(site);
	std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
	return g_failures;
}

// One archive of the synthetic data set: `entries` packed under `path` by the
// engine's own writer (new entries: no flags, the writer's fixed timestamp).
bool write_archive(const fs::path &path,
		const std::vector<std::pair<std::string, std::vector<uint8_t>>> &entries) {
	std::vector<opennova::pff::PffWriteEntry> packed;
	packed.reserve(entries.size());
	for (const auto &e : entries)
		packed.push_back({ e.first.c_str(), e.second.data(), static_cast<uint32_t>(e.second.size()),
				0, opennova::pff::PFF_NEW_ENTRY_TIMESTAMP, 0 });
	const int rc = opennova::pff::pff_write_archive(path.string().c_str(),
			opennova::pff::PFF_FORMAT_PFF3, packed.data(), static_cast<uint32_t>(packed.size()));
	if (rc != opennova::pff::PFF_WRITE_OK)
		std::fprintf(stderr, "%s: %s\n", path.string().c_str(),
				opennova::pff::pff_write_error_string(rc));
	return rc == opennova::pff::PFF_WRITE_OK;
}

// The synthetic data set: an install's root of one deathmatch map (servetst.bms)
// and the game text a server's listing reads, packed into the retail boot
// table's three archives (the mission in localres.pff, the texts in
// language.pff, as the mission catalog reads a base's pair; resource.pff a
// placeholder, since the table mounts no install without it) with a
// version.txt: the files a base data set's tree holds and nothing else.
int write_data_set(const fs::path &dir) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	const std::string placeholder = "OpenNova's synthetic data set: one deathmatch map.\n";
	const bool written =
			write_archive(dir / "resource.pff",
					{ { "readme.txt", std::vector<uint8_t>(placeholder.begin(), placeholder.end()) } }) &&
			write_archive(dir / "localres.pff",
					{ { "SERVETST.BMS", serve_test::deathmatch_mission("Serve Test Map") } }) &&
			write_archive(dir / "language.pff",
					{ { "gametext.bin",
							serve_test::gametext({
									{ "NovaWorld",
											{ { "STRNOVA07", "Jungle" }, { "STRNOVA08", "Desert" },
													{ "STRNOVA09", "Snow" }, { "STRNOVA10", "None" },
													{ "STRNOVA11", "Yes" }, { "STRNOVA12", "No" } } },
									{ "GateTypeAbbrev", { { "DM", "DM" } } },
							}) } }) &&
			serve_test::write_text(dir / "version.txt", "OpenNova Synthetic Data, V1.0.0.0; English\r\n");
	if (!written) {
		std::fprintf(stderr, "cannot write the data set to %s\n", dir.string().c_str());
		return 1;
	}
	std::printf("the synthetic data set (resource.pff, localres.pff with SERVETST.BMS, "
				"language.pff with gametext.bin, version.txt) is in %s\n",
			dir.string().c_str());
	return 0;
}

int usage() {
	std::fprintf(stderr,
			"usage: opennova-novaworld-stack-smoke run [--web <host:port>] [--site <host:port>]\n"
			"           [--gate <host:port>] [--livez <host:port>]... --admin-token <token>\n"
			"           [--hosting <data set id> [--mission <name.bms>] [--zone-file <path>]]\n"
			"           [--timeout <seconds>]\n"
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
		else if (arg == "--zone-file")
			o.zone_file = value;
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
