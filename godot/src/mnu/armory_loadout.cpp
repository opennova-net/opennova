#include "mnu/armory_loadout.h"

#include "mnu/menu_driver.h"
#include "mnu/mnu_document.h" // MenuDriver's inline document() copies a Ref<MnuDocument>
#include "object/weapon_database.h"
#include "player/player_profiles.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/weapon_kit_entry.h"
#include "util/string_convert.h"

using namespace godot;
using opennova::to_gd;
using opennova::to_std;

namespace {

const opennova::def::DefWeaponsFile &weapons_of(const Ref<WeaponDatabase> &p_weapons) {
	static const opennova::def::DefWeaponsFile empty{};
	return p_weapons.is_valid() ? p_weapons->native_file() : empty;
}

const opennova::playersav::Record &record_of(const Ref<PlayerProfiles> &p_profiles) {
	static const opennova::playersav::Record empty{};
	return p_profiles.is_valid() ? p_profiles->native().current_weapons() : empty;
}

// The typed kit rows as a page (null rows drop) and back.
opennova::playersav::KitPage page_of(const TypedArray<WeaponKitEntry> &p_kit) {
	opennova::playersav::KitPage page;
	for (int i = 0; i < p_kit.size(); ++i) {
		const Ref<WeaponKitEntry> row = p_kit[i];
		if (row.is_null()) continue;
		const opennova::world::WeaponKitEntry &value = row->value();
		page.entries.push_back({ value.name, value.ammo_primary, value.ammo_secondary,
				value.flags });
	}
	return page;
}

TypedArray<WeaponKitEntry> kit_of(const opennova::playersav::KitPage &p_page) {
	TypedArray<WeaponKitEntry> out;
	for (const opennova::playersav::KitEntry &entry : p_page.entries) {
		Ref<WeaponKitEntry> row;
		row.instantiate();
		row->assign({ entry.name, entry.ammo_primary, entry.ammo_secondary, entry.flags });
		out.push_back(row);
	}
	return out;
}

opennova::menu::WeaponAvailability availability_of(const Callable &p_availability) {
	if (!p_availability.is_valid()) return {};
	return [p_availability](const std::string &name) {
		return int(p_availability.call(to_gd(name)));
	};
}

} // namespace

void ArmoryLoadout::stage(const Ref<PlayerProfiles> &p_profiles, int p_entity_team,
		const TypedArray<WeaponKitEntry> &p_spawn) {
	value_.buffers.stage(record_of(p_profiles), p_entity_team, page_of(p_spawn));
}

bool ArmoryLoadout::stage_on_show(const Ref<PlayerProfiles> &p_profiles, int p_entity_team,
		const TypedArray<WeaponKitEntry> &p_spawn) {
	return value_.buffers.stage_on_show(record_of(p_profiles), p_entity_team, page_of(p_spawn));
}

void ArmoryLoadout::populate(const Ref<MenuDriver> &p_driver, const Ref<WeaponDatabase> &p_weapons,
		int p_entity_team, int p_selected_class, const Callable &p_availability,
		const String &p_none_label, const Ref<RtxtStringFile> &p_gametext) {
	if (p_driver.is_null()) return;
	value_.populate(p_driver->runtime(), weapons_of(p_weapons), p_entity_team, p_selected_class,
			availability_of(p_availability), opennova::gd_to_retail_text(p_none_label),
			game_text_lookup(p_gametext));
}

void ArmoryLoadout::flip_class(const Ref<MenuDriver> &p_driver,
		const Ref<WeaponDatabase> &p_weapons, int p_entity_team, int p_from_class,
		int p_to_class, const Callable &p_availability, const String &p_none_label,
		const Ref<RtxtStringFile> &p_gametext) {
	if (p_driver.is_null()) return;
	value_.flip_class(p_driver->runtime(), weapons_of(p_weapons), p_entity_team, p_from_class,
			p_to_class, availability_of(p_availability), opennova::gd_to_retail_text(p_none_label),
			game_text_lookup(p_gametext));
}

void ArmoryLoadout::pick_weapon(const Ref<MenuDriver> &p_driver,
		const Ref<WeaponDatabase> &p_weapons, const String &p_control,
		const Ref<RtxtStringFile> &p_gametext) {
	if (p_driver.is_null()) return;
	value_.pick_weapon(p_driver->runtime(), weapons_of(p_weapons), to_std(p_control),
			game_text_lookup(p_gametext));
}

bool ArmoryLoadout::pick_count(const Ref<MenuDriver> &p_driver, const String &p_control,
		int p_row) {
	return p_driver.is_valid() && value_.pick_count(p_driver->runtime(), to_std(p_control), p_row);
}

int ArmoryLoadout::selected_weapon(const Ref<MenuDriver> &p_driver, const String &p_control) const {
	if (p_driver.is_null()) return -1;
	return value_.selected_weapon(p_driver->runtime(), to_std(p_control));
}

int ArmoryLoadout::selected_clips(const Ref<MenuDriver> &p_driver, const String &p_control) const {
	return p_driver.is_valid()
			? opennova::menu::armory_selected_clips(p_driver->runtime(), to_std(p_control))
			: -1;
}

double ArmoryLoadout::weight(const Ref<MenuDriver> &p_driver,
		const Ref<WeaponDatabase> &p_weapons) const {
	if (p_driver.is_null() || p_weapons.is_null()) return 0.0;
	return value_.weight(p_driver->runtime(), p_weapons->native_file());
}

TypedArray<WeaponKitEntry> ArmoryLoadout::accept(const Ref<MenuDriver> &p_driver,
		const Ref<WeaponDatabase> &p_weapons, int p_entity_team, int p_selected_class) {
	if (p_driver.is_null()) return {};
	return kit_of(value_.accept(p_driver->runtime(), weapons_of(p_weapons), p_entity_team,
			p_selected_class));
}

TypedArray<WeaponKitEntry> ArmoryLoadout::get_page(int p_player_class) const {
	return kit_of(value_.buffers.page(p_player_class));
}

void ArmoryLoadout::set_title_root(const Ref<ResourceRoot> &p_root) {
	value_.title = &ResourceRoot::session_title(p_root);
}

void ArmoryLoadout::_bind_methods() {
	ClassDB::bind_method(D_METHOD("stage", "profiles", "entity_team", "spawn"),
			&ArmoryLoadout::stage);
	ClassDB::bind_method(D_METHOD("stage_on_show", "profiles", "entity_team", "spawn"),
			&ArmoryLoadout::stage_on_show);
	ClassDB::bind_method(D_METHOD("populate", "driver", "weapons", "entity_team",
								 "selected_class", "availability", "none_label", "gametext"),
			&ArmoryLoadout::populate);
	ClassDB::bind_method(D_METHOD("flip_class", "driver", "weapons", "entity_team", "from_class",
								 "to_class", "availability", "none_label", "gametext"),
			&ArmoryLoadout::flip_class);
	ClassDB::bind_method(D_METHOD("pick_weapon", "driver", "weapons", "control", "gametext"),
			&ArmoryLoadout::pick_weapon);
	ClassDB::bind_method(D_METHOD("pick_count", "driver", "control", "row"),
			&ArmoryLoadout::pick_count);
	ClassDB::bind_method(D_METHOD("selected_weapon", "driver", "control"),
			&ArmoryLoadout::selected_weapon);
	ClassDB::bind_method(D_METHOD("selected_clips", "driver", "control"),
			&ArmoryLoadout::selected_clips);
	ClassDB::bind_method(D_METHOD("weight", "driver", "weapons"), &ArmoryLoadout::weight);
	ClassDB::bind_method(D_METHOD("accept", "driver", "weapons", "entity_team", "selected_class"),
			&ArmoryLoadout::accept);
	ClassDB::bind_method(D_METHOD("get_page", "player_class"), &ArmoryLoadout::get_page);
	ClassDB::bind_method(D_METHOD("set_title_root", "root"), &ArmoryLoadout::set_title_root);
}
