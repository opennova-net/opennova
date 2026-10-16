#include "maintenance_jobs.h"

#include <net/novaworld/host_repository.h>
#include <net/novaworld/lobby_repository.h>

#include <algorithm>
#include <cstdio>

namespace opennova::novaworld_server::jobs {

void register_maintenance_jobs(JobRegistry &registry, const MaintenanceConfig &config) {
	const int64_t window_s = config.host_stale_window_ms / 1000;
	registry.add({ kJobHostsPrune,
			[window_s](JobContext &ctx) {
				auto lease = ctx.pool.acquire();
				// A lobby instance whose process is gone (unseen for kInstanceStaleSeconds, well
				// past a database failover), with its sessions, commands and listings; then the
				// listings no heartbeat refreshed.
				if (const size_t reaped = lobbydb::reap_stale_instances(
							*lease, lobbydb::kInstanceStaleSeconds);
						reaped > 0)
					std::printf("[sweep] reaped %zu stale lobby instance(s)\n", reaped);
				if (const int pruned = hostdb::prune_stale_hosts(*lease, window_s); pruned > 0)
					std::printf("[sweep] pruned %d stale host row(s)\n", pruned);
				return JobResult::done();
			},
			1 });
	registry.add({ kJobHostKeysPrune,
			[](JobContext &ctx) {
				auto lease = ctx.pool.acquire();
				if (const auto pruned = hostdb::prune_host_keys(*lease); pruned > 0)
					std::printf("[sweep] pruned %zu idle host key(s)\n", pruned);
				return JobResult::done();
			},
			1 });
	registry.add({ kJobJobsPrune,
			[](JobContext &ctx) {
				int64_t keep_s = kFinishedKeepS;
				if (const io::JsonValue *keep = ctx.job.payload.get(kPruneKeepKey);
						keep != nullptr && keep->is_number())
					keep_s = std::max(static_cast<int64_t>(keep->number), kPruneKeepMinS);
				auto lease = ctx.pool.acquire();
				if (const size_t pruned = prune_finished(*lease, keep_s); pruned > 0)
					std::printf("[sweep] pruned %zu finished job(s)\n", pruned);
				return JobResult::done();
			},
			1 });
}

std::vector<PeriodicJob> maintenance_schedule(const MaintenanceConfig &config) {
	std::vector<PeriodicJob> out;
	for (const char *kind : { kJobHostsPrune, kJobHostKeysPrune }) {
		PeriodicJob p;
		p.kind = kind;
		p.period_ms = config.sweep_interval_ms;
		out.push_back(std::move(p));
	}
	PeriodicJob jobs;
	jobs.kind = kJobJobsPrune;
	jobs.period_ms = config.jobs_prune_interval_ms;
	out.push_back(std::move(jobs));
	return out;
}

} // namespace opennova::novaworld_server::jobs
