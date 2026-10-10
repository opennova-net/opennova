#pragma once

#include <net/novaworld/db/sqlite.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace opennova::novaworld_server {

// The NovaWorld site's login sessions (web_sessions, migration 0008), the
// store behind POST /api/login, POST /api/logout and the session cookie the
// /api routes authenticate a browser by. Crow-free, so a unit test drives it on
// every build.
//
// A session is a 32-byte token from the OS CSPRNG (base/os_random), spelled as
// 64 lower-case hex digits in the cookie. The table keeps only the token's
// SHA-256 (base/io/sha256.h): a copy of the database names no usable session.
// It lasts kWebSessionLifetimeSeconds from its last use: every use more than
// kWebSessionTouchSeconds after the last recorded one moves last_seen_at and
// expires_at forward, so a busy session writes at most once a minute.

// The cookie that carries the token: on an http site, and with the __Host-
// prefix wherever the cookie is Secure (ONNET_COOKIE_SECURE), so a sibling
// subdomain cannot plant one (a __Host- cookie must be Secure, Path=/ and
// carry no Domain; the browser refuses any other). The server reads only the
// name its own config sets.
inline constexpr const char *kWebSessionCookie = "opennova_session";
inline constexpr const char *kWebSessionHostCookie = "__Host-opennova_session";
// The live sessions one account keeps; a login past this ends the oldest.
inline constexpr size_t kWebSessionsPerUser = 20;
// 30 days, sliding.
inline constexpr int64_t kWebSessionLifetimeSeconds = 30 * 24 * 60 * 60;
inline constexpr int64_t kWebSessionTouchSeconds = 60;
inline constexpr size_t kWebSessionTokenBytes = 32;

// The account a live session belongs to.
struct WebSessionUser {
	int64_t     id = 0;
	std::string username;
	std::string role; // players.role: "player" or "admin"
};

struct WebSessionLookup {
	WebSessionUser user;
	// This lookup moved expires_at forward (the cookie's Max-Age is renewed
	// with it).
	bool slid = false;
};

// True for 64 lower-case hex digits, the only form a minted token takes; any
// other cookie value is refused before it reaches the database.
bool is_web_session_token(std::string_view token);

// The stored form of a token: its SHA-256 as 64 lower-case hex digits.
std::string web_session_token_hash(std::string_view token);

// Mints a session for `user_id` and returns its token (the cookie value).
// `ip` and `user_agent` are recorded for the account's session list. The
// account's sessions beyond the newest kWebSessionsPerUser are deleted in the
// same transaction. Throws db::SqliteError when the insert fails.
std::string create_web_session(db::Database &db, int64_t user_id, const std::string &ip,
                               const std::string &user_agent);

// The user a token's live session belongs to, sliding its expiry when the last
// recorded use is older than kWebSessionTouchSeconds. nullopt for a malformed,
// unknown or expired token, and for a session whose account is no longer
// active (banned or restricted), which is deleted on the spot. Throws
// db::SqliteError on a database failure.
std::optional<WebSessionLookup> resolve_web_session(db::Database &db, std::string_view token);

// Deletes the token's session (logout); an unknown token is a no-op.
void delete_web_session(db::Database &db, std::string_view token);

// Deletes every session past its expiry and returns how many went. The main
// tick's sweep calls it.
size_t prune_expired_web_sessions(db::Database &db);

} // namespace opennova::novaworld_server
