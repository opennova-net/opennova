#include "web_session.h"

#include <base/io/sha256.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <array>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server {

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

std::optional<std::string> create_web_session(db::Database &db, int64_t user_id,
		int64_t credentials_gen, const std::string &ip, const std::string &user_agent) {
	std::array<uint8_t, kWebSessionTokenBytes> raw{};
	os_random_bytes(raw.data(), raw.size());
	const std::string token = strutil::bytes_to_hex(raw.data(), raw.size());
	db::Transaction tx(db);
	// One statement: the generation and status it tests are the ones the
	// session is written against. It locks the account's row to the commit
	// (FOR NO KEY UPDATE), so update_user, which moves the generation and
	// deletes the sessions in one transaction, runs wholly before this one (and
	// this one, waiting on the row, then reads the moved generation and mints
	// nothing) or wholly after (and deletes this session). Two logins of one
	// account take turns on the row too, so the cap below holds exactly. A
	// hosted server's service account gets none (players.kind).
	db.exec("INSERT INTO web_sessions "
			"(token_hash, user_id, created_at, expires_at, last_seen_at, ip, user_agent) "
			"SELECT $1::text, id, now(), now() + make_interval(secs => $2), now(), $3::text, "
			"$4::text "
			"FROM players WHERE id = $5 AND credentials_gen = $6 AND account_status = 'active' "
			"AND kind = 'player' FOR NO KEY UPDATE;",
			{ db::BindValue(web_session_token_hash(token)),
					db::BindValue(kWebSessionLifetimeSeconds), db::BindValue(ip),
					db::BindValue(user_agent), db::BindValue(user_id),
					db::BindValue(credentials_gen) });
	if (db.changes() == 0)
		return std::nullopt;
	// The account's newest kWebSessionsPerUser stay; the oldest beyond them go.
	// id orders sessions minted at one instant.
	db.exec(
		"DELETE FROM web_sessions WHERE user_id = $1 AND id NOT IN ("
		"SELECT id FROM web_sessions WHERE user_id = $1 "
		"ORDER BY created_at DESC, id DESC LIMIT $2);",
		{db::BindValue(user_id), db::BindValue(static_cast<int64_t>(kWebSessionsPerUser))});
	tx.commit();
	return token;
}

std::optional<WebSessionLookup> resolve_web_session(db::Database &db, std::string_view token) {
	if (!is_web_session_token(token)) return std::nullopt;
	const std::string hash = web_session_token_hash(token);
	const auto rows = db.query(
		"SELECT p.id, p.username, p.role, p.account_status, "
		"       s.last_seen_at <= now() - make_interval(secs => $1), p.kind "
		"FROM web_sessions s JOIN players p ON p.id = s.user_id "
		"WHERE s.token_hash = $2 AND s.expires_at > now() LIMIT 1;",
		{db::BindValue(kWebSessionTouchSeconds), db::BindValue(hash)});
	if (rows.empty()) return std::nullopt;
	const auto &row = rows.front();
	// A banned or restricted account keeps no session: the game login refuses
	// every status but 'active', and so does the site. (The schema refuses an
	// empty status, so 'active' alone is active.) Nor does a hosted server's
	// service account, which never gets one (create_web_session).
	const std::string status = row.as_text(3).value_or("");
	if (status != "active" || row.as_text(5).value_or("") != "player") {
		db.exec("DELETE FROM web_sessions WHERE token_hash = $1;", {db::BindValue(hash)});
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
			"UPDATE web_sessions SET last_seen_at = now(), "
			"expires_at = now() + make_interval(secs => $1) "
			"WHERE token_hash = $2 AND last_seen_at <= now() - make_interval(secs => $3);",
			{db::BindValue(kWebSessionLifetimeSeconds), db::BindValue(hash),
			 db::BindValue(kWebSessionTouchSeconds)});
		out.slid = db.changes() > 0;
	}
	return out;
}

void delete_web_session(db::Database &db, std::string_view token) {
	if (!is_web_session_token(token)) return;
	db.exec("DELETE FROM web_sessions WHERE token_hash = $1;",
	        {db::BindValue(web_session_token_hash(token))});
}

size_t prune_expired_web_sessions(db::Database &db) {
	db.exec("DELETE FROM web_sessions WHERE expires_at <= now();");
	return static_cast<size_t>(db.changes());
}

} // namespace opennova::novaworld_server
