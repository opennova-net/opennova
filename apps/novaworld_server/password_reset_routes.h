#pragma once

// The password reset routes (README "Account email and password resets"):
// POST /api/auth/forgot mails a reset link to an account's address, POST
// /api/auth/reset spends the link's token on a new password. Both are the
// site's own state changes, so each needs the CSRF header (WebAccess::kCsrfHeader)
// before anything else. The HttpListener owns one beside its other route
// families. Service policy, not a port.

#include "mailer.h"
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
	// The in-memory half of a reset's revocation (HttpListener's
	// forget_revoked_account: the retail login's pins).
	using ForgetAccount = std::function<void(int64_t user_id)>;

	// Both routes per client address (/64): 10, then one every 3 min.
	static constexpr RateLimiter::Params kPerIp{10, 1.0 / 180, 10000};
	// POST /api/auth/forgot per email address, whether or not an account has
	// it: 3, then one every 20 min, so nobody can fill a mailbox with links.
	static constexpr RateLimiter::Params kPerEmail{3, 1.0 / 1200, 10000};

	PasswordResetRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	PasswordResetRoutes(const PasswordResetRoutes &) = delete;
	PasswordResetRoutes &operator=(const PasswordResetRoutes &) = delete;

	// `outbox` (null: mail off) must outlive the routes; `site_url` is the
	// site's origin the links name (ONNET_SITE_URL).
	void register_routes(WebApp &app, MailOutbox *outbox, std::string site_url, ForgetAccount forget);

private:
	crow::response forgot(const crow::request &req);
	crow::response reset(const crow::request &req);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	MailOutbox *outbox_ = nullptr;
	std::string site_url_;
	ForgetAccount forget_;
	RateLimiter per_ip_{kPerIp};
	RateLimiter per_email_{kPerEmail};
};

} // namespace opennova::novaworld_server
