#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/menu/armory_loadout.h>

namespace godot {

class MenuDriver;
class PlayerProfiles;
class ResourceRoot;
class RtxtStringFile;
class WeaponDatabase;
class WeaponKitEntry;

// The WEAPON screen's loadout state over the engine's menu::ArmoryLoadout
// (runtime/menu/armory_loadout.h and loadout_screen.h carry the witnesses, D-MNU-9): the
// per-class kit pages and the screen's counts and type bytes, which outlive the screen and
// the mission, and the category lists and grenade positions as the last populate left
// them. ArmoryPresenter holds one for the process through its companion; the stages take
// the current weapon.sav record of a PlayerProfiles (none: an empty record) and the spawn
// buffer (the sim's resident kit); the screen's fills, handler writes, weight and ACCEPT
// run over its MenuDriver.
class ArmoryLoadout : public RefCounted {
	GDCLASS(ArmoryLoadout, RefCounted)

	opennova::menu::ArmoryLoadout value_;

protected:
	static void _bind_methods();

public:
	void stage(const Ref<PlayerProfiles> &p_profiles, int p_entity_team,
			const TypedArray<WeaponKitEntry> &p_spawn);
	bool stage_on_show(const Ref<PlayerProfiles> &p_profiles, int p_entity_team,
			const TypedArray<WeaponKitEntry> &p_spawn);
	// `p_availability` (weapon name -> value, a 0 drops the weapon) when valid.
	void populate(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
			int p_entity_team, int p_selected_class, const Callable &p_availability,
			const String &p_none_label, const Ref<RtxtStringFile> &p_gametext);
	void flip_class(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
			int p_entity_team, int p_from_class, int p_to_class, const Callable &p_availability,
			const String &p_none_label, const Ref<RtxtStringFile> &p_gametext);
	void pick_weapon(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
			const String &p_control, const Ref<RtxtStringFile> &p_gametext);
	bool pick_count(const Ref<MenuDriver> &p_driver, const String &p_control, int p_row);
	int selected_weapon(const Ref<MenuDriver> &p_driver, const String &p_control) const;
	// A category's AMMO1 row + 1, -1 with no AMMO1 or no selection (armory_selected_clips).
	int selected_clips(const Ref<MenuDriver> &p_driver, const String &p_control) const;
	double weight(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons) const;
	// The ACCEPT's page, also the class's page from here on.
	TypedArray<WeaponKitEntry> accept(const Ref<MenuDriver> &p_driver,
			const Ref<WeaponDatabase> &p_weapons, int p_entity_team, int p_selected_class);
	TypedArray<WeaponKitEntry> get_page(int p_player_class) const;
	// The title a session over `p_root` runs as, whose serializer the ACCEPT and the class flip
	// run (engine menu::ArmoryLoadout::title).
	void set_title_root(const Ref<ResourceRoot> &p_root);
};

} // namespace godot
