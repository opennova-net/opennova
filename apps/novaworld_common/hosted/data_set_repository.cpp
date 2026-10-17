#include "data_set_repository.h"

#include "hosted_repository.h"

#include <base/io/json.h>

#include <array>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

constexpr std::array<const char *, 2> kKindNames = { "base", "expansion" };
constexpr std::array<const char *, 6> kTreeStateNames = { "uploading", "finalizing", "building",
	"ready", "failed", "pruned" };
constexpr std::array<const char *, 2> kTreeSourceNames = { "baseline", "release" };
constexpr std::array<const char *, 6> kReleaseStatusNames = { "uploading", "validating", "invalid",
	"draft", "published", "retired" };

constexpr const char *kDataSetColumns =
		"id, display_name, kind, parent_id, expansion_code, title, game, updater_login, "
		"command_file, retail_updater, current_tree_id, baseline_tree_id, published_release_id, "
		"created_by, created_at, updated_at";
constexpr const char *kTreeColumns =
		"id, data_set_id, state, source, release_id, baseline_tree_id, version_text, "
		"manifest::text, missions::text, bytes, error, created_by, created_at, ready_at, pruned_at";
constexpr const char *kReleaseColumns =
		"id, status, sha256, bytes, package_product, name, error, created_by, created_at, "
		"published_at, retired_at";

BindValue optional_id(std::optional<int64_t> id) {
	if (!id)
		return BindValue(std::monostate{});
	return BindValue(*id);
}

DataSetRow row_to_data_set(const db::Row &r) {
	DataSetRow d;
	d.id = r.as_int(0).value_or(0);
	d.display_name = r.as_text(1).value_or("");
	d.kind = data_set_kind_from_name(r.as_text(2).value_or("")).value_or(DataSetKind::Base);
	d.parent_id = r.as_int(3);
	d.expansion_code = r.as_text(4).value_or("");
	d.title = r.as_text(5).value_or("");
	d.game = r.as_text(6).value_or("");
	d.updater_login = r.as_text(7).value_or("");
	d.command_file = r.as_text(8).value_or("");
	d.retail_updater = r.as_int(9).value_or(0) != 0;
	d.current_tree_id = r.as_int(10);
	d.baseline_tree_id = r.as_int(11);
	d.published_release_id = r.as_int(12);
	d.created_by = r.as_text(13).value_or("");
	d.created_at = r.as_text(14).value_or("");
	d.updated_at = r.as_text(15).value_or("");
	return d;
}

DataSetTree row_to_tree(const db::Row &r) {
	DataSetTree t;
	t.id = r.as_int(0).value_or(0);
	t.data_set_id = r.as_int(1).value_or(0);
	t.state = tree_state_from_name(r.as_text(2).value_or("")).value_or(TreeState::Failed);
	t.source = r.as_text(3).value_or("") == "release" ? TreeSource::Release : TreeSource::Baseline;
	t.release_id = r.as_int(4);
	t.baseline_tree_id = r.as_int(5);
	t.version_text = r.as_text(6).value_or("");
	decode_tree_files(r.as_text(7).value_or("{}"), t.files);
	decode_tree_missions(r.as_text(8).value_or("[]"), t.missions);
	t.bytes = r.as_int(9).value_or(0);
	t.error = r.as_text(10).value_or("");
	t.created_by = r.as_text(11).value_or("");
	t.created_at = r.as_text(12).value_or("");
	t.ready_at = r.as_text(13).value_or("");
	t.pruned_at = r.as_text(14).value_or("");
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
	return rel;
}

std::vector<DataSetRow> query_data_sets(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<DataSetRow> out;
	for (const auto &r : db.query(std::string("SELECT ") + kDataSetColumns + " FROM data_sets " + tail, binds))
		out.push_back(row_to_data_set(r));
	return out;
}

std::vector<DataSetTree> query_trees(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<DataSetTree> out;
	for (const auto &r : db.query(std::string("SELECT ") + kTreeColumns + " FROM data_set_trees " + tail, binds))
		out.push_back(row_to_tree(r));
	return out;
}

// Whether a release still names a data set that serves it; when none does, it is retired.
void retire_if_unreferenced(db::Database &db, int64_t release_id) {
	db.exec("UPDATE releases SET status = 'retired', retired_at = now() "
			"WHERE id = $1 AND status = 'published' "
			"AND NOT EXISTS (SELECT 1 FROM data_sets WHERE published_release_id = $1);",
			{ BindValue(release_id) });
}

// The servers whose pins are behind their data sets' current trees, as a WHERE over
// hosted_servers s (live ones).
constexpr const char *kOutdatedSql =
		"EXISTS (SELECT 1 FROM data_sets b WHERE b.id = s.data_set_id "
		"        AND b.current_tree_id IS NOT NULL AND b.current_tree_id IS DISTINCT FROM s.base_tree_id) "
		"OR EXISTS (SELECT 1 FROM data_sets e WHERE e.id = s.expansion_id "
		"        AND e.current_tree_id IS NOT NULL AND e.current_tree_id IS DISTINCT FROM s.expansion_tree_id)";

} // namespace

const char *data_set_kind_name(DataSetKind kind) {
	return kKindNames[static_cast<size_t>(kind) < kKindNames.size() ? static_cast<size_t>(kind) : 0];
}

std::optional<DataSetKind> data_set_kind_from_name(std::string_view name) {
	for (size_t i = 0; i < kKindNames.size(); ++i)
		if (name == kKindNames[i])
			return static_cast<DataSetKind>(i);
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

const char *tree_source_name(TreeSource source) {
	return kTreeSourceNames[source == TreeSource::Release ? 1 : 0];
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

// --- data sets ---

DataSetOutcome create_data_set(db::Database &db, const NewDataSet &data_set, int64_t &id) {
	db::Transaction tx(db);
	if (data_set.kind == DataSetKind::Expansion) {
		const auto parent = db.query("SELECT kind FROM data_sets WHERE id = $1 FOR SHARE;",
				{ optional_id(data_set.parent_id) });
		if (parent.empty() || parent.front().as_text(0).value_or("") != "base")
			return DataSetOutcome::NoSuchParent;
	}
	if (!data_set.updater_login.empty() &&
			!db.query("SELECT 1 FROM data_sets WHERE upper(updater_login) = upper($1::text);",
						{ BindValue(data_set.updater_login) })
					 .empty())
		return DataSetOutcome::DuplicateLogin;
	if (data_set.kind == DataSetKind::Expansion &&
			!db.query("SELECT 1 FROM data_sets WHERE parent_id = $1 AND expansion_code = $2;",
						{ optional_id(data_set.parent_id), BindValue(data_set.expansion_code) })
					 .empty())
		return DataSetOutcome::DuplicateExpansion;
	const bool base = data_set.kind == DataSetKind::Base;
	const auto rows = db.query(
			"INSERT INTO data_sets (display_name, kind, parent_id, expansion_code, title, game, "
			"updater_login, command_file, retail_updater, created_by) "
			"VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) RETURNING id;",
			{ BindValue(data_set.display_name), BindValue(data_set_kind_name(data_set.kind)),
					base ? BindValue(std::monostate{}) : optional_id(data_set.parent_id),
					BindValue(base ? std::string() : data_set.expansion_code),
					BindValue(base ? data_set.title : std::string()),
					BindValue(base ? data_set.game : std::string()),
					BindValue(data_set.updater_login), BindValue(data_set.command_file),
					BindValue(db::Bool{ data_set.retail_updater }), BindValue(data_set.created_by) });
	id = rows.front().as_int(0).value_or(0);
	tx.commit();
	return DataSetOutcome::Ok;
}

DataSetOutcome update_data_set(db::Database &db, int64_t id, const DataSetPatch &patch) {
	db::Transaction tx(db);
	const auto rows = query_data_sets(db, "WHERE id = $1 FOR NO KEY UPDATE", { BindValue(id) });
	if (rows.empty())
		return DataSetOutcome::NotFound;
	DataSetRow row = rows.front();
	if (patch.display_name)
		row.display_name = *patch.display_name;
	if (patch.game && row.kind == DataSetKind::Base)
		row.game = *patch.game;
	if (patch.updater_login)
		row.updater_login = *patch.updater_login;
	if (patch.command_file)
		row.command_file = *patch.command_file;
	if (patch.retail_updater)
		row.retail_updater = *patch.retail_updater;
	if (!row.updater_login.empty() &&
			!db.query("SELECT 1 FROM data_sets WHERE upper(updater_login) = upper($1::text) AND id <> $2;",
						{ BindValue(row.updater_login), BindValue(id) })
					 .empty())
		return DataSetOutcome::DuplicateLogin;
	db.exec("UPDATE data_sets SET display_name = $2, game = $3, updater_login = $4, "
			"command_file = $5, retail_updater = $6, updated_at = now() WHERE id = $1;",
			{ BindValue(id), BindValue(row.display_name), BindValue(row.game),
					BindValue(row.updater_login), BindValue(row.command_file),
					BindValue(db::Bool{ row.retail_updater }) });
	tx.commit();
	return DataSetOutcome::Ok;
}

DataSetOutcome delete_data_set(db::Database &db, int64_t id) {
	db::Transaction tx(db);
	if (db.query("SELECT 1 FROM data_sets WHERE id = $1 FOR UPDATE;", { BindValue(id) }).empty())
		return DataSetOutcome::NotFound;
	if (count_live_servers_of(db, id) > 0 ||
			!db.query("SELECT 1 FROM data_sets WHERE parent_id = $1;", { BindValue(id) }).empty())
		return DataSetOutcome::InUse;
	db.exec("UPDATE data_sets SET current_tree_id = NULL, baseline_tree_id = NULL, "
			"published_release_id = NULL WHERE id = $1;",
			{ BindValue(id) });
	db.exec("DELETE FROM data_sets WHERE id = $1;", { BindValue(id) });
	tx.commit();
	return DataSetOutcome::Ok;
}

std::optional<DataSetRow> find_data_set(db::Database &db, int64_t id) {
	const auto rows = query_data_sets(db, "WHERE id = $1", { BindValue(id) });
	if (rows.empty())
		return std::nullopt;
	return rows.front();
}

std::vector<DataSetRow> list_data_sets(db::Database &db) {
	return query_data_sets(db, "ORDER BY (kind = 'expansion'), id", {});
}

std::vector<DataSetRow> list_expansions_of(db::Database &db, int64_t parent_id) {
	return query_data_sets(db, "WHERE parent_id = $1 ORDER BY id", { BindValue(parent_id) });
}

int count_live_servers_of(db::Database &db, int64_t id) {
	const auto rows = db.query(std::string("SELECT count(*) FROM hosted_servers WHERE ") +
					kLiveServerSql + " AND (data_set_id = $1 OR expansion_id = $1);",
			{ BindValue(id) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

int count_outdated_servers_of(db::Database &db, int64_t id) {
	const auto rows = db.query(std::string("SELECT count(*) FROM hosted_servers s WHERE ") +
					kLiveServerSql + " AND (s.data_set_id = $1 OR s.expansion_id = $1) AND (" +
					kOutdatedSql + ");",
			{ BindValue(id) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

std::vector<OfferedDataSet> offered_data_sets(db::Database &db) {
	std::vector<OfferedDataSet> out;
	const auto rows = db.query(
			"SELECT d.id, d.display_name, d.kind, d.parent_id, d.expansion_code, d.title, d.game, "
			"t.id, t.version_text, t.missions::text "
			"FROM data_sets d JOIN data_set_trees t ON t.id = d.current_tree_id "
			"WHERE t.state = 'ready' ORDER BY (d.kind = 'expansion'), d.id;");
	for (const auto &r : rows) {
		const bool base = r.as_text(2).value_or("") == "base";
		if (base) {
			OfferedDataSet d;
			d.id = r.as_int(0).value_or(0);
			d.display_name = r.as_text(1).value_or("");
			d.title = r.as_text(5).value_or("");
			d.game = r.as_text(6).value_or("");
			d.tree_id = r.as_int(7).value_or(0);
			d.version = r.as_text(8).value_or("");
			decode_tree_missions(r.as_text(9).value_or("[]"), d.missions);
			out.push_back(std::move(d));
			continue;
		}
		const int64_t parent = r.as_int(3).value_or(0);
		for (OfferedDataSet &d : out) {
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

int64_t create_tree(db::Database &db, int64_t data_set_id, TreeSource source, TreeState state,
		std::optional<int64_t> release_id, std::optional<int64_t> baseline_tree_id,
		const std::string &created_by) {
	const auto rows = db.query(
			"INSERT INTO data_set_trees (data_set_id, state, source, release_id, baseline_tree_id, "
			"created_by) VALUES ($1, $2, $3, $4, $5, $6) RETURNING id;",
			{ BindValue(data_set_id), BindValue(tree_state_name(state)),
					BindValue(tree_source_name(source)), optional_id(release_id),
					optional_id(baseline_tree_id), BindValue(created_by) });
	return rows.front().as_int(0).value_or(0);
}

std::optional<DataSetTree> find_tree(db::Database &db, int64_t id) {
	const auto rows = query_trees(db, "WHERE id = $1", { BindValue(id) });
	if (rows.empty())
		return std::nullopt;
	return rows.front();
}

std::vector<DataSetTree> list_trees(db::Database &db, int64_t data_set_id) {
	return query_trees(db, "WHERE data_set_id = $1 ORDER BY id DESC", { BindValue(data_set_id) });
}

bool set_tree_state(db::Database &db, int64_t id, TreeState from, TreeState to,
		const std::string &error) {
	db.exec("UPDATE data_set_trees SET state = $3, error = $4 WHERE id = $1 AND state = $2;",
			{ BindValue(id), BindValue(tree_state_name(from)), BindValue(tree_state_name(to)),
					BindValue(error) });
	return db.changes() > 0;
}

bool finish_tree(db::Database &db, int64_t id, TreeState from, const TreeReady &ready) {
	db::Transaction tx(db);
	const auto rows = db.query(
			"UPDATE data_set_trees SET state = 'ready', ready_at = now(), error = '', "
			"manifest = $3::jsonb, missions = $4::jsonb, version_text = $5, bytes = $6 "
			"WHERE id = $1 AND state = $2 RETURNING data_set_id, source;",
			{ BindValue(id), BindValue(tree_state_name(from)), BindValue(encode_tree_files(ready.files)),
					BindValue(encode_tree_missions(ready.missions)), BindValue(ready.version_text),
					BindValue(ready.bytes) });
	if (rows.empty())
		return false;
	const int64_t data_set_id = rows.front().as_int(0).value_or(0);
	const bool baseline = rows.front().as_text(1).value_or("") == "baseline";
	const auto locked = db.query(
			"SELECT published_release_id FROM data_sets WHERE id = $1 FOR NO KEY UPDATE;",
			{ BindValue(data_set_id) });
	if (baseline) {
		db.exec("UPDATE data_sets SET current_tree_id = $2, baseline_tree_id = $2, "
				"published_release_id = NULL, updated_at = now() WHERE id = $1;",
				{ BindValue(data_set_id), BindValue(id) });
		if (!locked.empty())
			if (const auto release = locked.front().as_int(0))
				retire_if_unreferenced(db, *release);
	} else {
		db.exec("UPDATE data_sets SET current_tree_id = $2, updated_at = now() WHERE id = $1;",
				{ BindValue(data_set_id), BindValue(id) });
	}
	tx.commit();
	return true;
}

std::vector<int64_t> prunable_trees(db::Database &db, int64_t min_age_s, int64_t abandoned_upload_s) {
	std::vector<int64_t> out;
	const auto rows = db.query(
			std::string("SELECT t.id FROM data_set_trees t WHERE "
						"(t.state = 'pruned' AND t.pruned_at IS NULL) "
						"OR (t.state = 'uploading' AND t.created_at < now() - make_interval(secs => $2)) "
						"OR (t.state IN ('ready', 'failed') AND t.created_at < now() - make_interval(secs => $1) "
						"    AND NOT EXISTS (SELECT 1 FROM data_sets d WHERE d.current_tree_id = t.id OR d.baseline_tree_id = t.id) "
						"    AND NOT EXISTS (SELECT 1 FROM hosted_servers s WHERE ") +
					kLiveServerSql +
					"        AND (s.base_tree_id = t.id OR s.expansion_tree_id = t.id))) "
					"ORDER BY t.id;",
			{ BindValue(static_cast<double>(min_age_s)), BindValue(static_cast<double>(abandoned_upload_s)) });
	for (const auto &r : rows)
		out.push_back(r.as_int(0).value_or(0));
	return out;
}

void mark_tree_pruned(db::Database &db, int64_t id) {
	db.exec("UPDATE data_set_trees SET state = 'pruned', pruned_at = now() WHERE id = $1;",
			{ BindValue(id) });
}

void request_tree_prune(db::Database &db, int64_t id) {
	db.exec("UPDATE data_set_trees SET state = 'pruned', pruned_at = NULL WHERE id = $1;",
			{ BindValue(id) });
}

int count_tree_pins(db::Database &db, int64_t id) {
	const auto rows = db.query(std::string("SELECT count(*) FROM hosted_servers WHERE ") +
					kLiveServerSql + " AND (base_tree_id = $1 OR expansion_tree_id = $1);",
			{ BindValue(id) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

std::string tree_hold(db::Database &db, int64_t id, int &pinned_by) {
	pinned_by = 0;
	if (!db.query("SELECT 1 FROM data_sets WHERE current_tree_id = $1;", { BindValue(id) }).empty())
		return "current";
	if (!db.query("SELECT 1 FROM data_sets WHERE baseline_tree_id = $1;", { BindValue(id) }).empty())
		return "baseline";
	const auto rows = db.query(std::string("SELECT count(*) FROM hosted_servers WHERE ") +
					kLiveServerSql + " AND (base_tree_id = $1 OR expansion_tree_id = $1);",
			{ BindValue(id) });
	pinned_by = rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
	return pinned_by > 0 ? "pinned" : "";
}

// --- releases ---

int64_t create_release(db::Database &db, const std::string &name, int64_t bytes,
		const std::string &created_by) {
	const auto rows = db.query(
			"INSERT INTO releases (name, bytes, created_by) VALUES ($1, $2, $3) RETURNING id;",
			{ BindValue(name), BindValue(bytes), BindValue(created_by) });
	return rows.front().as_int(0).value_or(0);
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
				 "SELECT release_id, data_set_id, expansion, command_file, version, row_count, "
				 "blob_bytes, tree_id FROM release_sections WHERE release_id = $1 ORDER BY (expansion <> ''), data_set_id;",
				 { BindValue(release_id) })) {
		ReleaseSection s;
		s.release_id = r.as_int(0).value_or(0);
		s.data_set_id = r.as_int(1).value_or(0);
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
			"package_product = $4 WHERE id = $1 AND status = 'validating';",
			{ BindValue(id), BindValue(sha256), BindValue(bytes), BindValue(package_product) });
	if (db.changes() == 0)
		return false;
	db.exec("DELETE FROM release_sections WHERE release_id = $1;", { BindValue(id) });
	for (const NewReleaseSection &s : sections)
		db.exec("INSERT INTO release_sections (release_id, data_set_id, expansion, command_file, "
				"version, row_count, blob_bytes) VALUES ($1, $2, $3, $4, $5, $6, $7);",
				{ BindValue(id), BindValue(s.data_set_id), BindValue(s.expansion),
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
			"SELECT DISTINCT d.published_release_id FROM data_sets d "
			"JOIN release_sections s ON s.data_set_id = d.id "
			"WHERE s.release_id = $1 AND d.published_release_id IS NOT NULL AND d.published_release_id <> $1;",
			{ BindValue(id) });
	db.exec("UPDATE data_sets SET published_release_id = $1, updated_at = now() "
			"WHERE id IN (SELECT data_set_id FROM release_sections WHERE release_id = $1);",
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
	db.exec("UPDATE data_sets SET published_release_id = NULL, updated_at = now() "
			"WHERE published_release_id = $1;",
			{ BindValue(id) });
	tx.commit();
	return true;
}

bool delete_release(db::Database &db, int64_t id) {
	db.exec("DELETE FROM releases WHERE id = $1;", { BindValue(id) });
	return db.changes() > 0;
}

void set_section_tree(db::Database &db, int64_t release_id, int64_t data_set_id, int64_t tree_id) {
	db.exec("UPDATE release_sections SET tree_id = $3 WHERE release_id = $1 AND data_set_id = $2;",
			{ BindValue(release_id), BindValue(data_set_id), BindValue(tree_id) });
}

std::vector<PublishedSection> published_sections(db::Database &db) {
	std::vector<PublishedSection> out;
	for (const auto &r : db.query(
				 "SELECT d.id, r.id, d.updater_login, s.command_file, s.expansion, r.sha256, d.retail_updater "
				 "FROM data_sets d JOIN releases r ON r.id = d.published_release_id "
				 "JOIN release_sections s ON s.release_id = r.id AND s.data_set_id = d.id "
				 "WHERE d.updater_login <> '' AND r.status = 'published' ORDER BY d.updater_login;")) {
		PublishedSection p;
		p.data_set_id = r.as_int(0).value_or(0);
		p.release_id = r.as_int(1).value_or(0);
		p.updater_login = r.as_text(2).value_or("");
		p.command_file = r.as_text(3).value_or("");
		p.expansion = r.as_text(4).value_or("");
		p.sha256 = r.as_text(5).value_or("");
		p.retail_updater = r.as_int(6).value_or(0) != 0;
		out.push_back(std::move(p));
	}
	return out;
}

// --- the hosted servers' pins ---

std::optional<CurrentTrees> current_trees_of(db::Database &db, int64_t data_set_id,
		std::optional<int64_t> expansion_id) {
	const auto base = db.query(
			"SELECT d.current_tree_id FROM data_sets d JOIN data_set_trees t ON t.id = d.current_tree_id "
			"WHERE d.id = $1 AND d.kind = 'base' AND t.state = 'ready';",
			{ BindValue(data_set_id) });
	if (base.empty() || !base.front().as_int(0))
		return std::nullopt;
	CurrentTrees trees;
	trees.base_tree_id = *base.front().as_int(0);
	if (expansion_id) {
		const auto exp = db.query(
				"SELECT d.current_tree_id FROM data_sets d JOIN data_set_trees t ON t.id = d.current_tree_id "
				"WHERE d.id = $1 AND d.kind = 'expansion' AND d.parent_id = $2 AND t.state = 'ready';",
				{ BindValue(*expansion_id), BindValue(data_set_id) });
		if (exp.empty() || !exp.front().as_int(0))
			return std::nullopt;
		trees.expansion_tree_id = exp.front().as_int(0);
	}
	return trees;
}

void set_tree_pins(db::Database &db, int64_t server_id, int64_t base_tree_id,
		std::optional<int64_t> expansion_tree_id) {
	db.exec("UPDATE hosted_servers SET base_tree_id = $2, expansion_tree_id = $3, "
			"update_requested = false, updated_at = now() WHERE id = $1;",
			{ BindValue(server_id), BindValue(base_tree_id), optional_id(expansion_tree_id) });
}

UpdateOutcome request_update(db::Database &db, int64_t server_id) {
	db::Transaction tx(db);
	const auto rows = db.query(
			std::string("SELECT update_requested, (") + kLiveServerSql + ") AS live, (" + kOutdatedSql +
					") AS outdated FROM hosted_servers s WHERE id = $1 FOR NO KEY UPDATE;",
			{ BindValue(server_id) });
	if (rows.empty())
		return UpdateOutcome::NotFound;
	const bool requested = rows.front().as_int(0).value_or(0) != 0;
	const bool live = rows.front().as_int(1).value_or(0) != 0;
	const bool outdated = rows.front().as_int(2).value_or(0) != 0;
	if (!live)
		return UpdateOutcome::NotLive;
	if (requested) {
		tx.commit();
		return UpdateOutcome::Requested;
	}
	if (!outdated)
		return UpdateOutcome::UpToDate;
	db.exec("UPDATE hosted_servers SET update_requested = true, updated_at = now() WHERE id = $1;",
			{ BindValue(server_id) });
	tx.commit();
	return UpdateOutcome::Requested;
}

size_t request_update_all(db::Database &db, std::optional<int64_t> data_set_id) {
	std::string sql = std::string("UPDATE hosted_servers s SET update_requested = true, updated_at = now() WHERE ") +
			kLiveServerSql + " AND NOT update_requested AND (" + kOutdatedSql + ")";
	if (data_set_id) {
		db.exec(sql + " AND (s.data_set_id = $1 OR s.expansion_id = $1);", { BindValue(*data_set_id) });
	} else {
		db.exec(sql + ";");
	}
	return static_cast<size_t>(db.changes());
}

bool server_outdated(db::Database &db, int64_t server_id) {
	const auto rows = db.query(
			std::string("SELECT 1 FROM hosted_servers s WHERE id = $1 AND ") + kLiveServerSql + " AND (" +
					kOutdatedSql + ");",
			{ BindValue(server_id) });
	return !rows.empty();
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
