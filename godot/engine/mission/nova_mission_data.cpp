#include "nova_mission_data.h"

#include "resource_index/nova_resource_root.h"

#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cmath>
#include <cstring>
#include <utility>
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
	ClassDB::bind_method(D_METHOD("create_default"), &NovaMissionData::create_default);
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
	ClassDB::bind_method(D_METHOD("set_entity_property_string", "kind", "index", "property", "value"), &NovaMissionData::set_entity_property_string);
	ClassDB::bind_method(D_METHOD("set_header_string", "field", "value"), &NovaMissionData::set_header_string);
	ClassDB::bind_method(D_METHOD("set_header_int", "field", "value"), &NovaMissionData::set_header_int);
	ClassDB::bind_method(D_METHOD("set_header_flag", "bit", "on"), &NovaMissionData::set_header_flag);
	ClassDB::bind_method(D_METHOD("set_header_float", "field", "value"), &NovaMissionData::set_header_float);
	ClassDB::bind_method(D_METHOD("get_game_mode"), &NovaMissionData::get_game_mode);
	ClassDB::bind_method(D_METHOD("set_game_mode", "bit"), &NovaMissionData::set_game_mode);
	ClassDB::bind_method(D_METHOD("add_entity", "kind", "item_id", "position", "rotation_deg"), &NovaMissionData::add_entity);
	ClassDB::bind_method(D_METHOD("remove_entity", "kind", "index"), &NovaMissionData::remove_entity);

	ClassDB::bind_method(D_METHOD("get_waypoint_summaries"), &NovaMissionData::get_waypoint_summaries);
	ClassDB::bind_method(D_METHOD("get_waypoint_path", "index"), &NovaMissionData::get_waypoint_path);
	ClassDB::bind_method(D_METHOD("get_waypoint_paths"), &NovaMissionData::get_waypoint_paths);
	ClassDB::bind_method(D_METHOD("set_waypoint_path", "index", "marker_indices", "flags"), &NovaMissionData::set_waypoint_path);
	ClassDB::bind_method(D_METHOD("clear_waypoint_path", "index"), &NovaMissionData::clear_waypoint_path);
	ClassDB::bind_method(D_METHOD("add_waypoint_marker", "path_index", "marker_item_id", "position", "rotation_deg", "insert_index"), &NovaMissionData::add_waypoint_marker);

	ClassDB::bind_method(D_METHOD("get_area_trigger_count"), &NovaMissionData::get_area_trigger_count);
	ClassDB::bind_method(D_METHOD("get_area_triggers"), &NovaMissionData::get_area_triggers);
	ClassDB::bind_method(D_METHOD("get_area_trigger", "index"), &NovaMissionData::get_area_trigger);
	ClassDB::bind_method(D_METHOD("add_area_trigger", "min_bounds", "max_bounds", "active", "constrain_z", "zone_id"), &NovaMissionData::add_area_trigger);
	ClassDB::bind_method(D_METHOD("set_area_trigger", "index", "min_bounds", "max_bounds", "active", "constrain_z", "zone_id"), &NovaMissionData::set_area_trigger);
	ClassDB::bind_method(D_METHOD("remove_area_trigger", "index"), &NovaMissionData::remove_area_trigger);

	ClassDB::bind_method(D_METHOD("get_weapon_loadout"), &NovaMissionData::get_weapon_loadout);
	ClassDB::bind_method(D_METHOD("set_weapon_loadout", "entries"), &NovaMissionData::set_weapon_loadout);
	ClassDB::bind_method(D_METHOD("get_group_count"), &NovaMissionData::get_group_count);
	ClassDB::bind_method(D_METHOD("get_groups"), &NovaMissionData::get_groups);
	ClassDB::bind_method(D_METHOD("get_group", "index"), &NovaMissionData::get_group);
	ClassDB::bind_method(D_METHOD("set_group", "index", "field0", "field8", "field12"), &NovaMissionData::set_group);

	ClassDB::bind_method(D_METHOD("get_event_count"), &NovaMissionData::get_event_count);
	ClassDB::bind_method(D_METHOD("get_events"), &NovaMissionData::get_events);
	ClassDB::bind_method(D_METHOD("get_event", "index"), &NovaMissionData::get_event);
	ClassDB::bind_method(D_METHOD("get_event_chain", "index"), &NovaMissionData::get_event_chain);
	ClassDB::bind_method(D_METHOD("get_logic_summary"), &NovaMissionData::get_logic_summary);
	ClassDB::bind_method(D_METHOD("add_event", "flags", "reset_after", "delay"), &NovaMissionData::add_event);
	ClassDB::bind_method(D_METHOD("remove_event", "index"), &NovaMissionData::remove_event);
	ClassDB::bind_method(D_METHOD("set_event", "index", "flags", "reset_after", "delay"), &NovaMissionData::set_event);
	ClassDB::bind_method(D_METHOD("add_event_trigger", "event_index", "trigger"), &NovaMissionData::add_event_trigger);
	ClassDB::bind_method(D_METHOD("set_event_trigger", "event_index", "local_index", "trigger"), &NovaMissionData::set_event_trigger);
	ClassDB::bind_method(D_METHOD("remove_event_trigger", "event_index", "local_index"), &NovaMissionData::remove_event_trigger);
	ClassDB::bind_method(D_METHOD("move_event_trigger", "event_index", "local_index", "delta"), &NovaMissionData::move_event_trigger);
	ClassDB::bind_method(D_METHOD("add_event_action", "event_index", "action"), &NovaMissionData::add_event_action);
	ClassDB::bind_method(D_METHOD("set_event_action", "event_index", "local_index", "action"), &NovaMissionData::set_event_action);
	ClassDB::bind_method(D_METHOD("remove_event_action", "event_index", "local_index"), &NovaMissionData::remove_event_action);
	ClassDB::bind_method(D_METHOD("move_event_action", "event_index", "local_index", "delta"), &NovaMissionData::move_event_action);
	ClassDB::bind_method(D_METHOD("get_trigger_main_types"), &NovaMissionData::get_trigger_main_types);
	ClassDB::bind_method(D_METHOD("get_trigger_sub_types", "main_type"), &NovaMissionData::get_trigger_sub_types);
	ClassDB::bind_method(D_METHOD("get_action_types"), &NovaMissionData::get_action_types);
	ClassDB::bind_method(D_METHOD("get_action_sub_types", "action_type"), &NovaMissionData::get_action_sub_types);
	ClassDB::bind_method(D_METHOD("get_event_flag_bits"), &NovaMissionData::get_event_flag_bits);

	ClassDB::bind_method(D_METHOD("save_file"), &NovaMissionData::save_file);
	ClassDB::bind_method(D_METHOD("save_as", "path"), &NovaMissionData::save_as);
	ClassDB::bind_method(D_METHOD("is_modified"), &NovaMissionData::is_modified);
	ClassDB::bind_method(D_METHOD("begin_edit"), &NovaMissionData::begin_edit);
	ClassDB::bind_method(D_METHOD("commit_edit"), &NovaMissionData::commit_edit);
	ClassDB::bind_method(D_METHOD("can_undo"), &NovaMissionData::can_undo);
	ClassDB::bind_method(D_METHOD("can_redo"), &NovaMissionData::can_redo);
	ClassDB::bind_method(D_METHOD("undo"), &NovaMissionData::undo);
	ClassDB::bind_method(D_METHOD("redo"), &NovaMissionData::redo);
	ClassDB::bind_method(D_METHOD("undo_depth"), &NovaMissionData::undo_depth);
	ClassDB::bind_method(D_METHOD("clear_history"), &NovaMissionData::clear_history);
	ClassDB::bind_method(D_METHOD("is_dirty"), &NovaMissionData::is_dirty);
	ClassDB::bind_method(D_METHOD("mark_clean"), &NovaMissionData::mark_clean);
	ClassDB::bind_method(D_METHOD("object_records_revision"), &NovaMissionData::object_records_revision);
	ClassDB::bind_method(D_METHOD("structure_fingerprint"), &NovaMissionData::structure_fingerprint);

	BIND_CONSTANT(KIND_MARKER);
	BIND_CONSTANT(KIND_ITEM);
	BIND_CONSTANT(KIND_BUILDING);
	BIND_CONSTANT(KIND_ORGANIC);
	BIND_CONSTANT(WP_FLAG_DOES_NOT_LOOP);
	BIND_CONSTANT(WP_FLAG_BLUE_TEAM);
	BIND_CONSTANT(WP_FLAG_RED_TEAM);
	BIND_CONSTANT(ATTRIB_ROTATE_MAP_180);
	BIND_CONSTANT(ATTRIB_ENABLE_NVG);
	BIND_CONSTANT(ATTRIB_ADVANCE_AND_SECURE);
	BIND_CONSTANT(ATTRIB_CONQUER_AND_CONTROL);
	BIND_CONSTANT(ATTRIB_ATTACK_AND_DEFEND);
	BIND_CONSTANT(ATTRIB_COOP);
	BIND_CONSTANT(ATTRIB_DEATHMATCH);
	BIND_CONSTANT(ATTRIB_KING_OF_THE_HILL);
	BIND_CONSTANT(ATTRIB_FLAGBALL);
	BIND_CONSTANT(ATTRIB_CAPTURE_THE_FLAG);
	BIND_CONSTANT(ATTRIB_TEAM_DEATHMATCH);
	BIND_CONSTANT(ATTRIB_TEAM_KING_OF_THE_HILL);
	BIND_CONSTANT(ATTRIB_SEARCH_AND_DESTROY);
	BIND_CONSTANT(ATTRIB_GAME_MODE_MASK);
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

Error NovaMissionData::create_default() {
	source_path = String();
	last_error = String();
	document.create_default();
	modified = false;
	clear_history();
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
	out["mission_type"] = info.mission_type;
	out["attrib_flags"] = info.attrib_flags;
	out["game_mode"] = get_game_mode();
	out["start_time"] = info.start_time;
	out["minutes_per_day"] = info.minutes_per_day;
	out["player_health"] = info.player_health;
	out["max_saves"] = info.max_saves;
	out["music"] = info.music;
	out["reverb"] = info.reverb;
	out["wind_speed"] = info.wind_speed;
	out["wind_direction"] = info.wind_direction;
	out["map_zoom"] = info.map_zoom;
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
	out["max_simultaneous"] = record.max_simultaneous;  // = no_more_than (byte 74)
	out["no_less_than"] = record.no_less_than;          // byte 75
	out["map_symbol"] = record.map_symbol;              // byte 81
	out["name1"] = String(record.name1.c_str());        // AI class (iai_name)
	out["name2"] = String(record.name2.c_str());        // AI script (ai_textfile)
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
	// The name->member mapping (and the seed-from-record + clamp rules) lives in libs/mission, the
	// same as the header setters; the wrapper just forwards the field name. Adding an AI/waypoint
	// field is one edit there, not four parallel ones across this file and the inspector.
	if (!document.set_entity_property_int(to_native_kind(kind), static_cast<size_t>(index),
	            property.utf8().get_data(), value)) {
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::set_entity_property_string(int kind, int index, const String &property, const String &value) {
	if (index < 0) {
		return false;
	}
	if (!document.set_entity_property_string(to_native_kind(kind), static_cast<size_t>(index),
	            property.utf8().get_data(), value.utf8().get_data())) {
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::set_header_string(const String &field, const String &value) {
	if (!document.set_header_string(field.utf8().get_data(), value.utf8().get_data())) {
		last_error = String(document.last_error().c_str());
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::set_header_int(const String &field, int value) {
	if (!document.set_header_int(field.utf8().get_data(), value)) {
		last_error = String(document.last_error().c_str());
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::set_header_flag(int bit, bool on) {
	if (!document.set_header_flag(bit, on)) {
		last_error = String(document.last_error().c_str());
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::set_header_float(const String &field, float value) {
	if (!document.set_header_float(field.utf8().get_data(), value)) {
		last_error = String(document.last_error().c_str());
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

Dictionary NovaMissionData::area_trigger_to_dictionary(const opennova::mission::AreaTriggerRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["id"] = record.wp_number;  // off-0 dword (Phase-5 UNKNOWN; carried raw)
	// Mission-space corners (16.16 already converted to float). The placement layer maps to Godot space.
	out["min"] = Vector3(record.min_x, record.min_y, record.min_z);
	out["max"] = Vector3(record.max_x, record.max_y, record.max_z);
	out["active"] = record.active;
	out["constrain_z"] = record.constrain_z;
	out["raw_flags"] = record.reserved;
	return out;
}

int NovaMissionData::get_area_trigger_count() const {
	return static_cast<int>(document.area_trigger_count());
}

Array NovaMissionData::get_area_triggers() const {
	Array out;
	for (const opennova::mission::AreaTriggerRecord &record : document.area_triggers()) {
		out.push_back(area_trigger_to_dictionary(record));
	}
	return out;
}

Dictionary NovaMissionData::get_area_trigger(int index) const {
	if (index < 0) {
		return Dictionary();
	}
	opennova::mission::AreaTriggerRecord record;
	if (!document.get_area_trigger(static_cast<size_t>(index), record)) {
		return Dictionary();
	}
	return area_trigger_to_dictionary(record);
}

// Build a typed record from Godot-side corners, normalizing min<=max per axis (the engine does not
// auto-swap area triggers, so a crossed-corner drag must be fixed here). raw_flags is composed from
// the two known bits; any other flag bits start clear for a freshly authored zone.
static opennova::mission::AreaTriggerRecord make_area_record(const Vector3 &min_bounds, const Vector3 &max_bounds,
		bool active, bool constrain_z, int zone_id) {
	opennova::mission::AreaTriggerRecord record;
	record.wp_number = zone_id;
	record.min_x = MIN(min_bounds.x, max_bounds.x);
	record.max_x = MAX(min_bounds.x, max_bounds.x);
	record.min_y = MIN(min_bounds.y, max_bounds.y);
	record.max_y = MAX(min_bounds.y, max_bounds.y);
	record.min_z = MIN(min_bounds.z, max_bounds.z);
	record.max_z = MAX(min_bounds.z, max_bounds.z);
	record.active = active;
	record.constrain_z = constrain_z;
	record.reserved = (active ? 0x1 : 0) | (constrain_z ? 0x2 : 0);
	return record;
}

Dictionary NovaMissionData::add_area_trigger(const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id) {
	opennova::mission::AreaTriggerRecord out;
	if (!document.add_area_trigger(make_area_record(min_bounds, max_bounds, active, constrain_z, zone_id), &out)) {
		return Dictionary();
	}
	modified = true;
	return area_trigger_to_dictionary(out);
}

Dictionary NovaMissionData::set_area_trigger(int index, const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id) {
	if (index < 0) {
		return Dictionary();
	}
	// Preserve unknown flag bits across an edit: seed reserved from the existing record, then overwrite
	// only the two known bits below. A fresh make_area_record would otherwise zero them.
	opennova::mission::AreaTriggerRecord record = make_area_record(min_bounds, max_bounds, active, constrain_z, zone_id);
	opennova::mission::AreaTriggerRecord existing;
	if (document.get_area_trigger(static_cast<size_t>(index), existing)) {
		record.reserved = (existing.reserved & ~0x3) | (active ? 0x1 : 0) | (constrain_z ? 0x2 : 0);
	}
	opennova::mission::AreaTriggerRecord out;
	if (!document.set_area_trigger(static_cast<size_t>(index), record, &out)) {
		return Dictionary();
	}
	modified = true;
	return area_trigger_to_dictionary(out);
}

bool NovaMissionData::remove_area_trigger(int index) {
	if (index < 0) {
		return false;
	}
	if (!document.remove_area_trigger(static_cast<size_t>(index))) {
		return false;
	}
	modified = true;
	return true;
}

Dictionary NovaMissionData::weapon_loadout_to_dictionary(const opennova::mission::WeaponLoadoutEntry &entry, int index) const {
	Dictionary out;
	out["index"] = index;
	out["name"] = String::utf8(entry.name.c_str());
	out["value1"] = String::utf8(entry.value1.c_str());
	out["value2"] = String::utf8(entry.value2.c_str());
	return out;
}

Array NovaMissionData::get_weapon_loadout() const {
	Array out;
	const std::vector<opennova::mission::WeaponLoadoutEntry> entries = document.weapon_loadout();
	for (size_t i = 0; i < entries.size(); ++i) {
		out.push_back(weapon_loadout_to_dictionary(entries[i], static_cast<int>(i)));
	}
	return out;
}

bool NovaMissionData::set_weapon_loadout(const Array &entries) {
	std::vector<opennova::mission::WeaponLoadoutEntry> records;
	records.reserve(entries.size());
	for (int i = 0; i < entries.size(); ++i) {
		const Dictionary dict = entries[i];
		opennova::mission::WeaponLoadoutEntry record;
		record.name = String(dict.get("name", "")).utf8().get_data();
		// value1/value2 default to "-1" (the shipped convention) when a caller omits them.
		record.value1 = String(dict.get("value1", "-1")).utf8().get_data();
		record.value2 = String(dict.get("value2", "-1")).utf8().get_data();
		records.push_back(std::move(record));
	}
	if (!document.set_weapon_loadout(records)) {
		last_error = String(document.last_error().c_str());
		return false;
	}
	modified = true;
	return true;
}

Dictionary NovaMissionData::group_to_dictionary(const opennova::mission::GroupFields &fields) const {
	Dictionary out;
	out["index"] = static_cast<int>(fields.index);
	out["field0"] = fields.field0;
	out["field8"] = fields.field8;
	out["field12"] = fields.field12;
	return out;
}

int NovaMissionData::get_group_count() const {
	return static_cast<int>(document.group_count());
}

Array NovaMissionData::get_groups() const {
	Array out;
	for (const opennova::mission::GroupFields &fields : document.groups()) {
		out.push_back(group_to_dictionary(fields));
	}
	return out;
}

Dictionary NovaMissionData::get_group(int index) const {
	if (index < 0) {
		return Dictionary();
	}
	opennova::mission::GroupFields fields;
	if (!document.get_group(static_cast<size_t>(index), fields)) {
		return Dictionary();
	}
	return group_to_dictionary(fields);
}

bool NovaMissionData::set_group(int index, int field0, int field8, int field12) {
	if (index < 0) {
		return false;
	}
	if (!document.set_group(static_cast<size_t>(index), field0, field8, field12)) {
		return false;
	}
	modified = true;
	return true;
}

// --- Mission scripting (events / triggers / actions, Phase 4) ----------------

Dictionary NovaMissionData::event_to_dictionary(const opennova::mission::MissionEventRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["flags"] = record.flags;
	out["trigger_index"] = record.trigger_index;
	out["action_index"] = record.action_index;
	out["trigger_count"] = record.trigger_count;
	out["action_count"] = record.action_count;
	out["reset_after"] = record.reset_after;
	out["delay"] = record.delay;
	out["unknown5"] = record.unknown5;
	out["unknown6"] = record.unknown6;
	return out;
}

Dictionary NovaMissionData::trigger_to_dictionary(const opennova::mission::MissionTriggerRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["condition_flags"] = record.condition_flags;
	out["main_type"] = record.main_type;
	out["main_type_name"] = String::utf8(record.main_type_name.c_str());
	out["sub_type"] = record.sub_type;
	out["sub_type_name"] = String::utf8(record.sub_type_name.c_str());
	out["param1"] = record.param1;
	out["param2"] = record.param2;
	out["param3"] = record.param3;
	out["param4"] = record.param4;
	out["unknown7"] = record.unknown7;
	out["negated"] = record.negated;
	out["logic_or"] = record.logic_or;
	out["logic_xor"] = record.logic_xor;
	out["logic_operator"] = String::utf8(record.logic_operator.c_str());
	return out;
}

Dictionary NovaMissionData::action_to_dictionary(const opennova::mission::MissionActionRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["action_type"] = record.action_type;
	out["action_type_name"] = String::utf8(record.action_type_name.c_str());
	out["action_sub_type"] = record.action_sub_type;
	out["action_sub_type_name"] = String::utf8(record.action_sub_type_name.c_str());
	out["param1"] = record.param1;
	out["param2"] = record.param2;
	out["param3"] = record.param3;
	out["param4"] = record.param4;
	out["reserved0"] = record.reserved0;
	out["reserved1"] = record.reserved1;
	return out;
}

Dictionary NovaMissionData::logic_reference_to_dictionary(const opennova::mission::MissionLogicReference &reference) const {
	Dictionary out;
	out["source_kind"] = String::utf8(reference.source_kind.c_str());
	out["source_index"] = reference.source_index;
	out["target_kind"] = String::utf8(reference.target_kind.c_str());
	out["target_index"] = reference.target_index;
	out["param_slot"] = reference.param_slot;
	out["raw_value"] = reference.raw_value;
	out["label"] = String::utf8(reference.label.c_str());
	out["valid"] = reference.valid;
	return out;
}

Dictionary NovaMissionData::logic_diagnostic_to_dictionary(const opennova::mission::MissionLogicDiagnostic &diagnostic) const {
	Dictionary out;
	out["severity"] = String::utf8(diagnostic.severity.c_str());
	out["code"] = String::utf8(diagnostic.code.c_str());
	out["message"] = String::utf8(diagnostic.message.c_str());
	out["subject_kind"] = String::utf8(diagnostic.subject_kind.c_str());
	out["subject_index"] = diagnostic.subject_index;
	return out;
}

Dictionary NovaMissionData::event_chain_to_dictionary(const opennova::mission::MissionEventChain &chain) const {
	Dictionary out;
	out["event"] = event_to_dictionary(chain.event);
	Array triggers;
	for (const opennova::mission::MissionTriggerRecord &trigger : chain.triggers) {
		triggers.push_back(trigger_to_dictionary(trigger));
	}
	out["triggers"] = triggers;
	Array actions;
	for (const opennova::mission::MissionActionRecord &action : chain.actions) {
		actions.push_back(action_to_dictionary(action));
	}
	out["actions"] = actions;
	Array references;
	for (const opennova::mission::MissionLogicReference &reference : chain.references) {
		references.push_back(logic_reference_to_dictionary(reference));
	}
	out["references"] = references;
	Array diagnostics;
	for (const opennova::mission::MissionLogicDiagnostic &diagnostic : chain.diagnostics) {
		diagnostics.push_back(logic_diagnostic_to_dictionary(diagnostic));
	}
	out["diagnostics"] = diagnostics;
	return out;
}

opennova::mission::MissionTriggerRecord NovaMissionData::trigger_from_dictionary(const Dictionary &dict, const opennova::mission::MissionTriggerRecord &seed) const {
	opennova::mission::MissionTriggerRecord record = seed;
	record.main_type = static_cast<int>(dict.get("main_type", seed.main_type));
	record.sub_type = static_cast<int>(dict.get("sub_type", seed.sub_type));
	record.param1 = static_cast<int>(dict.get("param1", seed.param1));
	record.param2 = static_cast<int>(dict.get("param2", seed.param2));
	record.param3 = static_cast<int>(dict.get("param3", seed.param3));
	record.param4 = static_cast<int>(dict.get("param4", seed.param4));
	// Compose condition_flags bits 0/1/2 from the editor booleans; keep the seed's unmodeled high bits so
	// an edit never drops a flag the format carries but the editor does not surface. unknown7 rides on the
	// `record = seed` copy. Defaults for omitted keys come from the condition_flags bits (the canonical
	// source: condition_flags is what trigger_from_record serializes), not the seed's mirror bool fields,
	// so an out-of-sync seed can never propagate. The bool mirrors are then re-derived to stay consistent.
	int condition = seed.condition_flags & ~0x7;
	if (static_cast<bool>(dict.get("negated", (seed.condition_flags & 0x1) != 0))) {
		condition |= 0x1;
	}
	if (static_cast<bool>(dict.get("logic_or", (seed.condition_flags & 0x2) != 0))) {
		condition |= 0x2;
	}
	if (static_cast<bool>(dict.get("logic_xor", (seed.condition_flags & 0x4) != 0))) {
		condition |= 0x4;
	}
	record.condition_flags = condition;
	record.negated = (condition & 0x1) != 0;
	record.logic_or = (condition & 0x2) != 0;
	record.logic_xor = (condition & 0x4) != 0;
	return record;
}

opennova::mission::MissionActionRecord NovaMissionData::action_from_dictionary(const Dictionary &dict, const opennova::mission::MissionActionRecord &seed) const {
	opennova::mission::MissionActionRecord record = seed;
	record.action_type = static_cast<int>(dict.get("action_type", seed.action_type));
	record.action_sub_type = static_cast<int>(dict.get("action_sub_type", seed.action_sub_type));
	record.param1 = static_cast<int>(dict.get("param1", seed.param1));
	record.param2 = static_cast<int>(dict.get("param2", seed.param2));
	record.param3 = static_cast<int>(dict.get("param3", seed.param3));
	record.param4 = static_cast<int>(dict.get("param4", seed.param4));
	return record;  // reserved0/reserved1 ride on the `record = seed` copy
}

int NovaMissionData::get_event_count() const {
	return static_cast<int>(document.event_count());
}

Array NovaMissionData::get_events() const {
	Array out;
	for (const opennova::mission::MissionEventRecord &record : document.events()) {
		out.push_back(event_to_dictionary(record));
	}
	return out;
}

Dictionary NovaMissionData::get_event(int index) const {
	if (index < 0) {
		return Dictionary();
	}
	opennova::mission::MissionEventRecord record;
	if (!document.get_event(static_cast<size_t>(index), record)) {
		return Dictionary();
	}
	return event_to_dictionary(record);
}

Dictionary NovaMissionData::get_event_chain(int index) const {
	if (index < 0) {
		return Dictionary();
	}
	opennova::mission::MissionEventChain chain;
	if (!document.get_event_chain(static_cast<size_t>(index), chain)) {
		return Dictionary();
	}
	return event_chain_to_dictionary(chain);
}

Dictionary NovaMissionData::get_logic_summary() const {
	const opennova::mission::MissionLogicSummary summary = document.logic_summary();
	Dictionary out;
	out["events"] = static_cast<int>(summary.event_count);
	out["triggers"] = static_cast<int>(summary.trigger_count);
	out["actions"] = static_cast<int>(summary.action_count);
	out["area_triggers"] = static_cast<int>(summary.area_trigger_count);
	out["diagnostics"] = static_cast<int>(summary.diagnostic_count);
	return out;
}

Dictionary NovaMissionData::add_event(int flags, int reset_after, int delay) {
	opennova::mission::MissionEventRecord seed;
	seed.flags = flags;
	seed.reset_after = reset_after;
	seed.delay = delay;
	opennova::mission::MissionEventRecord out;
	if (!document.add_event(seed, &out)) {
		return Dictionary();
	}
	modified = true;
	return event_to_dictionary(out);
}

bool NovaMissionData::remove_event(int index) {
	if (index < 0) {
		return false;
	}
	if (!document.remove_event(static_cast<size_t>(index))) {
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::set_event(int index, int flags, int reset_after, int delay) {
	if (index < 0) {
		return false;
	}
	// Seed from the existing event so the read-only structural fields (trigger/action index + count) and
	// the unknown bytes survive: set_event applies only the editable attributes below.
	opennova::mission::MissionEventRecord record;
	if (!document.get_event(static_cast<size_t>(index), record)) {
		return false;
	}
	record.flags = flags;
	record.reset_after = reset_after;
	record.delay = delay;
	if (!document.set_event(static_cast<size_t>(index), record)) {
		return false;
	}
	modified = true;
	return true;
}

Dictionary NovaMissionData::add_event_trigger(int event_index, const Dictionary &trigger) {
	if (event_index < 0) {
		return Dictionary();
	}
	opennova::mission::MissionEventRecord event;
	if (!document.get_event(static_cast<size_t>(event_index), event)) {
		return Dictionary();
	}
	// A fresh trigger defaults to a Group / Null condition (a valid, named pairing) before the dict edits.
	opennova::mission::MissionTriggerRecord seed;
	seed.main_type = static_cast<int>(opennova::bms::TriggerMainType::Group);
	const opennova::mission::MissionTriggerRecord record = trigger_from_dictionary(trigger, seed);
	opennova::mission::MissionEventChain chain;
	if (!document.insert_event_trigger(static_cast<size_t>(event_index), static_cast<size_t>(event.trigger_count), record, &chain)) {
		return Dictionary();
	}
	modified = true;
	return event_chain_to_dictionary(chain);
}

Dictionary NovaMissionData::set_event_trigger(int event_index, int local_index, const Dictionary &trigger) {
	if (event_index < 0 || local_index < 0) {
		return Dictionary();
	}
	opennova::mission::MissionEventRecord event;
	if (!document.get_event(static_cast<size_t>(event_index), event)) {
		return Dictionary();
	}
	if (local_index >= event.trigger_count) {
		return Dictionary();
	}
	const size_t global = static_cast<size_t>(event.trigger_index) + static_cast<size_t>(local_index);
	opennova::mission::MissionTriggerRecord existing;
	if (!document.get_trigger(global, existing)) {
		return Dictionary();
	}
	const opennova::mission::MissionTriggerRecord record = trigger_from_dictionary(trigger, existing);
	if (!document.set_trigger(global, record)) {
		return Dictionary();
	}
	modified = true;
	opennova::mission::MissionEventChain chain;
	document.get_event_chain(static_cast<size_t>(event_index), chain);
	return event_chain_to_dictionary(chain);
}

bool NovaMissionData::remove_event_trigger(int event_index, int local_index) {
	if (event_index < 0 || local_index < 0) {
		return false;
	}
	if (!document.remove_event_trigger(static_cast<size_t>(event_index), static_cast<size_t>(local_index))) {
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::move_event_trigger(int event_index, int local_index, int delta) {
	if (event_index < 0 || local_index < 0) {
		return false;
	}
	if (!document.move_event_trigger(static_cast<size_t>(event_index), static_cast<size_t>(local_index), delta)) {
		return false;
	}
	modified = true;
	return true;
}

Dictionary NovaMissionData::add_event_action(int event_index, const Dictionary &action) {
	if (event_index < 0) {
		return Dictionary();
	}
	opennova::mission::MissionEventRecord event;
	if (!document.get_event(static_cast<size_t>(event_index), event)) {
		return Dictionary();
	}
	opennova::mission::MissionActionRecord seed;  // defaults to a Null action
	const opennova::mission::MissionActionRecord record = action_from_dictionary(action, seed);
	opennova::mission::MissionEventChain chain;
	if (!document.insert_event_action(static_cast<size_t>(event_index), static_cast<size_t>(event.action_count), record, &chain)) {
		return Dictionary();
	}
	modified = true;
	return event_chain_to_dictionary(chain);
}

Dictionary NovaMissionData::set_event_action(int event_index, int local_index, const Dictionary &action) {
	if (event_index < 0 || local_index < 0) {
		return Dictionary();
	}
	opennova::mission::MissionEventRecord event;
	if (!document.get_event(static_cast<size_t>(event_index), event)) {
		return Dictionary();
	}
	if (local_index >= event.action_count) {
		return Dictionary();
	}
	const size_t global = static_cast<size_t>(event.action_index) + static_cast<size_t>(local_index);
	opennova::mission::MissionActionRecord existing;
	if (!document.get_action(global, existing)) {
		return Dictionary();
	}
	const opennova::mission::MissionActionRecord record = action_from_dictionary(action, existing);
	if (!document.set_action(global, record)) {
		return Dictionary();
	}
	modified = true;
	opennova::mission::MissionEventChain chain;
	document.get_event_chain(static_cast<size_t>(event_index), chain);
	return event_chain_to_dictionary(chain);
}

bool NovaMissionData::remove_event_action(int event_index, int local_index) {
	if (event_index < 0 || local_index < 0) {
		return false;
	}
	if (!document.remove_event_action(static_cast<size_t>(event_index), static_cast<size_t>(local_index))) {
		return false;
	}
	modified = true;
	return true;
}

bool NovaMissionData::move_event_action(int event_index, int local_index, int delta) {
	if (event_index < 0 || local_index < 0) {
		return false;
	}
	if (!document.move_event_action(static_cast<size_t>(event_index), static_cast<size_t>(local_index), delta)) {
		return false;
	}
	modified = true;
	return true;
}

namespace {

Array enum_entries_to_array(const std::vector<opennova::mission::MissionEnumEntry> &entries) {
	Array out;
	for (const opennova::mission::MissionEnumEntry &entry : entries) {
		Dictionary dict;
		dict["value"] = entry.value;
		dict["name"] = String::utf8(entry.name.c_str());
		out.push_back(dict);
	}
	return out;
}

} // namespace

Array NovaMissionData::get_trigger_main_types() const {
	return enum_entries_to_array(document.trigger_main_types());
}

Array NovaMissionData::get_trigger_sub_types(int main_type) const {
	return enum_entries_to_array(document.trigger_sub_types(main_type));
}

Array NovaMissionData::get_action_types() const {
	return enum_entries_to_array(document.action_types());
}

Array NovaMissionData::get_action_sub_types(int action_type) const {
	return enum_entries_to_array(document.action_sub_types(action_type));
}

Array NovaMissionData::get_event_flag_bits() const {
	return enum_entries_to_array(document.event_flag_bits());
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

void NovaMissionData::begin_edit() {
	// Open an edit session, snapshotting the pre-edit document. The shared history
	// coalesces a run of edits into one step (begin is inert while a session is open);
	// we add only the "is a document loaded?" guard it cannot know about.
	if (!document.is_loaded()) {
		return;
	}
	history.begin(document.bms_file());
}

void NovaMissionData::commit_edit() {
	// Close the session, recording one undo step only if the document actually changed:
	// the history's equal-gate uses bms::equal, so a plain click / same-value edit /
	// programmatic refresh records nothing.
	history.commit(document.bms_file());
}

bool NovaMissionData::can_undo() const {
	return history.can_undo();
}

bool NovaMissionData::can_redo() const {
	return history.can_redo();
}

bool NovaMissionData::undo() {
	// O(1) in-memory swap of the live file with the top undo step: no serialize / parse,
	// so it cannot fail once a document is loaded.
	if (!document.is_loaded()) {
		return false;
	}
	return history.swap_undo(document.bms_file());
}

bool NovaMissionData::redo() {
	if (!document.is_loaded()) {
		return false;
	}
	return history.swap_redo(document.bms_file());
}

int NovaMissionData::undo_depth() const {
	return static_cast<int>(history.undo_depth());
}

void NovaMissionData::clear_history() {
	history.clear();
}

bool NovaMissionData::is_dirty() const {
	// Exact: the document differs from the clean baseline (set at open / save / new).
	// Before a baseline exists, fall back to the coarse "any mutation since load" flag.
	return history.is_dirty(document.bms_file(), modified);
}

void NovaMissionData::mark_clean() {
	history.mark_clean(document.bms_file());
}

int64_t NovaMissionData::object_records_revision() const {
	// 64-bit FNV-1a over the raw bytes of the placed-object record vectors. This mirrors
	// how bms::equal decides these vectors (memcmp via pod_vectors_equal), so two
	// documents with byte-identical object records share a revision and any change moves
	// it -- far cheaper than marshalling ~every entity into a Dictionary to hash it.
	const opennova::bms::File &file = document.bms_file();
	uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis
	const auto mix = [&h](const void *data, size_t size) {
		const unsigned char *p = static_cast<const unsigned char *>(data);
		for (size_t i = 0; i < size; ++i) {
			h ^= p[i];
			h *= 1099511628211ull; // FNV-1a 64-bit prime
		}
	};
	const auto mix_entities = [&](const std::vector<opennova::bms::Entity> &v) {
		const uint64_t count = v.size();
		mix(&count, sizeof(count)); // a count change moves the revision even at a byte realignment
		if (!v.empty()) {
			mix(v.data(), v.size() * sizeof(opennova::bms::Entity));
		}
	};
	mix_entities(file.items);
	mix_entities(file.buildings);
	mix_entities(file.markers);
	mix_entities(file.organics);
	return static_cast<int64_t>(h);
}

Dictionary NovaMissionData::structure_fingerprint() const {
	Dictionary out;
	out["events"] = get_event_count();
	out["zones"] = get_area_trigger_count();
	out["object_rev"] = object_records_revision();
	return out;
}

// Game mode is a single-select among the 11 attrib_flags mode bits. [orig: sub_402770, dfx2med.exe.
// Decode @0x4050c7 tests the bits in the priority order below and selects the matching combobox item;
// encode @0x4031cd clears them with `and 0x7CFFFF` (== ~ATTRIB_GAME_MODE_MASK) then OR's exactly one.]
int64_t NovaMissionData::get_game_mode() const {
	if (!document.is_loaded()) {
		return 0;
	}
	const uint32_t flags = static_cast<uint32_t>(document.bms_file().header.attrib_flags);
	// Engine decode priority (uint32 literals to avoid a narrowing conversion from the unnamed enum;
	// values mirror the ATTRIB_* constants named in the comments).
	const uint32_t priority[] = {
		0x1000000u,  // ATTRIB_COOP
		0x2000000u,  // ATTRIB_DEATHMATCH
		0x20000000u, // ATTRIB_TEAM_DEATHMATCH
		0x4000000u,  // ATTRIB_KING_OF_THE_HILL
		0x40000000u, // ATTRIB_TEAM_KING_OF_THE_HILL
		0x10000000u, // ATTRIB_CAPTURE_THE_FLAG
		0x800000u,   // ATTRIB_ATTACK_AND_DEFEND
		0x80000000u, // ATTRIB_SEARCH_AND_DESTROY
		0x8000000u,  // ATTRIB_FLAGBALL
		0x10000u,    // ATTRIB_ADVANCE_AND_SECURE
		0x20000u,    // ATTRIB_CONQUER_AND_CONTROL
	};
	for (uint32_t bit : priority) {
		if (flags & bit) {
			return static_cast<int64_t>(bit);
		}
	}
	return 0; // no mode bit set -> Single Player
}

bool NovaMissionData::set_game_mode(int64_t bit) {
	if (!document.is_loaded()) {
		return false;
	}
	const uint32_t b = static_cast<uint32_t>(static_cast<uint64_t>(bit));
	const uint32_t mask = static_cast<uint32_t>(ATTRIB_GAME_MODE_MASK);
	// Valid iff 0 (Single Player) or exactly one of the 11 mode bits.
	if (b != 0 && ((b & ~mask) != 0 || (b & (b - 1)) != 0)) {
		return false;
	}
	auto &field = document.bms_file().header.attrib_flags;
	const uint32_t cur = static_cast<uint32_t>(field);
	field = static_cast<opennova::bms::AttribFlags>((cur & ~mask) | b);
	modified = true;
	return true;
}
