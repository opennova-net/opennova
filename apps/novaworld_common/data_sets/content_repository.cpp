#include "content_repository.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>

namespace opennova::novaworld_server::content {

namespace {

using db::BindValue;

constexpr std::array<const char *, 2> kVisibilityNames = { "public", "private" };

constexpr const char *kProjectColumns = "id, key, display_name, visibility, retail, parent_id, "
										"head, created_by, created_at, updated_at";
constexpr const char *kRevisionColumns = "data_set_id, number, author_id, created_by, message, "
										 "manifest_sha256, file_count, bytes, created_at";

BindValue optional_id(std::optional<int64_t> id) {
	if (!id)
		return BindValue(std::monostate{});
	return BindValue(*id);
}

ContentProject row_to_project(const db::Row &r) {
	ContentProject p;
	p.id = r.as_int(0).value_or(0);
	p.key = r.as_text(1).value_or("");
	p.title = r.as_text(2).value_or("");
	p.visibility = visibility_from_name(r.as_text(3).value_or("")).value_or(Visibility::Private);
	p.retail = r.as_int(4).value_or(0) != 0;
	p.parent_id = r.as_int(5);
	p.head = r.as_int(6).value_or(0);
	p.created_by = r.as_text(7).value_or("");
	p.created_at = r.as_text(8).value_or("");
	p.updated_at = r.as_text(9).value_or("");
	return p;
}

ContentRevision row_to_revision(const db::Row &r) {
	ContentRevision v;
	v.data_set_id = r.as_int(0).value_or(0);
	v.number = r.as_int(1).value_or(0);
	v.author_id = r.as_int(2);
	v.created_by = r.as_text(3).value_or("");
	v.message = r.as_text(4).value_or("");
	v.manifest_sha256 = r.as_text(5).value_or("");
	v.file_count = r.as_int(6).value_or(0);
	v.bytes = r.as_int(7).value_or(0);
	v.created_at = r.as_text(8).value_or("");
	return v;
}

// The games with a project matching `tail` (a condition after "AND", then the order).
std::vector<ContentProject> query_projects(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	const auto rows = db.query(std::string("SELECT ") + kProjectColumns +
					" FROM data_sets WHERE key IS NOT NULL " + tail + ";",
			binds);
	std::vector<ContentProject> out;
	out.reserve(rows.size());
	for (const auto &r : rows)
		out.push_back(row_to_project(r));
	return out;
}

// A text[] literal of SHA-256s (hex digits alone, so nothing in them needs quoting).
std::string sha_array(const std::vector<std::string> &items) {
	std::string out = "{";
	for (size_t i = 0; i < items.size(); ++i) {
		if (i > 0)
			out += ',';
		out += items[i];
	}
	return out + "}";
}

} // namespace

const char *visibility_name(Visibility visibility) {
	return kVisibilityNames[static_cast<size_t>(visibility)];
}

std::optional<Visibility> visibility_from_name(std::string_view name) {
	for (size_t i = 0; i < kVisibilityNames.size(); ++i)
		if (name == kVisibilityNames[i])
			return static_cast<Visibility>(i);
	return std::nullopt;
}

bool valid_key(std::string_view key) {
	if (key.empty() || key.size() > 16)
		return false;
	for (const char c : key)
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	return true;
}

std::vector<ContentProject> list_projects(db::Database &db, bool include_private) {
	return query_projects(db,
			include_private ? "ORDER BY key COLLATE \"C\""
							: "AND visibility = 'public' ORDER BY key COLLATE \"C\"",
			{});
}

std::optional<ContentProject> find_project(db::Database &db, std::string_view key) {
	const auto rows = query_projects(db, "AND key = $1", { BindValue(std::string(key)) });
	if (rows.empty())
		return std::nullopt;
	return rows.front();
}

std::optional<ContentProject> find_project_by_id(db::Database &db, int64_t data_set_id) {
	const auto rows = query_projects(db, "AND id = $1", { BindValue(data_set_id) });
	if (rows.empty())
		return std::nullopt;
	return rows.front();
}

std::vector<ContentRevision> list_revisions(
		db::Database &db, int64_t data_set_id, int64_t before, int limit) {
	const auto rows = db.query(std::string("SELECT ") + kRevisionColumns +
					" FROM content_revisions WHERE data_set_id = $1 AND ($2 = 0 OR number < $2) "
					"ORDER BY number DESC LIMIT $3;",
			{ BindValue(data_set_id), BindValue(before),
					BindValue(static_cast<int64_t>(std::max(limit, 0))) });
	std::vector<ContentRevision> out;
	out.reserve(rows.size());
	for (const auto &r : rows)
		out.push_back(row_to_revision(r));
	return out;
}

std::optional<ContentRevision> find_revision(db::Database &db, int64_t data_set_id, int64_t number) {
	const auto rows = db.query(std::string("SELECT ") + kRevisionColumns +
					" FROM content_revisions WHERE data_set_id = $1 AND number = $2;",
			{ BindValue(data_set_id), BindValue(number) });
	if (rows.empty())
		return std::nullopt;
	return row_to_revision(rows.front());
}

std::string blob_key(int64_t data_set_id, std::string_view sha256) {
	return project_prefix(data_set_id) + "blobs/" + std::string(sha256);
}

std::string manifest_key(int64_t data_set_id, std::string_view sha256) {
	return project_prefix(data_set_id) + "manifests/" + std::string(sha256) + ".json";
}

std::string project_prefix(int64_t data_set_id) {
	return "content/" + std::to_string(static_cast<long long>(data_set_id)) + "/";
}

std::vector<StoredBlob> missing_blobs(
		db::Database &db, int64_t data_set_id, const std::vector<StoredBlob> &wanted) {
	std::vector<StoredBlob> unique;
	std::vector<std::string> shas;
	std::unordered_set<std::string> seen;
	for (const StoredBlob &blob : wanted)
		if (seen.insert(blob.sha256).second) {
			unique.push_back(blob);
			shas.push_back(blob.sha256);
		}
	if (unique.empty())
		return {};
	const auto rows = db.query("SELECT sha256, bytes FROM content_blobs "
							   "WHERE data_set_id = $1 AND sha256 = ANY($2::text[]);",
			{ BindValue(data_set_id), BindValue(sha_array(shas)) });
	std::unordered_map<std::string, int64_t> held;
	for (const auto &r : rows)
		held.emplace(r.as_text(0).value_or(""), r.as_int(1).value_or(-1));
	std::vector<StoredBlob> missing;
	for (const StoredBlob &blob : unique) {
		const auto at = held.find(blob.sha256);
		if (at == held.end() || at->second != blob.bytes)
			missing.push_back(blob);
	}
	return missing;
}

void record_blobs(db::Database &db, int64_t data_set_id, const std::vector<StoredBlob> &blobs) {
	db::Transaction tx(db);
	for (const StoredBlob &blob : blobs)
		db.exec("INSERT INTO content_blobs (data_set_id, sha256, bytes) VALUES ($1, $2, $3) "
				"ON CONFLICT (data_set_id, sha256) DO NOTHING;",
				{ BindValue(data_set_id), BindValue(blob.sha256), BindValue(blob.bytes) });
	tx.commit();
}

ProjectStorage project_storage(db::Database &db, int64_t data_set_id) {
	const auto rows = db.query("SELECT count(*), COALESCE(sum(bytes), 0)::bigint "
							   "FROM content_blobs WHERE data_set_id = $1;",
			{ BindValue(data_set_id) });
	ProjectStorage storage;
	if (!rows.empty()) {
		storage.blobs = rows.front().as_int(0).value_or(0);
		storage.bytes = rows.front().as_int(1).value_or(0);
	}
	return storage;
}

CommitOutcome commit_revision(db::Database &db, int64_t data_set_id, int64_t parent,
		const NewRevision &revision, int64_t &head) {
	db::Transaction tx(db);
	const auto moved = db.query("UPDATE data_sets SET head = head + 1, updated_at = now() "
								"WHERE id = $1 AND key IS NOT NULL AND head = $2 RETURNING head;",
			{ BindValue(data_set_id), BindValue(parent) });
	if (moved.empty()) {
		const auto now = db.query("SELECT head FROM data_sets WHERE id = $1 AND key IS NOT NULL;",
				{ BindValue(data_set_id) });
		if (now.empty())
			return CommitOutcome::NotFound;
		head = now.front().as_int(0).value_or(0);
		return CommitOutcome::Stale;
	}
	head = moved.front().as_int(0).value_or(0);
	db.exec("INSERT INTO content_revisions (data_set_id, number, author_id, created_by, message, "
			"manifest_sha256, file_count, bytes) VALUES ($1, $2, $3, $4, $5, $6, $7, $8);",
			{ BindValue(data_set_id), BindValue(head), optional_id(revision.author_id),
					BindValue(revision.created_by), BindValue(revision.message),
					BindValue(revision.manifest_sha256), BindValue(revision.file_count),
					BindValue(revision.bytes) });
	tx.commit();
	return CommitOutcome::Ok;
}

} // namespace opennova::novaworld_server::content
