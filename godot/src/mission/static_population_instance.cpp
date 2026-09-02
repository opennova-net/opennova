#include "mission/static_population_instance.h"

using namespace godot;

void StaticPopulationInstance::_bind_methods() {
	BIND_ENUM_CONSTANT(POPULATION_GLOBAL);
	BIND_ENUM_CONSTANT(POPULATION_BIN);
	ClassDB::bind_method(D_METHOD("set_population_kind", "kind"),
			&StaticPopulationInstance::set_population_kind);
	ClassDB::bind_method(D_METHOD("get_population_kind"),
			&StaticPopulationInstance::get_population_kind);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "population_kind", PROPERTY_HINT_ENUM,
						 "Global,Bin"),
			"set_population_kind", "get_population_kind");
	ClassDB::bind_method(D_METHOD("set_lod_index", "index"),
			&StaticPopulationInstance::set_lod_index);
	ClassDB::bind_method(D_METHOD("get_lod_index"),
			&StaticPopulationInstance::get_lod_index);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_index"), "set_lod_index",
			"get_lod_index");
	ClassDB::bind_method(D_METHOD("set_bin_x", "x"), &StaticPopulationInstance::set_bin_x);
	ClassDB::bind_method(D_METHOD("get_bin_x"), &StaticPopulationInstance::get_bin_x);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bin_x"), "set_bin_x", "get_bin_x");
	ClassDB::bind_method(D_METHOD("set_bin_z", "z"), &StaticPopulationInstance::set_bin_z);
	ClassDB::bind_method(D_METHOD("get_bin_z"), &StaticPopulationInstance::get_bin_z);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bin_z"), "set_bin_z", "get_bin_z");
	ClassDB::bind_method(D_METHOD("set_shadow_tagged", "tagged"),
			&StaticPopulationInstance::set_shadow_tagged);
	ClassDB::bind_method(D_METHOD("is_shadow_tagged"),
			&StaticPopulationInstance::is_shadow_tagged);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shadow_tagged"), "set_shadow_tagged",
			"is_shadow_tagged");
	ClassDB::bind_method(D_METHOD("set_graphic", "graphic"),
			&StaticPopulationInstance::set_graphic);
	ClassDB::bind_method(D_METHOD("get_graphic"), &StaticPopulationInstance::get_graphic);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "graphic"), "set_graphic", "get_graphic");
	ClassDB::bind_method(D_METHOD("set_slot_bms_ids", "ids"),
			&StaticPopulationInstance::set_slot_bms_ids);
	ClassDB::bind_method(D_METHOD("get_slot_bms_ids"),
			&StaticPopulationInstance::get_slot_bms_ids);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "slot_bms_ids"),
			"set_slot_bms_ids", "get_slot_bms_ids");
	ClassDB::bind_method(D_METHOD("set_slot_item_ids", "ids"),
			&StaticPopulationInstance::set_slot_item_ids);
	ClassDB::bind_method(D_METHOD("get_slot_item_ids"),
			&StaticPopulationInstance::get_slot_item_ids);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "slot_item_ids"),
			"set_slot_item_ids", "get_slot_item_ids");
	ClassDB::bind_method(D_METHOD("set_slot_attrib2", "values"),
			&StaticPopulationInstance::set_slot_attrib2);
	ClassDB::bind_method(D_METHOD("get_slot_attrib2"),
			&StaticPopulationInstance::get_slot_attrib2);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT64_ARRAY, "slot_attrib2"),
			"set_slot_attrib2", "get_slot_attrib2");
	ClassDB::bind_method(D_METHOD("set_slot_casts_shadow", "casts"),
			&StaticPopulationInstance::set_slot_casts_shadow);
	ClassDB::bind_method(D_METHOD("get_slot_casts_shadow"),
			&StaticPopulationInstance::get_slot_casts_shadow);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "slot_casts_shadow"),
			"set_slot_casts_shadow", "get_slot_casts_shadow");
	ClassDB::bind_method(D_METHOD("set_row_slots", "rows"),
			&StaticPopulationInstance::set_row_slots);
	ClassDB::bind_method(D_METHOD("get_row_slots"),
			&StaticPopulationInstance::get_row_slots);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "row_slots"),
			"set_row_slots", "get_row_slots");
}
