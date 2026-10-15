#pragma once

// The hosted servers' stats routes (README "Hosted servers"): one server's
// stats for its owner (GET /api/me/servers/<id>/stats) and for an admin (GET
// /api/admin/hosted/<id>/stats), an account's servers together (GET
// /api/me/servers/stats), and the whole fleet's (GET /api/admin/hosted/stats),
// each over ?range=1h|24h|7d|30d (stats_repository.h; 24h by default), as
// {"server_id" (one server's alone), "range", "live", "summary", "series"}.
// Reads only, answered Cache-Control: no-store, each caller's reads drawn
// from a bucket of their own. Another account's server is a 404, never a 403.
// Every reply is an error code with our own message, never database text.
// Service policy, not a port.

#include "admin_link.h"
#include "hosted_repository.h"
#include "rate_limiter.h"
#include "stats_repository.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::hosted {

class StatsRoutes {
public:
	// Each caller's stats reads (an account, or the Bearer token as one): 60 at
	// once, then two a second (a page polls a few charts).
	static constexpr RateLimiter::Params kStatsReadPerUser{ 60, 2.0, 10000 };
	// How old the newest sample may be for the live view's CPU and memory.
	static constexpr int64_t kLiveSampleMaxAge = 3 * 60;

	StatsRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	StatsRoutes(const StatsRoutes &) = delete;
	StatsRoutes &operator=(const StatsRoutes &) = delete;

	// `links` (the admin links, null with none) gives the live view its scene
	// and link state; it must outlive the routes.
	void register_routes(WebApp &app, const AdminLinks *links);

private:
	crow::response me_server(const crow::request &req, uint64_t id);
	crow::response me_servers(const crow::request &req);
	crow::response admin_server(const crow::request &req, uint64_t id);
	crow::response admin_fleet(const crow::request &req);
	// The reply for `scope` over the request's range, with `server`'s live view
	// for one server's stats.
	crow::response reply(const crow::request &req, const StatsScope &scope, const HostedServer *server);
	// The caller's bucket (429 with Retry-After when it is spent).
	std::optional<crow::response> take(const RouteAccess &access);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	const AdminLinks *links_ = nullptr;
	RateLimiter reads_{ kStatsReadPerUser };
};

} // namespace opennova::novaworld_server::hosted
