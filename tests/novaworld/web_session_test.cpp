// The NovaWorld site's login-session store (apps/novaworld_server/
// web_session.*) and account roles (auth.*: bootstrap_admin, the role column,
// update_user's session revocation) over a migrated in-memory database: the
// token minted and kept only as its SHA-256, the per-account session cap, a
// live session's user, the refusals (malformed, unknown, expired, an inactive
// account), the once-a-minute sliding expiry and its race, logout, the prune,
// the revocations (a password reset, a ban, a deleted player), the credentials
// generation (a reset that commits during a login's password check grants that
// login nothing), the username and handle rules, the known addresses, the
// one-shot bootstrap promotion, the unknown-username bcrypt run, and what a
// deleted or revoked account leaves of the servers it listed and joined.
// Crow-free (the
// sources compile straight in, as db_concurrency_test does with auth.cpp), so
// it runs on every build; the HTTP route harness drives the same store through
// the cookie.

#include "auth.h"
#include "web_session.h"

#include <base/io/sha256.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/host_repository.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace nws = opennova::novaworld_server;
using opennova::db::BindValue;
using opennova::db::Database;

namespace {

const std::filesystem::path kSourceDir{OPENNOVA_SOURCE_DIR};

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

bool session_exists(Database &db, const std::string &token) {
	return scalar(db, "SELECT COUNT(*) FROM web_sessions WHERE token_hash = ?;",
	              nws::web_session_token_hash(token)) == 1;
}

// The account's credentials generation now (-1: no account).
int64_t generation(Database &db, int64_t id) {
	const auto user = nws::get_user_by_id(db, id);
	return user ? user->credentials_gen : -1;
}

// A session minted at the account's current generation, the way a login just
// past its password check mints one: its token, "" when none was minted.
std::string mint(
		Database &db, int64_t id, const std::string &ip = "", const std::string &agent = "") {
	return nws::create_web_session(db, id, generation(db, id), ip, agent).value_or("");
}

// A known address recorded at the account's current generation.
bool remember(Database &db, int64_t id, const std::string &address) {
	return nws::record_login_address(db, id, generation(db, id), address);
}

// A minted token: 64 lower-case hex digits, a fresh one each time; the table
// holds its SHA-256 and the request's address and agent, never the token.
int test_create_keeps_only_the_hash(Database &db, int64_t alice) {
	const std::string token = mint(db, alice, "198.51.100.7", "agent/1");
	const std::string other = mint(db, alice, "198.51.100.7", "agent/1");
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

// An account keeps its newest kWebSessionsPerUser sessions: each login past
// them ends the oldest, and other accounts' sessions are untouched.
int test_sessions_per_account_capped(Database &db, int64_t) {
	const int64_t many = make_player(db, "many", "0000aa01");
	const int64_t other = make_player(db, "other", "0000aa02");
	TEST_EXPECT(many > 0 && other > 0);
	const std::string others = mint(db, other);
	std::vector<std::string> tokens;
	for (size_t i = 0; i < nws::kWebSessionsPerUser + 2; ++i) {
		tokens.push_back(mint(db, many));
	}
	const auto count = db.query("SELECT COUNT(*) FROM web_sessions WHERE user_id = ?;",
	                            {BindValue(many)});
	TEST_EXPECT(count[0].as_int(0).value() == static_cast<int64_t>(nws::kWebSessionsPerUser));
	TEST_EXPECT(!session_exists(db, tokens[0]));
	TEST_EXPECT(!session_exists(db, tokens[1]));
	for (size_t i = 2; i < tokens.size(); ++i) TEST_EXPECT(session_exists(db, tokens[i]));
	TEST_EXPECT(session_exists(db, others));
	return 0;
}

// A live session resolves to its account; a malformed, unknown or expired
// token resolves to nothing.
int test_resolve_and_refusals(Database &db, int64_t alice) {
	const std::string token = mint(db, alice);
	const auto found = nws::resolve_web_session(db, token);
	TEST_EXPECT(found.has_value());
	TEST_EXPECT(found->user.id == alice);
	TEST_EXPECT(found->user.username == "alice");
	TEST_EXPECT(found->user.role == "player");
	TEST_EXPECT(!found->slid); // minted this second

	TEST_EXPECT(!nws::is_web_session_token(""));
	TEST_EXPECT(!nws::is_web_session_token(token.substr(1)));
	TEST_EXPECT(!nws::is_web_session_token(std::string(64, 'A')));
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
	const std::string token = mint(db, alice);
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
	const std::string gone = mint(db, alice);
	const std::string kept = mint(db, alice);
	nws::delete_web_session(db, gone);
	TEST_EXPECT(!nws::resolve_web_session(db, gone).has_value());
	TEST_EXPECT(nws::resolve_web_session(db, kept).has_value());
	nws::delete_web_session(db, gone);          // twice: a no-op
	nws::delete_web_session(db, "not-a-token"); // malformed: a no-op

	const std::string stale = mint(db, alice);
	db.exec("UPDATE web_sessions SET expires_at = datetime('now', '-1 seconds') "
	        "WHERE token_hash = ?;",
	        {BindValue(nws::web_session_token_hash(stale))});
	TEST_EXPECT(nws::prune_expired_web_sessions(db) >= 1);
	TEST_EXPECT(!session_exists(db, stale));
	TEST_EXPECT(session_exists(db, kept));
	TEST_EXPECT(db.query("SELECT 1 FROM web_sessions WHERE expires_at <= datetime('now');").empty());
	return 0;
}

// authenticate_user reads the role, and an unknown username costs one bcrypt
// check exactly as a wrong password does (the counter seam fails this if the
// unknown-user hash is skipped).
int test_authenticate(Database &db, int64_t alice) {
	auto checks = [] { return nws::password_verifications(); };
	uint64_t before = checks();
	const auto user = nws::authenticate_user(db, "alice", "pw-alice");
	TEST_EXPECT(user && user->id == alice && user->role == "player");
	TEST_EXPECT(checks() == before + 1);
	before = checks();
	TEST_EXPECT(!nws::authenticate_user(db, "alice", "wrong").has_value());
	TEST_EXPECT(checks() == before + 1);
	before = checks();
	TEST_EXPECT(!nws::authenticate_user(db, "no-such-user", "pw-alice").has_value());
	TEST_EXPECT(checks() == before + 1);
	before = checks();
	TEST_EXPECT(!nws::authenticate_user(db, "no-such-user-2", "x").has_value());
	TEST_EXPECT(checks() == before + 1);
	return 0;
}

// loggable(): a control byte as \xNN (no client string can forge a log line),
// the text cut after max_bytes with "..".
int test_loggable(Database &, int64_t) {
	TEST_EXPECT(nws::loggable("alice") == "alice");
	TEST_EXPECT(nws::loggable("a\nb\r\x7f\x01") == "a\\x0Ab\\x0D\\x7F\\x01");
	TEST_EXPECT(nws::loggable(std::string(64, 'u')) == std::string(64, 'u'));
	TEST_EXPECT(nws::loggable(std::string(65, 'u')) == std::string(64, 'u') + "..");
	const std::string line(300, 'c');
	TEST_EXPECT(nws::loggable(line, line.size()) == line);
	TEST_EXPECT(nws::loggable("abcdef", 3) == "abc..");
	return 0;
}

// A new password or an inactive status ends the account's sessions in the
// same update and moves its credentials generation: after a reset the old
// session is gone, and a ban lifted before the session was ever used again
// revives nothing. Other edits (a new handle, a rename, a lifted ban) keep
// both. An empty status is refused; one stored empty reads as active.
int test_update_revokes_sessions(Database &db, int64_t) {
	const int64_t bob = make_player(db, "bob", "0000b0b0");
	TEST_EXPECT(bob > 0);
	auto update = [&db, bob](nws::UpdateUserParams p) { return nws::update_user(db, bob, p); };
	TEST_EXPECT(generation(db, bob) == 0);

	std::string token = mint(db, bob);
	TEST_EXPECT(!token.empty());
	nws::UpdateUserParams rename;
	rename.nwhandle = "Bobby";
	TEST_EXPECT(update(rename).ok);
	TEST_EXPECT(session_exists(db, token));
	TEST_EXPECT(generation(db, bob) == 0);

	nws::UpdateUserParams reset;
	reset.password_plaintext = "new-password";
	TEST_EXPECT(update(reset).ok);
	TEST_EXPECT(!session_exists(db, token));
	TEST_EXPECT(!nws::resolve_web_session(db, token).has_value());
	TEST_EXPECT(nws::authenticate_user(db, "bob", "new-password").has_value());
	TEST_EXPECT(generation(db, bob) == 1);

	token = mint(db, bob);
	nws::UpdateUserParams ban;
	ban.account_status = "banned";
	TEST_EXPECT(update(ban).ok);
	TEST_EXPECT(!session_exists(db, token));
	TEST_EXPECT(generation(db, bob) == 2);
	nws::UpdateUserParams unban;
	unban.account_status = "active";
	TEST_EXPECT(update(unban).ok);
	TEST_EXPECT(!nws::resolve_web_session(db, token).has_value());
	TEST_EXPECT(generation(db, bob) == 2);

	nws::UpdateUserParams empty;
	empty.account_status = "";
	const auto refused = update(empty);
	TEST_EXPECT(!refused.ok && refused.error_code == "invalid_field");
	token = mint(db, bob);
	db.exec("UPDATE players SET account_status = '' WHERE id = ?;", {BindValue(bob)});
	TEST_EXPECT(nws::resolve_web_session(db, token).has_value());
	TEST_EXPECT(nws::get_user_by_id(db, bob)->account_status == "active");
	return 0;
}

// The username rule, on registration and on a rename: not empty, at most 64
// bytes, no control byte (invalid_field), so every account can log in to the
// site and no stored name carries a byte into a log line or the retail
// browser. The handle rule, on registration and on an update: not empty, at
// most 30 bytes (the longest a stock host reads whole), no control byte.
int test_username_rule(Database &db, int64_t alice) {
	auto create = [&db](const std::string &name, const std::string &handle, const char *pcid) {
		nws::CreateUserParams p;
		p.username = name;
		p.password = "pw";
		p.pcid = pcid;
		p.nwh = "1";
		p.nwhandle = handle;
		return nws::create_user(db, p);
	};
	TEST_EXPECT(create(std::string(64, 'n'), "handle", "0000c001").ok);
	for (const std::string &bad : {std::string(65, 'n'), std::string("line\nbreak"),
	                               std::string("tab\tname"), std::string("del\x7f")}) {
		const auto refused = create(bad, "handle", "0000c002");
		TEST_EXPECT(!refused.ok && refused.error_code == "invalid_field");
		nws::UpdateUserParams rename;
		rename.username = bad;
		const auto renamed = nws::update_user(db, alice, rename);
		TEST_EXPECT(!renamed.ok && renamed.error_code == "invalid_field");
	}
	nws::UpdateUserParams unnamed;
	unnamed.username = "";
	const auto refused_empty = nws::update_user(db, alice, unnamed);
	TEST_EXPECT(!refused_empty.ok && refused_empty.error_code == "invalid_field");
	TEST_EXPECT(nws::get_user_by_id(db, alice)->username == "alice");

	TEST_EXPECT(nws::kHandleMaxBytes == 30);
	TEST_EXPECT(create("handle30", std::string(30, 'h'), "0000c003").ok);
	TEST_EXPECT(create("spaced", "Jim's \"Space\" Handle", "0000c004").ok);
	const std::string kept = nws::get_user_by_id(db, alice)->nwhandle;
	for (const std::string &bad : { std::string(31, 'h'), std::string("line\nbreak"),
				 std::string("cr\rlf"), std::string("nul\0x", 5), std::string("tab\thandle"),
				 std::string("del\x7f") }) {
		const auto refused = create("badhandle", bad, "0000c005");
		TEST_EXPECT(!refused.ok && refused.error_code == "invalid_field");
		nws::UpdateUserParams rehandle;
		rehandle.nwhandle = bad;
		const auto updated = nws::update_user(db, alice, rehandle);
		TEST_EXPECT(!updated.ok && updated.error_code == "invalid_field");
	}
	nws::UpdateUserParams unhandled;
	unhandled.nwhandle = "";
	const auto refused_blank = nws::update_user(db, alice, unhandled);
	TEST_EXPECT(!refused_blank.ok && refused_blank.error_code == "invalid_field");
	TEST_EXPECT(nws::get_user_by_id(db, alice)->nwhandle == kept);
	TEST_EXPECT(!nws::get_user_by_username(db, "badhandle").has_value());
	return 0;
}

// Known login addresses: recorded per account, read by username, kept 30
// days, at most 32 an account (the oldest go), kept through a password reset,
// a status other than 'active' and a rename (read by the new name), and gone
// with the account.
int test_login_addresses(Database &db, int64_t) {
	const int64_t fay = make_player(db, "fay", "0000fa01");
	TEST_EXPECT(fay > 0);
	TEST_EXPECT(!nws::is_known_login_address(db, "fay", "198.51.100.1"));
	remember(db, fay, "198.51.100.1");
	remember(db, fay, "198.51.100.1"); // again: one row
	TEST_EXPECT(nws::is_known_login_address(db, "fay", "198.51.100.1"));
	TEST_EXPECT(!nws::is_known_login_address(db, "fay", "198.51.100.2"));
	TEST_EXPECT(!nws::is_known_login_address(db, "alice", "198.51.100.1"));
	auto count = [&db, fay] {
		return db.query("SELECT COUNT(*) FROM login_addresses WHERE user_id = ?;", {BindValue(fay)})
				.front()
				.as_int(0)
				.value_or(-1);
	};
	TEST_EXPECT(count() == 1);

	// 30 days unused: no longer known, and pruned.
	db.exec("UPDATE login_addresses SET last_success_at = datetime('now', '-31 days') "
	        "WHERE user_id = ?;",
	        {BindValue(fay)});
	TEST_EXPECT(!nws::is_known_login_address(db, "fay", "198.51.100.1"));
	TEST_EXPECT(nws::prune_login_addresses(db) >= 1);
	TEST_EXPECT(count() == 0);

	// The newest 32 stay.
	for (int i = 0; i < 40; ++i)
		remember(db, fay, "10.0.0." + std::to_string(i));
	TEST_EXPECT(count() == static_cast<int64_t>(nws::kLoginAddressesPerUser));
	TEST_EXPECT(!nws::is_known_login_address(db, "fay", "10.0.0.0"));
	TEST_EXPECT(nws::is_known_login_address(db, "fay", "10.0.0.39"));

	// A rename (of the username, the name they are read by) keeps them; so do a
	// reset and a ban.
	const int64_t kept = count();
	nws::UpdateUserParams rename;
	rename.username = "fay-renamed";
	TEST_EXPECT(nws::update_user(db, fay, rename).ok);
	TEST_EXPECT(count() == kept);
	TEST_EXPECT(nws::is_known_login_address(db, "fay-renamed", "10.0.0.39"));
	TEST_EXPECT(!nws::is_known_login_address(db, "fay", "10.0.0.39"));
	TEST_EXPECT(!nws::update_revokes_sessions(rename));
	nws::UpdateUserParams reset;
	reset.password_plaintext = "new";
	TEST_EXPECT(nws::update_revokes_sessions(reset));
	TEST_EXPECT(nws::update_user(db, fay, reset).ok);
	TEST_EXPECT(count() == kept);
	TEST_EXPECT(nws::is_known_login_address(db, "fay-renamed", "10.0.0.39"));
	nws::UpdateUserParams ban;
	ban.account_status = "banned";
	TEST_EXPECT(nws::update_revokes_sessions(ban));
	TEST_EXPECT(nws::update_user(db, fay, ban).ok);
	TEST_EXPECT(count() == kept);
	TEST_EXPECT(nws::is_known_login_address(db, "fay-renamed", "10.0.0.39"));

	// Recorded only at the account's current generation: a login whose
	// password check read the one before the ban records nothing.
	TEST_EXPECT(!nws::record_login_address(db, fay, generation(db, fay) - 1, "198.51.100.3"));
	TEST_EXPECT(!nws::is_known_login_address(db, "fay-renamed", "198.51.100.3"));
	TEST_EXPECT(remember(db, fay, "198.51.100.3"));
	TEST_EXPECT(nws::is_known_login_address(db, "fay-renamed", "198.51.100.3"));

	remember(db, fay, "198.51.100.4");
	TEST_EXPECT(nws::delete_user(db, fay).ok);
	TEST_EXPECT(db.query("SELECT 1 FROM login_addresses WHERE user_id = ?;", {BindValue(fay)}).empty());
	return 0;
}

// The race the credentials generation closes, on the store: a login reads the
// account with its password hash, an admin reset commits while the password
// is checked (the seam runs it inside authenticate_user, after the match), and
// then nothing the login would grant is written: no website session, no
// retail session row, no known address. The next login, at the new
// generation, gets all three; a later reset ends the retail row's tag, and a
// banned account gets neither session at its own generation.
int test_reset_during_login(Database &db, int64_t) {
	const int64_t gil = make_player(db, "gil", "0000913a");
	TEST_EXPECT(gil > 0);
	auto reset = [&db](int64_t id, const char *password) {
		nws::UpdateUserParams p;
		p.password_plaintext = password;
		return nws::update_user(db, id, p).ok;
	};
	auto set_status = [&db](int64_t id, const char *status) {
		nws::UpdateUserParams p;
		p.account_status = status;
		return nws::update_user(db, id, p).ok;
	};
	auto register_row = [&db](int64_t id, int64_t gen, const std::string &tag) {
		return nws::register_active_user_session(db, id, gen, "gil", tag, "", "", "");
	};

	bool reset_ran = false;
	nws::set_password_accepted_hook(
			[&reset, &reset_ran](int64_t id) { reset_ran = reset(id, "pw-after-reset"); });
	const auto stale = nws::authenticate_user(db, "gil", "pw-gil");
	nws::set_password_accepted_hook(nullptr);
	TEST_EXPECT(reset_ran);
	TEST_EXPECT(stale && stale->id == gil && stale->credentials_gen == 0);
	TEST_EXPECT(generation(db, gil) == 1);
	TEST_EXPECT(!nws::create_web_session(db, gil, stale->credentials_gen, "", "").has_value());
	TEST_EXPECT(
			db.query("SELECT 1 FROM web_sessions WHERE user_id = ?;", { BindValue(gil) }).empty());
	TEST_EXPECT(!register_row(gil, stale->credentials_gen, "tag-gil-stale"));
	TEST_EXPECT(!nws::get_user_by_login_session_tag(db, "tag-gil-stale").has_value());
	TEST_EXPECT(!nws::record_login_address(db, gil, stale->credentials_gen, "198.51.100.9"));
	TEST_EXPECT(!nws::is_known_login_address(db, "gil", "198.51.100.9"));

	const auto fresh = nws::authenticate_user(db, "gil", "pw-after-reset");
	TEST_EXPECT(fresh && fresh->credentials_gen == 1);
	TEST_EXPECT(nws::create_web_session(db, gil, fresh->credentials_gen, "", "").has_value());
	TEST_EXPECT(register_row(gil, fresh->credentials_gen, "tag-gil"));
	const auto by_tag = nws::get_user_by_login_session_tag(db, "tag-gil");
	TEST_EXPECT(by_tag && by_tag->id == gil && by_tag->nwhandle == "gil");
	TEST_EXPECT(nws::record_login_address(db, gil, fresh->credentials_gen, "198.51.100.9"));
	TEST_EXPECT(!nws::get_user_by_login_session_tag(db, "").has_value());
	TEST_EXPECT(!nws::get_user_by_login_session_tag(db, "tag-nobody").has_value());

	TEST_EXPECT(reset(gil, "pw-third"));
	TEST_EXPECT(!nws::get_user_by_login_session_tag(db, "tag-gil").has_value());

	TEST_EXPECT(set_status(gil, "banned"));
	TEST_EXPECT(!register_row(gil, generation(db, gil), "tag-gil-banned"));
	TEST_EXPECT(!nws::create_web_session(db, gil, generation(db, gil), "", "").has_value());
	TEST_EXPECT(set_status(gil, "active"));
	TEST_EXPECT(register_row(gil, generation(db, gil), "tag-gil-back"));
	TEST_EXPECT(nws::get_user_by_login_session_tag(db, "tag-gil-back").has_value());
	return 0;
}

// A session whose account is banned behind the store's back (a direct status
// write) is refused and deleted on its next use; a deleted player takes its
// sessions with it.
int test_inactive_and_deleted_accounts(Database &db, int64_t) {
	const int64_t dan = make_player(db, "dan", "0000da01");
	TEST_EXPECT(dan > 0);
	const std::string token = mint(db, dan);
	db.exec("UPDATE players SET account_status = 'disabled' WHERE id = ?;", {BindValue(dan)});
	TEST_EXPECT(!nws::resolve_web_session(db, token).has_value());
	TEST_EXPECT(!session_exists(db, token));

	const int64_t carol = make_player(db, "carol", "0000ca01");
	TEST_EXPECT(carol > 0);
	const std::string carols = mint(db, carol);
	TEST_EXPECT(nws::delete_user(db, carol).ok);
	TEST_EXPECT(!session_exists(db, carols));
	return 0;
}

// A listed server's owner and a listed server's player rows. A password reset
// deletes the account's hosting keys, so its live listing's next refresh stores
// no owner. delete_user succeeds for an account that hosts a listed server and
// sits in another's player list (ON DELETE SET NULL): the listing stays with no
// owner, the player row with no account; a newcomer never gets the deleted id
// (AUTOINCREMENT), and the listing's next refresh, the lobby still holding the
// deleted id, names no one.
int test_listing_owner_revoked_and_deleted(Database &db, int64_t) {
	namespace hostdb = opennova::hostdb;
	auto owner_of = [&db](uint32_t rid) {
		const auto row = hostdb::find_host_by_rid(db, rid);
		return row ? row->host_user_id : std::optional<int64_t>(-1);
	};
	// A listing whose owner holds a claimed key, as the lobby writes it.
	auto list_for = [&db](int64_t owner, uint32_t rid, char letter) {
		const std::string key(hostdb::kHostKeyChars, letter);
		hostdb::record_host_key(db, key, owner);
		hostdb::HostRow row;
		row.rid = rid;
		row.gsid = "GSID-" + std::to_string(rid);
		row.game = "jop_2_consumer";
		row.server_name = "Listing " + std::to_string(rid);
		row.host_ip = "10.0.0.5";
		row.peer_ip = "10.0.0.5";
		row.host_user_id = hostdb::claim_host_key(db, key, rid);
		hostdb::upsert_host(db, row);
		return row;
	};

	const int64_t erin = make_player(db, "erin", "0000e201");
	TEST_EXPECT(erin > 0);
	hostdb::HostRow reset_row = list_for(erin, 0x0A0000E1u, 'E');
	TEST_EXPECT(owner_of(reset_row.rid) == erin);
	nws::UpdateUserParams reset;
	reset.password_plaintext = "pw-erin-2";
	TEST_EXPECT(nws::update_user(db, erin, reset).ok);
	TEST_EXPECT(db.query("SELECT 1 FROM host_keys WHERE user_id = ?;", {BindValue(erin)}).empty());
	TEST_EXPECT(owner_of(reset_row.rid) == erin); // until the listing's next refresh
	hostdb::update_host(db, reset_row);           // the lobby still holds erin
	TEST_EXPECT(owner_of(reset_row.rid) == std::nullopt);

	// An account hosts a listed server and has joined another.
	const int64_t gus = make_player(db, "gus", "0000a901");
	TEST_EXPECT(gus > 0);
	hostdb::HostRow gus_row = list_for(gus, 0x0A0000E2u, 'G');
	TEST_EXPECT(owner_of(gus_row.rid) == gus);
	hostdb::PlayerRow joined;
	joined.host_rid = reset_row.rid;
	joined.user_id = gus;
	joined.nwhandle = "gus";
	joined.peer_ip = "10.0.0.9";
	joined.peer_port = 0; // /NWJoin.dll's, which no ClientHostPlayerRemoved matches
	hostdb::add_player(db, joined);

	TEST_EXPECT(nws::delete_user(db, gus).ok);
	TEST_EXPECT(!nws::get_user_by_id(db, gus).has_value());
	TEST_EXPECT(owner_of(gus_row.rid) == std::nullopt);
	const auto players = hostdb::list_players(db, reset_row.rid);
	TEST_EXPECT(players.size() == 1 && !players[0].user_id.has_value());

	// A newcomer never takes the deleted id; the deleted owner's listing
	// refreshes (the lobby still holds gus's id) and names no one.
	const int64_t newcomer = make_player(db, "newcomer", "0000a902");
	TEST_EXPECT(newcomer > gus);
	hostdb::update_host(db, gus_row);
	TEST_EXPECT(owner_of(gus_row.rid) == std::nullopt);
	hostdb::upsert_host(db, gus_row);
	TEST_EXPECT(owner_of(gus_row.rid) == std::nullopt);
	TEST_EXPECT(hostdb::list_hosts_by_owner(db, gus).empty());

	hostdb::remove_host_by_rid(db, reset_row.rid);
	hostdb::remove_host_by_rid(db, gus_row.rid);
	return 0;
}

// ONNET_BOOTSTRAP_ADMIN: promotes the named account only while no account is
// an admin, so it can neither re-promote a demoted admin while another admin
// exists nor promote a name registered after the first admin was made. The
// role is read live by every session lookup; the admin update takes only
// player or admin.
int test_bootstrap_and_roles(Database &db, int64_t alice) {
	using Outcome = nws::BootstrapAdminResult::Outcome;
	auto role_of = [&db](int64_t id) {
		const auto user = nws::get_user_by_id(db, id);
		return user ? user->role : std::string("(none)");
	};
	auto session_role = [&db](const std::string &token) {
		const auto session = nws::resolve_web_session(db, token);
		return session ? session->user.role : std::string("(none)");
	};
	const std::string token = mint(db, alice);
	TEST_EXPECT(role_of(alice) == "player");
	TEST_EXPECT(nws::bootstrap_admin(db, "nobody").outcome == Outcome::NoSuchAccount);
	TEST_EXPECT(nws::bootstrap_admin(db, "alice").outcome == Outcome::Promoted);
	TEST_EXPECT(role_of(alice) == "admin");
	TEST_EXPECT(session_role(token) == "admin");

	const int64_t eve = make_player(db, "eve", "0000e7e0");
	TEST_EXPECT(eve > 0);
	auto again = nws::bootstrap_admin(db, "eve");
	TEST_EXPECT(again.outcome == Outcome::AdminExists);
	TEST_EXPECT(again.existing_admin == "alice");
	TEST_EXPECT(role_of(eve) == "player");

	// Demoting alice while another admin exists: a later boot leaves her be.
	TEST_EXPECT(db.query("SELECT 1 FROM players WHERE role = 'admin';").size() == 1);
	nws::UpdateUserParams promote_eve;
	promote_eve.role = "admin";
	TEST_EXPECT(nws::update_user(db, eve, promote_eve).ok);
	nws::UpdateUserParams demote;
	demote.role = "player";
	TEST_EXPECT(nws::update_user(db, alice, demote).ok);
	TEST_EXPECT(session_role(token) == "player");
	TEST_EXPECT(nws::bootstrap_admin(db, "alice").outcome == Outcome::AdminExists);
	TEST_EXPECT(role_of(alice) == "player");

	nws::UpdateUserParams bad;
	bad.role = "superuser";
	const auto refused = nws::update_user(db, eve, bad);
	TEST_EXPECT(!refused.ok && refused.error_code == "invalid_field");
	TEST_EXPECT(role_of(eve) == "admin");
	return 0;
}

// Requests racing on one stale session, each on its own connection to a WAL
// file: every one resolves the account, and exactly one slides the expiry
// (the UPDATE repeats the staleness test, so the others write nothing).
int test_slide_race() {
	test_temp::TempDir temp("web_session_race");
	opennova::db::ConnectionPool pool(temp.path / "race.db");
	int64_t id = 0;
	std::string token;
	{
		auto conn = pool.acquire();
		opennova::db::run_migrations(*conn, kSourceDir / "backend" / "migrations");
		id = make_player(*conn, "racer", "0000ace0");
		TEST_EXPECT(id > 0);
		token = mint(*conn, id);
		conn->exec("UPDATE web_sessions SET last_seen_at = datetime('now', '-120 seconds'), "
		           "expires_at = datetime('now', '+1 days') WHERE token_hash = ?;",
		           {BindValue(nws::web_session_token_hash(token))});
	}
	constexpr int kRacers = 8;
	std::atomic<int> ready{0};
	std::atomic<int> slid{0};
	std::atomic<int> resolved{0};
	std::vector<std::thread> racers;
	for (int i = 0; i < kRacers; ++i) {
		racers.emplace_back([&] {
			auto conn = pool.acquire();
			ready.fetch_add(1);
			while (ready.load() < kRacers) std::this_thread::yield();
			const auto session = nws::resolve_web_session(*conn, token);
			if (session && session->user.id == id) resolved.fetch_add(1);
			if (session && session->slid) slid.fetch_add(1);
		});
	}
	for (auto &t : racers) t.join();
	TEST_EXPECT(resolved.load() == kRacers);
	TEST_EXPECT(slid.load() == 1);
	return 0;
}

} // namespace

int main() {
	Database db(":memory:");
	opennova::db::run_migrations(db, kSourceDir / "backend" / "migrations");
	const int64_t alice = make_player(db, "alice", "0000a11c");
	TEST_EXPECT(alice > 0);

	struct Case { const char *name; int (*fn)(Database &, int64_t); };
	const Case cases[] = {
		{"create_keeps_only_the_hash", test_create_keeps_only_the_hash},
		{"sessions_per_account_capped", test_sessions_per_account_capped},
		{"resolve_and_refusals", test_resolve_and_refusals},
		{"sliding_expiry", test_sliding_expiry},
		{"delete_and_prune", test_delete_and_prune},
		{"authenticate", test_authenticate},
		{"loggable", test_loggable},
		{"update_revokes_sessions", test_update_revokes_sessions},
		{"username_rule", test_username_rule},
		{"login_addresses", test_login_addresses},
		{"reset_during_login", test_reset_during_login},
		{"inactive_and_deleted_accounts", test_inactive_and_deleted_accounts},
		{"bootstrap_and_roles", test_bootstrap_and_roles},
		{"listing_owner_revoked_and_deleted", test_listing_owner_revoked_and_deleted},
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn(db, alice) != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("-- slide_race\n");
	if (test_slide_race() != 0) {
		std::fprintf(stderr, "FAIL: slide_race\n");
		return 1;
	}
	std::printf("OK: web sessions + roles\n");
	return 0;
}
