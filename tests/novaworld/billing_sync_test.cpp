// The billing sync (apps/novaworld_common/billing/billing_sync.h) against the
// in-memory Stripe (billing_fake_stripe.h) on a migrated database of the test's
// own (common/test_database.h), its jobs' work (reconcile_all, sync_pending)
// driven by hand on a stepped clock, and the jobs themselves (billing_jobs.h)
// run as a worker runs them:
// a webhook event's customer synced into the entitlement, a redelivered event
// recorded once and two events one sync, a customer of another instance
// ignored and listed as an orphan, the billing tables wiped (the instance id
// kept) and rebuilt by a reconcile, a customer Stripe could not be asked about
// left pending with its backoff, a deleted customer's subscriptions canceled, a
// granting row Stripe no longer knows canceled, two granting tiers listed as a
// duplicate, the price check refusing a yearly price and a price of the
// other mode, on_reconciled called after a clean reconcile alone, its state
// kept in the database (another sync over it reads the same), and the
// billing.sync and billing.reconcile jobs enqueued, deduped, scheduled and run.
// Crow-free: compiles the billing sources directly.

#include "billing/billing_config.h"
#include "billing/billing_jobs.h"
#include "billing/billing_repository.h"
#include "billing/billing_sync.h"
#include "billing/entitlement.h"
#include "billing/stripe_client.h"
#include "billing_fake_stripe.h"
#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include "common/test_database.h"
#include "common/test_expect.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace billing = opennova::novaworld_server::billing;
namespace jobs = opennova::novaworld_server::jobs;
using billing::StripeRequest;
using billing_fake::FakeStripeApi;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

constexpr int64_t kStart = 1760000000;
constexpr int64_t kMonth = 30 * 24 * 60 * 60;

billing::BillingConfig test_config() {
	billing::BillingConfig c;
	c.secret_key = "sk_test_billingsync";
	c.webhook_secret = "whsec_billingsync";
	c.site_url = "http://localhost:5173";
	c.tier_prices[1] = { "price_tier1", "price_tier1_old" };
	c.tier_prices[2] = { "price_tier2" };
	c.tier_prices[3] = { "price_tier3" };
	c.own_machine_prices = { "price_own" };
	return c;
}

// One worker over a fresh database and a fake Stripe holding the configured
// prices; its clock in hand.
struct Rig {
	TestDatabase scratch;
	ConnectionPool pool;
	FakeStripeApi fake;
	billing::StripeClient client;
	billing::BillingConfig config = test_config();
	std::string instance;
	int64_t now = kStart;
	std::unique_ptr<billing::BillingSync> sync;
	jobs::JobRegistry registry;

	explicit Rig(size_t pool_max = opennova::db::kDefaultPoolMax) :
			pool(scratch.options(), pool_max), client(fake) {
		for (const char *id : { "price_tier1", "price_tier1_old", "price_tier2", "price_tier3", "price_own" }) {
			FakeStripeApi::Price p;
			p.id = id;
			p.unit_amount = std::string(id) == "price_own" ? 4500 : 500;
			p.active = std::string(id) != "price_tier1_old"; // a grandfathered price, archived
			fake.add_price(p);
		}
		instance = billing::instance_id(*pool.acquire());
		sync = std::make_unique<billing::BillingSync>(pool, client, config, instance, [this] { return now; });
		billing::register_billing_jobs(registry, *sync);
	}

	// A controller's start: the reconcile (it checks the prices), then the sync.
	void first_pass() {
		sync->reconcile_all(now);
		sync->sync_pending(now);
	}
	// A billing.sync job's work at `at`.
	void pass(int64_t at) { sync->sync_pending(at); }
	billing::SyncStatus status() { return sync->status(*pool.acquire()); }
	// Every job due, as a worker runs them.
	size_t run_jobs() { return jobs::run_due(pool, registry); }

	int64_t player(const std::string &name) {
		static int pcid = 0;
		char id[16];
		std::snprintf(id, sizeof(id), "0c%06x", ++pcid);
		const auto rows = pool.acquire()->query("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
												"VALUES ($1, 'x', $2, '1', $1) RETURNING id;",
				{ BindValue(name), BindValue(std::string(id)) });
		return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
	}
	// A customer of this instance's for `user`, as checkout makes it (with its row).
	std::string customer(int64_t user, const std::string &username, bool row = true) {
		const std::string id = fake.add_customer({ { billing::kMetaInstance, instance },
				{ billing::kMetaUserId, std::to_string(user) }, { billing::kMetaUsername, username } });
		if (row) billing::insert_customer(*pool.acquire(), user, id, false);
		return id;
	}
	void event(const std::string &id, const std::string &customer) {
		billing::record_event(*pool.acquire(), id, "customer.subscription.updated", false, customer, now);
	}
	billing::Entitlement entitlement(int64_t user) { return billing::entitlement_of(*pool.acquire(), user, now); }
	size_t pending() { return billing::pending_customers(*pool.acquire(), 100).size(); }
	std::string outcome_of(const std::string &event_id) {
		const auto rows = pool.acquire()->query("SELECT outcome FROM billing_events WHERE event_id = $1;",
				{ BindValue(event_id) });
		return rows.empty() ? std::string("<none>") : rows.front().as_text(0).value_or("");
	}
	size_t customer_reads(const std::string &customer) {
		return fake.count(StripeRequest::Method::Get, "/v1/customers/" + customer);
	}
};

// A webhook event marks its customer; the next pass reads it back and the
// entitlement follows. The first pass reconciles (nothing yet) and checks the
// prices; a redelivered event is recorded once; two events of one customer are
// one sync; a status change moves the entitlement.
int test_event_to_entitlement() {
	Rig rig;
	rig.first_pass();
	billing::SyncStatus status = rig.status();
	TEST_EXPECT(status.prices_checked && status.prices_ok && status.prices_error.empty());
	TEST_EXPECT(status.last_reconcile_at == rig.now && status.last_reconcile_error.empty());
	TEST_EXPECT(status.tier_prices[1] && status.tier_prices[1]->id == "price_tier1" &&
			status.tier_prices[1]->unit_amount == 500 && status.tier_prices[1]->currency == "usd");
	TEST_EXPECT(status.own_machine_price && status.own_machine_price->unit_amount == 4500);

	const int64_t ana = rig.player("sync_ana");
	const std::string cus = rig.customer(ana, "sync_ana");
	const std::string sub = rig.fake.add_subscription(cus, "price_tier2", 1, "active", rig.now + kMonth,
			{ { billing::kMetaInstance, rig.instance } });
	TEST_EXPECT(rig.entitlement(ana).max_servers == 0);
	rig.event("evt_a1", cus);
	rig.event("evt_a1", cus); // redelivered
	rig.event("evt_a2", cus);
	TEST_EXPECT(rig.pool.acquire()->query("SELECT 1 FROM billing_events;").size() == 2);
	rig.now += 5;
	rig.pass(rig.now);
	TEST_EXPECT(rig.customer_reads(cus) == 1);
	TEST_EXPECT(rig.pending() == 0 && rig.outcome_of("evt_a1") == "synced" && rig.outcome_of("evt_a2") == "synced");
	billing::Entitlement e = rig.entitlement(ana);
	TEST_EXPECT(e.max_servers == 2 && e.tier == 2 && e.can_start && e.status == "active");
	const auto row = billing::find_subscription(*rig.pool.acquire(), sub);
	TEST_EXPECT(row && row->item == billing::ItemKind::Tier && row->user_id == ana &&
			row->current_period_end == rig.now - 5 + kMonth);
	TEST_EXPECT(!billing::customer_of(*rig.pool.acquire(), ana)->synced_at.empty());

	// Stripe moves it to past_due: the next event's sync keeps the servers, starts none.
	{
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		rig.fake.subscriptions[sub].status = "past_due";
	}
	rig.event("evt_a3", cus);
	rig.pass(++rig.now);
	e = rig.entitlement(ana);
	TEST_EXPECT(e.max_servers == 2 && !e.can_start);
	// An add-on beside it.
	rig.fake.add_subscription(cus, "price_own", 2, "active", rig.now + kMonth);
	{
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		rig.fake.subscriptions[sub].status = "active";
	}
	rig.event("evt_a4", cus);
	rig.pass(++rig.now);
	e = rig.entitlement(ana);
	TEST_EXPECT(e.max_servers == 2 && e.own_machines == 2 && e.can_start);
	// The synchronous sync a route runs answers the same and marks its events.
	rig.event("evt_a5", cus);
	TEST_EXPECT(rig.sync->sync_customer(*rig.pool.acquire(), cus, rig.now) == billing::SyncOutcome::Synced);
	TEST_EXPECT(rig.outcome_of("evt_a5") == "synced");
	return 0;
}

// A customer of another instance (another developer's database on the shared
// test account): its event is marked foreign and nothing is written; the
// reconcile lists its granting subscription as an orphan, with the username
// its metadata names. One of ours naming no account here is an orphan too.
int test_foreign_and_orphans() {
	Rig rig;
	rig.first_pass();
	const std::string other = rig.fake.add_customer(
			{ { billing::kMetaInstance, "someone-elses-instance" }, { billing::kMetaUserId, "1" },
					{ billing::kMetaUsername, "their_user" } });
	const std::string their_sub = rig.fake.add_subscription(other, "price_tier1", 1, "active", rig.now + kMonth);
	rig.event("evt_f1", other);
	rig.pass(++rig.now);
	TEST_EXPECT(rig.outcome_of("evt_f1") == "foreign");
	TEST_EXPECT(rig.pool.acquire()->query("SELECT 1 FROM billing_subscriptions;").empty());
	// Ours, but its account is gone here.
	const std::string lost = rig.fake.add_customer({ { billing::kMetaInstance, rig.instance },
			{ billing::kMetaUserId, "999999" }, { billing::kMetaUsername, "gone_user" } });
	rig.fake.add_subscription(lost, "price_tier3", 1, "active", rig.now + kMonth);
	rig.event("evt_f2", lost);
	rig.pass(++rig.now);
	TEST_EXPECT(rig.outcome_of("evt_f2") == "orphan");
	// A canceled one of another instance is no orphan.
	rig.fake.add_subscription(other, "price_tier2", 1, "canceled", rig.now + kMonth);

	const billing::ReconcileReport report = rig.sync->reconcile_all(rig.now);
	TEST_EXPECT(report.completed && report.listed == 3 && report.written == 0 && report.orphans == 2);
	const billing::SyncStatus status = rig.status();
	TEST_EXPECT(status.orphans.size() == 2);
	bool theirs = false;
	bool gone = false;
	for (const billing::Orphan &o : status.orphans) {
		theirs = theirs || (o.stripe_subscription_id == their_sub && o.username_hint == "their_user" &&
								   o.stripe_customer_id == other && o.status == "active");
		gone = gone || (o.stripe_customer_id == lost && o.username_hint == "gone_user");
	}
	TEST_EXPECT(theirs && gone);
	TEST_EXPECT(rig.pool.acquire()->query("SELECT 1 FROM billing_subscriptions;").empty());
	return 0;
}

// The billing tables wiped (a restored dump without them, a hand's DELETE),
// the instance id kept: a reconcile reads every subscription back, makes the
// customer rows again from their metadata, and the entitlements return. A
// granting row Stripe no longer knows is read alone and canceled; an account
// paying for two tiers is a duplicate.
int test_reconcile_rebuilds() {
	Rig rig;
	rig.first_pass();
	const int64_t ana = rig.player("rec_ana");
	const int64_t ben = rig.player("rec_ben");
	const std::string ana_cus = rig.customer(ana, "rec_ana");
	const std::string ben_cus = rig.customer(ben, "rec_ben");
	rig.fake.add_subscription(ana_cus, "price_tier3", 1, "active", rig.now + kMonth);
	rig.fake.add_subscription(ben_cus, "price_tier1_old", 1, "trialing", rig.now + kMonth); // grandfathered
	rig.fake.add_subscription(ben_cus, "price_tier2", 1, "active", rig.now + kMonth);
	TEST_EXPECT(rig.sync->reconcile_all(rig.now).completed);
	TEST_EXPECT(rig.entitlement(ana).max_servers == 3 && rig.entitlement(ben).max_servers == 2);
	TEST_EXPECT(rig.status().duplicates == std::vector<int64_t>{ ben });

	{
		auto db = rig.pool.acquire();
		db->exec("DELETE FROM billing_subscriptions;");
		db->exec("DELETE FROM billing_customers;");
		db->exec("DELETE FROM billing_events;");
	}
	TEST_EXPECT(rig.entitlement(ana).max_servers == 0);
	TEST_EXPECT(billing::instance_id(*rig.pool.acquire()) == rig.instance);
	const billing::ReconcileReport report = rig.sync->reconcile_all(++rig.now);
	TEST_EXPECT(report.completed && report.written == 3 && report.customers_recreated == 2 && report.orphans == 0);
	TEST_EXPECT(rig.entitlement(ana).max_servers == 3 && rig.entitlement(ben).max_servers == 2);
	TEST_EXPECT(billing::user_of_customer(*rig.pool.acquire(), ana_cus) == ana);
	TEST_EXPECT(billing::user_of_customer(*rig.pool.acquire(), ben_cus) == ben);

	// A granting row Stripe no longer knows (its test data deleted).
	billing::SubscriptionRow stale;
	stale.stripe_subscription_id = "sub_vanished";
	stale.stripe_customer_id = ana_cus;
	stale.user_id = ana;
	stale.status = "active";
	stale.item = billing::ItemKind::OwnMachine;
	stale.price_id = "price_own";
	stale.current_period_end = rig.now + kMonth;
	billing::upsert_subscription(*rig.pool.acquire(), stale);
	TEST_EXPECT(rig.entitlement(ana).own_machines == 1);
	const billing::ReconcileReport again = rig.sync->reconcile_all(++rig.now);
	TEST_EXPECT(again.completed && again.canceled == 1);
	TEST_EXPECT(billing::find_subscription(*rig.pool.acquire(), "sub_vanished")->status == "canceled");
	TEST_EXPECT(rig.entitlement(ana).own_machines == 0);
	TEST_EXPECT(rig.fake.count(StripeRequest::Method::Get, "/v1/subscriptions/sub_vanished") == 1);
	return 0;
}

// Stripe unreachable for a customer: its events stay pending, and it waits its
// backoff (60 s, doubling) before the next try, which syncs it. A deleted
// customer's subscriptions are canceled here and its row goes.
int test_backoff_and_deleted() {
	Rig rig;
	rig.first_pass();
	const int64_t ana = rig.player("back_ana");
	const std::string cus = rig.customer(ana, "back_ana");
	rig.fake.add_subscription(cus, "price_tier1", 1, "active", rig.now + kMonth);
	rig.event("evt_b1", cus);
	rig.fake.fail_next(billing::StripeReply::Outcome::Unreachable);
	rig.pass(++rig.now);
	TEST_EXPECT(rig.pending() == 1 && rig.outcome_of("evt_b1").empty());
	TEST_EXPECT(rig.customer_reads(cus) == 1);
	// Within the backoff: not asked again.
	rig.now += 30;
	rig.pass(rig.now);
	TEST_EXPECT(rig.customer_reads(cus) == 1 && rig.pending() == 1);
	// A second failure doubles it.
	rig.now += 31;
	rig.fake.fail_next(503);
	rig.pass(rig.now);
	TEST_EXPECT(rig.customer_reads(cus) == 2 && rig.pending() == 1);
	rig.now += 61;
	rig.pass(rig.now);
	TEST_EXPECT(rig.customer_reads(cus) == 2);
	rig.now += 60;
	rig.pass(rig.now);
	TEST_EXPECT(rig.customer_reads(cus) == 3 && rig.pending() == 0 && rig.outcome_of("evt_b1") == "synced");
	TEST_EXPECT(rig.entitlement(ana).max_servers == 1);
	// The heartbeat's age: nothing pending.
	TEST_EXPECT(rig.sync->sync_age_s(*rig.pool.acquire(), rig.now) == 0);

	// The customer deleted in Stripe (its subscriptions canceled there).
	rig.fake.delete_customer(cus);
	rig.event("evt_b2", cus);
	rig.pass(++rig.now);
	TEST_EXPECT(rig.outcome_of("evt_b2") == "deleted");
	TEST_EXPECT(!billing::customer_of(*rig.pool.acquire(), ana));
	TEST_EXPECT(rig.entitlement(ana).max_servers == 0);
	return 0;
}

// The price check: a yearly price, a live-mode price under a test key, or a sold
// price archived turns prices_ok off with the reason; Stripe unreachable leaves
// the last check standing.
int test_price_validation() {
	{
		Rig rig;
		{
			std::lock_guard<std::mutex> lock(rig.fake.mu);
			rig.fake.prices["price_tier2"].interval = "year";
		}
		TEST_EXPECT(rig.sync->validate_prices());
		const billing::SyncStatus status = rig.status();
		TEST_EXPECT(status.prices_checked && !status.prices_ok);
		TEST_EXPECT(status.prices_error.find("STRIPE_PRICE_TIER2") != std::string::npos &&
				status.prices_error.find("monthly") != std::string::npos);
		// Fixed: the next check passes.
		{
			std::lock_guard<std::mutex> lock(rig.fake.mu);
			rig.fake.prices["price_tier2"].interval = "month";
		}
		TEST_EXPECT(rig.sync->validate_prices() && rig.status().prices_ok);
		// Unreachable: the last answer stands.
		rig.fake.fail_next(billing::StripeReply::Outcome::Unreachable);
		TEST_EXPECT(!rig.sync->validate_prices() && rig.status().prices_ok);
		// The sold price archived.
		{
			std::lock_guard<std::mutex> lock(rig.fake.mu);
			rig.fake.prices["price_own"].active = false;
		}
		TEST_EXPECT(rig.sync->validate_prices() && !rig.status().prices_ok);
		TEST_EXPECT(
				rig.status().prices_error.find("STRIPE_PRICE_OWN_MACHINE") != std::string::npos);
	}
	{
		Rig rig;
		rig.fake.livemode = true; // a live account's prices, the key a test one
		TEST_EXPECT(rig.sync->validate_prices());
		const billing::SyncStatus status = rig.status();
		TEST_EXPECT(!status.prices_ok && status.prices_error.find("live-mode") != std::string::npos);
		// A reconcile checks them too; a failed one ages the sync (from its first run).
		rig.fake.livemode = false;
		rig.fake.fail_next(billing::StripeReply::Outcome::Unreachable);
		TEST_EXPECT(!rig.sync->reconcile_all(rig.now).completed);
		TEST_EXPECT(
				!rig.status().last_reconcile_error.empty() && rig.status().started_at == rig.now);
		TEST_EXPECT(rig.sync->sync_age_s(*rig.pool.acquire(), rig.now + 100) == 100);
		// Its job (billing.reconcile) waits kReconcileRetryS after a failure.
		TEST_EXPECT(billing::enqueue_billing_reconcile(*rig.pool.acquire()) != 0);
		rig.fake.fail_next(billing::StripeReply::Outcome::Unreachable);
		TEST_EXPECT(rig.run_jobs() == 1 && rig.run_jobs() == 0);
		TEST_EXPECT(rig.pool.acquire()
							->query("SELECT 1 FROM jobs WHERE kind = 'billing.reconcile' AND state "
									"= 'queued' "
									"AND run_at > now() + interval '14 minutes';")
							.size() == 1);
		// Asked again by an admin: a reconcile at once.
		rig.now += 101;
		TEST_EXPECT(rig.sync->reconcile_all(rig.now).completed);
		TEST_EXPECT(rig.status().last_reconcile_error.empty() && rig.status().prices_ok);
		TEST_EXPECT(rig.status().last_reconcile_at == rig.now);
	}
	return 0;
}

// on_reconciled (the hosted servers' limits hold trusts the cache after it):
// called after a reconcile that read every subscription with the prices
// checking out, and after no other: not when Stripe could not be asked, not
// when a price is wrong, not while a subscription of ours sells a price the
// configuration does not name (listed in unknown_prices).
int test_on_reconciled() {
	Rig rig;
	int calls = 0;
	rig.sync->set_on_reconciled([&calls](Database &) { ++calls; });
	TEST_EXPECT(rig.sync->reconcile_all(rig.now).completed && calls == 1);
	rig.fake.fail_next(billing::StripeReply::Outcome::Unreachable);
	TEST_EXPECT(!rig.sync->reconcile_all(++rig.now).completed && calls == 1);
	{
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		rig.fake.prices["price_tier2"].interval = "year";
	}
	TEST_EXPECT(
			rig.sync->reconcile_all(++rig.now).completed && !rig.status().prices_ok && calls == 1);
	{
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		rig.fake.prices["price_tier2"].interval = "month";
	}
	// The job's reconcile (a worker's) calls it too.
	TEST_EXPECT(billing::enqueue_billing_reconcile(*rig.pool.acquire()) != 0);
	++rig.now;
	TEST_EXPECT(rig.run_jobs() == 1);
	TEST_EXPECT(rig.status().prices_ok && calls == 2);
	TEST_EXPECT(rig.status().unknown_prices.empty());

	// A subscription of ours selling a price no list names (one sold, then
	// left out of the configuration): listed, and no confirmation while it
	// would grant; canceled, the next reconcile confirms again.
	const int64_t ana = rig.player("onr_ana");
	const std::string cus = rig.customer(ana, "onr_ana");
	const std::string sub = rig.fake.add_subscription(cus, "price_unlisted", 1, "active", rig.now + kMonth);
	TEST_EXPECT(rig.sync->reconcile_all(++rig.now).completed && rig.status().prices_ok);
	TEST_EXPECT(rig.status().unknown_prices == std::vector<std::string>{ "price_unlisted" } &&
			calls == 2);
	TEST_EXPECT(rig.entitlement(ana).max_servers == 0);
	{
		std::lock_guard<std::mutex> lock(rig.fake.mu);
		rig.fake.subscriptions[sub].status = "canceled";
	}
	TEST_EXPECT(rig.sync->reconcile_all(++rig.now).completed);
	TEST_EXPECT(rig.status().unknown_prices.empty() && calls == 3);
	return 0;
}

// The jobs: a fresh webhook event's billing.sync (one queued however many
// events come) syncs its customer on a worker; the schedule enqueues the
// reconcile at the start and a billing.sync bucket only while an event waits;
// and the state is the database's: another sync over the same database (a
// worker in another process) reads the prices, the reconcile and the backoff
// this one wrote, and prices checked under other price lists read unchecked.
int test_jobs_and_state() {
	Rig rig;
	TEST_EXPECT(!rig.status().prices_checked);
	{
		auto db = rig.pool.acquire();
		const auto schedule = billing::billing_schedule(rig.config.reconcile_minutes);
		jobs::Scheduler scheduler(rig.pool, schedule, [&rig] { return rig.now * 1000; });
		scheduler.enqueue_at_start(*db, rig.now * 1000);
		// No event waits: the sync's bucket stays empty.
		TEST_EXPECT(scheduler.tick(*db, rig.now * 1000) == 0);
	}
	TEST_EXPECT(rig.run_jobs() == 1 && rig.status().prices_ok &&
			rig.status().last_reconcile_at == rig.now);
	const int64_t ana = rig.player("job_ana");
	const std::string cus = rig.customer(ana, "job_ana");
	rig.fake.add_subscription(cus, "price_tier1", 1, "active", rig.now + kMonth);
	rig.event("evt_j1", cus);
	rig.event("evt_j2", cus);
	{
		auto db = rig.pool.acquire();
		TEST_EXPECT(billing::enqueue_billing_sync(*db) != 0);
		TEST_EXPECT(billing::enqueue_billing_sync(*db) == 0); // one waits already
	}
	TEST_EXPECT(rig.run_jobs() == 1 && rig.pending() == 0 && rig.entitlement(ana).max_servers == 1);
	// A customer Stripe cannot be asked about: its backoff is a row.
	const std::string ben_cus = rig.customer(rig.player("job_ben"), "job_ben");
	rig.event("evt_j3", ben_cus);
	rig.fake.fail_next(billing::StripeReply::Outcome::Unreachable);
	rig.pass(++rig.now);
	TEST_EXPECT(rig.pending() == 1);
	{
		auto db = rig.pool.acquire();
		TEST_EXPECT(
				db->query("SELECT next_s FROM billing_sync_backoff WHERE stripe_customer_id = $1;",
						  { BindValue(ben_cus) })
						.front()
						.as_int(0) == rig.now + billing::BillingSync::kBackoffFirstS);
		// While it waits, the schedule enqueues the sync's bucket.
		jobs::Scheduler scheduler(
				rig.pool, billing::billing_schedule(rig.config.reconcile_minutes));
		scheduler.tick(*db, (rig.now + 120) * 1000);
		TEST_EXPECT(
				db->query("SELECT 1 FROM jobs WHERE kind = 'billing.sync' AND state = 'queued';")
						.size() == 1);
	}
	// Another sync over the same database (a worker elsewhere) reads it all,
	// and keeps the customer's backoff.
	billing::BillingSync other(
			rig.pool, rig.client, rig.config, rig.instance, [&rig] { return rig.now; });
	{
		const billing::SyncStatus status = other.status(*rig.pool.acquire());
		TEST_EXPECT(status.prices_checked && status.prices_ok && status.tier_prices[1] &&
				status.tier_prices[1]->unit_amount == 500 && status.last_reconcile_at > 0);
	}
	const size_t reads = rig.customer_reads(ben_cus);
	other.sync_pending(rig.now + 10);
	TEST_EXPECT(rig.customer_reads(ben_cus) == reads && rig.pending() == 1);
	other.sync_pending(rig.now + billing::BillingSync::kBackoffFirstS);
	TEST_EXPECT(rig.customer_reads(ben_cus) == reads + 1 && rig.pending() == 0);
	TEST_EXPECT(rig.pool.acquire()->query("SELECT 1 FROM billing_sync_backoff;").empty());
	// Other price lists: unchecked until a check under them.
	billing::BillingConfig changed = rig.config;
	changed.tier_prices[2] = { "price_tier3" };
	billing::BillingSync reconfigured(
			rig.pool, rig.client, changed, rig.instance, [&rig] { return rig.now; });
	TEST_EXPECT(!reconfigured.status(*rig.pool.acquire()).prices_checked);
	return 0;
}

// A billing.sync job holds no pooled connection while Stripe answers: on a pool of
// one, the connection stays free for the rest of the process (the job pool's keeper,
// a route) while a customer's read waits at Stripe, and the sync then writes what it
// read.
int test_no_connection_across_stripe() {
	Rig rig(1);
	rig.first_pass();
	const int64_t cy = rig.player("sync_cy");
	const std::string cus = rig.customer(cy, "sync_cy");
	rig.fake.add_subscription(cus, "price_tier2", 1, "active", rig.now + kMonth,
			{ { billing::kMetaInstance, rig.instance } });
	rig.event("evt_c1", cus);
	rig.fake.close_gate();
	std::thread job([&rig] { rig.pass(rig.now + 5); });
	const bool at_stripe = rig.fake.wait_at_gate(1, std::chrono::seconds(10));
	bool free = false;
	if (at_stripe) {
		try {
			free = !rig.pool.acquire()->query("SELECT 1;").empty();
		} catch (const opennova::db::Error &e) {
			std::fprintf(stderr, "  the pool's connection while the sync waits at Stripe: %s\n",
					e.what());
		}
	}
	rig.fake.open_gate();
	job.join();
	TEST_EXPECT(at_stripe && free);
	TEST_EXPECT(rig.pending() == 0 && rig.outcome_of("evt_c1") == "synced");
	TEST_EXPECT(rig.entitlement(cy).tier == 2);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "event_to_entitlement", test_event_to_entitlement },
		{ "foreign_and_orphans", test_foreign_and_orphans },
		{ "reconcile_rebuilds", test_reconcile_rebuilds },
		{ "backoff_and_deleted", test_backoff_and_deleted },
		{ "price_validation", test_price_validation },
		{ "on_reconciled", test_on_reconciled },
		{ "jobs_and_state", test_jobs_and_state },
		{ "no_connection_across_stripe", test_no_connection_across_stripe },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the billing sync\n");
	return 0;
}
