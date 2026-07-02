// Exercises the expansion-release / publish repository write layer
// (apps/novaworld_server/catalog_repository.cpp) against a fresh in-process
// SQLite DB. Covers the SQL-dialect translations ported from onnet
// (ON CONFLICT reset, COALESCE-preserving status updates, DELETE-then-INSERT
// file upsert). The HTTP routing + libcurl GitHub call are not exercised
// here — that's a manual integration smoke (see DEPLOY.md). This is where the
// SQL risk lives, so it's the part under test.

#include "catalog_repository.h"

#include <novaworld/db/sqlite.h>

#include "../common/test_expect.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

using opennova::db::Database;
using opennova::db::run_migrations;
namespace catalog = opennova::server::catalog;

namespace {

std::string read_file(const std::filesystem::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream os;
	os << in.rdbuf();
	return os.str();
}

// Fresh DB with migrations + seed applied (seed provides the revx02/onjo01/
// ondx01 expansions the write methods operate on).
Database make_seeded_db() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir   = source_dir / "backend" / "seed";

	Database db(":memory:");
	run_migrations(db, migrations);

	std::vector<std::filesystem::path> seeds;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() == ".sql") seeds.push_back(e.path());
	}
	std::sort(seeds.begin(), seeds.end());
	for (const auto &p : seeds) db.exec_script(read_file(p));
	return db;
}

int test_find_expansion() {
	Database db = make_seeded_db();
	auto found = catalog::find_expansion_by_slug(db, "revx02");
	TEST_EXPECT(found.found);
	TEST_EXPECT(found.id > 0);
	TEST_EXPECT(found.game_id > 0);
	// github_repo comes from the Terraform-generated catalogue seed and is what
	// the admin release handler tags (replaces the old hardcoded slug->repo map).
	TEST_EXPECT(found.github_repo == "opennova-net/revx02");

	auto missing = catalog::find_expansion_by_slug(db, "does-not-exist");
	TEST_EXPECT(!missing.found);
	return 0;
}

int test_set_expansion_version() {
	Database db = make_seeded_db();
	auto exp = catalog::find_expansion_by_slug(db, "onjo01");
	catalog::set_expansion_version(db, exp.id, "9.9.9");
	auto after = catalog::find_expansion_by_slug(db, "onjo01");
	TEST_EXPECT(after.version == "9.9.9");
	return 0;
}

int test_create_release_and_reset() {
	Database db = make_seeded_db();
	catalog::create_or_reset_release(db, "revx02", "1.0.0", "revx02-v1.0.0",
	                                 std::optional<std::string>{"first cut"});
	auto rel = catalog::get_release(db, "revx02", "1.0.0");
	TEST_EXPECT(rel.has_value());
	TEST_EXPECT(rel->status == "pending");
	TEST_EXPECT(rel->repo_ref == "revx02-v1.0.0");
	TEST_EXPECT(!rel->published_at.has_value());

	// Move it forward to 'tagged' with a target commit.
	catalog::update_release_status(db, "revx02", "1.0.0", "tagged",
	                               std::nullopt, std::nullopt,
	                               std::optional<std::string>{"abc123"},
	                               std::nullopt);
	rel = catalog::get_release(db, "revx02", "1.0.0");
	TEST_EXPECT(rel->status == "tagged");
	TEST_EXPECT(rel->target_commit == "abc123");

	// Re-release the same (slug, version) -> ON CONFLICT resets it.
	catalog::create_or_reset_release(db, "revx02", "1.0.0", "revx02-v1.0.0",
	                                 std::nullopt);
	rel = catalog::get_release(db, "revx02", "1.0.0");
	TEST_EXPECT(rel->status == "pending");
	TEST_EXPECT(rel->target_commit.empty());      // cleared
	TEST_EXPECT(!rel->published_at.has_value());   // cleared
	return 0;
}

int test_publish_coalesce_preserves_commit() {
	Database db = make_seeded_db();
	catalog::create_or_reset_release(db, "revx02", "2.0.0", "revx02-v2.0.0",
	                                 std::nullopt);
	// Tag step sets the commit.
	catalog::update_release_status(db, "revx02", "2.0.0", "tagged",
	                               std::nullopt, std::nullopt,
	                               std::optional<std::string>{"deadbeef"},
	                               std::nullopt);
	// Publish callback: status published + published_at, but passes NULL for
	// target_commit -> COALESCE must keep the earlier commit.
	catalog::update_release_status(db, "revx02", "2.0.0", "published",
	                               std::nullopt,
	                               std::optional<std::string>{"https://ci/run/1"},
	                               std::nullopt,
	                               std::optional<std::string>{"2026-06-23 12:00:00"});
	auto rel = catalog::get_release(db, "revx02", "2.0.0");
	TEST_EXPECT(rel->status == "published");
	TEST_EXPECT(rel->target_commit == "deadbeef");        // preserved
	TEST_EXPECT(rel->published_at.has_value());
	TEST_EXPECT(*rel->published_at == "2026-06-23 12:00:00");
	TEST_EXPECT(rel->workflow_url == "https://ci/run/1");
	return 0;
}

int test_upsert_file_is_idempotent() {
	Database db = make_seeded_db();
	auto exp = catalog::find_expansion_by_slug(db, "ondx01");  // seed has 0 files

	catalog::upsert_expansion_file(db, exp.id,
	                               "https://dl/ondx01-v1.zip", "sha-aaa",
	                               std::optional<int64_t>{111});
	catalog::upsert_expansion_file(db, exp.id,
	                               "https://dl/ondx01-v2.zip", "sha-bbb",
	                               std::optional<int64_t>{222});

	auto rows = db.query(
		"SELECT download_url, sha256, size_bytes FROM expansion_files "
		"WHERE expansion_id = ?;",
		{opennova::db::BindValue(exp.id)});
	TEST_EXPECT(rows.size() == 1);                               // replaced, not appended
	TEST_EXPECT(rows[0].as_text(0).value() == "https://dl/ondx01-v2.zip");
	TEST_EXPECT(rows[0].as_text(1).value() == "sha-bbb");
	TEST_EXPECT(rows[0].as_int(2).value() == 222);
	return 0;
}

int test_fail_status_records_error() {
	Database db = make_seeded_db();
	catalog::create_or_reset_release(db, "onjo01", "3.0.0", "onjo01-v3.0.0",
	                                 std::nullopt);
	catalog::update_release_status(db, "onjo01", "3.0.0", "failed",
	                               std::optional<std::string>{"boom"},
	                               std::nullopt, std::nullopt, std::nullopt);
	auto rel = catalog::get_release(db, "onjo01", "3.0.0");
	TEST_EXPECT(rel->status == "failed");
	TEST_EXPECT(rel->error_message.has_value());
	TEST_EXPECT(*rel->error_message == "boom");
	return 0;
}

} // namespace

int main() {
	if (test_find_expansion() != 0) return 1;
	if (test_set_expansion_version() != 0) return 1;
	if (test_create_release_and_reset() != 0) return 1;
	if (test_publish_coalesce_preserves_commit() != 0) return 1;
	if (test_upsert_file_is_idempotent() != 0) return 1;
	if (test_fail_status_records_error() != 0) return 1;
	std::printf("OK: expansion publish repository (release lifecycle, file upsert)\n");
	return 0;
}
