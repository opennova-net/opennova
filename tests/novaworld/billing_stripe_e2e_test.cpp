// The support tiers end to end against Stripe's TEST mode: the service in-process
// (novaworld/nw_service_harness.h) with billing over the real transport
// (CurlStripeApi, curl to api.stripe.com), driven through its routes the way the
// site's support page drives them:
//
//   1. GET /api/billing/tiers: the prices read back from Stripe.
//   2. POST /api/billing/checkout {"item": "tier1"}: a real Checkout session, the
//      settings the server sends (the terms of service consent among them) accepted
//      by Stripe. Prod's first checkouts failed here (2026-10-16): Stripe refused the
//      session and the site showed Cloudflare's 502.
//   3. A second checkout: the first session expired in Stripe, the customer reused.
//   4. The payment: Stripe Checkout's hosted page takes no API call, so the test makes
//      the subscription Checkout would make (the tier's price on the account's
//      customer, the instance's metadata), billed by invoice so no card is needed,
//      set to cancel itself ten minutes later.
//   5. Stripe's webhook, as Stripe sends it: a customer.subscription.created event
//      signed with the endpoint's secret (v1, HMAC-SHA256 of "<t>.<body>") to POST
//      /api/billing/webhook; the worker's pass reads the customer back from Stripe.
//   6. The hosting entitlement GET /api/me/billing shows: tier 1, one server; a second
//      tier's checkout refused (already_subscribed); the Customer Portal opens.
//
// Gated twice. ONNET_STRIPE_TEST_SECRET_KEY names a Stripe TEST-mode key (sk_test_ or
// rk_test_; a live key fails the test, never runs it): with none (unset, empty, or the
// vault's REPLACE_ME, which CI's secret holds until it is filled) the test prints why and
// exits 77 (Skipped) on a line the core suite accepts, so the suite passes on a machine
// without the key. The prices are the test account's found by the lookup keys `stripe
// setup` gives them (DEPLOY.md, "Monthly support tiers (Stripe)"). The database is
// OPENNOVA_TEST_PG's, as for every pg test. Locally, from the vault and never on disk:
//   op run --env-file=deploy/env/billing.e2e.env.tpl -- \
//     ctest --test-dir build -C Release -R '^billing_stripe_e2e$' --output-on-failure
// It leaves nothing in the test account: its last Checkout session is expired and its
// customer deleted at the end, which cancels the subscription (set to cancel itself
// anyway, should the test die first).

#include "auth.h"
#include "billing/billing.h"
#include "billing/billing_config.h"
#include "billing/billing_repository.h"
#include "billing/entitlement.h"
#include "billing/stripe_api.h"
#include "billing/stripe_client.h"
#include "command_runner.h"
#include "hosted/owner_file.h"
#include "net_http.h"
#include "net_sockets.h"
#include "web_session.h"

#include "novaworld/nw_service_harness.h"

#include <base/io/json.h>
#include <base/io/sha256.h>
#include <base/os_random/os_random.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace billing = opennova::novaworld_server::billing;
using opennova::io::JsonValue;

namespace {

constexpr const char *kJson = "Content-Type: application/json";
constexpr const char *kCsrf = "X-OpenNova-Request: 1";
// Where Checkout and the Portal return (never visited here).
constexpr const char *kSiteUrl = "https://example.com";
// Each route that calls Stripe makes a few calls of up to 20 s each.
constexpr int kRouteTimeoutMs = 90000;
// How long the subscription the test makes lives before it cancels itself.
constexpr int64_t kSubscriptionLifeS = 10 * 60;
// The lookup keys `stripe setup` gives the catalog's prices (deploy/bin/on-deploy).
constexpr const char *kLookupKeys[] = { "opennova_tier1_monthly", "opennova_tier2_monthly",
	"opennova_tier3_monthly", "opennova_own_machine_monthly" };

std::string random_hex(size_t bytes) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::vector<uint8_t> raw(bytes);
	opennova::os_random_bytes(raw.data(), raw.size());
	std::string out;
	for (const uint8_t b : raw) {
		out.push_back(kDigits[b >> 4]);
		out.push_back(kDigits[b & 0xFu]);
	}
	return out;
}

bool starts_with(const std::string &text, const std::string &head) {
	return text.rfind(head, 0) == 0;
}

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

std::string describe(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "no reply (" + reply.error + ")";
	return "HTTP " + std::to_string(reply.code) + " " + body_text(reply);
}

std::optional<JsonValue> parse(const std::string &text) {
	JsonValue json;
	std::string error;
	if (!opennova::io::json_parse(text, json, error))
		return std::nullopt;
	return json;
}

// A route's JSON body; an empty object when it is none.
JsonValue reply_json(const net::HttpReply &reply) {
	return parse(body_text(reply)).value_or(JsonValue::make_object());
}

// The site's requests: the account's session cookie, the CSRF header on a POST.
struct Site {
	uint16_t port = 0;
	std::string session;

	net::HttpReply send(const char *method, const std::string &path, const std::string &body = {},
			std::vector<std::string> headers = {}) const {
		if (!session.empty())
			headers.push_back(std::string("Cookie: ") + nws::kWebSessionCookie + "=" + session);
		if (std::string(method) == "POST") {
			headers.push_back(kJson);
			headers.push_back(kCsrf);
		}
		const net::HttpReply reply = net::http_exchange(method,
				"http://127.0.0.1:" + std::to_string(port) + path, headers, body, kRouteTimeoutMs,
				[](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
		std::printf("  %s %s -> %s\n", method, path.c_str(),
				reply.transport_ok ? std::to_string(reply.code).c_str() : reply.error.c_str());
		return reply;
	}
};

// The test's own calls to Stripe (the prices by lookup key, the subscription Checkout
// would make): the JSON of a 2xx, else nullopt with Stripe's error printed.
std::optional<JsonValue> stripe(billing::StripeApi &api, billing::StripeRequest::Method method,
		const std::string &path, std::vector<std::pair<std::string, std::string>> params = {}) {
	billing::StripeRequest request;
	request.method = method;
	request.path = path;
	request.params = std::move(params);
	if (method == billing::StripeRequest::Method::Post)
		request.idempotency_key = "opennova-e2e-" + random_hex(16);
	const billing::StripeReply reply = api.call(request);
	if (!reply.ok()) {
		using Method = billing::StripeRequest::Method;
		const char *verb = "GET";
		if (method == Method::Post)
			verb = "POST";
		if (method == Method::Delete)
			verb = "DELETE";
		std::fprintf(stderr, "  Stripe %s %s: %s\n", verb, path.c_str(),
				reply.outcome == billing::StripeReply::Outcome::Answered
						? ("HTTP " + std::to_string(reply.status) + " " + reply.body.substr(0, 400))
								  .c_str()
						: reply.error.c_str());
		return std::nullopt;
	}
	return parse(reply.body);
}

// A Stripe-Signature header for `body` under `secret` now, as Stripe signs a delivery.
std::string stripe_signature(const std::string &body, const std::string &secret) {
	const std::string t = std::to_string(billing::unix_now());
	return "Stripe-Signature: t=" + t +
			",v1=" + opennova::io::sha256_hex(opennova::io::hmac_sha256(secret, t + "." + body));
}

int run(const std::string &secret_key) {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	test_temp::TempDir temp("billing_stripe_e2e");
	const std::filesystem::path scratch = temp.path / "stripe";
	std::string error;
	TEST_EXPECT(opennova::novaworld_server::hosted::make_owner_directory(scratch, error));

	// The transport as main() makes it (make_billing): curl through the runner, the key
	// in an owner-only config file per call.
	billing::CurlStripeApi::Config transport;
	transport.curl = billing::BillingConfig{}.curl_path;
	transport.secret_key = secret_key;
	transport.scratch_dir = scratch.u8string();
	TEST_EXPECT(billing::CurlStripeApi::check_config(transport).empty());
	opennova::proc::ProcessCommandRunner runner(billing::curl_environment(), transport.scratch_dir);
	billing::CurlStripeApi api(runner, transport);

	// The test account's catalog, by lookup key.
	billing::BillingConfig config;
	config.secret_key = secret_key;
	// The webhook endpoint's secret is the test's own: it signs the deliveries below.
	config.webhook_secret = "whsec_" + random_hex(16);
	config.site_url = kSiteUrl;
	std::vector<std::string> prices;
	for (const char *lookup : kLookupKeys) {
		const auto listed = stripe(api, billing::StripeRequest::Method::Get, "/v1/prices",
				{ { "lookup_keys[]", lookup }, { "active", "true" } });
		const JsonValue *data = listed ? listed->get("data") : nullptr;
		if (data == nullptr || !data->is_array() || data->array.empty()) {
			std::fprintf(stderr,
					"  no active price has the lookup key %s in this Stripe test account "
					"(./deploy/run.sh stripe setup --mode test makes the catalog)\n",
					lookup);
			return 1;
		}
		prices.push_back(data->array[0].get_string("id", ""));
	}
	for (int tier = 1; tier <= billing::kTierCount; ++tier)
		config.tier_prices[tier] = { prices[tier - 1] };
	config.own_machine_prices = { prices[3] };
	const std::string why = billing::check_billing_config(config);
	if (!why.empty())
		std::fprintf(stderr, "  the settings: %s\n", why.c_str());
	TEST_EXPECT(why.empty() && !config.live());

	nw_service_test::Service service;
	billing::Billing billing_service(
			config, api, service.pool, billing::instance_id(*service.pool.acquire()));
	// The first reconcile at a start, by hand (no worker): the prices checked.
	billing_service.sync().reconcile_all(billing::unix_now());
	const billing::SyncStatus first = billing_service.sync().status(*service.pool.acquire());
	if (!first.prices_ok)
		std::fprintf(stderr, "  the prices: %s\n", first.prices_error.c_str());
	TEST_EXPECT(first.prices_ok);
	service.http.set_billing(&billing_service);
	TEST_EXPECT(service.start(source_dir, temp.path, /*require_host_key=*/false));
	const int64_t user = service.owner_id;
	Site site;
	site.port = service.http.bound_port();
	{
		auto conn = service.pool.acquire();
		const auto account = nws::get_user_by_id(*conn, user);
		TEST_EXPECT(account.has_value());
		const auto token = nws::create_web_session(
				*conn, user, account->credentials_gen, "127.0.0.1", "billing-e2e");
		TEST_EXPECT(token.has_value());
		site.session = *token;
	}
	int rc = 0;
	// What the test made in Stripe, undone at the end: the open session expired, the
	// customer deleted.
	std::string open_session;
	std::string customer_id;
	[&] {
		// 1. The tiers, priced by Stripe.
		auto reply = site.send("GET", "/api/billing/tiers");
		JsonValue json = reply_json(reply);
		const JsonValue *tiers = json.get("tiers");
		if (!(reply.code == 200 && json.get_bool("enabled", false) &&
					json.get_string("currency", "") == "usd" && tiers != nullptr &&
					tiers->is_array() && tiers->array.size() == 3 &&
					tiers->array[0].get_number("amount", -1) > 0)) {
			std::fprintf(stderr, "  tiers: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}

		// 2. A Checkout session for tier 1, as the support page starts one. A refusal
		// here prints the server's "[billing] ... checkout -> no session: <Stripe's
		// reason>" line above.
		reply = site.send("POST", "/api/billing/checkout", "{\"item\":\"tier1\"}");
		json = reply_json(reply);
		if (reply.code != 200 ||
				!starts_with(json.get_string("url", ""), "https://checkout.stripe.com/")) {
			std::fprintf(stderr, "  checkout: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}
		auto customer = billing::customer_of(*service.pool.acquire(), user);
		if (customer)
			customer_id = customer->stripe_customer_id;
		if (!customer || customer->livemode || customer->open_checkout_session.empty()) {
			std::fprintf(stderr, "  checkout: no test-mode customer row with its open session\n");
			rc = 1;
			return;
		}
		const std::string first_session = customer->open_checkout_session;
		const auto session = stripe(
				api, billing::StripeRequest::Method::Get, "/v1/checkout/sessions/" + first_session);
		const JsonValue *consent = session ? session->get("consent_collection") : nullptr;
		const JsonValue *metadata = session ? session->get("metadata") : nullptr;
		if (!session || session->get_string("status", "") != "open" ||
				session->get_string("mode", "") != "subscription" ||
				session->get_bool("livemode", true) ||
				session->get_string("customer", "") != customer->stripe_customer_id ||
				consent == nullptr || consent->get_string("terms_of_service", "") != "required" ||
				metadata == nullptr ||
				metadata->get_string(billing::kMetaInstance, "") != billing_service.instance_id()) {
			std::fprintf(stderr, "  the session in Stripe is not the one the server asked for\n");
			rc = 1;
			return;
		}

		// 3. Again: the first session expired in Stripe, the same customer.
		reply = site.send("POST", "/api/billing/checkout", "{\"item\":\"tier1\"}");
		customer = billing::customer_of(*service.pool.acquire(), user);
		const auto expired = billing_service.client().get_checkout_session(first_session);
		if (reply.code != 200 || !customer || customer->open_checkout_session.empty() ||
				customer->open_checkout_session == first_session || !expired.ok() ||
				expired.value->status != "expired") {
			std::fprintf(stderr, "  second checkout: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}
		open_session = customer->open_checkout_session;
		const std::string cus = customer->stripe_customer_id;

		// 4. The subscription Checkout would make once paid: the tier's price on the
		// customer, the instance's metadata; billed by invoice (no card), so it is
		// active at once, and set to cancel itself.
		const auto emailed = stripe(api, billing::StripeRequest::Method::Post,
				"/v1/customers/" + cus, { { "email", "billing-e2e@example.com" } });
		const auto made = stripe(api, billing::StripeRequest::Method::Post, "/v1/subscriptions",
				{ { "customer", cus }, { "items[0][price]", prices[0] },
						{ "items[0][quantity]", "1" }, { "collection_method", "send_invoice" },
						{ "days_until_due", "7" },
						{ "cancel_at", std::to_string(billing::unix_now() + kSubscriptionLifeS) },
						{ std::string("metadata[") + billing::kMetaInstance + "]",
								billing_service.instance_id() },
						{ std::string("metadata[") + billing::kMetaUserId + "]",
								std::to_string(user) },
						{ std::string("metadata[") + billing::kMetaUsername + "]",
								nw_service_test::kOwner } });
		const std::string sub = made ? made->get_string("id", "") : std::string();
		if (!emailed || !made || sub.empty() || made->get_string("status", "") != "active") {
			std::fprintf(stderr, "  the subscription: %s\n",
					made ? made->get_string("status", "?").c_str() : "not made");
			rc = 1;
			return;
		}

		// 5. Stripe's delivery: a bad signature refused, the signed one recorded; the
		// worker's pass reads the customer back.
		const std::string event = "{\"id\":\"evt_e2e" + random_hex(12) +
				"\",\"object\":\"event\",\"type\":\"customer.subscription.created\",\"livemode\":"
				"false,"
				"\"api_version\":\"" +
				billing::kStripeApiVersion +
				"\",\"created\":" + std::to_string(billing::unix_now()) +
				",\"data\":{\"object\":{\"id\":\"" + sub +
				"\",\"object\":\"subscription\",\"customer\":\"" + cus + "\"}}}";
		reply = site.send("POST", "/api/billing/webhook", event,
				{ stripe_signature(event, "whsec_" + random_hex(16)) });
		if (reply.code != 400 || reply_json(reply).get_string("error", "") != "bad_signature") {
			std::fprintf(stderr, "  webhook, a foreign signature: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}
		reply = site.send("POST", "/api/billing/webhook", event,
				{ stripe_signature(event, config.webhook_secret) });
		if (reply.code != 200 || !reply_json(reply).get_bool("received", false)) {
			std::fprintf(stderr, "  webhook: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}
		// The billing.sync job the webhook enqueued, as a worker runs it.
		billing_service.sync().sync_pending(billing::unix_now());

		// 6. The entitlement the service grants, as the support page reads it.
		reply = site.send("GET", "/api/me/billing");
		json = reply_json(reply);
		const JsonValue *subscription = json.get("subscription");
		if (reply.code != 200 || json.get_int("max_servers", -1) != 1 ||
				!json.get_bool("can_start", false) || !json.get_bool("can_manage", false) ||
				json.get_bool("can_subscribe", true) || subscription == nullptr ||
				!subscription->is_object() || subscription->get_int("tier", 0) != 1 ||
				subscription->get_string("status", "") != "active" ||
				!subscription->get_bool("entitled", false)) {
			std::fprintf(stderr, "  the account's billing: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}
		if (billing::entitlement_of(*service.pool.acquire(), user, billing::unix_now())
						.max_servers != 1) {
			std::fprintf(stderr, "  the entitlement is not one server\n");
			rc = 1;
			return;
		}
		reply = site.send("POST", "/api/billing/checkout", "{\"item\":\"tier2\"}");
		if (reply.code != 409 ||
				reply_json(reply).get_string("error", "") != "already_subscribed") {
			std::fprintf(stderr, "  a second tier: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}
		reply = site.send("POST", "/api/billing/portal");
		if (reply.code != 200 ||
				!starts_with(
						reply_json(reply).get_string("url", ""), "https://billing.stripe.com/")) {
			std::fprintf(stderr, "  portal: %s\n", describe(reply).c_str());
			rc = 1;
			return;
		}
	}();
	if (!open_session.empty() &&
			!billing_service.client().expire_checkout_session(open_session).ok())
		std::fprintf(stderr,
				"  the last Checkout session %s was not expired (it lapses in an hour)\n",
				open_session.c_str());
	if (!customer_id.empty()) {
		const auto deleted =
				stripe(api, billing::StripeRequest::Method::Delete, "/v1/customers/" + customer_id);
		if (!deleted || !deleted->get_bool("deleted", false)) {
			std::fprintf(
					stderr, "  cleanup: the customer %s was not deleted\n", customer_id.c_str());
			rc = 1;
		}
	}
	service.stop();
	return rc;
}

} // namespace

int main(int argc, char **argv) {
	const char *key = std::getenv("ONNET_STRIPE_TEST_SECRET_KEY");
	if (key == nullptr || *key == '\0' || std::string(key) == billing::kPlaceholder) {
		// Not the pg tests' "SKIP: needs" line, which fails the core suite: without the
		// key this test has nothing to run against (deploy/env/billing.e2e.env.tpl).
		std::printf(
				"STRIPE-E2E skipped: ONNET_STRIPE_TEST_SECRET_KEY names no Stripe test-mode key\n");
		return 77;
	}
	const std::string secret_key = key;
	if (!starts_with(secret_key, "sk_test_") && !starts_with(secret_key, "rk_test_")) {
		std::fprintf(stderr,
				"FAIL: ONNET_STRIPE_TEST_SECRET_KEY is not a test-mode key (sk_test_ or rk_test_); "
				"this test never runs in live mode\n");
		return 1;
	}
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	const int rc = run(secret_key);
	net::shutdown();
	if (rc == 0)
		std::printf("OK: billing against Stripe test mode\n");
	return rc;
}
