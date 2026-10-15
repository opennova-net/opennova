#include <net/novaworld/db/database.h>

#include <base/io/log.h>
#include <base/io/sha256.h>

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace opennova::db {

// ----------------------------------------------------------------------------
// Error
// ----------------------------------------------------------------------------

Error::Error(std::string message) : std::runtime_error(std::move(message)) {}

// Class 08 is a connection failure, but for 08P01: a protocol violation (a
// statement given a bind count it does not take) leaves the connection up.
Error::Error(std::string sqlstate, std::string message, bool connection_lost) :
		std::runtime_error(std::move(message)),
		sqlstate_(std::move(sqlstate)),
		connection_lost_(connection_lost ||
				(sqlstate_.compare(0, 2, "08") == 0 && sqlstate_ != "08P01")) {}

namespace {

// The type OIDs the wrapper binds and decodes: the server's built-in types,
// whose OIDs are fixed (pg_type.dat) and never renumbered.
constexpr Oid kBoolOid = 16;
constexpr Oid kByteaOid = 17;
constexpr Oid kInt8Oid = 20;
constexpr Oid kInt2Oid = 21;
constexpr Oid kInt4Oid = 23;
constexpr Oid kFloat4Oid = 700;
constexpr Oid kFloat8Oid = 701;
constexpr Oid kTimestampOid = 1114;
constexpr Oid kTimestamptzOid = 1184;
constexpr Oid kNumericOid = 1700;

// "YYYY-MM-DD HH:MM:SS": a timestamp's text without its fraction or offset.
constexpr std::size_t kTimestampChars = 19;

// Savepoint names may repeat; RELEASE and ROLLBACK TO act on the innermost
// one of that name, which is this guard's while its scope is the innermost.
constexpr const char *kNestedTransaction = "opennova_tx";

struct ResultFree {
	void operator()(PGresult *result) const { PQclear(result); }
};
using Result = std::unique_ptr<PGresult, ResultFree>;

// A libpq message without its trailing newline.
std::string trimmed(const char *message) {
	std::string text = message ? message : "";
	while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
	return text;
}

// "postgres <SQLSTATE> (<message>) — <context>", or without the code when the
// failure carried none.
std::string describe(const std::string &sqlstate, const std::string &message, std::string_view context) {
	std::ostringstream os;
	os << "postgres";
	if (!sqlstate.empty()) os << ' ' << sqlstate;
	os << " (" << message << ")";
	if (!context.empty()) os << " — " << context;
	return os.str();
}

// The error a failed command left: the server's SQLSTATE and primary message
// from its result, else the connection's message (a lost connection).
Error statement_error(PGconn *conn, const PGresult *result, std::string_view sql) {
	const char *state = result ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : nullptr;
	const char *primary = result ? PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY) : nullptr;
	const std::string sqlstate = state ? state : "";
	const std::string message = primary && *primary ? std::string(primary) : trimmed(PQerrorMessage(conn));
	const bool lost = PQstatus(conn) == CONNECTION_BAD;
	return Error(sqlstate, describe(sqlstate, message, sql), lost);
}

// The connection's server notices (a NOTICE, a WARNING) go to the log.
void log_notice(void *, const char *message) {
	const std::string text = trimmed(message);
	const bool warning = text.compare(0, 7, "WARNING") == 0;
	io::logf(warning ? io::LogLevel::kWarn : io::LogLevel::kInfo, "[db] %s", text.c_str());
}

// libpq's view of the binds: text values (NUL-terminated) for everything but
// bytea, which goes binary; a NULL value pointer binds NULL. The text of a
// number or a Bool lives in `text` (reserved up front, so its pointers stay
// put); a string's is the bind's own.
struct Params {
	std::vector<std::string> text;
	std::vector<Oid> types;
	std::vector<const char *> values;
	std::vector<int> lengths;
	std::vector<int> formats;
};

// A float8 bind's text: the shortest that reads back as `value` exactly
// (%.17g), and the server's spellings of the non-finite values.
std::string float_text(double value) {
	if (std::isnan(value)) return "NaN";
	if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.17g", value);
	return buf;
}

Params params_of(const std::vector<BindValue> &binds) {
	// An empty bytea needs a non-NULL pointer (NULL would bind NULL).
	static const char kEmpty = 0;
	Params p;
	p.text.reserve(binds.size());
	p.types.reserve(binds.size());
	p.values.reserve(binds.size());
	p.lengths.reserve(binds.size());
	p.formats.reserve(binds.size());
	for (const BindValue &bind : binds) {
		Oid type = 0;
		const char *value = nullptr;
		int length = 0;
		int format = 0;
		std::visit([&](auto &&v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
				// NULL, typed by its place in the statement.
			} else if constexpr (std::is_same_v<T, int64_t>) {
				type = kInt8Oid;
				p.text.push_back(std::to_string(v));
				value = p.text.back().c_str();
			} else if constexpr (std::is_same_v<T, double>) {
				type = kFloat8Oid;
				p.text.push_back(float_text(v));
				value = p.text.back().c_str();
			} else if constexpr (std::is_same_v<T, std::string>) {
				value = v.c_str();
			} else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
				type = kByteaOid;
				value = v.empty() ? &kEmpty : reinterpret_cast<const char *>(v.data());
				length = static_cast<int>(v.size());
				format = 1;
			} else if constexpr (std::is_same_v<T, Bool>) {
				type = kBoolOid;
				p.text.push_back(v.value ? "t" : "f");
				value = p.text.back().c_str();
			}
		}, bind);
		p.types.push_back(type);
		p.values.push_back(value);
		p.lengths.push_back(length);
		p.formats.push_back(format);
	}
	return p;
}

std::vector<uint8_t> bytea_of(const char *text) {
	std::size_t length = 0;
	unsigned char *raw = PQunescapeBytea(reinterpret_cast<const unsigned char *>(text), &length);
	if (!raw) throw Error("postgres (a bytea value would not decode)");
	std::vector<uint8_t> out(raw, raw + length);
	PQfreemem(raw);
	return out;
}

Value decode(const PGresult *result, int row, int column) {
	if (PQgetisnull(result, row, column)) return std::monostate{};
	const char *text = PQgetvalue(result, row, column);
	const int length = PQgetlength(result, row, column);
	switch (PQftype(result, column)) {
	case kInt2Oid:
	case kInt4Oid:
	case kInt8Oid:
		return static_cast<int64_t>(std::strtoll(text, nullptr, 10));
	case kBoolOid:
		return static_cast<int64_t>(text[0] == 't' ? 1 : 0);
	case kFloat4Oid:
	case kFloat8Oid:
	case kNumericOid:
		return std::strtod(text, nullptr);
	case kByteaOid:
		return bytea_of(text);
	case kTimestampOid:
	case kTimestamptzOid:
		return std::string(text, std::min<std::size_t>(static_cast<std::size_t>(length), kTimestampChars));
	default:
		return std::string(text, static_cast<std::size_t>(length));
	}
}

} // namespace

std::string masked_url(std::string_view url) {
	std::string out(url);
	// A URI's userinfo password: scheme://user:password@host...
	if (const auto scheme = out.find("://"); scheme != std::string::npos) {
		const std::size_t authority = scheme + 3;
		const std::size_t end = out.find_first_of("/?#", authority);
		const std::size_t at = out.rfind('@', end == std::string::npos ? std::string::npos : end);
		if (at != std::string::npos && at >= authority) {
			const std::size_t colon = out.find(':', authority);
			if (colon != std::string::npos && colon < at) out.replace(colon + 1, at - colon - 1, "***");
		}
	}
	// A password parameter, in a URI's query or a key=value conninfo.
	for (std::size_t from = 0;;) {
		const std::size_t key = out.find("password=", from);
		if (key == std::string::npos) break;
		const std::size_t start = key + 9;
		const std::size_t stop = out.find_first_of("& ", start);
		out.replace(start, (stop == std::string::npos ? out.size() : stop) - start, "***");
		from = start + 3;
	}
	return out;
}

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

Database::Database(const ConnectOptions &options) {
	if (options.url.empty()) {
		throw Error("", describe("", "no database URL", "connect"), true);
	}
	// Every connection's session settings, fixed here so none can drift: UTC
	// (a timestamp reads as UTC text), ISO dates, and the timeouts.
	const std::string session = "-c TimeZone=UTC -c DateStyle=ISO -c lock_timeout=" +
			std::to_string(options.lock_timeout_ms) +
			" -c statement_timeout=" + std::to_string(options.statement_timeout_ms) +
			" -c idle_in_transaction_session_timeout=" + std::to_string(kIdleInTransactionTimeoutMs);
	// Keywords are processed in order and a later one wins: the defaults
	// before the URL (dbname, expanded) let it override them; the session
	// settings, the byte-transparent client encoding and the password after
	// it are the service's.
	std::vector<const char *> keys = { "connect_timeout", "keepalives", "keepalives_idle",
		"keepalives_interval", "keepalives_count", "application_name", "dbname", "client_encoding",
		"options" };
	std::vector<const char *> values = { "5", "1", "30", "10", "3",
		options.application_name.c_str(), options.url.c_str(), "SQL_ASCII", session.c_str() };
	if (!options.password.empty()) {
		keys.push_back("password");
		values.push_back(options.password.c_str());
	}
	keys.push_back(nullptr);
	values.push_back(nullptr);
	conn_ = PQconnectdbParams(keys.data(), values.data(), 1);
	if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
		const std::string message = conn_ ? trimmed(PQerrorMessage(conn_)) : "out of memory";
		if (conn_) PQfinish(conn_);
		conn_ = nullptr;
		throw Error("", describe("", message, "connect " + masked_url(options.url)), true);
	}
	PQsetNoticeProcessor(conn_, &log_notice, nullptr);
}

Database::~Database() {
	if (conn_) PQfinish(conn_);
}

Database::Database(Database &&other) noexcept :
		conn_(other.conn_),
		changes_(other.changes_),
		in_transaction_(other.in_transaction_),
		in_read_snapshot_(other.in_read_snapshot_) {
	other.conn_ = nullptr;
	other.in_transaction_ = false;
	other.in_read_snapshot_ = false;
}

Database &Database::operator=(Database &&other) noexcept {
	if (this != &other) {
		if (conn_) PQfinish(conn_);
		conn_ = other.conn_;
		changes_ = other.changes_;
		in_transaction_ = other.in_transaction_;
		in_read_snapshot_ = other.in_read_snapshot_;
		other.conn_ = nullptr;
		other.in_transaction_ = false;
		other.in_read_snapshot_ = false;
	}
	return *this;
}

bool Database::connected() const {
	return conn_ && PQstatus(conn_) == CONNECTION_OK;
}

void Database::note_transaction_state() {
	switch (PQtransactionStatus(conn_)) {
	case PQTRANS_IDLE:
		in_transaction_ = false;
		in_read_snapshot_ = false;
		break;
	case PQTRANS_ACTIVE:
	case PQTRANS_INTRANS:
	case PQTRANS_INERROR:
		in_transaction_ = true;
		break;
	case PQTRANS_UNKNOWN:
		// Lost: keep what the last command left.
		break;
	}
}

void Database::ensure_connected() {
	if (!conn_) throw Error("", describe("", "no connection (moved from)", ""), true);
	if (PQstatus(conn_) == CONNECTION_OK) return;
	if (in_transaction_) {
		// Its transaction is gone with it: nothing may run as if it went on.
		throw Error("", describe("", "the connection was lost inside a transaction", ""), true);
	}
	PQreset(conn_);
	if (PQstatus(conn_) != CONNECTION_OK) {
		throw Error("", describe("", trimmed(PQerrorMessage(conn_)), "reconnect"), true);
	}
	in_read_snapshot_ = false;
	io::logf(io::LogLevel::kWarn, "[db] reconnected after a lost connection");
}

namespace {

// One statement through the extended protocol, which takes one statement
// alone: a second in `sql` fails at the server.
Result run_statement(PGconn *conn, std::string_view sql, const std::vector<BindValue> &binds) {
	const std::string text(sql);
	const Params p = params_of(binds);
	return Result(PQexecParams(conn, text.c_str(), static_cast<int>(binds.size()),
			p.types.data(), p.values.data(), p.lengths.data(), p.formats.data(), 0));
}

int changes_of(const PGresult *result) {
	const char *count = PQcmdTuples(const_cast<PGresult *>(result));
	return count && *count ? std::atoi(count) : 0;
}

} // namespace

void Database::exec(std::string_view sql) {
	exec(sql, {});
}

void Database::exec(std::string_view sql, const std::vector<BindValue> &binds) {
	ensure_connected();
	const Result result = run_statement(conn_, sql, binds);
	note_transaction_state();
	const ExecStatusType status = result ? PQresultStatus(result.get()) : PGRES_FATAL_ERROR;
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
		throw statement_error(conn_, result.get(), sql);
	}
	changes_ = changes_of(result.get());
}

void Database::exec_script(std::string_view sql_script) {
	ensure_connected();
	const std::string text(sql_script);
	const Result result(PQexec(conn_, text.c_str()));
	note_transaction_state();
	const ExecStatusType status = result ? PQresultStatus(result.get()) : PGRES_FATAL_ERROR;
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK && status != PGRES_EMPTY_QUERY) {
		throw statement_error(conn_, result.get(), "exec_script");
	}
	changes_ = 0;
}

std::vector<Row> Database::query(std::string_view sql) {
	return query(sql, {});
}

std::vector<Row> Database::query(std::string_view sql, const std::vector<BindValue> &binds) {
	ensure_connected();
	const Result result = run_statement(conn_, sql, binds);
	note_transaction_state();
	const ExecStatusType status = result ? PQresultStatus(result.get()) : PGRES_FATAL_ERROR;
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
		throw statement_error(conn_, result.get(), sql);
	}
	changes_ = changes_of(result.get());
	const int ncol = PQnfields(result.get());
	std::vector<std::string> columns;
	columns.reserve(static_cast<std::size_t>(ncol));
	for (int c = 0; c < ncol; ++c) {
		const char *name = PQfname(result.get(), c);
		columns.emplace_back(name ? name : "");
	}
	const int nrow = PQntuples(result.get());
	std::vector<Row> out;
	out.reserve(static_cast<std::size_t>(nrow));
	for (int r = 0; r < nrow; ++r) {
		Row row;
		row.columns = columns;
		row.values.reserve(static_cast<std::size_t>(ncol));
		for (int c = 0; c < ncol; ++c) row.values.push_back(decode(result.get(), r, c));
		out.push_back(std::move(row));
	}
	return out;
}

int Database::changes() const {
	return changes_;
}

void Database::begin() { exec("BEGIN;"); }
void Database::commit() { exec("COMMIT;"); }

void Database::rollback() {
	if (conn_ && PQstatus(conn_) != CONNECTION_OK) {
		// The server ended the transaction with the connection; the next
		// statement reconnects.
		in_transaction_ = false;
		in_read_snapshot_ = false;
		throw Error("", describe("", "the connection was lost; its transaction is gone", "ROLLBACK"), true);
	}
	exec("ROLLBACK;");
}

bool Database::in_transaction() const {
	return in_transaction_;
}

// ----------------------------------------------------------------------------
// Transaction
// ----------------------------------------------------------------------------

Transaction::Transaction(Database &db) : db_(db), nested_(db.in_transaction()) {
	if (db_.in_read_snapshot_) {
		// Its transaction is read-only: a write in it would fail (database.h,
		// Transaction).
		throw Error(describe("", "a Transaction inside a ReadSnapshot", ""));
	}
	if (nested_) {
		db_.exec(std::string("SAVEPOINT ") + kNestedTransaction + ";");
		return;
	}
	db_.begin();
	try {
		db_.exec("SELECT pg_advisory_xact_lock($1);", { BindValue(kWriterLock) });
	} catch (...) {
		try {
			db_.rollback();
		} catch (const Error &) {
			// Lost with the connection; nothing is left open.
		}
		throw;
	}
}

Transaction::~Transaction() {
	if (!open_) return;
	try {
		if (nested_) {
			db_.exec(std::string("ROLLBACK TO SAVEPOINT ") + kNestedTransaction + ";");
			db_.exec(std::string("RELEASE SAVEPOINT ") + kNestedTransaction + ";");
		} else {
			db_.rollback();
		}
	} catch (const Error &) {
		// The connection is gone, and its transaction with it; nothing is left
		// to undo.
	}
}

void Transaction::commit() {
	if (nested_) {
		db_.exec(std::string("RELEASE SAVEPOINT ") + kNestedTransaction + ";");
		open_ = false;
		return;
	}
	if (db_.connected() && PQtransactionStatus(db_.conn_) == PQTRANS_INERROR) {
		// A failed statement aborted it: the server would turn this COMMIT
		// into a ROLLBACK without an error. Say so instead.
		open_ = false;
		try {
			db_.rollback();
		} catch (const Error &) {
		}
		throw Error("25P02",
				describe("25P02", "a statement in this transaction failed; it was rolled back", "COMMIT"));
	}
	db_.commit();
	open_ = false;
}

// ----------------------------------------------------------------------------
// ReadSnapshot
// ----------------------------------------------------------------------------

ReadSnapshot::ReadSnapshot(Database &db) : db_(db), open_(!db.in_transaction()) {
	if (!open_) return;
	db_.exec("BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY;");
	db_.in_read_snapshot_ = true;
}

ReadSnapshot::~ReadSnapshot() {
	if (!open_) return;
	db_.in_read_snapshot_ = false;
	try {
		db_.rollback(); // nothing to commit: it read
	} catch (const Error &) {
		// Lost with the connection; nothing is left open.
	}
}

// ----------------------------------------------------------------------------
// SessionLock
// ----------------------------------------------------------------------------

SessionLock::SessionLock(Database &db, int64_t key) : db_(db), key_(key) {
	db_.exec("SELECT pg_advisory_lock($1);", { BindValue(key_) });
}

SessionLock::~SessionLock() {
	try {
		db_.exec("SELECT pg_advisory_unlock($1);", { BindValue(key_) });
	} catch (const Error &) {
		// Lost with the connection, which released it.
	}
}

// ----------------------------------------------------------------------------
// ConnectionPool
// ----------------------------------------------------------------------------

ConnectionPool::Lease::Lease(ConnectionPool &pool, std::unique_ptr<Database> db)
	: pool_(&pool), db_(std::move(db)) {}

ConnectionPool::Lease::~Lease() {
	if (db_) pool_->release(std::move(db_));
}

ConnectionPool::ConnectionPool(ConnectOptions options, std::size_t max) :
		options_(std::move(options)), max_(max == 0 ? 1 : max) {}

ConnectionPool::Lease ConnectionPool::acquire() {
	{
		std::unique_lock<std::mutex> lock(mu_);
		const auto deadline =
				std::chrono::steady_clock::now() + std::chrono::milliseconds(kPoolAcquireTimeoutMs);
		while (idle_.empty() && open_ >= max_) {
			if (returned_.wait_until(lock, deadline) == std::cv_status::timeout && idle_.empty() &&
					open_ >= max_) {
				throw Error(describe("",
						"every one of the pool's " + std::to_string(max_) + " connections stayed leased for " +
								std::to_string(kPoolAcquireTimeoutMs / 1000) + " s",
						"acquire"));
			}
		}
		if (!idle_.empty()) {
			std::unique_ptr<Database> db = std::move(idle_.back());
			idle_.pop_back();
			return Lease(*this, std::move(db));
		}
		++open_;
	}
	// Open outside the lock so a slow connect does not stall other threads'
	// acquire().
	try {
		return Lease(*this, std::make_unique<Database>(options_));
	} catch (...) {
		std::lock_guard<std::mutex> lock(mu_);
		--open_;
		returned_.notify_one();
		throw;
	}
}

void ConnectionPool::release(std::unique_ptr<Database> db) {
	// The next holder never inherits a transaction: one left open (a begin()
	// whose commit an exception skipped) is rolled back, and a connection
	// that cannot roll back, or was lost, is closed rather than pooled.
	bool keep = db->connected();
	if (keep && db->in_transaction()) {
		try {
			db->rollback();
		} catch (const Error &) {
			keep = false;
		}
	}
	if (!keep) db.reset();
	std::lock_guard<std::mutex> lock(mu_);
	if (keep) {
		idle_.push_back(std::move(db));
	} else {
		--open_;
	}
	returned_.notify_one();
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

// The database's contract: byte-transparent text (SQL_ASCII, as SQLite kept
// a retail client's cp1252 bytes) in byte order (C collation, with ASCII-only
// lower()). Created any other way, the service would refuse a wire string a
// stock client sends or sort and compare differently.
void check_database(Database &db) {
	const auto rows = db.query("SELECT pg_encoding_to_char(encoding), datcollate, datctype "
							   "FROM pg_database WHERE datname = current_database();");
	if (rows.empty()) throw Error(describe("", "the current database is not in pg_database", ""));
	const std::string encoding = rows.front().as_text(0).value_or("");
	const std::string collate = rows.front().as_text(1).value_or("");
	const std::string ctype = rows.front().as_text(2).value_or("");
	auto is_c = [](const std::string &locale) { return locale == "C" || locale == "POSIX"; };
	if (encoding != "SQL_ASCII" || !is_c(collate) || !is_c(ctype)) {
		throw Error(describe("",
				"the database is " + encoding + ", collation " + collate + ", ctype " + ctype +
						"; the service needs CREATE DATABASE <name> TEMPLATE template0 ENCODING "
						"'SQL_ASCII' LC_COLLATE 'C' LC_CTYPE 'C'",
				"run_migrations"));
	}
}

} // namespace

MigrationResult run_migrations(Database &db, const std::filesystem::path &migrations_dir) {
	MigrationResult result;
	const SessionLock lock(db, kMigrationLock);
	check_database(db);

	if (db.query("SELECT to_regclass('_schema_migrations') IS NULL;").front().as_int(0).value_or(1) != 0) {
		db.exec("CREATE TABLE _schema_migrations ("
				"  filename   text PRIMARY KEY,"
				"  sha256     text NOT NULL,"
				"  applied_at timestamptz NOT NULL DEFAULT now()"
				");");
	}

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

	std::map<std::string, std::string> recorded;
	for (const Row &row : db.query("SELECT filename, sha256 FROM _schema_migrations;")) {
		recorded[row.as_text(0).value_or("")] = row.as_text(1).value_or("");
	}

	for (const auto &path : files) {
		const std::string filename = path.filename().string();
		const std::string sql = read_file(path);
		const std::string hash = io::sha256_hex(io::sha256(sql));
		if (const auto it = recorded.find(filename); it != recorded.end()) {
			if (it->second != hash) {
				throw Error(describe("",
						"migration " + filename + " changed since it was applied (recorded sha256 " +
								it->second + ", the file's " + hash +
								"); migrations are append-only, a fix is a new file",
						"run_migrations"));
			}
			result.skipped.push_back(filename);
			continue;
		}
		db.begin();
		try {
			db.exec_script(sql);
			db.exec("INSERT INTO _schema_migrations (filename, sha256) VALUES ($1, $2);",
					{ BindValue{ filename }, BindValue{ hash } });
			db.commit();
		} catch (...) {
			try {
				db.rollback();
			} catch (const Error &) {
				// Lost with the connection; the file's transaction is gone.
			}
			throw;
		}
		result.applied.push_back(filename);
	}

	return result;
}

} // namespace opennova::db
