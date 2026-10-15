#include "billing_sync.h"

#include "loggable.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <set>
#include <utility>

namespace opennova::novaworld_server::billing {

namespace {

std::string meta(const Metadata &metadata, const char *key) {
	const auto it = metadata.find(key);
	return it == metadata.end() ? std::string() : it->second;
}

bool is_terminal(const std::string &status) { return status == "canceled" || status == "incomplete_expired"; }

// "rejected 404 (resource_missing)" for a log line; never a key.
std::string error_text(const StripeError &e) {
	std::string out = stripe_error_kind_name(e.kind);
	if (e.status != 0) out += " " + std::to_string(e.status);
	if (!e.stripe_code.empty()) out += " (" + e.stripe_code + ")";
	if (!e.message.empty()) out += ": " + loggable(e.message, 200);
	return out;
}

// Stripe no longer knows it (in this mode, on this account).
bool is_missing(const StripeError &e) { return e.kind == StripeError::Kind::Rejected && e.status == 404; }

} // namespace

const char *sync_outcome_name(SyncOutcome outcome) {
	switch (outcome) {
		case SyncOutcome::Synced: return "synced";
		case SyncOutcome::Foreign: return "foreign";
		case SyncOutcome::Deleted: return "deleted";
		case SyncOutcome::Orphan: return "orphan";
		case SyncOutcome::Unreachable: return "unreachable";
	}
	return "unknown";
}

BillingSync::BillingSync(db::ConnectionPool &pool, StripeClient &client, const BillingConfig &config,
		std::string instance_id, Clock clock) :
		pool_(pool), client_(client), config_(config), instance_id_(std::move(instance_id)), clock_(std::move(clock)) {
	status_.started_at = clock_();
}

BillingSync::~BillingSync() { stop(); }

void BillingSync::start() {
	if (thread_.joinable()) return;
	{
		std::lock_guard<std::mutex> lock(wake_mu_);
		stop_requested_ = false;
	}
	{
		std::lock_guard<std::mutex> lock(status_mu_);
		status_.started_at = clock_();
	}
	std::printf("[billing] sync worker: %s mode, instance %s, a full reconcile every %d min\n",
			config_.live() ? "live" : "test", instance_id_.c_str(), config_.reconcile_minutes);
	thread_ = std::thread([this] { run(); });
}

void BillingSync::stop() {
	{
		std::lock_guard<std::mutex> lock(wake_mu_);
		stop_requested_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) thread_.join();
}

void BillingSync::notify() {
	{
		std::lock_guard<std::mutex> lock(wake_mu_);
		woken_ = true;
	}
	wake_.notify_all();
}

void BillingSync::request_reconcile() {
	{
		std::lock_guard<std::mutex> lock(wake_mu_);
		reconcile_requested_ = true;
		woken_ = true;
	}
	wake_.notify_all();
}

void BillingSync::run() {
	for (;;) {
		try {
			pass(clock_());
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[billing] WARN sync pass failed: %s\n", e.what());
		}
		std::unique_lock<std::mutex> lock(wake_mu_);
		wake_.wait_for(lock, std::chrono::milliseconds(kIdleWaitMs), [this] { return woken_ || stop_requested_; });
		if (stop_requested_) return;
		woken_ = false;
	}
}

void BillingSync::pass(int64_t now) {
	bool asked = false;
	{
		std::lock_guard<std::mutex> lock(wake_mu_);
		asked = reconcile_requested_;
		reconcile_requested_ = false;
	}
	if (asked || now >= next_reconcile_at_) {
		const ReconcileReport report = reconcile_all(now);
		next_reconcile_at_ = now + (report.completed ? int64_t{ config_.reconcile_minutes } * 60 : kReconcileRetryS);
	} else {
		bool checked = false;
		{
			std::lock_guard<std::mutex> lock(status_mu_);
			checked = status_.prices_checked;
		}
		if (!checked && now >= next_price_check_at_ && !validate_prices()) next_price_check_at_ = now + kBackoffFirstS;
	}

	auto lease = pool_.acquire();
	db::Database &db = *lease;
	for (const std::string &customer : pending_customers(db, kPendingPerPass)) {
		Backoff &backoff = backoff_[customer];
		if (now < backoff.next_at) continue;
		SyncOutcome outcome = SyncOutcome::Unreachable;
		try {
			outcome = sync_customer(db, customer, now);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[billing] WARN sync %s: %s\n", loggable(customer).c_str(), e.what());
		}
		if (outcome == SyncOutcome::Unreachable) {
			backoff.next_at = now + backoff.delay;
			std::printf("[billing] sync %s: Stripe unreachable; again in %lld s\n", loggable(customer).c_str(),
					static_cast<long long>(backoff.delay));
			backoff.delay = std::min(backoff.delay * 2, kBackoffMaxS);
		} else {
			backoff_.erase(customer);
		}
	}
	// Forget the backoffs of customers no longer pending (synced by a route).
	if (backoff_.size() > 4 * kPendingPerPass) {
		const std::vector<std::string> pending = pending_customers(db, 4 * kPendingPerPass);
		const std::set<std::string> keep(pending.begin(), pending.end());
		for (auto it = backoff_.begin(); it != backoff_.end();)
			it = keep.count(it->first) == 0 ? backoff_.erase(it) : std::next(it);
	}
}

SubscriptionRow BillingSync::row_of(const Subscription &s, std::optional<int64_t> user_id) const {
	SubscriptionRow row;
	row.stripe_subscription_id = s.id;
	row.stripe_customer_id = s.customer;
	row.user_id = user_id;
	row.livemode = s.livemode;
	row.status = s.status;
	row.price_id = s.first_price_id();
	row.tier = tier_of_price(config_, row.price_id);
	row.item = row.tier > 0 ? ItemKind::Tier
							: is_own_machine_price(config_, row.price_id) ? ItemKind::OwnMachine : ItemKind::Unknown;
	row.quantity = std::max<int64_t>(s.first_quantity(), 0);
	if (const int64_t end = s.first_period_end(); end > 0) row.current_period_end = end;
	// A cancel the Portal scheduled reads either way, by version.
	row.cancel_at_period_end = s.cancel_at_period_end || s.cancel_at > 0;
	return row;
}

std::optional<int64_t> BillingSync::account_of(db::Database &db, const std::string &stripe_customer_id,
		const Metadata &metadata) const {
	if (const auto user = user_of_customer(db, stripe_customer_id)) return user;
	const auto id = strutil::parse_llong(meta(metadata, kMetaUserId));
	if (!id || *id <= 0) return std::nullopt;
	const auto rows = db.query("SELECT 1 FROM players WHERE id = $1 AND kind = 'player';",
			{ db::BindValue(static_cast<int64_t>(*id)) });
	if (rows.empty()) return std::nullopt;
	return *id;
}

std::optional<std::vector<Subscription>> BillingSync::stripe_subscriptions_of(const std::string &stripe_customer_id) {
	std::vector<Subscription> out;
	SubscriptionQuery query;
	query.customer = stripe_customer_id;
	for (int page = 0; page < kMaxReconcilePages; ++page) {
		const StripeResult<SubscriptionPage> listed = client_.list_subscriptions(query);
		if (!listed.ok()) {
			std::fprintf(stderr, "[billing] WARN listing %s's subscriptions: %s\n", loggable(stripe_customer_id).c_str(),
					error_text(listed.error).c_str());
			return std::nullopt;
		}
		for (const Subscription &s : listed.value->data) out.push_back(s);
		if (!listed.value->has_more || listed.value->data.empty()) return out;
		query.starting_after = listed.value->data.back().id;
	}
	return out;
}

void BillingSync::forget_customer(db::Database &db, const std::string &stripe_customer_id) {
	db::Transaction tx(db);
	end_subscriptions_of_customer(db, stripe_customer_id, {}, "canceled");
	delete_customer(db, stripe_customer_id);
	tx.commit();
}

SyncOutcome BillingSync::sync_customer(db::Database &db, const std::string &stripe_customer_id, int64_t now) {
	(void)now;
	// The events marked are those recorded by now: one delivered while Stripe is
	// read waits for the next sync.
	const std::string started = database_clock(db);
	const auto finish = [&](SyncOutcome outcome) {
		mark_processed(db, stripe_customer_id, sync_outcome_name(outcome), started);
		return outcome;
	};
	const StripeResult<Customer> customer = client_.get_customer(stripe_customer_id);
	if (!customer.ok()) {
		if (is_missing(customer.error)) {
			forget_customer(db, stripe_customer_id);
			std::printf("[billing] customer %s: Stripe knows none; its subscriptions canceled here\n",
					loggable(stripe_customer_id).c_str());
			return finish(SyncOutcome::Deleted);
		}
		std::fprintf(stderr, "[billing] WARN customer %s: %s\n", loggable(stripe_customer_id).c_str(),
				error_text(customer.error).c_str());
		return SyncOutcome::Unreachable;
	}
	if (customer.value->deleted) {
		forget_customer(db, stripe_customer_id);
		std::printf("[billing] customer %s deleted in Stripe; its subscriptions canceled here\n",
				loggable(stripe_customer_id).c_str());
		return finish(SyncOutcome::Deleted);
	}
	if (meta(customer.value->metadata, kMetaInstance) != instance_id_ || customer.value->livemode != config_.live())
		return finish(SyncOutcome::Foreign);
	const std::optional<int64_t> user = account_of(db, stripe_customer_id, customer.value->metadata);
	if (!user) {
		std::printf("[billing] customer %s names no account here (an orphan)\n", loggable(stripe_customer_id).c_str());
		return finish(SyncOutcome::Orphan);
	}
	const std::optional<std::vector<Subscription>> subscriptions = stripe_subscriptions_of(stripe_customer_id);
	if (!subscriptions) return SyncOutcome::Unreachable;

	std::vector<std::string> listed;
	{
		db::Transaction tx(db);
		if (!customer_by_id(db, stripe_customer_id) && !customer_of(db, *user))
			insert_customer(db, *user, stripe_customer_id, customer.value->livemode);
		for (const Subscription &s : *subscriptions) {
			upsert_subscription(db, row_of(s, user));
			listed.push_back(s.id);
		}
		// One Stripe lists no longer (deleted with its test data): canceled.
		end_subscriptions_of_customer(db, stripe_customer_id, listed, "canceled");
		mark_customer_synced(db, stripe_customer_id);
		tx.commit();
	}
	std::printf("[billing] customer %s (account %lld): %zu subscription(s) synced\n",
			loggable(stripe_customer_id).c_str(), static_cast<long long>(*user), subscriptions->size());
	return finish(SyncOutcome::Synced);
}

bool BillingSync::validate_prices() {
	std::string problem;
	std::array<std::optional<PriceInfo>, kTierCount + 1> tier_prices;
	std::optional<PriceInfo> own_price;
	std::string currency;
	const auto check_list = [&](const std::vector<std::string> &list, const std::string &variable,
									std::optional<PriceInfo> &sold) -> bool {
		for (size_t i = 0; i < list.size(); ++i) {
			const StripeResult<Price> price = client_.get_price(list[i]);
			if (!price.ok()) {
				if (price.error.kind == StripeError::Kind::Unreachable) {
					std::fprintf(stderr, "[billing] WARN price check: %s: %s\n", loggable(list[i]).c_str(),
							error_text(price.error).c_str());
					return false; // try again later
				}
				if (problem.empty())
					problem = variable + ": Stripe answers no price " + loggable(list[i], 80) + " (" +
							error_text(price.error) + ")";
				continue;
			}
			const Price &p = *price.value;
			std::string why;
			if (p.type != "recurring" || p.recurring_interval != "month" || p.recurring_interval_count != 1)
				why = "is not a monthly recurring price";
			else if (p.livemode != config_.live())
				why = std::string("is a ") + (p.livemode ? "live" : "test") + "-mode price, the key a " +
						(config_.live() ? "live" : "test") + "-mode one";
			else if (i == 0 && !p.active)
				why = "is archived, and it is the one sold";
			else if (i == 0 && !currency.empty() && p.currency != currency)
				why = "is in " + loggable(p.currency, 8) + ", the others in " + currency;
			if (!why.empty()) {
				if (problem.empty()) problem = variable + ": " + loggable(list[i], 80) + " " + why;
				continue;
			}
			if (i == 0) {
				if (currency.empty()) currency = p.currency;
				PriceInfo info;
				info.id = p.id;
				info.unit_amount = p.unit_amount;
				info.currency = p.currency;
				info.interval = p.recurring_interval;
				sold = info;
			}
		}
		return true;
	};
	for (int tier = 1; tier <= kTierCount; ++tier) {
		if (!check_list(config_.tier_prices[tier], "STRIPE_PRICE_TIER" + std::to_string(tier), tier_prices[tier]))
			return false;
	}
	if (!check_list(config_.own_machine_prices, "STRIPE_PRICE_OWN_MACHINE", own_price)) return false;
	{
		std::lock_guard<std::mutex> lock(status_mu_);
		const bool was_ok = status_.prices_ok;
		const bool was_checked = status_.prices_checked;
		status_.prices_checked = true;
		status_.prices_ok = problem.empty();
		status_.prices_error = problem;
		status_.tier_prices = tier_prices;
		status_.own_machine_price = own_price;
		if (!problem.empty() && (was_ok || !was_checked))
			std::fprintf(stderr, "[billing] WARN the prices: %s; checkout is off until they are fixed\n",
					problem.c_str());
		else if (problem.empty() && !was_ok)
			std::printf("[billing] the prices check out\n");
	}
	return true;
}

ReconcileReport BillingSync::reconcile_all(int64_t now) {
	ReconcileReport report;
	const auto fail = [&](std::string why) {
		report.error = std::move(why);
		std::fprintf(stderr, "[billing] WARN reconcile: %s\n", report.error.c_str());
		std::lock_guard<std::mutex> lock(status_mu_);
		status_.last_reconcile_error = report.error;
		return report;
	};
	if (!validate_prices()) return fail("Stripe could not be asked for the prices");

	// Every subscription, read first: no Stripe call inside the writes below.
	std::vector<Subscription> all;
	SubscriptionQuery query;
	query.expand_customer = true;
	bool whole = false;
	for (int page = 0; page < kMaxReconcilePages; ++page) {
		const StripeResult<SubscriptionPage> listed = client_.list_subscriptions(query);
		if (!listed.ok()) return fail("Stripe could not list the subscriptions: " + error_text(listed.error));
		for (const Subscription &s : listed.value->data) all.push_back(s);
		if (!listed.value->has_more || listed.value->data.empty()) {
			whole = true;
			break;
		}
		query.starting_after = listed.value->data.back().id;
	}
	report.listed = all.size();

	auto lease = pool_.acquire();
	db::Database &db = *lease;
	std::vector<Orphan> orphans;
	struct Ours {
		const Subscription *subscription;
		int64_t user;
	};
	std::vector<Ours> ours;
	std::set<std::string> listed_ids;
	for (const Subscription &s : all) {
		listed_ids.insert(s.id);
		const Metadata customer_meta = s.customer_metadata.value_or(Metadata{});
		const bool granting = status_grants(s.status);
		const bool mine = !s.customer_deleted && s.livemode == config_.live() &&
				meta(customer_meta, kMetaInstance) == instance_id_;
		const std::optional<int64_t> user = mine ? account_of(db, s.customer, customer_meta) : std::nullopt;
		if (mine && user) {
			ours.push_back({ &s, *user });
			continue;
		}
		if (granting) {
			Orphan o;
			o.stripe_customer_id = s.customer;
			o.stripe_subscription_id = s.id;
			o.username_hint = meta(customer_meta, kMetaUsername);
			o.status = s.status;
			orphans.push_back(std::move(o));
		}
	}
	// A granting row of ours Stripe did not list: read alone (a listing that
	// stopped at its page cap proves nothing, so then none is).
	std::vector<SubscriptionRow> refreshed;
	std::vector<std::string> gone;
	if (whole) {
		for (const SubscriptionRow &row : list_subscriptions(db)) {
			if (listed_ids.count(row.stripe_subscription_id) != 0 || !subscription_grants(row, now)) continue;
			const StripeResult<Subscription> one = client_.get_subscription(row.stripe_subscription_id);
			if (one.ok()) {
				refreshed.push_back(row_of(*one.value, row.user_id));
			} else if (is_missing(one.error)) {
				gone.push_back(row.stripe_subscription_id);
			} else {
				return fail("Stripe could not be asked for " + loggable(row.stripe_subscription_id, 80) + ": " +
						error_text(one.error));
			}
		}
	}
	{
		db::Transaction tx(db);
		for (const Ours &o : ours) {
			if (!customer_by_id(db, o.subscription->customer) && !customer_of(db, o.user) &&
					insert_customer(db, o.user, o.subscription->customer, o.subscription->livemode))
				++report.customers_recreated;
			upsert_subscription(db, row_of(*o.subscription, o.user));
			++report.written;
		}
		for (const SubscriptionRow &row : refreshed) upsert_subscription(db, row);
		for (const std::string &id : gone) {
			db.exec("UPDATE billing_subscriptions SET status = 'canceled', status_since = now(), updated_at = now() "
					"WHERE stripe_subscription_id = $1 AND status <> 'canceled';",
					{ db::BindValue(id) });
			report.canceled += static_cast<size_t>(db.changes());
		}
		tx.commit();
	}
	// The accounts paying for two tiers at once (a second checkout raced the
	// first, or a subscription made by hand).
	std::map<int64_t, int> granting_tiers;
	for (const SubscriptionRow &row : list_subscriptions(db)) {
		if (row.user_id && row.item == ItemKind::Tier && subscription_grants(row, now)) ++granting_tiers[*row.user_id];
	}
	std::vector<int64_t> duplicates;
	for (const auto &[user, count] : granting_tiers) {
		if (count > 1) duplicates.push_back(user);
	}
	report.orphans = orphans.size();
	report.completed = whole;
	{
		std::lock_guard<std::mutex> lock(status_mu_);
		status_.orphans = std::move(orphans);
		status_.duplicates = duplicates;
		if (whole) {
			status_.last_reconcile_at = now;
			status_.last_reconcile_error.clear();
		} else {
			status_.last_reconcile_error = "Stripe listed more than " + std::to_string(kMaxReconcilePages * 100) +
					" subscriptions; the rest were not read";
			report.error = status_.last_reconcile_error;
		}
	}
	std::printf("[billing] reconcile: %zu subscription(s) listed, %zu of ours written, %zu customer row(s) made "
				"again, %zu canceled, %zu orphan(s), %zu account(s) with two tiers\n",
			report.listed, report.written, report.customers_recreated, report.canceled, report.orphans,
			duplicates.size());
	return report;
}

SyncStatus BillingSync::status() const {
	std::lock_guard<std::mutex> lock(status_mu_);
	return status_;
}

int64_t BillingSync::sync_age_s(db::Database &db, int64_t now) const {
	int64_t age = oldest_pending_age_s(db);
	std::lock_guard<std::mutex> lock(status_mu_);
	if (!status_.last_reconcile_error.empty()) {
		const int64_t since = status_.last_reconcile_at > 0 ? status_.last_reconcile_at : status_.started_at;
		age = std::max(age, now - since);
	}
	return std::max<int64_t>(age, 0);
}

} // namespace opennova::novaworld_server::billing
