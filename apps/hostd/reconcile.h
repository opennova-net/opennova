#pragma once

// What one sync does on the machine, decided from the service's reply and what
// the runtime reports: a pure function, so every rule is a test. The rules:
//
//   - a container the reply names runs on (one that exited stays, reported,
//     until the service drops it: the service decides a restart, never the
//     agent);
//   - a running container the reply leaves out is asked to stop (SIGTERM) and,
//     still running kStopTimeoutMs later, killed; once it is not running it is
//     removed, and its slot's directory with it;
//   - a container the reply names that the machine lacks starts once its slot
//     is free: never while another container, of any state, still holds the
//     slot (so a slot's next server never meets its previous one's port, user
//     or files), at most `max_starts` a pass, and only with its files (sent
//     until the agent first reports it), its data set mounted here and its slot
//     on this machine; a container whose start failed is not started again;
//   - the pass is busy (the next sync comes sooner) while any of this waits.

#include "container_runtime.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace opennova::hostd {

inline constexpr uint64_t kStopTimeoutMs = 60 * 1000;
inline constexpr size_t kMaxStartsPerPass = 4;

struct PlanInput {
	const std::vector<hosted::DesiredContainer> *desired = nullptr;
	const std::vector<ContainerInfo> *observed = nullptr;
	// The containers asked to stop, and when (the agent's own clock, ms).
	const std::map<std::string, uint64_t> *stop_requested_ms = nullptr;
	// The containers whose start failed (reported exited, never started again).
	const std::set<std::string> *failed_starts = nullptr;
	int machine_slots = 0;
	const std::map<std::string, std::string> *data_sets = nullptr; // data set -> mount path
	uint64_t now_ms = 0;
	size_t max_starts = kMaxStartsPerPass;
};

struct Plan {
	std::vector<std::string> stop; // SIGTERM now
	std::vector<std::string> kill; // still running past kStopTimeoutMs
	std::vector<ContainerInfo> remove; // not running and not desired; its slot's directory goes too
	std::vector<size_t> start; // indexes into the desired list
	// Desired containers that cannot start here, with why (the agent logs each once).
	std::vector<std::pair<size_t, std::string>> refused;
	bool busy = false;
};

Plan plan_sync(const PlanInput &in);

} // namespace opennova::hostd
