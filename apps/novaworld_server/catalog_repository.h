#pragma once

#include <net/novaworld/db/sqlite.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::server {

// Read-only DB queries against the games catalogue. Lives at
// apps/novaworld_server/ because it's purely HTTP-side state — the UDP
// path doesn't read these — and that keeps engine/net/novaworld focused on
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
	std::string ver1;
	std::string ver2;
};

std::vector<GameRow> list_games(opennova::db::Database &db);

} // namespace catalog

} // namespace opennova::server
