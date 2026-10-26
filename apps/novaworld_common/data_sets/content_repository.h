#pragma once

// The content store's rows (backend/migrations/0025_content.sql, ADR 0069): each game's project's
// linear history of revisions (each a manifest in the object store,
// base/content/content_manifest.h) and the files its store holds. A project is its game's
// data_sets row (data_set_repository.h DataSetRow: a game with a key has one, the key the routes'
// name for it, the game's id the store's; list_projects, find_data_set_by_key), made, changed
// and deleted with it. The content routes and the release build share both. A push moves its
// project's head in one compare-and-set, so two pushes on one parent never both win; the push
// that loses is told the head that won.
// Crow-free. Each function throws db::Error on a database failure. Service policy, not a port.

#include "data_set_repository.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::content {

// Who may read a project: anyone (public), or an admin alone (private, a retail game's): the
// data_sets row's visibility, by these names.
enum class Visibility : uint8_t { Public, Private };
const char *visibility_name(Visibility visibility); // "public", "private"
std::optional<Visibility> visibility_from_name(std::string_view name);
// Whether `game`'s project is anyone's to read.
bool project_is_public(const hosted::DataSetRow &game);

// A content_revisions row.
struct ContentRevision {
	int64_t data_set_id = 0; // its game's
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

// A project's key: 1 to 16 of A-Z, 0-9 and _ (0025's check).
bool valid_key(std::string_view key);

// --- revisions ---

// A project's revisions newest first: at most `limit` of those numbered below `before` (0: from
// the head down).
std::vector<ContentRevision> list_revisions(
		db::Database &db, int64_t data_set_id, int64_t before, int limit);
std::optional<ContentRevision> find_revision(db::Database &db, int64_t data_set_id, int64_t number);

// --- the files the store holds ---

// The object store's keys for a project's files and manifests, named by its game's id, never by
// its key: `content/<data_set_id>/blobs/<sha256>` and
// `content/<data_set_id>/manifests/<sha256>.json`; `content/<data_set_id>/` holds every object of
// the project. A rename of the key moves no object,
// so a push begun under the old key lands where its revision's readers look, and a game's delete
// queues the prefix's removal (data_set_jobs.h game.purge).
std::string blob_key(int64_t data_set_id, std::string_view sha256);
std::string manifest_key(int64_t data_set_id, std::string_view sha256);
std::string project_prefix(int64_t data_set_id);

struct StoredBlob {
	std::string sha256;
	int64_t bytes = 0;
};
// Of `wanted`, the ones the project's store does not hold at that size, each once, in their first
// order (a file the store holds at another size is no file a manifest may name).
std::vector<StoredBlob> missing_blobs(
		db::Database &db, int64_t data_set_id, const std::vector<StoredBlob> &wanted);
// `blobs` recorded as held (each put in the store and its size checked by the caller); one held
// already is kept as it was.
void record_blobs(db::Database &db, int64_t data_set_id, const std::vector<StoredBlob> &blobs);

// What a project's store holds.
struct ProjectStorage {
	int64_t blobs = 0;
	int64_t bytes = 0;
};
ProjectStorage project_storage(db::Database &db, int64_t data_set_id);

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
	NotFound, // the game is gone, or has no project
	Stale, // the project's head is not `parent`: `head` is the one that is
};
// When the project's head is `parent`, the revision `parent + 1` written and the head moved to it,
// in one transaction; `head` the project's head afterwards (the new revision on Ok, the head that
// won on Stale). Every file the manifest lists is the caller's to have checked as held.
CommitOutcome commit_revision(db::Database &db, int64_t data_set_id, int64_t parent,
		const NewRevision &revision, int64_t &head);

} // namespace opennova::novaworld_server::content
