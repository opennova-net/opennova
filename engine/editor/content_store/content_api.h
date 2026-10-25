#pragma once

// The content store's routes as the client speaks them (ADR 0069; the service's side is
// apps/novaworld_web/content/content_routes.h): each route's request built for the account it acts
// as, and its reply read into typed answers or an error (the service's code and message, or the
// transport's). A signed-in request carries the session cookie, and a state change the CSRF header
// the website's own pages send (X-OpenNova-Request: 1); an upload to a URL the service handed out
// for its object store carries the headers it named and no cookie. A reply that renews the session
// (Set-Cookie on the service's cookie) is read with renewed_cookie, so the client keeps it.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <base/content/content_manifest.h>
#include <editor/content_store/content_account.h>
#include <editor/content_store/content_transport.h>

namespace opennova::editor {

// Why an exchange failed: the HTTP status (0 when no reply came), the service's error code
// ("stale", "not_found", "transport") and its message.
struct ContentApiError {
	int status = 0;
	std::string code;
	std::string message;

	// One line for Output: "<message> (<code>, HTTP <status>)".
	std::string line() const;
};

// A content project as the service lists it.
struct RemoteProject {
	std::string key;
	std::string title;
	bool is_public = false;
	bool retail = false;
	std::string base; // its base game's project's key, "" for none
	int64_t head = 0; // its newest revision, 0 before the first push
	bool can_push = false; // the account may push it
};

// A revision as the service lists it.
struct RemoteRevision {
	int64_t number = 0;
	std::string author;
	std::string message;
	std::string manifest_sha256;
	int64_t file_count = 0;
	int64_t bytes = 0;
	std::string created_at;
};

// A file the service's store lacks, with the request that uploads it (its body_file the caller's).
struct UploadTarget {
	content::ContentFile file;
	ContentRequest request;
};

namespace content_api {

// POST /api/login.
ContentRequest login(
		const std::string &server, const std::string &username, const std::string &password);
// The account a login's reply signs in (its cookie, name and role) over `account`'s server.
bool read_login(const ContentResponse &reply, ContentAccount &account, ContentApiError &error);
// POST /api/logout.
ContentRequest logout(const ContentAccount &account);

// The session cookie a reply renews (Set-Cookie on the service's cookie name), "" when it renews
// none; a reply that deletes it (Max-Age=0, an empty value) says so by `deleted`.
std::string renewed_cookie(const ContentResponse &reply, bool &deleted);

// GET /api/content/projects.
ContentRequest projects(const ContentAccount &account);
bool read_projects(
		const ContentResponse &reply, std::vector<RemoteProject> &out, ContentApiError &error);
// GET /api/content/projects/<key>.
ContentRequest project(const ContentAccount &account, const std::string &key);
bool read_project(const ContentResponse &reply, RemoteProject &out,
		std::optional<RemoteRevision> &head, ContentApiError &error);
// GET /api/content/projects/<key>/revisions?before=&limit=.
ContentRequest revisions(
		const ContentAccount &account, const std::string &key, int64_t before, int limit);
bool read_revisions(
		const ContentResponse &reply, std::vector<RemoteRevision> &out, ContentApiError &error);
// GET /api/content/projects/<key>/revisions/<n>/manifest: its files, the manifest's text checked
// against `manifest_sha256` (the revision's) when one is given.
ContentRequest manifest(const ContentAccount &account, const std::string &key, int64_t number);
bool read_manifest(const ContentResponse &reply, const std::string &manifest_sha256,
		std::vector<content::ContentFile> &out, ContentApiError &error);
// GET /api/content/projects/<key>/blobs/<sha256> into `file`, checked by the SHA-256 and the size
// the transport counted as it wrote.
ContentRequest blob(const ContentAccount &account, const std::string &key,
		const std::string &sha256, const std::string &file);
bool read_blob(
		const ContentResponse &reply, const content::ContentFile &expected, ContentApiError &error);

// POST /api/content/projects/<key>/downloads: each of `sha256s` (at most 1000) with the request
// that downloads it (the store's own signed URL, or the service's blob route, which the account's
// session goes to alone), keyed by SHA-256; each request's response_file the caller's.
ContentRequest downloads(const ContentAccount &account, const std::string &key,
		const std::vector<std::string> &sha256s);
bool read_downloads(const ContentResponse &reply, const ContentAccount &account,
		std::map<std::string, ContentRequest> &out, ContentApiError &error);

// POST /api/content/projects/<key>/missing: the files of `files` the store lacks, each with its
// upload.
ContentRequest missing(const ContentAccount &account, const std::string &key,
		const std::vector<content::ContentFile> &files);
bool read_missing(const ContentResponse &reply, const ContentAccount &account,
		std::vector<UploadTarget> &out, ContentApiError &error);
// An upload's request with its body the file at `file`.
ContentRequest upload(
		const ContentAccount &account, const UploadTarget &target, const std::string &file);
bool read_upload(const ContentResponse &reply, ContentApiError &error);

// POST /api/content/projects/<key>/revisions.
ContentRequest push(const ContentAccount &account, const std::string &key, int64_t parent,
		const std::string &message, const std::string &manifest_text);
enum class PushAnswer : uint8_t {
	Pushed, // `head` the new revision
	Stale, // another push won: `head` is the head now
	MissingBlobs, // the store lacks files the manifest lists: `hashes`
	Refused, // anything else: `error`
};
PushAnswer read_push(const ContentResponse &reply, int64_t &head, std::vector<std::string> &hashes,
		ContentApiError &error);

} // namespace content_api

} // namespace opennova::editor
