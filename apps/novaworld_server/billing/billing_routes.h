#pragma once

// The billing routes (README "Billing (monthly support tiers)"): the public
// GET /api/billing/tiers; the account's GET /api/me/billing, POST
// /api/billing/checkout, POST /api/billing/portal and POST /api/me/billing/sync
// (require_user, the CSRF header on the POSTs); Stripe's POST
// /api/billing/webhook (no session and no CSRF header: the Stripe-Signature
// header authenticates it); and the admin's GET /api/admin/billing, POST
// /api/admin/billing/reconcile and POST /api/admin/billing/link
// (require_admin). Without billing (set_billing null) every route answers 503
// billing_disabled but the tiers and the admin's view, which answer
// {"enabled": false} (the admin's with the limits hold), and an admin's
// override of the limits hold. Every
// refusal is an error code with our own message, never Stripe's or the
// database's text. No Stripe call runs inside a db::Transaction. Behind
// ONNET_API_PROXY_ONLY like every /api route (the webhook too: Stripe reaches it
// through the site's proxy). The admin's view shows the hosted servers' limits
// hold (hosted/limits_hold.h), billing on or off, and an admin's reconcile may
// carry the override that applies its stops. The HttpListener owns one.

#include "billing.h"
#include "billing_repository.h"
#include "hosted/limits_hold.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace opennova::novaworld_server::billing {

// "YYYY-MM-DDTHH:MM:SSZ" (UTC) for Unix seconds, and for the database's
// "YYYY-MM-DD HH:MM:SS" UTC text ("" stays "").
std::string iso_utc(int64_t unix_seconds);
std::string iso_from_db(const std::string &db_text);

// A subscription row for an admin view: {stripe_subscription_id, item, tier
// (null but for a tier), quantity, status, entitled (it grants at `now`),
// current_period_end (ISO, "" for none), cancel_at_period_end}.
crow::json::wvalue admin_subscription_json(const SubscriptionRow &row, int64_t now);

class BillingRoutes {
public:
	// A checkout or a portal session per account: 6 at once, then one every 20 s.
	static constexpr RateLimiter::Params kCheckoutPerUser{ 6, 1.0 / 20, 10000 };
	// The success page's synchronous sync per account: 3 at once, then one every 10 s.
	static constexpr RateLimiter::Params kSyncPerUser{ 3, 1.0 / 10, 10000 };
	// The largest webhook body read (413 past it).
	static constexpr size_t kWebhookMaxBytes = 1u << 20;
	// A checkout session's life: an hour, well past Stripe's minimum of 30
	// minutes from its creation (Stripe refuses one under that).
	static constexpr int64_t kCheckoutLifeS = 60 * 60;
	// The webhook events GET /api/admin/billing lists, newest first.
	static constexpr size_t kEventsShown = 50;

	BillingRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	BillingRoutes(const BillingRoutes &) = delete;
	BillingRoutes &operator=(const BillingRoutes &) = delete;

	// Before register_routes(); null: billing off. It must outlive the routes.
	void set_billing(Billing *billing) { billing_ = billing; }
	// The hosted servers' limits hold (null: none); it must outlive the routes.
	void set_limits_hold(hosted::LimitsHold *hold) { limits_hold_ = hold; }
	void register_routes(WebApp &app);

private:
	crow::response tiers();
	crow::response me_billing(const crow::request &req);
	crow::response checkout(const crow::request &req);
	crow::response portal(const crow::request &req);
	crow::response me_sync(const crow::request &req);
	crow::response webhook(const crow::request &req);
	crow::response admin_billing(const crow::request &req);
	crow::response admin_reconcile(const crow::request &req);
	crow::response admin_link(const crow::request &req);
	// GET /api/me/billing's body for `user_id`.
	crow::response me_billing_reply(db::Database &db, int64_t user_id);
	// The limits hold's fields of GET /api/admin/billing.
	void limits_hold_json(crow::json::wvalue &out) const;

	WebAccess &access_;
	db::ConnectionPool &pool_;
	Billing *billing_ = nullptr;
	hosted::LimitsHold *limits_hold_ = nullptr;
	RateLimiter checkout_per_user_{ kCheckoutPerUser };
	RateLimiter sync_per_user_{ kSyncPerUser };
};

} // namespace opennova::novaworld_server::billing
