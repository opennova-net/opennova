#include "simulation/deploy_rows.h"

#include "util/record_bind.h"

using namespace godot;

String DeployZoneRow::get_name_key() const {
	return String::utf8(value_.name_key.c_str());
}

int DeployZoneRow::get_occupant_handle(int p_index) const {
	if (p_index < 0 || p_index >= static_cast<int>(value_.occupants.size())) return 0xFFFF;
	return static_cast<int>(value_.occupants[static_cast<size_t>(p_index)].handle);
}

String DeployZoneRow::get_occupant_name(int p_index) const {
	if (p_index < 0 || p_index >= static_cast<int>(value_.occupants.size())) return String();
	return String::utf8(value_.occupants[static_cast<size_t>(p_index)].name.c_str());
}

bool DeployZoneRow::is_occupant_self(int p_index) const {
	if (p_index < 0 || p_index >= static_cast<int>(value_.occupants.size())) return false;
	return value_.occupants[static_cast<size_t>(p_index)].self;
}

void DeployZoneRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::INT, param)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::STRING, letter)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::STRING, name_key)
	OPENNOVA_RECORD_READ_ONLY_IS(DeployZoneRow, secured)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::INT, wave_countdown)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::INT, occupant_count)
	ClassDB::bind_method(D_METHOD("get_occupant_handle", "index"), &DeployZoneRow::get_occupant_handle);
	ClassDB::bind_method(D_METHOD("get_occupant_name", "index"), &DeployZoneRow::get_occupant_name);
	ClassDB::bind_method(D_METHOD("is_occupant_self", "index"), &DeployZoneRow::is_occupant_self);
}

String DeployListRow::get_text() const {
	return String::utf8(value_.text.c_str());
}

void DeployListRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DeployListRow, Variant::STRING, text)
	OPENNOVA_RECORD_READ_ONLY(DeployListRow, Variant::INT, value)
}
