#pragma once

// The static addresses' jobs' handlers (hosted/model/address_jobs.h:
// address.allocate, address.associate, address.release) and fleet.sweep's
// addresses over an AddressDriver and a ZoneDriver: the worker's alone, which
// calls the cloud and the zone. Service policy, not a port.

#include "address_driver.h"
#include "address_zone.h"
#include "hosted/model/address_jobs.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include <cstdint>

namespace opennova::novaworld_server::hosted {

// The handlers over `driver` and `zone`, which must outlive the workers. With
// `allocations_on` false (the add-on off: hosted/model/hosted_policy.h
// addresses_on) no address is allocated, while the held ones' releases,
// associations and records run.
void register_address_jobs(
		jobs::JobRegistry &registry, AddressDriver &driver, ZoneDriver &zone, bool allocations_on);

// fleet.sweep's addresses: the listing's orphans written; false when the
// listing failed (nothing written).
bool sweep_addresses(db::ConnectionPool &pool, AddressDriver &driver);

} // namespace opennova::novaworld_server::hosted
