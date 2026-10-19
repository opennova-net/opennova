#include "worker_schedule.h"

#include "accounts/account_jobs.h"
#include "billing/billing_config.h"
#include "billing/billing_jobs.h"
#include "hosted/model/fleet_jobs.h"
#include "hosted/model/stats_jobs.h"
#include "jobs/maintenance_jobs.h"

#include <utility>

namespace opennova::novaworld_server {

std::vector<jobs::PeriodicJob> worker_schedule(
		const ServerConfig &config, const hosted::HostedPolicy &hosting) {
	jobs::MaintenanceConfig maintenance;
	maintenance.sweep_interval_ms = static_cast<int64_t>(config.host_sweep_interval_ms);
	maintenance.host_stale_window_ms = static_cast<int64_t>(config.host_stale_window_ms);
	std::vector<jobs::PeriodicJob> out = jobs::maintenance_schedule(maintenance);
	const auto add = [&out](std::vector<jobs::PeriodicJob> more) {
		for (jobs::PeriodicJob &p : more)
			out.push_back(std::move(p));
	};
	add(jobs::account_schedule(maintenance));
	// The billing cache's prune whether or not billing is configured.
	add(billing::billing_prune_schedule(maintenance.sweep_interval_ms));
	// The hosted stats' prune whether or not hosting is on: a fleet turned off
	// leaves rows that still age out.
	add(hosted::stats_schedule());
	// Billing's jobs while its settings are complete, at their reconcile
	// period: the workers' billing runs them.
	if (const billing::BillingConfig billing_config = billing::billing_config_from_env();
			billing::check_billing_config(billing_config).empty())
		add(billing::billing_schedule(billing_config.reconcile_minutes));
	// The sweep of the driver's machines (hosted/model/fleet_jobs.h kFleetSweepIntervalMs).
	if (hosting.on)
		add(hosted::fleet_schedule());
	return out;
}

} // namespace opennova::novaworld_server
