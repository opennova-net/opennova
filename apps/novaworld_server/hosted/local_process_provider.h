#pragma once

// The development and test MachineProvider (ONNET_HOSTED_PROVIDER=local): each
// hosted server is an opennova-serve child process of the service, on the
// service's own machine, in a working directory of its own under the work root
// (ONNET_HOSTED_WORK_DIR/server-<id>). A launch deletes that directory, writes
// the LaunchSpec's files there from scratch (the secret ones owner-only: mode
// 0600, or on Windows a DACL that grants the service's own account alone; the
// directory itself 0700 on POSIX), and runs
//
//   <ONNET_HOSTED_SERVE_BIN> --resource-dir <ONNET_HOSTED_RESOURCE_DIR>
//       [--loose-root] <the spec's arguments>
//
// there through apps/common/process.h (an argv array, no shell; the child's
// output appended to console.log beside the files). The ports are free ones
// probed on loopback, admin.cfg admits 127.0.0.1, and the service reaches the
// machine at 127.0.0.1. The children end with the service (and with this
// provider: its destructor ends every one it runs), so a service restart finds
// none and relaunches its live servers.

#include "machine_provider.h"

#include "process.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
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
	};

	LocalProcessConfig config_;
	std::mutex mu_;
	std::map<std::string, Machine> machines_;
	uint64_t next_machine_ = 1;
};

} // namespace opennova::novaworld_server::hosted
