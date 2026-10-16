#pragma once

// The service's prunes as periodic jobs (scheduler.h), once the main loop's
// sweep (README "Background jobs"): every sweep_interval_ms, each its own job.
//
//   hosts.prune          the lobby instances whose process is gone
//                        (lobbydb::reap_stale_instances: their sessions,
//                        commands and listings with them), then the listings
//                        whose last word is older than the stale window
//                        (hostdb::prune_stale_hosts: the backstop for a
//                        listing its GOODBYE, stop or timeout teardown missed)
//   host_keys.prune      hosting keys idle a day (hostdb::prune_host_keys)
//   jobs.prune           finished jobs 14 days old (job_queue.h), or as old as
//                        its payload's keep_s says (an admin's prune, at
//                        least kPruneKeepMinS)
//
// and, with the HTTP layer (register_account_jobs, account_jobs.h), the
// website's: sessions past their expiry, login addresses, password reset links
// and old billing events. Service policy, not a port.

#include "scheduler.h"
#include "worker_pool.h"

#include <cstdint>
#include <vector>

namespace opennova::novaworld_server::jobs {

inline constexpr const char *kJobHostsPrune = "hosts.prune";
inline constexpr const char *kJobHostKeysPrune = "host_keys.prune";
inline constexpr const char *kJobJobsPrune = "jobs.prune";
// The member of a jobs.prune payload that names its keep (seconds), and the
// least it takes: a day of finished jobs always stays for the admin's view.
inline constexpr const char *kPruneKeepKey = "keep_s";
inline constexpr int64_t kPruneKeepMinS = 24 * 60 * 60;

struct MaintenanceConfig {
	int64_t sweep_interval_ms = 30 * 1000; // HOST_SWEEP_INTERVAL_MS
	int64_t host_stale_window_ms = 300 * 1000; // HOST_STALE_WINDOW_MS
	int64_t jobs_prune_interval_ms = 60 * 60 * 1000;
};

// The handlers (the worker's).
void register_maintenance_jobs(JobRegistry &registry, const MaintenanceConfig &config);
// Their schedule (the controller's).
std::vector<PeriodicJob> maintenance_schedule(const MaintenanceConfig &config);

} // namespace opennova::novaworld_server::jobs
