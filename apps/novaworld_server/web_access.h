#pragma once

// Who an /api request comes from: the website's login sessions, the CSRF
// header, the admin role and the Bearer ADMIN_API_TOKEN, the credential routes'
// rate limits (the login brake the retail POST /NWLogin.dll shares), and the
// routes that open and close a session (POST /api/login, POST /api/logout,
// GET /api/me). The HttpListener owns one WebAccess over its Crow app; every
// /api/admin/* handler opens with require_admin, and a session-authenticated
// site route with require_user.

#include "http_json.h"
#include "rate_limiter.h"
#include "web_session.h"

#include <net/novaworld/db/sqlite.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

struct ServerConfig;

// Renews the session cookie on the reply to any request whose session the
// store slid forward (WebSessionLookup::slid), whatever route answered it:
// WebAccess::require_user leaves the Set-Cookie text in this middleware's
// per-request context, and after_handle adds it.
struct SessionCookieRenewal {
	struct context {
		std::string set_cookie;
	};
	void before_handle(crow::request &, crow::response &, context &) {}
	void after_handle(crow::request &, crow::response &res, context &ctx) {
		if (!ctx.set_cookie.empty()) res.add_header("Set-Cookie", ctx.set_cookie);
	}
};

// The HttpListener's Crow app.
using WebApp = crow::App<SessionCookieRenewal>;

// The account a request acts as, or the response that refuses it: what
// require_user and require_admin return.
struct RouteAccess {
	// Set: the route sends this and does nothing else.
	std::optional<crow::response> refusal;
	// The session's account; empty when the Bearer ADMIN_API_TOKEN granted
	// access (a machine caller).
	std::optional<WebSessionUser> user;
};

// 429 {"error":"rate_limited"} with Retry-After: `wait` seconds.
crow::response too_many_requests(int64_t wait);

class WebAccess {
public:
	// The header every state-changing request a session cookie authenticates
	// must carry, with the value "1", and POST /api/register too. A cross-site
	// form cannot set a custom header, and a cross-origin fetch that sets one
	// needs a CORS preflight this server never grants (it sends no
	// Access-Control-* header), so a request bearing it came from the site's
	// own pages. SameSite=Lax already keeps the cookie off cross-site POSTs;
	// the header also covers same-site origins (game.<domain>) and the routes
	// no cookie authenticates.
	static constexpr const char *kCsrfHeader = "X-OpenNova-Request";
	static bool has_csrf_header(const crow::request &req);

	// The longest username POST /api/login takes (400 past it, before any
	// brake); the brake keys cut the retail login's names to the same length.
	static constexpr size_t kUsernameMaxBytes = 64;

	// The login brake, per RateLimiter bucket.
	// Every credentialed attempt, per client address: 20 at once, then one
	// every 3 s (a LAN party behind one address can still all log in at once).
	static constexpr RateLimiter::Params kLoginPerIp{20, 1.0 / 3, 10000};
	// Failed attempts only, per (username, client address): 10, then one every
	// 30 s. A guesser at one address runs dry; the account's owner, at another
	// address, never meets it.
	static constexpr RateLimiter::Params kLoginFailuresPerUserIp{10, 1.0 / 30, 10000};
	// Failed attempts only, per username from any address: 30, then one every
	// 2 s. Caps a guesser spread over many addresses; one address alone (10
	// failures, then two a minute) can neither empty nor hold it empty.
	static constexpr RateLimiter::Params kLoginFailuresPerUser{30, 1.0 / 2, 10000};
	// POST /api/register per client address: 10, then one every 3 min.
	static constexpr RateLimiter::Params kRegisterPerIp{10, 1.0 / 180, 10000};

	WebAccess(WebApp &app, db::ConnectionPool &pool) : app_(app), pool_(pool) {}

	WebAccess(const WebAccess &) = delete;
	WebAccess &operator=(const WebAccess &) = delete;

	// Takes the policy from the config (ADMIN_API_TOKEN, ONNET_COOKIE_SECURE,
	// ONNET_TRUSTED_PROXIES) and logs it. Before any route runs.
	void configure(const ServerConfig &config);

	// Registers POST /api/login, POST /api/logout and GET /api/me.
	void register_routes();

	// The client address, through a trusted proxy's X-Real-IP /
	// X-Forwarded-For (resolve_client_ip).
	std::string client_ip(const crow::request &req) const;

	// The logged-in account the session cookie names. Refused with 401 when
	// the request carries no live session (a dead cookie is deleted on the
	// way), and with 403 when the request changes state (any method but GET,
	// HEAD, OPTIONS) without the CSRF header. A session the lookup slid
	// renews its cookie on the reply.
	RouteAccess require_user(const crow::request &req);

	// An admin route's caller: the Bearer ADMIN_API_TOKEN (machine callers and
	// the deploy toolbox; no CSRF header, since a browser never attaches it on
	// its own), or else an admin-role session under require_user's rules. A
	// player's session is refused with 403; no credential at all with 401 and
	// the Bearer challenge. A granted state change is logged with its actor
	// ("token" or the session's username).
	RouteAccess require_admin(const crow::request &req);

	// The login brake both logins share, POST /api/login and the retail POST
	// /NWLogin.dll, so a guesser gains nothing by switching between them.
	// take_login_address draws the attempt from the client address's bucket;
	// login_wait reads the username's two failure buckets before the password
	// is checked (0, else the Retry-After seconds) and login_failed drains them
	// after a wrong one. A right password costs them nothing.
	int64_t take_login_address(const std::string &ip);
	int64_t login_wait(const std::string &username, const std::string &ip);
	void login_failed(const std::string &username, const std::string &ip);

	// POST /api/register's per-address brake: 0, or the Retry-After seconds.
	int64_t take_register(const std::string &ip);

private:
	// The Set-Cookie text for the session cookie: `token` for `max_age`
	// seconds, or (an empty token, max_age 0) the deletion of it.
	std::string session_cookie(const std::string &token, int64_t max_age) const;
	// The session token the request carries under this server's cookie name,
	// "" when it carries none.
	std::string session_token(const crow::request &req) const;
	// "Authorization: Bearer <ADMIN_API_TOKEN>", compared in constant time.
	// Never true while no token is configured.
	bool bearer_authorized(const crow::request &req) const;

	crow::response login(const crow::request &req);
	crow::response logout(const crow::request &req);

	WebApp &app_;
	db::ConnectionPool &pool_;
	std::string admin_token_;
	bool cookie_secure_ = false;
	std::string cookie_name_ = kWebSessionCookie; // kWebSessionHostCookie when Secure
	std::vector<std::string> trusted_proxies_;
	RateLimiter login_per_ip_{kLoginPerIp};
	RateLimiter login_failures_per_user_ip_{kLoginFailuresPerUserIp};
	RateLimiter login_failures_per_user_{kLoginFailuresPerUser};
	RateLimiter register_per_ip_{kRegisterPerIp};
};

} // namespace opennova::novaworld_server
