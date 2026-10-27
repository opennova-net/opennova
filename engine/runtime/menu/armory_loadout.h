#pragma once

// The WEAPON screen's loadout state (ADR 0042: the engine decides, the shell's
// ArmoryMenuCompanion only wires controls and mounts icons). The game keeps the per-class
// kit pages, the screen's counts and its two ammo-type bytes in globals that outlive the
// screen and the mission (D-MNU-9); this holds them, with the category lists and the
// grenade positions as the last populate left them, and runs the screen's populate, class
// flip, handler writes, weight and ACCEPT over them. The fills, the page walk, the
// serialize and the weight are loadout_screen.h's.

#include <runtime/menu/loadout_screen.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

class MenuRuntime;

struct ArmoryLoadout {
	ArmoryLoadoutBuffers buffers;
	ArmoryScreenPairs pairs;
	// Each category list's rows (loadout_fill_slot) and the grenade positions' weapon.def
	// rows (armory_fill_grenades) [orig: dword_25DCF3C..dword_25DCF44, the positions'
	// catalog rows].
	LoadoutSlotRows slot_rows[3];
	std::vector<int> grenade_rows;

	// UI_PopulateThreeCategoryLists over the class's page: the PRIMARY / SECONDARY /
	// ACCESSORY lists for the class's filter bit (world::armory_class_filter_mask) and the
	// entity team's mask (armory_team_mask) under the NONE row `none_label`, then the
	// page's populate (armory_populate_page) and the grenade positions
	// (armory_fill_grenades), which that populate runs last.
	// [orig: UI_PopulateThreeCategoryLists @0x566db0 - the fill @0x566e2c..0x566f34, the
	//  populate @0x566f3c; UI_PopulateAmmoTypeComboBoxes @0x564930 - the grenade fill
	//  @0x565465]
	void populate(MenuRuntime &menu, const def::DefWeaponsFile &weapons, int entity_team,
			int selected_class, const WeaponAvailability &availability,
			const std::string &none_label, const hud::GameTextLookup &text);

	// UI_HandleTeamClassSelection: the page the screen shows saved into the class it leaves
	// (armory_serialize_page), then the populate of the class it picks.
	// [orig: UI_HandleTeamClassSelection @0x566f60 - the serialize @0x566f89, the class
	//  @0x566f95, the filter switch @0x566fa5, the populate @0x566fc5]
	void flip_class(MenuRuntime &menu, const def::DefWeaponsFile &weapons, int entity_team,
			int from_class, int to_class, const WeaponAvailability &availability,
			const std::string &none_label, const hud::GameTextLookup &text);

	// A category list's pick: its ammo controls over the new selection (armory_fill_ammo); a
	// control that is no category list does nothing
	// [orig: UI_OnPrimaryWeaponTypeChanged @0x5662d0, UI_OnSecondaryWeaponChanged @0x566670,
	//  UI_OnWeaponAmmoSlotChanged @0x566a10].
	void pick_weapon(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
			const std::string &control, const hud::GameTextLookup &text);

	// The count and type handlers' writes for a pick of `row` on `control`: a category's
	// _AMMO1 / _AMMO2 the row + 1 into its selected weapon's pair (the rows carry the
	// weapon their fill read), a PRIMARY / SECONDARY _AMMO1_TYPE the row as its byte, a
	// GRENADE_AMMO1..3 the row into its position's count; false for another control.
	// [orig: WeaponDef_RegisterUICallbacks @0x567020 - the registrations
	//  @0x56705d..0x5671e9; UI_OnPrimaryAmmoCountSelected @0x566620, UI_OnSecondaryAmmoCountSelected @0x5669c0, UI_OnAccessoryAmmoCountSelected @0x566d40
	//  (offset 0 AMMO1, 1 AMMO2: pair + offset = row + 1 @0x56663e); UI_OnPrimaryAmmoTypeSelected @0x566650 /
	//  UI_OnSecondaryAmmoTypeSelected @0x5669f0 (the byte @0x566661 / @0x566a01); WeaponDef_UISlotSelectCallback
	//  @0x566d70 (the position's row @0x566d7e, the store @0x566d9a)]
	bool pick_count(const MenuRuntime &menu, const std::string &control, int row);

	// The weapon.def row a category list's selection carries (-1 for NONE, no selection or
	// another control).
	int32_t selected_weapon(const MenuRuntime &menu, const std::string &control) const;

	// The screen's weight over its selections (armory_screen_weight).
	double weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons) const;

	// The ACCEPT's head: the page the screen shows serialized into the class's buffer, the
	// page the apply then reads (the class's page; an empty one for a class outside the
	// buffers).
	// [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 - the serialize @0x565d22, the copy
	//  into g_LoadoutWeaponNameList @0x565d4b]
	const playersav::KitPage &accept(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
			int entity_team, int selected_class);
};

} // namespace opennova::menu
