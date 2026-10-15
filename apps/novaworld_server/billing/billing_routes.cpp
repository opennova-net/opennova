#include "billing_routes.h"

#include "auth.h"
#include "hosted/hosted_repository.h"
#include "http_json.h"
#include "loggable.h"
#include "stripe_signature.h"
#include "tiers.h"

#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::billing {

namespace {

crow::response no_store(crow::response res) {
	res.set_header("Cache-Control", "no-store");
	return res;
}

crow::response disabled() {
	return json_error(503, "billing_disabled", "support tiers are not open on this NovaWorld service");
}

crow::response stripe_unavailable() {
	return json_error(502, "stripe_unavailable", "Stripe did not answer; try again in a moment");
}

void text_or_null(crow::json::wvalue &out, const char *key, const std::string &text) {
	if (text.empty())
		out[key] = nullptr;
	else
		out[key] = text;
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

// The item a checkout names: a tier 1..3, or 0 for the add-on; nullopt for none.
std::optional<int> checkout_item(const std::string &item) {
	if (item == "own_machine") return 0;
	for (int tier = 1; tier <= kTierCount; ++tier) {
		if (item == "tier" + std::to_string(tier)) return tier;
	}
	return std::nullopt;
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
// unit; null while unread), interval: "month"}], "own_machine": {amount,
// interval: "month"} with billing on, else null}. The amounts are Stripe's
// price objects', never the service's own.
crow::response BillingRoutes::tiers() {
	SyncStatus status;
	if (billing_ != nullptr) status = billing_->sync().status();
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
		list.push_back(std::move(t));
	}
	out["tiers"] = std::move(list);
	if (billing_ != nullptr) {
		crow::json::wvalue own;
		amount_or_null(own, status.own_machine_price);
		own["interval"] = "month";
		out["own_machine"] = std::move(own);
	} else {
		out["own_machine"] = nullptr;
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "public, max-age=60");
	return res;
}

// The account's billing: {"enabled", "max_servers", "comp_servers",
// "own_machines", "used" (its live hosted servers), "can_start", "subscription"
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
	out["own_machines"] = e.own_machines;
	out["used"] = hosted::count_live_of(db, user_id);
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
	out["can_subscribe"] = billing_->sync().status().prices_ok && active && !tier_grants(rows, now);
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

// POST /api/billing/checkout — {"item": "tier1" | "tier2" | "tier3" |
// "own_machine"}: a Stripe Checkout session for it, {"url"} to send the browser
// to. In order: require_user (401, 403 without the CSRF header), 503
// billing_disabled, the account's kCheckoutPerUser bucket (429), 400
// invalid_json / invalid_item, 503 billing_misconfigured while the prices do
// not check out, 403 not_eligible for an account that is not an active
// player's or (the add-on) has no tier granting, 409 already_subscribed when a
// tier grants already (the portal changes it), 502 stripe_unavailable. The
// account's previous open session is expired first; its Stripe customer is made
// at its first checkout (the idempotency key opennova-<instance>-customer-<id>,
// so a retry answers the same one). The session asks for the terms of service,
// lives kCheckoutLifeS, returns to /me/support?checkout=success&session_id=...
// or ?checkout=cancel, and stamps the instance and the account on the session
// and on the subscription it makes.
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
	const std::optional<int> item = item_text ? checkout_item(*item_text) : std::nullopt;
	if (body->wrong_type() || !item) return json_error(400, "invalid_item", "item is tier1, tier2, tier3 or own_machine");
	const BillingConfig &config = billing_->config();
	if (!billing_->sync().status().prices_ok)
		return json_error(503, "billing_misconfigured", "the support tiers' prices are being checked; try again later");
	const std::string price = *item > 0 ? tier_price_for_sale(config, *item) : own_machine_price_for_sale(config);
	const int64_t now = unix_now();
	std::optional<CustomerRow> customer;
	try {
		auto db_conn = pool_.acquire();
		const auto account = get_user_by_id(*db_conn, user.id);
		if (!account || account->kind != "player" || account->account_status != "active") {
			log_account(user, "checkout", "not_eligible (account)");
			return json_error(403, "not_eligible", "this account cannot subscribe");
		}
		const bool granting_tier = tier_grants(subscriptions_of(*db_conn, user.id), now);
		if (*item > 0 && granting_tier) {
			log_account(user, "checkout", "already_subscribed");
			return json_error(409, "already_subscribed", "this account has a support tier; change it in the portal");
		}
		if (*item == 0 && !granting_tier) {
			log_account(user, "checkout", "not_eligible (no tier)");
			return json_error(403, "not_eligible", "an own machine needs a support tier first");
		}
		customer = customer_of(*db_conn, user.id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/billing/checkout failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	// Stripe, outside any transaction.
	StripeClient &client = billing_->client();
	if (!customer) {
		const auto made = client.create_customer(user.id, user.username, billing_->instance_id(),
				"opennova-" + billing_->instance_id() + "-customer-" + std::to_string(user.id));
		if (!made.ok()) {
			log_account(user, "checkout", std::string("customer not made: ") + stripe_error_kind_name(made.error.kind));
			return stripe_unavailable();
		}
		try {
			auto db_conn = pool_.acquire();
			insert_customer(*db_conn, user.id, made.value->id, made.value->livemode);
			customer = customer_of(*db_conn, user.id);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] POST /api/billing/checkout failed: %s\n", e.what());
			return json_error(500, "db_error");
		}
		if (!customer) return json_error(500, "db_error");
	}
	if (!customer->open_checkout_session.empty()) {
		// Already complete or expired: Stripe refuses, which is fine.
		client.expire_checkout_session(customer->open_checkout_session);
	}
	CheckoutCreate create;
	create.customer = customer->stripe_customer_id;
	create.client_reference_id = std::to_string(user.id);
	create.price_id = price;
	create.quantity = 1;
	create.success_url = config.site_url + "/me/support?checkout=success&session_id={CHECKOUT_SESSION_ID}";
	create.cancel_url = config.site_url + "/me/support?checkout=cancel";
	create.metadata = { { kMetaInstance, billing_->instance_id() }, { kMetaUserId, std::to_string(user.id) },
		{ kMetaUsername, user.username } };
	create.expires_at = now + kCheckoutLifeS;
	create.automatic_tax = config.automatic_tax;
	create.require_tos_consent = true;
	const auto session = client.create_checkout_session(create);
	if (!session.ok() || session.value->url.empty()) {
		log_account(user, "checkout",
				std::string("no session: ") + (session.ok() ? "no url" : stripe_error_kind_name(session.error.kind)));
		return stripe_unavailable();
	}
	try {
		set_open_checkout_session(*pool_.acquire(), user.id, session.value->id);
	} catch (const db::Error &e) {
		// The session stands; only the next checkout's expiry of it is lost.
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
// no_billing_account with no customer yet, 502 stripe_unavailable.
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
		log_account(user, "portal", std::string("no session: ") + stripe_error_kind_name(url.error.kind));
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
// no_billing_account with no customer yet, 502 stripe_unavailable.
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
// 200 {"received": true}: the worker reads the customer back from Stripe.
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
		const bool fresh = record_event(*pool_.acquire(), event->id, event->type, event->livemode, event->customer_id,
				event->created > 0 ? std::optional<int64_t>(event->created) : std::nullopt);
		std::printf("[billing] webhook %s %s customer %s%s\n", loggable(event->id).c_str(), loggable(event->type).c_str(),
				event->customer_id.empty() ? "(none)" : loggable(event->customer_id).c_str(), fresh ? "" : " (again)");
		if (fresh && !event->customer_id.empty()) billing_->sync().notify();
	} catch (const db::Error &e) {
		// Stripe delivers it again on a 5xx.
		std::fprintf(stderr, "[http] POST /api/billing/webhook failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return json_reply(200, out);
}

// GET /api/admin/billing — require_admin; 503 billing_disabled; {"enabled",
// "mode" ("test" | "live"), "prices_ok", "prices_error" (null when ok),
// "last_reconcile_at" (ISO, null before one completed), "last_reconcile_error"
// (null when the last completed), "pending_events" (events not synced yet),
// "subscriptions" (every row with an account: admin_subscription_json plus
// user_id, username, stripe_customer_id), "duplicates" (accounts with two
// granting tiers), "orphans" ({stripe_customer_id, stripe_subscription_id,
// username_hint, status}), "recent_events" (the newest kEventsShown: event_id,
// type, received_at (ISO), outcome)}. Cache-Control: no-store.
crow::response BillingRoutes::admin_billing(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	if (billing_ == nullptr) return disabled();
	const SyncStatus status = billing_->sync().status();
	const int64_t now = unix_now();
	crow::json::wvalue out;
	out["enabled"] = true;
	out["mode"] = billing_->config().live() ? "live" : "test";
	out["prices_ok"] = status.prices_ok;
	text_or_null(out, "prices_error", status.prices_error);
	if (status.last_reconcile_at > 0)
		out["last_reconcile_at"] = iso_utc(status.last_reconcile_at);
	else
		out["last_reconcile_at"] = nullptr;
	text_or_null(out, "last_reconcile_error", status.last_reconcile_error);
	// The hosted servers' limits hold: an admin's reconcile clears it.
	out["limits_hold"] = limits_hold_ != nullptr && limits_hold_->held();
	text_or_null(out, "limits_hold_reason", limits_hold_ != nullptr ? limits_hold_->reason() : std::string());
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

// POST /api/admin/billing/reconcile — a full reconcile on the worker's next
// pass, now: 202 {"reconcile": "requested"}. It clears the hosted servers'
// limits hold first, so the supervisor's next pass judges the owners' limits
// again (after the reconcile, when it lands first). require_admin; 503
// billing_disabled (the hold cleared all the same).
crow::response BillingRoutes::admin_reconcile(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal) return std::move(*caller.refusal);
	if (limits_hold_ != nullptr && limits_hold_->clear())
		std::printf("[hosted] the owners' limits hold cleared by an admin's reconcile\n");
	if (billing_ == nullptr) return disabled();
	billing_->sync().request_reconcile();
	std::printf("[billing] reconcile asked for by an admin\n");
	crow::json::wvalue out;
	out["reconcile"] = "requested";
	return json_reply(202, out);
}

// POST /api/admin/billing/link — {"user_id", "stripe_customer_id"}: a Stripe
// customer (an orphan's) made the account's: its metadata stamped with this
// instance and the account, the account's customer row replaced, and the
// customer synced. require_admin; 503 billing_disabled; 400 invalid_json,
// missing_field, invalid_field (no cus_ id, no positive id); 404 not_found for
// no such player or a customer Stripe does not know (or deleted); 502
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
