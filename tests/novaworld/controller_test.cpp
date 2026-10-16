// The controller's election (apps/novaworld_controller/controller.h) with
// two controllers on one database, each over a supervisor of its own
// (hosted/supervisor.h) and the FakeDriver (hosted_fake_driver.h), the fleet's
// jobs on real worker threads and a clock in the test's hand:
//
//   - one leads (the generation 1) and alone runs passes; the other waits;
//   - the leader's lock connection killed: it learns it lost, its passes stop
//     (fenced by the generation), and the standby leads within its lock timeout
//     (the generation 2);
//   - the new leader resumes from the rows: a machine's boot timer counts from
//     when the first leader saw it booting, and a kind's launch backoff the first
//     leader set still holds;
//   - a pass an election overtook launches nothing (the fence asked again inside
//     the launch's transaction), and an election waits for a write that holds it;
//   - a console command its controller claimed and never answered: a route with no
//     done row says it was not sent, and the next leader's drain answers it link
//     down.
//
// Compiles the hosted sources directly, as hosted_supervisor_test does.

#include "controller.h"
#include "hosted/console_queue.h"
#include "hosted_fleet_rig.h"

#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace hosted_rig;
using hosted::FleetMachineState;
namespace ctl = opennova::novaworld_server::controller;

namespace {

using Clock = std::chrono::steady_clock;

bool wait_until(const std::function<bool()> &done, int ms) {
	const auto end = Clock::now() + std::chrono::milliseconds(ms);
	while (Clock::now() < end) {
		if (done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return done();
}

// One controller process as the test plays it: its supervisor, its election,
// and whether it learnt it lost.
struct Contender {
	std::unique_ptr<hosted::Supervisor> sup;
	std::unique_ptr<ctl::Controller> leader;
	std::atomic<bool> lost{ false };

	Contender(Rig &rig, const std::string &holder) {
		sup = std::make_unique<hosted::Supervisor>(rig.pool, rig.fake.profile(), rig.config);
		opennova::db::ConnectOptions options = rig.scratch.options();
		// A standby tries for the lock every 0.3 s.
		options.lock_timeout_ms = 300;
		options.statement_timeout_ms = 3000;
		ctl::ControllerConfig config;
		config.holder = holder;
		config.check_ms = 50;
		config.on_lost = [this](const std::string &) { lost.store(true); };
		leader = std::make_unique<ctl::Controller>(options, config);
		ctl::Controller *l = leader.get();
		hosted::Supervisor *s = sup.get();
		s->set_pass_guard([l](opennova::db::Database &db) { return l->still_leads(db); });
		leader->add_part({ "supervisor", [s] { s->start(); }, [s] { s->stop(); } });
	}
};

int test_two_controllers() {
	Rig rig("controller_two");
	std::atomic<uint64_t> clock{ 1000 };
	rig.config.tick_interval_ms = 20;
	rig.config.clock_ms = [&clock] { return clock.load(); };
	rig.config.launch_backoff_ms = 5000;
	rig.fake.boot_timeout = 60000;
	jobs::WorkerPoolConfig worker_config;
	worker_config.threads = 2;
	worker_config.poll_ms = 10;
	worker_config.idle_max_ms = 20;
	jobs::WorkerPool workers(rig.pool, rig.registry, worker_config);
	workers.start();

	Contender a(rig, "controller-a");
	Contender b(rig, "controller-b");
	a.leader->start();
	TEST_EXPECT(wait_until([&a] { return a.leader->leading(); }, 5000));
	b.leader->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(700));
	// One leads, and alone runs passes.
	TEST_EXPECT(a.leader->generation() == 1 && !b.leader->leading());
	TEST_EXPECT(a.sup->pass_turns() > 0 && b.sup->pass_turns() == 0);

	// A server: its machine launched and booting under A (its agent never reports).
	const int64_t shared = rig.request("ctl_shared");
	TEST_EXPECT(shared > 0);
	TEST_EXPECT(wait_until([&rig, shared] { return rig.state(shared) == State::Booting; }, 10000));
	const int64_t machine = rig.machine_of(shared);
	TEST_EXPECT(machine > 0);
	TEST_EXPECT(wait_until(
			[&rig, machine] { return rig.machine(machine)->seen_state == "booting"; }, 5000));
	const int64_t booting_since = rig.machine(machine)->seen_since_ms;
	TEST_EXPECT(booting_since == 1000);
	// An own machine's launch the cloud refuses: A holds that kind's launches for 5 s.
	const int64_t owner = make_player(rig.db, "ctl_own_owner", 0);
	subscribe(rig.db, owner, 1, "active", "sub_ctl_own");
	subscribe_machines(rig.db, owner, 1, "sub_ctl_own_machine");
	rig.fake.fail_next_launch = true;
	const int64_t own = rig.request_as(owner, rig.settings(), true);
	TEST_EXPECT(own > 0);
	TEST_EXPECT(wait_until(
			[&rig] {
				return hosted::launch_not_before_ms(rig.db, hosted::MachineKind::Own) == 6000;
			},
			10000));
	const int launches = rig.fake.launches.load();

	// A's lock connection dies (its machine gone, its process wedged): it learns
	// it lost, and B leads within its lock timeout.
	const auto killed = Clock::now();
	rig.db.query("SELECT pg_terminate_backend($1::integer);",
			{ BindValue(static_cast<int64_t>(a.leader->backend_pid())) });
	TEST_EXPECT(wait_until([&b] { return b.leader->leading(); }, 5000));
	const auto handover =
			std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - killed).count();
	std::printf("  B leads %lld ms after A's lock went\n", static_cast<long long>(handover));
	TEST_EXPECT(b.leader->generation() == 2);
	TEST_EXPECT(wait_until([&a] { return a.lost.load(); }, 5000));
	// A, fenced, runs no pass now (a pass already under way when it learnt it
	// lost ends first); then its process ends.
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	const uint64_t a_turns = a.sup->pass_turns();
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	TEST_EXPECT(a.sup->pass_turns() == a_turns);
	a.leader->stop();
	TEST_EXPECT(wait_until([&b] { return b.sup->pass_turns() > 0; }, 5000));

	// B resumes A's launch backoff from its row: no own machine before 6000.
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	TEST_EXPECT(rig.fake.launches.load() == launches);
	clock.store(6000);
	TEST_EXPECT(wait_until([&rig, launches] { return rig.fake.launches.load() > launches; }, 5000));

	// And A's boot timer: the machine fails 60 s from when A saw it booting, not
	// from B's start.
	clock.store(static_cast<uint64_t>(booting_since) + 60000 - 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Booting);
	clock.store(static_cast<uint64_t>(booting_since) + 60000);
	TEST_EXPECT(wait_until(
			[&rig, machine] { return rig.machine(machine)->state == FleetMachineState::Failed; },
			5000));
	TEST_EXPECT(rig.machine(machine)->end_reason == "its machine did not boot in 60 s");

	b.leader->stop();
	workers.stop();
	return 0;
}

// A leader that stops (its process shutting down) lets the lock go: the standby
// leads at once, the generation moved on.
int test_orderly_handover() {
	Rig rig("controller_handover");
	rig.config.tick_interval_ms = 20;
	Contender a(rig, "controller-a");
	Contender b(rig, "controller-b");
	a.leader->start();
	TEST_EXPECT(wait_until([&a] { return a.leader->leading(); }, 5000));
	b.leader->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(400));
	TEST_EXPECT(!b.leader->leading());
	a.leader->stop();
	TEST_EXPECT(wait_until([&b] { return b.leader->leading(); }, 3000));
	TEST_EXPECT(b.leader->generation() == a.leader->generation() + 1 && !a.lost.load());
	TEST_EXPECT(ctl::Controller::generation_holds(rig.db, b.leader->generation()));
	b.leader->stop();
	return 0;
}

// A pass an election overtook: the generation moves on between the pass's start (its
// guard still held then) and its launch, so the launch writes nothing (no machine row,
// no launch job, no cloud call), and the new leader's pass launches the one machine.
// The fence's share lock, held by a write's transaction, makes an election wait for
// that write.
int test_stale_launch_fenced() {
	Rig rig("controller_stale_launch");
	rig.db.exec("UPDATE controller_leader SET generation = 1 WHERE id = 1;");
	int64_t generation = 1;
	bool elect_after_guard = false;
	rig.sup->set_pass_guard([&rig, &generation, &elect_after_guard](opennova::db::Database &db) {
		const bool holds = ctl::Controller::generation_holds(db, generation);
		if (elect_after_guard) {
			// Another controller's election lands right after this guard.
			elect_after_guard = false;
			rig.db.exec("UPDATE controller_leader SET generation = generation + 1 WHERE id = 1;");
		}
		return holds;
	});
	const int64_t id = rig.request("stale_owner");
	TEST_EXPECT(id > 0);
	rig.tick(); // -> provisioning
	TEST_EXPECT(rig.state(id) == State::Provisioning);
	elect_after_guard = true;
	rig.tick(); // its launch, after the election
	TEST_EXPECT(hosted::list_live_machines(rig.db).empty());
	TEST_EXPECT(
			rig.db.query("SELECT 1 FROM jobs WHERE kind = $1;",
						  { BindValue(std::string(hosted::kJobMachineLaunch)) })
					.empty());
	TEST_EXPECT(rig.fake.launches == 0 && rig.state(id) == State::Provisioning);
	// The new leader's pass launches it.
	generation = 2;
	rig.tick();
	TEST_EXPECT(hosted::list_live_machines(rig.db).size() == 1 && rig.fake.launches == 1);

	// A write holding the fence: an election waits for it (here past its lock timeout).
	{
		opennova::db::Transaction tx(rig.db);
		TEST_EXPECT(ctl::Controller::generation_holds(rig.db, 2));
		opennova::db::ConnectOptions options = rig.scratch.options();
		options.lock_timeout_ms = 200;
		opennova::db::Database election(options);
		bool waited = false;
		try {
			election.exec("UPDATE controller_leader SET generation = generation + 1 WHERE id = 1;");
		} catch (const opennova::db::Error &e) {
			waited = e.lock_timeout();
		}
		TEST_EXPECT(waited);
		tx.commit();
	}
	return 0;
}

// A console command its controller claimed and never answered (it went away with
// it): a route whose wait runs out with no done row says it was not sent; with a new
// leader, its drain answers the earlier generation's claim link down well inside the
// route's wait, and claims what it runs under its own generation.
int test_console_after_handover() {
	Rig rig("controller_console");
	const int64_t server = rig.request("console_owner");
	TEST_EXPECT(server > 0);
	rig.db.exec("UPDATE controller_leader SET generation = 1 WHERE id = 1;");
	hosted::AdminLinks links(&rig.pool);
	hosted::AdminCommandOptions options;
	options.actor = "owner";
	// The route's command claimed by generation 1's drain, gone since.
	const auto claimed_by_gone_controller = [&rig] {
		return wait_until(
				[&rig] {
					rig.db.exec("UPDATE hosted_console_commands SET state = 'running', "
								"claimed_generation = 1 WHERE state = 'queued';");
					return rig.db.changes() > 0;
				},
				5000);
	};
	{
		hosted::AdminCommandResult result;
		std::thread route([&] {
			result = hosted::run_console_command(
					rig.pool, server, "GET GAMESTATE", options, std::chrono::milliseconds(300));
		});
		const bool claimed = claimed_by_gone_controller();
		route.join();
		TEST_EXPECT(
				claimed && result.outcome == hosted::AdminCommandOutcome::TimedOut && !result.sent);
	}
	// Generation 2 leads; its drain starts once the route's command was claimed by 1.
	rig.db.exec("UPDATE controller_leader SET generation = 2 WHERE id = 1;");
	{
		hosted::AdminCommandResult result;
		const auto started = Clock::now();
		std::thread route([&] {
			result = hosted::run_console_command(
					rig.pool, server, "GET GAMESTATE", options, std::chrono::seconds(10));
		});
		const bool claimed = claimed_by_gone_controller();
		hosted::ConsoleDrain drain(rig.pool, links, 5);
		drain.start();
		route.join();
		const auto took =
				std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
						.count();
		// A command it claims itself: under its generation, answered (no link: link down).
		rig.db.exec("INSERT INTO hosted_console_commands (server_id, line, actor, deadline_at) "
					"VALUES ($1, 'GET GAMESTATE', 'owner', now() + interval '10 seconds');",
				{ BindValue(server) });
		const bool answered = wait_until(
				[&rig] {
					return !rig.db.query("SELECT 1 FROM hosted_console_commands WHERE state = "
										 "'done' AND claimed_generation = 2 AND outcome = "
										 "'link_down';")
									.empty();
				},
				5000);
		drain.stop();
		TEST_EXPECT(claimed && result.outcome == hosted::AdminCommandOutcome::LinkDown &&
				!result.sent && took < 5000);
		TEST_EXPECT(answered);
	}
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
		{ "two_controllers", test_two_controllers },
		{ "orderly_handover", test_orderly_handover },
		{ "stale_launch_fenced", test_stale_launch_fenced },
		{ "console_after_handover", test_console_after_handover },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the controller's election\n");
	return 0;
}
