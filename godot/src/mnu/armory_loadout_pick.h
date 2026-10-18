#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <runtime/menu/loadout_screen.h>

namespace godot {

// The WEAPON screen's selection at its ACCEPT, read-only (MenuDriver.armory_loadout_pick;
// engine menu::armory_loadout_pick, runtime/menu/loadout_screen.h carries the
// witness): each category's weapon name ("" for NONE) and clip count (-1 the
// def default) in PRIMARY, SECONDARY, ACCESSORY order, and the grenade entries
// (one Dictionary per KitEntry: name, ammo_primary, ammo_secondary, flags).
class ArmoryLoadoutPick : public RefCounted {
	GDCLASS(ArmoryLoadoutPick, RefCounted)

	PackedStringArray names_;
	PackedInt32Array clips_;
	Array grenades_;

protected:
	static void _bind_methods();

public:
	ArmoryLoadoutPick();
	void assign(const opennova::menu::ArmoryLoadoutPick &p_value);

	PackedStringArray get_names() const { return names_; }
	PackedInt32Array get_clips() const { return clips_; }
	Array get_grenades() const { return grenades_; }
};

} // namespace godot
