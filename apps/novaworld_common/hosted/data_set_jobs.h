#pragma once

// The hosted data sets' jobs (ADR 0064): tree.finalize, release.validate,
// release.apply and the hourly tree.prune, over the object store. Declared
// here; lane C's data_set_jobs.cpp defines them (this header is replaced by
// that lane's own, with the same two declarations).

#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"
#include "object_store.h"

#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

// The handlers over `store` (which must outlive the workers), their downloads
// under `scratch_dir`; registered as the fleet's are (fleet_jobs.h).
void register_data_set_jobs(jobs::JobRegistry &registry, ObjectStore &store, std::string scratch_dir);
// The periodic tree.prune (hourly).
std::vector<jobs::PeriodicJob> data_set_schedule();

} // namespace opennova::novaworld_server::hosted
