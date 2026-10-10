#pragma once

// Who an /api request comes from: the website's login sessions, the CSRF
// header, the admin role and the Bearer ADMIN_API_TOKEN, the credential routes'
// rate limits (the login brake the retail POST /NWLogin.dll runs too), and the
// routes that open and close a session (POST /api/login, POST /api/logout,
// GET /api/me). The HttpListener owns one WebAccess over its Crow app; every
// /api/admin/* handler opens with require_admin, and a session-authenticated
// site route with require_user.

#include "auth.h"
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

// The per-request work WebAccess leaves for after the handler: the renewed
// session cookie of a request whose session the store slid forward
// (WebSessionLookup::slid), whatever route answered it, and the admin
// state change require_admin let through, logged with its actor once the
// handler has answered (so the line carries the outcome).
struct AccessMiddleware {
	struct context {
		std::string set_cookie;
		std::string admin_actor; // "token" or "session '<username>'"
	};
	void before_handle(crow::request &, crow::response &, context &) {}
	void after_handle(crow::request &req, crow::response &res, context &ctx);
};

// The HttpListener's Crow app.
using WebApp = crow::App<AccessMiddleware>;

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

// Which login a brake bucket belongs to. The two keep their address-keyed
// buckets apart, so an address the website login is braked by (an X-Real-IP
// nginx took from a CF-Connecting-IP, which a Cloudflare Worker can set to
// any value) never brakes the retail login, which keys on the TCP peer (a
// stock client reaches :8080 directly, through no proxy).
enum class LoginRoute { Web, Retail };

// The failure buckets one password check drew from (begin_password_check):
// what password_accepted hands back.
struct LoginTicket {
	std::string pair_key;      // the (username, address) bucket's key
	std::string username_key;  // the per-username bucket's key; empty when not drawn
	std::string known_address; // the address key login_addresses remembers
};

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

	// The login brake, per RateLimiter bucket. An IPv6 address counts as its
	// /64 for the address bucket and its /56 for the (username, address) one.
	// Every credentialed attempt, per route and client address: 20 at once,
	// then one every 3 s (a LAN party behind one address can still all log in
	// at once).
	static constexpr RateLimiter::Params kLoginPerIp{20, 1.0 / 3, 10000};
	// Each password check, per route, username and client address: 10, then
	// one every 30 s, the token handed back when the password was right. A
	// guesser at one address runs dry; the owner, elsewhere, never meets it.
	static constexpr RateLimiter::Params kLoginFailuresPerUserIp{10, 1.0 / 30, 10000};
	// Each password check per username, from any address and either route,
	// except from an address the account logged in from in the last 30 days
	// (login_addresses): 30, then one every 2 s, handed back on success. Caps a
	// guesser spread over many addresses at 30 a minute; it can lock out only
	// logins from addresses the account has not used.
	static constexpr RateLimiter::Params kLoginFailuresPerUser{30, 1.0 / 2, 10000};
	// POST /api/register per client address (/64): 10, then one every 3 min.
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
	// the Bearer challenge. A state change it lets through is logged with its
	// actor and the reply's status (AccessMiddleware).
	RouteAccess require_admin(const crow::request &req);

	// The login brake both logins run. take_login_address draws the attempt
	// from the route's address bucket (0, else the Retry-After seconds).
	// begin_password_check draws one token from the route's (username,
	// address) bucket and, unless the address is one the account logged in
	// from lately, one from the username's bucket, before the password is
	// checked (so concurrent checks never pass a spent bucket): 0 with the
	// ticket, or the Retry-After seconds with nothing drawn. password_accepted
	// hands the ticket's tokens back and remembers the address for the
	// account; a wrong password keeps them spent.
	int64_t take_login_address(LoginRoute route, const std::string &ip);
	int64_t begin_password_check(LoginRoute route, const std::string &username,
	                             const std::string &ip, LoginTicket &ticket);
	void password_accepted(const LoginTicket &ticket, int64_t user_id);

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
