#include "s3_presign.h"

#include <base/io/sha256.h>

#include <algorithm>
#include <ctime>
#include <string_view>

namespace opennova::novaworld_server::content {

namespace {

std::string as_text(const io::Sha256Digest &digest) {
	return std::string(reinterpret_cast<const char *>(digest.data()), digest.size());
}

std::string hmac(std::string_view key, std::string_view message) {
	return as_text(io::hmac_sha256(key, message));
}

// The SHA-256 of `bytes`, in lower-case hex.
std::string sha256_of(std::string_view bytes) {
	return io::sha256_hex(bytes.data(), bytes.size());
}

// The UTC calendar time of `unix_seconds`, as SigV4 spells it: "20130524T000000Z".
std::string amz_date(int64_t unix_seconds) {
	const std::time_t t = static_cast<std::time_t>(unix_seconds);
	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &t);
#else
	gmtime_r(&t, &utc);
#endif
	char out[17];
	std::strftime(out, sizeof(out), "%Y%m%dT%H%M%SZ", &utc);
	return out;
}

} // namespace

std::string s3_host(const std::string &bucket, const std::string &region) {
	return bucket + ".s3." + region + ".amazonaws.com";
}

std::string sigv4_encode(const std::string &text, bool keep_slash) {
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(text.size() * 3);
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
				(byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
				byte == '~' || (keep_slash && byte == '/')) {
			out.push_back(c);
		} else {
			out.push_back('%');
			out.push_back(kHex[byte >> 4]);
			out.push_back(kHex[byte & 0xF]);
		}
	}
	return out;
}

std::string presign_s3_url(const PresignRequest &request) {
	const std::string date_time = amz_date(request.now_unix);
	const std::string date = date_time.substr(0, 8);
	const std::string scope = date + "/" + request.region + "/s3/aws4_request";

	// The signed headers: host and the request's own, by name.
	std::vector<std::pair<std::string, std::string>> headers = request.headers;
	headers.emplace_back("host", request.host);
	std::sort(headers.begin(), headers.end());
	std::string signed_names, canonical_headers;
	for (const auto &[name, value] : headers) {
		if (!signed_names.empty())
			signed_names += ';';
		signed_names += name;
		canonical_headers += name + ":" + value + "\n";
	}

	// The query, by name; the signature goes after it.
	std::vector<std::pair<std::string, std::string>> query = {
		{ "X-Amz-Algorithm", "AWS4-HMAC-SHA256" },
		{ "X-Amz-Credential", request.credentials.key_id + "/" + scope },
		{ "X-Amz-Date", date_time },
		{ "X-Amz-Expires", std::to_string(request.expires_s) },
		{ "X-Amz-SignedHeaders", signed_names },
	};
	if (!request.credentials.token.empty())
		query.emplace_back("X-Amz-Security-Token", request.credentials.token);
	std::sort(query.begin(), query.end());
	std::string canonical_query;
	for (const auto &[name, value] : query) {
		if (!canonical_query.empty())
			canonical_query += '&';
		canonical_query += sigv4_encode(name, false) + "=" + sigv4_encode(value, false);
	}

	const std::string uri = "/" + sigv4_encode(request.key, true);
	const std::string canonical_request = request.method + "\n" + uri + "\n" + canonical_query +
			"\n" + canonical_headers + "\n" + signed_names + "\nUNSIGNED-PAYLOAD";
	const std::string string_to_sign =
			"AWS4-HMAC-SHA256\n" + date_time + "\n" + scope + "\n" + sha256_of(canonical_request);
	std::string key = hmac("AWS4" + request.credentials.secret, date);
	key = hmac(key, request.region);
	key = hmac(key, "s3");
	key = hmac(key, "aws4_request");
	const std::string signature =
			io::sha256_hex(io::hmac_sha256(key, string_to_sign)); // the HMAC in hex
	return "https://" + request.host + uri + "?" + canonical_query +
			"&X-Amz-Signature=" + signature;
}

} // namespace opennova::novaworld_server::content
