#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/menu/player_info_loadout.h>

namespace godot {

class MenuDriver;
class PlayerInfoKitRestore;
class RtxtStringFile;
class WeaponDatabase;

// The PLAYER_INFO loadout panel's state over the engine's
// menu::PlayerInfoLoadout (runtime/menu/player_info_loadout.h carries the
// witnesses): the category lists and grenade positions as filled, the saved
// count pairs, the type bytes per team, the restored grenade slots and the
// class page. PlayerInfoMenuCompanion holds one for the session and routes
// the screen's fills and handler picks through it over its MenuDriver.
class PlayerInfoLoadout : public RefCounted {
	GDCLASS(PlayerInfoLoadout, RefCounted)

	opennova::menu::PlayerInfoLoadout value_;

protected:
	static void _bind_methods();

public:
	void clear_counts() { value_.clear_counts(); }
	void fill_slots(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
			int p_class_mask, int p_team_mask, const String &p_none_label,
			const Ref<RtxtStringFile> &p_gametext);
	// The page's class becomes page_class.
	void restore(const Ref<MenuDriver> &p_driver, const Ref<PlayerInfoKitRestore> &p_page, int p_team);
	int selected_weapon(const Ref<MenuDriver> &p_driver, const String &p_control) const;
	void fill_ammo(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
			const String &p_control, int p_team, const Ref<RtxtStringFile> &p_gametext);
	void fill_grenades(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
			int p_class_mask, int p_team_mask, const Ref<RtxtStringFile> &p_gametext);
	void pick_clips(const Ref<MenuDriver> &p_driver, const String &p_control, bool p_secondary, int p_row);
	void pick_ammo_type(const Ref<MenuDriver> &p_driver, const String &p_control, int p_team, int p_row);
	void pick_grenade(int p_position, int p_row) { value_.pick_grenade(p_position, p_row); }
	int selected_clips(const Ref<MenuDriver> &p_driver, const String &p_control) const;
	int selected_ammo_type(const String &p_control, int p_team) const;
	double weight(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons) const;
	// The kit page in retail's order, one Dictionary per KitEntry (name,
	// ammo_primary, ammo_secondary, flags; WeaponDatabase.kit_entry_rows).
	Array kit_entries(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons, int p_team,
			int p_player_class) const;
	int get_page_class() const { return value_.page_class; }
};

} // namespace godot
