#pragma once

// The projects' rows (backend/migrations/0001_baseline.sql, ADR 0069): each game's project's
// linear history of revisions (each a manifest in the object store,
// base/project/project_manifest.h) and the files its store holds. A project is its game's
// products row (product_repository.h ProductRow: every game and expansion has one, an app none;
// its key the routes' name for it, its id the store's; list_projects, find_product_by_key), made,
// changed and deleted with it, an admin's or an account's own (ProductRow::owner_id). Who may read,
// push and manage one is one rule (project_rights), every project route's. The project routes and
// the release build share both. A push moves its project's head in one compare-and-set, so two
// pushes on one parent never both win; the push that loses is told the head that won.
// Crow-free. Each function throws db::Error on a database failure. Service policy, not a port.

#include "product_repository.h"

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::projects {

// Who may read a project: anyone (public), or an admin and its owner alone (private, a retail
// game's): the products row's visibility, by these names.
enum class Visibility : uint8_t { Public, Private };
const char *visibility_name(Visibility visibility); // "public", "private"
std::optional<Visibility> visibility_from_name(std::string_view name);
// Whether `game`'s project is anyone's to read (an app has none).
bool project_is_public(const hosted::ProductRow &game);

// Who asks a project route: the account its session names (none signed out, and none for the
// Bearer ADMIN_API_TOKEN, a machine's) and whether it is an admin (an admin's session, or the
// token).
struct ProjectCaller {
	std::optional<int64_t> user_id;
	bool admin = false;
};
// What a caller may do with a project.
// Read: a public one, or any as an admin or its owner (else it is no project to the caller).
// Manage (its name, its visibility, its delete): an admin or its owner. Push: an admin's or its
// owner's own session, never the token, so every revision names its author. Release (build,
// publish, retire and delete its releases, ADR 0074): as push.
struct ProjectRights {
	bool read = false;
	bool manage = false;
	bool push = false;
	bool release = false;
};
// The one rule every project route answers by; an app's key names no project, so none of it.
ProjectRights project_rights(const hosted::ProductRow &project, const ProjectCaller &caller);

// A project_revisions row.
struct ProjectRevision {
	int64_t product_id = 0; // its game's
	int64_t number = 0; // 1, 2, ...: its parent is the number before
	std::optional<int64_t> author_id; // the pusher's (an admin, its owner), while it is there
	std::string created_by; // the pusher's name, kept
	std::string message;
	std::string manifest_sha256;
	int64_t file_count = 0;
	int64_t bytes = 0;
	std::string created_at;
};

// --- projects ---

// A product's key: 1 to 16 of A-Z, 0-9 and _ (products.key's check).
bool valid_key(std::string_view key);
// An account's own project's key (ADR 0074): a key of at most kOwnedKeyMax characters holding no
// "QQ" (without case), since its updater product is the key (updater_login KEY, command_file
// KEY.CMD): KEY.CMD fits a 1SFS name's 15 characters, and no value of its wizard holds the
// territory placeholder the updater replaces (formats/update/update_wizard.h
// substitute_territory).
inline constexpr size_t kOwnedKeyMax = 11;
bool valid_owned_key(std::string_view key);

// --- revisions ---

// A project's revisions newest first: at most `limit` of those numbered below `before` (0: from
// the head down).
std::vector<ProjectRevision> list_revisions(
		db::Database &db, int64_t product_id, int64_t before, int limit);
std::optional<ProjectRevision> find_revision(db::Database &db, int64_t product_id, int64_t number);

// --- the files the store holds ---

// The object store's keys for a project's files and manifests, named by its game's id, never by
// its key: `content/<product_id>/blobs/<sha256>` and
// `content/<product_id>/manifests/<sha256>.json`; `content/<product_id>/` holds every object of
// the project. A rename of the key moves no object,
// so a push begun under the old key lands where its revision's readers look, and a game's delete
// queues the prefix's removal (product_jobs.h product.purge).
std::string blob_key(int64_t product_id, std::string_view sha256);
std::string manifest_key(int64_t product_id, std::string_view sha256);
std::string project_prefix(int64_t product_id);

struct StoredBlob {
	std::string sha256;
	int64_t bytes = 0;
};
// Of `wanted`, the ones the project's store does not hold at that size, each once, in their first
// order (a file the store holds at another size is no file a manifest may name).
std::vector<StoredBlob> missing_blobs(
		db::Database &db, int64_t product_id, const std::vector<StoredBlob> &wanted);
// `blobs` recorded as held (each put in the store and its size checked by the caller); one held
// already is kept as it was.
void record_blobs(db::Database &db, int64_t product_id, const std::vector<StoredBlob> &blobs);

// What a project's store holds.
struct ProjectStorage {
	int64_t blobs = 0;
	int64_t bytes = 0;
};
ProjectStorage project_storage(db::Database &db, int64_t product_id);

// What the account `owner_id`'s own projects come to together: how many, and the bytes their
// stores and their releases' packages hold (product_repository.h kOwnedProjectsMax,
// kOwnedBytesMax): `bytes` the two together, `release_bytes` the packages alone (every release of
// theirs not deleted).
struct OwnedUsage {
	int64_t projects = 0;
	int64_t bytes = 0;
	int64_t release_bytes = 0;
};
OwnedUsage owned_usage(db::Database &db, int64_t owner_id);

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
CommitOutcome commit_revision(db::Database &db, int64_t product_id, int64_t parent,
		const NewRevision &revision, int64_t &head);

} // namespace opennova::novaworld_server::projects
