#include "project_urls.h"

#include "core/clock.h"
#include "s3_presign.h"

#include <base/io/base64.h>

#include <chrono>

namespace opennova::novaworld_server::projects {

namespace {

// `hex` (64 lower-case hex digits) as the base64 of its 32 bytes, x-amz-checksum-sha256's spelling.
std::string checksum_base64(const std::string &hex) {
	std::vector<uint8_t> bytes;
	bytes.reserve(hex.size() / 2);
	const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
	for (size_t i = 0; i + 1 < hex.size(); i += 2)
		bytes.push_back(static_cast<uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])));
	return io::base64_encode(bytes);
}

} // namespace

S3ProjectUrls::S3ProjectUrls(proc::CommandRunner &runner, infra::AwsApiConfig aws,
		ObjectStoreConfig store, std::function<int64_t()> now) :
		api_(runner, std::move(aws)),
		store_(std::move(store)),
		now_(now ? std::move(now) : std::function<int64_t()>(wall_now_s)) {}

bool S3ProjectUrls::sign(const char *method, const std::string &key,
		std::vector<std::pair<std::string, std::string>> headers, SignedTransfer &out,
		std::string &error, const std::string &filename) {
	const int64_t now = now_();
	infra::AwsCredentials credentials;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (credentials_at_ == 0 || now - credentials_at_ >= kCredentialsKeptS) {
			if (!api_.credentials(credentials_, error)) {
				credentials_at_ = 0;
				return false;
			}
			credentials_at_ = now;
		}
		credentials = credentials_;
	}
	PresignRequest request;
	request.method = method;
	request.host = s3_host(store_.bucket, store_.region);
	request.region = store_.region;
	request.key = object_store_key(store_, key);
	request.headers = headers;
	if (!filename.empty())
		request.response_content_disposition = "attachment; filename*=UTF-8''" + sigv4_encode(filename, false);
	request.expires_s = kExpiresS;
	request.now_unix = now;
	request.credentials = std::move(credentials);
	out.method = method;
	out.url = presign_s3_url(request);
	out.headers = std::move(headers);
	return true;
}

bool S3ProjectUrls::get(const std::string &key, SignedTransfer &out, std::string &error) {
	return sign("GET", key, {}, out, error);
}

bool S3ProjectUrls::download(const std::string &key, const std::string &filename,
		SignedTransfer &out, std::string &error) {
	return sign("GET", key, {}, out, error, filename);
}

bool S3ProjectUrls::put(const std::string &key, const std::string &sha256_hex, SignedTransfer &out,
		std::string &error) {
	return sign(
			"PUT", key, { { "x-amz-checksum-sha256", checksum_base64(sha256_hex) } }, out, error);
}

} // namespace opennova::novaworld_server::projects
