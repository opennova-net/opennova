#pragma once

// The machine drivers' own settings from the service's environment, checked:
// the worker's alone, which launches the machines (the fleet's policy, which
// every program reads alike, is fleet_config.h's). What the drivers launch
// from (ec2_machine_driver.h, docker_machine_driver.h) and where the machines
// reach the fleet. The variables:
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
//   ONNET_HOSTED_ADDRESS_ZONE_ID  ec2, with static addresses on: the play
//                                 zone's Route 53 id (Z...; address_zone.h
//                                 Route53Zone)
//   ONNET_HOSTED_ADDRESS_ZONE_FILE
//                                 docker, with static addresses on: the zone
//                                 file the worker writes (address_zone.h
//                                 FileZone), absolute
//   ONNET_HOSTED_DOCKER_ADDRESSES docker, with static addresses on: the
//                                 addresses it hands out (172.30.77.64/28;
//                                 docker_address_driver.h)
//
// and, for the ec2 driver, ONNET_HOSTED_EC2_REGION, ONNET_HOSTED_EC2_SUBNETS
// (comma-separated, a subnet per zone), ONNET_HOSTED_AWS_CLI and
// ONNET_HOSTED_MACHINE_SCRIPT (the path of machine/machine_user_data.sh); for
// the docker driver (the local stack's), ONNET_HOSTED_DOCKER_URL (the Docker
// Engine API, http://<host>:<port>), ONNET_HOSTED_DOCKER_NETWORK (the network
// its machines join), ONNET_HOSTED_DOCKER_DATA_FROM (the container holding
// the data sets' mounts), its machines' image the policy's hostd image
// (opennova-hostd with opennova-serve beside it). Each driver's settings are
// read as they are; its check_config decides whether they run. Service
// policy, not a port.

#include "docker_machine_driver.h"
#include "ec2_machine_driver.h"
#include "hosted/model/fleet_config.h"

#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

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
	// The static addresses' drivers' own (the variables above; the zone's name
	// is the policy's): read as they are, checked by the setup (parts.h).
	struct Addresses {
		std::string zone_id;
		std::string zone_file;
		std::string docker_range;
	} addresses;
};

// The drivers' settings from `env` over the policy `fleet` (the drivers' serve
// image and the docker driver's machines' hostd image are the policy's), as
// parse_fleet_config reads its own: every value that does not read keeps its
// default and adds a line to `errors` naming its variable; true when there
// was none.
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
