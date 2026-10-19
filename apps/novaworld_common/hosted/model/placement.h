#pragma once

// Where a hosted server runs: which machine and which slot (hosted/protocol/slots.h). Pure
// functions over plain structs, so the supervisor decides from the rows it read
// and the tests check every rule without a database or a cloud. Service policy,
// not a port.
//
// A shared server goes to the fullest shared machine that takes servers
// (launching, booting or ready; never one draining, ending or gone) and has a
// free slot, its lowest free slot (best fit, so machines fill one at a time and
// an emptied one can end). With none, a new shared machine is launched while the
// fleet has fewer than ONNET_FLEET_MAX_SHARED of them taking servers, and the
// server waits in provisioning for it ("waiting for a machine"; no restart is
// counted); at that maximum the server waits for a slot to free. A server on the
// own-machine add-on goes to the machine launched for it (reserved for its id)
// while that one takes servers (waiting while its slot is held, never getting a
// second), else to a new own machine, at most ONNET_FLEET_MAX_OWN of them taking
// servers. A launching machine takes servers:
// the servers that wait for it fill it as it boots, so one launch serves every
// server a pass places. Draining and ending machines count toward neither
// maximum: a fleet roll launches the machines it moves servers to while the old
// ones drain.

#include "hosted/protocol/slots.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// fleet_machines.kind.
enum class MachineKind : uint8_t {
	Shared, // up to ONNET_FLEET_SHARED_SLOTS servers, any account's
	Own, // one server, the add-on's: launched for it, ended when it stops
};

// fleet_machines.state: launching -> booting -> ready -> draining -> ending ->
// ended | failed, and lost once its agent's last report is older than 60 s.
enum class FleetMachineState : uint8_t {
	Launching, // the cloud is asked for it
	Booting, // it runs; its agent has not reported yet
	Ready, // its agent reports
	Draining, // its servers move off it, one at a time; nothing new lands on it
	Ending, // the cloud is asked to end it
	Ended,
	Failed,
	Lost, // its agent stopped reporting
};

// The columns' words: "shared", "own"; "launching", "booting", "ready",
// "draining", "ending", "ended", "failed", "lost".
const char *machine_kind_text(MachineKind kind);
std::optional<MachineKind> parse_machine_kind(std::string_view text);
const char *fleet_machine_state_text(FleetMachineState state);
std::optional<FleetMachineState> parse_fleet_machine_state(std::string_view text);

// Whether a machine in `state` takes new servers: launching, booting, ready.
// The same machines count toward the fleet's maximums.
bool machine_takes_servers(FleetMachineState state);

// What placement reads of one machine: its fleet_machines row and the slots the
// live servers on it hold. The server being placed holds none (the caller
// leaves it out).
struct FleetMachine {
	int64_t id = 0;
	MachineKind kind = MachineKind::Shared;
	FleetMachineState state = FleetMachineState::Launching;
	int slots = 0; // fleet_machines.slots
	std::vector<int> used_slots; // any order; one outside 0..slots-1 is ignored
	int64_t reserved_for = 0; // an own machine's server id (0: none)
};

// The fleet's knobs (ONNET_FLEET_*).
struct PlacementPolicy {
	int shared_slots = 12; // a new shared machine's slots (ONNET_FLEET_SHARED_SLOTS)
	int max_shared = 2; // ONNET_FLEET_MAX_SHARED
	int max_own = 2; // ONNET_FLEET_MAX_OWN
	// The static addresses the fleet sells (ONNET_FLEET_MAX_ADDRESSES; 0: the
	// add-on is off). Placement never reads it: sold_capacity.h does.
	int max_addresses = 0;
};

// "" when `policy` holds (shared_slots 1..kMaxSlots, each maximum 0 or more),
// else why not.
std::string check_placement_policy(const PlacementPolicy &policy);

struct PlacementRequest {
	int64_t server_id = 0;
	bool own_machine = false; // the account's own-machine add-on puts it on a machine of its own
};

struct Placement {
	enum class Outcome : uint8_t {
		Placed, // on `machine_id`, at `slot`
		Launch, // launch a machine of `launch_kind` (an own one reserved for the
				// server; a shared one of policy.shared_slots), then place again
		Full, // at the maximum, no slot free: the server waits
	};
	Outcome outcome = Outcome::Full;
	int64_t machine_id = 0;
	int slot = -1;
	MachineKind launch_kind = MachineKind::Shared;
};

// The lowest slot of `machine` no live server holds, or -1.
int first_free_slot(const FleetMachine &machine);
// How many of `machine`'s slots no live server holds.
int free_slot_count(const FleetMachine &machine);

// Where `request` goes on `machines` (every machine of the fleet, any state)
// under `policy`.
Placement place_server(const PlacementRequest &request, const std::vector<FleetMachine> &machines,
		const PlacementPolicy &policy);

// Where `request` would go once every server in `waiting` (the live servers
// holding no slot yet, in the order the supervisor places them: by id) took
// its place first: each one placed in turn on `machines`, a Placed one holding
// its slot, a Launch one adding the machine it would launch (launching, an own
// one reserved for it, a shared one of policy.shared_slots slots), a Full one
// waiting with no place. A start request whose answer is Full is the fleet's
// at_capacity: the supervisor would leave it waiting.
Placement place_after(const PlacementRequest &request, const std::vector<PlacementRequest> &waiting,
		std::vector<FleetMachine> machines, const PlacementPolicy &policy);

} // namespace opennova::novaworld_server::hosted
