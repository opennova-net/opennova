#include "simulation/entity_row.h"

#include "simulation/simulation_internal.h" // godot_from_mission_vec3, the ONE axis map

namespace godot {

String EntityRow::get_name() const {
	return String(value_.name.c_str());
}

String EntityRow::get_item_name() const {
	return String(value_.item_name.c_str());
}

String EntityRow::get_state_name() const {
	return String(value_.state_name.c_str());
}

Vector3 EntityRow::get_world_position() const {
	return sim_internal::godot_from_mission_vec3(value_.mission_position);
}

Vector3 EntityRow::get_mission_position() const {
	return Vector3(value_.mission_position.x, value_.mission_position.y,
			value_.mission_position.z);
}

Dictionary EntityRow::to_json_value() const {
	Dictionary out;
	out["index"] = value_.index;
	// The legacy view_index was the client-present row index; presented rows
	// ran the list in present order so it equaled the directory index, and
	// appended AI diagnostics carried -1.
	out["view_index"] = value_.presented ? value_.index : -1;
	out["ai_index"] = value_.ai_index;
	out["editable"] = value_.editable;
	out["presented"] = value_.presented;
	out["registry_present"] = value_.registry_present;
	out["kind"] = value_.kind;
	out["source_index"] = value_.source_index;
	out["bms_id"] = value_.bms_id;
	out["net_id"] = value_.net_id;
	out["type_id"] = value_.item_id;
	out["wire_handle"] = static_cast<int>(value_.wire_handle);
	out["name"] = get_name();
	out["item_name"] = get_item_name();
	out["state"] = get_state_name();
	out["health"] = value_.health;
	out["team"] = value_.team;
	out["alive"] = value_.alive;
	out["hidden"] = value_.hidden;
	out["world_position"] = get_world_position();
	out["mission_position"] = get_mission_position();
	return out;
}

void EntityRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_index"), &EntityRow::get_index);
	ClassDB::bind_method(D_METHOD("get_ai_index"), &EntityRow::get_ai_index);
	ClassDB::bind_method(D_METHOD("is_editable"), &EntityRow::is_editable);
	ClassDB::bind_method(D_METHOD("get_kind"), &EntityRow::get_kind);
	ClassDB::bind_method(D_METHOD("get_source_index"), &EntityRow::get_source_index);
	ClassDB::bind_method(D_METHOD("get_bms_id"), &EntityRow::get_bms_id);
	ClassDB::bind_method(D_METHOD("get_net_id"), &EntityRow::get_net_id);
	ClassDB::bind_method(D_METHOD("get_wire_handle"), &EntityRow::get_wire_handle);
	ClassDB::bind_method(D_METHOD("get_name"), &EntityRow::get_name);
	ClassDB::bind_method(D_METHOD("get_item_name"), &EntityRow::get_item_name);
	ClassDB::bind_method(D_METHOD("get_state_name"), &EntityRow::get_state_name);
	ClassDB::bind_method(D_METHOD("get_health"), &EntityRow::get_health);
	ClassDB::bind_method(D_METHOD("get_team"), &EntityRow::get_team);
	ClassDB::bind_method(D_METHOD("is_alive"), &EntityRow::is_alive);
	ClassDB::bind_method(D_METHOD("is_hidden"), &EntityRow::is_hidden);
	ClassDB::bind_method(D_METHOD("get_mission_position"), &EntityRow::get_mission_position);
	ClassDB::bind_method(D_METHOD("get_world_position"), &EntityRow::get_world_position);
	ClassDB::bind_method(D_METHOD("to_json_value"), &EntityRow::to_json_value);
}

} // namespace godot
