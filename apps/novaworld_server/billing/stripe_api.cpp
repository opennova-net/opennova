#include "stripe_api.h"

#include "hosted/owner_file.h"
#include "loggable.h"

#include <base/io/strutil.h>
#include <base/os_random/os_random.h>
#include <base/proc/child_process.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

namespace opennova::novaworld_server::billing {

namespace fs = std::filesystem;

namespace {

// curl's own bounds on one run, in seconds (the runner's timeout_ms sits over them).
constexpr int kConnectTimeoutS = 5;
constexpr int kMaxTimeS = 20;

bool is_unreserved(unsigned char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
			c == '.' || c == '_' || c == '~';
}

void append_encoded(std::string &out, std::string_view text) {
	static constexpr char kDigits[] = "0123456789ABCDEF";
	for (const char ch : text) {
		const auto c = static_cast<unsigned char>(ch);
		if (is_unreserved(c) || c == '[' || c == ']') {
			out.push_back(ch);
		} else {
			out.push_back('%');
			out.push_back(kDigits[c >> 4]);
			out.push_back(kDigits[c & 0xFu]);
		}
	}
}

bool is_path_char(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
			c == '/';
}

bool is_url_char(char c) { return is_path_char(c) || c == '.' || c == ':'; }

bool starts_with(std::string_view text, std::string_view head) { return text.substr(0, head.size()) == head; }

// `text` as a double-quoted curl config value ('\' and '"' escaped); nullopt
// when it holds a control byte, which no line of the file may carry.
std::optional<std::string> config_value(std::string_view text) {
	if (has_control_byte(text)) return std::nullopt;
	std::string out = "\"";
	for (const char c : text) {
		if (c == '\\' || c == '"') out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
	return out;
}

// `bytes` random bytes from the OS CSPRNG as lower-case hex.
std::string random_hex(size_t bytes) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < bytes; ++i) {
		uint8_t b = 0;
		os_random_bytes(&b, 1);
		out.push_back(kDigits[b >> 4]);
		out.push_back(kDigits[b & 0xFu]);
	}
	return out;
}

// `text` with every copy of `secret` replaced by "***".
std::string scrub(std::string text, const std::string &secret) {
	if (secret.empty()) return text;
	for (size_t at = text.find(secret); at != std::string::npos; at = text.find(secret, at + 3))
		text.replace(at, secret.size(), "***");
	return text;
}

// Whether a failed call may be sent again: a request Stripe answers once
// however often it arrives (a GET, a POST with its idempotency key) that met no
// answer, a 429 or a 5xx.
bool retryable(const StripeRequest &request, const StripeReply &reply) {
	if (request.method == StripeRequest::Method::Post && request.idempotency_key.empty()) return false;
	if (reply.outcome == StripeReply::Outcome::Unreachable) return true;
	return reply.status == 429 || reply.status >= 500;
}

StripeReply unreachable(std::string error) {
	StripeReply reply;
	reply.outcome = StripeReply::Outcome::Unreachable;
	reply.error = std::move(error);
	return reply;
}

} // namespace

std::string form_encode(const std::vector<std::pair<std::string, std::string>> &params) {
	std::string out;
	for (size_t i = 0; i < params.size(); ++i) {
		if (i != 0) out.push_back('&');
		append_encoded(out, params[i].first);
		out.push_back('=');
		append_encoded(out, params[i].second);
	}
	return out;
}

std::vector<std::string> curl_environment() {
	static const std::vector<std::string> kNames = {
		"PATH", "LANG", "LC_*", "TZ",
#ifdef _WIN32
		"SystemRoot", "TEMP", "TMP",
#endif
	};
	return proc::filter_environment(kNames);
}

CurlStripeApi::CurlStripeApi(proc::CommandRunner &runner, Config config) :
		runner_(runner), config_(std::move(config)) {}

std::string CurlStripeApi::check_config(const Config &config) {
	// A POSIX root or a drive letter (as Ec2Provider reads ONNET_HOSTED_AWS_CLI).
	const std::string &curl = config.curl;
	if (curl.empty() || !(curl[0] == '/' || (curl.size() > 2 && curl[1] == ':')))
		return "the curl path '" + loggable(curl, 200) + "' is not absolute";
	if (config.secret_key.empty()) return "no secret key";
	if (has_control_byte(config.secret_key)) return "the secret key holds a control byte";
	if (config.scratch_dir.empty()) return "no scratch directory";
	if (config.api_version.empty() || has_control_byte(config.api_version)) return "no API version";
	const std::string &url = config.base_url;
	const size_t scheme = starts_with(url, "https://") ? 8 : (starts_with(url, "http://") ? 7 : 0);
	bool chars = true;
	for (const char c : url) chars = chars && is_url_char(c);
	if (scheme == 0 || !chars || url.size() <= scheme || url.back() == '/' ||
			url.find('/', scheme) != std::string::npos)
		return "the base URL '" + loggable(url, 200) + "' is not http(s)://host[:port]";
	if (config.timeout_ms <= 0) return "the timeout is not positive";
	if (config.retry_delay_ms < 0) return "the retry delay is negative";
	return {};
}

StripeReply CurlStripeApi::call(const StripeRequest &request) {
	std::string refusal;
	const std::string text = config_text(request, refusal);
	// Refused before curl ran: nothing to try again.
	if (!refusal.empty()) return unreachable("the request was refused before it was sent: " + refusal);
	StripeReply reply = run_config(text);
	if (!retryable(request, reply)) return reply;
	if (config_.retry_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_delay_ms));
	return run_config(text);
}

std::string CurlStripeApi::config_text(const StripeRequest &request, std::string &refusal) const {
	bool path_ok = starts_with(request.path, "/v1/");
	for (const char c : request.path) path_ok = path_ok && is_path_char(c);
	if (!path_ok) {
		refusal = "the path '" + loggable(request.path, 120) + "' is not /v1/ and [A-Za-z0-9_/-]";
		return {};
	}
	const bool post = request.method == StripeRequest::Method::Post;
	const std::string form = form_encode(request.params);
	std::string url = config_.base_url + request.path;
	if (!post && !form.empty()) url += "?" + form;

	const auto url_value = config_value(url);
	const auto auth = config_value("Authorization: Bearer " + config_.secret_key);
	const auto version = config_value("Stripe-Version: " + config_.api_version);
	const auto key = config_value("Idempotency-Key: " + request.idempotency_key);
	if (!url_value) refusal = "the URL holds a control byte";
	else if (!auth) refusal = "the secret key holds a control byte";
	else if (!version) refusal = "the API version holds a control byte";
	else if (post && !key) refusal = "the idempotency key holds a control byte";
	if (!refusal.empty()) return {};

	std::string text;
	text += "url = " + *url_value + "\n";
	text += std::string("request = ") + (post ? "\"POST\"" : "\"GET\"") + "\n";
	text += "header = " + *auth + "\n";
	text += "header = " + *version + "\n";
	if (post) {
		if (!request.idempotency_key.empty()) text += "header = " + *key + "\n";
		// The form text is percent-encoded: no quote, backslash or control byte.
		text += "data-raw = " + *config_value(form) + "\n";
	}
	text += "user-agent = \"opennova-novaworld\"\n";
	text += "silent\n";
	text += "show-error\n";
	// No URL globbing: "expand[]" and "metadata[key]" are field names, not ranges.
	text += "globoff\n";
	text += std::string("proto = ") + (starts_with(config_.base_url, "http://") ? "\"=http\"" : "\"=https\"") + "\n";
	text += "connect-timeout = " + std::to_string(kConnectTimeoutS) + "\n";
	text += "max-time = " + std::to_string(kMaxTimeS) + "\n";
	// The status on a line of its own after the body.
	text += "write-out = \"\\n%{http_code}\"\n";
	return text;
}

StripeReply CurlStripeApi::run_config(const std::string &text) {
	const fs::path file = fs::u8path(config_.scratch_dir) / ("stripe-" + random_hex(8) + ".cfg");
	std::string error;
	std::error_code ec;
	if (!hosted::write_owner_file(file, text, true, error)) {
		fs::remove(file, ec);
		return unreachable("cannot write the request file: " + scrub(error, config_.secret_key));
	}
	const proc::CommandResult run =
			runner_.run({ config_.curl, "-q", "--config", file.u8string() }, config_.timeout_ms);
	fs::remove(file, ec);

	switch (run.outcome) {
		case proc::CommandResult::Outcome::NotStarted:
			return unreachable("curl did not start: " + loggable(scrub(run.error, config_.secret_key), 200));
		case proc::CommandResult::Outcome::TimedOut:
			return unreachable("curl timed out after " + std::to_string(config_.timeout_ms) + " ms");
		case proc::CommandResult::Outcome::Exited: break;
	}
	if (run.exit_code != 0) {
		// curl's exit code (6 no such host, 7 refused, 28 timed out, 35 TLS, 60 the
		// certificate) and the first line of its message.
		std::string why = "curl exited with code " + std::to_string(run.exit_code);
		const std::string_view err = strutil::trim_view(run.err);
		const std::string first(strutil::trim_view(err.substr(0, err.find('\n'))));
		if (!first.empty()) why += ": " + loggable(scrub(first, config_.secret_key), 300);
		return unreachable(why);
	}
	const size_t split = run.out.rfind('\n');
	const std::string code = split == std::string::npos ? std::string() : run.out.substr(split + 1);
	if (code.size() != 3 || !strutil::all_digits(code) || code == "000")
		return unreachable("curl's output ends with no HTTP status");
	StripeReply reply;
	reply.outcome = StripeReply::Outcome::Answered;
	reply.status = strutil::parse_int(code).value_or(0);
	reply.body = run.out.substr(0, split);
	return reply;
}

} // namespace opennova::novaworld_server::billing
