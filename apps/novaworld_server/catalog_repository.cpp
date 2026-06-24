#include "catalog_repository.h"

namespace opennova::server::catalog {

namespace {

opennova::db::BindValue i64(int64_t v) { return opennova::db::BindValue(v); }

// Bind a std::string by value.
opennova::db::BindValue txt(const std::string &v) { return opennova::db::BindValue(v); }

// Bind an optional text column: NULL (monostate) when absent, else the text.
opennova::db::BindValue opt_txt(const std::optional<std::string> &v) {
	if (v) return opennova::db::BindValue(*v);
	return opennova::db::BindValue(std::monostate{});
}

opennova::db::BindValue opt_i64(const std::optional<int64_t> &v) {
	if (v) return opennova::db::BindValue(*v);
	return opennova::db::BindValue(std::monostate{});
}

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
	e.id             = r.as_int(0).value_or(0);
	e.slug           = r.as_text(1).value_or("");
	e.display_name   = r.as_text(2).value_or("");
	e.summary        = r.as_text(3).value_or("");
	e.version        = r.as_text(4).value_or("");
	e.featured       = static_cast<int>(r.as_int(5).value_or(0));
	e.game_slug      = r.as_text(6).value_or("");
	e.package_type   = r.as_text(7).value_or("");
	e.install_subdir = r.as_text(8).value_or("");
	e.github_repo    = r.as_text(9).value_or("");
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
	rel.updated_at    = r.as_text(8).value_or("");
	if (auto v = r.as_text(9);  v && !v->empty()) rel.notes         = *v;
	if (auto v = r.as_text(10); v && !v->empty()) rel.published_at  = *v;
	if (auto v = r.as_text(11); v && !v->empty()) rel.error_message = *v;
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
		"       e.featured, g.slug AS game_slug, e.package_type, e.install_subdir, "
		"       e.github_repo "
		"FROM expansions e JOIN games g ON g.id = e.game_id "
		"ORDER BY e.featured DESC, e.slug;");
	std::vector<ExpansionRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_expansion(r));
	return out;
}

std::vector<ExpansionFileRow> list_expansion_files(opennova::db::Database &db,
                                                   int64_t expansion_id) {
	auto rows = db.query(
		"SELECT download_url, sha256, size_bytes, file_type, order_index "
		"FROM expansion_files WHERE expansion_id = ? ORDER BY order_index;",
		{i64(expansion_id)});
	std::vector<ExpansionFileRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		ExpansionFileRow f;
		f.download_url = r.as_text(0).value_or("");
		f.sha256       = r.as_text(1).value_or("");
		if (auto v = r.as_int(2)) f.size_bytes = *v;
		f.file_type    = r.as_text(3).value_or("archive");
		f.order_index  = static_cast<int>(r.as_int(4).value_or(1));
		out.push_back(std::move(f));
	}
	return out;
}

std::vector<ReleaseRow> list_recent_releases(opennova::db::Database &db, int limit) {
	if (limit < 1) limit = 20;
	if (limit > 100) limit = 100;
	auto rows = db.query(
		"SELECT id, slug, version, status, repo_ref, "
		"       workflow_url, target_commit, created_at, updated_at, "
		"       notes, published_at, error_message "
		"FROM expansion_releases ORDER BY created_at DESC LIMIT ?;",
		{i64(limit)});
	std::vector<ReleaseRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_release(r));
	return out;
}

// --- Write side ----------------------------------------------------------

ExpansionLookup find_expansion_by_slug(opennova::db::Database &db,
                                       const std::string &slug) {
	ExpansionLookup out;
	auto rows = db.query(
		"SELECT id, game_id, version, github_repo FROM expansions WHERE slug = ? LIMIT 1;",
		{txt(slug)});
	if (rows.empty()) return out;  // found == false
	const auto &r = rows.front();
	out.id          = r.as_int(0).value_or(0);
	out.game_id     = r.as_int(1).value_or(0);
	out.version     = r.as_text(2).value_or("");
	out.github_repo = r.as_text(3).value_or("");
	out.found       = true;
	return out;
}

void set_expansion_version(opennova::db::Database &db, int64_t expansion_id,
                           const std::string &version) {
	// onnet admin.py:252-263.
	db.exec(
		"UPDATE expansions SET version = ?, updated_at = CURRENT_TIMESTAMP "
		"WHERE id = ?;",
		{txt(version), i64(expansion_id)});
}

void create_or_reset_release(opennova::db::Database &db, const std::string &slug,
                             const std::string &version, const std::string &repo_ref,
                             const std::optional<std::string> &notes) {
	// onnet expansion_releases.py:27-58. Postgres EXCLUDED -> SQLite excluded.
	db.exec(
		"INSERT INTO expansion_releases (slug, version, repo_ref, notes) "
		"VALUES (?, ?, ?, ?) "
		"ON CONFLICT(slug, version) DO UPDATE SET "
		"    repo_ref = excluded.repo_ref, "
		"    notes = excluded.notes, "
		"    status = 'pending', "
		"    error_message = NULL, "
		"    workflow_url = NULL, "
		"    target_commit = NULL, "
		"    published_at = NULL, "
		"    updated_at = CURRENT_TIMESTAMP;",
		{txt(slug), txt(version), txt(repo_ref), opt_txt(notes)});
}

void update_release_status(opennova::db::Database &db, const std::string &slug,
                           const std::string &version, const std::string &status,
                           const std::optional<std::string> &error_message,
                           const std::optional<std::string> &workflow_url,
                           const std::optional<std::string> &target_commit,
                           const std::optional<std::string> &published_at) {
	// onnet expansion_releases.py:60-94. COALESCE preserves existing values
	// when the corresponding arg is NULL.
	db.exec(
		"UPDATE expansion_releases SET "
		"    status = ?, "
		"    error_message = ?, "
		"    workflow_url = COALESCE(?, workflow_url), "
		"    target_commit = COALESCE(?, target_commit), "
		"    published_at = COALESCE(?, published_at), "
		"    updated_at = CURRENT_TIMESTAMP "
		"WHERE slug = ? AND version = ?;",
		{txt(status), opt_txt(error_message), opt_txt(workflow_url),
		 opt_txt(target_commit), opt_txt(published_at), txt(slug), txt(version)});
}

void upsert_expansion_file(opennova::db::Database &db, int64_t expansion_id,
                           const std::string &download_url, const std::string &sha256,
                           const std::optional<int64_t> &size_bytes,
                           const std::string &file_type, int order_index) {
	// onnet expansions.py:135-159: DELETE then INSERT. onnet deletes by
	// (expansion_id, order_index) while the table UNIQUE is
	// (expansion_id, file_type, order_index); with the default
	// file_type='archive'/order_index=1 these coincide. Kept faithful to
	// onnet — revisit if multiple file_types per expansion are ever added.
	db.begin();
	try {
		db.exec(
			"DELETE FROM expansion_files "
			"WHERE expansion_id = ? AND order_index = ?;",
			{i64(expansion_id), i64(order_index)});
		db.exec(
			"INSERT INTO expansion_files "
			"    (expansion_id, download_url, sha256, size_bytes, file_type, order_index) "
			"VALUES (?, ?, ?, ?, ?, ?);",
			{i64(expansion_id), txt(download_url), txt(sha256), opt_i64(size_bytes),
			 txt(file_type), i64(order_index)});
		db.commit();
	} catch (...) {
		db.rollback();
		throw;
	}
}

std::optional<ReleaseRow> get_release(opennova::db::Database &db,
                                      const std::string &slug, const std::string &version) {
	auto rows = db.query(
		"SELECT id, slug, version, status, repo_ref, "
		"       workflow_url, target_commit, created_at, updated_at, "
		"       notes, published_at, error_message "
		"FROM expansion_releases WHERE slug = ? AND version = ? LIMIT 1;",
		{txt(slug), txt(version)});
	if (rows.empty()) return std::nullopt;
	return row_to_release(rows.front());
}

} // namespace opennova::server::catalog
