#pragma once

// opennova-hostd's containers as plain child processes (--runtime process): the
// runtime of a machine that is itself a container and runs no docker, the local
// stack's machines (the docker machine driver's, deploy/compose). A start is
//
//   <serve> --resource-dir <data set's mount path> <opennova-serve's arguments>
//
// in the slot's directory, its environment the system's variables alone (the
// `environment` it is given: PATH, HOME, LANG, LC_*, TZ, TMPDIR) and the env
// file's, its output appended to `output_path` (main.cpp: the agent's own
// standard error, so the servers' lines are the machine's log). Nothing isolates
// it beyond what the machine itself is: no user of the slot's (the uid, the CPUs,
// the memory and the pids cap are not applied), the machine's own network. What
// the agent sees is docker's: each one listed by name until it is removed,
// running until it exits (its code; one a signal ended 128 plus the signal's
// number), a stop SIGTERM (on which opennova-serve deregisters), a kill SIGKILL,
// a start that fails exit 125. Its image is the machine's own binary, so every
// image is present and no pull is ever made. It reports no CPU and no memory
// (0). The servers end with the agent (the parent-death signal): a machine whose
// agent ended runs nothing, as a machine container that ends with it. Platform
// plumbing, not a port.

#include "container_runtime.h"

#include <base/proc/child_process.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace opennova::hostd {

class ProcessRuntime : public ContainerRuntime {
public:
	using Clock = std::function<int64_t()>; // Unix seconds

	// `serve`: opennova-serve's absolute path. `environment`: the NAME=value
	// entries every server gets before its env file's. `output_path`: the file
	// each server's output is appended to (empty discards it).
	ProcessRuntime(std::string serve, std::vector<std::string> environment, std::string output_path,
			Clock now_s = {});
	// Kills and reaps every server still running.
	~ProcessRuntime() override;

	bool list(std::vector<ContainerInfo> &out, int timeout_ms, std::string &error) override;
	bool stats(const std::vector<std::string> &names, std::map<std::string, ContainerStats> &out,
			int timeout_ms, std::string &error) override;
	bool has_image(
			const std::string &image, bool &present, int timeout_ms, std::string &error) override;
	bool start_pull(const std::string &image, std::string &error) override;
	PullState poll_pull(const std::string &image, std::string &error) override;
	void cancel_pull(const std::string &image) override;
	bool run(
			const ContainerSpec &spec, int timeout_ms, int &exit_code, std::string &error) override;
	bool stop(const std::string &name, int timeout_ms, std::string &error) override;
	bool kill(const std::string &name, int timeout_ms, std::string &error) override;
	bool remove(const std::string &name, int timeout_ms, std::string &error) override;

	// The argv run() starts for `spec`.
	std::vector<std::string> run_argv(const ContainerSpec &spec) const;

private:
	struct Server {
		int slot = 0;
		proc::ChildProcess process;
		std::optional<int64_t> exit_code; // once it exited
		int64_t started_at = 0;
	};

	// Its exit code, read now when it has exited (reaping it).
	void poll(Server &server);

	std::string serve_;
	std::vector<std::string> environment_;
	std::string output_path_;
	Clock now_s_;
	std::map<std::string, Server> servers_; // by name
};

// An env file's NAME=value lines (the agent's, one variable a line) as entries;
// a line with no '=' or no name is skipped.
std::vector<std::string> read_env_file_lines(const std::string &text);

} // namespace opennova::hostd
