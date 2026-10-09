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

// Thrown for any sqlite_*() call that returns an error. `code` is the raw
// SQLITE_* return; `message` is the sqlite3_errmsg() text plus a context
// hint (statement text, file path, etc.) when the wrapper has it.
class SqliteError : public std::runtime_error {
public:
	SqliteError(int code, std::string message);
	int code() const noexcept { return code_; }

private:
	int code_;
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

// Bound parameter for exec/query. Use std::monostate to bind NULL.
using BindValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// RAII handle to a sqlite3* connection. NOT thread-safe: one thread uses a
// Database at a time. sqlite (THREADSAFE=1, serialized) keeps a shared handle
// from crashing, but last_insert_rowid(), changes(), the error text a
// SqliteError carries and any open transaction all belong to the connection,
// so two threads on one handle read each other's results and commit or roll
// back each other's writes. A multi-threaded process gives every thread its
// own connection (ConnectionPool below); the file is WAL, so connections read
// concurrently while one writes, and the busy timeout makes a second writer
// wait rather than fail.
class Database {
public:
	// Open or create the database at `path`, with foreign keys on, WAL
	// journaling and a 5 s busy timeout. ":memory:" is an in-memory database
	// private to this connection (handy for tests); a database several
	// connections share in memory takes a shared-cache URI,
	// "file:<name>?mode=memory&cache=shared". Shared cache locks per table
	// and its SQLITE_LOCKED skips the busy timeout, so a test that writes
	// from several threads at once uses a file. Throws SqliteError on failure.
	explicit Database(const std::filesystem::path &path);

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

	// Last INSERT row id (sqlite3_last_insert_rowid).
	int64_t last_insert_rowid() const;

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
	sqlite3 *handle_ = nullptr;
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
// transaction already reads one snapshot.
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

	// `path` as Database takes it. A path every connection would open as its
	// own private database is refused with SqliteError: ":memory:", an empty
	// path or file: URI name (a private temporary file), and an in-memory
	// file: URI (":memory:" as the name, or mode=memory) without
	// cache=shared. Use "file:<name>?mode=memory&cache=shared" instead.
	explicit ConnectionPool(std::filesystem::path path);
	ConnectionPool(const ConnectionPool &) = delete;
	ConnectionPool &operator=(const ConnectionPool &) = delete;

	// An idle connection, or a newly opened one. Throws SqliteError when the
	// open fails.
	Lease acquire();

	const std::filesystem::path &path() const { return path_; }

private:
	void release(std::unique_ptr<Database> db);

	std::filesystem::path path_;
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
