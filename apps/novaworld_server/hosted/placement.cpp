#include "placement.h"

#include <cstddef>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

struct KindName {
	MachineKind kind;
	const char *text;
};
constexpr KindName kKinds[] = {
	{ MachineKind::Shared, "shared" },
	{ MachineKind::Own, "own" },
};

struct StateName {
	FleetMachineState state;
	const char *text;
};
constexpr StateName kStates[] = {
	{ FleetMachineState::Launching, "launching" },
	{ FleetMachineState::Booting, "booting" },
	{ FleetMachineState::Ready, "ready" },
	{ FleetMachineState::Draining, "draining" },
	{ FleetMachineState::Ending, "ending" },
	{ FleetMachineState::Ended, "ended" },
	{ FleetMachineState::Failed, "failed" },
	{ FleetMachineState::Lost, "lost" },
};

// Among equally full machines, the one closest to running a server first.
int readiness_rank(FleetMachineState state) {
	switch (state) {
		case FleetMachineState::Ready:
			return 0;
		case FleetMachineState::Booting:
			return 1;
		default:
			return 2;
	}
}

// Which of `machine`'s slots live servers hold.
std::vector<bool> held_slots(const FleetMachine &machine) {
	std::vector<bool> held(machine.slots > 0 ? static_cast<size_t>(machine.slots) : 0, false);
	for (const int slot : machine.used_slots)
		if (slot >= 0 && slot < machine.slots)
			held[static_cast<size_t>(slot)] = true;
	return held;
}

int machines_taking_servers(const std::vector<FleetMachine> &machines, MachineKind kind) {
	int count = 0;
	for (const FleetMachine &machine : machines)
		if (machine.kind == kind && machine_takes_servers(machine.state))
			++count;
	return count;
}

Placement placed(const FleetMachine &machine) {
	Placement out;
	out.outcome = Placement::Outcome::Placed;
	out.machine_id = machine.id;
	out.slot = first_free_slot(machine);
	return out;
}

Placement launch_or_full(const std::vector<FleetMachine> &machines, MachineKind kind, int maximum) {
	Placement out;
	if (machines_taking_servers(machines, kind) < maximum) {
		out.outcome = Placement::Outcome::Launch;
		out.launch_kind = kind;
	}
	return out;
}

} // namespace

const char *machine_kind_text(MachineKind kind) {
	for (const KindName &k : kKinds)
		if (k.kind == kind)
			return k.text;
	return "shared";
}

std::optional<MachineKind> parse_machine_kind(std::string_view text) {
	for (const KindName &k : kKinds)
		if (text == k.text)
			return k.kind;
	return std::nullopt;
}

const char *fleet_machine_state_text(FleetMachineState state) {
	for (const StateName &s : kStates)
		if (s.state == state)
			return s.text;
	return "failed";
}

std::optional<FleetMachineState> parse_fleet_machine_state(std::string_view text) {
	for (const StateName &s : kStates)
		if (text == s.text)
			return s.state;
	return std::nullopt;
}

bool machine_takes_servers(FleetMachineState state) {
	return state == FleetMachineState::Launching || state == FleetMachineState::Booting ||
			state == FleetMachineState::Ready;
}

std::string check_placement_policy(const PlacementPolicy &policy) {
	if (policy.shared_slots < 1 || policy.shared_slots > kMaxSlots)
		return "a shared machine's slots must be 1.." + std::to_string(kMaxSlots) + ", not " +
				std::to_string(policy.shared_slots);
	if (policy.max_shared < 0)
		return "the most shared machines cannot be below 0";
	if (policy.max_own < 0)
		return "the most own machines cannot be below 0";
	return std::string();
}

int first_free_slot(const FleetMachine &machine) {
	const std::vector<bool> held = held_slots(machine);
	for (size_t slot = 0; slot < held.size(); ++slot)
		if (!held[slot])
			return static_cast<int>(slot);
	return -1;
}

int free_slot_count(const FleetMachine &machine) {
	int count = 0;
	for (const bool slot_held : held_slots(machine))
		if (!slot_held)
			++count;
	return count;
}

Placement place_server(const PlacementRequest &request, const std::vector<FleetMachine> &machines,
		const PlacementPolicy &policy) {
	if (request.own_machine) {
		for (const FleetMachine &machine : machines) {
			if (machine.kind != MachineKind::Own || machine.reserved_for != request.server_id ||
					!machine_takes_servers(machine.state))
				continue;
			// Its machine's slot still held: it waits for that one, never a second.
			if (first_free_slot(machine) < 0)
				return Placement();
			return placed(machine);
		}
		return launch_or_full(machines, MachineKind::Own, policy.max_own);
	}

	const FleetMachine *best = nullptr;
	int best_free = 0;
	for (const FleetMachine &machine : machines) {
		if (machine.kind != MachineKind::Shared || !machine_takes_servers(machine.state))
			continue;
		const int free = free_slot_count(machine);
		if (free == 0)
			continue;
		const bool better = best == nullptr || free < best_free ||
				(free == best_free &&
						(readiness_rank(machine.state) < readiness_rank(best->state) ||
								(readiness_rank(machine.state) == readiness_rank(best->state) &&
										machine.id < best->id)));
		if (better) {
			best = &machine;
			best_free = free;
		}
	}
	if (best)
		return placed(*best);
	return launch_or_full(machines, MachineKind::Shared, policy.max_shared);
}

} // namespace opennova::novaworld_server::hosted
