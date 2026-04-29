#include "auth.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>

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

namespace opennova::server {

namespace {

// Verify a password against a bcrypt hash. The stored hash format is
//     $2a$<cost>$<22-char-salt><31-char-hash>     (or $2b$ — same algo)
// We hand `bcrypt_hashpass` the password + the stored hash (which doubles
// as the salt source), and compare the recomputed hash byte-for-byte
// against the stored hash with timingsafe_bcmp.
//
// Falls back to plain-string compare for stored_hash values that don't
// look like a bcrypt hash — covers the dev seed where 'test'/'foo' are
// stored as plaintext until you regenerate the seed (`tools/gen-bcrypt`
// or via the admin POST /api/admin/users path). This path is logged so
// it's obvious when plaintext credentials are still in use.
bool verify_password(const std::string &plain, const std::string &stored) {
	if (stored.size() >= 4 && stored[0] == '$' && stored[1] == '2' &&
	    (stored[2] == 'a' || stored[2] == 'b' || stored[2] == 'y') &&
	    stored[3] == '$') {
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
	u.account_status = row.as_text(5).value_or("active");
	if (u.account_status.empty()) u.account_status = "active";
	if (u.id == 0) return std::nullopt;
	return u;
}

MutationResult err(const char *code, const char *msg) {
	MutationResult m;
	m.ok = false;
	m.error_code = code;
	m.error_message = msg;
	return m;
}

} // namespace

std::optional<UserRecord> authenticate_user(opennova::db::Database &db,
                                            const std::string &username,
                                            const std::string &password) {
	if (username.empty()) return std::nullopt;
	std::vector<opennova::db::Row> rows;
	try {
		rows = db.query(
			"SELECT id, username, pcid, nwh, nwhandle, account_status, password_hash "
			"FROM players WHERE username = ? LIMIT 1;",
			{opennova::db::BindValue(username)});
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN player lookup failed: %s\n", e.what());
		return std::nullopt;
	}
	if (rows.empty()) {
		std::fprintf(stderr, "[auth] user '%s' not found\n", username.c_str());
		return std::nullopt;
	}
	const auto stored_hash = rows.front().as_text(6).value_or("");
	if (!verify_password(password, stored_hash)) {
		std::fprintf(stderr, "[auth] bad password for user '%s'\n", username.c_str());
		return std::nullopt;
	}
	auto user = row_to_user(rows.front());
	if (user) {
		// Update last_login (best-effort; auth still succeeds if this fails).
		try {
			db.exec("UPDATE players SET last_login = CURRENT_TIMESTAMP WHERE id = ?;",
			        {opennova::db::BindValue(user->id)});
		} catch (const opennova::db::SqliteError &) { /* ignore */ }
	}
	return user;
}

std::optional<UserRecord> get_user_by_username(opennova::db::Database &db,
                                                const std::string &username) {
	if (username.empty()) return std::nullopt;
	try {
		auto rows = db.query(
			"SELECT id, username, pcid, nwh, nwhandle, account_status "
			"FROM players WHERE username = ? OR nwhandle = ? LIMIT 1;",
			{opennova::db::BindValue(username), opennova::db::BindValue(username)});
		if (rows.empty()) return std::nullopt;
		return row_to_user(rows.front());
	} catch (const opennova::db::SqliteError &) {
		return std::nullopt;
	}
}

std::optional<UserRecord> get_user_by_id(opennova::db::Database &db, int64_t id) {
	if (id == 0) return std::nullopt;
	try {
		auto rows = db.query(
			"SELECT id, username, pcid, nwh, nwhandle, account_status "
			"FROM players WHERE id = ? LIMIT 1;",
			{opennova::db::BindValue(id)});
		if (rows.empty()) return std::nullopt;
		return row_to_user(rows.front());
	} catch (const opennova::db::SqliteError &) {
		return std::nullopt;
	}
}

std::vector<UserRecord> list_dev_players(opennova::db::Database &db) {
	std::vector<UserRecord> out;
	try {
		auto rows = db.query(
			"SELECT id, username, pcid, nwh, nwhandle, account_status "
			"FROM players ORDER BY id;");
		out.reserve(rows.size());
		for (const auto &r : rows) {
			if (auto u = row_to_user(r)) out.push_back(*u);
		}
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN list_dev_players failed: %s\n", e.what());
	}
	return out;
}

std::vector<UserRecord> list_users(opennova::db::Database &db) {
	// Same as list_dev_players today — splitting the API surface so
	// "admin user listing" can grow independently (e.g. include
	// last_login, created_at) without touching the round-robin path.
	return list_dev_players(db);
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
		return err("db_error", e.what());
	}
}

bool has_active_user_session(opennova::db::Database &db, int64_t user_id) {
	if (user_id == 0) return false;
	try {
		auto rows = db.query(
			"SELECT 1 FROM active_user_sessions WHERE user_id = ? LIMIT 1;",
			{opennova::db::BindValue(user_id)});
		return !rows.empty();
	} catch (const opennova::db::SqliteError &e) {
		std::fprintf(stderr, "[auth] WARN active session lookup failed: %s\n", e.what());
		return false;
	}
}

void register_active_user_session(opennova::db::Database &db, int64_t user_id,
                                  const std::string &username,
                                  const std::string &session_tag,
                                  const std::string &persistent_id,
                                  const std::string &remote_ip,
                                  const std::string &user_agent) {
	db.exec(
		"INSERT OR REPLACE INTO active_user_sessions "
		"(user_id, username, session_tag, persistent_id, remote_ip, user_agent, created_at, last_seen_at) "
		"VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);",
		{opennova::db::BindValue(user_id), opennova::db::BindValue(username),
		 opennova::db::BindValue(session_tag), opennova::db::BindValue(persistent_id),
		 opennova::db::BindValue(remote_ip), opennova::db::BindValue(user_agent)});
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
	// 16 random bytes via std::random_device → bcrypt's modified-base64
	// → assembled as $2b$<cost>$<22-char-salt> for hand-off to bcrypt_hashpass.
	std::array<uint8_t, 16> raw{};
	std::random_device rd;
	for (auto &b : raw) b = static_cast<uint8_t>(rd());
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

MutationResult create_user(opennova::db::Database &db, const CreateUserParams &p) {
	if (p.username.empty() || p.password.empty() || p.pcid.empty() ||
	    p.nwhandle.empty()) {
		return err("missing_field",
		           "username, password, pcid, nwhandle are required");
	}
	std::string nwh = p.nwh.empty() ? std::string("1") : p.nwh;

	// Detect duplicate-by-username / duplicate-by-pcid up front so we can
	// return a useful error code instead of relying on sqlite's UNIQUE
	// constraint violation text.
	if (get_user_by_username(db, p.username)) {
		return err("username_exists", "username already taken");
	}
	try {
		auto rows = db.query(
			"SELECT id FROM players WHERE pcid = ? LIMIT 1;",
			{opennova::db::BindValue(p.pcid)});
		if (!rows.empty()) {
			return err("pcid_exists", "PCID already in use");
		}
	} catch (const opennova::db::SqliteError &e) {
		return err("db_error", e.what());
	}

	std::string hashed;
	try {
		hashed = hash_password(p.password);
	} catch (const std::exception &e) {
		return err("db_error", e.what());
	}
	try {
		db.exec(
			"INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
			"VALUES (?, ?, ?, ?, ?);",
			{opennova::db::BindValue(p.username),
			 opennova::db::BindValue(hashed),
			 opennova::db::BindValue(p.pcid),
			 opennova::db::BindValue(nwh),
			 opennova::db::BindValue(p.nwhandle)});
	} catch (const opennova::db::SqliteError &e) {
		return err("db_error", e.what());
	}
	MutationResult m;
	m.ok = true;
	m.id = db.last_insert_rowid();
	return m;
}

MutationResult delete_user(opennova::db::Database &db, int64_t id) {
	if (id <= 0) return err("not_found", "invalid id");
	try {
		db.exec("DELETE FROM players WHERE id = ?;",
		        {opennova::db::BindValue(id)});
		if (db.changes() == 0) return err("not_found", "no such user");
	} catch (const opennova::db::SqliteError &e) {
		return err("db_error", e.what());
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
	if (p.password_plaintext && !p.password_plaintext->empty()) {
		try {
			add_set("password_hash", hash_password(*p.password_plaintext));
		} catch (const std::exception &e) {
			return err("db_error", e.what());
		}
	}
	if (sets.empty()) {
		MutationResult m;
		m.ok = true;
		m.id = id;
		return m;  // no-op
	}
	binds.emplace_back(id);
	try {
		db.exec("UPDATE players SET " + sets + " WHERE id = ?;", binds);
	} catch (const opennova::db::SqliteError &e) {
		return err("db_error", e.what());
	}
	MutationResult m;
	m.ok = true;
	m.id = id;
	return m;
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
		return err("db_error", e.what());
	}
}

} // namespace opennova::server
