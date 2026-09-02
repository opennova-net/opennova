#include "simulation/deploy_rows.h"

using namespace godot;

void DeployOccupantRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_handle"), &DeployOccupantRow::get_handle);
	ClassDB::bind_method(D_METHOD("set_handle", "handle"), &DeployOccupantRow::set_handle);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "handle"), "set_handle", "get_handle");
	ClassDB::bind_method(D_METHOD("get_name"), &DeployOccupantRow::get_name);
	ClassDB::bind_method(D_METHOD("set_name", "name"), &DeployOccupantRow::set_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
	ClassDB::bind_method(D_METHOD("is_self"), &DeployOccupantRow::is_self);
	ClassDB::bind_method(D_METHOD("set_self", "self"), &DeployOccupantRow::set_self);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "self"), "set_self", "is_self");
}

void DeployZoneRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_param"), &DeployZoneRow::get_param);
	ClassDB::bind_method(D_METHOD("set_param", "param"), &DeployZoneRow::set_param);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "param"), "set_param", "get_param");
	ClassDB::bind_method(D_METHOD("get_letter"), &DeployZoneRow::get_letter);
	ClassDB::bind_method(D_METHOD("set_letter", "letter"), &DeployZoneRow::set_letter);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "letter"), "set_letter", "get_letter");
	ClassDB::bind_method(D_METHOD("get_name_key"), &DeployZoneRow::get_name_key);
	ClassDB::bind_method(D_METHOD("set_name_key", "key"), &DeployZoneRow::set_name_key);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name_key"), "set_name_key", "get_name_key");
	ClassDB::bind_method(D_METHOD("is_secured"), &DeployZoneRow::is_secured);
	ClassDB::bind_method(D_METHOD("set_secured", "secured"), &DeployZoneRow::set_secured);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "secured"), "set_secured", "is_secured");
	ClassDB::bind_method(D_METHOD("get_wave_countdown"), &DeployZoneRow::get_wave_countdown);
	ClassDB::bind_method(D_METHOD("set_wave_countdown", "ticks"),
			&DeployZoneRow::set_wave_countdown);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "wave_countdown"), "set_wave_countdown",
			"get_wave_countdown");
	ClassDB::bind_method(D_METHOD("get_occupants"), &DeployZoneRow::get_occupants);
	ClassDB::bind_method(D_METHOD("set_occupants", "rows"), &DeployZoneRow::set_occupants);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "occupants", PROPERTY_HINT_ARRAY_TYPE,
						 "DeployOccupantRow"),
			"set_occupants", "get_occupants");
}

void DeployListRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_text"), &DeployListRow::get_text);
	ClassDB::bind_method(D_METHOD("set_text", "text"), &DeployListRow::set_text);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "text"), "set_text", "get_text");
	ClassDB::bind_method(D_METHOD("get_value"), &DeployListRow::get_value);
	ClassDB::bind_method(D_METHOD("set_value", "value"), &DeployListRow::set_value);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "value"), "set_value", "get_value");
}
