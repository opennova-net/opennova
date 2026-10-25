#pragma once

// The content store's rows (backend/migrations/0023_content.sql, ADR 0069): the content
// projects (one per product, its key the store's and the routes' name for it), each one's
// linear history of revisions (each a manifest in the object store,
// base/content/content_manifest.h), and the files its store holds. The content routes and the
// release build share it. A push moves its project's head in one compare-and-set, so two pushes
// on one parent never both win; the push that loses is told the head that won. Crow-free. Each
// function throws db::Error on a database failure. Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::content {

// Who may read a project: anyone (public), or an admin alone (private, a retail game's).
enum class Visibility : uint8_t { Public, Private };
const char *visibility_name(Visibility visibility); // "public", "private"
std::optional<Visibility> visibility_from_name(std::string_view name);

// A content_projects row.
struct ContentProject {
	int64_t id = 0;
	std::string key; // "JO", "JOX01"
	std::string title;
	Visibility visibility = Visibility::Private;
	bool retail = false; // a stock game's import: revision 1 is the stock install, imported afresh
	std::optional<int64_t> data_set_id; // the data set a release built from it updates
	std::optional<int64_t> base_project_id; // an expansion's base game's project
	int64_t head = 0; // the newest revision's number, 0 before the first push
	std::string created_by;
	std::string created_at;
	std::string updated_at;
};

// A content_revisions row.
struct ContentRevision {
	int64_t project_id = 0;
	int64_t number = 0; // 1, 2, ...: its parent is the number before
	std::optional<int64_t> author_id; // the admin's account, while it is there
	std::string created_by; // the admin's name, kept
	std::string message;
	std::string manifest_sha256;
	int64_t file_count = 0;
	int64_t bytes = 0;
	std::string created_at;
};

// --- projects ---

struct NewProject {
	std::string key;
	std::string title;
	Visibility visibility = Visibility::Private;
	bool retail = false;
	std::optional<int64_t> data_set_id;
	std::optional<int64_t> base_project_id;
	std::string created_by;
};

// What a project write came to.
enum class ProjectOutcome : uint8_t {
	Ok,
	NotFound,
	DuplicateKey, // another project has the key
	DataSetTaken, // another project updates the data set
	NoSuchDataSet,
	NoSuchBase, // the base project is not there
	InUse, // an expansion's project has it as its base
};

// A new row; `id` its id on Ok. The key's and title's shapes are the routes' to check: here only
// the database's own rules answer.
ProjectOutcome create_project(db::Database &db, const NewProject &project, int64_t &id);

// What an admin may change afterwards.
struct ProjectPatch {
	std::optional<std::string> title;
	std::optional<Visibility> visibility;
};
ProjectOutcome update_project(db::Database &db, int64_t id, const ProjectPatch &patch);
// The row deleted with its revisions' and files' rows (their objects in the store are the
// caller's to remove); InUse while an expansion's project has it as its base.
ProjectOutcome delete_project(db::Database &db, int64_t id);

// Every project by key, the private ones only when `include_private`.
std::vector<ContentProject> list_projects(db::Database &db, bool include_private);
std::optional<ContentProject> find_project(db::Database &db, std::string_view key);
std::optional<ContentProject> find_project_by_id(db::Database &db, int64_t id);

// --- revisions ---

// A project's revisions newest first: at most `limit` of those numbered below `before` (0: from
// the head down).
std::vector<ContentRevision> list_revisions(
		db::Database &db, int64_t project_id, int64_t before, int limit);
std::optional<ContentRevision> find_revision(db::Database &db, int64_t project_id, int64_t number);

// --- the files the store holds ---

// The object store's keys for a project's files and manifests: `content/<key>/blobs/<sha256>` and
// `content/<key>/manifests/<sha256>.json`; `content/<key>/` holds every object of the project.
std::string blob_key(std::string_view project_key, std::string_view sha256);
std::string manifest_key(std::string_view project_key, std::string_view sha256);
std::string project_prefix(std::string_view project_key);

struct StoredBlob {
	std::string sha256;
	int64_t bytes = 0;
};
// Of `wanted`, the ones the project's store does not hold at that size, each once, in their first
// order (a file the store holds at another size is no file a manifest may name).
std::vector<StoredBlob> missing_blobs(
		db::Database &db, int64_t project_id, const std::vector<StoredBlob> &wanted);
// `blobs` recorded as held (each put in the store and its size checked by the caller); one held
// already is kept as it was.
void record_blobs(db::Database &db, int64_t project_id, const std::vector<StoredBlob> &blobs);

// What a project's store holds.
struct ProjectStorage {
	int64_t blobs = 0;
	int64_t bytes = 0;
};
ProjectStorage project_storage(db::Database &db, int64_t project_id);

// --- the push ---

struct NewRevision {
	std::optional<int64_t> author_id;
	std::string created_by;
	std::string message;
	std::string manifest_sha256;
	int64_t file_count = 0;
	int64_t bytes = 0;
};
enum class CommitOutcome : uint8_t {
	Ok, // the revision is `head` (parent + 1)
	NotFound,
	Stale, // the project's head is not `parent`: `head` is the one that is
};
// When the project's head is `parent`, the revision `parent + 1` written and the head moved to it,
// in one transaction; `head` the project's head afterwards (the new revision on Ok, the head that
// won on Stale). Every file the manifest lists is the caller's to have checked as held.
CommitOutcome commit_revision(db::Database &db, int64_t project_id, int64_t parent,
		const NewRevision &revision, int64_t &head);

} // namespace opennova::novaworld_server::content
