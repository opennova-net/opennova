#include "mission_links.h"

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

using mission::ParamKind;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

// The record type of the markers a teleport, an effect and a teammate's pickup find by their wp_number
// [orig: EventAction_SpawnParticleEffect @0x4540e0, `result[20] == 6088`].
constexpr int32_t kNumberedMarkerType = 6088;

// The groups' list on the mission row: the loadout, the availability rules, then the groups
// (mission_table's lists, in their order).
constexpr size_t kGroupsList = 2;

// The sub-types of a Single trigger that test a distance between two entities [orig:
// EventTrigger_EvaluateCondition @0x453620: cat 2 cases 43 @0x453842, 44 @0x45388c, 45 @0x4538d6].
constexpr int32_t kNear = 43, kInSight = 44, kSees = 45;

int32_t param_of(const bms::Trigger &trigger, int slot) {
	const int32_t params[4] = {trigger.param1, trigger.param2, trigger.param3, trigger.param4};
	return params[slot];
}
int32_t param_of(const bms::Action &action, int slot) {
	const int32_t params[4] = {action.param1, action.param2, action.param3, action.param4};
	return params[slot];
}

// The pool-3 markers of record type 6088 whose wp_number is `number`, in their order; the first alone
// where `first`.
std::vector<NodeId> numbered_markers(const MissionDocument &document, int64_t number, bool first) {
	std::vector<NodeId> out;
	for (const Node *row : document.rows_of(MissionKind::Marker)) {
		const bms::Entity &marker = static_cast<const EntityRow &>(*row).native;
		if (marker.type_id != kNumberedMarkerType || marker.wp_number != number) continue;
		out.push_back(row->id);
		if (first) break;
	}
	return out;
}

// What a value of a kind names: the record and the rows it reaches. False for a kind that names no
// record of the mission (a number, a dialog, a variable).
bool resolve(const MissionDocument &document, ParamKind kind, int64_t value, bool every_marker, MissionLink &out) {
	switch (kind) {
	case ParamKind::Entity:
		if (const Node *row = document.row(document.entity_holder(value))) {
			out.record = {row->id, row->kind, 0};
			out.rows.push_back(row->id);
		}
		return true;
	case ParamKind::Zone:
		if (const Node *row = document.row(document.zone_holder(value))) {
			out.record = {row->id, row->kind, 0};
			out.rows.push_back(row->id);
		}
		return true;
	case ParamKind::Path: {
		if (mission::path_names_none(value) || value < 0) return true;
		const Node *row = document.row_of(MissionKind::WaypointPath, size_t(value));
		if (!row) return true;
		out.record = {row->id, row->kind, 0};
		for (const uint32_t stop : static_cast<const PathRow &>(*row).native.stops)
			if (const Node *marker = document.row_of(MissionKind::Marker, stop)) out.rows.push_back(marker->id);
		return true;
	}
	case ParamKind::Group: {
		if (mission::group_names_none(value) || value < 0 || value >= int64_t(bms::kGroupRecordCount)) return true;
		const MissionRow *mission = document.mission_row();
		if (mission && mission->ids.lists.size() > kGroupsList && size_t(value) < mission->ids.lists[kGroupsList].size())
			out.record = {mission->id, k(MissionKind::Group), mission->ids.lists[kGroupsList][size_t(value)].id};
		out.rows = mission_group_rows(document, value);
		return true;
	}
	case ParamKind::Event:
		if (value >= 0)
			if (const Node *row = document.row_of(MissionKind::Event, size_t(value))) out.record = {row->id, row->kind, 0};
		return true;
	case ParamKind::TeleportTarget:
	case ParamKind::WpNumber:
		out.rows = numbered_markers(document, value, !every_marker);
		if (!out.rows.empty())
			if (const Node *row = document.row(out.rows.front())) out.record = {row->id, row->kind, 0};
		return true;
	default: return false;
	}
}

// A stop of the path another slot of the record names, by its number from 0 (-1 the nearest: no one
// stop) [docs/world/world-wac-ai-re.md: only node -1 requests the nearest, Entity_SetWaypointByTeam
// @0x43CD20].
void resolve_stop(const MissionDocument &document, int64_t path, int64_t stop, MissionLink &out) {
	if (stop < 0 || mission::path_names_none(path) || path < 0) return;
	const Node *row = document.row_of(MissionKind::WaypointPath, size_t(path));
	if (!row) return;
	const std::vector<uint32_t> &stops = static_cast<const PathRow &>(*row).native.stops;
	if (size_t(stop) >= stops.size()) return;
	if (const Node *marker = document.row_of(MissionKind::Marker, stops[size_t(stop)])) {
		out.record = {marker->id, marker->kind, 0};
		out.rows.push_back(marker->id);
	}
}

// What one trigger's or action's parameters name, appended to `out`; a distance trigger's reach.
template <class Record>
void link_record(const MissionDocument &document, const NodeAddress &from, const Record &record, MissionLinks &out) {
	constexpr bool action = std::is_same<Record, bms::Action>::value;
	ParamKind kinds[4];
	for (int slot = 0; slot < 4; ++slot) {
		if constexpr (action) kinds[slot] = mission::action_param_kind(record, slot);
		else kinds[slot] = mission::trigger_param_kind(record, slot);
	}
	int path_slot = -1;
	for (int slot = 0; slot < 4 && path_slot < 0; ++slot)
		if (kinds[slot] == ParamKind::Path) path_slot = slot;
	// An effect plays at every numbered marker of its number; a teleport and a teammate's pickup find the
	// first.
	bool every_marker = false;
	if constexpr (action) every_marker = record.action_type == bms::ActionType::ParticleEffectAction;
	for (int slot = 0; slot < 4; ++slot) {
		MissionLink link;
		link.from = from;
		link.action = action;
		link.slot = slot;
		link.kind = kinds[slot];
		link.value = param_of(record, slot);
		if (kinds[slot] == ParamKind::PathNode) {
			if (path_slot < 0) continue;
			resolve_stop(document, param_of(record, path_slot), link.value, link);
		} else if (!resolve(document, kinds[slot], link.value, every_marker, link)) {
			continue;
		}
		out.links.push_back(std::move(link));
	}
	if constexpr (!action) {
		if (record.main_type != bms::TriggerMainType::Single ||
		    (record.sub_type != kNear && record.sub_type != kInSight && record.sub_type != kSees))
			return;
		MissionReach reach;
		reach.from = from;
		reach.subject = document.entity_holder(record.param1);
		reach.other = document.entity_holder(record.param2);
		reach.metres = record.param3;
		reach.sight = record.sub_type != kNear;
		reach.facing = reach.centres = record.sub_type == kSees;
		out.reaches.push_back(reach);
	}
}

// The event row a logic record belongs to (the row itself for an event); null for any other record.
const EventRow *event_of(const MissionDocument &document, const NodeAddress &record) {
	if (!mission_logic_record(record)) return nullptr;
	const Node *row = document.row(record.row);
	return row && row->kind == k(MissionKind::Event) ? static_cast<const EventRow *>(row) : nullptr;
}

// Each trigger's and action's record with its address, in their order.
template <class Fn> void for_each_record(const EventRow &event, Fn &&fn) {
	if (event.ids.lists.size() < 2) return;
	for (size_t i = 0; i < event.native.triggers.size() && i < event.ids.lists[0].size(); ++i)
		fn(NodeAddress{event.id, k(MissionKind::Trigger), event.ids.lists[0][i].id}, &event.native.triggers[i], nullptr);
	for (size_t i = 0; i < event.native.actions.size() && i < event.ids.lists[1].size(); ++i)
		fn(NodeAddress{event.id, k(MissionKind::Action), event.ids.lists[1][i].id}, nullptr, &event.native.actions[i]);
}

} // namespace

std::vector<NodeId> mission_group_rows(const MissionDocument &document, int64_t group) {
	std::vector<NodeId> out;
	if (mission::group_names_none(group) || group < 0 || document.group_members(group) == 0) return out;
	for (const auto &row : document.rows())
		if (row && is_entity_kind(row->kind) && static_cast<const EntityRow &>(*row).native.group_id == group)
			out.push_back(row->id);
	return out;
}

int64_t mission_group_of(const MissionDocument &document, const NodeAddress &record) {
	const Node *row = document.row(record.row);
	if (!row) return -1;
	if (!record.child && is_entity_kind(row->kind)) return static_cast<const EntityRow &>(*row).native.group_id;
	if (record.child && record.kind == k(MissionKind::Group)) {
		Document::Placement at;
		if (document.placement(record, at)) return int64_t(at.index);
	}
	return -1;
}

std::vector<NodeAddress> mission_group_members(const MissionDocument &document, const std::vector<NodeAddress> &records) {
	std::vector<char> groups(256, 0);
	bool any = false;
	for (const NodeAddress &record : records) {
		const int64_t group = mission_group_of(document, record);
		if (group > 0 && group < int64_t(groups.size())) groups[size_t(group)] = any = true;
	}
	std::vector<NodeAddress> out;
	if (!any) return out;
	for (const auto &row : document.rows())
		if (row && is_entity_kind(row->kind) && groups[static_cast<const EntityRow &>(*row).native.group_id])
			out.push_back(NodeAddress{row->id, row->kind, 0});
	return out;
}

const char *mission_param_field(int slot) {
	static const char *const kFields[4] = {"param1", "param2", "param3", "param4"};
	return slot >= 0 && slot < 4 ? kFields[slot] : "";
}

bool mission_pick_slot(const MissionDocument &document, const NodeAddress &record, int slot, MissionPickSlot &out) {
	out = MissionPickSlot();
	if (slot < 0 || slot > 3 || !record.child) return false;
	const Node *row = document.row(record.row);
	Document::Placement at;
	if (!row || row->kind != k(MissionKind::Event) || !document.placement(record, at)) return false;
	const EventRow &event = static_cast<const EventRow &>(*row);
	int64_t params[4] = {0, 0, 0, 0};
	if (record.kind == k(MissionKind::Trigger) && at.index < event.native.triggers.size()) {
		const bms::Trigger &trigger = event.native.triggers[at.index];
		out.kind = mission::trigger_param_kind(trigger, slot);
		out.label = mission::trigger_param_label(trigger, slot);
		for (int i = 0; i < 4; ++i) params[i] = param_of(trigger, i);
		for (int i = 0; i < 4; ++i)
			if (mission::trigger_param_kind(trigger, i) == ParamKind::Path && out.path < 0) out.path = params[i];
	} else if (record.kind == k(MissionKind::Action) && at.index < event.native.actions.size()) {
		const bms::Action &action = event.native.actions[at.index];
		out.kind = mission::action_param_kind(action, slot);
		out.label = mission::action_param_label(action, slot);
		for (int i = 0; i < 4; ++i) params[i] = param_of(action, i);
		for (int i = 0; i < 4; ++i)
			if (mission::action_param_kind(action, i) == ParamKind::Path && out.path < 0) out.path = params[i];
	} else {
		return false;
	}
	if (out.kind != ParamKind::PathNode) out.path = -1;
	out.marks = true;
	switch (out.kind) {
	case ParamKind::Entity: out.what = "an entity"; break;
	case ParamKind::Zone: out.what = "an area"; break;
	case ParamKind::Group: out.what = "an entity in a group"; break;
	case ParamKind::Path: out.what = "a stop of a path"; break;
	case ParamKind::PathNode:
		out.what = mission::path_names_none(out.path) ? std::string("a stop of its path (it names none yet)")
		                                               : "a stop of path " + std::to_string(out.path);
		out.marks = !mission::path_names_none(out.path);
		break;
	case ParamKind::TeleportTarget:
	case ParamKind::WpNumber: out.what = "a numbered marker (record type 6088)"; break;
	default:
		out.what = "nothing in the view";
		out.marks = false;
		break;
	}
	return true;
}

bool mission_pick_value(const MissionDocument &document, const MissionPickSlot &slot, const NodeAddress &record,
		int64_t &value) {
	if (!slot.marks || record.child) return false;
	const Node *row = document.row(record.row);
	if (!row) return false;
	const bool entity = is_entity_kind(row->kind);
	switch (slot.kind) {
	case ParamKind::Entity:
		if (!entity) return false;
		value = static_cast<const EntityRow &>(*row).native.id;
		return true;
	case ParamKind::Zone:
		if (row->kind != k(MissionKind::Area)) return false;
		value = static_cast<const AreaRow &>(*row).native.id;
		return true;
	case ParamKind::Group:
		if (!entity || mission::group_names_none(static_cast<const EntityRow &>(*row).native.group_id)) return false;
		value = static_cast<const EntityRow &>(*row).native.group_id;
		return true;
	case ParamKind::Path:
	case ParamKind::PathNode: {
		if (row->kind != k(MissionKind::Marker)) return false;
		const size_t marker = document.index_of(*row);
		for (const Node *path : document.rows_of(MissionKind::WaypointPath)) {
			const PathRow &each = static_cast<const PathRow &>(*path);
			if (slot.kind == ParamKind::PathNode && each.native.number != slot.path) continue;
			for (size_t stop = 0; stop < each.native.stops.size(); ++stop)
				if (each.native.stops[stop] == marker) {
					value = slot.kind == ParamKind::Path ? int64_t(each.native.number) : int64_t(stop);
					return true;
				}
		}
		return false;
	}
	case ParamKind::TeleportTarget:
	case ParamKind::WpNumber: {
		if (row->kind != k(MissionKind::Marker)) return false;
		const bms::Entity &marker = static_cast<const EntityRow &>(*row).native;
		if (marker.type_id != kNumberedMarkerType) return false;
		value = marker.wp_number;
		return true;
	}
	default: return false;
	}
}

bool mission_logic_record(const NodeAddress &record) {
	if (!record.row) return false;
	if (!record.child) return record.kind == k(MissionKind::Event);
	return record.kind == k(MissionKind::Trigger) || record.kind == k(MissionKind::Action);
}

MissionLinks mission_links(const MissionDocument &document, const NodeAddress &record) {
	MissionLinks out;
	const EventRow *event = event_of(document, record);
	if (!event) return out;
	out.event = {event->id, event->kind, 0};
	for_each_record(*event, [&](const NodeAddress &at, const bms::Trigger *trigger, const bms::Action *action) {
		if (record.child && at != record) return;
		if (trigger) link_record(document, at, *trigger, out);
		else link_record(document, at, *action, out);
	});
	return out;
}

void MissionLinkCounts::follow(const MissionDocument &document) {
	if (held_ && identity_ == document.identity() && load_ == document.load_generation() &&
	    revision_ == document.revision())
		return;
	held_ = true;
	identity_ = document.identity();
	load_ = document.load_generation();
	revision_ = document.revision();
	++made_;
	std::unordered_map<NodeId, size_t> rows;
	groups_.assign(bms::kGroupRecordCount, 0);
	for (const Node *node : document.rows_of(MissionKind::Event)) {
		MissionLinks links = mission_links(document, NodeAddress{node->id, node->kind, 0});
		// Each record once an event, however many of its parameters name it.
		std::unordered_set<NodeId> named;
		std::vector<char> grouped(bms::kGroupRecordCount, 0);
		for (const MissionLink &link : links.links) {
			if (link.kind == ParamKind::Group) {
				if (link.record.child && link.value > 0 && link.value < int64_t(grouped.size())) grouped[size_t(link.value)] = 1;
				continue;
			}
			if (link.record.row && !link.record.child) named.insert(link.record.row);
			// Every marker a marker number names (an effect plays at each).
			if (link.kind == ParamKind::WpNumber || link.kind == ParamKind::TeleportTarget)
				named.insert(link.rows.begin(), link.rows.end());
		}
		for (const NodeId row : named) ++rows[row];
		for (size_t g = 0; g < grouped.size(); ++g) groups_[g] += size_t(grouped[g]);
	}
	rows_.assign(rows.begin(), rows.end());
	std::sort(rows_.begin(), rows_.end());
}

size_t MissionLinkCounts::events(NodeId row) const {
	const auto found = std::lower_bound(rows_.begin(), rows_.end(), std::make_pair(row, size_t(0)));
	return found != rows_.end() && found->first == row ? found->second : 0;
}

size_t MissionLinkCounts::group_events(int64_t group) const {
	return group >= 0 && size_t(group) < groups_.size() ? groups_[size_t(group)] : 0;
}

} // namespace opennova::editor
