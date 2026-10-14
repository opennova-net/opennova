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

// A row's text is the game's code-page bytes: the builder encodes the
// embedder's UTF-8 names into a zone or Default row, and a queued member's row
// carries the member's roster name as the roster holds it. The
// list's items take the rows' bytes from the records themselves
// (MenuDriver::set_deploy_list_items -> world::deploy_list_items, where the
// witness lives); this getter is the script's view of the row, read as cp1252
// (the roster's names are the page's bytes, D-NET-661), where a UTF-8 parse
// made a lone high byte U+FFFD (D-MNU-46).
String DeployListRow::get_text() const {
	return opennova::cp1252_to_gd(value_.text);
}

void DeployListRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DeployListRow, Variant::STRING, text)
	OPENNOVA_RECORD_READ_ONLY(DeployListRow, Variant::INT, value)
}
