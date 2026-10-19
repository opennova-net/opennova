#include "machine_profile.h"

#include "net_cidr.h"

namespace opennova::novaworld_server::hosted {

namespace {

MachineProfile profile_of(const char *provider, uint64_t boot_timeout_ms,
		const std::string &fleet_address) {
	MachineProfile out;
	out.provider = provider;
	out.boot_timeout_ms = boot_timeout_ms;
	out.slot_plan = [pattern = admin_pattern(fleet_address)](int slot) {
		SlotPlan plan;
		plan.game_port = slot_game_port(slot);
		plan.admin_port = slot_admin_port(slot);
		plan.admin_allowed_ip = pattern;
		return plan;
	};
	return out;
}

} // namespace

std::string admin_pattern(const std::string &fleet_address) {
	const auto address = parse_ipv4(fleet_address);
	if (!address)
		return fleet_address;
	return std::to_string(*address >> 24) + "." + std::to_string((*address >> 16) & 0xFFu) + ".*";
}

MachineProfile ec2_machine_profile(const std::string &fleet_address) {
	return profile_of("ec2", kEc2BootTimeoutMs, fleet_address);
}

MachineProfile docker_machine_profile(const std::string &fleet_address) {
	return profile_of("docker", kDockerBootTimeoutMs, fleet_address);
}

} // namespace opennova::novaworld_server::hosted
