#pragma once

// The fleet's settings from the service's environment, checked, in two sets.
// The fleet's policy (FleetConfig, parse_fleet_config; web, the fleet program
// and the worker read it alike, so each decides alike whether hosting is on):
// where the internal listener binds and whom it answers, placement's knobs
// (placement.h), the slots' limits every container gets and the serve image a
// start pins. The machine drivers' own (DriverConfig, parse_driver_config; the
// worker's alone, which launches the machines): what the drivers launch from
// (ec2_machine_driver.h, docker_machine_driver.h) and where the machines reach
// the fleet. The policy's variables:
//
//   ONNET_INTERNAL_HTTP_BIND      <IPv4>:<port> (127.0.0.1:8070): the
//                                 fleet's internal listener
//                                 (internal_listener.h), on the fleet task's
//                                 private address in a deployment (unset
//                                 there: the fleet finds it in its ECS task's
//                                 metadata, ecs_metadata.h); a private or
//                                 loopback address, never a public or
//                                 wildcard one. Also the address the
//                                 machines' admin.cfg admits (the fleet's
//                                 admin links come from it)
//   ONNET_HOSTED_BOOTSTRAP_CIDRS  comma-separated private IPv4 CIDRs: the
//                                 networks the agents' requests may come from
//                                 (the machines' subnets); none, on a loopback
//                                 bind, admits 127.0.0.0/8
//   ONNET_FLEET_SHARED_SLOTS      a new shared machine's slots (12; 1..32)
//   ONNET_FLEET_MAX_SHARED        the most shared machines taking servers (2)
//   ONNET_FLEET_MAX_OWN           the most own machines taking servers (2)
//   ONNET_FLEET_MAX_ADDRESSES     the static addresses the fleet sells (0: the
//                                 add-on is off; sold_capacity.h)
//   ONNET_FLEET_IDLE_MINUTES      how long a shared machine with no server lives
//                                 on before it ends (30)
//   ONNET_FLEET_SLOT_MEMORY_MB    each container's memory, swap none (512)
//   ONNET_FLEET_SLOT_CPUS         each container's CPUs (1; a decimal)
//   ONNET_HOSTED_SERVE_IMAGE      the serve image every start pins (the
//                                 deploy's pinned digest)
//   ONNET_FLEET_IMAGE_ID          the image the launch templates pin (the
//                                 deployment's fleet_ami, an ami-...), which
//                                 a fleet roll's preview holds each machine's
//                                 against; "" with none
//
// The drivers' variables:
//
//   ONNET_INTERNAL_URL            http://<host>:<port>: the fleet's internal
//                                 listener as the machines' agents reach it, by
//                                 the name the deployment's Cloud Map namespace
//                                 keeps on the fleet leader (fleet.<deployment>.
//                                 opennova.internal:8070; the local stack's:
//                                 its compose name), never a fixed address
//   ONNET_HOSTED_DEPLOYMENT       the tag (ec2) or label (docker) every machine
//                                 carries and every call is scoped to
//   ONNET_HOSTED_EC2_LAUNCH_TEMPLATES
//                                 the ec2 driver's launch templates by machine
//                                 kind, {"shared": "lt-...", "own": "lt-..."}
//                                 (infra/aws/fleet.tf's; a template may name a
//                                 version, lt-<id>:<version>)
//   ONNET_HOSTED_EC2_CLUSTER      the fleet cluster the ec2 driver's machines
//                                 join (infra/aws/fleet_agent.tf's
//                                 opennova-<deployment>-fleet)
//
// and, for the ec2 driver, ONNET_HOSTED_EC2_REGION, ONNET_HOSTED_EC2_SUBNETS
// (comma-separated, a subnet per zone), ONNET_HOSTED_AWS_CLI and
// ONNET_HOSTED_MACHINE_SCRIPT (the path of machine/machine_user_data.sh); for
// the docker driver (the local stack's), ONNET_HOSTED_DOCKER_URL (the Docker
// Engine API, http://<host>:<port>), ONNET_HOSTED_DOCKER_NETWORK (the network
// its machines join), ONNET_HOSTED_DOCKER_DATA_FROM (the container holding
// the data sets' mounts) and ONNET_HOSTED_HOSTD_IMAGE (the machines' image:
// opennova-hostd with opennova-serve beside it). Each driver's settings are
// read as they are; its check_config decides whether they run. Service
// policy, not a port.

#include "docker_machine_driver.h"
#include "ec2_machine_driver.h"
#include "net_cidr.h"
#include "placement.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// A container's process cap (docker run --pids-limit): not a setting.
inline constexpr int kSlotPids = 128;

// The fleet's policy.
struct FleetConfig {
	// ONNET_INTERNAL_HTTP_BIND (the fleet's).
	std::string internal_address = "127.0.0.1";
	uint16_t internal_port = 8070;
	// The networks the agent routes answer (agent_route.h): each private, or
	// 127.0.0.0/8 for a loopback bind.
	std::vector<Ipv4Cidr> agent_cidrs = { Ipv4Cidr{ 0x7F000000u, 0xFF000000u } };
	PlacementPolicy placement;
	// ONNET_FLEET_IMAGE_ID: the image the launch templates pin ("": none).
	std::string image_id;
	int idle_minutes = 30;
	int slot_memory_mb = 512;
	double slot_cpus = 1.0;
	int slot_pids = kSlotPids;
	// ONNET_HOSTED_SERVE_IMAGE ("" when unset).
	std::string serve_image;
};

// The machine drivers' own settings (the worker's).
struct DriverConfig {
	// ONNET_INTERNAL_URL ("" when unset).
	std::string internal_url;
	// The ec2 driver's, but its machine script's text (read from
	// machine_script_path by load_machine_script).
	Ec2MachineConfig ec2;
	std::string machine_script_path = "/app/hosted/machine_user_data.sh";
	// The docker driver's.
	DockerMachineConfig docker;
};

// What the agent routes (agent_route.h) are built with.
struct AgentRouteConfig {
	// The networks the routes answer (FleetConfig::agent_cidrs); one that
	// agent_cidr_allowed refuses is ignored.
	std::vector<Ipv4Cidr> admitted;
};

// The agent routes' settings: `config`'s networks.
AgentRouteConfig agent_route_config(const FleetConfig &config);

// Whether `cidr` may be among the agent routes' networks: inside the RFC 1918
// private ranges (ipv4_cidr_private), or inside 127.0.0.0/8.
bool agent_cidr_allowed(const Ipv4Cidr &cidr);
// Whether IPv4 `address` lies in 127.0.0.0/8.
bool ipv4_loopback(uint32_t address);

// The environment, one variable at a time: its value, or nullopt when it is
// unset or empty.
using EnvLookup = std::function<std::optional<std::string>(const char *name)>;

// This process's environment as an EnvLookup (an empty value is unset).
EnvLookup process_env();

// The fleet's policy from `env`, unset settings at their defaults. Every value
// that does not read, or reads outside its range, keeps its default and adds
// a line to `errors` naming its variable (never a secret: none of these is
// one); true when there was none.
bool parse_fleet_config(const EnvLookup &env, FleetConfig &out, std::vector<std::string> &errors);

// The drivers' settings from `env` over the policy `fleet` (the drivers' serve
// image and the address their machines' admin.cfg admits, its bind address,
// are the policy's), as parse_fleet_config reads its own.
bool parse_driver_config(const EnvLookup &env, const FleetConfig &fleet, DriverConfig &out,
		std::vector<std::string> &errors);

// ONNET_HOSTED_EC2_LAUNCH_TEMPLATES' text, {"shared": "<template>", "own":
// "<template>"} (infra/aws/fleet.tf's output; either may be missing, which
// Ec2MachineDriver::check_config refuses), into `shared` and `own`; false with
// `why` when it is no such object.
bool parse_launch_templates(
		std::string_view json, std::string &shared, std::string &own, std::string &why);

// The text of the machine script at `path` into `out`; false with `error`.
bool load_machine_script(const std::string &path, std::string &out, std::string &error);

} // namespace opennova::novaworld_server::hosted
