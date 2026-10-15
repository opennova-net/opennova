#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

// Forward-declared so the wrapper header stays libpq-free for consumers.
struct pg_conn;

namespace opennova::db {

// The NovaWorld service's database: PostgreSQL through libpq, one connection per
// Database. Every connection runs with TimeZone UTC, client_encoding SQL_ASCII
// and these timeouts (ConnectOptions): a statement that waits on a lock longer
// than the lock timeout fails 55P03, one that runs longer than the statement
// timeout fails 57014, and a connection idle inside a transaction for a minute
// is ended by the server. The lock timeout stays below the statement timeout:
// a lock wait is part of its statement (the writer lock is one SELECT
// pg_advisory_xact_lock), so with the two the other way round the statement
// timeout ends the wait first and a lock wait never reads lock_timeout().
inline constexpr int kDefaultLockTimeoutMs = 5000;
inline constexpr int kDefaultStatementTimeoutMs = 15000;
inline constexpr int kIdleInTransactionTimeoutMs = 60000;
// The connections one ConnectionPool opens at most, and how long acquire()
// waits for one to come back when every one is out.
inline constexpr std::size_t kDefaultPoolMax = 16;
inline constexpr int kPoolAcquireTimeoutMs = 30000;

// The advisory lock keys (pg_advisory_*): every Transaction holds the writer
// lock until it ends, and run_migrations (and the seeds main applies after
// it) hold the migration lock for the whole run. Both are per database.
inline constexpr int64_t kWriterLock = 0x4f4e4e57'00000001; // "ONNW" 1
inline constexpr int64_t kMigrationLock = 0x4f4e4e57'00000002; // "ONNW" 2

// Thrown for every failure the wrapper sees: a statement the server refused,
// a connection that would not open or was lost, a misuse of the wrapper.
// sqlstate() is the server's five-character code (empty when the failure
// carried none: a lost connection, a misuse); what() reads
// "postgres <SQLSTATE> (<message>) — <statement>", the statement being the SQL
// text (or what failed, for a connection), never a bound value.
class Error : public std::runtime_error {
public:
	explicit Error(std::string message);
	Error(std::string sqlstate, std::string message, bool connection_lost = false);

	const std::string &sqlstate() const noexcept { return sqlstate_; }
	// 23505: a unique or primary-key constraint refused a row.
	bool unique_violation() const noexcept { return sqlstate_ == "23505"; }
	// 55P03: a lock (a row's, the writer lock) not granted within the lock
	// timeout.
	bool lock_timeout() const noexcept { return sqlstate_ == "55P03"; }
	// The connection is gone (class 08 but 08P01, or libpq reported it bad):
	// nothing this statement wrote is known to have committed.
	bool connection_lost() const noexcept { return connection_lost_; }

private:
	std::string sqlstate_;
	bool connection_lost_ = false;
};

// A boolean bind (a BOOL parameter). Explicit, because a bare bool in the bind
// variant would also be what a string literal converts to.
struct Bool {
	bool value = false;
};

// Cell value for a row scanned out of a query: NULL, an integer (int2/int4/
// int8, and a boolean as 0 or 1), a floating-point number (float4/float8/
// numeric), text, or bytes (bytea). A timestamp reads as text
// "YYYY-MM-DD HH:MM:SS", in UTC (the session's time zone), without its
// fraction or offset; every other type reads as the server's text for it.
using Value = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// One row from a query. columns[i] and values[i] line up.
struct Row {
	std::vector<std::string> columns;
	std::vector<Value> values;

	std::optional<int64_t> as_int(std::size_t i) const;
	std::optional<std::string> as_text(std::size_t i) const;
	bool is_null(std::size_t i) const;
};

// Bound parameter for exec/query: binds[k] fills the statement's `$<k+1>` (one
// may appear several times), and the statement names exactly binds.size() of
// them. std::monostate binds NULL. An int64_t binds as int8, a double as
// float8, a Bool as bool and bytes as bytea; a string (and NULL) binds with no
// type, so the server takes the one its place in the statement gives it. Where
// nothing gives one (a bare `SELECT $1`, `$1 IS NULL`, a function overloaded
// on its argument), the statement casts: `$1::text`.
using BindValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>, Bool>;

// How to reach the database. `url` is a libpq connection URI
// (postgresql://user@host:port/dbname?sslmode=verify-full&sslrootcert=...),
// `password` the role's password when the URI names none (kept apart: it is a
// secret, the URI is not). application_name is what the server's views show
// for the connection. A caller that raises lock_timeout_ms raises
// statement_timeout_ms above it too (the timeouts above).
struct ConnectOptions {
	std::string url;
	std::string password;
	std::string application_name = "opennova-novaworld-server";
	int lock_timeout_ms = kDefaultLockTimeoutMs;
	int statement_timeout_ms = kDefaultStatementTimeoutMs;
};

// `url` with its password (a URI's user:password@) masked, for a log line.
std::string masked_url(std::string_view url);

// One connection. NOT thread-safe: one thread uses a Database at a time;
// changes(), the open transaction and the last error belong to the connection.
// A multi-threaded process gives every thread its own (ConnectionPool below).
//
// A connection the server or the network dropped is reopened (PQreset) before
// the next statement when no transaction was open on it; inside a transaction
// every statement fails connection_lost() until the transaction's guard ends.
// A failed statement is never retried.
class Database {
public:
	// Connects, or throws Error (connection_lost()).
	explicit Database(const ConnectOptions &options);

	~Database();
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;
	Database(Database &&other) noexcept;
	Database &operator=(Database &&other) noexcept;

	// Run one statement; a second statement in `sql` is refused by the
	// server. Rows it returns are dropped (query() keeps them).
	void exec(std::string_view sql);
	void exec(std::string_view sql, const std::vector<BindValue> &binds);

	// Run a script of statements separated by ';' (a migration, a seed), with
	// no binds. Statements run in order; on an error it throws, and without
	// an enclosing transaction the script's earlier statements stay applied
	// only when it holds its own COMMIT (the server runs a script with no
	// BEGIN of its own as one implicit transaction).
	void exec_script(std::string_view sql_script);

	// Run one statement and return every row. For the row counts the service's
	// tables hold; not for streaming large result sets.
	std::vector<Row> query(std::string_view sql);
	std::vector<Row> query(std::string_view sql, const std::vector<BindValue> &binds);

	// Rows the last exec()/query() inserted, updated or deleted.
	int changes() const;

	// Begin/commit/rollback. The caller pairs them; a write that spans several
	// statements takes the Transaction guard below instead.
	void begin();
	void commit();
	void rollback();

	// True while a transaction is open on this connection (a failed one
	// included, until it is rolled back).
	bool in_transaction() const;

	// False once the connection is lost (until a statement reopens it).
	bool connected() const;

private:
	friend class Transaction;
	friend class ReadSnapshot;
	friend class ConnectionPool;

	// Reopens a lost connection that held no transaction; throws otherwise.
	void ensure_connected();
	// The transaction state the last command left (kept for a lost
	// connection, whose own state reads unknown).
	void note_transaction_state();

	pg_conn *conn_ = nullptr;
	int changes_ = 0;
	bool in_transaction_ = false;
	// Set while a ReadSnapshot holds this connection's transaction open, so a
	// Transaction opened inside it is refused.
	bool in_read_snapshot_ = false;
};

// Scoped write transaction: BEGIN, then the writer lock (pg_advisory_xact_lock
// over kWriterLock), held to COMMIT on commit() or ROLLBACK when the guard is
// destroyed uncommitted (an exception unwinding past it). So one Transaction
// runs at a time per database, across every connection and process: a check
// and the write it decides (a cap counted then a row inserted, a row read then
// written back) see no other Transaction's write between them. Concurrent
// readers see the whole transaction or none of it. Taking the lock waits at
// most the lock timeout, then throws Error (lock_timeout()).
//
// Nothing slow inside one (a bcrypt hash, the AWS CLI, a Stripe call): every
// other writer waits on it.
//
// Opened inside another transaction on the same connection it is a SAVEPOINT:
// commit() releases it into the outer transaction, and destroying it
// uncommitted undoes only its own writes (a statement that failed inside it
// included, so the outer transaction goes on). Never inside a ReadSnapshot:
// that transaction is read-only, so the constructor refuses it with Error;
// take the Transaction first, or end the snapshot before writing. commit() of
// a transaction a failed statement left aborted rolls it back and throws.
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

// Scoped read snapshot: BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY, so
// every read on this connection sees the one snapshot its first read took
// while other connections write; destroying the guard ends it. It takes no
// lock, so it never waits on a writer. A read that spans statements and must
// agree with itself (host rows and their rosters) takes one. Inside an open
// transaction it does nothing: that transaction's reads are its own. It is for
// reads only: a write inside it fails (25006), and a Transaction is refused.
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

// A session advisory lock on `key`, held from construction until destruction
// (pg_advisory_lock / pg_advisory_unlock): run_migrations takes
// kMigrationLock this way, and main() around the seeds after it. Taking it
// waits at most the lock timeout, then throws Error.
class SessionLock {
public:
	SessionLock(Database &db, int64_t key);
	~SessionLock();
	SessionLock(const SessionLock &) = delete;
	SessionLock &operator=(const SessionLock &) = delete;

private:
	Database &db_;
	int64_t key_;
};

// Connections to one database, each checked out by one thread at a time. A
// lease owns its connection exclusively until it is destroyed, which puts the
// connection back for the next acquire() (rolling back any transaction left
// open on it, and dropping one that was lost); a thread that runs for the life
// of the process holds one lease for its lifetime, a request handler holds one
// for the request. acquire() hands out an idle connection, opens a new one
// while fewer than `max` are open, and otherwise waits for a lease to come
// back, up to kPoolAcquireTimeoutMs, then throws Error. Thread-safe. Must
// outlive every lease it hands out.
//
// A thread holds one lease at a time, from any pool, and a lease stays on the
// thread that acquired it: code that holds one hands its Database& to what it
// calls. A second acquire() on a thread already holding a lease can wait on
// itself (every lease out held by a thread waiting for a second, none coming
// back), so a debug build (NDEBUG unset) asserts there is none, and that a
// lease ends on its own thread; a thread that serves for the life of the
// process acquires its lease itself.
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
		std::thread::id holder_; // the thread that acquired it
	};

	explicit ConnectionPool(ConnectOptions options, std::size_t max = kDefaultPoolMax);
	ConnectionPool(const ConnectionPool &) = delete;
	ConnectionPool &operator=(const ConnectionPool &) = delete;

	// An idle connection, a newly opened one, or the next one a lease returns.
	// Throws Error when the open fails or none comes back in time. Never on a
	// thread that holds a lease (above).
	Lease acquire();

	// The leases the calling thread holds, from every pool: 0 or 1 where the
	// rule above holds.
	static std::size_t leases_held_by_this_thread();

	// The connection the calling thread holds its lease on, when that lease is
	// this pool's; else null. For a callback whose caller has no connection to
	// hand it, which runs on more than one thread (the ConnectionManager's
	// on_lost fires on whichever thread dropped the connection): it takes that
	// thread's own connection rather than a second lease.
	Database *leased_by_this_thread() const;

	const ConnectOptions &options() const { return options_; }
	std::size_t max() const { return max_; }

private:
	void release(std::unique_ptr<Database> db);

	ConnectOptions options_;
	std::size_t max_;
	std::mutex mu_;
	std::condition_variable returned_;
	std::size_t open_ = 0;
	std::vector<std::unique_ptr<Database>> idle_;
};

// ----------------------------------------------------------------------------
// Migration runner
// ----------------------------------------------------------------------------
// Applies the *.sql files in `migrations_dir`, in lexicographic order, that
// `_schema_migrations` has not recorded, each in its own transaction with its
// row (file name and SHA-256) and with no statement timeout (a file runs as
// long as it takes), and holds kMigrationLock for the whole run, so
// two processes booting on one database apply each file once. A recorded file
// whose SHA-256 no longer matches the file fails the run: migrations are
// append-only, a fix is a new file. A missing directory applies nothing.
//
// Naming: NNNN_descriptive_name.sql (0001_baseline.sql, 0002_billing.sql).
//
// A failing file throws and stops the run: its transaction rolls back, it is
// not recorded, and the files after it are not applied.

struct MigrationResult {
	std::vector<std::string> applied;  // filenames newly applied this call
	std::vector<std::string> skipped;  // filenames already in _schema_migrations
};

MigrationResult run_migrations(Database &db, const std::filesystem::path &migrations_dir);

} // namespace opennova::db
