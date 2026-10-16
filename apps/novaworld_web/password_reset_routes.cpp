#include "password_reset_routes.h"

#include "auth.h"
#include "http_json.h"
#include "loggable.h"
#include "password_reset.h"

#include <cstdio>
#include <optional>
#include <utility>

namespace opennova::novaworld_server {

namespace {

// The address buckets' IPv6 grouping (a subscriber's LAN), as the login's.
constexpr int kAddressV6Bits = 64;

crow::response no_store(int code) {
	crow::response res(code);
	res.set_header("Cache-Control", "no-store");
	return res;
}

} // namespace

void PasswordResetRoutes::register_routes(
		WebApp &app, bool mail_on, std::string site_url, ForgetAccount forget) {
	mail_on_ = mail_on;
	site_url_ = std::move(site_url);
	forget_ = std::move(forget);
	CROW_ROUTE(app, "/api/auth/forgot").methods("POST"_method)(
			[this](const crow::request &req) { return forgot(req); });
	CROW_ROUTE(app, "/api/auth/reset").methods("POST"_method)(
			[this](const crow::request &req) { return reset(req); });
}

// POST /api/auth/forgot {email}. The CSRF header first, then the per-address
// brake (429 + Retry-After), the body (400 invalid_json, invalid_field for a
// mistyped field or an address normalize_email refuses, missing_field), then
// the per-email brake, which counts every address whether or not an account
// has it. Then 202 with no body, the same for every address: when an active
// player's account has it, its reset link is minted (any older one dropped)
// and queued as a mail.send job, which a worker sends off the request's thread, so the
// reply's timing is the same too. A database failure is logged and answered
// 202 alike.
crow::response PasswordResetRoutes::forgot(const crow::request &req) {
	if (!WebAccess::has_csrf_header(req)) return json_error(403, "csrf_header_required");
	const std::string ip = access_.client_ip(req);
	const auto now = RateLimiter::Clock::now();
	if (const int64_t wait = per_ip_.take("forgot|" + address_key(ip, kAddressV6Bits), now)) {
		std::printf("[http] /api/auth/forgot rate-limited for %s\n", loggable(ip).c_str());
		return too_many_requests(wait);
	}
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const std::string raw = body->text_or("email", "");
	if (body->wrong_type()) return json_error(400, "invalid_field");
	if (raw.empty()) return json_error(400, "missing_field");
	const auto email = normalize_email(raw);
	if (!email) return json_error(400, "invalid_field", "email is not an email address");
	if (const int64_t wait = per_email_.take(*email, now)) {
		std::printf("[http] /api/auth/forgot rate-limited for an address, from %s\n", loggable(ip).c_str());
		return too_many_requests(wait);
	}
	try {
		auto db_conn = pool_.acquire();
		const auto user = get_user_by_email(*db_conn, *email);
		if (user && user->account_status == "active") {
			const std::string token = issue_password_reset(*db_conn, user->id);
			queue_mail(*db_conn, mail_on_,
					password_reset_mail(
							*email, user->username, password_reset_url(site_url_, token)));
			std::printf("[http] /api/auth/forgot -> reset link for user %lld, from %s\n",
					static_cast<long long>(user->id), loggable(ip).c_str());
		} else {
			std::printf("[http] /api/auth/forgot from %s: no active account has the address\n", loggable(ip).c_str());
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] /api/auth/forgot failed: %s\n", e.what());
	}
	return no_store(202);
}

// POST /api/auth/reset {token, password}. The CSRF header first, then the
// per-address brake, the body (400 invalid_json, invalid_field, missing_field).
// The token is spent before the password is hashed (claim_password_reset: a
// token works once, and a wrong one costs no bcrypt run): 400 invalid_token
// for a token that is malformed, unknown, spent or past its hour. Then the
// password is set the way an admin reset sets it (update_user): the account's
// credentials generation moves, and its website sessions, its retail login's
// session row, its hosting keys and (forget) its retail login's pins end. 204.
crow::response PasswordResetRoutes::reset(const crow::request &req) {
	if (!WebAccess::has_csrf_header(req)) return json_error(403, "csrf_header_required");
	const std::string ip = access_.client_ip(req);
	if (const int64_t wait =
					per_ip_.take("reset|" + address_key(ip, kAddressV6Bits), RateLimiter::Clock::now())) {
		std::printf("[http] /api/auth/reset rate-limited for %s\n", loggable(ip).c_str());
		return too_many_requests(wait);
	}
	auto body = JsonBody::parse(req.body);
	if (!body) return json_error(400, "invalid_json");
	const std::string token = body->text_or("token", "");
	const std::string password = body->text_or("password", "");
	if (body->wrong_type()) return json_error(400, "invalid_field");
	if (token.empty() || password.empty()) return json_error(400, "missing_field");
	std::optional<int64_t> user_id;
	try {
		auto db_conn = pool_.acquire();
		user_id = claim_password_reset(*db_conn, token);
		if (!user_id) {
			std::printf("[http] /api/auth/reset refused from %s: no live token\n", loggable(ip).c_str());
			return json_error(400, "invalid_token");
		}
		UpdateUserParams update;
		update.password_plaintext = password;
		const MutationResult result = update_user(*db_conn, *user_id, update);
		if (!result.ok) {
			// not_found: the account went (or is a service account) since the link.
			if (result.error_code == "not_found") return json_error(400, "invalid_token");
			return json_error(500, "db_error");
		}
		if (forget_) forget_(*db_conn, *user_id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] /api/auth/reset failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	std::printf("[http] /api/auth/reset -> new password for user %lld, from %s\n", static_cast<long long>(*user_id),
			loggable(ip).c_str());
	return no_store(204);
}

} // namespace opennova::novaworld_server
