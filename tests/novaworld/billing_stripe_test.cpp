// The billing seam's pieces below the sync worker and the routes (apps/novaworld_common/
// billing, apps/novaworld_web/billing): the webhook signature check (a fixed vector
// computed with Python's hmac, several
// v1 signatures, several secrets, the 300 s boundary either way, every malformed header) and
// the event envelope; form encoding; CurlStripeApi on a stub CommandRunner (the argv exactly
// {curl, -q, --config, <file>} with no key in it, the config file's lines, its 0600 mode on
// POSIX, its deletion after a success and after a failure, the status split off the body,
// curl's failures read as Unreachable with no key in the text, and the retry rules); the
// typed client's requests and its readers on inline basil-shaped JSON (a subscription's
// current_period_end on its item, a deleted customer, a list page's has_more) and its error
// kinds; the typed client against the FakeStripeApi the later billing tests drive; and the
// billing settings' checks. Crow-free and database-free: compiles the billing sources and
// core/owner_file.cpp directly, so it runs on every build.

#include "billing/billing_config.h"
#include "billing/stripe_api.h"
#include "billing/stripe_client.h"
#include "billing/stripe_signature.h"
#include "billing/tiers.h"
#include "command_runner.h"
#include "core/owner_file.h"

#include <base/io/json.h>
#include <base/io/sha256.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"
#include "novaworld/billing_fake_stripe.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace billing = opennova::novaworld_server::billing;
namespace nws = opennova::novaworld_server;
namespace proc = opennova::proc;
using billing::SignatureCheck;
using billing::StripeError;
using billing::StripeReply;
using billing::StripeRequest;
using proc::CommandResult;

namespace {

// --- the webhook signature ------------------------------------------------------------------

constexpr const char *kWebhookSecret = "whsec_opennova_test_vector";
constexpr const char *kPayload = "{\"id\":\"evt_1\",\"object\":\"event\"}";
constexpr int64_t kSignedAt = 1700000000;
// python -c "import hmac,hashlib;print(hmac.new(b'whsec_opennova_test_vector',b'1700000000.{\"id\":\"evt_1\",\"object\":\"event\"}',hashlib.sha256).hexdigest())"
constexpr const char *kVector = "c5f23348f5314262939ed11f6191e872264ba4874937cc85d051984c832ad707";

SignatureCheck check(const std::string &header, int64_t now = kSignedAt,
		const std::vector<std::string> &secrets = { kWebhookSecret }, const std::string &payload = kPayload,
		int64_t *skew = nullptr) {
	return billing::verify_stripe_signature(header, payload, secrets, now, 300, skew);
}

int test_signature() {
	const std::string v1 = kVector;
	const std::string t = "t=" + std::to_string(kSignedAt);
	// The vector is HMAC-SHA256 keyed with the whole "whsec_..." text.
	TEST_EXPECT(opennova::io::sha256_hex(opennova::io::hmac_sha256(kWebhookSecret, t.substr(2) + "." + kPayload)) == v1);

	TEST_EXPECT(check(t + ",v1=" + v1) == SignatureCheck::Ok);
	TEST_EXPECT(check(t + ", v1=" + v1) == SignatureCheck::Ok);
	// Several v1 (a secret roll signs with both), v0 and unknown items ignored, upper-case hex.
	const std::string zeros(64, '0');
	TEST_EXPECT(check(t + ",v1=" + zeros + ",v1=" + v1 + ",v0=" + zeros + ",x=1") == SignatureCheck::Ok);
	TEST_EXPECT(check("v1=" + v1 + "," + t) == SignatureCheck::Ok);
	std::string upper = v1;
	for (char &c : upper) c = (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
	TEST_EXPECT(check(t + ",v1=" + upper) == SignatureCheck::Ok);
	// Any of several secrets.
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt, { "whsec_old0123", kWebhookSecret }) == SignatureCheck::Ok);
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt, { "whsec_other0123" }) == SignatureCheck::NoMatch);
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt, {}) == SignatureCheck::NoMatch);
	TEST_EXPECT(check(t + ",v1=" + zeros) == SignatureCheck::NoMatch);
	// The payload and the time are both signed.
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt, { kWebhookSecret }, std::string(kPayload) + " ") ==
			SignatureCheck::NoMatch);
	TEST_EXPECT(check("t=1700000001,v1=" + v1, kSignedAt) == SignatureCheck::NoMatch);

	// 300 s either way passes, 301 is stale, and the skew is reported.
	int64_t skew = 0;
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt + 300, { kWebhookSecret }, kPayload, &skew) == SignatureCheck::Ok);
	TEST_EXPECT(skew == 300);
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt + 301, { kWebhookSecret }, kPayload, &skew) ==
			SignatureCheck::Stale);
	TEST_EXPECT(skew == 301);
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt - 300, { kWebhookSecret }, kPayload, &skew) == SignatureCheck::Ok);
	TEST_EXPECT(skew == -300);
	TEST_EXPECT(check(t + ",v1=" + v1, kSignedAt - 301, { kWebhookSecret }, kPayload, &skew) ==
			SignatureCheck::Stale);
	TEST_EXPECT(skew == -301);
	// An unsigned delivery is NoMatch whatever its time.
	TEST_EXPECT(check(t + ",v1=" + zeros, kSignedAt + 9999) == SignatureCheck::NoMatch);

	TEST_EXPECT(check("") == SignatureCheck::Missing);
	TEST_EXPECT(check("  ") == SignatureCheck::Missing);
	const std::vector<std::string> malformed = {
		"v1=" + v1,                         // no t
		"t=abc,v1=" + v1,                   // t no number
		"t=-5,v1=" + v1,                    // a sign
		"t=,v1=" + v1,                      // empty t
		"t=1,t=2,v1=" + v1,                 // two t
		t,                                  // no v1
		t + ",v0=" + v1,                    // v0 alone
		t + ",v1=abc",                      // short
		t + ",v1=" + v1.substr(1),          // 63 digits
		t + ",v1=" + std::string(64, 'g'),  // not hex
		t + ",v1=" + v1 + ",garbage",       // an item with no '='
		t + ",v1=" + v1 + "0",              // 65 digits
	};
	for (const std::string &header : malformed) {
		if (check(header) != SignatureCheck::Malformed) {
			std::fprintf(stderr, "  not malformed: %s\n", header.c_str());
			return 1;
		}
	}
	TEST_EXPECT(std::string(billing::signature_check_name(SignatureCheck::NoMatch)) == "no_match");
	TEST_EXPECT(std::string(billing::signature_check_name(SignatureCheck::Ok)) == "ok");
	return 0;
}

int test_envelope() {
	auto e = billing::parse_webhook_envelope(
			"{\"id\":\"evt_1\",\"object\":\"event\",\"type\":\"customer.subscription.updated\",\"livemode\":false,"
			"\"created\":1700000000,\"api_version\":\"2025-03-31.basil\",\"data\":{\"object\":{\"id\":\"sub_1\","
			"\"object\":\"subscription\",\"customer\":\"cus_1\",\"status\":\"active\"},\"previous_attributes\":{}}}");
	TEST_EXPECT(e && e->id == "evt_1" && e->type == "customer.subscription.updated" && !e->livemode);
	TEST_EXPECT(e->created == 1700000000 && e->object_type == "subscription" && e->object_id == "sub_1");
	TEST_EXPECT(e->customer_id == "cus_1");

	// A customer event is about the object itself.
	e = billing::parse_webhook_envelope("{\"id\":\"evt_2\",\"object\":\"event\",\"type\":\"customer.deleted\","
										"\"livemode\":true,\"created\":1,\"data\":{\"object\":{\"id\":\"cus_9\","
										"\"object\":\"customer\",\"deleted\":true}}}");
	TEST_EXPECT(e && e->livemode && e->customer_id == "cus_9" && e->object_type == "customer");
	// An expanded customer gives its id; a null one, or none, gives "".
	e = billing::parse_webhook_envelope("{\"id\":\"evt_3\",\"object\":\"event\",\"type\":\"checkout.session.completed\","
										"\"livemode\":false,\"created\":2,\"data\":{\"object\":{\"id\":\"cs_1\","
										"\"object\":\"checkout.session\",\"customer\":{\"id\":\"cus_4\","
										"\"object\":\"customer\"}}}}");
	TEST_EXPECT(e && e->customer_id == "cus_4" && e->object_id == "cs_1");
	e = billing::parse_webhook_envelope("{\"id\":\"evt_4\",\"object\":\"event\",\"type\":\"invoice.paid\","
										"\"livemode\":false,\"created\":3,\"data\":{\"object\":{\"id\":\"in_1\","
										"\"object\":\"invoice\",\"customer\":null}}}");
	TEST_EXPECT(e && e->customer_id.empty() && e->object_type == "invoice");
	e = billing::parse_webhook_envelope("{\"id\":\"evt_5\",\"object\":\"event\",\"type\":\"price.updated\","
										"\"livemode\":false,\"created\":3,\"data\":{\"object\":{}}}");
	TEST_EXPECT(e && e->customer_id.empty() && e->object_type.empty() && e->object_id.empty());

	const std::vector<std::string> refused = {
		"",
		"not json",
		"[]",
		"{\"id\":\"evt_1\",\"object\":\"charge\",\"type\":\"a\",\"livemode\":false,\"created\":1,\"data\":{\"object\":{}}}",
		"{\"object\":\"event\",\"type\":\"a\",\"livemode\":false,\"created\":1,\"data\":{\"object\":{}}}",
		"{\"id\":\"\",\"object\":\"event\",\"type\":\"a\",\"livemode\":false,\"created\":1,\"data\":{\"object\":{}}}",
		"{\"id\":\"evt_1\",\"object\":\"event\",\"livemode\":false,\"created\":1,\"data\":{\"object\":{}}}",
		"{\"id\":\"evt_1\",\"object\":\"event\",\"type\":\"a\",\"livemode\":\"no\",\"created\":1,\"data\":{\"object\":{}}}",
		"{\"id\":\"evt_1\",\"object\":\"event\",\"type\":\"a\",\"livemode\":false,\"created\":1.5,\"data\":{\"object\":{}}}",
		"{\"id\":\"evt_1\",\"object\":\"event\",\"type\":\"a\",\"livemode\":false,\"created\":-1,\"data\":{\"object\":{}}}",
		"{\"id\":\"evt_1\",\"object\":\"event\",\"type\":\"a\",\"livemode\":false,\"created\":1}",
		"{\"id\":\"evt_1\",\"object\":\"event\",\"type\":\"a\",\"livemode\":false,\"created\":1,\"data\":{\"object\":[]}}",
	};
	for (const std::string &body : refused) {
		if (billing::parse_webhook_envelope(body)) {
			std::fprintf(stderr, "  envelope read: %s\n", body.c_str());
			return 1;
		}
	}
	return 0;
}

int test_form_encode() {
	TEST_EXPECT(billing::form_encode({}).empty());
	TEST_EXPECT(billing::form_encode({ { "metadata[opennova_user_id]", "7" }, { "expand[]", "data.customer" } }) ==
			"metadata[opennova_user_id]=7&expand[]=data.customer");
	TEST_EXPECT(billing::form_encode({ { "q", "a b&c=d+e/f?g#h%" } }) == "q=a%20b%26c%3Dd%2Be%2Ff%3Fg%23h%25");
	TEST_EXPECT(billing::form_encode({ { "k", "AZaz09-._~" } }) == "k=AZaz09-._~");
	TEST_EXPECT(billing::form_encode({ { "name", "Caf\xC3\xA9" } }) == "name=Caf%C3%A9");
	TEST_EXPECT(billing::form_encode({ { "k", "\"\\'\n" } }) == "k=%22%5C%27%0A");
	TEST_EXPECT(billing::form_encode({ { "metadata[x]", "" }, { "a", "1" } }) == "metadata[x]=&a=1");
	TEST_EXPECT(billing::form_encode({ { "success_url", "https://x.test/me/support?checkout=success&session_id={CHECKOUT_SESSION_ID}" } }) ==
			"success_url=https%3A%2F%2Fx.test%2Fme%2Fsupport%3Fcheckout%3Dsuccess%26session_id%3D%7BCHECKOUT_SESSION_ID%7D");
	return 0;
}

// --- the transport --------------------------------------------------------------------------

constexpr const char *kCurl = "/usr/bin/curl";
constexpr const char *kKey = "sk_test_OpenNovaFakeKey0123";

struct CurlRun {
	std::vector<std::string> argv;
	int timeout_ms = 0;
	std::string config;  // the config file's text while curl ran
	bool file_existed = false;
	unsigned mode = 0;   // its permission bits (POSIX)
};

class CurlStub : public proc::CommandRunner {
public:
	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		CurlRun run;
		run.argv = argv;
		run.timeout_ms = timeout_ms;
		if (argv.size() == 4) {
			const fs::path path = fs::u8path(argv[3]);
			run.file_existed = fs::exists(path);
			std::ifstream in(path, std::ios::binary);
			run.config.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
#ifndef _WIN32
			struct stat st {};
			if (::stat(argv[3].c_str(), &st) == 0) run.mode = static_cast<unsigned>(st.st_mode & 0777);
#endif
		}
		runs.push_back(run);
		if (replies.empty()) return exited(99, "", "the stub has no reply for this run");
		CommandResult reply = replies.front();
		replies.pop_front();
		return reply;
	}

	static CommandResult exited(uint32_t code, const std::string &out, const std::string &err = {}) {
		CommandResult r;
		r.outcome = CommandResult::Outcome::Exited;
		r.exit_code = code;
		r.out = out;
		r.err = err;
		return r;
	}
	// curl's stdout for a reply: the body, then the status on its own line.
	static CommandResult answered(const std::string &body, int status) {
		return exited(0, body + "\n" + std::to_string(status));
	}

	std::vector<CurlRun> runs;
	std::deque<CommandResult> replies;
};

billing::CurlStripeApi::Config curl_config(const std::string &scratch) {
	billing::CurlStripeApi::Config c;
	c.curl = kCurl;
	c.secret_key = kKey;
	c.scratch_dir = scratch;
	c.retry_delay_ms = 0;
	return c;
}

bool has_line(const std::string &text, const std::string &line) {
	return ("\n" + text).find("\n" + line + "\n") != std::string::npos;
}

// Whether `text` holds the key, or anything spelled like a Stripe secret key.
bool holds_key(const std::string &text) {
	for (const char *head : { "sk_test_", "sk_live_", "rk_test_", "rk_live_" }) {
		if (text.find(head) != std::string::npos) return true;
	}
	return text.find(kKey) != std::string::npos;
}

StripeRequest get_request(const std::string &path, std::vector<std::pair<std::string, std::string>> params = {}) {
	StripeRequest r;
	r.method = StripeRequest::Method::Get;
	r.path = path;
	r.params = std::move(params);
	return r;
}

StripeRequest post_request(const std::string &path, std::vector<std::pair<std::string, std::string>> params,
		const std::string &idempotency_key) {
	StripeRequest r;
	r.method = StripeRequest::Method::Post;
	r.path = path;
	r.params = std::move(params);
	r.idempotency_key = idempotency_key;
	return r;
}

bool is_lower_hex(const std::string &text) {
	return !text.empty() && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

int test_curl_post(const std::string &scratch) {
	CurlStub stub;
	billing::CurlStripeApi api(stub, curl_config(scratch));
	stub.replies.push_back(CurlStub::answered("{\"id\":\"cus_1\",\"object\":\"customer\"}", 200));
	const StripeReply reply = api.call(post_request("/v1/customers",
			{ { "metadata[opennova_username]", "a b" }, { "expand[]", "x" } }, "customer-inst-7"));
	TEST_EXPECT(reply.ok() && reply.outcome == StripeReply::Outcome::Answered && reply.status == 200);
	TEST_EXPECT(reply.body == "{\"id\":\"cus_1\",\"object\":\"customer\"}" && reply.error.empty());
	TEST_EXPECT(stub.runs.size() == 1);
	const CurlRun &run = stub.runs[0];
	// argv exactly {curl, -q, --config, <file>}: -q first, nothing of the key on it.
	TEST_EXPECT(run.argv.size() == 4 && run.argv[0] == kCurl && run.argv[1] == "-q" && run.argv[2] == "--config");
	for (const std::string &arg : run.argv) TEST_EXPECT(!holds_key(arg));
	TEST_EXPECT(run.timeout_ms == 25000);
	const fs::path file = fs::u8path(run.argv[3]);
	TEST_EXPECT(fs::equivalent(file.parent_path(), fs::u8path(scratch)));
	const std::string name = file.filename().u8string();
	TEST_EXPECT(name.size() == 7 + 16 + 4 && name.compare(0, 7, "stripe-") == 0 &&
			name.compare(name.size() - 4, 4, ".cfg") == 0 && is_lower_hex(name.substr(7, 16)));
	TEST_EXPECT(run.file_existed);
#ifndef _WIN32
	TEST_EXPECT(run.mode == 0600);
#endif
	const std::string &config = run.config;
	TEST_EXPECT(has_line(config, "url = \"https://api.stripe.com/v1/customers\""));
	TEST_EXPECT(has_line(config, "request = \"POST\""));
	TEST_EXPECT(has_line(config, std::string("header = \"Authorization: Bearer ") + kKey + "\""));
	TEST_EXPECT(has_line(config, "header = \"Stripe-Version: 2025-03-31.basil\""));
	TEST_EXPECT(has_line(config, "header = \"Idempotency-Key: customer-inst-7\""));
	TEST_EXPECT(has_line(config, "data-raw = \"metadata[opennova_username]=a%20b&expand[]=x\""));
	TEST_EXPECT(has_line(config, "silent") && has_line(config, "show-error") && has_line(config, "globoff"));
	TEST_EXPECT(has_line(config, "proto = \"=https\""));
	TEST_EXPECT(has_line(config, "connect-timeout = 5") && has_line(config, "max-time = 20"));
	TEST_EXPECT(has_line(config, "write-out = \"\\n%{http_code}\""));
	// Deleted once curl ended: nothing of the call stays.
	TEST_EXPECT(!fs::exists(file));
	TEST_EXPECT(fs::is_empty(fs::u8path(scratch)));
	return 0;
}

int test_curl_get(const std::string &scratch) {
	CurlStub stub;
	billing::CurlStripeApi api(stub, curl_config(scratch));
	stub.replies.push_back(CurlStub::answered("{}", 200));
	StripeRequest request = get_request("/v1/subscriptions",
			{ { "customer", "cus_1" }, { "status", "all" }, { "expand[]", "data.customer" } });
	request.idempotency_key = "ignored-on-a-get";
	TEST_EXPECT(api.call(request).ok());
	TEST_EXPECT(stub.runs.size() == 1);
	const std::string &config = stub.runs[0].config;
	TEST_EXPECT(has_line(config, "url = \"https://api.stripe.com/v1/subscriptions?customer=cus_1&status=all&expand[]=data.customer\""));
	TEST_EXPECT(has_line(config, "request = \"GET\""));
	TEST_EXPECT(config.find("Idempotency-Key") == std::string::npos);
	TEST_EXPECT(config.find("data-raw") == std::string::npos);
	TEST_EXPECT(fs::is_empty(fs::u8path(scratch)));

	// A local mock over plain HTTP: the protocol follows the base URL.
	billing::CurlStripeApi::Config local = curl_config(scratch);
	local.base_url = "http://127.0.0.1:12111";
	billing::CurlStripeApi mock(stub, local);
	stub.replies.push_back(CurlStub::answered("{}", 200));
	TEST_EXPECT(mock.call(get_request("/v1/prices/price_1")).ok());
	TEST_EXPECT(has_line(stub.runs.back().config, "url = \"http://127.0.0.1:12111/v1/prices/price_1\""));
	TEST_EXPECT(has_line(stub.runs.back().config, "proto = \"=http\""));
	return 0;
}

int test_curl_status_split(const std::string &scratch) {
	CurlStub stub;
	billing::CurlStripeApi api(stub, curl_config(scratch));
	stub.replies.push_back(CurlStub::answered("{\n  \"error\": {\"code\": \"resource_missing\"}\n}\n", 404));
	StripeReply reply = api.call(get_request("/v1/customers/cus_x"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Answered && reply.status == 404 && !reply.ok());
	TEST_EXPECT(reply.body == "{\n  \"error\": {\"code\": \"resource_missing\"}\n}\n");
	TEST_EXPECT(stub.runs.size() == 1); // a 4xx is no retry

	stub.replies.push_back(CurlStub::answered("", 204));
	reply = api.call(get_request("/v1/customers/cus_x"));
	TEST_EXPECT(reply.ok() && reply.status == 204 && reply.body.empty());

	// curl ended well but wrote no status: no answer (a GET, so tried twice).
	stub.replies.push_back(CurlStub::exited(0, "{}"));
	stub.replies.push_back(CurlStub::exited(0, "{}\n000"));
	reply = api.call(get_request("/v1/customers/cus_x"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && reply.status == 0 && !reply.error.empty());
	TEST_EXPECT(stub.runs.size() == 4);
	TEST_EXPECT(fs::is_empty(fs::u8path(scratch)));
	return 0;
}

int test_curl_failures(const std::string &scratch) {
	CurlStub stub;
	billing::CurlStripeApi api(stub, curl_config(scratch));
	// curl's own failure (6: no such host) is Unreachable; a GET tries once more.
	stub.replies.push_back(CurlStub::exited(6, "", "curl: (6) Could not resolve host: api.stripe.com\n"));
	stub.replies.push_back(CurlStub::exited(60, "", "curl: (60) SSL certificate problem\nMore details here\n"));
	StripeReply reply = api.call(get_request("/v1/customers/cus_1"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && !reply.ok());
	// The last run's code and the first line of its message.
	TEST_EXPECT(reply.error == "curl exited with code 60: curl: (60) SSL certificate problem");
	TEST_EXPECT(stub.runs.size() == 2);
	// The file went after each failed run too.
	for (const CurlRun &run : stub.runs) TEST_EXPECT(run.file_existed && !fs::exists(fs::u8path(run.argv[3])));
	TEST_EXPECT(fs::is_empty(fs::u8path(scratch)));

	// No error text holds the key, whatever curl wrote.
	stub.runs.clear();
	stub.replies.push_back(CurlStub::exited(26, "", std::string("curl: bad header Bearer ") + kKey + "\n"));
	reply = api.call(post_request("/v1/customers", {}, ""));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && !holds_key(reply.error));
	TEST_EXPECT(reply.error.find("***") != std::string::npos && stub.runs.size() == 1);

	stub.replies.push_back(CurlStub::exited(0, ""));
	stub.replies.back().outcome = CommandResult::Outcome::TimedOut;
	stub.replies.push_back(stub.replies.back());
	reply = api.call(get_request("/v1/customers/cus_1"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && reply.error.find("timed out") != std::string::npos);

	CommandResult not_started;
	not_started.outcome = CommandResult::Outcome::NotStarted;
	not_started.error = std::string("cannot start /usr/bin/curl: No such file or directory ") + kKey;
	stub.replies.push_back(not_started);
	stub.replies.push_back(not_started);
	reply = api.call(get_request("/v1/customers/cus_1"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && reply.error.find("did not start") != std::string::npos);
	TEST_EXPECT(!holds_key(reply.error));
	TEST_EXPECT(fs::is_empty(fs::u8path(scratch)));

	// A scratch directory that is not there: no file, no run, no key in the text.
	billing::CurlStripeApi::Config gone = curl_config(scratch);
	gone.scratch_dir = (fs::u8path(scratch) / "no-such-dir").u8string();
	billing::CurlStripeApi lost(stub, gone);
	stub.runs.clear();
	reply = lost.call(get_request("/v1/customers/cus_1"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && stub.runs.empty());
	TEST_EXPECT(reply.error.find("cannot write") != std::string::npos && !holds_key(reply.error));
	return 0;
}

int test_curl_retries(const std::string &scratch) {
	CurlStub stub;
	billing::CurlStripeApi api(stub, curl_config(scratch));
	const auto runs_for = [&stub, &api](const StripeRequest &request, std::vector<CommandResult> replies,
								  StripeReply &reply) {
		stub.runs.clear();
		stub.replies.assign(replies.begin(), replies.end());
		reply = api.call(request);
		return stub.runs.size();
	};
	StripeReply reply;
	const StripeRequest get = get_request("/v1/subscriptions/sub_1");
	const StripeRequest keyed = post_request("/v1/customers", { { "a", "b" } }, "key-1");
	const StripeRequest unkeyed = post_request("/v1/customers", { { "a", "b" } }, "");

	// A GET: a 5xx, a 429 and no answer are tried once more, and once only.
	TEST_EXPECT(runs_for(get, { CurlStub::answered("{}", 503), CurlStub::answered("{\"ok\":1}", 200) }, reply) == 2);
	TEST_EXPECT(reply.ok() && reply.body == "{\"ok\":1}");
	TEST_EXPECT(runs_for(get, { CurlStub::answered("{}", 429), CurlStub::answered("{}", 429),
										CurlStub::answered("{}", 200) },
						reply) == 2);
	TEST_EXPECT(reply.status == 429 && !reply.ok());
	TEST_EXPECT(runs_for(get, { CurlStub::exited(7, ""), CurlStub::answered("{}", 200) }, reply) == 2);
	TEST_EXPECT(reply.ok());
	// A 4xx is Stripe's answer: never sent again.
	TEST_EXPECT(runs_for(get, { CurlStub::answered("{}", 400), CurlStub::answered("{}", 200) }, reply) == 1);
	TEST_EXPECT(reply.status == 400);
	TEST_EXPECT(runs_for(get, { CurlStub::answered("{}", 402) }, reply) == 1);

	// A POST with its idempotency key is sent again (the same file text: the same key).
	TEST_EXPECT(runs_for(keyed, { CurlStub::answered("{}", 500), CurlStub::answered("{}", 200) }, reply) == 2);
	TEST_EXPECT(reply.ok() && stub.runs[0].config == stub.runs[1].config);
	TEST_EXPECT(runs_for(keyed, { CurlStub::exited(28, ""), CurlStub::answered("{}", 200) }, reply) == 2);
	// One without is never: it might act twice.
	TEST_EXPECT(runs_for(unkeyed, { CurlStub::answered("{}", 500), CurlStub::answered("{}", 200) }, reply) == 1);
	TEST_EXPECT(reply.status == 500);
	TEST_EXPECT(runs_for(unkeyed, { CurlStub::exited(7, ""), CurlStub::answered("{}", 200) }, reply) == 1);
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable);
	// A DELETE (the billing end-to-end test's cleanup): its verb, no body and no key,
	// and never sent again.
	StripeRequest remove = get_request("/v1/customers/cus_1");
	remove.method = StripeRequest::Method::Delete;
	TEST_EXPECT(runs_for(remove, { CurlStub::answered("{}", 500), CurlStub::answered("{}", 200) },
						reply) == 1);
	TEST_EXPECT(runs_for(remove, { CurlStub::exited(7, ""), CurlStub::answered("{}", 200) },
						reply) == 1);
	TEST_EXPECT(runs_for(remove, { CurlStub::answered("{\"deleted\":true}", 200) }, reply) == 1 &&
			reply.ok());
	const std::string &config = stub.runs[0].config;
	TEST_EXPECT(has_line(config, "url = \"https://api.stripe.com/v1/customers/cus_1\""));
	TEST_EXPECT(has_line(config, "request = \"DELETE\""));
	TEST_EXPECT(config.find("data-raw") == std::string::npos &&
			config.find("Idempotency-Key") == std::string::npos);
	TEST_EXPECT(fs::is_empty(fs::u8path(scratch)));
	return 0;
}

int test_curl_refusals(const std::string &scratch) {
	CurlStub stub;
	billing::CurlStripeApi api(stub, curl_config(scratch));
	// Refused before any file or run: no retry either.
	for (const char *path : { "/v2/customers", "/v1/customers/cus_1?x=1", "/v1/customers/../x", "v1/customers",
				 "/v1/customers/cus 1" }) {
		const StripeReply reply = api.call(get_request(path));
		TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && reply.error.find("refused") != std::string::npos);
	}
	StripeReply reply = api.call(post_request("/v1/customers", {}, "key\nInjected: 1"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && reply.error.find("refused") != std::string::npos);
	TEST_EXPECT(stub.runs.empty() && fs::is_empty(fs::u8path(scratch)));

	// A value's quote and backslash are escaped in the file.
	stub.replies.push_back(CurlStub::answered("{}", 200));
	TEST_EXPECT(api.call(post_request("/v1/customers", {}, "a\"b\\c")).ok());
	TEST_EXPECT(has_line(stub.runs.back().config, "header = \"Idempotency-Key: a\\\"b\\\\c\""));
	TEST_EXPECT(has_line(stub.runs.back().config, "data-raw = \"\""));

	// A key with a control byte cannot be written into the file.
	billing::CurlStripeApi::Config bad = curl_config(scratch);
	bad.secret_key = std::string(kKey) + "\n";
	billing::CurlStripeApi refusing(stub, bad);
	stub.runs.clear();
	reply = refusing.call(get_request("/v1/customers/cus_1"));
	TEST_EXPECT(reply.outcome == StripeReply::Outcome::Unreachable && stub.runs.empty() && !holds_key(reply.error));
	return 0;
}

int test_curl_config_checks(const std::string &scratch) {
	const billing::CurlStripeApi::Config good = curl_config(scratch);
	TEST_EXPECT(billing::CurlStripeApi::check_config(good).empty());
	const auto refused = [&good](void (*spoil)(billing::CurlStripeApi::Config &)) {
		billing::CurlStripeApi::Config c = good;
		spoil(c);
		const std::string why = billing::CurlStripeApi::check_config(c);
		return !why.empty() && !holds_key(why);
	};
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.curl = "curl"; }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.curl.clear(); }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.secret_key.clear(); }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.secret_key += "\r"; }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.scratch_dir.clear(); }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.base_url = "https://api.stripe.com/"; }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.base_url = "https://api.stripe.com/v1"; }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.base_url = "ftp://api.stripe.com"; }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.base_url = "https://"; }));
	TEST_EXPECT(refused([](billing::CurlStripeApi::Config &c) { c.timeout_ms = 0; }));
	billing::CurlStripeApi::Config local = good;
	local.base_url = "http://127.0.0.1:12111";
	TEST_EXPECT(billing::CurlStripeApi::check_config(local).empty());
	local.curl = "C:\\Windows\\System32\\curl.exe";
	TEST_EXPECT(billing::CurlStripeApi::check_config(local).empty());
	return 0;
}

#ifdef _WIN32
void set_env(const char *name, const char *value) { _putenv_s(name, value); }
#else
void set_env(const char *name, const char *value) { setenv(name, value, 1); }
#endif

int test_curl_environment() {
	set_env("STRIPE_SECRET_KEY", kKey);
	set_env("AWS_SECRET_ACCESS_KEY", "opennova-test-secret");
	set_env("ONNET_OPENNOVA_TEST_SECRET", "hunter2");
	set_env("TZ", "UTC");
	const std::vector<std::string> env = billing::curl_environment();
	bool tz = false;
	for (const std::string &entry : env) {
		TEST_EXPECT(entry.rfind("STRIPE_", 0) != 0 && entry.rfind("AWS_", 0) != 0 && entry.rfind("ONNET_", 0) != 0);
		TEST_EXPECT(!holds_key(entry));
		tz = tz || entry == "TZ=UTC";
	}
	TEST_EXPECT(tz);
	return 0;
}

// --- the typed client -----------------------------------------------------------------------

// A StripeApi answering from a script and keeping each request.
class ScriptedApi : public billing::StripeApi {
public:
	StripeReply call(const StripeRequest &request) override {
		requests.push_back(request);
		if (replies.empty()) {
			StripeReply none;
			none.error = "the script has no reply";
			return none;
		}
		StripeReply reply = replies.front();
		replies.pop_front();
		return reply;
	}

	void answer(const std::string &body, int status = 200) {
		StripeReply reply;
		reply.outcome = StripeReply::Outcome::Answered;
		reply.status = status;
		reply.body = body;
		replies.push_back(reply);
	}

	// The value of the field `name` of request `index`; "\x01" when absent.
	std::string param(size_t index, const std::string &name) const {
		for (const auto &[key, value] : requests.at(index).params) {
			if (key == name) return value;
		}
		return "\x01";
	}

	std::vector<StripeRequest> requests;
	std::deque<StripeReply> replies;
};

// Basil's subscription: current_period_end on the item, not the subscription.
const char *kSubscriptionJson = R"({
  "id": "sub_1Abc",
  "object": "subscription",
  "customer": "cus_1Abc",
  "status": "active",
  "livemode": false,
  "cancel_at_period_end": true,
  "cancel_at": null,
  "created": 1759000000,
  "metadata": {"opennova_instance": "inst-1", "opennova_user_id": "7"},
  "items": {
    "object": "list",
    "data": [{
      "id": "si_1Abc",
      "object": "subscription_item",
      "price": {"id": "price_t2", "object": "price", "active": true, "currency": "usd", "unit_amount": 1000,
        "recurring": {"interval": "month", "interval_count": 1}},
      "quantity": 1,
      "current_period_start": 1759000000,
      "current_period_end": 1761592000
    }],
    "has_more": false,
    "url": "/v1/subscription_items?subscription=sub_1Abc"
  }
})";

int test_client_readers() {
	ScriptedApi api;
	billing::StripeClient client(api);

	api.answer(kSubscriptionJson);
	auto sub = client.get_subscription("sub_1Abc");
	TEST_EXPECT(sub.ok() && sub.value->id == "sub_1Abc" && sub.value->customer == "cus_1Abc");
	TEST_EXPECT(sub.value->status == "active" && !sub.value->livemode && sub.value->cancel_at_period_end);
	TEST_EXPECT(sub.value->cancel_at == 0 && sub.value->created == 1759000000);
	TEST_EXPECT(sub.value->items.size() == 1 && sub.value->first_price_id() == "price_t2");
	TEST_EXPECT(sub.value->first_period_end() == 1761592000 && sub.value->first_quantity() == 1);
	TEST_EXPECT(sub.value->metadata.at("opennova_instance") == "inst-1" && !sub.value->customer_metadata);
	TEST_EXPECT(api.requests[0].method == StripeRequest::Method::Get && api.requests[0].path == "/v1/subscriptions/sub_1Abc");
	TEST_EXPECT(api.requests[0].params.empty());

	// Expanded: the customer's metadata comes along, the id stays an id.
	std::string expanded = kSubscriptionJson;
	expanded.replace(expanded.find("\"cus_1Abc\""), 10,
			"{\"id\":\"cus_1Abc\",\"object\":\"customer\",\"livemode\":false,\"metadata\":{\"opennova_user_id\":\"7\"}}");
	api.answer(expanded);
	sub = client.get_subscription("sub_1Abc", true);
	TEST_EXPECT(sub.ok() && sub.value->customer == "cus_1Abc" && sub.value->customer_metadata);
	TEST_EXPECT(sub.value->customer_metadata->at("opennova_user_id") == "7" && !sub.value->customer_deleted);
	TEST_EXPECT(api.param(1, "expand[]") == "customer");

	// The shape before basil (the period on the subscription alone) is not this version's.
	std::string old_shape = kSubscriptionJson;
	old_shape.replace(old_shape.find("\"current_period_end\": 1761592000"), 32, "\"plan\": null");
	api.answer(old_shape);
	sub = client.get_subscription("sub_1Abc");
	TEST_EXPECT(!sub.ok() && sub.error.kind == StripeError::Kind::Malformed && sub.error.status == 200);

	// A deleted customer answers its id and the flag alone.
	api.answer("{\"id\":\"cus_gone\",\"object\":\"customer\",\"deleted\":true}");
	auto customer = client.get_customer("cus_gone");
	TEST_EXPECT(customer.ok() && customer.value->deleted && customer.value->id == "cus_gone");
	TEST_EXPECT(customer.value->metadata.empty());
	api.answer("{\"id\":\"cus_1\",\"object\":\"customer\",\"livemode\":true,\"email\":\"a@b.test\","
			   "\"metadata\":{\"opennova_user_id\":\"7\"}}");
	customer = client.get_customer("cus_1");
	TEST_EXPECT(customer.ok() && !customer.value->deleted && customer.value->livemode);
	TEST_EXPECT(customer.value->email == "a@b.test" && customer.value->metadata.at("opennova_user_id") == "7");
	// A live customer without its livemode is not one.
	api.answer("{\"id\":\"cus_1\",\"object\":\"customer\",\"metadata\":{}}");
	TEST_EXPECT(client.get_customer("cus_1").error.kind == StripeError::Kind::Malformed);

	// A list page: two subscriptions, more to come.
	api.answer(std::string("{\"object\":\"list\",\"url\":\"/v1/subscriptions\",\"has_more\":true,\"data\":[") +
			kSubscriptionJson + "," + expanded + "]}");
	billing::SubscriptionQuery query;
	query.customer = "cus_1Abc";
	query.starting_after = "sub_0";
	query.expand_customer = true;
	const size_t at = api.requests.size();
	auto page = client.list_subscriptions(query);
	TEST_EXPECT(page.ok() && page.value->has_more && page.value->data.size() == 2);
	TEST_EXPECT(!page.value->data[0].customer_metadata && page.value->data[1].customer_metadata);
	TEST_EXPECT(api.requests[at].path == "/v1/subscriptions" && api.requests[at].method == StripeRequest::Method::Get);
	TEST_EXPECT(api.param(at, "customer") == "cus_1Abc" && api.param(at, "status") == "all");
	TEST_EXPECT(api.param(at, "starting_after") == "sub_0" && api.param(at, "limit") == "100");
	TEST_EXPECT(api.param(at, "expand[]") == "data.customer");
	api.answer("{\"object\":\"list\",\"has_more\":false,\"data\":[]}");
	page = client.list_subscriptions({});
	TEST_EXPECT(page.ok() && !page.value->has_more && page.value->data.empty());
	TEST_EXPECT(api.param(at + 1, "customer") == "\x01" && api.param(at + 1, "expand[]") == "\x01");
	// One element that is no subscription spoils the page.
	api.answer(std::string("{\"object\":\"list\",\"has_more\":false,\"data\":[") + kSubscriptionJson +
			",{\"id\":\"x\",\"object\":\"invoice\"}]}");
	TEST_EXPECT(client.list_subscriptions({}).error.kind == StripeError::Kind::Malformed);

	// A price.
	api.answer("{\"id\":\"price_t1\",\"object\":\"price\",\"active\":true,\"livemode\":false,\"currency\":\"usd\","
			   "\"unit_amount\":500,\"type\":\"recurring\",\"recurring\":{\"interval\":\"month\",\"interval_count\":1,"
			   "\"usage_type\":\"licensed\"},\"product\":\"prod_1\"}");
	auto price = client.get_price("price_t1");
	TEST_EXPECT(price.ok() && price.value->active && price.value->unit_amount && *price.value->unit_amount == 500);
	TEST_EXPECT(price.value->currency == "usd" && price.value->type == "recurring");
	TEST_EXPECT(price.value->recurring_interval == "month" && price.value->recurring_interval_count == 1);
	TEST_EXPECT(price.value->product == "prod_1" && api.requests.back().path == "/v1/prices/price_t1");
	api.answer("{\"id\":\"price_x\",\"object\":\"price\",\"active\":false,\"livemode\":true,\"currency\":\"eur\","
			   "\"unit_amount\":null,\"type\":\"one_time\",\"recurring\":null,\"product\":{\"id\":\"prod_2\"}}");
	price = client.get_price("price_x");
	TEST_EXPECT(price.ok() && !price.value->unit_amount && price.value->recurring_interval.empty());
	TEST_EXPECT(price.value->livemode && price.value->product == "prod_2");
	return 0;
}

int test_client_requests() {
	ScriptedApi api;
	billing::StripeClient client(api);

	api.answer("{\"id\":\"cus_1\",\"object\":\"customer\",\"livemode\":false,\"metadata\":{}}");
	auto customer = client.create_customer(7, "Some One", "inst-1", "customer-inst-1-7");
	TEST_EXPECT(customer.ok() && customer.value->id == "cus_1");
	const StripeRequest &create = api.requests[0];
	TEST_EXPECT(create.method == StripeRequest::Method::Post && create.path == "/v1/customers");
	TEST_EXPECT(create.idempotency_key == "customer-inst-1-7");
	TEST_EXPECT(api.param(0, "metadata[opennova_instance]") == "inst-1");
	TEST_EXPECT(api.param(0, "metadata[opennova_user_id]") == "7");
	TEST_EXPECT(api.param(0, "metadata[opennova_username]") == "Some One");

	billing::CheckoutCreate checkout;
	checkout.customer = "cus_1";
	checkout.client_reference_id = "7";
	checkout.price_id = "price_t1";
	checkout.quantity = 2;
	checkout.success_url = "https://nw.test/me/support?checkout=success&session_id={CHECKOUT_SESSION_ID}";
	checkout.cancel_url = "https://nw.test/me/support?checkout=cancel";
	checkout.metadata = { { "opennova_instance", "inst-1" }, { "opennova_user_id", "7" } };
	checkout.expires_at = 1760001800;
	checkout.automatic_tax = true;
	checkout.require_tos_consent = true;
	api.answer("{\"id\":\"cs_test_1\",\"object\":\"checkout.session\",\"url\":\"https://checkout.stripe.com/c/pay/cs_test_1\","
			   "\"status\":\"open\",\"customer\":\"cus_1\",\"subscription\":null,\"client_reference_id\":\"7\","
			   "\"mode\":\"subscription\",\"payment_status\":\"unpaid\",\"metadata\":{},\"expires_at\":1760001800,"
			   "\"livemode\":false}");
	auto session = client.create_checkout_session(checkout);
	TEST_EXPECT(session.ok() && session.value->id == "cs_test_1" && session.value->status == "open");
	TEST_EXPECT(session.value->url == "https://checkout.stripe.com/c/pay/cs_test_1" && session.value->subscription.empty());
	TEST_EXPECT(session.value->expires_at == 1760001800 && session.value->client_reference_id == "7");
	TEST_EXPECT(api.requests[1].path == "/v1/checkout/sessions");
	// A fresh random key when the caller gave none: "opennova-" and 32 hex digits.
	const std::string &key = api.requests[1].idempotency_key;
	TEST_EXPECT(key.size() == 41 && key.rfind("opennova-", 0) == 0 && is_lower_hex(key.substr(9)));
	TEST_EXPECT(api.param(1, "mode") == "subscription" && api.param(1, "customer") == "cus_1");
	TEST_EXPECT(api.param(1, "line_items[0][price]") == "price_t1" && api.param(1, "line_items[0][quantity]") == "2");
	TEST_EXPECT(api.param(1, "client_reference_id") == "7" && api.param(1, "success_url") == checkout.success_url);
	TEST_EXPECT(api.param(1, "metadata[opennova_instance]") == "inst-1");
	TEST_EXPECT(api.param(1, "subscription_data[metadata][opennova_user_id]") == "7");
	TEST_EXPECT(api.param(1, "expires_at") == "1760001800");
	TEST_EXPECT(api.param(1, "automatic_tax[enabled]") == "true" && api.param(1, "customer_update[address]") == "auto");
	TEST_EXPECT(api.param(1, "consent_collection[terms_of_service]") == "required");
	// Without tax or consent those fields are not sent.
	checkout.automatic_tax = false;
	checkout.require_tos_consent = false;
	checkout.expires_at = 0;
	checkout.idempotency_key = "checkout-1";
	api.answer("{\"id\":\"cs_test_2\",\"object\":\"checkout.session\",\"url\":null,\"status\":\"expired\","
			   "\"customer\":\"cus_1\",\"metadata\":{},\"livemode\":false}");
	TEST_EXPECT(client.create_checkout_session(checkout).ok());
	TEST_EXPECT(api.requests[2].idempotency_key == "checkout-1");
	TEST_EXPECT(api.param(2, "automatic_tax[enabled]") == "\x01" && api.param(2, "consent_collection[terms_of_service]") == "\x01");
	TEST_EXPECT(api.param(2, "expires_at") == "\x01");

	api.answer("{\"id\":\"cs_test_1\",\"object\":\"checkout.session\",\"url\":null,\"status\":\"expired\","
			   "\"customer\":\"cus_1\",\"metadata\":{},\"livemode\":false}");
	session = client.expire_checkout_session("cs_test_1");
	TEST_EXPECT(session.ok() && session.value->status == "expired" && session.value->url.empty());
	TEST_EXPECT(api.requests[3].method == StripeRequest::Method::Post &&
			api.requests[3].path == "/v1/checkout/sessions/cs_test_1/expire");
	TEST_EXPECT(!api.requests[3].idempotency_key.empty() && api.requests[3].params.empty());

	api.answer("{\"id\":\"bps_1\",\"object\":\"billing_portal.session\",\"url\":\"https://billing.stripe.com/p/session/x\"}");
	const auto portal = client.create_portal_session("cus_1", "https://nw.test/me/support");
	TEST_EXPECT(portal.ok() && *portal.value == "https://billing.stripe.com/p/session/x");
	TEST_EXPECT(api.requests[4].path == "/v1/billing_portal/sessions" && api.param(4, "customer") == "cus_1");
	TEST_EXPECT(api.param(4, "return_url") == "https://nw.test/me/support" && !api.requests[4].idempotency_key.empty());

	api.answer(kSubscriptionJson);
	TEST_EXPECT(client.update_subscription_metadata("sub_1Abc", { { "opennova_instance", "inst-1" }, { "old", "" } }).ok());
	TEST_EXPECT(api.requests[5].method == StripeRequest::Method::Post && api.requests[5].path == "/v1/subscriptions/sub_1Abc");
	TEST_EXPECT(api.param(5, "metadata[opennova_instance]") == "inst-1" && api.param(5, "metadata[old]").empty());
	api.answer("{\"id\":\"cus_1\",\"object\":\"customer\",\"livemode\":false,\"metadata\":{\"a\":\"b\"}}");
	TEST_EXPECT(client.update_customer_metadata("cus_1", { { "a", "b" } }).ok());
	TEST_EXPECT(api.requests[6].path == "/v1/customers/cus_1" && api.param(6, "metadata[a]") == "b");

	// Arguments this side refuses never reach Stripe.
	const size_t sent = api.requests.size();
	auto refused = client.get_customer("cus_1/../x");
	TEST_EXPECT(!refused.ok() && refused.error.kind == StripeError::Kind::Rejected && refused.error.status == 0);
	TEST_EXPECT(!client.get_subscription("").ok() && !client.get_price("price 1").ok());
	TEST_EXPECT(!client.update_customer_metadata("cus_1", { { "a[b]", "c" } }).ok());
	TEST_EXPECT(!client.update_customer_metadata("cus_1", { { std::string(41, 'k'), "c" } }).ok());
	billing::CheckoutCreate bad = checkout;
	bad.quantity = 0;
	TEST_EXPECT(!client.create_checkout_session(bad).ok());
	bad = checkout;
	bad.success_url.clear();
	TEST_EXPECT(!client.create_checkout_session(bad).ok());
	billing::SubscriptionQuery query;
	query.starting_after = "sub_1&limit=1";
	TEST_EXPECT(!client.list_subscriptions(query).ok());
	TEST_EXPECT(!client.create_customer(0, "x", "inst-1", "k").ok());
	TEST_EXPECT(api.requests.size() == sent);
	TEST_EXPECT(billing::is_stripe_id("sub_1Abc") && !billing::is_stripe_id("sub-1") && !billing::is_stripe_id(""));
	return 0;
}

int test_client_errors() {
	ScriptedApi api;
	billing::StripeClient client(api);

	api.answer("{\"error\":{\"code\":\"resource_missing\",\"doc_url\":\"https://stripe.com/docs/error-codes/resource-missing\","
			   "\"message\":\"No such customer: 'cus_x'\",\"param\":\"id\",\"type\":\"invalid_request_error\"}}",
			404);
	auto r = client.get_customer("cus_x");
	TEST_EXPECT(!r.ok() && r.error.kind == StripeError::Kind::Rejected && r.error.status == 404);
	TEST_EXPECT(r.error.stripe_code == "resource_missing" && r.error.message == "No such customer: 'cus_x'");
	// The log line's text names Stripe's status, code and message: the routes'
	// failures (prod's first checkouts logged "rejected" alone, which said
	// nothing of why).
	TEST_EXPECT(billing::stripe_error_text(r.error) ==
			"rejected 404 (resource_missing): No such customer: 'cus_x'");
	// No code: the type stands in.
	api.answer("{\"error\":{\"message\":\"Keys for idempotent requests can only be used with the same parameters\","
			   "\"type\":\"idempotency_error\"}}",
			400);
	r = client.get_customer("cus_x");
	TEST_EXPECT(r.error.kind == StripeError::Kind::Rejected && r.error.stripe_code == "idempotency_error");
	// A 429 and a 5xx: try again later.
	api.answer("{\"error\":{\"type\":\"api_error\",\"message\":\"boom\"}}", 500);
	r = client.get_customer("cus_x");
	TEST_EXPECT(r.error.kind == StripeError::Kind::Unreachable && r.error.status == 500 && r.error.message == "boom");
	api.answer("not json", 429);
	r = client.get_customer("cus_x");
	TEST_EXPECT(r.error.kind == StripeError::Kind::Unreachable && r.error.status == 429);
	TEST_EXPECT(r.error.message == "Stripe answered HTTP 429");
	// No answer at all.
	StripeReply none;
	none.error = "curl exited with code 7: Failed to connect";
	api.replies.push_back(none);
	r = client.get_customer("cus_x");
	TEST_EXPECT(r.error.kind == StripeError::Kind::Unreachable && r.error.status == 0);
	TEST_EXPECT(r.error.message == "curl exited with code 7: Failed to connect");
	TEST_EXPECT(billing::stripe_error_text(r.error) ==
			"unreachable: curl exited with code 7: Failed to connect");
	// A 2xx that is not what was asked for.
	api.answer("<html>");
	r = client.get_customer("cus_x");
	TEST_EXPECT(r.error.kind == StripeError::Kind::Malformed && r.error.status == 200);
	api.answer("{\"id\":\"sub_1\",\"object\":\"subscription\"}");
	r = client.get_customer("cus_x");
	TEST_EXPECT(r.error.kind == StripeError::Kind::Malformed);
	api.answer("{\"id\":\"cus_1\",\"object\":\"customer\",\"livemode\":false,\"metadata\":{\"n\":7}}");
	TEST_EXPECT(client.get_customer("cus_1").error.kind == StripeError::Kind::Malformed);
	TEST_EXPECT(std::string(billing::stripe_error_kind_name(StripeError::Kind::Rejected)) == "rejected");
	return 0;
}

// --- the fake -------------------------------------------------------------------------------

int test_fake_round_trip() {
	billing_fake::FakeStripeApi fake;
	billing_fake::FakeStripeApi::Price tier1;
	tier1.id = "price_t1";
	fake.add_price(tier1);
	billing::StripeClient client(fake);

	// The same idempotency key answers the same customer.
	auto customer = client.create_customer(7, "alice", "inst-1", "customer-inst-1-7");
	TEST_EXPECT(customer.ok() && billing::is_stripe_id(customer.value->id));
	TEST_EXPECT(customer.value->metadata.at(billing::kMetaInstance) == "inst-1");
	auto again = client.create_customer(7, "alice", "inst-1", "customer-inst-1-7");
	TEST_EXPECT(again.ok() && again.value->id == customer.value->id && fake.customers.size() == 1);
	const std::string cus = customer.value->id;

	billing::CheckoutCreate checkout;
	checkout.customer = cus;
	checkout.client_reference_id = "7";
	checkout.price_id = "price_t1";
	checkout.success_url = "https://nw.test/ok";
	checkout.cancel_url = "https://nw.test/cancel";
	checkout.metadata = { { billing::kMetaInstance, "inst-1" } };
	auto session = client.create_checkout_session(checkout);
	TEST_EXPECT(session.ok() && session.value->status == "open");
	TEST_EXPECT(session.value->url.rfind("https://checkout.stripe.com/", 0) == 0);
	const std::string first = fake.complete_checkout(session.value->id);
	TEST_EXPECT(!first.empty());
	session = client.get_checkout_session(session.value->id);
	TEST_EXPECT(session.ok() && session.value->status == "complete" && session.value->subscription == first);
	TEST_EXPECT(session.value->url.empty());
	TEST_EXPECT(!client.expire_checkout_session(session.value->id).ok());

	auto sub = client.get_subscription(first, true);
	TEST_EXPECT(sub.ok() && sub.value->status == "active" && sub.value->first_price_id() == "price_t1");
	TEST_EXPECT(sub.value->first_period_end() == fake.now + 30 * 86400);
	TEST_EXPECT(sub.value->metadata.at(billing::kMetaInstance) == "inst-1");
	TEST_EXPECT(sub.value->customer_metadata && sub.value->customer_metadata->at(billing::kMetaUserId) == "7");

	// Pagination: newest first, one a page.
	const std::string second = fake.add_subscription(cus, "price_t1", 1, "past_due", fake.now + 86400);
	billing::SubscriptionQuery query;
	query.customer = cus;
	query.limit = 1;
	auto page = client.list_subscriptions(query);
	TEST_EXPECT(page.ok() && page.value->has_more && page.value->data.size() == 1 && page.value->data[0].id == second);
	query.starting_after = second;
	page = client.list_subscriptions(query);
	TEST_EXPECT(page.ok() && !page.value->has_more && page.value->data.size() == 1 && page.value->data[0].id == first);

	TEST_EXPECT(client.update_subscription_metadata(first, { { "x", "1" } }).ok());
	TEST_EXPECT(fake.subscriptions[first].metadata.at("x") == "1");
	const auto portal = client.create_portal_session(cus, "https://nw.test/me/support");
	TEST_EXPECT(portal.ok() && portal.value->rfind("https://billing.stripe.com/", 0) == 0);
	TEST_EXPECT(client.get_price("price_t1").ok() && !client.get_price("price_none").ok());

	// Failures queue up and leave the state alone.
	fake.fail_next(503);
	fake.fail_next(StripeReply::Outcome::Unreachable);
	auto failed = client.get_subscription(first);
	TEST_EXPECT(failed.error.kind == StripeError::Kind::Unreachable && failed.error.status == 503);
	failed = client.get_subscription(first);
	TEST_EXPECT(failed.error.kind == StripeError::Kind::Unreachable && failed.error.status == 0);
	TEST_EXPECT(client.get_subscription(first).ok());

	// A deleted customer: its subscriptions end, Stripe's default list leaves them out.
	fake.delete_customer(cus);
	customer = client.get_customer(cus);
	TEST_EXPECT(customer.ok() && customer.value->deleted);
	query = {};
	query.status.clear();
	page = client.list_subscriptions(query);
	TEST_EXPECT(page.ok() && page.value->data.empty());
	query.status = "all";
	page = client.list_subscriptions(query);
	TEST_EXPECT(page.ok() && page.value->data.size() == 2 && page.value->data[0].status == "canceled");
	TEST_EXPECT(fake.count(StripeRequest::Method::Post, "/v1/customers") == 2);
	return 0;
}

// --- the settings ---------------------------------------------------------------------------

using Env = std::map<std::string, std::string>;

billing::BillingConfig config_of(const Env &env) {
	return billing::billing_config_from([&env](const char *name) -> const char * {
		const auto it = env.find(name);
		return it == env.end() ? nullptr : it->second.c_str();
	});
}

Env complete_env() {
	return {
		{ "STRIPE_SECRET_KEY", " rk_test_OpenNovaRestrict01 " },
		{ "STRIPE_WEBHOOK_SECRET", "whsec_0123456789abcdef" },
		{ "STRIPE_PRICE_TIER1", "price_t1new, price_t1old" },
		{ "STRIPE_PRICE_TIER2", "price_t2" },
		{ "STRIPE_PRICE_TIER3", "price_t3,," },
		{ "STRIPE_PRICE_OWN_MACHINE", "price_own" },
		{ "ONNET_SITE_URL", "http://localhost:8080/" },
	};
}

// Whether `env` with `name` set to `value` (erased when null) is refused, with a reason that
// names the variable and holds no secret.
bool refused_with(const char *name, const char *value, const char *names) {
	Env env = complete_env();
	if (value == nullptr)
		env.erase(name);
	else
		env[name] = value;
	const std::string why = billing::check_billing_config(config_of(env));
	if (why.empty() || why.find(names) == std::string::npos || why.find("SECRETVALUE") != std::string::npos ||
			why.find("0123456789abcdef") != std::string::npos) {
		std::fprintf(stderr, "  %s=%s: '%s'\n", name, value ? value : "(unset)", why.c_str());
		return false;
	}
	return true;
}

int test_billing_config() {
	billing::BillingConfig c = config_of(complete_env());
	TEST_EXPECT(billing::check_billing_config(c).empty());
	TEST_EXPECT(c.secret_key == "rk_test_OpenNovaRestrict01" && !c.live());
	TEST_EXPECT(c.site_url == "http://localhost:8080" && c.curl_path == "/usr/bin/curl");
	TEST_EXPECT(c.tier_prices[0].empty() && c.tier_prices[1].size() == 2 && c.tier_prices[1][1] == "price_t1old");
	TEST_EXPECT(c.tier_prices[3].size() == 1 && c.own_machine_prices.size() == 1);
	TEST_EXPECT(!c.automatic_tax && c.reconcile_minutes == 360);

	// Missing, and the vault's placeholder, read the same.
	TEST_EXPECT(refused_with("STRIPE_SECRET_KEY", nullptr, "STRIPE_SECRET_KEY is not set"));
	TEST_EXPECT(refused_with("STRIPE_SECRET_KEY", "REPLACE_ME", "STRIPE_SECRET_KEY is not set"));
	TEST_EXPECT(refused_with("STRIPE_SECRET_KEY", "pk_test_SECRETVALUE", "STRIPE_SECRET_KEY"));
	TEST_EXPECT(refused_with("STRIPE_SECRET_KEY", "sk_tst_SECRETVALUE", "STRIPE_SECRET_KEY"));
	TEST_EXPECT(refused_with("STRIPE_SECRET_KEY", "sk_test_", "STRIPE_SECRET_KEY"));
	TEST_EXPECT(refused_with("STRIPE_SECRET_KEY", "sk_test_SECRET VALUE", "STRIPE_SECRET_KEY"));
	TEST_EXPECT(refused_with("STRIPE_WEBHOOK_SECRET", nullptr, "STRIPE_WEBHOOK_SECRET is not set"));
	TEST_EXPECT(refused_with("STRIPE_WEBHOOK_SECRET", " REPLACE_ME ", "STRIPE_WEBHOOK_SECRET is not set"));
	TEST_EXPECT(refused_with("STRIPE_WEBHOOK_SECRET", "wh_SECRETVALUE", "STRIPE_WEBHOOK_SECRET"));
	TEST_EXPECT(refused_with("STRIPE_PRICE_TIER2", nullptr, "STRIPE_PRICE_TIER2 is not set"));
	TEST_EXPECT(refused_with("STRIPE_PRICE_TIER2", "REPLACE_ME", "STRIPE_PRICE_TIER2 is not set"));
	TEST_EXPECT(refused_with("STRIPE_PRICE_TIER2", " , ", "STRIPE_PRICE_TIER2 is not set"));
	TEST_EXPECT(refused_with("STRIPE_PRICE_TIER3", "price_t3,prod_t3", "prod_t3"));
	TEST_EXPECT(refused_with("STRIPE_PRICE_OWN_MACHINE", "REPLACE_ME", "STRIPE_PRICE_OWN_MACHINE is not set"));
	TEST_EXPECT(refused_with("STRIPE_PRICE_OWN_MACHINE", "price_t2", "listed twice"));
	TEST_EXPECT(refused_with("STRIPE_PRICE_TIER2", "price_t1old", "listed twice"));
	// The static-address add-on's prices: needed while the fleet sells addresses alone.
	{
		Env addresses = complete_env();
		TEST_EXPECT(billing::check_billing_config(config_of(addresses)).empty() &&
				!config_of(addresses).sells_addresses);
		addresses["STRIPE_PRICE_STATIC_ADDRESS"] = "REPLACE_ME";
		TEST_EXPECT(billing::check_billing_config(config_of(addresses)).empty() &&
				config_of(addresses).static_address_prices.empty());
		addresses["ONNET_FLEET_MAX_ADDRESSES"] = "4";
		TEST_EXPECT(config_of(addresses).sells_addresses);
		TEST_EXPECT(billing::check_billing_config(config_of(addresses)).find("STRIPE_PRICE_STATIC_ADDRESS is not set") !=
				std::string::npos);
		addresses["STRIPE_PRICE_STATIC_ADDRESS"] = "price_own";
		TEST_EXPECT(billing::check_billing_config(config_of(addresses)).find("listed twice") != std::string::npos);
		addresses["STRIPE_PRICE_STATIC_ADDRESS"] = "price_addr, price_addrold";
		c = config_of(addresses);
		TEST_EXPECT(billing::check_billing_config(c).empty() && c.static_address_prices.size() == 2);
		TEST_EXPECT(billing::static_address_price_for_sale(c) == "price_addr" &&
				billing::is_static_address_price(c, "price_addrold") && !billing::is_static_address_price(c, "price_own"));
		addresses["ONNET_FLEET_MAX_ADDRESSES"] = "0";
		addresses["STRIPE_PRICE_STATIC_ADDRESS"] = "prod_x";
		TEST_EXPECT(billing::check_billing_config(config_of(addresses)).find("prod_x") != std::string::npos);
	}
	TEST_EXPECT(refused_with("ONNET_SITE_URL", nullptr, "ONNET_SITE_URL is not set"));
	TEST_EXPECT(refused_with("ONNET_SITE_URL", "localhost:8080", "ONNET_SITE_URL"));
	TEST_EXPECT(refused_with("ONNET_SITE_URL", "https://nw.test/app", "ONNET_SITE_URL"));
	TEST_EXPECT(refused_with("ONNET_SITE_URL", "https://nw.test?x=1", "ONNET_SITE_URL"));
	TEST_EXPECT(refused_with("ONNET_SITE_URL", "https://nw.test:port", "ONNET_SITE_URL"));
	TEST_EXPECT(refused_with("ONNET_STRIPE_CURL", "curl", "ONNET_STRIPE_CURL"));
	TEST_EXPECT(refused_with("ONNET_BILLING_RECONCILE_MINUTES", "abc", "ONNET_BILLING_RECONCILE_MINUTES"));
	TEST_EXPECT(refused_with("ONNET_BILLING_RECONCILE_MINUTES", "4", "ONNET_BILLING_RECONCILE_MINUTES"));
	TEST_EXPECT(refused_with("ONNET_BILLING_RECONCILE_MINUTES", "-60", "ONNET_BILLING_RECONCILE_MINUTES"));
	TEST_EXPECT(refused_with("ONNET_BILLING_RECONCILE_MINUTES", "99999999", "ONNET_BILLING_RECONCILE_MINUTES"));

	// The mode: a live key wants an https site; a test key takes either.
	Env env = complete_env();
	env["STRIPE_SECRET_KEY"] = "sk_live_SECRETVALUE";
	c = config_of(env);
	TEST_EXPECT(c.live());
	const std::string why = billing::check_billing_config(c);
	TEST_EXPECT(why.find("ONNET_SITE_URL must be https://") != std::string::npos && why.find("SECRETVALUE") == std::string::npos);
	env["ONNET_SITE_URL"] = "https://novaworld.example.org";
	TEST_EXPECT(billing::check_billing_config(config_of(env)).empty());
	env["STRIPE_SECRET_KEY"] = "rk_live_SECRETVALUE";
	TEST_EXPECT(config_of(env).live() && billing::check_billing_config(config_of(env)).empty());
	env["STRIPE_SECRET_KEY"] = "sk_test_SECRETVALUE";
	TEST_EXPECT(!config_of(env).live() && billing::check_billing_config(config_of(env)).empty());

	env["ONNET_STRIPE_CURL"] = "/opt/curl/bin/curl";
	env["ONNET_BILLING_AUTOMATIC_TAX"] = "1";
	env["ONNET_BILLING_RECONCILE_MINUTES"] = "60";
	c = config_of(env);
	TEST_EXPECT(c.curl_path == "/opt/curl/bin/curl" && c.automatic_tax && c.reconcile_minutes == 60);
	TEST_EXPECT(billing::check_billing_config(c).empty());
	env["ONNET_BILLING_AUTOMATIC_TAX"] = "0";
	TEST_EXPECT(!config_of(env).automatic_tax);
	env["ONNET_STRIPE_CURL"] = "C:\\Windows\\System32\\curl.exe";
	TEST_EXPECT(billing::check_billing_config(config_of(env)).empty());

	// Prices to tiers, the grandfathered ones too.
	c = config_of(complete_env());
	TEST_EXPECT(billing::tier_of_price(c, "price_t1new") == 1 && billing::tier_of_price(c, "price_t1old") == 1);
	TEST_EXPECT(billing::tier_of_price(c, "price_t2") == 2 && billing::tier_of_price(c, "price_t3") == 3);
	TEST_EXPECT(billing::tier_of_price(c, "price_own") == 0 && billing::tier_of_price(c, "price_none") == 0);
	TEST_EXPECT(billing::tier_of_price(c, "") == 0);
	TEST_EXPECT(billing::is_own_machine_price(c, "price_own") && !billing::is_own_machine_price(c, "price_t1new"));
	TEST_EXPECT(!billing::is_own_machine_price(c, ""));
	TEST_EXPECT(billing::tier_price_for_sale(c, 1) == "price_t1new" && billing::tier_price_for_sale(c, 0).empty());
	TEST_EXPECT(billing::tier_price_for_sale(c, 4).empty() && billing::own_machine_price_for_sale(c) == "price_own");
	TEST_EXPECT(billing::kTierServers[0] == 0 && billing::kTierServers[1] == 1 && billing::kTierServers[2] == 2 &&
			billing::kTierServers[3] == 3);
	return 0;
}

} // namespace

int main() {
	test_temp::TempDir temp("billing_stripe");
	const fs::path scratch_path = temp.path / "scratch";
	std::string error;
	if (!nws::make_owner_directory(scratch_path, error)) {
		std::fprintf(stderr, "cannot make the scratch directory: %s\n", error.c_str());
		return 1;
	}
	const std::string scratch = scratch_path.u8string();
	struct Case {
		const char *name;
		int (*fn)(const std::string &);
	};
	const Case cases[] = {
		{ "signature", [](const std::string &) { return test_signature(); } },
		{ "envelope", [](const std::string &) { return test_envelope(); } },
		{ "form_encode", [](const std::string &) { return test_form_encode(); } },
		{ "curl_post", test_curl_post },
		{ "curl_get", test_curl_get },
		{ "curl_status_split", test_curl_status_split },
		{ "curl_failures", test_curl_failures },
		{ "curl_retries", test_curl_retries },
		{ "curl_refusals", test_curl_refusals },
		{ "curl_config_checks", test_curl_config_checks },
		{ "curl_environment", [](const std::string &) { return test_curl_environment(); } },
		{ "client_readers", [](const std::string &) { return test_client_readers(); } },
		{ "client_requests", [](const std::string &) { return test_client_requests(); } },
		{ "client_errors", [](const std::string &) { return test_client_errors(); } },
		{ "fake_round_trip", [](const std::string &) { return test_fake_round_trip(); } },
		{ "billing_config", [](const std::string &) { return test_billing_config(); } },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn(scratch) != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: billing stripe\n");
	return 0;
}
