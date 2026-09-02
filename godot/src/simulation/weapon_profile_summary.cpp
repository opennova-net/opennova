#include "simulation/weapon_profile_summary.h"

using namespace godot;

void WeaponProfileSide::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_player_class"), &WeaponProfileSide::get_player_class);
	ClassDB::bind_method(D_METHOD("set_player_class", "value"),
			&WeaponProfileSide::set_player_class);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "player_class"), "set_player_class",
			"get_player_class");
	ClassDB::bind_method(D_METHOD("get_avatar_a"), &WeaponProfileSide::get_avatar_a);
	ClassDB::bind_method(D_METHOD("set_avatar_a", "value"), &WeaponProfileSide::set_avatar_a);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "avatar_a"), "set_avatar_a", "get_avatar_a");
	ClassDB::bind_method(D_METHOD("get_avatar_b"), &WeaponProfileSide::get_avatar_b);
	ClassDB::bind_method(D_METHOD("set_avatar_b", "value"), &WeaponProfileSide::set_avatar_b);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "avatar_b"), "set_avatar_b", "get_avatar_b");
	ClassDB::bind_method(D_METHOD("get_avatar_packed"), &WeaponProfileSide::get_avatar_packed);
	ClassDB::bind_method(D_METHOD("set_avatar_packed", "value"),
			&WeaponProfileSide::set_avatar_packed);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "avatar_packed"), "set_avatar_packed",
			"get_avatar_packed");
	ClassDB::bind_method(D_METHOD("get_kit"), &WeaponProfileSide::get_kit);
	ClassDB::bind_method(D_METHOD("set_kit", "names"), &WeaponProfileSide::set_kit);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "kit"), "set_kit", "get_kit");
}

void WeaponProfileSummary::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_error"), &WeaponProfileSummary::get_error);
	ClassDB::bind_method(D_METHOD("set_error", "error"), &WeaponProfileSummary::set_error);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "error"), "set_error", "get_error");
	ClassDB::bind_method(D_METHOD("is_loaded"), &WeaponProfileSummary::is_loaded);
	ClassDB::bind_method(D_METHOD("set_loaded", "loaded"), &WeaponProfileSummary::set_loaded);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loaded"), "set_loaded", "is_loaded");
	ClassDB::bind_method(D_METHOD("get_blue"), &WeaponProfileSummary::get_blue);
	ClassDB::bind_method(D_METHOD("set_blue", "side"), &WeaponProfileSummary::set_blue);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "blue", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT, "WeaponProfileSide"),
			"set_blue", "get_blue");
	ClassDB::bind_method(D_METHOD("get_red"), &WeaponProfileSummary::get_red);
	ClassDB::bind_method(D_METHOD("set_red", "side"), &WeaponProfileSummary::set_red);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "red", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT, "WeaponProfileSide"),
			"set_red", "get_red");
}
