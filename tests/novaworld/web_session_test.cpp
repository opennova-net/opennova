// The NovaWorld site's login-session store (apps/novaworld_server/
// web_session.*) and account roles (auth.*'s promote_to_admin, the role
// column) over a migrated in-memory database: the token minted and kept only
// as its SHA-256, a live session's user, the refusals (malformed, unknown,
// expired, an inactive account), the once-a-minute sliding expiry, logout,
// the prune, the cascade from a deleted player, and the bootstrap promotion.
// Crow-free (the sources compile straight in, as db_concurrency_test does
// with auth.cpp), so it runs on every build; the HTTP route harness drives the
// same store through the cookie.

#include "auth.h"
#include "web_session.h"

#include <base/io/sha256.h>
#include <net/novaworld/db/sqlite.h>

#include "common/test_expect.h"

#include <cstdio>
#include <filesystem>
#include <string>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace nws = opennova::novaworld_server;
using opennova::db::BindValue;
using opennova::db::Database;

namespace {

int64_t make_player(Database &db, const std::string &username, const std::string &pcid) {
	nws::CreateUserParams p;
	p.username = username;
	p.password = "pw-" + username;
	p.pcid = pcid;
	p.nwh = "1";
	p.nwhandle = username;
	const auto result = nws::create_user(db, p);
	return result.ok ? result.id : 0;
}

// A one-value query's integer (a 0/1 comparison, a count).
int64_t scalar(Database &db, const std::string &sql, const std::string &hash) {
	const auto rows = db.query(sql, {BindValue(hash)});
	return rows.empty() ? -1 : rows.front().as_int(0).value_or(-1);
}

// A minted token: 64 lower-case hex digits, a fresh one each time; the table
// holds its SHA-256 and the request's address and agent, never the token.
int test_create_keeps_only_the_hash(Database &db, int64_t alice) {
	const std::string token = nws::create_web_session(db, alice, "198.51.100.7", "agent/1");
	const std::string other = nws::create_web_session(db, alice, "198.51.100.7", "agent/1");
	TEST_EXPECT(nws::is_web_session_token(token));
	TEST_EXPECT(token != other);
	const std::string hash = nws::web_session_token_hash(token);
	TEST_EXPECT(hash == opennova::io::sha256_hex(opennova::io::sha256(token)));
	TEST_EXPECT(hash != token);
	const auto rows = db.query(
		"SELECT user_id, ip, user_agent, expires_at > datetime('now', '+29 days') "
		"FROM web_sessions WHERE token_hash = ?;",
		{BindValue(hash)});
	TEST_EXPECT(rows.size() == 1);
	TEST_EXPECT(rows[0].as_int(0).value() == alice);
	TEST_EXPECT(rows[0].as_text(1).value() == "198.51.100.7");
	TEST_EXPECT(rows[0].as_text(2).value() == "agent/1");
	TEST_EXPECT(rows[0].as_int(3).value() == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM web_sessions WHERE token_hash = ?;",
	                     {BindValue(token)}).empty());
	return 0;
}

// A live session resolves to its account; a malformed, unknown or expired
// token resolves to nothing.
int test_resolve_and_refusals(Database &db, int64_t alice) {
	const std::string token = nws::create_web_session(db, alice, "", "");
	const auto found = nws::resolve_web_session(db, token);
	TEST_EXPECT(found.has_value());
	TEST_EXPECT(found->user.id == alice);
	TEST_EXPECT(found->user.username == "alice");
	TEST_EXPECT(found->user.role == "player");
	TEST_EXPECT(!found->slid); // minted this second

	TEST_EXPECT(!nws::is_web_session_token(""));
	TEST_EXPECT(!nws::is_web_session_token(token.substr(1)));
	std::string upper = token;
	for (char &c : upper) c = (c >= 'a' && c <= 'f') ? static_cast<char>(c - 32) : c;
	TEST_EXPECT(upper == token || !nws::is_web_session_token(upper));
	TEST_EXPECT(!nws::resolve_web_session(db, "not-a-token").has_value());
	TEST_EXPECT(!nws::resolve_web_session(db, std::string(64, '0')).has_value());

	db.exec("UPDATE web_sessions SET expires_at = datetime('now', '-1 seconds') "
	        "WHERE token_hash = ?;",
	        {BindValue(nws::web_session_token_hash(token))});
	TEST_EXPECT(!nws::resolve_web_session(db, token).has_value());
	return 0;
}

// The expiry slides 30 days ahead of a use that comes a minute or more after
// the last recorded one, and stays put for a use within that minute.
int test_sliding_expiry(Database &db, int64_t alice) {
	const std::string token = nws::create_web_session(db, alice, "", "");
	const std::string hash = nws::web_session_token_hash(token);
	db.exec("UPDATE web_sessions SET last_seen_at = datetime('now', '-120 seconds'), "
	        "expires_at = datetime('now', '+1 days') WHERE token_hash = ?;",
	        {BindValue(hash)});
	auto found = nws::resolve_web_session(db, token);
	TEST_EXPECT(found && found->slid);
	TEST_EXPECT(scalar(db, "SELECT expires_at > datetime('now', '+29 days') FROM web_sessions "
	                       "WHERE token_hash = ?;", hash) == 1);
	TEST_EXPECT(scalar(db, "SELECT last_seen_at >= datetime('now', '-5 seconds') FROM web_sessions "
	                       "WHERE token_hash = ?;", hash) == 1);

	db.exec("UPDATE web_sessions SET last_seen_at = datetime('now', '-10 seconds'), "
	        "expires_at = datetime('now', '+1 days') WHERE token_hash = ?;",
	        {BindValue(hash)});
	found = nws::resolve_web_session(db, token);
	TEST_EXPECT(found && !found->slid);
	TEST_EXPECT(scalar(db, "SELECT expires_at < datetime('now', '+2 days') FROM web_sessions "
	                       "WHERE token_hash = ?;", hash) == 1);
	return 0;
}

// Logout deletes the one session; the prune deletes the expired ones and
// keeps the live.
int test_delete_and_prune(Database &db, int64_t alice) {
	const std::string gone = nws::create_web_session(db, alice, "", "");
	const std::string kept = nws::create_web_session(db, alice, "", "");
	nws::delete_web_session(db, gone);
	TEST_EXPECT(!nws::resolve_web_session(db, gone).has_value());
	TEST_EXPECT(nws::resolve_web_session(db, kept).has_value());
	nws::delete_web_session(db, gone);          // twice: a no-op
	nws::delete_web_session(db, "not-a-token"); // malformed: a no-op

	const std::string stale = nws::create_web_session(db, alice, "", "");
	db.exec("UPDATE web_sessions SET expires_at = datetime('now', '-1 seconds') "
	        "WHERE token_hash = ?;",
	        {BindValue(nws::web_session_token_hash(stale))});
	TEST_EXPECT(nws::prune_expired_web_sessions(db) >= 1);
	TEST_EXPECT(scalar(db, "SELECT COUNT(*) FROM web_sessions WHERE token_hash = ?;",
	                   nws::web_session_token_hash(stale)) == 0);
	TEST_EXPECT(scalar(db, "SELECT COUNT(*) FROM web_sessions WHERE token_hash = ?;",
	                   nws::web_session_token_hash(kept)) == 1);
	TEST_EXPECT(db.query("SELECT 1 FROM web_sessions WHERE expires_at <= datetime('now');").empty());
	return 0;
}

// A banned or restricted account keeps no session (resolving deletes it), and
// a deleted player takes its sessions with it.
int test_inactive_and_deleted_accounts(Database &db) {
	const int64_t bob = make_player(db, "bob", "0000b0b0");
	TEST_EXPECT(bob > 0);
	const std::string token = nws::create_web_session(db, bob, "", "");
	TEST_EXPECT(nws::resolve_web_session(db, token).has_value());
	nws::UpdateUserParams ban;
	ban.account_status = "banned";
	TEST_EXPECT(nws::update_user(db, bob, ban).ok);
	TEST_EXPECT(!nws::resolve_web_session(db, token).has_value());
	TEST_EXPECT(scalar(db, "SELECT COUNT(*) FROM web_sessions WHERE token_hash = ?;",
	                   nws::web_session_token_hash(token)) == 0);

	const int64_t carol = make_player(db, "carol", "0000ca01");
	TEST_EXPECT(carol > 0);
	const std::string carols = nws::create_web_session(db, carol, "", "");
	TEST_EXPECT(nws::delete_user(db, carol).ok);
	TEST_EXPECT(scalar(db, "SELECT COUNT(*) FROM web_sessions WHERE token_hash = ?;",
	                   nws::web_session_token_hash(carols)) == 0);
	return 0;
}

// ONNET_BOOTSTRAP_ADMIN's promotion: the account's role (read live by every
// session lookup) becomes admin; a name no account has promotes nobody. The
// admin update takes only "player" or "admin".
int test_roles(Database &db, int64_t alice) {
	auto role_of = [&db, alice]() {
		const auto user = nws::get_user_by_id(db, alice);
		return user ? user->role : std::string("(none)");
	};
	auto session_role = [&db](const std::string &token) {
		const auto session = nws::resolve_web_session(db, token);
		return session ? session->user.role : std::string("(none)");
	};
	const std::string token = nws::create_web_session(db, alice, "", "");
	TEST_EXPECT(role_of() == "player");
	TEST_EXPECT(nws::promote_to_admin(db, "alice"));
	TEST_EXPECT(nws::promote_to_admin(db, "alice")); // already one: still found
	TEST_EXPECT(role_of() == "admin");
	TEST_EXPECT(session_role(token) == "admin");
	TEST_EXPECT(!nws::promote_to_admin(db, "nobody"));
	TEST_EXPECT(!nws::promote_to_admin(db, ""));

	nws::UpdateUserParams bad;
	bad.role = "superuser";
	const auto refused = nws::update_user(db, alice, bad);
	TEST_EXPECT(!refused.ok && refused.error_code == "invalid_field");
	TEST_EXPECT(role_of() == "admin");
	nws::UpdateUserParams demote;
	demote.role = "player";
	TEST_EXPECT(nws::update_user(db, alice, demote).ok);
	TEST_EXPECT(session_role(token) == "player");
	return 0;
}

// authenticate_user reads the role, and an unknown name is refused like a
// wrong password.
int test_authenticate(Database &db, int64_t alice) {
	const auto user = nws::authenticate_user(db, "alice", "pw-alice");
	TEST_EXPECT(user && user->id == alice && user->role == "player");
	TEST_EXPECT(!nws::authenticate_user(db, "alice", "wrong").has_value());
	TEST_EXPECT(!nws::authenticate_user(db, "no-such-user", "pw-alice").has_value());
	return 0;
}

} // namespace

int main() {
	const std::filesystem::path source_dir{OPENNOVA_SOURCE_DIR};
	Database db(":memory:");
	opennova::db::run_migrations(db, source_dir / "backend" / "migrations");
	const int64_t alice = make_player(db, "alice", "0000a11c");
	TEST_EXPECT(alice > 0);

	struct Case { const char *name; int (*fn)(Database &, int64_t); };
	const Case cases[] = {
		{"create_keeps_only_the_hash", test_create_keeps_only_the_hash},
		{"resolve_and_refusals", test_resolve_and_refusals},
		{"sliding_expiry", test_sliding_expiry},
		{"delete_and_prune", test_delete_and_prune},
		{"authenticate", test_authenticate},
		{"roles", test_roles},
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn(db, alice) != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("-- inactive_and_deleted_accounts\n");
	if (test_inactive_and_deleted_accounts(db) != 0) {
		std::fprintf(stderr, "FAIL: inactive_and_deleted_accounts\n");
		return 1;
	}
	std::printf("OK: web sessions + roles\n");
	return 0;
}
