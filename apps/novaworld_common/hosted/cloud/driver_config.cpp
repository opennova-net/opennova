#include "driver_config.h"

#include "address_zone.h"
#include "core/loggable.h"
#include "core/text_util.h"
#include "docker_address_driver.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

// The ec2 driver's settings checked, into `setup`: false with `error`.
bool setup_ec2(DriverConfig &driver, std::string &error) {
	Ec2MachineConfig &ec2 = driver.ec2;
	std::string why;
	if (!load_machine_script(driver.machine_script_path, ec2.machine_script, why)) {
		error = "the ec2 driver: " + loggable(why, 300) + " (ONNET_HOSTED_MACHINE_SCRIPT)";
		return false;
	}
	if (const std::string bad = Ec2MachineDriver::check_config(ec2); !bad.empty()) {
		error = "the ec2 driver is not configured: " + bad;
		return false;
	}
	std::string subnets;
	for (const std::string &subnet : ec2.subnets)
		subnets += (subnets.empty() ? "" : ",") + subnet;
	std::printf("[boot] hosted servers: ec2 in %s, subnets %s, deployment %s, shared machines "
				"from %s, own from %s\n",
			ec2.region.c_str(), subnets.c_str(), ec2.deployment.c_str(),
			ec2.shared_template.c_str(), ec2.own_template.c_str());
	std::printf("[boot] hosted servers: the machines join ECS cluster %s\n", ec2.cluster.c_str());
	return true;
}

// The docker driver's settings checked (the local stack's machines): false
// with `error`.
bool setup_docker(const DriverConfig &driver, std::string &error) {
	const DockerMachineConfig &docker = driver.docker;
	if (const std::string why = DockerMachineDriver::check_config(docker); !why.empty()) {
		error = "the docker driver is not configured: " + why;
		return false;
	}
	std::printf("[boot] hosted servers: docker machines on network %s, the store's mounts "
				"from %s, deployment %s; the machines reach %s\n",
			docker.network.c_str(), docker.data_from.c_str(), docker.deployment.c_str(),
			loggable(docker.service_url, 255).c_str());
	return true;
}

// The static addresses' drivers' settings checked over a policy with them on
// (the play zone's Route 53 id for ec2; the zone file and the addresses for
// docker): false with `error`.
bool setup_addresses(const HostedPolicy &policy, const DriverConfig &driver,
		std::string &error) {
	const DriverConfig::Addresses &a = driver.addresses;
	std::string why;
	if (policy.provider == "ec2") {
		Route53ZoneConfig zone;
		zone.aws_cli = driver.ec2.aws_cli;
		zone.region = driver.ec2.region;
		zone.zone = policy.fleet.address_zone;
		zone.zone_id = a.zone_id;
		why = Route53Zone::check_config(zone);
	} else if (a.zone_file.empty() || a.zone_file[0] != '/') {
		why = "ONNET_HOSTED_ADDRESS_ZONE_FILE is no absolute path";
	} else {
		DockerAddressDriver::parse_range(a.docker_range, why);
	}
	if (!why.empty()) {
		error = "the static addresses' driver is not configured: " + why +
				" (ONNET_FLEET_MAX_ADDRESSES " + std::to_string(policy.fleet.placement.max_addresses) +
				")";
		return false;
	}
	const std::string from = policy.provider == "ec2"
			? "Elastic IPs, their records in Route 53 zone " + a.zone_id
			: "from " + a.docker_range + ", their records in " + loggable(a.zone_file, 260);
	std::printf("[boot] static addresses: %s\n", from.c_str());
	return true;
}

} // namespace

bool parse_launch_templates(
		std::string_view json, std::string &shared, std::string &own, std::string &why) {
	io::JsonValue v;
	std::string error;
	if (!io::json_parse(json, v, error) || !v.is_object()) {
		why = "is no JSON object";
		return false;
	}
	std::string read_shared;
	std::string read_own;
	for (const io::JsonMember &member : v.object) {
		std::string *slot = member.key == machine_kind_text(MachineKind::Shared) ? &read_shared
				: member.key == machine_kind_text(MachineKind::Own)				 ? &read_own
																				 : nullptr;
		if (slot == nullptr) {
			why = "names a machine kind other than shared and own";
			return false;
		}
		if (!member.value.is_string()) {
			why = "gives the " + member.key + " machines no launch template id";
			return false;
		}
		*slot = strutil::trim(member.value.string);
	}
	shared = std::move(read_shared);
	own = std::move(read_own);
	return true;
}

bool parse_driver_config(const EnvLookup &env, const FleetConfig &fleet, DriverConfig &out,
		std::vector<std::string> &errors) {
	const size_t errors_before = errors.size();
	const auto text = [&env](const char *name, std::string &into) {
		if (const auto value = env(name))
			into = strutil::trim(*value);
	};

	if (const auto url = env("ONNET_INTERNAL_URL")) {
		const std::string trimmed = strutil::trim(*url);
		std::string host;
		uint16_t port = 0;
		if (Ec2MachineDriver::parse_service_url(trimmed, host, port))
			out.internal_url = trimmed;
		else
			errors.push_back("ONNET_INTERNAL_URL is no http://<host>:<port>");
	}

	Ec2MachineConfig &ec2 = out.ec2;
	if (const auto templates = env("ONNET_HOSTED_EC2_LAUNCH_TEMPLATES")) {
		std::string why;
		if (!parse_launch_templates(*templates, ec2.shared_template, ec2.own_template, why))
			errors.push_back("ONNET_HOSTED_EC2_LAUNCH_TEMPLATES " + why);
	}
	text("ONNET_HOSTED_EC2_REGION", ec2.region);
	text("ONNET_HOSTED_EC2_CLUSTER", ec2.cluster);
	if (const auto subnets = env("ONNET_HOSTED_EC2_SUBNETS"))
		ec2.subnets = split_list(*subnets);
	text("ONNET_HOSTED_DEPLOYMENT", ec2.deployment);
	text("ONNET_HOSTED_AWS_CLI", ec2.aws_cli);
	text("ONNET_HOSTED_MACHINE_SCRIPT", out.machine_script_path);
	ec2.serve_image = fleet.serve_image;

	DockerMachineConfig &docker = out.docker;
	text("ONNET_HOSTED_DOCKER_URL", docker.engine_url);
	text("ONNET_HOSTED_DOCKER_NETWORK", docker.network);
	text("ONNET_HOSTED_DOCKER_DATA_FROM", docker.data_from);
	docker.image = fleet.hostd_image;
	docker.deployment = ec2.deployment;
	docker.service_url = out.internal_url;

	text("ONNET_HOSTED_ADDRESS_ZONE_ID", out.addresses.zone_id);
	text("ONNET_HOSTED_ADDRESS_ZONE_FILE", out.addresses.zone_file);
	text("ONNET_HOSTED_DOCKER_ADDRESSES", out.addresses.docker_range);
	return errors.size() == errors_before;
}

bool load_machine_script(const std::string &path, std::string &out, std::string &error) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		error = "cannot read the machine script " + path;
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	if (out.empty()) {
		error = "the machine script " + path + " is empty";
		return false;
	}
	return true;
}

bool driver_setup(const HostedPolicy &policy, DriverSetup &out, std::string &error) {
	std::vector<std::string> errors;
	if (!parse_driver_config(process_env(), policy.fleet, out.driver, errors)) {
		error = "the driver's settings do not read: " + loggable(errors.front(), 300);
		return false;
	}
	const bool machines = policy.provider == "ec2" ? setup_ec2(out.driver, error)
												   : setup_docker(out.driver, error);
	if (!machines)
		return false;
	return !policy.addresses_on || setup_addresses(policy, out.driver, error);
}

} // namespace opennova::novaworld_server::hosted
