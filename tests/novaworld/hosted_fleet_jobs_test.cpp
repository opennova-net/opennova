// The fleet on the job queue (apps/novaworld_common/hosted/fleet_jobs.h): the
// supervisor's pass never calls the machine driver; the job workers do. Over the
// FakeDriver (hosted_fake_driver.h) whose every launch, describe and terminate can
// be made slow, a migrated database and real worker threads:
//
//   - twenty servers started at once, each on a machine of its own: their launches
//     run side by side on the workers, so all twenty boot in about one call's time
//     (twenty calls one after another would take twenty), and no pass waits on one;
//   - a worker gone mid-launch (its call reached the cloud, its finish never came):
//     its lease runs out, the job runs again on another worker, told it is a run
//     again (so in its first run's subnet, though the zones' counts moved, under the
//     same client token: EC2's is one per subnet), with the same machine
//     id, which the cloud answers with the machine it launched (EC2's client token),
//     and one machine results, booting with the row's own bootstrap token;
//   - a launch whose answer could not be written (its mark_launched failed) runs
//     again before the cloud lists the machine its first run launched: its row
//     recorded that run's subnet before the call, so the run again calls there,
//     under the same client token, and one machine results;
//   - a describe hands the driver its machine's launch age by the row (launched_at,
//     the launch's answer by the database's clock), so any worker judges the cloud's
//     launch grace alike;
//   - a driver that stalls: the passes go on, so the stall watchdog (health.h) that
//     watches them never fires;
//   - a launch past the fleet's maximum for its kind (a forged row beside the
//     machine the cap allows) is refused before any cloud call, and its row fails.
//
// Compiles the hosted sources directly, as hosted_supervisor_test does.

#include "health.h"
#include "hosted_fleet_rig.h"

#include "common/test_expect.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace hosted_rig;
using hosted::FleetMachineState;

namespace {

using Clock = std::chrono::steady_clock;

int64_t ms_since(Clock::time_point start) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

jobs::WorkerPoolConfig pool_config(int threads) {
	jobs::WorkerPoolConfig c;
	c.threads = threads;
	c.poll_ms = 20;
	c.idle_max_ms = 50;
	c.keeper_ms = 100;
	return c;
}

// Twenty servers, a machine each, every cloud call 3 s. The pass that launches
// them writes twenty rows and their jobs and is done well inside one call; on 24
// workers the launches overlap, so all twenty machines are booting in about one
// call's time (one after another they would take a minute), and every pass while
// the calls run stays short. Then each server is placed on its own machine.
int test_parallel_launches() {
	Rig rig("fleet_jobs_parallel");
	rig.config.cap = 50;
	rig.config.placement.shared_slots = 1;
	rig.config.placement.max_shared = 20;
	rig.make_supervisor();
	jobs::JobRegistry registry;
	hosted::register_fleet_jobs(
			registry, rig.fake, rig.config.placement, hosted::FleetJobCaps{ 20, 20, 20 });
	std::vector<int64_t> ids;
	for (int i = 0; i < 20; ++i) {
		const int64_t id = rig.request("parallel_" + std::to_string(i));
		TEST_EXPECT(id > 0);
		ids.push_back(id);
	}
	constexpr int kCallMs = 3000;
	rig.fake.call_ms = kCallMs;
	// The passes run on this thread: the driver counts any call made on it.
	rig.fake.supervisor_thread = std::this_thread::get_id();
	rig.sup->tick(rig.now); // -> provisioning
	rig.now += 100;
	// The workers' own database pool, with room for all 24 beside the keeper's.
	opennova::db::ConnectionPool workers_pool(rig.scratch.options(), 26);
	jobs::WorkerPool workers(workers_pool, registry, pool_config(24));
	workers.start();
	TEST_EXPECT(workers.threads() == 24);
	const auto start = Clock::now();
	rig.sup->tick(rig.now); // the twenty rows and their launch jobs
	const int64_t launch_pass = ms_since(start);
	TEST_EXPECT(hosted::list_live_machines(rig.db).size() == 20);
	int64_t longest_pass = 0;
	int passes = 0;
	const auto booting = [&rig] {
		const auto machines = hosted::list_live_machines(rig.db);
		return std::count_if(
				machines.begin(), machines.end(), [](const hosted::FleetMachineRow &m) {
					return m.state == FleetMachineState::Booting;
				});
	};
	while (booting() < 20 && ms_since(start) < 60000) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		// Once the launches land the passes place servers (each a service
		// account's bcrypt hash): only the passes while every call runs are timed.
		if (booting() > 0)
			continue;
		rig.now += 100;
		const auto pass = Clock::now();
		rig.sup->tick(rig.now);
		longest_pass = std::max(longest_pass, ms_since(pass));
		++passes;
	}
	const int64_t launched = ms_since(start);
	std::printf("  20 machines launched in %lld ms (one launch takes %d ms); the launch pass %lld "
				"ms, the "
				"longest of %d passes while they ran %lld ms\n",
			static_cast<long long>(launched), kCallMs, static_cast<long long>(launch_pass), passes,
			static_cast<long long>(longest_pass));
	TEST_EXPECT(booting() == 20);
	TEST_EXPECT(rig.fake.launches == 20 && rig.fake.running() == 20);
	// No pass waited on a call: no launch, describe or terminate ran on the passes'
	// thread (timing alone would be a proxy: a loaded runner stretches a pass, which
	// takes tens of ms, past one call's time), and passes ran while the calls did.
	TEST_EXPECT(rig.fake.calls_on_supervisor_thread.load() == 0);
	TEST_EXPECT(passes >= 3);
	// About one call's time, not twenty (sixty seconds one after another). The
	// bound leaves room for a loaded runner's slow claims and pool start (seven
	// calls' time) while staying a third of the serial time.
	TEST_EXPECT(launched < 7 * kCallMs);
	// Each server placed on its own machine.
	rig.fake.call_ms = 0;
	for (int i = 0; i < 10 &&
			!std::all_of(
					ids.begin(), ids.end(),
					[&rig](int64_t id) { return rig.state(id) == State::Booting; });
			++i) {
		rig.now += 100;
		rig.sup->tick(rig.now);
	}
	workers.stop();
	std::set<int64_t> machines;
	for (const int64_t id : ids) {
		TEST_EXPECT(rig.state(id) == State::Booting);
		machines.insert(rig.machine_of(id));
	}
	TEST_EXPECT(machines.size() == 20 && machines.count(0) == 0);
	return 0;
}

// A worker that claimed a launch and went away after its call reached the cloud:
// the job's lease runs out, the job runs again on a live worker with the same
// machine id as its second attempt, and so in the first call's subnet (a first
// run would take the other now), where the cloud answers it with the machine the
// first call launched. One machine, booting with the row's own bootstrap token;
// the server boots on it.
int test_worker_gone_mid_launch() {
	Rig rig("fleet_jobs_gone_worker");
	const int64_t id = rig.request("gone_worker_owner");
	rig.sup->tick(rig.now); // -> provisioning
	rig.sup->tick(rig.now); // the machine's row and its launch job, together
	const auto rows = hosted::list_live_machines(rig.db);
	TEST_EXPECT(rows.size() == 1 && rows[0].state == FleetMachineState::Launching);
	const int64_t machine = rows[0].id;
	TEST_EXPECT(hosted::machine_launch_pending(rig.db, machine));
	// The worker claims it under a short lease and calls the cloud, then is gone.
	auto claimed = jobs::claim(rig.db, "gone-worker", { hosted::kJobMachineLaunch }, {}, 200);
	TEST_EXPECT(claimed && claimed->secret.size() == hosted::kMachineTokenChars);
	hosted::MachineLaunch call;
	call.machine_id = machine;
	call.kind = hosted::MachineKind::Shared;
	call.slots = rows[0].slots;
	call.bootstrap_token = claimed->secret;
	call.attempt = claimed->attempts;
	std::string error;
	const auto first = rig.fake.launch(call, error);
	TEST_EXPECT(first && rig.fake.launches == 1 && call.attempt == 1);
	// The zones' counts move meanwhile: a first run would now take the other subnet,
	// under its own client token (another machine).
	{
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		rig.fake.subnet = "subnet-b";
	}
	// Its lease runs out; the keeper's reap puts the job back, and a live worker runs it
	// as a run again, in its first run's subnet.
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	TEST_EXPECT(jobs::reap_expired(rig.db) == 1);
	TEST_EXPECT(rig.run_jobs() >= 1);
	TEST_EXPECT(rig.fake.launches == 2 && rig.fake.running() == 1);
	const auto launched = rig.machine(machine);
	TEST_EXPECT(launched && launched->state == FleetMachineState::Booting &&
			launched->cloud_id == first->cloud_id);
	TEST_EXPECT(rig.db.query("SELECT attempts, state, secret IS NULL FROM jobs WHERE id = $1;",
							  { BindValue(claimed->id) })
						.front()
						.as_int(0) == 2);
	// The cloud's machine carries the row's own token: its agent's bootstrap redeems it.
	std::string why;
	TEST_EXPECT(hosted::redeem_bootstrap_token(rig.db, machine,
			rig.fake.machines.at(first->cloud_id).token,
			hosted::agent_secret_digest(std::string(hosted::kAgentSecretChars, 'a')), "fake-agent",
			why));
	// The dead worker's late finish changes nothing.
	TEST_EXPECT(!jobs::complete(rig.db, *claimed));
	TEST_EXPECT(rig.walk(id, State::Listing));
	TEST_EXPECT(rig.machine_of(id) == machine && rig.fake.running() == 1);
	return 0;
}

// A describe hands the driver its machine's launch age as the row dates it
// (launched_at, set when the launch answered, by the database's clock), so a
// describe on any worker, the launch's or another, judges the cloud's launch grace
// alike: the launch's answer starts it, and a launch two minutes old reads two
// minutes.
int test_describe_grace_from_row() {
	Rig rig("fleet_jobs_describe_grace");
	TEST_EXPECT(rig.request("grace_owner") > 0);
	rig.sup->tick(rig.now); // -> provisioning
	rig.sup->tick(rig.now); // the machine's row and its launch job, together
	const auto rows = hosted::list_live_machines(rig.db);
	TEST_EXPECT(rows.size() == 1 && rows[0].state == FleetMachineState::Launching);
	const int64_t machine = rows[0].id;
	// The row was made an hour before its launch answered.
	rig.db.exec("UPDATE fleet_machines SET launched_at = now() - interval '1 hour' WHERE id = $1;",
			{ BindValue(machine) });
	TEST_EXPECT(rig.run_jobs() >= 1);
	const auto launched = rig.machine(machine);
	TEST_EXPECT(launched && launched->state == FleetMachineState::Booting);
	const auto fresh = hosted::launched_ago_ms(rig.db, machine);
	TEST_EXPECT(fresh && *fresh >= 0 && *fresh < 60 * 1000);
	const auto describe_age = [&rig, &launched, machine]() {
		if (hosted::enqueue_machine_describe(rig.db, machine, launched->cloud_id) == 0)
			return int64_t{ -3 };
		rig.run_jobs();
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		return rig.fake.last_launched_ago_ms;
	};
	const int64_t first = describe_age();
	TEST_EXPECT(first >= 0 && first < 60 * 1000);
	rig.db.exec(
			"UPDATE fleet_machines SET launched_at = now() - interval '2 minutes' WHERE id = $1;",
			{ BindValue(machine) });
	const int64_t second = describe_age();
	TEST_EXPECT(second >= 120 * 1000 && second < 180 * 1000);
	TEST_EXPECT(!hosted::launched_ago_ms(rig.db, machine + 1000));
	return 0;
}

// A launch whose answer could not be written: the cloud launched the machine, then
// the row's move to booting failed (the database errored), so the handler throws
// and its job is queued again. Its run again comes before EC2 lists that machine
// (the fake lagging: a describe of its machine id would find none), while the
// zones' counts have moved to the other subnet; its row recorded the first run's
// subnet before that run's call, so it calls there first, under the same client
// token, and the cloud answers with the machine it launched. One machine, booting.
int test_rerun_after_failed_mark() {
	Rig rig("fleet_jobs_rerun_mark");
	TEST_EXPECT(rig.request("rerun_mark_owner") > 0);
	rig.sup->tick(rig.now); // -> provisioning
	rig.sup->tick(rig.now); // the machine's row and its launch job, together
	const auto rows = hosted::list_live_machines(rig.db);
	TEST_EXPECT(rows.size() == 1 && rows[0].state == FleetMachineState::Launching &&
			rows[0].launch_subnet.empty());
	const int64_t machine = rows[0].id;
	// The launch's answer cannot be written: every write of a cloud id fails.
	rig.db.exec("CREATE FUNCTION fail_mark() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
				"RAISE EXCEPTION 'the database went away'; END $$;");
	rig.db.exec("CREATE TRIGGER fail_mark BEFORE UPDATE OF cloud_id ON fleet_machines "
				"FOR EACH ROW EXECUTE FUNCTION fail_mark();");
	TEST_EXPECT(rig.run_jobs() >= 1);
	TEST_EXPECT(rig.fake.launches == 1 && rig.fake.running() == 1);
	const std::string first = rig.fake.last;
	auto row = rig.machine(machine);
	TEST_EXPECT(row && row->state == FleetMachineState::Launching && row->cloud_id.empty() &&
			row->launch_subnet == "subnet-a");
	const auto job = [&rig] {
		return rig.db.query("SELECT attempts, state FROM jobs WHERE kind = $1;",
				{ BindValue(std::string(hosted::kJobMachineLaunch)) });
	};
	TEST_EXPECT(job().size() == 1 && job().front().as_int(0) == 1 &&
			job().front().as_text(1) == "queued");
	// The database answers again; EC2 does not list the first run's machine yet, and a
	// first run would now take the other subnet. The job runs again at once.
	rig.db.exec("DROP TRIGGER fail_mark ON fleet_machines;");
	{
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		rig.fake.subnet = "subnet-b";
		rig.fake.lagging = true;
	}
	rig.db.exec("UPDATE jobs SET run_at = now() WHERE kind = $1;",
			{ BindValue(std::string(hosted::kJobMachineLaunch)) });
	TEST_EXPECT(rig.run_jobs() >= 1);
	TEST_EXPECT(rig.fake.launches == 2 && rig.fake.running() == 1);
	row = rig.machine(machine);
	TEST_EXPECT(row && row->state == FleetMachineState::Booting && row->cloud_id == first &&
			row->launch_subnet == "subnet-a");
	TEST_EXPECT(job().front().as_int(0) == 2 && job().front().as_text(1) == "done");
	return 0;
}

// The cloud stalls (each call 5 s): the passes go on every 50 ms while the workers
// wait in their calls, and the watchdog, watching the passes' turns with a 1.5 s
// stall time, never fires over 3.5 s.
int test_stalled_driver() {
	Rig rig("fleet_jobs_stalled");
	jobs::JobRegistry registry;
	hosted::register_fleet_jobs(registry, rig.fake, rig.config.placement);
	const int64_t a = rig.request("stalled_a");
	const int64_t b = rig.request("stalled_b");
	TEST_EXPECT(a > 0 && b > 0);
	rig.fake.call_ms = 5000;
	nws::Watchdog watchdog(1500);
	hosted::Supervisor *sup = rig.sup.get();
	watchdog.watch("hosted servers' supervisor", [sup] { return sup->pass_turns(); });
	jobs::WorkerPool workers(rig.pool, registry, pool_config(2));
	workers.start();
	const auto start = Clock::now();
	std::string stalled;
	while (ms_since(start) < 3500 && stalled.empty()) {
		rig.sup->tick(rig.now);
		rig.now += 50;
		stalled = watchdog.check();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	const uint64_t turns = sup->pass_turns();
	// A launch went to the stalled cloud meanwhile, and no pass waited on it.
	TEST_EXPECT(stalled.empty() && watchdog.healthy() && turns >= 20);
	TEST_EXPECT(rig.state(a) == State::Provisioning);
	workers.stop();
	TEST_EXPECT(rig.fake.launches >= 1);
	return 0;
}

// The fleet's maximum held by the launch itself: with one shared machine allowed,
// the first row launches; a second shared row (forged, no placement made it) gets
// no cloud call, its row fails as a refused launch, and its job fails for good.
int test_launch_past_cap() {
	Rig rig("fleet_jobs_cap");
	hosted::PlacementPolicy limits = rig.config.placement;
	limits.max_shared = 1;
	jobs::JobRegistry registry;
	hosted::register_fleet_jobs(registry, rig.fake, limits);
	const auto launch_row = [&rig] {
		hosted::NewMachine m;
		m.provider = rig.fake.name();
		m.kind = hosted::MachineKind::Shared;
		m.slots = 4;
		const hosted::CreatedMachine created = hosted::create_machine(rig.db, m);
		hosted::enqueue_machine_launch(
				rig.db, created.id, hosted::MachineKind::Shared, 4, created.bootstrap_token);
		return created.id;
	};
	const int64_t first = launch_row();
	TEST_EXPECT(jobs::run_due(rig.pool, registry) == 1);
	TEST_EXPECT(rig.fake.launches == 1);
	TEST_EXPECT(rig.machine(first)->state == FleetMachineState::Booting);
	const int64_t forged = launch_row();
	jobs::run_due(rig.pool, registry);
	TEST_EXPECT(rig.fake.launches == 1);
	TEST_EXPECT(rig.machine(forged)->state == FleetMachineState::Failed);
	TEST_EXPECT(rig.db.query("SELECT state FROM jobs WHERE kind = 'machine.launch' ORDER BY id DESC "
							 "LIMIT 1;")
						.front()
						.as_text(0)
						.value_or("") == "failed");
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
		{ "parallel_launches", test_parallel_launches },
		{ "worker_gone_mid_launch", test_worker_gone_mid_launch },
		{ "describe_grace_from_row", test_describe_grace_from_row },
		{ "rerun_after_failed_mark", test_rerun_after_failed_mark },
		{ "stalled_driver", test_stalled_driver },
		{ "launch_past_cap", test_launch_past_cap },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the fleet on the job queue\n");
	return 0;
}
