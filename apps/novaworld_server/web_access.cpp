#include "web_access.h"

#include "auth.h"
#include "http_cookies.h"
#include "server_config.h"

#include <cstdio>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// The longest username a per-username bucket key keeps, so a huge name in a
// login body cannot grow the table's memory.
constexpr size_t kRateKeyMaxBytes = 128;
// The longest User-Agent a web session records.
constexpr size_t kUserAgentMaxBytes = 512;

// GET, HEAD and OPTIONS change nothing; every other method does.
bool is_state_changing(const crow::request &req) {
	return req.method != crow::HTTPMethod::Get && req.method != crow::HTTPMethod::Head &&
	       req.method != crow::HTTPMethod::Options;
}

bool has_csrf_header(const crow::request &req) {
	return req.get_header_value(WebAccess::kCsrfHeader) == "1";
}

// The session token the request's cookie carries, "" when it carries none.
std::string session_token(const crow::request &req) {
	const auto cookies = parse_cookie_header(request_cookie_header(req));
	const auto it = cookies.find(kWebSessionCookie);
	return it == cookies.end() ? std::string() : it->second;
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

crow::response json_reply(int code, const crow::json::wvalue &body) {
	crow::response res(code);
	res.body = body.dump();
	res.set_header("Content-Type", "application/json");
	return res;
}

crow::response json_error(int code, const char *error) {
	crow::json::wvalue body;
	body["error"] = error;
	return json_reply(code, body);
}

crow::response too_many_requests(int64_t wait) {
	crow::response res = json_error(429, "rate_limited");
	res.set_header("Retry-After", std::to_string(wait));
	return res;
}

void WebAccess::configure(const ServerConfig &config) {
	admin_token_ = config.admin_api_token;
	cookie_secure_ = config.cookie_secure;
	trusted_proxies_ = config.trusted_proxies;
	std::printf("[http] admin api: Bearer token %s; admin-role sessions accepted\n",
	            admin_token_.empty() ? "DISABLED (set ADMIN_API_TOKEN to enable)" : "ENABLED");
	std::printf("[http] session cookie %s; %zu trusted proxy address(es)\n",
	            cookie_secure_ ? "Secure" : "not Secure (set ONNET_COOKIE_SECURE=1 for https)",
	            trusted_proxies_.size());
}

std::string WebAccess::client_ip(const crow::request &req) const {
	return resolve_client_ip(req.remote_ip_address, req.get_header_value("X-Real-IP"),
	                         req.get_header_value("X-Forwarded-For"), trusted_proxies_);
}

std::string WebAccess::session_cookie(const std::string &token, int64_t max_age) const {
	std::string cookie = std::string(kWebSessionCookie) + "=" + token +
	                     "; Path=/; Max-Age=" + std::to_string(max_age) +
	                     "; HttpOnly; SameSite=Lax";
	if (cookie_secure_) cookie += "; Secure";
	return cookie;
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
		app_.get_context<SessionCookieRenewal>(req).set_cookie =
				session_cookie(token, kWebSessionLifetimeSeconds);
	}
	access.user = std::move(session->user);
	return access;
}

RouteAccess WebAccess::require_admin(const crow::request &req) {
	if (bearer_authorized(req)) return {};
	RouteAccess access = require_user(req);
	if (access.refusal) {
		if (access.refusal->code == 401) {
			access.refusal->set_header("WWW-Authenticate", "Bearer realm=\"opennova-admin\"");
		}
		return access;
	}
	if (access.user->role != "admin") {
		access.refusal = json_error(403, "forbidden");
		access.user.reset();
	}
	return access;
}

int64_t WebAccess::take_register(const std::string &ip) {
	return register_per_ip_.take(ip, RateLimiter::Clock::now());
}

// POST /api/login — the website's login. Body JSON {username, password}; the
// X-OpenNova-Request header is required here too, against login CSRF (a
// cross-site form posting the attacker's credentials would otherwise sign the
// victim's browser into the attacker's account). Rate-limited per client
// address and per username (429 + Retry-After). An unknown username and a
// wrong password get the same 401 and cost the same bcrypt run; only the right
// password learns that an account is banned or restricted (403, as the game
// login's NWEC11 / NWEC12). Success mints a session (the opennova_session
// cookie) and answers {id, username, role}; the session the browser held
// before, if any, is ended.
crow::response WebAccess::login(const crow::request &req) {
	if (!has_csrf_header(req)) return json_error(403, "csrf_header_required");
	const std::string ip = client_ip(req);
	const auto now = RateLimiter::Clock::now();
	if (const int64_t wait = login_per_ip_.take(ip, now)) {
		std::printf("[http] /api/login rate-limited for %s\n", ip.c_str());
		return too_many_requests(wait);
	}
	const auto body = crow::json::load(req.body);
	if (!body) return json_error(400, "invalid_json");
	auto text_field = [&body](const char *key) {
		return body.has(key) && body[key].t() == crow::json::type::String
		               ? std::string(body[key].s())
		               : std::string();
	};
	const std::string username = text_field("username");
	const std::string password = text_field("password");
	if (username.empty() || password.empty()) return json_error(400, "missing_field");
	if (const int64_t wait = login_per_user_.take(username.substr(0, kRateKeyMaxBytes), now)) {
		std::printf("[http] /api/login rate-limited for user '%s'\n", username.c_str());
		return too_many_requests(wait);
	}

	try {
		auto db_conn = pool_.acquire();
		const auto user = authenticate_user(*db_conn, username, password);
		if (!user) {
			std::printf("[http] /api/login refused from %s: bad credentials\n", ip.c_str());
			return json_error(401, "invalid_credentials");
		}
		if (user->account_status != "active") {
			std::printf("[http] /api/login refused: user %lld status=%s\n",
			            static_cast<long long>(user->id), user->account_status.c_str());
			return json_error(403, user->account_status == "banned" ? "account_banned"
			                                                         : "account_restricted");
		}
		delete_web_session(*db_conn, session_token(req));
		const std::string token = create_web_session(
				*db_conn, user->id, ip,
				req.get_header_value("User-Agent").substr(0, kUserAgentMaxBytes));
		std::printf("[http] /api/login -> session for user '%s' (id=%lld role=%s) from %s\n",
		            user->username.c_str(), static_cast<long long>(user->id), user->role.c_str(),
		            ip.c_str());
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
