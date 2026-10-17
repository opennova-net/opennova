#include "stats_jobs.h"

#include "fleet_repository.h"
#include "stats_repository.h"

#include <cstdio>
#include <utility>

namespace opennova::novaworld_server::hosted {

void register_stats_jobs(jobs::JobRegistry &registry, std::function<int64_t()> now_s) {
	registry.add({ kJobStatsPrune,
			[now_s = std::move(now_s)](jobs::JobContext &ctx) {
				auto lease = ctx.pool.acquire();
				prune_stats(*lease, now_s());
				if (const size_t pruned = prune_ended_machines(*lease); pruned > 0)
					std::printf("[sweep] pruned %zu ended fleet machine(s)\n", pruned);
				return jobs::JobResult::done();
			},
			1 });
}

std::vector<jobs::PeriodicJob> stats_schedule() {
	jobs::PeriodicJob p;
	p.kind = kJobStatsPrune;
	p.period_ms = kStatsPruneIntervalMs;
	return { std::move(p) };
}

} // namespace opennova::novaworld_server::hosted
