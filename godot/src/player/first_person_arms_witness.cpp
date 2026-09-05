#include "player/first_person_arms_witness.h"

using namespace godot;

void FirstPersonArmsWitness::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_character_id"), &FirstPersonArmsWitness::get_character_id);
	ClassDB::bind_method(D_METHOD("set_character_id", "value"),
			&FirstPersonArmsWitness::set_character_id);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "character_id"), "set_character_id", "get_character_id");
	ClassDB::bind_method(D_METHOD("get_arms_graphic"), &FirstPersonArmsWitness::get_arms_graphic);
	ClassDB::bind_method(D_METHOD("set_arms_graphic", "value"),
			&FirstPersonArmsWitness::set_arms_graphic);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "arms_graphic"), "set_arms_graphic", "get_arms_graphic");
	ClassDB::bind_method(D_METHOD("get_arms_camo"), &FirstPersonArmsWitness::get_arms_camo);
	ClassDB::bind_method(D_METHOD("set_arms_camo", "value"), &FirstPersonArmsWitness::set_arms_camo);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "arms_camo"), "set_arms_camo",
			"get_arms_camo");
	ClassDB::bind_method(D_METHOD("get_error"), &FirstPersonArmsWitness::get_error);
	ClassDB::bind_method(D_METHOD("set_error", "value"), &FirstPersonArmsWitness::set_error);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error"), "set_error", "get_error");
	ClassDB::bind_method(D_METHOD("is_valid"), &FirstPersonArmsWitness::is_valid);
}
