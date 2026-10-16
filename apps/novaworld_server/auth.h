#pragma once

#include "loggable.h"

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

// The longest username an account may have (create_user, update_user's
// rename) and POST /api/login takes.
inline constexpr size_t kUsernameMaxBytes = 64;

// The longest handle (players.nwhandle) an account may have (create_user,
// update_user). A stock host reads its own login's handle, the CHAR cookie,
// into a 32-byte buffer with an output size of 31, and the copy takes the
// handle's NUL only when the handle is 30 bytes or fewer, so a longer one
// reaches the host unterminated [orig: NapiNPServer_HandleNewConnection
// @0x4c8238..0x4c8249, outputSize 0x1F; the copy, sub_64EA90 @0x64eb3e..0x64eb59].
inline constexpr size_t kHandleMaxBytes = 30;

// The longest email address an account may have (RFC 5321's path limit, less
// its angle brackets).
inline constexpr size_t kEmailMaxBytes = 254;

// The address `raw` names, in the one form players.email stores and every
// lookup compares: its ASCII letters lowercased. nullopt unless it is at most
// kEmailMaxBytes bytes with exactly one '@', a local part, and a domain that
// holds a '.' with a label on each side of every one; no space and no control
// byte anywhere. Service policy, not a port: a check of the shape alone, the
// mail sent to it being the proof.
std::optional<std::string> normalize_email(std::string_view raw);

// Resolved player record returned by authenticate_user / get_user_by_*.
struct UserRecord {
	int64_t     id = 0;
	std::string username;
	std::string pcid;
	std::string nwh;
	std::string nwhandle;
	std::string account_status = "active";
	std::string role = "player"; // players.role: "player" or "admin"
	// players.kind: "player" for an account a person registered,
	// "service" for a hosted server's own account (srv-<id>), which the website's
	// login refuses and the admin users list hides.
	std::string kind = "player";
	// players.comp_servers: the hosted servers an admin granted the account
	// (0..16); its hosting entitlement is the larger of this and its support
	// tier's servers (billing/entitlement.h).
	int comp_servers = 0;
	// players.credentials_gen, bumped by every update that ends
	// the account's logins (update_revokes_sessions). What a login grants (a
	// website session, a retail login's session row and pin, a known address)
	// records the generation its password check read and counts only while the
	// account still has it.
	int64_t credentials_gen = 0;
	// players.email, normalized (normalize_email); empty for an account that
	// has none (one an admin made without one, a service account, an account
	// older than the column).
	std::string email;
};

struct GameAccessRecord {
	int64_t id = 0;
	std::string game_slug;
	std::string status = "active";
	std::string exp_bits;
};

struct ServerStatusRecord {
	bool maintenance_enabled = false;
	std::string message;
};

// Result of a CRUD mutation. ok=true means success; on failure,
// `error_code` is one of: "username_exists", "pcid_exists", "email_exists",
// "missing_field", "invalid_field", "not_found", "db_error". Used as the body
// foundation for /api/admin/users.* HTTP responses, so `error_message` is
// client-facing: a "db_error" leaves it empty, its text going to the server
// log alone.
struct MutationResult {
	bool        ok = false;
	int64_t     id = 0;
	std::string error_code;
	std::string error_message;
};

// Look up `username` and verify `password` against the stored
// `password_hash`. Returns the record on success, nullopt on missing
// user / wrong password / DB error. A missing user still costs one bcrypt
// run (against a hash no password matches), so the answer's timing does not
// tell an unknown username from a wrong password. The account's status is the
// caller's to check: a banned account with the right password is returned.
// Its credentials_gen is the one read with the hash, before bcrypt ran; what
// the caller then grants takes it (create_web_session,
// register_active_user_session, record_login_address), so a reset that
// commits while the password is being checked grants nothing.
//
// `password_hash` should be a bcrypt $2a$/$2b$ string (32+ chars). For
// dev convenience, plaintext password_hash is also accepted but logs a
// warning each time so it's obvious when seed data hasn't been
// regenerated. Mirrors onnw/auth.py::authenticate_user.
std::optional<UserRecord> authenticate_user(opennova::db::Database &db,
                                            const std::string &username,
                                            const std::string &password);

// Look up a user without verifying a password: the account a username (or a
// handle) names, for the duplicate checks of a create and a rename, and the
// account a PERSISTENTEXPRESSLOGINDATA pin names. A handle is no credential:
// no route takes a request's identity from the NWHANDLE cookie.
std::optional<UserRecord> get_user_by_username(opennova::db::Database &db,
                                                const std::string &username);
std::optional<UserRecord> get_user_by_id(opennova::db::Database &db, int64_t id);
// The account whose live retail login set `session_tag` (the LOGINSESSIONTAG
// POST /NWLogin.dll mints after a password check or a pin resume): its
// active_user_sessions row, until a logout, the 2 h idle sweep, a boot, the
// account's next login or a revoking update ends it, and only while the
// account is still at the credentials_gen the row was made under, so a
// password reset or a status other than 'active' ends it even where nothing
// deleted the row. nullopt for an empty or unknown tag. The account's status
// is the caller's to check. Throws db::Error.
std::optional<UserRecord> get_user_by_login_session_tag(
		opennova::db::Database &db, const std::string &session_tag);

// All registered players, stripped of password_hash, ordered by id; a hosted
// server's service account is no player and is left out. Powers GET
// /api/admin/users.
std::vector<UserRecord> list_users(opennova::db::Database &db);

std::optional<GameAccessRecord> get_game_access(opennova::db::Database &db,
                                                int64_t user_id,
                                                const std::string &game_slug);

ServerStatusRecord get_server_status(opennova::db::Database &db);
MutationResult update_server_status(opennova::db::Database &db,
                                    bool maintenance_enabled,
                                    const std::string &message);

// The retail login's session row (one per account; a later login replaces
// it), written only while the account is active and still at
// `credentials_gen`, the generation its password check (or pin) read, both
// tested by the insert itself: false, with nothing written, once a reset or a
// status change has committed since. Throws db::Error.
bool register_active_user_session(opennova::db::Database &db, int64_t user_id,
		int64_t credentials_gen, const std::string &username, const std::string &session_tag,
		const std::string &persistent_id, const std::string &remote_ip,
		const std::string &user_agent);
void touch_active_user_session(opennova::db::Database &db, int64_t user_id);
void clear_active_user_session(opennova::db::Database &db, int64_t user_id);
void clear_active_user_session_by_tag(opennova::db::Database &db,
                                      const std::string &session_tag);
void clear_all_active_user_sessions(opennova::db::Database &db);
std::size_t evict_active_user_sessions_older_than(opennova::db::Database &db,
                                                  int max_age_seconds);

// Generate a bcrypt $2b$<cost>$ hash of `plain`. Cost defaults to 10
// (matches Python bcrypt.gensalt default). Throws std::runtime_error on
// vendored-bcrypt failure (rare — typically ENOMEM or malformed cost).
std::string hash_password(const std::string &plain, int cost = 10);

struct CreateUserParams {
	std::string username;
	std::string password;   // plaintext; we'll bcrypt-hash it
	std::string pcid;       // 8-hex-char
	std::string nwh;        // typically "1"
	std::string nwhandle;
	// The account's email address: empty for none (an admin's create may
	// leave it out; POST /api/register requires it). Stored normalized
	// (normalize_email).
	std::string email;
};

// A username of at most kUsernameMaxBytes bytes and a handle of at most
// kHandleMaxBytes, neither holding a control byte nor starting with the service
// accounts' prefix (is_service_username), else invalid_field; an email that
// normalize_email refuses is invalid_field too, and one another account has is
// email_exists.
MutationResult create_user(opennova::db::Database &db, const CreateUserParams &p);
// Deletes the account with its sessions and hosting keys (ON DELETE CASCADE).
// A server it has listed stays listed with no owner, and its rows in listed
// servers' player lists stay with no account (ON DELETE SET NULL); the
// listing's later refreshes store no owner, the claim having gone with the
// keys. POST /api/admin/hosts/<rid>/stop ends such a listing. A hosted
// server's service account is not_found here (delete_service_account is the
// supervisor's).
MutationResult delete_user(opennova::db::Database &db, int64_t id);

// Optional fields (only those passed-in by the admin) are updated.
// `password_plaintext` if non-empty triggers a fresh bcrypt hash.
struct UpdateUserParams {
	std::optional<std::string> username;
	std::optional<std::string> password_plaintext;
	std::optional<std::string> pcid;
	std::optional<std::string> nwh;
	std::optional<std::string> nwhandle;
	std::optional<std::string> account_status;
	std::optional<std::string> role; // "player" or "admin", else invalid_field
};
// A rename and a new handle take create_user's rules, and neither may be empty
// (invalid_field). An update that sets a password or a status other than
// 'active' bumps the account's credentials_gen and ends its website sessions,
// its retail login's active_user_sessions row and its hosting keys (host_keys)
// in the same transaction (update_revokes_sessions). Its retail login's pins
// count only at the generation they were made under (the HTTP layer drops
// them as well). Its known login addresses stay: forgetting them would bare
// the owner to the per-username cap just when someone else is guessing, and
// the per-(username, address) bucket still brakes a guesser at one of them. A
// server the account has listed stays listed and loses its owner at its next
// refresh (the claim went with the keys). A rename or a new handle starting
// with the service accounts' prefix is invalid_field, and a hosted server's
// service account is not_found (the supervisor alone sets its password).
MutationResult update_user(opennova::db::Database &db, int64_t id,
                           const UpdateUserParams &p);
bool update_revokes_sessions(const UpdateUserParams &p);

// The addresses an account logged in from with the right password, kept 30
// days (login_addresses): the login brake's per-username cap
// spares them, so a stranger's failed logins cannot keep the owner out.
// `address` is the brake's address key (rate_limiter.h address_key, an IPv6
// address grouped to its /64). record_login_address remembers one only while
// the account is still at `credentials_gen`, the generation its password check
// read (false, nothing written, otherwise). Each throws db::Error on a
// database failure.
inline constexpr int kLoginAddressDays = 30;
inline constexpr int64_t kLoginAddressSeconds = int64_t{ kLoginAddressDays } * 24 * 60 * 60;
inline constexpr size_t kLoginAddressesPerUser = 32;
bool record_login_address(opennova::db::Database &db, int64_t user_id, int64_t credentials_gen,
		const std::string &address);
bool is_known_login_address(opennova::db::Database &db, const std::string &username,
                            const std::string &address);
size_t prune_login_addresses(opennova::db::Database &db);

// ONNET_BOOTSTRAP_ADMIN (main() calls it at boot): gives the account named
// `username` the admin role, once. While any account is an admin it does
// nothing, so a later boot can neither re-promote a deliberately demoted
// admin nor promote whoever registered the name after the first admin was
// made. Throws db::Error on a database failure.
struct BootstrapAdminResult {
	enum class Outcome { Promoted, AdminExists, NoSuchAccount };
	Outcome outcome = Outcome::NoSuchAccount;
	std::string existing_admin; // AdminExists: one admin's username
};
BootstrapAdminResult bootstrap_admin(opennova::db::Database &db, const std::string &username);

// Every bcrypt password check authenticate_user has run in this process: the
// test seam that pins the unknown-username path to one check like a wrong
// password's.
uint64_t password_verifications();

// The test seam between a password check and what it grants: when set,
// authenticate_user calls it with the account's id once the password matched,
// just before returning, so a test can commit a password reset in that window
// (the race credentials_gen closes). Unset in the server.
void set_password_accepted_hook(std::function<void(int64_t user_id)> hook);

// --- Hosted servers' accounts (players.kind, players.comp_servers) ---

// The prefix of every service account's username and handle: "srv-" and the
// hosted server's id. Reserved, case aside, so no person registers or renames
// into it.
inline constexpr const char *kServiceUsernamePrefix = "srv-";
bool is_service_username(std::string_view name);
std::string service_username(int64_t hosted_server_id);

// Sets an admin's grant of hosted servers to a player (players.comp_servers):
// invalid_field outside 0..16 (billing/tiers.h kMaxCompServers), not_found for an
// unknown id and for a service account; a database failure is db_error.
MutationResult set_comp_servers(opennova::db::Database &db, int64_t id, int comp_servers);

// The supervisor's account for a hosted server: creates the service account
// `username` (kind 'service', its handle the same, a random free PCID) with
// `password`, or, when a service account by that name exists, sets its password
// the way an admin reset does (its credentials generation moves, and its retail
// login's session row and its hosting keys go). The account's id, or a failed
// MutationResult: username_exists when a player holds the name (the prefix is
// reserved, so only a database written by hand gets there).
MutationResult upsert_service_account(
		opennova::db::Database &db, const std::string &username, const std::string &password);
// Deletes a service account with its sessions and keys (a listing it still
// holds keeps no owner). not_found for an id that is no service account.
MutationResult delete_service_account(opennova::db::Database &db, int64_t id);

struct UpdateGameAccessParams {
	std::string game_slug;
	std::string status;
	std::string exp_bits;
};
MutationResult update_game_access(opennova::db::Database &db, int64_t user_id,
                                  const UpdateGameAccessParams &p);

} // namespace opennova::novaworld_server
