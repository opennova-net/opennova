#pragma once

#include <novaworld/db/sqlite.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::server {

// Read-only DB queries against the content-management tables (games,
// expansions, expansion files, release history). Lives at
// apps/novaworld_server/ because it's purely HTTP-side state — the UDP
// path doesn't read these — and that keeps libs/novaworld focused on
// protocol + connection concerns.
//
// Handlers in http_listener.cpp call into these and never write SQL
// directly; if a query needs tweaking, it's the only place to look.
namespace catalog {

struct GameRow {
	int64_t     id = 0;
	std::string slug;
	std::string display_name;
	std::string lobby_name;
	std::string gate_tag;
	std::string executable_name;
	std::string ver1;
	std::string ver2;
};

struct ExpansionRow {
	int64_t     id = 0;
	std::string slug;
	std::string display_name;
	std::string summary;
	std::string version;
	int         featured = 0;
	std::string game_slug;
	std::string package_type;
	std::string install_subdir;
	// owner/repo the release pipeline tags. Sourced from the Terraform-managed
	// catalogue seed (infra/github/local.expansions); the admin release handler
	// reads this instead of a hardcoded slug->repo map.
	std::string github_repo;
};

// A published file for an expansion (the launcher's Expansion Manager fetches
// download_url to stage the package). Mirrors onnet's public api.py files[].
struct ExpansionFileRow {
	std::string                download_url;
	std::string                sha256;
	std::optional<int64_t>     size_bytes;
	std::string                file_type;
	int                        order_index = 1;
};

struct ReleaseRow {
	int64_t     id = 0;
	std::string slug;
	std::string version;
	std::string status;
	std::string repo_ref;
	std::string workflow_url;
	std::string target_commit;
	std::string created_at;
	std::string updated_at;
	std::optional<std::string> notes;
	std::optional<std::string> published_at;
	std::optional<std::string> error_message;
};

std::vector<GameRow>          list_games(opennova::db::Database &db);
std::vector<ExpansionRow>     list_expansions(opennova::db::Database &db);
std::vector<ExpansionFileRow> list_expansion_files(opennova::db::Database &db, int64_t expansion_id);
std::vector<ReleaseRow>       list_recent_releases(opennova::db::Database &db, int limit);

// --- Write side: the expansion-release / publish pipeline ----------------
// Ported from onnet's onnw/admin.py + admin_internal.py +
// expansion_releases.py + expansions.py (Postgres -> SQLite dialect). The
// HTTP routes in http_listener.cpp are the only callers. Multi-statement
// helpers run inside a transaction so a crash can't desync the expansion
// version from its release row.

// Result of a slug lookup. `found` distinguishes "no such expansion" (404)
// from a real row. Mirrors onnet ExpansionRepository.get_by_slug.
struct ExpansionLookup {
	int64_t     id = 0;
	int64_t     game_id = 0;
	std::string version;
	std::string github_repo;
	bool        found = false;
};

ExpansionLookup find_expansion_by_slug(opennova::db::Database &db,
                                       const std::string &slug);

// onnet admin.py:252-263 (_set_expansion_version).
void set_expansion_version(opennova::db::Database &db, int64_t expansion_id,
                           const std::string &version);

// onnet expansion_releases.py:27-58 (create). Upserts a pending release;
// re-releasing the same (slug, version) resets it to pending and clears the
// tag/publish bookkeeping.
void create_or_reset_release(opennova::db::Database &db, const std::string &slug,
                             const std::string &version, const std::string &repo_ref,
                             const std::optional<std::string> &notes);

// onnet expansion_releases.py:60-94 (update_status). NULL args preserve the
// existing workflow_url / target_commit / published_at via COALESCE.
void update_release_status(opennova::db::Database &db, const std::string &slug,
                           const std::string &version, const std::string &status,
                           const std::optional<std::string> &error_message,
                           const std::optional<std::string> &workflow_url,
                           const std::optional<std::string> &target_commit,
                           const std::optional<std::string> &published_at);

// onnet expansions.py:135-159 (upsert_file). DELETE-then-INSERT by
// (expansion_id, order_index).
void upsert_expansion_file(opennova::db::Database &db, int64_t expansion_id,
                           const std::string &download_url, const std::string &sha256,
                           const std::optional<int64_t> &size_bytes,
                           const std::string &file_type = "archive",
                           int order_index = 1);

std::optional<ReleaseRow> get_release(opennova::db::Database &db,
                                      const std::string &slug, const std::string &version);

} // namespace catalog

} // namespace opennova::server
