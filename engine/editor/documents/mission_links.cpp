#include "mission_links.h"

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/mission_uses.h>
#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission.h>
#include <runtime/world/spawn_select.h>

namespace opennova::editor {

using mission::ParamKind;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

// The record of group `group` on the mission row's list of groups, found by its kind among the row's lists
// (none past the list).
NodeAddress group_record(const MissionDocument &document, int64_t group) {
	const MissionRow *mission = document.mission_row();
	if (!mission || group < 0) return NodeAddress();
	for (const Document::Collection &list : document.collections_of({mission->id, mission->kind, 0}))
		if (list.spec.kind == k(MissionKind::Group))
			return size_t(group) < list.ids.size() ? NodeAddress{mission->id, k(MissionKind::Group), list.ids[size_t(group)]}
			                                       : NodeAddress();
	return NodeAddress();
}

// The sub-types of a Single trigger that test a distance between two entities [orig:
// EventTrigger_EvaluateCondition @0x453620: cat 2 cases 43 @0x453842, 44 @0x45388c, 45 @0x4538d6].
constexpr int32_t kNear = 43, kInSight = 44, kSees = 45;

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
		out.record = group_record(document, value);
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
		link.value = record.param(slot);
		if (kinds[slot] == ParamKind::PathNode) {
			if (path_slot < 0) continue;
			resolve_stop(document, record.param(path_slot), link.value, link);
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
		reach.player = !reach.subject && mission::ssn_is_players(record.param1);
		reach.other = document.entity_holder(record.param2);
		reach.metres = int64_t(int16_t(uint16_t(uint32_t(record.param3) & 0xFFFFu)));
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

bool mission_record_value(const MissionDocument &document, const NodeAddress &record, ReferenceKind kind, int64_t &value,
		std::string *refusal) {
	// What the record is to a parameter (mission_uses' reading), taken where the reference names that kind.
	ParamKind is = ParamKind::Unused;
	int64_t held = 0;
	if (!mission_record_value(document, record, is, held)) return false;
	const ReferenceKind names = is == ParamKind::Entity ? ReferenceKind::MissionEntity
	                            : is == ParamKind::Zone ? ReferenceKind::MissionZone
	                            : is == ParamKind::Path ? ReferenceKind::MissionPath
	                            : is == ParamKind::Event ? ReferenceKind::MissionEvent
	                            : is == ParamKind::Group ? ReferenceKind::MissionGroup
	                                                     : ReferenceKind::None;
	if (names != kind) return false;
	// The value names the record the game's lookup finds first: another one, refused.
	const NodeId holder = is == ParamKind::Entity ? document.entity_holder(held)
	                      : is == ParamKind::Zone ? document.zone_holder(held)
	                                              : record.row;
	if (holder != record.row) {
		if (refusal) {
			const Node *first = document.row(holder);
			*refusal = std::string(is == ParamKind::Entity ? "SSN " : "Zone id ") + std::to_string(held) + " names " +
			           (first ? document.record_title({first->id, first->kind, 0}) : std::string("another record")) +
			           ", which the game finds first.";
		}
		return false;
	}
	value = held;
	return true;
}

NodeAddress mission_group_address(const MissionDocument &document, int64_t group) { return group_record(document, group); }

int64_t mission_fresh_group(const MissionDocument &document) {
	// The groups some event's trigger or action names.
	std::vector<char> named(bms::kGroupRecordCount, 0);
	for (const Node *event : document.rows_of(MissionKind::Event))
		for (const MissionLink &link : mission_links(document, {event->id, event->kind, 0}).links)
			if (link.kind == ParamKind::Group && link.value > 0 && link.value < int64_t(named.size())) named[size_t(link.value)] = 1;
	const MissionRow *mission = document.mission_row();
	for (int64_t group = 1; group < int64_t(bms::kGroupRecordCount); ++group) {
		if (document.group_members(group) || named[size_t(group)]) continue;
		if (mission && size_t(group) < mission->native.group_records.size() &&
		    !bms::group_name(mission->native.group_records[size_t(group)]).empty())
			continue;
		return group;
	}
	return -1;
}

std::vector<MissionSubject> mission_subjects(const MissionDocument &document, const NodeAddress &record) {
	std::vector<MissionSubject> out;
	MissionSubject own;
	own.record = record;
	if (!mission_record_value(document, record, own.kind, own.value)) return out;
	out.push_back(own);
	const Node *row = document.row(record.row);
	if (own.kind == ParamKind::Entity && row && is_entity_kind(row->kind)) {
		const int64_t group = static_cast<const EntityRow &>(*row).native.group_id;
		MissionSubject in_group;
		in_group.record = mission_group_address(document, group);
		if (!mission::group_names_none(group) && in_group.record.row &&
		    mission_record_value(document, in_group.record, in_group.kind, in_group.value))
			out.push_back(in_group);
	}
	return out;
}

std::vector<MissionTypeSlot> mission_types_taking(ParamKind kind, bool actions) {
	std::vector<MissionTypeSlot> out;
	for (const LogicType &type : logic_types(actions)) {
		const mission::ParamRow *row = actions ? mission::action_params(type.type, type.sub) : mission::trigger_params(type.type, type.sub);
		if (!row) continue;
		for (int slot = 0; slot < 4; ++slot)
			if (row->p[slot] == kind) {
				out.push_back({&type, slot});
				break;
			}
	}
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
		for (int i = 0; i < 4; ++i) params[i] = trigger.param(i);
		for (int i = 0; i < 4; ++i)
			if (mission::trigger_param_kind(trigger, i) == ParamKind::Path && out.path < 0) out.path = params[i];
		if (const mission::ParamRow *type = mission::trigger_params(int32_t(trigger.main_type), trigger.sub_type))
			for (int i = 0; i < 4 && out.node_slot < 0; ++i)
				if (type->p[i] == ParamKind::PathNode) out.node_slot = i;
	} else if (record.kind == k(MissionKind::Action) && at.index < event.native.actions.size()) {
		const bms::Action &action = event.native.actions[at.index];
		out.kind = mission::action_param_kind(action, slot);
		out.label = mission::action_param_label(action, slot);
		for (int i = 0; i < 4; ++i) params[i] = action.param(i);
		for (int i = 0; i < 4; ++i)
			if (mission::action_param_kind(action, i) == ParamKind::Path && out.path < 0) out.path = params[i];
		// The stop slot its type's row reads beside a path (a Redirect's third parameter names a stop once its
		// waypoint list is a path, not a command).
		if (const mission::ParamRow *type = mission::action_params(int32_t(action.action_type), action.action_sub_type))
			for (int i = 0; i < 4 && out.node_slot < 0; ++i)
				if (type->p[i] == ParamKind::PathNode) out.node_slot = i;
		// An effect plays at every numbered marker of its number; a teleport and a teammate's pickup find the first.
		out.every_marker = action.action_type == bms::ActionType::ParticleEffectAction;
	} else {
		return false;
	}
	if (out.kind != ParamKind::PathNode) out.path = -1;
	if (out.kind != ParamKind::Path) out.node_slot = -1;
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
		int64_t &value, std::string *refusal) {
	if (!slot.marks || record.child) return false;
	const Node *row = document.row(record.row);
	if (!row) return false;
	const bool entity = is_entity_kind(row->kind);
	// A value that names another record than this one: refused with the record the game finds.
	const auto names_another = [&](const std::string &what, NodeId holder) {
		if (refusal) {
			const Node *first = document.row(holder);
			*refusal = what + " names " + (first ? document.record_title({first->id, first->kind, 0}) : std::string("another record")) +
			           ", which the game finds first.";
		}
		return false;
	};
	switch (slot.kind) {
	case ParamKind::Entity: {
		if (!entity) return false;
		value = static_cast<const EntityRow &>(*row).native.id;
		const NodeId holder = document.entity_holder(value);
		if (holder != row->id) return names_another("SSN " + std::to_string(value), holder);
		return true;
	}
	case ParamKind::Zone: {
		if (row->kind != k(MissionKind::Area)) return false;
		value = static_cast<const AreaRow &>(*row).native.id;
		const NodeId holder = document.zone_holder(value);
		if (holder != row->id) return names_another("Zone id " + std::to_string(value), holder);
		return true;
	}
	case ParamKind::Group:
		if (!entity || mission::group_names_none(static_cast<const EntityRow &>(*row).native.group_id)) return false;
		value = static_cast<const EntityRow &>(*row).native.group_id;
		return true;
	case ParamKind::Path: {
		if (row->kind != k(MissionKind::Marker)) return false;
		const std::vector<MissionDocument::StopPlace> &stops = document.stops_of(*row);
		if (stops.empty()) return false;
		value = stops.front().path;
		return true;
	}
	case ParamKind::PathNode: {
		if (row->kind != k(MissionKind::Marker)) return false;
		for (const MissionDocument::StopPlace &stop : document.stops_of(*row)) {
			if (stop.path != slot.path) continue;
			// A path's record holds 32 stops: the game reads no place past them (an AtWaypoint's bit is never
			// recorded, a Redirect's node reads past the slots).
			if (stop.place >= mission::kMaxWaypointPathMarkers) {
				if (refusal)
					*refusal = "Stop " + std::to_string(stop.place) + " of path " + std::to_string(stop.path) + " is past the " +
					           std::to_string(mission::kMaxWaypointPathMarkers) + " stops a path's record holds.";
				return false;
			}
			value = int64_t(stop.place);
			return true;
		}
		return false;
	}
	case ParamKind::TeleportTarget:
	case ParamKind::WpNumber: {
		if (row->kind != k(MissionKind::Marker)) return false;
		const bms::Entity &marker = static_cast<const EntityRow &>(*row).native;
		if (marker.type_id != kNumberedMarkerType) return false;
		value = marker.wp_number;
		const NodeId holder = document.numbered_holder(value);
		if (!slot.every_marker && holder != row->id) return names_another("Marker number " + std::to_string(value), holder);
		return true;
	}
	default: return false;
	}
}

bool mission_pick_node(const MissionDocument &document, const MissionPickSlot &slot, const NodeAddress &record,
		int64_t &place) {
	if (slot.kind != ParamKind::Path || slot.node_slot < 0 || record.child) return false;
	const Node *row = document.row(record.row);
	if (!row || row->kind != k(MissionKind::Marker)) return false;
	const std::vector<MissionDocument::StopPlace> &stops = document.stops_of(*row);
	if (stops.empty() || stops.front().place >= mission::kMaxWaypointPathMarkers) return false;
	place = int64_t(stops.front().place);
	return true;
}

std::vector<NodeId> mission_reveal_rows(const MissionDocument &document, const NodeAddress &record) {
	std::vector<NodeId> out;
	std::unordered_set<NodeId> seen; // each row once, in the order met
	const auto add = [&](NodeId row) {
		if (row && seen.insert(row).second) out.push_back(row);
	};
	const Node *row = document.row(record.row);
	if (!row) return out;
	if (mission_logic_record(record)) {
		// The whole event's names for an event; a trigger's or an action's own.
		const MissionLinks links = mission_links(document, record);
		for (const MissionLink &link : links.links) {
			for (const NodeId each : link.rows) add(each);
			if (link.kind == ParamKind::Zone) add(link.record.row);
		}
		for (const MissionReach &reach : links.reaches) {
			add(reach.subject);
			add(reach.other);
		}
		return out;
	}
	if (record.child && record.kind == k(MissionKind::Group)) {
		for (const NodeId each : mission_group_rows(document, mission_group_of(document, record))) add(each);
		return out;
	}
	if (record.child && record.kind == k(MissionKind::Stop)) {
		Document::Placement at;
		if (row->kind == k(MissionKind::WaypointPath) && document.placement(record, at)) {
			const std::vector<uint32_t> &stops = static_cast<const PathRow &>(*row).native.stops;
			if (at.index < stops.size())
				if (const Node *marker = document.row_of(MissionKind::Marker, stops[at.index])) add(marker->id);
		}
		return out;
	}
	if (record.child) return out;
	if (row->kind == k(MissionKind::WaypointPath)) {
		for (const uint32_t stop : static_cast<const PathRow &>(*row).native.stops)
			if (const Node *marker = document.row_of(MissionKind::Marker, stop)) add(marker->id);
		return out;
	}
	if (is_entity_kind(row->kind) || row->kind == k(MissionKind::Area)) add(row->id);
	return out;
}

std::vector<NodeId> mission_player_starts(const MissionDocument &document) {
	std::vector<NodeId> out;
	const MissionRow *mission = document.mission_row();
	if (!mission) return out;
	const uint32_t mode = game_type::for_mission_attribs(uint32_t(mission->native.header.attrib_flags));
	const world::StartMarkerTypes starts = world::start_marker_types(mode, 1);
	const std::vector<const Node *> markers = document.rows_of(MissionKind::Marker);
	for (const int32_t type : {starts.primary, starts.fallback}) {
		if (type == 0) continue;
		for (const Node *row : markers)
			if (static_cast<const EntityRow &>(*row).native.type_id == type) out.push_back(row->id);
		if (!out.empty()) break;
	}
	return out;
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
	std::unordered_map<NodeId, std::vector<NodeId>> rows;
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
		for (const NodeId row : named) rows[row].push_back(node->id);
		for (size_t g = 0; g < grouped.size(); ++g) groups_[g] += size_t(grouped[g]);
	}
	rows_.clear();
	rows_.reserve(rows.size());
	for (auto &[row, events] : rows) rows_.emplace_back(row, std::move(events));
	std::sort(rows_.begin(), rows_.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
}

const std::vector<NodeId> &MissionLinkCounts::namers(NodeId row) const {
	static const std::vector<NodeId> kNone;
	const auto found = std::lower_bound(rows_.begin(), rows_.end(), row,
	                                    [](const auto &each, NodeId id) { return each.first < id; });
	return found != rows_.end() && found->first == row ? found->second : kNone;
}

size_t MissionLinkCounts::events(NodeId row) const { return namers(row).size(); }

size_t MissionLinkCounts::group_events(int64_t group) const {
	return group >= 0 && size_t(group) < groups_.size() ? groups_[size_t(group)] : 0;
}

} // namespace opennova::editor
