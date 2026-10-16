#pragma once

// The addresses a program running as an ECS task finds from its own task, those
// no configuration can name ahead of the task's start (program.h's configure
// step):
//
// - the lobby's endpoint (lobby_endpoint): the one its instance row announces
//   and the gates hand its clients (udpnovaworld). ONNET_LOBBY_ENDPOINT names
//   it where the operator knows it (a fixed address, the tests); on ECS, its
//   task's public IPv4 with its own NovaWorld UDP port (ONNET_NW_UDP_PORT);
// - the controller's internal listener bind (internal_bind):
//   ONNET_INTERNAL_HTTP_BIND where set; on ECS, its task's private IPv4 at the
//   internal port (hosted/fleet_config.h), never a wildcard or public address.
//
// Neither source: no endpoint (the lobby does not boot, it would announce an
// address no client reaches); the controller keeps the environment's bind.
//
// The task metadata endpoint (version 4: the agent names it in every
// container's ECS_CONTAINER_METADATA_URI_V4) gives the task's private IPv4: in
// awsvpc mode (Fargate's) the task's containers share one network interface,
// so any container's first IPv4 address is the task's. Its public address is
// the interface's EC2 association, which no metadata names: asked of EC2
// (describe-network-interfaces, filtered by that address and the interface's
// MAC) with the task role's credentials, through the AWS CLI and the process
// seam the EC2 machine driver runs it through (apps/common/command_runner.h).
// Service policy, not a port.

#include "command_runner.h"

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::ecs {

// The environment variable the ECS agent names the endpoint in.
inline constexpr const char *kMetadataUriVariable = "ECS_CONTAINER_METADATA_URI_V4";

// ECS_CONTAINER_METADATA_URI_V4's value, "" when it is unset (not on ECS).
std::string metadata_uri_from_env();

// The task's network as its metadata gives it.
struct TaskNetwork {
	std::string private_ipv4; // its interface's first IPv4 address
	std::string mac_address; // its interface's MAC address ("" when not given)
	std::string region; // from its task ARN (arn:aws:ecs:<region>:...)
};

// The text of a GET <uri>/task reply read into `out`: false with `error` when
// it does not read as JSON, or names no IPv4 address or no region.
bool parse_task_network(const std::string &json, TaskNetwork &out, std::string &error);

// GET <uri>/task (each connect, send and receive at most `timeout_ms`) read
// into `out`: false with `error` when the endpoint does not answer 200 or its
// reply does not read (parse_task_network).
bool read_task_network(
		const std::string &uri, TaskNetwork &out, std::string &error, int timeout_ms = 3000);

// The public IPv4 address EC2 associates with the task's interface: `aws ec2
// describe-network-interfaces` in the task's region, filtered by its private
// address and (when the metadata gave one) its MAC address, through `runner`
// with `aws_cli` (an absolute path). False with `error` when the CLI fails,
// its answer does not read, it names no one interface, or the interface has
// no public address (a task without assign_public_ip).
bool public_ipv4(proc::CommandRunner &runner, const std::string &aws_cli, const TaskNetwork &task,
		std::string &out, std::string &error);

// Where the endpoints come from.
struct Sources {
	std::string configured; // the program's variable (ONNET_LOBBY_ENDPOINT,
							// ONNET_INTERNAL_HTTP_BIND); "": unset
	std::string metadata_uri; // ECS_CONTAINER_METADATA_URI_V4; "": not on ECS
	uint16_t port = 0; // the port to name (ONNET_NW_UDP_PORT; the internal port)
	std::string aws_cli = "/usr/local/bin/aws"; // ONNET_HOSTED_AWS_CLI (ServerConfig::aws_cli)
	int metadata_timeout_ms = 3000;
};

// The lobby's endpoint, <host>:<port>: `configured` as it is (refused unless
// <host>:<port>), else from the task, its public IPv4 and `port`; "" with
// `error` (the FATAL line's text) when neither gives one. `runner` runs the
// AWS CLI, on the ECS path alone.
std::string lobby_endpoint(const Sources &sources, proc::CommandRunner &runner, std::string &error);

// The controller's internal listener bind, <IPv4>:<port>, from the task when
// `configured` is unset and the task metadata is there; "" with no `error`
// when the environment's is to stand (set, or not on ECS), "" with `error`
// when the task's metadata does not give an address.
std::string internal_bind(const Sources &sources, std::string &error);

} // namespace opennova::novaworld_server::ecs
