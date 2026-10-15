#pragma once

// A Stripe webhook delivery's checks: its Stripe-Signature header against the
// endpoint's signing secret, and the few stable fields of its event the
// webhook route reads before it hands the customer to the sync worker. The
// payload is never trusted beyond that: the worker reads the customer and its
// subscriptions back from Stripe, so an event's order and its body's details do
// not matter. Crow-free. Service policy over Stripe's documented scheme, not a
// port.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::billing {

enum class SignatureCheck {
	Ok,        // a v1 signature matches a secret, and its time is within the tolerance
	Missing,   // no header (or a blank one)
	Malformed, // not "t=<seconds>,v1=<64 hex>,...": no t, two t, a t that is no number, a v1 that is not 64 hex, no v1
	Stale,     // a v1 matches, but t is more than the tolerance away from now
	NoMatch,   // no v1 matches any secret
};

// Checks `header` (Stripe-Signature: comma-separated key=value items) over the
// raw request body `payload`. The expected signature is the lower-case hex
// HMAC-SHA256 of "<t>.<payload>" keyed with the whole signing secret
// ("whsec_..." as it is, as Stripe's libraries key it); the delivery passes when
// any v1 item matches any of `secrets` (compared in constant time), so a roll,
// which signs with the old and the new secret at once, needs no second
// setting. v0 and unknown items are ignored. A match whose t is more than
// `tolerance_s` from `now_unix` either way is Stale; a signature is checked
// before the time, so an unsigned delivery never learns the clock. `skew_out`
// (when given) receives now - t whenever t was read.
SignatureCheck verify_stripe_signature(std::string_view header, std::string_view payload,
		const std::vector<std::string> &secrets, int64_t now_unix, int64_t tolerance_s = 300,
		int64_t *skew_out = nullptr);

// The check's name for a log line or a counter: "ok", "missing", ...
const char *signature_check_name(SignatureCheck check);

// An event's stable fields (the envelope every API version keeps).
struct WebhookEnvelope {
	std::string id;   // evt_...
	std::string type; // "customer.subscription.updated"
	bool livemode = false;
	int64_t created = 0; // Unix seconds
	std::string object_type; // data.object.object: "subscription", "customer", "checkout.session", ...
	std::string object_id;   // data.object.id
	// The customer the event is about: data.object.id for a customer, else
	// data.object.customer (its id when expanded); "" when it names none.
	std::string customer_id;
};

// The envelope of the event JSON `body`; nullopt unless it is an object of
// "object":"event" with a non-empty string id and type, a boolean livemode, a
// whole created and a data.object object. data.object's object, id and
// customer are read when they are there ("" when not), so an event of a kind
// the service ignores is still recorded and answered.
std::optional<WebhookEnvelope> parse_webhook_envelope(std::string_view body);

} // namespace opennova::novaworld_server::billing
