#pragma once

// The hosted servers' stats prune as a periodic job (jobs/scheduler.h), once
// the sampler's own on the supervisor's pass: every hour, stats.prune deletes
// the samples past kSampleKeepSeconds, the hours past kHourlyKeepSeconds and
// the machine months more than 13 months back (stats_repository.h
// prune_stats), then the fleet's machines ended or failed more than 14 days
// ago (fleet_repository.h prune_ended_machines), on a worker, so the
// supervisor's watched pass never waits on the deletes. Scheduled and handled
// with the HTTP layer whether or not hosting is on, so the rows of a fleet
// that was turned off still age out. Service policy, not a port.

#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace opennova::novaworld_server::hosted {

inline constexpr const char *kJobStatsPrune = "stats.prune";
inline constexpr int64_t kStatsPruneIntervalMs = 60 * 60 * 1000;

// The handler (the worker's): prune_stats at `now_s` (Unix seconds; the
// wall clock's, a test's hand-stepped one).
void register_stats_jobs(
		jobs::JobRegistry &registry,
		std::function<int64_t()> now_s = [] { return jobs::Scheduler::wall_now_ms() / 1000; });
// Its schedule (the fleet's): hourly.
std::vector<jobs::PeriodicJob> stats_schedule();

} // namespace opennova::novaworld_server::hosted
