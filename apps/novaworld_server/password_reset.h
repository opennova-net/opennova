#pragma once

// The password reset links (password_resets, 0005_account_email.sql), the store
// behind POST /api/auth/forgot and POST /api/auth/reset
// (password_reset_routes.h). Crow-free. Service policy, not a port.
//
// A link carries a token of kPasswordResetTokenBytes from the OS CSPRNG
// (base/os_random), spelled as 64 lower-case hex digits, the website session
// token's form; the table keeps only its SHA-256 (web_session_token_hash), so a
// copy of the database opens no account. A token lives
// kPasswordResetLifetimeSeconds and works once; an account holds at most one,
// the newest.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace opennova::novaworld_server {

inline constexpr int64_t kPasswordResetLifetimeSeconds = 60 * 60;
inline constexpr size_t kPasswordResetTokenBytes = 32;

// Mints a reset token for `user_id` and returns it (what the link carries).
// The account's earlier token, if any, is replaced: only the newest link
// works. Throws db::Error.
std::string issue_password_reset(db::Database &db, int64_t user_id);

// Spends `token`: deletes its row and answers the account it was minted for,
// while it is live. nullopt for a malformed or unknown token, one spent
// already, and one past its hour (deleted too). Of two claims racing on one
// token only one gets the account. Throws db::Error.
std::optional<int64_t> claim_password_reset(db::Database &db, std::string_view token);

// Deletes every token past its expiry and returns how many went. The main
// tick's sweep calls it.
size_t prune_password_resets(db::Database &db);

} // namespace opennova::novaworld_server
