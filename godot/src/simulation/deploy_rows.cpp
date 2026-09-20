#include "simulation/deploy_rows.h"

#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

String DeployZoneRow::get_name_key() const {
	return opennova::to_gd(value_.name_key);
}

void DeployZoneRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::INT, param)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::STRING, letter)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::STRING, name_key)
	OPENNOVA_RECORD_READ_ONLY_IS(DeployZoneRow, secured)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::INT, wave_countdown)
	OPENNOVA_RECORD_READ_ONLY(DeployZoneRow, Variant::INT, occupant_count)
}

String DeployListRow::get_text() const {
	return opennova::to_gd(value_.text);
}

void DeployListRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DeployListRow, Variant::STRING, text)
	OPENNOVA_RECORD_READ_ONLY(DeployListRow, Variant::INT, value)
}
