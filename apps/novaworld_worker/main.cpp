// opennova-novaworld-worker: a worker (program.h). The job pool
// (jobs/worker_pool.h, ONNET_WORKER_THREADS threads) and the handlers it runs:
// the prunes (the service's, the website's, the hosted stats'), the mail it
// sends, the billing sync, the fleet's machine driver calls and the hosted
// data sets' jobs over the object store. What it runs
// is what the other programs enqueued (the fleet's scheduler and
// supervisor, web's routes); any number of worker processes share the queue.
// Its watchdog watches the pool alone: a slow job occupies its worker, never a
// watched loop.

#include "parts.h"
#include "program.h"

#include "billing/billing_jobs.h"
#include "billing/entitlement.h"
#include "hosted/data_set_jobs.h"
#include "hosted/docker_machine_driver.h"
#include "hosted/ec2_machine_driver.h"
#include "hosted/fleet_jobs.h"
#include "hosted/limits_hold.h"
#include "hosted/stats_jobs.h"
#include "jobs/account_jobs.h"
#include "jobs/maintenance_jobs.h"
#include "jobs/worker_pool.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

// The machine driver over a hosting setup that is on. Its members go in
// reverse: the driver first, then its runner, then the scratch directory.
struct MachineDriverStack {
	ScratchDir scratch; // the ec2 driver's
	std::unique_ptr<proc::CommandRunner> runner;
	std::unique_ptr<hosted::MachineDriver> driver;
};

// The driver `setup` names: the ec2 driver over its own scratch directory and
// AWS CLI runner, or the docker driver over the Docker Engine API (the local
// stack's); no driver, with `error`, when its scratch directory cannot be made.
MachineDriverStack make_machine_driver(const HostedSetup &setup, std::string &error) {
	MachineDriverStack stack;
	if (setup.provider == "ec2") {
		hosted::Ec2MachineConfig ec2 = setup.fleet.ec2;
		stack.scratch = ScratchDir::make("opennova-hosted-", error);
		if (stack.scratch.empty()) {
			error = "the ec2 driver's scratch directory: " + error;
			return stack;
		}
		ec2.scratch_dir = stack.scratch.path().u8string();
		stack.runner = std::make_unique<proc::ProcessCommandRunner>(
				proc::ProcessCommandRunner::service_environment(), ec2.scratch_dir);
		stack.driver = std::make_unique<hosted::Ec2MachineDriver>(std::move(ec2), *stack.runner);
		return stack;
	}
	stack.driver = std::make_unique<hosted::DockerMachineDriver>(setup.fleet.docker);
	return stack;
}

int run(const program::Boot &boot) {
	const ServerConfig &config = boot.config;

	jobs::MaintenanceConfig maintenance;
	maintenance.sweep_interval_ms = static_cast<int64_t>(config.host_sweep_interval_ms);
	maintenance.host_stale_window_ms = static_cast<int64_t>(config.host_stale_window_ms);
	jobs::JobRegistry registry;
	jobs::register_maintenance_jobs(registry, maintenance);

	// Every part below outlives the pool (declared before it): the handlers
	// call into them.
	std::optional<BillingStack> billing_stack;
	try {
		auto lease = boot.pool.acquire();
		billing_stack.emplace(make_billing(billing::billing_config_from_env(), boot.pool, *lease));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[boot] FATAL: the worker's boot: %s\n", e.what());
		return 1;
	}
	MailStack mail = make_mail(config);
	const HostedSetup hosting = hosted_setup(config);
	// The object store the data sets' jobs run over (whether or not hosting is on).
	ObjectStoreStack store;
	if (hosting.store_ok) {
		std::string error;
		store = make_object_store(hosting, config, error);
		if (!store.store)
			return program::fatal("the object store: " + error);
		std::printf("[boot] data sets: jobs over the %s store\n", store.store->kind());
	}
	MachineDriverStack machines;
	if (hosting.on) {
		std::string error;
		machines = make_machine_driver(hosting, error);
		if (!machines.driver)
			return program::fatal("the machine driver: " + error);
	}

	jobs::register_account_jobs(registry);
	hosted::register_stats_jobs(registry);
	register_mail_jobs(registry, *mail.mailer);
	billing::Billing *billing = billing_stack->billing.get();
	if (billing != nullptr) {
		// A clean reconcile confirms the cache the limits hold (its row) doubts.
		billing->sync().set_on_reconciled(
				[](db::Database &db) { hosted::limits_hold::reconciled(db); });
		billing::register_billing_jobs(registry, billing->sync());
	}
	if (machines.driver)
		hosted::register_fleet_jobs(registry, *machines.driver);
	if (store.store)
		hosted::register_data_set_jobs(registry, *store.store, store.scratch.path().u8string());

	jobs::WorkerPoolConfig worker_config;
	worker_config.threads = config.worker_threads;
	jobs::WorkerPool workers(boot.pool, registry, worker_config);
	workers.start();

	// The worker's watchdog: its pool's claim loop.
	Watchdog watchdog;
	watchdog.watch("job pool", [&workers] { return workers.loop_turns(); });

	program::LoopHooks hooks;
	hooks.heartbeat = [billing](db::Database &db, Heartbeat &beat, int64_t) {
		beat.billing_sync_age_s =
				billing != nullptr ? billing->sync().sync_age_s(db, billing::unix_now()) : -1;
	};
	const int exit_code = program::run_main_loop("worker", boot, watchdog, hooks);

	// Each worker ends once its job has (a job cut short runs again on the
	// next lease's worker); then the parts, the driver last. The machines run
	// on for the fleet to resume.
	std::printf("[shutdown] stopping the job workers\n");
	workers.stop();
	return exit_code;
}

} // namespace

int main() {
	return program::run_program({ "worker", {}, run });
}
