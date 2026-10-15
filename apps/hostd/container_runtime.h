#pragma once

// The seam between opennova-hostd and the containers it runs. DockerRuntime
// (docker_runtime.h) is the real one, over the docker CLI; the tests' fake
// keeps containers in memory (tests/hostd). Only the containers the agent
// started are ever seen or touched (DockerRuntime marks them with a label).

#include "hosted/agent_protocol.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace opennova::hostd {

namespace hosted = novaworld_server::hosted;

// A desired container resolved to this machine: what one start runs.
struct ContainerSpec {
	std::string name;
	int slot = 0;
	std::string image;
	uint32_t uid = 0;
	std::string data_dir; // the data set's mount path, bound read-only at /opt/game
	std::string work_dir; // the slot's working directory, bound at /srv/serve
	std::vector<std::string> args;
	std::string env_file; // a docker --env-file, or "" for no environment
	double cpus = 1.0;
	int memory_mb = 512;
	int pids = 128;
};

// One of the agent's containers as the runtime sees it.
struct ContainerInfo {
	std::string name;
	int slot = -1; // -1: its slot label is missing or wrong
	hosted::ContainerState state = hosted::ContainerState::Starting;
	std::optional<int64_t> exit_code; // once it exited
	bool oom = false;
	std::optional<int64_t> started_at; // Unix seconds, once it started
};

struct ContainerStats {
	double cpu_percent = 0.0;
	uint64_t memory_bytes = 0;
};

class ContainerRuntime {
public:
	virtual ~ContainerRuntime() = default;
	// Every container the agent started, in any state. False with `error` when
	// the runtime cannot tell (the agent then acts on nothing).
	virtual bool list(std::vector<ContainerInfo> &out, std::string &error) = 0;
	// The CPU and memory of the running containers `names`, by name.
	virtual bool stats(const std::vector<std::string> &names,
			std::map<std::string, ContainerStats> &out, std::string &error) = 0;
	// Creates and starts `spec`'s container, detached (pulling its image when
	// the machine lacks it). False with the CLI's `exit_code` (125: the runtime
	// refused, the image did not pull) and `error` when it did not start.
	virtual bool run(const ContainerSpec &spec, int &exit_code, std::string &error) = 0;
	// Asks it to stop (SIGTERM, on which opennova-serve deregisters), at once,
	// without waiting for it.
	virtual bool stop(const std::string &name, std::string &error) = 0;
	// Ends it now (SIGKILL).
	virtual bool kill(const std::string &name, std::string &error) = 0;
	// Removes a container that is not running (ended, or never started).
	virtual bool remove(const std::string &name, std::string &error) = 0;
};

} // namespace opennova::hostd
