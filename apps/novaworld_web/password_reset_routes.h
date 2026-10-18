#pragma once

// The password reset routes (README "Account email and password resets"):
// POST /api/auth/forgot mails a reset link to an account's address, POST
// /api/auth/reset spends the link's token on a new password. Both are the
// site's own state changes, so each needs the CSRF header (WebAccess::kCsrfHeader)
// before anything else. The HttpListener owns one beside its other route
// families. Service policy, not a port.

#include "accounts/mailer.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <functional>
#include <string>

namespace opennova::novaworld_server {

class PasswordResetRoutes {
public:
	// The rest of a reset's revocation (HttpListener's forget_revoked_account:
	// the retail login's pins, the lobby's view of the account's listings), on
	// the handler's connection.
	using ForgetAccount = std::function<void(db::Database &db, int64_t user_id)>;

	// Both routes per client address (/64): 10, then one every 3 min.
	static constexpr RateLimiter::Params kPerIp{ "reset_ip", 10, 1.0 / 180 };
	// POST /api/auth/forgot per email address, whether or not an account has
	// it: 3, then one every 20 min, so nobody can fill a mailbox with links.
	static constexpr RateLimiter::Params kPerEmail{ "reset_email", 3, 1.0 / 1200 };

	PasswordResetRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis) :
			access_(access), pool_(pool), per_ip_(redis, kPerIp), per_email_(redis, kPerEmail) {}
	PasswordResetRoutes(const PasswordResetRoutes &) = delete;
	PasswordResetRoutes &operator=(const PasswordResetRoutes &) = delete;

	// `mail_on`: the links are queued (accounts/mailer.h), else logged as not sent;
	// `site_url` is the site's origin the links name (ONNET_SITE_URL).
	void register_routes(WebApp &app, bool mail_on, std::string site_url, ForgetAccount forget);

private:
	crow::response forgot(const crow::request &req);
	crow::response reset(const crow::request &req);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	bool mail_on_ = false;
	std::string site_url_;
	ForgetAccount forget_;
	RateLimiter per_ip_;
	RateLimiter per_email_;
};

} // namespace opennova::novaworld_server
