#include "fleet_config.h"
#include "text_util.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace opennova::novaworld_server::hosted {

namespace {

constexpr Ipv4Cidr kLoopback = { 0x7F000000u, 0xFF000000u }; // 127.0.0.0/8

// A whole number in decimal digits alone (no sign, no blank) within [lo, hi].
std::optional<int64_t> whole_in(const std::string &text, int64_t lo, int64_t hi) {
	if (text.empty() || text.size() > 18 || !strutil::all_digits(text))
		return std::nullopt;
	const auto value = strutil::parse_llong(text);
	if (!value || *value < lo || *value > hi)
		return std::nullopt;
	return static_cast<int64_t>(*value);
}

// A decimal ("1", "0.5", "2.25") within (lo, hi].
std::optional<double> decimal_in(const std::string &text, double lo, double hi) {
	if (text.empty() || text.size() > 16)
		return std::nullopt;
	size_t dots = 0;
	for (const char c : text) {
		if (c == '.')
			++dots;
		else if (c < '0' || c > '9')
			return std::nullopt;
	}
	if (dots > 1 || text.front() == '.' || text.back() == '.')
		return std::nullopt;
	const auto value = strutil::parse_double(text);
	if (!value || !(*value > lo && *value <= hi))
		return std::nullopt;
	return *value;
}

struct Reader {
	const EnvLookup &env;
	std::vector<std::string> &errors;

	template <typename T> void whole(const char *name, int64_t lo, int64_t hi, T &out) {
		const auto text = env(name);
		if (!text)
			return;
		const auto value = whole_in(*text, lo, hi);
		if (!value) {
			errors.push_back(std::string(name) + " is no whole number in " + std::to_string(lo) +
					".." + std::to_string(hi));
			return;
		}
		out = static_cast<T>(*value);
	}

	void text(const char *name, std::string &out) {
		if (const auto value = env(name))
			out = strutil::trim(*value);
	}
};

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

AgentRouteConfig agent_route_config(const FleetConfig &config) {
	AgentRouteConfig out;
	out.admitted = config.agent_cidrs;
	return out;
}

bool ipv4_loopback(uint32_t address) {
	return (address & kLoopback.mask) == kLoopback.network;
}

bool agent_cidr_allowed(const Ipv4Cidr &cidr) {
	if (ipv4_cidr_private(cidr))
		return true;
	return (cidr.mask & kLoopback.mask) == kLoopback.mask && ipv4_loopback(cidr.network);
}

bool parse_fleet_config(const EnvLookup &env, FleetConfig &out, std::vector<std::string> &errors) {
	const size_t errors_before = errors.size();
	Reader read{ env, errors };

	bool loopback_bind = true;
	if (const auto bind = env("ONNET_INTERNAL_HTTP_BIND")) {
		const std::string text = strutil::trim(*bind);
		const size_t colon = text.rfind(':');
		const auto address =
				colon == std::string::npos ? std::nullopt : parse_ipv4(text.substr(0, colon));
		const auto port = colon == std::string::npos ? std::nullopt
													 : whole_in(text.substr(colon + 1), 1, 65535);
		if (!address || !port) {
			errors.push_back("ONNET_INTERNAL_HTTP_BIND is no <IPv4>:<port>");
		} else if (!agent_cidr_allowed(Ipv4Cidr{ *address, ~uint32_t{ 0 } })) {
			errors.push_back("ONNET_INTERNAL_HTTP_BIND names no private or loopback address "
							 "(the internal listener never faces the internet)");
		} else {
			out.internal_address = text.substr(0, colon);
			out.internal_port = static_cast<uint16_t>(*port);
			loopback_bind = ipv4_loopback(*address);
		}
	}

	std::vector<Ipv4Cidr> cidrs;
	bool cidrs_read = true;
	if (const auto list = env("ONNET_HOSTED_BOOTSTRAP_CIDRS")) {
		for (const std::string &entry : split_list(*list)) {
			const auto cidr = parse_ipv4_cidr(entry);
			if (!cidr || !ipv4_cidr_private(*cidr)) {
				errors.push_back("ONNET_HOSTED_BOOTSTRAP_CIDRS holds '" + entry.substr(0, 32) +
						"', no private IPv4 network (10/8, 172.16/12, 192.168/16)");
				cidrs_read = false;
				continue;
			}
			cidrs.push_back(*cidr);
		}
	}
	if (cidrs_read) {
		if (loopback_bind)
			cidrs.push_back(kLoopback);
		if (cidrs.empty())
			errors.push_back("ONNET_HOSTED_BOOTSTRAP_CIDRS names no network the machines reach "
							 "the internal listener from");
		else
			out.agent_cidrs = std::move(cidrs);
	}

	PlacementPolicy placement = out.placement;
	read.whole("ONNET_FLEET_SHARED_SLOTS", 1, kMaxSlots, placement.shared_slots);
	read.whole("ONNET_FLEET_MAX_SHARED", 0, 64, placement.max_shared);
	read.whole("ONNET_FLEET_MAX_OWN", 0, 64, placement.max_own);
	read.whole("ONNET_FLEET_MAX_ADDRESSES", 0, 64, placement.max_addresses);
	if (check_placement_policy(placement).empty())
		out.placement = placement;
	read.whole("ONNET_FLEET_IDLE_MINUTES", 1, 7 * 24 * 60, out.idle_minutes);
	read.whole("ONNET_FLEET_SLOT_MEMORY_MB", 64, 65536, out.slot_memory_mb);
	if (const auto cpus = env("ONNET_FLEET_SLOT_CPUS")) {
		const auto value = decimal_in(strutil::trim(*cpus), 0.0, 64.0);
		if (value)
			out.slot_cpus = *value;
		else
			errors.push_back("ONNET_FLEET_SLOT_CPUS is no decimal above 0 and at most 64");
	}

	read.text("ONNET_HOSTED_SERVE_IMAGE", out.serve_image);
	read.text("ONNET_HOSTED_HOSTD_IMAGE", out.hostd_image);
	read.text("ONNET_FLEET_IMAGE_ID", out.image_id);
	read.text("ONNET_HOSTED_ADDRESS_ZONE", out.address_zone);
	return errors.size() == errors_before;
}

bool parse_driver_config(const EnvLookup &env, const FleetConfig &fleet, DriverConfig &out,
		std::vector<std::string> &errors) {
	const size_t errors_before = errors.size();
	Reader read{ env, errors };

	if (const auto url = env("ONNET_INTERNAL_URL")) {
		const std::string text = strutil::trim(*url);
		std::string host;
		uint16_t port = 0;
		if (Ec2MachineDriver::parse_service_url(text, host, port))
			out.internal_url = text;
		else
			errors.push_back("ONNET_INTERNAL_URL is no http://<host>:<port>");
	}

	Ec2MachineConfig &ec2 = out.ec2;
	if (const auto templates = env("ONNET_HOSTED_EC2_LAUNCH_TEMPLATES")) {
		std::string why;
		if (!parse_launch_templates(*templates, ec2.shared_template, ec2.own_template, why))
			errors.push_back("ONNET_HOSTED_EC2_LAUNCH_TEMPLATES " + why);
	}
	read.text("ONNET_HOSTED_EC2_REGION", ec2.region);
	read.text("ONNET_HOSTED_EC2_CLUSTER", ec2.cluster);
	if (const auto subnets = env("ONNET_HOSTED_EC2_SUBNETS"))
		ec2.subnets = split_list(*subnets);
	read.text("ONNET_HOSTED_DEPLOYMENT", ec2.deployment);
	read.text("ONNET_HOSTED_AWS_CLI", ec2.aws_cli);
	read.text("ONNET_HOSTED_MACHINE_SCRIPT", out.machine_script_path);
	ec2.serve_image = fleet.serve_image;
	ec2.admin_address = fleet.internal_address;

	DockerMachineConfig &docker = out.docker;
	read.text("ONNET_HOSTED_DOCKER_URL", docker.engine_url);
	read.text("ONNET_HOSTED_DOCKER_NETWORK", docker.network);
	read.text("ONNET_HOSTED_DOCKER_DATA_FROM", docker.data_from);
	docker.image = fleet.hostd_image;
	docker.deployment = ec2.deployment;
	docker.service_url = out.internal_url;
	docker.admin_address = fleet.internal_address;

	read.text("ONNET_HOSTED_ADDRESS_ZONE_ID", out.addresses.zone_id);
	read.text("ONNET_HOSTED_ADDRESS_ZONE_FILE", out.addresses.zone_file);
	read.text("ONNET_HOSTED_DOCKER_ADDRESSES", out.addresses.docker_range);
	return errors.size() == errors_before;
}

EnvLookup process_env() {
	return [](const char *name) -> std::optional<std::string> {
		const char *value = std::getenv(name);
		if (value == nullptr || *value == '\0')
			return std::nullopt;
		return std::string(value);
	};
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
