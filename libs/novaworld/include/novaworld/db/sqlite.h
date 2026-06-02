#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
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
	std::optional<double> as_real(std::size_t i) const;
	std::optional<std::string> as_text(std::size_t i) const;
	bool is_null(std::size_t i) const;
};

// Bound parameter for exec/query. Use std::monostate to bind NULL.
using BindValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

// RAII handle to a sqlite3* connection. NOT thread-safe — each thread that
// touches the DB should own its own Database (sqlite is configured with
// THREADSAFE=1 so the underlying engine handles internal locking, but the
// prepared-statement cache + transaction semantics on a single handle are
// caller-owned).
class Database {
public:
	// Open or create the database file at `path`. Pass ":memory:" for an
	// in-memory database (handy for tests). Throws SqliteError on failure.
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

	// Begin/commit/rollback. Caller is responsible for pairing — no scope
	// guard yet (intentional; transactions over multiple migration files
	// are explicit in the runner).
	void begin();
	void commit();
	void rollback();

	sqlite3 *raw() { return handle_; }

private:
	sqlite3 *handle_ = nullptr;
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
