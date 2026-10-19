#pragma once

// opennova-hostd's command line. Every setting is a flag (the fleet cluster's
// task definition names them, the same for every machine; the local stack's
// machine driver its own); the one variable the agent reads is the ECS
// agent's metadata endpoint (its own image, for the sync's envelope, and its
// own log stream).

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hostd {

// How the agent runs its containers (--runtime): docker containers on a cloud
// machine (docker_runtime.h), or opennova-serve child processes on a machine that
// is itself a container, the local stack's (process_runtime.h).
enum class RuntimeKind : uint8_t { Docker, Process };

struct HostdOptions {
	std::string service_url; // --service: http://<the fleet's private name or address>:<port>
	// --machine <id>, or --machine-file <path> (its one line: the user-data's
	// /etc/opennova/machine-id), read at the start: machine_identity.h.
	int64_t machine_id = 0;
	std::string machine_file;
	// --ecs-config: the ECS agent's config, whose ECS_INSTANCE_ATTRIBUTES
	// machine-id must be this machine's ("": no check).
	std::string ecs_config;
	std::string token_file = "/etc/opennova/hostd.token";
	std::string state_dir = "/var/lib/opennova-hostd";
	std::string slots_root = "/srv/slots";
	RuntimeKind runtime = RuntimeKind::Docker;
	std::string trees_root = "/opt/game"; // --trees-root: the trees' directories (tree_store.h)
	std::string docker = "/usr/bin/docker";
	std::string serve = "/usr/local/bin/opennova-serve"; // --serve: the process runtime's
	std::string aws = "/usr/local/bin/aws"; // --aws: the CLI a tree's `s3 sync` runs
	// --nft: the slots' fences and counters (nft_table.h), or "none" for neither
	// (the process runtime's machine, which has no slot users).
	std::string nft = "/usr/sbin/nft";
	// --region: the deployment's AWS region, which a tree's `aws s3 sync` names
	// (tree_store.h ProcessTreeFetcher; unset, the CLI's own) and the log group's.
	std::string region;
	// --log-group, with --region, the docker runtime's: the CloudWatch log group
	// the containers' output goes to, the servers' group (docker_runtime.h
	// ContainerLogs); unset, the machine's journal.
	std::string log_group;
	// --log-dir, the process runtime's: the local stack's log directory, where
	// each server's output goes (servers/<server id>/g<generation>.log) and a
	// copy of the agent's own lines (machines/<machine id>/hostd.log).
	std::string log_dir;
	bool once = false;
	bool verbose = false;
};

// CheckAlive: --check-alive, the ECS health check (exit 0 while the pass loop
// turns: machine_identity.h alive_within).
enum class ParseOutcome : uint8_t { Run, Help, Version, CheckAlive, Error };

ParseOutcome parse_hostd_options(
		const std::vector<std::string> &args, HostdOptions &out, std::string &error);
const char *hostd_usage();
// The host and port of a --service URL ("http://<host>[:<port>]", port 80 when
// none); false when it is not one.
bool service_host_port(const std::string &url, std::string &host, uint16_t &port);
// The agent's build (OPENNOVA_HOSTD_VERSION at configure time; "dev" without it).
const char *hostd_version();

} // namespace opennova::hostd
