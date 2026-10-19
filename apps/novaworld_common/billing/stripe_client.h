#pragma once

// The Stripe calls the billing code makes, typed: each builds its request over
// a StripeApi (stripe_api.h), and reads Stripe's reply into a value of the
// pinned API version's shape (kStripeApiVersion, basil: a subscription's
// current_period_end is on each of its items) or a StripeError. Nothing here
// keeps state between calls or touches the database; the sync worker and the
// routes decide what a reply means. Every POST carries an idempotency key (the
// caller's, or a fresh random one, so the transport's one retry can never act
// twice). An id is checked before it is spelled into a path. JSON through
// base/io/json.h. Crow-free. Service policy over Stripe's REST API, not a port.

#include "stripe_api.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace opennova::io {
struct JsonValue;
}

namespace opennova::novaworld_server::billing {

// The metadata keys the service stamps on its customers and subscriptions: the
// database instance (service_meta.instance_id) and the account, so a reset or
// a developer's local database on a shared Stripe test account never adopts
// another instance's subscription.
inline constexpr const char *kMetaInstance = "opennova_instance";
inline constexpr const char *kMetaUserId = "opennova_user_id";
inline constexpr const char *kMetaUsername = "opennova_username";

using Metadata = std::map<std::string, std::string>;

struct StripeError {
	enum class Kind {
		Unreachable, // no answer, or Stripe could not serve it now (429, 5xx): try again later
		Rejected,    // Stripe refused it (another 4xx), or this side refused the arguments (status 0)
		Malformed,   // a 2xx whose body is not the object asked for
	};
	Kind kind = Kind::Unreachable;
	int status = 0;          // Stripe's HTTP status; 0 when there was no answer
	std::string stripe_code; // error.code (error.type when it has no code): "resource_missing"
	std::string message;     // error.message, or what went wrong; never a key
};

const char *stripe_error_kind_name(StripeError::Kind kind);
// The error for a log line: its kind, Stripe's status and code, and its message
// ("rejected 404 (resource_missing): No such customer: 'cus_1'"); never a key.
std::string stripe_error_text(const StripeError &e);

// A value, or (when `value` is empty) the error that kept it.
template <typename T> struct StripeResult {
	std::optional<T> value;
	StripeError error;

	bool ok() const { return value.has_value(); }
};

struct Customer {
	std::string id; // cus_...
	bool deleted = false; // a deleted customer answers its id and this alone
	bool livemode = false;
	Metadata metadata;
	std::string email;
};

struct SubscriptionItem {
	std::string id;       // si_...
	std::string price_id; // price_...
	int64_t quantity = 0;
	int64_t current_period_end = 0; // Unix seconds (basil: per item)
};

struct Subscription {
	std::string id;       // sub_...
	std::string customer; // cus_... (its id also when it was expanded)
	// incomplete, incomplete_expired, trialing, active, past_due, canceled, unpaid, paused
	std::string status;
	bool livemode = false;
	std::vector<SubscriptionItem> items;
	bool cancel_at_period_end = false;
	int64_t cancel_at = 0; // Unix seconds; 0 when none
	int64_t created = 0;
	Metadata metadata;
	// The customer's metadata and deleted flag, when it was expanded.
	std::optional<Metadata> customer_metadata;
	bool customer_deleted = false;

	// The first item's price and period end ("" / 0 when it has no item): a
	// tier or dedicated-machine subscription carries one item.
	std::string first_price_id() const;
	int64_t first_period_end() const;
	int64_t first_quantity() const;
};

struct SubscriptionPage {
	std::vector<Subscription> data;
	bool has_more = false;
};

struct SubscriptionQuery {
	std::string customer;       // "" for every customer
	std::string status = "all"; // "all" includes canceled; "" is Stripe's default (all but canceled)
	std::string starting_after; // the last id of the previous page
	int limit = 100;            // 1..100
	bool expand_customer = false; // data.customer: each subscription's customer metadata
};

struct Price {
	std::string id; // price_...
	bool active = false;
	bool livemode = false;
	std::optional<int64_t> unit_amount; // in the currency's smallest unit; none for a tiered price
	std::string currency;               // "usd"
	std::string type;                   // "recurring" or "one_time"
	std::string recurring_interval;     // "month", "year", ...; "" when not recurring
	int64_t recurring_interval_count = 0;
	std::string product; // prod_...
};

struct CheckoutCreate {
	std::string customer;            // cus_...
	std::string client_reference_id; // the account id
	std::string price_id;
	int64_t quantity = 1;
	std::string success_url; // may hold Stripe's {CHECKOUT_SESSION_ID}
	std::string cancel_url;
	Metadata metadata; // on the session and on the subscription it creates
	int64_t expires_at = 0; // Unix seconds; 0 for Stripe's default (24 h)
	bool automatic_tax = false;       // Stripe Tax (the customer's address is collected onto it)
	bool require_tos_consent = false; // the terms-of-service checkbox
	std::string idempotency_key;      // "" for a fresh random one
};

struct CheckoutSession {
	std::string id;     // cs_...
	std::string url;    // "" once it is complete or expired
	std::string status; // open, complete, expired
	std::string customer;
	std::string subscription; // sub_... once complete
	std::string client_reference_id;
	std::string mode;
	std::string payment_status;
	Metadata metadata;
	int64_t expires_at = 0;
	bool livemode = false;
};

class StripeClient {
public:
	explicit StripeClient(StripeApi &api);

	// POST /v1/customers with the three opennova_* metadata keys; the
	// idempotency key is the caller's, so a second attempt (after a lost reply)
	// answers the same customer.
	StripeResult<Customer> create_customer(int64_t user_id, const std::string &username,
			const std::string &instance_id, const std::string &idempotency_key);
	StripeResult<Customer> get_customer(const std::string &id);
	// Sets each key (an empty value deletes it); others are kept.
	StripeResult<Customer> update_customer_metadata(const std::string &id, const Metadata &metadata);

	// A subscription-mode session for one line item.
	StripeResult<CheckoutSession> create_checkout_session(const CheckoutCreate &create);
	StripeResult<CheckoutSession> expire_checkout_session(const std::string &id);
	StripeResult<CheckoutSession> get_checkout_session(const std::string &id);

	// A Customer Portal session's URL; with `cancel_subscription` (sub_...),
	// the Portal's cancel flow for that subscription
	// (flow_data[type]=subscription_cancel).
	StripeResult<std::string> create_portal_session(const std::string &customer, const std::string &return_url,
			const std::string &cancel_subscription = {});

	// One page of GET /v1/subscriptions.
	StripeResult<SubscriptionPage> list_subscriptions(const SubscriptionQuery &query);
	StripeResult<Subscription> get_subscription(const std::string &id, bool expand_customer = false);
	StripeResult<Subscription> update_subscription_metadata(const std::string &id, const Metadata &metadata);
	// The subscription set to cancel at its period's end (cancel_at_period_end:
	// it renews no more, and grants until then), under the caller's
	// idempotency key.
	StripeResult<Subscription> cancel_at_period_end(const std::string &id, const std::string &idempotency_key);

	StripeResult<Price> get_price(const std::string &id);

private:
	StripeApi &api_;
};

// Whether `text` can be an id Stripe minted: 1 to 255 letters, digits and '_'
// (what a path may carry).
bool is_stripe_id(const std::string &text);

// The readers behind the calls, for a JSON object already parsed (the sync
// worker's tests, an expanded object): nullopt unless it is that object
// ("object":"customer", ...) with the fields each value requires.
std::optional<Customer> customer_from_json(const io::JsonValue &json);
std::optional<Subscription> subscription_from_json(const io::JsonValue &json);
std::optional<Price> price_from_json(const io::JsonValue &json);
std::optional<CheckoutSession> checkout_session_from_json(const io::JsonValue &json);

} // namespace opennova::novaworld_server::billing
