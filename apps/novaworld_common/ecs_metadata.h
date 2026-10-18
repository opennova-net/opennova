#pragma once

// The address a program running as an ECS task finds from its own task, one no
// configuration can name ahead of the task's start (program.h's configure
// step): the fleet's internal listener bind (internal_bind),
// ONNET_INTERNAL_HTTP_BIND where set; on ECS, its task's private IPv4 at the
// internal port (hosted/fleet_config.h), never a wildcard or public address.
// Neither: the fleet keeps the environment's bind.
//
// The task metadata endpoint (version 4: the agent names it in every
// container's ECS_CONTAINER_METADATA_URI_V4) gives the task's private IPv4: in
// awsvpc mode (Fargate's) the task's containers share one network interface,
// so any container's first IPv4 address is the task's. Service policy, not a
// port.

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::ecs {

// The environment variable the ECS agent names the endpoint in.
inline constexpr const char *kMetadataUriVariable = "ECS_CONTAINER_METADATA_URI_V4";

// ECS_CONTAINER_METADATA_URI_V4's value, "" when it is unset (not on ECS).
std::string metadata_uri_from_env();

// The task's private IPv4 address (its interface's first) from the text of a
// GET <uri>/task reply: false with `error` when it does not read as JSON or
// names no IPv4 address.
bool parse_task_address(const std::string &json, std::string &out, std::string &error);

// GET <uri>/task (each connect, send and receive at most `timeout_ms`) read
// into `out`: false with `error` when the endpoint does not answer 200 or its
// reply does not read (parse_task_address).
bool read_task_address(
		const std::string &uri, std::string &out, std::string &error, int timeout_ms = 3000);

// Where the bind comes from.
struct Sources {
	std::string configured; // ONNET_INTERNAL_HTTP_BIND; "": unset
	std::string metadata_uri; // ECS_CONTAINER_METADATA_URI_V4; "": not on ECS
	uint16_t port = 0; // the internal port
	int metadata_timeout_ms = 3000;
};

// The fleet's internal listener bind, <IPv4>:<port>, from the task when
// `configured` is unset and the task metadata is there; "" with no `error`
// when the environment's is to stand (set, or not on ECS), "" with `error`
// when the task's metadata does not give an address.
std::string internal_bind(const Sources &sources, std::string &error);

} // namespace opennova::novaworld_server::ecs
