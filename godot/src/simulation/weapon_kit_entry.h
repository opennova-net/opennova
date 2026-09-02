#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/world/weapon_inventory.h>

namespace godot {

// One canonical, unexpanded loadout tuple (world::WeaponKitEntry): the
// weapon.def name and the -1-defaulted ammo/flags words of the .bms spawn kit,
// the profile page and the armory ACCEPT buffer. Authorable: a test or a shell
// producer builds a kit from these (`make`), the sim reports its spawn kit as
// them (Simulation.get_local_player_loadout).
class WeaponKitEntry : public RefCounted {
	GDCLASS(WeaponKitEntry, RefCounted)

	opennova::world::WeaponKitEntry value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::WeaponKitEntry &p_value) { value_ = p_value; }
	const opennova::world::WeaponKitEntry &value() const { return value_; }

	static Ref<WeaponKitEntry> make(const String &p_name, int p_ammo_primary = -1,
			int p_ammo_secondary = -1, int p_flags = -1);

	String get_name() const;
	void set_name(const String &p_name);
	int get_ammo_primary() const { return value_.ammo_primary; }
	void set_ammo_primary(int p_value) { value_.ammo_primary = p_value; }
	int get_ammo_secondary() const { return value_.ammo_secondary; }
	void set_ammo_secondary(int p_value) { value_.ammo_secondary = p_value; }
	int get_flags() const { return value_.flags; }
	void set_flags(int p_value) { value_.flags = p_value; }
};

} // namespace godot
