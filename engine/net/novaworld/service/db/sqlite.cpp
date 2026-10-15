#include <net/novaworld/db/sqlite.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

namespace opennova::db {

// ----------------------------------------------------------------------------
// SqliteError
// ----------------------------------------------------------------------------

SqliteError::SqliteError(int code, std::string message) :
		SqliteError(code, code, std::nullopt, std::move(message)) {}

SqliteError::SqliteError(
		int code, int extended_code, std::optional<int64_t> elapsed_ms, std::string message) :
		std::runtime_error(std::move(message)),
		code_(code),
		extended_code_(extended_code),
		elapsed_ms_(elapsed_ms) {}

namespace {

using Clock = std::chrono::steady_clock;

// The error the failed call that returned `rc` left on `handle`: its message
// and extended code, read before anything else runs on the connection, and
// for a busy or locked failure the time since `started` (when the failing
// prepare or step began) when the caller timed it. A busy timeout that ran out
// shows about the whole timeout, a lock sqlite refused without waiting about
// none.
SqliteError sqlite_error(sqlite3 *handle, int rc, std::string_view ctx,
		std::optional<Clock::time_point> started = std::nullopt) {
	int extended = handle ? sqlite3_extended_errcode(handle) : rc;
	if ((extended & 0xff) != (rc & 0xff))
		extended = rc; // not this call's error
	std::optional<int64_t> elapsed_ms;
	const int primary = rc & 0xff;
	if (started && (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)) {
		const auto elapsed = Clock::now() - *started;
		elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	}
	std::ostringstream os;
	os << "sqlite error " << rc;
	if (handle) {
		const char *msg = sqlite3_errmsg(handle);
		if (msg && *msg) {
			os << " (" << msg << ")";
		}
	}
	os << " [extended " << extended;
	if (elapsed_ms)
		os << ", after " << *elapsed_ms << " ms";
	os << "]";
	if (!ctx.empty()) {
		os << " — " << ctx;
	}
	return SqliteError(rc, extended, elapsed_ms, os.str());
}

[[noreturn]] void throw_sqlite(sqlite3 *handle, int rc, std::string_view ctx) {
	throw sqlite_error(handle, rc, ctx);
}

// A prepared statement, finalized however its scope ends: a bind, step or row
// copy that throws would otherwise leak it, and sqlite3_close_v2 leaves a
// connection with an unfinalized statement open as a zombie. A throw reads the
// failed call's error in its operand, before unwinding finalizes the statement.
struct StatementFinalizer {
	void operator()(sqlite3_stmt *stmt) const { sqlite3_finalize(stmt); }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

Statement prepare(sqlite3 *handle, std::string_view sql, int &rc) {
	sqlite3_stmt *stmt = nullptr;
	rc = sqlite3_prepare_v2(handle, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
	return Statement(stmt);
}

// Bind `v` to the statement's parameter `$<n>` (1-based), the placeholder the
// SQL spells: the server dialect's numbered parameters, which sqlite takes as
// named ones. A `$n` the statement does not name fails SQLITE_RANGE, as a bind
// past a statement's parameters does.
void bind_at(sqlite3_stmt *stmt, int n, const BindValue &v) {
	const std::string name = "$" + std::to_string(n);
	const int i = sqlite3_bind_parameter_index(stmt, name.c_str());
	if (i == 0) {
		throw SqliteError(SQLITE_RANGE, "bind parameter " + name + ": the statement names no " + name);
	}
	int rc = SQLITE_OK;
	std::visit([&](auto &&val) {
		using T = std::decay_t<decltype(val)>;
		if constexpr (std::is_same_v<T, std::monostate>) {
			rc = sqlite3_bind_null(stmt, i);
		} else if constexpr (std::is_same_v<T, int64_t>) {
			rc = sqlite3_bind_int64(stmt, i, val);
		} else if constexpr (std::is_same_v<T, double>) {
			rc = sqlite3_bind_double(stmt, i, val);
		} else if constexpr (std::is_same_v<T, std::string>) {
			rc = sqlite3_bind_text(stmt, i, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
		} else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
			rc = sqlite3_bind_blob(stmt, i, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT);
		}
	}, v);
	if (rc != SQLITE_OK) {
		throw_sqlite(sqlite3_db_handle(stmt), rc, "bind parameter");
	}
}

// Every bind, `binds[k]` to `$<k+1>`. The statement names exactly those
// parameters: one it names that no bind fills fails SQLITE_RANGE too, rather
// than reading NULL.
void bind_all(sqlite3_stmt *stmt, const std::vector<BindValue> &binds) {
	for (std::size_t i = 0; i < binds.size(); ++i) {
		bind_at(stmt, static_cast<int>(i + 1), binds[i]);
	}
	const int named = sqlite3_bind_parameter_count(stmt);
	if (static_cast<std::size_t>(named) != binds.size()) {
		throw SqliteError(SQLITE_RANGE, "the statement names " + std::to_string(named) +
				" parameters, " + std::to_string(binds.size()) + " bound");
	}
}

Value column_value(sqlite3_stmt *stmt, int i) {
	const int type = sqlite3_column_type(stmt, i);
	switch (type) {
	case SQLITE_NULL:
		return std::monostate{};
	case SQLITE_INTEGER:
		return static_cast<int64_t>(sqlite3_column_int64(stmt, i));
	case SQLITE_FLOAT:
		return sqlite3_column_double(stmt, i);
	case SQLITE_TEXT: {
		const auto *txt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
		const int len = sqlite3_column_bytes(stmt, i);
		return std::string(txt ? txt : "", len > 0 ? static_cast<std::size_t>(len) : 0);
	}
	case SQLITE_BLOB: {
		const auto *blob = static_cast<const uint8_t *>(sqlite3_column_blob(stmt, i));
		const int len = sqlite3_column_bytes(stmt, i);
		return std::vector<uint8_t>(blob, blob + (len > 0 ? len : 0));
	}
	default:
		return std::monostate{};
	}
}

} // namespace

// ----------------------------------------------------------------------------
// Row
// ----------------------------------------------------------------------------

std::optional<int64_t> Row::as_int(std::size_t i) const {
	if (i >= values.size()) return std::nullopt;
	if (auto *v = std::get_if<int64_t>(&values[i])) return *v;
	return std::nullopt;
}

std::optional<std::string> Row::as_text(std::size_t i) const {
	if (i >= values.size()) return std::nullopt;
	if (auto *v = std::get_if<std::string>(&values[i])) return *v;
	return std::nullopt;
}

bool Row::is_null(std::size_t i) const {
	if (i >= values.size()) return true;
	return std::holds_alternative<std::monostate>(values[i]);
}

// ----------------------------------------------------------------------------
// Database
// ----------------------------------------------------------------------------

Database::Database(const std::filesystem::path &path, int busy_timeout_ms) {
	const std::string utf8 = path.string();
	const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI;
	int rc = sqlite3_open_v2(utf8.c_str(), &handle_, flags, nullptr);
	if (rc != SQLITE_OK) {
		// `handle_` may be non-null even on open failure — sqlite3_close_v2
		// is the documented cleanup, after the error is read off it.
		sqlite3 *bad = handle_;
		handle_ = nullptr;
		SqliteError error = sqlite_error(bad, rc, "open " + utf8);
		if (bad)
			sqlite3_close_v2(bad);
		throw error;
	}
	// Useful pragmas for an embedded server.
	sqlite3_busy_timeout(handle_, busy_timeout_ms);
	sqlite3_exec(handle_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
	sqlite3_exec(handle_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
	// Per connection (the file does not keep it). Under WAL, NORMAL syncs the
	// WAL before each checkpoint rather than at every commit as FULL (the
	// default) does, so most commits hold the write lock for no fsync. The
	// exception is the first commit after each WAL restart, which writes and
	// syncs the new WAL header with the lock held. Writers that take the lock
	// back to back (and the busy handler polls, so a waiter can starve behind
	// them) then no longer pay a loaded disk's fsync each, which at FULL could
	// add up past the busy timeout. The file stays consistent; a power loss or
	// OS crash (not a process crash) can roll back the last commits before it,
	// which this service's disposable database accepts.
	sqlite3_exec(handle_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
}

Database::~Database() {
	if (handle_) {
		sqlite3_close_v2(handle_);
	}
}

Database::Database(Database &&other) noexcept :
		handle_(other.handle_), in_read_snapshot_(other.in_read_snapshot_) {
	other.handle_ = nullptr;
	other.in_read_snapshot_ = false;
}

Database &Database::operator=(Database &&other) noexcept {
	if (this != &other) {
		if (handle_) sqlite3_close_v2(handle_);
		handle_ = other.handle_;
		in_read_snapshot_ = other.in_read_snapshot_;
		other.handle_ = nullptr;
		other.in_read_snapshot_ = false;
	}
	return *this;
}

void Database::exec(std::string_view sql) {
	exec(sql, {});
}

void Database::exec(std::string_view sql, const std::vector<BindValue> &binds) {
	const Clock::time_point prepare_started = Clock::now();
	int rc = SQLITE_OK;
	const Statement stmt = prepare(handle_, sql, rc);
	if (rc != SQLITE_OK) {
		throw sqlite_error(
				handle_, rc, std::string("prepare: ") + std::string(sql), prepare_started);
	}
	bind_all(stmt.get(), binds);
	const Clock::time_point step_started = Clock::now();
	rc = sqlite3_step(stmt.get());
	if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
		throw sqlite_error(handle_, rc, std::string("exec: ") + std::string(sql), step_started);
	}
}

void Database::exec_script(std::string_view sql_script) {
	const std::string copy(sql_script); // sqlite3_exec needs NUL-terminated
	int rc = sqlite3_exec(handle_, copy.c_str(), nullptr, nullptr, nullptr);
	if (rc != SQLITE_OK) {
		throw_sqlite(handle_, rc, "exec_script");
	}
}

std::vector<Row> Database::query(std::string_view sql) {
	return query(sql, {});
}

std::vector<Row> Database::query(std::string_view sql, const std::vector<BindValue> &binds) {
	const Clock::time_point prepare_started = Clock::now();
	int rc = SQLITE_OK;
	const Statement stmt = prepare(handle_, sql, rc);
	if (rc != SQLITE_OK) {
		throw sqlite_error(
				handle_, rc, std::string("prepare: ") + std::string(sql), prepare_started);
	}
	bind_all(stmt.get(), binds);

	const int ncol = sqlite3_column_count(stmt.get());
	std::vector<std::string> columns;
	columns.reserve(ncol);
	for (int i = 0; i < ncol; ++i) {
		const char *name = sqlite3_column_name(stmt.get(), i);
		columns.emplace_back(name ? name : "");
	}

	std::vector<Row> out;
	Clock::time_point step_started = Clock::now();
	while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
		Row row;
		row.columns = columns;
		row.values.reserve(ncol);
		for (int i = 0; i < ncol; ++i) {
			row.values.push_back(column_value(stmt.get(), i));
		}
		out.push_back(std::move(row));
		step_started = Clock::now();
	}
	if (rc != SQLITE_DONE) {
		throw sqlite_error(handle_, rc, std::string("query: ") + std::string(sql), step_started);
	}
	return out;
}

int Database::changes() const {
	return sqlite3_changes(handle_);
}

void Database::begin() { exec("BEGIN;"); }
void Database::commit() { exec("COMMIT;"); }
void Database::rollback() { exec("ROLLBACK;"); }

bool Database::in_transaction() const {
	return sqlite3_get_autocommit(handle_) == 0;
}

// ----------------------------------------------------------------------------
// Transaction
// ----------------------------------------------------------------------------

namespace {

// Savepoint names may repeat; RELEASE and ROLLBACK TO act on the innermost
// one of that name, which is this guard's while its scope is the innermost.
constexpr const char *kNestedTransaction = "opennova_tx";

} // namespace

Transaction::Transaction(Database &db) : db_(db), nested_(db.in_transaction()) {
	if (db_.in_read_snapshot_) {
		// Its writes would upgrade the snapshot's read, which never waits on
		// the busy timeout (sqlite.h, Transaction).
		throw SqliteError(SQLITE_MISUSE, "a Transaction inside a ReadSnapshot");
	}
	db_.exec(nested_ ? std::string("SAVEPOINT ") + kNestedTransaction + ";"
	                 : std::string("BEGIN IMMEDIATE;"));
}

Transaction::~Transaction() {
	if (!open_) return;
	try {
		if (nested_) {
			db_.exec(std::string("ROLLBACK TO ") + kNestedTransaction + ";");
			db_.exec(std::string("RELEASE ") + kNestedTransaction + ";");
		} else {
			db_.rollback();
		}
	} catch (const SqliteError &) {
		// sqlite already rolled the transaction back itself (some errors,
		// SQLITE_FULL among them, end it); nothing is left to undo.
	}
}

void Transaction::commit() {
	if (nested_) {
		db_.exec(std::string("RELEASE ") + kNestedTransaction + ";");
	} else {
		db_.commit();
	}
	open_ = false;
}

// ----------------------------------------------------------------------------
// ReadSnapshot
// ----------------------------------------------------------------------------

ReadSnapshot::ReadSnapshot(Database &db) : db_(db), open_(!db.in_transaction()) {
	if (!open_)
		return;
	db_.begin();
	db_.in_read_snapshot_ = true;
}

ReadSnapshot::~ReadSnapshot() {
	if (!open_) return;
	db_.in_read_snapshot_ = false;
	try {
		db_.commit();
	} catch (const SqliteError &) {
		try {
			db_.rollback();
		} catch (const SqliteError &) {
			// Already ended by sqlite itself; nothing is left open.
		}
	}
}

// ----------------------------------------------------------------------------
// ConnectionPool
// ----------------------------------------------------------------------------

namespace {

// True when every connection that opens `path` gets a database of its own:
// ":memory:", an empty path (a private temporary file), a file: URI with an
// empty name (the same), or a file: URI naming an in-memory database
// (":memory:" as the name, or mode=memory) without cache=shared.
bool opens_private_database(const std::string &path) {
	if (path.empty() || path == ":memory:") return true;
	if (path.rfind("file:", 0) != 0) return false;
	const std::size_t query_at = path.find_first_of("?#");
	const std::string name = path.substr(5, query_at == std::string::npos
	                                            ? std::string::npos
	                                            : query_at - 5);
	if (name.empty()) return true;
	bool memory = name == ":memory:";
	bool shared = false;
	if (query_at != std::string::npos && path[query_at] == '?') {
		std::string query = path.substr(query_at + 1);
		query = query.substr(0, query.find('#'));
		std::size_t start = 0;
		for (;;) {
			const std::size_t amp = query.find('&', start);
			const std::string param = query.substr(start, amp == std::string::npos
			                                                   ? std::string::npos
			                                                   : amp - start);
			if (param == "mode=memory") memory = true;
			else if (param == "cache=shared") shared = true;
			else if (param == "cache=private") shared = false;
			if (amp == std::string::npos) break;
			start = amp + 1;
		}
	}
	return memory && !shared;
}

} // namespace

ConnectionPool::Lease::Lease(ConnectionPool &pool, std::unique_ptr<Database> db)
	: pool_(&pool), db_(std::move(db)) {}

ConnectionPool::Lease::~Lease() {
	if (db_) pool_->release(std::move(db_));
}

ConnectionPool::ConnectionPool(std::filesystem::path path, int busy_timeout_ms) :
		path_(std::move(path)), busy_timeout_ms_(busy_timeout_ms) {
	if (opens_private_database(path_.string())) {
		throw SqliteError(SQLITE_MISUSE,
		                  "connection pool over a private database '" + path_.string() +
		                  "' — use file:<name>?mode=memory&cache=shared");
	}
}

ConnectionPool::Lease ConnectionPool::acquire() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (!idle_.empty()) {
			std::unique_ptr<Database> db = std::move(idle_.back());
			idle_.pop_back();
			return Lease(*this, std::move(db));
		}
	}
	// Open outside the lock so a slow open does not stall other threads'
	// acquire().
	return Lease(*this, std::make_unique<Database>(path_, busy_timeout_ms_));
}

void ConnectionPool::release(std::unique_ptr<Database> db) {
	// The next holder never inherits a transaction: one left open (a begin()
	// whose commit an exception skipped) is rolled back, and a connection
	// that cannot roll back is closed rather than pooled.
	if (db->in_transaction()) {
		try {
			db->rollback();
		} catch (const SqliteError &) {
			return;
		}
	}
	std::lock_guard<std::mutex> lock(mu_);
	idle_.push_back(std::move(db));
}

// ----------------------------------------------------------------------------
// Migration runner
// ----------------------------------------------------------------------------

namespace {

std::string read_file(const std::filesystem::path &p) {
	std::ifstream in(p, std::ios::binary);
	if (!in) {
		throw std::runtime_error("could not open " + p.string());
	}
	std::ostringstream os;
	os << in.rdbuf();
	return os.str();
}

} // namespace

MigrationResult run_migrations(Database &db, const std::filesystem::path &migrations_dir) {
	MigrationResult result;

	db.exec(
		"CREATE TABLE IF NOT EXISTS _schema_migrations ("
		"  filename TEXT PRIMARY KEY,"
		"  applied_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
		");"
	);

	if (!std::filesystem::exists(migrations_dir)) {
		return result;
	}

	std::vector<std::filesystem::path> files;
	for (const auto &entry : std::filesystem::directory_iterator(migrations_dir)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".sql") continue;
		files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());

	for (const auto &path : files) {
		const std::string filename = path.filename().string();

		auto rows = db.query(
			"SELECT 1 FROM _schema_migrations WHERE filename = $1;",
			{BindValue{filename}}
		);
		if (!rows.empty()) {
			result.skipped.push_back(filename);
			continue;
		}

		const std::string sql = read_file(path);
		db.begin();
		try {
			db.exec_script(sql);
			db.exec(
				"INSERT INTO _schema_migrations (filename) VALUES ($1);",
				{BindValue{filename}}
			);
			db.commit();
		} catch (...) {
			db.rollback();
			throw;
		}
		result.applied.push_back(filename);
	}

	return result;
}

} // namespace opennova::db
