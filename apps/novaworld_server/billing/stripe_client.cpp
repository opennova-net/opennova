#include "stripe_client.h"

#include "loggable.h"

#include <base/io/json.h>
#include <base/os_random/os_random.h>

#include <algorithm>
#include <utility>

namespace opennova::novaworld_server::billing {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// --- reading ------------------------------------------------------------------------------------

// The string member `key`; "" when absent, null or not a string.
std::string text_of(const io::JsonValue &object, const char *key) {
	const io::JsonValue *v = object.get(key);
	return v != nullptr && v->is_string() ? v->string : std::string();
}

// An id member that may be expanded into its object: the string, or the object's id.
std::string id_of(const io::JsonValue &object, const char *key) {
	const io::JsonValue *v = object.get(key);
	if (v == nullptr) return {};
	if (v->is_object()) return text_of(*v, "id");
	return v->is_string() ? v->string : std::string();
}

// A whole-number member into `out`: true when it is one (0 or more, exactly
// whole), or when it is absent or null and `optional` (out = 0).
bool whole_of(const io::JsonValue &object, const char *key, int64_t &out, bool optional) {
	out = 0;
	const io::JsonValue *v = object.get(key);
	if (v == nullptr || v->is_null()) return optional;
	return io::json_exact_whole_in(*v, 0, io::kJsonSafeWholeMax, out);
}

// A boolean member into `out`: true when it is one, or when it is absent and
// `optional` (out = false).
bool bool_of(const io::JsonValue &object, const char *key, bool &out, bool optional) {
	out = false;
	const io::JsonValue *v = object.get(key);
	if (v == nullptr) return optional;
	if (!v->is_bool()) return false;
	out = v->boolean;
	return true;
}

// The "metadata" member: an object of strings (absent or null: empty).
bool metadata_of(const io::JsonValue &object, Metadata &out) {
	out.clear();
	const io::JsonValue *v = object.get("metadata");
	if (v == nullptr || v->is_null()) return true;
	if (!v->is_object()) return false;
	for (const io::JsonMember &m : v->object) {
		if (!m.value.is_string()) return false;
		out[m.key] = m.value.string;
	}
	return true;
}

bool is_object_of(const io::JsonValue &json, const char *type) {
	return json.is_object() && text_of(json, "object") == type && !text_of(json, "id").empty();
}

std::optional<SubscriptionItem> item_from_json(const io::JsonValue &json) {
	if (!is_object_of(json, "subscription_item")) return std::nullopt;
	SubscriptionItem item;
	item.id = text_of(json, "id");
	const io::JsonValue *price = json.get("price");
	item.price_id = price != nullptr && price->is_object() ? text_of(*price, "id") : std::string();
	// Quantity is absent on a metered price, which grants nothing.
	if (item.price_id.empty() || !whole_of(json, "quantity", item.quantity, true) ||
			!whole_of(json, "current_period_end", item.current_period_end, false))
		return std::nullopt;
	return item;
}

// --- errors -------------------------------------------------------------------------------------

StripeError error_of(const StripeReply &reply) {
	StripeError e;
	if (reply.outcome == StripeReply::Outcome::Unreachable) {
		e.kind = StripeError::Kind::Unreachable;
		e.message = reply.error;
		return e;
	}
	e.status = reply.status;
	e.kind = reply.status == 429 || reply.status >= 500 ? StripeError::Kind::Unreachable
														 : StripeError::Kind::Rejected;
	io::JsonValue json;
	std::string parse_error;
	if (io::json_parse(reply.body, json, parse_error)) {
		if (const io::JsonValue *error = json.get("error"); error != nullptr && error->is_object()) {
			e.stripe_code = text_of(*error, "code");
			if (e.stripe_code.empty()) e.stripe_code = text_of(*error, "type");
			e.message = loggable(text_of(*error, "message"), 300);
		}
	}
	e.stripe_code = loggable(e.stripe_code, 80);
	if (e.message.empty()) e.message = "Stripe answered HTTP " + std::to_string(reply.status);
	return e;
}

template <typename T> StripeResult<T> refused(std::string message) {
	StripeResult<T> result;
	result.error.kind = StripeError::Kind::Rejected;
	result.error.message = std::move(message);
	return result;
}

template <typename T> StripeResult<T> malformed(int status, std::string message) {
	StripeResult<T> result;
	result.error.kind = StripeError::Kind::Malformed;
	result.error.status = status;
	result.error.message = std::move(message);
	return result;
}

// Sends `request` and reads a 2xx reply's JSON with `read`, which answers
// nullopt for a body that is not `what`.
template <typename T, typename Read>
StripeResult<T> exchange(StripeApi &api, const StripeRequest &request, const char *what, Read read) {
	const StripeReply reply = api.call(request);
	if (!reply.ok()) {
		StripeResult<T> result;
		result.error = error_of(reply);
		return result;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(reply.body, json, parse_error))
		return malformed<T>(reply.status, std::string("Stripe answered no JSON for ") + what + " (" +
						loggable(parse_error, 120) + ")");
	StripeResult<T> result;
	result.value = read(json);
	if (!result.value) return malformed<T>(reply.status, std::string("Stripe answered no ") + what);
	return result;
}

// --- writing ------------------------------------------------------------------------------------

std::string fresh_idempotency_key() {
	static constexpr char kDigits[] = "0123456789abcdef";
	uint8_t bytes[16];
	os_random_bytes(bytes, sizeof(bytes));
	std::string out = "opennova-";
	for (const uint8_t b : bytes) {
		out.push_back(kDigits[b >> 4]);
		out.push_back(kDigits[b & 0xFu]);
	}
	return out;
}

// "" when every key can be a field name (Stripe's: up to 40 characters, no
// bracket, no control byte), else why not.
std::string metadata_refusal(const Metadata &metadata) {
	for (const auto &[key, value] : metadata) {
		if (key.empty() || key.size() > 40 || key.find_first_of("[]") != std::string::npos ||
				has_control_byte(key))
			return "the metadata key '" + loggable(key, 60) + "' is not 1 to 40 characters with no bracket";
	}
	return {};
}

void append_metadata(Params &params, const std::string &prefix, const Metadata &metadata) {
	for (const auto &[key, value] : metadata) params.emplace_back(prefix + "[" + key + "]", value);
}

StripeRequest get(std::string path, Params params = {}) {
	StripeRequest r;
	r.method = StripeRequest::Method::Get;
	r.path = std::move(path);
	r.params = std::move(params);
	return r;
}

StripeRequest post(std::string path, Params params, std::string idempotency_key = {}) {
	StripeRequest r;
	r.method = StripeRequest::Method::Post;
	r.path = std::move(path);
	r.params = std::move(params);
	r.idempotency_key = idempotency_key.empty() ? fresh_idempotency_key() : std::move(idempotency_key);
	return r;
}

std::string not_an_id(const std::string &what, const std::string &id) {
	return what + " '" + loggable(id, 80) + "' is not a Stripe id";
}

std::optional<std::string> portal_url_from_json(const io::JsonValue &json) {
	if (!is_object_of(json, "billing_portal.session")) return std::nullopt;
	std::string url = text_of(json, "url");
	if (url.empty()) return std::nullopt;
	return url;
}

std::optional<SubscriptionPage> subscription_page_from_json(const io::JsonValue &json) {
	if (!json.is_object() || text_of(json, "object") != "list") return std::nullopt;
	const io::JsonValue *data = json.get("data");
	SubscriptionPage page;
	if (data == nullptr || !data->is_array() || !bool_of(json, "has_more", page.has_more, false))
		return std::nullopt;
	for (const io::JsonValue &element : data->array) {
		std::optional<Subscription> sub = subscription_from_json(element);
		if (!sub) return std::nullopt;
		page.data.push_back(std::move(*sub));
	}
	return page;
}

} // namespace

const char *stripe_error_kind_name(StripeError::Kind kind) {
	switch (kind) {
		case StripeError::Kind::Unreachable: return "unreachable";
		case StripeError::Kind::Rejected: return "rejected";
		case StripeError::Kind::Malformed: return "malformed";
	}
	return "unknown";
}

std::string Subscription::first_price_id() const { return items.empty() ? std::string() : items[0].price_id; }
int64_t Subscription::first_period_end() const { return items.empty() ? 0 : items[0].current_period_end; }
int64_t Subscription::first_quantity() const { return items.empty() ? 0 : items[0].quantity; }

bool is_stripe_id(const std::string &text) {
	if (text.empty() || text.size() > 255) return false;
	return std::all_of(text.begin(), text.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
	});
}

std::optional<Customer> customer_from_json(const io::JsonValue &json) {
	if (!is_object_of(json, "customer")) return std::nullopt;
	Customer c;
	c.id = text_of(json, "id");
	if (!bool_of(json, "deleted", c.deleted, true)) return std::nullopt;
	// A deleted customer answers {id, object, deleted} alone.
	if (!bool_of(json, "livemode", c.livemode, c.deleted) || !metadata_of(json, c.metadata)) return std::nullopt;
	c.email = text_of(json, "email");
	return c;
}

std::optional<Subscription> subscription_from_json(const io::JsonValue &json) {
	if (!is_object_of(json, "subscription")) return std::nullopt;
	Subscription s;
	s.id = text_of(json, "id");
	s.status = text_of(json, "status");
	s.customer = id_of(json, "customer");
	if (s.status.empty() || s.customer.empty() || !bool_of(json, "livemode", s.livemode, false) ||
			!bool_of(json, "cancel_at_period_end", s.cancel_at_period_end, true) ||
			!whole_of(json, "cancel_at", s.cancel_at, true) || !whole_of(json, "created", s.created, true) ||
			!metadata_of(json, s.metadata))
		return std::nullopt;
	if (const io::JsonValue *customer = json.get("customer"); customer->is_object()) {
		// Expanded: a customer, or what is left of a deleted one.
		const std::optional<Customer> expanded = customer_from_json(*customer);
		if (!expanded) return std::nullopt;
		s.customer_deleted = expanded->deleted;
		s.customer_metadata = expanded->metadata;
	}
	const io::JsonValue *items = json.get("items");
	const io::JsonValue *data = items != nullptr && items->is_object() ? items->get("data") : nullptr;
	if (data == nullptr || !data->is_array()) return std::nullopt;
	for (const io::JsonValue &element : data->array) {
		std::optional<SubscriptionItem> item = item_from_json(element);
		if (!item) return std::nullopt;
		s.items.push_back(std::move(*item));
	}
	return s;
}

std::optional<Price> price_from_json(const io::JsonValue &json) {
	if (!is_object_of(json, "price")) return std::nullopt;
	Price p;
	p.id = text_of(json, "id");
	p.currency = text_of(json, "currency");
	p.type = text_of(json, "type");
	p.product = id_of(json, "product");
	if (p.currency.empty() || p.type.empty() || !bool_of(json, "active", p.active, false) ||
			!bool_of(json, "livemode", p.livemode, false))
		return std::nullopt;
	if (const io::JsonValue *amount = json.get("unit_amount"); amount != nullptr && !amount->is_null()) {
		int64_t value = 0;
		if (!io::json_exact_whole_in(*amount, 0, io::kJsonSafeWholeMax, value)) return std::nullopt;
		p.unit_amount = value;
	}
	if (const io::JsonValue *recurring = json.get("recurring"); recurring != nullptr && !recurring->is_null()) {
		if (!recurring->is_object()) return std::nullopt;
		p.recurring_interval = text_of(*recurring, "interval");
		if (p.recurring_interval.empty() ||
				!whole_of(*recurring, "interval_count", p.recurring_interval_count, false))
			return std::nullopt;
	}
	return p;
}

std::optional<CheckoutSession> checkout_session_from_json(const io::JsonValue &json) {
	if (!is_object_of(json, "checkout.session")) return std::nullopt;
	CheckoutSession c;
	c.id = text_of(json, "id");
	c.url = text_of(json, "url");
	c.status = text_of(json, "status");
	c.customer = id_of(json, "customer");
	c.subscription = id_of(json, "subscription");
	c.client_reference_id = text_of(json, "client_reference_id");
	c.mode = text_of(json, "mode");
	c.payment_status = text_of(json, "payment_status");
	if (!metadata_of(json, c.metadata) || !whole_of(json, "expires_at", c.expires_at, true) ||
			!bool_of(json, "livemode", c.livemode, false))
		return std::nullopt;
	return c;
}

StripeClient::StripeClient(StripeApi &api) : api_(api) {}

StripeResult<Customer> StripeClient::create_customer(int64_t user_id, const std::string &username,
		const std::string &instance_id, const std::string &idempotency_key) {
	if (user_id <= 0 || instance_id.empty()) return refused<Customer>("no account or instance for the customer");
	Params params;
	append_metadata(params, "metadata",
			{ { kMetaInstance, instance_id }, { kMetaUserId, std::to_string(user_id) }, { kMetaUsername, username } });
	return exchange<Customer>(api_, post("/v1/customers", std::move(params), idempotency_key), "customer",
			customer_from_json);
}

StripeResult<Customer> StripeClient::get_customer(const std::string &id) {
	if (!is_stripe_id(id)) return refused<Customer>(not_an_id("the customer", id));
	return exchange<Customer>(api_, get("/v1/customers/" + id), "customer", customer_from_json);
}

StripeResult<Customer> StripeClient::update_customer_metadata(const std::string &id, const Metadata &metadata) {
	if (!is_stripe_id(id)) return refused<Customer>(not_an_id("the customer", id));
	if (const std::string why = metadata_refusal(metadata); !why.empty()) return refused<Customer>(why);
	Params params;
	append_metadata(params, "metadata", metadata);
	return exchange<Customer>(api_, post("/v1/customers/" + id, std::move(params)), "customer", customer_from_json);
}

StripeResult<CheckoutSession> StripeClient::create_checkout_session(const CheckoutCreate &create) {
	if (!is_stripe_id(create.customer)) return refused<CheckoutSession>(not_an_id("the customer", create.customer));
	if (!is_stripe_id(create.price_id)) return refused<CheckoutSession>(not_an_id("the price", create.price_id));
	if (create.quantity < 1) return refused<CheckoutSession>("the quantity is below 1");
	if (create.success_url.empty() || create.cancel_url.empty())
		return refused<CheckoutSession>("no success or cancel URL");
	if (const std::string why = metadata_refusal(create.metadata); !why.empty())
		return refused<CheckoutSession>(why);
	Params params = {
		{ "mode", "subscription" },
		{ "customer", create.customer },
		{ "line_items[0][price]", create.price_id },
		{ "line_items[0][quantity]", std::to_string(create.quantity) },
		{ "success_url", create.success_url },
		{ "cancel_url", create.cancel_url },
	};
	if (!create.client_reference_id.empty()) params.emplace_back("client_reference_id", create.client_reference_id);
	append_metadata(params, "metadata", create.metadata);
	append_metadata(params, "subscription_data[metadata]", create.metadata);
	if (create.expires_at > 0) params.emplace_back("expires_at", std::to_string(create.expires_at));
	if (create.automatic_tax) {
		params.emplace_back("automatic_tax[enabled]", "true");
		// Stripe Tax reads the customer's address: the one Checkout collects is saved onto it.
		params.emplace_back("customer_update[address]", "auto");
	}
	if (create.require_tos_consent) params.emplace_back("consent_collection[terms_of_service]", "required");
	return exchange<CheckoutSession>(api_, post("/v1/checkout/sessions", std::move(params), create.idempotency_key),
			"checkout session", checkout_session_from_json);
}

StripeResult<CheckoutSession> StripeClient::expire_checkout_session(const std::string &id) {
	if (!is_stripe_id(id)) return refused<CheckoutSession>(not_an_id("the checkout session", id));
	return exchange<CheckoutSession>(api_, post("/v1/checkout/sessions/" + id + "/expire", {}),
			"checkout session", checkout_session_from_json);
}

StripeResult<CheckoutSession> StripeClient::get_checkout_session(const std::string &id) {
	if (!is_stripe_id(id)) return refused<CheckoutSession>(not_an_id("the checkout session", id));
	return exchange<CheckoutSession>(api_, get("/v1/checkout/sessions/" + id), "checkout session",
			checkout_session_from_json);
}

StripeResult<std::string> StripeClient::create_portal_session(const std::string &customer,
		const std::string &return_url) {
	if (!is_stripe_id(customer)) return refused<std::string>(not_an_id("the customer", customer));
	if (return_url.empty()) return refused<std::string>("no return URL");
	return exchange<std::string>(api_,
			post("/v1/billing_portal/sessions", { { "customer", customer }, { "return_url", return_url } }),
			"portal session", portal_url_from_json);
}

StripeResult<SubscriptionPage> StripeClient::list_subscriptions(const SubscriptionQuery &query) {
	Params params;
	if (!query.customer.empty()) {
		if (!is_stripe_id(query.customer)) return refused<SubscriptionPage>(not_an_id("the customer", query.customer));
		params.emplace_back("customer", query.customer);
	}
	if (!query.status.empty()) params.emplace_back("status", query.status);
	if (!query.starting_after.empty()) {
		if (!is_stripe_id(query.starting_after))
			return refused<SubscriptionPage>(not_an_id("the page cursor", query.starting_after));
		params.emplace_back("starting_after", query.starting_after);
	}
	params.emplace_back("limit", std::to_string(std::clamp(query.limit, 1, 100)));
	if (query.expand_customer) params.emplace_back("expand[]", "data.customer");
	return exchange<SubscriptionPage>(api_, get("/v1/subscriptions", std::move(params)), "subscription list",
			subscription_page_from_json);
}

StripeResult<Subscription> StripeClient::get_subscription(const std::string &id, bool expand_customer) {
	if (!is_stripe_id(id)) return refused<Subscription>(not_an_id("the subscription", id));
	Params params;
	if (expand_customer) params.emplace_back("expand[]", "customer");
	return exchange<Subscription>(api_, get("/v1/subscriptions/" + id, std::move(params)), "subscription",
			subscription_from_json);
}

StripeResult<Subscription> StripeClient::update_subscription_metadata(const std::string &id,
		const Metadata &metadata) {
	if (!is_stripe_id(id)) return refused<Subscription>(not_an_id("the subscription", id));
	if (const std::string why = metadata_refusal(metadata); !why.empty()) return refused<Subscription>(why);
	Params params;
	append_metadata(params, "metadata", metadata);
	return exchange<Subscription>(api_, post("/v1/subscriptions/" + id, std::move(params)), "subscription",
			subscription_from_json);
}

StripeResult<Price> StripeClient::get_price(const std::string &id) {
	if (!is_stripe_id(id)) return refused<Price>(not_an_id("the price", id));
	return exchange<Price>(api_, get("/v1/prices/" + id), "price", price_from_json);
}

} // namespace opennova::novaworld_server::billing
