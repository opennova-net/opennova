#include "billing_jobs.h"

#include "billing_repository.h"

#include <cstdio>
#include <utility>

namespace opennova::novaworld_server::billing {

namespace {

// A reconcile tried again every kReconcileRetryS for a day, then failed until
// the next one the schedule enqueues.
constexpr int kReconcileAttempts = 96;

int64_t enqueue_kind(db::Database &db, const char *kind, int max_attempts) {
	jobs::NewJob job;
	job.kind = kind;
	job.dedupe_key = kind;
	job.max_attempts = max_attempts;
	return jobs::enqueue(db, job);
}

} // namespace

int64_t enqueue_billing_sync(db::Database &db) {
	return enqueue_kind(db, kJobBillingSync, 3);
}

int64_t enqueue_billing_reconcile(db::Database &db) {
	return enqueue_kind(db, kJobBillingReconcile, kReconcileAttempts);
}

void register_billing_jobs(jobs::JobRegistry &registry, BillingSync &sync) {
	registry.add({ kJobBillingSync,
			[&sync](jobs::JobContext &) {
				sync.sync_pending(sync.now());
				return jobs::JobResult::done();
			},
			1 });
	registry.add({ kJobBillingReconcile,
			[&sync](jobs::JobContext &) {
				const ReconcileReport report = sync.reconcile_all(sync.now());
				if (report.completed)
					return jobs::JobResult::done();
				return jobs::JobResult::retry(report.error, BillingSync::kReconcileRetryS * 1000);
			},
			1 });
}

std::vector<jobs::PeriodicJob> billing_schedule(int reconcile_minutes) {
	jobs::PeriodicJob reconcile;
	reconcile.kind = kJobBillingReconcile;
	reconcile.period_ms = static_cast<int64_t>(reconcile_minutes) * 60 * 1000;
	reconcile.max_attempts = kReconcileAttempts;
	reconcile.at_start = true;
	jobs::PeriodicJob sync;
	sync.kind = kJobBillingSync;
	sync.period_ms = 60 * 1000;
	sync.max_attempts = 3;
	// A bucket gets one only while an event waits (a customer's backoff).
	sync.wanted = [](db::Database &db) { return pending_event_count(db) > 0; };
	return { std::move(reconcile), std::move(sync) };
}

void register_billing_prune(jobs::JobRegistry &registry) {
	registry.add({ kJobBillingEventsPrune,
			[](jobs::JobContext &ctx) {
				auto lease = ctx.pool.acquire();
				if (const size_t pruned = prune_events(*lease); pruned > 0)
					std::printf("[sweep] pruned %zu old billing event(s)\n", pruned);
				return jobs::JobResult::done();
			},
			1 });
}

std::vector<jobs::PeriodicJob> billing_prune_schedule(int64_t period_ms) {
	jobs::PeriodicJob p;
	p.kind = kJobBillingEventsPrune;
	p.period_ms = period_ms;
	return { std::move(p) };
}

} // namespace opennova::novaworld_server::billing
