#pragma once

// The fleet's settings from the service's environment, checked: where the
// internal listener binds and whom it answers, placement's knobs (placement.h),
// the slots' limits every container gets, and what the EC2 machine driver
// launches from (ec2_machine_driver.h). The variables:
//
//   ONNET_INTERNAL_HTTP_BIND      <IPv4>:<port> (127.0.0.1:8070): the internal
//                                 listener (internal_listener.h), on the
//                                 service's private address in a deployment
//                                 (10.44.0.10:8070); a private or loopback
//                                 address, never a public or wildcard one
//   ONNET_HOSTED_BOOTSTRAP_CIDRS  comma-separated private IPv4 CIDRs: the
//                                 networks the agents' requests may come from
//                                 (the machines' subnets); none, on a loopback
//                                 bind, admits 127.0.0.0/8
//   ONNET_FLEET_SHARED_SLOTS      a new shared machine's slots (12; 1..32)
//   ONNET_FLEET_MAX_SHARED        the most shared machines taking servers (2)
//   ONNET_FLEET_MAX_OWN           the most own machines taking servers (2)
//   ONNET_FLEET_IDLE_MINUTES      how long a shared machine with no server lives
//                                 on before it ends (30)
//   ONNET_FLEET_SLOT_MEMORY_MB    each container's memory, swap none (512)
//   ONNET_FLEET_SLOT_CPUS         each container's CPUs (1; a decimal)
//   ONNET_HOSTED_EC2_LAUNCH_TEMPLATES
//                                 the ec2 driver's launch templates by machine
//                                 kind, {"shared": "lt-...", "own": "lt-..."}
//                                 (infra/aws/fleet.tf's; a template may name a
//                                 version, lt-<id>:<version>)
//   ONNET_HOSTED_HOSTD_IMAGE      the image opennova-hostd is copied out of
//                                 (the deploy's pinned digest; unset, the
//                                 serve image)
//
// and, for the ec2 driver, the variables its provider read before:
// ONNET_HOSTED_EC2_REGION, ONNET_HOSTED_EC2_SUBNET, ONNET_HOSTED_DEPLOYMENT,
// ONNET_HOSTED_SERVE_IMAGE, ONNET_HOSTED_AWS_CLI and ONNET_HOSTED_MACHINE_SCRIPT
// (the path of machine/machine_user_data.sh). The ec2 driver's settings are
// read as they are; Ec2MachineDriver::check_config decides whether they run.
// The service's private address is ONNET_INTERNAL_HTTP_BIND's. Service policy, not a
// port.

#include "ec2_machine_driver.h"
#include "net_cidr.h"
#include "placement.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// A container's process cap (docker run --pids-limit): not a setting.
inline constexpr int kSlotPids = 128;

struct FleetConfig {
	// ONNET_INTERNAL_HTTP_BIND.
	std::string internal_address = "127.0.0.1";
	uint16_t internal_port = 8070;
	// The networks the agent routes answer (agent_route.h): each private, or
	// 127.0.0.0/8 for a loopback bind.
	std::vector<Ipv4Cidr> agent_cidrs = { Ipv4Cidr{ 0x7F000000u, 0xFF000000u } };
	PlacementPolicy placement;
	int idle_minutes = 30;
	int slot_memory_mb = 512;
	double slot_cpus = 1.0;
	int slot_pids = kSlotPids;
	// The ec2 driver's, but its machine script's text (read from
	// machine_script_path by load_machine_script) and its data sets (the ones
	// the deployment offers, which main() knows).
	Ec2MachineConfig ec2;
	std::string machine_script_path = "/app/hosted/machine_user_data.sh";
};

// What the agent routes (agent_route.h) are built with.
struct AgentRouteConfig {
	// The networks the routes answer (FleetConfig::agent_cidrs); one that
	// agent_cidr_allowed refuses is ignored.
	std::vector<Ipv4Cidr> admitted;
	// The bootstrap reply's data sets: each one every machine mounts, and its
	// mount path (machine_driver.h machine_data_set_path).
	std::map<std::string, std::string> data_sets;
};

// The agent routes' settings: `config`'s networks, and each of `data_sets`
// (the games slugs of the data sets the deployment offers) at its mount path.
AgentRouteConfig agent_route_config(
		const FleetConfig &config, const std::vector<std::string> &data_sets);

// Whether `cidr` may be among the agent routes' networks: inside the RFC 1918
// private ranges (ipv4_cidr_private), or inside 127.0.0.0/8.
bool agent_cidr_allowed(const Ipv4Cidr &cidr);
// Whether IPv4 `address` lies in 127.0.0.0/8.
bool ipv4_loopback(uint32_t address);

// The environment, one variable at a time: its value, or nullopt when it is
// unset or empty.
using EnvLookup = std::function<std::optional<std::string>(const char *name)>;

// The fleet's settings from `env`, unset ones at their defaults. Every value
// that does not read, or reads outside its range, keeps its default and adds
// a line to `errors` naming its variable (never a secret: none of these is
// one); true when there was none.
bool parse_fleet_config(const EnvLookup &env, FleetConfig &out, std::vector<std::string> &errors);
// The same from this process's environment.
FleetConfig fleet_config_from_env(std::vector<std::string> &errors);

// ONNET_HOSTED_EC2_LAUNCH_TEMPLATES' text, {"shared": "<template>", "own":
// "<template>"} (infra/aws/fleet.tf's output; either may be missing, which
// Ec2MachineDriver::check_config refuses), into `shared` and `own`; false with
// `why` when it is no such object.
bool parse_launch_templates(
		std::string_view json, std::string &shared, std::string &own, std::string &why);

// The text of the machine script at `path` into `out`; false with `error`.
bool load_machine_script(const std::string &path, std::string &out, std::string &error);

} // namespace opennova::novaworld_server::hosted
