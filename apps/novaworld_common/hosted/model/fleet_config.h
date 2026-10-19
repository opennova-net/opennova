#pragma once

// The fleet's settings from the service's environment, checked, in two sets.
// The fleet's policy (FleetConfig, parse_fleet_config; web, the fleet program
// and the worker read it alike, so each decides alike whether hosting is on):
// where the internal listener binds and whom it answers, placement's knobs
// (placement.h), the slots' limits every container gets and the serve image a
// start pins. The machine drivers' own are the worker's alone, which launches
// the machines (hosted/cloud/driver_config.h). The policy's variables:
//
//   ONNET_INTERNAL_HTTP_BIND      <IPv4>:<port> (127.0.0.1:8070): the
//                                 fleet's internal listener
//                                 (internal_listener.h), on the fleet task's
//                                 private address in a deployment (unset
//                                 there: the fleet finds it in its ECS task's
//                                 metadata, core/ecs_metadata.h); a private or
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
//   ONNET_FLEET_MAX_OWN           the most dedicated machines taking servers (2)
//   ONNET_FLEET_MAX_ADDRESSES     the static addresses the fleet sells (0: the
//                                 add-on is off; sold_capacity.h)
//   ONNET_HOSTED_ADDRESS_ZONE     the play zone the addresses' names live in
//                                 (play.<domain>; infra/aws/edge.tf), no
//                                 trailing dot: the site shows each address
//                                 as <name>.<zone>
//   ONNET_FLEET_IDLE_MINUTES      how long a shared machine with no server lives
//                                 on before it ends (30)
//   ONNET_FLEET_SLOT_MEMORY_MB    each container's memory, swap none (512)
//   ONNET_FLEET_SLOT_CPUS         each container's CPUs (1; a decimal)
//   ONNET_HOSTED_SERVE_IMAGE      the serve image every start pins (the
//                                 deploy's pinned digest)
//   ONNET_FLEET_IMAGE_ID          the image the launch templates pin (the
//                                 deployment's fleet_ami, an ami-...), which
//                                 a fleet roll's preview and the fleet's
//                                 health (fleet_health.h) hold each machine's
//                                 against; "" with none
//   ONNET_HOSTED_HOSTD_IMAGE      the hostd image the release pins (app
//                                 deploy's, by digest): the image the docker
//                                 driver's machines run, and what the fleet's
//                                 health holds each machine's agent against;
//                                 "" with none
//
// Service policy, not a port.

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
	// ONNET_HOSTED_HOSTD_IMAGE ("" when unset).
	std::string hostd_image;
	// ONNET_HOSTED_ADDRESS_ZONE: the play zone the static addresses' names
	// live in ("" when unset; the policy checks it, hosted_policy.h).
	std::string address_zone;
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

} // namespace opennova::novaworld_server::hosted
