#include <novaworld/db/sqlite.h>

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace opennova::db {

// ----------------------------------------------------------------------------
// SqliteError
// ----------------------------------------------------------------------------

SqliteError::SqliteError(int code, std::string message)
	: std::runtime_error(std::move(message)), code_(code) {}

namespace {

[[noreturn]] void throw_sqlite(sqlite3 *handle, int rc, std::string_view ctx) {
	std::ostringstream os;
	os << "sqlite error " << rc;
	if (handle) {
		const char *msg = sqlite3_errmsg(handle);
		if (msg && *msg) {
			os << " (" << msg << ")";
		}
	}
	if (!ctx.empty()) {
		os << " — " << ctx;
	}
	throw SqliteError(rc, os.str());
}

void bind_at(sqlite3_stmt *stmt, int i, const BindValue &v) {
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

std::optional<double> Row::as_real(std::size_t i) const {
	if (i >= values.size()) return std::nullopt;
	if (auto *v = std::get_if<double>(&values[i])) return *v;
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

Database::Database(const std::filesystem::path &path) {
	const std::string utf8 = path.string();
	const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI;
	int rc = sqlite3_open_v2(utf8.c_str(), &handle_, flags, nullptr);
	if (rc != SQLITE_OK) {
		// `handle_` may be non-null even on open failure — sqlite3_close_v2
		// is the documented cleanup.
		sqlite3 *bad = handle_;
		handle_ = nullptr;
		std::string ctx = "open " + utf8;
		if (bad) {
			std::string msg = std::string("sqlite open failed: ") + sqlite3_errmsg(bad);
			sqlite3_close_v2(bad);
			throw SqliteError(rc, msg + " — " + ctx);
		}
		throw SqliteError(rc, "sqlite open failed — " + ctx);
	}
	// Useful pragmas for an embedded server.
	sqlite3_busy_timeout(handle_, /*ms*/ 5000);
	sqlite3_exec(handle_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
	sqlite3_exec(handle_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
}

Database::~Database() {
	if (handle_) {
		sqlite3_close_v2(handle_);
	}
}

Database::Database(Database &&other) noexcept : handle_(other.handle_) {
	other.handle_ = nullptr;
}

Database &Database::operator=(Database &&other) noexcept {
	if (this != &other) {
		if (handle_) sqlite3_close_v2(handle_);
		handle_ = other.handle_;
		other.handle_ = nullptr;
	}
	return *this;
}

void Database::exec(std::string_view sql) {
	exec(sql, {});
}

void Database::exec(std::string_view sql, const std::vector<BindValue> &binds) {
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
	if (rc != SQLITE_OK) {
		throw_sqlite(handle_, rc, std::string("prepare: ") + std::string(sql));
	}
	for (std::size_t i = 0; i < binds.size(); ++i) {
		bind_at(stmt, static_cast<int>(i + 1), binds[i]);
	}
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
		throw_sqlite(handle_, rc, std::string("exec: ") + std::string(sql));
	}
}

void Database::exec_script(std::string_view sql_script) {
	char *err = nullptr;
	const std::string copy(sql_script); // sqlite3_exec needs NUL-terminated
	int rc = sqlite3_exec(handle_, copy.c_str(), nullptr, nullptr, &err);
	if (rc != SQLITE_OK) {
		std::string msg = err ? err : "(no message)";
		sqlite3_free(err);
		throw SqliteError(rc, "exec_script: " + msg);
	}
}

std::vector<Row> Database::query(std::string_view sql) {
	return query(sql, {});
}

std::vector<Row> Database::query(std::string_view sql, const std::vector<BindValue> &binds) {
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(handle_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
	if (rc != SQLITE_OK) {
		throw_sqlite(handle_, rc, std::string("prepare: ") + std::string(sql));
	}
	for (std::size_t i = 0; i < binds.size(); ++i) {
		bind_at(stmt, static_cast<int>(i + 1), binds[i]);
	}

	const int ncol = sqlite3_column_count(stmt);
	std::vector<std::string> columns;
	columns.reserve(ncol);
	for (int i = 0; i < ncol; ++i) {
		const char *name = sqlite3_column_name(stmt, i);
		columns.emplace_back(name ? name : "");
	}

	std::vector<Row> out;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		Row row;
		row.columns = columns;
		row.values.reserve(ncol);
		for (int i = 0; i < ncol; ++i) {
			row.values.push_back(column_value(stmt, i));
		}
		out.push_back(std::move(row));
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		throw_sqlite(handle_, rc, std::string("query: ") + std::string(sql));
	}
	return out;
}

int64_t Database::last_insert_rowid() const {
	return sqlite3_last_insert_rowid(handle_);
}

int Database::changes() const {
	return sqlite3_changes(handle_);
}

void Database::begin() { exec("BEGIN;"); }
void Database::commit() { exec("COMMIT;"); }
void Database::rollback() { exec("ROLLBACK;"); }

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
			"SELECT 1 FROM _schema_migrations WHERE filename = ?;",
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
				"INSERT INTO _schema_migrations (filename) VALUES (?);",
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
