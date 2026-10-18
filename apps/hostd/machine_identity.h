#pragma once

// Which machine the agent is, and the checks that it is that one. On a cloud
// machine hostd runs as the fleet cluster's ECS daemon, one task definition for
// every machine, so its machine id cannot be on its command line: the
// machine's user-data writes it to a file (--machine-file,
// /etc/opennova/machine-id) and registers the same id with ECS as the
// instance attribute `machine-id` (ECS_INSTANCE_ATTRIBUTES in
// /etc/ecs/ecs.config, which the ECS agent registers at its start). The agent
// refuses to run where the two disagree (--ecs-config), so the toolbox's view
// of a container instance by its attribute (fleet shell, fleet status) is
// always the machine the service names. And the agent's liveness for its ECS
// health check (--check-alive): a file the pass loop touches, fresh while the
// loop turns, whether or not the service answers. Service policy, not a port.

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

namespace opennova::hostd {

// A machine id's text: 1..15 digits, above 0 (the service's fleet_machines id).
bool parse_machine_id(std::string_view text, int64_t &out);

// The machine id file's id (its one line, trimmed). False with `error`.
bool read_machine_file(const std::string &path, int64_t &out, std::string &error);

// The `machine-id` of an ecs.config's ECS_INSTANCE_ATTRIBUTES (its JSON
// object; the last such line counts, as the ECS agent reads it). False with
// `error` when there is none, or it does not read.
bool ecs_config_machine_id(std::string_view config, std::string &out, std::string &error);

// The alive file under the state directory, which the pass loop touches.
std::string alive_path(const std::string &state_dir);
// Its time set to now (made when missing); false when it cannot be.
bool touch_alive(const std::string &path);
// Whether the file was touched within `max_age_s` of `now` (the health check).
bool alive_within(const std::string &path, int64_t max_age_s, std::time_t now);

} // namespace opennova::hostd
