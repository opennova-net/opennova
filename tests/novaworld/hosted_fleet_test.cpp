// The fleet's side of the hosted servers' supervisor (apps/novaworld_common/
// hosted/supervisor.h) on a migrated database, a FakeDriver whose machines'
// agents the test plays (hosted_fake_driver.h, hosted_fleet_rig.h) and a
// hand-stepped clock: the desired state each machine's sync gets (the files
// until the agent reports a container running or exited, nothing for another machine, a
// stopping server's container that never started left unstarted, the drain
// flag, also while a draining machine is lost, a running container answered
// for without files by a supervisor that
// started since), and the machines' lifecycle: a boot that never reports, or
// whose cloud machine vanishes first (failed, a failed launch, its server placed
// again with no restart counted), a lost machine terminated once its
// agent stays silent, an agent that reports again, a shared machine idle past
// its time, a drain moving its servers one at a time with no restart counted, a
// fleet roll draining the machines it names (its plan: each machine kept on the
// pinned image, the rest replaced),
// an admin's end, a held agent's end (every server on it placed again at once,
// never drained), and the reconciliation of the driver's machines with the rows
// (a launch cut short, a cloud machine no row names, a row whose cloud machine
// is gone, a start whose files went with the service, a listing that fails),
// and the periodic sweep (a launch in flight spared). Compiles the hosted sources
// directly.

#include "hosted_fleet_rig.h"

#include "hosted/model/fleet_roll.h"

#include "common/test_expect.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace hosted_rig;
using hosted::FleetMachineState;

namespace {

int test_desired_state() {
	Rig rig("fleet_desired");
	const int64_t a = rig.request("desired_a");
	TEST_EXPECT(rig.walk(a, State::Booting));
	const int64_t machine = rig.machine_of(a);
	FakeAgent &agent = rig.agents.at(machine);
	// The answer that started it carried its files, its slot's user, the pinned
	// image and the slot's limits.
	const FakeAgent::Container *c = agent.find(rig.container(a));
	TEST_EXPECT(c && c->desired.files && c->desired.files->size() == 4);
	TEST_EXPECT(c->desired.files->count("credentials.txt") == 1 && c->desired.files->count("game.cfg") == 1);
	TEST_EXPECT(c->desired.uid == hosted::slot_uid(0) && c->desired.image == kServeImage && c->desired.cpus == 1.0);
	// Its tree, pinned at its start (Joint Operations' current tree, no
	// expansion), beside it in the answer: where the agent fetches it and its
	// manifest, and nothing else's tree.
	TEST_EXPECT(c->desired.base_tree == rig.data.jo_tree && !c->desired.expansion_tree &&
			c->desired.expansion_code.empty());
	TEST_EXPECT(agent.last.trees.size() == 1 && agent.last.trees.count(rig.data.jo_tree) == 1);
	if (agent.last.trees.count(rig.data.jo_tree) == 1) {
		const hosted::TreeSpec &tree = agent.last.trees.at(rig.data.jo_tree);
		TEST_EXPECT(tree.url == tree_url_of(rig.data.jo_tree) && tree.version == "JO V1.7.5.3" &&
				tree.files.size() == 2 && tree.files[0].name == "resource.pff" && tree.files[0].size == 1000 &&
				tree.files[1].name == "version.txt");
	}
	TEST_EXPECT(!agent.last.drain);
	// Reported once, the files are no longer sent (and no longer kept).
	rig.sync_all();
	TEST_EXPECT(agent.last.desired.size() == 1 && !agent.last.desired[0].files);
	rig.sync_all();
	TEST_EXPECT(agent.last.desired.size() == 1 && !agent.last.desired[0].files);

	// Another machine's sync names none of this machine's containers (a dedicated one,
	// so placement never picks it).
	Database *db = &rig.db;
	const auto other = hosted::create_machine(*db, { "fake", hosted::MachineKind::Own, 1, kServeImage, 0 });
	hosted::LaunchedMachine launched;
	launched.cloud_id = "fake-other";
	launched.private_address = "10.0.9.9";
	TEST_EXPECT(hosted::mark_launched(*db, other.id, launched));
	FakeAgent elsewhere(other.id);
	TEST_EXPECT(elsewhere.sync(*db, *rig.sup) && elsewhere.last.desired.empty());

	// A server placed whose agent never reported its container, then stopped:
	// the container stays unstarted, and the stop ends with nothing to stop.
	rig.muted.insert(machine);
	const int64_t b = rig.request("desired_b");
	TEST_EXPECT(rig.walk(b, State::Booting) && rig.machine_of(b) == machine);
	TEST_EXPECT(hosted::request_stop(*db, b, "stopped by its owner"));
	rig.muted.erase(machine);
	rig.sync_all();
	TEST_EXPECT(agent.last.desired.size() == 1 && agent.last.desired[0].name == rig.container(a));
	TEST_EXPECT(agent.find(rig.container(b)) == nullptr);
	rig.tick();
	TEST_EXPECT(rig.state(b) == State::Stopped);

	// A draining machine's answers say so.
	TEST_EXPECT(rig.walk(a, State::Listing));
	TEST_EXPECT(hosted::transition_machine(*db, machine, FleetMachineState::Ready, FleetMachineState::Draining, "test"));
	agent.sync(*db, *rig.sup);
	TEST_EXPECT(agent.last.drain);
	TEST_EXPECT(hosted::transition_machine(*db, machine, FleetMachineState::Draining, FleetMachineState::Ready, ""));

	// A supervisor that started since (the service restarted) answers for the
	// running container without files, with the same arguments.
	const std::vector<std::string> args = agent.find(rig.container(a))->desired.args;
	rig.make_supervisor();
	agent.sync(*db, *rig.sup);
	TEST_EXPECT(agent.last.desired.size() == 1 && !agent.last.desired[0].files);
	TEST_EXPECT(agent.last.desired[0].name == rig.container(a) && agent.last.desired[0].args == args);
	TEST_EXPECT(agent.last.desired[0].image == kServeImage && agent.last.desired[0].uid == hosted::slot_uid(0));
	TEST_EXPECT(!args.empty() && args.back() == "jo");
	// And the pinned tree from the row, with its spec read again.
	TEST_EXPECT(agent.last.desired[0].base_tree == rig.data.jo_tree && agent.last.trees.size() == 1 &&
			agent.last.trees.count(rig.data.jo_tree) == 1);
	return 0;
}

// A machine whose agent never reports: failed at the driver's boot timeout and
// terminated, a failed launch (the heartbeat counts it, and its kind waits out
// the launch backoff); its server, the machine's cause, is placed again with no
// restart counted, on a new machine. One whose cloud machine vanishes before it
// booted is the same, the second failure in a row: its backoff twice as long.
int test_boot_timeout() {
	Rig rig("fleet_boot_timeout");
	rig.fake.boot_timeout = 30000;
	rig.make_supervisor(); // its profile's boot timeout
	const int64_t id = rig.request("boot_owner");
	rig.tick(); // -> provisioning
	rig.tick(); // the launch (its job runs after the pass)
	rig.tick(); // the placement on the launched machine
	const int64_t machine = rig.machine_of(id);
	TEST_EXPECT(machine > 0 && rig.state(id) == State::Booting);
	rig.muted.insert(machine);
	rig.advance(30000 - 1);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Booting && rig.state(id) == State::Booting);
	rig.advance(1);
	rig.step();
	const auto failed = rig.machine(machine);
	TEST_EXPECT(failed->state == FleetMachineState::Failed && contains(failed->end_reason, "did not boot in 30 s"));
	TEST_EXPECT(rig.fake.was_terminated(failed->cloud_id));
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0 && !row->machine_ref);
	TEST_EXPECT(has_event(rig.db, id, "did not boot in 30 s; placed again (no restart counted)"));
	TEST_EXPECT(rig.sup->launch_failures() == 1);
	// The launch backoff holds the next machine of its kind.
	rig.step();
	TEST_EXPECT(rig.fake.launches == 1 && rig.state(id) == State::Provisioning);
	rig.advance(rig.config.launch_backoff_ms);
	rig.tick(); // the launch: its agent has not reported yet
	rig.tick(); // the placement
	TEST_EXPECT(rig.fake.launches == 2 && rig.state(id) == State::Booting);
	const int64_t second = rig.machine_of(id);
	TEST_EXPECT(second != machine);
	// That one's cloud machine vanishes before its agent reports.
	rig.muted.insert(second);
	rig.fake.vanish(rig.machine(second)->cloud_id);
	rig.step(); // its describe finds it gone
	rig.step(); // the pass reads it
	TEST_EXPECT(rig.machine(second)->state == FleetMachineState::Failed &&
			rig.machine(second)->end_reason == "its machine is gone before it booted");
	TEST_EXPECT(rig.server(id)->restarts == 0 && rig.sup->launch_failures() == 2);
	// The second failure in a row: twice the backoff.
	rig.advance(rig.config.launch_backoff_ms);
	rig.step();
	TEST_EXPECT(rig.fake.launches == 2 && rig.state(id) == State::Provisioning);
	rig.advance(rig.config.launch_backoff_ms);
	TEST_EXPECT(rig.walk(id, State::Listing));
	TEST_EXPECT(rig.machine_of(id) != machine && rig.machine_of(id) != second && rig.fake.launches == 3);
	TEST_EXPECT(rig.server(id)->restarts == 0);
	return 0;
}

// A lost machine still running: terminated lost_end_ms after it went lost, and
// its server placed again with no restart counted.
int test_lost_terminated() {
	Rig rig("fleet_lost");
	const int64_t id = rig.request("lost_owner");
	TEST_EXPECT(!rig.to_running(id, 3101).empty());
	const int64_t machine = rig.machine_of(id);
	const std::string cloud = rig.machine(machine)->cloud_id;
	rig.silence(machine);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Lost);
	TEST_EXPECT(contains(rig.machine(machine)->end_reason, "has not reported for 60 s"));
	TEST_EXPECT(hosted::count_machines(rig.db, FleetMachineState::Lost) == 1);
	rig.advance(rig.config.lost_end_ms - 1);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Lost && !rig.fake.was_terminated(cloud));
	TEST_EXPECT(rig.state(id) == State::Running);
	rig.advance(1);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Ended && rig.fake.was_terminated(cloud));
	auto row = rig.server(id);
	TEST_EXPECT(row->restarts == 0 && row->machine_ref != machine);
	TEST_EXPECT(has_event(rig.db, id, "silent for 180 s more; placed again (no restart counted)"));
	TEST_EXPECT(hosted::count_machines(rig.db, FleetMachineState::Lost) == 0);
	return 0;
}

// A lost machine whose agent reports again is ready again (a draining one
// draining again), its server untouched; the cloud not answering decides
// nothing.
int test_lost_reports_again() {
	Rig rig("fleet_lost_again");
	const int64_t id = rig.request("again_owner");
	TEST_EXPECT(!rig.to_running(id, 3201).empty());
	const int64_t machine = rig.machine_of(id);
	rig.silence(machine);
	// The cloud answers nothing from here: each describe since it went lost
	// reads unknown.
	rig.fake.unknown = true;
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Lost);
	TEST_EXPECT(rig.machine(machine)->cloud_state == "unknown");
	rig.advance(rig.config.lost_end_ms);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Lost);
	rig.fake.unknown = false;
	rig.muted.erase(machine);
	rig.sync_all(); // it reports again (a lost machine's sync is still answered)
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Ready);
	TEST_EXPECT(rig.state(id) == State::Running && rig.machine_of(id) == machine && rig.server(id)->restarts == 0);
	return 0;
}

// A draining machine lost and back: its agent is told to drain all along (the
// sync that finds it still lost included), and it drains again.
int test_lost_while_draining() {
	Rig rig("fleet_lost_draining");
	const int64_t id = rig.request("lost_draining_owner");
	TEST_EXPECT(!rig.to_running(id, 3251).empty());
	const int64_t machine = rig.machine_of(id);
	TEST_EXPECT(hosted::transition_machine(rig.db, machine, FleetMachineState::Ready,
			FleetMachineState::Draining, "drained by an admin"));
	rig.silence(machine);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Lost);
	rig.muted.erase(machine);
	rig.sync_all(); // still lost when it syncs
	TEST_EXPECT(rig.agents.at(machine).last.drain);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Draining);
	TEST_EXPECT(rig.agents.at(machine).last.drain);
	return 0;
}

// The mass-lost brake: more than max(2, half) of the live machines lost at once
// is the fleet's fault, so none ends at lost_end_ms; each stays held (the
// heartbeat's machines_held) until it reports again or an admin ends it, the
// brake let go of or not.
int test_mass_lost_brake() {
	Rig rig("fleet_mass_lost");
	rig.config.placement.shared_slots = 1; // a machine per server
	rig.config.placement.max_shared = 4;
	rig.make_supervisor();
	std::vector<int64_t> servers;
	std::vector<int64_t> machines;
	for (const char *owner : { "brake_a", "brake_b", "brake_c" }) {
		const int64_t id = rig.request(owner);
		TEST_EXPECT(!rig.to_running(id, 3300 + static_cast<int>(servers.size())).empty());
		servers.push_back(id);
		machines.push_back(rig.machine_of(id));
	}
	TEST_EXPECT(machines[0] != machines[1] && machines[1] != machines[2]);
	for (const int64_t m : machines)
		rig.silence(m);
	rig.step();
	TEST_EXPECT(hosted::count_machines(rig.db, FleetMachineState::Lost) == 3);
	rig.advance(rig.config.lost_end_ms);
	rig.step();
	// Three of three lost: none ends, each held.
	for (const int64_t m : machines) {
		TEST_EXPECT(rig.machine(m)->state == FleetMachineState::Lost &&
				rig.machine(m)->lost_held_since_ms > 0);
		TEST_EXPECT(!rig.fake.was_terminated(rig.machine(m)->cloud_id));
	}
	TEST_EXPECT(hosted::count_held_machines(rig.db) == 3);
	for (const int64_t id : servers)
		TEST_EXPECT(rig.state(id) == State::Running && rig.server(id)->restarts == 0);
	// One reports again: ready, let go of. The other two stay held, though two of
	// three lost would no longer engage the brake.
	rig.muted.erase(machines[0]);
	rig.sync_all();
	rig.step();
	TEST_EXPECT(rig.machine(machines[0])->state == FleetMachineState::Ready &&
			rig.machine(machines[0])->lost_held_since_ms == 0);
	rig.advance(rig.config.lost_end_ms);
	rig.step();
	TEST_EXPECT(rig.machine(machines[1])->state == FleetMachineState::Lost &&
			rig.machine(machines[2])->state == FleetMachineState::Lost);
	TEST_EXPECT(hosted::count_held_machines(rig.db) == 2);
	// An admin ends one: it ends, its server placed again with no restart counted.
	TEST_EXPECT(hosted::transition_machine(rig.db, machines[1], FleetMachineState::Lost,
			FleetMachineState::Ending, "ended by an admin"));
	rig.step();
	TEST_EXPECT(rig.machine(machines[1])->state == FleetMachineState::Ended &&
			rig.machine_of(servers[1]) != machines[1] && rig.server(servers[1])->restarts == 0);
	TEST_EXPECT(hosted::count_held_machines(rig.db) == 1);
	return 0;
}

// A shared machine with no server on it ends idle_ms after its last server
// left; one with a server stays.
int test_idle_scale_in() {
	Rig rig("fleet_idle");
	const int64_t id = rig.request("idle_owner");
	TEST_EXPECT(!rig.to_running(id, 3301).empty());
	const int64_t machine = rig.machine_of(id);
	const std::string cloud = rig.machine(machine)->cloud_id;
	rig.advance(rig.config.idle_ms);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Ready); // it holds a server
	TEST_EXPECT(hosted::request_stop(rig.db, id, "stopped by its owner"));
	hostdb::remove_host_by_rid(rig.db, 3301);
	rig.step();
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Stopped);
	rig.step(); // its idle time starts
	rig.advance(rig.config.idle_ms - 1);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == FleetMachineState::Ready && !rig.fake.was_terminated(cloud));
	rig.advance(1);
	rig.step();
	const auto ended = rig.machine(machine);
	TEST_EXPECT(ended->state == FleetMachineState::Ended && ended->end_reason == "no server on it for 10 min");
	TEST_EXPECT(rig.fake.was_terminated(cloud) && !ended->ended_at.empty());
	return 0;
}

// An admin's drain (or a fleet roll): the machine's servers move off it one at
// a time, the next once the last moved one runs again elsewhere, no restart
// counted; nothing new lands on it; once it holds none, it ends.
int test_drain_one_at_a_time() {
	Rig rig("fleet_drain");
	const int64_t a = rig.request("drain_a");
	const int64_t b = rig.request("drain_b");
	TEST_EXPECT(!rig.to_running(a, 3401).empty() && !rig.to_running(b, 3402).empty());
	const int64_t old_machine = rig.machine_of(a);
	TEST_EXPECT(rig.machine_of(b) == old_machine);
	Database *db = &rig.db;
	TEST_EXPECT(hosted::transition_machine(*db, old_machine, FleetMachineState::Ready, FleetMachineState::Draining,
			"drained by an admin"));
	rig.step(); // the first moves: a new machine launched for it (the draining one takes nothing)
	rig.step(); // its placement there
	// The first moved, the second stays.
	TEST_EXPECT(rig.machine_of(a) != old_machine && rig.machine_of(a) != 0 && rig.fake.launches == 2);
	TEST_EXPECT(rig.machine_of(b) == old_machine && rig.state(b) == State::Running);
	TEST_EXPECT(has_event(*db, a, "its machine drains; placed again (no restart counted)"));
	TEST_EXPECT(rig.agents.at(old_machine).last.drain);
	TEST_EXPECT(rig.walk(a, State::Listing));
	rig.step();
	TEST_EXPECT(rig.machine_of(b) == old_machine); // the moved one has not run yet
	TEST_EXPECT(list_as(*db, *rig.server(a)->service_user_id, 3403));
	rig.step(); // a runs
	rig.step(); // b moves
	TEST_EXPECT(rig.state(a) == State::Running && rig.machine_of(b) != old_machine);
	TEST_EXPECT(rig.machine_of(b) == rig.machine_of(a) && rig.fake.launches == 2);
	TEST_EXPECT(rig.walk(b, State::Listing));
	TEST_EXPECT(list_as(*db, *rig.server(b)->service_user_id, 3404));
	rig.step();
	rig.step();
	const auto drained = rig.machine(old_machine);
	TEST_EXPECT(drained->state == FleetMachineState::Ended && drained->end_reason == "drained by an admin; drained");
	TEST_EXPECT(rig.server(a)->restarts == 0 && rig.server(b)->restarts == 0);
	return 0;
}

// A machine drained while it boots, its agent never heard from, is never taken
// for lost: its server moves off (no restart counted) and it ends drained.
int test_drain_while_booting() {
	Rig rig("fleet_drain_booting");
	const int64_t id = rig.request("drain_booting_owner");
	rig.tick(); // -> provisioning
	rig.tick(); // the launch (its job runs after the pass)
	rig.tick(); // the placement
	const int64_t machine = rig.machine_of(id);
	TEST_EXPECT(machine > 0 && rig.machine(machine)->state == FleetMachineState::Booting);
	rig.muted.insert(machine);
	Database *db = &rig.db;
	TEST_EXPECT(hosted::transition_machine(
			*db, machine, FleetMachineState::Booting, FleetMachineState::Draining, "drained by an admin"));
	for (int i = 0; i < 6; ++i) {
		rig.advance(1000);
		rig.step();
		TEST_EXPECT(rig.machine(machine)->state != FleetMachineState::Lost);
	}
	const auto drained = rig.machine(machine);
	TEST_EXPECT(drained->state == FleetMachineState::Ended && drained->end_reason == "drained by an admin; drained");
	TEST_EXPECT(rig.machine_of(id) != machine && rig.machine_of(id) != 0 && rig.server(id)->restarts == 0);
	return 0;
}

// A fleet roll (POST /api/admin/fleet/roll: drain_machines): every machine that
// takes servers drains at once, each moving its servers off it to machines
// launched for them (the draining ones count toward no maximum), no restart
// counted, and each old machine ends once it holds none.
int test_roll() {
	Rig rig("fleet_roll");
	rig.config.placement.shared_slots = 1; // a machine per server
	rig.make_supervisor();
	const int64_t a = rig.request("roll_a");
	const int64_t b = rig.request("roll_b");
	TEST_EXPECT(!rig.to_running(a, 3451).empty() && !rig.to_running(b, 3452).empty());
	const int64_t old_a = rig.machine_of(a);
	const int64_t old_b = rig.machine_of(b);
	TEST_EXPECT(old_a != old_b && rig.fake.launches == 2);
	Database *db = &rig.db;
	TEST_EXPECT(hosted::drain_machines(*db, { old_b, old_a, 999999 }, "rolled by an admin") ==
			(std::vector<int64_t>{ old_a, old_b }));
	TEST_EXPECT(hosted::drain_machines(*db, { old_a, old_b }, "rolled by an admin").empty());
	rig.step();
	TEST_EXPECT(rig.fake.launches == 4);
	TEST_EXPECT(rig.machine_of(a) != old_a && rig.machine_of(b) != old_b);
	TEST_EXPECT(rig.walk(a, State::Listing) && rig.walk(b, State::Listing));
	TEST_EXPECT(list_as(*db, *rig.server(a)->service_user_id, 3453) && list_as(*db, *rig.server(b)->service_user_id, 3454));
	rig.step();
	rig.step();
	TEST_EXPECT(rig.state(a) == State::Running && rig.state(b) == State::Running);
	for (const int64_t old : { old_a, old_b }) {
		const auto m = rig.machine(old);
		TEST_EXPECT(m->state == FleetMachineState::Ended && m->end_reason == "rolled by an admin; drained");
	}
	TEST_EXPECT(rig.server(a)->restarts == 0 && rig.server(b)->restarts == 0);
	return 0;
}

// A roll's plan: the live machines that take servers, each kept on the pinned
// image and replaced otherwise (another image, or one not known here; every one
// with no image pinned), with its servers; the others left out.
int test_roll_plan() {
	using hosted::FleetMachineRow;
	const auto row = [](int64_t id, FleetMachineState state) {
		FleetMachineRow m;
		m.id = id;
		m.state = state;
		return m;
	};
	const std::vector<FleetMachineRow> machines = { row(1, FleetMachineState::Ready),
		row(2, FleetMachineState::Ready), row(3, FleetMachineState::Booting),
		row(4, FleetMachineState::Draining), row(5, FleetMachineState::Lost),
		row(6, FleetMachineState::Launching) };
	const std::map<int64_t, std::string> images = { { 1, "ami-new" }, { 2, "ami-old" },
		{ 3, "ami-new" }, { 4, "ami-old" } };
	const std::map<int64_t, int> servers = { { 1, 3 }, { 2, 1 } };
	const auto plan = hosted::plan_fleet_roll(machines, images, servers, "ami-new");
	TEST_EXPECT(plan.size() == 4);
	TEST_EXPECT(plan[0].machine_id == 1 && !plan[0].replace && plan[0].servers == 3 &&
			plan[0].why == "it runs the pinned image");
	TEST_EXPECT(plan[1].machine_id == 2 && plan[1].replace && plan[1].servers == 1 &&
			plan[1].why == "it runs ami-old, not the pinned ami-new");
	TEST_EXPECT(plan[2].machine_id == 3 && !plan[2].replace);
	TEST_EXPECT(plan[3].machine_id == 6 && plan[3].replace && plan[3].image_id.empty() &&
			plan[3].why == "its image is not known here");
	for (const hosted::RollStep &step : hosted::plan_fleet_roll(machines, images, servers, ""))
		TEST_EXPECT(step.replace);
	TEST_EXPECT(hosted::machine_rollable(FleetMachineState::Booting) &&
			!hosted::machine_rollable(FleetMachineState::Draining) &&
			!hosted::machine_rollable(FleetMachineState::Lost));
	return 0;
}

// An admin's end (the row moved to ending): the next pass terminates it and
// places its server again, no restart counted.
int test_admin_end() {
	Rig rig("fleet_end");
	const int64_t id = rig.request("end_owner");
	TEST_EXPECT(!rig.to_running(id, 3501).empty());
	const int64_t machine = rig.machine_of(id);
	const std::string cloud = rig.machine(machine)->cloud_id;
	TEST_EXPECT(hosted::transition_machine(rig.db, machine,
			{ FleetMachineState::Ready, FleetMachineState::Draining }, FleetMachineState::Ending, "ended by an admin"));
	rig.step();
	const auto ended = rig.machine(machine);
	TEST_EXPECT(ended->state == FleetMachineState::Ended && ended->end_reason == "ended by an admin");
	TEST_EXPECT(rig.fake.was_terminated(cloud));
	TEST_EXPECT(rig.server(id)->restarts == 0 && rig.machine_of(id) != machine);
	TEST_EXPECT(has_event(rig.db, id, "ended by an admin; placed again (no restart counted)"));
	return 0;
}

// A machine whose agent has spoken another protocol for 10 minutes, moved to
// ending by its sync (agent_route.h; hosted_agent_route pins the move): the next
// pass ends it as an admin's end, terminated and every server on it placed again
// at once, no restart counted. Never a drain's one at a time: its held agent
// stops none of them, so each would run on it beside its move.
int test_held_agent_end() {
	Rig rig("fleet_held_end");
	const int64_t a = rig.request("held_a");
	const int64_t b = rig.request("held_b");
	TEST_EXPECT(!rig.to_running(a, 3511).empty() && !rig.to_running(b, 3512).empty());
	const int64_t machine = rig.machine_of(a);
	TEST_EXPECT(rig.machine_of(b) == machine);
	const std::string cloud = rig.machine(machine)->cloud_id;
	const std::string why = "its agent speaks protocol " +
			std::to_string(hosted::kAgentProtocol + 1) + ", not " +
			std::to_string(hosted::kAgentProtocol) + " for 10 min";
	TEST_EXPECT(hosted::transition_machine(
			rig.db, machine, FleetMachineState::Ready, FleetMachineState::Ending, why));
	rig.step();
	const auto ended = rig.machine(machine);
	TEST_EXPECT(ended->state == FleetMachineState::Ended && ended->end_reason == why);
	TEST_EXPECT(rig.fake.was_terminated(cloud));
	TEST_EXPECT(rig.machine_of(a) != machine && rig.machine_of(b) != machine);
	TEST_EXPECT(rig.server(a)->restarts == 0 && rig.server(b)->restarts == 0);
	TEST_EXPECT(has_event(rig.db, a, why + "; placed again (no restart counted)") &&
			has_event(rig.db, b, why + "; placed again (no restart counted)"));
	return 0;
}

// The service starts again: a launch it cut short fails (and the cloud machine
// it left is terminated), a cloud machine no row names is terminated, a live
// row whose cloud machine is gone ends, and a server whose start's files went
// with the service (booting, never reported) is placed again with a new
// generation. A driver whose listing fails ends nothing.
int test_reconcile_machines() {
	Rig rig("fleet_reconcile");
	rig.config.placement.shared_slots = 1;
	rig.config.placement.max_shared = 4;
	rig.make_supervisor();
	const int64_t running = rig.request("rec_running");
	TEST_EXPECT(!rig.to_running(running, 3601).empty());
	const int64_t gone = rig.request("rec_gone");
	TEST_EXPECT(!rig.to_running(gone, 3602).empty());
	const int64_t cut = rig.request("rec_cut");
	rig.step();
	rig.step(); // its machine launched
	rig.step(); // placed; its machine's agent then muted before it took the files
	const int64_t cut_machine = rig.machine_of(cut);
	TEST_EXPECT(cut_machine > 0 && rig.state(cut) == State::Booting);
	Database *db = &rig.db;
	// Its agent never took the files: undo what its sync did.
	rig.agents.at(cut_machine).containers.clear();
	db->exec("DELETE FROM fleet_slot_reports WHERE machine_id = $1;", { BindValue(cut_machine) });
	const int64_t cut_generation = rig.server(cut)->generation;
	// A launch the service's end cut short, with the cloud machine it left.
	const auto launching = hosted::create_machine(*db, { "fake", hosted::MachineKind::Shared, 2, kServeImage, 0 });
	const std::string left = rig.fake.adopt(launching.id);
	// A cloud machine no row names.
	const std::string stray = rig.fake.adopt(9999);
	// A row whose cloud machine is gone.
	const int64_t gone_machine = rig.machine_of(gone);
	rig.fake.vanish(rig.machine(gone_machine)->cloud_id);

	// First a driver that cannot list: nothing ended, every row resumes.
	rig.fake.fail_list = true;
	rig.make_supervisor();
	rig.advance(1000);
	rig.reconcile(); // and its sweep job, which fails
	TEST_EXPECT(!rig.fake.was_terminated(stray) && !rig.fake.was_terminated(left));
	TEST_EXPECT(rig.machine(gone_machine)->state == FleetMachineState::Ready);
	TEST_EXPECT(rig.machine(launching.id)->state == FleetMachineState::Failed);

	// Then one that lists.
	rig.fake.fail_list = false;
	rig.make_supervisor();
	rig.advance(1000);
	rig.reconcile(); // and its sweep job
	TEST_EXPECT(rig.fake.was_terminated(stray) && rig.fake.was_terminated(left));
	// The sweep found the row's machine gone; the next pass ends it.
	TEST_EXPECT(rig.machine(gone_machine)->cloud_state == "gone");
	auto row = rig.server(cut);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0 && !row->machine_ref);
	TEST_EXPECT(has_event(*db, cut, "its start was cut short by the service's restart"));
	rig.tick();
	TEST_EXPECT(rig.machine(gone_machine)->state == FleetMachineState::Ended);
	TEST_EXPECT(has_event(*db, gone, "its machine is gone; placed again (no restart counted)"));
	TEST_EXPECT(rig.server(gone)->restarts == 0);
	TEST_EXPECT(rig.state(running) == State::Running);
	TEST_EXPECT(rig.walk(cut, State::Listing));
	TEST_EXPECT(rig.server(cut)->generation == cut_generation + 1);
	TEST_EXPECT(rig.walk(gone, State::Listing));
	return 0;
}

// A server whose container its agent reports starting (it waits for its image or
// its trees, the files in the agent's memory alone) when the service starts again:
// placed again with fresh files and a new generation, no restart counted, since a
// replaced agent could not start it. One reported running resumes.
int test_reconcile_waiting() {
	Rig rig("fleet_reconcile_waiting");
	rig.start_running = false;
	const int64_t waits = rig.request("rec_waits");
	TEST_EXPECT(rig.walk(waits, State::Booting));
	rig.step();
	const int64_t machine = rig.machine_of(waits);
	const FakeAgent::Container *c = rig.agents.at(machine).find(rig.container(waits));
	TEST_EXPECT(c && c->report.state == hosted::ContainerState::Starting);
	// Reported starting, its files are still sent.
	TEST_EXPECT(rig.agents.at(machine).last.desired.size() == 1 &&
			rig.agents.at(machine).last.desired[0].files);
	const int64_t generation = rig.server(waits)->generation;
	rig.make_supervisor();
	rig.advance(1000);
	rig.reconcile();
	TEST_EXPECT(rig.state(waits) == State::Provisioning && rig.server(waits)->restarts == 0);
	TEST_EXPECT(has_event(rig.db, waits, "its start was cut short by the service's restart"));
	rig.start_running = true;
	rig.agents.clear();
	TEST_EXPECT(rig.walk(waits, State::Listing));
	TEST_EXPECT(rig.server(waits)->generation == generation + 1);
	// Running at the next start of the service: it resumes where it was.
	rig.make_supervisor();
	rig.advance(1000);
	rig.reconcile();
	TEST_EXPECT(
			rig.state(waits) == State::Listing && rig.server(waits)->generation == generation + 1);
	return 0;
}

// The periodic sweep: a cloud machine no live row names is ended between the
// boots too, every kFleetSweepIntervalMs; the rows' own machines are kept.
int test_orphan_sweep() {
	Rig rig("fleet_sweep");
	const int64_t id = rig.request("sweep_owner");
	TEST_EXPECT(!rig.to_running(id, 3701).empty());
	const std::string own_cloud = rig.machine(rig.machine_of(id))->cloud_id;
	const std::string stray = rig.fake.adopt(4242);
	rig.step();
	TEST_EXPECT(!rig.fake.was_terminated(stray));
	rig.advance(hosted::kFleetSweepIntervalMs);
	rig.step();
	TEST_EXPECT(rig.fake.was_terminated(stray) && !rig.fake.was_terminated(own_cloud));
	TEST_EXPECT(rig.state(id) == State::Running);
	return 0;
}

// A launch in flight: its cloud machine runs, tagged with its row's id, while the
// row still launches (no cloud id until the launch's answer is written, a retried
// run-instances' worth of seconds). The sweep keeps it; once the answer lands it is
// the row's own, and another cloud machine tagged with that row is an orphan.
int test_sweep_spares_launch_in_flight() {
	Rig rig("fleet_sweep_in_flight");
	const auto launching = hosted::create_machine(
			rig.db, { "fake", hosted::MachineKind::Shared, 2, kServeImage, 0 });
	const std::string in_flight = rig.fake.adopt(launching.id);
	TEST_EXPECT(hosted::enqueue_fleet_sweep(rig.db) != 0);
	rig.run_jobs();
	TEST_EXPECT(!rig.fake.was_terminated(in_flight));
	TEST_EXPECT(rig.machine(launching.id)->state == FleetMachineState::Launching);
	hosted::LaunchedMachine launched;
	launched.cloud_id = in_flight;
	launched.private_address = "10.0.1.99";
	TEST_EXPECT(hosted::mark_launched(rig.db, launching.id, launched));
	const std::string second = rig.fake.adopt(launching.id);
	TEST_EXPECT(hosted::enqueue_fleet_sweep(rig.db) != 0);
	rig.run_jobs();
	TEST_EXPECT(!rig.fake.was_terminated(in_flight) && rig.fake.was_terminated(second));
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
		{ "desired_state", test_desired_state },
		{ "boot_timeout", test_boot_timeout },
		{ "lost_terminated", test_lost_terminated },
		{ "lost_reports_again", test_lost_reports_again },
		{ "lost_while_draining", test_lost_while_draining },
		{ "idle_scale_in", test_idle_scale_in },
		{ "drain_one_at_a_time", test_drain_one_at_a_time },
		{ "drain_while_booting", test_drain_while_booting },
		{ "roll", test_roll },
		{ "roll_plan", test_roll_plan },
		{ "mass_lost_brake", test_mass_lost_brake },
		{ "admin_end", test_admin_end },
		{ "held_agent_end", test_held_agent_end },
		{ "reconcile_machines", test_reconcile_machines },
		{ "reconcile_waiting", test_reconcile_waiting },
		{ "orphan_sweep", test_orphan_sweep },
		{ "sweep_spares_launch_in_flight", test_sweep_spares_launch_in_flight },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the hosted servers' fleet\n");
	return 0;
}
