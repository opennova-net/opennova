#pragma once

// The billing sync: keeps the billing tables (billing_repository.h) in step
// with Stripe, the record, as two jobs on the service's queue (billing_jobs.h,
// jobs/job_queue.h) that any worker runs. A webhook event only marks its
// customer dirty (billing_events) and enqueues a billing.sync; the job then
// reads that customer and every subscription of it back from Stripe and writes
// what it read, so an event's body is never trusted and the order events arrive
// in does not matter. Its state (the prices as last checked, the last
// reconcile and what it found, each waiting customer's backoff) is rows
// (0007_jobs.sql billing_sync_state, billing_sync_backoff), read by the routes
// and the heartbeat wherever they run. Service policy over Stripe's REST API,
// not a port.
//
//   - billing.reconcile (reconcile_all): at a release's start, every
//     reconcile_minutes (ONNET_BILLING_RECONCILE_MINUTES, 360) and when an
//     admin asks (POST /api/admin/billing/reconcile): every subscription of the account, a page of
//     100 at a time with its customer expanded (at most kMaxReconcilePages); a subscription whose
//     customer carries this database's instance id (opennova_instance) is ours and written, its
//     customer's row made again when it is missing (the account from the customer's
//     opennova_user_id); a granting one of another instance, or ours with no account here, is an
//     orphan the admin page lists (in memory: the next reconcile lists them
//     afresh); a granting row of ours Stripe no longer lists is read alone,
//     and canceled when Stripe no longer knows it. Accounts with more than one
//     granting tier subscription are listed as duplicates. The configured
//     prices are checked at every reconcile (and at the first pass): each one
//     a recurring monthly price in the key's mode, the one each list sells
//     active, the ones sold in one currency; else prices_ok is false and
//     checkout refuses (503 billing_misconfigured). A subscription of ours
//     whose status would grant but whose price none of the lists names is
//     listed (unknown_prices: a sold price missing from the configuration,
//     which leaves its account with nothing). One that read every
//     subscription with the prices checking out and no such price left calls
//     on_reconciled. One that did not complete is tried again
//     kReconcileRetryS later (the job's retry).
//   - billing.sync (sync_pending): the price check while the prices are not
//     checked (again kBackoffFirstS after one Stripe could not answer), then
//     the customers with an unprocessed event, the longest-waiting first
//     (kPendingPerPass at most), one sync each: a customer Stripe could not
//     be asked about waits kBackoffFirstS, doubling to kBackoffMaxS. Enqueued
//     by each fresh webhook event, and every minute while a customer waits.
//
// No Stripe call ever runs inside a db::Transaction, nor while a pooled
// connection is held for a job: a sync reads Stripe first, then leases a
// connection and writes what it read in one transaction. Thread-safe: the
// routes call sync_customer (POST /api/me/billing/sync) and status() from
// their own threads, the jobs from the workers'.

#include "billing_config.h"
#include "billing_repository.h"
#include "entitlement.h"
#include "stripe_client.h"

#include <net/novaworld/db/database.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::billing {

// A granting subscription no account here owns: another instance's (a
// developer's database on a shared test account, a database since reset), or
// one of ours whose customer names no account here. An admin relinks it.
struct Orphan {
	std::string stripe_customer_id;
	std::string stripe_subscription_id;
	std::string username_hint; // the customer's opennova_username, "" when none
	std::string status;
};

// A configured price as Stripe last answered it.
struct PriceInfo {
	std::string id;
	std::optional<int64_t> unit_amount; // the currency's smallest unit
	std::string currency;
	std::string interval; // "month"
};

struct SyncStatus {
	// A price check completed, and what it found.
	bool prices_checked = false;
	bool prices_ok = false;
	std::string prices_error; // "" when ok
	// The price each tier (index 1..kTierCount) and the add-on sells now, once
	// checked: what GET /api/billing/tiers shows.
	std::array<std::optional<PriceInfo>, kTierCount + 1> tier_prices;
	std::optional<PriceInfo> own_machine_price;
	// The last full reconcile that completed (Unix seconds; 0: none yet), and
	// why the last one did not ("" when it did).
	int64_t last_reconcile_at = 0;
	std::string last_reconcile_error;
	std::vector<Orphan> orphans;
	std::vector<int64_t> duplicates; // accounts with more than one granting tier subscription
	// The prices of our subscriptions that would grant but sell no configured
	// price (a sold price missing from STRIPE_PRICE_*): their accounts get
	// nothing for them, and no reconcile confirms the cache while any is left.
	std::vector<std::string> unknown_prices;
	int64_t started_at = 0; // when the billing sync first ran on this database
};

// What one customer's sync came to.
enum class SyncOutcome : uint8_t {
	Synced,      // read and written
	Foreign,     // the customer is another instance's: nothing written
	Deleted,     // Stripe deleted it (or no longer knows it): its row gone, its subscriptions canceled
	Orphan,      // ours, but naming no account here: nothing written
	Unreachable, // Stripe could not be asked (or refused the key): try again later
};
const char *sync_outcome_name(SyncOutcome outcome);

struct ReconcileReport {
	bool completed = false;
	std::string error;
	size_t listed = 0;              // subscriptions Stripe listed
	size_t written = 0;             // of ours, written
	size_t customers_recreated = 0; // customer rows made again
	size_t canceled = 0;            // local rows Stripe no longer knows
	size_t orphans = 0;
};

class BillingSync {
public:
	using Clock = std::function<int64_t()>;

	static constexpr int64_t kBackoffFirstS = 60;
	static constexpr int64_t kBackoffMaxS = 15 * 60;
	static constexpr size_t kPendingPerPass = 50;
	static constexpr int kMaxReconcilePages = 100;
	// A reconcile that did not complete is tried again this much later.
	static constexpr int64_t kReconcileRetryS = 15 * 60;

	// `config` must be complete (check_billing_config answers "") and, like
	// `client` and `pool`, outlive the sync. `clock` is the wall clock in Unix
	// seconds (the tests step one by hand).
	BillingSync(db::ConnectionPool &pool, StripeClient &client, const BillingConfig &config,
			std::string instance_id, Clock clock = unix_now);
	BillingSync(const BillingSync &) = delete;
	BillingSync &operator=(const BillingSync &) = delete;

	// Called on the job's thread, with a connection of its own, after each full
	// reconcile that completed with the prices checking out and no subscription
	// of ours selling a price the configuration does not name (the hosted
	// servers' limits hold, hosted/limits_hold.h, trusts the cache after one).
	void set_on_reconciled(std::function<void(db::Database &)> on_reconciled) {
		on_reconciled_ = std::move(on_reconciled);
	}

	// The billing.sync job at `now`: the price check while unchecked, then the
	// waiting customers past their backoff. Leases its own connections, each
	// for one step's reads or writes and none across a Stripe call; the
	// calling thread holds none.
	void sync_pending(int64_t now);
	// One customer, read back from Stripe and written on `db` (a route's
	// connection, held for its request); its events recorded so far marked with
	// the outcome (but Unreachable's, which stay pending).
	SyncOutcome sync_customer(db::Database &db, const std::string &stripe_customer_id, int64_t now);
	// The billing.reconcile job at `now`; the calling thread holds no lease.
	ReconcileReport reconcile_all(int64_t now);
	// The configured prices checked against Stripe and the answer written;
	// false when Stripe could not be asked (the state keeps what it had). The
	// calling thread holds no lease.
	bool validate_prices();

	// The state the jobs left (billing_sync_state), under this configuration:
	// prices checked under other price lists or another mode read unchecked.
	SyncStatus status(db::Database &db) const;
	// The heartbeat's billing_sync_age_s: the age of the oldest event not yet
	// synced, or, while the last reconcile failed, the time since the last one
	// that completed (since the sync first ran for none), whichever is more.
	int64_t sync_age_s(db::Database &db, int64_t now) const;

	const std::string &instance_id() const { return instance_id_; }
	// Its clock now (the jobs run at it).
	int64_t now() const { return clock_(); }

private:
	// sync_customer's work, its reads and writes on `held`, or with none (the
	// job's) each step on a connection leased for that step alone.
	SyncOutcome sync_one(db::Database *held, const std::string &stripe_customer_id);
	// A subscription of ours as a row for `user_id`.
	SubscriptionRow row_of(const Subscription &s, std::optional<int64_t> user_id) const;
	// The account a customer of ours names: its row's, else its metadata's
	// opennova_user_id when that is a player here; nullopt for none.
	std::optional<int64_t> account_of(db::Database &db, const std::string &stripe_customer_id,
			const Metadata &metadata) const;
	// Every subscription of a customer (all statuses); nullopt when Stripe could
	// not be asked.
	std::optional<std::vector<Subscription>> stripe_subscriptions_of(const std::string &stripe_customer_id);
	// The customer gone: its row deleted, its live subscriptions canceled.
	void forget_customer(db::Database &db, const std::string &stripe_customer_id);
	// The price lists and the mode the prices are checked under
	// (billing_sync_state.prices_config).
	std::string prices_config() const;

	db::ConnectionPool &pool_;
	StripeClient &client_;
	const BillingConfig &config_;
	const std::string instance_id_;
	const Clock clock_;
	std::function<void(db::Database &)> on_reconciled_;
};

} // namespace opennova::novaworld_server::billing
