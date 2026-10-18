#include "mission_variables.h"

#include <map>
#include <set>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_variable_names.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

// Each variable parameter of the events, in the events' order: its value and where it is.
template <class Visit> void each_variable(const MissionDocument &document, Visit &&visit) {
	size_t index = 0;
	for (const Node *row : document.rows_of(MissionKind::Event)) {
		const EventRow &event = static_cast<const EventRow &>(*row);
		const size_t event_index = index++;
		if (event.ids.lists.size() < 2) continue;
		for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i)
			for (int slot = 0; slot < 4; ++slot)
				if (mission::trigger_param_kind(event.native.triggers[i], slot) == mission::ParamKind::MissionVar)
					visit(event, event_index, false, i, slot, event.native.triggers[i].param(slot));
		for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
			for (int slot = 0; slot < 4; ++slot)
				if (mission::action_param_kind(event.native.actions[i], slot) == mission::ParamKind::MissionVar)
					visit(event, event_index, true, i, slot, event.native.actions[i].param(slot));
	}
}

} // namespace

std::vector<MissionVariable> mission_variables(const MissionDocument &document, const MissionNames &names) {
	std::map<int64_t, MissionVariable> held;
	const MissionRow *mission = document.mission_row();
	const bms::Header *header = mission ? &mission->native.header : nullptr;
	each_variable(document, [&](const EventRow &event, size_t event_index, bool action, size_t i, int slot, int64_t value) {
		MissionVariable &variable = held[value];
		variable.index = value;
		MissionVariableUse use;
		use.event = {event.id, event.kind, 0};
		use.event_index = event_index;
		use.action = action;
		use.record = {event.id, k(action ? MissionKind::Action : MissionKind::Trigger), event.ids.lists[action ? 1 : 0][i].id};
		use.slot = slot;
		use.words = action ? action_words(event.native.actions[i], names, header) : trigger_words(event.native.triggers[i], names);
		variable.uses.push_back(std::move(use));
	});
	// A variable the mission names (S24 Names) that no event uses yet: listed, so its name can be changed.
	for (const MissionVariableName &name : mission_variable_names(document)) held[name.index].index = name.index;
	std::vector<MissionVariable> out;
	for (auto &[index, variable] : held) {
		variable.name = names.variable(index);
		out.push_back(std::move(variable));
	}
	return out;
}

int64_t mission_new_variable(const MissionDocument &document, const std::vector<int64_t> &script) {
	std::set<int64_t> used(script.begin(), script.end());
	each_variable(document, [&](const EventRow &, size_t, bool, size_t, int, int64_t value) { used.insert(value); });
	for (const MissionVariableName &name : mission_variable_names(document)) used.insert(name.index);
	int64_t index = 0;
	while (used.count(index)) ++index;
	return index;
}

} // namespace opennova::editor
