#pragma once

// The admin's infrastructure view (the site's admin page Infrastructure,
// /admin/infrastructure; README "Infrastructure"): GET
// /api/admin/infrastructure, require_admin, the deployment as it runs now in
// one reply. From the database, which every web replica reads alike: the lobby
// instances (lobby_instances, with their sessions and listings), the fleet
// process that leads (fleet_leader) and the fleet's machines
// (fleet_machines: the live ones and the newest ended). From AWS, through
// infra_status.h (kept for its cache window, so the page's polls cost AWS
// nothing more): the programs' ECS services and their tasks, the balancer's
// target groups, the database, the cache, the release, and each machine's zone
// and EC2 state. Without AWS (the local stack) the cloud's sections say why and
// hold nothing. Read only: no route here changes anything. Every refusal is an
// error code with our own message, never database text; AWS's own error text
// reaches the admin page (it names an action and a resource, never a
// credential). The HttpListener owns one.

#include "infra_status.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstddef>
#include <string>

namespace opennova::novaworld_server::infra {

class InfraRoutes {
public:
	// The stopped tasks shown per program (and of the one-off tasks): the
	// newest.
	static constexpr size_t kStoppedShown = 3;
	// The ended or failed machines after the live ones.
	static constexpr size_t kEndedMachinesShown = 10;

	InfraRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	InfraRoutes(const InfraRoutes &) = delete;
	InfraRoutes &operator=(const InfraRoutes &) = delete;

	// The cloud's reader (it must outlive the routes); null: no cloud is read
	// here. Whether hosting is on and its driver's name (HostedRoutes decided
	// them). Before register_routes().
	void configure(InfraStatus *status, bool hosting, std::string provider);
	void register_routes(WebApp &app);

private:
	crow::response overview(const crow::request &req);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	InfraStatus *status_ = nullptr;
	bool hosting_ = false;
	std::string provider_ = "none";
};

} // namespace opennova::novaworld_server::infra
