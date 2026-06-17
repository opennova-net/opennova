#include <novaworld/db/sqlite.h>

#include "../common/test_expect.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using opennova::db::BindValue;
using opennova::db::Database;
using opennova::db::run_migrations;
using opennova::db::SqliteError;

namespace {

// Generate a unique temp dir under the system temp for test isolation.
std::filesystem::path scratch_dir() {
	auto base = std::filesystem::temp_directory_path() / "opennova_sqlite_test";
	std::random_device rd;
	std::mt19937_64 gen(rd());
	auto dir = base / std::to_string(gen());
	std::filesystem::create_directories(dir);
	return dir;
}

void write_file(const std::filesystem::path &p, std::string_view body) {
	std::ofstream out(p, std::ios::binary);
	out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

int test_open_and_basic_exec() {
	Database db(":memory:");
	db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL);");
	db.exec("INSERT INTO t (id, name) VALUES (?, ?);",
	        {BindValue{int64_t{1}}, BindValue{std::string("alpha")}});
	db.exec("INSERT INTO t (id, name) VALUES (?, ?);",
	        {BindValue{int64_t{2}}, BindValue{std::string("bravo")}});

	auto rows = db.query("SELECT id, name FROM t ORDER BY id;");
	TEST_EXPECT(rows.size() == 2);
	TEST_EXPECT(rows[0].as_int(0).value() == 1);
	TEST_EXPECT(rows[0].as_text(1).value() == "alpha");
	TEST_EXPECT(rows[1].as_int(0).value() == 2);
	TEST_EXPECT(rows[1].as_text(1).value() == "bravo");
	TEST_EXPECT(rows[0].columns[0] == "id");
	TEST_EXPECT(rows[0].columns[1] == "name");
	return 0;
}

int test_null_and_blob() {
	Database db(":memory:");
	db.exec("CREATE TABLE t (k INTEGER PRIMARY KEY, blob BLOB);");
	db.exec("INSERT INTO t (k, blob) VALUES (1, NULL);");
	db.exec("INSERT INTO t (k, blob) VALUES (?, ?);",
	        {BindValue{int64_t{2}}, BindValue{std::vector<uint8_t>{0x01, 0x02, 0xff}}});

	auto rows = db.query("SELECT blob FROM t ORDER BY k;");
	TEST_EXPECT(rows.size() == 2);
	TEST_EXPECT(rows[0].is_null(0));
	TEST_EXPECT(!rows[1].is_null(0));
	return 0;
}

int test_error_throws_with_context() {
	Database db(":memory:");
	bool caught = false;
	try {
		db.exec("SELECT FROM nowhere;"); // syntax error
	} catch (const SqliteError &e) {
		caught = true;
		const std::string what = e.what();
		TEST_EXPECT(what.find("sqlite error") != std::string::npos);
	}
	TEST_EXPECT(caught);
	return 0;
}

int test_transaction_rollback() {
	Database db(":memory:");
	db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY);");
	db.begin();
	db.exec("INSERT INTO t VALUES (1);");
	db.exec("INSERT INTO t VALUES (2);");
	db.rollback();
	auto rows = db.query("SELECT COUNT(*) FROM t;");
	TEST_EXPECT(rows[0].as_int(0).value() == 0);

	db.begin();
	db.exec("INSERT INTO t VALUES (3);");
	db.commit();
	rows = db.query("SELECT COUNT(*) FROM t;");
	TEST_EXPECT(rows[0].as_int(0).value() == 1);
	return 0;
}

int test_migrations_apply_then_skip() {
	auto dir = scratch_dir();
	auto db_path = dir / "state.db";
	auto migrations = dir / "migrations";
	std::filesystem::create_directories(migrations);

	write_file(migrations / "0001_initial.sql",
	           "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);");
	write_file(migrations / "0002_expansions.sql",
	           "CREATE TABLE expansions (slug TEXT PRIMARY KEY, name TEXT);"
	           "INSERT INTO expansions VALUES ('jox01', 'Joint Operations');");

	{
		Database db(db_path);
		auto r = run_migrations(db, migrations);
		TEST_EXPECT(r.applied.size() == 2);
		TEST_EXPECT(r.applied[0] == "0001_initial.sql");
		TEST_EXPECT(r.applied[1] == "0002_expansions.sql");
		TEST_EXPECT(r.skipped.empty());

		auto rows = db.query("SELECT slug, name FROM expansions;");
		TEST_EXPECT(rows.size() == 1);
		TEST_EXPECT(rows[0].as_text(0).value() == "jox01");
	}

	// Reopening: nothing new should be applied.
	{
		Database db(db_path);
		auto r = run_migrations(db, migrations);
		TEST_EXPECT(r.applied.empty());
		TEST_EXPECT(r.skipped.size() == 2);
	}

	// Add a third migration; only that one applies.
	write_file(migrations / "0003_lobbies.sql",
	           "CREATE TABLE lobbies (id INTEGER PRIMARY KEY, name TEXT);");
	{
		Database db(db_path);
		auto r = run_migrations(db, migrations);
		TEST_EXPECT(r.applied.size() == 1);
		TEST_EXPECT(r.applied[0] == "0003_lobbies.sql");
		TEST_EXPECT(r.skipped.size() == 2);
	}

	std::filesystem::remove_all(dir);
	return 0;
}

int test_migrations_rollback_on_error() {
	auto dir = scratch_dir();
	auto db_path = dir / "state.db";
	auto migrations = dir / "migrations";
	std::filesystem::create_directories(migrations);

	write_file(migrations / "0001_ok.sql",
	           "CREATE TABLE a (id INTEGER PRIMARY KEY);");
	// Bad SQL — syntax error on the second statement.
	write_file(migrations / "0002_bad.sql",
	           "CREATE TABLE b (id INTEGER PRIMARY KEY);"
	           "ZZZ NOT VALID SQL;");

	// Wrap DB in a scope so the connection closes before remove_all() —
	// on Windows, deleting the SQLite WAL/SHM siblings while the handle
	// is still open trips a stack guard fault during sqlite3_close_v2.
	{
		Database db(db_path);
		bool caught = false;
		try {
			run_migrations(db, migrations);
		} catch (const SqliteError &) {
			caught = true;
		}
		TEST_EXPECT(caught);

		// 0001 should be applied; 0002 should NOT be (rolled back) and not
		// in _schema_migrations either.
		auto applied = db.query(
			"SELECT filename FROM _schema_migrations ORDER BY filename;");
		TEST_EXPECT(applied.size() == 1);
		TEST_EXPECT(applied[0].as_text(0).value() == "0001_ok.sql");
	}

	std::filesystem::remove_all(dir);
	return 0;
}

int test_migrations_no_dir_is_not_an_error() {
	Database db(":memory:");
	auto r = run_migrations(db, "/path/that/does/not/exist/anywhere");
	TEST_EXPECT(r.applied.empty());
	TEST_EXPECT(r.skipped.empty());
	return 0;
}

} // namespace

int main() {
	if (test_open_and_basic_exec() != 0) return 1;
	if (test_null_and_blob() != 0) return 1;
	if (test_error_throws_with_context() != 0) return 1;
	if (test_transaction_rollback() != 0) return 1;
	if (test_migrations_apply_then_skip() != 0) return 1;
	if (test_migrations_rollback_on_error() != 0) return 1;
	if (test_migrations_no_dir_is_not_an_error() != 0) return 1;
	std::printf("OK: SQLite wrapper + migration runner (apply/skip/rollback)\n");
	return 0;
}
