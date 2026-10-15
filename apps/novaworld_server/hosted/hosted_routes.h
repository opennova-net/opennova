#pragma once

// The hosted servers' /api routes (ADR 0053, proposed; README "Hosted servers"):
// the owner's, by server id (an account runs as many as its hosting
// entitlement allows): GET /api/me/servers, GET /api/me/servers/<id>, POST
// /api/me/servers (a start) and POST /api/me/servers/<id>/stop (require_user,
// the CSRF header on the two POSTs; another account's id is 404, never 403),
// and the admin's fleet view
// GET /api/admin/hosted, its kill switch POST /api/admin/hosted/stop-all and
// POST /api/admin/hosted/<id>/stop (require_admin). They write requests and
// stops only (hosted_repository.h); the supervisor does the rest. Every reply
// is an error code with our own message, never database text. Behind
// ONNET_API_PROXY_ONLY like every /api route. A start needs the account's
// hosting entitlement (billing/entitlement.h); an admin's comp grant, PUT
// /api/admin/users/<id>/comp-servers, sits with the admin user routes
// (http_listener.cpp). The HttpListener owns one HostedRoutes beside its
// HostRoutes.

#include "hosted_repository.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

struct ServerConfig;

namespace hosted {

// A hosted server as JSON for its owner (GET /api/me/servers and
// /api/me/servers/<id>, the start and stop replies, an admin's user page) or,
// with `admin`, for the fleet view: its owner, its service account and its
// machine besides. Never its admin password. Without its events.
crow::json::wvalue server_json(db::Database &db, const HostedServer &s, bool admin);

class HostedRoutes {
public:
	// Each owner start or stop, per account: 6 at once, then one every 20 s
	// (service policy: a start costs a machine).
	static constexpr RateLimiter::Params kStartStopPerUser{ 6, 1.0 / 20, 10000 };
	// The transitions GET /api/me/servers/<id> shows, the ended servers GET
	// /api/me/servers lists after the live ones, and the servers the fleet view
	// lists.
	static constexpr size_t kEventsShown = 50;
	static constexpr size_t kEndedShown = 10;
	static constexpr size_t kFleetShown = 100;

	HostedRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	HostedRoutes(const HostedRoutes &) = delete;
	HostedRoutes &operator=(const HostedRoutes &) = delete;

	// Takes ONNET_HOSTED_PROVIDER, ONNET_HOSTED_CAP and the data sets the
	// provider offers (data_sets.h: a start's game, and its rotation when the
	// request names none) from the config; hosting is on with a provider and
	// at least one offered data set. Before register_routes().
	void configure(const ServerConfig &config);
	void register_routes(WebApp &app);

private:
	crow::response me_servers(const crow::request &req);
	crow::response me_server(const crow::request &req, uint64_t id);
	crow::response me_start(const crow::request &req);
	crow::response me_stop(const crow::request &req, uint64_t id);
	crow::response admin_fleet(const crow::request &req);
	crow::response admin_stop_all(const crow::request &req);
	crow::response admin_stop(const crow::request &req, uint64_t id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	bool enabled_ = false;
	std::string provider_ = "none";
	int cap_ = 5;
	std::vector<DataSet> data_sets_;
	RateLimiter start_stop_per_user_{ kStartStopPerUser };
};

} // namespace hosted
} // namespace opennova::novaworld_server
