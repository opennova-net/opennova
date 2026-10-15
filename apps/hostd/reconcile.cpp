#include "reconcile.h"

namespace opennova::hostd {

Plan plan_sync(const PlanInput &in) {
	Plan plan;
	std::set<std::string> desired_names;
	for (const hosted::DesiredContainer &d : *in.desired)
		desired_names.insert(d.name);

	std::set<int> held_slots;
	std::set<std::string> observed_names;
	for (const ContainerInfo &c : *in.observed) {
		observed_names.insert(c.name);
		held_slots.insert(c.slot);
		if (desired_names.count(c.name)) {
			if (c.state == hosted::ContainerState::Starting)
				plan.busy = true;
			continue;
		}
		plan.busy = true;
		if (c.state == hosted::ContainerState::Running) {
			const auto asked = in.stop_requested_ms->find(c.name);
			if (asked == in.stop_requested_ms->end())
				plan.stop.push_back(c.name);
			else if (in.now_ms - asked->second >= kStopTimeoutMs)
				plan.kill.push_back(c.name);
		} else {
			// Exited, or created and never started: nothing runs to stop.
			plan.remove.push_back(c);
		}
	}

	for (size_t i = 0; i < in.desired->size(); ++i) {
		const hosted::DesiredContainer &d = (*in.desired)[i];
		if (observed_names.count(d.name) || in.failed_starts->count(d.name))
			continue;
		if (d.slot >= in.machine_slots) {
			plan.refused.emplace_back(i,
					"slot " + std::to_string(d.slot) + " is not on this machine (" +
							std::to_string(in.machine_slots) + " slots)");
			continue;
		}
		if (!in.data_sets->count(d.data_set)) {
			plan.refused.emplace_back(i, "data set " + d.data_set + " is not mounted here");
			continue;
		}
		if (!d.files) {
			plan.refused.emplace_back(
					i, "its files were not sent (the service saw it reported before)");
			continue;
		}
		// It starts now, or waits (busy) while its slot is still another
		// container's, its image is pulling or this pass has started its share.
		plan.busy = true;
		if (held_slots.count(d.slot))
			continue;
		if (in.pulling && in.pulling->count(d.image)) {
			held_slots.insert(d.slot);
			plan.wait_for_image.push_back(i);
			continue;
		}
		if (plan.start.size() >= in.max_starts)
			continue;
		held_slots.insert(d.slot);
		plan.start.push_back(i);
	}
	return plan;
}

} // namespace opennova::hostd
