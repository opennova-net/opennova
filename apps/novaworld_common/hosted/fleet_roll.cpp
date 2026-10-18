#include "fleet_roll.h"

namespace opennova::novaworld_server::hosted {

bool machine_rollable(FleetMachineState state) {
	return state == FleetMachineState::Launching || state == FleetMachineState::Booting ||
			state == FleetMachineState::Ready;
}

std::vector<RollStep> plan_fleet_roll(const std::vector<FleetMachineRow> &machines,
		const std::map<int64_t, std::string> &image_of, const std::map<int64_t, int> &servers_of,
		const std::string &pinned_image) {
	std::vector<RollStep> out;
	for (const FleetMachineRow &m : machines) {
		if (!machine_rollable(m.state))
			continue;
		RollStep step;
		step.machine_id = m.id;
		step.kind = m.kind;
		step.state = m.state;
		if (const auto it = image_of.find(m.id); it != image_of.end())
			step.image_id = it->second;
		if (const auto it = servers_of.find(m.id); it != servers_of.end())
			step.servers = it->second;
		if (pinned_image.empty()) {
			step.replace = true;
			step.why = "no image is pinned here: every machine is replaced";
		} else if (step.image_id.empty()) {
			step.replace = true;
			step.why = "its image is not known here";
		} else if (step.image_id != pinned_image) {
			step.replace = true;
			step.why = "it runs " + step.image_id + ", not the pinned " + pinned_image;
		} else {
			step.why = "it runs the pinned image";
		}
		out.push_back(std::move(step));
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
