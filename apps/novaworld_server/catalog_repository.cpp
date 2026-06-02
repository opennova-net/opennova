#include "catalog_repository.h"

namespace opennova::server::catalog {

namespace {

opennova::db::BindValue i64(int64_t v) { return opennova::db::BindValue(v); }

GameRow row_to_game(const opennova::db::Row &r) {
	GameRow g;
	g.id              = r.as_int(0).value_or(0);
	g.slug            = r.as_text(1).value_or("");
	g.display_name    = r.as_text(2).value_or("");
	g.lobby_name      = r.as_text(3).value_or("");
	g.gate_tag        = r.as_text(4).value_or("");
	g.executable_name = r.as_text(5).value_or("");
	g.ver1            = r.as_text(6).value_or("");
	g.ver2            = r.as_text(7).value_or("");
	return g;
}

ExpansionRow row_to_expansion(const opennova::db::Row &r) {
	ExpansionRow e;
	e.id           = r.as_int(0).value_or(0);
	e.slug         = r.as_text(1).value_or("");
	e.display_name = r.as_text(2).value_or("");
	e.summary      = r.as_text(3).value_or("");
	e.version      = r.as_text(4).value_or("");
	e.featured     = static_cast<int>(r.as_int(5).value_or(0));
	e.game_slug    = r.as_text(6).value_or("");
	return e;
}

ReleaseRow row_to_release(const opennova::db::Row &r) {
	ReleaseRow rel;
	rel.id            = r.as_int(0).value_or(0);
	rel.slug          = r.as_text(1).value_or("");
	rel.version       = r.as_text(2).value_or("");
	rel.status        = r.as_text(3).value_or("");
	rel.repo_ref      = r.as_text(4).value_or("");
	rel.workflow_url  = r.as_text(5).value_or("");
	rel.target_commit = r.as_text(6).value_or("");
	rel.created_at    = r.as_text(7).value_or("");
	if (auto v = r.as_text(8); v && !v->empty()) rel.published_at  = *v;
	if (auto v = r.as_text(9); v && !v->empty()) rel.error_message = *v;
	return rel;
}

} // namespace

std::vector<GameRow> list_games(opennova::db::Database &db) {
	auto rows = db.query(
		"SELECT id, slug, display_name, lobby_name, gate_tag, "
		"       executable_name, ver1, ver2 "
		"FROM games ORDER BY slug;");
	std::vector<GameRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_game(r));
	return out;
}

std::vector<ExpansionRow> list_expansions(opennova::db::Database &db) {
	auto rows = db.query(
		"SELECT e.id, e.slug, e.display_name, e.summary, e.version, "
		"       e.featured, g.slug AS game_slug "
		"FROM expansions e JOIN games g ON g.id = e.game_id "
		"ORDER BY e.featured DESC, e.slug;");
	std::vector<ExpansionRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_expansion(r));
	return out;
}

std::vector<ReleaseRow> list_recent_releases(opennova::db::Database &db, int limit) {
	if (limit < 1) limit = 20;
	if (limit > 100) limit = 100;
	auto rows = db.query(
		"SELECT id, slug, version, status, repo_ref, "
		"       workflow_url, target_commit, created_at, "
		"       published_at, error_message "
		"FROM expansion_releases ORDER BY created_at DESC LIMIT ?;",
		{i64(limit)});
	std::vector<ReleaseRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_release(r));
	return out;
}

} // namespace opennova::server::catalog
