#pragma once

// The billing worker: a thread of the NovaWorld service that keeps the billing
// tables (billing_repository.h) in step with Stripe, the record. A webhook event
// only marks its customer dirty (billing_events); the worker then reads that
// customer and every subscription of it back from Stripe and writes what it
// read, so an event's body is never trusted and the order events arrive in
// does not matter. Service policy over Stripe's REST API, not a port.
//
// Each pass:
//   - reconciles in full at its first pass, every reconcile_minutes
//     (ONNET_BILLING_RECONCILE_MINUTES, 360) and when an admin asks
//     (request_reconcile): every subscription of the account, a page of 100 at
//     a time with its customer expanded (at most kMaxReconcilePages); a
//     subscription whose customer carries this database's instance id
//     (opennova_instance) is ours and written, its customer's row made again
//     when it is missing (the account from the customer's opennova_user_id);
//     a granting one of another instance, or ours with no account here, is an
//     orphan the admin page lists (in memory: the next reconcile lists them
//     afresh); a granting row of ours Stripe no longer lists is read alone,
//     and canceled when Stripe no longer knows it. Accounts with more than one
//     granting tier subscription are listed as duplicates. The configured
//     prices are checked at every reconcile (and at the first pass): each one
//     a recurring monthly price in the key's mode, the one each list sells
//     active, the ones sold in one currency; else prices_ok is false and
//     checkout refuses (503 billing_misconfigured). One that read every
//     subscription with the prices checking out calls on_reconciled.
//   - then syncs the customers with an unprocessed event, the longest-waiting
//     first (kPendingPerPass at most), one sync each: a customer Stripe could
//     not be asked about waits kBackoffFirstS, doubling to kBackoffMaxS.
//
// No Stripe call ever runs inside a db::Transaction: a sync reads Stripe
// first, then writes what it read in one. The thread leases a pooled connection
// per pass. Thread-safe: the routes call sync_customer (POST /api/me/billing/sync)
// and status() from their own threads.

#include "billing_config.h"
#include "billing_repository.h"
#include "entitlement.h"
#include "stripe_client.h"

#include <net/novaworld/db/database.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
	int64_t started_at = 0;          // the worker's start (or its construction)
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
	// The thread's wait between passes when nothing notifies it.
	static constexpr int64_t kIdleWaitMs = 10 * 1000;

	// `config` must be complete (check_billing_config answers "") and, like
	// `client` and `pool`, outlive the worker. `clock` is the wall clock in Unix
	// seconds (the tests step one by hand).
	BillingSync(db::ConnectionPool &pool, StripeClient &client, const BillingConfig &config,
			std::string instance_id, Clock clock = unix_now);
	~BillingSync();
	BillingSync(const BillingSync &) = delete;
	BillingSync &operator=(const BillingSync &) = delete;

	// Called on the worker's thread after each full reconcile that completed
	// with the prices checking out (the hosted servers' limits hold,
	// hosted/limits_hold.h, trusts the cache after one). Set before start().
	void set_on_reconciled(std::function<void()> on_reconciled) { on_reconciled_ = std::move(on_reconciled); }

	// Runs a pass at once, then after every notify() or kIdleWaitMs, on the
	// worker's own thread until stop().
	void start();
	void stop();
	// A webhook event was recorded: the next pass runs now.
	void notify();
	// An admin asked for a full reconcile: the next pass runs one.
	void request_reconcile();

	// One pass at `now` (what the thread runs; the tests drive it by hand).
	void pass(int64_t now);
	// One customer, read back from Stripe and written; its events recorded so
	// far marked with the outcome (but Unreachable's, which stay pending).
	SyncOutcome sync_customer(db::Database &db, const std::string &stripe_customer_id, int64_t now);
	ReconcileReport reconcile_all(int64_t now);
	// The configured prices checked against Stripe; false when Stripe could not
	// be asked (the status keeps what it had).
	bool validate_prices();

	SyncStatus status() const;
	// The heartbeat's billing_sync_age_s: the age of the oldest event not yet
	// synced, or, while the last reconcile failed, the time since the last one
	// that completed (since the worker's start for none), whichever is more.
	int64_t sync_age_s(db::Database &db, int64_t now) const;

	const std::string &instance_id() const { return instance_id_; }

private:
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
	void run();

	db::ConnectionPool &pool_;
	StripeClient &client_;
	const BillingConfig &config_;
	const std::string instance_id_;
	const Clock clock_;
	std::function<void()> on_reconciled_;

	mutable std::mutex status_mu_;
	SyncStatus status_;

	// The worker's own (pass() runs on one thread at a time).
	struct Backoff {
		int64_t next_at = 0;
		int64_t delay = kBackoffFirstS;
	};
	std::map<std::string, Backoff> backoff_;
	int64_t next_reconcile_at_ = 0;
	int64_t next_price_check_at_ = 0; // after a check Stripe could not answer

	std::thread thread_;
	std::mutex wake_mu_;
	std::condition_variable wake_;
	bool woken_ = false;
	bool reconcile_requested_ = false;
	bool stop_requested_ = false;
};

} // namespace opennova::novaworld_server::billing
