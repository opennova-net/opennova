#pragma once

// The hosted servers' /api routes (ADR 0053, proposed;
// apps/novaworld_common/hosted/README.md "Routes"):
// the owner's, by server id (an account runs as many as its hosting
// entitlement allows): GET /api/me/servers, GET /api/me/servers/<id>, POST
// /api/me/servers (a start), POST /api/me/servers/<id>/stop and POST
// /api/me/servers/<id>/update (the owner's move to its data sets' current
// data, ADR 0064; require_user, the CSRF header on the POSTs; another
// account's id is 404, never 403), and the admin's kill switch POST
// /api/admin/hosted/stop-all and POST /api/admin/hosted/<id>/stop
// (require_admin; the fleet view, the machines' routes and update-all are
// fleet_routes.h's). They write requests, stops and update requests only
// (hosted_repository.h, data_set_repository.h); the supervisor does the rest.
// Every reply is an error code with our own message, never database text.
// Behind ONNET_API_PROXY_ONLY like every /api route. A start needs the
// account's hosting entitlement (billing/entitlement.h); an admin's comp
// grants, PUT /api/admin/users/<id>/comp, sit with the admin user
// routes (http_listener.cpp). The HttpListener owns one HostedRoutes beside
// its HostRoutes.

#include "hosted/model/hosted_repository.h"
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
// /api/me/servers/<id>, the start, stop and update replies, an admin's user
// page) or, with `admin`, for the fleet view (fleet_routes.h): its owner (with
// its support tier), its service account, its container and machine address,
// its place in the fleet (machine_ref, slot, game_port, generation,
// serve_image), its pins and whether it is outdated, its last minute of ticks
// (`tick`: {p99_ms, late} from its slot's report, null with none) and its
// listing's players besides. Never its admin password. Without its events. The owner's
// views carry its phase and why its last run ended besides (phase_json).
// With `zone` (the play zone, "" for none), its address's hostname too.
crow::json::wvalue server_json(db::Database &db, const HostedServer &s, bool admin, const std::string &zone = {});

// Where server `s`'s start stands and why its last run ended (server_phase.h),
// read from its rows at `now_ms` (Unix milliseconds: the launch backoff's
// clock) into `e`: "phase" {name, and with downloading_data bytes_done and
// bytes_total, with restarting restart, restart_limit and cause (crash or
// timeout), with
// data_unavailable for an admin (`admin`) line, the agent's reason},
// "stopped_because" ({cause, limit, exit_code, oom, line} for a stopped or failed
// server whose last run ended, else null; the agent's reason a start never ran
// is an admin's line alone).
void put_phase(crow::json::wvalue &e, db::Database &db, const HostedServer &s, int64_t now_ms,
		bool admin);

// An offered data set (data_set_repository.h) as GET /api/me/servers lists it:
// {id, display_name, title, version, missions [{file, title}], limits, expansions
// [{id, code, display_name, version, missions}]}.
crow::json::wvalue offered_data_set_json(const OfferedDataSet &d);

class HostedRoutes {
public:
	// Each owner start or stop, per account: 6 at once, then one every 20 s
	// (service policy: a start costs a machine).
	static constexpr RateLimiter::Params kStartStopPerUser{ "hosted_start_stop", 6, 1.0 / 20 };
	// The transitions GET /api/me/servers/<id> shows, and the ended servers GET
	// /api/me/servers lists after the live ones.
	static constexpr size_t kEventsShown = 50;
	static constexpr size_t kEndedShown = 10;

	HostedRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis) :
			access_(access), pool_(pool), start_stop_per_user_(redis, kStartStopPerUser) {}

	HostedRoutes(const HostedRoutes &) = delete;
	HostedRoutes &operator=(const HostedRoutes &) = delete;

	// Takes ONNET_HOSTED_PROVIDER (the machine driver; hosting_setup made it
	// "none" without an object store) and ONNET_HOSTED_CAP from the config, and
	// the fleet's placement policy (fleet_config.h: the start's at_capacity and
	// the list's `full` ask the supervisor's planner). Hosting is on with a
	// driver and, at each request, at least one offered data set (a base with a
	// ready current tree, data_set_repository.h offered_data_sets). Before
	// register_routes().
	void configure(const ServerConfig &config, const PlacementPolicy &fleet);
	// The play zone the static addresses are named in (the servers' address
	// hostname); before register_routes().
	void set_address_zone(std::string zone) { address_zone_ = std::move(zone); }
	void register_routes(WebApp &app);

	// What configure decided (the fleet routes report them): a driver, its
	// name, the cap.
	bool hosting_on() const { return hosting_on_; }
	const std::string &provider() const { return provider_; }
	int cap() const { return cap_; }

private:
	crow::response me_servers(const crow::request &req);
	crow::response me_server(const crow::request &req, uint64_t id);
	crow::response me_start(const crow::request &req);
	crow::response me_stop(const crow::request &req, uint64_t id);
	crow::response me_update(const crow::request &req, uint64_t id);
	crow::response admin_stop_all(const crow::request &req);
	crow::response admin_stop(const crow::request &req, uint64_t id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	bool hosting_on_ = false;
	std::string provider_ = "none";
	int cap_ = 24;
	PlacementPolicy fleet_;
	std::string address_zone_;
	RateLimiter start_stop_per_user_;
};

} // namespace hosted
} // namespace opennova::novaworld_server
