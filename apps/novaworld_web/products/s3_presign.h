#pragma once

// Presigned S3 URLs (AWS Signature Version 4, query-string authentication): what the content
// store hands the editor so a file moves between it and the deployment's bucket with no byte
// through web (ADR 0069 d6). A GET's URL is the object's, its credential and signature in the query
// string; a PUT's also signs the headers its uploader must send as given (the object's
// x-amz-checksum-sha256, so S3 itself refuses bytes that are not the file named). A pure function
// of its inputs, the clock among them: the credentials are the caller's (the task role's,
// infra/aws_api.h). Platform plumbing, not a port.

#include "infra/aws_api.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::content {

struct PresignRequest {
	std::string method; // "GET" or "PUT"
	std::string host; // the bucket's virtual host (s3_host)
	std::string region; // the bucket's region: what the credential's scope names
	std::string key; // the object's full key under the bucket, no leading '/'
	// Headers the uploader sends and the signature covers, beside host: lower-case names.
	std::vector<std::pair<std::string, std::string>> headers;
	int64_t expires_s = 900; // how long the URL is good for, 1 to 604800
	int64_t now_unix = 0; // the signing time, Unix seconds (UTC)
	infra::AwsCredentials credentials;
};

// The bucket's virtual-hosted address: <bucket>.s3.<region>.amazonaws.com.
std::string s3_host(const std::string &bucket, const std::string &region);

// The presigned URL: https://<host>/<key>?X-Amz-Algorithm=...&X-Amz-Signature=<hex>.
std::string presign_s3_url(const PresignRequest &request);

// `text` percent-encoded as SigV4 asks: every byte but A-Z a-z 0-9 - . _ ~ as %XX (upper-case
// hex), and '/' kept when `keep_slash` (an object key's path).
std::string sigv4_encode(const std::string &text, bool keep_slash);

} // namespace opennova::novaworld_server::content
