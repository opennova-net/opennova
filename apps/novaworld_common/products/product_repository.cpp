#include "product_repository.h"

#include <base/io/json.h>

#include <array>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

constexpr std::array<const char *, 3> kKindNames = { "game", "expansion", "app" };
constexpr std::array<const char *, 4> kTreeStateNames = { "building", "ready", "failed", "pruned" };
constexpr std::array<const char *, 7> kReleaseStatusNames = { "uploading", "validating", "invalid",
	"draft", "published", "retired", "building" };

constexpr const char *kProductColumns =
		"id, display_name, kind, parent_id, expansion_code, title, lobby, updater_login, "
		"command_file, retail, current_tree_id, published_release_id, created_by, created_at, "
		"updated_at, product_name, product_exe, key, visibility, head, wizard_login, download, "
		"description, owner_id, (SELECT o.username FROM players o WHERE o.id = products.owner_id)";
constexpr const char *kTreeColumns =
		"id, product_id, state, release_id, version_text, manifest::text, missions::text, bytes, "
		"error, created_by, created_at, ready_at, pruned_at";
constexpr const char *kReleaseColumns =
		"id, status, sha256, bytes, package_product, name, error, created_by, created_at, "
		"published_at, retired_at, product_id, revision, base_revision, version, "
		"build_log";

ProductRow row_to_product(const db::Row &r) {
	ProductRow d;
	d.id = r.as_int(0).value_or(0);
	d.display_name = r.as_text(1).value_or("");
	d.kind = product_kind_from_name(r.as_text(2).value_or("")).value_or(ProductKind::Game);
	d.parent_id = r.as_int(3);
	d.expansion_code = r.as_text(4).value_or("");
	d.title = r.as_text(5).value_or("");
	d.lobby = r.as_text(6).value_or("");
	d.updater_login = r.as_text(7).value_or("");
	d.command_file = r.as_text(8).value_or("");
	d.retail = r.as_int(9).value_or(0) != 0;
	d.current_tree_id = r.as_int(10);
	d.published_release_id = r.as_int(11);
	d.created_by = r.as_text(12).value_or("");
	d.created_at = r.as_text(13).value_or("");
	d.updated_at = r.as_text(14).value_or("");
	d.product_name = r.as_text(15).value_or("");
	d.product_exe = r.as_text(16).value_or("");
	d.key = r.as_text(17).value_or("");
	d.visibility = r.as_text(18).value_or("");
	d.head = r.as_int(19).value_or(0);
	d.wizard_login = r.as_text(20).value_or("");
	d.download = r.as_text(21).value_or("");
	d.description = r.as_text(22).value_or("");
	d.owner_id = r.as_int(23);
	d.owner = r.as_text(24).value_or("");
	return d;
}

ProductTree row_to_tree(const db::Row &r) {
	ProductTree t;
	t.id = r.as_int(0).value_or(0);
	t.product_id = r.as_int(1).value_or(0);
	t.state = tree_state_from_name(r.as_text(2).value_or("")).value_or(TreeState::Failed);
	t.release_id = r.as_int(3);
	t.version_text = r.as_text(4).value_or("");
	decode_tree_files(r.as_text(5).value_or("{}"), t.files);
	decode_tree_missions(r.as_text(6).value_or("[]"), t.missions);
	t.bytes = r.as_int(7).value_or(0);
	t.error = r.as_text(8).value_or("");
	t.created_by = r.as_text(9).value_or("");
	t.created_at = r.as_text(10).value_or("");
	t.ready_at = r.as_text(11).value_or("");
	t.pruned_at = r.as_text(12).value_or("");
	return t;
}

ReleaseRow row_to_release(const db::Row &r) {
	ReleaseRow rel;
	rel.id = r.as_int(0).value_or(0);
	rel.status = release_status_from_name(r.as_text(1).value_or("")).value_or(ReleaseStatus::Invalid);
	rel.sha256 = r.as_text(2).value_or("");
	rel.bytes = r.as_int(3).value_or(0);
	rel.package_product = r.as_text(4).value_or("");
	rel.name = r.as_text(5).value_or("");
	rel.error = r.as_text(6).value_or("");
	rel.created_by = r.as_text(7).value_or("");
	rel.created_at = r.as_text(8).value_or("");
	rel.published_at = r.as_text(9).value_or("");
	rel.retired_at = r.as_text(10).value_or("");
	rel.product_id = r.as_int(11);
	rel.revision = r.as_int(12).value_or(0);
	rel.base_revision = r.as_int(13).value_or(0);
	rel.version = r.as_text(14).value_or("");
	rel.build_log = r.as_text(15).value_or("");
	return rel;
}

std::vector<ProductRow> query_products(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<ProductRow> out;
	for (const auto &r : db.query(std::string("SELECT ") + kProductColumns + " FROM products " + tail, binds))
		out.push_back(row_to_product(r));
	return out;
}

std::vector<ProductTree> query_trees(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<ProductTree> out;
	for (const auto &r : db.query(std::string("SELECT ") + kTreeColumns + " FROM product_trees " + tail, binds))
		out.push_back(row_to_tree(r));
	return out;
}

// Whether a release still names a product that serves it; when none does, it is retired.
void retire_if_unreferenced(db::Database &db, int64_t release_id) {
	db.exec("UPDATE releases SET status = 'retired', retired_at = now() "
			"WHERE id = $1 AND status = 'published' "
			"AND NOT EXISTS (SELECT 1 FROM products WHERE published_release_id = $1);",
			{ BindValue(release_id) });
}

// The products release $1 serves once published, as SQL: its sections' products, or for a
// release with no section (a download-only app's) its own.
constexpr const char *kReleaseProductsSql =
		"SELECT product_id FROM release_sections WHERE release_id = $1 "
		"UNION SELECT product_id FROM releases WHERE id = $1 AND product_id IS NOT NULL "
		"AND NOT EXISTS (SELECT 1 FROM release_sections WHERE release_id = $1)";

// A live hosted server as SQL on hosted_servers' state: the hosted servers'
// own rule (hosted/model/hosted_repository.h is_live, kLiveServerSql), which
// the products' holds on the servers' rows read without the model above them;
// product_repository_test pins the two alike.
constexpr const char *kServerLiveSql = "state NOT IN ('stopped', 'failed')";

} // namespace

const char *product_kind_name(ProductKind kind) {
	return kKindNames[static_cast<size_t>(kind) < kKindNames.size() ? static_cast<size_t>(kind) : 0];
}

std::optional<ProductKind> product_kind_from_name(std::string_view name) {
	for (size_t i = 0; i < kKindNames.size(); ++i)
		if (name == kKindNames[i])
			return static_cast<ProductKind>(i);
	return std::nullopt;
}

const char *tree_state_name(TreeState state) {
	const auto i = static_cast<size_t>(state);
	return i < kTreeStateNames.size() ? kTreeStateNames[i] : "failed";
}

std::optional<TreeState> tree_state_from_name(std::string_view name) {
	for (size_t i = 0; i < kTreeStateNames.size(); ++i)
		if (name == kTreeStateNames[i])
			return static_cast<TreeState>(i);
	return std::nullopt;
}

const char *release_status_name(ReleaseStatus status) {
	const auto i = static_cast<size_t>(status);
	return i < kReleaseStatusNames.size() ? kReleaseStatusNames[i] : "invalid";
}

std::optional<ReleaseStatus> release_status_from_name(std::string_view name) {
	for (size_t i = 0; i < kReleaseStatusNames.size(); ++i)
		if (name == kReleaseStatusNames[i])
			return static_cast<ReleaseStatus>(i);
	return std::nullopt;
}

// --- products ---

ProductOutcome create_product(db::Database &db, const NewProduct &product, int64_t &id) {
	db::Transaction tx(db);
	if (product.kind == ProductKind::Expansion) {
		const auto parent = db.query("SELECT kind FROM products WHERE id = $1 FOR SHARE;",
				{ db::int_or_null(product.parent_id) });
		if (parent.empty() || parent.front().as_text(0).value_or("") != "game")
			return ProductOutcome::NoSuchParent;
	}
	if (product.owner_id) {
		// The owner's row held while its projects are counted: two creates of one account's wait
		// on each other here, so the count never passes its cap.
		db.query("SELECT 1 FROM players WHERE id = $1 FOR NO KEY UPDATE;",
				{ BindValue(*product.owner_id) });
		const auto owned = db.query("SELECT count(*) FROM products WHERE owner_id = $1;",
				{ BindValue(*product.owner_id) });
		if (!owned.empty() && owned.front().as_int(0).value_or(0) >= kOwnedProjectsMax)
			return ProductOutcome::TooManyProjects;
	}
	if (!product.updater_login.empty() &&
			!db.query("SELECT 1 FROM products WHERE upper(updater_login) = upper($1::text);",
						{ BindValue(product.updater_login) })
					 .empty())
		return ProductOutcome::DuplicateLogin;
	if (!db.query("SELECT 1 FROM products WHERE key = $1;", { BindValue(product.key) }).empty())
		return ProductOutcome::DuplicateKey;
	if (product.kind == ProductKind::Expansion &&
			!db.query("SELECT 1 FROM products WHERE parent_id = $1 AND expansion_code = $2;",
						{ db::int_or_null(product.parent_id), BindValue(product.expansion_code) })
					 .empty())
		return ProductOutcome::DuplicateExpansion;
	const bool game = product.kind == ProductKind::Game;
	const auto rows = db.query(
			"INSERT INTO products (display_name, kind, parent_id, expansion_code, title, lobby, "
			"updater_login, command_file, retail, key, visibility, created_by, owner_id) "
			"VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) RETURNING id;",
			{ BindValue(product.display_name), BindValue(product_kind_name(product.kind)),
					game ? BindValue(std::monostate{}) : db::int_or_null(product.parent_id),
					BindValue(game ? std::string() : product.expansion_code),
					BindValue(game ? product.title : std::string()),
					BindValue(game ? product.lobby : std::string()),
					BindValue(product.updater_login), BindValue(product.command_file),
					BindValue(db::Bool{ product.retail }), BindValue(product.key),
					db::text_or_null(product.visibility), BindValue(product.created_by),
					db::int_or_null(product.owner_id) });
	id = rows.front().as_int(0).value_or(0);
	tx.commit();
	return ProductOutcome::Ok;
}

ProductOutcome update_product(db::Database &db, int64_t id, const ProductPatch &patch) {
	db::Transaction tx(db);
	const auto rows = query_products(db, "WHERE id = $1 FOR NO KEY UPDATE", { BindValue(id) });
	if (rows.empty())
		return ProductOutcome::NotFound;
	ProductRow row = rows.front();
	if (patch.display_name)
		row.display_name = *patch.display_name;
	if (patch.lobby && row.kind == ProductKind::Game)
		row.lobby = *patch.lobby;
	if (patch.updater_login)
		row.updater_login = *patch.updater_login;
	if (patch.command_file)
		row.command_file = *patch.command_file;
	if (patch.retail)
		row.retail = *patch.retail;
	if (patch.key)
		row.key = *patch.key;
	if (patch.visibility)
		row.visibility = *patch.visibility;
	if (!row.updater_login.empty() &&
			!db.query("SELECT 1 FROM products WHERE upper(updater_login) = upper($1::text) AND id <> $2;",
						{ BindValue(row.updater_login), BindValue(id) })
					 .empty())
		return ProductOutcome::DuplicateLogin;
	if (!db.query("SELECT 1 FROM products WHERE key = $1 AND id <> $2;",
						{ BindValue(row.key), BindValue(id) })
					 .empty())
		return ProductOutcome::DuplicateKey;
	db.exec("UPDATE products SET display_name = $2, lobby = $3, updater_login = $4, "
			"command_file = $5, retail = $6, key = $7, visibility = $8, updated_at = now() "
			"WHERE id = $1;",
			{ BindValue(id), BindValue(row.display_name), BindValue(row.lobby),
					BindValue(row.updater_login), BindValue(row.command_file),
					BindValue(db::Bool{ row.retail }), BindValue(row.key),
					db::text_or_null(row.visibility) });
	tx.commit();
	return ProductOutcome::Ok;
}

ProductOutcome delete_product(db::Database &db, int64_t id) {
	db::Transaction tx(db);
	if (db.query("SELECT 1 FROM products WHERE id = $1 FOR UPDATE;", { BindValue(id) }).empty())
		return ProductOutcome::NotFound;
	if (count_live_servers_of(db, id) > 0)
		return ProductOutcome::InUse;
	if (!db.query("SELECT 1 FROM products WHERE parent_id = $1;", { BindValue(id) }).empty())
		return ProductOutcome::HasExpansion;
	db.exec("UPDATE products SET current_tree_id = NULL, published_release_id = NULL "
			"WHERE id = $1;",
			{ BindValue(id) });
	db.exec("DELETE FROM products WHERE id = $1;", { BindValue(id) });
	tx.commit();
	return ProductOutcome::Ok;
}

std::optional<ProductRow> find_product(db::Database &db, int64_t id) {
	const auto rows = query_products(db, "WHERE id = $1", { BindValue(id) });
	if (rows.empty())
		return std::nullopt;
	return rows.front();
}

std::vector<ProductRow> list_products(db::Database &db) {
	return query_products(db, "ORDER BY (kind = 'app'), (kind = 'expansion'), id", {});
}

std::vector<ProductRow> list_expansions_of(db::Database &db, int64_t parent_id) {
	return query_products(db, "WHERE parent_id = $1 ORDER BY id", { BindValue(parent_id) });
}

std::vector<ProductRow> list_projects(db::Database &db, bool all, std::optional<int64_t> reader) {
	if (all)
		return query_products(db, "WHERE kind <> 'app' ORDER BY key COLLATE \"C\"", {});
	return query_products(db,
			"WHERE kind <> 'app' AND (visibility = 'public' OR owner_id = $1) "
			"ORDER BY key COLLATE \"C\"",
			{ db::int_or_null(reader) });
}

std::vector<ProductRow> list_owned_projects(db::Database &db, int64_t owner_id) {
	return query_products(
			db, "WHERE owner_id = $1 ORDER BY key COLLATE \"C\"", { BindValue(owner_id) });
}

std::optional<ProductRow> find_product_by_key(db::Database &db, std::string_view key) {
	const auto rows = query_products(db, "WHERE key = $1", { BindValue(std::string(key)) });
	if (rows.empty())
		return std::nullopt;
	return rows.front();
}

int count_live_servers_of(db::Database &db, int64_t id) {
	const auto rows = db.query(std::string("SELECT count(*) FROM hosted_servers WHERE ") +
					kServerLiveSql + " AND (product_id = $1 OR expansion_id = $1);",
			{ BindValue(id) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

std::vector<OfferedProduct> offered_products(db::Database &db) {
	std::vector<OfferedProduct> out;
	const auto rows = db.query(
			"SELECT d.id, d.display_name, d.kind, d.parent_id, d.expansion_code, d.title, d.lobby, "
			"t.id, t.version_text, t.missions::text "
			"FROM products d JOIN product_trees t ON t.id = d.current_tree_id "
			"WHERE d.kind <> 'app' AND t.state = 'ready' ORDER BY (d.kind = 'expansion'), d.id;");
	for (const auto &r : rows) {
		const bool game = r.as_text(2).value_or("") == "game";
		if (game) {
			OfferedProduct d;
			d.id = r.as_int(0).value_or(0);
			d.display_name = r.as_text(1).value_or("");
			d.title = r.as_text(5).value_or("");
			d.lobby = r.as_text(6).value_or("");
			d.tree_id = r.as_int(7).value_or(0);
			d.version = r.as_text(8).value_or("");
			decode_tree_missions(r.as_text(9).value_or("[]"), d.missions);
			out.push_back(std::move(d));
			continue;
		}
		const int64_t parent = r.as_int(3).value_or(0);
		for (OfferedProduct &d : out) {
			if (d.id != parent)
				continue;
			OfferedExpansion e;
			e.id = r.as_int(0).value_or(0);
			e.code = r.as_text(4).value_or("");
			e.display_name = r.as_text(1).value_or("");
			e.tree_id = r.as_int(7).value_or(0);
			e.version = r.as_text(8).value_or("");
			decode_tree_missions(r.as_text(9).value_or("[]"), e.missions);
			d.expansions.push_back(std::move(e));
		}
	}
	return out;
}

// --- trees ---

int64_t create_tree(db::Database &db, int64_t product_id, std::optional<int64_t> release_id,
		const std::string &created_by) {
	const auto rows = db.query(
			"INSERT INTO product_trees (product_id, state, release_id, created_by) "
			"VALUES ($1, 'building', $2, $3) RETURNING id;",
			{ BindValue(product_id), db::int_or_null(release_id), BindValue(created_by) });
	return rows.front().as_int(0).value_or(0);
}

std::optional<ProductTree> find_tree(db::Database &db, int64_t id) {
	const auto rows = query_trees(db, "WHERE id = $1", { BindValue(id) });
	if (rows.empty())
		return std::nullopt;
	return rows.front();
}

std::vector<ProductTree> list_trees(db::Database &db, int64_t product_id) {
	return query_trees(db, "WHERE product_id = $1 ORDER BY id DESC", { BindValue(product_id) });
}

bool set_tree_state(db::Database &db, int64_t id, TreeState from, TreeState to,
		const std::string &error) {
	db.exec("UPDATE product_trees SET state = $3, error = $4 WHERE id = $1 AND state = $2;",
			{ BindValue(id), BindValue(tree_state_name(from)), BindValue(tree_state_name(to)),
					BindValue(error) });
	return db.changes() > 0;
}

bool finish_tree(db::Database &db, int64_t id, const TreeReady &ready) {
	db::Transaction tx(db);
	const auto rows = db.query(
			"UPDATE product_trees SET state = 'ready', ready_at = now(), error = '', "
			"manifest = $2::jsonb, missions = $3::jsonb, version_text = $4, bytes = $5 "
			"WHERE id = $1 AND state = 'building' RETURNING product_id;",
			{ BindValue(id), BindValue(encode_tree_files(ready.files)),
					BindValue(encode_tree_missions(ready.missions)), BindValue(ready.version_text),
					BindValue(ready.bytes) });
	if (rows.empty())
		return false;
	db.exec("UPDATE products SET current_tree_id = $2, updated_at = now() WHERE id = $1;",
			{ BindValue(rows.front().as_int(0).value_or(0)), BindValue(id) });
	tx.commit();
	return true;
}

std::vector<int64_t> prunable_trees(db::Database &db, int64_t min_age_s) {
	std::vector<int64_t> out;
	const auto rows = db.query(
			std::string("SELECT t.id FROM product_trees t WHERE "
						"(t.state = 'pruned' AND t.pruned_at IS NULL) "
						"OR (t.state IN ('ready', 'failed') AND t.created_at < now() - make_interval(secs => $1) "
						"    AND NOT EXISTS (SELECT 1 FROM products d WHERE d.current_tree_id = t.id) "
						"    AND NOT EXISTS (SELECT 1 FROM hosted_servers s WHERE ") +
					kServerLiveSql +
					"        AND (s.base_tree_id = t.id OR s.expansion_tree_id = t.id))) "
					"ORDER BY t.id;",
			{ BindValue(static_cast<double>(min_age_s)) });
	for (const auto &r : rows)
		out.push_back(r.as_int(0).value_or(0));
	return out;
}

void mark_tree_pruned(db::Database &db, int64_t id) {
	db.exec("UPDATE product_trees SET state = 'pruned', pruned_at = now() WHERE id = $1;",
			{ BindValue(id) });
}

int count_tree_pins(db::Database &db, int64_t id) {
	const auto rows = db.query(std::string("SELECT count(*) FROM hosted_servers WHERE ") +
					kServerLiveSql + " AND (base_tree_id = $1 OR expansion_tree_id = $1);",
			{ BindValue(id) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

// --- releases ---

bool valid_release_version(std::string_view version) {
	if (version.empty() || version.size() > 32)
		return false;
	int dots = 0;
	size_t digits = 0;
	for (const char c : version) {
		if (c >= '0' && c <= '9') {
			++digits;
			continue;
		}
		if (c != '.' || digits == 0)
			return false;
		++dots;
		digits = 0;
	}
	return digits > 0 && dots < 4;
}

int64_t create_release(db::Database &db, const std::string &name, int64_t bytes,
		const std::string &created_by, std::optional<int64_t> product_id,
		const std::string &version) {
	const auto rows = db.query(
			"INSERT INTO releases (name, bytes, created_by, product_id, version) "
			"VALUES ($1, $2, $3, $4, $5) RETURNING id;",
			{ BindValue(name), BindValue(bytes), BindValue(created_by), db::int_or_null(product_id),
					BindValue(version) });
	return rows.front().as_int(0).value_or(0);
}

int64_t create_built_release(db::Database &db, const NewBuiltRelease &release) {
	const auto rows = db.query(
			"INSERT INTO releases (status, name, created_by, product_id, revision, "
			"base_revision, version) VALUES ('building', $1, $2, $3, $4, $5, $6) RETURNING id;",
			{ BindValue(release.name), BindValue(release.created_by),
					BindValue(release.product_id), BindValue(release.revision),
					release.base_revision > 0 ? BindValue(release.base_revision)
											  : BindValue(std::monostate{}),
					BindValue(release.version) });
	return rows.front().as_int(0).value_or(0);
}

bool set_release_built(db::Database &db, int64_t id, const std::string &build_log) {
	db.exec("UPDATE releases SET status = 'validating', build_log = $2 "
			"WHERE id = $1 AND status = 'building';",
			{ BindValue(id), BindValue(build_log) });
	return db.changes() > 0;
}

bool set_release_build_failed(
		db::Database &db, int64_t id, const std::string &error, const std::string &build_log) {
	db.exec("UPDATE releases SET status = 'invalid', error = $2, build_log = $3 "
			"WHERE id = $1 AND status = 'building';",
			{ BindValue(id), BindValue(error), BindValue(build_log) });
	return db.changes() > 0;
}

bool set_release_stored(db::Database &db, int64_t id, const std::string &sha256, int64_t bytes) {
	db.exec("UPDATE releases SET status = 'draft', error = '', sha256 = $2, bytes = $3 "
			"WHERE id = $1 AND status = 'building';",
			{ BindValue(id), BindValue(sha256), BindValue(bytes) });
	return db.changes() > 0;
}

std::optional<ReleaseRow> find_release(db::Database &db, int64_t id) {
	const auto rows = db.query(
			std::string("SELECT ") + kReleaseColumns + " FROM releases WHERE id = $1;", { BindValue(id) });
	if (rows.empty())
		return std::nullopt;
	return row_to_release(rows.front());
}

std::optional<int64_t> find_release_by_sha(db::Database &db, std::string_view sha256) {
	const auto rows = db.query("SELECT id FROM releases WHERE sha256 = $1 AND sha256 <> '';",
			{ BindValue(std::string(sha256)) });
	if (rows.empty())
		return std::nullopt;
	return rows.front().as_int(0);
}

std::vector<ReleaseRow> list_releases(db::Database &db) {
	std::vector<ReleaseRow> out;
	for (const auto &r : db.query(std::string("SELECT ") + kReleaseColumns + " FROM releases ORDER BY id DESC;"))
		out.push_back(row_to_release(r));
	return out;
}

std::vector<ReleaseSection> list_sections(db::Database &db, int64_t release_id) {
	std::vector<ReleaseSection> out;
	for (const auto &r : db.query(
				 "SELECT release_id, product_id, expansion, command_file, version, row_count, "
				 "blob_bytes, tree_id FROM release_sections WHERE release_id = $1 ORDER BY (expansion <> ''), product_id;",
				 { BindValue(release_id) })) {
		ReleaseSection s;
		s.release_id = r.as_int(0).value_or(0);
		s.product_id = r.as_int(1).value_or(0);
		s.expansion = r.as_text(2).value_or("");
		s.command_file = r.as_text(3).value_or("");
		s.version = r.as_text(4).value_or("");
		s.row_count = r.as_int(5).value_or(0);
		s.blob_bytes = r.as_int(6).value_or(0);
		s.tree_id = r.as_int(7);
		out.push_back(std::move(s));
	}
	return out;
}

bool set_release_status(db::Database &db, int64_t id, ReleaseStatus from, ReleaseStatus to,
		const std::string &error) {
	db.exec("UPDATE releases SET status = $3, error = $4 WHERE id = $1 AND status = $2;",
			{ BindValue(id), BindValue(release_status_name(from)), BindValue(release_status_name(to)),
					BindValue(error) });
	return db.changes() > 0;
}

bool set_release_validated(db::Database &db, int64_t id, const std::string &sha256, int64_t bytes,
		const std::string &package_product, const std::vector<NewReleaseSection> &sections) {
	db::Transaction tx(db);
	db.exec("UPDATE releases SET status = 'draft', error = '', sha256 = $2, bytes = $3, "
			"package_product = $4, product_id = COALESCE(product_id, $5) "
			"WHERE id = $1 AND status = 'validating';",
			{ BindValue(id), BindValue(sha256), BindValue(bytes), BindValue(package_product),
					sections.empty() ? BindValue(std::monostate{})
									 : BindValue(sections.front().product_id) });
	if (db.changes() == 0)
		return false;
	db.exec("DELETE FROM release_sections WHERE release_id = $1;", { BindValue(id) });
	for (const NewReleaseSection &s : sections)
		db.exec("INSERT INTO release_sections (release_id, product_id, expansion, command_file, "
				"version, row_count, blob_bytes) VALUES ($1, $2, $3, $4, $5, $6, $7);",
				{ BindValue(id), BindValue(s.product_id), BindValue(s.expansion),
						BindValue(s.command_file), BindValue(s.version), BindValue(s.row_count),
						BindValue(s.blob_bytes) });
	tx.commit();
	return true;
}

bool publish_release(db::Database &db, int64_t id) {
	db::Transaction tx(db);
	db.exec("UPDATE releases SET status = 'published', published_at = now(), retired_at = NULL "
			"WHERE id = $1 AND status = 'draft';",
			{ BindValue(id) });
	if (db.changes() == 0)
		return false;
	const auto previous = db.query(
			std::string("SELECT DISTINCT published_release_id FROM products WHERE id IN (") +
					kReleaseProductsSql +
					") AND published_release_id IS NOT NULL AND published_release_id <> $1;",
			{ BindValue(id) });
	db.exec(std::string("UPDATE products SET published_release_id = $1, updated_at = now() "
						"WHERE id IN (") +
					kReleaseProductsSql + ");",
			{ BindValue(id) });
	for (const auto &r : previous)
		if (const auto release = r.as_int(0))
			retire_if_unreferenced(db, *release);
	tx.commit();
	return true;
}

bool retire_release(db::Database &db, int64_t id) {
	db::Transaction tx(db);
	db.exec("UPDATE releases SET status = 'retired', retired_at = now() "
			"WHERE id = $1 AND status = 'published';",
			{ BindValue(id) });
	if (db.changes() == 0)
		return false;
	db.exec("UPDATE products SET published_release_id = NULL, updated_at = now() "
			"WHERE published_release_id = $1;",
			{ BindValue(id) });
	tx.commit();
	return true;
}

bool delete_release(db::Database &db, int64_t id) {
	db.exec("DELETE FROM releases WHERE id = $1;", { BindValue(id) });
	return db.changes() > 0;
}

void set_section_tree(db::Database &db, int64_t release_id, int64_t product_id, int64_t tree_id) {
	db.exec("UPDATE release_sections SET tree_id = $3 WHERE release_id = $1 AND product_id = $2;",
			{ BindValue(release_id), BindValue(product_id), BindValue(tree_id) });
}

std::vector<PublishedSection> published_sections(db::Database &db) {
	std::vector<PublishedSection> out;
	for (const auto &r : db.query(
				 "SELECT d.id, r.id, d.updater_login, s.command_file, s.expansion, r.sha256, d.retail "
				 "FROM products d JOIN releases r ON r.id = d.published_release_id "
				 "JOIN release_sections s ON s.release_id = r.id AND s.product_id = d.id "
				 "WHERE d.updater_login <> '' AND r.status = 'published' ORDER BY d.updater_login;")) {
		PublishedSection p;
		p.product_id = r.as_int(0).value_or(0);
		p.release_id = r.as_int(1).value_or(0);
		p.updater_login = r.as_text(2).value_or("");
		p.command_file = r.as_text(3).value_or("");
		p.expansion = r.as_text(4).value_or("");
		p.sha256 = r.as_text(5).value_or("");
		p.retail = r.as_int(6).value_or(0) != 0;
		out.push_back(std::move(p));
	}
	return out;
}

// --- the hosted servers' pins ---

std::optional<CurrentTrees> current_trees_of(db::Database &db, int64_t product_id,
		std::optional<int64_t> expansion_id) {
	const auto base = db.query(
			"SELECT d.current_tree_id FROM products d JOIN product_trees t ON t.id = d.current_tree_id "
			"WHERE d.id = $1 AND d.kind = 'game' AND t.state = 'ready';",
			{ BindValue(product_id) });
	if (base.empty() || !base.front().as_int(0))
		return std::nullopt;
	CurrentTrees trees;
	trees.base_tree_id = *base.front().as_int(0);
	if (expansion_id) {
		const auto exp = db.query(
				"SELECT d.current_tree_id FROM products d JOIN product_trees t ON t.id = d.current_tree_id "
				"WHERE d.id = $1 AND d.kind = 'expansion' AND d.parent_id = $2 AND t.state = 'ready';",
				{ BindValue(*expansion_id), BindValue(product_id) });
		if (exp.empty() || !exp.front().as_int(0))
			return std::nullopt;
		trees.expansion_tree_id = exp.front().as_int(0);
	}
	return trees;
}

// --- the JSON columns ---

std::string encode_tree_files(const std::vector<TreeFile> &files) {
	io::JsonValue list = io::JsonValue::make_array();
	for (const TreeFile &f : files) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("name", io::json_string(f.name));
		item.set("size", io::json_number(static_cast<double>(f.size)));
		list.push(std::move(item));
	}
	io::JsonValue root = io::JsonValue::make_object();
	root.set("files", std::move(list));
	return io::json_write(root);
}

bool decode_tree_files(std::string_view json, std::vector<TreeFile> &out) {
	out.clear();
	io::JsonValue root;
	std::string error;
	if (!io::json_parse(json, root, error) || !root.is_object())
		return false;
	const io::JsonValue *list = root.get("files");
	if (list == nullptr || !list->is_array())
		return true;
	for (const io::JsonValue &item : list->array) {
		if (!item.is_object())
			return false;
		TreeFile f;
		f.name = item.get_string("name", "");
		f.size = static_cast<uint64_t>(item.get_number("size", 0));
		out.push_back(std::move(f));
	}
	return true;
}

std::string encode_tree_missions(const std::vector<TreeMission> &missions) {
	io::JsonValue list = io::JsonValue::make_array();
	for (const TreeMission &m : missions) {
		io::JsonValue item = io::JsonValue::make_object();
		item.set("file", io::json_string(m.file));
		item.set("title", io::json_string(m.title));
		list.push(std::move(item));
	}
	return io::json_write(list);
}

bool decode_tree_missions(std::string_view json, std::vector<TreeMission> &out) {
	out.clear();
	io::JsonValue list;
	std::string error;
	if (!io::json_parse(json, list, error) || !list.is_array())
		return false;
	for (const io::JsonValue &item : list.array) {
		if (!item.is_object())
			return false;
		TreeMission m;
		m.file = item.get_string("file", "");
		m.title = item.get_string("title", "");
		out.push_back(std::move(m));
	}
	return true;
}

} // namespace opennova::novaworld_server::hosted
