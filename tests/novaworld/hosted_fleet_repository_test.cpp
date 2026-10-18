// The hosted servers' fleet in the database (apps/novaworld_common/hosted/
// fleet_repository.h, backend/migrations/0003_fleet.sql) on a migrated
// database: a machine's row and its one-time token (only its SHA-256 stored),
// the launch's answer, every lifecycle move as a compare-and-set (ready_at,
// the end's time and reason, the credentials spent at the end), the
// bootstrap's redeem (the right token, again for a fresh secret until the first
// sync spends it, a wrong, spent or lapsed one, a machine not booting), the
// agent's secret (its digest alone stored, a wrong or ended one refused), a
// sync's report (a booting machine ready, the slots' rows replaced, one per
// slot, the running one first, a slot past the machine's dropped; a listing
// that failed storing nothing of them), the silent machines, the addresses a
// describe writes,
// and the slots: assigned with the generation moving on, never twice to two
// live servers (the index holds it whatever writes the row), let go, and read
// back for placement, each assignment a start of its own (hosted_server_starts,
// one left open ended there); each kind's launch backoff (growing per failure
// in a row to its cap, counted once while it holds, cleared by a machine of
// the kind that boots); every move's event, in the move's own statement (the
// making, the launch, the first report, each transition, a drain, a refused
// launch with its reason, the lost machine, a new log stream), none for a move
// refused; what a report says of the machine (its image, log stream, use and
// trees; the slots' waits, network and ticks); and the prune of the machines
// ended 14 days ago (the references a server keeps nulled, one a live server
// names kept, the egress months outliving them).

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
	// Not spent yet: an agent whose reply was lost redeems it again, and the fresh
	// secret replaces the first.
	TEST_EXPECT(column_of(db, m.id, "bootstrap_token_hash") ==
			hosted::agent_secret_digest(m.bootstrap_token));
	const std::string again = hosted::agent_secret_digest(secret + "-again");
	TEST_EXPECT(hosted::redeem_bootstrap_token(db, m.id, m.bootstrap_token, again, "v1.3", why));
	TEST_EXPECT(column_of(db, m.id, "agent_secret_hash") == again);
	// The agent's first sync spends it: once synced, never again.
	TEST_EXPECT(hosted::record_sync(db, m.id, "v1.3", {}));
	TEST_EXPECT(column_of(db, m.id, "bootstrap_token_hash").empty());
	TEST_EXPECT(!hosted::redeem_bootstrap_token(db, m.id, m.bootstrap_token, digest, "v1", why));
	TEST_EXPECT(why.find("spent") != std::string::npos);
	TEST_EXPECT(column_of(db, m.id, "agent_secret_hash") == again);
	// A sync whose listing failed spends it too.
	const hosted::CreatedMachine unlisted =
			booting_machine(db, MachineKind::Shared, 4, "i-0ddddddddddddddd4", "10.44.1.24");
	TEST_EXPECT(hosted::redeem_bootstrap_token(db, unlisted.id, unlisted.bootstrap_token, digest,
			"v1", why));
	TEST_EXPECT(hosted::record_unlisted_sync(db, unlisted.id, "v1"));
	TEST_EXPECT(column_of(db, unlisted.id, "bootstrap_token_hash").empty());
	TEST_EXPECT(!hosted::redeem_bootstrap_token(db, unlisted.id, unlisted.bootstrap_token, digest,
			"v1", why));

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
	TEST_EXPECT(slots[0].net_in_bytes == 0 && !slots[0].tick &&
			slots[1].waiting == hosted::ContainerWait::None);
	// What a starting one waits for, a slot's ingress and a server's ticks ride the rows.
	{
		hosted::ContainerReport ticking = report("srv-7-g1", 0, ContainerState::Running, 1100);
		ticking.net_in_bytes = 4096;
		ticking.tick = hosted::TickHealth{ 1.5, 16.25, 40.0, 7 };
		hosted::ContainerReport waiting = report("srv-8-g1", 2, ContainerState::Starting);
		waiting.started_at.reset();
		waiting.waiting = hosted::ContainerWait::Trees;
		TEST_EXPECT(hosted::record_sync(db, m.id, "v2", { ticking, waiting }));
		slots = hosted::slot_reports(db, m.id);
		TEST_EXPECT(slots.size() == 2 && slots[0].net_in_bytes == 4096 && slots[0].tick &&
				slots[0].tick->p50_ms == 1.5 && slots[0].tick->p99_ms == 16.25 &&
				slots[0].tick->max_ms == 40.0 && slots[0].tick->late == 7);
		TEST_EXPECT(slots[1].waiting == hosted::ContainerWait::Trees && !slots[1].tick);
	}

	// The next replaces them: slot 2 gone, slot 0 exited with its OOM, slot 1 holding two
	// (the running one wins), slot 9 past the machine's dropped.
	hosted::ContainerReport oom = report("srv-7-g1", 0, ContainerState::Exited, 2500);
	oom.oom = true;
	oom.exit_code = 137;
	oom.started_at.reset();
	oom.reason = "its data could not be fetched: aws s3 sync exited 1";
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
	// The reason a container never started rides its row; a running one has none.
	TEST_EXPECT(slots[0].reason == "its data could not be fetched: aws s3 sync exited 1");
	TEST_EXPECT(slots[1].slot == 1 && slots[1].container == "srv-9-g2" && slots[1].reason.empty());
	// A sync whose listing failed: heard from (its version), the slots' rows as they were.
	db.exec("UPDATE fleet_machines SET last_report_at = now() - interval '5 minutes' "
			"WHERE id = $1;",
			{ BindValue(m.id) });
	row = hosted::record_unlisted_sync(db, m.id, "v2.1");
	TEST_EXPECT(row && row->state == FleetMachineState::Ready && row->agent_version == "v2.1");
	TEST_EXPECT(hosted::machine_reported_within(db, m.id, 60));
	TEST_EXPECT(hosted::slot_reports(db, m.id).size() == 2);
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
	TEST_EXPECT(!hosted::record_unlisted_sync(db, m.id, "v3"));
	TEST_EXPECT(hosted::slot_reports(db, m.id).size() == 1);
	// A booting machine heard from with a listing that failed stays booting.
	const hosted::CreatedMachine booting =
			booting_machine(db, MachineKind::Shared, 4, "i-0ddddddddddddddd4", "10.44.1.24");
	row = hosted::record_unlisted_sync(db, booting.id, "v1");
	TEST_EXPECT(row && row->state == FleetMachineState::Booting && row->ready_at.empty());
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
	// A booting machine is the boot timeout's, never silent; and so is one drained
	// while it booted, its agent never heard from.
	TEST_EXPECT(hosted::list_silent_machines(db, 120).empty());
	TEST_EXPECT(hosted::transition_machine(
			db, booting.id, FleetMachineState::Booting, FleetMachineState::Draining, "roll"));
	TEST_EXPECT(hosted::list_silent_machines(db, 120).empty());
	silent = hosted::list_silent_machines(db, 60);
	TEST_EXPECT(silent.size() == 1 && silent[0].id == quiet.id);
	TEST_EXPECT(hosted::transition_machine(
			db, quiet.id, FleetMachineState::Ready, FleetMachineState::Lost, "silent"));
	TEST_EXPECT(hosted::list_silent_machines(db, 60).empty());
	return 0;
}

// The events: every move of a machine, in the statement that makes it.
std::vector<hosted::FleetMachineEvent> events_of(Database &db, int64_t id) {
	return hosted::list_machine_events(db, id, 100);
}

bool event_is(const hosted::FleetMachineEvent &e, const char *from, const char *to,
		const std::string &detail) {
	return e.from_state == from && e.to_state == to && e.detail == detail && !e.at.empty();
}

int test_events() {
	TestDatabase scratch;
	Database db = scratch.open();
	const int64_t server = server_row(db);
	hosted::NewMachine wanted;
	wanted.provider = "ec2";
	wanted.slots = 4;
	wanted.serve_image = kImage;
	wanted.launched_for = server;
	const hosted::CreatedMachine m = hosted::create_machine(db, wanted);
	TEST_EXPECT(hosted::mark_launched(db, m.id, launched("i-0aaaaaaaaaaaaaaa1", "10.44.1.21")));
	TEST_EXPECT(hosted::record_sync(db, m.id, "v1", {}));
	// A second report is no move.
	TEST_EXPECT(hosted::record_sync(db, m.id, "v1", {}));
	TEST_EXPECT(hosted::mark_machine_lost(db, m.id, "its agent has not reported for 60 s", 1000));
	TEST_EXPECT(hosted::transition_machine(db, m.id, FleetMachineState::Lost,
			FleetMachineState::Ready, "its agent reports again"));
	// A move refused writes nothing; an empty reason keeps the end reason but the
	// event says nothing more.
	TEST_EXPECT(!hosted::transition_machine(
			db, m.id, FleetMachineState::Booting, FleetMachineState::Ready, "never"));
	TEST_EXPECT(hosted::drain_machines(db, { m.id }, "fleet roll") == std::vector<int64_t>{ m.id });
	TEST_EXPECT(hosted::transition_machine(db, m.id,
			{ FleetMachineState::Ready, FleetMachineState::Draining }, FleetMachineState::Ending,
			""));
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Ending, FleetMachineState::Ended, "drained"));
	const auto e = events_of(db, m.id);
	TEST_EXPECT(e.size() == 8);
	if (e.size() == 8) {
		TEST_EXPECT(
				event_is(e[0], "", "launching", "launched for server " + std::to_string(server)));
		TEST_EXPECT(event_is(
				e[1], "launching", "booting", "launched as i-0aaaaaaaaaaaaaaa1 (t3a.large)"));
		TEST_EXPECT(event_is(e[2], "booting", "ready", "its agent reports"));
		TEST_EXPECT(event_is(e[3], "ready", "lost", "its agent has not reported for 60 s"));
		TEST_EXPECT(event_is(e[4], "lost", "ready", "its agent reports again"));
		TEST_EXPECT(event_is(e[5], "ready", "draining", "fleet roll"));
		TEST_EXPECT(event_is(e[6], "draining", "ending", ""));
		TEST_EXPECT(event_is(e[7], "ending", "ended", "drained"));
		TEST_EXPECT(e[0].id < e[7].id && e[0].machine_id == m.id);
	}
	// The newest `limit`, oldest first.
	const auto last_two = hosted::list_machine_events(db, m.id, 2);
	TEST_EXPECT(last_two.size() == 2 && last_two[0].to_state == "ending" &&
			last_two[1].to_state == "ended");

	// A draining machine that goes lost names the state it left; a refused launch
	// its reason.
	const hosted::CreatedMachine d =
			booting_machine(db, MachineKind::Shared, 4, "i-0bbbbbbbbbbbbbbb2");
	TEST_EXPECT(hosted::record_sync(db, d.id, "v1", {}));
	TEST_EXPECT(hosted::transition_machine(
			db, d.id, FleetMachineState::Ready, FleetMachineState::Draining, "an admin's drain"));
	TEST_EXPECT(hosted::mark_machine_lost(db, d.id, "silent", 1000));
	TEST_EXPECT(event_is(events_of(db, d.id).back(), "draining", "lost", "silent"));
	hosted::NewMachine refused;
	refused.provider = "ec2";
	refused.kind = MachineKind::Own;
	refused.serve_image = kImage;
	const int64_t r = hosted::create_machine(db, refused).id;
	TEST_EXPECT(hosted::fail_refused_launch(db, r, "the launch failed (VcpuLimitExceeded)"));
	TEST_EXPECT(!hosted::fail_refused_launch(db, r, "again"));
	const auto re = events_of(db, r);
	TEST_EXPECT(re.size() == 2 && event_is(re[0], "", "launching", "") &&
			event_is(re[1], "launching", "failed", "the launch failed (VcpuLimitExceeded)"));
	TEST_EXPECT(hosted::find_machine(db, r)->end_reason == "the launch failed (VcpuLimitExceeded)");
	const auto taken = hosted::take_refused_launches(db);
	TEST_EXPECT(taken.size() == 1 && taken[0].reason == "the launch failed (VcpuLimitExceeded)");
	// They go with their machine.
	db.exec("DELETE FROM fleet_machines WHERE id = $1;", { BindValue(r) });
	TEST_EXPECT(events_of(db, r).empty());
	return 0;
}

// What a report says of the machine itself: its image and the agent's log stream
// (each kept when a report names none; a new stream an event), its use with its
// time, its trees replaced each report; nothing for a machine that no longer syncs.
int test_machine_report() {
	TestDatabase scratch;
	Database db = scratch.open();
	const hosted::CreatedMachine m = booting_machine(db, MachineKind::Shared, 4);
	TEST_EXPECT(hosted::record_sync(db, m.id, "v1", {}));
	auto row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row && row->image_id.empty() && row->agent_log_stream.empty() && !row->stats &&
			row->stats_at.empty());
	hosted::SyncRequest report;
	report.agent_version = "v1";
	report.image_id = "ami-0123456789abcdef0";
	report.log_stream = "hostd/hostd/0123456789abcdef";
	hosted::MachineStats stats;
	stats.cpu_percent = 42.5;
	stats.memory_used_bytes = 1ull << 30;
	stats.memory_total_bytes = 4ull << 30;
	stats.disk_free_bytes = 10ull << 30;
	stats.disk_total_bytes = 20ull << 30;
	stats.net_in_bytes = 1000;
	stats.net_out_bytes = 2000;
	report.machine = stats;
	hosted::TreeReport ready;
	ready.state = hosted::TreeReportState::Ready;
	ready.bytes = 2147483648ull;
	ready.tries = 1;
	hosted::TreeReport fetching;
	fetching.bytes = 5000;
	fetching.tries = 2;
	fetching.reason = "aws s3 sync exited 1";
	report.trees = { { 7, ready }, { 8, fetching } };
	TEST_EXPECT(hosted::record_machine_report(db, m.id, report));
	row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->image_id == "ami-0123456789abcdef0" &&
			row->agent_log_stream == "hostd/hostd/0123456789abcdef");
	TEST_EXPECT(row->stats && row->stats->cpu_percent == 42.5 &&
			row->stats->memory_used_bytes == (1ull << 30) &&
			row->stats->memory_total_bytes == (4ull << 30) &&
			row->stats->disk_free_bytes == (10ull << 30) &&
			row->stats->disk_total_bytes == (20ull << 30) && row->stats->net_in_bytes == 1000 &&
			row->stats->net_out_bytes == 2000 && !row->stats_at.empty());
	auto trees = hosted::machine_trees(db, m.id);
	TEST_EXPECT(trees.size() == 2 && trees[0].tree_id == 7 &&
			trees[0].state == hosted::TreeReportState::Ready && trees[0].bytes == 2147483648ull &&
			trees[0].tries == 1 && trees[0].error.empty() && !trees[0].reported_at.empty());
	TEST_EXPECT(trees[1].tree_id == 8 && trees[1].state == hosted::TreeReportState::Fetching &&
			trees[1].bytes == 5000 && trees[1].tries == 2 &&
			trees[1].error == "aws s3 sync exited 1");
	// The new stream is an event; the same one again is none.
	auto e = events_of(db, m.id);
	TEST_EXPECT(event_is(e.back(), "ready", "ready", "log stream hostd/hostd/0123456789abcdef"));
	const size_t events = e.size();
	TEST_EXPECT(hosted::record_machine_report(db, m.id, report));
	TEST_EXPECT(events_of(db, m.id).size() == events);
	// A report with no use, no image and no stream keeps the last of each; its
	// trees replace the last.
	hosted::SyncRequest bare;
	bare.agent_version = "v1";
	bare.trees = { { 9, ready } };
	TEST_EXPECT(hosted::record_machine_report(db, m.id, bare));
	row = hosted::find_machine(db, m.id);
	TEST_EXPECT(row->image_id == "ami-0123456789abcdef0" &&
			row->agent_log_stream == "hostd/hostd/0123456789abcdef" && row->stats &&
			row->stats->cpu_percent == 42.5);
	trees = hosted::machine_trees(db, m.id);
	TEST_EXPECT(trees.size() == 1 && trees[0].tree_id == 9);
	// A restarted agent's new task: a new stream, a new event.
	report.log_stream = "hostd/hostd/fedcba9876543210";
	TEST_EXPECT(hosted::record_machine_report(db, m.id, report));
	TEST_EXPECT(event_is(events_of(db, m.id).back(), "ready", "ready",
			"log stream hostd/hostd/fedcba9876543210"));
	// An ending machine's report is refused, its rows as they were.
	TEST_EXPECT(hosted::transition_machine(
			db, m.id, FleetMachineState::Ready, FleetMachineState::Ending, ""));
	report.trees.clear();
	TEST_EXPECT(!hosted::record_machine_report(db, m.id, report));
	TEST_EXPECT(hosted::machine_trees(db, m.id).size() == 2);
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
	// The assignment is a start of its own: its machine and slot, open.
	const auto starts = hosted::list_starts(db, a, 10);
	TEST_EXPECT(starts.size() == 1 && starts[0].generation == 1 &&
			starts[0].machine_id == shared.id && starts[0].slot == 0 &&
			!starts[0].placed_at.empty() && starts[0].ended_at.empty() && !starts[0].cause);
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
	// (1 at its first slot, 2 now.) Its first start, left open, ends there.
	TEST_EXPECT(hosted::assign_slot(db, c, own.id, 0, 32768) == std::optional<int64_t>(2));
	auto c_starts = hosted::list_starts(db, c, 10);
	TEST_EXPECT(c_starts.size() == 2 && c_starts[0].generation == 2 &&
			c_starts[0].machine_id == own.id && c_starts[0].ended_at.empty() &&
			c_starts[1].generation == 1 && !c_starts[1].ended_at.empty() &&
			c_starts[1].cause == hosted::StartCause::Fleet);
	// What it runs, its first report running, a cause asked ahead (a stop for its
	// owner's limits) standing at its end, and the end once alone.
	hosted::set_start_files(db, c, 2, kImage, 7, 8, std::to_string(c) + "/g2");
	hosted::mark_start_running(db, c, 2);
	hosted::set_start_cause(db, c, hosted::StartCause::Limits);
	TEST_EXPECT(hosted::end_start(
			db, c, 2, hosted::StartCause::Stop, { 3, false, "the session ended: x" }));
	TEST_EXPECT(!hosted::end_start(db, c, 2, hosted::StartCause::Crash));
	c_starts = hosted::list_starts(db, c, 1);
	TEST_EXPECT(c_starts.size() == 1 && c_starts[0].serve_image == kImage &&
			c_starts[0].base_tree_id == 7 && c_starts[0].expansion_tree_id == 8 &&
			c_starts[0].log_stream == std::to_string(c) + "/g2" &&
			!c_starts[0].running_at.empty() && !c_starts[0].ended_at.empty() &&
			c_starts[0].cause == hosted::StartCause::Limits && c_starts[0].exit_code == 3 &&
			c_starts[0].reason == "the session ended: x");
	for (const hosted::StartCause cause : { hosted::StartCause::Crash, hosted::StartCause::Fleet,
				 hosted::StartCause::Stop, hosted::StartCause::Update, hosted::StartCause::Limits,
				 hosted::StartCause::NoData, hosted::StartCause::Timeout,
				 hosted::StartCause::LinkLost })
		TEST_EXPECT(hosted::parse_start_cause(hosted::start_cause_text(cause)) == cause);
	TEST_EXPECT(!hosted::parse_start_cause("restart"));
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

// --- each kind's launch backoff -------------------------------------------------------

// The wait doubles per failure in a row from its base (1, 2, 4, 8 ... minutes)
// to its cap (30); a failure while the backoff holds (of a launch made before
// it began) changes nothing; each kind has its own; a machine of the kind that
// boots (its agent's first sync) clears that kind's, and a later sync of a
// ready machine clears nothing.
int test_launch_backoff() {
	constexpr uint64_t kMin = 60 * 1000;
	constexpr uint64_t kCap = 30 * kMin;
	const uint64_t waits[] = { 1, 2, 4, 8, 16, 30, 30 };
	for (int64_t n = 1; n <= 7; ++n)
		TEST_EXPECT(hosted::launch_backoff_wait_ms(n, kMin, kCap) == waits[n - 1] * kMin);
	TEST_EXPECT(hosted::launch_backoff_wait_ms(5000, kMin, kCap) == kCap);

	TestDatabase scratch;
	Database db = scratch.open();
	TEST_EXPECT(hosted::launch_not_before_ms(db, MachineKind::Shared) == 0);
	int64_t now = 1760000000000;
	auto b = hosted::note_launch_failure(db, MachineKind::Shared, now, kMin, kCap);
	TEST_EXPECT(b && b->failures == 1 && b->not_before_ms == now + static_cast<int64_t>(kMin));
	// Another refusal inside it, of a launch made before it began: nothing changes.
	TEST_EXPECT(!hosted::note_launch_failure(db, MachineKind::Shared, now + 59999, kMin, kCap));
	TEST_EXPECT(hosted::launch_backoff(db, MachineKind::Shared).failures == 1);
	TEST_EXPECT(hosted::launch_not_before_ms(db, MachineKind::Shared) == now + 60000);
	// Each one after it doubles the next, to the cap.
	for (int i = 1; i < 7; ++i) {
		now = hosted::launch_not_before_ms(db, MachineKind::Shared);
		b = hosted::note_launch_failure(db, MachineKind::Shared, now, kMin, kCap);
		TEST_EXPECT(b && b->failures == i + 1 &&
				b->not_before_ms == now + static_cast<int64_t>(waits[i] * kMin));
	}
	// The own machines' is their own.
	TEST_EXPECT(hosted::launch_not_before_ms(db, MachineKind::Own) == 0);
	b = hosted::note_launch_failure(db, MachineKind::Own, now, kMin, kCap);
	TEST_EXPECT(b && b->failures == 1);
	// A shared machine boots: the shared kind's backoff is over, the own one's holds.
	const hosted::CreatedMachine m = booting_machine(db, MachineKind::Shared, 4);
	TEST_EXPECT(hosted::record_sync(db, m.id, "v1", {}));
	const hosted::LaunchBackoff shared = hosted::launch_backoff(db, MachineKind::Shared);
	TEST_EXPECT(shared.failures == 0 && shared.not_before_ms == 0);
	TEST_EXPECT(hosted::launch_backoff(db, MachineKind::Own).failures == 1);
	// The next failure starts from the base again; the ready machine's next sync is
	// no boot and clears nothing.
	b = hosted::note_launch_failure(db, MachineKind::Shared, now, kMin, kCap);
	TEST_EXPECT(b && b->failures == 1 && b->not_before_ms == now + static_cast<int64_t>(kMin));
	TEST_EXPECT(hosted::record_sync(db, m.id, "v1", {}));
	TEST_EXPECT(hosted::launch_backoff(db, MachineKind::Shared).failures == 1);
	return 0;
}

// The prune of the ended machines: one ended or failed more than 14 days ago
// goes with its slot reports (its egress months stay, to their own 13 months),
// a stopped server that last ran on it left with no machine (machine_ref
// nulled); a younger one, a live one and one a live server still names stay,
// the last until that server stops.
int test_prune_ended_machines() {
	TestDatabase scratch;
	Database db = scratch.open();
	const auto ended_at = [&db](int64_t id, const char *ago) {
		db.exec("UPDATE fleet_machines SET ended_at = now() - $2::interval WHERE id = $1;",
				{ BindValue(id), BindValue(std::string(ago)) });
	};
	// A launch refused 15 days ago.
	hosted::NewMachine refused;
	refused.provider = "ec2";
	refused.kind = MachineKind::Own;
	refused.serve_image = kImage;
	const int64_t old_failed = hosted::create_machine(db, refused).id;
	TEST_EXPECT(hosted::fail_refused_launch(db, old_failed, "the launch failed"));
	ended_at(old_failed, "15 days");
	// One that ran a server (its slot report, its month's egress) and ended 20 days ago.
	const int64_t old_ended = booting_machine(db, MachineKind::Shared, 4, "i-0eeeeeeeeeeeeeee1").id;
	TEST_EXPECT(hosted::record_sync(
			db, old_ended, "v1", { report("srv-7-g1", 0, hosted::ContainerState::Running, 100) }));
	db.exec("INSERT INTO fleet_machine_egress (machine_id, month, egress_bytes) VALUES ($1, "
			"CAST(date_trunc('month', now()) AS date), 100);",
			{ BindValue(old_ended) });
	const int64_t stopped = server_row(db, "stopped");
	db.exec("UPDATE hosted_servers SET machine_ref = $2, slot = 0 WHERE id = $1;",
			{ BindValue(stopped), BindValue(old_ended) });
	TEST_EXPECT(hosted::transition_machine(
			db, old_ended, FleetMachineState::Ready, FleetMachineState::Ended, "idle"));
	ended_at(old_ended, "20 days");
	// Ended 13 days ago; ready a month; ended 20 days ago with a live server on it.
	const int64_t young = booting_machine(db, MachineKind::Shared, 4, "i-0eeeeeeeeeeeeeee2").id;
	TEST_EXPECT(hosted::transition_machine(
			db, young, FleetMachineState::Booting, FleetMachineState::Ended, "idle"));
	ended_at(young, "13 days");
	const int64_t live = booting_machine(db, MachineKind::Shared, 4, "i-0eeeeeeeeeeeeeee3").id;
	TEST_EXPECT(hosted::record_sync(db, live, "v1", {}));
	db.exec("UPDATE fleet_machines SET launched_at = now() - interval '30 days' WHERE id = $1;",
			{ BindValue(live) });
	const int64_t named = booting_machine(db, MachineKind::Shared, 4, "i-0eeeeeeeeeeeeeee4").id;
	TEST_EXPECT(hosted::transition_machine(
			db, named, FleetMachineState::Booting, FleetMachineState::Failed, "gone"));
	ended_at(named, "20 days");
	const int64_t running = server_row(db, "running");
	db.exec("UPDATE hosted_servers SET machine_ref = $2, slot = 1 WHERE id = $1;",
			{ BindValue(running), BindValue(named) });

	TEST_EXPECT(hosted::prune_ended_machines(db) == 2);
	TEST_EXPECT(!hosted::find_machine(db, old_failed) && !hosted::find_machine(db, old_ended));
	TEST_EXPECT(hosted::find_machine(db, young) && hosted::find_machine(db, live) &&
			hosted::find_machine(db, named));
	TEST_EXPECT(hosted::slot_reports(db, old_ended).empty());
	TEST_EXPECT(db.query("SELECT 1 FROM fleet_machine_egress WHERE machine_id = $1;",
						  { BindValue(old_ended) })
						.size() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM hosted_servers WHERE id = $1 AND machine_ref IS NULL;",
						  { BindValue(stopped) })
						.size() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM hosted_servers WHERE id = $1 AND machine_ref = $2;",
						  { BindValue(running), BindValue(named) })
						.size() == 1);
	// Its server stopped, the next prune takes it; a shorter keep the younger one.
	db.exec("UPDATE hosted_servers SET state = 'stopped' WHERE id = $1;", { BindValue(running) });
	TEST_EXPECT(hosted::prune_ended_machines(db) == 1 && !hosted::find_machine(db, named));
	TEST_EXPECT(hosted::prune_ended_machines(db, 12 * 24 * 60 * 60) == 1);
	TEST_EXPECT(!hosted::find_machine(db, young) && hosted::find_machine(db, live));
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
		{ "events", test_events },
		{ "machine_report", test_machine_report },
		{ "silent_machines", test_silent_machines },
		{ "slots", test_slots },
		{ "launch_backoff", test_launch_backoff },
		{ "prune_ended_machines", test_prune_ended_machines },
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
