// Apply backend/migrations/* + backend/seed/* against a fresh in-process
// SQLite DB and verify the schema + canonical seed data are intact. Catches
// SQL syntax errors and seed regressions at CI time before the standalone
// server boots them in production.

#include <net/novaworld/db/sqlite.h>

#include "../common/file_io.h"
#include "../common/temp_dir.h"
#include "../common/test_expect.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

using opennova::db::Database;
using opennova::db::run_migrations;

namespace {

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
		"games", "host_players", "host_roster", "hosts", "login_addresses", "player_game_access",
		"players", "server_status", "unknown_messages", "web_sessions"
	};
	TEST_EXPECT(rows.size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		TEST_EXPECT(rows[i].as_text(0).value() == expected[i]);
	}
	// 0007 retired the launcher's executable_name column.
	auto launcher_column = db.query(
		"SELECT name FROM pragma_table_info('games') WHERE name = 'executable_name';");
	TEST_EXPECT(launcher_column.empty());
	return 0;
}

int test_seed_populates_games() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";

	Database db(":memory:");
	run_migrations(db, migrations);

	// Apply seeds in filename order, matching the server's apply_seed()
	// (main.cpp). directory_iterator yields entries in an unspecified order
	// that differs across filesystems (sorted on NTFS, inode-order on ext4),
	// and 0002_dev_users.sql seeds player_game_access whose rows FK-reference
	// the games created by 0001_*; out of order, INSERT OR IGNORE silently
	// drops them.
	std::vector<std::filesystem::path> seeds;
	for (const auto &entry : std::filesystem::directory_iterator(seed_dir)) {
		if (entry.path().extension() == ".sql") seeds.push_back(entry.path());
	}
	std::sort(seeds.begin(), seeds.end());
	for (const auto &p : seeds) {
		db.exec_script(test_io::read_file_text(p.string()));
	}

	auto games = db.query("SELECT slug, gate_tag FROM games ORDER BY slug;");
	TEST_EXPECT(games.size() == 2);
	TEST_EXPECT(games[0].as_text(0).value() == "dfx2_consumer");
	TEST_EXPECT(games[0].as_text(1).value() == "dfx2:0:cus:buffy");
	TEST_EXPECT(games[1].as_text(0).value() == "jop_2_consumer");
	TEST_EXPECT(games[1].as_text(1).value() == "jop:cus2");

	auto players = db.query(
		"SELECT username, pcid, nwh, nwhandle FROM players ORDER BY username;"
	);
	TEST_EXPECT(players.size() == 5);
	TEST_EXPECT(players[0].as_text(0).value() == "foo");
	TEST_EXPECT(players[0].as_text(1).value() == "00000003");
	TEST_EXPECT(players[0].as_text(2).value() == "1");
	TEST_EXPECT(players[0].as_text(3).value() == "FooPlayer");
	TEST_EXPECT(players[1].as_text(0).value() == "test");
	TEST_EXPECT(players[1].as_text(1).value() == "00000002");
	TEST_EXPECT(players[1].as_text(2).value() == "1");
	TEST_EXPECT(players[1].as_text(3).value() == "TestPlayer");
	TEST_EXPECT(players[2].as_text(0).value() == "test1");
	TEST_EXPECT(players[2].as_text(3).value() == "TestPlayer1");
	TEST_EXPECT(players[3].as_text(0).value() == "test2");
	TEST_EXPECT(players[3].as_text(3).value() == "TestPlayer2");
	TEST_EXPECT(players[4].as_text(0).value() == "test3");
	TEST_EXPECT(players[4].as_text(3).value() == "TestPlayer3");

	auto access = db.query(
		"SELECT p.username, a.game_slug, a.status, a.exp_bits "
		"FROM player_game_access a JOIN players p ON p.id = a.user_id "
		"ORDER BY p.username, a.game_slug;"
	);
	// foo + test + test1/test2/test3 (the extra three are dev accounts for
	// concurrent multi-client local testing; password 'test', same access).
	TEST_EXPECT(access.size() == 5);
	TEST_EXPECT(access[0].as_text(0).value() == "foo");
	TEST_EXPECT(access[0].as_text(1).value() == "jop_2_consumer");
	TEST_EXPECT(access[0].as_text(2).value() == "active");
	TEST_EXPECT(access[0].as_text(3).value() == "3");
	TEST_EXPECT(access[1].as_text(0).value() == "test");
	TEST_EXPECT(access[1].as_text(1).value() == "jop_2_consumer");
	TEST_EXPECT(access[1].as_text(2).value() == "active");
	TEST_EXPECT(access[1].as_text(3).value() == "3");
	TEST_EXPECT(access[2].as_text(0).value() == "test1");
	TEST_EXPECT(access[2].as_text(1).value() == "jop_2_consumer");
	TEST_EXPECT(access[2].as_text(2).value() == "active");
	TEST_EXPECT(access[3].as_text(0).value() == "test2");
	TEST_EXPECT(access[4].as_text(0).value() == "test3");
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
	std::sort(seeds.begin(), seeds.end());  // deterministic, FK-safe order

	// Run the seed twice; INSERT OR IGNORE should keep counts stable.
	for (int pass = 0; pass < 2; ++pass) {
		for (const auto &p : seeds) {
			db.exec_script(test_io::read_file_text(p.string()));
		}
		if (pass == 0) {
			db.exec(
				"UPDATE players SET pcid='BADFOO', nwh='', nwhandle='', "
				"account_status='banned' WHERE username='foo';"
			);
			db.exec(
				"UPDATE player_game_access SET status='banned', exp_bits='0' "
				"WHERE user_id=(SELECT id FROM players WHERE username='foo') "
				"AND game_slug='jop_2_consumer';"
			);
		}
	}

	auto games = db.query("SELECT COUNT(*) FROM games;");
	TEST_EXPECT(games[0].as_int(0).value() == 2);
	auto foo = db.query(
		"SELECT pcid, nwh, nwhandle, account_status FROM players "
		"WHERE username='foo';"
	);
	TEST_EXPECT(foo.size() == 1);
	TEST_EXPECT(foo[0].as_text(0).value() == "00000003");
	TEST_EXPECT(foo[0].as_text(1).value() == "1");
	TEST_EXPECT(foo[0].as_text(2).value() == "FooPlayer");
	TEST_EXPECT(foo[0].as_text(3).value() == "active");
	auto access = db.query(
		"SELECT status, exp_bits FROM player_game_access "
		"WHERE user_id=(SELECT id FROM players WHERE username='foo') "
		"AND game_slug='jop_2_consumer';"
	);
	TEST_EXPECT(access.size() == 1);
	TEST_EXPECT(access[0].as_text(0).value() == "active");
	TEST_EXPECT(access[0].as_text(1).value() == "3");
	return 0;
}

int test_foreign_keys_enforced() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";

	Database db(":memory:");
	run_migrations(db, migrations);

	// Inserting a host against a non-existent game_id should fail.
	bool caught = false;
	try {
		db.exec(
			"INSERT INTO hosts (rid, gsid, game_id) "
			"VALUES (1, 'orphan', 9999);"
		);
	} catch (const opennova::db::SqliteError &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	return 0;
}

// 0008: every account starts as a 'player', the role takes only 'player' or
// 'admin', web_sessions is keyed by the token hash with its user_id index, and
// a deleted player takes its sessions.
int test_web_sessions_and_roles() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	Database db(":memory:");
	run_migrations(db, source_dir / "backend" / "migrations");

	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
	        "VALUES ('roleless', 'x', '0000aaaa', '1', 'Roleless');");
	auto role = db.query("SELECT role FROM players WHERE username = 'roleless';");
	TEST_EXPECT(role.size() == 1 && role[0].as_text(0).value() == "player");
	db.exec("UPDATE players SET role = 'admin' WHERE username = 'roleless';");
	bool caught = false;
	try {
		db.exec("UPDATE players SET role = 'root' WHERE username = 'roleless';");
	} catch (const opennova::db::SqliteError &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	role = db.query("SELECT role FROM players WHERE username = 'roleless';");
	TEST_EXPECT(role[0].as_text(0).value() == "admin");

	auto index = db.query(
		"SELECT name FROM sqlite_master WHERE type = 'index' AND tbl_name = 'web_sessions' "
		"AND name = 'idx_web_sessions_user_id';");
	TEST_EXPECT(index.size() == 1);

	db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
	        "SELECT 'h1', id, datetime('now', '+30 days') FROM players WHERE username = 'roleless';");
	// The token hash is the key: a second row under it is refused.
	caught = false;
	try {
		db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
		        "SELECT 'h1', id, datetime('now') FROM players WHERE username = 'roleless';");
	} catch (const opennova::db::SqliteError &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	// A session for no player is refused.
	caught = false;
	try {
		db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
		        "VALUES ('h2', 9999, datetime('now'));");
	} catch (const opennova::db::SqliteError &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	auto fresh = db.query("SELECT created_at IS NOT NULL, last_seen_at IS NOT NULL, ip, user_agent "
	                      "FROM web_sessions WHERE token_hash = 'h1';");
	TEST_EXPECT(fresh.size() == 1);
	TEST_EXPECT(fresh[0].as_int(0).value() == 1 && fresh[0].as_int(1).value() == 1);
	TEST_EXPECT(fresh[0].as_text(2).value().empty() && fresh[0].as_text(3).value().empty());

	db.exec("DELETE FROM players WHERE username = 'roleless';");
	TEST_EXPECT(db.query("SELECT 1 FROM web_sessions;").empty());
	return 0;
}

// 0008 over a database that already holds accounts (every deployment before
// it): the migrations up to 0007 and the seed, then the full set, which
// applies 0008 alone and gives every existing account the 'player' role.
int test_web_sessions_migration_over_existing_accounts() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";
	test_temp::TempDir before_0008("backend_schema_pre0008");
	for (const auto &e : std::filesystem::directory_iterator(migrations)) {
		if (e.path().extension() == ".sql" && e.path().filename().string() < "0008") {
			std::filesystem::copy_file(e.path(), before_0008.path / e.path().filename());
		}
	}

	Database db(":memory:");
	const auto first = run_migrations(db, before_0008.path);
	TEST_EXPECT(first.applied.size() == 7);
	std::vector<std::filesystem::path> seeds;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() == ".sql") seeds.push_back(e.path());
	}
	std::sort(seeds.begin(), seeds.end());
	for (const auto &p : seeds) db.exec_script(test_io::read_file_text(p.string()));
	TEST_EXPECT(db.query("SELECT name FROM pragma_table_info('players') WHERE name = 'role';").empty());

	const auto second = run_migrations(db, migrations);
	TEST_EXPECT(second.applied.size() == 1);
	TEST_EXPECT(second.applied[0] == "0008_web_sessions_and_roles.sql");
	const auto roles = db.query("SELECT COUNT(*), SUM(role = 'player') FROM players;");
	TEST_EXPECT(roles[0].as_int(0).value() == 5);
	TEST_EXPECT(roles[0].as_int(1).value() == 5);
	TEST_EXPECT(db.query("SELECT 1 FROM web_sessions;").empty());
	return 0;
}

} // namespace

int main() {
	if (test_migrations_create_expected_tables() != 0) return 1;
	if (test_seed_populates_games() != 0) return 1;
	if (test_seed_is_idempotent() != 0) return 1;
	if (test_foreign_keys_enforced() != 0) return 1;
	if (test_web_sessions_and_roles() != 0) return 1;
	if (test_web_sessions_migration_over_existing_accounts() != 0) return 1;
	std::printf("OK: backend schema + seed (migrations, seed idempotency, FKs, "
	            "web sessions + roles)\n");
	return 0;
}
