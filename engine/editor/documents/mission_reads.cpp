// The mission picture's reads (mission_reads.h): each entity, area trigger and used path of a mission
// document as typed records, the markers a path's stops name resolved to their rows [orig: a stop's
// word is the marker's index in the pool, Pool_GetEntryUnchecked @0x441FC0].
#include "mission_reads.h"

#include <base/io/fixed.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

namespace opennova::editor {

namespace {

using K = MissionKind;
constexpr NodeKind k(K kind) { return node_kind(kind); }

MissionEntityRead read_entity(const EntityRow &row) {
	const bms::Entity &e = row.native;
	MissionEntityRead out;
	out.address = {row.id, row.kind, 0};
	out.pool = static_cast<K>(row.kind);
	out.item_id = mission::entity_item_id(e);
	out.ssn = e.id;
	out.x = io::fp16_16_to_double(e.x);
	out.y = io::fp16_16_to_double(e.y);
	out.z = io::fp16_16_to_double(e.z);
	const mission::EntityTransform transform = mission::entity_transform(e);
	out.pitch = transform.pitch;
	out.yaw = transform.yaw;
	out.roll = transform.roll;
	out.group = e.group_id;
	out.waypoint_id = e.waypoint_id;
	out.wp_number = e.wp_number;
	out.team = e.team;
	out.attributes = e.bmsi_attributes;
	out.alert_state = e.alert_state;
	return out;
}

MissionAreaRead read_area(const AreaRow &row) {
	const bms::AreaTrigger &a = row.native;
	MissionAreaRead out;
	out.address = {row.id, row.kind, 0};
	out.id = a.id;
	out.x_min = io::fp16_16_to_double(a.x_min);
	out.x_max = io::fp16_16_to_double(a.x_max);
	out.y_min = io::fp16_16_to_double(a.y_min);
	out.y_max = io::fp16_16_to_double(a.y_max);
	out.z_min = io::fp16_16_to_double(a.z_min);
	out.z_max = io::fp16_16_to_double(a.z_max);
	out.constrains_z = a.constrains_z();
	out.mission_area = a.is_active();
	return out;
}

MissionPathRead read_path(const PathRow &row, const std::vector<const Node *> &markers) {
	MissionPathRead out;
	out.address = {row.id, row.kind, 0};
	out.number = row.native.number;
	out.flags = static_cast<uint32_t>(row.native.record.flags);
	const std::vector<uint32_t> &stops = row.native.stops;
	const std::vector<RecordIds> &ids = row.ids.lists.empty() ? std::vector<RecordIds>() : row.ids.lists[0];
	for (size_t i = 0; i < stops.size() && i < ids.size(); ++i) {
		out.stops.push_back({row.id, k(K::Stop), ids[i].id});
		const size_t index = stops[i];
		out.markers.push_back(index < markers.size() ? NodeAddress{markers[index]->id, markers[index]->kind, 0}
		                                             : NodeAddress());
	}
	return out;
}

} // namespace

std::vector<MissionEntityRead> mission_entities(const MissionDocument &document) {
	std::vector<MissionEntityRead> out;
	for (const auto &row : document.rows())
		if (row && is_entity_kind(row->kind)) out.push_back(read_entity(static_cast<const EntityRow &>(*row)));
	return out;
}

bool mission_entity(const MissionDocument &document, const NodeAddress &address, MissionEntityRead &out) {
	const Node *row = document.row(address.row);
	if (!row || address.child || !is_entity_kind(row->kind)) return false;
	out = read_entity(static_cast<const EntityRow &>(*row));
	return true;
}

std::vector<MissionAreaRead> mission_areas(const MissionDocument &document) {
	std::vector<MissionAreaRead> out;
	for (const Node *row : document.rows_of(K::Area)) out.push_back(read_area(static_cast<const AreaRow &>(*row)));
	return out;
}

bool mission_area(const MissionDocument &document, const NodeAddress &address, MissionAreaRead &out) {
	const Node *row = document.row(address.row);
	if (!row || address.child || row->kind != k(K::Area)) return false;
	out = read_area(static_cast<const AreaRow &>(*row));
	return true;
}

std::vector<MissionPathRead> mission_paths(const MissionDocument &document) {
	std::vector<MissionPathRead> out;
	const std::vector<const Node *> markers = document.rows_of(K::Marker);
	for (const Node *row : document.rows_of(K::WaypointPath)) {
		const PathRow &path = static_cast<const PathRow &>(*row);
		if (path.native.stops.empty()) continue;
		out.push_back(read_path(path, markers));
	}
	return out;
}

bool mission_path(const MissionDocument &document, const NodeAddress &address, MissionPathRead &out) {
	const Node *row = document.row(address.row);
	if (!row || address.child || row->kind != k(K::WaypointPath)) return false;
	out = read_path(static_cast<const PathRow &>(*row), document.rows_of(K::Marker));
	return true;
}

} // namespace opennova::editor
