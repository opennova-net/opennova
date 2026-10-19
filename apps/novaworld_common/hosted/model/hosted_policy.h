#pragma once

// The hosted servers' policy: whether hosting is on, its provider, the fleet's
// policy settings (placement, the slots' limits, the cap, the serve image), the
// object store its data sets' trees live in, the supervisor's MachineProfile
// and whether the static addresses are on (with their play zone's name). Web's
// routes (a start's at_capacity, the list's `full`), the fleet's supervisor and
// the worker's fleet jobs read the same environment the same way, so no
// program needs another's memory to agree with it. The machine driver's own
// settings are the worker's alone (hosted/cloud/driver_config.h driver_setup).
// Service policy, not a port.

#include "core/object_store.h"
#include "fleet_config.h"
#include "machine_profile.h"
#include "program/server_config.h"

#include <string>

namespace opennova::novaworld_server::hosted {

struct HostedPolicy {
	bool on = false;
	// The driver's name ("ec2" or "docker") while on, else "none".
	std::string provider = "none";
	// The object store (program/server_config.h object_store_of) the data sets'
	// trees live in, when it parses (store_ok). Read whether or not hosting is
	// on (web's data set routes run over it either way); hosting is on only
	// with it.
	ObjectStoreConfig store;
	bool store_ok = false;
	FleetConfig fleet;
	// The supervisor's view of the driver's machines (the fleet's).
	MachineProfile profile;
	// The static addresses (address_repository.h) are on: hosting on,
	// ONNET_FLEET_MAX_ADDRESSES above 0 and ONNET_HOSTED_ADDRESS_ZONE a zone
	// name (no name: a boot warning). Off, the fleet sells none
	// (fleet.placement.max_addresses is 0). Their drivers' settings are the
	// worker's (driver_setup).
	bool addresses_on = false;
};

// The hosted servers' policy ONNET_HOSTED_PROVIDER names, over the object store
// ONNET_OBJECT_STORE_URL names: off for "none", an unknown name, fleet settings
// that do not read, no store or one that does not parse, a loopback
// ONNET_PUBLIC_HOST (the servers list through the service's public address) or
// a serve image that is no image reference (each but "none" logged as a
// warning). The driver's own settings are no part of it (driver_setup). The
// fleet's settings are the environment's (fleet_config.h), the internal
// listener's bind the fleet found in its ECS task standing in for an unset
// ONNET_INTERNAL_HTTP_BIND (config.internal_bind).
HostedPolicy hosted_policy(const ServerConfig &config);

} // namespace opennova::novaworld_server::hosted
