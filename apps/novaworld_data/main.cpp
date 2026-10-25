// opennova-novaworld-data: the data program (program/program.h). The job pool
// (jobs/worker_pool.h, ONNET_WORKER_THREADS threads) over the hosted data sets'
// jobs alone (data_sets/data_set_jobs.h: tree.finalize, release.validate,
// release.apply, tree.prune; data_sets/release_build.h: release.build, which runs
// opennova-project and opennova-update-tool from beside this program) on the
// object store, and the scheduler of its one
// periodic kind (data_set_schedule: tree.prune, while there is a store), in
// every data process with no election. What it runs is what web's data set and
// release routes and its own scheduler enqueued. It holds the store's
// credentials (the task role's data_store) and the disk a tree or a package
// passes through, and nothing of the fleet, billing or mail (it reads no
// hosting policy, the store alone: program/server_config.h object_store_of):
// an admin's upload can occupy it, never a machine launch. With no object
// store (ONNET_OBJECT_STORE_URL) it has no kind to run and idles. Its watchdog
// watches the pool's claim loop and the scheduler's ticks.

#include "program/program.h"

#include "core/object_store_stack.h"
#include "data_sets/data_set_jobs.h"
#include "data_sets/release_build.h"
#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <system_error>
#include <string>
#include <vector>

namespace {

using namespace opennova;
using namespace opennova::novaworld_server;

// The release build's tools (release_build.h): beside this program, as the data image lays them
// out (/app), with the deployment's update host its UPDATE.WIZ names.
hosted::ReleaseTools release_tools(const ServerConfig &config) {
	std::error_code ec;
	const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
	const std::filesystem::path dir = ec ? std::filesystem::path("/app") : self.parent_path();
	hosted::ReleaseTools tools;
	tools.project_tool = (dir / "opennova-project").u8string();
	tools.update_tool = (dir / "opennova-update-tool").u8string();
	tools.update_host = config.update_host;
	return tools;
}

int run(const program::Boot &boot) {
	const ServerConfig &config = boot.config;

	// The object store the data sets' jobs run over, whether or not hosting is
	// on (web's data set routes upload into it either way). Outlives the pool.
	const std::optional<ObjectStoreConfig> store_config = object_store_of(config);
	ObjectStoreStack store;
	jobs::JobRegistry registry;
	if (store_config) {
		std::string error;
		store = make_object_store(*store_config, config.aws_cli, error);
		if (!store.store)
			return program::fatal("the object store: " + error);
		std::printf("[boot] data sets: jobs over the %s store\n", store.store->kind());
		hosted::register_data_set_jobs(registry, *store.store, store.scratch.path().u8string());
		hosted::register_release_build(
				registry, *store.store, store.scratch.path().u8string(), release_tools(config));
	} else {
		std::printf("[boot] data sets: no object store (ONNET_OBJECT_STORE_URL); no data set "
					"job runs\n");
	}

	jobs::WorkerPoolConfig pool_config;
	pool_config.threads = config.worker_threads;
	pool_config.name = "data";
	jobs::WorkerPool workers(boot.pool, registry, pool_config);
	workers.start();
	// The data sets' prune of the trees nothing needs, while there is a store.
	jobs::Scheduler scheduler(boot.pool,
			store_config ? hosted::data_set_schedule() : std::vector<jobs::PeriodicJob>{});
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
