#include "catalog_repository.h"

namespace opennova::novaworld_server::catalog {

namespace {

GameRow row_to_game(const opennova::db::Row &r) {
	GameRow g;
	g.id           = r.as_int(0).value_or(0);
	g.slug         = r.as_text(1).value_or("");
	g.display_name = r.as_text(2).value_or("");
	g.lobby_name   = r.as_text(3).value_or("");
	g.gate_tag     = r.as_text(4).value_or("");
	g.ver1         = r.as_text(5).value_or("");
	g.ver2         = r.as_text(6).value_or("");
	return g;
}

} // namespace

std::vector<GameRow> list_games(opennova::db::Database &db) {
	auto rows = db.query(
		"SELECT id, slug, display_name, lobby_name, gate_tag, ver1, ver2 "
		"FROM games ORDER BY slug;");
	std::vector<GameRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_game(r));
	return out;
}

} // namespace opennova::novaworld_server::catalog
