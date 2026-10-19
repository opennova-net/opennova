#pragma once

// What the supervisor knows of its driver's machines without the cloud and
// without a driver (machine_driver.h): the driver's name, how long a machine
// may take to boot, a slot's ports and the address its admin.cfg admits. The
// fleet's policy names the driver (ONNET_HOSTED_PROVIDER) and the fleet's
// bind (fleet_config.h), so web, the fleet program and the worker make the
// same profile from the same settings. Service policy, not a port.

#include "hosted/protocol/slots.h"

#include <cstdint>
#include <functional>
#include <string>

namespace opennova::novaworld_server::hosted {

// A slot's ports on a machine and the address its admin.cfg admits (the
// service, as the machine sees it): what the server's files are rendered with.
struct SlotPlan {
	uint16_t game_port = 0;
	uint16_t admin_port = 0;
	std::string admin_allowed_ip;
};

// What the supervisor knows of its driver's machines without asking the cloud.
struct MachineProfile {
	// The driver's name, stored with each machine (fleet_machines.provider).
	std::string provider;
	// How long one of its machines may take from the launch to its agent's
	// first report before the supervisor gives up on it.
	uint64_t boot_timeout_ms = 0;
	// The ports slot `slot` of one of its machines serves on, and the address
	// its admin.cfg admits: no cloud call (each driver's are the slot's fixed
	// ports, hosted/protocol/slots.h: every machine has an address of its own).
	std::function<SlotPlan(int slot)> slot_plan;
};

// From the launch to the agent's first report on an EC2 machine: EC2's
// pending, cloud-init, the ECS agent's registration, the hostd image's pull,
// the agent's start and its bootstrap.
inline constexpr uint64_t kEc2BootTimeoutMs = 15 * 60 * 1000;
// On a docker machine (the local stack's): a container's start and a
// bootstrap.
inline constexpr uint64_t kDockerBootTimeoutMs = 2 * 60 * 1000;

// The ip_restrict pattern a machine's admin.cfg admits the fleet by: its
// private address's /16, "10.44.*" for 10.44.0.17 (the deployment's VPC).
// The fleet leader's address moves with each replacement of its task, and
// a server keeps the admin.cfg its start rendered, so a pattern naming one
// address would refuse a new leader's admin links (and its health checks
// would then fail every running server); the machines' security group
// admits the admin ports from the fleet's group alone. `fleet_address`
// itself when it is no dotted quad.
std::string admin_pattern(const std::string &fleet_address);

// The ec2 driver's machines (hosted/cloud/ec2_machine_driver.h) and the
// docker driver's (hosted/cloud/docker_machine_driver.h) as the supervisor
// sees them: kEc2BootTimeoutMs or kDockerBootTimeoutMs, and slot k's fixed
// ports (32768 + k, 32800 + k) with the network of `fleet_address`, the
// fleet's internal listener's bind (ONNET_INTERNAL_HTTP_BIND), which its
// admin links connect from (admin_pattern), for admin.cfg's ip_restrict.
MachineProfile ec2_machine_profile(const std::string &fleet_address);
MachineProfile docker_machine_profile(const std::string &fleet_address);

} // namespace opennova::novaworld_server::hosted
