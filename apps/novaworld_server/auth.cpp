#include "auth.h"

#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <pycabcrypt.h>
// Forward-declared in pycabcrypt.h.
//   bcrypt_hashpass(const char *key, const char *salt, char *out, size_t len);
//   int encode_base64(char *b64, const u_int8_t *raw, size_t len);
//   timingsafe_bcmp(const void *a, const void *b, size_t n);
}
// pycabcrypt.h #define's snprintf to _snprintf for MSVC, which collides
// with std::snprintf. Drop the macro so the rest of this TU can use
// std-namespace cstdio normally.
#ifdef snprintf
#undef snprintf
#endif

namespace opennova::novaworld_server {

namespace {

// password_verifications(): every bcrypt check verify_password runs.
std::atomic<uint64_t> g_password_verifications{0};

// set_password_accepted_hook(): the test seam authenticate_user calls.
std::mutex g_password_accepted_hook_mu;
std::function<void(int64_t)> g_password_accepted_hook;

// The columns row_to_user reads, in its order.
constexpr const char *kUserColumns =
		"id, username, pcid, nwh, nwhandle, account_status, role, credentials_gen";

// Verify a password against a bcrypt hash. The stored hash format is
//     $2a$<cost>$<22-char-salt><31-char-hash>     (or $2b$ — same algo)
// We hand `bcrypt_hashpass` the password + the stored hash (which doubles
// as the salt source), and compare the recomputed hash byte-for-byte
// against the stored hash with timingsafe_bcmp.
//
// Falls back to plain-string compare for stored_hash values that don't
// look like a bcrypt hash — covers the dev seed where 'test'/'foo' are
// stored as plaintext until you regenerate the seed (via the admin
// POST /api/admin/users path). This path is logged so
// it's obvious when plaintext credentials are still in use.
bool verify_password(const std::string &plain, const std::string &stored) {
	if (stored.size() >= 4 && stored[0] == '$' && stored[1] == '2' &&
	    (stored[2] == 'a' || stored[2] == 'b' || stored[2] == 'y') &&
	    stored[3] == '$') {
		g_password_verifications.fetch_add(1, std::memory_order_relaxed);
		char recomputed[64] = {0};
		if (bcrypt_hashpass(plain.c_str(), stored.c_str(),
		                    recomputed, sizeof(recomputed)) != 0) {
			return false;
		}
		// timingsafe_bcmp returns 0 on equal.
		return timingsafe_bcmp(recomputed, stored.c_str(),
		                      stored.size()) == 0;
	}
	std::fprintf(stderr, "[auth] WARN plaintext password_hash in DB — "
	                     "regenerate with bcrypt before deploy\n");
	if (plain.size() != stored.size()) return false;
	unsigned diff = 0;
	for (size_t i = 0; i < plain.size(); ++i) {
		diff |= static_cast<unsigned>(plain[i]) ^ static_cast<unsigned>(stored[i]);
	}
	return diff == 0;
}

std::optional<UserRecord> row_to_user(const opennova::db::Row &row) {
	UserRecord u;
	u.id       = row.as_int(0).value_or(0);
	u.username = row.as_text(1).value_or("");
	u.pcid     = row.as_text(2).value_or("");
	u.nwh      = row.as_text(3).value_or("");
	u.nwhandle = row.as_text(4).value_or("");
	// The schema refuses an empty status (migration 0003), so 'active' alone
	// is active, here and in every query that tests it.
	u.account_status = row.as_text(5).value_or("");
	u.role     = row.as_text(6).value_or("player");
	u.credentials_gen = row.as_int(7).value_or(0);
	if (u.id == 0) return std::nullopt;
	return u;
}

// The hash an unknown username's password is checked against, so an unknown
// name costs the bcrypt run a wrong password does: the reply's timing does not
// enumerate accounts. Random bytes hashed at hash_password's cost, once per
// process; no password matches it.
const std::string &unknown_user_hash() {
	static const std::string hash = [] {
		std::array<uint8_t, 16> raw{};
		os_random_bytes(raw.data(), raw.size());
		return hash_password(strutil::bytes_to_hex(raw.data(), raw.size()));
	}();
	return hash;
}

MutationResult err(const char *code, const char *msg) {
	MutationResult m;
	m.ok = false;
	m.error_code = code;
	m.error_message = msg;
	return m;
}

// A "db_error" result, logged here with the operation that failed: the
// routes hand the result to the caller, so without this the failure (a
// SqliteError's extended code and wait among it) never reaches the log. The
// result carries the code alone, with no message: the routes send the message
// to the client, and what() names the failing statement.
MutationResult db_err(const char *op, const std::exception &e) {
	std::fprintf(stderr, "[auth] WARN %s failed: %s\n", op, e.what());
	return err("db_error", "");
}

} // namespace

uint64_t password_verifications() {
	return g_password_verifications.load(std::memory_order_relaxed);
}

void set_password_accepted_hook(std::function<void(int64_t user_id)> hook) {
	std::lock_guard<std::mutex> lock(g_password_accepted_hook_mu);
	g_password_accepted_hook = std::move(hook);
}

std::optional<UserRecord> authenticate_user(opennova::db::Database &db,
                                            const std::string &username,
                                            const std::string &password) {
	if (username.empty()) return std::nullopt;
	std::vector<opennova::db::Row> rows;
	try {
		// The generation comes from the same row as the hash: what the caller
		// grants on it is checked against the account's generation then.
		rows = db.query(std::string("SELECT ") + kUserColumns +
						", password_hash "
						"FROM players WHERE username = ? LIMIT 1;",
				{ opennova::db::BindValue(username) });
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN player lookup failed: %s\n", e.what());
		return std::nullopt;
	}
	if (rows.empty()) {
		try {
			verify_password(password, unknown_user_hash());
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[auth] WARN unknown-user hash failed: %s\n", e.what());
		}
		std::fprintf(stderr, "[auth] user '%s' not found\n", loggable(username).c_str());
		return std::nullopt;
	}
	const auto stored_hash = rows.front().as_text(8).value_or("");
	if (!verify_password(password, stored_hash)) {
		std::fprintf(stderr, "[auth] bad password for user '%s'\n", loggable(username).c_str());
		return std::nullopt;
	}
	auto user = row_to_user(rows.front());
	if (user) {
		// Update last_login (best-effort; auth still succeeds if this fails).
		try {
			db.exec("UPDATE players SET last_login = CURRENT_TIMESTAMP WHERE id = ?;",
			        {opennova::db::BindValue(user->id)});
		} catch (const opennova::db::SqliteError &e) {
			std::fprintf(stderr, "[auth] WARN last_login update failed: %s\n", e.what());
		}
		std::function<void(int64_t)> hook;
		{
			std::lock_guard<std::mutex> lock(g_password_accepted_hook_mu);
			hook = g_password_accepted_hook;
		}
		if (hook)
			hook(user->id);
	}
	return user;
}

std::optional<UserRecord> get_user_by_username(opennova::db::Database &db,
                                                const std::string &username) {
	if (username.empty()) return std::nullopt;
	try {
		auto rows = db.query(std::string("SELECT ") + kUserColumns +
						" FROM players WHERE username = ? OR nwhandle = ? LIMIT 1;",
				{ opennova::db::BindValue(username), opennova::db::BindValue(username) });
		if (rows.empty()) return std::nullopt;
		return row_to_user(rows.front());
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN user lookup by name failed: %s\n", e.what());
		return std::nullopt;
	}
}

std::optional<UserRecord> get_user_by_id(opennova::db::Database &db, int64_t id) {
	if (id == 0) return std::nullopt;
	try {
		auto rows = db.query(
				std::string("SELECT ") + kUserColumns + " FROM players WHERE id = ? LIMIT 1;",
				{ opennova::db::BindValue(id) });
		if (rows.empty()) return std::nullopt;
		return row_to_user(rows.front());
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN user lookup by id failed: %s\n", e.what());
		return std::nullopt;
	}
}

std::optional<UserRecord> get_user_by_login_session_tag(
		opennova::db::Database &db, const std::string &session_tag) {
	if (session_tag.empty())
		return std::nullopt;
	auto rows = db.query(
			"SELECT p.id, p.username, p.pcid, p.nwh, p.nwhandle, p.account_status, p.role, "
			"       p.credentials_gen "
			"FROM active_user_sessions s JOIN players p ON p.id = s.user_id "
			"WHERE s.session_tag = ? AND s.credentials_gen = p.credentials_gen LIMIT 1;",
			{ opennova::db::BindValue(session_tag) });
	if (rows.empty())
		return std::nullopt;
	return row_to_user(rows.front());
}

std::vector<UserRecord> list_users(opennova::db::Database &db) {
	std::vector<UserRecord> out;
	try {
		auto rows = db.query(std::string("SELECT ") + kUserColumns + " FROM players ORDER BY id;");
		out.reserve(rows.size());
		for (const auto &r : rows) {
			if (auto u = row_to_user(r)) out.push_back(*u);
		}
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN list_users failed: %s\n", e.what());
	}
	return out;
}

std::optional<GameAccessRecord> get_game_access(opennova::db::Database &db,
                                                int64_t user_id,
                                                const std::string &game_slug) {
	if (user_id == 0 || game_slug.empty()) return std::nullopt;
	try {
		auto rows = db.query(
			"SELECT user_id, game_slug, status, exp_bits "
			"FROM player_game_access "
			"WHERE user_id = ? AND game_slug = ? LIMIT 1;",
			{opennova::db::BindValue(user_id), opennova::db::BindValue(game_slug)});
		if (!rows.empty()) {
			GameAccessRecord a;
			a.id = rows.front().as_int(0).value_or(0);
			a.game_slug = rows.front().as_text(1).value_or(game_slug);
			a.status = rows.front().as_text(2).value_or("active");
			a.exp_bits = rows.front().as_text(3).value_or("");
			return a;
		}
		rows = db.query(
			"SELECT slug, exp_bits FROM games WHERE slug = ? LIMIT 1;",
			{opennova::db::BindValue(game_slug)});
		if (rows.empty()) return std::nullopt;
		GameAccessRecord a;
		a.id = user_id;
		a.game_slug = rows.front().as_text(0).value_or(game_slug);
		a.status = "active";
		a.exp_bits = rows.front().as_text(1).value_or("");
		return a;
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN get_game_access failed: %s\n", e.what());
		return std::nullopt;
	}
}

ServerStatusRecord get_server_status(opennova::db::Database &db) {
	ServerStatusRecord s;
	s.message = "NovaWorld is temporarily unavailable.";
	try {
		auto rows = db.query(
			"SELECT maintenance_enabled, message "
			"FROM server_status WHERE id = 1 LIMIT 1;");
		if (!rows.empty()) {
			s.maintenance_enabled = rows.front().as_int(0).value_or(0) != 0;
			s.message = rows.front().as_text(1).value_or(s.message);
		}
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN server_status lookup failed: %s\n", e.what());
	}
	return s;
}

MutationResult update_server_status(opennova::db::Database &db,
                                    bool maintenance_enabled,
                                    const std::string &message) {
	const std::string msg = message.empty()
		? std::string("NovaWorld is temporarily unavailable.")
		: message;
	try {
		db.exec(
			"INSERT INTO server_status (id, maintenance_enabled, message, updated_at) "
			"VALUES (1, ?, ?, CURRENT_TIMESTAMP) "
			"ON CONFLICT(id) DO UPDATE SET "
			"maintenance_enabled=excluded.maintenance_enabled, "
			"message=excluded.message, updated_at=CURRENT_TIMESTAMP;",
			{opennova::db::BindValue(static_cast<int64_t>(maintenance_enabled ? 1 : 0)),
			 opennova::db::BindValue(msg)});
		MutationResult m;
		m.ok = true;
		m.id = 1;
		return m;
	} catch (const opennova::db::SqliteError &e) {
		return db_err("update_server_status", e);
	}
}

bool register_active_user_session(opennova::db::Database &db, int64_t user_id,
		int64_t credentials_gen, const std::string &username, const std::string &session_tag,
		const std::string &persistent_id, const std::string &remote_ip,
		const std::string &user_agent) {
	// One statement: the generation and status it tests are the ones the row
	// is written against.
	db.exec("INSERT OR REPLACE INTO active_user_sessions "
			"(user_id, username, session_tag, persistent_id, remote_ip, user_agent, created_at, "
			"last_seen_at, credentials_gen) "
			"SELECT id, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, credentials_gen "
			"FROM players WHERE id = ? AND credentials_gen = ? AND account_status = 'active';",
			{ opennova::db::BindValue(username), opennova::db::BindValue(session_tag),
					opennova::db::BindValue(persistent_id), opennova::db::BindValue(remote_ip),
					opennova::db::BindValue(user_agent), opennova::db::BindValue(user_id),
					opennova::db::BindValue(credentials_gen) });
	return db.changes() > 0;
}

void touch_active_user_session(opennova::db::Database &db, int64_t user_id) {
	if (user_id == 0) return;
	db.exec("UPDATE active_user_sessions SET last_seen_at = CURRENT_TIMESTAMP WHERE user_id = ?;",
	        {opennova::db::BindValue(user_id)});
}

void clear_active_user_session(opennova::db::Database &db, int64_t user_id) {
	if (user_id == 0) return;
	db.exec("DELETE FROM active_user_sessions WHERE user_id = ?;",
	        {opennova::db::BindValue(user_id)});
}

void clear_active_user_session_by_tag(opennova::db::Database &db,
                                      const std::string &session_tag) {
	if (session_tag.empty()) return;
	db.exec("DELETE FROM active_user_sessions WHERE session_tag = ?;",
	        {opennova::db::BindValue(session_tag)});
}

void clear_all_active_user_sessions(opennova::db::Database &db) {
	db.exec("DELETE FROM active_user_sessions;");
}

std::size_t evict_active_user_sessions_older_than(opennova::db::Database &db,
                                                  int max_age_seconds) {
	if (max_age_seconds < 1) max_age_seconds = 3600;
	db.exec(
		"DELETE FROM active_user_sessions "
		"WHERE strftime('%s','now') - strftime('%s', last_seen_at) > ?;",
		{opennova::db::BindValue(static_cast<int64_t>(max_age_seconds))});
	return static_cast<std::size_t>(db.changes());
}

std::string hash_password(const std::string &plain, int cost) {
	if (cost < 4 || cost > 31) cost = 10;
	// 16 bytes from the OS CSPRNG (base/os_random) → bcrypt's modified-base64
	// → assembled as $2b$<cost>$<22-char-salt> for hand-off to bcrypt_hashpass.
	std::array<uint8_t, 16> raw{};
	os_random_bytes(raw.data(), raw.size());
	char salt_b64[64] = {0};
	if (encode_base64(salt_b64, raw.data(), raw.size()) != 0) {
		throw std::runtime_error("hash_password: encode_base64 failed");
	}
	char salt_full[64];
	std::snprintf(salt_full, sizeof(salt_full), "$2b$%02d$%s", cost, salt_b64);
	char out[64] = {0};
	if (bcrypt_hashpass(plain.c_str(), salt_full, out, sizeof(out)) != 0) {
		throw std::runtime_error("hash_password: bcrypt_hashpass failed");
	}
	return std::string(out);
}

namespace {

// The username rule: not empty (no login names it), at most kUsernameMaxBytes
// bytes (POST /api/login takes no longer one, so a longer account could never
// log in to the site) and no control byte (a name is logged and shown in the
// retail browser and chat).
std::optional<MutationResult> username_refusal(const std::string &username) {
	if (username.empty()) {
		return err("invalid_field", "username must not be empty");
	}
	if (username.size() > kUsernameMaxBytes) {
		return err("invalid_field", "username is longer than 64 bytes");
	}
	if (has_control_byte(username)) {
		return err("invalid_field", "username holds a control byte");
	}
	return std::nullopt;
}

// The handle rule: not empty, at most kHandleMaxBytes bytes (the longest a
// stock host reads back whole) and no control byte (a handle is the player's
// name in the game, its chat and the server browser, and goes out in cookies).
std::optional<MutationResult> handle_refusal(const std::string &nwhandle) {
	if (nwhandle.empty()) {
		return err("invalid_field", "nwhandle must not be empty");
	}
	if (nwhandle.size() > kHandleMaxBytes) {
		return err("invalid_field", "nwhandle is longer than 30 bytes");
	}
	if (has_control_byte(nwhandle)) {
		return err("invalid_field", "nwhandle holds a control byte");
	}
	return std::nullopt;
}

} // namespace

MutationResult create_user(opennova::db::Database &db, const CreateUserParams &p) {
	if (p.username.empty() || p.password.empty() || p.pcid.empty() ||
	    p.nwhandle.empty()) {
		return err("missing_field",
		           "username, password, pcid, nwhandle are required");
	}
	if (auto refused = username_refusal(p.username)) return *refused;
	if (auto refused = handle_refusal(p.nwhandle))
		return *refused;
	std::string nwh = p.nwh.empty() ? std::string("1") : p.nwh;

	// Detect duplicate-by-username / duplicate-by-pcid so we can return a
	// useful error code instead of sqlite's UNIQUE constraint violation text.
	auto duplicate = [&db, &p]() -> std::optional<MutationResult> {
		if (get_user_by_username(db, p.username)) {
			return err("username_exists", "username already taken");
		}
		auto rows = db.query(
			"SELECT id FROM players WHERE pcid = ? LIMIT 1;",
			{opennova::db::BindValue(p.pcid)});
		if (!rows.empty()) return err("pcid_exists", "PCID already in use");
		return std::nullopt;
	};

	// Checked once before the hash, so a taken name or PCID (and each of
	// /api/register's PCID retries) costs no bcrypt run.
	try {
		if (auto taken = duplicate()) return *taken;
	} catch (const opennova::db::SqliteError &e) {
		return db_err("create_user", e);
	}

	// Hash before the transaction below: bcrypt is slow, and the write lock
	// the transaction holds stalls every other writer.
	std::string hashed;
	try {
		hashed = hash_password(p.password);
	} catch (const std::exception &e) {
		return db_err("create_user password hash", e);
	}

	// Checked again inside one transaction with the insert and the id read:
	// a registration of the same username or PCID that raced past the first
	// check still gets its error code, and last_insert_rowid() is this
	// insert's.
	try {
		opennova::db::Transaction tx(db);
		if (auto taken = duplicate()) return *taken;
		db.exec(
			"INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
			"VALUES (?, ?, ?, ?, ?);",
			{opennova::db::BindValue(p.username),
			 opennova::db::BindValue(hashed),
			 opennova::db::BindValue(p.pcid),
			 opennova::db::BindValue(nwh),
			 opennova::db::BindValue(p.nwhandle)});
		MutationResult m;
		m.ok = true;
		m.id = db.last_insert_rowid();
		tx.commit();
		return m;
	} catch (const opennova::db::SqliteError &e) {
		return db_err("create_user", e);
	}
}

MutationResult delete_user(opennova::db::Database &db, int64_t id) {
	if (id <= 0) return err("not_found", "invalid id");
	try {
		db.exec("DELETE FROM players WHERE id = ?;",
		        {opennova::db::BindValue(id)});
		if (db.changes() == 0) return err("not_found", "no such user");
	} catch (const opennova::db::SqliteError &e) {
		return db_err("delete_user", e);
	}
	MutationResult m;
	m.ok = true;
	m.id = id;
	return m;
}

MutationResult update_user(opennova::db::Database &db, int64_t id,
                           const UpdateUserParams &p) {
	if (id <= 0) return err("not_found", "invalid id");
	if (!get_user_by_id(db, id)) return err("not_found", "no such user");
	if (p.role && *p.role != "player" && *p.role != "admin") {
		return err("invalid_field", "role must be player or admin");
	}
	// A status names itself: the schema refuses '' too (migration 0003), and
	// this answers it as the field's own error rather than a db_error.
	if (p.account_status && p.account_status->empty()) {
		return err("invalid_field", "account_status must not be empty");
	}
	if (p.nwhandle) {
		if (auto refused = handle_refusal(*p.nwhandle))
			return *refused;
	}
	if (p.username) {
		if (auto refused = username_refusal(*p.username)) return *refused;
	}
	// A new password or an inactive status ends the account's website
	// sessions, its retail login's session row and its hosting keys, in the
	// same transaction as the update: a reset locks out whoever held the old
	// one, and a ban lifted later revives no session. A server the account has
	// listed stays listed; its next refresh stores no owner (the claim went
	// with the keys), its next re-host links to no one (refused under
	// ONNET_REQUIRE_HOST_KEY), and POST /api/admin/hosts/<rid>/stop ends it at
	// once.
	const bool revoke_sessions = update_revokes_sessions(p);

	std::string sets;
	std::vector<opennova::db::BindValue> binds;
	auto add_set = [&](const char *col, const std::string &val) {
		if (!sets.empty()) sets += ", ";
		sets += col;
		sets += " = ?";
		binds.emplace_back(val);
	};
	if (p.username) {
		// Refuse to rename to an already-taken handle.
		if (auto existing = get_user_by_username(db, *p.username);
		    existing && existing->id != id) {
			return err("username_exists", "username already taken");
		}
		add_set("username", *p.username);
	}
	if (p.pcid)     add_set("pcid",     *p.pcid);
	if (p.nwh)      add_set("nwh",      *p.nwh);
	if (p.nwhandle) add_set("nwhandle", *p.nwhandle);
	if (p.account_status) add_set("account_status", *p.account_status);
	if (p.role)     add_set("role",     *p.role);
	if (p.password_plaintext && !p.password_plaintext->empty()) {
		try {
			add_set("password_hash", hash_password(*p.password_plaintext));
		} catch (const std::exception &e) {
			return db_err("update_user password hash", e);
		}
	}
	// A revoking update also moves the account's credentials generation, in
	// the same statement, which ends every login made under the old one: a
	// login whose password check read it grants nothing once this commits
	// (register_active_user_session, create_web_session, record_login_address),
	// and its retail session row and pins stop counting
	// (get_user_by_login_session_tag, HttpListener::pinned_user).
	if (revoke_sessions)
		sets += ", credentials_gen = credentials_gen + 1";
	if (sets.empty()) {
		MutationResult m;
		m.ok = true;
		m.id = id;
		return m;  // no-op
	}
	binds.emplace_back(id);
	try {
		opennova::db::Transaction tx(db);
		db.exec("UPDATE players SET " + sets + " WHERE id = ?;", binds);
		if (revoke_sessions) {
			db.exec("DELETE FROM web_sessions WHERE user_id = ?;", { opennova::db::BindValue(id) });
			// The retail login's session row (its LOGINSESSIONTAG names the
			// account to /NWHost.dll, /NWJoin.dll and /NWAccount.dll) and the
			// hosting keys it minted.
			db.exec("DELETE FROM active_user_sessions WHERE user_id = ?;",
			        {opennova::db::BindValue(id)});
			db.exec("DELETE FROM host_keys WHERE user_id = ?;", {opennova::db::BindValue(id)});
		}
		tx.commit();
	} catch (const opennova::db::SqliteError &e) {
		return db_err("update_user", e);
	}
	MutationResult m;
	m.ok = true;
	m.id = id;
	return m;
}

bool update_revokes_sessions(const UpdateUserParams &p) {
	return (p.password_plaintext && !p.password_plaintext->empty()) ||
	       (p.account_status && *p.account_status != "active");
}

bool record_login_address(opennova::db::Database &db, int64_t user_id, int64_t credentials_gen,
		const std::string &address) {
	opennova::db::Transaction tx(db);
	// One statement: the generation it tests is the one the row is written
	// against. (An upsert after a SELECT needs the SELECT's WHERE.)
	db.exec("INSERT INTO login_addresses (user_id, address, last_success_at) "
			"SELECT id, ?, datetime('now') FROM players WHERE id = ? AND credentials_gen = ? "
			"ON CONFLICT(user_id, address) DO UPDATE SET last_success_at = datetime('now');",
			{ opennova::db::BindValue(address), opennova::db::BindValue(user_id),
					opennova::db::BindValue(credentials_gen) });
	if (db.changes() == 0)
		return false;
	// The newest kLoginAddressesPerUser stay.
	db.exec("DELETE FROM login_addresses WHERE user_id = ? AND rowid NOT IN ("
	        "SELECT rowid FROM login_addresses WHERE user_id = ? "
	        "ORDER BY last_success_at DESC, rowid DESC LIMIT ?);",
	        {opennova::db::BindValue(user_id), opennova::db::BindValue(user_id),
	         opennova::db::BindValue(static_cast<int64_t>(kLoginAddressesPerUser))});
	tx.commit();
	return true;
}

bool is_known_login_address(opennova::db::Database &db, const std::string &username,
                            const std::string &address) {
	return !db.query("SELECT 1 FROM login_addresses a JOIN players p ON p.id = a.user_id "
	                 "WHERE p.username = ? AND a.address = ? "
	                 "AND a.last_success_at > datetime('now', ?) LIMIT 1;",
	                 {opennova::db::BindValue(username), opennova::db::BindValue(address),
	                  opennova::db::BindValue("-" + std::to_string(kLoginAddressDays) + " days")})
	                .empty();
}

size_t prune_login_addresses(opennova::db::Database &db) {
	db.exec("DELETE FROM login_addresses WHERE last_success_at <= datetime('now', ?);",
	        {opennova::db::BindValue("-" + std::to_string(kLoginAddressDays) + " days")});
	return static_cast<size_t>(db.changes());
}

BootstrapAdminResult bootstrap_admin(opennova::db::Database &db, const std::string &username) {
	BootstrapAdminResult out;
	opennova::db::Transaction tx(db);
	const auto admins = db.query("SELECT username FROM players WHERE role = 'admin' ORDER BY id LIMIT 1;");
	if (!admins.empty()) {
		out.outcome = BootstrapAdminResult::Outcome::AdminExists;
		out.existing_admin = admins.front().as_text(0).value_or("");
		return out;
	}
	db.exec("UPDATE players SET role = 'admin' WHERE username = ? AND "
	        "NOT EXISTS (SELECT 1 FROM players WHERE role = 'admin');",
	        {opennova::db::BindValue(username)});
	out.outcome = db.changes() > 0 ? BootstrapAdminResult::Outcome::Promoted
	                               : BootstrapAdminResult::Outcome::NoSuchAccount;
	tx.commit();
	return out;
}

MutationResult update_game_access(opennova::db::Database &db, int64_t user_id,
                                  const UpdateGameAccessParams &p) {
	if (user_id <= 0) return err("not_found", "invalid user id");
	if (p.game_slug.empty() || p.status.empty() || p.exp_bits.empty()) {
		return err("missing_field", "game_slug, status, and exp_bits are required");
	}
	if (!get_user_by_id(db, user_id)) return err("not_found", "no such user");
	try {
		auto rows = db.query(
			"SELECT 1 FROM games WHERE slug = ? LIMIT 1;",
			{opennova::db::BindValue(p.game_slug)});
		if (rows.empty()) return err("not_found", "no such game");
		db.exec(
			"INSERT INTO player_game_access "
			"(user_id, game_slug, status, exp_bits, updated_at) "
			"VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP) "
			"ON CONFLICT(user_id, game_slug) DO UPDATE SET "
			"status=excluded.status, exp_bits=excluded.exp_bits, "
			"updated_at=CURRENT_TIMESTAMP;",
			{opennova::db::BindValue(user_id), opennova::db::BindValue(p.game_slug),
			 opennova::db::BindValue(p.status), opennova::db::BindValue(p.exp_bits)});
		MutationResult m;
		m.ok = true;
		m.id = user_id;
		return m;
	} catch (const opennova::db::SqliteError &e) {
		return db_err("update_game_access", e);
	}
}

} // namespace opennova::novaworld_server
