// Apply backend/migrations/* + backend/seed/* to a fresh database of the
// test's own (common/test_database.h, one per case) and verify the schema and
// the canonical seed data are intact. Catches SQL errors and seed regressions
// at CI time before the standalone server boots them in production.

#include <net/novaworld/db/database.h>

#include "../common/file_io.h"
#include "../common/test_database.h"
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

using opennova::db::BindValue;
using opennova::db::Database;
using opennova::db::run_migrations;
using test_db::TestDatabase;

namespace {

// Whether the public schema holds index `name` on `table`.
bool has_index(Database &db, const char *table, const char *name) {
	return db.query("SELECT 1 FROM pg_indexes WHERE schemaname = 'public' AND tablename = $1 "
					"AND indexname = $2;",
				   { BindValue(std::string(table)), BindValue(std::string(name)) })
				   .size() == 1;
}

// Whether `table` has column `column`.
bool has_column(Database &db, const char *table, const char *column) {
	return db.query("SELECT 1 FROM information_schema.columns WHERE table_schema = 'public' "
					"AND table_name = $1 AND column_name = $2;",
				   { BindValue(std::string(table)), BindValue(std::string(column)) })
				   .size() == 1;
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

	TestDatabase scratch({ /*migrate=*/false });
	Database db = scratch.open();
	auto r = run_migrations(db, migrations);
	// Every file in the directory applies, in name order, and is recorded.
	std::vector<std::string> listing;
	for (const auto &e : std::filesystem::directory_iterator(migrations)) {
		if (e.path().extension() == ".sql") listing.push_back(e.path().filename().string());
	}
	std::sort(listing.begin(), listing.end());
	TEST_EXPECT(!listing.empty() && r.applied == listing);
	const auto recorded = db.query("SELECT filename FROM _schema_migrations ORDER BY filename;");
	TEST_EXPECT(recorded.size() == listing.size());
	for (size_t i = 0; i < listing.size(); ++i) TEST_EXPECT(recorded[i].as_text(0) == listing[i]);

	auto rows = db.query(
		"SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' "
		"AND table_type = 'BASE TABLE' AND table_name <> '_schema_migrations' ORDER BY table_name;");
	std::printf("  applied %zu migration(s); created %zu table(s)\n",
	            r.applied.size(), rows.size());

	// Expected tables from the backend migration set.
	const std::vector<std::string> expected = { "active_hosts", "active_user_sessions",
		"billing_customers", "billing_events", "billing_subscriptions", "billing_sync_backoff",
		"billing_sync_state", "cd_key_activations", "cd_keys", "controller_leader",
		"data_set_trees", "data_sets", "fleet_launch_backoff", "fleet_machine_egress",
		"fleet_machines", "fleet_slot_reports",
		"games", "host_keys", "host_players", "host_roster", "hosted_admin_links",
		"hosted_console_commands", "hosted_console_replies", "hosted_limits_hold",
		"hosted_server_console", "hosted_server_events", "hosted_server_hourly",
		"hosted_server_samples", "hosted_servers", "jobs", "lobby_commands", "lobby_instances",
		"lobby_sessions", "login_addresses", "password_resets", "player_game_access", "players",
		"release_sections", "releases", "server_status", "service_meta", "unknown_messages",
		"web_sessions" };
	TEST_EXPECT(rows.size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		TEST_EXPECT(rows[i].as_text(0).value() == expected[i]);
	}
	// The dialect's rules: every integer is bigint (no integer or smallint
	// column), every time a timestamptz, the flags booleans, the sample bytea.
	TEST_EXPECT(db.query("SELECT table_name, column_name FROM information_schema.columns "
						 "WHERE table_schema = 'public' AND data_type IN ('integer', 'smallint');")
						.empty());
	TEST_EXPECT(db.query("SELECT table_name, column_name FROM information_schema.columns "
						 "WHERE table_schema = 'public' AND table_name <> '_schema_migrations' "
						 "AND (column_name LIKE '%\\_at' OR column_name IN ('last_login', 'at')) "
						 "AND data_type <> 'timestamp with time zone';")
						.empty());
	const auto flags = db.query("SELECT table_name || '.' || column_name FROM information_schema.columns "
								"WHERE table_schema = 'public' AND data_type = 'boolean' ORDER BY 1;");
	const std::vector<std::string> expected_flags = {
		"billing_customers.livemode", "billing_events.livemode",
		"billing_subscriptions.cancel_at_period_end", "billing_subscriptions.livemode",
		"billing_sync_state.prices_checked", "billing_sync_state.prices_ok",
		"data_sets.retail_updater", "fleet_machines.launch_refused", "fleet_machines.was_draining",
		"fleet_slot_reports.oom",
		"hosted_console_commands.record", "hosted_console_commands.sent",
		"hosted_limits_hold.force_next", "hosted_limits_hold.held", "hosted_server_samples.link_up",
		"hosted_server_samples.mission_started", "hosted_servers.own_machine",
		"hosted_servers.team_ff", "hosted_servers.update_requested", "lobby_sessions.hosting",
		"lobby_sessions.listed",
		"server_status.maintenance_enabled",
	};
	TEST_EXPECT(flags.size() == expected_flags.size());
	for (size_t i = 0; i < flags.size() && i < expected_flags.size(); ++i)
		TEST_EXPECT(flags[i].as_text(0) == expected_flags[i]);
	TEST_EXPECT(db.query("SELECT 1 FROM information_schema.columns WHERE table_name = 'unknown_messages' "
						 "AND column_name = 'sample' AND data_type = 'bytea';")
						.size() == 1);
	return 0;
}

int test_seed_populates_games() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	const auto migrations = source_dir / "backend" / "migrations";
	const auto seed_dir = source_dir / "backend" / "seed";

	TestDatabase scratch;
	Database db = scratch.open();

	// Apply seeds in filename order, matching the server's apply_seed()
	// (main.cpp). directory_iterator yields entries in an unspecified order
	// that differs across filesystems (sorted on NTFS, inode-order on ext4),
	// and 0002_dev_users.sql seeds player_game_access whose rows join the games
	// created by 0001_*; out of order, they would be missing.
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

	TestDatabase scratch;
	Database db = scratch.open();

	std::vector<std::filesystem::path> seeds;
	for (const auto &e : std::filesystem::directory_iterator(seed_dir)) {
		if (e.path().extension() == ".sql") seeds.push_back(e.path());
	}
	std::sort(seeds.begin(), seeds.end());  // deterministic, FK-safe order

	// Run the seed twice; ON CONFLICT DO NOTHING should keep counts stable.
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
	TestDatabase scratch;
	Database db = scratch.open();

	// A joiner of a host that is not listed is refused (23503).
	bool caught = false;
	try {
		db.exec(
			"INSERT INTO host_players (host_rid, nwhandle, peer_ip, peer_port) "
			"VALUES (9999, 'orphan', '10.0.0.1', 1);"
		);
	} catch (const opennova::db::Error &e) {
		caught = e.sqlstate() == "23503";
	}
	TEST_EXPECT(caught);
	return 0;
}

// Every account starts as a 'player', the role takes only 'player' or
// 'admin', web_sessions is keyed by the token hash with its user_id index, and
// a deleted player takes its sessions.
int test_web_sessions_and_roles() {
	TestDatabase scratch;
	Database db = scratch.open();

	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
	        "VALUES ('roleless', 'x', '0000aaaa', '1', 'Roleless');");
	auto role = db.query("SELECT role FROM players WHERE username = 'roleless';");
	TEST_EXPECT(role.size() == 1 && role[0].as_text(0).value() == "player");
	db.exec("UPDATE players SET role = 'admin' WHERE username = 'roleless';");
	bool caught = false;
	try {
		db.exec("UPDATE players SET role = 'root' WHERE username = 'roleless';");
	} catch (const opennova::db::Error &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	role = db.query("SELECT role FROM players WHERE username = 'roleless';");
	TEST_EXPECT(role[0].as_text(0).value() == "admin");

	TEST_EXPECT(has_index(db, "web_sessions", "idx_web_sessions_user_id"));

	db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
	        "SELECT 'h1', id, (now() + interval '+30 days') FROM players WHERE username = 'roleless';");
	// The token hash is the key: a second row under it is refused.
	caught = false;
	try {
		db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
		        "SELECT 'h1', id, now() FROM players WHERE username = 'roleless';");
	} catch (const opennova::db::Error &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	// A session for no player is refused.
	caught = false;
	try {
		db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
		        "VALUES ('h2', 9999, now());");
	} catch (const opennova::db::Error &) {
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

// The games' title column, and the seed after the migrations (the boot's
// order) writes every row with its title, the OpenNova row among them.
int test_games_title_fresh() {
	const std::filesystem::path source_dir{ OPENNOVA_SOURCE_DIR };
	TestDatabase scratch;
	Database db = scratch.open();
	TEST_EXPECT(has_column(db, "games", "title"));
	TEST_EXPECT(!has_column(db, "games", "executable_name")); // the launcher's, retired
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

// A new account starts at credentials generation 0, a retail login's session
// row carries one, and neither column takes NULL. And account_status: 'active'
// by default, never empty (inserted or updated).
int test_credentials_generation() {
	TestDatabase scratch;
	Database db = scratch.open();

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
		} catch (const opennova::db::Error &) {
			caught = true;
		}
		TEST_EXPECT(caught);
	}
	return 0;
}

// The account deletes and host_keys. players.id is an identity, so a deleted
// account's id is never handed out again; a deleted account leaves its listed
// server listed and its player-list rows in place with no account (ON DELETE
// SET NULL) and takes its hosting keys (ON DELETE CASCADE). host_keys is keyed
// by the key hash, with its indexes; a key needs a player, and a fresh key row
// carries both times and no claim.
int test_host_keys_and_account_deletes() {
	TestDatabase scratch;
	Database db = scratch.open();
	auto add_player = [&db](const std::string &name) {
		db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
		        "VALUES ($1, 'x', $2, '1', $3);",
		        {opennova::db::BindValue(name), opennova::db::BindValue("pc-" + name),
		         opennova::db::BindValue(name)});
		return db.query("SELECT id FROM players WHERE username = $1;", {opennova::db::BindValue(name)})
		        .front()
		        .as_int(0)
		        .value();
	};

	// The newest account deleted, its id stays retired.
	const int64_t first = add_player("first");
	const int64_t newest = add_player("newest");
	TEST_EXPECT(newest > first);
	db.exec("DELETE FROM players WHERE id = $1;", {opennova::db::BindValue(newest)});
	TEST_EXPECT(add_player("newcomer") > newest);

	for (const char *index : {"idx_host_keys_user_id", "idx_host_keys_last_used_at"}) {
		TEST_EXPECT(has_index(db, "host_keys", index));
	}
	const int64_t host = add_player("host");
	db.exec("INSERT INTO host_keys (key_hash, user_id) VALUES ('h1', $1);",
	        {opennova::db::BindValue(host)});
	auto fresh = db.query("SELECT created_at IS NOT NULL, last_used_at IS NOT NULL, claimed_rid "
	                      "FROM host_keys WHERE key_hash = 'h1';");
	TEST_EXPECT(fresh.size() == 1);
	TEST_EXPECT(fresh[0].as_int(0).value() == 1 && fresh[0].as_int(1).value() == 1);
	TEST_EXPECT(!fresh[0].as_int(2).has_value());
	// The hash is the key: a second row under it is refused.
	bool caught = false;
	try {
		db.exec("INSERT INTO host_keys (key_hash, user_id) VALUES ('h1', $1);",
		        {opennova::db::BindValue(first)});
	} catch (const opennova::db::Error &) {
		caught = true;
	}
	TEST_EXPECT(caught);
	// A key for no player is refused.
	caught = false;
	try {
		db.exec("INSERT INTO host_keys (key_hash, user_id) VALUES ('h2', 9999);");
	} catch (const opennova::db::Error &) {
		caught = true;
	}
	TEST_EXPECT(caught);

	// The host's listed server, with the first account in its player list.
	db.exec("INSERT INTO active_hosts (rid, gsid, game, app_id, server_name, host_ip, host_port, "
	        "host_user_id, peer_ip, peer_port) VALUES (7, 'G7', 'jop_2_consumer', '1234', 'Owned', "
	        "'10.0.0.7', 32768, $1, '10.0.0.7', 32768);",
	        {opennova::db::BindValue(host)});
	db.exec("INSERT INTO host_players (host_rid, user_id, nwhandle, peer_ip, peer_port) "
	        "VALUES (7, $1, 'first', '10.0.0.8', 0);",
	        {opennova::db::BindValue(first)});
	db.exec("DELETE FROM players WHERE id IN ($1, $2);",
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
	} catch (const opennova::db::Error &) {
		return true;
	}
	return false;
}

// The CD keys' rules over `db` (migrated): a serial and a wire key are each unique,
// a key's account must exist, an account holds one live key per title (any
// number of revoked ones), a machine is one activation per key, and a
// deleted account takes its keys and their activations, a deleted key its
// activations. `prefix` keeps the rows of one call apart from another's.
int expect_cd_key_rules(Database &db, const std::string &prefix) {
	using opennova::db::BindValue;
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) VALUES ($1, 'x', $2, "
			"'1', $3);",
			{ BindValue(prefix + "owner"), BindValue(prefix + "pc"), BindValue(prefix + "owner") });
	const int64_t owner =
			db.query("SELECT id FROM players WHERE username = $1;", { BindValue(prefix + "owner") })
					.front()
					.as_int(0)
					.value();
	const auto add_key = [&](const std::string &serial, std::optional<int64_t> user, bool revoked) {
		return db.query("INSERT INTO cd_keys (title, serial, wire_key, user_id, created_by, revoked_at) "
						"VALUES ('dfx2', $1, $2, $3, 'token', $4) RETURNING id;",
						{ BindValue(prefix + serial), BindValue(prefix + serial + "-WIRE"),
								user ? BindValue(*user) : BindValue(std::monostate{}),
								revoked ? BindValue(std::string("2026-10-14 00:00:00"))
										: BindValue(std::monostate{}) })
				.front()
				.as_int(0)
				.value();
	};
	const int64_t live = add_key("LIVE", owner, false);
	const auto fresh = db.query(
			"SELECT created_at IS NOT NULL, assigned_at, revoked_at FROM cd_keys WHERE id = $1;",
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
			"$1, $2, $3, 'token');",
			{ BindValue(prefix + "OTHER"), BindValue(prefix + "OTHER-WIRE"), BindValue(owner) });

	db.exec("INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES ($1, "
			"'ABCDEFGHIJKLMNOPQRSTUVW', 'X');",
			{ BindValue(live) });
	const auto activation =
			db.query("SELECT created_at IS NOT NULL FROM cd_key_activations WHERE key_id = $1;",
					{ BindValue(live) });
	TEST_EXPECT(activation.size() == 1 && activation[0].as_int(0).value() == 1);
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES (" +
					std::to_string(live) + ", 'ABCDEFGHIJKLMNOPQRSTUVW', 'Y');"));
	TEST_EXPECT(refused(db,
			"INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES "
			"(999999, 'ABCDEFGHIJKLMNOPQRSTUVW', 'Y');"));
	db.exec("INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES ($1, "
			"'TTTTTTTTTTTTTTTTTTTTTTT', 'Z');",
			{ BindValue(live) });

	// A deleted key takes its activations; a deleted account its keys.
	const int64_t free_key =
			db.query("SELECT id FROM cd_keys WHERE serial = $1;", { BindValue(prefix + "FREE") })
					.front()
					.as_int(0)
					.value();
	db.exec("INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES ($1, "
			"'ABCDEFGHIJKLMNOPQRSTUVW', 'F');",
			{ BindValue(free_key) });
	db.exec("DELETE FROM cd_keys WHERE id = $1;", { BindValue(free_key) });
	TEST_EXPECT(
			db.query("SELECT 1 FROM cd_key_activations WHERE key_id = $1;", { BindValue(free_key) })
					.empty());
	db.exec("DELETE FROM players WHERE id = $1;", { BindValue(owner) });
	TEST_EXPECT(db.query("SELECT 1 FROM cd_keys WHERE serial LIKE $1::text || '%';", { BindValue(prefix) })
					.empty());
	TEST_EXPECT(db.query("SELECT 1 FROM cd_key_activations WHERE key_id = $1;", { BindValue(live) })
					.empty());
	return 0;
}

// The CD keys' two tables, their indexes and their rules.
int test_cd_keys_fresh() {
	TestDatabase scratch;
	Database db = scratch.open();
	for (const char *index :
			{ "idx_cd_keys_title", "idx_cd_keys_user", "idx_cd_keys_live_per_account" }) {
		TEST_EXPECT(has_index(db, "cd_keys", index));
	}
	return expect_cd_key_rules(db, "fresh-");
}

// A hosted server's game: a row that names none lists under Joint Operations'
// lobby, one that names its own keeps it, and the column takes no NULL; a row
// is on a shared machine unless it says otherwise, and an owner may have
// several live (its hosting entitlement, not the schema, bounds them).
int test_hosted_server_game() {
	TestDatabase scratch;
	Database db = scratch.open();
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
	        "VALUES ('host_game', 'x', '0e140001', '1', 'HostGame');");
	db.exec("INSERT INTO hosted_servers (owner_user_id, state, game_name, max_players, time_limit, kill_limit, "
	        "max_score, respawn, start_delay, missions) "
	        "SELECT id, 'stopped', 'Default', 8, 30, 50, 5, 5, 0, 'a.bms' FROM players WHERE username = 'host_game';");
	// A server runs a data set by row (0011_data_sets.sql): the seeded Joint Operations and
	// its Escalation; the old game and expansion columns are gone.
	const auto rows = db.query("SELECT data_set_id, team_ff, own_machine, update_requested FROM hosted_servers "
	                           "WHERE game_name = 'Default';");
	TEST_EXPECT(rows.size() == 1 && rows[0].is_null(0));
	TEST_EXPECT(rows[0].as_int(1) == 0 && rows[0].as_int(2) == 0 && rows[0].as_int(3) == 0);
	TEST_EXPECT(!has_column(db, "hosted_servers", "game") && !has_column(db, "hosted_servers", "expansion"));
	// A slot's report carries why its container never started (0011).
	TEST_EXPECT(has_column(db, "fleet_slot_reports", "reason"));
	db.exec("INSERT INTO hosted_servers (owner_user_id, state, data_set_id, expansion_id, game_name, "
	        "max_players, time_limit, kill_limit, max_score, respawn, start_delay, missions) "
	        "SELECT p.id, 'stopped', b.id, e.id, 'Own', 8, 30, 50, 5, 5, 0, 'a.bms' FROM players p, "
	        "data_sets b, data_sets e WHERE p.username = 'host_game' AND b.game = 'jop_2_consumer' "
	        "AND e.parent_id = b.id AND e.expansion_code = 'jox01';");
	const auto own = db.query("SELECT d.game, e.expansion_code FROM hosted_servers s "
	                          "JOIN data_sets d ON d.id = s.data_set_id JOIN data_sets e ON e.id = s.expansion_id "
	                          "WHERE s.game_name = 'Own';");
	TEST_EXPECT(own.size() == 1 && own[0].as_text(0).value() == "jop_2_consumer" && own[0].as_text(1).value() == "jox01");
	TEST_EXPECT(refused(db, "UPDATE hosted_servers SET data_set_id = 99999 WHERE game_name = 'Own';"));
	db.exec("UPDATE hosted_servers SET state = 'running' WHERE game_name = 'Own';");
	db.exec("UPDATE hosted_servers SET state = 'running', own_machine = true WHERE game_name = 'Default';");
	TEST_EXPECT(db.query("SELECT 1 FROM hosted_servers WHERE state = 'running';").size() == 2);
	TEST_EXPECT(!has_index(db, "hosted_servers", "idx_hosted_servers_one_live"));
	return 0;
}

// The billing tables (0002_billing.sql) and the comp grant beside them: the
// grant's range, a subscription's item and tier rules, the indexes, an account's
// delete taking its customer row and leaving its subscriptions with no account,
// one customer row per account and per Stripe customer.
int test_billing_tables() {
	TestDatabase scratch;
	Database db = scratch.open();
	TEST_EXPECT(has_column(db, "players", "comp_servers") && !has_column(db, "players", "can_host"));
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
	        "VALUES ('bill', 'x', '0b110001', '1', 'Bill'), ('bob', 'x', '0b110002', '1', 'Bob');");
	TEST_EXPECT(db.query("SELECT comp_servers FROM players WHERE username = 'bill';").front().as_int(0) == 0);
	db.exec("UPDATE players SET comp_servers = 16 WHERE username = 'bill';");
	TEST_EXPECT(refused(db, "UPDATE players SET comp_servers = 17 WHERE username = 'bill';"));
	TEST_EXPECT(refused(db, "UPDATE players SET comp_servers = -1 WHERE username = 'bill';"));
	for (const char *index : { "idx_billing_subscriptions_user", "idx_billing_subscriptions_customer" })
		TEST_EXPECT(has_index(db, "billing_subscriptions", index));
	TEST_EXPECT(has_index(db, "billing_events", "idx_billing_events_pending"));

	db.exec("INSERT INTO billing_customers (user_id, stripe_customer_id, livemode) "
	        "SELECT id, 'cus_bill', false FROM players WHERE username = 'bill';");
	TEST_EXPECT(refused(db, "INSERT INTO billing_customers (user_id, stripe_customer_id, livemode) "
	                        "SELECT id, 'cus_bill', false FROM players WHERE username = 'bob';"));
	TEST_EXPECT(refused(db, "INSERT INTO billing_customers (user_id, stripe_customer_id, livemode) "
	                        "SELECT id, 'cus_two', false FROM players WHERE username = 'bill';"));
	db.exec("INSERT INTO billing_subscriptions (stripe_subscription_id, stripe_customer_id, user_id, livemode, "
	        "status, item, price_id, tier) SELECT 'sub_bill', 'cus_bill', id, false, 'active', 'tier', 'price_1', 1 "
	        "FROM players WHERE username = 'bill';");
	const char *bad_rows[] = {
		"INSERT INTO billing_subscriptions (stripe_subscription_id, stripe_customer_id, livemode, status, item, "
		"price_id) VALUES ('sub_x', 'cus_x', false, 'active', 'plan', 'price_x');",
		"INSERT INTO billing_subscriptions (stripe_subscription_id, stripe_customer_id, livemode, status, item, "
		"price_id, tier) VALUES ('sub_y', 'cus_y', false, 'active', 'tier', 'price_y', 4);",
	};
	for (const char *sql : bad_rows) TEST_EXPECT(refused(db, sql));
	const auto row = db.query("SELECT quantity, cancel_at_period_end, current_period_end IS NULL, "
	                          "status_since IS NOT NULL FROM billing_subscriptions WHERE stripe_subscription_id = 'sub_bill';");
	TEST_EXPECT(row.size() == 1 && row[0].as_int(0) == 1 && row[0].as_int(1) == 0 && row[0].as_int(2) == 1 &&
			row[0].as_int(3) == 1);
	db.exec("INSERT INTO billing_events (event_id, type, livemode) VALUES ('evt_1', 'invoice.paid', false);");
	TEST_EXPECT(refused(db, "INSERT INTO billing_events (event_id, type, livemode) VALUES ('evt_1', 'x', false);"));
	TEST_EXPECT(db.query("SELECT customer_id, outcome, processed_at IS NULL FROM billing_events;").front().as_int(2) == 1);
	db.exec("DELETE FROM players WHERE username = 'bill';");
	TEST_EXPECT(db.query("SELECT 1 FROM billing_customers;").empty());
	TEST_EXPECT(db.query("SELECT user_id FROM billing_subscriptions WHERE stripe_subscription_id = 'sub_bill';")
						.front()
						.is_null(0));
	db.exec("INSERT INTO service_meta (key, value) VALUES ('instance_id', 'abc');");
	TEST_EXPECT(refused(db, "INSERT INTO service_meta (key, value) VALUES ('instance_id', 'def');"));
	return 0;
}

// The account email (0005_account_email.sql): players.email, NULL for none
// (as many accounts as like), stored lowercased (an upper-case or empty one is
// refused) and one account's (a unique index); the password reset links, one
// per account (a second row for it is refused, the token's hash unique), gone
// with the account.
int test_account_email() {
	TestDatabase scratch({ /*migrate=*/true });
	Database db = scratch.open();
	TEST_EXPECT(has_column(db, "players", "email"));
	TEST_EXPECT(has_index(db, "players", "idx_players_email"));
	TEST_EXPECT(has_index(db, "password_resets", "idx_password_resets_expires"));
	auto add = [&db](const char *name, const char *pcid) {
		db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) VALUES ($1, 'x', $2, '1', $1);",
				{ BindValue(std::string(name)), BindValue(std::string(pcid)) });
	};
	add("mail_a", "000000a1");
	add("mail_b", "000000b1");
	add("mail_c", "000000c1");
	// Accounts with no email stand side by side.
	TEST_EXPECT(db.query("SELECT COUNT(*) FROM players WHERE email IS NULL;").front().as_int(0) == 3);
	db.exec("UPDATE players SET email = 'a@example.com' WHERE username = 'mail_a';");
	TEST_EXPECT(refused(db, "UPDATE players SET email = 'a@example.com' WHERE username = 'mail_b';"));
	TEST_EXPECT(refused(db, "UPDATE players SET email = 'B@example.com' WHERE username = 'mail_b';"));
	TEST_EXPECT(refused(db, "UPDATE players SET email = '' WHERE username = 'mail_b';"));
	db.exec("UPDATE players SET email = 'b@example.com' WHERE username = 'mail_b';");

	const char *reset = "INSERT INTO password_resets (user_id, token_hash, expires_at) "
						"SELECT id, $1, now() + interval '1 hour' FROM players WHERE username = $2;";
	db.exec(reset, { BindValue(std::string("hash-a")), BindValue(std::string("mail_a")) });
	TEST_EXPECT(refused(db, "INSERT INTO password_resets (user_id, token_hash, expires_at) "
							"SELECT id, 'hash-a2', now() FROM players WHERE username = 'mail_a';"));
	TEST_EXPECT(refused(db, "INSERT INTO password_resets (user_id, token_hash, expires_at) "
							"SELECT id, 'hash-a', now() FROM players WHERE username = 'mail_b';"));
	TEST_EXPECT(refused(db, "INSERT INTO password_resets (user_id, token_hash) "
							"SELECT id, 'hash-c' FROM players WHERE username = 'mail_c';"));
	db.exec("DELETE FROM players WHERE username = 'mail_a';");
	TEST_EXPECT(db.query("SELECT 1 FROM password_resets;").empty());
	return 0;
}

// The job queue (0007_jobs.sql): the states and their rules, the dedupe key unique
// among the queued and running jobs alone, the schedule key unique for good, and the
// billing sync's one state row.
int test_jobs_tables() {
	TestDatabase scratch({ /*migrate=*/true });
	Database db = scratch.open();
	TEST_EXPECT(
			has_index(db, "jobs", "idx_jobs_dedupe") && has_index(db, "jobs", "idx_jobs_queued"));
	db.exec("INSERT INTO jobs (kind, dedupe_key) VALUES ('k', 'd');");
	TEST_EXPECT(refused(db, "INSERT INTO jobs (kind, dedupe_key) VALUES ('k', 'd');"));
	db.exec("UPDATE jobs SET state = 'done' WHERE dedupe_key = 'd';");
	db.exec("INSERT INTO jobs (kind, dedupe_key) VALUES ('k', 'd');");
	db.exec("INSERT INTO jobs (kind, schedule_key) VALUES ('k', 's@1');");
	db.exec("UPDATE jobs SET state = 'failed' WHERE schedule_key = 's@1';");
	TEST_EXPECT(refused(db, "INSERT INTO jobs (kind, schedule_key) VALUES ('k', 's@1');"));
	TEST_EXPECT(refused(db, "INSERT INTO jobs (kind, state) VALUES ('k', 'lost');"));
	TEST_EXPECT(refused(db, "INSERT INTO jobs (kind) VALUES ('');"));
	TEST_EXPECT(refused(db, "INSERT INTO jobs (kind, max_attempts) VALUES ('k', 0);"));
	TEST_EXPECT(db.query("SELECT 1 FROM billing_sync_state WHERE id = 1;").size() == 1);
	TEST_EXPECT(refused(db, "INSERT INTO billing_sync_state (id) VALUES (2);"));
	TEST_EXPECT(refused(
			db, "INSERT INTO fleet_launch_backoff (kind, not_before_ms) VALUES ('big', 1);"));
	// Each kind's failed launches in a row (0010_launch_backoff_failures.sql).
	TEST_EXPECT(refused(db,
			"INSERT INTO fleet_launch_backoff (kind, not_before_ms, failures) VALUES ('own', 1, "
			"-1);"));
	db.exec("INSERT INTO fleet_launch_backoff (kind, not_before_ms) VALUES ('own', 1);");
	TEST_EXPECT(db.query("SELECT failures FROM fleet_launch_backoff WHERE kind = 'own';")
						.front()
						.as_int(0) == 0);
	return 0;
}

// The controller's state (0008_controller_state.sql): the leadership's and the
// limits hold's one rows, the admin links' states (gone with their server), and
// the console's commands with their replies (gone with their command).
int test_controller_tables() {
	TestDatabase scratch({ /*migrate=*/true });
	Database db = scratch.open();
	TEST_EXPECT(
			db.query("SELECT generation FROM controller_leader WHERE id = 1;").front().as_int(0) ==
			0);
	TEST_EXPECT(refused(db, "INSERT INTO controller_leader (id) VALUES (2);"));
	TEST_EXPECT(
			db.query("SELECT held FROM hosted_limits_hold WHERE id = 1;").front().as_int(0) == 0);
	TEST_EXPECT(refused(db, "INSERT INTO hosted_limits_hold (id) VALUES (2);"));
	TEST_EXPECT(has_index(db, "hosted_console_commands", "idx_hosted_console_commands_queued"));
	TEST_EXPECT(refused(
			db, "INSERT INTO hosted_admin_links (server_id, state) VALUES (999999, 'up');"));
	TEST_EXPECT(refused(db,
			"INSERT INTO hosted_console_commands (server_id, line, actor, deadline_at) "
			"VALUES (999999, 'GET GAMESTATE', 'owner', now());"));
	return 0;
}

// The lobby instances (0009_lobby_instances.sql, 0012_lobby_instances_share_endpoint.sql): a row
// per lobby, two of them at one endpoint (the deployment's one lobby address), a state of two,
// the sessions, commands and listings that name an instance gone with it (a name no instance has
// refused), the commands' per-instance index, and the RID sequence (1..0xFFFFFF, cycling).
int test_lobby_instances_tables() {
	TestDatabase scratch({ /*migrate=*/true });
	Database db = scratch.open();
	const int64_t id = db.query("INSERT INTO lobby_instances (endpoint) VALUES ('10.0.0.1:64206') "
								"RETURNING id;")
							   .front()
							   .as_int(0)
							   .value_or(0);
	TEST_EXPECT(id > 0);
	const int64_t beside = db.query("INSERT INTO lobby_instances (endpoint) "
									"VALUES ('10.0.0.1:64206') RETURNING id;")
								   .front()
								   .as_int(0)
								   .value_or(0);
	TEST_EXPECT(beside > id);
	TEST_EXPECT(
			refused(db, "INSERT INTO lobby_instances (endpoint, state) VALUES ('x:1', 'gone');"));
	TEST_EXPECT(refused(db,
			"INSERT INTO lobby_sessions (lobby_id, connection_id, peer_ip, peer_port, state, "
			"connected_at, last_seen_at) VALUES (999999, 1, '10.0.0.2', 1, 'active', now(), "
			"now());"));
	db.exec("INSERT INTO lobby_sessions (lobby_id, connection_id, peer_ip, peer_port, state, "
			"connected_at, last_seen_at) VALUES ($1, 1, '10.0.0.2', 1, 'active', now(), now());",
			{ opennova::db::BindValue(id) });
	const std::string host =
			"INSERT INTO active_hosts (rid, gsid, game, app_id, server_name, host_ip, "
			"host_port, peer_ip, peer_port, lobby_id) VALUES ";
	db.exec(host +
					"(167772161, 'G', 'jop_2_consumer', '1', 'S', '10.0.0.2', 1, '10.0.0.2', 1, "
					"$1);",
			{ opennova::db::BindValue(id) });
	TEST_EXPECT(refused(db,
			host +
					"(167772162, 'G', 'jop_2_consumer', '1', 'S', '10.0.0.2', 1, "
					"'10.0.0.2', 1, 999999);"));
	TEST_EXPECT(has_index(db, "lobby_commands", "idx_lobby_commands_lobby"));
	db.exec("DELETE FROM lobby_instances WHERE id = $1;", { opennova::db::BindValue(id) });
	TEST_EXPECT(db.query("SELECT 1 FROM lobby_sessions;").empty() &&
			db.query("SELECT 1 FROM active_hosts;").empty());
	TEST_EXPECT(db.query("SELECT id FROM lobby_instances;").size() == 1);
	const auto first = db.query("SELECT nextval('lobby_rid_seq');").front().as_int(0).value_or(0);
	TEST_EXPECT(first >= 1 && first <= 0xFFFFFF);
	db.exec("SELECT setval('lobby_rid_seq', 16777215);");
	TEST_EXPECT(db.query("SELECT nextval('lobby_rid_seq');").front().as_int(0) == 1);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	if (test_migrations_create_expected_tables() != 0) return 1;
	if (test_seed_populates_games() != 0) return 1;
	if (test_seed_is_idempotent() != 0) return 1;
	if (test_foreign_keys_enforced() != 0) return 1;
	if (test_web_sessions_and_roles() != 0) return 1;
	if (test_games_title_fresh() != 0)
		return 1;
	if (test_host_keys_and_account_deletes() != 0) return 1;
	if (test_credentials_generation() != 0) return 1;
	if (test_hosted_server_game() != 0) return 1;
	if (test_cd_keys_fresh() != 0)
		return 1;
	if (test_billing_tables() != 0) return 1;
	if (test_account_email() != 0) return 1;
	if (test_jobs_tables() != 0)
		return 1;
	if (test_lobby_instances_tables() != 0)
		return 1;
	if (test_controller_tables() != 0)
		return 1;
	std::printf("OK: backend schema + seed (migrations, seed idempotency, FKs, "
				"web sessions + roles, games titles, host keys, credentials generation, hosted "
				"server games, "
				"cd keys, billing, account email, jobs, controller state)\n");
	return 0;
}
