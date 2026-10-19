#pragma once

// The hosted servers' supervisor on a fresh migrated database, a FakeDriver and
// a hand-stepped clock, with each machine's agent played by a FakeAgent
// (hosted_fake_driver.h) and the fleet's jobs (fleet_jobs.h) run on the test's
// thread after each pass, as the workers would within the second; and the
// fixtures hosted_supervisor_test and hosted_fleet_test share: the data sets'
// trees (seed_data: the seeded Joint Operations, Escalation and Delta Force
// Xtreme 2 rows each given a ready tree, data_set_repository.h), player
// accounts, the billing cache's subscriptions, a listing made by hand, the
// events read back. A tick() is one pass, then the periodic sweep's schedule
// and every job due; a step() is what a second of the service does: a tick,
// then one sync of every machine's agent (a muted machine's agent is silent).

#include "accounts/auth.h"
#include "billing/billing_repository.h"
#include "data_sets/data_set_repository.h"
#include "hosted/cloud/fleet_job_handlers.h"
#include "hosted/model/fleet_jobs.h"
#include "hosted/model/fleet_repository.h"
#include "hosted/model/hosted_repository.h"
#include "hosted/model/hosted_settings.h"
#include "hosted/supervisor.h"
#include "hosted_fake_driver.h"
#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include "common/temp_dir.h"
#include "common/test_database.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hosted_rig {

namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
namespace jobs = opennova::novaworld_server::jobs;
namespace hostdb = opennova::hostdb;
using hosted::State;
using hosted_test::FakeAgent;
using hosted_test::FakeDriver;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

// The wall clock the entitlements are read at (a subscription's period end is a
// date): the rigs' starting point.
inline constexpr int64_t kWall = 1760000000;
// The serve image the rigs' starts pin.
inline constexpr const char *kServeImage = "ghcr.io/opennova-net/opennova-serve:sha-test";

// The seeded data sets (migration 0011: Joint Operations, Escalation under it,
// Delta Force Xtreme 2) and the ready tree each got.
struct SeededData {
	int64_t jo = 0;
	int64_t jox = 0; // Escalation, jox01
	int64_t dfx2 = 0;
	int64_t jo_tree = 0;
	int64_t jox_tree = 0;
	int64_t dfx2_tree = 0;
};

inline int64_t seeded_data_set(Database &db, const char *display_name) {
	const auto rows = db.query("SELECT id FROM data_sets WHERE display_name = $1;", { BindValue(display_name) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

// A ready tree of `data_set_id` with `version`, `missions` (each file titled after itself)
// and a two-file manifest; its id.
inline int64_t seed_tree(Database &db, int64_t data_set_id, const std::string &version,
		const std::vector<std::string> &missions, const std::vector<std::string> &files = { "resource.pff", "version.txt" }) {
	const int64_t id = hosted::create_tree(db, data_set_id, hosted::TreeSource::Baseline, hosted::TreeState::Finalizing, std::nullopt, std::nullopt, "test");
	hosted::TreeReady ready;
	for (size_t i = 0; i < files.size(); ++i)
		ready.files.push_back({ files[i], 1000 + i });
	for (const std::string &m : missions) ready.missions.push_back({ m, "Map " + m });
	ready.version_text = version;
	ready.bytes = 2001;
	if (!hosted::finish_tree(db, id, hosted::TreeState::Finalizing, ready)) return 0;
	return id;
}

// The three seeded data sets each given a ready tree: Joint Operations' lists the two
// missions valid_settings names and one more, Escalation's one of its own, Delta Force
// Xtreme 2's two.
inline SeededData seed_data(Database &db) {
	SeededData d;
	d.jo = seeded_data_set(db, "Joint Operations");
	d.jox = seeded_data_set(db, "Escalation");
	d.dfx2 = seeded_data_set(db, "Delta Force Xtreme 2");
	d.jo_tree = seed_tree(db, d.jo, "JO V1.7.5.3", { "second.bms", "SERVETST.BMS", "third.bms" });
	d.jox_tree = seed_tree(db, d.jox, "JOX V1.0", { "escal.bms" }, { "jox01.pff", "jox01L.pff", "version.txt" });
	d.dfx2_tree = seed_tree(db, d.dfx2, "DFX2 V1.0", { "dfx2_a.bms", "dfx2_b.bms" });
	return d;
}

// The settings of a server on data set `data_set_id` (Joint Operations' for the rig's:
// its tree lists the two missions).
inline hosted::HostedSettings valid_settings(int64_t data_set_id) {
	hosted::HostedSettings s;
	s.data_set_id = data_set_id;
	s.game_name = "Hosted Test";
	s.missions = { "servetst.bms", "second.bms" };
	return s;
}

// The store's URL of tree `id` as the rigs' replies carry it.
inline std::string tree_url_of(int64_t id) {
	return "file:///store/trees/" + std::to_string(id) + "/";
}

// A player with an admin's grant of `comp_servers` hosted servers (comp_grants).
inline int64_t make_player(Database &db, const std::string &name, int comp_servers) {
	static int pcid_counter = 0;
	nws::CreateUserParams p;
	p.username = name;
	p.password = "pw-" + name;
	char pcid[16];
	std::snprintf(pcid, sizeof(pcid), "%08x", static_cast<unsigned>(0x0d000000 + ++pcid_counter));
	p.pcid = pcid;
	p.nwh = "1";
	p.nwhandle = name + "H";
	const auto created = nws::create_user(db, p);
	if (!created.ok) {
		std::fprintf(stderr, "  create_user %s: %s\n", name.c_str(), created.error_code.c_str());
		return 0;
	}
	if (comp_servers > 0 && !nws::billing::set_comp_grants(db, created.id, { comp_servers, 0 })) return 0;
	return created.id;
}

// A support tier subscription of `user`'s in the billing cache, as the sync
// worker writes it: `tier` (1..3), `status`, its period ending a month on.
inline void subscribe(Database &db, int64_t user, int tier, const std::string &status, const std::string &id) {
	namespace billing = opennova::novaworld_server::billing;
	billing::SubscriptionRow row;
	row.stripe_subscription_id = id;
	row.stripe_customer_id = "cus_" + id;
	row.user_id = user;
	row.status = status;
	row.item = billing::ItemKind::Tier;
	row.price_id = "price_tier" + std::to_string(tier);
	row.tier = tier;
	row.current_period_end = kWall + 30 * 24 * 60 * 60;
	billing::upsert_subscription(db, row);
}

// The owner's own-machine add-on in the billing cache, `quantity` machines.
inline void subscribe_machines(Database &db, int64_t user, int64_t quantity, const std::string &id) {
	namespace billing = opennova::novaworld_server::billing;
	billing::SubscriptionRow row;
	row.stripe_subscription_id = id;
	row.stripe_customer_id = "cus_" + id;
	row.user_id = user;
	row.status = "active";
	row.item = billing::ItemKind::OwnMachine;
	row.price_id = "price_own";
	row.quantity = quantity;
	row.current_period_end = kWall + 30 * 24 * 60 * 60;
	billing::upsert_subscription(db, row);
}

// A 48-letter A..P HostKey, distinct per `n`.
inline std::string key_for(uint32_t n) {
	std::string key(hostdb::kHostKeyChars, 'A');
	for (size_t i = 0; i < 8; ++i) key[i] = static_cast<char>('A' + ((n >> (i * 4)) & 0xF));
	return key;
}

// What the lobby does when `account`'s server hosts and presents a key its
// /NWHost.dll recorded: the row listed under `rid`, owned by the account.
inline bool list_as(Database &db, int64_t account, uint32_t rid) {
	const auto user = nws::get_user_by_id(db, account);
	if (!user) return false;
	const std::string key = key_for(rid);
	if (!hostdb::record_host_key(db, key, account, user->credentials_gen)) return false;
	hostdb::HostRow row;
	row.rid = rid;
	row.gsid = "GSID-" + std::to_string(rid);
	row.game = "jop_2_consumer";
	row.server_name = "Hosted Listing";
	row.host_ip = "198.51.100.1";
	row.host_port = 32768;
	row.peer_ip = "198.51.100.1";
	row.peer_port = 32768;
	row.host_user_id = hostdb::claim_host_key(db, key, rid);
	hostdb::upsert_host(db, row);
	return row.host_user_id == account;
}

inline std::vector<std::string> transitions(Database &db, int64_t id) {
	std::vector<std::string> out;
	for (const auto &e : hosted::list_events(db, id, 100)) out.push_back(e.from_state + ">" + e.to_state);
	return out;
}

inline std::string last_detail(Database &db, int64_t id) {
	const auto events = hosted::list_events(db, id, 1);
	return events.empty() ? std::string() : events.back().detail;
}

inline bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

// Whether any of the server's events says `part`.
inline bool has_event(Database &db, int64_t id, const std::string &part) {
	for (const auto &e : hosted::list_events(db, id, 100)) {
		if (contains(e.detail, part)) return true;
	}
	return false;
}

// Whether the server was stopped with `part` in its stop's event: stopping, or
// already stopped (a server with no container yet finishes its stop in the pass
// that asked it).
inline bool stopped_with(Database &db, int64_t id, const std::string &part) {
	const auto row = hosted::find_server(db, id);
	if (!row || (row->state != State::Stopping && row->state != State::Stopped)) return false;
	for (const auto &e : hosted::list_events(db, id, 100)) {
		if (e.to_state == "stopping" && contains(e.detail, part)) return true;
	}
	return false;
}

// The value of `key=` in the credentials file's text.
inline std::string credential(const std::string &text, const std::string &key) {
	const std::string prefix = key + "=";
	const auto at = text.find(prefix);
	if (at == std::string::npos) return {};
	const auto start = at + prefix.size();
	return text.substr(start, text.find('\n', start) - start);
}

// One supervisor over a fresh database and a FakeDriver, its clock in hand.
// The supervisor leases its pool; the test and these helpers use `db`, a
// connection of their own, so no thread ever holds two of the pool's leases.
struct Rig {
	test_temp::TempDir temp;
	test_db::TestDatabase scratch;
	ConnectionPool pool;
	Database db;
	FakeDriver fake;
	hosted::SupervisorConfig config;
	std::unique_ptr<hosted::Supervisor> sup;
	jobs::JobRegistry registry;
	std::unique_ptr<jobs::Scheduler> scheduler;
	std::vector<std::pair<uint32_t, int64_t>> pushes;
	std::map<int64_t, FakeAgent> agents; // by machine row
	std::set<int64_t> muted;             // machines whose agents stay silent
	bool start_running = true;           // a new agent's containers run at once
	uint64_t now = 1000;
	int64_t wall = kWall; // the entitlements' clock, Unix seconds
	SeededData data; // the data sets' rows and their ready trees

	explicit Rig(const char *name) : temp(name), pool(scratch.options()), db(scratch.open()) {
		data = seed_data(db);
		config.cap = 3;
		config.master_host = "203.0.113.5";
		config.master_gate_port = 7597;
		config.boot_timeout_ms = 10000;
		config.listing_timeout_ms = 20000;
		config.health_interval_ms = 5000;
		config.health_misses = 3;
		config.restart_limit = 2;
		config.restart_backoff_ms = 3000;
		config.stop_timeout_ms = 60000;
		config.tree_url = tree_url_of;
		config.wall_clock_s = [this] { return wall; };
		config.placement.shared_slots = 4;
		config.placement.max_shared = 2;
		config.placement.max_own = 2;
		config.serve_image = kServeImage;
		config.idle_ms = 10 * 60 * 1000;
		config.lost_end_ms = 3 * 60 * 1000;
		config.launch_backoff_ms = 5000;
		config.launch_backoff_max_ms = 20000; // 5, 10, 20, 20 s
		// Every pass that wants a describe asks one (the jobs then run at once).
		config.describe_interval_ms = 0;
		config.clock_ms = [this] { return now; };
		make_supervisor();
		lobby_up();
	}
	// The supervisor over `config`, and the fleet's jobs registered again with its
	// maximums (as a worker reads the same settings).
	void make_supervisor() {
		hosted::register_fleet_jobs(registry, fake, config.placement);
		sup = std::make_unique<hosted::Supervisor>(pool, fake.profile(), config);
		sup->set_stop_listing([this](opennova::db::Database &, uint32_t rid, int64_t account) {
			pushes.emplace_back(rid, account);
			return true;
		});
		scheduler = std::make_unique<jobs::Scheduler>(pool,
				hosted::fleet_schedule(hosted::kFleetSweepIntervalMs),
				[this] { return static_cast<int64_t>(now); });
	}
	// Every job due, run on this thread (it holds none of the pool's leases).
	size_t run_jobs() { return jobs::run_due(pool, registry); }
	// One pass, then the sweep's schedule and every job due.
	void tick() {
		sup->tick(now);
		scheduler->tick(db, static_cast<int64_t>(now));
		run_jobs();
	}
	// The supervisor's reconciliation (a new supervisor's start), then its jobs.
	void reconcile() {
		sup->reconcile(now);
		run_jobs();
	}
	void advance(uint64_t ms) { now += ms; }
	// Every live machine's agent syncs once (a muted one's does not).
	void sync_all() {
		for (const hosted::FleetMachineRow &m : hosted::list_live_machines(db)) {
			if (muted.count(m.id) != 0 || m.state == hosted::FleetMachineState::Launching) continue;
			auto it = agents.find(m.id);
			if (it == agents.end()) {
				it = agents.emplace(m.id, FakeAgent(m.id)).first;
				it->second.start_running = start_running;
			}
			it->second.sync(db, *sup);
		}
	}
	// A second of the service: a pass, then the agents' syncs.
	void step() {
		tick();
		sync_all();
	}
	std::optional<hosted::HostedServer> server(int64_t id) { return hosted::find_server(db, id); }
	State state(int64_t id) {
		const auto s = server(id);
		return s ? s->state : State::Failed;
	}
	std::optional<hosted::FleetMachineRow> machine(int64_t id) { return hosted::find_machine(db, id); }
	// The machine row the server holds a slot on (0: none).
	int64_t machine_of(int64_t id) {
		const auto s = server(id);
		return s && s->machine_ref ? *s->machine_ref : 0;
	}
	// The server's container's name (the row's container).
	std::string container(int64_t id) {
		const auto s = server(id);
		return s ? s->container : std::string();
	}
	// The agent of the machine the server is on.
	FakeAgent *agent_of(int64_t id) {
		const auto it = agents.find(machine_of(id));
		return it == agents.end() ? nullptr : &it->second;
	}
	// The container the server's agent runs for it, or null.
	const FakeAgent::Container *running_container(int64_t id) {
		FakeAgent *agent = agent_of(id);
		return agent ? agent->find(container(id)) : nullptr;
	}
	// valid_settings on Joint Operations' data set.
	hosted::HostedSettings settings() const { return valid_settings(data.jo); }
	// What a start may pick now (data_set_repository.h).
	std::vector<hosted::OfferedDataSet> offered() { return hosted::offered_data_sets(db); }
	// The request of a new owner an admin granted one server.
	int64_t request(const std::string &owner, std::optional<hosted::HostedSettings> settings = std::nullopt) {
		const int64_t user = make_player(db, owner, 1);
		return user == 0 ? 0 : request_as(user, settings);
	}
	// `user`'s request.
	int64_t request_as(int64_t user, std::optional<hosted::HostedSettings> settings = std::nullopt,
			bool own_machine = false) {
		const auto r = hosted::request_server(db, user, settings ? *settings : this->settings(), offered(),
				config.cap, wall, own_machine, &config.placement);
		return r.outcome == hosted::RequestOutcome::Created ? r.id : 0;
	}
	// Steps until the server is in `want`, `passes` at most; whether it got there.
	bool walk(int64_t id, State want, int passes = 10) {
		for (int i = 0; i < passes && state(id) != want; ++i) step();
		return state(id) == want;
	}
	// A request walked to running, listed under `rid`: its container's name.
	std::string to_running(int64_t id, uint32_t rid) {
		if (!walk(id, State::Listing)) return {};
		const auto s = server(id);
		if (!s || !s->service_user_id || !list_as(db, *s->service_user_id, rid)) return {};
		step(); // -> running
		return state(id) == State::Running ? container(id) : std::string();
	}
	// A crashed server (provisioning, its backoff pending) placed again and
	// walked back to running, listed under `rid` with a key of its new
	// credentials: its new container's name.
	std::string back_to_running(int64_t id, uint32_t rid) {
		advance(config.restart_backoff_ms);
		return to_running(id, rid);
	}
	// A lobby instance that takes clients, one from the rig's start (the
	// supervisor starts no server while none does), seen a day ahead so it stays
	// live however long a test runs (a running lobby moves its row every 5 s).
	void lobby_up() {
		const int64_t id = opennova::lobbydb::register_instance(db, "203.0.113.5:64206");
		db.exec("UPDATE lobby_instances SET last_seen_at = now() + interval '1 day' WHERE id = $1;",
				{ BindValue(id) });
	}
	// No lobby live: every instance's row gone, as a lobby's clean stop leaves it.
	void lobby_down() { db.exec("DELETE FROM lobby_instances;"); }
	// The machine's agent's last report a few minutes old, and silent from now.
	void silence(int64_t machine_id) {
		muted.insert(machine_id);
		db.exec("UPDATE fleet_machines SET last_report_at = now() - interval '5 minutes' WHERE id = $1;",
				{ BindValue(machine_id) });
	}
};

} // namespace hosted_rig
