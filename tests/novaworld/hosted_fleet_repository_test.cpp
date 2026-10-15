// The hosted servers' fleet in the database (apps/novaworld_server/hosted/
// fleet_repository.h, backend/migrations/0003_fleet.sql) on a migrated
// database: a machine's row and its one-time token (only its SHA-256 stored),
// the launch's answer, every lifecycle move as a compare-and-set (ready_at,
// the end's time and reason, the credentials spent at the end), the
// bootstrap's redeem (the right token once, a wrong, spent or lapsed one, a
// machine not booting), the agent's secret (its digest alone stored, a wrong
// or ended one refused), a sync's report (a booting machine ready, the slots'
// rows replaced, one per slot, the running one first, a slot past the
// machine's dropped), the silent machines, the addresses a describe writes,
// and the slots: assigned with the generation moving on, never twice to two
// live servers (the index holds it whatever writes the row), let go, and read
// back for placement.

#include "hosted/agent_protocol.h"
#include "hosted/fleet_repository.h"

#include <net/novaworld/db/database.h>

#include "common/test_database.h"
#include "common/test_expect.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace hosted = opennova::novaworld_server::hosted;
using hosted::FleetMachineState;
using hosted::MachineKind;
using opennova::db::BindValue;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

constexpr const char *kImage = "ghcr.io/opennova-net/opennova-serve@sha256:"
							   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

bool is_alnum_text(const std::string &text) {
	for (const char c : text) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!ok)
			return false;
	}
	return !text.empty();
}

std::string column_of(Database &db, int64_t id, const char *column) {
	const auto rows =
			db.query(std::string("SELECT ") + column + " FROM fleet_machines WHERE id = $1;",
					{ BindValue(id) });
	return rows.empty() ? std::string("<no row>") : rows.front().as_text(0).value_or("<null>");
}

hosted::LaunchedMachine launched(const std::string &cloud_id, const std::string &address) {
	hosted::LaunchedMachine out;
	out.cloud_id = cloud_id;
	out.private_address = address;
	out.public_address = "";
	out.instance_type = "t3a.large";
	out.template_version = "lt-0123456789abcdef0:$Default";
	return out;
}

// A booting machine (made, launched) and its token.
hosted::CreatedMachine booting_machine(Database &db, MachineKind kind = MachineKind::Shared,
		int slots = 4, const std::string &cloud_id = "i-0aaaaaaaaaaaaaaa1",
		const std::string &address = "10.44.1.21") {
	hosted::NewMachine m;
	m.provider = "ec2";
	m.kind = kind;
	m.slots = slots;
	m.serve_image = kImage;
	const hosted::CreatedMachine created = hosted::create_machine(db, m);
	hosted::mark_launched(db, created.id, launched(cloud_id, address));
	return created;
}

// A live hosted server row of no account's (so the one-live-per-owner index
// never meets it), in `state`.
int64_t server_row(Database &db, const char *state = "provisioning") {
	const auto rows = db.query(
			"INSERT INTO hosted_servers (state, game_name, max_players, time_limit, kill_limit, "
			"max_score, respawn, start_delay, missions) "
			"VALUES ($1, 'x', 8, 1, 1, 1, 0, 0, 'm.bms') RETURNING id;",
			{ BindValue(std::string(state)) });
	return rows.front().as_int(0).value_or(0);
}

hosted::ContainerReport report(
		const std::string &name, int slot, hosted::ContainerState state, uint64_t egress = 0) {
	hosted::ContainerReport r;
	r.name = name;
	r.slot = slot;
	r.state = state;
	r.started_at = 1760000000;
	r.cpu_percent = 12.5;
	r.memory_bytes = 64ull << 20;
	r.egress_bytes = egress;
	if (state == hosted::ContainerState::Exited)
		r.exit_code = 3;
	return r;
}

// --- the machines -----------------------------------------------------------------

int test_create_and_launch() {
	TestDatabase scratch;
	Database db = scratch.open();
	hosted::NewMachine m;
	m.provider = "ec2";
	m.kind = MachineKind::Shared;
	m.slots = 12;
	m.serve_image = kImage;
	const hosted::CreatedMachine created = hosted::create_machine(db, m);
	TEST_EXPECT(created.id > 0);
	TEST_EXPECT(created.bootstrap_token.size() == hosted::kMachineTokenChars &&
			is_alnum_text(created.bootstrap_token));
	// Only the token's SHA-256 is kept; it lapses kMachineTokenLifetimeS from now.
	TEST_EXPECT(column_of(db, created.id, "bootstrap_token_hash") ==
			hosted::agent_secret_digest(created.bootstrap_token));
	TEST_EXPECT(db.query("SELECT 1 FROM fleet_machines WHERE id = $1 AND bootstrap_expires_at "
						 "BETWEEN now() + interval '29 minutes' AND now() + interval '31 minutes';",
						  { BindValue(created.id) })
						.size() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM fleet_machines WHERE strpos(bootstrap_token_hash || "
						 "agent_secret_hash, $1) > 0;",
						  { BindValue(created.bootstrap_token) })
					.empty());
	auto row = hosted::find_machine(db, created.id);
	TEST_EXPECT(
			row && row->state == FleetMachineState::Launching && row->kind == MachineKind::Shared);
	TEST_EXPECT(row->slots == 12 && row->provider == "ec2" && row->serve_image == kImage);
	TEST_EXPECT(row->cloud_id.empty() && !row->bootstrapped && row->reserved_for == 0);
	TEST_EXPECT(!row->launched_at.empty() && row->ready_at.empty() && row->ended_at.empty());

	TEST_EXPECT(
			hosted::mark_launched(db, created.id, launched("i-0aaaaaaaaaaaaaaa1", "10.44.1.21")));
	row = hosted::find_machine(db, created.id);
	TEST_EXPECT(row->state == FleetMachineState::Booting && row->cloud_id == "i-0aaaaaaaaaaaaaaa1");
	TEST_EXPECT(row->private_address == "10.44.1.21" && row->instance_type == "t3a.large");
	TEST_EXPECT(row->template_version == "lt-0123456789abcdef0:$Default");
	// Once: the row is no longer launching.
	TEST_EXPECT(
			!hosted::mark_launched(db, created.id, launched("i-0bbbbbbbbbbbbbbb2", "10.44.1.22")));
	TEST_EXPECT(hosted::find_machine(db, created.id)->cloud_id == "i-0aaaaaaaaaaaaaaa1");
	const auto by_cloud = hosted::find_machine_by_cloud_id(db, "ec2", "i-0aaaaaaaaaaaaaaa1");
	TEST_EXPECT(by_cloud && by_cloud->id == created.id);
	TEST_EXPECT(!hosted::find_machine_by_cloud_id(db, "local", "i-0aaaaaaaaaaaaaaa1"));
	TEST_EXPECT(!hosted::find_machine_by_cloud_id(db, "ec2", ""));
	TEST_EXPECT(!hosted::find_machine(db, created.id + 100));

	// An own machine is reserved for its server.
	const int64_t server = server_row(db);
	hosted::NewMachine own;
	own.provider = "ec2";
	own.kind = MachineKind::Own;
	own.slots = 1;
	own.serve_image = kImage;
	own.reserved_for = server;
	const hosted::CreatedMachine own_created = hosted::create_machine(db, own);
	row = hosted::find_machine(db, own_created.id);
	TEST_EXPECT(
			row && row->kind == MachineKind::Own && row->reserved_for == server && row->slots == 1);
	// Two machines' tokens differ.
	TEST_EXPECT(own_created.bootstrap_token != created.bootstrap_token);
	// The schema's rules: a slot count of 1..32, a known kind.
	bool refused = false;
	try {
		db.exec("INSERT INTO fleet_machines (provider, kind, slots) VALUES ('ec2', 'shared', 33);");
	} catch (const opennova::db::Error &) {
		refused = true;
	}
	TEST_EXPECT(refused);
	refused = false;
	try {
		db.exec("INSERT INTO fleet_machines (provider, kind, slots) VALUES ('ec2', 'pod', 1);");
	} catch (const opennova::db::Error &) {
		refused = true;
	}
	TEST_EXPECT(refused);
	return 0;
}

int test_transitions() {
	TestDatabase scratch;
	Database db = scratch.open();
	const hosted::CreatedMachine m = booting_machine(db);
	// A move from a state the row is not in writes nothing.
	TEST_EXPECT(!hosted::transition_machine(
			db, m.id, FleetMachineState::Ready, FleetMachineState::Draining, "roll"));
	TEST_EXPECT(!hosted::transition_machine(
			db, m.id, std::vector<FleetMachineState>{}, FleetMachineState::Draining, "roll"));
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Booting, FleetMachineState::Ready, "ignored"));
	auto row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->state == FleetMachineState::Ready && !row->ready_at.empty() &&
			row->end_reason.empty());
	TEST_EXPECT(hosted::transition_machine(db, m.id,
			{ FleetMachineState::Booting, FleetMachineState::Ready }, FleetMachineState::Lost,
			"no report for 61 s"));
	row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->state == FleetMachineState::Lost && row->end_reason == "no report for 61 s");
	// Back to ready (its agent reports again): the reason goes, ready_at stays the first.
	const std::string first_ready = row->ready_at;
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Lost, FleetMachineState::Ready, ""));
	row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->end_reason.empty() && row->ready_at == first_ready);
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Ready, FleetMachineState::Draining, "fleet roll"));
	// An empty reason keeps the last.
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Draining, FleetMachineState::Ending, ""));
	row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->state == FleetMachineState::Ending && row->end_reason == "fleet roll");
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Ending, FleetMachineState::Ended, "idle for 30 minutes"));
	row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->state == FleetMachineState::Ended && !row->ended_at.empty());
	TEST_EXPECT(row->end_reason == "idle for 30 minutes" && !hosted::machine_is_live(row->state));
	// The credentials are spent with the end.
	TEST_EXPECT(column_of(db, m.id, "bootstrap_token_hash").empty());
	TEST_EXPECT(column_of(db, m.id, "agent_secret_hash").empty());
	TEST_EXPECT(column_of(db, m.id, "bootstrap_expires_at") == "<null>");

	// The live list leaves the ended out; the admin's list takes the newest ended after.
	const hosted::CreatedMachine second =
			booting_machine(db, MachineKind::Own, 1, "i-0bbbbbbbbbbbbbbb2", "10.44.1.22");
	hosted::NewMachine failing;
	failing.provider = "ec2";
	failing.slots = 12;
	failing.serve_image = kImage;
	const hosted::CreatedMachine third = hosted::create_machine(db, failing);
	TEST_EXPECT(hosted::transition_machine(db, third.id, FleetMachineState::Launching,
			FleetMachineState::Failed, "run-instances refused"));
	const auto live = hosted::list_live_machines(db);
	TEST_EXPECT(live.size() == 1 && live[0].id == second.id);
	const auto all = hosted::list_machines(db, 1);
	TEST_EXPECT(all.size() == 2 && all[0].id == second.id && all[1].id == third.id);
	TEST_EXPECT(all[1].state == FleetMachineState::Failed &&
			all[1].end_reason == "run-instances refused");
	TEST_EXPECT(hosted::list_machines(db, 5).size() == 3);
	return 0;
}

int test_addresses() {
	TestDatabase scratch;
	Database db = scratch.open();
	const hosted::CreatedMachine m =
			booting_machine(db, MachineKind::Shared, 4, "i-0aaaaaaaaaaaaaaa1", "");
	// The private address only fills an empty one; the public one follows the cloud.
	TEST_EXPECT(hosted::set_machine_addresses(db, m.id, "10.44.1.21", "54.1.2.3"));
	auto row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->private_address == "10.44.1.21" && row->public_address == "54.1.2.3");
	TEST_EXPECT(!hosted::set_machine_addresses(db, m.id, "10.44.1.21", "54.1.2.3"));
	TEST_EXPECT(hosted::set_machine_addresses(db, m.id, "10.44.9.9", "54.1.2.4"));
	row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->private_address == "10.44.1.21" && row->public_address == "54.1.2.4");
	// Not on an ended machine.
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Booting, FleetMachineState::Failed, "boot timeout"));
	TEST_EXPECT(!hosted::set_machine_addresses(db, m.id, "10.44.1.21", "54.1.2.5"));
	return 0;
}

// --- the agent's credentials --------------------------------------------------------

int test_redeem() {
	TestDatabase scratch;
	Database db = scratch.open();
	hosted::NewMachine fresh;
	fresh.provider = "ec2";
	fresh.slots = 4;
	fresh.serve_image = kImage;
	const hosted::CreatedMachine launching = hosted::create_machine(db, fresh);
	const std::string secret = "A1b2C3d4E5f6G7h8I9j0K1l2M3n4O5p6Q7r8S9t0U1v2W3x4";
	const std::string digest = hosted::agent_secret_digest(secret);
	std::string why;
	// A machine the cloud has not answered for yet is not booting.
	TEST_EXPECT(!hosted::redeem_bootstrap_token(
			db, launching.id, launching.bootstrap_token, digest, "v1", why));
	TEST_EXPECT(why.find("launching") != std::string::npos);

	const hosted::CreatedMachine m = booting_machine(db);
	// Not a token this service mints, or not this machine's.
	for (const std::string &bad : { std::string(), m.bootstrap_token.substr(1),
				 m.bootstrap_token.substr(1) + "'", launching.bootstrap_token }) {
		TEST_EXPECT(!hosted::redeem_bootstrap_token(db, m.id, bad, digest, "v1", why));
		TEST_EXPECT(!why.empty() && why.find(m.bootstrap_token) == std::string::npos);
	}
	TEST_EXPECT(
			!hosted::redeem_bootstrap_token(db, m.id + 100, m.bootstrap_token, digest, "v1", why));
	TEST_EXPECT(!hosted::find_machine(db, m.id)->bootstrapped);

	const auto redeemed =
			hosted::redeem_bootstrap_token(db, m.id, m.bootstrap_token, digest, "v1.2", why);
	TEST_EXPECT(redeemed && redeemed->id == m.id && redeemed->bootstrapped);
	TEST_EXPECT(redeemed->agent_version == "v1.2" && redeemed->state == FleetMachineState::Booting);
	TEST_EXPECT(column_of(db, m.id, "agent_secret_hash") == digest);
	TEST_EXPECT(column_of(db, m.id, "bootstrap_token_hash").empty());
	// Spent: once only.
	TEST_EXPECT(!hosted::redeem_bootstrap_token(db, m.id, m.bootstrap_token, digest, "v1", why));
	TEST_EXPECT(why.find("spent") != std::string::npos);

	// A lapsed token.
	const hosted::CreatedMachine late =
			booting_machine(db, MachineKind::Shared, 4, "i-0bbbbbbbbbbbbbbb2", "10.44.1.22");
	db.exec("UPDATE fleet_machines SET bootstrap_expires_at = now() - interval '1 second' WHERE id "
			"= $1;",
			{ BindValue(late.id) });
	TEST_EXPECT(
			!hosted::redeem_bootstrap_token(db, late.id, late.bootstrap_token, digest, "v1", why));
	TEST_EXPECT(why.find("lapsed") != std::string::npos);
	// A failed machine's token is spent with it.
	const hosted::CreatedMachine failed =
			booting_machine(db, MachineKind::Shared, 4, "i-0ccccccccccccccc3", "10.44.1.23");
	TEST_EXPECT(hosted::transition_machine(
			db, failed.id, FleetMachineState::Booting, FleetMachineState::Failed, "boot timeout"));
	TEST_EXPECT(!hosted::redeem_bootstrap_token(
			db, failed.id, failed.bootstrap_token, digest, "v1", why));
	return 0;
}

int test_authenticate() {
	TestDatabase scratch;
	Database db = scratch.open();
	const hosted::CreatedMachine m = booting_machine(db);
	const std::string secret = "A1b2C3d4E5f6G7h8I9j0K1l2M3n4O5p6Q7r8S9t0U1v2W3x4";
	std::string why;
	// No secret yet: nothing authenticates.
	TEST_EXPECT(!hosted::authenticate_agent(db, m.id, secret, why));
	TEST_EXPECT(hosted::redeem_bootstrap_token(
			db, m.id, m.bootstrap_token, hosted::agent_secret_digest(secret), "v1", why));
	auto row = hosted::authenticate_agent(db, m.id, secret, why);
	TEST_EXPECT(row && row->id == m.id && row->state == FleetMachineState::Booting);
	std::string other = secret;
	other[0] = 'B';
	for (const std::string &bad :
			{ other, secret.substr(1), std::string(), secret + "x", m.bootstrap_token }) {
		TEST_EXPECT(!hosted::authenticate_agent(db, m.id, bad, why));
		TEST_EXPECT(why.find(secret) == std::string::npos);
	}
	TEST_EXPECT(!hosted::authenticate_agent(db, m.id + 100, secret, why));
	// Ready, draining and lost machines sync; an ending one no longer.
	for (const auto &[from, to] : std::vector<std::pair<FleetMachineState, FleetMachineState>>{
				 { FleetMachineState::Booting, FleetMachineState::Ready },
				 { FleetMachineState::Ready, FleetMachineState::Lost },
				 { FleetMachineState::Lost, FleetMachineState::Draining } }) {
		TEST_EXPECT(hosted::transition_machine(db, m.id, from, to, "test"));
		row = hosted::authenticate_agent(db, m.id, secret, why);
		TEST_EXPECT(row && row->state == to);
	}
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Draining, FleetMachineState::Ending, ""));
	TEST_EXPECT(!hosted::authenticate_agent(db, m.id, secret, why));
	return 0;
}

// --- the agent's reports ------------------------------------------------------------

int test_record_sync() {
	TestDatabase scratch;
	Database db = scratch.open();
	const hosted::CreatedMachine m = booting_machine(db, MachineKind::Shared, 4);
	using hosted::ContainerState;
	auto row = hosted::record_sync(db, m.id, "v2",
			{ report("srv-7-g1", 0, ContainerState::Running, 1000),
					report("srv-8-g1", 2, ContainerState::Starting) });
	// The first report: ready, with the agent's version and the report's time.
	TEST_EXPECT(row && row->state == FleetMachineState::Ready && !row->ready_at.empty());
	TEST_EXPECT(row->agent_version == "v2" && !row->last_report_at.empty());
	auto slots = hosted::slot_reports(db, m.id);
	TEST_EXPECT(slots.size() == 2 && slots[0].slot == 0 && slots[1].slot == 2);
	TEST_EXPECT(slots[0].container == "srv-7-g1" && slots[0].state == ContainerState::Running);
	TEST_EXPECT(slots[0].egress_bytes == 1000 && slots[0].memory_bytes == (64ull << 20));
	TEST_EXPECT(slots[0].cpu_percent == 12.5 && slots[0].started_at == int64_t{ 1760000000 });
	TEST_EXPECT(!slots[0].exit_code && !slots[0].oom && !slots[0].reported_at.empty());

	// The next replaces them: slot 2 gone, slot 0 exited with its OOM, slot 1 holding two
	// (the running one wins), slot 9 past the machine's dropped.
	hosted::ContainerReport oom = report("srv-7-g1", 0, ContainerState::Exited, 2500);
	oom.oom = true;
	oom.exit_code = 137;
	oom.started_at.reset();
	row = hosted::record_sync(db, m.id, "v2",
			{ oom, report("srv-9-g2", 1, ContainerState::Running, 50),
					report("srv-9-g1", 1, ContainerState::Exited),
					report("srv-10-g1", 9, ContainerState::Running) });
	TEST_EXPECT(row && row->state == FleetMachineState::Ready);
	slots = hosted::slot_reports(db, m.id);
	TEST_EXPECT(slots.size() == 2);
	TEST_EXPECT(slots[0].slot == 0 && slots[0].state == ContainerState::Exited && slots[0].oom);
	TEST_EXPECT(slots[0].exit_code == int64_t{ 137 } && !slots[0].started_at &&
			slots[0].egress_bytes == 2500);
	TEST_EXPECT(slots[1].slot == 1 && slots[1].container == "srv-9-g2");
	// An empty report empties them.
	TEST_EXPECT(hosted::record_sync(db, m.id, "v2", {}));
	TEST_EXPECT(hosted::slot_reports(db, m.id).empty());

	// A lost machine's report is stored and it stays lost (the supervisor decides);
	// an ending one's is refused.
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Ready, FleetMachineState::Lost, "silent"));
	row = hosted::record_sync(db, m.id, "v3", { report("srv-7-g2", 0, ContainerState::Running) });
	TEST_EXPECT(row && row->state == FleetMachineState::Lost && row->agent_version == "v3");
	TEST_EXPECT(hosted::slot_reports(db, m.id).size() == 1);
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Lost, FleetMachineState::Ending, ""));
	TEST_EXPECT(!hosted::record_sync(db, m.id, "v3", {}));
	TEST_EXPECT(hosted::slot_reports(db, m.id).size() == 1);
	// The rows go with their machine.
	db.exec("DELETE FROM fleet_machines WHERE id = $1;", { BindValue(m.id) });
	TEST_EXPECT(hosted::slot_reports(db, m.id).empty());
	return 0;
}

int test_silent_machines() {
	TestDatabase scratch;
	Database db = scratch.open();
	const hosted::CreatedMachine quiet =
			booting_machine(db, MachineKind::Shared, 4, "i-0aaaaaaaaaaaaaaa1", "10.44.1.21");
	const hosted::CreatedMachine talking =
			booting_machine(db, MachineKind::Shared, 4, "i-0bbbbbbbbbbbbbbb2", "10.44.1.22");
	const hosted::CreatedMachine booting =
			booting_machine(db, MachineKind::Shared, 4, "i-0ccccccccccccccc3", "10.44.1.23");
	TEST_EXPECT(hosted::record_sync(db, quiet.id, "v1", {}) &&
			hosted::record_sync(db, talking.id, "v1", {}));
	db.exec("UPDATE fleet_machines SET last_report_at = now() - interval '61 seconds' WHERE id = "
			"$1;",
			{ BindValue(quiet.id) });
	auto silent = hosted::list_silent_machines(db, 60);
	TEST_EXPECT(silent.size() == 1 && silent[0].id == quiet.id);
	// A booting machine is the boot timeout's, never silent.
	(void)booting;
	TEST_EXPECT(hosted::list_silent_machines(db, 120).empty());
	TEST_EXPECT(hosted::transition_machine(
			db, quiet.id, FleetMachineState::Ready, FleetMachineState::Lost, "silent"));
	TEST_EXPECT(hosted::list_silent_machines(db, 60).empty());
	return 0;
}

// --- the slots ------------------------------------------------------------------------

int test_slots() {
	TestDatabase scratch;
	Database db = scratch.open();
	const hosted::CreatedMachine shared = booting_machine(db, MachineKind::Shared, 4);
	const int64_t a = server_row(db);
	const int64_t b = server_row(db, "booting");
	const int64_t c = server_row(db);
	TEST_EXPECT(hosted::assign_slot(db, a, shared.id, 0, 32768) == std::optional<int64_t>(1));
	// One slot per live server: b cannot take a's, a cannot take a second.
	TEST_EXPECT(!hosted::assign_slot(db, b, shared.id, 0, 32768));
	TEST_EXPECT(!hosted::assign_slot(db, a, shared.id, 1, 32769));
	TEST_EXPECT(hosted::assign_slot(db, b, shared.id, 2, 32770) == std::optional<int64_t>(1));
	// Out of the machine's slots, a port out of range, a machine that is no more.
	TEST_EXPECT(!hosted::assign_slot(db, c, shared.id, 4, 32772));
	TEST_EXPECT(!hosted::assign_slot(db, c, shared.id, -1, 32772));
	TEST_EXPECT(!hosted::assign_slot(db, c, shared.id, 1, 70000));
	TEST_EXPECT(!hosted::assign_slot(db, c, shared.id + 100, 1, 32769));
	// The index holds it whatever writes the row.
	bool refused = false;
	try {
		db.exec("UPDATE hosted_servers SET machine_ref = $1, slot = 0 WHERE id = $2;",
				{ BindValue(shared.id), BindValue(c) });
	} catch (const opennova::db::Error &e) {
		refused = e.unique_violation();
	}
	TEST_EXPECT(refused);
	// Inside a caller's transaction a refusal undoes only its own write.
	{
		opennova::db::Transaction tx(db);
		TEST_EXPECT(!hosted::assign_slot(db, c, shared.id, 0, 32768));
		TEST_EXPECT(hosted::assign_slot(db, c, shared.id, 1, 32769) == std::optional<int64_t>(1));
		tx.commit();
	}

	auto holders = hosted::slot_holders(db, shared.id);
	TEST_EXPECT(holders.size() == 3);
	TEST_EXPECT(holders[0].server_id == a && holders[0].slot == 0 && holders[0].game_port == 32768);
	TEST_EXPECT(holders[1].server_id == c && holders[1].slot == 1 && holders[1].generation == 1);
	TEST_EXPECT(holders[2].server_id == b && holders[2].state == hosted::State::Booting);
	// A restart on its slot: a new generation, the slot kept.
	TEST_EXPECT(hosted::next_generation(db, c) == std::optional<int64_t>(2));
	TEST_EXPECT(hosted::slot_holders(db, shared.id)[1].generation == 2);
	TEST_EXPECT(!hosted::next_generation(db, server_row(db)));
	// What the agent reported of a server's slot.
	TEST_EXPECT(hosted::record_sync(db, shared.id, "v1",
			{ report("srv-" + std::to_string(a) + "-g1", 0, hosted::ContainerState::Running,
					777) }));
	const auto of_a = hosted::slot_report_of(db, a);
	TEST_EXPECT(of_a && of_a->slot == 0 && of_a->egress_bytes == 777);
	TEST_EXPECT(!hosted::slot_report_of(db, c) && !hosted::slot_report_of(db, a + 1000));

	// Placement reads every live machine with the slots its live servers hold.
	const hosted::CreatedMachine own =
			booting_machine(db, MachineKind::Own, 1, "i-0bbbbbbbbbbbbbbb2", "10.44.1.22");
	db.exec("UPDATE fleet_machines SET reserved_for = $1 WHERE id = $2;",
			{ BindValue(c), BindValue(own.id) });
	auto machines = hosted::placement_machines(db);
	TEST_EXPECT(machines.size() == 2 && machines[0].id == shared.id && machines[1].id == own.id);
	auto used = machines[0].used_slots;
	std::sort(used.begin(), used.end());
	TEST_EXPECT(used == std::vector<int>({ 0, 1, 2 }) && machines[0].slots == 4);
	TEST_EXPECT(machines[0].state == FleetMachineState::Ready &&
			machines[0].kind == MachineKind::Shared);
	TEST_EXPECT(machines[1].used_slots.empty() && machines[1].reserved_for == c &&
			machines[1].kind == MachineKind::Own);

	// An ended server's slot is free again for placement (its row keeps where it ran).
	db.exec("UPDATE hosted_servers SET state = 'stopped' WHERE id = $1;", { BindValue(a) });
	TEST_EXPECT(hosted::slot_holders(db, shared.id).size() == 2);
	machines = hosted::placement_machines(db);
	used = machines[0].used_slots;
	std::sort(used.begin(), used.end());
	TEST_EXPECT(used == std::vector<int>({ 1, 2 }));
	const int64_t d = server_row(db);
	TEST_EXPECT(hosted::assign_slot(db, d, shared.id, 0, 32768) == std::optional<int64_t>(1));
	TEST_EXPECT(db.query("SELECT machine_ref, slot FROM hosted_servers WHERE id = $1;",
						  { BindValue(a) })
						.front()
						.as_int(1) == int64_t{ 0 });

	// Let go (a restart elsewhere): the generation stays and moves on at the next slot.
	TEST_EXPECT(hosted::release_slot(db, c));
	TEST_EXPECT(!hosted::release_slot(db, c));
	// (1 at its first slot, 2 at its restart there, 3 now.)
	TEST_EXPECT(hosted::assign_slot(db, c, own.id, 0, 32768) == std::optional<int64_t>(3));
	// A machine that no longer takes servers takes no slot.
	TEST_EXPECT(hosted::release_slot(db, b));
	TEST_EXPECT(hosted::transition_machine(
			db, shared.id, FleetMachineState::Ready, FleetMachineState::Draining, "roll"));
	TEST_EXPECT(!hosted::assign_slot(db, b, shared.id, 3, 32771));
	// Ended machines leave placement.
	TEST_EXPECT(hosted::transition_machine(
			db, shared.id, FleetMachineState::Draining, FleetMachineState::Ended, ""));
	machines = hosted::placement_machines(db);
	TEST_EXPECT(machines.size() == 1 && machines[0].id == own.id &&
			machines[0].used_slots == std::vector<int>({ 0 }));
	return 0;
}

int test_schema() {
	TestDatabase scratch;
	Database db = scratch.open();
	for (const char *column : { "machine_ref", "slot", "game_port", "generation" })
		TEST_EXPECT(db.query("SELECT 1 FROM information_schema.columns WHERE table_name = "
							 "'hosted_servers' AND column_name = $1 AND data_type = 'bigint';",
							  { BindValue(std::string(column)) })
							.size() == 1);
	TEST_EXPECT(
			db.query("SELECT 1 FROM pg_indexes WHERE indexname = 'idx_hosted_servers_live_slot';")
					.size() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM information_schema.columns WHERE table_name = "
						 "'fleet_slot_reports' "
						 "AND column_name = 'cpu_percent' AND data_type = 'double precision';")
						.size() == 1);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "create_and_launch", test_create_and_launch },
		{ "transitions", test_transitions },
		{ "addresses", test_addresses },
		{ "redeem", test_redeem },
		{ "authenticate", test_authenticate },
		{ "record_sync", test_record_sync },
		{ "silent_machines", test_silent_machines },
		{ "slots", test_slots },
		{ "schema", test_schema },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted fleet repository\n");
	return 0;
}
