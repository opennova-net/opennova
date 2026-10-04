#include <editor/import/import_plan_groups.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include <editor/assets/asset_registry.h>

namespace opennova::editor {

std::vector<ImportPlanGroup> import_plan_groups(const ImportPlan &plan) {
	using State = ImportPlanRow::State;
	using Group = ImportPlanGroup;
	std::vector<Group> groups;
	size_t chosen = 0;
	bool found = false;
	for (const ImportPlanRow &row : plan.rows) {
		chosen += row.state == State::Selected ? 1 : 0;
		found = found || row.state == State::Found;
	}
	// A kind's group under `parent` (kNone: a top one), made at its first row.
	std::map<std::pair<size_t, AssetKind>, size_t> kinds;
	const auto kind_group = [&](size_t parent, AssetKind kind, size_t root) {
		const auto made = kinds.find({parent, kind});
		if (made != kinds.end()) return made->second;
		Group group;
		group.parent = parent;
		group.depth = parent == Group::kNone ? 0 : groups[parent].depth + 1;
		group.root = root;
		group.kind = kind;
		groups.push_back(std::move(group));
		const size_t index = groups.size() - 1;
		if (parent != Group::kNone) groups[parent].children.push_back(index);
		kinds.emplace(std::make_pair(parent, kind), index);
		return index;
	};
	if (!found && chosen > kKindGroupsPast) {
		// By kind alone, the largest first (the plan's own summary order).
		for (const ImportPlanKind &entry : plan.by_kind()) kind_group(Group::kNone, entry.kind, Group::kNone);
		for (size_t i = 0; i < plan.rows.size(); ++i)
			if (plan.rows[i].state != State::NotFound)
				groups[kind_group(Group::kNone, plan.rows[i].kind, Group::kNone)].rows.push_back(i);
	} else {
		// The rows by name (the first of each), and the group each sits in.
		std::map<std::string, size_t> by_name;
		std::vector<size_t> group_of(plan.rows.size(), Group::kNone);
		for (size_t i = 0; i < plan.rows.size(); ++i)
			if (plan.rows[i].state != State::NotFound) by_name.emplace(normalized_logical_name(plan.rows[i].name), i);
		for (size_t i = 0; i < plan.rows.size(); ++i) {
			const ImportPlanRow &row = plan.rows[i];
			if (row.state == State::NotFound) continue;
			if (row.state == State::Selected) {
				Group group;
				group.root = i;
				group.kind = row.kind;
				group.rows.push_back(i);
				groups.push_back(std::move(group));
				group_of[i] = groups.size() - 1;
				continue;
			}
			// Under the group of the file that wanted it (planned before it); a file whose wanting file the
			// plan lacks, under a top group of its kind.
			const auto wanting = by_name.find(normalized_logical_name(row.needed_by.file));
			const size_t parent = wanting == by_name.end() ? Group::kNone : group_of[wanting->second];
			const size_t group = kind_group(parent, row.kind, parent == Group::kNone ? Group::kNone : groups[parent].root);
			groups[group].rows.push_back(i);
			group_of[i] = group;
		}
		// A file other planned files name too, listed under each one's group of its kind as well (once a
		// group), after every row has its own place.
		for (size_t i = 0; i < plan.rows.size(); ++i) {
			const ImportPlanRow &row = plan.rows[i];
			if (row.state != State::Found || row.wanted_by.size() < 2) continue;
			for (size_t w = 1; w < row.wanted_by.size(); ++w) {
				const auto other = by_name.find(normalized_logical_name(row.wanted_by[w]));
				if (other == by_name.end() || group_of[other->second] == Group::kNone) continue;
				const size_t parent = group_of[other->second];
				const size_t group = kind_group(parent, row.kind, groups[parent].root);
				if (group == group_of[i]) continue;
				std::vector<size_t> &also = groups[group].also;
				if (std::find(also.begin(), also.end(), i) == also.end()) also.push_back(i);
			}
		}
	}
	// The counts, the children first (each above its parent).
	for (size_t g = groups.size(); g-- > 0;) {
		Group &group = groups[g];
		for (const size_t row : group.rows) {
			++group.files;
			group.bytes += plan.rows[row].size;
		}
		for (const size_t child : group.children) {
			group.files += groups[child].files;
			group.bytes += groups[child].bytes;
		}
	}
	return groups;
}

} // namespace opennova::editor
