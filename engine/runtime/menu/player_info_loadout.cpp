#include <runtime/menu/player_info_loadout.h>

#include <runtime/menu/menu_runtime.h>
#include <runtime/world/player_loadout.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdlib>

namespace opennova::menu {
namespace {

// The category list a control names, -1 for another control.
int category_of(const std::string &control) {
	for (int i = 0; i < 3; ++i) {
		if (strutil::iequals(control, kPlayerInfoCategoryControls[i])) return i;
	}
	return -1;
}

int32_t saved_count(const LoadoutAmmoCounts &counts, int index) {
	const auto found = counts.find(index);
	return found == counts.end() ? -1 : found->second;
}

// An authored value attribute as a number: an optional sign and decimal digits,
// else 0.
int32_t authored_int(const std::string &value) {
	size_t i = (!value.empty() && (value[0] == '+' || value[0] == '-')) ? 1 : 0;
	if (i == value.size()) return 0;
	for (size_t k = i; k < value.size(); ++k) {
		if (value[k] < '0' || value[k] > '9') return 0;
	}
	return static_cast<int32_t>(std::strtol(value.c_str(), nullptr, 10));
}

bool team_index(int team) { return team == 0 || team == 1; }

} // namespace

void PlayerInfoLoadout::clear_counts() {
	ammo_primary.clear();
	ammo_secondary.clear();
}

void PlayerInfoLoadout::fill_slots(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const std::string &none_label,
		const hud::GameTextLookup &gametext) {
	// [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430]
	for (int i = 0; i < 3; ++i) {
		const int combo = menu.widget_id(kPlayerInfoCategoryControls[i]);
		if (combo < 0) continue;
		slot_rows[i] = loadout_fill_slot(menu, weapons, kPlayerInfoCategoryControls[i],
				kPlayerInfoCategorySlots[i], class_mask, team_mask, WeaponAvailability(), none_label,
				gametext);
	}
}

void PlayerInfoLoadout::restore(MenuRuntime &menu, const PlayerInfoKitRestore &page,
		int32_t restored_class, int team) {
	page_class = restored_class;
	clear_counts();
	// Catalog row r is weapon.def row r - 1; the "None" row 0 keeps its pair at -1.
	const size_t rows = std::min(page.ammo_primary.size(), page.ammo_secondary.size());
	for (size_t row = 0; row < rows; ++row) {
		const int index = static_cast<int>(row) - 1;
		if (page.ammo_primary[row] != -1) ammo_primary[index] = page.ammo_primary[row];
		if (page.ammo_secondary[row] != -1) ammo_secondary[index] = page.ammo_secondary[row];
	}
	for (int i = 0; i < 3; ++i) {
		if (page.select[i] >= 0) {
			loadout_select_weapon_row(menu, kPlayerInfoCategoryControls[i], slot_rows[i],
					page.select[i] - 1);
		}
	}
	for (int i = 0; i < 2; ++i) {
		if (page.has_type[i] && team_index(team)) ammo_type[i][team] = page.type[i];
	}
	for (int i = 0; i < 3; ++i) restored_grenades[i] = page.grenade_slots[i] - 1;
}

int32_t PlayerInfoLoadout::selected_weapon(const MenuRuntime &menu, const std::string &control) const {
	const int category = category_of(control);
	return category < 0 ? -1 : loadout_selected_weapon(menu, control, slot_rows[category]);
}

void PlayerInfoLoadout::fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int team, const hud::GameTextLookup &text) {
	const int category = category_of(control);
	if (category < 0) return;
	const int index = selected_weapon(menu, control);
	const int32_t saved_type = team_index(team) ? ammo_type[category][team] : 0;
	const int32_t type = player_info_fill_ammo(menu, weapons, control, index,
			saved_count(ammo_primary, index), saved_count(ammo_secondary, index), saved_type, text);
	if (team_index(team)) ammo_type[category][team] = type;
}

void PlayerInfoLoadout::fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const hud::GameTextLookup &text) {
	grenade_rows = player_info_fill_grenades(menu, weapons, class_mask, team_mask, ammo_primary, text);
}

void PlayerInfoLoadout::pick_clips(const MenuRuntime &menu, const std::string &control, bool secondary,
		int row) {
	const int32_t index = selected_weapon(menu, control);
	if (index < 0) return;
	// [orig: @0x55f730 -- the handler stores selected_row + 1]
	(secondary ? ammo_secondary : ammo_primary)[index] = row + 1;
}

void PlayerInfoLoadout::pick_ammo_type(const MenuRuntime &menu, const std::string &control, int team,
		int row) {
	const int category = category_of(control);
	if (category < 0 || !team_index(team)) return;
	// [orig: @0x55f760 / @0x55f7e0 -- the row's VALUE byte, stored per team]
	const int combo = menu.widget_id(control + "_AMMO1_TYPE");
	const std::string value = combo >= 0 && row >= 0 ? menu.item_value(combo, row) : std::string();
	ammo_type[category][team] = authored_int(value);
}

void PlayerInfoLoadout::pick_grenade(int position, int row) {
	if (position < 0 || static_cast<size_t>(position) >= grenade_rows.size()) return;
	ammo_primary[grenade_rows[static_cast<size_t>(position)]] = row; // [orig: @0x55fb70]
}

int32_t PlayerInfoLoadout::selected_clips(const MenuRuntime &menu, const std::string &control) const {
	const int32_t index = selected_weapon(menu, control);
	return index < 0 ? -1 : saved_count(ammo_primary, index);
}

int32_t PlayerInfoLoadout::selected_ammo_type(const std::string &control, int team) const {
	const int category = category_of(control);
	return category < 0 || !team_index(team) ? 0 : ammo_type[category][team];
}

double PlayerInfoLoadout::weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons) const {
	LoadoutParentIndices parents{ -1, -1, -1 };
	for (int i = 0; i < 3; ++i) parents[i] = selected_weapon(menu, kPlayerInfoCategoryControls[i]);
	return player_info_screen_weight(menu, weapons, parents, grenade_rows, ammo_primary, ammo_secondary);
}

PlayerInfoKitSelection PlayerInfoLoadout::kit_selection(const MenuRuntime &menu, int team,
		int player_class) const {
	PlayerInfoKitSelection sel;
	sel.team_mask = world::player_info_team_mask(team);
	sel.player_class = player_class;
	const auto pick = [this](int32_t index, int32_t flags) {
		KitSlotPick p;
		p.weapon_index = catalog_index_of_row(index);
		p.ammo_primary = saved_count(ammo_primary, index);
		p.ammo_secondary = saved_count(ammo_secondary, index);
		p.flags = flags;
		return p;
	};
	KitSlotPick *categories[3] = { &sel.primary, &sel.secondary, &sel.accessory };
	for (int i = 0; i < 3; ++i) {
		const char *control = kPlayerInfoCategoryControls[i];
		*categories[i] = pick(selected_weapon(menu, control),
				i < 2 ? selected_ammo_type(control, team) : kKitFiller);
	}
	for (int i = 0; i < 3; ++i) {
		const int32_t index = static_cast<size_t>(i) < grenade_rows.size()
				? grenade_rows[static_cast<size_t>(i)]
				: restored_grenades[static_cast<size_t>(i)];
		sel.grenades[i] = pick(index, kKitFiller);
	}
	return sel;
}

} // namespace opennova::menu
