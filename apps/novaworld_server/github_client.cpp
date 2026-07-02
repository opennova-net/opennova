#include "github_client.h"

#include <curl/curl.h>
#include <crow/json.h>

#include <string>

namespace opennova::server::github {

namespace {

constexpr const char *kApiBase  = "https://api.github.com";
constexpr const char *kUserAgent = "opennova-novaworld";

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto *out = static_cast<std::string *>(userdata);
	out->append(ptr, size * nmemb);
	return size * nmemb;
}

// Pull object.sha out of a GitHub git-ref response body. Returns "" if
// absent. onnet read this as resp.json().get("object", {}).get("sha").
std::string ref_object_sha(const std::string &body) {
	auto json = crow::json::load(body);
	if (!json || !json.has("object")) return "";
	const auto &obj = json["object"];
	if (!obj.has("sha")) return "";
	return std::string(obj["sha"].s());
}

} // namespace

HttpResult request(const std::string &method, const std::string &url,
                   const std::string &token, const std::string &json_body) {
	HttpResult result;

	CURL *curl = curl_easy_init();
	if (!curl) {
		// Mirrors onnet's URLError path: transport failure -> status 0.
		result.status = 0;
		result.body   = "curl_easy_init failed";
		return result;
	}

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
	const std::string auth = "Authorization: Bearer " + token;
	headers = curl_slist_append(headers, auth.c_str());
	if (!json_body.empty())
		headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);          // onnet urlopen(timeout=15)
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	// Default TLS verification stays ON (the runtime image ships ca-certificates).
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	if (!json_body.empty())
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());

	const CURLcode rc = curl_easy_perform(curl);
	if (rc == CURLE_OK) {
		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		result.status = code;
	} else {
		// Transport error: keep onnet's status-0 contract, surface the reason.
		result.status = 0;
		if (result.body.empty()) result.body = curl_easy_strerror(rc);
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return result;
}

TagResult create_git_tag(const std::string &repo, const std::string &repo_ref,
                         const std::string &token) {
	// onnet admin.py:70-111.
	TagResult out;
	if (token.empty() || repo.empty()) {
		out.message = "GitHub token or repository mapping missing";
		return out;
	}

	const std::string repo_api = std::string(kApiBase) + "/repos/" + repo;

	// 1. Resolve the default branch.
	HttpResult repo_resp = request("GET", repo_api, token, "");
	if (repo_resp.status != 200) {
		out.message = "Failed to fetch repository info: " + repo_resp.body;
		return out;
	}
	std::string default_branch = "main";
	if (auto j = crow::json::load(repo_resp.body); j && j.has("default_branch"))
		default_branch = std::string(j["default_branch"].s());

	// 2. Resolve the default branch head SHA.
	HttpResult ref_resp =
		request("GET", repo_api + "/git/ref/heads/" + default_branch, token, "");
	if (ref_resp.status != 200) {
		out.message = "Failed to resolve default branch: " + ref_resp.body;
		return out;
	}
	const std::string target_sha = ref_object_sha(ref_resp.body);
	if (target_sha.empty()) {
		out.message = "Unable to determine target commit";
		return out;
	}

	// 3. Create the tag ref.
	crow::json::wvalue payload;
	payload["ref"] = "refs/tags/" + repo_ref;
	payload["sha"] = target_sha;
	HttpResult create_resp =
		request("POST", repo_api + "/git/refs", token, payload.dump());

	if (create_resp.status == 201) {
		out.success       = true;
		out.message       = "Tag created";
		out.target_commit = target_sha;
		return out;
	}
	if (create_resp.status == 422 &&
	    create_resp.body.find("Reference already exists") != std::string::npos) {
		// Tag already exists; treat as success, fetch the existing SHA.
		out.success = true;
		out.message = "Tag already existed";
		HttpResult tag_resp =
			request("GET", repo_api + "/git/ref/tags/" + repo_ref, token, "");
		if (tag_resp.status == 200) {
			std::string sha = ref_object_sha(tag_resp.body);
			if (!sha.empty()) out.target_commit = sha;
		}
		return out;
	}

	out.message = "Failed to create tag: " + create_resp.body;
	return out;
}

} // namespace opennova::server::github
