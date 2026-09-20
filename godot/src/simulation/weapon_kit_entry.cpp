#include "simulation/weapon_kit_entry.h"
#include "util/string_convert.h"

using namespace godot;

Ref<WeaponKitEntry> WeaponKitEntry::make(const String &p_name, int p_ammo_primary,
		int p_ammo_secondary, int p_flags) {
	Ref<WeaponKitEntry> out;
	out.instantiate();
	out->set_name(p_name);
	out->set_ammo_primary(p_ammo_primary);
	out->set_ammo_secondary(p_ammo_secondary);
	out->set_flags(p_flags);
	return out;
}

String WeaponKitEntry::get_name() const { return opennova::to_gd(value_.name); }

void WeaponKitEntry::set_name(const String &p_name) {
	value_.name = opennova::to_std(p_name);
}

void WeaponKitEntry::_bind_methods() {
	ClassDB::bind_static_method("WeaponKitEntry",
			D_METHOD("make", "name", "ammo_primary", "ammo_secondary", "flags"),
			&WeaponKitEntry::make, DEFVAL(-1), DEFVAL(-1), DEFVAL(-1));
#define WEAPON_KIT_ENTRY_PROPERTY(m_variant, m_name)                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &WeaponKitEntry::get_##m_name);               \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &WeaponKitEntry::set_##m_name);      \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name), "set_" #m_name, "get_" #m_name);
	WEAPON_KIT_ENTRY_PROPERTY(Variant::STRING, name)
	WEAPON_KIT_ENTRY_PROPERTY(Variant::INT, ammo_primary)
	WEAPON_KIT_ENTRY_PROPERTY(Variant::INT, ammo_secondary)
	WEAPON_KIT_ENTRY_PROPERTY(Variant::INT, flags)
#undef WEAPON_KIT_ENTRY_PROPERTY
}
