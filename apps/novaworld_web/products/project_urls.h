#pragma once

// Where a project's file moves straight between the editor and the deployment's object store
// (ADR 0069 d6): the store's own signed URLs, a GET of a file and a PUT of one whose SHA-256 the
// store itself checks, which the project routes hand out in place of carrying the bytes. S3's are
// presigned with the task role's credentials (s3_presign.h); a store with none (a directory: the
// dev stack's, the tests') has no ProjectUrls, and the routes carry the bytes themselves. Service
// policy, not a port.

#include "core/object_store.h"
#include "infra/aws_api.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::projects {

// A request the editor makes of the store as given.
struct SignedTransfer {
	std::string method;
	std::string url;
	std::vector<std::pair<std::string, std::string>> headers;
};

class ProjectUrls {
public:
	virtual ~ProjectUrls() = default;
	// A GET of the object `key` (the store's key, under its root); false with `error` when none can
	// be signed now.
	virtual bool get(const std::string &key, SignedTransfer &out, std::string &error) = 0;
	// A PUT of the object `key` whose bytes have the SHA-256 `sha256_hex`, which the store checks.
	virtual bool put(const std::string &key, const std::string &sha256_hex, SignedTransfer &out,
			std::string &error) = 0;
};

// S3's, presigned for an hour with the task role's credentials, which it keeps ten minutes.
class S3ProjectUrls : public ProjectUrls {
public:
	static constexpr int64_t kExpiresS = 3600;
	static constexpr int64_t kCredentialsKeptS = 600;

	// `runner` must outlive it; `now` the Unix clock (the system's when empty).
	S3ProjectUrls(proc::CommandRunner &runner, infra::AwsApiConfig aws, ObjectStoreConfig store,
			std::function<int64_t()> now = {});

	bool get(const std::string &key, SignedTransfer &out, std::string &error) override;
	bool put(const std::string &key, const std::string &sha256_hex, SignedTransfer &out,
			std::string &error) override;

private:
	bool sign(const char *method, const std::string &key,
			std::vector<std::pair<std::string, std::string>> headers, SignedTransfer &out,
			std::string &error);

	infra::AwsApi api_;
	ObjectStoreConfig store_;
	std::function<int64_t()> now_;
	std::mutex mu_;
	infra::AwsCredentials credentials_;
	int64_t credentials_at_ = 0; // 0: none kept
};

} // namespace opennova::novaworld_server::projects
