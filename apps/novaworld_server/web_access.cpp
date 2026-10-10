#include "web_access.h"

#include "http_cookies.h"
#include "server_config.h"

#include <cstdio>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// The longest User-Agent a web session records.
constexpr size_t kUserAgentMaxBytes = 512;
// The IPv6 grouping of the address-keyed buckets and of known login addresses
// (a subscriber's LAN), and of the (username, address) buckets (a
// subscriber's whole delegation).
constexpr int kAddressV6Bits = 64;
constexpr int kPairV6Bits = 56;

// GET, HEAD and OPTIONS change nothing; every other method does.
bool is_state_changing(const crow::request &req) {
	return req.method != crow::HTTPMethod::Get && req.method != crow::HTTPMethod::Head &&
	       req.method != crow::HTTPMethod::Options;
}

// The route families' bucket-key prefixes: the website's and the retail
// login's address-keyed buckets never share a key.
const char *route_prefix(LoginRoute route) {
	return route == LoginRoute::Web ? "web|" : "nw|";
}

// What /api/me and /api/login answer with: the account a session acts as.
crow::json::wvalue session_user_json(int64_t id, const std::string &username,
                                     const std::string &role) {
	crow::json::wvalue out;
	out["id"] = id;
	out["username"] = username;
	out["role"] = role;
	return out;
}

} // namespace

void AccessMiddleware::after_handle(crow::request &req, crow::response &res, context &ctx) {
	if (!ctx.set_cookie.empty()) res.add_header("Set-Cookie", ctx.set_cookie);
	if (!ctx.admin_actor.empty()) {
		const std::string method = crow::method_name(req.method);
		std::printf("[http] admin %s %s by %s -> %d\n", method.c_str(), loggable(req.url).c_str(),
		            ctx.admin_actor.c_str(), res.code);
	}
}

crow::response too_many_requests(int64_t wait) {
	crow::response res = json_error(429, "rate_limited");
	res.set_header("Retry-After", std::to_string(wait));
	return res;
}

bool WebAccess::has_csrf_header(const crow::request &req) {
	return req.get_header_value(kCsrfHeader) == "1";
}

void WebAccess::configure(const ServerConfig &config) {
	admin_token_ = config.admin_api_token;
	cookie_secure_ = config.cookie_secure;
	cookie_name_ = cookie_secure_ ? kWebSessionHostCookie : kWebSessionCookie;
	trusted_proxies_ = config.trusted_proxies;
	std::printf("[http] admin api: Bearer token %s; admin-role sessions accepted\n",
	            admin_token_.empty() ? "DISABLED (set ADMIN_API_TOKEN to enable)" : "ENABLED");
	std::printf("[http] session cookie %s%s; %zu trusted proxy address(es)\n", cookie_name_.c_str(),
	            cookie_secure_ ? " (Secure)" : " (not Secure: set ONNET_COOKIE_SECURE=1 for https)",
	            trusted_proxies_.size());
}

std::string WebAccess::client_ip(const crow::request &req) const {
	return resolve_client_ip(req.remote_ip_address, req.get_header_value("X-Real-IP"),
	                         req.get_header_value("X-Forwarded-For"), trusted_proxies_);
}

std::string WebAccess::session_cookie(const std::string &token, int64_t max_age) const {
	std::string cookie = cookie_name_ + "=" + token + "; Path=/; Max-Age=" +
	                     std::to_string(max_age) + "; HttpOnly; SameSite=Lax";
	if (cookie_secure_) cookie += "; Secure";
	return cookie;
}

std::string WebAccess::session_token(const crow::request &req) const {
	const auto cookies = parse_cookie_header(request_cookie_header(req));
	const auto it = cookies.find(cookie_name_);
	return it == cookies.end() ? std::string() : it->second;
}

bool WebAccess::bearer_authorized(const crow::request &req) const {
	if (admin_token_.empty()) return false;
	const std::string auth = req.get_header_value("Authorization");
	if (auth.rfind("Bearer ", 0) != 0) return false;
	const std::string presented = auth.substr(7);
	if (presented.size() != admin_token_.size()) return false;
	unsigned diff = 0;
	for (size_t i = 0; i < presented.size(); ++i) {
		diff |= static_cast<unsigned>(presented[i]) ^ static_cast<unsigned>(admin_token_[i]);
	}
	return diff == 0;
}

RouteAccess WebAccess::require_user(const crow::request &req) {
	RouteAccess access;
	const std::string token = session_token(req);
	if (token.empty()) {
		access.refusal = json_error(401, "unauthorized");
		return access;
	}
	if (is_state_changing(req) && !has_csrf_header(req)) {
		access.refusal = json_error(403, "csrf_header_required");
		return access;
	}
	std::optional<WebSessionLookup> session;
	try {
		session = resolve_web_session(*pool_.acquire(), token);
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] WARN web session lookup failed: %s\n", e.what());
		access.refusal = json_error(500, "db_error");
		return access;
	}
	if (!session) {
		access.refusal = json_error(401, "unauthorized");
		access.refusal->add_header("Set-Cookie", session_cookie("", 0));
		return access;
	}
	if (session->slid) {
		app_.get_context<AccessMiddleware>(req).set_cookie =
				session_cookie(token, kWebSessionLifetimeSeconds);
	}
	access.user = std::move(session->user);
	return access;
}

RouteAccess WebAccess::require_admin(const crow::request &req) {
	RouteAccess access = bearer_authorized(req) ? RouteAccess{} : require_user(req);
	if (access.refusal) {
		if (access.refusal->code == 401) {
			access.refusal->set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
		}
		return access;
	}
	if (access.user && access.user->role != "admin") {
		access.refusal = json_error(403, "forbidden");
		access.user.reset();
		return access;
	}
	if (is_state_changing(req)) {
		app_.get_context<AccessMiddleware>(req).admin_actor =
				access.user ? "session '" + loggable(access.user->username) + "'" : std::string("token");
	}
	return access;
}

int64_t WebAccess::take_login_address(LoginRoute route, const std::string &ip) {
	return login_per_ip_.take(route_prefix(route) + address_key(ip, kAddressV6Bits),
	                          RateLimiter::Clock::now());
}

int64_t WebAccess::begin_password_check(LoginRoute route, const std::string &username,
                                        const std::string &ip, LoginTicket &ticket) {
	const auto now = RateLimiter::Clock::now();
	const std::string name = username.substr(0, kUsernameMaxBytes);
	ticket = LoginTicket{};
	ticket.pair_key = route_prefix(route) + name + "\n" + address_key(ip, kPairV6Bits);
	ticket.known_address = address_key(ip, kAddressV6Bits);
	if (const int64_t wait = login_failures_per_user_ip_.take(ticket.pair_key, now)) return wait;
	bool known = false;
	try {
		known = is_known_login_address(*pool_.acquire(), name, ticket.known_address);
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] WARN login address lookup failed: %s\n", e.what());
	}
	if (!known) {
		if (const int64_t wait = login_failures_per_user_.take(name, now)) {
			login_failures_per_user_ip_.refund(ticket.pair_key, now);
			return wait;
		}
		ticket.username_key = name;
	}
	return 0;
}

void WebAccess::password_accepted(const LoginTicket &ticket, int64_t user_id) {
	const auto now = RateLimiter::Clock::now();
	login_failures_per_user_ip_.refund(ticket.pair_key, now);
	if (!ticket.username_key.empty()) login_failures_per_user_.refund(ticket.username_key, now);
	try {
		record_login_address(*pool_.acquire(), user_id, ticket.known_address);
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] WARN login address record failed: %s\n", e.what());
	}
}

int64_t WebAccess::take_register(const std::string &ip) {
	return register_per_ip_.take("web|" + address_key(ip, kAddressV6Bits),
	                             RateLimiter::Clock::now());
}

// POST /api/login — the website's login. Body JSON {username, password}; the
// X-OpenNova-Request header is required here too, against login CSRF (a
// cross-site form posting the attacker's credentials would otherwise sign the
// victim's browser into the attacker's account), and is checked first, so a
// forged request draws on no bucket. A body that is no JSON object, a missing
// or mistyped field and a username over kUsernameMaxBytes are 400s before any
// brake. Then the login brake (429 + Retry-After): the address bucket, then
// one token from the username's failure buckets, handed back when the
// password is right. An unknown username and a wrong password get the same
// 401, cost the same bcrypt run and keep the same tokens spent; only the
// right password learns that an account is banned or restricted (403, as the
// game login's NWEC11 / NWEC12). Success mints a session (the session cookie)
// and answers {id, username, role}; the session the browser held before, if
// any, is ended.
crow::response WebAccess::login(const crow::request &req) {
	if (!has_csrf_header(req)) return json_error(403, "csrf_header_required");
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const std::string username = body->text_or("username", "");
	const std::string password = body->text_or("password", "");
	if (body->wrong_type()) return json_error(400, "invalid_field");
	if (username.empty() || password.empty()) return json_error(400, "missing_field");
	if (username.size() > kUsernameMaxBytes) return json_error(400, "invalid_field");

	const std::string ip = client_ip(req);
	if (const int64_t wait = take_login_address(LoginRoute::Web, ip)) {
		std::printf("[http] /api/login rate-limited for %s\n", loggable(ip).c_str());
		return too_many_requests(wait);
	}
	LoginTicket ticket;
	if (const int64_t wait = begin_password_check(LoginRoute::Web, username, ip, ticket)) {
		std::printf("[http] /api/login rate-limited for user '%s' from %s\n",
		            loggable(username).c_str(), loggable(ip).c_str());
		return too_many_requests(wait);
	}

	try {
		auto db_conn = pool_.acquire();
		const auto user = authenticate_user(*db_conn, username, password);
		if (!user) {
			std::printf("[http] /api/login refused from %s: bad credentials\n",
			            loggable(ip).c_str());
			return json_error(401, "invalid_credentials");
		}
		password_accepted(ticket, user->id);
		if (user->account_status != "active") {
			std::printf("[http] /api/login refused: user %lld status=%s\n",
			            static_cast<long long>(user->id), loggable(user->account_status).c_str());
			return json_error(403, user->account_status == "banned" ? "account_banned"
			                                                         : "account_restricted");
		}
		delete_web_session(*db_conn, session_token(req));
		const std::string token = create_web_session(
				*db_conn, user->id, ip,
				req.get_header_value("User-Agent").substr(0, kUserAgentMaxBytes));
		std::printf("[http] /api/login -> session for user '%s' (id=%lld role=%s) from %s\n",
		            loggable(user->username).c_str(), static_cast<long long>(user->id),
		            user->role.c_str(), loggable(ip).c_str());
		crow::response res =
				json_reply(200, session_user_json(user->id, user->username, user->role));
		res.add_header("Set-Cookie", session_cookie(token, kWebSessionLifetimeSeconds));
		res.set_header("Cache-Control", "no-store");
		return res;
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] /api/login failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
}

// POST /api/logout — ends the session the cookie names and deletes the cookie.
// Idempotent: with no live session it still answers 204 and clears the
// cookie. A state change, so the X-OpenNova-Request header is required (else a
// cross-site page could sign the user out).
crow::response WebAccess::logout(const crow::request &req) {
	if (!has_csrf_header(req)) return json_error(403, "csrf_header_required");
	try {
		delete_web_session(*pool_.acquire(), session_token(req));
	} catch (const db::SqliteError &e) {
		std::fprintf(stderr, "[http] /api/logout failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::response res(204);
	res.add_header("Set-Cookie", session_cookie("", 0));
	return res;
}

void WebAccess::register_routes() {
	CROW_ROUTE(app_, "/api/login").methods("POST"_method)(
	    [this](const crow::request &req) { return login(req); });
	CROW_ROUTE(app_, "/api/logout").methods("POST"_method)(
	    [this](const crow::request &req) { return logout(req); });

	// GET /api/me — the logged-in account, {id, username, role}, or 401.
	CROW_ROUTE(app_, "/api/me")([this](const crow::request &req) {
		auto access = require_user(req);
		if (access.refusal) return std::move(*access.refusal);
		crow::response res = json_reply(
				200, session_user_json(access.user->id, access.user->username, access.user->role));
		res.set_header("Cache-Control", "no-store");
		return res;
	});
}

} // namespace opennova::novaworld_server
