#include "billing_routes.h"

#include "auth.h"
#include "billing/billing_jobs.h"
#include "hosted/hosted_repository.h"
#include "http_json.h"
#include "loggable.h"
#include "stripe_signature.h"
#include "billing/tiers.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::billing {

namespace {

crow::response disabled() {
	return json_error(503, "billing_disabled", "support tiers are not open on this NovaWorld service");
}

// Stripe did not answer, or refused what was asked (the reason is in the log
// line the caller writes). 503, never 502: Cloudflare, in front of the site,
// answers an origin's 502 (and 504) with a page of its own in place of this
// body, so the site's page could not tell the visitor what happened.
crow::response stripe_unavailable() {
	return json_error(
			503, "stripe_unavailable", "Stripe could not take the request; try again in a moment");
}

void amount_or_null(crow::json::wvalue &out, const std::optional<PriceInfo> &price) {
	if (price && price->unit_amount)
		out["amount"] = *price->unit_amount;
	else
		out["amount"] = nullptr;
}

bool is_terminal(const std::string &status) { return status == "canceled" || status == "incomplete_expired"; }

// The subscription of `item` the account's page shows: a granting one (the
// highest tier, else the newest), else the newest still open; nullptr for none.
// Every candidate has a period end (the page shows it).
const SubscriptionRow *shown(const std::vector<SubscriptionRow> &rows, ItemKind item, int64_t now) {
	const SubscriptionRow *best = nullptr;
	for (const SubscriptionRow &r : rows) {
		if (r.item != item || !r.current_period_end || !subscription_grants(r, now)) continue;
		if (!best || r.tier > best->tier) best = &r;
	}
	if (best) return best;
	for (const SubscriptionRow &r : rows) {
		if (r.item == item && r.current_period_end && !is_terminal(r.status)) return &r;
	}
	return nullptr;
}

bool tier_grants(const std::vector<SubscriptionRow> &rows, int64_t now) {
	for (const SubscriptionRow &r : rows) {
		if (r.item == ItemKind::Tier && subscription_grants(r, now)) return true;
	}
	return false;
}

void log_account(const WebSessionUser &user, const char *what, const std::string &outcome) {
	std::printf("[billing] account '%s' (id=%lld) %s -> %s\n", loggable(user.username).c_str(),
			static_cast<long long>(user.id), what, outcome.c_str());
}

// What a checkout's "item" names: a tier 1..3, or the add-on; nullopt for none.
std::optional<std::pair<ItemKind, int>> checkout_item(const std::string &item) {
	if (item == "own_machine") return std::make_pair(ItemKind::OwnMachine, 0);
	for (int tier = 1; tier <= kTierCount; ++tier) {
		if (item == "tier" + std::to_string(tier)) return std::make_pair(ItemKind::Tier, tier);
	}
	return std::nullopt;
}

crow::response sold_out(const char *what) {
	return json_error(409, "sold_out", std::string("Hosting is full: new subscriptions are paused (") + what + ")");
}

} // namespace

std::string iso_utc(int64_t unix_seconds) {
	const std::time_t t = static_cast<std::time_t>(unix_seconds);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char out[32];
	std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return out;
}

std::string iso_from_db(const std::string &db_text) {
	if (db_text.size() != 19 || db_text[10] != ' ') return db_text;
	return db_text.substr(0, 10) + "T" + db_text.substr(11) + "Z";
}

crow::json::wvalue admin_subscription_json(const SubscriptionRow &row, int64_t now) {
	crow::json::wvalue out;
	out["stripe_subscription_id"] = row.stripe_subscription_id;
	out["item"] = item_kind_name(row.item);
	if (row.item == ItemKind::Tier && row.tier > 0)
		out["tier"] = row.tier;
	else
		out["tier"] = nullptr;
	out["quantity"] = row.quantity;
	out["status"] = row.status;
	out["entitled"] = subscription_grants(row, now);
	out["current_period_end"] = row.current_period_end ? iso_utc(*row.current_period_end) : std::string();
	out["cancel_at_period_end"] = row.cancel_at_period_end;
	return out;
}

void BillingRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/billing/tiers").methods("GET"_method)([this]() { return tiers(); });
	CROW_ROUTE(app, "/api/me/billing").methods("GET"_method)([this](const crow::request &req) { return me_billing(req); });
	CROW_ROUTE(app, "/api/billing/checkout")
			.methods("POST"_method)([this](const crow::request &req) { return checkout(req); });
	CROW_ROUTE(app, "/api/billing/portal").methods("POST"_method)([this](const crow::request &req) { return portal(req); });
	CROW_ROUTE(app, "/api/me/billing/sync")
			.methods("POST"_method)([this](const crow::request &req) { return me_sync(req); });
	CROW_ROUTE(app, "/api/billing/webhook")
			.methods("POST"_method)([this](const crow::request &req) { return webhook(req); });
	CROW_ROUTE(app, "/api/admin/billing")
			.methods("GET"_method)([this](const crow::request &req) { return admin_billing(req); });
	CROW_ROUTE(app, "/api/admin/billing/reconcile")
			.methods("POST"_method)([this](const crow::request &req) { return admin_reconcile(req); });
	CROW_ROUTE(app, "/api/admin/billing/link")
			.methods("POST"_method)([this](const crow::request &req) { return admin_link(req); });
}

// GET /api/billing/tiers — public: {"enabled" (billing on and its prices checked
// out), "currency" (the prices', lower case; "" before they are read), "tiers":
// [{tier, max_servers, amount (the price sold now, in the currency's smallest
// unit; null while unread), interval: "month", available}], "own_machine":
// {amount, interval: "month", available} with billing on, else null}. The
// amounts are Stripe's price objects', never the service's own. `available`
// is whether the fleet takes one more of the item for an account that has
// nothing yet (hosted/sold_capacity.h): false, the site says hosting is full
// and a checkout of it answers sold_out (an account whose grant already
// covers it may still buy it; the checkout judges each account's own).
crow::response BillingRoutes::tiers() {
	SyncStatus status;
	std::optional<hosted::Capacity> sold;
	const int64_t now = unix_now();
	if (billing_ != nullptr) {
		try {
			auto db_conn = pool_.acquire();
			status = billing_->sync().status(*db_conn);
			sold = hosted::sold_now(*db_conn, now);
		} catch (const db::Error &e) {
			// Read as prices not yet checked: billing shows off for the moment.
			std::fprintf(stderr, "[http] GET /api/billing/tiers: %s\n", e.what());
		}
	}
	// One more of `item` for a new account fits what is sold now; with the
	// sold unread, no.
	const auto available = [&](ItemKind item, int tier) {
		return sold && hosted::fits(*sold, hosted::added_by_new_account(item, tier, now), sellable_);
	};
	std::string currency;
	for (int tier = 1; tier <= kTierCount && currency.empty(); ++tier) {
		if (status.tier_prices[tier]) currency = status.tier_prices[tier]->currency;
	}
	crow::json::wvalue out;
	out["enabled"] = billing_ != nullptr && status.prices_ok;
	out["currency"] = currency;
	std::vector<crow::json::wvalue> list;
	for (int tier = 1; tier <= kTierCount; ++tier) {
		crow::json::wvalue t;
		t["tier"] = tier;
		t["max_servers"] = kTierServers[tier];
		amount_or_null(t, status.tier_prices[tier]);
		t["interval"] = "month";
		t["available"] = available(ItemKind::Tier, tier);
		list.push_back(std::move(t));
	}
	out["tiers"] = std::move(list);
	if (billing_ != nullptr) {
		crow::json::wvalue own;
		amount_or_null(own, status.own_machine_price);
		own["interval"] = "month";
		own["available"] = available(ItemKind::OwnMachine, 0);
		out["own_machine"] = std::move(own);
	} else {
		out["own_machine"] = nullptr;
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "public, max-age=60");
	return res;
}

// The account's billing: {"enabled", "max_servers", "comp_servers",
// "comp_own_machines" (an admin's grants), "own_machines", "used" (its live hosted servers), "stopping" (how many of
// those are stopping: the owners' limits count the rest), "can_start", "subscription"
// (its support tier: {tier, status, entitled, current_period_end (ISO UTC),
// cancel_at_period_end}, or null), "own_machine_subscription" ({quantity,
// status, current_period_end, cancel_at_period_end}, or null), "can_manage" (it
// has a Stripe customer: the portal opens), "can_subscribe" (a checkout of a
// tier may start: the prices checked out, the account an active player's, no
// tier granting)}.
crow::response BillingRoutes::me_billing_reply(db::Database &db, int64_t user_id) {
	const int64_t now = unix_now();
	const Entitlement e = entitlement_of(db, user_id, now);
	const std::vector<SubscriptionRow> rows = subscriptions_of(db, user_id);
	const auto user = get_user_by_id(db, user_id);
	const bool active = user && user->kind == "player" && user->account_status == "active";
	crow::json::wvalue out;
	out["enabled"] = true;
	out["max_servers"] = e.max_servers;
	out["comp_servers"] = e.comp_servers;
	out["comp_own_machines"] = e.comp_own_machines;
	out["own_machines"] = e.own_machines;
	const std::vector<hosted::HostedServer> live = hosted::live_servers_of(db, user_id);
	out["used"] = static_cast<int64_t>(live.size());
	out["stopping"] = static_cast<int64_t>(std::count_if(live.begin(), live.end(),
			[](const hosted::HostedServer &s) { return s.state == hosted::State::Stopping; }));
	out["can_start"] = e.can_start;
	if (const SubscriptionRow *tier = shown(rows, ItemKind::Tier, now)) {
		crow::json::wvalue s;
		s["tier"] = tier->tier;
		s["status"] = tier->status;
		s["entitled"] = subscription_grants(*tier, now);
		s["current_period_end"] = iso_utc(*tier->current_period_end);
		s["cancel_at_period_end"] = tier->cancel_at_period_end;
		out["subscription"] = std::move(s);
	} else {
		out["subscription"] = nullptr;
	}
	if (const SubscriptionRow *own = shown(rows, ItemKind::OwnMachine, now)) {
		crow::json::wvalue s;
		s["quantity"] = own->quantity;
		s["status"] = own->status;
		s["current_period_end"] = iso_utc(*own->current_period_end);
		s["cancel_at_period_end"] = own->cancel_at_period_end;
		out["own_machine_subscription"] = std::move(s);
	} else {
		out["own_machine_subscription"] = nullptr;
	}
	out["can_manage"] = customer_of(db, user_id).has_value();
	out["can_subscribe"] =
			billing_->sync().status(db).prices_ok && active && !tier_grants(rows, now);
	return no_store(json_reply(200, out));
}

// GET /api/me/billing — require_user (401); 503 billing_disabled; the
// account's billing (me_billing_reply), Cache-Control: no-store.
crow::response BillingRoutes::me_billing(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	if (billing_ == nullptr) return disabled();
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		return me_billing_reply(*db_conn, access.user->id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/me/billing failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

std::optional<crow::response> BillingRoutes::judge_checkout(
		db::Database &db, const WebSessionUser &user, const CheckoutItem &item, const std::string &replaced) {
	const auto account = get_user_by_id(db, user.id);
	if (!account || account->kind != "player" || account->account_status != "active") {
		log_account(user, "checkout", "not_eligible (account)");
		return json_error(403, "not_eligible", "this account cannot subscribe");
	}
	const int64_t now = unix_now();
	const std::vector<SubscriptionRow> rows = subscriptions_of(db, user.id);
	const bool granting_tier = tier_grants(rows, now);
	if (item.item == ItemKind::Tier && granting_tier) {
		log_account(user, "checkout", "already_subscribed");
		return json_error(409, "already_subscribed", "this account has a support tier; change it in the portal");
	}
	if (item.item == ItemKind::OwnMachine && !granting_tier) {
		log_account(user, "checkout", "not_eligible (no tier)");
		return json_error(403, "not_eligible", "an own machine needs a support tier first");
	}
	// The account as the sold capacity counts it: its subscriptions and its
	// open checkouts' reservations.
	const hosted::AccountHoldings holdings = hosted::holdings_of(db, user.id, now, replaced);
	const Entitlement e = entitlement_from(holdings.facts, holdings.subscriptions, now);
	// Each own machine holds one of the servers the account may run: more
	// machines than servers buys nothing.
	if (item.item == ItemKind::OwnMachine && e.own_machines >= e.max_servers) {
		log_account(user, "checkout", "own_machine_limit");
		return json_error(409, "own_machine_limit", "this account has an own machine for every server it may run");
	}
	const hosted::Capacity more = hosted::added_by(holdings.facts, holdings.subscriptions, item.item, item.tier, now);
	if (!hosted::fits(hosted::sold_now(db, now, replaced), more, sellable_)) {
		log_account(user, "checkout", "sold_out");
		return sold_out(item.item == ItemKind::Tier ? "support tiers" : "own machines");
	}
	return std::nullopt;
}

std::optional<crow::response> BillingRoutes::settle_previous_session(const WebSessionUser &user,
		const CustomerRow &customer) {
	StripeClient &client = billing_->client();
	const std::string &id = customer.open_checkout_session;
	StripeResult<CheckoutSession> previous = client.get_checkout_session(id);
	if (previous.ok() && previous.value->status == "open") {
		if (client.expire_checkout_session(id).ok()) return std::nullopt;
		// Not expired: it completed (or expired) meanwhile; read again.
		previous = client.get_checkout_session(id);
	}
	if (!previous.ok()) {
		// Stripe knows no such session (in this mode): nothing was sold through it.
		if (previous.error.kind == StripeError::Kind::Rejected && previous.error.status == 404) return std::nullopt;
		log_account(user, "checkout",
				"the previous session unread: " + stripe_error_text(previous.error));
		return stripe_unavailable();
	}
	if (previous.value->status != "complete") return std::nullopt;
	// Paid for, and maybe not synced yet (the webhook on its way, a second
	// tab): the customer read back from Stripe now (the checkout's judgement
	// after reads it).
	try {
		auto db_conn = pool_.acquire();
		if (billing_->sync().sync_customer(*db_conn, customer.stripe_customer_id, unix_now()) ==
				SyncOutcome::Unreachable) {
			log_account(user, "checkout", "stripe_unavailable (the previous session's sync)");
			return stripe_unavailable();
		}
		return std::nullopt;
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/billing/checkout failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/billing/checkout — {"item": "tier1" | "tier2" | "tier3" |
// "own_machine"}: a Stripe Checkout session for it, {"url"} to send the browser
// to. In order: require_user (401, 403 without the CSRF header), 503
// billing_disabled, the account's kCheckoutPerUser bucket (429), 400
// invalid_json / invalid_item, 503 billing_misconfigured while the prices do
// not check out, 409 checkout_in_progress while another checkout of the
// account is running (one at a time), 403 not_eligible for an account that is
// not an active player's or (the add-on) has no tier granting, 409 already_subscribed when a
// tier grants already (the portal changes it), 409 own_machine_limit for the
// add-on when the account has an own machine for every server it may run
// (each add-on checkout sells one; several are fine below that), 409
// sold_out when the fleet has sold all it holds of what the item adds
// (hosted/sold_capacity.h: shared places for a tier, own machines for the
// add-on), 503 stripe_unavailable. Those rules are judged first as a quick
// refusal, then again after the account's previous session is settled
// (settle_previous_session: an open one is expired; a complete one has the
// customer synced from Stripe now, so a payment the webhook has not brought
// yet is never sold twice), in ONE transaction under the hosting-start lock
// (database.h kHostedStartLock, the one a start counts under) that also drops
// the previous session's reservation and writes this checkout's
// (billing_repository.h "A checkout's hold", kReservationLifeS), so two
// checkouts at once never both take the fleet's last room. Stripe makes the
// session after that commit (its id then stamped on the reservation; the
// reservation dropped when no session comes). Its Stripe customer is made
// at its first checkout (the idempotency key opennova-<instance>-customer-<id>,
// so a retry answers the same one), and made again in place of a row of the
// other Stripe mode (the key changed modes), which no call of this key could
// use. The session asks for the terms of service, lives kCheckoutLifeS from
// just before it is made, returns to
// /me/support?checkout=success&session_id=... or ?checkout=cancel, and stamps
// the instance, the account and the reservation on the session and on the
// subscription it makes.
crow::response BillingRoutes::checkout(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	const WebSessionUser &user = *access.user;
	if (billing_ == nullptr) return disabled();
	if (const int64_t wait = checkout_per_user_.take("billing|" + std::to_string(user.id), RateLimiter::Clock::now())) {
		log_account(user, "checkout", "rate_limited");
		return too_many_requests(wait);
	}
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const auto item_text = body->text("item");
	const auto named = item_text ? checkout_item(*item_text) : std::nullopt;
	if (body->wrong_type() || !named) return json_error(400, "invalid_item", "item is tier1, tier2, tier3 or own_machine");
	CheckoutItem item;
	item.item = named->first;
	item.tier = named->second;
	const BillingConfig &config = billing_->config();
	bool prices_ok = false;
	try {
		prices_ok = billing_->sync().status(*pool_.acquire()).prices_ok;
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/billing/checkout failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	if (!prices_ok)
		return json_error(503, "billing_misconfigured", "the support tiers' prices are being checked; try again later");
	const std::string price =
			item.item == ItemKind::Tier ? tier_price_for_sale(config, item.tier) : own_machine_price_for_sale(config);
	// The account's one checkout at a time, released however this one ends.
	{
		std::lock_guard<std::mutex> lock(checkouts_mu_);
		if (!checkouts_running_.insert(user.id).second) {
			log_account(user, "checkout", "checkout_in_progress");
			return json_error(409, "checkout_in_progress", "a checkout of this account is already starting");
		}
	}
	struct Running {
		BillingRoutes &routes;
		int64_t user_id;
		~Running() {
			std::lock_guard<std::mutex> lock(routes.checkouts_mu_);
			routes.checkouts_running_.erase(user_id);
		}
	} running{ *this, user.id };
	std::optional<CustomerRow> customer;
	try {
		auto db_conn = pool_.acquire();
		customer = customer_of(*db_conn, user.id);
		// The quick refusal: the session this checkout replaces holds nothing.
		if (auto refusal = judge_checkout(*db_conn, user, item, customer ? customer->open_checkout_session : ""))
			return std::move(*refusal);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/billing/checkout failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	// Stripe, outside any transaction. A customer row of the other mode (the key
	// changed modes) names no customer this key can use: one is made in its place.
	StripeClient &client = billing_->client();
	const std::optional<std::string> other_mode =
			customer && customer->livemode != config.live() ? std::optional<std::string>(customer->stripe_customer_id)
															: std::nullopt;
	if (other_mode) customer.reset();
	// The session this checkout replaces (its reservation goes with the new one's).
	const std::string previous_session = customer ? customer->open_checkout_session : std::string();
	if (customer && !previous_session.empty()) {
		if (auto refusal = settle_previous_session(user, *customer)) return std::move(*refusal);
	}
	if (!customer) {
		const auto made = client.create_customer(user.id, user.username, billing_->instance_id(),
				"opennova-" + billing_->instance_id() + "-customer-" + std::to_string(user.id));
		if (!made.ok()) {
			log_account(user, "checkout", "customer not made: " + stripe_error_text(made.error));
			return stripe_unavailable();
		}
		try {
			auto db_conn = pool_.acquire();
			{
				db::Transaction tx(*db_conn);
				if (other_mode) delete_customer(*db_conn, *other_mode);
				insert_customer(*db_conn, user.id, made.value->id, made.value->livemode);
				tx.commit();
			}
			customer = customer_of(*db_conn, user.id);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] POST /api/billing/checkout failed: %s\n", e.what());
			return json_error(500, "db_error");
		}
		if (!customer) return json_error(500, "db_error");
		if (other_mode)
			log_account(user, "checkout", "a customer of the key's mode made in place of " + loggable(*other_mode));
	}
	// The judgement that counts and the reservation it writes, together, under
	// the lock a start's judgement takes.
	int64_t reservation = 0;
	try {
		auto db_conn = pool_.acquire();
		db::Transaction tx(*db_conn);
		db_conn->exec("SELECT pg_advisory_xact_lock($1);", { db::BindValue(db::kHostedStartLock) });
		drop_session_reservation(*db_conn, previous_session);
		if (auto refusal = judge_checkout(*db_conn, user, item)) return std::move(*refusal);
		reservation = reserve_checkout(*db_conn, user.id, item.item, item.tier, kReservationLifeS);
		tx.commit();
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/billing/checkout failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	// No session made: the reservation goes (a failure to drop it lapses at its end).
	const auto unreserve = [&] {
		try {
			drop_reservation(*pool_.acquire(), reservation, user.id);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] POST /api/billing/checkout: a reservation not dropped: %s\n", e.what());
		}
	};
	CheckoutCreate create;
	create.customer = customer->stripe_customer_id;
	create.client_reference_id = std::to_string(user.id);
	create.price_id = price;
	create.quantity = 1;
	create.success_url = config.site_url + "/me/support?checkout=success&session_id={CHECKOUT_SESSION_ID}";
	create.cancel_url = config.site_url + "/me/support?checkout=cancel";
	create.metadata = { { kMetaInstance, billing_->instance_id() }, { kMetaUserId, std::to_string(user.id) },
		{ kMetaUsername, user.username }, { kMetaReservation, std::to_string(reservation) } };
	create.automatic_tax = config.automatic_tax;
	// Stripe refuses the session while the account's public details name no
	// terms of service URL (Dashboard, Settings, Public details; DEPLOY.md).
	create.require_tos_consent = true;
	// Stripe counts the life from the session's creation and refuses one under
	// 30 minutes: the clock read last, after every slow call above.
	create.expires_at = unix_now() + kCheckoutLifeS;
	const auto session = client.create_checkout_session(create);
	if (!session.ok() || session.value->url.empty()) {
		const std::string why =
				session.ok() ? std::string("no url") : stripe_error_text(session.error);
		log_account(user, "checkout", "no session: " + why);
		unreserve();
		return stripe_unavailable();
	}
	try {
		auto db_conn = pool_.acquire();
		stamp_reservation(*db_conn, reservation, session.value->id);
		set_open_checkout_session(*db_conn, user.id, session.value->id);
	} catch (const db::Error &e) {
		// The session stands; the next checkout cannot settle it (expire it, or
		// see it paid).
		std::fprintf(stderr, "[http] POST /api/billing/checkout: the open session not kept: %s\n", e.what());
	}
	log_account(user, "checkout", "session for " + *item_text);
	crow::json::wvalue out;
	out["url"] = session.value->url;
	return no_store(json_reply(200, out));
}

// POST /api/billing/portal — a Customer Portal session for the account's Stripe
// customer, {"url"}, returning to /me/support. require_user (401, 403 without
// the CSRF header), 503 billing_disabled, the checkout bucket (429), 404
// no_billing_account with no customer yet, 503 stripe_unavailable.
crow::response BillingRoutes::portal(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	const WebSessionUser &user = *access.user;
	if (billing_ == nullptr) return disabled();
	if (const int64_t wait = checkout_per_user_.take("billing|" + std::to_string(user.id), RateLimiter::Clock::now())) {
		log_account(user, "portal", "rate_limited");
		return too_many_requests(wait);
	}
	std::optional<CustomerRow> customer;
	try {
		customer = customer_of(*pool_.acquire(), user.id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/billing/portal failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	if (!customer) return json_error(404, "no_billing_account", "this account has no billing account yet");
	const auto url =
			billing_->client().create_portal_session(customer->stripe_customer_id, billing_->config().site_url + "/me/support");
	if (!url.ok()) {
		log_account(user, "portal", "no session: " + stripe_error_text(url.error));
		return stripe_unavailable();
	}
	crow::json::wvalue out;
	out["url"] = *url.value;
	return no_store(json_reply(200, out));
}

// POST /api/me/billing/sync — the account's customer read back from Stripe now
// (the checkout success page's: the webhook may not have come yet), then as GET
// /api/me/billing. require_user (401, 403 without the CSRF header), 503
// billing_disabled, the account's kSyncPerUser bucket (429), 404
// no_billing_account with no customer yet, 503 stripe_unavailable.
crow::response BillingRoutes::me_sync(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	const WebSessionUser &user = *access.user;
	if (billing_ == nullptr) return disabled();
	if (const int64_t wait = sync_per_user_.take("billing|" + std::to_string(user.id), RateLimiter::Clock::now())) {
		log_account(user, "sync", "rate_limited");
		return too_many_requests(wait);
	}
	try {
		auto db_conn = pool_.acquire();
		const auto customer = customer_of(*db_conn, user.id);
		if (!customer) return json_error(404, "no_billing_account", "this account has no billing account yet");
		const SyncOutcome outcome = billing_->sync().sync_customer(*db_conn, customer->stripe_customer_id, unix_now());
		if (outcome == SyncOutcome::Unreachable) {
			log_account(user, "sync", "stripe_unavailable");
			return stripe_unavailable();
		}
		return me_billing_reply(*db_conn, user.id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/me/billing/sync failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/billing/webhook — Stripe's deliveries. 413 past kWebhookMaxBytes;
// 503 billing_disabled; 400 bad_signature unless a v1 signature of the
// Stripe-Signature header matches the endpoint's secret within 300 s (one WARN
// line names the check's class and the clock skew, never the body or the
// header); 400 invalid_json for a body that is no event; an event of the other
// mode is answered 200 and dropped with a WARN. Then the event is recorded once
// (a redelivery is a no-op) with the customer it names, the worker woken, and
// 200 {"received": true}: the worker reads the customer back from Stripe. A
// checkout.session.expired event drops its session's reservation (an expired
// session never makes a subscription), with the record.
crow::response BillingRoutes::webhook(const crow::request &req) {
	if (req.body.size() > kWebhookMaxBytes) return json_error(413, "payload_too_large");
	if (billing_ == nullptr) return disabled();
	const int64_t now = unix_now();
	int64_t skew = 0;
	const SignatureCheck check = verify_stripe_signature(req.get_header_value("Stripe-Signature"), req.body,
			{ billing_->config().webhook_secret }, now, 300, &skew);
	if (check != SignatureCheck::Ok) {
		billing_->count_webhook_reject();
		std::fprintf(stderr, "[billing] WARN webhook refused: %s signature (clock skew %lld s)\n",
				signature_check_name(check), static_cast<long long>(skew));
		return json_error(400, "bad_signature");
	}
	const std::optional<WebhookEnvelope> event = parse_webhook_envelope(req.body);
	if (!event) {
		billing_->count_webhook_reject();
		std::fprintf(stderr, "[billing] WARN webhook refused: a signed body that is no event\n");
		return json_error(400, "invalid_json");
	}
	crow::json::wvalue out;
	out["received"] = true;
	if (event->livemode != billing_->config().live()) {
		std::fprintf(stderr, "[billing] WARN webhook %s: a %s-mode event, the key a %s-mode one; dropped\n",
				loggable(event->id).c_str(), event->livemode ? "live" : "test", billing_->config().live() ? "live" : "test");
		return json_reply(200, out);
	}
	try {
		auto db_conn = pool_.acquire();
		const bool fresh =
				record_event(*db_conn, event->id, event->type, event->livemode, event->customer_id,
						event->created > 0 ? std::optional<int64_t>(event->created) : std::nullopt);
		std::printf("[billing] webhook %s %s customer %s%s\n", loggable(event->id).c_str(), loggable(event->type).c_str(),
				event->customer_id.empty() ? "(none)" : loggable(event->customer_id).c_str(), fresh ? "" : " (again)");
		if (event->type == "checkout.session.expired" && event->object_type == "checkout.session" &&
				drop_session_reservation(*db_conn, event->object_id) > 0)
			std::printf("[billing] checkout session %s expired: its reservation dropped\n",
					loggable(event->object_id).c_str());
		// A worker syncs its customer (billing.sync); one waiting already takes it.
		if (fresh && !event->customer_id.empty())
			enqueue_billing_sync(*db_conn);
	} catch (const db::Error &e) {
		// Stripe delivers it again on a 5xx.
		std::fprintf(stderr, "[http] POST /api/billing/webhook failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return json_reply(200, out);
}

// The hosted servers' limits hold (hosted/limits_hold.h) on an admin's view:
// "limits_hold", "limits_hold_reason" (null when not held), and the stops it
// keeps back as the supervisor's last pass counted them, "limits_hold_owners"
// and "limits_hold_servers" (0 when not held).
void BillingRoutes::limits_hold_json(db::Database &db, crow::json::wvalue &out) const {
	const hosted::LimitsHoldView hold = hosted::limits_hold::view(db);
	out["limits_hold"] = hold.held;
	text_or_null(out, "limits_hold_reason", hold.reason);
	out["limits_hold_owners"] = static_cast<int64_t>(hold.owners);
	out["limits_hold_servers"] = static_cast<int64_t>(hold.servers);
}

// GET /api/admin/billing — require_admin; {"enabled", "mode" ("test" |
// "live"), "prices_ok", "prices_error" (null when ok), "last_reconcile_at"
// (ISO, null before one completed), "last_reconcile_error" (null when the last
// completed), "pending_events" (events not synced yet), "subscriptions" (every
// row with an account: admin_subscription_json plus user_id, username,
// stripe_customer_id), "duplicates" (accounts with two granting tiers),
// "unknown_prices" (the prices of our subscriptions that would grant but that
// no STRIPE_PRICE_* list names, as the last reconcile found them: while any is
// left no reconcile releases the limits hold),
// "orphans" ({stripe_customer_id, stripe_subscription_id, username_hint,
// status}), "recent_events" (the newest kEventsShown: event_id, type,
// received_at (ISO), outcome)} with the limits hold (limits_hold_json). With
// billing off, {"enabled": false} and the limits hold alone (the hold reads the
// cache billing on or off). Cache-Control: no-store.
crow::response BillingRoutes::admin_billing(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	crow::json::wvalue out;
	SyncStatus status;
	try {
		auto db_conn = pool_.acquire();
		limits_hold_json(*db_conn, out);
		if (billing_ == nullptr) {
			out["enabled"] = false;
			return no_store(json_reply(200, out));
		}
		status = billing_->sync().status(*db_conn);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/billing failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	const int64_t now = unix_now();
	out["enabled"] = true;
	out["mode"] = billing_->config().live() ? "live" : "test";
	out["prices_ok"] = status.prices_ok;
	text_or_null(out, "prices_error", status.prices_error);
	if (status.last_reconcile_at > 0)
		out["last_reconcile_at"] = iso_utc(status.last_reconcile_at);
	else
		out["last_reconcile_at"] = nullptr;
	text_or_null(out, "last_reconcile_error", status.last_reconcile_error);
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		out["pending_events"] = pending_event_count(*db_conn);
		std::vector<crow::json::wvalue> rows;
		for (const SubscriptionRow &row : list_subscriptions(*db_conn)) {
			if (!row.user_id) continue;
			crow::json::wvalue e = admin_subscription_json(row, now);
			e["user_id"] = *row.user_id;
			const auto user = get_user_by_id(*db_conn, *row.user_id);
			e["username"] = user ? user->username : std::string();
			e["stripe_customer_id"] = row.stripe_customer_id;
			rows.push_back(std::move(e));
		}
		out["subscriptions"] = std::move(rows);
		std::vector<crow::json::wvalue> events;
		for (const EventRow &ev : recent_events(*db_conn, kEventsShown)) {
			crow::json::wvalue e;
			e["event_id"] = ev.event_id;
			e["type"] = ev.type;
			e["received_at"] = iso_from_db(ev.received_at);
			e["outcome"] = ev.outcome;
			events.push_back(std::move(e));
		}
		out["recent_events"] = std::move(events);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/billing failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	std::vector<crow::json::wvalue> duplicates;
	for (const int64_t user : status.duplicates) {
		crow::json::wvalue d;
		d = user;
		duplicates.push_back(std::move(d));
	}
	out["duplicates"] = std::move(duplicates);
	std::vector<crow::json::wvalue> unknown_prices;
	for (const std::string &price : status.unknown_prices) {
		crow::json::wvalue p;
		p = price;
		unknown_prices.push_back(std::move(p));
	}
	out["unknown_prices"] = std::move(unknown_prices);
	std::vector<crow::json::wvalue> orphans;
	for (const Orphan &o : status.orphans) {
		crow::json::wvalue e;
		e["stripe_customer_id"] = o.stripe_customer_id;
		e["stripe_subscription_id"] = o.stripe_subscription_id;
		e["username_hint"] = o.username_hint;
		e["status"] = o.status;
		orphans.push_back(std::move(e));
	}
	out["orphans"] = std::move(orphans);
	return no_store(json_reply(200, out));
}

// POST /api/admin/billing/reconcile — no body or {}: a full reconcile, a
// billing.reconcile job a worker runs at once: 202 {"reconcile": "requested", "force_limits":
// false}. The hosted servers' limits hold releases itself after a clean one
// when the same owners are still past their entitlements (hosted/limits_hold.h).
// {"force_limits": true}: the reconcile, and the supervisor's next pass applies
// the stops the hold keeps back past the brake, once ("force_limits": true;
// logged). require_admin; 400 invalid_json, invalid_field (force_limits not
// true or false). With billing off there is nothing to reconcile: 503
// billing_disabled, but an override still applies (the hold reads the cache
// billing on or off): 202 {"reconcile": "off", "force_limits": true}.
crow::response BillingRoutes::admin_reconcile(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body.empty() ? std::string("{}") : req.body);
	if (!body) return json_error(400, "invalid_json");
	const bool force = body->flag("force_limits").value_or(false);
	if (body->wrong_type()) return json_error(400, "invalid_field", "force_limits is true or false");
	if (billing_ == nullptr && !force) return disabled();
	if (force) {
		try {
			auto db_conn = pool_.acquire();
			hosted::limits_hold::force_next_pass(*db_conn);
			const hosted::LimitsHoldView hold = hosted::limits_hold::view(*db_conn);
			std::printf("[hosted] an admin's override: the next pass applies the owners' limits "
						"past the brake (held: "
						"%s)\n",
					hold.held ? hold.reason.c_str() : "no");
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] POST /api/admin/billing/reconcile failed: %s\n", e.what());
			return json_error(500, "db_error");
		}
	}
	crow::json::wvalue out;
	out["force_limits"] = force;
	if (billing_ == nullptr) {
		out["reconcile"] = "off";
		return json_reply(202, out);
	}
	try {
		// A worker runs it (billing.reconcile); one waiting already stands for it.
		enqueue_billing_reconcile(*pool_.acquire());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/billing/reconcile failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	std::printf("[billing] reconcile asked for by an admin\n");
	out["reconcile"] = "requested";
	return json_reply(202, out);
}

// POST /api/admin/billing/link — {"user_id", "stripe_customer_id"}: a Stripe
// customer (an orphan's) made the account's: its metadata stamped with this
// instance and the account, the account's customer row replaced, and the
// customer synced. require_admin; 503 billing_disabled; 400 invalid_json,
// missing_field, invalid_field (no cus_ id, no positive id); 404 not_found for
// no such player or a customer Stripe does not know (or deleted); 503
// stripe_unavailable; 200 {"user_id", "stripe_customer_id", "outcome"}.
crow::response BillingRoutes::admin_link(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	if (billing_ == nullptr) return disabled();
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const auto user_id = body->integer("user_id");
	const auto customer_id = body->text("stripe_customer_id");
	if (body->wrong_type()) return json_error(400, "invalid_field");
	if (!user_id || !customer_id) return json_error(400, "missing_field", "user_id and stripe_customer_id are required");
	if (*user_id <= 0 || customer_id->rfind("cus_", 0) != 0 || !is_stripe_id(*customer_id))
		return json_error(400, "invalid_field", "user_id is an account id and stripe_customer_id a cus_ id");
	std::optional<UserRecord> user;
	try {
		user = get_user_by_id(*pool_.acquire(), *user_id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/billing/link failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	if (!user || user->kind != "player") return json_error(404, "not_found", "no player account has this id");
	StripeClient &client = billing_->client();
	const auto customer = client.get_customer(*customer_id);
	if (!customer.ok()) {
		if (customer.error.kind == StripeError::Kind::Rejected && customer.error.status == 404)
			return json_error(404, "not_found", "Stripe has no such customer");
		return stripe_unavailable();
	}
	if (customer.value->deleted) return json_error(404, "not_found", "Stripe deleted this customer");
	const auto stamped = client.update_customer_metadata(*customer_id,
			{ { kMetaInstance, billing_->instance_id() }, { kMetaUserId, std::to_string(*user_id) },
					{ kMetaUsername, user->username } });
	if (!stamped.ok()) return stripe_unavailable();
	SyncOutcome outcome = SyncOutcome::Unreachable;
	try {
		auto db_conn = pool_.acquire();
		{
			db::Transaction tx(*db_conn);
			db_conn->exec("DELETE FROM billing_customers WHERE user_id = $1 OR stripe_customer_id = $2;",
					{ db::BindValue(static_cast<int64_t>(*user_id)), db::BindValue(*customer_id) });
			insert_customer(*db_conn, *user_id, *customer_id, customer.value->livemode);
			tx.commit();
		}
		outcome = billing_->sync().sync_customer(*db_conn, *customer_id, unix_now());
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/billing/link failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	std::printf("[billing] customer %s linked to account '%s' (id=%lld) by an admin: %s\n",
			loggable(*customer_id).c_str(), loggable(user->username).c_str(), static_cast<long long>(*user_id),
			sync_outcome_name(outcome));
	crow::json::wvalue out;
	out["user_id"] = *user_id;
	out["stripe_customer_id"] = *customer_id;
	out["outcome"] = sync_outcome_name(outcome);
	return json_reply(200, out);
}

} // namespace opennova::novaworld_server::billing
