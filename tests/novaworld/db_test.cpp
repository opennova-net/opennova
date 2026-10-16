// The NovaWorld service's database wrapper (net/novaworld/db/database.h) and its
// migration runner against a real PostgreSQL server (tests/common/
// test_database.h): binds and the types they read back as, the errors and
// their SQLSTATE, transactions and savepoints, read snapshots, the pool's
// bound and its reuse, each thread's lease, Transactions on two connections
// running side by side (no lock of their own), a row lock's wait and its
// timeout, a deadlock, the lock failures counted, a connection the server
// ended, and the runner's records, rollback, statement timeout, checksum
// refusal and database contract.

#include <net/novaworld/db/database.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
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

// The connection's server process id.
int64_t backend_pid(Database &db) {
	return db.query("SELECT pg_backend_pid();")[0].as_int(0).value_or(0);
}

// Waits, up to `limit_ms`, until server process `pid` waits on a lock (a
// pg_locks row of its not granted): whether it does.
bool waits_on_lock(Database &observer, int64_t pid, int64_t limit_ms = 4000) {
	const auto started = Clock::now();
	while (elapsed_ms(started) < limit_ms) {
		if (!observer.query("SELECT 1 FROM pg_locks WHERE pid = $1::integer AND NOT granted;",
							 { BindValue{ pid } })
						.empty())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
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

// A lease held on a thread of its own until the guard ends: a thread holds one
// lease at a time, so a case that needs a second out at once holds it here.
// `first` runs on the lease before the constructor returns.
class HeldLease {
public:
	explicit HeldLease(ConnectionPool &pool, std::function<void(Database &)> first = {}) :
			thread_([this, &pool, first = std::move(first)] {
				auto lease = pool.acquire();
				if (first) first(*lease);
				std::unique_lock<std::mutex> lock(mu_);
				db_ = lease.get();
				ready_ = true;
				cv_.notify_all();
				cv_.wait(lock, [this] { return released_; });
			}) {
		std::unique_lock<std::mutex> lock(mu_);
		cv_.wait(lock, [this] { return ready_; });
	}
	~HeldLease() {
		{
			std::lock_guard<std::mutex> lock(mu_);
			released_ = true;
		}
		cv_.notify_all();
		thread_.join();
	}
	HeldLease(const HeldLease &) = delete;
	HeldLease &operator=(const HeldLease &) = delete;

	// The leased connection; its own thread waits while the case reads it.
	Database *get() const { return db_; }

private:
	std::mutex mu_;
	std::condition_variable cv_;
	bool ready_ = false;
	bool released_ = false;
	Database *db_ = nullptr;
	std::thread thread_; // last: started once the members above exist
};

int test_connection_pool() {
	TestDatabase scratch({ /*migrate=*/false });
	ConnectionPool pool(scratch.options(), 2);
	TEST_EXPECT(pool.max() == 2);
	Database *first = nullptr;
	Database *second = nullptr;
	{
		// Two holders at once get two connections to one database.
		auto a = pool.acquire();
		first = a.get();
		a->exec("CREATE TABLE t (id bigint PRIMARY KEY);");
		// Every connection runs in UTC with the service's timeouts.
		TEST_EXPECT(a->query("SHOW TimeZone;")[0].as_text(0) == "UTC");
		TEST_EXPECT(a->query("SHOW lock_timeout;")[0].as_text(0) == "5s");
		TEST_EXPECT(a->query("SHOW statement_timeout;")[0].as_text(0) == "15s");
		TEST_EXPECT(a->query("SHOW client_encoding;")[0].as_text(0) == "SQL_ASCII");
		HeldLease b(pool, [](Database &db) {
			db.exec("INSERT INTO t VALUES (7);");
			// A transaction left open goes back rolled back.
			db.begin();
			db.exec("INSERT INTO t VALUES (8);");
		});
		second = b.get();
		TEST_EXPECT(first != second);
		TEST_EXPECT(count_rows(*a) == 1);

		// Both are out: a third holder waits until one comes back.
		// The waiter's lease is the one that came back, not a third connection.
		std::atomic<bool> got{ false };
		std::atomic<bool> reused{ false };
		std::thread waiter([&] {
			auto c = pool.acquire();
			reused = c.get() == first || c.get() == second;
			got = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		TEST_EXPECT(!got.load());
		{
			auto released = std::move(a);
		}
		waiter.join();
		TEST_EXPECT(got.load());
		TEST_EXPECT(reused.load());
	}
	{
		// Released connections are reused, not reopened, and none comes back
		// with a transaction open.
		auto c = pool.acquire();
		HeldLease d(pool);
		TEST_EXPECT(c.get() == first || c.get() == second);
		TEST_EXPECT(d.get() == first || d.get() == second);
		TEST_EXPECT(!c->in_transaction() && !d.get()->in_transaction());
		TEST_EXPECT(count_rows(*c) == 1);
	}
	return 0;
}

// The count the one-lease-per-thread assertion reads (a debug build asserts a
// second acquire() on a thread holding a lease; database.h), and the calling
// thread's own connection a callback takes (leased_by_this_thread): the
// lease's own, through a move, from its pool alone, and never another thread's.
int test_thread_leases() {
	TestDatabase scratch({ /*migrate=*/false });
	ConnectionPool pool(scratch.options(), 2);
	ConnectionPool other(scratch.options(), 1);
	TEST_EXPECT(ConnectionPool::leases_held_by_this_thread() == 0);
	TEST_EXPECT(pool.leased_by_this_thread() == nullptr);
	{
		auto lease = pool.acquire();
		TEST_EXPECT(ConnectionPool::leases_held_by_this_thread() == 1);
		TEST_EXPECT(pool.leased_by_this_thread() == lease.get());
		TEST_EXPECT(other.leased_by_this_thread() == nullptr);
		auto moved = std::move(lease);
		TEST_EXPECT(ConnectionPool::leases_held_by_this_thread() == 1);
		TEST_EXPECT(pool.leased_by_this_thread() == moved.get());

		std::atomic<bool> own{ false };
		std::thread elsewhere([&] {
			const bool none = ConnectionPool::leases_held_by_this_thread() == 0 &&
					pool.leased_by_this_thread() == nullptr;
			auto mine = pool.acquire();
			own = none && ConnectionPool::leases_held_by_this_thread() == 1 &&
					pool.leased_by_this_thread() == mine.get() && mine.get() != moved.get();
		});
		elsewhere.join();
		TEST_EXPECT(own.load());
		TEST_EXPECT(ConnectionPool::leases_held_by_this_thread() == 1);
		TEST_EXPECT(pool.leased_by_this_thread() == moved.get());
	}
	TEST_EXPECT(ConnectionPool::leases_held_by_this_thread() == 0);
	TEST_EXPECT(pool.leased_by_this_thread() == nullptr);
	{
		auto lease = other.acquire();
		TEST_EXPECT(other.leased_by_this_thread() == lease.get());
		TEST_EXPECT(pool.leased_by_this_thread() == nullptr);
	}
	TEST_EXPECT(ConnectionPool::leases_held_by_this_thread() == 0);
	return 0;
}

int test_transaction_locks() {
	TestDatabase scratch({ /*migrate=*/false });
	Database a = scratch.open();
	Database b = scratch.open();
	Database observer = scratch.open();
	a.exec("CREATE TABLE t (id bigint PRIMARY KEY);");
	a.exec("INSERT INTO t VALUES (1);");

	// A Transaction takes no lock of its own: two on two connections are open
	// at once, each writing, and neither sees the other's write until it
	// commits. No advisory lock is held.
	{
		Transaction ta(a);
		Transaction tb(b);
		a.exec("INSERT INTO t VALUES (2);");
		b.exec("INSERT INTO t VALUES (3);");
		TEST_EXPECT(count_rows(a) == 2 && count_rows(b) == 2);
		TEST_EXPECT(a.query(kAdvisoryLocksHere)[0].as_int(0) == 0);
		ta.commit();
		TEST_EXPECT(count_rows(b) == 3);
		tb.commit();
	}
	TEST_EXPECT(count_rows(a) == 3);

	// A row lock is the transaction's: another connection's lock of the row
	// waits for its commit, then reads the row as it committed it.
	a.exec("CREATE TABLE u (id bigint PRIMARY KEY, v bigint NOT NULL);");
	a.exec("INSERT INTO u VALUES (1, 0);");
	std::atomic<int64_t> seen{ -1 };
	{
		Transaction holding(a);
		a.exec("UPDATE u SET v = 7 WHERE id = 1;");
		const int64_t b_pid = backend_pid(b);
		std::thread other([&] {
			Transaction tx(b);
			seen = b.query("SELECT v FROM u WHERE id = 1 FOR UPDATE;")[0].as_int(0).value_or(0);
			tx.commit();
		});
		const bool waited = waits_on_lock(observer, b_pid);
		holding.commit();
		other.join();
		TEST_EXPECT(waited);
	}
	TEST_EXPECT(seen.load() == 7);

	// Its wait ends at the lock timeout: 55P03, counted in lock_failures().
	{
		auto options = scratch.options();
		options.lock_timeout_ms = 200;
		Database impatient(options);
		Transaction holding(a);
		a.exec("SELECT v FROM u WHERE id = 1 FOR UPDATE;");
		const auto before = opennova::db::lock_failures();
		const auto started = Clock::now();
		bool timed_out = false;
		{
			Transaction tx(impatient);
			try {
				impatient.exec("SELECT v FROM u WHERE id = 1 FOR UPDATE;");
			} catch (const Error &e) {
				timed_out = e.lock_timeout();
			}
		}
		TEST_EXPECT(timed_out);
		TEST_EXPECT(elapsed_ms(started) >= 150);
		TEST_EXPECT(!impatient.in_transaction());
		TEST_EXPECT(opennova::db::lock_failures().lock_timeouts == before.lock_timeouts + 1);
		TEST_EXPECT(opennova::db::lock_failures().deadlocks == before.deadlocks);
	}
	return 0;
}

// Two transactions each waiting on a row the other holds: the server ends one
// of the two statements (40P01, deadlock()) and the other goes on, and
// lock_failures() counts the one.
int test_deadlock() {
	TestDatabase scratch({ /*migrate=*/false });
	Database a = scratch.open();
	Database b = scratch.open();
	Database observer = scratch.open();
	a.exec("CREATE TABLE t (id bigint PRIMARY KEY, v bigint NOT NULL);");
	a.exec("INSERT INTO t VALUES (1, 0), (2, 0);");
	const auto before = opennova::db::lock_failures();
	std::atomic<int> deadlocks{ 0 };
	std::atomic<int> other_errors{ 0 };
	const auto note = [&](const Error &e) { ++(e.deadlock() ? deadlocks : other_errors); };
	const int64_t a_pid = backend_pid(a);
	{
		Transaction ta(a);
		Transaction tb(b);
		a.exec("UPDATE t SET v = 1 WHERE id = 1;");
		b.exec("UPDATE t SET v = 2 WHERE id = 2;");
		std::thread first([&] {
			try {
				a.exec("UPDATE t SET v = 1 WHERE id = 2;");
			} catch (const Error &e) {
				note(e);
			}
		});
		const bool waited = waits_on_lock(observer, a_pid);
		try {
			b.exec("UPDATE t SET v = 2 WHERE id = 1;");
		} catch (const Error &e) {
			note(e);
		}
		first.join();
		TEST_EXPECT(waited);
	}
	TEST_EXPECT(deadlocks.load() == 1);
	TEST_EXPECT(other_errors.load() == 0);
	TEST_EXPECT(opennova::db::lock_failures().deadlocks == before.deadlocks + 1);
	TEST_EXPECT(opennova::db::lock_failures().lock_timeouts == before.lock_timeouts);
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
	write_file(migrations / "0002_expansions.sql",
			"CREATE TABLE expansions (slug text PRIMARY KEY, name text);"
			"INSERT INTO expansions VALUES ('jox01', 'Joint Operations');");
	// A file runs past the connection's statement timeout (an index built over
	// a large table); the connection keeps its timeout after the run.
	write_file(migrations / "0004_slow.sql",
			"SELECT pg_sleep(0.6); CREATE TABLE slow (id bigint);");
	{
		auto options = scratch.options();
		options.statement_timeout_ms = 200;
		Database db(options);
		TEST_EXPECT(sqlstate_of([&] { db.exec("SELECT pg_sleep(0.6);"); }) == "57014");
		const auto r = run_migrations(db, migrations);
		TEST_EXPECT(r.applied == std::vector<std::string>{ "0004_slow.sql" });
		TEST_EXPECT(r.skipped.size() == 3);
		TEST_EXPECT(db.query("SELECT to_regclass('slow') IS NULL;")[0].as_int(0) == 0);
		TEST_EXPECT(db.query("SHOW statement_timeout;")[0].as_text(0) == "200ms");
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
		{ "thread_leases", test_thread_leases },
		{ "transaction_locks", test_transaction_locks },
		{ "deadlock", test_deadlock },
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
	std::printf(
			"OK: database wrapper, transactions, read snapshots, pool, locks + migration runner\n");
	return 0;
}
