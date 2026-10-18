// Two hosted servers end to end on one machine's slots: an in-process NovaWorld
// service (nw_service_harness.h, hosted servers on) with its supervisor and its
// job workers (the fleet's jobs, hosted/model/fleet_jobs.h) over a machine driver of
// the test's whose machines are the real agent, opennova-hostd, each a process
// on this system with its process runtime (as the local stack's machine
// containers run it), launched with its token file and reaching the service's
// internal listener (hosted/internal_listener.h) on loopback; its servers are
// real opennova-serve children (ctest passes both binaries) over synthetic
// packed trees (hosted_tree_support.h: resource.pff, localres.pff holding
// serve_test_support.h's deathmatch map, language.pff holding the game text,
// version.txt), one per data set: the seeded Joint Operations and Delta Force
// Xtreme 2 rows (migration 0011) each given a ready tree in a local object
// store (ADR 0064), which the agent fetches into its trees root (--trees-root)
// from the file:// URL the sync's reply names, checks against the manifest and
// reports present. Built only with BUILD_NOVAWORLD_HTTP; Linux (the agent is).
//
//   * no lobby live (the service's lobby drained): the first start below waits
//     in provisioning, no machine launched, until the lobby takes clients again;
//   * the owner, granted a server (comp_grants), starts a Joint
//     Operations server with POST /api/me/servers (its data_set_id and the
//     mission its tree lists); the supervisor launches a shared machine, whose
//     agent bootstraps and syncs, places the server on its first slot, pins it
//     to the data set's current tree, mints its service account srv-<id> and
//     renders its files, which the agent writes to the slot's directory before
//     it starts opennova-serve over the fetched tree's directory with /game jo;
//     it logs in as that account, hosts and lists; the server reaches running;
//   * a second owner's Delta Force Xtreme 2 server lands on the same machine's
//     second slot (no second machine), on its own game port, with its own tree
//     and /game dfx2, and runs beside the first; the machine reports both trees
//     present; each listing's port is its slot's game port (NovaWorld lists a
//     server at its datagrams' source port, so two servers share one address);
//   * GET /api/me/hosts shows the first listing as the owner's hosted server;
//     the owner's SetServerName (POST /api/me/hosts/<rid>/command) reaches it,
//     and it saves game.cfg in its slot's directory with the name;
//   * the supervisor's admin link comes up once the server runs: it logs in on
//     the slot's remote-admin port with the rendered svc login, runs PLAYER
//     LIST and a recorded CHAT SEND, and the console ring holds the session's
//     line, the command and its reply, never the password;
//   * POST /api/me/servers/<id>/stop: the supervisor pushes ServerStopHosting,
//     the listing leaves the browser, the container is gone from the machine,
//     the row is stopped and the service account deleted; the admin link went
//     down at stopping; the second server runs on;
//   * the lobby is replaced at a new address (a second lobby takes clients, the
//     first drains and stops, its listings with it), the supervisor's safety
//     net: the second server's NovaWorld session reconnects only to the old
//     address, so once its listing has been gone listing_lost_ms the supervisor
//     places it again, no restart counted, and it lists through the gate on the
//     new lobby; then it stops the same way, its ServerStopHosting through the
//     new lobby. (A deployment's lobbies share one address, so its lobby
//     replacement relists a host without this: lobby_instances_e2e.)

#include "hosted_tree_support.h"
#include "lobby_client.h"
#include "nw_service_harness.h"

#include "accounts/web_session.h"
#include "core/local_object_store.h"
#include "data_sets/data_set_repository.h"
#include "hosted/admin_link.h"
#include "hosted/internal_listener.h"
#include "hosted/model/fleet_config.h"
#include "hosted/model/fleet_jobs.h"
#include "hosted/model/fleet_repository.h"
#include "hosted/model/hosted_repository.h"
#include "hosted/model/machine_driver.h"
#include "hosted/supervisor.h"
#include "jobs/worker_pool.h"
#include "serve/serve_test_support.h"

#include "net_http.h"
#include "net_sockets.h"

#include <base/io/file_io.h>
#include <base/io/log.h>
#include <base/proc/child_process.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/napi/session.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include <crow/json.h>

#include "common/temp_dir.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>

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

// A lobby instance of its own on the service's database, at a new address, as a
// lobby whose address changed runs: the lobby program's NW UDP
// listener and its connections, wired as apps/novaworld_lobby/main.cpp wires
// them, its SessionInit naming the service's web endpoint as the first's does.
struct ReplacementLobby {
	opennova::ConnectionManager manager;
	nws::NwUdpListener listener;
	std::string endpoint;

	explicit ReplacementLobby(opennova::db::ConnectionPool &pool) : listener(manager) {
		listener.set_db_pool(&pool);
		manager.on_lost([this](const opennova::Connection &c, opennova::DropReason r) {
			listener.erase_lobby_state(c.addr, opennova::drop_reason_name(r));
		});
	}

	// On a port free a moment ago (bound and let go), which its instance announces.
	bool start(uint16_t http_port) {
		nws::ServerConfig config;
		config.web_host = "127.0.0.1";
		config.http_port = http_port;
		{
			net::ScopedSocket probe(net::udp_bind(0, &config.nw_udp_port));
		}
		endpoint = "127.0.0.1:" + std::to_string(config.nw_udp_port);
		config.lobby_endpoint = endpoint;
		return listener.start(config);
	}

	void stop() {
		listener.stop();
		manager.shutdown();
	}
};

// --- the machines: opennova-hostd processes ------------------------------------------------

std::string read_text(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The test's machine driver: each machine the real agent, opennova-hostd with its
// process runtime, as a process of this system's (the local stack runs it as a
// container of its own), its user-data a token file in the machine's directory,
// <work>/machine-<id> (its state, its slots, its log), its trees root
// <work>/trees (one machine is expected). Its servers share this system's
// network, so its profile's slot ports are free ones probed on loopback,
// admitting loopback.
class HostdMachines : public hosted::MachineDriver {
public:
	HostdMachines(std::string hostd, std::string serve, fs::path work) :
			hostd_(std::move(hostd)), serve_(std::move(serve)), work_(std::move(work)) {}
	~HostdMachines() override {
		std::vector<std::string> ids;
		{
			std::lock_guard<std::mutex> lock(mu_);
			for (const auto &entry : machines_)
				ids.push_back(entry.first);
		}
		for (const std::string &id : ids)
			terminate(id);
	}

	static hosted::MachineProfile profile() {
		hosted::MachineProfile out;
		out.provider = "hostd";
		out.boot_timeout_ms = 60 * 1000;
		out.slot_plan = [](int) {
			hosted::SlotPlan plan;
			{
				net::ScopedSocket udp(net::udp_bind(0, &plan.game_port));
			}
			{
				net::ScopedSocket tcp(
						net::tcp_listen(0, 1, &plan.admin_port, /*loopback_only=*/true));
			}
			plan.admin_allowed_ip = "127.0.0.1";
			return plan;
		};
		return out;
	}

	void set_service_url(const std::string &url) { service_url_ = url; }
	fs::path dir(int64_t machine_id) const {
		return work_ / ("machine-" + std::to_string(machine_id));
	}
	// The machines' trees root (--trees-root), and tree `id`'s directory under
	// it, where the agent keeps a fetched tree and runs its servers over.
	fs::path trees_root() const { return work_ / "trees"; }
	fs::path tree_dir(int64_t id) const { return trees_root() / std::to_string(id); }
	// The agent's process id; 0 for a machine not running.
	int64_t pid(const std::string &cloud_id) {
		std::lock_guard<std::mutex> lock(mu_);
		const auto it = machines_.find(cloud_id);
		return it == machines_.end() ? 0 : it->second->pid();
	}

	const char *name() const override { return "hostd"; }

	std::optional<hosted::LaunchedMachine> launch(
			const hosted::MachineLaunch &launch, std::string &error) override {
		const fs::path machine = dir(launch.machine_id);
		std::error_code ec;
		fs::remove_all(machine, ec);
		fs::create_directories(machine, ec);
		const fs::path token = machine / "hostd.token";
		{
			std::ofstream out(token, std::ios::binary);
			out << launch.bootstrap_token;
		}
		fs::permissions(token, fs::perms::owner_read | fs::perms::owner_write,
				fs::perm_options::replace, ec);
		opennova::proc::SpawnOptions options;
		options.argv = { hostd_, "--runtime", "process", "--serve", serve_, "--service",
			service_url_, "--machine", std::to_string(launch.machine_id), "--token-file",
			token.u8string(), "--state-dir", (machine / "state").u8string(), "--slots-root",
			(machine / "slots").u8string(), "--trees-root", trees_root().u8string(), "--nft",
			"none" };
		options.output_path = (machine / "hostd.log").u8string();
		auto child = std::make_unique<opennova::proc::ChildProcess>();
		if (!child->spawn(options, error))
			return std::nullopt;
		hosted::LaunchedMachine out;
		out.cloud_id = "hostd-" + std::to_string(launch.machine_id);
		out.private_address = "127.0.0.1";
		out.public_address = "127.0.0.1";
		out.instance_type = "process";
		std::lock_guard<std::mutex> lock(mu_);
		machines_[out.cloud_id] = std::move(child);
		return out;
	}

	hosted::CloudMachine describe(const std::string &cloud_id, int64_t) override {
		hosted::CloudMachine out;
		std::lock_guard<std::mutex> lock(mu_);
		const auto it = machines_.find(cloud_id);
		if (it == machines_.end() || it->second->poll())
			return out; // Gone
		out.state = hosted::CloudState::Running;
		out.private_address = "127.0.0.1";
		out.public_address = "127.0.0.1";
		return out;
	}

	void terminate(const std::string &cloud_id) override {
		std::unique_ptr<opennova::proc::ChildProcess> child;
		{
			std::lock_guard<std::mutex> lock(mu_);
			const auto it = machines_.find(cloud_id);
			if (it == machines_.end())
				return;
			child = std::move(it->second);
			machines_.erase(it);
		}
		child->terminate();
		if (!child->wait(10000)) {
			child->kill();
			child->wait(5000);
		}
	}

	std::vector<hosted::TaggedCloudMachine> list_tagged() override {
		std::vector<hosted::TaggedCloudMachine> out;
		std::lock_guard<std::mutex> lock(mu_);
		for (const auto &entry : machines_)
			out.push_back({ entry.first, std::stoll(entry.first.substr(6)) });
		return out;
	}

private:
	std::string hostd_;
	std::string serve_;
	fs::path work_;
	std::string service_url_;
	std::mutex mu_;
	std::map<std::string, std::unique_ptr<opennova::proc::ChildProcess>> machines_;
};

// The command lines of process `parent`'s running children (the agent's servers).
std::vector<std::vector<std::string>> children_argv(int64_t parent) {
	std::vector<std::vector<std::string>> out;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator("/proc", ec)) {
		const std::string name = entry.path().filename().string();
		if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
			continue;
		const std::string stat = read_text(entry.path() / "stat");
		const size_t paren = stat.rfind(')');
		if (paren == std::string::npos || paren + 4 >= stat.size())
			continue;
		// "<pid> (<comm>) <state> <ppid> ...": a zombie (Z) has ended.
		if (stat[paren + 2] == 'Z')
			continue;
		if (std::atoll(stat.c_str() + paren + 4) != parent)
			continue;
		const std::string cmdline = read_text(entry.path() / "cmdline");
		std::vector<std::string> argv;
		size_t start = 0;
		while (start < cmdline.size()) {
			const size_t end = cmdline.find('\0', start);
			argv.push_back(cmdline.substr(
					start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
		out.push_back(std::move(argv));
	}
	return out;
}

std::optional<hosted::HostedServer> server_row(Service &service, int64_t id) {
	return hosted::find_server(*service.pool.acquire(), id);
}

// The machine row a server is on.
std::optional<hosted::FleetMachineRow> machine_of(Service &service, int64_t id) {
	const auto row = server_row(service, id);
	if (!row || !row->machine_ref) return std::nullopt;
	return hosted::find_machine(*service.pool.acquire(), *row->machine_ref);
}

// The server's slot directory on its machine.
fs::path slot_dir(const HostdMachines &machines, Service &service, int64_t id) {
	const auto row = server_row(service, id);
	if (!row || !row->machine_ref || row->slot < 0) return {};
	return machines.dir(*row->machine_ref) / "slots" / hosted::slot_dir_name(row->slot);
}

// What the machine's agent runs over tree directory `data` (the server's argv), empty for none.
std::vector<std::string> command_over(
		HostdMachines &machines, Service &service, int64_t id, const fs::path &data) {
	const auto machine = machine_of(service, id);
	if (!machine)
		return {};
	for (auto &argv : children_argv(machines.pid(machine->cloud_id))) {
		for (size_t i = 0; i + 1 < argv.size(); ++i) {
			if (argv[i] == "--resource-dir" && argv[i + 1] == data.u8string())
				return argv;
		}
	}
	return {};
}

// The machine's log (its agent's lines and its servers') and the server's events, for a
// failure's diagnosis.
void dump(Service &service, const HostdMachines &machines, int64_t id) {
	const auto row = server_row(service, id);
	if (row && row->machine_ref) {
		std::string log;
		std::string error;
		const fs::path path = machines.dir(*row->machine_ref) / "hostd.log";
		if (opennova::io::read_file_text(path.u8string(), log, error))
			std::fprintf(stderr, "---- machine %lld (last 6000 bytes)\n%s\n----\n",
					static_cast<long long>(*row->machine_ref),
					log.substr(log.size() > 6000 ? log.size() - 6000 : 0).c_str());
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
	test_redis::init(argc, argv);
	if (argc < 3) {
		std::fprintf(stderr,
				"usage: hosted_local_e2e_test <opennova-serve binary> <opennova-hostd binary>\n");
		return 2;
	}
	const std::string serve_binary = fs::absolute(argv[1]).u8string();
	const std::string hostd_binary = fs::absolute(argv[2]).u8string();
	opennova::io::set_log_sink(&log_sink);
	if (net::startup() != 0) {
		std::fprintf(stderr, "FAIL: net::startup\n");
		return 1;
	}
	const fs::path source_dir{ OPENNOVA_SOURCE_DIR };
	{
		// Declared first, so the database's files and the working directories go last.
		test_temp::TempDir temp("hosted_local_e2e");
		// Two synthetic packed trees, two data sets: each one deathmatch map (named
		// after its data set, so the listing tells which tree a server mounted)
		// and the game text, packed as a retail install is; each put in the local
		// object store under trees/<id>/ as its seeded data set's ready current
		// tree (the manifest hashed, the missions read from the archives).
		const fs::path jo_source = temp.path / "jo_source";
		const fs::path dfx2_source = temp.path / "dfx2_source";
		expect(hosted_trees::write_base_tree(jo_source, "SERVETST.BMS", "Serve Test Map", "JO V1.7.5.3"),
				"Joint Operations' tree packs");
		expect(hosted_trees::write_base_tree(dfx2_source, "DFXTEST.BMS", "DFX2 Test Map", "DFX2 V1.0"),
				"Delta Force Xtreme 2's tree packs");
		nws::LocalObjectStore store((temp.path / "store").u8string());

		Service service;
		int64_t jo_set = 0;
		int64_t dfx2_set = 0;
		int64_t jo_tree = 0;
		int64_t dfx2_tree = 0;
		{
			auto db = service.pool.acquire();
			jo_set = hosted_trees::seeded_data_set(*db, "Joint Operations");
			dfx2_set = hosted_trees::seeded_data_set(*db, "Delta Force Xtreme 2");
			expect(jo_set != 0 && dfx2_set != 0, "the seeded data sets are there");
			jo_tree = hosted_trees::seed_ready_tree(*db, store, jo_set, hosted::DataSetKind::Base, "", jo_source);
			dfx2_tree = hosted_trees::seed_ready_tree(*db, store, dfx2_set, hosted::DataSetKind::Base, "", dfx2_source);
			expect(jo_tree != 0 && dfx2_tree != 0, "each data set has a ready tree in the store");
			const auto offered = hosted::offered_data_sets(*db);
			expect(offered.size() == 2, "both data sets are offered");
			const auto lower = [](std::string s) {
				for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				return s;
			};
			for (const auto &d : offered) {
				expect(d.missions.size() == 1 &&
								lower(d.missions[0].file) == (d.id == jo_set ? "servetst.bms" : "dfxtest.bms"),
						"each offered data set lists its tree's mission");
			}
		}
		const bool up = service.start(source_dir, temp.path, /*require_host_key=*/false,
				[&store](nws::ServerConfig &config) {
					config.hosted_provider = "docker";
					config.hosted_cap = 5;
					config.object_store_url = store.url_of("");
				});
		expect(up, "the service starts");
		expect(nws::billing::set_comp_grants(*service.pool.acquire(), service.owner_id, { 1, 0 }), "the owner is granted a server");

		// The machines: opennova-hostd processes.
		const fs::path work = temp.path / "work";
		HostdMachines driver(hostd_binary, serve_binary, work);
		hosted::SupervisorConfig config;
		// Where the machines fetch each tree from: the store's URL of trees/<id>/,
		// as the fleet hands it (a file:// copy for the agent).
		config.tree_url = [&store](int64_t tree_id) {
			return store.url_of("trees/" + std::to_string(tree_id) + "/");
		};
		config.cap = 5;
		config.master_host = "127.0.0.1";
		config.master_gate_port = service.gate.bound_port();
		config.tick_interval_ms = 100;
		config.listing_timeout_ms = 60 * 1000;
		config.restart_limit = 0; // a crash fails it at once, with its reason
		// A listing gone this long places its server again (the lobby's replacement below).
		config.listing_lost_ms = 10 * 1000;
		config.serve_image = "opennova-serve:local";
		hosted::Supervisor supervisor(service.pool, HostdMachines::profile(), config);
		// The job workers that run the driver's calls (the launch, the describes, the ends).
		nws::jobs::JobRegistry job_registry;
		hosted::register_fleet_jobs(job_registry, driver, hosted::PlacementPolicy{});
		nws::jobs::WorkerPoolConfig worker_config;
		worker_config.threads = 4;
		worker_config.poll_ms = 50;
		worker_config.idle_max_ms = 200;
		nws::jobs::WorkerPool workers(service.pool, job_registry, worker_config);
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
		supervisor.set_stop_listing([](opennova::db::Database &db, uint32_t rid, int64_t account) {
			return opennova::lobbydb::queue_stop_hosting(db, rid, opennova::SERVER_MSG_CODE_NOVAWORLD_SYSOP_PUNT, 0,
						   0, account) == opennova::lobbydb::PushResult::Queued;
		});
		// The machine agents' routes on loopback, as main() wires them.
		hosted::InternalListener internal(
				service.pool, supervisor, hosted::agent_route_config(hosted::FleetConfig{}));
		// The lobby that replaces the service's own, at a new address (below).
		ReplacementLobby replacement(service.pool);
		const bool internal_up = up && internal.start("127.0.0.1", 0);
		expect(!up || internal_up, "the internal listener serves on loopback");
		if (internal_up) {
			driver.set_service_url("http://127.0.0.1:" + std::to_string(internal.bound_port()));
			workers.start();
			supervisor.start();
		}

		const std::string session = internal_up ? site_login(service, kOwner, kOwnerPassword) : std::string();
		expect(!internal_up || !session.empty(), "the owner logs in to the site");

		// The first start, while no lobby is live (the service's own drained, as an
		// admin's drain marks its row): it waits in provisioning, no machine
		// launched, and starts once the lobby takes clients again.
		const bool drained = !session.empty() &&
				opennova::lobbydb::set_instance_state(
						*service.pool.acquire(), service.nwudp.lobby_id(), "draining");
		expect(session.empty() || drained, "the lobby drains");
		int64_t id = 0;
		if (!session.empty()) {
			const auto reply = send(service, "POST", "/api/me/servers", as_owner(session, true),
					"{\"data_set_id\":" + std::to_string(jo_set) +
							",\"game_name\":\"Hosted E2E\",\"max_players\":8,"
							"\"missions\":[\"servetst.bms\"]}");
			const auto json = crow::json::load(body_text(reply));
			if (reply.code != 202) std::fprintf(stderr, "  start: %d %s\n", reply.code, body_text(reply).c_str());
			const auto started = reply.code == 202 ? integer_at(json, { "server", "id" }) : std::nullopt;
			expect(reply.transport_ok && started.has_value(), "POST /api/me/servers is accepted");
			id = started.value_or(0);
		}
		if (id != 0 && drained) {
			std::this_thread::sleep_for(std::chrono::seconds(2));
			const auto row = server_row(service, id);
			expect(row && row->state == hosted::State::Provisioning && !row->machine_ref &&
							hosted::list_live_machines(*service.pool.acquire()).empty(),
					"with no lobby live the start waits in provisioning, no machine launched");
		}
		if (drained) {
			expect(opennova::lobbydb::set_instance_state(
						   *service.pool.acquire(), service.nwudp.lobby_id(), "active"),
					"the lobby takes clients again");
		}
		bool running = false;
		if (id != 0) {
			running = wait_for([&] {
				const auto row = server_row(service, id);
				return row && (row->state == hosted::State::Running || !hosted::is_live(row->state));
			}, 90);
			const auto row = server_row(service, id);
			running = running && row && row->state == hosted::State::Running;
			if (!running)
				dump(service, driver, id);
			expect(running, "the hosted server is placed, started on its slot, lists with its service account and runs");
		}

		// The second owner's server, on Delta Force Xtreme 2's data set, beside it.
		int64_t second_owner = 0;
		if (running) {
			nws::CreateUserParams p;
			p.username = "owner2";
			p.password = "pw-owner2";
			p.pcid = "0e2e0002";
			p.nwh = "1";
			p.nwhandle = "Owner2Handle";
			const auto created = nws::create_user(*service.pool.acquire(), p);
			second_owner = created.ok ? created.id : 0;
			expect(second_owner > 0 && nws::billing::set_comp_grants(*service.pool.acquire(), second_owner, { 1, 0 }),
					"a second owner is granted a server");
		}
		const std::string second = second_owner > 0 ? site_login(service, "owner2", "pw-owner2") : std::string();
		expect(!running || !second.empty(), "the second owner logs in to the site");
		int64_t dfx2_id = 0;
		if (!second.empty()) {
			const auto reply = send(service, "POST", "/api/me/servers", as_owner(second, true),
					"{\"data_set_id\":" + std::to_string(dfx2_set) +
							",\"game_name\":\"Hosted E2E DFX2\",\"max_players\":8,"
							"\"missions\":[\"dfxtest.bms\"]}");
			if (reply.code != 202) std::fprintf(stderr, "  dfx2 start: %d %s\n", reply.code, body_text(reply).c_str());
			dfx2_id = reply.code == 202 ? integer_at(crow::json::load(body_text(reply)), { "server", "id" }).value_or(0)
										: 0;
			expect(dfx2_id != 0, "POST /api/me/servers is accepted for Delta Force Xtreme 2");
		}
		bool dfx2_running = false;
		if (dfx2_id != 0) {
			dfx2_running = wait_for([&] {
				const auto row = server_row(service, dfx2_id);
				return row && (row->state == hosted::State::Running || !hosted::is_live(row->state));
			}, 90) && server_row(service, dfx2_id)->state == hosted::State::Running;
			if (!dfx2_running)
				dump(service, driver, dfx2_id);
			expect(dfx2_running, "the Delta Force Xtreme 2 server runs beside the first");
		}

		uint32_t rid = 0;
		if (running && dfx2_running) {
			const auto jo = server_row(service, id);
			const auto dfx2 = server_row(service, dfx2_id);
			const auto machine = machine_of(service, id);
			expect(jo->machine_ref && jo->machine_ref == dfx2->machine_ref, "both run on one machine");
			expect(jo->slot == 0 && dfx2->slot == 1 && jo->game_port != dfx2->game_port,
					"each on its own slot and game port");
			expect(machine && machine->kind == hosted::MachineKind::Shared &&
							machine->state == hosted::FleetMachineState::Ready &&
							machine->provider == "hostd",
					"the machine is a ready shared machine");
			expect(hosted::list_live_machines(*service.pool.acquire()).size() == 1, "no second machine");
			const auto account = jo->service_user_id ? nws::get_user_by_id(*service.pool.acquire(), *jo->service_user_id)
													 : std::nullopt;
			expect(account && account->kind == "service" && account->username == nws::service_username(id),
					"the server lists as its own service account");
			const fs::path dir = slot_dir(driver, service, id);
			expect(fs::exists(dir / "server.host") && fs::exists(dir / "credentials.txt"),
					"its slot's directory was written from its files");
			expect(jo->settings.data_set_id == jo_set && dfx2->settings.data_set_id == dfx2_set &&
							!jo->settings.expansion_id && !dfx2->settings.expansion_id &&
							jo->settings.missions == std::vector<std::string>{ "servetst.bms" } &&
							dfx2->settings.missions == std::vector<std::string>{ "dfxtest.bms" },
					"the rows name their data sets and rotations");
			expect(jo->base_tree_id == jo_tree && dfx2->base_tree_id == dfx2_tree &&
							!jo->expansion_tree_id && !dfx2->expansion_tree_id,
					"each server is pinned to its data set's current tree");
			// The trees on the machine: fetched from the store, marked, reported present
			// with their bytes.
			expect(fs::is_regular_file(driver.tree_dir(jo_tree) / ".ready") &&
							fs::is_regular_file(driver.tree_dir(jo_tree) / "localres.pff") &&
							fs::is_regular_file(driver.tree_dir(dfx2_tree) / ".ready") &&
							fs::is_regular_file(driver.tree_dir(dfx2_tree) / "localres.pff"),
					"both trees are in the machine's trees root, marked ready");
			const std::vector<std::string> jo_command =
					command_over(driver, service, id, driver.tree_dir(jo_tree));
			const std::vector<std::string> dfx2_command =
					command_over(driver, service, dfx2_id, driver.tree_dir(dfx2_tree));
			const auto loose = [](const std::vector<std::string> &argv) {
				return std::find(argv.begin(), argv.end(), "--loose-root") != argv.end();
			};
			expect(!jo_command.empty() && jo_command[0] == serve_binary &&
							has_pair(jo_command, "/game", "jo") && !loose(jo_command),
					"opennova-serve runs over Joint Operations' tree with /game jo, packed");
			expect(has_pair(dfx2_command, "/game", "dfx2") && !loose(dfx2_command),
					"opennova-serve runs over Delta Force Xtreme 2's tree with /game dfx2");

			// Each listing at its slot's game port: one address, two servers.
			auto reply = send(service, "GET", "/api/me/servers/" + std::to_string(id), as_owner(session, false));
			auto json = crow::json::load(body_text(reply));
			const auto listed_rid = integer_at(json, { "server", "listing", "rid" });
			if (!listed_rid) std::fprintf(stderr, "  /api/me/servers/<id>: %s\n", body_text(reply).c_str());
			expect(listed_rid.has_value(), "GET /api/me/servers/<id> names the listing");
			rid = static_cast<uint32_t>(listed_rid.value_or(0));
			expect(integer_at(json, { "server", "listing", "host_port" }) == jo->game_port,
					"the first listing's port is its slot's game port");
			reply = send(service, "GET", "/api/me/servers/" + std::to_string(dfx2_id), as_owner(second, false));
			json = crow::json::load(body_text(reply));
			expect(integer_at(json, { "server", "listing", "host_port" }) == dfx2->game_port &&
							text_at(json, { "server", "game_display_name" }) == "Delta Force Xtreme 2",
					"the second listing's port is its own slot's");

			// The owner's view: the listing, as its hosted server's.
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
			const std::string cfg_path = (dir / "game.cfg").u8string();
			const bool renamed = wait_for([&] {
				const auto cfg = opennova::gamecfg::load_file(cfg_path, {});
				return cfg.file_read && cfg.cfg.game_name == "Renamed Hosted";
			}, 20);
			if (!renamed)
				dump(service, driver, id);
			expect(renamed, "the server takes the owner's name and saves game.cfg in its slot");

			// The admin link: up from running, logged in with the rendered login.
			const auto login = hosted::admin_login_of(*service.pool.acquire(), id);
			expect(login && login->user == "svc" && login->password.size() == hosted::kAdminPasswordChars,
					"the start rendered the admin login");
			std::shared_ptr<hosted::AdminLink> link;
			const bool link_up = wait_for([&] {
				link = links.find(id);
				return link && link->status().state == hosted::AdminLinkState::Up;
			}, 30);
			if (!link_up)
				dump(service, driver, id);
			expect(link_up, "the admin link logs in on the running server's slot");
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
					bool reply_line = false;
					for (const auto &line : hosted::list_console(*service.pool.acquire(), id, 0, 100)) {
						up_line = up_line || (line.kind == "status" && line.text == "the admin session is up");
						command = command ||
								(line.kind == "command" && line.actor == "owner" &&
										line.text == "CHAT SEND hello from the service");
						reply_line = reply_line || (line.kind == "reply" && line.text == "OK - Chat sent.");
					}
					return up_line && command && reply_line;
				}, 10);
				expect(recorded, "the console ring holds the session, the command and its reply");
			}
			link.reset();

			// The first server's stop; the second runs on.
			const std::string container = jo->container;
			reply = send(service, "POST", "/api/me/servers/" + std::to_string(id) + "/stop", as_owner(session, true));
			json = crow::json::load(body_text(reply));
			const std::string stop_state = text_at(json, { "server", "state" });
			if (reply.code != 202) std::fprintf(stderr, "  stop: %d %s\n", reply.code, body_text(reply).c_str());
			expect(reply.code == 202 && (stop_state == "stopping" || stop_state == "stopped"),
					"POST /api/me/servers/<id>/stop is accepted");
			const bool stopped = wait_for([&] {
				const auto r = server_row(service, id);
				return r && !hosted::is_live(r->state);
			}, 60);
			const auto ended = server_row(service, id);
			if (!stopped || !ended || ended->state != hosted::State::Stopped)
				dump(service, driver, id);
			expect(stopped && ended && ended->state == hosted::State::Stopped, "the server ends stopped");
			expect(!opennova::hostdb::find_host_by_rid(*service.pool.acquire(), rid), "its listing has left the browser");
			expect(wait_for(
						   [&] {
							   return !fs::exists(dir) &&
									   command_over(driver, service, dfx2_id, driver.tree_dir(dfx2_tree)).size() > 0 &&
									   command_over(driver, service, id, driver.tree_dir(jo_tree)).empty();
						   },
						   30),
					"its server and its slot's directory are gone from the machine");
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
			expect(server_row(service, dfx2_id)->state == hosted::State::Running, "the second server runs on");
			std::printf("hosted_local_e2e: server %lld ran as RID %u on slot 0 beside server %lld, took its "
						"owner's name and stopped\n",
					static_cast<long long>(id), rid, static_cast<long long>(dfx2_id));
		}

		// A lobby's replacement at a new address (a deployment's lobbies share
		// one, so this is the supervisor's safety net for a host whose lobby
		// address is gone): a second lobby takes clients, the first drains and
		// stops, its listings going with it. The second server's NovaWorld session reconnects
		// only to the old lobby's address (retail's), so its listing stays gone;
		// listing_lost_ms on (short here) the supervisor places it again with no
		// restart counted (this test leaves it none), and its new start lists
		// through the gate on the new lobby and runs.
		if (dfx2_running) {
			const auto listed_rid = [&]() -> std::optional<uint32_t> {
				const auto r = server_row(service, dfx2_id);
				if (!r || !r->service_user_id)
					return std::nullopt;
				return hosted::listing_rid_of(*service.pool.acquire(), *r->service_user_id);
			};
			const std::string old_container = server_row(service, dfx2_id)->container;
			const auto old_rid = listed_rid();
			expect(old_rid.has_value(), "the second server is listed on the first lobby");
			const bool second_up = replacement.start(service.http.bound_port());
			expect(second_up, "a second lobby starts at a new address");
			expect(service.nwudp.drain(), "the first lobby drains");
			const net::Endpoint gate{ { 127, 0, 0, 1 }, service.gate.bound_port() };
			const auto hands_out_new = [&] {
				return lobby_client::gate_hands_out(gate) == replacement.endpoint;
			};
			const bool handed = second_up && wait_for(hands_out_new, 30);
			expect(handed, "the gate hands out the new lobby alone");
			service.nwudp.stop();
			service.manager.shutdown();
			const bool old_gone = !old_rid ||
					!opennova::hostdb::find_host_by_rid(*service.pool.acquire(), *old_rid);
			expect(old_gone, "the first lobby's stop took the listing with it");
			const int64_t new_lobby = replacement.listener.lobby_id();
			// Running again, a new container, listed on the new lobby's instance.
			const auto listed_on_new = [&] {
				const auto r = server_row(service, dfx2_id);
				const auto rid_now = listed_rid();
				if (!r || r->state != hosted::State::Running || r->container == old_container ||
						!rid_now)
					return false;
				const auto rows = service.pool.acquire()->query(
						"SELECT lobby_id FROM active_hosts WHERE rid = $1;",
						{ opennova::db::BindValue(static_cast<int64_t>(*rid_now)) });
				return !rows.empty() && rows[0].as_int(0) == new_lobby;
			};
			const bool relisted = handed && wait_for(listed_on_new, 90);
			if (!relisted)
				dump(service, driver, dfx2_id);
			expect(relisted, "the second server, placed again, lists on the new lobby");
			const auto after = server_row(service, dfx2_id);
			expect(after && after->restarts == 0, "no restart counted");
			const std::string placed = "placed again (no restart counted)";
			bool placed_again = false;
			for (const auto &e : hosted::list_events(*service.pool.acquire(), dfx2_id, 50)) {
				placed_again = placed_again ||
						(e.from_state == "running" && e.to_state == "provisioning" &&
								e.detail.rfind("no listing for ", 0) == 0 &&
								e.detail.find(placed) != std::string::npos);
			}
			expect(placed_again, "its timeline says why it was placed again");
			std::printf("hosted_local_e2e: the lobby replaced at a new address, server %lld was "
						"placed again and lists on the new lobby\n",
					static_cast<long long>(dfx2_id));
		}

		if (dfx2_id != 0) {
			const auto reply = send(service, "POST", "/api/me/servers/" + std::to_string(dfx2_id) + "/stop", as_owner(second, true));
			expect(reply.code == 202, "the second owner's stop is accepted");
			const bool stopped = wait_for([&] {
				const auto r = server_row(service, dfx2_id);
				return r && !hosted::is_live(r->state);
			}, 60);
			const auto ended = server_row(service, dfx2_id);
			if (!stopped || !ended || ended->state != hosted::State::Stopped)
				dump(service, driver, dfx2_id);
			expect(stopped && ended && ended->state == hosted::State::Stopped, "the Delta Force Xtreme 2 server stops");
		}

		supervisor.stop();
		links.stop_all();
		workers.stop();
		internal.stop();
		replacement.stop();
		service.stop();
	}
	net::shutdown();
	opennova::io::set_log_sink(nullptr);
	if (g_failures == 0) {
		std::printf("OK: two hosted servers run on one machine's slots, list, take their owners' commands and stop\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
