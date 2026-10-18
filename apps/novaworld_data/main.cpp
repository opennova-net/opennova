// opennova-novaworld-data: the data program (program.h). The job pool
// (jobs/worker_pool.h, ONNET_WORKER_THREADS threads) over the hosted data sets'
// jobs alone (hosted/data_set_jobs.h: tree.finalize, release.validate,
// release.apply, tree.prune) on the object store, and the scheduler of its one
// periodic kind (parts.h data_schedule: tree.prune), in every data process with
// no election. What it runs is what web's data set and release routes and its
// own scheduler enqueued. It holds the store's credentials (the task role's
// data_store) and the disk a tree or a package passes through, and nothing of
// the fleet, billing or mail: an admin's upload can occupy it, never a machine
// launch. With no object store (ONNET_OBJECT_STORE_URL) it has no kind to run
// and idles. Its watchdog watches the pool's claim loop and the scheduler's
// ticks.

#include "parts.h"
#include "program.h"

#include "hosted/data_set_jobs.h"
#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <cstdio>
#include <string>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

int run(const program::Boot &boot) {
	const ServerConfig &config = boot.config;

	const HostedPolicy hosting = hosted_policy(config);
	// The object store the data sets' jobs run over, whether or not hosting is
	// on (web's data set routes upload into it either way). Outlives the pool.
	ObjectStoreStack store;
	jobs::JobRegistry registry;
	if (hosting.store_ok) {
		std::string error;
		store = make_object_store(hosting, config, error);
		if (!store.store)
			return program::fatal("the object store: " + error);
		std::printf("[boot] data sets: jobs over the %s store\n", store.store->kind());
		hosted::register_data_set_jobs(registry, *store.store, store.scratch.path().u8string());
	} else {
		std::printf("[boot] data sets: no object store (ONNET_OBJECT_STORE_URL); no data set "
					"job runs\n");
	}

	jobs::WorkerPoolConfig pool_config;
	pool_config.threads = config.worker_threads;
	pool_config.name = "data";
	jobs::WorkerPool workers(boot.pool, registry, pool_config);
	workers.start();
	jobs::Scheduler scheduler(boot.pool, data_schedule(hosting));
	scheduler.start();

	Watchdog watchdog;
	watchdog.watch("job pool", [&workers] { return workers.loop_turns(); });
	watchdog.watch("job scheduler", [&scheduler] { return scheduler.turns(); });

	const int exit_code = program::run_main_loop("data", boot, watchdog, {});

	// The scheduler first; then each worker once its job has (one cut short
	// runs again on the next lease's worker); then the store.
	std::printf("[shutdown] stopping the data jobs\n");
	scheduler.stop();
	workers.stop();
	return exit_code;
}

} // namespace

int main() {
	return program::run_program({ "data", {}, run });
}
