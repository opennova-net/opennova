#include "simulation/model_list_node_row.h"

#include "util/record_bind.h"

using namespace godot;

String ModelListNodeRow::get_name() const { return String(value_.name.c_str()); }

Dictionary ModelListNodeRow::to_json_value() const {
	Dictionary out;
	out["name"] = get_name();
	out["foliage"] = get_foliage();
	out["load_index"] = get_load_index();
	out["page_index"] = get_page_index();
	return out;
}

void ModelListNodeRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(ModelListNodeRow, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(ModelListNodeRow, Variant::BOOL, foliage)
	OPENNOVA_RECORD_READ_ONLY(ModelListNodeRow, Variant::INT, load_index)
	OPENNOVA_RECORD_READ_ONLY(ModelListNodeRow, Variant::INT, page_index)
	ClassDB::bind_method(D_METHOD("to_json_value"), &ModelListNodeRow::to_json_value);
}
