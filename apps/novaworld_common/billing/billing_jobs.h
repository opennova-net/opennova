#pragma once

// The billing sync's jobs (billing_sync.h) on the service's queue
// (jobs/job_queue.h):
//
//   billing.reconcile  BillingSync::reconcile_all: at a release's start,
//                      every ONNET_BILLING_RECONCILE_MINUTES and when an admin
//                      asks; one that did not complete runs again
//                      kReconcileRetryS later (the job's retries).
//   billing.sync       BillingSync::sync_pending: enqueued by each fresh webhook
//                      event, and every minute while a customer's events wait
//                      (a backoff ran out) or the prices are not checked under
//                      the worker's lists (a STRIPE_PRICE_* list changed: the
//                      check lands within a minute, not at the next reconcile).
//
// Each deduped by its kind (one queued or running at a time), capped at one
// running across the workers. And the cache's prune, which runs whether or not
// billing is configured (the cache outlives its settings):
//
//   billing_events.prune  webhook events synced more than 30 days ago (the
//                         cache keeps no history Stripe does not), every
//                         sweep interval (jobs/maintenance_jobs.h)
//
// Service policy, not a port.

#include "billing_sync.h"

#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <vector>

namespace opennova::novaworld_server::billing {

inline constexpr const char *kJobBillingSync = "billing.sync";
inline constexpr const char *kJobBillingReconcile = "billing.reconcile";
inline constexpr const char *kJobBillingEventsPrune = "billing_events.prune";

// On `db`, in its open transaction when one is: 0 when one is queued or running.
int64_t enqueue_billing_sync(db::Database &db);
int64_t enqueue_billing_reconcile(db::Database &db);

// The handlers over `sync`, which must outlive the workers.
void register_billing_jobs(jobs::JobRegistry &registry, BillingSync &sync);
// The schedule under `config`: the reconcile at the start and every
// reconcile_minutes, the sync every minute while an event waits or the prices
// are not checked under config's lists (prices_checked_under).
std::vector<jobs::PeriodicJob> billing_schedule(const BillingConfig &config);

// The prune of the synced webhook events, and its schedule every `period_ms`
// (the sweep interval).
void register_billing_prune(jobs::JobRegistry &registry);
std::vector<jobs::PeriodicJob> billing_prune_schedule(int64_t period_ms);

} // namespace opennova::novaworld_server::billing
