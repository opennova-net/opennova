// The NovaWorld service's database wrapper (net/novaworld/db/database.h) and its
// migration runner against a real PostgreSQL server (tests/common/
// test_database.h): binds and the types they read back as, the errors and
// their SQLSTATE, transactions and savepoints, read snapshots, the pool's
// bound and its reuse, the writer lock across connections and its timeout, a
// connection the server ended, and the runner's records, rollback, checksum
// refusal and database contract.

#include <net/novaworld/db/database.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using opennova::db::BindValue;
using opennova::db::Bool;
using opennova::db::ConnectionPool;
using opennova::db::Database;
using opennova::db::Error;
using opennova::db::ReadSnapshot;
using opennova::db::Transaction;
using opennova::db::run_migrations;
using test_db::TestDatabase;

namespace {

using Clock = std::chrono::steady_clock;

// The advisory locks held on this database (pg_locks is the whole server's).
constexpr const char *kAdvisoryLocksHere =
		"SELECT COUNT(*) FROM pg_locks WHERE locktype = 'advisory' "
		"AND database = (SELECT oid FROM pg_database WHERE datname = current_database());";

int64_t elapsed_ms(Clock::time_point since) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

void write_file(const std::filesystem::path &p, std::string_view body) {
	std::ofstream out(p, std::ios::binary);
	out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

int count_rows(Database &db) {
	return static_cast<int>(db.query("SELECT COUNT(*) FROM t;")[0].as_int(0).value());
}

// The SQLSTATE `fn` throws, or "none".
template <typename Fn> std::string sqlstate_of(Fn fn) {
	try {
		fn();
	} catch (const Error &e) {
		return e.sqlstate();
	}
	return "none";
}

int test_binds_and_types() {
	TestDatabase scratch({ /*migrate=*/false });
	Database db = scratch.open();
	db.exec("CREATE TABLE t (id bigint PRIMARY KEY, name text NOT NULL, flag boolean, "
			"ratio double precision, blob bytea, at timestamptz, small integer, tiny smallint);");
	const std::vector<uint8_t> bytes{ 0x00, 0x01, 0x5c, 0x78, 0xff };
	db.exec("INSERT INTO t (id, name, flag, ratio, blob, at, small, tiny) "
			"VALUES ($1, $2, $3, $4, $5, $6, $7, $7);",
			{ BindValue{ int64_t{ 1 } }, BindValue{ std::string("alpha") }, BindValue{ Bool{ true } },
					BindValue{ 0.1 }, BindValue{ bytes }, BindValue{ std::string("2026-10-14 12:34:56.789+02") },
					BindValue{ int64_t{ 7 } } });
	TEST_EXPECT(db.changes() == 1);
	db.exec("INSERT INTO t (id, name, flag, blob) VALUES ($1, $2, $3, $4);",
			{ BindValue{ int64_t{ 4294967295 } }, BindValue{ std::string("bravo") },
					BindValue{ Bool{ false } }, BindValue{ std::vector<uint8_t>{} } });

	auto rows = db.query("SELECT id, name, flag, ratio, blob, at, small, tiny FROM t ORDER BY id;");
	TEST_EXPECT(rows.size() == 2);
	TEST_EXPECT(rows[0].columns[0] == "id" && rows[0].columns[5] == "at");
	TEST_EXPECT(rows[0].as_int(0) == 1);
	TEST_EXPECT(rows[0].as_text(1) == "alpha");
	TEST_EXPECT(rows[0].as_int(2) == 1); // a boolean reads as 0 or 1
	TEST_EXPECT(std::get<double>(rows[0].values[3]) == 0.1);
	TEST_EXPECT(std::get<std::vector<uint8_t>>(rows[0].values[4]) == bytes);
	// UTC, without the fraction or the offset.
	TEST_EXPECT(rows[0].as_text(5) == "2026-10-14 10:34:56");
	TEST_EXPECT(rows[0].as_int(6) == 7 && rows[0].as_int(7) == 7);
	TEST_EXPECT(rows[1].as_int(0) == 4294967295); // a u32 RID fits
	TEST_EXPECT(rows[1].as_int(2) == 0);
	TEST_EXPECT(std::get<std::vector<uint8_t>>(rows[1].values[4]).empty());
	TEST_EXPECT(rows[1].is_null(3) && rows[1].is_null(5));

	// A numbered parameter may repeat; one bind fills each. A bare one takes
	// its type from a cast.
	rows = db.query("SELECT name FROM t WHERE id = $1 OR id = $1 + $2 ORDER BY id;",
			{ BindValue{ int64_t{ 1 } }, BindValue{ int64_t{ 4294967294 } } });
	TEST_EXPECT(rows.size() == 2);
	rows = db.query("SELECT $1::text, $2::text IS NULL, 2.5::numeric, now() > $3::timestamptz;",
			{ BindValue{ std::string("x") }, BindValue{ std::monostate{} },
					BindValue{ std::string("2000-01-01 00:00:00") } });
	TEST_EXPECT(rows[0].as_text(0) == "x" && rows[0].as_int(1) == 1 && rows[0].as_int(3) == 1);
	TEST_EXPECT(std::get<double>(rows[0].values[2]) == 2.5);

	// Text is the bytes it was sent (SQL_ASCII): a stock client's cp1252 comes
	// back byte for byte, and lower() folds ASCII alone.
	const std::string cp1252 = "Caf\xe9 \x80\x99 X";
	rows = db.query("SELECT $1::text, lower($1::text), length($1::text);", { BindValue{ cp1252 } });
	TEST_EXPECT(rows[0].as_text(0) == cp1252);
	TEST_EXPECT(rows[0].as_text(1) == "caf\xe9 \x80\x99 x");
	TEST_EXPECT(rows[0].as_int(2) == static_cast<int64_t>(cp1252.size()));
	// Byte order (C collation): 'B' < 'a' < 0xe9.
	db.exec("CREATE TABLE o (s text);");
	db.exec("INSERT INTO o VALUES ('a'), ('B'), ($1);", { BindValue{ std::string("\xe9") } });
	rows = db.query("SELECT s FROM o ORDER BY s;");
	TEST_EXPECT(rows.size() == 3 && rows[0].as_text(0) == "B" && rows[1].as_text(0) == "a");
	return 0;
}

int test_errors() {
	TestDatabase scratch({ /*migrate=*/false });
	Database db = scratch.open();
	bool caught = false;
	try {
		db.exec("SELEC 1;"); // a syntax error
	} catch (const Error &e) {
		caught = true;
		TEST_EXPECT(e.sqlstate() == "42601");
		TEST_EXPECT(!e.unique_violation() && !e.lock_timeout() && !e.connection_lost());
		const std::string what = e.what();
		TEST_EXPECT(what.rfind("postgres 42601 (", 0) == 0);
		TEST_EXPECT(what.find(") — SELEC 1;") != std::string::npos);
	}
	TEST_EXPECT(caught);

	db.exec("CREATE TABLE t (id bigint PRIMARY KEY, name text UNIQUE);");
	db.exec("INSERT INTO t VALUES (1, 'alpha');");
	caught = false;
	try {
		db.exec("INSERT INTO t VALUES ($1, $2);", { BindValue{ int64_t{ 2 } }, BindValue{ std::string("alpha") } });
	} catch (const Error &e) {
		caught = e.unique_violation() && e.sqlstate() == "23505";
		// The bound value is not in the text, only the statement.
		TEST_EXPECT(std::string(e.what()).find("— INSERT INTO t VALUES ($1, $2);") != std::string::npos);
	}
	TEST_EXPECT(caught);

	// One statement per exec(); a parameter needs a bind, and an untyped bind
	// a place that types it.
	TEST_EXPECT(sqlstate_of([&] { db.exec("SELECT 1; SELECT 2;"); }) == "42601");
	TEST_EXPECT(sqlstate_of([&] { db.exec("SELECT $2::bigint;", { BindValue{ int64_t{ 1 } } }); }) == "08P01");
	TEST_EXPECT(sqlstate_of([&] {
		db.exec("SELECT $1::bigint;", { BindValue{ int64_t{ 1 } }, BindValue{ std::string("x") } });
	}) == "42P18");
	// A Bool is no integer.
	TEST_EXPECT(sqlstate_of([&] {
		db.exec("INSERT INTO t (id, name) VALUES ($1, 'bool');", { BindValue{ Bool{ true } } });
	}) == "42804");
	// The connection goes on working after each.
	TEST_EXPECT(db.connected());
	TEST_EXPECT(db.query("SELECT 1;").size() == 1);

	// A script runs every statement; its failure names its SQLSTATE.
	db.exec_script("CREATE TABLE s (id bigint); INSERT INTO s VALUES (1); INSERT INTO s VALUES (2);");
	TEST_EXPECT(db.query("SELECT COUNT(*) FROM s;")[0].as_int(0) == 2);
	TEST_EXPECT(sqlstate_of([&] { db.exec_script("INSERT INTO s VALUES (3); ZZZ;"); }) == "42601");
	// No BEGIN of its own: the server ran it as one transaction.
	TEST_EXPECT(db.query("SELECT COUNT(*) FROM s;")[0].as_int(0) == 2);

	TEST_EXPECT(opennova::db::masked_url("postgresql://nw:secret@db.example:5432/novaworld?sslmode=verify-full") ==
			"postgresql://nw:***@db.example:5432/novaworld?sslmode=verify-full");
	TEST_EXPECT(opennova::db::masked_url("postgresql://nw@db/novaworld?password=secret&x=1") ==
			"postgresql://nw@db/novaworld?password=***&x=1");
	TEST_EXPECT(opennova::db::masked_url("postgresql://nw@db/novaworld") == "postgresql://nw@db/novaworld");
	return 0;
}

int test_transactions() {
	TestDatabase scratch({ /*migrate=*/false });
	Database db = scratch.open();
	db.exec("CREATE TABLE t (id bigint PRIMARY KEY);");
	db.begin();
	db.exec("INSERT INTO t VALUES (1);");
	db.rollback();
	TEST_EXPECT(count_rows(db) == 0);

	// Destroyed uncommitted (an exception unwinding past it): rolled back.
	{
		Transaction tx(db);
		db.exec("INSERT INTO t VALUES (1);");
		TEST_EXPECT(db.in_transaction());
	}
	TEST_EXPECT(!db.in_transaction());
	TEST_EXPECT(count_rows(db) == 0);

	// A guard opened inside another is a savepoint: dropping it undoes only
	// its own writes, a statement that failed inside it included, and the
	// outer transaction goes on; committing it hands its writes to the outer.
	{
		Transaction tx(db);
		db.exec("INSERT INTO t VALUES (2);");
		{
			Transaction inner(db);
			db.exec("INSERT INTO t VALUES (3);");
		}
		try {
			Transaction inner(db);
			db.exec("INSERT INTO t VALUES (2);"); // a duplicate: fails
			inner.commit();
		} catch (const Error &e) {
			TEST_EXPECT(e.unique_violation());
		}
		{
			Transaction inner(db);
			db.exec("INSERT INTO t VALUES (4);");
			inner.commit();
		}
		TEST_EXPECT(db.in_transaction());
		tx.commit();
	}
	TEST_EXPECT(!db.in_transaction());
	auto rows = db.query("SELECT id FROM t ORDER BY id;");
	TEST_EXPECT(rows.size() == 2 && rows[0].as_int(0) == 2 && rows[1].as_int(0) == 4);

	// A failed statement the caller caught aborts the whole transaction:
	// commit() says so (25P02) and rolls it back, rather than the server's
	// silent ROLLBACK.
	bool refused = false;
	{
		Transaction tx(db);
		db.exec("INSERT INTO t VALUES (5);");
		try {
			db.exec("INSERT INTO t VALUES (5);");
		} catch (const Error &) {
		}
		try {
			tx.commit();
		} catch (const Error &e) {
			refused = e.sqlstate() == "25P02";
		}
	}
	TEST_EXPECT(refused);
	TEST_EXPECT(!db.in_transaction());
	TEST_EXPECT(count_rows(db) == 2);
	return 0;
}

int test_read_snapshot() {
	TestDatabase scratch({ /*migrate=*/false });
	Database reader = scratch.open();
	Database writer = scratch.open();
	writer.exec("CREATE TABLE t (id bigint PRIMARY KEY);");
	writer.exec("INSERT INTO t VALUES (1);");
	{
		ReadSnapshot snapshot(reader);
		TEST_EXPECT(reader.in_transaction());
		TEST_EXPECT(count_rows(reader) == 1);
		writer.exec("INSERT INTO t VALUES (2);"); // waits on nothing the snapshot holds
		TEST_EXPECT(count_rows(reader) == 1);
		// It is read-only.
		TEST_EXPECT(sqlstate_of([&] { reader.exec("INSERT INTO t VALUES (9);"); }) == "25006");
	}
	TEST_EXPECT(!reader.in_transaction());
	TEST_EXPECT(count_rows(reader) == 2);

	// Inside an open transaction it leaves that transaction to end it.
	{
		Transaction tx(reader);
		{
			ReadSnapshot snapshot(reader);
			TEST_EXPECT(count_rows(reader) == 2);
		}
		TEST_EXPECT(reader.in_transaction());
		tx.commit();
	}

	// A Transaction inside it is refused before it writes; the snapshot stays
	// open until its guard ends it, and a Transaction after it opens as usual.
	{
		ReadSnapshot snapshot(reader);
		TEST_EXPECT(count_rows(reader) == 2);
		bool refused = false;
		try {
			Transaction tx(reader);
		} catch (const Error &e) {
			refused = e.sqlstate().empty();
		}
		TEST_EXPECT(refused);
		TEST_EXPECT(reader.in_transaction());
	}
	{
		Transaction tx(reader);
		reader.exec("INSERT INTO t VALUES (3);");
		tx.commit();
	}
	TEST_EXPECT(count_rows(writer) == 3);
	return 0;
}

int test_connection_pool() {
	TestDatabase scratch({ /*migrate=*/false });
	ConnectionPool pool(scratch.options(), 2);
	TEST_EXPECT(pool.max() == 2);
	Database *first = nullptr;
	Database *second = nullptr;
	{
		// Two holders at once get two connections to one database.
		auto a = pool.acquire();
		auto b = pool.acquire();
		first = a.get();
		second = b.get();
		TEST_EXPECT(first != second);
		a->exec("CREATE TABLE t (id bigint PRIMARY KEY);");
		b->exec("INSERT INTO t VALUES (7);");
		TEST_EXPECT(count_rows(*a) == 1);
		// Every connection runs in UTC with the service's timeouts.
		TEST_EXPECT(b->query("SHOW TimeZone;")[0].as_text(0) == "UTC");
		TEST_EXPECT(b->query("SHOW lock_timeout;")[0].as_text(0) == "5s");
		TEST_EXPECT(b->query("SHOW statement_timeout;")[0].as_text(0) == "15s");
		TEST_EXPECT(b->query("SHOW client_encoding;")[0].as_text(0) == "SQL_ASCII");
		// A transaction left open goes back rolled back.
		b->begin();
		b->exec("INSERT INTO t VALUES (8);");

		// Both are out: a third holder waits until one comes back.
		std::atomic<bool> got{ false };
		std::thread waiter([&] {
			auto c = pool.acquire();
			got = true;
			TEST_EXPECT(c.get() == first || c.get() == second);
			return 0;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		TEST_EXPECT(!got.load());
		{
			auto released = std::move(a);
		}
		waiter.join();
		TEST_EXPECT(got.load());
	}
	{
		// Released connections are reused, not reopened, and none comes back
		// with a transaction open.
		auto c = pool.acquire();
		auto d = pool.acquire();
		TEST_EXPECT(c.get() == first || c.get() == second);
		TEST_EXPECT(d.get() == first || d.get() == second);
		TEST_EXPECT(!c->in_transaction() && !d->in_transaction());
		TEST_EXPECT(count_rows(*c) == 1);
	}
	return 0;
}

int test_writer_lock() {
	TestDatabase scratch({ /*migrate=*/false });
	Database a = scratch.open();
	a.exec("CREATE TABLE t (id bigint PRIMARY KEY);");

	// A Transaction on one connection holds the writer lock: another
	// connection's Transaction waits for its commit, then sees its write.
	std::atomic<int64_t> waited_ms{ -1 };
	std::atomic<int> seen{ -1 };
	{
		Transaction holding(a);
		a.exec("INSERT INTO t VALUES (1);");
		std::thread other([&] {
			Database b = scratch.open();
			const auto started = Clock::now();
			Transaction tx(b);
			waited_ms = elapsed_ms(started);
			seen = count_rows(b);
			tx.commit();
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		TEST_EXPECT(waited_ms.load() == -1);
		holding.commit();
		other.join();
	}
	TEST_EXPECT(waited_ms.load() >= 300);
	TEST_EXPECT(seen.load() == 1);

	// Its wait ends at the lock timeout: 55P03.
	{
		auto options = scratch.options();
		options.lock_timeout_ms = 200;
		Database impatient(options);
		Transaction holding(a);
		const auto started = Clock::now();
		bool timed_out = false;
		try {
			Transaction tx(impatient);
		} catch (const Error &e) {
			timed_out = e.lock_timeout();
		}
		TEST_EXPECT(timed_out);
		TEST_EXPECT(elapsed_ms(started) >= 150);
		TEST_EXPECT(!impatient.in_transaction());
	}
	// The lock is the transaction's: ended, it is free.
	{
		Transaction tx(a);
		const auto locks = a.query("SELECT COUNT(*) FROM pg_locks WHERE locktype = 'advisory' "
								   "AND pid = pg_backend_pid();");
		TEST_EXPECT(locks[0].as_int(0) == 1);
		tx.commit();
	}
	TEST_EXPECT(a.query(kAdvisoryLocksHere)[0].as_int(0) == 0);
	return 0;
}

int test_reconnect() {
	TestDatabase scratch({ /*migrate=*/false });
	Database admin = scratch.open();
	Database db = scratch.open();
	auto terminate = [&](Database &victim) {
		const int64_t pid = victim.query("SELECT pg_backend_pid();")[0].as_int(0).value();
		admin.exec("SELECT pg_terminate_backend($1::integer);", { BindValue{ pid } });
	};

	// Ended between statements: the next statement fails connection_lost()
	// (never retried), and the one after reconnects.
	terminate(db);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	bool lost = false;
	try {
		db.query("SELECT 1;");
	} catch (const Error &e) {
		lost = e.connection_lost();
	}
	TEST_EXPECT(lost);
	TEST_EXPECT(!db.connected());
	TEST_EXPECT(db.query("SELECT 1;").size() == 1);
	TEST_EXPECT(db.connected());

	// Ended inside a transaction: every statement fails until its guard ends
	// it, and nothing runs as if the transaction went on.
	db.exec("CREATE TABLE t (id bigint);");
	{
		Transaction tx(db);
		db.exec("INSERT INTO t VALUES (1);");
		terminate(db);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		bool first = false;
		bool second = false;
		try {
			db.exec("INSERT INTO t VALUES (2);");
		} catch (const Error &e) {
			first = e.connection_lost();
		}
		try {
			db.exec("INSERT INTO t VALUES (3);");
		} catch (const Error &e) {
			second = e.connection_lost();
		}
		TEST_EXPECT(first && second);
	}
	TEST_EXPECT(!db.in_transaction());
	TEST_EXPECT(count_rows(db) == 0);

	// The pool drops a lost connection rather than handing it out again.
	ConnectionPool pool(scratch.options(), 1);
	Database *dropped = nullptr;
	{
		auto lease = pool.acquire();
		dropped = lease.get();
		terminate(*lease);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		try {
			lease->query("SELECT 1;");
		} catch (const Error &) {
		}
	}
	{
		auto lease = pool.acquire();
		TEST_EXPECT(lease->connected());
		TEST_EXPECT(lease->query("SELECT 1;").size() == 1);
	}
	(void)dropped;
	return 0;
}

int test_migrations() {
	test_temp::TempDir temp("db_test_migrations");
	const auto migrations = temp.path / "migrations";
	std::filesystem::create_directories(migrations);
	write_file(migrations / "0001_initial.sql", "CREATE TABLE users (id bigint PRIMARY KEY, name text);");
	write_file(migrations / "0002_expansions.sql",
			"CREATE TABLE expansions (slug text PRIMARY KEY, name text);"
			"INSERT INTO expansions VALUES ('jox01', 'Joint Operations');");

	TestDatabase scratch({ /*migrate=*/false });
	{
		Database db = scratch.open();
		auto r = run_migrations(db, migrations);
		TEST_EXPECT(r.applied == (std::vector<std::string>{ "0001_initial.sql", "0002_expansions.sql" }));
		TEST_EXPECT(r.skipped.empty());
		TEST_EXPECT(db.query("SELECT slug FROM expansions;")[0].as_text(0) == "jox01");
		// Each file's record carries its SHA-256.
		const auto recorded = db.query("SELECT filename, sha256 FROM _schema_migrations ORDER BY filename;");
		TEST_EXPECT(recorded.size() == 2 && recorded[0].as_text(1)->size() == 64);
		// The migration lock is released when the run ends.
		TEST_EXPECT(db.query(kAdvisoryLocksHere)[0].as_int(0) == 0);
	}
	// A second run applies nothing; a new file alone applies.
	{
		Database db = scratch.open();
		auto r = run_migrations(db, migrations);
		TEST_EXPECT(r.applied.empty() && r.skipped.size() == 2);
		write_file(migrations / "0003_lobbies.sql", "CREATE TABLE lobbies (id bigint PRIMARY KEY);");
		r = run_migrations(db, migrations);
		TEST_EXPECT(r.applied == std::vector<std::string>{ "0003_lobbies.sql" } && r.skipped.size() == 2);
	}
	// A failing file rolls back whole and is not recorded; the ones before it
	// stay.
	write_file(migrations / "0004_bad.sql", "CREATE TABLE bad (id bigint); ZZZ NOT VALID SQL;");
	{
		Database db = scratch.open();
		bool caught = false;
		try {
			run_migrations(db, migrations);
		} catch (const Error &e) {
			caught = e.sqlstate() == "42601";
		}
		TEST_EXPECT(caught);
		TEST_EXPECT(!db.in_transaction());
		TEST_EXPECT(db.query("SELECT to_regclass('bad') IS NULL;")[0].as_int(0) == 1);
		TEST_EXPECT(db.query("SELECT COUNT(*) FROM _schema_migrations;")[0].as_int(0) == 3);
	}
	std::filesystem::remove(migrations / "0004_bad.sql");
	// An applied file that changed fails the run: migrations are append-only.
	write_file(migrations / "0002_expansions.sql", "CREATE TABLE expansions (slug text PRIMARY KEY);");
	{
		Database db = scratch.open();
		bool refused = false;
		try {
			run_migrations(db, migrations);
		} catch (const Error &e) {
			refused = std::string(e.what()).find("0002_expansions.sql changed since it was applied") !=
					std::string::npos;
		}
		TEST_EXPECT(refused);
	}
	// A missing directory applies nothing.
	{
		Database db = scratch.open();
		auto r = run_migrations(db, temp.path / "nowhere");
		TEST_EXPECT(r.applied.empty() && r.skipped.empty());
	}

	// The database contract: one made any other way than SQL_ASCII with C
	// collation is refused before anything is applied.
	{
		Database admin(test_db::options_for(test_db::server_url()));
		const std::string name = scratch.name() + "_utf8";
		admin.exec("DROP DATABASE IF EXISTS " + name + ";");
		admin.exec("CREATE DATABASE " + name + " TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C';");
		bool refused = false;
		{
			Database utf8(test_db::options_for(test_db::with_database(test_db::server_url(), name)));
			try {
				run_migrations(utf8, migrations);
			} catch (const Error &e) {
				refused = std::string(e.what()).find("ENCODING 'SQL_ASCII'") != std::string::npos;
			}
			TEST_EXPECT(utf8.query("SELECT to_regclass('_schema_migrations') IS NULL;")[0].as_int(0) == 1);
		}
		admin.exec("DROP DATABASE " + name + " WITH (FORCE);");
		TEST_EXPECT(refused);
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "binds_and_types", test_binds_and_types },
		{ "errors", test_errors },
		{ "transactions", test_transactions },
		{ "read_snapshot", test_read_snapshot },
		{ "connection_pool", test_connection_pool },
		{ "writer_lock", test_writer_lock },
		{ "reconnect", test_reconnect },
		{ "migrations", test_migrations },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: database wrapper, transactions, read snapshots, pool, writer lock + migration runner\n");
	return 0;
}
