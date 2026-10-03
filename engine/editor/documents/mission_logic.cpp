#include "mission_logic.h"

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <formats/mission/mission_chains.h>

namespace opennova::editor {

using mission::ParamKind;

namespace {

using K = ParamKind;
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

// A parameter's value named as the sentence names it, through a record holding only it: the
// sentence's own words for the kind.
std::string named(ParamKind kind, int64_t value, const MissionNames &names) {
	switch (kind) {
	case K::Group: return names.group(value);
	case K::Entity: return names.entity(value);
	case K::Zone: return names.zone(value);
	case K::Event: return names.event(value);
	case K::Path: return names.path(value);
	case K::PathNode: return value == -1 ? "the nearest waypoint" : "waypoint " + std::to_string(value);
	case K::Bool: return value ? "on" : "off";
	case K::Team: {
		const mission::MissionChoices choices = mission::param_choices(kind);
		const char *name = mission::mission_choice_name(choices.rows, choices.count, value);
		return name ? std::string(name) + " (team " + std::to_string(value) + ")" : "team " + std::to_string(value);
	}
	default: break;
	}
	const char *unit = logic_param_unit(kind);
	return std::to_string(value) + (*unit ? std::string(" ") + unit : std::string());
}

// The event row holding a trigger or an action, and its place in its list.
struct Located {
	const EventRow *event = nullptr;
	bool action = false;
	size_t index = 0;
};
bool locate(const MissionDocument &document, const NodeAddress &record, Located &out) {
	if (record.kind != k(MissionKind::Trigger) && record.kind != k(MissionKind::Action)) return false;
	const Node *row = document.row(record.row);
	if (!row || row->kind != k(MissionKind::Event)) return false;
	const EventRow &event = static_cast<const EventRow &>(*row);
	if (event.ids.lists.size() < 2) return false;
	out.event = &event;
	out.action = record.kind == k(MissionKind::Action);
	const std::vector<RecordIds> &ids = event.ids.lists[out.action ? 1 : 0];
	const size_t size = out.action ? event.native.actions.size() : event.native.triggers.size();
	for (size_t i = 0; i < ids.size() && i < size; ++i)
		if (ids[i].id == record.child) {
			out.index = i;
			return true;
		}
	return false;
}

const EventRow *event_row(const MissionDocument &document, NodeId event) {
	const Node *row = document.row(event);
	return row && row->kind == k(MissionKind::Event) ? static_cast<const EventRow *>(row) : nullptr;
}

const bms::Header *header_of(const MissionDocument &document) {
	const MissionRow *mission = document.mission_row();
	return mission ? &mission->native.header : nullptr;
}

ParamKind kind_of(bool action, int32_t type, int32_t sub, int slot) {
	if (action) {
		bms::Action record{};
		record.action_type = static_cast<bms::ActionType>(type);
		record.action_sub_type = sub;
		return mission::action_param_kind(record, slot);
	}
	bms::Trigger record{};
	record.main_type = static_cast<bms::TriggerMainType>(type);
	record.sub_type = sub;
	return mission::trigger_param_kind(record, slot);
}

Edit set_of(const NodeAddress &address, const char *field, int64_t value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = value;
	return edit;
}

} // namespace

int64_t logic_param_default(ParamKind kind) {
	// The value the install's 115 missions hold most often in a parameter of the kind (measured by the
	// mission_logic ctest's retail leg, which pins each): units 1 (179 of 290), metres 200 (27 of 202),
	// seconds 10 (8 of 17), km/h 20 (87 of 626), off 0 (781 of 1,047), team 2 (16 of 23), sub-goal 1
	// (131 of 548). No shipped record holds hit points: 0. Any other kind starts at 0, none named.
	switch (kind) {
	case K::Count: return 1;
	case K::DistanceM: return 200;
	case K::Seconds: return 10;
	case K::SpeedKph: return 20;
	case K::Team: return 2;
	case K::SubGoal: return 1;
	default: break;
	}
	return 0;
}

const char *logic_param_unit(ParamKind kind) {
	switch (kind) {
	case K::DistanceM: return "m";
	case K::Seconds: return "s";
	case K::SpeedKph: return "km/h";
	case K::Hp: return "hit points";
	case K::Count: return "units";
	default: break;
	}
	return "";
}

bool logic_param_picks(ParamKind kind) {
	return kind == K::Group || kind == K::Entity || kind == K::Zone || kind == K::Event || kind == K::Path;
}

bool logic_form(const MissionDocument &document, const NodeAddress &record, const MissionNames &names, LogicForm &out) {
	Located at;
	if (!locate(document, record, at)) return false;
	out = LogicForm();
	out.action = at.action;
	out.record = record;
	out.event = {at.event->id, at.event->kind, 0};
	out.index = at.index;
	const bms::Header *header = header_of(document);
	const auto param = [&](int slot, ParamKind kind, const char *label, int64_t value) {
		LogicParam p;
		p.slot = slot;
		p.field = kParams[slot];
		p.kind = kind;
		p.label = label && *label ? label : mission::param_kind_label(kind);
		if (p.label.empty()) p.label = "Parameter " + std::to_string(slot + 1);
		p.value = value;
		p.words = named(kind, value, names);
		p.unit = logic_param_unit(kind);
		p.picks = logic_param_picks(kind);
		return p;
	};
	if (at.action) {
		const std::vector<bms::Action> &list = at.event->native.actions;
		const bms::Action &action = list[at.index];
		out.count = list.size();
		out.last = at.index + 1 == list.size();
		out.type = logic_type(true, int32_t(action.action_type), action.action_sub_type);
		for (int slot = 0; slot < 4; ++slot) {
			const ParamKind kind = mission::action_param_kind(action, slot);
			const int64_t value = param_of(action, slot);
			if (kind == K::Unused) {
				if (value) out.unread.push_back(param(slot, K::Raw, "", value));
				continue;
			}
			out.params.push_back(param(slot, kind, mission::action_param_label(action, slot), value));
		}
		out.words = action_words(action, names, header);
	} else {
		const std::vector<bms::Trigger> &list = at.event->native.triggers;
		const bms::Trigger &trigger = list[at.index];
		out.count = list.size();
		out.last = at.index + 1 == list.size();
		out.type = logic_type(false, int32_t(trigger.main_type), trigger.sub_type);
		out.negated = trigger.is_negated();
		out.join = trigger_join(trigger);
		for (int slot = 0; slot < 4; ++slot) {
			const ParamKind kind = mission::trigger_param_kind(trigger, slot);
			const int64_t value = param_of(trigger, slot);
			if (kind == K::Unused) {
				if (value) out.unread.push_back(param(slot, K::Raw, "", value));
				continue;
			}
			out.params.push_back(param(slot, kind, mission::trigger_param_label(trigger, slot), value));
		}
		out.words = trigger_words(trigger, names);
	}
	if (out.type) {
		out.type_words = out.type->title;
	} else if (at.action) {
		const bms::Action &action = at.event->native.actions[at.index];
		// A type the dispatcher has a case for whose sub-type selects nothing (an AI change 0, a variable
		// change past 5, a special or a teammate call of no arm) does nothing [sections 1.5, 7.5].
		if (const char *title = logic_action_title(int32_t(action.action_type)))
			out.type_words = std::string(title) + ": no change (sub-type " + std::to_string(action.action_sub_type) +
			                 " does nothing)";
		else
			out.type_words = "Unknown action type " + std::to_string(int32_t(action.action_type)) +
			                 (action.action_sub_type ? " (" + std::to_string(action.action_sub_type) + ")" : std::string());
	} else {
		const bms::Trigger &trigger = at.event->native.triggers[at.index];
		out.type_words = "Unknown trigger type " + std::to_string(int32_t(trigger.main_type)) + " (" +
		                 std::to_string(trigger.sub_type) + ")";
	}
	out.sentence = event_sentence(at.event->native, names, header);
	return true;
}

bool logic_event_form(const MissionDocument &document, NodeId event, const MissionNames &names, LogicEventForm &out) {
	const EventRow *row = event_row(document, event);
	if (!row) return false;
	out = LogicEventForm();
	out.event = {row->id, row->kind, 0};
	for (const auto &other : document.rows()) {
		if (other.get() == row) break;
		out.index += other->kind == k(MissionKind::Event);
	}
	out.words = event_words(row->native, names, header_of(document));
	const uint32_t flags = uint32_t(row->native.event.flags);
	out.repeats = (flags & uint32_t(bms::EventFlags::ResetAfter)) != 0;
	out.at_start = (flags & uint32_t(bms::EventFlags::PreMission)) != 0;
	out.at_end = (flags & uint32_t(bms::EventFlags::PostMission)) != 0;
	out.delay = row->native.event.delay;
	out.repeat = row->native.event.reset_after;
	out.triggers = row->native.triggers.size();
	out.actions = row->native.actions.size();
	out.most = kMaxEventRecords;
	out.trigger_refusal = logic_add_refusal(document, event, false);
	out.action_refusal = logic_add_refusal(document, event, true);
	return true;
}

std::string logic_add_refusal(const MissionDocument &document, NodeId event, bool actions) {
	const EventRow *row = event_row(document, event);
	if (!row) return "That is no event of the mission.";
	const size_t held = actions ? row->native.actions.size() : row->native.triggers.size();
	if (held < kMaxEventRecords) return std::string();
	const char *what = actions ? "actions" : "triggers";
	return "This event holds " + std::to_string(held) + " " + what + ", the most an event holds (" +
	       std::to_string(kMaxEventRecords) + "; the shipped missions hold 16 triggers and 20 actions at most). Add it to "
	       "another event, or remove one first.";
}

bool logic_add_edits(const MissionDocument &document, NodeId event, const LogicType &type, size_t position,
                     std::vector<Edit> &out, std::string &error) {
	error = logic_add_refusal(document, event, type.action);
	if (!error.empty()) return false;
	const NodeKind kind = k(type.action ? MissionKind::Action : MissionKind::Trigger);
	out.clear();
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {event, kind, 0};
	add.position = position;
	add.field = type.action ? "action_type" : "main_type";
	add.value = int64_t(type.type);
	out.push_back(add);
	const NodeAddress made{event, kind, batch_made(0)};
	if (type.sub) out.push_back(set_of(made, type.action ? "action_sub_type" : "sub_type", type.sub));
	for (int slot = 0; slot < 4; ++slot) {
		const ParamKind param = kind_of(type.action, type.type, type.sub, slot);
		if (param == K::Unused) continue;
		const int64_t value = logic_param_default(param);
		if (value) out.push_back(set_of(made, kParams[slot], value));
	}
	return true;
}

bool logic_retype_edits(const MissionDocument &document, const NodeAddress &record, const LogicType &type,
                        std::vector<Edit> &out, std::string &error) {
	Located at;
	if (!locate(document, record, at) || at.action != type.action) {
		error = type.action ? "That is no action of the mission." : "That is no trigger of the mission.";
		return false;
	}
	out.clear();
	int32_t was_type = 0, was_sub = 0;
	int64_t values[4] = {};
	if (at.action) {
		const bms::Action &action = at.event->native.actions[at.index];
		was_type = int32_t(action.action_type);
		was_sub = action.action_sub_type;
		for (int slot = 0; slot < 4; ++slot) values[slot] = param_of(action, slot);
	} else {
		const bms::Trigger &trigger = at.event->native.triggers[at.index];
		was_type = int32_t(trigger.main_type);
		was_sub = trigger.sub_type;
		for (int slot = 0; slot < 4; ++slot) values[slot] = param_of(trigger, slot);
	}
	if (was_type == type.type && was_sub == type.sub) return true;
	if (was_type != type.type) out.push_back(set_of(record, type.action ? "action_type" : "main_type", type.type));
	if (was_sub != type.sub) out.push_back(set_of(record, type.action ? "action_sub_type" : "sub_type", type.sub));
	for (int slot = 0; slot < 4; ++slot) {
		const ParamKind before = kind_of(type.action, was_type, was_sub, slot);
		const ParamKind after = kind_of(type.action, type.type, type.sub, slot);
		// What still applies stays: the same kind in the same slot.
		if (after != K::Unused && after == before) continue;
		const int64_t value = after == K::Unused ? 0 : logic_param_default(after);
		if (values[slot] != value) out.push_back(set_of(record, kParams[slot], value));
	}
	return true;
}

bool logic_move_edits(const MissionDocument &document, const NodeAddress &record, NodeId to_event, size_t position,
                      std::vector<Edit> &out, std::string &error) {
	Located at;
	if (!locate(document, record, at)) {
		error = "That is no trigger or action of the mission.";
		return false;
	}
	if (!event_row(document, to_event)) {
		error = "That is no event of the mission.";
		return false;
	}
	out.clear();
	if (to_event == record.row) {
		Edit move;
		move.operation = EditOperation::Move;
		move.address = record;
		move.position = position == SIZE_MAX ? (at.action ? at.event->native.actions.size() : at.event->native.triggers.size()) - 1
		                                     : position;
		if (move.position == at.index) return true;
		out.push_back(move);
		return true;
	}
	error = logic_add_refusal(document, to_event, at.action);
	if (!error.empty()) return false;
	// Added there with every field it holds, then removed here: one batch, one undo step.
	const NodeKind kind = record.kind;
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {to_event, kind, 0};
	add.position = position;
	const NodeAddress made{to_event, kind, batch_made(0)};
	if (at.action) {
		const bms::Action &action = at.event->native.actions[at.index];
		add.field = "action_type";
		add.value = int64_t(action.action_type);
		out.push_back(add);
		if (action.action_sub_type) out.push_back(set_of(made, "action_sub_type", action.action_sub_type));
		for (int slot = 0; slot < 4; ++slot)
			if (param_of(action, slot)) out.push_back(set_of(made, kParams[slot], param_of(action, slot)));
	} else {
		const bms::Trigger &trigger = at.event->native.triggers[at.index];
		add.field = "main_type";
		add.value = int64_t(trigger.main_type);
		out.push_back(add);
		if (trigger.condition_flags) out.push_back(set_of(made, "condition_flags", trigger.condition_flags));
		if (trigger.sub_type) out.push_back(set_of(made, "sub_type", trigger.sub_type));
		for (int slot = 0; slot < 4; ++slot)
			if (param_of(trigger, slot)) out.push_back(set_of(made, kParams[slot], param_of(trigger, slot)));
	}
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = record;
	out.push_back(remove);
	return true;
}

bool logic_negate_edit(const MissionDocument &document, const NodeAddress &trigger, bool negated, Edit &out) {
	Located at;
	if (!locate(document, trigger, at) || at.action) return false;
	const int32_t flags = at.event->native.triggers[at.index].condition_flags;
	out = set_of(trigger, "condition_flags",
	             negated ? (flags | bms::Trigger::kConditionNegated) : (flags & ~bms::Trigger::kConditionNegated));
	return true;
}

bool logic_join_edit(const MissionDocument &document, const NodeAddress &trigger, LogicJoin join, Edit &out) {
	Located at;
	if (!locate(document, trigger, at) || at.action) return false;
	int32_t flags = at.event->native.triggers[at.index].condition_flags & ~(bms::Trigger::kConditionOr | bms::Trigger::kConditionXor);
	if (join == LogicJoin::Or) flags |= bms::Trigger::kConditionOr;
	if (join == LogicJoin::Xor) flags |= bms::Trigger::kConditionXor;
	out = set_of(trigger, "condition_flags", flags);
	return true;
}

} // namespace opennova::editor
