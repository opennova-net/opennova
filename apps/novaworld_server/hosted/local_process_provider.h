#pragma once

// The development and test MachineProvider (ONNET_HOSTED_PROVIDER=local): each
// hosted server is an opennova-serve child process of the service, on the
// service's own machine, in a working directory of its own under the work root
// (ONNET_HOSTED_WORK_DIR/server-<id>). A launch deletes that directory, writes
// the LaunchSpec's files there from scratch, and runs
//
//   <ONNET_HOSTED_SERVE_BIN> --resource-dir <ONNET_HOSTED_RESOURCE_DIR>
//       [--loose-root] <the spec's arguments>
//
// there through base/proc/child_process.h (an argv array, no shell; the child's
// output appended to console.log beside the files). The ports are free ones
// probed on loopback, admin.cfg admits 127.0.0.1, and the service reaches the
// machine at 127.0.0.1.
//
// Secrets. The child gets an environment of its own, kServeEnvironment's
// variables alone, so none of the service's (ADMIN_API_TOKEN, DATABASE_PATH,
// every ONNET_*) reaches it. The spec's secret files (the host file with its
// game password, admin.cfg, the credentials) are written owner-only: mode
// 0600 in a 0700 directory on POSIX; on Windows a protected DACL granting the
// service's own account alone, on those files only (the directory and its
// other files keep what they inherit). terminate() deletes them once the
// process has ended; console.log stays. What opennova-serve writes there itself
// is not covered: its admin_log.txt keeps the admin password in plaintext as
// retail's does (ADR 0053, Consequences), and the game.cfg it saves holds the
// game password, so on Windows those two take the directory's inherited ACL.
//
// Lifetime. The children are started from a spawner thread the provider owns,
// so on Linux their parent-death signal (which follows the starting thread)
// fires when the provider goes, not when the supervisor's thread is joined.
// They end with the service (the signal, or Windows' kill-on-close job) and
// with this provider: its destructor asks each to stop (SIGTERM, on which
// opennova-serve deregisters; TerminateProcess on Windows) and kills it after
// the grace. So a service restart finds none and relaunches its live servers.

#include "machine_provider.h"

#include <base/proc/child_process.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct LocalProcessConfig {
	std::string serve_binary; // ONNET_HOSTED_SERVE_BIN
	std::string resource_dir; // ONNET_HOSTED_RESOURCE_DIR (--resource-dir)
	bool loose_root = false;  // ONNET_HOSTED_LOOSE_ROOT (--loose-root: a directory with no archives)
	std::string work_dir;     // ONNET_HOSTED_WORK_DIR: the servers' working directories
};

class LocalProcessProvider : public MachineProvider {
public:
	// How long terminate() waits for a child it asked to stop before it kills it.
	static constexpr int kTerminateGraceMs = 5000;
	// The variables a server's environment keeps from the service's (a name
	// ending in '*' is a prefix); everything else stays the service's.
	static const std::vector<std::string> kServeEnvironment;

	// The paths are made absolute against the service's working directory, so a
	// child (which runs elsewhere) reads the same ones.
	explicit LocalProcessProvider(LocalProcessConfig config);
	~LocalProcessProvider() override;

	const char *name() const override { return "local"; }
	MachinePlan plan(int64_t server_id) override;
	std::optional<std::string> launch(const LaunchSpec &spec, std::string &error) override;
	MachineStatus describe(const std::string &machine_id) override;
	void terminate(const std::string &machine_id) override;
	std::vector<TaggedMachine> list_tagged() override;

	// Server `server_id`'s working directory.
	std::filesystem::path server_dir(int64_t server_id) const;
	// The console log beside the files.
	static constexpr const char *kConsoleLogName = "console.log";

private:
	struct Machine {
		int64_t server_id = 0;
		proc::ChildProcess process;
		std::vector<std::filesystem::path> secret_files; // deleted at terminate()
	};

	// Runs `task` on the spawner thread and returns once it ran: false with
	// `error` when it threw (caught there, so the thread lives on) or when the
	// provider is shutting down (the spawner ends; nothing is queued then).
	bool on_spawner(const std::function<void()> &task, std::string &error);
	void spawner_loop();

	LocalProcessConfig config_;
	std::mutex mu_;
	std::map<std::string, Machine> machines_;
	uint64_t next_machine_ = 1;

	std::mutex spawner_mu_;
	std::condition_variable spawner_cv_;
	std::deque<std::function<void()>> spawner_tasks_;
	bool spawner_stopping_ = false;
	std::thread spawner_;
};

} // namespace opennova::novaworld_server::hosted
