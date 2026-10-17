#pragma once

// The admin's fleet routes (ADR 0053, proposed; README "Hosted servers"): the
// fleet view (GET /api/admin/hosted: the machines and the servers on them,
// filtered by owner and state), one server with its
// machine (GET /api/admin/hosted/<id>), the machines' moves an admin makes: a
// drain, an end and a fleet roll (POST /api/admin/fleet/...), and the force of
// every outdated server onto its data sets' current data (POST
// /api/admin/hosted/update-all, ADR 0064). Every one require_admin; they write
// rows only (fleet_repository.h, data_set_repository.h), and the supervisor's
// next pass does the rest (moves the servers off, terminates the machine,
// starts the updated servers again). The kill switch and one server's stop are
// hosted_routes.h's. Every reply is an error
// code with our own message, never database text, and carries no credential:
// a machine row holds none (FleetMachineRow), a server's admin login is never
// read here. The HttpListener owns one FleetRoutes beside its HostedRoutes.

#include "hosted/fleet_repository.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server::hosted {

// A fleet machine as JSON for the admin: its id, driver, kind, state, cloud id,
// type, slots and the slots its live servers hold, addresses, agent, template,
// serve image, the server an own machine is reserved for, its times and why it
// ended, and its slots' egress this calendar month (UTC; stats_repository.h).
crow::json::wvalue machine_json(db::Database &db, const FleetMachineRow &machine);

class FleetRoutes {
public:
	// The servers the fleet view lists at most, and the ended machines after the
	// live ones.
	static constexpr size_t kServersShown = 100;
	static constexpr size_t kEndedMachinesShown = 10;
	static constexpr size_t kEventsShown = 50;

	FleetRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	FleetRoutes(const FleetRoutes &) = delete;
	FleetRoutes &operator=(const FleetRoutes &) = delete;

	// Whether this service has a machine driver, its name and ONNET_HOSTED_CAP
	// (HostedRoutes::configure decided them; the view's `enabled` adds at least
	// one offered data set, read at each request), and the fleet's placement
	// policy (the view's room per machine kind). Before register_routes().
	void configure(bool hosting_on, std::string provider, int cap, const PlacementPolicy &policy);
	void register_routes(WebApp &app);

private:
	crow::response fleet(const crow::request &req);
	crow::response server(const crow::request &req, uint64_t id);
	crow::response drain(const crow::request &req, uint64_t id);
	crow::response end(const crow::request &req, uint64_t id);
	crow::response roll(const crow::request &req);
	crow::response update_all(const crow::request &req);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	bool hosting_on_ = false;
	std::string provider_ = "none";
	int cap_ = 24;
	PlacementPolicy policy_;
};

} // namespace opennova::novaworld_server::hosted
