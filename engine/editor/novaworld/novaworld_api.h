#pragma once

// NovaWorld's routes as the client speaks them (ADR 0069; the service's side is
// apps/novaworld_web/products/project_routes.h): each route's request built for the account it acts
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

#include <base/project/project_manifest.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/novaworld/novaworld_transport.h>

namespace opennova::editor {

// Why an exchange failed: the HTTP status (0 when no reply came), the service's error code
// ("stale", "not_found", "transport") and its message.
struct NovaWorldApiError {
	int status = 0;
	std::string code;
	std::string message;

	// One line for Output: "<message> (<code>, HTTP <status>)".
	std::string line() const;
};

// A NovaWorld project as the service lists it.
struct RemoteProject {
	std::string key;
	std::string title;
	std::string kind; // "game" or "expansion"
	std::string game; // the gameprofile code it runs as ("jo"), an expansion's its base game's
	std::string expansion_code; // an expansion's ("jox01"), "" for a game
	std::string owner; // the account whose project it is, "" for the service's own (an admin's)
	bool is_public = false;
	bool retail = false;
	std::string base; // its base game's project's key, "" for none
	int64_t head = 0; // its newest revision, 0 before the first push
	bool can_push = false; // the account may push it: its owner, or an admin
	bool can_manage = false; // the account may rename it, make it public or private, or delete it
};

// A new project of the account's, as POST /api/products asks for one: its key and its name, a game
// (`game` the gameprofile code it runs as) or an expansion (`base` its base game's project's key,
// `expansion_code` its folder's name), public or private ("public", "private").
struct NewRemoteProject {
	std::string key;
	std::string title;
	std::string kind;
	std::string game;
	std::string base;
	std::string expansion_code;
	std::string visibility;
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
	project::ProjectFile file;
	NovaWorldRequest request;
};

namespace novaworld_api {

// POST /api/login.
NovaWorldRequest login(
		const std::string &server, const std::string &username, const std::string &password);
// The account a login's reply signs in (its cookie, name and role) over `account`'s server.
bool read_login(const NovaWorldResponse &reply, NovaWorldAccount &account, NovaWorldApiError &error);
// POST /api/logout.
NovaWorldRequest logout(const NovaWorldAccount &account);

// The session cookie a reply renews (Set-Cookie on the service's cookie name), "" when it renews
// none; a reply that deletes it (Max-Age=0, an empty value) says so by `deleted`.
std::string renewed_cookie(const NovaWorldResponse &reply, bool &deleted);

// GET /api/products.
NovaWorldRequest projects(const NovaWorldAccount &account);
bool read_projects(
		const NovaWorldResponse &reply, std::vector<RemoteProject> &out, NovaWorldApiError &error);
// GET /api/products/<key>: 404 for a project the service does not hold or does not show the
// account.
NovaWorldRequest project(const NovaWorldAccount &account, const std::string &key);
bool read_project(const NovaWorldResponse &reply, RemoteProject &out,
		std::optional<RemoteRevision> &head, NovaWorldApiError &error);
// POST /api/products: a new project of the account's (its owner), with no revision yet.
NovaWorldRequest create_project(const NovaWorldAccount &account, const NewRemoteProject &project);
// The reply (201 {project}): the project made. A refusal's code (invalid_field, missing_field,
// no_such_base, duplicate_key, duplicate_expansion, too_many_projects) is the error's.
bool read_created(const NovaWorldResponse &reply, RemoteProject &out, NovaWorldApiError &error);
// GET /api/products/<key>/revisions?before=&limit=.
NovaWorldRequest revisions(
		const NovaWorldAccount &account, const std::string &key, int64_t before, int limit);
bool read_revisions(
		const NovaWorldResponse &reply, std::vector<RemoteRevision> &out, NovaWorldApiError &error);
// GET /api/products/<key>/revisions/<n>/manifest: its files, the manifest's text checked
// against `manifest_sha256` (the revision's) when one is given.
NovaWorldRequest manifest(const NovaWorldAccount &account, const std::string &key, int64_t number);
bool read_manifest(const NovaWorldResponse &reply, const std::string &manifest_sha256,
		std::vector<project::ProjectFile> &out, NovaWorldApiError &error);
// GET /api/products/<key>/blobs/<sha256> into `file`, checked by the SHA-256 and the size
// the transport counted as it wrote.
NovaWorldRequest blob(const NovaWorldAccount &account, const std::string &key,
		const std::string &sha256, const std::string &file);
bool read_blob(
		const NovaWorldResponse &reply, const project::ProjectFile &expected, NovaWorldApiError &error);

// POST /api/products/<key>/downloads: each of `sha256s` (at most 1000) with the request
// that downloads it (the store's own signed URL, or the service's blob route, which the account's
// session goes to alone), keyed by SHA-256; each request's response_file the caller's.
NovaWorldRequest downloads(const NovaWorldAccount &account, const std::string &key,
		const std::vector<std::string> &sha256s);
bool read_downloads(const NovaWorldResponse &reply, const NovaWorldAccount &account,
		std::map<std::string, NovaWorldRequest> &out, NovaWorldApiError &error);

// POST /api/products/<key>/missing: the files of `files` the store lacks, each with its
// upload.
NovaWorldRequest missing(const NovaWorldAccount &account, const std::string &key,
		const std::vector<project::ProjectFile> &files);
bool read_missing(const NovaWorldResponse &reply, const NovaWorldAccount &account,
		std::vector<UploadTarget> &out, NovaWorldApiError &error);
// An upload's request with its body the file at `file`.
NovaWorldRequest upload(
		const NovaWorldAccount &account, const UploadTarget &target, const std::string &file);
bool read_upload(const NovaWorldResponse &reply, NovaWorldApiError &error);

// POST /api/products/<key>/revisions.
NovaWorldRequest push(const NovaWorldAccount &account, const std::string &key, int64_t parent,
		const std::string &message, const std::string &manifest_text);
enum class PushAnswer : uint8_t {
	Pushed, // `head` the new revision
	Stale, // another push won: `head` is the head now
	MissingBlobs, // the store lacks files the manifest lists: `hashes`
	Refused, // anything else: `error`
};
PushAnswer read_push(const NovaWorldResponse &reply, int64_t &head, std::vector<std::string> &hashes,
		NovaWorldApiError &error);

// POST /api/admin/products/<key>/releases {version, revision}: a release the service builds
// from `revision` (an expansion's on its base game's newest), an admin's (ADR 0069 d7).
NovaWorldRequest release(const NovaWorldAccount &account, const std::string &key,
		const std::string &version, int64_t revision);
// The reply: the release's id and name ("ACIDGAMBIT 0.2.0"), building.
bool read_release(const NovaWorldResponse &reply, int64_t &id, std::string &name, NovaWorldApiError &error);

} // namespace novaworld_api

} // namespace opennova::editor
