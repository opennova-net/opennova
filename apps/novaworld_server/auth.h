#pragma once

#include <novaworld/db/sqlite.h>

#include <optional>
#include <string>
#include <vector>

namespace opennova::server {

// Resolved player record returned by authenticate_user / get_user_by_*.
struct UserRecord {
	int64_t     id = 0;
	std::string username;
	std::string pcid;
	std::string nwh;
	std::string nwhandle;
	std::string account_status = "active";
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
// `error_code` is one of: "username_exists", "pcid_exists",
// "missing_field", "not_found", "db_error". Used as the body
// foundation for /api/admin/users.* HTTP responses.
struct MutationResult {
	bool        ok = false;
	int64_t     id = 0;
	std::string error_code;
	std::string error_message;
};

// Look up `username` and verify `password` against the stored
// `password_hash`. Returns the record on success, nullopt on missing
// user / wrong password / DB error.
//
// `password_hash` should be a bcrypt $2a$/$2b$ string (32+ chars). For
// dev convenience, plaintext password_hash is also accepted but logs a
// warning each time so it's obvious when seed data hasn't been
// regenerated. Mirrors onnw/auth.py::authenticate_user.
std::optional<UserRecord> authenticate_user(opennova::db::Database &db,
                                            const std::string &username,
                                            const std::string &password);

// Look up a user without verifying a password. Used by /NWJoin.dll's
// PUB encoding which only needs the joiner's PCID, and by the
// PERSISTENTEXPRESSLOGINDATA → user_id pin recovery path.
std::optional<UserRecord> get_user_by_username(opennova::db::Database &db,
                                                const std::string &username);
std::optional<UserRecord> get_user_by_id(opennova::db::Database &db, int64_t id);

// All seeded players in `id` order. Used by the dev-mode round-robin
// fallback in POST /NWLogin.dll when the request didn't supply an
// EPASK form field (curl walkthroughs).
std::vector<UserRecord> list_dev_players(opennova::db::Database &db);

// All registered players, stripped of password_hash, ordered by id.
// Powers GET /api/admin/users.
std::vector<UserRecord> list_users(opennova::db::Database &db);

std::optional<GameAccessRecord> get_game_access(opennova::db::Database &db,
                                                int64_t user_id,
                                                const std::string &game_slug);

ServerStatusRecord get_server_status(opennova::db::Database &db);
MutationResult update_server_status(opennova::db::Database &db,
                                    bool maintenance_enabled,
                                    const std::string &message);

bool has_active_user_session(opennova::db::Database &db, int64_t user_id);
void register_active_user_session(opennova::db::Database &db, int64_t user_id,
                                  const std::string &username,
                                  const std::string &session_tag,
                                  const std::string &persistent_id,
                                  const std::string &remote_ip,
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
};

MutationResult create_user(opennova::db::Database &db, const CreateUserParams &p);
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
};
MutationResult update_user(opennova::db::Database &db, int64_t id,
                           const UpdateUserParams &p);

struct UpdateGameAccessParams {
	std::string game_slug;
	std::string status;
	std::string exp_bits;
};
MutationResult update_game_access(opennova::db::Database &db, int64_t user_id,
                                  const UpdateGameAccessParams &p);

} // namespace opennova::server
