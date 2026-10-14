// Apply backend/migrations/* + backend/seed/* against a fresh in-process
// SQLite DB and verify the schema + canonical seed data are intact. Catches
// SQL syntax errors and seed regressions at CI time before the standalone
// server boots them in production.

#include <net/novaworld/db/sqlite.h>

#include "../common/file_io.h"
#include "../common/temp_dir.h"
#include "../common/test_expect.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

using opennova::db::Database;
using opennova::db::run_migrations;

namespace {

// The games rows as the seed wrote them before migration 0013 (no title, JO's
// pfid NULL): what every deployment's database held then. A test that builds
// an older schema writes these, since today's seed names columns it lacks.
constexpr const char *kGamesSeedBefore0013 =
		"INSERT OR IGNORE INTO games (slug, display_name, lobby_name, gate_tag, start_page, "
		"host_page, gsb_path, join_lan_url, ver1, ver2, exp_bits, pfid) VALUES "
		"('jop_2_consumer', 'Joint Operations', 'jop_2_consumer', 'jop:cus2', 'jop_2_start.htm', "
		"'jop_2_host1.htm', 'jop_2.gsb?a=1', NULL, '3', '2345', '3', NULL), "
		"('dfx2_consumer', 'Delta Force Xtreme 2', 'dfx2_consumer', 'dfx2:0:cus:buffy', "
		"'dfx2_0_start.htm', 'dfx2_0_host1.htm', 'dfx2_0.gsb?a=1', 'NWJoin.dll?pfid=38', '1', "
		"'9013', "
		"'1', '38');";

// The repo's migrations whose file names sort before `before` (e.g. "0008"),
// copied into `dir`.
void copy_migrations_before(const std::filesystem::path &migrations, const std::string &before,
		const std::filesystem::path &dir) {
	for (const auto &e : std::filesystem::directory_iterator(migrations)) {
		if (e.path().extension() == ".sql" && e.path().filename().string() < before) {
			std::filesystem::copy_file(e.path(), dir / e.path().filename());
		}
	}
}

// Every seed file in file-name order, as the server's apply_seed() runs them.
void apply_seeds(Database &db, const std::filesystem::path &seed_dir) {
	std::vector<std::filesystem::path> seeds;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() == ".sql")
			seeds.push_back(e.path());
	}
	std::sort(seeds.begin(), seeds.end());
	for (const auto &p : seeds)
		db.exec_script(test_io::read_file_text(p.string()));
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
		"active_hosts", "active_user_sessions", "cd_key_activations", "cd_keys",
		"games", "host_keys", "host_players", "host_roster", "hosted_server_console", "hosted_server_events",
		"hosted_servers", "hosts", "login_addresses",
		"player_game_access",
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

	auto games =
			db.query("SELECT slug, gate_tag, title, lobby_name, pfid FROM games ORDER BY slug;");
	TEST_EXPECT(games.size() == 3);
	TEST_EXPECT(games[0].as_text(0).value() == "dfx2_consumer");
	TEST_EXPECT(games[0].as_text(1).value() == "dfx2:0:cus:buffy");
	TEST_EXPECT(games[0].as_text(2).value() == "dfx2");
	TEST_EXPECT(games[0].as_text(3).value() == "dfx2_0_consumer");
	TEST_EXPECT(games[0].as_text(4).value() == "38");
	TEST_EXPECT(games[1].as_text(0).value() == "jop_2_consumer");
	TEST_EXPECT(games[1].as_text(1).value() == "jop:cus2");
	TEST_EXPECT(games[1].as_text(2).value() == "jo");
	TEST_EXPECT(games[1].as_text(3).value() == "jop_2_consumer");
	TEST_EXPECT(games[1].as_text(4).value() == "28");
	TEST_EXPECT(games[2].as_text(0).value() == "opennova_0_consumer");
	TEST_EXPECT(games[2].as_text(1).value() == "opennova:0");
	TEST_EXPECT(games[2].as_text(2).value() == "jo");
	TEST_EXPECT(games[2].as_text(3).value() == "opennova_0_consumer");
	TEST_EXPECT(games[2].as_text(4).value() == "28");

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
	TEST_EXPECT(games[0].as_int(0).value() == 3);
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
// it): the migrations up to 0007 and the seed of the time (the games rows
// before 0013, then the dev accounts), then the full set, which applies 0008
// (and 0009 on after it) and gives every existing account the 'player' role,
// and (0011) the 'player' kind with no hosting approval.
int test_web_sessions_migration_over_existing_accounts() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";
	test_temp::TempDir before_0008("backend_schema_pre0008");
	copy_migrations_before(migrations, "0008", before_0008.path);

	Database db(":memory:");
	const auto first = run_migrations(db, before_0008.path);
	TEST_EXPECT(first.applied.size() == 7);
	db.exec_script(kGamesSeedBefore0013);
	db.exec_script(test_io::read_file_text((seed_dir / "0002_dev_users.sql").string()));
	TEST_EXPECT(db.query("SELECT name FROM pragma_table_info('players') WHERE name = 'role';").empty());

	const auto second = run_migrations(db, migrations);
	TEST_EXPECT(second.applied.size() == 8);
	TEST_EXPECT(second.applied[0] == "0008_web_sessions_and_roles.sql");
	TEST_EXPECT(second.applied[1] == "0009_host_keys.sql");
	TEST_EXPECT(second.applied[2] == "0010_credentials_generation.sql");
	TEST_EXPECT(second.applied[3] == "0011_hosted_servers.sql");
	TEST_EXPECT(second.applied[4] == "0012_hosted_server_console.sql");
	TEST_EXPECT(second.applied[5] == "0013_games_title.sql");
	TEST_EXPECT(second.applied[6] == "0014_hosted_server_game.sql");
	TEST_EXPECT(second.applied[7] == "0015_cd_keys.sql");
	const auto roles = db.query("SELECT COUNT(*), SUM(role = 'player'), SUM(kind = 'player'), "
	                            "SUM(can_host) FROM players;");
	TEST_EXPECT(roles[0].as_int(0).value() == 5);
	TEST_EXPECT(roles[0].as_int(1).value() == 5);
	TEST_EXPECT(roles[0].as_int(2).value() == 5);
	TEST_EXPECT(roles[0].as_int(3).value() == 0);
	TEST_EXPECT(db.query("SELECT 1 FROM web_sessions;").empty());
	return 0;
}

// 0014 over a database whose hosted servers ran before it (every one on Joint
// Operations' data): the migrations up to 0012 with a hosted server written,
// then the full set, which gives that row the game 'jop_2_consumer'; a new row
// names its own.
int test_hosted_server_game_migration() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	const auto migrations = source_dir / "backend" / "migrations";
	test_temp::TempDir before_0013("backend_schema_pre0013");
	for (const auto &e : std::filesystem::directory_iterator(migrations)) {
		if (e.path().extension() == ".sql" && e.path().filename().string() < "0013") {
			std::filesystem::copy_file(e.path(), before_0013.path / e.path().filename());
		}
	}
	Database db(":memory:");
	const auto first = run_migrations(db, before_0013.path);
	TEST_EXPECT(first.applied.size() == 12);
	TEST_EXPECT(db.query("SELECT name FROM pragma_table_info('hosted_servers') WHERE name = 'game';").empty());
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
	        "VALUES ('host_before', 'x', '0e140001', '1', 'HostBefore');");
	db.exec("INSERT INTO hosted_servers (owner_user_id, state, game_name, max_players, time_limit, kill_limit, "
	        "max_score, respawn, start_delay, missions) "
	        "SELECT id, 'stopped', 'Before', 8, 30, 50, 5, 5, 0, 'a.bms' FROM players WHERE username = 'host_before';");

	const auto second = run_migrations(db, migrations);
	TEST_EXPECT(second.applied.size() >= 2 && second.applied[1] == "0014_hosted_server_game.sql");
	const auto rows = db.query("SELECT game FROM hosted_servers WHERE game_name = 'Before';");
	TEST_EXPECT(rows.size() == 1 && rows[0].as_text(0).value() == "jop_2_consumer");
	db.exec("INSERT INTO hosted_servers (owner_user_id, state, game, game_name, max_players, time_limit, "
	        "kill_limit, max_score, respawn, start_delay, missions) "
	        "SELECT id, 'stopped', 'dfx2_consumer', 'After', 8, 30, 50, 5, 5, 0, 'a.bms' FROM players "
	        "WHERE username = 'host_before';");
	const auto after = db.query("SELECT game FROM hosted_servers WHERE game_name = 'After';");
	TEST_EXPECT(after.size() == 1 && after[0].as_text(0).value() == "dfx2_consumer");
	// The column takes no NULL.
	bool refused = false;
	try {
		db.exec("UPDATE hosted_servers SET game = NULL WHERE game_name = 'After';");
	} catch (const opennova::db::SqliteError &) {
		refused = true;
	}
	TEST_EXPECT(refused);
	return 0;
}

// The OpenNova row, every column as the hosted-titles program decided it: its
// own tag, LobbyName and .gsb; JO's wire identity (title, version pair, pfid)
// and JO's pages; no expansion (the base bit alone).
int expect_opennova_row(Database &db) {
	const auto on = db.query(
			"SELECT display_name, lobby_name, gate_tag, title, start_page, host_page, gsb_path, "
			"join_lan_url IS NULL, ver1, ver2, exp_bits, pfid FROM games "
			"WHERE slug = 'opennova_0_consumer';");
	TEST_EXPECT(on.size() == 1);
	TEST_EXPECT(on[0].as_text(0).value() == "OpenNova");
	TEST_EXPECT(on[0].as_text(1).value() == "opennova_0_consumer");
	TEST_EXPECT(on[0].as_text(2).value() == "opennova:0");
	TEST_EXPECT(on[0].as_text(3).value() == "jo");
	TEST_EXPECT(on[0].as_text(4).value() == "jop_2_start.htm");
	TEST_EXPECT(on[0].as_text(5).value() == "jop_2_host1.htm");
	TEST_EXPECT(on[0].as_text(6).value() == "opennova_0.gsb?a=1");
	TEST_EXPECT(on[0].as_int(7).value() == 1);
	TEST_EXPECT(on[0].as_text(8).value() == "3");
	TEST_EXPECT(on[0].as_text(9).value() == "2345");
	TEST_EXPECT(on[0].as_text(10).value() == "1");
	TEST_EXPECT(on[0].as_text(11).value() == "28");
	return 0;
}

// Each row's gate answer: JO's LobbyName and startupurl query the ones this
// gate always answered, with no GLSVSS; DFX2's LobbyName, query (pfid=38 in
// it) and GLSVSS leg as NovaLogic's live gate answered the DFX2 tag
// (dfx2-vs-jo-re.md section 6.5, DFX2-6); OpenNova's JO's shape. Every
// query names its row's start page last.
int expect_gate_rows(Database &db) {
	const auto rows = db.query("SELECT slug, lobby_name, startup_query, glsvss_request, "
							   "glsvss_rims, glsvss_agrms, start_page FROM games ORDER BY slug;");
	TEST_EXPECT(rows.size() == 3);
	const std::string jo_query = "ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm";
	TEST_EXPECT(rows[0].as_text(0).value() == "dfx2_consumer");
	TEST_EXPECT(rows[0].as_text(1).value() == "dfx2_0_consumer");
	TEST_EXPECT(rows[0].as_text(2).value() ==
			"ver1=[VER1]&ver2=[VER2]&cc=[CC]&pfid=38&gt=[GT]&url=dfx2_0_start.htm");
	TEST_EXPECT(rows[0].as_text(3).value() == "[DFX2]KILLS");
	TEST_EXPECT(rows[0].as_int(4).value() == 3600000 && rows[0].as_int(5).value() == 45000);
	TEST_EXPECT(rows[1].as_text(1).value() == "jop_2_consumer");
	TEST_EXPECT(rows[1].as_text(2).value() == jo_query);
	TEST_EXPECT(rows[1].as_text(3).value().empty());
	TEST_EXPECT(rows[1].as_int(4).value() == 0 && rows[1].as_int(5).value() == 0);
	TEST_EXPECT(rows[2].as_text(1).value() == "opennova_0_consumer");
	TEST_EXPECT(rows[2].as_text(2).value() == jo_query);
	TEST_EXPECT(rows[2].as_text(3).value().empty());
	for (const auto &row : rows) {
		const std::string query = row.as_text(2).value();
		const std::string tail = "&url=" + row.as_text(6).value();
		TEST_EXPECT(query.size() > tail.size() &&
				query.compare(query.size() - tail.size(), tail.size(), tail) == 0);
	}
	return 0;
}

// 0013 on a fresh database: the column, and the seed after it (the boot's
// order) writes every row with its title, the OpenNova row among them.
int test_games_title_fresh() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	Database db(":memory:");
	run_migrations(db, source_dir / "backend" / "migrations");
	TEST_EXPECT(
			db.query("SELECT name FROM pragma_table_info('games') WHERE name = 'title';").size() ==
			1);
	apply_seeds(db, source_dir / "backend" / "seed");
	const auto titles = db.query("SELECT slug, title FROM games ORDER BY slug;");
	TEST_EXPECT(titles.size() == 3);
	TEST_EXPECT(titles[0].as_text(1).value() == "dfx2");
	TEST_EXPECT(titles[1].as_text(1).value() == "jo");
	TEST_EXPECT(titles[2].as_text(1).value() == "jo");
	if (expect_gate_rows(db) != 0)
		return 1;
	return expect_opennova_row(db);
}

// 0013 over a database at 0012 (every deployment before it): the migrations
// up to 0012 and the seed of the time, then the boot's order, the full set
// (0013 first among what it applies) and today's seed. The rows already there
// keep their ids and take their titles (JO's its pfid 28) and their gate
// answers (DFX2's LobbyName becomes dfx2_0_consumer), the accounts keep their
// game access, and the seed adds the OpenNova row.
int test_games_title_over_0012() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";
	test_temp::TempDir before_0013("backend_schema_pre0013");
	copy_migrations_before(migrations, "0013", before_0013.path);

	Database db(":memory:");
	TEST_EXPECT(run_migrations(db, before_0013.path).applied.size() == 12);
	db.exec_script(kGamesSeedBefore0013);
	db.exec_script(test_io::read_file_text((seed_dir / "0002_dev_users.sql").string()));
	TEST_EXPECT(
			db.query("SELECT name FROM pragma_table_info('games') WHERE name = 'title';").empty());
	const auto ids = db.query("SELECT id FROM games ORDER BY slug;");
	TEST_EXPECT(ids.size() == 2);

	const auto second = run_migrations(db, migrations);
	TEST_EXPECT(!second.applied.empty() && second.applied[0] == "0013_games_title.sql");
	apply_seeds(db, seed_dir);

	const auto games = db.query("SELECT slug, id, title, pfid FROM games ORDER BY slug;");
	TEST_EXPECT(games.size() == 3);
	TEST_EXPECT(games[0].as_text(0).value() == "dfx2_consumer");
	TEST_EXPECT(games[0].as_int(1).value() == ids[0].as_int(0).value());
	TEST_EXPECT(games[0].as_text(2).value() == "dfx2");
	TEST_EXPECT(games[0].as_text(3).value() == "38");
	TEST_EXPECT(games[1].as_text(0).value() == "jop_2_consumer");
	TEST_EXPECT(games[1].as_int(1).value() == ids[1].as_int(0).value());
	TEST_EXPECT(games[1].as_text(2).value() == "jo");
	TEST_EXPECT(games[1].as_text(3).value() == "28");
	TEST_EXPECT(games[2].as_text(0).value() == "opennova_0_consumer");
	TEST_EXPECT(db.query("SELECT 1 FROM player_game_access WHERE game_slug = 'jop_2_consumer';")
						.size() == 5);
	// The older rows' gate answers (DFX2's LobbyName among them) are 0013's.
	if (expect_gate_rows(db) != 0)
		return 1;
	return expect_opennova_row(db);
}

// 0010: a new account starts at credentials generation 0, a retail login's
// session row carries one, and neither column takes NULL. And 0003's
// account_status: 'active' by default, never empty (inserted or updated).
int test_credentials_generation() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	Database db(":memory:");
	run_migrations(db, source_dir / "backend" / "migrations");

	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
			"VALUES ('gen', 'x', '0000bbbb', '1', 'Gen');");
	TEST_EXPECT(db.query("SELECT credentials_gen FROM players WHERE username = 'gen';")
						.front()
						.as_int(0)
						.value() == 0);
	db.exec("INSERT INTO active_user_sessions (user_id, username, session_tag, credentials_gen) "
			"SELECT id, username, 'tag-gen', credentials_gen FROM players WHERE username = 'gen';");
	TEST_EXPECT(db.query("SELECT 1 FROM active_user_sessions WHERE session_tag = 'tag-gen' "
						 "AND credentials_gen = 0;")
						.size() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM players WHERE username = 'gen' "
						 "AND account_status = 'active';")
						.size() == 1);
	for (const char *sql : { "UPDATE players SET credentials_gen = NULL WHERE username = 'gen';",
				 "UPDATE active_user_sessions SET credentials_gen = NULL;",
				 "UPDATE players SET account_status = '' WHERE username = 'gen';",
				 "INSERT INTO players (username, password_hash, pcid, nwh, nwhandle, "
				 "account_status) VALUES ('blank', 'x', '0000cccc', '1', 'Blank', '');" }) {
		bool caught = false;
		try {
			db.exec(sql);
		} catch (const opennova::db::SqliteError &) {
			caught = true;
		}
		TEST_EXPECT(caught);
	}
	return 0;
}

// The fresh schema's account deletes and host_keys (0001, 0002, 0009, edited
// in place: the service database is reset on deploy). players.id is
// AUTOINCREMENT, so a deleted account's id is never handed out again; a
// deleted account leaves its listed server listed and its player-list rows
// in place with no account (ON DELETE SET NULL) and takes its hosting keys
// (ON DELETE CASCADE). host_keys is keyed by the key hash, with its indexes; a
// key needs a player, and a fresh key row carries both times and no claim.
int test_host_keys_and_account_deletes() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	Database db(":memory:");
	run_migrations(db, source_dir / "backend" / "migrations");
	auto add_player = [&db](const std::string &name) {
		db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
		        "VALUES (?, 'x', ?, '1', ?);",
		        {opennova::db::BindValue(name), opennova::db::BindValue("pc-" + name),
		         opennova::db::BindValue(name)});
		return db.query("SELECT id FROM players WHERE username = ?;", {opennova::db::BindValue(name)})
		        .front()
		        .as_int(0)
		        .value();
	};

	// The newest account deleted, its id stays retired.
	const int64_t first = add_player("first");
	const int64_t newest = add_player("newest");
	TEST_EXPECT(newest > first);
	db.exec("DELETE FROM players WHERE id = ?;", {opennova::db::BindValue(newest)});
	TEST_EXPECT(add_player("newcomer") > newest);

	for (const char *index : {"idx_host_keys_user_id", "idx_host_keys_last_used_at"}) {
		TEST_EXPECT(db.query("SELECT 1 FROM sqlite_master WHERE type = 'index' "
		                     "AND tbl_name = 'host_keys' AND name = ?;",
		                     {opennova::db::BindValue(std::string(index))})
		                    .size() == 1);
	}
	const int64_t host = add_player("host");
	db.exec("INSERT INTO host_keys (key_hash, user_id) VALUES ('h1', ?);",
	        {opennova::db::BindValue(host)});
	auto fresh = db.query("SELECT created_at IS NOT NULL, last_used_at IS NOT NULL, claimed_rid "
	                      "FROM host_keys WHERE key_hash = 'h1';");
	TEST_EXPECT(fresh.size() == 1);
	TEST_EXPECT(fresh[0].as_int(0).value() == 1 && fresh[0].as_int(1).value() == 1);
	TEST_EXPECT(!fresh[0].as_int(2).has_value());
	// The hash is the key: a second row under it is refused.
	bool caught = false;
	try {
		db.exec("INSERT INTO host_keys (key_hash, user_id) VALUES ('h1', ?);",
		        {opennova::db::BindValue(first)});
	} catch (const opennova::db::SqliteError &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	// A key for no player is refused.
	caught = false;
	try {
		db.exec("INSERT INTO host_keys (key_hash, user_id) VALUES ('h2', 9999);");
	} catch (const opennova::db::SqliteError &) {
		caught = true;
	}
	TEST_EXPECT(caught);

	// The host's listed server, with the first account in its player list.
	db.exec("INSERT INTO active_hosts (rid, gsid, game, app_id, server_name, host_ip, host_port, "
	        "host_user_id, peer_ip, peer_port) VALUES (7, 'G7', 'jop_2_consumer', '1234', 'Owned', "
	        "'10.0.0.7', 32768, ?, '10.0.0.7', 32768);",
	        {opennova::db::BindValue(host)});
	db.exec("INSERT INTO host_players (host_rid, user_id, nwhandle, peer_ip, peer_port) "
	        "VALUES (7, ?, 'first', '10.0.0.8', 0);",
	        {opennova::db::BindValue(first)});
	db.exec("DELETE FROM players WHERE id IN (?, ?);",
	        {opennova::db::BindValue(host), opennova::db::BindValue(first)});
	const auto listed = db.query("SELECT server_name, host_user_id FROM active_hosts WHERE rid = 7;");
	TEST_EXPECT(listed.size() == 1 && listed[0].as_text(0).value() == "Owned");
	TEST_EXPECT(!listed[0].as_int(1).has_value());
	const auto joined = db.query("SELECT nwhandle, user_id FROM host_players WHERE host_rid = 7;");
	TEST_EXPECT(joined.size() == 1 && joined[0].as_text(0).value() == "first");
	TEST_EXPECT(!joined[0].as_int(1).has_value());
	TEST_EXPECT(db.query("SELECT 1 FROM host_keys;").empty());
	return 0;
}

// A statement the schema must refuse.
bool refused(Database &db, const std::string &sql) {
	try {
		db.exec(sql);
	} catch (const opennova::db::SqliteError &) {
		return true;
	}
	return false;
}

// 0015's rules over `db` (migrated): a serial and a wire key are each unique,
// a key's account must exist, an account holds one live key per title (any
// number of revoked ones), a machine is one activation per key, and a
// deleted account takes its keys and their activations, a deleted key its
// activations. `prefix` keeps the rows of one call apart from another's.
int expect_cd_key_rules(Database &db, const std::string &prefix) {
	using opennova::db::BindValue;
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) VALUES (?, 'x', ?, "
			"'1', ?);",
			{ BindValue(prefix + "owner"), BindValue(prefix + "pc"), BindValue(prefix + "owner") });
	const int64_t owner =
			db.query("SELECT id FROM players WHERE username = ?;", { BindValue(prefix + "owner") })
					.front()
					.as_int(0)
					.value();
	const auto add_key = [&](const std::string &serial, std::optional<int64_t> user, bool revoked) {
		db.exec("INSERT INTO cd_keys (title, serial, wire_key, user_id, created_by, revoked_at) "
				"VALUES ('dfx2', ?, ?, ?, 'token', ?);",
				{ BindValue(prefix + serial), BindValue(prefix + serial + "-WIRE"),
						user ? BindValue(*user) : BindValue(std::monostate{}),
						revoked ? BindValue(std::string("2026-10-14 00:00:00"))
								: BindValue(std::monostate{}) });
		return db.last_insert_rowid();
	};
	const int64_t live = add_key("LIVE", owner, false);
	const auto fresh = db.query(
			"SELECT created_at IS NOT NULL, assigned_at, revoked_at FROM cd_keys WHERE id = ?;",
			{ BindValue(live) });
	TEST_EXPECT(fresh.size() == 1 && fresh[0].as_int(0).value() == 1);
	TEST_EXPECT(fresh[0].is_null(1) && fresh[0].is_null(2));
	add_key("OLD1", owner, true);
	add_key("OLD2", owner, true);
	add_key("FREE", std::nullopt, false);
	// A second live key for the account and title, a duplicate serial or wire key, a key
	// for no account: refused.
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_keys (title, serial, wire_key, user_id, created_by) VALUES "
			"('dfx2', '" +
					prefix + "TWO', '" + prefix + "TWO-WIRE', " + std::to_string(owner) +
					", 'token');"));
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_keys (title, serial, wire_key, created_by) VALUES ('dfx2', '" + prefix +
					"LIVE', 'OTHER-WIRE-" + prefix + "', 'token');"));
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_keys (title, serial, wire_key, created_by) VALUES ('dfx2', 'OTHER-" +
					prefix + "', '" + prefix + "LIVE-WIRE', 'token');"));
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_keys (title, serial, wire_key, user_id, created_by) VALUES "
			"('dfx2', 'GHOST-" +
					prefix + "', 'GHOST-WIRE-" + prefix + "', 999999, 'token');"));
	// Another title's live key is the account's own business.
	db.exec("INSERT INTO cd_keys (title, serial, wire_key, user_id, created_by) VALUES ('other', "
			"?, ?, ?, 'token');",
			{ BindValue(prefix + "OTHER"), BindValue(prefix + "OTHER-WIRE"), BindValue(owner) });

	db.exec("INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES (?, "
			"'ABCDEFGHIJKLMNOPQRSTUVW', 'X');",
			{ BindValue(live) });
	const auto activation =
			db.query("SELECT created_at IS NOT NULL FROM cd_key_activations WHERE key_id = ?;",
					{ BindValue(live) });
	TEST_EXPECT(activation.size() == 1 && activation[0].as_int(0).value() == 1);
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES (" +
					std::to_string(live) + ", 'ABCDEFGHIJKLMNOPQRSTUVW', 'Y');"));
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES "
			"(999999, 'ABCDEFGHIJKLMNOPQRSTUVW', 'Y');"));
	db.exec("INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES (?, "
			"'TTTTTTTTTTTTTTTTTTTTTTT', 'Z');",
			{ BindValue(live) });

	// A deleted key takes its activations; a deleted account its keys.
	const int64_t free_key =
			db.query("SELECT id FROM cd_keys WHERE serial = ?;", { BindValue(prefix + "FREE") })
					.front()
					.as_int(0)
					.value();
	db.exec("INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES (?, "
			"'ABCDEFGHIJKLMNOPQRSTUVW', 'F');",
			{ BindValue(free_key) });
	db.exec("DELETE FROM cd_keys WHERE id = ?;", { BindValue(free_key) });
	TEST_EXPECT(
			db.query("SELECT 1 FROM cd_key_activations WHERE key_id = ?;", { BindValue(free_key) })
					.empty());
	db.exec("DELETE FROM players WHERE id = ?;", { BindValue(owner) });
	TEST_EXPECT(db.query("SELECT 1 FROM cd_keys WHERE serial LIKE ? || '%';", { BindValue(prefix) })
					.empty());
	TEST_EXPECT(db.query("SELECT 1 FROM cd_key_activations WHERE key_id = ?;", { BindValue(live) })
					.empty());
	return 0;
}

// 0015 on a fresh database: the two tables, their indexes and their rules.
int test_cd_keys_fresh() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	Database db(":memory:");
	run_migrations(db, source_dir / "backend" / "migrations");
	for (const char *index :
			{ "idx_cd_keys_title", "idx_cd_keys_user", "idx_cd_keys_live_per_account" }) {
		TEST_EXPECT(db.query("SELECT 1 FROM sqlite_master WHERE type = 'index' AND tbl_name = "
							 "'cd_keys' AND name = ?;",
							  { opennova::db::BindValue(std::string(index)) })
							.size() == 1);
	}
	return expect_cd_key_rules(db, "fresh-");
}

// 0015 over a database at 0014 (every deployment before it), holding accounts:
// the full set applies 0015 alone, the accounts stay as they were, and the new
// tables start empty and take keys for those accounts.
int test_cd_keys_over_0014() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";
	test_temp::TempDir before_0015("backend_schema_pre0015");
	copy_migrations_before(migrations, "0015", before_0015.path);

	Database db(":memory:");
	TEST_EXPECT(run_migrations(db, before_0015.path).applied.size() == 14);
	apply_seeds(db, seed_dir);
	TEST_EXPECT(db.query("SELECT name FROM sqlite_master WHERE name = 'cd_keys';").empty());
	const auto accounts = db.query("SELECT COUNT(*) FROM players;").front().as_int(0).value();
	TEST_EXPECT(accounts == 5);

	const auto second = run_migrations(db, migrations);
	TEST_EXPECT(second.applied.size() == 1 && second.applied[0] == "0015_cd_keys.sql");
	TEST_EXPECT(db.query("SELECT COUNT(*) FROM players;").front().as_int(0).value() == accounts);
	TEST_EXPECT(db.query("SELECT 1 FROM cd_keys;").empty());
	TEST_EXPECT(db.query("SELECT 1 FROM cd_key_activations;").empty());
	db.exec("INSERT INTO cd_keys (title, serial, wire_key, user_id, created_by) "
			"SELECT 'dfx2', 'PRE0015-' || username, 'PRE0015-WIRE-' || username, id, 'token' FROM "
			"players;");
	TEST_EXPECT(db.query("SELECT COUNT(*) FROM cd_keys;").front().as_int(0).value() == accounts);
	return expect_cd_key_rules(db, "over-");
}

} // namespace

int main() {
	if (test_migrations_create_expected_tables() != 0) return 1;
	if (test_seed_populates_games() != 0) return 1;
	if (test_seed_is_idempotent() != 0) return 1;
	if (test_foreign_keys_enforced() != 0) return 1;
	if (test_web_sessions_and_roles() != 0) return 1;
	if (test_web_sessions_migration_over_existing_accounts() != 0) return 1;
	if (test_games_title_fresh() != 0)
		return 1;
	if (test_games_title_over_0012() != 0)
		return 1;
	if (test_host_keys_and_account_deletes() != 0) return 1;
	if (test_credentials_generation() != 0) return 1;
	if (test_hosted_server_game_migration() != 0) return 1;
	if (test_cd_keys_fresh() != 0)
		return 1;
	if (test_cd_keys_over_0014() != 0)
		return 1;
	std::printf("OK: backend schema + seed (migrations, seed idempotency, FKs, "
				"web sessions + roles, games titles, host keys, credentials generation, hosted "
				"server games, "
				"cd keys)\n");
	return 0;
}
