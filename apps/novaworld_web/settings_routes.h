#pragma once

// The admin's live settings (hosted/model/service_settings.h; the fleet's maximums on the admin's
// Servers > Machines, subscriptions on Billing): GET and PUT /api/admin/settings,
// whether support tiers are sold and the fleet's four maximums, live (no redeploy). Every handler
// opens with WebAccess::require_admin. The HttpListener owns one beside its other route families.
//
//   {"subscriptions_enabled", "fleet": {"max_shared", "shared_slots", "max_own", "max_addresses"}
//    (what the fleet runs by now), "boot": {the same four} (the deployment's ONNET_FLEET_*
//    values, which a maximum with no setting takes), "set": {the same four} (the admin's, null
//    for none), "addresses_available" (the deployment runs static addresses: boot max_addresses
//    above 0), "capacity" (billing_routes.h sold_capacity_json, against "fleet")}
//
// PUT takes {"subscriptions_enabled": true|false, "max_shared", "shared_slots", "max_own",
// "max_addresses"}: each maximum a whole number or null (the boot value), every key required, all
// written at once and answered with the GET shape (logged). 400 invalid_json, invalid_field (a
// value of the wrong type, or one service_settings.h's check refuses), missing_field. Service
// policy, not a port.

#include "hosted/model/placement.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

namespace opennova::novaworld_server {

class SettingsRoutes {
public:
	SettingsRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	SettingsRoutes(const SettingsRoutes &) = delete;
	SettingsRoutes &operator=(const SettingsRoutes &) = delete;

	// The placement policy the fleet booted with (fleet_config.h); before register_routes().
	void set_fleet(const hosted::PlacementPolicy &policy) { fleet_ = policy; }
	void register_routes(WebApp &app);

private:
	crow::response get(const crow::request &req);
	crow::response put(const crow::request &req);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	hosted::PlacementPolicy fleet_;
};

} // namespace opennova::novaworld_server
