#include "account_jobs.h"

#include "auth.h"
#include "password_reset.h"
#include "web_session.h"

#include <cstdio>
#include <functional>

namespace opennova::novaworld_server::jobs {

namespace {

// A prune's job: `prune` on a leased connection, its count logged as `what`.
JobKind prune_job(const char *kind, std::function<size_t(db::Database &)> prune, const char *what) {
	return { kind,
		[prune = std::move(prune), what](JobContext &ctx) {
			auto lease = ctx.pool.acquire();
			if (const size_t pruned = prune(*lease); pruned > 0)
				std::printf("[sweep] pruned %zu %s\n", pruned, what);
			return JobResult::done();
		},
		1 };
}

} // namespace

void register_account_jobs(JobRegistry &registry) {
	registry.add(
			prune_job(kJobWebSessionsPrune, prune_expired_web_sessions, "expired web session(s)"));
	registry.add(prune_job(
			kJobActiveSessionsPrune,
			[](db::Database &db) {
				return evict_active_user_sessions_older_than(db, kRetailLoginIdleSeconds);
			},
			"idle retail login(s)"));
	registry.add(
			prune_job(kJobLoginAddressesPrune, prune_login_addresses, "stale login address(es)"));
	registry.add(prune_job(
			kJobPasswordResetsPrune, prune_password_resets, "expired password reset link(s)"));
}

std::vector<PeriodicJob> account_schedule(const MaintenanceConfig &config) {
	std::vector<PeriodicJob> out;
	for (const char *kind : { kJobWebSessionsPrune, kJobActiveSessionsPrune,
				 kJobLoginAddressesPrune, kJobPasswordResetsPrune }) {
		PeriodicJob p;
		p.kind = kind;
		p.period_ms = config.sweep_interval_ms;
		out.push_back(std::move(p));
	}
	return out;
}

} // namespace opennova::novaworld_server::jobs
