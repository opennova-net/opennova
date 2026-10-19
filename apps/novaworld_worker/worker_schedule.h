#pragma once

// The worker's periodic jobs (jobs/scheduler.h), decided from the settings
// alone, so web's jobs page (which shows and runs on request every program's
// periodic kinds) and every worker process schedule alike. Service policy,
// not a port.

#include "hosted/model/hosted_policy.h"
#include "jobs/scheduler.h"
#include "program/server_config.h"

#include <vector>

namespace opennova::novaworld_server {

// The worker's periodic jobs over `config` and `hosting`: the prunes (the
// service's, the website's and the billing cache's, every
// HOST_SWEEP_INTERVAL_MS; the hosted stats' and the finished jobs', hourly),
// billing's while its settings are complete (check_billing_config), and the
// fleet's sweep while hosting is on. Every one of the worker's kinds that is
// periodic.
std::vector<jobs::PeriodicJob> worker_schedule(
		const ServerConfig &config, const hosted::HostedPolicy &hosting);

} // namespace opennova::novaworld_server
