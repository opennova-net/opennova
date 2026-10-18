// An in-memory Stripe behind the billing seam (apps/novaworld_common/billing/stripe_api.h)
// for the billing tests: customers, subscriptions, prices, Checkout and Customer Portal
// sessions, answering the routes the typed client calls (stripe_client.h) with the pinned
// API version's JSON shapes (basil: current_period_end on each subscription item). It
// records every request, replays a POST's reply for a repeated idempotency key, honours
// list pagination (newest first, limit, starting_after, has_more), and answers the next
// calls with queued failures (fail_next) and the next calls to one path with Stripe's refusal
// (refuse_next), and holds every call at a gate while a test keeps
// it closed (close_gate, wait_at_gate, open_gate). The test drives what Stripe would do on its own
// side: complete_checkout (the customer paid), the maps themselves (a status change, a
// renewal), delete_customer. Header-only; thread-safe through `mu` (the sync worker calls
// from its own thread: a test that edits the maps while one runs holds `mu`).
#pragma once

#include "billing/stripe_api.h"
#include "billing/stripe_client.h"

#include <base/io/json.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace billing_fake {

namespace billing = opennova::novaworld_server::billing;
using opennova::io::JsonValue;

class FakeStripeApi : public billing::StripeApi {
public:
	struct Customer {
		std::string id;
		bool deleted = false;
		billing::Metadata metadata;
		std::string email;
		int64_t created = 0;
	};
	struct Item {
		std::string id;
		std::string price_id;
		int64_t quantity = 1;
		int64_t current_period_end = 0;
	};
	struct Subscription {
		std::string id;
		std::string customer;
		std::string status = "active";
		std::vector<Item> items;
		bool cancel_at_period_end = false;
		int64_t cancel_at = 0;
		int64_t created = 0;
		billing::Metadata metadata;
		uint64_t seq = 0; // creation order: lists are newest first
	};
	struct Price {
		std::string id;
		bool active = true;
		std::optional<int64_t> unit_amount = 500;
		std::string currency = "usd";
		std::string type = "recurring";
		std::string interval = "month"; // "" for a one-time price
		int64_t interval_count = 1;
		std::string product = "prod_fake";
	};
	struct CheckoutSession {
		std::string id;
		std::string customer;
		std::string client_reference_id;
		std::string price_id;
		int64_t quantity = 1;
		std::string status = "open";
		std::string subscription;
		std::string success_url;
		std::string cancel_url;
		billing::Metadata metadata;
		billing::Metadata subscription_metadata;
		int64_t expires_at = 0;
		std::vector<std::pair<std::string, std::string>> params; // as Checkout received them
	};
	struct PortalSession {
		std::string id;
		std::string customer;
		std::string return_url;
		std::string url;
		std::string cancel_subscription; // flow_data[subscription_cancel][subscription]: "" for none
	};

	// The fake account's mode and clock (created stamps, Checkout's default expiry).
	bool livemode = false;
	int64_t now = 1760000000;

	std::mutex mu;
	std::map<std::string, Customer> customers;
	std::map<std::string, Subscription> subscriptions;
	std::map<std::string, Price> prices;
	std::map<std::string, CheckoutSession> checkout_sessions;
	std::vector<PortalSession> portal_sessions;
	std::vector<billing::StripeRequest> requests;

	// The next call (after those already queued) answers no reply at all.
	void fail_next(billing::StripeReply::Outcome outcome) {
		std::lock_guard<std::mutex> lock(mu);
		billing::StripeReply reply;
		reply.outcome = outcome;
		if (outcome == billing::StripeReply::Outcome::Unreachable) reply.error = "fake: curl exited with code 7";
		failures_.push_back(reply);
	}
	// The next call (after those already queued) answers `status` with a Stripe error body.
	void fail_next(int status) {
		std::lock_guard<std::mutex> lock(mu);
		failures_.push_back(error_reply(status, status == 429 ? "rate_limit" : "api_error",
				status == 429 ? "rate_limit_error" : "api_error", "fake: failure on request"));
	}

	// The next call to `path` (after the refusals already queued for it, and after any
	// fail_next) answers `status` with Stripe's error body for an invalid request:
	// `code` and `message`, as Stripe refuses a request the account's settings rule out.
	void refuse_next(const std::string &path, int status, const std::string &code,
			const std::string &message) {
		std::lock_guard<std::mutex> lock(mu);
		refusals_[path].push_back(error_reply(status, code, "invalid_request_error", message));
	}

	void add_price(const Price &price) {
		std::lock_guard<std::mutex> lock(mu);
		prices[price.id] = price;
	}

	std::string add_customer(const billing::Metadata &metadata, const std::string &email = {}) {
		std::lock_guard<std::mutex> lock(mu);
		return add_customer_locked(metadata, email);
	}

	// A subscription of one item, as if Checkout (or the dashboard) had made it.
	std::string add_subscription(const std::string &customer, const std::string &price_id, int64_t quantity,
			const std::string &status, int64_t period_end, const billing::Metadata &metadata = {}) {
		std::lock_guard<std::mutex> lock(mu);
		return add_subscription_locked(customer, price_id, quantity, status, period_end, metadata);
	}

	// The customer paid: the session completes and its subscription is made
	// (active, its period ending 30 days on). Returns the subscription's id; ""
	// when the session is not open.
	std::string complete_checkout(const std::string &session_id, const std::string &status = "active") {
		std::lock_guard<std::mutex> lock(mu);
		const auto it = checkout_sessions.find(session_id);
		if (it == checkout_sessions.end() || it->second.status != "open") return {};
		CheckoutSession &session = it->second;
		session.subscription = add_subscription_locked(session.customer, session.price_id, session.quantity, status,
				now + 30 * 86400, session.subscription_metadata);
		session.status = "complete";
		return session.subscription;
	}

	void delete_customer(const std::string &id) {
		std::lock_guard<std::mutex> lock(mu);
		const auto it = customers.find(id);
		if (it == customers.end()) return;
		it->second.deleted = true;
		it->second.metadata.clear();
		// Stripe cancels a deleted customer's subscriptions at once.
		for (auto &[sub_id, sub] : subscriptions) {
			if (sub.customer == id && sub.status != "canceled" && sub.status != "incomplete_expired")
				sub.status = "canceled";
		}
	}

	// A gate every call waits at while it is closed (counted as it arrives): a
	// test holds a route inside its Stripe call, then opens it.
	void close_gate() {
		std::lock_guard<std::mutex> lock(gate_mu_);
		gate_closed_ = true;
	}
	void open_gate() {
		{
			std::lock_guard<std::mutex> lock(gate_mu_);
			gate_closed_ = false;
		}
		gate_cv_.notify_all();
	}
	// Whether `n` calls wait at the closed gate within `timeout`.
	bool wait_at_gate(size_t n, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(gate_mu_);
		return gate_cv_.wait_for(lock, timeout, [&] { return gate_waiting_ >= n; });
	}

	// The requests made so far of `method` whose path is `path`.
	size_t count(billing::StripeRequest::Method method, const std::string &path) {
		std::lock_guard<std::mutex> lock(mu);
		size_t n = 0;
		for (const billing::StripeRequest &r : requests) n += r.method == method && r.path == path ? 1 : 0;
		return n;
	}

	billing::StripeReply call(const billing::StripeRequest &request) override {
		{
			std::unique_lock<std::mutex> gate(gate_mu_);
			if (gate_closed_) {
				++gate_waiting_;
				gate_cv_.notify_all();
				gate_cv_.wait(gate, [this] { return !gate_closed_; });
				--gate_waiting_;
			}
		}
		std::lock_guard<std::mutex> lock(mu);
		requests.push_back(request);
		if (!failures_.empty()) {
			billing::StripeReply reply = failures_.front();
			failures_.pop_front();
			return reply;
		}
		if (const auto refused = refusals_.find(request.path);
				refused != refusals_.end() && !refused->second.empty()) {
			billing::StripeReply reply = refused->second.front();
			refused->second.pop_front();
			return reply;
		}
		const bool post = request.method == billing::StripeRequest::Method::Post;
		if (post && !request.idempotency_key.empty()) {
			const auto seen = replayed_.find(request.idempotency_key);
			if (seen != replayed_.end() && seen->second.first == request.path) return seen->second.second;
		}
		billing::StripeReply reply = route(request);
		if (post && !request.idempotency_key.empty() && reply.status < 500)
			replayed_[request.idempotency_key] = { request.path, reply };
		return reply;
	}

private:
	// --- the replies ------------------------------------------------------------------------

	static billing::StripeReply json_reply(const JsonValue &json, int status = 200) {
		billing::StripeReply reply;
		reply.outcome = billing::StripeReply::Outcome::Answered;
		reply.status = status;
		reply.body = opennova::io::json_write(json);
		return reply;
	}

	static billing::StripeReply error_reply(int status, const std::string &code, const std::string &type,
			const std::string &message) {
		JsonValue error = JsonValue::make_object();
		if (!code.empty()) error.set("code", opennova::io::json_string(code));
		error.set("type", opennova::io::json_string(type));
		error.set("message", opennova::io::json_string(message));
		JsonValue body = JsonValue::make_object();
		body.set("error", error);
		return json_reply(body, status);
	}

	static billing::StripeReply missing(const std::string &what, const std::string &id) {
		return error_reply(404, "resource_missing", "invalid_request_error", "No such " + what + ": '" + id + "'");
	}

	static billing::StripeReply invalid(const std::string &message) {
		return error_reply(400, "parameter_invalid", "invalid_request_error", message);
	}

	static JsonValue text_or_null(const std::string &text) {
		return text.empty() ? JsonValue::make_null() : opennova::io::json_string(text);
	}

	static JsonValue number(int64_t value) { return opennova::io::json_number(static_cast<double>(value)); }

	static JsonValue metadata_json(const billing::Metadata &metadata) {
		JsonValue out = JsonValue::make_object();
		for (const auto &[key, value] : metadata) out.set(key, opennova::io::json_string(value));
		return out;
	}

	JsonValue customer_json(const Customer &c) const {
		JsonValue out = JsonValue::make_object();
		out.set("id", opennova::io::json_string(c.id));
		out.set("object", opennova::io::json_string("customer"));
		if (c.deleted) {
			out.set("deleted", JsonValue::make_bool(true));
			return out;
		}
		out.set("livemode", JsonValue::make_bool(livemode));
		out.set("created", number(c.created));
		out.set("email", text_or_null(c.email));
		out.set("metadata", metadata_json(c.metadata));
		return out;
	}

	JsonValue price_json(const Price &p) const {
		JsonValue out = JsonValue::make_object();
		out.set("id", opennova::io::json_string(p.id));
		out.set("object", opennova::io::json_string("price"));
		out.set("active", JsonValue::make_bool(p.active));
		out.set("livemode", JsonValue::make_bool(livemode));
		out.set("currency", opennova::io::json_string(p.currency));
		out.set("type", opennova::io::json_string(p.type));
		out.set("unit_amount", p.unit_amount ? number(*p.unit_amount) : JsonValue::make_null());
		out.set("product", opennova::io::json_string(p.product));
		if (p.interval.empty()) {
			out.set("recurring", JsonValue::make_null());
		} else {
			JsonValue recurring = JsonValue::make_object();
			recurring.set("interval", opennova::io::json_string(p.interval));
			recurring.set("interval_count", number(p.interval_count));
			recurring.set("usage_type", opennova::io::json_string("licensed"));
			out.set("recurring", recurring);
		}
		return out;
	}

	JsonValue subscription_json(const Subscription &s, bool expand_customer) const {
		JsonValue out = JsonValue::make_object();
		out.set("id", opennova::io::json_string(s.id));
		out.set("object", opennova::io::json_string("subscription"));
		const auto customer = customers.find(s.customer);
		if (expand_customer && customer != customers.end())
			out.set("customer", customer_json(customer->second));
		else
			out.set("customer", opennova::io::json_string(s.customer));
		out.set("status", opennova::io::json_string(s.status));
		out.set("livemode", JsonValue::make_bool(livemode));
		out.set("cancel_at_period_end", JsonValue::make_bool(s.cancel_at_period_end));
		out.set("cancel_at", s.cancel_at != 0 ? number(s.cancel_at) : JsonValue::make_null());
		out.set("created", number(s.created));
		out.set("metadata", metadata_json(s.metadata));
		JsonValue data = JsonValue::make_array();
		for (const Item &item : s.items) {
			JsonValue i = JsonValue::make_object();
			i.set("id", opennova::io::json_string(item.id));
			i.set("object", opennova::io::json_string("subscription_item"));
			const auto price = prices.find(item.price_id);
			Price shown;
			shown.id = item.price_id;
			i.set("price", price_json(price != prices.end() ? price->second : shown));
			i.set("quantity", number(item.quantity));
			i.set("current_period_start", number(item.current_period_end - 30 * 86400));
			i.set("current_period_end", number(item.current_period_end));
			data.push(i);
		}
		JsonValue items = JsonValue::make_object();
		items.set("object", opennova::io::json_string("list"));
		items.set("data", data);
		items.set("has_more", JsonValue::make_bool(false));
		out.set("items", items);
		return out;
	}

	JsonValue checkout_json(const CheckoutSession &c) const {
		JsonValue out = JsonValue::make_object();
		out.set("id", opennova::io::json_string(c.id));
		out.set("object", opennova::io::json_string("checkout.session"));
		out.set("url", c.status == "open" ? opennova::io::json_string("https://checkout.stripe.com/c/pay/" + c.id)
										  : JsonValue::make_null());
		out.set("status", opennova::io::json_string(c.status));
		out.set("customer", opennova::io::json_string(c.customer));
		out.set("subscription", text_or_null(c.subscription));
		out.set("client_reference_id", text_or_null(c.client_reference_id));
		out.set("mode", opennova::io::json_string("subscription"));
		out.set("payment_status", opennova::io::json_string(c.status == "complete" ? "paid" : "unpaid"));
		out.set("metadata", metadata_json(c.metadata));
		out.set("expires_at", number(c.expires_at));
		out.set("livemode", JsonValue::make_bool(livemode));
		return out;
	}

	// --- the state --------------------------------------------------------------------------

	std::string next_id(const char *prefix) {
		char digits[16];
		std::snprintf(digits, sizeof(digits), "%06llu", static_cast<unsigned long long>(++seq_));
		return std::string(prefix) + "fake" + digits;
	}

	std::string add_customer_locked(const billing::Metadata &metadata, const std::string &email) {
		Customer c;
		c.id = next_id("cus_");
		c.metadata = metadata;
		c.email = email;
		c.created = now;
		customers[c.id] = c;
		return c.id;
	}

	std::string add_subscription_locked(const std::string &customer, const std::string &price_id, int64_t quantity,
			const std::string &status, int64_t period_end, const billing::Metadata &metadata) {
		Subscription s;
		s.id = next_id("sub_");
		s.customer = customer;
		s.status = status;
		s.created = now;
		s.metadata = metadata;
		s.seq = seq_;
		Item item;
		item.id = next_id("si_");
		item.price_id = price_id;
		item.quantity = quantity;
		item.current_period_end = period_end;
		s.items.push_back(item);
		subscriptions[s.id] = s;
		return s.id;
	}

	// --- the routes -------------------------------------------------------------------------

	// A decimal field's value; -1 when it is no whole number (which every check refuses).
	static int64_t whole(const std::string &text) {
		if (text.empty() || text.size() > 18 || text.find_first_not_of("0123456789") != std::string::npos) return -1;
		return std::strtoll(text.c_str(), nullptr, 10);
	}

	static std::optional<std::string> param(const billing::StripeRequest &r, const std::string &name) {
		for (const auto &[key, value] : r.params) {
			if (key == name) return value;
		}
		return std::nullopt;
	}

	static bool has_param(const billing::StripeRequest &r, const std::string &name, const std::string &value) {
		for (const auto &[key, v] : r.params) {
			if (key == name && v == value) return true;
		}
		return false;
	}

	// The fields `prefix`[key] as a key -> value map.
	static billing::Metadata nested(const billing::StripeRequest &r, const std::string &prefix) {
		billing::Metadata out;
		for (const auto &[key, value] : r.params) {
			if (key.size() > prefix.size() + 2 && key.compare(0, prefix.size() + 1, prefix + "[") == 0 &&
					key.back() == ']' && key.find('[', prefix.size() + 1) == std::string::npos)
				out[key.substr(prefix.size() + 1, key.size() - prefix.size() - 2)] = value;
		}
		return out;
	}

	// Stripe's metadata update: each key set, an empty value deleting it.
	static void merge_metadata(billing::Metadata &into, const billing::Metadata &update) {
		for (const auto &[key, value] : update) {
			if (value.empty())
				into.erase(key);
			else
				into[key] = value;
		}
	}

	static std::vector<std::string> split_path(const std::string &path) {
		std::vector<std::string> out;
		size_t pos = 1;
		while (pos <= path.size()) {
			const size_t slash = path.find('/', pos);
			const size_t end = slash == std::string::npos ? path.size() : slash;
			out.push_back(path.substr(pos, end - pos));
			pos = end + 1;
		}
		return out;
	}

	billing::StripeReply route(const billing::StripeRequest &r) {
		const bool post = r.method == billing::StripeRequest::Method::Post;
		const std::vector<std::string> p = split_path(r.path);
		if (p.size() < 2 || p[0] != "v1") return error_reply(404, "", "invalid_request_error", "Unrecognized request URL");
		const std::string &resource = p[1];
		if (resource == "customers") {
			if (p.size() == 2 && post) return create_customer(r);
			if (p.size() == 3) return post ? update_customer(r, p[2]) : get_customer(p[2]);
		} else if (resource == "checkout" && p.size() >= 3 && p[2] == "sessions") {
			if (p.size() == 3 && post) return create_checkout(r);
			if (p.size() == 4 && !post) {
				const auto it = checkout_sessions.find(p[3]);
				return it == checkout_sessions.end() ? missing("checkout.session", p[3]) : json_reply(checkout_json(it->second));
			}
			if (p.size() == 5 && post && p[4] == "expire") return expire_checkout(p[3]);
		} else if (resource == "billing_portal" && p.size() == 3 && p[2] == "sessions" && post) {
			return create_portal(r);
		} else if (resource == "subscriptions") {
			if (p.size() == 2 && !post) return list_subscriptions(r);
			if (p.size() == 3) return post ? update_subscription(r, p[2]) : get_subscription(r, p[2]);
		} else if (resource == "prices" && p.size() == 3 && !post) {
			const auto it = prices.find(p[2]);
			return it == prices.end() ? missing("price", p[2]) : json_reply(price_json(it->second));
		}
		return error_reply(404, "", "invalid_request_error", "Unrecognized request URL (" + r.path + ")");
	}

	billing::StripeReply create_customer(const billing::StripeRequest &r) {
		billing::Metadata metadata;
		merge_metadata(metadata, nested(r, "metadata"));
		const std::string id = add_customer_locked(metadata, param(r, "email").value_or(""));
		return json_reply(customer_json(customers[id]));
	}

	billing::StripeReply get_customer(const std::string &id) {
		const auto it = customers.find(id);
		return it == customers.end() ? missing("customer", id) : json_reply(customer_json(it->second));
	}

	billing::StripeReply update_customer(const billing::StripeRequest &r, const std::string &id) {
		const auto it = customers.find(id);
		if (it == customers.end() || it->second.deleted) return missing("customer", id);
		merge_metadata(it->second.metadata, nested(r, "metadata"));
		return json_reply(customer_json(it->second));
	}

	billing::StripeReply create_checkout(const billing::StripeRequest &r) {
		if (!has_param(r, "mode", "subscription")) return invalid("fake: mode must be subscription");
		CheckoutSession c;
		c.customer = param(r, "customer").value_or("");
		const auto customer = customers.find(c.customer);
		if (customer == customers.end() || customer->second.deleted) return missing("customer", c.customer);
		c.price_id = param(r, "line_items[0][price]").value_or("");
		const auto price = prices.find(c.price_id);
		if (price == prices.end()) return missing("price", c.price_id);
		if (!price->second.active) return invalid("The price specified is inactive.");
		c.quantity = whole(param(r, "line_items[0][quantity]").value_or("1"));
		if (c.quantity < 1) return invalid("fake: the quantity must be 1 or more");
		c.success_url = param(r, "success_url").value_or("");
		c.cancel_url = param(r, "cancel_url").value_or("");
		if (c.success_url.empty() || c.cancel_url.empty()) return invalid("fake: success_url and cancel_url are required");
		c.client_reference_id = param(r, "client_reference_id").value_or("");
		c.metadata = nested(r, "metadata");
		c.subscription_metadata = nested(r, "subscription_data[metadata]");
		const std::string expires = param(r, "expires_at").value_or("");
		c.expires_at = expires.empty() ? now + 86400 : whole(expires);
		c.params = r.params;
		c.id = next_id("cs_test_");
		checkout_sessions[c.id] = c;
		return json_reply(checkout_json(c));
	}

	billing::StripeReply expire_checkout(const std::string &id) {
		const auto it = checkout_sessions.find(id);
		if (it == checkout_sessions.end()) return missing("checkout.session", id);
		if (it->second.status != "open")
			return invalid("Only Checkout Sessions with a status in [\"open\"] can be expired.");
		it->second.status = "expired";
		return json_reply(checkout_json(it->second));
	}

	billing::StripeReply create_portal(const billing::StripeRequest &r) {
		PortalSession s;
		s.customer = param(r, "customer").value_or("");
		const auto customer = customers.find(s.customer);
		if (customer == customers.end() || customer->second.deleted) return missing("customer", s.customer);
		s.return_url = param(r, "return_url").value_or("");
		if (param(r, "flow_data[type]").value_or("") == "subscription_cancel") {
			s.cancel_subscription = param(r, "flow_data[subscription_cancel][subscription]").value_or("");
			const auto sub = subscriptions.find(s.cancel_subscription);
			if (sub == subscriptions.end() || sub->second.customer != s.customer)
				return missing("subscription", s.cancel_subscription);
		}
		s.id = next_id("bps_");
		s.url = "https://billing.stripe.com/p/session/" + s.id;
		portal_sessions.push_back(s);
		JsonValue out = JsonValue::make_object();
		out.set("id", opennova::io::json_string(s.id));
		out.set("object", opennova::io::json_string("billing_portal.session"));
		out.set("customer", opennova::io::json_string(s.customer));
		out.set("return_url", text_or_null(s.return_url));
		out.set("url", opennova::io::json_string(s.url));
		out.set("livemode", JsonValue::make_bool(livemode));
		return json_reply(out);
	}

	billing::StripeReply get_subscription(const billing::StripeRequest &r, const std::string &id) {
		const auto it = subscriptions.find(id);
		if (it == subscriptions.end()) return missing("subscription", id);
		return json_reply(subscription_json(it->second, has_param(r, "expand[]", "customer")));
	}

	billing::StripeReply update_subscription(const billing::StripeRequest &r, const std::string &id) {
		const auto it = subscriptions.find(id);
		if (it == subscriptions.end()) return missing("subscription", id);
		merge_metadata(it->second.metadata, nested(r, "metadata"));
		return json_reply(subscription_json(it->second, false));
	}

	billing::StripeReply list_subscriptions(const billing::StripeRequest &r) {
		const std::string customer = param(r, "customer").value_or("");
		const std::optional<std::string> status = param(r, "status");
		const std::string after = param(r, "starting_after").value_or("");
		const int64_t limit = whole(param(r, "limit").value_or("10"));
		if (limit < 1 || limit > 100) return invalid("fake: limit must be 1 to 100");
		std::vector<const Subscription *> matching;
		for (const auto &[id, sub] : subscriptions) {
			if (!customer.empty() && sub.customer != customer) continue;
			if (!status) {
				if (sub.status == "canceled") continue;
			} else if (*status != "all" && sub.status != *status) {
				continue;
			}
			matching.push_back(&sub);
		}
		std::sort(matching.begin(), matching.end(),
				[](const Subscription *a, const Subscription *b) { return a->seq > b->seq; });
		size_t start = 0;
		if (!after.empty()) {
			const auto cursor = subscriptions.find(after);
			if (cursor == subscriptions.end()) return missing("subscription", after);
			while (start < matching.size() && matching[start]->seq >= cursor->second.seq) ++start;
		}
		const bool expand = has_param(r, "expand[]", "data.customer");
		JsonValue data = JsonValue::make_array();
		size_t i = start;
		for (; i < matching.size() && i < start + static_cast<size_t>(limit); ++i)
			data.push(subscription_json(*matching[i], expand));
		JsonValue out = JsonValue::make_object();
		out.set("object", opennova::io::json_string("list"));
		out.set("url", opennova::io::json_string("/v1/subscriptions"));
		out.set("data", data);
		out.set("has_more", JsonValue::make_bool(i < matching.size()));
		return json_reply(out);
	}

	uint64_t seq_ = 0;
	std::mutex gate_mu_;
	std::condition_variable gate_cv_;
	bool gate_closed_ = false;
	size_t gate_waiting_ = 0;
	std::deque<billing::StripeReply> failures_;
	std::map<std::string, std::deque<billing::StripeReply>> refusals_; // by path
	std::map<std::string, std::pair<std::string, billing::StripeReply>> replayed_;
};

} // namespace billing_fake
