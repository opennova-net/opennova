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
// MachineProfile (machine_profile.h), which needs no driver to make.
//
// Drivers: Ec2MachineDriver (hosted/cloud/ec2_machine_driver.h, an EC2 machine from the
// launch template of its kind, whose user-data installs the agent) and
// DockerMachineDriver (hosted/cloud/docker_machine_driver.h, the local stack's: a container
// of the serve image on the stack's network, its agent the image's own, whose
// servers are opennova-serve children of the agent).
//
// A launch carries the row's id and its one-time bootstrap token and nothing
// else: the machine's agent redeems the token for its long-lived secret over
// the internal listener (POST /hosted/machines/<id>/bootstrap), then asks what
// to run (POST /hosted/machines/<id>/sync). No server's file or credential is
// in a launch.

#include "machine_profile.h"
#include "placement.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

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
	// The static address it carries (address_repository.h), "" for none: the
	// docker driver starts the machine at it; on EC2 the worker associates it
	// once the machine is launched (address_jobs.h address.associate).
	std::string address;
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
