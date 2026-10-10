#include "web_session.h"

#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <array>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server {

namespace {

// SQLite datetime() modifiers: the lifetime ahead of now, and the touch
// interval behind it.
std::string seconds_modifier(int64_t seconds) {
	return (seconds < 0 ? "" : "+") + std::to_string(seconds) + " seconds";
}

} // namespace

bool is_web_session_token(std::string_view token) {
	if (token.size() != kWebSessionTokenBytes * 2) return false;
	for (const char c : token) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return true;
}

std::string web_session_token_hash(std::string_view token) {
	return io::sha256_hex(io::sha256(token));
}

std::string create_web_session(db::Database &db, int64_t user_id, const std::string &ip,
                               const std::string &user_agent) {
	std::array<uint8_t, kWebSessionTokenBytes> raw{};
	os_random_bytes(raw.data(), raw.size());
	const std::string token = strutil::bytes_to_hex(raw.data(), raw.size());
	db.exec(
		"INSERT INTO web_sessions "
		"(token_hash, user_id, created_at, expires_at, last_seen_at, ip, user_agent) "
		"VALUES (?, ?, datetime('now'), datetime('now', ?), datetime('now'), ?, ?);",
		{db::BindValue(web_session_token_hash(token)), db::BindValue(user_id),
		 db::BindValue(seconds_modifier(kWebSessionLifetimeSeconds)), db::BindValue(ip),
		 db::BindValue(user_agent)});
	return token;
}

std::optional<WebSessionLookup> resolve_web_session(db::Database &db, std::string_view token) {
	if (!is_web_session_token(token)) return std::nullopt;
	const std::string hash = web_session_token_hash(token);
	const auto rows = db.query(
		"SELECT p.id, p.username, p.role, p.account_status, "
		"       s.last_seen_at <= datetime('now', ?) "
		"FROM web_sessions s JOIN players p ON p.id = s.user_id "
		"WHERE s.token_hash = ? AND s.expires_at > datetime('now') LIMIT 1;",
		{db::BindValue(seconds_modifier(-kWebSessionTouchSeconds)), db::BindValue(hash)});
	if (rows.empty()) return std::nullopt;
	const auto &row = rows.front();
	// A banned or restricted account keeps no session: the game login refuses
	// every status but 'active', and so does the site.
	if (row.as_text(3).value_or("active") != "active") {
		db.exec("DELETE FROM web_sessions WHERE token_hash = ?;", {db::BindValue(hash)});
		return std::nullopt;
	}
	WebSessionLookup out;
	out.user.id = row.as_int(0).value_or(0);
	out.user.username = row.as_text(1).value_or("");
	out.user.role = row.as_text(2).value_or("player");
	if (row.as_int(4).value_or(0) != 0) {
		// The WHERE repeats the staleness test, so of two requests racing on
		// one stale session only one slides it.
		db.exec(
			"UPDATE web_sessions SET last_seen_at = datetime('now'), "
			"expires_at = datetime('now', ?) "
			"WHERE token_hash = ? AND last_seen_at <= datetime('now', ?);",
			{db::BindValue(seconds_modifier(kWebSessionLifetimeSeconds)), db::BindValue(hash),
			 db::BindValue(seconds_modifier(-kWebSessionTouchSeconds))});
		out.slid = db.changes() > 0;
	}
	return out;
}

void delete_web_session(db::Database &db, std::string_view token) {
	if (!is_web_session_token(token)) return;
	db.exec("DELETE FROM web_sessions WHERE token_hash = ?;",
	        {db::BindValue(web_session_token_hash(token))});
}

size_t prune_expired_web_sessions(db::Database &db) {
	db.exec("DELETE FROM web_sessions WHERE expires_at <= datetime('now');");
	return static_cast<size_t>(db.changes());
}

} // namespace opennova::novaworld_server
