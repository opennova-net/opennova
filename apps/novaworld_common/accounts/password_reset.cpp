#include "password_reset.h"

#include "web_session.h"

#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <array>

namespace opennova::novaworld_server {

std::string issue_password_reset(db::Database &db, int64_t user_id) {
	std::array<uint8_t, kPasswordResetTokenBytes> raw{};
	os_random_bytes(raw.data(), raw.size());
	const std::string token = strutil::bytes_to_hex(raw.data(), raw.size());
	db.exec("INSERT INTO password_resets (user_id, token_hash, created_at, expires_at) "
			"VALUES ($1, $2, now(), now() + make_interval(secs => $3)) "
			"ON CONFLICT (user_id) DO UPDATE SET token_hash = excluded.token_hash, "
			"created_at = excluded.created_at, expires_at = excluded.expires_at;",
			{ db::BindValue(user_id), db::BindValue(web_session_token_hash(token)),
					db::BindValue(kPasswordResetLifetimeSeconds) });
	return token;
}

std::optional<int64_t> claim_password_reset(db::Database &db, std::string_view token) {
	if (!is_web_session_token(token))
		return std::nullopt;
	// One statement: the delete is the claim, so a token is spent once.
	const auto rows = db.query("DELETE FROM password_resets WHERE token_hash = $1 "
							   "RETURNING user_id, expires_at > now();",
			{ db::BindValue(web_session_token_hash(token)) });
	if (rows.empty() || rows.front().as_int(1).value_or(0) == 0)
		return std::nullopt;
	return rows.front().as_int(0);
}

size_t prune_password_resets(db::Database &db) {
	db.exec("DELETE FROM password_resets WHERE expires_at <= now();");
	return static_cast<size_t>(db.changes());
}

} // namespace opennova::novaworld_server
