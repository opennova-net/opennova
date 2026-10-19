#pragma once

// The fleet's jobs' handlers (hosted/model/fleet_jobs.h: machine.launch,
// machine.describe, machine.terminate, fleet.sweep) over a MachineDriver: the
// worker's alone, which calls the cloud. Service policy, not a port.

#include "hosted/model/fleet_jobs.h"
#include "hosted/model/machine_driver.h"
#include "hosted/model/placement.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include <functional>
#include <string>
#include <string_view>

namespace opennova::novaworld_server::hosted {

// The most of each running at once, across every worker (the AWS API's rate
// limits).
struct FleetJobCaps {
	int launch = 8;
	int describe = 4;
	int terminate = 4;
};

// The handlers over `driver`, which must outlive the workers; `limits` holds
// the fleet's maximums the launch checks (placement.h PlacementPolicy, the
// fleet's policy settings). Each fleet.sweep whose machines' listing read also
// runs `also_sweep` (set: the static addresses' orphan report,
// address_job_handlers.h sweep_addresses).
void register_fleet_jobs(jobs::JobRegistry &registry, MachineDriver &driver,
		PlacementPolicy limits, FleetJobCaps caps = {},
		std::function<void(db::ConnectionPool &)> also_sweep = {});

// The cloud's error code a failure's text names: the AWS CLI's "An error
// occurred (<Code>) when calling ..." (VcpuLimitExceeded,
// InsufficientInstanceCapacity, UnauthorizedOperation, ...); "" for none.
std::string cloud_error_code(std::string_view text);
// A refused launch's end reason and event: "the launch failed (<code>)" with the
// cloud's error code (cloud_error_code), else "the launch failed".
std::string launch_failure_reason(std::string_view error);

} // namespace opennova::novaworld_server::hosted
