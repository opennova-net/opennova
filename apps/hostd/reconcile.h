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
//     until the agent first reports it), its trees held by the store and its
//     slot on this machine; a container whose start failed is not started again;
//   - one whose base or expansion tree the store is still fetching (a failed
//     fetch with a try left among them) waits for it instead (reported
//     starting), holding its slot, and is not counted among the pass's starts;
//     one whose tree the store gave up on fails (exited 125, the fetch's
//     reason), as one whose image did not pull does;
//   - one whose image the machine is pulling waits for the pull the same way;
//   - the pass is busy (the next sync comes sooner) while any of this waits.

#include "container_runtime.h"
#include "tree_store.h"

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
	// The images the machine is pulling now (null: none).
	const std::set<std::string> *pulling = nullptr;
	int machine_slots = 0;
	// Each tree's standing, by id (tree_store.h standings()): present, fetching
	// (a fetch to come included), or failed for good with the error. A tree it
	// lacks cannot be started on.
	const std::map<int64_t, TreeStanding> *trees = nullptr;
	uint64_t now_ms = 0;
	size_t max_starts = kMaxStartsPerPass;
};

struct Plan {
	std::vector<std::string> stop; // SIGTERM now
	std::vector<std::string> kill; // still running past kStopTimeoutMs
	std::vector<ContainerInfo> remove; // not running and not desired; its slot's directory goes too
	std::vector<size_t> start; // indexes into the desired list
	std::vector<size_t> wait_for_image; // the same: they start once their image pulled
	std::vector<size_t> wait_for_tree; // the same: they start once their trees are fetched
	// Desired containers whose tree the store gave up on, with the last fetch's
	// reason: their starts fail (exited 125).
	std::vector<std::pair<size_t, std::string>> data_failed;
	// Desired containers that cannot start here, with why (the agent logs each once).
	std::vector<std::pair<size_t, std::string>> refused;
	bool busy = false;
};

Plan plan_sync(const PlanInput &in);

} // namespace opennova::hostd
