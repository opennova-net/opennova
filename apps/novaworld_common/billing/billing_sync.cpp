#include "billing_sync.h"

#include "loggable.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <utility>

namespace opennova::novaworld_server::billing {

namespace {

std::string meta(const Metadata &metadata, const char *key) {
	const auto it = metadata.find(key);
	return it == metadata.end() ? std::string() : it->second;
}

bool is_terminal(const std::string &status) { return status == "canceled" || status == "incomplete_expired"; }

// The reservation of the checkout that made `row` (its opennova_reservation
// metadata) dropped once the row counts in its place: it grants, or it ended.
// One still incomplete (its payment pending) keeps the reservation to its end.
void settle_reservation(db::Database &db, const Subscription &s, const SubscriptionRow &row, int64_t now) {
	const auto id = strutil::parse_llong(meta(s.metadata, kMetaReservation));
	if (!id || *id <= 0 || !row.user_id) return;
	if (subscription_grants(row, now) || is_terminal(row.status)) drop_reservation(db, *id, *row.user_id);
}

// Stripe no longer knows it (in this mode, on this account).
bool is_missing(const StripeError &e) { return e.kind == StripeError::Kind::Rejected && e.status == 404; }

using db::BindValue;

// --- the state's JSON text (billing_sync_state) ---

io::JsonValue price_json(const std::optional<PriceInfo> &price) {
	if (!price)
		return io::JsonValue::make_null();
	io::JsonValue v = io::JsonValue::make_object();
	v.set("id", io::json_string(price->id));
	v.set("unit_amount",
			price->unit_amount ? io::json_number(static_cast<double>(*price->unit_amount))
							   : io::JsonValue::make_null());
	v.set("currency", io::json_string(price->currency));
	v.set("interval", io::json_string(price->interval));
	return v;
}

std::optional<PriceInfo> price_of(const io::JsonValue *v) {
	if (v == nullptr || !v->is_object())
		return std::nullopt;
	PriceInfo p;
	p.id = v->get_string("id", "");
	if (const io::JsonValue *amount = v->get("unit_amount");
			amount != nullptr && amount->is_number())
		p.unit_amount = static_cast<int64_t>(amount->number);
	p.currency = v->get_string("currency", "");
	p.interval = v->get_string("interval", "");
	return p;
}

std::string prices_text(const std::array<std::optional<PriceInfo>, kTierCount + 1> &tiers,
		const std::optional<PriceInfo> &own, const std::optional<PriceInfo> &address) {
	io::JsonValue v = io::JsonValue::make_object();
	io::JsonValue list = io::JsonValue::make_array();
	for (const auto &t : tiers)
		list.push(price_json(t));
	v.set("tiers", std::move(list));
	v.set("own_machine", price_json(own));
	v.set("static_address", price_json(address));
	return io::json_write(v);
}

std::string orphans_text(const std::vector<Orphan> &orphans) {
	io::JsonValue list = io::JsonValue::make_array();
	for (const Orphan &o : orphans) {
		io::JsonValue v = io::JsonValue::make_object();
		v.set("stripe_customer_id", io::json_string(o.stripe_customer_id));
		v.set("stripe_subscription_id", io::json_string(o.stripe_subscription_id));
		v.set("username_hint", io::json_string(o.username_hint));
		v.set("status", io::json_string(o.status));
		list.push(std::move(v));
	}
	return io::json_write(list);
}

std::string ids_text(const std::vector<int64_t> &ids) {
	io::JsonValue list = io::JsonValue::make_array();
	for (const int64_t id : ids)
		list.push(io::json_number(static_cast<double>(id)));
	return io::json_write(list);
}

io::JsonValue parsed(const std::string &text) {
	io::JsonValue v;
	std::string error;
	if (!io::json_parse(text, v, error))
		return io::JsonValue::make_null();
	return v;
}

} // namespace

const char *sync_outcome_name(SyncOutcome outcome) {
	switch (outcome) {
		case SyncOutcome::Synced:
			return "synced";
		case SyncOutcome::Foreign:
			return "foreign";
		case SyncOutcome::Deleted:
			return "deleted";
		case SyncOutcome::Orphan:
			return "orphan";
		case SyncOutcome::Unreachable:
			return "unreachable";
	}
	return "unknown";
}

BillingSync::BillingSync(db::ConnectionPool &pool, StripeClient &client,
		const BillingConfig &config, std::string instance_id, Clock clock) :
		pool_(pool),
		client_(client),
		config_(config),
		instance_id_(std::move(instance_id)),
		clock_(std::move(clock)) {}

std::string BillingSync::prices_config() const {
	std::string out = config_.live() ? "live" : "test";
	for (int tier = 1; tier <= kTierCount; ++tier) {
		out += "|";
		for (const std::string &p : config_.tier_prices[tier])
			out += p + ",";
	}
	out += "|";
	for (const std::string &p : config_.own_machine_prices)
		out += p + ",";
	out += "|";
	for (const std::string &p : config_.static_address_prices)
		out += p + ",";
	return out;
}

void BillingSync::sync_pending(int64_t now) {
	// The prices, while they are not checked under this configuration (again a
	// minute after a check Stripe could not answer).
	bool check = false;
	{
		auto lease = pool_.acquire();
		const auto rows =
				lease->query("SELECT price_check_after_s FROM billing_sync_state WHERE id = 1;");
		const int64_t after = rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
		check = !status(*lease).prices_checked && now >= after;
	}
	if (check && !validate_prices()) {
		auto lease = pool_.acquire();
		lease->exec("UPDATE billing_sync_state SET price_check_after_s = $1 WHERE id = 1;",
				{ BindValue(now + kBackoffFirstS) });
	}

	// The customers with an event to sync, each past its backoff. No connection
	// is held while Stripe is asked: each customer's sync leases one for each of
	// its reads and writes (sync_one), and its backoff one of its own.
	struct Backoff {
		int64_t next = 0;
		int64_t delay = kBackoffFirstS;
	};
	std::map<std::string, Backoff> backoffs;
	std::vector<std::string> pending;
	{
		auto lease = pool_.acquire();
		for (const db::Row &r : lease->query(
					 "SELECT stripe_customer_id, next_s, delay_s FROM billing_sync_backoff;"))
			backoffs[r.as_text(0).value_or("")] =
					Backoff{ r.as_int(1).value_or(0), r.as_int(2).value_or(kBackoffFirstS) };
		pending = pending_customers(*lease, kPendingPerPass);
	}
	for (const std::string &customer : pending) {
		const auto backoff = backoffs.find(customer);
		if (backoff != backoffs.end() && now < backoff->second.next)
			continue;
		SyncOutcome outcome = SyncOutcome::Unreachable;
		try {
			outcome = sync_one(nullptr, customer);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[billing] WARN sync %s: %s\n", loggable(customer).c_str(), e.what());
		}
		auto lease = pool_.acquire();
		db::Database &db = *lease;
		if (outcome == SyncOutcome::Unreachable) {
			const int64_t delay =
					backoff != backoffs.end() ? backoff->second.delay : kBackoffFirstS;
			db.exec("INSERT INTO billing_sync_backoff (stripe_customer_id, next_s, delay_s) VALUES "
					"($1, $2, $3) "
					"ON CONFLICT (stripe_customer_id) DO UPDATE SET next_s = EXCLUDED.next_s, "
					"delay_s = EXCLUDED.delay_s;",
					{ BindValue(customer), BindValue(now + delay),
							BindValue(std::min(delay * 2, kBackoffMaxS)) });
			std::printf("[billing] sync %s: Stripe unreachable; again in %lld s\n",
					loggable(customer).c_str(), static_cast<long long>(delay));
		} else if (backoff != backoffs.end()) {
			db.exec("DELETE FROM billing_sync_backoff WHERE stripe_customer_id = $1;",
					{ BindValue(customer) });
		}
	}
	// The backoffs of customers no longer waiting (synced by a route).
	pool_.acquire()->exec(
			"DELETE FROM billing_sync_backoff b WHERE NOT EXISTS (SELECT 1 FROM billing_events e "
			"WHERE e.customer_id = b.stripe_customer_id AND e.processed_at IS NULL);");
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
	if (row.tier > 0)
		row.item = ItemKind::Tier;
	else if (is_own_machine_price(config_, row.price_id))
		row.item = ItemKind::OwnMachine;
	else if (is_static_address_price(config_, row.price_id))
		row.item = ItemKind::StaticAddress;
	else
		row.item = ItemKind::Unknown;
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
					stripe_error_text(listed.error).c_str());
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
	return sync_one(&db, stripe_customer_id);
}

SyncOutcome BillingSync::sync_one(db::Database *held, const std::string &stripe_customer_id) {
	// Each step's database work on `held`, or with none on a connection leased
	// for that step alone, given back before the next Stripe call.
	const auto on_db = [this, held](const auto &step) {
		if (held != nullptr)
			return step(*held);
		auto lease = pool_.acquire();
		return step(*lease);
	};
	// The events marked are those recorded by now: one delivered while Stripe is
	// read waits for the next sync.
	const std::string started = on_db([](db::Database &db) { return database_clock(db); });
	const auto finish = [&](SyncOutcome outcome) {
		on_db([&](db::Database &db) {
			mark_processed(db, stripe_customer_id, sync_outcome_name(outcome), started);
		});
		return outcome;
	};
	const auto forget = [&] {
		on_db([&](db::Database &db) { forget_customer(db, stripe_customer_id); });
	};
	const StripeResult<Customer> customer = client_.get_customer(stripe_customer_id);
	if (!customer.ok()) {
		if (is_missing(customer.error)) {
			forget();
			std::printf("[billing] customer %s: Stripe knows none; its subscriptions canceled here\n",
					loggable(stripe_customer_id).c_str());
			return finish(SyncOutcome::Deleted);
		}
		std::fprintf(stderr, "[billing] WARN customer %s: %s\n", loggable(stripe_customer_id).c_str(),
				stripe_error_text(customer.error).c_str());
		return SyncOutcome::Unreachable;
	}
	if (customer.value->deleted) {
		forget();
		std::printf("[billing] customer %s deleted in Stripe; its subscriptions canceled here\n",
				loggable(stripe_customer_id).c_str());
		return finish(SyncOutcome::Deleted);
	}
	if (meta(customer.value->metadata, kMetaInstance) != instance_id_ || customer.value->livemode != config_.live())
		return finish(SyncOutcome::Foreign);
	const std::optional<int64_t> user = on_db([&](db::Database &db) {
		return account_of(db, stripe_customer_id, customer.value->metadata);
	});
	if (!user) {
		std::printf("[billing] customer %s names no account here (an orphan)\n", loggable(stripe_customer_id).c_str());
		return finish(SyncOutcome::Orphan);
	}
	const std::optional<std::vector<Subscription>> subscriptions = stripe_subscriptions_of(stripe_customer_id);
	if (!subscriptions) return SyncOutcome::Unreachable;

	on_db([&](db::Database &db) {
		std::vector<std::string> listed;
		db::Transaction tx(db);
		if (!customer_by_id(db, stripe_customer_id) && !customer_of(db, *user))
			insert_customer(db, *user, stripe_customer_id, customer.value->livemode);
		for (const Subscription &s : *subscriptions) {
			const SubscriptionRow row = row_of(s, user);
			upsert_subscription(db, row);
			settle_reservation(db, s, row, clock_());
			listed.push_back(s.id);
		}
		// One Stripe lists no longer (deleted with its test data): canceled.
		end_subscriptions_of_customer(db, stripe_customer_id, listed, "canceled");
		mark_customer_synced(db, stripe_customer_id);
		tx.commit();
	});
	std::printf("[billing] customer %s (account %lld): %zu subscription(s) synced\n",
			loggable(stripe_customer_id).c_str(), static_cast<long long>(*user), subscriptions->size());
	return finish(SyncOutcome::Synced);
}

bool BillingSync::validate_prices() {
	std::string problem;
	std::array<std::optional<PriceInfo>, kTierCount + 1> tier_prices;
	std::optional<PriceInfo> own_price;
	std::optional<PriceInfo> address_price;
	std::string currency;
	const auto check_list = [&](const std::vector<std::string> &list, const std::string &variable,
									std::optional<PriceInfo> &sold) -> bool {
		for (size_t i = 0; i < list.size(); ++i) {
			const StripeResult<Price> price = client_.get_price(list[i]);
			if (!price.ok()) {
				if (price.error.kind == StripeError::Kind::Unreachable) {
					std::fprintf(stderr, "[billing] WARN price check: %s: %s\n", loggable(list[i]).c_str(),
							stripe_error_text(price.error).c_str());
					return false; // try again later
				}
				if (problem.empty())
					problem = variable + ": Stripe answers no price " + loggable(list[i], 80) + " (" +
							stripe_error_text(price.error) + ")";
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
	if (!check_list(config_.static_address_prices, "STRIPE_PRICE_STATIC_ADDRESS", address_price)) return false;
	auto lease = pool_.acquire();
	db::Database &db = *lease;
	const SyncStatus was = status(db);
	db.exec("UPDATE billing_sync_state SET prices_config = $1, prices_checked = true, prices_ok = "
			"$2, "
			"prices_error = $3, prices = $4, price_check_after_s = 0 WHERE id = 1;",
			{ BindValue(prices_config()), BindValue(db::Bool{ problem.empty() }),
					BindValue(problem), BindValue(prices_text(tier_prices, own_price, address_price)) });
	if (!problem.empty() && (was.prices_ok || !was.prices_checked))
		std::fprintf(stderr,
				"[billing] WARN the prices: %s; checkout is off until they are fixed\n",
				problem.c_str());
	else if (problem.empty() && !was.prices_ok)
		std::printf("[billing] the prices check out\n");
	return true;
}

ReconcileReport BillingSync::reconcile_all(int64_t now) {
	ReconcileReport report;
	{
		// The sync's first run on this database: the heartbeat's age counts from it.
		auto lease = pool_.acquire();
		lease->exec("UPDATE billing_sync_state SET started_s = $1 WHERE id = 1 AND started_s = 0;",
				{ BindValue(now) });
	}
	// The connection the writes below hold, once they do: a failure is written
	// on it (a thread holds one lease at a time).
	std::optional<db::ConnectionPool::Lease> held;
	const auto fail = [&](std::string why) {
		report.error = std::move(why);
		std::fprintf(stderr, "[billing] WARN reconcile: %s\n", report.error.c_str());
		if (!held)
			held.emplace(pool_.acquire());
		(**held).exec("UPDATE billing_sync_state SET last_reconcile_error = $1 WHERE id = 1;",
				{ BindValue(report.error) });
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
		if (!listed.ok())
			return fail(
					"Stripe could not list the subscriptions: " + stripe_error_text(listed.error));
		for (const Subscription &s : listed.value->data) all.push_back(s);
		if (!listed.value->has_more || listed.value->data.empty()) {
			whole = true;
			break;
		}
		query.starting_after = listed.value->data.back().id;
	}
	report.listed = all.size();

	held.emplace(pool_.acquire());
	db::Database &db = **held;
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
						stripe_error_text(one.error));
			}
		}
	}
	{
		db::Transaction tx(db);
		for (const Ours &o : ours) {
			if (!customer_by_id(db, o.subscription->customer) && !customer_of(db, o.user) &&
					insert_customer(db, o.user, o.subscription->customer, o.subscription->livemode))
				++report.customers_recreated;
			const SubscriptionRow row = row_of(*o.subscription, o.user);
			upsert_subscription(db, row);
			settle_reservation(db, *o.subscription, row, now);
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
	// Ours that would grant but sell a price no list names: a sold price
	// missing from the configuration, which grants its account nothing.
	std::set<std::string> unknown;
	for (const SubscriptionRow &row : list_subscriptions(db)) {
		if (row.user_id && row.item == ItemKind::Tier && subscription_grants(row, now)) ++granting_tiers[*row.user_id];
		if (row.user_id && row.item == ItemKind::Unknown && status_grants(row.status) && row.current_period_end &&
				now < *row.current_period_end + kPeriodNetSeconds)
			unknown.insert(row.price_id);
	}
	const std::vector<std::string> unknown_prices(unknown.begin(), unknown.end());
	std::vector<int64_t> duplicates;
	for (const auto &[user, count] : granting_tiers) {
		if (count > 1) duplicates.push_back(user);
	}
	report.orphans = orphans.size();
	report.completed = whole;
	const bool prices_ok = status(db).prices_ok;
	if (!whole)
		report.error = "Stripe listed more than " + std::to_string(kMaxReconcilePages * 100) +
				" subscriptions; the rest were not read";
	{
		std::vector<BindValue> binds = { BindValue(orphans_text(orphans)),
			BindValue(ids_text(duplicates)),
			BindValue(io::json_write(io::json_string_array(unknown_prices))),
			BindValue(report.error) };
		if (whole)
			binds.push_back(BindValue(now));
		db.exec(std::string("UPDATE billing_sync_state SET orphans = $1, duplicates = $2, "
							"unknown_prices = $3, "
							"last_reconcile_error = $4") +
						(whole ? ", last_reconcile_s = $5" : "") + " WHERE id = 1;",
				binds);
	}
	std::printf("[billing] reconcile: %zu subscription(s) listed, %zu of ours written, %zu customer row(s) made "
				"again, %zu canceled, %zu orphan(s), %zu account(s) with two tiers\n",
			report.listed, report.written, report.customers_recreated, report.canceled, report.orphans,
			duplicates.size());
	if (!unknown_prices.empty()) {
		std::string list;
		for (const std::string &p : unknown_prices) list += (list.empty() ? "" : ", ") + loggable(p, 80);
		std::fprintf(stderr, "[billing] WARN reconcile: subscription(s) of ours sell a price no STRIPE_PRICE_* list "
							 "names (%s): their accounts get nothing for them, and the cache is not confirmed\n",
				list.c_str());
	}
	// The cache now matches Stripe, read whole under prices that check out and
	// name every price ours sell.
	if (whole && prices_ok && unknown_prices.empty() && on_reconciled_)
		on_reconciled_(db);
	return report;
}

SyncStatus BillingSync::status(db::Database &db) const {
	SyncStatus out;
	const auto rows =
			db.query("SELECT prices_config, prices_checked, prices_ok, prices_error, prices, "
					 "last_reconcile_s, last_reconcile_error, orphans, duplicates, unknown_prices, "
					 "started_s "
					 "FROM billing_sync_state WHERE id = 1;");
	if (rows.empty())
		return out;
	const db::Row &r = rows.front();
	// Prices checked under other lists (or the other mode) say nothing of these.
	if (r.as_text(0).value_or("") == prices_config()) {
		out.prices_checked = r.as_int(1).value_or(0) != 0;
		out.prices_ok = r.as_int(2).value_or(0) != 0;
		out.prices_error = r.as_text(3).value_or("");
		const io::JsonValue prices = parsed(r.as_text(4).value_or("{}"));
		if (const io::JsonValue *tiers = prices.get("tiers");
				tiers != nullptr && tiers->is_array()) {
			for (size_t i = 0; i < tiers->array.size() && i < out.tier_prices.size(); ++i)
				out.tier_prices[i] = price_of(&tiers->array[i]);
		}
		out.own_machine_price = price_of(prices.get("own_machine"));
		out.static_address_price = price_of(prices.get("static_address"));
	}
	out.last_reconcile_at = r.as_int(5).value_or(0);
	out.last_reconcile_error = r.as_text(6).value_or("");
	const io::JsonValue orphans = parsed(r.as_text(7).value_or("[]"));
	for (const io::JsonValue &o : orphans.array) {
		Orphan orphan;
		orphan.stripe_customer_id = o.get_string("stripe_customer_id", "");
		orphan.stripe_subscription_id = o.get_string("stripe_subscription_id", "");
		orphan.username_hint = o.get_string("username_hint", "");
		orphan.status = o.get_string("status", "");
		out.orphans.push_back(std::move(orphan));
	}
	for (const io::JsonValue &d : parsed(r.as_text(8).value_or("[]")).array) {
		if (d.is_number())
			out.duplicates.push_back(static_cast<int64_t>(d.number));
	}
	for (const io::JsonValue &p : parsed(r.as_text(9).value_or("[]")).array) {
		if (p.is_string())
			out.unknown_prices.push_back(p.string);
	}
	out.started_at = r.as_int(10).value_or(0);
	return out;
}

int64_t BillingSync::sync_age_s(db::Database &db, int64_t now) const {
	int64_t age = oldest_pending_age_s(db);
	const SyncStatus s = status(db);
	if (!s.last_reconcile_error.empty()) {
		const int64_t since = s.last_reconcile_at > 0 ? s.last_reconcile_at : s.started_at;
		age = std::max(age, now - since);
	}
	return std::max<int64_t>(age, 0);
}

} // namespace opennova::novaworld_server::billing
