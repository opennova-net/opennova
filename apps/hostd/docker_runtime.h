#pragma once

// opennova-hostd's containers through the docker CLI, run from an argv array
// with no shell between (apps/common/command_runner.h). Every container it
// starts carries the labels opennova.hostd=1 and opennova.slot=<k>, so the
// agent lists only its own (a restarted agent finds them again; nothing else on
// the machine is ever stopped or removed). A start is:
//
//   docker run -d --name <name> --label opennova.hostd=1 --label opennova.slot=<k>
//     --network host --user <uid>:<uid> --cap-drop ALL
//     --security-opt no-new-privileges --read-only --tmpfs /tmp --cpus <cpus>
//     --memory <mb>m --memory-swap <mb>m --pids-limit <pids> --stop-timeout 60
//     --log-driver journald --log-opt tag=<name>
//     --mount type=bind,source=<data set's mount path>,target=/opt/game,readonly
//     --mount type=bind,source=<slot's directory>,target=/srv/serve
//     [--env-file <file>] <image> <opennova-serve's arguments>
//
// The machine's own network (NovaWorld lists a server at the source of its
// datagrams; a bridge's NAT would rewrite the port), the slot's user with no
// capability and no way to gain one, a read-only root, swap no larger than the
// memory, a pids cap, the game data read-only and the slot's directory its only
// writable place but /tmp. A stop is SIGTERM (docker kill --signal SIGTERM:
// opennova-serve deregisters, and the agent waits for it without blocking), a
// kill SIGKILL. Stats come from `docker stats --no-stream`, the state from
// `docker inspect`. Platform plumbing, not a port.

#include "container_runtime.h"

#include "command_runner.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::hostd {

class DockerRuntime : public ContainerRuntime {
public:
	static constexpr const char *kManagedLabel = "opennova.hostd";
	static constexpr const char *kSlotLabel = "opennova.slot";
	static constexpr int kCommandTimeoutMs = 30 * 1000;
	// A start may pull its image first.
	static constexpr int kRunTimeoutMs = 5 * 60 * 1000;
	// docker's own wait between SIGTERM and SIGKILL when the machine shuts down.
	static constexpr int kStopTimeoutSeconds = 60;

	// `docker`: the CLI's absolute path. `runner` must outlive this.
	DockerRuntime(proc::CommandRunner &runner, std::string docker);

	bool list(std::vector<ContainerInfo> &out, std::string &error) override;
	bool stats(const std::vector<std::string> &names, std::map<std::string, ContainerStats> &out,
			std::string &error) override;
	bool run(const ContainerSpec &spec, int &exit_code, std::string &error) override;
	bool stop(const std::string &name, std::string &error) override;
	bool kill(const std::string &name, std::string &error) override;
	bool remove(const std::string &name, std::string &error) override;

	// The argv run() runs for `spec`.
	std::vector<std::string> run_argv(const ContainerSpec &spec) const;

private:
	bool simple(const char *what, std::vector<std::string> argv, std::string &error);

	proc::CommandRunner &runner_;
	std::string docker_;
};

// `docker inspect`'s JSON array as the agent's containers: the name without its
// leading '/', the state (created and restarting: starting; running and paused:
// running; exited, dead and removing: exited), the exit code and OOM flag once
// exited, the start time, the slot label. An entry without the agent's label is
// left out. False with `error` when `json` is no such array.
bool parse_docker_inspect(
		std::string_view json, std::vector<ContainerInfo> &out, std::string &error);
// `docker stats --format '{{json .}}'` lines, by container name: CPUPerc
// ("12.34%") and MemUsage's first figure ("45.5MiB / 512MiB"). A line that
// does not read is skipped.
void parse_docker_stats(std::string_view lines, std::map<std::string, ContainerStats> &out);
// An RFC 3339 time as docker writes it ("2026-10-14T12:34:56.123456789Z") in
// Unix seconds (its fraction dropped); nullopt for docker's zero time (year 1)
// and anything else before 1970 or unreadable.
std::optional<int64_t> parse_docker_time(std::string_view text);
// A size as docker writes it ("0B", "1.5KiB", "45.5MiB", "2GiB", "1.2kB",
// "3MB") in bytes; nullopt when it does not read.
std::optional<uint64_t> parse_docker_size(std::string_view text);

} // namespace opennova::hostd
