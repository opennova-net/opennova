#include "reconcile.h"

#include <optional>

namespace opennova::hostd {

namespace {

// Where `d`'s trees stand, all told: present, fetching (one still is), or
// failed for good (`error` the first such one's reason); nullopt with a tree
// the store holds nothing of (`error` says which).
std::optional<TreeFetchState> trees_state(
		const PlanInput &in, const hosted::DesiredContainer &d, std::string &error) {
	std::vector<int64_t> ids = { d.base_tree };
	if (d.expansion_tree)
		ids.push_back(*d.expansion_tree);
	if (!in.trees) {
		error = "tree " + std::to_string(d.base_tree) + " is not on this machine";
		return std::nullopt;
	}
	TreeFetchState state = TreeFetchState::Present;
	for (const int64_t id : ids) {
		const auto it = in.trees->find(id);
		if (it == in.trees->end()) {
			error = "tree " + std::to_string(id) + " is not on this machine";
			return std::nullopt;
		}
		if (it->second.state == TreeFetchState::Failed) {
			error = it->second.error;
			return TreeFetchState::Failed;
		}
		if (it->second.state == TreeFetchState::Fetching)
			state = TreeFetchState::Fetching;
	}
	return state;
}

} // namespace

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
		std::string tree_error;
		const std::optional<TreeFetchState> trees = trees_state(in, d, tree_error);
		if (!trees) {
			plan.refused.emplace_back(i, tree_error);
			continue;
		}
		if (!d.files) {
			plan.refused.emplace_back(
					i, "its files were not sent (the service saw it reported before)");
			continue;
		}
		if (*trees == TreeFetchState::Failed) {
			plan.data_failed.emplace_back(i, tree_error);
			continue;
		}
		// It starts now, or waits (busy) while its slot is still another
		// container's, its trees are fetching, its image is pulling or this pass
		// has started its share.
		plan.busy = true;
		if (held_slots.count(d.slot))
			continue;
		if (*trees == TreeFetchState::Fetching) {
			held_slots.insert(d.slot);
			plan.wait_for_tree.push_back(i);
			continue;
		}
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
