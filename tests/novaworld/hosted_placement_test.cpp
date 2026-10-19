// Where a hosted server runs (apps/novaworld_common/hosted/model/placement.h) and what a
// slot is (hosted/protocol/slots.h): best fit on the fullest shared machine that takes servers,
// its lowest free slot, the ties (the readier machine, then the older), the launch of a
// new shared machine below ONNET_FLEET_MAX_SHARED and the wait at it, a dedicated
// machine's reserved machine and its maximum, the states that take servers and
// count toward the maximums (draining and ending do neither), the columns' words and
// the policy's checks, and a request placed after the servers waiting before it (the
// start request's at_capacity). Pure functions: no database, no cloud.

#include "hosted/model/placement.h"
#include "hosted/protocol/slots.h"

#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace hosted = opennova::novaworld_server::hosted;
using hosted::FleetMachine;
using hosted::FleetMachineState;
using hosted::MachineKind;
using hosted::Placement;
using hosted::PlacementPolicy;
using hosted::PlacementRequest;

namespace {

FleetMachine shared(int64_t id, FleetMachineState state, int slots, std::vector<int> used) {
	FleetMachine m;
	m.id = id;
	m.kind = MachineKind::Shared;
	m.state = state;
	m.slots = slots;
	m.used_slots = std::move(used);
	return m;
}

FleetMachine own(
		int64_t id, FleetMachineState state, int64_t reserved_for, std::vector<int> used = {}) {
	FleetMachine m;
	m.id = id;
	m.kind = MachineKind::Own;
	m.state = state;
	m.slots = 1;
	m.used_slots = std::move(used);
	m.reserved_for = reserved_for;
	return m;
}

PlacementRequest shared_request(int64_t server) {
	PlacementRequest r;
	r.server_id = server;
	return r;
}

PlacementRequest own_request(int64_t server) {
	PlacementRequest r;
	r.server_id = server;
	r.own_machine = true;
	return r;
}

bool placed_at(const Placement &p, int64_t machine, int slot) {
	return p.outcome == Placement::Outcome::Placed && p.machine_id == machine && p.slot == slot;
}

bool launches(const Placement &p, MachineKind kind) {
	return p.outcome == Placement::Outcome::Launch && p.launch_kind == kind;
}

int test_slots() {
	TEST_EXPECT(hosted::slot_game_port(0) == 32768 && hosted::slot_admin_port(0) == 32800);
	TEST_EXPECT(hosted::slot_uid(0) == 10001 && hosted::slot_dir(0) == "/srv/slots/slot-0");
	TEST_EXPECT(hosted::slot_game_port(11) == 32779 && hosted::slot_admin_port(11) == 32811);
	TEST_EXPECT(hosted::slot_uid(31) == 10032 && hosted::slot_dir_name(31) == "slot-31");
	TEST_EXPECT(hosted::slot_game_port(hosted::kMaxSlots - 1) == 32799);
	TEST_EXPECT(hosted::slot_admin_port(hosted::kMaxSlots - 1) == 32831);
	TEST_EXPECT(hosted::slot_of_uid(10001) == 0 && hosted::slot_of_uid(10032) == 31);
	TEST_EXPECT(hosted::slot_of_uid(10000) == -1 && hosted::slot_of_uid(10033) == -1 &&
			hosted::slot_of_uid(0) == -1);
	TEST_EXPECT(hosted::slot_valid(0) && hosted::slot_valid(31) && !hosted::slot_valid(32) &&
			!hosted::slot_valid(-1));
	return 0;
}

int test_words() {
	for (const MachineKind kind : { MachineKind::Shared, MachineKind::Own })
		TEST_EXPECT(hosted::parse_machine_kind(hosted::machine_kind_text(kind)) == kind);
	TEST_EXPECT(std::string(hosted::machine_kind_text(MachineKind::Own)) == "own");
	TEST_EXPECT(!hosted::parse_machine_kind("Shared") && !hosted::parse_machine_kind("") &&
			!hosted::parse_machine_kind("dedicated"));
	const char *states[] = { "launching", "booting", "ready", "draining", "ending", "ended",
		"failed", "lost" };
	for (const char *text : states) {
		const auto state = hosted::parse_fleet_machine_state(text);
		TEST_EXPECT(state && std::string(hosted::fleet_machine_state_text(*state)) == text);
	}
	TEST_EXPECT(!hosted::parse_fleet_machine_state("running") &&
			!hosted::parse_fleet_machine_state("READY"));
	// Launching, booting and ready take servers; nothing else does.
	TEST_EXPECT(hosted::machine_takes_servers(FleetMachineState::Launching));
	TEST_EXPECT(hosted::machine_takes_servers(FleetMachineState::Booting));
	TEST_EXPECT(hosted::machine_takes_servers(FleetMachineState::Ready));
	for (const FleetMachineState s : { FleetMachineState::Draining, FleetMachineState::Ending,
				 FleetMachineState::Ended, FleetMachineState::Failed, FleetMachineState::Lost })
		TEST_EXPECT(!hosted::machine_takes_servers(s));
	return 0;
}

int test_policy() {
	PlacementPolicy policy;
	TEST_EXPECT(hosted::check_placement_policy(policy).empty());
	TEST_EXPECT(policy.shared_slots == 12 && policy.max_shared == 2 && policy.max_own == 2);
	policy.shared_slots = hosted::kMaxSlots;
	TEST_EXPECT(hosted::check_placement_policy(policy).empty());
	policy.shared_slots = hosted::kMaxSlots + 1;
	TEST_EXPECT(!hosted::check_placement_policy(policy).empty());
	policy.shared_slots = 0;
	TEST_EXPECT(!hosted::check_placement_policy(policy).empty());
	policy = {};
	policy.max_shared = -1;
	TEST_EXPECT(!hosted::check_placement_policy(policy).empty());
	policy = {};
	policy.max_own = -1;
	TEST_EXPECT(!hosted::check_placement_policy(policy).empty());
	// A maximum of 0 is a fleet without that kind.
	policy = {};
	policy.max_own = 0;
	TEST_EXPECT(hosted::check_placement_policy(policy).empty());
	return 0;
}

int test_free_slots() {
	FleetMachine m = shared(1, FleetMachineState::Ready, 4, { 0, 1, 3 });
	TEST_EXPECT(hosted::first_free_slot(m) == 2 && hosted::free_slot_count(m) == 1);
	// A slot outside the machine's and a repeat hold nothing more.
	m.used_slots = { 1, 1, 7, -1 };
	TEST_EXPECT(hosted::first_free_slot(m) == 0 && hosted::free_slot_count(m) == 3);
	m.used_slots = { 0, 1, 2, 3 };
	TEST_EXPECT(hosted::first_free_slot(m) == -1 && hosted::free_slot_count(m) == 0);
	m.slots = 0;
	m.used_slots.clear();
	TEST_EXPECT(hosted::first_free_slot(m) == -1 && hosted::free_slot_count(m) == 0);
	return 0;
}

int test_shared() {
	const PlacementPolicy policy;
	// An empty fleet launches a shared machine; the server waits for it.
	TEST_EXPECT(launches(hosted::place_server(shared_request(1), {}, policy), MachineKind::Shared));
	// The machine that launch made takes the server, and the next ones, at once.
	std::vector<FleetMachine> fleet = { shared(10, FleetMachineState::Launching, 12, {}) };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(1), fleet, policy), 10, 0));
	fleet[0].used_slots = { 0 };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(2), fleet, policy), 10, 1));

	// Best fit: the fullest machine with a free slot, at its lowest free slot.
	fleet = { shared(10, FleetMachineState::Ready, 12, { 0, 1, 2, 3, 4 }),
		shared(11, FleetMachineState::Ready, 12, { 0, 1, 2, 4, 5, 6, 7, 8, 9, 10 }) };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(3), fleet, policy), 11, 3));
	// A full machine takes nothing: the next fullest does.
	fleet[1].used_slots = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(3), fleet, policy), 10, 5));
	// Machines of different sizes compare by their free slots.
	fleet = { shared(10, FleetMachineState::Ready, 4, { 0, 1 }),
		shared(11, FleetMachineState::Ready, 12, { 0 }) };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(3), fleet, policy), 10, 2));

	// Ties: the readier machine first, then the older (the lower id).
	fleet = { shared(12, FleetMachineState::Booting, 12, { 0 }),
		shared(13, FleetMachineState::Ready, 12, { 0 }) };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(4), fleet, policy), 13, 1));
	fleet = { shared(12, FleetMachineState::Launching, 12, { 0 }),
		shared(13, FleetMachineState::Booting, 12, { 0 }) };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(4), fleet, policy), 13, 1));
	fleet = { shared(14, FleetMachineState::Ready, 12, { 0 }),
		shared(12, FleetMachineState::Ready, 12, { 5 }) };
	TEST_EXPECT(placed_at(hosted::place_server(shared_request(4), fleet, policy), 12, 0));
	return 0;
}

int test_shared_limits() {
	PlacementPolicy policy;
	policy.max_shared = 2;
	const std::vector<int> full = { 0, 1, 2, 3 };
	// Two full machines taking servers: the fleet is at its maximum, so the server waits.
	std::vector<FleetMachine> fleet = { shared(1, FleetMachineState::Ready, 4, full),
		shared(2, FleetMachineState::Booting, 4, full) };
	TEST_EXPECT(hosted::place_server(shared_request(5), fleet, policy).outcome ==
			Placement::Outcome::Full);
	// A draining machine takes no server and counts toward no maximum: a roll launches
	// the machine it moves servers to.
	fleet[1].state = FleetMachineState::Draining;
	fleet[1].used_slots = {};
	TEST_EXPECT(
			launches(hosted::place_server(shared_request(5), fleet, policy), MachineKind::Shared));
	for (const FleetMachineState gone : { FleetMachineState::Ending, FleetMachineState::Ended,
				 FleetMachineState::Failed, FleetMachineState::Lost }) {
		fleet[1].state = gone;
		TEST_EXPECT(launches(
				hosted::place_server(shared_request(5), fleet, policy), MachineKind::Shared));
	}
	// Dedicated machines count toward the dedicated maximum alone, and never take a shared server.
	fleet = { shared(1, FleetMachineState::Ready, 4, full), own(2, FleetMachineState::Ready, 77),
		own(3, FleetMachineState::Ready, 78) };
	TEST_EXPECT(
			launches(hosted::place_server(shared_request(5), fleet, policy), MachineKind::Shared));
	// A fleet without shared machines (max 0) never launches one.
	policy.max_shared = 0;
	TEST_EXPECT(hosted::place_server(shared_request(5), {}, policy).outcome ==
			Placement::Outcome::Full);
	return 0;
}

int test_own() {
	PlacementPolicy policy;
	policy.max_own = 2;
	// No machine of its own yet: launch one (reserved for the server by the caller).
	TEST_EXPECT(launches(hosted::place_server(own_request(7), {}, policy), MachineKind::Own));
	// A shared machine with room is never a dedicated machine.
	std::vector<FleetMachine> fleet = { shared(1, FleetMachineState::Ready, 12, {}) };
	TEST_EXPECT(launches(hosted::place_server(own_request(7), fleet, policy), MachineKind::Own));
	// The machine launched for it takes it, at slot 0, from its launch on.
	fleet.push_back(own(2, FleetMachineState::Launching, 7));
	TEST_EXPECT(placed_at(hosted::place_server(own_request(7), fleet, policy), 2, 0));
	fleet[1].state = FleetMachineState::Ready;
	TEST_EXPECT(placed_at(hosted::place_server(own_request(7), fleet, policy), 2, 0));
	// Another server's dedicated machine is not this one's, nor is a shared server's.
	fleet = { own(2, FleetMachineState::Ready, 8) };
	TEST_EXPECT(launches(hosted::place_server(own_request(7), fleet, policy), MachineKind::Own));
	TEST_EXPECT(
			launches(hosted::place_server(shared_request(8), fleet, policy), MachineKind::Shared));
	// At the dedicated maximum the server waits.
	fleet = { own(2, FleetMachineState::Ready, 8), own(3, FleetMachineState::Booting, 9) };
	TEST_EXPECT(hosted::place_server(own_request(7), fleet, policy).outcome ==
			Placement::Outcome::Full);
	// Its dedicated machine draining (a roll) or gone: a new one, the old one counting toward nothing.
	fleet = { own(2, FleetMachineState::Draining, 7), own(3, FleetMachineState::Ready, 9) };
	TEST_EXPECT(launches(hosted::place_server(own_request(7), fleet, policy), MachineKind::Own));
	// Its dedicated machine's slot still held: it waits for that machine and never gets a
	// second one, below the maximum too.
	fleet = { own(2, FleetMachineState::Ready, 7, { 0 }) };
	TEST_EXPECT(hosted::place_server(own_request(7), fleet, policy).outcome ==
			Placement::Outcome::Full);
	policy.max_own = 0;
	TEST_EXPECT(
			hosted::place_server(own_request(7), {}, policy).outcome == Placement::Outcome::Full);
	return 0;
}

// A request placed after the servers waiting before it (the start request's
// at_capacity): each waiting one takes its place first, a launch it would cause
// adding that machine, so the planner never promises one slot twice.
int test_place_after() {
	PlacementPolicy policy;
	policy.shared_slots = 2;
	policy.max_shared = 1;
	policy.max_own = 1;
	// Nothing waits: the empty fleet launches.
	TEST_EXPECT(launches(hosted::place_after(shared_request(9), {}, {}, policy), MachineKind::Shared));
	// One waits: its launch's machine has a second slot, the request's.
	TEST_EXPECT(placed_at(hosted::place_after(shared_request(9), { shared_request(1) }, {}, policy), -1, 1));
	// Two wait: that machine is theirs, and no second may launch.
	const std::vector<PlacementRequest> two = { shared_request(1), shared_request(2) };
	TEST_EXPECT(hosted::place_after(shared_request(9), two, {}, policy).outcome == Placement::Outcome::Full);
	// A dedicated request waits for its dedicated machine apart: the shared ones leave it room.
	TEST_EXPECT(launches(hosted::place_after(own_request(9), two, {}, policy), MachineKind::Own));
	// One dedicated server waiting takes the one dedicated machine the fleet may launch.
	TEST_EXPECT(hosted::place_after(own_request(9), { own_request(3) }, {}, policy).outcome ==
			Placement::Outcome::Full);
	// Over real machines: a free slot taken by a waiting one is gone for the request.
	std::vector<FleetMachine> fleet = { shared(5, FleetMachineState::Ready, 2, { 0 }) };
	TEST_EXPECT(placed_at(hosted::place_after(shared_request(9), {}, fleet, policy), 5, 1));
	TEST_EXPECT(hosted::place_after(shared_request(9), { shared_request(1) }, fleet, policy).outcome ==
			Placement::Outcome::Full);
	// The request itself among the waiting (an id already written) is not counted twice.
	TEST_EXPECT(placed_at(hosted::place_after(shared_request(1), { shared_request(1) }, fleet, policy), 5, 1));
	// A waiting dedicated server whose machine exists goes there and leaves the rest alone.
	fleet = { own(6, FleetMachineState::Ready, 3) };
	policy.max_own = 2;
	TEST_EXPECT(launches(hosted::place_after(own_request(9), { own_request(3) }, fleet, policy), MachineKind::Own));
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "slots", test_slots },
		{ "words", test_words },
		{ "policy", test_policy },
		{ "free_slots", test_free_slots },
		{ "shared", test_shared },
		{ "shared_limits", test_shared_limits },
		{ "own", test_own },
		{ "place_after", test_place_after },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted placement\n");
	return 0;
}
