#include "mnu/armory_loadout_pick.h"

#include <godot_cpp/variant/dictionary.hpp>

using namespace godot;

ArmoryLoadoutPick::ArmoryLoadoutPick() {
	assign(opennova::menu::ArmoryLoadoutPick());
}

void ArmoryLoadoutPick::assign(const opennova::menu::ArmoryLoadoutPick &p_value) {
	names_.clear();
	clips_.clear();
	grenades_.clear();
	for (int i = 0; i < 3; ++i) {
		// A weapon.def name reads as the WeaponDef record's (Latin-1 bytes).
		names_.push_back(String(p_value.names[i].c_str()));
		clips_.push_back(p_value.clips[i]);
	}
	for (const opennova::playersav::KitEntry &entry : p_value.grenades) {
		Dictionary row;
		row["name"] = String(entry.name.c_str());
		row["ammo_primary"] = entry.ammo_primary;
		row["ammo_secondary"] = entry.ammo_secondary;
		row["flags"] = entry.flags;
		grenades_.push_back(row);
	}
}

void ArmoryLoadoutPick::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_names"), &ArmoryLoadoutPick::get_names);
	ClassDB::bind_method(D_METHOD("get_clips"), &ArmoryLoadoutPick::get_clips);
	ClassDB::bind_method(D_METHOD("get_grenades"), &ArmoryLoadoutPick::get_grenades);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "names"), "", "get_names");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "clips"), "", "get_clips");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "grenades"), "", "get_grenades");
}
