#include "mission_variables.h"

#include <map>
#include <set>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

int64_t param_of(const bms::Trigger &trigger, int slot) {
	const int32_t params[4] = {trigger.param1, trigger.param2, trigger.param3, trigger.param4};
	return params[slot];
}
int64_t param_of(const bms::Action &action, int slot) {
	const int32_t params[4] = {action.param1, action.param2, action.param3, action.param4};
	return params[slot];
}

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
					visit(event, event_index, false, i, slot, param_of(event.native.triggers[i], slot));
		for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
			for (int slot = 0; slot < 4; ++slot)
				if (mission::action_param_kind(event.native.actions[i], slot) == mission::ParamKind::MissionVar)
					visit(event, event_index, true, i, slot, param_of(event.native.actions[i], slot));
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
	std::vector<MissionVariable> out;
	for (auto &[index, variable] : held) {
		variable.name = names.variable(index);
		out.push_back(std::move(variable));
	}
	return out;
}

int64_t mission_new_variable(const MissionDocument &document) {
	std::set<int64_t> used;
	each_variable(document, [&](const EventRow &, size_t, bool, size_t, int, int64_t value) { used.insert(value); });
	int64_t index = 0;
	while (used.count(index)) ++index;
	return index;
}

} // namespace opennova::editor
