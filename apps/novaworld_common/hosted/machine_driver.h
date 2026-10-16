#pragma once

// The fleet's one seam to the cloud: a driver launches the machine a
// fleet_machines row stands for (fleet_repository.h), says what the cloud says
// of it, ends it and lists the machines it runs. It decides nothing: the
// supervisor places servers (placement.h), moves the rows and ends machines,
// and each machine's agent (opennova-hostd) runs what the agent routes tell it
// (agent_route.h). A machine is never a host (CONTEXT.md "Machine"). Service
// policy, not a port.
//
// Only the job workers call a driver (fleet_jobs.h: machine.launch,
// machine.describe, machine.terminate, fleet.sweep); the supervisor's pass
// never does. What the pass knows of its machines without the cloud (the
// driver's name, its boot timeout, a slot's ports) is the driver's
// MachineProfile, which needs no driver to make.
//
// Drivers: Ec2MachineDriver (ec2_machine_driver.h, an EC2 machine from the
// launch template of its kind, whose user-data installs the agent) and
// DockerMachineDriver (docker_machine_driver.h, the local stack's: a container
// of the serve image on the stack's network, its agent the image's own, whose
// servers are opennova-serve children of the agent).
//
// A launch carries the row's id and its one-time bootstrap token and nothing
// else: the machine's agent redeems the token for its long-lived secret over
// the internal listener (POST /hosted/machines/<id>/bootstrap), then asks what
// to run (POST /hosted/machines/<id>/sync). No server's file or credential is
// in a launch.

#include "placement.h"
#include "slots.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// Where a cloud machine mounts each data set it holds, read-only:
// /opt/game/<data set>, the bind source of that data set's containers (their
// /opt/game). The bootstrap reply names these paths (agent_protocol.h).
inline constexpr const char *kMachineGameRoot = "/opt/game";
inline std::string machine_data_set_path(std::string_view data_set) {
	return std::string(kMachineGameRoot) + "/" + std::string(data_set);
}

// What a launch asks for.
struct MachineLaunch {
	int64_t machine_id = 0; // the fleet_machines row (the agent's --machine)
	MachineKind kind = MachineKind::Shared;
	int slots = 1; // the row's slots
	// The one-time bootstrap token (fleet_repository.h create_machine): the
	// launch hands it to the machine and keeps it nowhere.
	std::string bootstrap_token;
	// Its machine.launch job's run (fleet_jobs.h): 1, or past it a run again
	// after one cut short (a claim that lapsed, a database error), whose first
	// run may have launched the machine already.
	int attempt = 1;
	// The subnet an earlier run last called the cloud in, as its row recorded
	// it (fleet_machines.launch_subnet; "" when none did): a driver with
	// subnets calls there first, under the same client token, so the cloud
	// answers with the machine that call launched however late it lists it.
	std::string subnet;
	// Records on the row the subnet the launch is about to call the cloud in,
	// before each subnet's first call (the machine.launch job writes it). False
	// when it could not (a database error, or the row left launching): the
	// launch then calls nothing more and fails. Null: nothing is recorded.
	std::function<bool(const std::string &subnet)> record_subnet;
};

// The cloud's answer to a launch.
struct LaunchedMachine {
	std::string cloud_id; // the driver's handle (an EC2 instance id)
	// Where the service reaches the machine (the admin links) and where its
	// agent's requests come from: the agent routes answer that address alone.
	std::string private_address;
	// Where players reach its slots; empty until the cloud gives it one.
	std::string public_address;
	std::string instance_type;
	std::string template_version; // the launch template and its version
};

enum class CloudState : uint8_t {
	Pending, // launched, not running yet
	Running,
	Gone, // ending, ended, or never known
	Unknown, // the cloud did not answer: nothing may be decided on it
};

struct CloudMachine {
	CloudState state = CloudState::Gone;
	std::string private_address;
	std::string public_address;
};

// A machine the driver runs for this deployment, with the row id its tag names
// (0 when the tag names none).
struct TaggedCloudMachine {
	std::string cloud_id;
	int64_t machine_id = 0;
};

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
	// ports, slots.h: every machine has an address of its own).
	std::function<SlotPlan(int slot)> slot_plan;
};

class MachineDriver {
public:
	virtual ~MachineDriver() = default;

	// The driver's name (its MachineProfile's provider).
	virtual const char *name() const = 0;
	// Starts a machine for `launch`: what the cloud answered, or nullopt with
	// `error` (nothing runs then, or what runs carries the tags and the
	// supervisor's sweep ends it).
	virtual std::optional<LaunchedMachine> launch(
			const MachineLaunch &launch, std::string &error) = 0;
	// What the cloud says of machine `cloud_id`, whose launch answered
	// `launched_ago_ms` ago by its row's clock (fleet_repository.h
	// launched_ago_ms: the database's, so whichever worker describes it, its
	// launch's or another, judges the launch grace alike; a negative value
	// for a machine with no row). A cloud that lists a launch late reads it
	// Pending within the driver's grace, not Gone.
	virtual CloudMachine describe(const std::string &cloud_id, int64_t launched_ago_ms) = 0;
	// Ends the machine and everything on it; idempotent, and a machine that is
	// gone already is no error.
	virtual void terminate(const std::string &cloud_id) = 0;
	// Every machine the driver runs for this deployment (the boot's
	// reconciliation, the periodic sweep). A driver that cannot tell (a cloud
	// listing that failed) throws std::runtime_error rather than answer short,
	// so the supervisor ends nothing on it.
	virtual std::vector<TaggedCloudMachine> list_tagged() = 0;
};

} // namespace opennova::novaworld_server::hosted
