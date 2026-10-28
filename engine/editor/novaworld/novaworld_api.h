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
// ("stale", "not_found", "transport"), its message, and the request's field it names ("" none).
struct NovaWorldApiError {
	int status = 0;
	std::string code;
	std::string message;
	std::string field;

	// One line for Output: "<message> (<code>, HTTP <status>)".
	std::string line() const;
};

// A size in a line's words: "3.9 GiB", "120.5 MiB", "12 KiB", "512 bytes".
std::string novaworld_size_words(int64_t bytes);

// A lobby offered by the service. The encoded file is supplied only with a project's
// resolved lobby; the editor copies those bytes, never derives a gate tag itself.
struct RemoteLobby {
	std::string slug;
	std::string display_name;
	std::string title;
	std::string gate_tag_file; // base64 encoded gt.ssc
};

// A NovaWorld project as the service lists it.
struct RemoteProject {
	std::optional<RemoteLobby> lobby;
	bool lobby_locked = false;
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
	bool can_release = false; // the account may build and publish its releases: its owner, or an admin
	// Its published release, the one players get: its id (0 for none) and version ("0.2.0").
	int64_t release_id = 0;
	std::string release_version;
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
	std::string lobby;
	std::string new_lobby_name;
};

// A game a new project of the account's may be a mod of (GET /api/me/projects' games): its
// project's key, its name and the gameprofile code it runs as.
struct RemoteGame {
	std::string key;
	std::string title;
	std::string game;
};

// A release of a project as the service lists it (GET /api/products/<key>/releases): its id, name and
// version, its status ("building", "validating", "invalid", "draft", "awaiting_approval", "published",
// "retired"), an admin's note on a draft it sent back (rejected) and what failed an invalid one.
struct RemoteRelease {
	int64_t id = 0;
	std::string name;
	std::string version;
	std::string status;
	std::string review_note;
	std::string error;
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
// no_such_base, duplicate_key, duplicate_expansion, too_many_projects, rate_limited) is the
// error's.
bool read_created(const NovaWorldResponse &reply, RemoteProject &out, NovaWorldApiError &error);
// GET /api/me/projects, a signed-in account's: the games a new project of its may be a mod of (its
// projects, their usage and their limits are the site's to show).
NovaWorldRequest my_projects(const NovaWorldAccount &account);
bool read_my_games(
		const NovaWorldResponse &reply, std::vector<RemoteGame> &out, NovaWorldApiError &error);
bool read_my_lobbies(
		const NovaWorldResponse &reply, std::vector<RemoteLobby> &out, NovaWorldApiError &error);
// The same reply's titles a new game of the account's may run as (gameprofile codes: "jo", "dfx2").
bool read_my_titles(
		const NovaWorldResponse &reply, std::vector<std::string> &out, NovaWorldApiError &error);
// Why `what` ("push", "release") would pass the account's storage (413 quota_exceeded {used, limit}:
// a push's files it asks to upload or an upload, a release's package), in a line's words; "" for any
// other reply.
std::string quota_text(const NovaWorldResponse &reply, const char *what);
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

// POST /api/products/<key>/releases {version, revision}: a release the service builds from
// `revision` (an expansion's on its base game's newest), its owner's or an admin's (the project's
// can_release; ADR 0069 d7, ADR 0074).
NovaWorldRequest release(const NovaWorldAccount &account, const std::string &key,
		const std::string &version, int64_t revision);
// GET /api/products/<key>/releases, its owner's or an admin's: the project's releases, newest first.
NovaWorldRequest releases(const NovaWorldAccount &account, const std::string &key);
bool read_releases(
		const NovaWorldResponse &reply, std::vector<RemoteRelease> &out, NovaWorldApiError &error);
// POST /api/products/<key>/releases/<id>/publish: a draft published (an admin's) or submitted for an
// admin's approval (an owner's: awaiting_approval); the reply (202 {release}) read into `out`.
NovaWorldRequest submit_release(const NovaWorldAccount &account, const std::string &key, int64_t id);
bool read_submitted(const NovaWorldResponse &reply, RemoteRelease &out, NovaWorldApiError &error);
// The reply (201 {release}): the release's id and name ("ACIDGAMBIT 0.2.0"), building. A refusal's
// code (forbidden, release_in_progress, too_many_releases, quota_exceeded, no_revision) is the
// error's.
bool read_release(const NovaWorldResponse &reply, int64_t &id, std::string &name, NovaWorldApiError &error);

} // namespace novaworld_api

} // namespace opennova::editor
