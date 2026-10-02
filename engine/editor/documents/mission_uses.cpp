#include "mission_uses.h"

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

using mission::ParamKind;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

const char *const kParams[4] = {"param1", "param2", "param3", "param4"};

int64_t param_of(const bms::Trigger &trigger, int slot) {
	const int32_t params[4] = {trigger.param1, trigger.param2, trigger.param3, trigger.param4};
	return params[slot];
}
int64_t param_of(const bms::Action &action, int slot) {
	const int32_t params[4] = {action.param1, action.param2, action.param3, action.param4};
	return params[slot];
}

// The record's place among the rows of its kind.
size_t place_of(const MissionDocument &document, const Node &row) {
	size_t index = 0;
	for (const auto &other : document.rows()) {
		if (other.get() == &row) return index;
		index += other->kind == row.kind;
	}
	return index;
}

} // namespace

MissionUses mission_uses(const MissionDocument &document, const NodeAddress &record, const MissionNames &names) {
	MissionUses out;
	const Node *row = document.row(record.row);
	if (!row) return out;
	// What the record is to a parameter: its kind and the value naming it.
	ParamKind kind = ParamKind::Unused;
	int64_t value = 0;
	if (!record.child && is_entity_kind(row->kind)) {
		kind = ParamKind::Entity;
		value = static_cast<const EntityRow &>(*row).native.id;
		out.what = names.entity(value);
		if (document.entity_holder(value) != row->id)
			out.inert = row->kind == k(MissionKind::Organic) || row->kind == k(MissionKind::Item)
			                    ? "Another entity has this SSN: the game's lookups by SSN find the first in pool order "
			                      "(organics, items, buildings, markers), though an area check tests this one too."
			                    : "Another entity has this SSN: the game's lookups by SSN find the first in pool order "
			                      "(organics, items, buildings, markers), never this one.";
	} else if (!record.child && row->kind == k(MissionKind::Area)) {
		kind = ParamKind::Zone;
		value = static_cast<const AreaRow &>(*row).native.id;
		out.what = names.zone(value);
		if (document.zone_holder(value) != row->id)
			out.inert = "Another area trigger has this zone id: which of the two the game's resolver finds is not read yet.";
	} else if (!record.child && row->kind == k(MissionKind::WaypointPath)) {
		kind = ParamKind::Path;
		value = static_cast<const PathRow &>(*row).native.number;
		out.what = names.path(value);
	} else if (!record.child && row->kind == k(MissionKind::Event)) {
		kind = ParamKind::Event;
		value = int64_t(place_of(document, *row));
		out.what = names.event(value);
	} else if (record.child && record.kind == k(MissionKind::Group)) {
		Document::Placement at;
		if (!document.placement(record, at)) return out;
		kind = ParamKind::Group;
		value = int64_t(at.index);
		out.what = names.group(value);
		// Group 0 is none: what names it names no group [bms-event-runtime-re.md 3a: group 0 forced to
		// count 0].
		if (value == 0) return out;
	} else {
		return out;
	}
	const MissionRow *mission = document.mission_row();
	const bms::Header *header = mission ? &mission->native.header : nullptr;
	size_t event_index = 0;
	for (const auto &node : document.rows()) {
		if (!node || node->kind != k(MissionKind::Event)) continue;
		const EventRow &event = static_cast<const EventRow &>(*node);
		const size_t index = event_index++;
		if (event.ids.lists.size() < 2) continue;
		std::string sentence;
		const auto use = [&](bool action, size_t i, int slot) {
			if (sentence.empty()) sentence = event_sentence(event.native, names, header);
			MissionUse made;
			made.event = {event.id, event.kind, 0};
			made.event_index = index;
			made.action = action;
			made.record = {event.id, k(action ? MissionKind::Action : MissionKind::Trigger), event.ids.lists[action ? 1 : 0][i].id};
			made.slot = slot;
			made.field = kParams[slot];
			made.words = action ? action_words(event.native.actions[i], names, header)
			                    : trigger_words(event.native.triggers[i], names);
			made.sentence = sentence;
			out.uses.push_back(std::move(made));
		};
		for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i)
			for (int slot = 0; slot < 4; ++slot)
				if (mission::trigger_param_kind(event.native.triggers[i], slot) == kind &&
				    param_of(event.native.triggers[i], slot) == value)
					use(false, i, slot);
		for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
			for (int slot = 0; slot < 4; ++slot)
				if (mission::action_param_kind(event.native.actions[i], slot) == kind &&
				    param_of(event.native.actions[i], slot) == value)
					use(true, i, slot);
	}
	return out;
}

} // namespace opennova::editor
