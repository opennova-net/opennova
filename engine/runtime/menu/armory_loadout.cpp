#include <runtime/menu/armory_loadout.h>

#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/player_info_kit.h>
#include <runtime/world/player_loadout.h>

#include <base/io/strutil.h>

#include <iterator>

namespace opennova::menu {
namespace {

// The category lists and their weapon slots (DefWeaponDef::weapon_class_slot), in the
// serialize's order [orig: off_83C94C / off_83C950 / off_83C954].
const char *const kCategories[3] = { "PRIMARY", "SECONDARY", "ACCESSORY" };
const int kCategorySlots[3] = { 1, 2, 0 };
const char *const kGrenades[3] = { "GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3" };

// The category list a control names, -1 for another control.
int category_of(const std::string &control) {
	for (int i = 0; i < 3; ++i) {
		if (strutil::iequals(control, kCategories[i])) return i;
	}
	return -1;
}

} // namespace

void ArmoryLoadout::populate(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int entity_team, int selected_class, const WeaponAvailability &availability,
		const std::string &none_label, const hud::GameTextLookup &text) {
	const int32_t class_mask = world::armory_class_filter_mask(selected_class);
	const int32_t team_mask = armory_team_mask(entity_team);
	// DFX2-73: dfx2.exe's populate inserts no NONE row [orig: UI_PopulateThreeCategoryLists
	//  @0x565d10 - the sorts @0x565e33..0x565e4d, then the ammo populate @0x565e55 (dfx2.exe)].
	const std::string *none = require_title(title, "ArmoryLoadout::populate").armory_lists_none_row
			? &none_label
			: nullptr;
	for (int i = 0; i < 3; ++i)
		slot_rows[i] = loadout_fill_slot(menu, weapons, kCategories[i], kCategorySlots[i],
				class_mask, team_mask, availability, none, text);
	grenade_rows = armory_populate_page(
			menu, weapons, slot_rows, buffers.page(selected_class), pairs, text);
	const std::vector<int> filled =
			armory_fill_grenades(menu, weapons, class_mask, team_mask, pairs, availability, text);
	for (size_t i = 0; i < filled.size() && i < grenade_rows.size(); ++i)
		grenade_rows[i] = filled[i];
}

bool ArmoryLoadout::every_grenade_position() const {
	return require_title(title, "ArmoryLoadout::every_grenade_position").death_screen_loadout;
}

void ArmoryLoadout::flip_class(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int entity_team, int from_class, int to_class, const WeaponAvailability &availability,
		const std::string &none_label, const hud::GameTextLookup &text) {
	buffers.store(from_class,
			armory_serialize_page(menu, weapons, slot_rows, grenade_rows, pairs, entity_team,
					from_class, every_grenade_position()));
	populate(menu, weapons, entity_team, to_class, availability, none_label, text);
}

void ArmoryLoadout::pick_weapon(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, const hud::GameTextLookup &text) {
	const int category = category_of(control);
	if (category < 0) return;
	armory_fill_ammo(menu, weapons, kCategories[category],
			selected_weapon(menu, kCategories[category]), pairs, text);
}

bool ArmoryLoadout::pick_count(const MenuRuntime &menu, const std::string &control, int row) {
	for (int i = 0; i < 3; ++i) {
		const std::string category = kCategories[i];
		const int32_t catalog = catalog_index_of_row(selected_weapon(menu, category));
		if (strutil::iequals(control, category + "_AMMO1")) {
			pairs.set_clips(catalog, row + 1);
			return true;
		}
		if (strutil::iequals(control, category + "_AMMO2")) {
			pairs.set_secondary(catalog, row + 1);
			return true;
		}
		// The byte the TYPE pick stores is the row's low byte, read back signed.
		if (i < 2 && strutil::iequals(control, category + "_AMMO1_TYPE")) {
			pairs.type[i] = static_cast<int8_t>(row);
			return true;
		}
	}
	for (size_t i = 0; i < std::size(kGrenades); ++i) {
		if (!strutil::iequals(control, kGrenades[i])) continue;
		if (i < grenade_rows.size()) pairs.set_clips(catalog_index_of_row(grenade_rows[i]), row);
		return true;
	}
	return false;
}

int32_t ArmoryLoadout::selected_weapon(const MenuRuntime &menu, const std::string &control) const {
	const int category = category_of(control);
	return category >= 0 ? loadout_selected_weapon(menu, kCategories[category], slot_rows[category])
						 : -1;
}

double ArmoryLoadout::weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons) const {
	LoadoutParentIndices parents{};
	for (int i = 0; i < 3; ++i)
		parents[static_cast<size_t>(i)] = selected_weapon(menu, kCategories[i]);
	return armory_screen_weight(menu, weapons, parents, grenade_rows);
}

const playersav::KitPage &ArmoryLoadout::accept(const MenuRuntime &menu,
		const def::DefWeaponsFile &weapons, int entity_team, int selected_class) {
	buffers.store(selected_class,
			armory_serialize_page(menu, weapons, slot_rows, grenade_rows, pairs, entity_team,
					selected_class, every_grenade_position()));
	return buffers.page(selected_class);
}

} // namespace opennova::menu
