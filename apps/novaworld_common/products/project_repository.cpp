#include "project_repository.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>

namespace opennova::novaworld_server::projects {

namespace {

using db::BindValue;

constexpr std::array<const char *, 2> kVisibilityNames = { "public", "private" };

constexpr const char *kRevisionColumns = "product_id, number, author_id, created_by, message, "
										 "manifest_sha256, file_count, bytes, created_at";

ProjectRevision row_to_revision(const db::Row &r) {
	ProjectRevision v;
	v.product_id = r.as_int(0).value_or(0);
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

bool project_is_public(const hosted::ProductRow &game) {
	return game.kind != hosted::ProductKind::App &&
			visibility_from_name(game.visibility) == Visibility::Public;
}

ProjectRights project_rights(const hosted::ProductRow &project, const ProjectCaller &caller) {
	ProjectRights rights;
	if (project.kind == hosted::ProductKind::App)
		return rights;
	const bool owner = caller.user_id && project.owner_id == caller.user_id;
	rights.manage = caller.admin || owner;
	rights.read = rights.manage || project_is_public(project);
	rights.push = rights.manage && caller.user_id.has_value();
	return rights;
}

bool valid_key(std::string_view key) {
	if (key.empty() || key.size() > 16)
		return false;
	for (const char c : key)
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	return true;
}

std::vector<ProjectRevision> list_revisions(
		db::Database &db, int64_t product_id, int64_t before, int limit) {
	const auto rows = db.query(std::string("SELECT ") + kRevisionColumns +
					" FROM project_revisions WHERE product_id = $1 AND ($2 = 0 OR number < $2) "
					"ORDER BY number DESC LIMIT $3;",
			{ BindValue(product_id), BindValue(before),
					BindValue(static_cast<int64_t>(std::max(limit, 0))) });
	std::vector<ProjectRevision> out;
	out.reserve(rows.size());
	for (const auto &r : rows)
		out.push_back(row_to_revision(r));
	return out;
}

std::optional<ProjectRevision> find_revision(db::Database &db, int64_t product_id, int64_t number) {
	const auto rows = db.query(std::string("SELECT ") + kRevisionColumns +
					" FROM project_revisions WHERE product_id = $1 AND number = $2;",
			{ BindValue(product_id), BindValue(number) });
	if (rows.empty())
		return std::nullopt;
	return row_to_revision(rows.front());
}

std::string blob_key(int64_t product_id, std::string_view sha256) {
	return project_prefix(product_id) + "blobs/" + std::string(sha256);
}

std::string manifest_key(int64_t product_id, std::string_view sha256) {
	return project_prefix(product_id) + "manifests/" + std::string(sha256) + ".json";
}

std::string project_prefix(int64_t product_id) {
	return "content/" + std::to_string(static_cast<long long>(product_id)) + "/";
}

std::vector<StoredBlob> missing_blobs(
		db::Database &db, int64_t product_id, const std::vector<StoredBlob> &wanted) {
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
	const auto rows = db.query("SELECT sha256, bytes FROM project_blobs "
							   "WHERE product_id = $1 AND sha256 = ANY($2::text[]);",
			{ BindValue(product_id), BindValue(sha_array(shas)) });
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

void record_blobs(db::Database &db, int64_t product_id, const std::vector<StoredBlob> &blobs) {
	db::Transaction tx(db);
	for (const StoredBlob &blob : blobs)
		db.exec("INSERT INTO project_blobs (product_id, sha256, bytes) VALUES ($1, $2, $3) "
				"ON CONFLICT (product_id, sha256) DO NOTHING;",
				{ BindValue(product_id), BindValue(blob.sha256), BindValue(blob.bytes) });
	tx.commit();
}

ProjectStorage project_storage(db::Database &db, int64_t product_id) {
	const auto rows = db.query("SELECT count(*), COALESCE(sum(bytes), 0)::bigint "
							   "FROM project_blobs WHERE product_id = $1;",
			{ BindValue(product_id) });
	ProjectStorage storage;
	if (!rows.empty()) {
		storage.blobs = rows.front().as_int(0).value_or(0);
		storage.bytes = rows.front().as_int(1).value_or(0);
	}
	return storage;
}

OwnedUsage owned_usage(db::Database &db, int64_t owner_id) {
	const auto rows = db.query("SELECT (SELECT count(*) FROM products WHERE owner_id = $1), "
							   "(SELECT COALESCE(sum(b.bytes), 0)::bigint FROM project_blobs b "
							   "JOIN products p ON p.id = b.product_id WHERE p.owner_id = $1);",
			{ BindValue(owner_id) });
	OwnedUsage usage;
	if (!rows.empty()) {
		usage.projects = rows.front().as_int(0).value_or(0);
		usage.bytes = rows.front().as_int(1).value_or(0);
	}
	return usage;
}

CommitOutcome commit_revision(db::Database &db, int64_t product_id, int64_t parent,
		const NewRevision &revision, int64_t &head) {
	db::Transaction tx(db);
	const auto moved = db.query("UPDATE products SET head = head + 1, updated_at = now() "
								"WHERE id = $1 AND kind <> 'app' AND head = $2 RETURNING head;",
			{ BindValue(product_id), BindValue(parent) });
	if (moved.empty()) {
		const auto now = db.query("SELECT head FROM products WHERE id = $1 AND kind <> 'app';",
				{ BindValue(product_id) });
		if (now.empty())
			return CommitOutcome::NotFound;
		head = now.front().as_int(0).value_or(0);
		return CommitOutcome::Stale;
	}
	head = moved.front().as_int(0).value_or(0);
	db.exec("INSERT INTO project_revisions (product_id, number, author_id, created_by, message, "
			"manifest_sha256, file_count, bytes) VALUES ($1, $2, $3, $4, $5, $6, $7, $8);",
			{ BindValue(product_id), BindValue(head), db::int_or_null(revision.author_id),
					BindValue(revision.created_by), BindValue(revision.message),
					BindValue(revision.manifest_sha256), BindValue(revision.file_count),
					BindValue(revision.bytes) });
	tx.commit();
	return CommitOutcome::Ok;
}

} // namespace opennova::novaworld_server::projects
