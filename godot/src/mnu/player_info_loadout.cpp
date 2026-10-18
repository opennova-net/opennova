#include "mnu/player_info_loadout.h"

#include "mnu/menu_driver.h"
#include "mnu/mnu_document.h"
#include "object/weapon_database.h"
#include "player/player_info_kit_restore.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

using namespace godot;
using opennova::to_std;

namespace {

const opennova::def::DefWeaponsFile &weapons_of(const Ref<WeaponDatabase> &p_weapons) {
	static const opennova::def::DefWeaponsFile empty{};
	return p_weapons.is_valid() ? p_weapons->native_file() : empty;
}

} // namespace

void PlayerInfoLoadout::fill_slots(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
		int p_class_mask, int p_team_mask, const String &p_none_label,
		const Ref<RtxtStringFile> &p_gametext) {
	if (p_driver.is_null()) return;
	value_.fill_slots(p_driver->runtime(), weapons_of(p_weapons), p_class_mask, p_team_mask,
			to_std(p_none_label), game_text_lookup(p_gametext));
}

void PlayerInfoLoadout::restore(const Ref<MenuDriver> &p_driver, const Ref<PlayerInfoKitRestore> &p_page,
		int p_team) {
	if (p_driver.is_null() || p_page.is_null()) return;
	value_.restore(p_driver->runtime(), p_page->native(), p_page->get_page_class(), p_team);
}

int PlayerInfoLoadout::selected_weapon(const Ref<MenuDriver> &p_driver, const String &p_control) const {
	return p_driver.is_valid() ? value_.selected_weapon(p_driver->runtime(), to_std(p_control)) : -1;
}

void PlayerInfoLoadout::fill_ammo(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
		const String &p_control, int p_team, const Ref<RtxtStringFile> &p_gametext) {
	if (p_driver.is_null()) return;
	value_.fill_ammo(p_driver->runtime(), weapons_of(p_weapons), to_std(p_control), p_team,
			game_text_lookup(p_gametext));
}

void PlayerInfoLoadout::fill_grenades(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
		int p_class_mask, int p_team_mask, const Ref<RtxtStringFile> &p_gametext) {
	if (p_driver.is_null()) return;
	value_.fill_grenades(p_driver->runtime(), weapons_of(p_weapons), p_class_mask, p_team_mask,
			game_text_lookup(p_gametext));
}

void PlayerInfoLoadout::pick_clips(const Ref<MenuDriver> &p_driver, const String &p_control,
		bool p_secondary, int p_row) {
	if (p_driver.is_valid()) value_.pick_clips(p_driver->runtime(), to_std(p_control), p_secondary, p_row);
}

void PlayerInfoLoadout::pick_ammo_type(const Ref<MenuDriver> &p_driver, const String &p_control,
		int p_team, int p_row) {
	if (p_driver.is_valid()) value_.pick_ammo_type(p_driver->runtime(), to_std(p_control), p_team, p_row);
}

int PlayerInfoLoadout::selected_clips(const Ref<MenuDriver> &p_driver, const String &p_control) const {
	return p_driver.is_valid() ? value_.selected_clips(p_driver->runtime(), to_std(p_control)) : -1;
}

int PlayerInfoLoadout::selected_ammo_type(const String &p_control, int p_team) const {
	return value_.selected_ammo_type(to_std(p_control), p_team);
}

double PlayerInfoLoadout::weight(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons) const {
	if (p_driver.is_null() || p_weapons.is_null()) return 0.0;
	return value_.weight(p_driver->runtime(), p_weapons->native_file());
}

Array PlayerInfoLoadout::kit_entries(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
		int p_team, int p_player_class) const {
	if (p_driver.is_null() || p_weapons.is_null()) return Array();
	return p_weapons->kit_entry_rows(value_.kit_selection(p_driver->runtime(), p_team, p_player_class));
}

void PlayerInfoLoadout::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear_counts"), &PlayerInfoLoadout::clear_counts);
	ClassDB::bind_method(D_METHOD("fill_slots", "driver", "weapons", "class_mask", "team_mask", "none_label",
								 "gametext"),
			&PlayerInfoLoadout::fill_slots);
	ClassDB::bind_method(D_METHOD("restore", "driver", "page", "team"), &PlayerInfoLoadout::restore);
	ClassDB::bind_method(D_METHOD("selected_weapon", "driver", "control"), &PlayerInfoLoadout::selected_weapon);
	ClassDB::bind_method(D_METHOD("fill_ammo", "driver", "weapons", "control", "team", "gametext"),
			&PlayerInfoLoadout::fill_ammo);
	ClassDB::bind_method(D_METHOD("fill_grenades", "driver", "weapons", "class_mask", "team_mask", "gametext"),
			&PlayerInfoLoadout::fill_grenades);
	ClassDB::bind_method(D_METHOD("pick_clips", "driver", "control", "secondary", "row"),
			&PlayerInfoLoadout::pick_clips);
	ClassDB::bind_method(D_METHOD("pick_ammo_type", "driver", "control", "team", "row"),
			&PlayerInfoLoadout::pick_ammo_type);
	ClassDB::bind_method(D_METHOD("pick_grenade", "position", "row"), &PlayerInfoLoadout::pick_grenade);
	ClassDB::bind_method(D_METHOD("selected_clips", "driver", "control"), &PlayerInfoLoadout::selected_clips);
	ClassDB::bind_method(D_METHOD("selected_ammo_type", "control", "team"),
			&PlayerInfoLoadout::selected_ammo_type);
	ClassDB::bind_method(D_METHOD("weight", "driver", "weapons"), &PlayerInfoLoadout::weight);
	ClassDB::bind_method(D_METHOD("kit_entries", "driver", "weapons", "team", "player_class"),
			&PlayerInfoLoadout::kit_entries);
	ClassDB::bind_method(D_METHOD("get_page_class"), &PlayerInfoLoadout::get_page_class);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "page_class"), "", "get_page_class");
}
