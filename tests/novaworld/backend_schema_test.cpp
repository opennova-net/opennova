// Apply backend/migrations/* + backend/seed/* against a fresh in-process
// SQLite DB and verify the schema + canonical seed data are intact. Catches
// SQL syntax errors and seed regressions at CI time before the standalone
// server boots them in production.

#include <novaworld/db/sqlite.h>

#include "../common/test_expect.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

using opennova::db::Database;
using opennova::db::run_migrations;

namespace {

std::string read_file(const std::filesystem::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream os;
	os << in.rdbuf();
	return os.str();
}

int test_migrations_create_expected_tables() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";

	Database db(":memory:");
	auto r = run_migrations(db, migrations);
	TEST_EXPECT(!r.applied.empty());

	auto rows = db.query(
		"SELECT name FROM sqlite_master WHERE type='table' "
		"  AND name NOT LIKE 'sqlite_%' AND name <> '_schema_migrations' "
		"ORDER BY name;"
	);
	std::printf("  applied %zu migration(s); created %zu table(s)\n",
	            r.applied.size(), rows.size());

	// Expected tables from the backend migration set.
	const std::vector<std::string> expected = {
		"active_hosts", "active_user_sessions",
		"expansion_files", "expansion_releases", "expansions",
		"games", "host_players", "hosts", "player_game_access",
		"players", "server_status", "unknown_messages"
	};
	TEST_EXPECT(rows.size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		TEST_EXPECT(rows[i].as_text(0).value() == expected[i]);
	}
	return 0;
}

int test_seed_populates_games_and_expansions() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";

	Database db(":memory:");
	run_migrations(db, migrations);

	for (const auto &entry : std::filesystem::directory_iterator(seed_dir)) {
		if (entry.path().extension() != ".sql") continue;
		db.exec_script(read_file(entry.path()));
	}

	auto games = db.query("SELECT slug, gate_tag FROM games ORDER BY slug;");
	TEST_EXPECT(games.size() == 2);
	TEST_EXPECT(games[0].as_text(0).value() == "dfx2_consumer");
	TEST_EXPECT(games[0].as_text(1).value() == "dfx2:0:cus:buffy");
	TEST_EXPECT(games[1].as_text(0).value() == "jop_2_consumer");
	TEST_EXPECT(games[1].as_text(1).value() == "jop:cus2");

	auto expansions = db.query(
		"SELECT slug, display_name, featured FROM expansions ORDER BY slug;"
	);
	TEST_EXPECT(expansions.size() == 3);
	TEST_EXPECT(expansions[0].as_text(0).value() == "ondx01");
	TEST_EXPECT(expansions[1].as_text(0).value() == "onjo01");
	TEST_EXPECT(expansions[2].as_text(0).value() == "revx02");
	TEST_EXPECT(expansions[2].as_int(2).value() == 1); // featured

	auto files = db.query(
		"SELECT e.slug, COUNT(f.id) "
		"FROM expansions e LEFT JOIN expansion_files f ON f.expansion_id = e.id "
		"GROUP BY e.slug ORDER BY e.slug;"
	);
	TEST_EXPECT(files.size() == 3);
	for (const auto &row : files) {
		const auto slug = row.as_text(0).value();
		const auto count = row.as_int(1).value();
		if (slug == "revx02") TEST_EXPECT(count == 1);
		else TEST_EXPECT(count == 0);
	}

	auto access = db.query(
		"SELECT p.username, a.game_slug, a.status, a.exp_bits "
		"FROM player_game_access a JOIN players p ON p.id = a.user_id "
		"ORDER BY p.username, a.game_slug;"
	);
	TEST_EXPECT(access.size() == 2);
	TEST_EXPECT(access[0].as_text(0).value() == "foo");
	TEST_EXPECT(access[0].as_text(1).value() == "jop_2_consumer");
	TEST_EXPECT(access[0].as_text(2).value() == "active");
	TEST_EXPECT(access[0].as_text(3).value() == "3");
	TEST_EXPECT(access[1].as_text(0).value() == "test");
	TEST_EXPECT(access[1].as_text(1).value() == "jop_2_consumer");
	TEST_EXPECT(access[1].as_text(2).value() == "active");
	TEST_EXPECT(access[1].as_text(3).value() == "3");
	return 0;
}

int test_seed_is_idempotent() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";

	Database db(":memory:");
	run_migrations(db, migrations);

	std::vector<std::filesystem::path> seeds;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() == ".sql") seeds.push_back(e.path());
	}

	// Run the seed twice; INSERT OR IGNORE should keep counts stable.
	for (int pass = 0; pass < 2; ++pass) {
		for (const auto &p : seeds) {
			db.exec_script(read_file(p));
		}
	}

	auto games = db.query("SELECT COUNT(*) FROM games;");
	TEST_EXPECT(games[0].as_int(0).value() == 2);
	auto expansions = db.query("SELECT COUNT(*) FROM expansions;");
	TEST_EXPECT(expansions[0].as_int(0).value() == 3);
	auto files = db.query("SELECT COUNT(*) FROM expansion_files;");
	TEST_EXPECT(files[0].as_int(0).value() == 1);
	return 0;
}

int test_foreign_keys_enforced() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";

	Database db(":memory:");
	run_migrations(db, migrations);

	// Inserting an expansion against a non-existent game_id should fail.
	bool caught = false;
	try {
		db.exec(
			"INSERT INTO expansions "
			"(game_id, slug, display_name, version, install_subdir) "
			"VALUES (9999, 'orphan', 'Orphan', '0.0.0', 'expansions/orphan');"
		);
	} catch (const opennova::db::SqliteError &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	return 0;
}

} // namespace

int main() {
	if (test_migrations_create_expected_tables() != 0) return 1;
	if (test_seed_populates_games_and_expansions() != 0) return 1;
	if (test_seed_is_idempotent() != 0) return 1;
	if (test_foreign_keys_enforced() != 0) return 1;
	std::printf("OK: backend schema + seed (migrations, seed idempotency, FKs)\n");
	return 0;
}
