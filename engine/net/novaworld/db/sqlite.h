#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Forward-declare so the wrapper header can stay sqlite-free for consumers.
struct sqlite3;
struct sqlite3_stmt;

namespace opennova::db {

// How long a statement waits on another connection's lock before it fails
// SQLITE_BUSY, unless the connection's opener names another timeout.
inline constexpr int kDefaultBusyTimeoutMs = 5000;

// Thrown for any sqlite_*() call that returns an error. code() is the primary
// SQLITE_* result the call returned (the wrapper leaves extended result codes
// off); extended_code() is sqlite3_extended_errcode() beside it, which tells
// apart failures that share a primary code (SQLITE_BUSY_SNAPSHOT, 517: a read
// transaction tried to write after another connection committed, from a plain
// SQLITE_BUSY, 5). For a busy or locked failure of one statement (exec(),
// query()), elapsed_ms() is how long its failing prepare or step ran, which
// tells a busy timeout that ran out (about the whole timeout) from a lock
// sqlite refused without waiting (about none); otherwise it is empty. A failed
// sqlite call's what() reads "sqlite error <code> (<sqlite3_errmsg>)
// [extended <extended_code>, after <elapsed_ms> ms] — <context>", the "after"
// part only when elapsed_ms() is set and the context the statement text or
// file path.
class SqliteError : public std::runtime_error {
public:
	SqliteError(int code, std::string message);
	SqliteError(
			int code, int extended_code, std::optional<int64_t> elapsed_ms, std::string message);
	int code() const noexcept { return code_; }
	int extended_code() const noexcept { return extended_code_; }
	std::optional<int64_t> elapsed_ms() const noexcept { return elapsed_ms_; }

private:
	int code_;
	int extended_code_;
	std::optional<int64_t> elapsed_ms_;
};

// Cell value for a row scanned out of a query. Matches SQLite's storage
// classes (NULL/INT/REAL/TEXT/BLOB).
using Value = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// One row from a query. column_name(i) and value(i) line up.
struct Row {
	std::vector<std::string> columns;
	std::vector<Value> values;

	std::optional<int64_t> as_int(std::size_t i) const;
	std::optional<std::string> as_text(std::size_t i) const;
	bool is_null(std::size_t i) const;
};

// Bound parameter for exec/query: binds[k] fills the statement's `$<k+1>`
// (numbered, as the server dialect spells them; one may appear several times),
// and the statement names exactly binds.size() of them. Use std::monostate to
// bind NULL.
using BindValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// RAII handle to a sqlite3* connection. NOT thread-safe: one thread uses a
// Database at a time. sqlite (THREADSAFE=1, serialized) keeps a shared handle
// from crashing, but changes(), the error text a
// SqliteError carries and any open transaction all belong to the connection,
// so two threads on one handle read each other's results and commit or roll
// back each other's writes. A multi-threaded process gives every thread its
// own connection (ConnectionPool below); the file is WAL, so connections read
// concurrently while one writes, and the busy timeout makes a second writer
// wait rather than fail.
class Database {
public:
	// Open or create the database at `path`, with foreign keys on, WAL
	// journaling at synchronous=NORMAL (the WAL syncs at checkpoints, not at
	// every commit; sqlite.cpp says why) and a busy timeout of
	// `busy_timeout_ms`. ":memory:" is an in-memory database
	// private to this connection (handy for tests); a database several
	// connections share in memory takes a shared-cache URI,
	// "file:<name>?mode=memory&cache=shared". Shared cache locks per table
	// and its SQLITE_LOCKED skips the busy timeout, so a test that writes
	// from several threads at once uses a file. Throws SqliteError on failure.
	explicit Database(
			const std::filesystem::path &path, int busy_timeout_ms = kDefaultBusyTimeoutMs);

	~Database();
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;
	Database(Database &&other) noexcept;
	Database &operator=(Database &&other) noexcept;

	// Run a single statement (no return value, no rows). Trailing
	// statements after the first ';' are ignored — use exec_script() for
	// migration files with multiple statements.
	void exec(std::string_view sql);
	void exec(std::string_view sql, const std::vector<BindValue> &binds);

	// Execute a script that may contain many statements separated by ';'.
	// Used by the migration runner. Statements run sequentially in the
	// order they appear; on error throws and earlier statements stay
	// applied (wrap in a transaction yourself if you need atomicity).
	void exec_script(std::string_view sql_script);

	// Execute a query and return all matching rows. Reasonable for the
	// row-counts the lobby/expansions tables produce; not appropriate for
	// streaming large result sets.
	std::vector<Row> query(std::string_view sql);
	std::vector<Row> query(std::string_view sql, const std::vector<BindValue> &binds);

	// rows changed by the last DML (sqlite3_changes).
	int changes() const;

	// Begin/commit/rollback. Caller is responsible for pairing; a write that
	// spans several statements takes the Transaction guard below instead.
	void begin();
	void commit();
	void rollback();

	// True while a transaction is open on this connection
	// (!sqlite3_get_autocommit).
	bool in_transaction() const;

private:
	friend class Transaction;
	friend class ReadSnapshot;

	sqlite3 *handle_ = nullptr;
	// Set while a ReadSnapshot holds this connection's transaction open, so a
	// Transaction opened inside it is refused.
	bool in_read_snapshot_ = false;
};

// Scoped write transaction: BEGIN IMMEDIATE on construction, COMMIT on
// commit(), ROLLBACK when destroyed uncommitted (an exception unwinding past
// it). IMMEDIATE takes the write lock up front, so a concurrent writer is
// waited out by the busy timeout here; a deferred transaction that reads and
// then writes fails SQLITE_BUSY with no retry when another connection
// committed in between. Concurrent readers on other connections see the whole
// transaction or none of it. Opened inside another transaction on the same
// connection it is a SAVEPOINT: commit() releases it into the outer
// transaction, and destroying it uncommitted undoes only its own writes.
// Never inside a ReadSnapshot: there the SAVEPOINT's first write would upgrade
// the snapshot's read, and sqlite upgrades a read without calling the busy
// handler, so it fails at once with SQLITE_BUSY (another connection holds the
// write lock) or SQLITE_BUSY_SNAPSHOT (another connection committed since the
// snapshot began). The constructor refuses it with SqliteError (SQLITE_MISUSE);
// take the Transaction first, or end the snapshot before writing.
class Transaction {
public:
	explicit Transaction(Database &db);
	~Transaction();
	Transaction(const Transaction &) = delete;
	Transaction &operator=(const Transaction &) = delete;

	void commit();

private:
	Database &db_;
	bool nested_;
	bool open_ = true;
};

// Scoped read snapshot: a deferred BEGIN, so the first read pins one WAL
// snapshot and every later read on this connection sees that same commit
// while other connections write; destroying the guard ends it. It takes no
// write lock (a Transaction does), so it never waits on a writer. A read
// that spans statements and must agree with itself (host rows and their
// rosters) takes one. Inside an open transaction it does nothing: that
// transaction already reads one snapshot. It is for reads only: a write
// inside it, a Transaction (refused, see above) or a bare exec(), upgrades
// its read without waiting on the busy timeout.
class ReadSnapshot {
public:
	explicit ReadSnapshot(Database &db);
	~ReadSnapshot();
	ReadSnapshot(const ReadSnapshot &) = delete;
	ReadSnapshot &operator=(const ReadSnapshot &) = delete;

private:
	Database &db_;
	bool open_;
};

// Connections to one database, each checked out by one thread at a time. A
// lease owns its connection exclusively until it is destroyed, which puts the
// connection back for the next acquire() (rolling back any transaction left
// open on it); a thread that runs for the life of the process holds one lease
// for its lifetime, a request handler holds one for the request. acquire()
// opens a new connection when none is idle, so the pool grows to the peak
// number of concurrent leases and keeps them open. Thread-safe. Must outlive
// every lease it hands out.
class ConnectionPool {
public:
	class Lease {
	public:
		Lease(Lease &&other) noexcept = default;
		Lease &operator=(Lease &&other) = delete;
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;
		~Lease();

		Database &operator*() const { return *db_; }
		Database *operator->() const { return db_.get(); }
		Database *get() const { return db_.get(); }

	private:
		friend class ConnectionPool;
		Lease(ConnectionPool &pool, std::unique_ptr<Database> db);

		ConnectionPool *pool_;
		std::unique_ptr<Database> db_;
	};

	// `path` as Database takes it, and the busy timeout every connection the
	// pool opens gets. A path every connection would open as its
	// own private database is refused with SqliteError: ":memory:", an empty
	// path or file: URI name (a private temporary file), and an in-memory
	// file: URI (":memory:" as the name, or mode=memory) without
	// cache=shared. Use "file:<name>?mode=memory&cache=shared" instead.
	explicit ConnectionPool(
			std::filesystem::path path, int busy_timeout_ms = kDefaultBusyTimeoutMs);
	ConnectionPool(const ConnectionPool &) = delete;
	ConnectionPool &operator=(const ConnectionPool &) = delete;

	// An idle connection, or a newly opened one. Throws SqliteError when the
	// open fails.
	Lease acquire();

	const std::filesystem::path &path() const { return path_; }

private:
	void release(std::unique_ptr<Database> db);

	std::filesystem::path path_;
	int busy_timeout_ms_;
	std::mutex mu_;
	std::vector<std::unique_ptr<Database>> idle_;
};

// ----------------------------------------------------------------------------
// Migration runner
// ----------------------------------------------------------------------------
// Reads *.sql files from `migrations_dir` in lexicographic order, applies
// any whose filename hasn't been recorded in the `_schema_migrations` table
// yet, and records each on success. Idempotent — calling it on every server
// boot is safe.
//
// Naming convention (enforced loosely): NNNN_descriptive_name.sql, e.g.
// 0001_initial.sql, 0002_users.sql. NNNN is what gets stored as the
// migration version; the wrapper just uses the full filename string.
//
// On any individual migration failing, the function throws and stops —
// remaining files won't be applied. The currently-failing file is NOT
// marked applied. You can re-run after fixing the SQL.

struct MigrationResult {
	std::vector<std::string> applied;  // filenames newly applied this call
	std::vector<std::string> skipped;  // filenames already in _schema_migrations
};

MigrationResult run_migrations(Database &db, const std::filesystem::path &migrations_dir);

} // namespace opennova::db
