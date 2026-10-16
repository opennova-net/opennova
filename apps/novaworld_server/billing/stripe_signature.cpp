#include "stripe_signature.h"

#include <base/io/json.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>

namespace opennova::novaworld_server::billing {

namespace {

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// The string member `key` of `object`; "" when absent or not a string.
std::string text_of(const io::JsonValue &object, const char *key) {
	const io::JsonValue *v = object.get(key);
	return v != nullptr && v->is_string() ? v->string : std::string();
}

} // namespace

SignatureCheck verify_stripe_signature(std::string_view header, std::string_view payload,
		const std::vector<std::string> &secrets, int64_t now_unix, int64_t tolerance_s, int64_t *skew_out) {
	header = strutil::trim_view(header);
	if (header.empty()) return SignatureCheck::Missing;

	std::string_view t_text;
	bool seen_t = false;
	std::vector<std::string> signatures;
	size_t pos = 0;
	while (pos <= header.size()) {
		const size_t comma = header.find(',', pos);
		const size_t end = comma == std::string_view::npos ? header.size() : comma;
		const std::string_view item = strutil::trim_view(header.substr(pos, end - pos));
		pos = end + 1;
		if (item.empty()) continue;
		const size_t eq = item.find('=');
		if (eq == std::string_view::npos) return SignatureCheck::Malformed;
		const std::string_view key = item.substr(0, eq);
		const std::string_view value = item.substr(eq + 1);
		if (key == "t") {
			if (seen_t || value.empty() || value.size() > 18 || !strutil::all_digits(value))
				return SignatureCheck::Malformed;
			seen_t = true;
			t_text = value;
		} else if (key == "v1") {
			if (value.size() != 64) return SignatureCheck::Malformed;
			for (const char c : value) {
				if (!is_hex(c)) return SignatureCheck::Malformed;
			}
			signatures.push_back(strutil::to_lower(value));
		}
		// v0 (Stripe's test-only scheme) and any other key: ignored.
	}
	if (!seen_t || signatures.empty()) return SignatureCheck::Malformed;
	const int64_t t = static_cast<int64_t>(strutil::parse_llong(std::string(t_text)).value_or(0));
	const int64_t skew = now_unix - t;
	if (skew_out != nullptr) *skew_out = skew;

	std::string signed_text;
	signed_text.reserve(t_text.size() + 1 + payload.size());
	signed_text.append(t_text.data(), t_text.size());
	signed_text.push_back('.');
	signed_text.append(payload.data(), payload.size());
	bool matched = false;
	for (const std::string &secret : secrets) {
		if (secret.empty()) continue;
		const std::string expected = io::sha256_hex(io::hmac_sha256(secret, signed_text));
		// Every pair is compared, a match or not, so the time says nothing of which matched.
		for (const std::string &signature : signatures)
			matched = io::constant_time_equal(expected, signature) || matched;
	}
	if (!matched) return SignatureCheck::NoMatch;
	if (skew > tolerance_s || skew < -tolerance_s) return SignatureCheck::Stale;
	return SignatureCheck::Ok;
}

const char *signature_check_name(SignatureCheck check) {
	switch (check) {
		case SignatureCheck::Ok: return "ok";
		case SignatureCheck::Missing: return "missing";
		case SignatureCheck::Malformed: return "malformed";
		case SignatureCheck::Stale: return "stale";
		case SignatureCheck::NoMatch: return "no_match";
	}
	return "unknown";
}

std::optional<WebhookEnvelope> parse_webhook_envelope(std::string_view body) {
	io::JsonValue root;
	std::string error;
	if (!io::json_parse(body, root, error) || !root.is_object()) return std::nullopt;
	if (text_of(root, "object") != "event") return std::nullopt;
	WebhookEnvelope out;
	out.id = text_of(root, "id");
	out.type = text_of(root, "type");
	const io::JsonValue *livemode = root.get("livemode");
	const io::JsonValue *created = root.get("created");
	const io::JsonValue *data = root.get("data");
	const io::JsonValue *object = data != nullptr ? data->get("object") : nullptr;
	if (out.id.empty() || out.type.empty() || livemode == nullptr || !livemode->is_bool() || created == nullptr ||
			!io::json_exact_whole_in(*created, 0, io::kJsonSafeWholeMax, out.created) || object == nullptr ||
			!object->is_object())
		return std::nullopt;
	out.livemode = livemode->boolean;
	out.object_type = text_of(*object, "object");
	out.object_id = text_of(*object, "id");
	if (out.object_type == "customer") {
		out.customer_id = out.object_id;
	} else if (const io::JsonValue *customer = object->get("customer")) {
		// An id, or the customer itself when the sender expanded it.
		out.customer_id = customer->is_object() ? text_of(*customer, "id") : text_of(*object, "customer");
	}
	return out;
}

} // namespace opennova::novaworld_server::billing
