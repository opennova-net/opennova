#pragma once

// What a program running as an ECS task finds from its own task, which no
// configuration can name ahead of the task's start:
//
// - the fleet's internal listener bind (internal_bind, program/program.h's configure
//   step): ONNET_INTERNAL_HTTP_BIND where set; on ECS, its task's private IPv4
//   at the internal port (hosted/model/fleet_config.h), never a wildcard or public
//   address. Neither: the fleet keeps the environment's bind;
// - the release its tasks run (read_task_revision): its task definition's
//   family and revision, alike in every task of one revision, and new with
//   each release that changes the program (`app deploy` registers it): the
//   worker's at-start jobs' key (jobs/scheduler.h);
// - the task itself (read_task_facts): its id (its ARN's last segment) and its
//   size (Limits), which each program's usage samples are keyed and measured
//   by (program/program_usage.h).
//
// The task metadata endpoint (version 4: the agent names it in every
// container's ECS_CONTAINER_METADATA_URI_V4) gives them all: in awsvpc mode
// (Fargate's) the task's containers share one network interface, so any
// container's first IPv4 address is the task's. It also gives a container its
// own image as the task definition names it and the log stream its output goes
// to (machine agent opennova-hostd reports both in every sync, apps/hostd; it
// links the core). Service policy, not a port.

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

// The task's definition, "<family>:<revision>", from the text of a GET
// <uri>/task reply: false with `error` when it does not read as JSON or names
// no family and revision.
bool parse_task_revision(const std::string &json, std::string &out, std::string &error);

// GET <uri>/task read into `out` (parse_task_revision), as read_task_address.
bool read_task_revision(
		const std::string &uri, std::string &out, std::string &error, int timeout_ms = 3000);

// What a task's metadata says of the task itself.
struct TaskFacts {
	std::string id; // its TaskARN's last segment
	double cpu = 0; // its Limits' CPU, in CPUs; 0: none given
	int64_t memory_mib = 0; // its Limits' Memory, in MiB; 0: none given
};

// The task's id and limits from the text of a GET <uri>/task reply: false with
// `error` when it does not read as JSON or names no task ARN.
bool parse_task_facts(const std::string &json, TaskFacts &out, std::string &error);

// GET <uri>/task read into `out` (parse_task_facts), as read_task_address.
bool read_task_facts(
		const std::string &uri, TaskFacts &out, std::string &error, int timeout_ms = 3000);

// The container's own image (its metadata's "Image": the reference its task
// definition names, a release's pinned by digest) from the text of a GET <uri>
// reply: false with `error` when it does not read or names none.
bool parse_container_image(const std::string &json, std::string &out, std::string &error);

// GET <uri> (the container's own metadata) read into `out`
// (parse_container_image): false with `error` when the endpoint does not
// answer 200 or its reply does not read.
bool read_container_image(
		const std::string &uri, std::string &out, std::string &error, int timeout_ms = 3000);

// The container's awslogs stream (its metadata's LogOptions "awslogs-stream")
// from the text of a GET <uri> reply: false with `error` when it does not read
// as JSON or names none (a container logging elsewhere).
bool parse_container_log_stream(const std::string &json, std::string &out, std::string &error);

// GET <uri> (the container's own metadata) read into `out`
// (parse_container_log_stream): false with `error` when the endpoint does not
// answer 200 or its reply does not read.
bool read_container_log_stream(
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
