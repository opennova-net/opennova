#pragma once

// The settings an admin changes on the admin's Settings page, live (service_meta rows, no
// redeploy): whether support tiers are sold (subscriptions), and the fleet's four maximums over the
// ones its programs booted with (ONNET_FLEET_*, fleet_config.h). A key with no row takes its
// default: subscriptions off, each maximum the boot one. Every program reads them where it decides
// (a start's room, a placement, a launch, the sold capacity), so a change lands at the next
// decision. With subscriptions off no tier is sold and every account keeps its free shared server
// (billing/tiers.h kBaseSharedServers); a subscription already paid for still grants. Crow-free.
// Service policy, not a port.

#include "placement.h"

#include <net/novaworld/db/database.h>

#include <optional>
#include <string>

namespace opennova::novaworld_server::hosted {

// The highest a fleet maximum may be set to (fleet_config.cpp's ranges for ONNET_FLEET_MAX_*).
inline constexpr int kMaxFleetMachines = 64;

struct ServiceSettings {
	bool subscriptions_enabled = false;
	// Each maximum over the boot one; none: the boot one.
	std::optional<int> max_shared;
	std::optional<int> shared_slots;
	std::optional<int> max_own;
	std::optional<int> max_addresses;
};

// The settings as the rows hold them (a row that does not read is no row).
ServiceSettings read_service_settings(db::Database &db);

// "" when `settings` may be written over a fleet that booted with `boot`, else why not: each
// machine maximum 0..kMaxFleetMachines, the slots 1..kMaxSlots, and static addresses only where the
// deployment runs them (boot.max_addresses above 0: their zone and drivers are set at boot).
std::string check_service_settings(const ServiceSettings &settings, const PlacementPolicy &boot);

// The rows set to `settings` in one transaction (a maximum with no value deletes its row).
void write_service_settings(db::Database &db, const ServiceSettings &settings);

// The placement the fleet runs by now: `boot` with the settings' maximums over it, static addresses
// left off where the deployment runs none.
PlacementPolicy live_placement(const PlacementPolicy &boot, const ServiceSettings &settings);
PlacementPolicy live_placement(db::Database &db, const PlacementPolicy &boot);

// Whether support tiers are sold now.
bool subscriptions_enabled(db::Database &db);

} // namespace opennova::novaworld_server::hosted
