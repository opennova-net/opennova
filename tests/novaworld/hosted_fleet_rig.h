#pragma once

// The hosted servers' supervisor on a fresh migrated database, a FakeDriver and
// a hand-stepped clock, with each machine's agent played by a FakeAgent
// (hosted_fake_driver.h); and the fixtures hosted_supervisor_test and
// hosted_fleet_test share: player accounts, the billing cache's subscriptions,
// a listing made by hand, the events read back. A step() is what a second of
// the service does: one pass, then one sync of every machine's agent (a muted
// machine's agent is silent).

#include "auth.h"
#include "billing/billing_repository.h"
#include "hosted/fleet_repository.h"
#include "hosted/hosted_repository.h"
#include "hosted/hosted_settings.h"
#include "hosted/supervisor.h"
#include "hosted_fake_driver.h"

#include <net/novaworld/db/database.h>
#include <net/novaworld/host_repository.h>

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

inline hosted::HostedSettings valid_settings() {
	hosted::HostedSettings s;
	s.game = hosted::kDataSetJointOperations;
	s.game_name = "Hosted Test";
	s.missions = { "servetst.bms", "second.bms" };
	return s;
}

// A player with an admin's grant of `comp_servers` hosted servers.
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
	if (comp_servers > 0 && !nws::set_comp_servers(db, created.id, comp_servers).ok) return 0;
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
	std::vector<std::pair<uint32_t, int64_t>> pushes;
	std::map<int64_t, FakeAgent> agents; // by machine row
	std::set<int64_t> muted;             // machines whose agents stay silent
	bool start_running = true;           // a new agent's containers run at once
	uint64_t now = 1000;
	int64_t wall = kWall; // the entitlements' clock, Unix seconds

	explicit Rig(const char *name) : temp(name), pool(scratch.options()), db(scratch.open()) {
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
		config.data_sets = hosted::known_data_sets();
		config.wall_clock_s = [this] { return wall; };
		config.placement.shared_slots = 4;
		config.placement.max_shared = 2;
		config.placement.max_own = 2;
		config.serve_image = kServeImage;
		config.idle_ms = 10 * 60 * 1000;
		config.lost_end_ms = 3 * 60 * 1000;
		config.launch_backoff_ms = 5000;
		make_supervisor();
	}
	void make_supervisor() {
		sup = std::make_unique<hosted::Supervisor>(pool, fake, config);
		sup->set_stop_listing([this](uint32_t rid, int64_t account) {
			pushes.emplace_back(rid, account);
			return true;
		});
	}
	void tick() { sup->tick(now); }
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
	// The request of a new owner an admin granted one server.
	int64_t request(const std::string &owner, hosted::HostedSettings settings = valid_settings()) {
		const int64_t user = make_player(db, owner, 1);
		return user == 0 ? 0 : request_as(user, settings);
	}
	// `user`'s request.
	int64_t request_as(int64_t user, hosted::HostedSettings settings = valid_settings(), bool own_machine = false) {
		const auto r = hosted::request_server(
				db, user, settings, config.data_sets, config.cap, wall, own_machine, &config.placement);
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
	// The machine's agent's last report a few minutes old, and silent from now.
	void silence(int64_t machine_id) {
		muted.insert(machine_id);
		db.exec("UPDATE fleet_machines SET last_report_at = now() - interval '5 minutes' WHERE id = $1;",
				{ BindValue(machine_id) });
	}
};

} // namespace hosted_rig
