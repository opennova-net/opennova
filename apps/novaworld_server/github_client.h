#pragma once

#include <optional>
#include <string>

namespace opennova::server::github {

// Minimal GitHub REST client used by the expansion-release route to push a
// git tag onto an expansion's repository. Ported from onnet's
// onnw/admin.py:_github_request (:40-67) and _create_git_tag (:70-111),
// which used Python urllib; here it's libcurl (the server has no other
// outbound HTTP client). Lives at apps/novaworld_server/ — never in libs/ —
// so libcurl stays out of the portable core and only links when
// BUILD_NOVAWORLD_HTTP is ON.
//
// <curl/curl.h> is deliberately confined to the .cpp.

// Raw HTTP result. `status` is the HTTP status code, or 0 on a transport
// error (DNS/connect/TLS/timeout) — mirroring onnet's URLError -> status 0.
struct HttpResult {
	long        status = 0;
	std::string body;
};

// One authenticated GitHub API call. `method` is "GET"/"POST"; `json_body`
// is sent verbatim (with Content-Type: application/json) when non-empty.
HttpResult request(const std::string &method, const std::string &url,
                   const std::string &token, const std::string &json_body);

// Outcome of create_git_tag. `target_commit` is the SHA the tag points at
// (filled on success when known).
struct TagResult {
	bool                       success = false;
	std::string                message;
	std::optional<std::string> target_commit;
};

// Create refs/tags/<repo_ref> on `repo` (e.g. "opennova-net/revx02"),
// pointing at the tip of the default branch. onnet admin.py:70-111:
// resolves default_branch, then its head SHA, then POSTs the tag ref. A
// 422 "Reference already exists" is treated as success.
TagResult create_git_tag(const std::string &repo, const std::string &repo_ref,
                         const std::string &token);

} // namespace opennova::server::github
