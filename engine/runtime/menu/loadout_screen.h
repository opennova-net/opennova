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

// Keep each screen's control-presence/visibility gates and summation order.
// Indices name the selected definitions; the menu supplies the UI state.
double player_info_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades,
		const LoadoutAmmoCounts &primary, const LoadoutAmmoCounts &secondary);
double armory_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades);

} // namespace opennova::menu
