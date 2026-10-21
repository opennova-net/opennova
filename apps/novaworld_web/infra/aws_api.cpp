#include "aws_api.h"

#include "aws_xml.h"
#include "core/loggable.h"
#include "core/owner_file.h"
#include "core/text_util.h"

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace opennova::novaworld_server::infra {

namespace fs = std::filesystem;

namespace {

// curl's own bounds on one run, in seconds (the runner's timeout_ms sits over
// them); the agent's link-local endpoint answers at once or not at all.
constexpr int kConnectTimeoutMs = 5000;
constexpr int kMaxTimeMs = 12000;
constexpr int kCredentialsMaxTimeMs = 4000;

// Milliseconds as curl's decimal seconds, independent of the process locale.
std::string seconds_text(int milliseconds) {
	const std::string fraction = std::to_string(1000 + milliseconds % 1000).substr(1);
	return std::to_string(milliseconds / 1000) + "." + fraction;
}

int call_timeout(int configured, int requested) {
	return requested < 0 ? configured : std::min(configured, requested);
}

// STS credentials are taken from the expected element once, never from a
// similarly named descendant of another response or an ambiguous duplicate.
const XmlNode *only_child(const XmlNode &node, std::string_view name) {
	const XmlNode *found = nullptr;
	for (const XmlNode &child : node.children) {
		if (child.name != name)
			continue;
		if (found != nullptr)
			return nullptr;
		found = &child;
	}
	return found;
}

std::string credential_text(const XmlNode &node, std::string_view name) {
	const XmlNode *child = only_child(node, name);
	return child != nullptr && child->children.empty()
			? std::string(strutil::trim_view(child->text))
			: std::string();
}

bool is_unreserved(unsigned char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
			c == '.' || c == '_' || c == '~';
}

void append_encoded(std::string &out, std::string_view text) {
	static constexpr char kDigits[] = "0123456789ABCDEF";
	for (const char ch : text) {
		const auto c = static_cast<unsigned char>(ch);
		if (is_unreserved(c)) {
			out.push_back(ch);
		} else {
			out.push_back('%');
			out.push_back(kDigits[c >> 4]);
			out.push_back(kDigits[c & 0xFu]);
		}
	}
}

bool is_region_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

bool is_uri_char(char c) {
	return is_alnum(c) || c == '/' || c == '-' || c == '_' || c == '.';
}

// `text` as a double-quoted curl config value ('\' and '"' escaped); nullopt
// when it holds a control byte, which no line of the file may carry.
std::optional<std::string> config_value(std::string_view text) {
	if (has_control_byte(text))
		return std::nullopt;
	std::string out = "\"";
	for (const char c : text) {
		if (c == '\\' || c == '"')
			out.push_back('\\');
		out.push_back(c);
	}
	out.push_back('"');
	return out;
}

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

// `text` with every copy of each of `secrets` replaced by "***".
std::string scrub(std::string text, const std::vector<std::string> &secrets) {
	for (const std::string &secret : secrets) {
		if (secret.empty())
			continue;
		for (size_t at = text.find(secret); at != std::string::npos; at = text.find(secret, at + 3))
			text.replace(at, secret.size(), "***");
	}
	return text;
}

AwsReply unreachable(std::string error) {
	AwsReply reply;
	reply.outcome = AwsReply::Outcome::Unreachable;
	reply.error = std::move(error);
	return reply;
}

// The lines every run's config file ends with.
std::string common_tail(bool https, int max_time_ms) {
	std::string text;
	text += "user-agent = \"opennova-novaworld\"\n";
	text += "silent\n";
	text += "show-error\n";
	text += std::string("proto = ") + (https ? "\"=https\"" : "\"=http\"") + "\n";
	text += "connect-timeout = " + seconds_text(std::min(kConnectTimeoutMs, max_time_ms)) + "\n";
	text += "max-time = " + seconds_text(max_time_ms) + "\n";
	// The status on a line of its own after the body.
	text += "write-out = \"\\n%{http_code}\"\n";
	return text;
}

} // namespace

AwsApi::AwsApi(proc::CommandRunner &runner, AwsApiConfig config) :
		runner_(runner), config_(std::move(config)) {}

std::string AwsApi::check_config(const AwsApiConfig &config) {
	const std::string &curl = config.curl;
	if (curl.empty() || !(curl[0] == '/' || (curl.size() > 2 && curl[1] == ':')))
		return "the curl path '" + loggable(curl, 200) + "' is not absolute";
	if (config.region.empty() || config.region.size() > 32 ||
			!all_of(config.region, is_region_char))
		return "ONNET_INFRA_REGION is no region name";
	const std::string &uri = config.credentials_uri;
	if (uri.empty())
		return "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI is unset (not an ECS task with a role)";
	if (uri[0] != '/' || uri.size() > 256 || !all_of(uri, is_uri_char) ||
			uri.find("..") != std::string::npos)
		return "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI is no path the ECS agent serves";
	if (config.scratch_dir.empty())
		return "no scratch directory";
	if (config.timeout_ms <= 0)
		return "the timeout is not positive";
	return {};
}

std::string AwsApi::query_encode(const std::vector<std::pair<std::string, std::string>> &params) {
	std::string out;
	for (size_t i = 0; i < params.size(); ++i) {
		if (i != 0)
			out.push_back('&');
		append_encoded(out, params[i].first);
		out.push_back('=');
		append_encoded(out, params[i].second);
	}
	return out;
}

bool AwsApi::credentials(AwsCredentials &out, std::string &error, int timeout_ms) {
	out = AwsCredentials();
	error.clear();
	const int timeout = call_timeout(config_.timeout_ms, timeout_ms);
	if (timeout <= 0) {
		error = "the AWS read deadline was reached";
		return false;
	}
	const auto url = config_value(std::string(kCredentialsHost) + config_.credentials_uri);
	if (!url) {
		error = "the credentials path holds a control byte";
		return false;
	}
	std::string text = "url = " + *url + "\n";
	text += common_tail(false, std::min(kCredentialsMaxTimeMs, timeout));
	const AwsReply reply = run(text, {}, timeout);
	if (!reply.ok()) {
		error = reply.outcome == AwsReply::Outcome::Unreachable
				? "the ECS agent's credentials endpoint did not answer: " + reply.error
				: "the ECS agent's credentials endpoint answered " + std::to_string(reply.status);
		return false;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(reply.body, json, parse_error) || !json.is_object()) {
		error = "the ECS agent's credentials endpoint answered no JSON object";
		return false;
	}
	out.key_id = json.get_string("AccessKeyId", "");
	out.secret = json.get_string("SecretAccessKey", "");
	out.token = json.get_string("Token", "");
	if (out.key_id.empty() || out.secret.empty() || has_control_byte(out.key_id) ||
			has_control_byte(out.secret) || has_control_byte(out.token) ||
			out.key_id.find(':') != std::string::npos) {
		out = AwsCredentials();
		error = "the ECS agent's credentials endpoint answered no usable credentials";
		return false;
	}
	return true;
}

bool AwsApi::assume_role(const AwsCredentials &source, const std::string &role_arn,
		AwsCredentials &out, std::string &error, int timeout_ms) {
	const AwsCredentials credentials = source;
	out = AwsCredentials();
	error.clear();
	if (role_arn.empty() || role_arn.size() > 2048 || has_control_byte(role_arn)) {
		error = "the STS role ARN is missing or invalid";
		return false;
	}
	AwsReply reply = query_call(credentials, "sts",
			{ { "Action", "AssumeRole" }, { "Version", "2011-06-15" }, { "RoleArn", role_arn },
					{ "RoleSessionName", "opennova-costs" }, { "DurationSeconds", "900" } },
			timeout_ms);
	if (!reply.ok()) {
		// Scrub before extracting/truncating AWS's error, so even a response
		// echoing a source credential cannot expose part of it.
		const std::vector<std::string> secrets = { credentials.key_id, credentials.secret,
			credentials.token };
		reply.body = scrub(std::move(reply.body), secrets);
		reply.error = scrub(std::move(reply.error), secrets);
		error = failure("sts", reply);
		return false;
	}
	XmlNode root;
	std::string parse_error;
	if (!parse_xml(reply.body, root, parse_error) || root.name != "AssumeRoleResponse") {
		error = "sts answered no AssumeRole response";
		return false;
	}
	const XmlNode *result = only_child(root, "AssumeRoleResult");
	const XmlNode *node = result != nullptr ? only_child(*result, "Credentials") : nullptr;
	AwsCredentials assumed;
	if (node != nullptr) {
		assumed.key_id = credential_text(*node, "AccessKeyId");
		assumed.secret = credential_text(*node, "SecretAccessKey");
		assumed.token = credential_text(*node, "SessionToken");
	}
	if (assumed.key_id.empty() || assumed.secret.empty() || assumed.token.empty() ||
			has_control_byte(assumed.key_id) || has_control_byte(assumed.secret) ||
			has_control_byte(assumed.token) || assumed.key_id.find(':') != std::string::npos) {
		error = "sts answered no usable temporary credentials";
		return false;
	}
	out = std::move(assumed);
	return true;
}

AwsReply AwsApi::json_call(const AwsCredentials &credentials, std::string_view service,
		std::string_view target, const std::string &body, int timeout_ms) {
	return signed_call(credentials, service,
			std::string(service) + "." + config_.region + ".amazonaws.com", config_.region,
			"application/x-amz-json-1.1", target, body, timeout_ms);
}

AwsReply AwsApi::global_json_call(const AwsCredentials &credentials, std::string_view service,
		std::string_view host, std::string_view signing_region, std::string_view target,
		const std::string &body, int timeout_ms) {
	return signed_call(credentials, service, host, signing_region, "application/x-amz-json-1.1",
			target, body, timeout_ms);
}

AwsReply AwsApi::query_call(const AwsCredentials &credentials, std::string_view service,
		const std::vector<std::pair<std::string, std::string>> &params, int timeout_ms) {
	return signed_call(credentials, service,
			std::string(service) + "." + config_.region + ".amazonaws.com", config_.region,
			"application/x-www-form-urlencoded; charset=utf-8", {}, query_encode(params),
			timeout_ms);
}

AwsReply AwsApi::signed_call(const AwsCredentials &credentials, std::string_view service,
		std::string_view host, std::string_view region, std::string_view content_type,
		std::string_view target, const std::string &body, int timeout_ms) {
	const int timeout = call_timeout(config_.timeout_ms, timeout_ms);
	if (timeout <= 0)
		return unreachable("the AWS read deadline was reached");
	const std::string name(service);
	const auto url = config_value("https://" + std::string(host) + "/");
	const auto sigv4 = config_value("aws:amz:" + std::string(region) + ":" + name);
	const auto user = config_value(credentials.key_id + ":" + credentials.secret);
	const auto type = config_value("Content-Type: " + std::string(content_type));
	const auto amz_target = config_value("X-Amz-Target: " + std::string(target));
	const auto token = config_value("x-amz-security-token: " + credentials.token);
	const auto data = config_value(body);
	if (!url || !sigv4 || !type || !amz_target || !data)
		return unreachable(
				"the request was refused before it was sent: a value holds a control byte");
	if (!user || !token)
		return unreachable(
				"the request was refused before it was sent: the credentials hold a control byte");
	std::string text = "url = " + *url + "\n";
	text += "request = \"POST\"\n";
	text += "aws-sigv4 = " + *sigv4 + "\n";
	text += "user = " + *user + "\n";
	text += "header = " + *type + "\n";
	if (!target.empty())
		text += "header = " + *amz_target + "\n";
	if (!credentials.token.empty())
		text += "header = " + *token + "\n";
	text += "data-raw = " + *data + "\n";
	text += common_tail(true, std::min(kMaxTimeMs, timeout));
	return run(text, { credentials.secret, credentials.token, credentials.key_id }, timeout);
}

AwsReply AwsApi::run(
		const std::string &text, const std::vector<std::string> &secrets, int timeout_ms) {
	const fs::path file = fs::u8path(config_.scratch_dir) / ("aws-" + random_hex(8) + ".cfg");
	std::string error;
	std::error_code ec;
	if (!write_owner_file(file, text, true, error)) {
		fs::remove(file, ec);
		return unreachable("cannot write the request file: " + scrub(error, secrets));
	}
	const proc::CommandResult result =
			runner_.run({ config_.curl, "-q", "--config", file.u8string() }, timeout_ms);
	fs::remove(file, ec);

	switch (result.outcome) {
		case proc::CommandResult::Outcome::NotStarted:
			return unreachable(
					"curl did not start: " + loggable(scrub(result.error, secrets), 200));
		case proc::CommandResult::Outcome::TimedOut:
			return unreachable("curl timed out after " + std::to_string(timeout_ms) + " ms");
		case proc::CommandResult::Outcome::Exited:
			break;
	}
	if (result.exit_code != 0) {
		// curl's exit code (6 no such host, 7 refused, 28 timed out, 35 TLS) and
		// the first line of its message.
		std::string why = "curl exited with code " + std::to_string(result.exit_code);
		const std::string_view err = strutil::trim_view(result.err);
		const std::string first(strutil::trim_view(err.substr(0, err.find('\n'))));
		if (!first.empty())
			why += ": " + loggable(scrub(first, secrets), 300);
		return unreachable(why);
	}
	const size_t split = result.out.rfind('\n');
	const std::string code =
			split == std::string::npos ? std::string() : result.out.substr(split + 1);
	if (code.size() != 3 || !strutil::all_digits(code) || code == "000")
		return unreachable("curl's output ends with no HTTP status");
	AwsReply reply;
	reply.outcome = AwsReply::Outcome::Answered;
	reply.status = strutil::parse_int(code).value_or(0);
	reply.body = result.out.substr(0, split);
	return reply;
}

std::string AwsApi::failure(std::string_view service, const AwsReply &reply) {
	const std::string name(service);
	if (reply.outcome == AwsReply::Outcome::Unreachable)
		return name + " did not answer: " + reply.error;
	std::string out = name + " answered " + std::to_string(reply.status);
	std::string code;
	std::string message;
	const std::string_view body = strutil::trim_view(reply.body);
	if (!body.empty() && body.front() == '{') {
		io::JsonValue json;
		std::string parse_error;
		if (io::json_parse(body, json, parse_error) && json.is_object()) {
			// "__type" names the error, after a '#' in some services' replies.
			code = json.get_string("__type", "");
			if (const size_t hash = code.rfind('#'); hash != std::string::npos)
				code = code.substr(hash + 1);
			message = json.get_string("message", json.get_string("Message", ""));
		}
	} else if (!body.empty() && body.front() == '<') {
		XmlNode root;
		std::string parse_error;
		if (parse_xml(body, root, parse_error)) {
			if (const XmlNode *c = root.find("Code"))
				code = std::string(strutil::trim_view(c->text));
			if (const XmlNode *m = root.find("Message"))
				message = std::string(strutil::trim_view(m->text));
		}
	}
	if (!code.empty())
		out += ": " + loggable(code, 80);
	if (!message.empty())
		out += ": " + loggable(message, 300);
	return out;
}

} // namespace opennova::novaworld_server::infra
