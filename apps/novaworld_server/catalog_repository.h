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
	std::optional<std::string> published_at;
	std::optional<std::string> error_message;
};

std::vector<GameRow>      list_games(opennova::db::Database &db);
std::vector<ExpansionRow> list_expansions(opennova::db::Database &db);
std::vector<ReleaseRow>   list_recent_releases(opennova::db::Database &db, int limit);

} // namespace catalog

} // namespace opennova::server
