#pragma once

// The seam between opennova-hostd and the containers it runs. DockerRuntime
// (docker_runtime.h) is the real one, over the docker CLI; the tests' fake
// keeps containers in memory (tests/hostd). Only the containers the agent
// started are ever seen or touched (DockerRuntime marks them with a label).
// Every call but a pull's waits at most the `timeout_ms` it is given (the
// agent's share of its pass, agent.h) and reads as a failure past it; a pull
// runs in the background, polled, so no pass ever waits for one.

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
	std::string base_dir; // the base tree's directory, bound read-only at /opt/game
	// The expansion tree's directory, bound read-only at
	// /opt/game/expansion/<expansion_code>; "" (and no code) for none.
	std::string expansion_dir;
	std::string expansion_code;
	std::string work_dir; // the slot's working directory, bound at /srv/serve
	std::vector<std::string> args;
	std::string env_file; // a docker --env-file, or "" for no environment
	double cpus = 1.0;
	int memory_mb = 512;
	int pids = 128;
	std::string log_stream; // where its output goes, as the service names it
};

// One of the agent's containers as the runtime sees it.
struct ContainerInfo {
	std::string name;
	int slot = -1; // -1: its slot label is missing or wrong
	std::string image; // the reference it was run from ("" when unknown)
	hosted::ContainerState state = hosted::ContainerState::Starting;
	std::optional<int64_t> exit_code; // once it exited
	bool oom = false;
	std::optional<int64_t> started_at; // Unix seconds, once it started
};

struct ContainerStats {
	double cpu_percent = 0.0;
	uint64_t memory_bytes = 0;
};

// Where a background pull stands (poll_pull).
enum class PullState : uint8_t {
	Running,
	Done, // the image is on the machine
	Failed, // it ended without the image, or never ran
};

class ContainerRuntime {
public:
	virtual ~ContainerRuntime() = default;
	// Every container the agent started, in any state. False with `error` when
	// the runtime cannot tell (the agent then acts on nothing).
	virtual bool list(std::vector<ContainerInfo> &out, int timeout_ms, std::string &error) = 0;
	// The CPU and memory of the running containers `names`, by name.
	virtual bool stats(const std::vector<std::string> &names,
			std::map<std::string, ContainerStats> &out, int timeout_ms, std::string &error) = 0;
	// Whether the machine holds `image` (`present`). False with `error` when the
	// runtime cannot tell.
	virtual bool has_image(
			const std::string &image, bool &present, int timeout_ms, std::string &error) = 0;
	// Starts pulling `image` in the background and returns at once (a pull of
	// it already under way is no error). False with `error` when it did not start.
	virtual bool start_pull(const std::string &image, std::string &error) = 0;
	// The pull of `image` start_pull began: Running while it runs; Done or
	// Failed (with `error`) once it ended, and then forgotten. One the runtime
	// never began reads Failed.
	virtual PullState poll_pull(const std::string &image, std::string &error) = 0;
	// Ends the pull of `image` now, and forgets it.
	virtual void cancel_pull(const std::string &image) = 0;
	// Creates and starts `spec`'s container, detached, from an image the
	// machine holds (it never pulls one). False with the CLI's `exit_code`
	// (125: the runtime refused, the image is missing) and `error` when it did
	// not start.
	virtual bool run(
			const ContainerSpec &spec, int timeout_ms, int &exit_code, std::string &error) = 0;
	// Asks it to stop (SIGTERM, on which opennova-serve deregisters), at once,
	// without waiting for it.
	virtual bool stop(const std::string &name, int timeout_ms, std::string &error) = 0;
	// Ends it now (SIGKILL).
	virtual bool kill(const std::string &name, int timeout_ms, std::string &error) = 0;
	// Removes a container that is not running (ended, or never started).
	virtual bool remove(const std::string &name, int timeout_ms, std::string &error) = 0;
	// The images of `repository` the machine holds, each as
	// `<repository>@<digest>` (one held by a tag alone is none of these).
	// False with `error` when the runtime cannot tell.
	virtual bool list_images(const std::string &repository, std::vector<std::string> &out,
			int timeout_ms, std::string &error) = 0;
	// Removes image `image` (a list_images reference) from the machine; the
	// runtime refuses one a container still uses (false, `error`).
	virtual bool remove_image(const std::string &image, int timeout_ms, std::string &error) = 0;
};

} // namespace opennova::hostd
