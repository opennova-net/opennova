#include "nova_mission_data.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using namespace godot;

namespace {

opennova::mission::EntityKind to_native_kind(int kind) {
	switch (kind) {
		case NovaMissionData::KIND_MARKER:
			return opennova::mission::EntityKind::Marker;
		case NovaMissionData::KIND_BUILDING:
			return opennova::mission::EntityKind::Building;
		case NovaMissionData::KIND_ORGANIC:
			return opennova::mission::EntityKind::Organic;
		case NovaMissionData::KIND_ITEM:
		default:
			return opennova::mission::EntityKind::Item;
	}
}

} // namespace

void NovaMissionData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_file", "path"), &NovaMissionData::open_file);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name"), &NovaMissionData::open_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaMissionData::is_loaded);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaMissionData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaMissionData::get_last_error);
	ClassDB::bind_method(D_METHOD("get_mission_name"), &NovaMissionData::get_mission_name);
	ClassDB::bind_method(D_METHOD("get_designer"), &NovaMissionData::get_designer);
	ClassDB::bind_method(D_METHOD("get_terrain_ref"), &NovaMissionData::get_terrain_ref);
	ClassDB::bind_method(D_METHOD("get_environment_ref"), &NovaMissionData::get_environment_ref);
	ClassDB::bind_method(D_METHOD("get_info"), &NovaMissionData::get_info);
	ClassDB::bind_method(D_METHOD("get_entity_count", "kind"), &NovaMissionData::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entities", "kind"), &NovaMissionData::get_entities);
	ClassDB::bind_method(D_METHOD("get_entity", "kind", "index"), &NovaMissionData::get_entity);
	ClassDB::bind_method(D_METHOD("get_all_entities"), &NovaMissionData::get_all_entities);

	ClassDB::bind_method(D_METHOD("set_entity_transform", "kind", "index", "position", "rotation_deg"), &NovaMissionData::set_entity_transform);
	ClassDB::bind_method(D_METHOD("set_entity_property_int", "kind", "index", "property", "value"), &NovaMissionData::set_entity_property_int);
	ClassDB::bind_method(D_METHOD("add_entity", "kind", "item_id", "position", "rotation_deg"), &NovaMissionData::add_entity);
	ClassDB::bind_method(D_METHOD("remove_entity", "kind", "index"), &NovaMissionData::remove_entity);

	ClassDB::bind_method(D_METHOD("get_waypoint_summaries"), &NovaMissionData::get_waypoint_summaries);
	ClassDB::bind_method(D_METHOD("get_waypoint_path", "index"), &NovaMissionData::get_waypoint_path);
	ClassDB::bind_method(D_METHOD("get_waypoint_paths"), &NovaMissionData::get_waypoint_paths);
	ClassDB::bind_method(D_METHOD("set_waypoint_path", "index", "marker_indices", "flags"), &NovaMissionData::set_waypoint_path);
	ClassDB::bind_method(D_METHOD("clear_waypoint_path", "index"), &NovaMissionData::clear_waypoint_path);
	ClassDB::bind_method(D_METHOD("add_waypoint_marker", "path_index", "marker_item_id", "position", "rotation_deg", "insert_index"), &NovaMissionData::add_waypoint_marker);

	ClassDB::bind_method(D_METHOD("save_file"), &NovaMissionData::save_file);
	ClassDB::bind_method(D_METHOD("save_as", "path"), &NovaMissionData::save_as);
	ClassDB::bind_method(D_METHOD("is_modified"), &NovaMissionData::is_modified);
	ClassDB::bind_method(D_METHOD("snapshot"), &NovaMissionData::snapshot);
	ClassDB::bind_method(D_METHOD("restore_snapshot", "bytes"), &NovaMissionData::restore_snapshot);

	BIND_CONSTANT(KIND_MARKER);
	BIND_CONSTANT(KIND_ITEM);
	BIND_CONSTANT(KIND_BUILDING);
	BIND_CONSTANT(KIND_ORGANIC);
	BIND_CONSTANT(WP_FLAG_DOES_NOT_LOOP);
	BIND_CONSTANT(WP_FLAG_BLUE_TEAM);
	BIND_CONSTANT(WP_FLAG_RED_TEAM);
}

Error NovaMissionData::open_file(const String &path) {
	source_path = path;
	last_error = String();
	if (!document.load_bms_file(path.utf8().get_data())) {
		last_error = String(document.last_error().c_str());
		return ERR_CANT_OPEN;
	}
	modified = false;
	return OK;
}

Error NovaMissionData::open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
	last_error = String();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.is_empty()) {
		last_error = "Mission filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file);
	if (bytes.is_empty()) {
		last_error = "Mission file not found in resource root: " + file;
		return ERR_FILE_NOT_FOUND;
	}
	if (!document.load_bms_bytes(bytes.ptr(), static_cast<size_t>(bytes.size()))) {
		last_error = String(document.last_error().c_str());
		return ERR_CANT_OPEN;
	}
	source_path = file;
	modified = false;
	return OK;
}

bool NovaMissionData::is_loaded() const {
	return document.is_loaded();
}

String NovaMissionData::get_source_path() const {
	return source_path;
}

String NovaMissionData::get_last_error() const {
	return last_error;
}

String NovaMissionData::get_mission_name() const {
	return String(document.info().mission_name.c_str());
}

String NovaMissionData::get_designer() const {
	return String(document.info().designer.c_str());
}

String NovaMissionData::get_terrain_ref() const {
	return String(document.info().terrain.c_str());
}

String NovaMissionData::get_environment_ref() const {
	return String(document.info().environment.c_str());
}

Dictionary NovaMissionData::get_info() const {
	const opennova::mission::MissionInfo info = document.info();
	Dictionary out;
	out["mission_name"] = String(info.mission_name.c_str());
	out["designer"] = String(info.designer.c_str());
	out["briefing"] = String(info.briefing.c_str());
	out["terrain"] = String(info.terrain.c_str());
	out["environment"] = String(info.environment.c_str());
	out["climate"] = info.climate;
	out["weather"] = info.weather;
	out["attrib_flags"] = info.attrib_flags;
	out["start_time"] = info.start_time;
	out["minutes_per_day"] = info.minutes_per_day;
	out["player_health"] = info.player_health;
	out["max_saves"] = info.max_saves;
	out["music"] = info.music;
	out["reverb"] = info.reverb;
	return out;
}

Dictionary NovaMissionData::entity_to_dictionary(const opennova::mission::EntityRecord &record) const {
	Dictionary out;
	out["kind"] = static_cast<int>(record.kind);
	out["index"] = static_cast<int>(record.index);
	// item_id is the items.def key (bms type_id + 100000); use it to look up the model.
	out["item_id"] = record.item_id;
	out["type_id"] = record.bms_type_id;
	out["bms_id"] = record.bms_id;
	// Mission-space position (already converted from 16.16 fixed-point to float).
	out["position"] = Vector3(record.transform.x, record.transform.y, record.transform.z);
	// Euler degrees as authored; coordinate conversion to Godot space happens in
	// the placement layer where terrain context is available.
	out["rotation_deg"] = Vector3(record.transform.pitch, record.transform.yaw, record.transform.roll);
	out["group"] = record.group_id;
	out["waypoint_id"] = record.waypoint_id;
	out["wp_number"] = record.wp_number;
	out["team"] = record.team;
	out["ai_flags"] = record.ai_flags;
	out["perception"] = record.perception;
	out["accuracy"] = record.accuracy;
	out["alert_state"] = record.alert_state;
	out["min_engagement_distance"] = record.min_engagement_distance;
	out["max_engagement_distance"] = record.max_engagement_distance;
	out["max_attack_distance"] = record.max_attack_distance;
	out["spawn_count"] = record.spawn_count;
	out["max_simultaneous"] = record.max_simultaneous;
	return out;
}

int NovaMissionData::get_entity_count(int kind) const {
	return static_cast<int>(document.entity_count(to_native_kind(kind)));
}

Array NovaMissionData::get_entities(int kind) const {
	Array out;
	const opennova::mission::EntityKind native_kind = to_native_kind(kind);
	const size_t count = document.entity_count(native_kind);
	for (size_t i = 0; i < count; ++i) {
		opennova::mission::EntityRecord record;
		if (document.get_entity(native_kind, i, record)) {
			out.push_back(entity_to_dictionary(record));
		}
	}
	return out;
}

Dictionary NovaMissionData::get_entity(int kind, int index) const {
	if (index < 0) {
		return Dictionary();
	}
	opennova::mission::EntityRecord record;
	if (!document.get_entity(to_native_kind(kind), static_cast<size_t>(index), record)) {
		return Dictionary();
	}
	return entity_to_dictionary(record);
}

Array NovaMissionData::get_all_entities() const {
	Array out;
	const int kinds[] = { KIND_MARKER, KIND_ITEM, KIND_BUILDING, KIND_ORGANIC };
	for (int kind : kinds) {
		const Array entities = get_entities(kind);
		for (int i = 0; i < entities.size(); ++i) {
			out.push_back(entities[i]);
		}
	}
	return out;
}

bool NovaMissionData::set_entity_transform(int kind, int index, const Vector3 &position, const Vector3 &rotation_deg) {
	if (index < 0) {
		return false;
	}
	opennova::mission::EntityTransform transform;
	transform.x = position.x;
	transform.y = position.y;
	transform.z = position.z;
	// The format stores orientation as integer degrees; round rather than truncate.
	transform.pitch = static_cast<int>(std::lround(rotation_deg.x));
	transform.yaw = static_cast<int>(std::lround(rotation_deg.y));
	transform.roll = static_cast<int>(std::lround(rotation_deg.z));
	if (!document.set_entity_transform(to_native_kind(kind), static_cast<size_t>(index), transform)) {
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::set_entity_property_int(int kind, int index, const String &property, int value) {
	if (index < 0) {
		return false;
	}
	const opennova::mission::EntityKind native_kind = to_native_kind(kind);
	opennova::mission::EntityRecord record;
	if (!document.get_entity(native_kind, static_cast<size_t>(index), record)) {
		return false;
	}
	// set_entity_properties overwrites every field of EntityProperties, so seed it
	// from the entity's current state and change only the requested one. This keeps
	// the other twelve AI/waypoint properties intact.
	opennova::mission::EntityProperties properties;
	properties.group_id = record.group_id;
	properties.waypoint_id = record.waypoint_id;
	properties.wp_number = record.wp_number;
	properties.team = record.team;
	properties.ai_flags = record.ai_flags;
	properties.perception = record.perception;
	properties.accuracy = record.accuracy;
	properties.alert_state = record.alert_state;
	properties.min_engagement_distance = record.min_engagement_distance;
	properties.max_engagement_distance = record.max_engagement_distance;
	properties.max_attack_distance = record.max_attack_distance;
	properties.spawn_count = record.spawn_count;
	properties.max_simultaneous = record.max_simultaneous;

	// Each name matches the entity dictionary key it edits (group -> group_id). Any name
	// not in this set is rejected rather than silently no-op'd.
	if (property == "team") {
		properties.team = value;
	} else if (property == "group") {
		properties.group_id = value;
	} else if (property == "waypoint_id") {
		properties.waypoint_id = value;
	} else if (property == "wp_number") {
		properties.wp_number = value;
	} else if (property == "perception") {
		properties.perception = value;
	} else if (property == "accuracy") {
		properties.accuracy = value;
	} else if (property == "alert_state") {
		properties.alert_state = value;
	} else if (property == "min_engagement_distance") {
		properties.min_engagement_distance = value;
	} else if (property == "max_engagement_distance") {
		properties.max_engagement_distance = value;
	} else if (property == "max_attack_distance") {
		properties.max_attack_distance = value;
	} else if (property == "spawn_count") {
		properties.spawn_count = value;
	} else if (property == "max_simultaneous") {
		properties.max_simultaneous = value;
	} else if (property == "ai_flags") {
		properties.ai_flags = value;
	} else {
		return false;
	}
	if (!document.set_entity_properties(native_kind, static_cast<size_t>(index), properties, nullptr)) {
		return false;
	}
	modified = true;
	return true;
}

Dictionary NovaMissionData::add_entity(int kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg) {
	opennova::mission::EntityTransform transform;
	transform.x = position.x;
	transform.y = position.y;
	transform.z = position.z;
	// Same rounding contract as set_entity_transform: the format stores integer degrees.
	transform.pitch = static_cast<int>(std::lround(rotation_deg.x));
	transform.yaw = static_cast<int>(std::lround(rotation_deg.y));
	transform.roll = static_cast<int>(std::lround(rotation_deg.z));
	opennova::mission::EntityRecord record;
	if (!document.add_entity(to_native_kind(kind), item_id, transform, &record)) {
		return Dictionary();
	}
	modified = true;
	return entity_to_dictionary(record);
}

bool NovaMissionData::remove_entity(int kind, int index) {
	if (index < 0) {
		return false;
	}
	if (!document.remove_entity(to_native_kind(kind), static_cast<size_t>(index))) {
		return false;
	}
	modified = true;
	return true;
}

Dictionary NovaMissionData::waypoint_path_to_dictionary(const opennova::mission::WaypointPath &path) const {
	Dictionary out;
	out["index"] = static_cast<int>(path.index);
	out["flags"] = path.flags;
	out["marker_count"] = static_cast<int>(path.marker_indices.size());
	PackedInt32Array indices;
	indices.resize(static_cast<int64_t>(path.marker_indices.size()));
	for (size_t i = 0; i < path.marker_indices.size(); ++i) {
		indices.set(static_cast<int64_t>(i), path.marker_indices[i]);
	}
	out["marker_indices"] = indices;
	return out;
}

Array NovaMissionData::get_waypoint_summaries() const {
	Array out;
	for (const opennova::mission::WaypointSummary &summary : document.waypoint_summaries()) {
		Dictionary d;
		d["index"] = static_cast<int>(summary.index);
		d["flags"] = summary.flags;
		d["marker_count"] = summary.marker_count;
		out.push_back(d);
	}
	return out;
}

Dictionary NovaMissionData::get_waypoint_path(int index) const {
	if (index < 0) {
		return Dictionary();
	}
	opennova::mission::WaypointPath path;
	if (!document.get_waypoint_path(static_cast<size_t>(index), path)) {
		return Dictionary();
	}
	return waypoint_path_to_dictionary(path);
}

Array NovaMissionData::get_waypoint_paths() const {
	Array out;
	for (const opennova::mission::WaypointPath &path : document.waypoint_paths()) {
		out.push_back(waypoint_path_to_dictionary(path));
	}
	return out;
}

bool NovaMissionData::set_waypoint_path(int index, const PackedInt32Array &marker_indices, int flags) {
	if (index < 0) {
		return false;
	}
	std::vector<int> indices;
	indices.reserve(static_cast<size_t>(marker_indices.size()));
	for (int64_t i = 0; i < marker_indices.size(); ++i) {
		indices.push_back(marker_indices[i]);
	}
	if (!document.set_waypoint_path(static_cast<size_t>(index), indices, flags, nullptr)) {
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::clear_waypoint_path(int index) {
	if (index < 0) {
		return false;
	}
	if (!document.clear_waypoint_path(static_cast<size_t>(index), nullptr)) {
		return false;
	}
	modified = true;
	return true;
}

Dictionary NovaMissionData::add_waypoint_marker(int path_index, int marker_item_id, const Vector3 &position, const Vector3 &rotation_deg, int insert_index) {
	if (path_index < 0) {
		return Dictionary();
	}
	opennova::mission::EntityTransform transform;
	transform.x = position.x;
	transform.y = position.y;
	transform.z = position.z;
	// Same rounding contract as set_entity_transform / add_entity: integer degrees.
	transform.pitch = static_cast<int>(std::lround(rotation_deg.x));
	transform.yaw = static_cast<int>(std::lround(rotation_deg.y));
	transform.roll = static_cast<int>(std::lround(rotation_deg.z));
	opennova::mission::EntityRecord marker;
	opennova::mission::WaypointPath path;
	if (!document.add_waypoint_marker(static_cast<size_t>(path_index), marker_item_id, transform,
				insert_index, &marker, &path)) {
		return Dictionary();
	}
	modified = true;
	Dictionary out;
	out["marker"] = entity_to_dictionary(marker);
	out["path"] = waypoint_path_to_dictionary(path);
	return out;
}

Error NovaMissionData::save_file() {
	if (source_path.is_empty()) {
		// No path yet: let the shell route to Save As (matches the editor save contract).
		return ERR_INVALID_PARAMETER;
	}
	return save_as(source_path);
}

Error NovaMissionData::save_as(const String &path) {
	last_error = String();
	if (path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	if (!document.save_bms_file(path.utf8().get_data())) {
		last_error = String(document.last_error().c_str());
		return ERR_FILE_CANT_WRITE;
	}
	source_path = path;
	modified = false;
	return OK;
}

bool NovaMissionData::is_modified() const {
	return modified;
}

PackedByteArray NovaMissionData::snapshot() {
	PackedByteArray out;
	std::vector<uint8_t> bytes;
	// write_bms_bytes returns false when nothing is loaded; surface an empty array so
	// the caller can skip pushing a meaningless snapshot.
	if (!document.write_bms_bytes(bytes)) {
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

bool NovaMissionData::restore_snapshot(const PackedByteArray &bytes) {
	// load_bms_bytes clears the document before parsing, so on failure we are left with
	// an empty document; return the parse result and let the caller decide.
	const bool ok = document.load_bms_bytes(bytes.ptr(), static_cast<size_t>(bytes.size()));
	if (!ok) {
		last_error = String(document.last_error().c_str());
	}
	return ok;
}
