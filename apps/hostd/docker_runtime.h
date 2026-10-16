#pragma once

// opennova-hostd's containers through the docker CLI, run from an argv array
// with no shell between (apps/common/command_runner.h). Every container it
// starts carries the labels opennova.hostd=1 and opennova.slot=<k>, so the
// agent lists only its own (a restarted agent finds them again; nothing else on
// the machine is ever stopped or removed). A start is:
//
//   docker run --pull never -d --name <name> --label opennova.hostd=1
//     --label opennova.slot=<k> --network host --user <uid>:<uid> --cap-drop ALL
//     --security-opt no-new-privileges --read-only
//     --tmpfs /tmp:size=64m,nosuid,nodev,noexec --cpus <cpus>
//     --memory <mb>m --memory-swap <mb>m --pids-limit <pids> --stop-timeout 60
//     <the logs: ContainerLogs below>
//     --mount type=bind,source=<base tree's directory>,target=/opt/game,readonly
//     [--mount type=bind,source=<expansion tree's directory>,
//        target=/opt/game/expansion/<code>,readonly]
//     --mount type=bind,source=<slot's directory>,target=/srv/serve
//     [--env-file <file>] <image> <opennova-serve's arguments>
//
// The machine's own network (NovaWorld lists a server at the source of its
// datagrams; a bridge's NAT would rewrite the port), the slot's user with no
// capability and no way to gain one, a read-only root, swap no larger than the
// memory, a pids cap, the game data read-only (the base tree, and the expansion
// tree inside it where the container has one: its mount point under the base
// tree is the store's to make, tree_store.h, since a read-only bind cannot have
// one made in it) and the slot's directory its only
// writable place but a bounded /tmp. A start never pulls (--pull never): an image the
// machine lacks is pulled first, in the background, by `docker pull -q
// <image>` (a child of its own, its standard error kept in a scratch file for
// the failure's text), while the agent's passes go on; `docker image inspect`
// tells whether the machine holds one. A stop is SIGTERM (docker kill --signal
// SIGTERM: opennova-serve deregisters, and the agent waits for it without
// blocking), a kill SIGKILL. Stats come from `docker stats --no-stream`, the
// state from `docker inspect`. Each call waits at most the timeout it is
// given. Platform plumbing, not a port.

#include "container_runtime.h"

#include "command_runner.h"

#include <base/proc/child_process.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::hostd {

// Where the containers' output goes. With a group (a cloud machine; its
// user-data passes --log-group and --log-region): that CloudWatch log group, a
// stream per slot, "<machine id>/slot-<k>", which each server on the slot
// writes in turn, through docker's awslogs driver:
//
//     --log-driver awslogs --log-opt awslogs-region=<region>
//     --log-opt awslogs-group=<group> --log-opt awslogs-stream=<machine id>/slot-<k>
//     --log-opt mode=non-blocking --log-opt max-buffer-size=4m
//
// Non-blocking: CloudWatch slow or out of reach never holds up a write of the
// server's (lines past the 4 MB buffer are dropped) nor its start (docker makes
// the stream in the background); docker's credentials are the machine role's.
// Docker keeps a local copy as well (its dual logging), so `docker logs <name>`
// reads a container's output on the machine while the container exists.
// Without a group (a development machine): the machine's journal,
//
//     --log-driver journald --log-opt tag=<name>
struct ContainerLogs {
	std::string group;
	std::string region;
	int64_t machine_id = 0;
};

class DockerRuntime : public ContainerRuntime {
public:
	static constexpr const char *kManagedLabel = "opennova.hostd";
	static constexpr const char *kSlotLabel = "opennova.slot";
	// docker's own wait between SIGTERM and SIGKILL when the machine shuts down.
	static constexpr int kStopTimeoutSeconds = 60;
	// The container's /tmp: bounded (its pages count toward the container's
	// memory, but a full one fails the writes, not the slot), and nothing on it
	// runs, owns a device or gains an id.
	static constexpr const char *kTmpfsOptions = "size=64m,nosuid,nodev,noexec";

	// `docker`: the CLI's absolute path. `runner` must outlive this. A pull
	// runs with `environment` (NAME=value entries) alone, its standard error
	// in a file under `scratch_dir` (owner-only). The containers log as `logs`
	// says.
	DockerRuntime(proc::CommandRunner &runner, std::string docker,
			std::vector<std::string> environment, std::string scratch_dir, ContainerLogs logs = {});
	// Ends the pulls still under way.
	~DockerRuntime() override;

	bool list(std::vector<ContainerInfo> &out, int timeout_ms, std::string &error) override;
	bool stats(const std::vector<std::string> &names, std::map<std::string, ContainerStats> &out,
			int timeout_ms, std::string &error) override;
	bool has_image(
			const std::string &image, bool &present, int timeout_ms, std::string &error) override;
	bool start_pull(const std::string &image, std::string &error) override;
	PullState poll_pull(const std::string &image, std::string &error) override;
	void cancel_pull(const std::string &image) override;
	bool run(const ContainerSpec &spec, int timeout_ms, int &exit_code,
			std::string &error) override;
	bool stop(const std::string &name, int timeout_ms, std::string &error) override;
	bool kill(const std::string &name, int timeout_ms, std::string &error) override;
	bool remove(const std::string &name, int timeout_ms, std::string &error) override;

	// The argv run() runs for `spec`.
	std::vector<std::string> run_argv(const ContainerSpec &spec) const;
	// Slot `slot`'s CloudWatch stream on machine `machine_id`: "<id>/slot-<k>".
	static std::string slot_log_stream(int64_t machine_id, int slot);

private:
	struct Pull {
		proc::ChildProcess child;
		std::string error_path;
	};

	bool simple(
			const char *what, std::vector<std::string> argv, int timeout_ms, std::string &error);
	// The pull's error file's first line, and the file gone.
	static std::string take_error(const std::string &path);

	proc::CommandRunner &runner_;
	std::string docker_;
	std::vector<std::string> environment_;
	std::string scratch_dir_;
	ContainerLogs logs_;
	std::map<std::string, Pull> pulls_; // by image
	uint64_t pulls_started_ = 0; // names the pulls' error files
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
