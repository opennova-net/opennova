#pragma once

// The PLAYER_INFO screen's loadout panel state (ADR 0042: the engine decides,
// the shell's PlayerInfoMenuCompanion only wires controls and mounts icons):
// the three category lists and the grenade positions as their fills left
// them, the interleaved saved-count pairs keyed by weapon.def row, the ammo-type
// byte per list per team, the grenade slots and the class page the last kit
// restore left, and the reads and writes the screen's handlers make on them.
// The fills, the weight and the kit page are loadout_screen.h's and
// player_info_kit.h's; this holds what the screen keeps between them.

#include <runtime/menu/loadout_screen.h>
#include <runtime/menu/player_info_kit.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

class MenuRuntime;

// The three loadout categories' controls in the kit page's order
// [orig: the category_names[] array PlayerInfo_SerializeWeaponLoadout builds
//  @0x55e4bc..0x55e4cc].
inline constexpr const char *kPlayerInfoCategoryControls[3] = { "PRIMARY", "SECONDARY", "ACCESSORY" };
// Each category's weapon slot (DefWeaponDef::weapon_class_slot: 0 accessory,
// 1 primary, 2 secondary).
inline constexpr int kPlayerInfoCategorySlots[3] = { 1, 2, 0 };

struct PlayerInfoLoadout {
	// Each category list's rows (loadout_fill_slot) and the grenade positions'
	// weapon.def rows (player_info_fill_grenades).
	LoadoutSlotRows slot_rows[3];
	std::vector<int> grenade_rows;
	// The saved-count pairs by weapon.def row; absent = -1, the def default
	// [orig: g_PlayerInfoAmmoPriCounts @0x25DC560 / g_PlayerInfoAmmoSecCounts
	//  @0x25DC564, interleaved].
	LoadoutAmmoCounts ammo_primary;
	LoadoutAmmoCounts ammo_secondary;
	// The ammo-type byte per category list per team (0 FMJ, 1 AP, 2 SP; 0 until
	// set) [orig: g_PlayerInfoAmmoTypePri/Sec[teamIndex] @0x25DCD64 / @0x25DCD68].
	std::array<std::array<int32_t, 2>, 3> ammo_type{};
	// The grenade slots the last kit restore filled, as weapon.def rows (-1 the
	// "None" row): a slot the class/team filter leaves past grenade_rows keeps
	// the page's.
	std::array<int32_t, 3> restored_grenades{ -1, -1, -1 };
	// The class whose page of the shown side the last restore read: the page
	// the dialog's next save writes back.
	int32_t page_class = 0;

	// Every count pair dropped (a table read again: the pairs are keyed by the
	// old table's rows).
	void clear_counts();

	// The three category lists filled for the class and team masks under the
	// NONE row `none_label`.
	void fill_slots(MenuRuntime &menu, const def::DefWeaponsFile &weapons, int class_mask,
			int team_mask, const std::string &none_label, const hud::GameTextLookup &gametext);

	// The populate's restore of the shown side's class page (player_info_restore_kit's
	// record, catalog rows: weapon.def's row i is catalog row i + 1, the "None"
	// row 0 the weapon.def row -1): the count pairs reset to the page's, each
	// category list selected by the weapon the page names (a weapon the list
	// lacks leaves it with none), the team's PRIMARY / SECONDARY type bytes and
	// the grenade slots
	// [orig: PlayerInfo_PopulateWeaponAccessoryAmmoUI @0x55e8b0 -- the slot
	//  zeroing @0x55e8d0..0x55e8da, the pair reset @0x55e8e0..0x55e8f5, the
	//  selects @0x55eab2 / @0x55ea3d / @0x55ea79, the type bytes @0x55ea48 /
	//  @0x55ea84].
	void restore(MenuRuntime &menu, const PlayerInfoKitRestore &page, int32_t page_class, int team);

	// The weapon.def row a category list's selection carries (-1 for NONE).
	int32_t selected_weapon(const MenuRuntime &menu, const std::string &control) const;

	// A category's ammo controls filled for its selection from the saved pair
	// and the team's type byte, which the fill may reset (player_info_fill_ammo).
	void fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons, const std::string &control,
			int team, const hud::GameTextLookup &text);
	// The grenade positions filled from the saved counts (player_info_fill_grenades).
	void fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons, int class_mask,
			int team_mask, const hud::GameTextLookup &text);

	// The handlers' writes: an AMMO1 / AMMO2 pick stores its row + 1 for the
	// selected weapon [orig: @0x55f730, ctx 0 / 1 -- the interleaved pair]; a
	// type pick stores the row's authored value byte for the team
	// [orig: @0x55f760 / @0x55f7e0]; a grenade pick stores its row, the row
	// being the clip count (retail stores the row VALUE, rounds, which equals
	// clips only for clipsize-1 grenades; the clip count is the consistent half
	// [orig: @0x55fb70]).
	void pick_clips(const MenuRuntime &menu, const std::string &control, bool secondary, int row);
	void pick_ammo_type(const MenuRuntime &menu, const std::string &control, int team, int row);
	void pick_grenade(int position, int row);

	// A category's recorded clip pick for its selected weapon, -1 when untouched
	// (the def default) or for NONE; the team's type byte for a category list.
	int32_t selected_clips(const MenuRuntime &menu, const std::string &control) const;
	int32_t selected_ammo_type(const std::string &control, int team) const;

	// The screen's weight over its selections (player_info_screen_weight).
	double weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons) const;

	// The kit page's selection as the dialog serializes it: each category's
	// selection with its count pair and, for PRIMARY / SECONDARY, the team's
	// type byte as the flags word; then the three grenade slots, the fill's
	// positions and past them the restore's (player_info_kit_entries carries
	// the page order and the witnesses). The lists carry catalog rows, whose
	// row 0 is the "None" WeaponDef_LoadAll seeds, so the NONE row serializes
	// "None" [orig: the NONE insert UIList_AddRow(list, "NONE", 0, 0, 0)
	// @0x56058f in PlayerInfo_PopulateWeaponSlotLists @0x560430, read back
	// through UIList_GetSelectedValue @0x644660 at @0x55e6e8].
	PlayerInfoKitSelection kit_selection(const MenuRuntime &menu, int team, int player_class) const;
};

} // namespace opennova::menu
