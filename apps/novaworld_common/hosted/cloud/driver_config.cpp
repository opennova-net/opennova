#include "driver_config.h"

#include "core/text_util.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <fstream>
#include <iterator>
#include <utility>

namespace opennova::novaworld_server::hosted {

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

} // namespace opennova::novaworld_server::hosted
