#include "nova_mission_data.h"

#include <godot_cpp/variant/vector3.hpp>

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
	ClassDB::bind_method(D_METHOD("get_all_entities"), &NovaMissionData::get_all_entities);

	BIND_CONSTANT(KIND_MARKER);
	BIND_CONSTANT(KIND_ITEM);
	BIND_CONSTANT(KIND_BUILDING);
	BIND_CONSTANT(KIND_ORGANIC);
}

Error NovaMissionData::open_file(const String &path) {
	source_path = path;
	last_error = String();
	if (!document.load_bms_file(path.utf8().get_data())) {
		last_error = String(document.last_error().c_str());
		return ERR_CANT_OPEN;
	}
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
