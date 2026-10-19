#pragma once

// The service's road to Stripe's REST API (https://api.stripe.com/v1): one
// request in, Stripe's HTTP status and JSON body out. StripeApi is the seam the
// billing code calls (stripe_client.h types it) and the tests replace with a
// fake (tests/novaworld/billing_fake_stripe.h).
//
// CurlStripeApi runs curl through a proc::CommandRunner: an argv array with no
// shell, a timeout, and a child whose standard input is always the null device
// (base/proc), so nothing can be piped to it. The secret key, the headers and
// the form body therefore travel in a curl config file of the call's own,
// written owner-only (0600; a protected DACL on Windows) into the service's
// owner-only scratch directory and deleted once curl ends, success or not. The
// argv is exactly {curl, "-q", "--config", <that file>}: -q first, so no
// ~/.curlrc is read, and no secret on a command line another account could list.
// The child's environment is the runner's: hand it curl_environment() (the
// system's own variables, never STRIPE_* or AWS_*).
//
// Every request names the pinned API version (kStripeApiVersion; the parsers in
// stripe_client.h read that version's shapes), allows HTTPS alone (plain HTTP
// only when the base URL is http://, for a local mock), and bounds itself: a 5 s
// connect, 20 s in all, the runner's timeout_ms over that. A POST carries its
// Idempotency-Key. A call that does not reach Stripe (curl's own failure, its
// timeout), a 429 or a 5xx is tried once more about a second later, but only a
// GET or a POST with an idempotency key, which Stripe answers once however often
// it is sent (a DELETE, which only the billing end-to-end test's cleanup sends,
// is not). No error text ever holds the key. Thread-safe: a call keeps no
// state but its own file. Platform plumbing, not a port.

#include "command_runner.h"

#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::billing {

// The API version every request pins (the Stripe-Version header). Basil moved
// current_period_end from the subscription onto each of its items.
inline constexpr const char *kStripeApiVersion = "2025-03-31.basil";

struct StripeRequest {
	enum class Method { Get, Post, Delete };
	Method method = Method::Get;
	// From the API's root: "/v1/customers", "/v1/subscriptions/sub_123". Letters,
	// digits, '_', '-' and '/' alone (an id is checked before it is spelled in).
	std::string path;
	// The form fields, in order, unencoded: {"metadata[opennova_user_id]", "7"}.
	// A GET or a DELETE sends them as the query string, a POST as its body.
	std::vector<std::pair<std::string, std::string>> params;
	// A POST's Idempotency-Key (ignored on a GET). Empty: none, and no retry.
	std::string idempotency_key;
};

struct StripeReply {
	enum class Outcome {
		Answered,    // Stripe answered: `status` and `body` are its reply
		Unreachable, // no answer (curl failed, timed out, or never ran): `error` says why
	};
	Outcome outcome = Outcome::Unreachable;
	int status = 0;
	std::string body;
	// Why there is no answer: curl's exit code and its message. Never the key.
	std::string error;

	bool ok() const { return outcome == Outcome::Answered && status >= 200 && status < 300; }
};

class StripeApi {
public:
	virtual ~StripeApi() = default;
	virtual StripeReply call(const StripeRequest &request) = 0;
};

class CurlStripeApi final : public StripeApi {
public:
	struct Config {
		std::string curl;        // an absolute path, run as given (ONNET_STRIPE_CURL)
		std::string secret_key;  // sk_/rk_ (STRIPE_SECRET_KEY)
		std::string scratch_dir; // owner-only (core/owner_file.h make_owner_directory)
		std::string api_version = kStripeApiVersion;
		std::string base_url = "https://api.stripe.com"; // http(s)://host[:port], no path
		int timeout_ms = 25000;     // the runner's bound on one curl run
		int retry_delay_ms = 1000;  // the pause before the one retry
	};

	CurlStripeApi(proc::CommandRunner &runner, Config config);
	StripeReply call(const StripeRequest &request) override;

	// "" when `config` can serve requests, else why not (never the key's text):
	// an absolute curl path, a key with no control byte, a scratch directory,
	// an API version, an http(s)://host[:port] base URL, a positive timeout.
	static std::string check_config(const Config &config);

private:
	// The curl config file's text for `request`; "" with `refusal` set when it
	// cannot be sent (a path outside /v1/, a control byte in a value).
	std::string config_text(const StripeRequest &request, std::string &refusal) const;
	// One curl run over `text`: the file written, run, and deleted.
	StripeReply run_config(const std::string &text);

	proc::CommandRunner &runner_;
	Config config_;
};

// The variables curl's child keeps from this process's environment: PATH, LANG,
// LC_* and TZ (and SystemRoot, TEMP and TMP on Windows, which its sockets and
// temporary files need), never STRIPE_*, AWS_* or the service's own. The
// environment a proc::ProcessCommandRunner for CurlStripeApi is made with.
std::vector<std::string> curl_environment();

// `params` as application/x-www-form-urlencoded text, in order: each name and
// value percent-encoded byte by byte (upper-case hex), but RFC 3986's unreserved
// characters (letters, digits, '-', '.', '_', '~') and the brackets of Stripe's
// nested names ("metadata[key]", "expand[]"), kept as they are, as Stripe's own
// libraries send them.
std::string form_encode(const std::vector<std::pair<std::string, std::string>> &params);

} // namespace opennova::novaworld_server::billing
