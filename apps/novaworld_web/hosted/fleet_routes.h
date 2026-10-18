#pragma once

// The admin's fleet routes (ADR 0053, proposed; apps/novaworld_common/hosted/README.md
// "Routes"): the fleet view (GET /api/admin/hosted: the machines and the servers on them,
// filtered by owner and state), one server with its
// machine (GET /api/admin/hosted/<id>), one machine with its moves and its trees (GET
// /api/admin/fleet/machines/<id>), the machines' moves an admin makes: a
// drain, an end and a fleet roll (POST /api/admin/fleet/...; GET
// /api/admin/fleet/roll previews the roll, hosted/fleet_roll.h, each machine's
// image read from EC2 through the infrastructure view's reader), and the force of
// every outdated server onto its data sets' current data (POST
// /api/admin/hosted/update-all, ADR 0064). Every one require_admin; they write
// rows only (fleet_repository.h, data_set_repository.h), and the supervisor's
// next pass does the rest (moves the servers off, terminates the machine,
// starts the updated servers again). The kill switch and one server's stop are
// hosted_routes.h's. Every reply is an error
// code with our own message, never database text, and carries no credential:
// a machine row holds none (FleetMachineRow), a server's admin login is never
// read here. The HttpListener owns one FleetRoutes beside its HostedRoutes.

#include "hosted/fleet_health.h"
#include "hosted/fleet_repository.h"
#include "infra/infra_status.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include "hosted/fleet_roll.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

// A fleet machine as JSON for the admin: its id, driver, kind, state, cloud id,
// type, slots and the slots its live servers hold, addresses, agent (its
// version, protocol and image, its own log stream, and since when it speaks
// another protocol than the service's), its image, template, serve image, the
// server an own machine is reserved for, whether the mass-lost brake holds it,
// its times and why it ended, and its slots' egress this calendar month (UTC;
// stats_repository.h); its use as its agent last reported it ("stats", null
// before a report, with "stats_at"; its network's rates between its two newest
// minutes, infra/usage_routes.h machine_net_rates), the images `release` pins
// ("release") and whether it runs others ("outdated", fleet_health.h
// machine_outdated: null when no pair compares), and its static address ({id,
// name} or null).
crow::json::wvalue machine_json(
		db::Database &db, const FleetMachineRow &machine, const ReleaseImages &release);

class FleetRoutes {
public:
	// The servers the fleet view lists at most, and the ended machines after the
	// live ones.
	static constexpr size_t kServersShown = 100;
	static constexpr size_t kEndedMachinesShown = 10;
	static constexpr size_t kEventsShown = 50;
	// A machine's moves its page shows, the newest.
	static constexpr size_t kMachineEventsShown = 100;

	FleetRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	FleetRoutes(const FleetRoutes &) = delete;
	FleetRoutes &operator=(const FleetRoutes &) = delete;

	// Whether this service has a machine driver, its name and ONNET_HOSTED_CAP
	// (HostedRoutes::configure decided them; the view's `enabled` adds at least
	// one offered data set, read at each request), and the fleet's placement
	// policy (the view's room per machine kind). Before register_routes().
	void configure(bool hosting_on, std::string provider, int cap, const PlacementPolicy &policy);
	// The images the release pins (ONNET_FLEET_IMAGE_ID, the launch templates',
	// which the roll's preview holds each machine's against; ONNET_HOSTED_HOSTD_IMAGE;
	// "" for one not known) and the cloud's reader the roll's preview reads each
	// machine's image through (null: no cloud here, every image unknown). Before
	// register_routes().
	void configure_roll(ReleaseImages release, infra::InfraStatus *infra);
	void register_routes(WebApp &app);

private:
	crow::response fleet(const crow::request &req);
	crow::response server(const crow::request &req, uint64_t id);
	crow::response machine(const crow::request &req, uint64_t id);
	crow::response drain(const crow::request &req, uint64_t id);
	crow::response end(const crow::request &req, uint64_t id);
	crow::response roll_plan(const crow::request &req);
	crow::response roll(const crow::request &req);
	// The roll's plan now: the live machines with their images and servers.
	std::vector<RollStep> plan_roll(db::Database &db);
	crow::response update_all(const crow::request &req);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	bool hosting_on_ = false;
	std::string provider_ = "none";
	int cap_ = 24;
	PlacementPolicy policy_;
	ReleaseImages release_;
	infra::InfraStatus *infra_ = nullptr;
};

} // namespace opennova::novaworld_server::hosted
