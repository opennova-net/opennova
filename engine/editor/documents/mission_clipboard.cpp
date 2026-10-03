// The mission's clipboard (mission_document.h, ADR 0046 S14): records copied as a mission fragment
// written by bms::write and read back by bms::parse before it is handed out (the menu's rule), and
// pasted as rows of the file told apart from the rows there (an SSN or a zone id a row carries given a
// fresh one, every parameter of the pasted records that named the old value following it; an event
// index naming another pasted event following it), or as records of one kind into one owner.
#include "mission_document.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <base/io/strutil.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

using namespace mission;

namespace {

using K = MissionKind;
constexpr NodeKind k(K kind) { return node_kind(kind); }

constexpr const char *kClipHeader = "opennova-mission-clip";

// The payload's lines: the header, what it holds ("rows", or a nested kind's token), for rows the
// original indexes of the copied events ("events=3,5"), then the fragment's bytes in hex.
struct Clip {
	std::string holds;
	std::vector<size_t> events;
	bms::File fragment;
};

std::string hex_of(const std::vector<uint8_t> &bytes) {
	static const char digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(bytes.size() * 2);
	for (const uint8_t byte : bytes) {
		out += digits[byte >> 4];
		out += digits[byte & 15];
	}
	return out;
}

bool bytes_of_hex(const std::string &text, size_t from, std::vector<uint8_t> &out) {
	const auto digit = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	if (from > text.size() || (text.size() - from) % 2) return false;
	out.reserve((text.size() - from) / 2);
	for (size_t i = from; i + 1 < text.size(); i += 2) {
		const int hi = digit(text[i]), lo = digit(text[i + 1]);
		if (hi < 0 || lo < 0) return false;
		out.push_back(static_cast<uint8_t>(hi * 16 + lo));
	}
	return true;
}

bool read_clip(const std::string &payload, Clip &clip) {
	size_t at = 0;
	const auto line = [&](std::string &out) {
		const size_t end = payload.find('\n', at);
		if (end == std::string::npos) return false;
		out = payload.substr(at, end - at);
		at = end + 1;
		return true;
	};
	std::string header, events;
	if (!line(header) || header != kClipHeader || !line(clip.holds) || !line(events)) return false;
	if (events.compare(0, 7, "events=") != 0) return false;
	for (size_t i = 7; i < events.size();) {
		const size_t comma = events.find(',', i);
		const std::string one = events.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
		const std::optional<int> index = strutil::parse_int(one);
		if (!index || *index < 0) return false;
		clip.events.push_back(size_t(*index));
		if (comma == std::string::npos) break;
		i = comma + 1;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	return bytes_of_hex(payload, at, bytes) && bms::parse(bytes.data(), bytes.size(), clip.fragment, error);
}

std::string write_clip(const std::string &holds, const std::vector<size_t> &events, bms::File &fragment) {
	sync_counts(fragment);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bms::write(fragment, bytes, error)) return std::string();
	// The payload must read back as written: what the format cannot carry is refused here, not
	// lost in the paste.
	bms::File back;
	std::vector<uint8_t> again;
	if (!bms::parse(bytes.data(), bytes.size(), back, error) || !bms::write(back, again, error) || again != bytes)
		return std::string();
	std::string out = std::string(kClipHeader) + "\n" + holds + "\nevents=";
	for (size_t i = 0; i < events.size(); ++i) out += (i ? "," : "") + std::to_string(events[i]);
	out += "\n" + hex_of(bytes);
	return out;
}

const char *holds_of(NodeKind kind) {
	switch (static_cast<K>(kind)) {
	case K::Loadout: return "loadout";
	case K::Availability: return "availability";
	case K::BoundingBox: return "bounding_box";
	case K::Stop: return "stop";
	case K::Trigger: return "trigger";
	case K::Action: return "action";
	default: return nullptr;
	}
}

// A row's index among the rows of its kind.
size_t index_among(const std::vector<std::shared_ptr<const Node>> &rows, NodeId id, NodeKind kind) {
	size_t index = 0;
	for (const auto &row : rows) {
		if (row->id == id) return index;
		if (row->kind == kind) ++index;
	}
	return SIZE_MAX;
}

int32_t &param_of(bms::Trigger &trigger, int slot) {
	return slot == 0 ? trigger.param1 : slot == 1 ? trigger.param2 : slot == 2 ? trigger.param3 : trigger.param4;
}
int32_t &param_of(bms::Action &action, int slot) {
	return slot == 0 ? action.param1 : slot == 1 ? action.param2 : slot == 2 ? action.param3 : action.param4;
}

// The parameters of a fragment's events and the riders of its entities that name an SSN or a
// zone by id: each `old` made `now`.
void follow_id(bms::File &fragment, ParamKind kind, int32_t old, int32_t now) {
	if (old == now) return;
	for (bms::Trigger &trigger : fragment.triggers)
		for (int slot = 0; slot < 4; ++slot)
			if (trigger_param_kind(trigger, slot) == kind && param_of(trigger, slot) == old) param_of(trigger, slot) = now;
	for (bms::Action &action : fragment.actions)
		for (int slot = 0; slot < 4; ++slot)
			if (action_param_kind(action, slot) == kind && param_of(action, slot) == old) param_of(action, slot) = now;
	if (kind != ParamKind::Entity) return;
	for (std::vector<bms::Entity> *pool : {&fragment.items, &fragment.buildings, &fragment.markers, &fragment.organics})
		for (bms::Entity &entity : *pool)
			if (path_command_names_entity(entity.waypoint_id) && entity.wp_number == old) entity.wp_number = now;
}

} // namespace

std::string MissionDocument::copy(const std::vector<NodeAddress> &records) const {
	if (records.empty()) return std::string();
	bms::File fragment;
	make_default(fragment);
	std::vector<size_t> events;
	// Rows: entities of any pools, area triggers and events together, in the rows' order.
	if (!records.front().child) {
		std::set<NodeId> wanted;
		for (const NodeAddress &record : records) {
			const Node *node = row(record.row);
			if (record.child || !node || !(is_entity_kind(node->kind) || node->kind == k(K::Area) || node->kind == k(K::Event)))
				return std::string();
			wanted.insert(record.row);
		}
		std::vector<EventChain> chains;
		for (const auto &node : rows()) {
			if (!wanted.count(node->id)) continue;
			switch (static_cast<K>(node->kind)) {
			case K::Item: fragment.items.push_back(static_cast<const EntityRow &>(*node).native); break;
			case K::Building: fragment.buildings.push_back(static_cast<const EntityRow &>(*node).native); break;
			case K::Marker: fragment.markers.push_back(static_cast<const EntityRow &>(*node).native); break;
			case K::Organic: fragment.organics.push_back(static_cast<const EntityRow &>(*node).native); break;
			case K::Area: fragment.area_triggers.push_back(static_cast<const AreaRow &>(*node).native); break;
			case K::Event:
				events.push_back(index_among(rows(), node->id, k(K::Event)));
				chains.push_back(static_cast<const EventRow &>(*node).native);
				break;
			default: break;
			}
		}
		join_event_chains(chains, fragment);
		return write_clip("rows", events, fragment);
	}
	// Nested records: of one kind, from one owner, in their list's order.
	const Node *node = row(records.front().row);
	if (!node) return std::string();
	const NodeKind kind = records.front().kind;
	const char *holds = holds_of(kind);
	if (!holds) return std::string();
	std::set<NodeId> wanted;
	for (const NodeAddress &record : records) {
		if (record.row != node->id || record.kind != kind || !record.child) return std::string();
		wanted.insert(record.child);
	}
	Located first;
	if (!locate(*node, records.front().child, first) || first.is_row()) return std::string();
	const Located owner = owner_of(first);
	const size_t list = first.step().list;
	const TableKind *held = mission_table().kind(owner.record.kind);
	if (!held || list >= held->lists().size() || !owner.ids || list >= owner.ids->lists.size()) return std::string();
	const ListOps &ops = held->lists()[list].ops;
	const std::vector<RecordIds> &ids = owner.ids->lists[list];
	size_t found = 0;
	EventChain chain;
	for (size_t i = 0; i < ids.size(); ++i) {
		if (!wanted.count(ids[i].id)) continue;
		++found;
		const RecordHandle record = ops.at(owner.record, i);
		if (!record || record.kind != kind) return std::string();
		switch (static_cast<K>(kind)) {
		case K::Loadout: fragment.loadout.entries.push_back(record.as<bms::WeaponLoadoutRecord>()); break;
		case K::Availability: fragment.item_availability.push_back(record.as<bms::ItemAvailabilityEntry>()); break;
		case K::BoundingBox: fragment.bounding_boxes.push_back(record.as<bms::BoundingBox>()); break;
		case K::Stop: fragment.waypoint_records[0].waypoint_numbers.push_back(record.as<uint32_t>()); break;
		case K::Trigger: chain.triggers.push_back(record.as<bms::Trigger>()); break;
		case K::Action: chain.actions.push_back(record.as<bms::Action>()); break;
		default: break;
		}
	}
	// Every record asked for, of the one owner (one of another owner is not in this list).
	if (found != wanted.size()) return std::string();
	if (kind == k(K::Stop))
		fragment.waypoint_records[0].marker_count = uint32_t(fragment.waypoint_records[0].waypoint_numbers.size());
	if (kind == k(K::Trigger) || kind == k(K::Action)) join_event_chains({chain}, fragment);
	return write_clip(holds, {}, fragment);
}

bool mission_clip_middle(const std::string &payload, double out[2]) {
	Clip clip;
	if (!read_clip(payload, clip) || clip.holds != "rows") return false;
	const bms::File &fragment = clip.fragment;
	bool any = false;
	double low[2] = { 0.0, 0.0 }, high[2] = { 0.0, 0.0 };
	const auto take = [&](double x, double y) {
		if (!any) {
			low[0] = high[0] = x;
			low[1] = high[1] = y;
		}
		any = true;
		low[0] = std::min(low[0], x);
		low[1] = std::min(low[1], y);
		high[0] = std::max(high[0], x);
		high[1] = std::max(high[1], y);
	};
	for (const std::vector<bms::Entity> *pool : { &fragment.items, &fragment.buildings, &fragment.markers, &fragment.organics })
		for (const bms::Entity &entity : *pool) take(entity.x / 65536.0, entity.y / 65536.0);
	for (const bms::AreaTrigger &area : fragment.area_triggers)
		take((area.x_min + double(area.x_max)) / 131072.0, (area.y_min + double(area.y_max)) / 131072.0);
	if (!any) return false;
	out[0] = (low[0] + high[0]) * 0.5;
	out[1] = (low[1] + high[1]) * 0.5;
	return true;
}

std::string mission_clip_moved(const std::string &payload, double east, double north, double up) {
	Clip clip;
	if (!read_clip(payload, clip) || clip.holds != "rows") return std::string();
	bms::File &fragment = clip.fragment;
	const int32_t dx = bms::to_fixed_16_16(east), dy = bms::to_fixed_16_16(north), dz = bms::to_fixed_16_16(up);
	for (std::vector<bms::Entity> *pool : { &fragment.items, &fragment.buildings, &fragment.markers, &fragment.organics })
		for (bms::Entity &entity : *pool) {
			entity.x += dx;
			entity.y += dy;
			entity.z += dz;
		}
	for (bms::AreaTrigger &area : fragment.area_triggers) {
		area.x_min += dx;
		area.x_max += dx;
		area.y_min += dy;
		area.y_max += dy;
	}
	return write_clip(clip.holds, clip.events, fragment);
}

bool MissionDocument::pastes_rows(const std::string &payload) const {
	const size_t first = payload.find('\n');
	if (first == std::string::npos || payload.compare(0, first, kClipHeader) != 0) return false;
	const size_t second = payload.find('\n', first + 1);
	return second != std::string::npos && payload.compare(first + 1, second - first - 1, "rows") == 0;
}

bool MissionDocument::paste_rows(const Edit &edit, const std::vector<std::shared_ptr<const Node>> &rows,
                                 std::vector<std::shared_ptr<Node>> &out, std::string &error) {
	Clip clip;
	const auto *payload = std::get_if<std::string>(&edit.value);
	if (!payload || !read_clip(*payload, clip) || clip.holds != "rows") {
		error = "The clipboard holds no mission records.";
		return false;
	}
	bms::File &fragment = clip.fragment;
	// Told apart from the rows there: an SSN a row carries, a zone id taken, each given a fresh one,
	// the fragment's own references following.
	std::set<int32_t> ssns, zones;
	for (const auto &row : rows) {
		if (is_entity_kind(row->kind)) ssns.insert(static_cast<const EntityRow &>(*row).native.id);
		if (row->kind == k(K::Area)) zones.insert(static_cast<const AreaRow &>(*row).native.id);
	}
	int32_t next_ssn = next_free_ssn(rows);
	// Every SSN of the fragment first: a fresh one given to one copy is none another copy holds, so a
	// later copy's own SSN is never taken for it (follow_id follows by value). The player's 10000 is
	// never given.
	std::set<int32_t> held;
	for (const std::vector<bms::Entity> *pool : {&fragment.items, &fragment.buildings, &fragment.markers, &fragment.organics})
		for (const bms::Entity &entity : *pool) held.insert(entity.id);
	for (std::vector<bms::Entity> *pool : {&fragment.items, &fragment.buildings, &fragment.markers, &fragment.organics})
		for (bms::Entity &entity : *pool) {
			if (ssns.insert(entity.id).second) continue;
			while (ssns.count(next_ssn) || held.count(next_ssn) || next_ssn == 10000) ++next_ssn;
			const int32_t fresh = next_ssn++;
			follow_id(fragment, ParamKind::Entity, entity.id, fresh);
			entity.id = fresh;
			ssns.insert(fresh);
		}
	// A zone id a row holds, in 1..99 or not, gives the copy the lowest free one in 1..99.
	std::set<int32_t> copied_zones;
	for (const bms::AreaTrigger &area : fragment.area_triggers) copied_zones.insert(area.id);
	for (bms::AreaTrigger &area : fragment.area_triggers) {
		if (zones.insert(area.id).second) continue;
		int fresh = 0;
		for (int id = 1; id <= 99 && !fresh; ++id)
			if (!zones.count(id) && !copied_zones.count(id)) fresh = id;
		if (!fresh) {
			error = "Every zone id 1 to 99 is taken: the pasted area trigger has none to take.";
			return false;
		}
		follow_id(fragment, ParamKind::Zone, area.id, fresh);
		area.id = fresh;
		zones.insert(fresh);
	}
	std::vector<EventChain> chains;
	RunReport report;
	if (!split_event_chains(fragment, chains, report)) {
		error = "The clipboard's events cannot be read.";
		return false;
	}
	// An event index naming a copied event names that copy: an EventLink to its place among the
	// copies, which renumber_references reads once the step has put them in (the events grew), the
	// copies standing in their order wherever they landed. One naming an event that was not copied
	// names the event of that index here, which the same step moves as it moves that event.
	const auto copy_named = [&](int32_t value) -> int64_t {
		for (size_t i = 0; i < clip.events.size() && i < chains.size(); ++i)
			if (value >= 0 && size_t(value) == clip.events[i]) return int64_t(i);
		return -1;
	};
	std::vector<std::vector<EventLink>> links(chains.size());
	for (size_t c = 0; c < chains.size(); ++c) {
		for (size_t i = 0; i < chains[c].triggers.size(); ++i)
			if (trigger_param_kind(chains[c].triggers[i], 0) == ParamKind::Event)
				if (const int64_t copy = copy_named(chains[c].triggers[i].param1); copy >= 0)
					links[c].push_back({0, uint32_t(i), uint32_t(copy)});
		for (size_t i = 0; i < chains[c].actions.size(); ++i)
			if (action_param_kind(chains[c].actions[i], 0) == ParamKind::Event)
				if (const int64_t copy = copy_named(chains[c].actions[i].param1); copy >= 0)
					links[c].push_back({1, uint32_t(i), uint32_t(copy)});
	}
	// The rows, in band order (the base puts each where its band is, row_position).
	const auto entities = [&](K kind, const std::vector<bms::Entity> &records) {
		for (const bms::Entity &record : records) {
			auto row = std::make_shared<EntityRow>(k(kind), record);
			shape(*row);
			out.push_back(row);
		}
	};
	entities(K::Item, fragment.items);
	entities(K::Building, fragment.buildings);
	entities(K::Marker, fragment.markers);
	entities(K::Organic, fragment.organics);
	for (const bms::AreaTrigger &area : fragment.area_triggers) {
		auto row = std::make_shared<AreaRow>(k(K::Area), area);
		shape(*row);
		out.push_back(row);
	}
	for (size_t c = 0; c < chains.size(); ++c) {
		auto row = std::make_shared<EventRow>(k(K::Event), std::move(chains[c]));
		row->links = std::move(links[c]);
		shape(*row);
		out.push_back(row);
	}
	if (out.empty()) {
		error = "The clipboard holds no mission records.";
		return false;
	}
	return true;
}

bool MissionDocument::paste_records(Node &node, const Edit &edit, const IdAllocator &allocate, std::vector<NodeId> &added,
                                    std::string &error) {
	Clip clip;
	const auto *payload = std::get_if<std::string>(&edit.value);
	if (!payload || !read_clip(*payload, clip) || clip.holds == "rows") {
		error = "The clipboard holds no records to paste into this one.";
		return false;
	}
	Located owner;
	if (!locate(node, edit.parent, owner)) {
		error = "The record to paste into no longer exists.";
		return false;
	}
	// The kind the payload holds, and the owner's list of it.
	NodeKind kind = -1;
	for (const K each : {K::Loadout, K::Availability, K::BoundingBox, K::Stop, K::Trigger, K::Action})
		if (clip.holds == holds_of(k(each))) kind = k(each);
	size_t list = 0;
	if (kind < 0 || !list_of(owner, kind, list)) {
		error = "The clipboard's records go into another kind of record (a " + clip.holds + " into what holds one).";
		return false;
	}
	std::vector<DetachedRecord> records;
	const auto detach = [&](auto &vector) {
		for (auto &record : vector) {
			DetachedRecord detached;
			detached.kind = kind;
			detached.data = std::make_shared<std::remove_reference_t<decltype(record)>>(record);
			records.push_back(std::move(detached));
		}
	};
	bms::File &fragment = clip.fragment;
	switch (static_cast<K>(kind)) {
	case K::Loadout: detach(fragment.loadout.entries); break;
	case K::Availability: detach(fragment.item_availability); break;
	case K::BoundingBox: detach(fragment.bounding_boxes); break;
	case K::Stop: {
		// The stops the count says the path holds (the slots past them are the file's padding).
		bms::WaypointRecord &path = fragment.waypoint_records[0];
		path.waypoint_numbers.resize(std::min<size_t>(path.waypoint_numbers.size(), path.marker_count));
		detach(path.waypoint_numbers);
		break;
	}
	case K::Trigger: detach(fragment.triggers); break;
	case K::Action: detach(fragment.actions); break;
	default: break;
	}
	if (records.empty()) {
		error = "The clipboard holds no records to paste into this one.";
		return false;
	}
	// The type's own rule about the list (a path counted past its slots takes no stop).
	ListChange change;
	change.operation = EditOperation::Paste;
	change.owner = &owner;
	change.list = list;
	if (!accept_list_edit(node, change, error)) return false;
	const ListOps &ops = mission_table().kind(owner.record.kind)->lists()[list].ops;
	size_t position = std::min(edit.position, ops.size(owner.record));
	for (const DetachedRecord &record : records) {
		NodeId one = 0;
		if (!insert_record(owner, list, position, record, allocate, one, error)) return false;
		added.push_back(one);
		++position;
	}
	return true;
}

} // namespace opennova::editor
