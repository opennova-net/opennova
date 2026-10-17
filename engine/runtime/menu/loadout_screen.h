#pragma once

#include <formats/def/def.h>
#include <formats/playersav/weapon_sav.h>
#include <runtime/hud/game_text_lookup.h>

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::menu {

class MenuRuntime;

// Recorded counts are keyed by the parent weapon-table index, including the
// secondary count for its subclass. An absent pick means the authored default.
using LoadoutAmmoCounts = std::unordered_map<int, int>;
using LoadoutParentIndices = std::array<int, 3>; // PRIMARY, SECONDARY, ACCESSORY; -1 = NONE
using WeaponAvailability = std::function<int(const std::string &)>;

// Populate the existing native menu surface without emitting user selections.
// PLAYER_INFO retains hidden rows; its ammo-type statics stay authored and the
// returned saved type reflects NOAMMOTYPES. WEAPON retains its existing main-ammo
// only implementation (the subclass/type cascade remains a tracked deferral).
int player_info_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, int saved_primary, int saved_secondary,
		int saved_type, const hud::GameTextLookup &text);
void armory_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, const std::string &current_name,
		int current_clips, const hud::GameTextLookup &text);

// First three filtered grenade definitions in table order, including positions
// whose controls are absent. Availability restricts the armory's count rows only
// after a grenade owns its position; it never compacts the returned indices.
std::vector<int> player_info_fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const LoadoutAmmoCounts &saved,
		const hud::GameTextLookup &text);
std::vector<int> armory_fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const std::vector<playersav::KitEntry> &current,
		const WeaponAvailability &availability, const hud::GameTextLookup &text);

// One loadout category list as its fill left it: the weapon.def row each list
// row carries, row 0 the NONE row (-1).
using LoadoutSlotRows = std::vector<int32_t>;

// Fill the category list `control` (PRIMARY / SECONDARY / ACCESSORY, weapon
// slot `slot`) and select its row 0: the slot filter's rows for the class and
// team masks (world::weapon_slot_indices), less any whose availability reads 0
// when `availability` is set (the WEAPON screen's map term), labelled
// (weapon_label) and in the screens' row order (armory_slot_order), under the
// NONE row `none_label` at row 0. An absent control fills nothing and returns
// no rows.
// [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430 -- the NONE inserts
//  @0x56058f / @0x56059d / @0x5605ab; UI_PopulateThreeCategoryLists @0x566db0 --
//  the !g_ArmoryWeaponAvailability[i] skip @0x566e6b, NONE at row 0 @0x566f15;
//  the sorts loadout_labels.h armory_slot_order carries]
LoadoutSlotRows loadout_fill_slot(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int slot, int class_mask, int team_mask,
		const WeaponAvailability &availability, const std::string &none_label,
		const hud::GameTextLookup &gametext);

// The weapon.def row a category list's selection carries; -1 for the NONE row,
// no selection or an absent control.
int32_t loadout_selected_weapon(const MenuRuntime &menu, const std::string &control,
		const LoadoutSlotRows &rows);

// PLAYER_INFO's select of a saved page's weapon: the list row carrying the
// weapon.def row `weapon_row`, else no row at all
// [orig: UIList_SelectByValue @0x645240, which clears every row first, from
//  PlayerInfo_PopulateWeaponAccessoryAmmoUI @0x55eab2 / @0x55ea3d / @0x55ea79].
void loadout_select_weapon_row(MenuRuntime &menu, const std::string &control,
		const LoadoutSlotRows &rows, int32_t weapon_row);

// The WEAPON screen's reselect of the current kit's weapon: the first list row
// whose weapon is named `name` (case-insensitive); an empty name or no match
// keeps the fill's selection
// [orig: UIList_SelectByValue @0x645240 select-by-adm-index in
//  UI_PopulateAmmoTypeComboBoxes @0x564930].
void loadout_select_weapon_name(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, const LoadoutSlotRows &rows, const std::string &name);

// The WEAPON screen's clip count for a category: its AMMO1 row + 1, -1 (the
// def default) with no AMMO1 control, no selection or no rows; the main leg
// takes adm[23] raw [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0, @0x566166].
int armory_selected_clips(const MenuRuntime &menu, const std::string &control);

// The WEAPON screen's selection as WeaponLoadout_SerializeSelectionsToBuffer
// writes it before the apply: each category's weapon name ("" for NONE) and
// clip count, and one {name, count, -1, -1} entry per grenade position whose
// GRENADE_AMMO row is past 0, in position order
// [orig: WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 -- {name, ammoPri,
//  ammoSec, flags} per slot into the per-class buffer @0x25DD740 + 2048*class].
struct ArmoryLoadoutPick {
	std::string names[3];
	int clips[3] = { -1, -1, -1 };
	std::vector<playersav::KitEntry> grenades;
};
ArmoryLoadoutPick armory_loadout_pick(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutSlotRows (&rows)[3], const std::vector<int> &grenades);

// Keep each screen's control-presence/visibility gates and summation order.
// Indices name the selected definitions; the menu supplies the UI state.
double player_info_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades,
		const LoadoutAmmoCounts &primary, const LoadoutAmmoCounts &secondary);
double armory_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades);

} // namespace opennova::menu
